/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A test shim of the WPT runs (wpt-cross-origin.py), preloaded into the WPT
 * server and the browser: the names of the WPT hosts (web-platform.test,
 * not-web-platform.test and their subdomains) resolve to 127.0.0.1, so
 * that the cross-origin tests run without changing the host's /etc/hosts.
 * Every other name goes to the C library's getaddrinfo.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <string.h>

/* The suffix of the WPT hosts. */
#define WPT_HOSTS_SUFFIX	"web-platform.test"

typedef int (*wpt_getaddrinfo)(const char *, const char *, const struct addrinfo *, struct addrinfo **);

int
getaddrinfo(
	const char *node,
	const char *service,
	const struct addrinfo *hints,
	struct addrinfo **result)
{
	static wpt_getaddrinfo next;
	size_t length;
	size_t suffix;

	if (next == NULL)
		next = (wpt_getaddrinfo)dlsym(RTLD_NEXT, "getaddrinfo");

	/* A WPT host is the loopback address. */
	if (node != NULL) {
		length = strlen(node);
		suffix = strlen(WPT_HOSTS_SUFFIX);
		if (length >= suffix && strcmp(node + length - suffix, WPT_HOSTS_SUFFIX) == 0)
			node = "127.0.0.1";
	}
	return next(node, service, hints, result);
}
