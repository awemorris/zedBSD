/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The blocking TCP sender (ws220-p002): connects to a host's port and
 * writes a number of bytes there with blocking writes, for the long
 * transfers of plan/ws220/tests/tcp-stream-series.sh.
 *
 *   tcp-stream ADDRESS PORT BYTES
 *
 * ADDRESS is an IPv4 address in dotted form.  The bytes are a fixed
 * pattern written CHUNK bytes at a time; a write that takes fewer is
 * continued from where it stopped.  At the end the sender shuts its side
 * down and waits for the peer to close, so every byte has been taken when
 * it exits.  It prints "TCP-STREAM done bytes=N" and exits 0, or prints
 * what failed and exits 1 (2 for a bad command line).
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The bytes one write offers. */
#define STREAM_CHUNK 65536U

/*
 * The pattern every write sends.
 *
 * It is filled once at the start and never changes, so each write offers
 * the same bytes.
 */
static unsigned char stream_pattern[STREAM_CHUNK];

static int stream_connect(const char *address_text, const char *port_text, int *descriptor);
static int stream_write_all(int descriptor, unsigned long long bytes, unsigned long long *written);
static int stream_wait_close(int descriptor);

/*
 * Sends the bytes the command line asks for.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned long long bytes;
	unsigned long long written;
	unsigned index;
	char *end;
	int descriptor;
	int error;

	/* Refuses a command line without the address, the port and the count. */
	if (argc != 4) {
		fprintf(stderr, "usage: tcp-stream ADDRESS PORT BYTES\n");
		return 2;
	}

	/* The count of bytes to send. */
	errno = 0;
	bytes = strtoull(argv[3], &end, 0);
	if (errno != 0 || *end != '\0') {
		fprintf(stderr, "tcp-stream: bad byte count %s\n", argv[3]);
		return 2;
	}

	/* The pattern, a byte that changes along the chunk. */
	for (index = 0; index < STREAM_CHUNK; index++)
		stream_pattern[index] = (unsigned char)((index * 131U + 7U) & 0xffU);

	/* The connection to the host. */
	error = stream_connect(argv[1], argv[2], &descriptor);
	if (error != 0)
		return 1;

	/* Every byte, written with blocking writes. */
	written = 0;
	error = stream_write_all(descriptor, bytes, &written);
	if (error != 0) {
		fprintf(stderr, "tcp-stream: write failed after %llu bytes: %s\n", written, strerror(error));
		close(descriptor);
		return 1;
	}

	/* The end of the stream, taken by the peer before the sender exits. */
	error = stream_wait_close(descriptor);
	close(descriptor);
	if (error != 0) {
		fprintf(stderr, "tcp-stream: closing failed: %s\n", strerror(error));
		return 1;
	}

	/* Succeeded: every byte was sent and the peer closed. */
	printf("TCP-STREAM done bytes=%llu\n", written);
	return 0;
}

/* Connects a blocking stream socket to the address and port given. */
static int
stream_connect(
	const char *address_text,
	const char *port_text,
	int *descriptor)
{
	struct sockaddr_in address;
	unsigned long port;
	char *end;
	int parsed;
	int error;

	/* The port, which must be a number of 16 bits. */
	errno = 0;
	port = strtoul(port_text, &end, 10);
	if (errno != 0 || *end != '\0' || port == 0 || port > 65535UL) {
		fprintf(stderr, "tcp-stream: bad port %s\n", port_text);
		return EINVAL;
	}

	/* The peer's address. */
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);
	parsed = inet_pton(AF_INET, address_text, &address.sin_addr);
	if (parsed != 1) {
		fprintf(stderr, "tcp-stream: bad address %s\n", address_text);
		return EINVAL;
	}

	/* The socket. */
	*descriptor = socket(AF_INET, SOCK_STREAM, 0);
	if (*descriptor < 0) {
		error = errno;
		fprintf(stderr, "tcp-stream: socket: %s\n", strerror(error));
		return error;
	}

	/* The connection, waited for. */
	error = connect(*descriptor, (struct sockaddr *)&address, sizeof(address));
	if (error != 0) {
		error = errno;
		fprintf(stderr, "tcp-stream: connect %s:%s: %s\n", address_text, port_text, strerror(error));
		close(*descriptor);
		return error;
	}

	/* Succeeded: the socket is connected. */
	return 0;
}

/*
 * Writes the count of bytes from the pattern, one chunk at a time.
 *
 * A write that takes fewer bytes than offered is continued from where it
 * stopped; one interrupted by a signal is offered again.
 */
static int
stream_write_all(
	int descriptor,
	unsigned long long bytes,
	unsigned long long *written)
{
	unsigned long long left;
	size_t offset;
	size_t length;
	ssize_t taken;
	int error;

	/* Each chunk until the count is written. */
	left = bytes;
	offset = 0;
	while (left != 0) {
		/* The part of the pattern still to offer, no more than what is left. */
		length = STREAM_CHUNK - offset;
		if ((unsigned long long)length > left)
			length = (size_t)left;

		/* The write, which blocks until the kernel takes some of it. */
		taken = write(descriptor, stream_pattern + offset, length);
		if (taken < 0) {
			error = errno;
			if (error == EINTR)
				continue;
			return error;
		}

		/* What was taken moves the count and the place in the pattern on. */
		*written += (unsigned long long)taken;
		left -= (unsigned long long)taken;
		offset = (offset + (size_t)taken) % STREAM_CHUNK;
	}

	/* Succeeded: every byte was written. */
	return 0;
}

/*
 * Shuts the sending side down and reads until the peer closes, so the
 * peer has taken everything when this returns.
 */
static int
stream_wait_close(
	int descriptor)
{
	char buffer[256];
	ssize_t got;
	int error;

	/* The end of the stream for the peer. */
	error = shutdown(descriptor, SHUT_WR);
	if (error != 0)
		return errno;

	/* Whatever the peer sends, until it closes. */
	for (;;) {
		got = read(descriptor, buffer, sizeof(buffer));
		if (got == 0)
			break;
		if (got < 0) {
			error = errno;
			if (error == EINTR)
				continue;
			return error;
		}
	}

	/* Succeeded: the peer closed after taking the stream. */
	return 0;
}
