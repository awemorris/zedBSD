/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the Mali command stream firmware file: its header, then each
 * entry in turn.  Every length and offset is checked against the file
 * before it is used, so a damaged file is refused rather than read past
 * its end.  The file is little-endian and read a byte at a time.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/mali/mali-firmware-image.h"

/*
 * The file header: a magic word, the header's minor and major version
 * (only major 0 is known), a hash of the firmware's version, and the
 * size of the part the entries fill.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (struct panthor_fw_binary_hdr, CSF_FW_BINARY_HEADER_MAGIC,
 * CSF_FW_BINARY_HEADER_MAJOR_MAX).
 */
#define HEADER_MAGIC		0xc3f13a6eU
#define HEADER_MAJOR_KNOWN	0U
#define HEADER_BYTES		20U
#define HEADER_MAGIC_AT		0U
#define HEADER_MINOR_AT		4U
#define HEADER_MAJOR_AT		5U
#define HEADER_HASH_AT		8U
#define HEADER_SIZE_AT		16U

/*
 * An entry's first word: the type in bits 0 to 7, the entry's length in
 * bytes (its first word included) in bits 8 to 15, and whether an
 * unknown entry of the type may be skipped in bit 31.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (CSF_FW_BINARY_ENTRY_TYPE, CSF_FW_BINARY_ENTRY_SIZE,
 * CSF_FW_BINARY_ENTRY_OPTIONAL and enum panthor_fw_binary_entry_type).
 */
#define ENTRY_TYPE_MASK		0xffU
#define ENTRY_LENGTH_SHIFT	8U
#define ENTRY_LENGTH_MASK	0xffU
#define ENTRY_OPTIONAL		0x80000000U
#define ENTRY_INTERFACE		0U
#define ENTRY_CONFIG		1U
#define ENTRY_TEST		2U
#define ENTRY_TRACE_BUFFER	3U
#define ENTRY_TIMELINE		4U
#define ENTRY_BUILD_INFO	6U

/*
 * An interface entry after its first word: the flags, the section's
 * virtual start and end, and the file range of its data; any bytes left
 * in the entry are the section's name.  A build information entry holds
 * the offset and length of the build text.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (struct panthor_fw_binary_section_entry_hdr,
 * struct panthor_fw_build_info_hdr, CSF_FW_BINARY_IFACE_ENTRY_SUPPORTED_FLAGS).
 */
#define SECTION_BYTES		20U
#define BUILD_INFO_BYTES	8U
#define SECTION_KNOWN_FLAGS	(MALI_FW_SECTION_READ | \
				 MALI_FW_SECTION_WRITE | \
				 MALI_FW_SECTION_EXECUTE | \
				 MALI_FW_SECTION_CACHE_MASK | \
				 MALI_FW_SECTION_PROTECTED | \
				 MALI_FW_SECTION_SHARED | \
				 MALI_FW_SECTION_ZERO)

/* No section index yet. */
#define NO_SECTION		0xffffffffU

static uint32_t read32(const uint8_t *bytes);
static int parse_entry(const uint8_t *bytes, size_t file_size, uint32_t offset, uint32_t limit, uint32_t page_bytes, struct mali_fw_image *image, uint32_t *next);
static int parse_section(size_t file_size, const uint8_t *entry, uint32_t length, uint32_t page_bytes, struct mali_fw_image *image);
static void parse_build_info(const uint8_t *entry, uint32_t length, size_t file_size, struct mali_fw_image *image);

/*
 * Reads and checks a whole firmware file.
 *
 * EINVAL names a damaged or unknown file, E2BIG one with more sections
 * than the driver keeps.  The image must have the shared interface's
 * section.
 */
int
mali_fw_image_parse(
	const uint8_t *bytes,
	size_t size,
	uint32_t page_bytes,
	struct mali_fw_image *image)
{
	uint32_t magic;
	uint32_t offset;
	uint32_t next;
	int error;

	/* Refuses a file shorter than its header, or no page size to check against. */
	if (bytes == NULL || image == NULL)
		return EINVAL;
	if (size < HEADER_BYTES || page_bytes == 0U)
		return EINVAL;
	if ((page_bytes & (page_bytes - 1U)) != 0U)
		return EINVAL;

	/* Reads the header: the magic, the versions, the hash and the filled size. */
	magic = read32(bytes + HEADER_MAGIC_AT);
	if (magic != HEADER_MAGIC)
		return EINVAL;

	image->minor = bytes[HEADER_MINOR_AT];
	image->major = bytes[HEADER_MAJOR_AT];
	image->version_hash = read32(bytes + HEADER_HASH_AT);
	image->size = read32(bytes + HEADER_SIZE_AT);
	image->section_count = 0U;
	image->shared = NO_SECTION;
	image->skipped = 0U;
	image->build_info_start = 0U;
	image->build_info_size = 0U;

	/* Refuses a header of another major version, or one that claims more than the file. */
	if (image->major != HEADER_MAJOR_KNOWN)
		return EINVAL;
	if (image->size > size || image->size < HEADER_BYTES)
		return EINVAL;

	/* Reads the entries one after another until the filled size is used up. */
	offset = HEADER_BYTES;
	while (offset < image->size) {
		error = parse_entry(bytes, size, offset, image->size, page_bytes, image, &next);
		if (error != 0)
			return error;
		offset = next;
	}

	/* Refuses an image without the shared interface the host talks through. */
	if (image->shared == NO_SECTION)
		return EINVAL;

	/* Succeeded: the image describes its sections. */
	return 0;
}

/* Reads a little-endian 32-bit word. */
static uint32_t
read32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The lowest byte first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Succeeded: the word. */
	return value;
}

