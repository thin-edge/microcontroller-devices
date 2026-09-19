/* SPDX-License-Identifier: Apache-2.0
 *
 * Spike B (c8y-direct-spikes, tasks 4.1-4.6): stream a signed image into
 * slot1, test-boot it through MCUboot, and confirm it or let MCUboot revert
 * it. Throwaway measurement code; the production feature goes into
 * tedge-zephyr.
 *
 * - spike_ota_download(): HTTP or HTTPS (Bearer JWT for Cumulocity URLs),
 *   following up to SPIKE_OTA_MAX_REDIRECTS redirects itself (Zephyr's HTTP
 *   client doesn't), written to slot1 with progressive erase.
 * - Shell: `spike ota get <url>`, `spike ota confirm`, `spike ota status`,
 *   `spike reboot`.
 * - c8y_Firmware (515) is handled in spike_mqtt.c and uses this download.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>

#include <mbedtls/memory_buffer_alloc.h>

#include "boot_request.h"
#include "spike.h"

LOG_MODULE_REGISTER(spike_ota, LOG_LEVEL_INF);

#if defined(CONFIG_SPIKE_OTA_EXTRA_CAS)
/* Roots for GitHub release downloads: github.com (Sectigo E46, cross-signed
 * by USERTrust ECC) and release-assets.githubusercontent.com (ISRG X1).
 */
#define TAG_EXTRA_CA1 0x5A12
#define TAG_EXTRA_CA2 0x5A13
static const unsigned char extra_ca1[] = {
#include "spike/usertrust_ecc.pem.inc"
	0x00};
static const unsigned char extra_ca2[] = {
#include "spike/isrg_root_x1.pem.inc"
	0x00};
#endif

#define SLOT0_ID PARTITION_ID(slot0_partition)
#define SLOT1_ID PARTITION_ID(slot1_partition)

struct dl_state {
	struct flash_img_context img;
	int64_t t_start;
	size_t written;
	int write_err;
	/* Redirect handling: the parser callbacks capture Location. */
	bool in_location;
	char location[1024];
	uint16_t status;
	/* Diagnostic: download without writing flash. */
	bool discard;
};

static bool ota_discard;

static struct dl_state dl;
static uint8_t http_rx[2048];

/* ------------------------------------------------------------------------ */
/* URL parsing                                                               */
/* ------------------------------------------------------------------------ */

struct url {
	bool tls;
	char host[96];
	char port[6];
	char path[1024];
};

static int url_parse(const char *s, struct url *u)
{
	const char *p;
	const char *host_end;
	const char *colon;

	if (strncmp(s, "https://", 8) == 0) {
		u->tls = true;
		p = s + 8;
	} else if (strncmp(s, "http://", 7) == 0) {
		u->tls = false;
		p = s + 7;
	} else {
		return -EINVAL;
	}
	host_end = strchr(p, '/');
	if (!host_end) {
		host_end = p + strlen(p);
	}
	colon = memchr(p, ':', host_end - p);
	if ((size_t)((colon ? colon : host_end) - p) >= sizeof(u->host)) {
		return -ENAMETOOLONG;
	}
	snprintk(u->host, sizeof(u->host), "%.*s",
		 (int)((colon ? colon : host_end) - p), p);
	if (colon) {
		snprintk(u->port, sizeof(u->port), "%.*s",
			 (int)(host_end - colon - 1), colon + 1);
	} else {
		snprintk(u->port, sizeof(u->port), "%s", u->tls ? "443" : "80");
	}
	snprintk(u->path, sizeof(u->path), "%s", *host_end ? host_end : "/");
	return 0;
}

/* ------------------------------------------------------------------------ */
/* HTTP                                                                      */
/* ------------------------------------------------------------------------ */

static int on_header_field(struct http_parser *parser, const char *at,
			   size_t length)
{
	ARG_UNUSED(parser);
	dl.in_location = (length == 8 && strncasecmp(at, "Location", 8) == 0);
	return 0;
}

static int on_header_value(struct http_parser *parser, const char *at,
			   size_t length)
{
	ARG_UNUSED(parser);
	if (dl.in_location) {
		snprintk(dl.location, sizeof(dl.location), "%.*s", (int)length,
			 at);
		dl.in_location = false;
	}
	return 0;
}

