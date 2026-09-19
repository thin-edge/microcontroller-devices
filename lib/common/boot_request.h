/* SPDX-License-Identifier: Apache-2.0
 *
 * Boot request: a record in its own flash partition (`bootreq_partition`)
 * that tells MCUboot which image to launch. Erased flash means "no request":
 * MCUboot then boots the application through its normal primary/secondary
 * logic. The application sets the request to hand over to the Wi-Fi
 * provisioner (in `prov_partition`); only the provisioner clears it, so a reset
 * or a power cut inside the provisioner comes back into the provisioner.
 *
 * The layout is shared with the MCUboot hooks (lib/mcuboot-hooks), which read
 * it without Zephyr's settings or NVS: keep it a plain little-endian record.
 */
#ifndef APP_BOOT_REQUEST_H_
#define APP_BOOT_REQUEST_H_

#include <stdbool.h>
#include <stdint.h>
#if !defined(__MCUBOOT_HOOKS__)
#include <zephyr/toolchain.h>
#endif

#define BOOT_REQUEST_MAGIC 0x564f5250u /* "PROV" */

enum boot_request_target {
	BOOT_REQUEST_PROVISIONER = 1,
};

/** Why the application asked for the provisioner. */
enum boot_request_reason {
	/** No credentials: the provisioner waits (idle) when its window ends. */
	BOOT_REQUEST_NO_CREDENTIALS = 1,
	/** The operator asked (sw0 pattern): the window's end returns to the
	 *  application, which still has working credentials. */
	BOOT_REQUEST_OPERATOR = 2,
};

struct boot_request {
	uint32_t magic;
	uint32_t target;
	uint32_t reason;
	uint32_t reserved; /* keeps the record a multiple of 8 bytes */
};

#if !defined(__MCUBOOT_HOOKS__)
/**
 * Read the boot request.
 * @return 0 with @p out filled when a request is set; -ENOENT when none is.
 */
int boot_request_read(struct boot_request *out);

/** Set a request for @p target (replaces any previous one). */
int boot_request_set(enum boot_request_target target,
		     enum boot_request_reason reason);

/** Clear the request, so the next boot runs the application. */
int boot_request_clear(void);

/**
 * @return true when `prov_partition` starts with an MCUboot image header, i.e.
 *         a provisioner has been flashed. MCUboot still checks its signature
 *         before launching it.
 */
bool boot_request_provisioner_present(void);

/**
 * Reboot so MCUboot acts on the boot request. On Espressif SoCs this is a
 * full digital-system reset, not sys_reboot()'s CPU reset: on the ESP32-C6 a
 * CPU reset taken with Bluetooth and Wi-Fi running left MCUboot hanging in its
 * start-up until the next power cycle.
 */
FUNC_NORETURN void boot_request_reboot(void);
#endif

#endif /* APP_BOOT_REQUEST_H_ */
