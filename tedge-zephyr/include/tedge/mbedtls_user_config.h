/* SPDX-License-Identifier: Apache-2.0
 *
 * mbedTLS user configuration for images built with the tedge-zephyr
 * profiles. Zephyr includes it after its own mbedTLS config
 * (CONFIG_MBEDTLS_USER_CONFIG_FILE="tedge/mbedtls_user_config.h"), so it
 * can override what that config derived from Kconfig.
 *
 * Split TLS record buffers. Zephyr sizes both the input and the output
 * record buffer from CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN. The client only
 * ever *sends* small records: MQTT publishes, HTTP request headers, the
 * chunks a producer writes to an upload, WebSocket frames. Only received
 * records need the full size the server may use (16 KB from Cumulocity's
 * MQTT service and its binary downloads). A 4 KB output buffer therefore
 * costs nothing in function - a larger write is split into several
 * records, which is ordinary TLS and transparent to MQTT, HTTP and
 * WebSocket - and saves per concurrent session:
 *
 *   16 KB records: 12 KB    8 KB records: 4 KB
 *
 * which comes off the peak the mbedTLS heap has to hold. The heap sizes
 * in the profiles and board settings are measured with this in place
 * (reduce-memory-footprint, tier 2).
 *
 * One interaction to keep straight: with the default
 * CONFIG_NET_SOCKETS_TLS_SET_MAX_FRAGMENT_LENGTH, Zephyr advertises the
 * *smaller* of the two buffers as the RFC 6066 maximum fragment length
 * whenever it is below 16 KB, so this header alone would also ask the
 * server for 4 KB records. The 16 KB profiles therefore switch that
 * option off (they never advertised one, so the server's records are
 * unchanged), and a board that lowers CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN
 * below 16 KB keeps it on, as the ESP32-WROOM-32 settings do: there the
 * server must be told to stay within the input buffer.
 */
#ifndef TEDGE_MBEDTLS_USER_CONFIG_H_
#define TEDGE_MBEDTLS_USER_CONFIG_H_

#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

/* These devices are TLS clients only, but Zephyr's mbedTLS config defines
 * the server role unconditionally and the linker keeps its handshake
 * (mbedtls_ssl_handshake_server_step, 3,952 B, plus the SNI parser) because
 * the socket layer references it. Dropping the role removes code that is
 * never reached (reduce-memory-footprint, tier 3). */
#undef MBEDTLS_SSL_SRV_C

#endif /* TEDGE_MBEDTLS_USER_CONFIG_H_ */