/* The body is written from the parser's on_body callback, not from the
 * response callback: with Transfer-Encoding: chunked (Cumulocity binaries),
 * one receive buffer holds several body segments separated by chunk-size
 * lines, and Zephyr's http_client reports the first segment's start with the
 * last segment's length (body_frag_start/body_frag_len), which corrupts the
 * image. on_body gets each decoded segment exactly.
 */
static int on_body(struct http_parser *parser, const char *at, size_t length)
{
	if (parser->status_code != 200 || length == 0) {
		return 0;
	}
	if (dl.discard) {
		dl.written += length;
		return 0;
	}
	if (dl.write_err == 0) {
		dl.write_err = flash_img_buffered_write(&dl.img, (const uint8_t *)at,
							length, false);
		if (dl.written / (128 * 1024) != (dl.written + length) / (128 * 1024)) {
			LOG_INF("  %zu KB written", (dl.written + length) / 1024);
		}
		dl.written += length;
	}
	return 0;
}

static const struct http_parser_settings parser_cb = {
	.on_header_field = on_header_field,
	.on_header_value = on_header_value,
	.on_body = on_body,
};

static int on_response(struct http_response *rsp, enum http_final_call final,
		       void *user_data)
{
	ARG_UNUSED(final);
	ARG_UNUSED(user_data);

	dl.status = rsp->http_status_code;
	return dl.write_err ? -EIO : 0;
}

static int open_socket(const struct url *u)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res;
	int fd, ret;

	ret = zsock_getaddrinfo(u->host, u->port, &hints, &res);
	if (ret) {
		LOG_ERR("DNS lookup of %s failed: %d", u->host, ret);
		return -EHOSTUNREACH;
	}
	fd = zsock_socket(AF_INET, SOCK_STREAM,
			  u->tls ? IPPROTO_TLS_1_2 : IPPROTO_TCP);
	if (fd < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}
	if (u->tls) {
		static const sec_tag_t tags[] = {
			SPIKE_TAG_SERVER_CA,
#if defined(CONFIG_SPIKE_OTA_EXTRA_CAS)
			TAG_EXTRA_CA1, TAG_EXTRA_CA2,
#endif
		};

		zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, tags,
				 sizeof(tags));
		zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME, u->host,
				 strlen(u->host) + 1);
	}
	ret = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret) {
		ret = -errno;
		LOG_ERR("connect to %s:%s failed: %d", u->host, u->port, ret);
		zsock_close(fd);
		return ret;
	}
	return fd;
}

