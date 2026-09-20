/* SPDX-License-Identifier: Apache-2.0
 *
 * Downloading a file over HTTP(S) into a sink the caller provides.
 *
 * Three rules make this fiddly enough to keep in one place, away from the
 * features that use it (firmware update now; log upload and configuration
 * later):
 *
 *   - Zephyr's HTTP client reports chunked bodies wrongly in its response
 *     callback: one receive buffer can hold several segments, and the client
 *     keeps the first segment's start with the last segment's length.
 *     Cumulocity serves binaries chunked, so every byte is written from the
 *     parser's own on_body callback instead. See
 *     docs/zephyr-http-chunked-body-bug.md; remove the workaround once it is
 *     fixed upstream.
 *   - Zephyr's HTTP client does not follow redirects, and a release asset
 *     can answer with a Location header of nearly a kilobyte.
 *   - The cloud token must reach the tenant and nowhere else, including
 *     after a redirect.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <errno.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define RX_SIZE  1024
struct download_state {
	struct tedge_download *req;
	int status;
	int64_t written;
	int rc; /* a sink error, kept so the transfer can stop */
};

static struct download_state *current;

/* ------------------------------------------------------------------------ */
/* HTTP callbacks                                                            */
/* ------------------------------------------------------------------------ */

static int on_body(struct http_parser *parser, const char *at, size_t length)
{
	struct download_state *st = current;

	ARG_UNUSED(parser);
	if (st == NULL || st->rc != 0 || length == 0) {
		return 0;
	}
	st->rc = st->req->sink(at, length, st->req->user_data);
	if (st->rc != 0) {
		return -1; /* stop the parser */
	}
	st->written += length;
	if (st->req->progress != NULL) {
		st->req->progress(st->written, st->req->user_data);
	}
	return 0;
}

static const struct http_parser_settings parser_cb = { .on_body = on_body };

static int on_response(struct http_response *rsp, enum http_final_call final,
		       void *user_data)
{
	struct download_state *st = user_data;

	ARG_UNUSED(final);
	st->status = rsp->http_status_code;
	/* Chunked transfers have no length; progress then has no percentage. */
	if (rsp->content_length > 0) {
		st->req->total = rsp->content_length;
	}
	return 0;
}

/* Headers worth keeping: a redirect's target, and the length a
 * "Content-Range: bytes 0-887801/887802" reply states. */
static char redirect_to[TEDGE_URL_MAX];
static enum { HDR_OTHER, HDR_LOCATION, HDR_CONTENT_RANGE } header_kind;

static int on_header_field(struct http_parser *parser, const char *at,
			   size_t length)
{
	ARG_UNUSED(parser);
	if (length == 8 && strncasecmp(at, "location", 8) == 0) {
		header_kind = HDR_LOCATION;
	} else if (length == 13 && strncasecmp(at, "content-range", 13) == 0) {
		header_kind = HDR_CONTENT_RANGE;
	} else {
		header_kind = HDR_OTHER;
	}
	return 0;
}

