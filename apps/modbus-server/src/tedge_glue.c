/* SPDX-License-Identifier: Apache-2.0
 *
 * The glue between this application and the tedge-zephyr client. It lives
 * here, not in the module: the module knows nothing about lib/common.
 *
 * It gives the client this device's identity, shows the registration URL
 * while the device waits to be registered, drives the status LED from the
 * client's state, and hands it the platform reset this repository already
 * uses (a CPU reset hangs MCUboot on the ESP32-C6).
 */

#include "tedge_glue.h"

#include "boot_request.h"
#include "data_source.h"
#include "identity.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tedge/tedge.h>

#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <mbedtls/memory_buffer_alloc.h>
#endif

LOG_MODULE_REGISTER(tedge_glue, CONFIG_LOG_DEFAULT_LEVEL);

static void on_state(enum tedge_state state, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (state) {
	case TEDGE_STATE_AWAITING_REGISTRATION: {
		char url[220];

		/* The operator registers the device from this URL. It carries
		 * the one-time password, so the module never logs it itself;
		 * on a device with a display or a provisioner, show it there
		 * instead of on the console. */
		if (tedge_registration_url(url, sizeof(url)) > 0) {
			LOG_INF("register this device: %s", url);
		}
		status_led_set_mode(STATUS_LED_PROVISIONING);
		break;
	}
	case TEDGE_STATE_CONNECTED:
		status_led_set_mode(STATUS_LED_CONNECTED);
		break;
	default:
		break;
	}
}

/* The Modbus server keeps running across a restart, and clients reconnect,
 * so nothing here needs to veto it. */
static int restart_request(char *reason, size_t reason_len, void *user_data)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(reason_len);
	ARG_UNUSED(user_data);
	LOG_INF("restart requested from the cloud");
	return 0;
}

static void reset(void *user_data)
{
	ARG_UNUSED(user_data);
	boot_request_reboot();
}

#if defined(CONFIG_APP_TEDGE_TEST_REFUSE_FIRMWARE)
/* Test build only: pretend the application is unhealthy after an update, so
 * the image is never confirmed and the bootloader rolls it back. */
static int firmware_confirm_check(void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_WRN("test build: refusing to confirm this firmware");
	return -EINVAL;
}
#endif

static const struct tedge_hooks hooks = {
	.on_state = on_state,
	.restart_request = restart_request,
	.reset = reset,
#if defined(CONFIG_APP_TEDGE_TEST_REFUSE_FIRMWARE)
	.firmware_confirm_check = firmware_confirm_check,
#endif
};

#if defined(CONFIG_SHELL)
/* Test aid (task 8.3): the client's state and the TLS heap in use, so a
 * reconnect cycle can be checked for leaks. */
static int cmd_diag(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {
		"stopped",   "waiting-network", "waiting-time",
		"awaiting-registration", "connecting", "connected", "updating",
	};
	enum tedge_state state = tedge_get_state();
	size_t cur = 0, cur_blocks = 0, peak = 0, peak_blocks = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	mbedtls_memory_buffer_alloc_cur_get(&cur, &cur_blocks);
	mbedtls_memory_buffer_alloc_max_get(&peak, &peak_blocks);
	shell_print(sh, "tedge state=%s tls_heap cur=%zu (%zu blocks) peak=%zu",
		    ((size_t)state < ARRAY_SIZE(names)) ? names[state] : "?", cur,
		    cur_blocks, peak);
	return 0;
}

#if defined(CONFIG_TEDGE_TELEMETRY)
/* Test aids: an application normally decides for itself when something is
 * worth an event or an alarm, but a console is the quickest way to see one
 * arrive in the cloud. */
static int cmd_event(const struct shell *sh, size_t argc, char **argv)
{
	int rc = tedge_publish_event("app_test",
				     (argc > 1) ? argv[1] : "test event", 0);

	shell_print(sh, "event: %d", rc);
	return rc;
}

