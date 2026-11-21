/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: the overlay's numbers on a local socket (/sdl-stats-ipc)
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

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/** A read-only stream of lines on a local stream socket (AF_UNIX, on Windows too), for a
 * program of the same user that measures the client.
 *
 * A reader that connects gets the newest line at once, then each new line, until it closes.
 * What it sends is ignored. A reader that reads slower than lines come gets the newest one when
 * it is ready, never a queue. Only the user that runs the client may connect: the socket file
 * is 0600 in a directory only that user may change, and on Linux and macOS each reader's user is
 * checked too. One thread of its own does all the socket work; publish() only hands it a line. */
class SdlStatsIpc
{
  public:
	static constexpr size_t maxReaders = 8;

	/** Open the socket at path. An existing file there is replaced only when it is a socket of
	 * this user that nobody listens on.
	 * @return the server, or nullptr with the reason in error */
	[[nodiscard]] static std::unique_ptr<SdlStatsIpc> open(const std::string& path,
	                                                       std::string& error);

	/** The path used without one: per user and per process.
	 * @return the path, or an empty string with the reason in error */
	[[nodiscard]] static std::string defaultPath(std::string& error);

	SdlStatsIpc(const SdlStatsIpc& other) = delete;
	SdlStatsIpc(SdlStatsIpc&& other) = delete;
	SdlStatsIpc& operator=(const SdlStatsIpc& other) = delete;
	SdlStatsIpc& operator=(SdlStatsIpc&& other) = delete;

	/** Closes every reader and the socket and removes the socket file. */
	~SdlStatsIpc();

	/** Any thread: send line (without its line end) to every reader. */
	void publish(std::string line);

	[[nodiscard]] const std::string& path() const;

	/** @return the readers connected now */
	[[nodiscard]] size_t readers() const;

	/* the platform's socket, wide enough for both */
	using Socket = intptr_t;

  private:
	struct Impl;

	SdlStatsIpc(std::string path, Socket listener);
	void run();
	void wake();

	std::string _path;
	Socket _listener;
	std::unique_ptr<Impl> _impl;
	std::atomic<bool> _stop = false;
	std::atomic<size_t> _readers = 0;

	std::mutex _mutex;
	std::string _latest; /* with its line end, empty before the first */
	uint64_t _latestSeq = 0;

	std::thread _thread;
};
