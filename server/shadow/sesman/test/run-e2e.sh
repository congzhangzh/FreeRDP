#!/bin/bash
# freerdp-shadow-sesman e2e v2 (v1 + the headless GNOME has no monitor: the shadow server adds a virtual one,
# cursor as metadata; a fresh shadow server for a client that arrives at once), in a systemd container with GNOME Shell 50 (headless):
#
#   xfreerdp3 (Xvfb) --TCP 3389--> freerdp-shadow-sesman (root) --unix socket--> freerdp-shadow-cli
#       (in tester's systemd manager) --D-Bus/PipeWire/libei--> tester's gnome-shell
#
# The image: GNOME Shell 50 with PipeWire, WirePlumber, libei, Xvfb, xfreerdp3,
# foot, ImageMagick and systemd as PID 1, e.g. the rdp-release-kit's
# test/linux-host/Dockerfile. From the host:
#   podman run -d --name sesman-e2e --systemd=always -v <shadow build>:<same>:ro \
#       -v <freerdp-shadow-sesman>:<same>:ro -v <out>:/out -v <this dir>:/t:ro \
#       <image> /usr/lib/systemd/systemd
#   podman exec sesman-e2e bash /t/run-e2e.sh <freerdp-shadow-cli> /out <freerdp-shadow-sesman>
SHADOW=$1
SESMAN=$3
OUT=$2
W=1280
H=800
PASSWORD=shadow-test-pw
RUN=/run/user/1000
mkdir -p "$OUT"
chmod 777 "$OUT"

echo "tester:$PASSWORD" | chpasswd
for _ in $(seq 30); do
    systemctl is-system-running 2>/dev/null | grep -q -E 'running|degraded' && break
    sleep 1
done
# A rootless container cannot start polkitd ("Failed at step USER"): each D-Bus activation of it
# then waits 25 s (accounts-daemon, upowerd, GNOME Shell), and a headless GNOME may miss
# sesman's wait. Masked, the activations fail at once.
systemctl mask --runtime polkit.service > /dev/null 2>&1
loginctl enable-linger tester
for _ in $(seq 30); do
    [ -S $RUN/bus ] && break
    sleep 1
done

# runs a command as tester, in the session
as() {
    su tester -s /bin/bash -c "export XDG_RUNTIME_DIR=$RUN \
        DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus WAYLAND_DISPLAY=wayland-0 HOME=/home/tester; $*"
}

has_name() {
    as "gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner $1" 2>/dev/null | grep -q true
}

# --- GNOME Shell, headless, in tester's user manager ---------------------------
as "systemctl --user start pipewire.socket pipewire.service wireplumber.service" > "$OUT/units.log" 2>&1
as "systemd-run --user --unit=shell-under-test --setenv=XDG_SESSION_TYPE=wayland \
    gnome-shell --headless --unsafe-mode --virtual-monitor ${W}x${H} --virtual-monitor 1024x768" >> "$OUT/units.log" 2>&1
for _ in $(seq 60); do
    has_name org.gnome.Mutter.RemoteDesktop && break
    sleep 1
done
has_name org.gnome.Mutter.RemoteDesktop && echo "GNOME Shell is up" || echo "GNOME Shell did NOT come up"
# the shell answers D-Bus calls only once its start-up is over
for _ in $(seq 60); do
    t0=$(date +%s%N)
    as "gdbus call --session --dest org.gnome.Mutter.DisplayConfig --object-path /org/gnome/Mutter/DisplayConfig \
        --method org.gnome.Mutter.DisplayConfig.GetCurrentState" > /dev/null 2>&1
    t1=$(date +%s%N)
    [ $(( (t1 - t0) / 1000000 )) -lt 1000 ] && break
done
echo "GNOME Shell answers ($(( (t1 - t0) / 1000000 )) ms)"

# --- users: one without GNOME, one allowed a headless GNOME (long name) -----------
useradd -m other 2>/dev/null; echo "other:other-pw" | chpasswd
groupadd rdp-headless 2>/dev/null
useradd -m -G rdp-headless longusername1 2>/dev/null; echo "longusername1:long-pw" | chpasswd

