/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compute test on the started GT, and the request path of every GPU test.
 *
 * The compute test dispatches one SIMD8 thread with GPGPU_WALKER whose kernel
 * does an unconditional A64 store of 0xc0ffee02 and ends the thread.  The
 * kernel, the interface descriptor, the VFE state and the command order are
 * those that completed on the reference driver on the same GPU.  The batch
 * also copies the descriptor and the kernel back through the context's
 * address space, so a wrong mapping shows as a wrong readback.  Identity with
 * earlier runs is established by hashing the submitted bytes.
 *
 * The request is shaped like an execbuf request: i915_request_create (the
 * invalidating flush), gen8_emit_init_breadcrumb (the seqno - 1 store and
 * MI_ARB_CHECK), gen8_emit_bb_start (arbitration on around a PPGTT batch
 * start) and i915_request_add (the closing breadcrumb), on a fresh context of
 * the GT address space that inherits the engine's default state.  The test
 * passes when the command-streamer markers land and the EU store is visible;
 * it hangs when the request does not retire in time, and the engines are then
 * reset the way intel_gt_set_wedged would.
 */

#include "eu-internal.h"
#include "scenarios.h"
#include <kern/kcrt.h>

#include "../../context.h"
#include "../../engine.h"
#include "../../ggtt.h"
#include "../../gt.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../mmio.h"
#include "../../ppgtt.h"
#include "../../request.h"
#include "../../reset.h"
#include "../../submit.h"
#include "../../sync.h"
#include "../../tlb.h"
#include "../../workarounds.h"

#include <kern/klog.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../../intel/commands.h"
#include "../../intel/gt-regs.h"

/* MI_COPY_MEM_MEM with both addresses in the context's address space. */
#define I915_TEST_MI_COPY_MEM_MEM	0x17000003U

/* STATE_BASE_ADDRESS and its length. */
#define I915_TEST_STATE_BASE_ADDRESS	((0x6101U << 16) | (22U - 2U))

/* The Gen12 MOCS field value of a MOCS table index. */
#define I915_TEST_GEN12_MOCS(index)	((index) << 1)

/* The uncached and the write-back MOCS table entries. */
#define I915_TEST_MOCS_UNCACHED		3U
#define I915_TEST_MOCS_WRITEBACK	2U

/* PIPE_CONTROL's stall-at-scoreboard bit. */
#define I915_TEST_PC_STALL_AT_SCOREBOARD	(1U << 1)

/* PIPE_CONTROL's post-sync operation "write immediate data". */
#define I915_TEST_PC_POST_SYNC_WRITE	(1U << 14)

/* The value a marker holds until the GPU writes it. */
#define I915_TEST_EU_UNWRITTEN		0xdead0000U

/* How many pages from the shared page up the fixtures of every test occupy. */
#define I915_TEST_EU_FIXTURE_PAGES	6U

/* The five forcewake domains the tests hold while they run, as the device start takes them. */
#define I915_TEST_FORCEWAKE_DOMAINS	5U

/* The multicast selector and its slice and subslice fields. */
#define I915_TEST_MCR_SELECTOR		0x0fdcU
#define I915_TEST_MCR_SLICE_MASK	0x78000000U
#define I915_TEST_MCR_SUBSLICE_MASK	0x07000000U
#define I915_TEST_MCR_SLICE_SHIFT	27U
#define I915_TEST_MCR_SUBSLICE_SHIFT	24U

/* How many engine workaround registers the steered probe reads. */
#define I915_TEST_MCR_REGS		3U

/* How many requests the follow-up rounds submit on the first and on a new context. */
#define I915_TEST_EU_SAME_CONTEXT	3U
#define I915_TEST_EU_NEW_CONTEXT	2U

/*
 * A batch under construction.
 *
 * The builder keeps counting past the capacity, so an overflow is reported
 * once at the end instead of after every dword.
 */
struct i915_test_batch {
	/* The words and how many of them the batch has room for. */
	uint32_t *cmds;
	unsigned capacity;

	/* How many words were emitted, including any that did not fit. */
	unsigned count;

	/* Nonzero when a word did not fit. */
	int overflow;
};

/*
 * The store kernel of the compute test.
 *
 * SIMD8: an unconditional A64 untyped write of 0xc0ffee02 to the EU marker
 * (the address 0x100400c20 is in the kernel), then the end-of-thread message.
 * The table never changes.
 */
const uint32_t drv_i915_test_eu_kernel[I915_TEST_EU_KERNEL_DWORDS] = {
	0x00030061U, 0x05054220U, 0x00000000U, 0xc0ffee02U,
	0x80030061U, 0x7f050220U, 0x00460005U, 0x00000000U,
	0x80030061U, 0x01264aa0U, 0x00000000U, 0x00000001U,
	0x80030161U, 0x01064aa0U, 0x00000000U, 0x00400c20U,
	0x80000101U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x00030061U, 0x03260660U, 0x00000124U, 0x00000000U,
	0x00030161U, 0x03060660U, 0x00000104U, 0x00000000U,
	0x00039031U, 0x00000000U, 0xcdfa0314U, 0x019a050cU,
	0x80030131U, 0x00000004U, 0x70007f0cU, 0x00000000U
};

/*
 * The engine workaround registers the command-streamer verify cannot read.
 *
 * GEN8_ROW_CHICKEN2, GEN10_SAMPLER_MODE and GEN9_ROW_CHICKEN4 are multicast;
 * the probe reads them once per dual-subslice.  The table never changes.
 */
static const uint32_t i915_test_mcr_regs[I915_TEST_MCR_REGS] = {
	0xe4f4U,
	0xe18cU,
	0xe48cU
};

/*
 * The forcewake domains the tests hold, in the order they are taken.
 *
 * The same five the device start holds; taking them again only adds
 * references.  The table never changes.
 */
static const int i915_test_forcewake_domains[I915_TEST_FORCEWAKE_DOMAINS] = {
	I915_FORCEWAKE_RENDER,
	I915_FORCEWAKE_GT,
	I915_FORCEWAKE_MEDIA_VDBOX0,
	I915_FORCEWAKE_MEDIA_VDBOX2,
	I915_FORCEWAKE_MEDIA_VEBOX0
};

/*
 * The steered probe of the compute scenario.
 *
 * Too large for the start worker's stack; used only by the one scenario run
 * of a boot, on the start worker.
 */
static struct i915_test_mcr_probe i915_test_mcr;

/*
 * The compute test of the compute scenario.
 *
 * Too large for the start worker's stack.  Filled by the one scenario run of
 * a boot; after a hang its objects are kept for ever, because the GPU may
 * still use them.
 */
static struct i915_test_eu i915_test_eu_state;

/*
 * The GT TLB invalidation record of the tests.
 *
 * Counts the invalidations the tests asked for; the tests run one at a time
 * on the start worker, so nothing else touches it.
 */
static struct i915_gt_tlb i915_test_tlb;

static void i915_eu_emit(struct i915_test_batch *batch, uint32_t dword);
static void i915_eu_emit_pipe_control(struct i915_test_batch *batch, uint32_t flags);
static void i915_eu_emit_marker(struct i915_test_batch *batch, uint64_t va, uint32_t value);
static void i915_eu_emit_copy(struct i915_test_batch *batch, uint64_t destination, uint64_t source);
static void i915_eu_emit_state_base_address(struct i915_test_batch *batch, uint64_t shared, uint64_t inst_base, uint32_t mocs);
static void i915_eu_emit_vfe_and_walker(struct i915_test_batch *batch, uint32_t max_threads);
static uint32_t i915_eu_pipeline_select(uint32_t pipeline);
static int i915_eu_retired(struct i915_gt_request *rq, struct i915_execlists *el);
static int i915_eu_find_render(struct i915_test_eu *t, struct i915_gt_engines *es);
static int i915_eu_new_context(struct i915_test_eu *t, struct i915_gt_context *ce, struct i915_gt_object **tl_page, struct i915_gt_engine *ge, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm, unsigned repeat);
static int i915_eu_map_fixture(struct i915_test_eu *t, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm);
static void i915_eu_write_shared(struct i915_test_eu *t);
static void i915_eu_walk_fixture(struct i915_test_eu *t, struct i915_gt_ppgtt *vm);
static void i915_eu_read_back(struct i915_test_eu *t);
static void i915_eu_reset_markers(struct i915_test_eu *t);
static int i915_eu_round_context(struct i915_test_eu *t, struct i915_test_eu_round *round, struct i915_gt_engine *ge, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm, unsigned same_ctx, unsigned index, struct i915_gt_context **ce, struct i915_gt_object **tl);
static void i915_eu_read_round(struct i915_test_eu *t, struct i915_test_eu_round *round, struct i915_gt_context *ce, struct i915_execlists *el);
static uint32_t i915_eu_mcr_read_steered(struct i915_mmio *m, uint32_t reg, unsigned group, unsigned instance, uint32_t *selector_before, uint32_t *selector_after);
static void i915_eu_log_mcr(const struct i915_test_mcr_probe *probe, const struct i915_sseu *sseu);
static void i915_eu_log_run(struct i915_device *device, const struct i915_test_eu *t, int rc, unsigned user0, unsigned ctx0, unsigned error0);
static void i915_eu_log_walks(const struct i915_test_eu *t);
static void i915_eu_log_fixture(const struct i915_test_eu *t);
static void i915_eu_log_context(const struct i915_test_eu *t);
static void i915_eu_log_repeat(struct i915_device *device, const struct i915_test_eu *t, int rc, unsigned user0, unsigned ctx0, unsigned error0);
static void i915_eu_cleanup(struct i915_device *device, struct i915_test_eu *t, const char *name);

/*
 * Builds the compute test's batch into a buffer.
 *
 * The batch establishes 3D mode, programs the state base addresses, copies
 * the descriptor and the kernel back, switches to GPGPU, loads the VFE state
 * and the descriptor, dispatches one thread, writes the done tag with a
 * post-sync PIPE_CONTROL and a command-streamer marker, and ends.  Returns the
 * number of dwords, or 0 when the buffer was too small.
 */
