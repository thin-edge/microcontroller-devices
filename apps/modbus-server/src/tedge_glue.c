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
#include "identity.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tedge/tedge.h>

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

static const struct tedge_hooks hooks = {
	.on_state = on_state,
	.restart_request = restart_request,
	.reset = reset,
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_tedge,
	SHELL_CMD(diag, NULL, "client state and TLS heap", cmd_diag),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(tedge, &sub_tedge, "thin-edge.io client", NULL);
#endif /* CONFIG_SHELL */

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
	}
	return rc;
}
