/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: the stats and info overlay
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
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#include <freerdp/client.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/codecs.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/log.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include "sdl_overlay.hpp"
#include "sdl_stats_json.hpp"
#include "sdl_context.hpp"
#include "sdl_utils.hpp"
#include "dialogs/res/sdl3_resource_manager.hpp"

#define TAG CLIENT_TAG("SDL.overlay")

/* The first reading comes soon after the overlay shows, then one a second. */
static constexpr uint64_t firstIntervalNS = 100ull * 1000ull * 1000ull;
static constexpr uint64_t intervalNS = 1000ull * 1000ull * 1000ull;

/* What the overlay reads of the libraries, all of it: the client's runtime statistics, its
 * information strings and the graphics channel's decoder backends. Keep it the only place that
 * knows them. */
static std::vector<std::string> statsNames()
{
	std::vector<std::string> names;
	const size_t count = freerdp_client_stats_max_index();
	names.reserve(count);
	for (size_t x = 0; x < count; x++)
	{
		const char* name = freerdp_client_stats_name_for_index(x);
		names.emplace_back(name ? name : "");
	}
	return names;
}

/* On the session thread, which owns what is read */
static void statsRead(rdpContext* context, sdl::overlay::Reading& reading)
{
	const size_t count = freerdp_client_stats_max_index();
	reading.values.resize(count);
	for (size_t x = 0; x < count; x++)
		reading.values.at(x) = freerdp_client_stats_value_for_index(context, x);

	for (size_t x = 0; x < freerdp_client_stats_string_max_index(); x++)
	{
		const char* name = freerdp_client_stats_string_name_for_index(x);
		const char* value = freerdp_client_stats_string_value_for_index(context, x);
		if (name && value)
			reading.strings.emplace_back(name, value);
	}

	RdpgfxClientContext* gfx = context->gdi ? context->gdi->gfx : nullptr;
	if (!gfx)
		return;
	/* AV1's codec ID does not exist in a build without AV1 and AV2 */
	const std::vector<std::pair<const char*, UINT16>> ids = {
#if defined(WITH_GFX_AV1) || defined(WITH_GFX_AV2)
		{ "AV1", RDPGFX_CODECID_AV1 },
#endif
		{ "AVC420", RDPGFX_CODECID_AVC420 },
		{ "AVC444", RDPGFX_CODECID_AVC444 },
		{ "AVC444v2", RDPGFX_CODECID_AVC444v2 },
	};
	for (const auto& [label, codecId] : ids)
	{
		const char* backend = rdpgfx_stats_decoder_backend(gfx, codecId);
		if (backend)
			reading.backends.emplace_back(label, backend);
	}
}

static std::string decodersSupported(rdpContext* context)
{
	struct Codec
	{
		FreeRDP_CodecFlags flag;
		const char* label;
	};
	/* the flags of codecs a build leaves out do not exist */
	const std::vector<Codec> codecs = {
		{ FREERDP_CODEC_AVC420, "H.264 4:2:0" },    { FREERDP_CODEC_AVC444, "H.264 4:4:4" },
#if defined(WITH_GFX_AV1)
		{ FREERDP_CODEC_AV1_I420, "AV1 4:2:0" },    { FREERDP_CODEC_AV1_I444, "AV1 4:4:4" },
#endif
#if defined(WITH_GFX_AV2)
		{ FREERDP_CODEC_AV2_I420, "AV2 4:2:0" },    { FREERDP_CODEC_AV2_I444, "AV2 4:4:4" },
#endif
#if defined(WITH_GFX_AZURE)
		{ FREERDP_CODEC_HEVC_I420, "HEVC" },
#endif
		{ FREERDP_CODEC_REMOTEFX, "RemoteFX" },     { FREERDP_CODEC_PROGRESSIVE, "Progressive" },
		{ FREERDP_CODEC_CLEARCODEC, "ClearCodec" }, { FREERDP_CODEC_PLANAR, "Planar" },
	};
	std::string list;
	for (const auto& codec : codecs)
	{
		if (!freerdp_codec_supported(context, codec.flag, FALSE))
			continue;
		list += list.empty() ? codec.label : std::string(", ") + codec.label;
	}
	return list;
}

