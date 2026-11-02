/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a view of its own threads (BROWSER_THREADING_OWN): the
 * test's main thread stands in for the window's (it hands the view inputs,
 * calls browser_view_process and draws when told), and a second thread
 * stands in for the drawing one (it copies the latest picture,
 * browser_view_render_pixels, whenever frame_wake says there is one).
 *
 *   (a) while a timer's script spins for 5 s, and while a synchronous
 *       XMLHttpRequest waits for a connect that never comes, no call of
 *       the window's takes more than 20 ms;
 *   (b) presses, keys and a wheel reach the page's listeners in the order
 *       the window gave them;
 *   (c) pointer moves queued while the page is busy are merged into one;
 *   (d) destroying the view while a script runs for ever returns within 1 s;
 *   (e) a load from a page whose script runs for ever shows the new page
 *       within 1 s;
 *   (f) once 1024 presses wait, more are refused (ENOBUFS), a release is not;
 *   (g) browser_view_focus_field answers through the field callback, and
 *       text committed for a session that is not the page's is dropped;
 *   (h) a pagehide handler that runs for ever (the page is put away for
 *       going back) is stopped after 1 s and the new page is shown;
 *   and, in both modes, a link to a page whose style sheet comes slowly
 *   (as host-navigation-wait): the window's calls stay short, the page
 *   shown stays the first until the new one is ready, and the new one is
 *   shown with its sheet.
 *
 *   host-threaded-view      (run from the repository's root; prints one line per check and a summary)
 *
 * The test uses the view's public calls, and view_front_queued (the
 * mailbox's length) for (c).
 */

#include <browser/browser.h>

#include "view/front.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The longest a call of the window may take in a view of its own threads, and in one of the caller's (host-navigation-wait's bound), in ms. */
#define TEST_WINDOW_MAX_MS	20.0
#define TEST_CALLER_MAX_MS	400.0

/* How slowly the slow sheet comes, in milliseconds. */
#define TEST_SHEET_MS		1200

/* Where the test writes its pages. */
#define TEST_PAGES		"build/tmp/host-threaded-view"

/* What the stand-ins share: the console lines, the redraw and the field's answer (the main thread's), the picture's thread's state. */
static char test_console[65536];
static size_t test_console_length;
static int test_redraw;
static int test_field_answered;
static int test_field_error;
static unsigned test_checks;
static unsigned test_failures;
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_frame = PTHREAD_COND_INITIALIZER;
static int test_frames_owed;
static int test_render_stop;
static struct browser_view *test_render_view;
static unsigned long test_rendered;
static unsigned short test_port_serve;
static unsigned short test_port_hang;
static long long test_sheet_sent_ns;

static long long test_now(void);
static void test_sleep_ms(int ms);
static void test_check(int passed, const char *name, const char *format, ...);
static void test_wake(void *context);
static void test_frame_wake(void *context);
static void test_redraw_callback(void *context, struct browser_view *view);
static void test_console_callback(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void test_field(void *context, struct browser_view *view, int error, const float *caret);
static void *test_render(void *context);
static int test_view(int threading, struct browser_view **view);
static double test_pump(struct browser_view *view, int ms, const char *title);
static const char *test_page(const char *name, const char *body);
static int test_listen(int backlog, unsigned short *port);
static void *test_serve(void *context);
static void test_window_latency(void);
static void test_order_merge_bound(void);
static void test_destroy_navigate(void);
static void test_field_session(void);
static void test_navigation_wait(int threading);

/* Runs every check and prints the summary. */
int
main(
	void)
{
	struct sockaddr_in address;
	pthread_t thread;
	int listener;
	int filler;
	int index;

	/* The pages' directory, the server and the listener whose connects never complete. */
	setenv("XDG_DATA_HOME", "build/tmp/host-threaded-view-data", 1);
	mkdir("build/tmp", 0755);
	mkdir(TEST_PAGES, 0755);
	listener = test_listen(64, &test_port_serve);
	test_listen(0, &test_port_hang);
	if (listener < 0)
		return 2;
	for (index = 0; index < 4; index++) {
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(test_port_hang);
		filler = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		(void)connect(filler, (struct sockaddr *)&address, sizeof(address));
	}
	pthread_create(&thread, NULL, test_serve, (void *)(long)listener);

	/* The checks. */
	test_window_latency();
	test_order_merge_bound();
	test_destroy_navigate();
	test_field_session();
	test_navigation_wait(BROWSER_THREADING_CALLER);
	test_navigation_wait(BROWSER_THREADING_OWN);

	/* The summary. */
	printf("host-threaded-view: %u checks, %u failed\n", test_checks, test_failures);
	if (test_failures != 0)
		return 1;
	return 0;
}

/* The monotonic clock in nanoseconds. */
static long long
test_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* Sleeps so many milliseconds. */
static void
test_sleep_ms(
	int ms)
{
	struct timespec delay;

	delay.tv_sec = ms / 1000;
	delay.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&delay, NULL);
}

/* Counts a check and prints its line. */
static void
test_check(
	int passed,
	const char *name,
	const char *format,
	...)
{
	va_list arguments;

	test_checks++;
	if (!passed)
		test_failures++;
	printf("%s %s: ", passed ? "PASS" : "FAIL", name);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
	fflush(stdout);
}

/* The view's wake: the window's loop polls, so nothing is needed. */
static void
test_wake(
	void *context)
{
	(void)context;
}

/* A new picture: the drawing stand-in copies it. */
static void
test_frame_wake(
	void *context)
{
	(void)context;
	pthread_mutex_lock(&test_lock);
	test_frames_owed = 1;
	pthread_cond_signal(&test_frame);
	pthread_mutex_unlock(&test_lock);
}

/* The view asks to be drawn again. */
static void
test_redraw_callback(
	void *context,
	struct browser_view *view)
{
	(void)context;
	(void)view;
	test_redraw = 1;
}

/* A console line, kept. */
static void
test_console_callback(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	(void)context;
	(void)view;
	(void)level;
	if (test_console_length + length + 2U >= sizeof(test_console))
		return;
	memcpy(test_console + test_console_length, text, length);
	test_console_length += length;
	test_console[test_console_length++] = '\n';
	test_console[test_console_length] = '\0';
}

/* The answer of browser_view_focus_field. */
static void
test_field(
	void *context,
	struct browser_view *view,
	int error,
	const float *caret)
{
	(void)context;
	(void)view;
	(void)caret;
	test_field_answered = 1;
	test_field_error = error;
}

/* The drawing stand-in: the latest picture whenever there is a new one, until told to stop. */
static void *
test_render(
	void *context)
{
	uint32_t *pixels;

	(void)context;
	pixels = malloc((size_t)800U * 600U * sizeof(uint32_t));
	if (pixels == NULL)
		return NULL;
	pthread_mutex_lock(&test_lock);
	for (;;) {
		while (!test_frames_owed && !test_render_stop)
			pthread_cond_wait(&test_frame, &test_lock);
		if (test_render_stop)
			break;
		test_frames_owed = 0;
		pthread_mutex_unlock(&test_lock);
		if (browser_view_render_pixels(test_render_view, pixels, 800U, 600U, 800U * sizeof(uint32_t)) == 0)
			test_rendered++;
		pthread_mutex_lock(&test_lock);
	}
	pthread_mutex_unlock(&test_lock);
	free(pixels);
	return NULL;
}

/* Makes a view of 800 by 600 with the test's callbacks. */
static int
test_view(
	int threading,
	struct browser_view **view)
{
	static const struct browser_fonts fonts = {
		"userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf", "userland/desktop/fonts/Mahora-Bold.ttf",
		"userland/desktop/fonts/Mahora-Mono.ttf"
	};
	static struct browser_callbacks callbacks;
	struct browser_view_options options;

	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.redraw = test_redraw_callback;
	callbacks.console = test_console_callback;
	callbacks.field = test_field;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.width = 800U;
	options.height = 600U;
	options.wake = test_wake;
	options.threading = threading;
	options.frame_wake = test_frame_wake;
	return browser_view_create(&options, view);
}

/*
 * The window's loop for so many milliseconds (or until the page shown has
 * the title, when one is given): a pointer move, browser_view_process and
 * a picture when the view asked for a redraw, every 2 ms.  Returns the
 * longest time one round of the window's calls took, in milliseconds.
 */
static double
test_pump(
	struct browser_view *view,
	int ms,
	const char *title)
{
	static uint32_t pixels[800U * 600U];
	const char *shown;
	long long end;
	long long start;
	long long took;
	long long longest;

	longest = 0;
	end = test_now() + (long long)ms * 1000000LL;
	while (test_now() < end) {
		start = test_now();
		(void)browser_view_pointer_move(view, 400.0f, 300.0f, 0);
		browser_view_process(view);
		if (test_redraw) {
			test_redraw = 0;
			(void)browser_view_render_pixels(view, pixels, 800U, 600U, 800U * sizeof(uint32_t));
		}
		took = test_now() - start;
		if (took > longest)
			longest = took;
		if (title != NULL) {
			shown = browser_view_title(view);
			if (shown != NULL && strcmp(shown, title) == 0)
				break;
		}
		test_sleep_ms(2);
	}
	return (double)longest / 1e6;
}

/* Writes a page of the test and reports its absolute path (a static buffer of its own per name, up to 16 names). */
static const char *
test_page(
	const char *name,
	const char *body)
{
	static char paths[16][512];
	static unsigned used;
	char *path;
	char directory[256];
	FILE *file;

	path = paths[used++ % 16U];
	if (getcwd(directory, sizeof(directory)) == NULL)
		return NULL;
	snprintf(path, 512, "%s/%s/%s", directory, TEST_PAGES, name);
	file = fopen(path, "w");
	if (file == NULL)
		return NULL;
	fputs(body, file);
	fclose(file);
	return path;
}

/* Listens on a free port of 127.0.0.1 with a backlog; returns the socket or -1. */
static int
test_listen(
	int backlog,
	unsigned short *port)
{
	struct sockaddr_in address;
	socklen_t length;
	int listener;

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, backlog) != 0)
		return -1;
	length = sizeof(address);
	getsockname(listener, (struct sockaddr *)&address, &length);
	*port = ntohs(address.sin_port);
	return listener;
}

