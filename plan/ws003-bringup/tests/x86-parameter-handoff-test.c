/* Four-path bounded x86 parameter handoff regression (BR-T43). */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <boot/pc98-handoff.h>
#include "bootloader/include/amd64-handoff.h"
#include "bootloader/uefi/load-options.h"
#include "src/hal/amd64/bsp-pcat/handoff-validation.h"
#include "src/hal/x86/boot-parameters.h"

static const char explicit_text[] =
    "overlay-root=boot0:other.img overlay-data=boot0:work.img "
    "init=/bin/sh";

static void
make_record(struct zedbsd_boot_parameter_record *record, const char *text)
{
	size_t length = strlen(text);

	memset(record, 0, sizeof(*record));
	record->magic = ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC;
	record->version = ZEDBSD_BOOT_PARAMETER_RECORD_VERSION;
	record->size = sizeof(*record);
	record->flags = ZEDBSD_BOOT_PARAMETER_RECORD_FLAG_TEXT;
	record->length = (uint16_t)length;
	memcpy(record->text, text, length + 1U);
}

static void
test_bounded_copy(void)
{
	char destination[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE];
	char source[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE];

	assert(x86_boot_parameters_copy(destination, NULL, 0U) ==
	    X86_BOOT_PARAMETERS_OK);
	assert(strcmp(destination, ZEDBSD_BOOT_PARAMETERS_DEFAULT_TEXT) == 0);
	assert(x86_boot_parameters_copy(destination, "", 1U) ==
	    X86_BOOT_PARAMETERS_OK);
	assert(strcmp(destination, ZEDBSD_BOOT_PARAMETERS_DEFAULT_TEXT) == 0);

	memcpy(source, explicit_text, sizeof(explicit_text));
	assert(x86_boot_parameters_copy(destination, source, sizeof(source)) ==
	    X86_BOOT_PARAMETERS_OK);
	memset(source, 'x', sizeof(explicit_text));
	assert(strcmp(destination, explicit_text) == 0);

	memset(source, 'a', sizeof(source));
	source[ZEDBSD_BOOT_PARAMETERS_TEXT_MAX] = '\0';
	assert(x86_boot_parameters_copy(destination, source, sizeof(source)) ==
	    X86_BOOT_PARAMETERS_OK);
	assert(destination[ZEDBSD_BOOT_PARAMETERS_TEXT_MAX - 1U] == 'a');
	memset(source, 'a', sizeof(source));
	assert(x86_boot_parameters_copy(destination, source, sizeof(source)) ==
	    X86_BOOT_PARAMETERS_UNTERMINATED);
	source[0] = '\t';
	source[1] = '\0';
	assert(x86_boot_parameters_copy(destination, source, sizeof(source)) ==
	    X86_BOOT_PARAMETERS_NON_ASCII);
}

static void
test_record_validation(void)
{
	struct zedbsd_boot_parameter_record record;
	char destination[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE];

	make_record(&record, explicit_text);
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_OK);
	memset(record.text, 'z', record.length);
	assert(strcmp(destination, explicit_text) == 0);

	make_record(&record, explicit_text);
	record.magic ^= 1U;
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_INVALID_RECORD);
	make_record(&record, explicit_text);
	record.version++;
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_INVALID_RECORD);
	make_record(&record, explicit_text);
	record.size--;
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_INVALID_RECORD);
	make_record(&record, explicit_text);
	record.flags |= 2U;
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_INVALID_RECORD);
	make_record(&record, explicit_text);
	record.reserved = 1U;
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_INVALID_RECORD);
	make_record(&record, explicit_text);
	record.length = ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE;
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_INVALID_RECORD);
	make_record(&record, explicit_text);
	record.text[record.length] = 'x';
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_INVALID_RECORD);
	make_record(&record, explicit_text);
	record.text[1] = '\0';
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_INVALID_RECORD);
	make_record(&record, explicit_text);
	record.text[1] = (char)0x80;
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record)) == X86_BOOT_PARAMETERS_NON_ASCII);
	make_record(&record, explicit_text);
	assert(x86_boot_parameter_record_copy(destination, &record,
	    sizeof(record) - 1U) == X86_BOOT_PARAMETERS_INVALID_RECORD);
}

