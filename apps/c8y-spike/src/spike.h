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

#endif /* SPIKE_H_ */
