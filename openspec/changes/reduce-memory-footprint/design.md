## Context

All numbers here come from symbol and map attribution of the existing release
builds (`build_cand_*`, `build_fit_wroom_*`, `build_meas_wroom_*`), made with
`llvm-nm` and linker-map parsing in `zephyr-dev`. None of it comes from new
builds.

**Where static RAM goes (bytes):**

| Category | C6 snmp ota | C6 modbus full | C6 opcua ota | WROOM modbus ota+RA |
|---|---|---|---|---|
| mbedTLS heap (`MBEDTLS_HEAP_SIZE`) | 98,304 | 98,304 | 98,304 | 57,344 |
| System heap (`kheap__system_heap`) | 56,476 | 56,476 | 56,476 | 56,476 |
| Thread stacks | 55,140 | 74,596 | 64,356 | 62,824 |
| net_buf / pkt / ctx / TCP slabs | 61,734 | 61,830 | 61,734 | 20,606 |
| tedge buffers + 16 KB private heap | 34,154 | 47,493 | 34,154 | 38,726 |
| Wi-Fi driver statics | 20,815 | 21,139 | 20,815 | 16,830 |
| App / protocol library | 34,780 | 2,558 | 23,370 | 2,382 |
| Crypto statics (AES T-tables 8,192) | 9,452 | 9,452 | 9,452 | 9,452 |

**The libc malloc arena is whatever is left.** `COMMON_LIBC_MALLOC_ARENA_SIZE`
is -1, so it is not configured anywhere. open62541 allocates from it, which is
why OPC-UA "links but fails" beside the client. The C6 opcua-ota arena is
about 33 KB, and sessions fail. The WROOM standalone OPC-UA serves with about
68 KB. The WROOM modbus+RA build has 76 B.

**Three findings shape the design:**

1. **`CONFIG_HEAP_MEM_POOL_SIZE` is a no-op on every ESP board.**
   - `kernel/CMakeLists.txt` takes max(`HEAP_MEM_POOL_SIZE`, the sum of the
     `ADD_SIZE_*` values). `ADD_SIZE_ESP_WIFI` is 51,200.
   - So 16384 (WROOM) and 40960 (C6/S3 tedge-boards) both give 56,476 B.
   - The measured WROOM low-water mark is 21.6 KB free.
2. **Large RAM tables are never written.**
   - open62541's `UA_TYPES` (13,380 B) and 182 `*_members` arrays (7,764 B)
     are non-const `.data`.
   - mbedTLS's AES tables are 8 KB of `.bss`.
   - SNMP's `leaves[99]` stores a full `uint32_t oid[32]` per leaf
     (13,464 B), although every OID is a constant prefix plus a column and
     row.
3. **The mbedTLS heap is sized for symmetric 16 KB records.** Zephyr sets
   `SSL_IN_CONTENT_LEN = SSL_OUT_CONTENT_LEN = MAX_CONTENT_LEN`. The client
   sends small MQTT publishes, HTTP requests and WebSocket frames; only
   received records need the full size. Measured peaks: MQTT + tunnel is
   86.2 KB at 16 KB records and 53.4 KB at 8 KB.

**Constraints:**

- `tedge-zephyr/` must not depend on `lib/` or `apps/`.
- "Links" has proven insufficient twice: the connection-context shortage and
  the shell's heap starvation on the C6.
- LTO is unavailable. `SIZE_OPTIMIZATIONS`, asserts off, IPv6 off and
  open62541's minimal namespace zero are already in place.

## Goals / Non-Goals

**Goals:**

- Lower static internal RAM on every release build without removing a feature
  or changing protocol behaviour.
- Make the builds that miss by a small margin fit and pass their board run:
  - WROOM snmp tedge-ota (24 KB over);
  - WROOM modbus/agent full (8.4 / 3.7 KB over);
  - C6 and S3 opcua tedge-ota (runtime OOM).
- Make every RAM size in the configuration effective and justified by a
  measurement.
- **Run the `full` profile on every PSRAM board**: the ESP32-CAM at `full`
  instead of `ota`, and OPC-UA `full` on the S3-DevKitC and QT Py S3.
- **Give flash a budget and a gate**: application at most 80 % of `slot0`,
  provisioner at most 92 % of `prov`, no growth past the baseline; and take at
  least 40 KB off the largest `tedge-full` images.
