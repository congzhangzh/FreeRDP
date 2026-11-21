/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: the overlay's numbers on a local socket (/sdl-stats-ipc), Linux and macOS
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

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "sdl_stats_ipc.hpp"

namespace
{
	using Native = int;
	constexpr Native invalidSocket = -1;

	void closeSocket(Native s)
	{
		close(s);
	}

	bool wouldBlock()
	{
		return (errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR);
	}

	bool setNonBlocking(Native s)
	{
		const int flags = fcntl(s, F_GETFL, 0);
		if (flags < 0)
			return false;
		if (fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0)
			return false;
		return fcntl(s, F_SETFD, FD_CLOEXEC) == 0;
	}

	std::string lastError()
	{
		return strerror(errno);
	}

	int pollSockets(std::vector<pollfd>& fds, int timeoutMs)
	{
		return poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
	}

	using PollFd = pollfd;

	bool fillAddress(const std::string& path, sockaddr_un& address, std::string& error)
	{
		address = {};
		address.sun_family = AF_UNIX;
		if (path.empty() || (path.size() >= sizeof(address.sun_path)))
		{
			error = "the socket path must have 1 to " +
			        std::to_string(sizeof(address.sun_path) - 1) + " bytes: " + path;
			return false;
		}
		memcpy(address.sun_path, path.c_str(), path.size() + 1);
		return true;
	}

	/* someone listens on path */
	bool listening(const std::string& path)
	{
		sockaddr_un address = {};
		std::string ignored;
		if (!fillAddress(path, address, ignored))
			return false;
		const Native s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (s == invalidSocket)
			return false;
		const bool up =
		    connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
		closeSocket(s);
		return up;
	}

	/* Make room for the socket at path, or say why not. */
	bool prepare(const std::string& path, std::string& error)
	{
		const uid_t me = geteuid();
		const auto slash = path.find_last_of('/');
		const std::string parent = (slash == std::string::npos) ? "."
		                           : (slash == 0)               ? std::string("/")
		                                                        : path.substr(0, slash);
		struct stat dir = {};
		if (stat(parent.c_str(), &dir) != 0)
		{
			error = "no directory " + parent + ": " + lastError();
			return false;
		}
		/* nobody else may put or replace a file there, unless the sticky bit keeps ours ours */
		const bool owner = (dir.st_uid == me) || (dir.st_uid == 0);
		const bool othersWrite = (dir.st_mode & (S_IWGRP | S_IWOTH)) != 0;
		if (!S_ISDIR(dir.st_mode) || !owner || (othersWrite && ((dir.st_mode & S_ISVTX) == 0)))
		{
			error = "the directory " + parent +
			        " must be this user's and not writable by others (or sticky)";
			return false;
		}

		struct stat old = {};
		if (lstat(path.c_str(), &old) != 0)
		{
			if (errno == ENOENT)
				return true;
			error = "cannot look at " + path + ": " + lastError();
			return false;
		}
		if (!S_ISSOCK(old.st_mode) || (old.st_uid != me))
		{
			error = "not replacing a file that is no socket of this user: " + path;
			return false;
		}
		if (listening(path))
		{
			error = "another client listens on " + path;
			return false;
		}
		if (unlink(path.c_str()) != 0)
		{
			error = "the old socket could not be removed: " + path + ": " + lastError();
			return false;
		}
		return true;
	}

	/* the reader runs as this user */
	bool sameUser(Native s)
	{
#if defined(SO_PEERCRED)
		struct ucred cred = {};
		socklen_t len = sizeof(cred);
		if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
			return false;
		return cred.uid == geteuid();
#else
		uid_t uid = 0;
		gid_t gid = 0;
		if (getpeereid(s, &uid, &gid) != 0)
			return false;
		return uid == geteuid();
#endif
	}

	/* a write to a reader that is gone must not end the process */
	void noSigPipe([[maybe_unused]] Native s)
	{
#if defined(SO_NOSIGPIPE)
		const int on = 1;
		setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
	}

	constexpr int sendFlags()
	{
#if defined(MSG_NOSIGNAL)
		return MSG_NOSIGNAL;
#else
		return 0;
#endif
	}

	struct Reader
	{
		Native socket = invalidSocket;
		std::string line;
		size_t sent = 0;
		uint64_t seq = 0;
	};
} // namespace

struct SdlStatsIpc::Impl
{
	int listener = -1;
	int wake[2] = { -1, -1 };
	static constexpr int waitMs = -1;
	std::vector<Reader> readers;
};

std::string SdlStatsIpc::defaultPath(std::string& error)
{
	const std::string name = "sdl-freerdp-" + std::to_string(getpid()) + ".stats.sock";
	/* XDG_RUNTIME_DIR is the user's own (0700); macOS has none, its TMPDIR is per user */
	for (const char* variable : { "XDG_RUNTIME_DIR", "TMPDIR" })
	{
		const char* base = getenv(variable);
		if (!base || (strlen(base) == 0))
			continue;
		std::string dir = base;
		while ((dir.size() > 1) && (dir.back() == '/'))
			dir.pop_back();
		return dir + "/" + name;
	}
	error = "neither XDG_RUNTIME_DIR nor TMPDIR is set: give /sdl-stats-ipc a path";
	return {};
}

