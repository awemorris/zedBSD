/* Bounded zedbsd.cfg parsing and boot-parameter assembly. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "zedbsd-config.h"

struct config_line {
	const unsigned char *text;
	size_t length;
	size_t equal;
};

struct line_iterator {
	const unsigned char *source;
	size_t size;
	size_t position;
	unsigned count;
};

struct parameter_builder {
	struct zedbsd_boot_parameter_record *record;
	size_t length;
};

static void
configuration_zero(struct zbl_uefi_zedbsd_config *configuration)
{
	unsigned char *bytes = (unsigned char *)configuration;

	for (size_t index = 0U; index < sizeof(*configuration); index++)
		bytes[index] = 0U;
}

static int
text_matches(const unsigned char *text, size_t length, const char *candidate)
{
	size_t candidate_length = 0U;

	while (candidate[candidate_length] != '\0')
		candidate_length++;
	if (candidate_length != length)
		return 0;
	for (size_t index = 0U; index < length; index++)
		if (text[index] != (unsigned char)candidate[index])
			return 0;
	return 1;
}

static int
text_starts_with(const unsigned char *text, size_t length,
		 const char *prefix)
{
	size_t prefix_length = 0U;

	while (prefix[prefix_length] != '\0')
		prefix_length++;
	if (length < prefix_length)
		return 0;
	for (size_t index = 0U; index < prefix_length; index++)
		if (text[index] != (unsigned char)prefix[index])
			return 0;
	return 1;
}

static int
unsupported_syntax(unsigned char byte)
{
	return byte == '"' || byte == '\'' || byte == '#' || byte == ';' ||
	    byte == '\\';
}

/* Returns OK with line->text == NULL at end of input. */
static enum zbl_uefi_zedbsd_config_result
next_line(struct line_iterator *iterator, struct config_line *line)
{
	size_t start;
	size_t end;
	size_t length;

	line->text = NULL;
	line->length = 0U;
	line->equal = 0U;
	if (iterator->position == iterator->size)
		return ZBL_UEFI_ZEDBSD_CONFIG_OK;
	start = iterator->position;
	end = start;
	while (end < iterator->size && iterator->source[end] != '\n')
		end++;
	iterator->count++;
	if (iterator->count > ZBL_ZEDBSD_CONFIG_LINE_COUNT_MAX)
		return ZBL_UEFI_ZEDBSD_CONFIG_TOO_MANY_LINES;
	length = end - start;
	if (end < iterator->size) {
		iterator->position = end + 1U;
		if (length != 0U && iterator->source[end - 1U] == '\r')
			length--;
	} else {
		iterator->position = end;
	}
	if (length > ZBL_ZEDBSD_CONFIG_LINE_MAX)
		return ZBL_UEFI_ZEDBSD_CONFIG_LINE_TOO_LONG;
	for (size_t index = 0U; index < length; index++) {
		unsigned char byte = iterator->source[start + index];

		if (byte == '\r')
			return ZBL_UEFI_ZEDBSD_CONFIG_INVALID_LINE_ENDING;
		if (byte < 0x21U || byte > 0x7eU)
			return ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER;
		if (unsupported_syntax(byte))
			return ZBL_UEFI_ZEDBSD_CONFIG_UNSUPPORTED_SYNTAX;
	}
	line->text = iterator->source + start;
	line->length = length;
	if (length == 0U)
		return ZBL_UEFI_ZEDBSD_CONFIG_OK;
	while (line->equal < length && line->text[line->equal] != '=')
		line->equal++;
	if (line->equal == 0U || line->equal == length ||
	    line->equal + 1U == length)
		return ZBL_UEFI_ZEDBSD_CONFIG_MALFORMED_LINE;
	return ZBL_UEFI_ZEDBSD_CONFIG_OK;
}

static int
uppercase_hex(unsigned char byte)
{
	return (byte >= '0' && byte <= '9') ||
	    (byte >= 'A' && byte <= 'F');
}

static int
selected_uuid_valid(const char *uuid, size_t capacity)
{
	if (uuid == NULL || capacity <= ZBL_ZEDBSD_CONFIG_FAT_UUID_LENGTH)
		return 0;
	for (size_t index = 0U; index < ZBL_ZEDBSD_CONFIG_FAT_UUID_LENGTH;
	     index++) {
		unsigned char byte = (unsigned char)uuid[index];

		if (index == 4U) {
			if (byte != '-')
				return 0;
		} else if (!uppercase_hex(byte)) {
			return 0;
		}
	}
	return uuid[ZBL_ZEDBSD_CONFIG_FAT_UUID_LENGTH] == '\0';
}

