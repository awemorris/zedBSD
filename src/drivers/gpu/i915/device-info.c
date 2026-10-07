/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GT and engine information of the device (see device-info.h).
 *
 * The step follows Linux's intel_gt_init_mmio() for Alder Lake-P:
 * the timestamp clock (intel_gt_init_clock_frequency()), the SSEU fuse
 * decode (gen12_sseu_info_init()), the L3 bank steering mask
 * (intel_gt_mcr_init()), the engine objects with the media fuses applied
 * (intel_engines_init_mmio()), and the check and clear of any GPU fault left
 * pending by the firmware (intel_gt_check_and_clear_faults()).
 */

#include "device-info.h"
#include "mmio.h"

#include <kern/klog.h>

#include <stddef.h>

/* RPM_CONFIG0: the crystal clock frequency and the timestamp shift. */
#define I915_RPM_CONFIG0				0x00d00U
#define I915_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3U
#define I915_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(0x7U << 3)
#define I915_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT	1U
#define I915_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK	(0x3U << 1)

/* CTC_MODE: where the command streamer timestamp is derived from. */
#define I915_CTC_MODE					0x0a26cU
#define I915_CTC_SOURCE_PARAMETER_MASK			1U
#define I915_CTC_SOURCE_DIVIDE_LOGIC			1U

/* GEN9_TIMESTAMP_OVERRIDE: the timestamp frequency on the divide-logic path. */
#define I915_TIMESTAMP_OVERRIDE				0x044074U

/* GEN10_MIRROR_FUSE3: the L3 bank disable fuse. */
#define I915_MIRROR_FUSE3				0x09118U
#define I915_L3BANK_MASK				0x0fU

/* The Gen11/Gen12 slice, subslice and EU fuses. */
#define I915_EU_DISABLE					0x09134U
#define I915_EU_DIS_MASK				0xffU
#define I915_GT_SLICE_ENABLE				0x09138U
#define I915_GT_S_ENA_MASK				0xffU
#define I915_GT_GEOMETRY_DSS_ENABLE			0x0913cU

/* GEN11_GT_VEBOX_VDBOX_DISABLE: the media engine fuses. */
#define I915_GT_VEBOX_VDBOX_DISABLE			0x09140U
#define I915_GT_VDBOX_DISABLE_MASK			0xffU
#define I915_GT_VEBOX_DISABLE_SHIFT			16U
#define I915_GT_VEBOX_DISABLE_MASK			(0x0fU << 16)

/* The Gen12 GPU fault registers. */
#define I915_FAULT_TLB_DATA0				0x0ceb8U
#define I915_FAULT_TLB_DATA1				0x0cebcU
#define I915_RING_FAULT_REG				0x0cec4U
#define I915_RING_FAULT_VALID				(1U << 0)
#define I915_RING_FAULT_ENGINE_ID_SHIFT			12U
#define I915_RING_FAULT_ENGINE_ID_MASK			0x7U
#define I915_FAULT_VA_HIGH_BITS				0xfU
#define I915_FAULT_GTT_SEL				(1U << 4)

/*
 * The error registers intel_gt_clear_error_registers() clears.
 *
 * They sit in the render forcewake range.
 */
#define I915_PGTBL_ER					0x02024U
#define I915_IPEIR_I965					0x02064U
#define I915_GEN2_IIR					0x020a4U
#define I915_EIR					0x020b0U
#define I915_EMR					0x020b4U
#define I915_MASTER_ERROR_INTERRUPT			(1U << 15)

/* The logical ring context sizes (intel_engine_cs.c). */
#define I915_CONTEXT_PAGE_SIZE				4096U
#define I915_LR_CONTEXT_RENDER_SIZE			(14U * I915_CONTEXT_PAGE_SIZE)
#define I915_LR_CONTEXT_OTHER_SIZE			(2U * I915_CONTEXT_PAGE_SIZE)

/* How many video decode and video enhancement instances the media fuses describe. */
#define I915_MAX_VCS					8U
#define I915_MAX_VECS					4U

