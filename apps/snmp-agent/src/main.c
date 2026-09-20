/* SPDX-License-Identifier: Apache-2.0
 *
 * SNMP agent firmware entry point.
 *
 * Boot sequence:
 *   1. Bring up connectivity (Wi-Fi station on hardware; host networking on
 *      native_sim) and wait until the network is usable.
 *   2. Start the SNMPv2c agent (UDP responder on port 161 + trap originator),
 *      which presents the device as a managed switch/router.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "net.h"
#include "display.h"
#include "snmp_agent.h"
#if defined(CONFIG_TEDGE)
#include "tedge_glue.h"
#endif

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("SNMP agent firmware starting: device \"%s\"",
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

	ret = snmp_agent_start();
	if (ret) {
		LOG_ERR("Failed to start SNMP agent (%d)", ret);
		display_status_stage(DISPLAY_STAGE_ERROR);
		return 0;
	}
	LOG_INF("SNMP agent running on UDP port %d", CONFIG_APP_SNMP_PORT);

#if defined(CONFIG_TEDGE)
	/* Device management runs alongside the protocol agent. */
	if (tedge_glue_start() == 0) {
#if defined(CONFIG_TEDGE_TELEMETRY)
		tedge_glue_start_telemetry();
#endif
	}
#endif

	return 0;
}
