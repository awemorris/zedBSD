/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A measurement probe: how long a window's events wait while the view
 * works, with the window's loop as the browser's shell runs it today (one
 * thread: wait for the window or the view's wake, carry out the inputs,
 * browser_view_process, then lay out and draw when the view asked for a
 * redraw).  The window is a stand-in: an input thread writes a time stamp
 * to a pipe every PROBE_INPUT_MS, and the loop takes each one as a window
 * event (it hands the view a pointer move for it, as the shell does).  An
 * event's latency is the time from its stamp to the loop taking it.
 *
 *   stall-probe SCENARIO [SECONDS]
 *
 * The scenarios (the page is loaded through a small HTTP server of the
 * probe's own on 127.0.0.1; "hang-connect" is a listener whose accept
 * queue is full, so that a connect to it waits for the system's SYN
 * retries; "hang-read" accepts and never answers):
 *
 *   idle              a still page (the floor of the measurement)
 *   loop              a timer's script that spins for 3 s
 *   handler           a mousedown listener that spins for 1 s (one press at 0.5 s;
 *                     the view hands the page no mousemove)
 *   sync-xhr-connect  a timer's synchronous XMLHttpRequest to hang-connect
 *   sync-xhr-read     a timer's synchronous XMLHttpRequest to hang-read
 *   docwrite-connect  an inline script writes a <script src> on hang-connect
 *   async-connect     an <img> and a fetch() to hang-connect (the loader's)
 *   resize-big        a page of 6000 boxes resized every 250 ms
 *   decode-big        a 3000 x 3000 PNG image
 *
 * Prints one summary line: the events sent and taken, the longest
 * latency (or the time the loop has been held when it is still held at
 * the end: a watchdog thread prints then and ends the process), and how
 * many events waited longer than 50, 100 and 1000 ms.
 *
 * A probe of plan/ws345 (the design's measurements); not a test.
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
#define PROBE_INPUT_MS		5

/* The size of the view, in pixels. */
#define PROBE_WIDTH		800U
#define PROBE_HEIGHT		600U

/* The side of the big image, in pixels. */
#define PROBE_IMAGE_SIDE	3000U

/* The most bytes of a page the probe makes. */
#define PROBE_PAGE_MAX		(1024U * 1024U)

/* What the loop measured, shared with the watchdog. */
struct probe_stats {
	atomic_ullong sent;
	atomic_ullong taken;
	atomic_ullong over50;
	atomic_ullong over100;
	atomic_ullong over1000;
	atomic_ullong longest_ns;
	atomic_ullong last_taken_ns;
	atomic_int done;
};

/* One resource of the server. */
struct probe_resource {
	const char *path;
	const char *type;
	unsigned char *body;
	size_t length;
};

static struct probe_stats stats;
static struct probe_resource resources[4];
static size_t resource_count;
static const char *scenario;
static double seconds;
static int input_pipe[2];
static int wake_pipe[2];
static int redraw_due;
static unsigned short port_serve;
static unsigned short port_hang_connect;
static unsigned short port_hang_read;
static uint64_t start_ns;

static uint64_t probe_now(void);
static int probe_listen(int backlog, unsigned short *port);
static void *probe_input(void *context);
static void *probe_watchdog(void *context);
static void *probe_serve(void *context);
static void *probe_hold(void *context);
static void probe_answer(int client);
static void *probe_answer_thread(void *context);
static void probe_add(const char *path, const char *type, unsigned char *body, size_t length);
static int probe_page(char *page, size_t size);
static unsigned char *probe_png(unsigned side, size_t *length);
static void probe_summary(const char *how, uint64_t held_ns);
static void probe_wake(void *context);
static void probe_redraw(void *context, struct browser_view *view);
static void probe_console(void *context, struct browser_view *view, int level, const char *text, size_t length);

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
	uint64_t stamp;
	uint64_t now;
	uint64_t latency;
	uint64_t longest;
	uint64_t next_resize;
	uint32_t *pixels;
	char *page;
	char url[128];
	unsigned char *png;
	unsigned width;
	size_t png_length;
	ssize_t got;
	char drain[64];
	int listener;
	int pressed;
	int hold;
	int timeout;
	int error;
	int index;

	if (argc < 2) {
		fprintf(stderr, "usage: stall-probe SCENARIO [SECONDS]\n");
		return 2;
	}
	scenario = argv[1];
	seconds = argc > 2 ? atof(argv[2]) : 6.0;
	setenv("XDG_DATA_HOME", "build/ws345-probe-data", 1);

	/* The engine makes its threads with the POSIX threads. */
	error = browser_threads_set(browser_threads_posix());
	if (error != 0) {
		fprintf(stderr, "stall-probe: the engine refused the threads (%d)\n", error);
		return 2;
	}

	/* The server, and the two listeners that hold a connection. */
	listener = probe_listen(64, &port_serve);
	hold = probe_listen(0, &port_hang_connect);
	if (listener < 0 || hold < 0)
		return 2;
	for (index = 0; index < 4; index++) {
		struct sockaddr_in address;
		int filler;

		/* Connections nobody accepts fill the queue, so a later SYN is dropped. */
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port_hang_connect);
		filler = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		(void)connect(filler, (struct sockaddr *)&address, sizeof(address));
	}
	listener = probe_listen(64, &port_hang_read);
	if (listener < 0)
		return 2;
	pthread_create(&thread, NULL, probe_hold, (void *)(long)listener);
	listener = probe_listen(64, &port_serve);
	if (listener < 0)
		return 2;

	/* The pages. */
	page = malloc(PROBE_PAGE_MAX);
	if (page == NULL || probe_page(page, PROBE_PAGE_MAX) != 0) {
		fprintf(stderr, "stall-probe: unknown scenario %s\n", scenario);
		return 2;
	}
	probe_add("/first.html", "text/html", (unsigned char *)"<!DOCTYPE html><title>first</title><p>first", 43U);
	probe_add("/page.html", "text/html", (unsigned char *)page, strlen(page));
	if (strcmp(scenario, "decode-big") == 0) {
		png = probe_png(PROBE_IMAGE_SIDE, &png_length);
		if (png == NULL)
			return 2;
		probe_add("/big.png", "image/png", png, png_length);
	}
	pthread_create(&thread, NULL, probe_serve, (void *)(long)listener);

	/* The window's stand-in and the view's wake: two pipes the loop polls. */
	if (pipe(input_pipe) != 0 || pipe(wake_pipe) != 0)
		return 2;
	fcntl(input_pipe[1], F_SETFL, O_NONBLOCK);
	fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);

	/* The view, as the shell makes it. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.redraw = probe_redraw;
	callbacks.console = probe_console;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.width = PROBE_WIDTH;
	options.height = PROBE_HEIGHT;
	options.wake = probe_wake;
	error = browser_view_create(&options, &view);
	if (error != 0) {
		fprintf(stderr, "stall-probe: no view (%d)\n", error);
		return 2;
	}
	pixels = calloc((size_t)PROBE_WIDTH * PROBE_HEIGHT * 2U, sizeof(*pixels));
	if (pixels == NULL)
		return 2;

	/* The first page (read at once, as the shell's start page is), then the scenario's page through the loader. */
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/first.html", (unsigned)port_serve);
	error = browser_view_load(view, url);
	if (error != 0) {
		fprintf(stderr, "stall-probe: the first page did not load (%d)\n", error);
		return 2;
	}
	(void)browser_view_prepare(view);
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/page.html", (unsigned)port_serve);
	error = browser_view_load(view, url);
	if (error != 0) {
		fprintf(stderr, "stall-probe: the page did not load (%d)\n", error);
		return 2;
	}

	/* The window's events from now on, and the watchdog. */
	start_ns = probe_now();
	atomic_store(&stats.last_taken_ns, start_ns);
	pthread_create(&thread, NULL, probe_input, NULL);
	pthread_create(&thread, NULL, probe_watchdog, NULL);

	/* The shell's loop. */
	pressed = 0;
	width = PROBE_WIDTH;
	next_resize = start_ns + 1000000000ULL;
	while (probe_now() - start_ns < (uint64_t)(seconds * 1e9)) {
		/* Draws when the view asked for it. */
		if (redraw_due) {
			redraw_due = 0;
			(void)browser_view_prepare(view);
			(void)browser_view_draw_pixels(view, pixels, width, PROBE_HEIGHT, (size_t)width * 4U);
		}

		/* The window's resize, every 250 ms in its scenario. */
		if (strcmp(scenario, "resize-big") == 0 && probe_now() >= next_resize) {
			next_resize = probe_now() + 250000000ULL;
			width = width == PROBE_WIDTH ? PROBE_WIDTH + 200U : PROBE_WIDTH;
			(void)browser_view_resize(view, width, PROBE_HEIGHT);
			redraw_due = 1;
		}

		/* Waits for the window, the wake or the view's timer. */
		timeout = browser_view_timeout(view);
		if (timeout < 0 || timeout > 50)
			timeout = 50;
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
			now = probe_now();
			latency = now - stamp;
			longest = atomic_load(&stats.longest_ns);
			if (latency > longest)
				atomic_store(&stats.longest_ns, latency);
			if (latency > 50000000ULL)
				atomic_fetch_add(&stats.over50, 1);
			if (latency > 100000000ULL)
				atomic_fetch_add(&stats.over100, 1);
			if (latency > 1000000000ULL)
				atomic_fetch_add(&stats.over1000, 1);
			atomic_fetch_add(&stats.taken, 1);
			atomic_store(&stats.last_taken_ns, now);
			(void)browser_view_pointer_move(view, 20.0f + (float)(stamp % 7U), 10.0f, 0);
			if (!pressed && strcmp(scenario, "handler") == 0 && now - start_ns > 500000000ULL) {
				pressed = 1;
				(void)browser_view_pointer_button(view, 20.0f, 10.0f, BROWSER_BUTTON_PRIMARY, 1, 0);
				(void)browser_view_pointer_button(view, 20.0f, 10.0f, BROWSER_BUTTON_PRIMARY, 0, 0);
			}
			descriptors[0].revents = 0;
			(void)poll(descriptors, 1, 0);
		}

		/* The view's work. */
		browser_view_process(view);
	}

	/* The loop was never held past the end. */
	atomic_store(&stats.done, 1);
	probe_summary("ended", 0);
	fflush(stdout);
	_exit(0);
}

