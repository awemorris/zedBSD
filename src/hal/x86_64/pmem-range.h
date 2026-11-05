/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_AMD64_PMEM_RANGE_H
#define KERN_AMD64_PMEM_RANGE_H

#include <stdint.h>

#define AMD64_PMEM_PAGE 4096U

/* All mutation is serialized by the owner; firmware holes have no extent. */
struct amd64_pmem_extent {
	uint64_t base;
	uint64_t pages;
	uint64_t words;
	uint64_t *used;
	uint64_t *reserved;
	uint64_t *heads;
	uint64_t *tails;
	uint64_t *summary;
	uint64_t free_pages;
	uint64_t reserved_pages;
	uint64_t allocated_pages;
	uint64_t rotor;
	uint64_t scanned_words;
	uint64_t max_scan_words;
};

enum amd64_pmem_result {
	AMD64_PMEM_OK,
	AMD64_PMEM_INVALID,
	AMD64_PMEM_NOMEM,
	AMD64_PMEM_STATE
};

uint64_t amd64_pmem_metadata_size(uint64_t pages);
enum amd64_pmem_result amd64_pmem_extent_init(struct amd64_pmem_extent *extent,
    uint64_t base, uint64_t size, void *metadata, uint64_t metadata_size);
enum amd64_pmem_result amd64_pmem_reserve(struct amd64_pmem_extent *extent,
    uint64_t base, uint64_t size);
enum amd64_pmem_result amd64_pmem_release_reserved(struct amd64_pmem_extent *extent,
    uint64_t base, uint64_t size);
enum amd64_pmem_result amd64_pmem_extent_alloc(struct amd64_pmem_extent *extent,
    uint64_t size, uint64_t alignment, uint64_t minimum, uint64_t maximum,
    uint64_t boundary, uint64_t *physical, uint64_t *allocated_size);
enum amd64_pmem_result amd64_pmem_extent_free(struct amd64_pmem_extent *extent,
    uint64_t physical, uint64_t size);

#endif
