/* Simple HTTP/1.1 file retrieval utility.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
ascii_lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int
equal_case(const char *left, const char *right)
{
	while (*left != '\0' && ascii_lower((unsigned char)*left) ==
	    ascii_lower((unsigned char)*right)) {
		left++;
		right++;
	}
	return *left == *right;
}

static int
parse_url(const char *text, struct url *url)
{
	const char *host, *path, *colon;
	size_t length;
	char port_text[8], *end;
	unsigned long port;

	if (strncmp(text, "https://", 8) == 0) {
		fprintf(stderr, "fetch: https is not supported\n");
		return -1;
	}
	if (strncmp(text, "http://", 7) != 0) {
		fprintf(stderr, "fetch: unsupported or missing URL scheme\n");
		return -1;
	}
	host = text + 7;
	path = strchr(host, '/');
	if (path == NULL)
		path = text + strlen(text);
	colon = NULL;
	for (const char *cursor = host; cursor < path; cursor++)
		if (*cursor == ':') colon = cursor;
	length = (size_t)((colon != NULL ? colon : path) - host);
	if (length == 0 || length >= sizeof(url->host))
		return -1;
	memcpy(url->host, host, length);
	url->host[length] = '\0';
	url->port = 80;
	if (colon != NULL) {
		length = (size_t)(path - colon - 1);
		if (length == 0 || length >= sizeof(port_text)) return -1;
		memcpy(port_text, colon + 1, length);
		port_text[length] = '\0';
		port = strtoul(port_text, &end, 10);
		if (*end != '\0' || port == 0 || port > 65535U) return -1;
		url->port = (unsigned)port;
	}
	if (*path == '\0') path = "/";
	if (strlen(path) >= sizeof(url->path)) return -1;
	strcpy(url->path, path);
	return 0;
}

static int
write_all(int fd, const void *buffer, size_t length)
{
	const unsigned char *cursor = buffer;
	while (length != 0) {
		ssize_t count = write(fd, cursor, length);
		if (count <= 0) return -1;
		cursor += count;
		length -= (size_t)count;
	}
	return 0;
}

static int
send_all(int fd, const void *buffer, size_t length)
{
	const unsigned char *cursor = buffer;
	while (length != 0) {
		ssize_t count = send(fd, cursor, length, 0);
		if (count <= 0) return -1;
		cursor += count;
		length -= (size_t)count;
	}
	return 0;
}

static ssize_t
reader_fill(struct reader *reader)
{
	ssize_t count = recv(reader->fd, reader->data, sizeof(reader->data), 0);
	if (count > 0) {
		reader->position = 0;
		reader->length = (size_t)count;
	}
	return count;
}

static int
reader_byte(struct reader *reader)
{
	if (reader->position == reader->length && reader_fill(reader) <= 0)
		return -1;
	return reader->data[reader->position++];
}

static int
reader_line(struct reader *reader, char *line, size_t capacity)
{
	size_t used = 0;
	int byte;
	while ((byte = reader_byte(reader)) >= 0) {
		if (byte == '\n') {
			if (used != 0 && line[used - 1] == '\r') used--;
			line[used] = '\0';
			return 0;
		}
		if (used + 1 >= capacity) return -1;
		line[used++] = (char)byte;
	}
	return -1;
}

static int
copy_count(struct reader *reader, int output, unsigned long long remaining)
{
	while (remaining != 0) {
		size_t available;
		if (reader->position == reader->length && reader_fill(reader) <= 0)
			return -1;
		available = reader->length - reader->position;
		if ((unsigned long long)available > remaining)
			available = (size_t)remaining;
		if (write_all(output, reader->data + reader->position, available) != 0)
			return -1;
		reader->position += available;
		remaining -= available;
	}
	return 0;
}

static int
copy_to_eof(struct reader *reader, int output)
{
	for (;;) {
		ssize_t count;
		if (reader->position != reader->length) {
			if (write_all(output, reader->data + reader->position,
			    reader->length - reader->position) != 0) return -1;
			reader->position = reader->length;
		}
		count = reader_fill(reader);
		if (count == 0) return 0;
		if (count < 0) return -1;
	}
}

static int
copy_chunked(struct reader *reader, int output)
{
	char line[256], *end;
	unsigned long long amount;
	for (;;) {
		if (reader_line(reader, line, sizeof(line)) != 0) return -1;
		amount = strtoull(line, &end, 16);
		if (end == line || (*end != '\0' && *end != ';')) return -1;
		if (amount == 0) {
			do {
				if (reader_line(reader, line, sizeof(line)) != 0) return -1;
			} while (line[0] != '\0');
			return 0;
		}
		if (copy_count(reader, output, amount) != 0 ||
		    reader_byte(reader) != '\r' || reader_byte(reader) != '\n')
			return -1;
	}
}

static int
connect_server(const struct url *url)
{
	struct addrinfo hints, *addresses, *current;
	char service[8];
	int descriptor = -1, error;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%u", url->port);
	error = getaddrinfo(url->host, service, &hints, &addresses);
	if (error != 0) {
		fprintf(stderr, "fetch: %s: %s\n", url->host, gai_strerror(error));
		return -1;
	}
	for (current = addresses; current != NULL; current = current->ai_next) {
		descriptor = socket(current->ai_family, SOCK_STREAM, IPPROTO_TCP);
		if (descriptor >= 0 && connect(descriptor, current->ai_addr,
		    current->ai_addrlen) == 0) break;
		if (descriptor >= 0) close(descriptor);
		descriptor = -1;
	}
	freeaddrinfo(addresses);
	if (descriptor < 0)
		fprintf(stderr, "fetch: cannot connect to %s:%u\n",
		    url->host, url->port);
	return descriptor;
}

static int
make_redirect(const struct url *base, const char *location, char *result,
    size_t capacity)
{
	int count;
	if (strncmp(location, "http://", 7) == 0 ||
	    strncmp(location, "https://", 8) == 0)
		return snprintf(result, capacity, "%s", location) < (int)capacity ? 0 : -1;
	if (location[0] == '/')
		count = base->port == 80 ?
		    snprintf(result, capacity, "http://%s%s", base->host, location) :
		    snprintf(result, capacity, "http://%s:%u%s", base->host,
		    base->port, location);
	else
		return -1;
	return count >= 0 && count < (int)capacity ? 0 : -1;
}

static int
fetch_once(const char *url_text, int output, char redirect[1536])
{
	struct url url;
	struct reader reader;
	char request[1536], line[2048], *value, *end;
	unsigned long long content_length = 0;
	int descriptor, status, have_length = 0, chunked = 0;

	redirect[0] = '\0';
	if (parse_url(url_text, &url) != 0) return -1;
	descriptor = connect_server(&url);
	if (descriptor < 0) return -1;
	status = url.port == 80 ? snprintf(request, sizeof(request),
	    "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: zedBSD-fetch/0.1\r\n"
	    "Accept: */*\r\nConnection: close\r\n\r\n", url.path, url.host) :
	    snprintf(request, sizeof(request),
	    "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: zedBSD-fetch/0.1\r\n"
	    "Accept: */*\r\nConnection: close\r\n\r\n", url.path, url.host,
	    url.port);
	if (status < 0 || status >= (int)sizeof(request) ||
	    send_all(descriptor, request, (size_t)status) != 0) {
		close(descriptor);
		return -1;
	}
	memset(&reader, 0, sizeof(reader));
	reader.fd = descriptor;
	if (reader_line(&reader, line, sizeof(line)) != 0 ||
	    strncmp(line, "HTTP/", 5) != 0 || (value = strchr(line, ' ')) == NULL) {
		close(descriptor);
		return -1;
	}
	status = (int)strtoul(value + 1, &end, 10);
	if (verbose) fprintf(stderr, "%s\n", line);
	for (;;) {
		if (reader_line(&reader, line, sizeof(line)) != 0) {
			close(descriptor); return -1;
		}
		if (line[0] == '\0') break;
		if (verbose) fprintf(stderr, "%s\n", line);
		value = strchr(line, ':');
		if (value == NULL) continue;
		*value++ = '\0';
		while (*value == ' ' || *value == '\t') value++;
		if (equal_case(line, "Content-Length")) {
			content_length = strtoull(value, &end, 10);
			if (*end != '\0') { close(descriptor); return -1; }
			have_length = 1;
		} else if (equal_case(line, "Transfer-Encoding") &&
		    equal_case(value, "chunked")) chunked = 1;
		else if (equal_case(line, "Location")) {
			if (make_redirect(&url, value, redirect, 1536) != 0) {
				close(descriptor); return -1;
			}
		}
	}
	if (status == 301 || status == 302 || status == 303 ||
	    status == 307 || status == 308) {
		close(descriptor);
		return redirect[0] != '\0' ? 1 : -1;
	}
	if (status < 200 || status >= 300) {
		fprintf(stderr, "fetch: HTTP status %d\n", status);
		close(descriptor);
		return -1;
	}
	status = chunked ? copy_chunked(&reader, output) :
	    have_length ? copy_count(&reader, output, content_length) :
	    copy_to_eof(&reader, output);
	close(descriptor);
	return status;
}