/*
 * The per-engine reset domains (GEN11_GRDOM_*, intel_gt_regs.h).  VCS2 is
 * the third video decode instance, so its domain is MEDIA3, not MEDIA2.
 * (ws083-p007: the media and enhancement bits were the Gen6 layout's, which
 * named the GuC domain for VCS0.)
 */
#define I915_GRDOM_RENDER				(1U << 1)
#define I915_GRDOM_BLT					(1U << 2)
#define I915_GRDOM_MEDIA				(1U << 5)
#define I915_GRDOM_MEDIA3				(1U << 7)
#define I915_GRDOM_VECS					(1U << 13)

/* The user-visible engine capabilities (I915_VIDEO_CLASS_CAPABILITY_*). */
#define I915_CAPABILITY_HEVC				(1U << 0)
#define I915_CAPABILITY_SFC				(1U << 1)

/*
 * One engine the platform may have, as the reference table describes it.
 */
struct i915_engine_table_entry {
	int id;
	int class;
	int instance;
	uint32_t base;
	const char *name;
};

/*
 * The engines Alder Lake-P can have, in the reference's order.
 *
 * The table never changes and every device shares it.
 */
static const struct i915_engine_table_entry i915_engine_table[] = {
#include "intel/engine-table.inc"
};

static unsigned i915_hweight16(uint16_t bits);
static unsigned i915_hweight32(uint32_t bits);
static uint32_t i915_crystal_clock_frequency(uint32_t rpm_config);
static uint32_t i915_reference_timestamp_frequency(struct i915_mmio *mmio);
static uint32_t i915_read_clock_frequency(struct i915_mmio *mmio, uint32_t *period_ns);
static void i915_compute_sseu_info(struct i915_sseu *sseu, uint32_t subslice_enable, uint16_t eu_enable);
static void i915_sseu_info_init(struct i915_sseu *sseu, struct i915_mmio *mmio);
static uint32_t i915_engine_context_size(int graphics_ver, int class);
static uint32_t i915_engine_mask_apply_media_fuses(struct i915_gt_info *gt, uint32_t engine_mask, struct i915_mmio *mmio);
static uint32_t i915_engine_reset_domain(int id);
static void i915_engine_setup_capabilities(const struct i915_gt_info *gt, struct i915_engine_info *engine);
static void i915_check_and_clear_faults(struct i915_gt_info *gt, struct i915_mmio *mmio);

/*
 * Reads the GT and engine information of an Alder Lake-P device.
 *
 * This is Linux's intel_gt_init_mmio().  The platform engine mask is the
 * device-table value (RCS0, BCS0, VECS0, VCS0 and VCS2 for Alder Lake-P); the
 * media fuses are applied to it here exactly as the reference does.  The
 * caller holds the render and GT forcewake domains.  Returns 0.
 */
int
drv_i915_gt_init_mmio(
	struct i915_gt_info *gt,
	int graphics_ver,
	uint32_t platform_engine_mask,
	struct i915_mmio *mmio)
{
	const struct i915_engine_table_entry *entry;
	struct i915_engine_info *engine;
	unsigned table_length;
	unsigned engine_count;
	unsigned mask_weight;
	unsigned index;
	uint32_t engine_mask;
	uint32_t mirror_fuse;

	/* Reads the command streamer timestamp frequency (intel_gt_init_clock_frequency()). */
	gt->clock_frequency = i915_read_clock_frequency(mmio, &gt->clock_period_ns);
	gt->clock_valid = 0;
	if (gt->clock_frequency != 0U)
		gt->clock_valid = 1;

	/* Decodes which subslices and EUs survived fusing (intel_sseu_info_init()). */
	i915_sseu_info_init(&gt->sseu, mmio);

