/* SPDX-License-Identifier: Apache-2.0
 *
 * The crash that outlives the device's memory.
 *
 * The RAM log ring is gone after a fault; a dump written to flash is not. On
 * the next boot the client notices a stored dump, offers it as the log type
 * "coredump", and sends it in the same `#CD:` hex-line format Zephyr's own
 * console backend produces — so an uploaded file goes straight into
 * zephyr/scripts/coredump/coredump_serial_log_parser.py and then gdb, with
 * no tooling to write here.
 *
 * The dump is erased only once the cloud has it, so a failed upload can be
 * asked for again.
 */

#include "tedge_internal.h"

#include <zephyr/debug/coredump.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define BEGIN_LINE "#CD:BEGIN#\n"
#define END_LINE   "#CD:END#\n"
#define PREFIX     "#CD:"
/* 32 bytes a line: 64 hex characters plus the prefix, comfortably inside the
 * line lengths the parser and a log viewer handle. */
#define PER_LINE   32
/* "#CD:" + two characters a byte + a newline and its terminator. */
#define LINE_MAX   (4 + PER_LINE * 2 + 2)

static bool have_dump;

/* The backend answers this one with its return value, not through the
 * argument, which is easy to get wrong and silently reports an empty dump. */
static int stored_size(size_t *size)
{
	int ret = coredump_query(COREDUMP_QUERY_GET_STORED_DUMP_SIZE, NULL);

	if (ret < 0) {
		return ret;
	}
	*size = (size_t)ret;
	return 0;
}

/* Writes the stored dump as the text the parser reads. The two passes an
 * upload makes agree exactly, because the dump in flash does not move. */
static int dump_reader(const struct tedge_log_request *req,
		       tedge_write_fn write, void *ctx, void *user_data)
{
	uint8_t raw[PER_LINE];
	char line[LINE_MAX];
	size_t size = 0;
	size_t off = 0;
	int ret;

	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (stored_size(&size) != 0 || size == 0) {
		return -ENODATA;
	}
	ret = write(ctx, BEGIN_LINE, strlen(BEGIN_LINE));
	if (ret != 0) {
		return ret;
	}
	while (off < size) {
		struct coredump_cmd_copy_arg arg = {
			.offset = (off_t)off,
			.buffer = raw,
			.length = MIN(sizeof(raw), size - off),
		};
		size_t n;

		if (coredump_cmd(COREDUMP_CMD_COPY_STORED_DUMP, &arg) < 0) {
			return -EIO;
		}
		n = snprintf(line, sizeof(line), PREFIX);
		for (size_t i = 0; i < arg.length; i++) {
			n += snprintf(line + n, sizeof(line) - n, "%02x",
				      raw[i]);
		}
		n += snprintf(line + n, sizeof(line) - n, "\n");
		ret = write(ctx, line, n);
		if (ret != 0) {
			return ret;
		}
		off += arg.length;
	}
	return write(ctx, END_LINE, strlen(END_LINE));
}

void tedge_coredump_init(void)
{
	size_t size = 0;
	int stored = coredump_query(COREDUMP_QUERY_HAS_STORED_DUMP, NULL);

	if (stored != 1) {
		return;
	}
	(void)stored_size(&size);
	have_dump = true;
	LOG_WRN("a crash dump from before the last reset is stored (%zu B); "
		"ask for the \"coredump\" log to read it",
		size);
	(void)tedge_register_log_type("coredump", dump_reader, NULL);
}

void tedge_coredump_taken(void)
{
	if (!have_dump) {
		return;
	}
	if (coredump_cmd(COREDUMP_CMD_ERASE_STORED_DUMP, NULL) == 0) {
		have_dump = false;
		LOG_INF("the crash dump reached the cloud and was erased");
	}
}
