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
  sdl-stats-ipc.py --join run.jsonl server.jsonl [--connection ID]
                                              the client's lines beside a FreeRDP shadow server's
                                              statistics, per server period, lined up by frame id

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
SERVER_SCHEMA = "freerdp-shadow-stats"
SERVER_VERSION = 1


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


def frame_diff(a, b):
    """a - b for 32-bit frame ids that wrap."""
    d = (a - b) & 0xFFFFFFFF
    return d - 0x100000000 if d >= 0x80000000 else d


def server_periods(lines):
    """The server's periods per connection id, oldest first, each once."""
    periods = {}
    for line in lines:
        if not line.strip():
            continue
        top = json.loads(line)
        if top.get("schema") != SERVER_SCHEMA:
            raise SystemExit("not %s: %r" % (SERVER_SCHEMA, top.get("schema")))
        if top.get("version") != SERVER_VERSION:
            raise SystemExit("server schema version %r, this reader knows %d" % (top.get("version"), SERVER_VERSION))
        for client in top.get("clients", []):
            if not client.get("periods"):
                continue
            periods.setdefault(client["connection_id"], {})[client["periods"]] = client
    return {cid: [p[k] for k in sorted(p)] for cid, p in periods.items()}


def pick_connection(readings, periods, wanted):
    ids = {r.get("strings", {}).get("SERVER_CONNECTION_ID") for r in readings} - {None}
    if wanted:
        if wanted not in periods:
            raise SystemExit("the server has no connection %s: %s" % (wanted, ", ".join(sorted(periods)) or "none"))
        return wanted, "given"
    if len(ids) == 1:
        cid = ids.pop()
        if cid not in periods:
            raise SystemExit("the client's connection %s is not in the server's lines: %s" % (cid, ", ".join(sorted(periods)) or "none"))
        return cid, "the client's SERVER_CONNECTION_ID"
    if len(ids) > 1:
        raise SystemExit("the client's lines span several connections: %s; give --connection" % ", ".join(sorted(ids)))
    if len(periods) == 1:
        return next(iter(periods)), "the server's only connection (the client has no SERVER_CONNECTION_ID: /astats off?)"
    raise SystemExit("no SERVER_CONNECTION_ID in the client's lines and the server has %d connections: give --connection" % len(periods))


def decode_totals(counters):
    """(decode ns, pictures) summed over every codec."""
    ns = pictures = 0
    for name, value in counters.items():
        if name.startswith("RDPGFX_CODECID_") and name.endswith("_DECODE_NS_SUM"):
            ns += value
            pictures += counters.get(name[: -len("_DECODE_NS_SUM")] + "_PICTURES", 0)
    return ns, pictures


def client_window(readings, first, last):
    """The client lines around the frames first..last: the last one that had not received
    first yet, and the first one that had received last. None when the lines do not reach."""
    before = after = None
    for r in readings:
        c = r["counters"]
        if c.get("RDPGFX_FRAMES_DECODED", 0) == 0:
            before = r  # nothing received yet: before every frame
            continue
        got = c.get("RDPGFX_FRAME_ID_LAST", 0)
        if frame_diff(got, first) < 0:
            before = r
        elif after is None and frame_diff(got, last) >= 0:
            after = r
    if before is None or after is None or frame_diff(after["ns"], before["ns"]) == 0:
        return None
    return before, after


