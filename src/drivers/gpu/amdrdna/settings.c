/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The settings the amdrdna driver gives the graphics engine before it
 * starts the command processor and the copy engines, and the words it
 * derives from the engine's shape.
 *
 * The register settings are the hardware's recommended values (a mask of
 * the bits to change and the value of those bits).  The clear state is
 * the context state the run-list controller and the graphics ring reset
 * to.  The shape words follow from the render back ends and workgroup
 * processors the fuses leave.  Nothing here touches the hardware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/* The register settings of the graphics engine and of the copy engines. */
#define SETTINGS_GRAPHICS		40U
#define SETTINGS_COPY			26U

/* The runs of context registers the clear state covers, and the registers in it whose value is not 0. */
#define SETTINGS_CLEAR_RUNS		8U
#define SETTINGS_CLEAR_VALUES		85U

/*
 * The context register of the tile steering (an absolute index), and the
 * fields of its value: the scan converters (log2 at bit 12), the render
 * back ends per converter (log2 at bit 16) and the packers per converter
 * (log2 at bit 20).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmPA_SC_TILE_STEERING_OVERRIDE, segment 1) and gc_10_1_0_sh_mask.h
 * (PA_SC_TILE_STEERING_OVERRIDE__NUM_SC_MASK,
 * PA_SC_TILE_STEERING_OVERRIDE__NUM_RB_PER_SC_MASK,
 * PA_SC_TILE_STEERING_OVERRIDE__NUM_PACKER_PER_SC_MASK).
 * derived: 0xa000 + 0xd7 = 0xa0d7.
 */
#define SETTINGS_STEERING_REGISTER	0xa0d7U
#define SETTINGS_STEERING_CONVERTERS_SHIFT 12U
#define SETTINGS_STEERING_CONVERTERS_MASK 0x00003000U
#define SETTINGS_STEERING_BACKENDS_SHIFT 16U
#define SETTINGS_STEERING_BACKENDS_MASK	0x00030000U
#define SETTINGS_STEERING_PACKERS_SHIFT	20U
#define SETTINGS_STEERING_PACKERS_MASK	0x00100000U

/*
 * The fuse words' fields: the render back ends switched off (23:16), the
 * workgroup processors switched off (31:16), and the cache channels
 * switched off (channels 8 and up in 31:16, 0 to 7 in 15:8).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK,
 * CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK,
 * CGTS_TCC_DISABLE__TCC_DISABLE_MASK, CGTS_TCC_DISABLE__HI_TCC_DISABLE_MASK).
 */
#define SETTINGS_BACKENDS_MASK		0x00ff0000U
#define SETTINGS_BACKENDS_SHIFT		16U
#define SETTINGS_GROUPS_MASK		0xffff0000U
#define SETTINGS_GROUPS_SHIFT		16U
#define SETTINGS_CHANNELS_LOW_MASK	0xffff0000U
#define SETTINGS_CHANNELS_LOW_SHIFT	16U
#define SETTINGS_CHANNELS_HIGH_MASK	0x0000ff00U
#define SETTINGS_CHANNELS_HIGH_SHIFT	8U

/*
 * The shader memory mode of every context: 64-bit addresses, unaligned
 * accesses allowed (3 at bit 2), the retry of every fault (0 at bit 12),
 * and the instruction prefetch (3 at bit 14); and the apertures of the
 * contexts above 0 (the private aperture at 0x1000 << 48 in bits 15:0,
 * the shared one at 0x2000 << 48 in bits 31:16).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (DEFAULT_SH_MEM_CONFIG),
 * gc_10_1_0_sh_mask.h (SH_MEM_CONFIG__ALIGNMENT_MODE__SHIFT,
 * SH_MEM_CONFIG__INITIAL_INST_PREFETCH__SHIFT, SH_MEM_BASES__SHARED_BASE__SHIFT),
 * navi10_enum.h (SH_MEM_ALIGNMENT_MODE_UNALIGNED) and gmc_v10_0.c
 * (gmc_v10_0_early_init: the private and shared aperture starts).
 * derived: 3 << 2 | 3 << 14 = 0xc00c; 0x1000 | 0x2000 << 16 = 0x20001000.
 */
#define SETTINGS_SHADER_MEMORY_MODE	0x0000c00cU
#define SETTINGS_SHADER_APERTURES	0x20001000U

