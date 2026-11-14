/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fetch program: retrieves one file over HTTP/1.1 (RFC 9112), plain or
 * over TLS (https, RFC 9110 section 4.2.2), and follows redirections.
 *
 * TLS is libssl-compat and libcrypto-compat, zedBSD's own TLS libraries
 * with OpenSSL 3.5's API, which the program links.  A connection is TLS
 * 1.2 or later.  The server's chain is verified against the system's roots
 * (/etc/ssl/cert.pem, from the ca-certificates package) and any CA file
 * given with -C, and the certificate's name against the host of the URL
 * (RFC 9110 section 4.3.4, RFC 6125); a server that does not verify is
 * refused before the request is sent.  The host name is sent with SNI
 * (RFC 6066 section 3) unless it is an address literal, which SNI does not
 * carry.
 *
 * A body read to the end of the connection must end with the server's
 * close_notify over TLS (RFC 8446 section 6.1), as an end without it may
 * be a body cut short; a body with a length or chunks does not wait for
 * the end.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <compat/ssl/ssl.h>

#include "include/kern/os-name.h"

/* The size of the buffer a response is read through. */
#define BUFFER_SIZE		16384U

/* The most redirections followed for one URL. */
#define MAX_REDIRECTS		5

/* The longest URL, with its terminator. */
#define URL_MAX			1536U

/* The well-known port of http (RFC 9110 section 4.2.1). */
#define HTTP_PORT		80U

/* The well-known port of https (RFC 9110 section 4.2.2). */
#define HTTPS_PORT		443U

/* The file of the system's roots, which the ca-certificates package installs. */
#define SYSTEM_ROOTS_FILE	"/etc/ssl/cert.pem"

/* How the end of a response body is known (RFC 9112 section 6.3). */
enum body_framing {
	BODY_TO_EOF,
	BODY_LENGTH,
	BODY_CHUNKED
};

/*
 * One URL taken apart: whether it is https, the host, the port and the
 * path with its query.  It lives as long as the one request made to it.
 */
struct url {
	int secure;
	char host[256];
	char path[1024];
	unsigned port;
};

/*
 * One connection to a server: its socket and, for https, its TLS state.
 * It is opened for one request and closed when the response is read.
 */
struct connection {
	int fd;
	SSL *ssl;
	const char *host;
};

/*
 * The buffer a response is read through, byte by byte for the head and
 * in runs for the body.  It belongs to the connection it reads.
 */
struct reader {
	struct connection *connection;
	unsigned char data[BUFFER_SIZE];
	size_t position;
	size_t length;
};

/*
 * What the head of a response says: the status, how the body ends and
 * its length.  Filled by read_head for one response.
 */
struct response_head {
	int status;
	enum body_framing framing;
	unsigned long long content_length;
};

/* Set by -q: the URLs being fetched are not shown. */
static int quiet;

/* Set by -v: the status line and the header fields are shown. */
static int verbose;

/* The CA file given with -C, trusted besides the system's roots, or NULL; set before the first connection. */
static const char *ca_file;

/*
 * The TLS context every https connection is made from (the roots and the
 * settings), made at the first https URL and kept for the program's life;
 * NULL before then.
 */
static SSL_CTX *tls_context;

static void usage(void);
static int read_options(int argc, char **argv, const char **output_name, const char **url);
static const char *default_output(const char *url);
static int fetch_once(const char *url_text, int output, char redirect[URL_MAX]);
static int send_request(struct connection *connection, const struct url *url);
static int read_head(struct reader *reader, const struct url *url, struct response_head *head, char redirect[URL_MAX]);
static int read_field(char *line, const struct url *url, struct response_head *head, char redirect[URL_MAX]);
static int parse_url(const char *text, struct url *url);
static int parse_port(const char *text, size_t length, unsigned *port);
static int connect_server(const struct url *url);
static int tls_make_context(void);
static int tls_start(struct connection *connection, const struct url *url);
static int is_address_literal(const char *host);
static void tls_report(const char *what, const struct connection *connection, int result);
static void connection_close(struct connection *connection);
static int connection_send(struct connection *connection, const void *buffer, size_t length);
static int connection_receive(struct connection *connection, void *buffer, size_t capacity, size_t *count);
static int reader_line(struct reader *reader, char *line, size_t capacity);
static int reader_byte(struct reader *reader);
static int reader_fill(struct reader *reader);
static int equal_case(const char *left, const char *right);
static int ascii_lower(int c);
static int is_redirection(int status);
static int make_redirect(const struct url *base, const char *location, char *result, size_t capacity);
static int write_origin(const struct url *base, char *result, size_t capacity);
static int copy_chunked(struct reader *reader, int output);
static int copy_count(struct reader *reader, int output, unsigned long long remaining);
static int write_all(int fd, const void *buffer, size_t length);
static int copy_to_eof(struct reader *reader, int output);

/*
 * Runs the fetch command: fetch [-qv] [-C cafile] [-o file|-] URL.
 */
