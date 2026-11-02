/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the iframes' threads of a view of its own threads
 * (BROWSER_THREADING_OWN): the test's main thread stands in for the
 * window's (it hands the view a pointer move and calls
 * browser_view_process every 2 ms, and times those calls).
 *
 *   (a) an iframe's timer runs on a thread of its own, named
 *       browser-script, not the page's (the CPU time of a 1.5 s spin goes
 *       to the iframe's thread), the window's calls stay under 20 ms the
 *       while, and the page's own script runs again once the spin ended;
 *   (b) a parent and its same-origin child reach each other's documents
 *       and call each other's functions as in a view of the caller's;
 *   (c) postMessage from the parent to the child and back, as in a view
 *       of the caller's;
 *   (d) while an iframe's timer runs for ever the window's calls stay
 *       under 20 ms, and a load shows the new page within 1 s;
 *   (e) an iframe that leaves its document (a listener of the page's
 *       removes it) has its thread joined within 1 s;
 *   (f) of 64 iframes, the first 63 have a window and the 64th none (64
 *       frames with the page's own), and the page goes on;
 *   (g) a promise reaction a frame queues in its parent's realm runs at
 *       the frame's checkpoint, on the frame's thread;
 *   (h) an iframe's synchronous XMLHttpRequest of a response that comes
 *       after 300 ms ends on the iframe's thread, and its fetch() too;
 *   (i) going to another page ends the iframes' threads of the page left,
 *       going back gives them threads again and their timers go on;
 *   (j) where the program's threads cannot start the iframe's, the iframe
 *       has no window and the page goes on.
 *
 *   host-frames      (run from the repository's root; prints one line per check and a summary)
 *
 * The test gives the engine the POSIX threads itself (through a table
 * that records each thread and may refuse to start one), reads the
 * threads' CPU times in /proc, and the view's count of iframe threads
 * (view_frames_count).
 */

#include <browser/browser.h>

#include "userland/tests/browser-threads/threads-posix.h"
#include "view/front.h"
#include "view/frames.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The longest a call of the window may take, in ms. */
#define TEST_WINDOW_MAX_MS	20.0

/* Where the test writes its pages. */
#define TEST_PAGES		"build/tmp/host-frames"

/* The most threads the table records. */
#define TEST_THREADS_MAX	512U

/* One thread the table started: its name and its id. */
struct test_thread {
	char name[16];
	pid_t tid;
};

/* What a thread the table starts runs first. */
struct test_start {
	void (*run)(void *argument);
	void *argument;
	char name[16];
};

/* What the stand-ins share. */
static char test_console[262144];
static size_t test_console_length;
static unsigned test_checks;
static unsigned test_failures;
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static struct test_thread test_threads[TEST_THREADS_MAX];
static unsigned test_thread_count;
static int test_refuse;
static struct browser_threads test_table;
static const struct browser_threads *test_posix;
static unsigned short test_port;

static long long test_now(void);
static void test_sleep_ms(int ms);
static void test_check(int passed, const char *name, const char *format, ...);
static void test_trampoline(void *argument);
static int test_start_thread(void *context, const char *name, size_t stack_size, void (*run)(void *argument), void *argument, void **thread);
static void test_wake(void *context);
static void test_frame_wake(void *context);
static void test_console_callback(void *context, struct browser_view *view, int level, const char *text, size_t length);
static int test_view(int threading, struct browser_view **view);
static double test_pump(struct browser_view *view, int ms, const char *title, const char *line);
static const char *test_page(const char *name, const char *body);
static int test_listen(unsigned short *port);
static void *test_serve(void *context);
static double test_cpu_ms(pid_t tid);
static int test_alive(pid_t tid);
static int test_has_line(const char *line);
static size_t test_frames(struct browser_view *view);
static void test_thread_of_timer(void);
static void test_same_both(const char *label, const char *name, const char *body, const char *expected);
static void test_forever(void);
static void test_remove(void);
static void test_many(void);
static void test_promise(void);
static void test_xhr(void);
static void test_kept(void);
static void test_refused(void);

/* Runs every check and prints the summary. */
int
main(
	void)
{
	pthread_t thread;
	int listener;
	int error;

	/* The program's threads, through the test's table. */
	test_posix = browser_threads_posix();
	test_table = *test_posix;
	test_table.start = test_start_thread;
	error = browser_threads_set(&test_table);
	if (error != 0) {
		fprintf(stderr, "host-frames: the engine refused the threads (%d)\n", error);
		return 2;
	}

	/* The pages' directory and the server. */
	setenv("XDG_DATA_HOME", "build/tmp/host-frames-data", 1);
	mkdir("build/tmp", 0755);
	mkdir(TEST_PAGES, 0755);
	listener = test_listen(&test_port);
	if (listener < 0)
		return 2;
	pthread_create(&thread, NULL, test_serve, (void *)(long)listener);

	/* The checks. */
	test_thread_of_timer();
	test_same_both("(b) same-origin access", "same.html",
	    "<!DOCTYPE html><title>t</title><script>function f(x) { return x * 3; }"
	    " window.addEventListener('load', function () { var w = frames[0];"
	    " document.title = 'r:' + w.g(2) + ':' + w.document.body.textContent + ':' + w.parentTitle + ':' + (w.parent === window); });</script>"
	    "<iframe srcdoc=\"<script>window.parentTitle = parent.document.title; function g(x) { return parent.f(x) + 1; }</script><body>kid</body>\"></iframe>",
	    "r:7:kid:t:true");
	test_same_both("(c) postMessage", "post.html",
	    "<!DOCTYPE html><title>p</title><script>window.onmessage = function (e) { document.title = 'pm:' + e.data; };"
	    " window.onload = function () { frames[0].postMessage('hi', '*'); };</script>"
	    "<iframe srcdoc=\"<script>onmessage = function (e) { parent.postMessage(e.data + '!', '*'); };</script>\"></iframe>",
	    "pm:hi!");
	test_forever();
	test_remove();
	test_many();
	test_promise();
	test_xhr();
	test_kept();
	test_refused();

	/* The summary. */
	printf("host-frames: %u checks, %u failed\n", test_checks, test_failures);
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
	if (!passed && getenv("HOST_FRAMES_CONSOLE") != NULL) {
		pthread_mutex_lock(&test_lock);
		printf("console:\n%s", test_console);
		pthread_mutex_unlock(&test_lock);
	}
	fflush(stdout);
}

/* A thread the table started: records its id and name, then runs. */
static void
test_trampoline(
	void *argument)
{
	struct test_start *start;
	void (*run)(void *argument);
	void *inner;

	start = argument;
	pthread_mutex_lock(&test_lock);
	if (test_thread_count < TEST_THREADS_MAX) {
		snprintf(test_threads[test_thread_count].name, sizeof(test_threads[0].name), "%s", start->name);
		test_threads[test_thread_count].tid = (pid_t)syscall(SYS_gettid);
		test_thread_count++;
	}
	pthread_mutex_unlock(&test_lock);
	run = start->run;
	inner = start->argument;
	free(start);
	run(inner);
}

/* The table's start: the POSIX one through the trampoline, or a refusal of a script thread while the test asks for one. */
static int
test_start_thread(
	void *context,
	const char *name,
	size_t stack_size,
	void (*run)(void *argument),
	void *argument,
	void **thread)
{
	struct test_start *start;
	int error;

	if (__atomic_load_n(&test_refuse, __ATOMIC_ACQUIRE) && strcmp(name, "browser-script") == 0)
		return EAGAIN;
	start = calloc(1, sizeof(*start));
	if (start == NULL)
		return ENOMEM;
	start->run = run;
	start->argument = argument;
	snprintf(start->name, sizeof(start->name), "%s", name);
	error = test_posix->start(context, name, stack_size, test_trampoline, start, thread);
	if (error != 0)
		free(start);
	return error;
}

/* The view's wake: the window's loop polls, so nothing is needed. */
static void
test_wake(
	void *context)
{
	(void)context;
}

/* A new committed frame: nobody draws in this test. */
static void
test_frame_wake(
	void *context)
{
	(void)context;
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
	pthread_mutex_lock(&test_lock);
	if (test_console_length + length + 2U < sizeof(test_console)) {
		memcpy(test_console + test_console_length, text, length);
		test_console_length += length;
		test_console[test_console_length++] = '\n';
		test_console[test_console_length] = '\0';
	}
	pthread_mutex_unlock(&test_lock);
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
	callbacks.console = test_console_callback;
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
 * The window's loop for so many milliseconds, or until the page shown has
 * the title or the console has the line (when given): a pointer move and
 * browser_view_process every 2 ms (a view of the caller's waits for its
 * timeout too).  Returns the longest round of the window's calls, in ms.
 */
static double
test_pump(
	struct browser_view *view,
	int ms,
	const char *title,
	const char *line)
{
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
		took = test_now() - start;
		if (took > longest)
			longest = took;
		if (title != NULL) {
			shown = browser_view_title(view);
			if (shown != NULL && strcmp(shown, title) == 0)
				break;
		}
		if (line != NULL && test_has_line(line))
			break;
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

/* Listens on a free port of 127.0.0.1; returns the socket or -1. */
static int
test_listen(
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
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 64) != 0)
		return -1;
	length = sizeof(address);
	getsockname(listener, (struct sockaddr *)&address, &length);
	*port = ntohs(address.sin_port);
	return listener;
}

/* The server: /p/NAME is the test's page NAME, /slow.txt comes after 300 ms, anything else is "data". One request per connection. */
static void *
test_serve(
	void *context)
{
	char request[2048];
	char header[256];
	char path[2600];
	char *body;
	char *at;
	const char *type;
	FILE *file;
	size_t length;
	ssize_t got;
	int listener;
	int client;

	listener = (int)(long)context;
	body = malloc(65536);
	if (body == NULL)
		return NULL;
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
		type = "text/plain";
		snprintf(body, 65536, "data");
		if (strncmp(request, "GET /p/", 7) == 0) {
			at = strchr(request + 7, ' ');
			if (at != NULL)
				*at = '\0';
			snprintf(path, sizeof(path), "%s/%s", TEST_PAGES, request + 7);
			file = fopen(path, "r");
			if (file != NULL) {
				length = fread(body, 1, 65535, file);
				body[length] = '\0';
				fclose(file);
			}
			type = "text/html";
		} else if (strncmp(request, "GET /slow.txt", 13) == 0) {
			test_sleep_ms(300);
			snprintf(body, 65536, "slow");
		}
		snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		    type, strlen(body));
		(void)write(client, header, strlen(header));
		(void)write(client, body, strlen(body));
		close(client);
	}
	return NULL;
}

