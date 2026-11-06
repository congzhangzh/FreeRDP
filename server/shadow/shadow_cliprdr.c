/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * The clipboard of the shared session: the part common to all subsystems.
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

/* One clipboard in the session, one in every client. A subsystem talks to this
 * file in mime types, the clients in clipboard formats; the conversions between
 * them come from WinPR's wClipboard.
 *
 *   session copies   subsystem -> session_changed -> Format List to every client
 *   client pastes    Format Data Request -> subsystem->ClipboardRead
 *                    subsystem -> session_data   -> Format Data Response
 *
 *   client copies    Format List -> subsystem->ClipboardOffer
 *   session pastes   subsystem -> session_request -> Format Data Request to the owner
 *                    Format Data Response         -> subsystem->ClipboardProvide
 *
 * Nothing is copied before somebody pastes. The last one to copy owns the
 * clipboard: when a client does, the session offers its data, and the other
 * clients get an empty list, as their last list no longer holds.
 *
 * Data goes only to and from clients that may interact: a viewer sees the screen,
 * not the clipboard.
 *
 * Files go the same way, for a subsystem that sets ClipboardFiles:
 *
 *   session copies   text/uri-list -> the FileGroupDescriptorW list of WinPR's wClipboard,
 *                    which walks the directories and reads the files for the client
 *   client copies    the session offers text/uri-list and x-special/gnome-copied-files
 *   session pastes   a thread fetches the list and every file into a private directory
 *                    in the user's cache, and then answers with their paths
 *
 * The directory lives as long as what the client offered. The names in the list are
 * the client's: none may leave the directory. */

#include <winpr/assert.h>
#include <winpr/cast.h>
#include <winpr/clipboard.h>
#include <winpr/collections.h>
#include <winpr/crypto.h>
#include <winpr/file.h>
#include <winpr/image.h>
#include <winpr/path.h>
#include <winpr/print.h>
#include <winpr/shell.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/thread.h>
#include <winpr/user.h>

#include <freerdp/log.h>
#include <freerdp/utils/cliprdr_utils.h>

#include "shadow.h"
#include "shadow_cliprdr.h"

#define TAG SERVER_TAG("shadow.clipboard")

#if defined(CHANNEL_CLIPRDR_SERVER)

/* How long a client's own Format List may take to follow its capabilities */
#define SHADOW_CLIPBOARD_LIST_WAIT_MS 1000

/* A client that stopped answering must not block the pastes after it. */
#define SHADOW_CLIPBOARD_FETCH_TIMEOUT_MS 10000

#define SHADOW_CLIPBOARD_MIME_HTML "text/html"
#define SHADOW_CLIPBOARD_FORMAT_HTML "HTML Format"

#define SHADOW_CLIPBOARD_FORMAT_FILES "FileGroupDescriptorW"

/* the most mime types a client's clipboard turns into */
#define SHADOW_CLIPBOARD_MAX_OFFER 16

/* A client that stopped sending a file: the time between two answers */
#define SHADOW_CLIPBOARD_FILE_TIMEOUT_MS 30000
/* One round trip for each of these */
#define SHADOW_CLIPBOARD_FILE_CHUNK (1024u * 1024u)
/* The most a client gets in one answer */
#define SHADOW_CLIPBOARD_FILE_MAX_ANSWER (16u * 1024u * 1024u)
#define SHADOW_CLIPBOARD_FILE_MAX_COUNT 65536u
/* What a paste leaves free on the disk */
#define SHADOW_CLIPBOARD_FILE_DISK_SPARE (256ull * 1024ull * 1024ull)

typedef struct
{
	rdpShadowClient* client;
	BOOL ready; /* it sent its first Format List: it may get ours */
} ShadowClipboardPeer;

/* a client's paste: the session's data, which the subsystem delivers */
typedef struct
{
	rdpShadowClient* client; /* nullptr once it left */
	UINT32 formatId;
	char* mime;
} ShadowClipboardRead;

/* the session's paste of files: the owner's list and files, which a thread fetches */
typedef struct s_shadow_clipboard_transfer ShadowClipboardTransfer;

typedef struct
{
	UINT32 request;
	BOOL gnome; /* x-special/gnome-copied-files, or text/uri-list */
} ShadowClipboardWaiter;

/* the session's paste: the owner's data, which that client delivers */
typedef struct
{
	rdpShadowClient* client;
	ShadowClipboardTransfer* transfer; /* the file list for it, not a paste of the subsystem */
	UINT32 request;
	UINT32 formatId; /* as the client numbers it */
	UINT32 sourceId; /* as wClipboard numbers it */
	UINT32 mimeId;
	BOOL text;
	BOOL canceled; /* the answer is on its way, and nobody wants it */
	UINT64 sent;
} ShadowClipboardFetch;

struct s_shadow_clipboard_transfer
{
	rdpShadowClipboard* clipboard;
	rdpShadowClient* client; /* nullptr once nobody wants the files */
	HANDLE thread;
	HANDLE event;        /* an answer, or nullptr in client: manual reset */
	BOOL running;        /* the thread may still write to root */
	char* root;          /* removed with the transfer */
	wArrayList* waiters; /* the pastes until done */
	BOOL done;
	char* uriList;
	char* gnomeFiles;

	/* the client's latest answer: the thread takes it */
	UINT32 streamId;
	BOOL answered;
	BOOL answerOk;
	BYTE* answer;
	UINT32 answerSize;
};

struct rdp_shadow_clipboard
{
	rdpShadowServer* server;
	CRITICAL_SECTION lock;
	wClipboard* convert;
	UINT32 htmlId;

	/* the session's files: its own, as a conversion ends the file list of a wClipboard */
	wClipboard* files;
	UINT32 filesId;
	rdpShadowClient* filesClient; /* the one a file request answers to */

	wArrayList* peers;

	/* the session's clipboard */
	char** sessionMimes;
	size_t nSessionMimes;

	/* the client whose clipboard the session offers */
	rdpShadowClient* owner;
	CLIPRDR_FORMAT* ownerFormats;
	UINT32 nOwnerFormats;

	wArrayList* reads; /* the first is at the subsystem while readBusy */
	BOOL readBusy;
	wArrayList* fetches; /* the first is at the client while fetchBusy */
	BOOL fetchBusy;

	ShadowClipboardTransfer* transfer; /* of the owner's files */
	wArrayList* retired;               /* given up: their threads may still run */
};

static const char* const shadow_clipboard_text_mimes[] = { "text/plain;charset=utf-8",
	                                                       "UTF8_STRING", "text/plain" };

static const UINT32 shadow_clipboard_image_formats[] = { WINPR_IMAGE_PNG, WINPR_IMAGE_BITMAP,
	                                                     WINPR_IMAGE_WEBP, WINPR_IMAGE_JPEG };

static void shadow_clipboard_read_free(void* obj)
{
	ShadowClipboardRead* read = obj;
	if (!read)
		return;
	free(read->mime);
	free(read);
}

static void shadow_clipboard_formats_free(CLIPRDR_FORMAT* formats, UINT32 count)
{
	if (!formats)
		return;
	for (UINT32 x = 0; x < count; x++)
		free(formats[x].formatName);
	free(formats);
}

static void shadow_clipboard_mimes_free(char** mimes, size_t count)
{
	if (!mimes)
		return;
	for (size_t x = 0; x < count; x++)
		free(mimes[x]);
	free((void*)mimes);
}

WINPR_ATTR_NODISCARD
static rdpShadowSubsystem* shadow_clipboard_subsystem(const rdpShadowClipboard* clipboard)
{
	WINPR_ASSERT(clipboard);
	WINPR_ASSERT(clipboard->server);
	return clipboard->server->subsystem;
}

WINPR_ATTR_NODISCARD
static ShadowClipboardPeer* shadow_clipboard_peer(rdpShadowClipboard* clipboard,
                                                  const rdpShadowClient* client)
{
	WINPR_ASSERT(clipboard);

	const size_t count = ArrayList_Count(clipboard->peers);
	for (size_t x = 0; x < count; x++)
	{
		ShadowClipboardPeer* peer = ArrayList_GetItem(clipboard->peers, x);
		if (peer && (peer->client == client))
			return peer;
	}
	return nullptr;
}

/* Both sides can do it: a client said so in its capabilities */
WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_may_copy_files(const rdpShadowClipboard* clipboard,
                                            const rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	return shadow_clipboard_subsystem(clipboard)->ClipboardFiles && client->cliprdr &&
	       client->cliprdr->streamFileClipEnabled;
}

WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_session_has(const rdpShadowClipboard* clipboard, const char* mime)
{
	WINPR_ASSERT(clipboard);
	WINPR_ASSERT(mime);

	for (size_t x = 0; x < clipboard->nSessionMimes; x++)
	{
		if (strcmp(clipboard->sessionMimes[x], mime) == 0)
			return TRUE;
	}
	return FALSE;
}

