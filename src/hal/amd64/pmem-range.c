/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib. */
#include "pmem-range.h"

static uint64_t bits_mask(unsigned count);
static void refresh_summary(struct amd64_pmem_extent *extent, uint64_t word);
static int bounded_pages(const struct amd64_pmem_extent *extent, uint64_t base, uint64_t size, uint64_t *first, uint64_t *pages);
static uint64_t next_free(struct amd64_pmem_extent *extent, uint64_t first, uint64_t limit, uint64_t *scanned);
static uint64_t search_run(struct amd64_pmem_extent *extent, uint64_t first, uint64_t limit, uint64_t need, uint64_t align_pages, uint64_t boundary_pages, uint64_t *scanned);

/*
 * Reports exact metadata bytes for an extent, excluding address holes.
 */
uint64_t
amd64_pmem_metadata_size(
	uint64_t pages)
{
	uint64_t words;
	uint64_t summary_words;

	if (pages == 0 || pages > UINT64_MAX - 63U)
		return 0;
	words = (pages + 63U) / 64U;
	summary_words = (words + 63U) / 64U;
	if (words > (UINT64_MAX / 8U - summary_words) / 4U)
		return 0;
	return (words * 4U + summary_words) * 8U;
}

/*
 * Initializes one complete-page RAM extent in caller-owned storage.
 */
enum amd64_pmem_result
amd64_pmem_extent_init(
	struct amd64_pmem_extent *extent,
	uint64_t base,
	uint64_t size,
	void *metadata,
	uint64_t metadata_size)
{
	uint64_t bytes;
	uint64_t index;
	uint64_t *storage;

	if (extent == 0 || metadata == 0 || ((uintptr_t)metadata & 7U) != 0 ||
	    size == 0 || ((base | size) & (AMD64_PMEM_PAGE - 1U)) != 0 ||
	    size > UINT64_MAX - base)
		return AMD64_PMEM_INVALID;
	bytes = amd64_pmem_metadata_size(size / AMD64_PMEM_PAGE);
	if (bytes == 0 || metadata_size < bytes || bytes > UINTPTR_MAX - (uintptr_t)metadata)
		return AMD64_PMEM_INVALID;
	storage = metadata;
	for (index = 0; index < bytes / 8U; index++)
		storage[index] = 0;
	extent->base = base;
	extent->pages = size / AMD64_PMEM_PAGE;
	extent->words = (extent->pages + 63U) / 64U;
	extent->used = storage;
	extent->reserved = storage + extent->words;
	extent->heads = storage + extent->words * 2U;
	extent->tails = storage + extent->words * 3U;
	extent->summary = storage + extent->words * 4U;
	extent->free_pages = extent->pages;
	extent->reserved_pages = 0;
	extent->allocated_pages = 0;
	extent->rotor = 0;
	extent->scanned_words = 0;
	extent->max_scan_words = 0;
	if (extent->pages % 64U != 0) {
		extent->used[extent->words - 1U] = ~bits_mask((unsigned)(extent->pages % 64U));
		extent->reserved[extent->words - 1U] = extent->used[extent->words - 1U];
	}
	for (index = 0; index < extent->words; index++)
		refresh_summary(extent, index);
	return AMD64_PMEM_OK;
}

/*
 * Reserves an exact subrange, permitting an idempotent boot reservation.
 */
enum amd64_pmem_result
amd64_pmem_reserve(
	struct amd64_pmem_extent *extent,
	uint64_t base,
	uint64_t size)
{
	uint64_t first;
	uint64_t pages;
	uint64_t index;
	uint64_t mask;

	if (!bounded_pages(extent, base, size, &first, &pages))
		return AMD64_PMEM_INVALID;
	/* Rejects allocated ownership before making any change. */
	for (index = first; index < first + pages; index++) {
		mask = UINT64_C(1) << (index % 64U);
		if ((extent->used[index / 64U] & mask) != 0 &&
		    (extent->reserved[index / 64U] & mask) == 0)
			return AMD64_PMEM_STATE;
	}
	for (index = first; index < first + pages; index++) {
		mask = UINT64_C(1) << (index % 64U);
		if ((extent->reserved[index / 64U] & mask) == 0) {
			extent->used[index / 64U] |= mask;
			extent->reserved[index / 64U] |= mask;
			extent->free_pages--;
			extent->reserved_pages++;
		}
	}
	for (index = first / 64U; index <= (first + pages - 1U) / 64U; index++)
		refresh_summary(extent, index);
	return AMD64_PMEM_OK;
}

/*
 * Releases an entirely reserved run after its owner has retired.
 */
