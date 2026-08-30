/* Host fixture for the bounded UEFI zedbsd.cfg parser. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "bootloader/uefi/zedbsd-config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static const char selected_uuid[] = "6740-911D";

static void
require(int condition, const char *test, const char *detail)
{
	if (condition)
		return;
	fprintf(stderr, "zedbsd-config-host-test: %s: %s\n", test, detail);
	exit(EXIT_FAILURE);
}

static int
all_zero(const void *object, size_t size)
{
	const unsigned char *bytes = object;

	for (size_t index = 0U; index < size; index++)
		if (bytes[index] != 0U)
			return 0;
	return 1;
}

static enum zbl_uefi_zedbsd_config_result
parse(struct zbl_uefi_zedbsd_config *configuration, const void *source,
	      size_t source_size)
{
	return zbl_uefi_zedbsd_config_parse(configuration, source, source_size,
	    selected_uuid, sizeof(selected_uuid));
}

static void
check_success(const struct zbl_uefi_zedbsd_config *configuration,
	      const char *kernel, const char *parameters, const char *test)
{
	const struct zedbsd_boot_parameter_record *record =
	    &configuration->parameter_record;
	size_t length = strlen(parameters);

	require(strcmp(configuration->kernel_path, kernel) == 0, test,
	    "wrong normalized kernel path");
	require(record->magic == ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC, test,
	    "wrong record magic");
	require(record->version == ZEDBSD_BOOT_PARAMETER_RECORD_VERSION, test,
	    "wrong record version");
	require(record->size == sizeof(*record), test, "wrong record size");
	require(record->flags == ZEDBSD_BOOT_PARAMETER_RECORD_FLAG_TEXT, test,
	    "wrong record flags");
	require(record->length == length, test, "wrong record length");
	require(record->reserved == 0U, test, "nonzero reserved field");
	require(strcmp(record->text, parameters) == 0, test,
	    "wrong parameter text");
	require(record->text[length] == '\0', test, "record is not terminated");
	require(record->text[ZEDBSD_BOOT_PARAMETERS_TEXT_MAX] == '\0', test,
	    "unused record storage was not cleared");
}

static void
expect_ok(const char *source, const char *kernel, const char *parameters,
	  const char *test)
{
	struct zbl_uefi_zedbsd_config configuration;
	enum zbl_uefi_zedbsd_config_result result;

	memset(&configuration, 0xa5, sizeof(configuration));
	result = parse(&configuration, source, strlen(source));
	if (result != ZBL_UEFI_ZEDBSD_CONFIG_OK) {
		fprintf(stderr, "zedbsd-config-host-test: %s: got %s\n", test,
		    zbl_uefi_zedbsd_config_result_name(result));
		exit(EXIT_FAILURE);
	}
	check_success(&configuration, kernel, parameters, test);
}

static void
expect_error_bytes(const void *source, size_t source_size,
		   enum zbl_uefi_zedbsd_config_result expected,
		   const char *test)
{
	struct zbl_uefi_zedbsd_config configuration;
	enum zbl_uefi_zedbsd_config_result result;

	memset(&configuration, 0xa5, sizeof(configuration));
	result = parse(&configuration, source, source_size);
	if (result != expected) {
		fprintf(stderr,
		    "zedbsd-config-host-test: %s: expected %s, got %s\n",
		    test, zbl_uefi_zedbsd_config_result_name(expected),
		    zbl_uefi_zedbsd_config_result_name(result));
		exit(EXIT_FAILURE);
	}
	require(all_zero(&configuration, sizeof(configuration)), test,
	    "failed parse retained partial output");
}

static void
expect_error(const char *source,
	     enum zbl_uefi_zedbsd_config_result expected, const char *test)
{
	expect_error_bytes(source, strlen(source), expected, test);
}

static void
append_text(unsigned char *buffer, size_t capacity, size_t *position,
	    const char *text, const char *test)
{
	size_t length = strlen(text);

	require(length <= capacity - *position, test, "fixture buffer overflow");
	memcpy(buffer + *position, text, length);
	*position += length;
}

static void
append_repeat(unsigned char *buffer, size_t capacity, size_t *position,
	      unsigned char byte, size_t count, const char *test)
{
	require(count <= capacity - *position, test, "fixture buffer overflow");
	memset(buffer + *position, byte, count);
	*position += count;
}

static void
test_normalization(void)
{
	static const char compact[] =
	    "kernel=vmunix\n"
	    "overlay-root=rootfs.img\n"
	    "overlay-data=data.img\n"
	    "swap0=swapfile";
	static const char compact_parameters[] =
	    "boot0=UUID=6740-911D overlay-root=boot0:rootfs.img "
	    "overlay-data=boot0:data.img swap0=boot0:swapfile";
	static const char native_crlf[] =
	    "\r\nkernel=/kernels/vmunix\r\n\r\n"
	    "rootpart=PARTUUID=01234567-02\r\n"
	    "future=value=with=equals";
	static const char native_parameters[] =
	    "boot0=UUID=6740-911D rootpart=PARTUUID=01234567-02 "
	    "future=value=with=equals";
	static const char explicit[] =
	    "kernel=/vmunix\n"
	    "boot2=LABEL=ALT\n"
	    "boot0=LABEL=BOOT\n"
	    "overlay-root=boot0:root.img\n"
	    "overlay-data=boot3:/data.img\n"
	    "init=/bin/sh\n"
	    "unknown=x";
	static const char explicit_parameters[] =
	    "boot2=LABEL=ALT boot0=LABEL=BOOT overlay-root=boot0:root.img "
	    "overlay-data=boot3:/data.img init=/bin/sh unknown=x";
	static const char swaps[] =
	    "kernel=k\n"
	    "swap0=zero.img\n"
	    "swap1=/one.img\n"
	    "swap2=boot2:two.img\n"
	    "swap3=sda1";
	static const char swap_parameters[] =
	    "boot0=UUID=6740-911D swap0=boot0:zero.img "
	    "swap1=boot0:/one.img swap2=boot2:two.img "
	    "swap3=boot0:sda1";
	static const struct {
		const char *selector;
		const char *expected;
	} raw[] = {
		{ "/dev/sda1", "boot0=UUID=6740-911D swap0=/dev/sda1" },
		{ "UUID=ABCD", "boot0=UUID=6740-911D swap0=UUID=ABCD" },
		{ "LABEL=SWAP", "boot0=UUID=6740-911D swap0=LABEL=SWAP" },
		{ "PARTUUID=0123", "boot0=UUID=6740-911D swap0=PARTUUID=0123" },
		{ "PARTLABEL=SWAP", "boot0=UUID=6740-911D swap0=PARTLABEL=SWAP" },
	};
	char input[128];

	expect_ok(compact, "vmunix", compact_parameters, "compact overlay");
	expect_ok(native_crlf, "kernels/vmunix", native_parameters,
	    "CRLF native UFS and final line");
	expect_ok(explicit, "vmunix", explicit_parameters,
	    "explicit boot slots and unknown parameter");
	expect_ok(swaps, "k", swap_parameters, "four swap slots");
	expect_ok("kernel=k\noverlay-root=/images/root.img",
	    "k", "boot0=UUID=6740-911D overlay-root=boot0:/images/root.img",
	    "rooted selected-volume shorthand");
	expect_ok("kernel=k\noverlay-root=boot3:../defer-to-kernel",
	    "k",
	    "boot0=UUID=6740-911D overlay-root=boot3:../defer-to-kernel",
	    "explicit reference preserved for common validation");
	for (size_t index = 0U; index < ARRAY_COUNT(raw); index++) {
		int written = snprintf(input, sizeof(input), "kernel=k\nswap0=%s",
		    raw[index].selector);

		require(written > 0 && (size_t)written < sizeof(input),
		    "raw selector fixture", "snprintf failed");
		expect_ok(input, "k", raw[index].expected, "raw swap selector");
	}
}

static void
test_invalid_grammar(void)
{
	static const struct {
		const char *source;
		enum zbl_uefi_zedbsd_config_result result;
		const char *name;
	} cases[] = {
		{ "", ZBL_UEFI_ZEDBSD_CONFIG_MISSING_KERNEL, "empty file" },
		{ "\n\n", ZBL_UEFI_ZEDBSD_CONFIG_MISSING_KERNEL,
		    "only empty lines" },
		{ "rootpart=x", ZBL_UEFI_ZEDBSD_CONFIG_MISSING_KERNEL,
		    "missing kernel" },
		{ "kernel=", ZBL_UEFI_ZEDBSD_CONFIG_MALFORMED_LINE,
		    "empty kernel value" },
		{ "=value\nkernel=k", ZBL_UEFI_ZEDBSD_CONFIG_MALFORMED_LINE,
		    "empty name" },
		{ "value\nkernel=k", ZBL_UEFI_ZEDBSD_CONFIG_MALFORMED_LINE,
		    "missing equals" },
		{ "kernel=k\nname=", ZBL_UEFI_ZEDBSD_CONFIG_MALFORMED_LINE,
		    "empty value" },
		{ "kernel=a\nkernel=b", ZBL_UEFI_ZEDBSD_CONFIG_DUPLICATE_KERNEL,
		    "duplicate kernel" },
		{ " kernel=k", ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER,
		    "leading space" },
		{ "kernel=k ", ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER,
		    "trailing space" },
		{ "kernel=k\nname=two words",
		    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER, "embedded space" },
		{ "kernel=k\nname=tab\tvalue",
		    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER, "tab" },
		{ "kernel=k\r", ZBL_UEFI_ZEDBSD_CONFIG_INVALID_LINE_ENDING,
		    "bare CR" },
		{ "kernel=k\r\r\n", ZBL_UEFI_ZEDBSD_CONFIG_INVALID_LINE_ENDING,
		    "double CR" },
		{ "kernel=k\n#comment",
		    ZBL_UEFI_ZEDBSD_CONFIG_UNSUPPORTED_SYNTAX, "comment" },
		{ "kernel=k\nname=value#comment",
		    ZBL_UEFI_ZEDBSD_CONFIG_UNSUPPORTED_SYNTAX,
		    "inline comment" },
		{ "kernel=k\nname=\"value\"",
		    ZBL_UEFI_ZEDBSD_CONFIG_UNSUPPORTED_SYNTAX, "double quote" },
		{ "kernel=k\nname='value'",
		    ZBL_UEFI_ZEDBSD_CONFIG_UNSUPPORTED_SYNTAX, "single quote" },
		{ "kernel=k\nname=value\\\nnext=value",
		    ZBL_UEFI_ZEDBSD_CONFIG_UNSUPPORTED_SYNTAX, "continuation" },
		{ "kernel=k\n[section]", ZBL_UEFI_ZEDBSD_CONFIG_MALFORMED_LINE,
		    "section" },
	};
	static const unsigned char embedded_nul[] = {
		'k', 'e', 'r', 'n', 'e', 'l', '=', 'k', '\n',
		'n', 'a', 'm', 'e', '=', 0U, 'x',
	};
	static const unsigned char non_ascii[] = {
		'k', 'e', 'r', 'n', 'e', 'l', '=', 'k', '\n',
		'n', 'a', 'm', 'e', '=', 0x80U,
	};
	static const unsigned char bom[] = {
		0xefU, 0xbbU, 0xbfU, 'k', 'e', 'r', 'n', 'e', 'l', '=', 'k',
	};

	for (size_t index = 0U; index < ARRAY_COUNT(cases); index++)
		expect_error(cases[index].source, cases[index].result,
		    cases[index].name);
	expect_error_bytes(embedded_nul, sizeof(embedded_nul),
	    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER, "embedded NUL");
	expect_error_bytes(non_ascii, sizeof(non_ascii),
	    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER, "non-ASCII byte");
	expect_error_bytes(bom, sizeof(bom),
	    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER, "UTF-8 BOM");
}

static void
test_kernel_paths(void)
{
	static const char *const invalid_paths[] = {
		"/", ".", "..", "a//b", "a/./b", "a/../b", "a/",
		"//vmunix", "/dev/vmunix", "dev/vmunix", "boot0:vmunix",
		"fs0:vmunix", "UUID=ABCD", "LABEL=BOOT",
		"PARTUUID=0123", "PARTLABEL=BOOT",
	};
	unsigned char input[600];
	size_t position;
	struct zbl_uefi_zedbsd_config configuration;
	enum zbl_uefi_zedbsd_config_result result;

	expect_ok("kernel=/safe/subdir/vmunix", "safe/subdir/vmunix",
	    "boot0=UUID=6740-911D", "safe kernel subdirectory");
	for (size_t index = 0U; index < ARRAY_COUNT(invalid_paths); index++) {
		char source[320];
		int written = snprintf(source, sizeof(source), "kernel=%s",
		    invalid_paths[index]);

		require(written > 0 && (size_t)written < sizeof(source),
		    "kernel path fixture", "snprintf failed");
		expect_error(source, ZBL_UEFI_ZEDBSD_CONFIG_INVALID_KERNEL_PATH,
		    "unsafe kernel path");
	}

	position = 0U;
	append_text(input, sizeof(input), &position, "kernel=/",
	    "maximum kernel path");
	append_repeat(input, sizeof(input), &position, 'a',
	    ZBL_ZEDBSD_CONFIG_KERNEL_PATH_MAX, "maximum kernel path");
	result = parse(&configuration, input, position);
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_OK, "maximum kernel path",
	    zbl_uefi_zedbsd_config_result_name(result));
	require(strlen(configuration.kernel_path) ==
	    ZBL_ZEDBSD_CONFIG_KERNEL_PATH_MAX, "maximum kernel path",
	    "normalization changed boundary length");

	position = 0U;
	append_text(input, sizeof(input), &position, "kernel=",
	    "oversized kernel path");
	append_repeat(input, sizeof(input), &position, 'a',
	    ZBL_ZEDBSD_CONFIG_KERNEL_PATH_MAX + 1U, "oversized kernel path");
	expect_error_bytes(input, position,
	    ZBL_UEFI_ZEDBSD_CONFIG_KERNEL_PATH_TOO_LONG,
	    "oversized kernel path");
}

static void
test_parameter_paths(void)
{
	static const char *const invalid_values[] = {
		"/", ".", "..", "a//b", "a/./b", "a/../b", "a/",
	};
	unsigned char input[600];
	size_t position;

	for (size_t index = 0U; index < ARRAY_COUNT(invalid_values); index++) {
		char source[320];
		int written = snprintf(source, sizeof(source),
		    "kernel=k\noverlay-root=%s", invalid_values[index]);

		require(written > 0 && (size_t)written < sizeof(source),
		    "parameter path fixture", "snprintf failed");
		expect_error(source,
		    ZBL_UEFI_ZEDBSD_CONFIG_INVALID_PARAMETER_PATH,
		    "unsafe generated parameter path");
	}
	position = 0U;
	append_text(input, sizeof(input), &position, "kernel=k\nswap0=",
	    "oversized generated parameter path");
	append_repeat(input, sizeof(input), &position, 'a',
	    ZBL_ZEDBSD_CONFIG_KERNEL_PATH_MAX + 1U,
	    "oversized generated parameter path");
	expect_error_bytes(input, position,
	    ZBL_UEFI_ZEDBSD_CONFIG_PARAMETER_PATH_TOO_LONG,
	    "oversized generated parameter path");
}

static void
test_line_and_file_bounds(void)
{
	unsigned char input[ZBL_ZEDBSD_CONFIG_FILE_MAX + 1U];
	unsigned char expected[700];
	struct zbl_uefi_zedbsd_config configuration;
	enum zbl_uefi_zedbsd_config_result result;
	size_t position;
	size_t expected_position;

	position = 0U;
	append_text(input, sizeof(input), &position, "kernel=k\nfuture=",
	    "511-byte line");
	append_repeat(input, sizeof(input), &position, 'a', 504U,
	    "511-byte line");
	expected_position = 0U;
	append_text(expected, sizeof(expected), &expected_position,
	    "boot0=UUID=6740-911D future=", "511-byte line expected");
	append_repeat(expected, sizeof(expected), &expected_position, 'a', 504U,
	    "511-byte line expected");
	expected[expected_position] = '\0';
	result = parse(&configuration, input, position);
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_OK, "511-byte line",
	    zbl_uefi_zedbsd_config_result_name(result));
	check_success(&configuration, "k", (const char *)expected,
	    "511-byte line");
	append_repeat(input, sizeof(input), &position, 'a', 1U,
	    "512-byte line");
	expect_error_bytes(input, position, ZBL_UEFI_ZEDBSD_CONFIG_LINE_TOO_LONG,
	    "512-byte line");

	position = 0U;
	append_text(input, sizeof(input), &position, "kernel=k\n",
	    "64 physical lines");
	append_repeat(input, sizeof(input), &position, '\n', 63U,
	    "64 physical lines");
	result = parse(&configuration, input, position);
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_OK, "64 physical lines",
	    zbl_uefi_zedbsd_config_result_name(result));
	check_success(&configuration, "k", "boot0=UUID=6740-911D",
	    "64 physical lines");
	append_repeat(input, sizeof(input), &position, '\n', 1U,
	    "65 physical lines");
	expect_error_bytes(input, position,
	    ZBL_UEFI_ZEDBSD_CONFIG_TOO_MANY_LINES, "65 physical lines");

	/*
	 * A fully valid 4096-byte grammar cannot also fit the independent
	 * 3071-byte output cap.  This fixture reaches the file boundary with 64
	 * valid short lines, then fails at the later output-size check.
	 */
	position = 0U;
	append_text(input, sizeof(input), &position, "kernel=k\n",
	    "4096-byte file");
	for (unsigned line = 0U; line < 62U; line++) {
		append_text(input, sizeof(input), &position, "p=",
		    "4096-byte file");
		append_repeat(input, sizeof(input), &position, 'a', 62U,
		    "4096-byte file");
		append_repeat(input, sizeof(input), &position, '\n', 1U,
		    "4096-byte file");
	}
	append_text(input, sizeof(input), &position, "q=", "4096-byte file");
	append_repeat(input, sizeof(input), &position, 'a', 55U,
	    "4096-byte file");
	require(position == ZBL_ZEDBSD_CONFIG_FILE_MAX, "4096-byte file",
	    "fixture has wrong size");
	expect_error_bytes(input, position,
	    ZBL_UEFI_ZEDBSD_CONFIG_PARAMETERS_TOO_LONG, "4096-byte file");
	input[position] = 'a';
	expect_error_bytes(input, position + 1U,
	    ZBL_UEFI_ZEDBSD_CONFIG_FILE_TOO_LONG, "4097-byte file");
}

