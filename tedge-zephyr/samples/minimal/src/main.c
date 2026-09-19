/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tedge/tedge.h>

LOG_MODULE_REGISTER(tedge_minimal, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("tedge-zephyr %s", tedge_version());
	return 0;
}
