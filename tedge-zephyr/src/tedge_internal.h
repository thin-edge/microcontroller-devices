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

/** Ask the client to drop this session and connect again (a new credential). */
void tedge_request_reconnect(void);

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

/* --- Downloads (tedge_http_download.c) ----------------------------------- */

/** Receives each body segment as the parser reports it. */
typedef int (*tedge_sink_fn)(const void *data, size_t len, void *user_data);

struct tedge_download {
	const char *url;
	/** Bearer token, sent to the tenant's own hosts only. May be NULL. */
	const char *token;
	tedge_sink_fn sink;
	void (*progress)(int64_t written, void *user_data);
	void *user_data;
	int timeout_ms;
	int64_t written; /* out */
	int64_t total;   /* out: Content-Length, 0 when the server omits it */
};

int tedge_download(struct tedge_download *req);

/* --- Uploads (tedge_http_upload.c) --------------------------------------- */

/** Writes the body through @p sink. Used twice per upload: once to measure
 * it, once to send it, so it should produce the same bytes both times. */
typedef int (*tedge_producer_fn)(tedge_sink_fn sink, void *sink_ctx,
				 void *user_data);

struct tedge_upload {
	const char *url;
	/** Bearer token, sent to the tenant's own hosts only. May be NULL. */
	const char *token;
	const char *content_type;
	/** Names the file in Cumulocity's UI. May be NULL. */
	const char *filename;
	/** The body, either whole in memory... */
	const char *payload;
	/** ...or written by a producer. Exactly @p length bytes are sent:
	 *  a producer that stops early is padded, one that overruns is cut. */
	tedge_producer_fn producer;
	void *user_data;
	size_t length;
	int timeout_ms;
	/** out: the Location header, when the caller wants it. */
	char *location;
	size_t location_len;
	int status; /* out */
};

int tedge_upload(struct tedge_upload *req);
/** Redirect targets can be ~1 KB (a release asset). */
#define TEDGE_URL_MAX  1152
#define TEDGE_HOST_MAX 128

/** Split "scheme://host[:port]/path"; @p path points into @p url. */
int tedge_url_split(const char *url, bool *tls, char *host, size_t host_len,
		    uint16_t *port, const char **path);
/** True when @p host is the tenant or inside its parent domain. */
bool tedge_url_is_tenant(const char *host);
/** Resolve a redirect target against the URL it came from. */
int tedge_url_resolve(const char *base, const char *location, char *out,
		      size_t len);

/* --- Log upload (tedge_log_upload.c, tedge_log_ring.c) ------------------- */

struct tedge_log_event {
	int rc;          /* 0 when the log reached the cloud */
	char url[160];   /* where it landed */
	char reason[96]; /* why it did not */
};

/** Start sending the log a "522,…" line asks for. */
int tedge_log_request(const char *line, char *reason, size_t rlen);
/** Next result from the upload thread, or -ENOMSG. */
int tedge_log_poll_event(struct tedge_log_event *ev);
/** Builds "118,<type>,…"; returns 0 when the image offers no log type. */
size_t tedge_log_types_line(char *out, size_t len);
/** Registers the client's own log; called once at startup. */
void tedge_log_upload_init(void);

/** The client's own log, kept in RAM (tedge_log_ring.c). */
void tedge_log_ring_write(const uint8_t *data, size_t len);
size_t tedge_log_ring_read(size_t offset, uint8_t *out, size_t len);
size_t tedge_log_ring_size(void);
uint32_t tedge_log_ring_dropped(void);

/* --- Crash dumps (tedge_coredump.c) -------------------------------------- */

/** Offers a stored dump as a log type; called once at startup. */
void tedge_coredump_init(void);
/** The dump reached the cloud and may be erased. */
void tedge_coredump_taken(void);

/* --- Shell command (tedge_shell_cmd.c) ----------------------------------- */

#if defined(CONFIG_TEDGE_SHELL_COMMAND)
struct tedge_shell_event {
	int rc; /* 0 when the command ran and returned success */
	char output[CONFIG_TEDGE_SHELL_COMMAND_OUTPUT_BYTES];
};

/** Start the command a "511,…" line asks for. */
int tedge_shell_request(const char *line, char *reason, size_t rlen);
/** Next result from the command thread, or -ENOMSG. */
int tedge_shell_poll_event(struct tedge_shell_event *ev);
#endif

/* Pure, and unit-tested without a board: this is the function that decides
 * what a remote party may run. */
/** True when @p cmd may run under @p list; @p why explains a refusal. */
bool tedge_shell_command_allowed(const char *list, const char *cmd,
				 const char **why);

/* --- Firmware update (tedge_firmware.c) ---------------------------------- */

enum tedge_fw_event_type {
	TEDGE_FW_REBOOTING, /* downloaded; the device is about to swap */
	TEDGE_FW_INSTALLED, /* the new image confirmed itself */
	TEDGE_FW_REVERTED,  /* MCUboot rolled it back */
	TEDGE_FW_FAILED,    /* the update failed, with the reason */
};