static std::string version(int v)
{
	return std::to_string(SDL_VERSIONNUM_MAJOR(v)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(v)) +
	       "." + std::to_string(SDL_VERSIONNUM_MICRO(v));
}

/* a scale the font and the panels were made for: the same unless it really changed */
static bool sameScale(float a, float b)
{
	return std::fabs(a - b) < 0.001F;
}

SdlOverlay::SdlOverlay(SdlContext* sdl) : _sdl(sdl)
{
	WINPR_ASSERT(_sdl);
#if defined(__EMSCRIPTEN__)
	/* made here, before the session thread can wait on it */
	_tick = CreateEvent(nullptr, TRUE, FALSE, nullptr);
#endif
}

SdlOverlay::~SdlOverlay()
{
#if defined(__EMSCRIPTEN__)
	if (_sdlTimer)
		std::ignore = SDL_RemoveTimer(_sdlTimer);
	if (_tick)
		(void)CloseHandle(_tick);
#endif
}

HANDLE SdlOverlay::tickEvent() const
{
#if defined(__EMSCRIPTEN__)
	return _tick;
#else
	return nullptr;
#endif
}

void SdlOverlay::sessionTick(rdpContext* context)
{
#if defined(__EMSCRIPTEN__)
	if (!_tick || (WaitForSingleObject(_tick, 0) != WAIT_OBJECT_0))
		return;
	(void)ResetEvent(_tick);
	if (wanted() && !read(context, winpr_GetTickCount64NS()))
		WLog_Print(_sdl->getWLog(), WLOG_WARN, "the overlay's reading was not passed on");
#else
	WINPR_UNUSED(context);
#endif
}

bool SdlOverlay::visible() const
{
	return _visible;
}

bool SdlOverlay::openIpc(const char* path)
{
#if defined(__EMSCRIPTEN__)
	WINPR_UNUSED(path);
	WLog_Print(_sdl->getWLog(), WLOG_ERROR, "the web client has no /sdl-stats-ipc");
	return false;
#else
	std::string error;
	std::string where = path ? path : "";
	if (where.empty())
		where = SdlStatsIpc::defaultPath(error);
	if (!where.empty())
		_ipc = SdlStatsIpc::open(where, error);
	if (!_ipc)
	{
		WLog_Print(_sdl->getWLog(), WLOG_ERROR, "/sdl-stats-ipc: %s", error.c_str());
		return false;
	}
	WLog_Print(_sdl->getWLog(), WLOG_INFO, "/sdl-stats-ipc: the statistics go to %s",
	           _ipc->path().c_str());
	return true;
#endif
}

bool SdlOverlay::ipcOn() const
{
#if defined(__EMSCRIPTEN__)
	return false;
#else
	return _ipc != nullptr;
#endif
}

/* the timer runs for the overlay or the socket */
bool SdlOverlay::wanted() const
{
	return _visible || ipcOn();
}

bool SdlOverlay::show(bool on)
{
	if (on && !_visible.exchange(true) && !ipcOn())
	{
		/* nothing was read while hidden: the averages start over, never across the gap (with
		 * the socket on, the readings went on) */
		std::scoped_lock lock(_mutex);
		_restart = true;
	}
	_visible = on;
	if (!on || !_sdl->isConnected())
		return true;
	return arm();
}

bool SdlOverlay::connected()
{
	{
		std::scoped_lock lock(_mutex);
		_since = std::chrono::steady_clock::now();
		_pending.reset();
		_restart = true;
		_connection++;
		_startedAt = sdl::stats::localIso8601(std::time(nullptr));
	}
	if (!wanted())
		return true;
	return arm();
}

bool SdlOverlay::arm()
{
	/* one timer at most: the one that sees _armed false adds it */
	bool expected = false;
	if (!_armed.compare_exchange_strong(expected, true))
		return true;

#if defined(__EMSCRIPTEN__)
	/* libfreerdp has no freerdp_timer_* in the browser: SDL's timer wakes the session thread,
	 * which takes the reading (sessionTick) */
	if (_tick)
		_sdlTimer = SDL_AddTimerNS(firstIntervalNS, onSdlTimer, this);
	const bool started = _tick && (_sdlTimer != 0);
#else
	const bool started =
	    freerdp_timer_add(_sdl->context(), firstIntervalNS, onTimer, this, true) != 0;
#endif
	if (!started)
	{
		_armed = false;
		WLog_Print(_sdl->getWLog(), WLOG_ERROR, "the overlay's timer could not start");
		return false;
	}
	return true;
}

