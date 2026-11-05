/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Protocol Metrics
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#ifndef FREERDP_METRICS_H
#define FREERDP_METRICS_H

#include <winpr/wtypes.h>

#include <freerdp/api.h>
#include <freerdp/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

	struct rdp_metrics
	{
		rdpContext* context;

		UINT64 TotalCompressedBytes;
		UINT64 TotalUncompressedBytes;
		double TotalCompressionRatio;

		/* What the rdpsnd client channel saw over the life of the context. Here, as a channel
		 * comes and goes with a connection and a reader must not depend on it. */
		UINT64 AudioWaves;            /**< waves received @since version 3.34.0 */
		UINT64 AudioWaveBytes;        /**< their encoded bytes @since version 3.34.0 */
		UINT64 AudioWavesOverrun;     /**< not played, too much queued @since version 3.34.0 */
		UINT64 AudioWavesUndecodable; /**< not played, no decode @since version 3.34.0 */
		UINT64 AudioDeviceOpens;      /**< a format change opens it @since version 3.34.0 */
		UINT64 AudioFormatTag;        /**< WAVE_FORMAT_* of the last open @since version 3.34.0 */
		UINT64 AudioDelayMs; /**< last delay told to the server, arrival to the end of the play
		                        call plus what the backend reports @since version 3.34.0 */
	};
	typedef struct rdp_metrics rdpMetrics;

	WINPR_ATTR_NODISCARD
	FREERDP_API double metrics_write_bytes(rdpMetrics* metrics, UINT32 UncompressedBytes,
	                                       UINT32 CompressedBytes);

	FREERDP_API void metrics_free(rdpMetrics* metrics);

	WINPR_ATTR_MALLOC(metrics_free, 1)
	WINPR_ATTR_NODISCARD
	FREERDP_API rdpMetrics* metrics_new(rdpContext* context);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_METRICS_H */
