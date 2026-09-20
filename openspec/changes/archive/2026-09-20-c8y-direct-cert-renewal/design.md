## Context

The client enrolls itself with the Cumulocity CA (`c8y-direct-core`): a
P-256 key in PSA, a CSR signed inside PSA, EST `simpleenroll`, and the
PKCS#7 reply unwrapped to a DER certificate in settings. Renewal is the same
machinery pointed at `simplereenroll`, so this design is mostly about
*when*, and about what happens when it does not work.

From Spike C (archived):
- `simplereenroll` with **mutual TLS alone gets 401**; with
  `Authorization: Bearer <JWT>` it returns a new certificate. The token comes
  from `s/uat`, which only certificate devices receive, and the client
  already keeps a current one.
- The reply is the same base64 PKCS#7 as enrollment (~511 B for a 332 B
  certificate), and the same ASN.1 walk unwraps it.
- Signing a CSR takes ~1.2 s on an ESP32-C6.

## Goals / Non-Goals

**Goals:** a certificate that renews itself well before it expires; a
failure that is visible from the cloud long before it bites; no outage
caused by the renewal itself; no new code paths for the CSR, the request or
the reply.

**Non-Goals:** key rotation, recovering an already-expired device, and
renewing the trust anchors.

## Decisions

### D1: Renew on remaining lifetime, checked from the certificate itself

The client parses its stored certificate with `mbedtls_x509_crt_parse_der`
and reads `valid_to`. It renews when the remaining lifetime is less than
`TEDGE_CERT_RENEW_BEFORE_DAYS` (default 30). The check runs once after each
connect and then daily, on the client thread; it costs a parse of a 332-byte
certificate and no network traffic.

*Alternative: remember the issue date and renew after a fixed interval.*
Rejected: it drifts from what the CA actually issued, and a certificate
replaced by hand would be renewed on the wrong schedule.

The realtime clock decides this, so the check only runs once the clock is
valid — the client already refuses to connect before that.

### D2: The token is the credential, so renewal waits for a session

`simplereenroll` needs a Bearer token, so the client renews only while it has
one: after a successful connect. A device that cannot connect cannot renew,
which is why the margin is 30 days and not 3.

The request reuses `tedge_enroll.c`'s CSR, HTTPS and PKCS#7 code, moved into
shared internal functions rather than copied. The key is the same one: the
CSR is signed inside PSA, and the certificate that comes back belongs to the
same key pair.

### D3: The new certificate takes effect on the next connect

Order: store the new certificate in settings, replace the TLS credential
(delete and add at `TEDGE_TAG_DEVICE`), then disconnect so the next
connection uses it. Zephyr's `tls_credentials` are read when a TLS context is
created, so an open session keeps using the old certificate; a clean
disconnect and reconnect (the core's back-off starts at 3 s) is the simplest
way to pick up the new one, and it happens while the old certificate is still
valid.

If storing or registering fails, the old certificate stays in place and the
client keeps running on it; the renewal is retried on the next daily check.

### D4: Failure is loud before it is fatal

- Each failed attempt logs why and is retried: hourly while the remaining
  lifetime is under the renewal margin.
- When the remaining lifetime falls below `TEDGE_CERT_RENEW_ALARM_DAYS`
  (default 7) and the certificate has still not been renewed, the client
  raises a **critical alarm** naming the expiry date, and clears it once a
  renewal succeeds.
- The `tedge_Certificate` twin fragment carries the expiry and the days
  remaining on every connect, so a fleet's certificate health is a
  query rather than an investigation:

```json
{"expires":"2027-09-20T10:15:00Z","daysRemaining":365,"renewals":2}
```

An alarm needs the alarm API, which belongs to telemetry (P4) and is not
built yet; until then the client raises it as a SmartREST alarm directly
(`301,c8y_CertificateExpiring,…`), which is what the alarm API will use
anyway.

### D5: Kconfig

`TEDGE_CERT_RENEWAL` already depends on the direct transport and CA
authentication. It adds `TEDGE_CERT_RENEW_BEFORE_DAYS` (30),
`TEDGE_CERT_RENEW_ALARM_DAYS` (7) and `TEDGE_CERT_RENEW_CHECK_HOURS` (24),
and selects `TEDGE_HTTP` and `MBEDTLS_X509_CRT_PARSE_C`.

## Risks / Trade-offs

- [The device is offline for longer than the margin] → the margin is a
  Kconfig value; a device that is offline for a month has a bigger problem,
  and the alarm fires as soon as it reconnects.
- [The clock is wrong] → the client only checks once the clock is valid, and
  a clock far in the future would renew early rather than late, which is
  safe.
- [The CA refuses the renewal] → the old certificate keeps working until it
  expires; the alarm makes the failure visible with days to spare.
- [Renewal storms across a fleet] → the check is daily with a random offset
  within the day, so devices deployed together do not renew together.
- [A reconnect at a bad moment] → the reconnect happens after the renewal,
  not during an operation; features that own a session (a tunnel, a
  download) keep theirs, because they hold their own TLS contexts.

## Migration Plan

- Devices already enrolled need nothing: the check reads the certificate
  they have.
- A device whose certificate has already expired is not this feature's
  problem; it re-enrolls with a new one-time password.

## Open Questions

- Should a renewal also be triggerable from the cloud (an operation), for an
  operator who wants to rotate a certificate on demand?

## Results

### Hardware (ESP32-C6, Modbus + client, 2026-09-20)

Forced by building with a margin larger than the certificate's whole
lifetime (`TEDGE_CERT_RENEW_BEFORE_DAYS=3650`), so the client renews at once:

```
twin tedge_Certificate: {"expires":"2027-09-20T08:03:11Z","daysRemaining":364,"renewals":0}
certificate: 364 days left; renewing
certificate: renewed in 4111 ms (339 B), now valid for 365 days
reconnecting to pick up a new credential
state: connected
twin tedge_Certificate: {"expires":"2027-09-20T08:04:46Z","daysRemaining":364,"renewals":1}
```

| Check | Result |
|---|---|
| Renewal through `simplereenroll` with the Bearer token | a new certificate in **4.1 s**, same key |
| The new certificate is used | the client reconnects 8 s later and the expiry moves forward; Cumulocity shows the device CONNECTED throughout |
| Twin data | expiry, days remaining and a renewal count, on every connect and after each renewal |
| Footprint (C6, every feature) | 812 KB text, 116 KB of libc heap left: renewal adds ~3 KB over firmware update, because the CSR, EST and PKCS#7 code is shared with enrollment |

**One bug found and fixed:** the first twin message reported
`"daysRemaining":-1`, because the days were computed before the expiry had
been read from the certificate.

**Not exercised on hardware:** a renewal the server refuses, and the alarm.
Forcing either needs the tenant to reject a valid request, so the decision
(renew / retry / alarm / wait, including an expired certificate and a clock
that is not set) is covered by unit tests instead, and the failure path by
reading it: a failure leaves the stored certificate and the registered
credential untouched.