/* The CPU time a thread of the process took (user and system), in ms (-1 when it is gone). */
static double
test_cpu_ms(
	pid_t tid)
{
	char path[64];
	char line[1024];
	unsigned long user;
	unsigned long system;
	FILE *file;
	char *at;

	snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)tid);
	file = fopen(path, "r");
	if (file == NULL)
		return -1.0;
	if (fgets(line, sizeof(line), file) == NULL) {
		fclose(file);
		return -1.0;
	}
	fclose(file);
	at = strrchr(line, ')');
	if (at == NULL || sscanf(at + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &user, &system) != 2)
		return -1.0;
	return (double)(user + system) * 1000.0 / (double)sysconf(_SC_CLK_TCK);
}

/* Whether a thread of the process is still there. */
static int
test_alive(
	pid_t tid)
{
	char path[64];
	struct stat status;

	snprintf(path, sizeof(path), "/proc/self/task/%d", (int)tid);
	return stat(path, &status) == 0;
}

/* Whether the console has a line. */
static int
test_has_line(
	const char *line)
{
	char wanted[256];
	int found;

	snprintf(wanted, sizeof(wanted), "%s\n", line);
	pthread_mutex_lock(&test_lock);
	found = strstr(test_console, wanted) != NULL;
	pthread_mutex_unlock(&test_lock);
	return found;
}

