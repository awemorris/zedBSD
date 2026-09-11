/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_AMD64_RAM_MAP_H
#define KERN_AMD64_RAM_MAP_H

#include <stdint.h>

#define AMD64_RAM_BASE UINT64_C(0xffff800000000000)
#define AMD64_RAM_LIMIT UINT64_C(0x400000000000)

/* The caller owns every allocated table until all referencing roots retire. */
struct amd64_ram_builder {
	uint64_t *root;
	void *context;
	int (*allocate)(void *context, uint64_t *physical, uint64_t **table);
	uint64_t *(*resolve)(void *context, uint64_t physical);
	uint64_t physical_max;
	uint64_t mapped_bytes;
	uint64_t table_pages;
	uint64_t large_pages;
	uint64_t small_pages;
};

enum amd64_ram_result {
	AMD64_RAM_OK,
	AMD64_RAM_INVALID,
	AMD64_RAM_NOMEM,
	AMD64_RAM_COLLISION
};

enum amd64_ram_result amd64_ram_map(struct amd64_ram_builder *builder,
    uint64_t physical, uint64_t size, uint64_t flags);
int amd64_ram_lookup(const struct amd64_ram_builder *builder,
    uint64_t physical, uint64_t *entry);

#endif
