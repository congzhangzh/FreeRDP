/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, browser camera backend for Emscripten builds
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

/* JavaScript side of camera_emscripten.c, linked with --js-library. Every function runs on the
 * main browser thread (proxied there from the channel's threads). The camera goes out as H.264:
 * each frame is encoded by a WebCodecs VideoEncoder (Constrained Baseline, real-time, the bit
 * rate camera_emscripten.c chose), a key frame first and every 10 s. Chunks come in the
 * "avc" format (NAL units after their lengths, the SPS and PPS in the avcC record of the first
 * output), which camera_emscripten.c turns into Annex B samples.
 *
 * The page lists the cameras in Module.freerdpCameras = [{ deviceId, label }] once the user
 * allowed the camera; without a list the browser's default camera is used. The page sets
 * Module.freerdpCameraH264 to what VideoEncoder.isConfigSupported answered (without it a browser
 * with a VideoEncoder is taken to encode H.264). Module.onFreerdpCameraError(e) is called when
 * the browser cannot open or encode a camera. Module.freerdpCameraStats[index] holds the counters
 * of a running camera for the page's Info and the tests. */

function cam_web_device_count()
{
	const list = Module.freerdpCameras;
	return Array.isArray(list) ? list.length : 0;
}

function cam_web_device_label(index, buffer, size)
{
	const list = Module.freerdpCameras;
	const cam = Array.isArray(list) ? list[index] : null;
	const label = cam && typeof cam.label === "string" ? cam.label : "";
	if (!label)
		return 0;
	return stringToUTF8(label, buffer, size);
}

function cam_web_can_h264()
{
	if (Module.freerdpCameraH264 === true || Module.freerdpCameraH264 === false)
		return Module.freerdpCameraH264 ? 1 : 0;
	return typeof VideoEncoder === "function" ? 1 : 0;
}