/* How many iframes of a view of its own threads have a thread now. */
static size_t
test_frames(
	struct browser_view *view)
{
	return view_frames_count(view_front_frames(view_front_of(view)));
}

/* (a): an iframe's timer on its own thread; the window's calls and the page's own script meanwhile and after. */
static void
test_thread_of_timer(void)
{
	struct browser_view *view;
	const char *path;
	unsigned first;
	unsigned index;
	pid_t top;
	pid_t frame;
	double top_before;
	double frame_before;
	double top_spent;
	double frame_spent;
	double longest;
	char *after;
	int ticks;
	int error;

	pthread_mutex_lock(&test_lock);
	first = test_thread_count;
	test_console_length = 0;
	test_console[0] = '\0';
	pthread_mutex_unlock(&test_lock);
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(a) view", "no view (%d)", error);
		return;
	}
	path = test_page("spin.html", "<!DOCTYPE html><title>spin</title><script>var n = 0;"
	    " setInterval(function () { n++; console.log('tick ' + n); }, 50);</script>"
	    "<iframe srcdoc=\"<script>setTimeout(function () { console.log('spin begins'); var t = Date.now(); while (Date.now() - t < 1500) {}"
	    " console.log('spun'); }, 300);</script>\"></iframe>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, NULL, "spin begins");

	/* The page's script thread is the first browser-script the view started, the iframe's the next. */
	top = 0;
	frame = 0;
	pthread_mutex_lock(&test_lock);
	for (index = first; index < test_thread_count; index++) {
		if (strcmp(test_threads[index].name, "browser-script") != 0)
			continue;
		if (top == 0)
			top = test_threads[index].tid;
		else if (frame == 0)
			frame = test_threads[index].tid;
	}
	pthread_mutex_unlock(&test_lock);
	top_before = test_cpu_ms(top);
	frame_before = test_cpu_ms(frame);
	longest = test_pump(view, 3000, NULL, "spun");
	top_spent = test_cpu_ms(top) - top_before;
	frame_spent = test_cpu_ms(frame) - frame_before;
	test_check(frame != 0 && frame != top && test_frames(view) == 1U, "(a) the iframe has a browser-script thread of its own", "page %d, iframe %d, %zu iframe threads",
	    (int)top, (int)frame, test_frames(view));
	test_check(frame_spent >= 1000.0 && top_spent < 300.0, "(a) the iframe's timer spins on the iframe's thread", "iframe's thread %.0f ms, page's %.0f ms",
	    frame_spent, top_spent);
	test_check(longest <= TEST_WINDOW_MAX_MS, "(a) window calls while the iframe's timer spins 1.5 s", "longest %.2f ms", longest);

	/* The page's own interval goes on after the spin. */
	(void)test_pump(view, 600, NULL, NULL);
	ticks = 0;
	pthread_mutex_lock(&test_lock);
	after = strstr(test_console, "spun\n");
	while (after != NULL && (after = strstr(after + 1, "tick ")) != NULL)
		ticks++;
	pthread_mutex_unlock(&test_lock);
	test_check(ticks >= 5, "(a) the page's script runs again once the iframe's task ended", "%d ticks in 600 ms after it", ticks);
	browser_view_destroy(view);
}

