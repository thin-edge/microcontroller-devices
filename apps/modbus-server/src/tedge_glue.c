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
#if defined(CONFIG_APP_WIFI_CRED_STORE)
#include "prov_c8y.h"
#endif
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

#if defined(CONFIG_APP_TEDGE_TEST_FAULT_COMMAND)
/* Test aid: crashes the device on purpose, so the crash dump has something
 * to report. */
static int cmd_crash(const struct shell *sh, size_t argc, char **argv)
{
	volatile uint32_t *nowhere = NULL;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "crashing on purpose");
	*nowhere = 1;
	return 0;
}
#endif

/* These hang off the client's own "tedge" root, which tedge-zephyr defines.
 * An application that registered a root of the same name would give the
 * device two of them, and the shell would only ever reach one. */
SHELL_SUBCMD_ADD((tedge), diag, NULL, "client state and TLS heap", cmd_diag,
		 1, 0);
#if defined(CONFIG_APP_TEDGE_TEST_FAULT_COMMAND)
SHELL_SUBCMD_ADD((tedge), crash, NULL, "crash on purpose (test builds only)",
		 cmd_crash, 1, 0);
#endif
#if defined(CONFIG_TEDGE_TELEMETRY)
SHELL_SUBCMD_ADD((tedge), event, NULL, "publish an event: event [text]",
		 cmd_event, 1, 1);
SHELL_SUBCMD_ADD((tedge), alarm, NULL,
		 "raise an alarm: alarm [severity] [text]", cmd_alarm, 1, 2);
SHELL_SUBCMD_ADD((tedge), clear, NULL, "clear an alarm: clear [type]",
		 cmd_clear, 1, 1);
SHELL_SUBCMD_ADD((tedge), flood, NULL, "publish N measurements: flood [N]",
		 cmd_flood, 1, 1);
#endif
#endif /* CONFIG_SHELL */

#if defined(CONFIG_TEDGE_PARAMETERS) && defined(CONFIG_TEDGE_TELEMETRY)
/* ------------------------------------------------------------------------ */
/* What an operator may change from the cloud                                */
/*                                                                           */
/* The declaration is const, so it costs flash and no RAM, and the defaults  */
/* are this application's existing Kconfig values rather than a second set   */
/* written out beside them: whatever the image was built with is what the    */
/* device starts from, and anything an operator sets afterwards replaces it. */
/*                                                                           */
/* Register the schema in the tenant with "tedge params schema".             */
/* ------------------------------------------------------------------------ */

#define PUMP_SET "pump"

static const struct tedge_parameter pump_params[] = {
	TEDGE_PARAM_INT("interval_s", CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S,
			5, 3600, "Seconds between measurements"),
	TEDGE_PARAM_BOOL("telemetry", true,
			 "Publish measurements to the cloud"),
	TEDGE_PARAM_STRING("measurement_type",
			   CONFIG_APP_TEDGE_MEASUREMENT_TYPE, 31,
			   "What the cloud files these measurements under"),
};

static int32_t param_interval_s(void)
{
	int32_t seconds;

	if (tedge_parameter_get_int(PUMP_SET, "interval_s", &seconds) == 0) {
		return seconds;
	}
	return CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S;
}

static bool param_telemetry_on(void)
{
	bool on;

	if (tedge_parameter_get_bool(PUMP_SET, "telemetry", &on) == 0) {
		return on;
	}
	return true;
}

static const char *param_measurement_type(void)
{
	static char type[32];

	if (tedge_parameter_get_string(PUMP_SET, "measurement_type", type,
				       sizeof(type)) == 0 && type[0] != '\0') {
		return type;
	}
	return CONFIG_APP_TEDGE_MEASUREMENT_TYPE;
}

static void telemetry_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_fn);

/* The client has already checked every value against the declaration. What
 * is left is the one rule only this application knows: Cumulocity files a
 * measurement under its type, and a type with a space in it is not one. */