/*
 * The graphics engine's settings, as the GC window, the register's
 * offset in it, the bits to change (all of them when every bit is set)
 * and their value.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (golden_settings_gc_10_1) and
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h (the
 * registers' offsets and segments).
 */
static const struct amdrdna_register_setting settings_graphics[SETTINGS_GRAPHICS] = {
	{ 0U, 0x1422U, 0xffffffffU, 0x00400014U },
	{ 1U, 0x50b1U, 0xfcff8fffU, 0xf8000100U },
	{ 1U, 0x5080U, 0xcd000000U, 0x0d000100U },
	{ 1U, 0x508cU, 0x60000ff0U, 0x60000100U },
	{ 1U, 0x508dU, 0x40000000U, 0x40000100U },
	{ 1U, 0x5084U, 0xffff8fffU, 0xffff8100U },
	{ 1U, 0x5086U, 0xfeff8fffU, 0xfeff8100U },
	{ 1U, 0x2d90U, 0xffffffffU, 0xe4e4e4e4U },
	{ 1U, 0x2d94U, 0x00000002U, 0x00000000U },
	{ 0U, 0x1f57U, 0x000007ffU, 0x000005ffU },
	{ 0U, 0x13acU, 0x20000000U, 0x20000000U },
	{ 0U, 0x13adU, 0xffffffffU, 0x00000420U },
	{ 0U, 0x13aeU, 0x00000200U, 0x00000200U },
	{ 0U, 0x13afU, 0x07900000U, 0x04900000U },
	{ 0U, 0x13d2U, 0x0000ffffU, 0x0000003fU },
	{ 0U, 0x13baU, 0xffffffffU, 0x03860204U },
	{ 0U, 0x1583U, 0x1ff0ffffU, 0x00000500U },
	{ 0U, 0x1004U, 0x000007ffU, 0x000001feU },
	{ 1U, 0x2d10U, 0xffffffffU, 0xe4e4e4e4U },
	{ 1U, 0x2e25U, 0x77777777U, 0x10321032U },
	{ 1U, 0x2e26U, 0x77777777U, 0x02310231U },
	{ 1U, 0x2e21U, 0xffffffffU, 0xffffffcfU },
	{ 1U, 0x2e03U, 0xffffffffU, 0xffffffcfU },
	{ 1U, 0x50acU, 0x10000000U, 0x10000100U },
	{ 1U, 0x2e01U, 0xffffffffU, 0x1402002fU },
	{ 1U, 0x2e0cU, 0xffff9fffU, 0x00001188U },
	{ 0U, 0x1070U, 0xffffffffU, 0x00000800U },
	{ 0U, 0x109cU, 0x3fffffffU, 0x08000009U },
	{ 0U, 0x109dU, 0x00400000U, 0x04440000U },
	{ 0U, 0x107cU, 0x00000800U, 0x00000820U },
	{ 1U, 0x2281U, 0x0000ff0fU, 0x00000000U },
	{ 0U, 0x153fU, 0xffffffffU, 0xffff3101U },
	{ 0U, 0x11efU, 0x001f0000U, 0x00070104U },
	{ 1U, 0x508eU, 0xffffffffU, 0xffffffffU },
	{ 0U, 0x10acU, 0x00000100U, 0x00000130U },
	{ 1U, 0x5090U, 0xffffffffU, 0xffffffffU },
	{ 0U, 0x12e2U, 0xfff7ffffU, 0x01030000U },
	{ 0U, 0x18a2U, 0x60000010U, 0x479c0010U },
	{ 1U, 0x50c3U, 0xfeff0fffU, 0x40000100U },
	{ 0U, 0x1588U, 0x00c00000U, 0x00c00000U }
};

/*
 * The copy engines' settings: the chicken bits, every queue's
 * write-pointer poll control and the page policy, of both engines, then
 * the third run-list queue's poll control of both again with the
 * Navi10 mask.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (golden_settings_sdma_5,
 * golden_settings_sdma_nv10) and
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h (the
 * registers' offsets).
 */
