/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mock address producer behind struct i915_dma_ops (see mock_dma.h).
 */

#include "contract.h"
#include "mock_dma.h"

/*
 * Where the mock's device addresses start.
 *
 * No test uses a physical address this high, so a device address never
 * equals the physical address it came from.
 */
#define MOCK_DMA_IOVA_BASE	0x0000004000000000ULL

/* The width of the page number the translation reverses. */
#define MOCK_DMA_PAGE_BITS	24

/* The page number and in-page offset parts of an address. */
#define MOCK_DMA_PAGE_MASK	0xFFFFFFULL
#define MOCK_DMA_OFFSET_MASK	0xFFFULL
#define MOCK_DMA_PAGE_SHIFT	12

static uint64_t mock_dma_reverse(uint64_t page);
static int mock_dma_set_info(void *context, unsigned mask_bits, uint64_t max_segment);
static int mock_dma_map_sg(void *context, const struct i915_sg_entry *entries, unsigned orig_nents, struct i915_dma_segment *segments, unsigned out_capacity, enum i915_dma_direction direction);
static void mock_dma_unmap_sg(void *context, const struct i915_dma_segment *segments, unsigned nents, unsigned orig_nents, enum i915_dma_direction direction);
static uint64_t mock_dma_map_page(void *context, uint64_t phys, uint32_t size, enum i915_dma_direction direction);
static void mock_dma_unmap_page(void *context, uint64_t address, uint32_t size, enum i915_dma_direction direction);
static void mock_dma_sync_for_device(void *context, uint64_t address, uint32_t size, enum i915_dma_direction direction);
static void mock_dma_sync_for_cpu(void *context, uint64_t address, uint32_t size, enum i915_dma_direction direction);

/*
 * Returns the operations that reach a mock address producer.
 *
 * The context those operations receive is a struct mock_dma.  The mock
 * states a 39-bit, coherent device with 1 MiB segments.
 */
const struct i915_dma_ops *
mock_dma_ops(void)
{
	static const struct i915_dma_ops ops = {
		"mock",
		39U,
		0x100000U,
		1,
		mock_dma_set_info,
		mock_dma_map_sg,
		mock_dma_unmap_sg,
		mock_dma_map_page,
		mock_dma_unmap_page,
		mock_dma_sync_for_device,
		mock_dma_sync_for_cpu
	};

	/* Succeeded: the operations reach the mock. */
	return &ops;
}

/*
 * Clears the mock's switches and call record.
 */
void
mock_dma_reset(
	struct mock_dma *mock)
{
	/* Neither coalesces nor fails. */
	mock->coalesce = 0;
	mock->fail = 0;

	/* Forgets every call. */
	mock->set_info_calls = 0;
	mock->last_mask_bits = 0U;
	mock->last_max_segment = 0U;
	mock->sync_device_calls = 0;
	mock->sync_cpu_calls = 0;
	mock->unmap_sg_calls = 0;
	mock->unmap_page_calls = 0;
	mock->last_unmap_orig_nents = 0U;
}

/*
 * Computes the device address the mock gives a physical address.
 */
uint64_t
mock_dma_translate(
	uint64_t phys)
{
	uint64_t page;
	uint64_t offset;
	uint64_t reversed;

	/* Splits the address into its page number and in-page offset. */
	page = (phys >> MOCK_DMA_PAGE_SHIFT) & MOCK_DMA_PAGE_MASK;
	offset = phys & MOCK_DMA_OFFSET_MASK;

	/* Reverses the page number, so neighbouring pages land far apart. */
	reversed = mock_dma_reverse(page);

	/* Succeeded: the device address above the mock's base. */
	return MOCK_DMA_IOVA_BASE + (reversed << MOCK_DMA_PAGE_SHIFT) + offset;
}

/*
 * Computes the physical address a mock device address came from.
 */
uint64_t
mock_dma_untranslate(
	uint64_t address)
{
	uint64_t relative;
	uint64_t page;
	uint64_t offset;

	/* Splits the address above the base into its page number and offset. */
	relative = address - MOCK_DMA_IOVA_BASE;
	page = mock_dma_reverse((relative >> MOCK_DMA_PAGE_SHIFT) & MOCK_DMA_PAGE_MASK);
	offset = relative & MOCK_DMA_OFFSET_MASK;

	/* Succeeded: the reversal is its own inverse. */
	return (page << MOCK_DMA_PAGE_SHIFT) + offset;
}

