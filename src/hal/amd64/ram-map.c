/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "ram-map.h"
#include "defs.h"

#define RAM_LARGE_SIZE UINT64_C(0x200000)
#define RAM_LEAF_FLAGS (AMD64_PTE_WRITE | AMD64_PTE_WRITETHRU | AMD64_PTE_NOCACHE | AMD64_PTE_GLOBAL | AMD64_PTE_NX)

static enum amd64_ram_result child_table(struct amd64_ram_builder *builder, uint64_t *parent, unsigned index, uint64_t **child);

/*
 * Maps one complete RAM extent without spanning its boundary with a large page.
 * The caller splits extents at cache and permission boundaries. An error may
 * leave unpublished partial tables; it never grants an overlapping mapping.
 */
enum amd64_ram_result
amd64_ram_map(
	struct amd64_ram_builder *builder,
	uint64_t physical,
	uint64_t size,
	uint64_t flags)
{
	uint64_t address;
	uint64_t step;
	uint64_t *pdpt;
	uint64_t *pd;
	uint64_t *pt;
	uint64_t *leaf;
	enum amd64_ram_result result;

	if (builder == 0 || builder->root == 0 || builder->allocate == 0 ||
	    builder->resolve == 0 || size == 0 ||
	    ((physical | size) & (PAGE_SIZE - 1U)) != 0 ||
	    physical >= AMD64_RAM_LIMIT || size > AMD64_RAM_LIMIT - physical ||
	    physical > builder->physical_max || size - 1U > builder->physical_max - physical ||
	    (flags & ~RAM_LEAF_FLAGS) != 0 || (flags & AMD64_PTE_NX) == 0)
		return AMD64_RAM_INVALID;
	while (size != 0) {
		address = AMD64_RAM_BASE + physical;
		result = child_table(builder, builder->root, (unsigned)((address >> 39) & 511U), &pdpt);
		if (result != AMD64_RAM_OK)
			return result;
		result = child_table(builder, pdpt, (unsigned)((address >> 30) & 511U), &pd);
		if (result != AMD64_RAM_OK)
			return result;
		leaf = &pd[(address >> 21) & 511U];
		if ((physical & (RAM_LARGE_SIZE - 1U)) == 0 && size >= RAM_LARGE_SIZE) {
			if (*leaf != 0)
				return AMD64_RAM_COLLISION;
			step = RAM_LARGE_SIZE;
			*leaf = physical | flags | AMD64_PTE_PRESENT | AMD64_PTE_LARGE;
			builder->large_pages++;
		} else {
			result = child_table(builder, pd, (unsigned)((address >> 21) & 511U), &pt);
			if (result != AMD64_RAM_OK)
				return result;
			leaf = &pt[(address >> 12) & 511U];
			if (*leaf != 0)
				return AMD64_RAM_COLLISION;
			step = PAGE_SIZE;
			*leaf = physical | flags | AMD64_PTE_PRESENT;
			builder->small_pages++;
		}
		builder->mapped_bytes += step;
		physical += step;
		size -= step;
	}
	return AMD64_RAM_OK;
}

/*
 * Confirms that a physical byte has a matching present RAM alias.
 */
int
amd64_ram_lookup(
	const struct amd64_ram_builder *builder,
	uint64_t physical,
	uint64_t *entry)
{
	uint64_t address;
	uint64_t value;
	uint64_t mask;
	uint64_t *table;
	unsigned shift;

	if (builder == 0 || builder->root == 0 || builder->resolve == 0 || entry == 0 ||
	    physical >= AMD64_RAM_LIMIT || physical > builder->physical_max)
		return 0;
	address = AMD64_RAM_BASE + physical;
	table = builder->root;
	for (shift = 39; shift >= 12; shift -= 9) {
		value = table[(address >> shift) & 511U];
		if ((value & AMD64_PTE_PRESENT) == 0 || (value & AMD64_PTE_USER) != 0)
			return 0;
		if (shift == 12 || (shift == 21 && (value & AMD64_PTE_LARGE) != 0)) {
			mask = (UINT64_C(1) << shift) - 1U;
			if ((value & AMD64_PTE_ADDR_MASK & ~mask) != (physical & ~mask))
				return 0;
			*entry = value;
			return 1;
		}
		if ((value & AMD64_PTE_LARGE) != 0)
			return 0;
		table = builder->resolve(builder->context, value & AMD64_PTE_ADDR_MASK);
		if (table == 0)
			return 0;
	}
	return 0;
}

/* Acquires a child table without replacing a leaf or an existing owner. */
static enum amd64_ram_result
child_table(
	struct amd64_ram_builder *builder,
	uint64_t *parent,
	unsigned index,
	uint64_t **child)
{
	uint64_t physical;
	unsigned slot;

	if ((parent[index] & AMD64_PTE_PRESENT) != 0) {
		if ((parent[index] & (AMD64_PTE_LARGE | AMD64_PTE_USER)) != 0)
			return AMD64_RAM_COLLISION;
		*child = builder->resolve(builder->context, parent[index] & AMD64_PTE_ADDR_MASK);
		return *child != 0 ? AMD64_RAM_OK : AMD64_RAM_INVALID;
	}
	if (parent[index] != 0)
		return AMD64_RAM_COLLISION;
	if (!builder->allocate(builder->context, &physical, child))
		return AMD64_RAM_NOMEM;
	if (*child == 0 || (physical & (PAGE_SIZE - 1U)) != 0 ||
	    physical > builder->physical_max || PAGE_SIZE - 1U > builder->physical_max - physical ||
	    physical > AMD64_PTE_ADDR_MASK)
		return AMD64_RAM_INVALID;
	for (slot = 0; slot < 512; slot++)
		(*child)[slot] = 0;
	parent[index] = physical | AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	builder->table_pages++;
	return AMD64_RAM_OK;
}
