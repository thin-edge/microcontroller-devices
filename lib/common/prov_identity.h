/* SPDX-License-Identifier: Apache-2.0
 *
 * Application identity record: the application's unique hostname, DNS-SD
 * service type and port, kept in settings (`storage_partition`) so the Wi-Fi
 * provisioner, which is shared by every application, can advertise under the
 * application's name and report its service URL after provisioning.
 */
#ifndef APP_PROV_IDENTITY_H_
#define APP_PROV_IDENTITY_H_

#include <stdint.h>

struct prov_identity {
	char hostname[33];
	char service[24]; /* DNS-SD service type, e.g. "_modbus" */
	uint16_t port;
};

/** @return 0 with @p out filled, or -ENOENT when no record is stored. */
int prov_identity_load(struct prov_identity *out);

/** Store @p id unless the stored record already equals it. */
int prov_identity_store(const struct prov_identity *id);

#endif /* APP_PROV_IDENTITY_H_ */
