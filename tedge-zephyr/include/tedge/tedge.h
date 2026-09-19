/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief tedge-zephyr public API: thin-edge.io device management for Zephyr.
 *
 * @warning UNSTABLE. This is an outline of the integration contract between
 * the client and the application that hosts it. Only tedge_version() is
 * implemented. Everything else is declared so the shape can be reviewed,
 * and it will change as the features are built. Don't depend on it yet.
 *
 * Division of responsibilities:
 * - The application owns the network interface, the device identity, what
 *   telemetry to send, and the task watchdog.
 * - The client owns its threads, a bounded heap, the settings subtree
 *   "tedge/" and TLS credential tags from CONFIG_TEDGE_TLS_TAG_BASE.
 *
 * Unless stated otherwise, functions return 0 on success or a negative errno.
 */

#ifndef TEDGE_TEDGE_H_
#define TEDGE_TEDGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Set while the API is an outline that may change without notice. */
#define TEDGE_API_UNSTABLE 1

struct sockaddr;

/** @brief The module's version (the contents of its VERSION file). */
const char *tedge_version(void);

/* ------------------------------------------------------------------------ */
/* Lifecycle and state                                                       */
/* ------------------------------------------------------------------------ */

/** @brief Client states, reported through tedge_hooks::on_state. */
enum tedge_state {
	TEDGE_STATE_STOPPED,
	/** No IPv4 address yet; waiting for the application's network. */
	TEDGE_STATE_WAITING_NETWORK,
	/** Waiting for the clock (SNTP), needed to validate certificates. */
	TEDGE_STATE_WAITING_TIME,
	/** Waiting for the operator to register the device (CA onboarding). */
	TEDGE_STATE_AWAITING_REGISTRATION,
	TEDGE_STATE_CONNECTING,
	TEDGE_STATE_CONNECTED,
	/** A firmware update is downloading or waiting for its test boot. */
	TEDGE_STATE_UPDATING,
};

/**
 * @brief Device identity. Any NULL field takes the documented default.
 */
struct tedge_identity {
	/** Default: "<CONFIG_TEDGE_DEVICE_ID_PREFIX>-<MAC address>". */
	const char *external_id;
	/** Default: the external ID. */
	const char *name;
	/** Default: CONFIG_TEDGE_DEVICE_TYPE. */
	const char *type;
	/** Firmware reported to the cloud. Default: the application's name. */
	const char *firmware_name;
	/** Default: the application's version (APP_VERSION_STRING). */
	const char *firmware_version;
};

/**
 * @brief Remote-access target, passed to tedge_hooks::remote_access_allow.
 */
struct tedge_remote_target {
	const struct sockaddr *addr;
	uint16_t port;
};

/**
 * @brief Callbacks into the application. Every member is optional.
 *
 * Hooks run on the client's thread. They must return promptly and must not
 * call back into the client.
 */
struct tedge_hooks {
	/** State changed, for example to drive a status LED. */
	void (*on_state)(enum tedge_state state, void *user_data);

	/**
	 * The cloud asked for a restart. Prepare for it and return 0 to allow
	 * it, or return a negative errno and write a reason to veto it.
	 * The client never reboots without calling this first.
	 */
	int (*restart_request)(char *reason, size_t reason_len, void *user_data);

	/**
	 * The new firmware image is running and connected. Return 0 if the
	 * application is healthy, so the image can be confirmed. Any other
	 * value leaves it unconfirmed, and MCUboot reverts it on the next reset.
	 * Requires CONFIG_TEDGE_FIRMWARE_UPDATE.
	 */
	int (*firmware_confirm_check)(void *user_data);

	/**
	 * Narrows the Kconfig remote-access target policy. Return true to
	 * allow a target that the policy already allows. Requires
	 * CONFIG_TEDGE_REMOTE_ACCESS.
	 */
	bool (*remote_access_allow)(const struct tedge_remote_target *target,
				    void *user_data);

	/** Called periodically from each client thread, for a task watchdog. */
	void (*progress)(void *user_data);

	void *user_data;
};

/**
 * @brief Initialise the client. Call once, before tedge_start().
 *
 * @param identity Device identity, or NULL for all defaults. The client
 *                 copies it.
 * @param hooks    Application callbacks, or NULL. The client keeps the
 *                 pointer, so it must stay valid.
 */
int tedge_init(const struct tedge_identity *identity,
	       const struct tedge_hooks *hooks);

/**
 * @brief Start the client thread. It waits for the network, then connects.
 *
 * It may be called before the network is up.
 */
int tedge_start(void);

/** @brief Disconnect and stop the client thread. */
int tedge_stop(void);

/** @brief Current state. */
enum tedge_state tedge_get_state(void);