/* The mime type to read from the session for a format a client asks for */
WINPR_ATTR_NODISCARD
static const char* shadow_clipboard_session_mime(const rdpShadowClipboard* clipboard,
                                                 UINT32 formatId)
{
	WINPR_ASSERT(clipboard);

	if ((formatId == CF_UNICODETEXT) || (formatId == CF_TEXT) || (formatId == CF_OEMTEXT))
	{
		for (size_t x = 0; x < ARRAYSIZE(shadow_clipboard_text_mimes); x++)
		{
			if (shadow_clipboard_session_has(clipboard, shadow_clipboard_text_mimes[x]))
				return shadow_clipboard_text_mimes[x];
		}
	}
	else if ((formatId == CF_DIB) || (formatId == CF_DIBV5))
	{
		for (size_t x = 0; x < ARRAYSIZE(shadow_clipboard_image_formats); x++)
		{
			const UINT32 image = shadow_clipboard_image_formats[x];
			const char* mime = winpr_image_format_mime(image);
			if (mime && winpr_image_format_is_supported(image) &&
			    shadow_clipboard_session_has(clipboard, mime))
				return mime;
		}
	}
	else if ((formatId == clipboard->htmlId) &&
	         shadow_clipboard_session_has(clipboard, SHADOW_CLIPBOARD_MIME_HTML))
		return SHADOW_CLIPBOARD_MIME_HTML;
	else if (formatId == clipboard->filesId)
	{
		/* the first, as it also holds what was cut */
		if (shadow_clipboard_session_has(clipboard, SHADOW_CLIPBOARD_MIME_URI_LIST))
			return SHADOW_CLIPBOARD_MIME_URI_LIST;
		if (shadow_clipboard_session_has(clipboard, SHADOW_CLIPBOARD_MIME_GNOME_FILES))
			return SHADOW_CLIPBOARD_MIME_GNOME_FILES;
	}

	return nullptr;
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_send_list(const rdpShadowClipboard* clipboard, rdpShadowClient* client)
{
	WINPR_ASSERT(clipboard);
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->cliprdr);

	char htmlName[] = SHADOW_CLIPBOARD_FORMAT_HTML;
	char fileListName[] = SHADOW_CLIPBOARD_FORMAT_FILES;
	CLIPRDR_FORMAT formats[4] = WINPR_C_ARRAY_INIT;
	UINT32 count = 0;

	if (shadow_clipboard_session_mime(clipboard, CF_UNICODETEXT))
		formats[count++].formatId = CF_UNICODETEXT;
	if (shadow_clipboard_session_mime(clipboard, clipboard->htmlId))
	{
		formats[count].formatId = clipboard->htmlId;
		formats[count++].formatName = htmlName;
	}
	if (shadow_clipboard_session_mime(clipboard, CF_DIB))
		formats[count++].formatId = CF_DIB;
	if (shadow_clipboard_may_copy_files(clipboard, client) &&
	    shadow_clipboard_session_mime(clipboard, clipboard->filesId))
	{
		formats[count].formatId = clipboard->filesId;
		formats[count++].formatName = fileListName;
	}

	CLIPRDR_FORMAT_LIST list = WINPR_C_ARRAY_INIT;
	list.common.msgType = CB_FORMAT_LIST;
	list.numFormats = count;
	list.formats = formats;
	return client->cliprdr->ServerFormatList(client->cliprdr, &list);
}

/* To every client but one, which may be nullptr: what the session offers now */
static void shadow_clipboard_announce(rdpShadowClipboard* clipboard, const rdpShadowClient* except)
{
	WINPR_ASSERT(clipboard);

	const size_t count = ArrayList_Count(clipboard->peers);
	for (size_t x = 0; x < count; x++)
	{
		const ShadowClipboardPeer* peer = ArrayList_GetItem(clipboard->peers, x);
		if (!peer || !peer->ready || (peer->client == except) || !peer->client->mayInteract)
			continue;

		const UINT rc = shadow_clipboard_send_list(clipboard, peer->client);
		if (rc != CHANNEL_RC_OK)
			WLog_WARN(TAG, "ServerFormatList failed with %" PRIu32, rc);
	}
}

/* @return the data in the other format, to be freed, or nullptr */
WINPR_ATTR_NODISCARD
static BYTE* shadow_clipboard_convert(rdpShadowClipboard* clipboard, UINT32 sourceId,
                                      UINT32 targetId, const BYTE* data, size_t size,
                                      UINT32* pConverted)
{
	WINPR_ASSERT(clipboard);
	WINPR_ASSERT(pConverted);

	*pConverted = 0;
	if (!data || (size == 0) || (size > SHADOW_CLIPBOARD_MAX_SIZE))
		return nullptr;

	if (!ClipboardEmpty(clipboard->convert))
		return nullptr;
	const BOOL stored = ClipboardSetData(clipboard->convert, sourceId, data,
	                                     WINPR_ASSERTING_INT_CAST(UINT32, size));
	if (!stored)
		return nullptr;
	return ClipboardGetData(clipboard->convert, targetId, pConverted);
}

/* @return the session's files as a client's file list, to be freed, or nullptr */
WINPR_ATTR_NODISCARD
static BYTE* shadow_clipboard_file_list(rdpShadowClipboard* clipboard,
                                        const rdpShadowClient* client, const char* mime,
                                        const BYTE* data, size_t size, UINT32* pSize)
{
	WINPR_ASSERT(clipboard);
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->cliprdr);
	WINPR_ASSERT(pSize);

	*pSize = 0;
	if (!data || (size == 0) || (size > SHADOW_CLIPBOARD_MAX_SIZE))
		return nullptr;

	const UINT32 mimeId = ClipboardRegisterFormat(clipboard->files, mime);
	if (!ClipboardEmpty(clipboard->files) ||
	    !ClipboardSetData(clipboard->files, mimeId, data, WINPR_ASSERTING_INT_CAST(UINT32, size)))
		return nullptr;

	/* the file requests of the clients read this list */
	UINT32 bytes = 0;
	FILEDESCRIPTORW* descriptors = ClipboardGetData(clipboard->files, clipboard->filesId, &bytes);
	if (!descriptors)
		return nullptr;

	UINT32 flags = CB_STREAM_FILECLIP_ENABLED;
	if (client->cliprdr->hasHugeFileSupport)
		flags |= CB_HUGE_FILE_SUPPORT_ENABLED;

	BYTE* list = nullptr;
	const UINT rc = cliprdr_serialize_file_list_ex(flags, descriptors,
	                                               bytes / sizeof(FILEDESCRIPTORW), &list, pSize);
	free(descriptors);
	if (rc != CHANNEL_RC_OK)
	{
		WLog_WARN(TAG, "cannot write the file list: %" PRIu32, rc);
		return nullptr;
	}
	return list;
}

static void shadow_clipboard_respond(CliprdrServerContext* cliprdr, const BYTE* data, UINT32 size)
{
	WINPR_ASSERT(cliprdr);

	CLIPRDR_FORMAT_DATA_RESPONSE response = WINPR_C_ARRAY_INIT;
	response.common.msgType = CB_FORMAT_DATA_RESPONSE;
	response.common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
	response.common.dataLen = data ? size : 0;
	response.requestedFormatData = data;

	const UINT rc = cliprdr->ServerFormatDataResponse(cliprdr, &response);
	if (rc != CHANNEL_RC_OK)
		WLog_WARN(TAG, "ServerFormatDataResponse failed with %" PRIu32, rc);
}

/* Reads go to the subsystem one at a time, each one's answer starts the next */
static void shadow_clipboard_read_next(rdpShadowClipboard* clipboard)
{
	WINPR_ASSERT(clipboard);

	while (!clipboard->readBusy && (ArrayList_Count(clipboard->reads) > 0))
	{
		ShadowClipboardRead* read = ArrayList_GetItem(clipboard->reads, 0);
		WINPR_ASSERT(read);

		rdpShadowSubsystem* subsystem = shadow_clipboard_subsystem(clipboard);
		if (read->client && subsystem->ClipboardRead(subsystem, read->mime))
		{
			clipboard->readBusy = TRUE;
			return;
		}

		if (read->client)
			shadow_clipboard_respond(read->client->cliprdr, nullptr, 0);
		if (!ArrayList_RemoveAt(clipboard->reads, 0))
			return;
	}
}

static void shadow_clipboard_provide(rdpShadowClipboard* clipboard, UINT32 request,
                                     const BYTE* data, size_t size)
{
	WINPR_ASSERT(clipboard);

	rdpShadowSubsystem* subsystem = shadow_clipboard_subsystem(clipboard);
	if (!subsystem->ClipboardProvide(subsystem, request, data, size))
		WLog_WARN(TAG, "the subsystem did not take the data of request %" PRIu32, request);
}

/* The client's answer, for the thread of the transfer */
static void shadow_clipboard_transfer_answer(ShadowClipboardTransfer* transfer, BOOL ok,
                                             const BYTE* data, UINT32 size)
{
	WINPR_ASSERT(transfer);

	free(transfer->answer);
	transfer->answer = nullptr;
	transfer->answerSize = 0;
	transfer->answerOk = FALSE;
	if (ok && data && (size > 0))
	{
		transfer->answer = malloc(size);
		if (transfer->answer)
		{
			memcpy(transfer->answer, data, size);
			transfer->answerSize = size;
			transfer->answerOk = TRUE;
		}
	}
	transfer->answered = TRUE;
	if (!SetEvent(transfer->event))
		WLog_WARN(TAG, "cannot wake the file transfer");
}

/* The owner will not answer this one */
static void shadow_clipboard_fetch_failed(rdpShadowClipboard* clipboard,
                                          const ShadowClipboardFetch* fetch)
{
	WINPR_ASSERT(fetch);

	if (fetch->canceled)
		return;
	if (fetch->transfer)
		shadow_clipboard_transfer_answer(fetch->transfer, FALSE, nullptr, 0);
	else
		shadow_clipboard_provide(clipboard, fetch->request, nullptr, 0);
}