- Stop regressions with per-build RAM and flash baselines in CI.
- Restore flash headroom in the C6 provisioner's `prov` partition (96.8 %).

**Non-Goals:**

- Serializing operations onto one shared worker.
- Changing the partition layout or MCUboot mode.
- TLS records smaller than 8 KB.
- Adding new board ports.
- Resizing partitions, and saving flash by dropping application log levels,
  diagnostics or shell commands. The provisioner is the exception: its logs
  serve provisioning only and its partition is nearly full.
- Making 2 MB parts viable.
- Feature removal of any kind, including the Wi-Fi SoftAP code the ESP HAL
  forces on.

## Decisions

### D1. Work in three tiers, ordered by risk

| Tier | What | Verification |
|---|---|---|
| 1. Static, behaviour-neutral | AES ROM tables, const open62541 tables, SNMP compaction, `main` stack, tedge heap default, `POSIX_API` scoping, no-op heap lines | link + unit/protocol test on one board per SoC |
| 2. Runtime-sized | mbedTLS output buffer + heap resize, explicit system heap, PSRAM placement, measured stack trims, shell trims | release run on each affected board |
| 3. Flash | provisioner trims, application image trims, Wi-Fi shell scoping, flash budget in the gate | link + provisioning run on the C6, release run where the app image changed |
| 4. PSRAM `full` | CAM at `full`, open62541 allocating from PSRAM, OPC-UA `full` on S3/QT Py | release run on each PSRAM board |

Tier 1 lands first and moves the baseline. Its savings (about 40 KB on SNMP
builds, about 33 KB on OPC-UA builds, about 18 KB elsewhere) are banked before
any runtime risk is taken.

*Alternative:* one big "profile retune" per board. Rejected: a failure could
not be attributed to one change, and this repo's history is full of changes
that linked but failed at runtime.

### D2. open62541: patch const in the regen script, keep type descriptions

`scripts/regen-open62541.sh` gets a post-processing step. It `sed`s the
generated definitions and the `extern` declaration of `UA_TYPES` and the
`*_members` arrays to `const`. It then checks with `grep -c` that the expected
number of definitions changed, and exits non-zero otherwise. The public API
already takes `const UA_DataType *`. If any code in the amalgamation writes to
the tables, the compiler says so.

`UA_ENABLE_TYPEDESCRIPTION` stays on. Turning it off saves only about 3 KB
more, and type names show up in open62541's log output and diagnostics.
`UA_ENABLE_JSON_ENCODING` stays too: the linker already drops it (604 B
remain).

*Alternative:* move the tables with a linker section attribute. Rejected:
that is more fragile than `const`, and the section differs per SoC.

### D3. SNMP: leaves by reference, OIDs derived

- `struct mib_leaf` becomes `{ uint8_t kind; uint8_t col; uint8_t row; }`
  (padded to 4 B) plus the existing value accessor.
- A leaf's OID is produced into a caller's stack buffer by
  `mib_leaf_oid(leaf, buf)` from the constant `system`, `interfaces` and
  enterprise prefixes.
- Lookup (GET) and successor (GETNEXT) compare the request OID against the
  derived OID. They can do so without materialising it, by comparing prefix
  then column then row.
- The table stays sorted in lexicographic order at init, as today, so walk
  order is unchanged.
- Response varbinds hold `const struct mib_leaf *`. Cursors hold a leaf index.
  The requested name stays copied only where the response must echo a name
  that is not a leaf (noSuchObject).
- `MIB_MAX_LEAVES` is sized from `CONFIG_APP_SIM_SWITCH_IF_COUNT`, not
  `SIM_SWITCH_MAX_IF`.

The goal is about 23 KB saved (`leaves` 13.4 KB → about 0.4 KB, `vbs`
6.7 KB → under 1 KB, `cursor` 4.2 KB → about 0.1 KB). The deciding test is a
before/after `snmpwalk`/`snmpbulkwalk` diff on the same board, plus the
existing malformed-PDU and oversized-GETBULK cases.

*Alternative:* lower `BER_MAX_OID_LEN` to 16. Rejected: it caps request OIDs,
so a long request OID would be refused where today it gets `noSuchObject`.
That is a behaviour change.

### D4. mbedTLS: split record buffers via a module-owned user config

