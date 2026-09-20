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

> **Status: early.** Onboarding, the connection, device state, restart,
> remote access, firmware update, certificate renewal and telemetry (with the
> client's own health) work and are verified on hardware (ESP32-C6,
> ESP32-S3), as do log retrieval, crash dumps and the allow-listed shell
> command. Configuration management is not implemented yet: its API calls
> return `-ENOTSUP`, and the header says which change implements it. The API
> may still change.

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
| + firmware update, certificate renewal | 812 KB | 116 KB |
| + telemetry and health | 815 KB | 114 KB |
| + log upload and crash dumps | 824 KB | 100 KB |
| + the shell command (on an image that already has a shell) | +2.5 KB | 90 KB |

With the TLS heap in PSRAM on an ESP32-S3, the client costs no internal RAM
beyond its own buffers.

### What the client publishes, and how to map it

On the Cumulocity MQTT Service the client uses thin-edge.io's `te/` topics,
so the same messages suit a gateway or a Cumulocity Smart Function. On Core
MQTT the same values go out as direct inventory updates instead.

| Topic | Payload | Meaning |
|---|---|---|
| `te/device/<id>///twin/tedge_Agent` | `{"name":"tedge-zephyr","version":"0.0.1","transport":"c8y-mqtt-service","firmware":"<name> <version>"}` | which client and firmware the device runs |
| `te/device/<id>///twin/tedge_RemoteAccess` | `{"maxSessions":1,"activeSessions":0,"freeSessions":1,"policy":"lan","sessions":[]}` | remote-access capacity, published on every connect and whenever a session opens or ends |
| `te/device/<id>/service/tedge-zephyr/status/health` | `{"status":"up","time":<unix seconds>}` | the client is connected |

Twin values are **state, not events**: the client republishes all of them
after every reconnect, so a reboot never leaves a stale value, and nothing
relies on retained messages.

A Smart Function maps a twin message to a fragment on the managed object.
The mapping verified on the test tenant takes the `tedge_RemoteAccess`
message above and writes it unchanged under a `remoteAccess` fragment:

```json
{"remoteAccess": {"activeSessions": 0, "freeSessions": 1,
                  "maxSessions": 1, "policy": "lan", "sessions": []}}
```

Operations stay on SmartREST (`s/ds`, `501`/`503`/`502`), which is how
Cumulocity tracks an operation's lifecycle.

### Telemetry

`CONFIG_TEDGE_TELEMETRY` gives the application four calls, safe from any
thread and none of them blocking on the network:

```c
struct tedge_measurement_value v[] = {
        { .series = "speed", .value = 1480, .unit = "rpm" },
        { .series = "pressure", .value = 4.2, .unit = "bar" },
};

tedge_publish_measurement("pump", v, ARRAY_SIZE(v), 0); /* 0 = now */
tedge_publish_event("maintenance_due", "500 hours since service", 0);
tedge_raise_alarm("pump_stalled", TEDGE_ALARM_MAJOR, "no rotation");
tedge_clear_alarm("pump_stalled");
```

**The client samples nothing by itself.** What is worth measuring, and how
often, is the application's decision — it owns the sensors. The client's job
starts once the call is made.

A call builds its message there and then, with the time it was made, and
puts it in a buffer; the client thread sends it while connected. That is why
a reading buffered through an outage is recorded at the time it was taken,
not the time the device reconnected. An application that sampled earlier
passes its own Unix time in ms as the last argument.

| Kind | Topic (MQTT Service) | Payload | Core MQTT |
|---|---|---|---|
| Measurement | `te/device/<id>///m/<type>` | `{"time":"…","speed":1480.00,"pressure":4.20}` | `200,<type>,<series>,<value>` per series |
| Event | `te/device/<id>///e/<type>` | `{"time":"…","text":"…"}` | `400,<type>,"<text>"` |
| Alarm | `te/device/<id>///a/<type>` | `{"time":"…","severity":"major","text":"…"}` | `301`–`304` by severity |
| Clear an alarm | `te/device/<id>///a/<type>` | empty payload | `306,<type>` |

