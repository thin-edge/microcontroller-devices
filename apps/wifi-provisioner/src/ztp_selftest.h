/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ZTP_SELFTEST_H_
#define ZTP_SELFTEST_H_

/**
 * @brief Check the ZTP p256 crypto against the server's reference vectors.
 *
 * @return 0 when every check passed.
 */
int ztp_selftest_run(void);

#endif /* ZTP_SELFTEST_H_ */
