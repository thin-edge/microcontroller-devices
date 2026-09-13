/* SPDX-License-Identifier: Apache-2.0 */

#include "net.h"
#include "display.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>

LOG_MODULE_REGISTER(app_net, CONFIG_LOG_DEFAULT_LEVEL);

#if defined(CONFIG_NET_HOSTNAME_ENABLE)
#include <zephyr/net/hostname.h>
#endif

#if defined(CONFIG_DNS_SD)
#include <zephyr/net/dns_sd.h>
#include <string.h>
/* Advertise the OPC-UA server as a DNS-SD service (_opcua-tcp._tcp) so it can be
 * discovered on the LAN without a known IP. The instance name lives in a
 * mutable buffer that is filled at runtime with the unique per-device hostname
 * (CONFIG_NET_HOSTNAME_UNIQUE appends the MAC), so multiple devices on the same
 * network advertise distinct instances and do not clash. The mDNS responder
 * separately answers for <unique-hostname>.local. */
static char opcua_sd_instance[64] = CONFIG_NET_HOSTNAME;
DNS_SD_REGISTER_TCP_SERVICE(opcua_dns_sd, opcua_sd_instance, "_opcua-tcp",
			    "local", DNS_SD_EMPTY_TXT, CONFIG_APP_OPCUA_PORT);
#endif

/* Copy the current (unique) hostname into the DNS-SD instance buffer. */
static void update_identity(void)
{
#if defined(CONFIG_DNS_SD) && defined(CONFIG_NET_HOSTNAME_ENABLE)
	const char *h = net_hostname_get();

	strncpy(opcua_sd_instance, h, sizeof(opcua_sd_instance) - 1);
	opcua_sd_instance[sizeof(opcua_sd_instance) - 1] = '\0';
#endif
}

const char *app_net_hostname(void)
{
#if defined(CONFIG_NET_HOSTNAME_ENABLE)
	return net_hostname_get();
#else
	return CONFIG_APP_DEVICE_NAME;
#endif
}

/* Signalled once L4 connectivity is available. */
static K_SEM_DEFINE(net_connected_sem, 0, 1);
static atomic_t connected;
static uint32_t current_ipv4; /* network byte order */

bool app_net_is_connected(void)
{
	return atomic_get(&connected) != 0;
}

uint32_t app_net_ipv4(void)
{
	return current_ipv4;
}

static uint32_t read_iface_ipv4(void)
{
	struct net_if *iface = net_if_get_default();
	struct net_in_addr *a;

	if (iface == NULL) {
		return 0;
	}
	a = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	return a ? a->s_addr : 0;
}

int app_net_wait_connected(k_timeout_t timeout)
{
	if (app_net_is_connected()) {
		return 0;
	}
	return k_sem_take(&net_connected_sem, timeout) == 0 ? 0 : -ETIMEDOUT;
}

static void mark_connected(bool up)
{
	atomic_set(&connected, up ? 1 : 0);
	if (up) {
		update_identity(); /* MAC (and unique hostname) are set by now */
		current_ipv4 = read_iface_ipv4();
		display_status_ipv4(DISPLAY_STAGE_CONNECTED, current_ipv4);
		k_sem_give(&net_connected_sem);
	} else {
		display_status_stage(DISPLAY_STAGE_WIFI_CONNECTING);
	}
}

#if defined(CONFIG_NET_NATIVE_OFFLOADED_SOCKETS)

/* native_sim with host-offloaded sockets: the host provides connectivity, so
 * there is no association/DHCP step. Report connected immediately.
 */
int app_net_init(void)
{
	LOG_INF("Using host networking (native_sim offloaded sockets)");
	mark_connected(true);
	return 0;
}

#elif defined(CONFIG_APP_WIFI)

#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

#define L4_EVENTS   (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback wifi_cb;
static struct k_work_delayable reconnect_work;

static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params params = {0};

	if (iface == NULL) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	params.ssid = (const uint8_t *)CONFIG_APP_WIFI_SSID;
	params.ssid_length = strlen(CONFIG_APP_WIFI_SSID);
	params.psk = (const uint8_t *)CONFIG_APP_WIFI_PSK;
	params.psk_length = strlen(CONFIG_APP_WIFI_PSK);
	params.security = params.psk_length ? WIFI_SECURITY_TYPE_PSK
					    : WIFI_SECURITY_TYPE_NONE;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.mfp = WIFI_MFP_OPTIONAL;

	if (params.ssid_length == 0) {
		LOG_ERR("Wi-Fi SSID is empty — set APP_WIFI_SSID via a local "
			"overlay (see overlay-wifi-credentials.conf.example)");
		return -EINVAL;
	}

	LOG_INF("Connecting to Wi-Fi SSID \"%s\"...", CONFIG_APP_WIFI_SSID);
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}

static void reconnect_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!app_net_is_connected()) {
		(void)wifi_connect();
	}
}

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_INF("Network connectivity established");
		mark_connected(true);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_WRN("Network connectivity lost — scheduling reconnect");
		mark_connected(false);
		k_work_reschedule(&reconnect_work, K_SECONDS(2));
		break;
	default:
		break;
	}
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		LOG_WRN("Wi-Fi disconnected (reason %d) — will reconnect",
			status->disconn_reason);
		/* DIAG: show the disconnect reason code on a blue background. */
		display_status_code(DISPLAY_STAGE_WIFI_CONNECTING,
				    (uint32_t)status->disconn_reason);
		k_work_reschedule(&reconnect_work, K_SECONDS(2));
	} else if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		if (status->status) {
			LOG_WRN("Wi-Fi association failed (%d) — retrying",
				status->status);
			/* DIAG: show the connect failure code on orange. */
			display_status_code(DISPLAY_STAGE_ERROR,
					    (uint32_t)status->status);
			k_work_reschedule(&reconnect_work, K_SECONDS(2));
		} else {
			LOG_INF("Wi-Fi associated; awaiting IP address");
		}
	}
}

int app_net_init(void)
{
	k_work_init_delayable(&reconnect_work, reconnect_handler);

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENTS);
	net_mgmt_add_event_callback(&l4_cb);
	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, WIFI_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	return wifi_connect();
}

#else /* hardware with a wired/other interface and no explicit Wi-Fi */

static struct net_mgmt_event_callback l4_cb;

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("Network connectivity established");
		mark_connected(true);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		LOG_WRN("Network connectivity lost");
		mark_connected(false);
	}
}

int app_net_init(void)
{
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler,
				     NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&l4_cb);
	LOG_INF("Waiting for network connectivity...");
	return 0;
}

#endif
