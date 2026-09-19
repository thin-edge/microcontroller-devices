/* SPDX-License-Identifier: Apache-2.0
 *
 * SoftAP / captive-portal front end of the Wi-Fi provisioner
 * (c8y-direct-spikes, Spike E). An alternative to Improv over BLE that any
 * phone or laptop can use without an app:
 *
 *   - an open access point "<hostname>-setup" at 192.168.4.1, with a DHCP
 *     server that hands out 192.168.4.1 as router and DNS server;
 *   - a DNS responder that answers every A query with 192.168.4.1, so the
 *     phone's connectivity check lands here and it opens its captive-portal
 *     sheet;
 *   - a minimal HTTP server: GET / serves the form, any other GET redirects
 *     to it, POST /connect takes "ssid" and "psk".
 *
 * The station interface tests the credentials while the AP stays up (AP+STA);
 * success stores them and reboots into the application, as the BLE front end
 * does. A spike: no HTTPS, no authorization, no scan list.
 */

#include "softap.h"
#include "boot_request.h"
#include "net.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(softap, CONFIG_LOG_DEFAULT_LEVEL);

#define AP_ADDR "192.168.4.1"
#define AP_MASK "255.255.255.0"

static struct net_if *ap_iface;
static struct net_if *sta_iface;

/* Credentials from the form, handed from the HTTP loop to the test. */
static struct app_wifi_creds pending;
static char last_result[96];

/* ------------------------------------------------------------------------- */
/* Access point and DHCP                                                     */
/* ------------------------------------------------------------------------- */

static int ap_start(const char *ssid)
{
	static struct wifi_connect_req_params p;
	struct in_addr addr, mask;

	ap_iface = net_if_get_wifi_sap();
	sta_iface = net_if_get_wifi_sta();
	LOG_INF("interfaces: sta=%d ap=%d first_wifi=%d default=%d",
		net_if_get_by_iface(sta_iface), net_if_get_by_iface(ap_iface),
		net_if_get_by_iface(net_if_get_first_wifi()),
		net_if_get_by_iface(net_if_get_default()));
	if (ap_iface == NULL) {
		return -ENODEV;
	}

	net_addr_pton(AF_INET, AP_ADDR, &addr);
	net_addr_pton(AF_INET, AP_MASK, &mask);
	net_if_ipv4_set_gw(ap_iface, &addr);
	if (net_if_ipv4_addr_add(ap_iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("could not set the AP address");
	}
	net_if_ipv4_set_netmask_by_addr(ap_iface, &addr, &mask);

	p.ssid = (const uint8_t *)ssid;
	p.ssid_length = strlen(ssid);
	p.security = WIFI_SECURITY_TYPE_NONE;
	p.channel = WIFI_CHANNEL_ANY;
	p.band = WIFI_FREQ_BAND_2_4_GHZ;
	int rc = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &p, sizeof(p));

	if (rc) {
		LOG_ERR("AP enable failed (%d)", rc);
		return rc;
	}

	addr.s4_addr[3] += 10; /* pool from .11 */
	rc = net_dhcpv4_server_start(ap_iface, &addr);
	if (rc) {
		LOG_ERR("DHCP server failed (%d)", rc);
	}
	return rc;
}

/* ------------------------------------------------------------------------- */
/* Catch-all DNS                                                             */
/* ------------------------------------------------------------------------- */

