#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <winpr/crt.h>
#include <winpr/file.h>
#include <winpr/library.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/wlog.h>

/* The console appender must flush each line: a program reading a WinPR
 * program's log through a pipe has to see a line when it is logged, not when
 * the C runtime's buffer fills or the program exits. The child logs a first
 * line, waits, logs a last line and exits; the parent checks that the first
 * line arrived well before the last one. */

#define FIRST_LINE "console-flush-test first line"
#define LAST_LINE "console-flush-test last line"
#define CHILD_WAIT_MS 2000
#define MIN_GAP_MS 1000

#if defined(_WIN32)
#define test_popen _popen
#define test_pclose _pclose
#else
#define test_popen popen
#define test_pclose pclose
#endif

static int run_child(void)
{
	wLog* root = WLog_GetRoot();
	if (!root)
		return 2;
	if (!WLog_SetLogAppenderType(root, WLOG_APPENDER_CONSOLE))
		return 3;
	wLogAppender* appender = WLog_GetLogAppender(root);
	if (!WLog_ConfigureAppender(appender, "outputstream", "stdout"))
		return 4;
	if (!WLog_SetLogLevel(root, WLOG_INFO))
		return 5;

	wLog* log = WLog_Get("com.winpr.test.consoleflush");
	if (!log)
		return 6;

	WLog_Print(log, WLOG_INFO, FIRST_LINE);
	Sleep(CHILD_WAIT_MS);
	WLog_Print(log, WLOG_INFO, LAST_LINE);
	return 0;
}

int TestWLogConsoleFlush(int argc, char* argv[])
{
	if ((argc >= 2) && (strcmp(argv[1], "--child") == 0))
		return run_child();

	char exePath[MAX_PATH] = WINPR_C_ARRAY_INIT;
	const DWORD len = GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
	if ((len == 0) || (len >= sizeof(exePath)))
	{
		(void)fprintf(stderr, "GetModuleFileNameA failed\n");
		return 1;
	}

	char cmdline[MAX_PATH + 64] = WINPR_C_ARRAY_INIT;
#if defined(_WIN32)
	/* _popen runs cmd.exe /c, which strips one pair of outer quotes */
	(void)_snprintf(cmdline, sizeof(cmdline), "\"\"%s\" TestWLogConsoleFlush --child\"", exePath);
#else
	(void)snprintf(cmdline, sizeof(cmdline), "\"%s\" TestWLogConsoleFlush --child", exePath);
#endif

	FILE* pipe = test_popen(cmdline, "r");
	if (!pipe)
	{
		(void)fprintf(stderr, "popen(%s) failed\n", cmdline);
		return 1;
	}

	UINT64 firstAt = 0;
	UINT64 lastAt = 0;
	char line[1024] = WINPR_C_ARRAY_INIT;
	while (fgets(line, sizeof(line), pipe))
	{
		const UINT64 now = GetTickCount64();
		if (!firstAt && strstr(line, FIRST_LINE))
			firstAt = now;
		else if (!lastAt && strstr(line, LAST_LINE))
			lastAt = now;
	}

	const int status = test_pclose(pipe);
	if (status != 0)
	{
		(void)fprintf(stderr, "child exited with status %d\n", status);
		return 1;
	}

	if (!firstAt || !lastAt)
	{
		(void)fprintf(stderr, "missing line: first %s, last %s\n", firstAt ? "seen" : "not seen",
		              lastAt ? "seen" : "not seen");
		return 1;
	}

	const UINT64 gap = (lastAt >= firstAt) ? (lastAt - firstAt) : 0;
	(void)printf("first line arrived %" PRIu64 " ms before the last (child waited %d ms)\n", gap,
	             CHILD_WAIT_MS);
	if (gap < MIN_GAP_MS)
	{
		(void)fprintf(stderr,
		              "the first line arrived only %" PRIu64 " ms before the last: "
		              "the console appender did not flush it\n",
		              gap);
		return 1;
	}
	return 0;
}
