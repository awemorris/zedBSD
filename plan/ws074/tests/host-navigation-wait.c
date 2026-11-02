/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a navigation that waits for the network without
 * blocking its program: through the view (<browser/browser.h>), against a
 * small HTTP server of its own on 127.0.0.1, a click on a link goes to a
 * page whose style sheets come slowly, one linked in the markup and one a
 * script of the page links while it is parsed, and whose scripts that a
 * script adds each take a while.  While the sheets are on their way and the scripts
 * arrive, no browser_view_process call may take long (the program draws
 * and takes its input between them), the page shown stays the first one
 * until the new one is ready, and the new one is shown with both sheets
 * applied when its scripts after the parse run.
 *
 *   host-navigation-wait
 *
 * Run from the repository's root (the fonts are the checked-in ones).
 * Prints one line per failed check and a summary.
 */

#include <browser/browser.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* How slowly the sheets come, the longest one browser_view_process may take, and how long the test waits, in milliseconds. */
#define WAIT_SHEET_MS		1200
#define WAIT_PROCESS_MAX_MS	400.0
#define WAIT_LIMIT_MS		15000.0

/* The most console text kept, in bytes. */
#define WAIT_CONSOLE_MAX	8192U

/* What the callbacks saw. */
struct wait_state {
	char console[WAIT_CONSOLE_MAX];
	size_t console_length;
	int committed;
};

/* One resource of the server: its path, its type, its body, and how long it waits before answering. */
struct wait_resource {
	const char *path;
	const char *type;
	const char *body;
	int delay_ms;
};

static unsigned checks;
static unsigned failures;
static struct wait_state state;

/* The pages: the first one links to the second; the second links one slow sheet and a script of its links another, and adds four scripts that each spin a while. */
static const struct wait_resource wait_resources[] = {
	{ "/first.html", "text/html",
	  "<!DOCTYPE html><title>first</title><a id=go href=/second.html style='display:block;width:200px;height:50px'>go</a>", 0 },
	{ "/second.html", "text/html",
	  "<!DOCTYPE html><title>second</title><link rel=stylesheet href=/linked.css>"
	  "<script>var l = document.createElement('link'); l.rel = 'stylesheet'; l.href = '/added.css'; document.head.appendChild(l);</script>"
	  "<script>for (var i = 1; i <= 4; i++) { var s = document.createElement('script'); s.src = '/spin' + i + '.js'; document.head.appendChild(s); }</script>"
	  "<p id=p>text</p><script defer src=/check.js></script>", 0 },
	{ "/linked.css", "text/css", "p { color: rgb(1, 2, 3); }", WAIT_SHEET_MS },
	{ "/added.css", "text/css", "p { background-color: rgb(4, 5, 6); }", WAIT_SHEET_MS },
	{ "/spin1.js", "text/javascript", "var e = Date.now() + 150; while (Date.now() < e) {} console.log('spun 1');", 0 },
	{ "/spin2.js", "text/javascript", "var e = Date.now() + 150; while (Date.now() < e) {} console.log('spun 2');", 0 },
	{ "/spin3.js", "text/javascript", "var e = Date.now() + 150; while (Date.now() < e) {} console.log('spun 3');", 0 },
	{ "/spin4.js", "text/javascript", "var e = Date.now() + 150; while (Date.now() < e) {} console.log('spun 4');", 0 },
	{ "/check.js", "text/javascript",
	  "var p = getComputedStyle(document.getElementById('p')); console.log('styled ' + p.color);"
	  "setTimeout(function poll() { if (getComputedStyle(document.getElementById('p')).backgroundColor === 'rgb(4, 5, 6)') console.log('added sheet applied'); else setTimeout(poll, 20); }, 20);", 0 },
};

static void wait_check(int condition, const char *what);
static int wait_heard_all(void);
static double wait_now(void);
static void *wait_serve(void *context);
static void wait_answer(int client);
static void *wait_answer_thread(void *context);
static void wait_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void wait_committed(void *context, struct browser_view *view);

