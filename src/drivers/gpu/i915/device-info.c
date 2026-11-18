/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GT and engine information of the device (see device-info.h).
 *
 * The GT reports itself through fuses and clock registers: the command
 * streamer timestamp's source and rate, which slices, subslices and EUs
 * survived fusing, which L3 banks are usable, and which media engines are
 * present.  The engines are then built from the platform's engine list,
 * less the ones fused off, and any GPU fault the firmware left pending is
 * reported and cleared.
 *
 * The generations the driver serves differ in where they keep each of
 * these, so each has a description (struct i915_gt_generation) and the
 * steps below read it instead of testing the version at every turn.
 */

#include "device-info.h"
#include "mmio.h"

#include <kern/klog.h>

#include <stdbool.h>
#include <stddef.h>

/*
 * The timestamp's source and rate: CTC_MODE picks the divide logic
 * (TIMESTAMP_OVERRIDE) or the crystal; on Gen12 RPM_CONFIG0 names the
 * crystal and the shift, on Gen9 the crystal is 24 MHz and CTC_MODE holds
 * the shift.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (RPM_CONFIG0, GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK,
 * GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK, CTC_MODE,
 * CTC_SOURCE_PARAMETER_MASK, CTC_SOURCE_DIVIDE_LOGIC,
 * CTC_SHIFT_PARAMETER_MASK), drivers/gpu/drm/i915/i915_reg.h
 * (GEN9_TIMESTAMP_OVERRIDE) and drivers/gpu/drm/i915/gt/intel_gt_clock_utils.c
 * (read_reference_ts_freq, gen11_get_crystal_clock_freq,
 * gen9_read_clock_frequency).
 */
#define I915_INFO_RPM_CONFIG0		0x00d00U
#define I915_INFO_CRYSTAL_SHIFT		3U
#define I915_INFO_CRYSTAL_MASK		(0x7U << 3)
#define I915_INFO_RPM_TICK_SHIFT	1U
#define I915_INFO_RPM_TICK_MASK		(0x3U << 1)
#define I915_INFO_CTC_MODE		0x0a26cU
#define I915_INFO_CTC_DIVIDE_LOGIC	1U
#define I915_INFO_CTC_TICK_SHIFT	1U
#define I915_INFO_CTC_TICK_MASK		(0x3U << 1)
#define I915_INFO_TIMESTAMP_OVERRIDE	0x044074U
#define I915_INFO_GEN9_CRYSTAL_HZ	24000000U

static const uint32_t i915_info_crystal_hz[] = { 24000000U, 19200000U, 38400000U, 25000000U };

/*
 * The fuses: the L3 bank disables, the Gen12 slice, DSS and EU fuses, the
 * media engine disables, and the Gen9 slice and subslice fuse with its EU
 * disables a slice.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN10_MIRROR_FUSE3, GEN10_L3BANK_MASK, GEN11_EU_DISABLE, GEN11_EU_DIS_MASK,
 * GEN11_GT_SLICE_ENABLE, GEN11_GT_S_ENA_MASK, GEN12_GT_GEOMETRY_DSS_ENABLE,
 * GEN11_GT_VEBOX_VDBOX_DISABLE, GEN11_GT_VDBOX_DISABLE_MASK,
 * GEN11_GT_VEBOX_DISABLE_MASK, GEN8_FUSE2, GEN8_F2_S_ENA_MASK,
 * GEN9_F2_SS_DIS_MASK, GEN9_EU_DISABLE).
 */
#define I915_INFO_MIRROR_FUSE3		0x09118U
#define I915_INFO_L3BANK_MASK		0x0fU
#define I915_INFO_EU_DISABLE		0x09134U
#define I915_INFO_EU_DISABLE_MASK	0xffU
#define I915_INFO_SLICE_ENABLE		0x09138U
#define I915_INFO_SLICE_ENABLE_MASK	0xffU
#define I915_INFO_DSS_ENABLE		0x0913cU
#define I915_INFO_MEDIA_DISABLE		0x09140U
#define I915_INFO_VDBOX_MASK		0xffU
#define I915_INFO_VEBOX_SHIFT		16U
#define I915_INFO_VEBOX_MASK		(0x0fU << 16)
#define I915_INFO_GEN9_FUSE2		0x09120U
#define I915_INFO_GEN9_SLICE_SHIFT	25U
#define I915_INFO_GEN9_SLICE_MASK	(0x7U << 25)
#define I915_INFO_GEN9_SS_OFF_SHIFT	20U
#define I915_INFO_GEN9_SS_OFF_MASK	(0xfU << 20)
#define I915_INFO_GEN9_EU_DISABLE(s)	(0x09134U + (uint32_t)(s) * 4U)

