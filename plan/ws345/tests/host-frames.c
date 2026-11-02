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
 *       has no window and the page goes on;
 *   (k) an iframe of another origin (the server's second port) runs in an
 *       agent of its own: while its timer runs for ever, the page's own
 *       interval goes on, on another thread, and the window's calls stay
 *       under 20 ms;
 *   (l) an iframe of the page's origin stays in the page's agent (its
 *       document reached at once);
 *   (m) postMessage both ways between agents: the values (Array, plain
 *       object, Map, Set, Date, RegExp, -0, an ArrayBuffer transferred and
 *       detached on the sender's side) arrive equal, the source and origin
 *       are the sender's, a MessagePort in the transfer list is a
 *       DataCloneError;
 *   (n) frames.length, parent, top, closed and the SecurityError of the
 *       document across agents, both ways;
 *   (o) setting an iframe's location across agents navigates it, a frame's
 *       top.location navigates the page, and a navigation back to the
 *       page's origin puts the frame in the page's agent again;
 *   (p) the page with a frame of another agent is drawn (CPU and GPU) as a
 *       view of the caller's draws it, the frame's own scene in its frame
 *       item, and its later change is drawn without the page committing;
 *   (q) a press and a key over the frame of another agent go to that
 *       frame's document, not the page's;
 *   (r) the frame of another agent ends when its iframe leaves the
 *       document and when its page is kept for going back (and loads again
 *       when the page is shown again).
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
#include "view/remote.h"

#include <dlfcn.h>
#include <vulkan/vulkan.h>

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
static unsigned short test_port_other;

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
static const char *test_url(unsigned short port, const char *name);
static size_t test_remotes(struct browser_view *view);
static void test_cross_loop(void);
static void test_cross_same(void);
static void test_cross_post(void);
static void test_cross_relatives(void);
static void test_cross_navigate(void);
static void test_cross_pixels(void);
static void test_cross_input(void);
static void test_cross_end(void);
static int test_gpu_render(struct browser_view *view, int own, uint32_t *pixels);

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

	/* lavapipe stays loaded to the end (ThreadSanitizer stops when a library its suppressions name is unloaded). */
	(void)dlopen("libvulkan_lvp.so", RTLD_NOW | RTLD_NODELETE);

	/* The pages' directory and the server, on two ports (two origins). */
	setenv("XDG_DATA_HOME", "build/tmp/host-frames-data", 1);
	mkdir("build/tmp", 0755);
	mkdir(TEST_PAGES, 0755);
	listener = test_listen(&test_port);
	if (listener < 0)
		return 2;
	pthread_create(&thread, NULL, test_serve, (void *)(long)listener);
	listener = test_listen(&test_port_other);
	if (listener < 0)
		return 2;
	pthread_create(&thread, NULL, test_serve, (void *)(long)listener);

	/* The checks (HOST_FRAMES_CROSS_ONLY: only those of the frames of other agents). */
	if (getenv("HOST_FRAMES_CROSS_ONLY") != NULL)
		goto cross;
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
cross:
	{
		test_cross_loop();
		test_cross_same();
		test_cross_post();
		test_cross_relatives();
		test_cross_navigate();
		test_cross_pixels();
		test_cross_input();
		test_cross_end();
	}

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

/* The test server's URL of a page on a port (a static buffer of its own per call, up to 16). */
static const char *
test_url(
	unsigned short port,
	const char *name)
{
	static char urls[16][256];
	static unsigned used;
	char *url;

	url = urls[used++ % 16U];
	snprintf(url, 256, "http://127.0.0.1:%u/p/%s", (unsigned)port, name);
	return url;
}

/* How many frames of other agents a view of its own threads has now. */
static size_t
test_remotes(
	struct browser_view *view)
{
	return view_remotes_count(view_frames_remotes(view_front_frames(view_front_of(view))));
}

/* (k): an iframe of another origin loops for ever while the page's own interval goes on. */
static void
test_cross_loop(void)
{
	char page[1024];
	struct browser_view *view;
	unsigned first;
	unsigned index;
	pid_t top;
	pid_t frame;
	double frame_before;
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
	(void)test_page("xloop.html", "<!DOCTYPE html><title>loop</title><script>setTimeout(function () { console.log('xloop begins'); for (;;) {} }, 200);</script>");
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>xtop</title><script>var n = 0; setInterval(function () { n++; console.log('xtick ' + n); }, 50);</script>"
	    "<iframe src='%s'></iframe>", test_url(test_port_other, "xloop.html"));
	(void)test_page("xtop.html", page);
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(k) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, test_url(test_port, "xtop.html"));
	(void)test_pump(view, 5000, NULL, "xloop begins");

	/* The loop's thread: the last browser-script started (the page's own first, the iframe's own of the page's agent, then the frame's). */
	top = 0;
	frame = 0;
	pthread_mutex_lock(&test_lock);
	for (index = first; index < test_thread_count; index++) {
		if (strcmp(test_threads[index].name, "browser-script") != 0)
			continue;
		if (top == 0)
			top = test_threads[index].tid;
		frame = test_threads[index].tid;
	}
	pthread_mutex_unlock(&test_lock);
	frame_before = test_cpu_ms(frame);
	longest = test_pump(view, 1000, NULL, NULL);
	frame_spent = test_cpu_ms(frame) - frame_before;
	ticks = 0;
	pthread_mutex_lock(&test_lock);
	after = strstr(test_console, "xloop begins\n");
	while (after != NULL && (after = strstr(after + 1, "xtick ")) != NULL)
		ticks++;
	pthread_mutex_unlock(&test_lock);
	test_check(test_has_line("xloop begins") && test_remotes(view) == 1U && frame != top && frame_spent >= 700.0,
	    "(k) the iframe of another origin loops on a thread of its own", "%zu frames of other agents, its thread %.0f ms of 1000", test_remotes(view), frame_spent);
	test_check(ticks >= 12, "(k) the page's script runs on during the other agent's loop", "%d ticks of the page's 50 ms interval in 1000 ms", ticks);
	test_check(longest <= TEST_WINDOW_MAX_MS, "(k) window calls during the other agent's loop", "longest %.2f ms", longest);
	browser_view_destroy(view);
}