int
main(
	void)
{
	static const struct browser_fonts fonts = {
		"userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf", "userland/desktop/fonts/Mahora-Bold.ttf",
		"userland/desktop/fonts/Mahora-Mono.ttf"
	};
	struct browser_callbacks callbacks;
	struct browser_view_options options;
	struct browser_view *view;
	struct sockaddr_in address;
	socklen_t size;
	pthread_t server;
	char url[128];
	double start;
	double longest;
	double took;
	double end;
	int listener;
	int one;
	int error;
	int first_shown;
	int progress;
	int highest;
	int seen;
	int shrank;

	/* Storage stays under build/, away from the home directory. */
	setenv("XDG_DATA_HOME", "build/ws074-host-data", 1);

	/* The server on a port of the system's choice. */
	listener = socket(AF_INET, SOCK_STREAM, 0);
	one = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 16) != 0) {
		printf("host-navigation-wait: no server\n");
		return 2;
	}
	size = sizeof(address);
	getsockname(listener, (struct sockaddr *)&address, &size);
	pthread_create(&server, NULL, wait_serve, &listener);
	pthread_detach(server);

	/* The view on the first page, as a program's window would have it. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = wait_console;
	callbacks.committed = wait_committed;
	callbacks.context = &state;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.width = 600;
	options.height = 400;
	error = browser_view_create(&options, &view);
	wait_check(error == 0, "view: made");
	if (error != 0)
		return 1;
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/first.html", (unsigned)ntohs(address.sin_port));
	error = browser_view_load(view, url);
	wait_check(error == 0, "first page: loads");
	(void)browser_view_settle(view, 1000.0, BROWSER_SETTLE_LAYOUT);

	/* The click on the link, then the loop a program runs, timing each call. */
	state.committed = 0;
	wait_check(browser_view_load_progress(view) == -1, "progress: none before the click");
	(void)browser_view_pointer_move(view, 20.0f, 20.0f, 0);
	(void)browser_view_pointer_button(view, 20.0f, 20.0f, BROWSER_BUTTON_PRIMARY, 1, 0);
	(void)browser_view_pointer_button(view, 20.0f, 20.0f, BROWSER_BUTTON_PRIMARY, 0, 0);
	start = wait_now();
	longest = 0.0;
	first_shown = 1;
	progress = browser_view_load_progress(view);
	wait_check(progress >= 0 && progress < 1000, "progress: a load under way from the click on");
	highest = progress;
	seen = 1;
	shrank = 0;
	end = start + WAIT_LIMIT_MS;
	while (wait_now() < end && (state.committed == 0 || !wait_heard_all())) {
		took = wait_now();
		browser_view_process(view);
		took = wait_now() - took;
		if (took > longest)
			longest = took;
		if (wait_now() - start < WAIT_SHEET_MS - 200 && (state.committed || strstr(browser_view_url(view), "first.html") == NULL))
			first_shown = 0;
		progress = browser_view_load_progress(view);
		if (progress >= 0 && state.committed == 0) {
			if (progress < highest)
				shrank = 1;
			if (progress > highest) {
				highest = progress;
				seen++;
			}
		}
		usleep(2000);
	}

	/* The program was never held up, and the new page came whole. */
	printf("host-navigation-wait: the longest call %.0f ms, the page after %.0f ms\n", longest, wait_now() - start);
	wait_check(longest < WAIT_PROCESS_MAX_MS, "no browser_view_process call blocks while the sheets come and the scripts run");
	wait_check(first_shown, "the first page stays while the second one's sheets are on their way");
	wait_check(!shrank && seen >= 2, "progress: grows while the page loads");
	wait_check(browser_view_load_progress(view) == -1, "progress: none once the page is shown");
	wait_check(state.committed == 1 && strstr(browser_view_url(view), "second.html") != NULL, "the second page is shown");
	wait_check(strstr(state.console, "styled rgb(1, 2, 3)") != NULL, "its deferred script sees the sheet its markup links");
	wait_check(strstr(state.console, "added sheet applied") != NULL, "the sheet its script links applies when it comes");
	wait_check(strstr(state.console, "spun 1") != NULL && strstr(state.console, "spun 2") != NULL && strstr(state.console, "spun 3") != NULL && strstr(state.console, "spun 4") != NULL, "the scripts it added ran");
	browser_view_destroy(view);

	/* The summary. */
	printf("host-navigation-wait: %u checks, %u failed\n", checks, failures);
	if (failures != 0) {
		printf("console:\n%s", state.console);
		return 1;
	}
	return 0;
}

/* Counts a check and reports it when it failed. */
static void
wait_check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL %s\n", what);
	}
}

/* Tells whether the console has every line the second page writes. */
static int
wait_heard_all(void)
{
	static const char *const lines[] = { "styled", "added sheet applied", "spun 1", "spun 2", "spun 3", "spun 4" };
	size_t index;

	for (index = 0; index < sizeof(lines) / sizeof(lines[0]); index++) {
		if (strstr(state.console, lines[index]) == NULL)
			return 0;
	}
	return 1;
}

/* The monotonic clock, in milliseconds. */
static double
wait_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

/* The server's thread: each connection gets a thread that answers its one request. */
static void *
wait_serve(
	void *context)
{
	pthread_t thread;
	int listener;
	int client;

	listener = *(int *)context;
	for (;;) {
		client = accept(listener, NULL, NULL);
		if (client < 0)
			continue;
		pthread_create(&thread, NULL, wait_answer_thread, (void *)(long)client);
		pthread_detach(thread);
	}
	return NULL;
}

/* A connection's thread. */
static void *
wait_answer_thread(
	void *context)
{
	wait_answer((int)(long)context);
	return NULL;
}

/* Answers one request: the resource of its path after its delay, or 404; the connection then closes. */
static void
wait_answer(
	int client)
{
	const struct wait_resource *found;
	char request[2048];
	char head[256];
	ssize_t got;
	size_t length;
	size_t index;
	int head_length;

	/* The request line's path. */
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
	for (index = 0; index < sizeof(wait_resources) / sizeof(wait_resources[0]); index++) {
		size_t path = strlen(wait_resources[index].path);
		if (strncmp(request + 4, wait_resources[index].path, path) == 0 && request[4 + path] == ' ')
			found = &wait_resources[index];
	}

	/* The answer. */
	if (found == NULL) {
		head_length = snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		(void)send(client, head, (size_t)head_length, 0);
	} else {
		if (found->delay_ms > 0)
			usleep((useconds_t)found->delay_ms * 1000U);
		head_length = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
		    found->type, strlen(found->body));
		(void)send(client, head, (size_t)head_length, 0);
		(void)send(client, found->body, strlen(found->body), 0);
	}
	close(client);
}

/* Keeps what the pages write to the console, a line each. */
static void
wait_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct wait_state *seen;

	(void)view;
	(void)level;
	seen = context;
	if (seen->console_length + length + 2U > sizeof(seen->console))
		return;
	memcpy(seen->console + seen->console_length, text, length);
	seen->console_length += length;
	seen->console[seen->console_length++] = '\n';
	seen->console[seen->console_length] = '\0';
}

/* Counts the pages shown after the click. */
static void
wait_committed(
	void *context,
	struct browser_view *view)
{
	(void)view;
	((struct wait_state *)context)->committed++;
}
