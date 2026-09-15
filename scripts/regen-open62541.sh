#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Regenerate the vendored open62541 amalgamation (third_party/open62541/) and
# apply the small patches needed to build/run it on Zephyr.
#
# The amalgamation is a single-file build of open62541 configured for a
# constrained, read-only, POSIX-architecture profile. We vendor the generated
# open62541.{c,h} so the firmware builds without a code-generation step.
#
# Usage:
#   scripts/regen-open62541.sh [OPEN62541_SRC_DIR] [UA_LOGLEVEL]
#
#   OPEN62541_SRC_DIR  Path to the open62541 checkout
#                      (default: $WEST_TOPDIR/modules/lib/open62541, or the
#                       sibling modules/ dir of this repo).
#   UA_LOGLEVEL        open62541 log level to compile in (default 300 = INFO;
#                      use 100 for DEBUG/TRACE when diagnosing).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The OPC-UA frontend vendors the amalgamation under lib/opcua/third_party/.
out="${here}/lib/opcua/third_party"

src="${1:-}"
loglevel="${2:-300}"
# Profile: "minimal" (default) = MINIMAL ns0, no subscriptions — fits low-RAM
# boards like the ESP32-WROOM (writes still work). "reduced" = REDUCED ns0 with
# subscriptions enabled — needs more RAM than the WROOM has (OOMs at ns0 init);
# use it only on higher-RAM boards (e.g. ESP32-S2 with PSRAM).
profile="${3:-minimal}"
if [ -z "${src}" ]; then
  if [ -n "${WEST_TOPDIR:-}" ] && [ -d "${WEST_TOPDIR}/modules/lib/open62541" ]; then
    src="${WEST_TOPDIR}/modules/lib/open62541"
  else
    src="${here}/../modules/lib/open62541"
  fi
fi

case "${profile}" in
  reduced) NS0=REDUCED; SUBS=ON ;;
  minimal) NS0=MINIMAL; SUBS=OFF ;;
  *) echo "unknown profile '${profile}' (use minimal|reduced)"; exit 2 ;;
esac

echo "open62541 source: ${src}"
echo "output:           ${out}"
echo "profile:          ${profile} (NS0=${NS0}, SUBSCRIPTIONS=${SUBS})"
echo "UA_LOGLEVEL:      ${loglevel}"

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

# Generate the amalgamation (read-only base, POSIX architecture, single-threaded,
# no methods/discovery/history). Namespace-zero level and subscriptions depend on
# the selected profile (see above).
cmake -S "${src}" -B "${tmp}" \
  -DUA_ENABLE_AMALGAMATION=ON \
  -DUA_ARCHITECTURE=posix \
  -DUA_NAMESPACE_ZERO="${NS0}" \
  -DUA_ENABLE_SUBSCRIPTIONS="${SUBS}" \
  -DUA_ENABLE_METHODCALLS=OFF \
  -DUA_ENABLE_DISCOVERY=OFF \
  -DUA_ENABLE_HISTORIZING=OFF \
  -DUA_ENABLE_DIAGNOSTICS=OFF \
  -DUA_MULTITHREADING=0 \
  -DUA_LOGLEVEL="${loglevel}" \
  -DUA_ENABLE_NODEMANAGEMENT=ON \
  -DCMAKE_BUILD_TYPE=MinSizeRel >/dev/null
cmake --build "${tmp}" --target open62541-amalgamation >/dev/null 2>&1 || cmake --build "${tmp}" >/dev/null

mkdir -p "${out}"
cp "${tmp}/open62541.c" "${tmp}/open62541.h" "${out}/"

# --- Zephyr portability patches (applied to the generated amalgamation) ---
python3 - "${out}/open62541.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()

def sub(old, new, n=1):
    global s
    c = s.count(old)
    assert c == n, f"expected {n} occurrence(s), found {c}: {old[:60]!r}"
    s = s.replace(old, new)

# 1) IPv4-only: Zephyr's struct ipv6_mreq differs; we don't use IPv6/multicast.
sub("#define UA_IPV6 1", "#define UA_IPV6 0", n=2)

# 2) Skip the POSIX InterruptManager: it needs a self-signaling pipe whose
#    fcntl(O_NONBLOCK) is unsupported on Zephyr socket/pipe fds. We drive the
#    server via run_iterate(), so signal handling is not needed.
sub("            conf->eventLoop->registerEventSource(conf->eventLoop, &im->eventSource);",
    "            (void)im; /* Zephyr patch: skip interrupt manager (POSIX self-pipe fcntl unsupported) */")

# 3/4) Skip the UDP and raw-Ethernet connection managers: OPC-UA binary is
#      TCP-only for us (discovery multicast is disabled), and AF_PACKET raw
#      sockets are unsupported under native_sim NSOS.
sub("            conf->eventLoop->registerEventSource(conf->eventLoop, (UA_EventSource *)udpCM);",
    "            (void)udpCM; /* Zephyr patch: UDP CM not needed (TCP-only OPC-UA) */")
