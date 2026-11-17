#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/constants.h>
#include <freerdp/settings_types.h>

#include "../../SDL3/sdl_overlay_model.hpp"

using sdl::overlay::Names;
using sdl::overlay::Reading;
using sdl::overlay::Row;

namespace
{
	/* what AV1's counters are called before the caps version is known */
#if defined(WITH_GFX_AV2)
	const std::string av1Unknown = "AV1/AV2";
#else
	const std::string av1Unknown = "AV1";
#endif

	const std::vector<std::string> names = {
		"NETWORK_BYTES_RECEIVED",
		"NETWORK_BYTES_SENT",
		"NETWORK_BANDWIDTH_KBPS",
		"NETWORK_RTT_BASE_MS",
		"NETWORK_RTT_AVERAGE_MS",
		"AUDIO_WAVES",
		"AUDIO_WAVES_OVERRUN",
		"AUDIO_WAVES_UNDECODABLE",
		"AUDIO_FORMAT_TAG",
		"AUDIO_DELAY_MS",
		"GFX_CAPS_VERSION",
		"RDPGFX_CODECID_UNKNOWN",
		"RDPGFX_CODECID_AVC420",
		"RDPGFX_CODECID_AVC420_BYTES",
		"RDPGFX_CMDID_CACHETOSURFACE",
		"RDPGFX_CODECID_AV1_PICTURES",
		"RDPGFX_CODECID_AV1_FAILED",
		"RDPGFX_CODECID_AV1_NO_PICTURE",
		"RDPGFX_CODECID_AV1_DECODE_NS_SUM",
		"RDPGFX_CODECID_AV1_DECODE_NS_MAX",
		"RDPGFX_CODECID_AV1_HARDWARE",
		"RDPGFX_CODECID_AV1_CHROMA",
		"RDPGFX_CODECID_AV1",
		"RDPGFX_CODECID_UNKNOWN_PICTURES",
		"FREERDP_CLIENT_STATS_UNUSED",
		"NETWORK_BYTES_RECEIVED", /* a repeated name: the first one counts */
	};

	enum
	{
		RX,
		TX,
		KBPS,
		RTT_BASE,
		RTT,
		WAVES,
		OVERRUN,
		UNDECODABLE,
		TAG,
		DELAY,
		CAPS,
		UNKNOWN,
		AVC420,
		AVC420_BYTES,
		CACHE,
		AV1_PICTURES,
		AV1_FAILED,
		AV1_NO_PICTURE,
		AV1_NS_SUM,
		AV1_NS_MAX,
		AV1_HARDWARE,
		AV1_CHROMA,
		AV1_COMMANDS,
		UNKNOWN_PICTURES,
		UNUSED,
		RX_AGAIN,
		COUNT
	};

	Reading reading(uint64_t seconds)
	{
		Reading r;
		r.ns = seconds * 1000000000ull;
		r.values.resize(COUNT);
		r.desktopWidth = 1920;
		r.desktopHeight = 1080;
		return r;
	}

	const Row* find(const std::vector<Row>& rows, const std::string& label)
	{
		for (const auto& row : rows)
		{
			if (row.label == label)
				return &row;
		}
		return nullptr;
	}

	/* the row after the one labelled label */
	const Row* next(const std::vector<Row>& rows, const std::string& label)
	{
		for (size_t x = 0; x + 1 < rows.size(); x++)
		{
			if (rows[x].label == label)
				return &rows[x + 1];
		}
		return nullptr;
	}

