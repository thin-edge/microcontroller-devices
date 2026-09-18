/* SPDX-License-Identifier: Apache-2.0
 *
 * SNMPv2c agent: a UDP listener that parses GET/GETNEXT/GETBULK requests, maps
 * them onto the switch/router MIB view (snmp_mib) and replies. BER handling is
 * in snmp_ber; this file owns the socket loop, PDU framing and the GETBULK walk.
 *
 * Design constraints (see the change's design.md): one bound UDP socket, fixed
 * request/response work buffers, a compile-time bound on output varbinds, and no
 * per-request heap allocation. Malformed or oversized requests yield a valid
 * SNMP error (or a silent drop), never a crash.
 */

#include "snmp_agent.h"
#include "snmp_ber.h"
#include "snmp_mib.h"
#include "data_source.h"
#include "liveness.h"

#if defined(CONFIG_APP_SNMP_TRAP)
#include "snmp_trap.h"
#endif

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>

LOG_MODULE_REGISTER(app_snmp, CONFIG_LOG_DEFAULT_LEVEL);

/* SNMP version field: 0 = v1, 1 = v2c. We serve v2c only. */
#define SNMP_VERSION_2C 1

/* v2c error-status codes we emit. */
#define SNMP_ERR_NONE        0
#define SNMP_ERR_TOO_BIG     1
#define SNMP_ERR_GEN_ERR     5
#define SNMP_ERR_NOT_WRITABLE 17

/* Work-buffer and fan-out limits (bound RAM + response size). */
#define SNMP_RX_BUF   1500
#define SNMP_TX_BUF   1500
#define SNMP_MAX_REQ_VB   16 /* requested varbinds we parse */
#define SNMP_MAX_RESP_VB  40 /* varbinds we will emit (GETBULK truncates past this) */

static int agent_sock = -1;
static uint8_t rx_buf[SNMP_RX_BUF];
static uint8_t tx_buf[SNMP_TX_BUF];

/* A parsed request-name / planned response-name. */
struct vbname {
	uint32_t oid[BER_MAX_OID_LEN];
	size_t len;
};

/* One planned response varbind. */
struct resp_vb {
	uint32_t oid[BER_MAX_OID_LEN];
	size_t oid_len;
	const struct mib_leaf *leaf; /* value source, or NULL */
	uint8_t exception;           /* if leaf==NULL: SNMP_EXC_*; 0 -> NULL */
};

/* --- response encoding (backward) --- */

static void emit_varbind(struct ber_enc *e, const struct resp_vb *vb)
{
	size_t mark = ber_mark(e);

	if (vb->leaf) {
		mib_put_value(e, vb->leaf);
	} else if (vb->exception) {
		ber_put_exception(e, vb->exception);
	} else {
		ber_put_null(e);
	}
	ber_put_oid(e, vb->oid, vb->oid_len);
	ber_wrap(e, mark, BER_TAG_SEQUENCE);
}

/*
 * Encode a full Response message into `e`. Varbinds are emitted in reverse so
 * they land in order. @return encoded length, or 0 on overflow.
 */
static size_t encode_response(struct ber_enc *e, int32_t request_id,
			      int32_t err_status, int32_t err_index,
			      const uint8_t *community, size_t comm_len,
			      const struct resp_vb *vbs, size_t nvb)
{
	size_t top = ber_mark(e); /* == buffer end; used for PDU and message wraps */

	/* varbind list */
	size_t vbl_mark = ber_mark(e);

	for (size_t i = nvb; i-- > 0;) {
		emit_varbind(e, &vbs[i]);
	}
	ber_wrap(e, vbl_mark, BER_TAG_SEQUENCE);

	/* PDU fields (reverse order): error-index, error-status, request-id */
	ber_put_int(e, err_index);
	ber_put_int(e, err_status);
	ber_put_int(e, request_id);
	ber_wrap(e, top, SNMP_PDU_RESPONSE);

	/* message: community, version */
	ber_put_octet_str(e, community, comm_len);
	ber_put_int(e, SNMP_VERSION_2C);
	ber_wrap(e, top, BER_TAG_SEQUENCE);

	return ber_enc_length(e);
}