sub("            conf->eventLoop->registerEventSource(conf->eventLoop, (UA_EventSource *)ethCM);",
    "            (void)ethCM; /* Zephyr patch: raw Ethernet CM unsupported under NSOS */")

# 5) The (unregistered, unused) InterruptManager still contains a pipe() call
#    that ESP32/Zephyr does not provide. Neutralise it so it compiles; the
#    function is never invoked.
sub("    int err = pipe(pipefd);",
    "    int err = -1; (void)pipefd; /* Zephyr patch: pipe() unavailable; interrupt manager unused */")

# 6) Shrink the eventloop's shared RX buffer from 128 kB to 8 kB. The 128 kB
#    single allocation does not fit on a constrained MCU (e.g. ESP32-WROOM,
#    ~68 kB free heap after Wi-Fi). 8 kB is ample for OPC-UA.
sub("    UA_UInt32 rxBufSize = 2u << 16; /* The default is 64kb */",
    "    UA_UInt32 rxBufSize = 1u << 13; /* Zephyr patch: 8kB shared RX buffer (was 128kB) */")

# 7) Zephyr's getaddrinfo() rejects a NULL node (used by open62541 to listen on
#    all interfaces) with EAI_NONAME. Fall back to numeric "0.0.0.0", which
#    Zephyr accepts. Applies to both the listen and active-connect paths.
sub("    int retcode = getaddrinfo(hostname, portstr, &hints, &res);",
    "    int retcode = getaddrinfo(hostname ? hostname : \"0.0.0.0\", portstr, &hints, &res); /* Zephyr patch: NULL host unsupported */",
    n=2)

# 8) Zephyr's POSIX CLOCK_MONOTONIC(_RAW) does not advance reliably, which
#    breaks open62541's internal timers (repeated callbacks) and SecureChannel
#    lifetime management (clients get disconnected). Use Zephyr's k_uptime_get()
#    monotonic millisecond clock instead. (UA_DATETIME_SEC is 100ns ticks/sec.)
sub("UA_DateTime UA_DateTime_nowMonotonic(void) {\n#if defined(__APPLE__) || defined(__MACH__)",
    "UA_DateTime UA_DateTime_nowMonotonic(void) {\n    { extern int64_t k_uptime_get(void); return (UA_DateTime)(k_uptime_get() * (UA_DATETIME_SEC / 1000)); } /* Zephyr patch: CLOCK_MONOTONIC unreliable; use k_uptime */\n#if defined(__APPLE__) || defined(__MACH__)")

# 9) Back off on select() errors instead of spinning. When the socket layer
#    is transiently out of resources (Zephyr returns ENOMEM from select/poll
#    under connection churn), the event loop otherwise retries at the caller's
#    ~100 Hz, flooding the log and starving the Wi-Fi/net threads until the
#    whole device falls off the network. Yield ~50 ms so the stack can recover.
sub('                           "Error during select: %s", errno_str));\n        return UA_STATUSCODE_GOOD;',
    '                           "Error during select: %s", errno_str));\n        { extern void ua_zephyr_backoff(void); ua_zephyr_backoff(); } /* Zephyr patch: back off (k_msleep) on select error instead of spinning */\n        return UA_STATUSCODE_GOOD;')

# 10) Do NOT tear down the TCP listen socket on a transient accept() error.
#     Under net-buffer exhaustion (e.g. a burst of client (re)connections),
#     accept() returns ECONNABORTED/ENOMEM/ENOBUFS; open62541 treats every
#     accept error except EINTR as fatal and closes the server socket for good,
#     so the device stays pingable but stops serving OPC-UA until a reboot.
#     Treat these resource errors as retryable (like EINTR) so the listener
#     survives the storm and resumes accepting once buffers free.
sub("        /* Temporary error -- retry */\n        if(UA_ERRNO == UA_INTERRUPTED)\n            return;",
    "        /* Temporary error -- retry. Zephyr patch: also retry on transient\n"
    "         * resource-exhaustion errors so a net-buffer storm does not\n"
    "         * permanently close the listen socket (device would then need a\n"
    "         * reboot to serve again). */\n"
    "        if(UA_ERRNO == UA_INTERRUPTED || UA_ERRNO == UA_WOULDBLOCK ||\n"
    "           UA_ERRNO == EAGAIN || UA_ERRNO == ENOMEM ||\n"
    "           UA_ERRNO == ENOBUFS || UA_ERRNO == ECONNABORTED) {\n"
    "            extern void ua_zephyr_backoff(void); ua_zephyr_backoff();\n"
    "            return;\n"
    "        }")

open(p, "w").write(s)
print("Applied Zephyr patches to open62541.c")
PY

echo "Done. Vendored amalgamation refreshed in ${out}"
