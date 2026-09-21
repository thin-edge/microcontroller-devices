/* SPDX-License-Identifier: Apache-2.0
 *
 * lab-ztp-provisioner over BLE: the GATT peripheral and the provisioning state
 * machine. The wire behaviour mirrors the Go peripheral
 * (internal/transport/ble/ble.go) so the stock relays — the web app and the
 * desktop app — need no device-specific handling:
 *
 *   service  6e400001-b5a3-f393-e0a9-e50e24dcca9e, advertised name "ztp"
 *   request  6e400002  write        framed messages from the relay
 *   response 6e400003  notify       framed messages to the relay
 *   status   6e400004  read/notify  0 idle, 1 relaying, 2 done, 3 error
 *   timesync 6e400005  write        RFC 3339 UTC time from the relay
 *
 * Framing on request and response: [u16 BE length][payload], repeated; a
 * zero length ends the message. The relay first writes an empty message
 * (the "kick"): the device answers with its signed enrollment envelope. The
 * relay forwards it to the server and writes the server's text response back:
 * the device applies it, answers with an empty message and status "done",
 * and reboots into the application.
 *
 * Crypto takes about a second per ECC operation here, so nothing but
 * reassembly runs in Bluetooth context: complete messages go to the main
 * thread through a queue.
 */

#include "ztp.h"
#include "ztp_apply.h"
#include "ztp_crypto.h"
#include "ztp_envelope.h"
#include "ztp_manifest.h"

#include "boot_request.h"
#include "net.h"
#include "prov_identity.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/net/net_if.h>
#include <app_version.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(ztp, CONFIG_LOG_DEFAULT_LEVEL);

#define RESULT_GRACE_MS 5000 /* time for the relay to see "done" */
#define BTN_DEBOUNCE_MS 30

/* The relays write 180-byte fragments; accept anything up to the largest ATT
 * attribute so a relay with a different fragment size also works. */
#define FRAG_MAX         512
#define TX_FRAG_MAX      180
#define ENVELOPE_MAX     1280
#define MANIFEST_MAX     (CONFIG_APP_PROV_ZTP_RX_BYTES * 3 / 4 + 4)

enum ztp_status_byte {
	ZTP_ST_IDLE = 0,
	ZTP_ST_RELAYING = 1,
	ZTP_ST_DONE = 2,
	ZTP_ST_ERROR = 3,
};

/* ------------------------------------------------------------------------ */
/* Events for the main thread                                                */
/* ------------------------------------------------------------------------ */

enum ztp_msg {
	MSG_KICK,     /* empty message: send an envelope */
	MSG_RESPONSE, /* the server's response, in rx_buf */
	MSG_OVERFLOW, /* a message larger than rx_buf */
	MSG_BUTTON,   /* sw0 pressed */
};

K_MSGQ_DEFINE(ztp_q, sizeof(uint8_t), 4, 1);

static void post(enum ztp_msg m)
{
	uint8_t v = m;

	(void)k_msgq_put(&ztp_q, &v, K_NO_WAIT);
}

/* ------------------------------------------------------------------------ */
/* sw0: a short press restarts an expired window                             */
/* ------------------------------------------------------------------------ */

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback btn_cb;
static struct k_work_delayable btn_work;
static bool btn_level;

static void btn_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	(void)k_work_reschedule_for_queue(app_net_workq(), &btn_work,
					  K_MSEC(BTN_DEBOUNCE_MS));
}

static void btn_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	bool level = gpio_pin_get_dt(&btn) > 0;

	if (level != btn_level) {
		btn_level = level;
		if (level) {
			post(MSG_BUTTON);
		}
	}
}

static void button_start(void)
{
	if (!gpio_is_ready_dt(&btn)) {
		return;
	}
	k_work_init_delayable(&btn_work, btn_work_fn);
	if (gpio_pin_configure_dt(&btn, GPIO_INPUT) != 0 ||
	    gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH) != 0) {
		LOG_ERR("sw0 configuration failed");
		return;
	}
	btn_level = gpio_pin_get_dt(&btn) > 0;
	gpio_init_callback(&btn_cb, btn_isr, BIT(btn.pin));
	(void)gpio_add_callback(btn.port, &btn_cb);
}

