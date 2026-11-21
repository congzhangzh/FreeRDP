/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: the overlay's numbers as one line of JSON
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
#include <string>
#include <vector>

#include "sdl_overlay_model.hpp"

/* What /sdl-stats-ipc sends: one reading and the overlay's rows as one line of JSON. Nothing
 * here touches a session, so it is tested without one. */
namespace sdl::stats
{
	/** The schema's name and version. Adding a field keeps the version; renaming or removing
	 * one, or changing what a value means, raises it. */
	constexpr const char* schemaName = "freerdp-sdl-stats";
	constexpr unsigned schemaVersion = 1;

	struct Snapshot
	{
		uint64_t seq = 0;        /**< 1 for the first line, then one more per line */
		uint64_t connection = 0; /**< 1 for the first connection, one more per reconnect */
		uint64_t sessionSeconds = 0;
		int windowWidth = 0;
		int windowHeight = 0;
		/** the statistics' names, by index; unused and repeated names are left out */
		const std::vector<std::string>* names = nullptr;
		const sdl::overlay::Reading* reading = nullptr;
		const std::vector<sdl::overlay::Row>* client = nullptr;
		const std::vector<sdl::overlay::Row>* server = nullptr;
		const std::vector<sdl::overlay::Row>* realtime = nullptr;
	};

	/** @return s as a JSON string, quotes included; bytes that are no UTF-8 become U+FFFD */
	[[nodiscard]] std::string quote(const std::string& s);

	/** @return the snapshot as one JSON object, without a line end. Integers are exact. */
	[[nodiscard]] std::string toJson(const Snapshot& snapshot);
} // namespace sdl::stats
