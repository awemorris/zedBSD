/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The report descriptor's reading for the operating system layer
 * (ws161-p004; os.h): whether a HID interface is a security key's, by its
 * first application collection (HID 1.11 sections 6.2.2.2 to 6.2.2.8).
 *
 * The items are walked in order.  Usage Page is a global item and stays;
 * Usage is a local item and is forgotten after every main item.  A Usage of
 * four bytes carries its own page in the high half.  The first Collection
 * whose data is Application (1) decides.  A long item is skipped, and a
 * descriptor that ends inside an item is not a key's.
 */

#include "os.h"

/* FIDO's usage page and the CTAPHID usage (FIDO CTAP 2.1 section 11.2.8.1). */
#define DESCRIPTOR_PAGE_FIDO	0xf1d0U
#define DESCRIPTOR_USAGE_CTAPHID	0x01U

/* The items looked at: their prefix without the size bits, and the long item's prefix. */
#define DESCRIPTOR_USAGE_PAGE	0x04U
#define DESCRIPTOR_USAGE	0x08U
#define DESCRIPTOR_COLLECTION	0xa0U
#define DESCRIPTOR_LONG		0xfeU

/* A collection's data that makes it an application collection. */
#define DESCRIPTOR_APPLICATION	0x01U

static uint32_t descriptor_value(const uint8_t *data, size_t size);

/*
 * Tells whether a report descriptor's first application collection is
 * FIDO's CTAPHID: 1 when it is, 0 otherwise.
 */
int
pk_os_descriptor_is_fido(
	const uint8_t *descriptor,
	size_t size)
{
	uint32_t page;
	uint32_t usage;
	uint32_t value;
	size_t offset;
	size_t length;
	uint8_t prefix;
	int has_usage;

	/* No page and no usage yet. */
	page = 0U;
	usage = 0U;
	has_usage = 0;

	/* Each item in turn, until the first application collection. */
	offset = 0U;
	while (offset < size) {
		prefix = descriptor[offset];

		/* A long item: its data's size is the next byte, then a tag byte. */
		if (prefix == DESCRIPTOR_LONG) {
			if (offset + 2U >= size)
				return 0;
			offset += 3U + (size_t)descriptor[offset + 1U];
			continue;
		}

		/* A short item's data: 0, 1, 2 or 4 bytes, within the descriptor. */
		length = (size_t)(prefix & 0x03U);
		if (length == 3U)
			length = 4U;
		if (offset + 1U + length > size)
			return 0;
		value = descriptor_value(descriptor + offset + 1U, length);
		offset += 1U + length;

		/* The page, kept for the items after it. */
		if ((prefix & 0xfcU) == DESCRIPTOR_USAGE_PAGE) {
			page = value;
			continue;
		}

		/* A usage: four bytes carry their own page. */
		if ((prefix & 0xfcU) == DESCRIPTOR_USAGE) {
			usage = value;
			if (length == 4U) {
				page = value >> 16;
				usage = value & 0xffffU;
			}

			/* A usage is known until the next main item. */
			has_usage = 1;
			continue;
		}

		/* Another main item ends the local items. */
		if ((prefix & 0xfcU) != DESCRIPTOR_COLLECTION) {
			if ((prefix & 0x0cU) == 0U)
				has_usage = 0;
			continue;
		}

		/* A collection that is not an application's ends its usage too. */
		if (value != DESCRIPTOR_APPLICATION) {
			has_usage = 0;
			continue;
		}

		/* The first application collection decides. */
		if (has_usage && page == DESCRIPTOR_PAGE_FIDO && usage == DESCRIPTOR_USAGE_CTAPHID)
			return 1;
		return 0;
	}

	/* No application collection: not a key's. */
	return 0;
}

/* Reads an item's data, little-endian, of 0 to 4 bytes. */
static uint32_t
descriptor_value(
	const uint8_t *data,
	size_t size)
{
	uint32_t value;
	size_t index;

	/* The bytes, the lowest first. */
	value = 0U;
	for (index = 0U; index < size; index++)
		value |= (uint32_t)data[index] << (8U * index);
	return value;
}
