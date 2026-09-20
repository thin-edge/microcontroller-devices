## Context

The client can connect, be operated and keep itself alive; it cannot yet
carry the data the device exists to produce. The API is declared and the
topic shape is proven: the spikes published
`te/device/<id>///m/environment` every 10 s and the tenant owner's Smart
Function turned it into `c8y_Environment` measurements. The same messages
are what a thin-edge.io gateway consumes, which is why both transports come
out of one API call.

What the spikes left for this change:
- **A timestamp on every message** (problem P4). Without it the cloud stamps
  the processing time, so anything buffered while offline is recorded as
  having happened when the device reconnected.
- **An alarm API**: certificate renewal publishes `301,c8y_CertificateExpiring`
  by hand because nothing better exists yet.

## Goals / Non-Goals

**Goals:** an application can publish measurements, events and alarms from
any thread; nothing blocks the caller; a short outage does not lose data
silently; the same call works on both transports; the client reports its own
health.

**Non-Goals:** persistence across a reboot, batching, sampling, child
devices.

## Decisions

### D1: The API queues; the client thread publishes

A call from the application builds the message and puts it in a bounded
queue; the client thread drains it. The caller never waits for the network,
and the MQTT client stays owned by one thread, as everywhere else.

Messages are built at the point of call, not at publish time, because that
is when the values and the timestamp are true.

*Alternative: publish directly from the caller under a mutex.* Rejected:
it would block an application thread on a TLS write, and MQTT is not
thread-safe.

### D2: One buffer, in bytes, oldest dropped first

The queue is a byte ring (`TEDGE_TELEMETRY_BUFFER_BYTES`, default 2048) in
the module heap holding complete messages. Sizing it in bytes rather than
messages keeps the memory bound honest whatever an application sends.

When a message does not fit, the oldest is dropped to make room, and a
counter increases. The counter is published with the next health message and
logged, so loss shows up as a number rather than a mystery. Events and
alarms are never dropped in favour of a measurement: they are queued ahead
of measurements, because "the pump stopped" matters more than the pressure
reading that came with it.

### D3: Timestamps

Every message carries `time` in ISO 8601 UTC, taken when the application
calls, or supplied by the application (it may have sampled earlier). If the
clock is not valid yet the message is still sent, without a `time`, and the
cloud stamps it: a device reporting during its first seconds is better than
one reporting nothing.

### D4: Topics and payloads

On the MQTT Service (free-form topics):

| What | Topic | Payload |
|---|---|---|
| Measurement | `te/device/<id>///m/<type>` | `{"time":"…","<series>":<value>,…}` |
| Event | `te/device/<id>///e/<type>` | `{"time":"…","text":"…"}` |
| Alarm | `te/device/<id>///a/<type>` | `{"time":"…","severity":"critical","text":"…"}` |
| Clear an alarm | `te/device/<id>///a/<type>` | empty payload (thin-edge.io's convention) |

On Core MQTT the same calls become SmartREST: `200,<type>,<series>,<value>`
per series, `400,<type>,"<text>"`, `301/302/303/304,<type>,"<text>"` by
severity, and `306,<type>` to clear.

Measurements go out at QoS 0 and events and alarms at QoS 1: a lost reading
is one reading, a lost alarm is an unreported fault.

### D5: The client's own health

With `TEDGE_HEALTH`, every `TEDGE_HEALTH_INTERVAL_S` (default 900) the
client publishes `tedge_health` measurements it can gather without the
application: uptime in seconds, free bytes in its own heap, dropped
telemetry messages, and the reset reason from `hwinfo` at boot. Wi-Fi signal
strength belongs to the application (it owns the interface), so an
application that wants it publishes it as its own measurement.

This is deliberately the client's health, not the device's: what an
application knows about itself is the application's to send.

### D6: Certificate renewal uses the alarm API

`tedge_cert_renew.c` calls `tedge_raise_alarm(TEDGE_ALARM_CRITICAL, …)` and
`tedge_clear_alarm()`, and the transport decides the encoding. Its hand-made
SmartREST goes away. With `TEDGE_TELEMETRY` off, the client falls back to
the SmartREST it publishes today, so renewal keeps its alarm in a minimal
build.

## Risks / Trade-offs

- [An application floods the queue] → the bound is bytes, the drop is
  counted and reported; the client never grows its memory to keep up.
- [A long outage loses data] → stated plainly: the buffer is RAM and small.
  Applications that must not lose data should keep their own store and
  publish from it.
- [Timestamps depend on the clock] → the client already refuses to connect
  before SNTP has run, so in practice messages are stamped; the unstamped
  case is the first seconds after boot.
- [Measurements at QoS 0 can be lost in a reconnect] → deliberate; events
  and alarms, which carry meaning, use QoS 1.

## Migration Plan

- Applications that already integrate the client gain working calls where
  they had `-ENOTSUP`; nothing they do today breaks.
- The Modbus application starts publishing its simulation, which makes the
  repository's example show data in Cumulocity.

## Results (ESP32-C6, Modbus application, 2026-09-20)

**Cost.** Telemetry and health together add 2,696 B of code and 2,160 B of
RAM (the 2 KB buffer and its bookkeeping) over the same build without them.
The signed image jumps 64 KB, which is the ESP32 padding its flash segments
to 64 KB MMU pages, not the feature's own size.

**What was verified on hardware.** Both transports were flashed onto the same
device and driven from the console (`tedge event|alarm|clear|flood`, test
aids in the application, not the module):

| Check | Result |
|---|---|
| Free-form payloads (MQTT Service) | `m/pump`, `e/app_test`, `a/app_test_alarm` and an empty payload to clear, each carrying `time` |
| Measurements (Core MQTT) | `pump` measurements in Cumulocity, one object per series (static template 200 takes one series per line) |
| Event and alarm (Core MQTT) | raised as MINOR and cleared, then MAJOR, with the device's own timestamps |
| Buffered through an outage | interface down for 70 s: event published at 09:24:15Z, `creationTime` 09:25:36Z — the cloud records when the reading was taken, 81 s before it arrived |
| A full buffer | `flood 120` → 85 measurements dropped and counted, nothing refused, the client kept running |
| Health | `tedge_health` every interval: uptime, freeHeap, droppedMessages, resetCause |

Two Cumulocity behaviours worth knowing, both the cloud's and not the
client's: raising an alarm of a type that is already active updates the
existing alarm rather than creating a second one (its severity stays as
first raised), and `te/` messages only become measurements once the tenant
has a Smart Function mapping them — Core MQTT needs none, because SmartREST
is native.

`resetCause` reads 0 on the ESP32-C6 after an esptool reset; `hwinfo`
reports nothing useful there, so the number is only meaningful on boards
whose driver fills it in.

## Open Questions

- Should the client publish a "buffer overflowed" event, rather than only a
  counter in its health measurement?
