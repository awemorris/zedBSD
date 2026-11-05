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
 * it in place of that file.  It differs in three places: a vertex program
 * rounds its fixed-point XY to the nearest even instead of down and ends
 * without the VPM write wait that only 4.2 needs, and the two colour halves
 * of a fragment program never read two small immediates in one word.
 */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/compiler/shader-private.h"


static int vertex_output(struct bcm2711_shader_compiler *compiler);
static int fragment_output(struct bcm2711_shader_compiler *compiler);
static int vpm_store(struct bcm2711_shader_compiler *compiler, uint32_t index, struct bcm2711_qpu_source source);
static int viewport_component(struct bcm2711_shader_compiler *compiler, uint32_t component);
static int output_register(struct bcm2711_shader_compiler *compiler, struct bcm2711_qpu_source *source);

/*
 * Emits complete native stage outputs and the final program switch with both explicit delay slots.
 */
int
bcm2711_shader_finish(
	struct bcm2711_shader_compiler *compiler)
{
	int error;

	/* Fragment tile output and vertex VPM output use different completion protocols. */
	if (compiler->binary->stage == BCM2711_SHADER_FRAGMENT) {
		error = fragment_output(compiler);
	} else {
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

	/* A coordinate header ends after clip-position plus XY; it exports no render varyings. */
	compiler->binary->vpm_output_words = 6;
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

		/* Varyings follow the four render-header words in the exact fragment FIFO order. */
		for (index = 0; index < compiler->binary->varying_count; index++) {
			component = &compiler->binary->varyings[index];
			output = component->location * 4 + component->component;
			error = bcm2711_shader_operand(compiler, compiler->outputs[output], BCM2711_SHADER_SCRATCH_LEFT, &source);
			if (error != 0)
				return error;
			error = vpm_store(compiler, 4 + index, source);
			if (error != 0)
				return error;
		}

		/* The shader record must allocate exactly this complete render output header and varying extent. */
		compiler->binary->vpm_output_words = 4 + compiler->binary->varying_count;
	}

	/*
	 * Unlike 4.2, 7.1 needs no wait for the VPM stores before the program
	 * ends (Mesa 25.3.6 src/broadcom/compiler/nir_to_vir.c lines 2070-2085:
	 * GFXH-1684 is a 4.2 erratum).
	 */
	/* Every header and varying word has reached the native VPM output transaction. */
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

/* Acquires the tile scoreboard and exports all four target components; any blending happens in the tile buffer. */
static int
fragment_output(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source right;
	uint32_t index;
	uint32_t component;
	int error;

	/* The last switch before the tile writes is marked by a second switch in its first delay slot. */
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_LAST_FIRST);
	if (error != 0)
		return error;
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_LAST_SECOND);
	if (error != 0)
		return error;

	/*
	 * The tile buffer's default write format, which needs no configuration
	 * uniform, takes render target zero per pixel as four half floats in two
	 * words: red and green, then blue and alpha.  Physical target channel
	 * order is applied as the canonical colour is packed.  The packs read the
	 * source outputs where they live; a constant output is materialized, and
	 * a 7.1 word reads one small immediate, so of two constants one moves to
	 * a register even when they are equal.
	 */
	for (index = 0; index < 2; index++) {
		component = index * 2;
		if (compiler->key->swap_red_blue != 0)
			component = 2 - index * 2;
		error = bcm2711_shader_operand(compiler, compiler->outputs[component], BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		error = bcm2711_shader_operand(compiler, compiler->outputs[index * 2 + 1], BCM2711_SHADER_SCRATCH_RIGHT, &right);
		if (error != 0)
			return error;
		if (left.kind == BCM2711_QPU_CONSTANT &&
		    right.kind == BCM2711_QPU_CONSTANT) {
			error = output_register(compiler, &left);
			if (error != 0)
				return error;
		}

		/* A missing native color write prevents successful fragment termination. */
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_PACK_HALVES, 7, 1, left, right, 0, 0);
		if (error != 0)
			return error;
	}

	/* The native tile transaction contains both completed packed colour writes. */
	return 0;
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