def join(client_lines, server_lines, wanted=None):
    readings = [json.loads(line) for line in client_lines if line.strip()]
    for r in readings:
        check(r)
    if not readings:
        raise SystemExit("no client readings")
    periods = server_periods(server_lines)
    cid, why = pick_connection(readings, periods, wanted)
    # one connection of the client's: the one that carries the id, else the first
    conn = next((r["connection"] for r in readings if r.get("strings", {}).get("SERVER_CONNECTION_ID") == cid), readings[0]["connection"])
    readings = [r for r in readings if r["connection"] == conn]
    first_server = periods[cid][0]
    out = [
        "connection %s (%s)" % (cid, why),
        "client started_at %s, server started_at %s (each machine's own clock: for people, not used to join)"
        % (readings[0].get("started_at") or "-", first_server.get("started_at") or "-"),
        "lined up by frame id; the client reads once a second, so its window is the frames received between two of its lines",
        "",
        "%-4s %-19s | %7s %7s %5s %5s %9s %13s %9s %15s %7s | %-19s %7s %9s %8s | %9s"
        % ("per", "server frames", "capt/s", "sent/s", "held", "merg", "capture", "encode avg/mx", "round", "ack min/avg/mx", "flight",
           "client frames", "fps", "decode/f", "kbit/s", "net+queue"),
    ]
    for p in periods[cid]:
        c = p["counters"]
        seconds = (p.get("period_ms") or c.get("GFX_PERIOD_MS") or 0) / 1000.0
        per = lambda v: "%.1f" % (v / seconds) if seconds > 0 else "-"
        first, last = c.get("GFX_FRAME_ID_FIRST"), c.get("GFX_FRAME_ID_LAST")
        def ms(name):
            return "%.1f ms" % (c[name] / 1000) if name in c else "-"

        server = "%-4s %-19s | %7s %7s %5s %5s %9s %13s %9s %15s %7s" % (
            p["periods"], "%s-%s" % (first, last) if first is not None else "-",
            per(c.get("GFX_CAPTURED", 0)), per(c.get("GFX_SENT", 0)), c.get("GFX_HELD", "-"), c.get("GFX_MERGED", "-"),
            ms("GFX_CAPTURE_AVG_US"),
            "%.1f/%.1f ms" % (c.get("GFX_ENCODE_AVG_US", 0) / 1000, c.get("GFX_ENCODE_MAX_US", 0) / 1000),
            ms("GFX_ROUND_AVG_US"),
            "%s/%s/%s ms" % (c.get("GFX_ACK_DELAY_MIN_MS", "-"), c.get("GFX_ACK_DELAY_AVG_MS", "-"), c.get("GFX_ACK_DELAY_MAX_MS", "-")),
            "%s/%s" % (c.get("GFX_IN_FLIGHT_MAX", "-"), c.get("GFX_IN_FLIGHT_LIMIT", "-")))
        if first is None or last is None:
            out.append(server + " | %-19s" % "(no frames sent)")
            continue
        window = client_window(readings, first, last)
        if window is None:
            out.append(server + " | %-19s" % "(no client lines)")
            continue
        a, b = window
        ca, cb = a["counters"], b["counters"]
        cs = (b["ns"] - a["ns"]) / 1e9
        frames = cb.get("RDPGFX_FRAMES_DECODED", 0) - ca.get("RDPGFX_FRAMES_DECODED", 0)
        ns0, pic0 = decode_totals(ca)
        ns1, pic1 = decode_totals(cb)
        decode = (ns1 - ns0) / (pic1 - pic0) / 1e6 if pic1 > pic0 else None
        kbits = (cb.get("NETWORK_BYTES_RECEIVED", 0) - ca.get("NETWORK_BYTES_RECEIVED", 0)) * 8 / 1000 / cs
        span = "%s-%s" % ((ca.get("RDPGFX_FRAME_ID_LAST", 0) + 1) & 0xFFFFFFFF if ca.get("RDPGFX_FRAMES_DECODED", 0) else "start",
                          cb.get("RDPGFX_FRAME_ID_LAST", "-"))
        avg = c.get("GFX_ACK_DELAY_AVG_MS")
        rest = "%.1f ms" % (avg - decode) if (avg is not None and decode is not None) else "-"
        out.append(server + " | %-19s %7.1f %9s %8.0f | %9s" % (
            span, frames / cs, "%.2f ms" % decode if decode is not None else "-", kbits, rest))
    out.append("")
    out.append("net+queue = the server's mean ack delay minus the client's mean decode time: what the network,")
    out.append("the queues and drawing took (derived, approximate: the two means cover slightly different frames)")
    out.append("capture, round = the server's mean capture time and capture-to-sent time per frame (\"-\": not reported)")
    return "\n".join(out)


