/* SPDX-License-Identifier: Apache-2.0
 *
 * OPC-UA server firmware entry point.
 *
 * Boot sequence:
 *   1. Bring up connectivity (Wi-Fi station on hardware; host networking on
 *      native_sim) and wait until the network is usable.
 *   2. Start the OPC-UA server (when CONFIG_APP_OPCUA_SERVER is enabled).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "net.h"
#include "display.h"

#if defined(CONFIG_APP_OPCUA_SERVER)
#include "opcua_server.h"
#if defined(CONFIG_TEDGE)
#include "tedge_glue.h"
#endif
#endif

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("OPC-UA server firmware starting: device \"%s\"",
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

#if defined(CONFIG_APP_OPCUA_SERVER)
	ret = opcua_server_start();
	if (ret) {
		LOG_ERR("Failed to start OPC-UA server (%d)", ret);
		display_status_stage(DISPLAY_STAGE_ERROR);
		return 0;
	}
	LOG_INF("OPC-UA server running on port %d", CONFIG_APP_OPCUA_PORT);
#else
	LOG_INF("OPC-UA server disabled (CONFIG_APP_OPCUA_SERVER=n) — "
		"connectivity skeleton only");
#endif

#if defined(CONFIG_TEDGE)
	/* Device management runs alongside the protocol server. */
	if (tedge_glue_start() == 0) {
#if defined(CONFIG_TEDGE_TELEMETRY)
		tedge_glue_start_telemetry();
#endif
	}
#endif

	return 0;
}
