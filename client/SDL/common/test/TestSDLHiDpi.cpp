#include <cmath>
#include <cstdio>
#include <string>

#include "../../SDL3/sdl_hidpi.hpp"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return -1;                                                       \
		}                                                                    \
	} while (0)

using sdl::hidpi::Mode;

static bool same(const sdl::hidpi::MonitorSize& s, int w, int h, unsigned scale)
{
	if ((s.width == w) && (s.height == h) && (s.scale == scale))
		return true;
	(void)fprintf(stderr, "got %dx%d at %u, want %dx%d at %u\n", s.width, s.height, s.scale, w, h,
	              scale);
	return false;
}

static int checkParse()
{
	CHECK(sdl::hidpi::parse("points") == Mode::Points);
	CHECK(sdl::hidpi::parse("pixels") == Mode::Pixels);
	CHECK(!sdl::hidpi::parse("native"));
	CHECK(!sdl::hidpi::parse(""));
	CHECK(!sdl::hidpi::parse("Points"));
	CHECK(std::string(sdl::hidpi::name(Mode::Points)) == "points");
	CHECK(std::string(sdl::hidpi::name(Mode::Pixels)) == "pixels");

	/* scale:N, N from 50 to 400 */
	const auto s150 = sdl::hidpi::parse("scale:150");
	CHECK(s150 && (s150->mode == Mode::Scale) && (s150->percent == 150));
	CHECK(sdl::hidpi::describe(*s150) == "scale:150");
	CHECK(sdl::hidpi::parse("scale:50") && sdl::hidpi::parse("scale:400"));
	for (const char* bad : { "scale:", "scale:49", "scale:401", "scale:1500", "scale:-150",
	                         "scale:1.5", "scale:150%", "scale", "scale:+150" })
	{
		if (sdl::hidpi::parse(bad))
		{
			(void)fprintf(stderr, "accepted \"%s\"\n", bad);
			return -1;
		}
	}
	return 0;
}

static int checkPolicy()
{
	auto p = sdl::hidpi::parsePolicy("points");
	CHECK(p && (p->fallback == Mode::Points) && p->displays.empty());

	p = sdl::hidpi::parsePolicy("pixels");
	CHECK(p && (p->fallback == Mode::Pixels) && (p->forDisplay(7) == Mode::Pixels));

	/* a default and a display of its own */
	p = sdl::hidpi::parsePolicy("points,2:pixels");
	CHECK(p && (p->forDisplay(1) == Mode::Points) && (p->forDisplay(2) == Mode::Pixels));

	/* the overrides alone keep the default, points */
	p = sdl::hidpi::parsePolicy("1:pixels,3:points");
	CHECK(p && (p->forDisplay(1) == Mode::Pixels) && (p->forDisplay(3) == Mode::Points) &&
	      (p->forDisplay(9) == Mode::Points));

	/* scale:N as the default and as a display's own */
	p = sdl::hidpi::parsePolicy("scale:150");
	CHECK(p && (p->forDisplay(3).mode == Mode::Scale) && (p->forDisplay(3).percent == 150));
	p = sdl::hidpi::parsePolicy("points,2:scale:125");
	CHECK(p && (p->forDisplay(1).mode == Mode::Points) && (p->forDisplay(2).mode == Mode::Scale) &&
	      (p->forDisplay(2).percent == 125));
	CHECK(!sdl::hidpi::parsePolicy("points,2:scale:9"));

	/* the last word on a display wins */
	p = sdl::hidpi::parsePolicy("pixels,4:points,4:pixels");
	CHECK(p && (p->forDisplay(4) == Mode::Pixels));

	for (const char* bad :
	     { "", "native", "points,", ",points", "points,pixels", "x:pixels", "2:native",
	       "2:", ":pixels", "-1:pixels", "4294967296:pixels", "points;2:pixels" })
	{
		if (sdl::hidpi::parsePolicy(bad))
		{
			(void)fprintf(stderr, "accepted \"%s\"\n", bad);
			return -1;
		}
	}
	return 0;
}

