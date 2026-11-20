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

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client.h>
#include <freerdp/codec/audio.h>

#include "sdl_overlay_model.hpp"

namespace sdl::overlay
{
	namespace
	{
		struct Label
		{
			const char* id;
			const char* label;
		};

		/* by codec ID, as the web client's Info card names them (codec-stats.js) */
		constexpr std::array<Label, 13> codecs = { {
			{ "UNCOMPRESSED", "Uncompressed" },
			{ "AV1", "AV1" },
			{ "CAVIDEO", "RemoteFX" },
			{ "CLEARCODEC", "ClearCodec" },
			{ "CAPROGRESSIVE", "Progressive" },
			{ "PLANAR", "Planar" },
			{ "AVC420", "H.264 4:2:0 (AVC420)" },
			{ "ALPHA", "Alpha" },
			{ "AVC444", "H.264 4:4:4 (AVC444)" },
			{ "AVC444v2", "H.264 4:4:4 (AVC444v2)" },
			{ "CAPROGRESSIVE_V2", "Progressive v2" },
			{ "HEVC420", "HEVC 4:2:0" },
			{ "HEVC444", "HEVC 4:4:4" },
		} };

		/* paints that carry no codec */
		constexpr std::array<Label, 3> paints = { {
			{ "RDPGFX_CMDID_SOLIDFILL", "SolidFill" },
			{ "RDPGFX_CMDID_SURFACETOSURFACE", "SurfaceToSurface" },
			{ "RDPGFX_CMDID_CACHETOSURFACE", "CacheToSurface" },
		} };

		/* the decoders that report results, as the Info card names them */
		constexpr std::array<Label, 4> decoders = { {
			{ "AV1", "AV1" },
			{ "AVC420", "H.264 4:2:0" },
			{ "AVC444", "H.264 4:4:4" },
			{ "AVC444v2", "H.264 4:4:4 v2" },
		} };

		constexpr const char* codecPrefix = "RDPGFX_CODECID_";
	} // namespace

	static bool startsWith(const std::string& s, const char* prefix)
	{
		return s.compare(0, strlen(prefix), prefix) == 0;
	}

	/* a name function's text, "" where it has none */
	static std::string text(const char* s)
	{
		return s ? s : "";
	}

	static std::string fixed(double v, int precision)
	{
		std::array<char, 64> buffer{};
		(void)snprintf(buffer.data(), buffer.size(), "%.*f", precision, v);
		return buffer.data();
	}

	/* per second since prev; nothing without prev, or for a counter that went down (a new
	 * channel after a reconnect) */
	static std::optional<double> rate(const Names& names, const Reading& now, const Reading* prev,
	                                  const std::string& name)
	{
		if (!prev || (now.ns <= prev->ns))
			return std::nullopt;
		const auto a = names.get(now, name);
		const auto b = names.get(*prev, name);
		if (!a || !b || (*a < *b))
			return std::nullopt;
		const double dt = static_cast<double>(now.ns - prev->ns) / 1e9;
		return static_cast<double>(*a - *b) / dt;
	}

	static std::string per(const std::optional<double>& v, int precision = 1)
	{
		return v ? fixed(*v, precision) : "–";
	}

	/* AV1's counters count AV2 when the server confirmed RDPGFX_CAPVERSION_FRDP_2; without
	 * the caps version the label says both */
	static const char* av1Label(const Names& names, const Reading& now)
	{
#if defined(WITH_GFX_AV2)
		const auto caps = names.get(now, "GFX_CAPS_VERSION").value_or(0);
		if (caps == 0)
			return "AV1/AV2";
		return (caps == RDPGFX_CAPVERSION_FRDP_2) ? "AV2" : "AV1";
#else
		/* a build without AV2 never confirms it */
		WINPR_UNUSED(names);
		WINPR_UNUSED(now);
		return "AV1";
#endif
	}

