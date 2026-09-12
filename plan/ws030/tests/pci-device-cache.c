/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Tests PCI cache policy and retained subranges through the actual kernel bridge. */

#include <drivers/pci.h>
#include <kern/pmem.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <hal/hal.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define TEST_WINDOWS 8U
#define TEST_BAR_SMALL 16384U
#define TEST_BAR_LARGE 0x10000000U

#include "pci-cache-definition.h"

/* A fake HAL window reserves virtual space without allocating a large host RAM buffer. */
struct test_window {
	void *address;
	size_t bytes;
	hal_physaddr_t physical;
	uint32_t attributes;
};

/* Actual PCI functions own this list exactly as they do in the PC/AT host driver. */
static struct pcat_bar_mapping *bar_mappings;

/* The actual small-BAR fallback chooses addresses from this production initial base. */
static uint32_t pci_small_mmio_next = 0xf0800000U;

/* The serial HAL peer owns each virtual window until the matching unmap. */
static struct test_window windows[TEST_WINDOWS];

/* Expected cache policy distinguishes access bits from the kernel cache-only contract. */
static uint32_t expected_attributes;

/* One-shot allocation and HAL failures target specific incomplete construction stages. */
static unsigned allocations;
static unsigned fail_allocation;
static unsigned map_calls;
static unsigned unmap_calls;
static unsigned fail_maps;
static int map_failure;
static int unmap_failure;

/* BAR assignment observations retain the full relocated physical base and call count. */
static unsigned assignments;
static uint64_t assigned_address;
static int assignment_failure;

#include "pci-cache-prototypes.h"

static void test_cache_and_ranges(void);
static void test_failures_and_relocation(void);
static void test_error_translation(void);
static struct test_window *window_find(void *address);
static void expect_empty(void);

/*
 * Runs cache, large-address, subrange lifetime and construction rollback scenarios.
 */
int
main(void)
{
	/* The first uncached mapping fails this test if PCI mistakenly passes access flags. */
	test_cache_and_ranges();
	test_failures_and_relocation();
	test_error_translation();
	expect_empty();
	puts("PCI device cache: PASS (uncached/WT, 256MiB above4GiB, subranges, rollback)");

	/* Succeeded: actual PCI and kernel adapters preserved their cache-only boundary. */
	return 0;
}

/*
 * Allocates only the real PCI mapping metadata with targeted failures.
 */
void *
kern_malloc(
	size_t bytes)
{
	void *allocation;

	/* A countdown can fail the first ownership token or the later registry record. */
	if (fail_allocation != 0U) {
		fail_allocation--;

		/* The selected construction stage acquires no allocation. */
		if (fail_allocation == 0U)
			return NULL;
	}

	/* Ordinary host allocation represents metadata, never the large device aperture. */
	allocation = malloc(bytes);
	if (allocation == NULL)
		return NULL;

	/* The real PCI teardown must retire each successful metadata owner. */
	allocations++;

	/* Succeeded: this ownership token belongs to the actual PCI mapper. */
	return allocation;
}

/*
 * Releases one tracked metadata allocation.
 */
void
kern_free(
	void *allocation)
{
	/* Optional cleanup may release an absent token. */
	if (allocation == NULL)
		return;

	/* A duplicate release would underflow the fixture's ownership count. */
	assert(allocations != 0U);
	allocations--;
	free(allocation);

	/* Succeeded: no fixture owner retains this metadata. */
	return;
}

/*
 * Maps a fake hardware aperture while checking the kernel's final access/cache flags.
 */