/* (l): an iframe of the page's own origin stays in its agent. */
static void
test_cross_same(void)
{
	char page[1024];
	struct browser_view *view;
	const char *title;
	int error;

	(void)test_page("same-kid.html", "<!DOCTYPE html><title>same kid</title><body>kid");
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>same</title><script>window.onload = function () {"
	    " document.title = 'reach ' + frames[0].document.title + ' ' + document.querySelector('iframe').contentDocument.body.textContent; };</script>"
	    "<iframe src='%s'></iframe>", test_url(test_port, "same-kid.html"));
	(void)test_page("same-top.html", page);
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(l) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, test_url(test_port, "same-top.html"));
	(void)test_pump(view, 5000, "reach same kid kid", NULL);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "reach same kid kid") == 0 && test_remotes(view) == 0U, "(l) an iframe of the page's origin stays in its agent",
	    "\"%s\", %zu frames of other agents", title == NULL ? "" : title, test_remotes(view));
	browser_view_destroy(view);
}

/* (m): postMessage both ways between agents. */
static void
test_cross_post(void)
{
	char page[4096];
	char kid[4096];
	char expected[256];
	struct browser_view *view;
	const char *title;
	int error;

	snprintf(kid, sizeof(kid), "<!DOCTYPE html><title>kid</title><script>onmessage = function (e) { var v = e.data, ok;"
	    " ok = Array.isArray(v.a) && v.a[1] === 'two' && v.a[2].x === null && v.m instanceof Map && v.m.get('k')[1] === 2 &&"
	    " v.d instanceof Date && v.d.getTime() === 5 && v.b instanceof ArrayBuffer && v.b.byteLength === 8 && new Uint8Array(v.b)[0] === 7 &&"
	    " v.s instanceof Set && v.s.has(3) && v.r instanceof RegExp && v.r.source === 'a+' && v.r.flags === 'g' && Object.is(v.n, -0) && v.c === v.a &&"
	    " e.origin === 'http://127.0.0.1:%u' && e.source === parent && e.isTrusted;"
	    " parent.postMessage({ ok: ok ? 'ok' : 'bad', back: new Map([[1, [true]]]) }, 'http://127.0.0.1:%u'); };</script>",
	    (unsigned)test_port, (unsigned)test_port);
	(void)test_page("post-kid.html", kid);
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>post</title><script>var sent = -1, dce = 'none';"
	    " onmessage = function (e) { document.title = 'pm ' + e.data.ok + ' ' + (e.data.back.get(1)[0] === true) + ' ' + (e.source === frames[0]) + ' ' +"
	    " (e.origin === 'http://127.0.0.1:%u') + ' ' + sent + ' ' + dce; };"
	    " window.onload = function () { var b = new ArrayBuffer(8), a = [1, 'two', { x: null }]; new Uint8Array(b)[0] = 7;"
	    " try { frames[0].postMessage(1, '*', [new MessageChannel().port1]); } catch (e) { dce = e.name; }"
	    " frames[0].postMessage({ a: a, c: a, m: new Map([['k', [1, 2]]]), d: new Date(5), b: b, s: new Set([3]), r: /a+/g, n: -0 }, '*', [b]);"
	    " sent = b.byteLength; };</script><iframe src='%s'></iframe>", (unsigned)test_port_other, test_url(test_port_other, "post-kid.html"));
	(void)test_page("post-top.html", page);
	snprintf(expected, sizeof(expected), "pm ok true true true 0 DataCloneError");
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(m) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, test_url(test_port, "post-top.html"));
	(void)test_pump(view, 5000, expected, NULL);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, expected) == 0 && test_remotes(view) == 1U, "(m) postMessage both ways between agents",
	    "\"%s\"", title == NULL ? "" : title);
	browser_view_destroy(view);
}

