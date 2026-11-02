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
 *   (i) the committed frame drawn by the drawing thread is the page as a
 *       view of the caller's draws it, pixel for pixel: with the CPU
 *       (browser_view_render_pixels against browser_view_draw_pixels) and
 *       on the GPU (browser_view_render into an offscreen image against
 *       browser_view_draw; skipped without a Vulkan device), for pages with
 *       text, a web font, borders, a gradient, an image, filters and an
 *       iframe;
 *   (j) requestAnimationFrame runs once for each present
 *       (browser_view_presented) and not at all while nothing is presented;
 *   (k) while a script spins for 5 s the drawing thread draws the last
 *       committed frame, at a size other than the view's too, at once;
 *   (l) the engine's DOM pages of getComputedStyle, offsetWidth and
 *       getBoundingClientRect (plan/ws074/tests/dom computed, geometry,
 *       fit-content-computed) write the same console in a view of its own
 *       threads (where the style thread computes the styles and the boxes)
 *       as in a view of the caller's;
 *   (m) 30000 reads of offsetWidth after one change of the document lend
 *       the document to the style thread a few times only (the first read
 *       and the rendering update), 1000 reads each after a change lend it
 *       at least 1000 times; the time a lend takes is printed;
 *   (n) a heap lent to another thread (vm_heap_lend) does not collect
 *       while the work runs there, however much it allocates, and collects
 *       at the lender's next allocation; the work runs on a borrower; and
 *       the round trip of an empty work lent to a style thread is printed;
 *   (o) plan/ws074/tests/bench/bug411.html in both views: the same checks,
 *       the times and the lends printed;
 *   (p) no thread a heap was lent to entered a script or tried to collect
 *       (vm_borrower_slips) in all the checks above;
 *   (q) a page committed is told with its own state: the committed
 *       callback, with the window's thread processing while the new page's
 *       load handler spins before its state is published, reads the new
 *       location and title;
 *   and, in both modes, a link to a page whose style sheet comes slowly
 *   (as host-navigation-wait): the window's calls stay short, the page
 *   shown stays the first until the new one is ready, and the new one is
 *   shown with its sheet.
 *
 *   host-threaded-view      (run from the repository's root; prints one line per check and a summary)
 *
 * The test uses the view's public calls, view_front_queued (the
 * mailbox's length) for (c), view_front_lend_counts for (m) and (o), and
 * the heap's lending (vm/vm.h) for (n) and (p).
 */

#include <browser/browser.h>

#include "view/front.h"
#include "view/style.h"

#include <arpa/inet.h>
#include <dlfcn.h>
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
static unsigned test_committed;
static char test_committed_url[512];
static char test_committed_title[128];
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
static void test_committed_callback(void *context, struct browser_view *view);
static void test_committed_state(void);
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
static void test_same_pictures(void);
static int test_gpu_render(struct browser_view *view, int own, uint32_t *pixels);
static void test_animation_frames(void);
static void test_draw_during_script(void);
static void test_wait(struct browser_view *view, int ms, const char *title);
static void test_settle_console(struct browser_view *view, int quiet_ms, int most_ms);
static void test_dom_pages(void);
static void test_lends(void);
static int test_lender(void *context, vm_lend_work work, void *argument);
static void *test_borrower(void *context);
static int test_lent_work(void *argument);
static void test_lend_heap(void);
static void test_bench_both(void);
static int test_empty_work(void *argument);
static void test_lend_round_trip(void);

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

	/*
	 * lavapipe stays loaded to the end: ThreadSanitizer stops the process
	 * when a library its suppressions name is unloaded (the GPU checks load
	 * and unload it).
	 */
	(void)dlopen("libvulkan_lvp.so", RTLD_NOW | RTLD_NODELETE);

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
	test_same_pictures();
	test_animation_frames();
	test_draw_during_script();
	test_dom_pages();
	test_lends();
	test_lend_heap();
	test_lend_round_trip();
	test_bench_both();
	test_committed_state();
	test_check(vm_borrower_slips() == 0, "(p) no borrower thread entered a script or collected", "%lu times", vm_borrower_slips());

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

/* A page committed: the location and the title the getters read in the callback, kept. */
static void
test_committed_callback(
	void *context,
	struct browser_view *view)
{
	const char *url;
	const char *title;

	(void)context;
	url = browser_view_url(view);
	title = browser_view_title(view);
	snprintf(test_committed_url, sizeof(test_committed_url), "%s", url != NULL ? url : "(none)");
	snprintf(test_committed_title, sizeof(test_committed_title), "%s", title != NULL ? title : "(none)");
	test_committed++;
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
	callbacks.committed = test_committed_callback;
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
		if (test_redraw && view_front_of(view) == NULL) {
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
			if (threading == BROWSER_THREADING_CALLER)
				(void)browser_view_draw_pixels(view, pixels, 800U, 600U, 800U * sizeof(uint32_t));
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

/* (i): the committed frames, drawn by the drawing stand-in, are the caller's pictures of the same pages. */
static void
test_same_pictures(void)
{
	static uint32_t expected[800U * 600U];
	static uint32_t drawn[800U * 600U];
	static uint32_t gpu_expected[800U * 600U];
	static uint32_t gpu_drawn[800U * 600U];
	struct browser_view *caller;
	struct browser_view *own;
	const char *pages[6];
	char body[4096];
	char root[256];
	char name[512];
	long long start;
	size_t differ;
	size_t index;
	unsigned page;
	int caller_gpu;
	int own_gpu;
	int same;
	int error;

	/* The pages: the test's own (an iframe, a web font, an image, a gradient, filters) and three of the engine's. */
	if (getcwd(root, sizeof(root)) == NULL)
		return;
	pages[0] = test_page("commit-inner.html", "<!DOCTYPE html><body style='margin:0;background:#ffe'><p style='color:#a00'>inner frame text</p>"
	    "<div style='width:40px;height:30px;background:linear-gradient(90deg,#00f,#0f0)'></div>");
	snprintf(body, sizeof(body),
	    "<!DOCTYPE html><title>commit</title><style>@font-face { font-family: web; src: url('file://%s/userland/desktop/fonts/Mahora-Bold.ttf'); }"
	    " .web { font-family: web; font-size: 30px }</style><body style='background:#eef'>"
	    "<p class='web'>A web font: Hamburgefonstiv</p>"
	    "<div style='border:4px dashed #c33;border-radius:12px;padding:8px;width:300px;background:linear-gradient(45deg,#fc0,#0cf)'>gradient box</div>"
	    "<img src='file://%s/plan/ws074/tests/pages/canvas-background.png' style='width:120px;height:80px'>"
	    "<div style='filter:blur(2px) grayscale(1);width:100px;height:40px;background:#f60'>blur</div>"
	    "<div style='position:relative;width:200px;height:60px'><div style='position:absolute;inset:0;backdrop-filter:invert(1)'></div>backdrop</div>"
	    "<div style='opacity:0.5;background:#090;width:80px;height:30px'>half</div>"
	    "<iframe src='%s' style='width:300px;height:100px;border:2px solid #333'></iframe>",
	    root, root, pages[0]);
	pages[1] = test_page("commit.html", body);
	for (index = 0; index < 3U; index++) {
		static const char *const engine[3] = { "first", "blocks", "flex" };

		snprintf(name, sizeof(name), "%s/plan/ws074/tests/pages/%s.html", root, engine[index]);
		pages[2U + index] = strdup(name);
	}
	pages[5] = NULL;

	/* Each page in both views, the own one drawn by this thread (the drawing stand-in) until it shows the same, at most 5 s. */
	for (page = 1U; pages[page] != NULL; page++) {
		error = test_view(BROWSER_THREADING_CALLER, &caller);
		if (error == 0)
			error = test_view(BROWSER_THREADING_OWN, &own);
		if (error != 0) {
			test_check(0, "(i) views", "none (%d)", error);
			return;
		}
		(void)browser_view_load(caller, pages[page]);
		(void)browser_view_settle(caller, 5000.0, BROWSER_SETTLE_LAYOUT);
		error = browser_view_draw_pixels(caller, expected, 800U, 600U, 800U * sizeof(uint32_t));
		caller_gpu = test_gpu_render(caller, 0, gpu_expected);
		(void)browser_view_load(own, pages[page]);

		/* The CPU's picture of the latest committed frame, until it is the caller's. */
		same = 0;
		differ = 0;
		start = test_now();
		while (!same && test_now() - start < 5000000000LL) {
			browser_view_process(own);
			memset(drawn, 0, sizeof(drawn));
			if (browser_view_render_pixels(own, drawn, 800U, 600U, 800U * sizeof(uint32_t)) == 0) {
				differ = 0;
				for (index = 0; index < 800U * 600U; index++)
					differ += drawn[index] != expected[index];
				same = differ == 0;
			}
			if (!same)
				test_sleep_ms(20);
		}
		test_check(error == 0 && same, "(i) the CPU draws the committed frame as the caller's view draws the page", "%s: %zu pixels differ",
		    strrchr(pages[page], '/') + 1, differ);

		/* The GPU's, likewise (the frame is the same one now). */
		own_gpu = test_gpu_render(own, 1, gpu_drawn);
		if (caller_gpu == 77 || own_gpu == 77) {
			printf("SKIP (i) GPU: no Vulkan device\n");
		} else {
			differ = 0;
			for (index = 0; index < 800U * 600U; index++)
				differ += gpu_drawn[index] != gpu_expected[index];
			test_check(caller_gpu == 0 && own_gpu == 0 && differ == 0, "(i) the GPU draws the committed frame as the caller's view draws the page",
			    "%s: caller %d, own %d, %zu pixels differ", strrchr(pages[page], '/') + 1, caller_gpu, own_gpu, differ);
		}
		browser_view_destroy(own);
		browser_view_destroy(caller);
	}
}

/*
 * Draws a view on an offscreen image of 800 x 600 and reads it back: a
 * view of the caller's with browser_view_draw, a view of its own threads
 * with browser_view_render into a command buffer of the test's.  Reports
 * 0, 77 without a Vulkan device, or another error.
 */
static int
test_gpu_render(
	struct browser_view *view,
	int own,
	uint32_t *pixels)
{
	struct browser_offscreen *offscreen;
	struct browser_gpu_failure failure;
	struct browser_target target;
	struct browser_gpu gpu;
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo buffer_info;
	VkCommandBufferBeginInfo begin;
	VkSubmitInfo submit;
	VkCommandPool pool;
	VkCommandBuffer commands;
	VkQueue queue;
	uint64_t serial;
	int error;

	error = browser_offscreen_create(800U, 600U, &offscreen, &failure);
	if (error != 0)
		return 77;
	browser_offscreen_target(offscreen, &gpu, &target);
	browser_view_set_gpu(view, &gpu);
	if (!own) {
		error = browser_view_draw(view, &target, VK_NULL_HANDLE, VK_NULL_HANDLE);
	} else {
		memset(&pool_info, 0, sizeof(pool_info));
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.queueFamilyIndex = gpu.queue_family;
		error = vkCreateCommandPool(gpu.device, &pool_info, NULL, &pool) == VK_SUCCESS ? 0 : EIO;
		if (error == 0) {
			memset(&buffer_info, 0, sizeof(buffer_info));
			buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			buffer_info.commandPool = pool;
			buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			buffer_info.commandBufferCount = 1U;
			error = vkAllocateCommandBuffers(gpu.device, &buffer_info, &commands) == VK_SUCCESS ? 0 : EIO;
			if (error == 0) {
				memset(&begin, 0, sizeof(begin));
				begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				(void)vkBeginCommandBuffer(commands, &begin);
				error = browser_view_render(view, &target, commands, &serial);
				(void)vkEndCommandBuffer(commands);
				if (error == 0) {
					memset(&submit, 0, sizeof(submit));
					submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
					submit.commandBufferCount = 1U;
					submit.pCommandBuffers = &commands;
					vkGetDeviceQueue(gpu.device, gpu.queue_family, 0U, &queue);
					error = vkQueueSubmit(queue, 1U, &submit, VK_NULL_HANDLE) == VK_SUCCESS ? 0 : EIO;
					(void)vkQueueWaitIdle(queue);
				}
			}
			vkDestroyCommandPool(gpu.device, pool, NULL);
		}
	}
	if (error == 0)
		error = browser_offscreen_read(offscreen, pixels, 800U * sizeof(uint32_t), &failure);
	browser_view_set_gpu(view, NULL);
	browser_offscreen_destroy(offscreen);
	return error;
}

/* (j): an animation frame for each present, and none without one. */
static void
test_animation_frames(void)
{
	struct browser_view *view;
	const char *path;
	const char *title;
	char wanted[32];
	int presents;
	int error;

	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(j) view", "no view (%d)", error);
		return;
	}
	path = test_page("raf.html", "<!DOCTYPE html><title>n0</title><p>frames<script>var n = 0;"
	    " function f() { n++; document.title = 'n' + n; requestAnimationFrame(f); } requestAnimationFrame(f);</script>");
	(void)browser_view_load(view, path);

	/* Nothing presented for 600 ms after the page is shown: no animation frame. */
	(void)test_pump(view, 5000, "n0");
	(void)test_pump(view, 600, NULL);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "n0") == 0, "(j) no animation frame while nothing is presented", "title \"%s\"",
	    title != NULL ? title : "(none)");

	/* Ten presents, each after the frame of the one before: ten frames, then no more. */
	for (presents = 0; presents < 10; presents++) {
		browser_view_presented(view, 0);
		snprintf(wanted, sizeof(wanted), "n%d", presents + 1);
		(void)test_pump(view, 2000, wanted);
	}
	(void)test_pump(view, 400, NULL);
	title = browser_view_title(view);
	snprintf(wanted, sizeof(wanted), "n%d", presents);
	test_check(title != NULL && strcmp(title, wanted) == 0, "(j) one animation frame for each present", "title \"%s\" after %d presents",
	    title != NULL ? title : "(none)", presents);
	browser_view_destroy(view);
}

