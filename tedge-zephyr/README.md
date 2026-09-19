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

> **Status: skeleton.** This is the Kconfig menu, a public header outline
> (`include/tedge/tedge.h`, marked unstable) and a minimal sample. No feature
> logic exists yet; see `openspec/changes/archive/2026-09-19-c8y-direct-spikes/` in the incubating
> repository for the plan.

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

The requirements on your configuration (socket counts, poll slots, mbedTLS
options) are filled in as features are implemented.

## Layout

```
tedge-zephyr/
├── zephyr/module.yml         module name: tedge
├── CMakeLists.txt, Kconfig, VERSION
├── include/tedge/            public headers
├── src/                      implementation
├── profiles/                 feature-set overlays (minimal, full)
├── samples/minimal/          builds with Zephyr and this module only
├── tests/                    Kconfig dependency checks
└── scripts/                  checks (independence from the host repository)
```

This directory is self-contained. Nothing in it may reference files outside
it; `scripts/check-independence.py` enforces that, so the directory can move
into its own repository unchanged.

## Version

`VERSION` holds the module's version. It is independent of any application's
version and is returned by `tedge_version()`.
