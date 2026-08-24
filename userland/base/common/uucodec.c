/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/uucodec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UU_LINE_MAX 512U

static int
write_all(int descriptor, const void *data, size_t length)
{
	const unsigned char *bytes = data;

	while (length != 0) {
		ssize_t written = write(descriptor, bytes, length);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		bytes += (size_t)written;
		length -= (size_t)written;
	}
	return 0;
}

static int
read_chunk(int descriptor, unsigned char *buffer, size_t capacity,
	   size_t *length)
{
	size_t used = 0;

	while (used < capacity) {
		ssize_t count =
		    read(descriptor, buffer + used, capacity - used);

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return -1;
		if (count == 0)
			break;
		used += (size_t)count;
	}
	*length = used;
	return 0;
}

static char
historical_character(unsigned value)
{
	value &= 0x3fU;
	return value == 0 ? '`' : (char)(value + 0x20U);
}

static int
encode_historical(int descriptor)
{
	unsigned char input[45];
	char output[63];
	size_t length = 0;
	int status;

	while ((status = read_chunk(descriptor, input, sizeof(input),
				    &length)) == 0 &&
	       length != 0) {
		size_t source;
		size_t target = 0;

		output[target++] = historical_character((unsigned)length);
		for (source = 0; source < length; source += 3U) {
			unsigned a = input[source];
			unsigned b =
			    source + 1U < length ? input[source + 1U] : 0;
			unsigned c =
			    source + 2U < length ? input[source + 2U] : 0;

			output[target++] = historical_character(a >> 2);
			output[target++] =
			    historical_character((a << 4) | (b >> 4));
			output[target++] =
			    historical_character((b << 2) | (c >> 6));
			output[target++] = historical_character(c);
		}
		output[target++] = '\n';
		if (write_all(STDOUT_FILENO, output, target) != 0)
			return -1;
	}
	if (status != 0)
		return -1;
	return write_all(STDOUT_FILENO, "`\nend\n", 6);
}