def join_self_test():
    """Synthetic lines: frame ids that wrap, a repeated period, no id and a given id."""
    def client(seq, ns, last, decoded, dec_ns, pics, nbytes, cid="00ab00cd00ef0012"):
        strings = {"SERVER_CONNECTION_ID": cid} if cid else {}
        return json.dumps({"schema": SCHEMA, "version": VERSION, "seq": seq, "ns": ns, "time_ms": 0,
                           "connection": 1, "started_at": "2026-11-21T10:31:05-05:00",
                           "counters": {"RDPGFX_FRAMES_DECODED": decoded, "RDPGFX_FRAME_ID_LAST": last,
                                        "RDPGFX_CODECID_AV1_DECODE_NS_SUM": dec_ns,
                                        "RDPGFX_CODECID_AV1_PICTURES": pics, "NETWORK_BYTES_RECEIVED": nbytes},
                           "strings": strings, "rows": {}})

    def server(periods_, first, last, cid="00ab00cd00ef0012"):
        return json.dumps({"schema": SERVER_SCHEMA, "version": SERVER_VERSION, "seq": periods_, "time_ms": 0, "pid": 1,
                           "clients": [{"connection_id": cid, "periods": periods_, "period_ms": 5000,
                                        "started_at": "2026-11-21T15:31:05+00:00",
                                        "counters": {"GFX_CAPTURED": 300, "GFX_SENT": 250, "GFX_FRAME_ID_FIRST": first,
                                                     **({"GFX_CAPTURE_AVG_US": 15000, "GFX_ROUND_AVG_US": 40000} if periods_ == 2 else {}),
                                                     "GFX_FRAME_ID_LAST": last, "GFX_ACK_DELAY_AVG_MS": 12,
                                                     "GFX_ENCODE_AVG_US": 8000}}]})

    base = 0xFFFFFFF0  # the frame ids wrap inside the second period
    c = [client(1, 0, 0, 0, 0, 0, 0)]
    for k in range(1, 12):  # one line a second, 50 frames a second, 2 ms decode each
        c.append(client(k + 1, k * 1_000_000_000, (base + 50 * k - 1) & 0xFFFFFFFF, 50 * k, 2_000_000 * 50 * k, 50 * k, 125_000 * k))
    s = [server(1, base, (base + 249) & 0xFFFFFFFF), server(1, base, (base + 249) & 0xFFFFFFFF),
         server(2, (base + 250) & 0xFFFFFFFF, (base + 499) & 0xFFFFFFFF)]
    text = join(c, s)
    rows = [line for line in text.splitlines() if line[:4].strip() in ("1", "2")]
    assert len(rows) == 2, text  # the repeated period once
    assert "the client's SERVER_CONNECTION_ID" in text, text
    for row in rows:
        assert " 50.0 " in row and "2.00 ms" in row and "10.0 ms" in row and " 1000 " in row, row
    assert "start-" in rows[0] and "4294967280-" not in rows[1], rows  # the second window starts after the wrap
    assert "15.0 ms" in rows[1] and "40.0 ms" in rows[1] and "15.0 ms" not in rows[0], rows  # capture, round when reported
    # no id on the client side: the server's only connection, said so
    text = join([line.replace('"SERVER_CONNECTION_ID": "00ab00cd00ef0012"', "") for line in c], s)
    assert "only connection" in text, text
    # two server connections and no id: refused; with --connection: fine
    two = s + [server(1, 5, 9, cid="1111111111111111")]
    try:
        join([line.replace('"SERVER_CONNECTION_ID": "00ab00cd00ef0012"', "") for line in c], two)
        raise AssertionError("two connections without an id must be refused")
    except SystemExit:
        pass
    assert "(given)" in join(c, two, "00ab00cd00ef0012")
    # a period that sent nothing has no frame ids
    idle = json.loads(s[2])
    idle["clients"][0]["periods"] = 3
    for k in ("GFX_FRAME_ID_FIRST", "GFX_FRAME_ID_LAST"):
        del idle["clients"][0]["counters"][k]
    assert "(no frames sent)" in join(c, s + [json.dumps(idle)])
    assert frame_diff(5, 0xFFFFFFFE) == 7 and frame_diff(0xFFFFFFFE, 5) == -7


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
    join_self_test()
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
    mode.add_argument("--join", nargs=2, metavar=("CLIENT", "SERVER"))
    mode.add_argument("--self-test", action="store_true")
    parser.add_argument("--connection", help="--join: the server's connection_id to use")
    parser.add_argument("--count", type=int, default=0, help="--follow: stop after N readings")
    parser.add_argument("--timeout", type=float, default=10, help="seconds to wait for a reading")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.join:
        with open(args.join[0], encoding="utf-8") as fc, open(args.join[1], encoding="utf-8") as fs:
            print(join(fc.readlines(), fs.readlines(), args.connection))
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
