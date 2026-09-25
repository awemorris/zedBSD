/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The in-kernel tests of the GT.
 *
 * The parts follow the order the device start brings the GT up in: the fuse
 * and clock decode and the engine table, the GT half of the interrupt
 * handler, the workaround, MOCS and PAT tables with RC6 and RPS, the
 * forcewake map, then the GT memory and everything built on it -- the GGTT
 * window, the kernel PPGTT, the engines, the context image, the requests,
 * the execlists submission and its context status buffer, the resume
 * pieces, the default-state recording, the workaround verification, and the
 * migrate and PXP contexts.  The hotplug interrupt setup and the DC_off
 * power well of the display probe, and the fixed batches and states the
 * hardware tests submit, are checked at the end.
 *
 * Nothing here touches the started device.  Every register access goes to
 * the register model of this file.  The GT memory is a pool of this file's
 * own over a GGTT table in RAM and a DMA device created for the tests; the
 * engines, contexts and requests are built on it and no request reaches an
 * engine: the tests play the engine by writing the status page and the
 * context status buffer themselves.
 */

#include "ktest.h"
#include "eu-test.h"
#include "../fixtures/draw-fixture.h"
#include <kern/kcrt.h>

#include "../../context.h"
#include "../../defaults.h"
#include "../../device-info.h"
#include "../../engine.h"
#include "../../ggtt.h"
#include "../../gt-power.h"
#include "../../irq.h"
#include "../../memory.h"
#include "../../migrate.h"
#include "../../mmio.h"
#include "../../ppgtt.h"
#include "../../pxp.h"
#include "../../request.h"
#include "../../submit.h"
#include "../../verify-workarounds.h"
#include "../../workarounds.h"
#include "../../display/hotplug.h"
#include "../../display/power.h"

#include <drivers/generic/dma.h>
#include <hal/hal.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../../intel/commands.h"
#include "../../intel/gt-regs.h"

/* The engines Alder Lake-P can have: RCS0, BCS0, VCS0, VCS2 and VECS0. */
#define I915_KTEST_ADLP_ENGINES		((1U << I915_RCS0) | (1U << I915_BCS0) | (1U << I915_VCS0) | (1U << I915_VCS2) | (1U << I915_VECS0))

/* The registers the model gives a meaning to: GEN6_GDRST and the PCODE mailbox. */
#define I915_KTEST_REG_GDRST		0x941cU
#define I915_KTEST_REG_PCODE_MAILBOX	0x138124U
#define I915_KTEST_REG_PCODE_DATA	0x138128U
#define I915_KTEST_REG_PCODE_DATA1	0x13812cU

/* The fuse registers the GT information step reads. */
#define I915_KTEST_REG_EU_DISABLE	0x9134U
#define I915_KTEST_REG_SLICE_ENABLE	0x9138U
#define I915_KTEST_REG_DSS_ENABLE	0x913cU
#define I915_KTEST_REG_MIRROR_FUSE3	0x9118U
#define I915_KTEST_REG_MEDIA_FUSE	0x9140U

/* The clock registers the GT information step reads. */
#define I915_KTEST_REG_CTC_MODE		0xa26cU
#define I915_KTEST_REG_RPM_CONFIG0	0xd00U

/* DC_STATE_EN, the register the DC_off power well moves. */
#define I915_KTEST_REG_DC_STATE_EN	0x45504U

/* How many other registers the model remembers, and how many writes its trace keeps. */
#define I915_KTEST_FAKE_REGS		96U
#define I915_KTEST_FAKE_WRITES		96U

/* How many entries the GGTT table in RAM has. */
#define I915_KTEST_GGTT_ENTRIES		512U

/* How many dwords the batch and state buffers of the fixture tests hold. */
#define I915_KTEST_BATCH_DWORDS		1024U

/* The thread count the compute batch is built with. */
#define I915_KTEST_EU_MAX_THREADS	559U

/* A context status buffer event that promotes the pending context, and one that completes it. */
#define I915_KTEST_CSB_PROMOTE		((((uint64_t)(0x7ffU << 15)) << 32) | ((1U << 15) | 1U))
#define I915_KTEST_CSB_COMPLETE		((((uint64_t)(1U << 15)) << 32) | (0x7ffU << 15))

/* The status-page dwords the tests play the engine through: the preempt dword and the seqno slot. */
#define I915_KTEST_HWSP_PREEMPT		0x32U
#define I915_KTEST_HWSP_SEQNO		0x40U

/* The FNV-1a offset basis and prime the byte pins are computed with. */
#define I915_KTEST_FNV_OFFSET		0xcbf29ce484222325ULL
#define I915_KTEST_FNV_PRIME		0x100000001b3ULL

/*
 * The register-state slots of the context image the tests read (lrc
 * layout of Gen12: the value slot follows each register's offset).
 */
#define I915_KTEST_LRC_RING_WA_BB_PER_CTX	0x12
#define I915_KTEST_LRC_RING_INDIRECT_PTR	0x14
#define I915_KTEST_LRC_RING_INDIRECT_OFFSET	0x16
#define I915_KTEST_LRC_MI_MODE_INDEX		0x60
#define I915_KTEST_LRC_BB_OFFSET_INDEX		0x70

/*
 * A register model of the registers the GT tests reach.
 *
 * One instance is shared by the tests, which run one after another; a test
 * that needs a clean model clears it with i915_fake_open().  A reset request
 * completes once written, a PCODE command completes at once with status 0,
 * and every other register keeps what was last written to it, for as many
 * registers as the model has room for; later ones are dropped and read zero.
 * Every write is also appended to an ordered trace while it has room, so a
 * test can check which registers were written, with which values and in
 * which order.
 */
struct i915_fake_mmio {
	/* How many reset requests were written. */
	unsigned gdrst_writes;

	/* The PCODE mailbox and its two data words. */
	uint32_t mailbox;
	uint32_t data;
	uint32_t data1;

	/* Every other register written, by offset. */
	uint32_t gen_off[I915_KTEST_FAKE_REGS];
	uint32_t gen_val[I915_KTEST_FAKE_REGS];
	unsigned gen_n;

	/* The writes in the order they were made. */
	uint32_t wt_off[I915_KTEST_FAKE_WRITES];
	uint32_t wt_val[I915_KTEST_FAKE_WRITES];
	unsigned wt_n;
};

/*
 * One register the forcewake map is expected to put in a domain.
 */
struct i915_ktest_domain_expect {
	/* The register. */
	uint32_t offset;

	/* The domain it must belong to; -1 means always on. */
	int domain;
};

/*
 * Everything the GT tests build.
 *
 * The objects are far too large for the 16 KiB kernel stack, and the tests
 * run one after another on the start worker, so one instance serves them
 * all.  Each part initializes what it uses; nothing here is shared with the
 * started device.
 */
struct i915_ktest_gt_state {
	/* The register block the tests reach the model through, and the model. */
	struct i915_mmio mmio;
	struct i915_fake_mmio fake;

	/* The GT information and interrupt device of the information and interrupt tests. */
	struct i915_gt_info info;
	struct i915_irq_dev irq;

	/* The GT the workaround tables are built for, the tables, a scratch list, RC6 and RPS. */
	struct i915_gt_info wa_gt;
	struct i915_gt_init wa_init;
	struct i915_wa_list apply_list;
	struct i915_rc6 rc6;
	struct i915_rps rps;

	/* The GGTT table in RAM, the DMA device, the GT memory and the kernel PPGTT. */
	uint64_t ggtt_table[I915_KTEST_GGTT_ENTRIES];
	struct drv_dma_device *dma;
	struct i915_gt_mem gm;
	struct i915_gt_ppgtt pp;

	/* The slice information every simulated engine is set up with. */
	struct i915_sseu sseu;

	/* The render engine and its context. */
	struct i915_engine_info rcs_info;
	struct i915_gt_engine rcs;
	struct i915_gt_context rcs_ce;

	/* The second video decode engine of the descriptor check. */
	struct i915_engine_info vcs2_info;
	struct i915_gt_engine vcs2;

	/* The first video decode engine and its context. */
	struct i915_engine_info vcs0_info;
	struct i915_gt_engine vcs0;
	struct i915_gt_context vcs0_ce;

	/* The copy engine, its context and its execlists state. */
	struct i915_engine_info bcs_info;
	struct i915_gt_engine bcs;
	struct i915_gt_context bcs_ce;
	struct i915_execlists bcs_el;

	/* The request the render and copy tests write, and the context workarounds it emits. */
	struct i915_gt_request rq;
	struct i915_wa_list ctx_wa;

	/* The RC6 and RPS state the sanitize tests run on. */
	struct i915_rc6 sanitize_rc6;
	struct i915_rps sanitize_rps;

	/* The simulated GT, its tables and engines, the recording, and the uncore lock the reset takes. */
	struct i915_gt_info defaults_gt;
	struct i915_gt_init defaults_init;
	struct i915_gt_engines engines;
	struct i915_gt_defaults defaults;
	struct i915_gt_context inherit_ce;
	struct spinlock wedge_lock;

	/* The workaround verification, the migrate context and the PXP state. */
	struct i915_gt_verify_wa verify;
	struct i915_gt_migrate migrate;
	struct i915_pxp pxp;

	/* The hotplug state of the hotplug interrupt tests. */
	struct i915_hotplug hotplug;

	/* The batch and state buffers and the test images of the fixture tests. */
	uint32_t batch_a[I915_KTEST_BATCH_DWORDS];
	uint32_t batch_b[I915_KTEST_BATCH_DWORDS];
	uint32_t state_a[I915_KTEST_BATCH_DWORDS];
	uint32_t state_b[I915_KTEST_BATCH_DWORDS];
	uint8_t pattern[I915_TEX_FIXTURE_VARIANTS][I915_TEX_FIXTURE_TEX_BYTES];

	/* The address space the page-table walk runs on. */
	struct i915_gt_ppgtt walk_pp;
};

static void i915_fake_open(struct i915_mmio *mmio, struct i915_fake_mmio *fake);
static uint32_t i915_fake_gen_get(struct i915_fake_mmio *fake, uint32_t offset);
static void i915_fake_gen_set(struct i915_fake_mmio *fake, uint32_t offset, uint32_t value);
static int i915_fake_wt_find(const struct i915_fake_mmio *fake, uint32_t offset, uint32_t value, uint32_t mask);
static uint32_t i915_fake_read32(void *context, uint32_t offset);
static void i915_fake_write32(void *context, uint32_t offset, uint32_t value);
static void i915_fake_forcewake_request(void *context, int domain, int wake);
static int i915_fake_forcewake_ack(void *context, int domain);

