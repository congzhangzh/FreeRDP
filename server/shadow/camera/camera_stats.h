/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: what each client camera did, for the stats socket
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_STATS_H
#define FREERDP_SERVER_SHADOW_CAMERA_STATS_H

#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** the bytes of a device name a line carries, cut on a UTF-8 boundary */
#define SHADOW_CAMERA_STATS_NAME_MAX 64u

/** the refused cameras a client keeps, the newest ones */
#define SHADOW_CAMERA_STATS_REFUSALS 4u

	/** where a client camera is, as the stats line names it */
	typedef enum
	{
		SHADOW_CAMERA_STATS_NEGOTIATING, /**< activation, stream list, media types */
		SHADOW_CAMERA_STATS_IDLE,        /**< media type chosen, no stream (no app reads) */
		SHADOW_CAMERA_STATS_STARTING,
		SHADOW_CAMERA_STATS_STREAMING,
		SHADOW_CAMERA_STATS_STOPPING,
		SHADOW_CAMERA_STATS_FAILED,
		SHADOW_CAMERA_STATS_REFUSED
	} SHADOW_CAMERA_STATS_STATE;

	/** One device's counters: written by its thread, read by the client's (the caller locks).
	 *  A period runs from one shadow_camera_stats_take_period() to the next. */
	typedef struct
	{
		UINT64 periodStartMs;
		UINT64 samples;
		UINT64 bytes;
		UINT64 frames;
		UINT64 dropped;
		UINT64 sampleErrors;
		UINT64 decodes;
		UINT64 decodeSumNs;
		UINT64 decodeMaxNs;
		UINT64 gapMaxMs;
		UINT32 outstandingMax;
		/* across periods */
		UINT64 lastSampleMs; /**< 0: none since the stream started */
		UINT64 framesTotal;
	} ShadowCameraStats;

	/** a period, as the line carries it. Plain names count this period, *Total since the
	 *  device joined, *Now the moment the period was taken. */
	typedef struct
	{
		UINT64 periodMs;
		UINT64 samples;      /**< Sample Responses taken */
		UINT64 bytes;        /**< their sample bytes */
		UINT64 kbitPerS;     /**< bytes over the period, rounded */
		UINT64 frames;       /**< pictures written to the sink */
		UINT64 dropped;      /**< samples that showed nothing: undecodable, not written */
		UINT64 sampleErrors; /**< Sample Error Responses */
		UINT64 decodes;      /**< samples decoded (the times below are theirs) */
		UINT64 decodeAvgUs;
		UINT64 decodeMaxUs;
		UINT64 gapMaxMs; /**< the longest time without a sample while streaming */
		UINT32 outstandingMax;
		UINT64 framesTotal;
	} ShadowCameraStatsValues;

	/** What the line says of a device besides its counters */
	typedef struct
	{
		const char* name; /**< the client's, UTF-8; escaped and cut */
		SHADOW_CAMERA_STATS_STATE state;
		const char* reason; /**< refused or failed: why (ours, never the client's) */
		BOOL owner;         /**< it owns the virtual camera */
		const char* format; /**< nullptr until a media type is chosen */
		UINT32 width;
		UINT32 height;
		UINT32 fpsNumerator;
		UINT32 fpsDenominator;
		UINT32 startsTotal;
		UINT32 outstandingNow;
		UINT32 outstandingTargetNow;
	} ShadowCameraDeviceInfo;

	/** The newest refused cameras of a client, oldest first */
	typedef struct
	{
		char names[SHADOW_CAMERA_STATS_REFUSALS][SHADOW_CAMERA_STATS_NAME_MAX + 1];
		const char* reasons[SHADOW_CAMERA_STATS_REFUSALS];
		size_t count; /**< kept, at most SHADOW_CAMERA_STATS_REFUSALS */
		size_t next;  /**< the slot the next one takes */
	} ShadowCameraRefusals;

	/** a new device: its first period starts now */
	void shadow_camera_stats_init(ShadowCameraStats* stats, UINT64 nowMs);

	/** a stream starts: the gap counts from its first sample */
	void shadow_camera_stats_stream_started(ShadowCameraStats* stats);

	/** a Sample Response of bytes arrived */
	void shadow_camera_stats_sample(ShadowCameraStats* stats, size_t bytes, UINT64 nowMs);

	/** a sample was decoded in ns (whether or not it gave a picture) */
	void shadow_camera_stats_decoded(ShadowCameraStats* stats, UINT64 ns);

	/** a picture was written to the sink */
	void shadow_camera_stats_shown(ShadowCameraStats* stats);

	/** a sample showed nothing (it did not decode, or its picture was not written) */
	void shadow_camera_stats_dropped(ShadowCameraStats* stats);

	void shadow_camera_stats_sample_error(ShadowCameraStats* stats);

	/** the sample requests in flight after a refill */
	void shadow_camera_stats_outstanding(ShadowCameraStats* stats, UINT32 outstanding);

	/** Ends the period: its values, and the next one starts at nowMs.
	 *  @param streaming the stream runs now: a gap still open counts */
	void shadow_camera_stats_take_period(ShadowCameraStats* stats, UINT64 nowMs, BOOL streaming,
	                                     ShadowCameraStatsValues* values);

	/** @return the bytes of name a line carries: at most SHADOW_CAMERA_STATS_NAME_MAX, never
	 *  inside a UTF-8 sequence */
	WINPR_ATTR_NODISCARD size_t shadow_camera_stats_name_length(const char* name);

	/** keeps a refused camera, the oldest one going when full */
	void shadow_camera_refusals_add(ShadowCameraRefusals* refusals, const char* name,
	                                const char* reason);

	WINPR_ATTR_NODISCARD const char*
	shadow_camera_stats_state_name(SHADOW_CAMERA_STATS_STATE state);

	/** Appends a JSON string of text (its first length bytes) at *at: quotes, \ and control
	 *  characters escaped. nullptr text is null.
	 *  @return FALSE when it does not fit (*at is then unchanged) */
	WINPR_ATTR_NODISCARD BOOL shadow_camera_stats_json_string(char* buffer, size_t size, size_t* at,
	                                                          const char* text, size_t length);

	/** Appends one device object at *at; values nullptr (a refused camera): no counters.
	 *  @return FALSE when it does not fit (*at is then unchanged) */
	WINPR_ATTR_NODISCARD BOOL shadow_camera_stats_json_device(
	    char* buffer, size_t size, size_t* at, const ShadowCameraDeviceInfo* info,
	    const ShadowCameraStatsValues* values);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_STATS_H */
