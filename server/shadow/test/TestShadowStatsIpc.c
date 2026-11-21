/* The stats socket (/stats-ipc): the same cases as the SDL client's TestSDLStatsIpc, and the
 * line with every client's newest period. POSIX only: Wine has no AF_UNIX, a Windows run is
 * the kit's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/wtypes.h>
#include <winpr/winpr.h>

#include "../shadow_stats_ipc.h"
#include "../shadow_gfx_stats.h"

#if defined(_WIN32)
int TestShadowStatsIpc(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	return 0;
}
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return FALSE;                                                    \
		}                                                                    \
	} while (0)

static char dir[256];

static void path_in(char* path, size_t size, const char* name)
{
	(void)snprintf(path, size, "%s/%s", dir, name);
}

static int connect_to(const char* path)
{
	struct sockaddr_un address = { 0 };
	address.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(address.sun_path))
		return -1;
	strcpy(address.sun_path, path);
	const int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	if (connect(s, (const struct sockaddr*)&address, sizeof(address)) != 0)
	{
		close(s);
		return -1;
	}
	return s;
}

/* the next line (without its end) within ms, or an empty buffer at the end or the time out */
static BOOL read_line(int s, char* line, size_t size, int ms)
{
	size_t at = 0;
	line[0] = '\0';
	while (at + 1 < size)
	{
		struct pollfd fd = { s, POLLIN, 0 };
		if (poll(&fd, 1, ms) <= 0)
			return FALSE;
		char c = 0;
		const ssize_t n = recv(s, &c, 1, 0);
		if (n <= 0)
			return FALSE;
		if (c == '\n')
		{
			line[at] = '\0';
			return TRUE;
		}
		line[at++] = c;
	}
	return FALSE;
}

/* the peer closed (an end, no line) within ms */
static BOOL closed_by_peer(int s, int ms)
{
	struct pollfd fd = { s, POLLIN, 0 };
	if (poll(&fd, 1, ms) <= 0)
		return FALSE;
	char c = 0;
	return recv(s, &c, 1, 0) == 0;
}

static SHADOW_STATS_IPC* open_at(const char* name, char* path, size_t size)
{
	char error[512] = { 0 };
	path_in(path, size, name);
	SHADOW_STATS_IPC* ipc = shadow_stats_ipc_open(path, error, sizeof(error));
	if (!ipc)
		(void)fprintf(stderr, "open %s: %s\n", path, error);
	return ipc;
}

static BOOL wait_readers(SHADOW_STATS_IPC* ipc, size_t count)
{
	for (int x = 0; x < 200; x++)
	{
		if (shadow_stats_ipc_readers(ipc) == count)
			return TRUE;
		usleep(5000);
	}
	return FALSE;
}

static BOOL test_first_and_newest(void)
{
	char path[512];
	char line[8192];
	SHADOW_STATS_IPC* ipc = open_at("a.sock", path, sizeof(path));
	CHECK(ipc);
	struct stat st = { 0 };
	CHECK(lstat(path, &st) == 0);
	CHECK(S_ISSOCK(st.st_mode));
	CHECK((st.st_mode & 0777) == 0600);
	CHECK(strcmp(shadow_stats_ipc_path(ipc), path) == 0);

	shadow_stats_ipc_publish(ipc, "one");
	const int s = connect_to(path);
	CHECK(s >= 0);
	/* the newest line at once */
	CHECK(read_line(s, line, sizeof(line), 2000));
	CHECK(strcmp(line, "one") == 0);
	shadow_stats_ipc_publish(ipc, "two");
	CHECK(read_line(s, line, sizeof(line), 2000));
	CHECK(strcmp(line, "two") == 0);
	/* what a reader sends is ignored */
	CHECK(send(s, "x\n", 2, 0) == 2);
	shadow_stats_ipc_publish(ipc, "three");
	CHECK(read_line(s, line, sizeof(line), 2000));
	CHECK(strcmp(line, "three") == 0);
	close(s);
	CHECK(wait_readers(ipc, 0));

	shadow_stats_ipc_free(ipc);
	/* the file goes with it */
	CHECK(lstat(path, &st) != 0);
	return TRUE;
}

