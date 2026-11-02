/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A check under Wine (or Windows) of kl_window_on_resize: the sizes of a
 * window's content during the system's modal size loop.  The program
 * opens a window of an application, prints "resize-test: ready <x> <y>
 * <width> <height>" (the frame on the screen), and waits for three drags
 * of the frame's right edge (resize-wine.sh drives them with xdotool),
 * each one the system's modal size loop inside kl_app_dispatch, seen by a
 * hook on the window's thread (WM_ENTERSIZEMOVE, WM_SIZE, WM_EXITSIZEMOVE):
 *   1. without a callback (the default): nothing is called, and the
 *      KL_WINDOW_RESIZE comes after the loop;
 *   2. with a callback: each size comes to it on the window's thread while
 *      the loop runs, kl_window_size reports it there, the sizes grow,
 *      and the KL_WINDOW_RESIZE after the loop has the last of them;
 *   3. with the callback stopped (NULL): nothing more is called.
 * After each it prints "resize-test: round <n> done".  The deferred
 * configures have nothing to do on Windows.  Prints "resize-test: PASS
 * ..." or "resize-test: FAIL ..." and exits 0 or 1.
 */

#include <keiland/keiland.h>

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define RESIZE_FRAME_CLASS	L"KeilandWindow"
#define RESIZE_ROUND_MS		30000U

/* What the callback saw (the window's thread only). */
struct seen {
	struct kl_window *window;
	DWORD thread;
	unsigned calls;
	unsigned outside;
	unsigned wrong_thread;
	unsigned wrong_size;
	unsigned shrank;
	uint32_t width;
	uint32_t height;
};

static struct seen seen;
static unsigned failures;

/* The frame's messages of the modal size loop, seen by a hook on the window's thread. */
static HWND hooked_frame;
static int in_loop;
static unsigned loops;
static unsigned loop_sizes;

static void
fail(
	const char *what)
{
	printf("resize-test: FAIL %s\n", what);
	fflush(stdout);
	failures++;
}

static void
resize_heard(
	void *data,
	uint32_t width,
	uint32_t height)
{
	struct seen *state;
	uint32_t told_width;
	uint32_t told_height;

	state = data;
	state->calls++;
	if (!in_loop)
		state->outside++;
	if (GetCurrentThreadId() != state->thread)
		state->wrong_thread++;
	kl_window_size(state->window, &told_width, &told_height);
	if (told_width != width || told_height != height)
		state->wrong_size++;
	if (state->calls > 1U && width < state->width)
		state->shrank++;
	state->width = width;
	state->height = height;
}

static LRESULT CALLBACK
hook_message(
	int code,
	WPARAM wparam,
	LPARAM lparam)
{
	const CWPSTRUCT *message;

	message = (const CWPSTRUCT *)lparam;
	if (code == HC_ACTION && message->hwnd == hooked_frame) {
		if (message->message == WM_ENTERSIZEMOVE) {
			in_loop = 1;
			loops++;
		}
		if (message->message == WM_EXITSIZEMOVE)
			in_loop = 0;
		if (message->message == WM_SIZE && in_loop)
			loop_sizes++;
	}
	return CallNextHookEx(NULL, code, wparam, lparam);
}

/* Dispatches until a modal size loop ended and its KL_WINDOW_RESIZE was taken; the KL_WINDOW_RESIZE count. */
static unsigned
size_round(
	struct kl_app *app,
	unsigned round)
{
	struct kl_app_event event;
	unsigned loops_before;
	unsigned sizes_before;
	unsigned resizes;
	DWORD start;
	int ended;

	loops_before = loops;
	sizes_before = loop_sizes;
	resizes = 0;
	ended = 0;
	start = GetTickCount();
	while (GetTickCount() - start < RESIZE_ROUND_MS) {
		(void)kl_app_dispatch(app, 50);
		while (kl_app_take(app, &event)) {
			if (event.kind == KL_APP_WINDOW && event.input.kind == KL_WINDOW_RESIZE && loops != loops_before)
				resizes++;
		}
		if (loops != loops_before && !in_loop && resizes != 0U) {
			if (ended++ > 10)
				break;
		}
	}
	if (loops == loops_before)
		fail("no modal size loop came");
	printf("resize-test: round %u done (%u loops, %u sizes in them, %u resize inputs)\n", round, loops - loops_before,
	    loop_sizes - sizes_before, resizes);
	fflush(stdout);
	return resizes;
}