static const struct amdrdna_register_setting settings_copy[SETTINGS_COPY] = {
	{ 0U, 0x001dU, 0xffbf1f0fU, 0x03ab0107U },
	{ 0U, 0x0087U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x00e7U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0147U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x01a7U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0207U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0267U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x02c7U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0327U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0387U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x03e7U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0048U, 0x00ffffffU, 0x000c5c00U },
	{ 0U, 0x061dU, 0xffbf1f0fU, 0x03ab0107U },
	{ 0U, 0x0687U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x06e7U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0747U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x07a7U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0807U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0867U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x08c7U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0927U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0987U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x09e7U, 0xfffffff7U, 0x00403000U },
	{ 0U, 0x0648U, 0x00ffffffU, 0x000c5c00U },
	{ 0U, 0x0267U, 0x0000fff0U, 0x00403000U },
	{ 0U, 0x0867U, 0x0000fff0U, 0x00403000U }
};

/*
 * The runs of context registers the clear state sets: the first register
 * of each run (an absolute index) and its length.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/clearstate_gfx10.h (gfx10_SECT_CONTEXT_defs).
 */
static const struct amdrdna_register_run settings_clear_runs[SETTINGS_CLEAR_RUNS] = {
	{ 0xa000U, 215U },
	{ 0xa0d8U, 272U },
	{ 0xa1f5U, 4U },
	{ 0xa1ffU, 158U },
	{ 0xa2a0U, 2U },
	{ 0xa2a3U, 1U },
	{ 0xa2a5U, 66U },
	{ 0xa2f5U, 203U }
};

/*
 * The registers of the clear state whose value is not 0 (every other
 * register of the runs is 0): the register (an absolute index) and its
 * value.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/clearstate_gfx10.h (gfx10_SECT_CONTEXT_def_1
 * to gfx10_SECT_CONTEXT_def_8).
 */
static const struct amdrdna_register_value settings_clear_values[SETTINGS_CLEAR_VALUES] = {
	{ 0xa00dU, 0x40004000U },
	{ 0xa01fU, 0x00150055U },
	{ 0xa081U, 0x80000000U },
	{ 0xa082U, 0x40004000U },
	{ 0xa083U, 0x0000ffffU },
	{ 0xa085U, 0x40004000U },
	{ 0xa087U, 0x40004000U },
	{ 0xa089U, 0x40004000U },
	{ 0xa08bU, 0x40004000U },
	{ 0xa08cU, 0xaa99aaaaU },
	{ 0xa08eU, 0xffffffffU },
	{ 0xa08fU, 0xffffffffU },
	{ 0xa090U, 0x80000000U },
	{ 0xa091U, 0x40004000U },
	{ 0xa094U, 0x80000000U },
	{ 0xa095U, 0x40004000U },
	{ 0xa096U, 0x80000000U },
	{ 0xa097U, 0x40004000U },
	{ 0xa098U, 0x80000000U },
	{ 0xa099U, 0x40004000U },
	{ 0xa09aU, 0x80000000U },
	{ 0xa09bU, 0x40004000U },
	{ 0xa09cU, 0x80000000U },
	{ 0xa09dU, 0x40004000U },
	{ 0xa09eU, 0x80000000U },
	{ 0xa09fU, 0x40004000U },
	{ 0xa0a0U, 0x80000000U },
	{ 0xa0a1U, 0x40004000U },
	{ 0xa0a2U, 0x80000000U },
	{ 0xa0a3U, 0x40004000U },
	{ 0xa0a4U, 0x80000000U },
	{ 0xa0a5U, 0x40004000U },
	{ 0xa0a6U, 0x80000000U },
	{ 0xa0a7U, 0x40004000U },
	{ 0xa0a8U, 0x80000000U },
	{ 0xa0a9U, 0x40004000U },
	{ 0xa0aaU, 0x80000000U },
	{ 0xa0abU, 0x40004000U },
	{ 0xa0acU, 0x80000000U },
	{ 0xa0adU, 0x40004000U },
	{ 0xa0aeU, 0x80000000U },
	{ 0xa0afU, 0x40004000U },
	{ 0xa0b0U, 0x80000000U },
	{ 0xa0b1U, 0x40004000U },
	{ 0xa0b2U, 0x80000000U },
	{ 0xa0b3U, 0x40004000U },
	{ 0xa0b5U, 0x3f800000U },
	{ 0xa0b7U, 0x3f800000U },
	{ 0xa0b9U, 0x3f800000U },
	{ 0xa0bbU, 0x3f800000U },
	{ 0xa0bdU, 0x3f800000U },
	{ 0xa0bfU, 0x3f800000U },
	{ 0xa0c1U, 0x3f800000U },
	{ 0xa0c3U, 0x3f800000U },
	{ 0xa0c5U, 0x3f800000U },
	{ 0xa0c7U, 0x3f800000U },
	{ 0xa0c9U, 0x3f800000U },
	{ 0xa0cbU, 0x3f800000U },
	{ 0xa0cdU, 0x3f800000U },
	{ 0xa0cfU, 0x3f800000U },
	{ 0xa0d1U, 0x3f800000U },
	{ 0xa0d3U, 0x3f800000U },
	{ 0xa100U, 0xffffffffU },
	{ 0xa104U, 0x00550055U },
	{ 0xa10cU, 0x01000000U },
	{ 0xa10dU, 0x01000000U },
	{ 0xa1b6U, 0x00000002U },
	{ 0xa204U, 0x00090000U },
	{ 0xa205U, 0x00000004U },
	{ 0xa295U, 0x00000100U },
	{ 0xa296U, 0x00000080U },
	{ 0xa297U, 0x00000002U },
	{ 0xa2aaU, 0x000000ffU },
	{ 0xa2f7U, 0x00001000U },
	{ 0xa2f9U, 0x00000005U },
	{ 0xa2faU, 0x3f800000U },
	{ 0xa2fbU, 0x3f800000U },
	{ 0xa2fcU, 0x3f800000U },
	{ 0xa2fdU, 0x3f800000U },
	{ 0xa30eU, 0xffffffffU },
	{ 0xa30fU, 0xffffffffU },
	{ 0xa311U, 0x00000003U },
	{ 0xa313U, 0x00100000U },
	{ 0xa316U, 0x0000001eU },
	{ 0xa317U, 0x00000020U }
};

