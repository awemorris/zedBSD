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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

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
	SSL *ssl;

	if (size > 65536U)
		return 0;
	if (ctx == NULL) {
		ctx = SSL_CTX_new(TLS_client_method());
		if (ctx == NULL)
			return 0;
		SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		return 0;
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
	shutdown(pair[1], SHUT_WR);

	/* The client against them; a completed handshake reads what follows. */
	ssl = SSL_new(ctx);
	if (ssl != NULL && SSL_set_fd(ssl, pair[0]) == 1) {
		(void)SSL_set_tlsext_host_name(ssl, "fuzz.test");
		if (SSL_connect(ssl) == 1) {
			while (SSL_read(ssl, buffer, sizeof(buffer)) > 0)
				;
			(void)SSL_shutdown(ssl);
		}
	}
	SSL_free(ssl);
	ERR_clear_error();
	close(pair[0]);
	close(pair[1]);
	return 0;
}
