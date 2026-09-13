## Context

This is the first firmware application in the repo and establishes the Zephyr
RTOS build, configuration, and testing patterns the rest of the project builds
on. The goal for Phase 1 is a device that acts as an **OPC-UA server**, serving
device/sensor data over the network so a separate collector can read it via a
standard OPC-UA client.

Current state: empty repo (only OpenSpec scaffolding). No Zephyr workspace, no
board bring-up, no OPC-UA integration yet.

The on-hand hardware is Wi-Fi-only and resource-constrained; there is no
Ethernet board in the mix. Targets:
- ESP32-WROOM-32 — dual-core, ~520 KB internal SRAM, Wi-Fi (co-primary).
- Feather ESP32-S2 TFT — single-core, ~320 KB SRAM + 2 MB PSRAM, Wi-Fi
  (co-primary; PSRAM is the practical memory enabler here).
- Raspberry Pi Pico W (RP2040) — ~264 KB SRAM, CYW43 Wi-Fi (additional/stretch;
  Zephyr Wi-Fi support less mature than ESP32).

Key constraints:
- Tight RAM shared with a Wi-Fi stack — the OPC-UA stack must run within a
  bounded, measured RAM budget on each part.
- Wi-Fi (station mode) is the only transport; the device needs Wi-Fi
  credentials to join a network before serving OPC-UA.
- Reusability/portability across these Zephyr boards is a priority;
  board-specific settings live in overlays, not the app core.
- Must be developable and testable without hardware (`native_sim`).

## Goals / Non-Goals

**Goals:**
- A Zephyr application that runs an OPC-UA server and serves a small, static
  address space with device identity + one or more live measurement variables.
- A standard OPC-UA client can connect, browse, and read the served nodes.
- Reusable build: primary hardware target `nucleo_f767zi` plus a hardware-free
  `native_sim` target used for development and automated tests.
- A data-source abstraction so the sampled values can later come from real
  sensors without touching the OPC-UA layer.

**Non-Goals:**
- OPC-UA writes, methods, subscriptions/monitored items, PubSub, historical
  access (read-focused for Phase 1).
- Encryption/certificate-based security profiles (basic endpoint only).
- Real sensor drivers (simulated source first).
- thin-edge.io / Cumulocity connectivity (Phase 3) and other protocols (Phase 2).

## Decisions

### Decision: Use the open62541 OPC-UA stack
open62541 is a permissively licensed (MPL-2.0), portable C99 OPC-UA stack with a
configurable "nano"/minimal build profile that trims RAM/flash footprint. It has
prior use on embedded/RTOS targets and does not assume a full OS.

- **Alternatives considered**: FreeOpcUa (C++, heavier, less embedded-focused);
  a hand-rolled minimal OPC-UA server (too much protocol surface to implement
  correctly for interoperability); commercial SDKs (licensing/cost, closed).
- **Why open62541**: best fit for C + Zephyr + constrained resources, and its
  amalgamated single-file build integrates cleanly as a Zephyr module/library.

### Decision: Integrate open62541 as a Zephyr module/library, not upstream fork
Vendor open62541 via a `west` manifest entry (or as a Zephyr module with a thin
`CMakeLists.txt`/`Kconfig`), building the amalgamated source with a
constrained-profile configuration. This keeps our app code separate from the
stack and makes stack upgrades a manifest bump.

- **Alternatives**: copy sources directly into the app (harder to update);
  fork and patch (maintenance burden). Prefer a pinned external dependency.
- **Porting layer**: provide the small platform glue open62541 needs (clock,
  sockets via Zephyr BSD sockets/POSIX, memory). Zephyr's POSIX + BSD socket
  support is the integration seam.

### Decision: Co-primary ESP32-WROOM-32 + ESP32-S2, Pico W as stretch, `native_sim` for dev/CI
Both ESP32 variants are treated as co-primary hardware targets; the Pico W is an
additional/stretch target. `native_sim` (with Zephyr's networking backend, e.g.
a host TAP/`zeth` interface) is used for host-side development and automated
tests against a real OPC-UA client — no hardware needed for iteration/CI.

