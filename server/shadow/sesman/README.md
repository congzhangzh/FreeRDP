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

## Test

`test/run-e2e.sh` runs it against GNOME Shell 50 in a systemd container (see its
header): 14 checks.