function cam_web_start(index, capture, width, height, fpsNum, fpsDen, bitrate, codecPtr)
{
	const codec = UTF8ToString(codecPtr);
	const all = (Module.freerdpCamera = Module.freerdpCamera || {});
	if (all[index])
		all[index].stop();

	/* cam_web_capture in camera_emscripten.c, as 32 bit words */
	const w = capture >>> 2;
	const STATE = w, ACTIVE = w + 1, KIND = w + 2, SIZE = w + 3, CAPACITY = w + 4, FRAMES = w + 5,
	      SKIPPED = w + 6, ENCODE_US = w + 7, KEYS = w + 8, FIRST_US = w + 9, DATA = w + 10;
	const SLOT_FREE = 0, SLOT_FULL = 1, SLOT_FAILED = 2;
	const CHUNK_CONFIG = 0, CHUNK_KEY = 1, CHUNK_DELTA = 2;
	const fps = fpsNum / fpsDen;
	/* a key frame first, then every 10 s: RDP runs over TCP, so nothing is lost mid-stream, and
	 * every recovery (on-demand restart, a new owner, a reconnect) starts the stream again, with a
	 * key frame; more key frames would only cost bit rate */
	const KEY_INTERVAL_S = 10;
	const keyEvery = Math.max(1, Math.round(KEY_INTERVAL_S * fps));
	const t0 = performance.now();

	const stats = (Module.freerdpCameraStats = Module.freerdpCameraStats || {});
	const st = {
		stopped: false,
		stream: null,
		reader: null,
		video: null,
		encoder: null,
		queue: [], /* chunks for the slot: { kind, bytes } */
		pumping: false,
		needKey: true,
		encoded: 0,         /* frames given to the encoder */
		started: new Map(), /* timestamp -> performance.now() at encode() */
		stats: {
			format: "H264",
			codec: codec,
			width: width,
			height: height,
			bitrate: bitrate,
			chunks: 0,
			keys: 0,
			skipped: 0,
			bytes: 0,
			encodeMs: 0,
			firstMs: 0,
			encoder: ""
		}
	};
	stats[index] = st.stats;
	all[index] = st;

	const release = () => _camera_emscripten_release(capture);
	st.stop = () => {
		if (st.stopped)
			return;
		st.stopped = true;
		if (all[index] === st)
			delete all[index];
		if (st.reader)
			st.reader.cancel().catch(() => {});
		if (st.video)
		{
			st.video.pause();
			st.video.srcObject = null;
		}
		if (st.stream)
			st.stream.getTracks().forEach((t) => t.stop());
		if (st.encoder && st.encoder.state !== "closed")
		{
			try
			{
				st.encoder.close();
			}
			catch (e)
			{
			}
		}
		/* nothing writes to the slot after this tick: pump() checks st.stopped first */
		release();
	};

	const fail = (e) => {
		if (st.stopped || st.failed)
			return;
		st.failed = true;
		st.queue = [];
		err("[rdpecam] camera " + index + " failed: " + e);
		Atomics.store(HEAP32, STATE, SLOT_FAILED);
		Atomics.notify(HEAP32, STATE);
		if (st.stream)
			st.stream.getTracks().forEach((t) => t.stop());
		if (typeof Module.onFreerdpCameraError === "function")
			Module.onFreerdpCameraError(e);
	};

	/* hands the queued chunks to the capture thread, one at a time */
	const pump = () => {
		st.pumping = false;
		while (!st.stopped && !st.failed && st.queue.length && Atomics.load(HEAP32, ACTIVE) === 1)
		{
			const state = Atomics.load(HEAP32, STATE);
			if (state === SLOT_FAILED)
				return;
			if (state !== SLOT_FREE)
			{
				st.pumping = true;
				if (typeof Atomics.waitAsync === "function")
				{
					const r = Atomics.waitAsync(HEAP32, STATE, SLOT_FULL);
					if (r.async)
						r.value.then(pump);
					else
						queueMicrotask(pump);
				}
				else
					setTimeout(pump, 2);
				return;
			}
			const c = st.queue.shift();
			if (c.bytes.length > HEAPU32[CAPACITY])
			{
				/* cannot happen below the raw frame's size; the frames after it need a key frame */
				err("[rdpecam] a " + c.bytes.length + " byte chunk does not fit the slot");
				st.queue = st.queue.filter((q) => q.kind === CHUNK_CONFIG);
				st.needKey = true;
				continue;
			}
			HEAPU8.set(c.bytes, HEAPU32[DATA]);
			HEAPU32[KIND] = c.kind;
			HEAPU32[SIZE] = c.bytes.length;
			Atomics.store(HEAP32, STATE, SLOT_FULL);
			Atomics.notify(HEAP32, STATE);
		}
	};
	const push = (kind, bytes) => {
		st.queue.push({ kind: kind, bytes: bytes });
		if (!st.pumping)
			pump();
	};

	const output = (chunk, meta) => {
		if (st.stopped)
			return;
		const desc = meta && meta.decoderConfig && meta.decoderConfig.description;
		if (desc)
		{
			const view = ArrayBuffer.isView(desc)
			                 ? new Uint8Array(desc.buffer, desc.byteOffset, desc.byteLength)
			                 : new Uint8Array(desc);
			push(CHUNK_CONFIG, view.slice());
		}
		const bytes = new Uint8Array(chunk.byteLength);
		chunk.copyTo(bytes);
		const key = chunk.type === "key";
		const now = performance.now();
		const begun = st.started.get(chunk.timestamp);
		st.started.delete(chunk.timestamp);
		if (begun !== undefined)
		{
			HEAPU32[ENCODE_US] = (HEAPU32[ENCODE_US] + Math.round((now - begun) * 1000)) >>> 0;
			st.stats.encodeMs += now - begun;
		}
		HEAPU32[FRAMES] = (HEAPU32[FRAMES] + 1) >>> 0;
		st.stats.chunks++;
		st.stats.bytes += bytes.length;
		if (key)
		{
			HEAPU32[KEYS] = (HEAPU32[KEYS] + 1) >>> 0;
			st.stats.keys++;
		}
		if (st.stats.chunks === 1)
		{
			st.stats.firstMs = now - t0;
			HEAPU32[FIRST_US] = Math.round(st.stats.firstMs * 1000) >>> 0;
		}
		push(key ? CHUNK_KEY : CHUNK_DELTA, bytes);
	};

	/* the encoder gets frames of the size the server picked; another size is drawn to it */
	let canvas = null, ctx2d = null;
	const sized = (frame) => {
		if (frame.displayWidth === width && frame.displayHeight === height)
			return frame;
		if (!canvas)
		{
			canvas = new OffscreenCanvas(width, height);
			ctx2d = canvas.getContext("2d");
		}
		ctx2d.drawImage(frame, 0, 0, width, height);
		return new VideoFrame(canvas, { timestamp: frame.timestamp });
	};

	const interval = 1000 / fps;
	let last = -Infinity;
	/* the camera may run faster than the frame rate the server picked */
	const due = () => {
		const now = performance.now();
		if (now - last < interval * 0.9)
			return false;
		last = now;
		return true;
	};

	/* one camera frame: encoded, or skipped while the encoder or the network is behind (an
	 * encoded chunk is never dropped: the frames after it would break) */
	const encode = (frame) => {
		if (st.stopped || st.failed || !due())
			return;
		if (st.encoder.encodeQueueSize > 1 || st.queue.length > 2)
		{
			HEAPU32[SKIPPED] = (HEAPU32[SKIPPED] + 1) >>> 0;
			st.stats.skipped++;
			return;
		}
		const input = sized(frame);
		const key = st.needKey || st.encoded % keyEvery === 0;
		st.needKey = false;
		st.encoded++;
		st.started.set(input.timestamp, performance.now());
		try
		{
			st.encoder.encode(input, { keyFrame: key });
		}
		finally
		{
			if (input !== frame)
				input.close();
		}
	};

	(async () => {
		try
		{
			const config = {
				codec: codec,
				width: width,
				height: height,
				bitrate: bitrate,
				framerate: fps,
				latencyMode: "realtime",
				avc: { format: "avc" }
			};
			const support = await VideoEncoder.isConfigSupported(config);
			if (!support.supported)
				throw new Error("this browser cannot encode " + codec + " " + width + "x" + height);
			if (st.stopped)
				return;
			st.encoder = new VideoEncoder({ output: output, error: fail });
			st.encoder.configure(config);
			st.stats.encoder = codec + " " + (bitrate / 1000) + " kbit/s";

			const list = Module.freerdpCameras;
			const cam = Array.isArray(list) ? list[index] : null;
			const video = {
				width: { ideal: width },
				height: { ideal: height },
				frameRate: { ideal: fps }
			};
			if (cam && cam.deviceId)
				video.deviceId = { exact: cam.deviceId };
			const stream =
			    await navigator.mediaDevices.getUserMedia({ video: video, audio: false });
			if (st.stopped)
			{
				stream.getTracks().forEach((t) => t.stop());
				return;
			}
			st.stream = stream;
			const track = stream.getVideoTracks()[0];

			if (typeof MediaStreamTrackProcessor === "function")
			{
				/* Chrome, Edge: the camera's frames as they come */
				st.reader = new MediaStreamTrackProcessor({ track: track }).readable.getReader();
				for (;;)
				{
					const r = await st.reader.read();
					const frame = r.value;
					if (r.done || st.stopped)
					{
						if (frame)
							frame.close();
						break;
					}
					try
					{
						encode(frame);
					}
					finally
					{
						frame.close();
					}
				}
				return;
			}

			/* Firefox, Safari: a <video> element, a frame per requestVideoFrameCallback */
			const v = (st.video = document.createElement("video"));
			v.muted = true;
			v.playsInline = true;
			v.srcObject = stream;
			await v.play();
			const step = (now, meta) => {
				if (st.stopped)
					return;
				try
				{
					const frame = new VideoFrame(v, {
						timestamp: Math.round(
						    (meta && meta.mediaTime !== undefined ? meta.mediaTime : now / 1000) *
						    1e6)
					});
					try
					{
						encode(frame);
					}
					finally
					{
						frame.close();
					}
				}
				catch (e)
				{
					fail(e);
					return;
				}
				next();
			};
			const next = () => {
				if (st.stopped)
					return;
				if (typeof v.requestVideoFrameCallback === "function")
					v.requestVideoFrameCallback(step);
				else
					setTimeout(() => step(performance.now()), interval);
			};
			next();
		}
		catch (e)
		{
			fail(e);
		}
	})();
}

function cam_web_stop(index, capture)
{
	const st = Module.freerdpCamera && Module.freerdpCamera[index];
	if (st)
		st.stop();
	else
		_camera_emscripten_release(capture);
}

/* No __deps on camera_emscripten_release: it is EMSCRIPTEN_KEEPALIVE, and a dependency would make
 * it internal to the library. */
addToLibrary({
	cam_web_device_count,
	cam_web_device_count__sig: "i",
	cam_web_device_count__proxy: "sync",
	cam_web_device_label,
	cam_web_device_label__sig: "iipi",
	cam_web_device_label__proxy: "sync",
	cam_web_device_label__deps: ["$stringToUTF8"],
	cam_web_can_h264,
	cam_web_can_h264__sig: "i",
	cam_web_can_h264__proxy: "sync",
	cam_web_start,
	cam_web_start__sig: "vipiiiiip",
	cam_web_start__deps: ["$UTF8ToString"],
	cam_web_start__proxy: "async",
	cam_web_stop,
	cam_web_stop__sig: "vip",
	cam_web_stop__proxy: "async",
});
