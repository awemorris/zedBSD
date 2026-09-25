/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU-free kernel checks of the release contract of a GPU-drawn
 * picture.
 *
 * They cover the GT TLB invalidation words and its failure, the render
 * release (every mapping back to scratch, re-verified on every call, the
 * TLB before any object is freed, ownership kept on failure), a render
 * target mapped for a whole run, the retained state of a GPU not shown to
 * be done up to the outer teardown, and the reclaim when the display never
 * started.  The backing pages are real DMA allocations of a DMA device of
 * the part's own; a sentinel table stands in for the GGTT and a small
 * register model for the TLB invalidation registers.  Nothing reaches the
 * hardware.  The retained-state latch is the started display's, as the
 * LCD-G run uses it; the part drops it again at its end.
 */

#include "display-ktest.h"
#include "lcd-gpu.h"
#include "lcd-run.h"
#include <kern/kcrt.h>

#include "../execution/eu-test.h"
#include "../execution/fhd-render.h"
#include "../execution/ktest.h"
#include "../fixtures/draw-fixture.h"

#include "../../display/internal.h"
#include "../../display/diagnostics.h"
#include "../../display/modeset.h"
#include "../../display/scanout.h"
#include "../../device-info.h"
#include "../../engine.h"
#include "../../ggtt.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../mmio.h"
#include "../../ppgtt.h"
#include "../../tlb.h"

#include <drivers/generic/dma.h>
#include <kern/klog.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stdint.h>

/* How many engines the invalidation is asked for: RCS0, BCS0, VCS0, VCS2 and VECS0. */
#define I915_LCDG_KTEST_ENGINES		5U

/* How many register writes the model records. */
#define I915_LCDG_KTEST_WRITES		32U

/* The first and last engine TLB invalidation registers of the Gen12 table. */
#define I915_LCDG_KTEST_TLB_FIRST	0xced8U
#define I915_LCDG_KTEST_TLB_LAST	0xcf04U

/* GEN12_OA_TLB_INV_CR, written once after the engines (Wa_2207587034). */
#define I915_LCDG_KTEST_OA_TLB		0xceecU

/* How many reads the done bit stays set after a request in the normal case. */
#define I915_LCDG_KTEST_BUSY_READS	3U

/* How many entries the stand-in GGTT has. */
#define I915_LCDG_KTEST_TABLE_ENTRIES	16384U

/* What an entry nobody wrote holds, and the scratch encoding of a free entry. */
#define I915_LCDG_KTEST_SENTINEL	0x5a5a5a5a5a5a5a5aULL
#define I915_LCDG_KTEST_SCRATCH_PTE	0x00000000dead0001ULL

/* The draw's own mappings: the state page, the batch and the texture. */
#define I915_LCDG_KTEST_DRAW_MAPS	3U

/* How many render-target pages the model draws map (a small stand-in for the 2025 pages of full HD). */
#define I915_LCDG_KTEST_RT_PAGES	4U

/* An address outside the draw's layout. */
#define I915_LCDG_KTEST_BAD_VA		0x100000000ULL

/*
 * A register model for the TLB invalidation.
 *
 * An invalidation register reads back its request bits for busy_reads
 * reads after a write and then 0 (done), or for ever when stuck.  Every
 * write is recorded in order.  One instance serves the whole part.
 */
struct i915_lcdg_ktest_tlb_model {
	/* The writes, in order, and how many were recorded. */
	uint32_t write_offset[I915_LCDG_KTEST_WRITES];
	uint32_t write_value[I915_LCDG_KTEST_WRITES];
	unsigned writes;

	/* Nonzero makes every request bit stay set: an engine that never reports done. */
	int stuck;

	/* How many reads the done bit stays set after a request, and how many are left. */
	unsigned busy_reads;
	unsigned busy_left;

	/* How many reads of an invalidation register were made. */
	unsigned polls;
};

/*
 * The register model behind the part's register block.
 *
 * It is cleared at the start of the part; the checks reset its write
 * record and fault switch as they go.
 */
static struct i915_lcdg_ktest_tlb_model i915_lcdg_ktest_model;

/*
 * The register block over the model.
 *
 * It is initialised at the start of the part; its forcewake counts return
 * to zero after every invalidation.
 */
static struct i915_mmio i915_lcdg_ktest_mmio;

/*
 * The uncore lock the invalidation takes around its writes.
 *
 * It is initialised at the start of the part and never contended.
 */
static struct spinlock i915_lcdg_ktest_uncore_lock;

/*
 * The engines the invalidation walks, and their identities.
 *
 * Only the class and instance of each are set; the structures are static
 * because the engine set holds every engine's contexts.
 */
static struct i915_gt_engines i915_lcdg_ktest_engines;
static struct i915_engine_info i915_lcdg_ktest_engine_info[I915_LCDG_KTEST_ENGINES];

/*
 * The TLB invalidation state of the part.
 *
 * Its backend and test names tag the timeout lines, so the intended
 * faults below are told apart from a hardware anomaly.
 */
static struct i915_gt_tlb i915_lcdg_ktest_tlb;

/*
 * The stand-in GGTT, the GT memory over it and the GT address space.
 *
 * They are prepared once the TLB checks ran and finalised by the outer
 * teardown check; the memory holds the object pool, too large for the
 * stack.
 */
static uint64_t i915_lcdg_ktest_table[I915_LCDG_KTEST_TABLE_ENTRIES];
static struct i915_gt_mem i915_lcdg_ktest_gm;
static struct i915_gt_ppgtt i915_lcdg_ktest_vm;

/*
 * The draw whose release is checked, the render target mapped for a whole
 * run, and the scanout buffer of the reclaim checks.
 *
 * Each check fills them afresh; a draw or a buffer kept after a GPU not
 * shown to be done stays in them for ever.
 */
