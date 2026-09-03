/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 PC/AT RSDP, root-table, MADT, and MCFG discovery path.
 */

#include <hal/hal.h>
#include "acpi.h"
#include "../space.h"

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
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

struct madt {
	struct sdt header;
	uint32_t lapic_address;
	uint32_t flags;
} __attribute__((packed));

static struct amd64_acpi_ecam discovered_ecam[AMD64_ECAM_MAX];
static uint8_t *discovered_ecam_virtual[AMD64_ECAM_MAX];
static unsigned discovered_ecam_count;

static int bytes_equal(const void *left, const char *right, size_t size);
static int checksum_ok(const void *data, size_t size);
static uint16_t load_u16(const void *data);
static uint32_t load_u32(const void *data);
static uint64_t load_u64(const void *data);
static const void *map_physical(uint64_t physical, size_t size);
static const struct rsdp *scan_rsdp(uintptr_t start, uintptr_t end);
static const struct rsdp *rsdp_at(hal_physaddr_t physical);
static const struct rsdp *find_rsdp(hal_physaddr_t supplied);
static const struct sdt *map_sdt_header(uint64_t physical);
static const struct sdt *map_sdt(uint64_t physical);
static const struct sdt *find_sdt(const struct rsdp *root_pointer, const char signature[4], size_t minimum, int report_root);
static int discover_mcfg(struct amd64_acpi_info *result, const struct rsdp *root_pointer);
static int add_cpu(struct amd64_acpi_info *result, uint32_t apic_id);

/*
 * Discovers amd64 ACPI CPU, interrupt, and PCI topology.
 */
