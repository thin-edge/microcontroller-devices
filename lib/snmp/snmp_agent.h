/* SPDX-License-Identifier: Apache-2.0
 *
 * SNMPv2c agent frontend. Serves the shared switch/router data model (lib/common)
 * over SNMP: answers GET/GETNEXT/GETBULK on CONFIG_APP_SNMP_PORT and, when
 * CONFIG_APP_SNMP_TRAP is set, originates coldStart/linkUp/linkDown traps.
 * Conforms to the protocol-frontend contract (see lib/common/README.md).
 */
#ifndef APP_SNMP_AGENT_H_
#define APP_SNMP_AGENT_H_

/**
 * Start the SNMP agent. Initialises the data model/simulation, binds the agent
 * UDP socket and spawns its listener thread, and (if traps are enabled) starts
 * the trap watcher and emits a coldStart. Call once the network is up.
 *
 * @return 0 on success, negative errno on failure.
 */
int snmp_agent_start(void);

#endif /* APP_SNMP_AGENT_H_ */