static void
test_four_layouts(void)
{
	struct zedbsd_pc98_parameter_handoff pc98;
	struct zbl6_handoff_v5_bios bios;
	struct zbl6_handoff_v5_uefi uefi;
	char destination[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE];

	/* Multiboot has no declared length: its bounded pointer uses copy(). */
	assert(x86_boot_parameters_copy(destination, explicit_text,
	    sizeof(explicit_text)) == X86_BOOT_PARAMETERS_OK);

	memset(&pc98, 0, sizeof(pc98));
	pc98.common.magic = ZEDBSD_HANDOFF_MAGIC;
	pc98.common.version = ZEDBSD_HANDOFF_VERSION_PC98;
	pc98.common.size = sizeof(pc98);
	make_record(&pc98.parameters, explicit_text);
	assert(pc98.common.size == ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE);
	assert(x86_boot_parameter_record_copy(destination, &pc98.parameters,
	    sizeof(pc98.parameters)) == X86_BOOT_PARAMETERS_OK);

	memset(&bios, 0, sizeof(bios));
	bios.common.common.magic = ZBL6_HANDOFF_MAGIC;
	bios.common.common.version = ZBL6_HANDOFF_V5_VERSION;
	bios.common.common.size = sizeof(bios);
	bios.common.common.flags = ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;
	make_record(&bios.parameters, explicit_text);
	assert(bios.common.common.size == ZBL6_HANDOFF_V5_BIOS_SIZE);
	assert(x86_boot_parameter_record_copy(destination, &bios.parameters,
	    sizeof(bios.parameters)) == X86_BOOT_PARAMETERS_OK);

	memset(&uefi, 0, sizeof(uefi));
	uefi.common.common.magic = ZBL6_HANDOFF_MAGIC;
	uefi.common.common.version = ZBL6_HANDOFF_V5_VERSION;
	uefi.common.common.size = sizeof(uefi);
	uefi.common.common.flags = ZBL6_HANDOFF_FLAG_UEFI |
	    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;
	make_record(&uefi.parameters, explicit_text);
	assert(uefi.common.common.size == ZBL6_HANDOFF_V5_UEFI_SIZE);
	assert(x86_boot_parameter_record_copy(destination, &uefi.parameters,
	    sizeof(uefi.parameters)) == X86_BOOT_PARAMETERS_OK);

	/* Every supported old handoff selects the same compatibility default. */
	assert(x86_boot_parameters_copy(destination, NULL, 0U) ==
	    X86_BOOT_PARAMETERS_OK);
	assert(strcmp(destination, ZEDBSD_BOOT_PARAMETERS_DEFAULT_TEXT) == 0);
}

static void
test_zbl6_layout_validation(void)
{
	struct zbl6_handoff_v5_bios bios;
	struct zbl6_handoff_v5_uefi uefi_handoff;
	const uint32_t uefi = ZBL6_HANDOFF_FLAG_UEFI |
	    ZBL6_HANDOFF_FLAG_MEMORY_MAP | ZBL6_HANDOFF_FLAG_ACPI_RSDP;
	const uint32_t v5_uefi = uefi | ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
	    ZBL6_HANDOFF_FLAG_BOOT_UUID | ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;

	/* Supported compact ABIs keep their historical minimum-size/flag rule. */
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_VERSION,
	    ZBL6_HANDOFF_SIZE, 0U) == ZBL6_HANDOFF_FORM_LEGACY_BIOS);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_VERSION,
	    ZBL6_HANDOFF_FB_SIZE, UINT32_MAX) ==
	    ZBL6_HANDOFF_FORM_LEGACY_BIOS);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_VERSION,
	    ZBL6_HANDOFF_SIZE - 1U, 0U) == ZBL6_HANDOFF_FORM_INVALID);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V2_VERSION,
	    ZBL6_HANDOFF_V2_SIZE, uefi) == ZBL6_HANDOFF_FORM_LEGACY_UEFI);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V3_VERSION,
	    ZBL6_HANDOFF_V3_SIZE + 8U,
	    uefi | ZBL6_HANDOFF_FLAG_FRAMEBUFFER | (1U << 31)) ==
	    ZBL6_HANDOFF_FORM_LEGACY_UEFI);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V4_VERSION,
	    ZBL6_HANDOFF_V4_SIZE,
	    uefi | ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
	    ZBL6_HANDOFF_FLAG_BOOT_UUID) == ZBL6_HANDOFF_FORM_LEGACY_UEFI);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V4_VERSION,
	    ZBL6_HANDOFF_V4_SIZE,
	    uefi | ZBL6_HANDOFF_FLAG_FRAMEBUFFER) ==
	    ZBL6_HANDOFF_FORM_INVALID);

	/* v5 uses exact form sizes and rejects every unknown flag. */
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V5_VERSION,
	    ZBL6_HANDOFF_V5_BIOS_SIZE,
	    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS) == ZBL6_HANDOFF_FORM_V5_BIOS);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V5_VERSION,
	    ZBL6_HANDOFF_V5_BIOS_SIZE - 1U,
	    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS) == ZBL6_HANDOFF_FORM_INVALID);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V5_VERSION,
	    ZBL6_HANDOFF_V5_BIOS_SIZE,
	    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS | ZBL6_HANDOFF_FLAG_UEFI) ==
	    ZBL6_HANDOFF_FORM_INVALID);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V5_VERSION,
	    ZBL6_HANDOFF_V5_UEFI_SIZE, v5_uefi) ==
	    ZBL6_HANDOFF_FORM_V5_UEFI);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V5_VERSION,
	    ZBL6_HANDOFF_V5_UEFI_SIZE,
	    v5_uefi & ~ZBL6_HANDOFF_FLAG_ACPI_RSDP) ==
	    ZBL6_HANDOFF_FORM_INVALID);
	assert(zbl6_handoff_classify(ZBL6_HANDOFF_V5_VERSION,
	    ZBL6_HANDOFF_V5_UEFI_SIZE, v5_uefi | (1U << 31)) ==
	    ZBL6_HANDOFF_FORM_INVALID);

	/* The raw classifier selects the distinct BIOS/UEFI flag offsets. */
	memset(&bios, 0, sizeof(bios));
	bios.common.common.magic = ZBL6_HANDOFF_MAGIC;
	bios.common.common.version = ZBL6_HANDOFF_V5_VERSION;
	bios.common.common.size = sizeof(bios);
	bios.common.common.flags = ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;
	assert(zbl6_handoff_classify_raw(&bios) == ZBL6_HANDOFF_FORM_V5_BIOS);
	memset(&uefi_handoff, 0, sizeof(uefi_handoff));
	uefi_handoff.common.common.magic = ZBL6_HANDOFF_MAGIC;
	uefi_handoff.common.common.version = ZBL6_HANDOFF_V5_VERSION;
	uefi_handoff.common.common.size = sizeof(uefi_handoff);
	uefi_handoff.common.common.flags = v5_uefi;
	assert(zbl6_handoff_classify_raw(&uefi_handoff) ==
	    ZBL6_HANDOFF_FORM_V5_UEFI);
	uefi_handoff.common.common.magic = 0U;
	assert(zbl6_handoff_classify_raw(&uefi_handoff) ==
	    ZBL6_HANDOFF_FORM_INVALID);
}

