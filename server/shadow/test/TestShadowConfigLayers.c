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
#define BITRATE "FREERDP_SHADOW_H264_BITRATE"

/* The two options as freerdp-shadow-cli has them: the table belongs to the program */
static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "config-file", COMMAND_LINE_VALUE_REQUIRED, "<path>", nullptr, nullptr, -1, nullptr,
	  "The configuration file" },
	{ "gfx-codec-order", COMMAND_LINE_VALUE_REQUIRED, "<codec>[,<codec>...]", nullptr, nullptr, -1,
	  nullptr, "The order the GFX codecs are tried in" },
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

/* A new server parses argv; the first codec of its order and the H.264 bit rate must be as
 * expected. The file's settings stay in the environment afterwards, so every case starts with
 * both unset. */
WINPR_ATTR_NODISCARD
static BOOL expect(const char* what, char** argv, int argc, const char* env, BOOL accepted,
                   SHADOW_GFX_CODEC first, UINT32 bitRate)
{
	if (!set(ORDER, nullptr) || !set(BITRATE, nullptr) || (env && !set(ORDER, env)))
		return FALSE;

	rdpShadowServer* server = shadow_server_new();
	if (!server)
		return FALSE;
	const int status = shadow_server_parse_command_line(server, argc, argv, options);
	BOOL ok = ((status >= 0) == accepted);
	if (ok && accepted)
		ok = (server->GfxCodecOrder[0] == first) && (server->h264BitRate == bitRate);
	if (!ok)
		(void)fprintf(stderr,
		              "%s: status %d first codec %d bit rate %" PRIu32
		              ", expected %s, first codec %d bit rate %" PRIu32 "\n",
		              what, status, (int)server->GfxCodecOrder[0], server->h264BitRate,
		              accepted ? "accepted" : "refused", (int)first, bitRate);
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
	if (!file || !write_file(file, "# planar first, the H.264 cap at 3 Mbit/s\n"
	                               "FREERDP_SHADOW_GFX_CODEC_ORDER=planar\n"
	                               "  FREERDP_SHADOW_H264_BITRATE = \"3000\"\n"
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
		char* missing[] = { "freerdp-shadow-cli", "/config-file:/nonexistent/shadow.conf",
			                nullptr };

		/* the build's default first codec, with nothing set */
		SHADOW_GFX_CODEC defaultFirst = SHADOW_GFX_CODEC_NONE;
		if (!set(ORDER, nullptr) || !set(BITRATE, nullptr))
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
		ok &= expect("nothing set", none, 1, nullptr, TRUE, defaultFirst, 10000000);
		ok &= expect("file", withFile, 2, nullptr, TRUE, SHADOW_GFX_CODEC_PLANAR, 3000000);
		ok &=
		    expect("file and environment", withFile, 2, "rfx", TRUE, SHADOW_GFX_CODEC_RFX, 3000000);
		ok &= expect("file, environment and command line", withAll, 3, "rfx", TRUE,
		             SHADOW_GFX_CODEC_PROGRESSIVE, 3000000);
		ok &= expect("environment only", none, 1, "progressive,rfx", TRUE,
		             SHADOW_GFX_CODEC_PROGRESSIVE, 10000000);
		/* a wrong value in the environment: logged, the default order kept */
		ok &= expect("wrong environment", none, 1, "vp9", TRUE, defaultFirst, 10000000);
		/* a file named on the command line must be there */
		ok &= expect("missing file", missing, 2, nullptr, FALSE, defaultFirst, 10000000);

#if !defined(_WIN32) && !defined(__APPLE__)
		/* the default file, when there is one */
		{
			char* shadowDir = GetCombinedPath(dir, "freerdp");
			char* shadowDir2 = shadowDir ? GetCombinedPath(shadowDir, "shadow") : nullptr;
			char* defaultFile = shadowDir2 ? GetCombinedPath(shadowDir2, "shadow.conf") : nullptr;
			ok &= (defaultFile != nullptr) && winpr_PathMakePath(shadowDir2, nullptr) &&
			      write_file(defaultFile, "FREERDP_SHADOW_GFX_CODEC_ORDER=rfx\n");
			ok &= expect("default file", none, 1, nullptr, TRUE, SHADOW_GFX_CODEC_RFX, 10000000);
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
	(void)set(BITRATE, nullptr);
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
