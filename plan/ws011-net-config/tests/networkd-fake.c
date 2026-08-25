/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void
fail(const char *message)
{
	perror(message);
	exit(1);
}

int
main(int argc, char **argv)
{
	struct sockaddr_un address;
	int listener, index;

	if (argc < 3) {
		fprintf(stderr, "usage: fake SOCKET REQUEST...\n");
		return 2;
	}
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0)
		fail("socket");
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (strlen(argv[1]) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		fail("socket path");
	}
	strcpy(address.sun_path, argv[1]);
	(void)unlink(argv[1]);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 4) != 0)
		fail("listen");
	for (index = 2; index < argc; index++) {
		char request[512];
		size_t used = 0;
		int client = accept(listener, NULL, NULL);
		ssize_t count;
		if (client < 0)
			fail("accept");
		while (used + 1 < sizeof(request) &&
		       (count = read(client, request + used,
				     sizeof(request) - used - 1)) > 0)
			used += (size_t)count;
		request[used] = '\0';
		if (strcmp(request, argv[index]) != 0) {
			fprintf(stderr, "request %d: got <%s>, expected <%s>\n",
				index - 1, request, argv[index]);
			return 1;
		}
		if (write(client, "V1 OK\n", 6) != 6)
			fail("reply");
		(void)close(client);
	}
	(void)close(listener);
	(void)unlink(argv[1]);
	return 0;
}
