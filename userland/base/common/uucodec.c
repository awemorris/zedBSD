/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland uucodec support.
 */

#include "userland/base/common/uucodec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UU_LINE_MAX 512U

struct output_file {
	int descriptor;
	int directory;
	int standard_output;
	char final_name[256];
	char temporary_name[64];
};

static int write_all(int descriptor, const void *data, size_t length);
static int encode_base64(int descriptor);
static int read_chunk(int descriptor, unsigned char *buffer, size_t capacity, size_t *length);
static int encode_historical(int descriptor);
static char historical_character(unsigned value);
static int read_line(int descriptor, char *line, size_t capacity);
static int header_parse(char *line, int *base64, unsigned *mode, const char **path);
static int mode_parse(const char *text, unsigned *mode);
static int output_open(struct output_file *output, const char *path);
static int component_valid(const char *component);
static int decode_base64(int input, int output);
static int base64_value(char character);
static int decode_historical(int input, int output);
static int historical_value(char character, unsigned *value);
static void output_abort(struct output_file *output);
static int output_finish(struct output_file *output, unsigned mode);

/*
 * Implements the uu encode fd operation.
 */
int
uu_encode_fd(
	int input_fd,
	int base64,
	unsigned mode,
	const char *decode_path)
{
	int function_result;
	char header[UU_LINE_MAX];
	int length;

	/* Handles a failed strchr operation. */
	if (decode_path == NULL || decode_path[0] == '\0' ||
	    strchr(decode_path, '\n') != NULL ||
	    strchr(decode_path, '\r') != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	length = snprintf(header, sizeof(header), "%s %03o %s\n",
			  base64 ? "begin-base64" : "begin", mode & 0777U,
			  decode_path);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(header)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed write all operation. */
	if (write_all(STDOUT_FILENO, header, (size_t)length) != 0)
		return -1;

	/* Computes the function result. */
	function_result = base64 ? encode_base64(input_fd) : encode_historical(input_fd);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the uu decode fd operation.
 */
int
uu_decode_fd(
	int input_fd,
	const char *output_override)
{
	int function_result;
	char line[UU_LINE_MAX];
	int base64;
	unsigned mode;
	const char *header_path;
	char *path_copy;
	struct output_file output;
	int status;

	base64 = 0;
	mode = 0;
	header_path = NULL;
	path_copy = NULL;

	/* Process each remaining element. */
	while ((status = read_line(input_fd, line, sizeof(line))) > 0) {
		status = header_parse(line, &base64, &mode, &header_path);

		/* Checks the operation status. */
		if (status < 0) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the operation status. */
		if (status > 0)
			break;
	}

	/* Checks the operation status. */
	if (status <= 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	path_copy =
	    strdup(output_override != NULL ? output_override : header_path);

	/* Handles the path copy availability. */
	if (path_copy == NULL)
		return -1;

	/* Handles a failed output open operation. */
	if (output_open(&output, path_copy) != 0) {
		free(path_copy);

		/* Reports operation failure. */
		return -1;
	}
	free(path_copy);
	status = base64 ? decode_base64(input_fd, output.descriptor)
			: decode_historical(input_fd, output.descriptor);

	/* Checks the operation status. */
	if (status != 0) {
		output_abort(&output);
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the output finish result. */
	function_result = output_finish(&output, mode);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the write all operation. */
static int
write_all(
	int descriptor,
	const void *data,
	size_t length)
{
	ssize_t written;
	const unsigned char *bytes;

	bytes = data;

	/* Process each remaining element. */
	while (length != 0) {

		written = write(descriptor, bytes, length);

		/* Handles the reported system error. */
		if (written < 0 && errno == EINTR)
			continue;

		/* Handles the written condition. */
		if (written <= 0)
			return -1;
		bytes += (size_t)written;
		length -= (size_t)written;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the encode base64 operation. */
static int
encode_base64(
	int descriptor)
{
	int function_result;
	unsigned a;
	unsigned b;
	unsigned c;
	size_t source;
	size_t target;
	static const char alphabet[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned char input[57];
	char output[77];
	size_t length;
	int status;

	length = 0;

	/* Process each remaining element. */
	while ((status = read_chunk(descriptor, input, sizeof(input),
				    &length)) == 0 &&
	       length != 0) {

		target = 0;

		/* Process each remaining element. */
		for (source = 0; source < length; source += 3U) {
						a = input[source];
						b = source + 1U < length ? input[source + 1U] : 0;
						c = source + 2U < length ? input[source + 2U] : 0;

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

		/* Handles a failed write all operation. */
		if (write_all(STDOUT_FILENO, output, target) != 0)
			return -1;
	}

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	/* Obtains the write all result. */
	function_result = write_all(STDOUT_FILENO, "====\n", 5);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the read chunk operation. */
static int
read_chunk(
	int descriptor,
	unsigned char *buffer,
	size_t capacity,
	size_t *length)
{
	ssize_t count;
	size_t used;

	used = 0;

	/* Continue while the operation condition remains true. */
	while (used < capacity) {

		count = read(descriptor, buffer + used, capacity - used);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0)
			return -1;

		/* Checks the remaining item count. */
		if (count == 0)
			break;
		used += (size_t)count;
	}
	*length = used;
	/* Reports successful completion. */
	return 0;
}

/* Supports the encode historical operation. */
static int
encode_historical(
	int descriptor)
{
	int function_result;
	unsigned a;
	unsigned b;
	unsigned c;
	size_t source;
	size_t target;
	unsigned char input[45];
	char output[63];
	size_t length;
	int status;

	length = 0;

	/* Process each remaining element. */
	while ((status = read_chunk(descriptor, input, sizeof(input),
				    &length)) == 0 &&
	       length != 0) {

		target = 0;

		/* Process each remaining element. */
		output[target++] = historical_character((unsigned)length);
		for (source = 0; source < length; source += 3U) {
						a = input[source];
						b = source + 1U < length ? input[source + 1U] : 0;
						c = source + 2U < length ? input[source + 2U] : 0;

			output[target++] = historical_character(a >> 2);
			output[target++] =
			    historical_character((a << 4) | (b >> 4));
			output[target++] =
			    historical_character((b << 2) | (c >> 6));
			output[target++] = historical_character(c);
		}
		output[target++] = '\n';

		/* Handles a failed write all operation. */
		if (write_all(STDOUT_FILENO, output, target) != 0)
			return -1;
	}

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	/* Obtains the write all result. */
	function_result = write_all(STDOUT_FILENO, "`\nend\n", 6);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the historical character operation. */
static char
historical_character(
	unsigned value)
{
	value &= 0x3fU;

	/* Returns the computed result. */
	return value == 0 ? '`' : (char)(value + 0x20U);
}

/* Supports the read line operation. */
static int
read_line(
	int descriptor,
	char *line,
	size_t capacity)
{
	char byte;
	ssize_t count;
	size_t used;
	int too_long;

	used = 0;
	too_long = 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		count = read(descriptor, &byte, 1);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0)
			return -1;

		/* Checks the remaining item count. */
		if (count == 0) {
			/* Checks the current capacity usage. */
			if (used == 0 && !too_long)
				return 0;
			break;
		}

		/* Classifies the current byte. */
		if (byte == '\n')
			break;

		/* Checks the current capacity usage. */
		if (used + 1U < capacity)
			line[used++] = byte;
		else
			too_long = 1;
	}

	/* Handles the too long condition. */
	if (too_long) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the current capacity usage. */
	if (used != 0 && line[used - 1U] == '\r')
		used--;
	line[used] = '\0';

	/* Reports operation failure. */
	return 1;
}

/* Supports the header parse operation. */
static int
header_parse(
	char *line,
	int *base64,
	unsigned *mode,
	const char **path)
{
	char *mode_text;
	char *separator;

	/* Selects the matching prefix. */
	if (strncmp(line, "begin ", 6) == 0) {
		*base64 = 0;
		mode_text = line + 6;
	} else if (strncmp(line, "begin-base64 ", 13) == 0) {
		*base64 = 1;
		mode_text = line + 13;
	} else

		/* Reports successful completion. */
		return 0;
	separator = strchr(mode_text, ' ');

	/* Handles the separator availability. */
	if (separator == NULL || separator[1] == '\0')
		return -1;
	*separator = '\0';
	/* Handles a failed mode parse operation. */
	if (!mode_parse(mode_text, mode))
		return -1;
	*path = separator + 1;
	/* Reports operation failure. */
	return 1;
}

/* Supports the mode parse operation. */
static int
mode_parse(
	const char *text,
	unsigned *mode)
{
	unsigned value;
	size_t length;

	value = 0;
	length = 0;

	/* Continue while the operation condition remains true. */
	while (*text >= '0' && *text <= '7') {
		/* Checks the current data length. */
		if (++length > 6U)
			return 0;
		value = value * 8U + (unsigned)(*text++ - '0');
	}

	/* Validates the current text. */
	if (*text != '\0' || length == 0)
		return 0;
	*mode = value & 0777U;
	/* Reports operation failure. */
	return 1;
}

/* Supports the output open operation. */
static int
output_open(
	struct output_file *output,
	const char *path)
{
	int next;
	char *copy;
	char *component;
	char *slash;
	int directory;
	unsigned attempt;

	directory = AT_FDCWD;

	memset(output, 0, sizeof(*output));
	output->descriptor = -1;
	output->directory = -1;

	/* Selects the matching value. */
	if (strcmp(path, "-") == 0) {
		output->descriptor = STDOUT_FILENO;
		output->standard_output = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed strdup operation. */
	if (path[0] == '/' || (copy = strdup(path)) == NULL) {
		errno = path[0] == '/' ? EPERM : ENOMEM;

		/* Reports operation failure. */
		return -1;
	}

	/* Continue until the operation reaches a terminal state. */
	component = copy;
	for (;;) {
		slash = strchr(component, '/');

		/* Handles the slash availability. */
		if (slash == NULL)
			break;
		*slash = '\0';
		/* Handles a failed component valid operation. */
		if (!component_valid(component)) {
			errno = EPERM;
			goto failed;
		}

		next = openat(directory, component,
				  O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
				      O_CLOEXEC);

		/* Handles the next condition. */
		if (next < 0)
			goto failed;

		/* Handles the directory condition. */
		if (directory != AT_FDCWD)
			(void)close(directory);
		directory = next;
		component = slash + 1;
	}

	/* Handles a failed component valid operation. */
	if (!component_valid(component) ||
	    strlen(component) >= sizeof(output->final_name)) {
		errno = !component_valid(component) ? EPERM : ENAMETOOLONG;
		goto failed;
	}
	(void)strcpy(output->final_name, component);

	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 100U; attempt++) {
		(void)snprintf(output->temporary_name,
			       sizeof(output->temporary_name),
			       ".uudecode.%ld.%u", (long)getpid(), attempt);
		output->descriptor = openat(
		    directory, output->temporary_name,
		    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);

		/* Handles the reported system error. */
		if (output->descriptor >= 0 || errno != EEXIST)
			break;
	}

	/* Handles the output condition. */
	if (output->descriptor < 0)
		goto failed;
	output->directory = directory;
	free(copy);

	/* Reports successful completion. */
	return 0;

failed:

	/* Handles the directory condition. */
	if (directory != AT_FDCWD)
		(void)close(directory);
	free(copy);

	/* Reports operation failure. */
	return -1;
}

/* Supports the component valid operation. */
static int
component_valid(
	const char *component)
{
	int function_result;

	/* Computes the function result. */
	function_result = component[0] != '\0' && strcmp(component, ".") != 0 &&
	       strcmp(component, "..") != 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the decode base64 operation. */
static int
decode_base64(
	int input,
	int output)
{
	int a;
	int b;
	unsigned char decoded[3];
	size_t count;
	size_t length;
	size_t index;
	int saw_padding;
	char line[UU_LINE_MAX];
	int status;

	/* Process each remaining element. */
	while ((status = read_line(input, line, sizeof(line))) > 0) {

		saw_padding = 0;

		/* Selects the matching value. */
		if (strcmp(line, "====") == 0)
			return 0;
		length = strlen(line);

		/* Checks the current data length. */
		if (length == 0 || length > 76U || length % 4U != 0)
			return -1;

		/* Process each remaining element. */
		for (index = 0; index < length; index += 4U) {
						a = base64_value(line[index]);
						b = base64_value(line[index + 1U]);
			int c = line[index + 2U] == '='
				    ? -2
				    : base64_value(line[index + 2U]);
			int d = line[index + 3U] == '='
				    ? -2
				    : base64_value(line[index + 3U]);

						count = 1;

			/* Handles the saw padding condition. */
			if (saw_padding || a < 0 || b < 0 || c == -1 ||
			    d == -1 || (c == -2 && d != -2) ||
			    (c == -2 && (b & 15) != 0) ||
			    (d == -2 && c >= 0 && (c & 3) != 0))

				/* Reports operation failure. */
				return -1;
			decoded[0] = (unsigned char)((a << 2) | (b >> 4));

			/* Classifies the current input character. */
			if (c >= 0) {
				decoded[count++] =
				    (unsigned char)((b << 4) | (c >> 2));

				/* Checks the current descriptor. */
				if (d >= 0)
					decoded[count++] =
					    (unsigned char)((c << 6) | d);
			}

			/* Classifies the current input character. */
			if (c == -2 || d == -2) {
				saw_padding = 1;

				/* Checks the current index. */
				if (index + 4U != length)
					return -1;
			}

			/* Handles a failed write all operation. */
			if (write_all(output, decoded, count) != 0)
				return -1;
		}
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the base64 value operation. */
static int
base64_value(
	char character)
{
	/* Classifies the current input character. */
	if (character >= 'A' && character <= 'Z')
		return character - 'A';

	/* Classifies the current input character. */
	if (character >= 'a' && character <= 'z')
		return character - 'a' + 26;

	/* Classifies the current input character. */
	if (character >= '0' && character <= '9')
		return character - '0' + 52;

	/* Classifies the current input character. */
	if (character == '+')
		return 62;

	/* Classifies the current input character. */
	if (character == '/')
		return 63;

	/* Reports operation failure. */
	return -1;
}

/* Supports the decode historical operation. */
static int
decode_historical(
	int input,
	int output)
{
	unsigned a, b, c, d;
	unsigned length;
	size_t encoded;
	size_t index;
	unsigned char decoded[45];
	size_t used;
	char line[UU_LINE_MAX];
	int status;

	/* Process each remaining element. */
	while ((status = read_line(input, line, sizeof(line))) > 0) {

		used = 0;

		/* Handles a failed historical value operation. */
		if (!historical_value(line[0], &length) ||
		    length > sizeof(decoded))

			/* Reports operation failure. */
			return -1;

		/* Checks the current data length. */
		if (length == 0) {
			/* Handles a failed read line operation. */
			if (read_line(input, line, sizeof(line)) <= 0 ||
			    strcmp(line, "end") != 0)

				/* Reports operation failure. */
				return -1;

			/* Reports successful completion. */
			return 0;
		}
		encoded = ((size_t)length + 2U) / 3U * 4U;

		/* Handles a failed strlen operation. */
		if (strlen(line + 1) < encoded)
			return -1;

		/* Process each remaining element. */
		for (index = 0; index < encoded; index += 4U) {
			/* Handles a failed historical value operation. */
			if (!historical_value(line[index + 1U], &a) ||
			    !historical_value(line[index + 2U], &b) ||
			    !historical_value(line[index + 3U], &c) ||
			    !historical_value(line[index + 4U], &d))

				/* Reports operation failure. */
				return -1;

			/* Checks the current capacity usage. */
			if (used < length)
				decoded[used++] =
				    (unsigned char)((a << 2) | (b >> 4));

			/* Checks the current capacity usage. */
			if (used < length)
				decoded[used++] =
				    (unsigned char)((b << 4) | (c >> 2));

			/* Checks the current capacity usage. */
			if (used < length)
				decoded[used++] = (unsigned char)((c << 6) | d);
		}

		/* Handles a failed write all operation. */
		if (write_all(output, decoded, used) != 0)
			return -1;
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the historical value operation. */
static int
historical_value(
	char character,
	unsigned *value)
{
	unsigned byte;

	byte = (unsigned char)character;

	/* Classifies the current input character. */
	if (character == '`' || character == ' ')
		*value = 0;
	else if (byte >= 0x21U && byte <= 0x5fU)
		*value = byte - 0x20U;
	else

		/* Reports successful completion. */
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the output abort operation. */
static void
output_abort(
	struct output_file *output)
{
	/* Handles the output condition. */
	if (!output->standard_output && output->descriptor >= 0) {
		(void)close(output->descriptor);
		(void)unlinkat(output->directory, output->temporary_name, 0);
	}

	/* Handles the output condition. */
	if (output->directory >= 0)
		(void)close(output->directory);
}

/* Supports the output finish operation. */
static int
output_finish(
	struct output_file *output,
	unsigned mode)
{
	/* Handles the output condition. */
	if (output->standard_output)
		return 0;

	/* Handles a failed fchmod operation. */
	if (fchmod(output->descriptor, (mode_t)(mode & 0777U)) != 0 ||
	    close(output->descriptor) != 0) {
		output->descriptor = -1;
		output_abort(output);

		/* Reports operation failure. */
		return -1;
	}
	output->descriptor = -1;

	/* Handles a failed renameat operation. */
	if (renameat(output->directory, output->temporary_name,
		     output->directory, output->final_name) != 0) {
		output_abort(output);

		/* Reports operation failure. */
		return -1;
	}
	(void)close(output->directory);
	output->directory = -1;

	/* Reports successful completion. */
	return 0;
}