	/* " 4:4:4" from what the AV decoder last learnt of the stream (RDPGFX_CODECID_AV1_CHROMA:
	 * 420, 422, 444), nothing before it knows */
	static const char* av1Chroma(const Names& names, const Reading& now)
	{
		switch (names.get(now, std::string(codecPrefix) + "AV1_CHROMA").value_or(0))
		{
			case 420:
				return " 4:2:0";
			case 422:
				return " 4:2:2";
			case 444:
				return " 4:4:4";
			default:
				return "";
		}
	}

	static std::string decoderLabel(const Names& names, const Reading& now, const Label& d)
	{
		if (strcmp(d.id, "AV1") == 0)
			return av1Label(names, now);
		return d.label;
	}

	/* a decoder's _HARDWARE counter: 0 unknown, 1 no, 2 yes */
	static const char* hardwareText(uint64_t hardware)
	{
		switch (hardware)
		{
			case 1:
				return "software";
			case 2:
				return "hardware";
			default:
				return "";
		}
	}

	static std::string audioFormat(uint64_t tag)
	{
		if (tag == 0)
			return "no audio yet";
		const std::string name = text(audio_format_get_tag_string(static_cast<UINT16>(tag)));
		if (name.empty())
			return "unknown";
		constexpr const char* prefix = "WAVE_FORMAT_";
		return startsWith(name, prefix) ? name.substr(strlen(prefix)) : name;
	}

	static void videoRows(std::vector<Row>& rows, const Names& names, const Reading& now,
	                      const Reading* prev)
	{
		/* without the graphics channel's statistics the row hides */
		if (!names.has(std::string(codecPrefix) + "AVC420"))
			return;

		std::vector<std::string> parts;
		for (const auto& c : codecs)
		{
			const std::string name = std::string(codecPrefix) + c.id;
			if (names.get(now, name).value_or(0) == 0)
				continue;
			const std::string label =
			    (strcmp(c.id, "AV1") == 0)
			        ? std::string(av1Label(names, now)) + av1Chroma(names, now)
			        : c.label;
			const auto bytes = rate(names, now, prev, name + "_BYTES");
			const auto kbit = bytes ? std::optional<double>(*bytes * 8.0 / 1000.0) : std::nullopt;
			parts.push_back(label + " " + per(rate(names, now, prev, name), 0) + "/s " +
			                per(kbit, 0) + " kbit/s");
		}
		for (const auto& p : paints)
		{
			if (names.get(now, p.id).value_or(0) == 0)
				continue;
			parts.push_back(std::string(p.label) + " " + per(rate(names, now, prev, p.id), 0) +
			                "/s");
		}

		if (parts.empty())
		{
			rows.push_back({ "Video codecs", "no updates yet" });
			return;
		}
		for (size_t x = 0; x < parts.size(); x++)
			rows.push_back({ (x == 0) ? "Video codecs" : "", parts.at(x) });
	}

	static void decoderResultRows(std::vector<Row>& rows, const Names& names, const Reading& now,
	                              const Reading* prev)
	{
		for (const auto& d : decoders)
		{
			const std::string p = std::string(codecPrefix) + d.id;
			if (!names.has(p + "_PICTURES"))
				continue;
			const auto get = [&](const char* stat) { return names.get(now, p + stat).value_or(0); };
			const auto pictures = get("_PICTURES");
			const auto failed = get("_FAILED");
			const auto noPicture = get("_NO_PICTURE");
			const auto tries = pictures + failed + noPicture;
			if (tries == 0)
				continue;

			const double avgMs =
			    static_cast<double>(get("_DECODE_NS_SUM")) / static_cast<double>(tries) / 1e6;
			const double maxMs = static_cast<double>(get("_DECODE_NS_MAX")) / 1e6;

			std::string second =
			    fixed(avgMs, 1) + " ms per attempt, " + fixed(maxMs, 1) + " ms max";
			if (const auto key = names.get(now, p + "_KEY_FRAMES"))
				second += ", key frames: " + std::to_string(*key);
			const std::string hw = hardwareText(get("_HARDWARE"));
			if (!hw.empty())
				second += ", " + hw;
			if (const auto fallbacks = get("_FALLBACKS"); fallbacks != 0)
				second += ", " + std::to_string(fallbacks) + " fallbacks";

			const std::string chroma = (strcmp(d.id, "AV1") == 0) ? av1Chroma(names, now) : "";
			rows.push_back({ decoderLabel(names, now, d) + chroma + " results",
			                 "pictures: " + std::to_string(pictures) + " (" +
			                     per(rate(names, now, prev, p + "_PICTURES")) +
			                     " /s), failed: " + std::to_string(failed) +
			                     ", no picture: " + std::to_string(noPicture) });
			rows.push_back({ "", second });
		}
	}

