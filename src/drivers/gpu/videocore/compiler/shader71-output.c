/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Native stage epilogues for V3D 7.1, the Raspberry Pi 5's BCM2712.
 *
 * This is the 7.1 copy of shader-output.c; the board's source list selects
 * it in place of that file.  It differs in four places: a vertex program
 * rounds its fixed-point XY to the nearest even instead of down and ends
 * without the VPM write wait that only 4.2 needs, the two colour halves
 * of a fragment program never read two small immediates in one word, and a
 * fragment program writes up to four render targets, as halves or 32-bit
 * words.
 */
#include <stdbool.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/compiler/shader-private.h"


static int vertex_last_switch(struct bcm2711_shader_compiler *compiler);
static int vertex_output(struct bcm2711_shader_compiler *compiler);
static int fragment_output(struct bcm2711_shader_compiler *compiler);
static int depth_output(struct bcm2711_shader_compiler *compiler, bool rasterized);
static int target_output(struct bcm2711_shader_compiler *compiler, uint32_t target);
static int output_operand(struct bcm2711_shader_compiler *compiler, uint32_t output, uint32_t scratch, struct bcm2711_qpu_source *source);
static int vpm_store(struct bcm2711_shader_compiler *compiler, uint32_t index, struct bcm2711_qpu_source source);
static int viewport_component(struct bcm2711_shader_compiler *compiler, uint32_t component);
static int point_size(struct bcm2711_shader_compiler *compiler, uint32_t index);
static int output_register(struct bcm2711_shader_compiler *compiler, struct bcm2711_qpu_source *source);

/*
 * Emits complete native stage outputs and the final program switch with both explicit delay slots.
 */
int
bcm2711_shader_finish(
	struct bcm2711_shader_compiler *compiler)
{
	int error;

	/* Fragment tile output and vertex VPM output use different completion protocols; a compute program waits for its stores. */
	if (compiler->binary->stage == BCM2711_SHADER_FRAGMENT) {
		error = fragment_output(compiler);
	} else if (compiler->binary->stage == BCM2711_SHADER_COMPUTE) {
		error = bcm2711_shader_compute_finish(compiler);
	} else {
		error = vertex_last_switch(compiler);
		if (error == 0)
			error = vertex_output(compiler);
	}

	/* A failed stage output cannot acquire a successful program terminator. */
	if (error != 0)
		return error;

	/* The final thread switch ends the program only after its two uploaded delay slots execute. */
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_END);
	if (error != 0)
		return error;

	/* No native execution or implicit delay word extends past the program allocation. */
	return 0;
}

/*
 * Ends a vertex program's thread switches before its VPM output: its
 * storage buffer stores are waited for, and a program that switched
 * marks its last switch with a second one right after it, so that the
 * thread end follows.  A program that never switched starts in its last
 * section (starts_final).
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c
 * (vir_emit_last_thrsw before emit_vert_end; only a program without a
 * switch skips the marked one).
 */
static int
vertex_last_switch(
	struct bcm2711_shader_compiler *compiler)
{
	int error;

	/* The stores complete before the thread ends. */
	error = bcm2711_shader_storage_wait(compiler);
	if (error != 0)
		return error;

	/* Nothing to mark without a switch. */
	if (compiler->switches == 0)
		return 0;

	/* Two switches in a row mark the last one. */
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_LAST_FIRST);
	if (error != 0)
		return error;
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_LAST_SECOND);
	if (error != 0)
		return error;

	/* Succeeded: the VPM output and the thread end follow. */
	return 0;
}