/* (b), (c): a page whose title its frames' scripts make, in both views. */
static void
test_same_both(
	const char *label,
	const char *name,
	const char *body,
	const char *expected)
{
	struct browser_view *view;
	char titles[2][256];
	const char *path;
	const char *title;
	int mode;
	int error;

	path = test_page(name, body);
	for (mode = 0; mode < 2; mode++) {
		titles[mode][0] = '\0';
		error = test_view(mode == 0 ? BROWSER_THREADING_CALLER : BROWSER_THREADING_OWN, &view);
		if (error != 0)
			continue;
		(void)browser_view_load(view, path);
		(void)test_pump(view, 3000, expected, NULL);
		title = browser_view_title(view);
		snprintf(titles[mode], sizeof(titles[mode]), "%s", title == NULL ? "" : title);
		browser_view_destroy(view);
	}
	test_check(strcmp(titles[0], expected) == 0 && strcmp(titles[1], expected) == 0, label, "caller \"%s\", own \"%s\"", titles[0], titles[1]);
}

/* (d): an iframe's timer that runs for ever, and a load out of it. */
static void
test_forever(void)
{
	struct browser_view *view;
	const char *path;
	const char *title;
	long long begin;
	double longest;
	double took;
	int error;

	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(d) view", "no view (%d)", error);
		return;
	}
	test_console_length = 0;
	test_console[0] = '\0';
	path = test_page("forever.html", "<!DOCTYPE html><title>forever</title>"
	    "<iframe srcdoc=\"<script>setTimeout(function () { console.log('loops'); for (;;) {} }, 100);</script>\"></iframe>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, NULL, "loops");
	longest = test_pump(view, 2000, NULL, NULL);
	test_check(test_has_line("loops") && longest <= TEST_WINDOW_MAX_MS, "(d) window calls while an iframe's timer runs for ever", "longest %.2f ms", longest);
	path = test_page("after.html", "<!DOCTYPE html><title>after</title><p>after");
	begin = test_now();
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, "after", NULL);
	took = (double)(test_now() - begin) / 1e6;
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "after") == 0 && took < 1000.0 && test_frames(view) == 0U, "(d) a load out of it shows the new page",
	    "%.0f ms, \"%s\", %zu iframe threads", took, title == NULL ? "" : title, test_frames(view));
	browser_view_destroy(view);
}

