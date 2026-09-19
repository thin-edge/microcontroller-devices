## Context

Phase 3 builds **tedge-zephyr**: a thin-edge.io device-management client
shipped as a reusable Zephyr module that any user's application can include.
This repository's OPC-UA, Modbus and SNMP apps are example "user
applications". The client has two transports behind one feature set:

```
  ┌──────────────────── any user's Zephyr application ─────────────────────┐
  │  main(), own threads, own connectivity (Wi-Fi/Ethernet), own data      │
  │      │ tedge_publish_*()  tedge_register_*()        ▲ hooks: restart,  │
  │      ▼                                               │ fw confirm, state│
  │  ┌───────────── tedge-zephyr (module `tedge`) ───────┴──────────────┐  │
  │  │ restart │ firmware │ logs │ config │ shell │ telemetry │ remote  │  │
  │  └────────────────────────┬─────────────────────────────────────────┘  │
  └───────────────────────────┼────────────────────────────────────────────┘
                              │ transport interface
                    ┌─────────┴──────────────────────────┐
         ┌──────────▼───────────┐            ┌───────────▼──────────┐
         │ transport: c8y       │            │ transport: tedge     │
         │ (FIRST)              │            │ (LATER)              │
         │ MQTTS :9883 SmartREST│            │ te/ MQTT API :1883   │
         │ + free-form topics   │            │ + file transfer :8000│
         │ HTTPS+JWT, WSS       │            │ mDNS discovery       │
         └──────────────────────┘            └──────────────────────┘
```

Facts established during exploration (2026-09-19), which the spikes build on:

- **Cumulocity CA enrollment** (thin-edge.io `tedge cert download c8y`,
  `crates/core/tedge/src/cli/certificate/c8y/download.rs`):
  - The device generates the one-time password (at most 32 characters) and
    prints `https://<tenant>/apps/devicemanagement/index.html#/deviceregistration?externalId=<id>&one-time-password=<otp>`.
  - It then polls `POST https://<tenant>/.well-known/est/simpleenroll` with
    `Authorization: Basic <id>:<otp>` and `Content-Type: application/pkcs10`
    (PEM body without the armour lines) until it gets `200` with the
    certificate.
  - Renewal is `POST …/.well-known/est/simplereenroll`. `tedge` sends it
    through its local proxy with the device identity, so how the device
    authenticates directly (mTLS or JWT) is **unverified**.
- **The Cumulocity MQTT Service** (port 9883) carries free-form topics (GA).
  The SmartREST layer on it is **Public Preview** and needs the tenant feature
  `mqtt-service.smartrest`. Core MQTT (8883) is GA and SmartREST-only.
  Free-form messages are not stored as measurements until something consumes
  and maps them.
- **thin-edge.io gateway facts** (for the later transport):
  - Remote access is not available for child devices, because the mapper
    ignores SmartREST custom operations aimed at a child. Direct mode is the
    only way to reach a device-side remote-access endpoint.
  - `firmware_update` carries no `sha256`, so MCUboot's signature check is the
    integrity guarantee in both modes.
- **Zephyr 4.4.2** provides:
  - the MQTT client with TLS and websocket transports;
  - the HTTP client;
  - SNTP;
  - `SECURE_STORAGE` (PSA ITS over settings);
  - mbedTLS with `X509_CSR_WRITE_C`;
  - `dfu/flash_img` and `dfu/mcuboot`;
  - shell backends over telnet, websocket and MQTT;
  - `log_backend_mqtt`;
  - coredump to a flash partition.

  Two limits matter here. `tls_credentials` takes a private key only as a
  **buffer**; there is no opaque PSA key-ID credential type. And
  `MBEDTLS_SSL_MAX_CONTENT_LEN` defaults to **1500**.
- **This repo already has:**
  - MCUboot with swap-scratch and a development key, and A/B slots of 1280 KB
    (4 MB boards) or 3072 KB (S3-DevKitC-1);
  - a `storage` partition with settings/NVS shared with the provisioner;
  - `identity.*`, `liveness` and `diag`;
  - the RGB and GPIO status LED.

  The task watchdog has 4 of its 5 channels in use.

## Goals / Non-Goals

**Goals:**

- Turn each unknown below into a measured answer with a go/no-go.
- Produce the per-feature footprint numbers that decide board profiles.
- Leave behind the `tedge-zephyr/` module skeleton: the `CONFIG_TEDGE_*`
  menu that encodes build-time feature selection, an outline of the public
  header, and `samples/minimal`. The next change then adds feature logic into
  slots that already exist.
- Establish the integration contract (D7) and a directory layout that can
  move out of this repo (D8) before any feature code exists, so that no
  dependency on this repo's `lib/` creeps in.
- Prove that remote access can tunnel from Cumulocity through the MCU to
  **another host on the LAN** (for example SSH), because that is the feature
  users will value most (D9).

**Non-Goals:**

- Production-quality client code. Spike code may be crude, and it stays in
  `apps/c8y-spike/`.
- Any change to the three protocol apps or the provisioner.

## Unknowns and how each spike answers them

