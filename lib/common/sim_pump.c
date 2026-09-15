/* SPDX-License-Identifier: Apache-2.0
 *
 * Pump/motor simulation (CONFIG_APP_SIM_PUMP): a stateful model whose
 * measurements are driven by the control points, so writing a control visibly
 * changes what a client reads. Stepped on CONFIG_APP_SAMPLE_INTERVAL_MS.
 *
 * Model:
 *   commanded speed = 0 unless running AND mode != off; manual -> speed_setpoint,
 *   auto -> a nominal duty. Actual speed ramps toward commanded (motor inertia).
 *   Pump affinity laws: flow  proportional to speed (N),
 *                       pressure proportional to speed^2 (N^2).
 *   rpm and vibration rise with speed; motor temperature follows a first-order
 *   lag toward an ambient+load target; run_hours accrues only while running; an
 *   over-temperature trip sets the fault flag. Jitter is deterministic (no RNG).
 */

#include "data_source.h"
#include "controls.h"

#include <math.h>
#include <zephyr/kernel.h>

/* Nameplate / model constants. */
#define RPM_MAX        2900.0
#define FLOW_MAX       150.0   /* L/min at full speed */
#define PRESSURE_MAX   6.0     /* bar at full speed (head ~ N^2) */
#define VIB_IDLE       0.3     /* mm/s residual when spinning slowly */
#define VIB_GAIN       3.8     /* mm/s added at full speed */
#define AMBIENT_C      25.0
#define TEMP_RISE_C    70.0    /* full-speed steady-state rise (target ~95 C) */
#define OVERTEMP_C     85.0    /* fault trip threshold */
#define RAMP_K         0.25    /* speed inertia (fraction toward command / step) */
#define THERMAL_K      0.03    /* thermal lag (fraction toward target / step) */
#define AUTO_DUTY      75.0    /* nominal % in auto mode */

enum { MODE_OFF = 0, MODE_AUTO = 1, MODE_MANUAL = 2 };

/* Measurement indices (must match the Modbus register map). */
enum {
	M_FLOW = 0, M_PRESSURE, M_MOTOR_TEMP, M_RPM, M_VIBRATION, M_RUN_HOURS,
	M_COUNT
};

static const struct data_measurement descriptors[M_COUNT] = {
	[M_FLOW]       = { .name = "flow_lpm",      .unit = "L/min" },
	[M_PRESSURE]   = { .name = "pressure_bar",  .unit = "bar" },
	[M_MOTOR_TEMP] = { .name = "motor_temp_c",  .unit = "Cel" },
	[M_RPM]        = { .name = "rpm",           .unit = "rpm" },
	[M_VIBRATION]  = { .name = "vibration_mms", .unit = "mm/s" },
	[M_RUN_HOURS]  = { .name = "run_hours",     .unit = "h" },
};

/* Current values, refreshed each step and read by data_source_sample(). */
static double values[M_COUNT];
static double speed_pct;       /* actual (ramped) speed */
static double motor_temp_c = AMBIENT_C;
static double run_hours;
static bool fault;
static uint32_t step_n;

static struct k_work_delayable step_work;

static double dt_seconds(void)
{
	return (double)CONFIG_APP_SAMPLE_INTERVAL_MS / 1000.0;
}

static void sim_step(struct k_work *work)
{
	ARG_UNUSED(work);

	const bool running = app_control_running();
	const int mode = app_control_mode();
	const bool active = running && (mode != MODE_OFF);

	/* Commanded speed from the controls. */
	double cmd = 0.0;

	if (active) {
		if (mode == MODE_MANUAL) {
			cmd = (double)app_control_setpoint();
		} else { /* auto: nominal duty with gentle variation */
			cmd = AUTO_DUTY + 5.0 * sin((double)step_n * 0.05);
		}
	}
	if (cmd < 0.0) {
		cmd = 0.0;
	}
	if (cmd > 100.0) {
		cmd = 100.0;
	}

	/* Actual speed ramps toward commanded (inertia). */
	speed_pct += (cmd - speed_pct) * RAMP_K;
	if (speed_pct < 0.05) {
		speed_pct = 0.0;
	}
	const double s = speed_pct / 100.0;

	/* Deterministic small jitter for liveliness. */
	const double j = sin((double)step_n * 0.7);

	values[M_RPM] = s * RPM_MAX;
	values[M_FLOW] = s * FLOW_MAX * (1.0 + 0.01 * j);          /* flow ~ N   */
	values[M_PRESSURE] = s * s * PRESSURE_MAX * (1.0 + 0.01 * j); /* head ~ N^2 */
	values[M_VIBRATION] = (s > 0.0 ? VIB_IDLE + s * VIB_GAIN : 0.0)
			      + 0.05 * j;

	/* First-order thermal model toward an ambient+load target. */
	const double target = AMBIENT_C + s * TEMP_RISE_C;

	motor_temp_c += (target - motor_temp_c) * THERMAL_K;
	values[M_MOTOR_TEMP] = motor_temp_c;
	fault = motor_temp_c > OVERTEMP_C;

	/* run_hours accrues only while running. */
	if (active) {
		run_hours += dt_seconds() / 3600.0;
	}
	values[M_RUN_HOURS] = run_hours;

	step_n++;
	k_work_reschedule(&step_work, K_MSEC(CONFIG_APP_SAMPLE_INTERVAL_MS));
}

void data_source_init(void)
{
	motor_temp_c = AMBIENT_C;
	values[M_MOTOR_TEMP] = motor_temp_c;
	k_work_init_delayable(&step_work, sim_step);
	k_work_reschedule(&step_work, K_NO_WAIT);
}

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
	if (index >= M_COUNT) {
		return 0.0;
	}
	return values[index];
}

bool app_sim_fault(void)
{
	return fault;
}
