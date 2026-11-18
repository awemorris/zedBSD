/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-process GTTs: 48-bit, four levels of 512 entries.
 *
 * Two independent address-space implementations live here and are not
 * merged yet:
 *
 *   kernel PPGTT   (struct i915_gt_ppgtt, drv_i915_gt_ppgtt_*)
 *       The address space of the GT's kernel contexts, which get a PPGTT of
 *       their own rather than the GGTT.  Its tables are GT objects; every
 *       entry write is clflushed, because the GPU's table walker does not
 *       snoop the CPU caches.
 *
 *   session PPGTT  (struct i915_ppgtt, drv_i915_ppgtt_*)
 *       The private address space of one GPU session.  Its tables are
 *       single pages from the page manager written through the direct map,
 *       and the LLC keeps the table walks coherent with those stores.
 *       Ranges come from a bump allocator and are never reused.
 *
 * Facts both are built on:
 *   - four levels for 48 bits, so the top level is 3 and the top table
 *     has 512 entries
 *   - no page is mapped read-only
 *   - PAT index 3 is UC in the PAT this driver programs (WB, WC, WT, UC,
 *     WB, WB, WB, WB); index 3 sets PAT0 (bit 3) and PAT1 (bit 4)
 *   - an uncached directory entry is address | PRESENT | RW | PWT | PCD
 *     (bits 3 and 4)
 */

#ifndef DRIVERS_GPU_I915_PPGTT_H
#define DRIVERS_GPU_I915_PPGTT_H

#include <kern/pmem.h>
#include <stdint.h>

struct i915_gt_mem;
struct i915_gt_object;

/*
 * The Gen12 PPGTT entry bits of the kernel PPGTT.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gtt.h
 * (GEN8_PAGE_PRESENT, GEN8_PAGE_RW, GEN12_PPGTT_PTE_PAT0,
 * GEN12_PPGTT_PTE_PAT1, GEN12_PPGTT_PTE_PAT2, PPAT_UNCACHED).
 */
#define I915_PPGTT_BIT_PRESENT		(((uint64_t)1) << 0)
#define I915_PPGTT_BIT_RW		(((uint64_t)1) << 1)
#define I915_PPGTT_BIT_PAT0		(((uint64_t)1) << 3)
#define I915_PPGTT_BIT_PAT1		(((uint64_t)1) << 4)
#define I915_PPGTT_BIT_PAT2		(((uint64_t)1) << 7)
#define I915_PPGTT_DIR_UNCACHED		((((uint64_t)1) << 3) | (((uint64_t)1) << 4))

/* The uncached PAT index of the PAT this driver programs. */
#define I915_PAT_INDEX_CACHE_NONE	3U

/* The top level of the four-level tree and how many entries its table has. */
#define I915_PPGTT_TOP			3
#define I915_PPGTT_TOP_COUNT		512U

/* How many tables below the top the kernel PPGTT can allocate. */
#define I915_PPGTT_MAX_TABLES		16U

/* The session PPGTT: 48-bit canonical, four levels of 512 entries. */
#define I915_PPGTT_LEVELS		4U
#define I915_PPGTT_ENTRIES		512U
#define I915_PPGTT_ADDRESS_BITS		48U

/* Session objects are placed above the first 4 GiB, each on a 2 MiB boundary. */
#define I915_PPGTT_VA_START		0x100000000ULL
#define I915_PPGTT_VA_ALIGN		0x200000ULL

/* The address bits of a session PPGTT entry. */
#define I915_PPGTT_ADDRESS_MASK		(((1ULL << I915_PPGTT_ADDRESS_BITS) - 1ULL) & ~((uint64_t)4096U - 1ULL))

/*
 * One table the kernel PPGTT allocated below its top directory.
 *
 * It is recorded in the address space's table list, which is how a walk
 * finds the child an entry names; the address space frees it at destroy.
 */
struct i915_gt_ppgtt_table {
	/* The table page. */
	struct i915_gt_object *obj;

	/* The directory whose entry names this table, and that entry's index. */
	struct i915_gt_object *parent;
	unsigned idx;

	/* The table's level: 2 is a PDP, 1 a PD, 0 a PT. */
	int lvl;

	/* The table page's DMA address. */
	uint64_t dma;
};