/* Fetches go to the owner one at a time: a Format Data Response names no request */
static void shadow_clipboard_fetch_next(rdpShadowClipboard* clipboard)
{
	WINPR_ASSERT(clipboard);

	while (!clipboard->fetchBusy && (ArrayList_Count(clipboard->fetches) > 0))
	{
		ShadowClipboardFetch* fetch = ArrayList_GetItem(clipboard->fetches, 0);
		WINPR_ASSERT(fetch);
		WINPR_ASSERT(fetch->client);
		WINPR_ASSERT(fetch->client->cliprdr);

		CLIPRDR_FORMAT_DATA_REQUEST request = WINPR_C_ARRAY_INIT;
		request.common.msgType = CB_FORMAT_DATA_REQUEST;
		request.common.dataLen = 4;
		request.requestedFormatId = fetch->formatId;

		CliprdrServerContext* cliprdr = fetch->client->cliprdr;
		const UINT rc = cliprdr->ServerFormatDataRequest(cliprdr, &request);
		if (rc == CHANNEL_RC_OK)
		{
			fetch->sent = GetTickCount64();
			clipboard->fetchBusy = TRUE;
			return;
		}

		WLog_WARN(TAG, "ServerFormatDataRequest failed with %" PRIu32, rc);
		shadow_clipboard_fetch_failed(clipboard, fetch);
		if (!ArrayList_RemoveAt(clipboard->fetches, 0))
			return;
	}
}

/* The owner changes: no fetch waiting will be served. A client's answer to the
 * one it has still arrives (unless the client left): it stays to catch that. */
static void shadow_clipboard_cancel_fetches(rdpShadowClipboard* clipboard,
                                            const rdpShadowClient* gone)
{
	WINPR_ASSERT(clipboard);

	for (size_t x = ArrayList_Count(clipboard->fetches); x > 0; x--)
	{
		const size_t index = x - 1;
		ShadowClipboardFetch* fetch = ArrayList_GetItem(clipboard->fetches, index);
		WINPR_ASSERT(fetch);

		/* its transfer ends with the owner */
		if (fetch->transfer)
		{
			fetch->transfer = nullptr;
			fetch->canceled = TRUE;
		}

		const BOOL atClient = (index == 0) && clipboard->fetchBusy;
		if (atClient && (fetch->client != gone))
		{
			if (!fetch->canceled)
				shadow_clipboard_provide(clipboard, fetch->request, nullptr, 0);
			fetch->canceled = TRUE;
			continue;
		}

		if (atClient)
			clipboard->fetchBusy = FALSE;
		if (!fetch->canceled)
			shadow_clipboard_provide(clipboard, fetch->request, nullptr, 0);
		if (!ArrayList_RemoveAt(clipboard->fetches, index))
			WLog_WARN(TAG, "cannot drop a clipboard request");
	}
}

WINPR_ATTR_NODISCARD
static const CLIPRDR_FORMAT* shadow_clipboard_find_format(const CLIPRDR_FORMAT* formats,
                                                          UINT32 count, UINT32 formatId,
                                                          const char* name)
{
	for (UINT32 x = 0; x < count; x++)
	{
		const CLIPRDR_FORMAT* format = &formats[x];
		if (name)
		{
			if (format->formatName && (strcmp(format->formatName, name) == 0))
				return format;
		}
		else if (format->formatId == formatId)
			return format;
	}
	return nullptr;
}

/* The rest of a paste of files. In order: a thread takes what a client sends, as a paste
 * of the whole may take far longer than any lock may be held. */
static void shadow_clipboard_transfer_free(void* obj)
{
	ShadowClipboardTransfer* transfer = obj;
	if (!transfer)
		return;
	if (transfer->thread)
		(void)CloseHandle(transfer->thread);
	if (transfer->event)
		(void)CloseHandle(transfer->event);
	ArrayList_Free(transfer->waiters);
	free(transfer->root);
	free(transfer->uriList);
	free(transfer->gnomeFiles);
	free(transfer->answer);
	free(transfer);
}

static void shadow_clipboard_transfer_remove_root(const ShadowClipboardTransfer* transfer)
{
	WINPR_ASSERT(transfer);

	if (transfer->root && winpr_PathFileExists(transfer->root) &&
	    !winpr_RemoveDirectory_RecursiveA(transfer->root))
		WLog_WARN(TAG, "cannot remove the files of a paste: %s", transfer->root);
}

/* Forget the transfers whose threads ended */
static void shadow_clipboard_transfers_reap(rdpShadowClipboard* clipboard)
{
	WINPR_ASSERT(clipboard);

	for (size_t x = ArrayList_Count(clipboard->retired); x > 0; x--)
	{
		const ShadowClipboardTransfer* transfer = ArrayList_GetItem(clipboard->retired, x - 1);
		if (transfer && (WaitForSingleObject(transfer->thread, 0) == WAIT_OBJECT_0) &&
		    !ArrayList_RemoveAt(clipboard->retired, x - 1))
			WLog_WARN(TAG, "cannot drop a file transfer");
	}
}

/* Nobody wants the owner's files any more: pastes still waiting fail, and they go */
static void shadow_clipboard_transfer_drop(rdpShadowClipboard* clipboard)
{
	WINPR_ASSERT(clipboard);

	ShadowClipboardTransfer* transfer = clipboard->transfer;
	if (!transfer)
		return;
	clipboard->transfer = nullptr;
	transfer->client = nullptr;

	const size_t count = ArrayList_Count(clipboard->fetches);
	for (size_t x = 0; x < count; x++)
	{
		ShadowClipboardFetch* fetch = ArrayList_GetItem(clipboard->fetches, x);
		if (fetch && (fetch->transfer == transfer))
		{
			fetch->transfer = nullptr;
			fetch->canceled = TRUE;
		}
	}

	const size_t waiting = ArrayList_Count(transfer->waiters);
	for (size_t x = 0; x < waiting; x++)
	{
		const ShadowClipboardWaiter* waiter = ArrayList_GetItem(transfer->waiters, x);
		WINPR_ASSERT(waiter);
		shadow_clipboard_provide(clipboard, waiter->request, nullptr, 0);
	}
	ArrayList_Clear(transfer->waiters);

	if (!SetEvent(transfer->event))
		WLog_WARN(TAG, "cannot wake the file transfer");

	/* otherwise the thread does, on its way out */
	if (!transfer->running)
		shadow_clipboard_transfer_remove_root(transfer);

	if (!ArrayList_Append(clipboard->retired, transfer))
		WLog_WARN(TAG, "cannot keep a file transfer");
	shadow_clipboard_transfers_reap(clipboard);
}

/* @return the client's next answer, to be freed, in the thread of the transfer */
WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_transfer_wait(ShadowClipboardTransfer* transfer, BYTE** pData,
                                           UINT32* pSize)
{
	WINPR_ASSERT(transfer);
	WINPR_ASSERT(pData);
	WINPR_ASSERT(pSize);

	rdpShadowClipboard* clipboard = transfer->clipboard;
	if (WaitForSingleObject(transfer->event, SHADOW_CLIPBOARD_FILE_TIMEOUT_MS) != WAIT_OBJECT_0)
	{
		WLog_WARN(TAG, "no file data from the client");
		return FALSE;
	}

	EnterCriticalSection(&clipboard->lock);
	if (!ResetEvent(transfer->event))
		WLog_WARN(TAG, "cannot reset the event of the file transfer");
	const BOOL ok = transfer->client && transfer->answered && transfer->answerOk;
	if (ok)
	{
		*pData = transfer->answer;
		*pSize = transfer->answerSize;
		transfer->answer = nullptr;
		transfer->answerSize = 0;
	}
	transfer->answered = FALSE;
	LeaveCriticalSection(&clipboard->lock);
	return ok;
}

/* Ask the owner for the size or a piece of a file: the thread then waits */
WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_transfer_ask(ShadowClipboardTransfer* transfer, UINT32 flags,
                                          UINT32 index, UINT64 offset, UINT32 size)
{
	WINPR_ASSERT(transfer);

	rdpShadowClipboard* clipboard = transfer->clipboard;
	BOOL ok = FALSE;

	EnterCriticalSection(&clipboard->lock);
	if (transfer->client)
	{
		CLIPRDR_FILE_CONTENTS_REQUEST request = WINPR_C_ARRAY_INIT;
		request.common.msgType = CB_FILECONTENTS_REQUEST;
		request.streamId = ++transfer->streamId;
		request.listIndex = index;
		request.dwFlags = flags;
		request.nPositionLow = (UINT32)(offset & UINT32_MAX);
		request.nPositionHigh = (UINT32)(offset >> 32);
		request.cbRequested = size;

		CliprdrServerContext* cliprdr = transfer->client->cliprdr;
		const UINT rc = cliprdr->ServerFileContentsRequest(cliprdr, &request);
		ok = rc == CHANNEL_RC_OK;
		if (!ok)
			WLog_WARN(TAG, "ServerFileContentsRequest failed with %" PRIu32, rc);
	}
	LeaveCriticalSection(&clipboard->lock);
	return ok;
}

/* A name from a client, as a path below the directory of the paste.
 * @return the path with / for a separator, to be freed, or nullptr for a name that is not
 *         a plain relative path */
WINPR_ATTR_NODISCARD
static char* shadow_clipboard_file_name(const FILEDESCRIPTORW* descriptor)
{
	WINPR_ASSERT(descriptor);

	const size_t length = _wcsnlen(descriptor->cFileName, ARRAYSIZE(descriptor->cFileName));
	if ((length == 0) || (length >= ARRAYSIZE(descriptor->cFileName)))
		return nullptr;

	char* name = ConvertWCharNToUtf8Alloc(descriptor->cFileName, length, nullptr);
	if (!name)
		return nullptr;

	const char* part = name;
	for (char* c = name;; c++)
	{
		if (*c == '\\')
			*c = '/';

		if ((*c == '/') || (*c == '\0'))
		{
			const size_t len = WINPR_ASSERTING_INT_CAST(size_t, c - part);
			if ((len == 0) || ((len == 1) && (part[0] == '.')) ||
			    ((len == 2) && (part[0] == '.') && (part[1] == '.')))
				goto fail;
			part = c + 1;
		}

		if (*c == '\0')
			return name;

		if (((unsigned char)*c < 0x20) || (*c == 0x7f))
			goto fail;
	}

fail:
	free(name);
	return nullptr;
}

