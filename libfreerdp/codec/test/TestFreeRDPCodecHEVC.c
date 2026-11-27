#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freerdp/config.h>
#include <freerdp/freerdp.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/h264.h>
#include <freerdp/codec/hevc.h>

/* hardware HEVC encoders may refuse very small pictures */
#define TEST_WIDTH 256
#define TEST_HEIGHT 256

/* The four quadrants of the test picture, each a flat colour (R, G, B). */
static const BYTE quadrant_colours[4][3] = {
	{ 230, 30, 30 },  /* top left: red */
	{ 30, 200, 30 },  /* top right: green */
	{ 30, 30, 230 },  /* bottom left: blue */
	{ 230, 220, 30 }, /* bottom right: yellow */
};

static size_t quadrant_of(UINT32 x, UINT32 y)
{
	return ((y >= TEST_HEIGHT / 2) ? 2 : 0) + ((x >= TEST_WIDTH / 2) ? 1 : 0);
}

static void fill_picture(BYTE* data, UINT32 step)
{
	for (UINT32 y = 0; y < TEST_HEIGHT; y++)
	{
		for (UINT32 x = 0; x < TEST_WIDTH; x++)
		{
			const BYTE* c = quadrant_colours[quadrant_of(x, y)];
			(void)FreeRDPWriteColor(&data[1ull * y * step + 4ull * x], PIXEL_FORMAT_BGRX32,
			                        FreeRDPGetColor(PIXEL_FORMAT_BGRX32, c[0], c[1], c[2], 0xFF));
		}
	}
}

/* Bit reader over an RBSP with the emulation prevention bytes already removed. */
typedef struct
{
	const BYTE* data;
	size_t size;
	size_t bit;
} bit_reader;

static BOOL read_bits(bit_reader* br, size_t count, UINT32* value)
{
	*value = 0;
	for (size_t i = 0; i < count; i++)
	{
		if (br->bit >= br->size * 8)
			return FALSE;
		const BYTE b = br->data[br->bit / 8];
		*value = (*value << 1) | ((b >> (7 - (br->bit % 8))) & 1);
		br->bit++;
	}
	return TRUE;
}

static BOOL read_ue(bit_reader* br, UINT32* value)
{
	size_t zeros = 0;
	UINT32 bit = 0;
	do
	{
		if (!read_bits(br, 1, &bit))
			return FALSE;
		if (!bit)
			zeros++;
	} while (!bit && (zeros < 32));
	if (zeros >= 32)
		return FALSE;
	UINT32 rest = 0;
	if (!read_bits(br, zeros, &rest))
		return FALSE;
	*value = (1u << zeros) - 1 + rest;
	return TRUE;
}

/* chroma_format_idc of the first HEVC SPS (NAL type 33) in an Annex B stream: 1 = 4:2:0,
 * 3 = 4:4:4. -1 when there is none. */
static int sps_chroma_format(const BYTE* data, size_t size)
{
	for (size_t i = 0; i + 4 < size; i++)
	{
		if ((data[i] != 0) || (data[i + 1] != 0) || (data[i + 2] != 1))
			continue;
		const size_t start = i + 3;
		if (((data[start] >> 1) & 0x3F) != 33)
			continue;

		/* RBSP without emulation prevention bytes, after the two byte NAL header */
		BYTE rbsp[64] = WINPR_C_ARRAY_INIT;
		size_t n = 0;
		for (size_t j = start + 2; (j < size) && (n < sizeof(rbsp)); j++)
		{
			if ((j >= start + 4) && (data[j] == 3) && (data[j - 1] == 0) && (data[j - 2] == 0))
				continue;
			rbsp[n++] = data[j];
		}

		bit_reader br = { rbsp, n, 0 };
		UINT32 v = 0;
		UINT32 maxSubLayersMinus1 = 0;
		if (!read_bits(&br, 4, &v) || !read_bits(&br, 3, &maxSubLayersMinus1) ||
		    !read_bits(&br, 1, &v))
			return -1;
		if (maxSubLayersMinus1 != 0)
			return -1; /* sub layer profile/level not parsed here */
		br.bit += 96;  /* general profile_tier_level: 88 bits + general_level_idc */
		if (!read_ue(&br, &v) || !read_ue(&br, &v))
			return -1;
		return (int)v;
	}
	return -1;
}

