/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Runtime statistics of a client session
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

#include <winpr/assert.h>
#include <winpr/crt.h>
#include <winpr/sysinfo.h>

#include <freerdp/autodetect.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client.h>
#include <freerdp/constants.h>
#include <freerdp/freerdp.h>

#if defined(CHANNEL_RDPGFX_CLIENT)
#include <freerdp/client/rdpgfx.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#endif

#include "client_stats.h"

/* Nothing is counted here: this reads what the libraries count anyway, so no path that carries
 * data gets slower. */
enum
{
	STATS_BYTES_RECEIVED,
	STATS_BYTES_SENT,
	STATS_PACKETS_RECEIVED,
	STATS_PACKETS_SENT,
	STATS_BANDWIDTH_KBPS,
	STATS_RTT_BASE_MS,
	STATS_RTT_AVERAGE_MS,
	STATS_AUDIO_WAVES,
	STATS_AUDIO_WAVE_BYTES,
	STATS_AUDIO_WAVES_OVERRUN,
	STATS_AUDIO_WAVES_UNDECODABLE,
	STATS_AUDIO_DEVICE_OPENS,
	STATS_AUDIO_FORMAT_TAG,
	STATS_AUDIO_DELAY_MS,
	STATS_RDP_VERSION,
	STATS_SERVER_RDP_VERSION,
	STATS_SERVER_OS_MAJOR_TYPE,
	STATS_SERVER_OS_MINOR_TYPE,
	STATS_SERVER_PORT,
	STATS_GFX_CAPS_VERSION,
	STATS_GFX_CAPS_FLAGS,
	STATS_GFX_SURFACES,
	STATS_GFX_SURFACE1_CODECID, /* then _WIDTH, _HEIGHT, and so for each slot */
	STATS_GFX_SURFACE_SLOTS_END = STATS_GFX_SURFACE1_CODECID + 4 * 3,
	STATS_SESSION_MAX = STATS_GFX_SURFACE_SLOTS_END
};

#define STATS_GFX_SURFACE_SLOTS 4

static const char* const sessionNames[] = { "NETWORK_BYTES_RECEIVED",
	                                        "NETWORK_BYTES_SENT",
	                                        "NETWORK_PACKETS_RECEIVED",
	                                        "NETWORK_PACKETS_SENT",
	                                        "NETWORK_BANDWIDTH_KBPS",
	                                        "NETWORK_RTT_BASE_MS",
	                                        "NETWORK_RTT_AVERAGE_MS",
	                                        "AUDIO_WAVES",
	                                        "AUDIO_WAVE_BYTES",
	                                        "AUDIO_WAVES_OVERRUN",
	                                        "AUDIO_WAVES_UNDECODABLE",
	                                        "AUDIO_DEVICE_OPENS",
	                                        "AUDIO_FORMAT_TAG",
	                                        "AUDIO_DELAY_MS",
	                                        "RDP_VERSION",
	                                        "SERVER_RDP_VERSION",
	                                        "SERVER_OS_MAJOR_TYPE",
	                                        "SERVER_OS_MINOR_TYPE",
	                                        "SERVER_PORT",
	                                        "GFX_CAPS_VERSION",
	                                        "GFX_CAPS_FLAGS",
	                                        "RDPGFX_SURFACES",
	                                        "RDPGFX_SURFACE1_CODECID",
	                                        "RDPGFX_SURFACE1_WIDTH",
	                                        "RDPGFX_SURFACE1_HEIGHT",
	                                        "RDPGFX_SURFACE2_CODECID",
	                                        "RDPGFX_SURFACE2_WIDTH",
	                                        "RDPGFX_SURFACE2_HEIGHT",
	                                        "RDPGFX_SURFACE3_CODECID",
	                                        "RDPGFX_SURFACE3_WIDTH",
	                                        "RDPGFX_SURFACE3_HEIGHT",
	                                        "RDPGFX_SURFACE4_CODECID",
	                                        "RDPGFX_SURFACE4_WIDTH",
	                                        "RDPGFX_SURFACE4_HEIGHT" };