int
main(
	int argc,
	char **argv)
{
	const char *output_name;
	const char *url;
	char current[URL_MAX];
	char redirect[URL_MAX];
	size_t length;
	int to_standard_output;
	int output;
	int result;
	int redirects;
	int closed;
	int error;

	/* Starts with no output name, URL or descriptor, and no redirect followed yet. */
	output_name = NULL;
	url = NULL;
	output = -1;
	redirects = 0;

	/* Reads the options and the one URL. */
	error = read_options(argc, argv, &output_name, &url);
	if (error != 0) {
		usage();
		return 2;
	}

	/* Refuses a URL longer than any this program keeps. */
	length = strlen(url);
	if (length >= sizeof(current)) {
		usage();
		return 2;
	}

	/* Names the output after the last part of the URL when -o is not given. */
	if (output_name == NULL)
		output_name = default_output(url);

	/*
	 * A server that closes first must not end the program with SIGPIPE on
	 * the close_notify written after the body: a write to a closed socket
	 * fails with EPIPE instead.
	 */
	(void)signal(SIGPIPE, SIG_IGN);

	/* Opens the output, - being the standard output. */
	to_standard_output = 0;
	if (output_name[0] == '-' && output_name[1] == '\0')
		to_standard_output = 1;
	if (to_standard_output) {
		output = STDOUT_FILENO;
	} else {
		output = open(output_name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (output < 0) {
			fprintf(stderr, "fetch: cannot create %s: %s\n", output_name, strerror(errno));
			return 1;
		}
	}

	/* Fetches the URL and each one it redirects to, up to MAX_REDIRECTS of them. */
	memcpy(current, url, length + 1U);
	for (;;) {
		/* Shows the URL being fetched. */
		if (!quiet)
			fprintf(stderr, "fetch: %s\n", current);

		/* Fetches it: 0 is the body written, 1 a redirection, -1 a failure. */
		result = fetch_once(current, output, redirect);
		if (result != 1)
			break;

		/* Refuses a chain of redirections longer than MAX_REDIRECTS. */
		redirects++;
		if (redirects > MAX_REDIRECTS) {
			fprintf(stderr, "fetch: too many redirects\n");
			result = -1;
			break;
		}

		/* The next URL is the one the server named. */
		memcpy(current, redirect, sizeof(current));
	}

	/* Closes the output; a failed close loses the file. */
	if (!to_standard_output) {
		closed = close(output);
		if (closed != 0)
			result = -1;
	}

	/* Leaves no partial file behind a failure. */
	if (result != 0 && !to_standard_output)
		(void)unlink(output_name);

	/* Reports a failure. */
	if (result != 0)
		return 1;

	/* Succeeded: the file is written. */
	return 0;
}

/* Shows how the command is used. */
static void
usage(void)
{
	/* The synopsis. */
	fprintf(stderr, "usage: fetch [-qv] [-C cafile] [-o file|-] URL\n");
}

/* Reads the options and the one URL; returns 0, or -1 for a usage error. */
static int
read_options(
	int argc,
	char **argv,
	const char **output_name,
	const char **url)
{
	const char *argument;
	int index;

	/* Takes each argument in turn. */
	for (index = 1; index < argc; index++) {
		argument = argv[index];

		/* The URL: the one argument that is not an option. */
		if (argument[0] != '-') {
			if (*url != NULL)
				return -1;
			*url = argument;
			continue;
		}

		/* An option is one letter after the hyphen. */
		if (argument[1] == '\0' || argument[2] != '\0')
			return -1;

		/* Sets what the option says. */
		switch (argument[1]) {
		case 'q':
			/* The URLs are not shown. */
			quiet = 1;
			break;
		case 'v':
			/* The response heads are shown. */
			verbose = 1;
			break;
		case 'o':
			/* The output file, or - for the standard output. */
			index++;
			if (index >= argc)
				return -1;
			*output_name = argv[index];
			break;
		case 'C':
			/* A CA file trusted besides the system's roots. */
			index++;
			if (index >= argc)
				return -1;
			ca_file = argv[index];
			break;
		default:
			/* An option this program does not know. */
			return -1;
		}
	}

	/* Refuses a missing URL. */
	if (*url == NULL)
		return -1;

	/* Succeeded: the options are set. */
	return 0;
}

/* Finds the default output name: the last part of the URL, or index.html. */
static const char *
default_output(
	const char *url)
{
	const char *slash;

	/* The text after the last slash. */
	slash = strrchr(url, '/');

	/* A URL that ends with a slash (or has none) names no file. */
	if (slash == NULL)
		return "index.html";
	if (slash[1] == '\0')
		return "index.html";

	/* Succeeded: the last part of the URL. */
	return slash + 1;
}

/*
 * Fetches one URL into the output.  Returns 0 when the body is written, 1
 * when the server redirects (the URL is in redirect), or -1 on a failure
 * (already reported).
 */
static int
fetch_once(
	const char *url_text,
	int output,
	char redirect[URL_MAX])
{
	struct url url;
	struct connection connection;
	struct reader reader;
	struct response_head head;
	int redirection;
	int error;
	int copied;

	/* Starts with no redirect target. */
	redirect[0] = '\0';

	/* Takes the URL apart. */
	error = parse_url(url_text, &url);
	if (error != 0)
		return -1;

	/* Connects to the server. */
	memset(&connection, 0, sizeof(connection));
	connection.host = url.host;
	connection.ssl = NULL;
	connection.fd = connect_server(&url);
	if (connection.fd < 0)
		return -1;

	/* Starts TLS on an https connection; nothing is sent to a server that does not verify. */
	if (url.secure) {
		error = tls_start(&connection, &url);
		if (error != 0) {
			connection_close(&connection);
			return -1;
		}
	}

	/* Sends the request. */
	error = send_request(&connection, &url);
	if (error != 0) {
		fprintf(stderr, "fetch: %s: cannot send the request\n", url.host);
		connection_close(&connection);
		return -1;
	}

	/* Reads the status line and the header fields. */
	memset(&reader, 0, sizeof(reader));
	reader.connection = &connection;
	memset(&head, 0, sizeof(head));
	head.framing = BODY_TO_EOF;
	error = read_head(&reader, &url, &head, redirect);
	if (error != 0) {
		connection_close(&connection);
		return -1;
	}

	/* A redirection is followed when the server named where to. */
	redirection = is_redirection(head.status);
	if (redirection) {
		connection_close(&connection);
		if (redirect[0] == '\0') {
			fprintf(stderr, "fetch: HTTP status %d without a location\n", head.status);
			return -1;
		}

		/* Succeeded: a redirect target is waiting in redirect. */
		return 1;
	}

	/* Anything but a success is a failure. */
	if (head.status < 200 || head.status >= 300) {
		fprintf(stderr, "fetch: HTTP status %d\n", head.status);
		connection_close(&connection);
		return -1;
	}

	/* Copies the body, whose end is known from its framing. */
	if (head.framing == BODY_CHUNKED) {
		copied = copy_chunked(&reader, output);
	} else if (head.framing == BODY_LENGTH) {
		copied = copy_count(&reader, output, head.content_length);
	} else {
		copied = copy_to_eof(&reader, output);
	}

	/* Closes the connection, which the body has been taken from. */
	connection_close(&connection);
	if (copied != 0) {
		fprintf(stderr, "fetch: %s: the body could not be read or written in full\n", url.host);
		return -1;
	}

	/* Succeeded: the body is written. */
	return 0;
}

/* Sends a GET request for the URL's path; returns 0 or -1. */
static int
send_request(
	struct connection *connection,
	const struct url *url)
{
	char request[URL_MAX + 256U];
	char authority[sizeof(url->host) + 8U];
	unsigned default_port;
	int length;
	int error;

	/* The Host field names the port only when it is not the scheme's own (RFC 9110 section 7.2). */
	default_port = HTTP_PORT;
	if (url->secure)
		default_port = HTTPS_PORT;
	if (url->port == default_port) {
		length = snprintf(authority, sizeof(authority), "%s", url->host);
	} else {
		length = snprintf(authority, sizeof(authority), "%s:%u", url->host, url->port);
	}

	/* Refuses an authority that did not fit. */
	if (length < 0 || length >= (int)sizeof(authority))
		return -1;

	/* The request: one GET, and the connection closed after the response. */
	length = snprintf(request,
			  sizeof(request),
			  "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " ZEDBSD_OS_NAME "-fetch/0.2\r\n"
			  "Accept: */*\r\nConnection: close\r\n\r\n",
			  url->path,
			  authority);
	if (length < 0 || length >= (int)sizeof(request))
		return -1;

	/* Sends all of it. */
	error = connection_send(connection, request, (size_t)length);
	if (error != 0)
		return -1;

	/* Succeeded: the request is sent. */
	return 0;
}

/*
 * Reads the status line and the header fields of a response into head,
 * and the URL a Location field names into redirect.  Returns 0, or -1 on
 * a failure (already reported).
 */
static int
read_head(
	struct reader *reader,
	const struct url *url,
	struct response_head *head,
	char redirect[URL_MAX])
{
	char line[2048];
	char *space;
	char *end;
	int version;
	int error;

	/* The status line: HTTP/x.y, a space, and the status code. */
	error = reader_line(reader, line, sizeof(line));
	if (error != 0) {
		fprintf(stderr, "fetch: %s: no response\n", url->host);
		return -1;
	}

	/* Finds the status code in the line. */
	space = strchr(line, ' ');
	version = strncmp(line, "HTTP/", 5);
	if (space == NULL || version != 0) {
		fprintf(stderr, "fetch: %s: not an HTTP response\n", url->host);
		return -1;
	}

	/* Reads the status code. */
	head->status = (int)strtoul(space + 1, &end, 10);

	/* Shows the status line with -v. */
	if (verbose)
		fprintf(stderr, "%s\n", line);

	/* Reads the header fields up to the empty line. */
	for (;;) {
		/* One field. */
		error = reader_line(reader, line, sizeof(line));
		if (error != 0) {
			fprintf(stderr, "fetch: %s: the response head is cut short or too long\n", url->host);
			return -1;
		}

		/* The empty line ends the head. */
		if (line[0] == '\0')
			break;

		/* Shows the field with -v. */
		if (verbose)
			fprintf(stderr, "%s\n", line);

		/* Takes what the field says. */
		error = read_field(line, url, head, redirect);
		if (error != 0)
			return -1;
	}

	/* Succeeded: the head is read. */
	return 0;
}

/*
 * Takes one header field: the ones that say how the body ends and where a
 * redirection goes.  Returns 0, or -1 for a field that cannot be used
 * (reported).
 */
static int
read_field(
	char *line,
	const struct url *url,
	struct response_head *head,
	char redirect[URL_MAX])
{
	char *value;
	char *end;
	int is_length;
	int is_encoding;
	int is_location;
	int chunked;
	int error;

	/* A line without a colon is not a field and is passed over. */
	value = strchr(line, ':');
	if (value == NULL)
		return 0;

	/* Splits the name from the value, whose leading white space goes. */
	*value = '\0';
	value++;
	while (*value == ' ' || *value == '\t')
		value++;

	/* Names the field. */
	is_length = equal_case(line, "Content-Length");
	is_encoding = equal_case(line, "Transfer-Encoding");
	is_location = equal_case(line, "Location");

	/* The body is this many bytes, unless it is chunked. */
	if (is_length) {
		head->content_length = strtoull(value, &end, 10);
		if (end == value || *end != '\0') {
			fprintf(stderr, "fetch: %s: a bad Content-Length\n", url->host);
			return -1;
		}

		/* A length is the framing unless chunks were already named. */
		if (head->framing != BODY_CHUNKED)
			head->framing = BODY_LENGTH;
		return 0;
	}

	/* Chunks take the place of a length (RFC 9112 section 6.3). */
	if (is_encoding) {
		chunked = equal_case(value, "chunked");
		if (chunked)
			head->framing = BODY_CHUNKED;
		return 0;
	}

	/* The URL of a redirection, relative to this one. */
	if (is_location) {
		error = make_redirect(url, value, redirect, URL_MAX);
		if (error != 0) {
			fprintf(stderr, "fetch: %s: a Location this program cannot follow: %s\n", url->host, value);
			return -1;
		}

		/* Succeeded: the Location is recorded. */
		return 0;
	}

	/* Succeeded: any other field says nothing this program uses. */
	return 0;
}

/* Takes an http or https URL apart; returns 0, or -1 when it cannot be fetched (reported). */
static int
parse_url(
	const char *text,
	struct url *url)
{
	const char *host;
	const char *path;
	const char *colon;
	const char *cursor;
	const char *at;
	size_t length;
	int https;
	int http;
	int error;

	/* The scheme decides TLS and the default port. */
	https = strncmp(text, "https://", 8);
	http = strncmp(text, "http://", 7);
	if (https == 0) {
		url->secure = 1;
		url->port = HTTPS_PORT;
		host = text + 8;
	} else if (http == 0) {
		url->secure = 0;
		url->port = HTTP_PORT;
		host = text + 7;
	} else {
		fprintf(stderr, "fetch: unsupported or missing URL scheme\n");
		return -1;
	}

	/* The authority ends at the path, the query or the fragment. */
	path = host + strcspn(host, "/?#");

	/* A colon in the authority starts the port. */
	colon = NULL;
	for (cursor = host; cursor < path; cursor++) {
		if (*cursor == ':')
			colon = cursor;
	}

	/* The host. */
	length = (size_t)(path - host);
	if (colon != NULL)
		length = (size_t)(colon - host);
	if (length == 0 || length >= sizeof(url->host)) {
		fprintf(stderr, "fetch: the URL has no host or too long a one\n");
		return -1;
	}

	/* Refuses a user name, which this program does not send. */
	at = memchr(host, '@', (size_t)(path - host));
	if (at != NULL) {
		fprintf(stderr, "fetch: user names in URLs are not supported\n");
		return -1;
	}

	/* Copies the host name out. */
	memcpy(url->host, host, length);
	url->host[length] = '\0';

	/* The port, when the URL gives one. */
	if (colon != NULL) {
		error = parse_port(colon + 1, (size_t)(path - colon - 1), &url->port);
		if (error != 0) {
			fprintf(stderr, "fetch: a bad port in the URL\n");
			return -1;
		}
	}

	/* The path with its query, without the fragment, which is not sent (RFC 9110 section 4.2.5). */
	length = strcspn(path, "#");
	if (length + 2U > sizeof(url->path)) {
		fprintf(stderr, "fetch: the URL's path is too long\n");
		return -1;
	}

	/* Lays the path out with a leading slash. */
	if (length == 0 || path[0] != '/') {
		/* An empty path, or a query straight after the authority, is the root's. */
		url->path[0] = '/';
		memcpy(url->path + 1, path, length);
		url->path[length + 1U] = '\0';
	} else {
		/* The path as written. */
		memcpy(url->path, path, length);
		url->path[length] = '\0';
	}

	/* Succeeded: the URL is taken apart. */
	return 0;
}

/* Reads a decimal port of length characters; returns 0, or -1 when it is not 1 to 65535. */
static int
parse_port(
	const char *text,
	size_t length,
	unsigned *port)
{
	char digits[8];
	char *end;
	unsigned long number;

	/* The digits on their own. */
	if (length == 0 || length >= sizeof(digits))
		return -1;
	memcpy(digits, text, length);
	digits[length] = '\0';

	/* Their value, which starts with a digit and has nothing after it. */
	if (digits[0] < '0' || digits[0] > '9')
		return -1;
	number = strtoul(digits, &end, 10);
	if (*end != '\0')
		return -1;
	if (number == 0 || number > 65535UL)
		return -1;

	/* Succeeded: the port is read. */
	*port = (unsigned)number;
	return 0;
}

/* Connects to the URL's host and port; returns the socket, or -1 (reported). */
static int
connect_server(
	const struct url *url)
{
	struct addrinfo hints;
	struct addrinfo *addresses;
	struct addrinfo *current;
	char service[8];
	int descriptor;
	int error;
	int connected;

	/* Starts with no socket. */
	descriptor = -1;

	/* Looks up the host's addresses. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%u", url->port);
	error = getaddrinfo(url->host, service, &hints, &addresses);
	if (error != 0) {
		fprintf(stderr, "fetch: %s: %s\n", url->host, gai_strerror(error));
		return -1;
	}

	/* Tries each address until one takes the connection. */
	for (current = addresses; current != NULL; current = current->ai_next) {
		/* A socket for this address. */
		descriptor = socket(current->ai_family, SOCK_STREAM, IPPROTO_TCP);
		if (descriptor < 0)
			continue;

		/* The connection, or the next address. */
		connected = connect(descriptor, current->ai_addr, current->ai_addrlen);
		if (connected == 0)
			break;
		close(descriptor);
		descriptor = -1;
	}

	/* Releases the address list. */
	freeaddrinfo(addresses);

	/* No address took the connection. */
	if (descriptor < 0) {
		fprintf(stderr, "fetch: cannot connect to %s:%u\n", url->host, url->port);
		return -1;
	}

	/* Succeeded: the socket is connected. */
	return descriptor;
}

/*
 * Makes the TLS context, once: TLS 1.2 or later, the peer verified against
 * the system's roots and the -C file.  Returns 0, or -1 (reported).
 */
static int
tls_make_context(void)
{
	const SSL_METHOD *method;
	long set;
	int loaded;

	/* Already made. */
	if (tls_context != NULL)
		return 0;

	/* The client context. */
	method = TLS_client_method();
	tls_context = SSL_CTX_new(method);
	if (tls_context == NULL) {
		fprintf(stderr, "fetch: cannot set up TLS\n");
		return -1;
	}

	/* TLS 1.2 or later. */
	set = SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION);
	if (set != 1) {
		fprintf(stderr, "fetch: the TLS library refuses TLS 1.2 as the least version\n");
		return -1;
	}

	/* The server must verify, or the handshake fails. */
	SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, NULL);

	/* The system's roots; a missing file leaves none, which a verification failure then reports. */
	loaded = SSL_CTX_set_default_verify_paths(tls_context);
	if (loaded != 1) {
		fprintf(stderr, "fetch: cannot read the system's root certificates\n");
		return -1;
	}

	/* The CA file of -C. */
	if (ca_file != NULL) {
		loaded = SSL_CTX_load_verify_locations(tls_context, ca_file, NULL);
		if (loaded != 1) {
			fprintf(stderr, "fetch: cannot read the CA file %s\n", ca_file);
			return -1;
		}
	}

	/* Succeeded: https connections can be made. */
	return 0;
}

