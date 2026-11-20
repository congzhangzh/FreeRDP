#include <stdio.h>

#include <winpr/wtypes.h>
#include <winpr/cmdline.h>
#include <freerdp/server/shadow.h>

/* The options as freerdp-shadow-cli has them (server/shadow/cli/shadow.c): the table belongs to
 * the program, so this tests the parsing of the options, not the program's table. */
static COMMAND_LINE_ARGUMENT_A options[] = { { "framerate", COMMAND_LINE_VALUE_REQUIRED, "<1-120>",
	                                           nullptr, nullptr, -1, nullptr, "Frame rate" },
	                                         { nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr,
	                                           nullptr } };

/* Parses the options on a new server. Accepted: maxFrameRate, h264FrameRate and h264BitRate
 * must be as given. Refused: the command line must fail and every field keep its default. */
WINPR_ATTR_NODISCARD
static BOOL expect(const char* what, const char* option1, const char* option2, BOOL accepted,
                   UINT32 maxFrameRate, UINT32 h264FrameRate, UINT32 h264BitRate)
{
	char* argv[] = { "freerdp-shadow-cli", (char*)option1, (char*)option2, nullptr };
	const int argc = option2 ? 3 : (option1 ? 2 : 1);

	rdpShadowServer* server = shadow_server_new();
	if (!server)
	{
		(void)fprintf(stderr, "%s: shadow_server_new failed\n", what);
		return FALSE;
	}

	if (!accepted)
	{
		maxFrameRate = server->maxFrameRate;
		h264FrameRate = server->h264FrameRate;
		h264BitRate = server->h264BitRate;
	}

	const int status = shadow_server_parse_command_line(server, argc, argv, options);
	const UINT32 gotMax = server->maxFrameRate;
	const UINT32 gotH264 = server->h264FrameRate;
	const UINT32 gotBitRate = server->h264BitRate;
	shadow_server_free(server);

	if ((status >= 0) != accepted)
	{
		(void)fprintf(stderr, "%s: status %d, expected it %s\n", what, status,
		              accepted ? "accepted" : "refused");
		return FALSE;
	}
	if ((gotMax != maxFrameRate) || (gotH264 != h264FrameRate) || (gotBitRate != h264BitRate))
	{
		(void)fprintf(stderr,
		              "%s: maxFrameRate %" PRIu32 " h264FrameRate %" PRIu32 " h264BitRate %" PRIu32
		              ", expected %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",
		              what, gotMax, gotH264, gotBitRate, maxFrameRate, h264FrameRate, h264BitRate);
		return FALSE;
	}
	return TRUE;
}

int TestShadowFrameRate(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	BOOL ok = TRUE;

	/* the defaults: no frame rate of its own (each subsystem and encoder keeps its own), H.264
	 * at 30 */
	ok = expect("none", nullptr, nullptr, TRUE, 0, 30, 10000000) && ok;

	ok = expect("60", "/framerate:60", nullptr, TRUE, 60, 60, 10000000) && ok;
	ok = expect("lowest", "/framerate:1", nullptr, TRUE, 1, 1, 10000000) && ok;
	ok = expect("highest", "/framerate:120", nullptr, TRUE, 120, 120, 10000000) && ok;

	ok = expect("0", "/framerate:0", nullptr, FALSE, 0, 0, 0) && ok;
	ok = expect("121", "/framerate:121", nullptr, FALSE, 0, 0, 0) && ok;
	ok = expect("-1", "/framerate:-1", nullptr, FALSE, 0, 0, 0) && ok;
	ok = expect("abc", "/framerate:abc", nullptr, FALSE, 0, 0, 0) && ok;
	ok = expect("60fps", "/framerate:60fps", nullptr, FALSE, 0, 0, 0) && ok;
	ok = expect("empty", "/framerate:", nullptr, FALSE, 0, 0, 0) && ok;
	ok = expect("no value", "/framerate", nullptr, FALSE, 0, 0, 0) && ok;

	return ok ? 0 : -1;
}