enum
{
	STRINGS_SERVER_HOSTNAME,
	STRINGS_SERVER_CERT_CN,
	STRINGS_MAX
};

static const char* const stringNames[] = { "SERVER_HOSTNAME", "SERVER_CERT_CN" };

static uint64_t network_value(const rdpContext* context, size_t index)
{
	if (index <= STATS_PACKETS_SENT)
	{
		UINT64 values[4] = WINPR_C_ARRAY_INIT;
		if (!freerdp_get_stats(context->rdp, &values[STATS_BYTES_RECEIVED],
		                       &values[STATS_BYTES_SENT], &values[STATS_PACKETS_RECEIVED],
		                       &values[STATS_PACKETS_SENT]))
			return 0;
		return values[index];
	}

	const rdpAutoDetect* autodetect = context->autodetect;
	if (!autodetect)
		return 0;

	switch (index)
	{
		case STATS_BANDWIDTH_KBPS:
			return autodetect->netCharBandwidth;
		case STATS_RTT_BASE_MS:
			return autodetect->netCharBaseRTT;
		case STATS_RTT_AVERAGE_MS:
			return autodetect->netCharAverageRTT;
		default:
			return 0;
	}
}

static uint64_t audio_value(const rdpContext* context, size_t index)
{
	const rdpMetrics* metrics = context->metrics;
	if (!metrics)
		return 0;

	switch (index)
	{
		case STATS_AUDIO_WAVES:
			return metrics->AudioWaves;
		case STATS_AUDIO_WAVE_BYTES:
			return metrics->AudioWaveBytes;
		case STATS_AUDIO_WAVES_OVERRUN:
			return metrics->AudioWavesOverrun;
		case STATS_AUDIO_WAVES_UNDECODABLE:
			return metrics->AudioWavesUndecodable;
		case STATS_AUDIO_DEVICE_OPENS:
			return metrics->AudioDeviceOpens;
		case STATS_AUDIO_FORMAT_TAG:
			return metrics->AudioFormatTag;
		case STATS_AUDIO_DELAY_MS:
			return metrics->AudioDelayMs;
		default:
			return 0;
	}
}

/* what the server announced: 0 until it has (freerdp_get_remote_settings) */
static uint64_t server_value(const rdpContext* context, size_t index)
{
	const rdpSettings* remote = freerdp_get_remote_settings(context);
	if (!remote)
		return 0;
	const UINT32 serverVersion = freerdp_settings_get_uint32(remote, FreeRDP_RdpVersion);

	switch (index)
	{
		case STATS_RDP_VERSION:
			/* the local one is the client's own until the server's core data arrives */
			return serverVersion
			           ? freerdp_settings_get_uint32(context->settings, FreeRDP_RdpVersion)
			           : 0;
		case STATS_SERVER_RDP_VERSION:
			return serverVersion;
		case STATS_SERVER_OS_MAJOR_TYPE:
			return freerdp_settings_get_uint32(remote, FreeRDP_OsMajorType);
		case STATS_SERVER_OS_MINOR_TYPE:
			return freerdp_settings_get_uint32(remote, FreeRDP_OsMinorType);
		case STATS_SERVER_PORT:
			return serverVersion
			           ? freerdp_settings_get_uint32(context->settings, FreeRDP_ServerPort)
			           : 0;
		default:
			return 0;
	}
}

#if defined(CHANNEL_RDPGFX_CLIENT)
static int compare_surface_ids(const void* a, const void* b)
{
	const UINT16 x = *(const UINT16*)a;
	const UINT16 y = *(const UINT16*)b;
	return (x > y) - (x < y);
}

