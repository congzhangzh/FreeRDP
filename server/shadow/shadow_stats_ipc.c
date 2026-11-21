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
#include <ws2tcpip.h>
#include <sddl.h>
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

typedef struct
{
	UINT64 connectionId;
	char* object;
} SHADOW_STATS_CLIENT;

/* The transport: an AF_UNIX socket on Linux and macOS, a named pipe on Windows (Python's open(),
 * PowerShell and .NET read a pipe; Python on Windows has no AF_UNIX). The same rules on both:
 * the newest line, never a queue; at most SHADOW_STATS_IPC_MAX_READERS readers; this user only. */
#if !defined(_WIN32)
typedef int NATIVE_SOCKET;
#define SHADOW_STATS_INVALID (-1)
typedef struct pollfd SHADOW_POLLFD;
#define SHADOW_STATS_WAIT_MS (-1)

typedef struct
{
	NATIVE_SOCKET socket;
	char* line; /* the line it gets, with its line end */
	size_t length;
	size_t sent;
	UINT64 seq;
} SHADOW_STATS_READER;

struct s_shadow_stats_ipc
{
	char* path;
	NATIVE_SOCKET listener;
	int wake[2];
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
	close(s);
}

static BOOL would_block(void)
{
	return (errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR);
}

static BOOL set_non_blocking(NATIVE_SOCKET s)
{
	const int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0)
		return FALSE;
	if (fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0)
		return FALSE;
	return fcntl(s, F_SETFD, FD_CLOEXEC) == 0;
}

static const char* last_error(char* buffer, size_t size)
{
	return winpr_strerror(errno, buffer, size);
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
}

static void remove_path(const char* path)
{
	(void)unlink(path);
}

/* the reader runs as this user */
static BOOL same_user(NATIVE_SOCKET s)
{
#if defined(SO_PEERCRED)
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
	if (ipc->wake[1] >= 0)
	{
		const char c = 0;
		/* a full pipe already wakes the thread */
		const ssize_t rc = write(ipc->wake[1], &c, 1);
		WINPR_UNUSED(rc);
	}
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
		const ssize_t n = send(reader->socket, reader->line + reader->sent, left, send_flags());
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
		fds[count].fd = ipc->wake[0];
		fds[count++].events = POLLIN;
		const size_t first = count;
		for (size_t x = 0; x < ipc->readersUsed; x++)
		{
			fds[count].fd = ipc->readers[x].socket;
			fds[count].events = POLLIN;
			if (ipc->readers[x].sent < ipc->readers[x].length)
				fds[count].events |= POLLOUT;
			count++;
		}

		const int rc = poll(fds, (nfds_t)count, SHADOW_STATS_WAIT_MS);
		if (rc < 0)
		{
			if (would_block())
				continue;
			break;
		}
		if (InterlockedCompareExchange(&ipc->stop, 0, 0) != 0)
			break;

		if ((fds[1].revents & POLLIN) != 0)
		{
			char buffer[64];
			while (read(ipc->wake[0], buffer, sizeof(buffer)) > 0)
			{
			}
		}

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
}

SHADOW_STATS_IPC* shadow_stats_ipc_open(const char* path, char* error, size_t errorSize)
{
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
	if (chmod(path, S_IRUSR | S_IWUSR) != 0)
	{
		set_error(error, errorSize, "cannot make %s private: %s", path,
		          last_error(reason, sizeof(reason)));
		goto fail;
	}
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
	ipc->wake[0] = -1;
	ipc->wake[1] = -1;
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
	if ((pipe(ipc->wake) != 0) || !set_non_blocking(ipc->wake[0]) ||
	    !set_non_blocking(ipc->wake[1]))
	{
		set_error(error, errorSize, "pipe: %s%s", last_error(reason, sizeof(reason)), nullptr);
		shadow_stats_ipc_free(ipc); /* closes and removes what is open */
		return nullptr;
	}
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
	for (size_t x = 0; x < ARRAYSIZE(ipc->wake); x++)
	{
		if (ipc->wake[x] >= 0)
			close(ipc->wake[x]);
	}
	for (size_t x = 0; x < ipc->clientCount; x++)
		free(ipc->clients[x].object);
	DeleteCriticalSection(&ipc->lock);
	free(ipc->latest);
	free(ipc->path);
	free(ipc);
}
#else /* _WIN32: a named pipe */