	static void audioRows(std::vector<Row>& rows, const Names& names, const Reading& now,
	                      const Reading* prev)
	{
		if (const auto tag = names.get(now, "AUDIO_FORMAT_TAG"))
			rows.push_back({ "Audio codec", audioFormat(*tag) });

		const auto waves = names.get(now, "AUDIO_WAVES").value_or(0);
		if (waves == 0)
			return;
		const auto dropped = names.get(now, "AUDIO_WAVES_OVERRUN").value_or(0) +
		                     names.get(now, "AUDIO_WAVES_UNDECODABLE").value_or(0);
		const auto delay = names.get(now, "AUDIO_DELAY_MS").value_or(0);
		rows.push_back({ "Audio received", per(rate(names, now, prev, "AUDIO_WAVES")) + " /s, " +
		                                       std::to_string(waves) + " total, " +
		                                       std::to_string(dropped) + " dropped, " +
		                                       std::to_string(delay) + " ms delay" });
	}

	static void networkRows(std::vector<Row>& rows, const Names& names, const Reading& now,
	                        const Reading* prev)
	{
		const auto bytesRow = [&](const char* label, const char* name)
		{
			const auto total = names.get(now, name);
			if (!total)
				return;
			const auto bytes = rate(names, now, prev, name);
			const auto kbit = bytes ? std::optional<double>(*bytes * 8.0 / 1000.0) : std::nullopt;
			rows.push_back({ label, per(kbit, 0) + " kbit/s, " +
			                            fixed(static_cast<double>(*total) / 1e6, 1) +
			                            " MB this connection" });
		};
		bytesRow("Network in (RDP)", "NETWORK_BYTES_RECEIVED");
		bytesRow("Network out (RDP)", "NETWORK_BYTES_SENT");

		/* the server's own measurements, 0 until it makes them: not every server does */
		if (const auto rtt = names.get(now, "NETWORK_RTT_AVERAGE_MS"))
		{
			const auto base = names.get(now, "NETWORK_RTT_BASE_MS").value_or(0);
			rows.push_back(
			    { "Round trip (measured by the server)",
			      (*rtt != 0) ? std::to_string(*rtt) + " ms (lowest " + std::to_string(base) + ")"
			                  : "not measured" });
		}
		if (const auto kbps = names.get(now, "NETWORK_BANDWIDTH_KBPS"))
			rows.push_back({ "Bandwidth (measured by the server)",
			                 (*kbps != 0) ? std::to_string(*kbps) + " kbit/s" : "not measured" });
	}
	void History::add(Reading reading)
	{
		if (!_readings.empty() && (reading.ns <= _readings.back().ns))
			_readings.clear();
		_readings.push_back(std::move(reading));

		/* keep the newest reading that is at least five minutes old: the 5 min window's base */
		const uint64_t newest = _readings.back().ns;
		while ((_readings.size() > 1) && (newest - _readings.at(1).ns >= longestNs))
			_readings.pop_front();
	}

	void History::clear()
	{
		_readings.clear();
	}

	const Reading* History::latest() const
	{
		return _readings.empty() ? nullptr : &_readings.back();
	}

	const Reading* History::before(uint64_t ageNs) const
	{
		if (_readings.empty())
			return nullptr;
		const uint64_t newest = _readings.back().ns;
		for (auto it = _readings.rbegin(); it != _readings.rend(); ++it)
		{
			if (newest - it->ns >= ageNs)
				return &*it;
		}
		return nullptr;
	}

	size_t History::size() const
	{
		return _readings.size();
	}

