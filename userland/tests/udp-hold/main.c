/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A UDP receive queue probe: binds a UDP socket to PORT on every IPv4
 * address, leaves what arrives unread for SECONDS, then takes every
 * datagram still queued without waiting and prints
 * "UDP-HOLD port=PORT queued=N bytes=TOTAL largest=LENGTH".
 *
 *   udp-hold PORT SECONDS
 *
 * A peer sends its datagrams within the hold.  It exits 0, 1 when the
 * socket failed, or 2 for a bad command line.
 */

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The longest datagram read back, over the largest IPv4 one. */
#define HOLD_DATAGRAM_MAX 65536U

/* The longest hold in seconds. */
#define HOLD_SECONDS_MAX 3600UL

static int hold_number(const char *text, unsigned long minimum, unsigned long maximum, unsigned long *value);

/* Reads a decimal number in [minimum, maximum] into *value. */
static int
hold_number(
	const char *text,
	unsigned long minimum,
	unsigned long maximum,
	unsigned long *value)
{
	char *end;

	/* Reads it whole. */
	errno = 0;
	*value = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return EINVAL;

	/* Refuses one outside the range. */
	if (*value < minimum || *value > maximum)
		return EINVAL;

	/* Succeeded: the value is in range. */
	return 0;
}

int
main(
	int argc,
	char **argv)
{
	struct sockaddr_in address;
	unsigned long port;
	unsigned long seconds;
	unsigned long long total;
	unsigned long queued;
	unsigned char *buffer;
	ssize_t length;
	ssize_t largest;
	int descriptor;
	int error;

	/* Reads the port and the hold. */
	if (argc != 3) {
		fprintf(stderr, "usage: udp-hold PORT SECONDS\n");
		return 2;
	}

	/* The port to hold. */
	error = hold_number(argv[1], 1, 65535, &port);
	if (error != 0) {
		fprintf(stderr, "usage: udp-hold PORT SECONDS\n");
		return 2;
	}

	/* How long to hold. */
	error = hold_number(argv[2], 1, HOLD_SECONDS_MAX, &seconds);
	if (error != 0) {
		fprintf(stderr, "usage: udp-hold PORT SECONDS\n");
		return 2;
	}

	/* The buffer for the datagrams read back. */
	buffer = malloc(HOLD_DATAGRAM_MAX);
	if (buffer == NULL) {
		fprintf(stderr, "udp-hold: no memory\n");
		return 1;
	}

	/* Binds the socket. */
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0) {
		fprintf(stderr, "udp-hold: socket: %s\n", strerror(errno));
		free(buffer);
		return 1;
	}

	/* The address it listens on: every local one, on the port. */
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	error = bind(descriptor, (const struct sockaddr *)&address, sizeof(address));
	if (error != 0) {
		fprintf(stderr, "udp-hold: bind: %s\n", strerror(errno));
		close(descriptor);
		free(buffer);
		return 1;
	}

	/* Holds what arrives unread. */
	printf("udp-hold: holding port %lu for %lu s\n", port, seconds);
	fflush(stdout);
	sleep((unsigned)seconds);

	/* Takes what is queued, without waiting for more. */
	queued = 0;
	total = 0;
	largest = 0;
	for (;;) {
		length = recv(descriptor, buffer, HOLD_DATAGRAM_MAX, MSG_DONTWAIT);
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0)
			break;
		queued++;
		total += (unsigned long long)length;
		if (length > largest)
			largest = length;
	}

	/* Reports the queue. */
	printf("UDP-HOLD port=%lu queued=%lu bytes=%llu largest=%ld\n", port, queued, total, (long)largest);
	close(descriptor);
	free(buffer);

	/* Succeeded: the queue was reported. */
	return 0;
}
