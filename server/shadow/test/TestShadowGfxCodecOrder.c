#include <stdio.h>
#include <string.h>

#include <freerdp/config.h>

#include <winpr/wtypes.h>
#include <winpr/cmdline.h>
#include <freerdp/server/shadow.h>

/* The option as freerdp-shadow-cli has it (server/shadow/cli/shadow.c): the table belongs to the
 * program, so this tests the parsing of the option and the order it gives, not the program's
 * table. */
static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "gfx-codec-order", COMMAND_LINE_VALUE_REQUIRED, "<codec>[,<codec>...]", nullptr, nullptr, -1,
	  nullptr, "The order the GFX codecs are tried in" },
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
};

#define N SHADOW_GFX_CODEC_COUNT

static const SHADOW_GFX_CODEC defaults[N] = { SHADOW_GFX_CODEC_AV2,         SHADOW_GFX_CODEC_AV1,
	                                          SHADOW_GFX_CODEC_HEVC,        SHADOW_GFX_CODEC_AVC444,
	                                          SHADOW_GFX_CODEC_AVC420,      SHADOW_GFX_CODEC_RFX,
	                                          SHADOW_GFX_CODEC_PROGRESSIVE, SHADOW_GFX_CODEC_PLANAR,
	                                          SHADOW_GFX_CODEC_NONE };

/* Parses option on a new server. Accepted: the order must be expected. Refused: the command line
 * must fail and the order stay the default one. */
WINPR_ATTR_NODISCARD
static BOOL expect(const char* what, const char* option, BOOL accepted,
                   const SHADOW_GFX_CODEC* expected)
{
	char* argv[] = { "freerdp-shadow-cli", (char*)option, nullptr };
	const int argc = option ? 2 : 1;

	rdpShadowServer* server = shadow_server_new();
	if (!server)
	{
		(void)fprintf(stderr, "%s: shadow_server_new failed\n", what);
		return FALSE;
	}

	const int status = shadow_server_parse_command_line(server, argc, argv, options);
	SHADOW_GFX_CODEC got[N] = { SHADOW_GFX_CODEC_NONE };
	memcpy(got, server->GfxCodecOrder, sizeof(got));
	shadow_server_free(server);

	if ((status >= 0) != accepted)
	{
		(void)fprintf(stderr, "%s: status %d, expected it %s\n", what, status,
		              accepted ? "accepted" : "refused");
		return FALSE;
	}

	const SHADOW_GFX_CODEC* want = accepted ? expected : defaults;
	for (size_t x = 0; x < N; x++)
	{
		if (got[x] != want[x])
		{
			(void)fprintf(stderr, "%s: place %" PRIuz " has codec %d, expected %d\n", what, x,
			              (int)got[x], (int)want[x]);
			return FALSE;
		}
	}
	return TRUE;
}

/* A codec the build has: its name accepted at the front. One it lacks: refused. */
WINPR_ATTR_NODISCARD
static BOOL first(const char* name, SHADOW_GFX_CODEC codec, BOOL built)
{
	char option[64] = { 0 };
	(void)_snprintf(option, sizeof(option), "/gfx-codec-order:%s", name);

	SHADOW_GFX_CODEC expected[N] = { SHADOW_GFX_CODEC_NONE };
	size_t n = 0;
	expected[n++] = codec;
	for (size_t x = 0; (x < N) && (defaults[x] != SHADOW_GFX_CODEC_NONE); x++)
	{
		if (defaults[x] != codec)
			expected[n++] = defaults[x];
	}
	return expect(name, option, built, expected);
}

int TestShadowGfxCodecOrder(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	BOOL ok = TRUE;

	/* none: the order of the fixed chain it replaces */
	ok &= expect("none", nullptr, TRUE, defaults);

	/* listed first, the others after them in the default order */
	{
		const SHADOW_GFX_CODEC two[N] = { SHADOW_GFX_CODEC_PROGRESSIVE, SHADOW_GFX_CODEC_RFX,
			                              SHADOW_GFX_CODEC_AV2,         SHADOW_GFX_CODEC_AV1,
			                              SHADOW_GFX_CODEC_HEVC,        SHADOW_GFX_CODEC_AVC444,
			                              SHADOW_GFX_CODEC_AVC420,      SHADOW_GFX_CODEC_PLANAR,
			                              SHADOW_GFX_CODEC_NONE };
		ok &= expect("progressive,rfx", "/gfx-codec-order:progressive,rfx", TRUE, two);
	}
	/* /gfx's own spelling */
	ok &= first("RFX", SHADOW_GFX_CODEC_RFX, TRUE);
	ok &= first("planar", SHADOW_GFX_CODEC_PLANAR, TRUE);

	/* each codec by the build */
#if defined(WITH_GFX_AV2)
	ok &= first("av2", SHADOW_GFX_CODEC_AV2, TRUE);
#else
	ok &= first("av2", SHADOW_GFX_CODEC_AV2, FALSE);
#endif
#if defined(WITH_GFX_AV1)
	ok &= first("AV1", SHADOW_GFX_CODEC_AV1, TRUE);
#else
	ok &= first("AV1", SHADOW_GFX_CODEC_AV1, FALSE);
#endif
#if defined(WITH_GFX_AZURE)
	ok &= first("hevc", SHADOW_GFX_CODEC_HEVC, TRUE);
#else
	ok &= first("hevc", SHADOW_GFX_CODEC_HEVC, FALSE);
#endif
#if defined(WITH_GFX_H264)
	ok &= first("AVC444", SHADOW_GFX_CODEC_AVC444, TRUE);
	ok &= first("avc420", SHADOW_GFX_CODEC_AVC420, TRUE);
#else
	ok &= first("AVC444", SHADOW_GFX_CODEC_AVC444, FALSE);
	ok &= first("avc420", SHADOW_GFX_CODEC_AVC420, FALSE);
#endif

	/* wrong: the order stays the default one */
	ok &= expect("twice", "/gfx-codec-order:rfx,progressive,rfx", FALSE, nullptr);
	ok &= expect("unknown", "/gfx-codec-order:vp9", FALSE, nullptr);
	ok &= expect("one unknown", "/gfx-codec-order:planar,uncompressed", FALSE, nullptr);
	ok &= expect("no value", "/gfx-codec-order", FALSE, nullptr);

	return ok ? 0 : -1;
}
