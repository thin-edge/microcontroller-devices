/* SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP server frontend. Serves the shared device data model over TCP
 * (default port 502): input/holding registers, coils, discrete inputs. Conforms
 * to the protocol-frontend contract (see lib/common/README.md).
 */
#ifndef APP_MODBUS_SERVER_H_
#define APP_MODBUS_SERVER_H_

/**
 * Start the Modbus TCP server. Initialises the raw-ADU Modbus server and spawns
 * a listener thread on CONFIG_APP_MODBUS_PORT. Call once the network is up.
 *
 * @return 0 on success, negative errno on failure.
 */
int modbus_server_start(void);

#endif /* APP_MODBUS_SERVER_H_ */