int spike_ota_download(const char *url_in, const char *bearer)
{
	char url_buf[1152];
	char auth[1024];
	const char *headers[] = {NULL, NULL};
	size_t cur, blocks, peak;
	struct url u;
	int ret = -EINVAL;

	snprintk(url_buf, sizeof(url_buf), "%s", url_in);
	memset(&dl, 0, sizeof(dl));
	dl.discard = ota_discard;
	ret = flash_img_init_id(&dl.img, SLOT1_ID);
	if (ret) {
		LOG_ERR("flash_img_init_id(slot1) failed: %d", ret);
		return ret;
	}
	mbedtls_memory_buffer_alloc_max_reset();
	dl.t_start = k_uptime_get();

	for (int hop = 0; hop <= CONFIG_SPIKE_OTA_MAX_REDIRECTS; hop++) {
		struct http_request req = {0};
		int fd;

		ret = url_parse(url_buf, &u);
		if (ret) {
			LOG_ERR("bad URL: %s", url_buf);
			return ret;
		}
		LOG_INF("GET %s://%s:%s%s (hop %d)", u.tls ? "https" : "http",
			u.host, u.port, u.path, hop);

		fd = open_socket(&u);
		if (fd < 0) {
			return fd;
		}

		/* Credentials only go to the host they belong to. */
		headers[0] = NULL;
		if (bearer && hop == 0) {
			snprintk(auth, sizeof(auth), "Authorization: Bearer %s\r\n",
				 bearer);
			headers[0] = auth;
		}

		req.method = HTTP_GET;
		req.url = u.path;
		req.host = u.host;
		req.protocol = "HTTP/1.1";
		req.response = on_response;
		req.http_cb = &parser_cb;
		req.recv_buf = http_rx;
		req.recv_buf_len = sizeof(http_rx);
		req.header_fields = headers[0] ? headers : NULL;

		dl.location[0] = '\0';
		dl.status = 0;
		ret = http_client_req(fd, &req, 60000, NULL);
		zsock_close(fd);
		if (ret < 0) {
			LOG_ERR("HTTP request failed: %d", ret);
			return ret;
		}

		if (dl.status >= 300 && dl.status < 400 && dl.location[0]) {
			LOG_INF("HTTP %u redirect -> %s", dl.status, dl.location);
			if (dl.location[0] == '/') {
				/* Relative: same scheme, host and port. */
				snprintk(url_buf, sizeof(url_buf), "%s://%s:%s%s",
					 u.tls ? "https" : "http", u.host, u.port,
					 dl.location);
			} else {
				snprintk(url_buf, sizeof(url_buf), "%s",
					 dl.location);
			}
			continue;
		}
		break;
	}

	if (dl.status != 200) {
		LOG_ERR("download failed: HTTP %u", dl.status);
		return -EIO;
	}
	if (!dl.discard && dl.write_err == 0) {
		/* Flush the last partial block. */
		dl.write_err = flash_img_buffered_write(&dl.img, NULL, 0, true);
	}
	if (dl.write_err) {
		LOG_ERR("writing slot1 failed: %d", dl.write_err);
		return dl.write_err;
	}
	if (dl.discard) {
		int64_t dms = MAX(k_uptime_get() - dl.t_start, 1);

		LOG_INF("MEAS download (discarded, no flash writes): %zu B in "
			"%lld ms (%lld KB/s)", dl.written, dms,
			(long long)(dl.written / dms));
		return 0;
	}

	int64_t ms = MAX(k_uptime_get() - dl.t_start, 1);

	mbedtls_memory_buffer_alloc_cur_get(&cur, &blocks);
	mbedtls_memory_buffer_alloc_max_get(&peak, &blocks);
	LOG_INF("MEAS download: %zu B in %lld ms (%lld KB/s) into slot1; "
		"tls_heap cur=%zu peak=%zu (includes the MQTT session)",
		dl.written, ms, (long long)(dl.written / ms), cur, peak);

	struct mcuboot_img_header hdr;

	ret = boot_read_bank_header(SLOT1_ID, &hdr, sizeof(hdr));
	if (ret) {
		LOG_ERR("slot1 has no valid MCUboot header (%d)", ret);
		return ret;
	}
	LOG_INF("slot1 image: version %u.%u.%u+%u, %u B",
		hdr.h.v1.sem_ver.major, hdr.h.v1.sem_ver.minor,
		hdr.h.v1.sem_ver.revision, hdr.h.v1.sem_ver.build_num,
		hdr.h.v1.image_size);
	return 0;
}

int spike_ota_request_test_and_reboot(void)
{
	int ret = boot_request_upgrade(BOOT_UPGRADE_TEST);

	if (ret) {
		LOG_ERR("boot_request_upgrade(TEST) failed: %d", ret);
		return ret;
	}
	LOG_INF("MEAS ota: test upgrade requested at uptime %lld ms; rebooting",
		k_uptime_get());
	k_sleep(K_MSEC(300));
	boot_request_reboot();
	return 0;
}

static void print_bank(const struct shell *sh, const char *name, uint8_t id)
{
	struct mcuboot_img_header hdr;

	if (boot_read_bank_header(id, &hdr, sizeof(hdr)) == 0) {
		shell_print(sh, "%s: version %u.%u.%u+%u, %u B", name,
			    hdr.h.v1.sem_ver.major, hdr.h.v1.sem_ver.minor,
			    hdr.h.v1.sem_ver.revision,
			    hdr.h.v1.sem_ver.build_num, hdr.h.v1.image_size);
	} else {
		shell_print(sh, "%s: no image", name);
	}
}

int spike_ota_running_version(char *buf, size_t len)
{
	struct mcuboot_img_header hdr;
	int ret = boot_read_bank_header(SLOT0_ID, &hdr, sizeof(hdr));

	if (ret == 0) {
		snprintk(buf, len, "%u.%u.%u", hdr.h.v1.sem_ver.major,
			 hdr.h.v1.sem_ver.minor, hdr.h.v1.sem_ver.revision);
	}
	return ret;
}