static unsigned settings_count_bits(uint32_t value);
static unsigned settings_order(uint32_t value);
static uint32_t settings_mask(unsigned bits);
static uint32_t settings_clear_value(uint32_t index);

/* Reports the graphics engine's settings and their count. */
const struct amdrdna_register_setting *
amdrdna_graphics_settings(
	unsigned *count)
{
	/* The table and its length. */
	*count = SETTINGS_GRAPHICS;

	/* Succeeded: the table. */
	return settings_graphics;
}

/* Reports the copy engines' settings and their count. */
const struct amdrdna_register_setting *
amdrdna_copy_settings(
	unsigned *count)
{
	/* The table and its length. */
	*count = SETTINGS_COPY;

	/* Succeeded: the table. */
	return settings_copy;
}

/*
 * Reports whether a setting needs the register's old value: only a
 * setting that changes some bits but not all does.
 */
bool
amdrdna_setting_reads(
	const struct amdrdna_register_setting *setting)
{
	/* A setting of every bit replaces the word. */
	if (setting->mask == 0xffffffffU)
		return false;

	/* Succeeded: the old value is merged. */
	return true;
}

/*
 * Reports the word a setting writes, from the register's old value.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/soc15.c (soc15_program_register_sequence).
 */
uint32_t
amdrdna_setting_word(
	const struct amdrdna_register_setting *setting,
	uint32_t old)
{
	uint32_t word;
	bool reads;

	/* A setting of every bit is its value. */
	reads = amdrdna_setting_reads(setting);
	if (!reads)
		return setting->value;

	/* The other bits of the old value, and the setting's bits. */
	word = old & ~setting->mask;
	word |= setting->value & setting->mask;

	/* Succeeded: the merged word. */
	return word;
}

/*
 * Reports the words of the clear-state stream: the bracket's begin, the
 * context loads, one register write per run, the tile steering, the
 * bracket's end and the state reset.
 */
unsigned
amdrdna_clear_state_words(void)
{
	unsigned words;
	unsigned run;

	/* The begin and the context loads. */
	words = 2U + 3U;

	/* Each run's header, offset and registers. */
	for (run = 0U; run < SETTINGS_CLEAR_RUNS; run++)
		words += 2U + settings_clear_runs[run].count;

	/* The steering, the end and the reset. */
	words += 3U + 2U + 2U;

	/* Succeeded: the stream's length. */
	return words;
}

/*
 * Writes the clear-state stream, with the tile steering the engine's
 * shape gave, into words (amdrdna_clear_state_words() of them).  The
 * run-list controller reads the same stream from memory as the graphics
 * ring's preamble does.  Reports the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_get_csb_buffer,
 * gfx_v10_0_cp_gfx_start) and amdgpu_gfx.c (amdgpu_gfx_csb_preamble_start,
 * amdgpu_gfx_csb_data_parser, amdgpu_gfx_csb_preamble_end).
 */
