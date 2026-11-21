# freerdp-shadow-sesman

Route each RDP connection to the GNOME session of the user it names.

```
client --TCP 3389--> freerdp-shadow-sesman (root) --unix socket--> freerdp-shadow-cli
                                                        (in the user's systemd manager,
                                                         sharing the user's GNOME Shell
                                                         through Mutter's ScreenCast and
                                                         RemoteDesktop APIs)
```

The client's first packet (the X.224 Connection Request) names the user in its
cookie, `Cookie: mstshash=<user>`. The session manager reads that packet and nothing else:

1. finds the user (or the one local user whose name starts with the cookie,
   since some clients cut it short);
2. finds the user's GNOME Shell; without one, users of `--headless-group` get a
   headless GNOME session (a system unit with a PAM session, so logind registers
   it and the user's systemd manager stays up), others are refused;
3. starts FreeRDP's shadow server (Mutter backend) in the user's own systemd
   manager unless it already listens on `/run/user/<uid>/freerdp-sesman-shadow.sock`;
4. hands it the packet, drops to the user and copies the bytes both ways.

TLS and the login (PAM, the session's owner only) happen in the shadow server:
the session manager never sees a password, and every client works (nothing is
redirected).

## Build

`-DWITH_SHADOW_MUTTER=ON -DWITH_SHADOW_SESMAN=ON` (Linux). Headless sessions
use the Mutter backend's virtual monitor (`FREERDP_SHADOW_MUTTER_VIRTUAL`), so
the clients draw the cursor. Needs Mutter >= 45, systemd (logind, user
managers, `systemd-run --machine=<user>@.host`) and PAM.

## Headless sessions

A headless GNOME started by the session manager keeps running after the client leaves,
so the next connection finds the same desktop. It ends when the user logs out
in it, or with `systemctl stop freerdp-sesman-gnome-<user>`.

An update of the shadow server takes effect at the next connection: the session manager
sees that the running program is gone from disk and restarts it. The desktop stays; a client
connected to the old server is dropped and reconnects.

A session started this way has no seat, and polkit takes a session with no seat for a
remote one: what a desktop on a seat may simply do asks for an administrator. GNOME
Software refreshes and updates in the background, so it would bring password dialogs
nobody asked for, and those take the keyboard. It starts in two ways, and the session
manager closes both for these sessions: `/run/freerdp-sesman/xdg` first in
`XDG_CONFIG_DIRS` keeps it out of the autostart, and `systemctl --user mask --runtime`
keeps `gnome-software.service` from starting, until the user's manager ends. So it does
not open from the menu or show up in a search of Activities either. `gnome-software` in
a terminal still starts it, and it then asks like any other program.

## Security notes

- The session manager runs as root, reads at most one 1 KiB packet from a client before
  routing it, and drops to the user before relaying.
- Without `--headless-group`, only users with a running GNOME Shell are
  reachable. With it, a client that only names a user of that group starts a
  headless GNOME for them *before* any login (the login happens in the shadow
  server): keep the group small, or the port private (SSH tunnel, VPN).

## Use

```
freerdp-shadow-sesman [--port 3389] [--bind <address>] [--shadow <freerdp-shadow-cli>]
           [--headless-group <group>] [--headless-size 1920x1080]
           [-- <shadow server options>]
```

`freerdp-shadow-sesman.service` (installed with the server component) runs it as a system service.

The shadow servers it starts get every `FREERDP_SHADOW_*` variable of its own environment
(`Environment=` in a drop-in for the service), up to 32, but `FREERDP_SHADOW_MUTTER_VIRTUAL`,
which it sets for each user itself. A variable it does not pass on is logged. The encoder
settings are one of them, `FREERDP_SHADOW_CODEC`, with the value of `freerdp-shadow-cli`'s
`/codec` (keys separated by commas, each `<codec>:<key>:<value>`; the keys it does not name keep
their defaults), e.g.

```
Environment=FREERDP_SHADOW_CODEC=av1:rate-control:cq,av1:cq-level:26,av1:bitrate:4000,av1:threads:8
```

| Key | Default | Meaning |
|---|---|---|
| `av1:rate-control` | `cq` | `q` constant quality, `cq` the quality up to the bit rate, `cbr`, `vbr` |
| `av1:bitrate` | 12000 | kbit/s, the ceiling of `cq` and the target of `cbr` and `vbr` |
| `av1:cq-level` | 16 | libaom's quality, 0 (libaom's) to 63, lower is better |
| `av1:threads` | 0 | 0 for up to 4 |
| `av1:cpu-used` | 10 | 0 (slowest) to 12 |
| `av1:tile-columns` | 2 | log2, 0 to 6: 2 is four columns, 0 one |
| `av2:rate-control` | `cq` | as for AV1 |
| `av2:bitrate` | 10000 | kbit/s, as for AV1 |
| `av2:qp` | 100 | libavm's quantizer, 0 (libavm's) to 255, lower is better |
| `av2:threads` | up to 4 | 1 to 64 |
| `av2:cpu-used` | 10 | 0 (slowest) to 10 |
| `av2:tile-columns` | 1 | log2, 0 to 6: 1 is two columns |
| `av2:dpb-size` | 4 | reference frames, 1 to 16; libavm keeps 8, each a whole frame |
| `h264:rate-control` | `vbr` | `vbr` or `cqp` (H.264 has no other mode) |
| `h264:bitrate` | 10000 | kbit/s, for `vbr` |
| `h264:qp` | 0 | 0 to 51, for `cqp` |

A value with a wrong key changes nothing and is logged.

The sound has settings of its own:

| Variable | Default | Meaning |
|---|---|---|
| `FREERDP_SHADOW_SOUND_PACKET_MS` | 50 | the milliseconds of sound in one wave, 10 to 200 |
| `FREERDP_SHADOW_SOUND_DELAY_LOG` | 30 | seconds between the `sound delay:` lines of each client, 0 for none |

A `sound delay:` line tells how long a wave took from the capture to the client and back (the
client answers each wave when it arrives), and how long the client keeps a wave before it plays it
(its second answer); a client with a sound delay shows it there.

The libraries the shadow server loads read settings of their own. `FREERDP_SHADOW_ENV_<NAME>=<value>`
becomes `<NAME>=<value>` in the shadow server, unless `<NAME>` is set there already, so sesman (and
`shadow.conf`) can pass them on, e.g. a smaller PipeWire buffer for the sound it captures:

```
Environment=FREERDP_SHADOW_ENV_PIPEWIRE_LATENCY=256/48000
```

The shadow server logs each one it sets. It sets them while it reads its settings, before the
capture starts; settings read before that (`WLOG_*`) do not take effect this way.

## Test

`TestSesmanEnv` (ctest) checks which settings are passed on. `test/run-e2e.sh` runs it
against GNOME Shell 50 in a systemd container (see its header): 14 checks.
