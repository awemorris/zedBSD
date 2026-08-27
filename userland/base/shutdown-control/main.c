/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/zsv1-client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *
program_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash != NULL ? slash + 1 : path;
}

int
main(int argc, char **argv)
{
	struct zsv1_request request;
	struct zsv1_response response;
	const char *name = program_name(argv[0]);

	memset(&request, 0, sizeof(request));
	if (strcmp(name, "shutdown") == 0) {
		if (argc == 1 || (argc == 2 && strcmp(argv[1], "-h") == 0))
			request.command = ZSV1_COMMAND_POWEROFF;
		else if (argc == 2 && strcmp(argv[1], "-r") == 0)
			request.command = ZSV1_COMMAND_REBOOT;
		else {
			fprintf(stderr, "usage: shutdown [-h|-r]\n");
			return 2;
		}
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s\n", name);
		return 2;
	} else if (strcmp(name, "halt") == 0) {
		request.command = ZSV1_COMMAND_HALT;
	} else if (strcmp(name, "poweroff") == 0) {
		request.command = ZSV1_COMMAND_POWEROFF;
	} else if (strcmp(name, "reboot") == 0) {
		request.command = ZSV1_COMMAND_REBOOT;
	} else {
		fprintf(stderr, "%s: unsupported command name\n", name);
		return 2;
	}
	if (zsv1_client_call(ZSV1_INIT_SOCKET, &request, &response) != 0) {
		fprintf(stderr, "%s: init request failed: %s\n", name,
			strerror(errno));
		return 1;
	}
	if (response.error_present) {
		fprintf(stderr, "%s: init rejected request: %s (%d)\n", name,
			response.error_reason, response.error_number);
		return 1;
	}
	if (!response.ok_present ||
	    strcmp(response.ok_token, "scheduled") != 0 ||
	    response.service_count != 0 || response.dependency_count != 0) {
		fprintf(stderr, "%s: invalid init response\n", name);
		return 1;
	}
	return 0;
}
