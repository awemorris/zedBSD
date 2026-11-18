/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the GPU execution tests share among their files.
 *
 * The compute test owns the request path every test submits through: one
 * execbuf-shaped request on a fresh context of the GT address space
 * (i915_request_create, gen8_emit_init_breadcrumb, gen8_emit_bb_start,
 * i915_request_add), a bounded wait for it to retire, the switch back to the
 * kernel context, and the dump and engine reset after a hang.  The draw tests
 * and the full-HD draw reuse that path through the functions declared here.
 */

#ifndef DRIVERS_GPU_I915_TESTS_EXECUTION_EU_INTERNAL_H
#define DRIVERS_GPU_I915_TESTS_EXECUTION_EU_INTERNAL_H

#include "../../context.h"
#include "../../request.h"
#include "../../memory.h"
#include "eu-test.h"

#include <stddef.h>
#include <stdint.h>

struct i915_device;
struct i915_execlists;
struct i915_gt_engine;
struct i915_gt_engines;
struct i915_mmio;
struct i915_sseu;
struct i915_gt_ppgtt;
struct i915_wa_list;
struct spinlock;

/* How a test ended: the fixture's promise held, the request never retired, or something else failed. */
#define I915_TEST_EU_PASS		1
#define I915_TEST_EU_HANG		2
#define I915_TEST_EU_ERROR		3

/* How many follow-up rounds the compute test can run after its first pass. */
#define I915_TEST_EU_ROUNDS_MAX		8U

/* The FNV-1a 64 offset basis every fixture hash starts from. */
#define I915_TEST_FNV_BASIS		0xcbf29ce484222325ULL

/* How long a test waits for one request to retire. */
#define I915_TEST_TIMEOUT_MS		2000U

/*
 * One follow-up round of the compute test.
 *
 * Records what the same batch did on the first context (the next requests of
 * its timeline) or on a newly created one.
 */
struct i915_test_eu_round {
	/* 'A' for the first context, 'B' for the new one. */
	char ctx;

	/* The wait's result and whether the request completed, the engine parked and every marker landed. */
	int rc;
	int completed;
	int parked;
	int pass;

	/* The seqno the request carried and the one the status page showed. */
	uint32_t seqno;
	uint32_t hwsp_observed;

	/* The four markers as read back. */
	uint32_t ready;
	uint32_t eu;
	uint32_t done;
	uint32_t cs;

	/* Whether the descriptor and the kernel the GPU read back match what the CPU wrote. */
	int idd_rb_ok;
	int kernel_rb_ok;

	/* How many polls the wait took. */
	unsigned polls;

	/* The context, the ring span of the request and the last context status event. */
	uint32_t lrca;
	uint32_t ring_head;
	uint32_t ring_tail;
	uint32_t csb_hi;
	uint32_t csb_lo;
};

/*
 * The compute test, and the request path of every test.
 *
 * One instance lives for one scenario run; the draw tests embed one as their
 * first member for the context, the request, the shared page and the hang
 * record.  The release gives back everything the run created.
 */
struct i915_test_eu {
	/* The render engine's index in the GT's engine table. */
	unsigned engine_idx;

	/* The GT version the fixture is built for: 9, or 12 for every other value (the GT's version in the draw tests). */
	unsigned graphics_ver;

	/* How many fixture page entries the release put back to scratch. */
	unsigned ptes_scrubbed;

	/* The test's context, its timeline page and its last seqno. */
	struct i915_gt_context ce;
	struct i915_gt_object *tl_page;
	uint32_t tl_seqno;

	/* The shared page (state, markers, readbacks) and the batch. */
	struct i915_gt_object *shared;
	struct i915_gt_object *batch;
	unsigned batch_dwords;

	/* The VFE thread limit and the enabled dual-subslices it was computed from. */
	uint32_t max_threads;
	unsigned dss_count;

	/* The test's request and the kernel-context request that parks the engine. */
	struct i915_gt_request rq;
	struct i915_gt_request krq;

	/* The follow-up rounds: the second context, its timeline and the round request. */
	struct i915_gt_context ce2;
	struct i915_gt_object *tl_page2;
	uint32_t tl_seqno2;
	struct i915_gt_request rrq;
	struct i915_test_eu_round round[I915_TEST_EU_ROUNDS_MAX];
	unsigned n_rounds;
	unsigned rounds_passed;

	/* What happened to the request. */
	int submitted;
	int completed;
	int parked;
	int timed_out;
	int wedged;
	unsigned polls;

	/* What the shared page says afterwards. */
	uint32_t ready;
	uint32_t eu;
	uint32_t done;
	uint32_t cs;
	uint32_t idd_rb[8];
	uint32_t kernel_rb[I915_TEST_EU_KERNEL_DWORDS];
	int idd_rb_ok;
	int kernel_rb_ok;