unsigned
drv_i915_test_eu_build_batch(
	uint32_t *cmds,
	unsigned capacity,
	uint64_t shared_va,
	uint64_t inst_base,
	uint32_t max_threads)
{
	struct i915_test_batch batch;
	uint64_t done_va;
	uint32_t mocs;
	unsigned i;

	/* Starts an empty batch in the caller's buffer. */
	batch.cmds = cmds;
	batch.capacity = capacity;
	batch.count = 0U;
	batch.overflow = 0;
	mocs = I915_TEST_GEN12_MOCS(I915_TEST_MOCS_UNCACHED);
	done_va = shared_va + I915_TEST_EU_DONE_OFF;

	/* Establishes 3D mode; the base addresses are applied in 3D (Wa_1607854226). */
	i915_eu_emit_pipe_control(&batch,
	    PIPE_CONTROL_CS_STALL |
	    PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
	    PIPE_CONTROL_DEPTH_CACHE_FLUSH |
	    PIPE_CONTROL_DC_FLUSH_ENABLE |
	    PIPE_CONTROL_FLUSH_ENABLE);
	i915_eu_emit(&batch, i915_eu_pipeline_select(0U));
	i915_eu_emit_state_base_address(&batch, shared_va, inst_base, mocs);
	i915_eu_emit_pipe_control(&batch,
	    PIPE_CONTROL_CS_STALL |
	    PIPE_CONTROL_STATE_CACHE_INVALIDATE |
	    PIPE_CONTROL_CONST_CACHE_INVALIDATE |
	    PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
	    PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE);

	/* Has the GPU read the descriptor back through this context's address space. */
	for (i = 0U; i < 8U; i++) {
		i915_eu_emit_copy(&batch,
		    shared_va + I915_TEST_EU_IDD_RB_OFF + i * 4U,
		    shared_va + I915_TEST_EU_IDD_OFFSET + i * 4U);
	}

	/* Has the GPU read the kernel back the same way. */
	for (i = 0U; i < I915_TEST_EU_KERNEL_DWORDS; i++) {
		i915_eu_emit_copy(&batch,
		    shared_va + I915_TEST_EU_KERNEL_RB_OFF + i * 4U,
		    inst_base + I915_TEST_EU_KSP_OFFSET + i * 4U);
	}

	/* Switches 3D to GPGPU behind a stalling flush (the RT flush pulls in the HDC pipeline flush). */
	i915_eu_emit_pipe_control(&batch,
	    PIPE_CONTROL_CS_STALL |
	    PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
	    PIPE_CONTROL_DEPTH_CACHE_FLUSH);
	i915_eu_emit(&batch, i915_eu_pipeline_select(2U));
	i915_eu_emit_pipe_control(&batch, PIPE_CONTROL_CS_STALL | I915_TEST_PC_STALL_AT_SCOREBOARD);

	/* Loads the VFE state and the descriptor, marks readiness and dispatches the thread. */
	i915_eu_emit_vfe_and_walker(&batch, max_threads);

	/* Precedes the post-sync PIPE_CONTROL with a stalling flush without post-sync (Wa_1607156449). */
	i915_eu_emit_pipe_control(&batch,
	    PIPE_CONTROL_CS_STALL |
	    PIPE_CONTROL_DC_FLUSH_ENABLE |
	    PIPE_CONTROL_FLUSH_ENABLE);

	/* Writes the done tag into the context's address space as the post-sync operation. */
	i915_eu_emit(&batch, GFX_OP_PIPE_CONTROL(6));
	i915_eu_emit(&batch, PIPE_CONTROL_CS_STALL | I915_TEST_PC_POST_SYNC_WRITE);
	i915_eu_emit(&batch, (uint32_t)done_va);
	i915_eu_emit(&batch, (uint32_t)(done_va >> 32));
	i915_eu_emit(&batch, I915_TEST_EU_DONE_TAG);
	i915_eu_emit(&batch, 0U);

	/* Marks that the command streamer got past everything, and ends the batch. */
	i915_eu_emit_marker(&batch, shared_va + I915_TEST_EU_CS_OFF, I915_TEST_EU_CS_TAG);
	i915_eu_emit(&batch, MI_BATCH_BUFFER_END);
	i915_eu_emit(&batch, MI_NOOP);

	/* Refuses a batch that did not fit. */
	if (batch.overflow)
		return 0U;

	/* Succeeded: reports the batch length. */
	return batch.count;
}

/*
 * Checks the PIPELINE_SELECT words of a compute batch.
 *
 * Returns 0 when the batch holds exactly one 3D select (0x69041310) followed
 * by exactly one GPGPU select (0x69041312), and no other word with the
 * PIPELINE_SELECT header or with the 0x6104 header (GPGPU_CSR_BASE_ADDRESS,
 * which the compute test never emits); EINVAL otherwise.  The counts are
 * reported through check when it is not NULL.
 */
int
drv_i915_test_eu_check_pipeline_select(
	const uint32_t *cmds,
	unsigned count,
	struct i915_test_pipeline_select *check)
{
	struct i915_test_pipeline_select found;
	uint32_t dword;
	unsigned k;

	/* Counts every select-like word, remembering where the first of each kind is. */
	kern_memset(&found, 0, sizeof(found));
	for (k = 0U; k < count; k++) {
		dword = cmds[k];
		if (dword == 0x69041310U) {
			/* The fixed reference 3D select. */
			if (found.n_3d == 0U)
				found.idx_3d = k;
			found.n_3d++;
		} else if (dword == 0x69041312U) {
			/* The fixed reference GPGPU select. */
			if (found.n_gpgpu == 0U)
				found.idx_gpgpu = k;
			found.n_gpgpu++;
		} else if ((dword >> 16) == 0x6104U || (dword >> 16) == 0x6904U) {
			/* A select-like word that is neither. */
			if (found.n_bad == 0U) {
				found.idx_bad = k;
				found.bad_word = dword;
			}
			found.n_bad++;
		}
	}

	/* Reports the counts to a caller that wants them. */
	if (check != NULL)
		*check = found;

	/* Refuses anything but one 3D select followed by one GPGPU select. */
	if (found.n_3d != 1U)
		return EINVAL;
	if (found.n_gpgpu != 1U)
		return EINVAL;
	if (found.n_bad != 0U)
		return EINVAL;
	if (found.idx_3d >= found.idx_gpgpu)
		return EINVAL;

	/* Succeeded: the batch selects 3D, then GPGPU, and nothing else. */
	return 0;
}

/*
 * Checks the PIPELINE_SELECT words of a draw batch.
 *
 * Returns 0 when the batch holds exactly one 3D select, no GPGPU select and
 * no other select-like word; EINVAL otherwise.
 */
int
drv_i915_test_draw_check_pipeline_select(
	const uint32_t *cmds,
	unsigned count,
	struct i915_test_pipeline_select *check)
{
	struct i915_test_pipeline_select found;

	/* Counts the selects with the compute check, whose verdict does not apply here. */
	(void)drv_i915_test_eu_check_pipeline_select(cmds, count, &found);

	/* Reports the counts to a caller that wants them. */
	if (check != NULL)
		*check = found;

	/* Refuses anything but a single 3D select. */
	if (found.n_3d != 1U)
		return EINVAL;
	if (found.n_gpgpu != 0U)
		return EINVAL;
	if (found.n_bad != 0U)
		return EINVAL;

	/* Succeeded: the batch selects 3D once and nothing else. */
	return 0;
}

/*
 * Hashes bytes with FNV-1a 64, continuing from a previous hash.
 *
 * Start from I915_TEST_FNV_BASIS.  The fixture identities in the logs are
 * these hashes, so they must never change.
 */
uint64_t
drv_i915_test_fnv1a64(
	const void *data,
	size_t bytes,
	uint64_t hash)
{
	const uint8_t *byte;
	size_t i;

	/* Folds in every byte. */
	byte = data;
	for (i = 0U; i < bytes; i++) {
		hash ^= byte[i];
		hash *= 0x100000001b3ULL;
	}

	/* Reports the hash. */
	return hash;
}

/*
 * Records a test error and passes it back.
 *
 * Only the first error and its step are kept; the outcome becomes an error.
 */
int
drv_i915_test_eu_fail(
	struct i915_test_eu *t,
	int rc,
	const char *where)
{
	/* Keeps the first error and the step that reported it. */
	if (t->err == 0) {
		t->err = rc;
		t->err_where = where;
	}

	/* Marks the test failed and hands the error back to the caller's return. */
	t->outcome = I915_TEST_EU_ERROR;
	return rc;
}

/*
 * Waits until a request has retired.
 *
 * Polls the context status buffer every 50 us.  Retired means the request's
 * breadcrumb landed and the engine reported the context complete.  Returns 0,
 * EIO after a status buffer error or a time-base fault, or ETIMEDOUT.
 */
int
drv_i915_test_eu_wait_retired(
	struct i915_test_eu *t,
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_gt_request *rq,
	struct i915_mmio *m,
	unsigned timeout_ms)
{
	unsigned budget;
	unsigned k;
	int retired;
	int error;

	/* Polls for up to the timeout, 50 us per step. */
	budget = timeout_ms * 20U;
	for (k = 0U; k < budget; k++) {
		/* Takes in what the engine reported since the last poll. */
		t->polls++;
		(void)drv_i915_execlists_process_csb(ge, el, m);
		if (el->csb_errors != 0U)
			return EIO;

		/* Stops once the request is retired. */
		retired = i915_eu_retired(rq, el);
		if (retired)
			return 0;

		/* Waits one step; a failed wait is a time-base fault. */
		error = drv_i915_udelay(50U);
		if (error != 0) {
			error = drv_i915_test_eu_fail(t, EIO, "time base");
			return error;
		}
	}

	/* The request did not retire in time. */
	return ETIMEDOUT;
}

/*
 * Builds one execbuf-shaped request for a batch.
 *
 * i915_request_create (the invalidating flush), gen8_emit_init_breadcrumb
 * (the seqno - 1 store, MI_ARB_CHECK), gen8_emit_bb_start (arbitration on
 * around a PPGTT batch start) and i915_request_add (the closing breadcrumb).
 */