| # | Unknown | Answered by | Decides |
|---|---|---|---|
| U1 | Does TLS to 9883/8883 fit on the C6 and S3? Steady and peak heap, flash, handshake time | A | Board profiles; how many sessions can run at once |
| U2 | Does the handshake work with small records (`MAX_CONTENT_LEN` < 16 KB), i.e. does the server honour max-fragment-length? | A | Whether 16 KB input buffers are mandatory |
| U3 | Do SmartREST operations (`s/us`, `s/ds`, `s/e`) work over the MQTT Service with the feature flag? | A | Default transport profile (`mqtt-service` or `core`) |
| U4 | Does `s/uat` → `s/dat` return a JWT on 9883, with basic auth and with a certificate? | A, C | How HTTPS downloads and uploads authenticate |
| U5 | How long does an MCUboot swap take? Does revert work from a real HTTP download? Is `prov`, `storage` or `bootreq` touched? | B | The firmware-update design |
| U6 | Are redirects followed, and does a GitHub-release-style URL work? | B | Whether redirect handling must be written |
| U7 | Can the device generate the key, CSR and one-time password, enroll, and connect with mTLS? | C | The onboarding design |
| U8 | How does `simplereenroll` authenticate when called directly? | C | The certificate-renewal design |
| U9 | Is holding the private key as a buffer in RAM acceptable, or is an opaque-key TLS path needed? | C | Security posture; possible upstream work |
| U10 | Can a SoftAP provisioner fit 1024 KB, and does AP and station at the same time work? | E (optional) | BLE or captive portal |
| U11 | Can the MCU bridge a Cumulocity remote-access WebSocket (WSS) to a TCP connection to another LAN host? What are the RAM per session, throughput and latency with SSH/SCP? | F | Remote-access design, session limits per board |
| U12 | What does the device receive for `c8y_RemoteAccessConnect` over SmartREST (host, port, connection key), and which URL and authentication does the device-side WebSocket need (mTLS or JWT)? | F | Remote-access protocol handling |

## Decisions

### D1: Spikes live in a throwaway app; only the Kconfig skeleton is kept

`apps/c8y-spike/` composes `lib/common` (for Wi-Fi, identity and the data
model, as any user app would bring its own) and the `tedge-zephyr` module.
Spike code (the MQTT loop, the OTA download, the enrollment, the tunnel) lives
in the app's `src/` and is selected by spike Kconfig options
(`SPIKE_TLS_MQTT`, `SPIKE_OTA`, `SPIKE_ENROLL`, `SPIKE_REMOTE_ACCESS`). It is
built with sysbuild, so MCUboot and `slot1` are real. The module itself only
gets the skeleton.

- *Alternative: put spike code straight into `tedge-zephyr`.* Rejected: spike
  code is written to measure, not to keep, and it would anchor the design
  before the results are in.
- *Alternative: extend one of the protocol apps.* Rejected: it would mix the
  frontend's heap use with the client's and blur the measurements. The
  footprint table instead measures the client on top of each app in a separate
  build step (D5).

### D2: Build-time feature selection in `tedge-zephyr/Kconfig`

The skeleton this change leaves behind:

```
menuconfig TEDGE                      "thin-edge.io device management client" (default n)
  choice TEDGE_TRANSPORT
    TEDGE_TRANSPORT_C8Y               direct to Cumulocity
    TEDGE_TRANSPORT_GATEWAY           via a thin-edge.io gateway  (not yet selectable)
  choice TEDGE_C8Y_ENDPOINT           (if TEDGE_TRANSPORT_C8Y)
    TEDGE_C8Y_MQTT_SERVICE            :9883, SmartREST + free-form (default; needs TEDGE_AUTH_C8Y_CA)
    TEDGE_C8Y_CORE_MQTT               :8883, SmartREST only (GA fallback; basic-auth builds use it)
  choice TEDGE_AUTH
    TEDGE_AUTH_C8Y_CA                 x.509 from the Cumulocity CA (default)
    TEDGE_AUTH_BOOTSTRAP              basic auth via bootstrap user
  config TEDGE_TELEMETRY              tedge_publish_measurement/event/alarm()
  config TEDGE_HEALTH                 heap, RSSI, uptime, reset reason
  config TEDGE_RESTART
  config TEDGE_FIRMWARE_UPDATE        depends on BOOTLOADER_MCUBOOT; select TEDGE_HTTP
  config TEDGE_SHELL_COMMAND          depends on SHELL
  config TEDGE_LOG_UPLOAD             select TEDGE_HTTP
  config TEDGE_REMOTE_ACCESS          depends on TEDGE_AUTH_C8Y_CA; selects websocket
    choice TEDGE_REMOTE_ACCESS_TARGETS    see D9
      TEDGE_REMOTE_ACCESS_TARGETS_LAN     any host on the device's own IPv4 subnets (default)
      TEDGE_REMOTE_ACCESS_TARGETS_LIST    only host:port pairs in an allow-list (Kconfig or settings)
      TEDGE_REMOTE_ACCESS_TARGETS_LOCAL   only the device itself (127.0.0.1 / own address)
    config TEDGE_REMOTE_ACCESS_MAX_SESSIONS   default 1; each session costs one TLS session
    config TEDGE_REMOTE_ACCESS_BUF_SIZE       per-direction bridge buffer
  config TEDGE_CONFIG                 select TEDGE_HTTP
  config TEDGE_CERT_RENEWAL           depends on TEDGE_AUTH_C8Y_CA
  config TEDGE_HTTP                   (hidden) HTTPS client + JWT handling
  config TEDGE_THREAD_STACK_SIZE / TEDGE_THREAD_PRIORITY / TEDGE_HEAP_SIZE
  config TEDGE_TLS_TAG_BASE           first TLS credential tag the module may use
```