/* the surfaces by surface ID, the first STATS_GFX_SURFACE_SLOTS of them in slots */
static uint64_t surface_value(RdpgfxClientContext* gfx, size_t index)
{
	uint64_t value = 0;
	UINT16* ids = nullptr;
	UINT16 count = 0;

	EnterCriticalSection(&gfx->mux);
	if (!gfx->GetSurfaceIds || (gfx->GetSurfaceIds(gfx, &ids, &count) != CHANNEL_RC_OK))
		goto out;
	if (index == STATS_GFX_SURFACES)
	{
		value = count;
		goto out;
	}

	{
		const size_t slot = (index - STATS_GFX_SURFACE1_CODECID) / 3;
		const size_t field = (index - STATS_GFX_SURFACE1_CODECID) % 3;
		if (!ids || (slot >= count) || !gfx->GetSurfaceData)
			goto out;
		qsort(ids, count, sizeof(UINT16), compare_surface_ids);
		const gdiGfxSurface* surface = (const gdiGfxSurface*)gfx->GetSurfaceData(gfx, ids[slot]);
		if (!surface)
			goto out;
		switch (field)
		{
			case 0:
				value = surface->codecId;
				break;
			case 1:
				value = surface->mappedWidth;
				break;
			default:
				value = surface->mappedHeight;
				break;
		}
	}
out:
	LeaveCriticalSection(&gfx->mux);
	free(ids);
	return value;
}

static uint64_t gfx_value(const rdpContext* context, size_t index)
{
	if (!context->gdi)
		return 0;

	/* once: another thread can clear it at any time */
	RdpgfxClientContext* gfx = context->gdi->gfx;
	if (!gfx)
		return 0;
	switch (index)
	{
		case STATS_GFX_CAPS_VERSION:
			return gfx->ConfirmedCaps.version;
		case STATS_GFX_CAPS_FLAGS:
			return gfx->ConfirmedCaps.flags;
		default:
			if (index < STATS_SESSION_MAX)
				return surface_value(gfx, index);
			return rdpgfx_stats_value_for_index(gfx, index - STATS_SESSION_MAX);
	}
}
#endif

size_t freerdp_client_stats_max_index(void)
{
	/* in a function: without C11 this is an assert(), which a file scope does not take */
	WINPR_STATIC_ASSERT(ARRAYSIZE(sessionNames) == STATS_SESSION_MAX);

#if defined(CHANNEL_RDPGFX_CLIENT)
	return STATS_SESSION_MAX + rdpgfx_stats_max_index();
#else
	return STATS_SESSION_MAX;
#endif
}

const char* freerdp_client_stats_name_for_index(size_t index)
{
	if (index < STATS_SESSION_MAX)
		return sessionNames[index];

#if defined(CHANNEL_RDPGFX_CLIENT)
	if (index < freerdp_client_stats_max_index())
		return rdpgfx_stats_name_for_index(index - STATS_SESSION_MAX);
#endif
	return "FREERDP_CLIENT_STATS_UNUSED";
}

uint64_t freerdp_client_stats_value_for_index(const rdpContext* context, size_t index)
{
	if (!context)
		return 0;

	if (index <= STATS_RTT_AVERAGE_MS)
		return network_value(context, index);
	if (index <= STATS_AUDIO_DELAY_MS)
		return audio_value(context, index);
	if (index <= STATS_SERVER_PORT)
		return server_value(context, index);

#if defined(CHANNEL_RDPGFX_CLIENT)
	if (index < freerdp_client_stats_max_index())
		return gfx_value(context, index);
#endif
	return 0;
}

size_t freerdp_client_stats_string_max_index(void)
{
	WINPR_STATIC_ASSERT(ARRAYSIZE(stringNames) == STRINGS_MAX);
	return STRINGS_MAX;
}

const char* freerdp_client_stats_string_name_for_index(size_t index)
{
	if (index < STRINGS_MAX)
		return stringNames[index];
	return "FREERDP_CLIENT_STATS_UNUSED";
}

const char* freerdp_client_stats_string_value_for_index(const rdpContext* context, size_t index)
{
	if (!context)
		return nullptr;

	switch (index)
	{
		case STRINGS_SERVER_HOSTNAME:
			return freerdp_settings_get_string(context->settings, FreeRDP_ServerHostname);
		case STRINGS_SERVER_CERT_CN:
			return freerdp_settings_get_string(freerdp_get_remote_settings(context),
			                                   FreeRDP_CertificateName);
		default:
			return nullptr;
	}
}