#else /* no sw0 on this board */

static void button_start(void)
{
}

#endif

/* ------------------------------------------------------------------------ */
/* Clock: the relay's time, as an offset from uptime                         */
/* ------------------------------------------------------------------------ */

/* Never a wall-clock set: the offset only stamps the enrollment request, so
 * nothing here can fight SNTP once the application is up. */
static struct k_spinlock clock_lock;
static int64_t clock_offset_s;
static bool clock_known;

static void clock_set(int64_t unix_s)
{
	k_spinlock_key_t key = k_spin_lock(&clock_lock);

	clock_offset_s = unix_s - k_uptime_get() / 1000;
	clock_known = true;
	k_spin_unlock(&clock_lock, key);
}

static int64_t clock_now(void)
{
	k_spinlock_key_t key = k_spin_lock(&clock_lock);
	int64_t now = k_uptime_get() / 1000 + clock_offset_s;

	k_spin_unlock(&clock_lock, key);
	/* With no time yet this stamps 1970 + uptime: the server rejects it as
	 * skewed and answers with server_time, which fixes the next attempt. */
	return now;
}

/* ------------------------------------------------------------------------ */
/* GATT service                                                              */
/* ------------------------------------------------------------------------ */

#define ZTP_UUID(n) \
	BT_UUID_128_ENCODE(0x6e400000 + (n), 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

static const struct bt_uuid_128 uuid_svc = BT_UUID_INIT_128(ZTP_UUID(1));
static const struct bt_uuid_128 uuid_request = BT_UUID_INIT_128(ZTP_UUID(2));
static const struct bt_uuid_128 uuid_response = BT_UUID_INIT_128(ZTP_UUID(3));
static const struct bt_uuid_128 uuid_status = BT_UUID_INIT_128(ZTP_UUID(4));
static const struct bt_uuid_128 uuid_timesync = BT_UUID_INIT_128(ZTP_UUID(5));

static uint8_t cur_status = ZTP_ST_IDLE;

/* Reassembly. The Bluetooth thread fills rx_buf; once a message is complete
 * rx_busy hands it to the main thread, which clears rx_busy when done.
 *
 * The large buffers here and in ztp_envelope.c/ztp_apply.c are __noinit:
 * each is written before it is read, and on the WROOM-32
 * (CONFIG_ESP32_REGION_1_NOINIT) that places them in DRAM region 1 instead
 * of dram0, which the Bluetooth controller has nearly filled. */
static __noinit uint8_t rx_buf[CONFIG_APP_PROV_ZTP_RX_BYTES];
static size_t rx_len;
static bool rx_overflow;
static atomic_t rx_busy;
static __noinit uint8_t frag[2 + FRAG_MAX];
static size_t frag_have;

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &cur_status,
				 sizeof(cur_status));
}

static void rx_reset(void)
{
	rx_len = 0;
	rx_overflow = false;
	frag_have = 0;
}

/* One complete fragment is in frag[]. */
static void rx_fragment(void)
{
	uint16_t n = sys_get_be16(frag);

	if (n == 0) {
		/* End of message. Empty: the kick. Otherwise a response. */
		atomic_set(&rx_busy, 1);
		post(rx_len == 0   ? MSG_KICK
		     : rx_overflow ? MSG_OVERFLOW
				   : MSG_RESPONSE);
		return;
	}
	if (rx_len + n > sizeof(rx_buf)) {
		rx_overflow = true; /* keep reading to the terminator, then fail */
		return;
	}
	memcpy(&rx_buf[rx_len], &frag[2], n);
	rx_len += n;
}

static ssize_t write_request(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
		return 0; /* queued write: the data arrives on execute */
	}
	if (atomic_get(&rx_busy)) {
		return len; /* still working on the last message */
	}
	/* One write carries one fragment; a long (queued) write delivers it in
	 * pieces with increasing offsets. */
	if (offset == 0) {
		frag_have = 0;
	} else if (offset != frag_have) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (frag_have + len > sizeof(frag)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	memcpy(&frag[frag_have], buf, len);
	frag_have += len;

	if (frag_have >= 2 && frag_have >= 2u + sys_get_be16(frag)) {
		/* Bytes past the declared length are ignored, as the Go
		 * peripheral does. */
		rx_fragment();
		frag_have = 0;
	}
	return len;
}

