/* Focused host regression for amd64 UEFI memory-map normalization. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bootloader/uefi/memory-map.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define PAGE 4096ULL

static int failures;

static void
expect(int condition, const char *name)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", name);
		failures++;
	}
}

static EFI_MEMORY_DESCRIPTOR
descriptor(UINT32 type, uint64_t base, uint64_t pages)
{
	EFI_MEMORY_DESCRIPTOR result;

	memset(&result, 0, sizeof(result));
	result.Type = type;
	result.PhysicalStart = base;
	result.NumberOfPages = pages;
	return result;
}

static void
test_unsorted_and_merge(void)
{
	EFI_MEMORY_DESCRIPTOR input[] = {
	    descriptor(EfiConventionalMemory, 8 * PAGE, 2),
	    descriptor(EfiACPIMemoryNVS, 20 * PAGE, 1),
	    descriptor(EfiConventionalMemory, 4 * PAGE, 2),
	    descriptor(EfiConventionalMemory, 6 * PAGE, 2),
	    descriptor(EfiReservedMemoryType, 10 * PAGE, 2),
	};
	struct zbl6_memory_range output[8];
	uint32_t count = 99;
	enum zbl_uefi_map_result result;

	result = zbl_uefi_normalize_memory_map(input, sizeof(input),
					       sizeof(input[0]), output,
					       ARRAY_COUNT(output), &count);
	expect(result == ZBL_UEFI_MAP_OK, "unsorted map accepted");
	expect(count == 3, "same-type adjacent ranges merged");
	expect(output[0].base == 4 * PAGE && output[0].size == 6 * PAGE &&
		   output[0].type == ZBL6_MEMORY_USABLE,
	       "usable range canonicalized");
	expect(output[1].base == 10 * PAGE && output[1].size == 2 * PAGE &&
		   output[1].type == ZBL6_MEMORY_RESERVED,
	       "reserved range retained");
	expect(output[2].base == 20 * PAGE && output[2].size == PAGE &&
		   output[2].type == ZBL6_MEMORY_ACPI_NVS,
	       "ACPI range retained");
}

static void
test_descriptor_padding(void)
{
	struct padded {
		EFI_MEMORY_DESCRIPTOR descriptor;
		uint8_t extension[16];
	} input[2];
	struct zbl6_memory_range output[2];
	uint32_t count = 0;

	memset(input, 0xa5, sizeof(input));
	input[0].descriptor = descriptor(EfiMemoryMappedIO, 30 * PAGE, 1);
	input[1].descriptor = descriptor(EfiACPIReclaimMemory, 24 * PAGE, 2);
	expect(zbl_uefi_normalize_memory_map(
		   input, sizeof(input), sizeof(input[0]), output,
		   ARRAY_COUNT(output), &count) == ZBL_UEFI_MAP_OK,
	       "extended descriptor stride accepted");
	expect(count == 2 && output[0].base == 24 * PAGE &&
		   output[0].type == ZBL6_MEMORY_ACPI_RECLAIM &&
		   output[1].base == 30 * PAGE &&
		   output[1].type == ZBL6_MEMORY_MMIO,
	       "extended descriptors sorted and typed");
}

static void
test_rejections(void)
{
	EFI_MEMORY_DESCRIPTOR overlap[] = {
	    descriptor(EfiConventionalMemory, 4 * PAGE, 4),
	    descriptor(EfiBootServicesData, 6 * PAGE, 4),
	};
	EFI_MEMORY_DESCRIPTOR overflow =
	    descriptor(EfiConventionalMemory, UINT64_MAX - PAGE + 1, 1);
	EFI_MEMORY_DESCRIPTOR page_overflow =
	    descriptor(EfiConventionalMemory, 0, UINT64_MAX / PAGE + 1);
	EFI_MEMORY_DESCRIPTOR capacity[] = {
	    descriptor(EfiConventionalMemory, 2 * PAGE, 1),
	    descriptor(EfiReservedMemoryType, 4 * PAGE, 1),
	};
	EFI_MEMORY_DESCRIPTOR zero = descriptor(EfiConventionalMemory, 0, 0);
	struct zbl6_memory_range output[2];
	uint32_t count = 42;

	expect(zbl_uefi_normalize_memory_map(
		   overlap, sizeof(overlap), sizeof(overlap[0]), output,
		   ARRAY_COUNT(output), &count) == ZBL_UEFI_MAP_OVERLAP,
	       "overlap rejected");
	expect(count == 0, "failure does not publish partial count");
	expect(zbl_uefi_normalize_memory_map(
		   &overflow, sizeof(overflow), sizeof(overflow), output,
		   ARRAY_COUNT(output), &count) == ZBL_UEFI_MAP_RANGE_OVERFLOW,
	       "address range overflow rejected");
	expect(zbl_uefi_normalize_memory_map(
		   &page_overflow, sizeof(page_overflow), sizeof(page_overflow),
		   output, ARRAY_COUNT(output),
		   &count) == ZBL_UEFI_MAP_PAGE_OVERFLOW,
	       "page multiplication overflow rejected");
	expect(zbl_uefi_normalize_memory_map(capacity, sizeof(capacity),
					     sizeof(capacity[0]), output, 1,
					     &count) == ZBL_UEFI_MAP_CAPACITY,
	       "output capacity enforced");
	expect(zbl_uefi_normalize_memory_map(&zero, sizeof(zero), sizeof(zero),
					     output, ARRAY_COUNT(output),
					     &count) == ZBL_UEFI_MAP_EMPTY,
	       "empty effective map rejected");
	expect(zbl_uefi_normalize_memory_map(&zero, sizeof(zero) - 1,
					     sizeof(zero), output,
					     ARRAY_COUNT(output), &count) ==
		   ZBL_UEFI_MAP_INVALID_DESCRIPTOR_SIZE,
	       "truncated descriptor stream rejected");
}

int
main(void)
{
	test_unsorted_and_merge();
	test_descriptor_padding();
	test_rejections();
	if (failures != 0)
		return 1;
	puts("UEFI memory-map normalization test: PASS");
	return 0;
}