std::unique_ptr<SdlStatsIpc> SdlStatsIpc::open(const std::string& path, std::string& error)
{
	sockaddr_un address = {};
	if (!fillAddress(path, address, error) || !prepare(path, error))
	{
		return nullptr;
	}

	const Native s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s == invalidSocket)
	{
		error = "socket: " + lastError();
		return nullptr;
	}
	if (!setNonBlocking(s) ||
	    (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0))
	{
		error = "cannot open " + path + ": " + lastError();
		closeSocket(s);
		return nullptr;
	}
	if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
	{
		error = "cannot make " + path + " private: " + lastError();
		closeSocket(s);
		unlink(path.c_str());
		return nullptr;
	}
	if (listen(s, static_cast<int>(maxReaders)) != 0)
	{
		error = "cannot listen on " + path + ": " + lastError();
		closeSocket(s);
		unlink(path.c_str());
		return nullptr;
	}

	std::unique_ptr<SdlStatsIpc> ipc(new SdlStatsIpc(path));
	ipc->_impl->listener = s;
	if (pipe(ipc->_impl->wake) != 0 || !setNonBlocking(ipc->_impl->wake[0]) ||
	    !setNonBlocking(ipc->_impl->wake[1]))
	{
		error = "pipe: " + lastError();
		return nullptr; /* the destructor closes and removes what is open */
	}
	ipc->_thread = std::thread([raw = ipc.get()] { raw->run(); });
	return ipc;
}

SdlStatsIpc::SdlStatsIpc(std::string path) : _path(std::move(path)), _impl(std::make_unique<Impl>())
{
}

SdlStatsIpc::~SdlStatsIpc()
{
	_stop = true;
	wake();
	if (_thread.joinable())
		_thread.join();
	for (auto& reader : _impl->readers)
		closeSocket(reader.socket);
	if (_impl->listener >= 0)
		closeSocket(_impl->listener);
	unlink(_path.c_str());
	for (int& fd : _impl->wake)
	{
		if (fd >= 0)
			close(fd);
	}
}

void SdlStatsIpc::publish(std::string line)
{
	line += '\n';
	{
		std::scoped_lock lock(_mutex);
		_latest = std::move(line);
		_latestSeq++;
	}
	wake();
}

const std::string& SdlStatsIpc::path() const
{
	return _path;
}

size_t SdlStatsIpc::readers() const
{
	return _readers;
}

void SdlStatsIpc::wake()
{
	if (_impl->wake[1] >= 0)
	{
		const char c = 0;
		/* a full pipe already wakes the thread */
		[[maybe_unused]] const auto rc = write(_impl->wake[1], &c, 1);
	}
}

void SdlStatsIpc::run()
{
	auto& readers = _impl->readers;
	const Native listener = _impl->listener;

	while (!_stop)
	{
		/* each reader that is done with its line gets the newest one */
		{
			std::scoped_lock lock(_mutex);
			for (auto& reader : readers)
			{
				if ((reader.sent == reader.line.size()) && (reader.seq < _latestSeq))
				{
					reader.line = _latest;
					reader.sent = 0;
					reader.seq = _latestSeq;
				}
			}
		}

		std::vector<PollFd> fds;
		fds.push_back({});
		fds.back().fd = listener;
		fds.back().events = POLLIN;
		fds.push_back({});
		fds.back().fd = _impl->wake[0];
		fds.back().events = POLLIN;
		const size_t first = fds.size();
		for (const auto& reader : readers)
		{
			fds.push_back({});
			fds.back().fd = reader.socket;
			fds.back().events = POLLIN;
			if (reader.sent < reader.line.size())
				fds.back().events |= POLLOUT;
		}

		if (pollSockets(fds, Impl::waitMs) < 0)
		{
			if (wouldBlock())
				continue;
			break;
		}
		if (_stop)
			break;

		if ((fds.at(1).revents & POLLIN) != 0)
		{
			char buffer[64];
			while (read(_impl->wake[0], buffer, sizeof(buffer)) > 0)
			{
			}
		}

		std::vector<size_t> gone;
		for (size_t x = 0; x < readers.size(); x++)
		{
			auto& reader = readers.at(x);
			const auto revents = fds.at(first + x).revents;
			bool drop = (revents & (POLLERR | POLLNVAL)) != 0;
			if (!drop && ((revents & (POLLIN | POLLHUP)) != 0))
			{
				/* what a reader sends means nothing; its end closes it */
				char buffer[256];
				const auto n = recv(reader.socket, buffer, sizeof(buffer), 0);
				if ((n == 0) || ((n < 0) && !wouldBlock()))
					drop = true;
			}
			if (!drop && ((revents & POLLOUT) != 0) && (reader.sent < reader.line.size()))
			{
				const auto left = reader.line.size() - reader.sent;
				const auto n =
				    send(reader.socket, reader.line.data() + reader.sent, left, sendFlags());
				if (n > 0)
					reader.sent += static_cast<size_t>(n);
				else if (!wouldBlock())
					drop = true;
			}
			if (drop)
				gone.push_back(x);
		}
		for (auto it = gone.rbegin(); it != gone.rend(); ++it)
		{
			closeSocket(readers.at(*it).socket);
			readers.erase(readers.begin() + static_cast<std::ptrdiff_t>(*it));
		}

		if ((fds.at(0).revents & POLLIN) != 0)
		{
			for (;;)
			{
				const Native s = accept(listener, nullptr, nullptr);
				if (s == invalidSocket)
					break;
				/* a full house or another user: closed at once, so the reader sees its end */
				if ((readers.size() >= maxReaders) || !sameUser(s) || !setNonBlocking(s))
				{
					closeSocket(s);
					continue;
				}
				noSigPipe(s);
				readers.push_back({ s, {}, 0, 0 });
			}
		}
		_readers = readers.size();
	}
}
