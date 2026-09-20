/* SPDX-License-Identifier: Apache-2.0
 *
 * Answering "send me your log".
 *
 * A log type is a callback: the client asks the application (or itself) to
 * write the log through a sink when the cloud requests it. Nothing is stored,
 * nothing needs a filesystem, and no log is ever held whole in RAM.
 *
 * Cumulocity wants a Content-Length, and this device cannot hold a log twice,
 * so a request runs the reader twice: once through a counting sink to learn
 * the length, then again straight onto the socket. A reader that produces a
 * little less the second time is padded and one that produces more is cut, so
 * the promise in the header is always kept.
 *
 * The work runs on its own thread. Producing a log can be slow, and the
 * client thread has a connection to serve.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/clock.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define TYPE_SLOTS  CONFIG_TEDGE_LOG_TYPE_SLOTS
#define MAX_BYTES   CONFIG_TEDGE_LOG_UPLOAD_MAX_BYTES
#define TRUNC_NOTE  "\n--- truncated: this device sends at most " \
		    STRINGIFY(CONFIG_TEDGE_LOG_UPLOAD_MAX_BYTES) " bytes ---\n"

static struct {
	const char *type;
	tedge_log_reader_t reader;
	void *user_data;
} types[TYPE_SLOTS];

static K_THREAD_STACK_DEFINE(up_stack, CONFIG_TEDGE_LOG_UPLOAD_STACK_SIZE);
static struct k_thread up_thread;
static bool job_running;

static struct {
	char type[32];
	struct tedge_log_request req;
	char search[48];
	size_t limit;    /* bytes the reader may produce before the notice */
	bool truncated;  /* set by the measuring pass, kept for the sending one */
} job;

K_MSGQ_DEFINE(log_events, sizeof(struct tedge_log_event), 2, 4);

int tedge_log_poll_event(struct tedge_log_event *ev)
{
	return k_msgq_get(&log_events, ev, K_NO_WAIT);
}

/* ------------------------------------------------------------------------ */
/* The registry                                                              */
/* ------------------------------------------------------------------------ */

int tedge_register_log_type(const char *type, tedge_log_reader_t reader,
			    void *user_data)
{
	if (type == NULL || reader == NULL) {
		return -EINVAL;
	}
	for (int i = 0; i < TYPE_SLOTS; i++) {
		if (types[i].type == NULL ||
		    strcmp(types[i].type, type) == 0) {
			types[i].type = type;
			types[i].reader = reader;
			types[i].user_data = user_data;
			return 0;
		}
	}
	LOG_WRN("log: no free slot for the type \"%s\" "
		"(CONFIG_TEDGE_LOG_TYPE_SLOTS)", type);
	return -ENOSPC;
}

/* "118,<type>,<type>…": the log types this image can actually produce. */
size_t tedge_log_types_line(char *out, size_t len)
{
	size_t n = snprintf(out, len, "118");

	for (int i = 0; i < TYPE_SLOTS && n < len; i++) {
		if (types[i].type != NULL) {
			n += snprintf(out + n, len - n, ",%s", types[i].type);
		}
	}
	return (n > 3) ? n : 0;
}

/* ------------------------------------------------------------------------ */
/* Producing the log                                                         */
/* ------------------------------------------------------------------------ */

struct counter {
	size_t n;
	size_t cap;
	bool hit_cap;
};

static int count_sink(const void *data, size_t len, void *ctx)
{
	struct counter *c = ctx;

	ARG_UNUSED(data);
	if (c->n + len > c->cap) {
		c->hit_cap = true;
		return -ENOSPC; /* the reader passes this back and stops */
	}
	c->n += len;
	return 0;
}

/* A reader writes through tedge_write_fn (ctx first); the upload wants a
 * tedge_sink_fn (ctx last). This adapts one to the other and applies the
 * cap, so the two passes agree on where the log stops. */
struct capped {
	tedge_sink_fn sink;
	void *ctx;
	size_t left;
};

static int capped_write(void *ctx, const void *data, size_t len)
{
	struct capped *c = ctx;

	if (len > c->left) {
		len = c->left;
	}
	if (len == 0) {
		return -ENOSPC;
	}
	c->left -= len;
	return c->sink(data, len, c->ctx);
}

static tedge_log_reader_t reader_for(const char *type, void **user_data)
{
	for (int i = 0; i < TYPE_SLOTS; i++) {
		if (types[i].type != NULL && strcmp(types[i].type, type) == 0) {
			*user_data = types[i].user_data;
			return types[i].reader;
		}
	}
	return NULL;
}