	/*
	 * Installs the L3 bank steering (intel_gt_mcr_init()): graphics 11 up to
	 * 12.50 steer to an L3 bank, and the usable banks are the inverse of the
	 * mirror fuse.
	 */
	mirror_fuse = drv_i915_read32(mmio, I915_MIRROR_FUSE3);
	gt->l3bank_mask = (~mirror_fuse) & I915_L3BANK_MASK;
	if (gt->l3bank_mask == 0U)
		kern_logf("i915: WARN L3 bank mask is all zero!\n");

	gt->l3bank_steering = 1;

	/* Drops the engines the media fuses disabled (intel_engines_init_mmio()). */
	engine_mask = i915_engine_mask_apply_media_fuses(gt, platform_engine_mask, mmio);

	/* Builds one engine object for every engine that survived fusing. */
	engine_count = 0U;
	table_length = sizeof(i915_engine_table) / sizeof(i915_engine_table[0]);
	for (index = 0U; index < table_length; index++) {
		entry = &i915_engine_table[index];

		/* Skips an engine the platform does not have or the fuses removed. */
		if ((engine_mask & (1U << (unsigned)entry->id)) == 0U)
			continue;

		/* Stops when the engine array is full. */
		if (engine_count >= (unsigned)I915_MAX_ENGINES)
			break;

		/* Claims the next engine slot. */
		engine = &gt->engines[engine_count];
		engine_count++;

		/* Records who the engine is and where its registers are. */
		engine->id = entry->id;
		engine->class = entry->class;
		engine->instance = entry->instance;
		engine->mmio_base = entry->base;
		engine->mask = 1U << (unsigned)entry->id;
		engine->reset_domain = i915_engine_reset_domain(entry->id);
		engine->name = entry->name;
		engine->logical_instance = (unsigned)entry->instance;

		/* Sizes the engine's context image. */
		engine->context_size = i915_engine_context_size(graphics_ver, entry->class);

		/* Records what user space may use on the engine. */
		engine->uabi_capabilities = 0U;
		i915_engine_setup_capabilities(gt, engine);

		/* The slot now holds a built engine. */
		engine->in_use = 1;
	}

	/* Publishes the engines that were built. */
	gt->num_engines = engine_count;
	gt->engine_mask = engine_mask;

	/* Reports an engine mask that does not match the engines built. */
	mask_weight = i915_hweight32(engine_mask);
	if (mask_weight != engine_count) {
		kern_logf("i915: WARN engine_mask 0x%x has %u bits but %u engines were built\n",
			  engine_mask,
			  mask_weight,
			  engine_count);
	}

	/* Reports and clears any GPU fault the firmware left pending. */
	i915_check_and_clear_faults(gt, mmio);

	/* The information is complete. */
	gt->inited = 1;

	/* Succeeded: the GT information is filled. */
	return 0;
}

/* Counts the set bits of a 16-bit mask. */
static unsigned
i915_hweight16(
	uint16_t bits)
{
	unsigned count;

	/* Adds up the bits one at a time. */
	count = 0U;
	while (bits != 0U) {
		count += (unsigned)(bits & 1U);
		bits = (uint16_t)(bits >> 1);
	}

	/* Reports the number of set bits. */
	return count;
}

/* Counts the set bits of a 32-bit mask. */
static unsigned
i915_hweight32(
	uint32_t bits)
{
	unsigned count;

	/* Adds up the bits one at a time. */
	count = 0U;
	while (bits != 0U) {
		count += (unsigned)(bits & 1U);
		bits >>= 1;
	}

	/* Reports the number of set bits. */
	return count;
}

/* Decodes the crystal clock frequency RPM_CONFIG0 reports (gen11_get_crystal_clock_freq()). */
static uint32_t
i915_crystal_clock_frequency(
	uint32_t rpm_config)
{
	uint32_t crystal_clock;

	/* Extracts the crystal clock selector. */
	crystal_clock = (rpm_config & I915_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK) >> I915_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT;

	/* Translates the selector into Hz. */
	switch (crystal_clock) {
	case 0U:
		/* 24 MHz. */
		return 24000000U;
	case 1U:
		/* 19.2 MHz. */
		return 19200000U;
	case 2U:
		/* 38.4 MHz. */
		return 38400000U;
	case 3U:
		/* 25 MHz. */
		return 25000000U;
	default:
		/* A selector the reference does not know leaves the frequency unknown. */
		kern_logf("i915: MISSING_CASE crystal_clock=%u\n", crystal_clock);
		return 0U;
	}
}

