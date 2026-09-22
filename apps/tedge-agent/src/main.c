/* SPDX-License-Identifier: Apache-2.0
 *
 * Device-management agent firmware entry point.
 *
 * Boot sequence:
 *   1. Bring up connectivity (Wi-Fi station, stored or provisioned
 *      credentials; with none, lib/common hands off to the provisioner) and
 *      wait until the network is usable.
 *   2. Start the thin-edge.io client. There is no protocol server: the
 *      device is managed from the cloud, and remote access reaches hosts on
 *      its LAN.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "net.h"
#include "display.h"
#include "tedge_glue.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("Device agent firmware starting: device \"%s\"",
		CONFIG_APP_DEVICE_NAME);

	display_status_init();
	display_status_stage(DISPLAY_STAGE_WIFI_CONNECTING);

	ret = app_net_init();
	if (ret) {
		LOG_ERR("Connectivity init failed (%d)", ret);
		display_status_stage(DISPLAY_STAGE_ERROR);
		return 0;
	}

	LOG_INF("Waiting for network...");
	ret = app_net_wait_connected(K_SECONDS(60));
	if (ret) {
		LOG_ERR("Network did not come up in time (%d)", ret);
		display_status_stage(DISPLAY_STAGE_ERROR);
		return 0;
	}

	ret = tedge_glue_start();
	if (ret) {
		LOG_ERR("Device management did not start (%d)", ret);
		display_status_stage(DISPLAY_STAGE_ERROR);
		return 0;
	}
	LOG_INF("Device agent running");
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	size_t unused;

	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		LOG_INF("main stack: %zu of %d bytes never used", unused,
			CONFIG_MAIN_STACK_SIZE);
	}
#endif
	return 0;
}
