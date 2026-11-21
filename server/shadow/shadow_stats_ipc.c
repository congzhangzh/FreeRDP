/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server: the GFX counters on a local socket (/stats-ipc)
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

/* struct ucred (SO_PEERCRED) */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <freerdp/config.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <afunix.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/synch.h>
#include <winpr/interlocked.h>
#include <winpr/thread.h>
#include <winpr/sysinfo.h>
#include <winpr/string.h>
#include <winpr/debug.h>

#include "shadow_stats_ipc.h"

#if defined(_WIN32)
typedef SOCKET NATIVE_SOCKET;
#define SHADOW_STATS_INVALID INVALID_SOCKET
typedef WSAPOLLFD SHADOW_POLLFD;
/* WSAPoll waits on sockets only: the thread looks for new lines this often */
#define SHADOW_STATS_WAIT_MS 50
#else
typedef int NATIVE_SOCKET;
#define SHADOW_STATS_INVALID (-1)
typedef struct pollfd SHADOW_POLLFD;
#define SHADOW_STATS_WAIT_MS (-1)
#endif

typedef struct
{
	NATIVE_SOCKET socket;
	char* line; /* the line it gets, with its line end */
	size_t length;
	size_t sent;
	UINT64 seq;
} SHADOW_STATS_READER;

typedef struct
{
	UINT64 connectionId;
	char* object;
} SHADOW_STATS_CLIENT;

struct s_shadow_stats_ipc
{
	char* path;
	NATIVE_SOCKET listener;
#if !defined(_WIN32)
	int wake[2];
#endif
	HANDLE thread;
	volatile LONG stop;
	volatile LONG readerCount;

	CRITICAL_SECTION lock; /* latest, latestLength, latestSeq, clients, lineSeq */
	char* latest;          /* with its line end, nullptr before the first */
	size_t latestLength;
	UINT64 latestSeq;
	SHADOW_STATS_CLIENT clients[SHADOW_STATS_IPC_MAX_CLIENTS];
	size_t clientCount;
	UINT64 lineSeq;

	SHADOW_STATS_READER readers[SHADOW_STATS_IPC_MAX_READERS];
	size_t readersUsed; /* the socket thread's */
};

static void close_socket(NATIVE_SOCKET s)
{
#if defined(_WIN32)
	closesocket(s);
#else
	close(s);
#endif
}

static BOOL would_block(void)
{
#if defined(_WIN32)
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return (errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR);
#endif
}

static BOOL set_non_blocking(NATIVE_SOCKET s)
{
#if defined(_WIN32)
	u_long on = 1;
	return ioctlsocket(s, FIONBIO, &on) == 0;
#else
	const int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0)
		return FALSE;
	if (fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0)
		return FALSE;
	return fcntl(s, F_SETFD, FD_CLOEXEC) == 0;
#endif
}

static const char* last_error(char* buffer, size_t size)
{
#if defined(_WIN32)
	(void)_snprintf(buffer, size, "error %d", WSAGetLastError());
	return buffer;
#else
	return winpr_strerror(errno, buffer, size);
#endif
}

static void set_error(char* error, size_t size, const char* format, const char* a, const char* b)
{
	if (error && (size > 0))
		(void)_snprintf(error, size, format, a ? a : "", b ? b : "");
}

static BOOL fill_address(const char* path, struct sockaddr_un* address, char* error, size_t size)
{
	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	const size_t length = path ? strlen(path) : 0;
	if ((length == 0) || (length >= sizeof(address->sun_path)))
	{
		char limit[32] = WINPR_C_ARRAY_INIT;
		(void)_snprintf(limit, sizeof(limit), "%" PRIuz, sizeof(address->sun_path) - 1);
		set_error(error, size, "the socket path must have 1 to %s bytes: %s", limit, path);
		return FALSE;
	}
	memcpy(address->sun_path, path, length + 1);
	return TRUE;
}

/* someone listens on path */
static BOOL listening(const char* path)
{
	struct sockaddr_un address;
	if (!fill_address(path, &address, nullptr, 0))
		return FALSE;
	const NATIVE_SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s == SHADOW_STATS_INVALID)
		return FALSE;
	const BOOL up = connect(s, (const struct sockaddr*)&address, sizeof(address)) == 0;
	close_socket(s);
	return up;
}

