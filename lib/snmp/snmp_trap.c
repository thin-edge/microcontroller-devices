/* SPDX-License-Identifier: Apache-2.0
 *
 * SNMPv2c trap originator. A trap is a normal PDU (tag SNMPv2-Trap = 0xA7) whose
 * first two varbinds are sysUpTime.0 and snmpTrapOID.0, per RFC 3416. We build
 * it backward with the shared BER encoder and send it as one UDP datagram to the
 * configured manager. A watcher thread polls the switch simulation's interface
 * states on the sample interval and emits linkUp/linkDown only on a transition.
 */

#include "snmp_trap.h"
#include "snmp_ber.h"
#include "snmp_mib.h"
#include "sim_switch.h"

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/unistd.h>

LOG_MODULE_REGISTER(app_snmp_trap, CONFIG_LOG_DEFAULT_LEVEL);

#define SNMP_PDU_TRAP_V2 0xA7

/* Well-known OIDs used in trap varbinds. */
static const uint32_t oid_sysuptime[]  = { 1, 3, 6, 1, 2, 1, 1, 3, 0 };
static const uint32_t oid_trapoid[]    = { 1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0 };
static const uint32_t oid_coldstart[]  = { 1, 3, 6, 1, 6, 3, 1, 1, 5, 1 };
static const uint32_t oid_linkdown[]   = { 1, 3, 6, 1, 6, 3, 1, 1, 5, 3 };
static const uint32_t oid_linkup[]     = { 1, 3, 6, 1, 6, 3, 1, 1, 5, 4 };
/* ifIndex column prefix (row appended): 1.3.6.1.2.1.2.2.1.1.<n> */
static const uint32_t oid_ifindex_col[] = { 1, 3, 6, 1, 2, 1, 2, 2, 1, 1 };

static uint8_t trap_buf[512];
static int trap_sock = -1;
static struct sockaddr_in mgr_addr;
static bool mgr_resolved;       /* mgr_addr.sin_addr currently holds a valid dest */
static bool coldstart_pending;  /* coldStart still to send (manager not yet resolved) */
static int32_t trap_request_id;
static enum sim_if_status last_oper[SIM_SWITCH_MAX_IF];

/*
 * Resolve CONFIG_APP_SNMP_TRAP_MANAGER into mgr_addr.sin_addr. Accepts either a
 * literal IPv4 address (fast path, no lookup) or a hostname — including an mDNS
 * ".local" name when CONFIG_MDNS_RESOLVER is enabled — resolved via getaddrinfo.
 * Only the address is set here; family/port are set once in snmp_trap_start().
 * @return true if mgr_addr now holds a usable destination.
 */
static bool resolve_manager(void)
{
	const char *host = CONFIG_APP_SNMP_TRAP_MANAGER;

	/* Literal IPv4? No name lookup needed. */
	if (inet_pton(AF_INET, host, &mgr_addr.sin_addr) == 1) {
		return true;
	}

	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM,
	};
	struct zsock_addrinfo *res = NULL;
	int rc = zsock_getaddrinfo(host, NULL, &hints, &res);

	if (rc != 0 || res == NULL) {
		LOG_WRN("trap manager '%s' not resolvable (%d) — will retry", host, rc);
		return false;
	}

	mgr_addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	zsock_freeaddrinfo(res);
	LOG_INF("trap manager '%s' resolved", host);
	return true;
}

/* Emit one varbind SEQ{name, value} where the value is an OID. */
static void vb_oid(struct ber_enc *e, const uint32_t *name, size_t nlen,
		   const uint32_t *val, size_t vlen)
{
	size_t mark = ber_mark(e);

	ber_put_oid(e, val, vlen);
	ber_put_oid(e, name, nlen);
	ber_wrap(e, mark, BER_TAG_SEQUENCE);
}

/* Emit one varbind SEQ{name, value} where the value is a TimeTicks. */
static void vb_timeticks(struct ber_enc *e, const uint32_t *name, size_t nlen,
			 uint32_t ticks)
{
	size_t mark = ber_mark(e);

	ber_put_uint_tagged(e, ticks, BER_TAG_TIMETICKS);
	ber_put_oid(e, name, nlen);
	ber_wrap(e, mark, BER_TAG_SEQUENCE);
}

/* Emit one varbind SEQ{name, value} where the value is an INTEGER. */
static void vb_int(struct ber_enc *e, const uint32_t *name, size_t nlen, int32_t v)
{
	size_t mark = ber_mark(e);

	ber_put_int(e, v);
	ber_put_oid(e, name, nlen);
	ber_wrap(e, mark, BER_TAG_SEQUENCE);
}

/*
 * Build and send a trap. `trap_oid`/`trap_oid_len` identify the notification;
 * if `ifindex` > 0 an ifIndex varbind is appended (for linkUp/linkDown).
 */
