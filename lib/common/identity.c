/* SPDX-License-Identifier: Apache-2.0 */

#include "identity.h"
#include "net.h"
#if defined(CONFIG_APP_WIFI_CRED_STORE)
#include "prov_c8y.h"
#endif

#include <stdbool.h>

#include <app_version.h> /* APP_VERSION_STRING, generated from the app VERSION file */

const char *app_identity_device_id(void)
{
#if defined(CONFIG_APP_WIFI_CRED_STORE)
	/* A device onboarded through the ZTP provisioner was registered in
	 * Cumulocity under the external ID the server issued its one-time
	 * password for. That ID is its identity from then on — its certificate
	 * is issued for it — so it takes precedence over the hostname. */
	static char ztp_id[64];
	static bool looked;

	if (!looked) {
		looked = true;
		if (prov_c8y_external_id(ztp_id, sizeof(ztp_id)) != 0) {
			ztp_id[0] = '\0';
		}
	}
	if (ztp_id[0] != '\0') {
		return ztp_id;
	}
#endif
	return app_net_hostname();
}

const char *app_identity_firmware_name(void)
{
	return CONFIG_APP_FIRMWARE_NAME;
}

const char *app_identity_firmware_version(void)
{
	return APP_VERSION_STRING;
}

const char *app_identity_build_timestamp(void)
{
	return __DATE__ " " __TIME__;
}
