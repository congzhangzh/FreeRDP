# macOS shadow server

The shadow server shares the Mac's own screen (it does not start a new session). This backend
captures with ScreenCaptureKit (macOS 12.3; sound from macOS 13), posts input with Quartz events,
and lets the clients draw the cursor.

## Two ways to run it

| | In a user's session | As a daemon with agents |
|---|---|---|
| Who runs what | `freerdp-shadow-cli`, started by the user (or a LaunchAgent) | `freerdp-shadow-cli` as a root LaunchDaemon, plus `freerdp-shadow-agent` as a LaunchAgent in the session on the console |
| Login window (nobody logged in) | no | yes: the agent there runs as root and posts input to the HID system |
| Connection across a login or logout | ends with the session | stays up: the daemon switches to the agent of the new session |
| Screen Recording / Accessibility granted to | the server (or the terminal that starts it) | the agent |

### In a user's session

```sh
freerdp-shadow-cli /sec:tls        # log in with the Mac account's password (PAM)
```

macOS asks for Screen Recording on the first start; Accessibility (System Settings > Privacy &
Security) lets the clients type and click, without it they can only watch.

### As a daemon with agents

The daemon keeps the clients, TLS, the login and the encoder. It has no connection to the window
server, so an agent does the capture and the input in the session on the console and talks to the
daemon over XPC: frames cross as IOSurface objects, input as serialized `CGEvent`s, the cursor as
images and positions (see `mac_ipc.h`). launchd starts one agent at the login window and one in
each user's session; the daemon uses the one that said hello last, so a client stays connected
from the login window through the login to the desktop, and back at logout.

The daemon mode is on when `FREERDP_SHADOW_MAC_AGENT_SERVICE` names the daemon's Mach service.

`/Library/LaunchDaemons/org.freerdp.shadow.daemon.plist` (paths to adapt):

```xml
<dict>
	<key>Label</key><string>org.freerdp.shadow.daemon</string>
	<key>ProgramArguments</key>
	<array>
		<string>/usr/local/bin/freerdp-shadow-cli</string>
		<string>/sec:tls</string>
	</array>
	<key>EnvironmentVariables</key>
	<dict>
		<key>FREERDP_SHADOW_MAC_AGENT_SERVICE</key><string>org.freerdp.shadow</string>
		<!-- launchd gives a daemon no HOME; the server needs one -->
		<key>HOME</key><string>/var/root</string>
	</dict>
	<key>MachServices</key><dict><key>org.freerdp.shadow</key><true/></dict>
	<key>RunAtLoad</key><true/>
	<key>KeepAlive</key><true/>
</dict>
```

`/Library/LaunchAgents/org.freerdp.shadow.agent.plist`:

```xml
<dict>
	<key>Label</key><string>org.freerdp.shadow.agent</string>
	<key>LimitLoadToSessionType</key>
	<array><string>LoginWindow</string><string>Aqua</string></array>
	<key>ProgramArguments</key>
	<array>
		<string>/usr/local/bin/freerdp-shadow-agent</string>
		<string>org.freerdp.shadow</string>
	</array>
	<key>RunAtLoad</key><true/>
	<key>KeepAlive</key><true/>
</dict>
```

Grant Screen Recording and Accessibility to `freerdp-shadow-agent` once, in a user's session; the
grant holds at the login window too. A grant belongs to the program's code signature: an ad hoc
signed build (every local build) needs it again after each rebuild.

## Who may log in (PAM, `/sec:tls`)

The password is checked by Open Directory through PAM's `checkpw` service. With NLA (or extended
NLA) the server would look for a SAM file instead, so PAM needs `/sec:tls` (`-sec-nla` alone leaves
extended NLA on).

- In a user's session: only that user.
- As a daemon: the user on the console, or at the login window any local user but root. A client
  that logs in there sees the login window and still has to log in on it, as at the Mac.

## Environment

| Variable | Meaning |
|---|---|
| `FREERDP_SHADOW_MAC_AGENT_SERVICE` | daemon mode: the Mach service the agents connect to |
| `FREERDP_SHADOW_MAC_INPUT=hid` | post input to the HID system in a user's session too (as root at the login window it always is) |

## Limits

- FileVault: the disk is unlocked before macOS starts; no RDP server can reach that screen
  (`fdesetup authrestart` covers planned restarts).
- `IOHIDPostEvent`, the only way to type at the login window (Secure Event Input drops Quartz
  events there), is deprecated since macOS 11; it still works on macOS 26.
- A Mac that sleeps drops every connection; on power, `pmset -c sleep 0` keeps it awake.
- Not yet handled: a different display size on the handover between login window and session.