int
hal_space_map_device(
	hal_physaddr_t physical,
	size_t bytes,
	uint32_t attributes,
	void **mapped)
{
	struct test_window *window;
	void *address;
	unsigned index;

	/* Default policy must be RW plus NOCACHE; explicit WT must not retain NOCACHE. */
	assert(attributes == expected_attributes);
	map_calls++;

	/* A failed HAL mapping publishes neither an address nor a virtual-space owner. */
	if (fail_maps != 0U) {
		fail_maps--;
		return map_failure;
	}

	/* Reserve only address space so a 256MiB regression costs no equivalent host RAM. */
	address = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (address == MAP_FAILED)
		return HAL_ERR_NOMEM;

	/* Each successful aperture receives a separately tracked HAL lifetime. */
	window = NULL;
	for (index = 0U; index < TEST_WINDOWS; index++) {
		if (windows[index].address == NULL) {
			window = &windows[index];
			break;
		}
	}

	/* The fixture bound exceeds all simultaneously mapped scenario windows. */
	assert(window != NULL);
	window->address = address;
	window->bytes = bytes;
	window->physical = physical;
	window->attributes = attributes;
	*mapped = address;

	/* Succeeded: PCI may publish this retained hardware mapping. */
	return HAL_OK;
}

/*
 * Removes exactly the hardware window which a successful map created.
 */
int
hal_space_unmap_device(
	void *address,
	size_t bytes)
{
	struct test_window *window;
	int error;

	/* The kernel adapter must preserve a HAL refusal for callers able to retry. */
	unmap_calls++;
	if (unmap_failure != HAL_OK) {
		error = unmap_failure;
		unmap_failure = HAL_OK;
		return error;
	}

	/* Only the complete original window may own final hardware retirement. */
	window = window_find(address);
	assert(window != NULL);
	assert(window->bytes == bytes);
	error = munmap(address, bytes);
	assert(error == 0);
	memset(window, 0, sizeof(*window));

	/* Succeeded: this HAL aperture has retired exactly once. */
	return HAL_OK;
}

/*
 * Observes the fallback's ordinary BAR reassignment without emulating PCI config I/O.
 */
int
drv_pci_device_assign_bar(
	struct drv_pci_device *device,
	unsigned index,
	uint64_t address)
{
	/* The mapper retains the device and BAR identity while changing only the bus address. */
	assert(device != NULL);
	assert(index < 6U);
	assignments++;

	/* A rejected assignment cannot claim a different physical BAR address. */
	if (assignment_failure != 0)
		return assignment_failure;

	/* The next mapping must report this relocated physical base. */
	assigned_address = address;

	/* Succeeded: the fixture models a completed hardware BAR write. */
	return 0;
}

/*
 * Accepts mapping diagnostics without changing the state observed by tests.
 */
void
kern_logf(
	const char *format,
	...)
{
	/* Diagnostic text is unrelated to cache flags, aliases or resource ownership. */
	(void)format;

	/* Succeeded: the test does not require a kernel log backend. */
	return;
}

/* Actual complete functions are copied unchanged by the test-only runner. */
#include "pci-cache-functions.h"

/* Finds an exact fake HAL window base without confusing borrowed subrange addresses. */
static struct test_window *
window_find(
	void *address)
{
	unsigned index;

	/* A subrange borrows ownership and never registers a second hardware window. */
	for (index = 0U; index < TEST_WINDOWS; index++) {
		if (windows[index].address == address)
			return &windows[index];
	}

	/* No HAL owner exists for this address. */
	return NULL;
}

/* Confirms that no PCI metadata or virtual hardware aperture remains live. */
static void
expect_empty(void)
{
	unsigned index;

	/* PCI registry lifetime and metadata counts must finish together. */
	assert(bar_mappings == NULL);
	assert(allocations == 0U);
	for (index = 0U; index < TEST_WINDOWS; index++)
		assert(windows[index].address == NULL);

	/* Succeeded: the complete fixture namespace is empty. */
	return;
}

