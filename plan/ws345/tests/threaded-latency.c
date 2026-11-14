/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The acceptance test of a view of its own threads (BROWSER_THREADING_OWN):
 * a window's events never wait for the page.  The window is a stand-in: an
 * input thread writes a time stamp to a pipe every LATENCY_INPUT_MS, and
 * the window's loop takes each one as a window event (it hands the view a
 * pointer move for it, as the shell does), hands the view its size, and
 * carries out browser_view_process.  An event's latency is the time from
 * its stamp to the loop taking it.  A drawing thread draws the latest
 * committed frame (browser_view_render_pixels) whenever the view says there
 * is a new one (frame_wake) or the window's size changed; the window's
 * size changes every second (every 250 ms in resize-big), and a resize's
 * present time is the time from the resize to the end of the first drawing
 * at the new size.
 *
 *   threaded-latency SCENARIO [SECONDS]
 *
 * The scenarios (the page is loaded through a small HTTP server of the
 * test's own on 127.0.0.1; "hang-connect" is a listener whose accept queue
 * is full, so that a connect to it waits for the system's SYN retries;
 * "hang-read" accepts and never answers; a second port of the server is a
 * second origin):
 *
 *   idle              a still page (the floor of the measurement)
 *   loop              a timer's script that spins for 10 s
 *   handler           a mousedown listener that spins for 2 s (one press at 0.5 s,
 *                     or once the listener is there)
 *   sync-xhr-connect  a timer's synchronous XMLHttpRequest to hang-connect,
 *                     until the loader gives up (30 s)
 *   sync-xhr-read     a timer's synchronous XMLHttpRequest to hang-read, as long
 *   docwrite-connect  an inline script writes a <script src> on hang-connect,
 *                     and the parser waits for it until the loader gives up
 *   async-connect     an <img> and a fetch() to hang-connect (the loader's)
 *   resize-big        a page of 6000 boxes resized every 250 ms
 *   decode-big        a 3000 x 3000 PNG image
 *   iframe-loop       an iframe of the second origin whose script loops for
 *                     ever, while the page's own interval goes on
 *
 * Each scenario also has a sign that its page did what it is there for (a
 * console line, or the page's interval in iframe-loop); without it the
 * scenario fails.
 *
 * Prints one line: the scenario, PASS or FAIL, the events sent and taken,
 * the longest latency (or the time the loop has been held when it is still
 * held at the end: a watchdog thread prints then and ends the process), how
 * many events waited longer than 20, 100 and 1000 ms, the resizes, how many
 * of them were presented and the longest present time, and the page's
 * sign.  It passes when the longest latency is at most 20 ms, no event
 * waited longer than 100 ms, every resize but the last was presented within
 * 100 ms, and the sign came.  With NOTIME set in the environment (the
 * ThreadSanitizer variant, whose threads run many times slower) only the
 * sign is judged, and a scenario runs twice its seconds.
 */

#include <browser/browser.h>

#include "userland/tests/browser-threads/threads-posix.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* How often the stand-in window sends an event, in milliseconds. */
#define LATENCY_INPUT_MS	5

/* The size of the view, in pixels (the width alternates with LATENCY_WIDTH + LATENCY_GROW). */
#define LATENCY_WIDTH		800U
#define LATENCY_GROW		200U
#define LATENCY_HEIGHT		600U

/* How often the window's size changes, in milliseconds (resize-big's, and the others'). */
#define LATENCY_RESIZE_BIG_MS	250U
#define LATENCY_RESIZE_MS	1000U

/* The side of the big image, in pixels. */
#define LATENCY_IMAGE_SIDE	3000U

/* The most bytes of a page the test makes. */
#define LATENCY_PAGE_MAX	(1024U * 1024U)

/* The bounds of the acceptance, in nanoseconds. */
#define LATENCY_LONGEST_NS	20000000ULL
#define LATENCY_LONG_NS		100000000ULL
#define LATENCY_PRESENT_NS	100000000ULL

/* The page's interval ticks iframe-loop asks for after the iframe's loop started. */
#define LATENCY_TICKS_MIN	10ULL

/* What the loop measured, shared with the watchdog. */
struct latency_stats {
	atomic_ullong sent;
	atomic_ullong taken;
	atomic_ullong over20;
	atomic_ullong over100;
	atomic_ullong over1000;
	atomic_ullong longest_ns;
	atomic_ullong last_taken_ns;
	atomic_ullong signs;
	atomic_ullong ticks;
	atomic_int armed;
	atomic_int done;
};

/* One resource of the server. */
struct latency_resource {
	const char *path;
	const char *type;
	unsigned char *body;
	size_t length;
};

/* One scenario: its name, its seconds and the console line that is its sign. */
struct latency_scenario {
	const char *name;
	double seconds;
	const char *sign;
};

static const struct latency_scenario scenarios[] = {
	{ "idle", 4.0, "ready" },
	{ "loop", 12.0, "spun" },
	{ "handler", 4.0, "handled" },
	{ "sync-xhr-connect", 36.0, "xhr ended" },
	{ "sync-xhr-read", 36.0, "xhr ended" },
	{ "docwrite-connect", 36.0, "after" },
	{ "async-connect", 6.0, "ready" },
	{ "resize-big", 8.0, "ready" },
	{ "decode-big", 8.0, "decoded" },
	{ "iframe-loop", 8.0, "kid start" },
};

static struct latency_stats stats;
static struct latency_resource resources[6];
static size_t resource_count;
static const struct latency_scenario *scenario;
static double seconds;
static int notime;
static int input_pipe[2];
static int wake_pipe[2];
static int redraw_due;
static unsigned short port_serve;
static unsigned short port_other;
static unsigned short port_hang_connect;
static unsigned short port_hang_read;
static uint64_t start_ns;
static pthread_mutex_t draw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t draw_cond = PTHREAD_COND_INITIALIZER;
static int draw_owed;
static int draw_stop;
static unsigned draw_width;
static struct browser_view *draw_view;
static uint64_t resize_ns;
static unsigned resize_width;
static unsigned long long resizes;
static unsigned long long presented;
static unsigned long long missed;
static uint64_t present_longest_ns;

static uint64_t latency_now(void);
static int latency_listen(int backlog, unsigned short *port);
static void *latency_input(void *context);
static void *latency_watchdog(void *context);
static void *latency_serve(void *context);
static void *latency_hold(void *context);
static void latency_answer(int client);
static void *latency_answer_thread(void *context);
static void latency_add(const char *path, const char *type, unsigned char *body, size_t length);
static int latency_page(char *page, size_t size);
static unsigned char *latency_png(unsigned side, size_t *length);
static int latency_summary(const char *how, uint64_t held_ns);
static void latency_resized(unsigned width);
static void latency_wake(void *context);
static void latency_frame_wake(void *context);
static void *latency_draw(void *context);
static void latency_redraw(void *context, struct browser_view *view);
static void latency_console(void *context, struct browser_view *view, int level, const char *text, size_t length);

int
main(
	int argc,
	char **argv)
{
	static const struct browser_fonts fonts = {
		"userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf", "userland/desktop/fonts/Mahora-Bold.ttf",
		"userland/desktop/fonts/Mahora-Mono.ttf"
	};
	struct browser_callbacks callbacks;
	struct browser_view_options options;
	struct browser_view *view;
	struct pollfd descriptors[2];
	pthread_t thread;
	pthread_t draw_thread;
	uint64_t stamp;
	uint64_t now;
	uint64_t latency;
	uint64_t longest;
	uint64_t next_resize;
	uint64_t resize_period;
	char *page;
	char *kid;
	char url[128];
	unsigned char *png;
	unsigned width;
	size_t png_length;
	size_t index;
	ssize_t got;
	char drain[64];
	int listener;
	int pressed;
	int hold;
	int timeout;
	int error;
	int filler_index;

	if (argc < 2) {
		fprintf(stderr, "usage: threaded-latency SCENARIO [SECONDS]\n");
		return 2;
	}

	/* The scenario, its seconds and whether the times are judged. */
	scenario = NULL;
	for (index = 0; index < sizeof(scenarios) / sizeof(scenarios[0]); index++) {
		if (strcmp(scenarios[index].name, argv[1]) == 0)
			scenario = &scenarios[index];
	}
	if (scenario == NULL) {
		fprintf(stderr, "threaded-latency: unknown scenario %s\n", argv[1]);
		return 2;
	}
	notime = getenv("NOTIME") != NULL && getenv("NOTIME")[0] != '\0';
	seconds = scenario->seconds;
	if (notime)
		seconds *= 2.0;
	if (argc > 2)
		seconds = atof(argv[2]);
	setenv("XDG_DATA_HOME", "build/ws345-latency-data", 1);

	/* The engine makes its threads with the POSIX threads. */
	error = browser_threads_set(browser_threads_posix());
	if (error != 0) {
		fprintf(stderr, "threaded-latency: the engine refused the threads (%d)\n", error);
		return 2;
	}

	/* The listener that never accepts: its queue filled, so that a later SYN is dropped. */
	hold = latency_listen(0, &port_hang_connect);
	if (hold < 0)
		return 2;
	for (filler_index = 0; filler_index < 4; filler_index++) {
		struct sockaddr_in address;
		int filler;

		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port_hang_connect);
		filler = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		(void)connect(filler, (struct sockaddr *)&address, sizeof(address));
	}

	/* The listener that accepts and never answers. */
	listener = latency_listen(64, &port_hang_read);
	if (listener < 0)
		return 2;
	pthread_create(&thread, NULL, latency_hold, (void *)(long)listener);
	pthread_detach(thread);

	/* The pages. */
	listener = latency_listen(64, &port_serve);
	if (listener < 0)
		return 2;
	hold = latency_listen(64, &port_other);
	if (hold < 0)
		return 2;
	page = malloc(LATENCY_PAGE_MAX);
	if (page == NULL || latency_page(page, LATENCY_PAGE_MAX) != 0)
		return 2;
	latency_add("/first.html", "text/html", (unsigned char *)"<!DOCTYPE html><title>first</title><p>first", 43U);
	latency_add("/page.html", "text/html", (unsigned char *)page, strlen(page));
	if (strcmp(scenario->name, "decode-big") == 0) {
		png = latency_png(LATENCY_IMAGE_SIDE, &png_length);
		if (png == NULL)
			return 2;
		latency_add("/big.png", "image/png", png, png_length);
	}
	if (strcmp(scenario->name, "iframe-loop") == 0) {
		kid = malloc(512U);
		if (kid == NULL)
			return 2;
		snprintf(kid, 512U, "<!DOCTYPE html><title>kid</title><p>kid"
		    "<script>setTimeout(function () { parent.postMessage('kid start', 'http://127.0.0.1:%u'); for (;;) {} }, 200);</script>",
		    (unsigned)port_serve);
		latency_add("/kid.html", "text/html", (unsigned char *)kid, strlen(kid));
	}

	/* The server, on its two ports (two origins). */
	pthread_create(&thread, NULL, latency_serve, (void *)(long)listener);
	pthread_detach(thread);
	pthread_create(&thread, NULL, latency_serve, (void *)(long)hold);
	pthread_detach(thread);

	/* The window's stand-in and the view's wake: two pipes the loop polls. */
	if (pipe(input_pipe) != 0 || pipe(wake_pipe) != 0)
		return 2;
	fcntl(input_pipe[1], F_SETFL, O_NONBLOCK);
	fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);

	/* The view on its own threads, as the shell makes it. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.redraw = latency_redraw;
	callbacks.console = latency_console;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.width = LATENCY_WIDTH;
	options.height = LATENCY_HEIGHT;
	options.wake = latency_wake;
	options.threading = BROWSER_THREADING_OWN;
	options.frame_wake = latency_frame_wake;
	error = browser_view_create(&options, &view);
	if (error != 0) {
		fprintf(stderr, "threaded-latency: no view (%d)\n", error);
		return 2;
	}

	/* The first page, then the scenario's page. */
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/first.html", (unsigned)port_serve);
	error = browser_view_load(view, url);
	if (error != 0) {
		fprintf(stderr, "threaded-latency: the first page did not load (%d)\n", error);
		return 2;
	}
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/page.html", (unsigned)port_serve);
	error = browser_view_load(view, url);
	if (error != 0) {
		fprintf(stderr, "threaded-latency: the page did not load (%d)\n", error);
		return 2;
	}

	/* The window's events from now on, the watchdog and the drawing thread. */
	start_ns = latency_now();
	atomic_store(&stats.last_taken_ns, start_ns);
	pthread_create(&thread, NULL, latency_input, NULL);
	pthread_detach(thread);
	pthread_create(&thread, NULL, latency_watchdog, NULL);
	pthread_detach(thread);
	draw_view = view;
	draw_width = LATENCY_WIDTH;
	pthread_create(&draw_thread, NULL, latency_draw, NULL);

	/* The window's loop. */
	pressed = 0;
	width = LATENCY_WIDTH;
	resize_period = strcmp(scenario->name, "resize-big") == 0 ? LATENCY_RESIZE_BIG_MS : LATENCY_RESIZE_MS;
	resize_period *= 1000000ULL;
	next_resize = start_ns + 1000000000ULL;
	while (latency_now() - start_ns < (uint64_t)(seconds * 1e9)) {
		/* The drawing thread draws when the view asked for it. */
		if (redraw_due) {
			redraw_due = 0;
			pthread_mutex_lock(&draw_lock);
			draw_owed = 1;
			pthread_cond_signal(&draw_cond);
			pthread_mutex_unlock(&draw_lock);
		}

		/* The window's size changes. */
		if (latency_now() >= next_resize) {
			next_resize = latency_now() + resize_period;
			width = width == LATENCY_WIDTH ? LATENCY_WIDTH + LATENCY_GROW : LATENCY_WIDTH;
			(void)browser_view_resize(view, width, LATENCY_HEIGHT);
			latency_resized(width);
		}

		/* Waits for the window or the wake. */
		timeout = 20;
		descriptors[0].fd = input_pipe[0];
		descriptors[0].events = POLLIN;
		descriptors[1].fd = wake_pipe[0];
		descriptors[1].events = POLLIN;
		(void)poll(descriptors, 2, timeout);
		if (descriptors[1].revents & POLLIN)
			(void)read(wake_pipe[0], drain, sizeof(drain));

		/* The window's events: each one's latency, and a pointer move to the view. */
		while ((descriptors[0].revents & POLLIN) != 0) {
			got = read(input_pipe[0], &stamp, sizeof(stamp));
			if (got != (ssize_t)sizeof(stamp))
				break;
			now = latency_now();
			latency = now - stamp;
			longest = atomic_load(&stats.longest_ns);
			if (latency > longest)
				atomic_store(&stats.longest_ns, latency);
			if (latency > 20000000ULL)
				atomic_fetch_add(&stats.over20, 1);
			if (latency > 100000000ULL)
				atomic_fetch_add(&stats.over100, 1);
			if (latency > 1000000000ULL)
				atomic_fetch_add(&stats.over1000, 1);
			atomic_fetch_add(&stats.taken, 1);
			atomic_store(&stats.last_taken_ns, now);
			(void)browser_view_pointer_move(view, 20.0f + (float)(stamp % 7U), 10.0f, 0);
			if (!pressed && strcmp(scenario->name, "handler") == 0 && atomic_load(&stats.armed) && now - start_ns > 500000000ULL) {
				pressed = 1;
				(void)browser_view_pointer_button(view, 20.0f, 10.0f, BROWSER_BUTTON_PRIMARY, 1, 0);
				(void)browser_view_pointer_button(view, 20.0f, 10.0f, BROWSER_BUTTON_PRIMARY, 0, 0);
			}
			descriptors[0].revents = 0;
			(void)poll(descriptors, 1, 0);
		}

		/* The view's published state and callbacks. */
		browser_view_process(view);
	}

	/* The loop was never held past the end. */
	atomic_store(&stats.done, 1);
	error = latency_summary("ended", 0);
	fflush(stdout);
	_exit(error);
}

