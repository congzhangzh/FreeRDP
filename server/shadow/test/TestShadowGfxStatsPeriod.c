#include <stdio.h>
#include <stdlib.h>

#include <winpr/wtypes.h>
#include <winpr/string.h>
#include <winpr/cmdline.h>
#include <freerdp/server/shadow.h>

/* The option as freerdp-shadow-cli has it (server/shadow/cli/shadow.c): the table belongs to the
 * program, so this tests the parsing of the option, not the program's table. */
static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "gfx-stats-period", COMMAND_LINE_VALUE_REQUIRED, "<seconds>", nullptr, nullptr, -1, nullptr,
	  "GFX counters" },
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
};

/* The server reads FREERDP_SHADOW_GFX_STATS_PERIOD with getenv: the C runtime's environment.
 * nullptr unsets. */
WINPR_ATTR_NODISCARD
static BOOL set(const char* name, const char* value)
{
#if defined(_WIN32)
	return _putenv_s(name, value ? value : "") == 0;
#else
	if (!value)
		return unsetenv(name) == 0;     // NOLINT(concurrency-mt-unsafe): one thread
	return setenv(name, value, 1) == 0; // NOLINT(concurrency-mt-unsafe): one thread
#endif
}

/* Parses the option on a new server: accepted, statsPeriod must be as given; refused, the
 * command line must fail and the field keep its default. */
WINPR_ATTR_NODISCARD
static BOOL expect(const char* what, const char* option, BOOL accepted, UINT32 seconds)
{
	char* argv[] = { "freerdp-shadow-cli", (char*)option, nullptr };
	const int argc = option ? 2 : 1;

	rdpShadowServer* server = shadow_server_new();
	if (!server)
	{
		(void)fprintf(stderr, "%s: shadow_server_new failed\n", what);
		return FALSE;
	}
	if (!accepted)
		seconds = server->statsPeriod;

	const int status = shadow_server_parse_command_line(server, argc, argv, options);
	const UINT32 got = server->statsPeriod;
	shadow_server_free(server);

	if ((status >= 0) != accepted)
	{
		(void)fprintf(stderr, "%s: status %d, expected it %s\n", what, status,
		              accepted ? "accepted" : "refused");
		return FALSE;
	}
	if (got != seconds)
	{
		(void)fprintf(stderr, "%s: statsPeriod %" PRIu32 ", expected %" PRIu32 "\n", what, got,
		              seconds);
		return FALSE;
	}
	return TRUE;
}

int TestShadowGfxStatsPeriod(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	const char* name = "FREERDP_SHADOW_GFX_STATS_PERIOD";
	const char* old = getenv(name); // NOLINT(concurrency-mt-unsafe): one thread
	char* saved = old ? _strdup(old) : nullptr;
	BOOL ok = set(name, nullptr);

	/* the default: not written (only at DEBUG or TRACE) */
	ok = expect("none", nullptr, TRUE, 0) && ok;
	ok = expect("off", "/gfx-stats-period:0", TRUE, 0) && ok;
	ok = expect("10", "/gfx-stats-period:10", TRUE, 10) && ok;
	ok = expect("highest", "/gfx-stats-period:3600", TRUE, 3600) && ok;

	ok = expect("3601", "/gfx-stats-period:3601", FALSE, 0) && ok;
	ok = expect("-1", "/gfx-stats-period:-1", FALSE, 0) && ok;
	ok = expect("abc", "/gfx-stats-period:abc", FALSE, 0) && ok;
	ok = expect("empty", "/gfx-stats-period:", FALSE, 0) && ok;
	ok = expect("no value", "/gfx-stats-period", FALSE, 0) && ok;

	/* FREERDP_SHADOW_GFX_STATS_PERIOD: the same value without a command line; /gfx-stats-period
	 * wins; a wrong value is ignored, the command line still accepted */
	ok = set(name, "30") && ok;
	ok = expect("env 30", nullptr, TRUE, 30) && ok;
	ok = expect("env 30, /gfx-stats-period:2", "/gfx-stats-period:2", TRUE, 2) && ok;
	ok = set(name, "0") && ok;
	ok = expect("env 0", nullptr, TRUE, 0) && ok;
	ok = set(name, "abc") && ok;
	ok = expect("env abc", nullptr, TRUE, 0) && ok;
	ok = set(name, "3601") && ok;
	ok = expect("env 3601", nullptr, TRUE, 0) && ok;

	ok = set(name, saved) && ok;
	free(saved);
	return ok ? 0 : -1;
}