const char* freerdp_client_stats_rdp_version_name(uint64_t version)
{
	switch (version)
	{
		case RDP_VERSION_4:
			return "4";
		case RDP_VERSION_5_PLUS:
			return "5+";
		case RDP_VERSION_10_0:
			return "10.0";
		case RDP_VERSION_10_1:
			return "10.1";
		case RDP_VERSION_10_2:
			return "10.2";
		case RDP_VERSION_10_3:
			return "10.3";
		case RDP_VERSION_10_4:
			return "10.4";
		case RDP_VERSION_10_5:
			return "10.5";
		case RDP_VERSION_10_6:
			return "10.6";
		case RDP_VERSION_10_7:
			return "10.7";
		case RDP_VERSION_10_8:
			return "10.8";
		case RDP_VERSION_10_9:
			return "10.9";
		case RDP_VERSION_10_10:
			return "10.10";
		case RDP_VERSION_10_11:
			return "10.11";
		case RDP_VERSION_10_12:
			return "10.12";
		default:
			return "unknown";
	}
}

static const char* os_major_name(uint64_t major)
{
	switch (major)
	{
		case OSMAJORTYPE_UNSPECIFIED:
			return "unspecified";
		case OSMAJORTYPE_WINDOWS:
			return "Windows";
		case OSMAJORTYPE_OS2:
			return "OS/2";
		case OSMAJORTYPE_MACINTOSH:
			return "Macintosh";
		case OSMAJORTYPE_UNIX:
			return "Unix";
		case OSMAJORTYPE_IOS:
			return "iOS";
		case OSMAJORTYPE_OSX:
			return "macOS";
		case OSMAJORTYPE_ANDROID:
			return "Android";
		case OSMAJORTYPE_CHROME_OS:
			return "ChromeOS";
		default:
			return "unknown";
	}
}

static const char* os_minor_name(uint64_t minor)
{
	switch (minor)
	{
		case OSMINORTYPE_UNSPECIFIED:
			return nullptr;
		case OSMINORTYPE_WINDOWS_31X:
			return "Windows 3.1x";
		case OSMINORTYPE_WINDOWS_95:
			return "Windows 95";
		case OSMINORTYPE_WINDOWS_NT:
			return "Windows NT";
		case OSMINORTYPE_OS2_V21:
			return "OS/2 2.1";
		case OSMINORTYPE_POWER_PC:
			return "PowerPC";
		case OSMINORTYPE_MACINTOSH:
			return "Macintosh";
		case OSMINORTYPE_NATIVE_XSERVER:
			return "native X server";
		case OSMINORTYPE_PSEUDO_XSERVER:
			return "pseudo X server";
		case OSMINORTYPE_WINDOWS_RT:
			return "Windows RT";
		case OSMINORTYPE_NATIVE_WAYLAND:
			return "native Wayland";
		case OSMINORTYPE_NATIVE_SDL:
			return "native SDL";
		default:
			return "unknown";
	}
}

const char* freerdp_client_stats_os_name(uint64_t major, uint64_t minor, char* buffer, size_t size)
{
	if (!buffer || (size == 0))
		return nullptr;
	const char* minorName = os_minor_name(minor);
	if (minorName)
		(void)_snprintf(buffer, size, "%s / %s", os_major_name(major), minorName);
	else
		(void)_snprintf(buffer, size, "%s", os_major_name(major));
	return buffer;
}

