/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: the overlay's numbers on a named pipe (/sdl-stats-ipc), Windows
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

#include <windows.h>
#include <sddl.h>

#include <cstring>
#include <utility>
#include <vector>

#include "sdl_stats_ipc.hpp"

/* The same rules as the AF_UNIX socket, on a named pipe: Windows' own local channel, which
 * PowerShell, .NET and Python's open() read without sockets. */
namespace
{
	const std::string pipePrefix = "\\\\.\\pipe\\";
	constexpr DWORD outBuffer = 64 * 1024;

	std::wstring widen(const std::string& s)
	{
		if (s.empty())
			return {};
		const int n =
		    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		if (n <= 0)
			return {};
		std::wstring w(static_cast<size_t>(n), L'\0');
		(void)MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
		return w;
	}

	std::string lastError(const char* what)
	{
		return std::string(what) + ": error " + std::to_string(GetLastError());
	}

	/* the user SID of a process token, as a buffer holding TOKEN_USER */
	bool tokenUser(HANDLE process, std::vector<BYTE>& buffer)
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(process, TOKEN_QUERY, &token))
			return false;
		DWORD size = 0;
		(void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		buffer.resize(size);
		const BOOL ok =
		    (size > 0) && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
		CloseHandle(token);
		return ok != FALSE;
	}

	PSID sidOf(std::vector<BYTE>& tokenUserBuffer)
	{
		return reinterpret_cast<TOKEN_USER*>(tokenUserBuffer.data())->User.Sid;
	}

	/* the process at the other end of the pipe runs as us */
	bool sameUser(HANDLE pipe, PSID ours)
	{
		ULONG pid = 0;
		if (!GetNamedPipeClientProcessId(pipe, &pid))
			return false;
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process)
			return false;
		std::vector<BYTE> buffer;
		const bool ok = tokenUser(process, buffer) && EqualSid(sidOf(buffer), ours);
		CloseHandle(process);
		return ok;
	}

	/* An instance never moves: the system writes into its OVERLAPPED until the operation ends */
	struct Instance
	{
		HANDLE pipe = INVALID_HANDLE_VALUE;
		OVERLAPPED ov = {};
		bool pending = false; /* a ConnectNamedPipe or a WriteFile is under way */
		std::string line;
		uint64_t seq = 0;
	};

	void closeInstance(Instance& instance)
	{
		if (instance.pipe != INVALID_HANDLE_VALUE)
		{
			if (instance.pending)
			{
				/* the buffers of a cancelled operation are free only once it has ended */
				(void)CancelIoEx(instance.pipe, &instance.ov);
				DWORD n = 0;
				(void)GetOverlappedResult(instance.pipe, &instance.ov, &n, TRUE);
			}
			(void)DisconnectNamedPipe(instance.pipe);
			CloseHandle(instance.pipe);
		}
		if (instance.ov.hEvent)
			CloseHandle(instance.ov.hEvent);
		instance = Instance{};
	}
} // namespace

struct SdlStatsIpc::Impl
{
	std::wstring name;
	std::vector<BYTE> user; /* TOKEN_USER of this process */
	PSECURITY_DESCRIPTOR sd = nullptr;
	SECURITY_ATTRIBUTES sa = {};
	HANDLE wake = nullptr;
	std::unique_ptr<Instance> listening;
	std::vector<std::unique_ptr<Instance>> readers;

	~Impl()
	{
		if (wake)
			CloseHandle(wake);
		if (sd)
			LocalFree(sd);
	}

	/* a new instance of the pipe, waiting for a reader */
	bool listen(bool first, std::string& error)
	{
		auto owner = std::make_unique<Instance>();
		Instance& instance = *owner;
		instance.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!instance.ov.hEvent)
		{
			error = lastError("CreateEvent");
			return false;
		}
		const DWORD open = PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED |
		                   (first ? static_cast<DWORD>(FILE_FLAG_FIRST_PIPE_INSTANCE) : 0);
		SetLastError(ERROR_SUCCESS);
		instance.pipe = CreateNamedPipeW(
		    name.c_str(), open,
		    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		    static_cast<DWORD>(SdlStatsIpc::maxReaders), outBuffer, 0, 0, &sa);
		/* Windows refuses a first instance of a name that exists (ERROR_ACCESS_DENIED); Wine 11
		 * creates it and only says ERROR_ALREADY_EXISTS: refuse that too */
		if (first && (instance.pipe != INVALID_HANDLE_VALUE) &&
		    (GetLastError() == ERROR_ALREADY_EXISTS))
		{
			CloseHandle(instance.pipe);
			instance.pipe = INVALID_HANDLE_VALUE;
			SetLastError(ERROR_ACCESS_DENIED);
		}
		if (instance.pipe == INVALID_HANDLE_VALUE)
		{
			/* FILE_FLAG_FIRST_PIPE_INSTANCE: someone has the name already */
			error = (first && (GetLastError() == ERROR_ACCESS_DENIED))
			            ? std::string("another program has the pipe name")
			            : lastError("CreateNamedPipe");
			closeInstance(instance);
			return false;
		}
		if (ConnectNamedPipe(instance.pipe, &instance.ov))
			instance.pending = true; /* overlapped: never TRUE, but if so the event tells */
		else
		{
			switch (GetLastError())
			{
				case ERROR_IO_PENDING:
					instance.pending = true;
					break;
				case ERROR_PIPE_CONNECTED: /* a reader came between create and connect */
					(void)SetEvent(instance.ov.hEvent);
					break;
				default:
					error = lastError("ConnectNamedPipe");
					closeInstance(instance);
					return false;
			}
		}
		listening = std::move(owner);
		return true;
	}
};

