/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: everything the client takes from the network, through
 * the public interface.  The input is what the server sends: it is
 * written into one end of a socket pair (which then reaches end of file),
 * and SSL_connect runs on the other end with verification off, so that
 * the records, the handshake messages in any order and split anywhere,
 * the alerts and the end of the stream all come from the fuzzer.  The
 * client's own writes stay in the socket's buffer.  An input up to 64 KiB
 * fits the buffer (-max_len 65536).
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <compat/crypto/err.h>
#include <compat/ssl/ssl.h>

/* The libFuzzer entry point, run once for every generated input. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * Runs one client connection against the input as the server's bytes.
 *
 * Always reports 0, because a refusal by the client is not a failure.
 */
int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	static SSL_CTX *ctx;
	char buffer[256];
	int pair[2];
	int room;
	size_t at;
	ssize_t done;
	ssize_t count;
	SSL *ssl;
	int status;

	/* Skips an input larger than the socket buffer holds. */
	if (size > 65536U)
		return 0;

	/* Makes the shared client context on the first input, with verification off. */
	if (ctx == NULL) {
		ctx = SSL_CTX_new(TLS_client_method());
		if (ctx == NULL)
			return 0;

		/* Accepts any certificate, since the input is not a real server. */
		SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
	}

	/* Makes the connection between the fuzzer and the client. */
	status = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (status != 0)
		return 0;

	/* Enlarges the buffers so the whole input and the client's writes fit. */
	room = 1 << 20;
	(void)setsockopt(pair[1], SOL_SOCKET, SO_SNDBUF, &room, sizeof(room));
	(void)setsockopt(pair[0], SOL_SOCKET, SO_RCVBUF, &room, sizeof(room));
	(void)setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &room, sizeof(room));

	/* The server's bytes, then the end of its stream. */
	for (at = 0; at < size; at += (size_t)done) {
		done = write(pair[1], data + at, size - at);
		if (done <= 0)
			break;
	}

	/* Ends the server's stream so the client sees the end of file. */
	shutdown(pair[1], SHUT_WR);

	/* Starts the client on the other end of the pair. */
	ssl = SSL_new(ctx);
	status = 0;
	if (ssl != NULL)
		status = SSL_set_fd(ssl, pair[0]);

	/* Runs the client against the server's bytes; a completed handshake reads what follows. */
	if (ssl != NULL && status == 1) {
		(void)SSL_set_tlsext_host_name(ssl, "fuzz.test");

		/* Runs the handshake over the server's bytes. */
		status = SSL_connect(ssl);
		if (status == 1) {
			/* Reads everything the server sent after the handshake. */
			for (;;) {
				count = SSL_read(ssl, buffer, sizeof(buffer));
				if (count <= 0)
					break;
			}

			/* Closes the finished connection. */
			(void)SSL_shutdown(ssl);
		}
	}

	/* Releases the client and both ends of the pair. */
	SSL_free(ssl);
	ERR_clear_error();
	close(pair[0]);
	close(pair[1]);

	/* The input never fails the run on its own. */
	return 0;
}
