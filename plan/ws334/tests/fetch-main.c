/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program over libbrowser's network and its operating-system
 * layer, for a check under Wine: "fetch-tool URL" fetches an http URL
 * twice, with the blocking fetch (net_http_fetch) and with the loader on
 * its own threads (net_loader_fetch, waited for with net_loader_wait),
 * and prints each one's status and the bytes of its body:
 *   fetch-tool: blocking 200 1234
 *   fetch-tool: loader 200 1234
 * A failure prints its errno value instead.  The exit status is 0 when
 * both fetches got a response.
 */

#include "net/net.h"
#include "userland/desktop/browser/shell/shell.h"

#include <browser/browser.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* How long the loader's fetch may take, in seconds. */
#define FETCH_SECONDS	30

/* What the loader's callback records. */
struct fetch_outcome {
	int ended;
	int error;
	int status;
	size_t length;
};

static int fetch_blocking(const char *url);
static int fetch_loader(const char *url);
static void fetch_done(void *context, struct net_request *request);

int
main(
	int argc,
	char **argv)
{
	int failed;
	int error;

	/* The engine makes its threads with the browser's (libkeiland's Win32 threads). */
	error = browser_threads_set(shell_threads());
	if (error != 0) {
		printf("fetch-tool: the engine refused the threads (%d)\n", error);
		return 2;
	}

	/* One argument: the URL. */
	if (argc != 2) {
		fprintf(stderr, "usage: fetch-tool URL\n");
		return 2;
	}

	/* Both ways. */
	failed = fetch_blocking(argv[1]);
	failed |= fetch_loader(argv[1]);
	return failed;
}

/* The blocking fetch; returns 0 when it got a response. */
static int
fetch_blocking(
	const char *url)
{
	struct net_response response;
	int error;

	/* The response, or the failure. */
	net_response_init(&response);
	error = net_http_fetch(NULL, url, &response);
	if (error != 0) {
		printf("fetch-tool: blocking error %d\n", error);
		net_response_release(&response);
		return 1;
	}

	/* Its status and size. */
	printf("fetch-tool: blocking %d %zu\n", response.status, response.body.length);
	net_response_release(&response);
	return 0;
}

/* The loader's fetch; returns 0 when it got a response in time. */
static int
fetch_loader(
	const char *url)
{
	struct fetch_outcome outcome;
	struct net_loader *loader;
	time_t deadline;
	int error;

	/* The loader, without a notify (the caller waits). */
	error = net_loader_create(NULL, NULL, &loader);
	if (error != 0) {
		printf("fetch-tool: loader create error %d\n", error);
		return 1;
	}

	/* The request, run until it ends or the time is up. */
	memset(&outcome, 0, sizeof(outcome));
	error = net_loader_fetch(loader, url, fetch_done, &outcome, NULL);
	deadline = time(NULL) + FETCH_SECONDS;
	while (error == 0 && net_loader_pending(loader) != 0 && time(NULL) <= deadline) {
		net_loader_wait(loader, 1000);
		net_loader_dispatch(loader);
	}
	net_loader_destroy(loader);

	/* The outcome. */
	if (error == 0 && !outcome.ended)
		error = ETIMEDOUT;
	if (error == 0)
		error = outcome.error;
	if (error != 0) {
		printf("fetch-tool: loader error %d\n", error);
		return 1;
	}
	printf("fetch-tool: loader %d %zu\n", outcome.status, outcome.length);
	return 0;
}

/* The loader's callback: the outcome kept. */
static void
fetch_done(
	void *context,
	struct net_request *request)
{
	const struct net_response *response;
	struct fetch_outcome *outcome;

	/* Ended, well or not. */
	outcome = context;
	outcome->ended = 1;
	outcome->error = net_request_error(request);
	if (outcome->error != 0)
		return;

	/* The response's status and size. */
	response = net_request_response(request);
	outcome->status = response->status;
	outcome->length = response->body.length;
}
