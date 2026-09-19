/* SPDX-License-Identifier: Apache-2.0 */

#include "net.h"
#include "display.h"
#include "liveness.h"
#include "status_led.h"
#if defined(CONFIG_APP_PROV_HANDOFF)
#include "prov_handoff.h"
#endif
#if defined(CONFIG_APP_WIFI_CRED_STORE)
#include <zephyr/net/wifi_credentials.h>
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_WIFI)
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#endif

#if defined(CONFIG_WIFI_ESP32) && defined(CONFIG_APP_WIFI)
#include <esp_wifi.h>
#endif

#if defined(CONFIG_NET_IPV4) && !defined(CONFIG_NET_SOCKETS_OFFLOAD)
#include <zephyr/net/icmp.h>
#endif

LOG_MODULE_REGISTER(app_net, CONFIG_LOG_DEFAULT_LEVEL);

#if defined(CONFIG_NET_HOSTNAME_ENABLE)
#include <zephyr/net/hostname.h>
#endif

#if defined(CONFIG_DNS_SD)
#include <zephyr/net/dns_sd.h>
#include <string.h>
/* Advertise the frontend as a DNS-SD service so it can be discovered on the LAN
 * without a known IP. The service type, port and transport are per-firmware
 * (CONFIG_APP_DNSSD_SERVICE_TYPE / CONFIG_APP_DNSSD_PORT / CONFIG_APP_DNSSD_UDP;
 * defaults reproduce the OPC-UA firmware's _opcua-tcp / 4840). UDP-based
 * frontends (SNMP) register _<type>._udp so the advertised transport is correct.
 * The instance name lives in a mutable buffer filled at runtime with the unique
 * per-device hostname (CONFIG_NET_HOSTNAME_UNIQUE appends the MAC) so devices
 * don't clash. The mDNS responder separately answers for <unique-hostname>.local. */
static char dnssd_instance[64] = CONFIG_NET_HOSTNAME;
/* One TXT entry ("txtvers=1"), length-prefixed as DNS-SD requires. Zephyr's
 * DNS_SD_EMPTY_TXT is sized `sizeof(text) - 1` by the registration macro, so it
 * goes out as a zero-length TXT record; RFC 6763 6.1 requires at least one
 * byte, and macOS then ignores the service. */
static const char dnssd_txt[] = "\x09txtvers=1";
#if defined(CONFIG_APP_DNSSD_UDP)
DNS_SD_REGISTER_UDP_SERVICE(app_dns_sd, dnssd_instance,
			    CONFIG_APP_DNSSD_SERVICE_TYPE, "local",
			    dnssd_txt, CONFIG_APP_DNSSD_PORT);
#else
DNS_SD_REGISTER_TCP_SERVICE(app_dns_sd, dnssd_instance,
			    CONFIG_APP_DNSSD_SERVICE_TYPE, "local",
			    dnssd_txt, CONFIG_APP_DNSSD_PORT);
#endif
#endif