/* (k): the last committed frame is drawn while a script spins, at another size too. */
static void
test_draw_during_script(void)
{
	static uint32_t pixels[1000U * 700U];
	struct browser_view *view;
	const char *path;
	long long start;
	long long took;
	uint32_t corner;
	int drawn;
	int error;

	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(k) view", "no view (%d)", error);
		return;
	}
	path = test_page("spin-color.html", "<!DOCTYPE html><title>spin</title><body style='background:#336699'><p>spin<script>"
	    "setTimeout(function () { document.title = 'spinning'; var t = Date.now(); while (Date.now() - t < 5000) {} }, 300);</script>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 1000, NULL);

	/* While it spins: the frame at 1000 x 700, the part past the page's 800 x 600 in its canvas color. */
	start = test_now();
	drawn = browser_view_render_pixels(view, pixels, 1000U, 700U, 1000U * sizeof(uint32_t));
	took = test_now() - start;
	corner = pixels[699U * 1000U + 999U];
	test_check(drawn == 0 && corner == 0xFF336699U && took < 100000000LL, "(k) the drawing thread draws the last frame while a script spins",
	    "error %d, corner %08x, %.2f ms", drawn, (unsigned)corner, (double)took / 1e6);
	browser_view_destroy(view);
}

/* The window's loop without inputs for so many milliseconds, or until the page shown has the title (when one is given). */
static void
test_wait(
	struct browser_view *view,
	int ms,
	const char *title)
{
	const char *shown;
	long long end;

	end = test_now() + (long long)ms * 1000000LL;
	while (test_now() < end) {
		browser_view_process(view);
		if (title != NULL) {
			shown = browser_view_title(view);
			if (shown != NULL && strcmp(shown, title) == 0)
				return;
		}
		test_sleep_ms(2);
	}
}

