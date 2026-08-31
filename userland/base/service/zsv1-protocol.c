/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland service zsv1 protocol support.
 */

#include "userland/base/service/zsv1-protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int ascii_alphanumeric(unsigned char character);
static int copy_wire_line(const void *data, size_t length, size_t maximum, char *output);
static int split_fields(char *line, char **fields, size_t capacity, size_t *count);
static int parse_command(const char *text, enum zsv1_command *command);
static int command_has_service(enum zsv1_command command);
static int parse_state(const char *text, enum zsv1_service_state *state);
static int parse_decimal(const char *text, uint64_t maximum, uint64_t *result);
static int token_valid(const char *token);
static int format_result(size_t capacity, size_t *length, int result);
static int decoder_failure(struct zsv1_decoder *decoder, int error);
static int decoder_record(struct zsv1_decoder *decoder, const struct zsv1_record *record);
static int service_duplicate(const struct zsv1_response *response, const char *name);
static int dependency_duplicate(const struct zsv1_response *response, enum zsv1_dependency_type type, const char *name);

/*
 * Implements the zsv1 name valid operation.
 */
int
zsv1_name_valid(
	const char *name)
{
	unsigned char character;
	size_t length;

	/* Handles a failed ascii alphanumeric operation. */
	if (name == NULL || !ascii_alphanumeric((unsigned char)name[0]))
		return 0;

	/* Process each remaining element. */
	for (length = 0; name[length] != '\0'; length++) {
				character = (unsigned char)name[length];

		/* Handles a failed ascii alphanumeric operation. */
		if (!ascii_alphanumeric(character) && character != '_' &&
		    character != '-')

			/* Reports successful completion. */
			return 0;

		/* Checks the current data length. */
		if (length + 1 >= ZSV1_NAME_CAPACITY)
			return 0;
	}

	/* Returns the computed result. */
	return length != 0;
}

/*
 * Implements the zsv1 command name operation.
 */
const char *
zsv1_command_name(
	enum zsv1_command command)
{
	static const char *const names[] = {
	    "LIST",   "SHOW", "START",	  "STOP",   "RESTART",
	    "RELOAD", "HALT", "POWEROFF", "REBOOT",
	};

	/* Handles the command condition. */
	if ((unsigned int)command >= sizeof(names) / sizeof(names[0]))
		return NULL;

	/* Returns the computed result. */
	return names[command];
}

/*
 * Implements the zsv1 state name operation.
 */
const char *
zsv1_state_name(
	enum zsv1_service_state state)
{
	static const char *const names[] = {
	    "stopped", "starting", "running", "completed", "failed", "skipped",
	};

	/* Handles the state condition. */
	if ((unsigned int)state >= sizeof(names) / sizeof(names[0]))
		return NULL;

	/* Returns the computed result. */
	return names[state];
}

/*
 * Implements the zsv1 request parse operation.
 */
int
zsv1_request_parse(
	const void *data,
	size_t length,
	struct zsv1_request *request)
{
	char line[ZSV1_REQUEST_MAX + 1U], *fields[4];
	struct zsv1_request parsed;
	size_t count;

