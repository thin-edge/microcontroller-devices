/* SPDX-License-Identifier: Apache-2.0
 *
 * What to do about the certificate, given how many days of it are left.
 * Pure, so the awkward cases (expired, expiring today, a clock that is not
 * set yet) are unit-tested rather than reasoned about.
 */

#include "tedge_internal.h"

enum tedge_cert_action tedge_cert_action(int days_left, int renew_before_days,
					 int alarm_days, bool renewed)
{
	if (days_left < 0) {
		/* No clock, or no certificate: nothing can be decided yet. */
		return TEDGE_CERT_WAIT;
	}
	if (days_left > renew_before_days) {
		return TEDGE_CERT_WAIT;
	}
	if (!renewed) {
		return TEDGE_CERT_RENEW;
	}
	/* A renewal was attempted and failed: say so while there is time. */
	if (alarm_days > 0 && days_left <= alarm_days) {
		return TEDGE_CERT_ALARM;
	}
	return TEDGE_CERT_RETRY;
}
