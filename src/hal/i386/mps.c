/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 Intel MultiProcessor Specification discovery implementation.
 */

#include <hal/hal.h>

#include "apic-topology.h"
#include "defs.h"

struct mp_float {
	char signature[4];
	uint32_t config;
	uint8_t length;
	uint8_t revision;
	uint8_t checksum;
	uint8_t feature[5];
} __attribute__((packed));

struct mp_header {
	char signature[4];
	uint16_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem[8];
	char product[12];
	uint32_t oem_table;
	uint16_t oem_length;
	uint16_t entries;
	uint32_t lapic;
	uint16_t extended_length;
	uint8_t extended_checksum;
	uint8_t reserved;
} __attribute__((packed));

struct mp_cpu {
	uint8_t type;
	uint8_t id;
	uint8_t version;
	uint8_t flags;
	uint32_t signature;
	uint32_t features;
	uint32_t reserved[2];
} __attribute__((packed));

struct mp_bus {
	uint8_t type;
	uint8_t id;
	char name[6];
} __attribute__((packed));

struct mp_ioapic {
	uint8_t type;
	uint8_t id;
	uint8_t version;
	uint8_t flags;
	uint32_t address;
} __attribute__((packed));

struct mp_interrupt {
	uint8_t type;
	uint8_t interrupt_type;
	uint16_t flags;
	uint8_t source_bus;
	uint8_t source_irq;
	uint8_t destination_apic;
	uint8_t destination_irq;
} __attribute__((packed));

static const void *phys(uint32_t address);
static int equal_bytes(const void *left, const char *right, size_t size);
static int checksum_valid(const void *data, size_t size);
static const struct mp_float *scan_float(uint32_t first, uint32_t end);
static const struct mp_float *find_float(void);
static int add_route(struct i386_apic_topology *topology, const struct mp_interrupt *interrupt);

/*
 * Discovers i386 APIC topology through Intel MP tables.
 */
int
i386_mps_discover(
	struct i386_apic_topology *result)
{
	const struct mp_float *floating;
	const struct mp_header *header;
	const struct mp_cpu *cpu;
	const struct mp_ioapic *ioapic;
	struct i386_apic_cpu *cpu_result;
	struct i386_ioapic_desc *ioapic_result;
	const uint8_t *entry_data;
	const uint8_t *end;
	uint8_t type;
	uint8_t length;
	unsigned entry;

	/* Requires writable result storage. */
	if (result == NULL)
		return HAL_ERR_INVALID;

	/* Clears prior topology state before starting firmware discovery. */
	hal_memset(result, 0, sizeof(*result));
	floating = find_float();

	/* Requires a floating pointer to an explicit configuration table. */
	if (floating == NULL)
		return HAL_ERR_UNSUPPORTED;

	/* Rejects a floating pointer without an explicit configuration table. */
	if (floating->config == 0)
		return HAL_ERR_UNSUPPORTED;

	/* Maps and validates the complete MP configuration table. */
	header = phys(floating->config);

	/* Requires the MP configuration-table signature. */
	if (!equal_bytes(header->signature, "PCMP", 4))
		return HAL_ERR_INVALID;

	/* Requires the complete fixed MP header. */
	if (header->length < sizeof(*header))
		return HAL_ERR_INVALID;

	/* Rejects a table extent outside initially mapped physical memory. */
	if (floating->config > 0x08000000U - header->length)
		return HAL_ERR_INVALID;

	/* Requires a valid checksum over the complete base table. */
	if (!checksum_valid(header, header->length))
		return HAL_ERR_INVALID;

	/* Publishes the local-APIC address and legacy routing-mode feature. */
	result->lapic_address = header->lapic;
	result->imcr_present = (floating->feature[1] & 0x80U) != 0;

	/* Traverses every base-table entry in firmware order. */
	entry_data = (const uint8_t *)header + sizeof(*header);
	end = (const uint8_t *)header + header->length;
	for (entry = 0; entry < header->entries; entry++) {
		/* Rejects an entry whose type byte lies beyond the table. */
		if (entry_data >= end)
			return HAL_ERR_INVALID;

		/* Derives the fixed base-entry length from its type. */
		type = *entry_data;
		length = type == 0 ? 20U : 8U;

		/* Rejects a truncated fixed-size base entry. */
		if (entry_data + length > end)
			return HAL_ERR_INVALID;

		/* Publishes enabled processor, I/O-APIC, and interrupt entries. */
		if (type == 0) {
			cpu = (const void *)entry_data;

			/* Appends only enabled processor entries. */
			if ((cpu->flags & 1U) != 0U) {
				/* Rejects a CPU inventory beyond its fixed capacity. */
				if (result->cpu_count >= I386_APIC_MAX_CPUS)
					return HAL_ERR_UNSUPPORTED;

				/* Publishes the processor descriptor. */
				cpu_result = &result->cpus[result->cpu_count];
				result->cpu_count++;
				cpu_result->apic_id = cpu->id;
				cpu_result->bootstrap = (cpu->flags & 2U) != 0;
			}
		} else if (type == 2) {
			ioapic = (const void *)entry_data;

			/* Appends only enabled I/O-APIC entries. */
			if ((ioapic->flags & 1U) != 0U) {
				/* Rejects a controller inventory beyond its fixed capacity. */
				if (result->ioapic_count >= I386_IOAPIC_MAX)
					return HAL_ERR_UNSUPPORTED;

				/* Publishes the I/O-APIC descriptor. */
				ioapic_result =
				    &result->ioapics[result->ioapic_count];
				result->ioapic_count++;
				ioapic_result->apic_id = ioapic->id;
				ioapic_result->address = ioapic->address;
				ioapic_result->gsi_base = 0;
			}
		} else if (type == 3) {
			(void)add_route(result, (const void *)entry_data);
		} else if (type > 4) {
			return HAL_ERR_INVALID;
		}

		/* Advances to the next fixed-size base-table entry. */
		entry_data += length;
	}

