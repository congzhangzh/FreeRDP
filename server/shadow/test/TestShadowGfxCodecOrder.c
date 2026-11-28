#include <stdio.h>
#include <string.h>

#include <freerdp/config.h>

#include <winpr/wtypes.h>
#include <winpr/cmdline.h>
#include <freerdp/server/shadow.h>
#include <freerdp/channels/rdpgfx.h>

#include "../shadow_codec_order.h"

/* The option as freerdp-shadow-cli has it (server/shadow/cli/shadow.c): the table belongs to the
 * program, so this tests the parsing of the option and the order it gives, not the program's
 * table. */
static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "gfx-codec-order", COMMAND_LINE_VALUE_REQUIRED, "<codec>[,<codec>...]", nullptr, nullptr, -1,
	  nullptr, "The order the GFX codecs are tried in" },
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
};

#define N SHADOW_GFX_CODEC_COUNT

/* every host but macOS: today's order, byte for byte */
static const SHADOW_GFX_CODEC order_av2_first[N] = {
	SHADOW_GFX_CODEC_AV2,         SHADOW_GFX_CODEC_AV1,    SHADOW_GFX_CODEC_HEVC,
	SHADOW_GFX_CODEC_AVC444,      SHADOW_GFX_CODEC_AVC420, SHADOW_GFX_CODEC_RFX,
	SHADOW_GFX_CODEC_PROGRESSIVE, SHADOW_GFX_CODEC_PLANAR, SHADOW_GFX_CODEC_NONE
};
/* macOS: HEVC first (VideoToolbox encodes it in hardware) */
static const SHADOW_GFX_CODEC order_hevc_first[N] = {
	SHADOW_GFX_CODEC_HEVC,        SHADOW_GFX_CODEC_AV2,    SHADOW_GFX_CODEC_AV1,
	SHADOW_GFX_CODEC_AVC444,      SHADOW_GFX_CODEC_AVC420, SHADOW_GFX_CODEC_RFX,
	SHADOW_GFX_CODEC_PROGRESSIVE, SHADOW_GFX_CODEC_PLANAR, SHADOW_GFX_CODEC_NONE
};

/* this build's default: written out, not taken from the code under test */
#if defined(__APPLE__)
#define defaults order_hevc_first
#else
#define defaults order_av2_first
#endif

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

/* the default order for both platforms, on every platform */
WINPR_ATTR_NODISCARD
static BOOL test_default_order(void)
{
	const struct
	{
		BOOL hevcFirst;
		const SHADOW_GFX_CODEC* expected;
	} cases[] = { { FALSE, order_av2_first }, { TRUE, order_hevc_first } };
	BOOL ok = TRUE;
	for (size_t c = 0; c < ARRAYSIZE(cases); c++)
	{
		SHADOW_GFX_CODEC got[N] = { SHADOW_GFX_CODEC_NONE };
		const size_t n = shadow_gfx_codec_default_order(cases[c].hevcFirst, got, ARRAYSIZE(got));
		if (n != N - 1)
		{
			(void)fprintf(stderr, "default order %d: %" PRIuz " codecs\n", cases[c].hevcFirst, n);
			ok = FALSE;
		}
		for (size_t x = 0; x < N; x++)
		{
			if (got[x] != cases[c].expected[x])
			{
				(void)fprintf(stderr, "default order %d: place %" PRIuz " has %d, expected %d\n",
				              cases[c].hevcFirst, x, (int)got[x], (int)cases[c].expected[x]);
				ok = FALSE;
			}
		}
	}
	/* too small: nothing */
	SHADOW_GFX_CODEC small[N - 2] = { SHADOW_GFX_CODEC_NONE };
	if ((shadow_gfx_codec_default_order(FALSE, small, ARRAYSIZE(small)) != 0) ||
	    (shadow_gfx_codec_default_order(TRUE, nullptr, N) != 0))
	{
		(void)fprintf(stderr, "default order: a too small array must give 0\n");
		ok = FALSE;
	}
#if defined(__APPLE__)
	const BOOL macOrder = TRUE;
#else
	const BOOL macOrder = FALSE;
#endif
	if (SHADOW_GFX_HEVC_FIRST != macOrder)
	{
		(void)fprintf(stderr, "SHADOW_GFX_HEVC_FIRST does not match this platform\n");
		ok = FALSE;
	}
	return ok;
}

/* the first of the server's versions the client advertises, as the caps confirm takes it */
static UINT32 confirmed(const UINT32* versions, size_t count, const UINT32* advertised, size_t n)
{
	for (size_t x = 0; x < count; x++)
	{
		for (size_t y = 0; y < n; y++)
		{
			if (versions[x] == advertised[y])
				return versions[x];
		}
	}
	return 0;
}

