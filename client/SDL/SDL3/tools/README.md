# SDL client tools

## sdl-stats-ipc.py: measure the real client

`sdl-freerdp /sdl-stats-ipc[:<path>]` sends the numbers of the stats and info overlay to a local
socket, once a second, whether the overlay shows or not: one line of JSON per reading
(`"schema": "freerdp-sdl-stats"`, `"version": 1`). `counters` holds every runtime statistic of
the client by name, as exact integers (frames, bytes, decode time sums, RTT, audio delay, ...);
`rows` holds the overlay's own label and value strings. Only the user that runs the client can
connect; what a reader sends is ignored. Without a path the socket is
`$XDG_RUNTIME_DIR/sdl-freerdp-<pid>.stats.sock` (`$TMPDIR` on macOS,
`%LOCALAPPDATA%\FreeRDP` on Windows), and the client logs it at start.

To measure a change A/B, run the same session the same way twice, once per build or setting,
and record a fixed stretch of each: `sdl-stats-ipc.py <socket> --follow --count 60 > a.jsonl`,
then `b.jsonl`. `sdl-stats-ipc.py --summary a.jsonl` prints each counter's first and last value,
its change and its rate per second over the run (decode time per frame is
`RDPGFX_CODECID_<codec>_DECODE_NS_SUM` change over `_PICTURES` change); compare the two
summaries. Counters start over on a reconnect (`connection` grows), so a summary covers the
first connection only. For a quick look: `--rows` (what the overlay shows), `--get NAME...`,
`--rate NAME...`. The script needs only Python 3; `--self-test` checks it without a client.
Windows' Python has no AF_UNIX sockets: read the socket there with another program.