/* The monotonic clock, in nanoseconds. */
static uint64_t
probe_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* A listener on 127.0.0.1, on a port of the system's choice. */
static int
probe_listen(
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
		fprintf(stderr, "stall-probe: no listener\n");
		return -1;
	}
	size = sizeof(address);
	getsockname(listener, (struct sockaddr *)&address, &size);
	*port = ntohs(address.sin_port);
	return listener;
}

/* The window's stand-in: a stamp every PROBE_INPUT_MS (one the pipe cannot take is still counted as sent and lost). */
static void *
probe_input(
	void *context)
{
	uint64_t stamp;

	(void)context;
	while (!atomic_load(&stats.done)) {
		stamp = probe_now();
		(void)write(input_pipe[1], &stamp, sizeof(stamp));
		atomic_fetch_add(&stats.sent, 1);
		usleep(PROBE_INPUT_MS * 1000);
	}
	return NULL;
}

/* Ends the run when the loop is still held past its end, with how long it has been held. */
static void *
probe_watchdog(
	void *context)
{
	uint64_t end;
	uint64_t held;

	(void)context;
	end = start_ns + (uint64_t)(seconds * 1e9) + 1000000000ULL;
	while (probe_now() < end)
		usleep(20000);
	if (atomic_load(&stats.done))
		return NULL;
	held = probe_now() - atomic_load(&stats.last_taken_ns);
	probe_summary("held", held);
	fflush(stdout);
	_exit(0);
}

