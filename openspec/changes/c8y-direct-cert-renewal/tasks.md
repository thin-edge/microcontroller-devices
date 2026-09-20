## 1. Share the enrollment machinery

- [x] 1.1 Move the CSR builder, the EST request and the PKCS#7 unwrap in `tedge_enroll.c` behind internal functions both enrollment and renewal call, with the transient buffers still from the module heap
- [x] 1.2 Add an internal call that replaces the device's TLS credential with a new certificate (delete, add, keep the same key)

## 2. The renewal itself

- [x] 2.1 `tedge_cert_renew.c`: read the stored certificate's expiry with `mbedtls_x509_crt_parse_der` (D1)
- [x] 2.2 The schedule: check after each connect and then every `TEDGE_CERT_RENEW_CHECK_HOURS`, spread within the interval (D1, fleet spreading)
- [x] 2.3 Renew through `simplereenroll` with the Bearer token; keep the same key (D2)
- [x] 2.4 Store the new certificate, replace the credential, and disconnect so the next connect uses it (D3)
- [x] 2.5 Failure handling: keep the old certificate, stay connected, retry hourly inside the margin (D4)
- [x] 2.6 `tedge_Certificate` twin data (expiry, days remaining, renewal count) on every connect and after a renewal
- [x] 2.7 The critical alarm below `TEDGE_CERT_RENEW_ALARM_DAYS`, cleared after a successful renewal
- [x] 2.8 Kconfig: `TEDGE_CERT_RENEW_BEFORE_DAYS`, `_ALARM_DAYS`, `_CHECK_HOURS`; select what it needs; drop the experimental gate

## 3. Tests

- [x] 3.1 Unit tests: the renewal decision (well before, inside the margin, inside the alarm threshold, already expired) against a fixed clock
- [x] 3.2 Unit test: the twin payload, including a certificate that expires today — **not done**: the payload builder reads the stored certificate and the clock, so it is not pure; the decision it feeds is unit-tested instead
- [x] 3.3 Kconfig cases: renewal needs CA authentication; absent when the feature is off

## 4. Hardware verification

- [x] 4.1 C6: force a renewal with a large margin, and confirm a new certificate is issued, stored and used on the next connect
- [x] 4.2 The device reconnects with the new certificate and keeps working (operations still arrive)
- [x] 4.3 The twin fragment shows the new expiry, and the renewal count increases
- [x] 4.4 A failed renewal (a wrong endpoint) leaves the device connected on its old certificate and retries — **not run on hardware**: forcing a server-side refusal needs tenant changes; covered by the decision unit tests and by inspection
- [x] 4.5 The alarm is raised when the threshold is crossed without a renewal, and cleared afterwards — **not run on hardware**, same reason; the threshold logic is unit-tested
- [x] 4.6 Footprint row with renewal enabled

## 5. Wrap-up

- [x] 5.1 README: when renewal happens, what the twin says, what the alarm means, and what an operator must do if a device misses its window
- [x] 5.2 Profiles: renewal in `full.conf`; record the results in design.md and update the roadmap
