/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2017 Armin Novak <armin.novak@thincast.com>
 * Copyright 2017 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <errno.h>
#include <stdint.h>

#include <winpr/assert.h>
#include <winpr/crt.h>
#include <winpr/ssl.h>
#include <winpr/path.h>
#include <winpr/cmdline.h>
#include <winpr/winsock.h>
#include <winpr/sysinfo.h>

#include <freerdp/log.h>
#include <freerdp/version.h>

#include <winpr/tools/makecert.h>

#ifndef _WIN32
#include <sys/select.h>
#include <signal.h>
#endif

#include "shadow.h"
#include "shadow_config.h"
#include "shadow_cliprdr.h"

#define TAG SERVER_TAG("shadow")

static const char bind_address[] = "bind-address,";

#define fail_at(arg, rc) fail_at_((arg), (rc), __FILE__, __func__, __LINE__)
static int fail_at_(const COMMAND_LINE_ARGUMENT_A* arg, int rc, const char* file, const char* fkt,
                    size_t line)
{
	const DWORD level = WLOG_ERROR;
	wLog* log = WLog_Get(TAG);
	if (WLog_IsLevelActive(log, level))
		WLog_PrintTextMessage(log, level, line, file, fkt,
		                      "Command line parsing failed at '%s' value '%s' [%d]", arg->Name,
		                      arg->Value, rc);
	return rc;
}

WINPR_ATTR_NODISCARD
static int command_line_compare(const void* pa, const void* pb)
{
	const COMMAND_LINE_ARGUMENT_A* a = pa;
	const COMMAND_LINE_ARGUMENT_A* b = pb;

	if (!a && !b)
		return 0;
	if (!a)
		return -1;
	if (!b)
		return 1;

	return strcmp(a->Name, b->Name);
}

WINPR_ATTR_NODISCARD
static int shadow_server_print_command_line_help(int argc, char** argv,
                                                 const COMMAND_LINE_ARGUMENT_A* largs)
{
	if ((argc < 1) || !largs || !argv)
		return -1;

	{
		char* path = winpr_GetConfigFilePath(TRUE, "SAM");
		printf("Usage: %s [options]\n", argv[0]);
		printf("\n");
		printf("Notes: By default NLA security is active.\n");
		printf("\tIn this mode a SAM database is required.\n");
		printf("\tProvide one with /sam-file:<file with path>\n");
		printf("\telse the default path %s is used.\n", path);
		printf("\tIf there is no existing SAM file authentication for all users will fail.\n");
		printf("\n\tIf authentication against PAM is desired, start with -sec-nla (requires "
		       "compiled in "
		       "support for PAM)\n\n");
		printf("Syntax:\n");
		printf("    /flag (enables flag)\n");
		printf("    /option:<value> (specifies option with value)\n");
		printf("    +toggle -toggle (enables or disables toggle, where '/' is a synonym of '+')\n");
		printf("\n");
		free(path);
	}

	// TODO: Sort arguments
	size_t nrArgs = 0;
	{
		const COMMAND_LINE_ARGUMENT_A* arg = largs;
		while (arg->Name != nullptr)
		{
			nrArgs++;
			arg++;
		}
		nrArgs++;
	}
	COMMAND_LINE_ARGUMENT_A* args_copy = calloc(nrArgs, sizeof(COMMAND_LINE_ARGUMENT_A));
	if (!args_copy)
		return -1;
	memcpy(args_copy, largs, nrArgs * sizeof(COMMAND_LINE_ARGUMENT_A));
	qsort(args_copy, nrArgs - 1, sizeof(COMMAND_LINE_ARGUMENT_A), command_line_compare);

	const COMMAND_LINE_ARGUMENT_A* arg = args_copy;

	int rc = -1;
	do
	{
		if (arg->Flags & COMMAND_LINE_VALUE_FLAG)
		{
			printf("    %s", "/");
			printf("%-20s\n", arg->Name);
			printf("\t%s\n", arg->Text);
		}
		else if ((arg->Flags & COMMAND_LINE_VALUE_REQUIRED) ||
		         (arg->Flags & COMMAND_LINE_VALUE_OPTIONAL))
		{
			printf("    %s", "/");

			if (arg->Format)
			{
				const size_t length = (strlen(arg->Name) + strlen(arg->Format) + 2);
				char* str = (char*)calloc(length + 1, sizeof(char));

				if (!str)
					goto fail;

				(void)sprintf_s(str, length + 1, "%s:%s", arg->Name, arg->Format);
				(void)printf("%-20s\n", str);
				free(str);
			}
			else
			{
				printf("%-20s\n", arg->Name);
			}

			printf("\t%s\n", arg->Text);
		}
		else if (arg->Flags & COMMAND_LINE_VALUE_BOOL)
		{
			const size_t length = strlen(arg->Name) + 32;
			char* str = calloc(length + 1, sizeof(char));

			if (!str)
				goto fail;

			(void)sprintf_s(str, length + 1, "%s (default:%s)", arg->Name,
			                arg->Default ? "on" : "off");
			(void)printf("    %s", arg->Default ? "-" : "+");
			(void)printf("%-20s\n", str);
			(void)printf("\t%s\n", arg->Text);

			free(str);
		}
	} while ((arg = CommandLineFindNextArgumentA(arg)) != nullptr);

	rc = 1;
fail:
	free(args_copy);
	return rc;
}

int shadow_server_command_line_status_print(rdpShadowServer* server, int argc, char** argv,
                                            int status, const COMMAND_LINE_ARGUMENT_A* cargs)
{
	WINPR_UNUSED(server);

	if (status == COMMAND_LINE_STATUS_PRINT_VERSION)
	{
		printf("FreeRDP version %s (git %s)\n", FREERDP_VERSION_FULL, FREERDP_GIT_REVISION);
		return COMMAND_LINE_STATUS_PRINT_VERSION;
	}
	else if (status == COMMAND_LINE_STATUS_PRINT_BUILDCONFIG)
	{
		printf("%s\n", freerdp_get_build_config());
		return COMMAND_LINE_STATUS_PRINT_BUILDCONFIG;
	}
	else if (status == COMMAND_LINE_STATUS_PRINT)
	{
		return COMMAND_LINE_STATUS_PRINT;
	}
	else if (status < 0)
	{
		if (shadow_server_print_command_line_help(argc, argv, cargs) < 0)
			return -1;

		return COMMAND_LINE_STATUS_PRINT_HELP;
	}

	return 1;
}

/* A whole number from min to max, nothing else */
WINPR_ATTR_NODISCARD
static BOOL shadow_server_parse_u32(const char* text, UINT32 min, UINT32 max, UINT32* value)
{
	WINPR_ASSERT(value);
	if (!text)
		return FALSE;

	char* end = nullptr;
	errno = 0;
	const unsigned long long parsed = strtoull(text, &end, 0);
	if ((errno != 0) || !end || (*end != '\0') || (parsed < min) || (parsed > max))
		return FALSE;
	*value = (UINT32)parsed;
	return TRUE;
}

/* The text after prefix, or nullptr when text does not start with it */
WINPR_ATTR_NODISCARD
static const char* shadow_server_after(const char* text, const char* prefix)
{
	const size_t length = strlen(prefix);
	return (strncmp(text, prefix, length) == 0) ? &text[length] : nullptr;
}

/* A bit rate given in kbit/s (1 to 1000000), stored in the unit of its field: unit 1 for kbit/s,
 * 1000 for bit/s */