	std::vector<Row> averageRows(const Names& names, const History& history)
	{
		std::vector<Row> rows;
		const Reading* now = history.latest();
		if (!now)
			return rows;

		struct Window
		{
			const char* label;
			uint64_t ns;
		};
		constexpr std::array<Window, 4> windows = { {
			{ "1 s", 1 }, /* the reading before, about a second earlier */
			{ "5 s", 5000000000ull },
			{ "1 min", 60000000000ull },
			{ "5 min", History::longestNs },
		} };

		const auto averaged =
		    [&](const char* label, const char* name, double scale, int precision, const char* unit)
		{
			if (!names.has(name))
				return;
			std::string value;
			for (const auto& w : windows)
			{
				auto r = rate(names, *now, history.before(w.ns), name);
				if (r)
					*r *= scale;
				value +=
				    (value.empty() ? "" : " · ") + std::string(w.label) + " " + per(r, precision);
			}
			rows.push_back({ label, value + " " + unit });
		};
		averaged("Frames", "RDPGFX_FRAMES_DECODED", 1.0, 1, "/s");
		averaged("Network in", "NETWORK_BYTES_RECEIVED", 8.0 / 1000.0, 0, "kbit/s");
		averaged("Network out", "NETWORK_BYTES_SENT", 8.0 / 1000.0, 0, "kbit/s");
		return rows;
	}

	Names::Names(const std::vector<std::string>& names)
	{
		for (size_t x = 0; x < names.size(); x++)
		{
			const auto& name = names.at(x);
			if (name.empty() || (name == "FREERDP_CLIENT_STATS_UNUSED") ||
			    startsWith(name, "RDPGFX_CODECID_UNKNOWN") ||
			    startsWith(name, "RDPGFX_STATS_UNUSED"))
				continue;
			_index.emplace(name, x);
		}
	}

	std::optional<uint64_t> Names::get(const Reading& r, const std::string& name) const
	{
		const auto it = _index.find(name);
		if ((it == _index.end()) || (it->second >= r.values.size()))
			return std::nullopt;
		return r.values.at(it->second);
	}

	bool Names::has(const std::string& name) const
	{
		return _index.find(name) != _index.end();
	}

	std::vector<Row> realtimeRows(const Names& names, const Reading& now, const Reading* prev,
	                              const View& view)
	{
		std::vector<Row> rows;
		rows.push_back({ "Resolution", std::to_string(now.desktopWidth) + " × " +
		                                   std::to_string(now.desktopHeight) + ", window " +
		                                   std::to_string(view.windowWidth) + " × " +
		                                   std::to_string(view.windowHeight) });
		videoRows(rows, names, now, prev);
		decoderResultRows(rows, names, now, prev);
		audioRows(rows, names, now, prev);
		networkRows(rows, names, now, prev);

		std::array<char, 32> session{};
		(void)snprintf(session.data(), session.size(), "%" PRIu64 ":%02" PRIu64,
		               view.sessionSeconds / 60, view.sessionSeconds % 60);
		rows.push_back({ "Session", session.data() });
		return rows;
	}