static ssize_t write_timesync(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, const void *buf,
			      uint16_t len, uint16_t offset, uint8_t flags)
{
	const char *s = buf;
	int64_t t;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	while (len && (s[len - 1] == ' ' || s[len - 1] == '\n' ||
		       s[len - 1] == '\r')) {
		len--;
	}
	if (ztp_parse_rfc3339(s, len, &t) == 0) {
		clock_set(t);
		LOG_INF("Time from relay: %.*s", (int)len, s);
	} else {
		LOG_WRN("Ignoring unparseable time from relay");
	}
	return len;
}

BT_GATT_SERVICE_DEFINE(ztp_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),
	BT_GATT_CHARACTERISTIC(&uuid_request.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE,
			       NULL, write_request, NULL),
	BT_GATT_CHARACTERISTIC(&uuid_response.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, NULL, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&uuid_status.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_status, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&uuid_timesync.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_timesync, NULL),
);

/* --- Advertising ---------------------------------------------------------- */

/* Flags (3) + 128-bit UUID (18) + name "ztp" (5) = 26 of the 31-byte primary
 * PDU. Both the UUID and the name must be in the primary advertisement:
 * passive scanners (Windows relays) never see the scan response. The GAP
 * device name, readable after connecting, is the application's hostname. */
static const uint8_t ad_uuid[] = { ZTP_UUID(1) };
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_UUID128_ALL, ad_uuid, sizeof(ad_uuid)),
	BT_DATA_BYTES(BT_DATA_NAME_COMPLETE, 'z', 't', 'p'),
};

static bool adv_wanted;
static bool adv_running;
static struct bt_conn *cur_conn;
static K_SEM_DEFINE(disconnected_sem, 0, 1);

static void adv_start(void)
{
	if (!adv_wanted || adv_running || cur_conn != NULL) {
		return;
	}
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
				  NULL, 0);

	if (err) {
		LOG_ERR("Advertising failed to start (%d)", err);
		return;
	}
	adv_running = true;
}

static void adv_stop(void)
{
	adv_wanted = false;
	if (adv_running) {
		(void)bt_le_adv_stop();
		adv_running = false;
	}
}

static void adv_restart_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	adv_start();
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_fn);

/* As for Improv: Wi-Fi association shares the radio with BLE while a bundle
 * is applied, so ask for a 5 s supervision timeout, inside Apple's limits. */
static const struct bt_le_conn_param conn_param =
	BT_LE_CONN_PARAM_INIT(24, 48, 0, 500);

static void conn_param_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (cur_conn != NULL) {
		int err = bt_conn_le_param_update(cur_conn, &conn_param);

		if (err) {
			LOG_WRN("Connection parameter request failed (%d)", err);
		}
	}
}

static K_WORK_DELAYABLE_DEFINE(conn_param_work, conn_param_fn);

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}
	adv_running = false;
	cur_conn = bt_conn_ref(conn);
	LOG_INF("Relay connected");
	k_work_reschedule(&conn_param_work, K_MSEC(500));
}

static void on_le_param_updated(struct bt_conn *conn, uint16_t interval,
				uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);
	LOG_INF("Connection parameters: interval %u.%02u ms, latency %u, "
		"timeout %u ms", interval * 5 / 4, (interval * 125) % 100,
		latency, timeout * 10);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Relay disconnected (0x%02x)", reason);
	if (cur_conn == conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}
	/* A half-received message is useless now; a complete one the main
	 * thread is working on stays its own. */
	if (!atomic_get(&rx_busy)) {
		rx_reset();
	}
	k_sem_give(&disconnected_sem);
}

static void on_recycled(void)
{
	k_work_submit(&adv_restart_work);
}

BT_CONN_CB_DEFINE(ztp_conn_cbs) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_updated = on_le_param_updated,
	.recycled = on_recycled,
};

/* --- Outgoing ------------------------------------------------------------- */