/* Copy the current (unique) hostname into the DNS-SD instance buffer. */
static void update_identity(void)
{
#if defined(CONFIG_DNS_SD) && defined(CONFIG_NET_HOSTNAME_ENABLE)
	const char *h = net_hostname_get();

	strncpy(dnssd_instance, h, sizeof(dnssd_instance) - 1);
	dnssd_instance[sizeof(dnssd_instance) - 1] = '\0';
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

/* --- Diagnostics: gather network details and render them on the display --- */

static enum display_stage g_stage = DISPLAY_STAGE_BOOT;
static int g_last_reason = -1; /* last Wi-Fi disconnect/connect reason code */
static uint32_t status_ticks;  /* status_work cycles executed (diagnostic) */
static char lb[10][22];
static struct k_work_delayable status_work;

/* Connectivity work (the status tick and reconnects) runs on its own queue,
 * not the system workqueue. It renders the status (a Wi-Fi status query plus
 * formatting), sends the gateway ping (the whole IPv4/ARP/driver transmit path
 * runs on the caller's stack) and issues blocking Wi-Fi management requests:
 * that measured ~1.1 KB of stack, more than the ESP32 system workqueue's 1 KB,
 * and the overflow silently corrupted memory until the firmware stalled. A
 * dedicated queue sizes the stack for it and keeps a blocked driver call from
 * freezing every other system-workqueue user. */
#if defined(CONFIG_APP_WIFI)
static K_THREAD_STACK_DEFINE(net_wq_stack, CONFIG_APP_NET_WORKQ_STACK_SIZE);
static struct k_work_q net_wq;

static void net_wq_start(void)
{
	const struct k_work_queue_config cfg = { .name = "net_wq" };

	k_work_queue_init(&net_wq);
	k_work_queue_start(&net_wq, net_wq_stack,
			   K_THREAD_STACK_SIZEOF(net_wq_stack),
			   CONFIG_APP_NET_WORKQ_PRIORITY, &cfg);
}

static void net_wq_reschedule(struct k_work_delayable *work, k_timeout_t delay)
{
	(void)k_work_reschedule_for_queue(&net_wq, work, delay);
}
#else
/* No connectivity state machine: nothing is ever scheduled. */
static void net_wq_reschedule(struct k_work_delayable *work, k_timeout_t delay)
{
	(void)k_work_reschedule(work, delay);
}
#endif

static void fmt_ip(char *dst, size_t n, uint32_t be)
{
	const uint8_t *o = (const uint8_t *)&be;

	snprintf(dst, n, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
}

/* --- Gateway ping: does the device's Wi-Fi data path actually pass traffic? --- */
#if defined(CONFIG_NET_IPV4) && !defined(CONFIG_NET_SOCKETS_OFFLOAD)
static struct net_icmp_ctx ping_ctx;
static bool ping_ready;
static atomic_t ping_ok;   /* echo replies received */
static uint32_t ping_sent; /* echo requests sent */
static int ping_init_rc = -999; /* last net_icmp_init_ctx() result */
static int ping_send_rc = -999; /* last echo-request send result */
/* How far ping_gateway_once() got: 0=never called, 1=no iface, 2=gw==0,
 * 3=init done, 4=send attempted. */
static int ping_stage;

static enum net_verdict ping_reply(struct net_icmp_ctx *ctx, struct net_pkt *pkt,
				   struct net_icmp_ip_hdr *ip_hdr,
				   struct net_icmp_hdr *icmp_hdr, void *user_data)
{
	ARG_UNUSED(ctx); ARG_UNUSED(pkt); ARG_UNUSED(ip_hdr);
	ARG_UNUSED(icmp_hdr); ARG_UNUSED(user_data);
	atomic_inc(&ping_ok);
	return NET_OK;
}

/* Send one echo request to the gateway (non-blocking). Replies are counted
 * asynchronously by ping_reply(). */
static void ping_gateway_once(void)
{
	struct net_if *iface = net_if_get_default();

	ping_stage = 1;
	if (iface == NULL) {
		return;
	}

	struct net_in_addr target;

	ping_stage = 2;
	if (net_addr_pton(NET_AF_INET, CONFIG_APP_PING_TARGET, &target) != 0) {
		return; /* bad target address */
	}
	ping_stage = 3;
	if (!ping_ready) {
		ping_init_rc = net_icmp_init_ctx(&ping_ctx, NET_AF_INET,
						 NET_ICMPV4_ECHO_REPLY, 0,
						 ping_reply);
		if (ping_init_rc != 0) {
			return;
		}
		ping_ready = true;
	}

	struct net_sockaddr_in dst = {
		.sin_family = NET_AF_INET, .sin_addr = target,
	};
	struct net_icmp_ping_params p = {
		.identifier = 1, .sequence = (uint16_t)(ping_sent + 1),
	};

	ping_stage = 4;
	ping_send_rc = net_icmp_send_echo_request_no_wait(
		&ping_ctx, iface, (struct net_sockaddr *)&dst, &p, NULL);
	if (ping_send_rc == 0) {
		ping_sent++;
	}
}
#endif /* CONFIG_NET_IPV4 && !CONFIG_NET_SOCKETS_OFFLOAD */

/* Build the diagnostic lines (IP/GW/mask/Wi-Fi RSSI/state/reason/host) and draw
 * them. Called on connectivity changes and periodically so RSSI stays fresh and
 * silent Wi-Fi drops become visible. */
static void render_status(enum display_stage stage)
{
	struct net_if *iface = net_if_get_default();
	const char *lines[13];
	int n = 0;
	char tmp[16];

	g_stage = stage;

	fmt_ip(tmp, sizeof(tmp), current_ipv4);
	snprintf(lb[0], sizeof(lb[0]), "IP %s", tmp);
	lines[n++] = lb[0];

	/* Gateway/netmask come from the native IP stack (hardware); native_sim
	 * offloaded sockets (NSOS) has no such notion. */
#if !defined(CONFIG_NET_SOCKETS_OFFLOAD)
	if (iface != NULL) {
		struct net_in_addr gw = net_if_ipv4_get_gw(iface);
		struct net_in_addr self = { .s_addr = current_ipv4 };
		struct net_in_addr mask =
			net_if_ipv4_get_netmask_by_addr(iface, &self);

		fmt_ip(tmp, sizeof(tmp), gw.s_addr);
		snprintf(lb[1], sizeof(lb[1]), "GW %s", tmp);
		lines[n++] = lb[1];
		fmt_ip(tmp, sizeof(tmp), mask.s_addr);
		snprintf(lb[2], sizeof(lb[2]), "MASK %s", tmp);
		lines[n++] = lb[2];
	}
#else
	ARG_UNUSED(iface);
#endif

/* Only the display needs these, and the query blocks inside the Espressif
 * driver while an access point disappears or returns — long enough to trip the
 * liveness watchdog on the connectivity queue (2026-09-17 AP-restart soak). */
#if defined(CONFIG_WIFI) && defined(CONFIG_APP_DISPLAY_STATUS)
	struct net_if *wifi = net_if_get_first_wifi();
	struct wifi_iface_status st = {0};

	app_step(APP_CTX_NETWQ, "wifi-status");
	if (wifi != NULL &&
	    net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, wifi, &st, sizeof(st)) == 0) {
		snprintf(lb[3], sizeof(lb[3]), "RSSI %d CH%u", st.rssi,
			 st.channel);
		lines[n++] = lb[3];
		snprintf(lb[4], sizeof(lb[4]), "WST%d %.10s", st.state, st.ssid);
		lines[n++] = lb[4];
		/* BSSID of the AP we associated to — identifies the exact node. */
		snprintf(lb[7], sizeof(lb[7]),
			 "AP %02X%02X%02X%02X%02X%02X",
			 (uint8_t)st.bssid[0], (uint8_t)st.bssid[1],
			 (uint8_t)st.bssid[2], (uint8_t)st.bssid[3],
			 (uint8_t)st.bssid[4], (uint8_t)st.bssid[5]);
		lines[n++] = lb[7];
	}
#endif

	if (g_last_reason >= 0) {
		snprintf(lb[5], sizeof(lb[5]), "LAST RSN %d", g_last_reason);
		lines[n++] = lb[5];
	}
#if defined(CONFIG_NET_IPV4) && !defined(CONFIG_NET_SOCKETS_OFFLOAD)
	snprintf(lb[8], sizeof(lb[8]), "GWPING %d/%u t%u",
		 (int)atomic_get(&ping_ok), ping_sent, status_ticks);
	lines[n++] = lb[8];
	snprintf(lb[9], sizeof(lb[9]), "P st%d i%d s%d", ping_stage,
		 ping_init_rc, ping_send_rc);
	lines[n++] = lb[9];
#endif
	n += (int)app_liveness_boot_lines(&lines[n], ARRAY_SIZE(lines) - 1 - n);
	snprintf(lb[6], sizeof(lb[6]), "%s", app_net_hostname());
	lines[n++] = lb[6];

	display_status_lines(stage, lines, n);
}

void app_net_show_status(enum display_stage stage)
{
	render_status(stage);
}

/* Event-independent connectivity watchdog (real logic in the Wi-Fi branch,
 * no-op elsewhere). Runs each status_work tick so recovery does not depend on a
 * management event being delivered. */
static void connectivity_watchdog(void);

static void status_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	/* The connectivity queue got round to this tick: that is its progress. */
	app_step(APP_CTX_NETWQ, "tick");
	app_alive(APP_CTX_NETWQ);
	status_ticks++;
	app_step(APP_CTX_NETWQ, "watchdog");
	connectivity_watchdog();
#if defined(CONFIG_NET_IPV4) && !defined(CONFIG_NET_SOCKETS_OFFLOAD)
	app_step(APP_CTX_NETWQ, "ping");
	ping_gateway_once(); /* one echo request per cycle to the gateway */
#endif
	app_step(APP_CTX_NETWQ, "render");
	render_status(g_stage);
	app_step(APP_CTX_NETWQ, "idle");
	net_wq_reschedule(&status_work, K_SECONDS(3));
}

