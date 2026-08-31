/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland lpd client component.
 */

#include "userland/base/lp/lpd-client.h"

#include "userland/base/common/command.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define LPD_LINE_MAX 512
#define LPD_IO_TIMEOUT_SECONDS 15

static int lpd_copy_component(char *output, size_t capacity, const char *input, size_t length);
static int lpd_component_valid(const char *text, size_t maximum);
static int lpd_build_control(char **output, size_t *size, const char *host, const char *user, const char *title, const char *source_name, const char *data_name, unsigned copies, int mail);
static int lpd_connect(const struct lpd_destination *destination);
static int lpd_write_command(int descriptor, unsigned char command, const char *argument);
static int lpd_read_ack(int descriptor);
static int lpd_write_file_record(int descriptor, unsigned char command, const char *name, int input, uint64_t size);
static int lpd_write_record_header(int descriptor, unsigned char command, uint64_t size, const char *name);
static int lpd_write_memory_record(int descriptor, unsigned char command, const char *name, const void *data, size_t size);

/*
 * Parses a direct LPD destination name.
 */
int
lpd_parse_destination(
	const char *text,
	struct lpd_destination *result)
{
	const char *slash;
	const char *colon;
	const char *port;
	size_t host_length;
	size_t queue_length;
	unsigned long port_value;
	char *end;
	int status;

	/* Handles the text availability. */
	if (text == NULL || result == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	slash = strchr(text, '/');

	/* Handles a failed strchr operation. */
	if (slash == NULL || slash == text || slash[1] == '\0' ||
	    strchr(slash + 1, '/') != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	queue_length = strlen(slash + 1);
	status = lpd_copy_component(
		result->queue,
		sizeof(result->queue),
		slash + 1,
		queue_length);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	colon = memchr(text, ':', (size_t)(slash - text));

	/* Handles the colon availability. */
	if (colon == NULL) {
		host_length = (size_t)(slash - text);
		port = "515";
	} else {
		/* Handles a failed memchr operation. */
		if (memchr(colon + 1, ':', (size_t)(slash - colon - 1)) != NULL) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}

		host_length = (size_t)(colon - text);
		port = colon + 1;

		/* Handles the port condition. */
		if (port == slash) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}

		errno = 0;
		port_value = strtoul(port, &end, 10);

		/* Handles the reported system error. */
		if (errno != 0 || end != slash || port_value == 0 ||
		    port_value > 65535UL) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
	}

	status = lpd_copy_component(
		result->host,
		sizeof(result->host),
		text,
		host_length);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	/* Handles the colon availability. */
	if (colon == NULL) {
		strcpy(result->service, port);
	} else {
		host_length = (size_t)(slash - port);

		/* Handles the host length condition. */
		if (host_length >= sizeof(result->service)) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}