/* Reads the timestamp frequency the divide logic produces (read_reference_ts_freq()). */
static uint32_t
i915_reference_timestamp_frequency(
	struct i915_mmio *mmio)
{
	uint32_t timestamp_override;
	uint32_t base_frequency;
	uint32_t fraction_frequency;

	/* Reads the timestamp override register. */
	timestamp_override = drv_i915_read32(mmio, I915_TIMESTAMP_OVERRIDE);

	/* The whole-MHz part is the low ten bits plus one. */
	base_frequency = ((timestamp_override >> 0) & 0x3ffU) + 1U;
	base_frequency *= 1000000U;

	/* The fractional part is one MHz divided by the denominator field plus one. */
	fraction_frequency = (timestamp_override >> 12) & 0xfU;
	fraction_frequency = 1000000U / (fraction_frequency + 1U);

	/* Reports the timestamp frequency in Hz. */
	return base_frequency + fraction_frequency;
}

/* Reads the command streamer timestamp frequency and its period (gen11_read_clock_frequency()). */
static uint32_t
i915_read_clock_frequency(
	struct i915_mmio *mmio,
	uint32_t *period_ns)
{
	uint32_t ctc_mode;
	uint32_t rpm_config;
	uint32_t shift;
	uint32_t frequency;

	/*
	 * CTC_MODE says whether the timestamp comes from TIMESTAMP_OVERRIDE or
	 * from the crystal clock through RPM_CONFIG0.
	 */
	ctc_mode = drv_i915_read32(mmio, I915_CTC_MODE);
	if ((ctc_mode & I915_CTC_SOURCE_PARAMETER_MASK) == I915_CTC_SOURCE_DIVIDE_LOGIC) {
		frequency = i915_reference_timestamp_frequency(mmio);
	} else {
		rpm_config = drv_i915_read32(mmio, I915_RPM_CONFIG0);
		frequency = i915_crystal_clock_frequency(rpm_config);

		/*
		 * The timestamp may increment only every few crystal clock
		 * cycles; the shift parameter says how many.
		 */
		shift = (rpm_config & I915_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK) >> I915_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT;
		frequency >>= 3U - shift;
	}

	/* Reports the period, or zero when the frequency is unknown. */
	if (period_ns != NULL) {
		if (frequency != 0U) {
			*period_ns = 1000000000U / frequency;
		} else {
			*period_ns = 0U;
		}
	}

	/* Reports the frequency in Hz, or zero when it is unknown. */
	return frequency;
}

/* Records the enabled subslices and EUs and counts them (gen11_compute_sseu_info()). */
static void
i915_compute_sseu_info(
	struct i915_sseu *sseu,
	uint32_t subslice_enable,
	uint16_t eu_enable)
{
	uint32_t valid_subslice_mask;
	unsigned subslice;

	/* Limits the subslice fuse to the subslices the generation has. */
	if (sseu->max_subslices >= 32U) {
		valid_subslice_mask = 0xffffffffU;
	} else {
		valid_subslice_mask = (1U << sseu->max_subslices) - 1U;
	}

	/* Slice 0 is present and holds the enabled subslices. */
	sseu->slice_mask |= 1U;
	sseu->subslice_mask = (uint16_t)(subslice_enable & valid_subslice_mask);

	/* Gives every present subslice the same EU set (sseu_set_eus()). */
	for (subslice = 0U; subslice < sseu->max_subslices; subslice++) {
		if ((sseu->subslice_mask & (1U << subslice)) == 0U)
			continue;

		if (subslice < I915_SSEU_MAX_SUBSLICES)
			sseu->eu_mask[subslice] = eu_enable;
	}

	/* Counts the EUs in one subslice. */
	sseu->eu_per_subslice = (uint16_t)i915_hweight16(eu_enable);

	/* Counts the EUs over the populated subslices (compute_eu_total()). */
	sseu->eu_total = 0U;
	for (subslice = 0U; subslice < sseu->max_subslices; subslice++) {
		if (subslice >= I915_SSEU_MAX_SUBSLICES)
			break;

		sseu->eu_total = (uint16_t)(sseu->eu_total + i915_hweight16(sseu->eu_mask[subslice]));
	}
}