	std::vector<Row> serverRows(const Names& names, const Reading& now)
	{
		std::vector<Row> rows;
		const auto value = [&](const std::string& name)
		{ return names.get(now, name).value_or(0); };
		const auto info = [&](const char* name) -> std::string
		{
			for (const auto& [key, v] : now.strings)
			{
				if (key == name)
					return v;
			}
			return "";
		};

		if (const auto rdp = value("RDP_VERSION"); rdp != 0)
		{
			std::string v = text(freerdp_client_stats_rdp_version_name(rdp));
			const auto server = value("SERVER_RDP_VERSION");
			if ((server != 0) && (server != rdp))
				v += " (server " + text(freerdp_client_stats_rdp_version_name(server)) + ")";
			rows.push_back({ "RDP version", v });
		}

		if (const auto major = value("SERVER_OS_MAJOR_TYPE"); major != 0)
		{
			std::array<char, 128> buffer{};
			rows.push_back({ "Server OS",
			                 text(freerdp_client_stats_os_name(major, value("SERVER_OS_MINOR_TYPE"),
			                                                   buffer.data(), buffer.size())) });
		}

		const auto caps = value("GFX_CAPS_VERSION");
		if (caps != 0)
		{
			std::array<char, 256> buffer{};
			rows.push_back({ "Graphics caps",
			                 text(freerdp_client_stats_caps_version_name(caps)) + ", flags: " +
			                     text(freerdp_client_stats_caps_flags_name(
			                         value("GFX_CAPS_FLAGS"), buffer.data(), buffer.size())) });
		}

		constexpr uint64_t slots = 4;
		for (uint64_t slot = 1; slot <= slots; slot++)
		{
			const std::string prefix = "RDPGFX_SURFACE" + std::to_string(slot) + "_";
			const auto width = value(prefix + "WIDTH");
			if (width == 0)
				continue;
			rows.push_back(
			    { "Surface " + std::to_string(slot),
			      std::to_string(width) + " × " + std::to_string(value(prefix + "HEIGHT")) + ", " +
			          text(freerdp_client_stats_codec_name(value(prefix + "CODECID"), caps)) });
		}
		if (const auto count = value("RDPGFX_SURFACES"); count > slots)
			rows.push_back({ "Surfaces", "+" + std::to_string(count - slots) + " more" });

		const auto host = info("SERVER_HOSTNAME");
		const auto port = value("SERVER_PORT");
		if (!host.empty() && (port != 0))
			rows.push_back({ "Host", host + ":" + std::to_string(port) });
		if (const auto cn = info("SERVER_CERT_CN"); !cn.empty())
			rows.push_back({ "Certificate", cn });
		return rows;
	}

	std::vector<Row> decoderRows(const Names& names, const Reading& now)
	{
		std::vector<Row> rows;
		for (const auto& d : decoders)
		{
			const std::string p = std::string(codecPrefix) + d.id;
			const auto tries = names.get(now, p + "_PICTURES").value_or(0) +
			                   names.get(now, p + "_FAILED").value_or(0) +
			                   names.get(now, p + "_NO_PICTURE").value_or(0);
			if (tries == 0)
				continue;

			std::string value;
			for (const auto& [codec, backend] : now.backends)
			{
				if (codec == d.id)
					value = backend;
			}
			const std::string hw = hardwareText(names.get(now, p + "_HARDWARE").value_or(0));
			if (!hw.empty())
				value += value.empty() ? hw : ", " + hw;
			rows.push_back(
			    { decoderLabel(names, now, d) + " decoder", value.empty() ? "unknown" : value });
		}
		return rows;
	}

	Layout layout(const Box& left, const Box& server, const Box& right, float width, float margin)
	{
		const auto empty = [](const Box& b) { return (b.w <= 0) || (b.h <= 0); };

		Layout l;
		l.left = { margin, margin, left.w, left.h };
		const float serverY = empty(left) ? margin : margin + left.h + margin;
		l.server = { margin, serverY, server.w, server.h };
		l.right = { std::max(0.0F, width - margin - right.w), margin, right.w, right.h };

		/* the left column: the client part and the server part under it */
		const float columnWidth =
		    std::max(empty(left) ? 0.0F : left.w, empty(server) ? 0.0F : server.w);
		const float columnBottom =
		    empty(server) ? (empty(left) ? 0.0F : margin + left.h) : serverY + server.h;
		if ((columnWidth > 0) && (margin + columnWidth + margin + right.w + margin > width))
			l.right.y = columnBottom + margin;
		return l;
	}

	std::string buildSummary(const char* config)
	{
		if (!config)
			return "";

		const std::array<const char*, 3> keys = { "Build type:", "Compiler:",
			                                      "Target architecture:" };
		std::vector<std::string> parts;
		std::istringstream lines(config);
		std::string line;
		while (std::getline(lines, line))
		{
			for (const auto* key : keys)
			{
				if (!startsWith(line, key))
					continue;
				auto value = line.substr(strlen(key));
				const auto start = value.find_first_not_of(' ');
				if (start != std::string::npos)
					parts.push_back(value.substr(start));
			}
		}

		std::string summary;
		for (const auto& part : parts)
			summary += summary.empty() ? part : ", " + part;
		return summary;
	}
} // namespace sdl::overlay
