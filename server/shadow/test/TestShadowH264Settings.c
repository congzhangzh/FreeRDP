#include <stdio.h>
#include <stdlib.h>

#include <winpr/wtypes.h>
#include <freerdp/server/shadow.h>

#define RATE_CONTROL "FREERDP_SHADOW_H264_RATE_CONTROL"
#define BITRATE "FREERDP_SHADOW_H264_BITRATE"
#define QP "FREERDP_SHADOW_H264_QP"

/* The server reads the variables with getenv: set them in the C runtime's environment. An empty
 * value counts as unset. */
WINPR_ATTR_NODISCARD
static BOOL set(const char* name, const char* value)
{
#if defined(_WIN32)
	return _putenv_s(name, value ? value : "") == 0;
#else
	if (!value)
		return unsetenv(name) == 0;
	return setenv(name, value, 1) == 0;
#endif
}

WINPR_ATTR_NODISCARD
static BOOL expect(const char* what, const char* mode, const char* kbits, const char* qp,
                   H264_RATECONTROL_MODE expectedMode, UINT32 expectedBitRate, UINT32 expectedQP)
{
	if (!set(RATE_CONTROL, mode) || !set(BITRATE, kbits) || !set(QP, qp))
	{
		(void)fprintf(stderr, "%s: cannot set the environment\n", what);
		return FALSE;
	}

	rdpShadowServer* server = shadow_server_new();
	if (!server)
	{
		(void)fprintf(stderr, "%s: shadow_server_new failed\n", what);
		return FALSE;
	}

	const BOOL ok = (server->h264RateControlMode == expectedMode) &&
	                (server->h264BitRate == expectedBitRate) && (server->h264QP == expectedQP) &&
	                (server->h264FrameRate == 30);
	if (!ok)
		(void)fprintf(stderr,
		              "%s: mode %d bit rate %" PRIu32 " qp %" PRIu32 " fps %" PRIu32
		              ", expected mode %d bit rate %" PRIu32 " qp %" PRIu32 " fps 30\n",
		              what, (int)server->h264RateControlMode, server->h264BitRate, server->h264QP,
		              server->h264FrameRate, (int)expectedMode, expectedBitRate, expectedQP);
	shadow_server_free(server);
	return ok;
}

int TestShadowH264Settings(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	BOOL ok = TRUE;

	/* unset or empty: the defaults, VBR at 10 Mbit/s */
	ok &= expect("unset", nullptr, nullptr, nullptr, H264_RATECONTROL_VBR, 10000000, 0);
	ok &= expect("empty", "", "", "", H264_RATECONTROL_VBR, 10000000, 0);

	/* set */
	ok &= expect("vbr 4000", "vbr", "4000", nullptr, H264_RATECONTROL_VBR, 4000000, 0);
	ok &= expect("cqp 26", "cqp", nullptr, "26", H264_RATECONTROL_CQP, 10000000, 26);
	ok &= expect("upper case", "CQP", nullptr, "51", H264_RATECONTROL_CQP, 10000000, 51);
	ok &= expect("limits", "VBR", "1", "0", H264_RATECONTROL_VBR, 1000, 0);
	ok &= expect("highest bit rate", nullptr, "1000000", nullptr, H264_RATECONTROL_VBR, 1000000000,
	             0);

	/* wrong values keep the default of that setting (with a warning) */
	ok &= expect("no cbr in H.264", "cbr", "4000", nullptr, H264_RATECONTROL_VBR, 4000000, 0);
	ok &= expect("not a mode", "fast", nullptr, nullptr, H264_RATECONTROL_VBR, 10000000, 0);
	ok &= expect("bit rate 0", nullptr, "0", nullptr, H264_RATECONTROL_VBR, 10000000, 0);
	ok &=
	    expect("bit rate too high", nullptr, "1000001", nullptr, H264_RATECONTROL_VBR, 10000000, 0);
	ok &=
	    expect("bit rate not a number", nullptr, "4M", nullptr, H264_RATECONTROL_VBR, 10000000, 0);
	ok &= expect("qp too high", "cqp", nullptr, "52", H264_RATECONTROL_CQP, 10000000, 0);
	ok &= expect("qp negative", "cqp", nullptr, "-1", H264_RATECONTROL_CQP, 10000000, 0);

	if (!set(RATE_CONTROL, nullptr) || !set(BITRATE, nullptr) || !set(QP, nullptr))
		ok = FALSE;
	return ok ? 0 : -1;
}
