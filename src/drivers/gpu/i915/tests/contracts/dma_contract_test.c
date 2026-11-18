/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DMA mapping contract, checked on the host.
 *
 * Runs dma.c against the mock address producer: the set_info return
 * contract, the separation of CPU and device addresses, the scatter segment
 * count against the entry count, the cleanup of a failed mapping, the unmap
 * refusal while pinned, the order of the sync hand-overs, and the trace's
 * count of overwritten records.  It proves the contract, not hardware DMA.
 */

#include "contract.h"
#include "mock_dma.h"

#include "../../dma.h"
#include "../../trace.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* The page every entry of the checks covers. */
#define DMA_TEST_PAGE	0x1000U

/* How many trace records the order check looks at. */
#define DMA_TEST_TRACE_RECORDS	512U

/*
 * The records the order check copies out of the trace.
 *
 * Static rather than automatic because it is about 20 KiB; only the order
 * check uses it, once.
 */
static struct i915_trace_record dma_test_records[DMA_TEST_TRACE_RECORDS];

static int dma_trace_has_order(const struct i915_trace *trace, uint16_t first_op, uint16_t second_op);
static void dma_check_set_info(struct i915_dma *dma, struct mock_dma *mock);
static void dma_check_page_mapping(struct i915_dma *dma);
static void dma_check_scatter(struct i915_dma *dma, struct mock_dma *mock);
static void dma_check_failure(struct i915_dma *dma, struct mock_dma *mock);
static void dma_check_pin(struct i915_dma *dma);
static void dma_check_sync_order(struct i915_dma *dma, struct mock_dma *mock, struct i915_trace *trace);
static void dma_check_trace_overflow(struct i915_trace *trace);

/*
 * Runs the DMA mapping contract checks.
 */
int
main(void)
{
	static struct i915_trace trace;
	static struct i915_dma dma;
	static struct mock_dma mock;
	int status;

	contract_begin("DMA contract tests (mock backend, GPU-free)");

	/* Binds the mapping layer to a fresh mock producer. */
	drv_i915_trace_init(&trace);
	mock_dma_reset(&mock);
	drv_i915_dma_init(&dma, mock_dma_ops(), &mock, &trace);

	/* Runs each contract group in the order the old suite ran them. */
	dma_check_set_info(&dma, &mock);
	dma_check_page_mapping(&dma);
	dma_check_scatter(&dma, &mock);
	dma_check_failure(&dma, &mock);
	dma_check_pin(&dma);
	dma_check_sync_order(&dma, &mock, &trace);
	dma_check_trace_overflow(&trace);

	/* Reports whether every check held. */
	status = contract_end();
	if (status != 0)
		return status;

	/* Succeeded: the mapping layer keeps its contract. */
	return 0;
}

/* Reports nonzero when the trace holds second_op after a first_op. */
static int
dma_trace_has_order(
	const struct i915_trace *trace,
	uint16_t first_op,
	uint16_t second_op)
{
	uint32_t count;
	uint32_t index;
	int seen_first;

	/* Copies the records out, oldest first. */
	count = drv_i915_trace_snapshot(trace, dma_test_records, DMA_TEST_TRACE_RECORDS);

	/* Looks for the second operation once the first has been seen. */
	seen_first = 0;
	for (index = 0U; index < count; index++) {
		if (dma_test_records[index].op == first_op)
			seen_first = 1;

		/* The second operation after the first is the order asked for. */
		if (dma_test_records[index].op == second_op && seen_first != 0)
			return 1;
	}

	/* The order never appeared. */
	return 0;
}

/* Checks the set_info return contract and what it forwards. */
static void
dma_check_set_info(
	struct i915_dma *dma,
	struct mock_dma *mock)
{
	int error;

	contract_section("set_info: return contract 0/errno");

	/* A device mask and segment the call can mean are forwarded unchanged. */
	error = drv_i915_dma_set_info(dma, 39U, 0x100000U);
	contract_check(error == 0, "valid set_info returns 0");
	contract_check(mock->set_info_calls == 1, "backend set_info called once");
	contract_check(mock->last_mask_bits == 39U, "mask bits forwarded (device cap, not phys-bits)");

	/* A mask below 32 bits and an empty segment are refused before the producer. */
	error = drv_i915_dma_set_info(dma, 8U, 0x1000U);
	contract_check(error == EINVAL, "bad mask_bits -> EINVAL");
	error = drv_i915_dma_set_info(dma, 39U, 0U);
	contract_check(error == EINVAL, "zero max_segment -> EINVAL");
}

/* Checks that a page mapping never hands out the CPU address. */
static void
dma_check_page_mapping(
	struct i915_dma *dma)
{
	i915_cpu_phys_t phys;
	i915_dma_addr_t address;
	uint64_t address_value;
	uint64_t phys_value;
	uint64_t back;
	int failed;

	contract_section("DMA-1: non-identity mapping");