/* Makes room for the socket at path, or says why not. */
static BOOL prepare(const char* path, char* error, size_t size)
{
#if defined(_WIN32)
	/* the directory is the user's own (%LOCALAPPDATA% by default); its ACL keeps others out */
	WIN32_FIND_DATAA data = WINPR_C_ARRAY_INIT;
	HANDLE find = FindFirstFileA(path, &data);
	if (find == INVALID_HANDLE_VALUE)
		return TRUE;
	FindClose(find);
	const BOOL isSocket = ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) &&
	                      (data.dwReserved0 == IO_REPARSE_TAG_AF_UNIX);
	if (!isSocket)
	{
		set_error(error, size, "not replacing a file that is no socket: %s%s", path, nullptr);
		return FALSE;
	}
	if (listening(path))
	{
		set_error(error, size, "another server listens on %s%s", path, nullptr);
		return FALSE;
	}
	if (!DeleteFileA(path))
	{
		set_error(error, size, "the old socket could not be removed: %s%s", path, nullptr);
		return FALSE;
	}
	return TRUE;
#else
	char reason[128] = WINPR_C_ARRAY_INIT;
	const uid_t me = geteuid();
	char* parent = nullptr;
	const char* slash = strrchr(path, '/');
	if (!slash)
		parent = _strdup(".");
	else if (slash == path)
		parent = _strdup("/");
	else
		parent = strndup(path, (size_t)(slash - path));
	if (!parent)
	{
		set_error(error, size, "out of memory%s%s", nullptr, nullptr);
		return FALSE;
	}

	struct stat dir = WINPR_C_ARRAY_INIT;
	if (stat(parent, &dir) != 0)
	{
		set_error(error, size, "no directory %s: %s", parent, last_error(reason, sizeof(reason)));
		free(parent);
		return FALSE;
	}
	/* nobody else may put or replace a file there, unless the sticky bit keeps ours ours */
	const BOOL owner = (dir.st_uid == me) || (dir.st_uid == 0);
	const BOOL othersWrite = (dir.st_mode & (S_IWGRP | S_IWOTH)) != 0;
	if (!S_ISDIR(dir.st_mode) || !owner || (othersWrite && ((dir.st_mode & S_ISVTX) == 0)))
	{
		set_error(error, size,
		          "the directory %s must be this user's and not writable by others (or sticky)%s",
		          parent, nullptr);
		free(parent);
		return FALSE;
	}
	free(parent);

	struct stat old = WINPR_C_ARRAY_INIT;
	if (lstat(path, &old) != 0)
	{
		if (errno == ENOENT)
			return TRUE;
		set_error(error, size, "cannot look at %s: %s", path, last_error(reason, sizeof(reason)));
		return FALSE;
	}
	if (!S_ISSOCK(old.st_mode) || (old.st_uid != me))
	{
		set_error(error, size, "not replacing a file that is no socket of this user: %s%s", path,
		          nullptr);
		return FALSE;
	}
	if (listening(path))
	{
		set_error(error, size, "another server listens on %s%s", path, nullptr);
		return FALSE;
	}
	if (unlink(path) != 0)
	{
		set_error(error, size, "the old socket could not be removed: %s: %s", path,
		          last_error(reason, sizeof(reason)));
		return FALSE;
	}
	return TRUE;
#endif
}

static void remove_path(const char* path)
{
#if defined(_WIN32)
	(void)DeleteFileA(path);
#else
	(void)unlink(path);
#endif
}

/* the reader runs as this user */
static BOOL same_user(NATIVE_SOCKET s)
{
#if defined(_WIN32)
	WINPR_UNUSED(s);
	return TRUE;
#elif defined(SO_PEERCRED)
	struct ucred cred = WINPR_C_ARRAY_INIT;
	socklen_t len = sizeof(cred);
	if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
		return FALSE;
	return cred.uid == geteuid();
#else
	uid_t uid = 0;
	gid_t gid = 0;
	if (getpeereid(s, &uid, &gid) != 0)
		return FALSE;
	return uid == geteuid();
#endif
}

