#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Endpoint poller, trap recorder and outage detector for soak runs.

Every --interval seconds (default 5) it runs one protocol probe and one ping
against the device and writes a line to <run>.polls.log:

  snmp    SNMPv2c GET sysUpTime.0 (raw UDP; the reply also carries the device's
          uptime, so a reboot shows up as the uptime going backwards)
  modbus  Modbus TCP connect + read one holding register (any well-formed
          reply, including an exception, counts as "serving")
  opcua   OPC-UA TCP connect + HEL (an ACK or ERR reply counts as "serving")
  mqtt    a collector (e.g. tedge-dot) publishes the device's readings: serving
          while a message arrived on --mqtt-topic within --mqtt-max-age seconds.
          Use it when the collector holds the device's only protocol connection.

For --app snmp it also runs an unprivileged `snmptrapd` on --trap-port (1162)
writing <run>.traps.log. Build the firmware with that manager/port (see
scripts/soak/README.md) so the device's traps reach it.

Outage: --fail-threshold (3) consecutive failed protocol probes. The outage
starts at the first failed probe and ends at the next successful one; failures
before the first successful probe (the device is still booting) don't count. If the
run's --duration ends during an outage, recording continues until the device
recovers or --recovery-window seconds have passed since the outage started,
so the firmware's own recovery (liveness reset, last-resort reboot, rejoin)
is always observed. An outage longer than --recovery-window is "unrecovered".

At the end it writes <run>.summary.json. If --console is given, the console
log is scanned for boots, liveness reset records and health lines.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

SYS_UPTIME = (1, 3, 6, 1, 2, 1, 1, 3, 0)


def now_iso():
    return datetime.datetime.now().isoformat(timespec="milliseconds")


# --- minimal BER for one SNMP GET ------------------------------------------

def _len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def _tlv(tag, value):
    return bytes([tag]) + _len(len(value)) + value