static int
component_is_dot(const unsigned char *path, size_t start, size_t end)
{
	return (end - start == 1U && path[start] == '.') ||
	    (end - start == 2U && path[start] == '.' &&
	     path[start + 1U] == '.');
}

static enum zbl_uefi_zedbsd_config_result
relative_path_validate(const unsigned char *path, size_t length,
		       enum zbl_uefi_zedbsd_config_result invalid_result,
		       enum zbl_uefi_zedbsd_config_result long_result)
{
	size_t start = 0U;
	size_t component = 0U;

	if (length != 0U && path[0] == '/')
		start = 1U;
	if (length - start == 0U)
		return invalid_result;
	if (length - start > ZBL_ZEDBSD_CONFIG_KERNEL_PATH_MAX)
		return long_result;
	component = start;
	for (size_t index = start; index <= length; index++) {
		unsigned char byte = index == length ? 0U : path[index];

		if (byte == 0U || byte == '/') {
			if (index == component ||
			    component_is_dot(path, component, index))
				return invalid_result;
			component = index + 1U;
		} else if (byte < 0x21U || byte > 0x7eU || byte == '\\') {
			return invalid_result;
		}
	}
	return ZBL_UEFI_ZEDBSD_CONFIG_OK;
}

static int
kernel_selector_prefix(const unsigned char *path, size_t length)
{
	static const char *const prefixes[] = {
		"dev/", "UUID=", "LABEL=", "PARTUUID=", "PARTLABEL=",
	};

	for (size_t index = 0U;
	     index < sizeof(prefixes) / sizeof(prefixes[0]); index++)
		if (text_starts_with(path, length, prefixes[index]))
			return 1;
	return 0;
}

static enum zbl_uefi_zedbsd_config_result
kernel_path_copy(char *destination, const unsigned char *path, size_t length)
{
	size_t start = length != 0U && path[0] == '/' ? 1U : 0U;
	enum zbl_uefi_zedbsd_config_result result;

	result = relative_path_validate(path, length,
	    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_KERNEL_PATH,
	    ZBL_UEFI_ZEDBSD_CONFIG_KERNEL_PATH_TOO_LONG);
	if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK)
		return result;
	for (size_t index = start; index < length; index++) {
		if (path[index] == ':')
			return ZBL_UEFI_ZEDBSD_CONFIG_INVALID_KERNEL_PATH;
	}
	if (kernel_selector_prefix(path + start, length - start))
		return ZBL_UEFI_ZEDBSD_CONFIG_INVALID_KERNEL_PATH;
	for (size_t index = start; index < length; index++)
		destination[index - start] = (char)path[index];
	destination[length - start] = '\0';
	return ZBL_UEFI_ZEDBSD_CONFIG_OK;
}

static int
line_name_is(const struct config_line *line, const char *name)
{
	return text_matches(line->text, line->equal, name);
}

static int
boot_reference(const unsigned char *value, size_t length)
{
	return length >= 6U && value[0] == 'b' && value[1] == 'o' &&
	    value[2] == 'o' && value[3] == 't' && value[4] >= '0' &&
	    value[4] <= '3' && value[5] == ':';
}

static int
raw_swap_selector(const unsigned char *value, size_t length)
{
	static const char *const prefixes[] = {
		"/dev/", "UUID=", "LABEL=", "PARTUUID=", "PARTLABEL=",
	};

	for (size_t index = 0U;
	     index < sizeof(prefixes) / sizeof(prefixes[0]); index++)
		if (text_starts_with(value, length, prefixes[index]))
			return 1;
	return 0;
}

static int
line_is_swap(const struct config_line *line)
{
	return line->equal == 5U && line->text[0] == 's' &&
	    line->text[1] == 'w' && line->text[2] == 'a' &&
	    line->text[3] == 'p' && line->text[4] >= '0' &&
	    line->text[4] <= '3';
}