/* The window's size changed: the drawing thread owes a drawing at it, timed from now. */
static void
latency_resized(
	unsigned width)
{
	uint64_t now;
	uint64_t waited;

	now = latency_now();
	pthread_mutex_lock(&draw_lock);

	/* A resize the drawing thread has not presented yet waited at least until now. */
	if (resize_ns != 0) {
		waited = now - resize_ns;
		if (waited > present_longest_ns)
			present_longest_ns = waited;
		missed++;
	}
	resize_ns = now;
	resize_width = width;
	resizes++;
	draw_width = width;
	draw_owed = 1;
	pthread_cond_signal(&draw_cond);
	pthread_mutex_unlock(&draw_lock);
}

/* The monotonic clock, in nanoseconds. */
static uint64_t
latency_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* A listener on 127.0.0.1, on a port of the system's choice. */
static int
latency_listen(
	int backlog,
	unsigned short *port)
{
	struct sockaddr_in address;
	socklen_t size;
	int listener;
	int one;

	listener = socket(AF_INET, SOCK_STREAM, 0);
	one = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, backlog) != 0) {
		fprintf(stderr, "threaded-latency: no listener\n");
		return -1;
	}
	size = sizeof(address);
	getsockname(listener, (struct sockaddr *)&address, &size);
	*port = ntohs(address.sin_port);
	return listener;
}