bool SdlOverlay::keepTimer()
{
	if (wanted())
		return true;
	/* Stop, unless it was shown again meanwhile: then this timer goes on, or the one a show
	 * added does (the compare exchange lets only one go on). */
	_armed = false;
	if (!wanted())
		return false;
	bool expected = false;
	return _armed.compare_exchange_strong(expected, true);
}

#if defined(__EMSCRIPTEN__)
Uint64 SDLCALL SdlOverlay::onSdlTimer(void* userdata, WINPR_ATTR_UNUSED SDL_TimerID timerID,
                                      WINPR_ATTR_UNUSED Uint64 interval)
{
	auto overlay = static_cast<SdlOverlay*>(userdata);
	WINPR_ASSERT(overlay);
	if (!overlay->keepTimer())
		return 0;
	(void)SetEvent(overlay->_tick);
	return intervalNS;
}
#endif

uint64_t SdlOverlay::onTimer(rdpContext* context, void* userdata,
                             WINPR_ATTR_UNUSED FreeRDP_TimerID timerID, uint64_t timestamp,
                             WINPR_ATTR_UNUSED uint64_t interval)
{
	auto overlay = static_cast<SdlOverlay*>(userdata);
	WINPR_ASSERT(overlay);

	if (!overlay->keepTimer())
		return 0;

	if (!overlay->read(context, timestamp))
		WLog_Print(overlay->_sdl->getWLog(), WLOG_WARN, "the overlay's reading was not passed on");
	return intervalNS;
}

bool SdlOverlay::read(rdpContext* context, uint64_t timestamp)
{
	WINPR_ASSERT(context);

	sdl::overlay::Reading reading;
	reading.ns = timestamp;
	reading.wallMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                           std::chrono::system_clock::now().time_since_epoch())
	                                           .count());
	reading.desktopWidth = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
	reading.desktopHeight = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
	statsRead(context, reading);

	{
		std::scoped_lock lock(_mutex);
		_pending = std::move(reading);
	}
	return sdl_push_user_event(SDL_EVENT_USER_OVERLAY);
}

void SdlOverlay::clientRows(const char* rendererName, const std::string& hidpi)
{
	_client.clear();
	_hidpi = hidpi;
	std::string freerdp = freerdp_get_version_string();
	const char* revision = freerdp_get_build_revision();
	/* a build from outside a git tree says "n/a" */
	if (revision && (strlen(revision) > 0) && (strcmp(revision, "n/a") != 0))
		freerdp += std::string(" (") + revision + ")";
	_client.push_back({ "FreeRDP", freerdp });
	_client.push_back({ "Build", sdl::overlay::buildSummary(freerdp_get_build_config()) });
	_client.push_back({ "SDL", version(SDL_GetVersion()) + ", SDL_ttf " + version(TTF_Version()) +
	                               ", renderer " + (rendererName ? rendererName : "unknown") });
	_client.push_back({ "Decoders", decodersSupported(_sdl->context()) });
	if (!hidpi.empty())
		_client.push_back({ "HiDPI", hidpi });
}