/* Prints the summary line. */
static void
probe_summary(
	const char *how,
	uint64_t held_ns)
{
	uint64_t longest;

	longest = atomic_load(&stats.longest_ns);
	if (held_ns > longest)
		longest = held_ns;
	printf("stall-probe %s: %s sent=%llu taken=%llu longest_ms=%.1f over50=%llu over100=%llu over1000=%llu%s\n", scenario, how,
	    (unsigned long long)atomic_load(&stats.sent), (unsigned long long)atomic_load(&stats.taken), (double)longest / 1e6,
	    (unsigned long long)atomic_load(&stats.over50), (unsigned long long)atomic_load(&stats.over100),
	    (unsigned long long)atomic_load(&stats.over1000), held_ns != 0 ? " (the loop is still held at the end)" : "");
}

/* The server: each connection is answered by a thread of its own. */
static void *
probe_serve(
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
		pthread_create(&thread, NULL, probe_answer_thread, (void *)(long)client);
		pthread_detach(thread);
	}
	return NULL;
}

/* A connection's thread. */
static void *
probe_answer_thread(
	void *context)
{
	probe_answer((int)(long)context);
	return NULL;
}

/* hang-read: accepts and keeps every connection without a word. */
static void *
probe_hold(
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
probe_answer(
	int client)
{
	const struct probe_resource *found;
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
probe_add(
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

/* Writes the scenario's page; nonzero for an unknown scenario. */
static int
probe_page(
	char *page,
	size_t size)
{
	size_t used;
	unsigned index;

	if (strcmp(scenario, "idle") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>idle</title><p>idle");
	} else if (strcmp(scenario, "loop") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>loop</title><p>loop"
		    "<script>setTimeout(function () { var e = Date.now() + 3000; while (Date.now() < e) {} console.log('spun'); }, 300);</script>");
	} else if (strcmp(scenario, "handler") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>handler</title><body style='margin:0;height:2000px'><p>handler"
		    "<script>var once = 0; document.addEventListener('mousedown', function () { if (once++) return; var e = Date.now() + 1000; while (Date.now() < e) {} console.log('handled'); });</script>");
	} else if (strcmp(scenario, "sync-xhr-connect") == 0 || strcmp(scenario, "sync-xhr-read") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>xhr</title><p>xhr"
		    "<script>setTimeout(function () { var x = new XMLHttpRequest(); x.open('GET', 'http://127.0.0.1:%u/x', false);"
		    " try { x.send(); } catch (e) {} console.log('xhr ended'); }, 300);</script>",
		    (unsigned)(strcmp(scenario, "sync-xhr-connect") == 0 ? port_hang_connect : port_hang_read));
	} else if (strcmp(scenario, "docwrite-connect") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>write</title><p>write"
		    "<script>document.write('<script src=\"http://127.0.0.1:%u/x.js\"><\\/script>');</script><p>after",
		    (unsigned)port_hang_connect);
	} else if (strcmp(scenario, "async-connect") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>async</title><p>async<img src=\"http://127.0.0.1:%u/x.png\">"
		    "<script>fetch('http://127.0.0.1:%u/y').catch(function () {});</script>",
		    (unsigned)port_hang_connect, (unsigned)port_hang_connect);
	} else if (strcmp(scenario, "resize-big") == 0) {
		used = (size_t)snprintf(page, size, "<!DOCTYPE html><title>big</title><style>div{display:flex;flex-wrap:wrap}span{padding:2px;border:1px solid}</style>");
		for (index = 0; index < 6000U && used + 64U < size; index++)
			used += (size_t)snprintf(page + used, size - used, "<div><span>box %u</span><span>text</span></div>", index);
	} else if (strcmp(scenario, "decode-big") == 0) {
		snprintf(page, size, "<!DOCTYPE html><title>image</title><p>image<img src=\"/big.png\">");
	} else {
		return 1;
	}
	return 0;
}