static void dns_fn(void *a, void *b, void *c)
{
	static uint8_t buf[512];
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53) };
	int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (fd < 0 || zsock_bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		LOG_ERR("DNS: bind failed (%d)", errno);
		return;
	}
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		int n = zsock_recvfrom(fd, buf, sizeof(buf), 0,
				       (struct sockaddr *)&from, &fl);

		if (n < 12 || (buf[2] & 0x80) || buf[5] != 1) {
			continue; /* not a single-question query */
		}
		/* Walk the question name to its end (+ QTYPE, QCLASS). */
		int q = 12;

		while (q < n && buf[q] != 0) {
			q += buf[q] + 1;
		}
		q += 5;
		if (q > n || q + 16 > (int)sizeof(buf)) {
			continue;
		}
		uint16_t qtype = (buf[q - 4] << 8) | buf[q - 3];

		buf[2] = 0x84 | (buf[2] & 0x01); /* response, authoritative, RD */
		buf[3] = 0x80;                   /* RA, NOERROR */
		buf[6] = 0;
		buf[7] = (qtype == 1) ? 1 : 0;   /* one answer for A only */
		buf[8] = buf[9] = buf[10] = buf[11] = 0;
		int len = q;

		if (qtype == 1) {
			const uint8_t ans[] = {
				0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4,
				192, 168, 4, 1,
			};
			memcpy(&buf[q], ans, sizeof(ans));
			len += sizeof(ans);
		}
		(void)zsock_sendto(fd, buf, len, 0, (struct sockaddr *)&from, fl);
	}
}

K_THREAD_DEFINE(dns_tid, 2048, dns_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(8),
		0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------------- */
/* HTTP                                                                      */
/* ------------------------------------------------------------------------- */

static bool testing;

static const char form_head[] =
	"<!doctype html><html><head><meta name=viewport "
	"content='width=device-width,initial-scale=1'><title>Wi-Fi setup</title>"
	"<style>body{font-family:sans-serif;max-width:24em;margin:2em auto;"
	"padding:0 1em}input,button{width:100%;font-size:1.1em;margin:.3em 0;"
	"padding:.4em}</style>";
static const char form_head2[] = "</head><body><h2>Wi-Fi setup</h2>";
/* While a test runs, the page reloads itself: the reload waits in the
 * listen backlog until the test ends, then shows the result. */
static const char refresh[] = "<meta http-equiv=refresh content='3;url=/'>";
static const char form_body[] =
	"<form method=post action=/connect>"
	"<label>Network (SSID)<input name=ssid required maxlength=32></label>"
	"<label>Password<input name=psk type=password maxlength=64></label>"
	"<button>Connect</button></form>"
	"<p><small>The device tests the network, then restarts into its "
	"application. If this page comes back, the test failed.</small></p>"
	"</body></html>";

static void send_all(int fd, const char *s, size_t n)
{
	while (n > 0) {
		int w = zsock_send(fd, s, n, 0);

		if (w <= 0) {
			return;
		}
		s += w;
		n -= w;
	}
}

static void send_str(int fd, const char *s)
{
	send_all(fd, s, strlen(s));
}

static void send_page(int fd, const char *msg)
{
	char hdr[96];
	const char *r = testing ? refresh : "";
	size_t len = strlen(form_head) + strlen(r) + strlen(form_head2) +
		     strlen(form_body);
	char note[160] = "";

	if (msg && msg[0]) {
		snprintf(note, sizeof(note), "<p><b>%s</b></p>", msg);
		len += strlen(note);
	}
	snprintf(hdr, sizeof(hdr),
		 "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
		 "Content-Length: %u\r\nConnection: close\r\n\r\n",
		 (unsigned int)len);
	send_str(fd, hdr);
	send_str(fd, form_head);
	send_str(fd, r);
	send_str(fd, form_head2);
	send_str(fd, note);
	send_str(fd, form_body);
}

/* URL-decode "key=value&..." field @p key of @p body into @p out. */
static size_t form_field(const char *body, const char *key, char *out,
			 size_t cap)
{
	size_t kl = strlen(key), n = 0;
	const char *p = body;

	while (p && *p) {
		if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
			p += kl + 1;
			while (*p && *p != '&' && n < cap - 1) {
				if (*p == '+') {
					out[n++] = ' ';
					p++;
				} else if (*p == '%' && p[1] && p[2]) {
					char h[3] = { p[1], p[2], 0 };

					out[n++] = (char)strtol(h, NULL, 16);
					p += 3;
				} else {
					out[n++] = *p++;
				}
			}
			break;
		}
		p = strchr(p, '&');
		p = p ? p + 1 : NULL;
	}
	out[n] = '\0';
	return n;
}