#define SHADOW_STATS_PIPE_PREFIX "\\\\.\\pipe\\"
#define SHADOW_STATS_PIPE_BUFFER (64u * 1024u)

/* One instance of the pipe. It never moves while an operation is under way: the system writes
 * into its OVERLAPPED until the operation ends (each is malloc'd on its own; only pointers move).
 */
typedef struct
{
	HANDLE pipe;
	OVERLAPPED ov;
	BOOL pending; /* a ConnectNamedPipe or a WriteFile is under way */
	char* line;
	size_t length;
	UINT64 seq;
} SHADOW_STATS_INSTANCE;

struct s_shadow_stats_ipc
{
	char* path;
	WCHAR* name;
	BYTE user[256]; /* TOKEN_USER of this process */
	PSECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;
	HANDLE wake;
	SHADOW_STATS_INSTANCE* listening; /* waits for the next reader, nullptr when none */
	SHADOW_STATS_INSTANCE* readers[SHADOW_STATS_IPC_MAX_READERS];
	size_t readersUsed; /* the pipe thread's */

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
};

static void set_error(char* error, size_t size, const char* format, const char* a, const char* b)
{
	if (error && (size > 0))
		(void)_snprintf(error, size, format, a ? a : "", b ? b : "");
}

static void set_last_error(char* error, size_t size, const char* what)
{
	char code[32] = WINPR_C_ARRAY_INIT;
	(void)_snprintf(code, sizeof(code), "%lu", (unsigned long)GetLastError());
	set_error(error, size, "%s: error %s", what, code);
}

/* the user of a process, as TOKEN_USER in buffer */
static PSID process_user(HANDLE process, BYTE* buffer, DWORD size)
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(process, TOKEN_QUERY, &token))
		return nullptr;
	DWORD length = 0;
	const BOOL ok = GetTokenInformation(token, TokenUser, buffer, size, &length);
	(void)CloseHandle(token);
	return ok ? ((TOKEN_USER*)buffer)->User.Sid : nullptr;
}

/* the process at the other end of the pipe runs as this user */
static BOOL same_user(SHADOW_STATS_IPC* ipc, HANDLE pipe)
{
	ULONG pid = 0;
	if (!GetNamedPipeClientProcessId(pipe, &pid))
		return FALSE;
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return FALSE;
	BYTE buffer[256] = WINPR_C_ARRAY_INIT;
	PSID peer = process_user(process, buffer, sizeof(buffer));
	const BOOL same = peer && EqualSid(peer, ((TOKEN_USER*)ipc->user)->User.Sid);
	(void)CloseHandle(process);
	return same;
}

static void close_instance(SHADOW_STATS_INSTANCE* instance)
{
	if (!instance)
		return;
	if (instance->pipe != INVALID_HANDLE_VALUE)
	{
		if (instance->pending)
		{
			/* the buffers of a cancelled operation are free only once it has ended */
			(void)CancelIoEx(instance->pipe, &instance->ov);
			DWORD n = 0;
			(void)GetOverlappedResult(instance->pipe, &instance->ov, &n, TRUE);
		}
		(void)DisconnectNamedPipe(instance->pipe);
		(void)CloseHandle(instance->pipe);
	}
	if (instance->ov.hEvent)
		(void)CloseHandle(instance->ov.hEvent);
	free(instance->line);
	free(instance);
}

