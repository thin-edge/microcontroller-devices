/* SPDX-License-Identifier: Apache-2.0
 *
 * Connectivity management: Wi-Fi station mode on hardware, host networking on
 * native_sim. Provides a simple "bring the network up and tell me when it's
 * ready" interface plus reconnect handling.
 */
#ifndef APP_NET_H_
#define APP_NET_H_

#include <zephyr/kernel.h>
#include "display.h"

/**
 * Start connectivity. On Wi-Fi targets this begins station-mode association
 * using the configured credentials and installs reconnect handling. On
 * native_sim (host-offloaded sockets) connectivity is provided by the host,
 * so this is effectively a no-op that reports "connected".
 *
 * @return 0 on success, negative errno on failure to start.
 */
int app_net_init(void);

/**
 * Block until the network is connected (usable for opening sockets) or the
 * timeout elapses.
 *
 * @param timeout how long to wait (K_FOREVER to wait indefinitely).
 * @return 0 if connected, -ETIMEDOUT on timeout.
 */
int app_net_wait_connected(k_timeout_t timeout);

/**
 * @return true if the network is currently considered connected.
 */
bool app_net_is_connected(void);

/**
 * @return the device's current IPv4 address in network byte order, or 0 if
 *         none is assigned (e.g. on native_sim offloaded sockets).
 */
uint32_t app_net_ipv4(void);

/**
 * @return the device's unique network hostname (base name plus a MAC-derived
 *         suffix when CONFIG_NET_HOSTNAME_UNIQUE is enabled). Stable per chip;
 *         used as the OPC-UA application name and mDNS/DNS-SD identity.
 */
const char *app_net_hostname(void);

/**
 * Render the current network diagnostics (IP, gateway, netmask, Wi-Fi RSSI/
 * state, last disconnect reason, hostname) on the status display for the given
 * stage. No-op when no display is present.
 */
void app_net_show_status(enum display_stage stage);

/** Wi-Fi station credentials (WPA2-Personal, or open when psk_len is 0). */
struct app_wifi_creds {
	char ssid[33]; /* 32 bytes + NUL */
	size_t ssid_len;
	char psk[65];  /* 64 bytes + NUL */
	size_t psk_len;
};

/**
 * Resolve the credentials to join with: the stored entry (with
 * CONFIG_APP_WIFI_CRED_STORE), else the compile-time CONFIG_APP_WIFI_SSID/PSK.
 *
 * @return 0 with @p out filled, or -ENOENT when no source has any.
 */
int app_wifi_creds_resolve(struct app_wifi_creds *out);

/** The connectivity work queue (the sw0 monitors run on it). */
struct k_work_q *app_net_workq(void);

#if defined(CONFIG_APP_WIFI_PROVISIONER)
/**
 * Provisioner image: join with @p creds and wait for an IPv4 address.
 * Connectivity recovery (reconnects, reachability, last-resort reboot) is
 * suspended in this mode, so a failed attempt is not retried by net.c.
 *
 * @return 0 once an address is assigned; -ECONNREFUSED when the association
 *         fails; -ETIMEDOUT when no address arrives within @p timeout. On
 *         failure the station is disconnected again.
 */
int app_net_try_credentials(const struct app_wifi_creds *creds,
			    k_timeout_t timeout);
#endif

#endif /* APP_NET_H_ */
