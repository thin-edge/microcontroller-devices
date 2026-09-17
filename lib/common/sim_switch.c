/* SPDX-License-Identifier: Apache-2.0
 *
 * Switch/router simulation (CONFIG_APP_SIM_SWITCH): models a small managed
 * switch/router as a fixed set of Ethernet interfaces. Traffic counters advance
 * only on interfaces that are operationally up; one designated interface flaps
 * its link every CONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS steps (0 = never) so a
 * consuming frontend (lib/snmp) has linkUp/linkDown transitions to report.
 * Stepped on CONFIG_APP_SAMPLE_INTERVAL_MS.
 *
 * Everything is deterministic (no RNG): traffic and flapping are driven by the
 * step counter, so behaviour is reproducible across runs and boards.
 *
 * It also satisfies the generic data_source.h contract by exposing a few
 * aggregate scalars (up-interface count, total in/out octets), so the firmware
 * builds like any other simulation and a non-SNMP frontend could still show a
 * summary.
 */

#include "data_source.h"
#include "diag.h"
#include "sim_switch.h"

#include <string.h>
#include <zephyr/kernel.h>

/* Nameplate constants. */
#define IF_SPEED_BPS   1000000000U /* 1 Gbit/s */
#define IF_MTU         1500U
#define IF_TYPE_ETHER  6           /* ethernetCsmacd */

/* Per-interface simulated state. Counters are kept as 32-bit and allowed to
 * wrap, matching Counter32 semantics. */
struct sim_if {
	const char *descr;
	enum sim_if_status admin;
	enum sim_if_status oper;
	uint32_t counters[SIM_IF_COUNTER_COUNT];
	uint32_t base_rate; /* nominal octets/step when up, varies per port */
};

static struct sim_if ifs[SIM_SWITCH_MAX_IF];
static size_t if_count;
/* Which interface flaps: the last one is treated as an uplink and kept stable, so
 * a mid access port is chosen. The period is CONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS,
 * copied into a variable rather than used as a literal so that a configured 0
 * (never flap) cannot reach the modulo in sim_step(). */
static size_t flap_idx;      /* interface chosen to flap its link */
static uint32_t flap_period; /* steps between its transitions; 0 = never flap */
static uint32_t step_n;

/* Aggregate scalars for the generic data_source.h view. */
enum { M_IF_UP = 0, M_IN_OCTETS, M_OUT_OCTETS, M_COUNT };

static const struct data_measurement descriptors[M_COUNT] = {
	[M_IF_UP]      = { .name = "if_up",           .unit = "count" },
	[M_IN_OCTETS]  = { .name = "in_octets_total",  .unit = "octets" },
	[M_OUT_OCTETS] = { .name = "out_octets_total", .unit = "octets" },
};

static struct k_work_delayable step_work;

static size_t clamp_count(void)
{
	size_t n = CONFIG_APP_SIM_SWITCH_IF_COUNT;

	if (n < 1) {
		n = 1;
	}
	if (n > SIM_SWITCH_MAX_IF) {
		n = SIM_SWITCH_MAX_IF;
	}
	return n;
}

static void sim_step(struct k_work *work)
{
	ARG_UNUSED(work);

	step_n++;

	/* Flap the designated interface's link on the configured cadence so a
	 * frontend observes real up<->down transitions. */
	if (flap_period > 0 && if_count > 1 && (step_n % flap_period) == 0) {
		ifs[flap_idx].oper =
			(ifs[flap_idx].oper == SIM_IF_UP) ? SIM_IF_DOWN : SIM_IF_UP;
	}

	/* Advance traffic counters only on operationally-up interfaces. Rates are
	 * per-port and lightly modulated by the step counter for liveliness. */
	for (size_t i = 0; i < if_count; i++) {
		if (ifs[i].oper != SIM_IF_UP) {
			continue;
		}

		uint32_t rate = ifs[i].base_rate + (step_n & 0x3F) * 4U;
		uint32_t pkts = 1U + (rate / 512U);

		ifs[i].counters[SIM_IF_IN_OCTETS]     += rate;
		ifs[i].counters[SIM_IF_OUT_OCTETS]    += rate + (rate >> 3);
		ifs[i].counters[SIM_IF_IN_UCAST_PKTS] += pkts;
		ifs[i].counters[SIM_IF_OUT_UCAST_PKTS] += pkts;
	}

	k_work_reschedule(&step_work, K_MSEC(CONFIG_APP_SAMPLE_INTERVAL_MS));
}

