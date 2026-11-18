/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bounded zedbsd.cfg parsing and boot-parameter assembly shared by the
 * UEFI and the BIOS loaders.
 */

#include "zedbsd-config.h"

/*
 * One line of the configuration file.
 *
 * It points into the caller's source buffer and lives only while the line is
 * examined.  equal is the index of the line's first '='.
 */
struct config_line {
	const unsigned char *text;
	size_t length;
	size_t equal;
};

/* The position of one pass over the configuration file's lines. */
struct line_iterator {
	const unsigned char *source;
	size_t size;
	size_t position;
	unsigned count;
};

/* The boot parameter text being assembled into the result's record. */
struct parameter_builder {
	struct kern_boot_parameter_record *record;
	size_t length;
};

static void configuration_zero(struct zbl_uefi_kern_config *configuration);
static int text_matches(const unsigned char *text, size_t length, const char *candidate);
static int text_starts_with(const unsigned char *text, size_t length, const char *prefix);
static int unsupported_syntax(unsigned char byte);
static enum zbl_uefi_kern_config_result next_line(struct line_iterator *iterator, struct config_line *line);
static int uppercase_hex(unsigned char byte);
static int selected_uuid_valid(const char *uuid, size_t capacity);
static int component_is_dot(const unsigned char *path, size_t start, size_t end);
static enum zbl_uefi_kern_config_result relative_path_validate(const unsigned char *path, size_t length, enum zbl_uefi_kern_config_result invalid_result, enum zbl_uefi_kern_config_result long_result);
static int kernel_selector_prefix(const unsigned char *path, size_t length);
static enum zbl_uefi_kern_config_result kernel_path_copy(char *destination, const unsigned char *path, size_t length);
static int line_name_is(const struct config_line *line, const char *name);
static int boot_reference(const unsigned char *value, size_t length);
static int raw_swap_selector(const unsigned char *value, size_t length);
static int line_is_swap(const struct config_line *line);
static int line_needs_boot0(const struct config_line *line);
static enum zbl_uefi_kern_config_result builder_append(struct parameter_builder *builder, const unsigned char *text, size_t length);
static enum zbl_uefi_kern_config_result builder_separator(struct parameter_builder *builder);
static enum zbl_uefi_kern_config_result builder_selected_boot0(struct parameter_builder *builder, const char *uuid);
static enum zbl_uefi_kern_config_result builder_line(struct parameter_builder *builder, const struct config_line *line);
static void record_finish(struct kern_boot_parameter_record *record, size_t length);

/*
 * Parses zedbsd.cfg into the kernel path and the boot parameter record.
 *
 * The first pass validates every line and takes the one kernel= line; the
 * second assembles the other lines into the parameter text, supplying boot0=
 * from the selected volume when the file names none.
 */
