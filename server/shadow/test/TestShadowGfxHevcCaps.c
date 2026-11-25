#include <stdio.h>

#include <freerdp/config.h>

#include <winpr/wtypes.h>
#include <freerdp/channels/rdpgfx.h>

#include "../shadow_gfx_caps.h"

/* The HEVC flags of the shadow server's GFX caps confirm, and whether it then sends HEVC: a client
 * gets HEVC only when it offered it, whatever the server's /gfx-hevc says. */
typedef struct
{
	const char* what;
	UINT32 version;
	UINT32 offered; /* the client's capability set flags */
	BOOL hevc;      /* the server takes HEVC 4:2:0 */
	BOOL hevc444;   /* the server takes HEVC 4:4:4 */
	UINT32 confirm; /* the HEVC flags the confirm must carry */
	BOOL used;      /* the server sends HEVC */
} hevc_case;

#define AVC_OFF RDPGFX_CAPS_FLAG_AVC_DISABLED
#define HEVC RDPGFX_CAPS_FLAG_HEVC_ENABLE
#define I444 RDPGFX_CAPS_FLAG_HEVC_I444

static const hevc_case cases[] = {
	/* e.g. FreeRDP Web's "Least CPU" without hardware AV1: AVC420 only, no HEVC asked for */
	{ "11.2, AVC off, no HEVC offered, server HEVC on", RDPGFX_CAPVERSION_112, AVC_OFF, TRUE, FALSE,
	  0, FALSE },
	{ "11.5, AVC off, no HEVC offered, server HEVC + 4:4:4 on", RDPGFX_CAPVERSION_115, AVC_OFF,
	  TRUE, TRUE, 0, FALSE },
	{ "11.2, AVC off, HEVC offered, server HEVC on", RDPGFX_CAPVERSION_112, AVC_OFF | HEVC, TRUE,
	  FALSE, HEVC, TRUE },
	{ "11.2, AVC off, HEVC offered, server HEVC off", RDPGFX_CAPVERSION_112, AVC_OFF | HEVC, FALSE,
	  FALSE, 0, FALSE },
	{ "11.5, HEVC + 4:4:4 offered, server 4:2:0 only", RDPGFX_CAPVERSION_115, AVC_OFF | HEVC | I444,
	  TRUE, FALSE, HEVC, TRUE },
	{ "11.5, HEVC + 4:4:4 offered, server both", RDPGFX_CAPVERSION_115, AVC_OFF | HEVC | I444, TRUE,
	  TRUE, HEVC | I444, TRUE },
	{ "11.5, only 4:4:4 offered (no HEVC_ENABLE)", RDPGFX_CAPVERSION_115, AVC_OFF | I444, TRUE,
	  TRUE, 0, FALSE },
	/* FreeRDP Web's /gfx-codec-order:HEVC,AVC420: HEVC with AVC on as the fallback */
	{ "11.5, AVC on, HEVC offered, server HEVC on", RDPGFX_CAPVERSION_115, HEVC, TRUE, FALSE, HEVC,
	  TRUE },
	{ "11.2, AVC on, HEVC offered, server HEVC on", RDPGFX_CAPVERSION_112, HEVC, TRUE, FALSE, HEVC,
	  TRUE },
	{ "11.5, AVC on, HEVC offered, server HEVC off", RDPGFX_CAPVERSION_115, HEVC, FALSE, FALSE, 0,
	  FALSE },
	/* HEVC is not valid before 11.2 */
	{ "11.1, AVC on, HEVC offered, server HEVC on", RDPGFX_CAPVERSION_111, HEVC, TRUE, FALSE, HEVC,
	  FALSE },
};

int TestShadowGfxHevcCaps(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	int rc = 0;
	for (size_t x = 0; x < ARRAYSIZE(cases); x++)
	{
		const hevc_case* c = &cases[x];
		/* the confirm starts as a copy of the offered set, as shadow_client.c makes it */
		const UINT32 confirm =
		    shadow_gfx_hevc_confirm_flags(c->offered, c->offered, c->hevc, c->hevc444);
		const UINT32 got = confirm & (HEVC | I444);
		const BOOL used = shadow_gfx_hevc_used(c->version, confirm, c->hevc, c->hevc444);
		const BOOL ok = (got == c->confirm) && (used == c->used) &&
		                ((confirm & ~(HEVC | I444)) == (c->offered & ~(HEVC | I444)));
		(void)fprintf(stderr,
		              "%s: %s: confirm HEVC flags 0x%04" PRIx32 " (want 0x%04" PRIx32
		              "), HEVC sent %s (want %s)\n",
		              ok ? "ok" : "WRONG", c->what, got, c->confirm, used ? "yes" : "no",
		              c->used ? "yes" : "no");
		if (!ok)
			rc = -1;
	}
	return rc;
}
