/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali firmware file parser
 * (src/drivers/gpu/mali/mali-firmware-image.c) on images built here: a
 * good image of three sections (code, shared interface, protected) and a
 * build text, and the damaged images the parser must refuse (magic,
 * major version, size, misaligned section, data past the file, shared
 * region without its flag, unknown mandatory entry, no shared section).
 *
 * cc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *    tests/mali-firmware-host-test.c src/drivers/gpu/mali/mali-firmware-image.c -o <out> && <out>
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/mali/mali-firmware-image.h"

/* The largest image the test builds. */
#define IMAGE_BYTES	1024U

/* The image under construction and how much of it is written. */
static uint8_t image_bytes[IMAGE_BYTES];
static uint32_t image_length;

/* How many checks failed. */
static int failures;

static void check(int condition, const char *what);
static void put32(uint32_t offset, uint32_t value);
static void begin(uint32_t magic, uint8_t major);
static void add_section(uint32_t flags, uint32_t va_start, uint32_t va_end, uint32_t data_start, uint32_t data_end, const char *name);
static void add_entry(uint32_t type, uint32_t optional, uint32_t body_words);
static void finish(void);
static void build_good(void);
static void check_good(void);
static void check_damaged(void);

/* Runs every check and reports the count of failures. */
int
main(void)
{
	/* The good image, then the damaged ones. */
	check_good();
	check_damaged();

	/* Reports the outcome. */
	if (failures != 0) {
		printf("mali-firmware: %d failure(s)\n", failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("mali-firmware: all checks passed\n");
	return 0;
}

/* Counts and reports one failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A passing check says nothing. */
	if (condition)
		return;

	printf("FAIL: %s\n", what);
	failures++;
}

/* Writes a little-endian word into the image. */
static void
put32(
	uint32_t offset,
	uint32_t value)
{
	/* The lowest byte first. */
	image_bytes[offset] = (uint8_t)value;
	image_bytes[offset + 1U] = (uint8_t)(value >> 8);
	image_bytes[offset + 2U] = (uint8_t)(value >> 16);
	image_bytes[offset + 3U] = (uint8_t)(value >> 24);
}

/* Starts an image with its header; the size is written by finish(). */
static void
begin(
	uint32_t magic,
	uint8_t major)
{
	/* A cleared image and its 20-byte header. */
	memset(image_bytes, 0, sizeof(image_bytes));
	put32(0U, magic);
	image_bytes[4] = 1U;
	image_bytes[5] = major;
	put32(8U, 0x12345678U);
	image_length = 20U;
}

/* Appends one interface entry: its first word, the fixed part and a name. */
static void
add_section(
	uint32_t flags,
	uint32_t va_start,
	uint32_t va_end,
	uint32_t data_start,
	uint32_t data_end,
	const char *name)
{
	uint32_t name_bytes;
	uint32_t length;

	/* The name padded to whole words. */
	name_bytes = (uint32_t)strlen(name);
	length = 4U + 20U + ((name_bytes + 3U) & ~3U);

	/* The first word: type 0, the length. */
	put32(image_length, length << 8);
	put32(image_length + 4U, flags);
	put32(image_length + 8U, va_start);
	put32(image_length + 12U, va_end);
	put32(image_length + 16U, data_start);
	put32(image_length + 20U, data_end);
	memcpy(image_bytes + image_length + 24U, name, name_bytes);
	image_length += length;
}

/* Appends an entry of a type with a body of zero words. */
static void
add_entry(
	uint32_t type,
	uint32_t optional,
	uint32_t body_words)
{
	uint32_t length;

	/* The first word and the body. */
	length = 4U + body_words * 4U;
	put32(image_length, type | (length << 8) | optional);
	image_length += length;
}

/* Writes the filled size into the header; the file gets 256 bytes of data after it. */
static void
finish(void)
{
	/* The header's size covers the entries only. */
	put32(16U, image_length);
}

/* Builds the good image: code, the shared interface, a protected section and a build text. */
static void
build_good(void)
{
	/* The header and the three sections. */
	begin(0xc3f13a6eU, 0U);
	add_section(0x1U | 0x4U | 0x8U, 0x0U, 0x10000U, 0x300U, 0x340U, "code");
	add_section(0x1U | 0x2U | 0x40000000U | 0x80000000U | 0x10U, 0x04000000U, 0x04008000U, 0x340U, 0x350U, "shared");
	add_section(0x1U | 0x20U, 0x05000000U, 0x05001000U, 0x0U, 0x0U, "protected");

	/* A build information entry pointing at text inside the file. */
	put32(image_length, 6U | (12U << 8));
	put32(image_length + 4U, 0x380U);
	put32(image_length + 8U, 0x10U);
	image_length += 12U;

	/* A trace buffer entry the parser passes over. */
	add_entry(3U, 0U, 2U);
	finish();
}

/* Parses the good image and checks every field. */
static void
check_good(void)
{
	struct mali_fw_image image;
	int error;

	/* Builds and parses the image. */
	build_good();
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error == 0, "good image parsed");
	if (error != 0)
		return;

	/* The header. */
	check(image.major == 0U && image.minor == 1U, "header versions");
	check(image.version_hash == 0x12345678U, "version hash");

	/* Two loaded sections, the protected one skipped. */
	check(image.section_count == 2U, "two sections");
	check(image.skipped == 1U, "protected section skipped");
	check(image.shared == 1U, "shared section found");
	check(image.sections[0].va_end == 0x10000U, "code end");
	check(image.sections[0].data_start == 0x300U, "code data start");
	check(strcmp(image.sections[0].name, "code") == 0, "code name");
	check(strcmp(image.sections[1].name, "shared") == 0, "shared name");
	check((image.sections[1].flags & 0x80000000U) != 0U, "shared zero flag");

	/* The build text. */
	check(image.build_info_start == 0x380U && image.build_info_size == 0x10U, "build info");
}