/* The server: /first.html, /second.html (with /slow.css, which comes after TEST_SHEET_MS), one request per connection. */
static void *
test_serve(
	void *context)
{
	static const char first[] = "<!DOCTYPE html><title>first</title><p>first";
	static const char second[] = "<!DOCTYPE html><title>second</title><link rel=stylesheet href=/slow.css><p>second";
	static const char sheet[] = "body { height: 5000px; }";
	char request[2048];
	char header[256];
	const char *body;
	const char *type;
	ssize_t got;
	int listener;
	int client;

	listener = (int)(long)context;
	for (;;) {
		client = accept(listener, NULL, NULL);
		if (client < 0)
			continue;
		got = read(client, request, sizeof(request) - 1U);
		if (got <= 0) {
			close(client);
			continue;
		}
		request[got] = '\0';
		body = first;
		type = "text/html";
		if (strstr(request, "GET /second.html") != NULL) {
			body = second;
		} else if (strstr(request, "GET /slow.css") != NULL) {
			test_sleep_ms(TEST_SHEET_MS);
			__atomic_store_n(&test_sheet_sent_ns, test_now(), __ATOMIC_RELEASE);
			body = sheet;
			type = "text/css";
		}
		snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		    type, strlen(body));
		(void)write(client, header, strlen(header));
		(void)write(client, body, strlen(body));
		close(client);
	}
	return NULL;
}

