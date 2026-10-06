/* SPDX-License-Identifier: Apache-2.0
 *
 * Identify (CONFIG_APP_IDENTIFY): matching a physical board with its cloud
 * device, both ways.
 *
 * - Cloud to device: the shell command `tedge identify [seconds]`, which the
 *   cloud runs through c8y_Command when it is on the shell command's
 *   allow-list, blinks the status LED in the identify pattern. Registered by
 *   identify.c itself; nothing to call.
 * - Device to cloud: the sw0 identify pattern (button.c) calls
 *   app_identify_gesture(), which sends a zephyr_Identify event.
 */
#ifndef APP_IDENTIFY_H_
#define APP_IDENTIFY_H_

/** The sw0 identify pattern was recognized: acknowledge it on the LED and
 *  send a zephyr_Identify event (at most one per 5 s). Never blocks on the
 *  network. */
void app_identify_gesture(void);

#endif /* APP_IDENTIFY_H_ */