/* a new instance of the pipe, waiting for a reader */
static BOOL pipe_listen(SHADOW_STATS_IPC* ipc, BOOL first, char* error, size_t size)
{
	SHADOW_STATS_INSTANCE* instance = calloc(1, sizeof(*instance));
	if (!instance)
	{
		set_error(error, size, "out of memory%s%s", nullptr, nullptr);
		return FALSE;
	}
	instance->pipe = INVALID_HANDLE_VALUE;
	instance->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!instance->ov.hEvent)
	{
		set_last_error(error, size, "CreateEvent");
		close_instance(instance);
		return FALSE;
	}
	const DWORD open =
	    PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
	SetLastError(ERROR_SUCCESS);
	instance->pipe = CreateNamedPipeW(
	    ipc->name, open,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
	    SHADOW_STATS_IPC_MAX_READERS, SHADOW_STATS_PIPE_BUFFER, 0, 0, &ipc->sa);
	/* Windows refuses a first instance of a name that exists (ERROR_ACCESS_DENIED); Wine 11
	 * creates it and only says ERROR_ALREADY_EXISTS: refuse that too */
	if (first && (instance->pipe != INVALID_HANDLE_VALUE) &&
	    (GetLastError() == ERROR_ALREADY_EXISTS))
	{
		(void)CloseHandle(instance->pipe);
		instance->pipe = INVALID_HANDLE_VALUE;
		SetLastError(ERROR_ACCESS_DENIED);
	}
	if (instance->pipe == INVALID_HANDLE_VALUE)
	{
		if (first && (GetLastError() == ERROR_ACCESS_DENIED))
			set_error(error, size, "another program has the pipe name %s%s", ipc->path, nullptr);
		else
			set_last_error(error, size, "CreateNamedPipe");
		close_instance(instance);
		return FALSE;
	}
	if (ConnectNamedPipe(instance->pipe, &instance->ov))
		instance->pending = TRUE; /* overlapped: never TRUE, but if so the event tells */
	else
	{
		switch (GetLastError())
		{
			case ERROR_IO_PENDING:
				instance->pending = TRUE;
				break;
			case ERROR_PIPE_CONNECTED: /* a reader came between create and connect */
				(void)SetEvent(instance->ov.hEvent);
				break;
			default:
				set_last_error(error, size, "ConnectNamedPipe");
				close_instance(instance);
				return FALSE;
		}
	}
	ipc->listening = instance;
	return TRUE;
}

static void drop_reader(SHADOW_STATS_IPC* ipc, size_t index)
{
	close_instance(ipc->readers[index]);
	ipc->readersUsed--;
	ipc->readers[index] = ipc->readers[ipc->readersUsed];
	ipc->readers[ipc->readersUsed] = nullptr;
}

static void wake(SHADOW_STATS_IPC* ipc)
{
	if (ipc->wake)
		(void)SetEvent(ipc->wake);
}