Features without an implementation are declared with `depends on
TEDGE_FEATURE_AVAILABLE_<X>`, a hidden symbol that stays `n` until the feature
lands. They show in `menuconfig` but cannot be selected. Profiles are overlays
shipped with the module (`tedge-zephyr/profiles/minimal.conf` and
`full.conf`). Board and app defaults live in `apps/<app>/boards/*.conf` once
the numbers exist, because they belong to the host application and not to
the module. The `TEDGE_` prefix keeps the thin-edge.io branding and avoids
colliding with a host application's own symbols.

- *Alternative: one `DM` switch with everything always included.* Rejected:
  the WROOM running OPC-UA cannot afford even one extra TLS session plus HTTP,
  and the requirement is that features can be removed.
- *Alternative: runtime enable/disable.* It does not save flash or static RAM,
  which is the point. A runtime switch may be added on top later, for
  operators.

### D3: Spike A configuration

- **Time:** SNTP (`pool.ntp.org`, or the gateway if configured) runs before
  the first TLS connect. Certificates cannot be validated before the clock is
  set.
- **Server trust:** the tenant's CA chain (fetched once with
  `openssl s_client -showcerts`) is embedded as a `TLS_CREDENTIAL_CA_CERTIFICATE`
  in the spike. How trust anchors are managed in production is out of scope,
  but we record which roots were needed.
- **Authentication:** Spike A uses **mutual TLS** with a Cumulocity CA
  certificate for `tedge-spike-c6`, issued from a PC (key and CSR generated
  on the PC, `register-ca`, EST `simpleenroll`). It is embedded from the
  git-ignored `apps/c8y-spike/credentials/`. This replaces the planned basic
  auth, because the MQTT Service (9883) refuses basic auth (see Spike
  results, section 1). Spike C then moves key generation and enrollment onto
  the device, under the default identity `tedge-<MAC>`. The basic-auth
  device `tedge-spike-a-c6` remains available for an 8883 comparison.
- **Subscriptions:** one filter per SUBSCRIBE, exact topics only: `s/ds`,
  `s/e`, `s/dat`, `devicecontrol/notifications`, `error`.
- **Record size:** first attempt with `MBEDTLS_SSL_MAX_CONTENT_LEN=16384`,
  then 4096 with `MBEDTLS_SSL_MAX_FRAGMENT_LENGTH`. Record which one completes
  the handshake and moves data.
- **Heap:** measured through the `diag` health line (`heap=`, `malloc=`) and
  `sys_heap` runtime statistics at four points: idle, handshake peak,
  connected steady state, and with a concurrent HTTPS download.
- **Topics exercised:**
  - `s/us`: `100` (create), `114` (supported operations, `c8y_Restart`), `117`
    (required interval).
  - `s/ds`: receive `510` (restart) and report `501`/`503`.
  - One free-form topic, e.g. `te/device/<id>///m/environment`, with a
    thin-edge.io-shaped JSON payload. This tests whether the same payload shape
    can serve both transports later.
  - `s/uat` → `s/dat`.

### D4: Spike B mechanics

- The download is streamed through `flash_img_buffered_write()` into
  `slot1_partition`, with chunks of about 1–4 KB. The whole image is never
  held in RAM.
- The source is `python3 -m http.server` on the laptop first. Then HTTPS
  against a Cumulocity binary with the Spike A JWT, and a GitHub release asset
  to check redirects.
- After writing: check the image header and hash with `flash_img_check()`,
  call `boot_request_upgrade(BOOT_UPGRADE_TEST)`, then reboot.
- The new image confirms itself with `boot_write_img_confirmed()` after a
  shell command (the spike stands in for "reached Cumulocity"). In a second
  run it does not confirm, and a reset must revert it.
- Check that `prov`, `storage` (Wi-Fi credentials survive) and `bootreq` are
  unchanged, by reading them back or from the application's behaviour.
- The spike image carries a build-time version bump (`VERSION`), so each
  boot's version is unambiguous in the log.

### D5: Footprint measurement method

- One script, `scripts/measure_tedge.sh`, modelled on `measure_prov.sh`. It
  builds each (board, app) with `TEDGE=n`, with the minimal profile, and with
  each feature added on its own. It also builds `samples/minimal`, which gives
  the module's cost with no host app. It reports the flash and static RAM deltas from the link
  map, and the `zephyr.signed.bin` size against the slot.
- Runtime heap numbers come from the spikes on hardware. They cannot be read
  from a link map.
- WROOM builds are build-only. Their static numbers, plus the C6/S3 runtime
  numbers, decide whether any feature fits next to OPC-UA.

### D6: Spike C enrollment flow

```
 boot ─► SNTP ─► key in PSA ITS? ──no──► psa_generate_key(P-256, persistent)
                    │yes
                    ▼
            cert in storage? ──yes──► mTLS connect (Spike A code)
                    │no
                    ▼
   otp = 32 chars [A-Za-z0-9] from the CSPRNG, stored in settings
   print registration URL (console + `tedge enroll` shell command)
   CSR: CN=<external id: set by the app; default tedge-<MAC>>, signed with the PSA key
   loop every 10 s: POST simpleenroll (Basic id:otp) ─► 200? store cert PEM ─► connect
```

