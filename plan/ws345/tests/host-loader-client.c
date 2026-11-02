/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the loader's clients and waited requests (net/net.h),
 * and of the page's fetches that wait (a synchronous XMLHttpRequest, a
 * script document.write adds, a frame's script, the first page) going
 * through the loader, against a small HTTP server of the test's own on
 * 127.0.0.1 and a listener whose accept queue is full (a connect to it
 * waits):
 *
 *  - two threads each with a client of one loader get their own requests'
 *    callbacks from their own dispatch;
 *  - a waited request finishes with its response without a dispatch;
 *  - an abort from another thread ends a waited request that connects to
 *    the full listener within 100 ms of the abort (ECANCELED);
 *  - aborting a client's requests before an epoch leaves the later ones;
 *  - closing a client with a request under way returns at once;
 *  - every connect the engine makes, for the view's pages above, is made
 *    on the thread named browser-net (connect is wrapped: the program is
 *    linked with -Wl,--wrap=connect, and the wrapper counts the callers by
 *    their thread's name), while a view that reads every page at once
 *    (BROWSER_FETCH_AT_ONCE, no loader) still connects on its own thread
 *    (the counting's control).
 *
 *   host-loader-client
 *
 * Run from the repository's root.  Prints one line per failed check and a
 * summary.  plan/ws345/tests/host-loader-client.sh builds and runs it.
 */

#include "net/net.h"

#include <browser/browser.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The most console text kept, in bytes. */
#define CLIENT_CONSOLE_MAX	8192U

/* One resource of the server. */
struct client_resource {
	const char *path;
	const char *type;
	const char *body;
};

/* What one client thread saw: its loader, its client, whether its callback ran on it, and the body. */
struct client_thread {
	struct net_loader *loader;
	const char *url;
	pthread_t self;
	int called;
	int on_self;
	char body[64];
};

/* What an async request's callback saw. */
struct client_seen {
	int called;
	int error;
};

static unsigned checks;
static unsigned failures;
static unsigned short port_serve;
static unsigned short port_hang;
static char console_text[CLIENT_CONSOLE_MAX];
static size_t console_length;
static pthread_mutex_t connect_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long connect_net;
static unsigned long connect_other;

/* The server's pages. */
static const struct client_resource client_resources[] = {
	{ "/a", "text/plain", "A" },
	{ "/b", "text/plain", "B" },
	{ "/first.html", "text/html", "<!DOCTYPE html><title>first</title><p>first" },
	{ "/xhr.html", "text/html",
	  "<!DOCTYPE html><title>xhr</title><script>var x = new XMLHttpRequest(); x.open('GET', '/a', false); x.send();"
	  " console.log('xhr ' + x.responseText);</script>" },
	{ "/write.html", "text/html",
	  "<!DOCTYPE html><title>write</title><script>document.write('<script src=\"/s.js\"><\\/script>');</script>" },
	{ "/s.js", "text/javascript", "console.log('written script ran');" },
	{ "/frame-top.html", "text/html",
	  "<!DOCTYPE html><title>top</title><iframe src=/frame.html></iframe>"
	  "<script>setTimeout(function poll() { if (window.frameRan) console.log('frame script ran'); else setTimeout(poll, 20); }, 20);</script>" },
	{ "/frame.html", "text/html", "<!DOCTYPE html><title>frame</title><script src=/f.js></script>" },
	{ "/f.js", "text/javascript", "parent.frameRan = 1;" }
};

int __real_connect(int descriptor, const struct sockaddr *address, socklen_t length);
int __wrap_connect(int descriptor, const struct sockaddr *address, socklen_t length);
static void client_check(int condition, const char *what);
static double client_now(void);
static int client_listen(int backlog, unsigned short *port);
static void *client_serve(void *context);
static void *client_answer_thread(void *context);
static void *client_thread_main(void *context);
static void *client_abort_later(void *context);
static void *client_abort_epoch(void *context);
static void client_thread_done(void *context, struct net_request *request);
static void client_seen_done(void *context, struct net_request *request);
static void client_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static int client_view_page(const char *path, const char *line, enum browser_fetch fetch);

/* The client the epoch thread aborts in. */
static struct net_loader_client *epoch_client;

int
main(
	void)
{
	struct client_thread first;
	struct client_thread second;
	struct client_seen old;
	struct client_seen current;
	struct net_loader_client *closing;
	struct net_loader *loader;
	struct net_request *request;
	struct net_request *hanging;
	struct net_response response;
	struct sockaddr_in address;
	pthread_t one;
	pthread_t two;
	pthread_t aborter;
	char url[128];
	char hang_url[128];
	double start;
	double took;
	int listener;
	int hold;
	int index;
	int error;

	/* Storage stays under build/. */
	setenv("XDG_DATA_HOME", "build/ws345-loader-client-data", 1);

	/* The server, and the listener whose queue is filled by connections nobody accepts. */
	listener = client_listen(64, &port_serve);
	hold = client_listen(0, &port_hang);
	if (listener < 0 || hold < 0)
		return 2;
	for (index = 0; index < 4; index++) {
		int filler;

		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port_hang);
		filler = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		(void)connect(filler, (struct sockaddr *)&address, sizeof(address));
	}
	pthread_create(&one, NULL, client_serve, (void *)(long)listener);
	pthread_detach(one);
	snprintf(hang_url, sizeof(hang_url), "http://127.0.0.1:%u/x", (unsigned)port_hang);

	/* The loader. */
	error = net_loader_create(NULL, NULL, &loader);
	client_check(error == 0, "loader: made");
	if (error != 0)
		return 1;

	/* Two threads, each with a client: each callback runs on its own thread. */
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/a", (unsigned)port_serve);
	first.loader = loader;
	first.url = strdup(url);
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/b", (unsigned)port_serve);
	second.loader = loader;
	second.url = strdup(url);
	pthread_create(&one, NULL, client_thread_main, &first);
	pthread_create(&two, NULL, client_thread_main, &second);
	pthread_join(one, NULL);
	pthread_join(two, NULL);
	client_check(first.called && first.on_self && strcmp(first.body, "A") == 0, "clients: the first thread's callback ran on it with its body");
	client_check(second.called && second.on_self && strcmp(second.body, "B") == 0, "clients: the second thread's callback ran on it with its body");

	/* A waited request: its response without a dispatch. */
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/a", (unsigned)port_serve);
	error = net_loader_client_start_waited(net_loader_own_client(loader), url, NET_FETCH_DOCUMENT, NULL, &request);
	client_check(error == 0, "waited: started");
	if (error == 0) {
		error = net_request_wait(request);
		client_check(error == 0, "waited: finished well");
		net_request_take_response(request, &response);
		client_check(response.status == 200 && response.body.length == 1 && response.body.data[0] == 'A', "waited: its response");
		net_response_release(&response);
		net_request_release(request);
	}

	/* An abort from another thread ends a waited request that waits to connect. */
	error = net_loader_client_start_waited(net_loader_own_client(loader), hang_url, NET_FETCH_DOCUMENT, NULL, &hanging);
	client_check(error == 0, "abort: started");
	if (error == 0) {
		pthread_create(&aborter, NULL, client_abort_later, hanging);
		start = client_now();
		error = net_request_wait(hanging);
		took = client_now() - start;
		pthread_join(aborter, NULL);
		printf("host-loader-client: the aborted request ended %.0f ms after the wait began (the abort at 200 ms)\n", took);
		client_check(error == ECANCELED, "abort: the waited request ends with ECANCELED");
		client_check(took >= 190.0 && took < 300.0, "abort: within 100 ms of the abort");
		net_request_release(hanging);
	}

	/* The requests before an epoch are aborted (no callback), the later ones stay. */
	memset(&old, 0, sizeof(old));
	memset(&current, 0, sizeof(current));
	epoch_client = net_loader_own_client(loader);
	net_loader_client_set_epoch(epoch_client, 1);
	error = net_loader_client_send(epoch_client, hang_url, NET_FETCH_DOCUMENT, NULL, client_seen_done, &old, NULL);
	client_check(error == 0, "epoch: the old request started");
	net_loader_client_set_epoch(epoch_client, 2);
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/b", (unsigned)port_serve);
	error = net_loader_client_send(epoch_client, url, NET_FETCH_DOCUMENT, NULL, client_seen_done, &current, NULL);
	client_check(error == 0, "epoch: the current request started");
	pthread_create(&aborter, NULL, client_abort_epoch, NULL);
	pthread_join(aborter, NULL);
	start = client_now();
	while (net_loader_client_pending(epoch_client) != 0 && client_now() - start < 5000.0) {
		net_loader_client_wait(epoch_client, 100);
		(void)net_loader_client_dispatch_until(epoch_client, UINT64_MAX);
	}
	client_check(current.called && current.error == 0, "epoch: the current request's callback ran");
	client_check(!old.called, "epoch: the aborted request's callback did not run");
	client_check(net_loader_client_pending(epoch_client) == 0, "epoch: nothing is pending");

	/* A client closed with a request under way. */
	error = net_loader_client_open(loader, NULL, NULL, &closing);
	client_check(error == 0, "close: client opened");
	if (error == 0) {
		error = net_loader_client_send(closing, hang_url, NET_FETCH_DOCUMENT, NULL, client_seen_done, &old, NULL);
		client_check(error == 0, "close: request started");
		start = client_now();
		net_loader_client_close(closing);
		took = client_now() - start;
		client_check(took < 500.0 && !old.called, "close: returns at once without the callback");
	}
	net_loader_destroy(loader);

	/* The view's pages that wait for the network: every connect on browser-net. */
	pthread_mutex_lock(&connect_lock);
	connect_net = 0;
	connect_other = 0;
	pthread_mutex_unlock(&connect_lock);
	client_check(client_view_page("/xhr.html", "xhr A", BROWSER_FETCH_DEFAULT), "view: a synchronous XMLHttpRequest got its response");
	client_check(client_view_page("/write.html", "written script ran", BROWSER_FETCH_DEFAULT), "view: the script document.write added ran");
	client_check(client_view_page("/frame-top.html", "frame script ran", BROWSER_FETCH_DEFAULT), "view: the frame's script ran");
	pthread_mutex_lock(&connect_lock);
	printf("host-loader-client: connects on browser-net %lu, on other threads %lu\n", connect_net, connect_other);
	client_check(connect_net > 0 && connect_other == 0, "view: every connect is made on browser-net");
	connect_other = 0;
	pthread_mutex_unlock(&connect_lock);

	/* The control: a view without a loader connects on the caller's thread. */
	client_check(client_view_page("/first.html", "never", BROWSER_FETCH_AT_ONCE) == 0, "control: the page without the line");
	pthread_mutex_lock(&connect_lock);
	client_check(connect_other > 0, "control: a view without a loader connects on its own thread (the wrapper counts)");
	pthread_mutex_unlock(&connect_lock);

	/* The summary. */
	printf("host-loader-client: %u checks, %u failed\n", checks, failures);
	if (failures != 0) {
		printf("console:\n%s", console_text);
		return 1;
	}
	return 0;
}

/* Counts each connect by the name of the thread that makes it, then connects. */
int
__wrap_connect(
	int descriptor,
	const struct sockaddr *address,
	socklen_t length)
{
	char name[16];

	/* The caller's thread. */
	name[0] = '\0';
	(void)pthread_getname_np(pthread_self(), name, sizeof(name));
	pthread_mutex_lock(&connect_lock);
	if (strcmp(name, "browser-net") == 0)
		connect_net++;
	else
		connect_other++;
	pthread_mutex_unlock(&connect_lock);

	/* The system's connect. */
	return __real_connect(descriptor, address, length);
}

/* Loads a page in a view (the first page read at once) and settles it until the console has a line; 1 when it came. */
static int
client_view_page(
	const char *path,
	const char *line,
	enum browser_fetch fetch)
{
	static const struct browser_fonts fonts = {
		"userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf", "userland/desktop/fonts/Mahora-Bold.ttf",
		"userland/desktop/fonts/Mahora-Mono.ttf"
	};
	struct browser_callbacks callbacks;
	struct browser_view_options options;
	struct browser_view *view;
	char url[160];
	double start;
	int error;
	int found;

	/* The view, which reads its first page at once through its loader. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = client_console;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.width = 320;
	options.height = 200;
	options.fetch = fetch;
	error = browser_view_create(&options, &view);
	if (error != 0)
		return 0;
	snprintf(url, sizeof(url), "http://127.0.0.1:%u%s", (unsigned)port_serve, path);
	error = browser_view_load(view, url);

	/* Settled until the line comes, or 10 s (1 s for a line that never comes). */
	found = 0;
	start = client_now();
	while (error == 0 && client_now() - start < (strcmp(line, "never") == 0 ? 1000.0 : 10000.0)) {
		(void)browser_view_settle(view, 200.0, BROWSER_SETTLE_LAYOUT);
		if (strstr(console_text, line) != NULL) {
			found = 1;
			break;
		}
	}
	browser_view_destroy(view);
	return found;
}

/* A client thread: its client, its request, its dispatch until the callback ran. */
static void *
client_thread_main(
	void *context)
{
	struct client_thread *thread;
	struct net_loader_client *client;
	double start;
	int error;

	thread = context;
	thread->self = pthread_self();
	error = net_loader_client_open(thread->loader, NULL, NULL, &client);
	if (error != 0)
		return NULL;
	error = net_loader_client_send(client, thread->url, NET_FETCH_DOCUMENT, NULL, client_thread_done, thread, NULL);
	start = client_now();
	while (error == 0 && !thread->called && client_now() - start < 5000.0) {
		net_loader_client_wait(client, 100);
		(void)net_loader_client_dispatch_until(client, UINT64_MAX);
	}
	net_loader_client_close(client);
	return NULL;
}

/* A client thread's callback: on which thread it ran, and the body. */
static void
client_thread_done(
	void *context,
	struct net_request *request)
{
	struct client_thread *thread;
	const struct net_response *response;
	size_t length;

	thread = context;
	thread->called = 1;
	thread->on_self = pthread_equal(pthread_self(), thread->self);
	response = net_request_response(request);
	length = response->body.length;
	if (length >= sizeof(thread->body))
		length = sizeof(thread->body) - 1U;
	memcpy(thread->body, response->body.data, length);
	thread->body[length] = '\0';
}

/* Aborts a request 200 ms after it starts, from its own thread. */
static void *
client_abort_later(
	void *context)
{
	usleep(200000);
	net_request_abort(context);
	return NULL;
}

/* Aborts the epoch client's requests before epoch 2, from its own thread. */
static void *
client_abort_epoch(
	void *context)
{
	(void)context;
	net_loader_client_abort_before(epoch_client, 2);
	return NULL;
}

/* An async request's callback. */
static void
client_seen_done(
	void *context,
	struct net_request *request)
{
	struct client_seen *seen;

	seen = context;
	seen->called = 1;
	seen->error = net_request_error(request);
}

/* Keeps what the pages write to the console, a line each. */
static void
client_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	(void)context;
	(void)view;
	(void)level;
	if (console_length + length + 2U > sizeof(console_text))
		return;
	memcpy(console_text + console_length, text, length);
	console_length += length;
	console_text[console_length++] = '\n';
	console_text[console_length] = '\0';
}