static int cmd_alarm(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {"critical", "major", "minor",
					    "warning"};
	enum tedge_alarm_severity severity = TEDGE_ALARM_WARNING;
	int rc;

	if (argc > 1) {
		for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
			if (strcmp(argv[1], names[i]) == 0) {
				severity = (enum tedge_alarm_severity)i;
			}
		}
	}
	rc = tedge_raise_alarm("app_test_alarm", severity,
			       (argc > 2) ? argv[2] : "test alarm");
	shell_print(sh, "alarm: %d", rc);
	return rc;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	int rc = tedge_clear_alarm((argc > 1) ? argv[1] : "app_test_alarm");

	ARG_UNUSED(argv);
	shell_print(sh, "clear: %d", rc);
	return rc;
}

/* Publish faster than the link can carry, to see what a full buffer does. */
static int cmd_flood(const struct shell *sh, size_t argc, char **argv)
{
	struct tedge_measurement_value value = {.series = "flood", .value = 0};
	long count = (argc > 1) ? strtol(argv[1], NULL, 10) : 50;
	int last = 0;

	for (long i = 0; i < count; i++) {
		value.value = (double)i;
		last = tedge_publish_measurement("flood", &value, 1, 0);
	}
	shell_print(sh, "flood: %ld published, last=%d, dropped=%u", count,
		    last, tedge_telemetry_dropped());
	return 0;
}
#endif /* CONFIG_TEDGE_TELEMETRY */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_tedge,
	SHELL_CMD(diag, NULL, "client state and TLS heap", cmd_diag),
#if defined(CONFIG_TEDGE_TELEMETRY)
	SHELL_CMD_ARG(event, NULL, "publish an event: event [text]", cmd_event,
		      1, 1),
	SHELL_CMD_ARG(alarm, NULL, "raise an alarm: alarm [severity] [text]",
		      cmd_alarm, 1, 2),
	SHELL_CMD_ARG(clear, NULL, "clear an alarm: clear [type]", cmd_clear,
		      1, 1),
	SHELL_CMD_ARG(flood, NULL, "publish N measurements: flood [N]",
		      cmd_flood, 1, 1),
#endif
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(tedge, &sub_tedge, "thin-edge.io client", NULL);
#endif /* CONFIG_SHELL */

#if defined(CONFIG_TEDGE_TELEMETRY)
/* The application decides what to send and when; the client only carries
 * it. Here that is the pump simulation this firmware already serves over
 * Modbus, so the same values reach an operator who has no Modbus client.
 */
static void telemetry_fn(struct k_work *work)
{
	struct tedge_measurement_value values[8];
	size_t count = MIN(data_source_count(), ARRAY_SIZE(values));

	for (size_t i = 0; i < count; i++) {
		const struct data_measurement *d = data_source_descriptor(i);

		values[i].series = d->name;
		values[i].unit = d->unit;
		values[i].value = data_source_sample(i);
	}
	if (count > 0) {
		(void)tedge_publish_measurement(CONFIG_APP_TEDGE_MEASUREMENT_TYPE,
						values, count, 0);
	}
	(void)k_work_reschedule(k_work_delayable_from_work(work),
				K_SECONDS(CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S));
}

static K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_fn);
#endif

int tedge_glue_start(void)
{
	struct tedge_identity id = {
		.external_id = app_identity_device_id(),
		.name = app_identity_device_id(),
		.type = "thin-edge.io-zephyr-modbus",
		.firmware_name = app_identity_firmware_name(),
		.firmware_version = app_identity_firmware_version(),
	};
	int rc = tedge_init(&id, &hooks);

	if (rc != 0) {
		LOG_ERR("tedge_init failed (%d)", rc);
		return rc;
	}
	rc = tedge_start();
	if (rc != 0) {
		LOG_ERR("tedge_start failed (%d)", rc);
		return rc;
	}
#if defined(CONFIG_TEDGE_TELEMETRY)
	(void)k_work_schedule(&telemetry_work,
			      K_SECONDS(CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S));
#endif
	return rc;
}
