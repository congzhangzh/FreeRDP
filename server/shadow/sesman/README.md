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

The shadow servers it starts take the AV1 and AV2 encoder settings from these variables
of its own environment (`Environment=` in a drop-in for the service), as `freerdp-shadow-cli`
does from its environment:

| Variable | Default | Meaning |
|---|---|---|
| `FREERDP_SHADOW_AV1_CQ_LEVEL` | 16 | libaom's quality, 1 to 63 (lower is better) |
| `FREERDP_SHADOW_AV1_TARGET_BITRATE` | 12000 | kbit/s, the ceiling of `cq` and the target of `cbr` and `vbr`; 0 for `q` |
| `FREERDP_SHADOW_AV1_RATE_CONTROL` | `cq` (`q` without a target) | `q` constant quality, `cq` the quality up to the target, `cbr`, `vbr` |
| `FREERDP_SHADOW_AV1_THREADS` | 0 | 0 for up to 4 |
| `FREERDP_SHADOW_AV1_CPU_USED` | 10 | 0 (slowest) to 12 |
| `FREERDP_SHADOW_AV2_QP` | 100 | libavm's quantizer, 1 to 255 (lower is better) |
| `FREERDP_SHADOW_AV2_TARGET_BITRATE` | 10000 | kbit/s, the ceiling of `cq` and the target of `cbr` and `vbr`; 0 for `q` |
| `FREERDP_SHADOW_AV2_RATE_CONTROL` | `cq` (`q` without a target) | as for AV1 |
| `FREERDP_SHADOW_AV2_THREADS` | up to 4 | |
| `FREERDP_SHADOW_AV2_CPU_USED` | 10 | 0 (slowest) to 10 |
| `FREERDP_SHADOW_AV2_TILE_COLUMNS` | 1 | log2: 1 is two columns |
| `FREERDP_SHADOW_AV2_DPB_SIZE` | 4 | reference frames, 1 to 16; libavm keeps 8, each a whole frame |

## Test

`test/run-e2e.sh` runs it against GNOME Shell 50 in a systemd container (see its
header): 14 checks.