`tedge-zephyr/include/tedge/mbedtls_user_config.h` sets
`MBEDTLS_SSL_OUT_CONTENT_LEN 4096`. `IN` stays at `MAX_CONTENT_LEN`. The
profiles select the header with `CONFIG_MBEDTLS_USER_CONFIG_FILE`
(`CONFIG_MBEDTLS_USER_CONFIG_ENABLE` is deprecated in Zephyr 4.4). mbedTLS
includes it from `build_info.h` after Zephyr's config, so it overrides.

*Found while implementing (2026-09-22):* with Zephyr's default
`CONFIG_NET_SOCKETS_TLS_SET_MAX_FRAGMENT_LENGTH`, the TLS socket advertises
the *smaller* of the two buffers as the RFC 6066 maximum fragment length
whenever it is below 16 KB. The header alone would therefore have asked the
server for 4 KB records on every board, which is not what "IN stays at the
negotiated maximum" means. The 16 KB profiles switch the option off (they
never advertised a limit, so the server's records are unchanged); the WROOM
settings keep it on, because an 8 KB input buffer needs the server told, and
Zephyr already advertised 4 KB there (the largest RFC 6066 code below 8 KB).

Outgoing records larger than 4 KB are split by mbedTLS. That is legal TLS and
transparent to MQTT, HTTP and WebSocket. The one path that sends bulk data is
log upload. It already streams in chunks, and those chunks must be at most
4 KB of plaintext per `send()` or be fragmented; this has to be verified.

The heap is then resized per board from `CONFIG_MBEDTLS_MEMORY_DEBUG` peaks.
The workload has the board's maximum concurrent sessions open: MQTT + download
on ota; MQTT + tunnel + log upload on full. The peak gets a margin of 8 KB or
10 %, whichever is larger, which covers handshake fragmentation. The expected
C6 result is 98,304 → about 64 KB. That is the largest single gain, and it
goes straight to the malloc arena.

*Alternative:* `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`. It lets the buffers
shrink after the handshake, but not the peak, and the peak is what sizes the
static heap. Rejected as the primary lever; worth a later look.

### D5. System heap: make the size real, then measure it

The no-op `CONFIG_HEAP_MEM_POOL_SIZE` lines go from every board conf and
`prj.conf`. Each board conf's comment states the heap it actually gets.

On the WROOM, `CONFIG_HEAP_MEM_POOL_IGNORE_MIN=y` with an explicit size is set
only after a soak run. The run covers Wi-Fi reconnects, a firmware download
and a tunnel, and records the low-water mark through the existing diag or
health telemetry. The size is then low-water + 8 KB margin.

On the C6 the heap is already short. The tunnel + shell starvation showed it,
so the C6 heap is not lowered. Instead, `ESP32_WIFI_STATIC_RX_BUFFER_NUM` is
evaluated separately (10 → 6 frees about 6 KB of permanent heap), with the
tunnel throughput checked against the remote-access interactive-tuning notes.

### D6. PSRAM boards: move Wi-Fi heap and network allocations out

S3-DevKitC, QT Py S3 and ESP32-CAM confs add `CONFIG_ESP_WIFI_HEAP_SPIRAM=y`
(the Wi-Fi add-size drops 51,200 → 25,400) and
`CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM=y` (the net_buf pools go to external RAM).

The CAM is the main beneficiary: its bottleneck is dram1, where the net_buf
pools sit. The classic ESP32's PSRAM cache has known DMA restrictions, so this
has to be verified on the CAM before it goes into its config. If the network
allocations fail there, only the Wi-Fi heap move ships, and the board conf
records why.

This keeps `CONFIG_MBEDTLS_HEAP_CUSTOM_SECTION` where it already is and
extends the same idea, and it is what makes D13's CAM `full` build possible.

### D7. Stacks: measure, then trim, with a fixed margin rule

`wroom-stackinfo.local.conf` (thread analyzer) runs the release workloads on
the C6 and the WROOM. Sizes are set to peak + max(25 %, 512 B). The config
line records the peak and the date.

`CONFIG_MAIN_STACK_SIZE` 8192 → 4096 in modbus, snmp and opcua is tier 1.
`main()` returns after starting the threads, and tedge-agent measured 1.2 KB
at 4 KB. It is still checked by the analyzer run.

Candidates for tier 2:

- `TEDGE_THREAD_STACK_SIZE` 6144 (peak 3.3 KB)
- `OPCUA_THREAD_STACK_SIZE` 16384
- `ESP32_TIMER_TASK_STACK_SIZE` 4096
- `net_wq` 3072 (2 KB free)
- the shell's idle dummy stack 3072