/* The window's loop until the console has not grown for quiet_ms, at most most_ms. */
static void
test_settle_console(
	struct browser_view *view,
	int quiet_ms,
	int most_ms)
{
	long long end;
	long long quiet;
	size_t length;

	end = test_now() + (long long)most_ms * 1000000LL;
	length = test_console_length;
	quiet = test_now();
	while (test_now() < end && test_now() - quiet < (long long)quiet_ms * 1000000LL) {
		browser_view_process(view);
		if (test_console_length != length) {
			length = test_console_length;
			quiet = test_now();
		}
		test_sleep_ms(2);
	}
}

/* (l): the DOM pages of styles and boxes write the same console in both views. */
static void
test_dom_pages(void)
{
	static const char *const names[3] = { "computed", "geometry", "fit-content-computed" };
	static char caller_console[65536];
	struct browser_view *caller;
	struct browser_view *own;
	char path[1024];
	char root[256];
	unsigned index;
	long long start;
	size_t lines;
	size_t at;
	int error;

	if (getcwd(root, sizeof(root)) == NULL)
		return;
	for (index = 0; index < 3U; index++) {
		snprintf(path, sizeof(path), "%s/plan/ws074/tests/dom/%s.html", root, names[index]);

		/* The caller's view: the console of the load and its timers. */
		error = test_view(BROWSER_THREADING_CALLER, &caller);
		if (error != 0) {
			test_check(0, "(l) view", "none (%d)", error);
			return;
		}
		test_console_length = 0;
		test_console[0] = '\0';
		(void)browser_view_load(caller, path);
		(void)browser_view_settle(caller, 5000.0, BROWSER_SETTLE_LAYOUT);
		memcpy(caller_console, test_console, test_console_length + 1U);
		browser_view_destroy(caller);

		/* The view of its own threads: the console once it is quiet. */
		error = test_view(BROWSER_THREADING_OWN, &own);
		if (error != 0) {
			test_check(0, "(l) view", "none (%d)", error);
			return;
		}
		test_console_length = 0;
		test_console[0] = '\0';
		(void)browser_view_load(own, path);
		lines = 0;
		for (at = 0; caller_console[at] != '\0'; at++)
			lines += caller_console[at] == '\n';
		start = test_now();
		while (test_console_length < strlen(caller_console) && test_now() - start < 30000000000LL)
			test_wait(own, 20, NULL);
		test_settle_console(own, 300, 2000);
		browser_view_destroy(own);

		/* The same lines. */
		test_check(lines != 0 && strcmp(caller_console, test_console) == 0, "(l) the style thread's styles and boxes are the caller's view's",
		    "%s: %zu lines%s", names[index], lines, strcmp(caller_console, test_console) == 0 ? "" : ", the consoles differ");
		if (strcmp(caller_console, test_console) != 0)
			printf("caller:\n%sown:\n%s", caller_console, test_console);
	}
}

