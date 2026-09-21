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
#elif defined(CONFIG_APP_PROV_ZTP)
#include "ztp.h"
#else
#include "improv.h"
#endif
#include "net.h"
#if defined(CONFIG_APP_PROV_ZTP_CRYPTO_SELFTEST)
#include "ztp_selftest.h"
#endif

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	LOG_INF("Wi-Fi provisioner starting");

#if defined(CONFIG_APP_PROV_ZTP_CRYPTO_SELFTEST)
	/* Diagnostic only: a failure is logged, provisioning still starts. */
	(void)ztp_selftest_run();
#endif

	int rc = app_net_init();

	if (rc) {
		LOG_ERR("Wi-Fi init failed (%d)", rc);
	}
#if defined(CONFIG_APP_WIFI_PROV_SOFTAP)
	softap_run();
#elif defined(CONFIG_APP_PROV_ZTP)
	ztp_run();
#else
	improv_run();
#endif
}
