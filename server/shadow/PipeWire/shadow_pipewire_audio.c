/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * The sound of a Linux session through PipeWire
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

/* Both directions go through the session's PipeWire daemon, so neither needs a sound card:
 *
 *   applications -> default sink -> its monitor -> capture stream -> rdpsnd -> clients
 *   client's microphone -> audin -> ring -> virtual source -> applications
 *
 * On a machine without one, WirePlumber's Dummy Output is the default sink. The virtual
 * source is the node "freerdp-microphone": to the session's applications an ordinary
 * microphone, there only while clients are connected and one of them sends samples.
 *
 * A thread loop of its own keeps this independent of a subsystem's main loop. Its callbacks
 * hold the loop's lock while the capture posts to the clients, and subsystems tell about
 * clients with the server's client list locked, so calls from other threads never take that
 * lock: they go through pw_loop_invoke(). */

#include <math.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <winpr/assert.h>
#include <winpr/cast.h>
#include <winpr/crt.h>
#include <winpr/interlocked.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include <freerdp/codec/audio.h>
#include <freerdp/codec/dsp.h>
#include <freerdp/log.h>

#include "shadow_pipewire_audio.h"

#define TAG SERVER_TAG("shadow.pipewire")

/* What the clients hear: 48 kHz, 2 channels, 16 bits; PipeWire converts what
 * the sink plays to it. The build may have no resampler, so every format
 * offered has that rate and channel count, and 16 bits a sample: rdpsnd sizes
 * its buffers from the bits and channels of the first one. Opus first, so that
 * a client that has it takes it (96 kbit/s instead of 1.5 Mbit/s for PCM). Its
 * block alignment is 1: rdpsnd pads every wave to a multiple of it with zeros,
 * and an Opus packet with zeros after it no longer decodes. */
#define SOUND_RATE 48000
#define SOUND_CHANNELS 2
#define SOUND_FRAME_BYTES (SOUND_CHANNELS * 2)
/* samples are sent in messages of 20 ms */
#define SOUND_MESSAGE_FRAMES (SOUND_RATE / 50)
#define SOUND_LOG_MS 5000

/* What the session's applications hear from a client's microphone */
#define MIC_FRAME_BYTES 2
/* half a second: more than that behind is no conversation any more */
#define MIC_RING_FRAMES (SOUND_RATE / 2)
/* what must be there before the source starts playing, against the network's jitter */
#define MIC_START_FRAMES (SOUND_RATE / 25)
/* another client's microphone may take over after this much silence */
#define MIC_TAKEOVER_MS 1000

static const AUDIO_FORMAT sound_offered_formats[] = {
	{ WAVE_FORMAT_OPUS, SOUND_CHANNELS, SOUND_RATE, 12000, 1, 16, 0, nullptr },
	{ WAVE_FORMAT_PCM, SOUND_CHANNELS, SOUND_RATE, (SOUND_RATE * SOUND_FRAME_BYTES),
	  SOUND_FRAME_BYTES, 16, 0, nullptr },
};

static const AUDIO_FORMAT mic_offered_formats[] = {
	{ WAVE_FORMAT_PCM, 1, SOUND_RATE, SOUND_RATE * 2, 2, 16, 0, nullptr },
	{ WAVE_FORMAT_PCM, 2, SOUND_RATE, SOUND_RATE * 4, 4, 16, 0, nullptr },
};

struct s_shadow_pipewire_audio
{
	rdpShadowSubsystem* subsystem;

	struct pw_thread_loop* loop;
	struct pw_context* context;
	struct pw_core* core;
	struct spa_hook coreListener;

	/* sound: a capture of the default sink's monitor */
	struct pw_stream* capture;
	struct spa_hook captureListener;
	wStream* pending;
	/* the format of the samples in each message: it outlives every client, since
	 * rdpsnd keeps the pointer as its source format */
	AUDIO_FORMAT sourceFormat;
	UINT64 frames;
	UINT64 messages;
	float peak;
	UINT64 logged;