static struct i915_test_fhd_render i915_lcdg_ktest_render;
static struct i915_test_fhd_rt_map i915_lcdg_ktest_rt_map;
static struct i915_scanout i915_lcdg_ktest_so;

static uint32_t i915_lcdg_ktest_read32(void *context, uint32_t offset);
static void i915_lcdg_ktest_write32(void *context, uint32_t offset, uint32_t value);
static void i915_lcdg_ktest_forcewake_request(void *context, int domain, int wake);
static int i915_lcdg_ktest_forcewake_ack(void *context, int domain);
static int i915_lcdg_ktest_is_tlb_register(uint32_t offset);
static void i915_lcdg_ktest_setup(void);
static int i915_lcdg_ktest_dma_create(struct drv_dma_device **dma);
static int i915_lcdg_ktest_invalidate(void);
static int i915_lcdg_ktest_release(void);
static void i915_lcdg_ktest_tlb_words(struct i915_ktest *ktest);
static void i915_lcdg_ktest_tlb_stuck(struct i915_ktest *ktest);
static void i915_lcdg_ktest_fault(const char *test_id);
static void i915_lcdg_ktest_no_fault(void);
static int i915_lcdg_ktest_map_draw(struct i915_test_fhd_render *x, struct i915_gt_object *rt, unsigned rt_pages);
static int i915_lcdg_ktest_map_page(struct i915_gt_object *object, unsigned page, uint64_t va);
static unsigned i915_lcdg_ktest_present_at(uint64_t va, unsigned count);
static unsigned i915_lcdg_ktest_present_ptes(unsigned rt_pages);
static int i915_lcdg_ktest_address_space(struct i915_ktest *ktest, struct drv_dma_device *dma);
static struct i915_gt_object *i915_lcdg_ktest_release_contract(struct i915_ktest *ktest);
static void i915_lcdg_ktest_mapped_target(struct i915_ktest *ktest, struct i915_gt_object *rt);
static int i915_lcdg_ktest_buffer(void);
static void i915_lcdg_ktest_not_shown(struct i915_ktest *ktest, struct i915_display *display);
static void i915_lcdg_ktest_gpu_not_done(struct i915_ktest *ktest, struct i915_display *display);

/*
 * The bus end of the part's register block.
 *
 * Reads and writes reach the TLB model; forcewake is acknowledged at once.
 */
static const struct i915_mmio_ops i915_lcdg_ktest_ops = {
	i915_lcdg_ktest_read32,
	i915_lcdg_ktest_write32,
	i915_lcdg_ktest_forcewake_request,
	i915_lcdg_ktest_forcewake_ack
};

/*
 * The reclaim checks, reported as skipped on a device without a display,
 * whose retained-state latch they use.
 */
static const char *const i915_lcdg_ktest_display_checks[] = {
	"lcdg: FIN-NOTSHOWN display never acquired + GPU done: mappings, TLB, draw objects, then the buffer -- all reclaimed, nothing retained",
	"lcdg: FIN-GPU the GPU not done: buffer, state, batch, texture kept (keep=1), mappings left, device latch set",
	"lcdg: FIN-REFUSE a re-run is refused before anything is allocated; the runner summary says retained=1",
	"lcdg: FIN-TEARDOWN the outer teardown keeps the buffer AND the objects the request may use; the latch stays",
	"lcdg: FIN-DISCARD the latch is dropped only with its memory manager finalised (GPU-free model)"
};

/*
 * Checks the release contract of a GPU-drawn picture on models.
 *
 * The TLB invalidation runs on a register model and the draws on a
 * stand-in GGTT with a real GT address space; no request is ever
 * submitted.  The reclaim checks use the started display's retained-state
 * latch and are skipped without a display.
 */
void
drv_i915_display_ktest_lcdg(
	struct i915_ktest *ktest)
{
	struct drv_dma_device *dma;
	struct i915_display *display;
	struct i915_gt_object *rt;
	unsigned count;
	unsigned index;
	int error;

	kern_logf("i915: ktest section begin: lcdg (backend=MODEL; lines tagged expected_fault=1 below are intended)\n");

	/* Builds the register model, the engines and the TLB state. */
	i915_lcdg_ktest_setup();

	/* The reference's register table and request encodings, and the wait for the done bits. */
	i915_lcdg_ktest_tlb_words(ktest);

	/* An engine that never reports done. */
	i915_lcdg_ktest_tlb_stuck(ktest);

	/* Creates the DMA device the backing pages come from. */
	dma = NULL;
	error = i915_lcdg_ktest_dma_create(&dma);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "lcdg: SETUP gt_mem / ppgtt");
		kern_logf("i915: ktest section end: lcdg\n");
		return;
	}

	/* The stand-in GGTT, the GT memory and the address space. */
	error = i915_lcdg_ktest_address_space(ktest, dma);
	if (error != 0) {
		(void)drv_dma_device_destroy(dma);
		kern_logf("i915: ktest section end: lcdg\n");
		return;
	}

	/* The render release and a render target mapped for a whole run. */
	rt = i915_lcdg_ktest_release_contract(ktest);
	if (rt != NULL) {
		i915_lcdg_ktest_mapped_target(ktest, rt);
		drv_i915_gt_object_destroy(&i915_lcdg_ktest_gm, rt);
	}

	/* The reclaim decisions need the display's retained-state latch. */
	display = NULL;
	if (ktest->device != NULL)
		display = ktest->device->display;

	/* Checks the reclaim, or names each of its checks as not run. */
	if (display != NULL) {
		i915_lcdg_ktest_not_shown(ktest, display);
		i915_lcdg_ktest_gpu_not_done(ktest, display);
	} else {
		count = sizeof(i915_lcdg_ktest_display_checks) / sizeof(i915_lcdg_ktest_display_checks[0]);
		for (index = 0U; index < count; index++)
			drv_i915_ktest_skip(ktest, i915_lcdg_ktest_display_checks[index], "the device has no display");
		drv_i915_gt_mem_fini(&i915_lcdg_ktest_gm);
	}

	/*
	 * Gives the DMA device back.  The objects kept for a GPU not shown to
	 * be done stay allocated for ever, so the device may stay open.
	 */
	error = drv_dma_device_destroy(dma);
	if (error != 0)
		kern_logf("i915: lcdg ktest: DMA device kept (error %d): it still holds the objects kept on purpose\n", error);

	kern_logf("i915: ktest section end: lcdg\n");
}

