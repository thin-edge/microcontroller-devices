/* SPDX-License-Identifier: Apache-2.0
 *
 * Spike F (c8y-direct-spikes, tasks 6.1-6.8): Cumulocity Cloud Remote Access
 * through the device, to the device itself or another host on its network.
 * Throwaway measurement code; the production feature goes into tedge-zephyr.
 *
 * 530,<serial>,<host>,<port>,<connectionKey>
 *   -> target policy check (same subnet / allow-list / local only)
 *   -> TCP connect to <host>:<port>
 *   -> WSS to wss://<tenant>/service/remoteaccess/device/<connectionKey>
 *      (Authorization: Bearer <JWT>, or the client certificate alone;
 *       Sec-WebSocket-Protocol: binary)
 *   -> copy bytes both ways until either side closes or goes idle.
 *
 * One session at a time (CONFIG_SPIKE_RA_MAX_SESSIONS is 1 here). The MQTT
 * thread reports the operation and the events: the bridge thread only hands
 * it results through spike_ra_poll_event(), because the MQTT client is not
 * thread-safe.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/websocket.h>
#include <zephyr/sys/atomic.h>

#include <mbedtls/memory_buffer_alloc.h>

#include "spike.h"

LOG_MODULE_REGISTER(spike_ra, LOG_LEVEL_INF);

#define BUF_SIZE CONFIG_SPIKE_RA_BUF_SIZE

static atomic_t busy;
static char target_host[64];
static uint16_t target_port;
static char conn_key[64];

/* Results for the MQTT thread. */
K_MSGQ_DEFINE(ra_events, sizeof(struct spike_ra_event), 4, 4);

static uint8_t to_ws[BUF_SIZE];
static uint8_t to_tcp[BUF_SIZE];
static uint8_t ws_tmp[1024];

K_THREAD_STACK_DEFINE(ra_stack, 8192);
static struct k_thread ra_thread;

static void post(enum spike_ra_event_type type, const char *fmt, ...)
{
	struct spike_ra_event ev = {.type = type};
	va_list ap;

	va_start(ap, fmt);
	vsnprintk(ev.text, sizeof(ev.text), fmt, ap);
	va_end(ap);
	k_msgq_put(&ra_events, &ev, K_NO_WAIT);
}

int spike_ra_poll_event(struct spike_ra_event *ev)
{
	return k_msgq_get(&ra_events, ev, K_NO_WAIT);
}

/* ------------------------------------------------------------------------ */
/* Target policy                                                             */
/* ------------------------------------------------------------------------ */

static int policy_check(const struct in_addr *addr, const char *host,
			uint16_t port, char *reason, size_t rlen)
{
	struct net_if *iface = net_if_get_default();
	bool loopback = (ntohl(addr->s_addr) >> 24) == 127;
	bool own = net_if_ipv4_addr_lookup(addr, NULL) != NULL;

	if (IS_ENABLED(CONFIG_SPIKE_RA_POLICY_LOCAL)) {
		if (loopback || own) {
			return 0;
		}
		snprintk(reason, rlen, "target %s:%u denied: only the device itself "
			 "is allowed", host, port);
		return -EACCES;
	}
	if (IS_ENABLED(CONFIG_SPIKE_RA_POLICY_LIST)) {
		char entry[80];
		const char *list = CONFIG_SPIKE_RA_ALLOW_LIST;

		snprintk(entry, sizeof(entry), "%s:%u", host, port);
		for (const char *p = list; *p;) {
			const char *comma = strchr(p, ',');
			size_t n = comma ? (size_t)(comma - p) : strlen(p);

			if (n == strlen(entry) && strncmp(p, entry, n) == 0) {
				return 0;
			}
			p += n + (comma ? 1 : 0);
		}
		snprintk(reason, rlen, "target %s denied: not in the allow-list",
			 entry);
		return -EACCES;
	}
	/* Default: the device, or a host on one of its IPv4 subnets. */
	if (loopback || own || (iface && net_if_ipv4_addr_mask_cmp(iface, addr))) {
		return 0;
	}
	snprintk(reason, rlen, "target %s:%u denied: not on the device's subnet",
		 host, port);
	return -EACCES;
}