enum zbl_uefi_kern_config_result
zbl_uefi_kern_config_parse(
	struct zbl_uefi_kern_config *configuration,
	const void *source,
	size_t source_size,
	const char *selected_uuid,
	size_t selected_uuid_capacity)
{
	const unsigned char *bytes;
	struct line_iterator iterator;
	struct config_line line;
	struct parameter_builder builder;
	enum zbl_uefi_kern_config_result result;
	int have_kernel;
	int have_boot0;
	int valid;
	int is_kernel;
	int is_boot0;

	/* Views the source as bytes, with no kernel= or boot0= line seen yet. */
	bytes = (const unsigned char *)source;
	have_kernel = 0;
	have_boot0 = 0;

	/* Refuses a call with no result object. */
	if (configuration == NULL)
		return ZBL_UEFI_KERN_CONFIG_INVALID_ARGUMENT;

	/* Clears the result, so a failure leaves nothing half filled. */
	configuration_zero(configuration);

	/* Refuses a missing source that claims bytes. */
	if (source == NULL && source_size != 0U)
		return ZBL_UEFI_KERN_CONFIG_INVALID_ARGUMENT;

	/* Refuses a selected volume serial that is not in canonical form. */
	valid = selected_uuid_valid(selected_uuid, selected_uuid_capacity);
	if (!valid)
		return ZBL_UEFI_KERN_CONFIG_INVALID_SELECTED_UUID;

	/* Refuses a file beyond the loader's bound. */
	if (source_size > ZBL_KERN_CONFIG_FILE_MAX)
		return ZBL_UEFI_KERN_CONFIG_FILE_TOO_LONG;

	/* Validates every line and takes the kernel path in the first pass. */
	iterator.source = bytes;
	iterator.size = source_size;
	iterator.position = 0U;
	iterator.count = 0U;
	for (;;) {
		/* Reads the next line; a bad line fails the whole file. */
		result = next_line(&iterator, &line);
		if (result != ZBL_UEFI_KERN_CONFIG_OK) {
			configuration_zero(configuration);
			return result;
		}

		/* The end of the input ends the pass. */
		if (line.text == NULL)
			break;

		/* An empty line names nothing. */
		if (line.length == 0U)
			continue;

		/* The one kernel= line gives the kernel's path. */
		is_kernel = line_name_is(&line, "kernel");
		if (is_kernel) {
			/* Refuses a second kernel= line. */
			if (have_kernel) {
				configuration_zero(configuration);
				return ZBL_UEFI_KERN_CONFIG_DUPLICATE_KERNEL;
			}

			/* Copies the validated path. */
			result = kernel_path_copy(configuration->kernel_path,
						  line.text + line.equal + 1U,
						  line.length - line.equal - 1U);
			if (result != ZBL_UEFI_KERN_CONFIG_OK) {
				configuration_zero(configuration);
				return result;
			}

			have_kernel = 1;
			continue;
		}

		/* A boot0= line names the boot volume itself. */
		is_boot0 = line_name_is(&line, "boot0");
		if (is_boot0)
			have_boot0 = 1;
	}

	/* Refuses a file without a kernel= line. */
	if (!have_kernel) {
		configuration_zero(configuration);
		return ZBL_UEFI_KERN_CONFIG_MISSING_KERNEL;
	}

	/* Starts the parameter text with boot0= for the selected volume when the file names none. */
	builder.record = &configuration->parameter_record;
	builder.length = 0U;
	if (!have_boot0) {
		result = builder_selected_boot0(&builder, selected_uuid);
		if (result != ZBL_UEFI_KERN_CONFIG_OK) {
			configuration_zero(configuration);
			return result;
		}
	}

	/* Appends every other line to the parameter text in the second pass. */
	iterator.position = 0U;
	iterator.count = 0U;
	for (;;) {
		/* Reads the next line again. */
		result = next_line(&iterator, &line);
		if (result != ZBL_UEFI_KERN_CONFIG_OK) {
			configuration_zero(configuration);
			return result;
		}

		/* The end of the input ends the pass. */
		if (line.text == NULL)
			break;

		/* An empty line adds nothing. */
		if (line.length == 0U)
			continue;

		/* The kernel= line is the loader's, not the kernel's. */
		is_kernel = line_name_is(&line, "kernel");
		if (is_kernel)
			continue;

		/* Appends the line as a parameter. */
		result = builder_line(&builder, &line);
		if (result != ZBL_UEFI_KERN_CONFIG_OK) {
			configuration_zero(configuration);
			return result;
		}
	}

	/* Completes the record around the assembled text. */
	record_finish(builder.record, builder.length);

	/* Succeeded: the result holds the kernel path and the parameters. */
	return ZBL_UEFI_KERN_CONFIG_OK;
}