/* What the free space of the disk of a directory, which exists, leaves for a paste */
WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_disk_has_room(const char* dir, UINT64 bytes)
{
	DWORD sectorsPerCluster = 0;
	DWORD bytesPerSector = 0;
	DWORD freeClusters = 0;
	DWORD clusters = 0;
	if (!GetDiskFreeSpaceA(dir, &sectorsPerCluster, &bytesPerSector, &freeClusters, &clusters))
		return FALSE;

	const UINT64 available = 1ull * freeClusters * sectorsPerCluster * bytesPerSector;
	return (available >= SHADOW_CLIPBOARD_FILE_DISK_SPARE) &&
	       (bytes <= available - SHADOW_CLIPBOARD_FILE_DISK_SPARE);
}

/* Fetch one file of the list. The name was checked, and only we write to the directory. */
WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_transfer_file(ShadowClipboardTransfer* transfer,
                                           const FILEDESCRIPTORW* descriptor, UINT32 index,
                                           const char* name, UINT64* pBytes)
{
	WINPR_ASSERT(transfer);
	WINPR_ASSERT(descriptor);
	WINPR_ASSERT(name);
	WINPR_ASSERT(pBytes);

	BOOL rc = FALSE;
	FILE* file = nullptr;
	char* path = nullptr;
	size_t pathLength = 0;
	if (winpr_asprintf(&path, &pathLength, "%s/%s", transfer->root, name) < 0)
		return FALSE;

	if ((descriptor->dwFlags & FD_ATTRIBUTES) &&
	    (descriptor->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		rc = winpr_PathMakePath(path, nullptr);
		goto out;
	}

	/* the list need not name the directories */
	char* slash = strrchr(path, '/');
	WINPR_ASSERT(slash);
	*slash = '\0';
	const BOOL made = winpr_PathMakePath(path, nullptr);
	*slash = '/';
	if (!made)
		goto out;

	UINT64 size = (1ull * descriptor->nFileSizeHigh << 32) | descriptor->nFileSizeLow;
	if ((descriptor->dwFlags & FD_FILESIZE) == 0)
	{
		BYTE* answer = nullptr;
		UINT32 length = 0;
		if (!shadow_clipboard_transfer_ask(transfer, FILECONTENTS_SIZE, index, 0, 8) ||
		    !shadow_clipboard_transfer_wait(transfer, &answer, &length))
			goto out;
		if (length == 8)
			size = winpr_Data_Get_UINT64(answer);
		free(answer);
		if (length != 8)
			goto out;
		if (!shadow_clipboard_disk_has_room(transfer->root, size))
		{
			WLog_WARN(TAG, "no room on the disk for %s", name);
			goto out;
		}
	}

	/* x: the same name twice in a list */
	file = winpr_fopen(path, "wbx");
	if (!file)
		goto out;

	UINT64 offset = 0;
	while (offset < size)
	{
		const UINT32 wanted = (UINT32)MIN(size - offset, SHADOW_CLIPBOARD_FILE_CHUNK);
		BYTE* answer = nullptr;
		UINT32 length = 0;
		if (!shadow_clipboard_transfer_ask(transfer, FILECONTENTS_RANGE, index, offset, wanted) ||
		    !shadow_clipboard_transfer_wait(transfer, &answer, &length))
			goto out;

		const BOOL ok =
		    (length > 0) && (length <= wanted) && (fwrite(answer, 1, length, file) == length);
		free(answer);
		if (!ok)
			goto out;
		offset += length;
	}
	*pBytes += offset;
	rc = TRUE;

out:
	if (file && (fclose(file) != 0))
		rc = FALSE;
	if (!rc)
		WLog_WARN(TAG, "cannot fetch %s", name);
	free(path);
	return rc;
}

/* Into a uri-list: all but the plain characters in percent */
static size_t shadow_clipboard_uri_append(char* uri, const char* text)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t length = 0;
	for (const char* c = text; *c; c++)
	{
		const unsigned char byte = (unsigned char)*c;
		if (((byte >= 'a') && (byte <= 'z')) || ((byte >= 'A') && (byte <= 'Z')) ||
		    ((byte >= '0') && (byte <= '9')) || strchr("-._~/", byte))
			uri[length++] = *c;
		else
		{
			uri[length++] = '%';
			uri[length++] = hex[byte >> 4];
			uri[length++] = hex[byte & 0x0f];
		}
	}
	return length;
}

/* The selection holds what the client copied: the first level of the list */
WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_transfer_lists(ShadowClipboardTransfer* transfer, char* const* names,
                                            UINT32 count)
{
	WINPR_ASSERT(transfer);
	WINPR_ASSERT(names);

	const size_t rootLength = strlen(transfer->root);
	size_t room = sizeof("copy\n");
	for (UINT32 x = 0; x < count; x++)
		room += sizeof("file://") + 3 * (rootLength + 1 + strlen(names[x])) + 2;

	transfer->uriList = calloc(room, sizeof(char));
	transfer->gnomeFiles = calloc(room, sizeof(char));
	if (!transfer->uriList || !transfer->gnomeFiles)
		return FALSE;

	char* line = calloc(room, sizeof(char));
	if (!line)
		return FALSE;

	memcpy(transfer->gnomeFiles, "copy", 4);
	size_t uriLength = 0;
	size_t gnomeLength = 4;
	const char* top = nullptr;
	size_t topLength = 0;
	for (UINT32 x = 0; x < count; x++)
	{
		const char* slash = strchr(names[x], '/');
		const size_t length = slash ? (size_t)(slash - names[x]) : strlen(names[x]);
		if (top && (length == topLength) && (strncmp(top, names[x], length) == 0))
			continue;
		top = names[x];
		topLength = length;

		char* name = strndup(names[x], length);
		if (!name)
		{
			free(line);
			return FALSE;
		}

		memcpy(line, "file://", 7);
		size_t lineLength = 7;
		lineLength += shadow_clipboard_uri_append(&line[lineLength], transfer->root);
		lineLength += shadow_clipboard_uri_append(&line[lineLength], "/");
		lineLength += shadow_clipboard_uri_append(&line[lineLength], name);
		free(name);

		memcpy(&transfer->uriList[uriLength], line, lineLength);
		uriLength += lineLength;
		memcpy(&transfer->uriList[uriLength], "\r\n", 2);
		uriLength += 2;

		transfer->gnomeFiles[gnomeLength++] = '\n';
		memcpy(&transfer->gnomeFiles[gnomeLength], line, lineLength);
		gnomeLength += lineLength;
	}
	free(line);
	return TRUE;
}

/* Everything the client says here is checked: names, sizes, and how long it takes */
WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_transfer_run(ShadowClipboardTransfer* transfer)
{
	WINPR_ASSERT(transfer);

	BOOL rc = FALSE;
	BYTE* data = nullptr;
	UINT32 size = 0;
	FILEDESCRIPTORW* descriptors = nullptr;
	UINT32 count = 0;
	char** names = nullptr;
	UINT64 bytes = 0;
	UINT64 fetched = 0;
	const UINT64 start = GetTickCount64();

	if (!shadow_clipboard_transfer_wait(transfer, &data, &size))
		goto out;

	const UINT parsed = cliprdr_parse_file_list(data, size, &descriptors, &count);
	if ((parsed != CHANNEL_RC_OK) || (count == 0) || (count > SHADOW_CLIPBOARD_FILE_MAX_COUNT))
	{
		WLog_WARN(TAG, "a client's file list is not usable: %" PRIu32 " files", count);
		goto out;
	}

	names = (char**)calloc(count, sizeof(char*));
	if (!names)
		goto out;

	for (UINT32 x = 0; x < count; x++)
	{
		const FILEDESCRIPTORW* descriptor = &descriptors[x];
		names[x] = shadow_clipboard_file_name(descriptor);
		if (!names[x])
		{
			WLog_WARN(TAG, "a client's file list has a name that leaves the directory, refused");
			goto out;
		}

		const UINT64 fileSize = (1ull * descriptor->nFileSizeHigh << 32) | descriptor->nFileSizeLow;
		if (fileSize > UINT64_MAX - bytes)
		{
			WLog_WARN(TAG, "a client's file list is larger than a number holds, refused");
			goto out;
		}
		bytes += fileSize;
	}

	if (!winpr_PathMakePath(transfer->root, nullptr))
	{
		WLog_WARN(TAG, "cannot make %s", transfer->root);
		goto out;
	}

	if (!shadow_clipboard_disk_has_room(transfer->root, bytes))
	{
		WLog_WARN(TAG, "no room on the disk for %" PRIu64 " bytes of files", bytes);
		goto out;
	}

	for (UINT32 x = 0; x < count; x++)
	{
		if (!shadow_clipboard_transfer_file(transfer, &descriptors[x], x, names[x], &fetched))
			goto out;
	}

	rc = shadow_clipboard_transfer_lists(transfer, names, count);
	if (rc)
		WLog_INFO(TAG, "fetched %" PRIu32 " files, %" PRIu64 " bytes in %" PRIu64 " ms", count,
		          fetched, GetTickCount64() - start);

out:
	if (names)
	{
		for (UINT32 x = 0; x < count; x++)
			free(names[x]);
		free((void*)names);
	}
	free(descriptors);
	free(data);
	return rc;
}

