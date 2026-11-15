#include <stdio.h>
#include <string.h>

#include "../sesman_env.h"

static size_t droppedCount = 0;
static char droppedWhy[8][64];

static void dropped(const char* entry, const char* why)
{
	if (droppedCount < 8)
		(void)snprintf(droppedWhy[droppedCount], sizeof(droppedWhy[droppedCount]), "%.20s: %s",
		               entry, why);
	droppedCount++;
}

static int check(const char* what, int ok)
{
	if (!ok)
		(void)fprintf(stderr, "%s: failed\n", what);
	return ok;
}

int TestSesmanEnv(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	char tooLong[SESMAN_ENV_LEN + 16] = "FREERDP_SHADOW_X=";
	memset(&tooLong[strlen(tooLong)], 'v', sizeof(tooLong) - strlen(tooLong) - 1);
	tooLong[sizeof(tooLong) - 1] = '\0';

	char* envp[] = { "PATH=/usr/bin",
		             "FREERDP_SHADOW_AV1_CQ_LEVEL=20",
		             "WLOG_LEVEL=DEBUG",
		             "FREERDP_SHADOW_H264_BITRATE=4000",
		             "FREERDP_SHADOW_MUTTER_VIRTUAL=1920x1080",
		             "FREERDP_SHADOWX=1",
		             "XFREERDP_SHADOW_AV2_QP=1",
		             tooLong,
		             "FREERDP_SHADOW_H264_RATE_CONTROL=vbr",
		             NULL };

	int ok = 1;
	char out[SESMAN_ENV_MAX][SESMAN_ENV_LEN];

	/* every FREERDP_SHADOW_* but the one sesman sets and the one that does not fit */
	droppedCount = 0;
	size_t n = sesman_shadow_env(envp, out, SESMAN_ENV_MAX, dropped);
	ok &= check("count", n == 3);
	ok &= check("av1", (n > 0) && (strcmp(out[0], "--setenv=FREERDP_SHADOW_AV1_CQ_LEVEL=20") == 0));
	ok &= check("h264 bitrate",
	            (n > 1) && (strcmp(out[1], "--setenv=FREERDP_SHADOW_H264_BITRATE=4000") == 0));
	ok &= check("h264 mode",
	            (n > 2) && (strcmp(out[2], "--setenv=FREERDP_SHADOW_H264_RATE_CONTROL=vbr") == 0));
	ok &= check("dropped", droppedCount == 2);
	ok &= check("own", strstr(droppedWhy[0], "sesman sets it itself") != NULL);
	ok &= check("too long", strstr(droppedWhy[1], "too long") != NULL);

	/* a full table: the rest is reported, not lost silently */
	droppedCount = 0;
	n = sesman_shadow_env(envp, out, 1, dropped);
	ok &= check("full: count", n == 1);
	ok &= check("full: dropped", droppedCount == 4);
	ok &= check("full: why", strstr(droppedWhy[0], "too many settings") != NULL);

	/* nothing to pass on */
	char* none[] = { "PATH=/usr/bin", NULL };
	droppedCount = 0;
	ok &= check("none", sesman_shadow_env(none, out, SESMAN_ENV_MAX, dropped) == 0);
	ok &= check("none: dropped", droppedCount == 0);
	ok &= check("null", sesman_shadow_env(NULL, out, SESMAN_ENV_MAX, NULL) == 0);

	return ok ? 0 : -1;
}