const char* freerdp_client_stats_caps_version_name(uint64_t version)
{
	switch (version)
	{
#if defined(WITH_GFX_AV1)
		case RDPGFX_CAPVERSION_FRDP_1:
			return "FRDP_1 (AV1)";
#endif
#if defined(WITH_GFX_AV2)
		case RDPGFX_CAPVERSION_FRDP_2:
			return "FRDP_2 (AV2)";
#endif
		case RDPGFX_CAPVERSION_8:
			return "8.0";
		case RDPGFX_CAPVERSION_81:
			return "8.1";
		case RDPGFX_CAPVERSION_10:
			return "10.0";
		case RDPGFX_CAPVERSION_101:
			return "10.1";
		case RDPGFX_CAPVERSION_102:
			return "10.2";
		case RDPGFX_CAPVERSION_103:
			return "10.3";
		case RDPGFX_CAPVERSION_104:
			return "10.4";
		case RDPGFX_CAPVERSION_105:
			return "10.5";
		case RDPGFX_CAPVERSION_106:
			return "10.6";
		case RDPGFX_CAPVERSION_106_ERR:
			return "10.6 (err)";
		case RDPGFX_CAPVERSION_107:
			return "10.7";
#if defined(WITH_GFX_AZURE)
		case RDPGFX_CAPVERSION_111:
			return "11.1";
		case RDPGFX_CAPVERSION_112:
			return "11.2";
		case RDPGFX_CAPVERSION_113:
			return "11.3";
		case RDPGFX_CAPVERSION_115:
			return "11.5";
#endif
		default:
			return "unknown";
	}
}

const char* freerdp_client_stats_caps_flags_name(uint64_t flags, char* buffer, size_t size)
{
	static const struct
	{
		UINT32 flag;
		const char* name;
	} names[] = {
		{ RDPGFX_CAPS_FLAG_THINCLIENT, "thin client" },
		{ RDPGFX_CAPS_FLAG_SMALL_CACHE, "small cache" },
		{ RDPGFX_CAPS_FLAG_AVC420_ENABLED, "AVC420" },
		{ RDPGFX_CAPS_FLAG_AVC_DISABLED, "AVC disabled" },
		{ RDPGFX_CAPS_FLAG_AVC_THINCLIENT, "AVC thin client" },
		{ RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE, "no scaled map" },
#if defined(WITH_GFX_AZURE)
		{ RDPGFX_CAPS_FLAG_SCP_DISABLE, "no SCP" },
		{ RDPGFX_CAPS_FLAG_HEVC_ENABLE, "HEVC" },
		{ RDPGFX_CAPS_FLAG_AVCMM_ENABLE, "AVC mixed" },
		{ RDPGFX_CAPS_FLAG_HEVC_I444, "HEVC 4:4:4" },
#endif
#if defined(WITH_GFX_AV1) || defined(WITH_GFX_AV2)
		{ RDPGFX_CAPS_FLAG_AV1_I444_SUPPORTED, "AV1 4:4:4" },
		{ RDPGFX_CAPS_FLAG_AV1_I444_DISABLED, "AV1 4:4:4 disabled" },
#endif
	};

	if (!buffer || (size == 0))
		return nullptr;
	buffer[0] = '\0';
	if (flags == 0)
	{
		(void)_snprintf(buffer, size, "none");
		return buffer;
	}

	size_t used = 0;
	uint64_t rest = flags;
	for (size_t x = 0; x < ARRAYSIZE(names); x++)
	{
		if ((flags & names[x].flag) == 0)
			continue;
		rest &= ~(uint64_t)names[x].flag;
		const int rc =
		    _snprintf(&buffer[used], size - used, "%s%s", used ? ", " : "", names[x].name);
		if ((rc < 0) || ((size_t)rc >= size - used))
			return buffer;
		used += (size_t)rc;
	}
	if (rest)
		(void)_snprintf(&buffer[used], size - used, "%s0x%08" PRIx64, used ? ", " : "", rest);
	return buffer;
}

