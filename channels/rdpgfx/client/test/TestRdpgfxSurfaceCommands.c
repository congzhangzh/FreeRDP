/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Unit test for how the rdpgfx (graphics pipeline) client channel passes surface commands on
 *
 * The built-in rdpgfx DVC plugin is loaded through its DVCPluginEntry with a stub dynamic
 * channel manager, as TestFuzzChannelRdpgfx does. The test then plays a whole session as a
 * server would send it (caps confirm, reset graphics, create surface, start frame, surface
 * commands, end frame) and looks only at what the channel hands to the RdpgfxClientContext
 * callbacks and at the frame acknowledgements it writes back.
 *
 * Copyright 2026 Cong Zhang <congzhangzh@gmail.com>
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

#include <stdio.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/stream.h>
#include <winpr/wlog.h>

#include <freerdp/addin.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/codec/zgfx.h>
#include <freerdp/dvc.h>

#define TEST_CHECK(cond)                                                             \
	do                                                                               \
	{                                                                                \
		if (!(cond))                                                                 \
		{                                                                            \
			(void)fprintf(stderr, "%s:%d: failed: %s\n", __func__, __LINE__, #cond); \
			return FALSE;                                                            \
		}                                                                            \
	} while (0)

/* a codec ID that no version of the protocol defines */
#define TEST_CODECID_UNKNOWN 0x0007
/* AV2's codec ID before the merged wire form */
#define TEST_CODECID_AV2_PRE_MERGE 0x0002
/* the Reset Graphics PDU always has this size, padding included */
#define TEST_RESET_GRAPHICS_SIZE 340
#define TEST_MAX_COMMANDS 16

/* What the channel handed on, as the client's callbacks saw it */
typedef struct
{
	UINT32 commands;
	UINT32 codecIds[TEST_MAX_COMMANDS];
	UINT32 endFrames;
	UINT32 lastEndFrame;
	UINT32 acks;
	UINT32 lastAckFrame;
} test_seen;

static test_seen seen = WINPR_C_ARRAY_INIT;

typedef struct
{
	IDRDYNVC_ENTRY_POINTS iface;
	rdpContext* context;
	IWTSPlugin* plugin;
} test_entry_points;

typedef struct
{
	IWTSVirtualChannelManager iface;
	IWTSListener listener;
	IWTSListenerCallback* listenerCallback;
} test_channel_manager;

/* One channel, open, with everything needed to close it again */
typedef struct
{
	rdpContext* context;
	IWTSPlugin* plugin;
	test_channel_manager mgr;
	IWTSVirtualChannel channel;
	IWTSVirtualChannelCallback* callback;
} test_session;

static UINT test_register_plugin(IDRDYNVC_ENTRY_POINTS* pEntryPoints,
                                 WINPR_ATTR_UNUSED const char* name, IWTSPlugin* pPlugin)
{
	test_entry_points* ep = (test_entry_points*)pEntryPoints;
	if (ep->plugin)
		return CHANNEL_RC_ALREADY_INITIALIZED;
	ep->plugin = pPlugin;
	return CHANNEL_RC_OK;
}

static IWTSPlugin* test_get_plugin(WINPR_ATTR_UNUSED IDRDYNVC_ENTRY_POINTS* pEntryPoints,
                                   WINPR_ATTR_UNUSED const char* name)
{
	return nullptr;
}

static const ADDIN_ARGV* test_get_plugin_data(WINPR_ATTR_UNUSED IDRDYNVC_ENTRY_POINTS* pEntryPoints)
{
	return nullptr;
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

/* What the client sends: count the frame acknowledgements */
static UINT test_channel_write(WINPR_ATTR_UNUSED IWTSVirtualChannel* pChannel, ULONG cbSize,
                               const BYTE* pBuffer, WINPR_ATTR_UNUSED void* pReserved)
{
	wStream buffer = WINPR_C_ARRAY_INIT;
	wStream* s = Stream_StaticConstInit(&buffer, pBuffer, cbSize);
	if (!Stream_CheckAndLogRequiredLength("test", s, RDPGFX_HEADER_SIZE))
		return CHANNEL_RC_OK;

	const UINT16 cmdId = Stream_Get_UINT16(s);
	Stream_Seek(s, 6); /* flags, pduLength */
	if ((cmdId == RDPGFX_CMDID_FRAMEACKNOWLEDGE) && Stream_CheckAndLogRequiredLength("test", s, 8))
	{
		Stream_Seek_UINT32(s); /* queueDepth */
		seen.lastAckFrame = Stream_Get_UINT32(s);
		seen.acks++;
	}
	return CHANNEL_RC_OK;
}

static UINT test_channel_close(WINPR_ATTR_UNUSED IWTSVirtualChannel* pChannel)
{
	return CHANNEL_RC_OK;
}

/* The client's callbacks: a surface command with a planar bitstream stands for one the
 * decoder refuses */
static UINT test_surface_command(WINPR_ATTR_UNUSED RdpgfxClientContext* context,
                                 const RDPGFX_SURFACE_COMMAND* cmd)
{
	if (seen.commands < TEST_MAX_COMMANDS)
		seen.codecIds[seen.commands] = cmd->codecId;
	seen.commands++;
	if (cmd->codecId == RDPGFX_CODECID_PLANAR)
		return ERROR_INVALID_DATA;
	return CHANNEL_RC_OK;
}

static UINT test_end_frame(WINPR_ATTR_UNUSED RdpgfxClientContext* context,
                           const RDPGFX_END_FRAME_PDU* endFrame)
{
	seen.lastEndFrame = endFrame->frameId;
	seen.endFrames++;
	return CHANNEL_RC_OK;
}

/* A message from the server: the PDUs in s, in an uncompressed ZGFX segment */
static UINT test_send(test_session* session, wStream* s)
{
	const size_t length = Stream_GetPosition(s);
	wStream* msg = Stream_New(nullptr, length + 2);
	if (!msg)
		return CHANNEL_RC_NO_MEMORY;
	Stream_Write_UINT8(msg, ZGFX_SEGMENTED_SINGLE);
	Stream_Write_UINT8(msg, ZGFX_PACKET_COMPR_TYPE_RDP8);
	Stream_Write(msg, Stream_Buffer(s), length);
	Stream_SealLength(msg);
	Stream_ResetPosition(msg);

	const UINT rc = session->callback->OnDataReceived(session->callback, msg);
	Stream_Free(msg, TRUE);
	Stream_ResetPosition(s);
	return rc;
}

static BOOL test_header(wStream* s, UINT16 cmdId, size_t payload)
{
	if (!Stream_EnsureRemainingCapacity(s, RDPGFX_HEADER_SIZE + payload))
		return FALSE;
	Stream_Write_UINT16(s, cmdId);
	Stream_Write_UINT16(s, 0); /* flags */
	Stream_Write_UINT32(s, (UINT32)(RDPGFX_HEADER_SIZE + payload));
	return TRUE;
}

static BOOL test_caps_confirm(wStream* s, UINT32 version)
{
	if (!test_header(s, RDPGFX_CMDID_CAPSCONFIRM, 12))
		return FALSE;
	Stream_Write_UINT32(s, version);
	Stream_Write_UINT32(s, 4); /* capsDataLength */
	Stream_Write_UINT32(s, 0); /* flags */
	return TRUE;
}

static BOOL test_reset_graphics(wStream* s)
{
	if (!test_header(s, RDPGFX_CMDID_RESETGRAPHICS, TEST_RESET_GRAPHICS_SIZE - RDPGFX_HEADER_SIZE))
		return FALSE;
	Stream_Write_UINT32(s, 64); /* width */
	Stream_Write_UINT32(s, 64); /* height */
	Stream_Write_UINT32(s, 1);  /* monitorCount */
	Stream_Write_INT32(s, 0);   /* left */
	Stream_Write_INT32(s, 0);   /* top */
	Stream_Write_INT32(s, 63);  /* right */
	Stream_Write_INT32(s, 63);  /* bottom */
	Stream_Write_UINT32(s, 1);  /* flags: primary */
	Stream_Zero(s, TEST_RESET_GRAPHICS_SIZE - RDPGFX_HEADER_SIZE - 12 - 20);
	return TRUE;
}

static BOOL test_create_surface(wStream* s)
{
	if (!test_header(s, RDPGFX_CMDID_CREATESURFACE, 7))
		return FALSE;
	Stream_Write_UINT16(s, 1);  /* surfaceId */
	Stream_Write_UINT16(s, 64); /* width */
	Stream_Write_UINT16(s, 64); /* height */
	Stream_Write_UINT8(s, GFX_PIXEL_FORMAT_XRGB_8888);
	return TRUE;
}

static BOOL test_frame(wStream* s, UINT16 cmdId, UINT32 frameId)
{
	if (cmdId == RDPGFX_CMDID_STARTFRAME)
	{
		if (!test_header(s, cmdId, RDPGFX_START_FRAME_PDU_SIZE))
			return FALSE;
		Stream_Write_UINT32(s, 0); /* timestamp */
	}
	else if (!test_header(s, cmdId, 4))
		return FALSE;
	Stream_Write_UINT32(s, frameId);
	return TRUE;
}

/* A Wire To Surface 1 PDU of a 16x16 area. The codecs that carry an AVC420 metablock (AVC420,
 * AV1, AV2) get one with that rect, then a few bytes the stub never decodes. */
static BOOL test_wire_to_surface_1(wStream* s, UINT16 codecId, BOOL metablock)
{
	const size_t length = metablock ? (4 + 8 + 2 + 4) : 16;
	if (!test_header(s, RDPGFX_CMDID_WIRETOSURFACE_1, RDPGFX_WIRE_TO_SURFACE_PDU_1_SIZE + length))
		return FALSE;
	Stream_Write_UINT16(s, 1); /* surfaceId */
	Stream_Write_UINT16(s, codecId);
	Stream_Write_UINT8(s, GFX_PIXEL_FORMAT_XRGB_8888);
	Stream_Write_UINT16(s, 0);  /* destRect.left */
	Stream_Write_UINT16(s, 0);  /* destRect.top */
	Stream_Write_UINT16(s, 16); /* destRect.right */
	Stream_Write_UINT16(s, 16); /* destRect.bottom */
	Stream_Write_UINT32(s, (UINT32)length);
	if (metablock)
	{
		Stream_Write_UINT32(s, 1);  /* numRegionRects */
		Stream_Write_UINT16(s, 0);  /* left */
		Stream_Write_UINT16(s, 0);  /* top */
		Stream_Write_UINT16(s, 16); /* right */
		Stream_Write_UINT16(s, 16); /* bottom */
		Stream_Write_UINT8(s, 22);  /* qpVal */
		Stream_Write_UINT8(s, 100); /* qualityVal */
		Stream_Zero(s, 4);
	}
	else
		Stream_Zero(s, length);
	return TRUE;
}

static void test_close(test_session* session)
{
	if (session->callback)
		(void)session->callback->OnClose(session->callback); /* frees the channel callback */
	if (session->plugin)
		(void)session->plugin->Terminated(session->plugin); /* frees the plugin */
	freerdp_client_context_free(session->context);
	memset(session, 0, sizeof(*session));
}

/* Loads the plugin, opens its channel and plays the server's start of a session up to
 * frame 1: caps confirm with version, reset graphics, a 64x64 surface 1, start frame 1 */
static BOOL test_open(test_session* session, UINT32 version, wStream* s)
{
	memset(&seen, 0, sizeof(seen));

	RDP_CLIENT_ENTRY_POINTS entry = WINPR_C_ARRAY_INIT;
	entry.Version = RDP_CLIENT_INTERFACE_VERSION;
	entry.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	entry.ContextSize =
	    sizeof(rdpClientContext); /* the client API needs the whole client context */
	session->context = freerdp_client_context_new(&entry);
	TEST_CHECK(session->context);

	PDVC_PLUGIN_ENTRY pluginEntry = WINPR_FUNC_PTR_CAST(
	    freerdp_channels_load_static_addin_entry(RDPGFX_CHANNEL_NAME, nullptr, nullptr,
	                                             FREERDP_ADDIN_CHANNEL_DYNAMIC),
	    PDVC_PLUGIN_ENTRY);
	TEST_CHECK(pluginEntry);

	test_entry_points ep = WINPR_C_ARRAY_INIT;
	ep.iface.RegisterPlugin = test_register_plugin;
	ep.iface.GetPlugin = test_get_plugin;
	ep.iface.GetPluginData = test_get_plugin_data;
	ep.iface.GetRdpSettings = test_get_rdp_settings;
	ep.iface.GetRdpContext = test_get_rdp_context;
	ep.context = session->context;
	TEST_CHECK(pluginEntry(&ep.iface) == CHANNEL_RC_OK);
	TEST_CHECK(ep.plugin);
	session->plugin = ep.plugin;

	RdpgfxClientContext* gfx = (RdpgfxClientContext*)session->plugin->pInterface;
	TEST_CHECK(gfx);
	gfx->SurfaceCommand = test_surface_command;
	gfx->EndFrame = test_end_frame;

	session->mgr.iface.CreateListener = test_create_listener;
	session->mgr.iface.DestroyListener = test_destroy_listener;
	session->mgr.iface.GetChannelId = test_get_channel_id;
	session->mgr.iface.GetChannelName = test_get_channel_name;
	session->mgr.iface.FindChannelById = test_find_channel_by_id;
	session->channel.Write = test_channel_write;
	session->channel.Close = test_channel_close;

	TEST_CHECK(session->plugin->Initialize(session->plugin, &session->mgr.iface) == CHANNEL_RC_OK);
	TEST_CHECK(session->mgr.listenerCallback);

	BOOL accept = TRUE;
	TEST_CHECK(session->mgr.listenerCallback->OnNewChannelConnection(
	               session->mgr.listenerCallback, &session->channel, nullptr, &accept,
	               &session->callback) == CHANNEL_RC_OK);
	TEST_CHECK(session->callback);
	TEST_CHECK(session->callback->OnOpen(session->callback) == CHANNEL_RC_OK);

	TEST_CHECK(test_caps_confirm(s, version));
	TEST_CHECK(test_send(session, s) == CHANNEL_RC_OK);
	TEST_CHECK(test_reset_graphics(s));
	TEST_CHECK(test_send(session, s) == CHANNEL_RC_OK);
	TEST_CHECK(test_create_surface(s));
	TEST_CHECK(test_send(session, s) == CHANNEL_RC_OK);
	TEST_CHECK(test_frame(s, RDPGFX_CMDID_STARTFRAME, 1));
	TEST_CHECK(test_send(session, s) == CHANNEL_RC_OK);
	return TRUE;
}

/* A surface command the client cannot decode costs that command only: in one message an
 * unknown codec, a good command, one the decoder refuses, a good one and the end of the frame.
 * The channel goes on after each of them, the frame ends and is acknowledged, and the next
 * frame works. */
static BOOL test_undecodable_commands(wStream* s)
{
	test_session session = WINPR_C_ARRAY_INIT;
	BOOL rc = FALSE;
	if (!test_open(&session, RDPGFX_CAPVERSION_107, s))
		goto out;

	if (!test_wire_to_surface_1(s, TEST_CODECID_UNKNOWN, FALSE) ||
	    !test_wire_to_surface_1(s, RDPGFX_CODECID_UNCOMPRESSED, FALSE) ||
	    !test_wire_to_surface_1(s, RDPGFX_CODECID_PLANAR, FALSE) ||
	    !test_wire_to_surface_1(s, RDPGFX_CODECID_UNCOMPRESSED, FALSE) ||
	    !test_frame(s, RDPGFX_CMDID_ENDFRAME, 1))
		goto out;
	{
		const UINT error = test_send(&session, s);
		if (error != CHANNEL_RC_OK)
		{
			(void)fprintf(stderr, "%s: the message with an unknown codec failed: %" PRIu32 "\n",
			              __func__, error);
			goto out;
		}
	}

	/* the unknown codec never reaches the client, the refused one does and costs nothing more */
	if ((seen.commands != 3) || (seen.codecIds[0] != RDPGFX_CODECID_UNCOMPRESSED) ||
	    (seen.codecIds[1] != RDPGFX_CODECID_PLANAR) ||
	    (seen.codecIds[2] != RDPGFX_CODECID_UNCOMPRESSED))
	{
		(void)fprintf(stderr, "%s: %" PRIu32 " surface commands reached the client, not 3\n",
		              __func__, seen.commands);
		goto out;
	}
	if ((seen.endFrames != 1) || (seen.lastEndFrame != 1) || (seen.acks != 1) ||
	    (seen.lastAckFrame != 1))
	{
		(void)fprintf(stderr, "%s: frame 1: %" PRIu32 " end(s), %" PRIu32 " ack(s)\n", __func__,
		              seen.endFrames, seen.acks);
		goto out;
	}

	if (!test_frame(s, RDPGFX_CMDID_STARTFRAME, 2) ||
	    !test_wire_to_surface_1(s, RDPGFX_CODECID_UNCOMPRESSED, FALSE) ||
	    !test_frame(s, RDPGFX_CMDID_ENDFRAME, 2) || (test_send(&session, s) != CHANNEL_RC_OK))
		goto out;
	if ((seen.commands != 4) || (seen.endFrames != 2) || (seen.acks != 2) ||
	    (seen.lastAckFrame != 2))
	{
		(void)fprintf(stderr, "%s: frame 2 did not end as it should\n", __func__);
		goto out;
	}
	rc = TRUE;
out:
	test_close(&session);
	return rc;
}

/* AV2's codec ID before the merged wire form (0x0002) is read as RDPGFX_CODECID_AV1, the
 * merged form, under RDPGFX_CAPVERSION_FRDP_2, and only there */
static BOOL test_av2_pre_merge_codec_id(wStream* s, UINT32 version, BOOL mapped)
{
	test_session session = WINPR_C_ARRAY_INIT;
	BOOL rc = FALSE;
	if (!test_open(&session, version, s))
		goto out;

	if (!test_wire_to_surface_1(s, TEST_CODECID_AV2_PRE_MERGE, TRUE) ||
	    !test_frame(s, RDPGFX_CMDID_ENDFRAME, 1))
		goto out;
	{
		const UINT error = test_send(&session, s);
		if (error != CHANNEL_RC_OK)
		{
			(void)fprintf(stderr, "%s: caps 0x%08" PRIX32 ": codec ID 0x0002 failed: %" PRIu32 "\n",
			              __func__, version, error);
			goto out;
		}
	}

	if (mapped)
	{
#if defined(WITH_GFX_AV2)
		if ((seen.commands != 1) || (seen.codecIds[0] != RDPGFX_CODECID_AV1))
		{
			(void)fprintf(stderr,
			              "%s: caps 0x%08" PRIX32 ": codec ID 0x0002 reached the client %" PRIu32
			              " time(s), not once as RDPGFX_CODECID_AV1\n",
			              __func__, version, seen.commands);
			goto out;
		}
#endif
	}
	else if (seen.commands != 0)
	{
		(void)fprintf(stderr, "%s: caps 0x%08" PRIX32 ": codec ID 0x0002 reached the client\n",
		              __func__, version);
		goto out;
	}
	if ((seen.endFrames != 1) || (seen.acks != 1) || (seen.lastAckFrame != 1))
	{
		(void)fprintf(stderr, "%s: caps 0x%08" PRIX32 ": the frame did not end\n", __func__,
		              version);
		goto out;
	}
	rc = TRUE;
out:
	test_close(&session);
	return rc;
}

int TestRdpgfxSurfaceCommands(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	int rc = -1;
	wStream* s = Stream_New(nullptr, 1024);
	if (!s)
		return -1;

	if (!test_undecodable_commands(s))
		goto out;
#if defined(WITH_GFX_AV2)
	if (!test_av2_pre_merge_codec_id(s, RDPGFX_CAPVERSION_FRDP_2, TRUE))
		goto out;
#endif
	if (!test_av2_pre_merge_codec_id(s, RDPGFX_CAPVERSION_107, FALSE))
		goto out;
	rc = 0;
out:
	Stream_Free(s, TRUE);
	return rc;
}
