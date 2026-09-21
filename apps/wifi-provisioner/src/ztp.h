/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ZTP_H_
#define ZTP_H_

#include <zephyr/toolchain.h>

/**
 * @brief Run the lab-ztp-provisioner BLE peripheral until the device is
 * provisioned, then reboot into the application. Does not return.
 */
FUNC_NORETURN void ztp_run(void);

#endif /* ZTP_H_ */