static BOOL test_slow_reader(void)
{
	char path[512];
	SHADOW_STATS_IPC* ipc = open_at("slow.sock", path, sizeof(path));
	CHECK(ipc);
	const int s = connect_to(path);
	CHECK(s >= 0);
	CHECK(wait_readers(ipc, 1));

	/* 2000 lines of 4 KB while the reader reads nothing: far more than the socket holds */
	char* big = malloc(4097);
	CHECK(big);
	for (int x = 0; x < 2000; x++)
	{
		(void)snprintf(big, 4097, "%04d", x);
		memset(big + 4, 'a', 4092);
		big[4096] = '\0';
		shadow_stats_ipc_publish(ipc, big);
	}
	free(big);
	usleep(100000);

	size_t lines = 0;
	char* line = malloc(8192);
	CHECK(line);
	char last[8] = { 0 };
	while (read_line(s, line, 8192, 300))
	{
		CHECK(strlen(line) == 4096);
		memcpy(last, line, 4);
		lines++;
	}
	free(line);
	/* only some of them, never a queue, and the last one is the newest */
	CHECK(lines > 0);
	CHECK(lines < 2000);
	CHECK(strcmp(last, "1999") == 0);
	close(s);
	shadow_stats_ipc_free(ipc);
	return TRUE;
}

static BOOL test_too_many_readers(void)
{
	char path[512];
	char line[256];
	SHADOW_STATS_IPC* ipc = open_at("many.sock", path, sizeof(path));
	CHECK(ipc);
	shadow_stats_ipc_publish(ipc, "hello");
	int s[SHADOW_STATS_IPC_MAX_READERS + 1];
	for (size_t x = 0; x < SHADOW_STATS_IPC_MAX_READERS; x++)
	{
		s[x] = connect_to(path);
		CHECK(s[x] >= 0);
		CHECK(read_line(s[x], line, sizeof(line), 2000));
	}
	CHECK(wait_readers(ipc, SHADOW_STATS_IPC_MAX_READERS));
	s[SHADOW_STATS_IPC_MAX_READERS] = connect_to(path);
	CHECK(s[SHADOW_STATS_IPC_MAX_READERS] >= 0);
	/* the one more is closed at once */
	CHECK(closed_by_peer(s[SHADOW_STATS_IPC_MAX_READERS], 2000));
	for (size_t x = 0; x <= SHADOW_STATS_IPC_MAX_READERS; x++)
		close(s[x]);
	shadow_stats_ipc_free(ipc);
	return TRUE;
}

static BOOL test_paths(void)
{
	char path[512];
	char error[512];

	/* a file that is no socket is kept */
	path_in(path, sizeof(path), "file");
	FILE* f = fopen(path, "w");
	CHECK(f);
	(void)fputs("keep", f);
	(void)fclose(f);
	CHECK(!shadow_stats_ipc_open(path, error, sizeof(error)));
	struct stat st = { 0 };
	CHECK(lstat(path, &st) == 0);
	CHECK(S_ISREG(st.st_mode) && (st.st_size == 4));
	(void)unlink(path);

	/* a dead socket of ours is replaced */
	path_in(path, sizeof(path), "stale.sock");
	{
		struct sockaddr_un address = { 0 };
		address.sun_family = AF_UNIX;
		strcpy(address.sun_path, path);
		const int s = socket(AF_UNIX, SOCK_STREAM, 0);
		CHECK(s >= 0);
		CHECK(bind(s, (const struct sockaddr*)&address, sizeof(address)) == 0);
		close(s);
	}
	SHADOW_STATS_IPC* ipc = shadow_stats_ipc_open(path, error, sizeof(error));
	CHECK(ipc);
	/* one that listens is not */
	CHECK(!shadow_stats_ipc_open(path, error, sizeof(error)));
	CHECK(strstr(error, "listens"));
	shadow_stats_ipc_free(ipc);

	/* too long */
	char longPath[512];
	memset(longPath, 'x', sizeof(longPath) - 1);
	longPath[0] = '/';
	longPath[sizeof(longPath) - 1] = '\0';
	CHECK(!shadow_stats_ipc_open(longPath, error, sizeof(error)));
	CHECK(!shadow_stats_ipc_open("", error, sizeof(error)));

	/* a directory others may write to is refused, unless it is sticky */
	char open[512];
	path_in(open, sizeof(open), "open");
	CHECK(mkdir(open, 0700) == 0);
	CHECK(chmod(open, 0777) == 0);
	(void)snprintf(path, sizeof(path), "%s/x.sock", open);
	CHECK(!shadow_stats_ipc_open(path, error, sizeof(error)));
	CHECK(chmod(open, 01777) == 0);
	ipc = shadow_stats_ipc_open(path, error, sizeof(error));
	CHECK(ipc);
	shadow_stats_ipc_free(ipc);
	CHECK(rmdir(open) == 0);
	return TRUE;
}