	volatile LONG clients;

	/* microphone: mono samples between the audin thread and the source */
	struct pw_stream* mic;
	struct spa_hook micListener;
	CRITICAL_SECTION micLock;
	BOOL micLockReady;
	INT16* ring;
	size_t ringRead;
	size_t ringFill;
	BOOL micPlaying;
	const rdpShadowClient* micClient;
	UINT64 micSeen;
	UINT64 micDropped;
};

static void sound_msg_free(WINPR_ATTR_UNUSED UINT32 id, SHADOW_MSG_OUT* msg)
{
	SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES* samples = (SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES*)msg;
	if (samples)
		free(samples->buf);
	free(samples);
}

/* Sends what is pending to every client, as one message */
static void sound_post(ShadowPipewireAudio* audio)
{
	WINPR_ASSERT(audio);

	wStream* s = audio->pending;
	const size_t size = Stream_GetPosition(s);
	Stream_ResetPosition(s);
	if (size == 0)
		return;

	rdpShadowServer* server = audio->subsystem->server;
	if (!server || !server->clients || (ArrayList_Count(server->clients) == 0))
		return;

	SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES* msg = calloc(1, sizeof(SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES));
	if (!msg)
		return;

	msg->buf = malloc(size);
	if (!msg->buf)
	{
		free(msg);
		return;
	}

	memcpy(msg->buf, Stream_Buffer(s), size);
	audio->messages++;
	msg->common.Free = sound_msg_free;
	msg->audio_format = &audio->sourceFormat;
	msg->nFrames = WINPR_ASSERTING_INT_CAST(UINT32, size / SOUND_FRAME_BYTES);
	msg->wTimestamp = (UINT16)(GetTickCount64() & 0xFFFF);
	/* the message is freed with its last reference, also when no client took it */
	if (shadow_client_boardcast_msg(server, nullptr, SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES_ID,
	                                &msg->common, nullptr) < 0)
		WLog_DBG(TAG, "sound: a message was not sent");
}

static void sound_stats(ShadowPipewireAudio* audio, size_t frames)
{
	WINPR_ASSERT(audio);

	const UINT64 now = GetTickCount64();
	if (audio->logged == 0)
	{
		WLog_INFO(TAG, "sound: capturing what the default sink plays");
		audio->logged = now;
	}
	else if (now - audio->logged >= SOUND_LOG_MS)
	{
		WLog_DBG(TAG,
		         "sound: %" PRIu64 " frames captured, %" PRIu64
		         " messages sent, peak %.3f in the last %" PRIu64 " ms",
		         audio->frames, audio->messages, (double)audio->peak, now - audio->logged);
		audio->frames = 0;
		audio->messages = 0;
		audio->peak = 0.0f;
		audio->logged = now;
	}
	audio->frames += frames;
}

static void sound_on_process(void* data)
{
	ShadowPipewireAudio* audio = data;
	WINPR_ASSERT(audio);

	struct pw_buffer* pb = nullptr;

	while ((pb = pw_stream_dequeue_buffer(audio->capture)) != nullptr)
	{
		const struct spa_data* d = &pb->buffer->datas[0]; // codespell:ignore datas
		if (d->data && d->chunk && (d->chunk->size > 0) &&
		    ((size_t)d->chunk->offset + d->chunk->size <= d->maxsize))
		{
			const INT16* samples = (const INT16*)((const BYTE*)d->data + d->chunk->offset);
			const size_t frames = d->chunk->size / SOUND_FRAME_BYTES;
			sound_stats(audio, frames);

			wStream* s = audio->pending;
			for (size_t f = 0; f < frames; f++)
			{
				for (size_t c = 0; c < SOUND_CHANNELS; c++)
				{
					const INT16 value = samples[f * SOUND_CHANNELS + c];
					audio->peak = MAX(audio->peak, fabsf((float)value / (float)INT16_MAX));
					Stream_Write_INT16(s, value);
				}

				if (Stream_GetPosition(s) >= SOUND_MESSAGE_FRAMES * SOUND_FRAME_BYTES)
					sound_post(audio);
			}
		}
		(void)pw_stream_queue_buffer(audio->capture, pb);
	}
}

