/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TEDGE_GLUE_H_
#define TEDGE_GLUE_H_

/** Start the thin-edge.io client with this application's identity and hooks. */
int tedge_glue_start(void);

#if defined(CONFIG_TEDGE_TELEMETRY)
/** Begin publishing the server's measurements. Call after the client. */
void tedge_glue_start_telemetry(void);
#endif

#endif /* TEDGE_GLUE_H_ */
