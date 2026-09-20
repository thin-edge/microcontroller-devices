## Why

A device certificate expires. When it does, the device cannot connect, and
cannot be told to do anything about it: no remote access, no firmware update,
no operation of any kind. Recovering a fleet then means visiting each device.
This is the one feature whose absence turns every other feature off, on a
date that is already fixed.

`c8y-direct-spikes` answered how renewal works (unknown U8): EST
`simplereenroll` refuses a client certificate alone with 401, and accepts a
Bearer JWT, which only certificate devices can get. The client already keeps
such a token, already builds CSRs inside PSA, and already unwraps the PKCS#7
reply — enrollment does all three. This change is mostly the decision of
*when* to renew, and making sure a renewal that fails is visible long before
it matters. Roadmap step P6, brought forward.

## What Changes

- **`CONFIG_TEDGE_CERT_RENEWAL` becomes real.** The client reads the
  expiry from its own certificate and renews it through `simplereenroll`
  with its Bearer token once the remaining lifetime falls below
  `TEDGE_CERT_RENEW_BEFORE_DAYS` (default 30).
- **The new certificate is used without an outage:** it is stored, the TLS
  credential is replaced, and the client reconnects on its next cycle. A
  renewal that fails leaves the old certificate in place and is retried.
- **Expiry is visible from the cloud:** a `tedge_Certificate` twin fragment
  with the expiry date and the days remaining, published on every connect
  and after a renewal, so an operator can see a fleet's certificate health
  without asking each device.
- **A certificate that is about to expire raises an alarm** rather than
  failing quietly: if renewal has not succeeded by
  `TEDGE_CERT_RENEW_ALARM_DAYS` (default 7), the client raises a critical
  alarm naming the expiry date.
- **The check is cheap and rare:** once per connect and then daily, reading
  the stored certificate; no extra network traffic unless a renewal is due.

## Non-goals

- Rotating the key. The renewal reuses the device key, as the spike did;
  a key rotation is a separate decision with its own storage and rollback
  story.
- Renewing anything else (the server trust anchors, a tenant CA change).
- Basic-auth devices: they have no certificate and no token, so the feature
  cannot apply, and Kconfig already refuses the combination.
- Recovering a device whose certificate has already expired: that needs
  re-enrollment with a new one-time password, which is onboarding's job.

## Resource constraints

From the spikes, on top of what onboarding already costs:

| Item | Cost |
|---|---|
| Text | small: the CSR, EST and PKCS#7 code is already built for enrollment; this adds the expiry check and the schedule |
| RAM | the same transient buffers as an enrollment, from the module heap, only while renewing |
| Network | one HTTPS request per renewal, plus a reconnect |
| Time | a CSR signs in ~1.2 s on an ESP32-C6; the request is one round trip |

## Capabilities

### New Capabilities

- `tedge-cert-renewal`: when the client renews, how it authenticates, what
  it does with the result, and how an operator sees that it is working.

### Modified Capabilities

- `device-management-features`: certificate renewal stops being an
  unimplemented feature; the dependency on certificate authentication
  becomes load-bearing.

## Impact

- `tedge-zephyr/src/`: a new `tedge_cert_renew.c`, with the CSR, EST request
  and PKCS#7 unwrap shared with `tedge_enroll.c` rather than copied.
- Zephyr facilities: the ones enrollment already uses (PSA, mbedTLS X.509
  write and parse, `http_client`, `tls_credentials`, settings).
- Cumulocity: the tenant's `certificate-authority` feature, and the
  `/.well-known/est/simplereenroll` endpoint.
- The README gains what an operator has to know: when renewal happens, what
  the twin fragment says, and what the alarm means.
