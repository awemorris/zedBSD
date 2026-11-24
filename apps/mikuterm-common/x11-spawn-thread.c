/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * In-process thread implementation of x11-spawn.h: runs each SSH
 * connection's X server on a dedicated kl_thread using the tab-only
 * backend and a non-blocking kl_socket link.
 */

#include "x11-host.h"
#include "x11-serve.h"
#include "x11-spawn.h"
#include "x11-tab-only.h"
#include "x11-thread.h"

#include "userland/desktop/xserver/win32/scale.h"

#include <keiland/keiland_socket.h>
#include <keiland/keiland_thread.h>
#include <keiland/keiland_window.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Polling interval while waiting for the X server thread to stop (ms). */
#define THREAD_STOP_POLL_MS	10U

/* Maximum poll wait when no earlier core timer is pending (ms). */
#define THREAD_WAIT_MS		1000U

/* Minimum rootless screen side in X pixels. */
#define THREAD_ROOTLESS_MIN	640U

/* Default longest screen side when the host window size is not yet known. */
#define THREAD_DEFAULT_SIDE	1280U

/*
 * Per-connection X server thread state shared only for reference counting
 * and shutdown synchronization; the link socket carries all data.
 */
struct x11_thread {
	atomic_uint references;
	atomic_int done;
	struct kl_thread *thread;
	struct kl_socket *end;
	struct x11_serve serve;
	struct x11_serve_system system;
	struct x11_tab_only *tabs;
};

static void thread_release(struct x11_thread *state);
static void thread_run(void *argument);
static int thread_receive(void *context, void *buffer, size_t size, size_t *received);
static int thread_send(void *context, const void *bytes, size_t size, size_t *sent);
static int thread_start(void *context, struct x11_serve *serve);
static int thread_tab_take(void *context, const struct x11_link_frame *frame);
static void thread_tab_send(void *context);
static int thread_link_frame(void *context, unsigned type, uint32_t id, const void *body, uint32_t size);
static size_t thread_link_waiting(void *context);
static uint32_t thread_now(void);
static unsigned thread_clamp_scale(unsigned percent);

/*
 * No listener guard is required because the loopback socket is bound
 * exclusively within the process.
 */
void
x11_spawn_guard(
	struct kl_socket *listener)
{
	(void)listener;
}

/*
 * Starts an X server thread for host using child_end as its link socket.
 * Transfers ownership of child_end on success; returns 0 or an errno value.
 */
int
x11_spawn_start(
	struct x11_host *host,
	struct kl_socket *child_end)
{
	struct x11_thread *state;
	struct kl_thread_options options;
	int error;

	error = kl_socket_set_nonblocking(child_end, 1);
	if (error != 0)
		return error;

	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	atomic_init(&state->references, 2U);
	atomic_init(&state->done, 0);
	state->end = child_end;

	memset(&options, 0, sizeof(options));
	options.name = X11_THREAD_NAME;
	options.run = thread_run;
	options.argument = state;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	options.stack_size = X11_THREAD_STACK;

	/* Thread creation is the last failable step before transferring child_end. */
	error = kl_thread_start(&options, &state->thread);
	if (error != 0) {
		free(state);
		return error;
	}

	host->process = state;
	host->child_end = NULL;
	return 0;
}

/*
 * Waits briefly for the X server thread to exit after its link closes,
 * joining if finished or detaching otherwise, then drops host's reference.
 */
void
x11_spawn_stop(
	struct x11_host *host)
{
	struct x11_thread *state;
	unsigned waited;

	state = host->process;
	if (state == NULL)
		return;

	for (waited = 0U; waited < X11_SPAWN_END_MS; waited += THREAD_STOP_POLL_MS) {
		if (atomic_load(&state->done) != 0)
			break;
		kl_thread_sleep(THREAD_STOP_POLL_MS);
	}

	if (atomic_load(&state->done) != 0)
		(void)kl_thread_join(state->thread);
	else
		(void)kl_thread_detach(state->thread);

	state->thread = NULL;
	host->process = NULL;
	thread_release(state);
}

/*
 * Foreground window activation is a no-op when all X windows live in tabs.
 */
void
x11_host_foreground(
	const struct x11_host *host)
{
	(void)host;
}

/* Drops one reference to state and frees it when the last holder releases it. */
static void
thread_release(
	struct x11_thread *state)
{
	if (atomic_fetch_sub(&state->references, 1U) == 1U)
		free(state);
}

/* Runs the X server event loop until the link ends or fails. */
static void
thread_run(
	void *argument)
{
	struct x11_thread *state;
	struct kl_socket_wait wait;
	uint32_t now;
	uint32_t wake;
	int timeout;
	size_t ready;
	int error;

	state = argument;
	state->system.receive = thread_receive;
	state->system.send = thread_send;
	state->system.start = thread_start;
	state->system.tab_take = thread_tab_take;
	state->system.tab_send = thread_tab_send;
	state->system.context = state;

	error = x11_serve_init(&state->serve, &state->system);
	if (error != 0)
		goto out;

	for (;;) {
		x11_serve_round(&state->serve);
		if (state->serve.ending)
			break;

		timeout = (int)THREAD_WAIT_MS;
		if (state->serve.server != NULL) {
			now = thread_now();
			wake = now + THREAD_WAIT_MS;
			(void)x11server_tick(state->serve.server, now, &wake);
			if ((int32_t)(wake - now) < timeout)
				timeout = (int)(int32_t)(wake - now);
			if (timeout < 0)
				timeout = 0;

			x11_serve_flush(&state->serve);
			if (state->serve.ending)
				break;
		}

		wait.socket = state->end;
		wait.waker = NULL;
		wait.events = KL_SOCKET_READABLE;
		if (x11_serve_waiting(&state->serve) != 0U)
			wait.events |= KL_SOCKET_WRITABLE;
		wait.ready = 0U;

		error = kl_socket_poll(&wait, 1U, timeout, &ready);
		if (error != 0) {
			state->serve.ending = X11_SERVE_ENDED;
			break;
		}
	}

	x11_serve_end(&state->serve);

out:
	x11_tab_only_destroy(state->tabs);
	state->tabs = NULL;
	kl_socket_close(state->end);
	state->end = NULL;
	atomic_store(&state->done, 1);
	thread_release(state);
}

