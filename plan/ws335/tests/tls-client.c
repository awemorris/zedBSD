/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * tls-client: a small TLS client over the public interface of
 * libssl-compat (<compat/ssl/ssl.h>, <compat/crypto/err.h>,
 * <compat/crypto/x509.h>) for the interop tests.  It is built for the
 * host (with OpenSSL's libssl too: the calls are OpenSSL's), for zedBSD
 * and for Windows (the socket layer and WSAStartup are this program's).
 *
 *   tls-client [options] host port [cafile]
 *
 * It connects to host (or to -a address), runs the handshake with the
 * certificate checked against cafile for host, then talks to an OpenSSL
 * "s_server -rev" (each line comes back reversed) or, with -g, sends an
 * HTTP GET and reads the answer to its end.
 *
 *   -a address   Connect to this address instead of host (host stays the
 *                name to verify and the SNI name).
 *   -v version   1.2 or 1.3: only that version.
 *   -n           A non-blocking socket; the program waits with select.
 *   -k           No verification (SSL_VERIFY_NONE).
 *   -d           The trust store from SSL_CTX_set_default_verify_paths
 *                (cafile, when given, is loaded as well).
 *   -p           The name through SSL_get0_param and
 *                X509_VERIFY_PARAM_set1_host (or _set1_ip_asc for an
 *                address) instead of SSL_set1_host.
 *   -s size      The line sent to the reversing server: size bytes and a
 *                newline (default 64; 0 sends nothing).
 *   -r size      The buffer of each SSL_read (default 16384); a small one
 *                leaves data buffered, which SSL_pending must report.
 *   -P           Some read must have left data that SSL_pending reports
 *                (for a read buffer smaller than the server's records).
 *   -c close     How the connection ends:
 *                  notify   (default) the line "CLOSE"; the server's
 *                           close_notify must end the reading
 *                           (SSL_ERROR_ZERO_RETURN), then SSL_shutdown.
 *                  eof      "tls-client: waiting for eof" is printed and
 *                           the server is expected to vanish without a
 *                           close_notify: the read must fail with
 *                           SSL_R_UNEXPECTED_EOF_WHILE_READING.
 *                  eof-ok   The same with SSL_OP_IGNORE_UNEXPECTED_EOF:
 *                           the read must end with ZERO_RETURN.
 *                  client   SSL_shutdown from this side after the line.
 *   -g           HTTP: "GET / HTTP/1.1" with Host and Connection: close;
 *                the answer must start with "HTTP/1." and is read until
 *                the server closes (a close_notify or an EOF).
 *   -x expect    The handshake must fail: "verify=N" with
 *                SSL_get_verify_result N, or "reason=N" with that
 *                ERR_GET_REASON of the first queued error.
 *   -t seconds   The time allowed for each wait (default 30).
 *
 * Output: "tls-client: ..." lines; the last is "tls-client: PASS" (exit 0)
 * or "tls-client: FAIL <why>" (exit 1).
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/crypto/err.h>
#include <compat/crypto/x509.h>
#include <compat/ssl/ssl.h>

#ifdef _WIN32
typedef SOCKET client_socket;
#define CLIENT_NO_SOCKET INVALID_SOCKET
#else
typedef int client_socket;
#define CLIENT_NO_SOCKET (-1)
#endif

/* The largest line the reversing server takes back in one piece (its buffer is 16 KiB). */
#define LINE_MAX_SIZE	16000

/* The settings from the command line. */
struct options {
	const char *host;
	const char *address;
	const char *port;
	const char *cafile;
	int version;
	int nonblocking;
	int verify;
	int default_paths;
	int by_param;
	long line_size;
	long read_size;
	const char *close_mode;
	int http;
	int expect_pending;
	long expect_verify;
	long expect_reason;
	int expect_failure;
	long timeout;
};

/* Prints the result line and returns the exit status. */
static int
finish(
	const char *why)
{
	if (why == NULL) {
		printf("tls-client: PASS\n");
		fflush(stdout);
		return 0;
	}
	printf("tls-client: FAIL %s\n", why);
	fflush(stdout);
	return 1;
}

/* Prints every queued error (library, reason and text), oldest first, and empties the queue. */
static void
print_errors(void)
{
	char text[256];
	unsigned long code;

	for (;;) {
		code = ERR_get_error();
		if (code == 0UL)
			break;
		ERR_error_string_n(code, text, sizeof(text));
		printf("tls-client: error lib %d reason %d: %s\n", ERR_GET_LIB(code), ERR_GET_REASON(code), text);
	}
}

/* Returns 1 when text is an IPv4 or IPv6 address literal (digits and dots, or a colon). */
static int
is_address(
	const char *text)
{
	const char *at;

	if (strchr(text, ':') != NULL)
		return 1;
	for (at = text; *at != '\0'; at++) {
		if ((*at < '0' || *at > '9') && *at != '.')
			return 0;
	}
	return 1;
}

/* Reads the options; returns 0, or -1 after printing the usage. */
static int
parse_options(
	int argc,
	char **argv,
	struct options *options)
{
	int index;
	const char *argument;

	memset(options, 0, sizeof(*options));
	options->verify = 1;
	options->line_size = 64;
	options->read_size = 16384;
	options->close_mode = "notify";
	options->timeout = 30;
	index = 1;
	while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
		argument = argv[index];
		index++;

		/* The flags without a value. */
		if (strcmp(argument, "-n") == 0) {
			options->nonblocking = 1;
			continue;
		}
		if (strcmp(argument, "-k") == 0) {
			options->verify = 0;
			continue;
		}
		if (strcmp(argument, "-d") == 0) {
			options->default_paths = 1;
			continue;
		}
		if (strcmp(argument, "-p") == 0) {
			options->by_param = 1;
			continue;
		}
		if (strcmp(argument, "-g") == 0) {
			options->http = 1;
			continue;
		}
		if (strcmp(argument, "-P") == 0) {
			options->expect_pending = 1;
			continue;
		}

		/* The options with a value. */
		if (index >= argc)
			break;
		if (strcmp(argument, "-a") == 0) {
			options->address = argv[index++];
		} else if (strcmp(argument, "-v") == 0) {
			if (strcmp(argv[index], "1.2") == 0)
				options->version = TLS1_2_VERSION;
			else if (strcmp(argv[index], "1.3") == 0)
				options->version = TLS1_3_VERSION;
			else
				break;
			index++;
		} else if (strcmp(argument, "-s") == 0) {
			options->line_size = atol(argv[index++]);
		} else if (strcmp(argument, "-r") == 0) {
			options->read_size = atol(argv[index++]);
		} else if (strcmp(argument, "-c") == 0) {
			options->close_mode = argv[index++];
		} else if (strcmp(argument, "-t") == 0) {
			options->timeout = atol(argv[index++]);
		} else if (strcmp(argument, "-x") == 0) {
			options->expect_failure = 1;
			options->expect_verify = -1;
			options->expect_reason = -1;
			if (strncmp(argv[index], "verify=", 7) == 0)
				options->expect_verify = atol(argv[index] + 7);
			else if (strncmp(argv[index], "reason=", 7) == 0)
				options->expect_reason = atol(argv[index] + 7);
			else
				break;
			index++;
		} else {
			break;
		}
	}
	if (argc - index < 2 || argc - index > 3 ||
	    options->line_size < 0 || options->line_size > LINE_MAX_SIZE ||
	    options->read_size < 1 || options->read_size > 65536 || options->timeout < 1) {
		printf("usage: tls-client [-a address] [-v 1.2|1.3] [-n] [-k] [-d] [-p] [-s size] [-r size] [-P]\n"
		    "    [-c notify|eof|eof-ok|client] [-g] [-x verify=N|reason=N] [-t seconds] host port [cafile]\n");
		return -1;
	}
	options->host = argv[index];
	options->port = argv[index + 1];
	if (argc - index == 3)
		options->cafile = argv[index + 2];
	if (options->address == NULL)
		options->address = options->host;
	return 0;
}

/* Closes a socket. */
static void
close_socket(
	client_socket socket_value)
{
#ifdef _WIN32
	closesocket(socket_value);
#else
	close(socket_value);
#endif
}

/* Connects a TCP socket to address and port (blocking); returns it or CLIENT_NO_SOCKET. */
static client_socket
connect_to(
	const char *address,
	const char *port)
{
	struct addrinfo hints;
	struct addrinfo *list;
	struct addrinfo *entry;
	client_socket result;
	int error;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	error = getaddrinfo(address, port, &hints, &list);
	if (error != 0) {
		printf("tls-client: getaddrinfo %s %s: %d\n", address, port, error);
		return CLIENT_NO_SOCKET;
	}

	/* The first address that connects. */
	result = CLIENT_NO_SOCKET;
	for (entry = list; entry != NULL; entry = entry->ai_next) {
		result = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
		if (result == CLIENT_NO_SOCKET)
			continue;
		if (connect(result, entry->ai_addr, (int)entry->ai_addrlen) == 0)
			break;
		close_socket(result);
		result = CLIENT_NO_SOCKET;
	}
	freeaddrinfo(list);
	if (result == CLIENT_NO_SOCKET)
		printf("tls-client: cannot connect to %s port %s\n", address, port);
	return result;
}

/* Makes the socket non-blocking; returns 0 or -1. */
static int
set_nonblocking(
	client_socket socket_value)
{
#ifdef _WIN32
	u_long on;

	on = 1;
	return ioctlsocket(socket_value, FIONBIO, &on) == 0 ? 0 : -1;
#else
	int flags;

	flags = fcntl(socket_value, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

/*
 * After a call on ssl returned result: when SSL_get_error says the
 * socket would block, waits until it is ready (at most seconds) and
 * returns 1 (call again); otherwise returns 0 and stores the
 * classification in *kind.
 */
static int
wait_again(
	SSL *ssl,
	int result,
	client_socket socket_value,
	long seconds,
	int *kind,
	unsigned long *waits)
{
	fd_set read_set;
	fd_set write_set;
	struct timeval limit;
	int ready;

	*kind = SSL_get_error(ssl, result);
	if (*kind != SSL_ERROR_WANT_READ && *kind != SSL_ERROR_WANT_WRITE)
		return 0;

	/* select on the one direction the library waits for. */
	FD_ZERO(&read_set);
	FD_ZERO(&write_set);
	if (*kind == SSL_ERROR_WANT_READ)
		FD_SET(socket_value, &read_set);
	else
		FD_SET(socket_value, &write_set);
	limit.tv_sec = seconds;
	limit.tv_usec = 0;
	ready = select((int)socket_value + 1, &read_set, &write_set, NULL, &limit);
	if (ready <= 0) {
		printf("tls-client: no progress in %ld seconds (select %d)\n", seconds, ready);
		return 0;
	}
	(*waits)++;
	return 1;
}

/* Fills line with size bytes of a pattern (letters and digits) and a newline; the reversal of it is in reversed. */
static void
make_line(
	char *line,
	char *reversed,
	long size)
{
	static const char pattern[] = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	long index;

	for (index = 0; index < size; index++)
		line[index] = pattern[(index * 7L + index / 61L) % (long)(sizeof(pattern) - 1U)];
	line[size] = '\n';
	for (index = 0; index < size; index++)
		reversed[index] = line[size - 1L - index];
	reversed[size] = '\n';
}

/* Writes all of data (repeating the call after a wait with the same arguments); returns 0 or -1. */
static int
write_all(
	SSL *ssl,
	client_socket socket_value,
	const struct options *options,
	const char *data,
	int size,
	unsigned long *waits)
{
	int result;
	int kind;

	for (;;) {
		result = SSL_write(ssl, data, size);
		if (result == size)
			return 0;
		if (result > 0) {
			printf("tls-client: SSL_write wrote %d of %d\n", result, size);
			return -1;
		}
		if (!wait_again(ssl, result, socket_value, options->timeout, &kind, waits)) {
			printf("tls-client: SSL_write failed: SSL_get_error %d\n", kind);
			print_errors();
			return -1;
		}
	}
}

/*
 * Reads into buffer (up to room bytes) until stop bytes were read or the
 * connection ends; returns the count and stores the final
 * classification (SSL_ERROR_NONE when stop was reached) in *kind.
 * Checks SSL_pending after each read.
 */
static long
read_some(
	SSL *ssl,
	client_socket socket_value,
	const struct options *options,
	char *buffer,
	long room,
	long stop,
	int *kind,
	unsigned long *waits,
	unsigned long *pending_seen,
	int *pending_bad)
{
	long count;
	long ask;
	int result;
	int pending;

	count = 0;
	*kind = SSL_ERROR_NONE;
	while (count < stop) {
		ask = options->read_size;
		if (ask > room - count)
			ask = room - count;
		if (ask <= 0)
			break;
		result = SSL_read(ssl, buffer + count, (int)ask);
		if (result > 0) {
			count += result;

			/* What is left of the record must be reported, and be readable at once. */
			pending = SSL_pending(ssl);
			if (pending < 0 || pending > 16384)
				*pending_bad = 1;
			if (pending > 0)
				(*pending_seen)++;
			continue;
		}
		if (!wait_again(ssl, result, socket_value, options->timeout, kind, waits))
			return count;
	}
	return count;
}

/* The verify callback of the expected-failure runs: reports what it was told and keeps the library's verdict. */
static int
report_verify(
	int preverify_ok,
	X509_STORE_CTX *store)
{
	(void)store;
	printf("tls-client: verify callback preverify_ok %d\n", preverify_ok);
	return preverify_ok;
}

/* Runs the connection; returns NULL or the reason it failed. */
static const char *
run(
	const struct options *options)
{
	static char line[LINE_MAX_SIZE + 1];
	static char reversed[LINE_MAX_SIZE + 1];
	static char answer[65536];
	char request[512];
	SSL_CTX *ctx;
	SSL *ssl;
	client_socket socket_value;
	const char *why;
	long verify;
	long count;
	unsigned long waits;
	unsigned long pending_seen;
	unsigned long code;
	int pending_bad;
	int result;
	int kind;

	why = NULL;
	ssl = NULL;
	kind = SSL_ERROR_NONE;
	socket_value = CLIENT_NO_SOCKET;
	waits = 0;
	pending_seen = 0;
	pending_bad = 0;

	/* The context: versions, verification, the trust store. */
	ctx = SSL_CTX_new(TLS_client_method());
	if (ctx == NULL)
		return "SSL_CTX_new";
	if (options->version != 0) {
		if (SSL_CTX_set_min_proto_version(ctx, options->version) != 1 ||
		    SSL_CTX_set_max_proto_version(ctx, options->version) != 1) {
			why = "version ctrl";
			goto out;
		}
	}
	if (strcmp(options->close_mode, "eof-ok") == 0)
		SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
	SSL_CTX_set_verify(ctx, options->verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
	    options->expect_failure ? report_verify : NULL);
	if (options->default_paths && SSL_CTX_set_default_verify_paths(ctx) != 1) {
		why = "SSL_CTX_set_default_verify_paths";
		goto out;
	}
	if (options->cafile != NULL && SSL_CTX_load_verify_locations(ctx, options->cafile, NULL) != 1) {
		print_errors();
		why = "SSL_CTX_load_verify_locations";
		goto out;
	}

	/* The socket and the connection object: SNI for a name, the name or address to verify. */
	socket_value = connect_to(options->address, options->port);
	if (socket_value == CLIENT_NO_SOCKET) {
		why = "connect";
		goto out;
	}
	if (options->nonblocking && set_nonblocking(socket_value) != 0) {
		why = "non-blocking";
		goto out;
	}
	ssl = SSL_new(ctx);
	if (ssl == NULL || SSL_set_fd(ssl, (int)socket_value) != 1) {
		why = "SSL_new or SSL_set_fd";
		goto out;
	}
	if (!is_address(options->host) && SSL_set_tlsext_host_name(ssl, options->host) != 1) {
		why = "SSL_set_tlsext_host_name";
		goto out;
	}
	if (options->by_param) {
		if (is_address(options->host))
			result = X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), options->host);
		else
			result = X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl), options->host, 0);
	} else {
		result = SSL_set1_host(ssl, options->host);
	}
	if (result != 1) {
		why = "setting the name to verify";
		goto out;
	}

	/* The handshake. */
	for (;;) {
		result = SSL_connect(ssl);
		if (result == 1)
			break;
		if (!wait_again(ssl, result, socket_value, options->timeout, &kind, &waits))
			break;
	}
	verify = SSL_get_verify_result(ssl);
	printf("tls-client: handshake %s, verify result %ld (%s), %lu waits\n", result == 1 ? "done" : "failed",
	    verify, X509_verify_cert_error_string(verify), waits);
	if (options->expect_failure) {
		code = ERR_peek_error();
		print_errors();
		if (result == 1) {
			why = "the handshake succeeded";
		} else if (options->expect_verify >= 0 && verify != options->expect_verify) {
			why = "another verify result";
		} else if (options->expect_reason >= 0 && (long)ERR_GET_REASON(code) != options->expect_reason) {
			why = "another reason";
		}
		goto out;
	}
	if (result != 1) {
		printf("tls-client: SSL_get_error %d\n", kind);
		print_errors();
		why = "handshake";
		goto out;
	}
	if (verify != X509_V_OK && options->verify) {
		why = "verified without X509_V_OK";
		goto out;
	}

	/* HTTP: one GET, the answer to its end. */
	if (options->http) {
		sprintf(request, "GET / HTTP/1.1\r\nHost: %.200s\r\nUser-Agent: tls-client\r\nConnection: close\r\n\r\n", options->host);
		if (write_all(ssl, socket_value, options, request, (int)strlen(request), &waits) != 0) {
			why = "write";
			goto out;
		}
		count = read_some(ssl, socket_value, options, answer, (long)sizeof(answer) - 1L, (long)sizeof(answer) - 1L,
		    &kind, &waits, &pending_seen, &pending_bad);
		answer[count] = '\0';
		printf("tls-client: http %ld bytes, end %d\n", count, kind);
		print_errors();
		if (count < 8 || strncmp(answer, "HTTP/1.", 7) != 0)
			why = "no HTTP answer";
		else
			printf("tls-client: %.*s\n", (int)strcspn(answer, "\r\n"), answer);
		goto out;
	}

	/* The reversing server: one line out, its reversal back. */
	if (options->line_size > 0) {
		make_line(line, reversed, options->line_size);
		if (write_all(ssl, socket_value, options, line, (int)options->line_size + 1, &waits) != 0) {
			why = "write";
			goto out;
		}
		count = read_some(ssl, socket_value, options, answer, (long)sizeof(answer), options->line_size + 1L,
		    &kind, &waits, &pending_seen, &pending_bad);
		if (count != options->line_size + 1L || memcmp(answer, reversed, (size_t)count) != 0) {
			printf("tls-client: read %ld of %ld bytes, end %d\n", count, options->line_size + 1L, kind);
			print_errors();
			why = "the reversed line";
			goto out;
		}
		printf("tls-client: line of %ld bytes came back reversed, %lu reads left data pending\n",
		    options->line_size, pending_seen);
		if (pending_bad) {
			why = "SSL_pending out of range";
			goto out;
		}
		if (options->expect_pending && pending_seen == 0UL) {
			why = "SSL_pending never reported the rest of a record";
			goto out;
		}
	}

	/* The end of the connection. */
	if (strcmp(options->close_mode, "client") == 0) {
		result = SSL_shutdown(ssl);
		printf("tls-client: SSL_shutdown %d\n", result);
		if (result < 0)
			why = "SSL_shutdown";
		goto out;
	}
	if (strcmp(options->close_mode, "notify") == 0) {
		if (write_all(ssl, socket_value, options, "CLOSE\n", 6, &waits) != 0) {
			why = "write CLOSE";
			goto out;
		}
	} else {
		printf("tls-client: waiting for eof\n");
		fflush(stdout);
	}
	count = read_some(ssl, socket_value, options, answer, (long)sizeof(answer), (long)sizeof(answer),
	    &kind, &waits, &pending_seen, &pending_bad);
	code = ERR_peek_error();
	printf("tls-client: end of reading after %ld bytes: SSL_get_error %d, reason %d\n", count,
	    kind, ERR_GET_REASON(code));
	if (count != 0) {
		why = "data after the line";
	} else if (strcmp(options->close_mode, "eof") == 0) {
		if (kind != SSL_ERROR_SSL || ERR_GET_REASON(code) != SSL_R_UNEXPECTED_EOF_WHILE_READING)
			why = "an EOF without close_notify was not UNEXPECTED_EOF_WHILE_READING";
	} else if (kind != SSL_ERROR_ZERO_RETURN) {
		why = "the close was not ZERO_RETURN";
	}
	print_errors();
	if (why == NULL && strcmp(options->close_mode, "notify") == 0) {
		result = SSL_shutdown(ssl);
		printf("tls-client: SSL_shutdown %d\n", result);
		if (result != 1)
			why = "SSL_shutdown after the server's close_notify";
	}

out:
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	if (socket_value != CLIENT_NO_SOCKET)
		close_socket(socket_value);
	return why;
}

int
main(
	int argc,
	char **argv)
{
	struct options options;
	const char *why;
#ifdef _WIN32
	WSADATA data;
#endif

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (parse_options(argc, argv, &options) != 0)
		return 2;
#ifdef _WIN32
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return finish("WSAStartup");
#endif
	why = run(&options);
#ifdef _WIN32
	WSACleanup();
#endif
	return finish(why);
}
