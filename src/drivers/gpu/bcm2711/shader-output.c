/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native stage epilogues explicitly own VPM layouts, tile scoreboard acquisition and program termination. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/shader-private.h"

/* Four float components of render target zero use one per-pixel tile configuration byte. */
#define SHADER_RGBA_TILE_CONFIGURATION 0xffffff3fU

static int vertex_output(struct bcm2711_shader_compiler *compiler);
static int fragment_output(struct bcm2711_shader_compiler *compiler);
static int vpm_store(struct bcm2711_shader_compiler *compiler, uint32_t index, struct bcm2711_qpu_source source);
static int viewport_component(struct bcm2711_shader_compiler *compiler, uint32_t component);

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

	/* The final thread switch ends the program only after its two uploaded idle delay slots execute. */
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_SWITCH, 0, 2);
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

	/* V3D 4.2 must wait for VPM stores before ending a graphics shader. */
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_VPM_WAIT, 6, 1, ignored, ignored, 0, 0);
	if (error != 0)
		return error;

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

/* Acquires the tile scoreboard, optionally blends premultiplied RGBA, and exports all four target components. */
static int
fragment_output(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_qpu_source source;
	struct bcm2711_qpu_source previous;
	struct bcm2711_qpu_source inverse_alpha;
	struct bcm2711_qpu_source ignored;
	struct bcm2711_shader_uniform uniform;
	uint32_t colors[4];
	uint32_t retained[4];
	uint32_t index;
	uint32_t component;
	int error;

	/* The final pre-output switch is marked in its first delay slot before any tile read or write. */
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_SWITCH, 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_SWITCH, 0, 1);
	if (error != 0)
		return error;
	ignored = bcm2711_shader_bits(0);

	/* Source outputs are copied into independent physical registers so constants and channel swapping cannot clobber them. */
	for (index = 0; index < 4; index++) {
		error = bcm2711_shader_register(compiler, &colors[index]);
		if (error != 0)
			return error;
		error = bcm2711_shader_operand(compiler, compiler->outputs[index], BCM2711_SHADER_SCRATCH_LEFT, &source);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, colors[index], 0, source, ignored, 0, 0);
		if (error != 0)
			return error;
	}

	/* Blending reads four float tile words only after the scoreboard-owning final switch. */
	if (compiler->key->premultiplied_blend != 0) {
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_CONSTANT;
		uniform.bits = SHADER_RGBA_TILE_CONFIGURATION;
		error = bcm2711_shader_uniform_append(compiler, &uniform);
		if (error != 0)
			return error;
		for (index = 0; index < 4; index++) {
			error = bcm2711_shader_register(compiler, &retained[index]);
			if (error != 0)
				return error;
			if (index == 0) {
				error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TILE_CONFIGURED, retained[index], 2);
			} else {
				error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TILE, retained[index], 2);
			}

			/* Every requested tile component is checked before blend arithmetic can read it. */
			if (error != 0)
				return error;
		}

		/* Premultiplied source-over uses the original source alpha for every color and alpha component. */
		source = bcm2711_shader_bits(0x3f800000U);
		previous = bcm2711_shader_rf(colors[3]);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_SUBTRACT, BCM2711_SHADER_SCRATCH_AUX, 0, source, previous, 0, 0);
		if (error != 0)
			return error;
		inverse_alpha = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_AUX);
		for (index = 0; index < 4; index++) {
			/* Tile channel order follows the target format; shader source values remain canonical RGBA. */
			component = index;
			if (compiler->key->swap_red_blue != 0) {
				if (index == 0)
					component = 2;
				if (index == 2)
					component = 0;
			}

			/* Previous target channels are interpreted as canonical RGBA before source-over arithmetic. */
			previous = bcm2711_shader_rf(retained[component]);
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, BCM2711_SHADER_SCRATCH_WORK, 0, previous, inverse_alpha, 0, 0);
			if (error != 0)
				return error;
			source = bcm2711_shader_rf(colors[index]);
			previous = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_ADD, colors[index], 0, source, previous, 0, 0);
			if (error != 0)
				return error;
		}
	}

	/* The first color write consumes an explicit float RGBA per-pixel configuration word. */
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_CONSTANT;
	uniform.bits = SHADER_RGBA_TILE_CONFIGURATION;
	error = bcm2711_shader_uniform_append(compiler, &uniform);
	if (error != 0)
		return error;

	/* Physical target channel order is applied once, after any canonical source-over arithmetic. */
	for (index = 0; index < 4; index++) {
		component = index;
		if (compiler->key->swap_red_blue != 0) {
			if (index == 0)
				component = 2;
			if (index == 2)
				component = 0;
		}

		/* The chosen physical target channel is written only after its complete canonical color is available. */
		source = bcm2711_shader_rf(colors[component]);
		if (index == 0) {
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, 8, 1, source, ignored, 0, 0);
		} else {
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, 7, 1, source, ignored, 0, 0);
		}

		/* A missing native color write prevents successful fragment termination. */
		if (error != 0)
			return error;
	}

	/* The native tile transaction contains exactly four completed color component writes. */
	return 0;
}
