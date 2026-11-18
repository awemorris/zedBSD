/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ntpdate userland command.
 *
 * ntpdate [-w] [server ...] sets the clock from the first NTP server that
 * answers: the servers named, or those of rc.conf's ntpdate.servers.
 * With -w it waits for the network: when no server answers it tries them
 * again every NTPDATE_WAIT_SECONDS, NTPDATE_WAIT_TRIES times in all,
 * reporting only the outcome.  A machine without a clock of its own
 * starts it in the background at boot (the netclock service, BUG-320).
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

/* How long -w waits before it tries the servers again, and how many rounds it tries before it gives up (about ten minutes). */
#define NTPDATE_WAIT_SECONDS	10U
#define NTPDATE_WAIT_TRIES	60U

/* The longest list of servers, from rc.conf or the command line. */
#define NTPDATE_SERVERS_MAX	512U

static int servers_configured(char *servers, size_t size);
static int servers_named(int count, char **names, char *servers, size_t size);
static int query_servers(const char *servers, int report, struct timespec *result);
static int query_server(const char *server, int report, struct timespec *result);
static uint32_t read_u32(const unsigned char *input);

/*
 * Runs the ntpdate command.
 */
int
main(
	int argc,
	char **argv)
{
	char servers[NTPDATE_SERVERS_MAX];
	struct timespec selected;
	unsigned tries;
	uid_t user;
	int differs;
	int waiting;
	int first;
	int status;
	int error;

	/* Setting the clock needs root. */
	user = geteuid();
	if (user != 0) {
		fprintf(stderr, "ntpdate: must be run as root\n");
		return 1;
	}

	/* -w, which waits for the network, comes before the servers. */
	waiting = 0;
	first = 1;
	if (argc > 1) {
		differs = strcmp(argv[1], "-w");
		if (differs == 0) {
			waiting = 1;
			first = 2;
		}
	}

	/* The servers named on the command line, or else rc.conf's. */
	if (first < argc) {
		status = servers_named(argc - first, argv + first, servers, sizeof(servers));
	} else {
		status = servers_configured(servers, sizeof(servers));
	}

	/* A list that cannot be had was reported. */
	if (status != 0)
		return 1;

	/* One round over the servers, or with -w rounds until one answers or the tries run out. */
	tries = 0;
	for (;;) {
		status = query_servers(servers, !waiting, &selected);
		if (status == 0)
			break;

		/* Without -w one round is all; with it, the last round gives up. */
		tries++;
		if (!waiting || tries == NTPDATE_WAIT_TRIES) {
			fprintf(stderr, "ntpdate: no valid response\n");
			return 1;
		}

		/* The network may come up in the meantime. */
		sleep(NTPDATE_WAIT_SECONDS);
	}

	/* The clock, from the server's answer. */
	error = clock_settime(CLOCK_REALTIME, &selected);
	if (error != 0) {
		fprintf(stderr, "ntpdate: clock_settime: %s\n", strerror(errno));
		return 1;
	}

	/* The line that says what the clock was set to. */
	printf("ntpdate: clock set to %lld.%09ld\n", (long long)selected.tv_sec, selected.tv_nsec);

	/* Succeeded: the clock is the server's. */
	return 0;
}

/* Reads rc.conf's ntpdate.servers into servers; nonzero (reported) when there are none. */
static int
servers_configured(
	char *servers,
	size_t size)
{
	struct rcconf_model *snapshot;
	int error;

	/* The configuration, which is too large for the stack. */
	snapshot = malloc(sizeof(*snapshot));
	if (snapshot == NULL) {
		fprintf(stderr, "ntpdate: cannot load %s: %s\n", RCCONF_PATH, strerror(ENOMEM));
		return 1;
	}

	/* rc.conf as it is now. */
	error = rcconf_load(RCCONF_PATH, snapshot);
	if (error != 0) {
		fprintf(stderr, "ntpdate: cannot load %s: %s\n", RCCONF_PATH, strerror(errno));
		free(snapshot);
		return 1;
	}

	/* The servers it names. */
	error = rcconf_setting_get(snapshot, "ntpdate", "servers", servers, size);
	free(snapshot);
	if (error != 0) {
		fprintf(stderr, "ntpdate: no servers configured\n");
		return 1;
	}

	/* An empty setting names none. */
	if (servers[0] == '\0') {
		fprintf(stderr, "ntpdate: no servers configured\n");
		return 1;
	}

	/* Succeeded: servers holds the list. */
	return 0;
}