unsigned
amdrdna_clear_state(
	uint32_t *words,
	uint32_t steering)
{
	uint32_t values[AMDRDNA_CLEAR_RUN_LONGEST];
	const struct amdrdna_register_run *run;
	unsigned count;
	unsigned index;
	unsigned element;

	/* The bracket's begin and the context loads. */
	count = amdrdna_pm4_bracket(words, true);
	count += amdrdna_pm4_context_loads(words + count);

	/* One register write per run, its registers 0 but where the table says otherwise. */
	for (index = 0U; index < SETTINGS_CLEAR_RUNS; index++) {
		run = &settings_clear_runs[index];
		for (element = 0U; element < run->count; element++)
			values[element] = settings_clear_value(run->first + element);
		count += amdrdna_pm4_registers(words + count, AMDRDNA_SPACE_CONTEXT, run->first, values, run->count, false);
	}

	/* The tile steering. */
	values[0] = steering;
	count += amdrdna_pm4_registers(words + count, AMDRDNA_SPACE_CONTEXT, SETTINGS_STEERING_REGISTER, values, 1U, false);

	/* The bracket's end and the state reset. */
	count += amdrdna_pm4_bracket(words + count, false);
	count += amdrdna_pm4_state_reset(words + count);

	/* Succeeded: the stream's length. */
	return count;
}

/*
 * Reports the render back ends of one shader array that are on: the
 * fused-off and switched-off words read with the array selected, and the
 * back ends one array has.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_get_rb_active_bitmap).
 */
uint32_t
amdrdna_backends_on(
	uint32_t fused,
	uint32_t off,
	unsigned per_array)
{
	uint32_t disabled;
	uint32_t on;

	/* The back ends either word turns off. */
	disabled = (fused | off) & SETTINGS_BACKENDS_MASK;
	disabled >>= SETTINGS_BACKENDS_SHIFT;

	/* The others, of the array's own. */
	on = ~disabled & settings_mask(per_array);

	/* Succeeded: one bit per back end that is on. */
	return on;
}

/*
 * Reports the workgroup processors of one shader array that are on: the
 * fused-off and switched-off words read with the array selected, and the
 * processors one array has.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c
 * (gfx_v10_0_get_wgp_active_bitmap_per_sh).
 */
uint32_t
amdrdna_groups_on(
	uint32_t fused,
	uint32_t off,
	unsigned per_array)
{
	uint32_t disabled;
	uint32_t on;

	/* The processors either word turns off. */
	disabled = (fused & SETTINGS_GROUPS_MASK) >> SETTINGS_GROUPS_SHIFT;
	disabled |= (off & SETTINGS_GROUPS_MASK) >> SETTINGS_GROUPS_SHIFT;

	/* The others, of the array's own. */
	on = ~disabled & settings_mask(per_array);

	/* Succeeded: one bit per processor that is on. */
	return on;
}

/*
 * Reports the compute units the processors that are on hold: two each.
 */
unsigned
amdrdna_units_on(
	uint32_t groups)
{
	unsigned units;

	/* Two per processor. */
	units = 2U * settings_count_bits(groups);

	/* Succeeded: the units. */
	return units;
}

/*
 * Reports the level-2 cache channels that are off (channel n in bit n),
 * from the fused-off and switched-off words.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_get_tcc_info).
 */
uint32_t
amdrdna_channels_off(
	uint32_t fused,
	uint32_t off)
{
	uint32_t either;
	uint32_t channels;

	/* Either word turns a channel off. */
	either = fused | off;

	/* Channels 0 to 15 from the low field, 16 and up from the high one. */
	channels = (either & SETTINGS_CHANNELS_LOW_MASK) >> SETTINGS_CHANNELS_LOW_SHIFT;
	channels |= ((either & SETTINGS_CHANNELS_HIGH_MASK) >> SETTINGS_CHANNELS_HIGH_SHIFT) << 16;

	/* Succeeded: one bit per channel that is off. */
	return channels;
}

/*
 * Reports the tile steering from the engine's shape: the scan converters
 * (shader engines times arrays per engine times converters per array),
 * the render back ends on in one array per converter, and the packers per
 * converter.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c
 * (gfx_v10_0_init_pa_sc_tile_steering_override).
 */
