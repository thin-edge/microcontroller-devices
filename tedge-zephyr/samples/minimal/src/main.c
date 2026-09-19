/* SPDX-License-Identifier: Apache-2.0
 *
 * The smallest application that hosts the thin-edge.io client: join a Wi-Fi
 * network, then start the client and let it do the rest. Everything here is
 * the *application's* job; the client only reacts to the network being up.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#include <tedge/tedge.h>

#include <string.h>

LOG_MODULE_REGISTER(tedge_minimal, LOG_LEVEL_INF);

static void on_state(enum tedge_state state, void *user_data)
{
	ARG_UNUSED(user_data);

	if (state == TEDGE_STATE_AWAITING_REGISTRATION) {
		char url[220];

		/* The URL carries a one-time password: show it to the operator
		 * (here the console; a real device might use a display or its
		 * provisioner). */
		if (tedge_registration_url(url, sizeof(url)) > 0) {
			LOG_INF("register this device: %s", url);
		}
	}
}

static const struct tedge_hooks hooks = {
	.on_state = on_state,
};

#if defined(CONFIG_WIFI)
static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)CONFIG_TEDGE_SAMPLE_WIFI_SSID,
		.ssid_length = strlen(CONFIG_TEDGE_SAMPLE_WIFI_SSID),
		.psk = (const uint8_t *)CONFIG_TEDGE_SAMPLE_WIFI_PSK,
		.psk_length = strlen(CONFIG_TEDGE_SAMPLE_WIFI_PSK),
		.security = strlen(CONFIG_TEDGE_SAMPLE_WIFI_PSK)
				    ? WIFI_SECURITY_TYPE_PSK
				    : WIFI_SECURITY_TYPE_NONE,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	if (iface == NULL) {
		LOG_ERR("no Wi-Fi interface");
		return -ENODEV;
	}
	LOG_INF("joining \"%s\"", CONFIG_TEDGE_SAMPLE_WIFI_SSID);
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
}
#endif

int main(void)
{
	LOG_INF("tedge-zephyr %s", tedge_version());

#if defined(CONFIG_WIFI)
	/* The client waits for an address by itself, so a failure here only
	 * delays it: the application can retry however it likes. */
	(void)wifi_connect();
#endif

	if (tedge_init(NULL, &hooks) == 0) {
		(void)tedge_start();
	}
	return 0;
}
