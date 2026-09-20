/* SPDX-License-Identifier: Apache-2.0
 *
 * The glue between this application and the tedge-zephyr client. It lives
 * here, not in the module: the module knows nothing about lib/common.
 *
 * The SNMP agent answers polls and sends traps; the client gives the same
 * device a way to be managed from Cumulocity — onboarded, restarted,
 * updated, and adjusted through its parameter set — without either knowing
 * about the other.
 */

#include "tedge_glue.h"

#include "boot_request.h"
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

/* An SNMP poll is stateless and a manager retries, so a restart costs a
 * missed poll at worst; nothing here needs to veto it. */
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
/* What an operator may change on an SNMP device from the cloud. The
 * client's own "tedge" set is declared beside this one and covers how much
 * it logs, how often it reports its health and whether tunnels are allowed.
 */
#define AGENT_SET "agent"

static const struct tedge_parameter agent_params[] = {
	TEDGE_PARAM_INT("interval_s", CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S,
			5, 3600, "Seconds between measurements"),
	TEDGE_PARAM_BOOL("telemetry", true,
			 "Publish measurements to the cloud"),
};

static int32_t param_interval_s(void)
{
	int32_t seconds;

	if (tedge_parameter_get_int(AGENT_SET, "interval_s", &seconds) == 0) {
		return seconds;
	}
	return CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S;
}

static bool param_telemetry_on(void)
{
	bool on;

	if (tedge_parameter_get_bool(AGENT_SET, "telemetry", &on) == 0) {
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
		.type = "thin-edge.io-zephyr-snmp",
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
	rc = tedge_declare_parameters(AGENT_SET, agent_params,
				      ARRAY_SIZE(agent_params), NULL, NULL);
	if (rc != 0) {
		LOG_WRN("could not declare the '%s' parameters (%d)",
			AGENT_SET, rc);
	}
#endif
	rc = tedge_start();
	if (rc != 0) {
		LOG_ERR("tedge_start failed (%d)", rc);
	}
	return rc;
}

#if defined(CONFIG_TEDGE_TELEMETRY)
/* The agent already knows its uptime and what it has served; sending the
 * same numbers to Cumulocity costs one message per interval and gives an
 * operator something without an SNMP manager. */
static void telemetry_fn(struct k_work *work)
{
	struct tedge_measurement_value values[] = {
		{ .series = "uptime", .unit = "s" },
	};
	int32_t interval = CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S;
	bool on = true;

#if defined(CONFIG_TEDGE_PARAMETERS)
	interval = param_interval_s();
	on = param_telemetry_on();
#endif
	values[0].value = (double)(k_uptime_get() / 1000);
	if (on) {
		(void)tedge_publish_measurement(
			CONFIG_APP_TEDGE_MEASUREMENT_TYPE, values,
			ARRAY_SIZE(values), 0);
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
