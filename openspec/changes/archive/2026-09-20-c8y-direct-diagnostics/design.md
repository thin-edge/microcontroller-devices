## Context

Everything the client does today is about keeping a device working. This is
the first feature about a device that already failed: what was it saying
before it stopped, why did it reset, and what does it say if asked a direct
question now.

What the spikes and the repository already settled:

- **The shell must not be a network listener.** Spike problem P10:
  `shell_telnet` binds `INADDR_ANY` — an unauthenticated shell on the LAN —
  mirrors logs into the session and cannot turn echo off. A cloud-issued
  command has to run in-process instead.
- **Shell output and logging fight each other.** Spike B, finding 5: the
  shell thread prints deferred log messages, so a command that runs on the
  client thread would hold the client up. The command gets its own thread,
  as the download and the tunnel do.
- **HTTPS is Bearer-JWT and tenant-only.** `s/uat` → `s/dat` gives a
  certificate device a one-hour JWT; it is never sent off the tenant's
  domain, including after a redirect (`tedge_url_is_tenant()`).
- **The flash is already laid out for a dump.** Every board layout reserves
  a 4 KB `coredump_partition`; nothing has ever written to it.

Two promises are currently broken, and are repaired here rather than
documented around: `tedge_register_log_type()` has no definition at all (a
link error, where the spec requires "not supported"), and an operation the
image cannot run is dropped silently instead of being failed with a reason.

## Goals / Non-Goals

**Goals:** the last minutes of a device's log retrievable from the cloud;
the crash that erased them retrievable after the reboot; one allow-listed
command answerable on demand; an application able to add log types of its
own; none of it able to stall the connection.

**Non-Goals:** continuous log streaming, a filesystem, configuration
management (P7), decoding dumps on the device.

## Decisions

### D1: The command runs in-process, on its own thread, behind an allow-list

`c8y_Command` is executed with `shell_execute_cmd()` against a **dummy shell
backend** (`CONFIG_SHELL_BACKEND_DUMMY`), which Zephyr provides precisely to
capture output into a buffer. No listener, no port, no log mirroring.

It runs on a dedicated thread: a command that takes seconds must not stall
MQTT, and the shell thread is the one that prints deferred logs (spike B,
finding 5). One command at a time; a second is refused while one is running.

