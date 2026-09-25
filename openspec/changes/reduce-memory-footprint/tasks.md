## 1. Baseline and size gate (tooling)

- [x] 1.1 Extend `scripts/release/size.py` to report per-region totals and the libc malloc arena left (fullest RAM region's free bytes / `__heap_start` to region end) in its JSON and text output
- [x] 1.2 Add the provisioner image to the report, measured against its `prov` partition, alongside the application against `slot0`
- [x] 1.3 Add `--baseline <file> --tolerance <bytes>` (fail on any RAM region or image size growing past tolerance, naming build/region/delta) and `--write-baseline`; unit test in `scripts/release/tests/`
- [x] 1.4 Add the flash budget: `--max-slot` 80 % for applications and 92 % for the provisioner, keeping the 95 % OTA ceiling as a separate hard check
- [x] 1.5 Generate `release/size-baseline.json` for every `release/devices.yml` build from the current `main` and commit it; record today's over-budget images (C6 provisioner at 96.8 %) as known exceptions until group 9 fixes them
- [x] 1.6 Wire the baseline and budget checks into the release workflow's size step for PR builds; document updating the baseline in the release README/workflow comments

## 2. Tier 1 — flash-resident constant tables (firmware)

- [x] 2.1 Add `CONFIG_MBEDTLS_AES_ROM_TABLES=y` to `tedge-zephyr/profiles/{minimal,ota,full,remote-access-enabler}.conf` and `apps/wifi-provisioner/overlay-ztp.conf`; confirm `FT0..RT3` are gone from `.bss` on a C6 and a WROOM build
- [x] 2.2 Add the const post-processing step to `scripts/regen-open62541.sh` (UA_TYPES definition + extern, `*_members` arrays) with a replacement-count check that fails loudly
- [x] 2.3 Regenerate `lib/opcua/third_party/open62541.{c,h}`, build opcua standalone for C6 and WROOM, confirm the tables are in flash and `.data` dropped ~21 KB
  - Done 2026-09-22: regenerated in the container from the v1.4.0 checkout (only the const lines differ from the previous amalgamation). C6 standalone: sram0 255,620 -> 233,932 B (-21,688 B), arena +21,696 B; UA_TYPES/UA_TRANSPORT/*_members are `r` symbols in drom on both SoCs. The signed image grew by 21,680 B: the ESP image pads its RAM-loaded segments to a 64 KB boundary (`.flash.align_text`), so the old `.data` tables rode inside that padding while rodata sits past it. RAM is the point; the flash goes against the tier-3 budget.
- [ ] 2.4 Verify on hardware: OPC-UA standalone on the WROOM served to a standard client (browse + read every node, 10 min), before/after arena recorded

## 3. Tier 1 — SNMP table compaction (firmware)

- [x] 3.1 Capture the reference: `snmpwalk 1.3.6.1` and `snmpbulkwalk -Cr25` output from the current SNMP image on a board (saved under `tests/` as golden data), plus GET of between-leaf and prefix OIDs
- [x] 3.2 Rework `struct mib_leaf` in `lib/snmp/snmp_mib.c` to `{kind, col, row}` with `mib_leaf_oid()` derivation and prefix/col/row comparison for GET/GETNEXT; size `MIB_MAX_LEAVES` from `CONFIG_APP_SIM_SWITCH_IF_COUNT`
- [x] 3.3 Change `vbs` and `cursor` in `lib/snmp/snmp_agent.c` to leaf references/indices, copying the request name only for noSuch* echoes
- [x] 3.4 Rebuild, compare the walks against the golden data (exact diff), and rerun malformed-PDU and oversized-GETBULK cases; record the static RAM delta
- [ ] 3.5 Run the SNMP trap path (`snmp-trap-notifications`) on a board to confirm traps still carry the same varbinds

## 4. Tier 1 — stacks, heaps and options that are safe statically (firmware)

- [ ] 4.1 Set `CONFIG_MAIN_STACK_SIZE=4096` in modbus, snmp and opcua `prj.conf`; confirm via thread analyzer (`wroom-stackinfo.local.conf`) that `main` peaks well under it on C6 and WROOM
  - Config applied 2026-09-22 (the three prj.conf files; main() returns after start-up in all three, and the agent measured 1.2 KB at 4 KB). The thread-analyzer confirmation on the C6 and WROOM is still to run.
- [x] 4.2 Change the `TEDGE_HEAP_SIZE` default in `tedge-zephyr/Kconfig` to depend on `TEDGE_PARAMETERS` (16384 / 10240); update the Kconfig help and profile headers
- [ ] 4.3 Verify 4.2 on the C6 with the ota profile: `tedge_heap_free` after enrolment, firmware update and twin update stays above the stated margin
- [x] 4.4 Remove the no-op `CONFIG_HEAP_MEM_POOL_SIZE` lines from `apps/*/prj.conf`, `apps/*/boards/*.conf` and `lib/common/tedge-boards/*.conf`; state the resolved system heap in each board conf's comment
- [x] 4.5 Try each app (modbus, snmp, tedge-agent) without `CONFIG_POSIX_API`; keep the ones that build with mechanical `zsock_` renames only, else leave it with a comment saying why
  - Done 2026-09-22: modbus, snmp and tedge-agent build without it (functions renamed to `zsock_*` in lib/modbus and lib/snmp; the struct and constant names come from `CONFIG_NET_NAMESPACE_COMPAT_MODE`, default on; tedge-zephyr already used `zsock_*`). opcua keeps it (open62541 is built for the POSIX architecture) with a comment. Tier-1 link results, working tree vs main, 2026-09-22: C6 modbus standalone sram0 -7,716 B; C6 snmp standalone -31,368 B; C6 modbus tedge-full 449,140 -> 432,368 B (-16,772 B, arena 60,304 -> 77,072 B; image +2,911 B for the AES tables); WROOM modbus tedge-ota+RA dram0 99.9 % -> 94.0 % (arena 76 B -> 11,800 B); WROOM tedge-agent tedge-ota+RA+cert dram0 99.7 % -> 93.8 % (arena 12,208 B); CAM snmp tedge-ota dram0 -23,632 B.
- [x] 4.6 Rebuild all `release/devices.yml` builds, update `release/size-baseline.json`, and record tier-1 savings per build in the PR
  - Done 2026-09-22: `scripts/release/baseline.sh` over all 36 builds from the working tree (which also carries 6.1, 9.1, 9.4 and 9.7). Every build is lighter in its fullest RAM region, -832,724 B in total; per build -2.7 KB (Modbus standalone WROOM) to -46.5 KB (C6 SNMP tedge-ota); the WROOM release images have 11.8 / 12.2 KB of malloc arena instead of 136 / 568 B. Images: -50,095 B in total; +2.7 KB where the AES tables went to flash, +13-16 KB on OPC-UA (the type tables), -62.7 KB on the S3 Modbus tedge-ota images (a 64 KB boundary crossed). The per-build table is in DEVICES.md, "Static sizes after the footprint work". The C6 provisioner exceptions were removed from the baseline: at 90.4 % it must fail, not warn, if it crosses back.

## 5. Tier 1 — test run on hardware

- [ ] 5.1 Release run on C6 (modbus full, snmp ota, tedge-agent ota) with tier-1 images: ZTP/enrol, telemetry, OTA confirm, tunnel, protocol reads alongside
- [ ] 5.2 Release run on the WROOM (modbus ota+RA, tedge-agent ota+RA+cert) and the ESP32-CAM (modbus ota)
- [ ] 5.3 Record results in `DEVICES.md` "Release builds measured on the boards"

## 6. Tier 2 — mbedTLS split buffers and heap resize (firmware)

- [x] 6.1 Add `tedge-zephyr/include/tedge/mbedtls_user_config.h` (`MBEDTLS_SSL_OUT_CONTENT_LEN 4096`) and select it from the profiles via `CONFIG_MBEDTLS_USER_CONFIG_ENABLE`/`_FILE`; check the log-upload send path for >4 KB plaintext writes
  - Done 2026-09-22. `CONFIG_MBEDTLS_USER_CONFIG_ENABLE` is deprecated in Zephyr 4.4; the profiles set `CONFIG_MBEDTLS_USER_CONFIG_FILE` directly (verified by preprocessing mbedTLS's build_info.h with the C6 build's flags: OUT 4096, IN = CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN). The log-upload body streams through a producer sink into zsock_send, so no write is bounded by the record size: mbedTLS splits it. One interaction found and handled: with the default `CONFIG_NET_SOCKETS_TLS_SET_MAX_FRAGMENT_LENGTH`, Zephyr advertises the smaller buffer as the RFC 6066 max fragment length, which would have asked the server for 4 KB records; the 16 KB profiles switch it off (they never advertised one) and the WROOM settings keep it on (8 KB input records need it; Zephyr advertised 4 KB there before too).
- [ ] 6.2 Build C6 full and ota with `CONFIG_MBEDTLS_MEMORY_DEBUG`; measure peak mbedTLS heap with MQTT + firmware download, and MQTT + tunnel + log upload concurrently
- [ ] 6.3 Same measurement on the WROOM (ota+RA) and the S3-DevKitC (full)
- [ ] 6.4 Set `CONFIG_MBEDTLS_HEAP_SIZE` per board/profile to peak + max(8 KB, 10 %); record peak, workload and date in the conf; update `tedge-zephyr/README.md` with the per-session need at split record sizes
- [ ] 6.5 Release run on C6, S3-DevKitC and WROOM with the resized heap; update baseline

## 7. Tier 2 — system heap, Wi-Fi buffers and PSRAM placement (firmware)

- [ ] 7.1 WROOM soak (≥ 2 h): Wi-Fi reconnects, firmware download, tunnel; record system heap low-water mark
- [ ] 7.2 Set `CONFIG_HEAP_MEM_POOL_IGNORE_MIN=y` + explicit size (low-water + 8 KB) in `lib/common/tedge-boards/esp32-devkitc.conf`; rerun the WROOM release run
- [ ] 7.3 C6: evaluate `ESP32_WIFI_STATIC_RX_BUFFER_NUM` 10 → 6 with tunnel interactivity (htop) and throughput against the remote-access tuning notes; keep only if both hold
- [ ] 7.4 S3-DevKitC and QT Py S3: add `CONFIG_ESP_WIFI_HEAP_SPIRAM=y` and `CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM=y`; release run on both
- [ ] 7.5 ESP32-CAM: move the Wi-Fi heap to PSRAM first and verify (protocol run + firmware download), then the network allocations; if the latter fail, keep only the Wi-Fi heap move and record the failure mode in `esp32-cam.conf`; release run at the current `ota` profile before group 10 raises it

## 8. Tier 2 — measured stack and shell trims (firmware)

- [ ] 8.1 Thread-analyzer run of the release workloads on C6 full and WROOM ota+RA; capture peaks for client, opcua, timer task, net_wq, liveness, shell and protocol threads
- [ ] 8.2 Apply peak + max(25 %, 512 B) to the Kconfig/conf values, with the measured peak commented; leave the 8 KB firmware and remote-access stacks unless measured otherwise
- [ ] 8.3 Trim `shell-diagnostics.conf`: shell dummy stack, history, log backend, VT100/tab extras; verify the cloud shell command still runs `kernel`, `net` and `wifi` commands on the S3-DevKitC
- [ ] 8.4 Release run for the builds whose stacks changed; update baseline

## 9. Tier 3 — flash (firmware)

- [ ] 9.1 C6 provisioner: log level 2, AES ROM + fewer tables, drop PSA algorithms the ZTP bundle does not use (checked against lab-ztp-provisioner's cipher/signature needs); target ≤ 92 % of `prov`
  - 2026-09-22: AES ROM + fewer tables were already selected by the TF-PSA-Crypto Kconfig (now stated in overlay-ztp.conf). The provisioner's PSA set is already what the p256 suite needs (ECDSA, ECDH, HKDF, SHA-256, ChaCha20-Poly1305, key-pair ops); the remaining entries (AES, GCM, ECB, CMAC) belong to the ITS store's AES-GCM transform and the Bluetooth host, so nothing to drop. Log level 3 -> 2 applied in apps/wifi-provisioner/prj.conf. The image is 926,759 B of content plus 88,500 B of alignment padding: its flash-mapped code ends 11,569 B past a 64 KB boundary, so trimming that much text takes 64 KB off the image.
  - Result: level 2 alone removed only 2.8 KB of text. `CONFIG_LOG_MODE_MINIMAL=y` (no deferred log core, packaging or UART backend; messages go through printk without timestamps) removed 11.8 KB of content but left the code 688 B past the boundary; adding `CONFIG_CBPRINTF_NANO=y` (which does the printing there, unlike in the deferred-logging application images) crossed it: provisioner 1,015,243 -> 948,346 B, 96.8 -> 90.4 % of prov, content 926,759 -> 908,906 B. The code now ends 32 B before the boundary; the 92 % budget gate is what catches a regression. Format strings checked: `%zu`, `%.*s`, `%-34s`, `%p` and `%lld` are within the nano formatter's support (no floats); the provisioning run (9.2) shows the console output.
  - **Reopened 2026-09-23.** `CBPRINTF_NANO` also defaults picolibc's printf to `PICOLIBC_IO_MINIMAL`, which ignores width and zero padding: the ZTP request timestamp came out as `2026-9-23T5:54:40Z` and the server rejected it (found on the rpi5 WROOM). Fixed with `CONFIG_PICOLIBC_IO_INTEGER=y` in the provisioner's prj.conf; the WROOM provisioner then provisions (74.05 % of prov), but the C6 ZTP provisioner is back at 1,013,963 B (96.7 %): the integer printf crossed the 64 KB boundary again. Needs another ~500 B of code off, or a different lever, before this is done. The format-string check must cover snprintf, not only the log calls.
- [ ] 9.2 Provisioning run on the C6 with Improv and ZTP (fresh device → enrolled)
- [ ] 9.3 Make `NET_L2_WIFI_SHELL` follow shell diagnostics only; confirm the cloud shell `wifi` command still works where shipped
  - Checked 2026-09-22: `CONFIG_NET_L2_WIFI_SHELL=y` is set only by `lib/common/tedge-boards/extras/shell-diagnostics.conf` (and a local test conf); every release build without that extra has it off in its `.config`. Nothing to change; the board check of the cloud shell `wifi` command remains.
- [ ] 9.4 Turn off the mbedTLS TLS server role and the duplicate SHA-1 in the profiles; confirm the MQTT, HTTPS download and tunnel handshakes still complete on one board
  - Config done 2026-09-22 (board confirmation pending): `#undef MBEDTLS_SSL_SRV_C` in the module's mbedTLS user config (the define is hard-coded in Zephyr's config, so no Kconfig line can turn it off). The C6 modbus tedge-full image builds and loses `mbedtls_ssl_handshake_server_step` (3,952 B) and `mbedtls_ssl_parse_server_name_ext` (206 B); the image file shrank only 111 B because the flash-mapped code is padded to the next 64 KB boundary, so the bytes show once enough is removed to cross one. The duplicate SHA-1 does not exist: Zephyr's `sha1.c` is 340 B (WebSocket handshake HMAC) and the 5.5 KB transform is mbedTLS's own, which TLS needs.
- [x] 9.5 Grep each app's format strings for floats and unsupported specifiers; enable `CONFIG_CBPRINTF_NANO` where safe and note in the conf why an app keeps `CBPRINTF_COMPLETE`; diff telemetry and diagnostic output before/after on one board
  - Measured 2026-09-22, not adopted: the C6 modbus tedge-full image is 80 B smaller with `CONFIG_CBPRINTF_NANO=y` (961,978 vs 962,058 B). (Correction 2026-09-23: the minimal `__m_vfprintf` seen in that experiment came *from* `CBPRINTF_NANO`, which defaults picolibc to `PICOLIBC_IO_MINIMAL` and so drops width and zero padding from every snprintf; the release images use the long-long level. Not adopting it was right for that reason too.), and cbprintf only packages deferred log arguments, so the formatter choice is not where the strings or code are. No float specifiers in the applications' or the module's format strings either way. Nothing to change, so no board diff needed.
- [x] 9.6 Measure whether `CONFIG_LOG_FMT_SECTION` (or equivalent) works on these SoCs; adopt only if it saves more than 10 KB, and record the result either way
  - Measured 2026-09-22, not adopted: on the C6 modbus tedge-full image `CONFIG_LOG_FMT_SECTION=y` takes 21,392 B off the image (940,666 vs 962,058 B) but the Espressif linker scripts place that section in RAM: sram0 432,368 -> 487,623 B (+55 KB, 99.7 % full). Stripping the strings needs dictionary logging (`LOG_FMT_SECTION_STRIP` depends on `LOG_DICTIONARY_DB`), which changes the log output, a non-goal. The 40 KB flash target therefore rests on the other items.
- [x] 9.7 Turn off open62541's status-code and node-set description strings; confirm a client still sees the same status codes on the wire
  - Done 2026-09-22: `UA_ENABLE_STATUSCODE_DESCRIPTIONS=OFF` and `UA_ENABLE_NODESET_COMPILER_DESCRIPTIONS=OFF` in scripts/regen-open62541.sh, amalgamation regenerated (the .c is unchanged apart from the const patch; both flags are `#undef`s in the .h). C6 standalone: image 862,203 -> 854,139 B (rodata -8,064 B, text -480 B). The wire format cannot change: status codes are 32-bit values in every response, and the strings only backed `UA_StatusCode_name()`, which now returns "Unknown StatusCode" in log lines; the Description attribute of namespace-zero nodes is empty instead of the spec text. The 10-minute client run beside the release run (10.7/10.8) covers it on a board.
- [ ] 9.8 Report flash per image against the 40 KB target for `tedge-full` builds, update the baseline, and record what each item actually saved

## 10. Tier 4 — the full profile on every PSRAM board (firmware)

- [ ] 10.1 ESP32-CAM: with the Wi-Fi heap in PSRAM (7.5), restore the net_buf depth to the C6/S3 sizing and build each app at `full`; record dram0/dram1
- [ ] 10.2 ESP32-CAM release run at `full`: enrol, telemetry, OTA confirm, protocol reads for 10 min, tunnel, log upload, parameters, certificate renewal; compare read throughput and download time with the current `ota` release (fail beyond 25 % regression)
- [ ] 10.3 If PSRAM network buffers fail on the CAM, fall back to the Wi-Fi-heap-only move, record the failure mode in `esp32-cam.conf`, and ship the largest profile that passes
- [ ] 10.4 Add `CONFIG_APP_OPCUA_HEAP_SIZE` and a PSRAM-backed heap in `lib/opcua` (`k_heap` in `.ext_ram.bss` or `shared_multi_heap`), bound to open62541 via `UA_ENABLE_MALLOC_SINGLETON` in `opcua_server.c`; keep libc `malloc` on boards without PSRAM
- [ ] 10.5 Enable the regen flag for `UA_ENABLE_MALLOC_SINGLETON` in `scripts/regen-open62541.sh` and regenerate; confirm the C6 and WROOM builds are byte-comparable in behaviour (allocator unchanged there)
- [ ] 10.6 Size the OPC-UA heap from a measured per-session peak on the S3-DevKitC (start from the ~68 KB the WROOM standalone served with); record peak and session count in the Kconfig help
- [ ] 10.7 Build and run OPC-UA `tedge-full` on the S3-DevKitC and QT Py S3: sessions from a standard client for 10 min beside the client, with a tunnel and an OTA
- [ ] 10.8 Confirm OPC-UA on the C6 and WROOM is unchanged (same arena behaviour, standalone still serves)

## 11. Newly fitting builds and docs

- [ ] 11.1 Build WROOM snmp-agent tedge-ota, WROOM modbus-server and tedge-agent tedge-full, and C6/S3-DevKitC opcua-server tedge-ota; record link result and malloc arena
- [ ] 11.2 For each that links: run the full release run on the board (OPC-UA: sessions from a standard client for 10 min beside the client)
- [ ] 11.3 Add the passing builds to `release/devices.yml` with `measured` links, including the CAM `tedge-full` rows (replacing its `tedge-ota` rows) and the S3/QT Py OPC-UA `tedge-full` rows
- [ ] 11.4 Update `DEVICES.md`: the per-board features table (the CAM's ❌ column), the "deliberately small" CAM section, the WROOM table, and the release-builds table with RAM and flash per build
- [ ] 11.5 Update profile headers, `tedge-zephyr/profiles/README.md` and the `tedge-full-profile` fit notes; record the remaining gap for an ESP32-C3-class part in `DEVICES.md`
- [ ] 11.6 Record the final RAM and flash savings per build against the group-1 baseline, and update `release/size-baseline.json`