/* ------------------------------------------------------------------------ */
/* Bridge                                                                    */
/* ------------------------------------------------------------------------ */

static int tcp_connect(const struct in_addr *addr, uint16_t port)
{
	struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port),
				 .sin_addr = *addr};
	int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (fd < 0) {
		return -errno;
	}
	if (zsock_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int ret = -errno;

		zsock_close(fd);
		return ret;
	}
	return fd;
}

/* Capture the upgrade response for diagnosis (U12). */
static unsigned int upgrade_status;
static char upgrade_hdrs[256];
static bool hdr_interesting;
static size_t hdr_off;

static int ra_on_header_field(struct http_parser *parser, const char *at,
			      size_t length)
{
	hdr_interesting = (length >= 3 && (strncasecmp(at, "sec-websocket", 13) == 0 ||
					   strncasecmp(at, "www-authenticate", 16) == 0 ||
					   strncasecmp(at, "content-type", 12) == 0));
	if (hdr_interesting && hdr_off < sizeof(upgrade_hdrs) - 2) {
		hdr_off += snprintk(upgrade_hdrs + hdr_off, sizeof(upgrade_hdrs) - hdr_off,
				    "%.*s=", (int)length, at);
	}
	return 0;
}

static int ra_on_header_value(struct http_parser *parser, const char *at,
			      size_t length)
{
	if (hdr_interesting && hdr_off < sizeof(upgrade_hdrs) - 2) {
		hdr_off += snprintk(upgrade_hdrs + hdr_off, sizeof(upgrade_hdrs) - hdr_off,
				    "%.*s; ", (int)MIN(length, 60), at);
	}
	return 0;
}

static int ra_on_headers_complete(struct http_parser *parser)
{
	upgrade_status = parser->status_code;
	return 0;
}

static const struct http_parser_settings ra_parser_cb = {
	.on_header_field = ra_on_header_field,
	.on_header_value = ra_on_header_value,
	.on_headers_complete = ra_on_headers_complete,
};

static int wss_connect(int *http_fd_out)
{
	static char auth[1100];
	static char url[128];
	const char *headers[3] = {0};
	struct zsock_addrinfo hints = {.ai_family = AF_INET,
				       .ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *res;
	const char *jwt = spike_mqtt_jwt();
	bool mtls = IS_ENABLED(CONFIG_SPIKE_RA_AUTH_MTLS);
	sec_tag_t tags[] = {SPIKE_TAG_SERVER_CA, SPIKE_TAG_DEVICE};
	struct websocket_request req = {0};
	int fd, ws, ret, h = 0;

	ret = zsock_getaddrinfo(CONFIG_SPIKE_C8Y_HOST, "443", &hints, &res);
	if (ret) {
		return -EHOSTUNREACH;
	}
	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (fd < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}
	zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, tags,
			 (mtls ? 2 : 1) * sizeof(sec_tag_t));
	zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME,
			 CONFIG_SPIKE_C8Y_HOST, sizeof(CONFIG_SPIKE_C8Y_HOST));
	ret = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		ret = -errno;
		zsock_close(fd);
		return ret;
	}

	if (!mtls) {
		if (!jwt || !jwt[0]) {
			zsock_close(fd);
			return -EACCES;
		}
		snprintk(auth, sizeof(auth), "Authorization: Bearer %s\r\n", jwt);
		headers[h++] = auth;
	}
	headers[h++] = "Sec-WebSocket-Protocol: binary\r\n";

	snprintk(url, sizeof(url), "/service/remoteaccess/device/%s", conn_key);
	req.host = CONFIG_SPIKE_C8Y_HOST;
	req.url = url;
	req.optional_headers = headers;
	req.tmp_buf = ws_tmp;
	req.tmp_buf_len = sizeof(ws_tmp);
	req.http_cb = &ra_parser_cb;
	upgrade_status = 0;
	hdr_off = 0;
	upgrade_hdrs[0] = '\0';
	ws = websocket_connect(fd, &req, 15000, NULL);
	LOG_INF("RA upgrade: ws=%d HTTP %u; %s", ws, upgrade_status, upgrade_hdrs);
	if (ws < 0) {
		zsock_close(fd);
		return ws;
	}
	*http_fd_out = fd;
	return ws;
}