/* Names a parse result for a loader message. */
const char *
zbl_uefi_kern_config_result_name(
	enum zbl_uefi_kern_config_result result)
{
	/* Names each result; anything else is unknown. */
	switch (result) {
	case ZBL_UEFI_KERN_CONFIG_OK:
		return "ok";
	case ZBL_UEFI_KERN_CONFIG_INVALID_ARGUMENT:
		return "invalid-argument";
	case ZBL_UEFI_KERN_CONFIG_FILE_TOO_LONG:
		return "file-too-long";
	case ZBL_UEFI_KERN_CONFIG_TOO_MANY_LINES:
		return "too-many-lines";
	case ZBL_UEFI_KERN_CONFIG_LINE_TOO_LONG:
		return "line-too-long";
	case ZBL_UEFI_KERN_CONFIG_INVALID_CHARACTER:
		return "invalid-character";
	case ZBL_UEFI_KERN_CONFIG_INVALID_LINE_ENDING:
		return "invalid-line-ending";
	case ZBL_UEFI_KERN_CONFIG_UNSUPPORTED_SYNTAX:
		return "unsupported-syntax";
	case ZBL_UEFI_KERN_CONFIG_MALFORMED_LINE:
		return "malformed-line";
	case ZBL_UEFI_KERN_CONFIG_DUPLICATE_KERNEL:
		return "duplicate-kernel";
	case ZBL_UEFI_KERN_CONFIG_MISSING_KERNEL:
		return "missing-kernel";
	case ZBL_UEFI_KERN_CONFIG_INVALID_KERNEL_PATH:
		return "invalid-kernel-path";
	case ZBL_UEFI_KERN_CONFIG_KERNEL_PATH_TOO_LONG:
		return "kernel-path-too-long";
	case ZBL_UEFI_KERN_CONFIG_INVALID_PARAMETER_PATH:
		return "invalid-parameter-path";
	case ZBL_UEFI_KERN_CONFIG_PARAMETER_PATH_TOO_LONG:
		return "parameter-path-too-long";
	case ZBL_UEFI_KERN_CONFIG_INVALID_SELECTED_UUID:
		return "invalid-selected-uuid";
	case ZBL_UEFI_KERN_CONFIG_PARAMETERS_TOO_LONG:
		return "parameters-too-long";
	default:
		return "unknown";
	}
}

/* Clears the whole result object. */
static void
configuration_zero(
	struct zbl_uefi_kern_config *configuration)
{
	unsigned char *bytes;
	size_t index;

	/* Clears the object byte by byte; the loaders have no string functions. */
	bytes = (unsigned char *)configuration;
	for (index = 0U; index < sizeof(*configuration); index++)
		bytes[index] = 0U;
}

/* Reports whether a text is exactly the given string. */
static int
text_matches(
	const unsigned char *text,
	size_t length,
	const char *candidate)
{
	size_t candidate_length;
	size_t index;

	/* Measures the string. */
	candidate_length = 0U;
	while (candidate[candidate_length] != '\0')
		candidate_length++;

	/* A text of another length is not the string. */
	if (candidate_length != length)
		return 0;

	/* Compares the text with the string character by character. */
	for (index = 0U; index < length; index++) {
		if (text[index] != (unsigned char)candidate[index])
			return 0;
	}

	/* Succeeded: the text is the string. */
	return 1;
}

/* Reports whether a text starts with the given prefix. */
static int
text_starts_with(
	const unsigned char *text,
	size_t length,
	const char *prefix)
{
	size_t prefix_length;
	size_t index;

	/* Measures the prefix. */
	prefix_length = 0U;
	while (prefix[prefix_length] != '\0')
		prefix_length++;

	/* A text shorter than the prefix cannot start with it. */
	if (length < prefix_length)
		return 0;

	/* Compares the start of the text with the prefix. */
	for (index = 0U; index < prefix_length; index++) {
		if (text[index] != (unsigned char)prefix[index])
			return 0;
	}

	/* Succeeded: the text starts with the prefix. */
	return 1;
}

/* Reports whether a byte belongs to quoting, comment or escape syntax the file may not use. */
static int
unsupported_syntax(
	unsigned char byte)
{
	/* Quotes, comment markers and the backslash are refused rather than misread. */
	if (byte == '"' ||
	    byte == '\'' ||
	    byte == '#' ||
	    byte == ';' ||
	    byte == '\\')
		return 1;

	/* Any other byte is plain text. */
	return 0;
}

/*
 * Reads and validates the next line of the file.
 *
 * Returns OK with line->text == NULL at end of input.
 */