/* Reads a register of the model: an invalidation register reports its request bits until done. */
static uint32_t
i915_lcdg_ktest_read32(
	void *context,
	uint32_t offset)
{
	struct i915_lcdg_ktest_tlb_model *model;
	unsigned index;
	int invalidation;

	model = context;

	/* Registers other than the invalidation registers read as 0. */
	invalidation = i915_lcdg_ktest_is_tlb_register(offset);
	if (!invalidation)
		return 0U;

	/* Counts the poll, and finds the last request written to this register. */
	model->polls++;
	index = model->writes;
	while (index > 0U) {
		index--;

		/* A write to another register says nothing about this one. */
		if (model->write_offset[index] != offset)
			continue;

		/* The request bit never clears on a stuck engine. */
		if (model->stuck)
			return model->write_value[index] & 0xffffU;

		/* Accepted and still in progress. */
		if (model->busy_left != 0U) {
			model->busy_left--;
			return model->write_value[index] & 0xffffU;
		}

		/* Done: the bit cleared. */
		return 0U;
	}

	/* A register never requested reads as done. */
	return 0U;
}

/* Records a register write; a request starts the busy period. */
static void
i915_lcdg_ktest_write32(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct i915_lcdg_ktest_tlb_model *model;
	int invalidation;

	model = context;

	/* A request keeps the done bit set for the next busy_reads reads. */
	invalidation = i915_lcdg_ktest_is_tlb_register(offset);
	if (invalidation)
		model->busy_left = model->busy_reads;

	/* Records the write while there is room. */
	if (model->writes < I915_LCDG_KTEST_WRITES) {
		model->write_offset[model->writes] = offset;
		model->write_value[model->writes] = value;
		model->writes++;
	}
}

/* Accepts a forcewake request; the model has no power domains. */
static void
i915_lcdg_ktest_forcewake_request(
	void *context,
	int domain,
	int wake)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(domain);
	UNUSED_PARAMETER(wake);
}

/* Acknowledges every forcewake request at once. */
static int
i915_lcdg_ktest_forcewake_ack(
	void *context,
	int domain)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(domain);

	/* Every domain is awake. */
	return 1;
}

/* Reports whether a register is an engine TLB invalidation register of the model. */
static int
i915_lcdg_ktest_is_tlb_register(
	uint32_t offset)
{
	/* Below and above the engine registers. */
	if (offset < I915_LCDG_KTEST_TLB_FIRST)
		return 0;
	if (offset > I915_LCDG_KTEST_TLB_LAST)
		return 0;

	/* The OA unit's register completes at once. */
	if (offset == I915_LCDG_KTEST_OA_TLB)
		return 0;

	/* An engine's invalidation register. */
	return 1;
}

/* Builds the register model, the five engines and the TLB state. */
static void
i915_lcdg_ktest_setup(void)
{
	static const int engine_class[I915_LCDG_KTEST_ENGINES] = {
		I915_RENDER_CLASS,
		I915_COPY_ENGINE_CLASS,
		I915_VIDEO_DECODE_CLASS,
		I915_VIDEO_DECODE_CLASS,
		I915_VIDEO_ENHANCEMENT_CLASS
	};
	static const int engine_instance[I915_LCDG_KTEST_ENGINES] = {
		0,
		0,
		0,
		2,
		0
	};
	unsigned index;

	/* The register block reaches the model; no range needs forcewake. */
	kern_memset(&i915_lcdg_ktest_model, 0, sizeof(i915_lcdg_ktest_model));
	drv_i915_mmio_init(&i915_lcdg_ktest_mmio, &i915_lcdg_ktest_ops, &i915_lcdg_ktest_model, NULL, 0U, NULL);
	spin_init(&i915_lcdg_ktest_uncore_lock, LOCK_RANK_DEVICE, "lcdg-ktest-uncore");

	/* Names each engine's class and instance; nothing else of an engine is used. */
	kern_memset(&i915_lcdg_ktest_engines, 0, sizeof(i915_lcdg_ktest_engines));
	kern_memset(i915_lcdg_ktest_engine_info, 0, sizeof(i915_lcdg_ktest_engine_info));
	for (index = 0U; index < I915_LCDG_KTEST_ENGINES; index++) {
		i915_lcdg_ktest_engine_info[index].class = engine_class[index];
		i915_lcdg_ktest_engine_info[index].instance = engine_instance[index];
		i915_lcdg_ktest_engines.ge[index].info = &i915_lcdg_ktest_engine_info[index];
	}
	i915_lcdg_ktest_engines.n = I915_LCDG_KTEST_ENGINES;

	/* The timeout lines of the part name the model and the test. */
	kern_memset(&i915_lcdg_ktest_tlb, 0, sizeof(i915_lcdg_ktest_tlb));
	i915_lcdg_ktest_tlb.backend = "MODEL";
	i915_lcdg_ktest_tlb.test_id = "lcdg-ktest";
}

