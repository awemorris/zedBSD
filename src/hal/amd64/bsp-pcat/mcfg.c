/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Validated ACPI MCFG allocation parsing for amd64 PCI Express ECAM.
 */

#include <hal/hal.h>
#include "acpi.h"

struct mcfg_header {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
	uint64_t reserved;
} __attribute__((packed));

struct mcfg_entry {
	uint64_t address;
	uint16_t segment;
	uint8_t start_bus;
	uint8_t end_bus;
	uint32_t reserved;
} __attribute__((packed));

static int checksum_ok(const uint8_t *bytes, size_t size);

/*
 * Parses validated ECAM allocation regions from an ACPI MCFG table.
 */
int
amd64_acpi_parse_mcfg(
	const void *table,
	size_t available,
	struct amd64_acpi_ecam *regions,
	unsigned *region_count)
{
	const struct mcfg_header *header;
	const struct mcfg_entry *entry;
	uint64_t buses;
	uint64_t size;
	unsigned count;
	unsigned index;
	unsigned previous;

	/* Requires storage and enough bytes for the fixed MCFG header. */
	header = table;
	if (table == NULL || regions == NULL || region_count == NULL ||
	    available < sizeof(*header))
		return HAL_ERR_INVALID;

	/* Requires the exact ACPI MCFG signature. */
	if (header->signature[0] != 'M' || header->signature[1] != 'C' ||
	    header->signature[2] != 'F' || header->signature[3] != 'G')
		return HAL_ERR_INVALID;

	/* Requires a bounded sequence of complete allocation entries. */
	if (header->length < sizeof(*header) ||
	    header->length > available ||
	    (header->length - sizeof(*header)) % sizeof(struct mcfg_entry) != 0)
		return HAL_ERR_INVALID;

	/* Requires the ACPI checksum across the declared table length. */
	if (!checksum_ok(table, header->length))
		return HAL_ERR_INVALID;

	/* Enforces the fixed output capacity before writing any region. */
	count = (header->length - sizeof(*header)) /
	    sizeof(struct mcfg_entry);
	if (count > AMD64_ECAM_MAX)
		return HAL_ERR_UNSUPPORTED;

	/* Validates and copies every firmware ECAM allocation in order. */
	for (index = 0; index < count; index++) {
		entry = (const struct mcfg_entry *)
		    ((const uint8_t *)table + sizeof(*header) +
		    index * sizeof(*entry));

		/* Requires reserved bits, bus bounds, and ECAM alignment. */
		if (entry->reserved != 0 ||
		    entry->start_bus > entry->end_bus ||
		    (entry->address & 0xfffffU) != 0)
			return HAL_ERR_INVALID;

		/* Computes the full one-megabyte-per-bus physical span. */
		buses = (uint64_t)entry->end_bus - entry->start_bus + 1U;
		size = buses << 20;
		if (entry->address > UINTPTR_MAX || size > UINTPTR_MAX ||
		    entry->address > UINTPTR_MAX - size)
			return HAL_ERR_UNSUPPORTED;

		/* Rejects overlapping bus ranges within the same PCI segment. */
		for (previous = 0; previous < index; previous++) {
			/* Rejects an overlap with this earlier same-segment range. */
			if (regions[previous].segment == entry->segment &&
			    !(entry->end_bus < regions[previous].start_bus ||
			    entry->start_bus > regions[previous].end_bus))
				return HAL_ERR_INVALID;
		}

		/* Publishes the validated allocation in firmware order. */
		regions[index].address = (paddr_t)entry->address;
		regions[index].segment = entry->segment;
		regions[index].start_bus = entry->start_bus;
		regions[index].end_bus = entry->end_bus;
	}

	/* Reports the complete validated region count. */
	*region_count = count;
	return HAL_OK;
}

/* Verifies an ACPI byte-sum checksum. */
static int
checksum_ok(
	const uint8_t *bytes,
	size_t size)
{
	uint8_t sum;

	/* Adds every byte modulo 256. */
	sum = 0;
	while (size != 0) {
		sum = (uint8_t)(sum + *bytes++);
		size--;
	}

	/* Accepts the ACPI-required zero sum. */
	if (sum == 0)
		return 1;

	/* Rejects a nonzero checksum. */
	return 0;
}