/* Build a small error response (empty varbind list). Always fits. */
static size_t encode_error(struct ber_enc *e, int32_t request_id,
			   int32_t err_status, int32_t err_index,
			   const uint8_t *community, size_t comm_len)
{
	return encode_response(e, request_id, err_status, err_index,
			       community, comm_len, NULL, 0);
}

/* --- request parsing --- */

struct parsed_req {
	uint8_t pdu_type;
	int32_t request_id;
	int32_t field2; /* error-status  (GET*) or non-repeaters (GETBULK) */
	int32_t field3; /* error-index   (GET*) or max-repetitions (GETBULK) */
	struct vbname names[SNMP_MAX_REQ_VB];
	size_t nvb;
	const uint8_t *community;
	size_t comm_len;
};

/*
 * Parse an SNMPv2c request. @return true if it is a well-formed v2c PDU with a
 * matching community; false means "drop silently" (bad version/community/format).
 */
static bool parse_request(const uint8_t *buf, size_t len, struct parsed_req *r)
{
	struct ber_dec msg, pdu, vbl;
	int32_t version;

	ber_dec_init(&msg, buf, len);

	struct ber_dec top;

	if (!ber_enter(&msg, BER_TAG_SEQUENCE, &top)) {
		return false;
	}
	if (!ber_get_int(&top, &version) || version != SNMP_VERSION_2C) {
		return false; /* only v2c */
	}
	if (!ber_get_octet(&top, &r->community, &r->comm_len)) {
		return false;
	}

	/* Community must match the configured read community. */
	const char *want = CONFIG_APP_SNMP_READ_COMMUNITY;
	size_t want_len = strlen(want);

	if (r->comm_len != want_len ||
	    memcmp(r->community, want, want_len) != 0) {
		return false; /* silently ignore */
	}

	/* PDU (context-tagged, constructed). */
	uint8_t tag;
	const uint8_t *pdu_content;
	size_t pdu_len;

	if (!ber_get_tlv(&top, &tag, &pdu_content, &pdu_len)) {
		return false;
	}
	r->pdu_type = tag;
	ber_dec_init(&pdu, pdu_content, pdu_len);

	if (!ber_get_int(&pdu, &r->request_id) ||
	    !ber_get_int(&pdu, &r->field2) ||
	    !ber_get_int(&pdu, &r->field3)) {
		return false;
	}
	if (!ber_enter(&pdu, BER_TAG_SEQUENCE, &vbl)) {
		return false;
	}

	r->nvb = 0;
	while (!ber_dec_done(&vbl)) {
		struct ber_dec vb;

		if (!ber_enter(&vbl, BER_TAG_SEQUENCE, &vb)) {
			return false;
		}
		if (r->nvb >= SNMP_MAX_REQ_VB) {
			/* Too many varbinds to track; ignore the rest. */
			break;
		}

		struct vbname *n = &r->names[r->nvb];

		if (!ber_get_oid(&vb, n->oid, BER_MAX_OID_LEN, &n->len)) {
			return false;
		}
		/* value (NULL in a request) is ignored */
		r->nvb++;
	}

	return r->nvb > 0;
}

/* --- response planning --- */

static void plan_get(const struct parsed_req *r, struct resp_vb *out, size_t *nout)
{
	size_t n = 0;

	for (size_t i = 0; i < r->nvb; i++) {
		struct resp_vb *vb = &out[n++];

		memcpy(vb->oid, r->names[i].oid, r->names[i].len * sizeof(uint32_t));
		vb->oid_len = r->names[i].len;
		vb->leaf = mib_get_exact(r->names[i].oid, r->names[i].len);
		vb->exception = vb->leaf ? 0 : SNMP_EXC_NO_SUCH_INSTANCE;
	}
	*nout = n;
}