static int send_all(int fd, const uint8_t *p, size_t n)
{
	while (n > 0) {
		ssize_t w = zsock_send(fd, p, n, 0);

		if (w < 0) {
			return -errno;
		}
		p += w;
		n -= w;
	}
	return 0;
}

static void bridge(void *a, void *b, void *c)
{
	struct zsock_addrinfo hints = {.ai_family = AF_INET,
				       .ai_socktype = SOCK_STREAM};
	struct in_addr addr;
	char reason[96];
	uint64_t up = 0, down = 0;
	int64_t t0 = k_uptime_get(), t_tcp, t_up, last_activity, last_meas;
	int tcp = -1, ws = -1, http_fd = -1, ret;
	const char *why = "unknown";
	size_t cur, blocks, peak;

	/* Resolve (a name, or a numeric address). */
	if (zsock_inet_pton(AF_INET, target_host, &addr) != 1) {
		struct zsock_addrinfo *res;

		if (zsock_getaddrinfo(target_host, NULL, &hints, &res)) {
			post(SPIKE_RA_FAILED, "cannot resolve %s", target_host);
			goto out;
		}
		addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
		zsock_freeaddrinfo(res);
	}

	/* 6.2: policy before any socket. */
	if (policy_check(&addr, target_host, target_port, reason, sizeof(reason))) {
		LOG_WRN("RA %s", reason);
		post(SPIKE_RA_FAILED, "%s", reason);
		goto out;
	}

	/* 6.3: TCP to the target first, then the WebSocket to Cumulocity. */
	tcp = tcp_connect(&addr, target_port);
	t_tcp = k_uptime_get();
	if (tcp < 0) {
		post(SPIKE_RA_FAILED, "cannot connect to %s:%u (%d)", target_host,
		     target_port, tcp);
		goto out;
	}
	mbedtls_memory_buffer_alloc_max_reset();
	ws = wss_connect(&http_fd);
	t_up = k_uptime_get();
	if (ws < 0) {
		post(SPIKE_RA_FAILED, "remote-access WebSocket failed (%d, %s)", ws,
		     IS_ENABLED(CONFIG_SPIKE_RA_AUTH_MTLS) ? "client certificate only"
							   : "Bearer JWT");
		goto out;
	}
	mbedtls_memory_buffer_alloc_cur_get(&cur, &blocks);
	mbedtls_memory_buffer_alloc_max_get(&peak, &blocks);
	LOG_INF("MEAS ra: tunnel to %s:%u up; tcp %lld ms, wss %lld ms (%s); "
		"tls_heap cur=%zu peak=%zu (with MQTT)", target_host, target_port,
		t_tcp - t0, t_up - t_tcp,
		IS_ENABLED(CONFIG_SPIKE_RA_AUTH_MTLS) ? "client cert" : "Bearer JWT",
		cur, peak);
	post(SPIKE_RA_UP, "tunnel to %s:%u opened", target_host, target_port);

	/* 6.4: bridge. */
	last_activity = last_meas = k_uptime_get();
	for (;;) {
		struct zsock_pollfd fds[2] = {
			{.fd = tcp, .events = ZSOCK_POLLIN},
			{.fd = ws, .events = ZSOCK_POLLIN},
		};

		ret = zsock_poll(fds, 2, 1000);
		if (ret < 0) {
			why = "poll error";
			break;
		}
		if (fds[0].revents & ZSOCK_POLLIN) {
			ssize_t n = zsock_recv(tcp, to_ws, sizeof(to_ws), 0);

			if (n <= 0) {
				why = n == 0 ? "target closed" : "target error";
				break;
			}
			if (websocket_send_msg(ws, to_ws, n,
					       WEBSOCKET_OPCODE_DATA_BINARY, true,
					       true, 10000) < 0) {
				why = "WebSocket send failed";
				break;
			}
			up += n;
			last_activity = k_uptime_get();
		}
		if (fds[1].revents & ZSOCK_POLLIN) {
			uint32_t type = 0;
			uint64_t remaining = 1;
			bool closed = false;

			while (remaining > 0) {
				int n = websocket_recv_msg(ws, to_tcp, sizeof(to_tcp),
							   &type, &remaining, 0);

				if (n < 0) {
					if (n == -EAGAIN) {
						break;
					}
					closed = true;
					why = "WebSocket closed by Cumulocity";
					break;
				}
				if (type & WEBSOCKET_FLAG_CLOSE) {
					closed = true;
					why = "WebSocket close from Cumulocity";
					break;
				}
				if (n > 0 && send_all(tcp, to_tcp, n) < 0) {
					closed = true;
					why = "target send failed";
					break;
				}
				down += n;
			}
			if (closed) {
				break;
			}
			last_activity = k_uptime_get();
		}
		if ((fds[0].revents | fds[1].revents) & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
			why = (fds[0].revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR))
				      ? "target hung up"
				      : "WebSocket hung up";
			break;
		}
		if (CONFIG_SPIKE_RA_IDLE_TIMEOUT_S > 0 &&
		    k_uptime_get() - last_activity >
			    CONFIG_SPIKE_RA_IDLE_TIMEOUT_S * 1000LL) {
			why = "idle timeout";
			break;
		}
		if (k_uptime_get() - last_meas > 10000) {
			last_meas = k_uptime_get();
			mbedtls_memory_buffer_alloc_cur_get(&cur, &blocks);
			LOG_INF("MEAS ra: %llu B up, %llu B down, tls_heap cur=%zu",
				up, down, cur);
		}
	}

	{
		int64_t secs = MAX((k_uptime_get() - t_up) / 1000, 1);

		LOG_INF("MEAS ra: tunnel closed (%s) after %lld s: %llu B up, %llu B "
			"down (%llu KB/s down)", why, secs, up, down,
			down / 1024 / secs);
		post(SPIKE_RA_CLOSED, "tunnel to %s:%u closed (%s): %llu B up, %llu B "
		     "down", target_host, target_port, why, up, down);
	}
