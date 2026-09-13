/* SPDX-License-Identifier: Apache-2.0
 *
 * Connectivity management: Wi-Fi station mode on hardware, host networking on
 * native_sim. Provides a simple "bring the network up and tell me when it's
 * ready" interface plus reconnect handling.
 */
#ifndef APP_NET_H_
#define APP_NET_H_

#include <zephyr/kernel.h>

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

#endif /* APP_NET_H_ */