static void shadow_clipboard_provide_files(rdpShadowClipboard* clipboard,
                                           const ShadowClipboardTransfer* transfer, UINT32 request,
                                           BOOL gnome)
{
	const char* list = gnome ? transfer->gnomeFiles : transfer->uriList;
	shadow_clipboard_provide(clipboard, request, (const BYTE*)list, strlen(list));
}

static DWORD WINAPI shadow_clipboard_transfer_thread(LPVOID arg)
{
	ShadowClipboardTransfer* transfer = arg;
	WINPR_ASSERT(transfer);

	rdpShadowClipboard* clipboard = transfer->clipboard;
	const BOOL ok = shadow_clipboard_transfer_run(transfer);

	EnterCriticalSection(&clipboard->lock);
	if (clipboard->transfer == transfer)
	{
		if (ok)
		{
			transfer->done = TRUE;
			const size_t count = ArrayList_Count(transfer->waiters);
			for (size_t x = 0; x < count; x++)
			{
				const ShadowClipboardWaiter* waiter = ArrayList_GetItem(transfer->waiters, x);
				WINPR_ASSERT(waiter);
				shadow_clipboard_provide_files(clipboard, transfer, waiter->request, waiter->gnome);
			}
			ArrayList_Clear(transfer->waiters);
		}
		else
			shadow_clipboard_transfer_drop(clipboard); /* the next paste tries again */
	}
	transfer->running = FALSE;
	const BOOL dropped = !transfer->client;
	LeaveCriticalSection(&clipboard->lock);

	if (dropped)
		shadow_clipboard_transfer_remove_root(transfer);
	return 0;
}

/* @return the transfer of the owner's files, under way */
WINPR_ATTR_NODISCARD
static ShadowClipboardTransfer* shadow_clipboard_transfer_start(rdpShadowClipboard* clipboard)
{
	WINPR_ASSERT(clipboard);

	if (!clipboard->owner)
		return nullptr;
	const CLIPRDR_FORMAT* source = shadow_clipboard_find_format(
	    clipboard->ownerFormats, clipboard->nOwnerFormats, 0, SHADOW_CLIPBOARD_FORMAT_FILES);
	if (!source)
		return nullptr;

	ShadowClipboardTransfer* transfer = calloc(1, sizeof(ShadowClipboardTransfer));
	ShadowClipboardFetch* fetch = calloc(1, sizeof(ShadowClipboardFetch));
	char* cache = GetKnownPath(KNOWN_PATH_XDG_CACHE_HOME);
	BYTE random[16] = WINPR_C_ARRAY_INIT;
	char* tag = nullptr;
	size_t length = 0;
	if (!transfer || !fetch || !cache || (winpr_RAND(random, sizeof(random)) < 0))
		goto fail;
	tag = winpr_BinToHexString(random, sizeof(random), FALSE);
	if (!tag)
		goto fail;
	if (winpr_asprintf(&transfer->root, &length, "%s/freerdp-shadow/clipboard/%s", cache, tag) < 0)
		goto fail;

	transfer->clipboard = clipboard;
	transfer->client = clipboard->owner;
	transfer->event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	transfer->waiters = ArrayList_New(FALSE);
	if (!transfer->event || !transfer->waiters)
		goto fail;
	wObject* waiters = ArrayList_Object(transfer->waiters);
	WINPR_ASSERT(waiters);
	waiters->fnObjectFree = free;

	fetch->client = clipboard->owner;
	fetch->transfer = transfer;
	fetch->formatId = source->formatId;

	transfer->running = TRUE;
	transfer->thread =
	    CreateThread(nullptr, 0, shadow_clipboard_transfer_thread, transfer, 0, nullptr);
	if (!transfer->thread)
		goto fail;

	clipboard->transfer = transfer;
	if (!ArrayList_Append(clipboard->fetches, fetch))
	{
		free(fetch);
		shadow_clipboard_transfer_drop(clipboard);
		transfer = nullptr;
	}
	else
		shadow_clipboard_fetch_next(clipboard);

	free(tag);
	free(cache);
	return transfer;

fail:
	shadow_clipboard_transfer_free(transfer);
	free(fetch);
	free(tag);
	free(cache);
	return nullptr;
}

/* The session pastes the owner's files. All pastes of them share one transfer. */
static void shadow_clipboard_paste_files(rdpShadowClipboard* clipboard, UINT32 request, BOOL gnome)
{
	WINPR_ASSERT(clipboard);

	ShadowClipboardTransfer* transfer = clipboard->transfer;
	if (!transfer)
		transfer = shadow_clipboard_transfer_start(clipboard);

	if (transfer && transfer->done)
	{
		shadow_clipboard_provide_files(clipboard, transfer, request, gnome);
		return;
	}

	ShadowClipboardWaiter* waiter = calloc(1, sizeof(ShadowClipboardWaiter));
	if (transfer && waiter)
	{
		waiter->request = request;
		waiter->gnome = gnome;
		if (ArrayList_Append(transfer->waiters, waiter))
			return;
	}
	free(waiter);
	shadow_clipboard_provide(clipboard, request, nullptr, 0);
}

static void shadow_clipboard_clear_owner(rdpShadowClipboard* clipboard)
{
	WINPR_ASSERT(clipboard);

	shadow_clipboard_transfer_drop(clipboard);
	shadow_clipboard_formats_free(clipboard->ownerFormats, clipboard->nOwnerFormats);
	clipboard->ownerFormats = nullptr;
	clipboard->nOwnerFormats = 0;
	clipboard->owner = nullptr;
}

/* What the session offers is gone with its owner: a paste must not hang on it */
static void shadow_clipboard_release_owner(rdpShadowClipboard* clipboard)
{
	WINPR_ASSERT(clipboard);

	shadow_clipboard_clear_owner(clipboard);

	rdpShadowSubsystem* subsystem = shadow_clipboard_subsystem(clipboard);
	if (!subsystem->ClipboardOffer(subsystem, nullptr, 0))
		WLog_WARN(TAG, "the session keeps a clipboard nobody serves");
}

WINPR_ATTR_NODISCARD
static const CLIPRDR_FORMAT* shadow_clipboard_owner_text(const CLIPRDR_FORMAT* formats,
                                                         UINT32 count)
{
	const UINT32 ids[] = { CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT };
	for (size_t x = 0; x < ARRAYSIZE(ids); x++)
	{
		const CLIPRDR_FORMAT* format =
		    shadow_clipboard_find_format(formats, count, ids[x], nullptr);
		if (format)
			return format;
	}
	return nullptr;
}

WINPR_ATTR_NODISCARD
static const CLIPRDR_FORMAT* shadow_clipboard_owner_image(const CLIPRDR_FORMAT* formats,
                                                          UINT32 count)
{
	const CLIPRDR_FORMAT* format = shadow_clipboard_find_format(formats, count, CF_DIB, nullptr);
	if (format)
		return format;
	return shadow_clipboard_find_format(formats, count, CF_DIBV5, nullptr);
}

/* What a client's list means in the session. @return the number of mime types */
WINPR_ATTR_NODISCARD
static size_t shadow_clipboard_list_mimes(const CLIPRDR_FORMAT* formats, UINT32 count, BOOL files,
                                          const char* mimes[SHADOW_CLIPBOARD_MAX_OFFER])
{
	size_t offered = 0;

	if (files && shadow_clipboard_find_format(formats, count, 0, SHADOW_CLIPBOARD_FORMAT_FILES))
	{
		mimes[offered++] = SHADOW_CLIPBOARD_MIME_GNOME_FILES;
		mimes[offered++] = SHADOW_CLIPBOARD_MIME_URI_LIST;
	}

	if (shadow_clipboard_owner_text(formats, count))
	{
		for (size_t x = 0; x < ARRAYSIZE(shadow_clipboard_text_mimes); x++)
			mimes[offered++] = shadow_clipboard_text_mimes[x];
	}

	if (shadow_clipboard_find_format(formats, count, 0, SHADOW_CLIPBOARD_FORMAT_HTML))
		mimes[offered++] = SHADOW_CLIPBOARD_MIME_HTML;

	if (shadow_clipboard_owner_image(formats, count))
	{
		for (size_t x = 0; x < ARRAYSIZE(shadow_clipboard_image_formats); x++)
		{
			const UINT32 image = shadow_clipboard_image_formats[x];
			const char* mime = winpr_image_format_mime(image);
			if (mime && winpr_image_format_is_supported(image))
				mimes[offered++] = mime;
		}
	}

	WINPR_ASSERT(offered <= SHADOW_CLIPBOARD_MAX_OFFER);
	return offered;
}

