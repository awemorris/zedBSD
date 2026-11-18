/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The serialization of the V3D 4.2 binning state, without MMIO.
 */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/native-bin.h"

static void write_word(uint8_t *bytes, uint32_t word);
static void write_half(uint8_t *bytes, uint32_t half);
static uint32_t half_float(uint32_t bits);
static void write_varyings(uint8_t *bytes, uint8_t opcode, uint32_t mask);
static int validate_draw(const struct bcm2711_native_bin *state);
static void write_draw(uint8_t *bytes, const struct bcm2711_native_bin *state);
static int validate_stencil(const struct bcm2711_native_stencil *stencil);
static void write_stencil(uint8_t *bytes, const struct bcm2711_native_stencil *stencil, uint32_t front, uint32_t back);

/*
 * Encodes complete fixed graphics state followed by one draw of points, lines or triangles, indexed or not, of one or more instances.
 *
 * Every inherited flag is reset explicitly.  A blending draw uses the tile
 * buffer's blend equation, so fragment code never reads the tile buffer.  The depth and stencil tests and the colour write mask follow the
 * draw; both faces' stencil configurations are always written, so no
 * draw inherits another's.  The blend constant colour is always written,
 * for the constant blend factors.  Early-Z, multisampling, transform feedback and
 * queries remain disabled.  The caller owns every referenced native interval and
 * cleans the enclosing BCL.
 *
 * Values referenced: Mesa 25.0.7 src/broadcom/cle/v3d_packet.xml (the V3D 4.2
 * binning packets' fields).
 */

