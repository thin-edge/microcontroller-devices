## 1. The downloader

- [ ] 1.1 `tedge-zephyr/src/tedge_http_download.c`: URL parsing, TLS or plain HTTP, a sink callback, and writing from the parser's `on_body` (P5)
- [ ] 1.2 Redirects: up to `TEDGE_FIRMWARE_MAX_REDIRECTS` hops, relative, absolute and cross-scheme, with a URL buffer of at least 1 KB
- [ ] 1.3 The token rule: sent only to hosts in the tenant's parent domain, dropped when a redirect leaves it (D4)
- [ ] 1.4 A longer TLS connect timeout for hosts outside the tenant (P-384 chains take ~13 s)

## 2. The firmware flow

- [ ] 2.1 `tedge_firmware.c`: handle `515`, report `501` with the expected downtime, run the download on its own thread (D3)
- [ ] 2.2 Write into the secondary slot with `flash_img`, progressive erase, and check the slot's state before starting
- [ ] 2.3 Persist the pending marker in settings, request a test boot and reset through the platform reset
- [ ] 2.4 After the swap: confirm once connected and the application's hook passes; `115,<name>,<version>`, `503`, clear the marker
- [ ] 2.5 Revert path: the old image finds the marker while running confirmed and reports `502` naming the version
- [ ] 2.6 Report `115` on every connect, whatever the image (D6)
- [ ] 2.7 Kconfig: `TEDGE_FIRMWARE_STACK_SIZE`, `TEDGE_FIRMWARE_CONFIRM_AFTER_CONNECT`, `TEDGE_FIRMWARE_MAX_REDIRECTS`; select the DFU options; drop the experimental gate

## 3. Tests

- [ ] 3.1 Unit tests: URL parsing and the redirect rules, the token-domain rule (tenant host, tenant-ID host, a foreign host), and the `515` fields
- [ ] 3.2 Kconfig cases: the feature needs MCUboot; with it off, no HTTP client or DFU in the image
- [ ] 3.3 The secrets check still passes (the token must not reach a log)

## 4. Hardware verification

- [ ] 4.1 C6 Modbus + client: install a new version from Cumulocity; record download rate, swap time and the total time to SUCCESSFUL
- [ ] 4.2 The inventory shows the running version after the update, and after a reboot
- [ ] 4.3 Revert: install an image that cannot connect (wrong tenant) and confirm the previous image comes back and reports the failure
- [ ] 4.4 Revert: an image whose application hook refuses it
- [ ] 4.5 A download from a non-tenant host (a GitHub release asset) works and carries no token
- [ ] 4.6 TLS heap with MQTT + download, and the footprint row in `measure_tedge.sh`
- [ ] 4.7 S3 with the TLS heap in PSRAM: one update end to end

## 5. Wrap-up

- [ ] 5.1 README: the feature's requirements (slot layout, MCUboot, heap for a second session), what belongs in the confirm hook, and the downtime during a swap
- [ ] 5.2 Profiles: firmware update in `full.conf`; record the results in design.md
- [ ] 5.3 Delete `apps/c8y-spike` and its references, now that every feature it demonstrated lives in the module
- [ ] 5.4 Report the Zephyr HTTP-client chunked-body bug upstream (P5), with the reproduction from the spike