The 8 KB firmware and remote-access stacks stay: 4 KB overflowed in the TLS
handshake.

### D8. tedge private heap follows features

In `tedge-zephyr/Kconfig`, `TEDGE_HEAP_SIZE` defaults to 16384 when
`TEDGE_PARAMETERS`, and 10240 otherwise.

The enrolment buffers are 7.6 KB. They are not live at the same time as twin
or parameter work, so 10 KB holds them with margin. That is confirmed with the
existing `tedge_heap_free` health value after enrolment and after a firmware
update. Profiles that set it explicitly keep their value.

### D9. `POSIX_API` only where used

`apps/modbus-server`, `snmp-agent` and `tedge-agent` try without
`CONFIG_POSIX_API`, using the `zsock_*`/ZVFS names they need, which saves the
POSIX object pools (about 2 KB). If a file relies on POSIX names, it switches
to the `zsock_` API only where that is a mechanical rename. Otherwise the
option stays and the conf says why. open62541 keeps it.

### D10. Size baseline in CI

`scripts/release/size.py` gains:

- **Output:** the libc arena that is left (the end of RAM minus
  `_end`/`__heap_start`, or the fullest region's free bytes) and per-region
  totals in the JSON, alongside the image-versus-`slot0` figure it already
  reports.
- **Flag:** `--baseline release/size-baseline.json --tolerance 256`. A RAM
  region or the flash image that grows past the tolerance fails the step.
- **Flash budget:** the existing `--max-slot` gate drops from 95 % to 80 %
  for the application, and the provisioner image is gated the same way
  against its `prov` partition (92 %). The 95 % OTA rule stays as the hard
  ceiling; 80 % is the budget that keeps room to grow.

`size-baseline.json` is keyed by firmware name and generated by the same
script (`--write-baseline`). An image already over its budget carries a
`budget_exceptions` note in its entry (the C6 provisioner until D11 lands):
it then warns instead of failing, but still must not grow.

*Found while implementing:* the flash figure is the image file, and an ESP
image pads its RAM-loaded segments (IRAM code, `.data`) to a 64 KB boundary
before the flash-mapped code and rodata. Bytes that move from `.data` to
rodata therefore grow the image although nothing was added (D2's tables:
+21.7 KB on the C6 OPC-UA image, for the same 21.7 KB of RAM), and small
`.data` changes may not show at all. The PR workflow already runs `size.py` on the
`pr: true` builds, so no new job is needed. The provisioner is built by the
same sysbuild run, so its image size is read from the same build directory.

### D11. Provisioner flash

The C6 provisioner is 1,015,243 of 1,048,576 B:

- log level 2 (from 3), which cuts log strings;
- AES ROM tables with `FEWER_TABLES` (about 6 KB flash);
- dropping `PSA_WANT_*` algorithms the ZTP overlay does not use, checked
  against the ZTP bundle's cipher and signature needs in
  lab-ztp-provisioner.

Target: at most 92 % of `prov`. The provisioning run (Improv and ZTP) on the
C6 must still pass.

*Found while implementing (2026-09-22):* the AES ROM tables were already on
(TF-PSA-Crypto's Kconfig selects them), and the PSA set is already the p256
suite plus what the ITS store (AES-GCM) and the Bluetooth host (CMAC, ECB)
select, so neither item had bytes to give. Level 2 took only 2.8 KB of code.
What decided it is the 64 KB alignment of the flash-mapped code in an ESP
image (see D10): the provisioner's code ended 11.6 KB past a boundary, so
the image only moves once that much code is gone. `CONFIG_LOG_MODE_MINIMAL`
(12 KB, no deferred log core or backend; messages through printk without
timestamps) plus `CONFIG_CBPRINTF_NANO` crossed it: 96.8 → 90.4 %. The
provisioner is the image where a leaner log path is acceptable, for the
reason the non-goals give. It now ends 32 B before the boundary, which the
92 % gate protects.

### D12. Flash: a budget per partition, then encoding-level trims

The flash pressure is not where "26 % of flash" suggests. Per partition:

| Image | Size | Partition | Used |
|---|---|---|---|
| C6 `modbus tedge-full` | 959,147 | slot0 1,280 KB | 73 % |
| C6 `snmp/opcua tedge-full` (with shell) | 1,064,923–1,085,835 | slot0 1,280 KB | 81–83 % |
| QT Py `modbus tedge-full` | 807,691 | slot0 1,280 KB | 62 % |
| C6 wifi-provisioner | 1,015,243 | **prov 1,024 KB** | **96.8 %** |
| MCUboot | 46,512 | boot 64 KB | 73 % |

So the budget is per partition, and the provisioner is the one in danger
(D11). For the application images the target is at least 40 KB off the
largest `tedge-full` builds, from four encoding-level items that remove no
functionality:

1. **Log strings.** Merged string literals are 150–195 KB per image, the
   second-largest flash category after the Wi-Fi blobs. Rather than lowering
   log levels (a non-goal), the levers are `CONFIG_LOG_FMT_SECTION` where the
   SoC supports keeping format strings out of the image, and removing
   duplicated module prefixes. Measure first: this item ships only if it is
   worth more than 10 KB.
2. **`CONFIG_CBPRINTF_NANO`** in images that never print a float or a
   `%p`-heavy diagnostic. Expected 10–20 KB. Checked per app, because the
   telemetry path formats measurements; where floats are printed, the app
   keeps `CBPRINTF_COMPLETE` and says so.
3. **The mbedTLS server role** (`MBEDTLS_SSL_SRV_C`), about 4 KB
   (`mbedtls_ssl_handshake_server_step` is 3,952 B in the C6 full image; the
   define is hard-coded in Zephyr's mbedTLS config, so it can only go through
   the user-config header). The **duplicate SHA-1** turned out not to exist:
   Zephyr's `sha1.c` contributes 340 B (`hmac_sha1` for the WebSocket
   handshake) and the 5.5 KB transform is mbedTLS's own, which TLS needs.
4. **open62541's description strings** (`UA_ENABLE_STATUSCODE_DESCRIPTIONS`,
   `NODESET_COMPILER_DESCRIPTIONS`), which affect OPC-UA images only and
   change log text, not protocol responses. Status codes stay numeric on the
   wire either way.