/* a write to a reader that is gone must not end the process */
static void no_sigpipe(NATIVE_SOCKET s)
{
#if defined(SO_NOSIGPIPE)
	const int on = 1;
	(void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
	WINPR_UNUSED(s);
#endif
}

static int send_flags(void)
{
#if defined(MSG_NOSIGNAL)
	return MSG_NOSIGNAL;
#else
	return 0;
#endif
}

static void wake(SHADOW_STATS_IPC* ipc)
{
#if !defined(_WIN32)
	if (ipc->wake[1] >= 0)
	{
		const char c = 0;
		/* a full pipe already wakes the thread */
		const ssize_t rc = write(ipc->wake[1], &c, 1);
		WINPR_UNUSED(rc);
	}
#else
	WINPR_UNUSED(ipc);
#endif
}

static void drop_reader(SHADOW_STATS_IPC* ipc, size_t index)
{
	SHADOW_STATS_READER* reader = &ipc->readers[index];
	close_socket(reader->socket);
	free(reader->line);
	ipc->readersUsed--;
	if (index != ipc->readersUsed)
		ipc->readers[index] = ipc->readers[ipc->readersUsed];
	memset(&ipc->readers[ipc->readersUsed], 0, sizeof(ipc->readers[ipc->readersUsed]));
}

/* each reader that is done with its line gets the newest one */
static void hand_out(SHADOW_STATS_IPC* ipc)
{
	EnterCriticalSection(&ipc->lock);
	for (size_t x = 0; x < ipc->readersUsed; x++)
	{
		SHADOW_STATS_READER* reader = &ipc->readers[x];
		if ((reader->sent != reader->length) || (reader->seq >= ipc->latestSeq) || !ipc->latest)
			continue;
		char* copy = malloc(ipc->latestLength);
		if (!copy)
			continue;
		memcpy(copy, ipc->latest, ipc->latestLength);
		free(reader->line);
		reader->line = copy;
		reader->length = ipc->latestLength;
		reader->sent = 0;
		reader->seq = ipc->latestSeq;
	}
	LeaveCriticalSection(&ipc->lock);
}

static BOOL serve_reader(SHADOW_STATS_READER* reader, short revents)
{
	if ((revents & (POLLERR | POLLNVAL)) != 0)
		return FALSE;
	if ((revents & (POLLIN | POLLHUP)) != 0)
	{
		/* what a reader sends means nothing; its end closes it */
		char buffer[256];
		const int n = (int)recv(reader->socket, buffer, sizeof(buffer), 0);
		if ((n == 0) || ((n < 0) && !would_block()))
			return FALSE;
	}
	if (((revents & POLLOUT) != 0) && (reader->sent < reader->length))
	{
		const size_t left = reader->length - reader->sent;
#if defined(_WIN32)
		/* a line is a few KiB; send() takes an int there */
		const int chunk = (left > (size_t)INT32_MAX) ? INT32_MAX : (int)left;
		const int n = send(reader->socket, reader->line + reader->sent, chunk, send_flags());
#else
		const ssize_t n = send(reader->socket, reader->line + reader->sent, left, send_flags());
#endif
		if (n > 0)
			reader->sent += (size_t)n;
		else if (!would_block())
			return FALSE;
	}
	return TRUE;
}

static void accept_readers(SHADOW_STATS_IPC* ipc)
{
	for (;;)
	{
		const NATIVE_SOCKET s = accept(ipc->listener, nullptr, nullptr);
		if (s == SHADOW_STATS_INVALID)
			break;
		/* a full house or another user: closed at once, so the reader sees its end */
		if ((ipc->readersUsed >= SHADOW_STATS_IPC_MAX_READERS) || !same_user(s) ||
		    !set_non_blocking(s))
		{
			close_socket(s);
			continue;
		}
		no_sigpipe(s);
		SHADOW_STATS_READER* reader = &ipc->readers[ipc->readersUsed++];
		memset(reader, 0, sizeof(*reader));
		reader->socket = s;
	}
}

static DWORD WINAPI shadow_stats_ipc_thread(LPVOID arg)
{
	SHADOW_STATS_IPC* ipc = arg;
	WINPR_ASSERT(ipc);

	SHADOW_POLLFD fds[2 + SHADOW_STATS_IPC_MAX_READERS];
	while (InterlockedCompareExchange(&ipc->stop, 0, 0) == 0)
	{
		hand_out(ipc);

		size_t count = 0;
		memset(fds, 0, sizeof(fds));
		fds[count].fd = ipc->listener;
		fds[count++].events = POLLIN;
#if !defined(_WIN32)
		fds[count].fd = ipc->wake[0];
		fds[count++].events = POLLIN;
#endif
		const size_t first = count;
		for (size_t x = 0; x < ipc->readersUsed; x++)
		{
			fds[count].fd = ipc->readers[x].socket;
			fds[count].events = POLLIN;
			if (ipc->readers[x].sent < ipc->readers[x].length)
				fds[count].events |= POLLOUT;
			count++;
		}

#if defined(_WIN32)
		const int rc = WSAPoll(fds, (ULONG)count, SHADOW_STATS_WAIT_MS);
#else
		const int rc = poll(fds, (nfds_t)count, SHADOW_STATS_WAIT_MS);
#endif
		if (rc < 0)
		{
			if (would_block())
				continue;
			break;
		}
		if (InterlockedCompareExchange(&ipc->stop, 0, 0) != 0)
			break;

#if !defined(_WIN32)
		if ((fds[1].revents & POLLIN) != 0)
		{
			char buffer[64];
			while (read(ipc->wake[0], buffer, sizeof(buffer)) > 0)
			{
			}
		}
#endif

		/* backwards, so a dropped reader's place taken by the last one was already served */
		for (size_t x = ipc->readersUsed; x > 0; x--)
		{
			if (!serve_reader(&ipc->readers[x - 1], fds[first + x - 1].revents))
				drop_reader(ipc, x - 1);
		}

		if ((fds[0].revents & POLLIN) != 0)
			accept_readers(ipc);
		{
		const LONG previous = InterlockedExchange(&ipc->readerCount, (LONG)ipc->readersUsed);
		WINPR_UNUSED(previous);
	}
	}
	return 0;
}

BOOL shadow_stats_ipc_default_path(char* path, size_t size, char* error, size_t errorSize)
{
	if (!path || (size == 0))
		return FALSE;
	char name[64] = WINPR_C_ARRAY_INIT;
	(void)_snprintf(name, sizeof(name), "freerdp-shadow-%" PRIu32 ".stats.sock",
	                (UINT32)GetCurrentProcessId());
#if defined(_WIN32)
	const char* base = getenv("LOCALAPPDATA");
	if (!base || (*base == '\0'))
	{
		set_error(error, errorSize, "LOCALAPPDATA is not set: give /stats-ipc a path%s%s",
		          nullptr, nullptr);
		return FALSE;
	}
	char dir[MAX_PATH] = WINPR_C_ARRAY_INIT;
	(void)_snprintf(dir, sizeof(dir), "%s\\FreeRDP", base);
	if (!CreateDirectoryA(dir, nullptr) && (GetLastError() != ERROR_ALREADY_EXISTS))
	{
		set_error(error, errorSize, "cannot create %s%s", dir, nullptr);
		return FALSE;
	}
	const int rc = _snprintf(path, size, "%s\\%s", dir, name);
	return (rc > 0) && ((size_t)rc < size);
#else
	/* XDG_RUNTIME_DIR is the user's own (0700); macOS has none, its TMPDIR is per user */
	const char* variables[] = { "XDG_RUNTIME_DIR", "TMPDIR" };
	for (size_t x = 0; x < ARRAYSIZE(variables); x++)
	{
		// NOLINTNEXTLINE(concurrency-mt-unsafe)
		const char* base = getenv(variables[x]);
		if (!base || (*base == '\0'))
			continue;
		size_t length = strlen(base);
		while ((length > 1) && (base[length - 1] == '/'))
			length--;
		const int rc = _snprintf(path, size, "%.*s/%s", (int)length, base, name);
		return (rc > 0) && ((size_t)rc < size);
	}
	set_error(error, errorSize,
	          "neither XDG_RUNTIME_DIR nor TMPDIR is set: give /stats-ipc a path%s%s", nullptr,
	          nullptr);
	return FALSE;
#endif
}

SHADOW_STATS_IPC* shadow_stats_ipc_open(const char* path, char* error, size_t errorSize)
{
#if defined(_WIN32)
	WSADATA wsa = WINPR_C_ARRAY_INIT;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		set_error(error, errorSize, "WSAStartup failed%s%s", nullptr, nullptr);
		return nullptr;
	}
#endif
	char reason[128] = WINPR_C_ARRAY_INIT;
	struct sockaddr_un address;
	SHADOW_STATS_IPC* ipc = nullptr;
	NATIVE_SOCKET s = SHADOW_STATS_INVALID;
	BOOL bound = FALSE;

	if (!fill_address(path, &address, error, errorSize) || !prepare(path, error, errorSize))
		goto fail;

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s == SHADOW_STATS_INVALID)
	{
		set_error(error, errorSize, "socket: %s%s", last_error(reason, sizeof(reason)), nullptr);
		goto fail;
	}
	if (!set_non_blocking(s) || (bind(s, (const struct sockaddr*)&address, sizeof(address)) != 0))
	{
		set_error(error, errorSize, "cannot open %s: %s", path, last_error(reason, sizeof(reason)));
		goto fail;
	}
	bound = TRUE;
