/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "src/hal/amd64/ram-map.h"
#include "src/hal/amd64/defs.h"

static uint64_t root[512], tables[64][512];
static unsigned used, capacity;

static int allocate(void *context, uint64_t *physical, uint64_t **table)
{
	(void)context;
	if (used == capacity) return 0;
	*physical = 0x100000 + (uint64_t)used * 4096;
	*table = tables[used++];
	return 1;
}

static uint64_t *resolve(void *context, uint64_t physical)
{
	(void)context;
	if (physical < 0x100000 || (physical & 4095) != 0 ||
	    (physical - 0x100000) / 4096 >= used) return 0;
	return tables[(physical - 0x100000) / 4096];
}

int main(void)
{
	struct amd64_ram_builder builder;
	uint64_t entry, high, flags;
	memset(&builder, 0, sizeof(builder));
	builder.root = root; builder.allocate = allocate; builder.resolve = resolve;
	builder.physical_max = (1ULL << 46) - 1;
	capacity = 64;
	flags = AMD64_PTE_WRITE | AMD64_PTE_NX | AMD64_PTE_GLOBAL;
	assert(amd64_ram_map(&builder, 0x3ff00000, 0x200000, flags) == AMD64_RAM_OK);
	assert(builder.small_pages == 512 && builder.large_pages == 0);
	assert(amd64_ram_lookup(&builder, 0x40000123, &entry));
	assert((entry & AMD64_PTE_NX) != 0 && (entry & AMD64_PTE_WRITE) != 0);
	assert(!amd64_ram_lookup(&builder, 0x40100000, &entry));
	high = 1ULL << 32;
	assert(amd64_ram_map(&builder, high, 0x200000, flags) == AMD64_RAM_OK);
	assert(amd64_ram_lookup(&builder, high + 0x1fffff, &entry));
	assert((entry & AMD64_PTE_LARGE) != 0);
	assert(!amd64_ram_lookup(&builder, high - 1, &entry));
	assert(amd64_ram_map(&builder, high + 4096, 4096, flags) == AMD64_RAM_COLLISION);
	assert(amd64_ram_map(&builder, high, 0x200000, flags) == AMD64_RAM_COLLISION);
	assert(amd64_ram_map(&builder, 0x200000, 4096, AMD64_PTE_NX) == AMD64_RAM_OK);
	assert(amd64_ram_lookup(&builder, 0x200001, &entry));
	assert((entry & AMD64_PTE_WRITE) == 0);
	assert(!amd64_ram_lookup(&builder, 0xb8000, &entry));
	assert(!amd64_ram_lookup(&builder, 0xfee00000, &entry));
	assert(!amd64_ram_lookup(&builder, AMD64_RAM_LIMIT, &entry));
	assert(amd64_ram_map(&builder, AMD64_RAM_LIMIT - 4096, 4096, flags) == AMD64_RAM_OK);
	assert(amd64_ram_lookup(&builder, AMD64_RAM_LIMIT - 1, &entry));
	assert(amd64_ram_map(&builder, AMD64_RAM_LIMIT - 4096, 8192, flags) == AMD64_RAM_INVALID);
	assert(amd64_ram_map(&builder, UINT64_MAX - 4095, 4096, flags) == AMD64_RAM_INVALID);
	assert(amd64_ram_map(&builder, 1, 4096, flags) == AMD64_RAM_INVALID);
	assert(amd64_ram_map(&builder, 0, 4095, flags) == AMD64_RAM_INVALID);
	assert(amd64_ram_map(&builder, 0, 4096, AMD64_PTE_WRITE) == AMD64_RAM_INVALID);
	assert(amd64_ram_map(&builder, 0, 4096, flags | AMD64_PTE_USER) == AMD64_RAM_INVALID);
	builder.physical_max = (1ULL << 32) - 1;
	assert(amd64_ram_map(&builder, high, 4096, flags) == AMD64_RAM_INVALID);
	builder.physical_max = (1ULL << 46) - 1;
	capacity = used;
	assert(amd64_ram_map(&builder, 1ULL << 40, 4096, flags) == AMD64_RAM_NOMEM);
	assert(!amd64_ram_lookup(&builder, 1ULL << 40, &entry));
	capacity = used + 1;
	assert(amd64_ram_map(&builder, 1ULL << 40, 4096, flags) == AMD64_RAM_NOMEM);
	assert(!amd64_ram_lookup(&builder, 1ULL << 40, &entry));
	capacity = 64;
	assert(amd64_ram_map(&builder, 1ULL << 40, 4096, flags) == AMD64_RAM_OK);
	assert(amd64_ram_lookup(&builder, 1ULL << 40, &entry));
	assert(amd64_ram_map(&builder, 0x201000, 4096, flags | AMD64_PTE_WRITETHRU) == AMD64_RAM_OK);
	assert(amd64_ram_lookup(&builder, 0x200000, &entry) && (entry & AMD64_PTE_WRITE) == 0);
	assert(amd64_ram_lookup(&builder, 0x201000, &entry) && (entry & AMD64_PTE_WRITETHRU) != 0);
	assert(root[0] == 0 && root[511] == 0 && root[384] == 0);
	puts("PASS: sparse RAM tables, 1/4 GiB boundaries, holes, permissions, window and arena exhaustion");
	return 0;
}