static const struct pw_stream_events sound_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = sound_on_process,
};

WINPR_ATTR_NODISCARD
static const struct spa_pod* audio_format_pod(struct spa_pod_builder* b, uint32_t channels)
{
	struct spa_audio_info_raw info = WINPR_C_ARRAY_INIT;
	info.format = SPA_AUDIO_FORMAT_S16_LE;
	info.rate = SOUND_RATE;
	info.channels = channels;
	if (channels == 1)
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	else
	{
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
	}
	return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

/* loop thread: a capture of the default sink's monitor, for as long as a client is connected.
 * A stream made inactive and started later never gets its links going if the sink already
 * plays, so the first client would hear nothing of what was playing when it connected. */
WINPR_ATTR_NODISCARD
static BOOL sound_create_capture(ShadowPipewireAudio* audio)
{
	WINPR_ASSERT(audio);

	uint8_t podBuffer[1024] = WINPR_C_ARRAY_INIT;
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

	audio->capture =
	    pw_stream_new(audio->core, "FreeRDP shadow server sound",
	                  pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
	                                    "Capture", PW_KEY_MEDIA_ROLE, "Screen",
	                                    PW_KEY_STREAM_CAPTURE_SINK, "true", nullptr));
	if (!audio->capture)
		return FALSE;
	pw_stream_add_listener(audio->capture, &audio->captureListener, &sound_events, audio);

	const struct spa_pod* params[] = { audio_format_pod(&b, SOUND_CHANNELS) };

	/* PipeWire links it to the default sink's monitor and converts to our format */
	if (pw_stream_connect(audio->capture, PW_DIRECTION_INPUT, PW_ID_ANY,
	                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params,
	                      ARRAYSIZE(params)) < 0)
	{
		pw_stream_destroy(audio->capture);
		audio->capture = nullptr;
		return FALSE;
	}
	return TRUE;
}

/* The source plays what the ring holds, silence when it runs dry */
static void mic_on_process(void* data)
{
	ShadowPipewireAudio* audio = data;
	WINPR_ASSERT(audio);

	struct pw_buffer* pb = pw_stream_dequeue_buffer(audio->mic);
	if (!pb)
		return;

	struct spa_data* d = &pb->buffer->datas[0]; // codespell:ignore datas
	if (d->data && d->chunk)
	{
		size_t frames = d->maxsize / MIC_FRAME_BYTES;
		if (pb->requested > 0)
			frames = MIN(frames, (size_t)pb->requested);

		INT16* out = d->data;
		size_t copied = 0;

		EnterCriticalSection(&audio->micLock);
		if (!audio->micPlaying && (audio->ringFill >= MIC_START_FRAMES))
			audio->micPlaying = TRUE;
		if (audio->micPlaying)
		{
			copied = MIN(frames, audio->ringFill);
			for (size_t x = 0; x < copied; x++)
			{
				out[x] = audio->ring[audio->ringRead];
				audio->ringRead = (audio->ringRead + 1) % MIC_RING_FRAMES;
			}
			audio->ringFill -= copied;
			if (audio->ringFill == 0)
				audio->micPlaying = FALSE;
		}
		LeaveCriticalSection(&audio->micLock);

		memset(&out[copied], 0, (frames - copied) * MIC_FRAME_BYTES);
		d->chunk->offset = 0;
		d->chunk->stride = MIC_FRAME_BYTES;
		d->chunk->size = WINPR_ASSERTING_INT_CAST(uint32_t, frames* MIC_FRAME_BYTES);
	}
	(void)pw_stream_queue_buffer(audio->mic, pb);
}

