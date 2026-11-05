/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Unit test for the runtime statistics of a client session
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

#include <stdio.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/stream.h>
#include <winpr/thread.h>

#include <freerdp/addin.h>
#include <freerdp/autodetect.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/freerdp.h>

#if defined(CHANNEL_RDPGFX_CLIENT) || defined(CHANNEL_RDPSND_CLIENT)
#include <freerdp/dvc.h>
#endif

#if defined(CHANNEL_RDPGFX_CLIENT)
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/codec/zgfx.h>
#include <freerdp/gdi/gdi.h>
#endif

#if defined(CHANNEL_RDPSND_CLIENT)
#include <winpr/synch.h>

#include <freerdp/channels/rdpsnd.h>
#include <freerdp/client/rdpsnd.h>
#include <freerdp/codec/audio.h>
#endif

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define SESSION_STATS 14

/* The default settings need rdpdr, which brings rdpsnd along: without it no connection starts */
#if !defined(_WIN32) && defined(CHANNEL_RDPSND_CLIENT)
#define TEST_NETWORK
#endif

#define TEST_CHECK(cond)                                                             \
	do                                                                               \
	{                                                                                \
		if (!(cond))                                                                 \
		{                                                                            \
			(void)fprintf(stderr, "%s:%d: failed: %s\n", __func__, __LINE__, #cond); \
			return FALSE;                                                            \
		}                                                                            \
	} while (0)

static BOOL find_stat(const char* name, size_t* index)
{
	for (size_t x = 0; x < freerdp_client_stats_max_index(); x++)
	{
		if (strcmp(freerdp_client_stats_name_for_index(x), name) == 0)
		{
			*index = x;
			return TRUE;
		}
	}
	(void)fprintf(stderr, "no stat named %s\n", name);
	return FALSE;
}

static uint64_t stat_value(const rdpContext* context, const char* name)
{
	size_t index = 0;
	if (!find_stat(name, &index))
		return UINT64_MAX;
	return freerdp_client_stats_value_for_index(context, index);
}

static BOOL check_names(void)
{
	static const char* const session[SESSION_STATS] = {
		"NETWORK_BYTES_RECEIVED",   "NETWORK_BYTES_SENT",
		"NETWORK_PACKETS_RECEIVED", "NETWORK_PACKETS_SENT",
		"NETWORK_BANDWIDTH_KBPS",   "NETWORK_RTT_BASE_MS",
		"NETWORK_RTT_AVERAGE_MS",   "AUDIO_WAVES",
		"AUDIO_WAVE_BYTES",         "AUDIO_WAVES_OVERRUN",
		"AUDIO_WAVES_UNDECODABLE",  "AUDIO_DEVICE_OPENS",
		"AUDIO_FORMAT_TAG",         "AUDIO_DELAY_MS"
	};

	const size_t max = freerdp_client_stats_max_index();
#if defined(CHANNEL_RDPGFX_CLIENT)
	TEST_CHECK(max == SESSION_STATS + rdpgfx_stats_max_index());
#else
	TEST_CHECK(max == SESSION_STATS);
#endif

	for (size_t x = 0; x < max; x++)
	{
		const char* name = freerdp_client_stats_name_for_index(x);
		TEST_CHECK(name);
		TEST_CHECK(strlen(name) > 0);
		TEST_CHECK(strcmp(name, "FREERDP_CLIENT_STATS_UNUSED") != 0);
	}
	TEST_CHECK(strcmp(freerdp_client_stats_name_for_index(max), "FREERDP_CLIENT_STATS_UNUSED") ==
	           0);

	/* each exactly once, in all the statistics */
	for (size_t x = 0; x < ARRAYSIZE(session); x++)
	{
		size_t found = 0;
		for (size_t y = 0; y < max; y++)
		{
			if (strcmp(freerdp_client_stats_name_for_index(y), session[x]) == 0)
				found++;
		}
		TEST_CHECK(found == 1);
	}
	return TRUE;
}

static BOOL check_all_zero(const rdpContext* context)
{
	for (size_t x = 0; x <= freerdp_client_stats_max_index(); x++)
		TEST_CHECK(freerdp_client_stats_value_for_index(context, x) == 0);
	return TRUE;
}

static BOOL check_autodetect(rdpContext* context)
{
	/* a fresh session, nothing received or sent, no gdi and no graphics channel */
	TEST_CHECK(!context->gdi);
	TEST_CHECK(check_all_zero(context));

	context->autodetect->netCharBandwidth = 1001;
	context->autodetect->netCharBaseRTT = 1002;
	context->autodetect->netCharAverageRTT = 1003;
	TEST_CHECK(stat_value(context, "NETWORK_BANDWIDTH_KBPS") == 1001);
	TEST_CHECK(stat_value(context, "NETWORK_RTT_BASE_MS") == 1002);
	TEST_CHECK(stat_value(context, "NETWORK_RTT_AVERAGE_MS") == 1003);

	context->autodetect->netCharBandwidth = 0;
	context->autodetect->netCharBaseRTT = 0;
	context->autodetect->netCharAverageRTT = 0;
	TEST_CHECK(check_all_zero(context));

	/* a context on its way down has neither */
	context->autodetect->netCharBandwidth = 1001;
	rdpAutoDetect* autodetect = context->autodetect;
	rdpRdp* rdp = context->rdp;
	context->autodetect = nullptr;
	context->rdp = nullptr;
	const BOOL rc = check_all_zero(context);
	context->autodetect = autodetect;
	context->rdp = rdp;
	context->autodetect->netCharBandwidth = 0;
	return rc;
}

#if defined(TEST_NETWORK)
#define SERVER_BYTES 16

typedef struct
{
	int listener;
	int rc;
} test_server;

/* Takes the client's first packet and answers with something that is no RDP at all: enough for
 * bytes to pass both ways through the real transport. */
static DWORD WINAPI test_server_thread(LPVOID arg)
{
	test_server* server = (test_server*)arg;
	BYTE buffer[256] = WINPR_C_ARRAY_INIT;

	const int client = accept(server->listener, nullptr, nullptr);

	/* the client tries again with the next protocol: refuse it, or it waits here for an answer */
	(void)close(server->listener);
	server->listener = -1;
	if (client < 0)
		return 1;

	if (recv(client, buffer, sizeof(buffer), 0) > 0)
	{
		memset(buffer, 0xff, SERVER_BYTES);
		if (send(client, buffer, SERVER_BYTES, 0) == SERVER_BYTES)
			server->rc = 0;
	}
	(void)close(client);
	return 0;
}

static BOOL check_network(rdpContext* context)
{
	BOOL rc = FALSE;
	test_server server = { .listener = -1, .rc = -1 };
	struct sockaddr_in addr = WINPR_C_ARRAY_INIT;
	socklen_t len = sizeof(addr);
	HANDLE thread = nullptr;

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	/* accept() gives up as well, should the client never come */
	const struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };

	server.listener = socket(AF_INET, SOCK_STREAM, 0);
	if ((server.listener < 0) ||
	    (setsockopt(server.listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) ||
	    (bind(server.listener, (struct sockaddr*)&addr, sizeof(addr)) != 0) ||
	    (listen(server.listener, 1) != 0) ||
	    (getsockname(server.listener, (struct sockaddr*)&addr, &len) != 0))
		goto fail;

	if (!freerdp_settings_set_string(context->settings, FreeRDP_ServerHostname, "127.0.0.1") ||
	    !freerdp_settings_set_uint32(context->settings, FreeRDP_ServerPort, ntohs(addr.sin_port)) ||
	    !freerdp_settings_set_string(context->settings, FreeRDP_Username, "stats") ||
	    !freerdp_settings_set_bool(context->settings, FreeRDP_AutoReconnectionEnabled, FALSE))
		goto fail;

	thread = CreateThread(nullptr, 0, test_server_thread, &server, 0, nullptr);
	if (!thread)
		goto fail;

	/* fails: the other side is no RDP server, what passed until then is what counts */
	(void)freerdp_connect(context->instance);
	(void)WaitForSingleObject(thread, INFINITE);
	(void)CloseHandle(thread);

	if (server.rc != 0)
		goto fail;
	/* the server sent 16 bytes, the client's connection request is longer than that */
	{
		const uint64_t received = stat_value(context, "NETWORK_BYTES_RECEIVED");
		const uint64_t sent = stat_value(context, "NETWORK_BYTES_SENT");
		(void)printf("network: received %" PRIu64 ", sent %" PRIu64 "\n", received, sent);
		TEST_CHECK((received > 0) && (received <= SERVER_BYTES));
		TEST_CHECK(sent > SERVER_BYTES);
	}
	rc = TRUE;
fail:
	if (server.listener >= 0)
		(void)close(server.listener);
	if (!rc)
		(void)fprintf(stderr, "[%s] failed\n", __func__);
	return rc;
}
#endif

#if defined(CHANNEL_RDPGFX_CLIENT) || defined(CHANNEL_RDPSND_CLIENT)
typedef struct
{
	IDRDYNVC_ENTRY_POINTS iface;
	rdpContext* context;
	IWTSPlugin* plugin;
	const ADDIN_ARGV* args;
} test_entry_points;

typedef struct
{
	IWTSVirtualChannelManager iface;
	IWTSListener listener;
	IWTSListenerCallback* listenerCallback;
} test_channel_manager;

static UINT test_register_plugin(IDRDYNVC_ENTRY_POINTS* pEntryPoints,
                                 WINPR_ATTR_UNUSED const char* name, IWTSPlugin* pPlugin)
{
	((test_entry_points*)pEntryPoints)->plugin = pPlugin;
	return CHANNEL_RC_OK;
}

static IWTSPlugin* test_get_plugin(WINPR_ATTR_UNUSED IDRDYNVC_ENTRY_POINTS* pEntryPoints,
                                   WINPR_ATTR_UNUSED const char* name)
{
	return nullptr;
}

static const ADDIN_ARGV* test_get_plugin_data(IDRDYNVC_ENTRY_POINTS* pEntryPoints)
{
	return ((test_entry_points*)pEntryPoints)->args;
}

static rdpSettings* test_get_rdp_settings(IDRDYNVC_ENTRY_POINTS* pEntryPoints)
{
	return ((test_entry_points*)pEntryPoints)->context->settings;
}

static rdpContext* test_get_rdp_context(IDRDYNVC_ENTRY_POINTS* pEntryPoints)
{
	return ((test_entry_points*)pEntryPoints)->context;
}

static UINT test_create_listener(IWTSVirtualChannelManager* pChannelMgr,
                                 WINPR_ATTR_UNUSED const char* pszChannelName,
                                 WINPR_ATTR_UNUSED ULONG ulFlags,
                                 IWTSListenerCallback* pListenerCallback, IWTSListener** ppListener)
{
	test_channel_manager* mgr = (test_channel_manager*)pChannelMgr;
	mgr->listenerCallback = pListenerCallback;
	if (ppListener)
		*ppListener = &mgr->listener;
	return CHANNEL_RC_OK;
}

static UINT test_destroy_listener(IWTSVirtualChannelManager* pChannelMgr,
                                  WINPR_ATTR_UNUSED IWTSListener* pListener)
{
	((test_channel_manager*)pChannelMgr)->listenerCallback = nullptr;
	return CHANNEL_RC_OK;
}

static UINT32 test_get_channel_id(WINPR_ATTR_UNUSED IWTSVirtualChannel* channel)
{
	return 1;
}

static const char* test_get_channel_name(WINPR_ATTR_UNUSED IWTSVirtualChannel* channel)
{
	return RDPGFX_DVC_CHANNEL_NAME;
}

static IWTSVirtualChannel* test_find_channel_by_id(WINPR_ATTR_UNUSED IWTSVirtualChannelManager* mgr,
                                                   WINPR_ATTR_UNUSED UINT32 ChannelId)
{
	return nullptr;
}

static UINT test_channel_write(WINPR_ATTR_UNUSED IWTSVirtualChannel* pChannel,
                               WINPR_ATTR_UNUSED ULONG cbSize,
                               WINPR_ATTR_UNUSED const BYTE* pBuffer,
                               WINPR_ATTR_UNUSED void* pReserved)
{
	return CHANNEL_RC_OK;
}

static UINT test_channel_close(WINPR_ATTR_UNUSED IWTSVirtualChannel* pChannel)
{
	return CHANNEL_RC_OK;
}

static BOOL load_plugin(const char* name, rdpContext* context, const ADDIN_ARGV* args,
                        test_entry_points* ep, test_channel_manager* mgr)
{
	PDVC_PLUGIN_ENTRY pluginEntry =
	    WINPR_FUNC_PTR_CAST(freerdp_channels_load_static_addin_entry(name, nullptr, nullptr,
	                                                                 FREERDP_ADDIN_CHANNEL_DYNAMIC),
	                        PDVC_PLUGIN_ENTRY);
	TEST_CHECK(pluginEntry);

	ep->iface.RegisterPlugin = test_register_plugin;
	ep->iface.GetPlugin = test_get_plugin;
	ep->iface.GetPluginData = test_get_plugin_data;
	ep->iface.GetRdpSettings = test_get_rdp_settings;
	ep->iface.GetRdpContext = test_get_rdp_context;
	ep->context = context;
	ep->args = args;

	TEST_CHECK(pluginEntry(&ep->iface) == CHANNEL_RC_OK);
	TEST_CHECK(ep->plugin);

	mgr->iface.CreateListener = test_create_listener;
	mgr->iface.DestroyListener = test_destroy_listener;
	mgr->iface.GetChannelId = test_get_channel_id;
	mgr->iface.GetChannelName = test_get_channel_name;
	mgr->iface.FindChannelById = test_find_channel_by_id;
	return TRUE;
}
#endif

#if defined(CHANNEL_RDPGFX_CLIENT)
static UINT send_start_frame(IWTSVirtualChannelCallback* callback)
{
	wStream* s = Stream_New(nullptr, 2 + RDPGFX_HEADER_SIZE + RDPGFX_START_FRAME_PDU_SIZE);
	if (!s)
		return CHANNEL_RC_NO_MEMORY;
	Stream_Write_UINT8(s, ZGFX_SEGMENTED_SINGLE);
	Stream_Write_UINT8(s, ZGFX_PACKET_COMPR_TYPE_RDP8);
	Stream_Write_UINT16(s, RDPGFX_CMDID_STARTFRAME);
	Stream_Write_UINT16(s, 0); /* flags */
	Stream_Write_UINT32(s, RDPGFX_HEADER_SIZE + RDPGFX_START_FRAME_PDU_SIZE);
	Stream_Write_UINT32(s, 0); /* timestamp */
	Stream_Write_UINT32(s, 1); /* frameId */
	Stream_SealLength(s);
	Stream_ResetPosition(s);
	const UINT rc = callback->OnDataReceived(callback, s);
	Stream_Free(s, TRUE);
	return rc;
}

/* a graphics channel, reached the way a session reaches it: context->gdi->gfx */
static BOOL check_gfx_channel(rdpContext* context, IWTSPlugin* plugin, test_channel_manager* mgr)
{
	IWTSVirtualChannel channel = WINPR_C_ARRAY_INIT;
	channel.Write = test_channel_write;
	channel.Close = test_channel_close;

	TEST_CHECK(plugin->Initialize(plugin, &mgr->iface) == CHANNEL_RC_OK);
	TEST_CHECK(mgr->listenerCallback);

	IWTSVirtualChannelCallback* callback = nullptr;
	BOOL accept = TRUE;
	TEST_CHECK(mgr->listenerCallback->OnNewChannelConnection(
	               mgr->listenerCallback, &channel, nullptr, &accept, &callback) == CHANNEL_RC_OK);
	TEST_CHECK(callback);
	TEST_CHECK(callback->OnOpen(callback) == CHANNEL_RC_OK);

	rdpGdi gdi = WINPR_C_ARRAY_INIT;
	context->gdi = &gdi;

	/* no gfx yet, as on a session whose graphics channel closed */
	BOOL rc = stat_value(context, "RDPGFX_CMDID_STARTFRAME") == 0;

	if (rc)
	{
		gdi.gfx = (RdpgfxClientContext*)plugin->pInterface;
		rc = (stat_value(context, "RDPGFX_CMDID_STARTFRAME") == 0) &&
		     (send_start_frame(callback) == CHANNEL_RC_OK) &&
		     (stat_value(context, "RDPGFX_CMDID_STARTFRAME") == 1) &&
		     (stat_value(context, "RDPGFX_CMDID_ENDFRAME") == 0);
		gdi.gfx = nullptr;
		rc = rc && (stat_value(context, "RDPGFX_CMDID_STARTFRAME") == 0);
	}

	context->gdi = nullptr;
	(void)callback->OnClose(callback);
	TEST_CHECK(rc);
	return TRUE;
}

static BOOL check_gfx(rdpContext* context)
{
	test_entry_points ep = WINPR_C_ARRAY_INIT;
	test_channel_manager mgr = WINPR_C_ARRAY_INIT;
	TEST_CHECK(load_plugin(RDPGFX_CHANNEL_NAME, context, nullptr, &ep, &mgr));

	const BOOL rc = check_gfx_channel(context, ep.plugin, &mgr);

	/* frees the plugin */
	(void)ep.plugin->Terminated(ep.plugin);
	return rc;
}
#endif

#if defined(CHANNEL_RDPSND_CLIENT)
#define TEST_RATE 8000u

static void write_audio_format(wStream* s, UINT16 tag)
{
	Stream_Write_UINT16(s, tag);
	Stream_Write_UINT16(s, 1);         /* nChannels */
	Stream_Write_UINT32(s, TEST_RATE); /* nSamplesPerSec */
	Stream_Write_UINT32(s, TEST_RATE); /* nAvgBytesPerSec */
	Stream_Write_UINT16(s, 1);         /* nBlockAlign */
	Stream_Write_UINT16(s, 8);         /* wBitsPerSample */
	Stream_Write_UINT16(s, 0);         /* cbSize */
}

static UINT send_rdpsnd_pdu(IWTSVirtualChannelCallback* callback, wStream* s)
{
	Stream_SealLength(s);
	Stream_ResetPosition(s);
	const UINT rc = callback->OnDataReceived(callback, s);
	Stream_Free(s, TRUE);
	return rc;
}

static wStream* new_rdpsnd_pdu(BYTE msgType, UINT16 bodySize)
{
	wStream* s = Stream_New(nullptr, 4ull + bodySize);
	if (s)
	{
		Stream_Write_UINT8(s, msgType);
		Stream_Write_UINT8(s, 0); /* bPad */
		Stream_Write_UINT16(s, bodySize);
	}
	return s;
}

static UINT send_formats(IWTSVirtualChannelCallback* callback)
{
	const UINT16 formats = 2;
	const UINT16 body = 20 + formats * 18;
	wStream* s = new_rdpsnd_pdu(SNDC_FORMATS, body);
	if (!s)
		return CHANNEL_RC_NO_MEMORY;
	Stream_Zero(s, 14); /* dwFlags, dwVolume, dwPitch, wDGramPort */
	Stream_Write_UINT16(s, formats);
	Stream_Write_UINT8(s, 0);  /* cLastBlockConfirmed */
	Stream_Write_UINT16(s, 6); /* wVersion */
	Stream_Write_UINT8(s, 0);  /* bPad */
	write_audio_format(s, WAVE_FORMAT_PCM);
	write_audio_format(s, WAVE_FORMAT_ALAW);
	return send_rdpsnd_pdu(callback, s);
}

static UINT send_wave2(IWTSVirtualChannelCallback* callback, UINT16 formatNo, UINT16 size)
{
	wStream* s = new_rdpsnd_pdu(SNDC_WAVE2, 12 + size);
	if (!s)
		return CHANNEL_RC_NO_MEMORY;
	Stream_Write_UINT16(s, 0); /* wTimeStamp */
	Stream_Write_UINT16(s, formatNo);
	Stream_Zero(s, 8); /* cBlockNo, bPad, dwAudioTimeStamp */
	Stream_Zero(s, size);
	return send_rdpsnd_pdu(callback, s);
}

/* The old pair of PDUs: the wave's arrival is the first one, so the time between shows */
static BOOL send_wave_pair(IWTSVirtualChannelCallback* callback, UINT16 formatNo, UINT16 size,
                           DWORD delayMs)
{
	wStream* s = new_rdpsnd_pdu(SNDC_WAVE, 8 + size);
	TEST_CHECK(s);
	Stream_Write_UINT16(s, 0); /* wTimeStamp */
	Stream_Write_UINT16(s, formatNo);
	Stream_Zero(s, 8); /* cBlockNo, bPad, Data */
	TEST_CHECK(send_rdpsnd_pdu(callback, s) == CHANNEL_RC_OK);

	Sleep(delayMs);

	s = Stream_New(nullptr, size);
	TEST_CHECK(s);
	Stream_Zero(s, size);
	TEST_CHECK(send_rdpsnd_pdu(callback, s) == CHANNEL_RC_OK);
	return TRUE;
}

static BOOL check_audio_session(const rdpContext* context, IWTSVirtualChannelCallback* callback)
{
	const UINT16 longWave = 60000; /* 7.5 s at this rate */
	const UINT16 shortWave = 800;  /* 100 ms */

	TEST_CHECK(send_formats(callback) == CHANNEL_RC_OK);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES") == 0);
	TEST_CHECK(stat_value(context, "AUDIO_DEVICE_OPENS") == 0);

	TEST_CHECK(send_wave2(callback, 0, longWave) == CHANNEL_RC_OK);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES") == 1);
	TEST_CHECK(stat_value(context, "AUDIO_WAVE_BYTES") == longWave);
	TEST_CHECK(stat_value(context, "AUDIO_DEVICE_OPENS") == 1);
	TEST_CHECK(stat_value(context, "AUDIO_FORMAT_TAG") == WAVE_FORMAT_PCM);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES_OVERRUN") == 0);

	/* seconds are queued now, so this one has no place: counted, and not played */
	TEST_CHECK(send_wave2(callback, 0, shortWave) == CHANNEL_RC_OK);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES") == 2);
	TEST_CHECK(stat_value(context, "AUDIO_WAVE_BYTES") == 1ull * longWave + shortWave);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES_OVERRUN") == 1);
	TEST_CHECK(stat_value(context, "AUDIO_DEVICE_OPENS") == 1);

	/* another format opens the device again */
	TEST_CHECK(send_wave2(callback, 1, shortWave) == CHANNEL_RC_OK);
	TEST_CHECK(stat_value(context, "AUDIO_DEVICE_OPENS") == 2);
	TEST_CHECK(stat_value(context, "AUDIO_FORMAT_TAG") == WAVE_FORMAT_ALAW);

	TEST_CHECK(send_wave_pair(callback, 1, shortWave, 50));
	TEST_CHECK(stat_value(context, "AUDIO_WAVES") == 4);
	TEST_CHECK(stat_value(context, "AUDIO_DEVICE_OPENS") == 2);
	TEST_CHECK(stat_value(context, "AUDIO_DELAY_MS") >= 40);
	return TRUE;
}

typedef BOOL (*audio_session_fn)(const rdpContext* context, IWTSVirtualChannelCallback* callback);

static BOOL check_audio_channel(const rdpContext* context, IWTSPlugin* plugin,
                                test_channel_manager* mgr, audio_session_fn session)
{
	IWTSVirtualChannel channel = WINPR_C_ARRAY_INIT;
	channel.Write = test_channel_write;
	channel.Close = test_channel_close;

	TEST_CHECK(plugin->Initialize(plugin, &mgr->iface) == CHANNEL_RC_OK);
	TEST_CHECK(mgr->listenerCallback);

	BOOL accept = TRUE;
	IWTSVirtualChannelCallback* callback = nullptr;
	TEST_CHECK(mgr->listenerCallback->OnNewChannelConnection(
	               mgr->listenerCallback, &channel, nullptr, &accept, &callback) == CHANNEL_RC_OK);
	TEST_CHECK(callback);
	TEST_CHECK(callback->OnOpen(callback) == CHANNEL_RC_OK);

	const BOOL rc = session(context, callback);

	/* frees the channel callback */
	(void)callback->OnClose(callback);
	return rc;
}

static BOOL run_audio(rdpContext* context, const char* backend, audio_session_fn session)
{
	char name[] = RDPSND_CHANNEL_NAME;
	char subsystem[32] = WINPR_C_ARRAY_INIT;
	(void)_snprintf(subsystem, sizeof(subsystem), "sys:%s", backend);
	char* argv[] = { name, subsystem };
	const ADDIN_ARGV args = { ARRAYSIZE(argv), argv };

	test_entry_points ep = WINPR_C_ARRAY_INIT;
	test_channel_manager mgr = WINPR_C_ARRAY_INIT;
	TEST_CHECK(load_plugin(RDPSND_CHANNEL_NAME, context, &args, &ep, &mgr));

	const BOOL rc = check_audio_channel(context, ep.plugin, &mgr, session);

	/* frees the plugin */
	(void)ep.plugin->Terminated(ep.plugin);
	return rc;
}

#if !defined(WITH_DSP_FFMPEG)
#define TEST_UNDECODABLE

/* A backend that takes every format when they are announced and none when a wave comes, so the
 * wave goes to the decoder. The built-in one has no A-law, so it refuses it. The fake backend
 * takes every format at any time, and no other backend is sure to be there. */
#define STUB_BACKEND "stub"

static BOOL stubTakesFormats = TRUE;

static BOOL stub_format_supported(WINPR_ATTR_UNUSED rdpsndDevicePlugin* device,
                                  WINPR_ATTR_UNUSED const AUDIO_FORMAT* format)
{
	return stubTakesFormats;
}

static BOOL stub_open(WINPR_ATTR_UNUSED rdpsndDevicePlugin* device,
                      WINPR_ATTR_UNUSED const AUDIO_FORMAT* format,
                      WINPR_ATTR_UNUSED UINT32 latency)
{
	return TRUE;
}

static BOOL stub_set_volume(WINPR_ATTR_UNUSED rdpsndDevicePlugin* device,
                            WINPR_ATTR_UNUSED UINT32 value)
{
	return TRUE;
}

static UINT stub_play(WINPR_ATTR_UNUSED rdpsndDevicePlugin* device,
                      WINPR_ATTR_UNUSED const BYTE* data, WINPR_ATTR_UNUSED size_t size)
{
	return 0;
}

static void stub_free(rdpsndDevicePlugin* device)
{
	free(device);
}

static UINT VCAPITYPE stub_entry(PFREERDP_RDPSND_DEVICE_ENTRY_POINTS entryPoints)
{
	rdpsndDevicePlugin* device = calloc(1, sizeof(rdpsndDevicePlugin));
	if (!device)
		return CHANNEL_RC_NO_MEMORY;

	device->FormatSupported = stub_format_supported;
	device->Open = stub_open;
	device->SetVolume = stub_set_volume;
	device->Play = stub_play;
	device->Free = stub_free;
	entryPoints->pRegisterRdpsndDevice(entryPoints->rdpsnd, device);
	return CHANNEL_RC_OK;
}

static PVIRTUALCHANNELENTRY stub_provider(LPCSTR name, LPCSTR subsystem, LPCSTR type, DWORD flags)
{
	if (subsystem && (strcmp(subsystem, STUB_BACKEND) == 0))
	{
		PFREERDP_RDPSND_DEVICE_ENTRY entry = stub_entry;
		return WINPR_FUNC_PTR_CAST(entry, PVIRTUALCHANNELENTRY);
	}
	return freerdp_channels_load_static_addin_entry(name, subsystem, type, flags);
}

static BOOL check_undecodable_session(const rdpContext* context,
                                      IWTSVirtualChannelCallback* callback)
{
	const UINT16 size = 800;
	const uint64_t waves = stat_value(context, "AUDIO_WAVES");
	const uint64_t overrun = stat_value(context, "AUDIO_WAVES_OVERRUN");

	TEST_CHECK(send_formats(callback) == CHANNEL_RC_OK);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES_UNDECODABLE") == 0);

	stubTakesFormats = FALSE;
	const UINT rc = send_wave2(callback, 1, size);
	stubTakesFormats = TRUE;

	/* dropped and confirmed like the others: the channel goes on */
	TEST_CHECK(rc == CHANNEL_RC_OK);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES") == waves + 1);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES_UNDECODABLE") == 1);
	TEST_CHECK(stat_value(context, "AUDIO_WAVES_OVERRUN") == overrun);
	return TRUE;
}