/* Counts a check and reports it when it failed. */
static void
client_check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL %s\n", what);
	}
}

/* The monotonic clock, in milliseconds. */
static double
client_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

/* A listener on 127.0.0.1, on a port of the system's choice. */
static int
client_listen(
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
		printf("host-loader-client: no listener\n");
		return -1;
	}
	size = sizeof(address);
	getsockname(listener, (struct sockaddr *)&address, &size);
	*port = ntohs(address.sin_port);
	return listener;
}

/* The server: each connection is answered by a thread of its own. */
static void *
client_serve(
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
		pthread_create(&thread, NULL, client_answer_thread, (void *)(long)client);
		pthread_detach(thread);
	}
	return NULL;
}

/* Answers one request with its resource, or 404; the connection then closes. */
static void *
client_answer_thread(
	void *context)
{
	const struct client_resource *found;
	char request[2048];
	char head[256];
	ssize_t got;
	size_t length;
	size_t index;
	size_t path;
	int head_length;
	int client;

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
	for (index = 0; index < sizeof(client_resources) / sizeof(client_resources[0]); index++) {
		path = strlen(client_resources[index].path);
		if (strncmp(request + 4, client_resources[index].path, path) == 0 && request[4 + path] == ' ')
			found = &client_resources[index];
	}
	if (found == NULL) {
		head_length = snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		(void)send(client, head, (size_t)head_length, MSG_NOSIGNAL);
	} else {
		head_length = snprintf(head, sizeof(head),
		    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
		    found->type, strlen(found->body));
		(void)send(client, head, (size_t)head_length, MSG_NOSIGNAL);
		(void)send(client, found->body, strlen(found->body), MSG_NOSIGNAL);
	}
	close(client);
	return NULL;
}
