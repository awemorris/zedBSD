/* Minimal ACPI RSDP/XSDT/RSDT/MADT discovery for amd64 PC/AT. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "acpi.h"
#include "../space.h"

struct rsdp {
	char signature[8];
	uint8 checksum;
	char oem[6];
	uint8 revision;
	uint32 rsdt;
	uint32 length;
	uint64 xsdt;
	uint8 extended_checksum;
	uint8 reserved[3];
} __attribute__((packed));

struct sdt {
	char signature[4];
	uint32 length;
	uint8 revision;
	uint8 checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32 oem_revision;
	uint32 creator_id;
	uint32 creator_revision;
} __attribute__((packed));

struct madt {
	struct sdt header;
	uint32 lapic_address;
	uint32 flags;
	uint8 entries[];
} __attribute__((packed));

static int
bytes_equal(const void *left, const char *right, size_t size)
{
	const uint8 *p = left;
	size_t i;
	for (i = 0; i < size; i++)
		if (p[i] != (uint8)right[i])
			return 0;
	return 1;
}

static int
checksum_ok(const void *data, size_t size)
{
	const uint8 *p = data;
	uint8 sum = 0;
	while (size-- != 0)
		sum = (uint8)(sum + *p++);
	return sum == 0;
}

static const struct rsdp *
scan_rsdp(uintptr_t start, uintptr_t end)
{
	uintptr_t address;
	for (address = start; address + 20U <= end; address += 16U) {
		const struct rsdp *candidate = amd64_phys_to_direct(address);
		if (bytes_equal(candidate->signature, "RSD PTR ", 8) &&
		    checksum_ok(candidate, 20) &&
		    (candidate->revision < 2 ||
		    (candidate->length >= sizeof(*candidate) &&
		    candidate->length <= 4096U &&
		    candidate->length <= end - address &&
		    checksum_ok(candidate, candidate->length))))
			return candidate;
	}
	return NULL;
}

static const struct rsdp *
rsdp_at(hal_physaddr_t physical)
{
	const struct rsdp *candidate;

	if (physical >= AMD64_DIRECT_LIMIT ||
	    physical > AMD64_DIRECT_LIMIT - 20U)
		return NULL;
	candidate = amd64_phys_to_direct((uintptr_t)physical);
	if (!bytes_equal(candidate->signature, "RSD PTR ", 8) ||
	    !checksum_ok(candidate, 20))
		return NULL;
	if (candidate->revision >= 2 &&
	    (candidate->length < sizeof(*candidate) ||
	    candidate->length > 4096U ||
	    physical > AMD64_DIRECT_LIMIT - candidate->length ||
	    !checksum_ok(candidate, candidate->length)))
		return NULL;
	return candidate;
}

static const struct rsdp *
find_rsdp(hal_physaddr_t supplied)
{
	const uint16 *ebda_segment = amd64_phys_to_direct(0x40eU);
	uintptr_t ebda = (uintptr_t)*ebda_segment << 4;
	const struct rsdp *result = NULL;

	if (supplied != 0)
		return rsdp_at(supplied);
	if (ebda >= 0x400U && ebda < 0xa0000U)
		result = scan_rsdp(ebda, ebda + 1024U);
	if (result == NULL)
		result = scan_rsdp(0xe0000U, 0x100000U);
	return result;
}

static const struct sdt *
map_sdt(uint64 physical)
{
	const struct sdt *table;
	if (physical >= AMD64_DIRECT_LIMIT ||
	    physical > AMD64_DIRECT_LIMIT - sizeof(*table))
		return NULL;
	table = amd64_phys_to_direct((uintptr_t)physical);
	if (table->length < sizeof(*table) || table->length > 0x100000U ||
	    physical > AMD64_DIRECT_LIMIT - table->length ||
	    !checksum_ok(table, table->length))
		return NULL;
	return table;
}

static const struct madt *
find_madt(const struct rsdp *rsdp)
{
	const struct sdt *root;
	unsigned width, count, index;

	if (rsdp->revision >= 2 && rsdp->xsdt != 0) {
		root = map_sdt(rsdp->xsdt);
		width = 8;
		if (root == NULL || !bytes_equal(root->signature, "XSDT", 4))
			return NULL;
	} else {
		root = map_sdt(rsdp->rsdt);
		width = 4;
		if (root == NULL || !bytes_equal(root->signature, "RSDT", 4))
			return NULL;
	}
	if ((root->length - sizeof(*root)) % width != 0)
		return NULL;
	count = (root->length - sizeof(*root)) / width;
	for (index = 0; index < count; index++) {
		const uint8 *entries = (const uint8 *)root + sizeof(*root);
		uint64 physical = width == 8 ? ((const uint64 *)entries)[index] :
		    ((const uint32 *)entries)[index];
		const struct sdt *table = map_sdt(physical);
		if (table != NULL && bytes_equal(table->signature, "APIC", 4) &&
		    table->length >= sizeof(struct madt))
			return (const struct madt *)table;
	}
	return NULL;
}

static int
add_cpu(struct amd64_acpi_info *result, uint32 apic_id)
{
	unsigned i;
	if (apic_id > 255U)
		return HAL_ERR_UNSUPPORTED;
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
	const uint8 *entry, *end;
	unsigned i;

	if (result == NULL)
		return HAL_ERR_INVALID;
	hal_memset(result, 0, sizeof(*result));
	for (i = 0; i < 16; i++) {
		result->isa[i].source = (uint8)i;
		result->isa[i].gsi = i;
	}
	rsdp = find_rsdp(rsdp_address);
	if (rsdp == NULL || (madt = find_madt(rsdp)) == NULL)
		return HAL_ERR_UNSUPPORTED;
	result->lapic_address = madt->lapic_address;
	entry = madt->entries;
	end = (const uint8 *)madt + madt->header.length;
	while (entry + 2 <= end) {
		uint8 type = entry[0], length = entry[1];
		if (length < 2 || entry + length > end)
			return HAL_ERR_INVALID;
		if (type == 0 && length >= 8) {
			uint32 flags = *(const uint32 *)(entry + 4);
			uint8 apic_id = entry[3];
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
			io->address = *(const uint32 *)(entry + 4);
			io->gsi_base = *(const uint32 *)(entry + 8);
		} else if (type == 2 && length >= 10 && entry[2] == 0 &&
		    entry[3] < 16) {
			struct amd64_acpi_iso *iso = &result->isa[entry[3]];
			iso->source = entry[3];
			iso->gsi = *(const uint32 *)(entry + 4);
			iso->flags = *(const uint16 *)(entry + 8);
			iso->present = 1;
		} else if (type == 5 && length >= 12) {
			uint64 address = *(const uint64 *)(entry + 4);
			if (address > UINT32_MAX)
				return HAL_ERR_UNSUPPORTED;
			result->lapic_address = (uint32)address;
		} else if (type == 9 && length >= 16) {
			uint32 apic_id = *(const uint32 *)(entry + 4);
			uint32 flags = *(const uint32 *)(entry + 8);
			int error;
			if ((flags & 3U) != 0 &&
			    (error = add_cpu(result, apic_id)) != HAL_OK)
				return error;
		}
		entry += length;
	}
	if (result->cpu_count == 0 || result->ioapic_count == 0 ||
	    result->lapic_address == 0)
		return HAL_ERR_UNSUPPORTED;
	return HAL_OK;
}
