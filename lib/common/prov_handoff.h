/* SPDX-License-Identifier: Apache-2.0
 *
 * Application side of Wi-Fi provisioning (CONFIG_APP_PROV_HANDOFF). The
 * application carries no Bluetooth: provisioning runs in its own image, the
 * Wi-Fi provisioner, which MCUboot launches from `prov_partition` when the
 * boot request asks for it (see boot_request.h). This module decides when to
 * hand over: at boot with no credentials, and on the sw0 gestures.
 */
#ifndef APP_PROV_HANDOFF_H_
#define APP_PROV_HANDOFF_H_

/**
 * No credentials resolve: request the provisioner and reboot into it. Returns
 * only when no provisioner is flashed (the device then stays offline, as a
 * build without provisioning would).
 */
void app_prov_handoff_no_credentials(void);

/**
 * Watch sw0 for the provisioning pattern (reboot into the provisioner, keep
 * the credentials) and the erase hold (erase them, then reboot into the
 * provisioner). A no-op on boards without an sw0 alias.
 */
void app_prov_button_start(void);

/** Record this application's hostname, service type and port for the
 *  provisioner (written only when they changed). Call once connected. */
void app_prov_identity_update(void);

#endif /* APP_PROV_HANDOFF_H_ */