/* each monitor by its own density */
static int checkMonitors()
{
	/* a 2x display: points or every pixel */
	CHECK(same(sdl::hidpi::monitorSize(1280, 832, 2.0f, 1.0f, Mode::Points), 1280, 832, 100));
	CHECK(same(sdl::hidpi::monitorSize(1280, 832, 2.0f, 1.0f, Mode::Pixels), 2560, 1664, 200));
	/* Windows at 150 %: one pixel per point, its own scale kept in both */
	CHECK(same(sdl::hidpi::monitorSize(2560, 1440, 1.0f, 1.5f, Mode::Points), 2560, 1440, 150));
	CHECK(same(sdl::hidpi::monitorSize(2560, 1440, 1.0f, 1.5f, Mode::Pixels), 2560, 1440, 150));
	/* a 2x display with its own scale on top (a 2x Wayland output at 125 %) */
	CHECK(same(sdl::hidpi::monitorSize(1536, 960, 2.0f, 1.25f, Mode::Points), 1536, 960, 125));
	CHECK(same(sdl::hidpi::monitorSize(1536, 960, 2.0f, 1.25f, Mode::Pixels), 3072, 1920, 250));
	/* a 4K display at 2x is a 2x display like any other */
	CHECK(same(sdl::hidpi::monitorSize(1920, 1080, 2.0f, 1.0f, Mode::Points), 1920, 1080, 100));
	CHECK(same(sdl::hidpi::monitorSize(1920, 1080, 2.0f, 1.0f, Mode::Pixels), 3840, 2160, 200));
	/* a fractional density rounds */
	CHECK(same(sdl::hidpi::monitorSize(1001, 667, 1.5f, 1.0f, Mode::Pixels), 1502, 1001, 150));
	/* scale:N: the points times N/100, rounded to even, at scale 100, whatever the density */
	const sdl::hidpi::Setting s150{ Mode::Scale, 150 };
	CHECK(same(sdl::hidpi::monitorSize(1280, 832, 2.0f, 1.0f, s150), 1920, 1248, 100));
	CHECK(same(sdl::hidpi::monitorSize(1280, 832, 1.0f, 1.5f, s150), 1920, 1248, 100));
	CHECK(same(sdl::hidpi::monitorSize(1001, 667, 2.0f, 1.0f, s150), 1502, 1000, 100));
	CHECK(same(sdl::hidpi::monitorSize(1280, 833, 2.0f, 1.0f, { Mode::Scale, 125 }), 1600, 1042,
	           100));
	CHECK(sdl::hidpi::evenRound(1000.5) == 1000);
	CHECK(sdl::hidpi::evenRound(1001.0) == 1002);
	CHECK(sdl::hidpi::evenRound(1249.5) == 1250);

	/* nonsense from the system reads as one */
	CHECK(same(sdl::hidpi::monitorSize(800, 600, 0.0f, 0.0f, Mode::Pixels), 800, 600, 100));
	CHECK(same(sdl::hidpi::monitorSize(800, 600, -1.0f, -2.0f, Mode::Points), 800, 600, 100));
	return 0;
}

/* his Mac: the LED Cinema Display 2560x1440 at 1x (display 1) and the built-in Retina, 1280x832
 * points at 2x (display 2), each by its own density and its own mode */
static int checkMixedMac()
{
	struct Display
	{
		uint32_t id;
		int w;
		int h;
		float density;
	};
	const Display cinema{ 1, 2560, 1440, 1.0f };
	const Display retina{ 2, 1280, 832, 2.0f };
	const auto size = [](const sdl::hidpi::Policy& p, const Display& d)
	{ return sdl::hidpi::monitorSize(d.w, d.h, d.density, 1.0f, p.forDisplay(d.id)); };

	/* the default, points: the Cinema Display unchanged, the Retina in points */
	auto p = sdl::hidpi::parsePolicy("points");
	CHECK(p);
	CHECK(same(size(*p, cinema), 2560, 1440, 100));
	CHECK(same(size(*p, retina), 1280, 832, 100));

	/* the Retina in every pixel, the Cinema Display as it is */
	p = sdl::hidpi::parsePolicy("points,2:pixels");
	CHECK(p);
	CHECK(same(size(*p, cinema), 2560, 1440, 100));
	CHECK(same(size(*p, retina), 2560, 1664, 200));

	/* the Retina at 150 %, drawn into its every pixel; the Cinema Display as it is */
	p = sdl::hidpi::parsePolicy("points,2:scale:150");
	CHECK(p);
	CHECK(same(size(*p, cinema), 2560, 1440, 100));
	CHECK(same(size(*p, retina), 1920, 1248, 100));

	/* pixels everywhere: the behaviour before /sdl-hidpi */
	p = sdl::hidpi::parsePolicy("pixels");
	CHECK(p);
	CHECK(same(size(*p, cinema), 2560, 1440, 100));
	CHECK(same(size(*p, retina), 2560, 1664, 200));
	return 0;
}

