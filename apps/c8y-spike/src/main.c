/* SPDX-License-Identifier: Apache-2.0
 *
 * c8y-direct spike firmware entry point.
 *
 * Plays the part of a user application that includes tedge-zephyr: it owns
 * connectivity (lib/common) and runs whichever spikes are enabled once the
 * network is up.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tedge/tedge.h>

#include "net.h"
#include "spike.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("c8y spike firmware starting: tedge-zephyr %s", tedge_version());
	LOG_INF("spikes: tls_mqtt=%d ota=%d enroll=%d remote_access=%d",
		IS_ENABLED(CONFIG_SPIKE_TLS_MQTT), IS_ENABLED(CONFIG_SPIKE_OTA),
		IS_ENABLED(CONFIG_SPIKE_AUTH_ENROLLED),
		IS_ENABLED(CONFIG_SPIKE_REMOTE_ACCESS));

#if defined(CONFIG_SPIKE_OTA)
	spike_ota_log_boot();
#endif

	ret = app_net_init();
	if (ret) {
		LOG_ERR("Connectivity init failed (%d)", ret);
		return 0;
	}

	LOG_INF("Waiting for network...");
	ret = app_net_wait_connected(K_SECONDS(60));
	if (ret) {
		LOG_ERR("Network did not come up in time (%d)", ret);
		return 0;
	}

	LOG_INF("Network up");

	if (IS_ENABLED(CONFIG_SPIKE_TLS_MQTT)) {
		spike_mqtt_start();
	}
	return 0;
}
