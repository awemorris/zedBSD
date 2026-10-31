/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The harness's calls of the WMI layer (src/drivers/acpi/acpi-wmi.c) and
 * of methods with arguments.  Each call prints one line for its outcome
 * and one line with the writes of the SMI command port it caused, so a
 * test can tell that reading the WMI tables does not enter the firmware.
 *
 *   --wmi                      attach the mapping devices, list the blocks
 *   --wmi-path PATH            attach the device at PATH (a method _WDG too)
 *   --wmi-query GUID:I         query instance I of the block GUID
 *   --wmi-method GUID:I:M:HEX  call method M of instance I with the bytes
 *   --eval-args 'PATH ARG...'  evaluate PATH with arguments, each i:NUMBER,
 *                              s:TEXT or b:HEX (TEXT and HEX without spaces)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drivers/acpi/acpi.h>
#include <drivers/acpi/wmi.h>

#include "aml-host-hardware.h"
#include "aml-host-wmi.h"

/* The most arguments --eval-args passes, and the longest request it reads. */
#define EVAL_ARGUMENTS_MAX	7U
#define REQUEST_MAX		8192U

/* The longest path the harness prints. */
#define WMI_PATH_MAX		64U

/* The most bytes of a buffer printed before the rest is elided. */
#define PRINT_BYTES_MAX		256U

static void print_blocks(unsigned first);
static void print_guid(const uint8_t *guid);
static void print_object(const struct drv_acpi_object *object, unsigned depth);
static void print_smi(unsigned long before);
static int split_guid(const char *request, char *guid, const char **rest);
static int parse_hex(const char *text, uint8_t **bytes, size_t *length);
static struct drv_acpi_object *parse_argument(const char *text);

/*
 * Attaches the WMI mapping devices as the kernel does and lists the blocks.
 */
int
host_wmi_attach(void)
{
	unsigned long before;
	int error;

	/* Attaches them, counting the SMI writes on the way. */
	before = hardware_smi_writes();
	error = drv_acpi_wmi_attach();
	printf("WMI attach error %d\n", error);

	/* Lists every block and the SMI writes. */
	print_blocks(0);
	print_smi(before);

	/* Succeeded: ENODEV, no device, is an outcome the output shows. */
	return 0;
}

/*
 * Attaches the WMI mapping device at a path, a method _WDG too.
 */
int
host_wmi_attach_path(
	const char *path)
{
	unsigned long before;
	unsigned first;
	int error;

	/* Attaches it, counting the SMI writes on the way. */
	first = drv_acpi_wmi_block_count();
	before = hardware_smi_writes();
	error = drv_acpi_wmi_attach_path(path);
	printf("WMI attach-path %s error %d\n", path, error);

	/* Lists the blocks it added and the SMI writes. */
	print_blocks(first);
	print_smi(before);

	/* Reports a failed attach. */
	if (error != 0)
		return error;

	/* Succeeded: the device's blocks are listed. */
	return 0;
}

/*
 * Queries an instance of a data block: GUID:INSTANCE.
 */
int
host_wmi_query(
	const char *request)
{
	const struct drv_acpi_wmi_block *block;
	struct drv_acpi_object *result;
	char guid[DRV_ACPI_WMI_GUID_TEXT_LENGTH + 1U];
	const char *rest;
	unsigned long before;
	unsigned long instance;
	int error;

	/* Reads the GUID and the instance. */
	error = split_guid(request, guid, &rest);
	if (error != 0) {
		fprintf(stderr, "--wmi-query needs GUID:INSTANCE: %s\n", request);
		return error;
	}

	/* The instance after the colon. */
	instance = strtoul(rest, NULL, 0);

	/* Finds the block. */
	error = drv_acpi_wmi_find(guid, 0, &block);
	if (error != 0) {
		printf("WMI query %s = error %d\n", request, error);
		return error;
	}

	/* Queries it, counting the SMI writes on the way. */
	before = hardware_smi_writes();
	result = NULL;
	error = drv_acpi_wmi_query(block, (uint32_t)instance, &result);
	if (error != 0) {
		printf("WMI query %s = error %d\n", request, error);
	} else {
		printf("WMI query %s = ", request);
		print_object(result, 0);
		printf("\n");
		drv_acpi_object_release(result);
	}

	/* The SMI writes the query caused. */
	print_smi(before);

	/* Reports a failed query. */
	if (error != 0)
		return error;

	/* Succeeded: the contents are printed. */
	return 0;
}

