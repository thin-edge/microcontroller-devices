/* SPDX-License-Identifier: Apache-2.0
 *
 * Connectivity status LED: blinks while the device is not on the network and is
 * steady once connected, so an operator can tell a device-side network problem
 * from a collector-side one at a glance. Uses the board's `led0` alias; a no-op
 * on boards without one (and on native_sim). Complements the optional TFT status.
 *
 * BLE provisioning adds further patterns (see enum status_led_mode).
 */
#ifndef APP_STATUS_LED_H_
#define APP_STATUS_LED_H_

#include <stdbool.h>
#include <stdint.h>

enum status_led_mode {
	STATUS_LED_CONNECTED,    /**< steady on */
	STATUS_LED_DISCONNECTED, /**< 250 ms on / 250 ms off */
	STATUS_LED_PROVISIONING, /**< two short blinks, then a pause */
	STATUS_LED_IDENTIFY,     /**< fast 5 Hz blink */
	STATUS_LED_ERASE_ARMED,  /**< very fast ~10 Hz flicker */
	STATUS_LED_OFF,          /**< off */
};

/** Reflect connectivity on the status LED: steady when connected, blinking when
 *  not. Safe to call regardless of board/LED availability. */
void status_led_set_connected(bool connected);

/** Set the base pattern shown on the status LED. */
void status_led_set_mode(enum status_led_mode mode);

/** Show @p mode for @p ms, then return to the base pattern. */
void status_led_flash(enum status_led_mode mode, uint32_t ms);

/** @return true when the board has a usable status LED. */
bool status_led_present(void);

#endif /* APP_STATUS_LED_H_ */
