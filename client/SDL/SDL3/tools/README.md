# SDL client tools

## sdl-stats-ipc.py: measure the real client

`sdl-freerdp /sdl-stats-ipc[:<path>]` sends the numbers of the stats and info overlay to a local
socket (a named pipe on Windows), once a second, whether the overlay shows or not: one line of JSON per reading
(`"schema": "freerdp-sdl-stats"`, `"version": 1`). `counters` holds every runtime statistic of
the client by name, as exact integers (frames, bytes, decode time sums, RTT, audio delay, ...);
`rows` holds the overlay's own label and value strings. Only the user that runs the client can
connect; what a reader sends is ignored. Without a path the socket is
`$XDG_RUNTIME_DIR/sdl-freerdp-<pid>.stats.sock` (`$TMPDIR` on macOS) and on Windows the pipe
`\\.\pipe\sdl-freerdp-<pid>.stats` (a bare name given on Windows gets the `\\.\pipe\` prefix); the
client logs it at start. A socket path has at most 107 bytes on Linux (103 on macOS), the length of an
AF_UNIX address: the default directories are short, and a longer path given with the option is refused with that reason.

To measure a change A/B, run the same session the same way twice, once per build or setting,
and record a fixed stretch of each: `sdl-stats-ipc.py <socket> --follow --count 60 > a.jsonl`,
then `b.jsonl`. `sdl-stats-ipc.py --summary a.jsonl` prints each counter's first and last value,
its change and its rate per second over the run (decode time per frame is
`RDPGFX_CODECID_<codec>_DECODE_NS_SUM` change over `_PICTURES` change); compare the two
summaries. Counters start over on a reconnect (`connection` grows), so a summary covers the
first connection only. For a quick look: `--rows` (what the overlay shows), `--get NAME...`,
`--rate NAME...`. The script needs only Python 3; `--self-test` checks it without a client.
On Windows the script reads the pipe with plain `open()` (PowerShell: `NamedPipeClientStream`) and
checks that the pipe's server is the process its name says (`GetNamedPipeServerProcessId`), so a
program that took the name first is refused; reads there have no timeout.

To see which stage is slow, record the client's lines (`--follow > run.jsonl`) and a FreeRDP
shadow server's own statistics for the same time (`freerdp-shadow-stats` lines, `server.jsonl`),
then `sdl-stats-ipc.py --join run.jsonl server.jsonl`: per server period it prints what the
server captured, sent, held and how long it encoded and waited for acknowledgements, beside the
client's frame rate, decode time per frame and bandwidth for the same frames. The two are lined up
by the server's frame ids (`GFX_FRAME_ID_FIRST`/`_LAST` there, `RDPGFX_FRAME_ID_LAST` here), never
by clock time: two machines' clocks differ. The connection is the one in the client's
`strings.SERVER_CONNECTION_ID` (sent over the server's statistics channel), else the server's only
connection, else the one given with `--connection`. `time_ms` and `started_at` (each machine's own
clock) are printed for people only.