	/* Maps one page above 4 GiB. */
	phys = drv_i915_cpu_phys(0x0000000123456000ULL);
	address = drv_i915_dma_map_page(dma, phys, DMA_TEST_PAGE, I915_DMA_TO_DEVICE);
	failed = drv_i915_dma_mapping_failed(address);
	contract_check(failed == 0, "map_page succeeds");

	/* The device address differs from the CPU one and leads back to it. */
	address_value = drv_i915_dma_addr_raw(address);
	phys_value = drv_i915_cpu_phys_raw(phys);
	contract_check(address_value != phys_value, "device address != CPU physical");
	back = mock_dma_untranslate(address_value);
	contract_check(back == phys_value, "translation is reversible to the same phys");

	/* Gives the page back. */
	drv_i915_dma_unmap_page(dma, address, DMA_TEST_PAGE, I915_DMA_TO_DEVICE);
}

/* Checks the per-segment addresses and the coalesced segment count. */
static void
dma_check_scatter(
	struct i915_dma *dma,
	struct mock_dma *mock)
{
	struct i915_sg_entry entries[4];
	struct i915_dma_mapping *mapping;
	uint64_t expected;
	unsigned index;
	int count;
	int error;

	contract_section("DMA-2: scattered pages are not base + i*PAGE");

	/* Maps three physically adjacent pages without coalescing. */
	for (index = 0U; index < 3U; index++) {
		entries[index].phys = drv_i915_cpu_phys(0x100000ULL + index * DMA_TEST_PAGE);
		entries[index].length = DMA_TEST_PAGE;
	}
	mock->coalesce = 0;
	count = drv_i915_dma_map_sg(dma, entries, 3U, I915_DMA_TO_DEVICE, &mapping);
	contract_check(count == 3, "map_sg returns mapped count 3 (no coalesce)");
	contract_check(mapping != NULL, "handle returned");
	if (mapping == NULL)
		return;

	contract_check(mapping->nents == 3U, "handle has 3 segments");

	/* Each segment carries its own entry's translated address. */
	for (index = 0U; index < 3U; index++) {
		expected = mock_dma_translate(entries[index].phys.value);
		contract_check(mapping->segments[index].address.value == expected, "each segment addr == translate(phys[i])");
	}

	/* A "base + i * PAGE" consumer would be wrong: the segments are not linear. */
	contract_check(mapping->segments[1].address.value != mapping->segments[0].address.value + DMA_TEST_PAGE, "seg1 != seg0 + PAGE");
	contract_check(mapping->segments[2].address.value != mapping->segments[0].address.value + 2U * DMA_TEST_PAGE, "seg2 != seg0 + 2*PAGE");

	/* Gives the mapping back. */
	error = drv_i915_dma_unmap_sg(dma, mapping);
	contract_check(error == 0, "unmap ok");

	contract_section("DMA-3: coalescing: mapped nents < orig nents; unmap uses orig");

	/* Maps four physically adjacent pages with coalescing. */
	for (index = 0U; index < 4U; index++) {
		entries[index].phys = drv_i915_cpu_phys(0x200000ULL + index * DMA_TEST_PAGE);
		entries[index].length = DMA_TEST_PAGE;
	}
	mock->coalesce = 1;
	count = drv_i915_dma_map_sg(dma, entries, 4U, I915_DMA_TO_DEVICE, &mapping);
	contract_check(count >= 1, "at least one segment mapped");
	contract_check(count < 4, "mapped segment count < 4 (coalesced)");
	contract_check(mapping != NULL, "handle returned");
	if (mapping == NULL)
		return;

	/* The mapping keeps the entry count for the unmap and one segment for all pages. */
	contract_check(mapping->orig_nents == 4U, "handle remembers orig_nents=4 for unmap");
	contract_check(mapping->segments[0].length == 4U * DMA_TEST_PAGE, "coalesced segment covers all 4 pages");

	/* The unmap hands the producer the entry count, not the segment count. */
	error = drv_i915_dma_unmap_sg(dma, mapping);
	contract_check(error == 0, "unmap ok");
	contract_check(mock->last_unmap_orig_nents == 4U, "backend unmap received orig_nents=4, not the segment count");
	mock->coalesce = 0;
}

/* Checks that a failed mapping reports its own contract and leaks nothing. */
static void
dma_check_failure(
	struct i915_dma *dma,
	struct mock_dma *mock)
{
	struct i915_sg_entry entry;
	struct i915_dma_mapping *mapping;
	unsigned live;
	int count;
	int error;

	contract_section("DMA-4: mapping failure: 0 return, resources released");

	/* A failed dma_map_sg reports zero segments and hands out nothing. */
	entry.phys = drv_i915_cpu_phys(0x300000ULL);
	entry.length = DMA_TEST_PAGE;
	mock->fail = 1;
	count = drv_i915_dma_map_sg(dma, &entry, 1U, I915_DMA_TO_DEVICE, &mapping);
	contract_check(count == 0, "map_sg failure returns 0 (dma_map_sg contract)");
	contract_check(mapping == NULL, "no mapping handle on failure");