/* Joins the servers named on the command line into one list; nonzero (reported) when it does not fit. */
static int
servers_named(
	int count,
	char **names,
	char *servers,
	size_t size)
{
	size_t used;
	size_t length;
	int index;

	/* Each name after a space. */
	used = 0;
	servers[0] = '\0';
	for (index = 0; index < count; index++) {
		/* A name that does not fit with its space and the NUL ends the list. */
		length = strlen(names[index]);
		if (used + length + 2U > size) {
			fprintf(stderr, "ntpdate: too many servers\n");
			return 1;
		}

		/* The name, its space and the NUL after them. */
		memcpy(servers + used, names[index], length);
		used += length;
		servers[used] = ' ';
		used++;
		servers[used] = '\0';
	}

	/* Succeeded: servers holds the list. */
	return 0;
}

/* Asks each server of a list (separated by spaces, tabs or commas) in turn; 0 with the first answer's time. */
static int
query_servers(
	const char *servers,
	int report,
	struct timespec *result)
{
	char copy[NTPDATE_SERVERS_MAX];
	char *server;
	int status;

	/* A copy that strtok can cut. */
	snprintf(copy, sizeof(copy), "%s", servers);

	/* The servers in order, until one answers. */
	for (server = strtok(copy, " \t,");
	     server != NULL;
	     server = strtok(NULL, " \t,")) {
		status = query_server(server, report, result);
		if (status == 0)
			return 0;
	}

	/* No server answered. */
	return -1;
}

/* Asks one server for the time (over IPv4, the first of its addresses that answers); 0 with the time, -1 when none answers. */
static int
query_server(
	const char *server,
	int report,
	struct timespec *result)
{
	struct addrinfo hints;
	struct addrinfo *addresses;
	struct addrinfo *current;
	struct timeval timeout;
	unsigned char request[48];
	unsigned char response[48];
	uint64_t seconds;
	uint64_t fraction;
	ssize_t length;
	ssize_t sent;
	int descriptor;
	int status;
	unsigned mode;
	unsigned version;

	/* The server's IPv4 addresses for the NTP port; a name that does not resolve is reported unless the caller retries quietly. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	status = getaddrinfo(server, "123", &hints, &addresses);
	if (status != 0) {
		if (report)
			fprintf(stderr, "ntpdate: %s: %s\n", server, gai_strerror(status));
		return -1;
	}

	/* Each address in turn, until one answers with a whole packet within the timeout. */
	descriptor = -1;
	for (current = addresses; current != NULL; current = current->ai_next) {
		/* A socket that waits at most three seconds for the answer. */
		descriptor = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
		if (descriptor < 0)
			continue;
		timeout.tv_sec = 3;
		timeout.tv_usec = 0;
		(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

		/* The client's request: no leap warning, version 4, mode 3 (client). */
		memset(request, 0, sizeof(request));
		request[0] = 0x23;
		sent = sendto(descriptor, request, sizeof(request), 0, current->ai_addr, current->ai_addrlen);
		if (sent != (ssize_t)sizeof(request)) {
			close(descriptor);
			descriptor = -1;
			continue;
		}

		/* The answer, which must be a whole packet. */
		length = recv(descriptor, response, sizeof(response), 0);
		if (length == (ssize_t)sizeof(response))
			break;
		close(descriptor);
		descriptor = -1;
	}

	/* The addresses are not needed once one answered or none did. */
	freeaddrinfo(addresses);
	if (descriptor < 0)
		return -1;
	close(descriptor);

	/* A server's answer: mode 4, version 3 or 4, and a stratum from 1 to 15 (0 is a kiss-o'-death). */
	mode = response[0] & 7U;
	version = (response[0] >> 3) & 7U;
	if (mode != 4U)
		return -1;
	if (version < 3U || version > 4U)
		return -1;
	if (response[1] == 0 || response[1] > 15)
		return -1;

	/* The transmit time's seconds since 1900 and their fraction, which must fall after 1970. */
	seconds = read_u32(response + 40);
	fraction = read_u32(response + 44);
	if (seconds < NTP_UNIX_EPOCH)
		return -1;

	/* The time as the clock takes it. */
	result->tv_sec = (time_t)(seconds - NTP_UNIX_EPOCH);
	result->tv_nsec = (long)((fraction * 1000000000ULL) >> 32);

	/* Succeeded: result holds the server's time. */
	return 0;
}

/* Reads a big-endian 32-bit field of an NTP packet. */
static uint32_t
read_u32(
	const unsigned char *input)
{
	uint32_t network;
	uint32_t host;

	/* The field's bytes, in the host's order. */
	memcpy(&network, input, sizeof(network));
	host = ntohl(network);

	/* Succeeded: the field's value. */
	return host;
}