WINPR_ATTR_NODISCARD
static BOOL shadow_clipboard_set_owner(rdpShadowClipboard* clipboard, rdpShadowClient* client,
                                       const CLIPRDR_FORMAT_LIST* list, const char* const* mimes,
                                       size_t count)
{
	WINPR_ASSERT(clipboard);
	WINPR_ASSERT(client);
	WINPR_ASSERT(list);

	CLIPRDR_FORMAT* formats = calloc(list->numFormats, sizeof(CLIPRDR_FORMAT));
	if (!formats)
		return FALSE;

	for (UINT32 x = 0; x < list->numFormats; x++)
	{
		const CLIPRDR_FORMAT* format = &list->formats[x];
		formats[x].formatId = format->formatId;
		if (format->formatName)
		{
			formats[x].formatName = _strdup(format->formatName);
			if (!formats[x].formatName)
			{
				shadow_clipboard_formats_free(formats, list->numFormats);
				return FALSE;
			}
		}
	}

	shadow_clipboard_cancel_fetches(clipboard, nullptr);
	shadow_clipboard_clear_owner(clipboard);
	clipboard->owner = client;
	clipboard->ownerFormats = formats;
	clipboard->nOwnerFormats = list->numFormats;

	shadow_clipboard_mimes_free(clipboard->sessionMimes, clipboard->nSessionMimes);
	clipboard->sessionMimes = nullptr;
	clipboard->nSessionMimes = 0;
	if (!ClipboardEmpty(clipboard->files))
		WLog_WARN(TAG, "the file list of the session stays");

	shadow_clipboard_announce(clipboard, client);

	rdpShadowSubsystem* subsystem = shadow_clipboard_subsystem(clipboard);
	return subsystem->ClipboardOffer(subsystem, mimes, count);
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_client_format_list(CliprdrServerContext* context,
                                                const CLIPRDR_FORMAT_LIST* list)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(list);

	rdpShadowClient* client = context->custom;
	WINPR_ASSERT(client);

	CLIPRDR_FORMAT_LIST_RESPONSE response = WINPR_C_ARRAY_INIT;
	response.common.msgType = CB_FORMAT_LIST_RESPONSE;
	response.common.msgFlags = CB_RESPONSE_OK;
	const UINT rc = context->ServerFormatListResponse(context, &response);
	if (rc != CHANNEL_RC_OK)
		return rc;

	rdpShadowClipboard* clipboard = client->server->clipboard;
	WINPR_ASSERT(clipboard);

	EnterCriticalSection(&clipboard->lock);
	ShadowClipboardPeer* peer = shadow_clipboard_peer(clipboard, client);
	if (peer && client->mayInteract)
	{
		const BOOL first = !peer->ready;
		peer->ready = TRUE;

		const char* mimes[SHADOW_CLIPBOARD_MAX_OFFER] = WINPR_C_ARRAY_INIT;
		const BOOL files = shadow_clipboard_may_copy_files(clipboard, client);
		const size_t count =
		    shadow_clipboard_list_mimes(list->formats, list->numFormats, files, mimes);
		if (count > 0)
		{
			if (!shadow_clipboard_set_owner(clipboard, client, list, mimes, count))
				WLog_WARN(TAG, "the session did not take the client's clipboard");
		}
		else if (clipboard->owner == client)
		{
			/* its clipboard is empty, or holds nothing the session can use */
			shadow_clipboard_cancel_fetches(clipboard, nullptr);
			shadow_clipboard_release_owner(clipboard);
		}
		else if (first && (clipboard->nSessionMimes > 0))
		{
			/* a client that connects with nothing to offer gets the session's */
			const UINT sent = shadow_clipboard_send_list(clipboard, client);
			if (sent != CHANNEL_RC_OK)
				WLog_WARN(TAG, "ServerFormatList failed with %" PRIu32, sent);
		}
	}
	LeaveCriticalSection(&clipboard->lock);
	return CHANNEL_RC_OK;
}

/* A client's capabilities follow Monitor Ready. A list of its own usually comes right
 * after them, and a client with nothing to offer may send none (FreeRDP's own does not).
 * Our list must not cross one on the wire: it would replace the clipboard the client
 * just announced. The channel's thread runs this: anything to read wakes it, and the
 * list then goes out with shadow_clipboard_client_format_list(). */
WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_client_capabilities(CliprdrServerContext* context,
                                                 WINPR_ATTR_UNUSED const CLIPRDR_CAPABILITIES* caps)
{
	WINPR_ASSERT(context);

	rdpShadowClient* client = context->custom;
	WINPR_ASSERT(client);
	rdpShadowClipboard* clipboard = client->server->clipboard;
	WINPR_ASSERT(clipboard);

	const DWORD waited =
	    WaitForSingleObject(context->GetEventHandle(context), SHADOW_CLIPBOARD_LIST_WAIT_MS);
	if (waited != WAIT_TIMEOUT)
		return CHANNEL_RC_OK;

	EnterCriticalSection(&clipboard->lock);
	ShadowClipboardPeer* peer = shadow_clipboard_peer(clipboard, client);
	if (peer && !peer->ready && client->mayInteract)
	{
		peer->ready = TRUE;
		if (clipboard->nSessionMimes > 0)
		{
			const UINT sent = shadow_clipboard_send_list(clipboard, client);
			if (sent != CHANNEL_RC_OK)
				WLog_WARN(TAG, "ServerFormatList failed with %" PRIu32, sent);
		}
	}
	LeaveCriticalSection(&clipboard->lock);
	return CHANNEL_RC_OK;
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_client_data_request(CliprdrServerContext* context,
                                                 const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(request);

	rdpShadowClient* client = context->custom;
	WINPR_ASSERT(client);
	rdpShadowClipboard* clipboard = client->server->clipboard;
	WINPR_ASSERT(clipboard);

	BOOL queued = FALSE;
	EnterCriticalSection(&clipboard->lock);
	const char* mime = shadow_clipboard_session_mime(clipboard, request->requestedFormatId);
	if (mime && client->mayInteract && shadow_clipboard_peer(clipboard, client))
	{
		ShadowClipboardRead* read = calloc(1, sizeof(ShadowClipboardRead));
		if (read)
		{
			read->client = client;
			read->formatId = request->requestedFormatId;
			read->mime = _strdup(mime);
			queued = read->mime && ArrayList_Append(clipboard->reads, read);
			if (!queued)
				shadow_clipboard_read_free(read);
		}
	}

	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc): fetch owned by clipboard->reads
	if (queued)
		shadow_clipboard_read_next(clipboard);
	LeaveCriticalSection(&clipboard->lock);

	if (!queued)
		shadow_clipboard_respond(context, nullptr, 0);
	return CHANNEL_RC_OK;
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_client_data_response(CliprdrServerContext* context,
                                                  const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(response);

	rdpShadowClient* client = context->custom;
	WINPR_ASSERT(client);
	rdpShadowClipboard* clipboard = client->server->clipboard;
	WINPR_ASSERT(clipboard);

	EnterCriticalSection(&clipboard->lock);
	ShadowClipboardFetch* fetch =
	    clipboard->fetchBusy ? ArrayList_GetItem(clipboard->fetches, 0) : nullptr;
	if (fetch && (fetch->client == client))
	{
		clipboard->fetchBusy = FALSE;

		const BOOL ok = (response->common.msgFlags & CB_RESPONSE_OK) != 0;
		if (fetch->transfer)
			shadow_clipboard_transfer_answer(fetch->transfer, ok, response->requestedFormatData,
			                                 response->common.dataLen);
		else if (!fetch->canceled)
		{
			UINT32 size = 0;
			BYTE* data = nullptr;
			if (ok)
				data = shadow_clipboard_convert(clipboard, fetch->sourceId, fetch->mimeId,
				                                response->requestedFormatData,
				                                response->common.dataLen, &size);
			WLog_DBG(TAG, "client data: %" PRIu32 " bytes, %" PRIu32 " for the session",
			         response->common.dataLen, size);

			/* a Windows string ends in a NUL, text in a pipe does not */
			if (data && fetch->text)
				size = WINPR_ASSERTING_INT_CAST(UINT32, strnlen((const char*)data, size));
			shadow_clipboard_provide(clipboard, fetch->request, data, size);
			free(data);
		}

		if (!ArrayList_RemoveAt(clipboard->fetches, 0))
			WLog_WARN(TAG, "cannot drop a clipboard request");
		shadow_clipboard_fetch_next(clipboard);
	}
	LeaveCriticalSection(&clipboard->lock);
	return CHANNEL_RC_OK;
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_send_file_answer(wClipboardDelegate* delegate, UINT32 streamId,
                                              BOOL ok, const BYTE* data, UINT32 size)
{
	WINPR_ASSERT(delegate);

	const rdpShadowClipboard* clipboard = delegate->custom;
	WINPR_ASSERT(clipboard);
	rdpShadowClient* client = clipboard->filesClient;
	WINPR_ASSERT(client);
	CliprdrServerContext* cliprdr = client->cliprdr;
	WINPR_ASSERT(cliprdr);

	CLIPRDR_FILE_CONTENTS_RESPONSE response = WINPR_C_ARRAY_INIT;
	response.common.msgType = CB_FILECONTENTS_RESPONSE;
	response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
	response.streamId = streamId;
	response.cbRequested = size;
	response.requestedData = data;
	return cliprdr->ServerFileContentsResponse(cliprdr, &response);
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_file_size_success(wClipboardDelegate* delegate,
                                               const wClipboardFileSizeRequest* request,
                                               UINT64 fileSize)
{
	WINPR_ASSERT(request);

	BYTE size[8] = WINPR_C_ARRAY_INIT;
	winpr_Data_Write_UINT64(size, fileSize);
	return shadow_clipboard_send_file_answer(delegate, request->streamId, TRUE, size, sizeof(size));
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_file_size_failure(wClipboardDelegate* delegate,
                                               const wClipboardFileSizeRequest* request,
                                               WINPR_ATTR_UNUSED UINT errorCode)
{
	WINPR_ASSERT(request);
	return shadow_clipboard_send_file_answer(delegate, request->streamId, FALSE, nullptr, 0);
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_file_range_success(wClipboardDelegate* delegate,
                                                const wClipboardFileRangeRequest* request,
                                                const BYTE* data, UINT32 size)
{
	WINPR_ASSERT(request);
	return shadow_clipboard_send_file_answer(delegate, request->streamId, TRUE, data, size);
}

WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_file_range_failure(wClipboardDelegate* delegate,
                                                const wClipboardFileRangeRequest* request,
                                                WINPR_ATTR_UNUSED UINT errorCode)
{
	WINPR_ASSERT(request);
	return shadow_clipboard_send_file_answer(delegate, request->streamId, FALSE, nullptr, 0);
}

/* A client reads the session's files. The list is the one it fetched: when the session's
 * clipboard changes, wClipboard fails the request. */
WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_client_file_request(CliprdrServerContext* context,
                                                 const CLIPRDR_FILE_CONTENTS_REQUEST* request)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(request);

	rdpShadowClient* client = context->custom;
	WINPR_ASSERT(client);
	rdpShadowClipboard* clipboard = client->server->clipboard;
	WINPR_ASSERT(clipboard);

	UINT rc = ERROR_ACCESS_DENIED;
	EnterCriticalSection(&clipboard->lock);
	if (client->mayInteract && shadow_clipboard_peer(clipboard, client) &&
	    shadow_clipboard_may_copy_files(clipboard, client))
	{
		wClipboardDelegate* delegate = ClipboardGetDelegate(clipboard->files);
		WINPR_ASSERT(delegate);

		clipboard->filesClient = client;
		if (request->dwFlags == FILECONTENTS_SIZE)
		{
			const wClipboardFileSizeRequest size = { .streamId = request->streamId,
				                                     .listIndex = request->listIndex };
			rc = delegate->ClientRequestFileSize(delegate, &size);
		}
		else if (request->dwFlags == FILECONTENTS_RANGE)
		{
			/* an answer may be shorter: the client asks for the rest */
			const wClipboardFileRangeRequest range = {
				.streamId = request->streamId,
				.listIndex = request->listIndex,
				.nPositionLow = request->nPositionLow,
				.nPositionHigh = request->nPositionHigh,
				.cbRequested = MIN(request->cbRequested, SHADOW_CLIPBOARD_FILE_MAX_ANSWER)
			};
			rc = delegate->ClientRequestFileRange(delegate, &range);
		}
		else
			rc = ERROR_INVALID_PARAMETER;

		if (rc != CHANNEL_RC_OK)
		{
			const UINT sent =
			    shadow_clipboard_send_file_answer(delegate, request->streamId, FALSE, nullptr, 0);
			if (sent != CHANNEL_RC_OK)
				WLog_WARN(TAG, "ServerFileContentsResponse failed with %" PRIu32, sent);
		}
		clipboard->filesClient = nullptr;
	}
	LeaveCriticalSection(&clipboard->lock);

	if (rc == ERROR_ACCESS_DENIED)
	{
		CLIPRDR_FILE_CONTENTS_RESPONSE refused = WINPR_C_ARRAY_INIT;
		refused.common.msgType = CB_FILECONTENTS_RESPONSE;
		refused.common.msgFlags = CB_RESPONSE_FAIL;
		refused.streamId = request->streamId;
		return context->ServerFileContentsResponse(context, &refused);
	}
	return CHANNEL_RC_OK;
}

/* The owner sends a piece of a file. An answer that nobody waits for is dropped. */
WINPR_ATTR_NODISCARD
static UINT shadow_clipboard_client_file_response(CliprdrServerContext* context,
                                                  const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(response);

	rdpShadowClient* client = context->custom;
	WINPR_ASSERT(client);
	rdpShadowClipboard* clipboard = client->server->clipboard;
	WINPR_ASSERT(clipboard);

	EnterCriticalSection(&clipboard->lock);
	ShadowClipboardTransfer* transfer = clipboard->transfer;
	if (transfer && (transfer->client == client) && (response->streamId == transfer->streamId) &&
	    !transfer->answered)
	{
		const BOOL ok = (response->common.msgFlags & CB_RESPONSE_OK) != 0;
		shadow_clipboard_transfer_answer(transfer, ok, response->requestedData,
		                                 response->cbRequested);
	}
	LeaveCriticalSection(&clipboard->lock);
	return CHANNEL_RC_OK;
}

#endif /* CHANNEL_CLIPRDR_SERVER */

BOOL shadow_clipboard_session_changed(rdpShadowServer* server, const char* const* mimeTypes,
                                      size_t count)
{
	WINPR_ASSERT(server);
	WINPR_ASSERT(mimeTypes || (count == 0));

#if defined(CHANNEL_CLIPRDR_SERVER)
	rdpShadowClipboard* clipboard = server->clipboard;
	if (!clipboard)
		return FALSE;

	char** mimes = nullptr;
	if (count > 0)
	{
		mimes = (char**)calloc(count, sizeof(char*));
		if (!mimes)
			return FALSE;
		for (size_t x = 0; x < count; x++)
		{
			mimes[x] = _strdup(mimeTypes[x]);
			if (!mimes[x])
			{
				shadow_clipboard_mimes_free(mimes, count);
				return FALSE;
			}
		}
	}

	EnterCriticalSection(&clipboard->lock);
	shadow_clipboard_cancel_fetches(clipboard, nullptr);
	shadow_clipboard_clear_owner(clipboard);
	shadow_clipboard_mimes_free(clipboard->sessionMimes, clipboard->nSessionMimes);
	clipboard->sessionMimes = mimes;
	clipboard->nSessionMimes = count;
	if (!ClipboardEmpty(clipboard->files))
		WLog_WARN(TAG, "the file list of the session stays");
	shadow_clipboard_announce(clipboard, nullptr);
	LeaveCriticalSection(&clipboard->lock);
	return TRUE;
#else
	return FALSE;
#endif
}

BOOL shadow_clipboard_session_data(rdpShadowServer* server, const BYTE* data, size_t size)
{
	WINPR_ASSERT(server);

#if defined(CHANNEL_CLIPRDR_SERVER)
	rdpShadowClipboard* clipboard = server->clipboard;
	if (!clipboard)
		return FALSE;

	BOOL rc = FALSE;
	EnterCriticalSection(&clipboard->lock);
	const ShadowClipboardRead* read =
	    clipboard->readBusy ? ArrayList_GetItem(clipboard->reads, 0) : nullptr;
	if (read)
	{
		rc = TRUE;
		clipboard->readBusy = FALSE;

		if (read->client)
		{
			const UINT32 mimeId = ClipboardRegisterFormat(clipboard->convert, read->mime);
			UINT32 converted = 0;
			BYTE* payload = nullptr;
			if (read->formatId == clipboard->filesId)
				payload = shadow_clipboard_file_list(clipboard, read->client, read->mime, data,
				                                     size, &converted);
			else
				payload = shadow_clipboard_convert(clipboard, mimeId, read->formatId, data, size,
				                                   &converted);
			WLog_DBG(TAG, "session data: %" PRIuz " bytes of %s, %" PRIu32 " for the client", size,
			         read->mime, converted);
			shadow_clipboard_respond(read->client->cliprdr, payload, converted);
			free(payload);
		}

		if (!ArrayList_RemoveAt(clipboard->reads, 0))
			WLog_WARN(TAG, "cannot drop a clipboard read");
		shadow_clipboard_read_next(clipboard);
	}
	LeaveCriticalSection(&clipboard->lock);
	return rc;
#else
	WINPR_UNUSED(data);
	WINPR_UNUSED(size);
	return FALSE;
#endif
}

BOOL shadow_clipboard_session_request(rdpShadowServer* server, UINT32 request, const char* mimeType)
{
	WINPR_ASSERT(server);
	WINPR_ASSERT(mimeType);

#if defined(CHANNEL_CLIPRDR_SERVER)
	rdpShadowClipboard* clipboard = server->clipboard;
	if (!clipboard)
		return FALSE;

	EnterCriticalSection(&clipboard->lock);

	/* an owner that stopped answering */
	ShadowClipboardFetch* first =
	    clipboard->fetchBusy ? ArrayList_GetItem(clipboard->fetches, 0) : nullptr;
	if (first && (GetTickCount64() - first->sent >= SHADOW_CLIPBOARD_FETCH_TIMEOUT_MS))
	{
		WLog_WARN(TAG, "no clipboard data from the client");
		clipboard->fetchBusy = FALSE;
		shadow_clipboard_fetch_failed(clipboard, first);
		if (!ArrayList_RemoveAt(clipboard->fetches, 0))
			WLog_WARN(TAG, "cannot drop a clipboard request");
	}

	const BOOL gnomeFiles = strcmp(mimeType, SHADOW_CLIPBOARD_MIME_GNOME_FILES) == 0;
	if (gnomeFiles || (strcmp(mimeType, SHADOW_CLIPBOARD_MIME_URI_LIST) == 0))
	{
		shadow_clipboard_paste_files(clipboard, request, gnomeFiles);
		LeaveCriticalSection(&clipboard->lock);
		return TRUE;
	}

	const CLIPRDR_FORMAT* source = nullptr;
	if (clipboard->owner)
	{
		const CLIPRDR_FORMAT* formats = clipboard->ownerFormats;
		const UINT32 count = clipboard->nOwnerFormats;
		if (strcmp(mimeType, SHADOW_CLIPBOARD_MIME_HTML) == 0)
			source = shadow_clipboard_find_format(formats, count, 0, SHADOW_CLIPBOARD_FORMAT_HTML);
		else if (strncmp(mimeType, "image/", 6) == 0)
			source = shadow_clipboard_owner_image(formats, count);
		else
			source = shadow_clipboard_owner_text(formats, count);
	}

	BOOL queued = FALSE;
	if (source)
	{
		ShadowClipboardFetch* fetch = calloc(1, sizeof(ShadowClipboardFetch));
		if (fetch)
		{
			fetch->client = clipboard->owner;
			fetch->request = request;
			fetch->formatId = source->formatId;
			fetch->sourceId = source->formatName
			                      ? ClipboardRegisterFormat(clipboard->convert, source->formatName)
			                      : source->formatId;
			fetch->mimeId = ClipboardRegisterFormat(clipboard->convert, mimeType);
			fetch->text = strncmp(mimeType, "image/", 6) != 0;
			queued = ArrayList_Append(clipboard->fetches, fetch);
			if (!queued)
				free(fetch);
		}
	}

	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc): fetch owned by clipboard->fetches
	if (queued)
		shadow_clipboard_fetch_next(clipboard);
	else
		shadow_clipboard_provide(clipboard, request, nullptr, 0);
	LeaveCriticalSection(&clipboard->lock);
	return TRUE;
#else
	WINPR_UNUSED(request);
	return FALSE;
#endif
}

#if defined(CHANNEL_CLIPRDR_SERVER)

rdpShadowClipboard* shadow_clipboard_new(rdpShadowServer* server)
{
	WINPR_ASSERT(server);

	rdpShadowClipboard* clipboard = calloc(1, sizeof(rdpShadowClipboard));
	if (!clipboard)
		return nullptr;

	clipboard->server = server;
	if (!InitializeCriticalSectionAndSpinCount(&clipboard->lock, 4000))
	{
		free(clipboard);
		return nullptr;
	}

	clipboard->convert = ClipboardCreate();
	clipboard->files = ClipboardCreate();
	clipboard->peers = ArrayList_New(FALSE);
	clipboard->reads = ArrayList_New(FALSE);
	clipboard->fetches = ArrayList_New(FALSE);
	clipboard->retired = ArrayList_New(FALSE);
	if (!clipboard->convert || !clipboard->files || !clipboard->peers || !clipboard->reads ||
	    !clipboard->fetches || !clipboard->retired)
		goto fail;

	wObject* peers = ArrayList_Object(clipboard->peers);
	WINPR_ASSERT(peers);
	peers->fnObjectFree = free;
	wObject* reads = ArrayList_Object(clipboard->reads);
	WINPR_ASSERT(reads);
	reads->fnObjectFree = shadow_clipboard_read_free;
	wObject* fetches = ArrayList_Object(clipboard->fetches);
	WINPR_ASSERT(fetches);
	fetches->fnObjectFree = free;
	wObject* retired = ArrayList_Object(clipboard->retired);
	WINPR_ASSERT(retired);
	retired->fnObjectFree = shadow_clipboard_transfer_free;

	clipboard->htmlId = ClipboardRegisterFormat(clipboard->convert, SHADOW_CLIPBOARD_FORMAT_HTML);
	clipboard->filesId = ClipboardRegisterFormat(clipboard->files, SHADOW_CLIPBOARD_FORMAT_FILES);
	if ((clipboard->htmlId == 0) || (clipboard->filesId == 0))
		goto fail;

	wClipboardDelegate* delegate = ClipboardGetDelegate(clipboard->files);
	WINPR_ASSERT(delegate);
	delegate->custom = clipboard;
	delegate->ClipboardFileSizeSuccess = shadow_clipboard_file_size_success;
	delegate->ClipboardFileSizeFailure = shadow_clipboard_file_size_failure;
	delegate->ClipboardFileRangeSuccess = shadow_clipboard_file_range_success;
	delegate->ClipboardFileRangeFailure = shadow_clipboard_file_range_failure;
	return clipboard;

fail:
	shadow_clipboard_free(clipboard);
	return nullptr;
}

void shadow_clipboard_free(rdpShadowClipboard* clipboard)
{
	if (!clipboard)
		return;

	/* the clients are gone, and the last transfer with its owner: this only waits */
	if (clipboard->retired)
	{
		EnterCriticalSection(&clipboard->lock);
		shadow_clipboard_transfer_drop(clipboard);
		const size_t count = ArrayList_Count(clipboard->retired);
		LeaveCriticalSection(&clipboard->lock);

		for (size_t x = 0; x < count; x++)
		{
			const ShadowClipboardTransfer* transfer = ArrayList_GetItem(clipboard->retired, x);
			if (transfer && (WaitForSingleObject(transfer->thread, INFINITE) != WAIT_OBJECT_0))
				WLog_WARN(TAG, "cannot wait for a file transfer");
		}
		ArrayList_Free(clipboard->retired);
	}

	ArrayList_Free(clipboard->fetches);
	ArrayList_Free(clipboard->reads);
	ArrayList_Free(clipboard->peers);
	shadow_clipboard_formats_free(clipboard->ownerFormats, clipboard->nOwnerFormats);
	shadow_clipboard_mimes_free(clipboard->sessionMimes, clipboard->nSessionMimes);
	ClipboardDestroy(clipboard->files);
	ClipboardDestroy(clipboard->convert);
	DeleteCriticalSection(&clipboard->lock);
	free(clipboard);
}

int shadow_client_cliprdr_init(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->server);

	rdpShadowClipboard* clipboard = client->server->clipboard;
	const rdpShadowSubsystem* subsystem = client->subsystem;
	WINPR_ASSERT(subsystem);

	if (!clipboard || !subsystem->ClipboardOffer || !subsystem->ClipboardRead ||
	    !subsystem->ClipboardProvide)
		return 0;

	CliprdrServerContext* cliprdr = cliprdr_server_context_new(client->vcm);
	if (!cliprdr)
		return -1;

	cliprdr->custom = client;
	cliprdr->rdpcontext = &client->context;
	cliprdr->autoInitializationSequence = TRUE;
	cliprdr->useLongFormatNames = TRUE;
	if (subsystem->ClipboardFiles)
	{
		/* each then falls back to what the client's capabilities say */
		cliprdr->streamFileClipEnabled = TRUE;
		cliprdr->fileClipNoFilePaths = TRUE;
		cliprdr->hasHugeFileSupport = TRUE;
	}
	cliprdr->ClientCapabilities = shadow_clipboard_client_capabilities;
	cliprdr->ClientFormatList = shadow_clipboard_client_format_list;
	cliprdr->ClientFormatDataRequest = shadow_clipboard_client_data_request;
	cliprdr->ClientFormatDataResponse = shadow_clipboard_client_data_response;
	cliprdr->ClientFileContentsRequest = shadow_clipboard_client_file_request;
	cliprdr->ClientFileContentsResponse = shadow_clipboard_client_file_response;

	ShadowClipboardPeer* peer = calloc(1, sizeof(ShadowClipboardPeer));
	if (!peer)
	{
		cliprdr_server_context_free(cliprdr);
		return -1;
	}
	peer->client = client;
	client->cliprdr = cliprdr;

	EnterCriticalSection(&clipboard->lock);
	const BOOL added = ArrayList_Append(clipboard->peers, peer);
	LeaveCriticalSection(&clipboard->lock);
	if (!added)
	{
		free(peer);
		client->cliprdr = nullptr;
		cliprdr_server_context_free(cliprdr);
		return -1;
	}

	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc): peer owned by clipboard->peers
	const UINT rc = cliprdr->Start(cliprdr);
	if (rc != CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "cannot start the clipboard channel: %" PRIu32, rc);
		return -1; /* shadow_client_cliprdr_uninit() cleans up */
	}
	return 1;
}

