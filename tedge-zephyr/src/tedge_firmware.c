/* SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update through MCUboot.
 *
 *   515,<device>,<name>,<version>,<url>
 *     -> refuse if that version is already running
 *     -> 501, then download into the secondary slot (own thread)
 *     -> remember the pending version, request a test boot, reset
 *     -> MCUboot swaps; the device is offline for ~40 s (C6) / ~19 s (S3)
 *     -> the new image boots unconfirmed, connects, the application's hook
 *        agrees, and only then is it confirmed: 115 + 503
 *     -> if it never confirms, MCUboot reverts it and the old image reports
 *        502 naming the version that failed
 *
 * The marker in settings is the only state kept across the swap, which is
 * what lets the old image tell "I was reverted" from "I just booted".
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define MARKER_SEPARATOR ','

struct pending {
	char name[48];
	char version[24];
	char url[600];
};

static struct pending job;
static struct flash_img_context flash_ctx;
static int64_t image_total;
static int64_t last_progress_at;
static int last_progress_percent;
static int64_t last_progress_bytes;
static bool job_running;

static struct k_thread dl_thread;
static K_THREAD_STACK_DEFINE(dl_stack, CONFIG_TEDGE_FIRMWARE_STACK_SIZE);

/* Results for the client thread (the MQTT client is not thread-safe). */
K_MSGQ_DEFINE(fw_events, sizeof(struct tedge_fw_event), 2, 4);

static void post(enum tedge_fw_event_type type, const char *fmt, ...)
{
	struct tedge_fw_event ev = { .type = type };
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
	va_end(ap);
	(void)k_msgq_put(&fw_events, &ev, K_NO_WAIT);
}

int tedge_fw_poll_event(struct tedge_fw_event *ev)
{
	return k_msgq_get(&fw_events, ev, K_NO_WAIT);
}

/* ------------------------------------------------------------------------ */
/* The running image                                                         */
/* ------------------------------------------------------------------------ */

int tedge_fw_running_version(char *buf, size_t len)
{
	struct mcuboot_img_header hdr;

	if (boot_read_bank_header(PARTITION_ID(slot0_partition), &hdr,
				  sizeof(hdr)) != 0) {
		return -EIO;
	}
	return (snprintf(buf, len, "%u.%u.%u", hdr.h.v1.sem_ver.major,
			 hdr.h.v1.sem_ver.minor, hdr.h.v1.sem_ver.revision) <
		(int)len)
		       ? 0
		       : -ENOSPC;
}

/* ------------------------------------------------------------------------ */
/* Progress (free-form topics only, QoS 0)                                   */
/* ------------------------------------------------------------------------ */

static void publish_progress(const char *phase, int percent, int64_t bytes,
			     const char *reason)
{
#if defined(CONFIG_TEDGE_FIRMWARE_PROGRESS)
	char json[240];
	size_t n;

	n = snprintf(json, sizeof(json),
		     "{\"name\":\"%s\",\"version\":\"%s\",\"phase\":\"%s\"",
		     job.name, job.version, phase);
	if (n < sizeof(json) && (percent >= 0 || bytes > 0)) {
		if (percent >= 0) {
			n += snprintf(json + n, sizeof(json) - n,
				      ",\"percent\":%d", percent);
		}
		n += snprintf(json + n, sizeof(json) - n, ",\"bytes\":%lld",
			      (long long)bytes);
		if (image_total > 0) {
			n += snprintf(json + n, sizeof(json) - n,
				      ",\"total\":%lld", (long long)image_total);
		}
	}
	if (reason != NULL && n < sizeof(json)) {
		n += snprintf(json + n, sizeof(json) - n, ",\"reason\":\"%s\"",
			      reason);
	}
	if (n < sizeof(json)) {
		(void)snprintf(json + n, sizeof(json) - n, "}");
		/* At most once: progress is a hint, not state, and it must not
		 * queue up behind device management. */
		(void)tedge_c8y_publish_progress("firmware", json);
	}
#else
	ARG_UNUSED(phase);
	ARG_UNUSED(percent);
	ARG_UNUSED(bytes);
	ARG_UNUSED(reason);
#endif
}

/* ------------------------------------------------------------------------ */
/* The marker                                                                */
/* ------------------------------------------------------------------------ */

static char marker[80];