/* Receives bytes from the non-blocking link socket. */
static int
thread_receive(
	void *context,
	void *buffer,
	size_t size,
	size_t *received)
{
	struct x11_thread *state;

	state = context;
	return kl_socket_receive(state->end, buffer, size, received);
}

/* Sends bytes on the non-blocking link socket. */
static int
thread_send(
	void *context,
	const void *bytes,
	size_t size,
	size_t *sent)
{
	struct x11_thread *state;

	state = context;
	return kl_socket_send(state->end, bytes, size, sent);
}

/* Starts the tab-only backend and X server core once HELLO arrives. */
static int
thread_start(
	void *context,
	struct x11_serve *serve)
{
	struct x11_thread *state;
	struct x11_tab_only_link link;
	struct x11server_options options;
	unsigned screen_w;
	unsigned screen_h;
	unsigned screen_pct;
	unsigned scale;
	unsigned fit_w;
	unsigned fit_h;
	unsigned fit_pct;
	unsigned longest;
	unsigned side;
	int error;

	state = context;
	screen_w = 0U;
	screen_h = 0U;
	screen_pct = 0U;
	mikuterm_x11_screen(&screen_w, &screen_h, &screen_pct);

	scale = serve->scale;
	if (scale == 0U) {
		if (serve->root_width != 0U && serve->root_height != 0U && screen_w != 0U && screen_h != 0U) {
			fit_w = screen_w * 100U / serve->root_width;
			fit_h = screen_h * 100U / serve->root_height;
			fit_pct = (fit_w < fit_h) ? fit_w : fit_h;
			if (screen_pct != 0U && screen_pct < fit_pct)
				scale = screen_pct;
			else
				scale = fit_pct;
		} else {
			scale = screen_pct;
		}
		scale = thread_clamp_scale(scale);
	}

	link.frame = thread_link_frame;
	link.waiting = thread_link_waiting;
	link.context = state;
	error = x11_tab_only_create(&link, scale, &state->tabs);
	if (error != 0)
		return error;

	memset(&options, 0, sizeof(options));
	if (serve->root_width != 0U && serve->root_height != 0U) {
		options.mode = X11SERVER_ROOTED;
		options.width = serve->root_width;
		options.height = serve->root_height;
	} else {
		longest = (screen_w > screen_h) ? screen_w : screen_h;
		if (longest == 0U)
			longest = THREAD_DEFAULT_SIDE;
		side = longest * 100U / scale;
		if (side < THREAD_ROOTLESS_MIN)
			side = THREAD_ROOTLESS_MIN;
		options.mode = X11SERVER_ROOTLESS;
		options.width = side;
		options.height = side;
	}

	options.backend = &x11_tab_only_ops;
	options.backend_context = state->tabs;
	options.vulkan_loader = NULL;
	options.client_memory_max = X11_THREAD_CLIENT_MEMORY;
	options.server_memory_max = X11_THREAD_SERVER_MEMORY;
	x11_serve_options(serve, &options);

	error = x11server_create(&options, thread_now(), &serve->server);
	if (error != 0) {
		x11_tab_only_destroy(state->tabs);
		state->tabs = NULL;
		return error;
	}

	x11_tab_only_attach(state->tabs, serve->server);
	return 0;
}

/* Delivers one incoming tab frame from the link to the tab-only backend. */
static int
thread_tab_take(
	void *context,
	const struct x11_link_frame *frame)
{
	struct x11_thread *state;

	state = context;
	if (state->tabs == NULL)
		return 0;
	return x11_tab_only_take(state->tabs, frame, thread_now());
}

/* Flushes pending tab open and pixel frames onto the link. */
static void
thread_tab_send(
	void *context)
{
	struct x11_thread *state;

	state = context;
	if (state->tabs != NULL)
		x11_tab_only_send(state->tabs, thread_now());
}

/* Queues one tab frame onto the serve link. */
static int
thread_link_frame(
	void *context,
	unsigned type,
	uint32_t id,
	const void *body,
	uint32_t size)
{
	struct x11_thread *state;

	state = context;
	return x11_serve_frame(&state->serve, type, id, body, size);
}

/* Returns bytes currently queued for writing on the serve link. */
static size_t
thread_link_waiting(
	void *context)
{
	struct x11_thread *state;

	state = context;
	return x11_serve_waiting(&state->serve);
}

/* Returns monotonic milliseconds for X server core timestamps. */
static uint32_t
thread_now(void)
{
	return (uint32_t)(kl_clock_us() / 1000ULL);
}

/* Rounds percent down to a multiple of X11_WIN32_SCALE_STEP within [100, 400]. */
static unsigned
thread_clamp_scale(
	unsigned percent)
{
	unsigned rounded;

	if (percent <= X11_WIN32_SCALE_MIN)
		return X11_WIN32_SCALE_MIN;
	if (percent >= X11_WIN32_SCALE_MAX)
		return X11_WIN32_SCALE_MAX;
	rounded = (percent / X11_WIN32_SCALE_STEP) * X11_WIN32_SCALE_STEP;
	if (rounded < X11_WIN32_SCALE_MIN)
		rounded = X11_WIN32_SCALE_MIN;
	return rounded;
}
