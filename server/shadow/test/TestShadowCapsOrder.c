#include <stdio.h>
#include <string.h>

#include <freerdp/config.h>

#include <winpr/wtypes.h>
#include <winpr/cmdline.h>
#include <freerdp/server/shadow.h>

#include "../shadow_caps_order.h"

/* The server's own order of the versions past 8.1, as shadow_caps_order.c has it */
static const UINT32 server_order[] = {
#if defined(WITH_GFX_AV2)
	RDPGFX_CAPVERSION_FRDP_2,
#endif
#if defined(WITH_GFX_AV1)
	RDPGFX_CAPVERSION_FRDP_1,
#endif
#if defined(WITH_GFX_AZURE)
	RDPGFX_CAPVERSION_115,    RDPGFX_CAPVERSION_113, RDPGFX_CAPVERSION_112,
	RDPGFX_CAPVERSION_111,
#endif
	RDPGFX_CAPVERSION_107,    RDPGFX_CAPVERSION_106, RDPGFX_CAPVERSION_106_ERR,
	RDPGFX_CAPVERSION_105,    RDPGFX_CAPVERSION_104, RDPGFX_CAPVERSION_103,
	RDPGFX_CAPVERSION_102,    RDPGFX_CAPVERSION_101, RDPGFX_CAPVERSION_10,
};

/* A FreeRDP client's Caps Advertise today (channels/rdpgfx/client): ascending */
static const UINT32 ascending[] = {
	RDPGFX_CAPVERSION_8,      RDPGFX_CAPVERSION_81,  RDPGFX_CAPVERSION_10,  RDPGFX_CAPVERSION_101,
	RDPGFX_CAPVERSION_102,    RDPGFX_CAPVERSION_103, RDPGFX_CAPVERSION_104, RDPGFX_CAPVERSION_107,
#if defined(WITH_GFX_AZURE)
	RDPGFX_CAPVERSION_111,    RDPGFX_CAPVERSION_112, RDPGFX_CAPVERSION_113, RDPGFX_CAPVERSION_115,
#endif
#if defined(WITH_GFX_AV1)
	RDPGFX_CAPVERSION_FRDP_1,
#endif
#if defined(WITH_GFX_AV2)
	RDPGFX_CAPVERSION_FRDP_2,
#endif
};

static size_t make_sets(const UINT32* versions, size_t count, RDPGFX_CAPSET* sets)
{
	for (size_t x = 0; x < count; x++)
	{
		sets[x].version = versions[x];
		sets[x].length = 4;
		sets[x].flags = 0;
	}
	return count;
}

WINPR_ATTR_NODISCARD
static BOOL same(const char* what, const UINT32* got, size_t gotCount, const UINT32* want,
                 size_t wantCount)
{
	BOOL ok = (gotCount == wantCount);
	for (size_t x = 0; ok && (x < gotCount); x++)
		ok = (got[x] == want[x]);
	if (!ok)
	{
		(void)fprintf(stderr, "%s: got", what);
		for (size_t x = 0; x < gotCount; x++)
			(void)fprintf(stderr, " 0x%08" PRIx32, got[x]);
		(void)fprintf(stderr, ", expected");
		for (size_t x = 0; x < wantCount; x++)
			(void)fprintf(stderr, " 0x%08" PRIx32, want[x]);
		(void)fprintf(stderr, "\n");
	}
	return ok;
}

/* Today's client, ascending, and the same list descending (as a client might list them highest
 * first): no order of their own, so the server's order, as before */
