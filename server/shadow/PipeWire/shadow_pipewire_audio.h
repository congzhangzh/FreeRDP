/**
 * FreeRDP: A Remote Desktop Protocol Implementation
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

#ifndef FREERDP_SERVER_SHADOW_PIPEWIRE_AUDIO_H
#define FREERDP_SERVER_SHADOW_PIPEWIRE_AUDIO_H

#include <freerdp/server/shadow.h>

typedef struct s_shadow_pipewire_audio ShadowPipewireAudio;

void shadow_pipewire_audio_free(ShadowPipewireAudio* audio);

/** Connects to the PipeWire daemon of the session and sets the subsystem's rdpsnd and audin
 *  formats. The subsystem's AudinServerReceiveSamples hands the samples to
 *  shadow_pipewire_audio_microphone().
 *
 *  @return the object, or nullptr without PipeWire: the session then has no sound, and the
 *          rest works.
 */
WINPR_ATTR_MALLOC(shadow_pipewire_audio_free, 1)
WINPR_ATTR_NODISCARD
ShadowPipewireAudio* shadow_pipewire_audio_new(rdpShadowSubsystem* subsystem);

/** The number of clients changed: the capture runs while there are any, and the microphone
 *  exists only as long. Any thread. */
void shadow_pipewire_audio_clients(ShadowPipewireAudio* audio, size_t count);

/** Samples from a client's microphone. Any thread. */
WINPR_ATTR_NODISCARD
BOOL shadow_pipewire_audio_microphone(ShadowPipewireAudio* audio, rdpShadowClient* client,
                                      const AUDIO_FORMAT* format, wStream* samples);

#endif /* FREERDP_SERVER_SHADOW_PIPEWIRE_AUDIO_H */
