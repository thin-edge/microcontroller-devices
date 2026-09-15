/* SPDX-License-Identifier: Apache-2.0
 *
 * Connectivity status LED: blinks while the device is not on the network and is
 * steady once connected, so an operator can tell a device-side network problem
 * from a collector-side one at a glance. Uses the board's `led0` alias; a no-op
 * on boards without one (and on native_sim). Complements the optional TFT status.
 */
#ifndef APP_STATUS_LED_H_
#define APP_STATUS_LED_H_

#include <stdbool.h>

/** Reflect connectivity on the status LED: steady when connected, blinking when
 *  not. Safe to call regardless of board/LED availability. */
void status_led_set_connected(bool connected);

#endif /* APP_STATUS_LED_H_ */
