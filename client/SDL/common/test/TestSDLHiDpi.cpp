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

	/* pixels everywhere: the behaviour before /sdl-hidpi */
	p = sdl::hidpi::parsePolicy("pixels");
	CHECK(p);
	CHECK(same(size(*p, cinema), 2560, 1440, 100));
	CHECK(same(size(*p, retina), 2560, 1664, 200));
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
	return 0;
}

int TestSDLHiDpi([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	for (auto* check : { checkParse, checkPolicy, checkMonitors, checkMixedMac, checkOverlay })
	{
		const int rc = check();
		if (rc != 0)
			return rc;
	}
	return 0;
}