static int produce(tedge_sink_fn sink, void *ctx, void *user_data)
{
	struct capped c = { .sink = sink, .ctx = ctx, .left = job.limit };
	tedge_log_reader_t reader;
	void *reader_data = NULL;

	ARG_UNUSED(user_data);
	reader = reader_for(job.type, &reader_data);
	if (reader == NULL) {
		return -ENOENT;
	}
	(void)reader(&job.req, capped_write, &c, reader_data);
	if (job.truncated) {
		return sink(TRUNC_NOTE, strlen(TRUNC_NOTE), ctx);
	}
	return 0;
}

static size_t measure(void)
{
	struct counter c = { .cap = job.limit };

	(void)produce(count_sink, &c, NULL);
	job.truncated = c.hit_cap;
	if (job.truncated) {
		/* produce() adds the notice in the sending pass too, so it
		 * belongs in the length. */
		return c.n + strlen(TRUNC_NOTE);
	}
	return c.n;
}

/* ------------------------------------------------------------------------ */
/* Talking to Cumulocity                                                     */
/* ------------------------------------------------------------------------ */

/* The device knows its external ID; the event needs its managed-object id.
 * One small GET per session answers that. */
static char mo_id[24];

struct grab {
	char *buf;
	size_t len;
	size_t used;
};

static int grab_sink(const void *data, size_t len, void *ctx)
{
	struct grab *g = ctx;
	size_t n = MIN(len, g->len - 1 - g->used);

	memcpy(g->buf + g->used, data, n);
	g->used += n;
	g->buf[g->used] = '\0';
	return 0;
}

static int resolve_mo_id(void)
{
	char body[320];
	char url[256];
	struct grab g = { .buf = body, .len = sizeof(body) };
	struct tedge_download req = { .url = url,
				      .token = tedge_c8y_jwt(),
				      .sink = grab_sink,
				      .user_data = &g,
				      .timeout_ms = 15000 };
	int ret;

	if (mo_id[0] != '\0') {
		return 0;
	}
	snprintf(url, sizeof(url),
		 "https://%s/identity/externalIds/c8y_Serial/%s",
		 tedge_c8y_host(), tedge_identity()->external_id);
	ret = tedge_download(&req);
	if (ret != 0) {
		return ret;
	}
	/* {"self":…,"externalId":…,"type":…,"managedObject":{"self":…,"id":"…"}}
	 * The first "id": in that answer is the managed object's; "externalId"
	 * does not match the pattern, which looks for the quote before it. */
	if (tedge_json_field(body, "id", mo_id, sizeof(mo_id)) <= 0) {
		LOG_ERR("log: no device id in the identity answer");
		return -ENOENT;
	}
	return 0;
}

