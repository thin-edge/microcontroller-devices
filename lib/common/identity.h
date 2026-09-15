/* SPDX-License-Identifier: Apache-2.0
 *
 * Device identity + firmware build info, protocol-independent. Protocol
 * frontends (OPC-UA, SNMP, Modbus, ...) expose these strings over their wire
 * protocol so an operator/collector can identify a device and tell which
 * firmware it runs. All returned strings have static lifetime.
 */
#ifndef APP_IDENTITY_H_
#define APP_IDENTITY_H_

/** Unique device id (network hostname incl. MAC-derived suffix). */
const char *app_identity_device_id(void);

/** Firmware image/application name (CONFIG_APP_FIRMWARE_NAME). */
const char *app_identity_firmware_name(void);

/** Firmware version string (APP_VERSION_STRING, from the app's VERSION file). */
const char *app_identity_firmware_version(void);

/** Firmware build date/time (compiler __DATE__ " " __TIME__ of this build). */
const char *app_identity_build_timestamp(void);

#endif /* APP_IDENTITY_H_ */
