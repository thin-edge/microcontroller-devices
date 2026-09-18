/* SPDX-License-Identifier: Apache-2.0 */

#include "boot_request.h"

#include <errno.h>
#include <string.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <esp_rom_sys.h>
#endif

#define IMAGE_MAGIC 0x96f3b83du /* MCUboot image header */

int boot_request_read(struct boot_request *out)
{
	const struct flash_area *fa;
	int rc = flash_area_open(PARTITION_ID(bootreq_partition), &fa);

	if (rc != 0) {
		return rc;
	}
	rc = flash_area_read(fa, 0, out, sizeof(*out));
	flash_area_close(fa);
	if (rc != 0) {
		return rc;
	}
	return out->magic == BOOT_REQUEST_MAGIC ? 0 : -ENOENT;
}

int boot_request_set(enum boot_request_target target,
		     enum boot_request_reason reason)
{
	const struct boot_request req = {
		.magic = BOOT_REQUEST_MAGIC,
		.target = target,
		.reason = reason,
	};
	const struct flash_area *fa;
	int rc = flash_area_open(PARTITION_ID(bootreq_partition), &fa);

	if (rc != 0) {
		return rc;
	}
	rc = flash_area_flatten(fa, 0, fa->fa_size);
	if (rc == 0) {
		rc = flash_area_write(fa, 0, &req, sizeof(req));
	}
	flash_area_close(fa);
	return rc;
}

int boot_request_clear(void)
{
	const struct flash_area *fa;
	int rc = flash_area_open(PARTITION_ID(bootreq_partition), &fa);

	if (rc != 0) {
		return rc;
	}
	rc = flash_area_flatten(fa, 0, fa->fa_size);
	flash_area_close(fa);
	return rc;
}

bool boot_request_provisioner_present(void)
{
	const struct flash_area *fa;
	uint32_t magic = 0;
	int rc;

	if (flash_area_open(PARTITION_ID(prov_partition), &fa) != 0) {
		return false;
	}
	rc = flash_area_read(fa, 0, &magic, sizeof(magic));
	flash_area_close(fa);
	return rc == 0 && magic == IMAGE_MAGIC;
}

FUNC_NORETURN void boot_request_reboot(void)
{
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
	esp_rom_software_reset_system();
#endif
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}
