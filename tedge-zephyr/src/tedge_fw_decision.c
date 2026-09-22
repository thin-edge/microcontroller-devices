/* SPDX-License-Identifier: Apache-2.0
 *
 * Which version the running image is, and what a firmware operation means
 * given it. Pure, so the pre-release cases ("0.4.0-rc1" against "0.4.0")
 * are unit-tested rather than reasoned about.
 *
 * The version is the application's own string (tedge_identity's
 * firmware_version, normally APP_VERSION_STRING) because the bootloader's
 * image header holds only MAJOR.MINOR.PATCH: installing "0.4.0-rc1" and then
 * reading "0.4.0" back from the header would look like a rollback. With a
 * swap-based MCUboot the image running is always the one in the primary
 * slot, so the compiled-in string and the header describe the same image.
 */

#include "tedge_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int tedge_fw_version_pick(const char *app_version, const char *header_version,
			  char *buf, size_t len)
{
	const char *v = (app_version != NULL && app_version[0] != '\0')
				? app_version
				: header_version;
	int n;

	if (buf == NULL || len == 0 || v == NULL || v[0] == '\0') {
		return -ENOENT;
	}
	n = snprintf(buf, len, "%s", v);
	return (n > 0 && (size_t)n < len) ? 0 : -ENOSPC;
}

bool tedge_fw_is_running(const char *running_name, const char *running_version,
			 const char *name, const char *version)
{
	return running_version != NULL && running_version[0] != '\0' &&
	       strcmp(running_version, version) == 0 &&
	       strcmp(running_name, name) == 0;
}

enum tedge_fw_boot tedge_fw_boot_outcome(bool confirmed, const char *running,
					 const char *pending)
{
	if (!confirmed) {
		return TEDGE_FW_BOOT_TEST;
	}
	return (strcmp(running, pending) == 0) ? TEDGE_FW_BOOT_DONE
						: TEDGE_FW_BOOT_REVERTED;
}
