/* SPDX-License-Identifier: Apache-2.0
 *
 * Protocol-frontend TEMPLATE. Copy to lib/<protocol>/ and rename. Demonstrates
 * the contract from lib/common/README.md: a frontend exposes a
 * `<protocol>_frontend_start()` lifecycle entry point that the application calls
 * once connectivity is up, and maps the shared device data model
 * (data_source / controls / identity from lib/common) onto its wire protocol.
 */
#ifndef APP_TEMPLATE_FRONTEND_H_
#define APP_TEMPLATE_FRONTEND_H_

/**
 * Start the protocol frontend. Called from the application's main() after the
 * network is connected.
 *
 * @return 0 on success, negative errno on failure.
 */
int template_frontend_start(void);

#endif /* APP_TEMPLATE_FRONTEND_H_ */