#if !defined(_WIN32)
	if (chmod(path, S_IRUSR | S_IWUSR) != 0)
	{
		set_error(error, errorSize, "cannot make %s private: %s", path,
		          last_error(reason, sizeof(reason)));
		goto fail;
	}
#endif
	if (listen(s, (int)SHADOW_STATS_IPC_MAX_READERS) != 0)
	{
		set_error(error, errorSize, "cannot listen on %s: %s", path,
		          last_error(reason, sizeof(reason)));
		goto fail;
	}

	ipc = calloc(1, sizeof(*ipc));
	if (!ipc)
		goto fail;
	ipc->listener = s;
	s = SHADOW_STATS_INVALID;
#if !defined(_WIN32)
	ipc->wake[0] = -1;
	ipc->wake[1] = -1;
#endif
	ipc->path = _strdup(path);
	if (!ipc->path || !InitializeCriticalSectionAndSpinCount(&ipc->lock, 0))
	{
		free(ipc->path);
		close_socket(ipc->listener);
		free(ipc);
		ipc = nullptr;
		set_error(error, errorSize, "out of memory%s%s", nullptr, nullptr);
		goto fail;
	}
#if !defined(_WIN32)
	if ((pipe(ipc->wake) != 0) || !set_non_blocking(ipc->wake[0]) ||
	    !set_non_blocking(ipc->wake[1]))
	{
		set_error(error, errorSize, "pipe: %s%s", last_error(reason, sizeof(reason)), nullptr);
		shadow_stats_ipc_free(ipc); /* closes and removes what is open */
		return nullptr;
	}