	/* The slot the failed mapping claimed is free again. */
	live = drv_i915_dma_live_mappings(dma);
	contract_check(live == 0U, "no leaked mapping slot after failure");

	/* A failed dma_map_sgtable reports an errno instead. */
	mock->fail = 1;
	error = drv_i915_dma_map_sgtable(dma, &entry, 1U, I915_DMA_TO_DEVICE, &mapping);
	contract_check(error != 0, "map_sgtable failure returns an errno (not 0)");
	contract_check(mapping == NULL, "no sgtable handle on failure");
}

/* Checks that a pinned mapping cannot be given back. */
static void
dma_check_pin(
	struct i915_dma *dma)
{
	struct i915_sg_entry entry;
	struct i915_dma_mapping *mapping;
	i915_dma_addr_t pinned;
	unsigned live;
	int failed;
	int count;
	int error;

	contract_section("DMA-5: unmap refused while pinned into a GPU page table");

	/* Maps one page. */
	entry.phys = drv_i915_cpu_phys(0x400000ULL);
	entry.length = DMA_TEST_PAGE;
	count = drv_i915_dma_map_sg(dma, &entry, 1U, I915_DMA_TO_DEVICE, &mapping);
	contract_check(count == 1, "map ok");
	if (mapping == NULL)
		return;

	/* The pin hands out the device address, never the CPU one. */
	pinned = drv_i915_dma_pin(dma, mapping);
	failed = drv_i915_dma_mapping_failed(pinned);
	contract_check(failed == 0, "pin returns a device address");
	contract_check(pinned.value == mock_dma_translate(entry.phys.value), "pinned address is the DEVICE address (never the CPU physical)");

	/* The pinned mapping is refused and stays live. */
	error = drv_i915_dma_unmap_sg(dma, mapping);
	contract_check(error == EBUSY, "unmap while pinned -> EBUSY");
	live = drv_i915_dma_live_mappings(dma);
	contract_check(live == 1U, "pinned mapping still live (not freed)");

	/* Once unpinned the mapping is given back. */
	drv_i915_dma_unpin(dma, mapping);
	error = drv_i915_dma_unmap_sg(dma, mapping);
	contract_check(error == 0, "unmap ok after unpin");
	live = drv_i915_dma_live_mappings(dma);
	contract_check(live == 0U, "mapping freed after unpin+unmap");
}

/* Checks that the hand-over to the device precedes the hand-back to the CPU. */
static void
dma_check_sync_order(
	struct i915_dma *dma,
	struct mock_dma *mock,
	struct i915_trace *trace)
{
	struct i915_sg_entry entry;
	struct i915_dma_mapping *mapping;
	i915_dma_addr_t address;
	int ordered;
	int count;

	contract_section("DMA-6: sync_for_device before use, sync_for_cpu after");

	/* Starts an empty trace, so only this group's records are ordered. */
	drv_i915_trace_init(trace);

	/* Maps one page. */
	entry.phys = drv_i915_cpu_phys(0x500000ULL);
	entry.length = DMA_TEST_PAGE;
	count = drv_i915_dma_map_sg(dma, &entry, 1U, I915_DMA_TO_DEVICE, &mapping);
	contract_check(count == 1, "map ok");
	if (mapping == NULL)
		return;

	/* The CPU wrote the page and hands it over; the device ran; the CPU takes it back. */
	address = mapping->segments[0].address;
	drv_i915_dma_sync_for_device(dma, address, DMA_TEST_PAGE, I915_DMA_TO_DEVICE);
	drv_i915_dma_sync_for_cpu(dma, address, DMA_TEST_PAGE, I915_DMA_FROM_DEVICE);
	contract_check(mock->sync_device_calls == 1, "backend sync_for_device called");
	contract_check(mock->sync_cpu_calls == 1, "backend sync_for_cpu called");

	/* The trace keeps the order the hand-overs happened in. */
	ordered = dma_trace_has_order(trace, I915_TRACE_SYNC_DEVICE, I915_TRACE_SYNC_CPU);
	contract_check(ordered != 0, "trace records sync_for_device before sync_for_cpu");

	/* Gives the mapping back. */
	(void)drv_i915_dma_unmap_sg(dma, mapping);
}

/* Checks that records overwritten before any dump are counted. */
static void
dma_check_trace_overflow(
	struct i915_trace *trace)
{
	uint32_t index;

	contract_section("trace: overflow is counted (a gap != 'not run')");

	/* Writes ten records more than the ring holds. */
	drv_i915_trace_init(trace);
	for (index = 0U; index < I915_TRACE_CAPACITY + 10U; index++)
		drv_i915_trace_record(trace, 0U, I915_TRACE_NOTE, "flood", index, 0U);

	/* Exactly the overflow was dropped. */
	contract_check(trace->dropped == 10U, "dropped count == overflow amount");
}