/* Exports the exact coordinate or render vertex VPM header followed by canonical render varyings. */
static int
vertex_output(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_qpu_source source;
	struct bcm2711_qpu_source ignored;
	const struct bcm2711_shader_component *component;
	uint32_t index;
	uint32_t output;
	uint32_t base;
	int error;

	/* Clip W's reciprocal remains in scratch sixty-three throughout the viewport epilogue. */
	error = bcm2711_shader_operand(compiler, compiler->outputs[BCM2711_SHADER_POSITION + 3], BCM2711_SHADER_SCRATCH_LEFT, &source);
	if (error != 0)
		return error;
	ignored = bcm2711_shader_bits(0);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_RECIPROCAL, BCM2711_SHADER_SCRATCH_AUX, 0, source, ignored, 0, 0);
	if (error != 0)
		return error;

	/* Coordinate/bin programs begin with all four homogeneous clip-position words. */
	base = 0;
	if (compiler->binary->stage == BCM2711_SHADER_COORDINATE) {
		for (index = 0; index < 4; index++) {
			error = bcm2711_shader_operand(compiler, compiler->outputs[BCM2711_SHADER_POSITION + index], BCM2711_SHADER_SCRATCH_LEFT, &source);
			if (error != 0)
				return error;
			error = vpm_store(compiler, index, source);
			if (error != 0)
				return error;
		}

		/* The coordinate viewport header follows the four completed clip-position words. */
		base = 4;
	}

	/* Both vertex variants export viewport XY as .8 fixed-point values rounded to the nearest even. */
	for (index = 0; index < 2; index++) {
		error = viewport_component(compiler, index);
		if (error != 0)
			return error;
		source = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
		error = vpm_store(compiler, base + index, source);
		if (error != 0)
			return error;
	}

	/* A coordinate header ends after clip-position plus XY, and the point size of a points pipeline; it exports no render varyings. */
	compiler->binary->vpm_output_words = 6;
	if (compiler->binary->stage == BCM2711_SHADER_COORDINATE && compiler->binary->point_size != 0) {
		error = point_size(compiler, 6);
		if (error != 0)
			return error;
		compiler->binary->vpm_output_words = 7;
	}
	if (compiler->binary->stage == BCM2711_SHADER_VERTEX) {
		/* The render header additionally carries scaled depth and reciprocal clip W. */
		error = viewport_component(compiler, 2);
		if (error != 0)
			return error;
		source = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
		error = vpm_store(compiler, 2, source);
		if (error != 0)
			return error;
		source = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_AUX);
		error = vpm_store(compiler, 3, source);
		if (error != 0)
			return error;

		/* A points pipeline's point size follows the four render-header words. */
		base = 4;
		if (compiler->binary->point_size != 0) {
			error = point_size(compiler, 4);
			if (error != 0)
				return error;
			base = 5;
		}

		/* Varyings follow the header in the exact fragment FIFO order. */
		for (index = 0; index < compiler->binary->varying_count; index++) {
			component = &compiler->binary->varyings[index];
			output = component->location * 4 + component->component;
			error = bcm2711_shader_operand(compiler, compiler->outputs[output], BCM2711_SHADER_SCRATCH_LEFT, &source);
			if (error != 0)
				return error;
			error = vpm_store(compiler, base + index, source);
			if (error != 0)
				return error;
		}

		/* The shader record must allocate exactly this complete render output header and varying extent. */
		compiler->binary->vpm_output_words = base + compiler->binary->varying_count;
	}

	/*
	 * Unlike 4.2, 7.1 needs no wait for the VPM stores before the program
	 * ends (Mesa 25.3.6 src/broadcom/compiler/nir_to_vir.c lines 2070-2085:
	 * GFXH-1684 is a 4.2 erratum).
	 */
	/* Every header and varying word has reached the native VPM output transaction. */
	return 0;
}

/*
 * Stores the point size a shader wrote as VPM word index, no smaller than
 * one pixel, as Mesa 26.2.6's V3D compiler keeps it
 * (src/broadcom/compiler/vir.c: nir_lower_point_size with a minimum of
 * 1.0; v3d_nir_lower_io.c: the size follows the header).
 */
static int
point_size(
	struct bcm2711_shader_compiler *compiler,
	uint32_t index)
{
	struct bcm2711_qpu_source size;
	struct bcm2711_qpu_source one;
	int error;

	/*
	 * The larger of the shader's size and 1.0, in the work scratch register;
	 * the size is moved to a register first, as one word cannot read two
	 * small immediates (a constant size is one).
	 */
	error = bcm2711_shader_operand(compiler, compiler->outputs[BCM2711_SHADER_POINT_SIZE], BCM2711_SHADER_SCRATCH_LEFT, &size);
	if (error != 0)
		return error;
	one = bcm2711_shader_bits(0x3f800000U);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, BCM2711_SHADER_SCRATCH_LEFT, 0, size, bcm2711_shader_bits(0), 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MAXIMUM, BCM2711_SHADER_SCRATCH_WORK, 0, bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_LEFT), one, 0, 0);
	if (error != 0)
		return error;

	/* The size goes to its VPM word. */
	error = vpm_store(compiler, index, bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK));
	if (error != 0)
		return error;

	/* Succeeded: the size is in its VPM word. */
	return 0;
}