int
amd64_acpi_discover(
	struct amd64_acpi_info *result,
	hal_physaddr_t rsdp_address)
{
	const struct rsdp *root_pointer;
	const struct madt *madt;
	const uint8_t *entry;
	struct amd64_acpi_ioapic *ioapic;
	struct amd64_acpi_iso *override;
	uint64_t address;
	uint32_t flags;
	uint32_t apic_id;
	size_t remaining;
	size_t ecam_size;
	uint8_t type;
	uint8_t length;
	uint8_t legacy_apic_id;
	unsigned index;
	int error;

	/* Requires a destination for discovered topology. */
	if (result == NULL)
		return HAL_ERR_INVALID;

	/* Initializes the result and identity ISA routing defaults. */
	hal_memset(result, 0, sizeof(*result));
	for (index = 0; index < 16; index++) {
		result->isa[index].source = (uint8_t)index;
		result->isa[index].gsi = index;
	}

	/* Locates and validates the supplied or firmware-scanned RSDP. */
	root_pointer = find_rsdp(rsdp_address);
	if (root_pointer == NULL) {
		hal_puts("A64 ACPI RSDP FAIL\n");
		return HAL_ERR_UNSUPPORTED;
	}
	hal_printf(
		"A64 ACPI RSDP PASS rev=%u rsdt=%08X xsdt=%08X:%08X\n",
		root_pointer->revision,
		root_pointer->rsdt,
		(uint32_t)(root_pointer->xsdt >> 32),
		(uint32_t)root_pointer->xsdt);

	/* Locates and validates the interrupt-controller table. */
	madt = (const struct madt *)find_sdt(
		root_pointer,
		"APIC",
		sizeof(struct madt),
		1);
	if (madt == NULL) {
		hal_puts("A64 ACPI MADT FAIL\n");
		return HAL_ERR_UNSUPPORTED;
	}
	hal_printf(
		"A64 ACPI MADT READY length=%u lapic=%08X\n",
		madt->header.length,
		madt->lapic_address);

	/* Discovers optional PCI enhanced-configuration regions. */
	error = discover_mcfg(result, root_pointer);
	if (error != HAL_OK) {
		hal_printf("A64 ACPI MCFG FAIL error=%d\n", error);
		return error;
	}
	hal_printf("A64 ACPI MCFG READY regions=%u\n", result->ecam_count);

	/* Parses every complete MADT record in firmware order. */
	result->lapic_address = madt->lapic_address;
	entry = (const uint8_t *)madt + sizeof(*madt);
	remaining = madt->header.length - sizeof(*madt);
	while (remaining != 0) {
		/* Requires a complete common MADT entry header. */
		if (remaining < 2)
			return HAL_ERR_INVALID;
		type = entry[0];
		length = entry[1];

		/* Requires a bounded nonempty MADT record. */
		if (length < 2 || length > remaining)
			return HAL_ERR_INVALID;

		/* Applies the selected supported MADT record type. */
		if (type == 0 && length >= 8) {
			flags = load_u32(entry + 4);
			legacy_apic_id = entry[3];

			/* Adds enabled or online-capable legacy APIC records. */
			if ((flags & 3U) != 0) {
				/* Adds this APIC identifier and checks topology capacity. */
				error = add_cpu(result, legacy_apic_id);
				if (error != HAL_OK)
					return error;
			}
		} else if (type == 1 &&
		    length >= 12 &&
		    result->ioapic_count < AMD64_IOAPIC_MAX) {
			ioapic = &result->ioapics[result->ioapic_count++];
			ioapic->id = entry[2];
			ioapic->address = load_u32(entry + 4);
			ioapic->gsi_base = load_u32(entry + 8);
		} else if (type == 2 &&
		    length >= 10 &&
		    entry[2] == 0 &&
		    entry[3] < 16) {
			override = &result->isa[entry[3]];
			override->source = entry[3];
			override->gsi = load_u32(entry + 4);
			override->flags = load_u16(entry + 8);
			override->present = 1;
		} else if (type == 5 && length >= 12) {
			address = load_u64(entry + 4);

			/* Rejects a local APIC override beyond xAPIC MMIO width. */
			if (address > UINT32_MAX)
				return HAL_ERR_UNSUPPORTED;
			result->lapic_address = (uint32_t)address;
		} else if (type == 9 && length >= 16) {
			apic_id = load_u32(entry + 4);
			flags = load_u32(entry + 8);

			/* Adds enabled or online-capable x2APIC records. */
			if ((flags & 3U) != 0) {
				/* Adds this APIC identifier and checks topology capacity. */
				error = add_cpu(result, apic_id);
				if (error != HAL_OK)
					return error;
			}
		}

		/* Advances by the validated record length. */
		entry += length;
		remaining -= length;
	}

	/* Requires a usable CPU and interrupt-controller topology. */
	if (result->cpu_count == 0 ||
	    result->ioapic_count == 0 ||
	    result->lapic_address == 0) {
		hal_printf(
			"A64 ACPI TOPOLOGY FAIL cpus=%u ioapics=%u lapic=%08X\n",
			result->cpu_count,
			result->ioapic_count,
			result->lapic_address);
		return HAL_ERR_UNSUPPORTED;
	}

	/* Maps every discovered ECAM region into a persistent virtual window. */
	discovered_ecam_count = result->ecam_count;
	for (index = 0; index < result->ecam_count; index++) {
		ecam_size = ((size_t)result->ecam[index].end_bus -
		    result->ecam[index].start_bus + 1U) << 20;
		discovered_ecam[index] = result->ecam[index];

		/* Maps this region and checks persistent-window capacity. */
		error = amd64_mmio_map_ecam(
			result->ecam[index].address,
			ecam_size,
			(void **)&discovered_ecam_virtual[index]);
		if (error != HAL_OK) {
			hal_printf(
				"A64 ACPI ECAM MAP FAIL region=%u\n",
				index);
			return HAL_ERR_UNSUPPORTED;
		}
	}

	/* Reports the completely validated and mapped topology. */
	hal_printf(
		"A64 ACPI READY cpus=%u ioapics=%u ecam=%u lapic=%08X\n",
		result->cpu_count,
		result->ioapic_count,
		result->ecam_count,
		result->lapic_address);

	/* Reports successful ACPI discovery. */
	return HAL_OK;
}

/*
 * Resolves one PCI function to its physical ECAM page.
 */