/* Encode the test picture at a profile (0: 4:2:0, 1: 4:4:4), check the stream's chroma format,
 * and, when this build has an HEVC decoder too, decode it and compare each quadrant's centre.
 * Returns 1 on success, 0 when this build cannot do it, -1 on failure. */
static int round_trip(UINT32 profile)
{
	const char* name = profile ? "4:4:4" : "4:2:0";
	const UINT32 step = TEST_WIDTH * 4;
	int rc = -1;
	BYTE* coded = nullptr;
	UINT32 codedSize = 0;
	RDPGFX_H264_METABLOCK meta = WINPR_C_ARRAY_INIT;
	const RECTANGLE_16 rect = { 0, 0, TEST_WIDTH, TEST_HEIGHT };

	BYTE* src = calloc(TEST_HEIGHT, step);
	BYTE* dst = calloc(TEST_HEIGHT, step);
	FREERDP_HEVC_CONTEXT* enc = freerdp_hevc_context_new(TRUE);
	FREERDP_HEVC_CONTEXT* dec = freerdp_hevc_context_new(FALSE);
	if (!src || !dst)
		goto fail;
	if (!enc)
	{
		(void)fprintf(stderr, "HEVC %s: no encoder in this build\n", name);
		rc = 0;
		goto fail;
	}
	if (!freerdp_hevc_context_set_option(enc, FREERDP_HEVC_CONTEXT_OPTION_PROFILE, profile))
	{
		(void)fprintf(stderr, "HEVC %s: no encoder takes this profile\n", name);
		rc = 0;
		goto fail;
	}
	if (dec && !freerdp_hevc_context_set_option(dec, FREERDP_HEVC_CONTEXT_OPTION_PROFILE, profile))
	{
		freerdp_hevc_context_free(dec);
		dec = nullptr;
	}
	if (!dec)
		(void)fprintf(stderr, "HEVC %s: no decoder in this build, encode only\n", name);
#if defined(__APPLE__) && defined(WITH_APPLE_VIDEOTOOLBOX)
	/* VideoToolbox decodes HEVC 4:2:0 on macOS: the Mac client has no other HEVC decoder */
	if (!dec && (profile == 0))
	{
		(void)fprintf(stderr, "HEVC %s: WRONG, VideoToolbox should decode it\n", name);
		goto fail;
	}
#endif
	if (!freerdp_hevc_context_reset(enc, TEST_WIDTH, TEST_HEIGHT) ||
	    (dec && !freerdp_hevc_context_reset(dec, TEST_WIDTH, TEST_HEIGHT)) ||
	    !freerdp_hevc_context_set_option(enc, FREERDP_HEVC_CONTEXT_OPTION_BITRATE, 8000000))
		goto fail;

	fill_picture(src, step);
	if (freerdp_hevc_compress(enc, src, PIXEL_FORMAT_BGRX32, step, TEST_WIDTH, TEST_HEIGHT, &rect,
	                          &coded, &codedSize, &meta) <= 0)
	{
		(void)fprintf(stderr, "HEVC %s: compress failed\n", name);
		goto fail;
	}

	const int chroma = sps_chroma_format(coded, codedSize);
	const int want = profile ? 3 : 1;
	if (chroma != want)
	{
		(void)fprintf(stderr, "HEVC %s: the stream's chroma_format_idc is %d, want %d\n", name,
		              chroma, want);
		goto fail;
	}

	rc = 1;
	if (!dec)
		goto fail;

	if (freerdp_hevc_decompress(dec, coded, codedSize, dst, PIXEL_FORMAT_BGRX32, step, TEST_WIDTH,
	                            TEST_HEIGHT, &rect, 1) < 0)
	{
		(void)fprintf(stderr, "HEVC %s: decompress failed\n", name);
		rc = -1;
		goto fail;
	}

	/* which decoder made the picture: the client's stats and log name it the same way */
	FREERDP_DECODE_RESULT result = WINPR_C_ARRAY_INIT;
	if (!freerdp_hevc_context_get_last_decode(dec, &result))
	{
		rc = -1;
		goto fail;
	}
	const char* backend = result.backend ? result.backend : "none";
	const char* hardware = (result.hardware == FREERDP_DECODE_HARDWARE_YES)  ? "hardware"
	                       : (result.hardware == FREERDP_DECODE_HARDWARE_NO) ? "software"
	                                                                         : "unknown";
	(void)fprintf(stderr, "HEVC %s: decoded by %s (%s), status %d (picture is %d)\n", name, backend,
	              hardware, (int)result.status, (int)FREERDP_DECODE_PICTURE);
	if (result.status != FREERDP_DECODE_PICTURE)
		rc = -1;
#if defined(__APPLE__) && defined(WITH_APPLE_VIDEOTOOLBOX)
	if ((profile == 0) && (strcmp(backend, "VideoToolbox") != 0))
	{
		(void)fprintf(stderr, "HEVC %s: WRONG, want VideoToolbox\n", name);
		rc = -1;
	}
#endif

	for (size_t q = 0; q < 4; q++)
	{
		const UINT32 x = ((q & 1) ? 3 : 1) * TEST_WIDTH / 4;
		const UINT32 y = ((q & 2) ? 3 : 1) * TEST_HEIGHT / 4;
		const UINT32 color =
		    FreeRDPReadColor(&dst[1ull * y * step + 4ull * x], PIXEL_FORMAT_BGRX32);
		BYTE c[3] = WINPR_C_ARRAY_INIT;
		FreeRDPSplitColor(color, PIXEL_FORMAT_BGRX32, &c[0], &c[1], &c[2], nullptr, nullptr);
		int worst = 0;
		for (size_t k = 0; k < 3; k++)
			worst = MAX(worst, abs((int)c[k] - (int)quadrant_colours[q][k]));
		/* every quadrant is printed, so a passing run shows its colours too */
		(void)fprintf(stderr,
		              "HEVC %s: quadrant %" PRIuz
		              " is (%u, %u, %u), want (%u, %u, %u), off by %d%s\n",
		              name, q, c[0], c[1], c[2], quadrant_colours[q][0], quadrant_colours[q][1],
		              quadrant_colours[q][2], worst, (worst > 24) ? ", more than 24: WRONG" : "");
		if (worst > 24)
			rc = -1;
	}

fail:
	free_h264_metablock(&meta);
	freerdp_hevc_context_free(enc);
	freerdp_hevc_context_free(dec);
	free(src);
	free(dst);
	return rc;
}

