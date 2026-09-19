/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IMPROV_H_
#define IMPROV_H_

#include <zephyr/toolchain.h>

/** Advertise Improv Wi-Fi and provision. Never returns: every exit clears the
 *  boot request and reboots into the application. */
FUNC_NORETURN void improv_run(void);

#endif /* IMPROV_H_ */