		memcpy(result->service, port, host_length);
		result->service[host_length] = '\0';
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Submits one staged PDF as an LPD job.
 */
int
lpd_submit(
	const struct lpd_destination *destination,
	int input,
	uint64_t size,
	const char *host,
	const char *user,
	const char *title,
	const char *source_name,
	unsigned copies,
	int mail,
	unsigned sequence)
{
	char control_name[LPD_LINE_MAX];
	char data_name[LPD_LINE_MAX];
	char *control;
	size_t control_size;
	int descriptor;
	int result;
	int saved;

	/* Handles the destination availability. */
	if (destination == NULL || input < 0 || host == NULL || user == NULL ||
	    title == NULL || source_name == NULL || copies == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	result = snprintf(
		control_name,
		sizeof(control_name),
		"cfA%03u%s",
		sequence % 1000U,
		host);

	/* Checks the operation result. */
	if (result < 0 || (size_t)result >= sizeof(control_name)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	result = snprintf(
		data_name,
		sizeof(data_name),
		"dfA%03u%s",
		sequence % 1000U,
		host);

	/* Checks the operation result. */
	if (result < 0 || (size_t)result >= sizeof(data_name)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	result = lpd_build_control(
		&control,
		&control_size,
		host,
		user,
		title,
		source_name,
		data_name,
		copies,
		mail);

	/* Checks the operation result. */
	if (result != 0)
		return -1;

	descriptor = lpd_connect(destination);

	/* Checks the file descriptor. */
	if (descriptor < 0) {
		free(control);

		/* Reports operation failure. */
		return -1;
	}

	result = lpd_write_command(descriptor, 2, destination->queue);

	/* Checks the operation result. */
	if (result == 0) {
		result = lpd_write_file_record(
			descriptor,
			3,
			data_name,
			input,
			size);
	}

	/* Checks the operation result. */
	if (result == 0) {
		result = lpd_write_memory_record(
			descriptor,
			2,
			control_name,
			control,
			control_size);
	}

	saved = errno;
	free(control);

	/* Handles a failed close operation. */
	if (close(descriptor) != 0 && result == 0) {
		result = -1;
		saved = errno;
	}
	errno = saved;

	/* Returns the computed result. */
	return result;
}

/* Copies one validated destination component. */
static int
lpd_copy_component(
	char *output,
	size_t capacity,
	const char *input,
	size_t length)
{
	char temporary[LPD_HOST_MAX + 1];

	/* Checks the current data length. */
	if (length == 0 || length >= capacity || length >= sizeof(temporary)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	memcpy(temporary, input, length);
	temporary[length] = '\0';

	/* Handles a failed lpd component valid operation. */
	if (!lpd_component_valid(temporary, capacity - 1)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	memcpy(output, temporary, length + 1);

	/* Reports successful completion. */
	return 0;
}

/* Checks an LPD control-field component. */
static int
lpd_component_valid(
	const char *text,
	size_t maximum)
{
	size_t length;
	size_t index;
	unsigned char value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		return 0;

	length = strlen(text);

	/* Checks the current data length. */
	if (length > maximum)
		return 0;

	/* Reject control characters and protocol separators. */
	for (index = 0; index < length; index++) {
		value = (unsigned char)text[index];

		/* Validates the current value. */
		if (value < 0x21U || value > 0x7eU || value == '/')
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Builds the bounded LPD control-file payload. */
static int
lpd_build_control(
	char **output,
	size_t *size,
	const char *host,
	const char *user,
	const char *title,
	const char *source_name,
	const char *data_name,
	unsigned copies,
	int mail)
{
	char *control;
	size_t capacity;
	size_t used;
	unsigned copy;
	int length;

	/* Handles a failed lpd component valid operation. */
	if (!lpd_component_valid(host, LPD_HOST_MAX) ||
	    !lpd_component_valid(user, 63) ||
	    !lpd_component_valid(title, 255) ||
	    !lpd_component_valid(source_name, 255) ||
	    !lpd_component_valid(data_name, LPD_LINE_MAX - 1)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the copies condition. */
	if (copies > 999U) {
		errno = E2BIG;

		/* Reports operation failure. */
		return -1;
	}

	capacity = 1024U + (size_t)copies * (strlen(data_name) + 2U);
	control = malloc(capacity);

	/* Handles the control availability. */
	if (control == NULL)
		return -1;

	length = snprintf(
		control,
		capacity,
		"H%s\nP%s\nJ%s\n",
		host,
		user,
		title);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= capacity) {
		free(control);
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	used = (size_t)length;

	/* Handles the mail condition. */
	if (mail) {
		length = snprintf(control + used, capacity - used, "M%s\n", user);

		/* Checks the current data length. */
		if (length < 0 || (size_t)length >= capacity - used) {
			free(control);
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		used += (size_t)length;
	}

	/* Add one raw-data command for every requested copy. */
	for (copy = 0; copy < copies; copy++) {
		length = snprintf(
			control + used,
			capacity - used,
			"l%s\n",
			data_name);

		/* Checks the current data length. */
		if (length < 0 || (size_t)length >= capacity - used) {
			free(control);
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		used += (size_t)length;
	}

	length = snprintf(
		control + used,
		capacity - used,
		"U%s\nN%s\n",
		data_name,
		source_name);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= capacity - used) {
		free(control);
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	used += (size_t)length;

	*output = control;
	*size = used;

	/* Reports successful completion. */
	return 0;
}

/* Opens a timed stream connection to the LPD server. */
static int
lpd_connect(
	const struct lpd_destination *destination)
{
	struct addrinfo hints;
	struct addrinfo *addresses;
	struct addrinfo *current;
	struct timeval timeout;
	int descriptor;
	int status;
	int saved;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	status = getaddrinfo(
		destination->host,
		destination->service,
		&hints,
		&addresses);

	/* Checks the operation status. */
	if (status != 0) {
		errno = status == EAI_SYSTEM ? errno : EHOSTUNREACH;

		/* Reports operation failure. */
		return -1;
	}

	descriptor = -1;
	saved = ECONNREFUSED;

	/* Try every resolved address until one accepts the connection. */
	for (current = addresses; current != NULL; current = current->ai_next) {
		descriptor = socket(
			current->ai_family,
			current->ai_socktype,
			current->ai_protocol);

		/* Checks the file descriptor. */
		if (descriptor < 0) {
			saved = errno;
			continue;
		}

		status = connect(descriptor, current->ai_addr, current->ai_addrlen);

		/* Checks the operation status. */
		if (status == 0)
			break;

		saved = errno;
		close(descriptor);
		descriptor = -1;
	}

	freeaddrinfo(addresses);

	/* Checks the file descriptor. */
	if (descriptor < 0) {
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}

	timeout.tv_sec = LPD_IO_TIMEOUT_SECONDS;
	timeout.tv_usec = 0;
	status = setsockopt(
		descriptor,
		SOL_SOCKET,
		SO_RCVTIMEO,
		&timeout,
		sizeof(timeout));

	/* Checks the operation status. */
	if (status == 0) {
		status = setsockopt(
			descriptor,
			SOL_SOCKET,
			SO_SNDTIMEO,
			&timeout,
			sizeof(timeout));
	}

	/* Checks the operation status. */
	if (status != 0) {
		saved = errno;
		close(descriptor);
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return descriptor;
}

/* Writes one single-line LPD command. */
static int
lpd_write_command(
	int descriptor,
	unsigned char command,
	const char *argument)
{
	int function_result;
	char line[LPD_LINE_MAX];
	int length;
	int status;

	length = snprintf(line + 1, sizeof(line) - 1, "%s\n", argument);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(line) - 1) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	line[0] = (char)command;
	status = command_write_all(descriptor, line, (size_t)length + 1);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	/* Obtains the lpd read ack result. */
	function_result = lpd_read_ack(descriptor);

	/* Returns the computed result. */
	return function_result;
}

/* Reads and validates one LPD acknowledgement byte. */
static int
lpd_read_ack(
	int descriptor)
{
	unsigned char acknowledgement;
	ssize_t count;

	/* Retry only an interrupted acknowledgement read. */
	for (;;) {
		count = read(descriptor, &acknowledgement, 1);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		break;
	}

	/* Checks the remaining item count. */
	if (count != 1) {
		/* Checks the remaining item count. */
		if (count == 0)
			errno = ECONNRESET;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the acknowledgement condition. */
	if (acknowledgement != 0) {
		errno = ECONNREFUSED;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Writes one file-backed LPD record. */
static int
lpd_write_file_record(
	int descriptor,
	unsigned char command,
	const char *name,
	int input,
	uint64_t size)
{
	int function_result;
	unsigned char buffer[4096];
	unsigned char terminator;
	uint64_t remaining;
	size_t wanted;
	ssize_t count;
	int status;

	status = lpd_write_record_header(descriptor, command, size, name);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	remaining = size;

	/* Send exactly the byte count announced to the server. */
	while (remaining != 0) {
		/* Handles the remaining condition. */
		if (remaining > sizeof(buffer))
			wanted = sizeof(buffer);
		else
			wanted = (size_t)remaining;

		count = read(input, buffer, wanted);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0) {
			/* Checks the remaining item count. */
			if (count == 0)
				errno = EIO;

			/* Reports operation failure. */
			return -1;
		}

		status = command_write_all(descriptor, buffer, (size_t)count);

		/* Checks the operation status. */
		if (status != 0)
			return -1;

		remaining -= (uint64_t)count;
	}

	terminator = 0;
	status = command_write_all(descriptor, &terminator, 1);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	/* Obtains the lpd read ack result. */
	function_result = lpd_read_ack(descriptor);

	/* Returns the computed result. */
	return function_result;
}

/* Writes an LPD record header and receives its acknowledgement. */
static int
lpd_write_record_header(
	int descriptor,
	unsigned char command,
	uint64_t size,
	const char *name)
{
	int function_result;
	char argument[LPD_LINE_MAX];
	int length;

	length = snprintf(
		argument,
		sizeof(argument),
		"%llu %s",
		(unsigned long long)size,
		name);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(argument)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the lpd write command result. */
	function_result = lpd_write_command(descriptor, command, argument);

	/* Returns the computed result. */
	return function_result;
}

/* Writes one memory-backed LPD record. */
static int
lpd_write_memory_record(
	int descriptor,
	unsigned char command,
	const char *name,
	const void *data,
	size_t size)
{
	int function_result;
	unsigned char terminator;
	int status;

	status = lpd_write_record_header(descriptor, command, size, name);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	status = command_write_all(descriptor, data, size);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	terminator = 0;
	status = command_write_all(descriptor, &terminator, 1);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	/* Obtains the lpd read ack result. */
	function_result = lpd_read_ack(descriptor);

	/* Returns the computed result. */
	return function_result;
}