int
drv_i915_test_eu_build_request(
	struct i915_test_eu *t,
	struct i915_gt_request *rq,
	struct i915_gt_context *ce,
	struct i915_gt_object *tl_page,
	uint32_t seqno,
	uint64_t batch_va)
{
	uint32_t *cs;
	int error;

	/* Creates the request on the context's timeline; it has an initial breadcrumb. */
	error = drv_i915_request_create(rq, ce, seqno, (uint32_t)tl_page->ggtt_offset, (volatile uint32_t *)tl_page->cpu);
	if (error != 0) {
		error = drv_i915_test_eu_fail(t, error, "i915_request_create");
		return error;
	}

	/* Stores seqno - 1, which says the request has started (gen8_emit_init_breadcrumb). */
	cs = drv_i915_ring_begin(rq, 6U);
	if (cs == NULL) {
		error = drv_i915_test_eu_fail(t, rq->error, "emit_init_breadcrumb");
		return error;
	}

	*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
	*cs++ = rq->hwsp_ggtt;
	*cs++ = 0U;
	*cs++ = rq->seqno - 1U;
	*cs++ = MI_NOOP;
	*cs++ = MI_ARB_CHECK;
	drv_i915_ring_advance(rq, cs);

	/* Starts the batch in the context's address space with arbitration on (gen8_emit_bb_start). */
	cs = drv_i915_ring_begin(rq, 6U);
	if (cs == NULL) {
		error = drv_i915_test_eu_fail(t, rq->error, "emit_bb_start");
		return error;
	}

	*cs++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	*cs++ = MI_BATCH_BUFFER_START_GEN8 | (1U << 8);
	*cs++ = (uint32_t)batch_va;
	*cs++ = (uint32_t)(batch_va >> 32);
	*cs++ = MI_ARB_ON_OFF;
	*cs++ = MI_NOOP;
	drv_i915_ring_advance(rq, cs);

	/* Closes the request with its breadcrumb. */
	error = drv_i915_request_add(rq);
	if (error != 0) {
		error = drv_i915_test_eu_fail(t, error, "i915_request_add");
		return error;
	}

	/* Succeeded: the request is ready to submit. */
	return 0;
}

/*
 * Parks the engine on its kernel context.
 *
 * As intel_context_unpin and the engine park do, a request on the kernel
 * context switches the engine away from the test's context, unless nothing
 * was submitted since the last switch.  Returns 1 when the engine is parked.
 */
int
drv_i915_test_eu_park(
	struct i915_test_eu *t,
	struct i915_gt_engines *es,
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *m,
	unsigned timeout_ms)
{
	uint32_t kernel_seqno;
	int error;

	/* Nothing was submitted since the last switch: the engine is parked already. */
	if (el->wakeref_serial == el->serial)
		return 1;

	/* Creates and closes a request on the kernel context's timeline in the status page. */
	es->kernel_tl_seqno[t->engine_idx]++;
	kernel_seqno = es->kernel_tl_seqno[t->engine_idx];
	error = drv_i915_request_create(&t->krq,
	    &es->kernel_ce[t->engine_idx],
	    kernel_seqno,
	    (uint32_t)ge->hwsp_ggtt + I915_GEM_HWS_SEQNO_ADDR,
	    &ge->hwsp[I915_GEM_HWS_SEQNO_ADDR / 4U]);
	if (error == 0)
		error = drv_i915_request_add(&t->krq);

	/* The switch accounts for the serial even when it fails, as the engine park does. */
	el->wakeref_serial = el->serial + 1U;

	/* Submits the switch and waits for it. */
	if (error == 0)
		error = drv_i915_execlists_submit(ge, el, m, &t->krq);
	if (error == 0)
		error = drv_i915_test_eu_wait_retired(t, ge, el, &t->krq, m, timeout_ms);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, error, "switch_to_kernel_context");
		return 0;
	}

	/* Succeeded: the engine runs the kernel context. */
	return 1;
}

/*
 * Logs what a request and its context left behind.
 *
 * Written on every path, including a pass: the status page's raw seqno, the
 * context identity and the last context status event.
 */
void
drv_i915_test_eu_log_record(
	struct i915_test_eu *t,
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_gt_request *rq,
	struct i915_gt_context *ce,
	const char *path)
{
	int initial_seen;
	int seqno_reached;

	/* Samples the status page and the context descriptor. */
	t->hwsp_seqno_observed = *rq->hwsp_cpu;
	t->ctx_ccid_hi = (uint32_t)(ce->lrc_desc >> 32);
	t->ctx_ccid_lo = (uint32_t)ce->lrc_desc;

	/* A time-base fault is the error the wait records as "time base". */
	t->time_base_fault = 0;
	if (t->err == EIO && t->err_where != NULL && t->err_where[0] == 't')
		t->time_base_fault = 1;

	/* Tells whether the initial breadcrumb and the request's own seqno landed. */
	initial_seen = 0;
	if ((int32_t)(t->hwsp_seqno_observed - (rq->seqno - 1U)) >= 0)
		initial_seen = 1;
	seqno_reached = 0;
	if ((int32_t)(t->hwsp_seqno_observed - rq->seqno) >= 0)
		seqno_reached = 1;

	kern_logf("i915: EU-TEST record(%s): rq seqno expected=%u hwsp_observed=%u "
	    "initial_breadcrumb_seen=%d request_seqno_reached=%d | ctx sw_id=%u tag=%d lrca=%08x desc=%08x:%08x "
	    "state_ggtt=0x%llx ring_ggtt=0x%llx ring_emit=0x%x | csb_head=%u last_csb=%08x:%08x | time_base_fault=%d\n",
	    path,
	    rq->seqno,
	    t->hwsp_seqno_observed,
	    initial_seen,
	    seqno_reached,
	    ce->sw_id,
	    ce->tag,
	    ce->lrca,
	    t->ctx_ccid_hi,
	    t->ctx_ccid_lo,
	    (unsigned long long)ce->state->ggtt_offset,
	    (unsigned long long)ce->ring.ggtt_offset,
	    ce->ring.emit,
	    ge->csb_head,
	    el->last_csb_hi,
	    el->last_csb_lo,
	    t->time_base_fault);
}

/*
 * Dumps a hung engine, then resets every engine.
 *
 * The reset is the one intel_gt_set_wedged would do; nothing is submitted
 * afterwards and the test is marked wedged.
 */
void
drv_i915_test_eu_hang_dump_reset(
	struct i915_test_eu *t,
	struct i915_gt_engines *es,
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *m,
	struct spinlock *uncore_lock)
{
	uint32_t base;
	unsigned i;

	/* Dumps the engine and the registers that tell where the EUs stopped. */
	base = ge->info->mmio_base;
	drv_i915_engine_dump(ge, el, m, "eu-test");
	kern_logf("i915: EU-TEST hang: ipehr=%08x acthd=%08x:%08x instdone=%08x fault(0xcec4)=%08x "
	    "row_instdone(0xe164,raw)=%08x eu_dis(0x9134)=%08x slice_ack(0x804c)=%08x "
	    "ss01_eu_ack(0x805c)=%08x ss23_eu_ack(0x8060)=%08x\n",
	    drv_i915_read32(m, base + 0x68U),
	    drv_i915_read32(m, base + 0x5cU),
	    drv_i915_read32(m, base + 0x74U),
	    drv_i915_read32(m, base + 0x6cU),
	    drv_i915_read32(m, 0xcec4U),
	    drv_i915_read32(m, 0xe164U),
	    drv_i915_read32(m, 0x9134U),
	    drv_i915_read32(m, 0x804cU),
	    drv_i915_read32(m, 0x805cU),
	    drv_i915_read32(m, 0x8060U));

	/* Prepares every engine for the reset, then resets the GT. */
	for (i = 0U; i < es->n; i++)
		drv_i915_execlists_reset_prepare(&es->ge[i], m);
	(void)drv_i915_gt_reset_all(uncore_lock, m, 2000U);

	/* Nothing may be submitted after the reset. */
	t->wedged = 1;
}

/*
 * Puts a run of fixture pages back to scratch.
 *
 * Done before any page the entries named is freed.  Counts the entries that
 * were put back.
 */
void
drv_i915_test_eu_scrub_ptes(
	struct i915_test_eu *t,
	struct i915_gt_ppgtt *vm,
	uint64_t va,
	unsigned pages)
{
	unsigned page;
	int error;

	/* Refuses a test that never mapped anything. */
	if (vm == NULL)
		return;

	/* Points every page of the run at the scratch page. */
	for (page = 0U; page < pages; page++) {
		error = drv_i915_gt_ppgtt_insert_scratch(vm, va + (uint64_t)page * 4096U);
		if (error == 0)
			t->ptes_scrubbed++;
	}
}

/*
 * Invalidates the GT TLBs of the live device.
 *
 * Done after a test put its fixture pages back to scratch and before their
 * pages are freed, because the device keeps running afterwards.  Returns 0 or
 * the invalidation's error.
 */
int
drv_i915_test_eu_invalidate_tlb(
	struct i915_device *device)
{
	struct i915_gt *gt;
	int error;

	/* Invalidates every engine's TLB. */
	gt = &device->gt;
	error = drv_i915_gt_invalidate_tlb_full(&i915_test_tlb, &gt->engines, &gt->mmio, &gt->uncore_lock);
	if (error != 0)
		return error;

	/* Succeeded: no engine holds a translation of the scrubbed pages. */
	return 0;
}

/*
 * Takes the five forcewake domains the tests hold.
 *
 * Stops at the first domain that cannot be taken; *held says how many were
 * taken, so the put gives back exactly those.
 */
int
drv_i915_test_forcewake_get_all(
	struct i915_device *device,
	unsigned *held)
{
	int error;

	/* Takes the domains in order. */
	*held = 0U;
	while (*held < I915_TEST_FORCEWAKE_DOMAINS) {
		error = drv_i915_forcewake_get(&device->gt.mmio, i915_test_forcewake_domains[*held]);
		if (error != 0)
			return error;

		(*held)++;
	}

	/* Succeeded: every domain is held. */
	return 0;
}

/*
 * Gives back the forcewake domains a get took, in reverse.
 */
void
drv_i915_test_forcewake_put_all(
	struct i915_device *device,
	unsigned held)
{
	/* Puts back what was taken, last first. */
	while (held > 0U) {
		held--;
		(void)drv_i915_forcewake_put(&device->gt.mmio, i915_test_forcewake_domains[held]);
	}
}

/*
 * Names a test outcome for the log.
 */
const char *
drv_i915_test_eu_outcome_name(
	int outcome)
{
	/* Names the outcome. */
	switch (outcome) {
	case I915_TEST_EU_PASS:
		return "PASS";
	case I915_TEST_EU_HANG:
		return "HANG";
	default:
		break;
	}

	/* Anything else is an error. */
	return "ERROR";
}