*Alternative:* `SWAP_USING_MOVE` to reclaim the 124 KB scratch partition.
Rejected here (non-goal): the partition table cannot be changed over the air,
so it would split the fleet.

### D13. `full` on every PSRAM board

Two different reasons block it today, so there are two fixes.

**The ESP32-CAM is blocked by placement.** Its dram1 (96 KB) holds the
net_buf pools and the kernel heap, and `full` overflows it by 33 KB. The
board already puts the mbedTLS heap in its 4 MB PSRAM
(`MBEDTLS_HEAP_CUSTOM_SECTION`) and has already been trimmed to 24/20
net_bufs to fit `ota`. D6 moves the Wi-Fi heap and the network allocations to
PSRAM, which is exactly the dram1 relief `full` needs. Once that holds, the
CAM's net_buf counts go back to the C6/S3 depth, and the board builds `full`.

The classic ESP32's PSRAM cannot be a DMA target the way internal RAM can, so
this is the riskiest item in the change. The order is: Wi-Fi heap first,
verify; network allocations second, verify with a 10-minute protocol run and
a firmware download; only then `full`. If network buffers in PSRAM do not
work, the CAM gets what the Wi-Fi heap move alone allows (expect `ota` plus
remote access or parameters), and the board conf records the failure.

**OPC-UA is blocked by the allocator.** open62541 calls plain `malloc`, so it
draws on the internal libc arena, which is what is left over after
everything else. That is about 33 KB on the C6 and is why sessions fail with
`BadOutOfMemory`. The amalgamation already carries
`UA_ENABLE_MALLOC_SINGLETON` (currently undefined, `open62541.h:284`), which
routes `UA_malloc`/`UA_free`/`UA_calloc`/`UA_realloc` through function
pointers. So:

- `lib/opcua` defines a PSRAM-backed heap (a `k_heap` in `.ext_ram.bss`, or
  `shared_multi_heap` with `SMH_REG_ATTR_EXTERNAL`), sized by a new
  `CONFIG_APP_OPCUA_HEAP_SIZE`;
- `opcua_server.c` assigns the four singletons before `UA_Server_new()`;
- on a board without PSRAM the singletons stay bound to libc `malloc`, so the
  C6 and WROOM behave exactly as today.

This keeps `tedge-zephyr/` out of it: the allocator is host-application glue
in `lib/opcua`, and the module's own heap rules are unchanged. The gain is
that OPC-UA and the client stop competing: on the S3-DevKitC and QT Py,
OPC-UA gets a heap of megabytes, and `full` should serve.

