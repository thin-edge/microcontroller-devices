## Why

Internal RAM, not flash, decides which boards can run which firmware. Today
SNMP and OPC-UA do not fit beside the thin-edge.io client on the ESP32-WROOM-32
(24 KB and 16 KB over at the ota level). OPC-UA links beside the client on the
ESP32-C6 and S3-DevKitC but runs out of heap at runtime. The Modbus and agent
`full` builds miss the WROOM by 3.7–8.4 KB. The symbol-level breakdown of the
existing release builds shows that a large share of that RAM goes to generic
overhead, not to features:

- writable tables that are never written;
- per-record buffers sized for the worst case;
- stacks sized by guess;
- a system heap whose size the configuration does not actually control.

Recovering this overhead widens the set of boards each image ships on without
removing a feature. It is also the precondition for smaller parts such as the
ESP32-C3.

Flash has its own tight spot, and it is not the application slot (a C6
`tedge-full` image is 959,147 B of a 1,280 KB slot). It is the **1 MB `prov`
partition** on 4 MB parts, where the C6 Wi-Fi provisioner sits at **96.8 %**.
A provisioner that outgrows its partition takes BLE/ZTP provisioning off a
board entirely, so flash needs a budget and a gate of its own, and the app
images need headroom to grow into on 4 MB parts.

Finally, a board having PSRAM does not yet mean it runs everything. The
ESP32-CAM has 4 MB of PSRAM but still ships `ota` only, because the net_buf
pools sit in its 96 KB dram1; and OPC-UA `full` fails on the S3-DevKitC and
QT Py because open62541 allocates from the internal libc arena. On a board
with megabytes of external RAM, "the full profile does not fit" is a
placement problem, not a capacity one.

Phase: **3** for the client module and profile changes (`tedge-zephyr/`,
`tedge-zephyr/profiles/`, `lib/common/tedge-boards/`). **2** for the protocol
libraries and apps (`lib/snmp`, `lib/opcua`, `apps/*`), whose changes apply to
the standalone images as well.

## What Changes

**Static RAM, no behaviour change.** Each item is verified by a link plus the
existing protocol and client tests.

- Move mbedTLS's AES T-tables to flash (`CONFIG_MBEDTLS_AES_ROM_TABLES`). This
  is 8 KB on every TLS image, including the ZTP provisioner.
- Make open62541's generated type tables `const`, patched in
  `scripts/regen-open62541.sh`. That is about 21 KB of `.data` on every OPC-UA
  image, standalone included, and it goes straight to open62541's malloc arena.
- Compact the SNMP MIB and per-request tables. Each leaf stores `{kind, column,
  row}` and its OID is built on demand, instead of storing a 32-arc OID.
  Varbinds and walk cursors point at leaves instead of copying OIDs. About
  23 KB saved; walk order and GETBULK behaviour are unchanged.
- Right-size stacks from measured high-water marks, starting with `main`
  (8 KB → 4 KB in modbus, snmp and opcua, where `main()` returns after start-up)
  and the client thread. Each trim keeps a documented margin over the
  measured peak.
- Size the tedge private heap by feature. The 16 KB default is needed only
  with parameters.
- Link `POSIX_API` only into the images whose code needs it.
- Remove the `CONFIG_HEAP_MEM_POOL_SIZE` settings that have no effect. The
  Wi-Fi driver's 51,200 B `ADD_SIZE` minimum always wins, so 16384/40960 do
  nothing today. Each board states the system heap it actually gets.

**Runtime-sized RAM, verified on hardware.**

- Split mbedTLS's input and output record buffers: 4 KB output, input stays
  at the negotiated maximum. Then lower `CONFIG_MBEDTLS_HEAP_SIZE` to the
  measured peak of the board's concurrent sessions plus margin. On the C6
  (98,304 B) this is the largest single saving.
- Give the system heap an explicit, measured size
  (`CONFIG_HEAP_MEM_POOL_IGNORE_MIN`) where its measured low-water mark
  leaves slack (WROOM: about 21 KB free at peak).
- On PSRAM boards (S3-DevKitC, QT Py S3, ESP32-CAM), place the Wi-Fi heap and
  the network allocations in PSRAM (`CONFIG_ESP_WIFI_HEAP_SPIRAM`,
  `CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM`).
- Trim shell-diagnostics overhead (idle shell stack, history, log backend)
  where the shell command is built.

**Full profile on every PSRAM board.**

- Place the network buffers and the Wi-Fi heap in PSRAM, which is what the
  ESP32-CAM's dram1 needs, and build the CAM at `full` instead of `ota`. That
  restores remote access, log upload, parameters, certificate renewal, shell
  and crash dumps on that board.
