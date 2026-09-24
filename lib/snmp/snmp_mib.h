/* SPDX-License-Identifier: Apache-2.0
 *
 * Switch/router MIB view: the system group + a MIB-II interfaces group
 * (ifNumber + ifTable), mapped from the shared data model (identity/net/
 * sim_switch in lib/common). The MIB is materialised at init as a single
 * lexicographically-sorted array of leaf descriptors, so GET is an exact search
 * and GETNEXT/GETBULK are "the next entry in the array" — the ordering SNMP
 * walks depend on falls out of the sort for free.
 *
 * A leaf does not store its OID: every object here is a constant prefix (the
 * system group, ifNumber, ifEntry or the enterprise arc) plus a column and,
 * for table cells, a row. mib_leaf_oid() derives the OID when it is encoded or
 * compared, which keeps the table at a few bytes per leaf instead of a
 * 32-arc copy each. Measured on the ESP32-CAM tedge-ota image (2026-09-22):
 * dram0 166,720 -> 143,088 B (-23,632 B, leaves 13,464 -> 198 B), the same
 * snmpwalk/snmpbulkwalk transcript before and after (tests/snmp/golden/).
 */
#ifndef APP_SNMP_MIB_H_
#define APP_SNMP_MIB_H_

#include "snmp_ber.h"

#include <stddef.h>
#include <stdint.h>

/* Longest OID mib_leaf_oid() derives (ifEntry cell: 9 prefix + column + row). */
#define MIB_MAX_OID_LEN 11

/* A single readable MIB object instance (scalar .0 or ifTable cell). */
struct mib_leaf {
	uint8_t kind; /* value selector and OID prefix (see snmp_mib.c) */
	uint8_t col;  /* the arc after the prefix: scalar number or ifTable column */
	uint8_t row;  /* 1-based ifIndex for table cells; 0 for scalars */
};

/** Build the sorted MIB view for the current simulation and reset sysUpTime. */
void mib_init(void);

/** @return number of leaves in the MIB view. */
size_t mib_leaf_count(void);

/**
 * Derive `leaf`'s OID into out[0..MIB_MAX_OID_LEN).
 * @return the number of sub-identifiers written.
 */
size_t mib_leaf_oid(const struct mib_leaf *leaf, uint32_t *out);

/** @return the leaf exactly matching (oid,len), or NULL (for GET). */
const struct mib_leaf *mib_get_exact(const uint32_t *oid, size_t len);

/**
 * @return the lexicographically first leaf whose OID is strictly greater than
 *         (oid,len), or NULL if none (end of MIB) — for GETNEXT/GETBULK.
 */
const struct mib_leaf *mib_get_next(const uint32_t *oid, size_t len);

/**
 * @return the leaf after `leaf` in walk order, or NULL at the end of the MIB.
 *         Equivalent to mib_get_next() on the leaf's own OID.
 */
const struct mib_leaf *mib_next_leaf(const struct mib_leaf *leaf);

/** Encode `leaf`'s current value as a BER TLV into the (backward) encoder. */
void mib_put_value(struct ber_enc *e, const struct mib_leaf *leaf);

/** Encode `leaf`'s OID as a BER OID TLV into the (backward) encoder. */
void mib_put_oid(struct ber_enc *e, const struct mib_leaf *leaf);

/** @return sysUpTime in TimeTicks (hundredths of a second since mib_init). */
uint32_t mib_sys_uptime(void);

#endif /* APP_SNMP_MIB_H_ */
