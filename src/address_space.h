/* SPDX-License-Identifier: Apache-2.0
 *
 * Builds and maintains the OPC-UA address space from the data source. Kept
 * separate from server lifecycle so the node layout is easy to reason about.
 */
#ifndef APP_ADDRESS_SPACE_H_
#define APP_ADDRESS_SPACE_H_

#include <open62541.h>

/**
 * Populate the server's address space: a Device object carrying identity
 * metadata plus one Variable node per data-source measurement.
 *
 * @param server the OPC-UA server to add nodes to.
 * @return UA_STATUSCODE_GOOD on success, an error status otherwise.
 */
UA_StatusCode address_space_setup(UA_Server *server);

/**
 * Sample every measurement from the data source and write the current values
 * into their variable nodes. Intended to be called periodically from the
 * server's own context (e.g. a repeated callback).
 *
 * @param server the OPC-UA server whose nodes should be updated.
 */
void address_space_update(UA_Server *server);

/** @return the current value of the writable `setpoint` control node. */
double address_space_setpoint(void);

/** @return the current value of the writable `enabled` control node. */
bool address_space_enabled(void);

#endif /* APP_ADDRESS_SPACE_H_ */
