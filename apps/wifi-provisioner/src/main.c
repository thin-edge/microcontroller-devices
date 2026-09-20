/* SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi provisioner: a separate image in `prov_partition` that MCUboot
 * launches when the application has set the boot request (no credentials, or
 * the sw0 provisioning pattern). It advertises the Improv Wi-Fi BLE service
 * (or, with CONFIG_APP_WIFI_PROV_SOFTAP, a SoftAP captive portal),
 * tests the credentials a client sends by joining the network, stores them
 * for the application and reboots back into it. It carries Bluetooth so the
 * applications do not have to.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_APP_WIFI_PROV_SOFTAP)
#include "softap.h"
#else
#include "improv.h"
#endif
#include "net.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	LOG_INF("Wi-Fi provisioner starting");

	int rc = app_net_init();

	if (rc) {
		LOG_ERR("Wi-Fi init failed (%d)", rc);
	}
#if defined(CONFIG_APP_WIFI_PROV_SOFTAP)
	softap_run();
#else
	improv_run();
#endif
}