/* Creates a DMA device with the GPU's DMA constraints. */
static int
i915_lcdg_ktest_dma_create(
	struct drv_dma_device **dma)
{
	static const struct drv_dma_constraints constraints = {
		39U,
		0xffffffffU,
		0U,
		1
	};
	int error;

	/* Declares the GPU's 39-bit DMA capability, as the device start does. */
	error = drv_dma_device_create(&constraints, dma);
	if (error != 0)
		return error;

	/* Succeeded: the device hands out pages the GPU can address. */
	return 0;
}

/* Invalidates the TLBs of the part's engines on the model. */
static int
i915_lcdg_ktest_invalidate(void)
{
	int error;

	/* Runs the full invalidation. */
	error = drv_i915_gt_invalidate_tlb_full(&i915_lcdg_ktest_tlb, &i915_lcdg_ktest_engines, &i915_lcdg_ktest_mmio, &i915_lcdg_ktest_uncore_lock);
	if (error != 0)
		return error;

	/* Succeeded: every engine reported done. */
	return 0;
}

/* Releases the part's draw in the reference's order. */
static int
i915_lcdg_ktest_release(void)
{
	int error;

	/* Mappings back to scratch, the TLB, then the draw's own objects. */
	error = drv_i915_test_fhd_render_release(&i915_lcdg_ktest_render, &i915_lcdg_ktest_gm, &i915_lcdg_ktest_vm, &i915_lcdg_ktest_tlb, &i915_lcdg_ktest_engines, &i915_lcdg_ktest_mmio, &i915_lcdg_ktest_uncore_lock);
	if (error != 0)
		return error;

	/* Succeeded: the draw holds nothing any more. */
	return 0;
}

/* Checks the request words of every engine, the OA workaround, the seqno and the wait for the done bits. */
static void
i915_lcdg_ktest_tlb_words(
	struct i915_ktest *ktest)
{
	struct i915_lcdg_ktest_tlb_model *model;
	int passed;
	int error;

	model = &i915_lcdg_ktest_model;

	/* Invalidates every engine; each done bit reads 1 three times, then 0. */
	model->busy_reads = I915_LCDG_KTEST_BUSY_READS;
	model->polls = 0U;
	error = i915_lcdg_ktest_invalidate();