static enum zbl_uefi_kern_config_result
next_line(
	struct line_iterator *iterator,
	struct config_line *line)
{
	size_t start;
	size_t end;
	size_t length;
	size_t index;
	unsigned char byte;
	int unsupported;

	/* Starts with no line. */
	line->text = NULL;
	line->length = 0U;
	line->equal = 0U;

	/* The end of the input is reported as no line. */
	if (iterator->position == iterator->size)
		return ZBL_UEFI_KERN_CONFIG_OK;

	/* Finds the end of the line. */
	start = iterator->position;
	end = start;
	while (end < iterator->size && iterator->source[end] != '\n')
		end++;

	/* Refuses a file with more lines than the loader takes. */
	iterator->count++;
	if (iterator->count > ZBL_KERN_CONFIG_LINE_COUNT_MAX)
		return ZBL_UEFI_KERN_CONFIG_TOO_MANY_LINES;

	/* Moves past the newline and drops a CR just before it. */
	length = end - start;
	if (end < iterator->size) {
		iterator->position = end + 1U;
		if (length != 0U && iterator->source[end - 1U] == '\r')
			length--;
	} else {
		iterator->position = end;
	}

	/* Refuses a line beyond the loader's bound. */
	if (length > ZBL_KERN_CONFIG_LINE_MAX)
		return ZBL_UEFI_KERN_CONFIG_LINE_TOO_LONG;

	/* Refuses a stray CR, a blank or non-ASCII byte, and syntax the file may not use. */
	for (index = 0U; index < length; index++) {
		byte = iterator->source[start + index];
		if (byte == '\r')
			return ZBL_UEFI_KERN_CONFIG_INVALID_LINE_ENDING;
		if (byte < 0x21U || byte > 0x7eU)
			return ZBL_UEFI_KERN_CONFIG_INVALID_CHARACTER;

		unsupported = unsupported_syntax(byte);
		if (unsupported)
			return ZBL_UEFI_KERN_CONFIG_UNSUPPORTED_SYNTAX;
	}

	/* An empty line is valid. */
	line->text = iterator->source + start;
	line->length = length;
	if (length == 0U)
		return ZBL_UEFI_KERN_CONFIG_OK;

	/* Finds the line's first '='. */
	while (line->equal < length && line->text[line->equal] != '=')
		line->equal++;

	/* Refuses a line without a name, without '=', or without a value. */
	if (line->equal == 0U ||
	    line->equal == length ||
	    line->equal + 1U == length)
		return ZBL_UEFI_KERN_CONFIG_MALFORMED_LINE;

	/* Succeeded: the line is a name and a value. */
	return ZBL_UEFI_KERN_CONFIG_OK;
}

/* Reports whether a byte is an upper-case hexadecimal digit. */
static int
uppercase_hex(
	unsigned char byte)
{
	/* A decimal digit. */
	if (byte >= '0' && byte <= '9')
		return 1;

	/* An upper-case letter from A to F. */
	if (byte >= 'A' && byte <= 'F')
		return 1;

	/* Refuses anything else. */
	return 0;
}

/* Reports whether a FAT volume serial is in the canonical XXXX-XXXX form and terminated. */
static int
selected_uuid_valid(
	const char *uuid,
	size_t capacity)
{
	size_t index;
	unsigned char byte;
	int hex;

	/* Refuses a missing serial and one whose buffer cannot hold the form. */
	if (uuid == NULL || capacity <= ZBL_KERN_CONFIG_FAT_UUID_LENGTH)
		return 0;

	/* Checks each character: a dash in the middle and hexadecimal digits elsewhere. */
	for (index = 0U; index < ZBL_KERN_CONFIG_FAT_UUID_LENGTH; index++) {
		byte = (unsigned char)uuid[index];
		if (index == 4U) {
			/* The middle character is the dash. */
			if (byte != '-')
				return 0;
		} else {
			/* Every other character is an upper-case hexadecimal digit. */
			hex = uppercase_hex(byte);
			if (!hex)
				return 0;
		}
	}

	/* Refuses a serial that goes on past the form. */
	if (uuid[ZBL_KERN_CONFIG_FAT_UUID_LENGTH] != '\0')
		return 0;

	/* Succeeded: the serial is canonical. */
	return 1;
}