#endif
	ipc->thread = CreateThread(nullptr, 0, shadow_stats_ipc_thread, ipc, 0, nullptr);
	if (!ipc->thread)
	{
		set_error(error, errorSize, "no thread for %s%s", path, nullptr);
		shadow_stats_ipc_free(ipc);
		return nullptr;
	}
	return ipc;

fail:
	if (s != SHADOW_STATS_INVALID)
		close_socket(s);
	if (bound)
		remove_path(path);
#if defined(_WIN32)
	WSACleanup();
#endif
	return nullptr;
}

void shadow_stats_ipc_free(SHADOW_STATS_IPC* ipc)
{
	if (!ipc)
		return;
	{
		const LONG previous = InterlockedExchange(&ipc->stop, 1);
		WINPR_UNUSED(previous);
	}
	wake(ipc);
	if (ipc->thread)
	{
		(void)WaitForSingleObject(ipc->thread, INFINITE);
		(void)CloseHandle(ipc->thread);
	}
	while (ipc->readersUsed > 0)
		drop_reader(ipc, ipc->readersUsed - 1);
	close_socket(ipc->listener);
	remove_path(ipc->path);
#if defined(_WIN32)
	WSACleanup();
#else
	for (size_t x = 0; x < ARRAYSIZE(ipc->wake); x++)
	{
		if (ipc->wake[x] >= 0)
			close(ipc->wake[x]);
	}
#endif
	for (size_t x = 0; x < ipc->clientCount; x++)
		free(ipc->clients[x].object);
	DeleteCriticalSection(&ipc->lock);
	free(ipc->latest);
	free(ipc->path);
	free(ipc);
}

/* the lock is held */
static void publish_locked(SHADOW_STATS_IPC* ipc, char* lineWithEnd, size_t length)
{
	free(ipc->latest);
	ipc->latest = lineWithEnd;
	ipc->latestLength = length;
	ipc->latestSeq++;
}

void shadow_stats_ipc_publish(SHADOW_STATS_IPC* ipc, const char* line)
{
	if (!ipc || !line)
		return;
	const size_t length = strlen(line);
	char* copy = malloc(length + 2);
	if (!copy)
		return;
	memcpy(copy, line, length);
	copy[length] = '\n';
	copy[length + 1] = '\0';

	EnterCriticalSection(&ipc->lock);
	publish_locked(ipc, copy, length + 1);
	LeaveCriticalSection(&ipc->lock);
	wake(ipc);
}