/*
 * Calls a method of a method block: GUID:INSTANCE:METHOD:HEX.
 */
int
host_wmi_method(
	const char *request)
{
	const struct drv_acpi_wmi_block *block;
	struct drv_acpi_object *result;
	char guid[DRV_ACPI_WMI_GUID_TEXT_LENGTH + 1U];
	const char *rest;
	char *end;
	uint8_t *input;
	size_t size;
	unsigned long before;
	unsigned long instance;
	unsigned long method;
	int error;

	/* Reads the GUID, the instance, the method and the input. */
	error = split_guid(request, guid, &rest);
	if (error != 0) {
		fprintf(stderr, "--wmi-method needs GUID:INSTANCE:METHOD:HEX: %s\n", request);
		return error;
	}

	/* The instance, up to the next colon. */
	instance = strtoul(rest, &end, 0);
	if (*end != ':') {
		fprintf(stderr, "--wmi-method needs GUID:INSTANCE:METHOD:HEX: %s\n", request);
		return 1;
	}

	/* The method ID, up to the next colon. */
	method = strtoul(end + 1, &end, 0);
	if (*end != ':') {
		fprintf(stderr, "--wmi-method needs GUID:INSTANCE:METHOD:HEX: %s\n", request);
		return 1;
	}

	/* The input bytes. */
	error = parse_hex(end + 1, &input, &size);
	if (error != 0) {
		fprintf(stderr, "--wmi-method: the input is not hexadecimal bytes: %s\n", request);
		return error;
	}

	/* Finds the block. */
	error = drv_acpi_wmi_find(guid, 0, &block);
	if (error != 0) {
		printf("WMI method %s = error %d\n", request, error);
		free(input);
		return error;
	}

	/* Calls it, counting the SMI writes on the way. */
	before = hardware_smi_writes();
	result = NULL;
	error = drv_acpi_wmi_method(block, (uint32_t)instance, (uint32_t)method, input, size, &result);
	if (error != 0) {
		printf("WMI method %s = error %d\n", request, error);
	} else {
		printf("WMI method %s = ", request);
		print_object(result, 0);
		printf("\n");
		if (result != NULL)
			drv_acpi_object_release(result);
	}

	/* The SMI writes the call caused; the input is no longer needed. */
	print_smi(before);
	free(input);

	/* Reports a failed call. */
	if (error != 0)
		return error;

	/* Succeeded: the result is printed. */
	return 0;
}

/*
 * Evaluates a path with arguments: 'PATH ARG...'.
 */