/* (e): an iframe removed has its thread joined. */
static void
test_remove(void)
{
	struct browser_view *view;
	const char *path;
	unsigned first;
	unsigned index;
	pid_t frame;
	long long begin;
	size_t before;
	double took;
	int error;

	pthread_mutex_lock(&test_lock);
	first = test_thread_count;
	test_console_length = 0;
	test_console[0] = '\0';
	pthread_mutex_unlock(&test_lock);
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(e) view", "no view (%d)", error);
		return;
	}
	path = test_page("remove.html", "<!DOCTYPE html><title>remove</title>"
	    "<iframe srcdoc=\"<script>setInterval(function () { console.log('child'); }, 50);</script>\"></iframe>"
	    "<script>document.addEventListener('mousedown', function () { document.querySelector('iframe').remove(); console.log('removed'); });</script>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, NULL, "child");
	before = test_frames(view);
	frame = 0;
	pthread_mutex_lock(&test_lock);
	for (index = first; index < test_thread_count; index++) {
		if (strcmp(test_threads[index].name, "browser-script") == 0)
			frame = test_threads[index].tid;
	}
	pthread_mutex_unlock(&test_lock);
	(void)browser_view_pointer_button(view, 10.0f, 590.0f, 0, 1, 0);
	(void)browser_view_pointer_button(view, 10.0f, 590.0f, 0, 0, 0);
	(void)test_pump(view, 3000, NULL, "removed");
	begin = test_now();
	while (test_now() - begin < 1000000000LL && (test_frames(view) != 0U || test_alive(frame))) {
		browser_view_process(view);
		test_sleep_ms(2);
	}
	took = (double)(test_now() - begin) / 1e6;
	test_check(before == 1U && test_frames(view) == 0U && !test_alive(frame), "(e) a removed iframe's thread is joined within 1 s",
	    "%zu then %zu iframe threads, the thread gone after %.0f ms", before, test_frames(view), took);
	browser_view_destroy(view);
}

