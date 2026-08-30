/* Minimal ACPI RSDP/XSDT/RSDT/MADT discovery for amd64 PC/AT. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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
	uint8_t entries[];
} __attribute__((packed));

static struct amd64_acpi_ecam discovered_ecam[AMD64_ECAM_MAX];
static uint8_t *discovered_ecam_virtual[AMD64_ECAM_MAX];
static unsigned discovered_ecam_count;

static const struct rsdp *rsdp_at(hal_physaddr_t physical);

static int
bytes_equal(const void *left, const char *right, size_t size)
{
	const uint8_t *p = left;
	size_t i;
	for (i = 0; i < size; i++)
		if (p[i] != (uint8_t)right[i])
			return 0;
	return 1;
}

static int
checksum_ok(const void *data, size_t size)
{
	const uint8_t *p = data;
	uint8_t sum = 0;
	while (size-- != 0)
		sum = (uint8_t)(sum + *p++);
	return sum == 0;
}

static uint16_t
load_u16(const void *data)
{
	uint16_t value;

	hal_memcpy(&value, data, sizeof(value));
	return value;
}

static uint32_t
load_u32(const void *data)
{
	uint32_t value;

	hal_memcpy(&value, data, sizeof(value));
	return value;
}

static uint64_t
load_u64(const void *data)
{
	uint64_t value;

	hal_memcpy(&value, data, sizeof(value));
	return value;
}

static const void *
map_physical(uint64_t physical, size_t size)
{
	if (physical > UINTPTR_MAX || size == 0 ||
	    size - 1U > UINTPTR_MAX - (uintptr_t)physical)
		return NULL;
	return amd64_acpi_map_physical((paddr_t)physical, size);
}

static const struct rsdp *
scan_rsdp(uintptr_t start, uintptr_t end)
{
	uintptr_t address;
	for (address = start; address + 20U <= end; address += 16U) {
		const struct rsdp *candidate = rsdp_at(address);

		if (candidate != NULL &&
		    (candidate->revision < 2 ||
		     candidate->length <= end - address))
			return candidate;
	}
	return NULL;
}

static const struct rsdp *
rsdp_at(hal_physaddr_t physical)
{
	const struct rsdp *candidate;
	uint32_t length;

	candidate = map_physical(physical, 20U);
	if (candidate == NULL)
		return NULL;
	if (!bytes_equal(candidate->signature, "RSD PTR ", 8) ||
	    !checksum_ok(candidate, 20))
		return NULL;
	if (candidate->revision >= 2) {
		candidate = map_physical(physical, 24U);
		if (candidate == NULL)
			return NULL;
		length = candidate->length;
		if (length < sizeof(*candidate) || length > 4096U)
			return NULL;
		candidate = map_physical(physical, length);
		if (candidate == NULL || !checksum_ok(candidate, length))
			return NULL;
	}
	return candidate;
}

static const struct rsdp *
find_rsdp(hal_physaddr_t supplied)
{
	const uint16_t *ebda_segment;
	uintptr_t ebda;
	const struct rsdp *result = NULL;

	if (supplied != 0)
		return rsdp_at(supplied);
	ebda_segment = map_physical(0x40eU, sizeof(*ebda_segment));
	if (ebda_segment == NULL)
		return NULL;
	ebda = (uintptr_t)*ebda_segment << 4;
	if (ebda >= 0x400U && ebda < 0xa0000U)
		result = scan_rsdp(ebda, ebda + 1024U);
	if (result == NULL)
		result = scan_rsdp(0xe0000U, 0x100000U);
	return result;
}

static const struct sdt *
map_sdt_header(uint64_t physical)
{
	const struct sdt *table;

	table = map_physical(physical, sizeof(*table));
	if (table == NULL || table->length < sizeof(*table) ||
	    table->length > 0x100000U)
		return NULL;
	return table;
}

static const struct sdt *
map_sdt(uint64_t physical)
{
	const struct sdt *table = map_sdt_header(physical);
	uint32_t length;

	if (table == NULL)
		return NULL;
	length = table->length;
	table = map_physical(physical, length);
	if (table == NULL || !checksum_ok(table, length))
		return NULL;
	return table;
}

static const struct sdt *
find_sdt(const struct rsdp *rsdp, const char signature[4], size_t minimum,
	int report_root)
{
	const struct sdt *root;
	unsigned width, count, index;

	if (rsdp->revision >= 2 && rsdp->xsdt != 0) {
		root = map_sdt(rsdp->xsdt);
		width = 8;
		if (root == NULL) {
			hal_printf("A64 ACPI XSDT MAP FAIL %08X:%08X\n",
			    (uint32_t)(rsdp->xsdt >> 32), (uint32_t)rsdp->xsdt);
			return NULL;
		}
		if (!bytes_equal(root->signature, "XSDT", 4))
			return NULL;
	} else {
		root = map_sdt(rsdp->rsdt);
		width = 4;
		if (root == NULL) {
			hal_printf("A64 ACPI RSDT MAP FAIL %08X\n", rsdp->rsdt);
			return NULL;
		}
		if (!bytes_equal(root->signature, "RSDT", 4))
			return NULL;
	}
	if ((root->length - sizeof(*root)) % width != 0)
		return NULL;
	count = (root->length - sizeof(*root)) / width;
	if (report_root)
		hal_printf("A64 ACPI ROOT READY kind=%s length=%u entries=%u\n",
		    width == 8 ? "XSDT" : "RSDT", root->length, count);
	for (index = 0; index < count; index++) {
		const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
		const uint8_t *entry = entries + (size_t)index * width;
		uint64_t physical = width == 8 ? load_u64(entry) : load_u32(entry);
		const struct sdt *table;

		table = map_sdt_header(physical);

		if (table != NULL && bytes_equal(table->signature, signature, 4) &&
		    table->length >= minimum) {
			table = map_sdt(physical);
			if (table != NULL)
				return table;
		}
	}
	return NULL;
}

static int
discover_mcfg(struct amd64_acpi_info *result, const struct rsdp *rsdp)
{
	const struct sdt *mcfg = find_sdt(rsdp, "MCFG",
	    sizeof(struct sdt) + 8U, 0);
	if (mcfg == NULL)
		return HAL_OK;
	return amd64_acpi_parse_mcfg(mcfg, mcfg->length, result->ecam,
	    &result->ecam_count);
}

static int
add_cpu(struct amd64_acpi_info *result, uint32_t apic_id)
{
	unsigned i;
	if (apic_id > 255U) {
		hal_printf("A64 ACPI CPU ID FAIL id=%u limit=255\n", apic_id);
		return HAL_ERR_UNSUPPORTED;
	}
	for (i = 0; i < result->cpu_count; i++)
		if (result->cpus[i].apic_id == apic_id)
			return HAL_ERR_INVALID;
	if (result->cpu_count >= AMD64_SMP_MAX_CPUS)
		return HAL_ERR_UNSUPPORTED;
	result->cpus[result->cpu_count++].apic_id = apic_id;
	return HAL_OK;
}

int
amd64_acpi_discover(struct amd64_acpi_info *result,
	hal_physaddr_t rsdp_address)
{
	const struct rsdp *rsdp;
	const struct madt *madt;
	const uint8_t *entry;
	size_t remaining;
	unsigned i;
	int error;

	if (result == NULL)
		return HAL_ERR_INVALID;
	hal_memset(result, 0, sizeof(*result));
	for (i = 0; i < 16; i++) {
		result->isa[i].source = (uint8_t)i;
		result->isa[i].gsi = i;
	}
	rsdp = find_rsdp(rsdp_address);
	if (rsdp == NULL) {
		hal_puts("A64 ACPI RSDP FAIL\n");
		return HAL_ERR_UNSUPPORTED;
	}
	hal_printf("A64 ACPI RSDP PASS rev=%u rsdt=%08X xsdt=%08X:%08X\n",
	    rsdp->revision, rsdp->rsdt, (uint32_t)(rsdp->xsdt >> 32),
	    (uint32_t)rsdp->xsdt);
	madt = (const struct madt *)find_sdt(rsdp, "APIC",
	    sizeof(struct madt), 1);
	if (madt == NULL) {
		hal_puts("A64 ACPI MADT FAIL\n");
		return HAL_ERR_UNSUPPORTED;
	}
	hal_printf("A64 ACPI MADT READY length=%u lapic=%08X\n",
	    madt->header.length, madt->lapic_address);
	error = discover_mcfg(result, rsdp);
	if (error != HAL_OK) {
		hal_printf("A64 ACPI MCFG FAIL error=%d\n", error);
		return error;
	}
	hal_printf("A64 ACPI MCFG READY regions=%u\n", result->ecam_count);
	result->lapic_address = madt->lapic_address;
	entry = madt->entries;
	remaining = madt->header.length - sizeof(*madt);
	while (remaining != 0) {
		if (remaining < 2)
			return HAL_ERR_INVALID;
		uint8_t type = entry[0], length = entry[1];
		if (length < 2 || length > remaining)
			return HAL_ERR_INVALID;
		if (type == 0 && length >= 8) {
			uint32_t flags = load_u32(entry + 4);
			uint8_t apic_id = entry[3];
			if ((flags & 3U) != 0) {
				int error = add_cpu(result, apic_id);
				if (error != HAL_OK)
					return error;
			}
		} else if (type == 1 && length >= 12 &&
		    result->ioapic_count < AMD64_IOAPIC_MAX) {
			struct amd64_acpi_ioapic *io =
			    &result->ioapics[result->ioapic_count++];
			io->id = entry[2];
			io->address = load_u32(entry + 4);
			io->gsi_base = load_u32(entry + 8);
		} else if (type == 2 && length >= 10 && entry[2] == 0 &&
		    entry[3] < 16) {
			struct amd64_acpi_iso *iso = &result->isa[entry[3]];
			iso->source = entry[3];
			iso->gsi = load_u32(entry + 4);
			iso->flags = load_u16(entry + 8);
			iso->present = 1;
		} else if (type == 5 && length >= 12) {
			uint64_t address = load_u64(entry + 4);
			if (address > UINT32_MAX)
				return HAL_ERR_UNSUPPORTED;
			result->lapic_address = (uint32_t)address;
		} else if (type == 9 && length >= 16) {
			uint32_t apic_id = load_u32(entry + 4);
			uint32_t flags = load_u32(entry + 8);
			int error;
			if ((flags & 3U) != 0 &&
			    (error = add_cpu(result, apic_id)) != HAL_OK)
				return error;
		}
		entry += length;
		remaining -= length;
	}
	if (result->cpu_count == 0 || result->ioapic_count == 0 ||
	    result->lapic_address == 0) {
		hal_printf("A64 ACPI TOPOLOGY FAIL cpus=%u ioapics=%u lapic=%08X\n",
		    result->cpu_count, result->ioapic_count,
		    result->lapic_address);
		return HAL_ERR_UNSUPPORTED;
	}
	discovered_ecam_count = result->ecam_count;
	for (i = 0; i < result->ecam_count; i++) {
		size_t size = ((size_t)result->ecam[i].end_bus -
		    result->ecam[i].start_bus + 1U) << 20;
		discovered_ecam[i] = result->ecam[i];
		if (amd64_mmio_map_ecam(result->ecam[i].address, size,
		    (void **)&discovered_ecam_virtual[i]) != HAL_OK) {
			hal_printf("A64 ACPI ECAM MAP FAIL region=%u\n", i);
			return HAL_ERR_UNSUPPORTED;
		}
	}
	hal_printf("A64 ACPI READY cpus=%u ioapics=%u ecam=%u lapic=%08X\n",
	    result->cpu_count, result->ioapic_count, result->ecam_count,
	    result->lapic_address);
	return HAL_OK;
}

int
amd64_acpi_ecam_address(uint16_t segment, uint8_t bus, uint8_t device,
	uint8_t function, paddr_t *result)
{
	unsigned index;
	if (result == NULL || device >= 32U || function >= 8U)
		return HAL_ERR_INVALID;
	for (index = 0; index < discovered_ecam_count; index++) {
		const struct amd64_acpi_ecam *region = &discovered_ecam[index];
		if (region->segment == segment && bus >= region->start_bus &&
		    bus <= region->end_bus) {
			*result = region->address +
			    ((paddr_t)(bus - region->start_bus) << 20) +
			    ((paddr_t)device << 15) + ((paddr_t)function << 12);
			return HAL_OK;
		}
	}
	return HAL_ERR_UNSUPPORTED;
}

int
amd64_acpi_ecam_pointer(uint16_t segment, uint8_t bus, uint8_t device,
	uint8_t function, volatile uint8_t **result)
{
	unsigned index;
	if (result == NULL || device >= 32U || function >= 8U)
		return HAL_ERR_INVALID;
	for (index = 0; index < discovered_ecam_count; index++) {
		const struct amd64_acpi_ecam *region = &discovered_ecam[index];
		if (region->segment == segment && bus >= region->start_bus &&
		    bus <= region->end_bus) {
			*result = discovered_ecam_virtual[index] +
			    ((size_t)(bus - region->start_bus) << 20) +
			    ((size_t)device << 15) + ((size_t)function << 12);
			return HAL_OK;
		}
	}
	return HAL_ERR_UNSUPPORTED;
}
