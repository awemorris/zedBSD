/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland service rcconf support.
 */

#define _POSIX_C_SOURCE 200809L
#include "userland/base/service/rcconf.h"

#include "userland/base/service/service-config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef RCCONF_WRITE
#define RCCONF_WRITE write
#endif
#ifndef RCCONF_FSYNC
#define RCCONF_FSYNC fsync
#endif
#ifndef RCCONF_RENAME
#define RCCONF_RENAME rename
#endif
#ifndef RCCONF_UNLINK
#define RCCONF_UNLINK unlink
#endif

#define RCCONF_PATH_CAPACITY 4096

enum scalar_kind {
	SCALAR_STRING,
	SCALAR_BOOLEAN,
	SCALAR_INTEGER,
};

struct scalar {
	enum scalar_kind kind;
	const char *string;
	int boolean;
	uint64_t integer;
};

typedef int (*emit_function_t)(void *, const char *, size_t);

struct set_enabled_argument {
	const char *service;
	int enabled;
};

static int string_representable(const char *text, size_t capacity);
static size_t bounded_length(const char *text, size_t capacity);
static int plain_string(const char *text);
static int decimal_string(const char *text);
static int plain_character(int character);
static int setting_allowed(const char *service, const char *setting);
static int parse_stream(FILE *stream, struct rcconf_model *destination);
static int read_yaml_line(FILE *stream, char *line, size_t capacity);
static int parse_scalar(char *text, struct scalar *scalar);
static int copy_string(char *destination, size_t capacity, const char *source);
static struct rcconf_service *find_service(struct rcconf_model *model, const char *name);
static struct rcconf_setting *find_setting(struct rcconf_service *service, const char *name);
static int emit_model(emit_function_t emit, void *argument, const struct rcconf_model *model);
static int emit_text(emit_function_t emit, void *argument, const char *text);
static int emit_scalar(emit_function_t emit, void *argument, const char *text);
static void sort_services(const struct rcconf_model *model, size_t *order);
static void sort_settings(const struct rcconf_service *service, size_t *order);
static const struct rcconf_service *find_service_const(const struct rcconf_model *model, const char *name);
static const struct rcconf_setting *find_setting_const(const struct rcconf_service *service, const char *name);
static int lock_configuration(const char *path);
static int open_temporary(const char *path, char *temporary, size_t capacity);
static int emit_file(void *argument, const char *data, size_t length);
static int emit_descriptor(void *argument, const char *data, size_t length);
static int set_enabled_mutator(struct rcconf_model *model, void *opaque);

/*
 * Implements the rcconf model init operation.
 */
void
rcconf_model_init(
	struct rcconf_model *model)
{
	/* Handles the model availability. */
	if (model == NULL)
		return;
	memset(model, 0, sizeof(*model));
	model->version = RCCONF_VERSION;
}

/*
 * Implements the rcconf model validate operation.
 */