std::string SdlStatsIpc::defaultPath([[maybe_unused]] std::string& error)
{
	return pipePrefix + "sdl-freerdp-" + std::to_string(GetCurrentProcessId()) + ".stats";
}

std::unique_ptr<SdlStatsIpc> SdlStatsIpc::open(const std::string& path, std::string& error)
{
	if (path.empty())
	{
		error = "no pipe name";
		return nullptr;
	}
	const std::string full = (path.rfind(pipePrefix, 0) == 0) ? path : pipePrefix + path;
	/* the name after the prefix: 1 to 256 characters, no backslash */
	const std::string rest = full.substr(pipePrefix.size());
	if (rest.empty() || (rest.size() > 256) || (rest.find('\\') != std::string::npos))
	{
		error = "a pipe name needs 1 to 256 characters and no backslash: " + full;
		return nullptr;
	}

	std::unique_ptr<SdlStatsIpc> ipc(new SdlStatsIpc(full));
	auto& impl = *ipc->_impl;
	impl.name = widen(full);

	/* only this user (and SYSTEM) may open the pipe or create an instance of its name */
	if (!tokenUser(GetCurrentProcess(), impl.user))
	{
		error = lastError("OpenProcessToken");
		return nullptr;
	}
	LPWSTR sidString = nullptr;
	if (!ConvertSidToStringSidW(sidOf(impl.user), &sidString))
	{
		error = lastError("ConvertSidToStringSid");
		return nullptr;
	}
	const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sidString) + L")(A;;GA;;;SY)";
	LocalFree(sidString);
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
	                                                          &impl.sd, nullptr))
	{
		error = lastError("ConvertStringSecurityDescriptorToSecurityDescriptor");
		return nullptr;
	}
	impl.sa.nLength = sizeof(impl.sa);
	impl.sa.lpSecurityDescriptor = impl.sd;
	impl.sa.bInheritHandle = FALSE;

	impl.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!impl.wake)
	{
		error = lastError("CreateEvent");
		return nullptr;
	}
	if (!impl.listen(true, error))
	{
		error = full + ": " + error;
		return nullptr;
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
		closeInstance(*reader);
	if (_impl->listening)
		closeInstance(*_impl->listening);
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
	if (_impl->wake)
		(void)SetEvent(_impl->wake);
}

void SdlStatsIpc::run()
{
	auto& impl = *_impl;
	auto& readers = impl.readers;

	while (!_stop)
	{
		/* one instance waits for the next reader while there is room */
		if (!impl.listening && (readers.size() < maxReaders))
		{
			std::string ignored;
			(void)impl.listen(false, ignored); /* tried again on the next wake */
		}

		/* each reader that is done with its line gets the newest one */
		std::vector<size_t> gone;
		{
			std::scoped_lock lock(_mutex);
			for (size_t x = 0; x < readers.size(); x++)
			{
				auto& reader = *readers.at(x);
				if (reader.pending || (reader.seq >= _latestSeq))
					continue;
				reader.line = _latest;
				reader.seq = _latestSeq;
				if (WriteFile(reader.pipe, reader.line.data(),
				              static_cast<DWORD>(reader.line.size()), nullptr, &reader.ov) ||
				    (GetLastError() == ERROR_IO_PENDING))
					reader.pending = true; /* done at once or not: the event tells */
				else
					gone.push_back(x); /* ERROR_NO_DATA, ERROR_BROKEN_PIPE: the reader left */
			}
		}
		for (auto it = gone.rbegin(); it != gone.rend(); ++it)
		{
			closeInstance(*readers.at(*it));
			readers.erase(readers.begin() + static_cast<std::ptrdiff_t>(*it));
		}
		_readers = readers.size();

		std::vector<HANDLE> handles = { impl.wake };
		if (impl.listening)
			handles.push_back(impl.listening->ov.hEvent);
		for (const auto& reader : readers)
		{
			if (reader->pending)
				handles.push_back(reader->ov.hEvent);
		}
		/* without a listening instance (it failed) look again soon */
		const DWORD timeout = (impl.listening || (readers.size() >= maxReaders)) ? INFINITE : 100;
		const DWORD rc = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(),
		                                        FALSE, timeout);
		if (rc == WAIT_FAILED)
			break;
		if (_stop)
			break;

		/* finished writes */
		gone.clear();
		for (size_t x = 0; x < readers.size(); x++)
		{
			auto& reader = *readers.at(x);
			if (!reader.pending || !HasOverlappedIoCompleted(&reader.ov))
				continue;
			DWORD n = 0;
			reader.pending = false;
			if (!GetOverlappedResult(reader.pipe, &reader.ov, &n, FALSE) ||
			    (n != reader.line.size()))
				gone.push_back(x);
		}
		for (auto it = gone.rbegin(); it != gone.rend(); ++it)
		{
			closeInstance(*readers.at(*it));
			readers.erase(readers.begin() + static_cast<std::ptrdiff_t>(*it));
		}

		/* a reader came: ours, or closed at once */
		if (impl.listening &&
		    (!impl.listening->pending || HasOverlappedIoCompleted(&impl.listening->ov)) &&
		    (WaitForSingleObject(impl.listening->ov.hEvent, 0) == WAIT_OBJECT_0))
		{
			auto owner = std::move(impl.listening);
			Instance& instance = *owner;
			DWORD n = 0;
			const bool connected = !instance.pending ||
			                       GetOverlappedResult(instance.pipe, &instance.ov, &n, FALSE) ||
			                       (GetLastError() == ERROR_PIPE_CONNECTED);
			instance.pending = false;
			if (connected && sameUser(instance.pipe, sidOf(impl.user)))
			{
				(void)ResetEvent(instance.ov.hEvent);
				readers.push_back(std::move(owner));
			}
			else
				closeInstance(instance);
		}
		_readers = readers.size();
	}
}
