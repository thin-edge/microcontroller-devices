## 1. Something to test against

- [ ] 1.1 A thin-edge.io gateway on the Raspberry Pi, connected to the tenant, reachable from the device's network
- [ ] 1.2 Register one child device by hand and move one command through it, so the protocol is understood before device code is written
- [ ] 1.3 Record the command names, payloads and status transitions the gateway version in use actually serves (design.md open question)

## 2. The transport

- [ ] 2.1 `tedge_gateway.c`: connect to the local broker, register as a child device, and serve the session behind `struct tedge_transport` (D1, D2)
- [ ] 2.2 Whatever the interface is missing for commands, added as narrowly as the evidence demands, with the direct transport unchanged
- [ ] 2.3 Discovery: mDNS `_thin-edge_mqtt._tcp`, with `TEDGE_GATEWAY_HOST` as the fallback and a log line saying which was used (D5)
- [ ] 2.4 Kconfig: the gateway transport loses its experimental gate; remote access and certificate renewal depend on the direct transport (D6)

## 3. Telemetry and state

- [ ] 3.1 Measurements, events and alarms on the child-device topics (D2)
- [ ] 3.2 Twin data and the client's own health, republished on every connect as they are today
- [ ] 3.3 The telemetry buffer keeps working, with whatever the gateway's reachability changes about its sizing

## 4. Commands

- [ ] 4.1 Advertise the commands this image can carry out (D3)
- [ ] 4.2 Restart, end to end, as the first command
- [ ] 4.3 Firmware update, with the file fetched from the gateway (D3, D4)
- [ ] 4.4 Log upload, through the gateway's file-transfer service
- [ ] 4.5 Parameters, if `c8y-direct-parameters` has landed by then
- [ ] 4.6 A command for a feature that is not built in is failed with a reason

## 5. Files

- [ ] 5.1 `tedge_gateway_http.c`: plain HTTP to the gateway, no TLS, no token (D4)
- [ ] 5.2 The download and upload paths shared with the direct transport where they can be

## 6. Tests

- [ ] 6.1 Unit tests: the command topics and their status transitions
- [ ] 6.2 Kconfig cases: a gateway build has no TLS, no certificate and no remote access; a direct build is unchanged

## 7. Hardware verification

- [ ] 7.1 C6: registers, reports telemetry and is visible in the cloud through the gateway
- [ ] 7.2 Restart, firmware update and log upload through the gateway
- [ ] 7.3 The direct transport still passes its own verification (no regression)
- [ ] 7.4 An ESP32-WROOM-32 with a protocol server **and** device management, which is the point of the change
- [ ] 7.5 Footprint rows for a gateway build, next to the direct one

## 8. Wrap-up

- [ ] 8.1 README: choosing a transport, what the gateway owns, what is unavailable and why
- [ ] 8.2 A gateway profile; design.md results; SCOPE roadmap P8; archive