static DWORD WINAPI shadow_stats_ipc_thread(LPVOID arg)
{
	SHADOW_STATS_IPC* ipc = arg;
	WINPR_ASSERT(ipc);

	while (InterlockedCompareExchange(&ipc->stop, 0, 0) == 0)
	{
		/* one instance waits for the next reader while there is room */
		if (!ipc->listening && (ipc->readersUsed < SHADOW_STATS_IPC_MAX_READERS))
			(void)pipe_listen(ipc, FALSE, nullptr, 0); /* tried again on the next round */

		/* each reader that is done with its line gets the newest one */
		EnterCriticalSection(&ipc->lock);
		for (size_t x = ipc->readersUsed; x > 0; x--)
		{
			SHADOW_STATS_INSTANCE* reader = ipc->readers[x - 1];
			if (reader->pending || (reader->seq >= ipc->latestSeq) || !ipc->latest)
				continue;
			char* copy = malloc(ipc->latestLength);
			if (!copy)
				continue;
			memcpy(copy, ipc->latest, ipc->latestLength);
			free(reader->line);
			reader->line = copy;
			reader->length = ipc->latestLength;
			reader->seq = ipc->latestSeq;
			(void)ResetEvent(reader->ov.hEvent);
			if (WriteFile(reader->pipe, reader->line, (DWORD)reader->length, nullptr,
			              &reader->ov) ||
			    (GetLastError() == ERROR_IO_PENDING))
				reader->pending = TRUE; /* done at once or not: the event tells */
			else
				drop_reader(ipc, x - 1); /* ERROR_NO_DATA, ERROR_BROKEN_PIPE: the reader left */
		}
		LeaveCriticalSection(&ipc->lock);
		{
			const LONG previous = InterlockedExchange(&ipc->readerCount, (LONG)ipc->readersUsed);
			WINPR_UNUSED(previous);
		}

		HANDLE handles[2 + SHADOW_STATS_IPC_MAX_READERS] = WINPR_C_ARRAY_INIT;
		DWORD count = 0;
		handles[count++] = ipc->wake;
		if (ipc->listening)
			handles[count++] = ipc->listening->ov.hEvent;
		for (size_t x = 0; x < ipc->readersUsed; x++)
		{
			if (ipc->readers[x]->pending)
				handles[count++] = ipc->readers[x]->ov.hEvent;
		}
		/* without a listening instance (it failed) look again soon */
		const DWORD timeout =
		    (ipc->listening || (ipc->readersUsed >= SHADOW_STATS_IPC_MAX_READERS)) ? INFINITE : 100;
		if (WaitForMultipleObjects(count, handles, FALSE, timeout) == WAIT_FAILED)
			break;
		if (InterlockedCompareExchange(&ipc->stop, 0, 0) != 0)
			break;

		/* finished writes; a reader that left fails its write (no POLLHUP on a pipe: it is seen
		 * gone at its next line) */
		for (size_t x = ipc->readersUsed; x > 0; x--)
		{
			SHADOW_STATS_INSTANCE* reader = ipc->readers[x - 1];
			if (!reader->pending || (WaitForSingleObject(reader->ov.hEvent, 0) != WAIT_OBJECT_0))
				continue;
			DWORD n = 0;
			const BOOL ok = GetOverlappedResult(reader->pipe, &reader->ov, &n, FALSE);
			reader->pending = FALSE;
			(void)ResetEvent(reader->ov.hEvent);
			if (!ok || (n != reader->length))
				drop_reader(ipc, x - 1);
		}

		/* a reader connected: of this user, and there is room, or closed at once */
		SHADOW_STATS_INSTANCE* listening = ipc->listening;
		if (listening && (WaitForSingleObject(listening->ov.hEvent, 0) == WAIT_OBJECT_0))
		{
			DWORD n = 0;
			const BOOL ok = !listening->pending ||
			                GetOverlappedResult(listening->pipe, &listening->ov, &n, FALSE);
			listening->pending = FALSE;
			(void)ResetEvent(listening->ov.hEvent);
			ipc->listening = nullptr;
			if (ok && (ipc->readersUsed < SHADOW_STATS_IPC_MAX_READERS) &&
			    same_user(ipc, listening->pipe))
				ipc->readers[ipc->readersUsed++] = listening;
			else
				close_instance(listening);
		}
	}
	return 0;
}

BOOL shadow_stats_ipc_default_path(char* path, size_t size, char* error, size_t errorSize)
{
	WINPR_UNUSED(error);
	WINPR_UNUSED(errorSize);
	if (!path || (size == 0))
		return FALSE;
	const int rc =
	    _snprintf(path, size, SHADOW_STATS_PIPE_PREFIX "freerdp-shadow-%" PRIu32 ".stats",
	              (UINT32)GetCurrentProcessId());
	return (rc > 0) && ((size_t)rc < size);
}