static BOOL test_default_path(void)
{
	char path[512];
	char error[512];
	char expected[512];
	const char* old = getenv("XDG_RUNTIME_DIR");
	char* saved = old ? strdup(old) : nullptr;
	CHECK(setenv("XDG_RUNTIME_DIR", "/run/x//", 1) == 0);
	CHECK(shadow_stats_ipc_default_path(path, sizeof(path), error, sizeof(error)));
	(void)snprintf(expected, sizeof(expected), "/run/x/freerdp-shadow-%d.stats.sock",
	               (int)getpid());
	const BOOL same = strcmp(path, expected) == 0;
	if (saved)
		(void)setenv("XDG_RUNTIME_DIR", saved, 1);
	else
		(void)unsetenv("XDG_RUNTIME_DIR");
	free(saved);
	CHECK(same);
	return TRUE;
}

/* each line carries every client's newest period, in the order they came */
static BOOL test_clients(void)
{
	char path[512];
	char line[8192];
	SHADOW_STATS_IPC* ipc = open_at("clients.sock", path, sizeof(path));
	CHECK(ipc);
	const int s = connect_to(path);
	CHECK(s >= 0);
	CHECK(wait_readers(ipc, 1));

	shadow_stats_ipc_client_period(ipc, 1, "{\"connection_id\":\"1\",\"periods\":1}");
	CHECK(read_line(s, line, sizeof(line), 2000));
	{
		const char prefix[] = "{\"schema\":\"freerdp-shadow-stats\",\"version\":1,\"seq\":1,\"time_ms\":";
		CHECK(strncmp(line, prefix, sizeof(prefix) - 1) == 0);
	}
	CHECK(strstr(line, ",\"clients\":[{\"connection_id\":\"1\",\"periods\":1}]}"));

	shadow_stats_ipc_client_period(ipc, 2, "{\"connection_id\":\"2\",\"periods\":1}");
	CHECK(read_line(s, line, sizeof(line), 2000));
	CHECK(strstr(line, "\"seq\":2,"));
	CHECK(strstr(line, "[{\"connection_id\":\"1\",\"periods\":1},{\"connection_id\":\"2\",\"periods\":1}]}"));

	shadow_stats_ipc_client_period(ipc, 1, "{\"connection_id\":\"1\",\"periods\":2}");
	CHECK(read_line(s, line, sizeof(line), 2000));
	CHECK(strstr(line, "[{\"connection_id\":\"1\",\"periods\":2},{\"connection_id\":\"2\",\"periods\":1}]}"));

	shadow_stats_ipc_client_gone(ipc, 1);
	shadow_stats_ipc_client_period(ipc, 2, "{\"connection_id\":\"2\",\"periods\":2}");
	CHECK(read_line(s, line, sizeof(line), 2000));
	CHECK(strstr(line, "\"clients\":[{\"connection_id\":\"2\",\"periods\":2}]}"));
	close(s);
	shadow_stats_ipc_free(ipc);
	return TRUE;
}