static int params_changed(const char *set, char *reason, size_t reason_len,
			  void *user_data)
{
	const char *type = param_measurement_type();

	ARG_UNUSED(set);
	ARG_UNUSED(user_data);

	for (const char *p = type; *p != '\0'; p++) {
		if (*p == ' ' || *p == '\t') {
			snprintf(reason, reason_len,
				 "measurement_type '%s' cannot contain spaces",
				 type);
			return -EINVAL;
		}
	}
	/* Take effect now, rather than after one more of the old interval. */
	(void)k_work_reschedule(&telemetry_work, K_SECONDS(param_interval_s()));
	LOG_INF("parameters applied: interval=%ds telemetry=%s type=%s",
		param_interval_s(), param_telemetry_on() ? "on" : "off", type);
	return 0;
}
#endif /* CONFIG_TEDGE_PARAMETERS && CONFIG_TEDGE_TELEMETRY */

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
#if defined(CONFIG_TEDGE_PARAMETERS)
	if (count > 0 && param_telemetry_on()) {
		(void)tedge_publish_measurement(param_measurement_type(),
						values, count, 0);
	}
	(void)k_work_reschedule(k_work_delayable_from_work(work),
				K_SECONDS(param_interval_s()));
#else
	if (count > 0) {
		(void)tedge_publish_measurement(CONFIG_APP_TEDGE_MEASUREMENT_TYPE,
						values, count, 0);
	}
	(void)k_work_reschedule(k_work_delayable_from_work(work),
				K_SECONDS(CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S));
#endif
}

#if !defined(CONFIG_TEDGE_PARAMETERS)
static K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_fn);
#endif
#endif

#if defined(CONFIG_TEDGE_LOG_UPLOAD)
/* The client keeps its own log; this is the application's, and shows what
 * the hook is for: a few lines that only this firmware can answer, produced
 * when the cloud asks rather than stored anywhere. */
static int status_log(const struct tedge_log_request *req,
		      tedge_write_fn write, void *ctx, void *user_data)
{
	char line[128];
	size_t count = data_source_count();
	int rc;

	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	rc = snprintf(line, sizeof(line), "device %s, firmware %s %s\n",
		      app_identity_device_id(), app_identity_firmware_name(),
		      app_identity_firmware_version());
	rc = write(ctx, line, (size_t)rc);
	if (rc != 0) {
		return rc;
	}
	for (size_t i = 0; i < count; i++) {
		const struct data_measurement *d = data_source_descriptor(i);
		int n = snprintf(line, sizeof(line), "%s = %d.%02d %s\n",
				 d->name, (int)data_source_sample(i),
				 (int)((data_source_sample(i) -
					(int)data_source_sample(i)) * 100),
				 (d->unit != NULL) ? d->unit : "");

		rc = write(ctx, line, (size_t)n);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}
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
#if defined(CONFIG_APP_WIFI_CRED_STORE)
	/* Tenant and one-time password from the ZTP provisioner, if it left
	 * any. The external ID already came through app_identity_device_id(). */
	(void)prov_c8y_handoff();
#endif
#if defined(CONFIG_TEDGE_PARAMETERS) && defined(CONFIG_TEDGE_TELEMETRY)
	/* Before tedge_start(): the client loads the stored values over the
	 * declared defaults and reports the set as soon as it connects. */
	rc = tedge_declare_parameters(PUMP_SET, pump_params,
				      ARRAY_SIZE(pump_params), params_changed,
				      NULL);
	if (rc != 0) {
		LOG_ERR("could not declare the '%s' parameters (%d)", PUMP_SET,
			rc);
		return rc;
	}
#endif
	rc = tedge_start();
	if (rc != 0) {
		LOG_ERR("tedge_start failed (%d)", rc);
		return rc;
	}
#if defined(CONFIG_TEDGE_TELEMETRY)
	(void)k_work_schedule(&telemetry_work,
#if defined(CONFIG_TEDGE_PARAMETERS)
			      K_SECONDS(param_interval_s()));
#else
			      K_SECONDS(CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S));
#endif
#endif
#if defined(CONFIG_TEDGE_LOG_UPLOAD)
	(void)tedge_register_log_type("app-status", status_log, NULL);
#endif
	return rc;
}