SHADOW_STATS_IPC* shadow_stats_ipc_open(const char* path, char* error, size_t errorSize)
{
	if (!path || (*path == '\0'))
	{
		set_error(error, errorSize, "no pipe name%s%s", nullptr, nullptr);
		return nullptr;
	}
	/* a bare name gets the prefix; the name after it: 1 to 256 characters, no backslash */
	const size_t prefix = strlen(SHADOW_STATS_PIPE_PREFIX);
	const BOOL full = (_strnicmp(path, SHADOW_STATS_PIPE_PREFIX, prefix) == 0);
	const char* rest = full ? path + prefix : path;
	if ((*rest == '\0') || (strlen(rest) > 256) || strchr(rest, '\\'))
	{
		set_error(error, errorSize, "a pipe name needs 1 to 256 characters and no backslash: %s%s",
		          path, nullptr);
		return nullptr;
	}

	SHADOW_STATS_IPC* ipc = calloc(1, sizeof(*ipc));
	if (!ipc)
		return nullptr;
	if (!InitializeCriticalSectionAndSpinCount(&ipc->lock, 0))
	{
		free(ipc);
		return nullptr;
	}
	const size_t length = prefix + strlen(rest) + 1;
	ipc->path = malloc(length);
	if (!ipc->path)
		goto fail;
	(void)_snprintf(ipc->path, length, "%s%s", SHADOW_STATS_PIPE_PREFIX, rest);
	ipc->name = ConvertUtf8ToWCharAlloc(ipc->path, nullptr);
	if (!ipc->name)
		goto fail;

	/* only this user (and SYSTEM) may open the pipe or create an instance of its name */
	PSID me = process_user(GetCurrentProcess(), ipc->user, sizeof(ipc->user));
	LPWSTR sid = nullptr;
	if (!me || !ConvertSidToStringSidW(me, &sid))
	{
		set_last_error(error, errorSize, "the user of this process");
		goto fail;
	}
	WCHAR sddl[256] = WINPR_C_ARRAY_INIT;
	(void)_snwprintf(sddl, ARRAYSIZE(sddl) - 1, L"D:P(A;;GA;;;%s)(A;;GA;;;SY)", sid);
	LocalFree(sid);
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &ipc->sd,
	                                                          nullptr))
	{
		set_last_error(error, errorSize, "the pipe's access list");
		goto fail;
	}
	ipc->sa.nLength = sizeof(ipc->sa);
	ipc->sa.lpSecurityDescriptor = ipc->sd;
	ipc->sa.bInheritHandle = FALSE;
	ipc->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!ipc->wake)
	{
		set_last_error(error, errorSize, "CreateEvent");
		goto fail;
	}
	if (!pipe_listen(ipc, TRUE, error, errorSize))
		goto fail;
	ipc->thread = CreateThread(nullptr, 0, shadow_stats_ipc_thread, ipc, 0, nullptr);
	if (!ipc->thread)
	{
		set_error(error, errorSize, "no thread for %s%s", ipc->path, nullptr);
		goto fail;
	}
	return ipc;

fail:
	shadow_stats_ipc_free(ipc);
	return nullptr;
}

void shadow_stats_ipc_free(SHADOW_STATS_IPC* ipc)
{
	if (!ipc)
		return;
	(void)InterlockedExchange(&ipc->stop, 1);
	wake(ipc);
	if (ipc->thread)
	{
		(void)WaitForSingleObject(ipc->thread, INFINITE);
		(void)CloseHandle(ipc->thread);
	}
	while (ipc->readersUsed > 0)
		drop_reader(ipc, ipc->readersUsed - 1);
	close_instance(ipc->listening);
	if (ipc->wake)
		(void)CloseHandle(ipc->wake);
	if (ipc->sd)
		LocalFree(ipc->sd);
	for (size_t x = 0; x < ipc->clientCount; x++)
		free(ipc->clients[x].object);
	DeleteCriticalSection(&ipc->lock);
	free(ipc->latest);
	free(ipc->name);
	free(ipc->path);
	free(ipc);
}

#endif /* _WIN32 */

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
#if defined(_WIN32)
	if (getpeername((SOCKET)sockfd, (struct sockaddr*)&address, &length) != 0)
#else
	if (getpeername(sockfd, (struct sockaddr*)&address, &length) != 0)
#endif
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
