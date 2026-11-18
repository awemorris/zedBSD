/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GT and engine information of the device.
 *
 * Before any engine runs, the driver reads what the fuses and the clock
 * registers say about this particular part: how fast the command streamer
 * timestamp ticks, which subslices and EUs survived fusing, which L3 banks
 * replicated registers can be steered to, and which engines exist.
 *
 * Gen9 (Skylake and its Gen9.5 successors) reads the same facts from the
 * Gen8 fuse layout: GEN8_FUSE2 and one GEN9_EU_DISABLE register per slice,
 * a fixed 24 MHz crystal behind CTC_MODE, and no L3 bank steering.
 *
 * Easy to get wrong, and checked by the tests:
 *
 *   - GEN11_EU_DISABLE is a disable mask with one bit per pair of EUs; each
 *     set bit of its inverse enables two EUs.
 *   - Gen12 counts Dual-Subslices: at most 1 slice, 6 DSS, 16 EUs per DSS.
 *   - GEN11_GT_VEBOX_VDBOX_DISABLE is a disable register on media versions
 *     before 12.50, so it is inverted before use.
 */

#ifndef DRIVERS_GPU_I915_DEVICE_INFO_H
#define DRIVERS_GPU_I915_DEVICE_INFO_H

#include <stdint.h>

struct i915_mmio;

/* How many engines one device can have: RCS0, BCS0, VCS0, VCS2, VECS0, and one spare. */
#define I915_MAX_ENGINES		6

/* How many subslices the SSEU masks describe: Ice Lake has eight. */
#define I915_SSEU_MAX_SUBSLICES		8

/*
 * The engine identifiers: a block of ids for each class, the instance
 * added to the class's first id.  The interrupt code looks an engine up by
 * class and instance through them.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/intel_engine_types.h (enum intel_engine_id).
 */
#define I915_RCS0			0
#define I915_BCS0			1
#define I915_VCS0			8
#define I915_VCS2			10
#define I915_VECS0			16

/*
 * The engine classes, the ordinals the interrupt registers use.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/intel_engine_types.h (RENDER_CLASS,
 * VIDEO_DECODE_CLASS, VIDEO_ENHANCEMENT_CLASS, COPY_ENGINE_CLASS,
 * OTHER_CLASS).
 */
#define I915_RENDER_CLASS		0
#define I915_VIDEO_DECODE_CLASS		1
#define I915_VIDEO_ENHANCEMENT_CLASS	2
#define I915_COPY_ENGINE_CLASS		3
#define I915_OTHER_CLASS		4
#define I915_MAX_ENGINE_CLASS		4
#define I915_MAX_ENGINE_INSTANCE	7

/*
 * The platforms whose workarounds differ.
 *
 * Every Gen12 device the driver matches shares one set, apart from the
 * rules its product row lists in I915_PLATFORM_WA_*; Ice Lake (Gen11) has
 * its own lists; the Gen9 parts are told apart because their workaround
 * lists and stepping rules differ.
 */
#define I915_PLATFORM_GEN12		0
#define I915_PLATFORM_SKYLAKE		1
#define I915_PLATFORM_KABYLAKE		2
#define I915_PLATFORM_COFFEELAKE	3
#define I915_PLATFORM_COMETLAKE		4
#define I915_PLATFORM_GEN11		5

/*
 * The rules of the shared Gen12 lists that only some platforms take, as
 * bits of i915_gt_info's platform_wa.
 *
 * RENDER_PSMI keeps the render engine from powering down while it waits
 * for an event and from sending the semaphore idle message (Wa_1607297627).
 */
#define I915_PLATFORM_WA_RENDER_PSMI	(1U << 0)

/*
 * The slice, subslice and EU layout that survived fusing.
 *
 * It is filled once by the GT information step and only read afterwards; the
 * context images and the workarounds are built from it.
 */
struct i915_sseu {
	/* The layout the hardware generation allows at most. */
	uint8_t max_slices;
	uint8_t max_subslices;
	uint8_t max_eus_per_subslice;

