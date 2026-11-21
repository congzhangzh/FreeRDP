#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <winpr/json.h>

#include "../../SDL3/sdl_stats_json.hpp"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return -1;                                                       \
		}                                                                    \
	} while (0)

static bool contains(const std::string& s, const std::string& part)
{
	return s.find(part) != std::string::npos;
}

static int checkQuote()
{
	using sdl::stats::quote;
	CHECK(quote("") == "\"\"");
	CHECK(quote("a\"b\\c") == "\"a\\\"b\\\\c\"");
	CHECK(quote("\n\r\t") == "\"\\n\\r\\t\"");
	CHECK(quote(std::string("\x01\x1f\x7f", 3)) == "\"\\u0001\\u001f\\u007f\"");
	/* UTF-8 goes through as it is */
	CHECK(quote("1920 \xC3\x97 1080 \xE2\x80\x93 \xF0\x9F\x98\x80") ==
	      "\"1920 \xC3\x97 1080 \xE2\x80\x93 \xF0\x9F\x98\x80\"");
	/* what is no UTF-8 becomes U+FFFD, one per byte */
	const std::string bad = "\xEF\xBF\xBD";
	CHECK(quote("\xFF") == "\"" + bad + "\"");
	CHECK(quote("\xC0\xAF") == "\"" + bad + bad + "\"");                     /* overlong */
	CHECK(quote("\xED\xA0\x80") == "\"" + bad + bad + bad + "\"");           /* a surrogate */
	CHECK(quote("\xF4\x90\x80\x80") == "\"" + bad + bad + bad + bad + "\""); /* > U+10FFFF */
	CHECK(quote("a\xC3") == "\"a" + bad + "\"");                             /* cut short */
	return 0;
}

static int checkSnapshot()
{
	const std::vector<std::string> names = {
		"NETWORK_BYTES_RECEIVED",
		"",
		"FREERDP_CLIENT_STATS_UNUSED",
		"RDPGFX_CODECID_UNKNOWN_PICTURES",
		"AUDIO_DELAY_MS",
		"NETWORK_BYTES_RECEIVED", /* repeated: the first one counts */
		"BIG",
		"NETWORK_RTT_AVERAGE_MS", /* no value: the reading is shorter */
	};
	sdl::overlay::Reading reading;
	reading.ns = 123456789012345ull;
	reading.values = { 1000, 1, 2, 3, 42, 7, UINT64_MAX };
	reading.desktopWidth = 1920;
	reading.desktopHeight = 1080;
	reading.backends = { { "AV1", "dav1d" } };
	reading.strings = { { "SERVER_HOSTNAME", "host \"one\"" }, { "SERVER_HOSTNAME", "second" } };
	const std::vector<sdl::overlay::Row> client = { { "FreeRDP", "3.x" } };
	const std::vector<sdl::overlay::Row> server = {};
	const std::vector<sdl::overlay::Row> realtime = { { "Desktop", "1920 \xC3\x97 1080" },
		                                              { "Frames", "30 /s" } };

	sdl::stats::Snapshot snapshot;
	snapshot.seq = 5;
	snapshot.connection = 2;
	snapshot.sessionSeconds = 61;
	snapshot.windowWidth = 1280;
	snapshot.windowHeight = 720;
	snapshot.names = &names;
	snapshot.reading = &reading;
	snapshot.client = &client;
	snapshot.server = &server;
	snapshot.realtime = &realtime;
	const auto json = sdl::stats::toJson(snapshot);

	CHECK(json.find('\n') == std::string::npos);
	CHECK(contains(json, "\"schema\":\"freerdp-sdl-stats\",\"version\":1,"));
	CHECK(contains(json, "\"seq\":5,\"ns\":123456789012345,\"connection\":2,"));
	CHECK(contains(json, "\"session_seconds\":61,\"desktop\":[1920,1080],\"window\":[1280,720]"));
	CHECK(contains(json, "\"counters\":{\"NETWORK_BYTES_RECEIVED\":1000,\"AUDIO_DELAY_MS\":42,"
	                     "\"BIG\":18446744073709551615}"));
	CHECK(!contains(json, "UNUSED"));
	CHECK(!contains(json, "UNKNOWN"));
	CHECK(!contains(json, "NETWORK_RTT_AVERAGE_MS"));
	CHECK(contains(json, "\"strings\":{\"SERVER_HOSTNAME\":\"host \\\"one\\\"\"}"));
	CHECK(contains(json, "\"backends\":{\"AV1\":\"dav1d\"}"));
	CHECK(contains(json,
	               "\"rows\":{\"client\":[[\"FreeRDP\",\"3.x\"]],\"server\":[],"
	               "\"realtime\":[[\"Desktop\",\"1920 \xC3\x97 1080\"],[\"Frames\",\"30 /s\"]]}}"));

	/* a JSON parser reads it back, where WinPR has one */
	WINPR_JSON* root = WINPR_JSON_Parse(json.c_str());
	if (root)
	{
		const bool ok =
		    WINPR_JSON_IsObject(root) &&
		    (WINPR_JSON_GetIntegerValue(WINPR_JSON_GetObjectItemCaseSensitive(root, "seq")) == 5) &&
		    (WINPR_JSON_GetArraySize(WINPR_JSON_GetObjectItemCaseSensitive(
		         WINPR_JSON_GetObjectItemCaseSensitive(root, "rows"), "realtime")) == 2) &&
		    (std::string(WINPR_JSON_GetStringValue(WINPR_JSON_GetObjectItemCaseSensitive(
		         WINPR_JSON_GetObjectItemCaseSensitive(root, "strings"), "SERVER_HOSTNAME"))) ==
		     "host \"one\"");
		WINPR_JSON_Delete(root);
		CHECK(ok);
	}

	/* nothing read yet: still a whole object */
	sdl::stats::Snapshot empty;
	const auto none = sdl::stats::toJson(empty);
	CHECK(contains(none, "\"counters\":{},\"strings\":{},\"backends\":{},\"rows\":{\"client\":[],"
	                     "\"server\":[],\"realtime\":[]}}"));
	return 0;
}

int TestSDLStatsJson([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	for (auto* check : { checkQuote, checkSnapshot })
	{
		const int rc = check();
		if (rc != 0)
			return rc;
	}
	return 0;
}