- Give open62541 a PSRAM allocator (`UA_ENABLE_MALLOC_SINGLETON` bound to a
  `shared_multi_heap`/`k_heap` in external RAM), so OPC-UA stops competing
  with the client for the internal libc arena and OPC-UA `full` runs on the
  S3-DevKitC and QT Py S3.
- Goal: every application runs the `full` profile on every PSRAM board
  (S3-DevKitC, QT Py S3, ESP32-CAM), each confirmed by a board run.

**Flash.**

- Give each image a flash budget and gate it like RAM: the signed application
  at most 80 % of `slot0`, the provisioner at most 92 % of `prov`, and no
  image growing past its committed baseline.
- Bring the ESP32-C6 Wi-Fi provisioner back from 96.8 % of its 1 MB `prov`
  partition: log level, PSA algorithms the ZTP overlay does not use, AES ROM
  tables.
- Shrink the application images themselves, targeting at least 40 KB off the
  largest `tedge-full` builds: fewer log strings (merged string literals are
  150–195 KB per image), `CBPRINTF_NANO` where no float is printed, the
  mbedTLS TLS **server** role the clients never use (about 4 KB), the
  duplicate SHA-1 implementation (about 5 KB), and open62541's status-code
  and node-set description strings.
- Link the Wi-Fi shell (41 KB) only with shell diagnostics.

**Measurement and gate.**

- Extend `scripts/release/size.py` to report the libc malloc arena that is
  left, and to compare RAM *and flash* against a committed per-build
  baseline. A build that grows past the baseline without the baseline being
  updated fails the PR. The provisioner image is gated against `prov`, as the
  application already is against `slot0`.

**Release matrix.** Rows are added to `release/devices.yml` only for builds
that newly fit *and* pass the board run the manifest already requires.
Candidates:

- WROOM `snmp-agent` tedge-ota;
- WROOM `modbus-server` / `tedge-agent` tedge-full;
- C6 / S3-DevKitC `opcua-server` tedge-ota;
- ESP32-CAM `snmp-agent` / `modbus-server` / `tedge-agent` tedge-full
  (upgraded from tedge-ota);
- S3-DevKitC / QT Py S3 `opcua-server` tedge-full.

No **BREAKING** changes: every image keeps its features, protocol behaviour,
public `tedge_*` API and flash layout.

## Capabilities

### New Capabilities

- `memory-footprint`: the rules every image follows to keep RAM and flash for
  its features: flash-resident constant tables, measured stack and heap sizing
  with margins, PSRAM carrying what does not need internal RAM (so a PSRAM
  board runs the full profile), a flash budget per partition, and per-build
  RAM and flash baselines that CI enforces.

### Modified Capabilities

- `tedge-client-module`: "Bounded resource ownership". The module documents
  the mbedTLS heap per session with separate input and output record sizes,
  and sizes its private heap by the features built.
- `snmp-agent`: "Bounded, allocation-free request handling". The fixed work
  buffers are bounded by the configured interface count and hold leaf
  references, not OIDs; still with no per-request allocation.

## Non-goals

- Removing or disabling any feature, protocol object or OPC-UA service a
  build ships today.
- Changing the flash partition layout or the MCUboot mode
  (`SWAP_USING_MOVE` would free the scratch partition, but MCUboot and the
  partition table cannot be updated over the air). This can be a later change
  for new devices only. The flash work here therefore has to fit the existing
  partitions, not resize them.
- Dropping a log level, a diagnostic or a shell command to save flash. Flash
  savings come from how things are encoded and from code that is never
  reached, not from removing diagnostics — except in the provisioner, whose
  logs only serve provisioning and whose partition is nearly full.
- Making a 2 MB flash part viable. Two application slots plus a separate
  provisioner image do not fit, whatever this change saves.
- Serializing device-management operations onto one shared worker thread to
  share their stacks. It saves 8–12 KB on `full`, but changes concurrency.
- Smaller TLS records than 8 KB, or relying on max-fragment-length
  negotiation.
- Adding the ESP32-C3 or other new boards. This change measures the remaining
  gap for them; adding them is separate work.
- LTO. It is not available on these SoCs (`GEN_ISR_TABLES` without local ISR
  declarations).

## Target boards and resource constraints