/*
 * Starts TLS on a connected socket: the handshake, the chain and the
 * host's name.  Returns 0, or -1 (reported).
 */
static int
tls_start(
	struct connection *connection,
	const struct url *url)
{
	long sent;
	int result;
	int literal;

	/* The shared context. */
	result = tls_make_context();
	if (result != 0)
		return -1;

	/* The connection's state, which reads and writes the socket itself. */
	ERR_clear_error();
	connection->ssl = SSL_new(tls_context);
	if (connection->ssl == NULL) {
		fprintf(stderr, "fetch: %s: cannot set up TLS\n", url->host);
		return -1;
	}

	/* Hands the connected socket to the TLS object. */
	result = SSL_set_fd(connection->ssl, connection->fd);
	if (result != 1) {
		tls_report("cannot set up TLS", connection, 0);
		return -1;
	}

	/* The name sent with SNI, which carries no address literal (RFC 6066 section 3). */
	literal = is_address_literal(url->host);
	if (!literal) {
		sent = SSL_set_tlsext_host_name(connection->ssl, url->host);
		if (sent != 1) {
			tls_report("cannot set the server name", connection, 0);
			return -1;
		}
	}

	/* The name or the address the certificate must match. */
	result = SSL_set1_host(connection->ssl, url->host);
	if (result != 1) {
		tls_report("cannot set the name to verify", connection, 0);
		return -1;
	}

	/* The handshake, which fails when the chain or the name does not verify. */
	result = SSL_connect(connection->ssl);
	if (result != 1) {
		tls_report("TLS handshake failed", connection, result);
		return -1;
	}

	/* Succeeded: the connection is secure. */
	return 0;
}