/* Checks exact cache flags and 64-bit physical identity through retained subrange reuse. */
static void
test_cache_and_ranges(void)
{
	struct drv_pci_bar bar;
	struct drv_pci_bar slice;
	struct drv_pci_mapping mapping;
	struct drv_pci_mapping borrowed;
	struct test_window *window;
	struct drv_pci_device *device;
	unsigned device_identity;
	unsigned before_maps;
	unsigned before_unmaps;
	unsigned flags;
	unsigned kind;
	int error;

	/* Both aperture sizes begin above 4GiB so accidental address truncation is visible. */
	device_identity = 0U;
	device = (struct drv_pci_device *)&device_identity;
	for (kind = 0U; kind < 2U; kind++) {
		memset(&bar, 0, sizeof(bar));
		memset(&mapping, 0, sizeof(mapping));
		bar.index = 4U;
		bar.type = DRV_PCI_BAR_MEMORY64;
		bar.bus_address = UINT64_C(0x1230000000);
		bar.size = TEST_BAR_SMALL;

		/* The second case represents the actual full Venus host-visible aperture. */
		if (kind != 0U)
			bar.size = TEST_BAR_LARGE;

		/* READ/WRITE PCI flags must never be passed as kernel cache-policy bit values. */
		flags = DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE;
		expected_attributes = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE;
		error = pcat_map_bar(NULL, device, &bar, flags, &mapping);
		assert(error == 0);
		assert(mapping.physical_address == bar.bus_address);
		assert(mapping.size == bar.size);
		window = window_find(mapping.address);
		assert(window != NULL);
		assert(window->physical == bar.bus_address);
		assert(window->bytes == bar.size);

		/* An interior MSI-X-style range borrows the original mapping with its own offset. */
		slice = bar;
		slice.bus_address += 4096U;
		slice.size = 4096U;
		before_maps = map_calls;
		error = pcat_map_bar(NULL, device, &slice, flags, &borrowed);
		assert(error == 0);
		assert(map_calls == before_maps);
		assert(borrowed.address == (uint8_t *)mapping.address + 4096U);
		assert(borrowed.physical_address == bar.bus_address + 4096U);
		assert(borrowed.size == 4096U);

		/* A premature owner unmap must retain the window until the borrowed slice retires. */
		before_unmaps = unmap_calls;
		pcat_unmap_bar(NULL, &mapping);
		assert(unmap_calls == before_unmaps);
		assert(mapping.address != NULL);
		pcat_unmap_bar(NULL, &borrowed);
		assert(borrowed.address == NULL);
		assert(unmap_calls == before_unmaps);
		pcat_unmap_bar(NULL, &mapping);
		assert(mapping.address == NULL);
		assert(unmap_calls == before_unmaps + 1U);
		expect_empty();
	}

	/* Explicit write-through uses the separate cache-policy bit and preserves ordinary access. */
	bar.size = TEST_BAR_SMALL;
	expected_attributes = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_WRITETHRU;
	error = pcat_map_bar(NULL, device, &bar, DRV_PCI_MAP_WRITETHROUGH, &mapping);
	assert(error == 0);
	window = window_find(mapping.address);
	assert(window != NULL);
	assert(window->attributes == expected_attributes);
	pcat_unmap_bar(NULL, &mapping);
	expect_empty();

	/* No caller flags is also the ordinary uncached default. */
	expected_attributes = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE;
	error = pcat_map_bar(NULL, device, &bar, 0U, &mapping);
	assert(error == 0);
	pcat_unmap_bar(NULL, &mapping);
	expect_empty();

	/* Succeeded: cache policy, physical identity and subrange lifetime stayed intact. */
	return;
}