	bool expect(const Row* row, const std::string& value, int line)
	{
		if (!row)
		{
			(void)fprintf(stderr, "line %d: no row, expected '%s'\n", line, value.c_str());
			return false;
		}
		if (row->value != value)
		{
			(void)fprintf(stderr, "line %d: '%s' is '%s', expected '%s'\n", line,
			              row->label.c_str(), row->value.c_str(), value.c_str());
			return false;
		}
		return true;
	}

#define EXPECT(row, value)                 \
	if (!expect((row), (value), __LINE__)) \
	return __LINE__
#define CHECK(cond)                                              \
	if (!(cond))                                                 \
	{                                                            \
		(void)fprintf(stderr, "line %d: %s\n", __LINE__, #cond); \
		return __LINE__;                                         \
	}

	int checkNames()
	{
		const Names n(names);
		auto r = reading(1);
		r.values[RX] = 7;
		r.values[RX_AGAIN] = 99;
		r.values[UNKNOWN] = 5;
		CHECK(n.get(r, "NETWORK_BYTES_RECEIVED").value_or(0) == 7);
		CHECK(!n.has("RDPGFX_CODECID_UNKNOWN"));
		CHECK(!n.has("RDPGFX_CODECID_UNKNOWN_PICTURES"));
		CHECK(!n.has("FREERDP_CLIENT_STATS_UNUSED"));
		CHECK(!n.get(r, "NO_SUCH_NAME"));

		/* a reading shorter than the names, as from another client */
		Reading shorter;
		shorter.values.resize(2);
		CHECK(!n.get(shorter, "AUDIO_WAVES"));
		return 0;
	}

	int checkRates()
	{
		const Names n(names);
		auto prev = reading(10);
		auto now = reading(12);
		prev.values[RX] = 1000000;
		now.values[RX] = 1500000; /* 250000 bytes a second: 2000 kbit/s */
		now.values[TX] = 2500000;
		prev.values[AVC420] = 100;
		now.values[AVC420] = 160;
		prev.values[AVC420_BYTES] = 0;
		now.values[AVC420_BYTES] = 200000;
		now.values[CACHE] = 8;
		prev.values[CACHE] = 0;
		now.values[AV1_PICTURES] = 90;
		now.values[AV1_FAILED] = 2;
		now.values[AV1_NO_PICTURE] = 8;
		now.values[AV1_NS_SUM] = 400000000; /* 100 tries: 4 ms each */
		now.values[AV1_NS_MAX] = 12500000;
		now.values[AV1_HARDWARE] = 1;
		prev.values[AV1_PICTURES] = 30;

		const sdl::overlay::View view{ 1280, 720, 125 };
		const auto rows = sdl::overlay::realtimeRows(n, now, &prev, view);
		EXPECT(find(rows, "Resolution"), "1920 × 1080, window 1280 × 720");
		EXPECT(find(rows, "Network in (RDP)"), "2000 kbit/s, 1.5 MB this connection");
		/* without TX in prev: a counter that went up from 0 */
		EXPECT(find(rows, "Network out (RDP)"), "10000 kbit/s, 2.5 MB this connection");
		EXPECT(find(rows, "Video codecs"), "H.264 4:2:0 (AVC420) 30/s 800 kbit/s");
		EXPECT(next(rows, "Video codecs"), "CacheToSurface 4/s");
		/* no caps version yet: AV1's counters can be AV2's */
		EXPECT(find(rows, av1Unknown + " results"),
		       "pictures: 90 (30.0 /s), failed: 2, no picture: 8");
		EXPECT(next(rows, av1Unknown + " results"), "4.0 ms per attempt, 12.5 ms max, software");
		EXPECT(find(rows, "Session"), "2:05");

		/* no earlier reading: no rate */
		const auto first = sdl::overlay::realtimeRows(n, now, nullptr, view);
		EXPECT(find(first, "Network in (RDP)"), "– kbit/s, 1.5 MB this connection");
		EXPECT(find(first, av1Unknown + " results"),
		       "pictures: 90 (– /s), failed: 2, no picture: 8");

		/* a counter that went down is a new channel, never a negative rate */
		prev.values[AV1_PICTURES] = 500;
		const auto reset = sdl::overlay::realtimeRows(n, now, &prev, view);
		EXPECT(find(reset, av1Unknown + " results"),
		       "pictures: 90 (– /s), failed: 2, no picture: 8");
		return 0;
	}

	int checkLabels()
	{
		const Names n(names);
		auto now = reading(1);
		now.values[AV1_PICTURES] = 1;
		now.values[AV1_HARDWARE] = 2;

#if defined(WITH_GFX_AV2)
		now.values[CAPS] = RDPGFX_CAPVERSION_FRDP_2;
		auto rows = sdl::overlay::realtimeRows(n, now, nullptr, {});
		CHECK(find(rows, "AV2 results"));
		EXPECT(next(rows, "AV2 results"), "0.0 ms per attempt, 0.0 ms max, hardware");
#endif

		/* any other caps version: AV1 */
		now.values[CAPS] = RDPGFX_CAPVERSION_107;
		auto rows107 = sdl::overlay::realtimeRows(n, now, nullptr, {});
		CHECK(find(rows107, "AV1 results"));
		EXPECT(next(rows107, "AV1 results"), "0.0 ms per attempt, 0.0 ms max, hardware");

		/* the hardware is not known: nothing said */
		now.values[AV1_HARDWARE] = 0;
		const auto unknownHw = sdl::overlay::realtimeRows(n, now, nullptr, {});
		EXPECT(next(unknownHw, "AV1 results"), "0.0 ms per attempt, 0.0 ms max");

		/* the chroma the decoder learnt from the stream: after the codec's name in its codec row
		 * and its decoder rows, nothing while it is not known */
		now.values[AV1_COMMANDS] = 5;
		const auto noChroma = sdl::overlay::realtimeRows(n, now, nullptr, {});
		CHECK(find(noChroma, "AV1 results"));
		CHECK(find(noChroma, "Video codecs") &&
		      (find(noChroma, "Video codecs")->value.rfind("AV1 –/s", 0) == 0));
		now.values[AV1_CHROMA] = 444;
		const auto i444 = sdl::overlay::realtimeRows(n, now, nullptr, {});
		CHECK(find(i444, "AV1 4:4:4 results"));
		CHECK(find(i444, "Video codecs") &&
		      (find(i444, "Video codecs")->value.rfind("AV1 4:4:4 –/s", 0) == 0));
		now.values[AV1_CHROMA] = 420;
		const auto i420 = sdl::overlay::realtimeRows(n, now, nullptr, {});
		CHECK(find(i420, "AV1 4:2:0 results"));
		now.values[AV1_CHROMA] = 0;
		now.values[AV1_COMMANDS] = 0;

		/* nothing decoded: no codec and no decoder rows */
		const auto empty = sdl::overlay::realtimeRows(n, reading(1), nullptr, {});
		EXPECT(find(empty, "Video codecs"), "no updates yet");
		CHECK(!find(empty, av1Unknown + " results"));
		return 0;
	}

	int checkNetworkAndAudio()
	{
		const Names n(names);
		auto now = reading(1);
		auto rows = sdl::overlay::realtimeRows(n, now, nullptr, {});
		EXPECT(find(rows, "Round trip (measured by the server)"), "not measured");
		EXPECT(find(rows, "Bandwidth (measured by the server)"), "not measured");
		EXPECT(find(rows, "Audio codec"), "no audio yet");
		CHECK(!find(rows, "Audio received"));

		now.values[RTT] = 23;
		now.values[RTT_BASE] = 11;
		now.values[KBPS] = 54000;
		now.values[TAG] = 0x704F; /* Opus */
		now.values[WAVES] = 500;
		now.values[OVERRUN] = 3;
		now.values[UNDECODABLE] = 1;
		now.values[DELAY] = 42;
		auto prev = reading(0);
		prev.values[WAVES] = 450;
		rows = sdl::overlay::realtimeRows(n, now, &prev, {});
		EXPECT(find(rows, "Round trip (measured by the server)"), "23 ms (lowest 11)");
		EXPECT(find(rows, "Bandwidth (measured by the server)"), "54000 kbit/s");
		EXPECT(find(rows, "Audio codec"), "OPUS");
		EXPECT(find(rows, "Audio received"), "50.0 /s, 500 total, 4 dropped, 42 ms delay");

		/* a client without the graphics channel's statistics: no codec row at all */
		const Names few({ "NETWORK_BYTES_RECEIVED" });
		rows = sdl::overlay::realtimeRows(few, reading(1), nullptr, {});
		CHECK(!find(rows, "Video codecs"));
		CHECK(!find(rows, "Round trip (measured by the server)"));
		return 0;
	}

	int checkClient()
	{
		const Names n(names);
		auto now = reading(1);
		CHECK(sdl::overlay::decoderRows(n, now).empty());

		now.values[AV1_PICTURES] = 3;
		now.values[AV1_HARDWARE] = 1;
		now.values[CAPS] = RDPGFX_CAPVERSION_107;
		now.backends.emplace_back("AV1", "dav1d");
		auto rows = sdl::overlay::decoderRows(n, now);
		EXPECT(find(rows, "AV1 decoder"), "dav1d, software");

		now.backends.clear();
		now.values[AV1_HARDWARE] = 0;
		rows = sdl::overlay::decoderRows(n, now);
		EXPECT(find(rows, "AV1 decoder"), "unknown");

		const char* config = "Build configuration: WITH_A=ON WITH_B=OFF\n"
		                     "Build type:          Release\n"
		                     "CFLAGS:              -O2\n"
		                     "Compiler:            GNU, 16.2.0\n"
		                     "Target architecture: x86_64\n";
		CHECK(sdl::overlay::buildSummary(config) == "Release, GNU, 16.2.0, x86_64");
		CHECK(sdl::overlay::buildSummary(nullptr).empty());
		return 0;
	}
	bool overlap(const sdl::overlay::Box& a, const sdl::overlay::Box& b)
	{
		return (a.x < b.x + b.w) && (b.x < a.x + a.w) && (a.y < b.y + b.h) && (b.y < a.y + a.h);
	}

	sdl::overlay::Box box(float w, float h)
	{
		return { 0, 0, w, h };
	}

	int checkLayout()
	{
		const sdl::overlay::Box none{};

		/* all fit side by side: the server part under the client part, the right one at the top
		 * right */
		auto l = sdl::overlay::layout(box(400, 100), box(300, 80), box(300, 200), 1280, 12);
		CHECK((l.left.x == 12) && (l.left.y == 12));
		CHECK((l.server.x == 12) && (l.server.y == 12 + 100 + 12));
		CHECK((l.right.x == 1280 - 12 - 300) && (l.right.y == 12));

		/* the 1000 px window of the rig: the right part goes under the left column */
		l = sdl::overlay::layout(box(590, 110), none, box(480, 200), 1000, 12);
		CHECK((l.right.y == 12 + 110 + 12) && (l.right.x == 1000 - 12 - 480));
		l = sdl::overlay::layout(box(590, 110), box(300, 80), box(480, 200), 1000, 12);
		CHECK((l.right.y == 12 + 110 + 12 + 80 + 12) && (l.right.x == 1000 - 12 - 480));

		/* a wide server part decides the column too: the client part alone would fit beside */
		l = sdl::overlay::layout(box(300, 110), box(700, 80), box(480, 200), 1100, 12);
		CHECK(l.right.y == 12 + 110 + 12 + 80 + 12);

		/* narrower than the right part: it starts at the left edge, still under the column */
		l = sdl::overlay::layout(box(590, 110), none, box(480, 200), 400, 12);
		CHECK((l.right.x == 0) && (l.right.y == 134));

		/* no client part: the server part at the top, no part: the right one stays at the top */
		l = sdl::overlay::layout(none, box(300, 80), box(480, 200), 1280, 12);
		CHECK((l.server.y == 12) && (l.right.y == 12));
		l = sdl::overlay::layout(none, none, box(480, 200), 400, 12);
		CHECK(l.right.y == 12);

		/* at any width no part covers another */
		for (float width = 50; width <= 3000; width += 7)
		{
			for (const auto& sizes : { std::array<float, 6>{ 590, 110, 300, 80, 480, 200 },
			                           std::array<float, 6>{ 100, 300, 600, 120, 900, 50 },
			                           std::array<float, 6>{ 700, 90, 0, 0, 700, 400 },
			                           std::array<float, 6>{ 0, 0, 500, 160, 700, 400 } })
			{
				l = sdl::overlay::layout(box(sizes[0], sizes[1]), box(sizes[2], sizes[3]),
				                         box(sizes[4], sizes[5]), width, 12);
				const bool server = (sizes[2] > 0);
				if (overlap(l.left, l.right) || (server && overlap(l.server, l.right)) ||
				    (server && overlap(l.left, l.server)))
				{
					(void)fprintf(stderr, "overlap at width %.0f\n", static_cast<double>(width));
					return __LINE__;
				}
			}
		}
		return 0;
	}

	int checkServer()
	{
		const std::vector<std::string> serverNames = {
			"RDP_VERSION",
			"SERVER_RDP_VERSION",
			"SERVER_OS_MAJOR_TYPE",
			"SERVER_OS_MINOR_TYPE",
			"SERVER_PORT",
			"GFX_CAPS_VERSION",
			"GFX_CAPS_FLAGS",
			"RDPGFX_SURFACES",
			"RDPGFX_SURFACE1_CODECID",
			"RDPGFX_SURFACE1_WIDTH",
			"RDPGFX_SURFACE1_HEIGHT",
			"RDPGFX_SURFACE2_CODECID",
			"RDPGFX_SURFACE2_WIDTH",
			"RDPGFX_SURFACE2_HEIGHT",
		};
		const Names n(serverNames);
		Reading r;
		r.values.resize(serverNames.size());

		/* before the server announced anything: no row */
		CHECK(sdl::overlay::serverRows(n, r).empty());

		r.values = { RDP_VERSION_10_7,
			         RDP_VERSION_10_12,
			         OSMAJORTYPE_UNIX,
			         OSMINORTYPE_NATIVE_XSERVER,
			         3390,
			         RDPGFX_CAPVERSION_107,
			         RDPGFX_CAPS_FLAG_SMALL_CACHE,
			         6,
			         RDPGFX_CODECID_PLANAR,
			         1280,
			         720,
			         UINT32_MAX,
			         640,
			         480 };
		r.strings = { { "SERVER_HOSTNAME", "server.test" }, { "SERVER_CERT_CN", "cn.test" } };
		const auto rows = sdl::overlay::serverRows(n, r);
		CHECK(rows.size() == 8);
		CHECK(rows.front().label == "RDP version");
		EXPECT(find(rows, "RDP version"), "10.7 (server 10.12)");
		EXPECT(find(rows, "Server OS"), "Unix / native X server");
		EXPECT(find(rows, "Graphics caps"), "10.7, flags: small cache");
		EXPECT(find(rows, "Surface 1"), "1280 × 720, Planar");
		EXPECT(find(rows, "Surface 2"), "640 × 480, no picture yet");
		EXPECT(find(rows, "Surfaces"), "+2 more");
		EXPECT(find(rows, "Host"), "server.test:3390");
		EXPECT(find(rows, "Certificate"), "cn.test");

		/* the same version on both sides is named once; no certificate yet, no row */
		r.values[1] = RDP_VERSION_10_7;
		r.strings = { { "SERVER_HOSTNAME", "server.test" } };
		const auto same = sdl::overlay::serverRows(n, r);
		EXPECT(find(same, "RDP version"), "10.7");
		CHECK(!find(same, "Certificate"));
		return 0;
	}
} // namespace

int TestSDLOverlayModel([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	for (auto* check : { checkNames, checkRates, checkLabels, checkNetworkAndAudio, checkClient,
	                     checkLayout, checkServer })
	{
		const int rc = check();
		if (rc != 0)
			return rc;
	}
	return 0;
}