static int marker_cb(const char *key, size_t len, settings_read_cb read_cb,
		     void *cb_arg, void *param)
{
	ARG_UNUSED(key);
	ARG_UNUSED(param);
	if (len > 0 && len < sizeof(marker)) {
		ssize_t n = read_cb(cb_arg, marker, len);

		marker[MAX(n, 0)] = '\0';
	}
	return 0;
}

static void marker_load(void)
{
	(void)settings_load_subtree_direct(TEDGE_KEY_FIRMWARE, marker_cb, NULL);
}

static void marker_save(const char *name, const char *version)
{
	char buf[80];
	int n = snprintf(buf, sizeof(buf), "%s%c%s", name, MARKER_SEPARATOR,
			 version);

	if (n > 0 && n < (int)sizeof(buf)) {
		(void)settings_save_one(TEDGE_KEY_FIRMWARE, buf, n);
	}
}

static void marker_clear(void)
{
	marker[0] = '\0';
	(void)settings_delete(TEDGE_KEY_FIRMWARE);
}

/* ------------------------------------------------------------------------ */
/* The download                                                              */
/* ------------------------------------------------------------------------ */

static int write_sink(const void *data, size_t len, void *user_data)
{
	ARG_UNUSED(user_data);
	int ret = flash_img_buffered_write(&flash_ctx, (const uint8_t *)data,
					   len, false);

	if (ret != 0) {
		LOG_ERR("firmware: writing to the slot failed (%d)", ret);
	}
	return ret;
}

/* The transfer in flight, so the progress callback can see its length. */
static struct tedge_download dl_req;

/* Cumulocity serves binaries chunked, so there is usually no Content-Length
 * and no percentage to report; then progress is measured in bytes instead.
 */
#define PROGRESS_BYTES_STEP (128 * 1024)

static void report_progress(int64_t written, void *user_data)
{
	int percent = -1;

	ARG_UNUSED(user_data);
	image_total = dl_req.total;
	if (image_total > 0) {
		percent = (int)((written * 100) / image_total);
		if (percent <
		    last_progress_percent + CONFIG_TEDGE_FIRMWARE_PROGRESS_PERCENT) {
			return;
		}
	} else if (written < last_progress_bytes + PROGRESS_BYTES_STEP) {
		return;
	}
	if (k_uptime_get() - last_progress_at < 1000) {
		return; /* at most one a second */
	}
	last_progress_percent = (percent >= 0) ? percent : 0;
	last_progress_bytes = written;
	last_progress_at = k_uptime_get();
	publish_progress("downloading", percent, written, NULL);
}

static void download_thread(void *a, void *b, void *c)
{
	struct tedge_download req = {
		.url = job.url,
		.token = tedge_c8y_jwt(),
		.sink = write_sink,
		.progress = report_progress,
		.timeout_ms = 120000,
	};
	int64_t t0 = k_uptime_get();
	int ret;

	dl_req = req;
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	last_progress_percent = 0;
	last_progress_bytes = 0;
	last_progress_at = 0;
	image_total = 0;
	publish_progress("downloading", 0, 0, NULL);

	ret = flash_img_init(&flash_ctx);
	if (ret == 0) {
		ret = tedge_download(&dl_req);
	}
	if (ret == 0) {
		ret = flash_img_buffered_write(&flash_ctx, NULL, 0, true);
	}
	if (ret != 0) {
		publish_progress("failed", -1, 0, "download failed");
		post(TEDGE_FW_FAILED, "download failed (%d)", ret);
		job_running = false;
		return;
	}

	LOG_INF("firmware: %lld B written in %lld s", (long long)dl_req.written,
		(k_uptime_get() - t0) / 1000);
	publish_progress("installing", 100, dl_req.written, NULL);

	/* From here the device is about to go dark for the swap. */
	marker_save(job.name, job.version);
	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret != 0) {
		marker_clear();
		publish_progress("failed", -1, 0, "could not request the update");
		post(TEDGE_FW_FAILED, "could not request the update (%d)", ret);
		job_running = false;
		return;
	}
	post(TEDGE_FW_REBOOTING, "installing %s", job.version);
	k_msleep(1500); /* let the report and the progress leave */
	tedge_platform_reset();
}

/* ------------------------------------------------------------------------ */
/* The operation                                                             */
/* ------------------------------------------------------------------------ */

