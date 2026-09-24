/* SPDX-License-Identifier: Apache-2.0 */

#include "controls.h"

#include <zephyr/kernel.h>

static int32_t g_setpoint = CONFIG_APP_SETPOINT_DEFAULT;
static bool g_running = IS_ENABLED(CONFIG_APP_RUNNING_DEFAULT);
static int g_mode = CONFIG_APP_MODE_DEFAULT;
static bool g_local_writes = true;

int32_t app_control_setpoint(void)
{
	return g_setpoint;
}

int32_t app_control_set_setpoint(int32_t value)
{
	int32_t clamped = value;

	if (clamped < CONFIG_APP_SETPOINT_MIN) {
		clamped = CONFIG_APP_SETPOINT_MIN;
	}
	if (clamped > CONFIG_APP_SETPOINT_MAX) {
		clamped = CONFIG_APP_SETPOINT_MAX;
	}
	g_setpoint = clamped;
	return clamped;
}

bool app_control_running(void)
{
	return g_running;
}

void app_control_set_running(bool value)
{
	g_running = value;
}

int app_control_mode(void)
{
	return g_mode;
}

int app_control_set_mode(int value)
{
	if (value < 0) {
		value = 0;
	}
	if (value > 2) {
		value = 2;
	}
	g_mode = value;
	return g_mode;
}

bool app_control_local_writes(void)
{
	return g_local_writes;
}

void app_control_set_local_writes(bool allowed)
{
	g_local_writes = allowed;
}
