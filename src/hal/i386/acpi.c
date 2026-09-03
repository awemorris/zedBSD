/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 ACPI RSDT and MADT discovery implementation.
 */

#include <hal/hal.h>

#include "apic-topology.h"
#include "defs.h"

struct rsdp {
	char signature[8];
	uint8_t checksum;
	char oem[6];
	uint8_t revision;
	uint32_t rsdt;
	uint32_t length;
	uint64_t xsdt;
	uint8_t extended_checksum;
	uint8_t reserved[3];
} __attribute__((packed));

struct sdt {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem[6];
	char table[8];
	uint32_t oem_revision;
	uint32_t creator;
	uint32_t creator_revision;
} __attribute__((packed));

struct madt {
	struct sdt header;
	uint32_t lapic;
	uint32_t flags;
} __attribute__((packed));

static const void *phys(uint32_t address);
static int equal_bytes(const void *left, const char *right, size_t size);
static int checksum_valid(const void *data, size_t size);
static const struct rsdp *scan_rsdp(uint32_t address, uint32_t end);
static const struct rsdp *find_rsdp(void);
static const struct sdt *load_table(uint32_t address);

/*
 * Discovers i386 APIC topology through the ACPI RSDT and MADT.
 */
int
i386_acpi_discover(
	struct i386_apic_topology *result)
{
	const struct rsdp *rsdp;
	const struct sdt *root;
	const struct sdt *candidate;
	const struct madt *madt;
	struct i386_ioapic_desc *ioapic;
	struct i386_apic_route *route;
	const uint8_t *entry;
	const uint8_t *end;
	uint8_t overridden[16];
	uint8_t type;
	uint8_t length;
	uint32_t address;
	unsigned i;
	unsigned table_count;

	/* Requires writable result storage. */
	if (result == NULL)
		return HAL_ERR_INVALID;

	/* Clears prior topology state before starting firmware discovery. */
	hal_memset(result, 0, sizeof(*result));
	rsdp = find_rsdp();

	/* Reports the absence of an ACPI root pointer. */
	if (rsdp == NULL)
		return HAL_ERR_UNSUPPORTED;

	/* Loads and validates the 32-bit root-system-description table. */
	hal_memset(overridden, 0, sizeof(overridden));
	root = load_table(rsdp->rsdt);

	/* Rejects an invalid mapped root table. */
	if (root == NULL)
		return HAL_ERR_INVALID;

	/* Requires the 32-bit root-table signature. */
	if (!equal_bytes(root->signature, "RSDT", 4))
		return HAL_ERR_INVALID;

	/* Requires a whole number of 32-bit table addresses. */
	if ((root->length - sizeof(*root)) % 4U != 0U)
		return HAL_ERR_INVALID;

	/* Searches the root table for the interrupt-controller table. */
	madt = NULL;
	table_count = (root->length - sizeof(*root)) / 4U;
	for (i = 0; i < table_count; i++) {
		address = ((const uint32_t *)
		    ((const uint8_t *)root + sizeof(*root)))[i];
		candidate = load_table(address);

		/* Skips invalid child tables. */
		if (candidate == NULL)
			continue;

		/* Retains the first interrupt-controller table. */
		if (equal_bytes(candidate->signature, "APIC", 4)) {
			madt = (const void *)candidate;
			break;
		}
	}

	/* Requires a MADT large enough to include its fixed fields. */
	if (madt == NULL || madt->header.length < sizeof(*madt))
		return HAL_ERR_UNSUPPORTED;

	/* Traverses the variable MADT entry stream in firmware order. */
	result->lapic_address = madt->lapic;
	entry = (const uint8_t *)madt + sizeof(*madt);
	end = (const uint8_t *)madt + madt->header.length;
	while (entry + 2 <= end) {
		type = entry[0];
		length = entry[1];

		/* Rejects a truncated or non-advancing MADT entry. */
		if (length < 2 || entry + length > end)
			return HAL_ERR_INVALID;

		/* Publishes enabled processor, I/O-APIC, and IRQ-override entries. */
		if (type == 0 && length >= 8 &&
		    (*(const uint32_t *)(entry + 4) & 3U) != 0U) {
			/* Rejects a processor inventory beyond its fixed capacity. */
			if (result->cpu_count >= I386_APIC_MAX_CPUS)
				return HAL_ERR_UNSUPPORTED;

			/* Appends the enabled processor APIC identifier. */
			result->cpus[result->cpu_count].apic_id = entry[3];
			result->cpus[result->cpu_count].bootstrap = 0;
			result->cpu_count++;
		} else if (type == 1 && length >= 12) {
			/* Rejects a controller inventory beyond its fixed capacity. */
			if (result->ioapic_count >= I386_IOAPIC_MAX)
				return HAL_ERR_UNSUPPORTED;

			/* Appends the I/O-APIC descriptor. */
			ioapic = &result->ioapics[result->ioapic_count];
			result->ioapic_count++;
			ioapic->apic_id = entry[2];
			ioapic->address = *(const uint32_t *)(entry + 4);
			ioapic->gsi_base = *(const uint32_t *)(entry + 8);
		} else if (type == 2 && length >= 10 && entry[2] == 0 &&
		    entry[3] < 16) {
			/* Rejects a route inventory beyond its fixed capacity. */
			if (result->route_count >= I386_APIC_ROUTE_MAX)
				return HAL_ERR_UNSUPPORTED;

			/* Appends the ISA interrupt-source override. */
			route = &result->routes[result->route_count];
			result->route_count++;
			route->source_irq = entry[3];
			route->ioapic_id = 0xff;
			route->ioapic_pin =
			    (uint8_t)*(const uint32_t *)(entry + 4);
			route->polarity_low =
			    (*(const uint16_t *)(entry + 8) & 3U) == 3U;
			route->level_triggered =
			    ((*(const uint16_t *)(entry + 8) >> 2) & 3U) == 3U;
			overridden[entry[3]] = 1;
		}

		/* Advances by the firmware-supplied entry length. */
		entry += length;
	}

