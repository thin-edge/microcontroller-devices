/* SPDX-License-Identifier: Apache-2.0
 *
 * Setting the device up over USB with tedge-onboard: the hooks of
 * tiny-edge's provisioning library for these applications. The Wi-Fi
 * network goes through net.c, which tries it before keeping it; identify
 * flashes the status LED; the scan is the tiny-edge Zephyr port's.
 */

#include "prov_tedge.h"

#include "identity.h"
#include "net.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tedge/provisioning_zephyr.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_prov_tedge, CONFIG_LOG_DEFAULT_LEVEL);

#define JOIN_TIMEOUT K_SECONDS(30)
#define IDENTIFY_MS  10000

static int wifi_connect(const char *ssid, const char *password, void *ctx)
{
	struct app_wifi_creds creds = { 0 };
	int rc;

	ARG_UNUSED(ctx);
	creds.ssid_len = strlen(ssid);
	creds.psk_len = strlen(password);
	if (creds.ssid_len >= sizeof(creds.ssid) ||
	    creds.psk_len >= sizeof(creds.psk)) {
		return -EINVAL;
	}
	memcpy(creds.ssid, ssid, creds.ssid_len);
	memcpy(creds.psk, password, creds.psk_len);
	rc = app_net_switch_network(&creds, JOIN_TIMEOUT);
	memset(&creds, 0, sizeof(creds));
	return rc;
}

static void identify(void *ctx)
{
	ARG_UNUSED(ctx);
	status_led_flash(STATUS_LED_IDENTIFY, IDENTIFY_MS);
}

static struct tedge_provisioning_hooks hooks = {
	.wifi_connect = wifi_connect,
	.identify = identify,
	.wifi_scan = tedge_provisioning_wifi_scan,
	.chip = CONFIG_SOC,
};

int app_prov_tedge_init(void)
{
	int rc;

	hooks.firmware_name = app_identity_firmware_name();
	hooks.firmware_version = app_identity_firmware_version();
	rc = tedge_provisioning_init(&hooks);

	if (rc != 0) {
		LOG_ERR("tedge-onboard support did not start (%d)", rc);
	}
	return rc;
}