/* Converts a homogeneous position component through the pipeline's draw-time viewport uniforms. */
static int
viewport_component(
	struct bcm2711_shader_compiler *compiler,
	uint32_t component)
{
	struct bcm2711_qpu_source clip;
	struct bcm2711_qpu_source scale;
	struct bcm2711_qpu_source transformed;
	struct bcm2711_qpu_source reciprocal;
	struct bcm2711_shader_uniform uniform;
	int error;

	/* Each component reads its still-live clip scalar without replacing reciprocal W's reserved scratch register. */
	error = bcm2711_shader_operand(compiler, compiler->outputs[BCM2711_SHADER_POSITION + component], BCM2711_SHADER_SCRATCH_LEFT, &clip);
	if (error != 0)
		return error;
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_VIEWPORT_X;
	if (component == 1)
		uniform.kind = BCM2711_SHADER_VIEWPORT_Y;
	if (component == 2)
		uniform.kind = BCM2711_SHADER_VIEWPORT_Z;
	error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_RIGHT);
	if (error != 0)
		return error;

	/* The caller supplies XY scale already multiplied by 256 for the native fixed-point convention. */
	scale = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_RIGHT);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, BCM2711_SHADER_SCRATCH_WORK, 0, clip, scale, 0, 0);
	if (error != 0)
		return error;
	transformed = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
	reciprocal = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_AUX);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, BCM2711_SHADER_SCRATCH_WORK, 0, transformed, reciprocal, 0, 0);
	if (error != 0)
		return error;

	/* Depth remains floating point and receives its explicit viewport offset. */
	if (component == 2) {
		uniform.kind = BCM2711_SHADER_DEPTH_OFFSET;
		error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_RIGHT);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_ADD, BCM2711_SHADER_SCRATCH_WORK, 0, transformed, scale, 0, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/*
	 * The .8 fixed-point XY is rounded to the nearest even before the
	 * conversion; 7.1 has none of 4.2's double-rounding quirk, which needed a
	 * floor (Mesa 25.3.6 src/broadcom/compiler/v3d_nir_lower_io.c lines 528-541).
	 */
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_ROUND, BCM2711_SHADER_SCRATCH_WORK, 0, transformed, scale, 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_TO_SIGNED, BCM2711_SHADER_SCRATCH_WORK, 0, transformed, scale, 0, 0);
	if (error != 0)
		return error;

	/* The viewport component is ready for its native VPM header slot. */
	return 0;
}

/* Writes one checked scalar to an explicit VPM output index without replacing the source scratch register. */
static int
vpm_store(
	struct bcm2711_shader_compiler *compiler,
	uint32_t index,
	struct bcm2711_qpu_source source)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source offset;
	int error;

	/* Native VPM indices are full-width uniform bits, avoiding a second competing small constant port. */
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_CONSTANT;
	uniform.bits = index;
	error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_RIGHT);
	if (error != 0)
		return error;
	offset = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_RIGHT);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_VPM_STORE, 0, 0, offset, source, 0, 0);
	if (error != 0)
		return error;

	/* The store names a scalar index, never an unchecked memory address. */
	return 0;
}

/*
 * Acquires the tile scoreboard and exports each render target's colour; any blending happens in the tile buffer.
 *
 * Each target the key names is written from the outputs of its location,
 * as Mesa's vir_emit_tlb_color_write writes it: the first write of a
 * target carries the tile write configuration (TLBU) unless it is the
 * default one (target 0 as four halves, per pixel, which a plain TLB write
 * takes), the others continue it.  A program that writes gl_FragDepth
 * writes the depth first; one that discards writes the rasterized depth
 * first, so the tile buffer tests and updates the depth and stencil of the
 * fragments the program keeps (Mesa's emit_frag_end does both for a
 * program without early fragment tests, which a discard rules out).  A
 * program that writes neither a target nor a depth still writes target 0,
 * as every fragment program makes a tile write.
 *
 * Values referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c
 * (vir_emit_tlb_color_write and the TLB_* configuration bits: type F16
 * 3 << 6 or F32 0 << 6, render target 7 - n at bit 3, per-pixel 1 << 2,
 * half swap 1 << 1, four halves 1 << 0 or the vector size minus one).
 */