Measurements go at QoS 0 and events and alarms at QoS 1: a lost reading is
one reading, a lost alarm is an unreported fault. Values carry two decimals
(the client formats them without floating-point printf, which an application
should not have to pay for).

Two differences belong to Cumulocity rather than the client. On Core MQTT a
measurement with several series becomes one measurement object per series,
because the static template carries one; on the MQTT Service the whole
message arrives as one. And raising an alarm whose type is already active
updates that alarm instead of creating a second one, keeping the severity it
was first raised with. `te/` messages also need a Smart Function in the
tenant before they show up as measurements; SmartREST on Core MQTT needs
none.

**What the buffer can and cannot do.** It is
`CONFIG_TEDGE_TELEMETRY_BUFFER_BYTES` (default 2048) of RAM in the client's
heap — minutes of a typical reporting interval, not hours, and it does not
survive a reboot. When it is full the oldest *measurement* is dropped and
counted; events and alarms are never dropped to make room for a measurement.
`tedge_telemetry_dropped()` returns the count, and the client publishes it in
its own health, so loss is a number rather than a mystery. An application
that must lose nothing should keep its own store and publish from it.

With `CONFIG_TEDGE_HEALTH` the client also reports itself every
`CONFIG_TEDGE_HEALTH_INTERVAL_S` (default 900) as a `tedge_health`
measurement: uptime, free bytes in its heap, messages dropped, and the reason
for the last reset. That is the *client's* health; anything the application
knows about the device — signal strength, battery, sensor status — is the
application's to publish, because only it knows what those numbers mean.

Without the feature the same calls compile and return `-ENOTSUP`, so an
application does not need `#ifdef`s to build in a minimal configuration.

### Diagnostics: logs, a crash dump and a command

`CONFIG_TEDGE_LOG_UPLOAD` answers Cumulocity's request for a log file. The
client always has one log of its own — the most recent lines of everything
the image logs, kept in a RAM ring of `CONFIG_TEDGE_LOG_RING_BYTES` (2 KB by
default) — so a device says something useful even if its application never
built a log. An application adds its own:

```c
static int status_log(const struct tedge_log_request *req,
                      tedge_write_fn write, void *ctx, void *user_data)
{
        char line[128];
        int n = snprintf(line, sizeof(line), "pump %s\n", pump_state());

        return write(ctx, line, (size_t)n);  /* pass any error back */
}

tedge_register_log_type("app-status", status_log, NULL);
```

A log type is a callback, not a file: nothing is stored, nothing needs a
filesystem, and no log is ever held whole in RAM. The client applies what it
can of the request's filters (`search_text`, `max_lines`; the date range is
passed through for readers that keep wall-clock timestamps — its own ring
does not). **Your reader may be called twice for one request**, once to
measure the log and once to send it, so produce the same lines both times.

One request sends at most `CONFIG_TEDGE_LOG_UPLOAD_MAX_BYTES` (8 KB by
default); a longer log is cut with a line saying so, rather than refused.
The upload runs on its own thread and leaves the connection working.

**Crash dumps.** `CONFIG_TEDGE_COREDUMP` offers the last crash as the log
type `coredump`, and it appears in the device's supported logs only when a
dump is actually stored. Download it and read it with Zephyr's own tooling:

```sh
python3 $ZEPHYR_BASE/scripts/coredump/coredump_serial_log_parser.py \
        coredump.log core.bin
# then: coredump_gdbserver.py, and gdb against the ELF of that firmware
```

Two things your application must get right, because the client cannot:

- **Pick the backend and the dump mode.** They are Kconfig choices:
  `CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION=y` and
  `CONFIG_DEBUG_COREDUMP_MEMORY_DUMP_MIN=y`. The default mode dumps all of
  RAM and never fits a small partition.
- **Give it a partition big enough.** With the minimal mode a dump is the
  faulting thread's stack plus its registers, so a 4 KB `coredump_partition`
  holds a crash in a thread with a stack of about 3 KB. A fault in a larger
  thread is reported as `-ENOMEM` by the backend and no dump is kept.

The dump is erased only once the cloud has it, so a failed upload can be
requested again.

**The shell command.** `CONFIG_TEDGE_SHELL_COMMAND` runs `c8y_Command`
against Zephyr's dummy shell backend — in-process, with no listener on the
network — on its own thread, and returns what it printed.