- **Why co-primary ESP32 parts**: they are the boards on hand with the most
  headroom. WROOM-32 gives the most internal SRAM (~520 KB) and a second core;
  the S2 has less internal SRAM (~320 KB) but 2 MB PSRAM, which is the practical
  way to give open62541 room. Supporting both from the start forces the config/
  overlay abstraction to be real rather than bolted on later.
- **Pico W as stretch**: ~264 KB SRAM and less mature Zephyr Wi-Fi (CYW43) make
  it the riskiest; validate the ESP32 path first, then attempt Pico W.
- **Alternatives**: pick a single ESP32 as sole primary (less portability
  pressure, but hides board-specific assumptions); an Ethernet dev board (none
  on hand, and out of scope for the available hardware).

### Decision: Wi-Fi (station mode) as the transport
All targets are Wi-Fi-only, so the server is reachable over Wi-Fi after the
device joins a network in station mode. Wi-Fi credentials (SSID/PSK) are
provided via configuration. On `native_sim`, the host network backend stands in
for the radio so the same server logic is exercised without Wi-Fi.

- **Implication**: the Wi-Fi stack (ESP32 driver, or CYW43 on Pico W) shares the
  RAM budget with open62541 — a primary sizing constraint, measured per board.
- **Alternatives**: SoftAP mode (device hosts its own network) — deferred;
  station mode against an existing LAN is the expected collector-on-same-network
  topology.

### Decision: Discovery via mDNS / DNS-SD (Zephyr responder preferred)
Advertise the device over mDNS so it resolves as `<hostname>.local`, and publish
the OPC-UA endpoint as a DNS-SD service (`_opcua-tcp._tcp`). This lets the
collector (and tools like `dns-sd`/UaExpert) find the device without a
pre-known IP — valuable on Wi-Fi/DHCP where the address can change.

- **Preferred**: Zephyr's built-in mDNS responder + DNS-SD (`CONFIG_MDNS_RESPONDER`,
  `CONFIG_DNS_SD`). It is integrated with Zephyr's network stack, lighter, and
  independent of open62541's build profile.
- **Alternative**: open62541's own multicast discovery
  (`UA_ENABLE_DISCOVERY_MULTICAST`), which advertises `_opcua-tcp._tcp` itself.
  Rejected as the default because it adds RAM/flash to the OPC-UA build and can
  run its own mDNS responder that duplicates/conflicts with Zephyr's on the same
  interface. Keep it as a fallback if Zephyr DNS-SD proves insufficient.
- **Risk of running two mDNS responders**: only one should own the interface —
  do not enable both open62541 multicast discovery and Zephyr's responder at once.

### Decision: macOS as the primary development machine
Build/flash tooling and docs must work on macOS first (Linux differences noted).
Practical implications to capture in docs:
- ESP32 boards typically enumerate as `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*`
  on macOS and may need a USB-serial driver (CP210x/CH34x) depending on the
  board; flashing goes through `west flash` (esptool under the hood).
- Pico W uses UF2 mass-storage flashing (drag-and-drop / `west flash`), which
  behaves similarly on macOS and Linux.
- macOS has native mDNS (Bonjour): `dns-sd -B _opcua-tcp._tcp` and
  `ping <hostname>.local` work out of the box for discovery testing.
- `native_sim` runs as a Linux binary; on an Apple-Silicon/macOS host it is run
  via a Linux toolchain/VM/container — note this so contributors aren't blocked.

### Decision: Static, minimal address space with a pluggable data-source layer
Build a small, mostly static address space at startup: a device object (identity
metadata) plus one or more measurement variables. A separate `data_source`
module periodically samples values (simulated in Phase 1) and writes them into
the corresponding OPC-UA variable nodes.

- **Why**: bounds memory (no dynamic node explosion), keeps the OPC-UA layer
  independent of where data comes from, and makes swapping in real sensors a
  localized change.