- The key is exported into a RAM buffer only to register it as
  `TLS_CREDENTIAL_PRIVATE_KEY` for the session (U9). We record how long it
  stays resident and whether mbedTLS's PSA path can avoid the export.
- After a successful connection: try `simplereenroll` with mTLS only, then with
  `Authorization: Bearer <JWT>`, and record which one works (U8).
- The bootstrap fallback: connect with the bootstrap user, publish on `s/ucr`
  until the response is `70,<tenant>,<user>,<password>`, store the result, and
  reconnect as that user. This is only proved to work; its cost is recorded
  but not optimised.
- In production the provisioner generates the one-time password and returns
  the URL through Improv. The spike does it in the app, because the
  provisioner has no room for crypto and only needs a random string and a
  settings write, and that part is not in question.

### D7: Integration contract with the host application

The module is a guest in someone else's image:

| Concern | The host application | The module |
|---|---|---|
| Network | Brings up and recovers Wi-Fi or Ethernet | Waits for `NET_EVENT_IPV4_ADDR_ADD`/`IF_DOWN` (net_mgmt); never touches the interface |
| Identity | Sets the external ID, name, type and firmware name/version via Kconfig or the API | Defaults: external ID `tedge-<MAC>`, firmware from `APP_VERSION`/`KERNEL_VERSION` |
| Telemetry | Calls `tedge_publish_measurement/event/alarm()` | Buffers (bounded), encodes and sends; never samples on its own |
| Operations | May `tedge_register_operation()`, `…_log_type()`, `…_config_type()` | Built-in handlers for its compiled-in features; custom ones go to the app |
| Restart | Restart hook: prepare, or veto with a reason | Calls `sys_reboot()` only after the hook allows it |
| Firmware confirm | Adds its own health checks | Confirms only when the cloud is reachable **and** every app check has passed |
| Remote access | May narrow the targets (allow-list, or a per-request hook) | Enforces the Kconfig policy (D9) before opening any TCP connection |
| State and LED | Subscribes to `tedge_state` changes | Emits states (waiting for network, enrolling, connecting, connected, updating, tunnel open) and never drives GPIO |
| Watchdog | Owns the task watchdog | Calls an optional progress hook from each thread it owns |
| Resources | Provides the socket, poll and mbedTLS config the README lists | Own threads and own `k_heap`; settings under `tedge/`; TLS tags from `TEDGE_TLS_TAG_BASE` |

In this repo, the glue between `lib/common` and `tedge_*` (data_source to
telemetry, the tedge state to `status_led`, the progress hook to
`app_alive()`) lives in `apps/<app>/src/`, never in the module.

### D8: A directory layout that can move out of the repo

```
tedge-zephyr/                 ← becomes the root of its own repo (e.g. thin-edge/tedge-zephyr)
├── zephyr/module.yml         name: tedge
├── CMakeLists.txt, Kconfig, VERSION
├── include/tedge/            public headers only
├── src/                      (from c8y-direct-core on)
├── profiles/                 minimal.conf, full.conf
├── samples/minimal/          builds with Zephyr + this module only
├── tests/                    (twister, native_sim)
└── README.md                 integration guide + the D7 table
```

- **Today:** apps add it with `list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tedge-zephyr)`, the same way they add
  `lib/common`.
- **Later:** `git filter-repo --subdirectory-filter tedge-zephyr` (or
  `git subtree split`) moves it out with its history. `west.yml` gains a
  project for it, and the one `ZEPHYR_EXTRA_MODULES` line per app is removed.
  No file inside the module changes.
- **Rules that keep this true:**
  - nothing under `tedge-zephyr/` references `../`;
  - the module has its own README and changelog;
  - its version lives in `tedge-zephyr/VERSION`, independent of the apps'
    `VERSION`.
- *Alternative: `modules/tedge-zephyr/`.* Rejected: `.gitignore` excludes
  `/modules/`, because west can clone Zephyr modules into the tree there.
- *Alternative: `lib/tedge/`.* Rejected: `lib/` holds this repo's internal
  modules, and the placement would invite the dependencies D8 forbids.

### D9: Remote access through the MCU to hosts on the LAN

The MCU becomes a small **remote-access gateway**. Cumulocity's existing
remote-access UI and its passthrough, SSH, VNC and Telnet endpoints work
unchanged, and the target can be the MCU itself or any host it can reach:

```
 ssh/scp/VNC client ─► Cumulocity Cloud Remote Access ─► WSS (device-initiated, mTLS/JWT)
                                                               │
                                           ┌───────────────────▼──────────────┐
                                           │ MCU: tedge remote-access bridge  │
                                           │  policy check (D2 targets) ──────┼─► 192.168.1.20:22  (Pi / PLC / HMI)
                                           │  WSS ⇄ TCP, fixed buffers        │─► 127.0.0.1:23   (Zephyr shell_telnet)
                                           └──────────────────────────────────┘─► own :4840/:502 (OPC-UA / Modbus)
```