static void set_status(uint8_t s)
{
	const struct bt_gatt_attr *a;

	cur_status = s;
	a = bt_gatt_find_by_uuid(ztp_svc.attrs, ztp_svc.attr_count,
				 &uuid_status.uuid);
	if (a != NULL && cur_conn != NULL) {
		(void)bt_gatt_notify(cur_conn, a, &cur_status, 1);
	}
}

/* Notify, waiting out a full TX buffer pool rather than dropping a fragment. */
static int notify_retry(const struct bt_gatt_attr *a, const uint8_t *p,
			uint16_t n)
{
	for (int i = 0; i < 250; i++) {
		if (cur_conn == NULL) {
			return -ENOTCONN;
		}
		int rc = bt_gatt_notify(cur_conn, a, p, n);

		if (rc != -ENOMEM) {
			return rc;
		}
		k_sleep(K_MSEC(20));
	}
	return -ENOMEM;
}

/* Send one framed message on the response characteristic. */
static int send_message(const uint8_t *data, size_t len)
{
	static uint8_t buf[2 + TX_FRAG_MAX];
	const struct bt_gatt_attr *a = bt_gatt_find_by_uuid(
		ztp_svc.attrs, ztp_svc.attr_count, &uuid_response.uuid);
	size_t chunk;
	int rc;

	if (a == NULL || cur_conn == NULL) {
		return -ENOTCONN;
	}
	/* Fit each fragment in one notification at the negotiated MTU. */
	chunk = MIN(TX_FRAG_MAX, (size_t)bt_gatt_get_mtu(cur_conn) - 3 - 2);
	if (chunk == 0) {
		return -EINVAL;
	}
	for (size_t off = 0; off < len; off += chunk) {
		size_t n = MIN(chunk, len - off);

		sys_put_be16(n, buf);
		memcpy(&buf[2], &data[off], n);
		rc = notify_retry(a, buf, 2 + n);
		if (rc) {
			return rc;
		}
	}
	sys_put_be16(0, buf);
	return notify_retry(a, buf, 2);
}

/* ------------------------------------------------------------------------ */
/* Provisioning                                                              */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* Worker for the slow parts                                                 */
/* ------------------------------------------------------------------------ */

/*
 * Signing an envelope and applying a bundle each take a second or more (ECC
 * in software, then a Wi-Fi join). They run on this work queue, never on the
 * thread that calls Bluetooth APIs, because of a bug in the ESP32 Bluetooth
 * HAL: its NimBLE porting layer (hal_espressif, npl_os_zephyr.c) implements
 * critical sections with one global nesting count and one global saved
 * irq_lock() key, so a critical section entered on one thread and left on
 * another can hand a thread back with interrupts still locked. On the
 * ESP32-C6, bt_enable() returns to its caller that way (mstatus MIE clear).
 * That thread can then never be preempted: 1.4 s of ECC on it starves the
 * controller's thread, and the relay's link drops with a supervision timeout
 * (HCI 0x08) just as the envelope is sent. This thread starts before
 * bt_enable(), never calls Bluetooth, and so stays preemptible.
 */
#define WORKER_STACK_SIZE 6144

K_THREAD_STACK_DEFINE(ztp_worker_stack, WORKER_STACK_SIZE);
static struct k_work_q ztp_worker;

static int (*job_fn)(void *arg);
static void *job_arg;
static int job_rc;
static K_SEM_DEFINE(job_done, 0, 1);

static void job_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	job_rc = job_fn(job_arg);
	k_sem_give(&job_done);
}

static K_WORK_DEFINE(job_work, job_handler);

/* Run @p fn on the worker and wait for it. Only the main thread calls this,
 * one job at a time. */
static int run_on_worker(int (*fn)(void *arg), void *arg)
{
	job_fn = fn;
	job_arg = arg;
	(void)k_work_submit_to_queue(&ztp_worker, &job_work);
	(void)k_sem_take(&job_done, K_FOREVER);
	return job_rc;
}

/* ------------------------------------------------------------------------ */
/* Provisioning                                                              */
/* ------------------------------------------------------------------------ */

static struct prov_identity ident;
static bool has_ident;
static char device_id[33];
static char mac_str[18];