/* scale:N draws (session + offset) x scale and maps the mouse back: a round trip at 150 % */
static int checkMapping()
{
	/* the Retina at scale:150: a 1920x1248 session in 2560x1664 window pixels */
	sdl::hidpi::Mapping m;
	m.scaleX = 2560.0f / 1920.0f;
	m.scaleY = 1664.0f / 1248.0f;
	for (const auto& [x, y] : { std::pair<float, float>{ 0.0f, 0.0f },
	                            { 1919.0f, 1247.0f },
	                            { 960.5f, 624.25f },
	                            { 1.0f, 1247.0f } })
	{
		const auto [wx, wy] = sdl::hidpi::toWindow(x, y, m);
		const auto [sx, sy] = sdl::hidpi::toSession(wx, wy, m);
		CHECK((std::fabs(sx - x) < 0.01f) && (std::fabs(sy - y) < 0.01f));
	}
	const auto corner = sdl::hidpi::toWindow(1920.0f, 1248.0f, m);
	CHECK((std::fabs(corner.first - 2560.0f) < 0.01f) &&
	      (std::fabs(corner.second - 1664.0f) < 0.01f));

	/* with /multimon: the second monitor starts at x 2560 in the session */
	m.offsetX = -2560;
	m.offsetY = 0;
	const auto [wx, wy] = sdl::hidpi::toWindow(2560.0f + 960.0f, 624.0f, m);
	CHECK((std::fabs(wx - 1280.0f) < 0.01f) && (std::fabs(wy - 832.0f) < 0.01f));
	const auto [sx, sy] = sdl::hidpi::toSession(1280.0f, 832.0f, m);
	CHECK((std::fabs(sx - 3520.0f) < 0.01f) && (std::fabs(sy - 624.0f) < 0.01f));

	/* scale 1: only the offset, as before */
	const sdl::hidpi::Mapping plain{ 1.0f, 1.0f, -100, -50 };
	const auto p = sdl::hidpi::toWindow(150.0f, 80.0f, plain);
	CHECK((p.first == 50.0f) && (p.second == 30.0f));
	/* a broken scale never divides by zero */
	const sdl::hidpi::Mapping broken{ 0.0f, -1.0f, 0, 0 };
	const auto q = sdl::hidpi::toSession(10.0f, 20.0f, broken);
	CHECK((q.first == 10.0f) && (q.second == 20.0f));
	return 0;
}

static int checkOverlay()
{
	CHECK(sdl::hidpi::overlayText(Mode::Points, 2.0f) ==
	      "points (display 2x): the session in points, the system scales it");
	CHECK(sdl::hidpi::overlayText(Mode::Pixels, 2.0f) ==
	      "pixels (display 2x): every pixel, the server scales its UI");
	CHECK(sdl::hidpi::overlayText(Mode::Points, 1.0f) == "points (display 1x: every pixel)");
	CHECK(sdl::hidpi::overlayText(Mode::Pixels, 1.5f).find("display 1.5x") != std::string::npos);
	CHECK(sdl::hidpi::overlayText({ Mode::Scale, 150 }, 2.0f) ==
	      "scale:150 (display 2x): the session at the points x 150 %, drawn to fit");
	return 0;
}

/* [MS-RDPBCGR] 2.2.1.3.2, 2.2.1.3.10.1: the device scale is 100, 140 or 180, or the server drops
 * both factors */
static int checkDeviceScale()
{
	struct Case
	{
		uint32_t desktop;
		uint32_t device;
	};
	for (const auto& c : { Case{ 0, 100 }, Case{ 100, 100 }, Case{ 125, 100 }, Case{ 139, 100 },
	                       Case{ 140, 140 }, Case{ 150, 140 }, Case{ 175, 140 }, Case{ 179, 140 },
	                       Case{ 180, 180 }, Case{ 200, 180 }, Case{ 300, 180 }, Case{ 500, 180 } })
	{
		const auto got = sdl::hidpi::deviceScaleFactor(c.desktop);
		if (got != c.device)
		{
			(void)fprintf(stderr, "desktop %u: device %u, want %u\n", c.desktop, got, c.device);
			return -1;
		}
	}

	/* per display, from the desktop scale each one asks for (a mixed desktop) */
	const auto win150 = sdl::hidpi::monitorSize(1280, 720, 1.0f, 1.5f, Mode::Points);
	CHECK(sdl::hidpi::deviceScaleFactor(win150.scale) == 140);
	const auto retinaPixels = sdl::hidpi::monitorSize(1512, 982, 2.0f, 1.0f, Mode::Pixels);
	CHECK(sdl::hidpi::deviceScaleFactor(retinaPixels.scale) == 180);
	const auto retinaPoints = sdl::hidpi::monitorSize(1512, 982, 2.0f, 1.0f, Mode::Points);
	CHECK(sdl::hidpi::deviceScaleFactor(retinaPoints.scale) == 100);
	const auto scaled = sdl::hidpi::monitorSize(1512, 982, 2.0f, 1.0f, { Mode::Scale, 150 });
	CHECK(sdl::hidpi::deviceScaleFactor(scaled.scale) == 100);

	/* the display's own scale in /list:monitor */
	CHECK(sdl::hidpi::percent(1.0f) == 100);
	CHECK(sdl::hidpi::percent(1.25f) == 125);
	CHECK(sdl::hidpi::percent(1.5f) == 150);
	CHECK(sdl::hidpi::percent(1.75f) == 175);
	CHECK(sdl::hidpi::percent(2.0f) == 200);
	CHECK(sdl::hidpi::percent(0.0f) == 100);
	CHECK(sdl::hidpi::percent(-1.0f) == 100);
	return 0;
}

int TestSDLHiDpi([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	for (auto* check : { checkParse, checkPolicy, checkMonitors, checkMixedMac, checkMapping,
	                     checkOverlay, checkDeviceScale })
	{
		const int rc = check();
		if (rc != 0)
			return rc;
	}
	return 0;
}