uint32_t
amdrdna_tile_steering(
	unsigned converters,
	unsigned backends_per_converter,
	unsigned packers)
{
	uint32_t word;

	/* The three counts, as log2 rounded up, in their fields. */
	word = ((uint32_t)settings_order(converters) << SETTINGS_STEERING_CONVERTERS_SHIFT) & SETTINGS_STEERING_CONVERTERS_MASK;
	word |= ((uint32_t)settings_order(backends_per_converter) << SETTINGS_STEERING_BACKENDS_SHIFT) & SETTINGS_STEERING_BACKENDS_MASK;
	word |= ((uint32_t)settings_order(packers) << SETTINGS_STEERING_PACKERS_SHIFT) & SETTINGS_STEERING_PACKERS_MASK;

	/* Succeeded: the steering word. */
	return word;
}

/*
 * Reports the words that keep invalidations away from the workgroup
 * processors of one shader array that are off: the translation word and
 * the cache-target word, from their old values, the processors that are
 * on and the processors one array has.  Only the bits of the processors
 * (and of their instruction caches) change.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_tcp_harvest).
 */
void
amdrdna_harvest_words(
	uint32_t groups,
	unsigned per_array,
	uint32_t old_translation,
	uint32_t old_targets,
	uint32_t *translation,
	uint32_t *targets)
{
	uint32_t translation_skip;
	uint32_t target_skip;
	uint32_t translation_mask;
	uint32_t target_mask;
	unsigned group;

	/*
	 * The fields' widths: the translation word has two bits per
	 * processor's vector cache, two per instruction cache, four for the
	 * memory interface and one for the scalar cache; the target word two
	 * per vector cache, one per instruction cache and four for the
	 * level-1 caches.
	 */
	translation_mask = settings_mask(2U * per_array + 2U * per_array + 4U + 1U);
	target_mask = settings_mask(2U * per_array + per_array + 4U);

	/* The bits of each processor that is off. */
	translation_skip = 0U;
	target_skip = 0U;
	for (group = 0U; group < per_array; group++) {
		/* A processor that is on keeps its bits clear. */
		if ((groups & (1U << group)) != 0U)
			continue;

		/* Its vector cache and instruction cache. */
		target_skip |= 3U << (2U * group);
		target_skip |= 1U << (group + 2U * per_array);
		translation_skip |= 3U << (2U * group);
		translation_skip |= 3U << (2U * (per_array + group));
	}

	/* The old words with only the processors' bits replaced. */
	*translation = old_translation & (0xffffffffU << (4U * per_array));
	*translation |= translation_skip & translation_mask;
	*targets = old_targets & (0xffffffffU << (3U * per_array));
	*targets |= target_skip & target_mask;
}

/* Reports the shader memory mode every context gets. */
uint32_t
amdrdna_shader_memory_mode(void)
{
	/* The fixed mode. */
	return SETTINGS_SHADER_MEMORY_MODE;
}

/* Reports the shader apertures the contexts above 0 get. */
uint32_t
amdrdna_shader_apertures(void)
{
	/* The fixed apertures. */
	return SETTINGS_SHADER_APERTURES;
}

/* Counts the bits set in a word. */
static unsigned
settings_count_bits(
	uint32_t value)
{
	unsigned count;

	/* Clears the lowest set bit until none is left. */
	count = 0U;
	while (value != 0U) {
		value &= value - 1U;
		count++;
	}

	/* Succeeded: the count. */
	return count;
}

/* Reports log2 of a count rounded up (0 for 0 and 1). */
static unsigned
settings_order(
	uint32_t value)
{
	unsigned bits;

	/* Counts the bits up to the value. */
	bits = 0U;
	while (((uint32_t)1U << bits) < value)
		bits++;

	/* Succeeded: the exponent. */
	return bits;
}

/* Reports a mask of the low bits bits. */
static uint32_t
settings_mask(
	unsigned bits)
{
	/* Every bit for a full word. */
	if (bits >= 32U)
		return 0xffffffffU;

	/* Succeeded: the low bits. */
	return ((uint32_t)1U << bits) - 1U;
}

/* Reports the clear-state value of one context register (0 for most). */
static uint32_t
settings_clear_value(
	uint32_t index)
{
	unsigned value;

	/* Looks the register up among those that are not 0. */
	for (value = 0U; value < SETTINGS_CLEAR_VALUES; value++) {
		if (settings_clear_values[value].index == index)
			return settings_clear_values[value].value;
	}

	/* Succeeded: the register is 0. */
	return 0U;
}
