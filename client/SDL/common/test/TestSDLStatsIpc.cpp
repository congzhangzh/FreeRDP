#if defined(_WIN32)
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../SDL3/sdl_stats_ipc.hpp"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return -1;                                                       \
		}                                                                    \
	} while (0)

/* Windows: the same stream on a named pipe (sdl_stats_ipc_win32.cpp) */
namespace
{
	std::string uniqueName(const char* what)
	{
		return "\\\\.\\pipe\\sdl-stats-ipc-test-" + std::to_string(GetCurrentProcessId()) + "-" +
		       what;
	}

	/* a reader as Python's open(name, "rb") is one: read only */
	HANDLE connectTo(const std::string& name)
	{
		return CreateFileA(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	}

	/* What arrives within ms milliseconds, until the line count is reached (0: any). The end of
	 * the pipe ends it and sets closed. */
	std::string receive(HANDLE h, int ms, size_t lines = 0, bool* closed = nullptr)
	{
		std::string data;
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		while (std::chrono::steady_clock::now() < end)
		{
			DWORD avail = 0;
			if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
			{
				if (closed)
					*closed = true;
				break;
			}
			if (avail == 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			std::vector<char> buffer(avail);
			DWORD n = 0;
			if (!ReadFile(h, buffer.data(), avail, &n, nullptr) || (n == 0))
			{
				if (closed)
					*closed = true;
				break;
			}
			data.append(buffer.data(), n);
			if ((lines > 0) &&
			    (static_cast<size_t>(std::count(data.begin(), data.end(), '\n')) >= lines))
				break;
		}
		return data;
	}

	bool waitReaders(const SdlStatsIpc& ipc, size_t count)
	{
		for (int x = 0; x < 200; x++)
		{
			if (ipc.readers() == count)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}
} // namespace

static int checkStream()
{
	const std::string name = uniqueName("stream");
	std::string error;
	auto ipc = SdlStatsIpc::open(name, error);
	if (!ipc)
		(void)fprintf(stderr, "open %s: %s\n", name.c_str(), error.c_str());
	CHECK(ipc);
	CHECK(ipc->path() == name);

	/* a reader before the first line gets nothing until it comes */
	const HANDLE a = connectTo(name);
	CHECK(a != INVALID_HANDLE_VALUE);
	CHECK(waitReaders(*ipc, 1));
	CHECK(receive(a, 100).empty());
	ipc->publish("one");
	CHECK(receive(a, 2000, 1) == "one\n");
	ipc->publish("two");
	CHECK(receive(a, 2000, 1) == "two\n");

	/* a later reader gets the newest line at once */
	const HANDLE b = connectTo(name);
	CHECK(b != INVALID_HANDLE_VALUE);
	CHECK(receive(b, 2000, 1) == "two\n");
	ipc->publish("three");
	CHECK(receive(a, 2000, 1) == "three\n");
	CHECK(receive(b, 2000, 1) == "three\n");

	/* the name is taken: a second client cannot have it */
	std::string error2;
	CHECK(!SdlStatsIpc::open(name, error2));
	CHECK(!error2.empty());

	/* a reader that left is noticed at the next line */
	CloseHandle(b);
	ipc->publish("four");
	CHECK(receive(a, 2000, 1) == "four\n");
	CHECK(waitReaders(*ipc, 1));
	CloseHandle(a);
	ipc->publish("five");
	CHECK(waitReaders(*ipc, 0));

	/* closed: the name is free again */
	ipc.reset();
	CHECK(connectTo(name) == INVALID_HANDLE_VALUE);
	error.clear();
	ipc = SdlStatsIpc::open(name, error);
	CHECK(ipc);
	return 0;
}

static int checkSlowReader()
{
	const std::string name = uniqueName("slow");
	std::string error;
	auto ipc = SdlStatsIpc::open(name, error);
	CHECK(ipc);
	const HANDLE s = connectTo(name);
	CHECK(s != INVALID_HANDLE_VALUE);
	CHECK(waitReaders(*ipc, 1));

	/* far more than the pipe's buffer, while the reader reads nothing */
	const size_t count = 2000;
	const std::string body(4000, 'x');
	for (size_t x = 1; x <= count; x++)
		ipc->publish(std::to_string(x) + ":" + body);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	const auto data = receive(s, 3000, 0);
	size_t lines = 0;
	size_t last = 0;
	size_t start = 0;
	while (start < data.size())
	{
		const auto end = data.find('\n', start);
		CHECK(end != std::string::npos);
		const auto line = data.substr(start, end - start);
		const auto colon = line.find(':');
		CHECK(colon != std::string::npos);
		CHECK(line.substr(colon + 1) == body); /* every line whole */
		const size_t n = std::stoul(line.substr(0, colon));
		CHECK(n > last); /* in order */
		last = n;
		lines++;
		start = end + 1;
	}
	(void)fprintf(stderr, "slow reader: %zu of %zu lines, the last %zu\n", lines, count, last);
	CHECK(last == count);     /* the newest one came */
	CHECK(lines < count / 2); /* the rest was dropped, not queued */
	CloseHandle(s);
	return 0;
}

static int checkTooMany()
{
	const std::string name = uniqueName("many");
	std::string error;
	auto ipc = SdlStatsIpc::open(name, error);
	CHECK(ipc);
	ipc->publish("line");
	std::vector<HANDLE> readers;
	for (size_t x = 0; x < SdlStatsIpc::maxReaders; x++)
	{
		HANDLE h = INVALID_HANDLE_VALUE;
		/* the next instance appears once the thread has taken the last reader */
		for (int t = 0; (t < 200) && (h == INVALID_HANDLE_VALUE); t++)
		{
			h = connectTo(name);
			if (h == INVALID_HANDLE_VALUE)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		CHECK(h != INVALID_HANDLE_VALUE);
		readers.push_back(h);
		CHECK(receive(h, 2000, 1) == "line\n");
	}
	CHECK(waitReaders(*ipc, SdlStatsIpc::maxReaders));
	/* no instance is left for a ninth */
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	const HANDLE extra = connectTo(name);
	const DWORD why = GetLastError();
	CHECK(extra == INVALID_HANDLE_VALUE);
	CHECK(why == ERROR_PIPE_BUSY);
	for (auto r : readers)
		CloseHandle(r);
	return 0;
}

static int checkNames()
{
	/* a bare name gets the pipe prefix */
	const std::string bare =
	    "sdl-stats-ipc-test-" + std::to_string(GetCurrentProcessId()) + "-bare";
	std::string error;
	auto ipc = SdlStatsIpc::open(bare, error);
	CHECK(ipc);
	CHECK(ipc->path() == "\\\\.\\pipe\\" + bare);
	ipc.reset();

	/* not a pipe name */
	error.clear();
	CHECK(!SdlStatsIpc::open("\\\\.\\pipe\\a\\b", error));
	CHECK(!error.empty());
	error.clear();
	CHECK(!SdlStatsIpc::open(std::string(300, 'a'), error));
	CHECK(!error.empty());

	/* the default name is per process */
	error.clear();
	const auto name = SdlStatsIpc::defaultPath(error);
	CHECK(name == "\\\\.\\pipe\\sdl-freerdp-" + std::to_string(GetCurrentProcessId()) + ".stats");
	return 0;
}

int TestSDLStatsIpc([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	for (auto* check : { checkStream, checkSlowReader, checkTooMany, checkNames })
	{
		const int rc = check();
		if (rc != 0)
			return rc;
	}
	return 0;
}
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../../SDL3/sdl_stats_ipc.hpp"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return -1;                                                       \
		}                                                                    \
	} while (0)

namespace
{
	using Native = int;
	constexpr Native invalidSocket = -1;
	void closeSocket(Native s)
	{
		close(s);
	}
	int pollOne(Native s, short events, int ms)
	{
		pollfd fd = {};
		fd.fd = s;
		fd.events = events;
		return poll(&fd, 1, ms);
	}
	unsigned long pid()
	{
		return static_cast<unsigned long>(getpid());
	}

	Native connectTo(const std::string& path)
	{
		sockaddr_un address = {};
		address.sun_family = AF_UNIX;
		if (path.size() >= sizeof(address.sun_path))
			return invalidSocket;
		memcpy(address.sun_path, path.c_str(), path.size() + 1);
		const Native s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (s == invalidSocket)
			return s;
		if (connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			closeSocket(s);
			return invalidSocket;
		}
		return s;
	}

	/* a socket file nobody listens on, as a client that died leaves it */
	bool staleSocket(const std::string& path)
	{
		sockaddr_un address = {};
		address.sun_family = AF_UNIX;
		memcpy(address.sun_path, path.c_str(), path.size() + 1);
		const Native s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (s == invalidSocket)
			return false;
		const bool ok = bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
		closeSocket(s);
		return ok;
	}

	/* What arrives within ms milliseconds, until the line count is reached (0: any). EOF ends
	 * it and sets closed. */
	std::string receive(Native s, int ms, size_t lines = 0, bool* closed = nullptr)
	{
		std::string data;
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		for (;;)
		{
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			                      end - std::chrono::steady_clock::now())
			                      .count();
			if (left <= 0)
				break;
			if (pollOne(s, POLLIN, static_cast<int>(left)) <= 0)
				break;
			char buffer[65536];
			const auto n = recv(s, buffer, sizeof(buffer), 0);
			if (n <= 0)
			{
				if (closed)
					*closed = true;
				break;
			}
			data.append(buffer, static_cast<size_t>(n));
			if ((lines > 0) &&
			    (static_cast<size_t>(std::count(data.begin(), data.end(), '\n')) >= lines))
				break;
		}
		return data;
	}

	bool waitReaders(const SdlStatsIpc& ipc, size_t count)
	{
		for (int x = 0; x < 200; x++)
		{
			if (ipc.readers() == count)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}

	bool exists(const std::string& path)
	{
		std::error_code ec;
		return std::filesystem::exists(std::filesystem::symlink_status(path, ec));
	}

	std::filesystem::path makeDir(const char* name)
	{
		auto dir = std::filesystem::temp_directory_path() /
		           ("sdl-stats-ipc-" + std::to_string(pid()) + "-" + name);
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		std::filesystem::create_directory(dir);
		std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
		                             std::filesystem::perm_options::replace);
		return dir;
	}
} // namespace

static int checkStream()
{
	const auto dir = makeDir("stream");
	const std::string path = (dir / "s.sock").string();
	std::string error;
	auto ipc = SdlStatsIpc::open(path, error);
	if (!ipc)
		(void)fprintf(stderr, "open %s: %s\n", path.c_str(), error.c_str());
	CHECK(ipc);
	CHECK(error.empty());
	CHECK(ipc->path() == path);
	CHECK(exists(path));
	struct stat st = {};
	CHECK(lstat(path.c_str(), &st) == 0);
	CHECK(S_ISSOCK(st.st_mode));
	CHECK((st.st_mode & 0777) == 0600);

	/* a reader before the first line gets nothing until it comes */
	const Native a = connectTo(path);
	CHECK(a != invalidSocket);
	CHECK(waitReaders(*ipc, 1));
	CHECK(receive(a, 100).empty());
	ipc->publish("one");
	CHECK(receive(a, 2000, 1) == "one\n");
	ipc->publish("two");
	CHECK(receive(a, 2000, 1) == "two\n");

	/* what a reader sends is ignored */
	CHECK(send(a, "garbage\n", 8, 0) == 8);

	/* a later reader gets the newest line at once */
	const Native b = connectTo(path);
	CHECK(b != invalidSocket);
	CHECK(receive(b, 2000, 1) == "two\n");
	ipc->publish("three");
	CHECK(receive(a, 2000, 1) == "three\n");
	CHECK(receive(b, 2000, 1) == "three\n");

	/* a second client cannot take the path */
	std::string error2;
	CHECK(!SdlStatsIpc::open(path, error2));
	CHECK(!error2.empty());

	closeSocket(b);
	CHECK(waitReaders(*ipc, 1));
	closeSocket(a);
	CHECK(waitReaders(*ipc, 0));

	ipc.reset();
	CHECK(!exists(path));
	std::filesystem::remove_all(dir);
	return 0;
}

static int checkSlowReader()
{
	const auto dir = makeDir("slow");
	const std::string path = (dir / "s.sock").string();
	std::string error;
	auto ipc = SdlStatsIpc::open(path, error);
	if (!ipc)
		(void)fprintf(stderr, "open %s: %s\n", path.c_str(), error.c_str());
	CHECK(ipc);
	const Native s = connectTo(path);
	CHECK(s != invalidSocket);
	CHECK(waitReaders(*ipc, 1));

	/* far more than any socket buffer, while the reader reads nothing */
	const size_t count = 2000;
	const std::string body(4000, 'x');
	for (size_t x = 1; x <= count; x++)
		ipc->publish(std::to_string(x) + ":" + body);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	const auto data = receive(s, 3000, 0);
	size_t lines = 0;
	size_t last = 0;
	size_t start = 0;
	while (start < data.size())
	{
		const auto end = data.find('\n', start);
		CHECK(end != std::string::npos);
		const auto line = data.substr(start, end - start);
		const auto colon = line.find(':');
		CHECK(colon != std::string::npos);
		CHECK(line.substr(colon + 1) == body); /* every line whole */
		const size_t n = std::stoul(line.substr(0, colon));
		CHECK(n > last); /* in order */
		last = n;
		lines++;
		start = end + 1;
	}
	(void)fprintf(stderr, "slow reader: %zu of %zu lines, the last %zu\n", lines, count, last);
	CHECK(last == count);     /* the newest one came */
	CHECK(lines < count / 2); /* the rest was dropped, not queued */

	closeSocket(s);
	ipc.reset();
	std::filesystem::remove_all(dir);
	return 0;
}

static int checkTooMany()
{
	const auto dir = makeDir("many");
	const std::string path = (dir / "s.sock").string();
	std::string error;
	auto ipc = SdlStatsIpc::open(path, error);
	if (!ipc)
		(void)fprintf(stderr, "open %s: %s\n", path.c_str(), error.c_str());
	CHECK(ipc);
	ipc->publish("line");
	std::vector<Native> readers;
	for (size_t x = 0; x < SdlStatsIpc::maxReaders; x++)
	{
		readers.push_back(connectTo(path));
		CHECK(readers.back() != invalidSocket);
		CHECK(receive(readers.back(), 2000, 1) == "line\n");
	}
	CHECK(waitReaders(*ipc, SdlStatsIpc::maxReaders));
	const Native extra = connectTo(path);
	if (extra != invalidSocket)
	{
		bool closed = false;
		CHECK(receive(extra, 2000, 0, &closed).empty());
		CHECK(closed);
		closeSocket(extra);
	}
	CHECK(ipc->readers() == SdlStatsIpc::maxReaders);
	for (auto r : readers)
		closeSocket(r);
	ipc.reset();
	std::filesystem::remove_all(dir);
	return 0;
}

static int checkPaths()
{
	const auto dir = makeDir("paths");

	/* a file that is no socket stays as it is */
	const std::string file = (dir / "file").string();
	{
		std::ofstream out(file);
		out << "keep";
	}
	std::string error;
	CHECK(!SdlStatsIpc::open(file, error));
	CHECK(!error.empty());
	{
		std::ifstream in(file);
		std::string content;
		in >> content;
		CHECK(content == "keep");
	}

	/* a socket a dead client left is replaced */
	const std::string stale = (dir / "stale.sock").string();
	CHECK(staleSocket(stale));
	CHECK(exists(stale));
	error.clear();
	auto ipc = SdlStatsIpc::open(stale, error);
	CHECK(ipc);
	ipc.reset();
	CHECK(!exists(stale));

	/* too long for a socket address */
	error.clear();
	CHECK(!SdlStatsIpc::open((dir / std::string(200, 'a')).string(), error));
	CHECK(!error.empty());

	/* a directory others may change is refused, unless it is sticky */
	const auto open = dir / "open";
	std::filesystem::create_directory(open);
	CHECK(chmod(open.c_str(), 0777) == 0);
	error.clear();
	CHECK(!SdlStatsIpc::open((open / "s.sock").string(), error));
	CHECK(!error.empty());
	CHECK(chmod(open.c_str(), 01777) == 0);
	error.clear();
	ipc = SdlStatsIpc::open((open / "s.sock").string(), error);
	CHECK(ipc);
	ipc.reset();

	/* the default path is per process */
	error.clear();
	const auto path = SdlStatsIpc::defaultPath(error);
	if (!path.empty())
		CHECK(path.find(std::to_string(pid())) != std::string::npos);
	else
		CHECK(!error.empty());

	std::filesystem::remove_all(dir);
	return 0;
}

int TestSDLStatsIpc([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	int rc = 0;
	for (auto* check : { checkStream, checkSlowReader, checkTooMany, checkPaths })
	{
		rc = check();
		if (rc != 0)
			break;
	}
	return rc;
}
#endif