/* The provisioner image: the device is waiting for (or testing) credentials,
 * so being offline is expected. Recovery is suspended and the LED shows the
 * provisioning pattern instead of the connectivity state. */
static const bool prov_mode = IS_ENABLED(CONFIG_APP_WIFI_PROVISIONER);

#if defined(CONFIG_APP_WIFI_PROVISIONER)
/* Outcome of app_net_try_credentials(), signalled from the event handlers. */
static K_SEM_DEFINE(try_sem, 0, 1);
static int try_result;
static bool try_active;

static void try_done(int result)
{
	if (try_active) {
		try_active = false;
		try_result = result;
		k_sem_give(&try_sem);
	}
}
#else
static inline void try_done(int result)
{
	ARG_UNUSED(result);
}
#endif

static void mark_connected(bool up)
{
	atomic_set(&connected, up ? 1 : 0);
	if (!prov_mode) {
		status_led_set_connected(up);
	}
	if (up) {
		update_identity(); /* MAC (and unique hostname) are set by now */
		current_ipv4 = read_iface_ipv4();
		render_status(DISPLAY_STAGE_CONNECTED);
		k_sem_give(&net_connected_sem);
		try_done(0);
#if defined(CONFIG_APP_PROV_HANDOFF)
		app_prov_identity_update();
#endif
	} else {
		render_status(DISPLAY_STAGE_WIFI_CONNECTING);
	}
}