WINPR_ATTR_NODISCARD
static BOOL shadow_server_parse_bitrate(const char* text, UINT32 unit, UINT32* value)
{
	UINT32 kbits = 0;
	if (!shadow_server_parse_u32(text, 1, 1000000, &kbits))
		return FALSE;
	*value = kbits * unit;
	return TRUE;
}

#if defined(WITH_GFX_AV1) || defined(WITH_GFX_AV2)
/* vbr, cbr, cq or q: the order of FREERDP_AV1_RATECONTROL and FREERDP_AV2_RATECONTROL */
WINPR_ATTR_NODISCARD
static BOOL shadow_server_parse_av_mode(const char* text, UINT32* mode)
{
	const char* names[] = { "vbr", "cbr", "cq", "q" };
	for (UINT32 x = 0; x < ARRAYSIZE(names); x++)
	{
		if (strcmp(text, names[x]) == 0)
		{
			*mode = x;
			return TRUE;
		}
	}
	return FALSE;
}
#endif

/* /codec:<codec>:<key>:<value>[,<codec>:<key>:<value>...], for the codecs built in:
 *   h264: rate-control:vbr|cqp, bitrate:<kbit/s>, qp:<0-51>
 *   av1, av2: rate-control:vbr|cbr|cq|q, bitrate:<kbit/s>
 *   hevc: rate-control:vbr, bitrate:<kbit/s>
 * Nothing changes unless the whole value is right. */
WINPR_ATTR_NODISCARD
static BOOL shadow_server_parse_codec(rdpShadowServer* server, const char* value)
{
	WINPR_ASSERT(server);

	H264_RATECONTROL_MODE h264Mode = server->h264RateControlMode;
	UINT32 h264BitRate = server->h264BitRate; /* bit/s */
	UINT32 h264QP = server->h264QP;
#if defined(WITH_GFX_AV1)
	UINT32 av1Mode = (UINT32)server->AV1RateControlMode;
	UINT32 av1BitRate = server->AV1BitRate; /* kbit/s */
#endif
#if defined(WITH_GFX_AV2)
	UINT32 av2Mode = (UINT32)server->AV2RateControlMode;
	UINT32 av2BitRate = server->AV2BitRate; /* kbit/s */
#endif
#if defined(WITH_GFX_AZURE)
	FREERDP_HEVC_RATECONTROL hevcMode = server->HEVCRateControlMode;
	UINT32 hevcBitRate = server->HEVCBitRate; /* bit/s: the H.264 context's */
#endif

	BOOL ok = (value != nullptr);
	size_t count = 0;
	char** args = ok ? CommandLineParseCommaSeparatedValues(value, &count) : nullptr;
	if (ok && !args)
		ok = FALSE;
	for (size_t x = 0; ok && (x < count); x++)
	{
		const char* cur = args[x];
		const char* rest = nullptr;
		const char* v = nullptr;
		if ((rest = shadow_server_after(cur, "h264:")) != nullptr)
		{
			if ((v = shadow_server_after(rest, "rate-control:")) != nullptr)
			{
				if (strcmp(v, "vbr") == 0)
					h264Mode = H264_RATECONTROL_VBR;
				else if (strcmp(v, "cqp") == 0)
					h264Mode = H264_RATECONTROL_CQP;
				else
					ok = FALSE;
			}
			else if ((v = shadow_server_after(rest, "bitrate:")) != nullptr)
				ok = shadow_server_parse_bitrate(v, 1000, &h264BitRate);
			else if ((v = shadow_server_after(rest, "qp:")) != nullptr)
				ok = shadow_server_parse_u32(v, 0, 51, &h264QP);
			else
				ok = FALSE;
		}
#if defined(WITH_GFX_AV1)
		else if ((rest = shadow_server_after(cur, "av1:")) != nullptr)
		{
			if ((v = shadow_server_after(rest, "rate-control:")) != nullptr)
				ok = shadow_server_parse_av_mode(v, &av1Mode);
			else if ((v = shadow_server_after(rest, "bitrate:")) != nullptr)
				ok = shadow_server_parse_bitrate(v, 1, &av1BitRate);
			else
				ok = FALSE;
		}
#endif
#if defined(WITH_GFX_AV2)
		else if ((rest = shadow_server_after(cur, "av2:")) != nullptr)
		{
			if ((v = shadow_server_after(rest, "rate-control:")) != nullptr)
				ok = shadow_server_parse_av_mode(v, &av2Mode);
			else if ((v = shadow_server_after(rest, "bitrate:")) != nullptr)
				ok = shadow_server_parse_bitrate(v, 1, &av2BitRate);
			else
				ok = FALSE;
		}
#endif
#if defined(WITH_GFX_AZURE)
		else if ((rest = shadow_server_after(cur, "hevc:")) != nullptr)
		{
			/* only vbr: the HEVC encoder hands its mode to the H.264 context, which knows VBR
			 * and CQP only, and FREERDP_HEVC_CBR has CQP's value */
			if ((v = shadow_server_after(rest, "rate-control:")) != nullptr)
			{
				if (strcmp(v, "vbr") == 0)
					hevcMode = FREERDP_HEVC_VBR;
				else
					ok = FALSE;
			}
			else if ((v = shadow_server_after(rest, "bitrate:")) != nullptr)
				ok = shadow_server_parse_bitrate(v, 1000, &hevcBitRate);
			else
				ok = FALSE;
		}
#endif
		else
			ok = FALSE; /* an unknown codec, or one not built in */
	}
	CommandLineParserFree(args);

	if (!ok)
		return FALSE;
	server->h264RateControlMode = h264Mode;
	server->h264BitRate = h264BitRate;
	server->h264QP = h264QP;
#if defined(WITH_GFX_AV1)
	server->AV1RateControlMode = (FREERDP_AV1_RATECONTROL)av1Mode;
	server->AV1BitRate = av1BitRate;
#endif
#if defined(WITH_GFX_AV2)
	server->AV2RateControlMode = (FREERDP_AV2_RATECONTROL)av2Mode;
	server->AV2BitRate = av2BitRate;
#endif
#if defined(WITH_GFX_AZURE)
	server->HEVCRateControlMode = hevcMode;
	server->HEVCBitRate = hevcBitRate;
#endif
	return TRUE;
}

/* The GFX codecs by the names /gfx uses for them, in the default order of
 * rdpShadowServer::GfxCodecOrder */
static const struct
{
	const char* name;
	SHADOW_GFX_CODEC codec;
} shadow_gfx_codecs[] = {
	{ "av2", SHADOW_GFX_CODEC_AV2 },
	{ "av1", SHADOW_GFX_CODEC_AV1 },
	{ "hevc", SHADOW_GFX_CODEC_HEVC },
	{ "avc444", SHADOW_GFX_CODEC_AVC444 },
	{ "avc420", SHADOW_GFX_CODEC_AVC420 },
	{ "rfx", SHADOW_GFX_CODEC_RFX },
	{ "progressive", SHADOW_GFX_CODEC_PROGRESSIVE },
	{ "planar", SHADOW_GFX_CODEC_PLANAR },
};