/* The window's stand-in: a stamp every LATENCY_INPUT_MS (one the pipe cannot take is still counted as sent and lost). */
static void *
latency_input(
	void *context)
{
	uint64_t stamp;

	(void)context;
	while (!atomic_load(&stats.done)) {
		stamp = latency_now();
		(void)write(input_pipe[1], &stamp, sizeof(stamp));
		atomic_fetch_add(&stats.sent, 1);
		usleep(LATENCY_INPUT_MS * 1000);
	}
	return NULL;
}

/* Ends the run when the loop is still held past its end, with how long it has been held. */
static void *
latency_watchdog(
	void *context)
{
	uint64_t end;
	uint64_t held;

	(void)context;
	end = start_ns + (uint64_t)(seconds * 1e9) + 1000000000ULL;
	while (latency_now() < end)
		usleep(20000);
	if (atomic_load(&stats.done))
		return NULL;
	held = latency_now() - atomic_load(&stats.last_taken_ns);
	(void)latency_summary("held", held);
	fflush(stdout);
	_exit(1);
}

/* Prints the summary line and judges it; reports the exit status (0 when it passed, 1 when not). */
static int
latency_summary(
	const char *how,
	uint64_t held_ns)
{
	unsigned long long sign;
	unsigned long long ticks;
	unsigned long long resized;
	unsigned long long shown;
	unsigned long long late;
	uint64_t longest;
	uint64_t present;
	int passed;

	longest = atomic_load(&stats.longest_ns);
	if (held_ns > longest)
		longest = held_ns;
	pthread_mutex_lock(&draw_lock);
	resized = resizes;
	shown = presented;
	late = missed;
	present = present_longest_ns;
	pthread_mutex_unlock(&draw_lock);
	sign = atomic_load(&stats.signs);
	ticks = atomic_load(&stats.ticks);

	/* The page's sign: its console line, and in iframe-loop the page's interval after the iframe's loop started. */
	passed = sign != 0;
	if (strcmp(scenario->name, "iframe-loop") == 0 && ticks < LATENCY_TICKS_MIN)
		passed = 0;

	/* The times, unless they are not judged. */
	if (!notime) {
		if (held_ns != 0)
			passed = 0;
		if (longest > LATENCY_LONGEST_NS)
			passed = 0;
		if (atomic_load(&stats.over100) != 0)
			passed = 0;
		if (late != 0 || present > LATENCY_PRESENT_NS || shown == 0)
			passed = 0;
	}

	printf("threaded-latency %s: %s %s sent=%llu taken=%llu longest_ms=%.1f over20=%llu over100=%llu over1000=%llu"
	    " resizes=%llu presented=%llu late=%llu present_longest_ms=%.1f sign=%llu ticks=%llu%s%s\n",
	    scenario->name, passed ? "PASS" : "FAIL", how,
	    (unsigned long long)atomic_load(&stats.sent), (unsigned long long)atomic_load(&stats.taken), (double)longest / 1e6,
	    (unsigned long long)atomic_load(&stats.over20), (unsigned long long)atomic_load(&stats.over100),
	    (unsigned long long)atomic_load(&stats.over1000), resized, shown, late, (double)present / 1e6, sign, ticks,
	    notime ? " (times not judged)" : "", held_ns != 0 ? " (the loop is still held at the end)" : "");
	return passed ? 0 : 1;
}