	/*
	 * The Gen12 table as the writes show it: RCS 0xced8 and BCS 0xcee4
	 * plain, VCS 0xcedc and VECS 0xcee0 masked with BIT(instance).  The
	 * table lookup itself is private to the TLB code, so the words are
	 * read from what reached the registers.
	 */
	passed = 0;
	if (model->writes >= I915_LCDG_KTEST_ENGINES &&
	    model->write_offset[0] == 0xced8U &&
	    model->write_value[0] == 1U &&
	    model->write_offset[1] == 0xcee4U &&
	    model->write_value[1] == 1U &&
	    model->write_offset[2] == 0xcedcU &&
	    model->write_value[2] == 0x00010001U &&
	    model->write_offset[3] == 0xcedcU &&
	    model->write_value[3] == 0x00040004U &&
	    model->write_offset[4] == 0xcee0U &&
	    model->write_value[4] == 0x00010001U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: TLB-REGS gen12 table as written: RCS 0xced8 / BCS 0xcee4 plain, VCS 0xcedc / VECS 0xcee0 masked BIT(instance)");

	/* Every engine's request, then the OA unit's, and a completed generation. */
	passed = 0;
	if (error == 0 &&
	    model->writes == 6U &&
	    model->write_offset[0] == 0xced8U &&
	    model->write_offset[3] == 0xcedcU &&
	    model->write_value[3] == 0x00040004U &&
	    model->write_offset[5] == I915_LCDG_KTEST_OA_TLB &&
	    model->write_value[5] == 1U &&
	    i915_lcdg_ktest_tlb.seqno == 2U &&
	    i915_lcdg_ktest_tlb.engines_invalidated == 5U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: TLB-FULL every engine's request, then Wa_2207587034 (OA 0xceec), done bits back to 0, seqno += 2");

	/* The wait read each busy bit until it cleared: one read per engine plus the busy reads. */
	passed = 0;
	if (model->polls >= I915_LCDG_KTEST_ENGINES + I915_LCDG_KTEST_BUSY_READS)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: TLB-POLL the done bit reads 1 after the request and clears later: the wait polls until it reads 0");
}

/* Checks that an engine that never reports done fails the invalidation without a new generation. */
static void
i915_lcdg_ktest_tlb_stuck(
	struct i915_ktest *ktest)
{
	int passed;
	int error;

	/* The request bits never clear; the timeout line is tagged as intended. */
	i915_lcdg_ktest_model.writes = 0U;
	i915_lcdg_ktest_fault("TLB-STUCK");
	error = i915_lcdg_ktest_invalidate();

	/* A timeout, counted, and the seqno left where the last completed invalidation put it. */
	passed = 0;
	if (error == ETIMEDOUT &&
	    i915_lcdg_ktest_tlb.timeouts >= 1U &&
	    i915_lcdg_ktest_tlb.seqno == 2U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: TLB-STUCK an engine that never reports done is ETIMEDOUT, and the seqno does not advance");

	/* The engines complete again. */
	i915_lcdg_ktest_no_fault();
}

/* Makes every engine stop reporting done, and tags the timeout lines as an intended fault. */
static void
i915_lcdg_ktest_fault(
	const char *test_id)
{
	/* The request bits never clear from now on. */
	i915_lcdg_ktest_model.stuck = 1;

	/* A timeout line of this test is intended. */
	i915_lcdg_ktest_tlb.expected_fault = 1;
	i915_lcdg_ktest_tlb.test_id = test_id;
}

/* Lets every engine report done again, and tags the timeout lines as unexpected. */
static void
i915_lcdg_ktest_no_fault(void)
{
	/* The request bits clear after the busy reads. */
	i915_lcdg_ktest_model.stuck = 0;

	/* A timeout line from now on would be an anomaly. */
	i915_lcdg_ktest_tlb.expected_fault = 0;
	i915_lcdg_ktest_tlb.test_id = "lcdg-ktest";
}

/*
 * Builds a draw that was submitted but never ran: its three objects mapped
 * and, unless rt_pages is 0, the target's first pages.
 */
static int
i915_lcdg_ktest_map_draw(
	struct i915_test_fhd_render *x,
	struct i915_gt_object *rt,
	unsigned rt_pages)
{
	static const uint64_t draw_va[I915_LCDG_KTEST_DRAW_MAPS] = {
		I915_TEST_EU_SHARED_VA,
		I915_TEST_EU_BATCH_VA,
		I915_TEX_FIXTURE_TEX_VA
	};
	struct i915_gt_object *draw_object[I915_LCDG_KTEST_DRAW_MAPS];
	struct i915_gt_mem *gm;
	struct i915_gt_ppgtt *vm;
	unsigned index;
	int error;

	gm = &i915_lcdg_ktest_gm;
	vm = &i915_lcdg_ktest_vm;

	/* Starts an empty draw into the target's usual address. */
	kern_memset(x, 0, sizeof(*x));
	x->rt_va = I915_TEX_FHD_RT_VA;

	/* Creates the state page, the batch and the texture. */
	x->t.shared = drv_i915_gt_object_create(gm, 4096U);
	x->t.batch = drv_i915_gt_object_create(gm, 4096U);
	x->tex = drv_i915_gt_object_create(gm, 4096U);
	if (x->t.shared == NULL || x->t.batch == NULL || x->tex == NULL)
		return ENOMEM;

	/* Allocates the page tables of the draw's own range. */
	error = drv_i915_gt_ppgtt_alloc_range(gm, vm, I915_TEST_EU_SHARED_VA, 5U * 4096U);
	if (error != 0)
		return error;

	/* Allocates the page tables of the target's range. */
	if (rt_pages != 0U) {
		error = drv_i915_gt_ppgtt_alloc_range(gm, vm, I915_TEX_FHD_RT_VA, (uint64_t)rt_pages * 4096U);
		if (error != 0)
			return error;
	}

	/* Maps the state page, the batch and the texture. */
	draw_object[0] = x->t.shared;
	draw_object[1] = x->t.batch;
	draw_object[2] = x->tex;
	for (index = 0U; index < I915_LCDG_KTEST_DRAW_MAPS; index++) {
		error = i915_lcdg_ktest_map_page(draw_object[index], 0U, draw_va[index]);
		if (error != 0)
			return error;
	}

	/* Maps the target's pages, counting each one the draw now owns a mapping of. */
	for (index = 0U; index < rt_pages; index++) {
		error = i915_lcdg_ktest_map_page(rt, index, I915_TEX_FHD_RT_VA + (uint64_t)index * 4096U);
		if (error != 0)
			return error;

		x->rt_pages_mapped++;
	}

	/* The draw was submitted; whether the GPU is done is up to the check. */
	x->rt = rt;
	x->t.submitted = 1;

	/* Succeeded: the draw's mappings are in place. */
	return 0;
}

/* Maps one page of an object at an address of the part's address space. */
static int
i915_lcdg_ktest_map_page(
	struct i915_gt_object *object,
	unsigned page,
	uint64_t va)
{
	uint64_t dma;
	int error;

	/* Finds where the page lives. */
	dma = 0U;
	error = drv_i915_gt_object_page_dma(object, page, &dma);
	if (error != 0)
		return error;

	/* Writes the leaf entry. */
	error = drv_i915_gt_ppgtt_insert_page(&i915_lcdg_ktest_vm, dma, va, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the page is mapped. */
	return 0;
}

/* Counts the pages from va on whose leaf entry names a page rather than scratch. */
static unsigned
i915_lcdg_ktest_present_at(
	uint64_t va,
	unsigned count)
{
	struct i915_test_ppgtt_walk walk;
	unsigned index;
	unsigned present;
	int error;

	/* Walks each page's tables the way the GPU does. */
	present = 0U;
	for (index = 0U; index < count; index++) {
		error = drv_i915_test_ppgtt_walk(&i915_lcdg_ktest_vm, va + (uint64_t)index * 4096U, &walk);
		if (error != 0)
			continue;

		/* A leaf reached, present and not the scratch encoding. */
		if (walk.levels == 4 && walk.leaf_present && !walk.scratch[3])
			present++;
	}

	/* Reports how many pages are still translated. */
	return present;
}

/* Counts the draw's own three mappings and its first target pages that are still translated. */
static unsigned
i915_lcdg_ktest_present_ptes(
	unsigned rt_pages)
{
	unsigned present;

	/* The state page, the batch and the texture. */
	present = i915_lcdg_ktest_present_at(I915_TEST_EU_SHARED_VA, 1U);
	present += i915_lcdg_ktest_present_at(I915_TEST_EU_BATCH_VA, 1U);
	present += i915_lcdg_ktest_present_at(I915_TEX_FIXTURE_TEX_VA, 1U);

	/* The target's pages at its usual address. */
	present += i915_lcdg_ktest_present_at(I915_TEX_FHD_RT_VA, rt_pages);

	/* Reports how many are still translated. */
	return present;
}

/* Prepares the stand-in GGTT, the GT memory and the GT address space; EIO when they cannot be made. */
static int
i915_lcdg_ktest_address_space(
	struct i915_ktest *ktest,
	struct drv_dma_device *dma)
{
	unsigned index;
	int error;

	/* Marks every entry of the stand-in GGTT as not written. */
	for (index = 0U; index < I915_LCDG_KTEST_TABLE_ENTRIES; index++)
		i915_lcdg_ktest_table[index] = I915_LCDG_KTEST_SENTINEL;

	/* Prepares the GT memory over it. */
	error = drv_i915_gt_mem_init(&i915_lcdg_ktest_gm, dma, I915_DMA_MAX_ADDRESS, i915_lcdg_ktest_table, I915_LCDG_KTEST_TABLE_ENTRIES, I915_LCDG_KTEST_SCRATCH_PTE, NULL);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "lcdg: SETUP gt_mem / ppgtt");
		return EIO;
	}

	/* Creates the GT address space the draws map into. */
	error = drv_i915_gt_ppgtt_create(&i915_lcdg_ktest_gm, &i915_lcdg_ktest_vm);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "lcdg: SETUP gt_mem / ppgtt");
		drv_i915_gt_mem_fini(&i915_lcdg_ktest_gm);
		return EIO;
	}