static void load_identity(void)
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *ll = iface ? net_if_get_link_addr(iface) : NULL;

	has_ident = prov_identity_load(&ident) == 0;
	/* The device's ZTP ID is the application's hostname, which the
	 * application also uses as its Cumulocity default. */
	snprintf(device_id, sizeof(device_id), "%s",
		 has_ident ? ident.hostname : app_net_hostname());
	if (ll != NULL && ll->len == 6) {
		snprintf(mac_str, sizeof(mac_str),
			 "%02x:%02x:%02x:%02x:%02x:%02x", ll->addr[0],
			 ll->addr[1], ll->addr[2], ll->addr[3], ll->addr[4],
			 ll->addr[5]);
	}
}

static void release_rx(void)
{
	rx_reset();
	atomic_clear(&rx_busy);
}

struct kick_job {
	const struct ztp_request_info *info;
	char *out;
	size_t size;
};

static int kick_job(void *arg)
{
	struct kick_job *j = arg;

	return ztp_envelope_build(j->info, j->out, j->size);
}

static void handle_kick(void)
{
	static __noinit char envelope[ENVELOPE_MAX];
	const struct ztp_request_info info = {
		.device_id = device_id,
		.hostname = device_id,
		.model = CONFIG_BOARD,
		.mac = mac_str,
		.agent_version = "zephyr-wifi-provisioner/" APP_VERSION_STRING,
		.unix_s = clock_now(),
		.max_response_bytes = CONFIG_APP_PROV_ZTP_RX_BYTES,
	};
	int n;

	release_rx();
	set_status(ZTP_ST_RELAYING);
	if (!clock_known) {
		LOG_WRN("No time from the relay yet; the server will correct it");
	}
	struct kick_job job = { &info, envelope, sizeof(envelope) };

	n = run_on_worker(kick_job, &job);
	if (n < 0) {
		LOG_ERR("Building the enrollment envelope failed (%d)", n);
		set_status(ZTP_ST_ERROR);
		return;
	}
	LOG_INF("Sending enrollment envelope for \"%s\" (%d bytes)", device_id,
		n);
	n = send_message((const uint8_t *)envelope, n);
	set_status(n == 0 ? ZTP_ST_DONE : ZTP_ST_ERROR);
	if (n) {
		LOG_WRN("Sending the envelope failed (%d)", n);
	}
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

/* Apply the server's response (on the worker). Returns 0 only when the
 * bundle was applied. */
static int handle_response(void *arg)
{
	static __noinit uint8_t manifest[MANIFEST_MAX];
	struct ztp_response r;
	size_t mlen;
	int rc;

	ARG_UNUSED(arg);
	rc = ztp_response_parse((const char *)rx_buf, rx_len, &r);
	if (rc) {
		LOG_ERR("Unreadable server response (%d)", rc);
		return rc;
	}
	if (r.server_time) {
		/* Every response carries the server's clock; keep it for the
		 * next attempt even when this one was accepted. */
		clock_set(r.server_time);
	}
	if (r.status != ZTP_STATUS_ACCEPTED) {
		LOG_WRN("Server answered %s: %.*s; waiting for the relay to retry",
			r.status == ZTP_STATUS_PENDING ? "pending" : "rejected",
			(int)r.reason.len, r.reason.p);
		return -EAGAIN;
	}
	if (r.manifest.len == 0) {
		LOG_ERR("Accepted response carries no manifest%s",
			r.encrypted ? " (encrypted bundles are not supported)"
				    : "");
		return -EPROTO;
	}
	if (CONFIG_APP_PROV_ZTP_SERVER_PUBKEY[0] != '\0') {
		/* Pinning is reserved but not implemented: fail closed rather
		 * than apply a bundle the operator asked to have verified. */
		LOG_ERR("Server key pinning is not implemented; bundle refused");
		return -ENOTSUP;
	}
	if (base64_decode(manifest, sizeof(manifest) - 1, &mlen,
			  (const uint8_t *)r.manifest.p, r.manifest.len) != 0) {
		LOG_ERR("Manifest is not valid base64");
		return -EPROTO;
	}
	LOG_WRN("Applying the bundle without verifying the server's signature "
		"(trust on first use)");
	rc = ztp_apply_manifest((const char *)manifest, mlen, device_id);
	memset(manifest, 0, sizeof(manifest));
	return rc;
}

FUNC_NORETURN void ztp_run(void)
{
	int64_t deadline = k_uptime_get() + CONFIG_APP_WIFI_PROV_WINDOW_S * 1000LL;
	bool idle = false;
	struct boot_request req;
	/* Entered with the button pattern: the application still has working
	 * credentials, so the window's end returns to it. */
	bool return_on_expiry = boot_request_read(&req) == 0 &&
				req.reason == BOOT_REQUEST_OPERATOR;
	char name[CONFIG_BT_DEVICE_NAME_MAX];
	int err;

	/* Before bt_enable(): see the worker note above. */
	k_work_queue_start(&ztp_worker, ztp_worker_stack,
			   K_THREAD_STACK_SIZEOF(ztp_worker_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_thread_name_set(&ztp_worker.thread, "ztp_worker");

	status_led_set_mode(STATUS_LED_PROVISIONING);
	button_start();
	load_identity();

	err = ztp_crypto_init();
	if (err == 0) {
		uint8_t pub[ZTP_P256_POINT_LEN];

		/* Create the identity key now, not on the first kick: key
		 * generation takes as long as a signature. */
		err = ztp_identity_public(pub);
	}
	if (err) {
		LOG_ERR("ZTP crypto unavailable (%d)", err);
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		k_sleep(K_SECONDS(30));
		boot_request_reboot(); /* retry; the request stays set */
	}
	snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, device_id);
	(void)bt_set_name(name);

	adv_wanted = true;
	adv_start();
	LOG_INF("Advertising ZTP as \"ztp\" (device \"%s\") for %d s", device_id,
		CONFIG_APP_WIFI_PROV_WINDOW_S);

	for (;;) {
		int64_t now = k_uptime_get();
		k_timeout_t to = idle ? K_FOREVER : K_MSEC(MAX(deadline - now, 0));
		uint8_t m;

		if (k_msgq_get(&ztp_q, &m, to) == 0) {
			switch (m) {
			case MSG_KICK:
				if (!idle) {
					handle_kick();
				} else {
					release_rx();
				}
				break;
			case MSG_OVERFLOW:
				LOG_ERR("Server response larger than %d bytes; "
					"raise CONFIG_APP_PROV_ZTP_RX_BYTES",
					CONFIG_APP_PROV_ZTP_RX_BYTES);
				release_rx();
				set_status(ZTP_ST_ERROR);
				break;
			case MSG_RESPONSE:
				set_status(ZTP_ST_RELAYING);
				err = run_on_worker(handle_response, NULL);
				release_rx();
				ztp_session_end();
				if (err) {
					set_status(ZTP_ST_ERROR);
					break;
				}
				LOG_INF("Provisioned");
				(void)send_message(NULL, 0);
				set_status(ZTP_ST_DONE);
				adv_stop();
				k_sem_reset(&disconnected_sem);
				if (cur_conn != NULL) {
					(void)k_sem_take(&disconnected_sem,
							 K_MSEC(RESULT_GRACE_MS));
				}
				reboot_now();
			case MSG_BUTTON:
				if (idle) {
					LOG_INF("Button: advertising again");
					idle = false;
					deadline = k_uptime_get() +
						   CONFIG_APP_WIFI_PROV_WINDOW_S * 1000LL;
					status_led_set_mode(STATUS_LED_PROVISIONING);
					adv_wanted = true;
					adv_start();
				}
				break;
			default:
				break;
			}
			continue;
		}

		if (!idle && k_uptime_get() >= deadline) {
			struct app_wifi_creds c;
			bool have = return_on_expiry ||
				    app_wifi_creds_resolve(&c) == 0;

			memset(&c, 0, sizeof(c));
			if (have) {
				LOG_INF("Provisioning window expired; returning "
					"to the configured network");
				reboot_now();
			}
			LOG_INF("Provisioning window expired; idle until the "
				"button is pressed");
			adv_stop();
			if (cur_conn != NULL) {
				(void)bt_conn_disconnect(
					cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			}
			ztp_session_end();
			status_led_set_mode(STATUS_LED_OFF);
			idle = true;
		}
	}
}
