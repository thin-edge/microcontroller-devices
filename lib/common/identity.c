/* SPDX-License-Identifier: Apache-2.0 */

#include "identity.h"
#include "net.h"

#include <app_version.h> /* APP_VERSION_STRING, generated from the app VERSION file */

const char *app_identity_device_id(void)
{
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