/* (m): reads of a box that did not change lend nothing; reads after each change lend each time. */
static void
test_lends(void)
{
	struct browser_view *view;
	const char *path;
	const char *title;
	uint64_t lends[3];
	uint64_t waited[3];
	int error;

	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(m) view", "no view (%d)", error);
		return;
	}
	path = test_page("lends.html", "<!DOCTYPE html><title>ready</title><div id=b style='width:120px'>box</div><script>"
	    "var e = document.getElementById('b');"
	    "document.addEventListener('keydown', function (k) { var t = 0, i, s = Date.now();"
	    " if (k.key === 'a') { e.style.width = '130px'; for (i = 0; i < 30000; i++) t += e.offsetWidth;"
	    "  console.log('reads ' + (Date.now() - s)); document.title = 'a' + t; }"
	    " if (k.key === 'b') { for (i = 0; i < 1000; i++) { e.style.width = (100 + i % 50) + 'px'; t += e.offsetWidth; }"
	    "  console.log('changes ' + (Date.now() - s)); document.title = 'b' + t; } });</script>");
	(void)browser_view_load(view, path);
	test_wait(view, 5000, "ready");
	test_wait(view, 500, NULL);
	test_console_length = 0;
	test_console[0] = '\0';

	/* 30000 reads after one change. */
	view_front_lend_counts(view, &lends[0], &waited[0]);
	(void)browser_view_key(view, "a", "KeyA", "a", 1, 0, 0);
	(void)browser_view_key(view, "a", "KeyA", NULL, 0, 0, 0);
	test_wait(view, 10000, "a3900000");
	view_front_lend_counts(view, &lends[1], &waited[1]);

	/* 1000 reads, each after a change. */
	(void)browser_view_key(view, "b", "KeyB", "b", 1, 0, 0);
	(void)browser_view_key(view, "b", "KeyB", NULL, 0, 0, 0);
	test_wait(view, 10000, "b124500");
	view_front_lend_counts(view, &lends[2], &waited[2]);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "b124500") == 0 && lends[1] - lends[0] <= 10U,
	    "(m) 30000 reads of a box after one change lend a few times", "%llu lends (%s)", (unsigned long long)(lends[1] - lends[0]), test_console);
	test_check(lends[2] - lends[1] >= 1000U, "(m) 1000 reads each after a change lend each time", "%llu lends, %.1f us a lend",
	    (unsigned long long)(lends[2] - lends[1]), (double)(waited[2] - waited[1]) / 1e3 / (double)(lends[2] - lends[1] + (lends[2] == lends[1])));
	browser_view_destroy(view);
}

