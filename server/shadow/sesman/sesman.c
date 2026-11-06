/*
 * FreeRDP: A Remote Desktop Protocol Implementation
 * freerdp-shadow-sesman: route each RDP connection to the GNOME session of the
 * user it names
 *
 * Copyright 2026 Cong Zhang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * How it works:
 *
 *   client --TCP 3389--> freerdp-shadow-sesman (root) --unix socket--> freerdp-shadow-cli
 *                                                           (the user, in the user's
 *                                                            systemd manager, sharing
 *                                                            the user's GNOME Shell)
 *
 * The client's first packet, the X.224 Connection Request, names the user in
 * its cookie ("Cookie: mstshash=<user>"). The session manager reads that packet and
 * nothing else: it finds the user's GNOME Shell (or starts a headless one,
 * when allowed), starts the shadow server in the user's own systemd manager
 * if it is not running yet, hands it the packet and then copies the bytes
 * both ways. TLS and the login (PAM, the session's owner only) happen in the
 * shadow server: the session manager never sees a password, and every client works,
 * since nothing is redirected.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CR_MAX 1024         /* a Connection Request is far smaller */
#define CR_TIMEOUT_S 10     /* to send it */
#define SHADOW_TIMEOUT_S 120 /* for the shadow server to listen (GNOME may be starting) */
#define GNOME_SESSION "/usr/bin/gnome-session"
#define AUTOSTART_ROOT "/run/freerdp-sesman"
#define USER_MAX 64
#ifndef SESMAN_SHADOW
#define SESMAN_SHADOW "freerdp-shadow-cli"
#endif
#define SOCKET_NAME "freerdp-sesman-shadow.sock"
#define SHADOW_UNIT "freerdp-sesman-shadow"
#define GNOME_UNIT "freerdp-sesman-gnome"

typedef struct
{
	const char* bind;
	const char* port;
	const char* shadow;
	const char* headlessGroup; /* users of this group get a headless GNOME */
	const char* headlessSize;
	char** shadowArgs; /* after "--" */
	int shadowArgCount;
} Config;

static void logmsg(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "sesman[%d]: ", (int)getpid());
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---------------------------------------------------------------------- */
/* the Connection Request                                                  */
/* ---------------------------------------------------------------------- */

static bool read_full(int fd, uint8_t* buf, size_t len)
{
	size_t got = 0;
	while (got < len)
	{
		const ssize_t n = read(fd, buf + got, len - got);
		if (n > 0)
			got += (size_t)n;
		else if ((n < 0) && (errno == EINTR))
			continue;
		else
			return false;
	}
	return true;
}

static bool write_full(int fd, const uint8_t* buf, size_t len)
{
	size_t done = 0;
	while (done < len)
	{
		const ssize_t n = write(fd, buf + done, len - done);
		if (n > 0)
			done += (size_t)n;
		else if ((n < 0) && (errno == EINTR))
			continue;
		else
			return false;
	}
	return true;
}

/* TPKT (4 bytes) + X.224 Connection Request: length indicator, code 0xE0,
 * dst-ref, src-ref, class (7 bytes), then the cookie line and the RDP
 * negotiation request. Returns the packet's length, 0 on error. */
static size_t read_connection_request(int fd, uint8_t* buf, size_t size)
{
	if (!read_full(fd, buf, 4))
		return 0;
	if (buf[0] != 3)
	{
		logmsg("not an RDP connection (TPKT version %u)", buf[0]);
		return 0;
	}
	const size_t len = ((size_t)buf[2] << 8) | buf[3];
	if ((len < 11) || (len > size))
	{
		logmsg("bad Connection Request length %zu", len);
		return 0;
	}
	if (!read_full(fd, buf + 4, len - 4))
		return 0;
	if ((buf[5] & 0xF0) != 0xE0)
	{
		logmsg("the first packet is not a Connection Request");
		return 0;
	}
	return len;
}

/* "Cookie: mstshash=<user>\r\n" -> user, without a domain ("DOM\user",
 * "user@dom"); only the characters a local user name has */
