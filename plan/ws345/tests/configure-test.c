/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A host test of libkeiland's deferred configures (kl_window_set_configure_deferred,
 * kl_window_configure_take and _ack) against configure-server.c.  A window of an
 * application (KL_PRESENT_NONE) is drawn by a thread of the test's own, which
 * commits wl_shm buffers on the window's surface through its own queue (as a
 * Vulkan swapchain of another thread does) and asks the window's thread for the
 * acknowledgement with kl_window_post before the first frame of a new size.  The
 * server checks that no frame is committed at a size other than the configure
 * acknowledged last, and that every acknowledgement names a serial sent and not
 * consumed.  It checks the default too (a configure acknowledged as it arrives,
 * so a frame of the old size after it is caught: the checker's control).
 */

#define _GNU_SOURCE

#include "configure-server.h"

#include <keiland/keiland.h>
#include <wayland-client.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CODE_ACK	7U

/* A frame the drawing thread is asked for. */
struct request {
	uint32_t width;
	uint32_t height;
	uint32_t serial;
	int ack;
};

/* The drawing thread and what it shares with the window's thread (lock). */
static struct kl_window *window;
static struct wl_display *display;
static struct wl_event_queue *draw_queue;
static struct wl_shm *shm;
static pthread_t draw_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct request request;
static int request_waiting;
static int request_done;
static int acked;
static int ack_status;
static int draw_stop;
static unsigned resizes;
static unsigned failures;

static void
fail(
	const char *what)
{
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

static uint64_t
now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void
registry_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	(void)data;
	(void)version;
	if (strcmp(interface, "wl_shm") == 0 && shm == NULL)
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1U);
}

static void
registry_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global, registry_remove
};

/* Commits a frame of a size on the window's surface, from the drawing thread. */
static int
draw_commit(
	uint32_t width,
	uint32_t height)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	size_t size;
	int fd;

	size = (size_t)width * height * 4U;
	fd = memfd_create("configure-test", MFD_CLOEXEC);
	if (fd < 0)
		return errno;
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return errno;
	}
	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height, (int32_t)(width * 4U), WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(kl_window_surface(window), buffer, 0, 0);
	wl_surface_damage(kl_window_surface(window), 0, 0, (int32_t)width, (int32_t)height);
	wl_surface_commit(kl_window_surface(window));
	wl_display_flush(display);
	(void)wl_display_roundtrip_queue(display, draw_queue);
	wl_buffer_destroy(buffer);
	return 0;
}

static void *
draw_run(
	void *argument)
{
	struct request taken;
	int error;

	(void)argument;
	pthread_mutex_lock(&lock);
	for (;;) {
		while (!request_waiting && !draw_stop)
			pthread_cond_wait(&changed, &lock);
		if (draw_stop)
			break;
		taken = request;
		request_waiting = 0;
		pthread_mutex_unlock(&lock);

		/* The acknowledgement first, done by the window's thread, waited for here. */
		if (taken.ack) {
			error = kl_window_post(window, CODE_ACK, (void *)(uintptr_t)taken.serial);
			if (error != 0)
				fail("kl_window_post");
			pthread_mutex_lock(&lock);
			while (!acked && !draw_stop)
				pthread_cond_wait(&changed, &lock);
			acked = 0;
			pthread_mutex_unlock(&lock);
		}

		/* Then the frame. */
		error = draw_commit(taken.width, taken.height);
		if (error != 0)
			fail("draw_commit");
		pthread_mutex_lock(&lock);
		request_done = 1;
		pthread_cond_broadcast(&changed);
	}
	pthread_mutex_unlock(&lock);
	return NULL;
}

/* The window's thread: dispatches and takes for a while, or until a resize or the drawing is done. */
static void
pump(
	struct kl_app *app,
	int until_resize,
	int until_drawn,
	unsigned ms)
{
	struct kl_app_event event;
	unsigned resizes_before;
	uint64_t start;
	int status;
	int done;

	resizes_before = resizes;
	start = now_ms();
	while (now_ms() - start < ms) {
		(void)kl_app_dispatch(app, 10);
		while (kl_app_take(app, &event)) {
			if (event.kind != KL_APP_WINDOW || event.window != window)
				continue;
			if (event.input.kind == KL_WINDOW_RESIZE)
				resizes++;
			if (event.input.kind == KL_WINDOW_POST && event.input.code == CODE_ACK) {
				status = kl_window_configure_ack(window, (uint32_t)(uintptr_t)event.input.data);
				pthread_mutex_lock(&lock);
				ack_status = status;
				acked = 1;
				pthread_cond_broadcast(&changed);
				pthread_mutex_unlock(&lock);
			}
		}
		if (until_resize && resizes != resizes_before)
			return;
		if (until_drawn) {
			pthread_mutex_lock(&lock);
			done = request_done;
			pthread_mutex_unlock(&lock);
			if (done)
				return;
		}
	}
	if (until_resize)
		fail("no KL_WINDOW_RESIZE");
	if (until_drawn)
		fail("the frame was not drawn");
}

