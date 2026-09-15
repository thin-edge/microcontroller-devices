/* SPDX-License-Identifier: Apache-2.0
 *
 * Writable control points, protocol-independent. The shared device data model
 * owns this state (in RAM, not persisted); protocol frontends map their
 * writable nodes/registers onto it, and the firmware can act on it. Range
 * clamping for the setpoint is enforced here so every frontend behaves
 * identically.
 */
#ifndef APP_CONTROLS_H_
#define APP_CONTROLS_H_

#include <stdbool.h>
#include <stdint.h>

/** @return the current Setpoint value. */
int32_t app_control_setpoint(void);

/**
 * Set the Setpoint, clamping to [CONFIG_APP_SETPOINT_MIN, MAX].
 *
 * @param value requested value.
 * @return the stored (possibly clamped) value.
 */
int32_t app_control_set_setpoint(int32_t value);

/** @return the current Running flag. */
bool app_control_running(void);

/** Set the Running flag. */
void app_control_set_running(bool value);

/** @return the current mode (0=off, 1=auto, 2=manual). */
int app_control_mode(void);

/**
 * Set the mode, clamping to [0, 2].
 *
 * @param value requested mode.
 * @return the stored (clamped) mode.
 */
int app_control_set_mode(int value);

#endif /* APP_CONTROLS_H_ */
