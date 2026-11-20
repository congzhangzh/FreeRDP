#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/wtypes.h>
#include <winpr/cmdline.h>
#include <winpr/file.h>
#include <winpr/path.h>
#include <winpr/thread.h>
#include <freerdp/server/shadow.h>

#define ORDER "FREERDP_SHADOW_GFX_CODEC_ORDER"
#define CODEC "FREERDP_SHADOW_CODEC"

/* The options as freerdp-shadow-cli has them: the table belongs to the program */
static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "config-file", COMMAND_LINE_VALUE_REQUIRED, "<path>", nullptr, nullptr, -1, nullptr,
	  "The configuration file" },
	{ "gfx-codec-order", COMMAND_LINE_VALUE_REQUIRED, "<codec>[,<codec>...]", nullptr, nullptr, -1,
	  nullptr, "The order the GFX codecs are tried in" },
	{ "codec", COMMAND_LINE_VALUE_REQUIRED, "<codec>:<key>:<value>[,<codec>:<key>:<value>...]",
	  nullptr, nullptr, -1, nullptr, "Encoder settings" },
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
};

/* The server reads the settings with getenv: the C runtime's environment. nullptr unsets. */
WINPR_ATTR_NODISCARD
static BOOL set(const char* name, const char* value)
{
#if defined(_WIN32)
	return _putenv_s(name, value ? value : "") == 0;
#else
	if (!value)
		return unsetenv(name) == 0;
	return setenv(name, value, 1) == 0;
#endif
}

WINPR_ATTR_NODISCARD
static BOOL write_file(const char* path, const char* text)
{
	FILE* fp = winpr_fopen(path, "w");
	if (!fp)
		return FALSE;
	const BOOL ok = fputs(text, fp) >= 0;
	return (fclose(fp) == 0) && ok;
}

/* A new server parses argv; the first codec of its order, the H.264 bit rate and QP must be as
 * expected. env sets FREERDP_SHADOW_GFX_CODEC_ORDER, codecEnv FREERDP_SHADOW_CODEC. The file's
 * settings stay in the environment afterwards, so every case starts with both unset. */
WINPR_ATTR_NODISCARD
static BOOL expect(const char* what, char** argv, int argc, const char* env, const char* codecEnv,
                   BOOL accepted, SHADOW_GFX_CODEC first, UINT32 bitRate, UINT32 qp)
{
	if (!set(ORDER, nullptr) || !set(CODEC, nullptr) || (env && !set(ORDER, env)) ||
	    (codecEnv && !set(CODEC, codecEnv)))
		return FALSE;

	rdpShadowServer* server = shadow_server_new();
	if (!server)
		return FALSE;
	const int status = shadow_server_parse_command_line(server, argc, argv, options);
	BOOL ok = ((status >= 0) == accepted);
	if (ok && accepted)
		ok = (server->GfxCodecOrder[0] == first) && (server->h264BitRate == bitRate) &&
		     (server->h264QP == qp);
	if (!ok)
		(void)fprintf(stderr,
		              "%s: status %d first codec %d bit rate %" PRIu32 " qp %" PRIu32
		              ", expected %s, first codec %d bit rate %" PRIu32 " qp %" PRIu32 "\n",
		              what, status, (int)server->GfxCodecOrder[0], server->h264BitRate,
		              server->h264QP, accepted ? "accepted" : "refused", (int)first, bitRate, qp);
	shadow_server_free(server);
	return ok;
}

