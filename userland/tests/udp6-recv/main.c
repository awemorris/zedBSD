/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A UDP receive probe over IPv6: binds a UDP socket to PORT on every
 * IPv6 address, and for each datagram that arrives prints
 * "UDP6 len=LENGTH fnv=HASH" (HASH the 32-bit FNV-1a of its bytes), until
 * COUNT datagrams came or SECONDS passed; then "UDP6-RECV done count=N".
 * It prints "UDP6-RECV ready port=PORT" once bound.
 *
 *   udp6-recv PORT SECONDS COUNT
 *
 * It exits 0, 1 when the socket failed, or 2 for a bad command line.
 */

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The longest datagram read, over the largest IPv6 payload. */
#define RECV_DATAGRAM_MAX	65536U

/* The longest wait in seconds, and the most datagrams. */
#define RECV_SECONDS_MAX	3600UL
#define RECV_COUNT_MAX		100000UL

/* The 32-bit FNV-1a's start and multiplier. */
#define RECV_FNV_BASIS		2166136261UL
#define RECV_FNV_PRIME		16777619UL

static int recv_number(const char *text, unsigned long minimum, unsigned long maximum, unsigned long *value);
static unsigned long recv_hash(const unsigned char *bytes, size_t length);

/* Takes datagrams on the port and prints each one's length and hash. */
int
main(
	int argc,
	char **argv)
{
	struct sockaddr_in6 address;
	struct pollfd waiting;
	unsigned long port;
	unsigned long seconds;
	unsigned long count;
	unsigned long received;
	unsigned long hash;
	unsigned char *buffer;
	time_t deadline;
	time_t now;
	ssize_t length;
	int descriptor;
	int ready;
	int error;

	/* Three arguments are wanted: the port, the time and the count. */
	if (argc != 4) {
		fprintf(stderr, "usage: udp6-recv PORT SECONDS COUNT\n");
		return 2;
	}

	/* The port to take datagrams on. */
	error = recv_number(argv[1], 1, 65535, &port);
	if (error != 0) {
		fprintf(stderr, "usage: udp6-recv PORT SECONDS COUNT\n");
		return 2;
	}

	/* How long to wait in all. */
	error = recv_number(argv[2], 1, RECV_SECONDS_MAX, &seconds);
	if (error != 0) {
		fprintf(stderr, "usage: udp6-recv PORT SECONDS COUNT\n");
		return 2;
	}

	/* How many datagrams to take. */
	error = recv_number(argv[3], 1, RECV_COUNT_MAX, &count);
	if (error != 0) {
		fprintf(stderr, "usage: udp6-recv PORT SECONDS COUNT\n");
		return 2;
	}

	/* The buffer for the datagrams. */
	buffer = malloc(RECV_DATAGRAM_MAX);
	if (buffer == NULL) {
		fprintf(stderr, "udp6-recv: no memory\n");
		return 1;
	}

	/* The socket. */
	descriptor = socket(AF_INET6, SOCK_DGRAM, 0);
	if (descriptor < 0) {
		fprintf(stderr, "udp6-recv: socket: %s\n", strerror(errno));
		free(buffer);
		return 1;
	}

	/* The address it listens on: every local IPv6 one, on the port. */
	memset(&address, 0, sizeof(address));
	address.sin6_family = AF_INET6;
	address.sin6_port = htons((unsigned short)port);
	address.sin6_addr = in6addr_any;
	error = bind(descriptor, (const struct sockaddr *)&address, sizeof(address));
	if (error != 0) {
		fprintf(stderr, "udp6-recv: bind: %s\n", strerror(errno));
		close(descriptor);
		free(buffer);
		return 1;
	}

	/* Bound: the sender may start. */
	printf("UDP6-RECV ready port=%lu\n", port);
	fflush(stdout);

	/* Each datagram until the count or the time is up. */
	received = 0;
	now = time(NULL);
	deadline = now + (time_t)seconds;
	while (received < count && now < deadline) {
		/* Waits for one, a second at a time. */
		waiting.fd = descriptor;
		waiting.events = POLLIN;
		waiting.revents = 0;
		ready = poll(&waiting, 1, 1000);
		now = time(NULL);

		/* Nothing came in that second, or the wait failed: waits again. */
		if (ready <= 0)
			continue;

		/* Takes it. */
		length = recv(descriptor, buffer, RECV_DATAGRAM_MAX, 0);
		if (length < 0)
			continue;

		/* Its length and hash. */
		hash = recv_hash(buffer, (size_t)length);
		printf("UDP6 len=%ld fnv=%08lx\n", (long)length, hash);
		fflush(stdout);
		received++;
	}

	/* What came. */
	printf("UDP6-RECV done count=%lu\n", received);
	close(descriptor);
	free(buffer);

	/* Succeeded: the datagrams were reported. */
	return 0;
}

/* Reads a decimal number in [minimum, maximum] into *value. */
static int
recv_number(
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

/* Gives the 32-bit FNV-1a hash of some bytes. */
static unsigned long
recv_hash(
	const unsigned char *bytes,
	size_t length)
{
	unsigned long hash;
	size_t index;

	/* Each byte folded in. */
	hash = RECV_FNV_BASIS;
	for (index = 0; index < length; index++) {
		hash ^= bytes[index];
		hash = (hash * RECV_FNV_PRIME) & 0xffffffffUL;
	}

	/* Succeeded: the hash. */
	return hash;
}