/* The Gen9 shape (not the LP parts): 3 slices of 4 subslices of 8 EUs; Gen12: 1 slice of 6 DSS of 16 EUs. */
#define I915_INFO_GEN9_SLICES		3U
#define I915_INFO_GEN9_SUBSLICES	4U
#define I915_INFO_GEN9_EUS		8U
#define I915_INFO_GEN12_DSS		6U
#define I915_INFO_GEN12_EUS		16U

/*
 * The Ice Lake shape: 1 slice of 8 subslices of 8 EUs.  Its subslice fuse
 * names the disabled subslices (Gen12 names the enabled DSS at the same
 * register), and each EU enable bit is one EU, the same in every subslice.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_sseu.c
 * (gen11_sseu_info_init) and drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GT_SUBSLICE_DISABLE).
 */
#define I915_INFO_GEN11_SUBSLICES	8U
#define I915_INFO_GEN11_EUS		8U
#define I915_INFO_GEN11_SUBSLICE_DISABLE	0x0913cU

/* How many decode and enhancement instances the media fuse describes. */
#define I915_INFO_MEDIA_VCS		8U
#define I915_INFO_MEDIA_VECS		4U

/*
 * The GPU fault registers (the ring fault, its valid bit and engine field,
 * the two TLB data words, the address high bits and the GGTT select), and
 * the error registers cleared with them.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN12_FAULT_TLB_DATA0, GEN12_FAULT_TLB_DATA1, GEN12_RING_FAULT_REG,
 * GEN8_FAULT_TLB_DATA0, GEN8_FAULT_TLB_DATA1, GEN8_RING_FAULT_REG,
 * RING_FAULT_VALID, RING_FAULT_ENGINE_ID, FAULT_VA_HIGH_BITS, FAULT_GTT_SEL,
 * IPEIR_I965), drivers/gpu/drm/i915/i915_reg.h (PGTBL_ER, GEN2_IIR, EIR,
 * EMR, I915_MASTER_ERROR_INTERRUPT) and drivers/gpu/drm/i915/gt/intel_gt.c
 * (gen8_check_faults, intel_gt_clear_error_registers).
 */
#define I915_INFO_FAULT_VALID		(1U << 0)
#define I915_INFO_FAULT_ENGINE_SHIFT	12U
#define I915_INFO_FAULT_ENGINE_MASK	0x7U
#define I915_INFO_FAULT_VA_HIGH		0xfU
#define I915_INFO_FAULT_GGTT		(1U << 4)
#define I915_INFO_PGTBL_ER		0x02024U
#define I915_INFO_IPEIR			0x02064U
#define I915_INFO_IIR			0x020a4U
#define I915_INFO_EIR			0x020b0U
#define I915_INFO_EMR			0x020b4U
#define I915_INFO_MASTER_ERROR		(1U << 15)

/*
 * The logical ring context image sizes: render on Gen11 and Gen12, render
 * on Gen9, and every other class.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_cs.c
 * (GEN11_LR_CONTEXT_RENDER_SIZE, GEN9_LR_CONTEXT_RENDER_SIZE,
 * GEN8_LR_CONTEXT_OTHER_SIZE, intel_engine_context_size).
 */
#define I915_INFO_PAGE			4096U
#define I915_INFO_RENDER_PAGES		14U
#define I915_INFO_GEN9_RENDER_PAGES	22U
#define I915_INFO_OTHER_PAGES		2U

/* The capabilities user space sees on a video engine. */
#define I915_INFO_CAP_HEVC		(1U << 0)
#define I915_INFO_CAP_SFC		(1U << 1)

/* One engine of a platform's list. */
struct i915_engine_table_entry {
	int id;
	int class;
	int instance;
	uint32_t base;
	const char *name;
};