enum amd64_pmem_result
amd64_pmem_release_reserved(
	struct amd64_pmem_extent *extent,
	uint64_t base,
	uint64_t size)
{
	uint64_t first;
	uint64_t pages;
	uint64_t index;
	uint64_t mask;

	if (!bounded_pages(extent, base, size, &first, &pages))
		return AMD64_PMEM_INVALID;

	/* Rejects mixed ownership before changing any accounting. */
	for (index = first; index < first + pages; index++) {
		mask = UINT64_C(1) << (index % 64U);
		if ((extent->reserved[index / 64U] & mask) == 0)
			return AMD64_PMEM_STATE;
	}

	/* Returns the retired pages to the free-word index. */
	for (index = first; index < first + pages; index++) {
		mask = UINT64_C(1) << (index % 64U);
		extent->used[index / 64U] &= ~mask;
		extent->reserved[index / 64U] &= ~mask;
	}
	extent->reserved_pages -= pages;
	extent->free_pages += pages;
	for (index = first / 64U; index <= (first + pages - 1U) / 64U; index++)
		refresh_summary(extent, index);

	return AMD64_PMEM_OK;
}

/*
 * Allocates within inclusive physical limits, alignment and a DMA boundary.
 */
enum amd64_pmem_result
amd64_pmem_extent_alloc(
	struct amd64_pmem_extent *extent,
	uint64_t size,
	uint64_t alignment,
	uint64_t minimum,
	uint64_t maximum,
	uint64_t boundary,
	uint64_t *physical,
	uint64_t *allocated_size)
{
	uint64_t first;
	uint64_t limit;
	uint64_t base_page;
	uint64_t need;
	uint64_t start;
	uint64_t middle;
	uint64_t index;
	uint64_t scanned;

	if (extent == 0 || physical == 0 || allocated_size == 0 ||
	    size == 0 || size > UINT64_MAX - (AMD64_PMEM_PAGE - 1U) || minimum > maximum ||
	    alignment == 0 || (alignment & (alignment - 1U)) != 0 ||
	    (boundary != 0 && ((boundary & (boundary - 1U)) != 0 || boundary < AMD64_PMEM_PAGE)))
		return AMD64_PMEM_INVALID;
	need = (size + AMD64_PMEM_PAGE - 1U) / AMD64_PMEM_PAGE;
	if (need > extent->free_pages || (boundary != 0 && need > boundary / AMD64_PMEM_PAGE))
		return AMD64_PMEM_NOMEM;
	if (alignment < AMD64_PMEM_PAGE)
		alignment = AMD64_PMEM_PAGE;
	base_page = extent->base / AMD64_PMEM_PAGE;
	first = minimum / AMD64_PMEM_PAGE + (minimum % AMD64_PMEM_PAGE != 0);
	limit = maximum / AMD64_PMEM_PAGE + (maximum % AMD64_PMEM_PAGE == AMD64_PMEM_PAGE - 1U);
	if (first < base_page)
		first = base_page;
	if (limit > base_page + extent->pages)
		limit = base_page + extent->pages;
	if (first >= limit || need > limit - first)
		return AMD64_PMEM_NOMEM;
	first -= base_page;
	limit -= base_page;
	middle = extent->rotor >= first && extent->rotor < limit ? extent->rotor : first;
	scanned = 0;
	start = search_run(extent, middle, limit, need, alignment / AMD64_PMEM_PAGE,
	    boundary / AMD64_PMEM_PAGE, &scanned);
	if (start == UINT64_MAX && middle != first)
		start = search_run(extent, first, limit, need, alignment / AMD64_PMEM_PAGE,
		    boundary / AMD64_PMEM_PAGE, &scanned);
	extent->scanned_words += scanned;
	if (scanned > extent->max_scan_words)
		extent->max_scan_words = scanned;
	if (start == UINT64_MAX)
		return AMD64_PMEM_NOMEM;
	for (index = start; index < start + need; index++)
		extent->used[index / 64U] |= UINT64_C(1) << (index % 64U);
	extent->heads[start / 64U] |= UINT64_C(1) << (start % 64U);
	extent->tails[(start + need - 1U) / 64U] |= UINT64_C(1) << ((start + need - 1U) % 64U);
	for (index = start / 64U; index <= (start + need - 1U) / 64U; index++)
		refresh_summary(extent, index);
	extent->free_pages -= need;
	extent->allocated_pages += need;
	extent->rotor = start + need == extent->pages ? 0 : start + need;
	*physical = extent->base + start * AMD64_PMEM_PAGE;
	*allocated_size = need * AMD64_PMEM_PAGE;
	return AMD64_PMEM_OK;
}

/*
 * Frees one complete allocation, rejecting partial or joined descriptors.
 */