/* The server: each connection is answered by a thread of its own. */
static void *
latency_serve(
	void *context)
{
	pthread_t thread;
	int listener;
	int client;

	listener = (int)(long)context;
	for (;;) {
		client = accept(listener, NULL, NULL);
		if (client < 0)
			continue;
		pthread_create(&thread, NULL, latency_answer_thread, (void *)(long)client);
		pthread_detach(thread);
	}
	return NULL;
}

/* A connection's thread. */
static void *
latency_answer_thread(
	void *context)
{
	latency_answer((int)(long)context);
	return NULL;
}

/* hang-read: accepts and keeps every connection without a word. */
static void *
latency_hold(
	void *context)
{
	int listener;

	listener = (int)(long)context;
	for (;;)
		(void)accept(listener, NULL, NULL);
	return NULL;
}

/* Answers one request with its resource, or 404. */
static void
latency_answer(
	int client)
{
	const struct latency_resource *found;
	char request[2048];
	char head[256];
	ssize_t got;
	size_t length;
	size_t index;
	size_t path;
	int head_length;

	length = 0;
	while (length < sizeof(request) - 1U) {
		got = recv(client, request + length, sizeof(request) - 1U - length, 0);
		if (got <= 0)
			break;
		length += (size_t)got;
		request[length] = '\0';
		if (strstr(request, "\r\n\r\n") != NULL)
			break;
	}
	request[length] = '\0';
	found = NULL;
	for (index = 0; index < resource_count; index++) {
		path = strlen(resources[index].path);
		if (strncmp(request + 4, resources[index].path, path) == 0 && request[4 + path] == ' ')
			found = &resources[index];
	}
	if (found == NULL) {
		head_length = snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		(void)send(client, head, (size_t)head_length, MSG_NOSIGNAL);
	} else {
		head_length = snprintf(head, sizeof(head),
		    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
		    found->type, found->length);
		(void)send(client, head, (size_t)head_length, MSG_NOSIGNAL);
		(void)send(client, found->body, found->length, MSG_NOSIGNAL);
	}
	close(client);
}