/*
 * Runs the compute test once on the render engine.
 *
 * Creates the shared page and the batch, maps them at the fixture addresses of
 * the GT address space, writes the kernel, the descriptor and unwritten
 * markers, builds and verifies the batch, creates a new context and its
 * timeline, submits one request, waits for it and reads the page back.  Returns
 * 0 on a pass, a positive errno otherwise; t says what happened.
 */
int
drv_i915_test_eu_run(
	struct i915_test_eu *t,
	struct i915_gt_engines *es,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	const struct i915_sseu *sseu,
	struct i915_mmio *m,
	struct spinlock *uncore_lock,
	unsigned timeout_ms)
{
	struct i915_gt_engine *ge;
	struct i915_execlists *el;
	unsigned bit;
	int error;

	/* Refuses a missing argument. */
	if (t == NULL || es == NULL || vm == NULL || gm == NULL)
		return EINVAL;
	if (sseu == NULL || m == NULL)
		return EINVAL;

	/* Finds the render engine. */
	kern_memset(t, 0, sizeof(*t));
	error = i915_eu_find_render(t, es);
	if (error != 0)
		return error;

	ge = &es->ge[t->engine_idx];
	el = &es->el[t->engine_idx];

	/* Sets the VFE thread limit: 112 threads per enabled dual-subslice, less one, as blorp does. */
	for (bit = 0U; bit < 16U; bit++) {
		if (((sseu->subslice_mask >> bit) & 1U) != 0U)
			t->dss_count++;
	}

	if (t->dss_count == 0U) {
		error = drv_i915_test_eu_fail(t, EINVAL, "no subslice");
		return error;
	}

	t->max_threads = 112U * t->dss_count - 1U;

	/* Creates the two objects and maps them at the fixture addresses. */
	error = i915_eu_map_fixture(t, vm, gm);
	if (error != 0)
		return error;

	/* Writes the kernel, the descriptor and the unwritten markers. */
	i915_eu_write_shared(t);

	/* Builds the batch into the object that is submitted. */
	t->batch_dwords = drv_i915_test_eu_build_batch((uint32_t *)t->batch->cpu,
	    1024U,
	    I915_TEST_EU_SHARED_VA,
	    I915_TEST_EU_SHARED_VA,
	    t->max_threads);
	if (t->batch_dwords == 0U) {
		error = drv_i915_test_eu_fail(t, ENOSPC, "build_batch");
		return error;
	}

	/* Reads the selects back from the submitted object; a bad select is never submitted. */
	t->pipesel_rc = drv_i915_test_eu_check_pipeline_select((const uint32_t *)t->batch->cpu, t->batch_dwords, &t->pipesel);
	if (t->pipesel_rc != 0) {
		error = drv_i915_test_eu_fail(t, t->pipesel_rc, "pipeline_select_verify");
		return error;
	}

	/* Hashes the fixture as submitted: the batch, and the descriptor up to the end of the kernel. */
	t->batch_hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)t->batch_dwords * 4U, I915_TEST_FNV_BASIS);
	t->fixture_hash = drv_i915_test_fnv1a64((const char *)t->shared->cpu + I915_TEST_EU_IDD_OFFSET,
	    (I915_TEST_EU_KSP_OFFSET - I915_TEST_EU_IDD_OFFSET) + sizeof(drv_i915_test_eu_kernel),
	    I915_TEST_FNV_BASIS);

	/* Creates a context of the GT address space; it inherits the engine's default state. */
	error = i915_eu_new_context(t, &t->ce, &t->tl_page, ge, vm, gm, 0U);
	if (error != 0)
		return error;

	/* Builds the request; the initial breadcrumb advances the seqno by two. */
	t->tl_seqno = 2U;
	error = drv_i915_test_eu_build_request(t, &t->rq, &t->ce, t->tl_page, t->tl_seqno, I915_TEST_EU_BATCH_VA);
	if (error != 0)
		return error;

	/* Records what the GPU will walk for the fixture, read from the tables PDP0 names. */
	i915_eu_walk_fixture(t, vm);

	/* Submits the request. */
	error = drv_i915_execlists_submit(ge, el, m, &t->rq);
	if (error != 0) {
		error = drv_i915_test_eu_fail(t, error, "execlists_submit");
		return error;
	}

	t->submitted = 1;

	/* Waits for the request (i915_request_wait). */
	error = drv_i915_test_eu_wait_retired(t, ge, el, &t->rq, m, timeout_ms);
	if (error == 0) {
		t->completed = 1;
	} else if (error == ETIMEDOUT) {
		t->timed_out = 1;
	} else {
		(void)drv_i915_test_eu_fail(t, error, "i915_request_wait");
	}

	/* Reads the page back either way; a hang may still have left the CS markers. */
	i915_eu_read_back(t);

	/* A completed request passes when the EU store, the done tag and the CS marker all landed. */
	if (t->completed) {
		drv_i915_test_eu_log_record(t, ge, el, &t->rq, &t->ce, "completed");
		t->parked = drv_i915_test_eu_park(t, es, ge, el, m, timeout_ms);
		t->outcome = I915_TEST_EU_ERROR;
		if (t->eu == I915_TEST_EU_STORE_TAG &&
		    t->cs == I915_TEST_EU_CS_TAG &&
		    t->done == I915_TEST_EU_DONE_TAG)
			t->outcome = I915_TEST_EU_PASS;
		if (t->outcome == I915_TEST_EU_ERROR && t->err == 0)
			(void)drv_i915_test_eu_fail(t, EIO, "markers");

		/* Reports the first error of a completed request. */
		if (t->err != 0)
			return t->err;

		/* Succeeded: the thread ran and every marker landed. */
		return 0;
	}

	/* Records a hang or an error, dumps, and resets the engines as intel_gt_set_wedged would. */
	if (t->timed_out)
		t->outcome = I915_TEST_EU_HANG;
	drv_i915_test_eu_log_record(t, ge, el, &t->rq, &t->ce, "hang");
	drv_i915_test_eu_hang_dump_reset(t, es, ge, el, m, uncore_lock);

	/* Reports the first error, or the timeout. */
	if (t->err != 0)
		return t->err;

	/* The request never retired. */
	return ETIMEDOUT;
}

/*
 * Runs the compute batch again after a pass.
 *
 * Submits same_ctx more requests on the first context (the next requests of
 * its timeline), then new_ctx requests on a newly created context.  Stops at
 * the first round that does not pass; nothing is submitted after a hang.
 * Returns 0 when every round passed.
 */
int
drv_i915_test_eu_repeat(
	struct i915_test_eu *t,
	struct i915_gt_engines *es,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	struct i915_mmio *m,
	struct spinlock *uncore_lock,
	unsigned timeout_ms,
	unsigned same_ctx,
	unsigned new_ctx)
{
	struct i915_test_eu_round *round;
	struct i915_gt_context *ce;
	struct i915_gt_object *tl;
	struct i915_gt_engine *ge;
	struct i915_execlists *el;
	uint64_t hash;
	unsigned polls_before;
	unsigned total;
	unsigned r;
	int error;

	/* Refuses a missing argument, or a first run that did not pass cleanly. */
	if (t == NULL || es == NULL || vm == NULL || gm == NULL || m == NULL)
		return EINVAL;
	if (t->outcome != I915_TEST_EU_PASS || t->wedged || !t->parked)
		return EINVAL;

	/* Refuses more rounds than the record holds. */
	total = same_ctx + new_ctx;
	if (total > I915_TEST_EU_ROUNDS_MAX)
		return EINVAL;

	ge = &es->ge[t->engine_idx];
	el = &es->el[t->engine_idx];

	/* Refuses a batch object that no longer holds the verified words. */
	error = drv_i915_test_eu_check_pipeline_select((const uint32_t *)t->batch->cpu, t->batch_dwords, NULL);
	hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)t->batch_dwords * 4U, I915_TEST_FNV_BASIS);
	if (error != 0 || hash != t->batch_hash) {
		error = drv_i915_test_eu_fail(t, EINVAL, "repeat: batch changed");
		return error;
	}

	/* Submits one request per round and checks what it left. */
	for (r = 0U; r < total; r++) {
		/* Starts the round's record and picks its context. */
		round = &t->round[r];
		polls_before = t->polls;
		kern_memset(round, 0, sizeof(*round));
		t->n_rounds = r + 1U;
		error = i915_eu_round_context(t, round, ge, vm, gm, same_ctx, r, &ce, &tl);
		if (error != 0)
			return error;

		/* Resets the markers and builds the request. */
		i915_eu_reset_markers(t);
		error = drv_i915_test_eu_build_request(t, &t->rrq, ce, tl, round->seqno, I915_TEST_EU_BATCH_VA);
		if (error != 0) {
			round->rc = error;
			return error;
		}

		/* Submits the request. */
		error = drv_i915_execlists_submit(ge, el, m, &t->rrq);
		if (error != 0) {
			round->rc = error;
			error = drv_i915_test_eu_fail(t, error, "repeat: execlists_submit");
			return error;
		}

		/* Waits for it and reads back what it left. */
		error = drv_i915_test_eu_wait_retired(t, ge, el, &t->rrq, m, timeout_ms);
		round->rc = error;
		round->completed = 0;
		if (error == 0)
			round->completed = 1;
		round->polls = t->polls - polls_before;
		i915_eu_read_round(t, round, ce, el);

		/* A round that did not retire is recorded, dumped and reset, and ends the rounds. */
		if (!round->completed) {
			if (error == ETIMEDOUT) {
				t->timed_out = 1;
				t->outcome = I915_TEST_EU_HANG;
			} else {
				(void)drv_i915_test_eu_fail(t, error, "repeat: i915_request_wait");
			}
			drv_i915_test_eu_log_record(t, ge, el, &t->rrq, ce, "repeat-hang");
			drv_i915_test_eu_hang_dump_reset(t, es, ge, el, m, uncore_lock);
			return error;
		}

		/* Parks the engine; the round passes when every marker, both readbacks and the seqno are right. */
		round->parked = drv_i915_test_eu_park(t, es, ge, el, m, timeout_ms);
		round->pass = 0;
		if (round->parked &&
		    round->ready == I915_TEST_EU_READY_TAG &&
		    round->eu == I915_TEST_EU_STORE_TAG &&
		    round->done == I915_TEST_EU_DONE_TAG &&
		    round->cs == I915_TEST_EU_CS_TAG &&
		    round->idd_rb_ok &&
		    round->kernel_rb_ok &&
		    round->hwsp_observed == round->seqno)
			round->pass = 1;
		if (!round->pass) {
			error = drv_i915_test_eu_fail(t, EIO, "repeat: markers");
			return error;
		}

		t->rounds_passed++;
	}

	/* Succeeded: every round passed. */
	return 0;
}