int
host_eval_args(
	const char *request)
{
	struct drv_acpi_object *arguments[EVAL_ARGUMENTS_MAX];
	struct drv_acpi_object *result;
	char copy[REQUEST_MAX];
	size_t length;
	char *path;
	char *word;
	char *cursor;
	unsigned long before;
	unsigned count;
	unsigned index;
	int error;

	/* Refuses a request longer than the copy. */
	length = strlen(request);
	if (length >= sizeof(copy)) {
		fprintf(stderr, "--eval-args: the request is too long\n");
		return 1;
	}

	/* Copies the request to cut it into words. */
	strcpy(copy, request);

	/* The path, then the arguments. */
	cursor = copy;
	path = strsep(&cursor, " ");
	count = 0;
	error = 0;
	while (cursor != NULL) {
		/* Takes the next word; runs of spaces give empty words. */
		word = strsep(&cursor, " ");
		if (*word == '\0')
			continue;

		/* Refuses more arguments than a method takes. */
		if (count >= EVAL_ARGUMENTS_MAX) {
			fprintf(stderr, "--eval-args: more than %u arguments\n", EVAL_ARGUMENTS_MAX);
			error = 1;
			break;
		}

		/* Makes the argument. */
		arguments[count] = parse_argument(word);
		if (arguments[count] == NULL) {
			fprintf(stderr, "--eval-args: the argument is not i:NUMBER, s:TEXT or b:HEX: %s\n", word);
			error = 1;
			break;
		}

		/* The argument is kept. */
		count++;
	}

	/* Evaluates the path, counting the SMI writes on the way. */
	if (error == 0) {
		before = hardware_smi_writes();
		result = NULL;
		error = drv_acpi_evaluate(NULL, path, arguments, count, &result);
		if (error != 0) {
			printf("%s = error %d\n", request, error);
		} else {
			printf("%s = ", request);
			print_object(result, 0);
			printf("\n");
			if (result != NULL)
				drv_acpi_object_release(result);
		}

		/* The SMI writes the evaluation caused. */
		print_smi(before);
	}

	/* Gives the arguments back. */
	for (index = 0; index < count; index++)
		drv_acpi_object_release(arguments[index]);

	/* Reports a failed evaluation. */
	if (error != 0)
		return error;

	/* Succeeded: the result is printed. */
	return 0;
}

/* Prints the listed blocks from index first on, and how many there are. */
static void
print_blocks(
	unsigned first)
{
	const struct drv_acpi_wmi_block *block;
	char path[WMI_PATH_MAX];
	unsigned count;
	unsigned index;
	int error;

	/* Prints one line for each block. */
	count = drv_acpi_wmi_block_count();
	for (index = first; index < count; index++) {
		/* The device's path. */
		block = drv_acpi_wmi_block_at(index);
		error = drv_acpi_node_path(block->device, path, sizeof(path));
		if (error != 0)
			strcpy(path, "?");

		/* The GUID, the identifier (an event's Notify value in hexadecimal), the instances and the flags. */
		printf("WMI block %s ", path);
		print_guid(block->entry.guid);
		if ((block->entry.flags & DRV_ACPI_WMI_FLAG_EVENT) != 0U) {
			printf(" notify 0x%02x", block->entry.id[0]);
		} else {
			printf(" id %c%c", block->entry.id[0], block->entry.id[1]);
		}

		/* The instances and the flags. */
		printf(" instances %u flags 0x%02x\n", block->entry.instance_count, block->entry.flags);
	}

	/* The count. */
	printf("WMI blocks %u\n", count);
}