/* Tells whether a host is written as an IPv4 address rather than a name. */
static int
is_address_literal(
	const char *host)
{
	struct in_addr address;
	int converted;

	/* An IPv4 address in dotted form. */
	converted = inet_pton(AF_INET, host, &address);
	if (converted == 1)
		return 1;

	/* A name. */
	return 0;
}

/* Reports a TLS failure: the certificate's verification, or the library's first error. */
static void
tls_report(
	const char *what,
	const struct connection *connection,
	int result)
{
	const char *described;
	unsigned long queued;
	long verified;
	int reason;
	int readable;
	int saved_errno;
	char text[256];

	/* Keeps the errno the failing call left, which the work below may change. */
	saved_errno = errno;

	/* A certificate that did not verify says why. */
	verified = SSL_get_verify_result(connection->ssl);
	if (verified != X509_V_OK) {
		described = X509_verify_cert_error_string(verified);
		fprintf(stderr, "fetch: %s: %s: certificate verify failed: %s\n", connection->host, what, described);
		ERR_clear_error();

		/* Without the system's roots no server verifies: says where they come from. */
		readable = access(SYSTEM_ROOTS_FILE, R_OK);
		if (readable != 0 && ca_file == NULL)
			fprintf(stderr, "fetch: %s is missing: install the ca-certificates package or give -C cafile\n", SYSTEM_ROOTS_FILE);
		return;
	}

	/* Otherwise the first queued error. */
	queued = ERR_get_error();
	if (queued != 0) {
		ERR_error_string_n(queued, text, sizeof(text));
		fprintf(stderr, "fetch: %s: %s: %s\n", connection->host, what, text);
		ERR_clear_error();
		return;
	}

	/* No queued error: the socket failed underneath. */
	reason = SSL_get_error(connection->ssl, result);
	if (reason == SSL_ERROR_SYSCALL && saved_errno != 0) {
		fprintf(stderr, "fetch: %s: %s: %s\n", connection->host, what, strerror(saved_errno));
		return;
	}

	/* Nothing more is known. */
	fprintf(stderr, "fetch: %s: %s (SSL error %d)\n", connection->host, what, reason);
}

