/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of Back and Forward from the pages the view keeps, through
 * the view (<browser/browser.h>), against a small HTTP server of its own on
 * 127.0.0.1 that counts the requests of each page: Back to a page left
 * shows it again at once, as it was (its script's state, no new request,
 * no new run of its scripts), and so does Forward; a page whose document
 * came with Cache-Control: no-store is fetched again; a new page after Back
 * drops the steps after it.  A kept page hears pagehide with persisted true
 * when it is put away and pageshow with persisted true when it is shown
 * again (its first pageshow has persisted false).  location.replace, and a
 * script's navigation before the load event ends, take the place of the
 * step shown instead of adding one.
 *
 *   host-back-forward
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

/* The most console text kept, in bytes. */
#define BACK_CONSOLE_MAX	8192U

/* One page of the server: its path, whether it is no-store, its body, and how many times it was asked for. */
struct back_page {
	const char *path;
	int no_store;
	const char *body;
	int requests;
};

/* What the callbacks saw. */
struct back_state {
	char console[BACK_CONSOLE_MAX];
	size_t console_length;
	int committed;
};

static unsigned checks;
static unsigned failures;
static struct back_state state;
static pthread_mutex_t back_lock = PTHREAD_MUTEX_INITIALIZER;

/* Each page counts its own runs in a global and links to the next. */
static struct back_page back_pages[] = {
	{ "/a.html", 0, "<!DOCTYPE html><title>a</title><script>var runs = (window.runs || 0) + 1; var mark = 'kept-a'; console.log('a ran');"
	  "addEventListener('pageshow', function (e) { console.log('a pageshow ' + e.persisted + ' ' + (e.target === document)); });"
	  "addEventListener('pagehide', function (e) { console.log('a pagehide ' + e.persisted); });</script>"
	  "<a href=/b.html style='display:block;height:40px'>b</a>", 0 },
	{ "/b.html", 0, "<!DOCTYPE html><title>b</title><script>console.log('b ran');</script><a href=/c.html style='display:block;height:40px'>c</a>", 0 },
	{ "/c.html", 0, "<!DOCTYPE html><title>c</title><script>console.log('c ran');</script>", 0 },
	{ "/n.html", 1, "<!DOCTYPE html><title>n</title><script>console.log('n ran');</script><a href=/b.html style='display:block;height:40px'>b</a>", 0 },
	{ "/r.html", 0, "<!DOCTYPE html><title>r</title><script>console.log('r ran');"
	  "addEventListener('load', function () { setTimeout(function () { location.replace('/c.html'); }, 0); });</script>", 0 },
	{ "/e.html", 0, "<!DOCTYPE html><title>e</title><script>console.log('e ran'); location.href = '/b.html';</script>", 0 },
};

