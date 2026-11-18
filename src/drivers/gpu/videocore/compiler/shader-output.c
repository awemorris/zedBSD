/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native stage epilogues explicitly own VPM layouts, tile scoreboard acquisition and program termination. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/compiler/shader-private.h"


static int vertex_output(struct bcm2711_shader_compiler *compiler);
static int fragment_output(struct bcm2711_shader_compiler *compiler);
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

	/* Both vertex variants export viewport XY as floor-converted .8 fixed-point values. */
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

	/* V3D 4.2 must wait for VPM stores before ending a graphics shader. */
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_VPM_WAIT, 6, 1, ignored, ignored, 0, 0);
	if (error != 0)
		return error;

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

	/* Floor before integer conversion avoids V3D 4.2's documented XY double-rounding quirk. */
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_FLOOR, BCM2711_SHADER_SCRATCH_WORK, 0, transformed, scale, 0, 0);
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

	/* This engine's programs do not write depth. */
	if (compiler->binary->writes_depth != 0)
		return bcm2711_shader_fail(compiler, ENOTSUP, "depth output on a 4.2 engine");

	/* This engine's passes have one render target of four halves, target 0. */
	if (compiler->key->target_count > 1U ||
	    compiler->key->target_unused != 0 ||
	    compiler->key->target_wide != 0)
		return ENOTSUP;

	/*
	 * The tile buffer's default write format, which needs no configuration
	 * uniform, takes render target zero per pixel as four half floats in two
	 * words: red and green, then blue and alpha.  Physical target channel
	 * order is applied as the canonical colour is packed.  The packs read the
	 * source outputs where they live (zero for a component the program
	 * leaves out); a constant output is materialized, and two different
	 * small constants cannot share the one constant port.
	 */
	for (index = 0; index < 2; index++) {
		component = index * 2;
		if ((compiler->key->swap_red_blue & 1U) != 0)
			component = 2 - index * 2;
		left = bcm2711_shader_bits(0);
		if (compiler->outputs[component] != BCM2711_SHADER_ABSENT) {
			error = bcm2711_shader_operand(compiler, compiler->outputs[component], BCM2711_SHADER_SCRATCH_LEFT, &left);
			if (error != 0)
				return error;
		}

		/* The pack's second channel, likewise. */
		right = bcm2711_shader_bits(0);
		if (compiler->outputs[index * 2 + 1] != BCM2711_SHADER_ABSENT) {
			error = bcm2711_shader_operand(compiler, compiler->outputs[index * 2 + 1], BCM2711_SHADER_SCRATCH_RIGHT, &right);
			if (error != 0)
				return error;
		}

		/* Two different constants cannot share the constant port. */
		if (left.kind == BCM2711_QPU_CONSTANT &&
		    right.kind == BCM2711_QPU_CONSTANT &&
		    left.number != right.number) {
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

/* Copies a small constant output into a scratch register so the pack's other constant keeps the constant port. */
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
