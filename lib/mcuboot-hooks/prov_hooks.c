/* SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot hooks: boot the Wi-Fi provisioner from `prov_partition` when the
 * boot request in `bootreq_partition` asks for it; otherwise leave MCUboot's
 * normal primary/secondary logic (swap, revert) untouched.
 *
 * The Espressif launcher only knows "primary" and "secondary" and resolves the
 * image's flash area through flash_area_id_from_multi_image_slot(), so while a
 * provisioner launch is in progress that lookup is redirected to prov.
 */
#include <zephyr/kernel.h>
#include <string.h>
#include <zephyr/storage/flash_map.h>

#include "bootutil/bootutil.h"
#include "bootutil/bootutil_public.h"
#include "bootutil/image.h"
#include "bootutil/fault_injection_hardening.h"
#include "bootutil/boot_hooks.h"
#include "bootutil/bootutil_log.h"
#include "bootutil/mcuboot_status.h"
#include "bootutil_priv.h"

#define __MCUBOOT_HOOKS__
#include "boot_request.h" /* lib/common: the record the apps write */

BOOT_LOG_MODULE_DECLARE(mcuboot);

/* MCUboot's own log goes to a Zephyr console this bootloader build does not
 * have on Espressif; the ROM printf reaches the same port as the "I (boot)"
 * lines of the Espressif loader. */
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <esp_rom_sys.h>
#define HOOK_LOG(fmt, ...) esp_rom_printf("I (prov-hook): " fmt "\n", ##__VA_ARGS__)
#else
#define HOOK_LOG(fmt, ...) BOOT_LOG_INF(fmt, ##__VA_ARGS__)
#endif


static bool prov_launch;
static struct image_header prov_hdr;
static uint8_t tmpbuf[256];

static bool provisioner_requested(void)
{
	const struct flash_area *fa;
	struct boot_request req = { 0 };
	int rc;

	if (flash_area_open(PARTITION_ID(bootreq_partition), &fa) != 0) {
		return false;
	}
	rc = flash_area_read(fa, 0, &req, sizeof(req));
	flash_area_close(fa);
	return rc == 0 && req.magic == BOOT_REQUEST_MAGIC &&
	       req.target == BOOT_REQUEST_PROVISIONER;
}

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
	FIH_DECLARE(fih_rc, FIH_FAILURE);
	const struct flash_area *fa;

	if (!provisioner_requested()) {
		FIH_RET(FIH_BOOT_HOOK_REGULAR);
	}
	HOOK_LOG("boot request: provisioner");

	if (flash_area_open(PARTITION_ID(prov_partition), &fa) != 0) {
		HOOK_LOG("provisioner partition missing; booting the application");
		FIH_RET(FIH_BOOT_HOOK_REGULAR);
	}
	if (boot_image_load_header(fa, &prov_hdr) != 0) {
		HOOK_LOG("no provisioner image; booting the application");
		flash_area_close(fa);
		FIH_RET(FIH_BOOT_HOOK_REGULAR);
	}
	/* bootutil_img_validate() bounds the image by the application slots'
	 * size, which it reads from a loader state (with swap modes a NULL
	 * state is dereferenced). Build that state the way boot_go() does. */
	struct boot_loader_state *st = boot_get_loader_state();

	memset(st, 0, sizeof(*st));
	if (boot_open_all_flash_areas(st) != 0 || boot_read_sectors(st, NULL) != 0) {
		HOOK_LOG("cannot read the slot layout; booting the application");
		boot_close_all_flash_areas(st);
		memset(st, 0, sizeof(*st));
		flash_area_close(fa);
		FIH_RET(FIH_BOOT_HOOK_REGULAR);
	}
	HOOK_LOG("validating provisioner at 0x%x", (unsigned int)flash_area_get_off(fa));
	FIH_CALL(bootutil_img_validate, fih_rc, st, &prov_hdr, fa, tmpbuf,
		 sizeof(tmpbuf), NULL, 0, NULL);
	boot_close_all_flash_areas(st);
	memset(st, 0, sizeof(*st));
	if (FIH_NOT_EQ(fih_rc, FIH_SUCCESS)) {
		HOOK_LOG("provisioner image invalid; booting the application");
		flash_area_close(fa);
		FIH_RET(FIH_BOOT_HOOK_REGULAR);
	}

	rsp->br_flash_dev_id = flash_area_get_device_id(fa);
	rsp->br_image_off = flash_area_get_off(fa);
	rsp->br_hdr = &prov_hdr;
	flash_area_close(fa);
	prov_launch = true;
	HOOK_LOG("launching provisioner");
	FIH_RET(FIH_SUCCESS);
}

int flash_area_id_from_multi_image_slot_hook(int image_index, int slot,
					     int *area_id)
{
	ARG_UNUSED(image_index);
	ARG_UNUSED(slot);

	if (prov_launch) {
		*area_id = PARTITION_ID(prov_partition);
		return 0;
	}
	return BOOT_HOOK_REGULAR;
}

int flash_area_get_device_id_hook(const struct flash_area *fa, uint8_t *dev_id)
{
	ARG_UNUSED(fa);
	ARG_UNUSED(dev_id);
	return BOOT_HOOK_REGULAR;
}

/* MCUboot's own log is off on Espressif, so a refused application would boot
 * to silence: say why. */
void mcuboot_status_change(mcuboot_status_type_t status)
{
	if (status == MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND) {
		HOOK_LOG("no valid application in slot0 (unsigned, corrupted or "
			 "erased): not booting");
	} else if (status == MCUBOOT_STATUS_BOOT_FAILED) {
		HOOK_LOG("booting the application failed");
	}
}