/* (f): the 64th iframe gets no window. */
static void
test_many(void)
{
	static char page[16384];
	struct browser_view *view;
	const char *path;
	const char *title;
	long long begin;
	double caller_ms;
	double own_ms;
	double longest;
	size_t length;
	int index;
	int error;

	length = (size_t)snprintf(page, sizeof(page), "<!DOCTYPE html><title>many</title><script>window.onload = function () { var f = document.querySelectorAll('iframe'), i, first = -1, n = 0;"
	    " for (i = 0; i < f.length; i++) { if (f[i].contentWindow === null) { n++; if (first < 0) first = i; } }"
	    " document.title = 'nulls ' + n + ' first ' + first; };</script>");
	for (index = 0; index < 64; index++)
		length += (size_t)snprintf(page + length, sizeof(page) - length, "<iframe srcdoc=\"<p>%d\"></iframe>", index);
	path = test_page("many.html", page);

	/* A view of the caller's has no bound (for the time it takes). */
	error = test_view(BROWSER_THREADING_CALLER, &view);
	if (error != 0) {
		test_check(0, "(f) view", "no view (%d)", error);
		return;
	}
	begin = test_now();
	(void)browser_view_load(view, path);
	(void)test_pump(view, 120000, "nulls 0 first -1", NULL);
	caller_ms = (double)(test_now() - begin) / 1e6;
	browser_view_destroy(view);

	/* A view of its own threads. */
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(f) view", "no view (%d)", error);
		return;
	}
	begin = test_now();
	(void)browser_view_load(view, path);
	longest = test_pump(view, 120000, "nulls 1 first 63", NULL);
	own_ms = (double)(test_now() - begin) / 1e6;
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "nulls 1 first 63") == 0 && test_frames(view) == 63U && longest <= TEST_WINDOW_MAX_MS,
	    "(f) of 64 iframes the 64th gets no window", "\"%s\", %zu iframe threads, loaded in %.0f ms (a view of the caller's %.0f ms), window calls at most %.2f ms",
	    title == NULL ? "" : title, test_frames(view), own_ms, caller_ms, longest);
	browser_view_destroy(view);
}

/* (g): a reaction a frame queues in its parent's realm runs at the frame's checkpoint, on its thread. */
static void
test_promise(void)
{
	struct browser_view *view;
	const char *path;
	unsigned first;
	unsigned index;
	pid_t top;
	pid_t frame;
	double top_before;
	double frame_before;
	double top_spent;
	double frame_spent;
	int error;

	pthread_mutex_lock(&test_lock);
	first = test_thread_count;
	test_console_length = 0;
	test_console[0] = '\0';
	pthread_mutex_unlock(&test_lock);
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(g) view", "no view (%d)", error);
		return;
	}
	path = test_page("promise.html", "<!DOCTYPE html><title>promise</title><script>"
	    "function queueSpin() { Promise.resolve().then(function () { var t = Date.now(); while (Date.now() - t < 800) {} console.log('job ran'); }); }</script>"
	    "<iframe srcdoc=\"<script>setTimeout(function () { console.log('queues'); parent.queueSpin(); }, 300);</script>\"></iframe>");
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, NULL, "queues");
	top = 0;
	frame = 0;
	pthread_mutex_lock(&test_lock);
	for (index = first; index < test_thread_count; index++) {
		if (strcmp(test_threads[index].name, "browser-script") != 0)
			continue;
		if (top == 0)
			top = test_threads[index].tid;
		else if (frame == 0)
			frame = test_threads[index].tid;
	}
	pthread_mutex_unlock(&test_lock);
	top_before = test_cpu_ms(top);
	frame_before = test_cpu_ms(frame);
	(void)test_pump(view, 3000, NULL, "job ran");
	top_spent = test_cpu_ms(top) - top_before;
	frame_spent = test_cpu_ms(frame) - frame_before;
	test_check(test_has_line("job ran") && frame_spent >= 500.0 && top_spent < 300.0, "(g) a reaction queued in the parent's realm runs on the frame's thread",
	    "iframe's thread %.0f ms, page's %.0f ms", frame_spent, top_spent);
	browser_view_destroy(view);
}

