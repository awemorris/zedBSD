/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A host test of libbrowser's loader with its own network thread, over the
 * operating-system layer: a small HTTP server on a thread of the test
 * answers "/fast" at once and "/slow" after a while.  It checks that the
 * callbacks run from net_loader_dispatch on the test's thread, that notify
 * is called once until the dispatch, that a cancel (before the end, after
 * the end but before the dispatch, and from another request's callback)
 * keeps the callback from running, that a refused connection ends with its
 * error, that net_loader_ring from another thread rings notify once until
 * the dispatch (also when it rings many times from several threads while
 * requests run), that net_loader_wait ends at its time without news, and
 * that the loader can be destroyed with requests in flight.
 */

#include "net/net.h"
#include "userland/tests/browser-threads/threads-posix.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* How long "/slow" waits before it answers, in milliseconds. */
#define SLOW_MS		400

/* How many requests a round of the many case starts. */
#define MANY		24

/* How many threads ring at once, and how many times each. */
#define RINGERS		4
#define RINGS		200

/* What a test request records. */
struct outcome {
	int ended;
	int error;
	int status;
	pthread_t thread;
	struct net_request *request;
	struct net_request *victim;
};

/* The server's listening socket and its port. */
static int server_socket;
static int server_port;

/* How many times notify ran, and the test's thread. */
static atomic_int notified;
static pthread_t test_thread;

/* How many checks failed. */
static int failures;

static void check(int good, const char *what);
static void *serve(void *argument);
static void *answer(void *argument);
static void notify(void *context);
static void done(void *context, struct net_request *request);
static void pause_ms(long milliseconds);
static int settle(struct net_loader *loader, int seconds);
static void url(char *out, size_t size, const char *path);
static void *ring_once(void *argument);
static void *ring_many(void *argument);
static long now_ms(void);

int
main(void)
{
	struct sockaddr_in address;
	socklen_t length;
	pthread_t server;
	struct net_loader *loader;
	struct outcome outcomes[MANY];
	struct outcome fast;
	struct outcome slow;
	struct outcome later;
	struct outcome refused;
	char text[128];
	pthread_t ringer;
	pthread_t ringers[RINGERS];
	long started;
	long waited;
	int before;
	int index;
	int error;

	/* The engine makes its threads with the POSIX threads. */
	error = browser_threads_set(browser_threads_posix());
	if (error != 0) {
		printf("net-ring-test: the engine refused the threads (%d)\n", error);
		return 2;
	}

	/* The server. */
	test_thread = pthread_self();
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(server_socket, 64) != 0) {
		perror("server");
		return 2;
	}
	length = sizeof(address);
	getsockname(server_socket, (struct sockaddr *)&address, &length);
	server_port = ntohs(address.sin_port);
	pthread_create(&server, NULL, serve, NULL);
	pthread_detach(server);

	/* The loader. */
	error = net_loader_create(notify, NULL, &loader);
	check(error == 0, "create");
	if (error != 0)
		return 1;

	/* Many at once: every callback once, on this thread, with 200. */
	memset(outcomes, 0, sizeof(outcomes));
	for (index = 0; index < MANY; index++) {
		url(text, sizeof(text), "/fast");
		error = net_loader_fetch(loader, text, done, &outcomes[index], &outcomes[index].request);
		check(error == 0, "many: fetch");
	}
	check(net_loader_pending(loader) == MANY, "many: pending");
	check(settle(loader, 10) == 0, "many: settled");
	for (index = 0; index < MANY; index++) {
		check(outcomes[index].ended == 1, "many: ended once");
		check(outcomes[index].status == 200, "many: status 200");
		check(pthread_equal(outcomes[index].thread, test_thread), "many: callback on the test's thread");
	}

	/* notify once until the dispatch. */
	memset(&fast, 0, sizeof(fast));
	memset(&later, 0, sizeof(later));
	before = atomic_load(&notified);
	url(text, sizeof(text), "/fast");
	net_loader_fetch(loader, text, done, &fast, &fast.request);
	net_loader_fetch(loader, text, done, &later, &later.request);
	pause_ms(500);
	check(atomic_load(&notified) - before == 1, "notify: once before the dispatch");
	net_loader_dispatch(loader);
	check(fast.ended == 1 && later.ended == 1, "notify: both ended at the dispatch");

	/* A cancel before the end: no callback. */
	memset(&slow, 0, sizeof(slow));
	url(text, sizeof(text), "/slow");
	net_loader_fetch(loader, text, done, &slow, &slow.request);
	net_request_cancel(slow.request);
	check(net_loader_pending(loader) == 0, "cancel: no longer pending");
	pause_ms(SLOW_MS + 300);
	net_loader_dispatch(loader);
	check(slow.ended == 0, "cancel: no callback");

	/* A cancel after the end, before the dispatch: no callback. */
	memset(&fast, 0, sizeof(fast));
	url(text, sizeof(text), "/fast");
	net_loader_fetch(loader, text, done, &fast, &fast.request);
	net_loader_wait(loader, 5000);
	net_request_cancel(fast.request);
	net_loader_dispatch(loader);
	check(fast.ended == 0, "cancel after the end: no callback");

	/* A callback cancels another request: its callback does not run. */
	memset(&fast, 0, sizeof(fast));
	memset(&slow, 0, sizeof(slow));
	url(text, sizeof(text), "/slow");
	net_loader_fetch(loader, text, done, &slow, &slow.request);
	url(text, sizeof(text), "/fast");
	fast.victim = slow.request;
	net_loader_fetch(loader, text, done, &fast, &fast.request);
	check(settle(loader, 10) == 0, "cancel from a callback: settled");
	pause_ms(SLOW_MS + 300);
	net_loader_dispatch(loader);
	check(fast.ended == 1, "cancel from a callback: the canceller ended");
	check(slow.ended == 0, "cancel from a callback: the cancelled did not");

	/* A refused connection: the callback with its error. */
	memset(&refused, 0, sizeof(refused));
	net_loader_fetch(loader, "http://127.0.0.1:1/", done, &refused, &refused.request);
	check(settle(loader, 10) == 0, "refused: settled");
	check(refused.ended == 1 && refused.error == ECONNREFUSED, "refused: ECONNREFUSED");

	/* A ring from another thread: notify once, and a waiting caller wakes. */
	net_loader_dispatch(loader);
	before = atomic_load(&notified);
	pthread_create(&ringer, NULL, ring_once, loader);
	net_loader_wait(loader, 5000);
	pthread_join(ringer, NULL);
	check(atomic_load(&notified) - before == 1, "ring: notify");
	net_loader_ring(loader);
	net_loader_ring(loader);
	pause_ms(100);
	check(atomic_load(&notified) - before == 1, "ring: no second notify before the dispatch");
	net_loader_dispatch(loader);
	pause_ms(100);
	check(atomic_load(&notified) - before == 1, "ring: nothing more after the dispatch");

	/* Many rings from several threads while requests run: every request ends, notify comes. */
	memset(outcomes, 0, sizeof(outcomes));
	for (index = 0; index < MANY; index++) {
		url(text, sizeof(text), (index % 4) == 0 ? "/slow" : "/fast");
		net_loader_fetch(loader, text, done, &outcomes[index], &outcomes[index].request);
	}
	before = atomic_load(&notified);
	for (index = 0; index < RINGERS; index++)
		pthread_create(&ringers[index], NULL, ring_many, loader);
	for (index = 0; index < RINGERS; index++)
		pthread_join(ringers[index], NULL);
	check(settle(loader, 10) == 0, "rings: settled");
	for (index = 0; index < MANY; index++)
		check(outcomes[index].ended == 1 && outcomes[index].status == 200, "rings: each request once with 200");
	check(atomic_load(&notified) - before >= 1, "rings: notify");
	net_loader_dispatch(loader);

	/* A wait without news ends at its time, measured on the monotonic clock; 0 does not wait. */
	started = now_ms();
	net_loader_wait(loader, 300);
	waited = now_ms() - started;
	check(waited >= 290 && waited < 3000, "wait: ends at its time");
	started = now_ms();
	net_loader_wait(loader, 0);
	waited = now_ms() - started;
	check(waited < 100, "wait: 0 does not wait");

	/* Destroyed with requests in flight: no callback. */
	memset(outcomes, 0, sizeof(outcomes));
	for (index = 0; index < 8; index++) {
		url(text, sizeof(text), "/slow");
		net_loader_fetch(loader, text, done, &outcomes[index], NULL);
	}
	pause_ms(50);
	net_loader_destroy(loader);
	for (index = 0; index < 8; index++)
		check(outcomes[index].ended == 0, "destroy: no callback");

	/* The verdict. */
	if (failures != 0) {
		printf("net-ring-test: %d failed\n", failures);
		return 1;
	}
	printf("net-ring-test: PASS\n");
	return 0;
}

