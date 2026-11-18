/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The raw HID devices' reading of a report descriptor (ws161-p002): the
 * top collection's usage, whether the reports are numbered, and the sizes
 * of the input and the output reports.  Pure (no lock, no allocation), so
 * the host test compiles it as it is (plan/ws161/tests/hidraw-describe-host-test.sh).
 */

#include <drivers/generic/hidraw.h>

#include <uapi/errno.h>

/* The short items of a report descriptor this class reads (HID 1.11 section 6.2.2). */
#define HIDRAW_ITEM_LONG		0xfeU
#define HIDRAW_ITEM_USAGE_PAGE		0x04U
#define HIDRAW_ITEM_USAGE		0x08U
#define HIDRAW_ITEM_REPORT_SIZE		0x74U
#define HIDRAW_ITEM_REPORT_ID		0x84U
#define HIDRAW_ITEM_REPORT_COUNT	0x94U
#define HIDRAW_ITEM_INPUT		0x80U
#define HIDRAW_ITEM_OUTPUT		0x90U
#define HIDRAW_ITEM_COLLECTION		0xa0U
#define HIDRAW_ITEM_END_COLLECTION	0xc0U

/* The vendor-defined usage pages (HID Usage Tables 1.4, section 3). */
#define HIDRAW_USAGE_PAGE_VENDOR_FIRST	0xff00U
#define HIDRAW_USAGE_PAGE_VENDOR_LAST	0xffffU

/*
 * Reads what a raw device needs from its report descriptor: the usage
 * page and the usage of the first top collection, whether the device
 * numbers its reports (a Report ID item anywhere), the bytes of the
 * input and of the output reports (the sums of Report Size times Report
 * Count over the Input and the Output items; for a device that numbers its
 * reports, the sums of all of them, which bound each one), and whether
 * every top collection is of a vendor-defined usage page.  Returns 0,
 * ENOENT when there is no collection, or EINVAL for a descriptor whose
 * items run past its end.
 */
int
drv_hidraw_describe(
	const uint8_t *descriptor,
	size_t size,
	struct drv_hidraw_layout *layout)
{
	uint64_t input_bits;
	uint64_t output_bits;
	uint32_t data;
	uint32_t page;
	uint32_t local_usage;
	uint32_t collection_page;
	uint32_t report_size;
	uint32_t report_count;
	size_t offset;
	size_t length;
	size_t index;
	unsigned depth;
	unsigned found;
	unsigned extended;
	unsigned vendor_only;
	uint8_t prefix;
	uint8_t item;

	/* Nothing seen yet. */
	if (descriptor == NULL || layout == NULL)
		return EINVAL;
	layout->usage_page = 0U;
	layout->usage = 0U;
	layout->numbered = 0;
	layout->input_size = 0U;
	layout->output_size = 0U;
	layout->vendor_only = 0;
	input_bits = 0U;
	output_bits = 0U;
	page = 0U;
	local_usage = 0U;
	report_size = 0U;
	report_count = 0U;
	extended = 0U;
	depth = 0U;
	found = 0U;
	vendor_only = 1U;

	/* Walks the items. */
	offset = 0U;
	while (offset < size) {
		/* A long item: its length and tag, then its data, skipped. */
		prefix = descriptor[offset];
		if (prefix == HIDRAW_ITEM_LONG) {
			if (offset + 3U > size)
				return EINVAL;
			length = descriptor[offset + 1U];
			if (offset + 3U + length > size)
				return EINVAL;
			offset += 3U + length;
			continue;
		}

		/* A short item: 0, 1, 2 or 4 bytes of data, little-endian. */
		length = prefix & 3U;
		if (length == 3U)
			length = 4U;
		if (offset + 1U + length > size)
			return EINVAL;
		data = 0U;
		for (index = 0U; index < length; index++)
			data |= (uint32_t)descriptor[offset + 1U + index] << (8U * index);
		item = (uint8_t)(prefix & 0xfcU);
		offset += 1U + length;

		/* Each item that matters here; a Usage of four bytes carries its own page. */
		switch (item) {
		case HIDRAW_ITEM_USAGE_PAGE:
			page = data;
			break;
		case HIDRAW_ITEM_USAGE:
			local_usage = data;
			extended = 0U;
			if (length == 4U)
				extended = 1U;
			break;
		case HIDRAW_ITEM_REPORT_ID:
			layout->numbered = 1;
			break;
		case HIDRAW_ITEM_REPORT_SIZE:
			report_size = data;
			break;
		case HIDRAW_ITEM_REPORT_COUNT:
			report_count = data;
			break;
		case HIDRAW_ITEM_INPUT:
			/* The input report grows by the item's fields. */
			input_bits += (uint64_t)report_size * report_count;
			local_usage = 0U;
			break;
		case HIDRAW_ITEM_OUTPUT:
			/* The output report grows by the item's fields. */
			output_bits += (uint64_t)report_size * report_count;
			local_usage = 0U;
			break;
		case HIDRAW_ITEM_COLLECTION:
			/* A top collection's page: the Usage Page, or a Usage of four bytes' own. */
			collection_page = page;
			if (extended)
				collection_page = local_usage >> 16U;

			/* Every top collection says whether the device is a vendor's alone. */
			if (depth == 0U && (collection_page < HIDRAW_USAGE_PAGE_VENDOR_FIRST || collection_page > HIDRAW_USAGE_PAGE_VENDOR_LAST))
				vendor_only = 0U;

			/* The first collection at the top is the device's. */
			if (depth == 0U && !found) {
				layout->usage_page = (uint16_t)collection_page;
				layout->usage = (uint16_t)local_usage;
				found = 1U;
			}

			/* The collection opens a level. */
			depth++;
			local_usage = 0U;
			break;
		case HIDRAW_ITEM_END_COLLECTION:
			if (depth > 0U)
				depth--;
			local_usage = 0U;
			break;
		default:
			break;
		}
	}

	/* A descriptor without a collection describes nothing. */
	if (!found)
		return ENOENT;

	/* The reports' bytes, rounded up, as far as a report may be. */
	input_bits = (input_bits + 7U) / 8U;
	output_bits = (output_bits + 7U) / 8U;
	if (input_bits > HIDRAW_REPORT_MAX)
		input_bits = HIDRAW_REPORT_MAX;
	if (output_bits > HIDRAW_REPORT_MAX)
		output_bits = HIDRAW_REPORT_MAX;
	layout->input_size = (uint32_t)input_bits;
	layout->output_size = (uint32_t)output_bits;
	layout->vendor_only = (int)vendor_only;

	/* Succeeded: the device's layout. */
	return 0;
}
