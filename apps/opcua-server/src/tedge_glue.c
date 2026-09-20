/* SPDX-License-Identifier: Apache-2.0
 *
 * The glue between this application and the tedge-zephyr client. It lives
 * here, not in the module: the module knows nothing about lib/common.
 *
 * The OPC-UA server serves its address space to whoever browses it; the
 * client gives the same device a way to be managed from Cumulocity —
 * onboarded, restarted, updated, and adjusted through its parameter set —
 * without either knowing about the other.
 *
 * Note that the WROOM-32 boards this application usually runs on cannot
 * carry much of the client beside it; see DEVICES.md.
 */

#include "tedge_glue.h"

#include "boot_request.h"
#include "data_source.h"
#include "identity.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tedge/tedge.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(tedge_glue, CONFIG_LOG_DEFAULT_LEVEL);

static void on_state(enum tedge_state state, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (state) {
	case TEDGE_STATE_AWAITING_REGISTRATION: {
		char url[220];

		/* The operator registers the device from this URL. It carries
		 * the one-time password, so the module never logs it itself. */
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

/* An OPC-UA client reconnects and re-subscribes after a restart, so a
 * restart costs a gap in the data at worst; nothing here needs to veto it. */
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

static const struct tedge_hooks hooks = {
	.on_state = on_state,
	.restart_request = restart_request,
	.reset = reset,
};

#if defined(CONFIG_TEDGE_PARAMETERS) && defined(CONFIG_TEDGE_TELEMETRY)
/* What an operator may change on an OPC-UA device from the cloud. The
 * client's own "tedge" set is declared beside this one and covers how much
 * it logs, how often it reports its health and whether tunnels are allowed.
 */
#define SERVER_SET "server"

static const struct tedge_parameter server_params[] = {
	TEDGE_PARAM_INT("interval_s", CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S,
			5, 3600, "Seconds between measurements"),
	TEDGE_PARAM_BOOL("telemetry", true,
			 "Publish measurements to the cloud"),
};

static int32_t param_interval_s(void)
{
	int32_t seconds;

	if (tedge_parameter_get_int(SERVER_SET, "interval_s", &seconds) == 0) {
		return seconds;
	}
	return CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S;
}

static bool param_telemetry_on(void)
{
	bool on;

	if (tedge_parameter_get_bool(SERVER_SET, "telemetry", &on) == 0) {
		return on;
	}
	return true;
}
#endif /* CONFIG_TEDGE_PARAMETERS && CONFIG_TEDGE_TELEMETRY */

int tedge_glue_start(void)
{
	struct tedge_identity id = {
		.external_id = app_identity_device_id(),
		.name = app_identity_device_id(),
		.type = "thin-edge.io-zephyr-opcua",
		.firmware_name = app_identity_firmware_name(),
		.firmware_version = app_identity_firmware_version(),
	};
	int rc = tedge_init(&id, &hooks);

	if (rc != 0) {
		LOG_ERR("tedge_init failed (%d)", rc);
		return rc;
	}
#if defined(CONFIG_TEDGE_PARAMETERS) && defined(CONFIG_TEDGE_TELEMETRY)
	/* Before tedge_start(): the client loads the stored values over the
	 * declared defaults and reports the set as soon as it connects. */
	rc = tedge_declare_parameters(SERVER_SET, server_params,
				      ARRAY_SIZE(server_params), NULL, NULL);
	if (rc != 0) {
		LOG_WRN("could not declare the '%s' parameters (%d)",
			SERVER_SET, rc);
	}
#endif
	rc = tedge_start();
	if (rc != 0) {
		LOG_ERR("tedge_start failed (%d)", rc);
	}
	return rc;
}

#if defined(CONFIG_TEDGE_TELEMETRY)
/* The same values the OPC-UA address space exposes, sent to Cumulocity once
 * per interval so an operator without an OPC-UA client can still see them. */
static void telemetry_fn(struct k_work *work)
{
	struct tedge_measurement_value values[8];
	size_t count = MIN(data_source_count(), ARRAY_SIZE(values));
	int32_t interval = CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S;
	bool on = true;

#if defined(CONFIG_TEDGE_PARAMETERS)
	interval = param_interval_s();
	on = param_telemetry_on();
#endif
	for (size_t i = 0; i < count; i++) {
		const struct data_measurement *d = data_source_descriptor(i);

		values[i].series = d->name;
		values[i].unit = d->unit;
		values[i].value = data_source_sample(i);
	}
	if (on && count > 0) {
		(void)tedge_publish_measurement(
			CONFIG_APP_TEDGE_MEASUREMENT_TYPE, values, count, 0);
	}
	(void)k_work_reschedule(k_work_delayable_from_work(work),
				K_SECONDS(interval));
}

static K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_fn);

void tedge_glue_start_telemetry(void)
{
	(void)k_work_schedule(&telemetry_work,
			      K_SECONDS(CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S));
}
#endif /* CONFIG_TEDGE_TELEMETRY */
