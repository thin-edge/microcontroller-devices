/* SPDX-License-Identifier: Apache-2.0
 *
 * Data source abstraction — decoupled from the OPC-UA layer.
 *
 * The OPC-UA address space asks this module "how many measurements are there,
 * what are they called, and what is the current value?". A real sensor backend
 * can replace the simulated implementation without any change to the OPC-UA
 * code, as long as it satisfies this interface.
 */
#ifndef APP_DATA_SOURCE_H_
#define APP_DATA_SOURCE_H_

#include <stddef.h>

/** A measurement descriptor. */
struct data_measurement {
	const char *name; /**< Stable identifier / browse name, e.g. "temperature". */
	const char *unit; /**< Engineering unit, e.g. "Cel". */
};

/** Initialise the data source (called once at startup). */
void data_source_init(void);

/** @return number of measurements this source provides. */
size_t data_source_count(void);

/**
 * @param index measurement index in [0, data_source_count()).
 * @return the measurement's static descriptor, or NULL if out of range.
 */
const struct data_measurement *data_source_descriptor(size_t index);

/**
 * Sample the current value for a measurement.
 *
 * @param index measurement index in [0, data_source_count()).
 * @return current value; 0.0 if index is out of range.
 */
double data_source_sample(size_t index);

#endif /* APP_DATA_SOURCE_H_ */