/*
 * Gives back what the compute test created.
 *
 * The fixture pages go back to scratch first, then the timelines, the
 * contexts and the objects are freed.  The page tables of the fixture range
 * stay with the address space.  The caller has shown the GPU done with them.
 */
void
drv_i915_test_eu_release(
	struct i915_test_eu *t,
	struct i915_gt_mem *gm)
{
	/* Refuses a missing argument. */
	if (t == NULL || gm == NULL)
		return;

	/* Puts the fixture pages back to scratch before any page they named is freed. */
	drv_i915_test_eu_scrub_ptes(t, t->ce.vm, I915_TEST_EU_SHARED_VA, I915_TEST_EU_FIXTURE_PAGES);

	/* Frees the first context and its timeline. */
	if (t->tl_page != NULL) {
		drv_i915_gt_object_destroy(gm, t->tl_page);
		t->tl_page = NULL;
	}

	if (t->ce.allocated)
		drv_i915_lrc_release(&t->ce, gm);

	/* Frees the second context and its timeline. */
	if (t->tl_page2 != NULL) {
		drv_i915_gt_object_destroy(gm, t->tl_page2);
		t->tl_page2 = NULL;
	}

	if (t->ce2.allocated)
		drv_i915_lrc_release(&t->ce2, gm);

	/* Frees the batch and the shared page. */
	if (t->batch != NULL) {
		drv_i915_gt_object_destroy(gm, t->batch);
		t->batch = NULL;
	}

	if (t->shared != NULL) {
		drv_i915_gt_object_destroy(gm, t->shared);
		t->shared = NULL;
	}
}

/*
 * Reads the engine workaround registers once per dual-subslice.
 *
 * The command-streamer verify reads multicast registers through the default
 * steering only; this reads GEN8_ROW_CHICKEN2, GEN10_SAMPLER_MODE and
 * GEN9_ROW_CHICKEN4 steered at every enabled dual-subslice of slice 0 and
 * compares them with the workaround list the way wa_verify does.  The caller
 * holds forcewake; the multicast lock is the exclusive section.
 */
int
drv_i915_test_mcr_probe_wa(
	struct i915_test_mcr_probe *probe,
	struct i915_mmio *m,
	const struct i915_wa_list *wal,
	const struct i915_sseu *sseu)
{
	struct i915_test_mcr_entry *entry;
	uint32_t reg;
	uint32_t set;
	uint32_t mask;
	unsigned r;
	unsigned ss;
	unsigned k;
	int listed;
	int error;

	/* Refuses a missing argument. */
	if (probe == NULL || m == NULL || wal == NULL || sseu == NULL)
		return EINVAL;

	/* Takes the multicast lock; the probe records whether it could. */
	kern_memset(probe, 0, sizeof(*probe));
	error = drv_i915_mcr_lock(m, 0U);
	probe->lock_rc = error;
	if (error != 0)
		return error;

	/* Reads each register. */
	for (r = 0U; r < I915_TEST_MCR_REGS; r++) {
		/* Merges what the workaround list expects of the register. */
		reg = i915_test_mcr_regs[r];
		set = 0U;
		mask = 0U;
		listed = 0;
		for (k = 0U; k < wal->count; k++) {
			if (wal->list[k].reg != reg)
				continue;

			listed = 1;
			set |= wal->list[k].set;
			mask |= wal->list[k].read_mask;
		}

		/* Reads it at every enabled dual-subslice of the first slice. */
		for (ss = 0U; ss < 8U; ss++) {
			if (((sseu->subslice_mask >> ss) & 1U) == 0U)
				continue;
			if (probe->n == I915_TEST_MCR_MAX)
				break;

			entry = &probe->e[probe->n];
			probe->n++;
			entry->reg = reg;
			entry->group = 0U;
			entry->instance = ss;
			entry->listed = listed;
			entry->expected_set = set;
			entry->read_mask = mask;
			entry->raw = i915_eu_mcr_read_steered(m, reg, 0U, ss, &entry->selector_before, &entry->selector_after);

			/* Compares as wa_verify does: (current ^ set) & read mask. */
			entry->masked_mismatch = 0U;
			if (listed)
				entry->masked_mismatch = (entry->raw ^ set) & mask;
			if (entry->masked_mismatch != 0U)
				probe->mismatches++;
		}
	}

	drv_i915_mcr_unlock(m);

	/* Succeeded: every enabled dual-subslice was read. */
	return 0;
}

/*
 * Runs the compute scenario.
 *
 * Takes forcewake, reads the multicast workaround registers, runs the
 * compute test once and, after a pass, three more requests on the same
 * context and two on a new one.  Logs everything the earlier hardware runs
 * logged, then cleans up so the node is served normally.  Returns 0 when the
 * test and every round passed.
 */
int
drv_i915_test_execution_eu(
	struct i915_device *device)
{
	struct i915_test_eu *t;
	struct i915_gt *gt;
	unsigned user0;
	unsigned ctx0;
	unsigned error0;
	unsigned held;
	int repeat_error;
	int error;

	gt = &device->gt;
	t = &i915_test_eu_state;

	/* Holds the forcewake domains for the whole test. */
	error = drv_i915_test_forcewake_get_all(device, &held);
	if (error != 0) {
		kern_logf("i915: EU-TEST not run: forcewake failed rc=%d\n", error);
		drv_i915_test_forcewake_put_all(device, held);
		return error;
	}

	/* Samples the GT interrupt counters, so the verdict line says what this test raised. */
	user0 = gt->irq.gt_user_intr;
	ctx0 = gt->irq.gt_ctx_switch_intr;
	error0 = gt->irq.gt_error_intr;

	/* Reads the multicast workaround registers after the start and before any submission. */
	(void)drv_i915_test_mcr_probe_wa(&i915_test_mcr, &gt->mmio, &gt->init.engine_wa[0], &gt->info.sseu);
	i915_eu_log_mcr(&i915_test_mcr, &gt->info.sseu);

	/* Runs the compute test and logs it. */
	error = drv_i915_test_eu_run(t,
	    &gt->engines,
	    &gt->ppgtt,
	    &gt->mem,
	    &gt->info.sseu,
	    &gt->mmio,
	    &gt->uncore_lock,
	    I915_TEST_TIMEOUT_MS);
	i915_eu_log_run(device, t, error, user0, ctx0, error0);

	/* After a pass, runs the same batch three more times on the context, then twice on a new one. */
	repeat_error = EINVAL;
	if (t->outcome == I915_TEST_EU_PASS) {
		repeat_error = drv_i915_test_eu_repeat(t,
		    &gt->engines,
		    &gt->ppgtt,
		    &gt->mem,
		    &gt->mmio,
		    &gt->uncore_lock,
		    I915_TEST_TIMEOUT_MS,
		    I915_TEST_EU_SAME_CONTEXT,
		    I915_TEST_EU_NEW_CONTEXT);
		i915_eu_log_repeat(device, t, repeat_error, user0, ctx0, error0);
	}

	/* Gives back what the test created, unless the GPU may still use it. */
	i915_eu_cleanup(device, t, "EU-TEST");
	drv_i915_test_forcewake_put_all(device, held);

	/* Reports a failed first run. */
	if (error != 0)
		return error;

	/* Reports a failed follow-up round. */
	if (repeat_error != 0)
		return repeat_error;
	if (t->rounds_passed != I915_TEST_EU_SAME_CONTEXT + I915_TEST_EU_NEW_CONTEXT)
		return EIO;

	/* Succeeded: the first run and every round passed. */
	return 0;
}

/* Appends one dword, or notes that it did not fit. */
static void
i915_eu_emit(
	struct i915_test_batch *batch,
	uint32_t dword)
{
	/* Stores the dword when there is room. */
	if (batch->count < batch->capacity) {
		batch->cmds[batch->count] = dword;
	} else {
		batch->overflow = 1;
	}

	batch->count++;
}

/* Emits a six-dword PIPE_CONTROL; a render-target flush also asks for the HDC pipeline flush. */
static void
i915_eu_emit_pipe_control(
	struct i915_test_batch *batch,
	uint32_t flags)
{
	uint32_t header;
	unsigned i;

	/* Adds the HDC pipeline flush to a render-target flush, as the draw emitter does. */
	header = GFX_OP_PIPE_CONTROL(6);
	if ((flags & PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH) != 0U)
		header |= PIPE_CONTROL0_HDC_PIPELINE_FLUSH;

	/* Emits the header, the flags and four zero dwords. */
	i915_eu_emit(batch, header);
	i915_eu_emit(batch, flags);
	for (i = 0U; i < 4U; i++)
		i915_eu_emit(batch, 0U);
}

/* Emits a store of a marker value to an address of the context's address space. */
static void
i915_eu_emit_marker(
	struct i915_test_batch *batch,
	uint64_t va,
	uint32_t value)
{
	/* MI_STORE_DWORD_IMM with the address split in two dwords. */
	i915_eu_emit(batch, MI_STORE_DWORD_IMM_GEN4);
	i915_eu_emit(batch, (uint32_t)va);
	i915_eu_emit(batch, (uint32_t)(va >> 32));
	i915_eu_emit(batch, value);
}

/* Emits a copy of one dword between two addresses of the context's address space. */
static void
i915_eu_emit_copy(
	struct i915_test_batch *batch,
	uint64_t destination,
	uint64_t source)
{
	/* MI_COPY_MEM_MEM, destination first. */
	i915_eu_emit(batch, I915_TEST_MI_COPY_MEM_MEM);
	i915_eu_emit(batch, (uint32_t)destination);
	i915_eu_emit(batch, (uint32_t)(destination >> 32));
	i915_eu_emit(batch, (uint32_t)source);
	i915_eu_emit(batch, (uint32_t)(source >> 32));
}