/* Handle one request. Returns true when credentials are waiting in pending. */
static bool http_handle(int fd)
{
	static char req[1536];
	int n = 0, body_at = -1, clen = 0;

	/* Read headers, then the body announced by Content-Length. */
	while (n < (int)sizeof(req) - 1) {
		int r = zsock_recv(fd, req + n, sizeof(req) - 1 - n, 0);

		if (r <= 0) {
			break;
		}
		n += r;
		req[n] = '\0';
		if (body_at < 0) {
			char *e = strstr(req, "\r\n\r\n");

			if (e) {
				body_at = e + 4 - req;
				char *cl = strstr(req, "Content-Length:");

				if (!cl) {
					cl = strstr(req, "content-length:");
				}
				clen = cl ? atoi(cl + 15) : 0;
			}
		}
		if (body_at >= 0 && n - body_at >= clen) {
			break;
		}
	}
	req[n] = '\0';

	char method[8] = "", path[64] = "";

	sscanf(req, "%7s %63s", method, path);
	LOG_INF("HTTP %s %s", method, path);

	if (strcmp(method, "POST") == 0 && strcmp(path, "/connect") == 0 &&
	    body_at >= 0) {
		char ssid[33], psk[65];

		memset(&pending, 0, sizeof(pending));
		pending.ssid_len = form_field(req + body_at, "ssid", ssid,
					      sizeof(ssid));
		pending.psk_len = form_field(req + body_at, "psk", psk,
					     sizeof(psk));
		memcpy(pending.ssid, ssid, pending.ssid_len);
		memcpy(pending.psk, psk, pending.psk_len);
		memset(psk, 0, sizeof(psk));
		memset(req, 0, sizeof(req));
		if (pending.ssid_len == 0) {
			send_page(fd, "Enter a network name.");
			return false;
		}
		snprintf(last_result, sizeof(last_result),
			 "Testing \"%s\"... this takes up to %d s.", pending.ssid,
			 CONFIG_APP_WIFI_PROV_CONNECT_TIMEOUT_S);
		testing = true;
		send_page(fd, last_result);
		return true;
	}
	if (strcmp(path, "/") == 0 || strncmp(path, "/?", 2) == 0) {
		send_page(fd, last_result);
		return false;
	}
	/* Any other URL (the OS connectivity checks included): redirect to the
	 * form, which makes the phone show its captive-portal sheet. */
	send_str(fd, "HTTP/1.1 302 Found\r\nLocation: http://" AP_ADDR "/\r\n"
		     "Content-Length: 0\r\nConnection: close\r\n\r\n");
	return false;
}

/* ------------------------------------------------------------------------- */
/* Credential test and hand-off                                              */
/* ------------------------------------------------------------------------- */

static int listen_fd = -1;

/* Answer the reload that waited out the test, so the phone sees success. */
static void serve_final_page(void)
{
	struct zsock_pollfd pfd = { .fd = listen_fd, .events = ZSOCK_POLLIN };

	if (listen_fd >= 0 && zsock_poll(&pfd, 1, 5000) > 0) {
		int fd = zsock_accept(listen_fd, NULL, NULL);

		if (fd >= 0) {
			static char drain[512];
			struct zsock_timeval tv = { .tv_sec = 2 };

			(void)zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
					       sizeof(tv));
			(void)zsock_recv(fd, drain, sizeof(drain), 0);
			send_page(fd, last_result);
			zsock_close(fd);
		}
	}
	k_msleep(1000);
}

