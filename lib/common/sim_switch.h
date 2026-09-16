/* SPDX-License-Identifier: Apache-2.0
 *
 * Switch/router simulation — typed interface accessors (CONFIG_APP_SIM_SWITCH).
 *
 * This is part of the shared device data model in lib/common: it models a small
 * managed switch/router as a fixed set of network interfaces, each with an
 * operational/administrative status, a nominal speed, and monotonic traffic
 * counters. A protocol frontend (e.g. lib/snmp) reads this structured state and
 * maps it onto its wire protocol's interface table; it MUST NOT own the state
 * itself. State is stepped on CONFIG_APP_SAMPLE_INTERVAL_MS via the standard
 * data_source_init()/data_source.h entry points.
 *
 * Interfaces are addressed 0-based here; a frontend that needs a 1-based index
 * (SNMP ifIndex) adds one.
 */
#ifndef APP_SIM_SWITCH_H_
#define APP_SIM_SWITCH_H_

#include <stddef.h>
#include <stdint.h>

/** Upper bound on simulated interfaces (bounds all fixed buffers). */
#define SIM_SWITCH_MAX_IF 8

/** Interface operational/administrative status (matches SNMP ifOperStatus). */
enum sim_if_status {
	SIM_IF_UP = 1,
	SIM_IF_DOWN = 2,
};

/** Per-interface monotonic counters (Counter32 semantics — wrap at 2^32). */
enum sim_if_counter {
	SIM_IF_IN_OCTETS = 0,
	SIM_IF_IN_UCAST_PKTS,
	SIM_IF_OUT_OCTETS,
	SIM_IF_OUT_UCAST_PKTS,
	SIM_IF_COUNTER_COUNT,
};

/** @return number of simulated interfaces (== CONFIG_APP_SIM_SWITCH_IF_COUNT). */
size_t sim_switch_if_count(void);

/**
 * @param idx interface index in [0, sim_switch_if_count()).
 * @return the interface's stable description (e.g. "GigabitEthernet0/1"), or
 *         "?" if out of range. Static lifetime.
 */
const char *sim_switch_if_descr(size_t idx);

/** @return the interface's ifType (6 = ethernetCsmacd), or 0 if out of range. */
int sim_switch_if_type(size_t idx);

/** @return the interface's MTU in octets, or 0 if out of range. */
uint32_t sim_switch_if_mtu(size_t idx);

/** @return the interface's nominal speed in bits/s (Gauge32), or 0 if out of range. */
uint32_t sim_switch_if_speed(size_t idx);

/** @return the interface's administrative status, or SIM_IF_DOWN if out of range. */
enum sim_if_status sim_switch_if_admin_status(size_t idx);

/** @return the interface's operational status, or SIM_IF_DOWN if out of range. */
enum sim_if_status sim_switch_if_oper_status(size_t idx);

/**
 * @param idx interface index in [0, sim_switch_if_count()).
 * @param c which counter.
 * @return the counter's current 32-bit value (wraps), or 0 if out of range.
 */
uint32_t sim_switch_if_counter(size_t idx, enum sim_if_counter c);

#endif /* APP_SIM_SWITCH_H_ */