static size_t
make_record_edge(unsigned char *input, size_t capacity, size_t last_length,
		 const char *test)
{
	size_t position = 0U;

	append_text(input, capacity, &position, "kernel=k\n", test);
	for (unsigned line = 0U; line < 5U; line++) {
		char name[] = "p0=";

		name[1] = (char)('0' + line);
		append_text(input, capacity, &position, name, test);
		append_repeat(input, capacity, &position, 'a', 508U, test);
		append_repeat(input, capacity, &position, '\n', 1U, test);
	}
	append_text(input, capacity, &position, "p5=", test);
	require(last_length >= 3U, test, "last line is too short");
	append_repeat(input, capacity, &position, 'a', last_length - 3U, test);
	return position;
}

static void
test_record_bound(void)
{
	unsigned char input[4096];
	struct zbl_uefi_zedbsd_config configuration;
	enum zbl_uefi_zedbsd_config_result result;
	size_t size;

	size = make_record_edge(input, sizeof(input), 490U,
	    "3071-byte parameter record");
	result = parse(&configuration, input, size);
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_OK,
	    "3071-byte parameter record",
	    zbl_uefi_zedbsd_config_result_name(result));
	require(configuration.parameter_record.length ==
	    ZEDBSD_BOOT_PARAMETERS_TEXT_MAX, "3071-byte parameter record",
	    "record did not reach exact maximum");
	require(configuration.parameter_record.text[
	    ZEDBSD_BOOT_PARAMETERS_TEXT_MAX] == '\0',
	    "3071-byte parameter record", "maximum record lacks NUL");

	size = make_record_edge(input, sizeof(input), 491U,
	    "3072-byte parameter record");
	expect_error_bytes(input, size,
	    ZBL_UEFI_ZEDBSD_CONFIG_PARAMETERS_TOO_LONG,
	    "3072-byte parameter record");
}

