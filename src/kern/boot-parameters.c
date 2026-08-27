/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/boot-parameters.h"

#include <errno.h>

struct parameter_name {
	const char *text;
	size_t length;
};

#define PARAMETER_NAME(value) { value, sizeof(value) - 1U }

static const struct parameter_name parameter_names[KERN_BOOT_PARAMETER_COUNT] = {
	PARAMETER_NAME("boot0"),
	PARAMETER_NAME("boot1"),
	PARAMETER_NAME("boot2"),
	PARAMETER_NAME("boot3"),
	PARAMETER_NAME("rootpart"),
	PARAMETER_NAME("overlay-root"),
	PARAMETER_NAME("overlay-data"),
	PARAMETER_NAME("swap0"),
	PARAMETER_NAME("swap1"),
	PARAMETER_NAME("swap2"),
	PARAMETER_NAME("swap3"),
	PARAMETER_NAME("init"),
};

static struct kern_boot_parameters current_parameters;
static int current_parameters_valid;
static int current_parameters_source_present;

static void
parameters_reset(struct kern_boot_parameters *parameters)
{
	unsigned index;

	parameters->storage[0] = '\0';
	for (index = 0; index < KERN_BOOT_PARAMETER_COUNT; index++)
		parameters->value_offset[index] =
		    KERN_BOOT_PARAMETER_OFFSET_ABSENT;
	parameters->unknown_count = 0;
	parameters->unknown_name_truncated = 0;
	parameters->unknown_name[0] = '\0';
}

static int
parse_error(struct kern_boot_parameters *parameters, int error)
{
	parameters_reset(parameters);
	return error;
}

static int
name_matches(const char *name, size_t length,
	     const struct parameter_name *candidate)
{
	size_t index;

	if (length != candidate->length)
		return 0;
	for (index = 0; index < length; index++)
		if (name[index] != candidate->text[index])
			return 0;
	return 1;
}

static int
parameter_key(const char *name, size_t length,
	      enum kern_boot_parameter_key *key)
{
	unsigned index;

	for (index = 0; index < KERN_BOOT_PARAMETER_COUNT; index++)
		if (name_matches(name, length, &parameter_names[index])) {
			*key = (enum kern_boot_parameter_key)index;
			return 1;
		}
	return 0;
}

static void
record_unknown(struct kern_boot_parameters *parameters, const char *name,
	       size_t length)
{
	size_t copy_length;

	parameters->unknown_count++;
	if (parameters->unknown_count != 1U)
		return;
	copy_length = length;
	if (copy_length > KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX) {
		copy_length = KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX;
		parameters->unknown_name_truncated = 1;
	}
	for (size_t index = 0; index < copy_length; index++)
		parameters->unknown_name[index] = name[index];
	parameters->unknown_name[copy_length] = '\0';
}

int
kern_boot_parameters_parse(struct kern_boot_parameters *parameters,
			   const char *input, size_t input_capacity)
{
	size_t length = 0;
	size_t scan_limit;
	size_t position;
	int terminated = 0;

	if (parameters == NULL)
		return EINVAL;
	parameters_reset(parameters);
	if (input == NULL)
		return input_capacity == 0U ? 0 : EINVAL;
	if (input_capacity == 0U)
		return EINVAL;

	scan_limit = input_capacity;
	if (scan_limit > KERN_BOOT_PARAMETERS_STORAGE_SIZE)
		scan_limit = KERN_BOOT_PARAMETERS_STORAGE_SIZE;
	for (length = 0; length < scan_limit; length++) {
		unsigned char byte = (unsigned char)input[length];

		if (byte == 0U) {
			terminated = 1;
			break;
		}
		if (byte > 0x7fU)
			return parse_error(parameters, EILSEQ);
	}
	if (!terminated)
		return parse_error(parameters,
		    input_capacity >= KERN_BOOT_PARAMETERS_STORAGE_SIZE ? E2BIG :
								 EINVAL);
	for (position = 0; position <= length; position++)
		parameters->storage[position] = input[position];

	position = 0;
	while (position < length) {
		size_t token_start;
		size_t token_end;
		size_t next;
		size_t equal;
		size_t value_start;
		size_t value_length;
		enum kern_boot_parameter_key key;
		int known;

		while (position < length &&
		       parameters->storage[position] == ' ')
			position++;
		if (position == length)
			break;
		token_start = position;
		while (position < length &&
		       parameters->storage[position] != ' ') {
			unsigned char byte =
			    (unsigned char)parameters->storage[position];

			if (byte < 0x21U || byte > 0x7eU)
				return parse_error(parameters, EINVAL);
			position++;
		}
		token_end = position;
		next = token_end;
		while (next < length && parameters->storage[next] == ' ')
			next++;
		equal = token_start;
		while (equal < token_end && parameters->storage[equal] != '=')
			equal++;
		if (equal == token_start || equal == token_end ||
		    equal + 1U == token_end)
			return parse_error(parameters, EINVAL);

		known = parameter_key(parameters->storage + token_start,
		    equal - token_start, &key);
		value_start = equal + 1U;
		value_length = token_end - value_start;
		if (known &&
		    parameters->value_offset[key] !=
			KERN_BOOT_PARAMETER_OFFSET_ABSENT)
			return parse_error(parameters, EEXIST);
		if (known && key == KERN_BOOT_PARAMETER_INIT) {
			if (parameters->storage[value_start] != '/')
				return parse_error(parameters, EINVAL);
			if (value_length > KERN_BOOT_PARAMETERS_INIT_PATH_MAX)
				return parse_error(parameters, ENAMETOOLONG);
		}

		if (token_end < length)
			parameters->storage[token_end] = '\0';
		parameters->storage[equal] = '\0';
		if (known)
			parameters->value_offset[key] = (uint16_t)value_start;
		else
			record_unknown(parameters,
			    parameters->storage + token_start,
			    equal - token_start);
		position = next;
	}
	return 0;
}

