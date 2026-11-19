/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot information records the kernel reads after its image.
 *
 * Each record is a 16-bit tag, the 16-bit size of the whole record and
 * the data, big-endian; the list ends with a record of the tag 0.  The
 * loader keeps every record a multiple of four bytes long.  A record
 * that does not fit marks the list as overflowed and is left out.
 */

#include "bootloader/mac68k/stage2.h"

/*
 * The size of a record's header (and of the last record), and the tag of
 * the last record.
 *
 * Values referenced: Linux master arch/m68k/include/uapi/asm/bootinfo.h
 * (struct bi_record: the tag, the size of the whole record, the data;
 * BI_LAST).
 */
#define RECORD_HEADER		4U
#define TAG_LAST		0x0000U

static uint8_t *reserve_record(struct mac68k_bootinfo *info, uint16_t tag, uint32_t data_size);

/*
 * Starts an empty list of records in a buffer.
 */
void
mac68k_bootinfo_begin(
	struct mac68k_bootinfo *info,
	uint8_t *buffer,
	uint32_t capacity)
{
	/* Nothing written yet; the last record is kept room for. */
	info->buffer = buffer;
	info->capacity = capacity;
	info->used = 0;
	info->overflow = 0;
}

/*
 * Adds a record holding one 32-bit value.
 */
void
mac68k_bootinfo_add_long(
	struct mac68k_bootinfo *info,
	uint16_t tag,
	uint32_t value)
{
	uint8_t *data;

	/* Writes the value when the record fits. */
	data = reserve_record(info, tag, 4U);
	if (data == NULL)
		return;

	mac68k_write_be32(data, value);
}

/*
 * Adds a record holding two 32-bit values, such as a memory bank's base
 * and size.
 */
void
mac68k_bootinfo_add_pair(
	struct mac68k_bootinfo *info,
	uint16_t tag,
	uint32_t first,
	uint32_t second)
{
	uint8_t *data;

	/* Writes both values when the record fits. */
	data = reserve_record(info, tag, 8U);
	if (data == NULL)
		return;

	mac68k_write_be32(data, first);
	mac68k_write_be32(data + 4, second);
}

/*
 * Adds a record holding a string with its terminator.
 */
void
mac68k_bootinfo_add_text(
	struct mac68k_bootinfo *info,
	uint16_t tag,
	const char *text)
{
	uint8_t *data;
	uint32_t length;

	/* Writes the text and its terminator when the record fits. */
	length = mac68k_string_length(text) + 1U;
	data = reserve_record(info, tag, length);
	if (data == NULL)
		return;

	mac68k_copy_bytes(data, text, length);
}

/*
 * Ends the list with the last record, which always has room.
 */
void
mac68k_bootinfo_end(
	struct mac68k_bootinfo *info)
{
	uint8_t *record;

	/* The last record is a bare header of tag 0. */
	record = info->buffer + info->used;
	mac68k_write_be16(record, TAG_LAST);
	mac68k_write_be16(record + 2, 0);
	info->used += RECORD_HEADER;
}

/*
 * Reserves a record of a tag and a data size, padded to four bytes and
 * zeroed, and writes its header.  Reports where its data goes, or NULL
 * when it does not fit before the room kept for the last record.
 */
static uint8_t *
reserve_record(
	struct mac68k_bootinfo *info,
	uint16_t tag,
	uint32_t data_size)
{
	uint8_t *record;
	uint32_t size;

	/* The whole record, padded to four bytes. */
	size = (RECORD_HEADER + data_size + 3U) & ~3U;

	/* Refuses a record that would not leave room for the last one or for its size field. */
	if (size > 0xffffU ||
	    info->used + size + RECORD_HEADER > info->capacity) {
		info->overflow = 1;
		return NULL;
	}

	/* Writes the header over a zeroed record. */
	record = info->buffer + info->used;
	mac68k_fill_bytes(record, 0, size);
	mac68k_write_be16(record, tag);
	mac68k_write_be16(record + 2, (uint16_t)size);
	info->used += size;

	/* Succeeded: the data follows the header. */
	return record + RECORD_HEADER;
}