/* Ends a connection: close_notify over TLS, then the socket. */
static void
connection_close(
	struct connection *connection)
{
	/* Ends TLS without waiting for the server's close_notify, and frees its state. */
	if (connection->ssl != NULL) {
		(void)SSL_shutdown(connection->ssl);
		SSL_free(connection->ssl);
		connection->ssl = NULL;
	}

	/* Closes the socket. */
	if (connection->fd >= 0) {
		close(connection->fd);
		connection->fd = -1;
	}
}

/* Sends all of a buffer over the connection; returns 0 or -1. */
static int
connection_send(
	struct connection *connection,
	const void *buffer,
	size_t length)
{
	const unsigned char *cursor;
	ssize_t count;
	int chunk;
	int written;
	int reason;

	/* Sends until all of it is gone. */
	cursor = buffer;
	while (length != 0) {
		if (connection->ssl == NULL) {
			/* Plain: the socket itself. */
			count = send(connection->fd, cursor, length, 0);
			if (count <= 0)
				return -1;
		} else {
			/* TLS: one write (the library takes an int); a write that must wait is tried again. */
			chunk = INT_MAX;
			if (length < (size_t)INT_MAX)
				chunk = (int)length;
			written = SSL_write(connection->ssl, cursor, chunk);
			if (written <= 0) {
				reason = SSL_get_error(connection->ssl, written);
				if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE)
					continue;
				tls_report("TLS write failed", connection, written);
				return -1;
			}

			/* Takes the bytes the write took. */
			count = written;
		}

		/* Moves past what was sent. */
		cursor += count;
		length -= (size_t)count;
	}

	/* Succeeded: everything is sent. */
	return 0;
}

