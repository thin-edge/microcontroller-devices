/* SPDX-License-Identifier: Apache-2.0
 *
 * Cloud remote access: the device bridges a Cumulocity session to a TCP
 * target, which may be the device itself or another host on its network.
 *
 *   530,<serial>,<host>,<port>,<key>   (on s/ds)
 *     -> resolve, check the target policy and the application's hook
 *     -> take a seat (CONFIG_TEDGE_REMOTE_ACCESS_MAX_SESSIONS)
 *     -> TCP to <host>:<port>
 *     -> wss://<tenant>/service/remoteaccess/device/<key>
 *        (Authorization: Bearer <token>, Sec-WebSocket-Protocol: binary)
 *     -> copy bytes both ways until either side closes or it goes idle
 *
 * Each session runs on its own thread: the bridge blocks in poll(), and the
 * client thread owns the MQTT session. Results travel back through a message
 * queue, because Zephyr's MQTT client is not thread-safe.
 *
 * The connection key is a bearer credential for the session: it is never
 * logged, at any level.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/websocket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define MAX_SESSIONS CONFIG_TEDGE_REMOTE_ACCESS_MAX_SESSIONS
#define BUF_SIZE     CONFIG_TEDGE_REMOTE_ACCESS_BUF_SIZE
#define TELNET_PORT  23

struct session {
	char host[64];
	uint16_t port;
	char key[48]; /* the connection key; never logged */
	atomic_t busy;
	time_t since; /* wall clock while the tunnel is up, else 0 */
	struct k_thread thread;
	uint8_t to_ws[BUF_SIZE];
	uint8_t to_target[BUF_SIZE];
	uint8_t ws_scratch[1024];
};

static struct session sessions[MAX_SESSIONS];
static K_THREAD_STACK_ARRAY_DEFINE(session_stacks, MAX_SESSIONS,
				   CONFIG_TEDGE_REMOTE_ACCESS_STACK_SIZE);

K_MSGQ_DEFINE(ra_events, sizeof(struct tedge_ra_event), MAX_SESSIONS * 2, 4);

static void post(enum tedge_ra_event_type type, const char *fmt, ...)
{
	struct tedge_ra_event ev = { .type = type };
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
	va_end(ap);
	(void)k_msgq_put(&ra_events, &ev, K_NO_WAIT);
}

int tedge_ra_poll_event(struct tedge_ra_event *ev)
{
	return k_msgq_get(&ra_events, ev, K_NO_WAIT);
}

/* ------------------------------------------------------------------------ */
/* Target policy                                                             */
/* ------------------------------------------------------------------------ */