int
bcm2711_native_bin_encode(
	const struct bcm2711_native_bin *state,
	uint8_t *bytes,
	size_t capacity)
{
	uint32_t config;
	uint32_t index;
	uint32_t units;
	int error;

	/* Missing storage or short capacity refuses before any caller-owned byte is touched. */
	if (state == NULL || bytes == NULL)
		return EINVAL;
	if (capacity < BCM2711_NATIVE_BIN_BYTES)
		return ENOSPC;

	/* Shader state includes at least one attribute fetch for the native 4.2 CS/VS workaround. */
	if (state->shader == 0 ||
	    (state->shader & 31U) != 0 ||
	    state->attributes == 0 ||
	    state->attributes > 16U ||
	    state->vertices == 0)
		return EINVAL;
	if ((uint64_t)state->shader + 36U + state->attributes * 16U > ((uint64_t)1 << 32))
		return EINVAL;

	/* The draw's counts, bases and index interval fit its packets. */
	error = validate_draw(state);
	if (error != 0)
		return error;

	/* Finite raster state can encode only the implemented Boolean facing choices. */
	if (state->forward > 1 ||
	    state->reverse > 1 ||
	    state->clockwise > 1 ||
	    state->depth_offset > 1 ||
	    state->blend > 15U)
		return EINVAL;

	/* Each target's blend equation fits the Blend Cfg's fields. */
	for (index = 0; index < 4U; index++) {
		if (state->blend_factors[index] > 0xffffffU)
			return EINVAL;
	}

	/* Depth, stencil and colour-mask state names only the hardware's fields and codes. */
	if (state->colour_skip > 0xffffU ||
	    state->oversample > 1U ||
	    state->sample_skip > 15U ||
	    state->depth_test > 1 ||
	    state->depth_compare > 7U ||
	    state->depth_write > 1 ||
	    state->stencil > 1)
		return EINVAL;

	/* A depth write without a depth test has no hardware meaning here. */
	if (state->depth_test == 0 && state->depth_write != 0)
		return EINVAL;

	/* Both faces' stencil configurations fit their packet fields. */
	error = validate_stencil(&state->front);
	if (error != 0)
		return error;
	error = validate_stencil(&state->back);
	if (error != 0)
		return error;

	/* Clipping stays inside the advertised drawable extent, including empty intersections. */
	for (index = 0; index < 4; index++) {
		if (state->window[index] > 4096U)
			return EINVAL;
	}

	/* Complete rectangle endpoints stay within the same bounded drawable. */
	if (state->window[2] > 4096U - state->window[0] || state->window[3] > 4096U - state->window[1])
		return EINVAL;

	/* Shader XY scales are finite positive dimension multiples; unsupported numerical metadata never reaches a packet. */
	if (state->viewport.x_scale == 0 ||
	    state->viewport.x_scale > 0x49000000U ||
	    state->viewport.y_scale == 0 ||
	    state->viewport.y_scale > 0x49000000U)
		return EINVAL;
	if ((state->clipper.depth_scale & 0x7fffffffU) == 0 || (state->clipper.depth_scale & 0x7fffffffU) > 0x3f800000U)
		return EINVAL;
	if ((state->clipper.depth_offset & 0x7fffffffU) > 0x3f800000U ||
	    (state->clipper.minimum & 0x7fffffffU) > 0x40000000U ||
	    (state->clipper.maximum & 0x7fffffffU) > 0x40000000U)
		return EINVAL;

	/* Validation is complete; every packet and reserved field starts from an initialized zero image. */
	kern_memset(bytes, 0, BCM2711_NATIVE_BIN_BYTES);

	/* Fixed point/line sizes and the covered samples (all but the skipped ones) remove inherited primitive/sample state. */
	bytes[0] = 104;
	write_word(bytes + 1, 0x3f800000U);
	if (state->point_size != 0)
		write_word(bytes + 1, state->point_size);
	bytes[5] = 105;
	write_word(bytes + 6, 0x3f800000U);
	if (state->line_width != 0)
		write_word(bytes + 6, state->line_width);
	bytes[10] = 91;
	bytes[11] = (uint8_t)(15U & ~state->sample_skip);
	write_half(bytes + 13, 0x3f80U);

	/* Native clip window is the complete intersection of drawable, render area, viewport and user scissor. */
	bytes[15] = 107;
	write_half(bytes + 16, state->window[0]);
	write_half(bytes + 18, state->window[1]);
	write_half(bytes + 20, state->window[2]);
	write_half(bytes + 22, state->window[3]);

	/* Clipper uses the same XY export scale and its separately guarded depth transform. */
	bytes[24] = 110;
	write_word(bytes + 25, state->viewport.x_scale);
	write_word(bytes + 29, state->viewport.y_scale);
	bytes[33] = 111;
	write_word(bytes + 34, state->clipper.depth_scale);
	write_word(bytes + 38, state->clipper.depth_offset);
	bytes[42] = 109;
	write_word(bytes + 43, state->clipper.minimum);
	write_word(bytes + 47, state->clipper.maximum);
	bytes[51] = 108;
	write_word(bytes + 52, state->clipper.x_offset);
	write_word(bytes + 56, state->clipper.y_offset);

	/* Vulkan uses the first provoking vertex; early-Z stays off, and blending, depth and stencil follow the draw. */
	config = (1U << 21) | (1U << 4);

	/* Without a depth test every fragment passes it (function always, 7) and no depth is written. */
	if (state->depth_test != 0) {
		config |= state->depth_compare << 12;
		config |= state->depth_write << 15;
	} else {
		config |= 7U << 12;
	}

	/* The stencil test, the facing choices and blending follow the draw. */
	config |= state->stencil << 18;
	config |= state->forward;
	config |= state->reverse << 1;
	config |= state->clockwise << 2;
	config |= state->depth_offset << 3;
	if (state->blend != 0)
		config |= 1U << 19;
	config |= state->oversample << 6;
	bytes[60] = 96;
	bytes[61] = (uint8_t)config;
	bytes[62] = (uint8_t)(config >> 8);
	bytes[63] = (uint8_t)(config >> 16);

	/* Each target is written in the channels the draw keeps (four mask bits a target, targets 4 to 7 disabled), and the blending ones blend. */
	bytes[64] = 87;
	write_word(bytes + 65, 0xffff0000U | state->colour_skip);
	bytes[69] = 83;
	bytes[70] = (uint8_t)state->blend;

	/*
	 * Disabled feedback/query state cannot accidentally reference a
	 * previous job's native addresses; a pass that counts occlusion
	 * writes its counter's address into the query packet afterwards.
	 */
	bytes[71] = 74;
	bytes[BCM2711_NATIVE_BIN_OCCLUSION - 1U] = 92;

	/* Two explicit twenty-four-bit groups define every supported scalar varying and clear all higher flags. */
	write_varyings(bytes + 78, 98, state->flat);
	write_varyings(bytes + 88, 100, state->noperspective);
	bytes[98] = 88;

	/* The shader's cached sixteen-vertex batches, which its checked VPM segments leave room for (two each without one). */
	bytes[99] = 71;
	bytes[100] = 0x22U;
	if (state->vcm != 0)
		bytes[100] = (uint8_t)state->vcm;
	bytes[101] = 64;
	write_word(bytes + 102, state->shader | state->attributes);

	/*
	 * Target 0's blend equation, the draw's factors and modes for colour
	 * and alpha; the other targets' follow the depth offset.  Each takes
	 * effect only while the blend enables above select its target.
	 */
	bytes[106] = 84;
	write_word(bytes + 107, (1U << 24) | state->blend_factors[0]);

	/*
	 * The forward-facing (front) and the reverse-facing (back) stencil
	 * configurations, each written alone so that the two faces may differ.
	 * They take effect only while the stencil enable above is set.
	 */
	write_stencil(bytes + 111, &state->front, 1, 0);
	write_stencil(bytes + 117, &state->back, 0, 1);

	/* The blend constant colour, as half floats in red, green, blue and alpha order. */
	bytes[123] = 86;
	for (index = 0; index < 4; index++)
		write_half(bytes + 124 + index * 2U, half_float(state->blend_constants[index]));

	/*
	 * The depth offset: the slope and constant factors as their IEEE
	 * single's top sixteen bits (f187), and the clamp.  It takes effect
	 * only while the configuration enables it.  4.2 counts the constant
	 * factor in 24-bit depth steps, so a D16 attachment's is 256 times
	 * larger (Mesa 26.2.6 src/broadcom/vulkan/v3dvx_cmd_buffer.c, the
	 * depth bias of V3D_VERSION <= 42).
	 */
	units = state->depth_offset_units;
	if (state->depth_offset_d16 != 0 && (units & 0x7f800000U) != 0 && (units & 0x7f800000U) < (247U << 23))
		units += 8U << 23;
	bytes[132] = 106;
	write_half(bytes + 133, state->depth_offset_factor >> 16);
	write_half(bytes + 135, units >> 16);
	write_word(bytes + 137, state->depth_offset_limit);

	/* Targets 1 to 3's blend equations, each a Blend Cfg of its own target. */
	for (index = 1; index < 4U; index++) {
		bytes[141U + (index - 1U) * 5U] = 84;
		write_word(bytes + 142U + (index - 1U) * 5U, (1U << (24U + index)) | state->blend_factors[index]);
	}

	/* The draw follows the state. */
	write_draw(bytes + BCM2711_NATIVE_BIN_STATE_BYTES, state);

	/* Succeeded: the complete atomic sequence can be copied into the owned enclosing bin list before native launch. */
	return 0;
}