/* ------------------------------------------------------------------------ */
/* Onboarding (CONFIG_TEDGE_AUTH_C8Y_CA)                                     */
/* ------------------------------------------------------------------------ */

/**
 * @brief Copy the registration URL (external ID and one-time password
 * pre-filled) into @p buf, so the application can show it (console,
 * display, provisioning result).
 *
 * @return URL length, -ENOENT if the device is already enrolled.
 */
int tedge_registration_url(char *buf, size_t len);

/* ------------------------------------------------------------------------ */
/* Telemetry (CONFIG_TEDGE_TELEMETRY)                                        */
/* ------------------------------------------------------------------------ */

/** @brief One value of a measurement. */
struct tedge_measurement_value {
	/** Series name, e.g. "temperature". */
	const char *series;
	double value;
	/** Unit, e.g. "C". May be NULL. */
	const char *unit;
};

/**
 * @brief Publish a measurement of @p type made of @p count values.
 *
 * @param timestamp_ms Unix time in ms, or 0 for "now".
 * @return 0 when queued, -ENOMEM if the client's buffer is full.
 */
int tedge_publish_measurement(const char *type,
			      const struct tedge_measurement_value *values,
			      size_t count, int64_t timestamp_ms);

/** @brief Publish an event. */
int tedge_publish_event(const char *type, const char *text,
			int64_t timestamp_ms);

enum tedge_alarm_severity {
	TEDGE_ALARM_CRITICAL,
	TEDGE_ALARM_MAJOR,
	TEDGE_ALARM_MINOR,
	TEDGE_ALARM_WARNING,
};

/** @brief Raise (or update) the alarm of @p type. */
int tedge_raise_alarm(const char *type, enum tedge_alarm_severity severity,
		      const char *text);

/** @brief Clear the alarm of @p type. */
int tedge_clear_alarm(const char *type);

/* ------------------------------------------------------------------------ */
/* Custom operations                                                         */
/* ------------------------------------------------------------------------ */

/** Opaque handle for an operation in progress. */
struct tedge_operation;

/**
 * @brief Handler for an application-defined operation.
 *
 * Called on the client's thread with the operation already marked as
 * executing. Finish it with tedge_operation_succeed() or
 * tedge_operation_fail(), now or later from another thread.
 */
typedef void (*tedge_operation_handler_t)(struct tedge_operation *op,
					  void *user_data);

/**
 * @brief Register an operation the application implements.
 *
 * The operation is advertised as supported. @p name is the cloud's operation
 * name (for Cumulocity, the fragment, e.g. "c8y_Command").
 */
int tedge_register_operation(const char *name,
			     tedge_operation_handler_t handler,
			     void *user_data);

/** @brief The operation's raw payload (SmartREST CSV or JSON). */
const char *tedge_operation_payload(const struct tedge_operation *op);

/** @brief Report success, with an optional result string. */
int tedge_operation_succeed(struct tedge_operation *op, const char *result);

/** @brief Report failure with a reason. */
int tedge_operation_fail(struct tedge_operation *op, const char *reason);

/* ------------------------------------------------------------------------ */
/* Log and configuration types (CONFIG_TEDGE_LOG_UPLOAD, CONFIG_TEDGE_CONFIG) */
/* ------------------------------------------------------------------------ */

/**
 * @brief Sink the application writes file contents into. It returns 0, or
 * a negative errno that the application must pass back to abort.
 */
typedef int (*tedge_write_fn)(void *ctx, const void *data, size_t len);

/** @brief Log request filters (0 or NULL = unset). */
struct tedge_log_request {
	int64_t date_from_ms;
	int64_t date_to_ms;
	const char *search_text;
	uint32_t max_lines;
};

/** @brief Produces a log of one type by writing it through @p write. */
typedef int (*tedge_log_reader_t)(const struct tedge_log_request *req,
				  tedge_write_fn write, void *ctx,
				  void *user_data);

/** @brief Add a log type to the ones the device offers. */
int tedge_register_log_type(const char *type, tedge_log_reader_t reader,
			    void *user_data);

/** @brief Writes the current configuration of one type (snapshot). */
typedef int (*tedge_config_reader_t)(tedge_write_fn write, void *ctx,
				     void *user_data);

/**
 * @brief Applies a new configuration of one type, delivered in chunks.
 * @p data is NULL with @p len 0 on the final call. Return a negative errno
 * to reject it.
 */
typedef int (*tedge_config_writer_t)(const void *data, size_t len,
				     void *user_data);

/** @brief Add a configuration type (snapshot and/or update). */
int tedge_register_config_type(const char *type, tedge_config_reader_t reader,
			       tedge_config_writer_t writer, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* TEDGE_TEDGE_H_ */