static int
line_needs_boot0(const struct config_line *line)
{
	const unsigned char *value = line->text + line->equal + 1U;
	size_t value_length = line->length - line->equal - 1U;
	int overlay = line_name_is(line, "overlay-root") ||
	    line_name_is(line, "overlay-data");
	int swap = line_is_swap(line);

	if ((!overlay && !swap) || boot_reference(value, value_length))
		return 0;
	if (swap && raw_swap_selector(value, value_length))
		return 0;
	return 1;
}

static enum zbl_uefi_zedbsd_config_result
builder_append(struct parameter_builder *builder, const unsigned char *text,
	       size_t length)
{
	if (length > ZEDBSD_BOOT_PARAMETERS_TEXT_MAX - builder->length)
		return ZBL_UEFI_ZEDBSD_CONFIG_PARAMETERS_TOO_LONG;
	for (size_t index = 0U; index < length; index++)
		builder->record->text[builder->length + index] = (char)text[index];
	builder->length += length;
	return ZBL_UEFI_ZEDBSD_CONFIG_OK;
}

static enum zbl_uefi_zedbsd_config_result
builder_separator(struct parameter_builder *builder)
{
	static const unsigned char separator[] = " ";

	if (builder->length == 0U)
		return ZBL_UEFI_ZEDBSD_CONFIG_OK;
	return builder_append(builder, separator, sizeof(separator) - 1U);
}

static enum zbl_uefi_zedbsd_config_result
builder_selected_boot0(struct parameter_builder *builder, const char *uuid)
{
	static const unsigned char prefix[] = "boot0=UUID=";
	enum zbl_uefi_zedbsd_config_result result;

	result = builder_append(builder, prefix, sizeof(prefix) - 1U);
	if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK)
		return result;
	return builder_append(builder, (const unsigned char *)uuid,
	    ZBL_ZEDBSD_CONFIG_FAT_UUID_LENGTH);
}

static enum zbl_uefi_zedbsd_config_result
builder_line(struct parameter_builder *builder, const struct config_line *line)
{
	static const unsigned char qualifier[] = "boot0:";
	const unsigned char *value = line->text + line->equal + 1U;
	size_t value_length = line->length - line->equal - 1U;
	enum zbl_uefi_zedbsd_config_result result;

	result = builder_separator(builder);
	if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK)
		return result;
	if (!line_needs_boot0(line))
		return builder_append(builder, line->text, line->length);
	result = relative_path_validate(value, value_length,
	    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_PARAMETER_PATH,
	    ZBL_UEFI_ZEDBSD_CONFIG_PARAMETER_PATH_TOO_LONG);
	if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK)
		return result;
	result = builder_append(builder, line->text, line->equal + 1U);
	if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK)
		return result;
	result = builder_append(builder, qualifier, sizeof(qualifier) - 1U);
	if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK)
		return result;
	return builder_append(builder, value, value_length);
}

static void
record_finish(struct zedbsd_boot_parameter_record *record, size_t length)
{
	record->magic = ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC;
	record->version = ZEDBSD_BOOT_PARAMETER_RECORD_VERSION;
	record->size = (uint16_t)sizeof(*record);
	record->flags = ZEDBSD_BOOT_PARAMETER_RECORD_FLAG_TEXT;
	record->length = (uint16_t)length;
	record->reserved = 0U;
	record->text[length] = '\0';
}