static int
encode_base64(int descriptor)
{
	static const char alphabet[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned char input[57];
	char output[77];
	size_t length = 0;
	int status;

	while ((status = read_chunk(descriptor, input, sizeof(input),
				    &length)) == 0 &&
	       length != 0) {
		size_t source;
		size_t target = 0;

		for (source = 0; source < length; source += 3U) {
			unsigned a = input[source];
			unsigned b =
			    source + 1U < length ? input[source + 1U] : 0;
			unsigned c =
			    source + 2U < length ? input[source + 2U] : 0;

			output[target++] = alphabet[a >> 2];
			output[target++] = alphabet[((a & 3U) << 4) | (b >> 4)];
			output[target++] =
			    source + 1U < length
				? alphabet[((b & 15U) << 2) | (c >> 6)]
				: '=';
			output[target++] =
			    source + 2U < length ? alphabet[c & 63U] : '=';
		}
		output[target++] = '\n';
		if (write_all(STDOUT_FILENO, output, target) != 0)
			return -1;
	}
	if (status != 0)
		return -1;
	return write_all(STDOUT_FILENO, "====\n", 5);
}

int
uu_encode_fd(int input_fd, int base64, unsigned mode, const char *decode_path)
{
	char header[UU_LINE_MAX];
	int length;

	if (decode_path == NULL || decode_path[0] == '\0' ||
	    strchr(decode_path, '\n') != NULL ||
	    strchr(decode_path, '\r') != NULL) {
		errno = EINVAL;
		return -1;
	}
	length = snprintf(header, sizeof(header), "%s %03o %s\n",
			  base64 ? "begin-base64" : "begin", mode & 0777U,
			  decode_path);
	if (length < 0 || (size_t)length >= sizeof(header)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (write_all(STDOUT_FILENO, header, (size_t)length) != 0)
		return -1;
	return base64 ? encode_base64(input_fd) : encode_historical(input_fd);
}

static int
read_line(int descriptor, char *line, size_t capacity)
{
	size_t used = 0;
	int too_long = 0;

	for (;;) {
		char byte;
		ssize_t count = read(descriptor, &byte, 1);

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return -1;
		if (count == 0) {
			if (used == 0 && !too_long)
				return 0;
			break;
		}
		if (byte == '\n')
			break;
		if (used + 1U < capacity)
			line[used++] = byte;
		else
			too_long = 1;
	}
	if (too_long) {
		errno = EOVERFLOW;
		return -1;
	}
	if (used != 0 && line[used - 1U] == '\r')
		used--;
	line[used] = '\0';
	return 1;
}

static int
mode_parse(const char *text, unsigned *mode)
{
	unsigned value = 0;
	size_t length = 0;

	while (*text >= '0' && *text <= '7') {
		if (++length > 6U)
			return 0;
		value = value * 8U + (unsigned)(*text++ - '0');
	}
	if (*text != '\0' || length == 0)
		return 0;
	*mode = value & 0777U;
	return 1;
}

static int
header_parse(char *line, int *base64, unsigned *mode, const char **path)
{
	char *mode_text;
	char *separator;

	if (strncmp(line, "begin ", 6) == 0) {
		*base64 = 0;
		mode_text = line + 6;
	} else if (strncmp(line, "begin-base64 ", 13) == 0) {
		*base64 = 1;
		mode_text = line + 13;
	} else
		return 0;
	separator = strchr(mode_text, ' ');
	if (separator == NULL || separator[1] == '\0')
		return -1;
	*separator = '\0';
	if (!mode_parse(mode_text, mode))
		return -1;
	*path = separator + 1;
	return 1;
}

struct output_file {
	int descriptor;
	int directory;
	int standard_output;
	char final_name[256];
	char temporary_name[64];
};

static int
component_valid(const char *component)
{
	return component[0] != '\0' && strcmp(component, ".") != 0 &&
	       strcmp(component, "..") != 0;
}

static int
output_open(struct output_file *output, const char *path)
{
	char *copy;
	char *component;
	char *slash;
	int directory = AT_FDCWD;
	unsigned attempt;

	memset(output, 0, sizeof(*output));
	output->descriptor = -1;
	output->directory = -1;
	if (strcmp(path, "-") == 0) {
		output->descriptor = STDOUT_FILENO;
		output->standard_output = 1;
		return 0;
	}
	if (path[0] == '/' || (copy = strdup(path)) == NULL) {
		errno = path[0] == '/' ? EPERM : ENOMEM;
		return -1;
	}
	component = copy;
	for (;;) {
		slash = strchr(component, '/');
		if (slash == NULL)
			break;
		*slash = '\0';
		if (!component_valid(component)) {
			errno = EPERM;
			goto failed;
		}
		{
			int next = openat(directory, component,
					  O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
					      O_CLOEXEC);

			if (next < 0)
				goto failed;
			if (directory != AT_FDCWD)
				(void)close(directory);
			directory = next;
		}
		component = slash + 1;
	}
	if (!component_valid(component) ||
	    strlen(component) >= sizeof(output->final_name)) {
		errno = !component_valid(component) ? EPERM : ENAMETOOLONG;
		goto failed;
	}
	(void)strcpy(output->final_name, component);
	for (attempt = 0; attempt < 100U; attempt++) {
		(void)snprintf(output->temporary_name,
			       sizeof(output->temporary_name),
			       ".uudecode.%ld.%u", (long)getpid(), attempt);
		output->descriptor = openat(
		    directory, output->temporary_name,
		    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (output->descriptor >= 0 || errno != EEXIST)
			break;
	}
	if (output->descriptor < 0)
		goto failed;
	output->directory = directory;
	free(copy);
	return 0;

failed:
	if (directory != AT_FDCWD)
		(void)close(directory);
	free(copy);
	return -1;
}

static void
output_abort(struct output_file *output)
{
	if (!output->standard_output && output->descriptor >= 0) {
		(void)close(output->descriptor);
		(void)unlinkat(output->directory, output->temporary_name, 0);
	}
	if (output->directory >= 0)
		(void)close(output->directory);
}

static int
output_finish(struct output_file *output, unsigned mode)
{
	if (output->standard_output)
		return 0;
	if (fchmod(output->descriptor, (mode_t)(mode & 0777U)) != 0 ||
	    close(output->descriptor) != 0) {
		output->descriptor = -1;
		output_abort(output);
		return -1;
	}
	output->descriptor = -1;
	if (renameat(output->directory, output->temporary_name,
		     output->directory, output->final_name) != 0) {
		output_abort(output);
		return -1;
	}
	(void)close(output->directory);
	output->directory = -1;
	return 0;
}

static int
historical_value(char character, unsigned *value)
{
	unsigned byte = (unsigned char)character;

	if (character == '`' || character == ' ')
		*value = 0;
	else if (byte >= 0x21U && byte <= 0x5fU)
		*value = byte - 0x20U;
	else
		return 0;
	return 1;
}

static int
decode_historical(int input, int output)
{
	char line[UU_LINE_MAX];
	int status;

	while ((status = read_line(input, line, sizeof(line))) > 0) {
		unsigned length;
		size_t encoded;
		size_t index;
		unsigned char decoded[45];
		size_t used = 0;

		if (!historical_value(line[0], &length) ||
		    length > sizeof(decoded))
			return -1;
		if (length == 0) {
			if (read_line(input, line, sizeof(line)) <= 0 ||
			    strcmp(line, "end") != 0)
				return -1;
			return 0;
		}
		encoded = ((size_t)length + 2U) / 3U * 4U;
		if (strlen(line + 1) < encoded)
			return -1;
		for (index = 0; index < encoded; index += 4U) {
			unsigned a, b, c, d;

			if (!historical_value(line[index + 1U], &a) ||
			    !historical_value(line[index + 2U], &b) ||
			    !historical_value(line[index + 3U], &c) ||
			    !historical_value(line[index + 4U], &d))
				return -1;
			if (used < length)
				decoded[used++] =
				    (unsigned char)((a << 2) | (b >> 4));
			if (used < length)
				decoded[used++] =
				    (unsigned char)((b << 4) | (c >> 2));
			if (used < length)
				decoded[used++] = (unsigned char)((c << 6) | d);
		}
		if (write_all(output, decoded, used) != 0)
			return -1;
	}
	return -1;
}

static int
base64_value(char character)
{
	if (character >= 'A' && character <= 'Z')
		return character - 'A';
	if (character >= 'a' && character <= 'z')
		return character - 'a' + 26;
	if (character >= '0' && character <= '9')
		return character - '0' + 52;
	if (character == '+')
		return 62;
	if (character == '/')
		return 63;
	return -1;
}

static int
decode_base64(int input, int output)
{
	char line[UU_LINE_MAX];
	int status;

	while ((status = read_line(input, line, sizeof(line))) > 0) {
		size_t length;
		size_t index;
		int saw_padding = 0;

		if (strcmp(line, "====") == 0)
			return 0;
		length = strlen(line);
		if (length == 0 || length > 76U || length % 4U != 0)
			return -1;
		for (index = 0; index < length; index += 4U) {
			int a = base64_value(line[index]);
			int b = base64_value(line[index + 1U]);
			int c = line[index + 2U] == '='
				    ? -2
				    : base64_value(line[index + 2U]);
			int d = line[index + 3U] == '='
				    ? -2
				    : base64_value(line[index + 3U]);
			unsigned char decoded[3];
			size_t count = 1;

			if (saw_padding || a < 0 || b < 0 || c == -1 ||
			    d == -1 || (c == -2 && d != -2) ||
			    (c == -2 && (b & 15) != 0) ||
			    (d == -2 && c >= 0 && (c & 3) != 0))
				return -1;
			decoded[0] = (unsigned char)((a << 2) | (b >> 4));
			if (c >= 0) {
				decoded[count++] =
				    (unsigned char)((b << 4) | (c >> 2));
				if (d >= 0)
					decoded[count++] =
					    (unsigned char)((c << 6) | d);
			}
			if (c == -2 || d == -2) {
				saw_padding = 1;
				if (index + 4U != length)
					return -1;
			}
			if (write_all(output, decoded, count) != 0)
				return -1;
		}
	}
	return -1;
}

int
uu_decode_fd(int input_fd, const char *output_override)
{
	char line[UU_LINE_MAX];
	int base64 = 0;
	unsigned mode = 0;
	const char *header_path = NULL;
	char *path_copy = NULL;
	struct output_file output;
	int status;

	while ((status = read_line(input_fd, line, sizeof(line))) > 0) {
		status = header_parse(line, &base64, &mode, &header_path);
		if (status < 0) {
			errno = EINVAL;
			return -1;
		}
		if (status > 0)
			break;
	}
	if (status <= 0) {
		errno = EINVAL;
		return -1;
	}
	path_copy =
	    strdup(output_override != NULL ? output_override : header_path);
	if (path_copy == NULL)
		return -1;
	if (output_open(&output, path_copy) != 0) {
		free(path_copy);
		return -1;
	}
	free(path_copy);
	status = base64 ? decode_base64(input_fd, output.descriptor)
			: decode_historical(input_fd, output.descriptor);
	if (status != 0) {
		output_abort(&output);
		errno = EINVAL;
		return -1;
	}
	return output_finish(&output, mode);
}