/* Reports whether a path component is "." or "..". */
static int
component_is_dot(
	const unsigned char *path,
	size_t start,
	size_t end)
{
	/* The component is ".". */
	if (end - start == 1U && path[start] == '.')
		return 1;

	/* The component is "..". */
	if (end - start == 2U &&
	    path[start] == '.' &&
	    path[start + 1U] == '.')
		return 1;

	/* Any other component is a name. */
	return 0;
}

/*
 * Validates a path relative to the boot volume.
 *
 * One leading slash is allowed.  Empty, "." and ".." components, blanks,
 * non-ASCII bytes and backslashes are refused with invalid_result, and a path
 * beyond the bound with long_result.
 */
static enum zbl_uefi_kern_config_result
relative_path_validate(
	const unsigned char *path,
	size_t length,
	enum zbl_uefi_kern_config_result invalid_result,
	enum zbl_uefi_kern_config_result long_result)
{
	size_t start;
	size_t component;
	size_t index;
	unsigned char byte;
	int dot;

	/* Skips one leading slash. */
	start = 0U;
	if (length != 0U && path[0] == '/')
		start = 1U;

	/* Refuses an empty path. */
	if (length - start == 0U)
		return invalid_result;

	/* Refuses a path beyond the bound. */
	if (length - start > ZBL_KERN_CONFIG_KERNEL_PATH_MAX)
		return long_result;

	/* Checks each component as its end is reached; the end of the path ends the last one. */
	component = start;
	for (index = start; index <= length; index++) {
		byte = index == length ? 0U : path[index];
		if (byte == 0U || byte == '/') {
			/* Refuses an empty component. */
			if (index == component)
				return invalid_result;

			/* Refuses a "." or ".." component. */
			dot = component_is_dot(path, component, index);
			if (dot)
				return invalid_result;

			component = index + 1U;
		} else if (byte < 0x21U || byte > 0x7eU || byte == '\\') {
			/* Refuses a blank, a non-ASCII byte and a backslash. */
			return invalid_result;
		}
	}

	/* Succeeded: the path is a safe relative path. */
	return ZBL_UEFI_KERN_CONFIG_OK;
}

/* Reports whether a kernel path starts like a device or volume selector. */
static int
kernel_selector_prefix(
	const unsigned char *path,
	size_t length)
{
	/* The selector forms a kernel path may not take. */
	static const char *const prefixes[] = {
		"dev/", "UUID=", "LABEL=", "PARTUUID=", "PARTLABEL=",
	};
	size_t index;
	int starts;

	/* Compares the path with each selector form. */
	for (index = 0U; index < sizeof(prefixes) / sizeof(prefixes[0]); index++) {
		starts = text_starts_with(path, length, prefixes[index]);
		if (starts)
			return 1;
	}

	/* The path is a plain file path. */
	return 0;
}

/* Validates the kernel path and copies it without its leading slash. */
static enum zbl_uefi_kern_config_result
kernel_path_copy(
	char *destination,
	const unsigned char *path,
	size_t length)
{
	size_t start;
	size_t index;
	enum zbl_uefi_kern_config_result result;
	int selector;

	/* Skips one leading slash. */
	start = 0U;
	if (length != 0U && path[0] == '/')
		start = 1U;

	/* Refuses an unsafe path. */
	result = relative_path_validate(path,
					length,
					ZBL_UEFI_KERN_CONFIG_INVALID_KERNEL_PATH,
					ZBL_UEFI_KERN_CONFIG_KERNEL_PATH_TOO_LONG);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* Refuses a path with a volume qualifier: the kernel is on the boot volume. */
	for (index = start; index < length; index++) {
		if (path[index] == ':')
			return ZBL_UEFI_KERN_CONFIG_INVALID_KERNEL_PATH;
	}

	/* Refuses a device or volume selector in place of a path. */
	selector = kernel_selector_prefix(path + start, length - start);
	if (selector)
		return ZBL_UEFI_KERN_CONFIG_INVALID_KERNEL_PATH;

	/* Copies the path and terminates it. */
	for (index = start; index < length; index++)
		destination[index - start] = (char)path[index];
	destination[length - start] = '\0';

	/* Succeeded: the destination holds the kernel path. */
	return ZBL_UEFI_KERN_CONFIG_OK;
}