/* Asks the drawing thread for a frame and serves its acknowledgement until it is committed. */
static void
draw(
	struct kl_app *app,
	uint32_t width,
	uint32_t height,
	uint32_t serial,
	int ack)
{
	pthread_mutex_lock(&lock);
	request.width = width;
	request.height = height;
	request.serial = serial;
	request.ack = ack;
	request_waiting = 1;
	request_done = 0;
	ack_status = -1;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	pump(app, 0, 1, 3000U);
	if (ack && ack_status != 0)
		fail("kl_window_configure_ack from the post");
}

/* Every request sent so far reached the server. */
static void
barrier(void)
{
	struct wl_event_queue *queue;
	int status;

	queue = wl_display_create_queue(display);
	status = wl_display_roundtrip_queue(display, queue);
	if (status < 0)
		fail("roundtrip");
	wl_event_queue_destroy(queue);
}

static void
expect_ack(
	const char *step,
	uint32_t serial)
{
	struct server_state seen;

	barrier();
	server_state(&seen);
	if (seen.last_ack != serial) {
		fprintf(stderr, "FAIL %s: last ack %u, expected %u\n", step, seen.last_ack, serial);
		failures++;
	}
}

static void
expect_take(
	const char *step,
	uint32_t serial,
	uint32_t width,
	uint32_t height,
	struct kl_window_configure *configure)
{
	int status;

	status = kl_window_configure_take(window, configure);
	if (status != 0 || configure->serial != serial || configure->width != width || configure->height != height) {
		fprintf(stderr, "FAIL %s: take %d serial %u %ux%u, expected %u %ux%u\n", step, status,
		    configure->serial, configure->width, configure->height, serial, width, height);
		failures++;
	}
}

