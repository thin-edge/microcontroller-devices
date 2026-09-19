/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Internals shared inside the module. Not a public API.
 *
 * The core (tedge_core.c) owns the client thread, the state machine and the
 * module heap. A transport (today only tedge_c8y.c) owns the cloud session
 * and is driven from that thread: connect(), then poll() in a loop.
 */

#ifndef TEDGE_INTERNAL_H_
#define TEDGE_INTERNAL_H_

#include <tedge/tedge.h>

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Resolved identity: the application's values with the defaults applied. */
struct tedge_id {
	char external_id[64];
	char name[64];
	char type[48];
	char firmware_name[48];
	char firmware_version[24];
};

/** Transport operations, called only from the client thread. */
struct tedge_transport {
	/** Open the session (TLS, protocol connect, subscriptions). */
	int (*connect)(void);
	/** Service the session for up to @p timeout_ms. 0, or a negative
	 *  errno when the session is gone and the core should reconnect. */
	int (*poll)(int timeout_ms);
	/** Close the session cleanly, if one is open. */
	void (*disconnect)(void);
	/** Publish or refresh one twin fragment. */
	int (*publish_twin)(const char *fragment, const char *json);
};

const struct tedge_transport *tedge_transport_get(void);

/* --- Services the core provides to the transport ------------------------- */

const struct tedge_id *tedge_identity(void);
const struct tedge_hooks *tedge_hook_table(void);
void tedge_set_state(enum tedge_state state);

/** The module's bounded heap (CONFIG_TEDGE_HEAP_SIZE). */
void *tedge_alloc(size_t size);
void tedge_free(void *p);

/** Cumulocity tenant host: settings override, else CONFIG_TEDGE_C8Y_URL. */
const char *tedge_c8y_host(void);
int tedge_c8y_host_set(const char *host);

/** Republish every stored twin fragment (after a reconnect). */
void tedge_twin_republish(void);

/**
 * Reset the device: the application's hook if it has one, else the
 * platform's full-system reset. Does not return.
 */
FUNC_NORETURN void tedge_platform_reset(void);

/** "<CONFIG_TEDGE_DEVICE_ID_PREFIX>-<MAC>" into @p buf. */
int tedge_platform_default_id(char *buf, size_t len);

/** True once the realtime clock is plausible (set by SNTP or the app). */
bool tedge_time_is_valid(void);
/** Set the clock with SNTP unless it is already valid. */
int tedge_time_sync(void);

/* --- SmartREST helpers (tedge_smartrest.c) ------------------------------- */

/** Copy field @p index of a SmartREST line into @p out. Length, or -ENOENT. */
int tedge_sr_field(const char *line, int index, char *out, size_t len);
/** The template number (first field), or a negative errno. */
int tedge_sr_template(const char *line);
/** Quote @p in for use as a SmartREST field (adds the surrounding quotes). */
int tedge_sr_quote(const char *in, char *out, size_t len);

/* --- Helpers (unit-tested on native_sim) --------------------------------- */

/** Seconds to wait before the next connect attempt after @p current. */
uint32_t tedge_backoff_next(uint32_t current, uint32_t max);

/**
 * Take the first certificate out of a base64 PKCS#7 certs-only structure.
 * @p clean and @p der are scratch buffers the caller owns.
 */
int tedge_pkcs7_first_cert(const char *b64, char *clean, size_t clean_cap,
			   uint8_t *der, size_t der_cap, uint8_t *out,
			   size_t cap, size_t *out_len);

/* --- Transport internals ------------------------------------------------- */

/** Publish one SmartREST line on s/us (QoS 1). */
int tedge_c8y_publish_sr(const char *line);
/** The latest JWT from s/dat, or an empty string. */
const char *tedge_c8y_jwt(void);

/* --- Remote access (tedge_remote_access.c) ------------------------------- */

enum tedge_ra_event_type {
	TEDGE_RA_UP,     /* the tunnel is up: report the operation successful */
	TEDGE_RA_FAILED, /* it never came up: report it failed, with the text */
	TEDGE_RA_CLOSED, /* an established tunnel ended: publish the event */
};

struct tedge_ra_event {
	enum tedge_ra_event_type type;
	char text[144];
};

/** True when "<host>:<port>" is one of the comma-separated @p list entries. */
bool tedge_ra_in_allow_list(const char *list, const char *host, uint16_t port);

/** Handle a "530,..." line: start a session, or fail with @p reason. */
int tedge_ra_request(const char *line, char *reason, size_t rlen);
/** Next result from a bridge thread; 0 when @p ev was filled. */
int tedge_ra_poll_event(struct tedge_ra_event *ev);
/** The tedge_RemoteAccess twin value as JSON. */
int tedge_ra_twin(char *buf, size_t len);

/* --- Onboarding (tedge_enroll.c / tedge_bootstrap.c) --------------------- */

/**
 * Make sure the device can authenticate: enroll with the Cumulocity CA and
 * register the TLS credentials, or fetch bootstrap credentials. Called from
 * the client thread before every connect; cheap once it has succeeded.
 *
 * Writes the external ID (which enrollment may decide) to @p id_out.
 */
int tedge_auth_prepare(char *id_out, size_t id_len);

/** Basic-auth user and password for the MQTT client, or NULL for mutual TLS. */
const char *tedge_auth_username(void);
const char *tedge_auth_password(void);

/* --- Settings keys (all under the module's "tedge/" subtree) ------------- */

#define TEDGE_SETTINGS_ROOT      "tedge"
#define TEDGE_KEY_C8Y_URL        TEDGE_SETTINGS_ROOT "/c8y/url"
#define TEDGE_KEY_ENROLL_OTP     TEDGE_SETTINGS_ROOT "/enroll/otp"
#define TEDGE_KEY_ENROLL_CERT    TEDGE_SETTINGS_ROOT "/enroll/cert"
#define TEDGE_KEY_BOOTSTRAP_USER TEDGE_SETTINGS_ROOT "/bootstrap/user"
#define TEDGE_KEY_BOOTSTRAP_PASS TEDGE_SETTINGS_ROOT "/bootstrap/pass"
#define TEDGE_KEY_RESTART        TEDGE_SETTINGS_ROOT "/restart"

/* --- TLS credential tags ------------------------------------------------- */

#define TEDGE_TAG_SERVER_CA (CONFIG_TEDGE_TLS_TAG_BASE + 0)
#define TEDGE_TAG_DEVICE    (CONFIG_TEDGE_TLS_TAG_BASE + 1)

#endif /* TEDGE_INTERNAL_H_ */
