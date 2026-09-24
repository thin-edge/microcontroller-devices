/* SPDX-License-Identifier: Apache-2.0
 *
 * The client's own tunables, offered to the cloud as a parameter set.
 *
 * Everything else in this module is configured at build time, which is the
 * right default for a microcontroller: an option that cannot change cannot
 * surprise anyone, and it costs no flash. But a few of those choices are
 * ones an operator wants to revisit on a device that is already in the
 * field and misbehaving — and reflashing a fleet to turn up a log level is
 * not a plan.
 *
 * These belong to the client rather than to an application, so every
 * application that builds the client gets them without writing a line. The
 * set is named "tedge"; an application's own parameters go in a set of its
 * own, and the two are seen and changed separately.
 *
 * Only values that are read where they are used can go here — nothing that
 * sizes a buffer, a stack or a thread, because those are fixed once the
 * image is linked. Each one below names the place it takes effect.
 */

#include "tedge_internal.h"
#include <tedge/tedge.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define SELF_SET "zephyr_tedge"
/* remote_access is a set of its own, declared only when remote access is
 * built. The Parameters tab sends a whole set on every change, and a device
 * refuses a name it does not declare, so a remote_access field in the main
 * set made every change fail on an image without remote access (seen on the
 * WROOM and the ESP32-CAM, 2026-09-23). Each set's definition now lists
 * exactly what every device declaring the set accepts. */
#define RA_SET "zephyr_tedge_remote_access"

/* Zephyr's severities, lowest first, so the index is the level. */
static const char *const log_levels[] = { "off", "err", "wrn", "inf", "dbg" };

/* The build-time log level is the declared default, so a device starts
 * where its image says and only moves if someone asks it to. */
#define DEFAULT_LOG_LEVEL log_levels[MIN(CONFIG_TEDGE_LOG_LEVEL,             \
					 (int)ARRAY_SIZE(log_levels) - 1)]

static const struct tedge_parameter self_params[] = {
	TEDGE_PARAM_ENUM("log_level", DEFAULT_LOG_LEVEL,
			 ("off", "err", "wrn", "inf", "dbg"),
			 "How much the client logs. Raise it on a device that "
			 "is misbehaving, lower it when you are done."),
#if defined(CONFIG_TEDGE_HEALTH)
	TEDGE_PARAM_INT("health_interval_s", CONFIG_TEDGE_HEALTH_INTERVAL_S, 0,
			86400,
			"Seconds between the client's own health "
			"measurements. 0 stops them."),
#endif
#if defined(CONFIG_TEDGE_TRANSPORT_C8Y)
	TEDGE_PARAM_INT("required_interval_min",
			CONFIG_TEDGE_REQUIRED_INTERVAL_MIN, 0, 1440,
			"Minutes of silence after which Cumulocity calls this "
			"device offline. 0 disables the check. Cumulocity only "
			"accepts this from the device while it has no "
			"availability set, so on a device that already has "
			"one, change it in the cloud instead."),
#endif
};

#if defined(CONFIG_TEDGE_REMOTE_ACCESS)
static const struct tedge_parameter ra_params[] = {
	TEDGE_PARAM_BOOL("remote_access", true,
			 "Whether the cloud may open a tunnel to this device. "
			 "Turning it off refuses every tunnel until it is "
			 "turned back on."),
};
#endif

/* ------------------------------------------------------------------------ */
/* What the rest of the client reads                                         */
/*                                                                           */
/* Each of these is consulted where the value is used, never cached at       */
/* startup, so a change takes effect on the next use without anything having */
/* to be told about it.                                                      */
/* ------------------------------------------------------------------------ */

#if defined(CONFIG_TEDGE_HEALTH)
int tedge_self_health_interval_s(void)
{
	int32_t seconds;

	if (tedge_parameter_get_int(SELF_SET, "health_interval_s", &seconds) ==
	    0) {
		return (int)seconds;
	}
	return CONFIG_TEDGE_HEALTH_INTERVAL_S;
}
#endif