/* Writes one numerical native word in little-endian byte order without requiring aligned CPU storage. */
static void
write_word(
	uint8_t *bytes,
	uint32_t word)
{
	/* Each byte preserves its exact hardware field regardless of the host fixture's word representation. */
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);

	/* Succeeded: one complete little-endian word occupies the caller's exact field. */
	return;
}

/* Writes one bounded unsigned sixteen-bit packet field in explicit target byte order. */
static void
write_half(
	uint8_t *bytes,
	uint32_t half)
{
	/* These geometry and sample fields have already been bounded before image construction. */
	bytes[0] = (uint8_t)half;
	bytes[1] = (uint8_t)(half >> 8);

	/* Succeeded: one complete little-endian halfword occupies its exact field. */
	return;
}

/*
 * Converts IEEE single bits to half-float bits, rounding to the nearest
 * (ties to even) as Mesa's _mesa_float_to_half does; too large a value
 * becomes infinity and a NaN stays a NaN.
 */
static uint32_t
half_float(
	uint32_t bits)
{
	uint32_t sign;
	uint32_t exponent;
	uint32_t mantissa;
	uint32_t half;
	uint32_t shift;
	uint32_t remainder;
	uint32_t halfway;

	/* The sign moves to bit 15; the exponent and the mantissa are converted below. */
	sign = (bits >> 16) & 0x8000U;
	exponent = (bits >> 23) & 0xffU;
	mantissa = bits & 0x7fffffU;

	/* Infinity and NaN keep their kind. */
	if (exponent == 0xffU) {
		if (mantissa != 0)
			return sign | 0x7e00U;
		return sign | 0x7c00U;
	}

	/* A value of 65536 or more is beyond the largest finite half. */
	if (exponent > 142U)
		return sign | 0x7c00U;

	/* A value below 2^-25 rounds to zero. */
	if (exponent < 102U)
		return sign;

	/*
	 * A normal half keeps ten mantissa bits and rebiases the exponent; a
	 * subnormal one shifts the mantissa, with its implicit bit, into the
	 * ten bits of 2^-24 units.  A carry out of the mantissa rounds up into
	 * the exponent, as it should.
	 */
	if (exponent >= 113U) {
		half = ((exponent - 112U) << 10) | (mantissa >> 13);
		shift = 13U;
	} else {
		mantissa |= 0x800000U;
		shift = 126U - exponent;
		half = mantissa >> shift;
	}

	/* The dropped bits round to the nearest, ties to even. */
	remainder = mantissa & ((1U << shift) - 1U);
	halfway = 1U << (shift - 1U);
	if (remainder > halfway ||
	    (remainder == halfway && (half & 1U) != 0))
		half++;

	/* Succeeded: the half's bits. */
	return sign | half;
}