static BOOL test_json(void)
{
	SHADOW_GFX_STATS stats = { 0 };
	stats.captured = 300;
	stats.sent = 290;
	stats.held = 10;
	stats.merged = 9;
	stats.acks = 288;
	stats.ackDelays = 2;
	stats.ackDelaySumNs = 401000000;   /* avg 200.5 ms -> 201 */
	stats.ackDelayMaxNs = 250400000;   /* 250 */
	stats.ackDelayMinNs = 150600000;   /* 151 */
	stats.encodeSumNs = 290ull * 12345678ull; /* avg 12345.678 us -> 12346 */
	stats.encodeMaxNs = 41000400;      /* 41000 */
	stats.maxInFlight = 7;

	SHADOW_GFX_VALUES values = { 0 };
	stats.firstFrameId = 100;
	stats.lastFrameId = 389;
	shadow_gfx_stats_values(&stats, 12, 5003, 383, &values);
	CHECK(values.ackDelayAvgMs == 201);
	CHECK(values.ackDelayMinMs == 151);
	CHECK(values.encodeAvgUs == 12346);

	const SHADOW_GFX_PERIOD_INFO info = { .connectionId = 0x00ab00cd00ef0012ull,
		                                  .peer = "127.0.0.1:43210",
		                                  .startedAt = "2026-11-21T10:31:05-05:00",
		                                  .connectedMs = 1763700000000ull,
		                                  .periods = 3,
		                                  .timeMs = 1763700015003ull,
		                                  .ns = 987654321012ull,
		                                  .acksSuspended = FALSE,
		                                  .fps = 50,
		                                  .maxFps = 60 };
	char object[1024];
	CHECK(shadow_gfx_stats_json(&values, &info, object, sizeof(object)));
	const char* expected =
	    "{\"connection_id\":\"00ab00cd00ef0012\",\"peer\":\"127.0.0.1:43210\",\"connected_ms\":1763700000000,"
	    "\"started_at\":\"2026-11-21T10:31:05-05:00\",\"periods\":3,"
	    "\"time_ms\":1763700015003,\"ns\":987654321012,\"period_ms\":5003,\"acks_suspended\":false,\"fps\":50,"
	    "\"max_fps\":60,\"counters\":{\"GFX_PERIOD_MS\":5003,\"GFX_CAPTURED\":300,"
	    "\"GFX_SENT\":290,\"GFX_HELD\":10,\"GFX_MERGED\":9,\"GFX_ACKS\":288,"
	    "\"GFX_ACK_DELAY_AVG_MS\":201,\"GFX_ACK_DELAY_MAX_MS\":250,\"GFX_ACK_DELAY_MIN_MS\":151,"
	    "\"GFX_ENCODE_AVG_US\":12346,\"GFX_ENCODE_MAX_US\":41000,\"GFX_IN_FLIGHT_MAX\":7,"
	    "\"GFX_IN_FLIGHT_LIMIT\":12,\"GFX_FRAME_ID_FIRST\":100,\"GFX_FRAME_ID_LAST\":389,"
	    "\"GFX_FRAME_ID_ACKED_LAST\":383}}";
	if (strcmp(object, expected) != 0)
	{
		(void)fprintf(stderr, "got      %s\nexpected %s\n", object, expected);
		return FALSE;
	}
	/* too small a buffer is refused, not cut */
	CHECK(!shadow_gfx_stats_json(&values, &info, object, 100));

	/* nothing to average */
	SHADOW_GFX_STATS none = { 0 };
	shadow_gfx_stats_values(&none, 0, 0, 0, &values);
	CHECK((values.ackDelayAvgMs == 0) && (values.encodeAvgUs == 0) && (values.inFlightLimit == 0));
	CHECK((values.frameIdFirst == 0) && (values.frameIdLast == 0));
	/* a period that sent nothing has no frame ids, not 0 */
	CHECK(shadow_gfx_stats_json(&values, &info, object, sizeof(object)));
	CHECK(!strstr(object, "GFX_FRAME_ID_FIRST") && !strstr(object, "GFX_FRAME_ID_LAST"));
	CHECK(strstr(object, ",\"GFX_FRAME_ID_ACKED_LAST\":0}}"));

	/* a peer that is no address is left out */
	SHADOW_GFX_PERIOD_INFO odd = info;
	odd.peer = "a\"b";
	CHECK(shadow_gfx_stats_json(&values, &odd, object, sizeof(object)));
	CHECK(strstr(object, "\"peer\":null,"));
	odd.peer = "[::1]:3389";
	CHECK(shadow_gfx_stats_json(&values, &odd, object, sizeof(object)));
	CHECK(strstr(object, "\"peer\":\"[::1]:3389\","));
	return TRUE;
}