int
rcconf_model_validate(
	const struct rcconf_model *model)
{
	const struct rcconf_setting *setting;
	const struct rcconf_service *service;
	size_t service_index, other_service, setting_index, other_setting;

	/* Handles a failed string representable operation. */
	if (model == NULL || model->version != RCCONF_VERSION ||
	    model->service_count > RCCONF_SERVICE_MAX ||
	    model->hostname[0] == '\0' ||
	    !string_representable(model->hostname, sizeof(model->hostname)))
		goto invalid;

	/* Process each remaining element. */
	for (service_index = 0; service_index < model->service_count;
	     service_index++) {
				service = &model->services[service_index];

		/* Handles a failed service name valid operation. */
		if (!service_name_valid(service->name) ||
		    service->enabled < 0 || service->enabled > 1 ||
		    service->setting_count > RCCONF_SETTING_MAX)
			goto invalid;

		/* Process each remaining element. */
		for (other_service = 0; other_service < service_index;
		     other_service++) {
			/* Selects the matching value. */
			if (strcmp(model->services[other_service].name,
				   service->name) == 0)
				goto invalid;
		}

		/* Process each remaining element. */
		for (setting_index = 0; setting_index < service->setting_count;
		     setting_index++) {
						setting = &service->settings[setting_index];

			/* Handles a failed service name valid operation. */
			if (!service_name_valid(setting->name) ||
			    !setting_allowed(service->name, setting->name) ||
			    !string_representable(setting->value,
						  sizeof(setting->value)))
				goto invalid;

			/* Process each remaining element. */
			for (other_setting = 0; other_setting < setting_index;
			     other_setting++) {
				/* Selects the matching value. */
				if (strcmp(
					service->settings[other_setting].name,
					setting->name) == 0)
					goto invalid;
			}
		}
	}

	/* Reports successful completion. */
	return 0;

invalid:
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the rcconf load operation.
 */
int
rcconf_load(
	const char *path,
	struct rcconf_model *model)
{
	struct rcconf_model *parsed;
	FILE *stream;
	int result, saved_errno;

	/* Handles the path availability. */
	if (path == NULL || model == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	stream = fopen(path, "r");

	/* Handles the stream availability. */
	if (stream == NULL)
		return -1;
	parsed = malloc(sizeof(*parsed));

	/* Handles the parsed availability. */
	if (parsed == NULL) {
		saved_errno = errno;
		(void)fclose(stream);
		errno = saved_errno;

		/* Reports operation failure. */
		return -1;
	}
	result = parse_stream(stream, parsed);
	saved_errno = errno;

	/* Handles a failed fclose operation. */
	if (fclose(stream) != 0 && result == 0) {
		result = -1;
		saved_errno = errno;
	}

	/* Checks the operation result. */
	if (result == 0)
		*model = *parsed;
	else
		errno = saved_errno;
	free(parsed);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the rcconf write operation.
 */
int
rcconf_write(
	FILE *stream,
	const struct rcconf_model *model)
{
	int function_result;

	/* Handles the stream availability. */
	if (stream == NULL || model == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the emit model result. */
	function_result = emit_model(emit_file, stream, model);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the rcconf service enabled operation.
 */
int
rcconf_service_enabled(
	const struct rcconf_model *model,
	const char *name,
	int *enabled)
{
	const struct rcconf_service *service;

	/* Handles a failed service name valid operation. */
	if (model == NULL || !service_name_valid(name) || enabled == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	service = find_service_const(model, name);

	/* Handles the service availability. */
	if (service == NULL) {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}
	*enabled = service->enabled;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rcconf setting get operation.
 */
int
rcconf_setting_get(
	const struct rcconf_model *model,
	const char *service_name,
	const char *setting_name,
	char *output,
	size_t capacity)
{
	int function_result;
	const struct rcconf_service *service;
	const struct rcconf_setting *setting;

	/* Handles a failed service name valid operation. */
	if (model == NULL || !service_name_valid(service_name) ||
	    !service_name_valid(setting_name) || output == NULL ||
	    capacity == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	service = find_service_const(model, service_name);
	setting =
	    service != NULL ? find_setting_const(service, setting_name) : NULL;

	/* Handles the setting availability. */
	if (setting == NULL) {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the copy string result. */
	function_result = copy_string(output, capacity, setting->value);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the rcconf model set enabled operation.
 */
int
rcconf_model_set_enabled(
	struct rcconf_model *model,
	const char *name,
	int enabled)
{
	struct rcconf_service *service;

	/* Handles a failed service name valid operation. */
	if (model == NULL || !service_name_valid(name) ||
	    (enabled != 0 && enabled != 1)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	service = find_service(model, name);

	/* Handles the service availability. */
	if (service == NULL) {
		/* Handles the model condition. */
		if (model->service_count == RCCONF_SERVICE_MAX) {
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		service = &model->services[model->service_count++];
		memset(service, 0, sizeof(*service));
		strcpy(service->name, name);
	}
	service->enabled = enabled;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rcconf model set setting operation.
 */
int
rcconf_model_set_setting(
	struct rcconf_model *model,
	const char *service_name,
	const char *setting_name,
	const char *value)
{
	struct rcconf_service *service;
	struct rcconf_setting *setting;

	/* Handles a failed service name valid operation. */
	if (model == NULL || !service_name_valid(service_name) ||
	    !service_name_valid(setting_name) ||
	    !setting_allowed(service_name, setting_name) ||
	    !string_representable(value, RCCONF_SETTING_VALUE_CAPACITY)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	service = find_service(model, service_name);

	/* Handles the service availability. */
	if (service == NULL) {
		/* Handles the model condition. */
		if (model->service_count == RCCONF_SERVICE_MAX) {
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		service = &model->services[model->service_count++];
		memset(service, 0, sizeof(*service));
		strcpy(service->name, service_name);
	}
	setting = find_setting(service, setting_name);

	/* Handles the setting availability. */
	if (setting == NULL) {
		/* Handles the service condition. */
		if (service->setting_count == RCCONF_SETTING_MAX) {
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		setting = &service->settings[service->setting_count++];
		memset(setting, 0, sizeof(*setting));
		strcpy(setting->name, setting_name);
	}
	strcpy(setting->value, value);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rcconf update operation.
 */
int
rcconf_update(
	const char *path,
	rcconf_mutator_t mutator,
	void *argument)
{
	struct rcconf_model *model;
	char temporary[RCCONF_PATH_CAPACITY];
	int lock_descriptor, output_descriptor;
	int failed, temporary_exists, saved_errno;

	model = NULL;
	lock_descriptor = -1;
	output_descriptor = -1;
	failed = 0;
	temporary_exists = 0;
	saved_errno = 0;

	/* Handles the path availability. */
	if (path == NULL || mutator == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	lock_descriptor = lock_configuration(path);

	/* Handles the lock descriptor condition. */
	if (lock_descriptor < 0)
		return -1;
	model = malloc(sizeof(*model));

	/* Handles a failed rcconf load operation. */
	if (model == NULL || rcconf_load(path, model) != 0 ||
	    mutator(model, argument) != 0 ||
	    rcconf_model_validate(model) != 0) {
		failed = 1;
		saved_errno = errno;
		goto done;
	}
	output_descriptor = open_temporary(path, temporary, sizeof(temporary));

	/* Handles the output descriptor condition. */
	if (output_descriptor < 0) {
		failed = 1;
		saved_errno = errno;
		goto done;
	}
	temporary_exists = 1;

	/* Handles a failed fchmod operation. */
	if (fchmod(output_descriptor, 0644) != 0 ||
	    (geteuid() == 0 && fchown(output_descriptor, 0, 0) != 0) ||
	    emit_model(emit_descriptor, &output_descriptor, model) != 0 ||
	    RCCONF_FSYNC(output_descriptor) != 0) {
		failed = 1;
		saved_errno = errno;
		(void)close(output_descriptor);
		output_descriptor = -1;
		goto done;
	}

	/* Handles a failed close operation. */
	if (close(output_descriptor) != 0) {
		failed = 1;
		saved_errno = errno;
		output_descriptor = -1;
		goto done;
	}
	output_descriptor = -1;

	/* Handles a failed RCCONF RENAME operation. */
	if (RCCONF_RENAME(temporary, path) != 0) {
		failed = 1;
		saved_errno = errno;
		goto done;
	}
	temporary_exists = 0;

done:

	/* Handles the output descriptor condition. */
	if (output_descriptor >= 0)
		(void)close(output_descriptor);

	/* Handles the temporary exists condition. */
	if (temporary_exists)
		(void)RCCONF_UNLINK(temporary);
	free(model);
	(void)close(lock_descriptor);

	/* Handles an operation failure. */
	if (failed) {
		errno = saved_errno != 0 ? saved_errno : EIO;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rcconf set enabled operation.
 */
int
rcconf_set_enabled(
	const char *path,
	const char *service,
	int enabled)
{
	int function_result;
	struct set_enabled_argument argument;

	/* Handles a failed service name valid operation. */
	if (!service_name_valid(service) || (enabled != 0 && enabled != 1)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	argument.service = service;
	argument.enabled = enabled;

	/* Obtains the rcconf update result. */
	function_result = rcconf_update(path, set_enabled_mutator, &argument);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the string representable operation. */
static int
string_representable(
	const char *text,
	size_t capacity)
{
	int function_result;
	unsigned char character;
	size_t index, length;
	int single_quote, double_quote;

	length = bounded_length(text, capacity);
	single_quote = 0;
	double_quote = 0;

	/* Checks the current data length. */
	if (length == capacity)
		return 0;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
				character = (unsigned char)text[index];

		/* Classifies the current input character. */
		if (character < 0x20 || character > 0x7e)
			return 0;

		/* Classifies the current input character. */
		if (character == '\'')
			single_quote = 1;

		/* Classifies the current input character. */
		if (character == '"')
			double_quote = 1;
	}

	/* Computes the function result. */
	function_result = plain_string(text) || !single_quote || !double_quote;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the bounded length operation. */
static size_t
bounded_length(
	const char *text,
	size_t capacity)
{
	size_t length;

	/* Handles the text availability. */
	if (text == NULL)
		return capacity;

	/* Process each remaining element. */
	for (length = 0; length < capacity && text[length] != '\0'; length++)
		;

	/* Returns the computed result. */
	return length;
}

/* Supports the plain string operation. */
static int
plain_string(
	const char *text)
{
	size_t index;

	/* Handles a failed decimal string operation. */
	if (*text == '\0' || strcmp(text, "true") == 0 ||
	    strcmp(text, "false") == 0 || decimal_string(text))

		/* Reports successful completion. */
		return 0;

	/* Process each remaining element. */
	for (index = 0; text[index] != '\0'; index++) {
		/* Handles a failed plain character operation. */
		if (!plain_character((unsigned char)text[index]))
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the decimal string operation. */
static int
decimal_string(
	const char *text)
{
	size_t index;

	/* Validates the current text. */
	if (*text == '\0')
		return 0;

	/* Process each remaining element. */
	for (index = 0; text[index] != '\0'; index++) {
		/* Handles a failed isdigit operation. */
		if (!isdigit((unsigned char)text[index]))
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the plain character operation. */
static int
plain_character(
	int character)
{
	int function_result;

	/* Computes the function result. */
	function_result = isalnum((unsigned char)character) || character == '.' ||
	       character == '_' || character == '-' || character == '/' ||
	       character == ':';

	/* Returns the computed result. */
	return function_result;
}

/* Supports the setting allowed operation. */
static int
setting_allowed(
	const char *service,
	const char *setting)
{
	int function_result;

	/* Computes the function result. */
	function_result = strcmp(service, "ntpdate") == 0 &&
	       strcmp(setting, "servers") == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the parse stream operation. */
static int
parse_stream(
	FILE *stream,
	struct rcconf_model *destination)
{
	struct rcconf_service *service;
	char *content, *colon, *value;
	size_t indent;
	int mapping;
	size_t index;
	struct rcconf_model *model;
	char line[RCCONF_LINE_CAPACITY];
	unsigned char enabled_seen[RCCONF_SERVICE_MAX] = {0};
	unsigned char settings_seen[RCCONF_SERVICE_MAX] = {0};
	size_t current_service;
	int seen_version, seen_hostname, seen_services;
	int in_services, in_settings, failed;
	int line_result;
	struct scalar scalar;

	current_service = SIZE_MAX;
	seen_version = 0;
	seen_hostname = 0;
	seen_services = 0;
	in_services = 0;
	in_settings = 0;
	failed = 0;

	model = malloc(sizeof(*model));

	/* Handles the model availability. */
	if (model == NULL)
		return -1;
	rcconf_model_init(model);

	/* Process each remaining element. */
	while ((line_result = read_yaml_line(stream, line, sizeof(line))) > 0) {
		memset(&scalar, 0, sizeof(scalar));
				value = NULL;
				indent = 0;

		/* Handles a failed strchr operation. */
		if (strchr(line, '\t') != NULL) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		while (line[indent] == ' ')
			indent++;
		content = line + indent;

		/* Handles the content condition. */
		if (*content == '\0' || *content == '#')
			continue;

		/* Handles the indent condition. */
		if ((indent & 1U) != 0 || indent > 6) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		colon = strchr(content, ':');

		/* Handles the colon availability. */
		if (colon == NULL || colon == content) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		*colon = '\0';
		mapping = colon[1] == '\0';

		/* Handles the mapping condition. */
		if (!mapping) {
			/* Handles the colon condition. */
			if (colon[1] != ' ' || colon[2] == '\0' ||
			    colon[2] == ' ') {
				errno = EINVAL;
				failed = 1;
				break;
			}
			value = colon + 2;

			/* Handles a failed parse scalar operation. */
			if (parse_scalar(value, &scalar) != 0) {
				failed = 1;
				break;
			}
		}

		/* Handles the indent condition. */
		if (indent == 0) {
			current_service = SIZE_MAX;
			in_services = 0;
			in_settings = 0;

			/* Selects the matching value. */
			if (strcmp(content, "version") == 0 && !mapping &&
			    !seen_version && scalar.kind == SCALAR_INTEGER &&
			    scalar.integer == RCCONF_VERSION) {
				seen_version = 1;
			} else if (strcmp(content, "hostname") == 0 &&
				   !mapping && !seen_hostname &&
				   scalar.kind == SCALAR_STRING &&
				   copy_string(model->hostname,
					       sizeof(model->hostname),
					       scalar.string) == 0) {
				seen_hostname = 1;
			} else if (strcmp(content, "services") == 0 &&
				   mapping && !seen_services) {
				seen_services = 1;
				in_services = 1;
			} else {
				errno = EINVAL;
				failed = 1;
				break;
			}
			continue;
		}

		/* Handles the in services condition. */
		if (!in_services) {
			errno = EINVAL;
			failed = 1;
			break;
		}

		/* Handles the indent condition. */
		if (indent == 2) {
			in_settings = 0;

			/* Handles a failed service name valid operation. */
			if (!mapping || !service_name_valid(content) ||
			    find_service(model, content) != NULL ||
			    model->service_count == RCCONF_SERVICE_MAX) {
				errno = EINVAL;
				failed = 1;
				break;
			}
			current_service = model->service_count++;
			service = &model->services[current_service];
			memset(service, 0, sizeof(*service));
			strcpy(service->name, content);
			continue;
		}

		/* Handles the current service condition. */
		if (current_service == SIZE_MAX) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		service = &model->services[current_service];

		/* Handles the indent condition. */
		if (indent == 4) {
			in_settings = 0;

			/* Selects the matching value. */
			if (strcmp(content, "enabled") == 0 && !mapping &&
			    !enabled_seen[current_service] &&
			    scalar.kind == SCALAR_BOOLEAN) {
				service->enabled = scalar.boolean;
				enabled_seen[current_service] = 1;
			} else if (strcmp(content, "settings") == 0 &&
				   mapping && !settings_seen[current_service]) {
				in_settings = 1;
				settings_seen[current_service] = 1;
			} else {
				errno = EINVAL;
				failed = 1;
				break;
			}
			continue;
		}

		/* Handles a failed service name valid operation. */
		if (!in_settings || mapping || scalar.kind != SCALAR_STRING ||
		    !service_name_valid(content) ||
		    !setting_allowed(service->name, content) ||
		    find_setting(service, content) != NULL ||
		    service->setting_count == RCCONF_SETTING_MAX) {
			errno = EINVAL;
			failed = 1;
			break;
		}

		/* Handles a failed copy string operation. */
		if (copy_string(service->settings[service->setting_count].name,
				sizeof(service->settings[0].name),
				content) != 0 ||
		    copy_string(service->settings[service->setting_count].value,
				sizeof(service->settings[0].value),
				scalar.string) != 0) {
			failed = 1;
			break;
		}
		service->setting_count++;
	}

	/* Handles the line result condition. */
	if (line_result < 0) {
		failed = 1;
	}

	/* Handles an operation failure. */
	if (!failed && (!seen_version || !seen_hostname || !seen_services)) {
		errno = EINVAL;
		failed = 1;
	}

	/* Handles an operation failure. */
	if (!failed) {
		/* Process each remaining element. */
		for (index = 0; index < model->service_count; index++) {
			/* Handles the enabled seen condition. */
			if (!enabled_seen[index]) {
				errno = EINVAL;
				failed = 1;
				break;
			}
		}
	}

	/* Handles an operation failure. */
	if (!failed && rcconf_model_validate(model) != 0)
		failed = 1;

	/* Handles an operation failure. */
	if (failed) {
		free(model);

		/* Reports operation failure. */
		return -1;
	}
	*destination = *model;
	free(model);

	/* Reports successful completion. */
	return 0;
}

/* Supports the read yaml line operation. */
static int
read_yaml_line(
	FILE *stream,
	char *line,
	size_t capacity)
{
	size_t length;
	int character;

	length = 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		character = fgetc(stream);

		/* Handles the end-of-file condition. */
		if (character == EOF) {
			/* Handles an operation failure. */
			if (ferror(stream)) {
				errno = EIO;

				/* Reports operation failure. */
				return -1;
			}

			/* Checks the current data length. */
			if (length == 0)
				return 0;
			break;
		}

		/* Classifies the current input character. */
		if (character == '\n')
			break;

		/* Classifies the current input character. */
		if (character == '\0' || character < 0x20 || character > 0x7e) {
			/* Classifies the current input character. */
			if (character != '\r') {
				errno = EINVAL;

				/* Reports operation failure. */
				return -1;
			}
		}

		/* Checks the current data length. */
		if (length + 1 >= capacity) {
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		line[length++] = (char)character;
	}

	/* Checks the current data length. */
	if (length != 0 && line[length - 1] == '\r')
		length--;
	line[length] = '\0';

	/* Handles a failed strchr operation. */
	if (strchr(line, '\r') != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the parse scalar operation. */
static int
parse_scalar(
	char *text,
	struct scalar *scalar)
{
	unsigned char character;
	int quote;
	unsigned int digit;
	size_t index, length;
	uint64_t number;

	number = 0;

	/* Handles the text availability. */
	if (text == NULL || scalar == NULL || *text == '\0')
		goto invalid;
	length = strlen(text);

	/* Validates the current text. */
	if (text[0] == '\'' || text[0] == '"') {
				quote = (unsigned char)text[0];

		/* Checks the current data length. */
		if (length < 2 || text[length - 1] != quote)
			goto invalid;
		text[length - 1] = '\0';
		text++;

		/* Process each remaining element. */
		for (index = 0; text[index] != '\0'; index++) {
						character = (unsigned char)text[index];

			/* Classifies the current input character. */
			if (character < 0x20 || character > 0x7e ||
			    character == quote)
				goto invalid;
		}
		scalar->kind = SCALAR_STRING;
		scalar->string = text;

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (index = 0; text[index] != '\0'; index++) {
		/* Handles a failed plain character operation. */
		if (!plain_character((unsigned char)text[index]))
			goto invalid;
	}

	/* Selects the matching value. */
	if (strcmp(text, "true") == 0 || strcmp(text, "false") == 0) {
		scalar->kind = SCALAR_BOOLEAN;
		scalar->boolean = text[0] == 't';

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the decimal string condition. */
	if (decimal_string(text)) {
		/* Process each remaining element. */
		for (index = 0; text[index] != '\0'; index++) {
						digit = (unsigned int)(text[index] - '0');

			/* Handles the number condition. */
			if (number > (UINT64_MAX - digit) / 10)
				goto invalid;
			number = number * 10 + digit;
		}
		scalar->kind = SCALAR_INTEGER;
		scalar->integer = number;

		/* Reports successful completion. */
		return 0;
	}
	scalar->kind = SCALAR_STRING;
	scalar->string = text;

	/* Reports successful completion. */
	return 0;

invalid:
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/* Supports the copy string operation. */
static int
copy_string(
	char *destination,
	size_t capacity,
	const char *source)
{
	size_t length;

	length = bounded_length(source, capacity);

	/* Checks the current data length. */
	if (length == capacity) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	memcpy(destination, source, length + 1);

	/* Reports successful completion. */
	return 0;
}

/* Supports the find service operation. */
static struct rcconf_service *
find_service(
	struct rcconf_model *model,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < model->service_count; index++) {
		/* Selects the matching value. */
		if (strcmp(model->services[index].name, name) == 0)
			return &model->services[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the find setting operation. */
static struct rcconf_setting *
find_setting(
	struct rcconf_service *service,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < service->setting_count; index++) {
		/* Selects the matching value. */
		if (strcmp(service->settings[index].name, name) == 0)
			return &service->settings[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the emit model operation. */
static int
emit_model(
	emit_function_t emit,
	void *argument,
	const struct rcconf_model *model)
{
	const struct rcconf_setting *setting;
	const struct rcconf_service *service;
	size_t setting_order[RCCONF_SETTING_MAX];
	size_t setting_position;
	size_t service_order[RCCONF_SERVICE_MAX];
	size_t service_position;

	/* Handles a failed rcconf model validate operation. */
	if (rcconf_model_validate(model) != 0)
		return -1;

	/* Handles a failed emit text operation. */
	if (emit_text(emit, argument, "version: 1\nhostname: ") != 0 ||
	    emit_scalar(emit, argument, model->hostname) != 0 ||
	    emit_text(emit, argument, "\n\nservices:\n") != 0)

		/* Reports operation failure. */
		return -1;
	sort_services(model, service_order);

	/* Process each remaining element. */
	for (service_position = 0; service_position < model->service_count;
	     service_position++) {
				service = &model->services[service_order[service_position]];

		/* Handles a failed emit text operation. */
		if (emit_text(emit, argument, "  ") != 0 ||
		    emit_text(emit, argument, service->name) != 0 ||
		    emit_text(emit, argument,
			      service->enabled
				  ? ":\n    enabled: true\n"
				  : ":\n    enabled: false\n") != 0)

			/* Reports operation failure. */
			return -1;

		/* Handles the service condition. */
		if (service->setting_count == 0)
			continue;

		/* Handles a failed emit text operation. */
		if (emit_text(emit, argument, "    settings:\n") != 0)
			return -1;
		sort_settings(service, setting_order);

		/* Process each remaining element. */
		for (setting_position = 0;
		     setting_position < service->setting_count;
		     setting_position++) {
						setting = &service->settings[setting_order[setting_position]];

			/* Handles a failed emit text operation. */
			if (emit_text(emit, argument, "      ") != 0 ||
			    emit_text(emit, argument, setting->name) != 0 ||
			    emit_text(emit, argument, ": ") != 0 ||
			    emit_scalar(emit, argument, setting->value) != 0 ||
			    emit_text(emit, argument, "\n") != 0)

				/* Reports operation failure. */
				return -1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the emit text operation. */
static int
emit_text(
	emit_function_t emit,
	void *argument,
	const char *text)
{
	int function_result;

	/* Obtains the emit result. */
	function_result = emit(argument, text, strlen(text));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the emit scalar operation. */
static int
emit_scalar(
	emit_function_t emit,
	void *argument,
	const char *text)
{
	int function_result;
	char quote;

	/* Handles the plain string condition. */
	if (plain_string(text)) {
		/* Obtains the emit text result. */
		function_result = emit_text(emit, argument, text);

		/* Returns the computed result. */
		return function_result;
	}

	quote = strchr(text, '"') == NULL ? '"' : '\'';

	/* Handles a failed emit operation. */
	if (emit(argument, &quote, 1) != 0 ||
	    emit_text(emit, argument, text) != 0 ||
	    emit(argument, &quote, 1) != 0)

		/* Reports operation failure. */
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the sort services operation. */
static void
sort_services(
	const struct rcconf_model *model,
	size_t *order)
{
	size_t left;
	size_t right;
	size_t index, position;

	/* Process each remaining element. */
	for (index = 0; index < model->service_count; index++) {
		/* Process each remaining element. */
		order[index] = index;
		for (position = index; position > 0; position--) {
						left = order[position - 1];
						right = order[position];

			/* Selects the matching value. */
			if (strcmp(model->services[left].name,
				   model->services[right].name) <= 0)
				break;
			order[position - 1] = right;
			order[position] = left;
		}
	}
}

/* Supports the sort settings operation. */
static void
sort_settings(
	const struct rcconf_service *service,
	size_t *order)
{
	size_t left;
	size_t right;
	size_t index, position;

	/* Process each remaining element. */
	for (index = 0; index < service->setting_count; index++) {
		/* Process each remaining element. */
		order[index] = index;
		for (position = index; position > 0; position--) {
						left = order[position - 1];
						right = order[position];

			/* Selects the matching value. */
			if (strcmp(service->settings[left].name,
				   service->settings[right].name) <= 0)
				break;
			order[position - 1] = right;
			order[position] = left;
		}
	}
}

/* Supports the find service const operation. */
static const struct rcconf_service *
find_service_const(
	const struct rcconf_model *model,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < model->service_count; index++) {
		/* Selects the matching value. */
		if (strcmp(model->services[index].name, name) == 0)
			return &model->services[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the find setting const operation. */
static const struct rcconf_setting *
find_setting_const(
	const struct rcconf_service *service,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < service->setting_count; index++) {
		/* Selects the matching value. */
		if (strcmp(service->settings[index].name, name) == 0)
			return &service->settings[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the lock configuration operation. */
static int
lock_configuration(
	const char *path)
{
	int saved_errno_local;
	int saved_errno_local1;
	struct flock lock;
	char lock_path[RCCONF_PATH_CAPACITY];
	int descriptor;

	/* Handles a failed snprintf operation. */
	if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >=
	    (int)sizeof(lock_path)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	descriptor = open(lock_path, O_RDWR | O_CREAT, 0600);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles a failed fchmod operation. */
	if (fchmod(descriptor, 0600) != 0) {
				saved_errno_local = errno;

		close(descriptor);
		errno = saved_errno_local;

		/* Reports operation failure. */
		return -1;
	}
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;

	/* Handles a failed fcntl operation. */
	if (fcntl(descriptor, F_SETLKW, &lock) != 0) {
				saved_errno_local1 = errno;

		close(descriptor);
		errno = saved_errno_local1;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return descriptor;
}

/* Supports the open temporary operation. */
static int
open_temporary(
	const char *path,
	char *temporary,
	size_t capacity)
{
	int descriptor;
	unsigned int attempt;

	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 100; attempt++) {
		/* Handles a failed snprintf operation. */
		if (snprintf(temporary, capacity, "%s.tmp.%ld.%u", path,
			     (long)getpid(), attempt) >= (int)capacity) {
			errno = ENAMETOOLONG;

			/* Reports operation failure. */
			return -1;
		}
		descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);

		/* Checks the file descriptor. */
		if (descriptor >= 0)
			return descriptor;

		/* Handles the reported system error. */
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;

	/* Reports operation failure. */
	return -1;
}

/* Supports the emit file operation. */
static int
emit_file(
	void *argument,
	const char *data,
	size_t length)
{
	FILE *stream;

	stream = argument;

	/* Handles a failed fwrite operation. */
	if (length != 0 && fwrite(data, 1, length, stream) != length) {
		/* Handles the reported system error. */
		if (errno == 0)
			errno = EIO;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the emit descriptor operation. */
static int
emit_descriptor(
	void *argument,
	const char *data,
	size_t length)
{
	ssize_t result;
	int descriptor;
	size_t offset;

	descriptor = *(int *)argument;
	offset = 0;

	/* Process each remaining element. */
	while (offset < length) {

		result = RCCONF_WRITE(descriptor, data + offset, length - offset);

		/* Checks the operation result. */
		if (result > 0) {
			offset += (size_t)result;
			continue;
		}

		/* Handles the reported system error. */
		if (result < 0 && errno == EINTR)
			continue;

		/* Checks the operation result. */
		if (result == 0)
			errno = EIO;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the set enabled mutator operation. */
static int
set_enabled_mutator(
	struct rcconf_model *model,
	void *opaque)
{
	int function_result;
	struct set_enabled_argument *argument;

	argument = opaque;

	/* Obtains the rcconf model set enabled result. */
	function_result = rcconf_model_set_enabled(model, argument->service,
					argument->enabled);

	/* Returns the computed result. */
	return function_result;
}