/* Sets both native varying flag groups while clearing every inherited higher flag. */
static void
write_varyings(
	uint8_t *bytes,
	uint8_t opcode,
	uint32_t mask)
{
	/* First group defines bits zero through twenty-three and clears higher groups. */
	bytes[0] = opcode;
	bytes[1] = 0x40U;
	bytes[2] = (uint8_t)mask;
	bytes[3] = (uint8_t)(mask >> 8);
	bytes[4] = (uint8_t)(mask >> 16);

	/* Second group preserves the lower group, defines the remaining eight bits and again clears unused higher groups. */
	bytes[5] = opcode;
	bytes[6] = 0x41U;
	bytes[7] = (uint8_t)(mask >> 24);
	bytes[8] = 0;
	bytes[9] = 0;

	/* Succeeded: interpolation metadata has no dependence on a previous native draw's flags. */
	return;
}

/* Refuses a draw whose counts, bases or index interval its packets cannot hold. */
static int
validate_draw(
	const struct bcm2711_native_bin *state)
{
	/* Every draw has at least one instance, a known kind and a primitive the packets name. */
	if (state->instances == 0 || state->indexed > 1)
		return EINVAL;
	if (state->primitive > 6U || state->primitive == 2U || state->restart > 1)
		return EINVAL;
	if (state->restart != 0 && state->indexed == 0)
		return EINVAL;

	/* A vertex array has no index interval. */
	if (state->indexed == 0)
		return 0;

	/* The indexed list's length field has 31 bits. */
	if (state->vertices > 0x7fffffffU)
		return EINVAL;

	/* Indices are 16 or 32 bits, aligned, and the read ones lie inside the stated interval. */
	if (state->index_size != 2U && state->index_size != 4U)
		return EINVAL;
	if (state->index_address == 0 ||
	    (state->index_address & (state->index_size - 1U)) != 0 ||
	    (state->first & (state->index_size - 1U)) != 0)
		return EINVAL;
	if ((uint64_t)state->index_address + state->index_bytes > ((uint64_t)1 << 32))
		return EINVAL;
	if ((uint64_t)state->first + (uint64_t)state->vertices * state->index_size > state->index_bytes)
		return EINVAL;

	/* Succeeded: the draw's packets can hold it. */
	return 0;
}

/*
 * Writes one draw after the state, padded with NOPs to the
 * fixed draw length (a zero byte would be Halt).
 *
 * The base vertex and base instance are set by every draw, so none
 * inherits another's.  A single instance uses the plain list packets, as
 * Mesa does; the primitive mode is the draw's, an indexed draw restarts its
 * strip or fan at an index of all ones when it asks (the hardware's fixed
 * restart index), and an indexed list's offset is in bytes.
 *
 * Values referenced: Mesa 25.0.7 src/broadcom/cle/v3d_packet.xml (Base
 * Vertex Base Instance 43, Index Buffer Setup 44, Indexed Prim List 32,
 * Indexed Instanced Prim List 34, Vertex Array Prims 36, Vertex Array
 * Instanced Prims 38, NOP 1) and vulkan/v3dvx_cmd_buffer.c's draw emission.
 */