#if defined(CONFIG_APP_WIFI_CRED_STORE)
static void stored_ssid_cb(void *cb_arg, const char *ssid, size_t ssid_len)
{
	struct app_wifi_creds *out = cb_arg;

	if (out->ssid_len == 0 && ssid_len < sizeof(out->ssid)) {
		memcpy(out->ssid, ssid, ssid_len);
		out->ssid[ssid_len] = '\0';
		out->ssid_len = ssid_len;
	}
}

static int stored_creds(struct app_wifi_creds *out)
{
	struct wifi_credentials_personal c;

	memset(out, 0, sizeof(*out));
	wifi_credentials_for_each_ssid(stored_ssid_cb, out);
	if (out->ssid_len == 0) {
		return -ENOENT;
	}
	if (wifi_credentials_get_by_ssid_personal_struct(out->ssid, out->ssid_len,
							 &c) != 0 ||
	    c.password_len >= sizeof(out->psk)) {
		memset(out, 0, sizeof(*out));
		return -ENOENT;
	}
	memcpy(out->psk, c.password, c.password_len);
	out->psk[c.password_len] = '\0';
	out->psk_len = c.password_len;
	memset(&c, 0, sizeof(c));
	return 0;
}
#endif

int app_wifi_creds_resolve(struct app_wifi_creds *out)
{
#if defined(CONFIG_APP_WIFI_CRED_STORE)
	if (stored_creds(out) == 0) {
		return 0;
	}
#endif
	memset(out, 0, sizeof(*out));
#if defined(CONFIG_APP_WIFI)
	size_t ssid_len = strlen(CONFIG_APP_WIFI_SSID);
	size_t psk_len = strlen(CONFIG_APP_WIFI_PSK);

	if (ssid_len == 0 || ssid_len >= sizeof(out->ssid) ||
	    psk_len >= sizeof(out->psk)) {
		return -ENOENT;
	}
	memcpy(out->ssid, CONFIG_APP_WIFI_SSID, ssid_len + 1);
	out->ssid_len = ssid_len;
	memcpy(out->psk, CONFIG_APP_WIFI_PSK, psk_len + 1);
	out->psk_len = psk_len;
	return 0;
#else
	return -ENOENT;
#endif
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

/* Host provides connectivity on native_sim; nothing to watchdog. */
static void connectivity_watchdog(void) { }

#elif defined(CONFIG_APP_WIFI)

#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

#define L4_EVENTS   (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback wifi_cb;
static struct k_work_delayable reconnect_work;

#if defined(CONFIG_APP_STATIC_IP)
/* Assign a fixed IPv4 address/netmask/gateway (instead of DHCP) once the Wi-Fi
 * link is up. Requires DHCP auto-start to be disabled so it is not overridden. */
static void apply_static_ip(struct net_if *iface)
{
	struct net_in_addr addr, nm, gw;

	if (iface == NULL) {
		return;
	}
	if (net_addr_pton(NET_AF_INET, CONFIG_APP_STATIC_IP_ADDR, &addr) != 0) {
		LOG_ERR("Invalid static IP '%s'", CONFIG_APP_STATIC_IP_ADDR);
		return;
	}
	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	if (net_addr_pton(NET_AF_INET, CONFIG_APP_STATIC_NETMASK, &nm) == 0) {
		net_if_ipv4_set_netmask_by_addr(iface, &addr, &nm);
	}
	if (net_addr_pton(NET_AF_INET, CONFIG_APP_STATIC_GW, &gw) == 0) {
		net_if_ipv4_set_gw(iface, &gw);
	}
	LOG_INF("Applied static IP %s (gw %s)", CONFIG_APP_STATIC_IP_ADDR,
		CONFIG_APP_STATIC_GW);
}
#endif

/* Optionally override the Wi-Fi station MAC before connecting (ESP32 only).
 * Called while Wi-Fi is initialised but not yet started/connected. */
static void maybe_override_mac(void)
{
#if defined(CONFIG_WIFI_ESP32) && defined(CONFIG_APP_WIFI)
	if (strlen(CONFIG_APP_WIFI_MAC) == 0) {
		return;
	}

	unsigned int b[6];

	if (sscanf(CONFIG_APP_WIFI_MAC, "%x:%x:%x:%x:%x:%x",
		   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
		LOG_ERR("Bad APP_WIFI_MAC '%s'", CONFIG_APP_WIFI_MAC);
		return;
	}

	uint8_t mac[6];

	for (int i = 0; i < 6; i++) {
		mac[i] = (uint8_t)b[i];
	}
	int rc = esp_wifi_set_mac(WIFI_IF_STA, mac);

	LOG_INF("esp_wifi_set_mac(%s) -> %d", CONFIG_APP_WIFI_MAC, rc);
#endif
}

static int wifi_connect(const struct app_wifi_creds *creds)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params params = {0};

	if (iface == NULL) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	maybe_override_mac();

#if defined(CONFIG_WIFI_ESP32) && defined(CONFIG_APP_WIFI)
	/* Disable Wi-Fi power save. With modem sleep the device dozes between
	 * beacons and unicast frames (ARP/ping/TCP) can be dropped — the device
	 * associates and DHCP limps through, but it appears unreachable. An
	 * always-on OPC-UA server should stay awake. */
	{
		int ps = esp_wifi_set_ps(WIFI_PS_NONE);

		LOG_INF("esp_wifi_set_ps(NONE) -> %d", ps);
	}
#endif

	params.ssid = (const uint8_t *)creds->ssid;
	params.ssid_length = creds->ssid_len;
	params.psk = (const uint8_t *)creds->psk;
	params.psk_length = creds->psk_len;
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

	LOG_INF("Connecting to Wi-Fi SSID \"%s\"...", creds->ssid);
	app_step(APP_CTX_NETWQ, "wifi-connect");

	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));

	app_step(APP_CTX_NETWQ, "wifi-connect done");
	return rc;
}

