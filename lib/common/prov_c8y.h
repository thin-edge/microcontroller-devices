/* SPDX-License-Identifier: Apache-2.0
 *
 * Cumulocity onboarding data handed from the provisioner image to the
 * application, through the shared settings store: `prov/c8y/url`,
 * `prov/c8y/tenant`, `prov/c8y/external_id` and `prov/c8y/otp`.
 *
 * The provisioner never writes the tedge-zephyr module's own `tedge/`
 * subtree, and does not link the module at all: the application reads these
 * keys and passes them to the module through its public API.
 */

#ifndef APP_PROV_C8Y_H_
#define APP_PROV_C8Y_H_

#include <stdbool.h>
#include <stddef.h>

struct prov_c8y {
	char url[128];        /* as the ZTP server sent it; may carry a scheme */
	char tenant[32];
	char external_id[64]; /* the ID the one-time password was issued for */
	char otp[65];         /* tedge_set_enroll_otp() takes up to 64 */
};

/**
 * @brief Store onboarding data (provisioner). Empty fields are not written.
 *
 * @return 0, or a negative errno from settings.
 */
int prov_c8y_store(const struct prov_c8y *c);

/**
 * @brief Load onboarding data (application).
 *
 * @return 0 when at least the URL or the one-time password is present,
 *         -ENOENT when nothing is stored.
 */
int prov_c8y_load(struct prov_c8y *out);

/**
 * @brief Delete the onboarding data the client has taken over: URL, tenant
 * and one-time password.
 *
 * The external ID is kept. It is not consumed but becomes the device's
 * identity for good — the Cumulocity certificate is issued for it — so
 * app_identity_device_id() keeps returning it on every later boot.
 */
void prov_c8y_clear(void);

/**
 * @brief The external ID a ZTP server issued, if any.
 *
 * @return 0 with @p out filled, -ENOENT when the device was not onboarded
 *         through ZTP.
 */
int prov_c8y_external_id(char *out, size_t size);

/**
 * @brief Hand ZTP onboarding data to the tedge client (application).
 *
 * Call after tedge_init() and before tedge_start(). Passes the tenant host
 * and the one-time password through the client's public API, then deletes
 * them. Does nothing when there is no data. On an error the data is kept, so
 * the next boot tries again rather than losing the password.
 *
 * @return 0, or the first error from the client.
 */
int prov_c8y_handoff(void);

#endif /* APP_PROV_C8Y_H_ */