/* Whether this build has the codec's encoder */
WINPR_ATTR_NODISCARD
static BOOL shadow_gfx_codec_built(SHADOW_GFX_CODEC codec)
{
	switch (codec)
	{
		case SHADOW_GFX_CODEC_AV2:
#if defined(WITH_GFX_AV2)
			return TRUE;
#else
			return FALSE;
#endif
		case SHADOW_GFX_CODEC_AV1:
#if defined(WITH_GFX_AV1)
			return TRUE;
#else
			return FALSE;
#endif
		case SHADOW_GFX_CODEC_HEVC:
#if defined(WITH_GFX_AZURE)
			return TRUE;
#else
			return FALSE;
#endif
		case SHADOW_GFX_CODEC_AVC444:
		case SHADOW_GFX_CODEC_AVC420:
#if defined(WITH_GFX_H264)
			return TRUE;
#else
			return FALSE;
#endif
		case SHADOW_GFX_CODEC_RFX:
		case SHADOW_GFX_CODEC_PROGRESSIVE:
		case SHADOW_GFX_CODEC_PLANAR:
			return TRUE;
		default:
			return FALSE;
	}
}

/* The listed codecs first, then the others in the default order */
static void shadow_server_set_codec_order(rdpShadowServer* server, const SHADOW_GFX_CODEC* listed,
                                          size_t count)
{
	WINPR_ASSERT(server);
	WINPR_ASSERT(listed || (count == 0));

	size_t n = 0;
	for (size_t x = 0; x < count; x++)
		server->GfxCodecOrder[n++] = listed[x];
	for (size_t x = 0; x < ARRAYSIZE(shadow_gfx_codecs); x++)
	{
		BOOL found = FALSE;
		for (size_t y = 0; y < count; y++)
		{
			if (listed[y] == shadow_gfx_codecs[x].codec)
				found = TRUE;
		}
		if (!found)
			server->GfxCodecOrder[n++] = shadow_gfx_codecs[x].codec;
	}
	while (n < ARRAYSIZE(server->GfxCodecOrder))
		server->GfxCodecOrder[n++] = SHADOW_GFX_CODEC_NONE;
}

/* /gfx-codec-order:<name>[,<name>...]: it ranks the codecs, it does not switch any on. A name
 * /gfx does not know, a codec not built in or a name given twice is an error, and then the order
 * stays as it was. */
WINPR_ATTR_NODISCARD
static BOOL shadow_server_parse_codec_order(rdpShadowServer* server, const char* value)
{
	WINPR_ASSERT(server);
	if (!value)
		return FALSE;

	SHADOW_GFX_CODEC listed[ARRAYSIZE(shadow_gfx_codecs)] = WINPR_C_ARRAY_INIT;
	size_t count = 0;
	size_t argc = 0;
	char** args = CommandLineParseCommaSeparatedValues(value, &argc);
	BOOL ok = (args != nullptr) && (argc > 0);
	for (size_t x = 0; ok && (x < argc); x++)
	{
		SHADOW_GFX_CODEC codec = SHADOW_GFX_CODEC_NONE;
		for (size_t y = 0; y < ARRAYSIZE(shadow_gfx_codecs); y++)
		{
			if (_stricmp(args[x], shadow_gfx_codecs[y].name) == 0)
				codec = shadow_gfx_codecs[y].codec;
		}
		if ((codec == SHADOW_GFX_CODEC_NONE) || !shadow_gfx_codec_built(codec))
		{
			WLog_ERR(TAG, "/gfx-codec-order: '%s' is no codec of this build", args[x]);
			ok = FALSE;
			break;
		}
		for (size_t y = 0; y < count; y++)
		{
			if (listed[y] == codec)
			{
				WLog_ERR(TAG, "/gfx-codec-order: '%s' is given twice", args[x]);
				ok = FALSE;
			}
		}
		if (ok)
			listed[count++] = codec;
	}
	CommandLineParserFree(args);

	if (ok)
		shadow_server_set_codec_order(server, listed, count);
	return ok;
}

static void shadow_server_env_defaults(rdpShadowServer* server);

/* The configuration file, before the command line: /config-file:<path> (it must be readable), or
 * <config dir>/shadow/shadow.conf if there is one. Its settings come in under the environment's,
 * so the defaults are read again; the command line comes after them. */
WINPR_ATTR_NODISCARD
static BOOL shadow_server_load_config(rdpShadowServer* server, int argc, char** argv)
{
	const char* path = nullptr;
	for (int x = 1; argv && (x < argc); x++)
	{
		if (argv[x] && (strncmp(argv[x], "/config-file:", 13) == 0))
			path = &argv[x][13];
	}

	BOOL rc = FALSE;
	if (path)
		rc = shadow_config_load(path, TRUE);
	else
	{
		char* defaultPath = shadow_config_default_path();
		rc = shadow_config_load(defaultPath, FALSE);
		free(defaultPath);
	}
	if (rc)
		shadow_server_env_defaults(server);
	return rc;
}