/* Adds a resource to the server. */
static void
latency_add(
	const char *path,
	const char *type,
	unsigned char *body,
	size_t length)
{
	resources[resource_count].path = path;
	resources[resource_count].type = type;
	resources[resource_count].body = body;
	resources[resource_count].length = length;
	resource_count++;
}

/* Writes the scenario's page; nonzero when it does not fit. */
static int
latency_page(
	char *page,
	size_t size)
{
	const char *name;
	size_t used;
	unsigned index;

	name = scenario->name;
	if (strcmp(name, "idle") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>idle</title><p>idle<script>console.log('ready');</script>");
	} else if (strcmp(name, "loop") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>loop</title><p>loop"
		    "<script>setTimeout(function () { var e = Date.now() + 10000; while (Date.now() < e) {} console.log('spun'); }, 300);</script>");
	} else if (strcmp(name, "handler") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>handler</title><body style='margin:0;height:2000px'><p>handler"
		    "<script>var once = 0; document.addEventListener('mousedown', function () { if (once++) return; var e = Date.now() + 2000;"
		    " while (Date.now() < e) {} console.log('handled'); }); console.log('armed');</script>");
	} else if (strcmp(name, "sync-xhr-connect") == 0 || strcmp(name, "sync-xhr-read") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>xhr</title><p>xhr"
		    "<script>setTimeout(function () { var x = new XMLHttpRequest(); x.open('GET', 'http://127.0.0.1:%u/x', false);"
		    " try { x.send(); } catch (e) {} console.log('xhr ended'); }, 300);</script>",
		    (unsigned)(strcmp(name, "sync-xhr-connect") == 0 ? port_hang_connect : port_hang_read));
	} else if (strcmp(name, "docwrite-connect") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>write</title><p>write"
		    "<script>document.write('<script src=\"http://127.0.0.1:%u/x.js\"><\\/script>');</script><p>after"
		    "<script>console.log('after');</script>",
		    (unsigned)port_hang_connect);
	} else if (strcmp(name, "async-connect") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>async</title><p>async<img src=\"http://127.0.0.1:%u/x.png\">"
		    "<script>fetch('http://127.0.0.1:%u/y').catch(function () {}); console.log('ready');</script>",
		    (unsigned)port_hang_connect, (unsigned)port_hang_connect);
	} else if (strcmp(name, "resize-big") == 0) {
		used = (size_t)snprintf(page, size, "<!DOCTYPE html><title>big</title><style>div{display:flex;flex-wrap:wrap}span{padding:2px;border:1px solid}</style>");
		for (index = 0; index < 6000U && used + 64U < size; index++)
			used += (size_t)snprintf(page + used, size - used, "<div><span>box %u</span><span>text</span></div>", index);
		if (used + 64U >= size)
			return 1;
		snprintf(page + used, size - used, "<script>console.log('ready');</script>");
	} else if (strcmp(name, "decode-big") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>image</title><p>image<img src=\"/big.png\" onload=\"console.log('decoded')\">");
	} else {
		snprintf(page, size, "<!DOCTYPE html><title>frames</title><p>frames"
		    "<script>var started = 0; addEventListener('message', function (e) { if (e.data === 'kid start') { started = 1; console.log('kid start'); } });"
		    " setInterval(function () { if (started) console.log('tick'); }, 100);</script>"
		    "<iframe src=\"http://127.0.0.1:%u/kid.html\"></iframe>",
		    (unsigned)port_other);
	}
	return 0;
}