/* Checks allocation failures, large-window refusal and small-BAR relocation ownership. */
static void
test_failures_and_relocation(void)
{
	struct drv_pci_bar bar;
	struct drv_pci_mapping mapping;
	struct drv_pci_device *device;
	unsigned device_identity;
	unsigned before_maps;
	unsigned before_unmaps;
	unsigned before_assignments;
	int error;

	/* Every failure case begins with an empty PCI registry and uncached default policy. */
	device_identity = 0U;
	device = (struct drv_pci_device *)&device_identity;
	memset(&bar, 0, sizeof(bar));
	memset(&mapping, 0, sizeof(mapping));
	bar.index = 2U;
	bar.type = DRV_PCI_BAR_MEMORY64;
	bar.bus_address = UINT64_C(0x2340000000);
	bar.size = TEST_BAR_LARGE;
	expected_attributes = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE;

	/* Failure before the first metadata owner must not reach the HAL. */
	fail_allocation = 1U;
	before_maps = map_calls;
	error = pcat_map_bar(NULL, device, &bar, 0U, &mapping);
	assert(error == ENOMEM);
	assert(map_calls == before_maps);
	expect_empty();

	/* A 256MiB aperture refusal must never attempt the old small-BAR relocation fallback. */
	fail_maps = 1U;
	map_failure = HAL_ERR_NOMEM;
	before_assignments = assignments;
	error = pcat_map_bar(NULL, device, &bar, 0U, &mapping);
	assert(error == ENOMEM);
	assert(assignments == before_assignments);
	expect_empty();

	/* Registry-record allocation failure must unmap the earlier successful HAL window. */
	fail_allocation = 2U;
	before_unmaps = unmap_calls;
	error = pcat_map_bar(NULL, device, &bar, 0U, &mapping);
	assert(error == ENOMEM);
	assert(unmap_calls == before_unmaps + 1U);
	assert(mapping.address == NULL);
	assert(mapping.physical_address == 0U);
	expect_empty();

	/* A small BAR may relocate, but its resulting physical_address must report the new base. */
	bar.size = TEST_BAR_SMALL;
	fail_maps = 1U;
	error = pcat_map_bar(NULL, device, &bar, 0U, &mapping);
	assert(error == 0);
	assert(assignments == before_assignments + 1U);
	assert(mapping.physical_address == assigned_address);
	assert(mapping.physical_address != bar.bus_address);
	pcat_unmap_bar(NULL, &mapping);
	expect_empty();

	/* Assignment failure must release metadata without pretending any window exists. */
	fail_maps = 1U;
	assignment_failure = EINVAL;
	error = pcat_map_bar(NULL, device, &bar, 0U, &mapping);
	assert(error == ENOMEM);
	assignment_failure = 0;
	expect_empty();

	/* A second HAL refusal after successful assignment must also release all local ownership. */
	fail_maps = 2U;
	error = pcat_map_bar(NULL, device, &bar, 0U, &mapping);
	assert(error == ENOMEM);
	expect_empty();

	/* Succeeded: no rejected construction leaked metadata or hardware mapping ownership. */
	return;
}

/* Checks the actual errno bridge separately from PCI's historical ENOMEM fallback. */
static void
test_error_translation(void)
{
	void *mapped;
	int statuses[5] = { HAL_ERR_INVALID, HAL_ERR_NOMEM, HAL_ERR_BUSY, HAL_ERR_UNSUPPORTED, HAL_ERR_IO };
	int errors[5] = { EINVAL, ENOMEM, EBUSY, ENOTSUP, EIO };
	unsigned index;
	int error;

	/* Every documented HAL refusal must preserve its exact kernel errno meaning. */
	expected_attributes = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE;
	for (index = 0U; index < 5U; index++) {
		mapped = NULL;
		fail_maps = 1U;
		map_failure = statuses[index];
		error = kern_device_map(UINT64_C(0x100000000), 4096U, KERN_DEVICE_UNCACHED, &mapped);
		assert(error == errors[index]);
		assert(mapped == NULL);
	}

	/* A kernel unmap refusal leaves the HAL mapping live for an ordinary retry. */
	error = kern_device_map(UINT64_C(0x100000000), 4096U, KERN_DEVICE_UNCACHED, &mapped);
	assert(error == 0);
	unmap_failure = HAL_ERR_BUSY;
	error = kern_device_unmap(mapped, 4096U);
	assert(error == EBUSY);
	error = kern_device_unmap(mapped, 4096U);
	assert(error == 0);
	expect_empty();

	/* Succeeded: the kernel bridge preserves mapping error and retry semantics. */
	return;
}