static void
check(
	int good,
	const char *what)
{
	if (good)
		return;
	printf("FAIL %s\n", what);
	failures++;
}

/* The server: a thread for each connection. */
static void *
serve(
	void *argument)
{
	pthread_t thread;
	intptr_t client;

	(void)argument;
	for (;;) {
		client = accept(server_socket, NULL, NULL);
		if (client < 0)
			continue;
		pthread_create(&thread, NULL, answer, (void *)client);
		pthread_detach(thread);
	}
	return NULL;
}

/* Answers one request on a connection, then closes it. */
static void *
answer(
	void *argument)
{
	static const char reply[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
	char request[4096];
	size_t used;
	ssize_t got;
	int client;

	client = (int)(intptr_t)argument;
	used = 0;
	while (used < sizeof(request) - 1U) {
		got = read(client, request + used, sizeof(request) - 1U - used);
		if (got <= 0)
			break;
		used += (size_t)got;
		request[used] = '\0';
		if (strstr(request, "\r\n\r\n") != NULL)
			break;
	}
	request[used] = '\0';
	if (strncmp(request, "GET /slow", 9) == 0)
		pause_ms(SLOW_MS);
	(void)write(client, reply, sizeof(reply) - 1U);
	close(client);
	return NULL;
}

/* The loader's notify, from its network thread. */
static void
notify(
	void *context)
{
	(void)context;
	check(!pthread_equal(pthread_self(), test_thread), "notify off the test's thread");
	atomic_fetch_add(&notified, 1);
}

/* A request's callback; one with a victim cancels it. */
static void
done(
	void *context,
	struct net_request *request)
{
	struct outcome *outcome;

	outcome = context;
	outcome->ended++;
	outcome->error = net_request_error(request);
	outcome->thread = pthread_self();
	if (outcome->error == 0)
		outcome->status = net_request_response(request)->status;
	if (outcome->victim != NULL)
		net_request_cancel(outcome->victim);
}

static void
pause_ms(
	long milliseconds)
{
	struct timespec time;

	time.tv_sec = milliseconds / 1000;
	time.tv_nsec = (milliseconds % 1000) * 1000000L;
	nanosleep(&time, NULL);
}

/* Waits for the loader's news and runs it until nothing is pending; nonzero after seconds. */
static int
settle(
	struct net_loader *loader,
	int seconds)
{
	time_t deadline;

	deadline = time(NULL) + seconds;
	while (net_loader_pending(loader) != 0) {
		if (time(NULL) > deadline)
			return 1;
		net_loader_wait(loader, 1000);
		net_loader_dispatch(loader);
	}
	return 0;
}

static void
url(
	char *out,
	size_t size,
	const char *path)
{
	snprintf(out, size, "http://127.0.0.1:%d%s", server_port, path);
}

/* Rings the loader once, from a thread of its own. */
static void *
ring_once(
	void *argument)
{
	pause_ms(50);
	net_loader_ring(argument);
	return NULL;
}

/* Rings the loader many times, from a thread of its own. */
static void *
ring_many(
	void *argument)
{
	int index;

	for (index = 0; index < RINGS; index++)
		net_loader_ring(argument);
	return NULL;
}

/* The monotonic clock in milliseconds. */
static long
now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}
