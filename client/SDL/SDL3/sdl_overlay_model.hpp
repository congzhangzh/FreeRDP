/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: what the stats and info overlay shows
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

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/* Turns readings of the client's runtime statistics into the overlay's rows. Nothing here
 * counts or touches a session, so it is tested without one. */
namespace sdl::overlay
{
	/** One reading of the statistics, taken on the session thread. */
	struct Reading
	{
		uint64_t ns = 0;              /**< when it was taken, in ns from any base */
		uint64_t wallMs = 0;          /**< the same moment by the wall clock, ms since 1970 (UTC) */
		std::vector<uint64_t> values; /**< by statistic index */
		uint32_t desktopWidth = 0;
		uint32_t desktopHeight = 0;
		/** decoder backend names, by codec label ("AVC420", "AV1"), empty if none decoded */
		std::vector<std::pair<std::string, std::string>> backends;
		/** the information strings by name (SERVER_HOSTNAME, ...), only those the session has */
		std::vector<std::pair<std::string, std::string>> strings;
	};

	/** The statistics' names, looked up once: an index means nothing from one version to the
	 * next, a name does. Unused slots and repeated names are left out, the first one wins. */
	class Names
	{
	  public:
		explicit Names(const std::vector<std::string>& names);

		/** @return the value of name in r, or nothing when this client has no such statistic */
		[[nodiscard]] std::optional<uint64_t> get(const Reading& r, const std::string& name) const;
		[[nodiscard]] bool has(const std::string& name) const;

		/** @return false for a name that is no statistic: empty, or an unused slot */
		[[nodiscard]] static bool used(const std::string& name);

	  private:
		std::unordered_map<std::string, size_t> _index;
	};

	struct Row
	{
		std::string label;
		std::string value;
	};

	/** What the overlay knows besides the statistics. */
	struct View
	{
		int windowWidth = 0;
		int windowHeight = 0;
		uint64_t sessionSeconds = 0;
	};

	/** The readings of the last five minutes, oldest first, to average the rates over 5 s,
	 * 1 min and 5 min. It keeps one reading older than five minutes as the base of that
	 * window and drops the rest. */
	class History
	{
	  public:
		static constexpr uint64_t longestNs = 300ull * 1000000000ull;

		/** a reading that is not newer than the last one starts the history over */
		void add(Reading reading);
		void clear();

		/** @return the newest reading, or nullptr */
		[[nodiscard]] const Reading* latest() const;
		/** @return the newest reading at least ageNs older than the newest one, or nullptr
		 * when the history does not reach that far back */
		[[nodiscard]] const Reading* before(uint64_t ageNs) const;

		[[nodiscard]] size_t size() const;

	  private:
		std::deque<Reading> _readings;
	};

	/** Rates over the last 1 s, 5 s, 1 min and 5 min: frames, network in and out. A window the
	 * history does not cover yet, or in which a counter went down (a new connection), shows
	 * "–". Nothing without a reading. */
	[[nodiscard]] std::vector<Row> averageRows(const Names& names, const History& history);

	/** The real-time part: picture, decoders, network and sound, as the web client's Info card
	 * shows them. prev: the reading before now, or nullptr; a counter that went down gives no
	 * rate. */
	[[nodiscard]] std::vector<Row> realtimeRows(const Names& names, const Reading& now,
	                                            const Reading* prev, const View& view);

	/** The server part, under the client part: what the server announced (RDP version, OS,
	 * the graphics caps it confirmed, its surfaces and their codecs) and where this client
	 * connected, with the names of the client statistics' name functions, as the web client's
	 * Info card shows them, and what a FreeRDP server measured in its last period (its
	 * statistics channel, /astats). A row the server has not told yet is left out. */
	[[nodiscard]] std::vector<Row> serverRows(const Names& names, const Reading& now);

	/** The client part's decoder rows: per decoder that has worked, its backend and whether it
	 * runs in hardware (from the decoder's own _HARDWARE counter: 0 unknown, 1 no, 2 yes). */
	[[nodiscard]] std::vector<Row> decoderRows(const Names& names, const Reading& now);

	struct Box
	{
		float x = 0;
		float y = 0;
		float w = 0;
		float h = 0;
	};

	struct Layout
	{
		Box left;
		Box server;
		Box right;
	};

	/** Where the three parts go in a window width pixels wide: the client part at the top left,
	 * the server part under it, the real-time part at the top right, or under the other two when
	 * it does not fit beside them, so none covers another. A part that is wider than the window
	 * is cut at its edge. An empty part has the size 0. */
	[[nodiscard]] Layout layout(const Box& left, const Box& server, const Box& right, float width,
	                            float margin);

	/** The build lines of freerdp_get_build_config(): its type, compiler and architecture. */
	[[nodiscard]] std::string buildSummary(const char* config);
} // namespace sdl::overlay