const char *
kern_boot_parameters_value(const struct kern_boot_parameters *parameters,
			   enum kern_boot_parameter_key key)
{
	uint16_t offset;

	if (parameters == NULL || (unsigned)key >= KERN_BOOT_PARAMETER_COUNT)
		return NULL;
	offset = parameters->value_offset[key];
	if (offset == KERN_BOOT_PARAMETER_OFFSET_ABSENT)
		return NULL;
	return parameters->storage + offset;
}

const char *
kern_boot_parameters_boot(const struct kern_boot_parameters *parameters,
			 unsigned index)
{
	if (index >= 4U)
		return NULL;
	return kern_boot_parameters_value(parameters,
	    (enum kern_boot_parameter_key)(KERN_BOOT_PARAMETER_BOOT0 + index));
}

const char *
kern_boot_parameters_swap(const struct kern_boot_parameters *parameters,
			 unsigned index)
{
	if (index >= 4U)
		return NULL;
	return kern_boot_parameters_value(parameters,
	    (enum kern_boot_parameter_key)(KERN_BOOT_PARAMETER_SWAP0 + index));
}

const char *
kern_boot_parameters_rootpart(const struct kern_boot_parameters *parameters)
{
	return kern_boot_parameters_value(parameters,
	    KERN_BOOT_PARAMETER_ROOTPART);
}

const char *
kern_boot_parameters_overlay_root(
	const struct kern_boot_parameters *parameters)
{
	return kern_boot_parameters_value(parameters,
	    KERN_BOOT_PARAMETER_OVERLAY_ROOT);
}

const char *
kern_boot_parameters_overlay_data(
	const struct kern_boot_parameters *parameters)
{
	return kern_boot_parameters_value(parameters,
	    KERN_BOOT_PARAMETER_OVERLAY_DATA);
}

const char *
kern_boot_parameters_init_path(const struct kern_boot_parameters *parameters)
{
	const char *path = kern_boot_parameters_value(parameters,
	    KERN_BOOT_PARAMETER_INIT);

	return path != NULL ? path : "/sbin/init";
}

unsigned
kern_boot_parameters_unknown_count(
	const struct kern_boot_parameters *parameters)
{
	return parameters != NULL ? parameters->unknown_count : 0U;
}

const char *
kern_boot_parameters_unknown_name(
	const struct kern_boot_parameters *parameters, int *truncated)
{
	if (truncated != NULL)
		*truncated = parameters != NULL &&
		    parameters->unknown_name_truncated != 0U;
	if (parameters == NULL || parameters->unknown_count == 0U)
		return NULL;
	return parameters->unknown_name;
}

int
kern_boot_parameters_initialize(const char *input, size_t input_capacity)
{
	int error = kern_boot_parameters_parse(&current_parameters, input,
	    input_capacity);

	current_parameters_valid = error == 0;
	current_parameters_source_present = error == 0 && input != NULL;
	return error;
}

const struct kern_boot_parameters *
kern_boot_parameters_current(void)
{
	return current_parameters_valid ? &current_parameters : NULL;
}

int
kern_boot_parameters_source_present(void)
{
	return current_parameters_valid && current_parameters_source_present;
}
