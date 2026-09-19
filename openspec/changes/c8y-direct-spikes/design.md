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
    TEDGE_C8Y_MQTT_SERVICE            :9883, SmartREST + free-form (default)
    TEDGE_C8Y_CORE_MQTT               :8883, SmartREST only (GA fallback)
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
- **Authentication:** Spike A uses a device user with basic authentication
  (created on the test tenant). mTLS comes from Spike C.
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

## Spike results

_To be filled in as each spike completes: measurements, go/no-go, and the
decision each unknown (U1–U12) produced._