/* (n): the relatives across agents and the SecurityError, both ways. */
static void
test_cross_relatives(void)
{
	char page[4096];
	char kid[4096];
	struct browser_view *view;
	const char *title;
	const char *expected;
	int error;

	snprintf(kid, sizeof(kid), "<!DOCTYPE html><title>kid</title><iframe srcdoc='x'></iframe><script>onload = function () { var d = 'none';"
	    " try { parent.document; } catch (e) { d = e.name; }"
	    " parent.postMessage([parent === top, parent.length, window.length, top.closed, d, window.frameElement === null].join(','), '*'); };</script>");
	(void)test_page("rel-kid.html", kid);
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>rel</title><script>"
	    " onmessage = function (e) { var w = frames[0], d = 'none';"
	    " try { w.document; } catch (x) { d = x.name; }"
	    " document.title = e.data + '|' + [frames.length, w.parent === window, w.top === window, w.window === w, w.frames === w, w.length, w.closed, d,"
	    " document.querySelector('iframe').contentDocument === null].join(','); };</script><iframe src='%s'></iframe>",
	    test_url(test_port_other, "rel-kid.html"));
	(void)test_page("rel-top.html", page);
	expected = "true,1,1,false,SecurityError,true|1,true,true,true,true,1,false,SecurityError,true";
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(n) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, test_url(test_port, "rel-top.html"));
	(void)test_pump(view, 5000, expected, NULL);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, expected) == 0, "(n) frames, parent, top, closed and SecurityError across agents", "\"%s\"",
	    title == NULL ? "" : title);
	browser_view_destroy(view);
}