/* Reports whether a line's name is the given name. */
static int
line_name_is(
	const struct config_line *line,
	const char *name)
{
	int matches;

	/* Compares the text before the '=' with the name. */
	matches = text_matches(line->text, line->equal, name);
	if (!matches)
		return 0;

	/* Succeeded: the line has the name. */
	return 1;
}

/* Reports whether a value is already qualified by a boot volume (boot0: to boot3:). */
static int
boot_reference(
	const unsigned char *value,
	size_t length)
{
	/* The value starts with bootN: for N from 0 to 3. */
	if (length >= 6U &&
	    value[0] == 'b' &&
	    value[1] == 'o' &&
	    value[2] == 'o' &&
	    value[3] == 't' &&
	    value[4] >= '0' &&
	    value[4] <= '3' &&
	    value[5] == ':')
		return 1;

	/* Any other value is unqualified. */
	return 0;
}

/* Reports whether a swap value names a raw device or volume rather than a file. */
static int
raw_swap_selector(
	const unsigned char *value,
	size_t length)
{
	/* The selector forms that name a raw swap device. */
	static const char *const prefixes[] = {
		"/dev/", "UUID=", "LABEL=", "PARTUUID=", "PARTLABEL=",
	};
	size_t index;
	int starts;

	/* Compares the value with each selector form. */
	for (index = 0U; index < sizeof(prefixes) / sizeof(prefixes[0]); index++) {
		starts = text_starts_with(value, length, prefixes[index]);
		if (starts)
			return 1;
	}

	/* The value is a swap file. */
	return 0;
}

/* Reports whether a line is swap0= to swap3=. */
static int
line_is_swap(
	const struct config_line *line)
{
	/* The name is swapN for N from 0 to 3. */
	if (line->equal == 5U &&
	    line->text[0] == 's' &&
	    line->text[1] == 'w' &&
	    line->text[2] == 'a' &&
	    line->text[3] == 'p' &&
	    line->text[4] >= '0' &&
	    line->text[4] <= '3')
		return 1;

	/* Any other name is not a swap line. */
	return 0;
}

/* Reports whether a line's relative file value must be qualified with boot0:. */
static int
line_needs_boot0(
	const struct config_line *line)
{
	const unsigned char *value;
	size_t value_length;
	int overlay;
	int swap;
	int reference;
	int selector;

	/* Finds the value after the '='. */
	value = line->text + line->equal + 1U;
	value_length = line->length - line->equal - 1U;

	/* Finds whether the line names an overlay image or a swap area. */
	overlay = line_name_is(line, "overlay-root");
	if (!overlay)
		overlay = line_name_is(line, "overlay-data");
	swap = line_is_swap(line);

	/* Only overlay and swap lines name files on a volume. */
	if (!overlay && !swap)
		return 0;

	/* A value already qualified by a boot volume is kept as it is. */
	reference = boot_reference(value, value_length);
	if (reference)
		return 0;

	/* A swap line that names a raw device is kept as it is. */
	if (swap) {
		selector = raw_swap_selector(value, value_length);
		if (selector)
			return 0;
	}

	/* Succeeded: the value is a file on the boot volume. */
	return 1;
}

/* Appends text to the parameter text when it fits the kernel's limit. */
static enum zbl_uefi_kern_config_result
builder_append(
	struct parameter_builder *builder,
	const unsigned char *text,
	size_t length)
{
	size_t index;

	/* Refuses text that would pass the kernel's limit. */
	if (length > KERN_BOOT_PARAMETERS_TEXT_MAX - builder->length)
		return ZBL_UEFI_KERN_CONFIG_PARAMETERS_TOO_LONG;

	/* Copies the text after what is there. */
	for (index = 0U; index < length; index++)
		builder->record->text[builder->length + index] = (char)text[index];
	builder->length += length;

	/* Succeeded: the text is appended. */
	return ZBL_UEFI_KERN_CONFIG_OK;
}