static void
test_pc98_layout_validation(void)
{
	assert(x86_pc98_handoff_classify(ZEDBSD_HANDOFF_VERSION_PC98,
	    ZEDBSD_PC98_HANDOFF_COMMON_SIZE) == X86_PC98_HANDOFF_LEGACY);
	assert(x86_pc98_handoff_classify(ZEDBSD_HANDOFF_VERSION_MULTIBOOT,
	    ZEDBSD_PC98_HANDOFF_COMMON_SIZE) == X86_PC98_HANDOFF_LEGACY);
	assert(x86_pc98_handoff_classify(ZEDBSD_HANDOFF_VERSION_PC98,
	    ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE) ==
	    X86_PC98_HANDOFF_PARAMETERS);
	assert(x86_pc98_handoff_classify(ZEDBSD_HANDOFF_VERSION_PC98,
	    ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE - 1U) ==
	    X86_PC98_HANDOFF_INVALID);
	assert(x86_pc98_handoff_classify(ZEDBSD_HANDOFF_VERSION_MULTIBOOT,
	    ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE) == X86_PC98_HANDOFF_INVALID);
}

static void
test_uefi_conversion(void)
{
	struct zedbsd_boot_parameter_record record;
	uint16_t options[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE + 1U];
	const char fallback[] = ZEDBSD_BOOT_PARAMETERS_DEFAULT_TEXT;
	size_t length = strlen(explicit_text);

	assert(zbl_uefi_load_options_record(&record, NULL, 0U, fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_OK);
	assert(strcmp(record.text, fallback) == 0);
	for (size_t index = 0; index < length; index++)
		options[index] = (uint8_t)explicit_text[index];
	options[length] = 0;
	assert(zbl_uefi_load_options_record(&record, options,
	    (uint32_t)((length + 1U) * sizeof(options[0])), fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_OK);
	memset(options, 0x5a, (length + 1U) * sizeof(options[0]));
	assert(strcmp(record.text, explicit_text) == 0);

	options[0] = 0;
	assert(zbl_uefi_load_options_record(&record, options, 2U, fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_OK);
	assert(strcmp(record.text, fallback) == 0);
	assert(zbl_uefi_load_options_record(&record, options, 1U, fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_ODD_SIZE);
	assert(zbl_uefi_load_options_record(&record, NULL, 2U, fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_INVALID_ARGUMENT);
	options[0] = 'x';
	assert(zbl_uefi_load_options_record(&record, options, 2U, fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_MISSING_NUL);
	options[0] = 'x';
	options[1] = 0;
	options[2] = 'y';
	options[3] = 0;
	assert(zbl_uefi_load_options_record(&record, options, 8U, fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_EMBEDDED_NUL);
	options[0] = 0x80U;
	options[1] = 0;
	assert(zbl_uefi_load_options_record(&record, options, 4U, fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_NON_ASCII);

	for (size_t index = 0; index < ZEDBSD_BOOT_PARAMETERS_TEXT_MAX; index++)
		options[index] = 'a';
	options[ZEDBSD_BOOT_PARAMETERS_TEXT_MAX] = 0;
	assert(zbl_uefi_load_options_record(&record, options,
	    ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE * sizeof(options[0]), fallback,
	    sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_OK);
	options[ZEDBSD_BOOT_PARAMETERS_TEXT_MAX] = 'a';
	options[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE] = 0;
	assert(zbl_uefi_load_options_record(&record, options,
	    (ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE + 1U) * sizeof(options[0]),
	    fallback, sizeof(fallback)) == ZBL_UEFI_LOAD_OPTIONS_TOO_LONG);
}

int
main(void)
{
	test_bounded_copy();
	test_record_validation();
	test_four_layouts();
	test_zbl6_layout_validation();
	test_pc98_layout_validation();
	test_uefi_conversion();
	puts("BR-T43 x86 parameter handoff: PASS");
	return 0;
}