static const struct pw_stream_events mic_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = mic_on_process,
};

/* loop thread */
static void mic_create(ShadowPipewireAudio* audio)
{
	WINPR_ASSERT(audio);

	if (audio->mic || !audio->core)
		return;

	uint8_t podBuffer[1024] = WINPR_C_ARRAY_INIT;
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

	audio->mic =
	    pw_stream_new(audio->core, "FreeRDP microphone",
	                  pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Source",
	                                    PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_NODE_NAME,
	                                    "freerdp-microphone", PW_KEY_NODE_DESCRIPTION,
	                                    "Microphone of the remote client", nullptr));
	if (!audio->mic)
		return;
	pw_stream_add_listener(audio->mic, &audio->micListener, &mic_events, audio);

	const struct spa_pod* params[] = { audio_format_pod(&b, 1) };
	if (pw_stream_connect(audio->mic, PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS,
	                      params, ARRAYSIZE(params)) < 0)
	{
		WLog_WARN(TAG, "microphone: cannot create the source");
		pw_stream_destroy(audio->mic);
		audio->mic = nullptr;
		return;
	}
	WLog_INFO(TAG, "microphone: the session has a source now");
}

/* loop thread */
static void mic_destroy(ShadowPipewireAudio* audio)
{
	WINPR_ASSERT(audio);

	if (!audio->mic)
		return;

	pw_stream_destroy(audio->mic);
	audio->mic = nullptr;

	EnterCriticalSection(&audio->micLock);
	audio->ringRead = 0;
	audio->ringFill = 0;
	audio->micPlaying = FALSE;
	audio->micClient = nullptr;
	LeaveCriticalSection(&audio->micLock);
	WLog_INFO(TAG, "microphone: the source is gone");
}

/* loop thread */
static int follow_clients(WINPR_ATTR_UNUSED struct spa_loop* loop, WINPR_ATTR_UNUSED bool async,
                          WINPR_ATTR_UNUSED uint32_t seq, WINPR_ATTR_UNUSED const void* data,
                          WINPR_ATTR_UNUSED size_t size, void* userData)
{
	ShadowPipewireAudio* audio = userData;
	WINPR_ASSERT(audio);

	const BOOL active = InterlockedCompareExchange(&audio->clients, 0, 0) > 0;
	if (!active)
		mic_destroy(audio);

	if (active == (audio->capture != nullptr))
		return 0;

	if (active)
	{
		if (!sound_create_capture(audio))
		{
			WLog_WARN(TAG, "sound: cannot start the capture");
			return 0;
		}
	}
	else
	{
		pw_stream_destroy(audio->capture);
		audio->capture = nullptr;
		Stream_ResetPosition(audio->pending);
	}
	WLog_DBG(TAG, "sound: capture %s", active ? "started" : "stopped");
	return 0;
}

/* loop thread */
static int follow_microphone(WINPR_ATTR_UNUSED struct spa_loop* loop, WINPR_ATTR_UNUSED bool async,
                             WINPR_ATTR_UNUSED uint32_t seq, WINPR_ATTR_UNUSED const void* data,
                             WINPR_ATTR_UNUSED size_t size, void* userData)
{
	ShadowPipewireAudio* audio = userData;
	WINPR_ASSERT(audio);

	if (InterlockedCompareExchange(&audio->clients, 0, 0) > 0)
		mic_create(audio);
	return 0;
}

void shadow_pipewire_audio_clients(ShadowPipewireAudio* audio, size_t count)
{
	if (!audio)
		return;

	const LONG before = InterlockedExchange(&audio->clients, WINPR_ASSERTING_INT_CAST(LONG, count));
	WINPR_UNUSED(before);
	const int rc = pw_loop_invoke(pw_thread_loop_get_loop(audio->loop), follow_clients, 0, nullptr,
	                              0, false, audio);
	if (rc < 0)
		WLog_WARN(TAG, "sound: cannot follow the clients: %s", strerror(-rc));
}

