/* SPDX-License-Identifier: Apache-2.0
 *
 * The client's own log, kept in a ring of RAM so that the last minutes
 * before a problem can be fetched from the cloud afterwards.
 *
 * This is a Zephyr log backend: it sees every message the image logs, not
 * only the client's, which is what makes it useful on a device whose
 * application never built a log of its own. It is deliberately small and
 * deliberately volatile — a reboot loses it, which is what the crash dump
 * exists for.
 *
 * When the ring is full the oldest whole line is dropped, so what remains is
 * always readable from its first character: half a line at the top of a log
 * file wastes the reader's time.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>

#include <string.h>

#define RING_BYTES CONFIG_TEDGE_LOG_RING_BYTES

static uint8_t ring[RING_BYTES];
static size_t used;
static uint32_t dropped_lines;
static struct k_spinlock lock;

/* ------------------------------------------------------------------------ */
/* The ring                                                                  */
/* ------------------------------------------------------------------------ */

/* Drop whole lines until @p need bytes are free. */
static void make_room(size_t need)
{
	while (used + need > RING_BYTES && used > 0) {
		uint8_t *nl = memchr(ring, '\n', used);
		size_t line = (nl != NULL) ? (size_t)(nl - ring) + 1 : used;

		memmove(ring, ring + line, used - line);
		used -= line;
		dropped_lines++;
	}
}

/* Not static: the unit test writes through this, where a real image has the
 * logging subsystem do it. */
void tedge_log_ring_write(const uint8_t *data, size_t len)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (len > RING_BYTES) {
		/* One enormous line: keep its tail, which is where the
		 * interesting part of a message usually is. */
		data += len - RING_BYTES;
		len = RING_BYTES;
	}
	make_room(len);
	memcpy(ring + used, data, len);
	used += len;
	k_spin_unlock(&lock, key);
}

size_t tedge_log_ring_read(size_t offset, uint8_t *out, size_t len)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	size_t n = 0;

	if (offset < used) {
		n = MIN(len, used - offset);
		memcpy(out, ring + offset, n);
	}
	k_spin_unlock(&lock, key);
	return n;
}

size_t tedge_log_ring_size(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	size_t n = used;

	k_spin_unlock(&lock, key);
	return n;
}

uint32_t tedge_log_ring_dropped(void)
{
	return dropped_lines;
}

/* ------------------------------------------------------------------------ */
/* The Zephyr backend                                                        */
/* ------------------------------------------------------------------------ */

static int out_fn(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	tedge_log_ring_write(data, length);
	return (int)length;
}

/* One line at a time; the formatter needs no more than this. */
static uint8_t out_buf[64];
LOG_OUTPUT_DEFINE(log_output_tedge, out_fn, out_buf, sizeof(out_buf));

static void backend_process(const struct log_backend *const backend,
			    union log_msg_generic *msg)
{
	uint32_t flags = LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_TIMESTAMP |
			 LOG_OUTPUT_FLAG_FORMAT_TIMESTAMP;

	ARG_UNUSED(backend);
	log_output_msg_process(&log_output_tedge, &msg->log, flags);
}

static void backend_init(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
}

static int backend_format_set(const struct log_backend *const backend,
			      uint32_t log_type)
{
	ARG_UNUSED(backend);
	ARG_UNUSED(log_type);
	return -ENOTSUP; /* the text format is the only one worth keeping */
}

static void backend_panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	/* Nothing to flush: the ring is RAM, and whatever caused the panic
	 * is the crash dump's job, not this one's. */
}

static const struct log_backend_api backend_api = {
	.process = backend_process,
	.init = backend_init,
	.format_set = backend_format_set,
	.panic = backend_panic,
};

LOG_BACKEND_DEFINE(tedge_log_ring, backend_api, true);