static void set_vb_next(struct resp_vb *vb, const uint32_t *from, size_t from_len)
{
	const struct mib_leaf *leaf = mib_get_next(from, from_len);

	if (leaf) {
		memcpy(vb->oid, leaf->oid, leaf->oid_len * sizeof(uint32_t));
		vb->oid_len = leaf->oid_len;
		vb->leaf = leaf;
		vb->exception = 0;
	} else {
		memcpy(vb->oid, from, from_len * sizeof(uint32_t));
		vb->oid_len = from_len;
		vb->leaf = NULL;
		vb->exception = SNMP_EXC_END_OF_MIB_VIEW;
	}
}

static void plan_getnext(const struct parsed_req *r, struct resp_vb *out, size_t *nout)
{
	size_t n = 0;

	for (size_t i = 0; i < r->nvb; i++) {
		set_vb_next(&out[n++], r->names[i].oid, r->names[i].len);
	}
	*nout = n;
}

static void plan_getbulk(const struct parsed_req *r, struct resp_vb *out, size_t *nout)
{
	int32_t non_rep = r->field2;
	int32_t max_rep = r->field3;
	size_t n = 0;

	if (non_rep < 0) {
		non_rep = 0;
	}
	if ((size_t)non_rep > r->nvb) {
		non_rep = (int32_t)r->nvb;
	}
	if (max_rep < 0) {
		max_rep = 0;
	}

	/* Non-repeaters: one GETNEXT each. */
	for (int32_t i = 0; i < non_rep && n < SNMP_MAX_RESP_VB; i++) {
		set_vb_next(&out[n++], r->names[i].oid, r->names[i].len);
	}

	/* Repeaters: per-repetition round-robin, tracking each walker's cursor. */
	size_t nrep = r->nvb - (size_t)non_rep;

	if (nrep == 0 || max_rep == 0) {
		*nout = n;
		return;
	}

	/* Cursor OIDs for each repeater, seeded from the request. */
	static struct vbname cursor[SNMP_MAX_REQ_VB];
	bool done_walk[SNMP_MAX_REQ_VB] = { false };

	for (size_t j = 0; j < nrep; j++) {
		cursor[j] = r->names[non_rep + j];
	}

	for (int32_t rep = 0; rep < max_rep && n < SNMP_MAX_RESP_VB; rep++) {
		for (size_t j = 0; j < nrep && n < SNMP_MAX_RESP_VB; j++) {
			if (done_walk[j]) {
				continue;
			}

			struct resp_vb *vb = &out[n++];

			set_vb_next(vb, cursor[j].oid, cursor[j].len);
			if (vb->exception == SNMP_EXC_END_OF_MIB_VIEW) {
				done_walk[j] = true;
			} else {
				memcpy(cursor[j].oid, vb->oid,
				       vb->oid_len * sizeof(uint32_t));
				cursor[j].len = vb->oid_len;
			}
		}
	}

	*nout = n;
}