def _int(v):
    return _tlv(0x02, v.to_bytes(max(1, (v.bit_length() + 8) // 8), "big", signed=True))


def _oid(parts):
    body = bytes([parts[0] * 40 + parts[1]])
    for p in parts[2:]:
        enc = [p & 0x7F]
        p >>= 7
        while p:
            enc.insert(0, 0x80 | (p & 0x7F))
            p >>= 7
        body += bytes(enc)
    return _tlv(0x06, body)


def snmp_get_request(community, req_id, oid):
    varbind = _tlv(0x30, _oid(oid) + b"\x05\x00")
    pdu = _tlv(0xA0, _int(req_id) + _int(0) + _int(0) + _tlv(0x30, varbind))
    return _tlv(0x30, _int(1) + _tlv(0x04, community.encode()) + pdu)


def _read_tlv(buf, i):
    tag = buf[i]
    n = buf[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(buf[i:i + k], "big")
        i += k
    return tag, buf[i:i + n], i + n


def snmp_parse_uptime(buf, req_id):
    """Return sysUpTime in centiseconds, or raise ValueError."""
    tag, msg, _ = _read_tlv(buf, 0)
    if tag != 0x30:
        raise ValueError("not a sequence")
    _, _, i = _read_tlv(msg, 0)          # version
    _, _, i = _read_tlv(msg, i)          # community
    tag, pdu, _ = _read_tlv(msg, i)
    if tag != 0xA2:
        raise ValueError(f"unexpected PDU 0x{tag:02x}")
    _, rid, j = _read_tlv(pdu, 0)
    if int.from_bytes(rid, "big", signed=True) != req_id:
        raise ValueError("request-id mismatch")
    _, err, j = _read_tlv(pdu, j)
    if int.from_bytes(err, "big"):
        raise ValueError(f"error-status {int.from_bytes(err, 'big')}")
    _, _, j = _read_tlv(pdu, j)          # error-index
    _, vbl, _ = _read_tlv(pdu, j)
    _, vb, _ = _read_tlv(vbl, 0)
    _, _, k = _read_tlv(vb, 0)           # oid
    tag, val, _ = _read_tlv(vb, k)
    if tag != 0x43:
        raise ValueError(f"sysUpTime has tag 0x{tag:02x}")
    return int.from_bytes(val, "big")


# --- probes -----------------------------------------------------------------

class Probe:
    def __init__(self, args):
        self.args = args
        self.req_id = 1000
        self.mqtt_sub = None
        self.mqtt_last = None
        self.mqtt_count = 0

    def snmp(self):
        self.req_id = (self.req_id + 1) & 0x7FFFFFFF
        pkt = snmp_get_request(self.args.community, self.req_id, SYS_UPTIME)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(self.args.timeout)
            s.sendto(pkt, (self.args.host, self.args.snmp_port))
            deadline = time.monotonic() + self.args.timeout
            while True:
                s.settimeout(max(0.05, deadline - time.monotonic()))
                data, _ = s.recvfrom(2048)
                try:
                    cs = snmp_parse_uptime(data, self.req_id)
                except ValueError:
                    if time.monotonic() >= deadline:
                        raise
                    continue  # a late reply to an earlier request
                return cs / 100.0, f"uptime={cs / 100.0:.2f}s"

    def modbus(self):
        tid = self.req_id = (self.req_id + 1) & 0xFFFF
        req = struct.pack(">HHHBBHH", tid, 0, 6, self.args.unit_id, 3, 0, 1)
        with socket.create_connection((self.args.host, self.args.modbus_port),
                                      timeout=self.args.timeout) as s:
            s.settimeout(self.args.timeout)
            s.sendall(req)
            hdr = _recv_exact(s, 8)
            rtid, proto, length, _unit, fc = struct.unpack(">HHHBB", hdr)
            if rtid != tid or proto != 0 or length < 2:
                raise ValueError(f"bad MBAP tid={rtid} proto={proto} len={length}")
            _recv_exact(s, length - 2)
            return None, "exception" if fc & 0x80 else "fc3 ok"

    def opcua(self):
        url = f"opc.tcp://{self.args.host}:{self.args.opcua_port}".encode()
        body = struct.pack("<IIIII", 0, 65536, 65536, 0, 0) + struct.pack("<i", len(url)) + url
        hel = b"HELF" + struct.pack("<I", 8 + len(body)) + body
        with socket.create_connection((self.args.host, self.args.opcua_port),
                                      timeout=self.args.timeout) as s:
            s.settimeout(self.args.timeout)
            s.sendall(hel)
            hdr = _recv_exact(s, 8)
            if hdr[:3] not in (b"ACK", b"ERR"):
                raise ValueError(f"unexpected reply {hdr[:4]!r}")
            return None, hdr[:3].decode()

    def mqtt(self):
        if self.mqtt_sub is None:
            self._start_mqtt()
        last = self.mqtt_last
        if last is None:
            raise TimeoutError("no collector message yet")
        age = time.time() - last
        if age > self.args.mqtt_max_age:
            raise TimeoutError(f"last collector message {age:.0f}s ago")
        return None, f"last msg {age:.0f}s ago ({self.mqtt_count} total)"

    def _start_mqtt(self):
        import threading
        self.mqtt_last = None
        self.mqtt_count = 0
        cmd = ["mosquitto_sub", "-h", self.args.mqtt_host]
        for t in self.args.mqtt_topic:
            cmd += ["-t", t]
        self.mqtt_sub = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                         stderr=subprocess.DEVNULL, text=True)

        def reader():
            for _ in self.mqtt_sub.stdout:
                self.mqtt_last = time.time()
                self.mqtt_count += 1

        threading.Thread(target=reader, daemon=True).start()

    def close(self):
        if getattr(self, "mqtt_sub", None) is not None:
            self.mqtt_sub.terminate()

    def ping(self):
        if sys.platform == "darwin":
            cmd = ["ping", "-c", "1", "-t", str(max(1, int(self.args.timeout))), self.args.host]
        else:
            cmd = ["ping", "-c", "1", "-W", str(max(1, int(self.args.timeout))), self.args.host]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return False, "no reply"
        m = re.search(r"time[=<]([\d.]+) ?ms", r.stdout)
        return True, f"{m.group(1)}ms" if m else "ok"


def _recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed by device")
        buf += chunk
    return buf


# --- trap listener ------------------------------------------------------------

class PyTrapListener:
    """Minimal built-in trap recorder, used when snmptrapd isn't installed.

    Logs each datagram with a timestamp, the sender and which of the
    firmware's traps it is (coldStart / linkDown / linkUp) plus the ifIndex.
    """
    NAMES = {
        _oid((1, 3, 6, 1, 6, 3, 1, 1, 5, 1)): "coldStart",
        _oid((1, 3, 6, 1, 6, 3, 1, 1, 5, 3)): "linkDown",
        _oid((1, 3, 6, 1, 6, 3, 1, 1, 5, 4)): "linkUp",
    }

    def __init__(self, port, path):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.sock.settimeout(0.5)
        self.out = open(path, "a", buffering=1, encoding="utf-8")
        self.stop = False
        import threading
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while not self.stop:
            try:
                data, (src, _) = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            name = next((n for enc, n in self.NAMES.items() if enc in data), "trap")
            m = re.search(rb"\x02\x01(.)$", data, re.S)
            ifx = f" ifIndex={m.group(1)[0]}" if m and name != "coldStart" else ""
            stamp = datetime.datetime.now().isoformat(timespec="seconds")
            self.out.write(f"{stamp} {src} {name}{ifx} len={len(data)}\n")

    def terminate(self):
        self.stop = True
        self.thread.join(2)
        self.sock.close()
        self.out.close()

    def wait(self, timeout=None):
        return 0

    def kill(self):
        pass


def start_trapd(args, path):
    exe = shutil.which("snmptrapd") or ("/usr/sbin/snmptrapd"
                                        if os.path.exists("/usr/sbin/snmptrapd") else None)
    if exe is None:
        return PyTrapListener(args.trap_port, path), None
    conf = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
    conf.write("disableAuthorization yes\n")
    conf.close()
    fmt = "%.4y-%.2m-%.2lT%.2h:%.2j:%.2k %b %v\n"
    # Keep net-snmp's persistent state out of /var/db (not writable unprivileged).
    env = dict(os.environ, SNMP_PERSISTENT_DIR=tempfile.mkdtemp(prefix="soak-snmp-"))
    proc = subprocess.Popen(
        [exe, "-f", "-n", "-On", "-m", "", "-C", "-c", conf.name, "-Lf", path,
         "-F", fmt, f"udp:{args.trap_port}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    return proc, conf.name


# --- console scan --------------------------------------------------------------

def scan_console(path):
    # The ROM prints "rst:0x<n> (<REASON>)" on every reset; the Zephyr banner
    # can be lost in the boot-time noise, so count ROM lines.
    rom_reset = re.compile(r"\brst:0x[0-9a-f]+ \(([A-Z0-9_]+)\)")
    out = {"boots": 0, "rom_reset_reasons": [], "liveness_resets": [],
           "hw_watchdog_resets": 0, "fatal_errors": 0,
           "health_lines": 0, "stale_reports": 0, "last_resort_reboots": 0,
           "connectivity_lost": 0}
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = rom_reset.search(line)
            if m:
                out["boots"] += 1
                out["rom_reset_reasons"].append(m.group(1))
            elif "ZEPHYR FATAL ERROR" in line:
                out["fatal_errors"] += 1
            elif "LIVENESS RESET" in line:
                out["liveness_resets"].append(line.split(" ", 1)[1].strip())
            elif "hardware watchdog reset" in line:
                out["hw_watchdog_resets"] += 1
            elif " HEALTH " in line:
                out["health_lines"] += 1
            elif " STALE " in line:
                out["stale_reports"] += 1
            elif "despite reconnects — rebooting" in line:
                out["last_resort_reboots"] += 1
            elif "Connectivity lost" in line:
                out["connectivity_lost"] += 1
    return out


# --- main loop -------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", required=True, choices=["snmp", "modbus", "opcua", "mqtt"])
    ap.add_argument("--host", required=True, help="device IP (literal, not .local)")
    ap.add_argument("--out", required=True, help="run prefix, e.g. runs/2026-09-16T1700-cam")
    ap.add_argument("--duration", type=float, default=3600)
    ap.add_argument("--interval", type=float, default=5.0)
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--fail-threshold", type=int, default=3)
    ap.add_argument("--recovery-window", type=float, default=30 + 300 + 120,
                    help="liveness timeout + last-resort timeout + rejoin (s)")
    ap.add_argument("--stop-after-outages", type=int, default=0,
                    help="end the run once this many outages have finished (0 = never)")
    ap.add_argument("--community", default="public")
    ap.add_argument("--snmp-port", type=int, default=161)
    ap.add_argument("--modbus-port", type=int, default=502)
    ap.add_argument("--unit-id", type=int, default=1)
    ap.add_argument("--opcua-port", type=int, default=4840)
    ap.add_argument("--trap-port", type=int, default=1162)
    ap.add_argument("--no-traps", action="store_true")
    ap.add_argument("--mqtt-host", default="127.0.0.1")
    ap.add_argument("--mqtt-topic", action="append", default=[],
                    help="collector topic for the device (repeatable)")
    ap.add_argument("--mqtt-max-age", type=float, default=30.0)
    ap.add_argument("--console", help="console log to scan into the summary")
    ap.add_argument("--meta", default="{}", help="JSON merged into the summary")
    args = ap.parse_args()

    probe = Probe(args)
    probe_fn = getattr(probe, args.app)
    trapd, trapconf = (None, None)
    traps_path = args.out + ".traps.log"
    if args.app == "snmp" and not args.no_traps:
        trapd, trapconf = start_trapd(args, traps_path)

    t_start = time.time()
    t_end = t_start + args.duration
    start_iso = now_iso()
    stats = {"probes": 0, "probe_ok": 0, "pings": 0, "ping_ok": 0}
    outages = []
    fails = 0
    first_fail_at = None
    cur = None                 # outage in progress
    last_ok_at = None
    first_ok_at = None
    last_ok_uptime = None
    last_recovery_at = t_start
    reboots_seen = []

    polls = open(args.out + ".polls.log", "a", buffering=1, encoding="utf-8")
    polls.write(f"{now_iso()} ##### HOST: polling {args.app} {args.host} "
                f"every {args.interval}s for {args.duration}s\n")
    try:
        while True:
            tick = time.time()
            try:
                uptime, detail = probe_fn()
                ok = True
            except Exception as e:  # noqa: BLE001 - any failure is a failed probe
                uptime, detail, ok = None, f"{type(e).__name__}: {e}"[:80], False
            ping_ok, ping_detail = probe.ping()
            stats["probes"] += 1
            stats["probe_ok"] += ok
            stats["pings"] += 1
            stats["ping_ok"] += ping_ok
            polls.write(f"{now_iso()} {'OK  ' if ok else 'FAIL'} {detail} "
                        f"| ping {'ok' if ping_ok else 'FAIL'} {ping_detail}\n")

            if ok:
                if uptime is not None and last_ok_uptime is not None and uptime + 1 < last_ok_uptime:
                    reboots_seen.append({"at": now_iso(), "uptime_before_s": last_ok_uptime,
                                         "uptime_after_s": uptime})
                    polls.write(f"{now_iso()} ##### HOST: device rebooted "
                                f"(uptime {last_ok_uptime:.0f}s -> {uptime:.0f}s)\n")
                if cur is not None:
                    cur["end"] = now_iso()
                    cur["duration_s"] = round(tick - cur["_t"], 1)
                    cur["recovered"] = cur["duration_s"] <= args.recovery_window
                    cur["uptime_after_s"] = uptime
                    cur["rebooted"] = (uptime is not None and cur["uptime_before_s"] is not None
                                       and uptime < cur["uptime_before_s"])
                    polls.write(f"{now_iso()} ##### HOST: outage over after "
                                f"{cur['duration_s']}s\n")
                    outages.append(cur)
                    cur = None
                    last_recovery_at = tick
                fails, first_fail_at = 0, None
                if first_ok_at is None:
                    first_ok_at = tick
                last_ok_at, last_ok_uptime = tick, uptime if uptime is not None else last_ok_uptime
            else:
                fails += 1
                if fails == 1:
                    first_fail_at = tick
                # Failures before the first successful probe are boot time, not
                # an outage.
                if fails == args.fail_threshold and cur is None and last_ok_at is not None:
                    cur = {
                        "_t": first_fail_at,
                        "start": datetime.datetime.fromtimestamp(first_fail_at)
                                 .isoformat(timespec="seconds"),
                        "ttf_since_run_start_s": round(first_fail_at - t_start, 1),
                        "ttf_since_last_recovery_s": round(first_fail_at - last_recovery_at, 1),
                        "uptime_before_s": last_ok_uptime,
                        "ping_at_detection": ping_ok,
                    }
                    polls.write(f"{now_iso()} ##### HOST: outage "
                                f"({args.fail_threshold} failed probes)\n")

            if args.stop_after_outages and len(outages) >= args.stop_after_outages:
                break
            now = time.time()
            if now >= t_end:
                if cur is None:
                    break
                if now - cur["_t"] >= args.recovery_window:
                    break
            time.sleep(max(0.0, args.interval - (time.time() - tick)))
    except KeyboardInterrupt:
        pass
    finally:
        probe.close()
        if trapd is not None:
            trapd.terminate()
            try:
                trapd.wait(5)
            except subprocess.TimeoutExpired:
                trapd.kill()
            if trapconf:
                os.unlink(trapconf)

    t_stop = time.time()
    if cur is not None:
        cur["end"] = None
        cur["duration_s"] = round(t_stop - cur["_t"], 1)
        cur["recovered"] = False
        outages.append(cur)
    for o in outages:
        o.pop("_t", None)
    polls.write(f"{now_iso()} ##### HOST: run ended\n")
    polls.close()

    traps = 0
    if os.path.exists(traps_path):
        with open(traps_path, encoding="utf-8", errors="replace") as f:
            traps = sum(1 for line in f if re.match(r"^\d{4}-\d{2}-\d{2}T", line))

    summary = {
        "app": args.app,
        "host": args.host,
        "start": start_iso,
        "end": now_iso(),
        "runtime_s": round(t_stop - t_start, 1),
        "probe_interval_s": args.interval,
        "recovery_window_s": args.recovery_window,
        **stats,
        # A device that never answered has no "outage" by the rule above, but
        # the run is still a failure.
        "first_ok_after_s": (round(first_ok_at - t_start, 1)
                             if first_ok_at is not None else None),
        "never_reachable": first_ok_at is None,
        "outage_count": len(outages),
        "unrecovered_count": sum(1 for o in outages if not o["recovered"]),
        "outages": outages,
        "reboots_seen_by_uptime": reboots_seen,
        "traps_received": traps,
        "console": scan_console(args.console),
    }
    summary.update(json.loads(args.meta))
    with open(args.out + ".summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    print(json.dumps({k: summary[k] for k in
                      ("runtime_s", "never_reachable", "outage_count", "unrecovered_count",
                       "traps_received")}))


if __name__ == "__main__":
    main()