/* Decodes the Gen12 slice, subslice and EU fuses (gen12_sseu_info_init()). */
static void
i915_sseu_info_init(
	struct i915_sseu *sseu,
	struct i915_mmio *mmio)
{
	uint32_t slice_fuse;
	uint32_t dss_enable;
	uint32_t eu_disable;
	uint16_t eu_enable;
	uint8_t eu_enable_fuse;
	uint8_t slice_enable;
	unsigned index;

	/*
	 * Gen12 has Dual-Subslices, which behave like two Gen11 subslices:
	 * one slice, six DSS, sixteen EUs per DSS (intel_sseu_set_info()).
	 */
	sseu->max_slices = 1U;
	sseu->max_subslices = 6U;
	sseu->max_eus_per_subslice = 16U;
	sseu->slice_mask = 0U;
	sseu->subslice_mask = 0U;

	/* Starts with no EU present in any subslice. */
	for (index = 0U; index < I915_SSEU_MAX_SUBSLICES; index++)
		sseu->eu_mask[index] = 0U;

	/* Tiger Lake, Rocket Lake, DG1 and Alder Lake only ever had a single slice. */
	slice_fuse = drv_i915_read32(mmio, I915_GT_SLICE_ENABLE);
	slice_enable = (uint8_t)(slice_fuse & I915_GT_S_ENA_MASK);
	if (slice_enable != 0x1U)
		kern_logf("i915: WARN GT_SLICE_ENABLE=0x%x (expected 0x1)\n", (unsigned)slice_enable);

	/* Reads which DSS are enabled. */
	dss_enable = drv_i915_read32(mmio, I915_GT_GEOMETRY_DSS_ENABLE);

	/* Reads the EU disable fuse and inverts it into an enable fuse. */
	eu_disable = drv_i915_read32(mmio, I915_EU_DISABLE);
	eu_enable_fuse = (uint8_t)(~(eu_disable & I915_EU_DIS_MASK));

	/* Expands each enabled pair of EUs into two EU bits. */
	eu_enable = 0U;
	for (index = 0U; index < (unsigned)(sseu->max_eus_per_subslice / 2U); index++) {
		if ((eu_enable_fuse & (1U << index)) != 0U)
			eu_enable = (uint16_t)(eu_enable | (1U << (index * 2U)) | (1U << (index * 2U + 1U)));
	}

	/* Records the enabled subslices and EUs. */
	i915_compute_sseu_info(sseu, dss_enable, eu_enable);

	/* Tiger Lake only supports slice-level power gating. */
	sseu->has_slice_pg = 1;
	sseu->valid = 1;
}

/* Reports the size of an engine class's context image (intel_engine_context_size()). */
static uint32_t
i915_engine_context_size(
	int graphics_ver,
	int class)
{
	/* The render context size depends on the graphics version. */
	if (class == I915_RENDER_CLASS) {
		/* Picks the render context size of the graphics version. */
		switch (graphics_ver) {
		case 12:
		case 11:
			return I915_LR_CONTEXT_RENDER_SIZE;
		default:
			/* An unknown version is reported and given the Gen11 size. */
			kern_logf("i915: MISSING_CASE context size ver=%d\n", graphics_ver);
			return I915_LR_CONTEXT_RENDER_SIZE;
		}
	}

	/* Every non-render class on Gen8 and later uses the other size. */
	return I915_LR_CONTEXT_OTHER_SIZE;
}

