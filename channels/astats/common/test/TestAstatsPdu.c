/* FreeRDP::Advanced::Stats PDUs: written and read back, an unknown id kept for the reader to
 * skip, an unknown type read as nothing, and PDUs cut short or too large refused */
#include <stdio.h>
#include <string.h>

#include <winpr/stream.h>

#include "../astats_common.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			goto fail;                                                       \
		}                                                                    \
	} while (0)

int TestAstatsPdu(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	int rc = -1;
	ASTATS_PDU pdu = { 0 };
	wStream* s = Stream_New(nullptr, 64);
	CHECK(s);

	/* HELLO */
	CHECK(astats_write_hello(s, 0x00ab00cd00ef0012ull));
	CHECK(Stream_GetPosition(s) == 12);
	{
		const BYTE expected[] = { 1, 0, 1, 0, 0x12, 0, 0xef, 0, 0xcd, 0, 0xab, 0 };
		CHECK(memcmp(Stream_Buffer(s), expected, sizeof(expected)) == 0);
	}
	Stream_SealLength(s);
	Stream_ResetPosition(s);
	CHECK(astats_read(s, &pdu));
	CHECK((pdu.pduType == ASTATS_PDU_HELLO) && (pdu.connectionId == 0x00ab00cd00ef0012ull));

	/* REPORT, with an id this reader does not know */
	{
		const ASTATS_VALUE values[] = { { ASTATS_ID_SENT, 290 },
			                            { ASTATS_ID_FRAME_ID_LAST, 0xffffffffull },
			                            { 999, 7 } };
		Stream_ResetPosition(s);
		CHECK(astats_write_report(s, values, ARRAYSIZE(values)));
		CHECK(Stream_GetPosition(s) == 6 + 10 * ARRAYSIZE(values));
		Stream_SealLength(s);
		Stream_ResetPosition(s);
		CHECK(astats_read(s, &pdu));
		CHECK((pdu.pduType == ASTATS_PDU_REPORT) && (pdu.count == 3));
		CHECK((pdu.values[0].id == ASTATS_ID_SENT) && (pdu.values[0].value == 290));
		CHECK(pdu.values[1].value == 0xffffffffull);
		CHECK((pdu.values[2].id == 999) && (pdu.values[2].value == 7));

		/* cut short: one byte missing */
		Stream_ResetPosition(s);
		Stream_SetLength(s, 6 + 10 * ARRAYSIZE(values) - 1);
		CHECK(!astats_read(s, &pdu));
	}

	/* an unknown type: read, nothing in it */
	{
		const BYTE unknown[] = { 9, 0, 1, 0, 1, 2, 3 };
		wStream sbuffer = { 0 };
		wStream* u = Stream_StaticConstInit(&sbuffer, unknown, sizeof(unknown));
		CHECK(astats_read(u, &pdu));
		CHECK((pdu.pduType == 9) && (pdu.count == 0));
	}

	/* a header cut short, a HELLO cut short, a report claiming too many values */
	{
		const BYTE header[] = { 1, 0, 1 };
		const BYTE hello[] = { 1, 0, 1, 0, 1, 2, 3 };
		const BYTE many[] = { 2, 0, 1, 0, 0xff, 0xff };
		wStream sbuffer = { 0 };
		CHECK(!astats_read(Stream_StaticConstInit(&sbuffer, header, sizeof(header)), &pdu));
		CHECK(!astats_read(Stream_StaticConstInit(&sbuffer, hello, sizeof(hello)), &pdu));
		CHECK(!astats_read(Stream_StaticConstInit(&sbuffer, many, sizeof(many)), &pdu));
	}

	/* the writer refuses more than a report may carry */
	{
		ASTATS_VALUE values[ASTATS_MAX_VALUES + 1] = { 0 };
		Stream_ResetPosition(s);
		CHECK(!astats_write_report(s, values, ARRAYSIZE(values)));
		CHECK(astats_write_report(s, values, ASTATS_MAX_VALUES));
	}
	rc = 0;
fail:
	Stream_Free(s, TRUE);
	return rc;
}