#if defined(CONFIG_APP_NET_RECONNECT_REBOOT)
#include <zephyr/sys/reboot.h>
#endif

/* Attempt a reconnect. Keep it gentle: if the request errors (commonly
 * -EALREADY — the driver is already associating), just retry later rather than
 * interrupting the in-progress attempt. Breaking a genuinely stale association is
 * handled by the watchdog only after a prolonged stall (see below). */
static void reconnect_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	app_step(APP_CTX_NETWQ, "reconnect");
	app_alive(APP_CTX_NETWQ); /* the queue reached this item: progress */
	if (app_net_is_connected() || prov_mode) {
		return;
	}

	struct app_wifi_creds creds;

	(void)app_wifi_creds_resolve(&creds);
	int rc = wifi_connect(&creds);

	memset(&creds, 0, sizeof(creds));
	if (rc != 0) {
		net_wq_reschedule(&reconnect_work, K_SECONDS(2));
	}
}

/* Event-independent recovery: runs every status_work tick (~3 s). Reconnection
 * on management events alone can dead-end (associated-but-no-IP, a no-op connect,
 * a silent lease loss, or a router L3 block that fires no event). The watchdog
 * judges health by actual reachability — not just association + IP — and drives
 * the connected state (both ways) plus the last-resort reboot. */
