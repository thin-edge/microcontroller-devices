/* SPDX-License-Identifier: Apache-2.0
 *
 * OPC-UA server lifecycle. The server runs in its own thread; it builds the
 * address space from the data source and serves read/browse requests until
 * stopped.
 */
#ifndef APP_OPCUA_SERVER_H_
#define APP_OPCUA_SERVER_H_

/**
 * Start the OPC-UA server. Spawns the server thread which sets up the address
 * space, binds the configured opc.tcp port and runs the event loop.
 *
 * @return 0 on success, negative errno on failure.
 */
int opcua_server_start(void);

/** Request the OPC-UA server to stop and join its thread. */
void opcua_server_stop(void);

#endif /* APP_OPCUA_SERVER_H_ */