const char* freerdp_client_stats_codec_name(uint64_t codecId, uint64_t capsVersion)
{
	switch (codecId)
	{
		case RDPGFX_CODECID_UNCOMPRESSED:
			return "Uncompressed";
#if defined(WITH_GFX_AV1) || defined(WITH_GFX_AV2)
		case RDPGFX_CODECID_AV1:
#if defined(WITH_GFX_AV2)
			if (capsVersion == RDPGFX_CAPVERSION_FRDP_2)
				return "AV2";
#endif
			return "AV1";
#endif
		case RDPGFX_CODECID_CAVIDEO:
			return "RemoteFX";
		case RDPGFX_CODECID_CLEARCODEC:
			return "ClearCodec";
		case RDPGFX_CODECID_CAPROGRESSIVE:
			return "Progressive";
		case RDPGFX_CODECID_PLANAR:
			return "Planar";
		case RDPGFX_CODECID_AVC420:
			return "H.264 4:2:0 (AVC420)";
		case RDPGFX_CODECID_ALPHA:
			return "Alpha";
		case RDPGFX_CODECID_CAPROGRESSIVE_V2:
			return "Progressive v2";
		case RDPGFX_CODECID_AVC444:
			return "H.264 4:4:4 (AVC444)";
		case RDPGFX_CODECID_AVC444v2:
			return "H.264 4:4:4 (AVC444v2)";
#if defined(WITH_GFX_AZURE)
		case RDPGFX_CODECID_HEVC420:
			return "HEVC 4:2:0";
		case RDPGFX_CODECID_HEVC444:
			return "HEVC 4:4:4";
#endif
		case UINT32_MAX:
			return "no picture yet";
		default:
			WINPR_UNUSED(capsVersion);
			return "unknown";
	}
}

#if !defined(_WIN32)
#include <pthread.h>

/* The reader of the web client (built in every build but Windows, so tests run it; only the web
 * client calls it, through CLIENT_STATS_WEB_*). A page has one session and no pointer to it, and
 * it reads on the browser's main thread while the session runs on its own, which frees what the
 * reader looks at:
 *
 * - gdi with the session's PostDisconnect, so the session is detached before that;
 * - the graphics channel's context in rdp_client_disconnect(), before any PostDisconnect, and on
 *   every auto-reconnect, which has no PostDisconnect at all. What always comes first is the
 *   channel's disconnected event, which clears gdi->gfx, so a reader finds nothing from then on.
 *   client_stats_web_barrier() waits for a reader that got there earlier.
 *
 * The session takes the lock only at these points and to copy the surfaces, no data path waits
 * for it. */
static pthread_mutex_t web_lock = PTHREAD_MUTEX_INITIALIZER;
static const rdpContext* web_context = nullptr;

/* The surfaces are read under the graphics context's lock, which the graphics channel's thread
 * holds while it hands work to the browser's main thread: waiting there for it would never end.
 * So that thread copies them itself, after a frame and at most once a second, and the page reads
 * the copy. (No FreeRDP timer: the web build has none, timer.c.) */
#define WEB_SURFACE_VALUES (STATS_SESSION_MAX - STATS_GFX_SURFACES)
static uint64_t web_surfaces[WEB_SURFACE_VALUES] = WINPR_C_ARRAY_INIT;
static uint64_t web_surfaces_ns = 0;

/* What comes from the settings is copied the same way: the session frees and replaces
 * context->settings and the remote settings when it connects again (an auto-reconnect, a
 * redirection) while the page stays attached, so the page never reads them itself. */
#define WEB_SERVER_VALUES (STATS_SERVER_PORT + 1 - STATS_RDP_VERSION)
static uint64_t web_server[WEB_SERVER_VALUES] = WINPR_C_ARRAY_INIT;
static char web_strings[STRINGS_MAX][256] = WINPR_C_ARRAY_INIT;

/* with the lock held */
static void web_clear(void)
{
	memset(web_surfaces, 0, sizeof(web_surfaces));
	memset(web_server, 0, sizeof(web_server));
	memset(web_strings, 0, sizeof(web_strings));
}

#if defined(CHANNEL_RDPGFX_CLIENT)
static pcRdpgfxEndFrame web_end_frame_next = nullptr;