/* What the lend of (n) did: the heap, the collections before and while the work ran, whether it ran on a borrower and was lent. */
struct test_lend {
	struct vm_heap *heap;
	vm_lend_work work;
	void *argument;
	int result;
	size_t collections_before;
	size_t collections_after;
	int borrower;
	int lent;
	size_t made;
};

/* The test's lender: runs the work on a new thread marked a borrower, and waits for it. */
static int
test_lender(
	void *context,
	vm_lend_work work,
	void *argument)
{
	struct test_lend *lend;
	pthread_t thread;

	lend = context;
	lend->work = work;
	lend->argument = argument;
	pthread_create(&thread, NULL, test_borrower, lend);
	pthread_join(thread, NULL);
	return lend->result;
}

/* The borrower thread of (n). */
static void *
test_borrower(
	void *context)
{
	struct test_lend *lend;

	lend = context;
	vm_thread_set_borrower(1);
	lend->result = lend->work(lend->argument);
	return NULL;
}

/* The work of (n): 16 MiB of strings, nothing keeping them. */
static int
test_lent_work(
	void *argument)
{
	struct vm_heap_stats stats;
	struct test_lend *lend;
	unsigned char bytes[1000];
	unsigned index;

	lend = argument;
	lend->borrower = vm_thread_is_borrower();
	lend->lent = vm_heap_lent(lend->heap);
	memset(bytes, 'x', sizeof(bytes));
	vm_heap_stats(lend->heap, &stats);
	lend->collections_before = stats.collections;
	for (index = 0; index < 16384U; index++) {
		if (vm_string_from_latin1(lend->heap, bytes, sizeof(bytes)) != NULL)
			lend->made++;
	}
	vm_heap_stats(lend->heap, &stats);
	lend->collections_after = stats.collections;
	return 0;
}