/* Reverses the order of the low 24 bits of a page number. */
static uint64_t
mock_dma_reverse(
	uint64_t page)
{
	uint64_t reversed;
	int bit;

	/* Moves each bit from the low end of the input to the low end of the output. */
	reversed = 0U;
	for (bit = 0; bit < MOCK_DMA_PAGE_BITS; bit++) {
		reversed = (reversed << 1) | (page & 1U);
		page >>= 1;
	}

	/* Succeeded: the reversed page number. */
	return reversed;
}

/* Accepts every mask and records what it was given. */
static int
mock_dma_set_info(
	void *context,
	unsigned mask_bits,
	uint64_t max_segment)
{
	struct mock_dma *mock;

	/* Records the request so the test can see what was forwarded. */
	mock = context;
	mock->set_info_calls++;
	mock->last_mask_bits = mask_bits;
	mock->last_max_segment = max_segment;

	/* Succeeded: the mock takes any width. */
	return 0;
}

/* Translates each entry, merging contiguous ones when asked, or fails once. */
static int
mock_dma_map_sg(
	void *context,
	const struct i915_sg_entry *entries,
	unsigned orig_nents,
	struct i915_dma_segment *segments,
	unsigned out_capacity,
	enum i915_dma_direction direction)
{
	struct mock_dma *mock;
	unsigned produced;
	unsigned index;
	uint64_t base_phys;
	uint32_t length;

	UNUSED_PARAMETER(direction);

	/* A requested failure is spent here and reported as dma_map_sg does: zero segments. */
	mock = context;
	if (mock->fail != 0) {
		mock->fail = 0;
		return 0;
	}

	/* Produces one segment per run of entries. */
	produced = 0U;
	index = 0U;
	while (index < orig_nents) {
		base_phys = entries[index].phys.value;
		length = entries[index].length;

		/* Folds the physically following entries into this segment when coalescing. */
		while (mock->coalesce != 0 && index + 1U < orig_nents) {
			if (entries[index].phys.value + entries[index].length != entries[index + 1U].phys.value)
				break;

			length += entries[index + 1U].length;
			index++;
		}

		/* A list that does not fit the caller's segments fails as a whole. */
		if (produced >= out_capacity)
			return 0;

		/* Stores the segment at its translated address. */
		segments[produced].address.value = mock_dma_translate(base_phys);
		segments[produced].length = length;
		produced++;
		index++;
	}

	/* Succeeded: reports how many segments were produced. */
	return (int)produced;
}

/* Records a scatter unmap and the entry count it was given. */
static void
mock_dma_unmap_sg(
	void *context,
	const struct i915_dma_segment *segments,
	unsigned nents,
	unsigned orig_nents,
	enum i915_dma_direction direction)
{
	struct mock_dma *mock;

	UNUSED_PARAMETER(segments);
	UNUSED_PARAMETER(nents);
	UNUSED_PARAMETER(direction);

	/* Records the entry count, which must be the original one. */
	mock = context;
	mock->unmap_sg_calls++;
	mock->last_unmap_orig_nents = orig_nents;
}

/* Translates one physical run, or fails once when asked. */
static uint64_t
mock_dma_map_page(
	void *context,
	uint64_t phys,
	uint32_t size,
	enum i915_dma_direction direction)
{
	struct mock_dma *mock;
	uint64_t address;

	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(direction);

	/* A requested failure is spent here and reported with the mapping-error value. */
	mock = context;
	if (mock->fail != 0) {
		mock->fail = 0;
		return I915_DMA_MAPPING_ERROR;
	}

	/* Translates the run's start. */
	address = mock_dma_translate(phys);

	/* Succeeded: the device address of the run. */
	return address;
}

/* Records a page unmap. */
static void
mock_dma_unmap_page(
	void *context,
	uint64_t address,
	uint32_t size,
	enum i915_dma_direction direction)
{
	struct mock_dma *mock;

	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(direction);

	/* Counts the unmap. */
	mock = context;
	mock->unmap_page_calls++;
}

/* Records a hand-over to the device. */
static void
mock_dma_sync_for_device(
	void *context,
	uint64_t address,
	uint32_t size,
	enum i915_dma_direction direction)
{
	struct mock_dma *mock;

	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(direction);

	/* Counts the hand-over. */
	mock = context;
	mock->sync_device_calls++;
}

/* Records a hand-back to the CPU. */
static void
mock_dma_sync_for_cpu(
	void *context,
	uint64_t address,
	uint32_t size,
	enum i915_dma_direction direction)
{
	struct mock_dma *mock;

	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(direction);

	/* Counts the hand-back. */
	mock = context;
	mock->sync_cpu_calls++;
}