/* Returns 0 when the target may be contacted, or -EACCES with a reason. */
static int policy_check(const struct in_addr *addr, const char *host,
			uint16_t port, char *reason, size_t rlen)
{
	struct net_if *iface = net_if_get_default();
	bool loopback = (ntohl(addr->s_addr) >> 24) == 127;
	bool own = net_if_ipv4_addr_lookup(addr, NULL) != NULL;
	const struct tedge_hooks *hooks = tedge_hook_table();
	bool allowed;

	if (IS_ENABLED(CONFIG_TEDGE_REMOTE_ACCESS_TARGETS_LOCAL)) {
		allowed = loopback || own;
		if (!allowed) {
			snprintf(reason, rlen,
				 "target %s:%u refused: this device only", host,
				 port);
		}
	} else if (IS_ENABLED(CONFIG_TEDGE_REMOTE_ACCESS_TARGETS_LIST)) {
		allowed = tedge_ra_in_allow_list(
			CONFIG_TEDGE_REMOTE_ACCESS_ALLOW_LIST, host, port);
		if (!allowed) {
			snprintf(reason, rlen,
				 "target %s:%u refused: not in the allow-list",
				 host, port);
		}
	} else { /* the device's own IPv4 subnets */
		allowed = loopback || own ||
			  (iface != NULL &&
			   net_if_ipv4_addr_mask_cmp(iface, addr));
		if (!allowed) {
			snprintf(reason, rlen,
				 "target %s:%u refused: not on this device's "
				 "network", host, port);
		}
	}
	if (!allowed) {
		return -EACCES;
	}

	/* The application may narrow the policy, never widen it. */
	if (hooks != NULL && hooks->remote_access_allow != NULL) {
		struct sockaddr_in sa = { .sin_family = AF_INET,
					  .sin_port = htons(port),
					  .sin_addr = *addr };
		struct tedge_remote_target target = {
			.addr = (const struct sockaddr *)(const void *)&sa,
			.port = port,
		};

		if (!hooks->remote_access_allow(&target, hooks->user_data)) {
			snprintf(reason, rlen,
				 "target %s:%u refused by the application", host,
				 port);
			return -EACCES;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Sockets                                                                   */
/* ------------------------------------------------------------------------ */

static int tcp_connect(const struct in_addr *addr, uint16_t port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_port = htons(port),
				  .sin_addr = *addr };
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

/* The device-side WebSocket. Cumulocity refuses the client certificate alone
 * (401), so this needs the token the client keeps for the tenant.
 */
static int wss_connect(struct session *s, int *http_fd_out)
{
	static const sec_tag_t tags[] = { TEDGE_TAG_SERVER_CA, TEDGE_TAG_DEVICE };
	struct zsock_addrinfo hints = { .ai_family = AF_INET,
					.ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res;
	struct websocket_request req = { 0 };
	const char *host = tedge_c8y_host();
	const char *token = tedge_c8y_jwt();
	const char *headers[3] = { 0 };
	char auth[1100];
	char url[128];
	int fd, ws, ret, h = 0;

	if (token == NULL || token[0] == '\0') {
		return -EACCES;
	}
	ret = zsock_getaddrinfo(host, "443", &hints, &res);
	if (ret != 0) {
		return -EHOSTUNREACH;
	}
	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (fd < 0) {
		LOG_ERR("remote access: no TLS socket (%d); raise "
			"CONFIG_NET_SOCKETS_TLS_MAX_CONTEXTS", errno);
		zsock_freeaddrinfo(res);
		return -errno;
	}
	(void)zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, tags,
			       sizeof(tags));
	(void)zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME, host,
			       strlen(host) + 1);
	ret = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("remote access: TLS to %s failed (%d)", host, ret);
		zsock_close(fd);
		return ret;
	}

	snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", token);
	headers[h++] = auth;
	headers[h++] = "Sec-WebSocket-Protocol: binary\r\n";
	snprintf(url, sizeof(url), "/service/remoteaccess/device/%s", s->key);

	req.host = host;
	req.url = url;
	req.optional_headers = headers;
	req.tmp_buf = s->ws_scratch;
	req.tmp_buf_len = sizeof(s->ws_scratch);
	ws = websocket_connect(fd, &req, 15000, NULL);
	memset(auth, 0, sizeof(auth));
	memset(url, 0, sizeof(url));
	if (ws < 0) {
		LOG_ERR("remote access: the cloud refused the WebSocket (%d)", ws);
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

/* ------------------------------------------------------------------------ */
/* The bridge                                                                */
/* ------------------------------------------------------------------------ */

static void bridge(void *a, void *b, void *c)
{
	struct session *s = a;
	struct zsock_addrinfo hints = { .ai_family = AF_INET,
					.ai_socktype = SOCK_STREAM };
	struct in_addr addr;
	char reason[112];
	uint64_t up = 0, down = 0;
	int64_t t_up = 0, last_activity;
	int tcp = -1, ws = -1, http_fd = -1, ret;
	const char *why = "unknown";

	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (zsock_inet_pton(AF_INET, s->host, &addr) != 1) {
		struct zsock_addrinfo *res;

		if (zsock_getaddrinfo(s->host, NULL, &hints, &res) != 0) {
			post(TEDGE_RA_FAILED, "cannot resolve %s", s->host);
			goto out;
		}
		addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
		zsock_freeaddrinfo(res);
	}

	/* The policy decides before any socket is opened. */
	if (policy_check(&addr, s->host, s->port, reason, sizeof(reason)) != 0) {
		LOG_WRN("%s", reason);
		post(TEDGE_RA_FAILED, "%s", reason);
		goto out;
	}

	tcp = tcp_connect(&addr, s->port);
	if (tcp < 0) {
		post(TEDGE_RA_FAILED, "cannot reach %s:%u (%d)", s->host, s->port,
		     tcp);
		goto out;
	}
	ws = wss_connect(s, &http_fd);
	if (ws < 0) {
		post(TEDGE_RA_FAILED, "the cloud connection failed (%d)", ws);
		goto out;
	}

	{
		struct timespec ts;

		(void)sys_clock_gettime(SYS_CLOCK_REALTIME, &ts);
		s->since = ts.tv_sec;
	}
	t_up = k_uptime_get();
	LOG_INF("remote access: tunnel to %s:%u is up", s->host, s->port);
	post(TEDGE_RA_UP, "tunnel to %s:%u opened", s->host, s->port);

	/* Zephyr's telnet backend turns echo off and never offers it, and the
	 * cloud's terminal waits for the server: without this nobody echoes. */
	if (s->port == TELNET_PORT) {
		static const uint8_t offer[] = { 0xFF, 0xFB, 0x01,
						 0xFF, 0xFB, 0x03 };

		(void)websocket_send_msg(ws, offer, sizeof(offer),
					 WEBSOCKET_OPCODE_DATA_BINARY, true, true,
					 5000);
	}

	last_activity = k_uptime_get();
	for (;;) {
		struct zsock_pollfd fds[2] = {
			{ .fd = tcp, .events = ZSOCK_POLLIN },
			{ .fd = ws, .events = ZSOCK_POLLIN },
		};

		ret = zsock_poll(fds, 2, 1000);
		if (ret < 0) {
			why = "poll error";
			break;
		}
		if (fds[0].revents & ZSOCK_POLLIN) {
			ssize_t n = zsock_recv(tcp, s->to_ws, BUF_SIZE, 0);

			if (n <= 0) {
				why = (n == 0) ? "the target closed"
					       : "the target failed";
				break;
			}
			if (websocket_send_msg(ws, s->to_ws, n,
					       WEBSOCKET_OPCODE_DATA_BINARY, true,
					       true, 10000) < 0) {
				why = "sending to the cloud failed";
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
				int n = websocket_recv_msg(ws, s->to_target,
							   BUF_SIZE, &type,
							   &remaining, 0);

				if (n < 0) {
					if (n == -EAGAIN) {
						break;
					}
					closed = true;
					why = "the cloud connection dropped";
					break;
				}
				if (type & WEBSOCKET_FLAG_CLOSE) {
					closed = true;
					why = "the client closed the session";
					break;
				}
				if (n > 0 && send_all(tcp, s->to_target, n) < 0) {
					closed = true;
					why = "sending to the target failed";
					break;
				}
				down += n;
			}
			if (closed) {
				break;
			}
			last_activity = k_uptime_get();
		}
		if ((fds[0].revents | fds[1].revents) &
		    (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
			why = (fds[0].revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR))
				      ? "the target hung up"
				      : "the cloud connection hung up";
			break;
		}
		if (CONFIG_TEDGE_REMOTE_ACCESS_IDLE_TIMEOUT_S > 0 &&
		    k_uptime_get() - last_activity >
			    CONFIG_TEDGE_REMOTE_ACCESS_IDLE_TIMEOUT_S * 1000LL) {
			why = "idle timeout";
			break;
		}
	}

	{
		int64_t secs = MAX((k_uptime_get() - t_up) / 1000, 1);

		LOG_INF("remote access: tunnel to %s:%u ended (%s) after %lld s: "
			"%llu B up, %llu B down", s->host, s->port, why, secs, up,
			down);
		post(TEDGE_RA_CLOSED,
		     "tunnel to %s:%u closed (%s): %llu B up, %llu B down",
		     s->host, s->port, why, up, down);
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
	s->since = 0;
	memset(s->key, 0, sizeof(s->key));
	atomic_clear(&s->busy);
}

/* ------------------------------------------------------------------------ */
/* Requests and capacity                                                     */
/* ------------------------------------------------------------------------ */

int tedge_ra_request(const char *line, char *reason, size_t rlen)
{
	char host[64], port_s[8], key[48];
	struct session *s = NULL;
	int idx = 0;

	/* 530,<serial>,<host>,<port>,<connectionKey> */
	if (tedge_sr_field(line, 2, host, sizeof(host)) <= 0 ||
	    tedge_sr_field(line, 3, port_s, sizeof(port_s)) <= 0 ||
	    tedge_sr_field(line, 4, key, sizeof(key)) <= 0) {
		snprintf(reason, rlen, "malformed remote-access request");
		return -EINVAL;
	}
	LOG_INF("remote access: request for %s:%s", host, port_s);

	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (atomic_cas(&sessions[i].busy, 0, 1)) {
			s = &sessions[i];
			idx = i;
			break;
		}
	}
	if (s == NULL) {
		snprintf(reason, rlen, "no free session (the limit is %d)",
			 MAX_SESSIONS);
		return -EBUSY;
	}

	snprintf(s->host, sizeof(s->host), "%s", host);
	s->port = (uint16_t)strtoul(port_s, NULL, 10);
	snprintf(s->key, sizeof(s->key), "%s", key);
	memset(key, 0, sizeof(key));

	k_thread_create(&s->thread, session_stacks[idx],
			K_THREAD_STACK_SIZEOF(session_stacks[idx]), bridge, s,
			NULL, NULL,
			K_PRIO_PREEMPT(CONFIG_TEDGE_THREAD_PRIORITY), 0,
			K_NO_WAIT);
	k_thread_name_set(&s->thread, "tedge_ra");
	return 0;
}

int tedge_ra_twin(char *buf, size_t len)
{
	int taken = 0, active = 0;
	const char *policy =
		IS_ENABLED(CONFIG_TEDGE_REMOTE_ACCESS_TARGETS_LOCAL)  ? "local"
		: IS_ENABLED(CONFIG_TEDGE_REMOTE_ACCESS_TARGETS_LIST) ? "list"
								      : "lan";
	int n;

	for (int i = 0; i < MAX_SESSIONS; i++) {
		taken += atomic_get(&sessions[i].busy) ? 1 : 0;
		active += sessions[i].since ? 1 : 0;
	}
	n = snprintf(buf, len,
		     "{\"maxSessions\":%d,\"activeSessions\":%d,"
		     "\"freeSessions\":%d,\"policy\":\"%s\"",
		     MAX_SESSIONS, active, MAX_SESSIONS - taken, policy);

#if defined(CONFIG_TEDGE_REMOTE_ACCESS_TWIN_SESSIONS)
	n += snprintf(buf + n, (n < (int)len) ? len - n : 0, ",\"sessions\":[");
	for (int i = 0, printed = 0; i < MAX_SESSIONS && n < (int)len; i++) {
		struct tm tm;
		char iso[24];
		time_t since = sessions[i].since;

		if (since == 0) {
			continue;
		}
		gmtime_r(&since, &tm);
		strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
		n += snprintf(buf + n, len - n,
			      "%s{\"target\":\"%s:%u\",\"since\":\"%s\"}",
			      printed++ ? "," : "", sessions[i].host,
			      sessions[i].port, iso);
	}
	if (n < (int)len) {
		n += snprintf(buf + n, len - n, "]");
	}
#endif
	if (n < (int)len) {
		n += snprintf(buf + n, len - n, "}");
	}
	return (n < (int)len) ? 0 : -ENOSPC;
}
