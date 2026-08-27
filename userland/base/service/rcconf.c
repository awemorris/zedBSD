/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static size_t
bounded_length(const char *text, size_t capacity)
{
	size_t length;

	if (text == NULL)
		return capacity;
	for (length = 0; length < capacity && text[length] != '\0'; length++)
		;
	return length;
}

static int
setting_allowed(const char *service, const char *setting)
{
	return strcmp(service, "ntpdate") == 0 &&
	       strcmp(setting, "servers") == 0;
}

static int
plain_character(int character)
{
	return isalnum((unsigned char)character) || character == '.' ||
	       character == '_' || character == '-' || character == '/' ||
	       character == ':';
}

static int
decimal_string(const char *text)
{
	size_t index;

	if (*text == '\0')
		return 0;
	for (index = 0; text[index] != '\0'; index++) {
		if (!isdigit((unsigned char)text[index]))
			return 0;
	}
	return 1;
}

static int
plain_string(const char *text)
{
	size_t index;

	if (*text == '\0' || strcmp(text, "true") == 0 ||
	    strcmp(text, "false") == 0 || decimal_string(text))
		return 0;
	for (index = 0; text[index] != '\0'; index++) {
		if (!plain_character((unsigned char)text[index]))
			return 0;
	}
	return 1;
}

static int
string_representable(const char *text, size_t capacity)
{
	size_t index, length = bounded_length(text, capacity);
	int single_quote = 0, double_quote = 0;

	if (length == capacity)
		return 0;
	for (index = 0; index < length; index++) {
		unsigned char character = (unsigned char)text[index];

		if (character < 0x20 || character > 0x7e)
			return 0;
		if (character == '\'')
			single_quote = 1;
		if (character == '"')
			double_quote = 1;
	}
	return plain_string(text) || !single_quote || !double_quote;
}

static int
copy_string(char *destination, size_t capacity, const char *source)
{
	size_t length = bounded_length(source, capacity);

	if (length == capacity) {
		errno = EOVERFLOW;
		return -1;
	}
	memcpy(destination, source, length + 1);
	return 0;
}

static int
parse_scalar(char *text, struct scalar *scalar)
{
	size_t index, length;
	uint64_t number = 0;

	if (text == NULL || scalar == NULL || *text == '\0')
		goto invalid;
	length = strlen(text);
	if (text[0] == '\'' || text[0] == '"') {
		int quote = (unsigned char)text[0];

		if (length < 2 || text[length - 1] != quote)
			goto invalid;
		text[length - 1] = '\0';
		text++;
		for (index = 0; text[index] != '\0'; index++) {
			unsigned char character = (unsigned char)text[index];

			if (character < 0x20 || character > 0x7e ||
			    character == quote)
				goto invalid;
		}
		scalar->kind = SCALAR_STRING;
		scalar->string = text;
		return 0;
	}
	for (index = 0; text[index] != '\0'; index++) {
		if (!plain_character((unsigned char)text[index]))
			goto invalid;
	}
	if (strcmp(text, "true") == 0 || strcmp(text, "false") == 0) {
		scalar->kind = SCALAR_BOOLEAN;
		scalar->boolean = text[0] == 't';
		return 0;
	}
	if (decimal_string(text)) {
		for (index = 0; text[index] != '\0'; index++) {
			unsigned int digit = (unsigned int)(text[index] - '0');

			if (number > (UINT64_MAX - digit) / 10)
				goto invalid;
			number = number * 10 + digit;
		}
		scalar->kind = SCALAR_INTEGER;
		scalar->integer = number;
		return 0;
	}
	scalar->kind = SCALAR_STRING;
	scalar->string = text;
	return 0;

invalid:
	errno = EINVAL;
	return -1;
}

static int
read_yaml_line(FILE *stream, char *line, size_t capacity)
{
	size_t length = 0;
	int character;

	for (;;) {
		character = fgetc(stream);
		if (character == EOF) {
			if (ferror(stream)) {
				errno = EIO;
				return -1;
			}
			if (length == 0)
				return 0;
			break;
		}
		if (character == '\n')
			break;
		if (character == '\0' || character < 0x20 || character > 0x7e) {
			if (character != '\r') {
				errno = EINVAL;
				return -1;
			}
		}
		if (length + 1 >= capacity) {
			errno = EOVERFLOW;
			return -1;
		}
		line[length++] = (char)character;
	}
	if (length != 0 && line[length - 1] == '\r')
		length--;
	line[length] = '\0';
	if (strchr(line, '\r') != NULL) {
		errno = EINVAL;
		return -1;
	}
	return 1;
}

