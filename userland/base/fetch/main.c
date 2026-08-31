/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Simple HTTP/1.1 file retrieval utility.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 16384U
#define MAX_REDIRECTS 5

struct url {
	char host[256];
	char path[1024];
	unsigned port;
};

struct reader {
	int fd;
	unsigned char data[BUFFER_SIZE];
	size_t position, length;
};

static int quiet, verbose;

static int usage(void);
static const char *default_output(const char *url);
static int fetch_once(const char *url_text, int output, char redirect[1536]);
static int parse_url(const char *text, struct url *url);
static int connect_server(const struct url *url);
static int send_all(int fd, const void *buffer, size_t length);
static int reader_line(struct reader *reader, char *line, size_t capacity);
static int reader_byte(struct reader *reader);
static ssize_t reader_fill(struct reader *reader);
static int equal_case(const char *left, const char *right);
static int ascii_lower(int c);
static int make_redirect(const struct url *base, const char *location, char *result, size_t capacity);
static int copy_chunked(struct reader *reader, int output);
static int copy_count(struct reader *reader, int output, unsigned long long remaining);
static int write_all(int fd, const void *buffer, size_t length);
static int copy_to_eof(struct reader *reader, int output);

/*
 * Runs the fetch command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int index_for;
	const char *output_name, *url;
	char current[1536], redirect[1536];
	int output, result, redirects;

	output_name = NULL;
	url = NULL;
	output = -1;
	redirects = 0;

	/* Process each remaining command-line operand. */
	for (index_for = 1; index_for < argc; index_for++) {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[index_for], "-q") == 0)
			quiet = 1;
		else if (strcmp(argv[index_for], "-v") == 0)
			verbose = 1;
		else if (strcmp(argv[index_for], "-o") == 0) {
			/* Validates the command-line arguments. */
			if (++index_for >= argc) {
				/* Obtains the usage result. */
				function_result = usage();

				/* Returns the computed result. */
				return function_result;
			}
			output_name = argv[index_for];
		} else if (argv[index_for][0] == '-') {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		} else if (url != NULL) {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		} else {
			url = argv[index_for];
		}
	}

	/* Handles a failed strlen operation. */
	if (url == NULL || strlen(url) >= sizeof(current)) {
		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the output name availability. */
	if (output_name == NULL)
		output_name = default_output(url);

	/* Selects the matching value. */
	if (strcmp(output_name, "-") == 0) {
		output = STDOUT_FILENO;
	} else {
		output = open(output_name, O_WRONLY | O_CREAT | O_TRUNC, 0666);

		/* Handles the output condition. */
		if (output < 0) {
			fprintf(stderr, "fetch: cannot create %s\n",
				output_name);

			/* Reports operation failure. */
			return 1;
		}
	}
	strcpy(current, url);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the quiet condition. */
		if (!quiet)
			fprintf(stderr, "fetch: %s\n", current);
		result = fetch_once(current, output, redirect);

		/* Checks the operation result. */
		if (result != 1)
			break;

		/* Handles the redirects condition. */
		if (++redirects > MAX_REDIRECTS) {
			fprintf(stderr, "fetch: too many redirects\n");
			result = -1;
			break;
		}
		strcpy(current, redirect);
	}

	/* Handles a failed close operation. */
	if (output != STDOUT_FILENO && close(output) != 0)
		result = -1;

	/* Handles the output name availability. */
	if (result != 0 && output_name != NULL && strcmp(output_name, "-") != 0)
		(void)unlink(output_name);

	/* Returns the computed result. */
	return result == 0 ? 0 : 1;
}