enum zbl_uefi_zedbsd_config_result
zbl_uefi_zedbsd_config_parse(
	struct zbl_uefi_zedbsd_config *configuration,
	const void *source, size_t source_size,
	const char *selected_uuid, size_t selected_uuid_capacity)
{
	const unsigned char *bytes = (const unsigned char *)source;
	struct line_iterator iterator;
	struct config_line line;
	struct parameter_builder builder;
	enum zbl_uefi_zedbsd_config_result result;
	int have_kernel = 0;
	int have_boot0 = 0;

	if (configuration == NULL)
		return ZBL_UEFI_ZEDBSD_CONFIG_INVALID_ARGUMENT;
	configuration_zero(configuration);
	if ((source == NULL && source_size != 0U) ||
	    !selected_uuid_valid(selected_uuid, selected_uuid_capacity))
		return source == NULL && source_size != 0U ?
		    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_ARGUMENT :
		    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_SELECTED_UUID;
	if (source_size > ZBL_ZEDBSD_CONFIG_FILE_MAX)
		return ZBL_UEFI_ZEDBSD_CONFIG_FILE_TOO_LONG;

	iterator.source = bytes;
	iterator.size = source_size;
	iterator.position = 0U;
	iterator.count = 0U;
	for (;;) {
		result = next_line(&iterator, &line);
		if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK) {
			configuration_zero(configuration);
			return result;
		}
		if (line.text == NULL)
			break;
		if (line.length == 0U)
			continue;
		if (line_name_is(&line, "kernel")) {
			if (have_kernel) {
				configuration_zero(configuration);
				return ZBL_UEFI_ZEDBSD_CONFIG_DUPLICATE_KERNEL;
			}
			result = kernel_path_copy(configuration->kernel_path,
			    line.text + line.equal + 1U,
			    line.length - line.equal - 1U);
			if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK) {
				configuration_zero(configuration);
				return result;
			}
			have_kernel = 1;
		} else if (line_name_is(&line, "boot0")) {
			have_boot0 = 1;
		}
	}
	if (!have_kernel) {
		configuration_zero(configuration);
		return ZBL_UEFI_ZEDBSD_CONFIG_MISSING_KERNEL;
	}

	builder.record = &configuration->parameter_record;
	builder.length = 0U;
	if (!have_boot0) {
		result = builder_selected_boot0(&builder, selected_uuid);
		if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK) {
			configuration_zero(configuration);
			return result;
		}
	}
	iterator.position = 0U;
	iterator.count = 0U;
	for (;;) {
		result = next_line(&iterator, &line);
		if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK) {
			configuration_zero(configuration);
			return result;
		}
		if (line.text == NULL)
			break;
		if (line.length == 0U || line_name_is(&line, "kernel"))
			continue;
		result = builder_line(&builder, &line);
		if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK) {
			configuration_zero(configuration);
			return result;
		}
	}
	record_finish(builder.record, builder.length);
	return ZBL_UEFI_ZEDBSD_CONFIG_OK;
}

const char *
zbl_uefi_zedbsd_config_result_name(
	enum zbl_uefi_zedbsd_config_result result)
{
	switch (result) {
	case ZBL_UEFI_ZEDBSD_CONFIG_OK:
		return "ok";
	case ZBL_UEFI_ZEDBSD_CONFIG_INVALID_ARGUMENT:
		return "invalid-argument";
	case ZBL_UEFI_ZEDBSD_CONFIG_FILE_TOO_LONG:
		return "file-too-long";
	case ZBL_UEFI_ZEDBSD_CONFIG_TOO_MANY_LINES:
		return "too-many-lines";
	case ZBL_UEFI_ZEDBSD_CONFIG_LINE_TOO_LONG:
		return "line-too-long";
	case ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER:
		return "invalid-character";
	case ZBL_UEFI_ZEDBSD_CONFIG_INVALID_LINE_ENDING:
		return "invalid-line-ending";
	case ZBL_UEFI_ZEDBSD_CONFIG_UNSUPPORTED_SYNTAX:
		return "unsupported-syntax";
	case ZBL_UEFI_ZEDBSD_CONFIG_MALFORMED_LINE:
		return "malformed-line";
	case ZBL_UEFI_ZEDBSD_CONFIG_DUPLICATE_KERNEL:
		return "duplicate-kernel";
	case ZBL_UEFI_ZEDBSD_CONFIG_MISSING_KERNEL:
		return "missing-kernel";
	case ZBL_UEFI_ZEDBSD_CONFIG_INVALID_KERNEL_PATH:
		return "invalid-kernel-path";
	case ZBL_UEFI_ZEDBSD_CONFIG_KERNEL_PATH_TOO_LONG:
		return "kernel-path-too-long";
	case ZBL_UEFI_ZEDBSD_CONFIG_INVALID_PARAMETER_PATH:
		return "invalid-parameter-path";
	case ZBL_UEFI_ZEDBSD_CONFIG_PARAMETER_PATH_TOO_LONG:
		return "parameter-path-too-long";
	case ZBL_UEFI_ZEDBSD_CONFIG_INVALID_SELECTED_UUID:
		return "invalid-selected-uuid";
	case ZBL_UEFI_ZEDBSD_CONFIG_PARAMETERS_TOO_LONG:
		return "parameters-too-long";
	default:
		return "unknown";
	}
}
