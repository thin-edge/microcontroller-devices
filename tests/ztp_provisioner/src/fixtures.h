/* SPDX-License-Identifier: Apache-2.0
 *
 * Fixtures rendered by lab-ztp-provisioner's own pkg/protocol (a8b7ccb):
 * MarshalEnrollText over a p256-suite bundle, exactly what the server writes
 * back to a response_format=text device. The accepted response carries a
 * clear wifi.v2 module, a c8y.v2 module sealed to fx_device_scalar, and an
 * ssh module this device has no applier for. Keys are synthetic.
 */

#ifndef ZTP_TEST_FIXTURES_H_
#define ZTP_TEST_FIXTURES_H_

#include <stdint.h>

static const uint8_t fx_device_scalar[32] = {
	0xaa, 0x2f, 0x51, 0xb8, 0x44, 0x63, 0x6c, 0x77, 0x1c, 0x7e, 0x2e, 0x8c,
	0xb2, 0xc1, 0xc6, 0x04, 0x14, 0x5d, 0x33, 0xb8, 0x58, 0x77, 0xd9, 0x39,
	0x44, 0x48, 0xb7, 0x3f, 0xf1, 0x9b, 0xf1, 0x61
};

static const char fx_accepted[] =
	"protocol_version=1\n"
	"status=accepted\n"
	"server_time=2026-09-21T09:00:00Z\n"
	"manifest.alg=ecdsa-p256-sha256\n"
	"manifest.key_id=server\n"
	"manifest.payload=ZGV2aWNlX2lkPXRlZGdlLW1vZGJ1cwpleHBpcmVzX2F0PTIwMjYtMDktMjJUMDk6MDA6MDBaCmlzc3VlZF9hdD0yMDI2LTA5LTIxVDA5OjAwOjAwWgptb2R1bGUtc2VhbGVkPWM4eS52MiByYXcgQkFndVk0YzVsOWV0eUZJVHR2R2lML014WXV5dU1MQVlVV29pMlg4RDAwMlcrazRnRWJJY0d3MDJMS0ZxdVNDTE9vY2lDODEwdUUvZlFXb2Y0Vk5zbkJBPSBSS0ovY0xRWjFNVGZBTmtOIENmVDRvU3V1V3lPdE5QQW5STGRUYk9UY2p2QkNSNks0KzlkOG0yVG1ReHVNYzZvTDJmMy93Ums2MkszVTkzSjVQTWFhM1RHZVN3UlZGSUorbGpvWmxRVHY5YzRTUmFldjZvU0ZkQnp1OWNocENadlB1STJUWW5ybGkwSldWUVJnVjQyWXl2VnA1dEdKQ1FJWDJjdDdDSThweUVtczY1ekJ2NkM5citBRlM0cDlxZHFPZ2k0Zwptb2R1bGU9c3NoLmF1dGhvcml6ZWRfa2V5cy52MiBXM056YUYwS2EyVjVQWE56YUMxbFpESTFOVEU1SUVGQlFVRUsKbW9kdWxlPXdpZmkudjIgVzI1bGRIZHZjbXRkQ25OemFXUTliR0ZpTFc1bGRIZHZjbXNLY0dGemMzZHZjbVE5ZDJsbWFTMXpaV055WlhRS2EyVjVYMjFuYlhROVYxQkJMVkJUU3dwb2FXUmtaVzQ5Wm1Gc2MyVUtjSEpwYjNKcGRIazlNQW89CnByb3RvY29sX3ZlcnNpb249MQ==\n"
	"manifest.signature=2tqda2QH7LeKAR7URBoBqX9//eYCJRbteGgfHMHl+TZeGjizccFa/8DQuRvx58fI0f94MxehXZcppuouhA3d0A==\n";

static const char fx_pending[] =
	"protocol_version=1\n"
	"status=pending\n"
	"reason=awaiting operator approval\n"
	"retry_after=30\n"
	"server_time=2026-09-21T09:00:00Z\n";

/* The EnrollRequest inputs of testdata/vectors/sign_p256.json, whose
 * canonical bytes are gv_canon in ztp_selftest_vectors.h. */
#define GV_PUBLIC_KEY \
	"BKzPAQbvhY+i2RkzE0aAWni1i7rQuETlx4koeRRhh90mZq2ngbt/ERNyJRqJEGIfY03xKKxI44H9bvkGBzH2lKQ="
#define GV_EPHEMERAL \
	"BFMKtDgfnXmP6Xo8+gtPnooV1FReoVc78zqG2V1ddpor/FhCzddeAq5VEowkFSpKI1iIe/+5xG+fsA/80gaxtZ0="
#define GV_NONCE     "AAECAwQFBgcICQoLDA0ODw=="
#define GV_UNIX_S    1767268800LL /* 2026-01-01T12:00:00Z */

#endif /* ZTP_TEST_FIXTURES_H_ */
