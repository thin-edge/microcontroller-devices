/* SPDX-License-Identifier: Apache-2.0
 *
 * SNMPv2c trap originator. Sends coldStart on startup and linkUp/linkDown as the
 * switch simulation's interfaces change operational state, to the manager
 * configured by CONFIG_APP_SNMP_TRAP_*. Compiled only when CONFIG_APP_SNMP_TRAP.
 */
#ifndef APP_SNMP_TRAP_H_
#define APP_SNMP_TRAP_H_

/**
 * Start trap generation: open the trap socket, emit one coldStart, snapshot the
 * current interface states and spawn the watcher that emits linkUp/linkDown on
 * subsequent transitions. Call once, after the network is up.
 */
void snmp_trap_start(void);

#endif /* APP_SNMP_TRAP_H_ */