/* (h): an iframe's synchronous XMLHttpRequest and fetch(). */
static void
test_xhr(void)
{
	char page[2048];
	char location[256];
	struct browser_view *view;
	const char *title;
	int error;

	test_console_length = 0;
	test_console[0] = '\0';
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>xhr</title>"
	    "<iframe srcdoc=\"<script>setTimeout(function () { var x = new XMLHttpRequest(); x.open('GET', 'http://127.0.0.1:%u/slow.txt', false); x.send();"
	    " parent.document.title = 'sync ' + x.status + ' ' + x.responseText;"
	    " fetch('http://127.0.0.1:%u/data.txt').then(function (r) { return r.text(); }).then(function (t) { console.log('fetched ' + t); }); }, 100);</script>\"></iframe>",
	    (unsigned)test_port, (unsigned)test_port);
	(void)test_page("xhr.html", page);
	snprintf(location, sizeof(location), "http://127.0.0.1:%u/p/xhr.html", (unsigned)test_port);
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(h) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, location);
	(void)test_pump(view, 5000, "sync 200 slow", NULL);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "sync 200 slow") == 0, "(h) an iframe's synchronous XMLHttpRequest", "\"%s\"", title == NULL ? "" : title);
	(void)test_pump(view, 3000, NULL, "fetched data");
	test_check(test_has_line("fetched data"), "(h) an iframe's fetch()", "%s", test_has_line("fetched data") ? "fetched" : "no answer");
	browser_view_destroy(view);
}

/* (i): the iframes' threads of a page kept for going back. */
static void
test_kept(void)
{
	struct browser_view *view;
	const char *kept;
	const char *other;
	const char *title;
	size_t shown;
	size_t left;
	size_t back;
	size_t lines;
	int error;

	test_console_length = 0;
	test_console[0] = '\0';
	kept = test_page("kept.html", "<!DOCTYPE html><title>kept</title>"
	    "<iframe srcdoc=\"<script>var n = 0; setInterval(function () { n++; console.log('k' + n); }, 100);</script>\"></iframe>");
	other = test_page("other.html", "<!DOCTYPE html><title>other</title><p>other");
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(i) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, kept);
	(void)test_pump(view, 3000, NULL, "k3");
	shown = test_frames(view);
	(void)browser_view_load(view, other);
	(void)test_pump(view, 3000, "other", NULL);
	(void)test_pump(view, 200, NULL, NULL);
	left = test_frames(view);
	pthread_mutex_lock(&test_lock);
	lines = test_console_length;
	pthread_mutex_unlock(&test_lock);
	(void)test_pump(view, 400, NULL, NULL);
	pthread_mutex_lock(&test_lock);
	lines = test_console_length - lines;
	pthread_mutex_unlock(&test_lock);
	(void)browser_view_go(view, -1);
	(void)test_pump(view, 3000, "kept", NULL);
	title = browser_view_title(view);
	back = test_frames(view);
	(void)test_pump(view, 3000, NULL, "k6");
	test_check(shown == 1U && left == 0U && lines == 0U && back == 1U && title != NULL && strcmp(title, "kept") == 0 && test_has_line("k6"),
	    "(i) a kept page's iframe ends its thread and gets one again", "%zu, %zu (console %zu bytes meanwhile), back %zu \"%s\", k6 %s", shown, left, lines,
	    back, title == NULL ? "" : title, test_has_line("k6") ? "yes" : "no");
	browser_view_destroy(view);
}

/* (j): an iframe whose thread cannot start. */
static void
test_refused(void)
{
	struct browser_view *view;
	const char *path;
	const char *title;
	int error;

	path = test_page("refused.html", "<!DOCTYPE html><title>refused</title><script>window.onload = function () {"
	    " document.title = 'window ' + (document.querySelector('iframe').contentWindow === null ? 'none' : 'some'); };</script>"
	    "<iframe srcdoc=\"<p>x\"></iframe>");
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(j) view", "no view (%d)", error);
		return;
	}
	__atomic_store_n(&test_refuse, 1, __ATOMIC_RELEASE);
	(void)browser_view_load(view, path);
	(void)test_pump(view, 3000, "window none", NULL);
	__atomic_store_n(&test_refuse, 0, __ATOMIC_RELEASE);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "window none") == 0 && test_frames(view) == 0U, "(j) an iframe whose thread cannot start has no window",
	    "\"%s\"", title == NULL ? "" : title);
	browser_view_destroy(view);
}