void shadow_client_cliprdr_uninit(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->server);

	CliprdrServerContext* cliprdr = client->cliprdr;
	if (!cliprdr)
		return;

	rdpShadowClipboard* clipboard = client->server->clipboard;
	WINPR_ASSERT(clipboard);

	EnterCriticalSection(&clipboard->lock);
	ShadowClipboardPeer* peer = shadow_clipboard_peer(clipboard, client);
	if (peer && !ArrayList_Remove(clipboard->peers, peer))
		WLog_WARN(TAG, "cannot drop a client of the clipboard");

	for (size_t x = ArrayList_Count(clipboard->reads); x > 0; x--)
	{
		const size_t index = x - 1;
		ShadowClipboardRead* read = ArrayList_GetItem(clipboard->reads, index);
		WINPR_ASSERT(read);
		if (read->client != client)
			continue;

		/* the one at the subsystem stays until its answer */
		if ((index == 0) && clipboard->readBusy)
			read->client = nullptr;
		else if (!ArrayList_RemoveAt(clipboard->reads, index))
			WLog_WARN(TAG, "cannot drop a clipboard read");
	}

	if (clipboard->owner == client)
	{
		shadow_clipboard_cancel_fetches(clipboard, client);
		shadow_clipboard_release_owner(clipboard);
	}
	else if (clipboard->fetchBusy)
	{
		/* an answer it still owed from when it was the owner */
		const ShadowClipboardFetch* fetch = ArrayList_GetItem(clipboard->fetches, 0);
		if (fetch && (fetch->client == client))
		{
			clipboard->fetchBusy = FALSE;
			if (!ArrayList_RemoveAt(clipboard->fetches, 0))
				WLog_WARN(TAG, "cannot drop a clipboard request");
			shadow_clipboard_fetch_next(clipboard);
		}
	}
	LeaveCriticalSection(&clipboard->lock);

	/* outside the lock: its thread may wait for it */
	const UINT rc = cliprdr->Stop(cliprdr);
	if (rc != CHANNEL_RC_OK)
		WLog_WARN(TAG, "cannot stop the clipboard channel: %" PRIu32, rc);
	cliprdr_server_context_free(cliprdr);
	client->cliprdr = nullptr;
}

#else /* CHANNEL_CLIPRDR_SERVER */

rdpShadowClipboard* shadow_clipboard_new(WINPR_ATTR_UNUSED rdpShadowServer* server)
{
	return nullptr;
}

void shadow_clipboard_free(WINPR_ATTR_UNUSED rdpShadowClipboard* clipboard)
{
}

int shadow_client_cliprdr_init(WINPR_ATTR_UNUSED rdpShadowClient* client)
{
	return 0;
}

void shadow_client_cliprdr_uninit(WINPR_ATTR_UNUSED rdpShadowClient* client)
{
}

#endif /* CHANNEL_CLIPRDR_SERVER */