	/* The fixture's identity: FNV-1a 64 over the batch dwords and over the shared fixture. */
	uint64_t batch_hash;
	uint64_t fixture_hash;

	/* The PIPELINE_SELECT words read back from the object that is submitted. */
	struct i915_test_pipeline_select pipesel;
	int pipesel_rc;

	/* What the GPU walks for the fixture addresses, read from the submitted tables. */
	struct i915_test_ppgtt_walk walk[5];
	unsigned walks;
	int pdp0_matches_top;

	/* The hang record, taken before the reset. */
	uint32_t hwsp_seqno_observed;
	uint32_t ctx_ccid_hi;
	uint32_t ctx_ccid_lo;
	int time_base_fault;

	/* The outcome, the first error and the step that reported it. */
	int outcome;
	int err;
	const char *err_where;
};

/*
 * A steered read of one multicast register.
 *
 * The workaround check reads multicast registers through the steering the
 * hardware defaults to; this record says what each dual-subslice returns.
 */
struct i915_test_mcr_entry {
	uint32_t reg;
	unsigned group;
	unsigned instance;
	uint32_t raw;
	uint32_t expected_set;
	uint32_t read_mask;
	uint32_t masked_mismatch;
	uint32_t selector_before;
	uint32_t selector_after;

	/* Nonzero when the register is in the workaround list. */
	int listed;
};

/* How many steered reads one probe records. */
#define I915_TEST_MCR_MAX		32U

/*
 * The steered reads of the engine workaround registers.
 *
 * Filled once before the compute test and logged with it.
 */
struct i915_test_mcr_probe {
	struct i915_test_mcr_entry e[I915_TEST_MCR_MAX];
	unsigned n;
	unsigned mismatches;
	int lock_rc;
};

/*
 * The store kernel of the compute test.
 *
 * SIMD8: an unconditional A64 untyped write of 0xc0ffee02 to the EU marker,
 * then the end-of-thread message.  The table never changes.
 */
extern const uint32_t drv_i915_test_eu_kernel[I915_TEST_EU_KERNEL_DWORDS];

/*
 * The same store kernel for Gen9.
 *
 * The same stores in Gen9's encoding, with no software scoreboard, the
 * address and the data in one message and a NOP after the end of the
 * thread.  The table never changes.
 */
extern const uint32_t drv_i915_test_eu_kernel_gen9[I915_TEST_EU_KERNEL_DWORDS];

int drv_i915_test_eu_fail(struct i915_test_eu *t, int rc, const char *where);
int drv_i915_test_eu_wait_retired(struct i915_test_eu *t, struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_gt_request *rq, struct i915_mmio *m, unsigned timeout_ms);
int drv_i915_test_eu_build_request(struct i915_test_eu *t, struct i915_gt_request *rq, struct i915_gt_context *ce, struct i915_gt_object *tl_page, uint32_t seqno, uint64_t batch_va);
int drv_i915_test_eu_park(struct i915_test_eu *t, struct i915_gt_engines *es, struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *m, unsigned timeout_ms);
void drv_i915_test_eu_log_record(struct i915_test_eu *t, struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_gt_request *rq, struct i915_gt_context *ce, const char *path);
void drv_i915_test_eu_hang_dump_reset(struct i915_test_eu *t, struct i915_gt_engines *es, struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *m, struct spinlock *uncore_lock);
void drv_i915_test_eu_scrub_ptes(struct i915_test_eu *t, struct i915_gt_ppgtt *vm, uint64_t va, unsigned pages);
int drv_i915_test_eu_invalidate_tlb(struct i915_device *device);
uint64_t drv_i915_test_fnv1a64(const void *data, size_t bytes, uint64_t hash);
int drv_i915_test_forcewake_get_all(struct i915_device *device, unsigned *held);
void drv_i915_test_forcewake_put_all(struct i915_device *device, unsigned held);
const char *drv_i915_test_eu_outcome_name(int outcome);

int drv_i915_test_eu_run(struct i915_test_eu *t, struct i915_gt_engines *es, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm, const struct i915_sseu *sseu, struct i915_mmio *m, struct spinlock *uncore_lock, unsigned timeout_ms, unsigned graphics_ver);
int drv_i915_test_eu_repeat(struct i915_test_eu *t, struct i915_gt_engines *es, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm, struct i915_mmio *m, struct spinlock *uncore_lock, unsigned timeout_ms, unsigned same_ctx, unsigned new_ctx);
void drv_i915_test_eu_release(struct i915_test_eu *t, struct i915_gt_mem *gm);
int drv_i915_test_mcr_probe_wa(struct i915_test_mcr_probe *probe, struct i915_mmio *m, const struct i915_wa_list *wal, const struct i915_sseu *sseu);

#endif