static UINT web_end_frame(RdpgfxClientContext* context, const RDPGFX_END_FRAME_PDU* endFrame)
{
	const UINT rc = web_end_frame_next ? web_end_frame_next(context, endFrame) : CHANNEL_RC_OK;

	const uint64_t now = winpr_GetTickCount64NS();
	if (web_surfaces_ns && (now - web_surfaces_ns < 1000ull * 1000ull * 1000ull))
		return rc;
	web_surfaces_ns = now;

	uint64_t values[WEB_SURFACE_VALUES] = WINPR_C_ARRAY_INIT;
	for (size_t x = 0; x < WEB_SURFACE_VALUES; x++)
		values[x] = surface_value(context, STATS_GFX_SURFACES + x);

	/* The settings, on this thread: the channel closes, and this thread stops, before the
	 * session replaces them. The session the page reads is this one: attached at PostConnect,
	 * detached at PostDisconnect after the channels closed. */
	(void)pthread_mutex_lock(&web_lock);
	const rdpContext* session = web_context;
	(void)pthread_mutex_unlock(&web_lock);

	uint64_t server[WEB_SERVER_VALUES] = WINPR_C_ARRAY_INIT;
	char strings[STRINGS_MAX][256] = WINPR_C_ARRAY_INIT;
	if (session)
	{
		for (size_t x = 0; x < WEB_SERVER_VALUES; x++)
			server[x] = server_value(session, STATS_RDP_VERSION + x);
		for (size_t x = 0; x < STRINGS_MAX; x++)
		{
			const char* v = freerdp_client_stats_string_value_for_index(session, x);
			(void)_snprintf(strings[x], sizeof(strings[x]), "%s", v ? v : "");
		}
	}

	/* the lock only to copy: never while waiting for the graphics context's */
	(void)pthread_mutex_lock(&web_lock);
	memcpy(web_surfaces, values, sizeof(web_surfaces));
	memcpy(web_server, server, sizeof(web_server));
	memcpy(web_strings, strings, sizeof(web_strings));
	(void)pthread_mutex_unlock(&web_lock);
	return rc;
}
#endif

void client_stats_web_gfx_connected(RdpgfxClientContext* gfx)
{
#if defined(CHANNEL_RDPGFX_CLIENT)
	if (!gfx || (gfx->EndFrame == web_end_frame))
		return;
	web_end_frame_next = gfx->EndFrame;
	gfx->EndFrame = web_end_frame;
	web_surfaces_ns = 0;
#else
	WINPR_UNUSED(gfx);
#endif
}

void client_stats_web_attach(const rdpContext* context)
{
	(void)pthread_mutex_lock(&web_lock);
	web_context = context;
	web_clear();
	(void)pthread_mutex_unlock(&web_lock);
}

void client_stats_web_detach(const rdpContext* context)
{
	(void)pthread_mutex_lock(&web_lock);
	if (web_context == context)
		web_context = nullptr;
	web_clear();
	(void)pthread_mutex_unlock(&web_lock);
}

void client_stats_web_barrier(void)
{
	/* the graphics channel is gone, and the connection may be on its way to the next one: the
	 * copies go with it */
	(void)pthread_mutex_lock(&web_lock);
	web_clear();
	(void)pthread_mutex_unlock(&web_lock);
}

uint64_t client_stats_web_value(size_t index)
{
	uint64_t value = 0;
	(void)pthread_mutex_lock(&web_lock);
	if (!web_context)
		value = 0;
	else if ((index >= STATS_GFX_SURFACES) && (index < STATS_SESSION_MAX))
		value = web_surfaces[index - STATS_GFX_SURFACES];
	else if ((index >= STATS_RDP_VERSION) && (index <= STATS_SERVER_PORT))
		value = web_server[index - STATS_RDP_VERSION];
	else
		value = freerdp_client_stats_value_for_index(web_context, index);
	(void)pthread_mutex_unlock(&web_lock);
	return value;
}

const char* client_stats_web_string(size_t index, char* buffer, size_t size)
{
	if (!buffer || (size == 0))
		return nullptr;
	(void)pthread_mutex_lock(&web_lock);
	const char* v = (web_context && (index < STRINGS_MAX)) ? web_strings[index] : "";
	(void)_snprintf(buffer, size, "%s", v);
	(void)pthread_mutex_unlock(&web_lock);
	return buffer;
}
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