/* (a): the window's calls stay short while a script spins and while a synchronous request waits for a connect. */
static void
test_window_latency(void)
{
	struct browser_view *view;
	pthread_t render;
	char page[1024];
	const char *path;
	double longest;
	int error;

	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(a) view", "no view (%d)", error);
		return;
	}
	test_render_view = view;
	test_render_stop = 0;
	pthread_create(&render, NULL, test_render, NULL);

	/* A timer's script that spins for 5 s. */
	path = test_page("spin.html", "<!DOCTYPE html><title>spin</title><p>spin<script>"
	    "setTimeout(function () { var t = Date.now(); while (Date.now() - t < 5000) {} }, 50);</script>");
	(void)browser_view_load(view, path);
	longest = test_pump(view, 5500, NULL);
	test_check(longest <= TEST_WINDOW_MAX_MS, "(a) window calls while a timer's script spins 5 s", "longest %.2f ms", longest);

	/* A timer's synchronous XMLHttpRequest to a connect that never completes. */
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>xhr</title><p>xhr<script>setTimeout(function () {"
	    " var x = new XMLHttpRequest(); x.open('GET', 'http://127.0.0.1:%u/', false); try { x.send(); } catch (e) {} }, 50);</script>",
	    (unsigned)test_port_hang);
	path = test_page("xhr.html", page);
	(void)browser_view_load(view, path);
	longest = test_pump(view, 4000, NULL);
	test_check(longest <= TEST_WINDOW_MAX_MS, "(a) window calls while a synchronous XMLHttpRequest waits for a connect", "longest %.2f ms",
	    longest);

	/* The drawing stand-in drew, and the view goes (out of the request it waits for). */
	pthread_mutex_lock(&test_lock);
	test_render_stop = 1;
	pthread_cond_signal(&test_frame);
	pthread_mutex_unlock(&test_lock);
	pthread_join(render, NULL);
	test_check(test_rendered > 0, "(a) the drawing thread copied pictures", "%lu pictures", test_rendered);
	browser_view_destroy(view);
}