int shadow_server_parse_command_line(rdpShadowServer* server, int argc, char** argv,
                                     COMMAND_LINE_ARGUMENT_A* cargs)
{
	int status = 0;
	DWORD flags = 0;
	const COMMAND_LINE_ARGUMENT_A* arg = nullptr;
	rdpSettings* settings = server->settings;

	if (!shadow_server_load_config(server, argc, argv))
		return COMMAND_LINE_ERROR;

	if ((argc < 2) || !argv || !cargs)
		return 1;

	CommandLineClearArgumentsA(cargs);
	flags = COMMAND_LINE_SEPARATOR_COLON;
	flags |= COMMAND_LINE_SIGIL_SLASH | COMMAND_LINE_SIGIL_PLUS_MINUS;
	status = CommandLineParseArgumentsA(argc, argv, cargs, flags, server, nullptr, nullptr);

	if (status < 0)
		return status;

	arg = cargs;
	errno = 0;

	do
	{
		if (!(arg->Flags & COMMAND_LINE_ARGUMENT_PRESENT))
			continue;

		CommandLineSwitchStart(arg) CommandLineSwitchCase(arg, "port")
		{
			long val = strtol(arg->Value, nullptr, 0);

			if ((errno != 0) || (val <= 0) || (val > UINT16_MAX))
				return fail_at(arg, COMMAND_LINE_ERROR);

			server->port = (DWORD)val;
		}
		CommandLineSwitchCase(arg, "ipc-socket")
		{
			/* /bind-address is incompatible */
			if (server->ipcSocket)
				return fail_at(arg, COMMAND_LINE_ERROR);
			server->ipcSocket = _strdup(arg->Value);

			if (!server->ipcSocket)
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "bind-address")
		{
			int rc = 0;
			size_t len = strlen(arg->Value) + sizeof(bind_address);
			/* /ipc-socket is incompatible */
			if (server->ipcSocket)
				return fail_at(arg, COMMAND_LINE_ERROR);
			server->ipcSocket = calloc(len, sizeof(CHAR));

			if (!server->ipcSocket)
				return fail_at(arg, COMMAND_LINE_ERROR);

			rc = _snprintf(server->ipcSocket, len, "%s%s", bind_address, arg->Value);
			if ((rc < 0) || ((size_t)rc != len - 1))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "may-view")
		{
			server->mayView = arg->Value != nullptr;
		}
		CommandLineSwitchCase(arg, "bitmap-compat")
		{
			server->SupportMultiRectBitmapUpdates = arg->Value == nullptr;
		}
		CommandLineSwitchCase(arg, "may-interact")
		{
			server->mayInteract = arg->Value != nullptr;
		}
		CommandLineSwitchCase(arg, "server-side-cursor")
		{
			server->ShowMouseCursor = arg->Value != nullptr;
		}
		CommandLineSwitchCase(arg, "mouse-relative")
		{
			const BOOL val = arg->Value != nullptr;
			if (!freerdp_settings_set_bool(settings, FreeRDP_MouseUseRelativeMove, val) ||
			    !freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, val))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "max-connections")
		{
			errno = 0;
			unsigned long val = strtoul(arg->Value, nullptr, 0);

			if ((errno != 0) || (val > UINT32_MAX))
				return fail_at(arg, COMMAND_LINE_ERROR);
			server->maxClientsConnected = val;
		}
		CommandLineSwitchCase(arg, "rect")
		{
			char* p = nullptr;
			char* tok[4];
			long x = -1;
			long y = -1;
			long w = -1;
			long h = -1;
			char* str = _strdup(arg->Value);

			if (!str)
				return fail_at(arg, COMMAND_LINE_ERROR);

			tok[0] = p = str;
			p = strchr(p + 1, ',');

			if (!p)
			{
				free(str);
				return fail_at(arg, COMMAND_LINE_ERROR);
			}

			*p++ = '\0';
			tok[1] = p;
			p = strchr(p + 1, ',');

			if (!p)
			{
				free(str);
				return fail_at(arg, COMMAND_LINE_ERROR);
			}

			*p++ = '\0';
			tok[2] = p;
			p = strchr(p + 1, ',');

			if (!p)
			{
				free(str);
				return fail_at(arg, COMMAND_LINE_ERROR);
			}

			*p++ = '\0';
			tok[3] = p;
			x = strtol(tok[0], nullptr, 0);

			if (errno != 0)
				goto fail;

			y = strtol(tok[1], nullptr, 0);

			if (errno != 0)
				goto fail;

			w = strtol(tok[2], nullptr, 0);

			if (errno != 0)
				goto fail;

			h = strtol(tok[3], nullptr, 0);

			if (errno != 0)
				goto fail;

		fail:
			free(str);

			if ((x < 0) || (y < 0) || (w < 1) || (h < 1) || (errno != 0))
				return fail_at(arg, COMMAND_LINE_ERROR);

			if ((x > UINT16_MAX) || (y > UINT16_MAX) || (x + w > UINT16_MAX) ||
			    (y + h > UINT16_MAX))
				return fail_at(arg, COMMAND_LINE_ERROR);
			server->subRect.left = (UINT16)x;
			server->subRect.top = (UINT16)y;
			server->subRect.right = (UINT16)(x + w);
			server->subRect.bottom = (UINT16)(y + h);
			server->shareSubRect = TRUE;
		}
		CommandLineSwitchCase(arg, "auth")
		{
			server->authentication = arg->Value != nullptr;
		}
		CommandLineSwitchCase(arg, "remote-guard")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_RemoteCredentialGuard,
			                               arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "restricted-admin")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_RestrictedAdminModeSupported,
			                               arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "vmconnect")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_VmConnectMode, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "sec")
		{
			if (strcmp("rdp", arg->Value) == 0) /* Standard RDP */
			{
				if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, TRUE))
					return fail_at(arg, COMMAND_LINE_ERROR);
			}
			else if (strcmp("tls", arg->Value) == 0) /* TLS */
			{
				if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
			}
			else if (strcmp("nla", arg->Value) == 0) /* NLA */
			{
				if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, TRUE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
			}
			else if (strcmp("ext", arg->Value) == 0) /* NLA Extended */
			{
				if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE))
					return fail_at(arg, COMMAND_LINE_ERROR);
				if (!freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, TRUE))
					return fail_at(arg, COMMAND_LINE_ERROR);
			}
			else
			{
				WLog_ERR(TAG, "unknown protocol security: %s", arg->Value);
				return fail_at(arg, COMMAND_LINE_ERROR_UNEXPECTED_VALUE);
			}
		}
		CommandLineSwitchCase(arg, "sec-rdp")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "sec-tls")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "sec-nla")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "sec-ext")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "sam-file")
		{
			if (!freerdp_settings_set_string(settings, FreeRDP_NtlmSamFile, arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "log-level")
		{
			wLog* root = WLog_GetRoot();

			if (!WLog_SetStringLogLevel(root, arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "log-filters")
		{
			if (!WLog_AddStringLogFilters(arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "nsc")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_NSCodec, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "rfx")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "gfx")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline,
			                               arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "codec")
		{
			if (!shadow_server_parse_codec(server, arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
#if defined(WITH_RDPEUDP_SERVER)
		CommandLineSwitchCase(arg, "multitransport")
		{
			if ((arg->Value != nullptr) &&
			    !freerdp_settings_set_uint32(settings, FreeRDP_MultitransportFlags,
			                                 TRANSPORT_TYPE_UDP_FECR | SOFTSYNC_TCP_TO_UDP))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
#endif

		CommandLineSwitchCase(arg, "config-file")
		{
			/* read before the other options: shadow_server_load_config() */
		}
		CommandLineSwitchCase(arg, "gfx-codec-order")
		{
			if (!shadow_server_parse_codec_order(server, arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "framerate")
		{
			/* H.264 follows it too */
			UINT32 rate = 0;
			if (!shadow_server_parse_u32(arg->Value, 1, 120, &rate))
				return fail_at(arg, COMMAND_LINE_ERROR);
			server->maxFrameRate = rate;
			server->h264FrameRate = rate;
		}
		CommandLineSwitchCase(arg, "gfx-progressive")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
#if defined(WITH_GFX_AV1)
		CommandLineSwitchCase(arg, "gfx-av1")
		{
			BOOL parseFailed = FALSE;
			BOOL enabled = TRUE;
			UINT32 profile = freerdp_settings_get_uint32(settings, FreeRDP_GfxCodecAV1Profile);
			if (arg->Value)
			{
				size_t count = 0;
				char** args = CommandLineParseCommaSeparatedValues(arg->Value, &count);
				WINPR_ASSERT(args || (count == 0));
				for (size_t x = 0; x < count; x++)
				{
					const char* cur = args[x];
					if (strncmp("profile:", cur, 8) == 0)
					{
						const char* val = &cur[8];
						if (strcmp("high", val) == 0)
							profile = 1;
						else if (strcmp("low", val) == 0)
							profile = 0;
						else
						{
							const unsigned long v = strtoul(val, nullptr, 0);
							if ((errno != 0) || (v >= UINT32_MAX))
							{
								parseFailed = TRUE;
								break;
							}
							else
								profile = WINPR_ASSERTING_INT_CAST(UINT32, v);
						}
					}
					else if (strcmp("off", cur) == 0)
					{
						enabled = FALSE;
					}
					else
					{
						parseFailed = TRUE;
						break;
					}
				}
				CommandLineParserFree(args);
			}
			if (parseFailed)
				return fail_at(arg, COMMAND_LINE_ERROR);
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxCodecAV1, enabled))
				return fail_at(arg, COMMAND_LINE_ERROR);
			if (!freerdp_settings_set_uint32(settings, FreeRDP_GfxCodecAV1Profile, profile))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
#endif
#if defined(WITH_GFX_AV2)
		CommandLineSwitchCase(arg, "gfx-av2")
		{
			BOOL parseFailed = FALSE;
			BOOL enabled = TRUE;
			UINT32 profile = freerdp_settings_get_uint32(settings, FreeRDP_GfxCodecAV2Profile);
			if (arg->Value)
			{
				size_t count = 0;
				char** args = CommandLineParseCommaSeparatedValues(arg->Value, &count);
				WINPR_ASSERT(args || (count == 0));
				for (size_t x = 0; x < count; x++)
				{
					const char* cur = args[x];
					if (strncmp("profile:", cur, 8) == 0)
					{
						const char* val = &cur[8];
						if (strcmp("high", val) == 0)
							profile = 1;
						else if (strcmp("low", val) == 0)
							profile = 0;
						else
						{
							const unsigned long v = strtoul(val, nullptr, 0);
							if ((errno != 0) || (v >= UINT32_MAX))
							{
								parseFailed = TRUE;
								break;
							}
							else
								profile = WINPR_ASSERTING_INT_CAST(UINT32, v);
						}
					}
					else if (strcmp("off", cur) == 0)
					{
						enabled = FALSE;
					}
					else
					{
						parseFailed = TRUE;
						break;
					}
				}
				CommandLineParserFree(args);
			}
			if (parseFailed)
				return fail_at(arg, COMMAND_LINE_ERROR);
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxCodecAV2, enabled))
				return fail_at(arg, COMMAND_LINE_ERROR);
			if (!freerdp_settings_set_uint32(settings, FreeRDP_GfxCodecAV2Profile, profile))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
#endif
		CommandLineSwitchCase(arg, "gfx-rfx")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "gfx-planar")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxPlanar, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "gfx-avc420")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxH264, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "gfx-avc444")
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, arg->Value != nullptr))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
#if defined(WITH_GFX_AZURE)
		CommandLineSwitchCase(arg, "gfx-hevc")
		{
			BOOL parseFailed = FALSE;
			BOOL enabled = TRUE;
			BOOL enabled444 = TRUE;

			if (arg->Value)
			{
				size_t count = 0;
				char** args = CommandLineParseCommaSeparatedValues(arg->Value, &count);
				WINPR_ASSERT(args || (count == 0));
				for (size_t x = 0; x < count; x++)
				{
					const char* cur = args[x];
					if (_stricmp("on", cur) == 0)
					{
						enabled = TRUE;
						enabled444 = TRUE;
					}
					else if (_stricmp("i444", cur) == 0)
					{
						enabled = TRUE;
						enabled444 = TRUE;
					}
					else if (_stricmp("i420", cur) == 0)
					{
						enabled = TRUE;
						enabled444 = FALSE;
					}
					else if (_stricmp("off", cur) == 0)
					{
						enabled = FALSE;
						enabled444 = FALSE;
					}
					else
					{
						parseFailed = TRUE;
						break;
					}
				}
				CommandLineParserFree(args);
			}
			if (parseFailed)
				return fail_at(arg, COMMAND_LINE_ERROR);
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxCodecHEVC, enabled))
				return fail_at(arg, COMMAND_LINE_ERROR);
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxCodecHEVC444, enabled444))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
#endif
		CommandLineSwitchCase(arg, "keytab")
		{
			if (!freerdp_settings_set_string(settings, FreeRDP_KerberosKeytab, arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "ccache")
		{
			if (!freerdp_settings_set_string(settings, FreeRDP_KerberosCache, arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchCase(arg, "tls-secrets-file")
		{
			if (!freerdp_settings_set_string(settings, FreeRDP_TlsSecretsFile, arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		CommandLineSwitchDefault(arg)
		{
		}
		CommandLineSwitchEnd(arg)
	} while ((arg = CommandLineFindNextArgumentA(arg)) != nullptr);

	/* Before /monitors: the subsystem decides which monitors there are */
	arg = CommandLineFindArgumentA(cargs, "subsystem");

	if (arg && (arg->Flags & COMMAND_LINE_ARGUMENT_PRESENT))
	{
		if (arg->Flags & COMMAND_LINE_VALUE_PRESENT)
		{
			if (!shadow_subsystem_set_order(arg->Value))
				return fail_at(arg, COMMAND_LINE_ERROR);
		}
		else
		{
			shadow_subsystem_list();
			status = COMMAND_LINE_STATUS_PRINT;
		}
	}

	arg = CommandLineFindArgumentA(cargs, "monitors");

	if (arg && (arg->Flags & COMMAND_LINE_ARGUMENT_PRESENT))
	{
		UINT32 numMonitors = 0;
		MONITOR_DEF monitors[16] = WINPR_C_ARRAY_INIT;
		numMonitors = shadow_enum_monitors(monitors, 16);

		if (arg->Flags & COMMAND_LINE_VALUE_PRESENT)
		{
			/* Select monitors. errno may hold anything from what ran before
			 * (enumerating the monitors makes system calls of its own) */
			errno = 0;
			long val = strtol(arg->Value, nullptr, 0);

			if ((val < 0) || (errno != 0) || ((UINT32)val >= numMonitors))
			{
				WLog_ERR(TAG, "no monitor '%s': there are %" PRIu32 " (/monitors lists them)",
				         arg->Value, numMonitors);
				status = COMMAND_LINE_STATUS_PRINT;
			}

			server->selectedMonitor = (UINT32)val;
		}
		else
		{
			/* List monitors */

			for (UINT32 index = 0; index < numMonitors; index++)
			{
				const MONITOR_DEF* monitor = &monitors[index];
				const INT64 width = monitor->right - monitor->left + 1;
				const INT64 height = monitor->bottom - monitor->top + 1;
				WLog_INFO(
				    TAG, "      %s [%" PRIu32 "] %" PRId64 "x%" PRId64 "\t+%" PRId32 "+%" PRId32 "",
				    (monitor->flags == 1) ? "*" : " ", index, width, height, monitor->left,
				    monitor->top);
			}

			status = COMMAND_LINE_STATUS_PRINT;
		}
	}

	/* If we want to disable authentication we need to ensure that NLA security
	 * is not activated. Only TLS and RDP security allow anonymous login.
	 */
	if (!server->authentication && !freerdp_settings_get_bool(settings, FreeRDP_VmConnectMode))
	{
		if (!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE))
			return COMMAND_LINE_ERROR;
		if (!freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE))
			return COMMAND_LINE_ERROR;
	}
	return status;
}

WINPR_ATTR_NODISCARD
static DWORD WINAPI shadow_server_thread(LPVOID arg)
{
	rdpShadowServer* server = (rdpShadowServer*)arg;
	BOOL running = TRUE;
	DWORD status = 0;
	freerdp_listener* listener = server->listener;
	if (shadow_subsystem_start(server->subsystem) < 0)
		running = FALSE;

	while (running)
	{
		HANDLE events[MAXIMUM_WAIT_OBJECTS] = WINPR_C_ARRAY_INIT;
		DWORD nCount = 0;
		events[nCount++] = server->StopEvent;
		nCount += listener->GetEventHandles(listener, &events[nCount], ARRAYSIZE(events) - nCount);

		if (nCount <= 1)
		{
			WLog_ERR(TAG, "Failed to get FreeRDP file descriptor");
			break;
		}

		status = WaitForMultipleObjects(nCount, events, FALSE, INFINITE);

		switch (status)
		{
			case WAIT_FAILED:
			case WAIT_OBJECT_0:
				running = FALSE;
				break;

			default:
			{
				if (!listener->CheckFileDescriptor(listener))
				{
					WLog_ERR(TAG, "Failed to check FreeRDP file descriptor");
					running = FALSE;
				}
				else
				{
#ifdef _WIN32
					Sleep(100); /* FIXME: listener event handles */
#endif
				}
			}
			break;
		}
	}

	listener->Close(listener);
	shadow_subsystem_stop(server->subsystem);

	/* Signal to the clients that server is being stopped and wait for them
	 * to disconnect. */
	if (shadow_client_boardcast_quit(server, 0))
	{
		while (ArrayList_Count(server->clients) > 0)
		{
			Sleep(100);
		}
	}

	ExitThread(0);
	return 0;
}

WINPR_ATTR_NODISCARD
static BOOL open_port(rdpShadowServer* server, char* address)
{
	BOOL status = 0;
	char* modaddr = address;

	if (modaddr)
	{
		if (modaddr[0] == '[')
		{
			char* end = strchr(address, ']');
			if (!end)
			{
				WLog_ERR(TAG, "Could not parse bind-address %s", address);
				return -1;
			}
			*end++ = '\0';
			if (strlen(end) > 0)
			{
				WLog_ERR(TAG, "Excess data after IPv6 address: '%s'", end);
				return -1;
			}
			modaddr++;
		}
	}
	status = server->listener->Open(server->listener, modaddr, (UINT16)server->port);

	if (!status)
	{
		WLog_ERR(TAG,
		         "Problem creating TCP listener. (Port already used or insufficient permissions?)");
	}

	return status;
}

int shadow_server_start(rdpShadowServer* server)
{
	BOOL ipc = 0;
	BOOL status = 0;
	WSADATA wsaData;

	if (!server)
		return -1;

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return -1;

#ifndef _WIN32
	(void)signal(SIGPIPE, SIG_IGN);
#endif
	server->screen = shadow_screen_new(server);

	if (!server->screen)
	{
		WLog_ERR(TAG, "screen_new failed");
		return -1;
	}

	server->capture = shadow_capture_new(server);

	if (!server->capture)
	{
		WLog_ERR(TAG, "capture_new failed");
		return -1;
	}

	/* Bind magic:
	 *
	 * empty                 ... bind TCP all
	 * <local path>          ... bind local (IPC)
	 * bind-socket,<address> ... bind TCP to specified interface
	 */
	ipc = server->ipcSocket && (strncmp(bind_address, server->ipcSocket,
	                                    strnlen(bind_address, sizeof(bind_address))) != 0);
	if (!ipc)
	{
		size_t count = 0;

		char** ptr = CommandLineParseCommaSeparatedValuesEx(nullptr, server->ipcSocket, &count);
		if (!ptr || (count <= 1))
		{
			if (server->ipcSocket == nullptr)
			{
				if (!open_port(server, nullptr))
				{
					CommandLineParserFree(ptr);
					return -1;
				}
			}
			else
			{
				CommandLineParserFree(ptr);
				return -1;
			}
		}

		WINPR_ASSERT(ptr || (count == 0));
		for (size_t x = 1; x < count; x++)
		{
			BOOL success = open_port(server, ptr[x]);
			if (!success)
			{
				CommandLineParserFree(ptr);
				return -1;
			}
		}
		CommandLineParserFree(ptr);
	}
	else
	{
		status = server->listener->OpenLocal(server->listener, server->ipcSocket);

		if (!status)
		{
			WLog_ERR(TAG, "Problem creating local socket listener. (Port already used or "
			              "insufficient permissions?)");
			return -1;
		}
	}

	if (!(server->thread =
	          CreateThread(nullptr, 0, shadow_server_thread, (void*)server, 0, nullptr)))
	{
		return -1;
	}

	return 0;
}

int shadow_server_stop(rdpShadowServer* server)
{
	if (!server)
		return -1;

	if (server->thread)
	{
		(void)SetEvent(server->StopEvent);
		(void)WaitForSingleObject(server->thread, INFINITE);
		(void)CloseHandle(server->thread);
		server->thread = nullptr;
		if (server->listener && server->listener->Close)
			server->listener->Close(server->listener);
	}

	if (server->screen)
	{
		shadow_screen_free(server->screen);
		server->screen = nullptr;
	}

	if (server->capture)
	{
		shadow_capture_free(server->capture);
		server->capture = nullptr;
	}

	return 0;
}

WINPR_ATTR_NODISCARD
static int shadow_server_init_config_path(rdpShadowServer* server)
{
	if (!server->ConfigPath)
	{
		char* configHome = freerdp_settings_get_config_path();

		if (configHome)
		{
			if (!winpr_PathFileExists(configHome) && !winpr_PathMakePath(configHome, nullptr))
			{
				WLog_ERR(TAG, "Failed to create directory '%s'", configHome);
				free(configHome);
				return -1;
			}

			server->ConfigPath = configHome;
		}
	}

	if (!server->ConfigPath)
		return -1; /* no usable config path */

	return 1;
}

#if defined(WITH_WINPR_TOOLS)
WINPR_ATTR_NODISCARD
static BOOL shadow_server_create_certificate(rdpShadowServer* server, const char* filepath)
{
	BOOL rc = FALSE;
	char* makecert_argv[6] = { "makecert", "-rdp", "-live", "-silent", "-y", "5" };

	WINPR_STATIC_ASSERT(ARRAYSIZE(makecert_argv) <= INT_MAX);
	const size_t makecert_argc = ARRAYSIZE(makecert_argv);

	MAKECERT_CONTEXT* makecert = makecert_context_new();

	if (!makecert)
		goto out_fail;

	if (makecert_context_process(makecert, (int)makecert_argc, makecert_argv) < 0)
		goto out_fail;

	if (makecert_context_set_output_file_name(makecert, "shadow") != 1)
		goto out_fail;

	WINPR_ASSERT(server);
	WINPR_ASSERT(filepath);
	if (!winpr_PathFileExists(server->CertificateFile))
	{
		if (makecert_context_output_certificate_file(makecert, filepath) != 1)
			goto out_fail;
	}

	if (!winpr_PathFileExists(server->PrivateKeyFile))
	{
		if (makecert_context_output_private_key_file(makecert, filepath) != 1)
			goto out_fail;
	}
	rc = TRUE;
out_fail:
	if (!rc)
		WLog_ERR(TAG, "cannot make the certificate and its key in %s", filepath);
	makecert_context_free(makecert);
	return rc;
}
#endif

WINPR_ATTR_NODISCARD
static BOOL shadow_server_read_pem(rdpShadowServer* server)
{
	BOOL rc = FALSE;
	WINPR_ASSERT(server);

	rdpSettings* settings = server->settings;
	WINPR_ASSERT(settings);

	{
		rdpPrivateKey* key = freerdp_key_new_from_file_enc(server->PrivateKeyFile, nullptr);
		if (!key)
		{
			WLog_ERR(TAG, "Private Key PEM %s invalid, aborting", server->PrivateKeyFile);
			return FALSE;
		}

		if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1))
		{
			WLog_ERR(TAG, "Private Key PEM %s valid, but some properties failed to be extracted",
			         server->PrivateKeyFile);
			return FALSE;
		}
	}
	{
		rdpCertificate* cert = freerdp_certificate_new_from_file(server->CertificateFile);
		if (!cert)
		{
			WLog_ERR(TAG, "Certificate PEM %s invalid, aborting", server->CertificateFile);
			return FALSE;
		}

		if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1))
			goto out_fail;

		if (!freerdp_certificate_is_rdp_security_compatible(cert))
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE))
				goto out_fail;
			if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE))
				goto out_fail;
		}
	}
	rc = TRUE;

