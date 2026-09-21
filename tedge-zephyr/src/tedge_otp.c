/* SPDX-License-Identifier: Apache-2.0
 *
 * One-time password rules for Cumulocity CA enrollment, kept free of network
 * and crypto dependencies so the unit tests can exercise them.
 */

#include "tedge_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

bool tedge_otp_valid(const char *password)
{
	size_t n = password != NULL ? strlen(password) : 0;

	if (n == 0 || n > TEDGE_OTP_MAX) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		/* It travels in an HTTP Basic credential: printable ASCII,
		 * no spaces or control characters. */
		if (password[i] < 0x21 || password[i] > 0x7e) {
			return false;
		}
	}
	return true;
}

int tedge_basic_credential(char *out, size_t size, const char *external_id,
			   const char *password)
{
	int n = snprintf(out, size, "%s:%s", external_id, password);

	/* A truncated credential is a wrong one, so it is an error rather than
	 * a shorter string that would simply fail to authenticate. */
	if (n < 0 || (size_t)n >= size) {
		return -ENAMETOOLONG;
	}
	return n;
}
