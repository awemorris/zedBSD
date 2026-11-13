/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One connection of Mail's TLS (userland/desktop/mailer/tls.c) to a test
 * server, for run-mailer-tls.sh: the program is built with libssl-compat
 * and libcrypto-compat (host static archives), and each case is one run.
 *
 *   mailer-tls <ca.pem|-> <port> <pin|-> ok
 *       the connection is made; "hello" sent, the server's reversed line
 *       ("olleh", openssl s_server -rev) read back
 *   mailer-tls <ca.pem|-> <port> <pin|-> untrusted <fingerprint>
 *       ML_ERROR_UNTRUSTED, and ml_tls_fingerprint is the given one
 *
 * The host name is always localhost; the server is at 127.0.0.1, or at the
 * IPv4 address in MAILER_TLS_ADDRESS (the host as a QEMU guest sees it).
 * The last line is "mailer-tls: PASS <what>" or "mailer-tls: FAIL <why>";
 * the exit status says the same.
 */

#include "userland/desktop/mailer/mail.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Reports a failure and ends the run. */
static int
fail(
	const char *why)
{
	printf("mailer-tls: FAIL %s (error \"%s\", fingerprint \"%s\")\n", why, ml_tls_error(), ml_tls_fingerprint());
	return EXIT_FAILURE;
}

/* Connects to the server (127.0.0.1, or MAILER_TLS_ADDRESS) at a port; -1 on failure. */
static int
connect_local(
	int port)
{
	struct sockaddr_in address;
	const char *text;
	int descriptor;

	/* The socket. */
	descriptor = socket(AF_INET, SOCK_STREAM, 0);
	if (descriptor < 0)
		return -1;

	/* The server on the loopback address. */
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	text = getenv("MAILER_TLS_ADDRESS");
	if (text != NULL && inet_pton(AF_INET, text, &address.sin_addr) != 1) {
		close(descriptor);
		return -1;
	}
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0) {
		close(descriptor);
		return -1;
	}

	/* Succeeded: a connected socket. */
	return descriptor;
}

/* Sends a line and reads the server's answer up to its newline into line (NUL-ended, without the newline). */
static int
exchange(
	void *tls,
	char *line,
	size_t size)
{
	static const unsigned char hello[] = "hello\n";
	unsigned char byte;
	size_t received;
	size_t used;
	int error;

	/* The line. */
	error = ml_tls_write(tls, hello, sizeof(hello) - 1U);
	if (error != 0)
		return error;

	/* The answer, a byte at a time, to its newline. */
	used = 0;
	for (;;) {
		error = ml_tls_read(tls, &byte, 1U, &received);
		if (error != 0)
			return error;
		if (received == 0U)
			return EPIPE;
		if (byte == '\n')
			break;
		if (byte != '\r' && used + 1U < size)
			line[used++] = (char)byte;
	}

	/* Its end. */
	line[used] = '\0';
	return 0;
}

int
main(
	int argc,
	char **argv)
{
	const char *pin;
	const char *expect;
	char line[64];
	void *tls;
	int descriptor;
	int error;

	/* The arguments. */
	if (argc < 5) {
		printf("usage: mailer-tls <ca.pem|-> <port> <pin|-> ok|untrusted <fingerprint>\n");
		return EXIT_FAILURE;
	}
	pin = argv[3];
	if (strcmp(pin, "-") == 0)
		pin = NULL;
	expect = argv[4];
	if (strcmp(argv[1], "-") != 0 && ml_tls_add_ca_file(argv[1]) != 0)
		return fail("ml_tls_add_ca_file");

	/* The connection. */
	descriptor = connect_local(atoi(argv[2]));
	if (descriptor < 0)
		return fail("cannot connect");
	tls = NULL;
	error = ml_tls_open(descriptor, "localhost", pin, &tls);

	/* A certificate that is not trusted, with its fingerprint. */
	if (strcmp(expect, "untrusted") == 0) {
		close(descriptor);
		if (argc < 6)
			return fail("no fingerprint given");
		if (error != ML_ERROR_UNTRUSTED)
			return fail("not ML_ERROR_UNTRUSTED");
		if (strcmp(ml_tls_fingerprint(), argv[5]) != 0)
			return fail("another fingerprint");
		printf("mailer-tls: PASS untrusted: %s; %s\n", ml_tls_error(), ml_tls_fingerprint());
		return EXIT_SUCCESS;
	}

	/* A connection made: a line each way, then the close. */
	if (error != 0)
		return fail("ml_tls_open failed");
	error = exchange(tls, line, sizeof(line));
	ml_tls_close(tls);
	close(descriptor);
	if (error != 0)
		return fail("the exchange failed");
	if (strcmp(line, "olleh") != 0)
		return fail("another answer");
	printf("mailer-tls: PASS ok: answer \"%s\"\n", line);
	return EXIT_SUCCESS;
}
