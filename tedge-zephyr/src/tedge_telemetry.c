/* SPDX-License-Identifier: Apache-2.0
 *
 * Telemetry: the measurements, events and alarms an application asks the
 * client to send.
 *
 * A call builds the message there and then — that is when the values and the
 * time are true — and puts it in a bounded buffer. The client thread drains
 * the buffer while it is connected, so an application never waits for the
 * network and the MQTT client stays owned by one thread.
 *
 * The buffer is small and in RAM. When it fills, the oldest measurement is
 * dropped and counted, so a device that cannot keep up says so instead of
 * growing its memory or losing data quietly. Events and alarms are never
 * dropped for a measurement: "the pump stopped" matters more than the
 * pressure reading beside it.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/clock.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

/* One message in the buffer: a header, then the topic suffix and payload. */
struct queued {
	uint16_t len;   /* of this record, including the header */
	uint8_t kind;   /* enum tedge_msg_kind */
	char text[];    /* "<type>\0<payload>\0" */
};

#define RING_BYTES CONFIG_TEDGE_TELEMETRY_BUFFER_BYTES

static uint8_t ring[RING_BYTES];
static size_t ring_used;
static uint32_t dropped;
static K_MUTEX_DEFINE(ring_lock);

/* ------------------------------------------------------------------------ */
/* The buffer                                                                */
/* ------------------------------------------------------------------------ */

static struct queued *first(void)
{
	return (ring_used > 0) ? (struct queued *)ring : NULL;
}

static void pop_first(void)
{
	struct queued *q = first();

	if (q == NULL) {
		return;
	}
	ring_used -= q->len;
	memmove(ring, ring + q->len, ring_used);
}

/* Drop the oldest measurement to make room; returns false when there is
 * nothing left to drop (the buffer holds only events and alarms). */
static bool drop_oldest_measurement(void)
{
	size_t off = 0;

	while (off < ring_used) {
		struct queued *q = (struct queued *)(ring + off);

		if (q->kind == TEDGE_MSG_MEASUREMENT) {
			size_t len = q->len;

			memmove(ring + off, ring + off + len,
				ring_used - off - len);
			ring_used -= len;
			dropped++;
			return true;
		}
		off += q->len;
	}
	return false;
}

static int enqueue(enum tedge_msg_kind kind, const char *type,
		   const char *payload)
{
	size_t tlen = strlen(type) + 1;
	size_t plen = strlen(payload) + 1;
	size_t need = sizeof(struct queued) + tlen + plen;
	int rc = 0;

	if (need > RING_BYTES) {
		return -EMSGSIZE;
	}
	k_mutex_lock(&ring_lock, K_FOREVER);
	while (ring_used + need > RING_BYTES) {
		if (!drop_oldest_measurement()) {
			/* Only events and alarms left, and still no room: the
			 * new message is the one that has to go. */
			rc = -ENOMEM;
			goto out;
		}
	}
	{
		struct queued *q = (struct queued *)(ring + ring_used);

		q->len = (uint16_t)need;
		q->kind = (uint8_t)kind;
		memcpy(q->text, type, tlen);
		memcpy(q->text + tlen, payload, plen);
		ring_used += need;
	}
out:
	k_mutex_unlock(&ring_lock);
	if (rc == 0) {
		tedge_telemetry_wake();
	} else {
		LOG_WRN("telemetry: no room for a %s message",
			(kind == TEDGE_MSG_MEASUREMENT) ? "measurement"
							: "event or alarm");
	}
	return rc;
}

/* Called from the client thread while connected. */
void tedge_telemetry_flush(void)
{
	for (;;) {
		char type[40];
		char payload[224];
		enum tedge_msg_kind kind;
		struct queued *q;

		k_mutex_lock(&ring_lock, K_FOREVER);
		q = first();
		if (q == NULL) {
			k_mutex_unlock(&ring_lock);
			return;
		}
		kind = (enum tedge_msg_kind)q->kind;
		snprintf(type, sizeof(type), "%s", q->text);
		snprintf(payload, sizeof(payload), "%s",
			 q->text + strlen(q->text) + 1);
		k_mutex_unlock(&ring_lock);

		if (tedge_c8y_publish_telemetry(kind, type, payload) != 0) {
			return; /* still offline: keep it for the next round */
		}
		k_mutex_lock(&ring_lock, K_FOREVER);
		pop_first();
		k_mutex_unlock(&ring_lock);
	}
}

uint32_t tedge_telemetry_dropped(void)
{
	return dropped;
}

/* ------------------------------------------------------------------------ */
/* Building messages                                                         */
/* ------------------------------------------------------------------------ */