/* The CRC-32 of PNG chunks (ISO 3309), over a running value. */
static uint32_t
probe_crc(
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
probe_put32(
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
probe_png(
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
	probe_put32(png + at, 13U);
	memcpy(png + at + 4, "IHDR", 4U);
	probe_put32(png + at + 8, side);
	probe_put32(png + at + 12, side);
	png[at + 16] = 8;
	png[at + 17] = 2;
	png[at + 18] = 0;
	png[at + 19] = 0;
	png[at + 20] = 0;
	probe_put32(png + at + 21, probe_crc(0, png + at + 4, 17U));
	at += 25;
	probe_put32(png + at, (uint32_t)data_length);
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
	probe_put32(png + at, (b << 16) | a);
	at += 4;
	probe_put32(png + at, probe_crc(0, png + 33 + 4, 4U + data_length));
	at += 4;
	probe_put32(png + at, 0);
	memcpy(png + at + 4, "IEND", 4U);
	probe_put32(png + at + 8, probe_crc(0, png + at + 4, 4U));
	at += 12;
	free(raw);
	*length = at;
	return png;
}

/* The view's wake, from the library's thread: a byte in the wake pipe. */
static void
probe_wake(
	void *context)
{
	char byte;

	(void)context;
	byte = 1;
	(void)write(wake_pipe[1], &byte, 1U);
}

/* The view asks for a redraw. */
static void
probe_redraw(
	void *context,
	struct browser_view *view)
{
	(void)context;
	(void)view;
	redraw_due = 1;
}

/* The page's console, on the probe's error output. */
static void
probe_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	(void)context;
	(void)view;
	(void)level;
	fprintf(stderr, "console: %.*s (%.0f ms)\n", (int)length, text, (double)(probe_now() - start_ns) / 1e6);
}