static void back_check(int condition, const char *what);
static int back_count(const char *text);
static int back_requests(const char *path);
static void back_run(struct browser_view *view, double ms);
static void back_click(struct browser_view *view);
static void *back_serve(void *context);
static void *back_answer(void *context);
static void back_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void back_committed(void *context, struct browser_view *view);

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
	int listener;
	int one;
	int error;

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
		printf("host-back-forward: no server\n");
		return 2;
	}
	size = sizeof(address);
	getsockname(listener, (struct sockaddr *)&address, &size);
	pthread_create(&server, NULL, back_serve, &listener);
	pthread_detach(server);

	/* The view, on a.html. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = back_console;
	callbacks.committed = back_committed;
	callbacks.context = &state;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.width = 400;
	options.height = 300;
	error = browser_view_create(&options, &view);
	back_check(error == 0, "view: made");
	if (error != 0)
		return 1;
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/a.html", (unsigned)ntohs(address.sin_port));
	error = browser_view_load(view, url);
	back_check(error == 0, "a: loads");
	back_run(view, 300.0);

	/* 1. a, then b by its link, then Back: a at once, as it was. */
	back_click(view);
	back_run(view, 1500.0);
	back_check(strstr(browser_view_url(view), "/b.html") != NULL && back_count("b ran") == 1, "b: shown by a's link");
	state.committed = 0;
	error = browser_view_go(view, -1);
	back_check(error == 0 && state.committed == 1 && strstr(browser_view_url(view), "/a.html") != NULL, "Back: a is shown within the call");
	back_run(view, 300.0);
	back_check(back_requests("/a.html") == 1 && back_count("a ran") == 1, "Back: a is neither fetched nor run again");
	back_check(strcmp(browser_view_title(view), "a") == 0, "Back: a's title");
	back_check(back_count("a pageshow false true") == 1 && back_count("a pagehide true") == 1 && back_count("a pageshow true true") == 1,
	    "a: pageshow persisted false when loaded, pagehide persisted true when kept, pageshow persisted true when shown again");

	/* 2. Forward: b at once. */
	state.committed = 0;
	error = browser_view_go(view, 1);
	back_check(error == 0 && state.committed == 1 && strstr(browser_view_url(view), "/b.html") != NULL, "Forward: b is shown within the call");
	back_run(view, 300.0);
	back_check(back_requests("/b.html") == 1 && back_count("b ran") == 1, "Forward: b is neither fetched nor run again");

	/* 3. Back to a, then c by a script's navigation: Forward goes to c, not b. */
	(void)browser_view_go(view, -1);
	(void)browser_view_follow(view, "/c.html");
	back_run(view, 1500.0);
	back_check(strstr(browser_view_url(view), "/c.html") != NULL, "c: shown after Back");
	back_check(browser_view_can_go(view, 1) == 0, "c: no step after it");
	(void)browser_view_go(view, -1);
	back_check(strstr(browser_view_url(view), "/a.html") != NULL && back_count("a ran") == 1, "Back from c: a as it was");
	browser_view_destroy(view);

	/* 4. A no-store page is fetched again when Back goes to it. */
	error = browser_view_create(&options, &view);
	back_check(error == 0, "second view: made");
	if (error != 0)
		return 1;
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/n.html", (unsigned)ntohs(address.sin_port));
	(void)browser_view_load(view, url);
	back_run(view, 300.0);
	back_click(view);
	back_run(view, 1500.0);
	(void)browser_view_go(view, -1);
	back_run(view, 1500.0);
	back_check(strstr(browser_view_url(view), "/n.html") != NULL && back_requests("/n.html") == 2 && back_count("n ran") == 2,
	    "Back to a no-store page fetches and runs it again");
	browser_view_destroy(view);

	/* 5. location.replace after the load: Back from the page it went to skips the page that replaced itself, and Forward comes back to it. */
	error = browser_view_create(&options, &view);
	back_check(error == 0, "third view: made");
	if (error != 0)
		return 1;
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/a.html", (unsigned)ntohs(address.sin_port));
	(void)browser_view_load(view, url);
	back_run(view, 300.0);
	(void)browser_view_follow(view, "/r.html");
	back_run(view, 1500.0);
	back_check(strstr(browser_view_url(view), "/c.html") != NULL && back_count("r ran") == 1, "replace: r went on to c");
	back_check(browser_view_can_go(view, -2) == 0 && browser_view_can_go(view, -1) == 1, "replace: one step before c");
	(void)browser_view_go(view, -1);
	back_run(view, 300.0);
	back_check(strstr(browser_view_url(view), "/a.html") != NULL, "replace: Back from c is a");
	(void)browser_view_go(view, 1);
	back_run(view, 1500.0);
	back_check(strstr(browser_view_url(view), "/c.html") != NULL && back_requests("/r.html") == 1, "replace: Forward from a is c, r is not asked for again");

	/* 6. A script's navigation while its page loads replaces that page too. */
	(void)browser_view_follow(view, "/e.html");
	back_run(view, 1500.0);
	back_check(strstr(browser_view_url(view), "/b.html") != NULL && back_count("e ran") == 1, "early: e went on to b");
	(void)browser_view_go(view, -1);
	back_run(view, 1500.0);
	back_check(strstr(browser_view_url(view), "/c.html") != NULL, "early: Back from b is c, not e");
	browser_view_destroy(view);

	/* The summary. */
	printf("host-back-forward: %u checks, %u failed\n", checks, failures);
	if (failures != 0) {
		printf("console:\n%s", state.console);
		return 1;
	}
	return 0;
}

