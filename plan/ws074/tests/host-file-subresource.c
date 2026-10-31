/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A page's subresources (images, scripts, style sheets, fonts, media, XHR
 * and fetch all read through page_fetch) read a local file only from a
 * page that is itself a local file: a web, data: or about: page gets
 * EACCES and no bytes, whatever the spelling of the file: URL.  A local
 * page (a path or a file: URL) still reads one, relative or absolute, and
 * a top-level load of a file still reads it.
 *
 * Run from the top of the tree (the tree's LICENSE is the local file read).
 */

#include "page/page.h"
#include "net/net.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One subresource read: the page's location, the href, and the error it must give. */
struct file_case {
	const char *base;
	const char *href;
	int error;
};

static unsigned checks;
static unsigned failures;

static void file_check(int condition, const char *name);
static void file_case_run(const char *base, const char *href, int expected);

/*
 * Reads the tree's LICENSE as a subresource from local and other pages.
 */
int
main(
	void)
{
	char license[PATH_MAX];
	char page_path[PATH_MAX + 32];
	char page_url[PATH_MAX + 48];
	char file_url[PATH_MAX + 48];
	char upper_url[PATH_MAX + 48];
	struct net_response response;
	size_t index;
	int error;

	/* The local file, as a path and as file: URLs, and a local page beside it. */
	if (realpath("LICENSE", license) == NULL) {
		fprintf(stderr, "host-file-subresource: run from the top of the tree\n");
		return 2;
	}
	snprintf(page_path, sizeof(page_path), "%s.html", license);
	snprintf(page_url, sizeof(page_url), "file://%s.html", license);
	snprintf(file_url, sizeof(file_url), "file://%s", license);
	snprintf(upper_url, sizeof(upper_url), "FILE://%s", license);

	{
		const struct file_case cases[] = {
			{ page_path, file_url, 0 },
			{ page_path, "LICENSE", 0 },
			{ page_url, file_url, 0 },
			{ page_url, "LICENSE", 0 },
			{ "http://example.invalid/page.html", file_url, EACCES },
			{ "https://example.invalid/", file_url, EACCES },
			{ "http://example.invalid/page.html", upper_url, EACCES },
			{ "https://example.invalid/", "file:LICENSE", EACCES },
			{ "data:text/html,page", file_url, EACCES },
			{ "about:blank", file_url, EACCES },
		};

		for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
			file_case_run(cases[index].base, cases[index].href, cases[index].error);
	}

	/* A top-level load of a local file (its own location as the base) still reads it. */
	error = page_fetch_response(file_url, file_url, &response);
	file_check(error == 0 && response.body.length > 0, "top-level file load reads the file");
	net_response_release(&response);

	printf("file subresources: %u/%u passed\n", checks - failures, checks);
	if (failures != 0)
		return 1;
	printf("host-file-subresource: PASS\n");
	return 0;
}

/* Counts one observation and reports a failed one. */
static void
file_check(
	int condition,
	const char *name)
{
	checks++;
	if (condition)
		return;
	failures++;
	printf("FAIL %s\n", name);
}

/* Reads href against base as a subresource and checks the error and the bytes. */
static void
file_case_run(
	const char *base,
	const char *href,
	int expected)
{
	struct wb_buffer bytes;
	char name[512];
	int error;

	wb_buffer_init(&bytes);
	error = page_fetch(base, href, &bytes, NULL);
	snprintf(name, sizeof(name), "%.200s from %.200s: error %d (want %d), %zu bytes", href, base, error, expected,
		 bytes.length);
	if (expected == 0)
		file_check(error == 0 && bytes.length > 0, name);
	else
		file_check(error == expected && bytes.length == 0, name);
	wb_buffer_release(&bytes);
}
