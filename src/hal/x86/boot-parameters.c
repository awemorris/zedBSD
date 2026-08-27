/* Early x86 boot-parameter record validation and kernel-owned storage. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "boot-parameters.h"
#include <boot/pc98-handoff.h>

static void
copy_default(char destination[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE])
{
	static const char text[] = ZEDBSD_BOOT_PARAMETERS_DEFAULT_TEXT;
	size_t index;

	_Static_assert(sizeof(text) <= ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE,
	    "default boot parameters must fit transport storage");
	for (index = 0; index < sizeof(text); index++)
		destination[index] = text[index];
}

enum x86_boot_parameters_result
x86_boot_parameters_copy(
	char destination[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE],
	const char *source, size_t source_capacity)
{
	size_t length;

	if (destination == NULL || (source == NULL && source_capacity != 0U))
		return X86_BOOT_PARAMETERS_INVALID_ARGUMENT;
	if (source == NULL || source_capacity == 0U || source[0] == '\0') {
		copy_default(destination);
		return X86_BOOT_PARAMETERS_OK;
	}
	for (length = 0; length < source_capacity &&
	     length < ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE; length++) {
		unsigned char byte = (unsigned char)source[length];

		if (byte == 0U)
			break;
		if (byte < 0x20U || byte > 0x7eU)
			return X86_BOOT_PARAMETERS_NON_ASCII;
	}
	if (length == source_capacity ||
	    length == ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE)
		return X86_BOOT_PARAMETERS_UNTERMINATED;
	for (size_t index = 0; index <= length; index++)
		destination[index] = source[index];
	return X86_BOOT_PARAMETERS_OK;
}

enum x86_boot_parameters_result
x86_boot_parameter_record_copy(
	char destination[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE],
	const struct zedbsd_boot_parameter_record *record, size_t available)
{
	size_t index;

	if (destination == NULL || record == NULL)
		return X86_BOOT_PARAMETERS_INVALID_ARGUMENT;
	if (available < sizeof(*record) ||
	    record->magic != ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC ||
	    record->version != ZEDBSD_BOOT_PARAMETER_RECORD_VERSION ||
	    record->size != sizeof(*record) ||
	    record->flags != ZEDBSD_BOOT_PARAMETER_RECORD_FLAG_TEXT ||
	    record->reserved != 0U ||
	    record->length > ZEDBSD_BOOT_PARAMETERS_TEXT_MAX ||
	    record->text[record->length] != '\0')
		return X86_BOOT_PARAMETERS_INVALID_RECORD;
	for (index = 0; index < record->length; index++) {
		unsigned char byte = (unsigned char)record->text[index];

		if (byte == 0U)
			return X86_BOOT_PARAMETERS_INVALID_RECORD;
		if (byte < 0x20U || byte > 0x7eU)
			return X86_BOOT_PARAMETERS_NON_ASCII;
	}
	return x86_boot_parameters_copy(destination, record->text,
	    (size_t)record->length + 1U);
}

enum x86_pc98_handoff_form
x86_pc98_handoff_classify(uint16_t version, uint16_t size)
{
	if (version == ZEDBSD_HANDOFF_VERSION_PC98 &&
	    size == ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE)
		return X86_PC98_HANDOFF_PARAMETERS;
	if ((version == ZEDBSD_HANDOFF_VERSION_PC98 ||
	     version == ZEDBSD_HANDOFF_VERSION_MULTIBOOT) &&
	    size == ZEDBSD_PC98_HANDOFF_COMMON_SIZE)
		return X86_PC98_HANDOFF_LEGACY;
	return X86_PC98_HANDOFF_INVALID;
}