/* Emits STATE_BASE_ADDRESS: surface, dynamic and bindless state in the shared page, instructions at inst_base. */
static void
i915_eu_emit_state_base_address(
	struct i915_test_batch *batch,
	uint64_t shared,
	uint64_t inst_base,
	uint32_t mocs)
{
	uint32_t shared_low;
	uint32_t writeback;

	shared_low = (uint32_t)shared & 0xfffff000U;
	writeback = I915_TEST_GEN12_MOCS(I915_TEST_MOCS_WRITEBACK);

	/* The header, and the general state base at 0 with the stateless data-port MOCS. */
	i915_eu_emit(batch, I915_TEST_STATE_BASE_ADDRESS);
	i915_eu_emit(batch, 1U | (mocs << 4));
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, mocs << 16);

	/* The surface and the dynamic state bases at the shared page. */
	i915_eu_emit(batch, 1U | (mocs << 4) | shared_low);
	i915_eu_emit(batch, (uint32_t)(shared >> 32));
	i915_eu_emit(batch, 1U | (mocs << 4) | shared_low);
	i915_eu_emit(batch, (uint32_t)(shared >> 32));

	/* The indirect object base at 0, and the instruction base, cached write-back. */
	i915_eu_emit(batch, 1U | (mocs << 4));
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 1U | (writeback << 4) | ((uint32_t)inst_base & 0xfffff000U));
	i915_eu_emit(batch, (uint32_t)(inst_base >> 32));

	/* The general, dynamic, indirect and instruction sizes, all at the maximum. */
	i915_eu_emit(batch, 1U | (0xfffffU << 12));
	i915_eu_emit(batch, 1U | (0xfffffU << 12));
	i915_eu_emit(batch, 1U | (0xfffffU << 12));
	i915_eu_emit(batch, 1U | (0xfffffU << 12));

	/* The bindless surface base at the shared page, one page long, and the bindless sampler base at 0. */
	i915_eu_emit(batch, 1U | (mocs << 4) | shared_low);
	i915_eu_emit(batch, (uint32_t)(shared >> 32));
	i915_eu_emit(batch, (4096U / 64U - 1U) << 12);
	i915_eu_emit(batch, 1U | (mocs << 4));
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
}

/* Emits the VFE state, the descriptor load, the ready marker and the one-thread walker. */
static void
i915_eu_emit_vfe_and_walker(
	struct i915_test_batch *batch,
	uint32_t max_threads)
{
	/* MEDIA_VFE_STATE (9 dwords): the thread limit and the URB layout. */
	i915_eu_emit(batch, 0x70000007U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, (max_threads << 16) | (2U << 8));
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 2U << 16);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);

	/* MEDIA_STATE_FLUSH before the load clears the temporary descriptor storage. */
	i915_eu_emit(batch, 0x70040000U);
	i915_eu_emit(batch, 0U);

	/* MEDIA_INTERFACE_DESCRIPTOR_LOAD: one 32-byte descriptor at its dynamic offset. */
	i915_eu_emit(batch, 0x70020002U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 32U);
	i915_eu_emit(batch, I915_TEST_EU_IDD_OFFSET);

	/* Marks that the command streamer reached the dispatch. */
	i915_eu_emit_marker(batch, I915_TEST_EU_SHARED_VA + I915_TEST_EU_READY_OFF, I915_TEST_EU_READY_TAG);

	/* GPGPU_WALKER: descriptor 0, no indirect data, SIMD8, one group of one thread at the origin. */
	i915_eu_emit(batch, 0x7105000dU);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 1U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 1U);
	i915_eu_emit(batch, 0U);
	i915_eu_emit(batch, 1U);

	/* The right and the bottom execution masks: one lane. */
	i915_eu_emit(batch, 0x1U);
	i915_eu_emit(batch, 0xffffffffU);

	/* MEDIA_STATE_FLUSH after the walker. */
	i915_eu_emit(batch, 0x70040000U);
	i915_eu_emit(batch, 0U);
}

/* Encodes PIPELINE_SELECT with the mask bits set for a pipeline (0 = 3D, 2 = GPGPU). */
static uint32_t
i915_eu_pipeline_select(
	uint32_t pipeline)
{
	/* The Gen12 header 0x6904, the mask bits, the media sampler DOP clock gate enable and the pipeline. */
	return (0x6904U << 16) | (0x13U << 8) | (1U << 4) | pipeline;
}

/* Tells whether a request has landed and the engine has reported its context complete. */
static int
i915_eu_retired(
	struct i915_gt_request *rq,
	struct i915_execlists *el)
{
	int completed;

	/* The breadcrumb must have landed. */
	completed = drv_i915_request_completed(rq);
	if (!completed)
		return 0;

	/* The engine must have nothing active or pending. */
	if (el->have_active)
		return 0;
	if (el->pending[0] != NULL)
		return 0;

	/* The request is retired. */
	return 1;
}

/* Finds the render engine and records its index. */
static int
i915_eu_find_render(
	struct i915_test_eu *t,
	struct i915_gt_engines *es)
{
	unsigned i;
	int error;

	/* Looks for the first engine of the render class. */
	for (i = 0U; i < es->n; i++) {
		if (es->ge[i].info->class == I915_RENDER_CLASS) {
			t->engine_idx = i;
			return 0;
		}
	}

	/* The GT has no render engine. */
	error = drv_i915_test_eu_fail(t, ENODEV, "no render engine");
	return error;
}

/* Creates a context of the GT address space and its timeline page, ready for a first request. */
static int
i915_eu_new_context(
	struct i915_test_eu *t,
	struct i915_gt_context *ce,
	struct i915_gt_object **tl_page,
	struct i915_gt_engine *ge,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	unsigned repeat)
{
	static const char *const steps[2][3] = {
		{ "intel_context_create", "intel_timeline_create", "intel_timeline_pin" },
		{ "repeat: intel_context_create", "repeat: intel_timeline_create", "repeat: intel_timeline_pin" }
	};
	int error;

	/* Creates the context (intel_context_create); it inherits the engine's default state. */
	error = drv_i915_lrc_alloc(ce, ge, vm, gm, 4096U, 0U);
	if (error != 0) {
		error = drv_i915_test_eu_fail(t, error, steps[repeat][0]);
		return error;
	}

	/* Creates the timeline page (intel_timeline_create). */
	*tl_page = drv_i915_gt_object_create(gm, 4096U);
	if (*tl_page == NULL) {
		error = drv_i915_test_eu_fail(t, ENOMEM, steps[repeat][1]);
		return error;
	}

	/* Binds it into the GGTT, where the breadcrumbs are written (intel_timeline_pin). */
	error = drv_i915_gt_ggtt_bind(gm, *tl_page);
	if (error != 0) {
		error = drv_i915_test_eu_fail(t, error, steps[repeat][2]);
		return error;
	}

	/* Writes the context image and its ring registers. */
	drv_i915_lrc_init_state(ce);
	(void)drv_i915_lrc_update_regs(ce, ce->ring.tail);

	/* Succeeded: the context takes its first request. */
	return 0;
}

/* Creates the shared page and the batch and maps them at the fixture addresses. */
static int
i915_eu_map_fixture(
	struct i915_test_eu *t,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm)
{
	uint64_t dma;
	int error;

	/* Creates the two objects. */
	t->shared = drv_i915_gt_object_create(gm, 4096U);
	if (t->shared == NULL) {
		error = drv_i915_test_eu_fail(t, ENOMEM, "gem_create");
		return error;
	}

	t->batch = drv_i915_gt_object_create(gm, 4096U);
	if (t->batch == NULL) {
		error = drv_i915_test_eu_fail(t, ENOMEM, "gem_create");
		return error;
	}

	/* Allocates the page tables of the two pages. */
	error = drv_i915_gt_ppgtt_alloc_range(gm, vm, I915_TEST_EU_SHARED_VA, 2U * 4096U);
	if (error != 0) {
		error = drv_i915_test_eu_fail(t, error, "allocate_va_range");
		return error;
	}

	/* Maps the shared page, cached (I915_CACHE_LLC is PAT 0). */
	error = drv_i915_gt_object_page_dma(t->shared, 0U, &dma);
	if (error == 0)
		error = drv_i915_gt_ppgtt_insert_page(vm, dma, I915_TEST_EU_SHARED_VA, 0U);

	/* Maps the batch the same way. */
	if (error == 0)
		error = drv_i915_gt_object_page_dma(t->batch, 0U, &dma);
	if (error == 0)
		error = drv_i915_gt_ppgtt_insert_page(vm, dma, I915_TEST_EU_BATCH_VA, 0U);
	if (error != 0) {
		error = drv_i915_test_eu_fail(t, error, "ppgtt_insert");
		return error;
	}

	/* Succeeded: both pages are mapped. */
	return 0;
}

/* Writes the kernel, the descriptor and unwritten markers into the shared page. */
static void
i915_eu_write_shared(
	struct i915_test_eu *t)
{
	volatile uint32_t *page;
	volatile uint32_t *idd;

	page = (volatile uint32_t *)t->shared->cpu;
	idd = page + I915_TEST_EU_IDD_OFFSET / 4U;

	/* The kernel at its start pointer. */
	kern_memcpy((char *)t->shared->cpu + I915_TEST_EU_KSP_OFFSET, drv_i915_test_eu_kernel, sizeof(drv_i915_test_eu_kernel));

	/* The descriptor: the kernel start pointer, the sampler count field and one thread per group. */
	idd[0] = I915_TEST_EU_KSP_OFFSET;
	idd[1] = 0U;
	idd[2] = 1U << 20;
	idd[3] = 0U;
	idd[4] = 0U;
	idd[5] = 0U;
	idd[6] = 1U;
	idd[7] = 0U;

	/* The markers, until the GPU writes them. */
	page[I915_TEST_EU_READY_OFF / 4U] = I915_TEST_EU_UNWRITTEN;
	page[I915_TEST_EU_EU_OFF / 4U] = I915_TEST_EU_UNWRITTEN;
	page[I915_TEST_EU_DONE_OFF / 4U] = I915_TEST_EU_UNWRITTEN;
	page[I915_TEST_EU_CS_OFF / 4U] = I915_TEST_EU_UNWRITTEN;
}

/* Records what the GPU will walk for the batch, the descriptor, the kernel, the EU target and the done marker. */
static void
i915_eu_walk_fixture(
	struct i915_test_eu *t,
	struct i915_gt_ppgtt *vm)
{
	static const uint32_t offsets[5] = {
		0U,
		I915_TEST_EU_IDD_OFFSET,
		I915_TEST_EU_KSP_OFFSET,
		I915_TEST_EU_EU_OFF,
		I915_TEST_EU_DONE_OFF
	};
	uint64_t pdp0;
	uint64_t va;
	unsigned i;

