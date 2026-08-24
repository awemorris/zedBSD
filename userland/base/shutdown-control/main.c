/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/service-config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *
program_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash != NULL ? slash + 1 : path;
}

int
main(int argc, char **argv)
{
	struct sockaddr_un address;
	char request[32], response[128];
	const char *name = program_name(argv[0]);
	ssize_t length;
	int descriptor = -1;

	if (strcmp(name, "shutdown") == 0) {
		if (argc == 2 && strcmp(argv[1], "-r") == 0)
			name = "reboot";
		else if (argc != 1 &&
			 !(argc == 2 && strcmp(argv[1], "-h") == 0)) {
			fprintf(stderr, "usage: shutdown [-h|-r]\n");
			return 2;
		} else
			name = "poweroff";
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s\n", name);
		return 2;
	}
	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		goto failed;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, ZEDBSD_INIT_SOCKET);
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
	    0)
		goto failed;
	snprintf(request, sizeof(request), "%s\n", name);
	if (write(descriptor, request, strlen(request)) !=
	    (ssize_t)strlen(request))
		goto failed;
	length = read(descriptor, response, sizeof(response));
	if (length > 0)
		(void)write(STDOUT_FILENO, response, (size_t)length);
	close(descriptor);
	return length > 0 && memcmp(response, "OK ", 3) == 0 ? 0 : 1;

failed:
	fprintf(stderr, "%s: init is unavailable: %s\n", name, strerror(errno));
	if (descriptor >= 0)
		close(descriptor);
	return 1;
}