	/* Succeeded: draws can be mapped. */
	return 0;
}

/* Checks the render release: busy, a failed TLB, the retry and a further call; returns the target, or NULL. */
static struct i915_gt_object *
i915_lcdg_ktest_release_contract(
	struct i915_ktest *ktest)
{
	struct i915_test_fhd_render *fr;
	struct i915_gt_mem *gm;
	struct i915_gt_object *rt;
	unsigned live;
	unsigned present;
	int passed;
	int error;
	int release_error;

	fr = &i915_lcdg_ktest_render;
	gm = &i915_lcdg_ktest_gm;

	/* A four-page target and a draw into it that the GPU is not shown to be done with. */
	rt = drv_i915_gt_object_create(gm, I915_LCDG_KTEST_RT_PAGES * 4096U);
	error = ENOMEM;
	if (rt != NULL)
		error = i915_lcdg_ktest_map_draw(fr, rt, I915_LCDG_KTEST_RT_PAGES);
	live = gm->objects_live;

	/* Releases it. */
	release_error = error;
	if (error == 0)
		release_error = i915_lcdg_ktest_release();
	present = i915_lcdg_ktest_present_ptes(I915_LCDG_KTEST_RT_PAGES);

	/* Nothing was unmapped or freed. */
	passed = 0;
	if (error == 0 &&
	    release_error == EBUSY &&
	    present == 7U &&
	    gm->objects_live == live &&
	    !fr->released)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: REL-BUSY the GPU not shown to be done: nothing is unmapped or freed");

	/* Nothing else can be checked without the draw. */
	if (error != 0)
		return rt;

	/* The GPU is done, but the TLB invalidation fails. */
	fr->gpu_done = 1;
	i915_lcdg_ktest_fault("REL-TLB");
	release_error = i915_lcdg_ktest_release();
	present = i915_lcdg_ktest_present_ptes(I915_LCDG_KTEST_RT_PAGES);

	/* Every mapping is back at scratch, but no object was freed and the draw still owns everything. */
	passed = 0;
	if (release_error == ETIMEDOUT &&
	    fr->maps_scratch == 7U &&
	    fr->maps_total == 7U &&
	    present == 0U &&
	    gm->objects_live == live &&
	    fr->tex != NULL &&
	    fr->t.shared != NULL &&
	    fr->rt == rt &&
	    !fr->released)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: REL-TLB all 7 PTEs back to scratch, but the TLB invalidation failed: no object is freed, ownership stays");

	/* The TLB works again; the release is retried. */
	i915_lcdg_ktest_no_fault();
	release_error = i915_lcdg_ktest_release();

	/* The retry counted the mappings afresh, then freed the draw's own three objects and not the target. */
	passed = 0;
	if (release_error == 0 &&
	    fr->released &&
	    fr->maps_scratch == 7U &&
	    fr->maps_total == 7U &&
	    fr->release_calls == 3U &&
	    gm->objects_live == live - 3U &&
	    rt->in_use &&
	    fr->rt == NULL)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: REL-RETRY the retry re-verifies (7 of 7, not 14): then the TLB, then the draw's 3 objects -- the target itself is not freed");

	/* Releases once more. */
	release_error = i915_lcdg_ktest_release();

	/* A released draw stays as it is. */
	passed = 0;
	if (release_error == 0 && fr->release_calls == 3U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: REL-ONCE a further call after success changes nothing");

	/* Succeeded: the target is free for the next checks. */
	return rt;
}

/* Checks a render target mapped for a whole run: the draws leave it, its own unmap takes it down. */
static void
i915_lcdg_ktest_mapped_target(
	struct i915_ktest *ktest,
	struct i915_gt_object *rt)
{
	const struct i915_test_fhd_va *layout;
	struct i915_test_fhd_render *fr;
	struct i915_test_fhd_rt_map *map;
	struct i915_gt_mem *gm;
	unsigned count;
	unsigned live;
	unsigned present;
	unsigned present_b;
	int passed;
	int error;
	int release_error;

	fr = &i915_lcdg_ktest_render;
	map = &i915_lcdg_ktest_rt_map;
	gm = &i915_lcdg_ktest_gm;

	/* Reads the draw's layout table and its overlap check. */
	layout = NULL;
	count = 0U;
	error = drv_i915_test_fhd_va_layout(&layout, &count);