/* (b), (c), (f): the order of inputs, the merge of moves, the bound on presses. */
static void
test_order_merge_bound(void)
{
	static const char *expected = "E mousedown\nE keydown a\nE keyup a\nE mouseup\nE click\nE wheel\nE keydown b\nE keyup b\n";
	struct browser_view *view;
	const char *path;
	char got[1024];
	const char *line;
	const char *next;
	size_t length;
	size_t queued;
	int refused;
	int error;
	int index;

	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(b) view", "no view (%d)", error);
		return;
	}

	/* (b): the page's listeners write what they hear. */
	path = test_page("order.html", "<!DOCTYPE html><title>order</title><style>body { height: 3000px; margin: 0; }</style><script>"
	    "['mousedown', 'mouseup', 'click', 'wheel'].forEach(function (t) { document.addEventListener(t, function () { console.log('E ' + t); }); });"
	    "['keydown', 'keyup'].forEach(function (t) { document.addEventListener(t, function (e) { console.log('E ' + t + ' ' + e.key); }); });"
	    "</script>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, "order");
	test_console_length = 0;
	test_console[0] = '\0';
	(void)browser_view_pointer_button(view, 100.0f, 100.0f, BROWSER_BUTTON_PRIMARY, 1, 0);
	(void)browser_view_key(view, "a", "KeyA", "a", 1, 0, 0);
	(void)browser_view_key(view, "a", "KeyA", "", 0, 0, 0);
	(void)browser_view_pointer_button(view, 100.0f, 100.0f, BROWSER_BUTTON_PRIMARY, 0, 0);
	(void)browser_view_wheel(view, 100.0f, 100.0f, 0.0f, 40.0f, 0);
	(void)browser_view_key(view, "b", "KeyB", "b", 1, 0, 0);
	(void)browser_view_key(view, "b", "KeyB", "", 0, 0, 0);
	for (index = 0; index < 1000 && strstr(test_console, "E keyup b") == NULL; index++) {
		browser_view_process(view);
		test_sleep_ms(2);
	}
	length = 0;
	got[0] = '\0';
	for (line = test_console; *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next == NULL)
			break;
		next++;
		if (strncmp(line, "E ", 2) == 0 && length + (size_t)(next - line) < sizeof(got)) {
			memcpy(got + length, line, (size_t)(next - line));
			length += (size_t)(next - line);
			got[length] = '\0';
		}
	}
	test_check(strcmp(got, expected) == 0, "(b) the listeners hear the inputs in the window's order", "\"%s\"", got);

	/* (c) and (f): while the page is busy, moves merge and presses are bounded. */
	path = test_page("busy.html", "<!DOCTYPE html><title>busy</title><script>"
	    "setTimeout(function () { var t = Date.now(); while (Date.now() - t < 2000) {} }, 300);</script>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, "busy");
	test_sleep_ms(700);
	for (index = 0; index < 500; index++)
		(void)browser_view_pointer_move(view, (float)index, 10.0f, 0);
	queued = view_front_queued(view);
	test_check(queued == 1, "(c) 500 moves queued while the page is busy are one entry", "%zu entries", queued);
	refused = 0;
	for (index = 0; index < 1100; index++) {
		error = browser_view_pointer_button(view, 10.0f, 10.0f, BROWSER_BUTTON_PRIMARY, 1, 0);
		if (error == ENOBUFS)
			refused++;
	}
	error = browser_view_pointer_button(view, 10.0f, 10.0f, BROWSER_BUTTON_PRIMARY, 0, 0);
	test_check(refused == 1100 - 1024 && error == 0, "(f) presses past 1024 are refused, a release is not",
	    "%d refused, release %d", refused, error);
	browser_view_destroy(view);
}