/*
 * Receives what the server sent, up to capacity bytes.  Returns 0 with
 * *count bytes (0 at the end of the connection), or -1 on a failure
 * (reported for TLS).  Over TLS the end is the server's close_notify; an
 * end without it is a failure.
 */
static int
connection_receive(
	struct connection *connection,
	void *buffer,
	size_t capacity,
	size_t *count)
{
	ssize_t received;
	int chunk;
	int result;
	int reason;

	/* Starts with nothing received. */
	*count = 0;

	/* Plain: the socket itself. */
	if (connection->ssl == NULL) {
		received = recv(connection->fd, buffer, capacity, 0);
		if (received < 0)
			return -1;
		*count = (size_t)received;
		return 0;
	}

	/* TLS: reads until a record gives bytes, the end, or a failure. */
	chunk = INT_MAX;
	if (capacity < (size_t)INT_MAX)
		chunk = (int)capacity;
	for (;;) {
		/* One read. */
		result = SSL_read(connection->ssl, buffer, chunk);
		if (result > 0) {
			*count = (size_t)result;
			return 0;
		}

		/* A record not complete yet is read again. */
		reason = SSL_get_error(connection->ssl, result);
		if (reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE)
			break;
	}

	/* Anything but the server's close_notify, an end without it among it, is a failure. */
	if (reason != SSL_ERROR_ZERO_RETURN) {
		tls_report("TLS read failed", connection, result);
		return -1;
	}

	/* Succeeded: the server's close_notify is the end. */
	return 0;
}