/* The CRC-32 of PNG chunks (ISO 3309), over a running value. */
static uint32_t
latency_crc(
	uint32_t crc,
	const unsigned char *bytes,
	size_t length)
{
	size_t index;
	int bit;

	crc = ~crc;
	for (index = 0; index < length; index++) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
	}
	return ~crc;
}

/* Writes a big-endian word. */
static void
latency_put32(
	unsigned char *at,
	uint32_t value)
{
	at[0] = (unsigned char)(value >> 24);
	at[1] = (unsigned char)(value >> 16);
	at[2] = (unsigned char)(value >> 8);
	at[3] = (unsigned char)value;
}

/* A side x side RGB PNG of a gradient, its image data in stored (uncompressed) deflate blocks. */
static unsigned char *
latency_png(
	unsigned side,
	size_t *length)
{
	static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	unsigned char *raw;
	unsigned char *png;
	size_t raw_length;
	size_t blocks;
	size_t data_length;
	size_t at;
	size_t done;
	size_t piece;
	uint32_t a;
	uint32_t b;
	unsigned x;
	unsigned y;

	raw_length = (size_t)side * (1U + (size_t)side * 3U);
	raw = malloc(raw_length);
	if (raw == NULL)
		return NULL;
	at = 0;
	for (y = 0; y < side; y++) {
		raw[at++] = 0;
		for (x = 0; x < side; x++) {
			raw[at++] = (unsigned char)x;
			raw[at++] = (unsigned char)y;
			raw[at++] = (unsigned char)(x ^ y);
		}
	}
	blocks = (raw_length + 65534U) / 65535U;
	data_length = 2U + raw_length + blocks * 5U + 4U;
	png = malloc(8U + 25U + 12U + data_length + 12U);
	if (png == NULL)
		return NULL;
	memcpy(png, signature, 8U);
	at = 8;
	latency_put32(png + at, 13U);
	memcpy(png + at + 4, "IHDR", 4U);
	latency_put32(png + at + 8, side);
	latency_put32(png + at + 12, side);
	png[at + 16] = 8;
	png[at + 17] = 2;
	png[at + 18] = 0;
	png[at + 19] = 0;
	png[at + 20] = 0;
	latency_put32(png + at + 21, latency_crc(0, png + at + 4, 17U));
	at += 25;
	latency_put32(png + at, (uint32_t)data_length);
	memcpy(png + at + 4, "IDAT", 4U);
	at += 8;
	png[at++] = 0x78;
	png[at++] = 0x01;
	a = 1;
	b = 0;
	for (done = 0; done < raw_length; done += piece) {
		piece = raw_length - done;
		if (piece > 65535U)
			piece = 65535U;
		png[at++] = done + piece == raw_length ? 1 : 0;
		png[at++] = (unsigned char)piece;
		png[at++] = (unsigned char)(piece >> 8);
		png[at++] = (unsigned char)~piece;
		png[at++] = (unsigned char)(~piece >> 8);
		memcpy(png + at, raw + done, piece);
		at += piece;
	}
	for (done = 0; done < raw_length; done++) {
		a = (a + raw[done]) % 65521U;
		b = (b + a) % 65521U;
	}
	latency_put32(png + at, (b << 16) | a);
	at += 4;
	latency_put32(png + at, latency_crc(0, png + 33 + 4, 4U + data_length));
	at += 4;
	latency_put32(png + at, 0);
	memcpy(png + at + 4, "IEND", 4U);
	latency_put32(png + at + 8, latency_crc(0, png + at + 4, 4U));
	at += 12;
	free(raw);
	*length = at;
	return png;
}