WINPR_ATTR_NODISCARD
static BOOL test_monotonic(void)
{
	RDPGFX_CAPSET sets[32] = { 0 };
	size_t count = make_sets(ascending, ARRAYSIZE(ascending), sets);
	UINT32 got[32] = { 0 };
	size_t gotCount = shadow_caps_order(sets, count, TRUE, got, ARRAYSIZE(got));
	BOOL ok = !shadow_caps_reordered(sets, count);
	ok &= same("ascending client", got, gotCount, server_order, ARRAYSIZE(server_order));

	UINT32 descending[ARRAYSIZE(ascending)] = { 0 };
	for (size_t x = 0; x < ARRAYSIZE(ascending); x++)
		descending[x] = ascending[ARRAYSIZE(ascending) - 1 - x];
	count = make_sets(descending, ARRAYSIZE(descending), sets);
	gotCount = shadow_caps_order(sets, count, TRUE, got, ARRAYSIZE(got));
	ok &= !shadow_caps_reordered(sets, count);
	ok &= same("descending client", got, gotCount, server_order, ARRAYSIZE(server_order));

	/* 10.6 then 10.6_ERR is one set twice, not an order: still ascending */
	{
		const UINT32 client[] = { RDPGFX_CAPVERSION_8,       RDPGFX_CAPVERSION_10,
			                      RDPGFX_CAPVERSION_105,     RDPGFX_CAPVERSION_106,
			                      RDPGFX_CAPVERSION_106_ERR, RDPGFX_CAPVERSION_107 };
		count = make_sets(client, ARRAYSIZE(client), sets);
		gotCount = shadow_caps_order(sets, count, TRUE, got, ARRAYSIZE(got));
		ok &= !shadow_caps_reordered(sets, count);
		ok &= same("10.6 and 10.6_ERR", got, gotCount, server_order, ARRAYSIZE(server_order));
	}
	return ok;
}

/* The server's own order without following, whatever the client sends */
WINPR_ATTR_NODISCARD
static BOOL test_server_order(void)
{
	const UINT32 client[] = { RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_10 };
	RDPGFX_CAPSET sets[2] = { 0 };
	const size_t count = make_sets(client, ARRAYSIZE(client), sets);

	UINT32 got[32] = { 0 };
	const size_t gotCount = shadow_caps_order(sets, count, FALSE, got, ARRAYSIZE(got));
	return same("not followed", got, gotCount, server_order, ARRAYSIZE(server_order));
}

/* A client with an order of its own puts the set it prefers last */
WINPR_ATTR_NODISCARD
static BOOL test_reordered(void)
{
	BOOL ok = TRUE;

	/* 10.7 preferred over every other */
	{
		/* 10.0, 10.4, then 8.0: an order of its own in every build */
		const UINT32 client[] = { RDPGFX_CAPVERSION_10,     RDPGFX_CAPVERSION_104,
			                      RDPGFX_CAPVERSION_8,
#if defined(WITH_GFX_AV1)
			                      RDPGFX_CAPVERSION_FRDP_1,
#endif
#if defined(WITH_GFX_AV2)
			                      RDPGFX_CAPVERSION_FRDP_2,
#endif
			                      RDPGFX_CAPVERSION_107 };
		RDPGFX_CAPSET sets[8] = { 0 };
		const size_t count = make_sets(client, ARRAYSIZE(client), sets);
		UINT32 got[32] = { 0 };
		const size_t gotCount = shadow_caps_order(sets, count, TRUE, got, ARRAYSIZE(got));
		ok &= (gotCount > 0) && (got[0] == RDPGFX_CAPVERSION_107);
		if (!ok)
			(void)fprintf(stderr, "10.7 last: not tried first\n");
	}

#if defined(WITH_GFX_AV1)
	/* FreeRDP's client with /gfx-codec-order:progressive,AV1: AV1's set first, the 8.x and 10.x
	 * sets last, ascending among themselves */
	{
		const UINT32 client[] = { RDPGFX_CAPVERSION_FRDP_1, RDPGFX_CAPVERSION_8,
			                      RDPGFX_CAPVERSION_81,     RDPGFX_CAPVERSION_10,
			                      RDPGFX_CAPVERSION_104,    RDPGFX_CAPVERSION_107 };
		const UINT32 want[] = { RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_104, RDPGFX_CAPVERSION_10,
			                    RDPGFX_CAPVERSION_FRDP_1 };
		RDPGFX_CAPSET sets[6] = { 0 };
		const size_t count = make_sets(client, ARRAYSIZE(client), sets);
		UINT32 got[32] = { 0 };
		const size_t gotCount = shadow_caps_order(sets, count, TRUE, got, ARRAYSIZE(got));
		ok &= shadow_caps_reordered(sets, count);
		ok &= same("progressive first", got, gotCount, want, ARRAYSIZE(want));
	}

	/* The rule's known limit: a list of its own that happens to be descending (only FRDP_1 and
	 * 10.7, 10.7 preferred) cannot be told from a descending one, so it gets the server's order */
	{
		const UINT32 client[] = { RDPGFX_CAPVERSION_FRDP_1, RDPGFX_CAPVERSION_107 };
		RDPGFX_CAPSET sets[2] = { 0 };
		const size_t count = make_sets(client, ARRAYSIZE(client), sets);
		UINT32 got[32] = { 0 };
		const size_t gotCount = shadow_caps_order(sets, count, TRUE, got, ARRAYSIZE(got));
		ok &= !shadow_caps_reordered(sets, count);
		ok &= same("two sets, descending", got, gotCount, server_order, ARRAYSIZE(server_order));
	}
#endif

#if defined(WITH_GFX_AV1) && defined(WITH_GFX_AV2)
	/* AV1 preferred over AV2 */
	{
		const UINT32 client[] = { RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_FRDP_2,
			                      RDPGFX_CAPVERSION_FRDP_1 };
		const UINT32 want[] = { RDPGFX_CAPVERSION_FRDP_1, RDPGFX_CAPVERSION_FRDP_2,
			                    RDPGFX_CAPVERSION_107 };
		RDPGFX_CAPSET sets[3] = { 0 };
		const size_t count = make_sets(client, ARRAYSIZE(client), sets);
		UINT32 got[32] = { 0 };
		const size_t gotCount = shadow_caps_order(sets, count, TRUE, got, ARRAYSIZE(got));
		ok &= same("AV1 last", got, gotCount, want, ARRAYSIZE(want));
	}
#endif
	return ok;
}

