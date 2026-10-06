/* SPDX-License-Identifier: Apache-2.0
 *
 * The sw0 watcher (CONFIG_APP_BUTTON): debounces the board's sw0 button,
 * feeds the gesture classifier (button_gesture.h) and hands each recognized
 * gesture to the feature that owns it: provisioning and erase to the
 * provisioning hand-off, the identify pattern to identify.c. Gestures whose
 * feature is not built in are switched off in the classifier.
 */
#ifndef APP_BUTTON_H_
#define APP_BUTTON_H_

/** Start watching sw0. A no-op on boards without an sw0 alias, and when
 *  called again. */
void app_button_start(void);

#endif /* APP_BUTTON_H_ */