int
amd64_acpi_ecam_address(
	uint16_t segment,
	uint8_t bus,
	uint8_t device,
	uint8_t function,
	paddr_t *result)
{
	const struct amd64_acpi_ecam *region;
	unsigned index;

	/* Validates the destination and PCI function geometry. */
	if (result == NULL || device >= 32U || function >= 8U)
		return HAL_ERR_INVALID;

	/* Searches the discovered regions for this segment and bus. */
	for (index = 0; index < discovered_ecam_count; index++) {
		region = &discovered_ecam[index];

		/* Selects the region containing the requested function. */
		if (region->segment == segment &&
		    bus >= region->start_bus &&
		    bus <= region->end_bus) {
			*result = region->address +
			    ((paddr_t)(bus - region->start_bus) << 20) +
			    ((paddr_t)device << 15) +
			    ((paddr_t)function << 12);

			/* Reports a resolved ECAM address. */
			return HAL_OK;
		}
	}

	/* Reports a PCI bus outside every discovered region. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Resolves one PCI function to its mapped ECAM page.
 */
int
amd64_acpi_ecam_pointer(
	uint16_t segment,
	uint8_t bus,
	uint8_t device,
	uint8_t function,
	volatile uint8_t **result)
{
	const struct amd64_acpi_ecam *region;
	unsigned index;

	/* Validates the destination and PCI function geometry. */
	if (result == NULL || device >= 32U || function >= 8U)
		return HAL_ERR_INVALID;

	/* Searches the mapped regions for this segment and bus. */
	for (index = 0; index < discovered_ecam_count; index++) {
		region = &discovered_ecam[index];

		/* Selects the region containing the requested function. */
		if (region->segment == segment &&
		    bus >= region->start_bus &&
		    bus <= region->end_bus) {
			*result = discovered_ecam_virtual[index] +
			    ((size_t)(bus - region->start_bus) << 20) +
			    ((size_t)device << 15) +
			    ((size_t)function << 12);

			/* Reports a resolved ECAM pointer. */
			return HAL_OK;
		}
	}

	/* Reports a PCI bus outside every mapped region. */
	return HAL_ERR_UNSUPPORTED;
}

/* Compares one byte range with a fixed character sequence. */
static int
bytes_equal(
	const void *left,
	const char *right,
	size_t size)
{
	const uint8_t *bytes;
	size_t index;

	/* Compares every byte in ascending order. */
	bytes = left;
	for (index = 0; index < size; index++) {
		/* Stops at the first different byte. */
		if (bytes[index] != (uint8_t)right[index])
			return 0;
	}

	/* Reports identical byte sequences. */
	return 1;
}

/* Validates an ACPI byte-range checksum. */
static int
checksum_ok(
	const void *data,
	size_t size)
{
	const uint8_t *bytes;
	uint8_t sum;

	/* Accumulates every byte modulo 256. */
	bytes = data;
	sum = 0;
	while (size-- != 0)
		sum = (uint8_t)(sum + *bytes++);

	/* Accepts the ACPI-required zero sum. */
	if (sum == 0)
		return 1;

	/* Rejects a nonzero checksum. */
	return 0;
}

/* Loads one potentially unaligned 16-bit firmware value. */
static uint16_t
load_u16(
	const void *data)
{
	uint16_t value;

	/* Copies without imposing host alignment on firmware data. */
	hal_memcpy(&value, data, sizeof(value));

	/* Returns the copied value. */
	return value;
}

/* Loads one potentially unaligned 32-bit firmware value. */
static uint32_t
load_u32(
	const void *data)
{
	uint32_t value;

	/* Copies without imposing host alignment on firmware data. */
	hal_memcpy(&value, data, sizeof(value));

	/* Returns the copied value. */
	return value;
}

/* Loads one potentially unaligned 64-bit firmware value. */
static uint64_t
load_u64(
	const void *data)
{
	uint64_t value;

	/* Copies without imposing host alignment on firmware data. */
	hal_memcpy(&value, data, sizeof(value));

	/* Returns the copied value. */
	return value;
}

/* Maps one bounded physical firmware range. */
static const void *
map_physical(
	uint64_t physical,
	size_t size)
{
	const void *result;

	/* Rejects empty, unrepresentable, and wrapping host ranges. */
	if (physical > UINTPTR_MAX ||
	    size == 0 ||
	    size - 1U > UINTPTR_MAX - (uintptr_t)physical)
		return NULL;

	/* Maps the validated range through the persistent ACPI window. */
	result = amd64_acpi_map_physical((paddr_t)physical, size);

	/* Returns the mapper result unchanged. */
	return result;
}

/* Scans one 16-byte-aligned physical range for a valid RSDP. */
static const struct rsdp *
scan_rsdp(
	uintptr_t start,
	uintptr_t end)
{
	const struct rsdp *candidate;
	uintptr_t address;

	/* Tests every complete legacy RSDP slot in ascending order. */
	for (address = start; address + 20U <= end; address += 16U) {
		candidate = rsdp_at(address);

		/* Accepts legacy or range-contained extended RSDPs. */
		if (candidate != NULL &&
		    (candidate->revision < 2 ||
		    candidate->length <= end - address))
			return candidate;
	}

	/* Reports no valid RSDP in the scanned range. */
	return NULL;
}

/* Validates an RSDP at one physical address. */
static const struct rsdp *
rsdp_at(
	hal_physaddr_t physical)
{
	const struct rsdp *candidate;
	uint32_t length;
	int equal;
	int checksum;

	/* Maps the legacy RSDP prefix. */
	candidate = map_physical(physical, 20U);
	if (candidate == NULL)
		return NULL;

	/* Validates the legacy signature before its checksum. */
	equal = bytes_equal(candidate->signature, "RSD PTR ", 8);
	if (!equal)
		return NULL;
	checksum = checksum_ok(candidate, 20);
	if (!checksum)
		return NULL;

	/* Validates the complete extended RSDP when advertised. */
	if (candidate->revision >= 2) {
		/* Maps and requires the extended length prefix. */
		candidate = map_physical(physical, 24U);
		if (candidate == NULL)
			return NULL;

		/* Bounds the advertised complete RSDP length. */
		length = candidate->length;
		if (length < sizeof(*candidate) || length > 4096U)
			return NULL;

		/* Maps and requires the complete extended RSDP. */
		candidate = map_physical(physical, length);
		if (candidate == NULL)
			return NULL;

		/* Requires a valid checksum across the complete record. */
		checksum = checksum_ok(candidate, length);
		if (!checksum)
			return NULL;
	}

	/* Returns the complete validated RSDP mapping. */
	return candidate;
}

/* Finds the supplied or firmware-scanned RSDP. */
static const struct rsdp *
find_rsdp(
	hal_physaddr_t supplied)
{
	const uint16_t *ebda_segment;
	const struct rsdp *result;
	uintptr_t ebda;

	/* Uses an explicitly supplied handoff address when present. */
	result = NULL;
	if (supplied != 0) {
		result = rsdp_at(supplied);

		/* Returns the supplied-address validation result. */
		return result;
	}

	/* Reads the BIOS data-area EBDA segment. */
	ebda_segment = map_physical(0x40eU, sizeof(*ebda_segment));
	if (ebda_segment == NULL)
		return NULL;
	ebda = (uintptr_t)*ebda_segment << 4;

	/* Searches a plausible EBDA before the high BIOS area. */
	if (ebda >= 0x400U && ebda < 0xa0000U)
		result = scan_rsdp(ebda, ebda + 1024U);
	if (result == NULL)
		result = scan_rsdp(0xe0000U, 0x100000U);

	/* Returns the first valid firmware RSDP. */
	return result;
}

/* Maps and bounds one ACPI system-description-table header. */
static const struct sdt *
map_sdt_header(
	uint64_t physical)
{
	const struct sdt *table;

	/* Maps the fixed header before trusting its length. */
	table = map_physical(physical, sizeof(*table));
	if (table == NULL ||
	    table->length < sizeof(*table) ||
	    table->length > 0x100000U)
		return NULL;

	/* Returns the bounded header mapping. */
	return table;
}

/* Maps and checksums one complete ACPI system-description table. */
static const struct sdt *
map_sdt(
	uint64_t physical)
{
	const struct sdt *table;
	uint32_t length;
	int checksum;

	/* Maps and validates the fixed header. */
	table = map_sdt_header(physical);
	if (table == NULL)
		return NULL;

	/* Remaps and checksums the declared complete table. */
	length = table->length;
	table = map_physical(physical, length);
	if (table == NULL)
		return NULL;
	checksum = checksum_ok(table, length);
	if (!checksum)
		return NULL;

	/* Returns the complete validated table mapping. */
	return table;
}

/* Finds one ACPI table through the selected root table. */
static const struct sdt *
find_sdt(
	const struct rsdp *root_pointer,
	const char signature[4],
	size_t minimum,
	int report_root)
{
	const struct sdt *root;
	const struct sdt *table;
	const uint8_t *entries;
	const uint8_t *entry;
	const char *root_name;
	uint64_t physical;
	unsigned width;
	unsigned count;
	unsigned index;
	int equal;

	/* Selects XSDT when available, otherwise the legacy RSDT. */
	if (root_pointer->revision >= 2 && root_pointer->xsdt != 0) {
		/* Maps the XSDT and records its entry width. */
		root = map_sdt(root_pointer->xsdt);
		width = 8;
		if (root == NULL) {
			hal_printf(
				"A64 ACPI XSDT MAP FAIL %08X:%08X\n",
				(uint32_t)(root_pointer->xsdt >> 32),
				(uint32_t)root_pointer->xsdt);
			return NULL;
		}

		/* Requires the canonical XSDT signature. */
		equal = bytes_equal(root->signature, "XSDT", 4);
		if (!equal)
			return NULL;
	} else {
		/* Maps the RSDT and records its entry width. */
		root = map_sdt(root_pointer->rsdt);
		width = 4;
		if (root == NULL) {
			hal_printf(
				"A64 ACPI RSDT MAP FAIL %08X\n",
				root_pointer->rsdt);
			return NULL;
		}

		/* Requires the canonical RSDT signature. */
		equal = bytes_equal(root->signature, "RSDT", 4);
		if (!equal)
			return NULL;
	}

	/* Validates and counts the fixed-width root entries. */
	if ((root->length - sizeof(*root)) % width != 0)
		return NULL;
	count = (root->length - sizeof(*root)) / width;

	/* Reports the selected root table when requested. */
	if (report_root) {
		/* Selects the name matching the root entry width. */
		if (width == 8)
			root_name = "XSDT";
		else
			root_name = "RSDT";
		hal_printf(
			"A64 ACPI ROOT READY kind=%s length=%u entries=%u\n",
			root_name,
			root->length,
			count);
	}

	/* Searches every root entry for a matching bounded table. */
	entries = (const uint8_t *)root + sizeof(*root);
	for (index = 0; index < count; index++) {
		entry = entries + (size_t)index * width;

		/* Loads this entry through the selected root-table width. */
		if (width == 8)
			physical = load_u64(entry);
		else
			physical = load_u32(entry);
		table = map_sdt_header(physical);

		/* Remaps only a matching table with the required minimum size. */
		if (table != NULL) {
			/* Tests the candidate signature and minimum length. */
			equal = bytes_equal(table->signature, signature, 4);
			if (equal && table->length >= minimum) {
				/* Returns only a successfully mapped complete table. */
				table = map_sdt(physical);
				if (table != NULL)
					return table;
			}
		}
	}

	/* Reports no matching valid table in the selected root. */
	return NULL;
}

/* Discovers and parses the optional MCFG table. */
static int
discover_mcfg(
	struct amd64_acpi_info *result,
	const struct rsdp *root_pointer)
{
	const struct sdt *mcfg;
	int error;

	/* Looks up the optional MCFG table. */
	mcfg = find_sdt(
		root_pointer,
		"MCFG",
		sizeof(struct sdt) + 8U,
		0);
	if (mcfg == NULL)
		return HAL_OK;

	/* Parses every validated allocation entry. */
	error = amd64_acpi_parse_mcfg(
		mcfg,
		mcfg->length,
		result->ecam,
		&result->ecam_count);

	/* Returns the parser result unchanged. */
	return error;
}

/* Adds one unique xAPIC-width processor identifier. */
static int
add_cpu(
	struct amd64_acpi_info *result,
	uint32_t apic_id)
{
	unsigned index;

	/* Rejects identifiers that xAPIC delivery cannot address. */
	if (apic_id > 255U) {
		hal_printf("A64 ACPI CPU ID FAIL id=%u limit=255\n", apic_id);
		return HAL_ERR_UNSUPPORTED;
	}

	/* Rejects duplicate firmware processor identifiers. */
	for (index = 0; index < result->cpu_count; index++) {
		/* Rejects the first matching identifier. */
		if (result->cpus[index].apic_id == apic_id)
			return HAL_ERR_INVALID;
	}

	/* Rejects processor counts beyond the per-CPU storage limit. */
	if (result->cpu_count >= AMD64_SMP_MAX_CPUS)
		return HAL_ERR_UNSUPPORTED;

	/* Appends the processor in firmware discovery order. */
	result->cpus[result->cpu_count++].apic_id = apic_id;

	/* Reports a successfully added processor. */
	return HAL_OK;
}