**The allow-list is empty by default.** `TEDGE_SHELL_COMMAND_ALLOW_LIST` is a
comma-separated list of command prefixes ("kernel uptime,net iface,tedge
diag"); a request must begin with one of them, and the rest must be plain
arguments. A request with `;`, `|`, `&`, a newline or a backtick is refused
whatever the list says, so an allowed prefix cannot be used to smuggle a
second command. With no list, every command is refused, naming the
configuration option — the feature being compiled in is not consent to run
anything.

*Alternative: a default list of "safe" read-only commands.* Rejected: what
is safe depends on what else is in the image, which only the integrator
knows. A command that dumps a key is safe in one product and not in another.

The matching is a pure function in its own file, unit-tested on native_sim,
like `tedge_ra_allow_list.c`.

### D2: A log type is a callback, not a file

`tedge_register_log_type(type, reader, user_data)` adds a type. The reader
writes through the sink it is given, so the client never needs a filesystem
and never holds a whole log. The request's filters
(`struct tedge_log_request`: from, to, search text, max lines) are passed
through; a reader applies what it can and ignores the rest.

The client registers two of its own: `tedge-log` (D3) and, when a dump is
stored, `coredump` (D5).

### D3: The client keeps its own log in a RAM ring

A Zephyr log backend writes formatted lines into a ring of
`TEDGE_LOG_RING_BYTES` (default 2048) in the module's heap, oldest dropped.
That is the log type every device has without its application doing
anything — the common case is a device whose application never built a log
of its own.

It is RAM: a reboot loses it. That is what the crash dump is for.

### D4: The upload is measured, then streamed

Cumulocity wants a length, and the device has no room to hold a log twice.
So an upload runs the reader **twice**: once through a counting sink to
learn the length, then again streaming into the request body, clamped to
the length just measured (padded with spaces if the second pass produces
less, truncated if more, because a ring can gain lines between passes).

*Alternative: buffer the whole log in the module heap.* Rejected: it puts a
hard ceiling on what can ever be uploaded and takes kilobytes from a 16 KB
heap at the worst possible moment.

**The upload API, settled against the tenant with curl (task 1.1).** No
multipart is needed, which takes a lot of code off the device:

1. `POST /event/events` with `Accept: application/json` and
   `{"source":{"id":"<mo-id>"},"type":"c8y_LogfileRequest","text":"<log
   type>","time":"<iso>"}` → `201` with `Location:
   https://<tenant-host>/event/events/<event-id>`. **`Accept` is not
   optional**: without it Cumulocity answers `201` with no body and no
   `Location`, and the device has nothing to work with.
2. `POST /event/events/<event-id>/binaries` with `Content-Type: text/plain`,
   `Content-Length`, an optional `Content-Disposition: attachment;
   filename="…"` (which names the file in the UI), and the log as the plain
   request body → `201`.
3. Answer `503,c8y_LogfileRequest,https://<tenant-host>/event/events/<event-id>/binaries`.
   The device builds that URL itself; no JSON parsing beyond reading
   `Location` out of a header it already captures for redirects.

The device knows its external ID, not its managed-object id, so it resolves
one once per session with `GET
/identity/externalIds/c8y_Serial/<external-id>` and keeps it. That is one
small GET, and its answer is the only JSON the upload path has to read.

*Alternative: chunked transfer encoding.* Rejected for now: Zephyr's HTTP
client already mishandles chunked **responses**
(`docs/zephyr-http-chunked-body-bug.md`), and the tenant's acceptance of a
chunked request body is unverified.

`TEDGE_LOG_UPLOAD_MAX_BYTES` (default 8192) caps a single upload; a longer
log is truncated with a final line saying so, rather than refused.

### D5: The crash dump is uploaded as the text format the tooling already reads

`CONFIG_DEBUG_COREDUMP` with the flash-partition backend writes a dump to
the `coredump_partition` on a fatal error. After the reboot the client sees
a stored dump (`coredump_query()`), advertises the `coredump` log type, and
its reader emits the same `#CD:` hex-line format Zephyr's console backend
produces — so `zephyr/scripts/coredump/coredump_serial_log_parser.py` reads
an uploaded file unchanged, with no new tooling to write.

The stored dump's length is known, so the reader's two passes agree exactly.
The dump is erased only when the cloud has taken it, so a failed upload can
be retried.

### D6: `TEDGE_HTTP` finally owns the HTTP code

`tedge_http_download.c` and `tedge_url.c` are compiled today when firmware
update is on, which was always a shortcut. They move to
`CONFIG_TEDGE_HTTP`, which log upload also selects, and `TEDGE_HTTP` gains
the Zephyr selects its help text has been promising. A new
`tedge_http_upload.c` adds the POST with a body callback; it shares the
redirect and tenant-only-JWT rules, because the reasons for both are the
same.

### D7: The protocol

| Direction | Message | Meaning |
|---|---|---|
| up, on connect | `118,<type>,<type>…` | the log types this image offers |
| up, on connect | `114,…,c8y_Command,…` | `c8y_Command` when the feature is in |
| down | `511,<id>,<command>` | run a command |
| down | `522,<id>,<type>,<from>,<to>,<search>,<lines>` | send a log |
| up | `501,<op>` then `503,<op>[,<result-or-url>]` or `502,<op>,<reason>` | progress, as everywhere else |

A log request answers `503,c8y_LogfileRequest,<url>` with the uploaded
binary's URL. `501` always goes first: Cumulocity's `502` fails the oldest
*executing* operation, so refusing straight from PENDING would leave the
operation stuck (the rule firmware update already follows).

A command's result goes in the `503`. SmartREST is one line, so newlines in
the output become spaces and the result is truncated to what the template
carries; the fix for a command with a lot to say is a log type, not a bigger
line.

### D8: An operation the image cannot run is failed, not dropped

The dispatcher's default branch fails the operation with a reason naming the
missing feature, instead of logging a debug line and leaving it pending.
This is already required ("Operation for an absent feature") and is the
smallest part of this change, but it is why a device with no log upload
stops looking broken when someone asks it for a log.

## Risks / Trade-offs

- [A cloud-issued shell command is powerful] → empty by default, prefix
  allow-list, no shell metacharacters, one at a time, and the whole feature
  compiled out unless asked for.
- [A command that never returns] → the operation reports a timeout after
  `TEDGE_SHELL_COMMAND_TIMEOUT_S`, but Zephyr cannot preempt a running
  command, so the thread stays busy and further commands are refused until
  it finishes. Documented, with the advice that an allow-list should not
  contain commands that can block forever.
- [The log ring is small and volatile] → stated plainly; the crash dump
  covers the case the ring cannot.
- [The dump costs flash and code] → 4 KB already reserved in every layout,
  and the feature is its own Kconfig option.
- [Two passes over a log] → cheap for RAM sources, and the clamp keeps the
  body honest if the ring moves underneath.
- [The upload API shape is unverified] → the first task is to confirm it
  against the tenant with curl before any device code is written.

## Migration Plan

- Applications gain working registration calls where they had a link error.
- `full.conf` drops its "not implemented yet" caveat for these two features;
  only configuration management remains experimental.
- Boards that want dumps add `CONFIG_TEDGE_COREDUMP=y`; the partition is
  already in their layout, so nothing repartitions and no device loses its
  settings.

## Results (ESP32-C6, Modbus application, 2026-09-20)

**Cost**, each row against the same build with every other feature on:

| Build | text | libc heap left |
|---|---|---|
| Everything but diagnostics | 815,800 | 112,672 |
| + log upload and crash dumps | 824,392 (+8.4 KB) | 100,096 (−12.3 KB) |
| + the Zephyr shell, without this feature | 855,888 (+39 KB) | 103,104 |
| + the shell command on top of that shell | 858,412 (+2.5 KB) | 90,528 (−12.6 KB) |

The shell command's real price is the Zephyr shell itself (39 KB), which an
integrator who wants this feature is paying anyway; the client's own part of
it is 2.5 KB of code and a thread.

**Verified on hardware**, against the tenant:

| Check | Result |
|---|---|
| Capabilities | `c8y_SupportedLogs: ["tedge-log","app-status"]`, and `coredump` appears only when a dump is stored |
| The client's own log | 2 KB of real log in Cumulocity, readable as it appeared on the console |
| An application's log type | the Modbus application's `app-status`, produced on demand |
| Filters | `searchText: "tedge"` with `maximumLines: 5` returned exactly those five lines |
| A log the device does not have | operation FAILED: "this device has no log called \"syslog\"" |
| An allowed command | `kernel uptime` → SUCCESSFUL with its output |
| An unlisted command | FAILED: "this command is not on the device's allow-list" |
| A chained command | FAILED: "the command contains a character that could chain another command" |
| No allow-list configured | every command FAILED, naming the option to set |
| A crash dump | forced fault → 1139 B stored → uploaded → decoded by Zephyr's parser to a 1139 B core → erased on the device |
| Two operations at once | the second waits for the first, and each gets its own result |

### What the hardware taught us

**The upload thread needs 8 KB, not 4.** Two TLS handshakes run on it (the
identity lookup and the upload). With 4096 it overflowed mid-handshake and,
with no `CONFIG_HW_STACK_PROTECTION`, showed up as an illegal instruction
with every register reading `0xaaaaaaaa` — the stack fill pattern. The
download and the tunnel already use 8192 for the same reason.

**One operation at a time is not optional.** Cumulocity's `501`, `502` and
`503` act on the oldest operation in the matching state, not on an operation
named in the message. Two operations executing at once therefore collect
each other's results. The client now runs one at a time and queues the rest
(D8 extended), which also fixes a latent problem: firmware update, remote
access and restart could already overlap.

**An operation interrupted by a reset stays EXECUTING in Cumulocity.** The
device cannot see it after the reboot — SmartREST resends only PENDING
operations — so it stays until an operator clears it, and until then every
`503` the device sends completes that stale one instead. Nothing here can
fix it from the device; it is worth knowing when reading a device's
operation history.

**A dump has to fit the partition.** With `MEMORY_DUMP_MIN` the dump is the
faulting thread's stack plus registers, so a thread with a 4 KB stack does
not fit the 4 KB `coredump_partition` — the backend reports `-ENOMEM` while
the device is already dying. The default `LINKER_RAM` mode never fits.
Applications get told both things in the README.

## Open Questions

- Whether the client should keep its log ring across a warm restart in the
  noinit region, so a commanded reboot does not lose it.