/* (o): navigations across agents, and back into the page's agent. */
static void
test_cross_navigate(void)
{
	char page[4096];
	struct browser_view *view;
	const char *title;
	int error;

	/* The page sets the frame's location: the frame's next document tells it. */
	(void)test_page("nav-first.html", "<!DOCTYPE html><title>first</title><script>onload = function () { parent.postMessage('first', '*'); };</script>");
	(void)test_page("nav-second.html", "<!DOCTYPE html><title>second</title><script>onload = function () { parent.postMessage('second', '*'); };</script>");
	(void)test_page("nav-home.html", "<!DOCTYPE html><title>home kid</title><body>home");
	(void)test_page("nav-landed.html", "<!DOCTYPE html><title>landed</title><p>landed");
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>nav</title><script>var seen = [];"
	    " onmessage = function (e) { seen.push(e.data); if (e.data === 'first') frames[0].location = '%s';"
	    " if (e.data === 'second') { frames[0].location.href = '%s'; setTimeout(function check() {"
	    " try { document.title = 'back ' + frames[0].document.title + ' ' + seen.join(' '); } catch (x) { setTimeout(check, 20); } }, 20); } };</script>"
	    "<iframe src='%s'></iframe>", test_url(test_port_other, "nav-second.html"), test_url(test_port, "nav-home.html"), test_url(test_port_other, "nav-first.html"));
	(void)test_page("nav-top.html", page);
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(o) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, test_url(test_port, "nav-top.html"));
	(void)test_pump(view, 8000, "back home kid first second", NULL);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "back home kid first second") == 0 && test_remotes(view) == 0U,
	    "(o) the page navigates the frame of another agent, and back into its own agent", "\"%s\", %zu frames of other agents", title == NULL ? "" : title,
	    test_remotes(view));

	/* The frame sets its top's location: the page goes. */
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>kid</title><script>onload = function () { top.location = '%s'; };</script>",
	    test_url(test_port, "nav-landed.html"));
	(void)test_page("nav-up-kid.html", page);
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>up</title><iframe src='%s'></iframe>", test_url(test_port_other, "nav-up-kid.html"));
	(void)test_page("nav-up.html", page);
	(void)browser_view_load(view, test_url(test_port, "nav-up.html"));
	(void)test_pump(view, 8000, "landed", NULL);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "landed") == 0 && test_remotes(view) == 0U, "(o) the frame of another agent navigates its top",
	    "\"%s\", %zu frames of other agents", title == NULL ? "" : title, test_remotes(view));
	browser_view_destroy(view);
}