	/* Requires at least one processor, controller, and local-APIC address. */
	if (result->cpu_count == 0 || result->ioapic_count == 0 ||
	    result->lapic_address == 0) {
		return HAL_ERR_UNSUPPORTED;
	}

	/* Reports a complete MP interrupt topology. */
	return HAL_OK;
}

/* Converts one physical firmware address through the initial direct map. */
static const void *
phys(
	uint32_t address)
{
	/* Returns the corresponding kernel virtual address. */
	return (const void *)(uintptr_t)(address | SYS_START);
}

/* Compares one fixed-size firmware byte sequence. */
static int
equal_bytes(
	const void *left,
	const char *right,
	size_t size)
{
	const uint8_t *bytes;
	size_t i;

	/* Compares each byte without depending on the hosted C library. */
	bytes = left;
	for (i = 0; i < size; i++) {
		/* Reports the first byte mismatch. */
		if (bytes[i] != (uint8_t)right[i])
			return 0;
	}

	/* Reports identical fixed-size sequences. */
	return 1;
}

/* Verifies an MP checksum over one table extent. */
static int
checksum_valid(
	const void *data,
	size_t size)
{
	const uint8_t *bytes;
	uint8_t sum;

	/* Accumulates every byte modulo 256. */
	bytes = data;
	sum = 0;
	while (size != 0U) {
		sum = (uint8_t)(sum + *bytes);
		bytes++;
		size--;
	}

	/* Reports a checksum satisfying the MP zero-sum convention. */
	if (sum == 0)
		return 1;

	/* Reports a malformed MP checksum. */
	return 0;
}

/* Scans one physical interval for a valid MP floating pointer. */
static const struct mp_float *
scan_float(
	uint32_t first,
	uint32_t end)
{
	const struct mp_float *floating;
	uint32_t address;

	/* Tests every required 16-byte-aligned candidate. */
	for (address = first;
	     address + sizeof(struct mp_float) <= end;
	     address += 16U) {
		floating = phys(address);

		/* Skips candidates without the MP floating-pointer signature. */
		if (!equal_bytes(floating->signature, "_MP_", 4))
			continue;

		/* Skips candidates whose legacy structure length is not one. */
		if (floating->length != 1)
			continue;

		/* Returns the first checksum-valid floating pointer. */
		if (checksum_valid(floating, 16))
			return floating;
	}

	/* Reports that the interval contained no floating pointer. */
	return NULL;
}

/* Finds the MP floating pointer in each architectural search window. */
static const struct mp_float *
find_float(
	void)
{
	const uint16_t *bios_data;
	const struct mp_float *floating;
	uint32_t ebda;
	uint32_t base;

	/* Reads the EBDA segment from the BIOS data area. */
	bios_data = phys(0x400U);
	ebda = (uint32_t)bios_data[7] << 4;
	floating = NULL;

	/* Searches the first KiB of a plausible EBDA. */
	if (ebda >= 0x400U && ebda < 0xa0000U)
		floating = scan_float(ebda, ebda + 1024U);

	/* Searches the last KiB of conventional memory when needed. */
	if (floating == NULL) {
		base = (uint32_t)*(const uint16_t *)phys(0x413U) * 1024U;

		/* Searches only a plausible conventional-memory boundary. */
		if (base >= 1024U && base <= 0xa0000U)
			floating = scan_float(base - 1024U, base);
	}

	/* Searches the BIOS ROM area when earlier windows did not match. */
	if (floating == NULL)
		floating = scan_float(0xf0000U, 0x100000U);

	/* Returns the first valid floating pointer found. */
	return floating;
}

/* Adds one supported I/O-interrupt assignment to the topology. */
static int
add_route(
	struct i386_apic_topology *topology,
	const struct mp_interrupt *interrupt)
{
	struct i386_apic_route *route;
	unsigned polarity;
	unsigned trigger;

	/* Decodes the MP polarity and trigger flag fields. */
	polarity = interrupt->flags & 3U;
	trigger = (interrupt->flags >> 2) & 3U;

	/* Ignores non-INT, non-legacy, and excess assignments. */
	if (interrupt->interrupt_type != 0 || interrupt->source_irq >= 16 ||
	    topology->route_count >= I386_APIC_ROUTE_MAX) {
		return HAL_OK;
	}

	/* Appends the firmware route and its electrical characteristics. */
	route = &topology->routes[topology->route_count];
	topology->route_count++;
	route->source_irq = interrupt->source_irq;
	route->ioapic_id = interrupt->destination_apic;
	route->ioapic_pin = interrupt->destination_irq;
	route->polarity_low = polarity == 3U;
	route->level_triggered = trigger == 3U;

	/* Reports the accepted route entry. */
	return HAL_OK;
}