# --- freerdp-shadow-sesman, as root --------------------------------------------------------
nohup stdbuf -oL -eL $SESMAN --port 3389 --shadow $SHADOW --headless-group rdp-headless \
    --headless-size 1280x800 -- /log-level:INFO > "$OUT/sesman.log" 2>&1 &
SESMAN_PID=$!
sleep 1

Xvfb :99 -noreset -screen 0 1600x1000x24 > /dev/null 2>&1 &
export DISPLAY=:99
unset WAYLAND_DISPLAY
sleep 2

# a bare Connection Request, as a client that sends this cookie would
raw_cr() { # cookie-line or "" ; prints what came back (bytes)
    python3 - "$1" <<'PY'
import socket, sys
cookie = sys.argv[1].encode()
neg = bytes([1, 0, 8, 0, 3, 0, 0, 0])            # RDP_NEG_REQ: TLS | CredSSP
x224 = bytes([0xE0, 0, 0, 0, 0, 0]) + (cookie + b"\r\n" if cookie else b"") + neg
pkt = bytes([3, 0]) + (4 + 1 + len(x224)).to_bytes(2, "big") + bytes([len(x224)]) + x224
s = socket.create_connection(("127.0.0.1", 3389), timeout=8)
s.sendall(pkt)
try:
    data = s.recv(64)
except Exception as e:
    data = b""
print(len(data))
PY
}
connect() { # name user password log
    xfreerdp3 /v:127.0.0.1:3389 /u:$2 /p:$3 /cert:ignore /sec:tls /size:1280x800 /bpp:32 \
        > "$OUT/client-$1.log" 2>&1 &
    eval "CLIENT_$1=\$!"
}

# 1. tester, who runs GNOME: their session
connect tester tester "$PASSWORD"
sleep 6
# an idle GNOME may send no full frame for a while after the stream starts
# (Mutter skips it as "too early" after the cursor frame): make the screen change
as "nohup foot > /tmp/foot.log 2>&1 &"
sleep 4
import -window root "$OUT/1-tester.png"
# 2. a second client of tester: the same shadow server
connect tester2 tester "$PASSWORD"
sleep 6
as "journalctl --user --no-pager -o cat -u freerdp-sesman-shadow" > "$OUT/shadow-tester.log" 2>&1
kill $CLIENT_tester $CLIENT_tester2 2>/dev/null
sleep 2

# tester's checks are done: stop their GNOME and its services. In a rootless
# container every process counts against the host user's inotify instances
# (fs.inotify.max_user_instances, 128 by default), and a second GNOME stack
# (the headless one below) would not fit next to it: its user manager fails
# with "Failed to create control group inotify object: Too many open files"
as "systemctl --user stop shell-under-test pipewire.socket pipewire.service wireplumber.service" >> "$OUT/units.log" 2>&1
sleep 2

# 3. other: no GNOME session, not in the headless group
connect other other other-pw
sleep 5
kill $CLIENT_other 2>/dev/null
# 4. no such user, 5. no cookie
echo "ghost: $(raw_cr 'Cookie: mstshash=ghost') bytes back" > "$OUT/raw.txt"
echo "no cookie: $(raw_cr '') bytes back" >> "$OUT/raw.txt"
# 6. a cookie cut short (mstsc keeps 9 characters) names longusername1 alone;
#    it is in rdp-headless and has no session: a headless GNOME starts
echo "longusern: $(raw_cr 'Cookie: mstshash=longusern') bytes back" >> "$OUT/raw.txt"
# 7. then longusername1 logs in for real and gets that headless session
for _ in $(seq 60); do
    pgrep -u longusername1 -x gnome-shell > /dev/null && break
    sleep 1
done
sleep 25   # GNOME Shell's start-up (its first D-Bus answers stall while it starts)
connect long longusername1 long-pw
sleep 30
su longusername1 -s /bin/bash -c "XDG_RUNTIME_DIR=/run/user/$(id -u longusername1) WAYLAND_DISPLAY=wayland-0 nohup foot > /tmp/foot-long.log 2>&1 &"
sleep 5
import -window root "$OUT/7-long.png"
kill $CLIENT_long 2>/dev/null
sleep 2
su longusername1 -s /bin/bash -c "XDG_RUNTIME_DIR=/run/user/$(id -u longusername1) \
    journalctl --user --no-pager -o cat -u freerdp-sesman-shadow" > "$OUT/shadow-long.log"; journalctl --no-pager -o cat -u freerdp-sesman-gnome-longusername1 > "$OUT/gnome-long.log" 2>&1; loginctl list-sessions > "$OUT/sessions.txt" 2>&1 2>&1