/* (d) and (e): the view's end and a navigation out of a script that runs for ever. */
static void
test_destroy_navigate(void)
{
	static const char *looping = "<!DOCTYPE html><title>loop</title><p>loop<script>setTimeout(function () { for (;;) {} }, 0);</script>";
	struct browser_view *view;
	const char *path;
	const char *title;
	long long start;
	double took;
	int error;

	/* (d). */
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(d) view", "no view (%d)", error);
		return;
	}
	path = test_page("loop.html", looping);
	(void)browser_view_load(view, path);
	(void)test_pump(view, 500, NULL);
	start = test_now();
	browser_view_destroy(view);
	took = (double)(test_now() - start) / 1e6;
	test_check(took <= 1000.0, "(d) destroying a view whose script runs for ever", "%.1f ms", took);

	/* (e). */
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(e) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, path);
	(void)test_pump(view, 500, NULL);
	path = test_page("b.html", "<!DOCTYPE html><title>B</title><p>b");
	start = test_now();
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, "B");
	took = (double)(test_now() - start) / 1e6;
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "B") == 0 && took <= 1000.0, "(e) a load out of a script that runs for ever",
	    "title \"%s\" after %.1f ms", title != NULL ? title : "(none)", took);

	/* (h): a page whose pagehide handler runs for ever, left for another. */
	path = test_page("hide.html", "<!DOCTYPE html><title>hide</title><p>hide<script>"
	    "addEventListener('pagehide', function () { for (;;) {} });</script>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, "hide");
	path = test_page("c.html", "<!DOCTYPE html><title>C</title><p>c");
	start = test_now();
	(void)browser_view_load(view, path);
	(void)test_pump(view, 4000, "C");
	took = (double)(test_now() - start) / 1e6;
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "C") == 0 && took >= 900.0 && took <= 1600.0,
	    "(h) a pagehide handler that runs for ever is stopped after 1 s", "title \"%s\" after %.1f ms", title != NULL ? title : "(none)",
	    took);
	browser_view_destroy(view);
}

