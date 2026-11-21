#!/usr/bin/env python3
# FreeRDP: A Remote Desktop Protocol Implementation
# Reads what sdl-freerdp /sdl-stats-ipc sends: the overlay's numbers, one JSON line a second.
#
# Copyright 2026 Cong Zhang <congzhangzh@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Read the statistics of a running sdl-freerdp started with /sdl-stats-ipc.

  sdl-stats-ipc.py [SOCKET] --once            the newest reading, as JSON
  sdl-stats-ipc.py [SOCKET] --rows            the overlay's rows, as text
  sdl-stats-ipc.py [SOCKET] --get NAME...     counters of the newest reading
  sdl-stats-ipc.py [SOCKET] --rate NAME...    counters per second over the next reading
  sdl-stats-ipc.py [SOCKET] --follow [--count N] > run.jsonl   every reading, one per line
  sdl-stats-ipc.py --summary run.jsonl        each counter: first, last, change, per second

Without SOCKET it takes the one sdl-freerdp-<pid>.stats.sock in $XDG_RUNTIME_DIR or $TMPDIR.
Only the Python standard library is used. Windows' Python has no AF_UNIX sockets.
"""

import argparse
import glob
import json
import os
import socket
import sys
import tempfile
import threading

SCHEMA = "freerdp-sdl-stats"
VERSION = 1


def find_socket():
    found = []
    for variable in ("XDG_RUNTIME_DIR", "TMPDIR"):
        base = os.environ.get(variable)
        if base:
            found += glob.glob(os.path.join(base, "sdl-freerdp-*.stats.sock"))
    found = sorted(set(found))
    if len(found) != 1:
        raise SystemExit(
            "give the socket: %s" % ("none found" if not found else "several: " + ", ".join(found))
        )
    return found[0]


class Reader:
    """One connection: lines as they come."""

    def __init__(self, path, timeout):
        if not hasattr(socket, "AF_UNIX"):
            raise SystemExit("this Python has no AF_UNIX sockets")
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.buffer = b""

    def line(self):
        while b"\n" not in self.buffer:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("the client closed the socket")
            self.buffer += data
        raw, self.buffer = self.buffer.split(b"\n", 1)
        return raw.decode("utf-8", errors="replace")

    def reading(self):
        value = json.loads(self.line())
        check(value)
        return value

    def close(self):
        self.sock.close()


def check(reading):
    if reading.get("schema") != SCHEMA:
        raise SystemExit("not %s: %r" % (SCHEMA, reading.get("schema")))
    if reading.get("version") != VERSION:
        raise SystemExit("schema version %r, this reader knows %d" % (reading.get("version"), VERSION))


def rate(first, second, name):
    """name per second between two readings, None when it went down (a new connection)."""
    a = first["counters"].get(name)
    b = second["counters"].get(name)
    seconds = (second["ns"] - first["ns"]) / 1e9
    if a is None or b is None or seconds <= 0 or b < a or first["connection"] != second["connection"]:
        return None
    return (b - a) / seconds


def rows_text(reading):
    out = []
    for part in ("client", "server", "realtime"):
        for label, value in reading["rows"].get(part, []):
            out.append("%-9s %-40s %s" % (part, label, value))
    return "\n".join(out)


def summary(lines):
    readings = [json.loads(line) for line in lines if line.strip()]
    if not readings:
        raise SystemExit("no readings")
    for reading in readings:
        check(reading)
    first = readings[0]
    # only the part of the run that is one connection: the counters start over at a reconnect
    last = [r for r in readings if r["connection"] == first["connection"]][-1]
    seconds = (last["ns"] - first["ns"]) / 1e9
    out = ["readings %d, seconds %.1f, connection %d" % (len(readings), seconds, first["connection"])]
    for name in sorted(set(first["counters"]) | set(last["counters"])):
        a = first["counters"].get(name, 0)
        b = last["counters"].get(name, 0)
        per = "%.3f" % ((b - a) / seconds) if seconds > 0 else "-"
        out.append("%-44s %20d %20d %20d %14s" % (name, a, b, b - a, per))
    return "\n".join(out)


def self_test():
    """A fake client on a temporary socket, then each mode against it."""
    sample = {
        "schema": SCHEMA, "version": VERSION, "seq": 1, "ns": 1_000_000_000, "time_ms": 1_795_000_000_000,
        "connection": 1,
        "session_seconds": 1, "desktop": [1920, 1080], "window": [1280, 720],
        "counters": {"RDPGFX_FRAMES_DECODED": 10, "BIG": 2**64 - 1},
        "strings": {"SERVER_HOSTNAME": "hé"}, "backends": {"AV1": "dav1d"},
        "rows": {"client": [["FreeRDP", "3"]], "server": [], "realtime": [["Frames", "30 /s"]]},
    }
    second = dict(sample, seq=2, ns=3_000_000_000,
                  counters={"RDPGFX_FRAMES_DECODED": 70, "BIG": 2**64 - 1})
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "s.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(1)

        def serve():
            connection, _ = server.accept()
            for item in (sample, second):
                connection.sendall((json.dumps(item, ensure_ascii=False) + "\n").encode("utf-8"))
            connection.close()

        thread = threading.Thread(target=serve)
        thread.start()
        reader = Reader(path, 5)
        a = reader.reading()
        b = reader.reading()
        reader.close()
        thread.join()
        server.close()
    assert a["counters"]["BIG"] == 2**64 - 1, "integers are exact"
    assert a["strings"]["SERVER_HOSTNAME"] == "hé"
    assert a["time_ms"] == 1_795_000_000_000
    assert rate(a, b, "RDPGFX_FRAMES_DECODED") == 30.0
    assert rate(a, dict(b, connection=2), "RDPGFX_FRAMES_DECODED") is None
    assert "Frames" in rows_text(a)
    text = summary([json.dumps(a), json.dumps(b)])
    assert "RDPGFX_FRAMES_DECODED" in text and "30.000" in text, text
    try:
        check(dict(a, version=2))
        raise AssertionError("a newer schema must be refused")
    except SystemExit:
        pass
    print("self test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("socket", nargs="?", help="the client's socket (default: the only one there is)")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--once", action="store_true")
    mode.add_argument("--rows", action="store_true")
    mode.add_argument("--get", nargs="+", metavar="NAME")
    mode.add_argument("--rate", nargs="+", metavar="NAME")
    mode.add_argument("--follow", action="store_true")
    mode.add_argument("--summary", metavar="FILE")
    mode.add_argument("--self-test", action="store_true")
    parser.add_argument("--count", type=int, default=0, help="--follow: stop after N readings")
    parser.add_argument("--timeout", type=float, default=10, help="seconds to wait for a reading")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.summary:
        with open(args.summary, encoding="utf-8") as f:
            print(summary(f))
        return 0

    reader = Reader(args.socket or find_socket(), args.timeout)
    try:
        if args.follow:
            n = 0
            while args.count == 0 or n < args.count:
                line = reader.line()
                check(json.loads(line))
                sys.stdout.write(line + "\n")
                sys.stdout.flush()
                n += 1
            return 0
        first = reader.reading()
        if args.once:
            print(json.dumps(first, indent=1, ensure_ascii=False))
        elif args.rows:
            print(rows_text(first))
        elif args.get:
            missing = [name for name in args.get if name not in first["counters"]]
            for name in args.get:
                print("%s %s" % (name, first["counters"].get(name, "-")))
            return 1 if missing else 0
        elif args.rate:
            second = reader.reading()
            for name in args.rate:
                value = rate(first, second, name)
                print("%s %s" % (name, "-" if value is None else "%.3f" % value))
        return 0
    finally:
        reader.close()


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