/* (p): the frame item drawn as a view of the caller's draws the page, CPU and GPU, and a change of the frame alone. */
static void
test_cross_pixels(void)
{
	static uint32_t expected[800U * 600U];
	static uint32_t drawn[800U * 600U];
	static uint32_t gpu_expected[800U * 600U];
	static uint32_t gpu_drawn[800U * 600U];
	char page[4096];
	struct browser_view *caller;
	struct browser_view *own;
	struct view_commit *commit;
	uint64_t serial;
	uint64_t later;
	long long start;
	size_t differ;
	size_t index;
	int blue;
	int caller_gpu;
	int own_gpu;
	int same;
	int error;

	(void)test_page("pix-kid.html", "<!DOCTYPE html><title>kid</title><style>body { margin: 4px; background: #fed; font: 16px sans-serif }"
	    " div { width: 60px; height: 20px; background: #c33 }</style><div id='d'></div><p>Frame text"
	    "<script>onmessage = function () { document.getElementById('d').style.background = '#33c'; };</script>");
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>pix</title><style>body { background: #eef } iframe { width: 300px; height: 120px; border: 3px solid #333 }</style>"
	    "<script>addEventListener('keydown', function () { frames[0].postMessage('blue', '*'); });</script>"
	    "<h1>Page</h1><iframe src='%s'></iframe><p>after the frame", test_url(test_port_other, "pix-kid.html"));
	(void)test_page("pix-top.html", page);
	error = test_view(BROWSER_THREADING_CALLER, &caller);
	if (error == 0)
		error = test_view(BROWSER_THREADING_OWN, &own);
	if (error != 0) {
		test_check(0, "(p) views", "none (%d)", error);
		return;
	}
	(void)browser_view_load(caller, test_url(test_port, "pix-top.html"));
	(void)browser_view_settle(caller, 5000.0, BROWSER_SETTLE_LAYOUT);
	error = browser_view_draw_pixels(caller, expected, 800U, 600U, 800U * sizeof(uint32_t));
	caller_gpu = test_gpu_render(caller, 0, gpu_expected);
	(void)browser_view_load(own, test_url(test_port, "pix-top.html"));

	/* The CPU's picture of the latest committed frame with the frame's scene, until it is the caller's. */
	same = 0;
	differ = 0;
	start = test_now();
	while (!same && test_now() - start < 8000000000LL) {
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
	test_check(error == 0 && same && test_remotes(own) == 1U, "(p) the CPU draws the frame item as the caller's view draws the page", "%zu pixels differ, %zu frames of other agents",
	    differ, test_remotes(own));
	own_gpu = test_gpu_render(own, 1, gpu_drawn);
	if (caller_gpu == 77 || own_gpu == 77) {
		printf("SKIP (p) GPU: no Vulkan device\n");
	} else {
		differ = 0;
		for (index = 0; index < 800U * 600U; index++)
			differ += gpu_drawn[index] != gpu_expected[index];
		test_check(caller_gpu == 0 && own_gpu == 0 && differ == 0, "(p) the GPU draws the frame item as the caller's view draws the page",
		    "caller %d, own %d, %zu pixels differ", caller_gpu, own_gpu, differ);
	}

	/* The frame alone changes (a key of the page's posts it a message): its new scene is drawn in the same committed frame of the page. */
	commit = view_front_commit(own);
	serial = commit != NULL ? commit->serial : 0;
	if (commit != NULL)
		view_commit_release(commit);
	(void)browser_view_key(own, "b", "KeyB", "b", 1, 0, 0);
	(void)browser_view_key(own, "b", "KeyB", "b", 0, 0, 0);
	blue = 0;
	start = test_now();
	while (!blue && test_now() - start < 5000000000LL) {
		browser_view_process(own);
		if (browser_view_render_pixels(own, drawn, 800U, 600U, 800U * sizeof(uint32_t)) == 0) {
			for (index = 0; index < 800U * 600U && !blue; index++)
				blue = (drawn[index] & 0xffffffU) == 0x3333ccU;
		}
		if (!blue)
			test_sleep_ms(20);
	}
	commit = view_front_commit(own);
	later = commit != NULL ? commit->serial : 0;
	if (commit != NULL)
		view_commit_release(commit);
	test_check(blue && later == serial, "(p) the frame's own change is drawn in the page's committed frame", "%s, the page's commit %llu then %llu",
	    blue ? "drawn" : "not drawn", (unsigned long long)serial, (unsigned long long)later);
	browser_view_destroy(own);
	browser_view_destroy(caller);
}

/* (q): a press and a key over the frame of another agent go to its document. */
static void
test_cross_input(void)
{
	char page[4096];
	char kid[2048];
	struct browser_view *view;
	const char *title;
	int error;

	snprintf(kid, sizeof(kid), "<!DOCTYPE html><title>kid</title><style>body { margin: 0; background: #ccf }</style><script>var got = [];"
	    " addEventListener('mousedown', function (e) { got.push('down ' + e.clientX + ' ' + e.clientY); });"
	    " addEventListener('click', function () { got.push('click'); });"
	    " addEventListener('keydown', function (e) { got.push('key ' + e.key); parent.postMessage(got.join(','), '*'); });</script>");
	(void)test_page("in-kid.html", kid);
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>in</title><style>body { margin: 0 } iframe { vertical-align: top; margin: 50px 0 0 100px; width: 200px;"
	    " height: 100px; border: 0 }</style><script>var mine = 0; addEventListener('mousedown', function () { mine++; });"
	    " onmessage = function (e) { document.title = e.data + ' page ' + mine; };</script><iframe src='%s'></iframe>", test_url(test_port_other, "in-kid.html"));
	(void)test_page("in-top.html", page);
	test_console_length = 0;
	test_console[0] = '\0';
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(q) view", "no view (%d)", error);
		return;
	}
	(void)browser_view_load(view, test_url(test_port, "in-top.html"));
	{
		long long begin = test_now();
		uint32_t pixel = 0;
		static uint32_t pixels[800U * 600U];

		/* Until the frame's scene is drawn (its committed rectangle is there). */
		while (test_now() - begin < 8000000000LL) {
			browser_view_process(view);
			if (browser_view_render_pixels(view, pixels, 800U, 600U, 800U * sizeof(uint32_t)) == 0) {
				pixel = pixels[60U * 800U + 110U];
				if ((pixel & 0xffffffU) == 0xccccffU)
					break;
			}
			test_sleep_ms(20);
		}
	}
	(void)browser_view_pointer_button(view, 130.0f, 70.0f, 0, 1, 0);
	(void)browser_view_pointer_button(view, 130.0f, 70.0f, 0, 0, 0);
	(void)browser_view_key(view, "a", "KeyA", "a", 1, 0, 0);
	(void)browser_view_key(view, "a", "KeyA", "a", 0, 0, 0);
	(void)test_pump(view, 5000, "down 30 20,click,key a page 0", NULL);
	title = browser_view_title(view);
	test_check(title != NULL && strcmp(title, "down 30 20,click,key a page 0") == 0, "(q) a press and a key over the frame of another agent are its document's",
	    "\"%s\"", title == NULL ? "" : title);
	browser_view_destroy(view);
}