/* (n): a lent heap does not collect while it is lent, and collects again after. */
static void
test_lend_heap(void)
{
	struct vm_heap_stats stats;
	struct vm_agent agent;
	struct test_lend lend;
	unsigned char bytes[16];
	int error;

	memset(&lend, 0, sizeof(lend));
	error = vm_heap_create(&lend.heap, 0);
	if (error != 0) {
		test_check(0, "(n) heap", "none (%d)", error);
		return;
	}
	vm_heap_set_stack_base(lend.heap, __builtin_frame_address(0));
	vm_agent_init(&agent);
	vm_agent_set_lender(&agent, test_lender, &lend);
	vm_heap_set_agent(lend.heap, &agent);
	error = vm_heap_lend(lend.heap, test_lent_work, &lend);
	test_check(error == 0 && lend.borrower && lend.lent && lend.made == 16384U && lend.collections_after == lend.collections_before,
	    "(n) a lent heap does not collect while the work runs", "%zu strings on a %s, %zu collections while lent",
	    lend.made, lend.borrower ? "borrower" : "lender", lend.collections_after - lend.collections_before);
	memset(bytes, 'y', sizeof(bytes));
	(void)vm_string_from_latin1(lend.heap, bytes, sizeof(bytes));
	vm_heap_stats(lend.heap, &stats);
	test_check(stats.collections == lend.collections_after + 1U && vm_heap_lent(lend.heap) == 0, "(n) the lender's next allocation collects",
	    "%zu collections, %zu live cells", stats.collections - lend.collections_after, stats.live_cells);
	vm_heap_set_agent(lend.heap, NULL);
	vm_heap_destroy(lend.heap);
}