	/* Handles the request availability. */
	if (request == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed copy wire line operation. */
	if (copy_wire_line(data, length, ZSV1_REQUEST_MAX, line) != 0)
		return -1;

	/* Handles a failed split fields operation. */
	if (split_fields(line, fields, sizeof(fields) / sizeof(fields[0]),
			 &count) != 0)

		/* Reports operation failure. */
		return -1;

	/* Checks the remaining item count. */
	if (count == 0 || strcmp(fields[0], "ZSV1") != 0) {
		errno = EPROTONOSUPPORT;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed parse command operation. */
	if (count < 2 || parse_command(fields[1], &parsed.command) != 0)
		return -1;

	/* Handles a failed command has service operation. */
	if (count != (size_t)(command_has_service(parsed.command) ? 3 : 2)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	parsed.service[0] = '\0';

	/* Handles the command has service condition. */
	if (command_has_service(parsed.command)) {
		/* Handles a failed zsv1 name valid operation. */
		if (!zsv1_name_valid(fields[2])) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		strcpy(parsed.service, fields[2]);
	}
	*request = parsed;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the zsv1 request format operation.
 */
int
zsv1_request_format(
	const struct zsv1_request *request,
	char *output,
	size_t capacity,
	size_t *length)
{
	const char *command;
	int result;

	/* Handles the request availability. */
	if (request == NULL || output == NULL || capacity == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	command = zsv1_command_name(request->command);

	/* Handles a failed command has service operation. */
	if (command == NULL ||
	    (command_has_service(request->command) &&
	     !zsv1_name_valid(request->service)) ||
	    (!command_has_service(request->command) && request->service[0])) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed command has service operation. */
	if (command_has_service(request->command))
		result = snprintf(output, capacity, "ZSV1 %s %s\n", command,
				  request->service);
	else
		result = snprintf(output, capacity, "ZSV1 %s\n", command);

	/* Checks the operation result. */
	if (result < 0 || (size_t)result >= capacity ||
	    (size_t)result > ZSV1_REQUEST_MAX) {
		errno = EMSGSIZE;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the length availability. */
	if (length != NULL)
		*length = (size_t)result;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the zsv1 record parse operation.
 */
int
zsv1_record_parse(
	const void *data,
	size_t length,
	struct zsv1_record *record)
{
	char line[ZSV1_RESPONSE_LINE_MAX + 1U], *fields[8];
	struct zsv1_record parsed;
	uint64_t number;
	size_t count;

	/* Handles the record availability. */
	if (record == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed copy wire line operation. */
	if (copy_wire_line(data, length, ZSV1_RESPONSE_LINE_MAX, line) != 0)
		return -1;

	/* Handles a failed split fields operation. */
	if (split_fields(line, fields, sizeof(fields) / sizeof(fields[0]),
			 &count) != 0)

		/* Reports operation failure. */
		return -1;

	/* Checks the remaining item count. */
	if (count == 0 || strcmp(fields[0], "ZSV1") != 0) {
		errno = EPROTONOSUPPORT;

		/* Reports operation failure. */
		return -1;
	}
	memset(&parsed, 0, sizeof(parsed));

	/* Checks the remaining item count. */
	if (count == 6 && strcmp(fields[1], "SERVICE") == 0) {
		parsed.type = ZSV1_RECORD_SERVICE;

		/* Handles a failed zsv1 name valid operation. */
		if (!zsv1_name_valid(fields[2]) ||
		    parse_state(fields[3], &parsed.service.state) != 0 ||
		    (strcmp(fields[4], "0") != 0 &&
		     strcmp(fields[4], "1") != 0) ||
		    parse_decimal(fields[5], INT_MAX, &number) != 0)

			/* Reports operation failure. */
			return -1;
		strcpy(parsed.service.name, fields[2]);
		parsed.service.enabled = fields[4][0] == '1';
		parsed.service.pid = (pid_t)number;
	} else if (count == 3 && strcmp(fields[1], "AFTER") == 0) {
		parsed.type = ZSV1_RECORD_AFTER;

		/* Handles a failed zsv1 name valid operation. */
		if (!zsv1_name_valid(fields[2]))
			goto invalid;
		strcpy(parsed.name, fields[2]);
	} else if (count == 3 && strcmp(fields[1], "REQUIRES") == 0) {
		parsed.type = ZSV1_RECORD_REQUIRES;

		/* Handles a failed zsv1 name valid operation. */
		if (!zsv1_name_valid(fields[2]))
			goto invalid;
		strcpy(parsed.name, fields[2]);
	} else if (count == 3 && strcmp(fields[1], "OK") == 0) {
		parsed.type = ZSV1_RECORD_OK;

		/* Handles a failed token valid operation. */
		if (!token_valid(fields[2]))
			goto invalid;
		strcpy(parsed.token, fields[2]);
	} else if (count == 4 && strcmp(fields[1], "ERROR") == 0) {
		parsed.type = ZSV1_RECORD_ERROR;

		/* Handles a failed parse decimal operation. */
		if (parse_decimal(fields[2], INT_MAX, &number) != 0 ||
		    number == 0 || !token_valid(fields[3]))
			goto invalid;
		parsed.error_number = (int)number;
		strcpy(parsed.token, fields[3]);
	} else if (count == 2 && strcmp(fields[1], "END") == 0) {
		parsed.type = ZSV1_RECORD_END;
	} else {
		goto invalid;
	}
	*record = parsed;
	/* Reports successful completion. */
	return 0;

invalid:
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the zsv1 record format operation.
 */
int
zsv1_record_format(
	const struct zsv1_record *record,
	char *output,
	size_t capacity,
	size_t *length)
{
	int function_result;
	const char *state;
	int result;

	/* Handles the record availability. */
	if (record == NULL || output == NULL || capacity == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Dispatch the selected syntax or record type. */
	switch (record->type) {
	case ZSV1_RECORD_SERVICE:
		state = zsv1_state_name(record->service.state);

		/* Handles a failed zsv1 name valid operation. */
		if (!zsv1_name_valid(record->service.name) || state == NULL ||
		    (record->service.enabled != 0 &&
		     record->service.enabled != 1) ||
		    record->service.pid < 0)
			goto invalid;
		result = snprintf(
		    output, capacity, "ZSV1 SERVICE %s %s %d %ld\n",
		    record->service.name, state, record->service.enabled,
		    (long)record->service.pid);
		break;
	case ZSV1_RECORD_AFTER:
	case ZSV1_RECORD_REQUIRES:
		/* Handles a failed zsv1 name valid operation. */
		if (!zsv1_name_valid(record->name))
			goto invalid;
		result = snprintf(
		    output, capacity, "ZSV1 %s %s\n",
		    record->type == ZSV1_RECORD_AFTER ? "AFTER" : "REQUIRES",
		    record->name);
		break;
	case ZSV1_RECORD_OK:
		/* Handles a failed token valid operation. */
		if (!token_valid(record->token))
			goto invalid;
		result =
		    snprintf(output, capacity, "ZSV1 OK %s\n", record->token);
		break;
	case ZSV1_RECORD_ERROR:
		/* Handles an operation failure. */
		if (record->error_number <= 0 || !token_valid(record->token))
			goto invalid;
		result = snprintf(output, capacity, "ZSV1 ERROR %d %s\n",
				  record->error_number, record->token);
		break;
	case ZSV1_RECORD_END:
		result = snprintf(output, capacity, "ZSV1 END\n");
		break;
	default:
		goto invalid;
	}

	/* Obtains the format result result. */
	function_result = format_result(capacity, length, result);

	/* Returns the computed result. */
	return function_result;

invalid:
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the zsv1 decoder init operation.
 */
void
zsv1_decoder_init(
	struct zsv1_decoder *decoder)
{
	/* Handles the decoder availability. */
	if (decoder != NULL)
		memset(decoder, 0, sizeof(*decoder));
}

/*
 * Implements the zsv1 decoder feed operation.
 */
int
zsv1_decoder_feed(
	struct zsv1_decoder *decoder,
	const void *data,
	size_t length)
{
	int function_result;
	struct zsv1_record record;
	unsigned char byte;
	const unsigned char *bytes;
	size_t index;

	bytes = data;

	/* Handles the decoder availability. */
	if (decoder == NULL || (data == NULL && length != 0)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles an operation failure. */
	if (decoder->failed) {
		/* Obtains the decoder failure result. */
		function_result = decoder_failure(decoder, EINVAL);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {

				byte = bytes[index];

		/* Handles the decoder condition. */
		if (decoder->response.ended) {
			/* Obtains the decoder failure result. */
			function_result = decoder_failure(decoder, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}

		/* Classifies the current byte. */
		if (byte != '\n' && (byte < 0x20 || byte > 0x7e)) {
			/* Obtains the decoder failure result. */
			function_result = decoder_failure(decoder, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the decoder condition. */
		if (decoder->line_length == ZSV1_RESPONSE_LINE_MAX) {
			/* Obtains the decoder failure result. */
			function_result = decoder_failure(decoder, EMSGSIZE);

			/* Returns the computed result. */
			return function_result;
		}
		decoder->line[decoder->line_length++] = (char)byte;

		/* Classifies the current byte. */
		if (byte != '\n')
			continue;
		decoder->line[decoder->line_length] = '\0';

		/* Handles a failed zsv1 record parse operation. */
		if (zsv1_record_parse(decoder->line, decoder->line_length,
				      &record) != 0) {
			/* Obtains the decoder failure result. */
			function_result = decoder_failure(decoder, errno);

			/* Returns the computed result. */
			return function_result;
		}
		decoder->line_length = 0;

		/* Handles a failed decoder record operation. */
		if (decoder_record(decoder, &record) != 0)
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the zsv1 decoder finish operation.
 */
int
zsv1_decoder_finish(
	struct zsv1_decoder *decoder)
{
	int function_result;

	/* Handles the decoder availability. */
	if (decoder == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles an operation failure. */
	if (decoder->failed || decoder->line_length != 0 ||
	    !decoder->response.ended) {
		/* Obtains the decoder failure result. */
		function_result = decoder_failure(decoder, EINVAL);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the zsv1 decoder response operation.
 */
const struct zsv1_response *
zsv1_decoder_response(
	const struct zsv1_decoder *decoder)
{
	/* Handles an operation failure. */
	if (decoder == NULL || decoder->failed || decoder->line_length != 0 ||
	    !decoder->response.ended)

		/* Reports that no result is available. */
		return NULL;

	/* Returns the computed result. */
	return &decoder->response;
}

/* Supports the ascii alphanumeric operation. */
static int
ascii_alphanumeric(
	unsigned char character)
{
	/* Returns the computed result. */
	return (character >= 'A' && character <= 'Z') ||
	       (character >= 'a' && character <= 'z') ||
	       (character >= '0' && character <= '9');
}

/* Supports the copy wire line operation. */
static int
copy_wire_line(
	const void *data,
	size_t length,
	size_t maximum,
	char *output)
{
	const unsigned char *bytes;
	size_t index;

	bytes = data;

	/* Handles the data availability. */
	if (data == NULL || output == NULL || length < 2) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the current data length. */
	if (length > maximum) {
		errno = EMSGSIZE;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the bytes condition. */
	if (bytes[length - 1] != '\n') {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = 0; index + 1 < length; index++) {
		/* Handles the bytes condition. */
		if (bytes[index] < 0x20 || bytes[index] > 0x7e) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
	}
	memcpy(output, data, length - 1);
	output[length - 1] = '\0';

	/* Reports successful completion. */
	return 0;
}

/* Supports the split fields operation. */
static int
split_fields(
	char *line,
	char **fields,
	size_t capacity,
	size_t *count)
{
	char *cursor;
	size_t field_count;

	cursor = line;
	field_count = 0;

	/* Checks the current cursor position. */
	if (*cursor == '\0' || *cursor == ' ')
		goto invalid;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the field count condition. */
		if (field_count == capacity)
			goto invalid;

		/* Continue while the operation condition remains true. */
		fields[field_count++] = cursor;
		while (*cursor != '\0' && *cursor != ' ')
			cursor++;

		/* Checks the current cursor position. */
		if (*cursor == '\0')
			break;
		*cursor++ = '\0';
		/* Checks the current cursor position. */
		if (*cursor == '\0' || *cursor == ' ')
			goto invalid;
	}
	*count = field_count;
	/* Reports successful completion. */
	return 0;

invalid:
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/* Supports the parse command operation. */
static int
parse_command(
	const char *text,
	enum zsv1_command *command)
{
	enum zsv1_command candidate;

	/* Process each element required by the operation. */
	for (candidate = ZSV1_COMMAND_LIST; candidate <= ZSV1_COMMAND_REBOOT;
	     candidate++) {
		/* Handles a failed zsv1 command name operation. */
		if (strcmp(text, zsv1_command_name(candidate)) == 0) {
			*command = candidate;
			/* Reports successful completion. */
			return 0;
		}
	}
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/* Supports the command has service operation. */
static int
command_has_service(
	enum zsv1_command command)
{
	/* Returns the computed result. */
	return command == ZSV1_COMMAND_SHOW || command == ZSV1_COMMAND_START ||
	       command == ZSV1_COMMAND_STOP || command == ZSV1_COMMAND_RESTART;
}

/* Supports the parse state operation. */
static int
parse_state(
	const char *text,
	enum zsv1_service_state *state)
{
	enum zsv1_service_state candidate;

	/* Process each element required by the operation. */
	for (candidate = ZSV1_STATE_STOPPED; candidate <= ZSV1_STATE_SKIPPED;
	     candidate++) {
		/* Handles a failed zsv1 state name operation. */
		if (strcmp(text, zsv1_state_name(candidate)) == 0) {
			*state = candidate;
			/* Reports successful completion. */
			return 0;
		}
	}
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/* Supports the parse decimal operation. */
static int
parse_decimal(
	const char *text,
	uint64_t maximum,
	uint64_t *result)
{
	unsigned int digit;
	uint64_t value;
	size_t index;

	value = 0;

	/* Handles the text availability. */
	if (text == NULL || text[0] < '0' || text[0] > '9')
		goto invalid;

	/* Process each remaining element. */
	for (index = 0; text[index] != '\0'; index++) {
		/* Validates the current text. */
		if (text[index] < '0' || text[index] > '9')
			goto invalid;
		digit = (unsigned int)(text[index] - '0');

		/* Validates the current value. */
		if (value > (maximum - digit) / 10U)
			goto invalid;
		value = value * 10U + digit;
	}
	*result = value;
	/* Reports successful completion. */
	return 0;

invalid:
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/* Supports the token valid operation. */
static int
token_valid(
	const char *token)
{
	unsigned char character;
	size_t length;
	int previous_hyphen;

	previous_hyphen = 0;

	/* Handles the token availability. */
	if (token == NULL || token[0] < 'a' || token[0] > 'z')
		return 0;

	/* Process each remaining element. */
	for (length = 0; token[length] != '\0'; length++) {
				character = (unsigned char)token[length];

		/* Classifies the current input character. */
		if ((character < 'a' || character > 'z') &&
		    (character < '0' || character > '9') && character != '-')

			/* Reports successful completion. */
			return 0;

		/* Classifies the current input character. */
		if (character == '-' && previous_hyphen)
			return 0;
		previous_hyphen = character == '-';

		/* Checks the current data length. */
		if (length + 1 >= ZSV1_TOKEN_CAPACITY)
			return 0;
	}

	/* Returns the computed result. */
	return length != 0 && !previous_hyphen;
}

/* Supports the format result operation. */
static int
format_result(
	size_t capacity,
	size_t *length,
	int result)
{
	/* Checks the operation result. */
	if (result < 0 || (size_t)result >= capacity ||
	    (size_t)result > ZSV1_RESPONSE_LINE_MAX) {
		errno = EMSGSIZE;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the length availability. */
	if (length != NULL)
		*length = (size_t)result;
	/* Reports successful completion. */
	return 0;
}

/* Supports the decoder failure operation. */
static int
decoder_failure(
	struct zsv1_decoder *decoder,
	int error)
{
	decoder->failed = 1;
	errno = error;

	/* Reports operation failure. */
	return -1;
}

/* Supports the decoder record operation. */
static int
decoder_record(
	struct zsv1_decoder *decoder,
	const struct zsv1_record *record)
{
	int function_result;
	enum zsv1_dependency_type type;
	struct zsv1_dependency_record *dependency;
	struct zsv1_response *response;

	response = &decoder->response;

	/* Handles an operation failure. */
	if (response->error_present || response->ok_present) {
		/* Handles the record condition. */
		if (record->type == ZSV1_RECORD_END) {
			response->ended = 1;

			/* Reports successful completion. */
			return 0;
		}

		/* Obtains the decoder failure result. */
		function_result = decoder_failure(decoder, EINVAL);

		/* Returns the computed result. */
		return function_result;
	}

	/* Dispatch the selected syntax or record type. */
	switch (record->type) {
	case ZSV1_RECORD_SERVICE:
		/* Handles a failed service duplicate operation. */
		if (response->service_count == ZSV1_SERVICE_MAX ||
		    service_duplicate(response, record->service.name)) {
			/* Obtains the decoder failure result. */
			function_result = decoder_failure(decoder, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		response->services[response->service_count++] = record->service;

		/* Reports successful completion. */
		return 0;
	case ZSV1_RECORD_AFTER:
	case ZSV1_RECORD_REQUIRES:
				type = record->type == ZSV1_RECORD_AFTER
		? ZSV1_DEPENDENCY_AFTER
		: ZSV1_DEPENDENCY_REQUIRES;

	/* Handles a failed dependency duplicate operation. */
	if (response->dependency_count == ZSV1_DEPENDENCY_MAX ||
	    dependency_duplicate(response, type, record->name)) {
		/* Obtains the decoder failure result. */
		function_result = decoder_failure(decoder, EINVAL);

		/* Returns the computed result. */
		return function_result;
	}

	dependency =
	    &response->dependencies[response->dependency_count++];
	dependency->type = type;
	strcpy(dependency->name, record->name);

	/* Reports successful completion. */
	return 0;
	case ZSV1_RECORD_OK:
		response->ok_present = 1;
		strcpy(response->ok_token, record->token);

		/* Reports successful completion. */
		return 0;
	case ZSV1_RECORD_ERROR:
		/* Handles the response condition. */
		if (response->service_count != 0 ||
		    response->dependency_count != 0) {
			/* Obtains the decoder failure result. */
			function_result = decoder_failure(decoder, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		response->error_present = 1;
		response->error_number = record->error_number;
		strcpy(response->error_reason, record->token);

		/* Reports successful completion. */
		return 0;
	case ZSV1_RECORD_END:
		response->ended = 1;

		/* Reports successful completion. */
		return 0;
	default:
		/* Obtains the decoder failure result. */
		function_result = decoder_failure(decoder, EINVAL);

		/* Returns the computed result. */
		return function_result;
	}
}

/* Supports the service duplicate operation. */
static int
service_duplicate(
	const struct zsv1_response *response,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < response->service_count; index++) {
		/* Selects the matching value. */
		if (strcmp(response->services[index].name, name) == 0)
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the dependency duplicate operation. */
static int
dependency_duplicate(
	const struct zsv1_response *response,
	enum zsv1_dependency_type type,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < response->dependency_count; index++) {
		/* Handles the response condition. */
		if (response->dependencies[index].type == type &&
		    strcmp(response->dependencies[index].name, name) == 0)

			/* Reports operation failure. */
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}