# 8. fresh start on the virtual monitor: stop longusername1's shadow server, connect at once
LUID=$(id -u longusername1)
systemctl --machine=longusername1@.host --user stop freerdp-sesman-shadow > /dev/null 2>&1
sleep 1
connect fresh longusername1 long-pw
sleep 12
import -window root "$OUT/8-fresh.png"
su longusername1 -s /bin/bash -c "XDG_RUNTIME_DIR=/run/user/$LUID WAYLAND_DISPLAY=wayland-0 nohup foot > /tmp/foot-fresh.log 2>&1 &"
sleep 4
import -window root "$OUT/8-fresh-foot.png"
FRESH_PX=$(compare -metric AE "$OUT/8-fresh.png" "$OUT/8-fresh-foot.png" null: 2>&1 | cut -d' ' -f1 | cut -d. -f1)
kill $CLIENT_fresh 2>/dev/null
sleep 2
su longusername1 -s /bin/bash -c "XDG_RUNTIME_DIR=/run/user/$LUID \
    journalctl --user --no-pager -o cat -u freerdp-sesman-shadow" > "$OUT/shadow-long-all.log" 2>&1
journalctl --no-pager -o short-precise > "$OUT/journal.log" 2>&1
kill $SESMAN_PID 2>/dev/null

# --- result ----------------------------------------------------------------------
pass=0
fail=0
check() {
    if eval "$2"; then
        echo "PASS $1"
        pass=$((pass + 1))
    else
        echo "FAIL $1"
        fail=$((fail + 1))
    fi
}
check "tester routed to their session" "grep -q 'tester -> the shadow server' $OUT/sesman.log"
check "tester's shadow server: Mutter, activated" "grep -q 'subsystem: Mutter\|sharing monitor' $OUT/shadow-tester.log && grep -q 'is activated' $OUT/shadow-tester.log"
check "two clients of tester, one shadow server" "[ \$(grep -c 'is activated' $OUT/shadow-tester.log) -ge 2 ] && [ \$(grep -c 'starting the shadow server for tester' $OUT/sesman.log) -eq 1 ]"
check "tester's picture is not black" "[ \$(convert $OUT/1-tester.png -crop 1280x800+0+0 -format '%[fx:mean>0.05]' info:) = 1 ]"
check "other (no GNOME) refused" "grep -q 'other has no GNOME session: refused' $OUT/sesman.log"
check "unknown user refused" "grep -q \"no user 'ghost': refused\" $OUT/sesman.log && grep -q '^ghost: 0 bytes' $OUT/raw.txt"
check "no cookie refused" "grep -q 'named no user' $OUT/sesman.log && grep -q '^no cookie: 0 bytes' $OUT/raw.txt"
check "cut-short cookie -> longusername1, headless GNOME started" "grep -q 'longusername1 has no GNOME session: starting a headless one' $OUT/sesman.log"
check "longusername1 gets the headless session" "grep -q 'longusername1 -> the shadow server' $OUT/sesman.log && grep -q 'is activated' $OUT/shadow-long.log"
check "longusername1's picture is not black" "[ \$(convert $OUT/7-long.png -crop 1280x800+0+0 -format '%[fx:mean>0.05]' info:) = 1 ]"
check "headless: a virtual monitor of the shadow server's own" "grep -q 'sharing a virtual monitor of its own: 1280x800' $OUT/shadow-long.log && grep -q 'RecordVirtual\\|virtual monitor' $OUT/shadow-long.log"
check "headless GNOME started without a monitor" "! grep -q 'virtual-monitor' $OUT/gnome-long.log"
check "fresh start on the virtual monitor: picture" "[ \$(convert $OUT/8-fresh.png -crop 1280x800+0+0 -format '%[fx:mean>0.05]' info:) = 1 ]"
check "fresh start on the virtual monitor: a new window reaches the client" "[ \"\${FRESH_PX:-0}\" -gt 10000 ] 2>/dev/null"
echo "passed $pass, failed $fail"
