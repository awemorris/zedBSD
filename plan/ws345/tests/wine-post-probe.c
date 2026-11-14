/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The control of guest-latency.sh's Wine cases: a Windows program with
 * nothing of the browser, whose thread posts a message to the main thread
 * every 5 ms (PostThreadMessage, as libkeiland's post does) for SECONDS
 * (default 60) and times each post and each message's wait until the main
 * thread takes it (GetMessage).  Each post or wait over 20 ms is printed
 * with the seconds since the start, then the longest of each.  Under Wine a
 * post that stalls is the Wine server's, since nothing else runs here.
 *
 *   x86_64-w64-mingw32-clang -O1 -o wine-post-probe.exe plan/ws345/tests/wine-post-probe.c
 *   wine wine-post-probe.exe [SECONDS]
 */

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

/* How often the message is posted, in milliseconds. */
#define PROBE_PERIOD_MS		5

/* The waits printed, in milliseconds. */
#define PROBE_LONG_MS		20.0

/* The message the posting thread sends. */
#define PROBE_MESSAGE		(WM_USER + 1)

static volatile LONG probe_done;
static volatile double probe_sent_ms;
static DWORD probe_main;
static double probe_start_ms;
static double probe_seconds;
static double probe_post_longest_ms;

/* The monotonic clock, in milliseconds. */
static double
probe_now(void)
{
	LARGE_INTEGER frequency;
	LARGE_INTEGER counter;

	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

/* The posting thread: a message every PROBE_PERIOD_MS, each post timed. */
static DWORD WINAPI
probe_post(
	LPVOID argument)
{
	double sent;
	double took;

	(void)argument;
	while (probe_now() - probe_start_ms < probe_seconds * 1000.0) {
		sent = probe_now();
		probe_sent_ms = sent;
		PostThreadMessage(probe_main, PROBE_MESSAGE, 0, 0);
		took = probe_now() - sent;
		if (took > probe_post_longest_ms)
			probe_post_longest_ms = took;
		if (took > PROBE_LONG_MS) {
			printf("post stalled %.1f ms at %.1f s\n", took, (sent - probe_start_ms) / 1000.0);
			fflush(stdout);
		}
		Sleep(PROBE_PERIOD_MS);
	}

	/* The end: the main thread's wait ends. */
	InterlockedExchange(&probe_done, 1);
	PostThreadMessage(probe_main, WM_USER, 0, 0);
	return 0;
}

int
main(
	int argc,
	char **argv)
{
	MSG message;
	double waited;
	double longest;

	probe_seconds = argc > 1 ? atof(argv[1]) : 60.0;
	probe_main = GetCurrentThreadId();
	probe_start_ms = probe_now();

	/* The main thread's queue, before the posting thread starts. */
	PeekMessage(&message, NULL, 0, 0, PM_NOREMOVE);
	CreateThread(NULL, 0, probe_post, NULL, 0, NULL);

	/* Takes the messages, each one's wait timed. */
	longest = 0.0;
	while (!probe_done && GetMessage(&message, NULL, 0, 0) > 0) {
		if (message.message != PROBE_MESSAGE)
			continue;
		waited = probe_now() - probe_sent_ms;
		if (waited > longest)
			longest = waited;
		if (waited > PROBE_LONG_MS) {
			printf("message waited %.1f ms at %.1f s\n", waited, (probe_now() - probe_start_ms) / 1000.0);
			fflush(stdout);
		}
	}
	printf("wine-post-probe: longest wait %.1f ms, longest post %.1f ms\n", longest, probe_post_longest_ms);
	return 0;
}