- **Flow** (thin-edge.io's `c8y-remote-access-plugin` has the same shape):
  1. The operation `c8y_RemoteAccessConnect` arrives with host, port and a
     connection key (U12 confirms the exact template).
  2. The device checks the target against the policy.
  3. It opens a TCP connection to the target.
  4. It opens the WebSocket to Cumulocity's device endpoint for that key.
  5. It pipes bytes both ways until either side closes.
  6. It reports the operation as successful once the tunnel is up, or as
     failed with a reason: policy denied, target unreachable, or WebSocket
     rejected.
- **Why this is valuable:** the gateway transport can't offer it, because
  thin-edge.io ignores remote access aimed at child devices. A brownfield site
  with an MCU on Wi-Fi gets SSH access to the machines next to it without a
  Linux gateway. It is the strongest reason to pick the direct transport.
- **Target policy:** a Kconfig choice (see D2): LAN subnets (default), an
  allow-list, or local only. The host app can narrow it further through the
  D7 hook. Every open and close is published as an event naming the target.
- **Cost:** one TLS session for the WebSocket, plus two fixed buffers
  (`TEDGE_REMOTE_ACCESS_BUF_SIZE`, about 2–4 KB each), plus one bridge
  thread. The session count is capped (`TEDGE_REMOTE_ACCESS_MAX_SESSIONS`,
  default 1). Spike F measures all of this.
- **Spike F** (U11, U12):
  - a WSS client (Zephyr websocket over a TLS socket) to Cumulocity's
    remote-access device endpoint, triggered by `c8y_RemoteAccessConnect`
    received through the Spike A MQTT loop;
  - bridged to `ssh` on a Raspberry Pi on the same LAN;
  - measured: interactive SSH latency, `scp` throughput for a 10 MB file, heap
    per session, and behaviour when either end drops;
  - a second target on the device itself (Zephyr `shell_telnet` on loopback)
    proves the local case.
- *Alternative: local targets only (the MCU's own services).* Rejected as the
  default: forwarding to neighbouring hosts is the feature users want, and
  thin-edge.io's own plugin connects to any host it is given. Local-only
  remains a Kconfig choice for cautious deployments.

## Risks / Trade-offs

- [The tenant lacks `mqtt-service.smartrest`, or the preview changes] → Spike
  A also runs against Core MQTT 8883. The endpoint is a Kconfig choice (D2),
  so the fallback costs one option, not a redesign.
- [16 KB TLS input buffers are unavoidable] → Budget about 50 KB per session.
  Allow at most one concurrent HTTPS session next to MQTT, or one
  remote-access session. Remote access
  becomes C6/S3-only, and the WROOM gets telemetry and restart at most.
- [Private key in RAM] → Accept for Phase 3 exploration and document it. If
  needed, raise an upstream issue or PR for opaque PSA keys in
  `tls_credentials`.
- [Unencrypted BLE exposes the one-time password (future provisioner
  integration)] → The enrollment race window is limited by the poll interval.
  `APP_WIFI_PROV_REQUIRE_AUTH` proves physical presence. The captive-portal
  spike (E) is the alternative if WPA2 on the link is wanted.
- [Clock not set yet at TLS time] → SNTP must succeed before the connect. A
  failure is reported on the LED and retried; the device never connects with
  certificate validation turned off.
- [Task-watchdog channels exhausted] → The module does not own the task
  watchdog. It offers a progress hook that the host app wires to its own
  watchdog (D7). In this repo that means raising `CONFIG_TASK_WDT_CHANNELS` in
  the apps, which is noted for `c8y-direct-core`.
- [A dependency on `lib/` creeps into the module and makes extraction hard] →
  `samples/minimal` builds without `lib/`, and a CI grep rejects `lib/` and
  `apps/` paths under `tedge-zephyr/` (D8).
- [The integration hooks are guessed wrong before a real integration exists] →
  The spike only outlines the header. `c8y-direct-core` fixes the API after
  wiring one protocol app and the minimal sample through it.
- [The MCU becomes a pivot into the customer's LAN] → The remote-access
  targets policy is a Kconfig choice. The default covers only the device's own
  subnets, and every session is logged as an event with its target (D9). Only
  CA-authenticated devices can build the feature, and Cumulocity's remote-access
  permissions gate who can open a session.
- [A tunnel starves the host application] → One session by default. The
  bridge thread runs below the host app's priority, with fixed buffers from
  the module's heap. The session closes on WebSocket or TCP error, or after a
  Kconfig idle timeout.
- [A spike result invalidates the roadmap order] → That is the purpose of the
  spikes. The results section records the decision and `SCOPE.md` is updated.

## Open problems

Problems found or deferred during the spikes, to be picked up later. Each one
needs an owner in `c8y-direct-core` (or its own change) before this change is
archived.

| # | Problem | Why it matters | Where it came from | Next step |
|---|---|---|---|---|
| P1 | **Reconnect after losing the network is unmeasured** (task 3.8 deferred). Nobody has measured how long the device takes to get back to "connected" after the Wi-Fi or the path drops silently, or whether TLS heap and TCP contexts return to baseline. | The real client must recover on its own, without leaking memory or connection contexts. The context exhaustion seen in the cycle test hints at the risk. | Deferred 2026-09-19: dropping Wi-Fi at the access point needs someone at the AP. | Run with the AP switched off, or the C6's MAC (`e8:f6:0a:fc:32:0c`) blocked, three times for about 60 s, with `tools/console.py`. Or build a repeatable test into the real client's test plan (for example a Wi-Fi disconnect triggered from the shell, plus a real AP drop). |
| P2 | **The C6 hangs in MCUboot after a CPU reset** (`sys_reboot()`) with Wi-Fi running. `lib/common/net.c`'s last-resort reboot still uses it. | On the C6, the existing apps' recovery reboot may leave the device hung instead of recovering it. tedge-zephyr's restart needs its own full-system reset on Espressif parts. | Spike A, task 3.3. | Fix `net.c` to reset the whole system, as `boot_request_reboot()` does, in a separate change; give tedge-zephyr a platform reset hook. |
| P3 | **8 KB TLS buffers leave about 2 KB of margin** over today's 5.9 KB server certificate chain on 9883. | A longer chain after a server certificate rotation would break 8 KB builds in the field. | Spike A, task 3.6. | Default to 16 KB; document 8 KB as an opt-in saving for the MQTT Service; consider failing over to a 16 KB session if the handshake fails with `-0x87`. |
| P4 | **Free-form telemetry is published but not yet seen in the cloud.** | Telemetry over the MQTT Service needs a cloud-side consumer (for example the Dynamic Mapper). | Section 1 and Spike A. | Task 7.4. |

## Spike results

_To be filled in as each spike completes: measurements, go/no-go, and the
decision each unknown (U1–U12) produced._

### Spike A: TLS and MQTT from the device (section 3, 2026-09-19)

**Setup:** `apps/c8y-spike` with `overlay-spike-a.conf`, TLS 1.2 with the
`ECDHE-RSA-AES128-GCM-SHA256` and `ECDHE-ECDSA-AES128-GCM-SHA256`
ciphersuites (on Zephyr 4.4 / TF-PSA-Crypto the ciphersuite options pull in
the PSA algorithms; the older `KEY_EXCHANGE_*` options alone are dropped),
mbedTLS on its own heap so TLS memory is measured directly, a dedicated
10 KB thread. Identity: `tedge-spike-c6`, mutual TLS with a certificate
from the Cumulocity CA issued on a PC.

**U1: cost.**

| | ESP32-C6 (RISC-V, 160 MHz) | ESP32-S3-DevKitC-1 (Xtensa, 240 MHz) |
|---|---|---|
| TCP + TLS handshake, 9883, 10 cycles | 2,640–2,807 ms (median ~2.7 s) | 1,830–2,191 ms (median ~1.9 s) |
| CONNACK after handshake | 40–240 ms | 70–340 ms |
| TLS heap, handshake peak (16 KB buffers) | 51,788 B | 51,788 B |
| TLS heap, connected (16 KB buffers) | 34,836 B | 34,836 B |
| TLS heap after disconnect | 0 B, every cycle (no leak) | — |
| Client thread stack, peak | 2,604–2,828 B of 10,240 | 2,928 B |
| Signed image (slot 1280 KB / 3072 KB) | 875,771 B (68%) | 725,388 B |

- Static cost of Spike A on the C6, against the same app without it:
  text +133.6 KB; static RAM +142.6 KB, of which 96 KB is the deliberately
  oversized measuring heap for mbedTLS. The real TLS need is the peak above,
  so a production heap of about 56 KB (16 KB buffers) or 40 KB (8 KB buffers)
  per session plus margin is the figure to use.
- The handshake time is almost all public-key work in software: verifying
  the RSA-4096 intermediate and the RSA-2048 server certificate, ECDHE, and
  an ECDSA signature. It scales with the CPU clock (C6 vs S3). Zephyr's
  mbedTLS does not use the ESP32 RSA/SHA accelerators.
- SNTP (`pool.ntp.org`) set the clock in 51 ms on the first attempt;
  certificates validate against it.

**U2: record size (task 3.6).**

- Below 16 KB, Zephyr offers max-fragment-length automatically, and
  Cumulocity honours it: records are at most 4096 bytes (level-3 mbedTLS
  debug: `found max_fragment_length extension`, input records of 4096).
- **But mbedTLS reassembles a handshake message inside its input buffer**,
  and it needs room for the partial message plus the next record. With a
  5,883-byte certificate chain on 9883:
  - 4096: fails, `requesting more data than fits` (`-0x87`);
  - 6144: fails the same way;
  - **8192: works**. Heap peak 35,404 B and connected 18,452 B, **16,384 B
    less than with 16 KB buffers**. The handshake time is unchanged. A
    6,223-byte downlink message crossed several records correctly.
- 8883 needs 16 KB: its CertificateRequest alone is 12,919 bytes.
- Margin: about 2 KB over today's chain. A longer server chain after a
  certificate rotation could break 8 KB buffers, so the production default
  should be 16 KB, with 8 KB as a documented saving for the MQTT Service.

**U3/U4: the device-management contract on the device.**

- 9883 with mutual TLS: all five subscriptions granted (one filter per
  SUBSCRIBE), `100`/`114`/`117` applied, JWT on `s/dat` (768 bytes), and
  free-form telemetry published
  (`{"temperature":19.18,"humidity":53.19,"pressure":1016.83}` on
  `te/device/tedge-spike-c6///m/environment`).
- 8883 with mutual TLS: the same, with SmartREST `200` telemetry. Handshake
  2,915–3,174 ms on the C6, about 0.3 s slower than 9883 (the 12.9 KB CA
  list). Same heap with 16 KB buffers.
- 8883 with basic auth (`tedge-spike-a-c6`): connects in 2,625 ms (no client
  signature). Heap 51,356 / 34,380 B, about 430 B less than mutual TLS. The
  JWT is refused
  (`41,,MqttAuth: Cannot publish token for device that do not use certificate authentication`),
  as on the PC.

**Restart (task 3.3).**

- `510` → `501` → marker in settings → reboot → reconnect → `503`: the
  operation went to SUCCESSFUL 32 s after it was created. The marker survives
  a reboot: an operation left EXECUTING by a hung reboot was completed on the
  next boot.
- **The reboot must be a full-system reset on the C6.** After `sys_reboot()`'s
  CPU reset (`rst:0xc SW_CPU`) with only Wi-Fi running, MCUboot hung after
  "SPI Flash Size : 4MB" until the next reset. `boot_request_reboot()`
  (`esp_rom_software_reset_system()`, `rst:0x3 LP_SW_HPSYS`) works. This
  matters beyond the spike:
  - tedge-zephyr's restart needs its own full-system reset on Espressif
    parts, since it can't use `lib/common`;
  - `lib/common/net.c`'s last-resort reboot still calls
    `sys_reboot(SYS_REBOOT_COLD)`, so on the C6 it would likely leave the
    device hung in MCUboot. That's a separate fix.

**Integration requirements found.**

- **TCP connection contexts:** reconnecting once a second ran out after three
  cycles (`Not enough connection contexts`, board `NET_MAX_CONN=6`), because
  a closed TLS connection holds its context until TCP finishes closing.
  Spike A uses `NET_MAX_CONN=10` / `NET_MAX_CONTEXTS=12` with 3 s between
  cycles, and 10/10 then connect. tedge-zephyr's README must list the extra
  contexts it needs, and the client must back off between reconnects.
- One transient DNS failure (`-101`) and occasional first-boot Wi-Fi
  association retries were seen; the retry loop covered both.

### Section 1: tenant and host-side findings (2026-09-19)

**Tenant:** `tedge-dev05.preprod.c8y.io` (tenant `t297258657`).

- Features `certificate-authority` (GA) and `mqtt-service.smartrest`
  (Public Preview) are on. The tenant CA certificate exists (valid until 2029,
  auto-registration on).
- The tenant is subscribed to the `cloud-remote-access` and `mqtt-service`
  microservices. No Dynamic Mapper is subscribed.
- Credentials: `c8y.local.conf` holds go-c8y-cli environment variables, not
  a Kconfig overlay. Firmware overlays therefore use other `*.local.conf`
  names (all git-ignored). The Spike A device credentials are in
  `spike-a-device.local.conf`.
- The bootstrap credentials (`management/devicebootstrap`) work against the
  tenant: `POST /devicecontrol/deviceCredentials` returns 404 for an unknown
  ID. go-c8y-cli does not use them unless `C8Y_BOOTSTRAP_*` are set; with the
  session token the same request returns 403.

**TLS (U1, U2):**

- Every port uses the same chain: `*.preprod.c8y.io` → GoDaddy DV R1v1 →
  GoDaddy Root R1 → Go Daddy Root G2. All certificates are RSA (2048/4096).
  TLS 1.3 and TLS 1.2 (ECDHE-RSA, AES-GCM) both work.
- **All ports accept max-fragment-length.** The server echoes the extension
  (id 1) on TLS 1.2 when the client offers 4096. Whether mbedTLS then
  reassembles handshake messages that span several records is checked on the
  device in 3.6.
- Largest handshake messages (TLS 1.2):

  | Port | Certificate | CertificateRequest | NewSessionTicket |
  |---|---|---|---|
  | 9883 | 5,883 B | 44 B (no CA list) | 6,153 B |
  | 8883 | 5,883 B | **12,919 B (166 acceptable client CAs)** | 6,167 B |

  NewSessionTicket is only sent if the client offers tickets. With tickets
  off, 9883 needs roughly an 8 KB input buffer (or MFL). Core MQTT's
  CertificateRequest grows with the platform's trusted certificates, so
  8883 is the riskier endpoint for a fixed MCU buffer.

**MQTT and SmartREST (U3, U4), checked from a PC with
`apps/c8y-spike/tools/c8y_mqtt_probe.py`:**

| | Core MQTT 8883 | MQTT Service 9883 |
|---|---|---|
| Basic auth (`<tenant>/device_<id>`) | connects | **Not authorized**. Tried 4 user-name formats and the MQTT Service roles; also refused for a device freshly created with `register-basic` and never used with a certificate, which connects to 8883 seconds later |
| mTLS (Cumulocity CA certificate) | connects | connects |
| Publish on `s/us` (100/114) | applied | applied (114 changed `c8y_SupportedOperations`) |
| Subscribe `s/ds`, `s/e`, `s/dat` | granted; `510` and `40,999,…` arrive | granted **only with one filter per SUBSCRIBE**. A SUBSCRIBE carrying several filters is refused as a whole (SUBACK 0x80 on 3.1.1, "Topic filter invalid" on MQTT 5), whatever the filters are. Sent one at a time, every filter is granted, including wildcards, `s/ucr` and `te/device/<id>///cmd/+/+`. Then `510` and `40,999,…` arrive |
| `s/uat` → `s/dat` JWT | certificate devices only (771-byte JWT, 1 h). A basic-auth device gets `41,,MqttAuth: Cannot publish token for device that do not use certificate authentication` | works (certificate device) |
| Persistent session (clean session off) | not tested | no CONNACK |

- **Firmware rule: send one topic filter per SUBSCRIBE.** It costs nothing
  on 8883 and is required on 9883. The multi-filter packet alone is the
  trigger. The exact set tedge's bridge subscribes to (`s/dat`, `s/dt`,
  `s/ds`, `s/e`, `devicecontrol/notifications`, `error`, from
  `tedge bridge inspect c8y`) is refused as one packet. `s/ucr` and
  `te/…/cmd/#` are each granted on their own. tedge works on 9883 because
  its bridge subscribes one topic at a time
  (`crates/extensions/tedge_mqtt_bridge/src/lib.rs`, `start_subscribe_round`).
  The firmware subscribes to the same downstream set as tedge, minus `s/ucr`,
  which only the bootstrap flow needs.
- With certificate authentication, 9883 carries the whole device-management
  contract: operations (`510` → `501`/`503`), errors on `s/e` and the JWT.
  The MQTT Service remains the default endpoint.
- **Basic authentication only works on 8883.** Both auth methods stay
  selectable in Kconfig, and the endpoint follows the choice:
  `TEDGE_C8Y_MQTT_SERVICE` depends on `TEDGE_AUTH_C8Y_CA`, so a basic-auth
  (bootstrap) build falls back to Core MQTT automatically. The constraint is
  one line, to be dropped if the MQTT Service accepts basic auth for devices.
  Covered by the `basic-auth-uses-core-mqtt` and `mqtt-service-needs-ca`
  Kconfig cases.
- **Every multi-filter SUBSCRIBE is refused, from two filters up.** The
  same happens for two plain free-form topics (`spike/a` + `spike/b`) and
  for `s/ds` + `s/e` in either order. Growing tedge's set from one filter:
  1 is granted, 2 or more are refused.
- Free-form: publishes on `spike/test` and `te/device/<id>///m/environment`
  are accepted (PUBACK). **The MQTT Service isolates clients from each
  other**, so a free-form message is never delivered to another device's
  subscription. A device-to-device test can't show anything, and the
  two-device attempt (`tedge-spike-host2` publishing) was void for that
  reason. Free-form topics are for traffic between a device and the cloud:
  whether device publishes arrive is checked in 7.4 with a cloud-side
  consumer, and cloud-to-device free-form traffic comes from the cloud side.
- Wildcard free-form subscriptions aren't supported by the MQTT Service at
  present (per the tenant owner), **even though the SUBACK grants them**
  (`spike/+`, `spike/#`, `te/…/m/+` were all granted). A granted SUBACK is
  therefore no proof that a subscription works. **Firmware rule: subscribe
  only to exact topics.**
- A basic-auth (bootstrap) device authenticates HTTPS with its own
  credentials, because it can't get a JWT. A certificate device must get a
  JWT over MQTT (`s/uat`) for HTTPS. That answers U4 for 8883.

**Cumulocity CA enrollment (U7, host-side rehearsal):**

- `c8y deviceregistration register-ca` with a 32-character one-time password,
  then `POST /.well-known/est/simpleenroll` (Basic `<id>:<otp>`, PKCS#10 body
  without the armour lines), returns `200`.
- **The response is base64 PKCS#7** (`application/pkcs7-mime;
  smime-type=certs-only`), not PEM. The firmware must unwrap it
  (mbedTLS `MBEDTLS_PKCS7_C`, or pull out the single certificate).
- The issued certificate: `CN=<external id>`, issuer `O=<tenant domain>,
  CN=<tenant id>`, P-256, **valid for 1 year**, 330 bytes as DER, and no chain.
  It connected with mTLS to both 8883 and 9883.

**Remote access target (for Spike F):**

- The Pi `rpi5-d83add9f145a` is at 192.168.68.72/22 on `wlan0`. `sshd`
  already listens on `0.0.0.0:22` and is reachable from the LAN.
- The endpoints are on `tedge-spike-a-c6` (managed object 26211202) for now:
  `pi-ssh` (PASSTHROUGH to 192.168.68.72:22; the local ssh client
  authenticates, and no Pi credentials are stored in Cumulocity) and
  `device-shell` (TELNET to 127.0.0.1:23). The CA-enrolled device needs the
  same endpoints once it exists (Spike C).

**Identities created on the tenant:**

- `tedge-spike-c6`: CA certificate issued from the PC, managed object
  20211235, the C6's identity for Spike A (and F). Remote-access endpoints
  `pi-ssh` and `device-shell` are configured on it.

- `tedge-spike-a-c6`: basic auth, managed object 26211202, for Spike A.
- `tedge-spike-host`: CA certificate, managed object 20211208, a host-only
  probe identity.
- `tedge-spike-basic-9883`: basic auth, created only to reproduce the 9883
  basic-auth refusal on a fresh device.
- `tedge-spike-host2`: CA certificate, a second host identity. It was
  created for a device-to-device delivery test, which client isolation makes
  void.