#define WD_TRIGGER_TICKS  2  /* ~6 s of bad before nudging a reconnect */
#define WD_PING_STALL_MAX 5  /* ~15 s with no ping replies = data path down */

static void connectivity_watchdog(void)
{
	static uint32_t bad_ticks;

	if (prov_mode) {
		return; /* offline by design while waiting for credentials */
	}

	bool healthy = read_iface_ipv4() != 0; /* an IP is the minimum */

#if defined(CONFIG_NET_IPV4) && !defined(CONFIG_NET_SOCKETS_OFFLOAD)
	/* Reachability gate: once the gateway has answered at least once, a sustained
	 * loss of ping replies means the data path is down (e.g. a router L3 block)
	 * even while we remain associated with a valid IP. The `proven` guard avoids
	 * false-offline loops on networks that never answer ICMP to the target. */
	static uint32_t last_ping_ok;
	static uint32_t ping_stall;
	static bool ping_proven;
	uint32_t now_ok = (uint32_t)atomic_get(&ping_ok);

	if (now_ok != last_ping_ok) {
		ping_proven = true;
		ping_stall = 0;
		last_ping_ok = now_ok;
	} else {
		ping_stall++;
	}
	if (ping_proven && ping_stall >= WD_PING_STALL_MAX) {
		healthy = false;
	}
#endif

	if (healthy) {
		/* Recovered (DHCP bound, or an L3 block lifted — no L4 event fires for a
		 * block, so restore the connected state here). */
		if (!app_net_is_connected()) {
			LOG_INF("Connectivity restored");
			mark_connected(true);
		}
		bad_ticks = 0;
		return;
	}

	if (app_net_is_connected()) {
		LOG_WRN("Connectivity lost (no reachable data path) — recovering");
		mark_connected(false);
	}

	bad_ticks++;

	/* Escalate gently. ~every 6 s nudge a reconnect (does not interrupt an
	 * in-progress association). After a prolonged stall (~30 s), clear any stale
	 * association once so a wedged connect can restart cleanly; its disconnect
	 * event then drives a fresh reconnect. */
	if ((bad_ticks % WD_TRIGGER_TICKS) == 0) {
		if (bad_ticks == 10) {
			struct net_if *iface = net_if_get_first_wifi();

			if (iface != NULL) {
				LOG_WRN("Offline ~30 s — clearing association to restart connect");
				app_step(APP_CTX_NETWQ, "wifi-disconnect");
				(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
				app_step(APP_CTX_NETWQ, "wifi-disconnect done");
			}
		}
		net_wq_reschedule(&reconnect_work, K_NO_WAIT);
	}

#if defined(CONFIG_APP_NET_RECONNECT_REBOOT)
	/* status_work ticks every 3 s, so offline seconds ~= bad_ticks * 3. */
	if (bad_ticks * 3u >= (uint32_t)CONFIG_APP_NET_REBOOT_TIMEOUT_S) {
		LOG_ERR("Offline > %d s despite reconnects — rebooting to recover",
			CONFIG_APP_NET_REBOOT_TIMEOUT_S);
		sys_reboot(SYS_REBOOT_COLD);
	}
#endif
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
		mark_connected(false);
		if (prov_mode) {
			break;
		}
		LOG_WRN("Network connectivity lost — scheduling reconnect");
		net_wq_reschedule(&reconnect_work, K_SECONDS(2));
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

		g_last_reason = status->disconn_reason;
		mark_connected(false); /* update state + LED now, not only on L4 */
		if (prov_mode) {
			LOG_WRN("Wi-Fi disconnected (reason %d)",
				status->disconn_reason);
			try_done(-ECONNREFUSED);
			return;
		}
		LOG_WRN("Wi-Fi disconnected (reason %d) — will reconnect",
			status->disconn_reason);
		net_wq_reschedule(&reconnect_work, K_SECONDS(2));
	} else if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		if (status->status) {
			g_last_reason = status->status;
			render_status(DISPLAY_STAGE_ERROR);
			if (prov_mode) {
				LOG_WRN("Wi-Fi association failed (%d)",
					status->status);
				try_done(-ECONNREFUSED);
				return;
			}
			LOG_WRN("Wi-Fi association failed (%d) — retrying",
				status->status);
			net_wq_reschedule(&reconnect_work, K_SECONDS(2));
		} else {
			LOG_INF("Wi-Fi associated; awaiting IP address");
#if defined(CONFIG_APP_STATIC_IP)
			apply_static_ip(iface);
#endif
		}
	}
}