static int
fragment_output(
	struct bcm2711_shader_compiler *compiler)
{
	uint32_t count;
	uint32_t target;
	uint32_t written;
	uint32_t index;
	int error;

	/* The last switch before the tile writes is marked by a second switch in its first delay slot. */
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_LAST_FIRST);
	if (error != 0)
		return error;
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_LAST_SECOND);
	if (error != 0)
		return error;

	/* The program's depth, or the rasterized one of a program that discards, ahead of the colour. */
	written = 0;
	if (compiler->outputs[BCM2711_SHADER_FRAG_DEPTH] != BCM2711_SHADER_ABSENT) {
		error = depth_output(compiler, false);
		if (error != 0)
			return error;
		written++;
	} else if (compiler->discards != 0) {
		error = depth_output(compiler, true);
		if (error != 0)
			return error;
		compiler->binary->writes_depth = 1;
		written++;
	}

	/* The targets the key names: one, target 0, by default. */
	count = compiler->key->target_count;
	if (count == 0)
		count = 1;
	if (count > BCM2711_SHADER_TARGETS)
		return EINVAL;

	/* Each target the program writes: one with an output at its location. */
	for (target = 0; target < count; target++) {
		if ((compiler->key->target_unused & (1U << target)) != 0)
			continue;
		for (index = 0; index < 4U; index++) {
			if (compiler->outputs[target * 4U + index] != BCM2711_SHADER_ABSENT)
				break;
		}
		if (index == 4U)
			continue;
		error = target_output(compiler, target);
		if (error != 0)
			return error;
		written++;
	}

	/* A program that writes neither a target nor a depth still makes its tile write, to target 0. */
	if (written == 0) {
		error = target_output(compiler, 0);
		if (error != 0)
			return error;
	}

	/* The native tile transaction contains every completed colour write. */
	return 0;
}

/*
 * Writes the fragment's depth: one TLBU word, configured as a depth write
 * of the program's value (the 32-bit float of its gl_FragDepth output), or
 * when `rasterized` of the depth the rasterizer gave the fragment (the
 * word's value unused), per pixel, as Mesa's emit_frag_end writes them.
 *
 * Values referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c
 * (emit_frag_end and TLB_TYPE_DEPTH 2 << 6, TLB_V42_DEPTH_TYPE_PER_PIXEL
 * 1 << 3 or TLB_V42_DEPTH_TYPE_INVARIANT 0 << 3, TLB_SAMPLE_MODE_PER_PIXEL
 * 1 << 2, with 0xffffff00).
 */
static int
depth_output(
	struct bcm2711_shader_compiler *compiler,
	bool rasterized)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source depth;
	int error;

	/* The configuration: a per-pixel depth, the program's value or the rasterized one. */
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_CONSTANT;
	uniform.bits = 0xffffff00U | (2U << 6) | (1U << 2);
	if (!rasterized)
		uniform.bits |= 1U << 3;

	/* The depth's register, or its constant; the rasterized depth's word carries nothing. */
	depth = bcm2711_shader_bits(0);
	if (!rasterized) {
		error = bcm2711_shader_operand(compiler, compiler->outputs[BCM2711_SHADER_FRAG_DEPTH], BCM2711_SHADER_SCRATCH_LEFT, &depth);
		if (error != 0)
			return error;
	}

	/* The configured tile write, its configuration next in the uniform stream. */
	error = bcm2711_shader_uniform_append(compiler, &uniform);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, 8U, 1, depth, bcm2711_shader_bits(0), 0, 0);
	if (error != 0)
		return error;

	/* Succeeded: the depth is written. */
	return 0;
}

/*
 * Writes one render target's colour from the outputs of its location: four
 * half floats in two words (red and green, then blue and alpha), or its
 * channels as 32-bit words for a wide target.  Physical channel order is
 * applied as the colour is packed; a component the program leaves out is
 * zero.
 */