/* An engine's own GDRST bit. */
struct i915_info_reset_domain {
	int id;
	uint32_t domain;
};

/* The engines Alder Lake-P and a Gen9 GT can have. */
static const struct i915_engine_table_entry i915_engine_table[] = {
#include "intel/engine-table.inc"
};
static const struct i915_engine_table_entry i915_engine_table_gen9[] = {
#include "intel/engine-table-gen9.inc"
};

/*
 * The engines' own GDRST bits.  The third decode instance (VCS2) has the
 * MEDIA3 bit.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GRDOM_RENDER, GEN11_GRDOM_BLT, GEN11_GRDOM_MEDIA, GEN11_GRDOM_MEDIA3,
 * GEN11_GRDOM_VECS, GEN6_GRDOM_RENDER, GEN6_GRDOM_MEDIA, GEN6_GRDOM_BLT,
 * GEN6_GRDOM_VECS) and drivers/gpu/drm/i915/gt/intel_engine_cs.c
 * (get_reset_domain).
 */
static const struct i915_info_reset_domain i915_info_domains_gen12[] = {
	{ I915_RCS0, 1U << 1 },
	{ I915_BCS0, 1U << 2 },
	{ I915_VCS0, 1U << 5 },
	{ I915_VCS2, 1U << 7 },
	{ I915_VECS0, 1U << 13 },
};
static const struct i915_info_reset_domain i915_info_domains_gen9[] = {
	{ I915_RCS0, 1U << 1 },
	{ I915_BCS0, 1U << 3 },
	{ I915_VCS0, 1U << 2 },
	{ I915_VECS0, 1U << 4 },
};

/* Where a generation keeps what the GT reports about itself. */
struct i915_gt_generation {
	const struct i915_engine_table_entry *engines;
	size_t engine_count;
	const struct i915_info_reset_domain *domains;
	size_t domain_count;
	uint32_t fault;
	uint32_t fault_data0;
	uint32_t fault_data1;
	bool gen9;

	/* Reads the slice, subslice and EU fuses. */
	void (*read_sseu)(struct i915_sseu *sseu, struct i915_mmio *mmio);
};

static void i915_info_sseu_gen12(struct i915_sseu *sseu, struct i915_mmio *mmio);
static void i915_info_sseu_gen11(struct i915_sseu *sseu, struct i915_mmio *mmio);
static void i915_info_sseu_gen9(struct i915_sseu *sseu, struct i915_mmio *mmio);

static const struct i915_gt_generation i915_info_gen12 = {
	i915_engine_table, sizeof(i915_engine_table) / sizeof(i915_engine_table[0]),
	i915_info_domains_gen12, sizeof(i915_info_domains_gen12) / sizeof(i915_info_domains_gen12[0]),
	0x0cec4U, 0x0ceb8U, 0x0cebcU, false,
	i915_info_sseu_gen12
};

/*
 * The Gen11 description: the Gen12 engines and reset domains, the Gen8
 * fault registers.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt.c
 * (intel_gt_check_and_clear_faults: graphics versions 8 to 11 take
 * gen8_check_faults) and drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN8_RING_FAULT_REG, GEN8_FAULT_TLB_DATA0, GEN8_FAULT_TLB_DATA1).
 */
static const struct i915_gt_generation i915_info_gen11 = {
	i915_engine_table, sizeof(i915_engine_table) / sizeof(i915_engine_table[0]),
	i915_info_domains_gen12, sizeof(i915_info_domains_gen12) / sizeof(i915_info_domains_gen12[0]),
	0x04094U, 0x04b10U, 0x04b14U, false,
	i915_info_sseu_gen11
};
static const struct i915_gt_generation i915_info_gen9 = {
	i915_engine_table_gen9, sizeof(i915_engine_table_gen9) / sizeof(i915_engine_table_gen9[0]),
	i915_info_domains_gen9, sizeof(i915_info_domains_gen9) / sizeof(i915_info_domains_gen9[0]),
	0x04094U, 0x04b10U, 0x04b14U, true,
	i915_info_sseu_gen9
};