BOOL shadow_pipewire_audio_microphone(ShadowPipewireAudio* audio, rdpShadowClient* client,
                                      const AUDIO_FORMAT* format, wStream* samples)
{
	WINPR_ASSERT(client);
	WINPR_ASSERT(format);
	WINPR_ASSERT(samples);

	if (!audio)
		return TRUE;

	if ((format->wFormatTag != WAVE_FORMAT_PCM) || (format->wBitsPerSample != 16) ||
	    (format->nSamplesPerSec != SOUND_RATE) || (format->nChannels < 1) ||
	    (format->nChannels > 2))
		return TRUE; /* we offered none of that: nothing to play */

	const size_t channels = format->nChannels;
	const size_t frames = Stream_GetRemainingLength(samples) / (channels * 2);
	const BYTE* data = Stream_ConstPointer(samples);
	const UINT64 now = GetTickCount64();

	BOOL first = FALSE;
	EnterCriticalSection(&audio->micLock);
	if (audio->micClient && (audio->micClient != client) &&
	    (now - audio->micSeen < MIC_TAKEOVER_MS))
	{
		LeaveCriticalSection(&audio->micLock); /* one speaker at a time, no mixing */
		return TRUE;
	}
	first = !audio->micClient;
	audio->micClient = client;
	audio->micSeen = now;

	for (size_t f = 0; f < frames; f++)
	{
		INT16 left = 0;
		memcpy(&left, &data[f * channels * 2], sizeof(left));
		INT32 value = left;
		if (channels == 2)
		{
			INT16 right = 0;
			memcpy(&right, &data[f * channels * 2 + 2], sizeof(right));
			value = (value + right) / 2;
		}

		if (audio->ringFill == MIC_RING_FRAMES) /* the oldest sample goes */
		{
			audio->ringRead = (audio->ringRead + 1) % MIC_RING_FRAMES;
			audio->ringFill--;
			audio->micDropped++;
		}
		audio->ring[(audio->ringRead + audio->ringFill) % MIC_RING_FRAMES] =
		    WINPR_ASSERTING_INT_CAST(INT16, value);
		audio->ringFill++;
	}
	LeaveCriticalSection(&audio->micLock);

	if (first)
	{
		const int rc = pw_loop_invoke(pw_thread_loop_get_loop(audio->loop), follow_microphone, 0,
		                              nullptr, 0, false, audio);
		if (rc < 0)
			WLog_WARN(TAG, "microphone: cannot create the source: %s", strerror(-rc));
	}
	return TRUE;
}

static void audio_on_core_error(WINPR_ATTR_UNUSED void* data, uint32_t id,
                                WINPR_ATTR_UNUSED int seq, WINPR_ATTR_UNUSED int res,
                                const char* message)
{
	WLog_WARN(TAG, "PipeWire error on %" PRIu32 ": %s", id, message);
}

static const struct pw_core_events audio_core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = audio_on_core_error,
};

WINPR_ATTR_NODISCARD
static BOOL audio_set_formats(ShadowPipewireAudio* audio)
{
	WINPR_ASSERT(audio);

	rdpShadowSubsystem* subsystem = audio->subsystem;

	AUDIO_FORMAT* sound = audio_formats_new(ARRAYSIZE(sound_offered_formats));
	AUDIO_FORMAT* mic = audio_formats_new(ARRAYSIZE(mic_offered_formats));
	if (!sound || !mic)
	{
		audio_formats_free(sound, ARRAYSIZE(sound_offered_formats));
		audio_formats_free(mic, ARRAYSIZE(mic_offered_formats));
		return FALSE;
	}

	size_t count = 0;
	for (size_t x = 0; x < ARRAYSIZE(sound_offered_formats); x++)
	{
		const AUDIO_FORMAT* format = &sound_offered_formats[x];
		if (freerdp_dsp_supports_format(format, TRUE))
			sound[count++] = *format;
	}
	for (size_t x = 0; x < ARRAYSIZE(mic_offered_formats); x++)
		mic[x] = mic_offered_formats[x];

	subsystem->rdpsndFormats = sound;
	subsystem->nRdpsndFormats = count;
	subsystem->audinFormats = mic;
	subsystem->nAudinFormats = ARRAYSIZE(mic_offered_formats);
	return count > 0;
}

