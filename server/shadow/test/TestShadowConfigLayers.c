#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/wtypes.h>
#include <winpr/cmdline.h>
#include <winpr/file.h>
#include <winpr/path.h>
#include <winpr/thread.h>
#include <freerdp/server/shadow.h>

#define ORDER "FREERDP_SHADOW_GFX_CLIENT_ORDER"
#define BITRATE "FREERDP_SHADOW_H264_BITRATE"

/* The two options as freerdp-shadow-cli has them: the table belongs to the program */
static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "config-file", COMMAND_LINE_VALUE_REQUIRED, "<path>", nullptr, nullptr, -1, nullptr,
	  "The configuration file" },
	{ "gfx-client-order", COMMAND_LINE_VALUE_BOOL, nullptr, BoolValueTrue, nullptr, -1, nullptr,
	  "Follow the order of the client's GFX capability sets" },
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

/* A new server parses argv; the switch and the H.264 bit rate must be as expected. The file's
 * settings stay in the environment afterwards, so every case starts with both unset. */
WINPR_ATTR_NODISCARD
static BOOL expect(const char* what, char** argv, int argc, const char* env, BOOL accepted,
                   BOOL follow, UINT32 bitRate)
{
	if (!set(ORDER, nullptr) || !set(BITRATE, nullptr) || (env && !set(ORDER, env)))
		return FALSE;

	rdpShadowServer* server = shadow_server_new();
	if (!server)
		return FALSE;
	const int status = shadow_server_parse_command_line(server, argc, argv, options);
	BOOL ok = ((status >= 0) == accepted);
	if (ok && accepted)
		ok = (server->GfxFollowClientCapsOrder == follow) && (server->h264BitRate == bitRate);
	if (!ok)
		(void)fprintf(stderr,
		              "%s: status %d follow %d bit rate %" PRIu32
		              ", expected %s, follow %d bit rate %" PRIu32 "\n",
		              what, status, server->GfxFollowClientCapsOrder, server->h264BitRate,
		              accepted ? "accepted" : "refused", follow, bitRate);
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
	file = GetCombinedPath(dir, "shadow.conf");
	if (!file || !write_file(file, "# the switch off, the H.264 cap at 3 Mbit/s\n"
	                               "FREERDP_SHADOW_GFX_CLIENT_ORDER=off\n"
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
		char* withAll[] = { "freerdp-shadow-cli", option, "+gfx-client-order", nullptr };
		char* missing[] = { "freerdp-shadow-cli", "/config-file:/nonexistent/shadow.conf",
			                nullptr };

		BOOL ok = TRUE;
		/* the default, then each layer over the one before it: file, environment, command line */
		ok &= expect("file", withFile, 2, nullptr, TRUE, FALSE, 3000000);
		ok &= expect("file and environment", withFile, 2, "on", TRUE, TRUE, 3000000);
		ok &= expect("file, environment and command line", withAll, 3, "off", TRUE, TRUE, 3000000);
		ok &= expect("environment only", none, 1, "off", TRUE, FALSE, 10000000);
		/* a file named on the command line must be there */
		ok &= expect("missing file", missing, 2, nullptr, FALSE, TRUE, 10000000);

#if !defined(_WIN32) && !defined(__APPLE__)
		/* the default file: <XDG_CONFIG_HOME>/freerdp/shadow/shadow.conf */
		{
			const char* home = getenv("XDG_CONFIG_HOME");
			char* oldHome = home ? _strdup(home) : nullptr;
			char* shadowDir = GetCombinedPath(dir, "freerdp");
			char* shadowDir2 = shadowDir ? GetCombinedPath(shadowDir, "shadow") : nullptr;
			char* defaultFile = shadowDir2 ? GetCombinedPath(shadowDir2, "shadow.conf") : nullptr;
			ok &= (defaultFile != nullptr) && set("XDG_CONFIG_HOME", dir);
			ok &= expect("no default file", none, 1, nullptr, TRUE, TRUE, 10000000);
			ok &= (shadowDir2 != nullptr) && winpr_PathMakePath(shadowDir2, nullptr) &&
			      write_file(defaultFile, "FREERDP_SHADOW_GFX_CLIENT_ORDER=off\n");
			ok &= expect("default file", none, 1, nullptr, TRUE, FALSE, 10000000);
			if (defaultFile)
				(void)winpr_DeleteFile(defaultFile);
			if (shadowDir2)
				(void)winpr_RemoveDirectory(shadowDir2);
			if (shadowDir)
				(void)winpr_RemoveDirectory(shadowDir);
			ok &= set("XDG_CONFIG_HOME", oldHome);
			free(oldHome);
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
	if (file)
		(void)winpr_DeleteFile(file);
	if (dir)
		(void)winpr_RemoveDirectory(dir);
	free(file);
	free(dir);
	free(temp);
	return rc;
}
