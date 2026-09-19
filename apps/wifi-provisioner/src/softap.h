/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SOFTAP_H_
#define SOFTAP_H_

#include <zephyr/toolchain.h>

/** Run the SoftAP / captive-portal front end; never returns. */
FUNC_NORETURN void softap_run(void);

#endif /* SOFTAP_H_ */