static int on_header_value(struct http_parser *parser, const char *at,
			   size_t length)
{
	ARG_UNUSED(parser);
	if (header_kind == HDR_LOCATION && length < sizeof(redirect_to)) {
		memcpy(redirect_to, at, length);
		redirect_to[length] = '\0';
	} else if (header_kind == HDR_CONTENT_RANGE && current != NULL) {
		/* "bytes <first>-<last>/<total>"; anything else is ignored. */
		char value[64];
		const char *slash;

		if (length < sizeof(value)) {
			memcpy(value, at, length);
			value[length] = '\0';
			slash = strrchr(value, '/');
			if (slash != NULL && slash[1] != '*') {
				int64_t total = strtoll(slash + 1, NULL, 10);

				if (total > 0) {
					current->req->total = total;
				}
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* One request                                                               */
/* ------------------------------------------------------------------------ */

static int fetch_once(const char *url, struct download_state *st,
		      bool *redirected)
{
	static const sec_tag_t tags[] = { TEDGE_TAG_SERVER_CA };
	struct http_parser_settings cb = parser_cb;
	struct zsock_addrinfo hints = { .ai_family = AF_INET,
					.ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res = NULL;
	struct http_request req = { 0 };
	const char *headers[3] = { 0 };
	static uint8_t rx[RX_SIZE];
	char auth[1100];
	char host[TEDGE_HOST_MAX];
	char port_s[8];
	const char *path;
	uint16_t port;
	bool tls;
	int fd = -1, ret, h = 0;

	*redirected = false;
	ret = tedge_url_split(url, &tls, host, sizeof(host), &port, &path);
	if (ret != 0) {
		LOG_ERR("download: cannot parse the URL");
		return ret;
	}
	snprintf(port_s, sizeof(port_s), "%u", port);
	ret = zsock_getaddrinfo(host, port_s, &hints, &res);
	if (ret != 0) {
		LOG_ERR("download: cannot resolve %s (%d)", host, ret);
		return -EHOSTUNREACH;
	}
	fd = zsock_socket(AF_INET, SOCK_STREAM,
			  tls ? IPPROTO_TLS_1_2 : IPPROTO_TCP);
	if (fd < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}
	if (tls) {
		(void)zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST,
				       tags, sizeof(tags));
		(void)zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME,
				       host, strlen(host) + 1);
		/* A host outside the tenant may present a chain that takes
		 * ~13 s to verify in software; the handshake budget is
		 * CONFIG_NET_SOCKETS_TLS_CONNECT_TIMEOUT, which the README
		 * asks applications to raise. */
	}
	ret = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("download: cannot connect to %s (%d)", host, ret);
		zsock_close(fd);
		return ret;
	}

	/* Ask for the whole file as a range: a server that supports ranges
	 * answers 206 with "Content-Range: bytes 0-<last>/<total>", which is
	 * the only way to learn the size of a chunked transfer (Cumulocity
	 * serves binaries chunked). A server that ignores it answers 200 as
	 * usual. */
	headers[h++] = "Range: bytes=0-\r\n";

	/* The token is for the tenant only, never a redirect target. */
	if (st->req->token != NULL && st->req->token[0] != '\0' &&
	    tedge_url_is_tenant(host)) {
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n",
			 st->req->token);
		headers[h++] = auth;
	}

	cb.on_header_field = on_header_field;
	cb.on_header_value = on_header_value;
	redirect_to[0] = '\0';
	header_kind = HDR_OTHER;
	st->status = 0;

	req.method = HTTP_GET;
	req.url = path;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.header_fields = (h > 0) ? headers : NULL;
	req.response = on_response;
	req.http_cb = &cb;
	req.recv_buf = rx;
	req.recv_buf_len = sizeof(rx);

	current = st;
	ret = http_client_req(fd, &req, st->req->timeout_ms, st);
	current = NULL;
	memset(auth, 0, sizeof(auth));
	zsock_close(fd);

	if (st->rc != 0) {
		return st->rc; /* the sink refused */
	}
	if (ret < 0) {
		LOG_ERR("download: the transfer failed (%d)", ret);
		return ret;
	}
	if (st->status >= 300 && st->status < 400 && redirect_to[0] != '\0') {
		*redirected = true;
		return 0;
	}
	if (st->status != 200 && st->status != 206) {
		LOG_ERR("download: the server answered %d", st->status);
		return -EIO;
	}
	return 0;
}

int tedge_download(struct tedge_download *req)
{
	struct download_state st = { .req = req };
	char url[TEDGE_URL_MAX];
	char next[TEDGE_URL_MAX];
	int hops = 0;

	if (req == NULL || req->url == NULL || req->sink == NULL) {
		return -EINVAL;
	}
	if (snprintf(url, sizeof(url), "%s", req->url) >= (int)sizeof(url)) {
		return -ENOSPC;
	}
	if (req->timeout_ms == 0) {
		req->timeout_ms = 60000;
	}

	for (;;) {
		bool redirected;
		int ret = fetch_once(url, &st, &redirected);

		if (ret != 0) {
			return ret;
		}
		if (!redirected) {
			break;
		}
		if (++hops > CONFIG_TEDGE_FIRMWARE_MAX_REDIRECTS) {
			LOG_ERR("download: too many redirects");
			return -ELOOP;
		}
		ret = tedge_url_resolve(url, redirect_to, next, sizeof(next));
		if (ret != 0) {
			return ret;
		}
		memcpy(url, next, sizeof(url));
		LOG_INF("download: following a redirect (hop %d)", hops);
	}
	req->written = st.written;
	return 0;
}
