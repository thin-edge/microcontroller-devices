/* SPDX-License-Identifier: Apache-2.0
 *
 * Applying a ZTP bundle: module dispatch by type, and the wifi.v2 and c8y.v2
 * appliers. Unknown module types are skipped, as the wire format requires.
 *
 * Applying is two-phase so a bundle is all-or-nothing up to the first
 * side effect. Every module is unsealed, parsed and validated ("staged")
 * before anything is written; then the staged appliers commit in table
 * order — Wi-Fi first, tested by joining before it is stored — and the first
 * failed commit stops the rest. A bundle naming an unreachable network
 * therefore leaves no Cumulocity data behind either.
 */

#ifndef ZTP_APPLY_H_
#define ZTP_APPLY_H_

#include <stddef.h>

/**
 * @brief Apply a decoded manifest.
 *
 * @param manifest   the manifest text (base64-decoded manifest.payload)
 * @param device_id  this device's ZTP ID; the manifest must be for it
 * @return 0 when every staged module committed; -EPROTO for a malformed or
 *         foreign manifest, -EBADMSG for a sealed module that does not open,
 *         -ENODATA when nothing in the bundle applied, or the failing
 *         applier's error.
 */
int ztp_apply_manifest(const char *manifest, size_t len, const char *device_id);

#endif /* ZTP_APPLY_H_ */
