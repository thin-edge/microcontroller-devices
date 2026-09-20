# tedge-zephyr

A [thin-edge.io](https://thin-edge.io) device-management client for
[Zephyr RTOS](https://www.zephyrproject.org/) applications. Add it to your own
Zephyr application to get device management next to whatever the application
already does:

- remote access, including to other hosts on the device's network;
- firmware update through MCUboot;
- telemetry;
- log retrieval;
- configuration management.

> **Status: early.** Onboarding, the connection, device state, restart and
> remote access work and are verified on hardware (ESP32-C6, ESP32-S3).
> Firmware update, telemetry, logs, configuration and certificate renewal are
> not implemented yet: their API calls return `-ENOTSUP`, and the header says
> which change implements each. The API may still change.

## Transports

- **Direct to Cumulocity** (first): MQTTS to the Cumulocity MQTT Service
  (:9883), with Core MQTT (:8883) as a fallback. Onboarding uses the Cumulocity
  CA, with bootstrap-user basic-auth credentials as the alternative.
- **Via a thin-edge.io gateway** (later): the device becomes a child device
  that speaks the thin-edge.io MQTT API on the local network.

## Using it in an application

The module is named `tedge`. Until it has its own repository, add it as an
extra module before `find_package(Zephyr)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES /path/to/tedge-zephyr)
```

Then enable it in `prj.conf`:

```
CONFIG_TEDGE=y
```

With `CONFIG_TEDGE=n` (the default), the module adds nothing to the image.

Every feature is a separate Kconfig option under `CONFIG_TEDGE`, so an image
carries only what the board can afford. Run `west build -t menuconfig` and see
"thin-edge.io device management client". Options for features that aren't
implemented yet are shown but cannot be selected. `CONFIG_TEDGE_EXPERIMENTAL_FEATURES`
unlocks them for development and spikes only.

## Integration contract

The module is a guest in your image.

| Concern | Your application | tedge-zephyr |
|---|---|---|
| Network | Brings up and recovers Wi-Fi or Ethernet | Waits for an IPv4 address (net_mgmt events); never touches the interface |
| Identity | Sets the external ID, name, type and firmware name/version | Defaults: external ID `<prefix>-<MAC>`; firmware from the app's version |
| Telemetry | Calls `tedge_publish_measurement()`, `tedge_publish_event()` and the alarm functions | Buffers (bounded), encodes and sends; never samples on its own |
| Operations | May register its own operations, log types and configuration types | Handles its compiled-in features; passes custom ones to the application |
| Restart | Restart hook: prepare, or veto with a reason | Reboots only after the hook allows it |
| Firmware confirm | Adds its own health checks | Confirms a new image only when the cloud is reachable **and** your checks pass |
| Remote access | May narrow the allowed targets | Enforces the Kconfig target policy before opening any connection |
| State | Receives state changes (for example to drive an LED) | Emits states; never drives GPIO |
| Watchdog | Owns the task watchdog | Calls an optional progress hook from each thread it owns |
| Resources | Provides the socket, poll and mbedTLS configuration listed below | Runs its own threads with its own bounded heap; settings under `tedge/`; TLS credential tags from `CONFIG_TEDGE_TLS_TAG_BASE` |

### What your application must configure

The client selects the protocol pieces it needs (MQTT, sockets, TLS sockets,
credentials, SNTP, settings). These are yours to size, because they are
shared with the rest of your image:

| Option | Why, and what to set |
|---|---|
| `CONFIG_MBEDTLS_HEAP_SIZE` | The TLS memory for **all** TLS in your image. One session needs **52 KB** while it shakes hands and holds **35 KB**; every extra concurrent session (a remote-access tunnel, a download) needs another 52 KB peak. 64 KB for the connection alone, **96 KB** with remote access. |
| `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` | 16384. 8192 works on the MQTT Service and saves 16 KB per session, but leaves only ~2 KB of margin over today's server chain. |
| `CONFIG_NET_SOCKETS_TLS_MAX_CONTEXTS` | One for the client, plus one per remote-access session. |
| `CONFIG_NET_MAX_CONN`, `CONFIG_NET_MAX_CONTEXTS` | About 4 more than your application needs: a closed TLS connection holds its TCP context while it finishes closing. |
| `CONFIG_SECURE_STORAGE_ITS_STORE_IMPLEMENTATION_*` | The device key lives in PSA ITS. It is a Kconfig choice, so the module cannot pick it; `..._SETTINGS` works with an NVS settings backend. |
| mbedTLS ciphersuites and `CONFIG_PSA_WANT_ECC_SECP_R1_256` | The TLS 1.2 baseline Cumulocity needs; see `profiles/`. |

`profiles/minimal.conf`, `profiles/full.conf` and
`profiles/remote-access-enabler.conf` set all of this for you.

### Measured cost (ESP32-C6, Modbus application, 2026-09-20)

| Build | Flash (text) | libc heap left |
|---|---|---|
| Application alone | 616 KB | 296 KB |
| + client (onboarding, connection, restart) | 775 KB | 180 KB |
| + remote access | 798 KB | 130 KB |

With the TLS heap in PSRAM on an ESP32-S3, the client costs no internal RAM
beyond its own buffers.

### Security limitations

- The device key is stored in PSA ITS, encrypted with a key derived from the
  device ID, and is exported into RAM while the TLS credential is registered.
  It is obfuscated, not protected: use flash encryption for a real
  deployment.
- Remote access reaches whatever the target policy allows. Keep the default
  (the device's own subnets) or an allow-list, and narrow it further from the
  application.

## Layout

```
tedge-zephyr/
├── zephyr/module.yml         module name: tedge
├── CMakeLists.txt, Kconfig, VERSION
├── include/tedge/            public headers
├── src/                      implementation
├── profiles/                 feature-set overlays (minimal, full)
├── samples/minimal/          builds with Zephyr and this module only
├── tests/                    unit tests (native_sim) and Kconfig checks
└── scripts/                  checks (self-containment, no logged secrets)
```

This directory is self-contained. Nothing in it may reference files outside
it; `scripts/check-independence.py` enforces that, so the directory can move
into its own repository unchanged.

## Version

`VERSION` holds the module's version. It is independent of any application's
version and is returned by `tedge_version()`.