static bool cookie_user(const uint8_t* cr, size_t len, char* user, size_t size)
{
	static const char prefix[] = "Cookie: mstshash=";
	const size_t plen = sizeof(prefix) - 1;
	size_t pos = 11;

	if ((len < pos + plen) || (memcmp(cr + pos, prefix, plen) != 0))
		return false;
	pos += plen;

	size_t end = pos;
	while ((end + 1 < len) && !((cr[end] == '\r') && (cr[end + 1] == '\n')))
		end++;
	if (end + 1 >= len)
		return false;

	size_t start = pos;
	for (size_t x = pos; x < end; x++)
	{
		if (cr[x] == '\\')
			start = x + 1;
	}
	size_t stop = end;
	for (size_t x = start; x < end; x++)
	{
		if (cr[x] == '@')
		{
			stop = x;
			break;
		}
	}

	const size_t n = stop - start;
	if ((n == 0) || (n >= size) || (n > USER_MAX))
		return false;
	for (size_t x = 0; x < n; x++)
	{
		const char c = (char)cr[start + x];
		if (!isalnum((unsigned char)c) && (c != '.') && (c != '_') && (c != '-'))
			return false;
		user[x] = c;
	}
	user[n] = '\0';
	return true;
}

/* ---------------------------------------------------------------------- */
/* the user and the user's GNOME                                           */
/* ---------------------------------------------------------------------- */

/* The user named, or the one local user whose name starts with it: some
 * clients cut the cookie short (mstsc keeps 9 characters) */
static bool find_user(const char* name, struct passwd* out, char* buf, size_t size)
{
	struct passwd* found = NULL;
	if ((getpwnam_r(name, out, buf, size, &found) == 0) && found)
		return true;

	const size_t n = strlen(name);
	int matches = 0;
	char match[USER_MAX + 1] = { 0 };
	setpwent();
	for (struct passwd* pw = getpwent(); pw; pw = getpwent())
	{
		if ((pw->pw_uid >= 1000) && (pw->pw_uid < 60000) && (strncmp(pw->pw_name, name, n) == 0))
		{
			matches++;
			snprintf(match, sizeof(match), "%s", pw->pw_name);
		}
	}
	endpwent();
	if (matches != 1)
	{
		if (matches > 1)
			logmsg("'%s' names %d users: refused", name, matches);
		return false;
	}
	return (getpwnam_r(match, out, buf, size, &found) == 0) && found;
}

/* Does the user run GNOME Shell? */
static bool has_gnome_shell(uid_t uid)
{
	DIR* proc = opendir("/proc");
	if (!proc)
		return false;

	bool found = false;
	for (struct dirent* e = readdir(proc); e && !found; e = readdir(proc))
	{
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		char path[300];
		snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
		FILE* f = fopen(path, "r");
		if (!f)
			continue;
		char line[256];
		bool shell = false;
		bool mine = false;
		while (fgets(line, sizeof(line), f))
		{
			if (strncmp(line, "Name:\tgnome-shell\n", 18) == 0)
				shell = true;
			unsigned real = 0;
			if (sscanf(line, "Uid:\t%u", &real) == 1)
				mine = (real == uid);
		}
		fclose(f);
		found = shell && mine;
	}
	closedir(proc);
	return found;
}

static bool in_group(const struct passwd* pw, const char* group)
{
	if (!group)
		return false;
	struct group* gr = getgrnam(group);
	if (!gr)
		return false;
	if (pw->pw_gid == gr->gr_gid)
		return true;
	for (char** m = gr->gr_mem; m && *m; m++)
	{
		if (strcmp(*m, pw->pw_name) == 0)
			return true;
	}
	return false;
}