/* The view's wake, from the library's thread: a byte in the wake pipe. */
static void
latency_wake(
	void *context)
{
	char byte;

	(void)context;
	byte = 1;
	(void)write(wake_pipe[1], &byte, 1U);
}

/* A new committed frame (from the view's thread): the drawing thread draws it. */
static void
latency_frame_wake(
	void *context)
{
	(void)context;
	pthread_mutex_lock(&draw_lock);
	draw_owed = 1;
	pthread_cond_signal(&draw_cond);
	pthread_mutex_unlock(&draw_lock);
}

/* The drawing thread: the latest committed frame at the window's width, whenever one is owed; a resize's present timed. */
static void *
latency_draw(
	void *context)
{
	uint32_t *pixels;
	uint64_t waited;
	unsigned width;
	int error;

	(void)context;
	pixels = malloc((size_t)(LATENCY_WIDTH + LATENCY_GROW) * LATENCY_HEIGHT * 4U);
	if (pixels == NULL)
		return NULL;
	pthread_mutex_lock(&draw_lock);
	while (!draw_stop) {
		while (!draw_owed && !draw_stop)
			pthread_cond_wait(&draw_cond, &draw_lock);
		draw_owed = 0;
		width = draw_width;
		pthread_mutex_unlock(&draw_lock);
		error = browser_view_render_pixels(draw_view, pixels, width, LATENCY_HEIGHT, (size_t)width * 4U);
		pthread_mutex_lock(&draw_lock);

		/* A drawing at the size of the resize that waits presents it. */
		if (error == 0 && resize_ns != 0 && width == resize_width) {
			waited = latency_now() - resize_ns;
			if (waited > present_longest_ns)
				present_longest_ns = waited;
			presented++;
			resize_ns = 0;
		}
	}
	pthread_mutex_unlock(&draw_lock);
	free(pixels);
	return NULL;
}

/* The view asks for a redraw. */
static void
latency_redraw(
	void *context,
	struct browser_view *view)
{
	(void)context;
	(void)view;
	redraw_due = 1;
}

/* The page's console: its sign and ticks counted, and the line on the error output. */
static void
latency_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	(void)context;
	(void)view;
	(void)level;
	if (length == strlen(scenario->sign) && memcmp(text, scenario->sign, length) == 0)
		atomic_fetch_add(&stats.signs, 1);
	if (length == 4U && memcmp(text, "tick", 4U) == 0)
		atomic_fetch_add(&stats.ticks, 1);
	if (length == 5U && memcmp(text, "armed", 5U) == 0)
		atomic_store(&stats.armed, 1);
	fprintf(stderr, "console: %.*s (%.0f ms)\n", (int)length, text, (double)(latency_now() - start_ns) / 1e6);
}