/*
 * The kernel PPGTT: the scratch tower, the top directory and
 * every table allocated below it.
 *
 * Its tables are reached by the GPU through a context's PDP0 pair, by DMA
 * address; they are not bound into the GGTT.  The owner embeds it and
 * destroys it before the GT memory goes.
 */
struct i915_gt_ppgtt {
	/* scratch[0] is the data page; scratch[n] is a table filled with scratch[n - 1]'s encoding. */
	struct i915_gt_object *scratch[I915_PPGTT_TOP + 1];
	uint64_t scratch_encode[I915_PPGTT_TOP + 1];

	/* The top directory (PML4) and its DMA address, which PDP0 of a context holds. */
	struct i915_gt_object *top_pd;
	uint64_t top_pd_dma;

	/* How many entries the top directory has, and its level. */
	unsigned top_count;
	int top;

	/* Nonzero between a successful create and destroy. */
	int inited;

	/* The tables allocated below the top directory. */
	struct i915_gt_ppgtt_table tables[I915_PPGTT_MAX_TABLES];
	unsigned n_tables;
};

/* Receives one leaf table (PT) of a range, in ascending order. */
typedef void (*i915_gt_ppgtt_pt_fn)(struct i915_gt_ppgtt *pp, struct i915_gt_object *pt, uint64_t pt_dma, void *data);

/*
 * One page-table page of a session PPGTT, kept for release at destroy.
 *
 * The walk reads child addresses out of the entries, so this list only has
 * to remember which pages to return to the pool.
 */
struct i915_ppgtt_page {
	struct kern_pmem run;
	struct i915_ppgtt_page *next;
};

/*
 * One session's private 48-bit address space.
 *
 * The scratch chain (page, table, directory, directory pointer) backs every
 * range no object occupies.  Ranges are handed out by a bump allocator and
 * never reused within the lifetime of the space.
 */
struct i915_ppgtt {
	/* The top table and the scratch chain with each level's scratch entry. */
	struct kern_pmem pml4;
	struct kern_pmem scratch[I915_PPGTT_LEVELS];
	uint64_t scratch_entry[I915_PPGTT_LEVELS];

	/* The tables inserts allocated, and how many. */
	struct i915_ppgtt_page *pages;
	unsigned page_count;

	/* The next range the bump allocator hands out. */
	uint64_t next_va;

	/* Nonzero between a successful create and destroy. */
	unsigned created;

	/* A space whose session closed while quarantined waits on the device list for reset. */
	uint32_t owner;
	struct i915_ppgtt *next;
};

uint64_t drv_i915_ppgtt_leaf_encode(uint64_t dma, unsigned pat_index);
uint64_t drv_i915_ppgtt_dir_encode_uncached(uint64_t dma);
uint64_t drv_i915_ppgtt_dir_encode(uint64_t dma);

int drv_i915_gt_ppgtt_create(struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp);
void drv_i915_gt_ppgtt_destroy(struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp);
int drv_i915_gt_ppgtt_alloc_range(struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp, uint64_t start, uint64_t length);
int drv_i915_gt_ppgtt_foreach_pt(struct i915_gt_ppgtt *pp, uint64_t start, uint64_t length, i915_gt_ppgtt_pt_fn fn, void *data);
int drv_i915_gt_ppgtt_insert_page(struct i915_gt_ppgtt *pp, uint64_t dma, uint64_t offset, unsigned pat_index);
int drv_i915_gt_ppgtt_insert_scratch(struct i915_gt_ppgtt *pp, uint64_t offset);

int drv_i915_ppgtt_create(struct i915_ppgtt *vm);
void drv_i915_ppgtt_destroy(struct i915_ppgtt *vm);
int drv_i915_ppgtt_va_alloc(struct i915_ppgtt *vm, uint64_t bytes, uint64_t *va);
int drv_i915_ppgtt_insert(struct i915_ppgtt *vm, uint64_t va, uint64_t physical, unsigned pages);
int drv_i915_ppgtt_insert_uncached(struct i915_ppgtt *vm, uint64_t va, uint64_t physical, unsigned pages);
void drv_i915_ppgtt_clear(struct i915_ppgtt *vm, uint64_t va, unsigned pages);

#endif