void spike_ota_log_boot(void)
{
#if defined(CONFIG_SPIKE_OTA_EXTRA_CAS)
	tls_credential_add(TAG_EXTRA_CA1, TLS_CREDENTIAL_CA_CERTIFICATE, extra_ca1,
			   sizeof(extra_ca1));
	tls_credential_add(TAG_EXTRA_CA2, TLS_CREDENTIAL_CA_CERTIFICATE, extra_ca2,
			   sizeof(extra_ca2));
#endif

	struct mcuboot_img_header hdr;

	if (boot_read_bank_header(SLOT0_ID, &hdr, sizeof(hdr)) == 0) {
		LOG_INF("running image %u.%u.%u+%u (tag %s), %s",
			hdr.h.v1.sem_ver.major, hdr.h.v1.sem_ver.minor,
			hdr.h.v1.sem_ver.revision, hdr.h.v1.sem_ver.build_num,
			CONFIG_SPIKE_BUILD_TAG,
			boot_is_img_confirmed() ? "confirmed" : "NOT confirmed (test boot)");
	}
}

/* ------------------------------------------------------------------------ */
/* Shell                                                                     */
/* ------------------------------------------------------------------------ */

/* The download runs on its own thread: with the shell as the log backend,
 * the shell thread prints the logs, so running the download on it would
 * hold them back until it finished.
 */
K_THREAD_STACK_DEFINE(ota_stack, 8192);
static struct k_thread ota_thread;
static char ota_url[1152];
static bool ota_reboot;
static atomic_t ota_busy;

static void ota_thread_fn(void *a, void *b, void *c)
{
	int ret = spike_ota_download(ota_url, NULL);

	ota_discard = false;
	if (ret) {
		LOG_ERR("download failed: %d", ret);
	} else if (ota_reboot) {
		spike_ota_request_test_and_reboot();
	}
	atomic_clear(&ota_busy);
}

static int cmd_get(const struct shell *sh, size_t argc, char **argv)
{
	if (!atomic_cas(&ota_busy, 0, 1)) {
		shell_error(sh, "a download is already running");
		return -EBUSY;
	}
	snprintk(ota_url, sizeof(ota_url), "%s", argv[1]);
	ota_reboot = true;
	ota_discard = false;
	for (size_t i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--no-reboot") == 0) {
			ota_reboot = false;
		} else if (strcmp(argv[i], "--discard") == 0) {
			ota_discard = true;
			ota_reboot = false;
		}
	}
	k_thread_create(&ota_thread, ota_stack, K_THREAD_STACK_SIZEOF(ota_stack),
			ota_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(9), 0,
			K_NO_WAIT);
	k_thread_name_set(&ota_thread, "spike_ota");
	shell_print(sh, "download started");
	return 0;
}

static int cmd_confirm(const struct shell *sh, size_t argc, char **argv)
{
	int ret = boot_write_img_confirmed();

	shell_print(sh, "boot_write_img_confirmed: %d; confirmed=%d", ret,
		    boot_is_img_confirmed());
	return ret;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "tag %s, confirmed=%d", CONFIG_SPIKE_BUILD_TAG,
		    boot_is_img_confirmed());
	print_bank(sh, "slot0 (running)", SLOT0_ID);
	print_bank(sh, "slot1", SLOT1_ID);
	return 0;
}

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "full-system reset");
	k_sleep(K_MSEC(100));
	boot_request_reboot();
	return 0;
}

/* Spike E: start the provisioner as the button pattern would (operator
 * request, so it returns here when its window expires). */
static int cmd_provision(const struct shell *sh, size_t argc, char **argv)
{
	int rc = boot_request_set(BOOT_REQUEST_PROVISIONER, BOOT_REQUEST_OPERATOR);

	if (rc) {
		shell_error(sh, "boot request failed (%d)", rc);
		return rc;
	}
	shell_print(sh, "rebooting into the provisioner");
	k_sleep(K_MSEC(100));
	boot_request_reboot();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ota,
	SHELL_CMD_ARG(get, NULL, "<url> [--no-reboot] [--discard]: download into slot1, test-boot",
		      cmd_get, 2, 2),
	SHELL_CMD(confirm, NULL, "confirm the running image", cmd_confirm),
	SHELL_CMD(status, NULL, "slot and confirmation state", cmd_status),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_spike,
	SHELL_CMD(ota, &sub_ota, "Spike B: firmware update", NULL),
	SHELL_CMD(reboot, NULL, "full-system reset", cmd_reboot),
	SHELL_CMD(provision, NULL, "reboot into the Wi-Fi provisioner", cmd_provision),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(spike, &sub_spike, "c8y-direct spike commands", NULL);