static unsigned i915_info_bits(uint32_t bits);
static uint32_t i915_info_timestamp_hz(const struct i915_gt_generation *g, struct i915_mmio *mmio, uint32_t *period_ns);
static uint32_t i915_info_media_fuses(struct i915_gt_info *gt, uint32_t engine_mask, struct i915_mmio *mmio);
static uint32_t i915_info_context_size(int graphics_ver, int class);
static uint32_t i915_info_reset_domain_of(const struct i915_gt_generation *g, int id);
static uint32_t i915_info_capabilities(const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static void i915_info_faults(struct i915_gt_info *gt, const struct i915_gt_generation *g, struct i915_mmio *mmio);

/*
 * Reads the GT and engine information.
 *
 * platform_engine_mask is the platform's engine list (RCS0, BCS0, VECS0,
 * VCS0 and VCS2 for Alder Lake-P); the media fuses take engines out of it.
 * The caller holds the render and GT forcewake domains.  Returns 0.
 */
int
drv_i915_gt_init_mmio(
	struct i915_gt_info *gt,
	int graphics_ver,
	uint32_t platform_engine_mask,
	struct i915_mmio *mmio)
{
	const struct i915_gt_generation *g;
	const struct i915_engine_table_entry *entry;
	struct i915_engine_info *engine;
	uint32_t engine_mask;
	unsigned built;
	unsigned weight;
	size_t i;

	/* Picks the description of the generation. */
	g = &i915_info_gen12;
	if (graphics_ver == 9) {
		g = &i915_info_gen9;
	} else if (graphics_ver == 11) {
		g = &i915_info_gen11;
	}
	gt->graphics_ver = graphics_ver;

	gt->clock_frequency = i915_info_timestamp_hz(g, mmio, &gt->clock_period_ns);
	gt->clock_valid = gt->clock_frequency != 0U ? 1 : 0;

	/* Reads the slice, subslice and EU fuses the generation's way. */
	g->read_sseu(&gt->sseu, mmio);

	/* Gen12 steers to an L3 bank: the usable banks are the ones the mirror fuse leaves. */
	if (g->gen9) {
		gt->l3bank_mask = 0U;
		gt->l3bank_steering = 0;
	} else {
		gt->l3bank_mask = ~drv_i915_read32(mmio, I915_INFO_MIRROR_FUSE3) & I915_INFO_L3BANK_MASK;
		if (gt->l3bank_mask == 0U)
			kern_logf("i915: WARN L3 bank mask is all zero!\n");
		gt->l3bank_steering = 1;
	}

	/* Gen9 has no media fuse: every engine of the list is there. */
	if (g->gen9) {
		engine_mask = platform_engine_mask;
		gt->sfc_mask = 0U;
		gt->vdbox_sfc_access = 0U;
	} else {
		engine_mask = i915_info_media_fuses(gt, platform_engine_mask, mmio);
	}

	built = 0U;
	for (i = 0; i < g->engine_count; i++) {
		entry = &g->engines[i];
		if ((engine_mask & (1U << (unsigned)entry->id)) == 0U)
			continue;
		if (built >= (unsigned)I915_MAX_ENGINES)
			break;

		engine = &gt->engines[built++];
		engine->id = entry->id;
		engine->graphics_ver = graphics_ver;
		engine->class = entry->class;
		engine->instance = entry->instance;
		engine->mmio_base = entry->base;
		engine->mask = 1U << (unsigned)entry->id;
		engine->reset_domain = i915_info_reset_domain_of(g, entry->id);
		engine->name = entry->name;
		engine->logical_instance = (unsigned)entry->instance;
		engine->context_size = i915_info_context_size(graphics_ver, entry->class);

		/* Gen9 offers no video work, so its video engines advertise nothing. */
		engine->uabi_capabilities = g->gen9 ? 0U : i915_info_capabilities(gt, engine);
		engine->in_use = 1;
	}
	gt->num_engines = built;
	gt->engine_mask = engine_mask;

	weight = i915_info_bits(engine_mask);
	if (weight != built)
		kern_logf("i915: WARN engine_mask 0x%x has %u bits but %u engines were built\n", engine_mask, weight, built);

	i915_info_faults(gt, g, mmio);
	gt->inited = 1;
	return 0;
}

/* The number of set bits. */
static unsigned
i915_info_bits(
	uint32_t bits)
{
	unsigned n;

	for (n = 0U; bits != 0U; bits >>= 1)
		n += bits & 1U;
	return n;
}

/*
 * The command streamer timestamp's rate in Hz, zero when unknown, and its
 * period in nanoseconds (zero with it).  The timestamp ticks once every
 * 2^(3 - shift) crystal cycles.
 */
static uint32_t
i915_info_timestamp_hz(
	const struct i915_gt_generation *g,
	struct i915_mmio *mmio,
	uint32_t *period_ns)
{
	uint32_t ctc;
	uint32_t rpm;
	uint32_t ov;
	uint32_t crystal;
	uint32_t shift;
	uint32_t hz;

	ctc = drv_i915_read32(mmio, I915_INFO_CTC_MODE);
	if ((ctc & I915_INFO_CTC_DIVIDE_LOGIC) == I915_INFO_CTC_DIVIDE_LOGIC) {
		/* Whole MHz in 9:0 (less one), and 1 MHz over the denominator in 15:12 (less one). */
		ov = drv_i915_read32(mmio, I915_INFO_TIMESTAMP_OVERRIDE);
		hz = ((ov & 0x3ffU) + 1U) * 1000000U + 1000000U / (((ov >> 12) & 0xfU) + 1U);
	} else if (g->gen9) {
		shift = (ctc & I915_INFO_CTC_TICK_MASK) >> I915_INFO_CTC_TICK_SHIFT;
		hz = I915_INFO_GEN9_CRYSTAL_HZ >> (3U - shift);
	} else {
		rpm = drv_i915_read32(mmio, I915_INFO_RPM_CONFIG0);
		crystal = (rpm & I915_INFO_CRYSTAL_MASK) >> I915_INFO_CRYSTAL_SHIFT;
		hz = 0U;
		if (crystal < sizeof(i915_info_crystal_hz) / sizeof(i915_info_crystal_hz[0]))
			hz = i915_info_crystal_hz[crystal];
		else
			kern_logf("i915: MISSING_CASE crystal_clock=%u\n", crystal);
		shift = (rpm & I915_INFO_RPM_TICK_MASK) >> I915_INFO_RPM_TICK_SHIFT;
		hz >>= 3U - shift;
	}

	if (period_ns != NULL)
		*period_ns = hz != 0U ? 1000000000U / hz : 0U;
	return hz;
}

/*
 * The Gen12 slice, DSS and EU fuses.  One slice; each enable bit of the EU
 * fuse stands for a pair of EUs, the same in every DSS.
 */
static void
i915_info_sseu_gen12(
	struct i915_sseu *sseu,
	struct i915_mmio *mmio)
{
	uint32_t slices;
	uint32_t dss;
	uint8_t pairs;
	uint16_t eus;
	unsigned i;

	sseu->max_slices = 1U;
	sseu->max_subslices = (uint8_t)I915_INFO_GEN12_DSS;
	sseu->max_eus_per_subslice = (uint8_t)I915_INFO_GEN12_EUS;
	sseu->slice_mask = 0U;
	sseu->subslice_mask = 0U;
	for (i = 0U; i < I915_SSEU_MAX_SUBSLICES; i++)
		sseu->eu_mask[i] = 0U;

	slices = drv_i915_read32(mmio, I915_INFO_SLICE_ENABLE) & I915_INFO_SLICE_ENABLE_MASK;
	if (slices != 0x1U)
		kern_logf("i915: WARN GT_SLICE_ENABLE=0x%x (expected 0x1)\n", (unsigned)slices);
	dss = drv_i915_read32(mmio, I915_INFO_DSS_ENABLE);
	pairs = (uint8_t)~(drv_i915_read32(mmio, I915_INFO_EU_DISABLE) & I915_INFO_EU_DISABLE_MASK);

	eus = 0U;
	for (i = 0U; i < I915_INFO_GEN12_EUS / 2U; i++) {
		if ((pairs & (1U << i)) != 0U)
			eus = (uint16_t)(eus | (3U << (i * 2U)));
	}

	/* Slice 0 holds the enabled DSS, each with the same EUs. */
	sseu->slice_mask |= 1U;
	sseu->subslice_mask = (uint16_t)(dss & ((1U << I915_INFO_GEN12_DSS) - 1U));
	sseu->eu_total = 0U;
	for (i = 0U; i < I915_INFO_GEN12_DSS && i < I915_SSEU_MAX_SUBSLICES; i++) {
		if ((sseu->subslice_mask & (1U << i)) != 0U)
			sseu->eu_mask[i] = eus;
		sseu->eu_total = (uint16_t)(sseu->eu_total + i915_info_bits(sseu->eu_mask[i]));
	}
	sseu->eu_per_subslice = (uint16_t)i915_info_bits(eus);

	/* Only the slice as a whole can be power gated. */
	sseu->has_slice_pg = 1;
	sseu->valid = 1;
}

/*
 * The Gen11 slice, subslice and EU fuses.
 *
 * One slice; the subslice fuse names the disabled subslices, and each bit
 * of the EU fuse is one EU, the same in every subslice.  Slices,
 * subslices and EUs can each be power gated.
 */
static void
i915_info_sseu_gen11(
	struct i915_sseu *sseu,
	struct i915_mmio *mmio)
{
	uint32_t slices;
	uint32_t subslices;
	uint16_t eus;
	unsigned i;

	/* Starts from the Ice Lake shape with nothing present. */
	sseu->max_slices = 1U;
	sseu->max_subslices = (uint8_t)I915_INFO_GEN11_SUBSLICES;
	sseu->max_eus_per_subslice = (uint8_t)I915_INFO_GEN11_EUS;
	sseu->slice_mask = 0U;
	sseu->subslice_mask = 0U;
	for (i = 0U; i < I915_SSEU_MAX_SUBSLICES; i++)
		sseu->eu_mask[i] = 0U;

	/* Ice Lake has a single slice in practice; another value is only reported. */
	slices = drv_i915_read32(mmio, I915_INFO_SLICE_ENABLE) & I915_INFO_SLICE_ENABLE_MASK;
	if (slices != 0x1U)
		kern_logf("i915: WARN GT_SLICE_ENABLE=0x%x (expected 0x1)\n", (unsigned)slices);

	/* The present subslices are the ones the fuse does not disable. */
	subslices = ~drv_i915_read32(mmio, I915_INFO_GEN11_SUBSLICE_DISABLE);
	subslices &= (1U << I915_INFO_GEN11_SUBSLICES) - 1U;

	/* The present EUs of one subslice are the ones the fuse does not disable. */
	eus = (uint16_t)(~drv_i915_read32(mmio, I915_INFO_EU_DISABLE) & I915_INFO_EU_DISABLE_MASK);

	/* Slice 0 holds the present subslices, each with the same EUs. */
	sseu->slice_mask = 1U;
	sseu->subslice_mask = (uint16_t)subslices;
	sseu->eu_total = 0U;
	for (i = 0U; i < I915_INFO_GEN11_SUBSLICES; i++) {
		if ((subslices & (1U << i)) != 0U)
			sseu->eu_mask[i] = eus;
		sseu->eu_total = (uint16_t)(sseu->eu_total + i915_info_bits(sseu->eu_mask[i]));
	}

	/* One subslice's EUs. */
	sseu->eu_per_subslice = (uint16_t)i915_info_bits(eus);

	/* Slices, subslices and EUs can each be power gated. */
	sseu->has_slice_pg = 1;
	sseu->valid = 1;
}

/*
 * The Gen9 slice, subslice and EU fuses.  The subslice disables of FUSE2
 * apply to every enabled slice, and each slice has eight EU disable bits a
 * subslice.  The masks describe the lowest enabled slice; the EU total and
 * the EUs a subslice (rounded up, for the uneven die-recovery parts) count
 * every slice.
 */
static void
i915_info_sseu_gen9(
	struct i915_sseu *sseu,
	struct i915_mmio *mmio)
{
	uint32_t fuse2;
	uint32_t live_ss;
	uint32_t off;
	uint16_t eus;
	unsigned subslices;
	unsigned slice;
	unsigned ss;
	bool first;

	sseu->max_slices = (uint8_t)I915_INFO_GEN9_SLICES;
	sseu->max_subslices = (uint8_t)I915_INFO_GEN9_SUBSLICES;
	sseu->max_eus_per_subslice = (uint8_t)I915_INFO_GEN9_EUS;
	sseu->subslice_mask = 0U;
	sseu->eu_total = 0U;
	sseu->eu_per_subslice = 0U;
	for (ss = 0U; ss < I915_SSEU_MAX_SUBSLICES; ss++)
		sseu->eu_mask[ss] = 0U;

	fuse2 = drv_i915_read32(mmio, I915_INFO_GEN9_FUSE2);
	sseu->slice_mask = (uint8_t)((fuse2 & I915_INFO_GEN9_SLICE_MASK) >> I915_INFO_GEN9_SLICE_SHIFT);
	live_ss = ((1U << I915_INFO_GEN9_SUBSLICES) - 1U) & ~((fuse2 & I915_INFO_GEN9_SS_OFF_MASK) >> I915_INFO_GEN9_SS_OFF_SHIFT);

	subslices = 0U;
	first = true;
	for (slice = 0U; slice < I915_INFO_GEN9_SLICES; slice++) {
		if ((sseu->slice_mask & (1U << slice)) == 0U)
			continue;
		if (first)
			sseu->subslice_mask = (uint16_t)live_ss;

		off = drv_i915_read32(mmio, I915_INFO_GEN9_EU_DISABLE(slice));
		for (ss = 0U; ss < I915_INFO_GEN9_SUBSLICES; ss++) {
			if ((live_ss & (1U << ss)) == 0U)
				continue;
			eus = (uint16_t)(~(off >> (ss * 8U)) & 0xffU);
			sseu->eu_total = (uint16_t)(sseu->eu_total + i915_info_bits(eus));
			subslices++;
			if (first)
				sseu->eu_mask[ss] = eus;
		}
		first = false;
	}

	if (subslices != 0U)
		sseu->eu_per_subslice = (uint16_t)((sseu->eu_total + subslices - 1U) / subslices);

	/* Slice power gating needs more than one slice. */
	sseu->has_slice_pg = i915_info_bits(sseu->slice_mask) > 1U ? 1 : 0;
	sseu->valid = 1;
}

/*
 * Takes the media engines the fuse disables out of the mask, and records
 * which decode engines reach a scaler and format converter: on media 12 an
 * even decode engine always does, an odd one only when the even one before
 * it is fused off.  Every instance keeps its converter before media 12.50.
 */
static uint32_t
i915_info_media_fuses(
	struct i915_gt_info *gt,
	uint32_t engine_mask,
	struct i915_mmio *mmio)
{
	uint32_t live;
	uint32_t bit;
	uint16_t vcs;
	uint16_t vecs;
	unsigned i;

	live = ~drv_i915_read32(mmio, I915_INFO_MEDIA_DISABLE);
	vcs = (uint16_t)(live & I915_INFO_VDBOX_MASK);
	vecs = (uint16_t)((live & I915_INFO_VEBOX_MASK) >> I915_INFO_VEBOX_SHIFT);
	gt->sfc_mask = ~0U;

	for (i = 0U; i < I915_INFO_MEDIA_VCS; i++) {
		bit = 1U << (I915_VCS0 + i);
		if ((engine_mask & bit) == 0U) {
			vcs = (uint16_t)(vcs & ~(1U << i));
			continue;
		}
		if ((vcs & (1U << i)) == 0U) {
			engine_mask &= ~bit;
			kern_logf("i915: vcs%u fused off\n", i);
			continue;
		}
		if ((i % 2U) == 0U || (vcs & (1U << (i - 1U))) == 0U)
			gt->vdbox_sfc_access |= 1U << i;
	}

	for (i = 0U; i < I915_INFO_MEDIA_VECS; i++) {
		bit = 1U << (I915_VECS0 + i);
		if ((engine_mask & bit) == 0U) {
			vecs = (uint16_t)(vecs & ~(1U << i));
			continue;
		}
		if ((vecs & (1U << i)) == 0U) {
			engine_mask &= ~bit;
			kern_logf("i915: vecs%u fused off\n", i);
		}
	}
	return engine_mask;
}

/* The context image size of an engine class; an unknown version gets the Gen11 render size. */
static uint32_t
i915_info_context_size(
	int graphics_ver,
	int class)
{
	if (class != I915_RENDER_CLASS)
		return I915_INFO_OTHER_PAGES * I915_INFO_PAGE;
	if (graphics_ver == 9)
		return I915_INFO_GEN9_RENDER_PAGES * I915_INFO_PAGE;
	if (graphics_ver != 11 && graphics_ver != 12)
		kern_logf("i915: MISSING_CASE context size ver=%d\n", graphics_ver);
	return I915_INFO_RENDER_PAGES * I915_INFO_PAGE;
}

/* An engine's own GDRST bit, or 0 for an engine the generation has none for. */
static uint32_t
i915_info_reset_domain_of(
	const struct i915_gt_generation *g,
	int id)
{
	size_t i;

	for (i = 0; i < g->domain_count; i++) {
		if (g->domains[i].id == id)
			return g->domains[i].domain;
	}
	return 0U;
}

/* What user space may use on a video engine: HEVC on decode, and the converter where the engine reaches one. */
static uint32_t
i915_info_capabilities(
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	uint32_t caps;

	caps = 0U;
	if (engine->class == I915_VIDEO_DECODE_CLASS) {
		caps |= I915_INFO_CAP_HEVC;
		if ((gt->vdbox_sfc_access & (1U << (unsigned)engine->instance)) != 0U)
			caps |= I915_INFO_CAP_SFC;
	} else if (engine->class == I915_VIDEO_ENHANCEMENT_CLASS) {
		if ((gt->sfc_mask & (1U << (unsigned)engine->instance)) != 0U)
			caps |= I915_INFO_CAP_SFC;
	}
	return caps;
}

/*
 * Reports a GPU fault the firmware left pending and clears the error
 * registers.  An error that stays set after the clear is masked, since it
 * can become stuck.  Only the valid bit of the ring fault register is
 * cleared: writing the whole register to zero clobbers its other fields and
 * leaves the GT in a state that breaks the later display bring-up.
 */
static void
i915_info_faults(
	struct i915_gt_info *gt,
	const struct i915_gt_generation *g,
	struct i915_mmio *mmio)
{
	uint32_t fault;
	uint32_t data0;
	uint32_t data1;
	uint32_t stuck;
	uint32_t mask;
	uint64_t address;

	fault = drv_i915_read32(mmio, g->fault);
	gt->fault_reg = fault;
	if ((fault & I915_INFO_FAULT_VALID) != 0U) {
		data0 = drv_i915_read32(mmio, g->fault_data0);
		data1 = drv_i915_read32(mmio, g->fault_data1);
		address = ((uint64_t)(data1 & I915_INFO_FAULT_VA_HIGH) << 44) | ((uint64_t)data0 << 12);
		gt->fault_valid_seen = 1;
		kern_logf("i915: Unexpected fault addr=0x%llx space=%s engine_id=%u\n",
			  (unsigned long long)address,
			  (data1 & I915_INFO_FAULT_GGTT) != 0U ? "GGTT" : "PPGTT",
			  (fault >> I915_INFO_FAULT_ENGINE_SHIFT) & I915_INFO_FAULT_ENGINE_MASK);
	}

	drv_i915_write32(mmio, I915_INFO_PGTBL_ER, 0U);
	drv_i915_write32(mmio, I915_INFO_IPEIR, 0U);
	drv_i915_write32(mmio, I915_INFO_EIR, 0U);
	stuck = drv_i915_read32(mmio, I915_INFO_EIR);
	if (stuck != 0U) {
		mask = drv_i915_read32(mmio, I915_INFO_EMR);
		kern_logf("i915: EIR stuck: 0x%08x, masking\n", stuck);
		drv_i915_write32(mmio, I915_INFO_EMR, mask | stuck);
		drv_i915_write32(mmio, I915_INFO_IIR, I915_INFO_MASTER_ERROR);
	}

	drv_i915_write32(mmio, g->fault, drv_i915_read32(mmio, g->fault) & ~I915_INFO_FAULT_VALID);
	drv_i915_posting_read32(mmio, g->fault);
}