- **Alternatives**: value callbacks that read the source lazily on client read
  (also viable) — chosen the push/sample model first for a predictable sampling
  cadence; lazy callbacks can be added later.

### Decision: Configuration via Kconfig + prj.conf, board specifics via overlays
Expose tunables (server port, application/device name, sampling interval, and
Wi-Fi credentials/SSID) through Kconfig with sane defaults in `prj.conf`. Keep
board-specific bits (Wi-Fi driver, PSRAM enablement, pin/clock config) in
`boards/<board>.overlay` / `boards/<board>.conf` so the app core stays portable.
Wi-Fi credentials should be overridable without committing secrets (e.g. a local
`.conf`/overlay or build-time define), not hard-coded in the app.

## Risks / Trade-offs

- **[open62541 RAM footprint competes with the Wi-Fi stack]** → Use the reduced/
  nano build profile, cap max sessions/connections, keep the address space
  static/small, and measure RAM/flash early on each board. On ESP32-S2, place
  open62541 heap in PSRAM. Document the minimum viable footprint per board and
  drop the tightest target (Pico W) from the milestone if it can't fit.
- **[Zephyr ESP32/Pico W Wi-Fi maturity]** → Zephyr's ESP32 Wi-Fi and CYW43
  drivers are less battle-tested than ESP-IDF. Bring Wi-Fi up and prove a stable
  TCP socket on hardware *before* layering OPC-UA on top; if Zephyr Wi-Fi proves
  unworkable on a given board, record it and fall back to the boards that work.
- **[open62541 ↔ Zephyr porting friction (sockets, clock, POSIX)]** → Lean on
  Zephyr's POSIX + BSD socket layer; isolate glue in a small porting file so
  breakage is contained and testable on `native_sim` first.
- **[`native_sim` networking differs from real Wi-Fi]** → Treat `native_sim` as
  functional/protocol validation only; always confirm the Phase 1 milestone on
  real ESP32 hardware over Wi-Fi before calling it done.
- **[Two mDNS responders conflicting]** → Enable exactly one mDNS responder on
  the interface (Zephyr's, preferred); keep open62541 multicast discovery off
  unless it replaces Zephyr's entirely.
- **[`native_sim` does not run natively on macOS]** → `native_sim` is a
  Linux binary; on the macOS dev machine, run it via a Linux container/VM for
  local host testing and CI, or rely on hardware. Document this so macOS
  contributors aren't blocked.
- **[Interoperability gaps with specific OPC-UA clients]** → Validate against at
  least one widely used reference client (e.g. UaExpert / open62541 client) and
  keep the endpoint to standard, broadly supported settings.
- **[Scope creep toward security/other protocols]** → Explicitly deferred;
  basic unsecured, anonymous endpoint acceptable for the Phase 1 milestone,
  tracked as a follow-up.

## Open Questions

- Which open62541 version/profile and exact Kconfig footprint settings fit each
  board — and does the WROOM-32 (no PSRAM) have enough internal SRAM alongside
  Wi-Fi, or is PSRAM (S2) effectively required?
- Is Zephyr's Wi-Fi support on these specific parts (ESP32-WROOM-32, ESP32-S2,
  Pico W/CYW43) mature enough for a stable OPC-UA server, or should any board be
  descoped?
- `native_sim` networking approach for CI: `zeth`/TAP setup vs. an alternative
  loopback path — what is simplest to run in automation?
- Minimal address-space shape: how many measurement variables and which
  standard node identifiers/namespaces to use for the first milestone.
- How should Wi-Fi credentials be supplied (build-time config vs. runtime
  provisioning) without committing secrets?
- Is anonymous-only access acceptable for Phase 1 (assumed yes)?
- mDNS: is Zephyr's DNS-SD responder sufficient and stable on the ESP32/CYW43
  Wi-Fi drivers, or is open62541's multicast discovery needed as a fallback?
- macOS dev flow for `native_sim`: standardize on a specific container/VM image
  so host-side tests are reproducible for macOS contributors?
