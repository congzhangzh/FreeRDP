#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/wtypes.h>
#include <winpr/cmdline.h>
#include <winpr/file.h>
#include <winpr/path.h>
#include <winpr/thread.h>
#include <freerdp/server/shadow.h>

/* FREERDP_SHADOW_ENV_<NAME>=<value> reaches the shadow server's libraries as <NAME>=<value>, from
 * the file and from the environment, and never over a <NAME> that is set already. */

static COMMAND_LINE_ARGUMENT_A options[] = {
	{ "config-file", COMMAND_LINE_VALUE_REQUIRED, "<path>", nullptr, nullptr, -1, nullptr,
	  "The configuration file" },
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
};

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

WINPR_ATTR_NODISCARD
static BOOL expect(const char* name, const char* value)
{
	const char* got = getenv(name);
	const BOOL ok = value ? (got && (strcmp(got, value) == 0)) : (!got || (*got == '\0'));
	if (!ok)
		(void)fprintf(stderr, "%s: \"%s\", expected \"%s\"\n", name, got ? got : "(unset)",
		              value ? value : "(unset)");
	return ok;
}

int TestShadowConfigEnv(int argc, char* argv[])
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
		(void)_snprintf(sub, sizeof(sub), "TestShadowConfigEnv-%" PRIu32, GetCurrentProcessId());
		dir = GetCombinedPath(temp, sub);
	}
	if (!dir || !winpr_PathMakePath(dir, nullptr))
		goto fail;

	file = GetCombinedPath(dir, "shadow.conf");
	if (!file || !write_file(file, "FREERDP_SHADOW_ENV_TSCE_FROM_FILE=256/48000\n"
	                               "FREERDP_SHADOW_ENV_TSCE_KEPT=from the file\n"
	                               "FREERDP_SHADOW_ENV_TSCE_EMPTY=\n"))
		goto fail;
	(void)_snprintf(option, sizeof(option), "/config-file:%s", file);

	if (!set("TSCE_FROM_FILE", nullptr) || !set("TSCE_FROM_ENV", nullptr) ||
	    !set("TSCE_KEPT", "already") || !set("FREERDP_SHADOW_ENV_TSCE_FROM_ENV", "15") ||
	    !set("FREERDP_SHADOW_ENV_9BAD", "x") || !set("FREERDP_SHADOW_ENV_", "x"))
		goto fail;

	{
		char* withFile[] = { "freerdp-shadow-cli", option, nullptr };
		rdpShadowServer* server = shadow_server_new();
		if (!server)
			goto fail;
		const int status = shadow_server_parse_command_line(server, 2, withFile, options);
		shadow_server_free(server);
		if (status < 0)
		{
			(void)fprintf(stderr, "parse: %d\n", status);
			goto fail;
		}
	}

	if (!expect("TSCE_FROM_FILE", "256/48000") || !expect("TSCE_FROM_ENV", "15") ||
	    !expect("TSCE_KEPT", "already") || !expect("9BAD", nullptr))
		goto fail;

	rc = 0;
fail:
	if (file)
		(void)winpr_DeleteFile(file);
	if (dir)
		(void)winpr_RemoveDirectory(dir);
	free(file);
	free(dir);
	free(temp);
	return rc;
}