static void
test_arguments_and_diagnostics(void)
{
	struct zbl_uefi_zedbsd_config configuration;
	static const char source[] = "kernel=k";
	static const char bad_uuid[] = "674Z-911D";
	enum zbl_uefi_zedbsd_config_result result;

	result = zbl_uefi_zedbsd_config_parse(NULL, source, sizeof(source) - 1U,
	    selected_uuid, sizeof(selected_uuid));
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_INVALID_ARGUMENT,
	    "NULL output", "wrong result");

	memset(&configuration, 0xa5, sizeof(configuration));
	result = zbl_uefi_zedbsd_config_parse(&configuration, NULL, 1U,
	    selected_uuid, sizeof(selected_uuid));
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_INVALID_ARGUMENT,
	    "NULL input with size", "wrong result");
	require(all_zero(&configuration, sizeof(configuration)),
	    "NULL input with size", "output not cleared");

	result = zbl_uefi_zedbsd_config_parse(&configuration, NULL, 0U,
	    selected_uuid, sizeof(selected_uuid));
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_MISSING_KERNEL,
	    "NULL empty input", "wrong result");

	result = zbl_uefi_zedbsd_config_parse(&configuration, source,
	    sizeof(source) - 1U, NULL, 0U);
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_INVALID_SELECTED_UUID,
	    "NULL selected UUID", "wrong result");
	result = zbl_uefi_zedbsd_config_parse(&configuration, source,
	    sizeof(source) - 1U, selected_uuid,
	    ZBL_ZEDBSD_CONFIG_FAT_UUID_LENGTH);
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_INVALID_SELECTED_UUID,
	    "unterminated selected UUID capacity", "wrong result");
	result = zbl_uefi_zedbsd_config_parse(&configuration, source,
	    sizeof(source) - 1U, bad_uuid, sizeof(bad_uuid));
	require(result == ZBL_UEFI_ZEDBSD_CONFIG_INVALID_SELECTED_UUID,
	    "malformed selected UUID", "wrong result");
	require(strcmp(zbl_uefi_zedbsd_config_result_name(
	    ZBL_UEFI_ZEDBSD_CONFIG_PARAMETERS_TOO_LONG),
	    "parameters-too-long") == 0, "diagnostic name", "wrong name");
	require(strcmp(zbl_uefi_zedbsd_config_result_name(
	    (enum zbl_uefi_zedbsd_config_result)999), "unknown") == 0,
	    "unknown diagnostic name", "wrong name");
}

int
main(void)
{
	test_normalization();
	test_invalid_grammar();
	test_kernel_paths();
	test_parameter_paths();
	test_line_and_file_bounds();
	test_record_bound();
	test_arguments_and_diagnostics();
	puts("CT-T008/T009/T010/T012/T013/T014/T015 zedbsd.cfg parser: PASS");
	return EXIT_SUCCESS;
}