/* "time":"2026-09-20T10:15:00Z", or nothing at all when the clock has not
 * been set yet: a message without a time is better than no message, and the
 * cloud stamps it on arrival. */
static size_t put_time(char *buf, size_t len, int64_t timestamp_ms)
{
	struct timespec now;
	struct tm tm;
	char iso[24];
	time_t secs;

	if (timestamp_ms > 0) {
		secs = (time_t)(timestamp_ms / 1000);
	} else {
		if (!tedge_time_is_valid()) {
			return 0;
		}
		(void)sys_clock_gettime(SYS_CLOCK_REALTIME, &now);
		secs = now.tv_sec;
	}
	gmtime_r(&secs, &tm);
	strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return snprintf(buf, len, "\"time\":\"%s\",", iso);
}

/* Formats a double with two decimals; Zephyr's printf has no floats by
 * default and telemetry should not force them on an application. */
static size_t put_number(char *buf, size_t len, double v)
{
	long scaled = (long)(v * 100.0 + (v >= 0 ? 0.5 : -0.5));
	long whole = scaled / 100;
	long frac = scaled % 100;

	return snprintf(buf, len, "%s%ld.%02ld",
			(scaled < 0 && whole == 0) ? "-" : "", whole,
			frac < 0 ? -frac : frac);
}

int tedge_publish_measurement(const char *type,
			      const struct tedge_measurement_value *values,
			      size_t count, int64_t timestamp_ms)
{
	char json[224];
	size_t n = 0;

	if (!IS_ENABLED(CONFIG_TEDGE_TELEMETRY)) {
		return -ENOTSUP;
	}
	if (type == NULL || values == NULL || count == 0) {
		return -EINVAL;
	}
	n += snprintf(json + n, sizeof(json) - n, "{");
	n += put_time(json + n, sizeof(json) - n, timestamp_ms);
	for (size_t i = 0; i < count && n < sizeof(json); i++) {
		n += snprintf(json + n, sizeof(json) - n, "%s\"%s\":",
			      i ? "," : "", values[i].series);
		n += put_number(json + n, sizeof(json) - n, values[i].value);
	}
	if (n >= sizeof(json) - 2) {
		return -EMSGSIZE;
	}
	snprintf(json + n, sizeof(json) - n, "}");
	return enqueue(TEDGE_MSG_MEASUREMENT, type, json);
}

int tedge_publish_event(const char *type, const char *text,
			int64_t timestamp_ms)
{
	char json[224];
	char escaped[160];
	size_t n = 0;

	if (!IS_ENABLED(CONFIG_TEDGE_TELEMETRY)) {
		return -ENOTSUP;
	}
	if (type == NULL || text == NULL) {
		return -EINVAL;
	}
	tedge_json_escape(text, escaped, sizeof(escaped));
	n += snprintf(json + n, sizeof(json) - n, "{");
	n += put_time(json + n, sizeof(json) - n, timestamp_ms);
	snprintf(json + n, sizeof(json) - n, "\"text\":\"%s\"}", escaped);
	return enqueue(TEDGE_MSG_EVENT, type, json);
}

static const char *severity_name(enum tedge_alarm_severity s)
{
	switch (s) {
	case TEDGE_ALARM_CRITICAL:
		return "critical";
	case TEDGE_ALARM_MAJOR:
		return "major";
	case TEDGE_ALARM_MINOR:
		return "minor";
	default:
		return "warning";
	}
}

int tedge_raise_alarm(const char *type, enum tedge_alarm_severity severity,
		      const char *text)
{
	char json[224];
	char escaped[140];
	size_t n = 0;

	if (!IS_ENABLED(CONFIG_TEDGE_TELEMETRY)) {
		return -ENOTSUP;
	}
	if (type == NULL || text == NULL) {
		return -EINVAL;
	}
	tedge_json_escape(text, escaped, sizeof(escaped));
	n += snprintf(json + n, sizeof(json) - n, "{");
	n += put_time(json + n, sizeof(json) - n, 0);
	snprintf(json + n, sizeof(json) - n,
		 "\"severity\":\"%s\",\"text\":\"%s\"}", severity_name(severity),
		 escaped);
	return enqueue(TEDGE_MSG_ALARM, type, json);
}

int tedge_clear_alarm(const char *type)
{
	if (!IS_ENABLED(CONFIG_TEDGE_TELEMETRY)) {
		return -ENOTSUP;
	}
	if (type == NULL) {
		return -EINVAL;
	}
	/* thin-edge.io clears an alarm with an empty payload. */
	return enqueue(TEDGE_MSG_ALARM_CLEAR, type, "");
}
