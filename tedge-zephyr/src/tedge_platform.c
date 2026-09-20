/* SPDX-License-Identifier: Apache-2.0
 *
 * Platform bits the client needs: the default device ID and the reset.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>

#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <esp_rom_sys.h>
#endif

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

int tedge_platform_default_id(char *buf, size_t len)
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *link;

	if (iface == NULL) {
		return -ENODEV;
	}
	link = net_if_get_link_addr(iface);
	if (link == NULL || link->len < 6) {
		return -ENODEV;
	}
	return snprintf(buf, len, "%s-%02x%02x%02x%02x%02x%02x",
			CONFIG_TEDGE_DEVICE_ID_PREFIX, link->addr[0],
			link->addr[1], link->addr[2], link->addr[3],
			link->addr[4], link->addr[5]) < (int)len
		       ? 0
		       : -ENOSPC;
}

FUNC_NORETURN void tedge_platform_reset(void)
{
	const struct tedge_hooks *hooks = tedge_hook_table();

	LOG_INF("resetting the device");
	log_flush(); /* deferred logging: get the line out first */
	if (hooks != NULL && hooks->reset != NULL) {
		hooks->reset(hooks->user_data);
		/* A hook that returns is a bug; fall through to the default. */
		LOG_ERR("the application's reset hook returned");
	}
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
	/* A full-system reset. After sys_reboot()'s CPU-only reset with Wi-Fi
	 * running, an ESP32-C6 hangs in MCUboot until it is power-cycled
	 * (c8y-direct-spikes, P2).
	 */
	esp_rom_software_reset_system();
#else
	sys_reboot(SYS_REBOOT_COLD);
#endif
	CODE_UNREACHABLE;
}