enum amd64_pmem_result
amd64_pmem_extent_free(
	struct amd64_pmem_extent *extent,
	uint64_t physical,
	uint64_t size)
{
	uint64_t first;
	uint64_t pages;
	uint64_t index;
	uint64_t mask;

	if (!bounded_pages(extent, physical, size, &first, &pages))
		return AMD64_PMEM_INVALID;
	if ((extent->heads[first / 64U] & (UINT64_C(1) << (first % 64U))) == 0 ||
	    (extent->tails[(first + pages - 1U) / 64U] &
	    (UINT64_C(1) << ((first + pages - 1U) % 64U))) == 0)
		return AMD64_PMEM_STATE;
	for (index = first; index < first + pages; index++) {
		mask = UINT64_C(1) << (index % 64U);
		if ((extent->used[index / 64U] & mask) == 0 || (extent->reserved[index / 64U] & mask) != 0 ||
		    (index != first && (extent->heads[index / 64U] & mask) != 0) ||
		    (index != first + pages - 1U && (extent->tails[index / 64U] & mask) != 0))
			return AMD64_PMEM_STATE;
	}
	for (index = first; index < first + pages; index++)
		extent->used[index / 64U] &= ~(UINT64_C(1) << (index % 64U));
	extent->heads[first / 64U] &= ~(UINT64_C(1) << (first % 64U));
	extent->tails[(first + pages - 1U) / 64U] &= ~(UINT64_C(1) << ((first + pages - 1U) % 64U));
	for (index = first / 64U; index <= (first + pages - 1U) / 64U; index++)
		refresh_summary(extent, index);
	extent->free_pages += pages;
	extent->allocated_pages -= pages;
	return AMD64_PMEM_OK;
}

/* Forms a low-bit mask without a shift by the word width. */
static uint64_t
bits_mask(unsigned count)
{
	return count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1U;
}

/* Publishes whether one bitmap word still contains a free page. */
static void
refresh_summary(struct amd64_pmem_extent *extent, uint64_t word)
{
	uint64_t mask;

	mask = UINT64_C(1) << (word % 64U);
	if (extent->used[word] != UINT64_MAX)
		extent->summary[word / 64U] |= mask;
	else
		extent->summary[word / 64U] &= ~mask;
}

/* Validates a full-page subrange before touching its ownership bitmap. */
static int
bounded_pages(const struct amd64_pmem_extent *extent, uint64_t base,
    uint64_t size, uint64_t *first, uint64_t *pages)
{
	if (extent == 0 || size == 0 || ((base | size) & (AMD64_PMEM_PAGE - 1U)) != 0 ||
	    base < extent->base || (base - extent->base) / AMD64_PMEM_PAGE >= extent->pages)
		return 0;
	*first = (base - extent->base) / AMD64_PMEM_PAGE;
	*pages = size / AMD64_PMEM_PAGE;
	return *pages <= extent->pages - *first;
}

/* Skips full words using the second-level free summary. */
static uint64_t
next_free(struct amd64_pmem_extent *extent, uint64_t first,
    uint64_t limit, uint64_t *scanned)
{
	uint64_t word;
	uint64_t summary;
	uint64_t available;
	uint64_t result;

	word = first / 64U;
	while (word < extent->words && word * 64U < limit) {
		(*scanned)++;
		summary = extent->summary[word / 64U] & (UINT64_MAX << (word % 64U));
		if (summary == 0) {
			word = (word / 64U + 1U) * 64U;
			continue;
		}
		word = word / 64U * 64U + (unsigned)__builtin_ctzll(summary);
		if (word >= extent->words || word * 64U >= limit)
			break;
		available = ~extent->used[word];
		if (word == first / 64U)
			available &= UINT64_MAX << (first % 64U);
		if (available != 0) {
			result = word * 64U + (unsigned)__builtin_ctzll(available);
			return result < limit ? result : UINT64_MAX;
		}
		word++;
	}
	return UINT64_MAX;
}

/* Finds a contiguous free run while respecting physical alignment and boundary. */
static uint64_t
search_run(struct amd64_pmem_extent *extent, uint64_t first, uint64_t limit,
    uint64_t need, uint64_t align_pages, uint64_t boundary_pages, uint64_t *scanned)
{
	uint64_t start;
	uint64_t physical_page;
	uint64_t base_page;
	uint64_t checked;
	uint64_t count;
	uint64_t mask;
	uint64_t occupied;
	unsigned offset;

	base_page = extent->base / AMD64_PMEM_PAGE;
	while (first < limit && need <= limit - first) {
		start = next_free(extent, first, limit, scanned);
		if (start == UINT64_MAX)
			return UINT64_MAX;
		physical_page = (base_page + start + align_pages - 1U) & ~(align_pages - 1U);
		if (boundary_pages != 0 && need > boundary_pages - physical_page % boundary_pages)
			physical_page = (physical_page + boundary_pages - 1U) & ~(boundary_pages - 1U);
		start = physical_page - base_page;
		if (start >= limit || need > limit - start)
			return UINT64_MAX;
		checked = 0;
		while (checked < need) {
			offset = (unsigned)((start + checked) % 64U);
			count = need - checked < 64U - offset ? need - checked : 64U - offset;
			mask = bits_mask((unsigned)count) << offset;
			occupied = extent->used[(start + checked) / 64U] & mask;
			(*scanned)++;
			if (occupied != 0)
				break;
			checked += count;
		}
		if (checked == need)
			return start;
		first = (start + checked) / 64U * 64U + (unsigned)__builtin_ctzll(occupied) + 1U;
	}
	return UINT64_MAX;
}