int
main(void)
{
	struct kl_window_configure configure;
	struct kl_window_options options;
	struct kl_app_options app_options;
	struct server_state seen;
	struct wl_display *wrapper;
	struct wl_registry *registry;
	struct kl_app *app;
	const char *name;
	uint32_t serial;
	uint32_t later;
	uint32_t previous;
	int status;

	/* The server, the application and its window (first configure 400x300, acknowledged at once). */
	name = server_start(400U, 300U);
	if (name == NULL) {
		fprintf(stderr, "FAIL server\n");
		return 1;
	}
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = name;
	app_options.application = "configure-test";
	app = kl_app_open(&app_options);
	if (app == NULL) {
		fprintf(stderr, "FAIL kl_app_open: %s\n", strerror(errno));
		return 1;
	}
	memset(&options, 0, sizeof(options));
	options.title = "configure-test";
	options.width = 400U;
	options.height = 300U;
	options.present = KL_PRESENT_NONE;
	window = kl_app_window_create(app, &options);
	if (window == NULL) {
		fprintf(stderr, "FAIL kl_app_window_create: %s\n", strerror(errno));
		return 1;
	}

	/* The drawing thread's queue and its wl_shm. */
	display = kl_window_display(window);
	draw_queue = wl_display_create_queue(display);
	wrapper = wl_proxy_create_wrapper(display);
	wl_proxy_set_queue((struct wl_proxy *)wrapper, draw_queue);
	registry = wl_display_get_registry(wrapper);
	wl_proxy_wrapper_destroy(wrapper);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	(void)wl_display_roundtrip_queue(display, draw_queue);
	if (shm == NULL) {
		fprintf(stderr, "FAIL no wl_shm\n");
		return 1;
	}
	pthread_create(&draw_thread, NULL, draw_run, NULL);

	/* The first configure, acknowledged at once; a frame of it. */
	server_state(&seen);
	if (seen.acks != 1U)
		fail("the first configure was not acknowledged");
	draw(app, 400U, 300U, 0U, 0);

	/* The default: acknowledged as it arrives, nothing to take. */
	serial = server_configure(500U, 350U, 0);
	pump(app, 1, 0, 3000U);
	expect_ack("default", serial);
	status = kl_window_configure_take(window, &configure);
	if (status != EAGAIN)
		fail("take without deferral");
	draw(app, 500U, 350U, 0U, 0);

	/* Deferred: a new size waits; an old-size frame meanwhile is fine; the ack comes just before the new size. */
	status = kl_window_set_configure_deferred(window, 1);
	if (status != 0)
		fail("set_configure_deferred");
	later = server_configure(640U, 480U, 0);
	pump(app, 1, 0, 3000U);
	expect_ack("deferred, before the take", serial);
	draw(app, 500U, 350U, 0U, 0);
	expect_take("deferred", later, 640U, 480U, &configure);
	expect_ack("deferred, after the take", serial);
	draw(app, 640U, 480U, configure.serial, 1);
	expect_ack("deferred, after the frame", later);
	status = kl_window_configure_ack(window, configure.serial);
	if (status != EALREADY)
		fail("a second ack is not EALREADY");
	status = kl_window_configure_ack(window, configure.serial + 1000U);
	if (status != EINVAL)
		fail("another serial is not EINVAL");
	status = kl_window_configure_take(window, &configure);
	if (status != EAGAIN)
		fail("take with nothing waiting");

	/* A configure that changes nothing while another waits: acknowledged with it (the latest serial). */
	previous = later;
	serial = server_configure(800U, 600U, 0);
	pump(app, 1, 0, 3000U);
	later = server_configure(800U, 600U, 0);
	pump(app, 0, 0, 200U);
	expect_ack("same size while waiting", previous);
	expect_take("same size while waiting", later, 800U, 600U, &configure);
	draw(app, 800U, 600U, configure.serial, 1);
	expect_ack("same size while waiting, after the frame", later);

	/* A size changed after the take: the taken one alone, then the later one. */
	serial = server_configure(900U, 700U, 0);
	pump(app, 1, 0, 3000U);
	expect_take("size after the take", serial, 900U, 700U, &configure);
	later = server_configure(1000U, 800U, 0);
	pump(app, 1, 0, 3000U);
	draw(app, 900U, 700U, configure.serial, 1);
	expect_ack("size after the take, first", serial);
	expect_take("size after the take, second", later, 1000U, 800U, &configure);
	draw(app, 1000U, 800U, configure.serial, 1);
	expect_ack("size after the take, second", later);

	/* A configure that changes nothing while none waits: acknowledged at once. */
	serial = server_configure(1000U, 800U, 0);
	pump(app, 0, 0, 200U);
	expect_ack("same size, none waiting", serial);

	/* The maximized state changes the layout: it waits too. */
	later = server_configure(1000U, 800U, 1);
	pump(app, 1, 0, 3000U);
	expect_ack("maximized, before the take", serial);
	expect_take("maximized", later, 1000U, 800U, &configure);
	draw(app, 1000U, 800U, configure.serial, 1);
	expect_ack("maximized, after the frame", later);

	/* Deferral turned off with a configure waiting: acknowledged then; later ones at once. */
	serial = server_configure(1100U, 850U, 0);
	pump(app, 1, 0, 3000U);
	expect_ack("turned off, before", later);
	status = kl_window_set_configure_deferred(window, 0);
	if (status != 0)
		fail("set_configure_deferred off");
	expect_ack("turned off", serial);
	draw(app, 1100U, 850U, 0U, 0);

	/* The checker's control: the default acknowledges at once, and an old-size frame after it is caught. */
	serial = server_configure(1200U, 900U, 0);
	pump(app, 1, 0, 3000U);
	expect_ack("control", serial);
	draw(app, 1100U, 850U, 0U, 0);
	barrier();
	server_state(&seen);
	if (seen.violations != 1U)
		fail("the control frame was not caught");
	if (seen.bad_acks != 0U)
		fail("an acknowledgement of a serial not sent or consumed");

	/* The end. */
	pthread_mutex_lock(&lock);
	draw_stop = 1;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	pthread_join(draw_thread, NULL);
	wl_registry_destroy(registry);
	wl_shm_destroy(shm);
	wl_event_queue_destroy(draw_queue);
	kl_window_close(window);
	kl_app_close(app);
	server_stop();
	server_state(&seen);
	if (failures != 0U) {
		fprintf(stderr, "FAIL %u (violations %u: %s)\n", failures, seen.violations, seen.violation);
		return 1;
	}
	printf("PASS acks=%u commits=%u resizes=%u control=\"%s\"\n", seen.acks, seen.commits, resizes, seen.violation);
	return 0;
}
