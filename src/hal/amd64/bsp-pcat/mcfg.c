/* Validated ACPI MCFG allocation parser. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static int
checksum_ok(const uint8_t *bytes, size_t size)
{
	uint8_t sum = 0;
	while (size-- != 0)
		sum = (uint8_t)(sum + *bytes++);
	return sum == 0;
}

int
amd64_acpi_parse_mcfg(const void *table, size_t available,
	struct amd64_acpi_ecam *regions, unsigned *region_count)
{
	const struct mcfg_header *header = table;
	unsigned count, index, previous;
	if (table == NULL || regions == NULL || region_count == NULL ||
	    available < sizeof(*header) ||
	    header->signature[0] != 'M' || header->signature[1] != 'C' ||
	    header->signature[2] != 'F' || header->signature[3] != 'G' ||
	    header->length < sizeof(*header) || header->length > available ||
	    (header->length - sizeof(*header)) % sizeof(struct mcfg_entry) != 0 ||
	    !checksum_ok(table, header->length))
		return HAL_ERR_INVALID;
	count = (header->length - sizeof(*header)) / sizeof(struct mcfg_entry);
	if (count > AMD64_ECAM_MAX)
		return HAL_ERR_UNSUPPORTED;
	for (index = 0; index < count; index++) {
		const struct mcfg_entry *entry = (const struct mcfg_entry *)
		    ((const uint8_t *)table + sizeof(*header) +
		    index * sizeof(*entry));
		uint64_t buses, size;
		if (entry->reserved != 0 || entry->start_bus > entry->end_bus ||
		    (entry->address & 0xfffffU) != 0)
			return HAL_ERR_INVALID;
		buses = (uint64_t)entry->end_bus - entry->start_bus + 1U;
		size = buses << 20;
		if (entry->address > UINTPTR_MAX || size > UINTPTR_MAX ||
		    entry->address > UINTPTR_MAX - size)
			return HAL_ERR_UNSUPPORTED;
		for (previous = 0; previous < index; previous++)
			if (regions[previous].segment == entry->segment &&
			    !(entry->end_bus < regions[previous].start_bus ||
			    entry->start_bus > regions[previous].end_bus))
				return HAL_ERR_INVALID;
		regions[index].address = (paddr_t)entry->address;
		regions[index].segment = entry->segment;
		regions[index].start_bus = entry->start_bus;
		regions[index].end_bus = entry->end_bus;
	}
	*region_count = count;
	return HAL_OK;
}
