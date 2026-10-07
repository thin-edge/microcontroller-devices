/* SPDX-License-Identifier: Apache-2.0
 *
 * Remote-access target decisions: which names go to mDNS, which lookup
 * failures are worth another try, whether the policy lets a resolved
 * address through, and how a target is written in events and logs; and
 * the one-shot mDNS query the client sends for a ".local" target, with
 * the parser for its answer. Pure, and unit-tested on native_sim.
 */

#include "tedge_internal.h"

#include <zephyr/net/dns_resolve.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

bool tedge_ra_is_mdns_name(const char *host)
{
	static const char suffix[] = ".local";
	size_t n = strlen(host);

	/* A fully qualified "pi.local." is the same name. */
	if (n > 0 && host[n - 1] == '.') {
		n--;
	}
	/* "local" alone, or ".local", names no host. */
	if (n <= sizeof(suffix) - 1) {
		return false;
	}
	return strncasecmp(host + n - (sizeof(suffix) - 1), suffix,
			   sizeof(suffix) - 1) == 0;
}

bool tedge_ra_resolve_retryable(int status)
{
	switch (status) {
	case DNS_EAI_CANCELED: /* nobody answered in time */
	case DNS_EAI_AGAIN:
	case DNS_EAI_SYSTEM:   /* no socket or server to send on, yet */
	case DNS_EAI_MEMORY:
		return true;
	default:
		/* An authoritative "no such name" (DNS_EAI_NONAME, _NODATA,
		 * _FAIL) gives the same answer the next time. */
		return false;
	}
}

enum tedge_ra_verdict tedge_ra_policy_decide(enum tedge_ra_policy policy,
					     const struct tedge_ra_facts *f)
{
	bool lan = f->loopback || f->own || f->on_subnet;
	bool allowed;

	switch (policy) {
	case TEDGE_RA_POLICY_LOCAL:
		allowed = f->loopback || f->own;
		break;
	case TEDGE_RA_POLICY_LIST:
		allowed = f->in_list;
		break;
	case TEDGE_RA_POLICY_LAN:
	default:
		allowed = lan;
		break;
	}
	if (!allowed) {
		return TEDGE_RA_REFUSED_POLICY;
	}
	/* Any host on the link can answer an mDNS query, so a ".local" name
	 * is only trusted to point at this link, whatever the policy. */
	if (f->mdns && !lan) {
		return TEDGE_RA_REFUSED_OFF_LINK;
	}
	return TEDGE_RA_ALLOWED;
}

int tedge_ra_target_str(char *buf, size_t len, const char *host,
			const char *addr, uint16_t port)
{
	if (addr == NULL || addr[0] == '\0' || strcmp(host, addr) == 0) {
		return snprintf(buf, len, "%s:%u", host, port);
	}
	return snprintf(buf, len, "%s (%s):%u", host, addr, port);
}

/* ------------------------------------------------------------------------ */
/* One-shot mDNS (RFC 6762 section 6.7)                                      */
/* ------------------------------------------------------------------------ */

#define DNS_HDR_LEN   12
#define DNS_TYPE_A    1
#define DNS_CLASS_IN  1
#define DNS_FLAG_QR   0x8000
/* The top bit of an answer's class is mDNS's cache-flush flag. */
#define DNS_CLASS_MASK 0x7fff

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v & 0xff;
}

int tedge_mdns_build_query(uint8_t *buf, size_t len, uint16_t id,
			   const char *name)
{
	size_t n = strlen(name), pos = DNS_HDR_LEN;
	const char *label = name;

	if (n > 0 && name[n - 1] == '.') {
		n--;
	}
	/* Labels, each with its length byte, a terminating zero, type and
	 * class. */
	if (n == 0 || n > 253 || len < DNS_HDR_LEN + n + 2 + 4) {
		return -EINVAL;
	}
	memset(buf, 0, DNS_HDR_LEN);
	put16(buf, id);
	put16(buf + 4, 1); /* one question */

	while (label < name + n) {
		const char *dot = memchr(label, '.', (size_t)(name + n - label));
		size_t l = (dot ? dot : name + n) - label;

		if (l == 0 || l > 63) {
			return -EINVAL;
		}
		buf[pos++] = (uint8_t)l;
		memcpy(buf + pos, label, l);
		pos += l;
		label += l + 1;
	}
	buf[pos++] = 0;
	put16(buf + pos, DNS_TYPE_A);
	put16(buf + pos + 2, DNS_CLASS_IN);
	return (int)(pos + 4);
}

/* Read the (possibly compressed) name at @p off into @p out as
 * "a.b.local". Returns the offset just past it in the message, or -EINVAL. */
static int read_name(const uint8_t *msg, size_t len, size_t off, char *out,
		     size_t out_len)
{
	size_t o = 0, end = 0;
	int hops = 0;

	for (;;) {
		uint8_t l;

		if (off >= len) {
			return -EINVAL;
		}
		l = msg[off];
		if ((l & 0xc0) == 0xc0) {
			if (off + 1 >= len || ++hops > 16) {
				return -EINVAL;
			}
			if (end == 0) {
				end = off + 2;
			}
			off = ((l & 0x3f) << 8) | msg[off + 1];
			continue;
		}
		if (l & 0xc0) {
			return -EINVAL;
		}
		if (l == 0) {
			if (end == 0) {
				end = off + 1;
			}
			break;
		}
		if (off + 1 + l > len || o + l + 2 > out_len) {
			return -EINVAL;
		}
		if (o > 0) {
			out[o++] = '.';
		}
		memcpy(out + o, msg + off + 1, l);
		o += l;
		off += 1 + l;
	}
	out[o] = '\0';
	return (int)end;
}

int tedge_mdns_parse_a(const uint8_t *msg, size_t len, uint16_t id,
		       const char *name, uint8_t addr[4])
{
	char want[256], got[256];
	size_t n = strlen(name);
	uint16_t qd, an;
	int off;

	if (len < DNS_HDR_LEN || n == 0 || n >= sizeof(want)) {
		return -EINVAL;
	}
	memcpy(want, name, n + 1);
	if (want[n - 1] == '.') {
		want[n - 1] = '\0';
	}
	/* A legacy-unicast answer echoes the query's id. */
	if (get16(msg) != id || !(get16(msg + 2) & DNS_FLAG_QR)) {
		return -ENOENT;
	}
	qd = get16(msg + 4);
	an = get16(msg + 6);
	off = DNS_HDR_LEN;

	for (uint16_t i = 0; i < qd; i++) {
		off = read_name(msg, len, off, got, sizeof(got));
		if (off < 0 || (size_t)off + 4 > len) {
			return -EINVAL;
		}
		off += 4;
	}
	for (uint16_t i = 0; i < an; i++) {
		uint16_t type, class, rdlen;

		off = read_name(msg, len, off, got, sizeof(got));
		if (off < 0 || (size_t)off + 10 > len) {
			return -EINVAL;
		}
		type = get16(msg + off);
		class = get16(msg + off + 2) & DNS_CLASS_MASK;
		rdlen = get16(msg + off + 8);
		off += 10;
		if ((size_t)off + rdlen > len) {
			return -EINVAL;
		}
		if (type == DNS_TYPE_A && class == DNS_CLASS_IN && rdlen == 4 &&
		    strcasecmp(got, want) == 0) {
			memcpy(addr, msg + off, 4);
			return 0;
		}
		off += rdlen;
	}
	return -ENOENT;
}
