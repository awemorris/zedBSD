/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU execution tests on the started GT.
 *
 * Each test builds a fixed batch, submits it on the render engine through the
 * GT's own context, request and execlists code on a fresh context of the GT
 * address space, waits for the request to retire, and compares what the GPU
 * wrote with what the fixture promises.  The GPU-free helpers here (the batch
 * builders, the pipeline-select check, the page-table walk and the address
 * layout of the full-HD draw) are also used by the unit tests.
 */

#ifndef DRIVERS_GPU_I915_TESTS_EXECUTION_EU_TEST_H
#define DRIVERS_GPU_I915_TESTS_EXECUTION_EU_TEST_H

#include <stdint.h>

struct i915_gt_ppgtt;

/* Where the compute test's shared page and batch sit in the GT address space. */
#define I915_TEST_EU_SHARED_VA		0x100400000ULL
#define I915_TEST_EU_BATCH_VA		0x100401000ULL

/* Where the interface descriptor and the kernel sit in the shared page. */
#define I915_TEST_EU_IDD_OFFSET		896U
#define I915_TEST_EU_KSP_OFFSET		1024U

/* Where the markers sit in the shared page; the EU offset is hard-coded in the store kernel. */
#define I915_TEST_EU_READY_OFF		0xc00U
#define I915_TEST_EU_EU_OFF		0xc20U
#define I915_TEST_EU_DONE_OFF		0xc28U
#define I915_TEST_EU_CS_OFF		0xc30U

/* Where the batch copies back the interface descriptor and the kernel as the GPU read them. */
#define I915_TEST_EU_IDD_RB_OFF		0xd00U
#define I915_TEST_EU_KERNEL_RB_OFF	0xe00U

/* The values the markers end up with when every step ran. */
#define I915_TEST_EU_READY_TAG		0xc0ffee10U
#define I915_TEST_EU_STORE_TAG		0xc0ffee02U
#define I915_TEST_EU_DONE_TAG		0xc0ffee20U
#define I915_TEST_EU_CS_TAG		0xc0ffee30U

/* How long the store kernel is. */
#define I915_TEST_EU_KERNEL_DWORDS	36U

/* Where the draw tests' render target sits. */
#define I915_TEST_DRAW_RT_VA		0x100402000ULL

/* Where the mixed test's draw batch sits, beside the compute batch. */
#define I915_TEST_R1_DRAW_BATCH_VA	0x100403000ULL

/* The byte the texture tests fill the guard after each texture with. */
#define I915_TEST_TEX_GUARD_BYTE	0xa5U

/*
 * What a walk of the GT address space found for one address.
 *
 * The walk reads the tables the way the GPU does, top level first, and stops
 * at the leaf or at the first entry that does not name one of this space's
 * own tables.
 */
struct i915_test_ppgtt_walk {
	/* The address walked and what PDP0 of a context must hold for it. */
	uint64_t va;
	uint64_t top_dma;

	/* The PML4, PDP, PD and PT indices of the address. */
	unsigned idx[4];

	/* The raw entries read, top level first. */
	uint64_t raw[4];

	/* The address bits of the three directory entries, and whether each names one of this space's tables. */
	uint64_t child_dma[3];
	int child_known[3];

	/* Whether each entry equals the scratch encoding of its level. */
	int scratch[4];

	/* How many entries were read; 4 means the walk reached the page entry. */
	int levels;

	/* What the page entry says: the page, whether it is present and writable, and its PAT index. */
	uint64_t leaf_dma;
	int leaf_present;
	int leaf_rw;
	unsigned leaf_pat;
};

/*
 * What a batch holds in the way of PIPELINE_SELECT words.
 *
 * The check compares against fixed Gen12.0 words (or Gen9 words), not against
 * the emitter's macro, so a mis-encoded select is caught however it was
 * produced.
 */
struct i915_test_pipeline_select {
	/* How many 3D and GPGPU selects, and how many unexpected select-like words. */
	unsigned n_3d;
	unsigned n_gpgpu;
	unsigned n_bad;

	/* Where the first of each was found. */
	unsigned idx_3d;
	unsigned idx_gpgpu;
	unsigned idx_bad;

	/* The first unexpected word. */
	uint32_t bad_word;
};

/*
 * One range of the full-HD draw's address layout.
 *
 * The layout table is fixed; the unit tests check that no two ranges overlap
 * and that every range is aligned.
 */
struct i915_test_fhd_va {
	const char *name;
	uint64_t va;
	uint64_t len;
	uint64_t align;
};

int drv_i915_test_ppgtt_walk(struct i915_gt_ppgtt *pp, uint64_t va, struct i915_test_ppgtt_walk *walk);
unsigned drv_i915_test_eu_build_batch(uint32_t *cmds, unsigned capacity, uint64_t shared_va, uint64_t inst_base, uint32_t max_threads);
unsigned drv_i915_test_eu_build_batch_gen(uint32_t *cmds, unsigned capacity, uint64_t shared_va, uint64_t inst_base, uint32_t max_threads, unsigned graphics_ver);
int drv_i915_test_eu_check_pipeline_select(const uint32_t *cmds, unsigned count, struct i915_test_pipeline_select *check);
int drv_i915_test_eu_check_pipeline_select_gen(const uint32_t *cmds, unsigned count, struct i915_test_pipeline_select *check, unsigned graphics_ver);
void drv_i915_test_eu_fill_idd(uint32_t *idd, unsigned graphics_ver);
int drv_i915_test_draw_check_pipeline_select(const uint32_t *cmds, unsigned count, struct i915_test_pipeline_select *check);
int drv_i915_test_fhd_va_layout(const struct i915_test_fhd_va **layout, unsigned *count);

#endif