static void iso_now(char *out, size_t len)
{
	struct timespec now;
	struct tm tm;

	(void)sys_clock_gettime(SYS_CLOCK_REALTIME, &now);
	gmtime_r(&now.tv_sec, &tm);
	strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Creates the event the binary hangs off, and returns its URL. */
static int create_event(char *url, size_t len)
{
	char payload[224];
	char when[24];
	struct tedge_upload up = { .url = url,
				   .token = tedge_c8y_jwt(),
				   .content_type = "application/json",
				   .payload = payload,
				   .location = url,
				   .location_len = len,
				   .timeout_ms = 20000 };
	char endpoint[160];

	iso_now(when, sizeof(when));
	snprintf(payload, sizeof(payload),
		 "{\"source\":{\"id\":\"%s\"},\"type\":\"c8y_LogfileRequest\","
		 "\"text\":\"%s\",\"time\":\"%s\"}",
		 mo_id, job.type, when);
	snprintf(endpoint, sizeof(endpoint), "https://%s/event/events",
		 tedge_c8y_host());
	up.url = endpoint;
	return tedge_upload(&up);
}

static void post(int rc, const char *url, const char *reason)
{
	struct tedge_log_event ev = { .rc = rc };

	if (url != NULL) {
		snprintf(ev.url, sizeof(ev.url), "%s", url);
	}
	if (reason != NULL) {
		snprintf(ev.reason, sizeof(ev.reason), "%s", reason);
	}
	(void)k_msgq_put(&log_events, &ev, K_NO_WAIT);
}

static void upload_thread(void *a, void *b, void *c)
{
	char event_url[192];
	char binaries[224];
	char filename[112];
	size_t length;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	ret = resolve_mo_id();
	if (ret != 0) {
		post(ret, NULL, "the device could not resolve its own id");
		goto done;
	}
	length = measure();
	if (length == 0) {
		post(-ENODATA, NULL, "the log is empty");
		goto done;
	}
	ret = create_event(event_url, sizeof(event_url));
	if (ret != 0 || event_url[0] == '\0') {
		post((ret != 0) ? ret : -EIO, NULL,
		     "the device could not create the log's event");
		goto done;
	}
	snprintf(binaries, sizeof(binaries), "%s/binaries", event_url);
	snprintf(filename, sizeof(filename), "%s-%s.log", job.type,
		 tedge_identity()->external_id);
	{
		struct tedge_upload up = { .url = binaries,
					   .token = tedge_c8y_jwt(),
					   .content_type = "text/plain",
					   .filename = filename,
					   .producer = produce,
					   .length = length,
					   .timeout_ms = 60000 };

		ret = tedge_upload(&up);
	}
	if (ret != 0) {
		post(ret, NULL, "the log could not be uploaded");
		goto done;
	}
	LOG_INF("log: sent %s (%zu B)", job.type, length);
#if defined(CONFIG_TEDGE_COREDUMP)
	if (strcmp(job.type, "coredump") == 0) {
		tedge_coredump_taken();
	}
#endif
	post(0, binaries, NULL);
done:
	job_running = false;
}

/* ------------------------------------------------------------------------ */
/* The request                                                               */
/* ------------------------------------------------------------------------ */

int tedge_log_request(const char *line, char *reason, size_t rlen)
{
	char type[32], field[24];
	void *unused = NULL;

	/* 522,<device>,<type>,<from>,<to>,<search>,<lines> */
	if (tedge_sr_field(line, 2, type, sizeof(type)) <= 0) {
		snprintf(reason, rlen, "malformed log request");
		return -EINVAL;
	}
	if (job_running) {
		snprintf(reason, rlen, "a log is already being sent");
		return -EBUSY;
	}
	if (reader_for(type, &unused) == NULL) {
		snprintf(reason, rlen, "this device has no log called \"%s\"",
			 type);
		return -ENOENT;
	}

	memset(&job, 0, sizeof(job));
	snprintf(job.type, sizeof(job.type), "%s", type);
	(void)tedge_sr_field(line, 5, job.search, sizeof(job.search));
	if (job.search[0] != '\0') {
		job.req.search_text = job.search;
	}
	if (tedge_sr_field(line, 6, field, sizeof(field)) > 0) {
		job.req.max_lines = (uint32_t)strtoul(field, NULL, 10);
	}
	/* The dates are passed through for readers that keep wall-clock
	 * timestamps; the client's own ring does not, and ignores them. */
	job.limit = MAX_BYTES - strlen(TRUNC_NOTE);

	job_running = true;
	k_thread_create(&up_thread, up_stack, K_THREAD_STACK_SIZEOF(up_stack),
			upload_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(CONFIG_TEDGE_THREAD_PRIORITY), 0,
			K_NO_WAIT);
	k_thread_name_set(&up_thread, "tedge_log");
	return 0;
}

/* ------------------------------------------------------------------------ */
/* The client's own log                                                      */
/* ------------------------------------------------------------------------ */

/* Reads the ring a line at a time, applying the filters it can: a search
 * text and a line limit. Date filters need wall-clock timestamps, which the
 * ring does not keep, so they are ignored rather than half-honoured. */
static int ring_reader(const struct tedge_log_request *req,
		       tedge_write_fn write, void *ctx, void *user_data)
{
	char line[160];
	size_t offset = 0;
	size_t total = tedge_log_ring_size();
	uint32_t sent = 0;

	ARG_UNUSED(user_data);
	while (offset < total) {
		size_t n = tedge_log_ring_read(offset, (uint8_t *)line,
					       sizeof(line) - 1);
		char *nl;
		size_t len;
		int ret;

		if (n == 0) {
			break;
		}
		line[n] = '\0';
		nl = memchr(line, '\n', n);
		len = (nl != NULL) ? (size_t)(nl - line) + 1 : n;
		offset += len;

		if (req->search_text != NULL) {
			char save = line[len];

			line[len] = '\0';
			if (strstr(line, req->search_text) == NULL) {
				line[len] = save;
				continue;
			}
			line[len] = save;
		}
		ret = write(ctx, line, len);
		if (ret != 0) {
			return ret; /* the cap, or the connection */
		}
		if (req->max_lines != 0 && ++sent >= req->max_lines) {
			break;
		}
	}
	return 0;
}

void tedge_log_upload_init(void)
{
	(void)tedge_register_log_type("tedge-log", ring_reader, NULL);
#if defined(CONFIG_TEDGE_COREDUMP)
	tedge_coredump_init();
#endif
}