static BOOL check_undecodable(rdpContext* context)
{
	const FREERDP_LOAD_CHANNEL_ADDIN_ENTRY_FN provider = freerdp_get_current_addin_provider();
	TEST_CHECK(freerdp_register_addin_provider(stub_provider, 0) == CHANNEL_RC_OK);
	const BOOL rc = run_audio(context, STUB_BACKEND, check_undecodable_session);
	TEST_CHECK(freerdp_register_addin_provider(provider, 0) == CHANNEL_RC_OK);
	return rc;
}
#endif

static BOOL check_audio(rdpContext* context)
{
	TEST_CHECK(
	    freerdp_settings_set_bool(context->settings, FreeRDP_SynchronousDynamicChannels, TRUE));

	/* the fake backend: a test makes no sound */
	TEST_CHECK(run_audio(context, "fake", check_audio_session));

	/* a reader needs no channel: the figures stay with the context */
	TEST_CHECK(stat_value(context, "AUDIO_WAVES") == 4);

#if defined(TEST_UNDECODABLE)
	TEST_CHECK(check_undecodable(context));
#endif

	TEST_CHECK(
	    freerdp_settings_set_bool(context->settings, FreeRDP_SynchronousDynamicChannels, FALSE));
	return TRUE;
}
#endif

static rdpContext* new_context(void)
{
	RDP_CLIENT_ENTRY_POINTS entry = WINPR_C_ARRAY_INIT;
	entry.Version = RDP_CLIENT_INTERFACE_VERSION;
	entry.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	entry.ContextSize = sizeof(rdpClientContext);
	return freerdp_client_context_new(&entry);
}

int TestClientStats(WINPR_ATTR_UNUSED int argc, WINPR_ATTR_UNUSED char* argv[])
{
	if (!check_names())
		return -1;
	if (!check_all_zero(nullptr))
		return -1;

	rdpContext* context = new_context();
	if (!context)
		return -1;

	BOOL rc = check_autodetect(context);
#if defined(CHANNEL_RDPGFX_CLIENT)
	rc = rc && check_gfx(context);
#endif
#if defined(CHANNEL_RDPSND_CLIENT)
	rc = rc && check_audio(context);
#endif
#if defined(TEST_NETWORK)
	rc = rc && check_network(context);
#endif

	freerdp_client_context_free(context);
	return rc ? 0 : -1;
}