/*
 * Reads one entry at an offset and reports where the next one starts.
 */
static int
parse_entry(
	const uint8_t *bytes,
	size_t file_size,
	uint32_t offset,
	uint32_t limit,
	uint32_t page_bytes,
	struct mali_fw_image *image,
	uint32_t *next)
{
	uint32_t word;
	uint32_t type;
	uint32_t length;
	const uint8_t *entry;
	int error;

	/* The entry's first word lies inside the filled part, on a word boundary. */
	if ((offset & 3U) != 0U)
		return EINVAL;
	if (limit - offset < 4U)
		return EINVAL;

	word = read32(bytes + offset);
	type = word & ENTRY_TYPE_MASK;
	length = (word >> ENTRY_LENGTH_SHIFT) & ENTRY_LENGTH_MASK;

	/* The length counts the first word and whole words, and stays inside the filled part. */
	if (length < 4U || (length & 3U) != 0U)
		return EINVAL;
	if (length > limit - offset)
		return EINVAL;

	/* The body follows the first word. */
	entry = bytes + offset + 4U;
	*next = offset + length;
	error = 0;

	/* Reads the entry by its type; the kinds the driver does not use are passed over. */
	switch (type) {
	case ENTRY_INTERFACE:
		error = parse_section(file_size, entry, length - 4U, page_bytes, image);
		break;
	case ENTRY_BUILD_INFO:
		parse_build_info(entry, length - 4U, file_size, image);
		break;
	case ENTRY_CONFIG:
	case ENTRY_TEST:
	case ENTRY_TRACE_BUFFER:
	case ENTRY_TIMELINE:
		break;
	default:
		/* An unknown entry is refused unless the file says it may be skipped. */
		if ((word & ENTRY_OPTIONAL) == 0U)
			error = EINVAL;
		break;
	}

	/* Reports a section the parser refused. */
	if (error != 0)
		return error;

	/* Succeeded: the entry is read. */
	return 0;
}

/*
 * Reads one interface entry: the section's flags, its virtual range, the
 * file range of its data, and its name.
 */
static int
parse_section(
	size_t file_size,
	const uint8_t *entry,
	uint32_t length,
	uint32_t page_bytes,
	struct mali_fw_image *image)
{
	struct mali_fw_section *section;
	uint32_t flags;
	uint32_t va_start;
	uint32_t va_end;
	uint32_t data_start;
	uint32_t data_end;
	uint32_t name_length;
	uint32_t index;

	/* The fixed part of the entry. */
	if (length < SECTION_BYTES)
		return EINVAL;

	flags = read32(entry);
	va_start = read32(entry + 4U);
	va_end = read32(entry + 8U);
	data_start = read32(entry + 12U);
	data_end = read32(entry + 16U);

	/* Both ranges run forwards, and the data lies inside the file. */
	if (data_end < data_start)
		return EINVAL;
	if (va_end < va_start)
		return EINVAL;
	if ((uint64_t)data_end > (uint64_t)file_size)
		return EINVAL;

	/* The section covers whole pages. */
	if ((va_start & (page_bytes - 1U)) != 0U)
		return EINVAL;
	if ((va_end & (page_bytes - 1U)) != 0U)
		return EINVAL;

	/* Every flag is one the driver knows. */
	if ((flags & ~SECTION_KNOWN_FLAGS) != 0U)
		return EINVAL;

	/* A protected-mode section is not loaded; protected mode is not used. */
	if ((flags & MALI_FW_SECTION_PROTECTED) != 0U) {
		image->skipped++;
		return 0;
	}

	/* The section at the shared region's start must be the shared interface. */
	if (va_start == MALI_FW_SHARED_START && (flags & MALI_FW_SECTION_SHARED) == 0U)
		return EINVAL;

	/* The data fits the section; an empty section is passed over. */
	if (data_end - data_start > va_end - va_start)
		return EINVAL;
	if (va_end == va_start)
		return 0;

	/* Room in the image's list. */
	if (image->section_count >= MALI_FW_SECTIONS_MAX)
		return E2BIG;

	/* Records the section. */
	index = image->section_count;
	section = &image->sections[index];
	section->flags = flags;
	section->va_start = va_start;
	section->va_end = va_end;
	section->data_start = data_start;
	section->data_end = data_end;

	/* Keeps as much of the name as fits, always terminated. */
	name_length = length - SECTION_BYTES;
	if (name_length > MALI_FW_NAME_BYTES - 1U)
		name_length = MALI_FW_NAME_BYTES - 1U;
	for (index = 0U; index < name_length; index++)
		section->name[index] = (char)entry[SECTION_BYTES + index];
	section->name[name_length] = '\0';

	/* The shared interface's section is the one the host talks through. */
	if (va_start == MALI_FW_SHARED_START)
		image->shared = image->section_count;

	/* Succeeded: one more section. */
	image->section_count++;
	return 0;
}

/*
 * Reads a build information entry; a damaged one is ignored, since the
 * build text is only reported.
 */
static void
parse_build_info(
	const uint8_t *entry,
	uint32_t length,
	size_t file_size,
	struct mali_fw_image *image)
{
	uint32_t start;
	uint32_t count;

	/* The offset and the length of the text. */
	if (length < BUILD_INFO_BYTES)
		return;

	start = read32(entry);
	count = read32(entry + 4U);

	/* Keeps the text only when it lies inside the file. */
	if ((uint64_t)start > (uint64_t)file_size)
		return;
	if ((uint64_t)count > (uint64_t)file_size - start)
		return;

	image->build_info_start = start;
	image->build_info_size = count;
}
