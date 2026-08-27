/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static uint32_t
read_u32(const unsigned char *input)
{
	uint32_t value;
	memcpy(&value, input, sizeof(value));
	return ntohl(value);
}

static int
query_server(const char *server, struct timespec *result)
{
	struct addrinfo hints, *addresses, *current;
	struct timeval timeout = {3, 0};
	unsigned char request[48], response[48];
	int status, descriptor = -1;
	ssize_t length;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	status = getaddrinfo(server, "123", &hints, &addresses);
	if (status != 0) {
		fprintf(stderr, "ntpdate: %s: %s\n", server,
			gai_strerror(status));
		return -1;
	}
	for (current = addresses; current != NULL; current = current->ai_next) {
		descriptor = socket(current->ai_family, current->ai_socktype,
				    current->ai_protocol);
		if (descriptor < 0)
			continue;
		(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
				 sizeof(timeout));
		memset(request, 0, sizeof(request));
		request[0] = 0x23;
		if (sendto(descriptor, request, sizeof(request), 0,
			   current->ai_addr,
			   current->ai_addrlen) != (ssize_t)sizeof(request)) {
			close(descriptor);
			descriptor = -1;
			continue;
		}
		length = recv(descriptor, response, sizeof(response), 0);
		if (length == (ssize_t)sizeof(response))
			break;
		close(descriptor);
		descriptor = -1;
	}
	freeaddrinfo(addresses);
	if (descriptor < 0)
		return -1;
	close(descriptor);
	if ((response[0] & 7) != 4 || ((response[0] >> 3) & 7) < 3 ||
	    ((response[0] >> 3) & 7) > 4 || response[1] == 0 ||
	    response[1] > 15)
		return -1;
	{
		uint64_t seconds = read_u32(response + 40);
		uint64_t fraction = read_u32(response + 44);
		if (seconds < NTP_UNIX_EPOCH)
			return -1;
		result->tv_sec = (time_t)(seconds - NTP_UNIX_EPOCH);
		result->tv_nsec = (long)((fraction * 1000000000ULL) >> 32);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	char configured[512], copy[512], *server;
	struct rcconf_model *snapshot;
	struct timespec selected;
	int index;
	if (geteuid() != 0) {
		fprintf(stderr, "ntpdate: must be run as root\n");
		return 1;
	}
	if (argc == 1) {
		snapshot = malloc(sizeof(*snapshot));
		if (snapshot == NULL ||
		    rcconf_load(RCCONF_PATH, snapshot) != 0) {
			fprintf(stderr, "ntpdate: cannot load %s: %s\n",
				RCCONF_PATH, strerror(errno));
			free(snapshot);
			return 1;
		}
		if (rcconf_setting_get(snapshot, "ntpdate", "servers",
				       configured, sizeof(configured)) != 0 ||
		    configured[0] == '\0') {
			free(snapshot);
			fprintf(stderr, "ntpdate: no servers configured\n");
			return 1;
		}
		free(snapshot);
		strcpy(copy, configured);
		for (server = strtok(copy, " \t,"); server != NULL;
		     server = strtok(NULL, " \t,"))
			if (query_server(server, &selected) == 0)
				goto set_clock;
	} else {
		for (index = 1; index < argc; index++)
			if (query_server(argv[index], &selected) == 0)
				goto set_clock;
	}
	fprintf(stderr, "ntpdate: no valid response\n");
	return 1;

set_clock:
	if (clock_settime(CLOCK_REALTIME, &selected) != 0) {
		fprintf(stderr, "ntpdate: clock_settime: %s\n",
			strerror(errno));
		return 1;
	}
	printf("ntpdate: clock set to %lld.%09ld\n", (long long)selected.tv_sec,
	       selected.tv_nsec);
	return 0;
}
