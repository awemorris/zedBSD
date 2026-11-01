/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program over libbrowser's network and TLS, for a check under Wine:
 *   tls-tool [--ca-file PEM] [--root DER] [--disallowed DER] URL
 * fetches an https URL twice, with the blocking fetch and with the loader
 * on its own threads, and prints each one's status and the bytes of its
 * body, or the failure and the reason TLS kept:
 *   tls-tool: blocking 200 1234
 *   tls-tool: loader 200 1234
 *   tls-tool: blocking error 71 (handshake: certificate verify failed: ...)
 * --ca-file trusts a PEM file's authorities (net_tls_add_ca_file).  --root
 * puts a DER certificate into the current user's ROOT store, and
 * --disallowed into its Disallowed store, for the run (they are taken out
 * again before the program ends).  The exit status is 0 when both fetches
 * got a response.
 */

#include "net/net.h"

#include <windows.h>
#include <wincrypt.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How long the loader's fetch may take, in seconds. */
#define FETCH_SECONDS	30

/* The most certificates --root and --disallowed put in the system's stores. */
#define PLACED_MAX	4

/* The largest certificate file read. */
#define DER_MAX		16384

/* What the loader's callback records. */
struct fetch_outcome {
	int ended;
	int error;
	int status;
	size_t length;
};

/* A certificate put into a system store, to be taken out again. */
struct placed {
	HCERTSTORE store;
	PCCERT_CONTEXT certificate;
};

/* The certificates put into the system's stores (the first placed_count entries). */
static struct placed placed[PLACED_MAX];

static size_t placed_count;

static int place(const wchar_t *store_name, const char *path);
static void unplace(void);
static int fetch_blocking(const char *url);
static int fetch_loader(const char *url);
static void fetch_done(void *context, struct net_request *request);

int
main(
	int argc,
	char **argv)
{
	const char *url;
	int failed;
	int index;
	int error;

	/* The options, then the URL. */
	url = NULL;
	for (index = 1; index < argc; index++) {
		if (strcmp(argv[index], "--ca-file") == 0 && index + 1 < argc) {
			error = net_tls_add_ca_file(argv[index + 1]);
			if (error != 0) {
				printf("tls-tool: --ca-file error %d\n", error);
				return 2;
			}
			index++;
		} else if (strcmp(argv[index], "--root") == 0 && index + 1 < argc) {
			if (place(L"ROOT", argv[index + 1]) != 0)
				return 2;
			index++;
		} else if (strcmp(argv[index], "--disallowed") == 0 && index + 1 < argc) {
			if (place(L"Disallowed", argv[index + 1]) != 0)
				return 2;
			index++;
		} else if (url == NULL) {
			url = argv[index];
		} else {
			url = NULL;
			break;
		}
	}
	if (url == NULL) {
		fprintf(stderr, "usage: tls-tool [--ca-file PEM] [--root DER] [--disallowed DER] URL\n");
		unplace();
		return 2;
	}

	/* Both ways, and the stores as they were. */
	failed = fetch_blocking(url);
	failed |= fetch_loader(url);
	unplace();
	return failed;
}

/* Puts a DER certificate file into a store of the current user; returns 0 or 1 (printed). */
static int
place(
	const wchar_t *store_name,
	const char *path)
{
	unsigned char der[DER_MAX];
	struct placed *entry;
	size_t length;
	FILE *file;

	/* The file. */
	file = fopen(path, "rb");
	if (file == NULL) {
		printf("tls-tool: cannot open %s\n", path);
		return 1;
	}
	length = fread(der, 1, sizeof(der), file);
	fclose(file);
	if (length == 0 || placed_count == PLACED_MAX) {
		printf("tls-tool: cannot use %s\n", path);
		return 1;
	}

	/* The store, made when it does not exist yet. */
	entry = &placed[placed_count];
	entry->store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, store_name);
	if (entry->store == NULL) {
		printf("tls-tool: cannot open the store %ls (error %lu)\n", store_name, (unsigned long)GetLastError());
		return 1;
	}

	/* The certificate in it. */
	entry->certificate = NULL;
	if (!CertAddEncodedCertificateToStore(entry->store, X509_ASN_ENCODING, der, (DWORD)length,
	    CERT_STORE_ADD_REPLACE_EXISTING, &entry->certificate)) {
		printf("tls-tool: cannot add %s to %ls (error %lu)\n", path, store_name, (unsigned long)GetLastError());
		CertCloseStore(entry->store, 0);
		return 1;
	}

	/* Written out now: Wine's registry-backed store shows a change to other readers only after a commit. */
	CertControlStore(entry->store, 0, CERT_STORE_CTRL_COMMIT, NULL);
	placed_count++;
	return 0;
}

/* Takes the placed certificates out of their stores again. */
static void
unplace(void)
{
	size_t index;

	/* Each one, newest first; the delete frees the context. */
	for (index = placed_count; index > 0; index--) {
		CertDeleteCertificateFromStore(placed[index - 1].certificate);
		CertControlStore(placed[index - 1].store, 0, CERT_STORE_CTRL_COMMIT, NULL);
		CertCloseStore(placed[index - 1].store, 0);
	}
	placed_count = 0;
}

/* The blocking fetch; returns 0 when it got a response. */
static int
fetch_blocking(
	const char *url)
{
	struct net_response response;
	int error;

	/* The response, or the failure with TLS's reason. */
	net_response_init(&response);
	error = net_http_fetch(NULL, url, &response);
	if (error != 0) {
		printf("tls-tool: blocking error %d (%s)\n", error, net_tls_error());
		net_response_release(&response);
		return 1;
	}

	/* Its status and size. */
	printf("tls-tool: blocking %d %zu\n", response.status, response.body.length);
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
		printf("tls-tool: loader create error %d\n", error);
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
		printf("tls-tool: loader error %d\n", error);
		return 1;
	}
	printf("tls-tool: loader %d %zu\n", outcome.status, outcome.length);
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