static struct rcconf_service *
find_service(struct rcconf_model *model, const char *name)
{
	size_t index;

	for (index = 0; index < model->service_count; index++) {
		if (strcmp(model->services[index].name, name) == 0)
			return &model->services[index];
	}
	return NULL;
}

static const struct rcconf_service *
find_service_const(const struct rcconf_model *model, const char *name)
{
	size_t index;

	for (index = 0; index < model->service_count; index++) {
		if (strcmp(model->services[index].name, name) == 0)
			return &model->services[index];
	}
	return NULL;
}

static struct rcconf_setting *
find_setting(struct rcconf_service *service, const char *name)
{
	size_t index;

	for (index = 0; index < service->setting_count; index++) {
		if (strcmp(service->settings[index].name, name) == 0)
			return &service->settings[index];
	}
	return NULL;
}

static const struct rcconf_setting *
find_setting_const(const struct rcconf_service *service, const char *name)
{
	size_t index;

	for (index = 0; index < service->setting_count; index++) {
		if (strcmp(service->settings[index].name, name) == 0)
			return &service->settings[index];
	}
	return NULL;
}

void
rcconf_model_init(struct rcconf_model *model)
{
	if (model == NULL)
		return;
	memset(model, 0, sizeof(*model));
	model->version = RCCONF_VERSION;
}

int
rcconf_model_validate(const struct rcconf_model *model)
{
	size_t service_index, other_service, setting_index, other_setting;

	if (model == NULL || model->version != RCCONF_VERSION ||
	    model->service_count > RCCONF_SERVICE_MAX ||
	    model->hostname[0] == '\0' ||
	    !string_representable(model->hostname, sizeof(model->hostname)))
		goto invalid;
	for (service_index = 0; service_index < model->service_count;
	     service_index++) {
		const struct rcconf_service *service =
		    &model->services[service_index];

		if (!service_name_valid(service->name) ||
		    service->enabled < 0 || service->enabled > 1 ||
		    service->setting_count > RCCONF_SETTING_MAX)
			goto invalid;
		for (other_service = 0; other_service < service_index;
		     other_service++) {
			if (strcmp(model->services[other_service].name,
				   service->name) == 0)
				goto invalid;
		}
		for (setting_index = 0; setting_index < service->setting_count;
		     setting_index++) {
			const struct rcconf_setting *setting =
			    &service->settings[setting_index];

			if (!service_name_valid(setting->name) ||
			    !setting_allowed(service->name, setting->name) ||
			    !string_representable(setting->value,
						  sizeof(setting->value)))
				goto invalid;
			for (other_setting = 0; other_setting < setting_index;
			     other_setting++) {
				if (strcmp(
					service->settings[other_setting].name,
					setting->name) == 0)
					goto invalid;
			}
		}
	}
	return 0;

invalid:
	errno = EINVAL;
	return -1;
}