out_fail:
	WLog_ERR(TAG, "Certificate PEM %s valid, but some properties failed to be extracted",
	         server->CertificateFile);
	return rc;
}

WINPR_ATTR_NODISCARD
static BOOL shadow_server_init_certificate(rdpShadowServer* server)
{
	char* filepath = nullptr;
	BOOL ret = FALSE;

	WINPR_ASSERT(server);

	if (!winpr_PathFileExists(server->ConfigPath) &&
	    !winpr_PathMakePath(server->ConfigPath, nullptr))
	{
		WLog_ERR(TAG, "Failed to create directory '%s'", server->ConfigPath);
		return FALSE;
	}

	if (!(filepath = GetCombinedPath(server->ConfigPath, "shadow")))
		return FALSE;

	if (!winpr_PathFileExists(filepath) && !winpr_PathMakePath(filepath, nullptr))
	{
		if (!winpr_PathMakePath(filepath, nullptr))
		{
			WLog_ERR(TAG, "Failed to create directory '%s'", filepath);
			goto out_fail;
		}
	}

	server->CertificateFile = GetCombinedPath(filepath, "shadow.crt");
	server->PrivateKeyFile = GetCombinedPath(filepath, "shadow.key");

	if (!server->CertificateFile || !server->PrivateKeyFile)
		goto out_fail;

	if ((!winpr_PathFileExists(server->CertificateFile)) ||
	    (!winpr_PathFileExists(server->PrivateKeyFile)))
	{
#if defined(WITH_WINPR_TOOLS)
		if (!shadow_server_create_certificate(server, filepath))
#else
		const char* cexist = winpr_PathFileExists(server->CertificateFile) ? "exists" : "not found";
		const char* pexist = winpr_PathFileExists(server->PrivateKeyFile) ? "exists" : "not found";
		WLog_ERR(TAG, "PEM files do not exist: cert: %s (%s), key: %s (%s)",
		         server->CertificateFile, cexist, server->PrivateKeyFile, pexist);
#endif
			goto out_fail;
	}

	ret = shadow_server_read_pem(server);

out_fail:
	free(filepath);
	return ret;
}