void data_source_init(void)
{
	static const char *const names[SIM_SWITCH_MAX_IF] = {
		"GigabitEthernet0/1", "GigabitEthernet0/2", "GigabitEthernet0/3",
		"GigabitEthernet0/4", "GigabitEthernet0/5", "GigabitEthernet0/6",
		"GigabitEthernet0/7", "Uplink0/0",
	};

	if_count = clamp_count();
	step_n = 0;
	flap_period = CONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS;
	/* Flap a middle access port when available, else the first interface. */
	flap_idx = (if_count > 2) ? (if_count - 2) : 0;

	for (size_t i = 0; i < if_count; i++) {
		memset(&ifs[i], 0, sizeof(ifs[i]));
		ifs[i].descr = names[i];
		ifs[i].admin = SIM_IF_UP;
		ifs[i].oper = SIM_IF_UP;
		/* Vary nominal load per port so counters differ. */
		ifs[i].base_rate = 800U + (uint32_t)i * 250U;
	}

	k_work_init_delayable(&step_work, sim_step);
	app_diag_watch_work("sim", &step_work);
	k_work_reschedule(&step_work, K_MSEC(CONFIG_APP_SAMPLE_INTERVAL_MS));
}

/* --- typed switch accessors (sim_switch.h) --- */

size_t sim_switch_if_count(void)
{
	return if_count;
}

const char *sim_switch_if_descr(size_t idx)
{
	return (idx < if_count) ? ifs[idx].descr : "?";
}

int sim_switch_if_type(size_t idx)
{
	return (idx < if_count) ? IF_TYPE_ETHER : 0;
}

uint32_t sim_switch_if_mtu(size_t idx)
{
	return (idx < if_count) ? IF_MTU : 0;
}

uint32_t sim_switch_if_speed(size_t idx)
{
	return (idx < if_count) ? IF_SPEED_BPS : 0;
}

enum sim_if_status sim_switch_if_admin_status(size_t idx)
{
	return (idx < if_count) ? ifs[idx].admin : SIM_IF_DOWN;
}

enum sim_if_status sim_switch_if_oper_status(size_t idx)
{
	return (idx < if_count) ? ifs[idx].oper : SIM_IF_DOWN;
}

uint32_t sim_switch_if_counter(size_t idx, enum sim_if_counter c)
{
	if (idx >= if_count || c >= SIM_IF_COUNTER_COUNT) {
		return 0;
	}
	return ifs[idx].counters[c];
}

/* --- generic data_source.h view (aggregates) --- */

size_t data_source_count(void)
{
	return M_COUNT;
}

const struct data_measurement *data_source_descriptor(size_t index)
{
	if (index >= M_COUNT) {
		return NULL;
	}
	return &descriptors[index];
}

double data_source_sample(size_t index)
{
	double up = 0.0, in_oct = 0.0, out_oct = 0.0;

	for (size_t i = 0; i < if_count; i++) {
		if (ifs[i].oper == SIM_IF_UP) {
			up += 1.0;
		}
		in_oct += (double)ifs[i].counters[SIM_IF_IN_OCTETS];
		out_oct += (double)ifs[i].counters[SIM_IF_OUT_OCTETS];
	}

	switch (index) {
	case M_IF_UP:
		return up;
	case M_IN_OCTETS:
		return in_oct;
	case M_OUT_OCTETS:
		return out_oct;
	default:
		return 0.0;
	}
}

bool app_sim_fault(void)
{
	/* A switch reports a "fault" if any admin-up interface is operationally
	 * down (a link fault). */
	for (size_t i = 0; i < if_count; i++) {
		if (ifs[i].admin == SIM_IF_UP && ifs[i].oper != SIM_IF_UP) {
			return true;
		}
	}
	return false;
}
