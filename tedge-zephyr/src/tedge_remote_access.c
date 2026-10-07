/* SPDX-License-Identifier: Apache-2.0
 *
 * Cloud remote access: the device bridges a Cumulocity session to a TCP
 * target, which may be the device itself or another host on its network.
 *
 *   530,<serial>,<host>,<port>,<key>   (on s/ds)
 *     -> resolve (".local" names over mDNS), retrying a lookup that times
 *        out, then check the target policy and the application's hook
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
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/random/random.h>
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
/* How long the bridge waits when nothing is moving. */
#define IDLE_POLL_MS 1000
/* And while bytes are flowing: straight back for more. Pacing this was
 * tried against the allocation failures a saturating session provokes and
 * made them worse, not better. What a tunnel actually runs out of is
 * receive buffers on the board — see
 * docs/debugging-against-real-devices.md — so the answer is
 * CONFIG_NET_BUF_DATA_SIZE and NET_BUF_RX_COUNT there, not a throttle
 * here. */
#define ACTIVE_POLL_MS 0

#define RESOLVE_TIMEOUT_MS  CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_TIMEOUT_MS
#define RESOLVE_ATTEMPTS    CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_ATTEMPTS
#define RESOLVE_RETRY_MS    CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_RETRY_DELAY_MS
/* The seat is held while a name resolves; keep the worst case bounded. */
BUILD_ASSERT(RESOLVE_ATTEMPTS * RESOLVE_TIMEOUT_MS +
		     (RESOLVE_ATTEMPTS - 1) * RESOLVE_RETRY_MS <= 30000,
	     "remote-access name resolution could hold a seat for over 30 s");