/* A version the server does not know, 8.x, and one given twice are not in the list */
WINPR_ATTR_NODISCARD
static BOOL test_skipped(void)
{
	const UINT32 client[] = { RDPGFX_CAPVERSION_107, 0x00FF0000u,          RDPGFX_CAPVERSION_81,
		                      RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_10, RDPGFX_CAPVERSION_8 };
	const UINT32 want[] = { RDPGFX_CAPVERSION_10, RDPGFX_CAPVERSION_107 };
	RDPGFX_CAPSET sets[6] = { 0 };
	const size_t count = make_sets(client, ARRAYSIZE(client), sets);
	UINT32 got[32] = { 0 };
	const size_t gotCount = shadow_caps_order(sets, count, TRUE, got, ARRAYSIZE(got));
	BOOL ok = same("unknown, 8.x and twice", got, gotCount, want, ARRAYSIZE(want));

	/* no room: nothing written past it */
	UINT32 one[1] = { 0 };
	ok &= (shadow_caps_order(sets, count, TRUE, one, ARRAYSIZE(one)) == 1) &&
	      (one[0] == RDPGFX_CAPVERSION_10);
	if (!ok)
		(void)fprintf(stderr, "no room: not the one set expected\n");

	/* no sets, no order of the client's: the server's */
	ok &= (shadow_caps_order(nullptr, 0, TRUE, one, ARRAYSIZE(one)) == 1) &&
	      (one[0] == server_order[0]);
	if (!ok)
		(void)fprintf(stderr, "no sets: not the server's order\n");
	return ok;
}

/* +gfx-client-order / -gfx-client-order, with its own copy of the program's table entry */
static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "gfx-client-order", COMMAND_LINE_VALUE_BOOL, nullptr, BoolValueTrue, nullptr, -1, nullptr,
	  "Follow the order of the client's GFX capability sets" },
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
};

WINPR_ATTR_NODISCARD
static BOOL test_switch(const char* option, BOOL expected)
{
	char* argv[] = { "freerdp-shadow-cli", (char*)option, nullptr };
	rdpShadowServer* server = shadow_server_new();
	if (!server)
		return FALSE;
	const int status = shadow_server_parse_command_line(server, option ? 2 : 1, argv, options);
	const BOOL ok = (status >= 0) && (server->GfxFollowClientCapsOrder == expected);
	if (!ok)
		(void)fprintf(stderr, "%s: status %d, follow %d, expected %d\n",
		              option ? option : "no option", status, server->GfxFollowClientCapsOrder,
		              expected);
	shadow_server_free(server);
	return ok;
}

int TestShadowCapsOrder(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	BOOL ok = TRUE;
	ok &= test_monotonic();
	ok &= test_server_order();
	ok &= test_reordered();
	ok &= test_skipped();
	ok &= test_switch(nullptr, TRUE);
	ok &= test_switch("-gfx-client-order", FALSE);
	ok &= test_switch("+gfx-client-order", TRUE);
	return ok ? 0 : -1;
}
