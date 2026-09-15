/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulated data source: produces changing, in-range synthetic values so the
 * OPC-UA server has something meaningful to serve before real sensors are
 * wired in. Values vary smoothly over time (deterministically, from the system
 * uptime) so a client polling the same node sees it change.
 */

#include "data_source.h"

#include <math.h>
#include <zephyr/kernel.h>

/* Each measurement oscillates as base + amplitude * sin(2*pi*t / period). */
struct sim_signal {
	struct data_measurement desc;
	double base;
	double amplitude;
	double period_s;
};

static const struct sim_signal signals[] = {
	{ .desc = { .name = "temperature", .unit = "Cel" },
	  .base = 22.0, .amplitude = 3.0,  .period_s = 60.0 },
	{ .desc = { .name = "humidity",    .unit = "%RH" },
	  .base = 45.0, .amplitude = 10.0, .period_s = 120.0 },
	{ .desc = { .name = "pressure",    .unit = "hPa" },
	  .base = 1013.0, .amplitude = 5.0, .period_s = 300.0 },
};

#define SIM_COUNT ARRAY_SIZE(signals)

void data_source_init(void)
{
	/* Nothing to initialise for the simulated source. */
}

size_t data_source_count(void)
{
	return SIM_COUNT;
}

const struct data_measurement *data_source_descriptor(size_t index)
{
	if (index >= SIM_COUNT) {
		return NULL;
	}
	return &signals[index].desc;
}

double data_source_sample(size_t index)
{
	if (index >= SIM_COUNT) {
		return 0.0;
	}

	const struct sim_signal *s = &signals[index];
	double t = (double)k_uptime_get() / 1000.0; /* seconds */
	double phase = 2.0 * 3.14159265358979323846 * t / s->period_s;

	return s->base + s->amplitude * sin(phase);
}