static FUNC_NORETURN void reboot_now(void)
{
	int rc = boot_request_clear();

	if (rc != 0) {
		LOG_ERR("Could not clear the boot request (%d)", rc);
	}
	LOG_INF("Leaving the provisioner: rebooting into the application");
	log_flush();
	boot_request_reboot();
	CODE_UNREACHABLE;
}

static void try_pending(void)
{
	int64_t t0 = k_uptime_get();

	LOG_INF("Testing credentials for SSID \"%s\" (AP stays up)",
		pending.ssid);
	int rc = app_net_try_credentials(
		&pending, K_SECONDS(CONFIG_APP_WIFI_PROV_CONNECT_TIMEOUT_S));

	LOG_INF("MEAS softap: credential test %s after %lld ms",
		rc ? "failed" : "passed", k_uptime_get() - t0);
	testing = false;
	if (rc != 0) {
		snprintf(last_result, sizeof(last_result),
			 "Could not join \"%s\" (%d). Check the password.",
			 pending.ssid, rc);
		memset(&pending, 0, sizeof(pending));
		return;
	}
	(void)wifi_credentials_delete_all();
	rc = wifi_credentials_set_personal(
		pending.ssid, pending.ssid_len,
		pending.psk_len ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
		NULL, 0, pending.psk, pending.psk_len, 0, 0, 0);
	memset(&pending, 0, sizeof(pending));
	if (rc != 0) {
		LOG_ERR("Storing the credentials failed (%d)", rc);
		snprintf(last_result, sizeof(last_result),
			 "Storing the credentials failed (%d).", rc);
		return;
	}
	LOG_INF("Provisioned over SoftAP");
	snprintf(last_result, sizeof(last_result),
		 "Connected. The device restarts into its application now.");
	serve_final_page();
	reboot_now();
}

FUNC_NORETURN void softap_run(void)
{
	char ssid[40];
	int64_t deadline = k_uptime_get() + CONFIG_APP_WIFI_PROV_WINDOW_S * 1000LL;
	struct boot_request req;
	bool return_on_expiry = boot_request_read(&req) == 0 &&
				req.reason == BOOT_REQUEST_OPERATOR;

	status_led_set_mode(STATUS_LED_PROVISIONING);
	snprintf(ssid, sizeof(ssid), "%.26s-setup", app_net_hostname());
	if (ap_start(ssid) != 0) {
		k_sleep(K_SECONDS(30));
		boot_request_reboot(); /* retry; the request stays set */
	}
	k_thread_start(dns_tid);
	LOG_INF("SoftAP \"%s\" up: http://" AP_ADDR "/ for %d s", ssid,
		CONFIG_APP_WIFI_PROV_WINDOW_S);

	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(80) };
	int lfd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	listen_fd = lfd;
	int one = 1;

	(void)zsock_setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (lfd < 0 || zsock_bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    zsock_listen(lfd, 2) < 0) {
		LOG_ERR("HTTP: listen failed (%d)", errno);
	}

	for (;;) {
		struct zsock_pollfd pfd = { .fd = lfd, .events = ZSOCK_POLLIN };
		int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			struct app_wifi_creds c;
			bool have = return_on_expiry ||
				    app_wifi_creds_resolve(&c) == 0;

			memset(&c, 0, sizeof(c));
			if (have) {
				LOG_INF("Provisioning window expired; returning");
				reboot_now();
			}
			deadline = k_uptime_get() +
				   CONFIG_APP_WIFI_PROV_WINDOW_S * 1000LL;
			continue;
		}
		if (zsock_poll(&pfd, 1, (int)MIN(left, 5000)) <= 0) {
			continue;
		}
		int fd = zsock_accept(lfd, NULL, NULL);

		if (fd < 0) {
			continue;
		}
		struct zsock_timeval tv = { .tv_sec = 5 };

		(void)zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
				       sizeof(tv));
		bool go = http_handle(fd);

		zsock_close(fd);
		if (go) {
			k_msleep(500); /* let the reply leave before the test */
			try_pending();
		}
	}
}
