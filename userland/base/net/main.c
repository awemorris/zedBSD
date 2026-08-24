/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/service-config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define NETWORKD_SOCKET "/run/networkd.sock"

int
main(int argc, char **argv)
{
	struct sockaddr_un address;
	char request[160], response[512];
	ssize_t length;
	int descriptor = -1, failed = 0;

	if (argc < 2 || argc > 3 ||
	    (strcmp(argv[1], "show") != 0 && strcmp(argv[1], "up") != 0 &&
	     strcmp(argv[1], "down") != 0 && strcmp(argv[1], "dhcp") != 0 &&
	     strcmp(argv[1], "reload") != 0)) {
		fprintf(stderr, "usage: net {show [interface]|up|down|dhcp "
				"interface|reload}\n");
		return 2;
	}
	if ((strcmp(argv[1], "up") == 0 || strcmp(argv[1], "down") == 0 ||
	     strcmp(argv[1], "dhcp") == 0) &&
	    argc != 3)
		return 2;
	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		goto unavailable;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
	    0)
		goto unavailable;
	if (snprintf(request, sizeof(request), argc == 3 ? "%s %s\n" : "%s\n",
		     argv[1],
		     argc == 3 ? argv[2] : NULL) >= (int)sizeof(request) ||
	    write(descriptor, request, strlen(request)) !=
		(ssize_t)strlen(request))
		goto unavailable;
	while ((length = read(descriptor, response, sizeof(response))) > 0) {
		if (length >= 4 && memcmp(response, "ERR ", 4) == 0)
			failed = 1;
		if (write(STDOUT_FILENO, response, (size_t)length) != length)
			failed = 1;
	}
	close(descriptor);
	return failed;

unavailable:
	fprintf(stderr, "net: networkd is unavailable: %s\n", strerror(errno));
	if (descriptor >= 0)
		close(descriptor);
	return 1;
}