static const char *
default_output(const char *url)
{
	const char *slash = strrchr(url, '/');
	return slash != NULL && slash[1] != '\0' ? slash + 1 : "index.html";
}

static int
usage(void)
{
	fprintf(stderr, "usage: fetch [-qv] [-o file|-] URL\n");
	return 2;
}

int
main(int argc, char **argv)
{
	const char *output_name = NULL, *url = NULL;
	char current[1536], redirect[1536];
	int output = -1, result, redirects = 0;

	for (int index = 1; index < argc; index++) {
		if (strcmp(argv[index], "-q") == 0) quiet = 1;
		else if (strcmp(argv[index], "-v") == 0) verbose = 1;
		else if (strcmp(argv[index], "-o") == 0) {
			if (++index >= argc) return usage();
			output_name = argv[index];
		} else if (argv[index][0] == '-') return usage();
		else if (url != NULL) return usage();
		else url = argv[index];
	}
	if (url == NULL || strlen(url) >= sizeof(current)) return usage();
	if (output_name == NULL) output_name = default_output(url);
	if (strcmp(output_name, "-") == 0) output = STDOUT_FILENO;
	else {
		output = open(output_name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (output < 0) {
			fprintf(stderr, "fetch: cannot create %s\n", output_name);
			return 1;
		}
	}
	strcpy(current, url);
	for (;;) {
		if (!quiet) fprintf(stderr, "fetch: %s\n", current);
		result = fetch_once(current, output, redirect);
		if (result != 1) break;
		if (++redirects > MAX_REDIRECTS) {
			fprintf(stderr, "fetch: too many redirects\n");
			result = -1; break;
		}
		strcpy(current, redirect);
	}
	if (output != STDOUT_FILENO && close(output) != 0) result = -1;
	if (result != 0 && output_name != NULL && strcmp(output_name, "-") != 0)
		(void)unlink(output_name);
	return result == 0 ? 0 : 1;
}