/* Supports the usage operation. */
static int
usage(
	void)
{
	fprintf(stderr, "usage: fetch [-qv] [-o file|-] URL\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the default output operation. */
static const char *
default_output(
	const char *url)
{
	const char *slash;

	slash = strrchr(url, '/');

	/* Returns the computed result. */
	return slash != NULL && slash[1] != '\0' ? slash + 1 : "index.html";
}

/* Supports the fetch once operation. */
static int
fetch_once(
	const char *url_text,
	int output,
	char redirect[1536])
{
	struct url url;
	struct reader reader;
	char request[1536], line[2048], *value, *end;
	unsigned long long content_length;
	int descriptor, status, have_length, chunked;

	content_length = 0;
	have_length = 0;
	chunked = 0;

	redirect[0] = '\0';

	/* Handles a failed parse url operation. */
	if (parse_url(url_text, &url) != 0)
		return -1;
	descriptor = connect_server(&url);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;
	status = url.port == 80
		     ? snprintf(request, sizeof(request),
				"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
				"zedBSD-fetch/0.1\r\n"
				"Accept: */*\r\nConnection: close\r\n\r\n",
				url.path, url.host)
		     : snprintf(request, sizeof(request),
				"GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: "
				"zedBSD-fetch/0.1\r\n"
				"Accept: */*\r\nConnection: close\r\n\r\n",
				url.path, url.host, url.port);

	/* Handles a failed send all operation. */
	if (status < 0 || status >= (int)sizeof(request) ||
	    send_all(descriptor, request, (size_t)status) != 0) {
		close(descriptor);

		/* Reports operation failure. */
		return -1;
	}
	memset(&reader, 0, sizeof(reader));
	reader.fd = descriptor;

	/* Handles a failed reader line operation. */
	if (reader_line(&reader, line, sizeof(line)) != 0 ||
	    strncmp(line, "HTTP/", 5) != 0 ||
	    (value = strchr(line, ' ')) == NULL) {
		close(descriptor);

		/* Reports operation failure. */
		return -1;
	}
	status = (int)strtoul(value + 1, &end, 10);

	/* Handles the verbose condition. */
	if (verbose)
		fprintf(stderr, "%s\n", line);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed reader line operation. */
		if (reader_line(&reader, line, sizeof(line)) != 0) {
			close(descriptor);

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the line condition. */
		if (line[0] == '\0')
			break;

		/* Handles the verbose condition. */
		if (verbose)
			fprintf(stderr, "%s\n", line);
		value = strchr(line, ':');

		/* Handles the value availability. */
		if (value == NULL)
			continue;

		/* Continue while the operation condition remains true. */
		*value++ = '\0';
		while (*value == ' ' || *value == '\t')
			value++;

		/* Handles the equal case condition. */
		if (equal_case(line, "Content-Length")) {
			content_length = strtoull(value, &end, 10);

			/* Checks the current endpoint. */
			if (*end != '\0') {
				close(descriptor);

				/* Reports operation failure. */
				return -1;
			}
			have_length = 1;
		} else if (equal_case(line, "Transfer-Encoding") &&
			   equal_case(value, "chunked"))
			chunked = 1;
		else if (equal_case(line, "Location")) {
			/* Handles a failed make redirect operation. */
			if (make_redirect(&url, value, redirect, 1536) != 0) {
				close(descriptor);

				/* Reports operation failure. */
				return -1;
			}
		}
	}

	/* Checks the operation status. */
	if (status == 301 || status == 302 || status == 303 || status == 307 ||
	    status == 308) {
		close(descriptor);

		/* Returns the computed result. */
		return redirect[0] != '\0' ? 1 : -1;
	}

	/* Checks the operation status. */
	if (status < 200 || status >= 300) {
		fprintf(stderr, "fetch: HTTP status %d\n", status);
		close(descriptor);

		/* Reports operation failure. */
		return -1;
	}
	status = chunked       ? copy_chunked(&reader, output)
		 : have_length ? copy_count(&reader, output, content_length)
			       : copy_to_eof(&reader, output);
	close(descriptor);

	/* Returns the computed result. */
	return status;
}

/* Supports the parse url operation. */
static int
parse_url(
	const char *text,
	struct url *url)
{
	const char *cursor_for;
	const char *host, *path, *colon;
	size_t length;
	char port_text[8], *end;
	unsigned long port;

	/* Selects the matching prefix. */
	if (strncmp(text, "https://", 8) == 0) {
		fprintf(stderr, "fetch: https is not supported\n");

		/* Reports operation failure. */
		return -1;
	}

	/* Selects the matching prefix. */
	if (strncmp(text, "http://", 7) != 0) {
		fprintf(stderr, "fetch: unsupported or missing URL scheme\n");

		/* Reports operation failure. */
		return -1;
	}
	host = text + 7;
	path = strchr(host, '/');

	/* Handles the path availability. */
	if (path == NULL)

	/* Process each element required by the operation. */
		path = text + strlen(text);
	colon = NULL;
	for (cursor_for = host; cursor_for < path; cursor_for++) {
		/* Handles the cursor for condition. */
		if (*cursor_for == ':')
			colon = cursor_for;
	}
	length = (size_t)((colon != NULL ? colon : path) - host);

	/* Checks the current data length. */
	if (length == 0 || length >= sizeof(url->host))
		return -1;
	memcpy(url->host, host, length);
	url->host[length] = '\0';
	url->port = 80;

	/* Handles the colon availability. */
	if (colon != NULL) {
		length = (size_t)(path - colon - 1);

		/* Checks the current data length. */
		if (length == 0 || length >= sizeof(port_text))
			return -1;
		memcpy(port_text, colon + 1, length);
		port_text[length] = '\0';
		port = strtoul(port_text, &end, 10);

		/* Checks the current endpoint. */
		if (*end != '\0' || port == 0 || port > 65535U)
			return -1;
		url->port = (unsigned)port;
	}

	/* Handles the path condition. */
	if (*path == '\0')
		path = "/";

	/* Handles a failed strlen operation. */
	if (strlen(path) >= sizeof(url->path))
		return -1;
	strcpy(url->path, path);

	/* Reports successful completion. */
	return 0;
}

/* Supports the connect server operation. */
static int
connect_server(
	const struct url *url)
{
	struct addrinfo hints, *addresses, *current;
	char service[8];
	int descriptor, error;

	descriptor = -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%u", url->port);
	error = getaddrinfo(url->host, service, &hints, &addresses);

	/* Handles an operation failure. */
	if (error != 0) {
		fprintf(stderr, "fetch: %s: %s\n", url->host,
			gai_strerror(error));

		/* Reports operation failure. */
		return -1;
	}

	/* Process each element required by the operation. */
	for (current = addresses; current != NULL; current = current->ai_next) {
		descriptor =
		    socket(current->ai_family, SOCK_STREAM, IPPROTO_TCP);

		/* Handles a failed connect operation. */
		if (descriptor >= 0 && connect(descriptor, current->ai_addr,
					       current->ai_addrlen) == 0)
			break;

		/* Checks the file descriptor. */
		if (descriptor >= 0)
			close(descriptor);
		descriptor = -1;
	}
	freeaddrinfo(addresses);

	/* Checks the file descriptor. */
	if (descriptor < 0) {
		fprintf(stderr, "fetch: cannot connect to %s:%u\n", url->host,
			url->port);
	}

	/* Returns the computed result. */
	return descriptor;
}

/* Supports the send all operation. */
static int
send_all(
	int fd,
	const void *buffer,
	size_t length)
{
	ssize_t count;
	const unsigned char *cursor;

	/* Process each remaining element. */
	cursor = buffer;
	while (length != 0) {
		count = send(fd, cursor, length, 0);

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		cursor += count;
		length -= (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the reader line operation. */
static int
reader_line(
	struct reader *reader,
	char *line,
	size_t capacity)
{
	size_t used;
	int byte;

	/* Continue while the operation condition remains true. */
	used = 0;
	while ((byte = reader_byte(reader)) >= 0) {
		/* Classifies the current byte. */
		if (byte == '\n') {
			/* Checks the current capacity usage. */
			if (used != 0 && line[used - 1] == '\r')
				used--;
			line[used] = '\0';

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the current capacity usage. */
		if (used + 1 >= capacity)
			return -1;
		line[used++] = (char)byte;
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the reader byte operation. */
static int
reader_byte(
	struct reader *reader)
{
	/* Handles a failed reader fill operation. */
	if (reader->position == reader->length && reader_fill(reader) <= 0)
		return -1;

	/* Returns the computed result. */
	return reader->data[reader->position++];
}

/* Supports the reader fill operation. */
static ssize_t
reader_fill(
	struct reader *reader)
{
	ssize_t count;

	count = recv(reader->fd, reader->data, sizeof(reader->data), 0);

	/* Checks the remaining item count. */
	if (count > 0) {
		reader->position = 0;
		reader->length = (size_t)count;
	}

	/* Returns the computed result. */
	return count;
}

/* Supports the equal case operation. */
static int
equal_case(
	const char *left,
	const char *right)
{
	/* Continue while the operation condition remains true. */
	while (*left != '\0' && ascii_lower((unsigned char)*left) ==
				    ascii_lower((unsigned char)*right)) {
		left++;
		right++;
	}

	/* Returns the computed result. */
	return *left == *right;
}

/* Supports the ascii lower operation. */
static int
ascii_lower(
	int c)
{
	/* Returns the computed result. */
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/* Supports the make redirect operation. */
static int
make_redirect(
	const struct url *base,
	const char *location,
	char *result,
	size_t capacity)
{
	int function_result;
	int count;

	/* Selects the matching prefix. */
	if (strncmp(location, "http://", 7) == 0 ||
	    strncmp(location, "https://", 8) == 0) {
		/* Computes the function result. */
		function_result = snprintf(result, capacity, "%s", location) <
			       (int)capacity
			   ? 0
			   : -1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the location condition. */
	if (location[0] == '/') {
		count = base->port == 80
			    ? snprintf(result, capacity, "http://%s%s",
				       base->host, location)
			    : snprintf(result, capacity, "http://%s:%u%s",
				       base->host, base->port, location);
	} else {
		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return count >= 0 && count < (int)capacity ? 0 : -1;
}

/* Supports the copy chunked operation. */
static int
copy_chunked(
	struct reader *reader,
	int output)
{
	char line[256], *end;
	unsigned long long amount;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed reader line operation. */
		if (reader_line(reader, line, sizeof(line)) != 0)
			return -1;
		amount = strtoull(line, &end, 16);

		/* Checks the current endpoint. */
		if (end == line || (*end != '\0' && *end != ';'))
			return -1;

		/* Handles the amount condition. */
		if (amount == 0) {
			do {
				/* Handles a failed reader line operation. */
				if (reader_line(reader, line, sizeof(line)) !=
				    0)

					/* Reports operation failure. */
					return -1;
			} while (line[0] != '\0');

			/* Reports successful completion. */
			return 0;
		}

		/* Handles a failed copy count operation. */
		if (copy_count(reader, output, amount) != 0 ||
		    reader_byte(reader) != '\r' || reader_byte(reader) != '\n')

			/* Reports operation failure. */
			return -1;
	}
}

/* Supports the copy count operation. */
static int
copy_count(
	struct reader *reader,
	int output,
	unsigned long long remaining)
{
	size_t available;

	/* Continue while the operation condition remains true. */
	while (remaining != 0) {
		/* Handles a failed reader fill operation. */
		if (reader->position == reader->length &&
		    reader_fill(reader) <= 0)

			/* Reports operation failure. */
			return -1;
		available = reader->length - reader->position;

		/* Handles the available condition. */
		if ((unsigned long long)available > remaining)
			available = (size_t)remaining;

		/* Handles a failed write all operation. */
		if (write_all(output, reader->data + reader->position,
			      available) != 0)

			/* Reports operation failure. */
			return -1;
		reader->position += available;
		remaining -= available;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the write all operation. */
static int
write_all(
	int fd,
	const void *buffer,
	size_t length)
{
	ssize_t count;
	const unsigned char *cursor;

	/* Process each remaining element. */
	cursor = buffer;
	while (length != 0) {
		count = write(fd, cursor, length);

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		cursor += count;
		length -= (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the copy to eof operation. */
static int
copy_to_eof(
	struct reader *reader,
	int output)
{
	ssize_t count;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the reader condition. */
		if (reader->position != reader->length) {
			/* Handles a failed write all operation. */
			if (write_all(output, reader->data + reader->position,
				      reader->length - reader->position) != 0)

				/* Reports operation failure. */
				return -1;
			reader->position = reader->length;
		}
		count = reader_fill(reader);

		/* Checks the remaining item count. */
		if (count == 0)
			return 0;

		/* Checks the remaining item count. */
		if (count < 0)
			return -1;
	}
}