/* Runs a command, waits for it; true when it exited with 0 */
static bool run(char* const argv[])
{
	const pid_t pid = fork();
	if (pid < 0)
		return false;
	if (pid == 0)
	{
		execvp(argv[0], argv);
		_exit(127);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
	{
		if (errno != EINTR)
			return false;
	}
	return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

/* Is the user's GNOME Shell a headless one this session manager started? */
static bool headless_is_ours(const struct passwd* pw)
{
	char unit[USER_MAX + 32];
	snprintf(unit, sizeof(unit), GNOME_UNIT "-%s.service", pw->pw_name);
	char* argv[] = { "systemctl", "is-active", "--quiet", unit, NULL };
	return run(argv);
}

/* Mutter answers on the user's session bus once GNOME Shell is up */
static bool mutter_ready(const struct passwd* pw, int seconds)
{
	char machine[USER_MAX + 16];
	snprintf(machine, sizeof(machine), "--machine=%s@.host", pw->pw_name);
	char* argv[] = { "busctl",
		             machine,
		             "--user",
		             "--quiet",
		             "--timeout=5",
		             "call",
		             "org.gnome.Mutter.DisplayConfig",
		             "/org/gnome/Mutter/DisplayConfig",
		             "org.gnome.Mutter.DisplayConfig",
		             "GetCurrentState",
		             NULL };
	const time_t end = time(NULL) + seconds;
	do
	{
		if (run(argv))
			return true;
		sleep(1);
	} while (time(NULL) < end);
	return false;
}

/* GNOME Software stays in the background of a desktop and refreshes and updates what
 * the system has. polkit takes a session with no seat for a remote one, so each of those
 * steps asks for an administrator: a dialog nobody asked for, which also takes the
 * keyboard. A desktop entry of the same name earlier in XDG_CONFIG_DIRS turns it off for
 * the sessions started here, and it still opens by hand. */
static void hide_autostart(void)
{
	static const char* const dirs[] = { AUTOSTART_ROOT, AUTOSTART_ROOT "/xdg",
		                                AUTOSTART_ROOT "/xdg/autostart" };
	for (size_t x = 0; x < sizeof(dirs) / sizeof(dirs[0]); x++)
	{
		if ((mkdir(dirs[x], 0755) < 0) && (errno != EEXIST))
		{
			logmsg("%s: %s", dirs[x], strerror(errno));
			return;
		}
	}

	FILE* file = fopen(AUTOSTART_ROOT "/xdg/autostart/org.gnome.Software.desktop", "w");
	if (!file)
	{
		logmsg("cannot turn off the background of GNOME Software: %s", strerror(errno));
		return;
	}
	fputs("[Desktop Entry]\nType=Application\nName=GNOME Software\nHidden=true\n", file);
	fclose(file);
}

/* A headless GNOME for a user of --headless-group that has no session. It runs as a
 * system unit with a PAM session, as a display manager would start it: logind registers
 * a (wayland) session with no seat, so the user's systemd manager starts and stays as
 * long as it runs, and GNOME Shell comes up with no monitor. gnome-session gives the
 * user a whole desktop: the settings daemons (keyboard layouts, shortcuts, what X11
 * programs read for fonts and scale), the ssh agent, the autostart programs. */
static bool start_headless(const Config* cfg, const struct passwd* pw)
{
	char unit[USER_MAX + 32];
	char uid[32];
	char runtime[64];
	char bus[96];
	char machine[USER_MAX + 16];
	snprintf(unit, sizeof(unit), "--unit=" GNOME_UNIT "-%s", pw->pw_name);
	snprintf(uid, sizeof(uid), "--uid=%u", (unsigned)pw->pw_uid);
	snprintf(runtime, sizeof(runtime), "--setenv=XDG_RUNTIME_DIR=/run/user/%u",
	         (unsigned)pw->pw_uid);
	snprintf(bus, sizeof(bus), "--setenv=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus",
	         (unsigned)pw->pw_uid);
	snprintf(machine, sizeof(machine), "--machine=%s@.host", pw->pw_name);

	/* a system with only the shell still gets a desktop to look at */
	const bool session = access(GNOME_SESSION, X_OK) == 0;

	/* no monitor of its own: the shadow server adds one (RecordVirtual) */
	logmsg("%s has no GNOME session: starting a headless one (%s)", pw->pw_name, cfg->headlessSize);
	char* startShell[] = { "systemd-run",
		                   "--quiet",
		                   "--collect",
		                   unit,
		                   uid,
		                   "--property=PAMName=login",
		                   "--setenv=XDG_SESSION_TYPE=wayland",
		                   "--setenv=XDG_SESSION_CLASS=user",
		                   runtime,
		                   bus,
		                   "--setenv=XDG_CURRENT_DESKTOP=GNOME",
		                   "--setenv=XDG_SESSION_DESKTOP=gnome",
		                   "--setenv=DESKTOP_SESSION=gnome",
		                   "--setenv=XDG_CONFIG_DIRS=" AUTOSTART_ROOT "/xdg:/etc/xdg",
		                   session ? GNOME_SESSION : "gnome-shell",
		                   session ? NULL : "--headless",
		                   NULL };
	char* startPipewire[] = {
		"systemctl",           machine, "--user", "start", "pipewire.socket", "pipewire.service",
		"wireplumber.service", NULL
	};
	hide_autostart();
	if (!run(startShell))
	{
		logmsg("cannot start GNOME Shell for %s", pw->pw_name);
		return false;
	}
	if (!run(startPipewire))
		logmsg("PipeWire did not start for %s: no picture", pw->pw_name);
	if (!mutter_ready(pw, SHADOW_TIMEOUT_S))
	{
		logmsg("GNOME Shell of %s did not come up", pw->pw_name);
		return false;
	}
	logmsg("headless GNOME of %s is up", pw->pw_name);
	return true;
}

/* ---------------------------------------------------------------------- */
/* the shadow server                                                       */
/* ---------------------------------------------------------------------- */

static int connect_unix(const char* path)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path))
		return -1;
	strcpy(addr.sun_path, path);
	const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		close(fd);
		return -1;
	}
	return fd;
}

