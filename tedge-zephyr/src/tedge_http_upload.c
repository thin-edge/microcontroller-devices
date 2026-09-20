/* SPDX-License-Identifier: Apache-2.0
 *
 * Sending a body to the cloud over HTTP(S): the other half of
 * tedge_http_download.c, and it keeps that file's two rules — the token
 * reaches the tenant and nowhere else, and a redirect is followed rather
 * than failed.
 *
 * The body is either a string the caller already has, or written by a
 * producer callback. Either way its length is known before the request
 * starts, because Cumulocity wants a Content-Length and this device has no
 * room to hold a log twice. A source that runs out early is padded, and one
 * that has more to say is cut: the length promised in the header is the
 * length that goes on the wire, whatever the source does underneath.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define RX_SIZE   512
#define SEND_SIZE 256

static struct tedge_upload *current;

/* ------------------------------------------------------------------------ */
/* The body                                                                  */
/* ------------------------------------------------------------------------ */

static int send_all(int sock, const uint8_t *data, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		int ret = zsock_send(sock, data + sent, len - sent, 0);

		if (ret <= 0) {
			return (ret == 0) ? -ECONNRESET : -errno;
		}
		sent += (size_t)ret;
	}
	return (int)sent;
}

/* The sink the producer writes through while the request body is on the
 * wire: it sends, counts, and refuses more than the length promised in the
 * header. */
struct wire {
	int sock;
	size_t left;
	int rc;
};

static int wire_sink(const void *data, size_t len, void *ctx)
{
	struct wire *w = ctx;
	int ret;

	if (w->left == 0) {
		return -ENOSPC; /* the producer has more to say than it said */
	}
	len = MIN(len, w->left);
	ret = send_all(w->sock, data, len);
	if (ret < 0) {
		w->rc = ret;
		return ret;
	}
	w->left -= len;
	return 0;
}

static int send_body(int sock, struct http_request *req, void *user_data)
{
	struct tedge_upload *up = user_data;
	struct wire w = { .sock = sock, .left = up->length };
	uint8_t pad[SEND_SIZE];

	ARG_UNUSED(req);
	if (up->payload != NULL) {
		int ret = send_all(sock, (const uint8_t *)up->payload,
				   up->length);

		return (ret < 0) ? ret : (int)up->length;
	}
	(void)up->producer(wire_sink, &w, up->user_data);
	if (w.rc != 0) {
		return w.rc;
	}
	/* A producer that ran short still owes the server the bytes the
	 * Content-Length promised; spaces are harmless in a log. */
	memset(pad, ' ', sizeof(pad));
	while (w.left > 0) {
		size_t n = MIN(w.left, sizeof(pad));
		int ret = send_all(sock, pad, n);

		if (ret < 0) {
			return ret;
		}
		w.left -= n;
	}
	return (int)up->length;
}

/* ------------------------------------------------------------------------ */
/* Headers worth keeping                                                     */
/* ------------------------------------------------------------------------ */

static enum { HDR_OTHER, HDR_LOCATION } header_kind;

static int on_header_field(struct http_parser *parser, const char *at,
			   size_t length)
{
	ARG_UNUSED(parser);
	header_kind = (length == 8 && strncasecmp(at, "location", 8) == 0)
			      ? HDR_LOCATION
			      : HDR_OTHER;
	return 0;
}

static int on_header_value(struct http_parser *parser, const char *at,
			   size_t length)
{
	struct tedge_upload *up = current;

	ARG_UNUSED(parser);
	if (header_kind == HDR_LOCATION && up != NULL && up->location != NULL &&
	    length < up->location_len) {
		memcpy(up->location, at, length);
		up->location[length] = '\0';
	}
	return 0;
}

static int on_response(struct http_response *rsp, enum http_final_call final,
		       void *user_data)
{
	struct tedge_upload *up = user_data;

	ARG_UNUSED(final);
	up->status = rsp->http_status_code;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* One request                                                               */
/* ------------------------------------------------------------------------ */

int tedge_upload(struct tedge_upload *req)
{
	static const sec_tag_t tags[] = { TEDGE_TAG_SERVER_CA };
	struct http_parser_settings cb = { .on_header_field = on_header_field,
					   .on_header_value = on_header_value };
	struct zsock_addrinfo hints = { .ai_family = AF_INET,
					.ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res = NULL;
	struct http_request hr = { 0 };
	const char *headers[4] = { 0 };
	static uint8_t rx[RX_SIZE];
	char auth[1100];
	char disposition[96];
	char host[TEDGE_HOST_MAX];
	char port_s[8];
	const char *path;
	uint16_t port;
	bool tls;
	int fd = -1, ret, h = 0;

	if (req == NULL || req->url == NULL ||
	    (req->payload == NULL && req->producer == NULL)) {
		return -EINVAL;
	}
	if (req->timeout_ms == 0) {
		req->timeout_ms = 30000;
	}
	if (req->location != NULL && req->location_len > 0) {
		req->location[0] = '\0';
	}
	ret = tedge_url_split(req->url, &tls, host, sizeof(host), &port, &path);
	if (ret != 0) {
		LOG_ERR("upload: cannot parse the URL");
		return ret;
	}
	snprintf(port_s, sizeof(port_s), "%u", port);
	ret = zsock_getaddrinfo(host, port_s, &hints, &res);
	if (ret != 0) {
		LOG_ERR("upload: cannot resolve %s (%d)", host, ret);
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
	}
	ret = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("upload: cannot connect to %s (%d)", host, ret);
		zsock_close(fd);
		return ret;
	}

	/* Cumulocity answers 201 with an empty body and no Location unless it
	 * is asked for JSON, and the created object's id is the whole point of
	 * the request. */
	headers[h++] = "Accept: application/json\r\n";
	if (req->filename != NULL) {
		snprintf(disposition, sizeof(disposition),
			 "Content-Disposition: attachment; filename=\"%s\"\r\n",
			 req->filename);
		headers[h++] = disposition;
	}
	/* The token is for the tenant only. */
	if (req->token != NULL && req->token[0] != '\0' &&
	    tedge_url_is_tenant(host)) {
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n",
			 req->token);
		headers[h++] = auth;
	}

	if (req->payload != NULL && req->length == 0) {
		req->length = strlen(req->payload);
	}
	header_kind = HDR_OTHER;
	req->status = 0;

	hr.method = HTTP_POST;
	hr.url = path;
	hr.host = host;
	hr.protocol = "HTTP/1.1";
	hr.content_type_value = (req->content_type != NULL) ? req->content_type
							    : "text/plain";
	hr.header_fields = headers;
	hr.payload_cb = send_body;
	hr.payload_len = req->length;
	hr.response = on_response;
	hr.http_cb = &cb;
	hr.recv_buf = rx;
	hr.recv_buf_len = sizeof(rx);

	current = req;
	ret = http_client_req(fd, &hr, req->timeout_ms, req);
	current = NULL;
	memset(auth, 0, sizeof(auth));
	zsock_close(fd);

	if (ret < 0) {
		LOG_ERR("upload: the request failed (%d)", ret);
		return ret;
	}
	if (req->status < 200 || req->status >= 300) {
		LOG_ERR("upload: the server answered %d", req->status);
		return -EIO;
	}
	return 0;
}