/* Prints a GUID in its text form from _WDG's bytes. */
static void
print_guid(
	const uint8_t *guid)
{
	/* Data1, Data2 and Data3 little-endian, then Data4's bytes in order. */
	printf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
	       guid[3], guid[2], guid[1], guid[0],
	       guid[5], guid[4],
	       guid[7], guid[6],
	       guid[8], guid[9],
	       guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

/* Prints an object on the current line. */
static void
print_object(
	const struct drv_acpi_object *object,
	unsigned depth)
{
	const uint8_t *bytes;
	const char *text;
	size_t length;
	size_t index;
	unsigned count;
	unsigned element;

	/* A method that returned nothing. */
	if (object == NULL) {
		printf("nothing");
		return;
	}

	/* Prints the object by its type. */
	switch (drv_acpi_object_type(object)) {
	case DRV_ACPI_TYPE_INTEGER:
		printf("Integer 0x%llx", (unsigned long long)drv_acpi_object_integer(object));
		break;
	case DRV_ACPI_TYPE_STRING:
		text = drv_acpi_object_string(object, NULL);
		printf("String \"%s\"", text);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		/* The length, then the bytes up to PRINT_BYTES_MAX. */
		bytes = drv_acpi_object_buffer(object, &length);
		printf("Buffer len=%zu {", length);
		for (index = 0; index < length && index < PRINT_BYTES_MAX; index++)
			printf(" %02x", bytes[index]);
		if (length > PRINT_BYTES_MAX)
			printf(" ...");

		/* The end of the bytes. */
		printf(" }");
		break;
	case DRV_ACPI_TYPE_PACKAGE:
		/* The count, then each element, nested ones too. */
		count = drv_acpi_object_package_count(object);
		printf("Package [%u] {", count);
		for (element = 0; element < count; element++) {
			printf(" ");
			print_object(drv_acpi_object_package_element(object, element), depth + 1U);
			if (element + 1U < count)
				printf(",");
		}

		/* The end of the elements. */
		printf(" }");
		break;
	default:
		printf("type %u", (unsigned)drv_acpi_object_type(object));
		break;
	}
}

/* Prints how many SMI command writes happened since before. */
static void
print_smi(
	unsigned long before)
{
	/* The writes the call caused. */
	printf("SMI writes %lu\n", hardware_smi_writes() - before);
}

/* Copies the GUID before the first colon of a request and points rest after it. */
static int
split_guid(
	const char *request,
	char *guid,
	const char **rest)
{
	const char *colon;
	size_t length;

	/* The GUID is everything before the first colon. */
	colon = strchr(request, ':');
	if (colon == NULL)
		return 1;
	length = (size_t)(colon - request);
	if (length != DRV_ACPI_WMI_GUID_TEXT_LENGTH)
		return 1;

	/* Copies it and points at what follows. */
	memcpy(guid, request, length);
	guid[length] = '\0';
	*rest = colon + 1;

	/* Succeeded: guid holds the GUID's text. */
	return 0;
}

/* Reads bytes written as pairs of hexadecimal digits; an empty text is no byte. */
static int
parse_hex(
	const char *text,
	uint8_t **bytes,
	size_t *length)
{
	char pair[3];
	char *end;
	size_t digits;
	size_t index;

	/* Refuses an odd number of digits. */
	*bytes = NULL;
	*length = 0;
	digits = strlen(text);
	if (digits % 2U != 0U)
		return 1;

	/* Allocates the bytes (one at least, so that free is always right). */
	*bytes = malloc(digits / 2U + 1U);
	if (*bytes == NULL)
		return 1;

	/* Reads each pair. */
	pair[2] = '\0';
	for (index = 0; index < digits / 2U; index++) {
		pair[0] = text[index * 2U];
		pair[1] = text[index * 2U + 1U];
		(*bytes)[index] = (uint8_t)strtoul(pair, &end, 16);
		if (*end != '\0') {
			free(*bytes);
			*bytes = NULL;
			return 1;
		}
	}

	/* Says how many bytes were read. */
	*length = digits / 2U;

	/* Succeeded: bytes holds length bytes. */
	return 0;
}

/* Makes one --eval-args argument: i:NUMBER, s:TEXT or b:HEX; NULL for anything else. */
static struct drv_acpi_object *
parse_argument(
	const char *text)
{
	struct drv_acpi_object *object;
	uint8_t *bytes;
	size_t length;
	size_t size;
	char *end;
	unsigned long long value;
	int error;

	/* Refuses a word without a type. */
	length = strlen(text);
	if (length < 2U || text[1] != ':')
		return NULL;

	/* Makes the object of the type. */
	object = NULL;
	switch (text[0]) {
	case 'i':
		value = strtoull(text + 2, &end, 0);
		if (*end == '\0')
			object = drv_acpi_object_integer_new(value);
		break;
	case 's':
		object = drv_acpi_object_string_new(text + 2);
		break;
	case 'b':
		error = parse_hex(text + 2, &bytes, &size);
		if (error != 0)
			break;
		object = drv_acpi_object_buffer_new(bytes, size);
		free(bytes);
		break;
	default:
		break;
	}

	/* Reports the object, or NULL. */
	return object;
}