| Board | Binding region | Today (tightest release build) |
|---|---|---|
| ESP32-WROOM-32 | dram0 192 KB, no PSRAM | 99.6–99.9 % dram0; 76 B malloc arena with remote access |
| ESP32-CAM | dram1 96 KB (net_buf / `.noinit`) | 92.5 % dram1 |
| ESP32-C6-DevKitC | sram0 478 KB, no PSRAM | 93–99 % with tedge; OPC-UA arena about 33 KB |
| ESP32-S3-DevKitC (N16R8) / QT Py S3 (N4R2) | dram0; PSRAM available | 82–99 % |
| C6 Wi-Fi provisioner | `prov` partition 1 MB flash | 96.8 % |

Every change here must *lower* or hold each build's static RAM and flash. The
runtime changes must keep a measured free-heap margin on the board (see
design).

## Kconfig options added and expected cost

This change adds no new user-facing `CONFIG_TEDGE_*` option. It changes these
defaults and settings:

| Option / setting | Where | Expected change |
|---|---|---|
| `CONFIG_MBEDTLS_AES_ROM_TABLES=y` | profiles, provisioner ZTP overlay | −8,192 B RAM, about +2 KB flash, all TLS images |
| mbedTLS user config (`MBEDTLS_SSL_OUT_CONTENT_LEN` 4096) via `CONFIG_MBEDTLS_USER_CONFIG_FILE` | profiles | runtime −4 KB (8 KB records) / −12 KB (16 KB records) per session; static only through the heap resize below |
| `CONFIG_MBEDTLS_HEAP_SIZE` | profiles, `tedge-boards/*.conf` | C6 98,304 → measured peak + margin (expected about 64 KB) |
| `CONFIG_TEDGE_HEAP_SIZE` default | `tedge-zephyr/Kconfig` | 16 KB with parameters, about 10 KB without |
| `CONFIG_MAIN_STACK_SIZE` | modbus, snmp, opcua `prj.conf` | 8192 → 4096 (−4 KB each) |
| `CONFIG_HEAP_MEM_POOL_IGNORE_MIN` + size | WROOM board confs | −about 12 KB, after a hardware low-water run |
| `CONFIG_ESP_WIFI_HEAP_SPIRAM`, `CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM` | PSRAM board confs | −about 25 KB internal (Wi-Fi heap), −about 45 KB (net_buf pools) |
| `UA_ENABLE_MALLOC_SINGLETON` + a PSRAM heap for open62541 | `lib/opcua` (regen flag, Kconfig `APP_OPCUA_HEAP_*`) | OPC-UA stops drawing on the internal libc arena on PSRAM boards; internal RAM unchanged, arena freed |
| `CONFIG_CBPRINTF_NANO` where no float is printed | `apps/*/prj.conf`, provisioner | flash only, expected −10 to −20 KB per image |
| `CONFIG_LOG_DEFAULT_LEVEL` 3 → 2, trimmed `PSA_WANT_*` | provisioner confs | flash only; target ≤ 92 % of `prov` |
| mbedTLS TLS server role off, single SHA-1, open62541 description strings off | profiles, `lib/opcua` | flash only, about −15 KB combined |

These costs are estimated from symbol sizes in the existing release builds.
The tasks replace them with measured before/after numbers per target board, in
`DEVICES.md` and the profile headers.

## Public API

No change to the `tedge_*` API. The mbedTLS user-config header lives in
`tedge-zephyr/` because the module documents its mbedTLS needs. Host-app glue
stays in `apps/*/src/tedge_glue.c`.

## Impact

- **Code:**
  - `lib/snmp/snmp_mib.c`, `snmp_agent.c`, `snmp_ber.h`
  - `scripts/regen-open62541.sh` and the regenerated
    `lib/opcua/third_party/open62541.{c,h}`
  - `tedge-zephyr/Kconfig`, `tedge-zephyr/profiles/*.conf`, a new mbedTLS
    user-config header
  - `lib/common/tedge-boards/*.conf`
  - `apps/*/prj.conf` and `apps/*/boards/*.conf`
  - `apps/wifi-provisioner` confs
  - `lib/opcua/CMakeLists.txt`, `lib/opcua/Kconfig` and a small PSRAM
    allocator in `lib/opcua/opcua_server.c` (host-app side, not
    `tedge-zephyr/`)
- **Tooling:** `scripts/release/size.py`, committed RAM and flash baselines,
  and the release workflow's size step.
- **Docs:** `DEVICES.md` fit tables, the profile headers, and
  `tedge-zephyr/README.md` (mbedTLS heap per session).
- **Release:** possible new rows in `release/devices.yml`, each gated by a
  board run.
- **Risk:** runtime heap exhaustion that a link does not show, which has
  happened twice before. Every runtime-sized change carries a hardware
  verification task. PSRAM placement adds its own: the classic ESP32's PSRAM
  has DMA restrictions, so network buffers there are verified on the CAM
  before they ship.
