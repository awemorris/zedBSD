/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
int
main(int argc, char **argv)
{
	struct addrinfo hints = {0}, *r, *p;
	int e, found = 0;
	if (argc != 2) {
		fprintf(stderr, "usage: host name\n");
		return 2;
	}
	hints.ai_family = AF_INET;
	e = getaddrinfo(argv[1], NULL, &hints, &r);
	if (e) {
		fprintf(stderr, "host: %s: %s\n", argv[1], gai_strerror(e));
		return 1;
	}
	for (p = r; p; p = p->ai_next) {
		char b[INET_ADDRSTRLEN];
		struct sockaddr_in *sin = (struct sockaddr_in *)p->ai_addr;
		if (inet_ntop(AF_INET, &sin->sin_addr, b, sizeof(b))) {
			printf("%s has address %s\n", argv[1], b);
			found = 1;
		}
	}
	freeaddrinfo(r);
	return found ? 0 : 1;
}
