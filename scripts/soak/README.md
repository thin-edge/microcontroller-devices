# Soak harness

Host-side scripts to run long, comparable reachability experiments against a
device. They were written for the ESP32 network-freeze investigation
(`openspec/changes/esp32-network-freeze-investigation/`), whose `evidence.md`
records every run. They run on macOS with the esptool venv (`/tmp/flashenv` or
`~/flashenv`, which has pyserial). Set `SOAK_PYTHON` to use another
interpreter.

| Script | What it does |
|---|---|
| `build.sh DIR APP BOARD [CONF...]` | Builds an app in the `zephyr-dev` container, always adding the Wi-Fi credentials overlay. Extra confs are paths relative to the repo root. |
| `flash.sh DIR PORT` | Flashes a classic ESP32 (`esp32_devkitc` target) at `0x1000`. |
| `run.sh ...` | Runs one experiment: console logger plus poller, then writes the summary. |
| `console_log.py` | Opens the serial port once and logs each line with a host timestamp. |
| `poll.py` | Probes the device every 5 s, detects outages and writes the summary. |
| `remote.sh CMD ...` | Flashes and runs soaks on another host that has the boards attached (see below). |

## Boards on another host

When the boards hang off another Linux machine (for example the gateway Pi),
`remote.sh` flashes them and runs the harness there, detached, so a run
survives the laptop going away. Builds still happen locally.

```sh
export SOAK_HOST=root@rpi5-d83add9f145a.local
scripts/soak/remote.sh setup                                   # once: venv + scripts
scripts/soak/remote.sh flash build_x /dev/serial/by-path/<port>   # add esp32s3 for the S3
scripts/soak/remote.sh run --board esp32-cam --app snmp --host 192.168.68.74 \
    --port /dev/serial/by-path/<port> --variant accept --duration 86400
scripts/soak/remote.sh status
scripts/soak/remote.sh pull                                    # copy records here
```

On the remote host:
- Records live under `/opt/soak/runs`.
- The run is stamped with the git SHA of this checkout (`run.sh --sha`).
- Without net-snmp installed, `poll.py` records traps with its built-in
  listener.

Use `/dev/serial/by-path/...` names for the ports: two CP2102 bridges with the
same serial number collide under `/dev/serial/by-id/`.

## A run

```sh
scripts/soak/run.sh --board esp32-cam --app snmp --host 192.168.68.74 \
    --port /dev/cu.usbserial-210 --variant baseline --duration 3600
```

The console is opened first. On the ESP32-CAM this resets the board, so the run
starts at boot; it is never re-opened mid-run. Leave out `--port` for a board
whose console you can't reach.

Each run writes these files to `scripts/soak/runs/` (git-ignored), under the
stem `<stamp>-<board>-<app>-<variant>`:

- `.console.log`: the console, each line prefixed with host time (ISO 8601).
- `.polls.log`: one line per probe: `OK`/`FAIL`, detail, and the ping result.
  `#####` lines mark outages, recoveries and reboots.
- `.traps.log` (SNMP): the traps `snmptrapd` received.
- `.summary.json`: the run's result (see below).

## Probes

| `--app` | Probe | Counts as serving |
|---|---|---|
| `snmp` | SNMPv2c GET `sysUpTime.0` over UDP 161 | a matching response. Its uptime also reveals reboots. |
| `modbus` | TCP connect to 502 and a read of holding register 0 | any well-formed Modbus reply, including an exception |
| `opcua` | TCP connect to 4840 and a `HEL` message | an `ACK` or `ERR` reply |
| `mqtt` | watches a collector's MQTT topic for the device (`-- --mqtt-topic 'te/device/<name>///m/+'`), using `mosquitto_sub` | a message within `--mqtt-max-age` (30 s) |

Every probe is followed by one ICMP ping, which is logged but does not decide
outages. A device that answers ping but not its protocol is itself a finding.

The Modbus server serves one client at a time. If a collector such as
tedge-dot holds that connection, a direct `modbus` probe queues behind it; use
`--app mqtt` against the collector's topic instead.

## Traps

For `--app snmp`, `poll.py` runs `snmptrapd` unprivileged on UDP 1162. Build
the firmware to send there with a local overlay, for example
`soak-trap.local.conf` (git-ignored through `*.local.conf`):

```
CONFIG_APP_SNMP_TRAP_MANAGER="192.168.68.51"   # this Mac's IP
CONFIG_APP_SNMP_TRAP_PORT=1162
```

Use a literal IP: the firmware can't resolve `.local` trap managers. Pass
`-- --no-traps` to skip the listener.

## Outages and the summary

- **Outage:** 3 consecutive failed protocol probes. It starts at the first
  failure and ends at the next success. Failures before the first successful
  probe are boot time, not an outage; a device that never answers is reported
  as `"never_reachable": true`.
- **Recovery window:** liveness timeout + last-resort reboot timeout + 120 s
  (450 s by default; adjust with `--liveness-s`, `--last-resort-s` and
  `--rejoin-s`). If `--duration` runs out during an outage, recording
  continues until the device recovers or the window has passed. An outage
  longer than the window is **unrecovered** (`"recovered": false`).
- **Per outage:** time to failure since run start and since the last recovery,
  the device uptime before and after (SNMP), and whether it rebooted.
- **Console counters:** boots, `LIVENESS RESET` records, hardware watchdog
  resets, `HEALTH` and `STALE` lines, last-resort reboots, and
  `Connectivity lost` events.
- **Metadata:** board, git SHA (with `-dirty` if `lib/` or `apps/` have local
  changes), variant, planned duration and serial port.

`--stop-after-outages N` ends the run once N outages have finished, which is
useful for collecting a time-to-failure distribution.
