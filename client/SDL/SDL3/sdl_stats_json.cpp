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

#include <array>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "sdl_stats_json.hpp"

namespace sdl::stats
{
	/* the length of the UTF-8 sequence at s[i], or 0 when it is none (overlong, a surrogate,
	 * beyond U+10FFFF or cut short) */
	static size_t utf8Length(const std::string& s, size_t i)
	{
		const auto at = [&](size_t k) { return static_cast<unsigned char>(s[i + k]); };
		const auto cont = [&](size_t k) { return (i + k < s.size()) && ((at(k) & 0xC0) == 0x80); };
		const unsigned char c = at(0);
		if (c < 0x80)
			return 1;
		if ((c >= 0xC2) && (c <= 0xDF))
			return cont(1) ? 2 : 0;
		if ((c >= 0xE0) && (c <= 0xEF))
		{
			if (!cont(1) || !cont(2))
				return 0;
			if ((c == 0xE0) && (at(1) < 0xA0))
				return 0;
			if ((c == 0xED) && (at(1) >= 0xA0))
				return 0;
			return 3;
		}
		if ((c >= 0xF0) && (c <= 0xF4))
		{
			if (!cont(1) || !cont(2) || !cont(3))
				return 0;
			if ((c == 0xF0) && (at(1) < 0x90))
				return 0;
			if ((c == 0xF4) && (at(1) >= 0x90))
				return 0;
			return 4;
		}
		return 0;
	}

	std::string iso8601(const std::tm& local, long offsetSeconds)
	{
		const char sign = (offsetSeconds < 0) ? '-' : '+';
		const long offset = std::labs(offsetSeconds) / 60;
		char buffer[40] = {};
		(void)std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld",
		                    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
		                    local.tm_min, local.tm_sec, sign, offset / 60, offset % 60);
		return buffer;
	}

	std::string localIso8601(std::time_t t)
	{
		std::tm local = {};
#if defined(_WIN32)
		if (localtime_s(&local, &t) != 0)
			return {};
		/* the offset: this moment read as local time, minus the same fields read as UTC */
		std::tm utc = {};
		if (gmtime_s(&utc, &t) != 0)
			return {};
		utc.tm_isdst = -1;
		const std::time_t utcAsLocal = std::mktime(&utc);
		const long offset = static_cast<long>(std::difftime(t, utcAsLocal));
#else
		if (!localtime_r(&t, &local))
			return {};
		const long offset = local.tm_gmtoff;
#endif
		return iso8601(local, offset);
	}

	std::string quote(const std::string& s)
	{
		static constexpr std::array<char, 16> hex = { '0', '1', '2', '3', '4', '5', '6', '7',
			                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
		std::string out;
		out.reserve(s.size() + 2);
		out += '"';
		for (size_t i = 0; i < s.size();)
		{
			const auto c = static_cast<unsigned char>(s[i]);
			const size_t n = utf8Length(s, i);
			if (n == 0)
			{
				out += "\xEF\xBF\xBD";
				i++;
				continue;
			}
			if (n > 1)
			{
				out.append(s, i, n);
				i += n;
				continue;
			}
			switch (c)
			{
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				default:
					if ((c < 0x20) || (c == 0x7F))
					{
						out += "\\u00";
						out += hex.at(c >> 4);
						out += hex.at(c & 0x0F);
					}
					else
						out += static_cast<char>(c);
					break;
			}
			i++;
		}
		out += '"';
		return out;
	}

	static void rows(std::string& out, const char* key, const std::vector<sdl::overlay::Row>* list)
	{
		out += quote(key);
		out += ":[";
		if (list)
		{
			bool first = true;
			for (const auto& row : *list)
			{
				if (!first)
					out += ',';
				first = false;
				out += '[' + quote(row.label) + ',' + quote(row.value) + ']';
			}
		}
		out += ']';
	}

	template <typename Pairs>
	static void object(std::string& out, const char* key, const Pairs& pairs)
	{
		out += quote(key);
		out += ":{";
		bool first = true;
		std::unordered_set<std::string> seen;
		for (const auto& [name, value] : pairs)
		{
			/* the first one wins, as in the overlay */
			if (!seen.insert(name).second)
				continue;
			if (!first)
				out += ',';
			first = false;
			out += quote(name) + ':' + quote(value);
		}
		out += '}';
	}

	std::string toJson(const Snapshot& snapshot)
	{
		const auto* reading = snapshot.reading;
		std::string out = "{";
		out += "\"schema\":" + quote(schemaName);
		out += ",\"version\":" + std::to_string(schemaVersion);
		out += ",\"seq\":" + std::to_string(snapshot.seq);
		out += ",\"ns\":" + std::to_string(reading ? reading->ns : 0);
		/* the wall clock, to join these lines with the server's */
		out += ",\"time_ms\":" + std::to_string(reading ? reading->wallMs : 0);
		out += ",\"connection\":" + std::to_string(snapshot.connection);
		out += ",\"started_at\":" + quote(snapshot.startedAt);
		out += ",\"session_seconds\":" + std::to_string(snapshot.sessionSeconds);
		out += ",\"desktop\":[" + std::to_string(reading ? reading->desktopWidth : 0) + ',' +
		       std::to_string(reading ? reading->desktopHeight : 0) + ']';
		out += ",\"window\":[" + std::to_string(snapshot.windowWidth) + ',' +
		       std::to_string(snapshot.windowHeight) + ']';

		out += ",\"counters\":{";
		if (reading && snapshot.names)
		{
			const auto& names = *snapshot.names;
			bool first = true;
			std::unordered_set<std::string> seen;
			for (size_t x = 0; (x < names.size()) && (x < reading->values.size()); x++)
			{
				const auto& name = names.at(x);
				if (!sdl::overlay::Names::used(name))
					continue;
				if (!seen.insert(name).second)
					continue;
				if (!first)
					out += ',';
				first = false;
				out += quote(name) + ':' + std::to_string(reading->values.at(x));
			}
		}
		out += '}';

		static const std::vector<std::pair<std::string, std::string>> none;
		out += ',';
		object(out, "strings", reading ? reading->strings : none);
		out += ',';
		object(out, "backends", reading ? reading->backends : none);

		out += ",\"rows\":{";
		rows(out, "client", snapshot.client);
		out += ',';
		rows(out, "server", snapshot.server);
		out += ',';
		rows(out, "realtime", snapshot.realtime);
		out += "}}";
		return out;
	}
} // namespace sdl::stats