/* Reads one line, without its CR LF; returns 0, or -1 at the end or for too long a line. */
static int
reader_line(
	struct reader *reader,
	char *line,
	size_t capacity)
{
	size_t used;
	int byte;

	/* Reads bytes up to the line feed. */
	used = 0;
	for (;;) {
		/* The next byte; the end of the connection cuts the line short. */
		byte = reader_byte(reader);
		if (byte < 0)
			return -1;

		/* The line feed ends the line, and a carriage return before it goes. */
		if (byte == '\n') {
			if (used != 0 && line[used - 1] == '\r')
				used--;
			line[used] = '\0';
			return 0;
		}

		/* Refuses a line longer than the buffer. */
		if (used + 1 >= capacity)
			return -1;
		line[used] = (char)byte;
		used++;
	}
}

/* Reads one byte; returns it, or -1 at the end or on a failure. */
static int
reader_byte(
	struct reader *reader)
{
	int filled;
	int byte;

	/* Fills the buffer when it is used up. */
	if (reader->position == reader->length) {
		filled = reader_fill(reader);
		if (filled <= 0)
			return -1;
	}

	/* Succeeded: the next byte. */
	byte = reader->data[reader->position];
	reader->position++;
	return byte;
}

/* Fills the buffer; returns 1 with bytes in it, 0 at the end, or -1 on a failure. */
static int
reader_fill(
	struct reader *reader)
{
	size_t count;
	int error;

	/* What the server sent next. */
	error = connection_receive(reader->connection, reader->data, sizeof(reader->data), &count);
	if (error != 0)
		return -1;

	/* The end of the connection. */
	if (count == 0)
		return 0;

	/* Succeeded: the buffer holds new bytes. */
	reader->position = 0;
	reader->length = count;
	return 1;
}

/* Tells whether two texts are equal regardless of ASCII case. */
static int
equal_case(
	const char *left,
	const char *right)
{
	int left_small;
	int right_small;

	/* Walks the two texts together until they differ or end. */
	for (;;) {
		left_small = ascii_lower((unsigned char)*left);
		right_small = ascii_lower((unsigned char)*right);
		if (left_small != right_small)
			return 0;
		if (*left == '\0')
			return 1;
		left++;
		right++;
	}
}

/* Turns an ASCII capital letter into its small letter. */
static int
ascii_lower(
	int c)
{
	/* A capital letter. */
	if (c >= 'A' && c <= 'Z')
		return c + ('a' - 'A');

	/* Anything else stays. */
	return c;
}

/* Tells whether a status is a redirection this program follows (RFC 9110 section 15.4). */
static int
is_redirection(
	int status)
{
	/* Moved Permanently, Found, See Other, Temporary Redirect and Permanent Redirect. */
	switch (status) {
	case 301:
	case 302:
	case 303:
	case 307:
	case 308:
		return 1;
	default:
		return 0;
	}
}

/*
 * Resolves a Location against the URL it came with (RFC 9110 section
 * 10.2.2, RFC 3986 section 5.2): an absolute http or https URL, a
 * network-path reference (//host/path) in the same scheme, an absolute
 * path on the same origin, or a path relative to the current one's
 * directory.  Returns 0, or -1 for one that cannot be followed.
 */
static int
make_redirect(
	const struct url *base,
	const char *location,
	char *result,
	size_t capacity)
{
	const char *slash;
	const char *scheme;
	const char *colon;
	size_t directory;
	size_t used;
	size_t name_length;
	int https;
	int http;
	int length;
	int error;

	/* An absolute URL in either scheme, a change between http and https among them. */
	https = strncmp(location, "https://", 8);
	http = strncmp(location, "http://", 7);
	if (https == 0 || http == 0) {
		length = snprintf(result, capacity, "%s", location);
		if (length < 0 || length >= (int)capacity)
			return -1;
		return 0;
	}

	/* A network-path reference keeps the scheme. */
	if (location[0] == '/' && location[1] == '/') {
		scheme = "http:";
		if (base->secure)
			scheme = "https:";
		length = snprintf(result, capacity, "%s%s", scheme, location);
		if (length < 0 || length >= (int)capacity)
			return -1;
		return 0;
	}

	/* The origin of the current URL, which the other forms keep. */
	error = write_origin(base, result, capacity);
	if (error != 0)
		return -1;
	used = strlen(result);

	/* An absolute path on the same origin. */
	if (location[0] == '/') {
		length = snprintf(result + used, capacity - used, "%s", location);
		if (length < 0 || length >= (int)(capacity - used))
			return -1;
		return 0;
	}

	/* An empty reference, or one with another scheme (a colon before any slash), is not followed. */
	if (location[0] == '\0')
		return -1;
	name_length = strcspn(location, "/?#");
	colon = memchr(location, ':', name_length);
	if (colon != NULL)
		return -1;

	/* A relative path: the current path up to its last slash, then the reference. */
	slash = strrchr(base->path, '/');
	directory = 1;
	if (slash != NULL)
		directory = (size_t)(slash - base->path) + 1U;
	length = snprintf(result + used, capacity - used, "%.*s%s", (int)directory, base->path, location);
	if (length < 0 || length >= (int)(capacity - used))
		return -1;

	/* Succeeded: the URL to follow is written. */
	return 0;
}

