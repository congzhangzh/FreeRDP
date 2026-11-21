/* The frame update in two halves (shadow_subsystem_frame_publish() / _wait(), on the multi-client
 * event): a publish returns before the clients took the frame, a wait returns only when every
 * one did, a publish after a publish waits for the first, and a client that never takes it is
 * dropped after the timeout as before. Two client threads that "encode" for a while. */
#include <stdio.h>

#include <winpr/wtypes.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/sysinfo.h>
#include <winpr/interlocked.h>

#include "../shadow_mcevent.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return FALSE;                                                    \
		}                                                                    \
	} while (0)

typedef struct
{
	rdpShadowMultiClientEvent* event;
	void* subscriber;
	DWORD encodeMs;
	volatile LONG taken; /* frames this client took */
	volatile LONG stop;
	BOOL never; /* takes nothing: a stuck client */
} FAKE_CLIENT;

static DWORD WINAPI fake_client(LPVOID arg)
{
	FAKE_CLIENT* c = arg;
	HANDLE h = shadow_multiclient_getevent(c->subscriber);
	while (InterlockedCompareExchange(&c->stop, 0, 0) == 0)
	{
		if (WaitForSingleObject(h, 20) != WAIT_OBJECT_0)
			continue;
		if (c->never)
		{
			Sleep(5);
			continue;
		}
		Sleep(c->encodeMs); /* the encode, from the surface */
		(void)InterlockedIncrement(&c->taken);
		(void)shadow_multiclient_consume(c->subscriber);
	}
	return 0;
}

static BOOL run(DWORD encodeA, DWORD encodeB, BOOL stuck)
{
	rdpShadowMultiClientEvent* event = shadow_multiclient_new();
	CHECK(event);
	FAKE_CLIENT a = { event, shadow_multiclient_get_subscriber(event), encodeA, 0, 0, FALSE };
	FAKE_CLIENT b = { event, shadow_multiclient_get_subscriber(event), encodeB, 0, 0, stuck };
	CHECK(a.subscriber && b.subscriber);
	HANDLE ta = CreateThread(nullptr, 0, fake_client, &a, 0, nullptr);
	HANDLE tb = CreateThread(nullptr, 0, fake_client, &b, 0, nullptr);
	CHECK(ta && tb);

	/* publish returns at once, though both clients take 50 ms */
	UINT64 t0 = GetTickCount64();
	shadow_multiclient_publish(event);
	const UINT64 publishMs = GetTickCount64() - t0;
	CHECK(publishMs < 20);

	/* the next capture runs meanwhile: 40 ms; then the wait ends when both took the frame */
	Sleep(40);
	shadow_multiclient_wait(event);
	const UINT64 roundMs = GetTickCount64() - t0;
	if (!stuck)
	{
		CHECK(InterlockedCompareExchange(&a.taken, 0, 0) == 1);
		CHECK(InterlockedCompareExchange(&b.taken, 0, 0) == 1);
		/* the capture overlapped the encode: about max(40, 50), not 40 + 50 */
		CHECK(roundMs >= 45);
		CHECK(roundMs < 85);
	}
	else
	{
		/* a client that never takes it costs the timeout, then the round is dropped */
		CHECK(roundMs >= 900);
	}

	/* ten rounds as shadow_subsystem_frame_publish() runs them: wait for the round before, then
	 * publish */
	for (int x = 0; x < 10; x++)
	{
		shadow_multiclient_wait(event);
		shadow_multiclient_publish(event);
		Sleep(10);
	}
	shadow_multiclient_wait(event);
	if (!stuck)
	{
		CHECK(InterlockedCompareExchange(&a.taken, 0, 0) == 11);
		CHECK(InterlockedCompareExchange(&b.taken, 0, 0) == 11);
	}

	(void)InterlockedExchange(&a.stop, 1);
	(void)InterlockedExchange(&b.stop, 1);
	(void)WaitForSingleObject(ta, INFINITE);
	(void)WaitForSingleObject(tb, INFINITE);
	(void)CloseHandle(ta);
	(void)CloseHandle(tb);
	shadow_multiclient_release_subscriber(a.subscriber);
	shadow_multiclient_release_subscriber(b.subscriber);
	shadow_multiclient_free(event);
	return TRUE;
}

int TestShadowPipeline(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	if (!run(50, 50, FALSE))
		return -1;
	if (!run(50, 30, FALSE))
		return -1;
	if (!run(50, 0, TRUE))
		return -1;
	return 0;
}
