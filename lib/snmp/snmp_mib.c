/* SPDX-License-Identifier: Apache-2.0
 *
 * Switch/router MIB view. See snmp_mib.h. The leaf array is built once in
 * lexicographic order: the system-group scalars, then ifNumber, then the
 * ifTable in column-major order (all rows of column c before column c+1), and
 * last the enterprise firmware-info scalars — which is exactly SNMP's walk
 * order (the private arc 1.3.6.1.4.1 sorts after mib-2's 1.3.6.1.2).
 */

#include "snmp_mib.h"
#include "sim_switch.h"
#include "identity.h"
#include "net.h"

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>

/* Value selectors. Scalars first, then ifTable columns. */
enum {
	K_SYS_DESCR = 0, K_SYS_OBJECTID, K_SYS_UPTIME, K_SYS_CONTACT,
	K_SYS_NAME, K_SYS_LOCATION, K_SYS_SERVICES, K_IF_NUMBER,
	K_IF_INDEX, K_IF_DESCR, K_IF_TYPE, K_IF_MTU, K_IF_SPEED,
	K_IF_ADMIN, K_IF_OPER, K_IF_IN_OCTETS, K_IF_IN_UPKTS,
	K_IF_OUT_OCTETS, K_IF_OUT_UPKTS,
	K_FW_NAME, K_FW_VERSION, K_FW_BUILD,
};

/* ifTable column numbers (standard IF-MIB). */
struct if_column {
	uint16_t kind;
	uint32_t col;
};

static const struct if_column if_columns[] = {
	{ K_IF_INDEX, 1 }, { K_IF_DESCR, 2 }, { K_IF_TYPE, 3 },
	{ K_IF_MTU, 4 }, { K_IF_SPEED, 5 }, { K_IF_ADMIN, 7 },
	{ K_IF_OPER, 8 }, { K_IF_IN_OCTETS, 10 }, { K_IF_IN_UPKTS, 11 },
	{ K_IF_OUT_OCTETS, 16 }, { K_IF_OUT_UPKTS, 17 },
};

#define NUM_IF_COLUMNS ARRAY_SIZE(if_columns)
#define NUM_SCALARS    8
/* Enterprise firmware-info scalars, in the order their kinds are enumerated. */
#define NUM_FW_SCALARS 3
#define MIB_MAX_LEAVES (NUM_SCALARS + NUM_IF_COLUMNS * SIM_SWITCH_MAX_IF + NUM_FW_SCALARS)

/* sysObjectID value: a private-enterprise arc identifying this simulated switch
 * (1.3.6.1.4.1.99999.1 — a placeholder enterprise number for the demo). It is
 * also the parent of the firmware-info scalars added at the end of mib_init(),
 * so a manager that reads sysObjectID.0 knows where to walk for them. */
static const uint32_t sys_object_id[] = { 1, 3, 6, 1, 4, 1, 99999, 1 };

static struct mib_leaf leaves[MIB_MAX_LEAVES];
static size_t leaf_count;
static int64_t uptime_start_ms;

/* Append a leaf with the given OID and value selector. */
static void add_leaf(const uint32_t *oid, size_t len, uint16_t kind, uint8_t row)
{
	if (leaf_count >= MIB_MAX_LEAVES || len > BER_MAX_OID_LEN) {
		return;
	}

	struct mib_leaf *l = &leaves[leaf_count++];

	memcpy(l->oid, oid, len * sizeof(uint32_t));
	l->oid_len = (uint8_t)len;
	l->kind = kind;
	l->row = row;
}

void mib_init(void)
{
	static const uint32_t sys[] = { 1, 3, 6, 1, 2, 1, 1 }; /* system group */
	static const uint32_t ifentry[] = { 1, 3, 6, 1, 2, 1, 2, 2, 1 };
	static const uint32_t ifnum[] = { 1, 3, 6, 1, 2, 1, 2, 1, 0 };
	uint32_t oid[BER_MAX_OID_LEN];

	leaf_count = 0;
	uptime_start_ms = k_uptime_get();

	/* --- system group scalars: 1.3.6.1.2.1.1.<n>.0 --- */
	for (uint16_t n = 1; n <= NUM_SCALARS - 1; n++) { /* 1..7 -> sysDescr..sysServices */
		memcpy(oid, sys, sizeof(sys));
		oid[7] = n;
		oid[8] = 0;
		add_leaf(oid, 9, (uint16_t)(K_SYS_DESCR + (n - 1)), 0);
	}

	/* --- ifNumber.0 : 1.3.6.1.2.1.2.1.0 --- */
	add_leaf(ifnum, ARRAY_SIZE(ifnum), K_IF_NUMBER, 0);

	/* --- ifTable, column-major: 1.3.6.1.2.1.2.2.1.<col>.<row> --- */
	size_t nif = sim_switch_if_count();

	for (size_t ci = 0; ci < NUM_IF_COLUMNS; ci++) {
		for (size_t r = 1; r <= nif; r++) {
			memcpy(oid, ifentry, sizeof(ifentry));
			oid[9] = if_columns[ci].col;
			oid[10] = (uint32_t)r;
			add_leaf(oid, 11, if_columns[ci].kind, (uint8_t)r);
		}
	}

	/* --- firmware info: <sysObjectID>.<n>.0 (1.3.6.1.4.1.99999.1.<n>.0) ---
	 * Which firmware image a device runs has no home in MIB-II: sysDescr.0
	 * carries all three strings in one sentence, which a collector would have
	 * to parse. These give each one its own object, so it can be read (and
	 * reported to a cloud) on its own.
	 */
	for (uint16_t n = 1; n <= NUM_FW_SCALARS; n++) {
		memcpy(oid, sys_object_id, sizeof(sys_object_id));
		oid[ARRAY_SIZE(sys_object_id)] = n;
		oid[ARRAY_SIZE(sys_object_id) + 1] = 0;
		add_leaf(oid, ARRAY_SIZE(sys_object_id) + 2,
			 (uint16_t)(K_FW_NAME + (n - 1)), 0);
	}
}