static int
target_output(
	struct bcm2711_shader_compiler *compiler,
	uint32_t target)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source right;
	uint32_t configuration;
	uint32_t destination;
	uint32_t components;
	uint32_t index;
	uint32_t component;
	uint32_t swap;
	int error;

	/* The tile write configuration: render target 7 - n, per pixel. */
	swap = (compiler->key->swap_red_blue >> target) & 1U;
	configuration = 0xffffff00U | (1U << 2) | ((7U - target) << 3);

	/* A wide target: each channel's 32 bits in a word of its own, the first carrying the configuration. */
	if ((compiler->key->target_wide & (1U << target)) != 0) {
		components = compiler->key->target_components[target];
		if (components == 0 || components > 4U)
			return EINVAL;
		configuration |= components - 1U;
		for (index = 0; index < components; index++) {
			component = index;
			if (swap != 0 && index == 0)
				component = 2;
			if (swap != 0 && index == 2)
				component = 0;
			error = output_operand(compiler, target * 4U + component, BCM2711_SHADER_SCRATCH_LEFT, &left);
			if (error != 0)
				return error;
			destination = 7U;
			if (index == 0) {
				kern_memset(&uniform, 0, sizeof(uniform));
				uniform.kind = BCM2711_SHADER_CONSTANT;
				uniform.bits = configuration;
				error = bcm2711_shader_uniform_append(compiler, &uniform);
				if (error != 0)
					return error;
				destination = 8U;
			}
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 1, left, bcm2711_shader_bits(0), 0, 0);
			if (error != 0)
				return error;
		}

		/* Succeeded: the target's channels are written. */
		return 0;
	}

	/*
	 * Four halves in two words; the default configuration (target 0) needs
	 * no uniform.  The packs read the source outputs where they live; a
	 * constant output is materialized, and a 7.1 word reads one small
	 * immediate, so of two constants one moves to a register even when
	 * they are equal.
	 */
	configuration |= (3U << 6) | (1U << 1) | 1U;
	for (index = 0; index < 2; index++) {
		component = index * 2;
		if (swap != 0)
			component = 2 - index * 2;
		error = output_operand(compiler, target * 4U + component, BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		error = output_operand(compiler, target * 4U + index * 2 + 1, BCM2711_SHADER_SCRATCH_RIGHT, &right);
		if (error != 0)
			return error;
		if (left.kind == BCM2711_QPU_CONSTANT &&
		    right.kind == BCM2711_QPU_CONSTANT) {
			error = output_register(compiler, &left);
			if (error != 0)
				return error;
		}

		/* The first pack of a target other than the default one carries its configuration. */
		destination = 7U;
		if (index == 0 && configuration != 0xffffffffU) {
			kern_memset(&uniform, 0, sizeof(uniform));
			uniform.kind = BCM2711_SHADER_CONSTANT;
			uniform.bits = configuration;
			error = bcm2711_shader_uniform_append(compiler, &uniform);
			if (error != 0)
				return error;
			destination = 8U;
		}

		/* A missing native color write prevents successful fragment termination. */
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_PACK_HALVES, destination, 1, left, right, 0, 0);
		if (error != 0)
			return error;
	}

	/* Succeeded: the target's colour is written. */
	return 0;
}

/* Reads one colour output (location * 4 + component) as a source, zero when the program leaves it out. */
static int
output_operand(
	struct bcm2711_shader_compiler *compiler,
	uint32_t output,
	uint32_t scratch,
	struct bcm2711_qpu_source *source)
{
	/* A component the program does not write. */
	if (compiler->outputs[output] == BCM2711_SHADER_ABSENT) {
		*source = bcm2711_shader_bits(0);
		return 0;
	}

	/* Succeeded once the output's register or constant is the source. */
	return bcm2711_shader_operand(compiler, compiler->outputs[output], scratch, source);
}

/* Copies a small constant output into a scratch register so the pack's other constant keeps the word's one small immediate. */
static int
output_register(
	struct bcm2711_shader_compiler *compiler,
	struct bcm2711_qpu_source *source)
{
	struct bcm2711_qpu_source ignored;
	uint32_t number;
	int error;

	/* A bit-preserving move introduces no rounding before the pack's own conversion. */
	ignored = bcm2711_shader_bits(0);
	number = bcm2711_shader_scratch(compiler, BCM2711_SHADER_SCRATCH_WORK);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, number, 0, *source, ignored, 0, 0);
	if (error != 0)
		return error;
	*source = bcm2711_shader_rf(number);

	/* Succeeded: the pack reads this output from a register. */
	return 0;
}
