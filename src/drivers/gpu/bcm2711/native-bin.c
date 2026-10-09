/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent 4.2 binning state serialization follows audited XML field facts, without upstream packing code or MMIO. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-bin.h"

static void write_word(uint8_t *bytes, uint32_t word);
static void write_half(uint8_t *bytes, uint32_t half);
static void write_varyings(uint8_t *bytes, uint8_t opcode, uint32_t mask);

/*
 * Encodes complete fixed graphics state followed by one non-indexed triangle-list draw rebased to vertex zero.
 *
 * Every inherited flag is reset explicitly.  Blending occurs in compiled
 * fragment code, so hardware blending remains disabled.  Depth/stencil,
 * multisampling, transform feedback and queries remain disabled.  The
 * caller owns every referenced native interval and cleans the enclosing BCL.
 */
int
bcm2711_native_bin_encode(
	const struct bcm2711_native_bin *state,
	uint8_t *bytes,
	size_t capacity)
{
	uint32_t config;
	uint32_t index;

	/* Missing storage or short capacity refuses before any caller-owned byte is touched. */
	if (state == NULL || bytes == NULL)
		return EINVAL;
	if (capacity < BCM2711_NATIVE_BIN_BYTES)
		return ENOSPC;

	/* Shader state includes at least one attribute fetch for the native 4.2 CS/VS workaround. */
	if (state->shader == 0 || (state->shader & 31U) != 0 || state->attributes == 0 || state->attributes > 16U || state->vertices == 0)
		return EINVAL;
	if ((uint64_t)state->shader + 36U + state->attributes * 16U > ((uint64_t)1 << 32))
		return EINVAL;

	/* Finite raster state can encode only the implemented Boolean facing choices. */
	if (state->forward > 1 || state->reverse > 1 || state->clockwise > 1)
		return EINVAL;

	/* Clipping stays inside the advertised drawable extent, including empty intersections. */
	for (index = 0; index < 4; index++) {
		if (state->window[index] > 4096U)
			return EINVAL;
	}

	/* Complete rectangle endpoints stay within the same bounded drawable. */
	if (state->window[2] > 4096U - state->window[0] || state->window[3] > 4096U - state->window[1])
		return EINVAL;

	/* Shader XY scales are finite positive dimension multiples; unsupported numerical metadata never reaches a packet. */
	if (state->viewport.x_scale == 0 || state->viewport.x_scale > 0x49000000U || state->viewport.y_scale == 0 || state->viewport.y_scale > 0x49000000U)
		return EINVAL;
	if ((state->clipper.depth_scale & 0x7fffffffU) == 0 || (state->clipper.depth_scale & 0x7fffffffU) > 0x3f800000U)
		return EINVAL;
	if ((state->clipper.depth_offset & 0x7fffffffU) > 0x3f800000U || (state->clipper.minimum & 0x7fffffffU) > 0x40000000U || (state->clipper.maximum & 0x7fffffffU) > 0x40000000U)
		return EINVAL;

	/* Validation is complete; every packet and reserved field starts from an initialized zero image. */
	kern_memset(bytes, 0, BCM2711_NATIVE_BIN_BYTES);

	/* Fixed point/line sizes and one fully covered sample remove inherited primitive/sample state. */
	bytes[0] = 104;
	write_word(bytes + 1, 0x3f800000U);
	bytes[5] = 105;
	write_word(bytes + 6, 0x3f800000U);
	bytes[10] = 91;
	bytes[11] = 15;
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

	/* Vulkan uses the first provoking vertex; no depth updates/early-Z/stencil/hardware blend can suppress software tile blending. */
	config = (1U << 21) | (7U << 12) | (1U << 4);
	config |= state->forward;
	config |= state->reverse << 1;
	config |= state->clockwise << 2;
	bytes[60] = 96;
	bytes[61] = (uint8_t)config;
	bytes[62] = (uint8_t)(config >> 8);
	bytes[63] = (uint8_t)(config >> 16);

	/* Only target zero writes all channels; unused targets and fixed-function blending remain disabled. */
	bytes[64] = 87;
	write_word(bytes + 65, 0xfff0U);
	bytes[69] = 83;

	/* Disabled feedback/query state cannot accidentally reference a previous job's native addresses. */
	bytes[71] = 74;
	bytes[73] = 92;

	/* Two explicit twenty-four-bit groups define every supported scalar varying and clear all higher flags. */
	write_varyings(bytes + 78, 98, state->flat);
	write_varyings(bytes + 88, 100, state->noperspective);
	bytes[98] = 88;

	/* Two sixteen-vertex batches match the independently checked live-plus-two-cached shared VPM reservation. */
	bytes[99] = 71;
	bytes[100] = 0x22U;
	bytes[101] = 64;
	write_word(bytes + 102, state->shader | state->attributes);

	/* Packed fetch already applied source firstVertex; this hardware array starts at zero and consumes exactly copied vertices. */
	bytes[106] = 36;
	bytes[107] = 4;
	write_word(bytes + 108, state->vertices);
	write_word(bytes + 112, 0);

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
