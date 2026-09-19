#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Serve firmware images to the spike over plain HTTP (Spike B).

  GET /<file>                   the file from --dir
  GET /redirect/<n>/<file>      302 -> /redirect/<n-1>/<file> ... -> /<file>
  GET /abs-redirect/<file>      302 with an absolute Location to /<file>

Each request is logged with its size and duration, so the device's download
rate can be compared with what the server saw.

Usage: ota_server.py --dir DIR [--port 8000]
"""

import argparse
import http.server
import os
import time


def make_handler(root):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            print(f"[{time.strftime('%H:%M:%S')}] {self.client_address[0]} "
                  f"{fmt % args}", flush=True)

        def redirect(self, location):
            self.send_response(302)
            self.send_header("Location", location)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            parts = self.path.strip("/").split("/")
            if parts[0] == "redirect" and len(parts) == 3:
                n, name = int(parts[1]), parts[2]
                self.redirect(f"/redirect/{n - 1}/{name}" if n > 1
                              else f"/{name}")
                return
            if parts[0] == "abs-redirect" and len(parts) == 2:
                host = self.headers.get("Host")
                self.redirect(f"http://{host}/{parts[1]}")
                return

            path = os.path.join(root, os.path.basename(self.path))
            if not os.path.isfile(path):
                self.send_error(404)
                return
            size = os.path.getsize(path)
            t0 = time.monotonic()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with open(path, "rb") as f:
                while chunk := f.read(16384):
                    self.wfile.write(chunk)
            dt = time.monotonic() - t0
            print(f"    sent {size} B in {dt:.1f} s "
                  f"({size / 1024 / max(dt, 1e-3):.0f} KB/s)", flush=True)

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", args.port),
                                          make_handler(args.dir))
    print(f"serving {args.dir} on :{args.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