*Alternative:* raise `COMMON_LIBC_MALLOC_ARENA_SIZE` and let everything share
a bigger arena. Rejected: it takes the bytes from the same internal RAM, and
it would let open62541 starve the client instead of the other way round.

*Note on the C6:* it has no PSRAM, so D13 does not help it. The C6's OPC-UA
hope rests on tiers 1–2 (D2's 21 KB, D4's ~32 KB) growing the arena from
about 33 KB towards about 95 KB, above the ~68 KB at which the WROOM
standalone served.

## Risks / Trade-offs

- **An image links but fails at runtime.** It has happened twice.
  → Every tier-2 change is gated by the release run on each affected board
  (spec: Footprint work preserves functionality). Tier 1 is verified with
  protocol tests on at least one board per SoC family.
- **A splitting or peak edge case not covered by the measured workload.**
  → The heap margin rule, and the existing "TLS heap too small" logging and
  backoff, make a failure visible and recoverable. The heap is measured with
  every session type open at once.
- **The SNMP rewrite introduces a walk-order or `noSuchInstance` difference.**
  → A before/after walk diff on the same image values is the acceptance test.
  Walk order is compared as data, not by eye.
- **open62541 regen patch drifts on upgrade.** → The script counts
  replacements and fails loudly.
- **PSRAM network buffers on the classic ESP32 (CAM).** DMA cannot target
  PSRAM the way it targets internal RAM, and a failure here can look like
  corrupted frames rather than a clean error. → Verify in the order set in
  D13, with a protocol run and a firmware download; fall back to the
  Wi-Fi-heap-only move and record it.
- **PSRAM is slower than internal RAM**, so moving the network buffers and
  open62541's heap there costs throughput. → The CAM and the S3 runs measure
  protocol reads and the firmware-download time against the current release;
  a regression beyond 25 % means the buffers stay internal.
- **`CBPRINTF_NANO` silently changes formatting** if a float or an
  unsupported specifier is printed. → Applied per app only after grepping the
  format strings, and the telemetry and diagnostic output is compared before
  and after on one board.
- **A flash budget of 80 % could block a legitimate feature.** → It is a
  budget, not the OTA ceiling (95 %), and raising it is a one-line change with
  a stated reason.
- **Savings are consumed by the malloc arena instead of fixing a link.** On
  OPC-UA this is the intent. For link-bound builds the baseline shows where
  the bytes went.
- **Lower performance.**
  - ROM AES tables slow AES slightly. Irrelevant beside Wi-Fi throughput.
  - A 4 KB output buffer means more records for bulk upload. Log upload
    throughput is checked.

## Migration Plan

- There is no device-side migration. The images keep their layout and
  features, and devices update over the air as usual.
- Each tier lands as its own PR, with the baseline updated in the same PR.
- **Rollback:** revert the PR. Devices can OTA back to the previous release
  image, since the layout is unchanged.
- New `release/devices.yml` rows are added only in the PR that records their
  board run in `DEVICES.md`.

## Open Questions

- **Largest outgoing record.** Does Cumulocity's MQTT Service
  or the remote-access WebSocket endpoint ever require the device to *send* a
  record larger than 4 KB? Expected no. The first tier-2 run answers it.
- **Does the WROOM OPC-UA tedge-ota become viable?** It is likely to link,
  but its arena would be about 25–35 KB, below the level where C6 sessions
  failed. Measure the arena open62541 actually needs per session before
  listing it.
- **How big should `APP_OPCUA_HEAP_SIZE` be?** open62541's peak per session
  is not measured yet. The first tier-4 run sizes it, starting from the
  ~68 KB at which the WROOM standalone served and scaling by session count.
- **Does the CAM's Wi-Fi driver tolerate PSRAM buffers at all?** If not, the
  CAM's `full` goal falls back to "as much as the Wi-Fi heap move allows",
  and the change says so rather than dropping the goal silently.
- **Is `LOG_FMT_SECTION` available on these SoCs?** If it is not, the log-
  string item in D12 is worth much less, and the 40 KB flash target leans on
  `CBPRINTF_NANO` instead.
- **ESP32-C3 gap.** After tiers 1–2, record the remaining gap for an ESP32-C3
  (about 320 KB usable SRAM) in `DEVICES.md` as input for a board-port
  change.