struct tedge_fw_event {
	enum tedge_fw_event_type type;
	char text[144];
};

/** Handle a "515,..." line: start an update, or fail with @p reason. */
int tedge_fw_request(const char *line, char *reason, size_t rlen);
/** Next result from the download thread; 0 when @p ev was filled. */
int tedge_fw_poll_event(struct tedge_fw_event *ev);
/**
 * Start the deadline by which a test-booted image must confirm itself; after
 * it, the device resets so the bootloader can roll the image back.
 */
void tedge_fw_arm_confirm_deadline(void);
/** Confirm a test boot, or report a rollback. Called once per session. */
void tedge_fw_on_connected(void);
/** The running image's version, from MCUboot's header. */
int tedge_fw_running_version(char *buf, size_t len);
/** Text for the operation while the device installs and swaps. */
const char *tedge_fw_downtime_hint(void);
/** Publish @p json on the free-form progress topic (QoS 0); no-op on Core MQTT. */
int tedge_c8y_publish_progress(const char *kind, const char *json);

/* --- Telemetry (tedge_telemetry.c, tedge_health.c) ----------------------- */

enum tedge_msg_kind {
	TEDGE_MSG_MEASUREMENT,
	TEDGE_MSG_EVENT,
	TEDGE_MSG_ALARM,
	TEDGE_MSG_ALARM_CLEAR,
};

/** Send one queued message; -ENOTCONN leaves it queued for later. */
int tedge_c8y_publish_telemetry(enum tedge_msg_kind kind, const char *type,
				const char *payload);
/** Send whatever is queued, oldest first (client thread). */
void tedge_telemetry_flush(void);
/** Wake the client thread when something is queued. */
void tedge_telemetry_wake(void);
/** How many messages the buffer has had to drop. */
uint32_t tedge_telemetry_dropped(void);
/** Publish the client's own vital signs when the interval has passed. */
void tedge_health_tick(void);
/** Bytes free in the module's heap. */
size_t tedge_heap_free(void);
/** Copy @p in into @p out with JSON's escapes applied. */
void tedge_json_escape(const char *in, char *out, size_t len);
/** Read a string field out of JSON this module built. Length, or -ENOENT. */
int tedge_json_field(const char *json, const char *key, char *out, size_t len);
/** Walk the numeric members; 1 while one was found, 0 at the end. */
int tedge_json_next_number(const char *json, size_t *pos, char *key,
			   size_t key_len, char *value, size_t value_len);

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

/* --- EST, shared by enrollment and renewal ------------------------------- */

/** Build a CSR for the device key; @p out may be NULL to keep it internal. */
int tedge_est_make_csr(char *out, size_t len);
/** POST the CSR to @p path and unwrap the certificate from the reply. */
int tedge_est_request(const char *path, const char *auth, uint8_t *out,
		      size_t cap, size_t *out_len);
/** Free the transient buffers a CSR/EST flow used. */
void tedge_est_release(void);
/** Store @p der as the device certificate and use it for TLS from now on. */
int tedge_credentials_replace(const uint8_t *der, size_t len);
/** The certificate in use, or NULL before enrollment. */
const uint8_t *tedge_credentials_cert(size_t *len);

/* --- Certificate renewal (tedge_cert_renew.c) ---------------------------- */

/** What the client should do about the certificate right now. */
enum tedge_cert_action {
	TEDGE_CERT_WAIT,  /* plenty of life left, or nothing to judge yet */
	TEDGE_CERT_RENEW, /* inside the margin: renew now */
	TEDGE_CERT_RETRY, /* a renewal failed; try again before long */
	TEDGE_CERT_ALARM, /* failing and close to expiry: tell the operator */
};

enum tedge_cert_action tedge_cert_action(int days_left, int renew_before_days,
					 int alarm_days, bool renewed);

/** Check the certificate's age and renew it when it is due. */
void tedge_cert_renew_tick(void);
/** The tedge_Certificate twin value as JSON. */
int tedge_cert_twin(char *buf, size_t len);

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
#define TEDGE_KEY_FIRMWARE       TEDGE_SETTINGS_ROOT "/firmware"
/* Separate from the marker on purpose: the marker's format must never
 * change, because the image that reads it is the one on the other side of a
 * swap, which may be older code. */
#define TEDGE_KEY_FIRMWARE_SIZE  TEDGE_SETTINGS_ROOT "/firmware_size"

/* --- TLS credential tags ------------------------------------------------- */

#define TEDGE_TAG_SERVER_CA (CONFIG_TEDGE_TLS_TAG_BASE + 0)
#define TEDGE_TAG_DEVICE    (CONFIG_TEDGE_TLS_TAG_BASE + 1)

#endif /* TEDGE_INTERNAL_H_ */