/* For the page: Module._freerdp_web_stats_count(), Module._freerdp_web_stats_value(i) and
 * Module.ccall("freerdp_web_stats_name", "string", ["number"], [i]). Names say what a value is,
 * indices can differ between versions. A value comes as a double, which a page reads without
 * BigInt, exact up to 2^53. Values are 0 without a session. */
EMSCRIPTEN_KEEPALIVE size_t freerdp_web_stats_count(void);
EMSCRIPTEN_KEEPALIVE size_t freerdp_web_stats_count(void)
{
	return freerdp_client_stats_max_index();
}

EMSCRIPTEN_KEEPALIVE const char* freerdp_web_stats_name(size_t index);
EMSCRIPTEN_KEEPALIVE const char* freerdp_web_stats_name(size_t index)
{
	return freerdp_client_stats_name_for_index(index);
}

EMSCRIPTEN_KEEPALIVE double freerdp_web_stats_value(size_t index);
EMSCRIPTEN_KEEPALIVE double freerdp_web_stats_value(size_t index)
{
	return (double)client_stats_web_value(index);
}

/* The information strings, the same way: Module._freerdp_web_info_count() and
 * Module.ccall("freerdp_web_info_name" / "freerdp_web_info_value", "string", ["number"], [i]).
 * A value is copied under the lock (the session may replace it), "" for none. The name
 * functions below return strings the page reads at once, as ccall does; one buffer each. */
EMSCRIPTEN_KEEPALIVE size_t freerdp_web_info_count(void);
EMSCRIPTEN_KEEPALIVE size_t freerdp_web_info_count(void)
{
	return freerdp_client_stats_string_max_index();
}

EMSCRIPTEN_KEEPALIVE const char* freerdp_web_info_name(size_t index);
EMSCRIPTEN_KEEPALIVE const char* freerdp_web_info_name(size_t index)
{
	return freerdp_client_stats_string_name_for_index(index);
}

EMSCRIPTEN_KEEPALIVE const char* freerdp_web_info_value(size_t index);
EMSCRIPTEN_KEEPALIVE const char* freerdp_web_info_value(size_t index)
{
	static char value[512] = WINPR_C_ARRAY_INIT;
	return client_stats_web_string(index, value, sizeof(value));
}

EMSCRIPTEN_KEEPALIVE const char* freerdp_web_rdp_version_name(double version);
EMSCRIPTEN_KEEPALIVE const char* freerdp_web_rdp_version_name(double version)
{
	return freerdp_client_stats_rdp_version_name((uint64_t)version);
}

EMSCRIPTEN_KEEPALIVE const char* freerdp_web_os_name(double major, double minor);
EMSCRIPTEN_KEEPALIVE const char* freerdp_web_os_name(double major, double minor)
{
	static char name[128] = WINPR_C_ARRAY_INIT;
	return freerdp_client_stats_os_name((uint64_t)major, (uint64_t)minor, name, sizeof(name));
}

EMSCRIPTEN_KEEPALIVE const char* freerdp_web_caps_version_name(double version);
EMSCRIPTEN_KEEPALIVE const char* freerdp_web_caps_version_name(double version)
{
	return freerdp_client_stats_caps_version_name((uint64_t)version);
}

EMSCRIPTEN_KEEPALIVE const char* freerdp_web_caps_flags_name(double flags);
EMSCRIPTEN_KEEPALIVE const char* freerdp_web_caps_flags_name(double flags)
{
	static char name[256] = WINPR_C_ARRAY_INIT;
	return freerdp_client_stats_caps_flags_name((uint64_t)flags, name, sizeof(name));
}

EMSCRIPTEN_KEEPALIVE const char* freerdp_web_codec_name(double codecId, double capsVersion);
EMSCRIPTEN_KEEPALIVE const char* freerdp_web_codec_name(double codecId, double capsVersion)
{
	return freerdp_client_stats_codec_name((uint64_t)codecId, (uint64_t)capsVersion);
}
#endif