/* (o): the BUG-411 bench in both views, its lines and lends printed; the checks the same. */
static void
test_bench_both(void)
{
	static char caller_console[65536];
	struct browser_view *caller;
	struct browser_view *own;
	char path[1024];
	char root[256];
	char *line;
	char *next;
	char *caller_line;
	char *caller_next;
	long long start;
	uint64_t lends[2];
	uint64_t waited[2];
	int error;
	int same;

	if (getcwd(root, sizeof(root)) == NULL)
		return;
	snprintf(path, sizeof(path), "%s/plan/ws074/tests/bench/bug411.html", root);
	error = test_view(BROWSER_THREADING_CALLER, &caller);
	if (error != 0) {
		test_check(0, "(o) view", "none (%d)", error);
		return;
	}
	test_console_length = 0;
	test_console[0] = '\0';
	(void)browser_view_load(caller, path);
	(void)browser_view_settle(caller, 5000.0, BROWSER_SETTLE_LAYOUT);
	memcpy(caller_console, test_console, test_console_length + 1U);
	browser_view_destroy(caller);
	error = test_view(BROWSER_THREADING_OWN, &own);
	if (error != 0) {
		test_check(0, "(o) view", "none (%d)", error);
		return;
	}
	test_console_length = 0;
	test_console[0] = '\0';
	view_front_lend_counts(own, &lends[0], &waited[0]);
	(void)browser_view_load(own, path);
	start = test_now();
	while (strstr(test_console, "compile-many-names") == NULL && test_now() - start < 30000000000LL)
		test_wait(own, 20, NULL);
	view_front_lend_counts(own, &lends[1], &waited[1]);
	browser_view_destroy(own);

	/* The checks (the last word of each line) the same, the lines printed. */
	same = 1;
	line = test_console;
	caller_line = caller_console;
	while (*line != '\0' && *caller_line != '\0') {
		next = strchr(line, '\n');
		caller_next = strchr(caller_line, '\n');
		if (next == NULL || caller_next == NULL)
			break;
		*next = '\0';
		*caller_next = '\0';
		printf("  own    %s\n  caller %s\n", line, caller_line);
		if (strrchr(line, ' ') == NULL || strrchr(caller_line, ' ') == NULL || strcmp(strrchr(line, ' '), strrchr(caller_line, ' ')) != 0)
			same = 0;
		line = next + 1;
		caller_line = caller_next + 1;
	}
	test_check(same && test_console_length != 0, "(o) the bench's checks are the same in both views", "%llu lends, %.2f ms waited",
	    (unsigned long long)(lends[1] - lends[0]), (double)(waited[1] - waited[0]) / 1e6);
}

/* An empty work, counted. */
static int
test_empty_work(
	void *argument)
{
	(*(unsigned *)argument)++;
	return 0;
}

/* (n): the round trip of an empty work lent to a style thread, 10000 times. */
static void
test_lend_round_trip(void)
{
	struct view_style *style;
	unsigned count;
	long long start;
	long long took;
	unsigned index;
	int error;

	error = view_style_start(NULL, NULL, &style);
	if (error != 0) {
		test_check(0, "(n) style thread", "none (%d)", error);
		return;
	}
	count = 0;
	start = test_now();
	for (index = 0; index < 10000U; index++)
		(void)view_style_lend(style, test_empty_work, &count);
	took = test_now() - start;
	view_style_stop(style);
	test_check(count == 10000U, "(n) the round trip of an empty lend", "%.2f us", (double)took / 1e3 / 10000.0);
}

/*
 * (q): a page committed is told with its own state.  The page's load
 * handler spins 800 ms after the commit, before the script thread
 * publishes the new state, while the window's thread runs
 * browser_view_process every 2 ms: the committed callback must come only
 * with the published state (the new location and title), not with the old
 * page's.
 */
static void
test_committed_state(void)
{
	struct browser_view *view;
	const char *first;
	const char *second;
	unsigned before;
	size_t length;
	int index;
	int error;

	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(q) view", "no view (%d)", error);
		return;
	}
	first = test_page("first.html", "<!DOCTYPE html><title>First</title><p>first");
	(void)browser_view_load(view, first);
	(void)test_pump(view, 3000, "First");
	second = test_page("second.html", "<!DOCTYPE html><title>Second</title><p>second<script>"
	    "addEventListener('load', function () { var end = Date.now() + 800; while (Date.now() < end) {} });</script>");
	before = test_committed;
	(void)browser_view_load(view, second);
	for (index = 0; index < 2000 && test_committed == before; index++) {
		browser_view_process(view);
		test_sleep_ms(2);
	}
	length = strlen(test_committed_url);
	test_check(test_committed != before && length >= 11 && strcmp(test_committed_url + length - 11, "second.html") == 0 &&
	    strcmp(test_committed_title, "Second") == 0, "(q) the committed callback reads the new page's state",
	    "%u callbacks, url \"%s\", title \"%s\"", test_committed - before, test_committed_url, test_committed_title);
	browser_view_destroy(view);
}