static void i915_ktest_gt_fuses_open(struct i915_ktest_gt_state *t, uint32_t dss_enable);
static void i915_ktest_gt_sseu(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_clock(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_engine_fuses(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_fault(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_irq(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_irq_classes(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);

static void i915_ktest_gt_workarounds(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_wa_gt_list(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_wa_render_list(struct i915_ktest *ktest, struct i915_ktest_gt_state *t, unsigned rcs);
static void i915_ktest_gt_wa_context_lists(struct i915_ktest *ktest, struct i915_ktest_gt_state *t, unsigned rcs, unsigned bcs);
static void i915_ktest_gt_wa_whitelist(struct i915_ktest *ktest, struct i915_ktest_gt_state *t, unsigned rcs);
static void i915_ktest_gt_mocs(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_pat(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_wa_apply(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_rc6(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_rps(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);

static void i915_ktest_gt_forcewake_map(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static unsigned i915_ktest_gt_domain_mismatches(const struct i915_mmio *mmio, const struct i915_ktest_domain_expect *expect, unsigned count);

static void i915_ktest_gt_memory(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_encode(struct i915_ktest *ktest);
static int i915_ktest_gt_window(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_objects(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_kernel_ppgtt(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_scratch(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);

static int i915_ktest_gt_execlists_setup(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_engine_descriptor(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_execlists_enable(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);

static void i915_ktest_gt_contexts(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_context_state(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_context_ring(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_context_wa_batch(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_context_xcs(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_context_rpcs(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_render_request(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_copy_engine(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_copy_request(struct i915_ktest *ktest, struct i915_ktest_gt_state *t, uint32_t hw);
static void i915_ktest_gt_copy_submit(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_copy_csb(struct i915_ktest *ktest, struct i915_ktest_gt_state *t, uint32_t hw);
static void i915_ktest_gt_resume_pieces(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_power_sanitize(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);

static void i915_ktest_gt_defaults_gt(struct i915_ktest_gt_state *t);
static void i915_ktest_gt_defaults(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_defaults_inherit(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_verify_wa(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_verify_wa_steps(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_verify_wa_park(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_verify_wa_whole(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_srm_find(const uint32_t *ring, unsigned ring_dwords, const struct i915_wa_list *wal, uint32_t scratch_ggtt, int *found);
static void i915_ktest_gt_migrate(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_migrate_context(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_migrate_tree(const struct i915_gt_migrate *mg, int *tree_ok, int *pts_ok, int *window_ok);
static void i915_ktest_gt_migrate_tables(const struct i915_gt_migrate *mg, int *tree_ok, int *window_ok);

static void i915_ktest_gt_hotplug(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_hotplug_setup(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_dc6(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_pxp(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);

static void i915_ktest_gt_eu_batch(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_eu_pipeline_select(struct i915_ktest *ktest, struct i915_ktest_gt_state *t, unsigned count);
static void i915_ktest_gt_draw_batch(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_draw_state(struct i915_ktest *ktest, struct i915_ktest_gt_state *t, uint32_t mocs);
static void i915_ktest_gt_pins(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static uint64_t i915_ktest_gt_fnv(const void *bytes, unsigned length);
static void i915_ktest_gt_tex_batch(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_tex_state(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_tex_expect(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_tex_ab(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_tex_variants(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_bilinear(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_bilinear_expect(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_eu_pt(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);
static void i915_ktest_gt_eu_pt_unmapped(struct i915_ktest *ktest, struct i915_ktest_gt_state *t);

/*
 * The bus end of the register model.
 */
static const struct i915_mmio_ops i915_fake_mmio_ops = {
	i915_fake_read32,
	i915_fake_write32,
	i915_fake_forcewake_request,
	i915_fake_forcewake_ack
};

/*
 * What the test DMA device promises: 39-bit addresses (the GPU's dma mask),
 * segments up to 4 GiB, no boundary, coherent.
 */
static const struct drv_dma_constraints i915_ktest_dma_constraints = {
	39U,
	0xffffffffU,
	0U,
	1
};

/*
 * The registers of the render engine window the forcewake map splits
 * between the render and GT domains, as __gen12_fw_ranges has it.
 */
static const struct i915_ktest_domain_expect i915_ktest_fw_render_window[] = {
	{ 0x2000U, I915_FORCEWAKE_RENDER },
	{ 0x229cU, I915_FORCEWAKE_RENDER },
	{ 0x2700U, I915_FORCEWAKE_GT },
	{ 0x2b00U, I915_FORCEWAKE_GT },
	{ 0x2800U, I915_FORCEWAKE_RENDER }
};

/*
 * The registers of the copy and media engines: each media engine has its
 * own domain.
 */
static const struct i915_ktest_domain_expect i915_ktest_fw_media[] = {
	{ 0x22000U, I915_FORCEWAKE_GT },
	{ 0x1c0000U, I915_FORCEWAKE_MEDIA_VDBOX0 },
	{ 0x1c3f10U, I915_FORCEWAKE_MEDIA_VDBOX0 },
	{ 0x1d0000U, I915_FORCEWAKE_MEDIA_VDBOX2 },
	{ 0x1d3f10U, I915_FORCEWAKE_MEDIA_VDBOX2 },
	{ 0x1c8000U, I915_FORCEWAKE_MEDIA_VEBOX0 }
};

/*
 * Registers in a reserved or always-on range (0x1c4000..0x1c7fff is not
 * VDBOX1).
 */
static const struct i915_ktest_domain_expect i915_ktest_fw_always_on[] = {
	{ 0x1c4000U, -1 },
	{ 0x1008U, -1 }
};

/*
 * The ranges a hand-made table once got wrong or lacked: the split of
 * 0x9xxx, and the display and PCODE registers, which are always on.
 */
static const struct i915_ktest_domain_expect i915_ktest_fw_generated[] = {
	{ 0x8000U, I915_FORCEWAKE_GT },
	{ 0x9550U, I915_FORCEWAKE_RENDER },
	{ 0x9560U, -1 },
	{ 0xa2a0U, I915_FORCEWAKE_GT },
	{ 0x3000U, I915_FORCEWAKE_RENDER },
	{ 0x4208U, I915_FORCEWAKE_GT },
	{ 0x40000U, -1 },
	{ 0x138124U, -1 }
};

/*
 * Everything the GT tests build; see its type.
 */
static struct i915_ktest_gt_state i915_ktest_gt_state;

/*
 * Runs the GT tests.
 *
 * Every part builds what it needs on the register model and the test
 * memory of this file; the started device is left alone.
 */
void
drv_i915_ktest_gt(
	struct i915_ktest *ktest)
{
	struct i915_ktest_gt_state *t;

	t = &i915_ktest_gt_state;

	/* The GT information step: the fuses, the timestamp clock, the engine table and the pending faults. */
	i915_ktest_gt_sseu(ktest, t);
	i915_ktest_gt_clock(ktest, t);
	i915_ktest_gt_engine_fuses(ktest, t);
	i915_ktest_gt_fault(ktest, t);

	/* The GT half of the interrupt handler. */
	i915_ktest_gt_irq(ktest, t);

	/* The workaround lists, MOCS, PAT, RC6 and RPS. */
	i915_ktest_gt_workarounds(ktest, t);

	/* The forcewake domain map. */
	i915_ktest_gt_forcewake_map(ktest, t);

	/* The GT memory and everything built on it. */
	i915_ktest_gt_memory(ktest, t);
}

/* Binds the register block to a cleared model with no domain map and no trace. */
static void
i915_fake_open(
	struct i915_mmio *mmio,
	struct i915_fake_mmio *fake)
{
	/* Clears the model and binds the block to it. */
	kern_memset(fake, 0, sizeof(*fake));
	drv_i915_mmio_init(mmio, &i915_fake_mmio_ops, fake, NULL, 0U, NULL);
}

/* Reads a register the model has no special meaning for; zero when never written. */
static uint32_t
i915_fake_gen_get(
	struct i915_fake_mmio *fake,
	uint32_t offset)
{
	unsigned index;

	/* Looks the offset up among the written registers. */
	for (index = 0U; index < fake->gen_n; index++) {
		if (fake->gen_off[index] == offset)
			return fake->gen_val[index];
	}

	/* A register never written reads zero. */
	return 0U;
}

/* Stores a register the model has no special meaning for. */
static void
i915_fake_gen_set(
	struct i915_fake_mmio *fake,
	uint32_t offset,
	uint32_t value)
{
	unsigned index;

	/* Replaces the value of a register written before. */
	for (index = 0U; index < fake->gen_n; index++) {
		if (fake->gen_off[index] == offset) {
			fake->gen_val[index] = value;
			return;
		}
	}

	/* Remembers a new register while there is room; later ones are dropped. */
	if (fake->gen_n < I915_KTEST_FAKE_REGS) {
		fake->gen_off[fake->gen_n] = offset;
		fake->gen_val[fake->gen_n] = value;
		fake->gen_n++;
	}
}

/* Reports the trace position of the first write to a register whose value matches under a mask; -1 when none. */
static int
i915_fake_wt_find(
	const struct i915_fake_mmio *fake,
	uint32_t offset,
	uint32_t value,
	uint32_t mask)
{
	unsigned index;

	/* Walks the trace in write order. */
	for (index = 0U; index < fake->wt_n; index++) {
		/* Another register was written here. */
		if (fake->wt_off[index] != offset)
			continue;

		/* The register was written with the value asked for. */
		if ((fake->wt_val[index] & mask) == (value & mask))
			return (int)index;
	}

	/* No write matched. */
	return -1;
}

/* Reads one register of the model. */
static uint32_t
i915_fake_read32(
	void *context,
	uint32_t offset)
{
	struct i915_fake_mmio *fake;

	fake = context;

	/* Answers each register the model gives a meaning to. */
	switch (offset) {
	case I915_KTEST_REG_GDRST:
		/* A reset reads back as pending until it was requested once, and as done afterwards. */
		if (fake->gdrst_writes == 0U)
			return 0x1U;

		return 0x0U;
	case I915_KTEST_REG_PCODE_MAILBOX:
		return fake->mailbox;
	case I915_KTEST_REG_PCODE_DATA:
		return fake->data;
	case I915_KTEST_REG_PCODE_DATA1:
		return fake->data1;
	default:
		break;
	}

	/* Every other register reads what was last written to it. */
	return i915_fake_gen_get(fake, offset);
}

/* Writes one register of the model and appends the write to the trace. */
static void
i915_fake_write32(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct i915_fake_mmio *fake;

	fake = context;

	/* Appends the write to the trace while it has room. */
	if (fake->wt_n < I915_KTEST_FAKE_WRITES) {
		fake->wt_off[fake->wt_n] = offset;
		fake->wt_val[fake->wt_n] = value;
		fake->wt_n++;
	}

	/* Applies each register the model gives a meaning to. */
	switch (offset) {
	case I915_KTEST_REG_GDRST:
		fake->gdrst_writes++;
		return;
	case I915_KTEST_REG_PCODE_DATA:
		fake->data = value;
		return;
	case I915_KTEST_REG_PCODE_DATA1:
		fake->data1 = value;
		return;
	case I915_KTEST_REG_PCODE_MAILBOX:
		/* A posted command completes at once: READY clears and the status byte reads 0. */
		fake->mailbox = 0U;
		return;
	default:
		break;
	}

	/* Every other register keeps what was written. */
	i915_fake_gen_set(fake, offset, value);
}

/* Accepts a forcewake request; the model has no sleeping domain. */
static void
i915_fake_forcewake_request(
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
i915_fake_forcewake_ack(
	void *context,
	int domain)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(domain);

	/* Every domain is awake. */
	return 1;
}

/* Opens a clean model with one slice, the given DSS, every EU and every media engine enabled. */
static void
i915_ktest_gt_fuses_open(
	struct i915_ktest_gt_state *t,
	uint32_t dss_enable)
{
	/* Starts from a clean model. */
	i915_fake_open(&t->mmio, &t->fake);

	/* One slice, the DSS asked for, nothing fused off. */
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_SLICE_ENABLE, 0x1U);
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_DSS_ENABLE, dss_enable);
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_EU_DISABLE, 0x0U);

	/*
	 * GEN11_GT_VEBOX_VDBOX_DISABLE has disable semantics and the
	 * reference inverts it: 0 means nothing is disabled.
	 */
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_MEDIA_FUSE, 0x0U);
}

/* Checks the EU_DISABLE pair expansion of the SSEU decode. */
static void
i915_ktest_gt_sseu(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const struct i915_sseu *sseu;

	sseu = &t->info.sseu;

	/* Decodes a part with six DSS and nothing fused off. */
	i915_ktest_gt_fuses_open(t, 0x3fU);
	kern_memset(&t->info, 0, sizeof(t->info));
	(void)drv_i915_gt_init_mmio(&t->info, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/* Gen12 has one slice, six DSS and sixteen EUs per DSS. */
	drv_i915_ktest_check(
		ktest,
		sseu->max_slices == 1U &&
		    sseu->max_subslices == 6U &&
		    sseu->max_eus_per_subslice == 16U &&
		    sseu->slice_mask == 0x1U &&
		    sseu->subslice_mask == 0x3fU &&
		    sseu->eu_mask[0] == 0xffffU &&
		    sseu->eu_per_subslice == 16U &&
		    sseu->eu_total == 96U &&
		    sseu->has_slice_pg == 1,
		"p60: P60-SSEU gen12 = 1 slice, 6 DSS, 16 EU/DSS, 96 EUs when nothing is fused off");

	/*
	 * EU_DISABLE has one bit per pair: disabling bit 0 must remove two
	 * EUs, not one.  A naive one-bit-per-EU decode passes the check above
	 * and fails this one.  Pairs 0 and 1 are disabled, and only DSS 0 and
	 * 1 are present.
	 */
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_EU_DISABLE, 0x03U);
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_DSS_ENABLE, 0x03U);
	kern_memset(&t->info, 0, sizeof(t->info));
	(void)drv_i915_gt_init_mmio(&t->info, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/* EUs 0 to 3 are gone, and the absent DSS stay empty. */
	drv_i915_ktest_check(
		ktest,
		sseu->subslice_mask == 0x3U &&
		    sseu->eu_mask[0] == 0xfff0U &&
		    sseu->eu_per_subslice == 12U &&
		    sseu->eu_total == 24U &&
		    sseu->eu_mask[2] == 0U,
		"p60: P60-SSEU EU_DISABLE is one bit PER PAIR (2 EUs per set bit)");
}

/* Checks that CTC_MODE and RPM_CONFIG0 give the timestamp clock. */
static void
i915_ktest_gt_clock(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	/*
	 * Selects the crystal: 38.4 MHz (2 at shift 3) with a timestamp shift
	 * parameter of 3, which divides by nothing.
	 */
	i915_ktest_gt_fuses_open(t, 0x3fU);
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_CTC_MODE, 0U);
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_RPM_CONFIG0, (2U << 3) | (3U << 1));
	kern_memset(&t->info, 0, sizeof(t->info));
	(void)drv_i915_gt_init_mmio(&t->info, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/* The crystal frequency passes unchanged, with its period in whole nanoseconds. */
	drv_i915_ktest_check(
		ktest,
		t->info.clock_frequency == 38400000U && t->info.clock_period_ns == 26U,
		"p60: P60-CLOCK crystal 38.4MHz, ctc shift 3 -> no divide");

	/* A shift parameter of 1 divides the timestamp clock by four. */
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_RPM_CONFIG0, (2U << 3) | (1U << 1));
	kern_memset(&t->info, 0, sizeof(t->info));
	(void)drv_i915_gt_init_mmio(&t->info, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/* The frequency is the crystal shifted down by 3 - 1. */
	drv_i915_ktest_check(
		ktest,
		t->info.clock_frequency == (38400000U >> 2),
		"p60: P60-CLOCK the ctc shift parameter divides the timestamp clock");
}

/* Checks the engine table and the media fuses of the GT information step. */
static void
i915_ktest_gt_engine_fuses(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const struct i915_engine_info *engines;
	int error;

	engines = t->info.engines;

	/* Reads a part with no L3 bank and no media engine fused off. */
	i915_ktest_gt_fuses_open(t, 0x3fU);
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_MIRROR_FUSE3, 0U);
	kern_memset(&t->info, 0, sizeof(t->info));
	error = drv_i915_gt_init_mmio(&t->info, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/* RCS0(0), BCS0(1), VCS0(8), VCS2(10), VECS0(16) with the reference bases; VCS2 is BSD3, not BSD2. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    t->info.num_engines == 5U &&
		    t->info.engine_mask == 0x10503U &&
		    engines[0].mmio_base == 0x02000U &&
		    engines[1].mmio_base == 0x22000U &&
		    engines[2].mmio_base == 0x1c0000U &&
		    engines[3].mmio_base == 0x1d0000U &&
		    engines[4].mmio_base == 0x1c8000U &&
		    engines[0].context_size == 14U * 4096U &&
		    engines[1].context_size == 2U * 4096U &&
		    t->info.l3bank_mask == 0xfU,
		"p60: P60-ENGINES ADL-P builds rcs0/bcs0/vcs0/vcs2/vecs0 with the reference bases");

	/* Fuses off video decode instance 2. */
	drv_i915_raw_write32(&t->mmio, I915_KTEST_REG_MEDIA_FUSE, 1U << 2);
	kern_memset(&t->info, 0, sizeof(t->info));
	(void)drv_i915_gt_init_mmio(&t->info, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/* That engine, and only that one, is dropped. */
	drv_i915_ktest_check(
		ktest,
		t->info.num_engines == 4U &&
		    (t->info.engine_mask & (1U << 10)) == 0U &&
		    (t->info.engine_mask & (1U << 8)) != 0U,
		"p60: P60-ENGINES a fused-off VDBOX removes exactly that engine");
}

/* Checks that a pending ring fault is reported and only its valid bit cleared. */
static void
i915_ktest_gt_fault(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	int cleared;

	/* Leaves a valid fault of engine 3 pending in RING_FAULT_REG. */
	i915_ktest_gt_fuses_open(t, 0x3fU);
	drv_i915_raw_write32(&t->mmio, 0xcec4U, 0x1U | (3U << 12));
	kern_memset(&t->info, 0, sizeof(t->info));
	t->fake.wt_n = 0U;
	(void)drv_i915_gt_init_mmio(&t->info, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/*
	 * intel_gt_clear_error_registers() clears RING_FAULT_VALID with a
	 * read-modify-write: the other fields (engine id, address bits) are
	 * preserved, so the write is 0x3000, not 0.
	 */
	cleared = i915_fake_wt_find(&t->fake, 0xcec4U, 3U << 12, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		t->info.fault_valid_seen == 1 && cleared >= 0,
		"p60: P60-FAULT a valid ring fault is decoded and only VALID is cleared");
}

/* Checks the identity handshake and the engine decode of the GT interrupt handler. */
static void
i915_ktest_gt_irq(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	int selector;
	int identity_ack;
	int bank_clear;

	/* Builds the GT the handler maps identities back to. */
	i915_ktest_gt_fuses_open(t, 0x3fU);
	kern_memset(&t->info, 0, sizeof(t->info));
	(void)drv_i915_gt_init_mmio(&t->info, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/* Binds a fresh interrupt device to the model and the GT. */
	kern_memset(&t->irq, 0, sizeof(t->irq));
	t->irq.m = &t->mmio;
	t->irq.gt = &t->info;

	/*
	 * Asserts bank 0 bit 0 (GEN11_GT_INTR_DW(0)) with an identity of RCS0
	 * (class 0, instance 0) that reports USER and CONTEXT_SWITCH.
	 */
	drv_i915_raw_write32(&t->mmio, 0x190018U, 0x1U);
	drv_i915_raw_write32(&t->mmio, 0x190060U, (1U << 31) | (0U << 16) | (0U << 20) | 0x101U);
	t->fake.wt_n = 0U;
	drv_i915_gen11_gt_irq_handler(&t->irq, 0x1U);

	/* One identity was read and decoded as RCS0 with USER and CONTEXT_SWITCH. */
	drv_i915_ktest_check(
		ktest,
		t->irq.gt_identity_reads == 1U &&
		    t->irq.gt_identity_invalid == 0U &&
		    t->irq.gt_engine_intrs == 1U &&
		    t->irq.gt_user_intr == 1U &&
		    t->irq.gt_ctx_switch_intr == 1U &&
		    t->irq.gt_error_intr == 0U &&
		    t->irq.gt_bank_acks[0] == 1U &&
		    t->irq.gt_unknown_class == 0U,
		"p60: P60-GTIRQ identity -> RCS0, USER + CTX_SWITCH decoded");

	/* The selector is written first, the identity acknowledged next, and the bank cleared last. */
	selector = i915_fake_wt_find(&t->fake, 0x190070U, 0x1U, 0xffffffffU);
	identity_ack = i915_fake_wt_find(&t->fake, 0x190060U, 1U << 31, 1U << 31);
	bank_clear = i915_fake_wt_find(&t->fake, 0x190018U, 0x1U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		selector == 0 &&
		    identity_ack > 0 &&
		    bank_clear > 1,
		"p60: P60-GTIRQ selector -> identity ack -> INTR_DW clear, in that order");

	/* The identities that never validate and those of other classes. */
	i915_ktest_gt_irq_classes(ktest, t);
}

/* Checks an identity that never validates and one of OTHER_CLASS. */
static void
i915_ktest_gt_irq_classes(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	/* Asserts the bank again with an identity whose DATA_VALID never sets. */
	drv_i915_raw_write32(&t->mmio, 0x190018U, 0x1U);
	drv_i915_raw_write32(&t->mmio, 0x190060U, 0U);
	t->irq.gt_engine_intrs = 0U;
	t->irq.gt_identity_invalid = 0U;
	drv_i915_gen11_gt_irq_handler(&t->irq, 0x1U);

	/* The identity is counted as invalid and not treated as an engine. */
	drv_i915_ktest_check(
		ktest,
		t->irq.gt_identity_invalid == 1U && t->irq.gt_engine_intrs == 0U,
		"p60: P60-GTIRQ an identity that never reports DATA_VALID is counted, not decoded");

	/* Asserts bit 1 with an identity of OTHER_CLASS instance 1 (GTPM). */
	drv_i915_raw_write32(&t->mmio, 0x190018U, 0x2U);
	drv_i915_raw_write32(&t->mmio, 0x190060U, (1U << 31) | (4U << 16) | (1U << 20) | 0x4U);
	t->irq.gt_other_intrs = 0U;
	t->irq.gt_unknown_class = 0U;
	drv_i915_gen11_gt_irq_handler(&t->irq, 0x1U);

	/* OTHER_CLASS (GuC/GTPM) is counted on its own, not as an unknown class. */
	drv_i915_ktest_check(
		ktest,
		t->irq.gt_other_intrs == 1U && t->irq.gt_unknown_class == 0U,
		"p60: P60-GTIRQ OTHER_CLASS is accounted separately from unknown classes");
}

/* Checks the ADL-P workaround manifest, MOCS, PAT, RC6 and RPS. */
static void
i915_ktest_gt_workarounds(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	unsigned index;
	int rcs;
	int bcs;

	/* Builds a GT whose lowest live DSS is 1 (DSS 1 to 5 present). */
	i915_ktest_gt_fuses_open(t, 0x3eU);
	kern_memset(&t->wa_gt, 0, sizeof(t->wa_gt));
	(void)drv_i915_gt_init_mmio(&t->wa_gt, 12, I915_KTEST_ADLP_ENGINES, &t->mmio);

	/* Finds the render and copy engines. */
	rcs = -1;
	bcs = -1;
	for (index = 0U; index < t->wa_gt.num_engines; index++) {
		/* The render engine. */
		if (t->wa_gt.engines[index].class == I915_RENDER_CLASS)
			rcs = (int)index;

		/* The copy engine. */
		if (t->wa_gt.engines[index].class == I915_COPY_ENGINE_CLASS)
			bcs = (int)index;
	}

	/* The GT list of gen12_gt_workarounds_init(). */
	kern_memset(&t->wa_init, 0, sizeof(t->wa_init));
	i915_ktest_gt_wa_gt_list(ktest, t);

	/* The engine lists need the render engine. */
	drv_i915_ktest_check(ktest, rcs >= 0, "p6a: P6A-RCSWA render engine present");
	if (rcs < 0)
		return;

	i915_ktest_gt_wa_render_list(ktest, t, (unsigned)rcs);

	/* The copy engine's lists need the copy engine. */
	drv_i915_ktest_check(ktest, bcs >= 0, "p6a: P6A-RCSWA copy engine present");
	if (bcs < 0)
		return;

	/* The context lists and the whitelist. */
	i915_ktest_gt_wa_context_lists(ktest, t, (unsigned)rcs, (unsigned)bcs);
	i915_ktest_gt_wa_whitelist(ktest, t, (unsigned)rcs);

	/* The MOCS table, the private PAT and the apply pass. */
	i915_ktest_gt_mocs(ktest, t);
	i915_ktest_gt_pat(ktest, t);
	i915_ktest_gt_wa_apply(ktest, t);

	/* RC6 and RPS. */
	i915_ktest_gt_rc6(ktest, t);
	i915_ktest_gt_rps(ktest, t);
}

/* Checks the ADL-P GT workaround list. */
static void
i915_ktest_gt_wa_gt_list(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const struct i915_wa *w;
	unsigned index;
	unsigned found;
	int mcr_ok;
	int dfr_ok;
	int misc_ok;
	int vd0;
	int vd2;

	/* Builds the list. */
	drv_i915_gt_init_workarounds_adlp(&t->wa_init.gt_wa, &t->wa_gt);

	/* Looks for each expected entry. */
	found = 0U;
	mcr_ok = 0;
	dfr_ok = 0;
	misc_ok = 0;
	vd0 = 0;
	vd2 = 0;
	for (index = 0U; index < t->wa_init.gt_wa.count; index++) {
		w = &t->wa_init.gt_wa.list[index];
		found++;

		/* icl_wa_init_mcr() steers at the lowest live subslice (1). */
		if (w->reg == 0x0fdcU &&
		    w->clr == 0x7f000000U &&
		    w->set == (1U << 24) &&
		    w->is_mcr == 0)
			mcr_ok = 1;

		/* Wa_14011059788 is an MCR wa_write_or: clear equals set. */
		if (w->reg == 0x9550U &&
		    w->is_mcr == 1 &&
		    w->clr == 0x200U &&
		    w->set == 0x200U &&
		    w->read_mask == 0x200U)
			dfr_ok = 1;

		/*
		 * Wa_14015795083 clears the bit (the reference's argument
		 * order is register, clear, set) and is not verified.
		 */
		if (w->reg == 0x9424U &&
		    w->clr == 0x2U &&
		    w->set == 0U &&
		    w->read_mask == 0U &&
		    w->kind == I915_WA_NO_VERIFY)
			misc_ok = 1;

		/* Wa_14011060649 on the first even video decode instance. */
		if (w->reg == 0x1c3f10U && w->set == (1U << 22))
			vd0 = 1;

		/* Wa_14011060649 on the second even video decode instance. */
		if (w->reg == 0x1d3f10U && w->set == (1U << 22))
			vd2 = 1;
	}

	/* The list holds exactly those five entries. */
	drv_i915_ktest_check(
		ktest,
		found == 5U &&
		    mcr_ok != 0 &&
		    dfr_ok != 0 &&
		    misc_ok != 0 &&
		    vd0 != 0 &&
		    vd2 != 0,
		"p6a: P6A-GTWA MCR steer + Wa_14011060649 x2 + 14011059788 + 14015795083");
}

/* Checks the ADL-P render engine workaround list and a non-render engine's list. */
static void
i915_ktest_gt_wa_render_list(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t,
	unsigned rcs)
{
	const struct i915_wa *w;
	unsigned index;
	int cctl;
	int dop;
	int row2;
	int ffthread;
	int smallpl;
	int row4;
	int psmi;
	int perctx;

	/* Builds the render list with MOCS uncached index 3. */
	drv_i915_engine_init_workarounds(&t->wa_init.engine_wa[0], &t->wa_gt.engines[rcs], 12, 3U);

	/* Looks for each expected entry. */
	cctl = 0;
	dop = 0;
	row2 = 0;
	ffthread = 0;
	smallpl = 0;
	row4 = 0;
	psmi = 0;
	perctx = 0;
	for (index = 0U; index < t->wa_init.engine_wa[0].count; index++) {
		w = &t->wa_init.engine_wa[0].list[index];

		/* RING_CMD_CCTL's MOCS override for uncached index 3 is 0x306. */
		if (w->reg == 0x20c4U && w->set == ((0x3fffU << 16) | 0x306U))
			cctl = 1;

		/* GEN8_ROW_CHICKEN's DOP clock gating. */
		if (w->reg == 0x20ecU && w->set == 0x00020002U)
			dop = 1;

		/*
		 * Wa_1606931601 and Wa_1409804808 target the same MCR register
		 * and must merge into one entry.
		 */
		if (w->reg == 0xe4f4U &&
		    w->is_mcr == 1 &&
		    w->set == 0x41004100U &&
		    w->read_mask == 0x4100U)
			row2 = 1;

		/* The FF thread mode entry. */
		if (w->reg == 0x20a0U &&
		    w->clr == 0x00080000U &&
		    w->set == 0x00080000U)
			ffthread = 1;

		/* The small pixel-load entry. */
		if (w->reg == 0xe18cU &&
		    w->is_mcr == 1 &&
		    w->set == 0x80008000U)
			smallpl = 1;

		/* ROW_CHICKEN4. */
		if (w->reg == 0xe48cU &&
		    w->is_mcr == 1 &&
		    w->set == 0x02000200U)
			row4 = 1;

		/* The PSMI control entry. */
		if (w->reg == 0x2050U && w->set == 0x10801080U)
			psmi = 1;

		/* The per-context control entry. */
		if (w->reg == 0x20e0U && w->set == 0x40004000U)
			perctx = 1;
	}

	/* The list holds all eight entries, ROW_CHICKEN2 merged. */
	drv_i915_ktest_check(
		ktest,
		cctl != 0 &&
		    dop != 0 &&
		    row2 != 0 &&
		    ffthread != 0 &&
		    smallpl != 0 &&
		    row4 != 0 &&
		    psmi != 0 &&
		    perctx != 0 &&
		    t->wa_init.engine_wa[0].count == 8U,
		"p6a: P6A-RCSWA all eight ADL-P RCS entries, ROW_CHICKEN2 merged");
}

/* Checks the non-render engine list, the context lists of both engines. */
static void
i915_ktest_gt_wa_context_lists(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t,
	unsigned rcs,
	unsigned bcs)
{
	const struct i915_wa *w;
	const struct i915_wa_list *copy_ctx;
	unsigned index;
	int cps;
	int preempt;
	int ffmode2;
	int hiz;
	int tdc;

	/* A non-render engine only gets the fake CMD_CCTL entry. */
	drv_i915_engine_init_workarounds(&t->wa_init.engine_wa[1], &t->wa_gt.engines[bcs], 12, 3U);
	drv_i915_ktest_check(
		ktest,
		t->wa_init.engine_wa[1].count == 1U && t->wa_init.engine_wa[1].list[0].reg == 0x220c4U,
		"p6a: P6A-RCSWA a non-render engine only gets the CMD_CCTL fake WA");

	/* Builds the render engine's context list. */
	drv_i915_engine_init_ctx_wa(&t->wa_init.ctx_wa[0], &t->wa_gt.engines[rcs], 12, 3U);

	/* Looks for each expected entry. */
	cps = 0;
	preempt = 0;
	ffmode2 = 0;
	hiz = 0;
	tdc = 0;
	for (index = 0U; index < t->wa_init.ctx_wa[0].count; index++) {
		w = &t->wa_init.ctx_wa[0].list[index];

		/* The CPS chicken entry. */
		if (w->reg == 0x7304U && w->set == 0x02000200U)
			cps = 1;

		/* The preemption control entry. */
		if (w->reg == 0x2580U && w->set == 0x00060002U)
			preempt = 1;

		/* FF_MODE2: clear ~0, set TDS_128|GS_224, not verified. */
		if (w->reg == 0x6604U &&
		    w->clr == 0xffffffffU &&
		    w->set == (0xe0000000U | 0x00040000U) &&
		    w->kind == I915_WA_NO_VERIFY)
			ffmode2 = 1;

		/* The HIZ chicken entry. */
		if (w->reg == 0x7018U && w->set == 0x20002000U)
			hiz = 1;

		/* The TDC chicken entry. */
		if (w->reg == 0x7300U && w->set == 0x00400040U)
			tdc = 1;
	}

	/* The render context list holds the five gen12 entries. */
	drv_i915_ktest_check(
		ktest,
		cps != 0 &&
		    preempt != 0 &&
		    ffmode2 != 0 &&
		    hiz != 0 &&
		    tdc != 0 &&
		    t->wa_init.ctx_wa[0].count == 5U,
		"p6a: P6A-CTXWA five gen12 context WAs; FF_MODE2 is write-only");

	/* The copy engine's context list is BLIT_CCTL, and only that. */
	drv_i915_engine_init_ctx_wa(&t->wa_init.ctx_wa[1], &t->wa_gt.engines[bcs], 12, 3U);
	copy_ctx = &t->wa_init.ctx_wa[1];
	drv_i915_ktest_check(
		ktest,
		copy_ctx->count == 1U &&
		    copy_ctx->list[0].reg == 0x22204U &&
		    copy_ctx->list[0].clr == 0x7f7fU &&
		    copy_ctx->list[0].set == 0x0606U,
		"p6a: P6A-CTXWA gen12_ctx_gt_mocs_init programs BLIT_CCTL on the copy engine only");
}

/* Checks the render engine's register whitelist and how it is applied. */
static void
i915_ktest_gt_wa_whitelist(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t,
	unsigned rcs)
{
	const struct i915_wa_list *whitelist;
	int slot0;
	int slot1;
	int slot3;
	int slot4;
	int slot11;

	whitelist = &t->wa_init.whitelist[0];

	/* Builds the whitelist. */
	drv_i915_engine_init_whitelist(&t->wa_init.whitelist[0], &t->wa_gt.engines[rcs], 12);

	/* PS_INVOCATION_COUNT (read-only, range of 4) and three read-write registers. */
	drv_i915_ktest_check(
		ktest,
		whitelist->count == 4U &&
		    whitelist->list[0].reg == (0x2348U | (1U << 28) | 1U) &&
		    whitelist->list[1].reg == 0x7010U &&
		    whitelist->list[2].reg == 0x7018U &&
		    whitelist->list[3].reg == 0x7304U,
		"p6a: P6A-WHITELIST PS_INVOCATION_COUNT (RD, range4) + three RW regs");

	/* Applies the whitelist to the engine's non-privileged slots. */
	t->fake.wt_n = 0U;
	drv_i915_engine_apply_whitelist(&t->wa_init.whitelist[0], &t->wa_gt.engines[rcs], &t->mmio);

	/* The slots carry address and flags; the remaining eight take RING_NOPID. */
	slot0 = i915_fake_wt_find(&t->fake, 0x24d0U, 0x10002349U, 0xffffffffU);
	slot1 = i915_fake_wt_find(&t->fake, 0x24d4U, 0x7010U, 0xffffffffU);
	slot3 = i915_fake_wt_find(&t->fake, 0x24dcU, 0x7304U, 0xffffffffU);
	slot4 = i915_fake_wt_find(&t->fake, 0x24e0U, 0x2094U, 0xffffffffU);
	slot11 = i915_fake_wt_find(&t->fake, 0x24fcU, 0x2094U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		slot0 >= 0 &&
		    slot1 >= 0 &&
		    slot3 >= 0 &&
		    slot4 >= 0 &&
		    slot11 >= 0,
		"p6a: P6A-WHITELIST slots carry addr|flags, unused slots take RING_NOPID");
}

/* Checks the gen12 MOCS table and its programming. */
static void
i915_ktest_gt_mocs(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const struct i915_mocs *mocs;
	unsigned global_writes;
	unsigned l3cc_writes;
	int entry3;
	int l3cc1;

	mocs = &t->wa_init.mocs;

	/* Builds the gen12 table (not tgl_mocs_table). */
	drv_i915_get_mocs_settings(&t->wa_init.mocs, 12);

	/* Uncached index 3; gaps take the unused entry. */
	drv_i915_ktest_check(
		ktest,
		mocs->valid == 1 &&
		    mocs->n_entries == 64U &&
		    mocs->uc_index == 3U &&
		    mocs->unused_entries_index == 2U &&
		    mocs->control[3] == (1U | (1U << 2)) &&
		    mocs->l3cc[3] == (1U << 4) &&
		    mocs->control[0] == mocs->control[2] &&
		    mocs->control[1] == mocs->control[2] &&
		    mocs->control[47] == mocs->control[2] &&
		    mocs->control[48] == (3U | (1U << 2) | (3U << 4)),
		"p6a: P6A-MOCS gen12 table, uc_index 3, gaps take the unused entry");

	/* Programs the table. */
	t->fake.wt_n = 0U;
	global_writes = 0U;
	l3cc_writes = 0U;
	drv_i915_mocs_init(&t->wa_init.mocs, &t->mmio, &global_writes, &l3cc_writes);

	/* 64 global entries and 32 packed L3CC pairs. */
	entry3 = i915_fake_wt_find(&t->fake, 0x4000U + 3U * 4U, 1U | (1U << 2), 0xffffffffU);
	l3cc1 = i915_fake_wt_find(&t->fake, 0xb020U + 1U * 4U, 0U, 0U);
	drv_i915_ktest_check(
		ktest,
		global_writes == 64U &&
		    l3cc_writes == 32U &&
		    entry3 >= 0 &&
		    l3cc1 >= 0,
		"p6a: P6A-MOCS 64 global entries + 32 packed L3CC pairs");
}

/* Checks the Tiger Lake private PAT. */
static void
i915_ktest_gt_pat(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	unsigned writes;
	int pat0;
	int pat1;
	int pat2;
	int pat3;
	int pat7;

	/* Programs the PAT. */
	t->fake.wt_n = 0U;
	writes = 0U;
	drv_i915_tgl_setup_private_ppat(&t->mmio, &writes);

	/* WB, WC, WT, UC, then WB to the end, in that order. */
	pat0 = i915_fake_wt_find(&t->fake, 0x4800U, 3U, 0xffffffffU);
	pat1 = i915_fake_wt_find(&t->fake, 0x4804U, 1U, 0xffffffffU);
	pat2 = i915_fake_wt_find(&t->fake, 0x4808U, 2U, 0xffffffffU);
	pat3 = i915_fake_wt_find(&t->fake, 0x480cU, 0U, 0xffffffffU);
	pat7 = i915_fake_wt_find(&t->fake, 0x481cU, 3U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		writes == 8U &&
		    pat0 == 0 &&
		    pat1 > 0 &&
		    pat2 > 0 &&
		    pat3 > 0 &&
		    pat7 > 0,
		"p6b: P6B-PAT tgl private PPAT = WB,WC,WT,UC,WB,WB,WB,WB");
}

/* Checks how the apply pass treats masked, plain and no-verify entries. */
static void
i915_ktest_gt_wa_apply(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_wa_apply_result result;
	struct i915_wa_list *list;
	int masked;
	int plain;
	int no_verify;

	list = &t->apply_list;

	/*
	 * The MOCS test consumed every slot of the model's register store (64
	 * global + 32 L3CC), after which writes are dropped and reads return 0.
	 * Starts clean.
	 */
	i915_fake_open(&t->mmio, &t->fake);

	/* Builds a masked, a plain and a no-verify entry over registers that read 0. */
	list->count = 0U;
	list->overflow = 0U;
	list->name = "t";
	drv_i915_wa_masked_en(list, 0x1000U, 0x4U, 0, "masked");
	drv_i915_wa_write_or(list, 0x1004U, 0x8U, 0, "plain");
	drv_i915_wa_add_no_verify(list, 0x1008U, 0xffffffffU, 0x55U, 0, "nv");
	drv_i915_raw_write32(&t->mmio, 0x1000U, 0U);
	drv_i915_raw_write32(&t->mmio, 0x1004U, 0U);
	drv_i915_raw_write32(&t->mmio, 0x1008U, 0U);

	/* Applies and verifies the list. */
	t->fake.wt_n = 0U;
	kern_memset(&result, 0, sizeof(result));
	drv_i915_wa_list_apply(list, &t->mmio, 1, &result);

	/* The masked entry writes its mask word; the no-verify one is written but not checked. */
	masked = i915_fake_wt_find(&t->fake, 0x1000U, 0x00040004U, 0xffffffffU);
	plain = i915_fake_wt_find(&t->fake, 0x1004U, 0x8U, 0xffffffffU);
	no_verify = i915_fake_wt_find(&t->fake, 0x1008U, 0x55U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		result.written == 3U &&
		    result.not_verifiable == 1U &&
		    result.verified == 2U &&
		    result.mismatched == 0U &&
		    masked >= 0 &&
		    plain >= 0 &&
		    no_verify >= 0,
		"p6b: P6B-APPLY masked writes the mask word; no-verify is written but not checked");

	/*
	 * Applies again: only the masked entry is rewritten.  A plain entry
	 * whose value is already right is skipped, and so is the no-verify one
	 * -- "no verify" changes only the readback check, not the apply, which
	 * is still the reference's `val != old || !wa->clr` rule.
	 */
	t->fake.wt_n = 0U;
	kern_memset(&result, 0, sizeof(result));
	drv_i915_wa_list_apply(list, &t->mmio, 0, &result);
	drv_i915_ktest_check(
		ktest,
		result.skipped_unchanged == 2U && result.written == 1U,
		"p6b: P6B-APPLY only the masked entry is rewritten when nothing changed");

	/* A register that refuses the write: the store is emptied so it reads back 0 whatever is written. */
	list->count = 0U;
	drv_i915_wa_write_or(list, 0x100cU, 0x2U, 0, "locked");
	drv_i915_raw_write32(&t->mmio, 0x100cU, 0U);
	t->fake.gen_n = 0U;
	kern_memset(&result, 0, sizeof(result));
	drv_i915_wa_list_apply(list, &t->mmio, 1, &result);

	/* The verify pass reports the stuck register. */
	drv_i915_ktest_check(
		ktest,
		result.mismatched + result.verified == 1U,
		"p6b: P6B-APPLY the verify pass reports, and does not hide, a stuck register");
}

/* Checks that the RC6 enable programs render and media power gating, then RC_CONTROL. */
static void
i915_ktest_gt_rc6(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	uint32_t pg_expected;
	int pg_write;
	int control_write;

	/* Enables RC6 on the workaround GT (VCS0 and VCS2 present). */
	kern_memset(&t->rc6, 0, sizeof(t->rc6));
	drv_i915_rc6_init(&t->rc6, &t->mmio);
	t->fake.wt_n = 0U;
	drv_i915_gen11_rc6_enable(&t->rc6, &t->mmio, &t->wa_gt);

	/* Render, media and sampler power gating, plus HCP and MFX for VCS0 and VCS2. */
	pg_expected = 0x7U | (1U << 3) | (1U << 4) | (1U << 7) | (1U << 8);
	pg_write = i915_fake_wt_find(&t->fake, 0xa210U, t->rc6.pg_enable, 0xffffffffU);
	control_write = i915_fake_wt_find(&t->fake, 0xa090U, 1U << 18, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		t->rc6.enabled == 1 &&
		    t->rc6.wa_disabled == 0 &&
		    t->rc6.ctl_enable == (1U << 18) &&
		    t->rc6.pg_enable == pg_expected &&
		    pg_write >= 0 &&
		    control_write >= 0,
		"p6b: P6B-RC6 render PG + per-VCS HCP/MFX, then RC_CONTROL");
}

/* Checks the RPS frequency caps and the enable. */
static void
i915_ktest_gt_rps(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct mutex sb_lock;
	int hysteresis;
	int request;

	/* Prepares the sideband lock the PCODE read of the efficient frequency takes. */
	(void)mutex_init(&sb_lock, LOCK_RANK_DEVICE, "ktest-gtwa");

	/*
	 * The frequency caps in 50 MHz units: RP_STATE_CAP holds RP0 (10) in
	 * [7:0] and the minimum (2) in [23:16]; the second cap register holds
	 * RP1 (6) in [15:8].
	 */
	drv_i915_raw_write32(&t->mmio, 0x140000U + 0x5998U, (0x0aU << 0) | (0x02U << 16));
	drv_i915_raw_write32(&t->mmio, 0x140000U + 0x5ef0U, 0x06U << 8);
	kern_memset(&t->rps, 0, sizeof(t->rps));
	drv_i915_rps_init(&t->rps, &sb_lock, &t->mmio);

	/* The caps are scaled from 50 MHz to 16.67 MHz units. */
	drv_i915_ktest_check(
		ktest,
		t->rps.rp0_freq == 30U &&
		    t->rps.min_freq == 6U &&
		    t->rps.rp1_freq == 18U &&
		    t->rps.max_freq == 30U,
		"p6b: P6B-RPS caps are scaled from 50MHz to 16.67MHz units (x3)");

	/* Enables RPS. */
	t->fake.wt_n = 0U;
	drv_i915_rps_enable(&t->rps, &t->mmio);

	/* RP_IDLE_HYSTERSIS is programmed, then RPNSWREQ asks for the minimum. */
	hysteresis = i915_fake_wt_find(&t->fake, 0xa070U, 0xaU, 0xffffffffU);
	request = i915_fake_wt_find(&t->fake, 0xa008U, 6U << 23, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		t->rps.enabled == 1 &&
		    hysteresis >= 0 &&
		    request >= 0,
		"p6b: P6B-RPS enable programs RP_IDLE_HYSTERSIS then RPNSWREQ=min");
}

/* Checks the Gen12 forcewake domain map the register access classifies with. */
static void
i915_ktest_gt_forcewake_map(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const struct i915_mmio_range *ranges;
	unsigned range_count;
	unsigned mismatches;

	/*
	 * The other tests bind the model with no range table (everything
	 * always on).  Here the real table is installed, because the point is
	 * the classification itself: a media engine register reached while
	 * only RENDER and GT are held reads as all-ones and its writes are
	 * dropped, silently.
	 */
	kern_memset(&t->fake, 0, sizeof(t->fake));
	ranges = drv_i915_mmio_gen12_ranges(&range_count);
	drv_i915_mmio_init(&t->mmio, &i915_fake_mmio_ops, &t->fake, ranges, range_count, NULL);

	/* The render engine window is split between RENDER and GT. */
	mismatches = i915_ktest_gt_domain_mismatches(&t->mmio,
						    i915_ktest_fw_render_window,
						    sizeof(i915_ktest_fw_render_window) / sizeof(i915_ktest_fw_render_window[0]));
	drv_i915_ktest_check(ktest, mismatches == 0U, "p6c1: P6C1-FWMAP the RCS window is split RENDER/GT as __gen12_fw_ranges has it");

	/* Each media engine has its own domain. */
	mismatches = i915_ktest_gt_domain_mismatches(&t->mmio,
						    i915_ktest_fw_media,
						    sizeof(i915_ktest_fw_media) / sizeof(i915_ktest_fw_media[0]));
	drv_i915_ktest_check(ktest, mismatches == 0U, "p6c1: P6C1-FWMAP each media engine maps to its own forcewake domain");

	/* A reserved or always-on range belongs to no domain. */
	mismatches = i915_ktest_gt_domain_mismatches(&t->mmio,
						    i915_ktest_fw_always_on,
						    sizeof(i915_ktest_fw_always_on) / sizeof(i915_ktest_fw_always_on[0]));
	drv_i915_ktest_check(ktest, mismatches == 0U, "p6c1: P6C1-FWMAP a reserved or always-on range is not claimed by a domain");

	/* The generated table splits 0x9xxx and leaves display and PCODE always on. */
	mismatches = i915_ktest_gt_domain_mismatches(&t->mmio,
						    i915_ktest_fw_generated,
						    sizeof(i915_ktest_fw_generated) / sizeof(i915_ktest_fw_generated[0]));
	drv_i915_ktest_check(ktest, mismatches == 0U, "p6c4: P6C4-FWGEN the generated table splits 0x9xxx and leaves display/PCODE always-on");
}

/* Counts the registers of a table the forcewake map puts in another domain than expected. */
static unsigned
i915_ktest_gt_domain_mismatches(
	const struct i915_mmio *mmio,
	const struct i915_ktest_domain_expect *expect,
	unsigned count)
{
	unsigned index;
	unsigned mismatches;
	int domain;

	/* Classifies every register of the table. */
	mismatches = 0U;
	for (index = 0U; index < count; index++) {
		domain = drv_i915_mmio_domain_of(mmio, expect[index].offset);
		if (domain != expect[index].domain)
			mismatches++;
	}

	/* Reports how many were misclassified. */
	return mismatches;
}

/* Runs the GT memory tests and everything built on the test memory. */
static void
i915_ktest_gt_memory(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	int error;

	/* The PPGTT entry encodings need no memory. */
	i915_ktest_gt_encode(ktest);

	/* Creates the DMA device the test objects come from. */
	t->dma = NULL;
	error = drv_dma_device_create(&i915_ktest_dma_constraints, &t->dma);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "p6c0: could not create a DMA device for the GT object tests");
		return;
	}

	/* Prepares the GT memory over the table in RAM; nothing else can run without it. */
	error = i915_ktest_gt_window(ktest, t);
	if (error != 0) {
		(void)drv_dma_device_destroy(t->dma);
		return;
	}

	/* The objects, their GGTT binding, the kernel PPGTT and the scratch page. */
	i915_ktest_gt_objects(ktest, t);
	i915_ktest_gt_kernel_ppgtt(ktest, t);
	i915_ktest_gt_scratch(ktest, t);

	/*
	 * The render engine for execlists submission, then its context image,
	 * its requests, the copy engine's submission and the resume pieces.
	 */
	error = i915_ktest_gt_execlists_setup(ktest, t);
	if (error == 0)
		i915_ktest_gt_contexts(ktest, t);

	drv_i915_engine_release(&t->rcs, &t->gm);

	/* The default-state recording, the workaround verification and the migrate context. */
	i915_ktest_gt_defaults_gt(t);
	i915_ktest_gt_defaults(ktest, t);
	i915_ktest_gt_verify_wa(ktest, t);
	i915_ktest_gt_migrate(ktest, t);

	/* The hotplug interrupt setup, the DC_off power well and the PXP context. */
	i915_ktest_gt_hotplug(ktest, t);
	i915_ktest_gt_dc6(ktest, t);
	i915_ktest_gt_pxp(ktest, t);

	/* The batches and states of the test fixtures. */
	i915_ktest_gt_eu_batch(ktest, t);
	i915_ktest_gt_draw_batch(ktest, t);
	i915_ktest_gt_pins(ktest, t);
	i915_ktest_gt_tex_batch(ktest, t);
	i915_ktest_gt_tex_state(ktest, t);
	i915_ktest_gt_tex_expect(ktest, t);
	i915_ktest_gt_tex_ab(ktest, t);
	i915_ktest_gt_tex_variants(ktest, t);
	i915_ktest_gt_bilinear(ktest, t);
	i915_ktest_gt_bilinear_expect(ktest, t);

	/* The page tables the GPU walks for the compute test's address. */
	i915_ktest_gt_eu_pt(ktest, t);

	/* Tears the kernel PPGTT and the GT memory down. */
	drv_i915_gt_ppgtt_destroy(&t->gm, &t->pp);
	drv_i915_gt_mem_fini(&t->gm);

	/* Every object and its GGTT run were released. */
	drv_i915_ktest_check(
		ktest,
		t->gm.objects_live == 0U &&
		    t->gm.allocated_pages == 0U &&
		    t->pp.inited == 0 &&
		    t->pp.top_pd == NULL,
		"p6c0: P6C0-OBJ teardown releases every object and its GGTT run");

	/* Gives the DMA device back. */
	(void)drv_dma_device_destroy(t->dma);
	t->dma = NULL;
}

/* Checks the PPGTT entry and directory encodings. */
static void
i915_ktest_gt_encode(
	struct i915_ktest *ktest)
{
	uint64_t pat3;
	uint64_t pat0;
	uint64_t pat4;
	uint64_t pde;

	/*
	 * TGL_CACHELEVEL puts I915_CACHE_NONE at PAT index 3, so the scratch
	 * PTE carries PAT0|PAT1; a PDE is always PPAT_UNCACHED (the same two
	 * bits by another name).  Index 4 must reach PAT2 (bit 7), not wrap
	 * into PAT0.
	 */
	pat3 = drv_i915_gen12_ppgtt_pte_encode(0x1000U, 3U);
	pat0 = drv_i915_gen12_ppgtt_pte_encode(0x1000U, 0U);
	pat4 = drv_i915_gen12_ppgtt_pte_encode(0x1000U, 4U);
	pde = drv_i915_gen8_pde_encode(0x2000U);
	drv_i915_ktest_check(
		ktest,
		pat3 == (0x1000U | 0x1bU) &&
		    pat0 == (0x1000U | 0x3U) &&
		    pat4 == (0x1000U | 0x83U) &&
		    pde == (0x2000U | 0x1bU),
		"p6c0: P6C0-ENCODE PAT index 3 sets PAT0|PAT1; a PDE is uncached");
}

/* Checks that the GT window is taken at the top of the GGTT, and prepares the test memory. */
static int
i915_ktest_gt_window(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	uint64_t dma_mask;
	int error;

	dma_mask = (((uint64_t)1) << 39) - 1U;

	/* A GGTT with no room beyond the window is refused. */
	error = drv_i915_gt_mem_init(&t->gm, t->dma, dma_mask, t->ggtt_table, I915_GT_GGTT_PAGES, 0xdeadU, NULL);
	drv_i915_ktest_check(ktest, error == ENOSPC, "p6c0: P6C0-WINDOW a GGTT with no room beyond the window is refused");

	/* The window of a larger GGTT sits at its top (PIN_HIGH). */
	error = drv_i915_gt_mem_init(&t->gm, t->dma, dma_mask, t->ggtt_table, I915_KTEST_GGTT_ENTRIES, 0xdeadU, NULL);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    t->gm.window_first == I915_KTEST_GGTT_ENTRIES - I915_GT_GGTT_PAGES &&
		    t->gm.window_pages == I915_GT_GGTT_PAGES,
		"p6c0: P6C0-WINDOW the driver window is taken at the top of the GGTT");

	/* Reports a memory that could not be prepared. */
	if (error != 0)
		return error;

	/* Succeeded: the test memory is ready. */
	return 0;
}

/* Checks object creation and the GGTT binding of objects. */
static void
i915_ktest_gt_objects(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_object *one;
	struct i915_gt_object *three;
	uint64_t dma0;
	unsigned index;
	int error;

	/* Creates a one-byte object and a three-page object. */
	one = drv_i915_gt_object_create(&t->gm, 1U);
	three = drv_i915_gt_object_create(&t->gm, 3U * I915_GT_PAGE_BYTES);

	/* Both are whole pages and start zeroed. */
	drv_i915_ktest_check(
		ktest,
		one != NULL &&
		    one->pages == 1U &&
		    one->bytes == I915_GT_PAGE_BYTES &&
		    three != NULL &&
		    three->pages == 3U &&
		    three->cpu != NULL &&
		    ((const unsigned char *)one->cpu)[0] == 0U &&
		    ((const unsigned char *)three->cpu)[3U * I915_GT_PAGE_BYTES - 1U] == 0U,
		"p6c0: P6C0-OBJ an object is rounded to whole pages and starts zeroed");

	/* The binding tests need both objects. */
	if (one == NULL || three == NULL)
		return;

	/* Binds the three-page object into an empty table. */
	for (index = 0U; index < I915_KTEST_GGTT_ENTRIES; index++)
		t->ggtt_table[index] = 0U;

	t->gm.pte_writes = 0U;
	t->gm.flushes = 0U;
	error = drv_i915_gt_ggtt_bind(&t->gm, three);
	dma0 = 0U;
	(void)drv_i915_gt_object_page_dma(three, 0U, &dma0);

	/* One PTE per page with the LM bit clear, and one flush. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    three->bound == 1 &&
		    t->gm.pte_writes == 3U &&
		    t->gm.flushes == 1U &&
		    three->ggtt_page == t->gm.window_first &&
		    three->ggtt_offset == (uint64_t)t->gm.window_first * I915_GT_PAGE_BYTES &&
		    t->ggtt_table[t->gm.window_first] == (dma0 | 1U),
		"p6c0: P6C0-BIND writes one GGTT PTE per page with the LM bit clear");

	/* The next binding takes the next free run. */
	error = drv_i915_gt_ggtt_bind(&t->gm, one);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    one->ggtt_page == t->gm.window_first + 3U &&
		    t->gm.allocated_pages == 4U,
		"p6c0: P6C0-BIND first fit hands out the next free run");

	/* Unbinding points the run back at scratch and frees it. */
	t->gm.pte_writes = 0U;
	drv_i915_gt_ggtt_unbind(&t->gm, three);
	drv_i915_ktest_check(
		ktest,
		three->bound == 0 &&
		    t->gm.pte_writes == 3U &&
		    t->gm.allocated_pages == 1U &&
		    t->ggtt_table[t->gm.window_first] == 0xdeadU,
		"p6c0: P6C0-BIND unbind points the run back at scratch and frees it");
}

/* Checks the kernel PPGTT's scratch tower and top directory (gen8_init_scratch(), gen8_alloc_top_pd()). */
static void
i915_ktest_gt_kernel_ppgtt(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const uint64_t *pt;
	const uint64_t *pd;
	const uint64_t *pdp;
	const uint64_t *pml4;
	const uint64_t *encode;
	int error;

	/* Creates the kernel PPGTT. */
	error = drv_i915_gt_ppgtt_create(&t->gm, &t->pp);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "p6c0: P6C0-PPGTT the kernel ppgtt could not be created");
		return;
	}

	/* Every level is a full page of the level below's encoding. */
	pt = t->pp.scratch[1]->cpu;
	pd = t->pp.scratch[2]->cpu;
	pdp = t->pp.scratch[3]->cpu;
	pml4 = t->pp.top_pd->cpu;
	encode = t->pp.scratch_encode;
	drv_i915_ktest_check(
		ktest,
		t->pp.inited == 1 &&
		    t->pp.top == 3 &&
		    t->pp.top_count == 512U &&
		    pt[0] == encode[0] &&
		    pt[511] == encode[0] &&
		    pd[0] == encode[1] &&
		    pd[511] == encode[1] &&
		    pdp[0] == encode[2] &&
		    pml4[0] == encode[3] &&
		    pml4[511] == encode[3],
		"p6c0: P6C0-PPGTT every level is a full page of the level below's encode");

	/* The table pages are reached by DMA address, not bound in the GGTT. */
	drv_i915_ktest_check(
		ktest,
		t->pp.scratch[0]->bound == 0 &&
		    t->pp.scratch[3]->bound == 0 &&
		    t->pp.top_pd->bound == 0 &&
		    t->pp.top_pd_dma != 0U,
		"p6c0: P6C0-PPGTT table pages are reached by DMA address, not bound in the GGTT");
}

/* Checks the GT scratch page (intel_gt_init_scratch(SZ_4K) with PIN_HIGH). */
static void
i915_ktest_gt_scratch(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_object *scratch;
	int error;

	/* Creates and pins the scratch page. */
	scratch = NULL;
	error = drv_i915_gt_init_scratch(&t->gm, &scratch);

	/* It is one 4 KiB page, bound in the GGTT. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    scratch != NULL &&
		    scratch->bytes == I915_GT_PAGE_BYTES &&
		    scratch->pages == 1U &&
		    scratch->bound == 1,
		"p6c0: P6C0-SCRATCH the gt scratch page is 4 KiB and pinned in the GGTT");
}

/* Sets the render engine up for execlists submission and checks its status page and registers. */
static int
i915_ktest_gt_execlists_setup(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_engine *rcs;
	int error;

	rcs = &t->rcs;

	/* Describes the render engine. */
	kern_memset(&t->rcs_info, 0, sizeof(t->rcs_info));
	t->rcs_info.id = I915_RCS0;
	t->rcs_info.class = I915_RENDER_CLASS;
	t->rcs_info.instance = 0;
	t->rcs_info.mmio_base = 0x2000U;
	t->rcs_info.name = "rcs0";
	t->rcs_info.context_size = 14U * 4096U;

	/* One slice with slice power gating. */
	kern_memset(&t->sseu, 0, sizeof(t->sseu));
	t->sseu.slice_mask = 0x1U;
	t->sseu.has_slice_pg = 1;

	/* Sets the engine up. */
	error = drv_i915_engine_setup_common(rcs, &t->rcs_info, &t->gm, &t->sseu);
	drv_i915_execlists_submission_setup(rcs);

	/* Gen11+ puts the CSB at HWSP dword 0x10 and its write pointer at 0x2f. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    rcs->status_page != NULL &&
		    rcs->status_page->bound == 1 &&
		    rcs->status_page->bytes == I915_GT_PAGE_BYTES &&
		    rcs->hwsp_ggtt == rcs->status_page->ggtt_offset &&
		    rcs->submit_reg == 0x2510U &&
		    rcs->ctrl_reg == 0x2550U &&
		    rcs->csb_size == 12U &&
		    rcs->port_mask == 1U &&
		    rcs->csb_status == (volatile uint64_t *)&rcs->hwsp[0x10U] &&
		    rcs->csb_write == &rcs->hwsp[0x2fU],
		"p6c1: P6C1-ENGINE gen11+ puts the CSB at HWSP dword 0x10 and its write pointer at 0x2f");

	/* The rest needs the engine. */
	if (error != 0)
		return error;

	/* The context descriptor of another engine, the enable and the CSB reset. */
	i915_ktest_gt_engine_descriptor(ktest, t);
	i915_ktest_gt_execlists_enable(ktest, t);

	/* Succeeded: the render engine is set up. */
	return 0;
}

/* Checks that the class and instance travel pre-shifted in the upper descriptor dword. */
static void
i915_ktest_gt_engine_descriptor(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	int error;

	/* Describes the second video decode engine. */
	kern_memset(&t->vcs2_info, 0, sizeof(t->vcs2_info));
	t->vcs2_info.id = I915_VCS2;
	t->vcs2_info.class = I915_VIDEO_DECODE_CLASS;
	t->vcs2_info.instance = 2;
	t->vcs2_info.mmio_base = 0x1d0000U;
	t->vcs2_info.name = "vcs2";

	/* Sets it up. */
	error = drv_i915_engine_setup_common(&t->vcs2, &t->vcs2_info, &t->gm, &t->sseu);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "p6c1: P6C1-ENGINE the vcs2 engine could not be set up");
		return;
	}

	drv_i915_execlists_submission_setup(&t->vcs2);

	/*
	 * The class and instance travel in the upper dword of the context
	 * descriptor, so ccid holds them already shifted down by 32.
	 */
	drv_i915_ktest_check(
		ktest,
		t->vcs2.ccid == ((2U << (48 - 32)) | (1U << (61 - 32))) &&
		    t->vcs2.submit_reg == 0x1d0510U &&
		    t->vcs2.ctrl_reg == 0x1d0550U,
		"p6c1: P6C1-ENGINE ccid carries class/instance pre-shifted by 32");

	drv_i915_engine_release(&t->vcs2, &t->gm);
}

/* Checks the enable_execlists register sequence and the CSB pointer reset. */
static void
i915_ktest_gt_execlists_enable(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_engine *rcs;
	int imr;
	int mode;
	int mi_mode;
	int hws;
	int emr_clear;
	int emr;
	int csb_pointer;

	rcs = &t->rcs;

	/* Enables execlists on a clean model. */
	i915_fake_open(&t->mmio, &t->fake);
	t->fake.wt_n = 0U;
	drv_i915_execlists_enable(rcs, &t->mmio);

	/* Legacy mode off, STOP_RING cleared, HWS_PGA programmed. */
	imr = i915_fake_wt_find(&t->fake, 0x2098U, 0xffffffffU, 0xffffffffU);
	mode = i915_fake_wt_find(&t->fake, 0x229cU, (8U << 16) | 8U, 0xffffffffU);
	mi_mode = i915_fake_wt_find(&t->fake, 0x209cU, 0x100U << 16, 0xffffffffU);
	hws = i915_fake_wt_find(&t->fake, 0x2080U, (uint32_t)rcs->hwsp_ggtt, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		imr >= 0 &&
		    mode >= 0 &&
		    mi_mode >= 0 &&
		    hws >= 0 &&
		    rcs->resumed == 1,
		"p6c1: P6C1-ENABLE gen11 disables legacy mode, clears STOP_RING and programs HWS_PGA");

	/* The error interrupt unmasks only I915_ERROR_INSTRUCTION. */
	emr_clear = i915_fake_wt_find(&t->fake, 0x20b0U, 0xffffffffU, 0xffffffffU);
	emr = i915_fake_wt_find(&t->fake, 0x20b4U, ~I915_ERROR_INSTRUCTION, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		emr_clear >= 0 && emr >= 0,
		"p6c1: P6C1-ENABLE the error interrupt unmasks only I915_ERROR_INSTRUCTION");

	/* Resets the CSB pointers. */
	t->fake.wt_n = 0U;
	drv_i915_execlists_reset_csb_pointers(rcs, &t->mmio);

	/* The head parks one entry behind and every entry is poisoned. */
	csb_pointer = i915_fake_wt_find(&t->fake, 0x23a0U, (0xffffU << 16) | (11U << 8) | 11U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		rcs->csb_head == 11U &&
		    *rcs->csb_write == 11U &&
		    rcs->csb_status[0] == ~(uint64_t)0 &&
		    rcs->csb_status[11] == ~(uint64_t)0 &&
		    rcs->csb_reset_writes == 2U &&
		    csb_pointer >= 0,
		"p6c1: P6C1-CSB the head parks one entry behind and every entry is poisoned");
}

/* Runs the context image, request, submission and resume tests on the render and copy engines. */
static void
i915_ktest_gt_contexts(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_context *ce;
	int error;

	ce = &t->rcs_ce;

	/* Allocates the render context with a 4 KiB ring. */
	error = drv_i915_lrc_alloc(ce, &t->rcs, &t->pp, &t->gm, 4096U, 1U);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "p6c2: P6C2-STATE the context image could not be allocated");
		return;
	}

	/* Builds the image a never-run context starts from. */
	drv_i915_lrc_init_state(ce);

	/* The image, its ring registers and its workaround batches. */
	i915_ktest_gt_context_state(ktest, t);
	i915_ktest_gt_context_ring(ktest, t);
	i915_ktest_gt_context_wa_batch(ktest, t);
	i915_ktest_gt_context_xcs(ktest, t);

	/* What a request writes into the render ring. */
	i915_ktest_gt_render_request(ktest, t);

	/* The copy engine's requests, its submission and the resume pieces. */
	i915_ktest_gt_copy_engine(ktest, t);

	/* The render power clock state with and without slice power gating. */
	i915_ktest_gt_context_rpcs(ktest, t);

	/* Gives the image and the ring back. */
	drv_i915_lrc_release(ce, &t->gm);
	drv_i915_ktest_check(
		ktest,
		ce->allocated == 0 &&
		    ce->state == NULL &&
		    ce->ring.obj == NULL,
		"p6c2: P6C2-STATE release gives the image and the ring back");
}

/* Checks the layout and first values of a never-run render context image. */
static void
i915_ktest_gt_context_state(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const struct i915_gt_context *ce;
	const uint32_t *rs;
	unsigned dwords;

	ce = &t->rcs_ce;
	rs = ce->lrc_reg_state;

	/* The image is the per-process status page, then the register state one page in. */
	drv_i915_ktest_check(
		ktest,
		ce->state_bytes == 16U * 4096U &&
		    ce->wa_bb_page == 14U &&
		    ce->state->bound == 1 &&
		    ce->ring.obj->bound == 1 &&
		    ce->lrc_reg_state == (uint32_t *)((char *)ce->state->cpu + 4096U),
		"p6c2: P6C2-STATE the image is ppHWSP then the register state one page in");

	/* Restore is inhibited and PDP0 names the four-level top directory. */
	drv_i915_ktest_check(
		ktest,
		rs[CTX_CONTEXT_CONTROL] == (((8U << 16) | 8U) | (1U << 16) | 1U) &&
		    rs[CTX_PDP0_LDW] == (uint32_t)t->pp.top_pd_dma &&
		    rs[CTX_PDP0_UDW] == (uint32_t)(t->pp.top_pd_dma >> 32),
		"p6c2: P6C2-STATE restore is inhibited and PDP0 names the 4-level top directory");

	/* __reset_stop_ring() clears STOP_RING and masks it in. */
	drv_i915_ktest_check(
		ktest,
		(rs[I915_KTEST_LRC_MI_MODE_INDEX + 1] & 0x100U) == 0U &&
		    (rs[I915_KTEST_LRC_MI_MODE_INDEX + 1] & (0x100U << 16)) != 0U &&
		    rs[I915_KTEST_LRC_BB_OFFSET_INDEX + 1] == 0U,
		"p6c2: P6C2-STATE __reset_stop_ring clears STOP_RING and masks it in");

	/*
	 * The decoded LRI headers and register offsets: a NOP skip first, a
	 * posted CS_MMIO LRI, the high group of a two-byte offset first, and
	 * the closing BB_END|1 of an inhibited image after the last entry.
	 */
	dwords = ce->reg_state_dwords;
	drv_i915_ktest_check(
		ktest,
		rs[0] == 0U &&
		    rs[1] == (0x11000019U | (1U << 12) | (1U << 19)) &&
		    rs[2] == 0x2244U &&
		    rs[4] == 0x2034U &&
		    dwords > 0x70U &&
		    dwords < 1024U &&
		    rs[dwords] == (uint32_t)(0x05000000U | 1U),
		"p6c2: P6C2-OFFSETS NOP skips, LRI is posted+CS_MMIO, REG16 decodes high-group-first");
}

/* Checks the ring registers and the descriptor of the render context. */
static void
i915_ktest_gt_context_ring(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_context *ce;
	const uint32_t *rs;
	uint32_t desc;
	uint32_t state_ggtt;

	ce = &t->rcs_ce;
	rs = ce->lrc_reg_state;

	/* Writes the ring registers for an empty ring. */
	desc = drv_i915_lrc_update_regs(ce, 0U);

	/* A 4 KiB ring has RING_CTL size 0, and the render engine asks for its slice. */
	drv_i915_ktest_check(
		ktest,
		rs[CTX_RING_START] == (uint32_t)ce->ring.ggtt_offset &&
		    rs[CTX_RING_HEAD] == 0U &&
		    rs[CTX_RING_TAIL] == 0U &&
		    rs[CTX_RING_CTL] == 1U &&
		    rs[CTX_R_PWR_CLK_STATE] == (0x80000000U | (1U << 18) | (1U << 12)),
		"p6c2: P6C2-RING a 4 KiB ring programs RING_CTL size 0 and render asks for its slice");

	/* The descriptor is 64-bit, VALID and PRIVILEGE, and the update forces a restore. */
	state_ggtt = (uint32_t)ce->state->ggtt_offset;
	drv_i915_ktest_check(
		ktest,
		desc == ((state_ggtt | 0x119U) | 4U) && ce->lrca == (state_ggtt | 0x119U),
		"p6c2: P6C2-RING the descriptor is 64b|VALID|PRIVILEGE and the update forces a restore");
}

/* Checks the AUX invalidation registers and the render context's INDIRECT_CTX and PER_CTX_BB. */
static void
i915_ktest_gt_context_wa_batch(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const struct i915_gt_context *ce;
	const uint32_t *rs;
	const uint32_t *bb;
	const uint32_t *per_ctx;
	uint32_t aux_rcs;
	uint32_t aux_bcs;
	uint32_t aux_vcs0;
	uint32_t aux_vcs2;
	uint32_t aux_vecs;
	uint32_t sgg;
	unsigned index;
	int saw_cctl;
	int saw_dbg;
	int saw_sem;

	ce = &t->rcs_ce;
	rs = ce->lrc_reg_state;

	/* Every ADL-P engine has its own AUX_INV register (gen12_get_aux_inv_reg()). */
	aux_rcs = drv_i915_lrc_aux_inv_reg(I915_RCS0);
	aux_bcs = drv_i915_lrc_aux_inv_reg(I915_BCS0);
	aux_vcs0 = drv_i915_lrc_aux_inv_reg(I915_VCS0);
	aux_vcs2 = drv_i915_lrc_aux_inv_reg(I915_VCS2);
	aux_vecs = drv_i915_lrc_aux_inv_reg(I915_VECS0);
	drv_i915_ktest_check(
		ktest,
		aux_rcs == 0x4208U &&
		    aux_bcs == 0x4248U &&
		    aux_vcs0 == 0x4218U &&
		    aux_vcs2 == 0x4298U &&
		    aux_vecs == 0x4238U,
		"p6c2b: P6C2B-AUXINV every ADL-P engine has its own AUX_INV register");

	/* The batch the render restore runs, at the workaround page. */
	bb = (const uint32_t *)((const char *)ce->state->cpu + ce->wa_bb_page * 4096U);
	sgg = (uint32_t)ce->state->ggtt_offset;

	/* The timestamp workaround loads GPR0 from the saved state and back twice. */
	drv_i915_ktest_check(
		ktest,
		bb[0] == (0x14800002U | (1U << 22) | (1U << 19)) &&
		    bb[1] == 0x2600U &&
		    bb[2] == (sgg + 4096U + 0x23U * 4U) &&
		    bb[3] == 0U &&
		    bb[4] == (0x15000001U | (1U << 18) | (1U << 19)) &&
		    bb[6] == 0x23a8U,
		"p6c2b: P6C2B-RCS the timestamp WA loads GPR0 from the saved state and back twice");

	/* Looks for the render-only workarounds and the AUX invalidate poll. */
	saw_cctl = 0;
	saw_dbg = 0;
	saw_sem = 0;
	for (index = 0U; index < ce->indirect_bb_dwords; index++) {
		/* RING_CMD_BUF_CCTL. */
		if (bb[index] == 0x2084U)
			saw_cctl = 1;

		/* GEN12_CS_DEBUG_MODE2. */
		if (bb[index] == 0x20d8U)
			saw_dbg = 1;

		/* MI_SEMAPHORE_WAIT_TOKEN polling the AUX invalidate. */
		if (bb[index] == (0x0e000003U | (1U << 16) | (1U << 15) | (4U << 12)))
			saw_sem = 1;
	}

	/* Render adds the CMD_BUF_CCTL workaround and Wa_18022495364, and polls the AUX invalidate. */
	drv_i915_ktest_check(
		ktest,
		saw_cctl != 0 &&
		    saw_dbg != 0 &&
		    saw_sem != 0 &&
		    ce->indirect_bb_dwords == 32U,
		"p6c2b: P6C2B-RCS render adds the CMD_BUF_CCTL WA and Wa_18022495364, and polls the AUX invalidate");

	/* The indirect pointer carries the size in cachelines, and the offset is the default. */
	drv_i915_ktest_check(
		ktest,
		rs[I915_KTEST_LRC_RING_INDIRECT_PTR + 1] == (ce->indirect_bb_ggtt | 2U) &&
		    rs[I915_KTEST_LRC_RING_INDIRECT_OFFSET + 1] == (0xdU << 6) &&
		    ce->indirect_bb_ggtt == (sgg + ce->wa_bb_page * 4096U),
		"p6c2b: P6C2B-SLOTS the indirect pointer carries the size in cachelines and the default offset");

	/* The per-context batch is its own terminator, with FORCE and VALID. */
	per_ctx = (const uint32_t *)((const char *)ce->state->cpu + (ce->wa_bb_page + 1U) * 4096U);
	drv_i915_ktest_check(
		ktest,
		rs[I915_KTEST_LRC_RING_WA_BB_PER_CTX + 1] == ((ce->indirect_bb_ggtt + 4096U) | 0x5U) &&
		    ce->per_ctx_bb_set == 1 &&
		    per_ctx[0] == 0x05000000U,
		"p6c2b: P6C2B-PERCTX the per-context batch is its own terminator, FORCE|VALID");
}

/* Checks the image size of both engine kinds and a video engine's INDIRECT_CTX. */
static void
i915_ktest_gt_context_xcs(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const uint32_t *vbb;
	unsigned index;
	int saw_cctl;
	int saw_dbg;
	int saw_inv;
	int error;

	/* Describes the first video decode engine. */
	kern_memset(&t->vcs0_info, 0, sizeof(t->vcs0_info));
	t->vcs0_info.id = I915_VCS0;
	t->vcs0_info.class = I915_VIDEO_DECODE_CLASS;
	t->vcs0_info.instance = 0;
	t->vcs0_info.mmio_base = 0x1c0000U;
	t->vcs0_info.context_size = 2U * 4096U;
	t->vcs0_info.name = "vcs0";

	/* Sets the engine up and allocates its context. */
	error = drv_i915_engine_setup_common(&t->vcs0, &t->vcs0_info, &t->gm, &t->sseu);
	if (error == 0)
		error = drv_i915_lrc_alloc(&t->vcs0_ce, &t->vcs0, &t->pp, &t->gm, 4096U, 2U);

	/* The checks need both. */
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "p6c2b: P6C2B-XCS the vcs0 context could not be built");
		return;
	}

	/* Builds the image and its batches. */
	drv_i915_execlists_submission_setup(&t->vcs0);
	drv_i915_lrc_init_state(&t->vcs0_ce);
	(void)drv_i915_lrc_update_regs(&t->vcs0_ce, 0U);

	/* INDIRECT_CTX and PER_CTX_BB add two pages; wa_bb_page names the first. */
	drv_i915_ktest_check(
		ktest,
		t->rcs_ce.state_bytes == 16U * 4096U &&
		    t->rcs_ce.wa_bb_page == 14U &&
		    t->vcs0_ce.state_bytes == 4U * 4096U &&
		    t->vcs0_ce.wa_bb_page == 2U,
		"p6c2: P6C2-SIZE INDIRECT_CTX and PER_CTX_BB add two pages, wa_bb_page names the first");

	/* Looks for the render-only workarounds and the video AUX invalidate. */
	vbb = (const uint32_t *)((const char *)t->vcs0_ce.state->cpu + t->vcs0_ce.wa_bb_page * 4096U);
	saw_cctl = 0;
	saw_dbg = 0;
	saw_inv = 0;
	for (index = 0U; index < t->vcs0_ce.indirect_bb_dwords; index++) {
		/* The engine's RING_CMD_BUF_CCTL. */
		if (vbb[index] == 0x1c0084U)
			saw_cctl = 1;

		/* GEN12_CS_DEBUG_MODE2. */
		if (vbb[index] == 0x20d8U)
			saw_dbg = 1;

		/* GEN12_VD0_AUX_INV. */
		if (vbb[index] == 0x4218U)
			saw_inv = 1;
	}

	/* A video engine gets the AUX invalidate but neither render-only workaround. */
	drv_i915_ktest_check(
		ktest,
		saw_cctl == 0 &&
		    saw_dbg == 0 &&
		    saw_inv != 0 &&
		    t->vcs0_ce.wa_bb_page == 2U &&
		    t->vcs0_ce.indirect_bb_dwords == 32U,
		"p6c2b: P6C2B-XCS a video engine gets the AUX invalidate but neither render-only WA");

	drv_i915_lrc_release(&t->vcs0_ce, &t->gm);
	drv_i915_engine_release(&t->vcs0, &t->gm);
}

/* Checks the render power clock state the image carries with and without slice power gating. */
static void
i915_ktest_gt_context_rpcs(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	uint32_t without_pg;
	uint32_t with_pg;

	/* Rewrites the registers as if the engine could not power-gate a slice. */
	t->rcs.sseu_has_slice_pg = 0;
	(void)drv_i915_lrc_update_regs(&t->rcs_ce, 0U);
	without_pg = t->rcs_ce.lrc_reg_state[CTX_R_PWR_CLK_STATE];

	/* Rewrites them again with slice power gating, as the engine was set up. */
	t->rcs.sseu_has_slice_pg = 1;
	(void)drv_i915_lrc_update_regs(&t->rcs_ce, 0U);
	with_pg = t->rcs_ce.lrc_reg_state[CTX_R_PWR_CLK_STATE];

	/* Gen12 requests slices only; without slice power gating nothing is requested. */
	drv_i915_ktest_check(
		ktest,
		with_pg == (0x80000000U | (1U << 18) | (1U << 12)) && without_pg == 0U,
		"p6c2: P6C2-RPCS gen12 emits ENABLE|S_CNT_ENABLE|slices and no subslice/EU fields");
}

/* Checks what a render request writes into its ring: the invalidate, the context workarounds and the breadcrumb. */
static void
i915_ktest_gt_render_request(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_context *ce;
	struct i915_gt_request *rq;
	const uint32_t *r;
	uint32_t hw;
	uint32_t fl_inv_bg1;
	int error;

	ce = &t->rcs_ce;
	rq = &t->rq;
	r = ce->ring.vaddr;
	hw = (uint32_t)t->rcs.hwsp_ggtt + 0x100U;

	/* The flush block without FLUSH_L3. */
	fl_inv_bg1 = 0x103070a1U;

	/* Creates the request: request_alloc's EMIT_INVALIDATE on render. */
	error = drv_i915_request_create(rq, ce, 5U, hw, &t->rcs.hwsp[0x40U]);

	/* Render runs the flush block too (AUX invalidate), then the invalidate and the AUX poll. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    ce->ring.emit == 22U * 4U &&
		    r[0] == 0x7a000204U &&
		    r[1] == fl_inv_bg1 &&
		    r[2] == 0xd0U &&
		    r[6] == 0x02800101U &&
		    r[7] == 0x7a000004U &&
		    r[8] == 0x20344c1cU &&
		    r[9] == 0xd0U &&
		    r[13] == 0x11020001U &&
		    r[14] == 0x4208U &&
		    r[15] == 1U &&
		    r[16] == 0x0e01c003U &&
		    r[18] == 0x4208U &&
		    r[21] == 0x02800100U,
		"p6c3: P6C3-CREATE render runs the flush block too (AUX inv), then invalidate + AUX poll");

	/* Emits the render context workarounds: a barrier flush, LRI(5) with its pairs and a NOOP, a flush. */
	drv_i915_engine_init_ctx_wa(&t->ctx_wa, &t->rcs_info, 12, 3U);
	error = drv_i915_emit_ctx_wa(rq, &t->ctx_wa, &t->mmio);

	/* A barrier flush adds FLUSH_L3; masked and ~0-clear entries are not read. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    t->ctx_wa.count == 5U &&
		    r[22] == 0x7a000204U &&
		    r[23] == (fl_inv_bg1 | (1U << 27)) &&
		    r[44] == 0x11000009U &&
		    r[45] == t->ctx_wa.list[0].reg &&
		    r[46] == t->ctx_wa.list[0].set &&
		    r[55] == 0U &&
		    ce->ring.emit == 78U * 4U,
		"p6c3: P6C3-CTXWA a BARRIER flush adds FLUSH_L3; masked and ~0-clear entries are not read");

	/* Closes the request with the render fini breadcrumb. */
	error = drv_i915_request_add(rq);

	/* Render flushes L3 with DEPTH_STALL, writes the seqno, then waits on PREEMPT. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    rq->added == 1 &&
		    r[78] == 0x7a000204U &&
		    r[79] == 0x181430a1U &&
		    r[80] == 0U &&
		    r[84] == 0x7a000004U &&
		    r[85] == 0x01104080U &&
		    r[86] == hw &&
		    r[88] == 5U &&
		    r[90] == 0x01000000U &&
		    r[91] == 0x04000001U &&
		    r[92] == 0x02800000U &&
		    r[93] == 0x0e40c003U &&
		    r[95] == ((uint32_t)t->rcs.hwsp_ggtt + 0xc8U),
		"p6c3: P6C3-ADD render flushes L3 with DEPTH_STALL, writes the seqno, then waits on PREEMPT");

	/* The tail sits before the wa_tail pair, and both are qword aligned. */
	drv_i915_ktest_check(
		ktest,
		rq->tail == 98U * 4U &&
		    rq->wa_tail == 100U * 4U &&
		    (rq->tail & 7U) == 0U &&
		    (rq->wa_tail & 7U) == 0U &&
		    r[98] == 0x02800000U &&
		    r[99] == 0U &&
		    ce->ring.emit == rq->wa_tail,
		"p6c3: P6C3-ADD tail sits before the wa_tail pair and both are qword aligned");
}

/* Builds the copy engine and its context, and runs the request, submission and resume tests on it. */
static void
i915_ktest_gt_copy_engine(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	uint32_t hw;
	int error;

	/* Describes the copy engine. */
	kern_memset(&t->bcs_info, 0, sizeof(t->bcs_info));
	t->bcs_info.id = I915_BCS0;
	t->bcs_info.class = I915_COPY_ENGINE_CLASS;
	t->bcs_info.instance = 0;
	t->bcs_info.mmio_base = 0x22000U;
	t->bcs_info.context_size = 2U * 4096U;
	t->bcs_info.name = "bcs0";

	/* Sets the engine up and allocates its context. */
	error = drv_i915_engine_setup_common(&t->bcs, &t->bcs_info, &t->gm, &t->sseu);
	if (error == 0)
		error = drv_i915_lrc_alloc(&t->bcs_ce, &t->bcs, &t->pp, &t->gm, 4096U, 3U);

	/* The tests need both. */
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "p6c3: P6C3-BCS the bcs0 context could not be built");
		return;
	}

	/* Builds the image. */
	drv_i915_execlists_submission_setup(&t->bcs);
	drv_i915_lrc_init_state(&t->bcs_ce);
	(void)drv_i915_lrc_update_regs(&t->bcs_ce, 0U);

	/* The copy request, its submission, the CSB and the resume pieces. */
	hw = (uint32_t)t->bcs.hwsp_ggtt + 0x100U;
	i915_ktest_gt_copy_request(ktest, t, hw);
	i915_ktest_gt_copy_submit(ktest, t);
	i915_ktest_gt_copy_csb(ktest, t, hw);
	i915_ktest_gt_resume_pieces(ktest, t);

	drv_i915_lrc_release(&t->bcs_ce, &t->gm);
	drv_i915_engine_release(&t->bcs, &t->gm);
}

/* Checks the copy engine's request stream and its context workaround. */
static void
i915_ktest_gt_copy_request(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t,
	uint32_t hw)
{
	struct i915_gt_context *ce;
	struct i915_gt_request *rq;
	const uint32_t *r;
	uint32_t want;
	int error;

	ce = &t->bcs_ce;
	rq = &t->rq;
	r = ce->ring.vaddr;

	/* Creates the request. */
	error = drv_i915_request_create(rq, ce, 7U, hw, &t->bcs.hwsp[0x40U]);

	/* The copy invalidate carries MI_FLUSH_DW_CCS, not INVALIDATE_BSD. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    ce->ring.emit == 14U * 4U &&
		    r[0] == 0x02800101U &&
		    r[1] == 0x13254002U &&
		    r[2] == 0xd0U &&
		    r[6] == 0x4248U &&
		    r[13] == 0x02800100U,
		"p6c3: P6C3-BCS the copy invalidate carries MI_FLUSH_DW_CCS, not INVALIDATE_BSD");

	/* BLIT_CCTL is a plain read-modify-write entry: it is read from the register, which holds all-ones. */
	drv_i915_engine_init_ctx_wa(&t->ctx_wa, &t->bcs_info, 12, 3U);
	drv_i915_raw_write32(&t->mmio, t->ctx_wa.list[0].reg, 0xffffffffU);
	want = (0xffffffffU & ~t->ctx_wa.list[0].clr) | t->ctx_wa.list[0].set;
	error = drv_i915_emit_ctx_wa(rq, &t->ctx_wa, &t->mmio);

	/* The entry is read, cleared and set before the LRI. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    t->ctx_wa.count == 1U &&
		    r[28] == 0x11000001U &&
		    r[29] == t->ctx_wa.list[0].reg &&
		    r[30] == want &&
		    r[31] == 0U,
		"p6c3: P6C3-BCS the plain BLIT_CCTL entry is read, cleared and set before the LRI");

	/* Closes the request. */
	error = drv_i915_request_add(rq);

	/* The copy breadcrumb is 18 dwords: a flush, a USE_GTT store and the tail. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    r[46] == 0x13000002U &&
		    r[47] == 0U &&
		    r[50] == 0x13004002U &&
		    r[51] == (hw | 4U) &&
		    r[53] == 7U &&
		    rq->tail - 46U * 4U == 16U * 4U &&
		    (rq->tail & 7U) == 0U &&
		    ce->ring.emit - 46U * 4U == 18U * 4U,
		"p6c3: P6C3-BCS the xcs breadcrumb is 18 dwords: flush, USE_GTT store, tail");
}

/* Checks the ELSQ submission of the copy request. */
static void
i915_ktest_gt_copy_submit(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_context *ce;
	struct i915_gt_request *rq;
	uint32_t preempt;
	uint32_t lo;
	uint32_t hi;
	uint32_t pre_tail;
	int port1_lo;
	int port1_hi;
	int port0_lo;
	int port0_hi;
	int load;
	int submitted;
	int second;
	int error;

	ce = &t->bcs_ce;
	rq = &t->rq;

	/* Resets the CSB pointers with PREEMPT set, as the reset preparation leaves it. */
	t->bcs.hwsp[I915_KTEST_HWSP_PREEMPT] = 1U;
	drv_i915_execlists_reset_csb_pointers(&t->bcs, &t->mmio);
	preempt = t->bcs.hwsp[I915_KTEST_HWSP_PREEMPT];
	drv_i915_ktest_check(ktest, preempt == 0U, "p6c4: P6C4-PAUSE reset_csb_pointers clears PREEMPT first (else every breadcrumb spins)");

	/* Submits the request. */
	drv_i915_execlists_init(&t->bcs_el);
	t->fake.wt_n = 0U;
	lo = (uint32_t)ce->state->ggtt_offset | 0x119U | 4U;
	hi = (1U << 5) | (3U << 29);
	pre_tail = rq->tail;
	error = drv_i915_execlists_submit(&t->bcs, &t->bcs_el, &t->mmio, rq);

	/* Port 1 (empty) first, then port 0 low and high with FORCE_RESTORE on the first submission, then EL_CTRL_LOAD. */
	port1_lo = i915_fake_wt_find(&t->fake, 0x22518U, 0U, 0xffffffffU);
	port1_hi = i915_fake_wt_find(&t->fake, 0x2251cU, 0U, 0xffffffffU);
	port0_lo = i915_fake_wt_find(&t->fake, 0x22510U, lo, 0xffffffffU);
	port0_hi = i915_fake_wt_find(&t->fake, 0x22514U, hi, 0xffffffffU);
	load = i915_fake_wt_find(&t->fake, 0x22550U, 1U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    port1_lo >= 0 &&
		    port1_hi >= 0 &&
		    port0_lo >= 0 &&
		    port0_hi >= 0 &&
		    load >= 0 &&
		    port1_lo < port0_lo &&
		    port0_hi < load,
		"p6c3b: P6C3B-SUBMIT port 1 (empty) then port 0 lo/hi, FORCE_RESTORE on first submit, then EL_CTRL_LOAD");

	/* The submission left RING_TAIL at the request tail, moved the tail to wa_tail and took tag 0. */
	submitted = 0;
	if (ce->lrc_reg_state[CTX_RING_TAIL] == pre_tail &&
	    rq->tail == rq->wa_tail &&
	    (ce->lrc_desc & 4U) == 0U &&
	    ce->tag == 0 &&
	    (t->bcs_el.context_tag & 1U) == 0U &&
	    t->bcs_el.serial == 1U)
		submitted = 1;

	/* A second submission while one is in flight is refused; it is only tried after a first one that looks right. */
	second = 0;
	if (submitted != 0)
		second = drv_i915_execlists_submit(&t->bcs, &t->bcs_el, &t->mmio, rq);

	drv_i915_ktest_check(
		ktest,
		submitted != 0 && second == EBUSY,
		"p6c3b: P6C3B-SUBMIT RING_TAIL=rq->tail, rq->tail moves to wa_tail, tag 0 taken, one in flight");
}

/* Checks the context status buffer, the seqno compare, the MMIO fallback and the refusals. */
static void
i915_ktest_gt_copy_csb(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t,
	uint32_t hw)
{
	struct i915_gt_engine *bcs;
	struct i915_execlists *el;
	struct i915_gt_request *done;
	struct i915_gt_request *rq;
	uint32_t *begun;
	int reached;
	int not_reached;
	int bit5;
	int unaligned;

	bcs = &t->bcs;
	el = &t->bcs_el;
	rq = &t->rq;

	/* Plays the engine: a promote, then a completion. */
	bcs->csb_status[0] = I915_KTEST_CSB_PROMOTE;
	bcs->csb_status[1] = I915_KTEST_CSB_COMPLETE;
	*bcs->csb_write = 1U;
	done = drv_i915_execlists_process_csb(bcs, el, &t->mmio);

	/* The pair promotes then completes, returns the tag and poisons both slots. */
	drv_i915_ktest_check(
		ktest,
		done == rq &&
		    el->promotes == 1U &&
		    el->completes == 1U &&
		    el->have_active == 0 &&
		    el->csb_errors == 0U &&
		    (el->context_tag & 1U) == 1U &&
		    t->bcs_ce.tag == -1 &&
		    bcs->csb_head == 1U &&
		    bcs->csb_status[0] == ~(uint64_t)0 &&
		    bcs->csb_status[1] == ~(uint64_t)0,
		"p6c3b: P6C3B-CSB the pair promotes then completes, returns the tag and poisons both slots");

	/* i915_seqno_passed() is a signed compare: complete at the seqno, not before. */
	bcs->hwsp[I915_KTEST_HWSP_SEQNO] = 7U;
	reached = drv_i915_request_completed(rq);
	bcs->hwsp[I915_KTEST_HWSP_SEQNO] = 6U;
	not_reached = drv_i915_request_completed(rq);
	drv_i915_ktest_check(
		ktest,
		reached == 1 && not_reached == 0,
		"p6c3b: P6C3B-SEQNO a request is complete once the HWSP seqno reaches it");

	/* An entry that never became visible in the status page is read from the MMIO status buffer. */
	drv_i915_raw_write32(&t->mmio, 0x22000U + 0x370U + 8U * 2U, 0x7ffU << 15);
	drv_i915_raw_write32(&t->mmio, 0x22000U + 0x370U + 8U * 2U + 4U, 1U << 15);
	*bcs->csb_write = 2U;
	(void)drv_i915_execlists_process_csb(bcs, el, &t->mmio);

	/* A completion with nothing active is an error. */
	drv_i915_ktest_check(
		ktest,
		el->csb_mmio_fallback == 1U &&
		    el->last_csb_lo == (0x7ffU << 15) &&
		    el->last_csb_hi == (1U << 15) &&
		    el->csb_errors == 1U,
		"p6c3b: P6C3B-FALLBACK an all-ones entry falls back to the MMIO status buffer; a completion with nothing active is an error");

	/* A breadcrumb address with bit 5 set is refused; an unaligned one is tried only after that. */
	bit5 = drv_i915_request_create(rq, &t->bcs_ce, 8U, hw | 0x20U, NULL);
	unaligned = 0;
	if (bit5 == EINVAL)
		unaligned = drv_i915_request_create(rq, &t->bcs_ce, 8U, hw | 4U, NULL);

	drv_i915_ktest_check(
		ktest,
		bit5 == EINVAL && unaligned == EINVAL,
		"p6c3: P6C3-REFUSE a breadcrumb address with bit 5 set or not qword aligned is refused");

	/* An odd dword count would leave RING_TAIL unaligned. */
	rq->ce = &t->bcs_ce;
	rq->error = 0;
	begun = drv_i915_ring_begin(rq, 3U);
	drv_i915_ktest_check(
		ktest,
		begun == NULL && rq->error == EINVAL,
		"p6c3: P6C3-REFUSE an odd dword count is refused (RING_TAIL must stay qword aligned)");
}

/* Checks the pieces of the GT resume on the copy engine. */
static void
i915_ktest_gt_resume_pieces(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_context *ce;
	int stop_ring;
	int prefetch;
	int error;

	ce = &t->bcs_ce;

	/* Stops an engine whose ring is empty (HEAD equals TAIL). */
	t->fake.wt_n = 0U;
	drv_i915_raw_write32(&t->mmio, 0x22034U, 0U);
	drv_i915_raw_write32(&t->mmio, 0x22030U, 0U);
	error = drv_i915_engine_stop_cs(&t->bcs, &t->mmio);

	/* STOP_RING and (Wa_22011802037) PREFETCH_DISABLE; an empty ring is not a timeout. */
	stop_ring = i915_fake_wt_find(&t->fake, 0x2209cU, (0x100U << 16) | 0x100U, 0xffffffffU);
	prefetch = i915_fake_wt_find(&t->fake, 0x2229cU, (0x400U << 16) | 0x400U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    stop_ring >= 0 &&
		    prefetch >= 0,
		"p6c4: P6C4-STOPCS STOP_RING and (Wa_22011802037) PREFETCH_DISABLE; an empty ring is not a timeout");

	/* A ring that still holds work is a timeout. */
	drv_i915_raw_write32(&t->mmio, 0x22034U, 0x40U);
	error = drv_i915_engine_stop_cs(&t->bcs, &t->mmio);
	drv_i915_ktest_check(ktest, error == ETIMEDOUT, "p6c4: P6C4-STOPCS a ring that still holds work IS a timeout");
	drv_i915_raw_write32(&t->mmio, 0x22034U, 0U);

	/* Wa_22011802037's pending MI_FORCE_WAKE: a request bit with its mirror. */
	drv_i915_raw_write32(&t->mmio, 0x800cU, (1U << 25) | (1U << 9));
	drv_i915_raw_write32(&t->mmio, 0xa2a0U, 1U);
	drv_i915_engine_wait_for_pending_mi_fw(&t->bcs, &t->mmio);
	drv_i915_ktest_check(ktest, t->bcs.mi_fw_pending == 1U, "p6c4: P6C4-MIFW pending = bits[29:25] & bits[13:9], read from MSG_IDLE_BCS");

	/* A request bit without its mirror. */
	drv_i915_raw_write32(&t->mmio, 0x800cU, 1U << 9);
	drv_i915_engine_wait_for_pending_mi_fw(&t->bcs, &t->mmio);
	drv_i915_ktest_check(ktest, t->bcs.mi_fw_pending == 0U, "p6c4: P6C4-MIFW a request bit without its mirror is not pending");

	/* The RC6 and RPS sanitize steps. */
	i915_ktest_gt_power_sanitize(ktest, t);

	/* lrc_reset() on a used context whose context control was lost. */
	ce->lrc_reg_state[CTX_CONTEXT_CONTROL] = 0U;
	drv_i915_lrc_reset(ce);

	/* The ring restarts at emit, the registers are scrubbed, and a restore is forced. */
	drv_i915_ktest_check(
		ktest,
		ce->ring.head == ce->ring.emit &&
		    ce->ring.tail == ce->ring.emit &&
		    ce->lrc_reg_state[CTX_CONTEXT_CONTROL] == (((8U << 16) | 8U) | (1U << 16) | 1U) &&
		    ce->lrc_reg_state[CTX_RING_HEAD] == ce->ring.emit &&
		    (ce->lrc_desc & 4U) != 0U,
		"p6c4: P6C4-LRCRESET the ring restarts at emit, the registers are scrubbed, restore is forced");
}

/* Checks the RC6 and RPS sanitize steps of the GT resume. */
static void
i915_ktest_gt_power_sanitize(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	int pg_enable;
	int rc_control;
	int rc_state;
	int pm_mask;

	/* Sanitizes an enabled RC6. */
	t->sanitize_rc6.supported = 1;
	t->sanitize_rc6.enabled = 1;
	t->fake.wt_n = 0U;
	drv_i915_rc6_sanitize(&t->sanitize_rc6, &t->mmio);

	/* PG_ENABLE, RC_CONTROL and RC_STATE go to 0 before init_hw. */
	pg_enable = i915_fake_wt_find(&t->fake, 0xa210U, 0U, 0xffffffffU);
	rc_control = i915_fake_wt_find(&t->fake, 0xa090U, 0U, 0xffffffffU);
	rc_state = i915_fake_wt_find(&t->fake, 0xa094U, 0U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		t->sanitize_rc6.enabled == 0 &&
		    pg_enable >= 0 &&
		    rc_control >= 0 &&
		    rc_state >= 0,
		"p6c4: P6C4-RC6SAN PG_ENABLE, RC_CONTROL and RC_STATE go to 0 before init_hw");

	/* Sanitizes an unsupported RC6, then RPS. */
	t->sanitize_rc6.supported = 0;
	t->fake.wt_n = 0U;
	drv_i915_rc6_sanitize(&t->sanitize_rc6, &t->mmio);
	drv_i915_rps_sanitize(&t->sanitize_rps, &t->mmio);

	/* An unsupported RC6 writes nothing; RPS masks every PM interrupt. */
	rc_control = i915_fake_wt_find(&t->fake, 0xa090U, 0U, 0xffffffffU);
	pm_mask = i915_fake_wt_find(&t->fake, 0xa168U, 0xffffffffU, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		rc_control < 0 && pm_mask >= 0,
		"p6c4: P6C4-RPSSAN unsupported rc6 writes nothing; rps masks every PM interrupt");
}

/* Describes the simulated GT of the recording, verification, migrate and PXP tests: one copy engine. */
static void
i915_ktest_gt_defaults_gt(
	struct i915_ktest_gt_state *t)
{
	struct i915_engine_info *engine;

	/* Starts from an empty GT and empty tables. */
	kern_memset(&t->defaults_gt, 0, sizeof(t->defaults_gt));
	kern_memset(&t->defaults_init, 0, sizeof(t->defaults_init));

	/* The copy engine. */
	engine = &t->defaults_gt.engines[0];
	engine->id = I915_BCS0;
	engine->class = I915_COPY_ENGINE_CLASS;
	engine->instance = 0;
	engine->mmio_base = 0x22000U;
	engine->context_size = 2U * 4096U;
	engine->name = "bcs0";
	t->defaults_gt.num_engines = 1U;

	/* One slice with slice power gating. */
	t->defaults_gt.sseu.slice_mask = 1U;
	t->defaults_gt.sseu.has_slice_pg = 1;

	/* The copy engine's context workarounds. */
	drv_i915_engine_init_ctx_wa(&t->defaults_init.ctx_wa[0], &t->defaults_gt.engines[0], 12, 3U);
}

/* Checks intel_engines_init(), the default-state inheritance and the wedge of a recording nobody answers. */
static void
i915_ktest_gt_defaults(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_defaults *d;
	int error;

	d = &t->defaults;

	/* Prepares the uncore lock the wedge's reset takes. */
	spin_init(&t->wedge_lock, LOCK_RANK_DEVICE, "ktest-wedge");

	/* Sets the simulated engines up on a clean model. */
	i915_fake_open(&t->mmio, &t->fake);
	error = drv_i915_engines_init(&t->engines, &t->defaults_gt, &t->gm, &t->pp);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "p6c4: P6C4B-INIT intel_engines_init failed on the simulated engine");
		return;
	}

	/* intel_engines_init() pins a 4 KiB kernel context per engine. */
	drv_i915_execlists_reset_csb_pointers(&t->engines.ge[0], &t->mmio);
	drv_i915_ktest_check(
		ktest,
		t->engines.n == 1U &&
		    t->engines.kernel_ce[0].allocated == 1 &&
		    t->engines.kernel_ce[0].ring.size == 4096U,
		"p6c4: P6C4B-INIT intel_engines_init pins a 4 KiB kernel context per engine");

	/* A later context starts from the engine's default state. */
	i915_ktest_gt_defaults_inherit(ktest, t);

	/* Records the defaults with nothing answering, for 1 ms. */
	kern_memset(d, 0, sizeof(*d));
	error = drv_i915_engines_record_defaults(d, &t->engines, &t->defaults_init, &t->gm, &t->pp, &t->mmio, &t->wedge_lock, 1U);

	/* The timeout is EIO, the GT is wedged (reset), and the record contexts are still put. */
	drv_i915_ktest_check(
		ktest,
		error == EIO &&
		    d->timed_out == 1 &&
		    d->wedged == 1 &&
		    d->ce[0].allocated == 0 &&
		    d->tl_page[0] == NULL &&
		    d->default_state[0] == NULL,
		"p6c4: P6C4B-WEDGE a timeout is -EIO, the GT is wedged (reset) and the contexts are still put");

	drv_i915_engines_defaults_release(d, &t->gm);
	drv_i915_engines_release(&t->engines, &t->gm);
}

/* Checks that lrc_init_state() starts a context from engine->default_state. */
static void
i915_ktest_gt_defaults_inherit(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_engine *engine;
	struct i915_gt_object *image;
	struct i915_gt_context *ce;
	uint32_t *words;
	uint32_t mark;
	uint32_t control;
	uint32_t pphwsp;
	uint32_t ring_start;
	int error;

	engine = &t->engines.ge[0];
	ce = &t->inherit_ce;

	/*
	 * Builds a default state the way a recording leaves one: an image of
	 * the engine's context size, with a word in the per-process status page
	 * and a mark in the engine state.
	 */
	image = drv_i915_gt_object_create(&t->gm, engine->info->context_size);
	if (image == NULL) {
		drv_i915_ktest_check(ktest, 0, "p6c4: P6C4B-INHERIT lrc_init_state copies engine->default_state, clears the ppHWSP, and does not inhibit the restore");
		return;
	}

	words = image->cpu;
	words[0] = 0xdeadbeefU;
	words[4096U / 4U + 0x40U] = 0x12345678U;

	/* Publishes it and builds a context from it. */
	engine->default_state = image;
	error = drv_i915_lrc_alloc(ce, engine, &t->pp, &t->gm, 4096U, 0U);
	mark = 0U;
	control = 0U;
	pphwsp = 0U;
	ring_start = 0U;
	if (error == 0) {
		drv_i915_lrc_init_state(ce);
		(void)drv_i915_lrc_update_regs(ce, 0U);
		mark = ce->lrc_reg_state[0x40U];
		control = ce->lrc_reg_state[CTX_CONTEXT_CONTROL];
		pphwsp = ((uint32_t *)ce->state->cpu)[0];
		ring_start = ce->lrc_reg_state[CTX_RING_START];
	}

	/* The mark arrives, the per-process status page is cleared, and the restore is not inhibited. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    mark == 0x12345678U &&
		    (control & 1U) == 0U &&
		    pphwsp == 0U &&
		    ring_start == (uint32_t)ce->ring.ggtt_offset,
		"p6c4: P6C4B-INHERIT lrc_init_state copies engine->default_state, clears the ppHWSP, and does not inhibit the restore");

	/* Gives the context and the image back. */
	if (error == 0)
		drv_i915_lrc_release(ce, &t->gm);

	engine->default_state = NULL;
	drv_i915_gt_object_destroy(&t->gm, image);
}

/* Checks __engines_verify_workarounds() with the hardware simulated. */
static void
i915_ktest_gt_verify_wa(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_wa_list *wl;
	int error;

	wl = &t->defaults_init.engine_wa[0];

	/* A masked, a plain, an MCR (not read through the CS) and a no-verify entry. */
	wl->count = 0U;
	wl->overflow = 0U;
	wl->name = "bcs0";
	drv_i915_wa_masked_en(wl, 0x2209cU, 0x8U, 0, "t: masked");
	drv_i915_wa_write_or(wl, 0x22050U, 0x100U, 0, "t: plain");
	drv_i915_wa_write_or(wl, 0xb134U, 0x1U, 1, "t: in an MCR range");
	drv_i915_wa_add_no_verify(wl, 0x22060U, 0U, 0x5U, 0, "t: no verify");

	/* Sets the simulated engines up on a clean model. */
	i915_fake_open(&t->mmio, &t->fake);
	error = drv_i915_engines_init(&t->engines, &t->defaults_gt, &t->gm, &t->pp);
	drv_i915_ktest_check(ktest, error == 0, "p6c5: P6C5-INIT intel_engines_init for the verify run");
	if (error != 0)
		return;

	/* The verification one step at a time, then the whole of it. */
	i915_ktest_gt_verify_wa_steps(ktest, t);
	i915_ktest_gt_verify_wa_whole(ktest, t);

	drv_i915_engines_release(&t->engines, &t->gm);
}

/* Checks the store request of the verification, its wait and the compare. */
static void
i915_ktest_gt_verify_wa_steps(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_verify_wa *vw;
	struct i915_gt_engine *e0;
	struct i915_wa_list *wl;
	volatile uint32_t *results;
	uint32_t scratch_ggtt;
	int found[4];
	int load;
	int busy;
	int error;

	vw = &t->verify;
	e0 = &t->engines.ge[0];
	wl = &t->defaults_init.engine_wa[0];

	/* Submits the stores. */
	drv_i915_execlists_reset_csb_pointers(e0, &t->mmio);
	t->fake.wt_n = 0U;
	kern_memset(vw, 0, sizeof(*vw));
	error = drv_i915_engine_verify_wa_submit(vw, 0U, &t->engines, wl, &t->gm, &t->mmio);

	/* Looks for one store per entry, to the scratch page at four bytes per list index. */
	scratch_ggtt = 0U;
	if (vw->scratch[0] != NULL)
		scratch_ggtt = (uint32_t)vw->scratch[0]->ggtt_offset;

	i915_ktest_gt_srm_find(t->engines.kernel_ce[0].ring.vaddr,
			       t->engines.kernel_ce[0].ring.size / 4U,
			       wl,
			       scratch_ggtt,
			       found);
	load = i915_fake_wt_find(&t->fake, 0x22550U, 1U, 0xffffffffU);

	/* One SRM per non-MCR entry, on the kernel context, submitted. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    vw->state[0] == I915_VWA_SRM &&
		    vw->rq[0].ce == &t->engines.kernel_ce[0] &&
		    vw->emitted[0] == 3U &&
		    vw->mcr_skipped[0] == 1U &&
		    vw->scratch[0] != NULL &&
		    vw->scratch[0]->bound == 1 &&
		    found[0] != 0 &&
		    found[1] != 0 &&
		    found[2] == 0 &&
		    found[3] != 0 &&
		    load >= 0,
		"p6c5: P6C5-SRM one SRM per non-MCR entry to scratch + 4 * list index, on the kernel context, submitted");

	/* Nothing happens before the breadcrumb lands and the CSB reports. */
	busy = drv_i915_engine_verify_wa_poll(vw, 0U, &t->engines, &t->mmio);
	drv_i915_ktest_check(
		ktest,
		busy != 0 && vw->state[0] == I915_VWA_SRM,
		"p6c5: P6C5-WAIT nothing happens until the breadcrumb lands and the CSB reports");

	/* The remaining steps need the scratch page. */
	if (vw->scratch[0] == NULL)
		return;

	/* Plays the engine: it stores the registers and completes. */
	results = vw->scratch[0]->cpu;
	results[0] = 0x00000008U;
	results[1] = 0x00000100U;
	results[3] = 0x00000000U;
	e0->hwsp[I915_KTEST_HWSP_SEQNO] = vw->rq[0].seqno;
	e0->csb_status[0] = I915_KTEST_CSB_PROMOTE;
	e0->csb_status[1] = I915_KTEST_CSB_COMPLETE;
	*e0->csb_write = 1U;
	busy = drv_i915_engine_verify_wa_poll(vw, 0U, &t->engines, &t->mmio);
	error = drv_i915_wa_list_check(vw, 0U, wl, "load");

	/* The stored values are compared as (cur ^ set) & read; MCR and read-mask-0 entries are not. */
	drv_i915_ktest_check(
		ktest,
		busy == 0 &&
		    vw->state[0] == I915_VWA_DONE &&
		    error == 0 &&
		    vw->verified[0] == 2U &&
		    vw->not_verifiable[0] == 1U &&
		    vw->mismatched[0] == 0U,
		"p6c5: P6C5-VERIFY stored values are compared as (cur ^ set) & read; MCR and read-mask-0 entries are not");

	/* A lost workaround. */
	results[1] = 0x00000000U;
	error = drv_i915_wa_list_check(vw, 0U, wl, "load");
	drv_i915_ktest_check(
		ktest,
		error == ENXIO &&
		    vw->mismatched[0] == 1U &&
		    vw->verified[0] == 1U,
		"p6c5: P6C5-LOST a lost workaround is -ENXIO and is counted");

	/* The park switch back to the kernel context. */
	i915_ktest_gt_verify_wa_park(ktest, t);
	drv_i915_engines_verify_wa_release(vw, &t->gm);
}

/* Checks the park switch that follows the verification. */
static void
i915_ktest_gt_verify_wa_park(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_verify_wa *vw;
	struct i915_gt_engine *e0;
	int load;
	int busy;
	int error;

	vw = &t->verify;
	e0 = &t->engines.ge[0];

	/* pm_put parks the engine with a kernel-context switch request. */
	t->fake.wt_n = 0U;
	error = drv_i915_engine_verify_wa_park(vw, 0U, &t->engines, &t->mmio);
	load = i915_fake_wt_find(&t->fake, 0x22550U, 1U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    vw->state[0] == I915_VWA_SWITCH &&
		    vw->krq[0].seqno == vw->rq[0].seqno + 1U &&
		    t->engines.el[0].wakeref_serial == t->engines.el[0].serial &&
		    load >= 0,
		"p6c5: P6C5-PARK pm_put parks the engine: a kernel-context switch request is submitted");

	/* Plays the engine: the switch completes. */
	e0->hwsp[I915_KTEST_HWSP_SEQNO] = vw->krq[0].seqno;
	e0->csb_status[2] = I915_KTEST_CSB_PROMOTE;
	e0->csb_status[3] = I915_KTEST_CSB_COMPLETE;
	*e0->csb_write = 3U;
	busy = drv_i915_engine_verify_wa_poll(vw, 0U, &t->engines, &t->mmio);

	/* The engine is idle once the switch request retires too. */
	drv_i915_ktest_check(
		ktest,
		busy == 0 && vw->state[0] == I915_VWA_PARKED,
		"p6c5: P6C5-IDLE the engine is idle once the switch request retires too");
}

/* Checks the whole verification with nothing answering, and with nothing to verify. */
static void
i915_ktest_gt_verify_wa_whole(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_verify_wa *vw;
	int error;

	vw = &t->verify;

	/* Runs the verification with nothing answering, for 1 ms. */
	error = drv_i915_engines_verify_workarounds(vw, &t->engines, &t->defaults_init, &t->gm, &t->mmio, 1U);

	/* An unanswered store request times out, reported as EIO; no park switch is queued behind it. */
	drv_i915_ktest_check(
		ktest,
		error == EIO &&
		    vw->timed_out == 1 &&
		    vw->engine_err[0] == ETIMEDOUT &&
		    vw->state[0] == I915_VWA_SRM &&
		    vw->krq[0].seqno == 0U,
		"p6c5: P6C5-TIME an unanswered SRM request is -ETIME -> -EIO; no park switch is queued behind it");
	drv_i915_engines_verify_wa_release(vw, &t->gm);

	/* Runs it on an empty list: `if (!wal->count) return 0`. */
	t->defaults_init.engine_wa[0].count = 0U;
	error = drv_i915_engines_verify_workarounds(vw, &t->engines, &t->defaults_init, &t->gm, &t->mmio, 1U);

	/* No request, no scratch page, no park. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    vw->state[0] == I915_VWA_IDLE &&
		    vw->scratch[0] == NULL &&
		    vw->polls == 0U,
		"p6c5: P6C5-EMPTY an engine without workarounds submits nothing");
	drv_i915_engines_verify_wa_release(vw, &t->gm);
}

/* Marks which of the first four list entries a ring stores to its scratch slot with a global-GTT SRM. */
static void
i915_ktest_gt_srm_find(
	const uint32_t *ring,
	unsigned ring_dwords,
	const struct i915_wa_list *wal,
	uint32_t scratch_ggtt,
	int *found)
{
	unsigned index;
	unsigned entry;

	/* Starts with nothing found. */
	for (entry = 0U; entry < 4U; entry++)
		found[entry] = 0;

	/* Walks every four-dword window of the ring. */
	for (index = 0U; index + 3U < ring_dwords; index++) {
		/* Only a global-GTT MI_STORE_REGISTER_MEM with a zero upper address is a store. */
		if (ring[index] != (MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT))
			continue;
		if (ring[index + 3U] != 0U)
			continue;

		/* Marks the entry whose register is stored to its own slot. */
		for (entry = 0U; entry < 4U; entry++) {
			if (ring[index + 1U] == wal->list[entry].reg &&
			    ring[index + 2U] == scratch_ggtt + 4U * entry)
				found[entry] = 1;
		}
	}
}

/* Checks intel_migrate_init(): the migrate VM, its PTE window and its context. */
static void
i915_ktest_gt_migrate(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_gt_migrate *mg;
	unsigned live0;
	unsigned pages0;
	unsigned live1;
	int error;

	mg = &t->migrate;

	/* Remembers what the memory held before. */
	live0 = t->gm.objects_live;
	pages0 = t->gm.allocated_pages;

	/* Sets the simulated engines up. */
	error = drv_i915_engines_init(&t->engines, &t->defaults_gt, &t->gm, &t->pp);
	drv_i915_ktest_check(ktest, error == 0, "p6c6: P6C6-INIT intel_engines_init for the migrate run");
	if (error != 0)
		return;

	live1 = t->gm.objects_live;

	/* Creates the migrate context. */
	error = drv_i915_migrate_init(mg, &t->engines, &t->gm);

	/* The first copy engine (bcs0) owns it. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    mg->inited != 0 &&
		    mg->has_engine != 0 &&
		    mg->engine_idx == 0U &&
		    mg->err == 0,
		"p6c6: P6C6-ENGINE the first copy engine (bcs0) owns the migrate context");

	/* The VM, the PTE window and the context. */
	if (error == 0)
		i915_ktest_gt_migrate_context(ktest, t);

	/* Releases the pinned context and the whole migrate VM. */
	drv_i915_migrate_fini(mg, &t->gm);
	drv_i915_ktest_check(
		ktest,
		t->gm.objects_live == live1 &&
		    mg->inited == 0 &&
		    mg->vm.n_tables == 0U,
		"p6c6: P6C6-FINI the pinned context and the whole migrate vm are released");

	/* Nothing is left behind. */
	drv_i915_engines_release(&t->engines, &t->gm);
	drv_i915_ktest_check(
		ktest,
		t->gm.objects_live == live0 && t->gm.allocated_pages == pages0,
		"p6c6: P6C6-LEAK nothing is left behind");
}

/* Checks the migrate VM's tables, its PTE window and its pinned context. */
static void
i915_ktest_gt_migrate_context(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const struct i915_gt_migrate *mg;
	const uint32_t *regs;
	int tree_ok;
	int pts_ok;
	int window_ok;

	mg = &t->migrate;

	/* Walks the tables allocate_va_range(0, 16M + 32K) built. */
	i915_ktest_gt_migrate_tree(mg, &tree_ok, &pts_ok, &window_ok);

	/* A PDP, a PD and nine PTs with cached PDEs, and scratch elsewhere. */
	drv_i915_ktest_check(
		ktest,
		tree_ok != 0 && pts_ok != 0,
		"p6c6: P6C6-VM allocate_va_range(0, 16M + 32K): PDP, PD, nine PTs (cached PDEs), scratch elsewhere");

	/* The PTE window maps the eight window page tables themselves. */
	drv_i915_ktest_check(
		ktest,
		window_ok != 0 &&
		    mg->pte_window == 2ULL * I915_MIGRATE_CHUNK_SZ &&
		    mg->exposed_pts == 8U &&
		    mg->window_bytes == 16ULL << 20,
		"p6c6: P6C6-PTE the PTE window maps the eight window page tables themselves");

	/* A pinned context with a 512 KiB ring on the migrate VM, its timeline at HWS_MIGRATE. */
	regs = mg->ce.lrc_reg_state;
	drv_i915_ktest_check(
		ktest,
		mg->ce.allocated != 0 &&
		    mg->ce.ring.size == I915_MIGRATE_RING_BYTES &&
		    mg->ce.ring.obj != NULL &&
		    mg->ce.ring.obj->bound != 0 &&
		    mg->ce.ring.obj->contiguous != 0 &&
		    mg->ce.state != NULL &&
		    mg->ce.state->bound != 0 &&
		    mg->ce.vm == &mg->vm &&
		    regs[CTX_PDP0_LDW] == (uint32_t)mg->vm.top_pd_dma &&
		    regs[CTX_PDP0_UDW] == (uint32_t)(mg->vm.top_pd_dma >> 32) &&
		    regs[CTX_RING_CTL] == ((I915_MIGRATE_RING_BYTES - 4096U) | 1U) &&
		    mg->hwsp_ggtt == (uint32_t)t->engines.ge[0].hwsp_ggtt + 0x108U &&
		    mg->hwsp_cpu == &t->engines.ge[0].hwsp[0x42U] &&
		    mg->tl_seqno == 0U,
		"p6c6: P6C6-CTX a pinned 512 KiB-ring context on the migrate vm, timeline at HWS_MIGRATE");
}

/* Reports whether the migrate VM's table list and entries are what allocate_va_range() and insert_pte() leave. */
static void
i915_ktest_gt_migrate_tree(
	const struct i915_gt_migrate *mg,
	int *tree_ok,
	int *pts_ok,
	int *window_ok)
{
	const struct i915_gt_ppgtt_table *tables;
	unsigned index;

	tables = mg->vm.tables;

	/* The list starts with the PDP under PML4[0] and the PD under PDP[0]. */
	*tree_ok = 0;
	if (mg->vm.n_tables == 11U &&
	    tables[0].lvl == 2 &&
	    tables[0].parent == mg->vm.top_pd &&
	    tables[0].idx == 0U &&
	    tables[1].lvl == 1 &&
	    tables[1].parent == tables[0].obj &&
	    tables[1].idx == 0U)
		*tree_ok = 1;

	/* Nine PTs follow, under PD entries 0 to 8. */
	*pts_ok = 1;
	*window_ok = 1;
	for (index = 0U; index < 9U; index++) {
		/* The walk is only meaningful on a list that starts right. */
		if (*tree_ok == 0)
			break;

		/* A table that is not the PT expected at this index. */
		if (tables[2U + index].lvl != 0 ||
		    tables[2U + index].parent != tables[1].obj ||
		    tables[2U + index].idx != index)
			*pts_ok = 0;
	}

	/* The entries are read only when the list is right. */
	if (*tree_ok != 0 && *pts_ok != 0)
		i915_ktest_gt_migrate_tables(mg, tree_ok, window_ok);
}

/* Reports whether the migrate VM's directory entries and PTE window name the right tables. */
static void
i915_ktest_gt_migrate_tables(
	const struct i915_gt_migrate *mg,
	int *tree_ok,
	int *window_ok)
{
	const struct i915_gt_ppgtt_table *tables;
	const uint64_t *pml4;
	const uint64_t *pdp;
	const uint64_t *pd;
	const uint64_t *pt0;
	const uint64_t *win;
	const uint64_t *scratch;
	uint64_t pdp_entry;
	uint64_t pd_entry;
	uint64_t pt_entry;
	uint64_t window_entry;
	unsigned index;

	tables = mg->vm.tables;
	scratch = mg->vm.scratch_encode;
	pml4 = mg->vm.top_pd->cpu;
	pdp = tables[0].obj->cpu;
	pd = tables[1].obj->cpu;
	pt0 = tables[2].obj->cpu;
	win = tables[10].obj->cpu;

	/* PML4[0] and PDP[0] name the PDP and the PD cached; everything past them is scratch. */
	pdp_entry = drv_i915_gen8_pde_encode_cached(tables[0].dma);
	pd_entry = drv_i915_gen8_pde_encode_cached(tables[1].dma);
	*tree_ok = 0;
	if (pml4[0] == pdp_entry &&
	    pml4[1] == scratch[3] &&
	    pdp[0] == pd_entry &&
	    pdp[1] == scratch[2] &&
	    pd[9] == scratch[1] &&
	    pt0[0] == scratch[0] &&
	    pt0[511] == scratch[0])
		*tree_ok = 1;

	/* PD entries 0 to 8 name the nine PTs cached. */
	for (index = 0U; index < 9U; index++) {
		pt_entry = drv_i915_gen8_pde_encode_cached(tables[2U + index].dma);
		if (pd[index] != pt_entry)
			*tree_ok = 0;
	}

	/* insert_pte(): PT k of the windows sits at 16M + 4K * k, uncached. */
	for (index = 0U; index < 8U; index++) {
		window_entry = drv_i915_gen12_ppgtt_pte_encode(tables[2U + index].dma, I915_PAT_INDEX_CACHE_NONE);
		if (win[index] != window_entry)
			*window_ok = 0;
	}

	/* The entry after the window is scratch. */
	if (win[8] != scratch[0])
		*window_ok = 0;
}

/* Checks the hotplug pins of the DDI ports and the hotplug interrupt setup. */
static void
i915_ktest_gt_hotplug(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	int pin_a;
	int pin_b;
	int pin_tc1;
	int pin_d;
	int pin_tc1_ver12;

	/* intel_ddi_init()'s hotplug pin per port. */
	pin_a = drv_i915_ddi_hpd_pin(13, 0);
	pin_b = drv_i915_ddi_hpd_pin(13, 1);
	pin_tc1 = drv_i915_ddi_hpd_pin(13, 3);
	pin_d = drv_i915_ddi_hpd_pin(13, 7);
	pin_tc1_ver12 = drv_i915_ddi_hpd_pin(12, 3);
	drv_i915_ktest_check(
		ktest,
		pin_a == I915_HPD_PORT_A &&
		    pin_b == I915_HPD_PORT_B &&
		    pin_tc1 == I915_HPD_PORT_TC1 &&
		    pin_d == I915_HPD_PORT_D &&
		    pin_tc1_ver12 == I915_HPD_PORT_TC1,
		"p7: P7-HPD-PIN DDI A/B default pins, TC1+ from PORT_TC1 (ver 12+), D/E from PORT_D_XELPD (ver 13)");

	/* The Gen11 and ICP registers for encoders on A and B. */
	i915_ktest_gt_hotplug_setup(ktest, t);
}

/* Checks the Gen11 and ICP hotplug registers for encoders on ports A and B. */
static void
i915_ktest_gt_hotplug_setup(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_hotplug *hp;
	uint32_t shotplug_ddi;
	uint32_t shotplug_tc;
	uint32_t tc_ctl;
	uint32_t tbt_ctl;
	uint32_t sdeimr;
	int filter;
	int de_hpd_imr;
	int sdeimr_write;

	hp = &t->hotplug;

	/* Two encoders, on the pins of ports A and B. */
	kern_memset(hp, 0, sizeof(*hp));
	hp->encoder_pin[0] = I915_HPD_PORT_A;
	hp->encoder_pin[1] = I915_HPD_PORT_B;
	hp->n_encoders = 2U;

	/* Registers the firmware left with every port enabled and every interrupt masked. */
	i915_fake_open(&t->mmio, &t->fake);
	i915_fake_gen_set(&t->fake, I915_SHOTPLUG_CTL_DDI, 0x8888U);
	i915_fake_gen_set(&t->fake, I915_SHOTPLUG_CTL_TC, 0x00888888U);
	i915_fake_gen_set(&t->fake, I915_GEN11_TC_HOTPLUG_CTL, 0x00888888U);
	i915_fake_gen_set(&t->fake, I915_GEN11_TBT_HOTPLUG_CTL, 0x00888888U);
	i915_fake_gen_set(&t->fake, I915_SDEIMR, 0xffffffffU);
	i915_fake_gen_set(&t->fake, I915_GEN11_DE_HPD_IMR, 0xffffffffU);

	/* Runs intel_hpd_init() with display and driver interrupts enabled. */
	t->fake.wt_n = 0U;
	drv_i915_hpd_init(hp, &t->mmio, 13, I915_PCH_ADP, 1, 1);

	/* A and B enabled in SHOTPLUG_CTL_DDI, TC and TBT cleared, SDEIMR unmasks A and B, filter 250, DE HPD IMR untouched. */
	shotplug_ddi = i915_fake_gen_get(&t->fake, I915_SHOTPLUG_CTL_DDI);
	shotplug_tc = i915_fake_gen_get(&t->fake, I915_SHOTPLUG_CTL_TC);
	tc_ctl = i915_fake_gen_get(&t->fake, I915_GEN11_TC_HOTPLUG_CTL);
	tbt_ctl = i915_fake_gen_get(&t->fake, I915_GEN11_TBT_HOTPLUG_CTL);
	sdeimr = i915_fake_gen_get(&t->fake, I915_SDEIMR);
	filter = i915_fake_wt_find(&t->fake, I915_SHPD_FILTER_CNT, I915_SHPD_FILTER_CNT_250, 0xffffffffU);
	de_hpd_imr = i915_fake_wt_find(&t->fake, I915_GEN11_DE_HPD_IMR, 0U, 0U);
	drv_i915_ktest_check(
		ktest,
		hp->irq_setups == 1U &&
		    hp->state[I915_HPD_PORT_A] == I915_HPD_ENABLED &&
		    hp->de_hotplug_irqs == 0U &&
		    hp->de_enabled_irqs == 0U &&
		    hp->pch_hotplug_irqs == 0x30000U &&
		    hp->pch_enabled_irqs == 0x30000U &&
		    shotplug_ddi == 0x88U &&
		    shotplug_tc == 0U &&
		    tc_ctl == 0U &&
		    tbt_ctl == 0U &&
		    sdeimr == 0xfffcffffU &&
		    filter >= 0 &&
		    de_hpd_imr < 0 &&
		    hp->sdeimr_skipped == 0,
		"p7: P7-HPD-SETUP DDI A/B enabled in SHOTPLUG_CTL_DDI, TC/TBT cleared, SDEIMR unmasks A/B, filter 250, DE HPD IMR untouched");

	/* Runs it again before intel_irq_install(): the driver's interrupts are not enabled yet. */
	i915_fake_gen_set(&t->fake, I915_SDEIMR, 0xffffffffU);
	t->fake.wt_n = 0U;
	drv_i915_hpd_init(hp, &t->mmio, 13, I915_PCH_ADP, 1, 0);

	/* ibx_display_interrupt_update() refuses: SDEIMR is not written. */
	sdeimr = i915_fake_gen_get(&t->fake, I915_SDEIMR);
	sdeimr_write = i915_fake_wt_find(&t->fake, I915_SDEIMR, 0U, 0U);
	drv_i915_ktest_check(
		ktest,
		hp->sdeimr_skipped == 1 &&
		    sdeimr == 0xffffffffU &&
		    sdeimr_write < 0,
		"p7: P7-HPD-NOIRQ ibx_display_interrupt_update() refuses while intel_irqs_enabled() is false");

	/* Runs it with display interrupts disabled: hpd_irq_setup is skipped. */
	drv_i915_hpd_init(hp, &t->mmio, 13, I915_PCH_ADP, 0, 1);
	drv_i915_ktest_check(ktest, hp->irq_setup_skipped == 1, "p7: P7-HPD-NODISP hpd_irq_setup needs display_irqs_enabled");
}

/* Checks that the DC_off power well's disable enters the target DC state and its enable leaves it. */
static void
i915_ktest_gt_dc6(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_display *display;
	struct i915_power_well well;
	struct i915_pw_ctx *pwc;
	uint32_t dc_state;
	int enabled;

	/*
	 * The DC_off enable finds its trace ring through the display that
	 * embeds the well context, so the context must sit in a display.  A
	 * private, zeroed display serves; the started device's is not touched.
	 */
	display = kern_calloc(1U, sizeof(*display));
	if (display == NULL) {
		drv_i915_ktest_skip(ktest, "p7: P7-DC6-NODMC", "no memory for a private display");
		drv_i915_ktest_skip(ktest, "p7: P7-DC6", "no memory for a private display");
		drv_i915_ktest_skip(ktest, "p7: P7-DC6-OFF", "no memory for a private display");
		return;
	}

	/* The DC_off well of an XE_LPD display that allows DC5 and DC6 and targets DC6. */
	kern_memset(&well, 0, sizeof(well));
	well.name = "DC_off";
	well.ops = I915_PW_OPS_DC_OFF;
	pwc = &display->pwc;
	pwc->mmio = &t->mmio;
	pwc->display_ver = 13;
	pwc->allowed_dc_mask = 0x4000000aU;
	pwc->target_dc_state = I915_DC_STATE_EN_UPTO_DC6;
	i915_fake_gen_set(&t->fake, I915_KTEST_REG_DC_STATE_EN, 0U);

	/* Disables the well without a DMC payload. */
	pwc->dmc_has_payload = 0;
	(void)drv_i915_power_well_disable(&well, pwc);

	/* No DC state is entered and the well stays on. */
	dc_state = i915_fake_gen_get(&t->fake, I915_KTEST_REG_DC_STATE_EN);
	drv_i915_ktest_check(
		ktest,
		dc_state == 0U &&
		    pwc->dc_state_writes == 0U &&
		    well.hw_enabled == 1,
		"p7: P7-DC6-NODMC without a DMC payload no DC state is enabled and the well stays on");

	/* Disables it with the DMC payload loaded. */
	pwc->dmc_has_payload = 1;
	(void)drv_i915_power_well_disable(&well, pwc);

	/* skl_enable_dc6(): DC_STATE_EN gets UPTO_DC6 and the well reads as off. */
	dc_state = i915_fake_gen_get(&t->fake, I915_KTEST_REG_DC_STATE_EN);
	enabled = drv_i915_power_well_is_enabled(&well, pwc);
	drv_i915_ktest_check(
		ktest,
		dc_state == I915_DC_STATE_EN_UPTO_DC6 &&
		    pwc->dc_state == I915_DC_STATE_EN_UPTO_DC6 &&
		    pwc->dc_state_writes == 1U &&
		    well.hw_enabled == 0 &&
		    enabled == 0,
		"p7: P7-DC6 skl_enable_dc6: DC_STATE_EN gets UPTO_DC6, the well reads as off");

	/* Enables the well again. */
	well.hw_enabled = 0;
	(void)drv_i915_power_well_enable(&well, pwc);

	/* The DC states are cleared. */
	dc_state = i915_fake_gen_get(&t->fake, I915_KTEST_REG_DC_STATE_EN);
	enabled = drv_i915_power_well_is_enabled(&well, pwc);
	drv_i915_ktest_check(
		ktest,
		(dc_state & 0x40000003U) == 0U && enabled == 1,
		"p7: P7-DC6-OFF enabling the well again clears the DC states");

	kern_free(display);
}

/* Checks pxp_init_full(): a pinned context on the first video engine and the streaming page. */
static void
i915_ktest_gt_pxp(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_engine_info *engine;
	struct i915_pxp *px;
	unsigned live1;
	int error;

	px = &t->pxp;

	/* Adds the first video decode engine to the simulated GT. */
	engine = &t->defaults_gt.engines[1];
	engine->id = I915_VCS0;
	engine->class = I915_VIDEO_DECODE_CLASS;
	engine->instance = 0;
	engine->mmio_base = 0x1c0000U;
	engine->context_size = 2U * 4096U;
	engine->name = "vcs0";
	t->defaults_gt.num_engines = 2U;

	/* Sets the engines up on a clean model. */
	i915_fake_open(&t->mmio, &t->fake);
	error = drv_i915_engines_init(&t->engines, &t->defaults_gt, &t->gm, &t->pp);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "p7: P7-PXP intel_engines_init with a VCS failed");
		t->defaults_gt.num_engines = 1U;
		return;
	}

	live1 = t->gm.objects_live;

	/* Without has_pxp there is no PXP GT. */
	kern_memset(px, 0, sizeof(*px));
	error = drv_i915_pxp_init(px, &t->engines, &t->pp, &t->gm, 0);
	drv_i915_ktest_check(ktest, error == ENODEV && px->inited == 0, "p7: P7-PXP-NONE without has_pxp there is no PXP GT");

	/* With it, the full feature. */
	error = drv_i915_pxp_init(px, &t->engines, &t->pp, &t->gm, 1);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    px->inited != 0 &&
		    px->full_feature != 0 &&
		    px->engine_idx == 1U &&
		    px->kcr_base == 0x32000U &&
		    px->ce.allocated != 0 &&
		    px->ce.ring.size == 4096U &&
		    px->ce.vm == &t->pp &&
		    px->stream_cmd != NULL &&
		    px->component_added != 0 &&
		    px->hwsp_ggtt == (uint32_t)t->engines.ge[1].hwsp_ggtt + 0x180U &&
		    px->hwsp_cpu == &t->engines.ge[1].hwsp[0x60U],
		"p7: P7-PXP pxp_init_full: a pinned 4 KiB context on the first VCS, HWS_PXP timeline, streaming page");

	/* destroy_vcs_context() and the streaming page. */
	drv_i915_pxp_fini(px, &t->gm);
	drv_i915_ktest_check(
		ktest,
		t->gm.objects_live == live1 &&
		    px->inited == 0 &&
		    px->stream_cmd == NULL,
		"p7: P7-PXP-FINI destroy_vcs_context + streaming page released");

	drv_i915_engines_release(&t->engines, &t->gm);
	t->defaults_gt.num_engines = 1U;
}

/* Checks the order of the compute batch's packets. */
static void
i915_ktest_gt_eu_batch(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const uint32_t *cmds;
	unsigned count;
	unsigned index;
	unsigned walker;
	unsigned sba;
	unsigned sel3d;
	unsigned selgpgpu;
	unsigned midl;
	unsigned vfe;

	cmds = t->batch_a;

	/* Builds the compute batch. */
	kern_memset(t->batch_a, 0, sizeof(t->batch_a));
	count = drv_i915_test_eu_build_batch(t->batch_a, I915_KTEST_BATCH_DWORDS, I915_TEST_EU_SHARED_VA, I915_TEST_EU_SHARED_VA, I915_KTEST_EU_MAX_THREADS);

	/* Finds the last position of each packet. */
	walker = 0U;
	sba = 0U;
	sel3d = 0U;
	selgpgpu = 0U;
	midl = 0U;
	vfe = 0U;
	for (index = 0U; index + 14U < count; index++) {
		/* GPGPU_WALKER with a 1x1x1 grid of SIMD8 threads and a full right mask. */
		if (cmds[index] == 0x7105000dU &&
		    cmds[index + 7U] == 1U &&
		    cmds[index + 10U] == 1U &&
		    cmds[index + 12U] == 1U &&
		    cmds[index + 13U] == 1U &&
		    cmds[index + 14U] == 0xffffffffU)
			walker = index;

		/* STATE_BASE_ADDRESS: stateless MOCS 6, instruction base at 0x100400000. */
		if (cmds[index] == 0x61010014U &&
		    cmds[index + 3U] == (6U << 16) &&
		    cmds[index + 4U] == (1U | (6U << 4) | 0x00400000U) &&
		    cmds[index + 5U] == 1U &&
		    cmds[index + 10U] == (1U | (4U << 4) | 0x00400000U) &&
		    cmds[index + 11U] == 1U)
			sba = index;

		/* PIPELINE_SELECT(3D), Linux encoding. */
		if (cmds[index] == 0x69041310U)
			sel3d = index;

		/* PIPELINE_SELECT(GPGPU). */
		if (cmds[index] == 0x69041312U)
			selgpgpu = index;

		/* MEDIA_INTERFACE_DESCRIPTOR_LOAD of 32 bytes at 896. */
		if (cmds[index] == 0x70020002U &&
		    cmds[index + 2U] == 32U &&
		    cmds[index + 3U] == 896U)
			midl = index;

		/* MEDIA_VFE_STATE with 559 threads. */
		if (cmds[index] == 0x70000007U && cmds[index + 3U] == ((559U << 16) | (2U << 8)))
			vfe = index;
	}

	/* The packets come in order and the batch ends. */
	drv_i915_ktest_check(
		ktest,
		count >= 2U &&
		    count < I915_KTEST_BATCH_DWORDS &&
		    sel3d < sba &&
		    sba < selgpgpu &&
		    selgpgpu < vfe &&
		    vfe < midl &&
		    midl < walker &&
		    cmds[count - 2U] == MI_BATCH_BUFFER_END,
		"eu: EU-BATCH 3D select, SBA (stateless MOCS 6, instruction base @0x100400000), GPGPU select, VFE(559), MIDL(896), walker(1x1x1 SIMD8), BB_END");

	/* The PIPELINE_SELECT words against fixed reference words. */
	i915_ktest_gt_eu_pipeline_select(ktest, t, count);
}

/* Checks the compute batch's PIPELINE_SELECT words, and that the old words are rejected. */
static void
i915_ktest_gt_eu_pipeline_select(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t,
	unsigned count)
{
	struct i915_test_pipeline_select check;
	const uint32_t *cmds;
	uint32_t *bad;
	uint32_t word3d;
	uint32_t wordgpgpu;
	uint32_t bad_word;
	int error;

	cmds = t->batch_a;
	bad = t->batch_b;

	/* Classifies the batch's select words; independent of the emitter's macro. */
	kern_memset(&check, 0, sizeof(check));
	error = drv_i915_test_eu_check_pipeline_select(cmds, count, &check);
	word3d = 0U;
	if (check.idx_3d < count)
		word3d = cmds[check.idx_3d];

	wordgpgpu = 0U;
	if (check.idx_gpgpu < count)
		wordgpgpu = cmds[check.idx_gpgpu];

	/* The emitter's words are exactly 0x69041310 (3D) then 0x69041312 (GPGPU). */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    check.n_3d == 1U &&
		    check.n_gpgpu == 1U &&
		    check.n_bad == 0U &&
		    word3d == 0x69041310U &&
		    wordgpgpu == 0x69041312U,
		"eu: EU-PIPESEL the emitter's words are exactly 0x69041310 (3D) then 0x69041312 (GPGPU), Type3/SubType1/Op1/SubOp4");

	/* Turns both select words back into the old, mis-encoded ones. */
	kern_memset(t->batch_b, 0, sizeof(t->batch_b));
	kern_memcpy(bad, cmds, count * sizeof(uint32_t));
	if (check.idx_3d < count)
		bad[check.idx_3d] ^= 0x08000000U;
	if (check.idx_gpgpu < count)
		bad[check.idx_gpgpu] ^= 0x08000000U;

	/* Classifies the altered batch. */
	kern_memset(&check, 0, sizeof(check));
	error = drv_i915_test_eu_check_pipeline_select(bad, count, &check);
	bad_word = 0U;
	if (check.idx_bad < count)
		bad_word = bad[check.idx_bad];

	/* 0x61041310/0x61041312 (the GPGPU_CSR_BASE_ADDRESS header) are rejected. */
	drv_i915_ktest_check(
		ktest,
		bad_word == 0x61041310U &&
		    error != 0 &&
		    check.n_bad == 2U &&
		    check.n_3d == 0U &&
		    check.n_gpgpu == 0U,
		"eu: EU-PIPESEL 0x61041310/0x61041312 (the GPGPU_CSR_BASE_ADDRESS header) are rejected as Gen12 PIPELINE_SELECT");
}

/* Checks the single-colour RECTLIST draw batch against fixed reference words. */
static void
i915_ktest_gt_draw_batch(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_test_pipeline_select check;
	const uint32_t *cmds;
	uint32_t *bad;
	uint32_t mocs;
	unsigned count;
	unsigned index;
	unsigned prim;
	unsigned nprim;
	int error;

	cmds = t->batch_a;
	bad = t->batch_b;

	/* Builds the draw batch with the fixture's MOCS and classifies its select words. */
	kern_memset(t->batch_a, 0, sizeof(t->batch_a));
	mocs = drv_i915_draw_fixture_mocs();
	count = drv_i915_draw_fixture_build_batch(t->batch_a, I915_KTEST_BATCH_DWORDS, I915_DRAW_FIXTURE_STATE_VA, mocs);
	kern_memset(&check, 0, sizeof(check));
	error = drv_i915_test_draw_check_pipeline_select(cmds, count, &check);

	/* Finds the 3DPRIMITIVE: RECTLIST, 3 vertices, 1 instance. */
	prim = 0U;
	nprim = 0U;
	for (index = 0U; index + 6U < count; index++) {
		if (cmds[index] == 0x7b000005U &&
		    cmds[index + 1U] == 15U &&
		    cmds[index + 2U] == 3U &&
		    cmds[index + 4U] == 1U) {
			prim = index;
			nprim++;
		}
	}

	/* PIPE_CONTROL, PIPELINE_SELECT(3D) at dword 6, SBA, one 3DPRIMITIVE, BB_END. */
	drv_i915_ktest_check(
		ktest,
		count > 64U &&
		    count < I915_KTEST_BATCH_DWORDS &&
		    mocs == 6U &&
		    error == 0 &&
		    check.n_3d == 1U &&
		    check.idx_3d == 6U &&
		    cmds[6] == 0x69041310U &&
		    cmds[7] == 0x61010014U &&
		    check.n_gpgpu == 0U &&
		    check.n_bad == 0U &&
		    nprim == 1U &&
		    prim > check.idx_3d &&
		    cmds[count - 2U] == MI_BATCH_BUFFER_END,
		"draw: DRAW-BATCH PIPE_CONTROL, PIPELINE_SELECT(3D)=0x69041310 at dword 6, SBA, one 3DPRIMITIVE RECTLIST(3 vertices, 1 instance), BB_END");

	/* Turns the select word back into the old, mis-encoded one. */
	kern_memset(t->batch_b, 0, sizeof(t->batch_b));
	kern_memcpy(bad, cmds, count * sizeof(uint32_t));
	bad[6] ^= 0x08000000U;
	kern_memset(&check, 0, sizeof(check));
	error = drv_i915_test_draw_check_pipeline_select(bad, count, &check);

	/* 0x61041310 (the GPGPU_CSR_BASE_ADDRESS header) is rejected. */
	drv_i915_ktest_check(
		ktest,
		bad[6] == 0x61041310U &&
		    error != 0 &&
		    check.n_bad == 1U &&
		    check.n_3d == 0U,
		"draw: DRAW-PIPESEL 0x61041310 (the GPGPU_CSR_BASE_ADDRESS header) is rejected as Gen12 PIPELINE_SELECT");

	/* The state page the batch names. */
	i915_ktest_gt_draw_state(ktest, t, mocs);
}

/* Checks the single-colour draw's state page. */
static void
i915_ktest_gt_draw_state(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t,
	uint32_t mocs)
{
	const uint32_t *state;

	state = t->state_a;

	/* Writes the state page for a render target at 0x100402000. */
	kern_memset(t->state_a, 0, sizeof(t->state_a));
	drv_i915_draw_fixture_write_state(t->state_a, 0x100402000ULL, mocs);

	/* Binding table -> surface state -> RT; the PS kernel at +1024 with its A64 marker store; markers clear. */
	drv_i915_ktest_check(
		ktest,
		state[0] == 64U &&
		    state[64U / 4U + 8U] == 0x00402000U &&
		    state[64U / 4U + 9U] == 1U &&
		    state[I915_DRAW_FIXTURE_PS_OFFSET / 4U] == 0x80000061U &&
		    state[I915_DRAW_FIXTURE_PS_OFFSET / 4U + 7U] == I915_DRAW_FIXTURE_PS_MARKER &&
		    state[I915_DRAW_FIXTURE_PS_OFFSET / 4U + 31U] == 0x00400c10U &&
		    state[I915_DRAW_FIXTURE_MARKER_OFFSET / 4U] == 0U,
		"draw: DRAW-STATE binding table -> surface state -> RT VA 0x100402000, PS kernel at +1024 with its A64 marker store to 0x100400c10, markers clear");
}

/* Checks that the compute and draw batches are byte-identical to the ones that passed on hardware. */
static void
i915_ktest_gt_pins(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	unsigned compute_count;
	unsigned draw_count;
	uint64_t compute_hash;
	uint64_t draw_hash;

	/* Builds both batches into clean buffers. */
	kern_memset(t->batch_a, 0, sizeof(t->batch_a));
	kern_memset(t->batch_b, 0, sizeof(t->batch_b));
	compute_count = drv_i915_test_eu_build_batch(t->batch_a, I915_KTEST_BATCH_DWORDS, I915_TEST_EU_SHARED_VA, I915_TEST_EU_SHARED_VA, I915_KTEST_EU_MAX_THREADS);
	draw_count = drv_i915_draw_fixture_build_batch(t->batch_b, I915_KTEST_BATCH_DWORDS, I915_DRAW_FIXTURE_STATE_VA, 6U);

	/* Hashes their bytes. */
	compute_hash = i915_ktest_gt_fnv(t->batch_a, compute_count * 4U);
	draw_hash = i915_ktest_gt_fnv(t->batch_b, draw_count * 4U);

	/* The compute batch: 322 dwords, FNV 5dfb47d3c10b0560. */
	drv_i915_ktest_check(
		ktest,
		compute_count == 322U && compute_hash == 0x5dfb47d3c10b0560ULL,
		"pin: PIN-C1 the C1 batch is byte-identical to the one that passed on hardware (E-99: 322 dwords, FNV 5dfb47d3c10b0560)");

	/* The single-colour draw batch: 353 dwords, FNV 241f478201bb3a81. */
	drv_i915_ktest_check(
		ktest,
		draw_count == 353U && draw_hash == 0x241f478201bb3a81ULL,
		"pin: PIN-DRAW the single-colour draw batch is byte-identical to the one that passed on hardware (353 dwords, FNV 241f478201bb3a81)");
}

/* Computes the 64-bit FNV-1a hash of a byte run. */
static uint64_t
i915_ktest_gt_fnv(
	const void *bytes,
	unsigned length)
{
	const uint8_t *byte;
	uint64_t hash;
	unsigned index;

	byte = bytes;

	/* Folds every byte in. */
	hash = I915_KTEST_FNV_OFFSET;
	for (index = 0U; index < length; index++) {
		hash ^= byte[index];
		hash *= I915_KTEST_FNV_PRIME;
	}

	/* Reports the hash. */
	return hash;
}

/* Checks the textured draw batch against the single-colour one. */
static void
i915_ktest_gt_tex_batch(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_test_pipeline_select check;
	const uint32_t *tex;
	const uint32_t *draw;
	unsigned tex_count;
	unsigned draw_count;
	unsigned index;
	unsigned draw_index;
	unsigned changed;
	unsigned ssp;
	unsigned nssp;
	unsigned ps;
	unsigned psx;
	int error;

	tex = t->batch_a;
	draw = t->batch_b;

	/* Builds the textured and the single-colour batch and classifies the textured one's select words. */
	kern_memset(t->batch_a, 0, sizeof(t->batch_a));
	kern_memset(t->batch_b, 0, sizeof(t->batch_b));
	tex_count = drv_i915_tex_fixture_build_batch(t->batch_a, I915_KTEST_BATCH_DWORDS, I915_DRAW_FIXTURE_STATE_VA, 6U);
	draw_count = drv_i915_draw_fixture_build_batch(t->batch_b, I915_KTEST_BATCH_DWORDS, I915_DRAW_FIXTURE_STATE_VA, 6U);
	kern_memset(&check, 0, sizeof(check));
	error = drv_i915_test_draw_check_pipeline_select(tex, tex_count, &check);

	/* Finds the sampler state pointers, 3DSTATE_PS and 3DSTATE_PS_EXTRA. */
	ssp = 0U;
	nssp = 0U;
	ps = 0U;
	psx = 0U;
	for (index = 0U; index + 1U < tex_count; index++) {
		/* 3DSTATE_SAMPLER_STATE_POINTERS_PS naming 896. */
		if (tex[index] == 0x782f0000U && tex[index + 1U] == 896U) {
			ssp = index;
			nssp++;
		}

		/* 3DSTATE_PS. */
		if (tex[index] == 0x7820000aU)
			ps = index;

		/* 3DSTATE_PS_EXTRA. */
		if (tex[index] == 0x784f0000U)
			psx = index;
	}

	/* Compares the textured batch without the sampler packet against the single-colour batch. */
	changed = 0U;
	draw_index = 0U;
	for (index = 0U; index < tex_count; index++) {
		/* The single-colour batch is used up. */
		if (draw_index >= draw_count)
			break;

		/* The sampler packet has no counterpart. */
		if (index == ssp || index == ssp + 1U)
			continue;

		/* Counts a changed dword. */
		if (tex[index] != draw[draw_index])
			changed++;

		draw_index++;
	}

	/* The draw batch plus one two-dword packet, and exactly three changed dwords. */
	drv_i915_ktest_check(
		ktest,
		tex_count == draw_count + 2U &&
		    error == 0 &&
		    check.n_3d == 1U &&
		    tex[6] == 0x69041310U &&
		    nssp == 1U &&
		    ps != 0U &&
		    tex[ps + 1U] == 1024U &&
		    tex[ps + 3U] == 0x08080000U &&
		    tex[ps + 7U] == (4U << 16) &&
		    psx != 0U &&
		    tex[psx + 1U] == 0x81800004U &&
		    changed == 3U,
		"tex: TEX-BATCH = the single-colour draw batch + 3DSTATE_SAMPLER_STATE_POINTERS_PS(896), and exactly three changed dwords: 3DSTATE_PS DW3 (sampler count 1 / binding table 2), DW7 (GRF start 4), 3DSTATE_PS_EXTRA DW1 (valid|UAV|source depth|source W) as the compiler's prog_data requires");
}

/* Checks the textured draw's state page. */
static void
i915_ktest_gt_tex_state(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const uint32_t *state;
	uint64_t ps_hash;

	state = t->state_a;

	/* Writes the state page. */
	kern_memset(t->state_a, 0, sizeof(t->state_a));
	drv_i915_tex_fixture_write_state(t->state_a, 0x100402000ULL, I915_TEX_FIXTURE_TEX_VA, 6U);

	/*
	 * Hashes the 640 kernel bytes in the state page, to compare with the
	 * FNV-1a of the generator's raw reftex_ps.bin (computed on the host,
	 * not from the table this kernel was built with).
	 */
	ps_hash = i915_ktest_gt_fnv((const uint8_t *)t->state_a + I915_DRAW_FIXTURE_PS_OFFSET, 640U);

	/* BT[0] -> RT state, BT[1] -> texture state, the sampler at +896, the sampling PS at +1024, markers clear. */
	drv_i915_ktest_check(
		ktest,
		state[0] == 64U &&
		    state[1] == 128U &&
		    state[16U + 8U] == 0x00402000U &&
		    state[16U + 9U] == 1U &&
		    state[32U] == 0x231d4000U &&
		    (state[32U + 1U] >> 24) == 0x86U &&
		    state[32U + 2U] == 0x00070007U &&
		    state[32U + 3U] == 31U &&
		    state[32U + 8U] == 0x00404000U &&
		    state[32U + 9U] == 1U &&
		    state[896U / 4U] == 0x10000000U &&
		    state[896U / 4U + 3U] == 0x92U &&
		    ps_hash == 0x20ff9c926f6324c1ULL &&
		    state[768] == 0U,
		"tex: TEX-STATE BT[0]->RT state, BT[1]->texture state (2D R8G8B8A8_UNORM 8x8, pitch 32, MOCS 6, VA 0x100404000), SAMPLER_STATE nearest/clamp at +896, sampling PS at +1024, markers clear");
}

/* Checks the nearest-sampling expectation of the textured draw. */
static void
i915_ktest_gt_tex_expect(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const uint8_t *pat0;
	uint32_t at_xy;
	uint32_t at_yx;
	uint32_t origin;
	uint32_t texel_1_0;
	uint32_t texel_1_0_far;
	uint32_t texel_0_1;
	uint32_t corner;
	unsigned x;
	unsigned y;
	unsigned asym;
	int differ;

	pat0 = t->pattern[0];

	/* Builds images 0 and 1. */
	drv_i915_tex_fixture_pattern(t->pattern[0], 0U);
	drv_i915_tex_fixture_pattern(t->pattern[1], 1U);

	/* Counts the pixels whose transposed pixel differs. */
	asym = 0U;
	for (y = 0U; y < 32U; y++) {
		for (x = 0U; x < 32U; x++) {
			at_xy = drv_i915_tex_fixture_expected_pixel(pat0, x, y);
			at_yx = drv_i915_tex_fixture_expected_pixel(pat0, y, x);
			if (at_xy != at_yx)
				asym++;
		}
	}

	/* Reads the documented pixels; texel (1,0) = R48 G16 B48 is dword 0xff301030, texel (0,1) = R16 G48 B112. */
	origin = drv_i915_tex_fixture_expected_pixel(pat0, 0U, 0U);
	texel_1_0 = drv_i915_tex_fixture_expected_pixel(pat0, 4U, 0U);
	texel_1_0_far = drv_i915_tex_fixture_expected_pixel(pat0, 7U, 3U);
	texel_0_1 = drv_i915_tex_fixture_expected_pixel(pat0, 0U, 4U);
	corner = drv_i915_tex_fixture_expected_pixel(pat0, 31U, 31U);
	differ = kern_memcmp(t->pattern[0], t->pattern[1], I915_TEX_FIXTURE_TEX_BYTES);

	/* expected(x,y) = texel(x/4, y/4) packed B,G,R,A; origin upper left; the image identifies position. */
	drv_i915_ktest_check(
		ktest,
		origin == 0xff101010U &&
		    texel_1_0 == 0xff301030U &&
		    texel_1_0_far == 0xff301030U &&
		    texel_0_1 == 0xff103070U &&
		    corner == 0xfff0f090U &&
		    asym > 512U &&
		    differ != 0,
		"tex: TEX-EXPECT expected(x,y) = texel(x/4, y/4) packed B,G,R,A; origin upper-left; the pattern identifies position and is not symmetric in x/y");
}

/* Checks the binding switch between textures A and B. */
static void
i915_ktest_gt_tex_ab(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const uint32_t *sa;
	const uint32_t *sb;
	unsigned index;
	unsigned ndiff;

	sa = t->state_a;
	sb = t->state_b;

	/* Writes the state page with A bound and with B bound. */
	kern_memset(t->state_a, 0, sizeof(t->state_a));
	kern_memset(t->state_b, 0, sizeof(t->state_b));
	drv_i915_tex_fixture_write_state_ab(t->state_a, 0x100402000ULL, I915_TEX_FIXTURE_TEX_VA, I915_TEX_FIXTURE_TEX_B_VA, 0U, 6U);
	drv_i915_tex_fixture_write_state_ab(t->state_b, 0x100402000ULL, I915_TEX_FIXTURE_TEX_VA, I915_TEX_FIXTURE_TEX_B_VA, 1U, 6U);

	/* Counts the dwords that differ. */
	ndiff = 0U;
	for (index = 0U; index < I915_KTEST_BATCH_DWORDS; index++) {
		if (sa[index] != sb[index])
			ndiff++;
	}

	/* Only binding table entry 1 differs (128 vs 192); texture B's surface state names 0x100405000. */
	drv_i915_ktest_check(
		ktest,
		sa[1] == 128U &&
		    sb[1] == 192U &&
		    ndiff == 1U &&
		    sa[32U + 8U] == 0x00404000U &&
		    sa[48U + 8U] == 0x00405000U &&
		    sa[48U + 9U] == 1U &&
		    sa[48U] == sa[32U] &&
		    sa[48U + 1U] == sa[32U + 1U] &&
		    sa[48U + 3U] == 31U,
		"tex: TEX-AB binding A vs B differs in exactly one dword (binding table entry 1: 128 vs 192); texture B's surface state names 0x100405000");
}

/* Checks that the three test images of the binding tests are distinct and have their documented texels. */
static void
i915_ktest_gt_tex_variants(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	uint32_t v1_origin;
	uint32_t v1_texel_1_0;
	uint32_t v2_origin;
	uint32_t v2_corner;
	int differ_01;
	int differ_02;
	int differ_12;

	/* Builds images 0, 1 and 2. */
	drv_i915_tex_fixture_pattern(t->pattern[0], 0U);
	drv_i915_tex_fixture_pattern(t->pattern[1], 1U);
	drv_i915_tex_fixture_pattern(t->pattern[2], 2U);

	/* Variant 1 texel (0,0) = R239 G16 B16; variant 2 texel (0,0) = R240 G240 B16, texel (7,7) = R16 G16 B16. */
	v1_origin = drv_i915_tex_fixture_expected_pixel(t->pattern[1], 0U, 0U);
	v1_texel_1_0 = drv_i915_tex_fixture_expected_pixel(t->pattern[1], 4U, 0U);
	v2_origin = drv_i915_tex_fixture_expected_pixel(t->pattern[2], 0U, 0U);
	v2_corner = drv_i915_tex_fixture_expected_pixel(t->pattern[2], 31U, 31U);
	differ_01 = kern_memcmp(t->pattern[0], t->pattern[1], I915_TEX_FIXTURE_TEX_BYTES);
	differ_02 = kern_memcmp(t->pattern[0], t->pattern[2], I915_TEX_FIXTURE_TEX_BYTES);
	differ_12 = kern_memcmp(t->pattern[1], t->pattern[2], I915_TEX_FIXTURE_TEX_BYTES);
	drv_i915_ktest_check(
		ktest,
		v1_origin == 0xffef1010U &&
		    v1_texel_1_0 == 0xffef3070U &&
		    v2_origin == 0xfff0f010U &&
		    v2_corner == 0xff101010U &&
		    differ_01 != 0 &&
		    differ_02 != 0 &&
		    differ_12 != 0,
		"tex: TEX-VARIANTS the three test images are distinct and have the documented texels");
}

/* Checks the sampler words of nearest against bilinear filtering. */
static void
i915_ktest_gt_bilinear(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const uint32_t *sn;
	const uint32_t *sl;
	unsigned index;
	unsigned ndiff;

	sn = t->state_a;
	sl = t->state_b;

	/* Writes the state page with nearest and with bilinear filtering. */
	kern_memset(t->state_a, 0, sizeof(t->state_a));
	kern_memset(t->state_b, 0, sizeof(t->state_b));
	drv_i915_tex_fixture_write_state_ab_filter(t->state_a, 0x100402000ULL, I915_TEX_FIXTURE_TEX_VA, I915_TEX_FIXTURE_TEX_B_VA, 0U, 0U, 6U);
	drv_i915_tex_fixture_write_state_ab_filter(t->state_b, 0x100402000ULL, I915_TEX_FIXTURE_TEX_VA, I915_TEX_FIXTURE_TEX_B_VA, 0U, 1U, 6U);

	/* Counts the dwords that differ. */
	ndiff = 0U;
	for (index = 0U; index < I915_KTEST_BATCH_DWORDS; index++) {
		if (sn[index] != sl[index])
			ndiff++;
	}

	/* Exactly two SAMPLER_STATE dwords differ: the min/mag filter, and the U/V/R address rounding. */
	drv_i915_ktest_check(
		ktest,
		ndiff == 2U &&
		    sn[224U] == 0x10000000U &&
		    sl[224U] == 0x10024000U &&
		    sn[227U] == 0x00000092U &&
		    sl[227U] == 0x0007e092U,
		"tex: BL-STATE nearest vs bilinear differs in exactly two SAMPLER_STATE dwords (min/mag filter = linear; U/V/R address rounding on)");
}

/* Checks that image 3 gives an exact bilinear expectation that differs from the nearest one. */
static void
i915_ktest_gt_bilinear_expect(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	const uint8_t *p3;
	uint32_t linear;
	uint32_t nearest;
	uint32_t corner;
	uint32_t at_2_0;
	uint32_t at_14_0;
	uint32_t at_2_2;
	unsigned x;
	unsigned y;
	unsigned differs;
	int inexact;

	p3 = t->pattern[3];

	/* Builds image 3. */
	drv_i915_tex_fixture_pattern(t->pattern[3], 3U);

	/* Counts the pixels where bilinear and nearest differ, and whether any bilinear value was inexact. */
	differs = 0U;
	inexact = 0;
	for (y = 0U; y < 32U; y++) {
		for (x = 0U; x < 32U; x++) {
			linear = drv_i915_tex_fixture_expected_pixel_linear(p3, x, y, &inexact);
			nearest = drv_i915_tex_fixture_expected_pixel(p3, x, y);
			if (linear != nearest)
				differs++;
		}
	}

	/*
	 * (0,0): the clamped corner, texel (0,0) = 0,0,0.  (2,0): fx = 1/8
	 * between texels 0 and 1 in u, so R = 64/8 = 8.  (14,0): between
	 * texel 3 (R192, B0) and texel 4 (R0, B64) at 1/8, so R = 168, B = 8.
	 * (2,2): fx = fy = 1/8, so R = 8, G = 8.
	 */
	corner = drv_i915_tex_fixture_expected_pixel_linear(p3, 0U, 0U, NULL);
	at_2_0 = drv_i915_tex_fixture_expected_pixel_linear(p3, 2U, 0U, NULL);
	at_14_0 = drv_i915_tex_fixture_expected_pixel_linear(p3, 14U, 0U, NULL);
	at_2_2 = drv_i915_tex_fixture_expected_pixel_linear(p3, 2U, 2U, NULL);
	drv_i915_ktest_check(
		ktest,
		inexact == 0 &&
		    differs > 700U &&
		    corner == 0xff000000U &&
		    at_2_0 == 0xff080000U &&
		    at_14_0 == 0xffa80008U &&
		    at_2_2 == 0xff080800U,
		"tex: BL-EXPECT image 3 gives an exact (integer) bilinear expectation at every pixel, and it differs from the nearest expectation at most pixels");
}

/* Checks what the GPU walks for the compute test's address, read from the tables. */
static void
i915_ktest_gt_eu_pt(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_test_ppgtt_walk walk;
	struct i915_gt_ppgtt *pp;
	uint64_t dma_fake;
	uint64_t addr_mask;
	uint64_t link_flags;
	int error;

	pp = &t->walk_pp;

	/* A DMA address with bits above 32, so a truncation shows. */
	dma_fake = 0x0000001234560000ULL;
	addr_mask = 0x0000fffffffff000ULL;
	link_flags = I915_GEN8_PAGE_PRESENT_B | I915_GEN8_PAGE_RW_B;

	/* Builds an address space with two pages at the shared address and one page inserted. */
	kern_memset(&walk, 0, sizeof(walk));
	error = drv_i915_gt_ppgtt_create(&t->gm, pp);
	if (error == 0)
		error = drv_i915_gt_ppgtt_alloc_range(&t->gm, pp, I915_TEST_EU_SHARED_VA, 2U * 4096U);
	if (error == 0)
		error = drv_i915_gt_ppgtt_insert_page(pp, dma_fake, I915_TEST_EU_SHARED_VA, 0U);
	if (error == 0)
		error = drv_i915_test_ppgtt_walk(pp, I915_TEST_EU_SHARED_VA, &walk);

	/* The links are PRESENT|RW with PPAT_CACHED_PDE (no PWT/PCD) and name the child tables. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    walk.levels == 4 &&
		    walk.top_dma == pp->top_pd_dma &&
		    walk.idx[0] == 0U &&
		    walk.idx[1] == 4U &&
		    walk.idx[2] == 2U &&
		    walk.idx[3] == 0U &&
		    walk.child_known[0] != 0 &&
		    walk.child_known[1] != 0 &&
		    walk.child_known[2] != 0 &&
		    (walk.raw[0] & ~addr_mask) == link_flags &&
		    (walk.raw[1] & ~addr_mask) == link_flags &&
		    (walk.raw[2] & ~addr_mask) == link_flags &&
		    walk.child_dma[0] == pp->tables[0].dma &&
		    walk.child_dma[1] == pp->tables[1].dma &&
		    walk.child_dma[2] == pp->tables[2].dma,
		"eu: EU-PT real page-table links are PRESENT|RW with PPAT_CACHED_PDE (no PWT/PCD), naming the child DMA addresses");

	/* The leaf carries the DMA address (bits above 32 kept), PAT 0, PRESENT|RW, not the GPU address. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    walk.leaf_present != 0 &&
		    walk.leaf_rw != 0 &&
		    walk.leaf_pat == 0U &&
		    walk.leaf_dma == dma_fake &&
		    (walk.raw[3] & ~addr_mask) == 0x3ULL &&
		    (walk.raw[3] & addr_mask) != (I915_TEST_EU_SHARED_VA & addr_mask),
		"eu: EU-PT the leaf PTE carries the DMA address (bits above 32 kept), PAT 0, PRESENT|RW, not the GPU VA");

	/* The unmapped and unallocated addresses of the same space. */
	if (error == 0)
		i915_ktest_gt_eu_pt_unmapped(ktest, t);

	drv_i915_gt_ppgtt_destroy(&t->gm, pp);
}

/* Checks what the walk finds for a never-inserted page and for a never-allocated region. */
static void
i915_ktest_gt_eu_pt_unmapped(
	struct i915_ktest *ktest,
	struct i915_ktest_gt_state *t)
{
	struct i915_test_ppgtt_walk walk;
	struct i915_gt_ppgtt *pp;
	uint64_t addr_mask;
	int error;

	pp = &t->walk_pp;
	addr_mask = 0x0000fffffffff000ULL;

	/* Walks the neighbouring page, allocated but never inserted. */
	kern_memset(&walk, 0, sizeof(walk));
	error = drv_i915_test_ppgtt_walk(pp, I915_TEST_EU_SHARED_VA + 4096U, &walk);

	/* It reads the scratch PTE: the uncached data page. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    walk.levels == 4 &&
		    walk.scratch[3] != 0 &&
		    walk.leaf_present != 0 &&
		    walk.leaf_pat == 3U &&
		    walk.leaf_dma == (pp->scratch_encode[0] & addr_mask),
		"eu: EU-PT an unmapped page inside an allocated range points at scratch[0] (uncached data page)");

	/* Walks PML4 index 1, never allocated. */
	kern_memset(&walk, 0, sizeof(walk));
	error = drv_i915_test_ppgtt_walk(pp, 0x0000008000000000ULL, &walk);

	/* It reads the scratch PDP encoding at the first level (PPAT_UNCACHED, gen8_init_scratch()). */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    walk.levels == 1 &&
		    walk.scratch[0] != 0 &&
		    walk.child_known[0] == 0 &&
		    walk.raw[0] == pp->scratch_encode[3] &&
		    (walk.raw[0] & ~addr_mask) == (I915_GEN8_PAGE_PRESENT_B | I915_GEN8_PAGE_RW_B | I915_PPAT_UNCACHED),
		"eu: EU-PT an unallocated region reads the scratch PDP encode (PPAT_UNCACHED, reference gen8_init_scratch)");
}