	/* Five ranges without overlap, the fifth being render target B. */
	passed = 0;
	if (error == 0 &&
	    layout != NULL &&
	    count == 5U &&
	    layout[4].va == I915_TEX_FHD_RT_B_VA)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: RTMAP-LAYOUT the second render target has its own VA range, no overlap with A or the draw's pages");

	/* Maps every page of the target at B's address. */
	error = drv_i915_test_fhd_rt_map(map, gm, &i915_lcdg_ktest_vm, rt, I915_TEX_FHD_RT_B_VA);
	present_b = i915_lcdg_ktest_present_at(I915_TEX_FHD_RT_B_VA, I915_LCDG_KTEST_RT_PAGES);

	/* All four pages are mapped and the walk reaches the object's own pages. */
	passed = 0;
	if (error == 0 &&
	    map->mapped == I915_LCDG_KTEST_RT_PAGES &&
	    map->walk_ok &&
	    present_b == I915_LCDG_KTEST_RT_PAGES)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: RTMAP-MAP every page of the target at B's VA, the walk names the object's own pages");

	/* A draw into the premapped target, done on the GPU, is released. */
	error = i915_lcdg_ktest_map_draw(fr, rt, 0U);
	fr->rt_va = I915_TEX_FHD_RT_B_VA;
	fr->rt_premapped = 1;
	fr->gpu_done = 1;
	live = gm->objects_live;
	release_error = error;
	if (error == 0)
		release_error = i915_lcdg_ktest_release();
	present = i915_lcdg_ktest_present_ptes(0U);
	present_b = i915_lcdg_ktest_present_at(I915_TEX_FHD_RT_B_VA, I915_LCDG_KTEST_RT_PAGES);

	/* Only the draw's own three mappings and objects went; the target stays mapped. */
	passed = 0;
	if (release_error == 0 &&
	    fr->maps_total == 3U &&
	    fr->maps_scratch == 3U &&
	    present == 0U &&
	    present_b == I915_LCDG_KTEST_RT_PAGES &&
	    gm->objects_live == live - 3U &&
	    rt->in_use)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: RTMAP-DRAW a draw into the mapped target releases only its own 3 PTEs + objects; the target stays mapped");

	/* The target's own unmap meets a failing TLB invalidation. */
	i915_lcdg_ktest_fault("RTMAP-TLB");
	release_error = drv_i915_test_fhd_rt_unmap(map, &i915_lcdg_ktest_vm, &i915_lcdg_ktest_tlb, &i915_lcdg_ktest_engines, &i915_lcdg_ktest_mmio, &i915_lcdg_ktest_uncore_lock);
	present_b = i915_lcdg_ktest_present_at(I915_TEX_FHD_RT_B_VA, I915_LCDG_KTEST_RT_PAGES);