	/* Compares PDP0 of the context with the top directory of the space. */
	pdp0 = ((uint64_t)t->ce.lrc_reg_state[CTX_PDP0_UDW] << 32) | t->ce.lrc_reg_state[CTX_PDP0_LDW];
	t->pdp0_matches_top = 0;
	if (pdp0 == vm->top_pd_dma)
		t->pdp0_matches_top = 1;

	/* Walks the batch address, then the four addresses of the shared page. */
	for (i = 0U; i < 5U; i++) {
		va = I915_TEST_EU_SHARED_VA + offsets[i];
		if (i == 0U)
			va = I915_TEST_EU_BATCH_VA;
		(void)drv_i915_test_ppgtt_walk(vm, va, &t->walk[i]);
	}

	t->walks = 5U;
}

/* Reads the markers and both readbacks from the shared page. */
static void
i915_eu_read_back(
	struct i915_test_eu *t)
{
	volatile uint32_t *page;
	unsigned i;

	page = (volatile uint32_t *)t->shared->cpu;

	/* The four markers. */
	t->ready = page[I915_TEST_EU_READY_OFF / 4U];
	t->eu = page[I915_TEST_EU_EU_OFF / 4U];
	t->done = page[I915_TEST_EU_DONE_OFF / 4U];
	t->cs = page[I915_TEST_EU_CS_OFF / 4U];

	/* The descriptor as the GPU read it, compared with what the CPU wrote. */
	t->idd_rb_ok = 1;
	for (i = 0U; i < 8U; i++) {
		t->idd_rb[i] = page[I915_TEST_EU_IDD_RB_OFF / 4U + i];
		if (t->idd_rb[i] != page[I915_TEST_EU_IDD_OFFSET / 4U + i])
			t->idd_rb_ok = 0;
	}

	/* The kernel as the GPU read it, compared with the table. */
	t->kernel_rb_ok = 1;
	for (i = 0U; i < I915_TEST_EU_KERNEL_DWORDS; i++) {
		t->kernel_rb[i] = page[I915_TEST_EU_KERNEL_RB_OFF / 4U + i];
		if (t->kernel_rb[i] != drv_i915_test_eu_kernel[i])
			t->kernel_rb_ok = 0;
	}
}

/* Puts the markers back to unwritten and clears both readback areas. */
static void
i915_eu_reset_markers(
	struct i915_test_eu *t)
{
	volatile uint32_t *page;
	unsigned i;

	page = (volatile uint32_t *)t->shared->cpu;

	/* The markers. */
	page[I915_TEST_EU_READY_OFF / 4U] = I915_TEST_EU_UNWRITTEN;
	page[I915_TEST_EU_EU_OFF / 4U] = I915_TEST_EU_UNWRITTEN;
	page[I915_TEST_EU_DONE_OFF / 4U] = I915_TEST_EU_UNWRITTEN;
	page[I915_TEST_EU_CS_OFF / 4U] = I915_TEST_EU_UNWRITTEN;

	/* The descriptor readback. */
	for (i = 0U; i < 8U; i++)
		page[I915_TEST_EU_IDD_RB_OFF / 4U + i] = 0U;

	/* The kernel readback. */
	for (i = 0U; i < I915_TEST_EU_KERNEL_DWORDS; i++)
		page[I915_TEST_EU_KERNEL_RB_OFF / 4U + i] = 0U;
}

/* Picks the context of a round, creating the second context on its first round, and advances its seqno. */
static int
i915_eu_round_context(
	struct i915_test_eu *t,
	struct i915_test_eu_round *round,
	struct i915_gt_engine *ge,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	unsigned same_ctx,
	unsigned index,
	struct i915_gt_context **ce,
	struct i915_gt_object **tl)
{
	int error;

	/* The first rounds use the first context: the next requests of its timeline. */
	if (index < same_ctx) {
		round->ctx = 'A';
		*ce = &t->ce;
		*tl = t->tl_page;
		t->tl_seqno += 2U;
		round->seqno = t->tl_seqno;
		return 0;
	}

	/* The rest use a new context with a new timeline, created on the first of them. */
	round->ctx = 'B';
	if (!t->ce2.allocated) {
		error = i915_eu_new_context(t, &t->ce2, &t->tl_page2, ge, vm, gm, 1U);
		if (error != 0) {
			round->rc = error;
			return error;
		}

		t->tl_seqno2 = 0U;
	}

	*ce = &t->ce2;
	*tl = t->tl_page2;
	t->tl_seqno2 += 2U;
	round->seqno = t->tl_seqno2;

	/* Succeeded: the round has its context and seqno. */
	return 0;
}

/* Records what a round's request left: status page, markers, readbacks, ring span and status event. */
static void
i915_eu_read_round(
	struct i915_test_eu *t,
	struct i915_test_eu_round *round,
	struct i915_gt_context *ce,
	struct i915_execlists *el)
{
	volatile uint32_t *page;
	unsigned i;

	page = (volatile uint32_t *)t->shared->cpu;

	/* The status page and the four markers. */
	round->hwsp_observed = *t->rrq.hwsp_cpu;
	round->ready = page[I915_TEST_EU_READY_OFF / 4U];
	round->eu = page[I915_TEST_EU_EU_OFF / 4U];
	round->done = page[I915_TEST_EU_DONE_OFF / 4U];
	round->cs = page[I915_TEST_EU_CS_OFF / 4U];

	/* The descriptor readback. */
	round->idd_rb_ok = 1;
	for (i = 0U; i < 8U; i++) {
		if (page[I915_TEST_EU_IDD_RB_OFF / 4U + i] != page[I915_TEST_EU_IDD_OFFSET / 4U + i])
			round->idd_rb_ok = 0;
	}

	/* The kernel readback. */
	round->kernel_rb_ok = 1;
	for (i = 0U; i < I915_TEST_EU_KERNEL_DWORDS; i++) {
		if (page[I915_TEST_EU_KERNEL_RB_OFF / 4U + i] != drv_i915_test_eu_kernel[i])
			round->kernel_rb_ok = 0;
	}

	/* The context, the ring span and the last status event. */
	round->lrca = ce->lrca;
	round->ring_head = t->rrq.head;
	round->ring_tail = t->rrq.tail;
	round->csb_hi = el->last_csb_hi;
	round->csb_lo = el->last_csb_lo;
}

/* Reads a register steered at one group and instance, then restores the selector (rw_with_mcr_steering_fw). */
static uint32_t
i915_eu_mcr_read_steered(
	struct i915_mmio *m,
	uint32_t reg,
	unsigned group,
	unsigned instance,
	uint32_t *selector_before,
	uint32_t *selector_after)
{
	uint32_t selector;
	uint32_t steered;
	uint32_t value;

	/* Steers at the group and instance, keeping the multicast bit (Wa_22013088509). */
	selector = drv_i915_raw_read32(m, I915_TEST_MCR_SELECTOR);
	steered = selector & ~(I915_TEST_MCR_SLICE_MASK | I915_TEST_MCR_SUBSLICE_MASK);
	steered |= (((uint32_t)group) & 0xfU) << I915_TEST_MCR_SLICE_SHIFT;
	steered |= (((uint32_t)instance) & 0x7U) << I915_TEST_MCR_SUBSLICE_SHIFT;
	drv_i915_raw_write32(m, I915_TEST_MCR_SELECTOR, steered);

	/* Reads the register, then puts the selector back and reads what it holds now. */
	value = drv_i915_raw_read32(m, reg);
	drv_i915_raw_write32(m, I915_TEST_MCR_SELECTOR, selector);
	*selector_before = selector;
	*selector_after = drv_i915_raw_read32(m, I915_TEST_MCR_SELECTOR);

	/* Reports the steered value. */
	return value;
}

/* Logs every steered read and the summary. */
static void
i915_eu_log_mcr(
	const struct i915_test_mcr_probe *probe,
	const struct i915_sseu *sseu)
{
	const struct i915_test_mcr_entry *entry;
	unsigned i;

	/* One line per read. */
	for (i = 0U; i < probe->n; i++) {
		entry = &probe->e[i];
		kern_logf("i915: MCR-PROBE phase=pre-eu reg=0x%05x group=%u instance=%u "
		    "raw=0x%08x expected_set=0x%08x read_mask=0x%08x masked_mismatch=0x%08x "
		    "selector_before=0x%08x selector_after=0x%08x listed=%d\n",
		    entry->reg,
		    entry->group,
		    entry->instance,
		    entry->raw,
		    entry->expected_set,
		    entry->read_mask,
		    entry->masked_mismatch,
		    entry->selector_before,
		    entry->selector_after,
		    entry->listed);
	}

	kern_logf("i915: MCR-PROBE summary: entries=%u mismatches=%u lock_rc=%d subslice_mask=0x%x\n",
	    probe->n,
	    probe->mismatches,
	    probe->lock_rc,
	    sseu->subslice_mask);
}

/* Logs the compute test: fixture identity, walks, selects, fixture words, batch, verdict and context. */
static void
i915_eu_log_run(
	struct i915_device *device,
	const struct i915_test_eu *t,
	int rc,
	unsigned user0,
	unsigned ctx0,
	unsigned error0)
{
	struct i915_gt *gt;
	const char *where;

	gt = &device->gt;
	where = "-";
	if (t->err_where != NULL)
		where = t->err_where;

	/* The fixture's identity, then what the GPU walks for it. */
	kern_logf("i915: EU-TEST fixture: batch_hash=%016llx fixture_hash=%016llx batch_dwords=%u pdp0_matches_top=%d\n",
	    (unsigned long long)t->batch_hash,
	    (unsigned long long)t->fixture_hash,
	    t->batch_dwords,
	    t->pdp0_matches_top);
	i915_eu_log_walks(t);

	/* The selects read back from the submitted object. */
	kern_logf("i915: EU-TEST pipeline_select (read from the submitted object): rc=%d "
	    "3d=%u@%u gpgpu=%u@%u bad=%u@%u bad_word=%08x\n",
	    t->pipesel_rc,
	    t->pipesel.n_3d,
	    t->pipesel.idx_3d,
	    t->pipesel.n_gpgpu,
	    t->pipesel.idx_gpgpu,
	    t->pipesel.n_bad,
	    t->pipesel.idx_bad,
	    t->pipesel.bad_word);

	/* The descriptor, the kernel and the batch as submitted. */
	i915_eu_log_fixture(t);

	/* The verdict. */
	kern_logf("i915: EU-TEST %s: rc=%d where=%s engine=%s dss=%u max_threads=%u "
	    "batch_dwords=%u submitted=%d completed=%d parked=%d timed_out=%d wedged=%d "
	    "polls=%u | ready=%08x eu=%08x done=%08x cs=%08x idd_rb_ok=%d kernel_rb_ok=%d "
	    "| rq seqno=%u krq seqno=%u | gt irq: user=%u ctx_switch=%u error=%u\n",
	    drv_i915_test_eu_outcome_name(t->outcome),
	    rc,
	    where,
	    gt->engines.ge[t->engine_idx].info->name,
	    t->dss_count,
	    t->max_threads,
	    t->batch_dwords,
	    t->submitted,
	    t->completed,
	    t->parked,
	    t->timed_out,
	    t->wedged,
	    t->polls,
	    t->ready,
	    t->eu,
	    t->done,
	    t->cs,
	    t->idd_rb_ok,
	    t->kernel_rb_ok,
	    t->rq.seqno,
	    t->krq.seqno,
	    gt->irq.gt_user_intr - user0,
	    gt->irq.gt_ctx_switch_intr - ctx0,
	    gt->irq.gt_error_intr - error0);

	/* The context's registers and both readbacks. */
	i915_eu_log_context(t);
}

