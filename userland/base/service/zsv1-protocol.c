/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/zsv1-protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int
ascii_alphanumeric(unsigned char character)
{
	return (character >= 'A' && character <= 'Z') ||
	       (character >= 'a' && character <= 'z') ||
	       (character >= '0' && character <= '9');
}

int
zsv1_name_valid(const char *name)
{
	size_t length;

	if (name == NULL || !ascii_alphanumeric((unsigned char)name[0]))
		return 0;
	for (length = 0; name[length] != '\0'; length++) {
		unsigned char character = (unsigned char)name[length];

		if (!ascii_alphanumeric(character) && character != '_' &&
		    character != '-')
			return 0;
		if (length + 1 >= ZSV1_NAME_CAPACITY)
			return 0;
	}
	return length != 0;
}

static int
token_valid(const char *token)
{
	size_t length;
	int previous_hyphen = 0;

	if (token == NULL || token[0] < 'a' || token[0] > 'z')
		return 0;
	for (length = 0; token[length] != '\0'; length++) {
		unsigned char character = (unsigned char)token[length];

		if ((character < 'a' || character > 'z') &&
		    (character < '0' || character > '9') && character != '-')
			return 0;
		if (character == '-' && previous_hyphen)
			return 0;
		previous_hyphen = character == '-';
		if (length + 1 >= ZSV1_TOKEN_CAPACITY)
			return 0;
	}
	return length != 0 && !previous_hyphen;
}

const char *
zsv1_command_name(enum zsv1_command command)
{
	static const char *const names[] = {
	    "LIST",   "SHOW", "START",	  "STOP",   "RESTART",
	    "RELOAD", "HALT", "POWEROFF", "REBOOT",
	};

	if ((unsigned int)command >= sizeof(names) / sizeof(names[0]))
		return NULL;
	return names[command];
}

const char *
zsv1_state_name(enum zsv1_service_state state)
{
	static const char *const names[] = {
	    "stopped", "starting", "running", "completed", "failed", "skipped",
	};

	if ((unsigned int)state >= sizeof(names) / sizeof(names[0]))
		return NULL;
	return names[state];
}

static int
parse_command(const char *text, enum zsv1_command *command)
{
	enum zsv1_command candidate;

	for (candidate = ZSV1_COMMAND_LIST; candidate <= ZSV1_COMMAND_REBOOT;
	     candidate++) {
		if (strcmp(text, zsv1_command_name(candidate)) == 0) {
			*command = candidate;
			return 0;
		}
	}
	errno = EINVAL;
	return -1;
}

static int
parse_state(const char *text, enum zsv1_service_state *state)
{
	enum zsv1_service_state candidate;

	for (candidate = ZSV1_STATE_STOPPED; candidate <= ZSV1_STATE_SKIPPED;
	     candidate++) {
		if (strcmp(text, zsv1_state_name(candidate)) == 0) {
			*state = candidate;
			return 0;
		}
	}
	errno = EINVAL;
	return -1;
}

static int
copy_wire_line(const void *data, size_t length, size_t maximum, char *output)
{
	const unsigned char *bytes = data;
	size_t index;

	if (data == NULL || output == NULL || length < 2) {
		errno = EINVAL;
		return -1;
	}
	if (length > maximum) {
		errno = EMSGSIZE;
		return -1;
	}
	if (bytes[length - 1] != '\n') {
		errno = EINVAL;
		return -1;
	}
	for (index = 0; index + 1 < length; index++) {
		if (bytes[index] < 0x20 || bytes[index] > 0x7e) {
			errno = EINVAL;
			return -1;
		}
	}
	memcpy(output, data, length - 1);
	output[length - 1] = '\0';
	return 0;
}