/* the caps versions a codec order gives, and what a client offering two of them gets */
WINPR_ATTR_NODISCARD
static BOOL test_caps_versions(void)
{
	BOOL ok = TRUE;
	UINT32 v[16] = { 0 };

	/* AV2 first: FRDP_2, FRDP_1, the 11x, the 10x (of what is built) */
	size_t count = shadow_gfx_caps_versions(order_av2_first, N, v, ARRAYSIZE(v));
	size_t at = 0;
#if defined(WITH_GFX_AV2)
	ok &= (at < count) && (v[at++] == RDPGFX_CAPVERSION_FRDP_2);
#endif
#if defined(WITH_GFX_AV1)
	ok &= (at < count) && (v[at++] == RDPGFX_CAPVERSION_FRDP_1);
#endif
#if defined(WITH_GFX_AZURE)
	ok &= (at < count) && (v[at++] == RDPGFX_CAPVERSION_115);
	at += 3;
#endif
	ok &= (at < count) && (v[at] == RDPGFX_CAPVERSION_107) && (count == at + 9) &&
	      (v[count - 1] == RDPGFX_CAPVERSION_10);
	if (!ok)
		(void)fprintf(stderr, "caps versions, AV2 first: wrong (%" PRIuz ")\n", count);

	/* HEVC first: the 11x before FRDP_2 and FRDP_1 */
	UINT32 m[16] = { 0 };
	const size_t mCount = shadow_gfx_caps_versions(order_hevc_first, N, m, ARRAYSIZE(m));
	BOOL mok = (mCount == count);
	at = 0;
#if defined(WITH_GFX_AZURE)
	mok &= (m[at++] == RDPGFX_CAPVERSION_115) && (m[at++] == RDPGFX_CAPVERSION_113) &&
	       (m[at++] == RDPGFX_CAPVERSION_112) && (m[at++] == RDPGFX_CAPVERSION_111);
#endif
#if defined(WITH_GFX_AV2)
	mok &= (m[at++] == RDPGFX_CAPVERSION_FRDP_2);
#endif
#if defined(WITH_GFX_AV1)
	mok &= (m[at++] == RDPGFX_CAPVERSION_FRDP_1);
#endif
	mok &= (m[at] == RDPGFX_CAPVERSION_107);
	if (!mok)
		(void)fprintf(stderr, "caps versions, HEVC first: wrong\n");
	ok &= mok;

	/* a client offering AV1 + HEVC, then AV2 + HEVC: HEVC on the Mac order, AV1 / AV2 on the
	 * other one */
	const UINT32 av1Hevc[] = { RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_115,
		                       RDPGFX_CAPVERSION_FRDP_1 };
	const UINT32 av2Hevc[] = { RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_115,
		                       RDPGFX_CAPVERSION_FRDP_2 };
#if defined(WITH_GFX_AZURE)
	ok &= confirmed(m, mCount, av1Hevc, ARRAYSIZE(av1Hevc)) == RDPGFX_CAPVERSION_115;
	ok &= confirmed(m, mCount, av2Hevc, ARRAYSIZE(av2Hevc)) == RDPGFX_CAPVERSION_115;
#endif
#if defined(WITH_GFX_AV1)
	ok &= confirmed(v, count, av1Hevc, ARRAYSIZE(av1Hevc)) == RDPGFX_CAPVERSION_FRDP_1;
#endif
#if defined(WITH_GFX_AV2)
	ok &= confirmed(v, count, av2Hevc, ARRAYSIZE(av2Hevc)) == RDPGFX_CAPVERSION_FRDP_2;
#endif
	/* a client without HEVC: the same on both orders */
	const UINT32 av1Only[] = { RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_FRDP_1 };
	ok &= confirmed(m, mCount, av1Only, ARRAYSIZE(av1Only)) ==
	      confirmed(v, count, av1Only, ARRAYSIZE(av1Only));
	if (!ok)
		(void)fprintf(stderr, "caps versions: a client offering HEVC got the wrong caps\n");

	/* a too small array: cut, never past it */
	UINT32 two[2] = { 0 };
	ok &= shadow_gfx_caps_versions(order_hevc_first, N, two, ARRAYSIZE(two)) == 2;
	return ok;
}

int TestShadowGfxCodecOrder(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	BOOL ok = test_default_order();
	ok &= test_caps_versions();

	/* none: this platform's default order */
	ok &= expect("none", nullptr, TRUE, defaults);

	/* listed first, the others after them in the default order */
	{
		SHADOW_GFX_CODEC two[N] = { SHADOW_GFX_CODEC_PROGRESSIVE, SHADOW_GFX_CODEC_RFX };
		size_t n = 2;
		for (size_t x = 0; (x < N) && (defaults[x] != SHADOW_GFX_CODEC_NONE); x++)
		{
			if ((defaults[x] != SHADOW_GFX_CODEC_PROGRESSIVE) &&
			    (defaults[x] != SHADOW_GFX_CODEC_RFX))
				two[n++] = defaults[x];
		}
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
