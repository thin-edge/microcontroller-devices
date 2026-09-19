/* SPDX-License-Identifier: Apache-2.0
 *
 * Spike entry points (c8y-direct-spikes). Throwaway measurement code.
 */
#ifndef SPIKE_H_
#define SPIKE_H_

/**
 * Set the realtime clock from SNTP. Blocks, retrying, until it succeeds:
 * certificates can't be validated without the time (task 3.1).
 */
void spike_time_sync(void);

/** Start Spike A: the MQTTS thread to Cumulocity (tasks 3.2-3.8). */
void spike_mqtt_start(void);

#endif /* SPIKE_H_ */