bool SdlOverlay::update(int windowWidth, int windowHeight, const char* rendererName,
                        const std::string& hidpi)
{
	std::optional<sdl::overlay::Reading> reading;
	std::chrono::steady_clock::time_point since;
	bool restart = false;
	{
		std::scoped_lock lock(_mutex);
		reading.swap(_pending);
		since = _since;
		restart = _restart;
		_restart = false;
	}
	if (restart)
		_history.clear();
	if (!reading || !wanted())
		return false;

	if (!_names)
	{
		_statNames = statsNames();
		_names.emplace(_statNames);
	}
	const auto& names = *_names;
	/* without a window there is no renderer to name yet */
	if (_client.empty() || !rendererName || (_hidpi != hidpi))
		clientRows(rendererName, hidpi);

	sdl::overlay::View view;
	view.windowWidth = windowWidth;
	view.windowHeight = windowHeight;
	view.sessionSeconds = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - since)
	        .count());

	_left = _client;
	const auto decoders = sdl::overlay::decoderRows(names, *reading);
	_left.insert(_left.end(), decoders.begin(), decoders.end());
	_server = sdl::overlay::serverRows(names, *reading);
	_right = sdl::overlay::realtimeRows(names, *reading, _history.latest(), view);
	_history.add(std::move(*reading));
	const sdl::overlay::Reading* latest = _history.latest();
	for (auto& row : sdl::overlay::averageRows(names, _history))
		_right.push_back(std::move(row));
	_generation++;
	if (latest)
		publish(view, *latest);
	return _visible;
}

void SdlOverlay::publish(const sdl::overlay::View& view, const sdl::overlay::Reading& reading)
{
#if defined(__EMSCRIPTEN__)
	WINPR_UNUSED(view);
	WINPR_UNUSED(reading);
#else
	if (!_ipc)
		return;
	sdl::stats::Snapshot snapshot;
	snapshot.seq = ++_ipcSeq;
	{
		std::scoped_lock lock(_mutex);
		snapshot.connection = _connection;
		snapshot.startedAt = _startedAt;
	}
	snapshot.sessionSeconds = view.sessionSeconds;
	snapshot.windowWidth = view.windowWidth;
	snapshot.windowHeight = view.windowHeight;
	snapshot.names = &_statNames;
	snapshot.reading = &reading;
	snapshot.client = &_left;
	snapshot.server = &_server;
	snapshot.realtime = &_right;
	_ipc->publish(sdl::stats::toJson(snapshot));
#endif
}

const std::vector<sdl::overlay::Row>& SdlOverlay::left() const
{
	return _left;
}

const std::vector<sdl::overlay::Row>& SdlOverlay::server() const
{
	return _server;
}

const std::vector<sdl::overlay::Row>& SdlOverlay::right() const
{
	return _right;
}

uint64_t SdlOverlay::generation() const
{
	return _generation;
}

SdlOverlayPainter::SdlOverlayPainter(SDL_Renderer* renderer)
    : _renderer(renderer), _engine(TTF_CreateRendererTextEngine(renderer))
{
	if (!_engine)
		WLog_WARN(TAG, "TTF_CreateRendererTextEngine: %s", SDL_GetError());
}

SdlOverlayPainter::~SdlOverlayPainter()
{
	clear();
	if (_font)
		TTF_CloseFont(_font);
	if (_engine)
		TTF_DestroyRendererTextEngine(_engine);
}

void SdlOverlayPainter::clear()
{
	for (auto* list : { &_left, &_server, &_right })
	{
		for (auto& cell : *list)
		{
			if (cell.label)
				TTF_DestroyText(cell.label);
			if (cell.value)
				TTF_DestroyText(cell.value);
		}
		list->clear();
	}
}

std::vector<SdlOverlayPainter::Cell>
SdlOverlayPainter::cells(const std::vector<sdl::overlay::Row>& rows)
{
	std::vector<Cell> list;
	for (const auto& row : rows)
	{
		Cell cell;
		if (!row.label.empty())
			cell.label = TTF_CreateText(_engine, _font, row.label.c_str(), row.label.size());
		if (!row.value.empty())
			cell.value = TTF_CreateText(_engine, _font, row.value.c_str(), row.value.size());
		for (auto* text : { cell.label, cell.value })
		{
			if (text)
				std::ignore = TTF_SetTextColor(text, 0xFF, 0xFF, 0xFF, 0xFF);
		}
		/* the labels a little dimmer than the values */
		if (cell.label)
			std::ignore = TTF_SetTextColor(cell.label, 0xB8, 0xC4, 0xD0, 0xFF);
		list.push_back(cell);
	}
	return list;
}