ShadowPipewireAudio* shadow_pipewire_audio_new(rdpShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	pw_init(nullptr, nullptr);

	ShadowPipewireAudio* audio = calloc(1, sizeof(ShadowPipewireAudio));
	if (!audio)
		return nullptr;

	audio->subsystem = subsystem;
	audio->sourceFormat.wFormatTag = WAVE_FORMAT_PCM;
	audio->sourceFormat.nChannels = SOUND_CHANNELS;
	audio->sourceFormat.nSamplesPerSec = SOUND_RATE;
	audio->sourceFormat.nAvgBytesPerSec = SOUND_RATE * SOUND_FRAME_BYTES;
	audio->sourceFormat.nBlockAlign = SOUND_FRAME_BYTES;
	audio->sourceFormat.wBitsPerSample = 16;

	if (!InitializeCriticalSectionAndSpinCount(&audio->micLock, 400))
		goto fail;
	audio->micLockReady = TRUE;

	audio->pending = Stream_New(nullptr, SOUND_MESSAGE_FRAMES * SOUND_FRAME_BYTES);
	audio->ring = calloc(MIC_RING_FRAMES, sizeof(INT16));
	audio->loop = pw_thread_loop_new("freerdp-sound", nullptr);
	if (!audio->pending || !audio->ring || !audio->loop)
		goto fail;

	audio->context = pw_context_new(pw_thread_loop_get_loop(audio->loop), nullptr, 0);
	if (!audio->context)
		goto fail;

	if (pw_thread_loop_start(audio->loop) < 0)
		goto fail;

	/* the session's PipeWire daemon, through XDG_RUNTIME_DIR */
	pw_thread_loop_lock(audio->loop);
	audio->core = pw_context_connect(audio->context, nullptr, 0);
	BOOL ready = FALSE;
	if (audio->core)
	{
		pw_core_add_listener(audio->core, &audio->coreListener, &audio_core_events, audio);
		ready = TRUE;
	}
	pw_thread_loop_unlock(audio->loop);

	if (!ready || !audio_set_formats(audio))
		goto fail;
	return audio;

fail:
	WLog_INFO(TAG, "no PipeWire: no sound");
	shadow_pipewire_audio_free(audio);
	return nullptr;
}

void shadow_pipewire_audio_free(ShadowPipewireAudio* audio)
{
	if (!audio)
		return;

	if (audio->loop)
		pw_thread_loop_stop(audio->loop);

	if (audio->mic)
		pw_stream_destroy(audio->mic);
	if (audio->capture)
		pw_stream_destroy(audio->capture);
	if (audio->core)
		pw_core_disconnect(audio->core);
	if (audio->context)
		pw_context_destroy(audio->context);
	if (audio->loop)
		pw_thread_loop_destroy(audio->loop);

	rdpShadowSubsystem* subsystem = audio->subsystem;
	audio_formats_free(subsystem->rdpsndFormats, subsystem->nRdpsndFormats);
	subsystem->rdpsndFormats = nullptr;
	subsystem->nRdpsndFormats = 0;
	audio_formats_free(subsystem->audinFormats, subsystem->nAudinFormats);
	subsystem->audinFormats = nullptr;
	subsystem->nAudinFormats = 0;

	free(audio->ring);
	Stream_Free(audio->pending, TRUE);
	if (audio->micLockReady)
		DeleteCriticalSection(&audio->micLock);
	free(audio);
}