/* Builds damaged images and checks each is refused. */
static void
check_damaged(void)
{
	struct mali_fw_image image;
	int error;

	/* A wrong magic. */
	build_good();
	put32(0U, 0xdeadbeefU);
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error != 0, "bad magic refused");

	/* An unknown major version. */
	build_good();
	image_bytes[5] = 1U;
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error != 0, "major 1 refused");

	/* A filled size larger than the file. */
	build_good();
	put32(16U, IMAGE_BYTES + 4U);
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error != 0, "size past the file refused");

	/* A file shorter than its header. */
	error = mali_fw_image_parse(image_bytes, 10U, 4096U, &image);
	check(error != 0, "short file refused");

	/* A section not on a page boundary. */
	begin(0xc3f13a6eU, 0U);
	add_section(0x1U, 0x800U, 0x2000U, 0x0U, 0x0U, "bad");
	add_section(0x1U | 0x40000000U, 0x04000000U, 0x04001000U, 0x0U, 0x0U, "shared");
	finish();
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error != 0, "misaligned section refused");

	/* Section data past the end of the file. */
	begin(0xc3f13a6eU, 0U);
	add_section(0x1U | 0x40000000U, 0x04000000U, 0x04001000U, 0x0U, IMAGE_BYTES + 16U, "shared");
	finish();
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error != 0, "data past the file refused");

	/* The shared region's start without the shared flag. */
	begin(0xc3f13a6eU, 0U);
	add_section(0x1U, 0x04000000U, 0x04001000U, 0x0U, 0x0U, "shared");
	finish();
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error != 0, "unflagged shared region refused");

	/* An unknown entry that may not be skipped, and one that may. */
	build_good();
	add_entry(9U, 0U, 1U);
	finish();
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error != 0, "unknown mandatory entry refused");
	build_good();
	add_entry(9U, 0x80000000U, 1U);
	finish();
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error == 0, "unknown optional entry skipped");

	/* No shared section at all. */
	begin(0xc3f13a6eU, 0U);
	add_section(0x1U, 0x0U, 0x1000U, 0x0U, 0x0U, "code");
	finish();
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 4096U, &image);
	check(error != 0, "no shared section refused");

	/* A section with more data than room. */
	begin(0xc3f13a6eU, 0U);
	add_section(0x1U | 0x40000000U, 0x04000000U, 0x04000100U, 0x0U, 0x200U, "shared");
	finish();
	error = mali_fw_image_parse(image_bytes, IMAGE_BYTES, 256U, &image);
	check(error != 0, "data larger than the section refused");
}