/* Logs every walk of the fixture addresses. */
static void
i915_eu_log_walks(
	const struct i915_test_eu *t)
{
	const struct i915_test_ppgtt_walk *w;
	unsigned i;

	/* One line per address: every level's entry, then what the page entry maps. */
	for (i = 0U; i < t->walks; i++) {
		w = &t->walk[i];
		kern_logf("i915: EU-TEST walk va=0x%llx top=0x%llx levels=%d | "
		    "PML4[%u]=%016llx child=0x%llx known=%d scr=%d | PDP[%u]=%016llx child=0x%llx "
		    "known=%d scr=%d | PD[%u]=%016llx child=0x%llx known=%d scr=%d | "
		    "PT[%u]=%016llx leaf=0x%llx present=%d rw=%d pat=%u scr=%d\n",
		    (unsigned long long)w->va,
		    (unsigned long long)w->top_dma,
		    w->levels,
		    w->idx[0],
		    (unsigned long long)w->raw[0],
		    (unsigned long long)w->child_dma[0],
		    w->child_known[0],
		    w->scratch[0],
		    w->idx[1],
		    (unsigned long long)w->raw[1],
		    (unsigned long long)w->child_dma[1],
		    w->child_known[1],
		    w->scratch[1],
		    w->idx[2],
		    (unsigned long long)w->raw[2],
		    (unsigned long long)w->child_dma[2],
		    w->child_known[2],
		    w->scratch[2],
		    w->idx[3],
		    (unsigned long long)w->raw[3],
		    (unsigned long long)w->leaf_dma,
		    w->leaf_present,
		    w->leaf_rw,
		    w->leaf_pat,
		    w->scratch[3]);
	}
}

/* Logs the descriptor, the kernel and the batch as they sit in the submitted objects. */
static void
i915_eu_log_fixture(
	const struct i915_test_eu *t)
{
	const uint32_t *words;
	unsigned k;

	/* The descriptor and the kernel in the shared page. */
	if (t->shared != NULL) {
		words = (const uint32_t *)t->shared->cpu;
		kern_logf("i915: EU-TEST fixture-idd[@%u]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		    I915_TEST_EU_IDD_OFFSET,
		    words[224],
		    words[225],
		    words[226],
		    words[227],
		    words[228],
		    words[229],
		    words[230],
		    words[231]);
		for (k = 0U; k < I915_TEST_EU_KERNEL_DWORDS; k += 6U) {
			kern_logf("i915: EU-TEST fixture-kernel[%02u]: %08x %08x %08x %08x %08x %08x\n",
			    k,
			    words[256U + k],
			    words[257U + k],
			    words[258U + k],
			    words[259U + k],
			    words[260U + k],
			    words[261U + k]);
		}
	}

	/* The batch, eight dwords to a line. */
	if (t->batch != NULL && t->batch_dwords != 0U) {
		words = (const uint32_t *)t->batch->cpu;
		for (k = 0U; k < t->batch_dwords; k += 8U) {
			kern_logf("i915: EU-TEST batch[%03u]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			    k,
			    words[k],
			    words[k + 1U],
			    words[k + 2U],
			    words[k + 3U],
			    words[k + 4U],
			    words[k + 5U],
			    words[k + 6U],
			    words[k + 7U]);
		}
	}
}

/* Logs the context's control, ring and PDP0 registers and both readbacks. */
static void
i915_eu_log_context(
	const struct i915_test_eu *t)
{
	unsigned long long state_ggtt;
	uint32_t context_control;
	uint32_t ring_control;
	uint32_t pdp0_high;
	uint32_t pdp0_low;
	unsigned k;

	/* Samples the context image, when there is one. */
	context_control = 0U;
	ring_control = 0U;
	pdp0_high = 0U;
	pdp0_low = 0U;
	if (t->ce.lrc_reg_state != NULL) {
		context_control = t->ce.lrc_reg_state[CTX_CONTEXT_CONTROL];
		ring_control = t->ce.lrc_reg_state[CTX_RING_CTL];
		pdp0_high = t->ce.lrc_reg_state[CTX_PDP0_UDW];
		pdp0_low = t->ce.lrc_reg_state[CTX_PDP0_LDW];
	}

	state_ggtt = 0ULL;
	if (t->ce.state != NULL)
		state_ggtt = (unsigned long long)t->ce.state->ggtt_offset;

	kern_logf("i915: EU-TEST ctx: CTX_CTRL=%08x RING_CTL=%08x PDP0=%08x:%08x "
	    "ring ggtt=0x%llx state ggtt=0x%llx | idd_rb=%08x %08x %08x %08x %08x %08x %08x %08x\n",
	    context_control,
	    ring_control,
	    pdp0_high,
	    pdp0_low,
	    (unsigned long long)t->ce.ring.ggtt_offset,
	    state_ggtt,
	    t->idd_rb[0],
	    t->idd_rb[1],
	    t->idd_rb[2],
	    t->idd_rb[3],
	    t->idd_rb[4],
	    t->idd_rb[5],
	    t->idd_rb[6],
	    t->idd_rb[7]);

	/* The kernel readback, twelve dwords to a line. */
	for (k = 0U; k < I915_TEST_EU_KERNEL_DWORDS; k += 12U) {
		kern_logf("i915: EU-TEST kernel_rb[%u..]: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
		    k,
		    t->kernel_rb[k],
		    t->kernel_rb[k + 1U],
		    t->kernel_rb[k + 2U],
		    t->kernel_rb[k + 3U],
		    t->kernel_rb[k + 4U],
		    t->kernel_rb[k + 5U],
		    t->kernel_rb[k + 6U],
		    t->kernel_rb[k + 7U],
		    t->kernel_rb[k + 8U],
		    t->kernel_rb[k + 9U],
		    t->kernel_rb[k + 10U],
		    t->kernel_rb[k + 11U]);
	}
}

/* Logs every follow-up round and their verdict. */
static void
i915_eu_log_repeat(
	struct i915_device *device,
	const struct i915_test_eu *t,
	int rc,
	unsigned user0,
	unsigned ctx0,
	unsigned error0)
{
	const struct i915_test_eu_round *round;
	struct i915_gt *gt;
	const char *verdict;
	const char *where;
	unsigned i;

	gt = &device->gt;

	/* One line per round. */
	for (i = 0U; i < t->n_rounds; i++) {
		round = &t->round[i];
		kern_logf("i915: EU-REPEAT round=%u ctx=%c lrca=%08x seqno=%u hwsp_observed=%u "
		    "rc=%d completed=%d parked=%d pass=%d polls=%u | ready=%08x eu=%08x done=%08x cs=%08x "
		    "idd_rb_ok=%d kernel_rb_ok=%d | ring head=0x%x tail=0x%x last_csb=%08x:%08x\n",
		    i + 1U,
		    round->ctx,
		    round->lrca,
		    round->seqno,
		    round->hwsp_observed,
		    round->rc,
		    round->completed,
		    round->parked,
		    round->pass,
		    round->polls,
		    round->ready,
		    round->eu,
		    round->done,
		    round->cs,
		    round->idd_rb_ok,
		    round->kernel_rb_ok,
		    round->ring_head,
		    round->ring_tail,
		    round->csb_hi,
		    round->csb_lo);
	}

	/* The verdict: every round passed, a hang, or another error. */
	if (rc == 0 && t->rounds_passed == I915_TEST_EU_SAME_CONTEXT + I915_TEST_EU_NEW_CONTEXT) {
		verdict = "PASS";
	} else if (t->wedged) {
		verdict = "HANG";
	} else {
		verdict = "ERROR";
	}

	where = "-";
	if (t->err_where != NULL)
		where = t->err_where;

	kern_logf("i915: EU-REPEAT %s: rc=%d where=%s rounds=%u passed=%u (same-context 3 + new-context 2) "
	    "wedged=%d | gt irq: user=%u ctx_switch=%u error=%u\n",
	    verdict,
	    rc,
	    where,
	    t->n_rounds,
	    t->rounds_passed,
	    t->wedged,
	    gt->irq.gt_user_intr - user0,
	    gt->irq.gt_ctx_switch_intr - ctx0,
	    gt->irq.gt_error_intr - error0);
}

/* Scrubs the fixture pages, invalidates the TLBs and frees the objects, unless the engines were reset. */
static void
i915_eu_cleanup(
	struct i915_device *device,
	struct i915_test_eu *t,
	const char *name)
{
	int error;

	/* After a hang the GPU may still use the objects: they are kept for ever. */
	if (t->wedged || t->timed_out) {
		kern_logf("i915: %s cleanup: objects kept after a hang (the engines were reset)\n", name);
		return;
	}

	/* Puts the fixture pages back to scratch and drops every translation of them. */
	drv_i915_test_eu_scrub_ptes(t, t->ce.vm, I915_TEST_EU_SHARED_VA, I915_TEST_EU_FIXTURE_PAGES);
	error = drv_i915_test_eu_invalidate_tlb(device);
	if (error != 0) {
		kern_logf("i915: %s cleanup: TLB invalidation failed rc=%d; objects kept\n", name, error);
		return;
	}

	/* Frees what the test created. */
	drv_i915_test_eu_release(t, &device->gt.mem);
	kern_logf("i915: %s cleanup: fixture unmapped, TLB invalidated, objects released\n", name);
}