/* (r): the frame of another agent ends with its iframe, and with its page kept for going back. */
static void
test_cross_end(void)
{
	char page[2048];
	struct browser_view *view;
	const char *title;
	size_t shown;
	size_t removed;
	size_t left;
	size_t back;
	long long begin;
	int error;

	(void)test_page("end-kid.html", "<!DOCTYPE html><title>kid</title><script>setInterval(function () { console.log('end kid'); }, 100);</script>");
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>end</title><script>document.addEventListener('keydown', function () {"
	    " document.querySelector('iframe').remove(); document.title = 'removed'; });</script><iframe src='%s'></iframe>",
	    test_url(test_port_other, "end-kid.html"));
	(void)test_page("end-top.html", page);
	snprintf(page, sizeof(page), "<!DOCTYPE html><title>keep</title><iframe src='%s'></iframe>", test_url(test_port_other, "end-kid.html"));
	(void)test_page("keep-top.html", page);
	error = test_view(BROWSER_THREADING_OWN, &view);
	if (error != 0) {
		test_check(0, "(r) view", "no view (%d)", error);
		return;
	}
	test_console_length = 0;
	test_console[0] = '\0';
	(void)browser_view_load(view, test_url(test_port, "end-top.html"));
	(void)test_pump(view, 5000, NULL, "end kid");
	shown = test_remotes(view);
	(void)browser_view_key(view, "x", "KeyX", "x", 1, 0, 0);
	(void)test_pump(view, 3000, "removed", NULL);
	begin = test_now();
	while (test_now() - begin < 1000000000LL && test_remotes(view) != 0U) {
		browser_view_process(view);
		test_sleep_ms(2);
	}
	removed = test_remotes(view);
	title = browser_view_title(view);
	test_check(shown == 1U && removed == 0U && title != NULL && strcmp(title, "removed") == 0, "(r) the frame of another agent ends with its iframe",
	    "%zu then %zu frames of other agents", shown, removed);

	/* Kept for going back: none while away, one again once back. */
	(void)browser_view_load(view, test_url(test_port, "keep-top.html"));
	(void)test_pump(view, 5000, "keep", NULL);
	begin = test_now();
	while (test_now() - begin < 3000000000LL && test_remotes(view) != 1U) {
		browser_view_process(view);
		test_sleep_ms(2);
	}
	shown = test_remotes(view);
	(void)browser_view_load(view, test_url(test_port, "same-kid.html"));
	(void)test_pump(view, 5000, "same kid", NULL);
	(void)test_pump(view, 200, NULL, NULL);
	left = test_remotes(view);
	(void)browser_view_go(view, -1);
	(void)test_pump(view, 5000, "keep", NULL);
	begin = test_now();
	while (test_now() - begin < 3000000000LL && test_remotes(view) != 1U) {
		browser_view_process(view);
		test_sleep_ms(2);
	}
	back = test_remotes(view);
	test_check(shown == 1U && left == 0U && back == 1U, "(r) a kept page's frame of another agent ends, and loads again when shown", "%zu, away %zu, back %zu",
	    shown, left, back);
	browser_view_destroy(view);
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