struct session {
	char host[64];
	char addr_s[NET_IPV4_ADDR_LEN]; /* what host resolved to */
	uint16_t port;
	/* One lookup at a time, filled in by the resolver's callback. */
	struct k_sem resolved;
	int resolve_status;
	struct in_addr answer;
	bool have_answer;
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

/* Returns 0 when the target may be contacted, or -EACCES with a reason.
 * @p target is the target as events name it. */
static int policy_check(const struct in_addr *addr, const char *host,
			const char *target, uint16_t port, char *reason,
			size_t rlen)
{
	struct net_if *iface = net_if_get_default();
	const struct tedge_hooks *hooks = tedge_hook_table();
	enum tedge_ra_policy policy =
		IS_ENABLED(CONFIG_TEDGE_REMOTE_ACCESS_TARGETS_LOCAL)
			? TEDGE_RA_POLICY_LOCAL
		: IS_ENABLED(CONFIG_TEDGE_REMOTE_ACCESS_TARGETS_LIST)
			? TEDGE_RA_POLICY_LIST
			: TEDGE_RA_POLICY_LAN;
	struct tedge_ra_facts facts = {
		.loopback = (ntohl(addr->s_addr) >> 24) == 127,
		.own = net_if_ipv4_addr_lookup(addr, NULL) != NULL,
		.on_subnet = iface != NULL &&
			     net_if_ipv4_addr_mask_cmp(iface, addr),
		.mdns = tedge_ra_is_mdns_name(host),
	};

#if defined(CONFIG_TEDGE_REMOTE_ACCESS_TARGETS_LIST)
	facts.in_list = tedge_ra_in_allow_list(
		CONFIG_TEDGE_REMOTE_ACCESS_ALLOW_LIST, host, port);
#endif

	switch (tedge_ra_policy_decide(policy, &facts)) {
	case TEDGE_RA_ALLOWED:
		break;
	case TEDGE_RA_REFUSED_OFF_LINK:
		snprintf(reason, rlen,
			 "target %s refused: an mDNS answer must be on this "
			 "device's network", target);
		return -EACCES;
	default:
		snprintf(reason, rlen, "target %s refused: %s", target,
			 policy == TEDGE_RA_POLICY_LOCAL ? "this device only"
			 : policy == TEDGE_RA_POLICY_LIST
				 ? "not in the allow-list"
				 : "not on this device's network");
		return -EACCES;
	}

	/* The application may narrow the policy, never widen it. */
	if (hooks != NULL && hooks->remote_access_allow != NULL) {
		struct sockaddr_in sa = { .sin_family = AF_INET,
					  .sin_port = htons(port),
					  .sin_addr = *addr };
		struct tedge_remote_target t = {
			.addr = (const struct sockaddr *)(const void *)&sa,
			.port = port,
		};

		if (!hooks->remote_access_allow(&t, hooks->user_data)) {
			snprintf(reason, rlen,
				 "target %s refused by the application",
				 target);
			return -EACCES;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Resolving the target                                                      */
/* ------------------------------------------------------------------------ */

#if defined(CONFIG_TEDGE_REMOTE_ACCESS_MDNS)
/* One mDNS lookup of s->host: 0 with s->answer, or a DNS_EAI_* code.
 *
 * Not Zephyr's resolver (4.4): it has an mDNS server only when the
 * application sets static DNS servers, and when the application also runs
 * the mDNS responder it sends from a socket it never reads, while
 * responders answer such a query to the port it came from (RFC 6762 6.7).
 * So the query goes out from a socket of our own, which reads the answer
 * itself: the same "legacy unicast" exchange, whatever else is running. */
static int mdns_once(struct session *s)
{
	struct sockaddr_in group = { .sin_family = AF_INET,
				     .sin_port = htons(5353) };
	uint16_t id = sys_rand16_get();
	int64_t deadline = k_uptime_get() + RESOLVE_TIMEOUT_MS;
	uint8_t buf[512];
	int fd, n, status = DNS_EAI_CANCELED;

	(void)zsock_inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
	n = tedge_mdns_build_query(buf, sizeof(buf), id, s->host);
	if (n < 0) {
		return DNS_EAI_NONAME;
	}
	fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		return DNS_EAI_SYSTEM;
	}
	if (zsock_sendto(fd, buf, n, 0, (struct sockaddr *)&group,
			 sizeof(group)) < 0) {
		zsock_close(fd);
		return DNS_EAI_SYSTEM;
	}

	/* Every responder that knows the name answers; take the first one
	 * that answers this query, and ignore anything else. */
	for (int64_t left; (left = deadline - k_uptime_get()) > 0;) {
		struct zsock_pollfd pfd = { .fd = fd, .events = ZSOCK_POLLIN };
		uint8_t a4[4];

		if (zsock_poll(&pfd, 1, (int)left) <= 0) {
			break;
		}
		n = zsock_recv(fd, buf, sizeof(buf), ZSOCK_MSG_DONTWAIT);
		if (n <= 0) {
			continue;
		}
		if (tedge_mdns_parse_a(buf, n, id, s->host, a4) == 0) {
			memcpy(&s->answer, a4, sizeof(a4));
			status = 0;
			break;
		}
	}
	zsock_close(fd);
	return status;
}
#endif

static void resolve_cb(enum dns_resolve_status status,
		       struct dns_addrinfo *info, void *user_data)
{
	struct session *s = user_data;

	if (status == DNS_EAI_INPROGRESS && info != NULL) {
		if (!s->have_answer && info->ai_family == AF_INET) {
			s->answer = ((struct sockaddr_in *)&info->ai_addr)
					    ->sin_addr;
			s->have_answer = true;
		}
		return;
	}
	s->resolve_status = (status == DNS_EAI_ALLDONE && s->have_answer)
				    ? 0
				    : (int)status;
	k_sem_give(&s->resolved);
}

/* One lookup: 0 with s->answer, or a DNS_EAI_* code. */
static int resolve_once(struct session *s)
{
	uint16_t id = 0;
	int ret;

#if defined(CONFIG_TEDGE_REMOTE_ACCESS_MDNS)
	if (tedge_ra_is_mdns_name(s->host)) {
		return mdns_once(s);
	}
#endif
	k_sem_reset(&s->resolved);
	s->have_answer = false;
	s->resolve_status = DNS_EAI_CANCELED;

	ret = dns_get_addr_info(s->host, DNS_QUERY_TYPE_A, &id, resolve_cb, s,
				RESOLVE_TIMEOUT_MS);
	if (ret < 0) {
		/* An errno: no server to send to, or a query slot in use (one
		 * at a time by default). getaddrinfo() calls this a system
		 * error too. */
		return DNS_EAI_SYSTEM;
	}
	/* The resolver times the query out itself and calls back with
	 * DNS_EAI_CANCELED; the margin covers a callback that never comes,
	 * and cancelling then makes sure none arrives later. */
	if (k_sem_take(&s->resolved, K_MSEC(RESOLVE_TIMEOUT_MS + 500)) != 0) {
		(void)dns_cancel_addr_info_with_name(s->host, DNS_QUERY_TYPE_A,
						     id);
		return DNS_EAI_CANCELED;
	}
	return s->resolve_status;
}

/* Resolve s->host into @p addr, retrying what is worth retrying. Returns 0,
 * or -EHOSTUNREACH with the reason in @p reason. */
static int resolve_target(struct session *s, struct in_addr *addr,
			  char *reason, size_t rlen)
{
	bool mdns = tedge_ra_is_mdns_name(s->host);
	int status = DNS_EAI_FAIL;
	int attempt;

	if (zsock_inet_pton(AF_INET, s->host, addr) == 1) {
		s->addr_s[0] = '\0';
		return 0;
	}
	if (mdns && !IS_ENABLED(CONFIG_TEDGE_REMOTE_ACCESS_MDNS)) {
		snprintf(reason, rlen, "cannot resolve %s: mDNS is not built in",
			 s->host);
		return -EHOSTUNREACH;
	}

	for (attempt = 1; attempt <= RESOLVE_ATTEMPTS; attempt++) {
		if (attempt > 1) {
			k_msleep(RESOLVE_RETRY_MS);
		}
		status = resolve_once(s);
		if (status == 0) {
			*addr = s->answer;
			(void)zsock_inet_ntop(AF_INET, addr, s->addr_s,
					      sizeof(s->addr_s));
			return 0;
		}
		LOG_WRN("remote access: lookup %d/%d of %s failed (%d)",
			attempt, RESOLVE_ATTEMPTS, s->host, status);
		if (!tedge_ra_resolve_retryable(status)) {
			break;
		}
	}
	attempt = MIN(attempt, RESOLVE_ATTEMPTS);

	if (status == DNS_EAI_CANCELED || status == DNS_EAI_AGAIN) {
		snprintf(reason, rlen, "%s did not answer%s after %d attempt%s",
			 s->host, mdns ? " over mDNS" : "", attempt,
			 attempt == 1 ? "" : "s");
	} else {
		snprintf(reason, rlen, "cannot resolve %s after %d attempt%s (%d)",
			 s->host, attempt, attempt == 1 ? "" : "s", status);
	}
	return -EHOSTUNREACH;
}

/* ------------------------------------------------------------------------ */
/* Sockets                                                                   */
/* ------------------------------------------------------------------------ */

/* Turn off Nagle. Best effort: a transport that does not support it is no
 * worse off than before. */
static void set_nodelay(int fd)
{
	int on = 1;

	(void)zsock_setsockopt(fd, IPPROTO_TCP, ZSOCK_TCP_NODELAY, &on,
			       sizeof(on));
}

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
	/* An interactive session is a stream of small writes — keystrokes one
	 * way, a shell's or a TUI's output the other. Nagle holds each of
	 * them until the previous segment is acknowledged, and against a peer
	 * doing delayed ACK that is tens to hundreds of milliseconds per
	 * exchange. Line-at-a-time output survives it; anything that repaints
	 * a screen does not. */
	set_nodelay(fd);
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
	set_nodelay(fd);
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
	struct in_addr addr;
	char reason[160], target[96];
	uint64_t up = 0, down = 0;
	int64_t t_up = 0, last_activity;
	int tcp = -1, ws = -1, http_fd = -1, ret;
	const char *why = "unknown";

	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (resolve_target(s, &addr, reason, sizeof(reason)) != 0) {
		LOG_WRN("remote access: %s", reason);
		post(TEDGE_RA_FAILED, "%s", reason);
		goto out;
	}
	(void)tedge_ra_target_str(target, sizeof(target), s->host, s->addr_s,
				  s->port);

	/* The policy decides before any socket is opened. */
	if (policy_check(&addr, s->host, target, s->port, reason,
			 sizeof(reason)) != 0) {
		LOG_WRN("%s", reason);
		post(TEDGE_RA_FAILED, "%s", reason);
		goto out;
	}

	tcp = tcp_connect(&addr, s->port);
	if (tcp < 0) {
		post(TEDGE_RA_FAILED, "cannot reach %s (%d)", target, tcp);
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
	LOG_INF("remote access: tunnel to %s is up", target);
	post(TEDGE_RA_UP, TEDGE_RA_OPENED_FMT, target);

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
	/* While bytes are flowing, come straight back for more instead of
	 * waiting on the next poll: a second between buffers caps the tunnel
	 * at one BUF_SIZE per poll (~2 KB/s at the default 2048), which is
	 * enough for a command and its output but not for anything that
	 * repaints a screen. The long wait is only for an idle session; a
	 * busy one paces at ACTIVE_POLL_MS instead. */
	for (int poll_ms = IDLE_POLL_MS;;) {
		struct zsock_pollfd fds[2] = {
			{ .fd = tcp, .events = ZSOCK_POLLIN },
			{ .fd = ws, .events = ZSOCK_POLLIN },
		};
		bool moved = false;

		ret = zsock_poll(fds, 2, poll_ms);
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
			moved = true;
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
			moved = true;
			last_activity = k_uptime_get();
		}
		poll_ms = moved ? ACTIVE_POLL_MS : IDLE_POLL_MS;
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

		LOG_INF("remote access: tunnel to %s ended (%s) after %lld s: "
			"%llu B up, %llu B down", target, why, secs, up, down);
		post(TEDGE_RA_CLOSED, TEDGE_RA_CLOSED_FMT, target, why, up,
		     down);
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
	s->addr_s[0] = '\0';
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

	/* An operator can close the door on a device already in the field,
	 * through the "tedge" parameter set, without reflashing it. Checked
	 * before anything is parsed: a refusal should cost nothing. */
#if defined(CONFIG_TEDGE_PARAMETERS_SELF)
	if (!tedge_self_remote_access_allowed()) {
		snprintf(reason, rlen,
			 "remote access is turned off on this device");
		LOG_WRN("remote access: refused, turned off by parameter");
		return -EACCES;
	}
#endif

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
	s->addr_s[0] = '\0';
	k_sem_init(&s->resolved, 0, 1);
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