#if defined(CONFIG_TEDGE_TRANSPORT_C8Y)
int tedge_self_required_interval_min(void)
{
	int32_t minutes;

	if (tedge_parameter_get_int(SELF_SET, "required_interval_min",
				    &minutes) == 0) {
		return (int)minutes;
	}
	return CONFIG_TEDGE_REQUIRED_INTERVAL_MIN;
}
#endif

#if defined(CONFIG_TEDGE_REMOTE_ACCESS)
bool tedge_self_remote_access_allowed(void)
{
	bool allowed;

	if (tedge_parameter_get_bool(RA_SET, "remote_access", &allowed) == 0) {
		return allowed;
	}
	return true;
}
#endif

/* ------------------------------------------------------------------------ */
/* Applying a change                                                         */
/* ------------------------------------------------------------------------ */

/* The log level is the one value that has to be pushed somewhere rather
 * than read where it is used, because the filter lives in the logging
 * subsystem. Without CONFIG_LOG_RUNTIME_FILTERING there is nothing to push
 * it into, and the parameter is still reported and stored — it simply takes
 * effect at the next boot, which the help text says. */
static void apply_log_level(void)
{
#if defined(CONFIG_LOG_RUNTIME_FILTERING)
	char level[8];
	int source;

	if (tedge_parameter_get_string(SELF_SET, "log_level", level,
				       sizeof(level)) != 0) {
		return;
	}
	source = log_source_id_get(STRINGIFY(LOG_MODULE_NAME));
	if (source < 0) {
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(log_levels); i++) {
		if (strcmp(log_levels[i], level) == 0) {
			(void)log_filter_set(NULL, 0, (uint32_t)source,
					     (uint32_t)i);
			return;
		}
	}
#endif
}

static int on_change(const char *set, char *reason, size_t reason_len,
		     void *user_data)
{
	ARG_UNUSED(set);
	ARG_UNUSED(reason);
	ARG_UNUSED(reason_len);
	ARG_UNUSED(user_data);

	/* Nothing here can be refused: every value was already checked
	 * against the declaration, and none of them can contradict another. */
	apply_log_level();
#if defined(CONFIG_TEDGE_TRANSPORT_C8Y)
	/* Tell Cumulocity the new window rather than waiting for the next
	 * connect.
	 *
	 * Template 117 *creates* c8y_RequiredAvailability; it does not
	 * update one that already exists. So this lands on a device that has
	 * never had an availability set, and is quietly ignored afterwards —
	 * the parameter still reports and stores the operator's value, and
	 * the device is still judged by whatever the cloud holds. Changing
	 * the window on a device already in the fleet is a cloud-side edit.
	 * The parameter's help text says so. */
	{
		char line[32];
		int minutes = tedge_self_required_interval_min();

		if (minutes > 0) {
			snprintf(line, sizeof(line), "117,%d", minutes);
			(void)tedge_c8y_publish_sr(line);
		}
	}
#endif
	return 0;
}

void tedge_self_params_declare(void)
{
	int rc = tedge_declare_parameters(SELF_SET, self_params,
					  ARRAY_SIZE(self_params), on_change,
					  NULL);

	if (rc != 0) {
		LOG_WRN("the client's own parameters were not declared (%d)",
			rc);
		return;
	}
	/* A level stored before the last reboot has to be put back now. */
	apply_log_level();
#if defined(CONFIG_TEDGE_REMOTE_ACCESS)
	/* Read where it is used (tedge_self_remote_access_allowed()), so
	 * there is nothing to apply on a change. */
	rc = tedge_declare_parameters(RA_SET, ra_params, ARRAY_SIZE(ra_params),
				      NULL, NULL);
	if (rc != 0) {
		LOG_WRN("the '%s' parameters were not declared (%d)", RA_SET,
			rc);
	}
#endif
}