/* Appends the space that separates a parameter from the one before it. */
static enum zbl_uefi_kern_config_result
builder_separator(
	struct parameter_builder *builder)
{
	/* The separator between two parameters. */
	static const unsigned char separator[] = " ";
	enum zbl_uefi_kern_config_result result;

	/* The first parameter needs no separator. */
	if (builder->length == 0U)
		return ZBL_UEFI_KERN_CONFIG_OK;

	/* Appends the space. */
	result = builder_append(builder, separator, sizeof(separator) - 1U);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* Succeeded: the next parameter can follow. */
	return ZBL_UEFI_KERN_CONFIG_OK;
}

/* Appends boot0=UUID= with the selected volume's serial. */
static enum zbl_uefi_kern_config_result
builder_selected_boot0(
	struct parameter_builder *builder,
	const char *uuid)
{
	/* The parameter's name and the selector form of its value. */
	static const unsigned char prefix[] = "boot0=UUID=";
	enum zbl_uefi_kern_config_result result;

	/* Appends the name and the selector form. */
	result = builder_append(builder, prefix, sizeof(prefix) - 1U);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* Appends the serial. */
	result = builder_append(builder, (const unsigned char *)uuid, ZBL_KERN_CONFIG_FAT_UUID_LENGTH);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* Succeeded: boot0= names the selected volume. */
	return ZBL_UEFI_KERN_CONFIG_OK;
}

/* Appends one configuration line as a parameter, qualifying a relative file value with boot0:. */
static enum zbl_uefi_kern_config_result
builder_line(
	struct parameter_builder *builder,
	const struct config_line *line)
{
	/* The qualifier that puts a relative file on the boot volume. */
	static const unsigned char qualifier[] = "boot0:";
	const unsigned char *value;
	size_t value_length;
	enum zbl_uefi_kern_config_result result;
	int needs_boot0;

	/* Finds the value after the '='. */
	value = line->text + line->equal + 1U;
	value_length = line->length - line->equal - 1U;

	/* Separates the parameter from the one before it. */
	result = builder_separator(builder);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* A line whose value needs no qualifier is appended as it is. */
	needs_boot0 = line_needs_boot0(line);
	if (!needs_boot0) {
		result = builder_append(builder, line->text, line->length);
		if (result != ZBL_UEFI_KERN_CONFIG_OK)
			return result;

		return ZBL_UEFI_KERN_CONFIG_OK;
	}

	/* Refuses an unsafe relative file path. */
	result = relative_path_validate(value,
					value_length,
					ZBL_UEFI_KERN_CONFIG_INVALID_PARAMETER_PATH,
					ZBL_UEFI_KERN_CONFIG_PARAMETER_PATH_TOO_LONG);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* Appends the name and the '='. */
	result = builder_append(builder, line->text, line->equal + 1U);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* Appends the qualifier. */
	result = builder_append(builder, qualifier, sizeof(qualifier) - 1U);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* Appends the path. */
	result = builder_append(builder, value, value_length);
	if (result != ZBL_UEFI_KERN_CONFIG_OK)
		return result;

	/* Succeeded: the line is appended with its value on the boot volume. */
	return ZBL_UEFI_KERN_CONFIG_OK;
}

/* Fills the record's header around the assembled text and terminates the text. */
static void
record_finish(
	struct kern_boot_parameter_record *record,
	size_t length)
{
	/* Marks the record as a text record of the current version. */
	record->magic = KERN_BOOT_PARAMETER_RECORD_MAGIC;
	record->version = KERN_BOOT_PARAMETER_RECORD_VERSION;
	record->size = (uint16_t)sizeof(*record);
	record->flags = KERN_BOOT_PARAMETER_RECORD_FLAG_TEXT;
	record->length = (uint16_t)length;
	record->reserved = 0U;
	record->text[length] = '\0';
}
