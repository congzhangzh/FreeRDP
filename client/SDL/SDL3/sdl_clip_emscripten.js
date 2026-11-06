/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client clipboard helper: browser side for Emscripten builds
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

/* JavaScript side of the browser clipboard in sdl_clip.cpp, linked with --js-library. Runs on
 * the main browser thread. */
function freerdp_web_clipboard_write_js(utf8)
{
	const text = UTF8ToString(utf8);
	if (Module.freerdpClipboardWrite)
		Module.freerdpClipboardWrite(text);
	else if (navigator.clipboard)
		navigator.clipboard.writeText(text).catch(() => {}); /* needs the page focused */
}

function freerdp_web_clipboard_write_image_js(data, size, png)
{
	const bytes = HEAPU8.slice(data, data + size);
	if (Module.freerdpClipboardWriteImage)
		Module.freerdpClipboardWriteImage(bytes, !!png); /* CF_DIB, or PNG as it is */
}

function freerdp_web_clipboard_files_js(list, size)
{
	if (Module.freerdpClipboardFiles)
		Module.freerdpClipboardFiles(size ? HEAPU8.slice(list, list + size) : null);
}

/* A page without file support offers no files, and refuses what it is asked for anyway */
function freerdp_web_clipboard_file_request_js(streamId, listIndex, flags, offsetLow, offsetHigh,
                                               size)
{
	if (Module.freerdpClipboardFileRequest)
		Module.freerdpClipboardFileRequest(streamId, listIndex, flags, offsetLow, offsetHigh, size);
	else
		Module.ccall("freerdp_web_clipboard_file_answer", "number",
		             [ "number", "number", "number", "number" ], [ streamId, 0, 0, 0 ]);
}

function freerdp_web_clipboard_file_data_js(streamId, ok, data, size)
{
	if (Module.freerdpClipboardFileData)
		Module.freerdpClipboardFileData(streamId, !!ok, size ? HEAPU8.slice(data, data + size)
		                                                     : new Uint8Array(0));
}

addToLibrary({
	freerdp_web_clipboard_write_js,
	freerdp_web_clipboard_write_js__sig: "vp",
	freerdp_web_clipboard_write_js__deps: ["$UTF8ToString"],
	freerdp_web_clipboard_write_image_js,
	freerdp_web_clipboard_write_image_js__sig: "vppi",
	freerdp_web_clipboard_files_js,
	freerdp_web_clipboard_files_js__sig: "vpp",
	freerdp_web_clipboard_file_request_js,
	freerdp_web_clipboard_file_request_js__sig: "viiiiii",
	freerdp_web_clipboard_file_data_js,
	freerdp_web_clipboard_file_data_js__sig: "viipi",
});
