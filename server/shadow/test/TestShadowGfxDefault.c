#include <stdio.h>

#include <winpr/wtypes.h>
#include <winpr/cmdline.h>
#include <freerdp/settings.h>
#include <freerdp/server/shadow.h>

/* The option as freerdp-shadow-cli has it (server/shadow/cli/shadow.c): the table belongs to the
 * program, so this tests the parsing of the option, not the program's table. */
static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "gfx", COMMAND_LINE_VALUE_BOOL, nullptr, BoolValueTrue, nullptr, -1, nullptr,
	  "Allow GFX pipeline" },
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
};

/* A new server parses option (nullptr: none); its settings must have the graphics pipeline
 * on or off as expected. */
WINPR_ATTR_NODISCARD
static BOOL expect(const char* what, const char* option, BOOL gfx)
{
	char* argv[] = { "freerdp-shadow-cli", (char*)option, nullptr };
	const int argc = option ? 2 : 1;

	rdpShadowServer* server = shadow_server_new();
	if (!server || !server->settings)
	{
		(void)fprintf(stderr, "%s: shadow_server_new failed\n", what);
		shadow_server_free(server);
		return FALSE;
	}
	const int status = shadow_server_parse_command_line(server, argc, argv, options);
	const BOOL got = freerdp_settings_get_bool(server->settings, FreeRDP_SupportGraphicsPipeline);
	shadow_server_free(server);
	if ((status < 0) || (got != gfx))
	{
		(void)fprintf(stderr, "%s: status %d, graphics pipeline %d, expected %d\n", what, status,
		              got, gfx);
		return FALSE;
	}
	return TRUE;
}

int TestShadowGfxDefault(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	/* on by default: a session manager starts the server without /gfx */
	BOOL ok = expect("default", nullptr, TRUE);
	ok = expect("/gfx", "/gfx", TRUE) && ok;
	ok = expect("+gfx", "+gfx", TRUE) && ok;
	ok = expect("-gfx", "-gfx", FALSE) && ok;
	return ok ? 0 : -1;
}