static int
split_fields(char *line, char **fields, size_t capacity, size_t *count)
{
	char *cursor = line;
	size_t field_count = 0;

	if (*cursor == '\0' || *cursor == ' ')
		goto invalid;
	for (;;) {
		if (field_count == capacity)
			goto invalid;
		fields[field_count++] = cursor;
		while (*cursor != '\0' && *cursor != ' ')
			cursor++;
		if (*cursor == '\0')
			break;
		*cursor++ = '\0';
		if (*cursor == '\0' || *cursor == ' ')
			goto invalid;
	}
	*count = field_count;
	return 0;

invalid:
	errno = EINVAL;
	return -1;
}

static int
command_has_service(enum zsv1_command command)
{
	return command == ZSV1_COMMAND_SHOW || command == ZSV1_COMMAND_START ||
	       command == ZSV1_COMMAND_STOP || command == ZSV1_COMMAND_RESTART;
}

int
zsv1_request_parse(const void *data, size_t length,
		   struct zsv1_request *request)
{
	char line[ZSV1_REQUEST_MAX + 1U], *fields[4];
	struct zsv1_request parsed;
	size_t count;

	if (request == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (copy_wire_line(data, length, ZSV1_REQUEST_MAX, line) != 0)
		return -1;
	if (split_fields(line, fields, sizeof(fields) / sizeof(fields[0]),
			 &count) != 0)
		return -1;
	if (count == 0 || strcmp(fields[0], "ZSV1") != 0) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	if (count < 2 || parse_command(fields[1], &parsed.command) != 0)
		return -1;
	if (count != (size_t)(command_has_service(parsed.command) ? 3 : 2)) {
		errno = EINVAL;
		return -1;
	}
	parsed.service[0] = '\0';
	if (command_has_service(parsed.command)) {
		if (!zsv1_name_valid(fields[2])) {
			errno = EINVAL;
			return -1;
		}
		strcpy(parsed.service, fields[2]);
	}
	*request = parsed;
	return 0;
}

int
zsv1_request_format(const struct zsv1_request *request, char *output,
		    size_t capacity, size_t *length)
{
	const char *command;
	int result;

	if (request == NULL || output == NULL || capacity == 0) {
		errno = EINVAL;
		return -1;
	}
	command = zsv1_command_name(request->command);
	if (command == NULL ||
	    (command_has_service(request->command) &&
	     !zsv1_name_valid(request->service)) ||
	    (!command_has_service(request->command) && request->service[0])) {
		errno = EINVAL;
		return -1;
	}
	if (command_has_service(request->command))
		result = snprintf(output, capacity, "ZSV1 %s %s\n", command,
				  request->service);
	else
		result = snprintf(output, capacity, "ZSV1 %s\n", command);
	if (result < 0 || (size_t)result >= capacity ||
	    (size_t)result > ZSV1_REQUEST_MAX) {
		errno = EMSGSIZE;
		return -1;
	}
	if (length != NULL)
		*length = (size_t)result;
	return 0;
}

static int
parse_decimal(const char *text, uint64_t maximum, uint64_t *result)
{
	uint64_t value = 0;
	size_t index;

	if (text == NULL || text[0] < '0' || text[0] > '9')
		goto invalid;
	for (index = 0; text[index] != '\0'; index++) {
		unsigned int digit;

		if (text[index] < '0' || text[index] > '9')
			goto invalid;
		digit = (unsigned int)(text[index] - '0');
		if (value > (maximum - digit) / 10U)
			goto invalid;
		value = value * 10U + digit;
	}
	*result = value;
	return 0;

invalid:
	errno = EINVAL;
	return -1;
}

int
zsv1_record_parse(const void *data, size_t length, struct zsv1_record *record)
{
	char line[ZSV1_RESPONSE_LINE_MAX + 1U], *fields[8];
	struct zsv1_record parsed;
	uint64_t number;
	size_t count;

	if (record == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (copy_wire_line(data, length, ZSV1_RESPONSE_LINE_MAX, line) != 0)
		return -1;
	if (split_fields(line, fields, sizeof(fields) / sizeof(fields[0]),
			 &count) != 0)
		return -1;
	if (count == 0 || strcmp(fields[0], "ZSV1") != 0) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	memset(&parsed, 0, sizeof(parsed));
	if (count == 6 && strcmp(fields[1], "SERVICE") == 0) {
		parsed.type = ZSV1_RECORD_SERVICE;
		if (!zsv1_name_valid(fields[2]) ||
		    parse_state(fields[3], &parsed.service.state) != 0 ||
		    (strcmp(fields[4], "0") != 0 &&
		     strcmp(fields[4], "1") != 0) ||
		    parse_decimal(fields[5], INT_MAX, &number) != 0)
			return -1;
		strcpy(parsed.service.name, fields[2]);
		parsed.service.enabled = fields[4][0] == '1';
		parsed.service.pid = (pid_t)number;
	} else if (count == 3 && strcmp(fields[1], "AFTER") == 0) {
		parsed.type = ZSV1_RECORD_AFTER;
		if (!zsv1_name_valid(fields[2]))
			goto invalid;
		strcpy(parsed.name, fields[2]);
	} else if (count == 3 && strcmp(fields[1], "REQUIRES") == 0) {
		parsed.type = ZSV1_RECORD_REQUIRES;
		if (!zsv1_name_valid(fields[2]))
			goto invalid;
		strcpy(parsed.name, fields[2]);
	} else if (count == 3 && strcmp(fields[1], "OK") == 0) {
		parsed.type = ZSV1_RECORD_OK;
		if (!token_valid(fields[2]))
			goto invalid;
		strcpy(parsed.token, fields[2]);
	} else if (count == 4 && strcmp(fields[1], "ERROR") == 0) {
		parsed.type = ZSV1_RECORD_ERROR;
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
	return 0;

invalid:
	errno = EINVAL;
	return -1;
}

static int
format_result(size_t capacity, size_t *length, int result)
{
	if (result < 0 || (size_t)result >= capacity ||
	    (size_t)result > ZSV1_RESPONSE_LINE_MAX) {
		errno = EMSGSIZE;
		return -1;
	}
	if (length != NULL)
		*length = (size_t)result;
	return 0;
}

int
zsv1_record_format(const struct zsv1_record *record, char *output,
		   size_t capacity, size_t *length)
{
	const char *state;
	int result;

	if (record == NULL || output == NULL || capacity == 0) {
		errno = EINVAL;
		return -1;
	}
	switch (record->type) {
	case ZSV1_RECORD_SERVICE:
		state = zsv1_state_name(record->service.state);
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
		if (!zsv1_name_valid(record->name))
			goto invalid;
		result = snprintf(
		    output, capacity, "ZSV1 %s %s\n",
		    record->type == ZSV1_RECORD_AFTER ? "AFTER" : "REQUIRES",
		    record->name);
		break;
	case ZSV1_RECORD_OK:
		if (!token_valid(record->token))
			goto invalid;
		result =
		    snprintf(output, capacity, "ZSV1 OK %s\n", record->token);
		break;
	case ZSV1_RECORD_ERROR:
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
	return format_result(capacity, length, result);

invalid:
	errno = EINVAL;
	return -1;
}

void
zsv1_decoder_init(struct zsv1_decoder *decoder)
{
	if (decoder != NULL)
		memset(decoder, 0, sizeof(*decoder));
}

static int
decoder_failure(struct zsv1_decoder *decoder, int error)
{
	decoder->failed = 1;
	errno = error;
	return -1;
}

static int
service_duplicate(const struct zsv1_response *response, const char *name)
{
	size_t index;

	for (index = 0; index < response->service_count; index++) {
		if (strcmp(response->services[index].name, name) == 0)
			return 1;
	}
	return 0;
}

static int
dependency_duplicate(const struct zsv1_response *response,
		     enum zsv1_dependency_type type, const char *name)
{
	size_t index;

	for (index = 0; index < response->dependency_count; index++) {
		if (response->dependencies[index].type == type &&
		    strcmp(response->dependencies[index].name, name) == 0)
			return 1;
	}
	return 0;
}

static int
decoder_record(struct zsv1_decoder *decoder, const struct zsv1_record *record)
{
	struct zsv1_response *response = &decoder->response;

	if (response->error_present || response->ok_present) {
		if (record->type == ZSV1_RECORD_END) {
			response->ended = 1;
			return 0;
		}
		return decoder_failure(decoder, EINVAL);
	}
	switch (record->type) {
	case ZSV1_RECORD_SERVICE:
		if (response->service_count == ZSV1_SERVICE_MAX ||
		    service_duplicate(response, record->service.name))
			return decoder_failure(decoder, EINVAL);
		response->services[response->service_count++] = record->service;
		return 0;
	case ZSV1_RECORD_AFTER:
	case ZSV1_RECORD_REQUIRES: {
		enum zsv1_dependency_type type =
		    record->type == ZSV1_RECORD_AFTER
			? ZSV1_DEPENDENCY_AFTER
			: ZSV1_DEPENDENCY_REQUIRES;
		struct zsv1_dependency_record *dependency;

		if (response->dependency_count == ZSV1_DEPENDENCY_MAX ||
		    dependency_duplicate(response, type, record->name))
			return decoder_failure(decoder, EINVAL);
		dependency =
		    &response->dependencies[response->dependency_count++];
		dependency->type = type;
		strcpy(dependency->name, record->name);
		return 0;
	}
	case ZSV1_RECORD_OK:
		response->ok_present = 1;
		strcpy(response->ok_token, record->token);
		return 0;
	case ZSV1_RECORD_ERROR:
		if (response->service_count != 0 ||
		    response->dependency_count != 0)
			return decoder_failure(decoder, EINVAL);
		response->error_present = 1;
		response->error_number = record->error_number;
		strcpy(response->error_reason, record->token);
		return 0;
	case ZSV1_RECORD_END:
		response->ended = 1;
		return 0;
	default:
		return decoder_failure(decoder, EINVAL);
	}
}

int
zsv1_decoder_feed(struct zsv1_decoder *decoder, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t index;

	if (decoder == NULL || (data == NULL && length != 0)) {
		errno = EINVAL;
		return -1;
	}
	if (decoder->failed)
		return decoder_failure(decoder, EINVAL);
	for (index = 0; index < length; index++) {
		struct zsv1_record record;
		unsigned char byte = bytes[index];

		if (decoder->response.ended)
			return decoder_failure(decoder, EINVAL);
		if (byte != '\n' && (byte < 0x20 || byte > 0x7e))
			return decoder_failure(decoder, EINVAL);
		if (decoder->line_length == ZSV1_RESPONSE_LINE_MAX)
			return decoder_failure(decoder, EMSGSIZE);
		decoder->line[decoder->line_length++] = (char)byte;
		if (byte != '\n')
			continue;
		decoder->line[decoder->line_length] = '\0';
		if (zsv1_record_parse(decoder->line, decoder->line_length,
				      &record) != 0)
			return decoder_failure(decoder, errno);
		decoder->line_length = 0;
		if (decoder_record(decoder, &record) != 0)
			return -1;
	}
	return 0;
}

int
zsv1_decoder_finish(struct zsv1_decoder *decoder)
{
	if (decoder == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (decoder->failed || decoder->line_length != 0 ||
	    !decoder->response.ended)
		return decoder_failure(decoder, EINVAL);
	return 0;
}

const struct zsv1_response *
zsv1_decoder_response(const struct zsv1_decoder *decoder)
{
	if (decoder == NULL || decoder->failed || decoder->line_length != 0 ||
	    !decoder->response.ended)
		return NULL;
	return &decoder->response;
}