/*
 * Removes the media engines the fuses disabled (engine_mask_apply_media_fuses()).
 *
 * On media versions before 12.50 the register has disable semantics, so the
 * reference inverts it before extracting the masks.
 */
static uint32_t
i915_engine_mask_apply_media_fuses(
	struct i915_gt_info *gt,
	uint32_t engine_mask,
	struct i915_mmio *mmio)
{
	uint32_t media_fuse;
	uint32_t engine_bit;
	uint16_t vdbox_mask;
	uint16_t vebox_mask;
	unsigned instance;

	/* Reads the media fuse and inverts it into an enable mask. */
	media_fuse = drv_i915_read32(mmio, I915_GT_VEBOX_VDBOX_DISABLE);
	media_fuse = ~media_fuse;

	/* Splits the enable mask into the video decode and enhancement halves. */
	vdbox_mask = (uint16_t)(media_fuse & I915_GT_VDBOX_DISABLE_MASK);
	vebox_mask = (uint16_t)((media_fuse & I915_GT_VEBOX_DISABLE_MASK) >> I915_GT_VEBOX_DISABLE_SHIFT);

	/* Before media 12.50 every instance has its scaler and format converter. */
	gt->sfc_mask = ~0U;

	/* Drops the fused-off video decode engines and assigns the shared converters. */
	for (instance = 0U; instance < I915_MAX_VCS; instance++) {
		engine_bit = 1U << (I915_VCS0 + instance);

		/* An engine the platform does not have is not in the fuse either. */
		if ((engine_mask & engine_bit) == 0U) {
			vdbox_mask = (uint16_t)(vdbox_mask & ~(1U << instance));
			continue;
		}

		/* An engine the fuse disabled leaves the mask. */
		if ((vdbox_mask & (1U << instance)) == 0U) {
			engine_mask &= ~engine_bit;
			kern_logf("i915: vcs%u fused off\n", instance);
			continue;
		}

		/*
		 * On media 12 an even physical video decode engine always reaches
		 * its converter, and an odd one only when the even engine before
		 * it is fused off (gen11_vdbox_has_sfc(), MEDIA_VER >= 12).  The
		 * Gen11 rule counted logical instances, which left VCS2 of Alder
		 * Lake-P without one (ws083-p007).
		 */
		if ((instance % 2U) == 0U) {
			gt->vdbox_sfc_access |= 1U << instance;
		} else if ((vdbox_mask & (1U << (instance - 1U))) == 0U) {
			gt->vdbox_sfc_access |= 1U << instance;
		}
	}

	/* Drops the fused-off video enhancement engines. */
	for (instance = 0U; instance < I915_MAX_VECS; instance++) {
		engine_bit = 1U << (I915_VECS0 + instance);

		/* An engine the platform does not have is not in the fuse either. */
		if ((engine_mask & engine_bit) == 0U) {
			vebox_mask = (uint16_t)(vebox_mask & ~(1U << instance));
			continue;
		}

		/* An engine the fuse disabled leaves the mask. */
		if ((vebox_mask & (1U << instance)) == 0U) {
			engine_mask &= ~engine_bit;
			kern_logf("i915: vecs%u fused off\n", instance);
		}
	}

	/* Reports the engine mask with the fused-off engines removed. */
	return engine_mask;
}

/* Reports the GDRST bit that resets one engine alone (get_reset_domain(), Gen11+). */
static uint32_t
i915_engine_reset_domain(
	int id)
{
	/* Picks the engine's own reset domain. */
	switch (id) {
	case I915_RCS0:
		return I915_GRDOM_RENDER;
	case I915_BCS0:
		return I915_GRDOM_BLT;
	case I915_VCS0:
		return I915_GRDOM_MEDIA;
	case I915_VCS2:
		return I915_GRDOM_MEDIA3;
	case I915_VECS0:
		return I915_GRDOM_VECS;
	default:
		/* An engine outside the table has no reset domain. */
		return 0U;
	}
}