static void send_trap(const uint32_t *trap_oid, size_t trap_oid_len, int ifindex)
{
	if (trap_sock < 0 || !mgr_resolved) {
		return;
	}

	struct ber_enc e;

	ber_enc_init(&e, trap_buf, sizeof(trap_buf));

	size_t top = ber_mark(&e);

	/* varbind list (emit reverse: [ifIndex], snmpTrapOID, sysUpTime) */
	size_t vbl = ber_mark(&e);

	if (ifindex > 0) {
		uint32_t name[BER_MAX_OID_LEN];
		size_t nlen = ARRAY_SIZE(oid_ifindex_col);

		memcpy(name, oid_ifindex_col, sizeof(oid_ifindex_col));
		name[nlen++] = (uint32_t)ifindex;
		vb_int(&e, name, nlen, ifindex);
	}
	vb_oid(&e, oid_trapoid, ARRAY_SIZE(oid_trapoid), trap_oid, trap_oid_len);
	vb_timeticks(&e, oid_sysuptime, ARRAY_SIZE(oid_sysuptime), mib_sys_uptime());
	ber_wrap(&e, vbl, BER_TAG_SEQUENCE);

	/* PDU fields: error-index, error-status, request-id */
	ber_put_int(&e, 0);
	ber_put_int(&e, 0);
	ber_put_int(&e, ++trap_request_id);
	ber_wrap(&e, top, SNMP_PDU_TRAP_V2);

	/* message: community, version */
	const char *comm = CONFIG_APP_SNMP_TRAP_COMMUNITY;

	ber_put_octet_str(&e, (const uint8_t *)comm, strlen(comm));
	ber_put_int(&e, 1 /* v2c */);
	ber_wrap(&e, top, BER_TAG_SEQUENCE);

	size_t len = ber_enc_length(&e);

	if (len == 0) {
		LOG_WRN("trap did not fit buffer");
		return;
	}

	const uint8_t *pkt = trap_buf + (sizeof(trap_buf) - len);

	if (sendto(trap_sock, pkt, len, 0, (struct sockaddr *)&mgr_addr,
		   sizeof(mgr_addr)) < 0) {
		LOG_WRN("trap sendto failed (%d) — will re-resolve manager", errno);
		/* Force a fresh lookup next tick in case the manager's IP changed. */
		mgr_resolved = false;
	}
}

/* --- watcher thread --- */

#define TRAP_THREAD_STACK_SIZE 3072
#define TRAP_THREAD_PRIORITY   7

K_THREAD_STACK_DEFINE(trap_stack, TRAP_THREAD_STACK_SIZE);
static struct k_thread trap_thread;

static void trap_watcher(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		k_sleep(K_MSEC(CONFIG_APP_SAMPLE_INTERVAL_MS));

		/* Keep trying to resolve the manager until it succeeds (it may be
		 * offline at boot, or its mDNS name may not be answerable yet). */
		if (!mgr_resolved) {
			mgr_resolved = resolve_manager();
		}
		/* Send the deferred coldStart once the manager first resolves. */
		if (mgr_resolved && coldstart_pending) {
			coldstart_pending = false;
			LOG_INF("sending deferred coldStart");
			send_trap(oid_coldstart, ARRAY_SIZE(oid_coldstart), 0);
		}

		size_t nif = sim_switch_if_count();

		for (size_t i = 0; i < nif; i++) {
			enum sim_if_status now = sim_switch_if_oper_status(i);

			if (now == last_oper[i]) {
				continue;
			}
			last_oper[i] = now;

			int ifindex = (int)(i + 1);

			if (now == SIM_IF_DOWN) {
				LOG_INF("linkDown on ifIndex %d", ifindex);
				send_trap(oid_linkdown, ARRAY_SIZE(oid_linkdown),
					  ifindex);
			} else {
				LOG_INF("linkUp on ifIndex %d", ifindex);
				send_trap(oid_linkup, ARRAY_SIZE(oid_linkup),
					  ifindex);
			}
		}
	}
}

void snmp_trap_start(void)
{
	trap_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (trap_sock < 0) {
		LOG_ERR("trap socket() failed (%d) — traps disabled", errno);
		return;
	}

	memset(&mgr_addr, 0, sizeof(mgr_addr));
	mgr_addr.sin_family = AF_INET;
	mgr_addr.sin_port = htons(CONFIG_APP_SNMP_TRAP_PORT);

	/* Resolve the manager (IP literal or hostname/.local). If it can't be
	 * resolved yet, the watcher retries and sends coldStart once it can. */
	mgr_resolved = resolve_manager();

	/* Snapshot current states so we only trap on subsequent transitions. */
	for (size_t i = 0; i < sim_switch_if_count(); i++) {
		last_oper[i] = sim_switch_if_oper_status(i);
	}

	LOG_INF("SNMP traps -> %s:%d",
		CONFIG_APP_SNMP_TRAP_MANAGER, CONFIG_APP_SNMP_TRAP_PORT);
	if (mgr_resolved) {
		send_trap(oid_coldstart, ARRAY_SIZE(oid_coldstart), 0);
	} else {
		coldstart_pending = true;
	}

	k_thread_create(&trap_thread, trap_stack, TRAP_THREAD_STACK_SIZE,
			trap_watcher, NULL, NULL, NULL,
			TRAP_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&trap_thread, "snmp_trap");
}
