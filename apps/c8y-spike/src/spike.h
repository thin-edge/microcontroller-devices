/* SPDX-License-Identifier: Apache-2.0
 *
 * Spike entry points (c8y-direct-spikes). Throwaway measurement code.
 */
#ifndef SPIKE_H_
#define SPIKE_H_

#include <stddef.h>

/**
 * Set the realtime clock from SNTP. Blocks, retrying, until it succeeds:
 * certificates can't be validated without the time (task 3.1).
 */
void spike_time_sync(void);

/** Start Spike A: the MQTTS thread to Cumulocity (tasks 3.2-3.8). */
void spike_mqtt_start(void);

/** TLS credential tag of the server trust anchor (added by spike_mqtt.c). */
#define SPIKE_TAG_SERVER_CA 0x5A10
/** TLS credential tag of the device certificate and key. */
#define SPIKE_TAG_DEVICE 0x5A11

/** The latest JWT from s/dat, or an empty string. */
const char *spike_mqtt_jwt(void);

/**
 * Spike C: make sure the device has a key and a Cumulocity CA certificate
 * (enrolling if needed, which blocks until an operator registers it), and
 * register them as TLS credentials. Writes the external ID to @p id_out.
 */
int spike_enroll_run(char *id_out, size_t id_len);

/**
 * Spike B: download @p url into slot1 (HTTP or HTTPS, following redirects).
 * @p bearer is a JWT sent to the first host only, or NULL.
 */
int spike_ota_download(const char *url, const char *bearer);

/** Request a test boot of slot1 and do a full-system reset. */
int spike_ota_request_test_and_reboot(void);

/** Log the running image's version and whether it is confirmed. */
void spike_ota_log_boot(void);

/** Write the running image's "major.minor.revision" into @p buf. */
int spike_ota_running_version(char *buf, size_t len);

/* Spike F: remote access. */
enum spike_ra_event_type {
	SPIKE_RA_UP,     /* tunnel established: report the operation SUCCESSFUL */
	SPIKE_RA_FAILED, /* before the tunnel was up: report FAILED with text */
	SPIKE_RA_CLOSED, /* an established tunnel ended: publish an event */
};

struct spike_ra_event {
	enum spike_ra_event_type type;
	char text[128];
};

/**
 * Handle a "530,<serial>,<host>,<port>,<key>" message: start the bridge
 * thread, or return a negative errno with @p reason (malformed, busy).
 */
int spike_ra_request(const char *msg, char *reason, size_t rlen);

/**
 * Task 6.9: the tedge_RemoteAccess twin value (session limit, active and free
 * sessions, target policy, and with CONFIG_SPIKE_RA_TWIN_SESSIONS the open
 * session's target and start time) as JSON.
 */
int spike_ra_twin(char *buf, size_t len);

/** Next result from the bridge thread; 0 if @p ev was filled. */
int spike_ra_poll_event(struct spike_ra_event *ev);

#endif /* SPIKE_H_ */