	/* The mappings are at scratch, but the owner may not free the target yet. */
	passed = 0;
	if (release_error == ETIMEDOUT &&
	    !map->released &&
	    map->scratch == I915_LCDG_KTEST_RT_PAGES &&
	    present_b == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: RTMAP-TLB PTEs back to scratch but the TLB failed: not released (the owner must not free the target)");

	/* The TLB works again; the unmap is retried. */
	i915_lcdg_ktest_no_fault();
	release_error = drv_i915_test_fhd_rt_unmap(map, &i915_lcdg_ktest_vm, &i915_lcdg_ktest_tlb, &i915_lcdg_ktest_engines, &i915_lcdg_ktest_mmio, &i915_lcdg_ktest_uncore_lock);

	/* A retry that counted 4 of 4 afresh and invalidated the TLB is called once more. */
	error = EINVAL;
	if (release_error == 0 &&
	    map->released &&
	    map->scratch == I915_LCDG_KTEST_RT_PAGES &&
	    map->unmap_calls == 2U)
		error = drv_i915_test_fhd_rt_unmap(map, &i915_lcdg_ktest_vm, &i915_lcdg_ktest_tlb, &i915_lcdg_ktest_engines, &i915_lcdg_ktest_mmio, &i915_lcdg_ktest_uncore_lock);

	/* The further call changed nothing. */
	passed = 0;
	if (error == 0 && map->unmap_calls == 2U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: RTMAP-RETRY the retry re-verifies 4 of 4 (not 8), then the TLB; a further call changes nothing");

	/* Maps the target at an address outside the layout. */
	error = drv_i915_test_fhd_rt_map(map, gm, &i915_lcdg_ktest_vm, rt, I915_LCDG_KTEST_BAD_VA);

	/* The map is refused. */
	passed = 0;
	if (error == EINVAL)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: RTMAP-VA a VA outside the layout is refused");
}

/* Makes a small pinned scanout buffer and a draw into it; 0, or the first failure. */
static int
i915_lcdg_ktest_buffer(void)
{
	struct i915_scanout *so;
	int error;

	so = &i915_lcdg_ktest_so;

	/* An empty storage for a 64x64 buffer. */
	kern_memset(so, 0, sizeof(*so));
	error = drv_i915_scanout_create(&i915_lcdg_ktest_gm, 64U, 64U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);
	if (error != 0)
		return error;

	/* Pins it for the display. */
	error = drv_i915_scanout_pin(so, "lcdg-ktest");
	if (error != 0)
		return error;

	/* A draw into its first pages. */
	error = i915_lcdg_ktest_map_draw(&i915_lcdg_ktest_render, so->obj, I915_LCDG_KTEST_RT_PAGES);
	if (error != 0)
		return error;

	/* Succeeded: the buffer has two users, the display and the GPU. */
	return 0;
}

/* Checks the reclaim when the display never started and the GPU is done. */
static void
i915_lcdg_ktest_not_shown(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_gt_mem *gm;
	unsigned live;
	int render_error;
	int released;
	int retained;
	int passed;
	int error;

	gm = &i915_lcdg_ktest_gm;

	/* Claims the display window and makes a buffer with a draw the GPU is done with. */
	error = drv_i915_gt_display_window_init(gm, I915_GT_DISPLAY_PAGES);
	if (error == 0)
		error = i915_lcdg_ktest_buffer();
	i915_lcdg_ktest_render.gpu_done = 1;
	live = gm->objects_live;

	/* Decides the release: the display never acquired the buffer. */
	released = 0;
	render_error = 0;
	if (error == 0)
		released = drv_i915_test_lcdg_finish(display, &i915_lcdg_ktest_render, &i915_lcdg_ktest_so, 0, 0, gm, &i915_lcdg_ktest_vm, &i915_lcdg_ktest_tlb, &i915_lcdg_ktest_engines, &i915_lcdg_ktest_mmio, &i915_lcdg_ktest_uncore_lock, &render_error);
	retained = drv_i915_lcd_show_retained(display);

	/* Mappings, TLB, the draw's three objects and then the buffer were all given back. */
	passed = 0;
	if (error == 0 &&
	    released == 1 &&
	    render_error == 0 &&
	    i915_lcdg_ktest_so.state == I915_SCANOUT_NONE &&
	    gm->objects_live == live - 4U &&
	    gm->display_allocated_pages == 0U &&
	    !retained)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: FIN-NOTSHOWN display never acquired + GPU done: mappings, TLB, draw objects, then the buffer -- all reclaimed, nothing retained");
}

/* Checks that a GPU not shown to be done keeps everything up to the outer teardown, and the latch until discarded. */
static void
i915_lcdg_ktest_gpu_not_done(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_test_fhd_render *fr;
	struct i915_scanout *so;
	struct i915_gt_mem *gm;
	unsigned live;
	unsigned present;
	int render_error;
	int released;
	int gpu_retained;
	int abandoned;
	int kernel_gpu_retained;
	int refused;
	int discarded;
	int kept;
	int alive;
	int passed;
	int error;

	fr = &i915_lcdg_ktest_render;
	so = &i915_lcdg_ktest_so;
	gm = &i915_lcdg_ktest_gm;

	/* A buffer with a draw that was submitted and never retired: a hang not shown to be over. */
	error = i915_lcdg_ktest_buffer();
	fr->gpu_done = 0;
	live = gm->objects_live;

	/* Decides the release. */
	released = 9;
	render_error = 0;
	if (error == 0)
		released = drv_i915_test_lcdg_finish(display, fr, so, 0, 0, gm, &i915_lcdg_ktest_vm, &i915_lcdg_ktest_tlb, &i915_lcdg_ktest_engines, &i915_lcdg_ktest_mmio, &i915_lcdg_ktest_uncore_lock, &render_error);
	present = i915_lcdg_ktest_present_ptes(I915_LCDG_KTEST_RT_PAGES);
	gpu_retained = drv_i915_lcd_show_gpu_retained(display);
	abandoned = drv_i915_lcd_kernel_abandoned(display);
	kernel_gpu_retained = drv_i915_lcd_kernel_gpu_retained(display);

	/* Every object the request may use must be there to be examined. */
	kept = 0;
	if (error == 0 &&
	    so->obj != NULL &&
	    fr->tex != NULL &&
	    fr->t.shared != NULL &&
	    fr->t.batch != NULL) {
		/* Each of them is marked keep. */
		if (so->obj->keep &&
		    fr->tex->keep &&
		    fr->t.shared->keep &&
		    fr->t.batch->keep)
			kept = 1;
	}

	/* The buffer, the state, the batch and the texture are kept, the mappings left, the latch set. */
	passed = 0;
	if (released == 0 &&
	    so->state == I915_SCANOUT_ABANDONED &&
	    kept &&
	    present == 7U &&
	    gm->objects_live == live &&
	    gpu_retained &&
	    abandoned &&
	    kernel_gpu_retained)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: FIN-GPU the GPU not done: buffer, state, batch, texture kept (keep=1), mappings left, device latch set");

	/*
	 * A further panel run is refused before anything is allocated.  The
	 * refusal is the retained-state test of the panel runs; the LCD-G
	 * scenario itself is not started from the unit tests, because it
	 * would light the panel if it were not refused.
	 */
	refused = drv_i915_test_lcd_retained(display);

	/* The retained state refuses the run, and nothing was allocated meanwhile. */
	passed = 0;
	if (refused == 1 && gm->objects_live == live)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: FIN-REFUSE a re-run is refused before anything is allocated; the runner summary says retained=1");

	/* The outer teardown of the GT memory. */
	drv_i915_gt_mem_fini(gm);
	gpu_retained = drv_i915_lcd_show_gpu_retained(display);

	/* Reads whether every kept object is still in its pool slot. */
	alive = 0;
	if (kept) {
		/* Each of them survived the teardown. */
		if (so->obj->in_use &&
		    fr->tex->in_use &&
		    fr->t.shared->in_use &&
		    fr->t.batch->in_use)
			alive = 1;
	}

	/* The buffer and every object the request may use survive it; the latch stays. */
	passed = 0;
	if (gm->kept_objects >= 4U && alive && gpu_retained)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: FIN-TEARDOWN the outer teardown keeps the buffer AND the objects the request may use; the latch stays");

	/* Drops the latch with its memory manager finalised. */
	discarded = drv_i915_lcd_show_discard_gpu_model(display, gm, 1);
	gpu_retained = drv_i915_lcd_show_gpu_retained(display);

	/* The latch is gone. */
	passed = 0;
	if (discarded == 0 && !gpu_retained)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcdg: FIN-DISCARD the latch is dropped only with its memory manager finalised (GPU-free model)");
}