int tedge_fw_request(const char *line, char *reason, size_t rlen)
{
	char running[24] = "";
	char name[48], version[24], url[600];

	/* 515,<device>,<name>,<version>,<url> */
	if (tedge_sr_field(line, 2, name, sizeof(name)) <= 0 ||
	    tedge_sr_field(line, 3, version, sizeof(version)) <= 0 ||
	    tedge_sr_field(line, 4, url, sizeof(url)) <= 0) {
		snprintf(reason, rlen, "malformed firmware request");
		return -EINVAL;
	}
	if (job_running) {
		LOG_WRN("firmware: refused %s: an update is already running",
			version);
		snprintf(reason, rlen, "an update is already running");
		return -EBUSY;
	}

	/* Re-installing what is already running costs a download and the
	 * downtime of a swap, for no change. */
	(void)tedge_fw_running_version(running, sizeof(running));
	if (running[0] != '\0' && strcmp(running, version) == 0 &&
	    strcmp(name, tedge_identity()->firmware_name) == 0) {
		LOG_INF("firmware: refused %s %s: it is already running", name,
			version);
		snprintf(reason, rlen, "%s %s is already running", name, version);
		return -EALREADY;
	}

	snprintf(job.name, sizeof(job.name), "%s", name);
	snprintf(job.version, sizeof(job.version), "%s", version);
	if (snprintf(job.url, sizeof(job.url), "%s", url) >= (int)sizeof(job.url)) {
		snprintf(reason, rlen, "the download URL is too long");
		return -ENOSPC;
	}

	LOG_INF("firmware: installing %s %s", job.name, job.version);
	job_running = true;
	k_thread_create(&dl_thread, dl_stack, K_THREAD_STACK_SIZEOF(dl_stack),
			download_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(CONFIG_TEDGE_THREAD_PRIORITY), 0,
			K_NO_WAIT);
	k_thread_name_set(&dl_thread, "tedge_fw");
	return 0;
}

/* How long the device will be offline while the bootloader swaps, for the
 * operator to see in the operation. */
const char *tedge_fw_downtime_hint(void)
{
	return "the device will be offline for up to a minute while it installs";
}

/* Called from the client thread once the session is up. Confirms a test
 * boot, or reports that a previous image was reverted. */
void tedge_fw_on_connected(void)
{
	const struct tedge_hooks *hooks = tedge_hook_table();
	char version[24] = "";
	bool confirmed = boot_is_img_confirmed();
	char *sep;

	if (marker[0] == '\0') {
		marker_load();
	}
	if (marker[0] == '\0') {
		return; /* nothing pending */
	}
	sep = strchr(marker, MARKER_SEPARATOR);
	if (sep != NULL) {
		snprintf(job.name, sizeof(job.name), "%.*s",
			 (int)(sep - marker), marker);
		snprintf(job.version, sizeof(job.version), "%s", sep + 1);
	}
	(void)tedge_fw_running_version(version, sizeof(version));

	if (confirmed && strcmp(version, job.version) != 0) {
		/* We are running a confirmed image that is not the one that was
		 * installed: MCUboot reverted it. */
		LOG_WRN("firmware: %s did not come up; running %s", job.version,
			version);
		post(TEDGE_FW_REVERTED,
		     "%s was rolled back; the device is running %s", job.version,
		     version);
		publish_progress("failed", -1, 0, "rolled back");
		marker_clear();
		return;
	}
	if (confirmed) {
		marker_clear(); /* already confirmed, nothing to do */
		return;
	}

	if (hooks != NULL && hooks->firmware_confirm_check != NULL) {
		int rc = hooks->firmware_confirm_check(hooks->user_data);

		if (rc != 0) {
			LOG_WRN("firmware: the application refused %s (%d); it "
				"will be rolled back on the next reset",
				job.version, rc);
			post(TEDGE_FW_FAILED,
			     "the application refused %s; it will be rolled back",
			     job.version);
			publish_progress("failed", -1, 0,
					 "the application refused it");
			return; /* leave it unconfirmed on purpose */
		}
	}
	if (IS_ENABLED(CONFIG_TEDGE_FIRMWARE_CONFIRM_AFTER_CONNECT)) {
		int rc = boot_write_img_confirmed();

		if (rc != 0) {
			LOG_ERR("firmware: could not confirm the image (%d)", rc);
			return;
		}
		LOG_INF("firmware: %s confirmed", job.version);
		post(TEDGE_FW_INSTALLED, "%s", job.version);
		publish_progress("done", 100, 0, NULL);
		marker_clear();
	}
}