/* The encoder's bit rate, in bit/s. */
static int test_bitrate(void)
{
	FREERDP_HEVC_CONTEXT* hevc = freerdp_hevc_context_new(TRUE);
	if (!hevc)
	{
		(void)fprintf(stderr, "no HEVC encoder in this build\n");
		return 0;
	}

	int rc = -1;
	/* 500 kbit/s, given in bit/s */
	if (!freerdp_hevc_context_set_option(hevc, FREERDP_HEVC_CONTEXT_OPTION_BITRATE, 500000) ||
	    (freerdp_hevc_context_get_option(hevc, FREERDP_HEVC_CONTEXT_OPTION_BITRATE) != 500000))
	{
		(void)fprintf(stderr, "the bit rate is not taken\n");
		goto fail;
	}

	rc = 0;
fail:
	freerdp_hevc_context_free(hevc);
	return rc;
}

/* A new context is 4:2:0 (profile 0) until the caller asks for 4:4:4. */
static int test_default_profile(void)
{
	int rc = 0;
	for (BOOL compressor = FALSE; compressor <= TRUE; compressor++)
	{
		FREERDP_HEVC_CONTEXT* hevc = freerdp_hevc_context_new(compressor);
		if (!hevc)
			continue;
		const UINT32 profile =
		    freerdp_hevc_context_get_option(hevc, FREERDP_HEVC_CONTEXT_OPTION_PROFILE);
		if (profile != 0)
		{
			(void)fprintf(stderr, "a new HEVC %s starts at profile %" PRIu32 ", want 0 (4:2:0)\n",
			              compressor ? "encoder" : "decoder", profile);
			rc = -1;
		}
		freerdp_hevc_context_free(hevc);
	}
	return rc;
}

int TestFreeRDPCodecHEVC(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	int rc = 0;
	if (test_bitrate() < 0)
		rc = -1;
	if (test_default_profile() < 0)
		rc = -1;
	if (round_trip(0) < 0)
		rc = -1;
	if (round_trip(1) < 0)
		rc = -1;
	return rc;
}
