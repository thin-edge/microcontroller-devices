# Debugging against real devices

Notes from chasing real failures on ESP32 hardware, written down because
every one of them cost hours and several of them cost a confident wrong
answer first. The worked example throughout is a remote-access tunnel that
could not carry `htop`; the traps generalise.

The short version: **on this hardware, most of the time lost goes to
believing a measurement that was never measuring the right thing.**

## Trust the device's own counters, not your stopwatch

The client logs what a tunnel actually moved:

```
remote access: tunnel to 192.168.68.72:22 ended (the client closed the
session) after 5 s: 105885 B up, 3418 B down
```

That is ground truth: bytes the bridge passed, over the tunnel's lifetime.

An SSH transfer timed from the host is not. It includes TCP connect, the
SSH handshake and authentication — which on a slow link dominate a small
transfer. Timing `ssh <host> 'dd …'` once reported **0.9 KB/s** for a
tunnel the device's own log showed running at **21 KB/s**. Two rounds of
tuning were spent on that phantom.

If you must time from the host, transfer enough to swamp setup (1 MB, not
100 KB) and compare against the device's figure before believing it.

## Signals that look like liveness and are not

- **`c8y_Availability.lastMessage`.** It tracks particular message types,
  not connectivity. It read `07:41` for a device observed connecting at
  `15:08`. A working image was reverted on the strength of it.
- **`c8y_Connection.status`.** Can read `CONNECTED` for a device that has
  been silent for minutes.

What does work: a recent **measurement** (real traffic), or the console.

## The console is authoritative — and on some boards, destructive

On the **ESP32-C6** the console rides USB-Serial-JTAG, and opening it from
macOS **resets the chip** (`app_liveness: … esp_reason 11`, `ESP_RST_USB`).
So:

- Attaching mid-session kills the session you are debugging. Symptoms
  reported as "the board restarts when I use htop" were partly the console
  being attached underneath.
- **Attach before the test starts and hold the port open** for its whole
  duration. Take the reset up front.
- The boot counter in that same line (`boot 248`) tells you how many resets
  a board has taken — useful for noticing you are the cause.

## Two ESP32 allocation failures that are easily confused

```
esp32_wifi: Failed to allocate net buffer          ← RX net_buf pool. Fatal.
esp32c6_wifi_adapter: memory allocation failed     ← kernel heap. Often survivable.
```

They come from different pools and need different fixes. Chasing the second
while the first was the killer cost a full cycle of build-flash-test.

- **`Failed to allocate net buffer`** is
  `drivers/wifi/esp32`, `net_pkt_rx_alloc_with_buffer(..., K_MSEC(100))`.
  It means `NET_BUF_RX_COUNT` / `NET_PKT_RX_COUNT` are exhausted. Frames
  are dropped, TCP retransmits, a TLS socket eventually returns `-116`
  (`ETIMEDOUT`) and whatever it was carrying dies.
- **`memory allocation failed`** is `k_malloc` from
  `CONFIG_HEAP_MEM_POOL_SIZE`. Two consumers share it: the Wi-Fi adapter,
  and `websocket_send_msg()`, which allocates a masking buffer **per frame**
  (masking is mandatory client→server). Its `-ENOMEM` surfaces as the
  bridge's "sending to the cloud failed".

### Burst depth, not bandwidth

`CONFIG_NET_BUF_DATA_SIZE` defaults to **128 bytes**, so one 1500-byte
frame costs **12 buffers**. `NET_BUF_RX_COUNT=64` is therefore about **five
full-size packets in flight** — fine for MQTT, nowhere near enough for a
terminal repainting through a tunnel.

The tell: **it fails at a low byte count**. A session that dies after
12.9 KB while a 1 MB bulk transfer succeeds is not short of bandwidth; it
cannot absorb a burst. Size for ~16 full packets before suspecting
throughput.

## Reproducing an interactive session

A TUI is not a pipe, and the difference decides the result.

- **Piping through `cat`/`tee` makes ncurses emit full repaints.** A real
  terminal gets **incremental, cursor-addressed** updates, which are far
  less tolerant of loss. A piped test passes while the real session fails.
  If the bug is interactive, the test must use a PTY.
- **`pty.fork()` ignores `LINES`/`COLUMNS`.** Without a `TIOCSWINSZ` ioctl
  the remote runs at 80×24 and moves a quarter of the data a 200×50 window
  does. Set the size explicitly and match the size the human is using —
  frame cost scales with area.
- **Measure repaints over time, not bytes.** "One screen then frozen" and
  "updating happily" move similar amounts of data in the first seconds.
  Count updates and look at the gaps between them; htop's own advancing
  `TIME+` field is a convenient per-refresh marker.
- **Do not grep a terminal capture with `strings`.** It splits at escape
  sequences, so `Tasks:` appears to have no value when it does. That looked
  like packet loss for an hour. Render the capture, or match on the raw
  bytes including the escapes.

## Method

1. **Get the device's own account first.** Console attached before the
   test, plus the failure counters above. Most of this page exists because
   host-side inference was wrong and the device had been saying so.
2. **Change one thing.** Two tunings landed together once and the result
   was unreadable.
3. **Let the human drive the real test.** A manual session found in one
   attempt what four increasingly elaborate harnesses had missed, because
   the harnesses kept accidentally testing something easier.
4. **Prefer the board with headroom when isolating.** If a failure appears
   on both an S3 (PSRAM, ~78 KB spare) and a C6 (no PSRAM), it is not about
   memory, and every memory theory can be dropped at once.
5. **Check whether a lever does anything before tuning it.** Reducing
   `MBEDTLS_SSL_MAX_CONTENT_LEN` changed **zero** bytes of static RAM — the
   mbedTLS heap is a fixed `.bss` array — yet it is the obvious knob and
   two builds went into it.

## Levers, and whether they help

| Lever | Effect |
|---|---|
| `NET_BUF_DATA_SIZE`, `NET_BUF_RX_COUNT` | **the fix** for RX burst starvation |
| `HEAP_MEM_POOL_SIZE` | needed once a tunnel shares the radio with MQTT |
| bridge drain loop (poll only when idle) | 2 KB/s → 27 KB/s; without it nothing else matters |
| `TCP_NODELAY` | correct, and Zephyr defaults Nagle **on** — but not the cause here |
| `MBEDTLS_SSL_MAX_CONTENT_LEN` | no static effect; only per-session heap use |
| throttling the bridge | made it **worse** (12 allocation failures vs 2) |

See also `tedge-zephyr/profiles/` for per-board sizing and
`docs/zephyr-http-chunked-body-bug.md` for an upstream bug found the same
way.
