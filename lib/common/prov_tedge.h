/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APP_PROV_TEDGE_H_
#define APP_PROV_TEDGE_H_

#include <stdbool.h>

#if defined(CONFIG_APP_TEDGE_PROVISIONING)

/** Start tedge-onboard support: call after tedge_init().
 *  @return 0, or a negative errno. */
int app_prov_tedge_init(void);

/**
 * At start-up: when the device has no Wi-Fi network yet, start the client at
 * once with @p client_start, since its provisioning library is how the
 * device gets one, and return true. The application then waits for the
 * network as long as that takes, and does not start the client again.
 */
bool app_prov_tedge_start_unprovisioned(int (*client_start)(void));

#else

/* A macro, so builds without the client need no client_start symbol. */
#define app_prov_tedge_start_unprovisioned(client_start) false

#endif

#endif /* APP_PROV_TEDGE_H_ */