**It refuses everything by default.** `CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST`
is empty until you set it, for example:

```
CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST="kernel uptime,net iface,tedge diag"
```

A request must begin with one of those prefixes and may add arguments.
Anything containing `;`, `|`, `&`, `` ` ``, `$`, `<`, `>` or a newline is
refused whatever the list says, so an allowed prefix cannot become a doorway
to a second command. Compiling the feature in is not consent to run
something: only you know what is safe to expose in your image.

Zephyr cannot interrupt a running command. After
`CONFIG_TEDGE_SHELL_COMMAND_TIMEOUT_S` the operation is reported as failed,
but the thread stays busy until the command returns and further commands are
refused meanwhile — so keep commands that can block for ever off the list.
The result travels in one SmartREST field: newlines become spaces and a long
answer is cut. A command with a lot to say belongs behind a log type.

**One operation at a time.** Cumulocity's operation statuses act on the
oldest operation in each state, not on the one a message names, so the
client executes one operation at a time and queues what arrives meanwhile
(two deep). This is also why an operation interrupted by a reset stays
EXECUTING in Cumulocity: the device cannot see it after the reboot, and it
has to be cleared from the cloud side.

### Firmware update

`CONFIG_TEDGE_FIRMWARE_UPDATE` needs an MCUboot (sysbuild) build with a
second slot — the layout this repository's applications already use.

What happens, and what it costs:

| Step | On an ESP32-C6 |
|---|---|
| Download into the second slot | ~30 s for 890 KB, while MQTT stays up |
| The bootloader swaps | ~40 s (~19 s on an S3) — **the device is offline for this** |
| The new image confirms itself | once it reaches the cloud and your hook agrees |
| Whole operation | about 105 s |

**Your `firmware_confirm_check` hook decides whether an update sticks.**
Check what would make the device useless in the field: that your protocol
server accepted a connection, that a sensor answers, that the peer you
depend on is reachable. Returning non-zero leaves the image unconfirmed, and
it is rolled back.

**An image that never confirms rolls itself back.** It resets the device
after `CONFIG_TEDGE_FIRMWARE_CONFIRM_TIMEOUT_S` (default 900 s) so the
bootloader can restore the previous image; the restored image then reports
the failure to the cloud. This only protects against images that carry the
client: an image that crashes earlier still needs a hardware watchdog.

Progress is published on `te/device/<id>///progress/firmware` at QoS 0:

```json
{"name":"app","version":"1.6.0","phase":"downloading","percent":40,
 "bytes":355866,"total":888011}
```

Phases are `downloading`, `installing`, `done` and `failed` (with a
`reason`). The size comes from a `Range: bytes=0-` request, because
Cumulocity serves binaries chunked and sends no `Content-Length`.

### Certificate renewal

A device whose certificate has expired cannot be reached at all: no
operations, no remote access, no firmware update. `CONFIG_TEDGE_CERT_RENEWAL`
keeps that from happening.

- The client reads the expiry from its own certificate and renews through
  the CA's re-enrollment endpoint once fewer than
  `CONFIG_TEDGE_CERT_RENEW_BEFORE_DAYS` (default 30) remain. The margin is
  what covers a device that is offline for a while.
- The same key is kept; only the certificate changes. The renewal takes
  about 4 s on an ESP32-C6, and the client reconnects straight away so the
  new certificate is in use (the old one is still valid at that point).
- A renewal that fails changes nothing: the device keeps working on its
  current certificate and tries again hourly.
- `te/device/<id>///twin/tedge_Certificate` carries the expiry on every
  connect, so certificate health is a query rather than an investigation:

```json
{"expires":"2027-09-20T08:04:46Z","daysRemaining":364,"renewals":1}
```

- If renewal keeps failing and fewer than
  `CONFIG_TEDGE_CERT_RENEW_ALARM_DAYS` (default 7) remain, the device raises
  a critical `c8y_CertificateExpiring` alarm naming the date, and clears it
  when a renewal succeeds.

**If a device does miss its window**, its certificate is no longer accepted
and it has to be onboarded again, with a new registration and one-time
password. That is why the alarm exists.

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