/* the lock is held: a line with every client's newest period */
static void publish_clients_locked(SHADOW_STATS_IPC* ipc)
{
	char head[192] = WINPR_C_ARRAY_INIT;
	const UINT64 nowMs = winpr_GetUnixTimeNS() / 1000000ull;
	const int rc = _snprintf(head, sizeof(head),
	                         "{\"schema\":\"%s\",\"version\":%u,\"seq\":%" PRIu64
	                         ",\"time_ms\":%" PRIu64 ",\"pid\":%" PRIu32 ",\"clients\":[",
	                         SHADOW_STATS_IPC_SCHEMA, SHADOW_STATS_IPC_VERSION, ++ipc->lineSeq,
	                         nowMs, (UINT32)GetCurrentProcessId());
	if ((rc <= 0) || ((size_t)rc >= sizeof(head)))
		return;

	size_t length = (size_t)rc + 3; /* "]}" and the line end */
	for (size_t x = 0; x < ipc->clientCount; x++)
		length += strlen(ipc->clients[x].object) + 1;

	char* line = malloc(length + 1);
	if (!line)
		return;
	size_t at = 0;
	memcpy(line, head, (size_t)rc);
	at += (size_t)rc;
	for (size_t x = 0; x < ipc->clientCount; x++)
	{
		if (x > 0)
			line[at++] = ',';
		const size_t n = strlen(ipc->clients[x].object);
		memcpy(&line[at], ipc->clients[x].object, n);
		at += n;
	}
	line[at++] = ']';
	line[at++] = '}';
	line[at++] = '\n';
	line[at] = '\0';
	publish_locked(ipc, line, at);
}

void shadow_stats_ipc_client_period(SHADOW_STATS_IPC* ipc, UINT64 connectionId, const char* object)
{
	if (!ipc || !object)
		return;
	char* copy = _strdup(object);
	if (!copy)
		return;

	EnterCriticalSection(&ipc->lock);
	SHADOW_STATS_CLIENT* slot = nullptr;
	for (size_t x = 0; x < ipc->clientCount; x++)
	{
		if (ipc->clients[x].connectionId == connectionId)
			slot = &ipc->clients[x];
	}
	if (!slot && (ipc->clientCount < ARRAYSIZE(ipc->clients)))
	{
		slot = &ipc->clients[ipc->clientCount++];
		slot->connectionId = connectionId;
		slot->object = nullptr;
	}
	if (slot)
	{
		free(slot->object);
		slot->object = copy;
		copy = nullptr;
		publish_clients_locked(ipc);
	}
	LeaveCriticalSection(&ipc->lock);
	free(copy);
	wake(ipc);
}

void shadow_stats_ipc_client_gone(SHADOW_STATS_IPC* ipc, UINT64 connectionId)
{
	if (!ipc)
		return;
	EnterCriticalSection(&ipc->lock);
	for (size_t x = 0; x < ipc->clientCount; x++)
	{
		if (ipc->clients[x].connectionId != connectionId)
			continue;
		free(ipc->clients[x].object);
		ipc->clientCount--;
		/* in order, so the clients stay in the order they came */
		memmove(&ipc->clients[x], &ipc->clients[x + 1],
		        (ipc->clientCount - x) * sizeof(ipc->clients[0]));
		memset(&ipc->clients[ipc->clientCount], 0, sizeof(ipc->clients[0]));
		break;
	}
	LeaveCriticalSection(&ipc->lock);
}

BOOL shadow_stats_ipc_peer_name(int sockfd, char* buffer, size_t size)
{
	if (!buffer || (size == 0) || (sockfd < 0))
		return FALSE;
	buffer[0] = '\0';
	struct sockaddr_storage address = WINPR_C_ARRAY_INIT;
	socklen_t length = sizeof(address);
	if (getpeername((NATIVE_SOCKET)sockfd, (struct sockaddr*)&address, &length) != 0)
		return FALSE;

	char ip[INET6_ADDRSTRLEN] = WINPR_C_ARRAY_INIT;
	int rc = -1;
	if (address.ss_family == AF_INET)
	{
		const struct sockaddr_in* in = (const struct sockaddr_in*)&address;
		if (!inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip)))
			return FALSE;
		rc = _snprintf(buffer, size, "%s:%u", ip, (unsigned)ntohs(in->sin_port));
	}
	else if (address.ss_family == AF_INET6)
	{
		const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)&address;
		if (!inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip)))
			return FALSE;
		rc = _snprintf(buffer, size, "[%s]:%u", ip, (unsigned)ntohs(in6->sin6_port));
	}
	return (rc > 0) && ((size_t)rc < size);
}

const char* shadow_stats_ipc_path(const SHADOW_STATS_IPC* ipc)
{
	return ipc ? ipc->path : nullptr;
}

size_t shadow_stats_ipc_readers(SHADOW_STATS_IPC* ipc)
{
	if (!ipc)
		return 0;
	return (size_t)InterlockedCompareExchange(&ipc->readerCount, 0, 0);
}