/* Records the features user space may use on an engine (intel_engine_setup_capabilities()). */
static void
i915_engine_setup_capabilities(
	const struct i915_gt_info *gt,
	struct i915_engine_info *engine)
{
	/* Video decode has HEVC everywhere and the converter where it was assigned. */
	if (engine->class == I915_VIDEO_DECODE_CLASS) {
		engine->uabi_capabilities |= I915_CAPABILITY_HEVC;
		if ((gt->vdbox_sfc_access & (1U << (unsigned)engine->instance)) != 0U)
			engine->uabi_capabilities |= I915_CAPABILITY_SFC;
	} else if (engine->class == I915_VIDEO_ENHANCEMENT_CLASS) {
		/* Video enhancement has the converter when its instance keeps one. */
		if ((gt->sfc_mask & (1U << (unsigned)engine->instance)) != 0U)
			engine->uabi_capabilities |= I915_CAPABILITY_SFC;
	}
}

/*
 * Reports a pending GPU fault and clears the error registers.
 *
 * This is Linux's intel_gt_check_and_clear_faults() with the Gen12 register
 * set of gen8_check_faults(), followed by intel_gt_clear_error_registers().
 */
static void
i915_check_and_clear_faults(
	struct i915_gt_info *gt,
	struct i915_mmio *mmio)
{
	uint32_t fault;
	uint32_t ring_fault;
	uint32_t tlb_data0;
	uint32_t tlb_data1;
	uint32_t error_identity;
	uint32_t error_mask;
	uint64_t address;
	const char *space;

	/* Reads the ring fault register and reports a valid fault. */
	fault = drv_i915_read32(mmio, I915_RING_FAULT_REG);
	gt->fault_reg = fault;
	if ((fault & I915_RING_FAULT_VALID) != 0U) {
		/* Rebuilds the faulting address from the two TLB data words. */
		tlb_data0 = drv_i915_read32(mmio, I915_FAULT_TLB_DATA0);
		tlb_data1 = drv_i915_read32(mmio, I915_FAULT_TLB_DATA1);
		address = ((uint64_t)(tlb_data1 & I915_FAULT_VA_HIGH_BITS) << 44) | ((uint64_t)tlb_data0 << 12);

		/* The select bit says whether the address is global or per-process. */
		if ((tlb_data1 & I915_FAULT_GTT_SEL) != 0U) {
			space = "GGTT";
		} else {
			space = "PPGTT";
		}

		/* Records that a fault was pending and reports it. */
		gt->fault_valid_seen = 1;
		kern_logf("i915: Unexpected fault addr=0x%llx space=%s engine_id=%u\n",
			  (unsigned long long)address,
			  space,
			  (fault >> I915_RING_FAULT_ENGINE_ID_SHIFT) & I915_RING_FAULT_ENGINE_ID_MASK);
	}

	/* Clears the page table and instruction error registers. */
	drv_i915_write32(mmio, I915_PGTBL_ER, 0U);
	drv_i915_write32(mmio, I915_IPEIR_I965, 0U);
	drv_i915_write32(mmio, I915_EIR, 0U);

	/* Masks an error that stays set after the clear, since it can become stuck. */
	error_identity = drv_i915_read32(mmio, I915_EIR);
	if (error_identity != 0U) {
		error_mask = drv_i915_read32(mmio, I915_EMR);
		kern_logf("i915: EIR stuck: 0x%08x, masking\n", error_identity);
		drv_i915_write32(mmio, I915_EMR, error_mask | error_identity);
		drv_i915_write32(mmio, I915_GEN2_IIR, I915_MASTER_ERROR_INTERRUPT);
	}

	/*
	 * Clears only the valid bit of the ring fault register.  Writing the
	 * whole register to zero clobbers its other fields and leaves the GT in
	 * a state that breaks the later display bring-up.
	 */
	ring_fault = drv_i915_read32(mmio, I915_RING_FAULT_REG);
	drv_i915_write32(mmio, I915_RING_FAULT_REG, ring_fault & ~I915_RING_FAULT_VALID);
	drv_i915_posting_read32(mmio, I915_RING_FAULT_REG);
}