/* Build and return the response length in tx_buf, or 0 to send nothing. */
static size_t handle_request(const uint8_t *buf, size_t len)
{
	struct parsed_req r;
	struct ber_enc e;

	if (!parse_request(buf, len, &r)) {
		return 0; /* drop */
	}

	/* Read-only agent: refuse SET without touching state. */
	if (r.pdu_type == SNMP_PDU_SET) {
		ber_enc_init(&e, tx_buf, sizeof(tx_buf));
		return encode_error(&e, r.request_id, SNMP_ERR_NOT_WRITABLE, 1,
				    r.community, r.comm_len);
	}

	static struct resp_vb vbs[SNMP_MAX_RESP_VB];
	size_t nvb = 0;

	switch (r.pdu_type) {
	case SNMP_PDU_GET:
		plan_get(&r, vbs, &nvb);
		break;
	case SNMP_PDU_GETNEXT:
		plan_getnext(&r, vbs, &nvb);
		break;
	case SNMP_PDU_GETBULK:
		plan_getbulk(&r, vbs, &nvb);
		break;
	default:
		/* Unknown/unsupported PDU: genErr keeps managers informed. */
		ber_enc_init(&e, tx_buf, sizeof(tx_buf));
		return encode_error(&e, r.request_id, SNMP_ERR_GEN_ERR, 0,
				    r.community, r.comm_len);
	}

	ber_enc_init(&e, tx_buf, sizeof(tx_buf));
	size_t out_len = encode_response(&e, r.request_id, SNMP_ERR_NONE, 0,
					 r.community, r.comm_len, vbs, nvb);

	if (out_len == 0) {
		/* Did not fit one datagram — respond tooBig with an empty list. */
		ber_enc_init(&e, tx_buf, sizeof(tx_buf));
		out_len = encode_error(&e, r.request_id, SNMP_ERR_TOO_BIG, 0,
				       r.community, r.comm_len);
	}
	return out_len;
}

/* --- listener thread --- */

#define SNMP_THREAD_STACK_SIZE 4096
#define SNMP_THREAD_PRIORITY   6
/* Longest wait for a request before the loop reports progress again. */
#define PROTO_WAIT_MS          5000

K_THREAD_STACK_DEFINE(snmp_stack, SNMP_THREAD_STACK_SIZE);
static struct k_thread snmp_thread;

static void snmp_listener(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		struct sockaddr_in src;
		socklen_t src_len = sizeof(src);
		struct zsock_pollfd pfd = { .fd = agent_sock, .events = ZSOCK_POLLIN };

		/* Wait with a bound so an idle agent still shows it is alive. */
		app_alive(APP_CTX_PROTO);
		int prc = zsock_poll(&pfd, 1, PROTO_WAIT_MS);

		if (prc == 0) {
			continue; /* no request this interval */
		}
		if (prc < 0) {
			LOG_WRN("poll failed (%d)", errno);
			k_sleep(K_MSEC(100));
			continue;
		}

		ssize_t rc = recvfrom(agent_sock, rx_buf, sizeof(rx_buf), 0,
				      (struct sockaddr *)&src, &src_len);

		if (rc <= 0) {
			if (rc < 0) {
				LOG_WRN("recvfrom failed (%d)", errno);
				k_sleep(K_MSEC(100));
			}
			continue;
		}

		size_t out_len = handle_request(rx_buf, (size_t)rc);

		if (out_len == 0) {
			continue; /* silently dropped */
		}

		/* Response is encoded at the tail of tx_buf. */
		const uint8_t *resp = tx_buf + (sizeof(tx_buf) - out_len);

		if (sendto(agent_sock, resp, out_len, 0,
			   (struct sockaddr *)&src, src_len) < 0) {
			LOG_WRN("sendto failed (%d)", errno);
		}
	}
}

int snmp_agent_start(void)
{
	/* Start the data model / simulation (switch sim's step timer). */
	data_source_init();

	/* Materialise the MIB view for the active simulation. */
	mib_init();

	agent_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (agent_sock < 0) {
		LOG_ERR("socket() failed (%d)", errno);
		return -errno;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(CONFIG_APP_SNMP_PORT),
	};

	if (bind(agent_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("bind(%d) failed (%d)", CONFIG_APP_SNMP_PORT, errno);
		close(agent_sock);
		agent_sock = -1;
		return -errno;
	}

	k_thread_create(&snmp_thread, snmp_stack, SNMP_THREAD_STACK_SIZE,
			snmp_listener, NULL, NULL, NULL,
			SNMP_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&snmp_thread, "snmp");

	LOG_INF("SNMPv2c agent listening on UDP %d (%zu MIB objects)",
		CONFIG_APP_SNMP_PORT, mib_leaf_count());

#if defined(CONFIG_APP_SNMP_TRAP)
	snmp_trap_start();
#endif

	return 0;
}