int TestShadowConfigLayers(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	int rc = -1;
	char* temp = GetKnownPath(KNOWN_PATH_TEMP);
	char* dir = nullptr;
	char* file = nullptr;
	char* oldHome = nullptr;
	char option[512] = { 0 };
	if (!temp)
		goto fail;
	{
		char sub[64] = { 0 };
		(void)_snprintf(sub, sizeof(sub), "TestShadowConfigLayers-%" PRIu32, GetCurrentProcessId());
		dir = GetCombinedPath(temp, sub);
	}
	if (!dir || !winpr_PathMakePath(dir, nullptr))
		goto fail;

#if !defined(_WIN32) && !defined(__APPLE__)
	/* the default file is <XDG_CONFIG_HOME>/freerdp/shadow/shadow.conf: the test's own, never the
	 * user's */
	{
		const char* home = getenv("XDG_CONFIG_HOME");
		oldHome = home ? _strdup(home) : nullptr;
		if (!set("XDG_CONFIG_HOME", dir))
			goto fail;
	}
#endif

	file = GetCombinedPath(dir, "shadow.conf");
	if (!file || !write_file(file, "# planar first, H.264 at 3 Mbit/s and qp 20\n"
	                               "FREERDP_SHADOW_GFX_CODEC_ORDER=planar\n"
	                               "  FREERDP_SHADOW_CODEC = \"h264:bitrate:3000,h264:qp:20\"\n"
	                               "\n"
	                               "; not ours, and not a setting: both passed over\n"
	                               "OTHER_SETTING=1\n"
	                               "not a line\n"))
		goto fail;
	(void)_snprintf(option, sizeof(option), "/config-file:%s", file);

	{
		char* none[] = { "freerdp-shadow-cli", nullptr };
		char* withFile[] = { "freerdp-shadow-cli", option, nullptr };
		char* withAll[] = { "freerdp-shadow-cli", option, "/gfx-codec-order:progressive", nullptr };
		char* withCodec[] = { "freerdp-shadow-cli", option, "/codec:h264:bitrate:2000", nullptr };
		char* missing[] = { "freerdp-shadow-cli", "/config-file:/nonexistent/shadow.conf",
			                nullptr };

		/* the build's default first codec, with nothing set */
		SHADOW_GFX_CODEC defaultFirst = SHADOW_GFX_CODEC_NONE;
		if (!set(ORDER, nullptr) || !set(CODEC, nullptr))
			goto fail;
		{
			rdpShadowServer* server = shadow_server_new();
			if (!server)
				goto fail;
			defaultFirst = server->GfxCodecOrder[0];
			shadow_server_free(server);
		}

		BOOL ok = (defaultFirst != SHADOW_GFX_CODEC_NONE);
		/* the default, then each layer over the one before it: file, environment, command line */
		ok =
		    expect("nothing set", none, 1, nullptr, nullptr, TRUE, defaultFirst, 10000000, 0) && ok;
		ok = expect("file", withFile, 2, nullptr, nullptr, TRUE, SHADOW_GFX_CODEC_PLANAR, 3000000,
		            20) &&
		     ok;
		ok = expect("file and environment", withFile, 2, "rfx", nullptr, TRUE, SHADOW_GFX_CODEC_RFX,
		            3000000, 20) &&
		     ok;
		ok = expect("file, environment and command line", withAll, 3, "rfx", nullptr, TRUE,
		            SHADOW_GFX_CODEC_PROGRESSIVE, 3000000, 20) &&
		     ok;
		ok = expect("environment only", none, 1, "progressive,rfx", nullptr, TRUE,
		            SHADOW_GFX_CODEC_PROGRESSIVE, 10000000, 0) &&
		     ok;
		/* FREERDP_SHADOW_CODEC key by key: the file's bit rate, the environment's qp over the
		 * file's, the command line's bit rate over the file's */
		ok = expect("codec: file and environment", withFile, 2, nullptr, "h264:qp:30", TRUE,
		            SHADOW_GFX_CODEC_PLANAR, 3000000, 30) &&
		     ok;
		ok = expect("codec: file, environment and command line", withCodec, 3, nullptr,
		            "h264:qp:30", TRUE, SHADOW_GFX_CODEC_PLANAR, 2000000, 30) &&
		     ok;
		/* a wrong codec value in the environment: logged, the file's keys kept */
		ok = expect("codec: wrong environment", withFile, 2, nullptr, "h264:qp:99", TRUE,
		            SHADOW_GFX_CODEC_PLANAR, 3000000, 20) &&
		     ok;
		/* a wrong value in the environment: logged, the default order kept */
		ok =
		    expect("wrong environment", none, 1, "vp9", nullptr, TRUE, defaultFirst, 10000000, 0) &&
		    ok;
		/* a file named on the command line must be there */
		ok = expect("missing file", missing, 2, nullptr, nullptr, FALSE, defaultFirst, 10000000,
		            0) &&
		     ok;

#if !defined(_WIN32) && !defined(__APPLE__)
		/* the default file, when there is one */
		{
			char* shadowDir = GetCombinedPath(dir, "freerdp");
			char* shadowDir2 = shadowDir ? GetCombinedPath(shadowDir, "shadow") : nullptr;
			char* defaultFile = shadowDir2 ? GetCombinedPath(shadowDir2, "shadow.conf") : nullptr;
			ok &= (defaultFile != nullptr) && winpr_PathMakePath(shadowDir2, nullptr) &&
			      write_file(defaultFile, "FREERDP_SHADOW_GFX_CODEC_ORDER=rfx\n");
			ok = expect("default file", none, 1, nullptr, nullptr, TRUE, SHADOW_GFX_CODEC_RFX,
			            10000000, 0) &&
			     ok;
			if (defaultFile)
				(void)winpr_DeleteFile(defaultFile);
			if (shadowDir2)
				(void)winpr_RemoveDirectory(shadowDir2);
			if (shadowDir)
				(void)winpr_RemoveDirectory(shadowDir);
			free(defaultFile);
			free(shadowDir2);
			free(shadowDir);
		}
#endif
		if (ok)
			rc = 0;
	}

fail:
	(void)set(ORDER, nullptr);
	(void)set(CODEC, nullptr);
#if !defined(_WIN32) && !defined(__APPLE__)
	(void)set("XDG_CONFIG_HOME", oldHome);
#endif
	free(oldHome);
	if (file)
		(void)winpr_DeleteFile(file);
	if (dir)
		(void)winpr_RemoveDirectory(dir);
	free(file);
	free(dir);
	free(temp);
	return rc;
}