/* (g): the field callback, and text for an old session dropped. */
static void
test_field_session(void)
{
	struct browser_view *view;
	const char *path;
	char text[256];
	size_t cursor;
	uint64_t session;
	unsigned hints;
	int purpose;
	int index;
	int error;

	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(g) view", "no view (%d)", error);
		return;
	}
	path = test_page("field.html", "<!DOCTYPE html><title>field</title><input id=a><input autocomplete=one-time-code value=x>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, "field");
	test_field_answered = 0;
	error = browser_view_focus_field(view, "one-time-code");
	for (index = 0; index < 1000 && !test_field_answered; index++) {
		browser_view_process(view);
		test_sleep_ms(2);
	}
	test_check(error == 0 && test_field_answered && test_field_error == 0, "(g) the field callback answers browser_view_focus_field",
	    "queued %d, answered %d, error %d", error, test_field_answered, test_field_error);

	/* The state after the focus, then text for another session and for this one. */
	(void)test_pump(view, 200, NULL);
	session = browser_view_text_session(view);
	(void)browser_view_commit_text_in(view, session + 12345U, "zz", 0, 0);
	(void)test_pump(view, 200, NULL);
	text[0] = '\0';
	(void)browser_view_text_context(view, text, sizeof(text), &cursor, &purpose, &hints);
	test_check(strstr(text, "zz") == NULL, "(g) text for another session is dropped", "value \"%s\"", text);
	(void)browser_view_commit_text_in(view, session, "zz", 0, 0);
	(void)test_pump(view, 200, NULL);
	text[0] = '\0';
	(void)browser_view_text_context(view, text, sizeof(text), &cursor, &purpose, &hints);
	test_check(strstr(text, "zz") != NULL, "(g) text for the page's session goes in", "value \"%s\"", text);
	browser_view_destroy(view);
}

/* The host-navigation-wait scene in one mode: a load of a page whose sheet comes slowly. */
static void
test_navigation_wait(
	int threading)
{
	static uint32_t pixels[800U * 600U];
	struct browser_view *view;
	const char *mode;
	const char *url;
	char location[128];
	long long start;
	long long took;
	long long longest;
	long long switched;
	long long sent;
	double height;
	double limit;
	int error;

	mode = threading == BROWSER_THREADING_OWN ? "own" : "caller";
	limit = threading == BROWSER_THREADING_OWN ? TEST_WINDOW_MAX_MS : TEST_CALLER_MAX_MS;
	error = test_view(threading, &view);
	if (error != 0) {
		test_check(0, "navigation-wait view", "%s: no view (%d)", mode, error);
		return;
	}
	snprintf(location, sizeof(location), "http://127.0.0.1:%u/first.html", (unsigned)test_port_serve);
	(void)browser_view_load(view, location);
	(void)test_pump(view, 3000, "first");
	__atomic_store_n(&test_sheet_sent_ns, 0, __ATOMIC_RELEASE);
	snprintf(location, sizeof(location), "http://127.0.0.1:%u/second.html", (unsigned)test_port_serve);
	(void)browser_view_load(view, location);

	/* The loop until the new page is shown with its sheet, at most 6 s. */
	longest = 0;
	switched = 0;
	height = 0.0;
	start = test_now();
	while (test_now() - start < 6000000000LL) {
		took = test_now();
		browser_view_process(view);
		if (test_redraw) {
			test_redraw = 0;
			if (threading == BROWSER_THREADING_OWN) {
				(void)browser_view_render_pixels(view, pixels, 800U, 600U, 800U * sizeof(uint32_t));
			} else {
				(void)browser_view_draw_pixels(view, pixels, 800U, 600U, 800U * sizeof(uint32_t));
			}
		}
		url = browser_view_url(view);
		if (switched == 0 && url != NULL && strstr(url, "second.html") != NULL)
			switched = test_now();
		height = browser_view_document_height(view);
		took = test_now() - took;
		if (took > longest)
			longest = took;
		if (switched != 0 && height >= 5000.0)
			break;
		test_sleep_ms(2);
	}
	sent = __atomic_load_n(&test_sheet_sent_ns, __ATOMIC_ACQUIRE);
	test_check((double)longest / 1e6 <= limit, "navigation-wait: the window's calls stay short", "%s: longest %.2f ms (limit %.0f)", mode,
	    (double)longest / 1e6, limit);
	test_check(switched != 0 && sent != 0 && switched >= sent, "navigation-wait: the first page stays until the new one is ready",
	    "%s: shown %.1f ms after the sheet was sent", mode, (double)(switched - sent) / 1e6);
	test_check(height >= 5000.0, "navigation-wait: the new page is shown with its sheet", "%s: height %.0f", mode, height);
	browser_view_destroy(view);
}
