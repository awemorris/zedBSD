/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ntpdate userland command.
 */

#include "userland/base/service/rcconf.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NTP_UNIX_EPOCH 2208988800ULL

static int query_server(const char *server, struct timespec *result);
static uint32_t read_u32(const unsigned char *input);

/*
 * Runs the ntpdate command.
 */
int
main(
	int argc,
	char **argv)
{
	char configured[512], copy[512], *server;
	struct rcconf_model *snapshot;
	struct timespec selected;
	int index;

	/* Handles a failed geteuid operation. */
	if (geteuid() != 0) {
		fprintf(stderr, "ntpdate: must be run as root\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (argc == 1) {
		snapshot = malloc(sizeof(*snapshot));

		/* Handles a failed rcconf load operation. */
		if (snapshot == NULL ||
		    rcconf_load(RCCONF_PATH, snapshot) != 0) {
			fprintf(stderr, "ntpdate: cannot load %s: %s\n",
				RCCONF_PATH, strerror(errno));
			free(snapshot);

			/* Reports operation failure. */
			return 1;
		}

		/* Handles a failed rcconf setting get operation. */
		if (rcconf_setting_get(snapshot, "ntpdate", "servers",
				       configured, sizeof(configured)) != 0 ||
		    configured[0] == '\0') {
			free(snapshot);
			fprintf(stderr, "ntpdate: no servers configured\n");

			/* Reports operation failure. */
			return 1;
		}
		free(snapshot);
		strcpy(copy, configured);

		/* Process each element required by the operation. */
		for (server = strtok(copy, " \t,"); server != NULL;
		     server = strtok(NULL, " \t,")) {
			/* Handles a failed query server operation. */
			if (query_server(server, &selected) == 0)
				goto set_clock;
		}
	} else {
		/* Process each remaining command-line operand. */
		for (index = 1; index < argc; index++) {
			/* Validates the command-line arguments. */
			if (query_server(argv[index], &selected) == 0)
				goto set_clock;
		}
	}
	fprintf(stderr, "ntpdate: no valid response\n");

	/* Reports operation failure. */
	return 1;

set_clock:

	/* Handles a failed clock settime operation. */
	if (clock_settime(CLOCK_REALTIME, &selected) != 0) {
		fprintf(stderr, "ntpdate: clock_settime: %s\n",
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	printf("ntpdate: clock set to %lld.%09ld\n", (long long)selected.tv_sec,
	       selected.tv_nsec);

	/* Reports successful completion. */
	return 0;
}

/* Supports the query server operation. */
static int
query_server(
	const char *server,
	struct timespec *result)
{
	uint64_t seconds;
	uint64_t fraction;
	struct addrinfo hints, *addresses, *current;
	struct timeval timeout = {3, 0};
	unsigned char request[48], response[48];
	int status, descriptor;
	ssize_t length;

	descriptor = -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	status = getaddrinfo(server, "123", &hints, &addresses);

	/* Checks the operation status. */
	if (status != 0) {
		fprintf(stderr, "ntpdate: %s: %s\n", server,
			gai_strerror(status));

		/* Reports operation failure. */
		return -1;
	}

	/* Process each element required by the operation. */
	for (current = addresses; current != NULL; current = current->ai_next) {
		descriptor = socket(current->ai_family, current->ai_socktype,
				    current->ai_protocol);

		/* Checks the file descriptor. */
		if (descriptor < 0)
			continue;
		(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
				 sizeof(timeout));
		memset(request, 0, sizeof(request));
		request[0] = 0x23;

		/* Handles a failed sendto operation. */
		if (sendto(descriptor, request, sizeof(request), 0,
			   current->ai_addr,
			   current->ai_addrlen) != (ssize_t)sizeof(request)) {
			close(descriptor);
			descriptor = -1;
			continue;
		}
		length = recv(descriptor, response, sizeof(response), 0);

		/* Checks the current data length. */
		if (length == (ssize_t)sizeof(response))
			break;
		close(descriptor);
		descriptor = -1;
	}
	freeaddrinfo(addresses);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;
	close(descriptor);

	/* Handles the response condition. */
	if ((response[0] & 7) != 4 || ((response[0] >> 3) & 7) < 3 ||
	    ((response[0] >> 3) & 7) > 4 || response[1] == 0 ||
	    response[1] > 15)

		/* Reports operation failure. */
		return -1;

	seconds = read_u32(response + 40);
	fraction = read_u32(response + 44);

	/* Handles the seconds condition. */
	if (seconds < NTP_UNIX_EPOCH)
		return -1;
	result->tv_sec = (time_t)(seconds - NTP_UNIX_EPOCH);
	result->tv_nsec = (long)((fraction * 1000000000ULL) >> 32);

	/* Reports successful completion. */
	return 0;
}

/* Supports the read u32 operation. */
static uint32_t
read_u32(
	const unsigned char *input)
{
	uint32_t function_result;
	uint32_t value;

	memcpy(&value, input, sizeof(value));

	/* Obtains the ntohl result. */
	function_result = ntohl(value);

	/* Returns the computed result. */
	return function_result;
}
