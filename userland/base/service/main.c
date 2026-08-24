/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/service-config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int
send_request(const char *command, const char *name)
{
	struct sockaddr_un address;
	char request[160], response[512];
	ssize_t length;
	int descriptor = socket(AF_UNIX, SOCK_STREAM, 0), failed = 0;

	if (descriptor < 0) {
		fprintf(stderr, "service: socket: %s\n", strerror(errno));
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, ZEDBSD_INIT_SOCKET);
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
	    0) {
		fprintf(stderr, "service: init is unavailable: %s\n",
			strerror(errno));
		close(descriptor);
		return 1;
	}
	if (snprintf(request, sizeof(request),
		     name != NULL ? "%s %s\n" : "%s\n", command,
		     name) >= (int)sizeof(request) ||
	    write(descriptor, request, strlen(request)) !=
		(ssize_t)strlen(request)) {
		fprintf(stderr, "service: request failed: %s\n",
			strerror(errno));
		close(descriptor);
		return 1;
	}
	while ((length = read(descriptor, response, sizeof(response))) > 0) {
		if (write(STDOUT_FILENO, response, (size_t)length) != length)
			failed = 1;
		if (length >= 4 && memcmp(response, "ERR ", 4) == 0)
			failed = 1;
	}
	if (length < 0)
		failed = 1;
	close(descriptor);
	return failed;
}

int
main(int argc, char **argv)
{
	const char *command, *name = NULL;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: service "
				"{list|reload|status|start|stop|restart|enable|"
				"disable} [name]\n");
		return 2;
	}
	command = argv[1];
	if (argc == 3)
		name = argv[2];
	if ((strcmp(command, "list") == 0 || strcmp(command, "reload") == 0) &&
	    name != NULL) {
		fprintf(stderr, "service: %s takes no service name\n", command);
		return 2;
	}
	if (strcmp(command, "enable") == 0 || strcmp(command, "disable") == 0) {
		if (name == NULL || geteuid() != 0) {
			fprintf(
			    stderr,
			    "service: %s requires root and a service name\n",
			    command);
			return name == NULL ? 2 : 1;
		}
		if (rcconf_set_enabled(ZEDBSD_RC_CONF, name,
				       strcmp(command, "enable") == 0) != 0) {
			fprintf(stderr, "service: cannot update %s: %s\n",
				ZEDBSD_RC_CONF, strerror(errno));
			return 1;
		}
		return send_request("reload", NULL);
	}
	if (strcmp(command, "list") != 0 && strcmp(command, "reload") != 0 &&
	    strcmp(command, "status") != 0 && strcmp(command, "start") != 0 &&
	    strcmp(command, "stop") != 0 && strcmp(command, "restart") != 0) {
		fprintf(stderr, "service: unknown command: %s\n", command);
		return 2;
	}
	if (strcmp(command, "list") != 0 && strcmp(command, "reload") != 0 &&
	    name == NULL) {
		fprintf(stderr, "service: %s requires a service name\n",
			command);
		return 2;
	}
	return send_request(command, name);
}