/* Counts a check and reports it when it failed. */
static void
back_check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL %s\n", what);
	}
}

/* Counts the console's lines with the text. */
static int
back_count(
	const char *text)
{
	const char *at;
	int count;

	count = 0;
	for (at = strstr(state.console, text); at != NULL; at = strstr(at + 1, text))
		count++;
	return count;
}

/* How many times the server was asked for a page. */
static int
back_requests(
	const char *path)
{
	size_t index;
	int count;

	count = -1;
	pthread_mutex_lock(&back_lock);
	for (index = 0; index < sizeof(back_pages) / sizeof(back_pages[0]); index++) {
		if (strcmp(back_pages[index].path, path) == 0)
			count = back_pages[index].requests;
	}
	pthread_mutex_unlock(&back_lock);
	return count;
}

/* Runs the view as a program's loop does for a while. */
static void
back_run(
	struct browser_view *view,
	double ms)
{
	struct timespec now;
	double start;
	double at;

	clock_gettime(CLOCK_MONOTONIC, &now);
	start = (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
	do {
		browser_view_process(view);
		usleep(5000);
		clock_gettime(CLOCK_MONOTONIC, &now);
		at = (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
	} while (at - start < ms);
}

/* Clicks the page's link (at its top left). */
static void
back_click(
	struct browser_view *view)
{
	(void)browser_view_pointer_move(view, 10.0f, 15.0f, 0);
	(void)browser_view_pointer_button(view, 10.0f, 15.0f, BROWSER_BUTTON_PRIMARY, 1, 0);
	(void)browser_view_pointer_button(view, 10.0f, 15.0f, BROWSER_BUTTON_PRIMARY, 0, 0);
}

/* The server's thread: each connection gets a thread that answers its one request. */
static void *
back_serve(
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
		pthread_create(&thread, NULL, back_answer, (void *)(long)client);
		pthread_detach(thread);
	}
	return NULL;
}

/* Answers one request: the page of its path (counted), or 404; the connection then closes. */
static void *
back_answer(
	void *context)
{
	struct back_page *found;
	char request[2048];
	char head[256];
	ssize_t got;
	size_t length;
	size_t index;
	size_t path;
	int head_length;
	int client;

	/* The request line's path. */
	client = (int)(long)context;
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
	for (index = 0; index < sizeof(back_pages) / sizeof(back_pages[0]); index++) {
		path = strlen(back_pages[index].path);
		if (strncmp(request + 4, back_pages[index].path, path) == 0 && request[4 + path] == ' ')
			found = &back_pages[index];
	}

	/* The answer: a page that the cache would have to ask about again (no-cache), or no-store. */
	if (found == NULL) {
		head_length = snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		(void)send(client, head, (size_t)head_length, 0);
	} else {
		pthread_mutex_lock(&back_lock);
		found->requests++;
		pthread_mutex_unlock(&back_lock);
		head_length = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nCache-Control: %s\r\nConnection: close\r\n\r\n",
		    strlen(found->body), found->no_store ? "no-store" : "no-cache");
		(void)send(client, head, (size_t)head_length, 0);
		(void)send(client, found->body, strlen(found->body), 0);
	}
	close(client);
	return NULL;
}

/* Keeps what the pages write to the console, a line each. */
static void
back_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct back_state *seen;

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

/* Counts the pages shown. */
static void
back_committed(
	void *context,
	struct browser_view *view)
{
	(void)view;
	((struct back_state *)context)->committed++;
}