size_t mib_leaf_count(void)
{
	return leaf_count;
}

uint32_t mib_sys_uptime(void)
{
	int64_t ms = k_uptime_get() - uptime_start_ms;

	if (ms < 0) {
		ms = 0;
	}
	return (uint32_t)(ms / 10); /* hundredths of a second */
}

const struct mib_leaf *mib_get_exact(const uint32_t *oid, size_t len)
{
	for (size_t i = 0; i < leaf_count; i++) {
		if (oid_cmp(leaves[i].oid, leaves[i].oid_len, oid, len) == 0) {
			return &leaves[i];
		}
	}
	return NULL;
}

const struct mib_leaf *mib_get_next(const uint32_t *oid, size_t len)
{
	/* Leaves are sorted; return the first strictly greater. */
	for (size_t i = 0; i < leaf_count; i++) {
		if (oid_cmp(leaves[i].oid, leaves[i].oid_len, oid, len) > 0) {
			return &leaves[i];
		}
	}
	return NULL;
}

/* --- value encoders --- */

static void put_str(struct ber_enc *e, const char *s)
{
	ber_put_octet_str(e, (const uint8_t *)s, strlen(s));
}

void mib_put_value(struct ber_enc *e, const struct mib_leaf *leaf)
{
	size_t idx = (leaf->row > 0) ? (size_t)(leaf->row - 1) : 0;
	char buf[96];

	switch (leaf->kind) {
	case K_SYS_DESCR:
		snprintf(buf, sizeof(buf), "%s %s (build %s)",
			 app_identity_firmware_name(),
			 app_identity_firmware_version(),
			 app_identity_build_timestamp());
		put_str(e, buf);
		break;
	case K_SYS_OBJECTID:
		ber_put_oid(e, sys_object_id, ARRAY_SIZE(sys_object_id));
		break;
	case K_SYS_UPTIME:
		ber_put_uint_tagged(e, mib_sys_uptime(), BER_TAG_TIMETICKS);
		break;
	case K_SYS_CONTACT:
		put_str(e, CONFIG_APP_SNMP_SYS_CONTACT);
		break;
	case K_SYS_NAME:
		put_str(e, app_net_hostname());
		break;
	case K_SYS_LOCATION:
		put_str(e, CONFIG_APP_SNMP_SYS_LOCATION);
		break;
	case K_SYS_SERVICES:
		/* datalink (L2) + internet (L3): 2^1 + 2^2 = 6. */
		ber_put_int(e, 6);
		break;
	case K_IF_NUMBER:
		ber_put_int(e, (int32_t)sim_switch_if_count());
		break;
	case K_IF_INDEX:
		ber_put_int(e, (int32_t)leaf->row);
		break;
	case K_IF_DESCR:
		put_str(e, sim_switch_if_descr(idx));
		break;
	case K_IF_TYPE:
		ber_put_int(e, sim_switch_if_type(idx));
		break;
	case K_IF_MTU:
		ber_put_int(e, (int32_t)sim_switch_if_mtu(idx));
		break;
	case K_IF_SPEED:
		ber_put_uint_tagged(e, sim_switch_if_speed(idx), BER_TAG_GAUGE32);
		break;
	case K_IF_ADMIN:
		ber_put_int(e, (int32_t)sim_switch_if_admin_status(idx));
		break;
	case K_IF_OPER:
		ber_put_int(e, (int32_t)sim_switch_if_oper_status(idx));
		break;
	case K_IF_IN_OCTETS:
		ber_put_uint_tagged(e, sim_switch_if_counter(idx, SIM_IF_IN_OCTETS),
				    BER_TAG_COUNTER32);
		break;
	case K_IF_IN_UPKTS:
		ber_put_uint_tagged(e, sim_switch_if_counter(idx, SIM_IF_IN_UCAST_PKTS),
				    BER_TAG_COUNTER32);
		break;
	case K_IF_OUT_OCTETS:
		ber_put_uint_tagged(e, sim_switch_if_counter(idx, SIM_IF_OUT_OCTETS),
				    BER_TAG_COUNTER32);
		break;
	case K_IF_OUT_UPKTS:
		ber_put_uint_tagged(e, sim_switch_if_counter(idx, SIM_IF_OUT_UCAST_PKTS),
				    BER_TAG_COUNTER32);
		break;
	case K_FW_NAME:
		put_str(e, app_identity_firmware_name());
		break;
	case K_FW_VERSION:
		put_str(e, app_identity_firmware_version());
		break;
	case K_FW_BUILD:
		put_str(e, app_identity_build_timestamp());
		break;
	default:
		ber_put_null(e);
		break;
	}
}