int app_net_init(void)
{
	net_wq_start();
	k_work_init_delayable(&reconnect_work, reconnect_handler);

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENTS);
	net_mgmt_add_event_callback(&l4_cb);
	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, WIFI_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	/* Periodically refresh the on-screen diagnostics (RSSI, state, ...). */
	k_work_init_delayable(&status_work, status_work_handler);
	net_wq_reschedule(&status_work, K_SECONDS(3));
	app_diag_watch_work("status", &status_work);
	app_diag_watch_work("reconn", &reconnect_work);

#if defined(CONFIG_APP_WIFI_PROVISIONER)
	/* The provisioner joins only to test candidate credentials, through
	 * app_net_try_credentials(). */
	return 0;
#else
	struct app_wifi_creds creds;
	bool have_creds = app_wifi_creds_resolve(&creds) == 0;

#if defined(CONFIG_APP_PROV_HANDOFF)
	if (!have_creds) {
		app_prov_handoff_no_credentials(); /* reboots when it can */
	}
	app_prov_button_start();
#endif

	if (!have_creds) {
		LOG_ERR("Wi-Fi SSID is empty — set APP_WIFI_SSID via a local "
			"overlay (see overlay-wifi-credentials.conf.example)");
		return -EINVAL;
	}

	int rc = wifi_connect(&creds);

	memset(&creds, 0, sizeof(creds));
	return rc;
#endif
}

struct k_work_q *app_net_workq(void)
{
	return &net_wq;
}

#if defined(CONFIG_APP_WIFI_PROVISIONER)
int app_net_try_credentials(const struct app_wifi_creds *creds,
			    k_timeout_t timeout)
{
	struct net_if *iface = net_if_get_first_wifi();

	k_sem_reset(&try_sem);
	try_active = true;
	int rc = wifi_connect(creds);

	if (rc == 0) {
		/* Success means an IPv4 address, not just association: the
		 * events only wake this loop, the address decides. */
		k_timepoint_t end = sys_timepoint_calc(timeout);
		/* The ESP32 driver can report a spurious failure on the first
		 * association after boot; only give up after a few. */
		int attempts = 1;

		rc = -ETIMEDOUT;
		while (!sys_timepoint_expired(end)) {
			if (k_sem_take(&try_sem, K_MSEC(500)) == 0 &&
			    try_result != 0) {
				if (attempts >= 3) {
					rc = try_result;
					break;
				}
				attempts++;
				k_msleep(1000);
				try_active = true;
				(void)wifi_connect(creds);
				continue;
			}
			if (read_iface_ipv4() != 0) {
				rc = 0;
				break;
			}
			try_active = true; /* re-arm after a wake-up */
		}
	}
	try_active = false;
	if (rc != 0 && iface != NULL) {
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	}
	return rc;
}
#endif

#else /* hardware with a wired/other interface and no explicit Wi-Fi */

static struct net_mgmt_event_callback l4_cb;

/* No Wi-Fi reconnect state machine on a wired link. */
static void connectivity_watchdog(void) { }

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
