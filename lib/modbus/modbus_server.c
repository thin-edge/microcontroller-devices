/* SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP server frontend: maps the shared pump/motor data model (lib/common)
 * onto the four Modbus object types and serves them over TCP. Uses Zephyr's
 * modbus subsystem in raw-ADU server mode; a listener thread owns the socket and
 * shuttles MBAP-framed ADUs to/from the Modbus core (mirrors upstream
 * samples/subsys/modbus/tcp_server). Single client at a time; transient socket
 * errors are retried, never fatal.
 */

#include "modbus_server.h"

#include "data_source.h"
#include "controls.h"
#include "net.h"

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/unistd.h>

LOG_MODULE_REGISTER(app_modbus, CONFIG_LOG_DEFAULT_LEVEL);

/* Measurement indices as provided by the pump simulation (data_source order). */
enum { MEAS_FLOW = 0, MEAS_PRESSURE, MEAS_MOTOR_TEMP, MEAS_RPM,
       MEAS_VIBRATION, MEAS_RUN_HOURS };

/* --- helpers: pack a float / 32-bit value into a big-endian register pair --- */
static uint32_t float_bits(double v)
{
	float f = (float)v;
	uint32_t bits;

	memcpy(&bits, &f, sizeof(bits));
	return bits;
}

/* Return register `word` (0 = high, 1 = low) of a 32-bit big-endian value. */
static uint16_t be_word(uint32_t v, int word)
{
	return (word == 0) ? (uint16_t)(v >> 16) : (uint16_t)(v & 0xFFFF);
}

/* --- Modbus object callbacks (register map — see the spec/README) --- */