	/* The slices that are present. */
	uint8_t slice_mask;

	/*
	 * The (dual-)subslices of the lowest present slice.  Gen11 and Gen12
	 * have one slice; on Gen9 the subslice fuse applies to every slice
	 * alike.
	 */
	uint16_t subslice_mask;

	/* The EUs present in each subslice of the lowest present slice. */
	uint16_t eu_mask[I915_SSEU_MAX_SUBSLICES];

	/* How many EUs one subslice has, and how many the whole GT has. */
	uint16_t eu_per_subslice;
	uint16_t eu_total;

	/* Nonzero when the hardware can power-gate a whole slice. */
	int has_slice_pg;

	/* Nonzero once the fuses have been decoded. */
	int valid;
};

/*
 * One engine the fuses left enabled.
 *
 * It holds what is known of the engine before any submission exists.  It
 * lives inside the GT information for the life of
 * the device; the execution side points at it rather than copying it.
 */
struct i915_engine_info {
	/* The engine identifier, one of I915_RCS0 and the rest. */
	int id;

	/*
	 * The graphics version of the GT the engine belongs to: 9 picks the
	 * Gen9 context layout, submission ports and commands; any other value
	 * the Gen12 ones.
	 */
	int graphics_ver;

	/* The engine class and the instance within that class. */
	int class;
	int instance;

	/* Where the engine's register block starts. */
	uint32_t mmio_base;

	/* The engine's bit in the engine mask. */
	uint32_t mask;

	/* The GDRST bit that resets this engine alone. */
	uint32_t reset_domain;

	/* How large the engine's logical ring context image is. */
	uint32_t context_size;

	/* The instance number user space sees for this engine. */
	unsigned logical_instance;

	/* The engine's name, a static string. */
	const char *name;

	/* The optional features user space may use on this engine. */
	uint32_t uabi_capabilities;

	/* Nonzero for a slot that holds a built engine. */
	int in_use;
};

/*
 * The GT information of one device.
 *
 * It is filled by drv_i915_gt_init_mmio() during the device start and only
 * read afterwards.  The owner keeps it for the life of the device.
 */
struct i915_gt_info {
	/* The graphics version the information was read for: 9, 11 or 12. */
	int graphics_ver;

	/*
	 * The platform (I915_PLATFORM_*) and the PCI revision, which is the
	 * stepping some Gen9 workarounds depend on.  The device start fills
	 * both from the PCI configuration space before this step.
	 */
	int platform;
	unsigned revision;

	/* The workaround rules of the shared lists the platform takes (I915_PLATFORM_WA_*), filled with the platform. */
	uint32_t platform_wa;

	/* The command streamer timestamp frequency in Hz, and its period. */
	uint32_t clock_frequency;
	uint32_t clock_period_ns;

	/* Nonzero when the timestamp frequency could be determined. */
	int clock_valid;

	/* The slices, subslices and EUs that survived fusing. */
	struct i915_sseu sseu;

	/* The L3 banks that replicated registers may be steered to. */
	uint32_t l3bank_mask;

	/* Nonzero once the L3 bank steering table applies. */
	int l3bank_steering;

	/* The engines that survived fusing, in table order. */
	struct i915_engine_info engines[I915_MAX_ENGINES];
	unsigned num_engines;

	/* The engine mask after the media fuses were applied. */
	uint32_t engine_mask;

	/* The video decode instances that can reach a scaler and format converter. */
	uint32_t vdbox_sfc_access;

	/* The instances whose scaler and format converter are present. */
	uint32_t sfc_mask;

	/* Nonzero when a GPU fault was pending at start, and the fault register seen. */
	int fault_valid_seen;
	uint32_t fault_reg;

	/* Nonzero once the information has been filled. */
	int inited;
};

int drv_i915_gt_init_mmio(struct i915_gt_info *gt, int graphics_ver, uint32_t platform_engine_mask, struct i915_mmio *mmio);

#endif