out:
	if (ws >= 0) {
		websocket_disconnect(ws);
	}
	if (http_fd >= 0) {
		zsock_close(http_fd);
	}
	if (tcp >= 0) {
		zsock_close(tcp);
	}
	atomic_clear(&busy);
}

int spike_ra_request(const char *msg, char *reason, size_t rlen)
{
	/* 530,<serial>,<host>,<port>,<connectionKey> */
	char buf[192];
	char *f[5] = {0};
	int n = 0;

	snprintk(buf, sizeof(buf), "%s", msg);
	for (char *p = buf; p && n < 5; n++) {
		f[n] = p;
		p = strchr(p, ',');
		if (p) {
			*p++ = '\0';
		}
	}
	if (n < 5 || strcmp(f[0], "530") != 0) {
		snprintk(reason, rlen, "malformed 530 message");
		return -EINVAL;
	}
	/* 6.1: log the fields, never the connection key. */
	LOG_INF("RA 530: serial=%s host=%s port=%s key=<%zu chars>", f[1], f[2],
		f[3], strlen(f[4]));

	if (!atomic_cas(&busy, 0, 1)) {
		snprintk(reason, rlen, "session limit reached (%d)",
			 CONFIG_SPIKE_RA_MAX_SESSIONS);
		return -EBUSY;
	}
	snprintk(target_host, sizeof(target_host), "%s", f[2]);
	target_port = (uint16_t)strtoul(f[3], NULL, 10);
	snprintk(conn_key, sizeof(conn_key), "%s", f[4]);
	k_thread_create(&ra_thread, ra_stack, K_THREAD_STACK_SIZEOF(ra_stack),
			bridge, NULL, NULL, NULL, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	k_thread_name_set(&ra_thread, "spike_ra");
	return 0;
}