/* The shadow server of the user, on a unix socket in the user's runtime
 * directory; started in the user's systemd manager when it does not answer */
/* An update leaves the old server running for as long as anyone reaches it: is the program
 * behind this socket gone from disk? Asked as the user: root may lack the capability to read
 * another user's /proc/<pid>/exe, in a container for one. */
static bool server_replaced(int fd, const struct passwd* pw)
{
	struct ucred cred = { 0 };
	socklen_t len = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
		return false;

	const pid_t pid = fork();
	if (pid < 0)
		return false;
	if (pid == 0)
	{
		if ((setgroups(0, NULL) < 0) || (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) < 0) ||
		    (setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid) < 0))
			_exit(0);

		char link[64];
		char exe[PATH_MAX];
		snprintf(link, sizeof(link), "/proc/%d/exe", (int)cred.pid);
		const ssize_t n = readlink(link, exe, sizeof(exe) - 1);
		if (n <= 0)
			_exit(0);
		exe[n] = '\0';
		_exit(strstr(exe, " (deleted)") ? 1 : 0);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
	{
		if (errno != EINTR)
			return false;
	}
	return WIFEXITED(status) && (WEXITSTATUS(status) == 1);
}

static int shadow_connect(const Config* cfg, const struct passwd* pw)
{
	char path[128];
	snprintf(path, sizeof(path), "/run/user/%u/" SOCKET_NAME, (unsigned)pw->pw_uid);

	char machine[USER_MAX + 16];
	snprintf(machine, sizeof(machine), "--machine=%s@.host", pw->pw_name);

	int fd = connect_unix(path);
	if (fd >= 0)
	{
		if (!server_replaced(fd, pw))
			return fd;

		/* the desktop stays: a client connected to the old server has to reconnect */
		logmsg("the shadow server of %s was replaced on disk: restarting it", pw->pw_name);
		close(fd);
		char* stop[] = { "systemctl", machine, "--user", "stop", SHADOW_UNIT, NULL };
		if (!run(stop))
			logmsg("could not stop the old shadow server of %s", pw->pw_name);
	}

	char ipc[160];
	char virtualMonitor[64];
	snprintf(ipc, sizeof(ipc), "/ipc-socket:%s", path);
	/* a headless GNOME of ours has no monitor: the shadow server adds its own,
	 * which also lets the clients draw the cursor */
	snprintf(virtualMonitor, sizeof(virtualMonitor), "--setenv=FREERDP_SHADOW_MUTTER_VIRTUAL=%s",
	         headless_is_ours(pw) ? cfg->headlessSize : "");

	/* its log goes to the journal: WinPR's journald appender when it has one,
	 * else lines on stdout, which a pipe would buffer in blocks (stdbuf) */
	char* argv[32 + 16] = { "systemd-run",
		                    machine,
		                    "--user",
		                    "--quiet",
		                    "--collect",
		                    "--unit=" SHADOW_UNIT,
		                    "--setenv=WLOG_APPENDER=journald",
		                    virtualMonitor,
		                    "stdbuf",
		                    "-oL",
		                    "-eL",
		                    (char*)cfg->shadow,
		                    ipc,
		                    "/sec:tls" };
	int argc = 14;
	for (int x = 0; (x < cfg->shadowArgCount) && (argc < 46); x++)
		argv[argc++] = cfg->shadowArgs[x];
	argv[argc] = NULL;

	logmsg("starting the shadow server for %s", pw->pw_name);
	if (!run(argv))
		logmsg("systemd-run failed (the unit may be starting already): waiting for it");

	for (int x = 0; x < SHADOW_TIMEOUT_S * 4; x++)
	{
		fd = connect_unix(path);
		if (fd >= 0)
			return fd;
		usleep(250 * 1000);
	}
	logmsg("the shadow server of %s did not listen on %s", pw->pw_name, path);
	return -1;
}