WINPR_ATTR_NODISCARD
static BOOL shadow_server_check_peer_restrictions(freerdp_listener* listener)
{
	WINPR_ASSERT(listener);

	rdpShadowServer* server = (rdpShadowServer*)listener->info;
	WINPR_ASSERT(server);

	if (server->maxClientsConnected > 0)
	{
		const size_t count = ArrayList_Count(server->clients);
		if (count >= server->maxClientsConnected)
		{
			WLog_WARN(TAG, "connection limit [%" PRIuz "] reached, discarding client",
			          server->maxClientsConnected);
			return FALSE;
		}
	}
	return TRUE;
}

int shadow_server_init(rdpShadowServer* server)
{
	int status = 0;
	if (!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
		return -1;
	if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi()))
		return -1;

	if (!(server->clients = ArrayList_New(TRUE)))
		goto fail;

	if (!(server->StopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr)))
		goto fail;

	if (!InitializeCriticalSectionAndSpinCount(&(server->lock), 4000))
		goto fail;

	status = shadow_server_init_config_path(server);

	if (status < 0)
		goto fail;

	if (!shadow_server_init_certificate(server))
		goto fail;

	server->listener = freerdp_listener_new();

	if (!server->listener)
		goto fail;

	server->listener->info = (void*)server;
	server->listener->CheckPeerAcceptRestrictions = shadow_server_check_peer_restrictions;
	server->listener->PeerAccepted = shadow_client_accepted;
	server->clipboard = shadow_clipboard_new(server);

	server->subsystem = shadow_subsystem_new();

	if (!server->subsystem)
		goto fail;

	status = shadow_subsystem_init(server->subsystem, server);
	if (status < 0)
		goto fail;

	return status;