static void
write_draw(
	uint8_t *bytes,
	const struct bcm2711_native_bin *state)
{
	uint32_t type;
	uint32_t restart;

	/* Every byte the draw leaves is a NOP. */
	kern_memset(bytes, 1, BCM2711_NATIVE_BIN_BYTES - BCM2711_NATIVE_BIN_STATE_BYTES);

	/* The bases come first: the vertex offset added to indices and the first instance. */
	bytes[0] = 43;
	write_word(bytes + 1, state->base_vertex);
	write_word(bytes + 5, state->base_instance);

	/* A vertex array draws vertices first to first plus vertices, once or per instance. */
	if (state->indexed == 0) {
		if (state->instances == 1) {
			bytes[9] = 36;
			bytes[10] = (uint8_t)state->primitive;
			write_word(bytes + 11, state->vertices);
			write_word(bytes + 15, state->first);
		} else {
			bytes[9] = 38;
			bytes[10] = (uint8_t)state->primitive;
			write_word(bytes + 11, state->vertices);
			write_word(bytes + 15, state->instances);
			write_word(bytes + 19, state->first);
		}

		/* Succeeded: one vertex-array draw follows the bases. */
		return;
	}

	/* The index buffer, whose 16-bit indices are type 1 and 32-bit ones type 2. */
	bytes[9] = 44;
	write_word(bytes + 10, state->index_address);
	write_word(bytes + 14, state->index_bytes);
	type = 1U;
	if (state->index_size == 4U)
		type = 2U;

	/* One indexed list, once or per instance; a restart enable is bit 31 of the length word. */
	restart = state->restart << 31;
	if (state->instances == 1) {
		bytes[18] = 32;
		bytes[19] = (uint8_t)(state->primitive | (type << 6));
		write_word(bytes + 20, state->vertices | restart);
		write_word(bytes + 24, state->first);
	} else {
		bytes[18] = 34;
		bytes[19] = (uint8_t)(state->primitive | (type << 6));
		write_word(bytes + 20, state->vertices | restart);
		write_word(bytes + 24, state->instances);
		write_word(bytes + 28, state->first);
	}

	/* Succeeded: the index buffer and one indexed draw follow the bases. */
	return;
}

/* Refuses a face's stencil configuration that the packet's fields cannot hold. */
static int
validate_stencil(
	const struct bcm2711_native_stencil *stencil)
{
	/* Each operation and the compare function have three-bit codes. */
	if (stencil->fail > 7U ||
	    stencil->pass > 7U ||
	    stencil->depth_fail > 7U ||
	    stencil->compare > 7U)
		return EINVAL;

	/* The masks and the reference are eight-bit, as the stencil buffer is. */
	if (stencil->compare_mask > 0xffU ||
	    stencil->write_mask > 0xffU ||
	    stencil->reference > 0xffU)
		return EINVAL;

	/* Succeeded: every field fits its packet field. */
	return 0;
}

/*
 * Writes one Stencil Cfg packet for the faces named by front and back.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/cle/v3d_packet.xml (Stencil
 * Cfg, code 80, the same for 4.2 and 7.1) and vulkan/v3dvx_pipeline.c
 * pack_single_stencil_cfg.
 */
static void
write_stencil(
	uint8_t *bytes,
	const struct bcm2711_native_stencil *stencil,
	uint32_t front,
	uint32_t back)
{
	uint32_t word;

	/* The reference, the compare mask and function, the three operations and the faces fill the first word. */
	word = stencil->reference;
	word |= stencil->compare_mask << 8;
	word |= stencil->compare << 16;
	word |= stencil->fail << 19;
	word |= stencil->depth_fail << 22;
	word |= stencil->pass << 25;
	word |= front << 28;
	word |= back << 29;
	bytes[0] = 80;
	write_word(bytes + 1, word);

	/* The write mask is the fifth payload byte. */
	bytes[5] = (uint8_t)stencil->write_mask;

	/* Succeeded: the face's complete configuration occupies its six bytes. */
	return;
}