static int input_reg_rd(uint16_t addr, uint16_t *reg)
{
	switch (addr) {
	case 0: /* flow_lpm ×10 */
		*reg = (uint16_t)(data_source_sample(MEAS_FLOW) * 10.0);
		return 0;
	case 1: /* pressure_bar ×100 */
		*reg = (uint16_t)(data_source_sample(MEAS_PRESSURE) * 100.0);
		return 0;
	case 2: /* motor_temp_c ×10 (signed) */
		*reg = (uint16_t)(int16_t)(data_source_sample(MEAS_MOTOR_TEMP) * 10.0);
		return 0;
	case 3: /* rpm (direct) */
		*reg = (uint16_t)(data_source_sample(MEAS_RPM));
		return 0;
	case 4: /* vibration_mms ×100 */
		*reg = (uint16_t)(data_source_sample(MEAS_VIBRATION) * 100.0);
		return 0;
	case 10: /* run time (seconds) — 32-bit counter, high word */
	case 11: /* ... low word */
		*reg = be_word((uint32_t)(data_source_sample(MEAS_RUN_HOURS) * 3600.0),
			       addr - 10);
		return 0;
	case 20: /* flow_lpm float, high word */
	case 21: /* ... low word */
		*reg = be_word(float_bits(data_source_sample(MEAS_FLOW)), addr - 20);
		return 0;
	case 22: /* pressure_bar float */
	case 23:
		*reg = be_word(float_bits(data_source_sample(MEAS_PRESSURE)), addr - 22);
		return 0;
	case 24: /* motor_temp_c float */
	case 25:
		*reg = be_word(float_bits(data_source_sample(MEAS_MOTOR_TEMP)), addr - 24);
		return 0;
	default:
		return -ENOTSUP; /* -> illegal data address */
	}
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	switch (addr) {
	case 0: /* speed_setpoint (%) */
		*reg = (uint16_t)app_control_setpoint();
		return 0;
	case 1: /* mode (0=off,1=auto,2=manual) */
		*reg = (uint16_t)app_control_mode();
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	switch (addr) {
	case 0: /* speed_setpoint — clamped by the shared control API */
		app_control_set_setpoint((int32_t)reg);
		return 0;
	case 1: /* mode — clamped 0..2 */
		app_control_set_mode((int)reg);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int coil_rd(uint16_t addr, bool *state)
{
	if (addr != 0) {
		return -ENOTSUP;
	}
	*state = app_control_running();
	return 0;
}

static int coil_wr(uint16_t addr, bool state)
{
	if (addr != 0) {
		return -ENOTSUP;
	}
	app_control_set_running(state);
	return 0;
}

static int discrete_input_rd(uint16_t addr, bool *state)
{
	switch (addr) {
	case 0: /* running mirror */
		*state = app_control_running();
		return 0;
	case 1: /* fault (simulated over-temp) */
		*state = app_sim_fault();
		return 0;
	case 2: /* network connected */
		*state = app_net_is_connected();
		return 0;
	default:
		return -ENOTSUP;
	}
}

static struct modbus_user_callbacks mbs_cbs = {
	.coil_rd = coil_rd,
	.coil_wr = coil_wr,
	.discrete_input_rd = discrete_input_rd,
	.input_reg_rd = input_reg_rd,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
};

/* --- raw-ADU plumbing (single client) --- */
static struct modbus_adu tmp_adu;
static K_SEM_DEFINE(response_ready, 0, 1);
static int server_iface;

static int server_raw_tx(const int iface, const struct modbus_adu *adu,
			 void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	tmp_adu.trans_id = adu->trans_id;
	tmp_adu.proto_id = adu->proto_id;
	tmp_adu.length = adu->length;
	tmp_adu.unit_id = adu->unit_id;
	tmp_adu.fc = adu->fc;
	memcpy(tmp_adu.data, adu->data,
	       MIN(adu->length, CONFIG_MODBUS_BUFFER_SIZE));
	k_sem_give(&response_ready);
	return 0;
}

static const struct modbus_iface_param server_param = {
	.mode = MODBUS_MODE_RAW,
	.server = {
		.user_cb = &mbs_cbs,
		.unit_id = CONFIG_APP_MODBUS_UNIT_ID,
	},
	.rawcb.raw_tx_cb = server_raw_tx,
	.rawcb.user_data = NULL,
};

static int modbus_reply(int client, struct modbus_adu *adu)
{
	uint8_t header[MODBUS_MBAP_AND_FC_LENGTH];

	modbus_raw_put_header(adu, header);
	if (send(client, header, sizeof(header), 0) < 0) {
		return -errno;
	}
	if (adu->length && send(client, adu->data, adu->length, 0) < 0) {
		return -errno;
	}
	return 0;
}

/* Serve one request; returns 0 on success, negative on client close/error. */
static int serve_once(int client)
{
	uint8_t header[MODBUS_MBAP_AND_FC_LENGTH];
	int rc;

	rc = recv(client, header, sizeof(header), MSG_WAITALL);
	if (rc <= 0) {
		return rc == 0 ? -ENOTCONN : -errno;
	}

	modbus_raw_get_header(&tmp_adu, header);
	if (tmp_adu.length > 0) {
		rc = recv(client, tmp_adu.data, tmp_adu.length, MSG_WAITALL);
		if (rc <= 0) {
			return rc == 0 ? -ENOTCONN : -errno;
		}
	}

	if (modbus_raw_submit_rx(server_iface, &tmp_adu) != 0) {
		LOG_WRN("Failed to submit raw ADU");
		return 0; /* keep the connection; skip this frame */
	}

	if (k_sem_take(&response_ready, K_MSEC(1000)) != 0) {
		modbus_raw_set_server_failure(&tmp_adu);
	}

	return modbus_reply(client, &tmp_adu);
}

/* --- listener thread --- */
#define MODBUS_THREAD_STACK_SIZE 3072
#define MODBUS_THREAD_PRIORITY   6

K_THREAD_STACK_DEFINE(modbus_stack, MODBUS_THREAD_STACK_SIZE);
static struct k_thread modbus_thread;

static void modbus_listener(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		int serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		if (serv < 0) {
			LOG_ERR("socket() failed (%d) — retry", errno);
			k_sleep(K_SECONDS(1));
			continue;
		}

		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_ANY),
			.sin_port = htons(CONFIG_APP_MODBUS_PORT),
		};

		if (bind(serv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
		    listen(serv, 1) < 0) {
			LOG_ERR("bind/listen failed (%d) — retry", errno);
			close(serv);
			k_sleep(K_SECONDS(1));
			continue;
		}

		LOG_INF("Modbus TCP server listening on port %d (unit id %d)",
			CONFIG_APP_MODBUS_PORT, CONFIG_APP_MODBUS_UNIT_ID);

		while (true) {
			struct sockaddr_in cli;
			socklen_t cli_len = sizeof(cli);
			int client = accept(serv, (struct sockaddr *)&cli, &cli_len);

			if (client < 0) {
				/* Transient (e.g. ENOMEM under pressure): retry,
				 * do not tear down the listener. */
				LOG_WRN("accept() failed (%d) — retry", errno);
				k_sleep(K_MSEC(200));
				continue;
			}

			LOG_INF("Modbus client connected");
			while (serve_once(client) == 0) {
				/* keep serving this client */
			}
			LOG_INF("Modbus client disconnected");
			close(client);
		}
	}
}

int modbus_server_start(void)
{
	char iface_name[] = "RAW_0";
	int err;

	/* Start the data model / simulation (starts the pump sim's step timer). */
	data_source_init();

	server_iface = modbus_iface_get_by_name(iface_name);
	if (server_iface < 0) {
		LOG_ERR("No raw Modbus iface '%s' (check CONFIG_MODBUS_NUMOF_RAW_ADU)",
			iface_name);
		return server_iface;
	}

	err = modbus_init_server(server_iface, server_param);
	if (err < 0) {
		LOG_ERR("modbus_init_server failed (%d)", err);
		return err;
	}

	k_thread_create(&modbus_thread, modbus_stack,
			K_THREAD_STACK_SIZEOF(modbus_stack),
			modbus_listener, NULL, NULL, NULL,
			MODBUS_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&modbus_thread, "modbus");
	return 0;
}