fail:
	shadow_server_uninit(server);
	WLog_ERR(TAG, "Failed to initialize shadow server");
	return -1;
}

int shadow_server_uninit(rdpShadowServer* server)
{
	if (!server)
		return -1;

	shadow_server_stop(server);
	shadow_subsystem_uninit(server->subsystem);
	shadow_subsystem_free(server->subsystem);
	server->subsystem = nullptr;
	shadow_clipboard_free(server->clipboard);
	server->clipboard = nullptr;
	freerdp_listener_free(server->listener);
	server->listener = nullptr;
	free(server->CertificateFile);
	server->CertificateFile = nullptr;
	free(server->PrivateKeyFile);
	server->PrivateKeyFile = nullptr;
	free(server->ConfigPath);
	server->ConfigPath = nullptr;
	DeleteCriticalSection(&(server->lock));
	(void)CloseHandle(server->StopEvent);
	server->StopEvent = nullptr;
	ArrayList_Free(server->clients);
	server->clients = nullptr;
	return 1;
}

/* A setting from the environment, or its default when unset or out of range */
WINPR_ATTR_NODISCARD
static UINT32 shadow_server_env_u32(const char* name, UINT32 value, UINT32 min, UINT32 max)
{
	const char* text = getenv(name);
	if (!text || (*text == '\0'))
		return value;

	char* end = nullptr;
	errno = 0;
	const unsigned long long parsed = strtoull(text, &end, 10);
	if ((errno != 0) || !end || (*end != '\0') || (parsed < min) || (parsed > max))
	{
		WLog_WARN(TAG,
		          "%s=%s%s is not a number from %" PRIu32 " to %" PRIu32 ": %" PRIu32 " instead",
		          name, text, shadow_config_origin(name), min, max, value);
		return value;
	}
	return (UINT32)parsed;
}

/* vbr or cqp: the H.264 encoders have no other mode (H264_RATECONTROL_MODE) */
WINPR_ATTR_NODISCARD
static H264_RATECONTROL_MODE shadow_server_env_h264_rate_control(const char* name,
                                                                 H264_RATECONTROL_MODE mode)
{
	const char* text = getenv(name);
	if (!text || (*text == '\0'))
		return mode;

	if (_stricmp(text, "vbr") == 0)
		return H264_RATECONTROL_VBR;
	if (_stricmp(text, "cqp") == 0)
		return H264_RATECONTROL_CQP;

	WLog_WARN(TAG, "%s=%s%s is not vbr or cqp (H.264 has no other mode): %s instead", name, text,
	          shadow_config_origin(name), (mode == H264_RATECONTROL_CQP) ? "cqp" : "vbr");
	return mode;
}

#if defined(WITH_GFX_AV1) || defined(WITH_GFX_AV2)
/* q, cq, cbr or vbr: FREERDP_AV1_RATECONTROL and FREERDP_AV2_RATECONTROL have the same order
 * (VBR, CBR, CQ, Q). Unset: Q without a target bit rate, CQ with one: the quality as long as
 * the picture is cheap, the target as a ceiling when it is busy. Any mode but Q needs a target. */
WINPR_ATTR_NODISCARD
static UINT32 shadow_server_env_rate_control(const char* name, UINT32 target)
{
	const UINT32 vbr = 0;
	const UINT32 cbr = 1;
	const UINT32 cq = 2;
	const UINT32 q = 3;

	UINT32 mode = (target > 0) ? cq : q;
	const char* text = getenv(name);
	if (text && (*text != '\0'))
	{
		if (_stricmp(text, "q") == 0)
			mode = q;
		else if (_stricmp(text, "cq") == 0)
			mode = cq;
		else if (_stricmp(text, "cbr") == 0)
			mode = cbr;
		else if (_stricmp(text, "vbr") == 0)
			mode = vbr;
		else
			WLog_WARN(TAG, "%s=%s%s is not q, cq, cbr or vbr: %s instead", name, text,
			          shadow_config_origin(name), (mode == q) ? "q" : "cq");
	}
	if ((mode != q) && (target == 0))
	{
		WLog_WARN(TAG, "%s%s: %s needs a target bit rate: q instead", name,
		          shadow_config_origin(name),
		          (mode == cq)    ? "cq"
		          : (mode == cbr) ? "cbr"
		                          : "vbr");
		mode = q;
	}
	return mode;
}
#endif