int
main(void)
{
	struct kl_window_configure configure;
	struct kl_window_options options;
	struct kl_app_event event;
	struct kl_app_options app_options;
	struct kl_window *window;
	struct kl_app *app;
	uint32_t width_before;
	uint32_t height_before;
	uint32_t width;
	uint32_t height;
	unsigned calls_before;
	unsigned resizes;
	RECT place;
	HWND frame;
	int status;

	/* Contents shown as they are sized (the system's choice otherwise). */
	(void)SystemParametersInfoW(SPI_SETDRAGFULLWINDOWS, TRUE, NULL, 0);

	/* The application and its window, its frame at a known place. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "resize-test";
	app = kl_app_open(&app_options);
	if (app == NULL) {
		printf("resize-test: FAIL kl_app_open %d\n", errno);
		return 1;
	}
	memset(&options, 0, sizeof(options));
	options.title = "resize-test";
	options.width = 640U;
	options.height = 400U;
	options.present = KL_PRESENT_NONE;
	window = kl_app_window_create(app, &options);
	if (window == NULL) {
		printf("resize-test: FAIL kl_app_window_create %d\n", errno);
		return 1;
	}
	frame = FindWindowW(RESIZE_FRAME_CLASS, NULL);
	if (frame == NULL) {
		printf("resize-test: FAIL no frame\n");
		return 1;
	}
	hooked_frame = frame;
	if (SetWindowsHookExW(WH_CALLWNDPROC, hook_message, NULL, GetCurrentThreadId()) == NULL)
		fail("SetWindowsHookExW");
	(void)SetWindowPos(frame, NULL, 100, 100, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
	(void)SetForegroundWindow(frame);

	/* No configures on Windows. */
	status = kl_window_set_configure_deferred(window, 1);
	if (status != 0)
		fail("set_configure_deferred");
	status = kl_window_configure_take(window, &configure);
	if (status != EAGAIN)
		fail("configure_take is not EAGAIN");
	status = kl_window_configure_ack(window, 1U);
	if (status != EINVAL)
		fail("configure_ack is not EINVAL");

	/* Settled, then the place for the drags. */
	(void)kl_app_dispatch(app, 200);
	while (kl_app_take(app, &event) == 1)
		continue;
	(void)GetWindowRect(frame, &place);
	printf("resize-test: ready %ld %ld %ld %ld\n", (long)place.left, (long)place.top, (long)(place.right - place.left),
	    (long)(place.bottom - place.top));
	fflush(stdout);

	/* 1. The default: no callback, the size after the loop. */
	seen.window = window;
	seen.thread = GetCurrentThreadId();
	kl_window_size(window, &width_before, &height_before);
	resizes = size_round(app, 1U);
	kl_window_size(window, &width, &height);
	printf("resize-test: default %ux%u -> %ux%u\n", width_before, height_before, width, height);
	if (width <= width_before)
		fail("the first drag did not widen the window");
	if (resizes == 0U)
		fail("no KL_WINDOW_RESIZE after the default loop");
	if (seen.calls != 0U)
		fail("a callback without one asked");

	/* 2. With the callback: every size inside the loop. */
	kl_window_on_resize(window, resize_heard, &seen);
	width_before = width;
	resizes = size_round(app, 2U);
	kl_window_size(window, &width, &height);
	printf("resize-test: callback %u calls, last %ux%u, window %ux%u\n", seen.calls, seen.width, seen.height, width, height);
	if (seen.calls < 2U)
		fail("fewer than two sizes inside the loop");
	if (seen.outside != 0U)
		fail("a call outside the loop");
	if (seen.wrong_thread != 0U)
		fail("a call on another thread");
	if (seen.wrong_size != 0U)
		fail("kl_window_size differs inside the callback");
	if (seen.shrank != 0U)
		fail("a size shrank while the right edge moved right");
	if (width <= width_before || seen.width != width || seen.height != height)
		fail("the last size heard is not the window's");
	if (resizes == 0U)
		fail("no KL_WINDOW_RESIZE at the loop's end");

	/* 3. Stopped again: nothing more is heard. */
	kl_window_on_resize(window, NULL, NULL);
	calls_before = seen.calls;
	resizes = size_round(app, 3U);
	if (seen.calls != calls_before)
		fail("a call after the callback was stopped");
	if (resizes == 0U)
		fail("no KL_WINDOW_RESIZE after the callback was stopped");

	kl_window_close(window);
	kl_app_close(app);
	if (failures != 0U) {
		printf("resize-test: FAIL %u\n", failures);
		return 1;
	}
	printf("resize-test: PASS calls=%u last=%ux%u\n", calls_before, seen.width, seen.height);
	return 0;
}