bool SdlOverlayPainter::rebuild(const SdlOverlay& overlay, float scale)
{
	if (!_engine)
		return false;

	constexpr float points = 13.0F;
	if (!_font)
	{
		auto ops = SDL3ResourceManager::get(SDLResourceManager::typeFonts(),
		                                    "OpenSans-VariableFont_wdth,wght.ttf");
		if (!ops)
			return false;
		_font = TTF_OpenFontIO(ops, true, points * scale);
		if (!_font)
		{
			WLog_WARN(TAG, "TTF_OpenFontIO: %s", SDL_GetError());
			return false;
		}
	}
	else if (!sameScale(scale, _scale))
		std::ignore = TTF_SetFontSize(_font, points * scale);

	clear();
	_left = cells(overlay.left());
	_server = cells(overlay.server());
	_right = cells(overlay.right());
	_scale = scale;
	_generation = overlay.generation();
	return true;
}

SDL_FPoint SdlOverlayPainter::panelSize(const std::vector<Cell>& list, float& labelWidth) const
{
	labelWidth = 0;
	float valueWidth = 0;
	for (const auto& cell : list)
	{
		int w = 0;
		int h = 0;
		if (cell.label && TTF_GetTextSize(cell.label, &w, &h))
			labelWidth = std::max(labelWidth, static_cast<float>(w));
		if (cell.value && TTF_GetTextSize(cell.value, &w, &h))
			valueWidth = std::max(valueWidth, static_cast<float>(w));
	}
	const float gap = 12.0F * _scale;
	const float pad = 8.0F * _scale;
	const auto lines = static_cast<float>(list.size());
	return { pad + labelWidth + gap + valueWidth + pad,
		     pad + (lines * static_cast<float>(TTF_GetFontLineSkip(_font))) + pad };
}

void SdlOverlayPainter::drawPanel(const std::vector<Cell>& list, const sdl::overlay::Box& where)
{
	if (list.empty())
		return;

	float labelWidth = 0;
	std::ignore = panelSize(list, labelWidth);
	const float x = where.x;
	const float y = where.y;

	const SDL_FRect box = { where.x, where.y, where.w, where.h };
	std::ignore = SDL_SetRenderDrawColor(_renderer, 0x10, 0x14, 0x18, 0xA8);
	std::ignore = SDL_RenderFillRect(_renderer, &box);

	const float gap = 12.0F * _scale;
	const float pad = 8.0F * _scale;
	const auto skip = static_cast<float>(TTF_GetFontLineSkip(_font));
	float line = y + pad;
	for (const auto& cell : list)
	{
		if (cell.label)
			std::ignore = TTF_DrawRendererText(cell.label, x + pad, line);
		if (cell.value)
			std::ignore = TTF_DrawRendererText(cell.value, x + pad + labelWidth + gap, line);
		line += skip;
	}
}

void SdlOverlayPainter::draw(const SdlOverlay& overlay, int width, int height, float scale)
{
	if ((scale <= 0.0F) || (width <= 0) || (height <= 0))
		return;
	if ((overlay.generation() != _generation) || !sameScale(scale, _scale))
	{
		if (!rebuild(overlay, scale))
			return;
	}

	SDL_BlendMode mode = SDL_BLENDMODE_NONE;
	Uint8 r = 0;
	Uint8 g = 0;
	Uint8 b = 0;
	Uint8 a = 0;
	std::ignore = SDL_GetRenderDrawBlendMode(_renderer, &mode);
	std::ignore = SDL_GetRenderDrawColor(_renderer, &r, &g, &b, &a);
	std::ignore = SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);

	float labelWidth = 0;
	const auto size = [&](const std::vector<Cell>& list)
	{
		const auto s = list.empty() ? SDL_FPoint{ 0, 0 } : panelSize(list, labelWidth);
		return sdl::overlay::Box{ 0, 0, s.x, s.y };
	};
	const auto where = sdl::overlay::layout(size(_left), size(_server), size(_right),
	                                        static_cast<float>(width), 12.0F * _scale);
	drawPanel(_left, where.left);
	drawPanel(_server, where.server);
	drawPanel(_right, where.right);

	std::ignore = SDL_SetRenderDrawBlendMode(_renderer, mode);
	std::ignore = SDL_SetRenderDrawColor(_renderer, r, g, b, a);
}