#if defined(WITH_GFX_AV2)
WINPR_ATTR_NODISCARD
static UINT32 shadow_server_cpus(void)
{
	SYSTEM_INFO sysInfo = WINPR_C_ARRAY_INIT;
	GetNativeSystemInfo(&sysInfo);
	return (sysInfo.dwNumberOfProcessors > 0) ? sysInfo.dwNumberOfProcessors : 1;
}
#endif

/* The encoders' settings, all in one place: the best quality per bit measured on screen content
 * (1600x900 4:4:4, a terminal still and scrolling, video in a window and full screen).
 * Constrained quality: as sharp as the quality setting while the picture is cheap, the target
 * bit rate as a ceiling when it is busy: quality most, limited by the bandwidth (AV1 12 Mbit/s,
 * AV2 10 Mbit/s). Text keeps its quality at any ceiling from 4 Mbit/s up (AV1 47 dB, about 230
 * kbit/s still and 720 scrolling); the ceiling decides video. A busy second went 4 to 6 % over it.
 * Each has an environment variable, read when the server is made: FREERDP_SHADOW_AV1_* and
 * FREERDP_SHADOW_AV2_* RATE_CONTROL (q, cq, cbr, vbr), TARGET_BITRATE (0: q), CQ_LEVEL or QP,
 * THREADS, CPU_USED, and for AV2 TILE_COLUMNS and DPB_SIZE.
 * H.264 (AVC420 and AVC444): FREERDP_SHADOW_H264_RATE_CONTROL (vbr, cqp), BITRATE (kbit/s, for
 * vbr) and QP (0 to 51, for cqp). */
static void shadow_server_encoder_defaults(rdpShadowServer* server)
{
	WINPR_ASSERT(server);
	/* VBR at 10 Mbit/s and 30 fps */
	server->h264RateControlMode = shadow_server_env_h264_rate_control(
	    "FREERDP_SHADOW_H264_RATE_CONTROL", H264_RATECONTROL_VBR);
	server->h264BitRate =
	    1000u * shadow_server_env_u32("FREERDP_SHADOW_H264_BITRATE", 10000, 1, 1000000);
	server->h264FrameRate = 30;
	server->h264QP = shadow_server_env_u32("FREERDP_SHADOW_H264_QP", 0, 0, 51);
#if defined(WITH_GFX_AV1)
	/* libaom: cq-level 16 under 12 Mbit/s, cpu-used 10, rows on up to 4 threads (0) */
	server->AV1BitRate =
	    shadow_server_env_u32("FREERDP_SHADOW_AV1_TARGET_BITRATE", 12000, 0, 1000000);
	server->AV1RateControlMode = (FREERDP_AV1_RATECONTROL)shadow_server_env_rate_control(
	    "FREERDP_SHADOW_AV1_RATE_CONTROL", server->AV1BitRate);
	server->AV1CQLevel = shadow_server_env_u32("FREERDP_SHADOW_AV1_CQ_LEVEL", 16, 1, 63);
	server->AV1Threads = shadow_server_env_u32("FREERDP_SHADOW_AV1_THREADS", 0, 0, 64);
	server->AV1Speed = shadow_server_env_u32("FREERDP_SHADOW_AV1_CPU_USED", 10, 0, 12);
#endif
#if defined(WITH_GFX_AV2)
	/* libavm: qp 100 under 10 Mbit/s, cpu-used 10, 4 threads on 2 tile columns, 4 reference
	 * frames (libavm keeps 8, each a whole frame in 16 bits: 4 saves about 100 MB at 1080p in
	 * 4:4:4); qp 160 left marks on scrolling text, 100 is 46 dB on it at 21 to 28 fps */
	server->AV2BitRate =
	    shadow_server_env_u32("FREERDP_SHADOW_AV2_TARGET_BITRATE", 10000, 0, 1000000);
	server->AV2RateControlMode = (FREERDP_AV2_RATECONTROL)shadow_server_env_rate_control(
	    "FREERDP_SHADOW_AV2_RATE_CONTROL", server->AV2BitRate);
	server->AV2QP = shadow_server_env_u32("FREERDP_SHADOW_AV2_QP", 100, 1, 255);
	server->AV2Threads =
	    shadow_server_env_u32("FREERDP_SHADOW_AV2_THREADS", MIN(4, shadow_server_cpus()), 1, 64);
	server->AV2Speed = shadow_server_env_u32("FREERDP_SHADOW_AV2_CPU_USED", 10, 0, 10);
	server->AV2TileColumns = shadow_server_env_u32("FREERDP_SHADOW_AV2_TILE_COLUMNS", 1, 0, 6);
	server->AV2DpbSize = shadow_server_env_u32("FREERDP_SHADOW_AV2_DPB_SIZE", 4, 1, 16);
#endif
}

/* The settings with an environment variable (and so a line in the configuration file) */
static void shadow_server_env_defaults(rdpShadowServer* server)
{
	WINPR_ASSERT(server);
	shadow_server_encoder_defaults(server);
	shadow_server_set_codec_order(server, nullptr, 0);

	/* /framerate's value, for a server started with no command line of its own (a session
	 * manager's); a wrong one is logged and the rates stay each their own */
	server->maxFrameRate = shadow_server_env_u32("FREERDP_SHADOW_FRAMERATE", 0, 1, 120);
	if (server->maxFrameRate > 0)
		server->h264FrameRate = server->maxFrameRate;

	const char* name = "FREERDP_SHADOW_GFX_CODEC_ORDER";
	const char* order = getenv(name);
	if (order && (*order != '\0') && !shadow_server_parse_codec_order(server, order))
		WLog_WARN(TAG, "%s=%s%s is not a codec order: the default one instead", name, order,
		          shadow_config_origin(name));
}

rdpShadowServer* shadow_server_new(void)
{
	rdpShadowServer* server = nullptr;
	server = (rdpShadowServer*)calloc(1, sizeof(rdpShadowServer));

	if (!server)
		return nullptr;

	server->SupportMultiRectBitmapUpdates = TRUE;
	server->port = 3389;
	server->mayView = TRUE;
	server->mayInteract = TRUE;
	server->h264HwAccel = FALSE;
	server->authentication = TRUE;
	shadow_server_env_defaults(server);
#if defined(WITH_GFX_AZURE)
	server->HEVCBitRate = 500000;
	server->HEVCRateControlMode = FREERDP_HEVC_VBR;
#endif
	server->settings = freerdp_settings_new(FREERDP_SETTINGS_SERVER_MODE);
	/* The graphics pipeline (and with it AV1, AV2, H.264, progressive) unless -gfx: libfreerdp's
	 * server settings leave it off by default, and a session manager starts the shadow server
	 * without /gfx, so its clients got bitmaps only */
	if (!server->settings ||
	    !freerdp_settings_set_bool(server->settings, FreeRDP_SupportGraphicsPipeline, TRUE))
	{
		freerdp_settings_free(server->settings);
		free(server);
		return nullptr;
	}
	return server;
}

void shadow_server_free(rdpShadowServer* server)
{
	if (!server)
		return;

	free(server->ipcSocket);
	server->ipcSocket = nullptr;
	freerdp_settings_free(server->settings);
	server->settings = nullptr;
	free(server);
}