/* Writes scheme://host[:port] of a URL; returns 0 or -1 when it does not fit. */
static int
write_origin(
	const struct url *base,
	char *result,
	size_t capacity)
{
	const char *scheme;
	unsigned default_port;
	int length;

	/* The scheme and its default port. */
	scheme = "http";
	default_port = HTTP_PORT;
	if (base->secure) {
		scheme = "https";
		default_port = HTTPS_PORT;
	}

	/* The port is written only when it is not the default. */
	if (base->port == default_port) {
		length = snprintf(result, capacity, "%s://%s", scheme, base->host);
	} else {
		length = snprintf(result, capacity, "%s://%s:%u", scheme, base->host, base->port);
	}

	/* Refuses an origin that did not fit. */
	if (length < 0 || length >= (int)capacity)
		return -1;

	/* Succeeded: the origin is written. */
	return 0;
}

/* Copies a chunked body (RFC 9112 section 7.1); returns 0 or -1. */
static int
copy_chunked(
	struct reader *reader,
	int output)
{
	char line[256];
	char *end;
	unsigned long long amount;
	int error;
	int byte;

	/* Copies chunks until the last, empty one. */
	for (;;) {
		/* The chunk's size in hex, with any extension after a semicolon. */
		error = reader_line(reader, line, sizeof(line));
		if (error != 0)
			return -1;
		amount = strtoull(line, &end, 16);
		if (end == line)
			return -1;
		if (*end != '\0' && *end != ';' && *end != ' ' && *end != '\t')
			return -1;

		/* The last chunk: the trailer fields up to the empty line end the body. */
		if (amount == 0) {
			do {
				error = reader_line(reader, line, sizeof(line));
				if (error != 0)
					return -1;
			} while (line[0] != '\0');
			return 0;
		}

		/* The chunk's data. */
		error = copy_count(reader, output, amount);
		if (error != 0)
			return -1;

		/* The CR LF after the data. */
		byte = reader_byte(reader);
		if (byte != '\r')
			return -1;
		byte = reader_byte(reader);
		if (byte != '\n')
			return -1;
	}
}

/* Copies exactly remaining bytes of the body; returns 0, or -1 when the connection ends first. */
static int
copy_count(
	struct reader *reader,
	int output,
	unsigned long long remaining)
{
	size_t available;
	int filled;
	int error;

	/* Copies what the buffer holds, filling it as it runs out. */
	while (remaining != 0) {
		/* More bytes when the buffer is used up. */
		if (reader->position == reader->length) {
			filled = reader_fill(reader);
			if (filled <= 0)
				return -1;
		}

		/* Up to the bytes still owed. */
		available = reader->length - reader->position;
		if ((unsigned long long)available > remaining)
			available = (size_t)remaining;

		/* Writes them out. */
		error = write_all(output, reader->data + reader->position, available);
		if (error != 0)
			return -1;
		reader->position += available;
		remaining -= available;
	}

	/* Succeeded: the body is copied. */
	return 0;
}

/* Writes all of a buffer to a file; returns 0 or -1. */
static int
write_all(
	int fd,
	const void *buffer,
	size_t length)
{
	const unsigned char *cursor;
	ssize_t count;

	/* Writes until all of it is out. */
	cursor = buffer;
	while (length != 0) {
		count = write(fd, cursor, length);
		if (count <= 0)
			return -1;
		cursor += count;
		length -= (size_t)count;
	}

	/* Succeeded: everything is written. */
	return 0;
}

/* Copies a body that ends with the connection; returns 0, or -1 on a failure. */
static int
copy_to_eof(
	struct reader *reader,
	int output)
{
	int filled;
	int error;

	/* Copies the buffer and refills it until the end. */
	for (;;) {
		/* What the buffer still holds. */
		if (reader->position != reader->length) {
			error = write_all(output, reader->data + reader->position, reader->length - reader->position);
			if (error != 0)
				return -1;
			reader->position = reader->length;
		}

		/* The next bytes, the end, or a failure. */
		filled = reader_fill(reader);
		if (filled == 0)
			return 0;
		if (filled < 0)
			return -1;
	}
}