/* ---------------------------------------------------------------------- */
/* a connection                                                            */
/* ---------------------------------------------------------------------- */

static void relay(int a, int b)
{
	uint8_t buf[65536];
	struct pollfd fds[2] = { { .fd = a, .events = POLLIN }, { .fd = b, .events = POLLIN } };
	for (;;)
	{
		if (poll(fds, 2, -1) < 0)
		{
			if (errno == EINTR)
				continue;
			return;
		}
		for (int x = 0; x < 2; x++)
		{
			if (!(fds[x].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			const ssize_t n = read(fds[x].fd, buf, sizeof(buf));
			if (n <= 0)
				return;
			if (!write_full(fds[1 - x].fd, buf, (size_t)n))
				return;
		}
	}
}

/* The shadow server sets these on a client's socket, but behind us it has a unix socket:
 * without them a client that went away with no FIN (a closed laptop) stays connected for
 * good. The same values as FreeRDP's defaults. */
static void keep_alive(int fd)
{
	const int on = 1;
	const int idle = 5;
	const int interval = 2;
	const int count = 3;
	const unsigned ackMs = 9000;
	(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
	(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
	(void)setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ackMs, sizeof(ackMs));
}

static void handle(const Config* cfg, int client, const char* peer)
{
	const struct timeval tv = { .tv_sec = CR_TIMEOUT_S };
	(void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	keep_alive(client);

	uint8_t cr[CR_MAX];
	const size_t len = read_connection_request(client, cr, sizeof(cr));
	if (len == 0)
		return;

	char name[USER_MAX + 1];
	if (!cookie_user(cr, len, name, sizeof(name)))
	{
		logmsg("%s: the client named no user (Cookie: mstshash=): refused", peer);
		return;
	}

	struct passwd pw;
	char pwbuf[4096];
	if (!find_user(name, &pw, pwbuf, sizeof(pwbuf)))
	{
		logmsg("%s: no user '%s': refused", peer, name);
		return;
	}

	if (!has_gnome_shell(pw.pw_uid))
	{
		if (!in_group(&pw, cfg->headlessGroup))
		{
			logmsg("%s: %s has no GNOME session: refused", peer, pw.pw_name);
			return;
		}
		if (!start_headless(cfg, &pw))
			return;
	}

	const int shadow = shadow_connect(cfg, &pw);
	if (shadow < 0)
		return;

	/* nothing below needs root: copy the bytes as the user */
	if ((setgroups(0, NULL) < 0) || (setresgid(pw.pw_gid, pw.pw_gid, pw.pw_gid) < 0) ||
	    (setresuid(pw.pw_uid, pw.pw_uid, pw.pw_uid) < 0))
	{
		logmsg("cannot drop to %s: %s", pw.pw_name, strerror(errno));
		close(shadow);
		return;
	}

	const struct timeval none = { 0 };
	(void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
	logmsg("%s: %s -> the shadow server of their GNOME session", peer, pw.pw_name);
	if (write_full(shadow, cr, len))
		relay(client, shadow);
	close(shadow);
	logmsg("%s: %s: closed", peer, pw.pw_name);
}

/* ---------------------------------------------------------------------- */
/* the listener                                                            */
/* ---------------------------------------------------------------------- */

static int listen_on(const char* address, const char* port)
{
	struct addrinfo hints = { .ai_family = AF_UNSPEC,
		                      .ai_socktype = SOCK_STREAM,
		                      .ai_flags = AI_PASSIVE };
	struct addrinfo* res = NULL;
	const int rc = getaddrinfo(address, port, &hints, &res);
	if (rc != 0)
	{
		logmsg("cannot resolve %s:%s: %s", address ? address : "*", port, gai_strerror(rc));
		return -1;
	}

	int fd = -1;
	/* prefer IPv6 (it takes IPv4 too) */
	for (int pass = 0; (pass < 2) && (fd < 0); pass++)
	{
		for (struct addrinfo* ai = res; ai && (fd < 0); ai = ai->ai_next)
		{
			if ((pass == 0) != (ai->ai_family == AF_INET6))
				continue;
			fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
			if (fd < 0)
				continue;
			const int on = 1;
			const int off = 0;
			(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
			if (ai->ai_family == AF_INET6)
				(void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
			if ((bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) || (listen(fd, 64) < 0))
			{
				logmsg("cannot listen on %s:%s: %s", address ? address : "*", port,
				       strerror(errno));
				close(fd);
				fd = -1;
			}
		}
	}
	freeaddrinfo(res);
	return fd;
}

static void usage(const char* self)
{
	fprintf(stderr,
	        "usage: %s [options] [-- <shadow server options>]\n"
	        "  --port <port>            listen here (3389)\n"
	        "  --bind <address>         listen on this address only\n"
	        "  --shadow <path>          the shadow server (" SESMAN_SHADOW ")\n"
	        "  --headless-group <group> its users get a headless GNOME when they have none\n"
	        "  --headless-size <WxH>    that GNOME's monitor (1920x1080)\n",
	        self);
}

int main(int argc, char** argv)
{
	Config cfg = { .port = "3389", .shadow = SESMAN_SHADOW, .headlessSize = "1920x1080" };
	static const struct option options[] = { { "port", required_argument, NULL, 'p' },
		                                     { "bind", required_argument, NULL, 'b' },
		                                     { "shadow", required_argument, NULL, 's' },
		                                     { "headless-group", required_argument, NULL, 'g' },
		                                     { "headless-size", required_argument, NULL, 'z' },
		                                     { "help", no_argument, NULL, 'h' },
		                                     { NULL, 0, NULL, 0 } };
	for (int c; (c = getopt_long(argc, argv, "p:b:s:g:z:h", options, NULL)) != -1;)
	{
		switch (c)
		{
			case 'p':
				cfg.port = optarg;
				break;
			case 'b':
				cfg.bind = optarg;
				break;
			case 's':
				cfg.shadow = optarg;
				break;
			case 'g':
				cfg.headlessGroup = optarg;
				break;
			case 'z':
				cfg.headlessSize = optarg;
				break;
			default:
				usage(argv[0]);
				return (c == 'h') ? 0 : 2;
		}
	}
	cfg.shadowArgs = argv + optind;
	cfg.shadowArgCount = argc - optind;

	unsigned w = 0;
	unsigned h = 0;
	if ((sscanf(cfg.headlessSize, "%ux%u", &w, &h) != 2) || (w < 320) || (h < 200) || (w > 8192) ||
	    (h > 8192))
	{
		logmsg("bad --headless-size '%s'", cfg.headlessSize);
		return 2;
	}
	if (geteuid() != 0)
		logmsg("not root: only the own user's session can be reached");

	const int listener = listen_on(cfg.bind, cfg.port);
	if (listener < 0)
		return 1;
	signal(SIGCHLD, SIG_IGN); /* children are reaped by the kernel */
	signal(SIGPIPE, SIG_IGN);
	logmsg("listening on %s:%s%s%s", cfg.bind ? cfg.bind : "*", cfg.port,
	       cfg.headlessGroup ? ", headless GNOME for group " : "",
	       cfg.headlessGroup ? cfg.headlessGroup : "");

	for (;;)
	{
		struct sockaddr_storage addr;
		socklen_t alen = sizeof(addr);
		const int client = accept4(listener, (struct sockaddr*)&addr, &alen, SOCK_CLOEXEC);
		if (client < 0)
		{
			if (errno == EINTR)
				continue;
			logmsg("accept: %s", strerror(errno));
			continue;
		}

		char host[NI_MAXHOST] = "?";
		char serv[NI_MAXSERV] = "?";
		(void)getnameinfo((struct sockaddr*)&addr, alen, host, sizeof(host), serv, sizeof(serv),
		                  NI_NUMERICHOST | NI_NUMERICSERV);
		char peer[NI_MAXHOST + NI_MAXSERV + 2];
		snprintf(peer, sizeof(peer), "%s:%s", host, serv);

		const pid_t pid = fork();
		if (pid == 0)
		{
			close(listener);
			signal(SIGCHLD, SIG_DFL); /* run() waits for its own children */
			handle(&cfg, client, peer);
			close(client);
			_exit(0);
		}
		if (pid < 0)
			logmsg("fork: %s", strerror(errno));
		close(client);
	}
}
