/* SPDX-License-Identifier: Apache-2.0 */

#include "prov_handoff.h"
#include "boot_request.h"
#include "net.h"
#include "prov_identity.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/wifi_credentials.h>
#include <string.h>

LOG_MODULE_REGISTER(app_prov, CONFIG_LOG_DEFAULT_LEVEL);

#define ACK_MS 1000 /* LED acknowledgement before a reboot */

static FUNC_NORETURN void reboot_now(void)
{
	log_flush(); /* deferred logging: print before the reset */
	boot_request_reboot();
	CODE_UNREACHABLE;
}

/* Set the boot request; false when there is no provisioner to hand over to. */
static bool request_provisioner(enum boot_request_reason reason)
{
	if (!boot_request_provisioner_present()) {
		LOG_ERR("No Wi-Fi provisioner is flashed (prov partition is "
			"empty); flash it with scripts/flash.sh");
		return false;
	}
	int rc = boot_request_set(BOOT_REQUEST_PROVISIONER, reason);

	if (rc != 0) {
		LOG_ERR("Could not set the boot request (%d)", rc);
		return false;
	}
	return true;
}

void app_prov_handoff_no_credentials(void)
{
	LOG_INF("No Wi-Fi credentials: rebooting into the Wi-Fi provisioner");
	if (request_provisioner(BOOT_REQUEST_NO_CREDENTIALS)) {
		reboot_now();
	}
}

void app_prov_identity_update(void)
{
	struct prov_identity id;

	memset(&id, 0, sizeof(id));
	strncpy(id.hostname, app_net_hostname(), sizeof(id.hostname) - 1);
	strncpy(id.service, CONFIG_APP_DNSSD_SERVICE_TYPE, sizeof(id.service) - 1);
	id.port = CONFIG_APP_DNSSD_PORT;
	int rc = prov_identity_store(&id);

	if (rc != 0) {
		LOG_WRN("Could not store the identity record (%d)", rc);
	}
}

static void ack_and_reboot(void)
{
	status_led_flash(STATUS_LED_IDENTIFY, ACK_MS);
	k_msleep(ACK_MS);
	reboot_now();
}

void app_prov_handle_gesture(enum gesture g)
{
	switch (g) {
	case GESTURE_PROVISION:
		LOG_INF("Button pattern: rebooting into the Wi-Fi provisioner");
		if (request_provisioner(BOOT_REQUEST_OPERATOR)) {
			ack_and_reboot();
		}
		break;
	case GESTURE_ERASE_ARMED:
		LOG_WRN("Erase armed: release the button to erase Wi-Fi credentials");
		status_led_flash(STATUS_LED_ERASE_ARMED, 120000);
		break;
	case GESTURE_ERASE: {
		struct app_wifi_creds c;
		bool have;

		LOG_WRN("Erasing stored Wi-Fi credentials");
		(void)wifi_credentials_delete_all();
		/* Compile-time credentials may still resolve: then the window's
		 * end returns to them rather than waiting. */
		have = app_wifi_creds_resolve(&c) == 0;
		memset(&c, 0, sizeof(c));
		if (request_provisioner(have ? BOOT_REQUEST_OPERATOR
					     : BOOT_REQUEST_NO_CREDENTIALS)) {
			ack_and_reboot();
		}
		reboot_now(); /* nothing to hand over to: restart without them */
	}
	default:
		break;
	}
}
