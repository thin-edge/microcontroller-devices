/* SPDX-License-Identifier: Apache-2.0
 *
 * Switch/router MIB view: the system group + a MIB-II interfaces group
 * (ifNumber + ifTable), mapped from the shared data model (identity/net/
 * sim_switch in lib/common). The MIB is materialised at init as a single
 * lexicographically-sorted array of leaf descriptors, so GET is an exact search
 * and GETNEXT/GETBULK are "the next entry in the array" — the ordering SNMP
 * walks depend on falls out of the sort for free.
 */
#ifndef APP_SNMP_MIB_H_
#define APP_SNMP_MIB_H_

#include "snmp_ber.h"

#include <stddef.h>
#include <stdint.h>

/* A single readable MIB object instance (scalar .0 or ifTable cell). */
struct mib_leaf {
	uint32_t oid[BER_MAX_OID_LEN];
	uint8_t oid_len;
	uint16_t kind; /* internal value selector (see snmp_mib.c) */
	uint8_t row;   /* 1-based ifIndex for table cells; 0 for scalars */
};

/** Build the sorted MIB view for the current simulation and reset sysUpTime. */
void mib_init(void);

/** @return number of leaves in the MIB view. */
size_t mib_leaf_count(void);

/** @return the leaf exactly matching (oid,len), or NULL (for GET). */
const struct mib_leaf *mib_get_exact(const uint32_t *oid, size_t len);

/**
 * @return the lexicographically first leaf whose OID is strictly greater than
 *         (oid,len), or NULL if none (end of MIB) — for GETNEXT/GETBULK.
 */
const struct mib_leaf *mib_get_next(const uint32_t *oid, size_t len);

/** Encode `leaf`'s current value as a BER TLV into the (backward) encoder. */
void mib_put_value(struct ber_enc *e, const struct mib_leaf *leaf);

/** @return sysUpTime in TimeTicks (hundredths of a second since mib_init). */
uint32_t mib_sys_uptime(void);

#endif /* APP_SNMP_MIB_H_ */
