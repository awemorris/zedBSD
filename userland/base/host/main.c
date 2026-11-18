/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD host userland command.
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>

/*
 * Runs the host command.
 */
int
main(
	int argc,
	char **argv)
{
	char b[INET6_ADDRSTRLEN];
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;
	struct addrinfo hints = {0}, *r, *p;
	const char *written;
	int e, found;

	found = 0;

	/* Validates the command-line arguments. */
	if (argc != 2) {
		fprintf(stderr, "usage: host name\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Both families (ws130-p005), one record of each address. */
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	e = getaddrinfo(argv[1], NULL, &hints, &r);

	/* Handles the e condition. */
	if (e) {
		fprintf(stderr, "host: %s: %s\n", argv[1], gai_strerror(e));

		/* Reports operation failure. */
		return 1;
	}

	/* Each address: the IPv4 ones, then the IPv6 ones. */
	for (p = r; p; p = p->ai_next) {
		if (p->ai_family != AF_INET)
			continue;
		sin = (struct sockaddr_in *)p->ai_addr;
		written = inet_ntop(AF_INET, &sin->sin_addr, b, sizeof(b));
		if (written != NULL) {
			printf("%s has address %s\n", argv[1], b);
			found = 1;
		}
	}
	for (p = r; p; p = p->ai_next) {
		if (p->ai_family != AF_INET6)
			continue;
		sin6 = (struct sockaddr_in6 *)p->ai_addr;
		written = inet_ntop(AF_INET6, &sin6->sin6_addr, b, sizeof(b));
		if (written != NULL) {
			printf("%s has IPv6 address %s\n", argv[1], b);
			found = 1;
		}
	}
	freeaddrinfo(r);

	/* Succeeded when an address was found. */
	if (!found)
		return 1;
	return 0;
}
