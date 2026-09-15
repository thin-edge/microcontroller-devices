/* SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP server firmware entry point.
 *
 * Boot sequence:
 *   1. Bring up connectivity (Wi-Fi station on hardware; host networking on
 *      native_sim) and wait until the network is usable.
 *   2. Start the Modbus TCP server (raw-ADU server + TCP listener on port 502).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "net.h"
#include "display.h"
#include "modbus_server.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("Modbus TCP server firmware starting: device \"%s\"",
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

	ret = modbus_server_start();
	if (ret) {
		LOG_ERR("Failed to start Modbus server (%d)", ret);
		display_status_stage(DISPLAY_STAGE_ERROR);
		return 0;
	}
	LOG_INF("Modbus TCP server running on port %d", CONFIG_APP_MODBUS_PORT);

	return 0;
}
