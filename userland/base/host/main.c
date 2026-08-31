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
	char b[INET_ADDRSTRLEN];
	struct sockaddr_in *sin;
	struct addrinfo hints = {0}, *r, *p;
	int e, found;

	found = 0;

	/* Validates the command-line arguments. */
	if (argc != 2) {
		fprintf(stderr, "usage: host name\n");

		/* Reports operation failure. */
		return 2;
	}
	hints.ai_family = AF_INET;
	e = getaddrinfo(argv[1], NULL, &hints, &r);

	/* Handles the e condition. */
	if (e) {
		fprintf(stderr, "host: %s: %s\n", argv[1], gai_strerror(e));

		/* Reports operation failure. */
		return 1;
	}

	/* Process each element required by the operation. */
	for (p = r; p; p = p->ai_next) {
		sin = (struct sockaddr_in *)p->ai_addr;

		/* Handles a failed inet ntop operation. */
		if (inet_ntop(AF_INET, &sin->sin_addr, b, sizeof(b))) {
			printf("%s has address %s\n", argv[1], b);
			found = 1;
		}
	}
	freeaddrinfo(r);

	/* Returns the computed result. */
	return found ? 0 : 1;
}