	/* Supplies identity routes for IRQs without firmware overrides. */
	for (i = 0; i < 16; i++) {
		/* Skips IRQs already described by firmware. */
		if (overridden[i])
			continue;

		/* Rejects an identity route beyond the fixed capacity. */
		if (result->route_count >= I386_APIC_ROUTE_MAX)
			return HAL_ERR_UNSUPPORTED;

		/* Appends the default ISA identity route. */
		route = &result->routes[result->route_count];
		result->route_count++;
		route->source_irq = (uint8_t)i;
		route->ioapic_id = 0xff;
		route->ioapic_pin = (uint8_t)i;
		route->polarity_low = 0;
		route->level_triggered = 0;
	}

	/* Requires at least one processor, controller, and local-APIC address. */
	if (result->cpu_count == 0 || result->ioapic_count == 0 ||
	    result->lapic_address == 0) {
		return HAL_ERR_UNSUPPORTED;
	}

	/* Reports a complete ACPI interrupt topology. */
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

/* Verifies an ACPI checksum over one table extent. */
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

	/* Reports a checksum satisfying the ACPI zero-sum convention. */
	if (sum == 0)
		return 1;

	/* Reports a malformed ACPI checksum. */
	return 0;
}

/* Scans one physical interval for a valid ACPI root pointer. */
static const struct rsdp *
scan_rsdp(
	uint32_t address,
	uint32_t end)
{
	const struct rsdp *rsdp;

	/* Tests every required 16-byte-aligned candidate. */
	for (; address + 20U <= end; address += 16U) {
		rsdp = phys(address);

		/* Skips candidates without the ACPI root signature. */
		if (!equal_bytes(rsdp->signature, "RSD PTR ", 8))
			continue;

		/* Returns the first checksum-valid root pointer. */
		if (checksum_valid(rsdp, 20))
			return rsdp;
	}

	/* Reports that the interval contained no root pointer. */
	return NULL;
}

/* Finds the ACPI root pointer in the EBDA or BIOS search window. */
static const struct rsdp *
find_rsdp(
	void)
{
	const uint16_t *bios_data;
	const struct rsdp *rsdp;
	uint32_t ebda;

	/* Reads the EBDA segment from the BIOS data area. */
	bios_data = phys(0x400U);
	ebda = (uint32_t)bios_data[7] << 4;
	rsdp = NULL;

	/* Searches the first KiB of a plausible EBDA. */
	if (ebda >= 0x400U && ebda < 0xa0000U)
		rsdp = scan_rsdp(ebda, ebda + 1024U);

	/* Returns the EBDA result without searching the fallback window. */
	if (rsdp != NULL)
		return rsdp;

	/* Searches the standard BIOS root-pointer window. */
	rsdp = scan_rsdp(0xe0000U, 0x100000U);

	/* Returns the BIOS-window result. */
	return rsdp;
}

/* Validates and maps one ACPI system-description table. */
static const struct sdt *
load_table(
	uint32_t address)
{
	const struct sdt *table;

	/* Rejects a null or fixed-header address outside mapped RAM. */
	if (address == 0 || address > 0x08000000U - sizeof(*table))
		return NULL;

	/* Maps the table before validating its firmware-supplied length. */
	table = phys(address);

	/* Rejects an implausible firmware-supplied table extent. */
	if (table->length < sizeof(*table) || table->length > 0x100000U)
		return NULL;

	/* Rejects an extent outside initially mapped physical memory. */
	if (address > 0x08000000U - table->length)
		return NULL;

	/* Rejects a table whose complete checksum is invalid. */
	if (!checksum_valid(table, table->length))
		return NULL;

	/* Returns the validated mapped table. */
	return table;
}