/* the peer of a loopback TCP connection is what that side's getsockname says */
static BOOL test_peer_name(void)
{
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(listener >= 0);
	struct sockaddr_in address = { 0 };
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0);
	CHECK(listen(listener, 1) == 0);
	socklen_t length = sizeof(address);
	CHECK(getsockname(listener, (struct sockaddr*)&address, &length) == 0);
	const int c = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(c >= 0);
	CHECK(connect(c, (struct sockaddr*)&address, sizeof(address)) == 0);
	const int s = accept(listener, nullptr, nullptr);
	CHECK(s >= 0);
	struct sockaddr_in local = { 0 };
	length = sizeof(local);
	CHECK(getsockname(c, (struct sockaddr*)&local, &length) == 0);
	char expected[64];
	(void)snprintf(expected, sizeof(expected), "127.0.0.1:%u", (unsigned)ntohs(local.sin_port));
	char name[64];
	CHECK(shadow_stats_ipc_peer_name(s, name, sizeof(name)));
	const BOOL same = strcmp(name, expected) == 0;
	CHECK(!shadow_stats_ipc_peer_name(listener, name, sizeof(name))); /* not connected */
	close(s);
	close(c);
	close(listener);
	CHECK(same);
	return TRUE;
}

/* ISO 8601 with the offset, in two zones */
static BOOL test_local_time(void)
{
	char text[64] = { 0 };
	const char* old = getenv("TZ");
	char* saved = old ? strdup(old) : nullptr;
	BOOL ok = TRUE;
	(void)setenv("TZ", "UTC0", 1);
	tzset();
	ok = ok && shadow_gfx_stats_local_time(1763739065123ull, text, sizeof(text)) &&
	     (strcmp(text, "2025-11-21T15:31:05+00:00") == 0);
	(void)setenv("TZ", "EST5", 1);
	tzset();
	ok = ok && shadow_gfx_stats_local_time(1763739065123ull, text, sizeof(text)) &&
	     (strcmp(text, "2025-11-21T10:31:05-05:00") == 0);
	ok = ok && !shadow_gfx_stats_local_time(1763739065123ull, text, 20);
	if (!ok)
		(void)fprintf(stderr, "local time: %s\n", text);
	if (saved)
		(void)setenv("TZ", saved, 1);
	else
		(void)unsetenv("TZ");
	tzset();
	free(saved);
	return ok;
}

int TestShadowStatsIpc(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	const char* base = getenv("TMPDIR");
	(void)snprintf(dir, sizeof(dir), "%s/TestShadowStatsIpc.XXXXXX",
	               (base && *base) ? base : "/tmp");
	if (!mkdtemp(dir))
		return -1;

	const BOOL ok = test_json() && test_first_and_newest() && test_slow_reader() &&
	                test_too_many_readers() && test_paths() && test_default_path() &&
	                test_clients() && test_peer_name() &&
	                test_local_time();
	(void)rmdir(dir);
	return ok ? 0 : -1;
}
#endif