static int
parse_stream(FILE *stream, struct rcconf_model *destination)
{
	struct rcconf_model *model;
	char line[RCCONF_LINE_CAPACITY];
	unsigned char enabled_seen[RCCONF_SERVICE_MAX] = {0};
	unsigned char settings_seen[RCCONF_SERVICE_MAX] = {0};
	size_t current_service = SIZE_MAX;
	int seen_version = 0, seen_hostname = 0, seen_services = 0;
	int in_services = 0, in_settings = 0, failed = 0;
	int line_result;

	model = malloc(sizeof(*model));
	if (model == NULL)
		return -1;
	rcconf_model_init(model);
	while ((line_result = read_yaml_line(stream, line, sizeof(line))) > 0) {
		struct rcconf_service *service;
		struct scalar scalar = {0};
		char *content, *colon, *value = NULL;
		size_t indent = 0;
		int mapping;

		if (strchr(line, '\t') != NULL) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		while (line[indent] == ' ')
			indent++;
		content = line + indent;
		if (*content == '\0' || *content == '#')
			continue;
		if ((indent & 1U) != 0 || indent > 6) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		colon = strchr(content, ':');
		if (colon == NULL || colon == content) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		*colon = '\0';
		mapping = colon[1] == '\0';
		if (!mapping) {
			if (colon[1] != ' ' || colon[2] == '\0' ||
			    colon[2] == ' ') {
				errno = EINVAL;
				failed = 1;
				break;
			}
			value = colon + 2;
			if (parse_scalar(value, &scalar) != 0) {
				failed = 1;
				break;
			}
		}
		if (indent == 0) {
			current_service = SIZE_MAX;
			in_services = 0;
			in_settings = 0;
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
		if (!in_services) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		if (indent == 2) {
			in_settings = 0;
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
		if (current_service == SIZE_MAX) {
			errno = EINVAL;
			failed = 1;
			break;
		}
		service = &model->services[current_service];
		if (indent == 4) {
			in_settings = 0;
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
		if (!in_settings || mapping || scalar.kind != SCALAR_STRING ||
		    !service_name_valid(content) ||
		    !setting_allowed(service->name, content) ||
		    find_setting(service, content) != NULL ||
		    service->setting_count == RCCONF_SETTING_MAX) {
			errno = EINVAL;
			failed = 1;
			break;
		}
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
	if (line_result < 0) {
		failed = 1;
	}
	if (!failed && (!seen_version || !seen_hostname || !seen_services)) {
		errno = EINVAL;
		failed = 1;
	}
	if (!failed) {
		size_t index;

		for (index = 0; index < model->service_count; index++) {
			if (!enabled_seen[index]) {
				errno = EINVAL;
				failed = 1;
				break;
			}
		}
	}
	if (!failed && rcconf_model_validate(model) != 0)
		failed = 1;
	if (failed) {
		free(model);
		return -1;
	}
	*destination = *model;
	free(model);
	return 0;
}

int
rcconf_load(const char *path, struct rcconf_model *model)
{
	struct rcconf_model *parsed;
	FILE *stream;
	int result, saved_errno;

	if (path == NULL || model == NULL) {
		errno = EINVAL;
		return -1;
	}
	stream = fopen(path, "r");
	if (stream == NULL)
		return -1;
	parsed = malloc(sizeof(*parsed));
	if (parsed == NULL) {
		saved_errno = errno;
		(void)fclose(stream);
		errno = saved_errno;
		return -1;
	}
	result = parse_stream(stream, parsed);
	saved_errno = errno;
	if (fclose(stream) != 0 && result == 0) {
		result = -1;
		saved_errno = errno;
	}
	if (result == 0)
		*model = *parsed;
	else
		errno = saved_errno;
	free(parsed);
	return result;
}

static int
emit_text(emit_function_t emit, void *argument, const char *text)
{
	return emit(argument, text, strlen(text));
}

static int
emit_scalar(emit_function_t emit, void *argument, const char *text)
{
	char quote;

	if (plain_string(text))
		return emit_text(emit, argument, text);
	quote = strchr(text, '"') == NULL ? '"' : '\'';
	if (emit(argument, &quote, 1) != 0 ||
	    emit_text(emit, argument, text) != 0 ||
	    emit(argument, &quote, 1) != 0)
		return -1;
	return 0;
}

static void
sort_services(const struct rcconf_model *model, size_t *order)
{
	size_t index, position;

	for (index = 0; index < model->service_count; index++) {
		order[index] = index;
		for (position = index; position > 0; position--) {
			size_t left = order[position - 1];
			size_t right = order[position];

			if (strcmp(model->services[left].name,
				   model->services[right].name) <= 0)
				break;
			order[position - 1] = right;
			order[position] = left;
		}
	}
}

static void
sort_settings(const struct rcconf_service *service, size_t *order)
{
	size_t index, position;

	for (index = 0; index < service->setting_count; index++) {
		order[index] = index;
		for (position = index; position > 0; position--) {
			size_t left = order[position - 1];
			size_t right = order[position];

			if (strcmp(service->settings[left].name,
				   service->settings[right].name) <= 0)
				break;
			order[position - 1] = right;
			order[position] = left;
		}
	}
}

static int
emit_model(emit_function_t emit, void *argument,
	   const struct rcconf_model *model)
{
	size_t service_order[RCCONF_SERVICE_MAX];
	size_t service_position;

	if (rcconf_model_validate(model) != 0)
		return -1;
	if (emit_text(emit, argument, "version: 1\nhostname: ") != 0 ||
	    emit_scalar(emit, argument, model->hostname) != 0 ||
	    emit_text(emit, argument, "\n\nservices:\n") != 0)
		return -1;
	sort_services(model, service_order);
	for (service_position = 0; service_position < model->service_count;
	     service_position++) {
		const struct rcconf_service *service =
		    &model->services[service_order[service_position]];
		size_t setting_order[RCCONF_SETTING_MAX];
		size_t setting_position;

		if (emit_text(emit, argument, "  ") != 0 ||
		    emit_text(emit, argument, service->name) != 0 ||
		    emit_text(emit, argument,
			      service->enabled
				  ? ":\n    enabled: true\n"
				  : ":\n    enabled: false\n") != 0)
			return -1;
		if (service->setting_count == 0)
			continue;
		if (emit_text(emit, argument, "    settings:\n") != 0)
			return -1;
		sort_settings(service, setting_order);
		for (setting_position = 0;
		     setting_position < service->setting_count;
		     setting_position++) {
			const struct rcconf_setting *setting =
			    &service->settings[setting_order[setting_position]];

			if (emit_text(emit, argument, "      ") != 0 ||
			    emit_text(emit, argument, setting->name) != 0 ||
			    emit_text(emit, argument, ": ") != 0 ||
			    emit_scalar(emit, argument, setting->value) != 0 ||
			    emit_text(emit, argument, "\n") != 0)
				return -1;
		}
	}
	return 0;
}

static int
emit_file(void *argument, const char *data, size_t length)
{
	FILE *stream = argument;

	if (length != 0 && fwrite(data, 1, length, stream) != length) {
		if (errno == 0)
			errno = EIO;
		return -1;
	}
	return 0;
}

int
rcconf_write(FILE *stream, const struct rcconf_model *model)
{
	if (stream == NULL || model == NULL) {
		errno = EINVAL;
		return -1;
	}
	return emit_model(emit_file, stream, model);
}

int
rcconf_service_enabled(const struct rcconf_model *model, const char *name,
		       int *enabled)
{
	const struct rcconf_service *service;

	if (model == NULL || !service_name_valid(name) || enabled == NULL) {
		errno = EINVAL;
		return -1;
	}
	service = find_service_const(model, name);
	if (service == NULL) {
		errno = ENOENT;
		return -1;
	}
	*enabled = service->enabled;
	return 0;
}

int
rcconf_setting_get(const struct rcconf_model *model, const char *service_name,
		   const char *setting_name, char *output, size_t capacity)
{
	const struct rcconf_service *service;
	const struct rcconf_setting *setting;

	if (model == NULL || !service_name_valid(service_name) ||
	    !service_name_valid(setting_name) || output == NULL ||
	    capacity == 0) {
		errno = EINVAL;
		return -1;
	}
	service = find_service_const(model, service_name);
	setting =
	    service != NULL ? find_setting_const(service, setting_name) : NULL;
	if (setting == NULL) {
		errno = ENOENT;
		return -1;
	}
	return copy_string(output, capacity, setting->value);
}

int
rcconf_model_set_enabled(struct rcconf_model *model, const char *name,
			 int enabled)
{
	struct rcconf_service *service;

	if (model == NULL || !service_name_valid(name) ||
	    (enabled != 0 && enabled != 1)) {
		errno = EINVAL;
		return -1;
	}
	service = find_service(model, name);
	if (service == NULL) {
		if (model->service_count == RCCONF_SERVICE_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
		service = &model->services[model->service_count++];
		memset(service, 0, sizeof(*service));
		strcpy(service->name, name);
	}
	service->enabled = enabled;
	return 0;
}

int
rcconf_model_set_setting(struct rcconf_model *model, const char *service_name,
			 const char *setting_name, const char *value)
{
	struct rcconf_service *service;
	struct rcconf_setting *setting;

	if (model == NULL || !service_name_valid(service_name) ||
	    !service_name_valid(setting_name) ||
	    !setting_allowed(service_name, setting_name) ||
	    !string_representable(value, RCCONF_SETTING_VALUE_CAPACITY)) {
		errno = EINVAL;
		return -1;
	}
	service = find_service(model, service_name);
	if (service == NULL) {
		if (model->service_count == RCCONF_SERVICE_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
		service = &model->services[model->service_count++];
		memset(service, 0, sizeof(*service));
		strcpy(service->name, service_name);
	}
	setting = find_setting(service, setting_name);
	if (setting == NULL) {
		if (service->setting_count == RCCONF_SETTING_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
		setting = &service->settings[service->setting_count++];
		memset(setting, 0, sizeof(*setting));
		strcpy(setting->name, setting_name);
	}
	strcpy(setting->value, value);
	return 0;
}

static int
lock_configuration(const char *path)
{
	struct flock lock;
	char lock_path[RCCONF_PATH_CAPACITY];
	int descriptor;

	if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >=
	    (int)sizeof(lock_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	descriptor = open(lock_path, O_RDWR | O_CREAT, 0600);
	if (descriptor < 0)
		return -1;
	if (fchmod(descriptor, 0600) != 0) {
		int saved_errno = errno;

		close(descriptor);
		errno = saved_errno;
		return -1;
	}
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	if (fcntl(descriptor, F_SETLKW, &lock) != 0) {
		int saved_errno = errno;

		close(descriptor);
		errno = saved_errno;
		return -1;
	}
	return descriptor;
}

static int
open_temporary(const char *path, char *temporary, size_t capacity)
{
	unsigned int attempt;

	for (attempt = 0; attempt < 100; attempt++) {
		int descriptor;

		if (snprintf(temporary, capacity, "%s.tmp.%ld.%u", path,
			     (long)getpid(), attempt) >= (int)capacity) {
			errno = ENAMETOOLONG;
			return -1;
		}
		descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (descriptor >= 0)
			return descriptor;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}

static int
emit_descriptor(void *argument, const char *data, size_t length)
{
	int descriptor = *(int *)argument;
	size_t offset = 0;

	while (offset < length) {
		ssize_t result =
		    RCCONF_WRITE(descriptor, data + offset, length - offset);

		if (result > 0) {
			offset += (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		if (result == 0)
			errno = EIO;
		return -1;
	}
	return 0;
}

int
rcconf_update(const char *path, rcconf_mutator_t mutator, void *argument)
{
	struct rcconf_model *model = NULL;
	char temporary[RCCONF_PATH_CAPACITY];
	int lock_descriptor = -1, output_descriptor = -1;
	int failed = 0, temporary_exists = 0, saved_errno = 0;

	if (path == NULL || mutator == NULL) {
		errno = EINVAL;
		return -1;
	}
	lock_descriptor = lock_configuration(path);
	if (lock_descriptor < 0)
		return -1;
	model = malloc(sizeof(*model));
	if (model == NULL || rcconf_load(path, model) != 0 ||
	    mutator(model, argument) != 0 ||
	    rcconf_model_validate(model) != 0) {
		failed = 1;
		saved_errno = errno;
		goto done;
	}
	output_descriptor = open_temporary(path, temporary, sizeof(temporary));
	if (output_descriptor < 0) {
		failed = 1;
		saved_errno = errno;
		goto done;
	}
	temporary_exists = 1;
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
	if (close(output_descriptor) != 0) {
		failed = 1;
		saved_errno = errno;
		output_descriptor = -1;
		goto done;
	}
	output_descriptor = -1;
	if (RCCONF_RENAME(temporary, path) != 0) {
		failed = 1;
		saved_errno = errno;
		goto done;
	}
	temporary_exists = 0;

done:
	if (output_descriptor >= 0)
		(void)close(output_descriptor);
	if (temporary_exists)
		(void)RCCONF_UNLINK(temporary);
	free(model);
	(void)close(lock_descriptor);
	if (failed) {
		errno = saved_errno != 0 ? saved_errno : EIO;
		return -1;
	}
	return 0;
}

struct set_enabled_argument {
	const char *service;
	int enabled;
};

static int
set_enabled_mutator(struct rcconf_model *model, void *opaque)
{
	struct set_enabled_argument *argument = opaque;

	return rcconf_model_set_enabled(model, argument->service,
					argument->enabled);
}

int
rcconf_set_enabled(const char *path, const char *service, int enabled)
{
	struct set_enabled_argument argument;

	if (!service_name_valid(service) || (enabled != 0 && enabled != 1)) {
		errno = EINVAL;
		return -1;
	}
	argument.service = service;
	argument.enabled = enabled;
	return rcconf_update(path, set_enabled_mutator, &argument);
}
