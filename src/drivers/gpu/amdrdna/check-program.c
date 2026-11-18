/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compute program the amdrdna driver runs once to check that a
 * dispatch works, and the words the check reads back.
 *
 * The program is the amdrdna shader compiler's output for this GLSL:
 *
 *	layout(local_size_x = 4, local_size_y = 3, local_size_z = 2) in;
 *	layout(std430, set = 0, binding = 0) buffer Out { uint words[]; };
 *	void main()
 *	{
 *		uvec3 total = gl_NumWorkGroups * gl_WorkGroupSize;
 *		uvec3 id = gl_GlobalInvocationID;
 *		uint at = (id.x + total.x * (id.y + total.y * id.z)) * 8u;
 *		words[at] = gl_LocalInvocationID.x;
 *		words[at + 1u] = gl_LocalInvocationID.y;
 *		words[at + 2u] = gl_LocalInvocationID.z;
 *		words[at + 3u] = gl_LocalInvocationIndex;
 *		words[at + 4u] = gl_WorkGroupID.x;
 *		words[at + 5u] = gl_WorkGroupID.y;
 *		words[at + 6u] = gl_WorkGroupID.z;
 *		words[at + 7u] = gl_NumWorkGroups.x + 10u * gl_NumWorkGroups.y +
 *		    100u * gl_NumWorkGroups.z;
 *	}
 *
 * Every invocation writes eight words that say where it ran, so a wrong
 * workgroup size, a missing workgroup ID or a lost lane shows as a wrong
 * word.  The program reads its two buffer descriptors from the table s[0:1]
 * points at: the output buffer's at byte 0, the buffer of the three
 * workgroup counts at byte 16.  Nothing here touches the hardware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/* The program's code words, without the padding. */
#define CHECK_CODE_WORDS		136U

/* The workgroup size the program declares. */
#define CHECK_LOCAL_X			4U
#define CHECK_LOCAL_Y			3U
#define CHECK_LOCAL_Z			2U

/* The words each invocation writes. */
#define CHECK_WORDS_PER_INVOCATION	8U

/*
 * The program's register values (the counts and the inputs words, which
 * the compiler computes), the table's layout and size, and the padding
 * the instruction prefetcher may read past the end (the code rounded up
 * to a 64-byte line, then three more lines: 16 and 48 words).
 *
 * Values referenced: the amdrdna compiler's output for the program
 * (amdrdna_gfx10_program_registers: 0x002f0001 and 0x00001384; the
 * storage bindings at 0 and 16 of a 32-byte table) and the AMD RDNA 1.0
 * Instruction Set Architecture reference guide (S_CODE_END, the
 * instruction the padding is made of: 0xbf9f0000).
 */
#define CHECK_COUNTS			0x002f0001U
#define CHECK_INPUTS			0x00001384U
#define CHECK_OUTPUT_AT			0U
#define CHECK_GROUPS_AT			16U
#define CHECK_TABLE_BYTES		32U
#define CHECK_LINE_WORDS		16U
#define CHECK_PADDING_WORDS		48U
#define CHECK_END_WORD			0xbf9f0000U

/*
 * A raw buffer descriptor's fourth word: the channels in order (4, 5, 6,
 * 7 in three bits each from bit 0), one 32-bit float channel (format 22
 * at bit 12), the resource level (bit 24) and the bounds checked by byte
 * offset (3 at bit 28); and the base's high bits in the second word.
 *
 * Values referenced: Mesa 25.3.6 src/amd/registers/gfx10-rsrc.json (the
 * buffer resource's DST_SEL_X to DST_SEL_W, FORMAT, RESOURCE_LEVEL and
 * OOB_SELECT fields; the format 32_FLOAT 22) and
 * src/amd/common/ac_descriptors.c (ac_build_raw_buffer_descriptor).
 * derived: 4 | 5 << 3 | 6 << 6 | 7 << 9 | 22 << 12 | 1 << 24 | 3 << 28 =
 * 0x31016fac.
 */
#define CHECK_DESCRIPTOR_RAW		0x31016facU
#define CHECK_DESCRIPTOR_HIGH_MASK	0x0000ffffU

/*
 * The dispatch's launch word: the compute shader on (bit 0), starting at
 * workgroup 0 (bit 2), waves allowed out of order (bit 6) and wave32
 * (bit 15).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (COMPUTE_DISPATCH_INITIATOR__COMPUTE_SHADER_EN__SHIFT,
 * COMPUTE_DISPATCH_INITIATOR__FORCE_START_AT_000__SHIFT,
 * COMPUTE_DISPATCH_INITIATOR__ORDER_MODE__SHIFT,
 * COMPUTE_DISPATCH_INITIATOR__CS_W32_EN__SHIFT) and Mesa 25.3.6
 * src/amd/vulkan/radv_device.c and radv_cmd_buffer.c (the dispatch
 * initiator of a wave32 dispatch).
 * derived: 1 | 1 << 2 | 1 << 6 | 1 << 15 = 0x8045.
 */
#define CHECK_INITIATOR			0x00008045U

/*
 * The program's code, as the compiler emitted it.
 *
 * Values referenced: the amdrdna compiler's output for the program above.
 */
static const uint32_t check_code[CHECK_CODE_WORDS] = {
	0xf4080200U, 0xfa000000U, 0xf4080300U, 0xfa000010U,
	0x7e080280U, 0xbf8cc07fU, 0xe030d000U, 0x80030304U,
	0x7e0a0284U, 0xe030d000U, 0x80030405U, 0x7e0c0288U,
	0xe030d000U, 0x80030506U, 0xbf8c3f70U, 0xd5690006U,
	0x00010903U, 0xd5690003U, 0x00010704U, 0xd5690004U,
	0x00010505U, 0x7e080202U, 0xd5690005U, 0x00010904U,
	0x7e080300U, 0x4a0e0905U, 0x7e080203U, 0xd5690005U,
	0x00010704U, 0x7e080301U, 0x4a100905U, 0x7e080204U,
	0xd5690005U, 0x00010504U, 0x7e080302U, 0x4a120905U,
	0xd5690004U, 0x00021303U, 0x4a060908U, 0xd5690004U,
	0x00020706U, 0x4a060907U, 0xd5690004U, 0x00011103U,
	0x7e060300U, 0xd5690005U, 0x00010904U, 0xd5250006U,
	0x00010105U, 0xe0701000U, 0x80020306U, 0xd5250003U,
	0x00010304U, 0x7e0a0301U, 0xd5690006U, 0x00010903U,
	0xd5250003U, 0x00010106U, 0xe0701000U, 0x80020503U,
	0xd5250003U, 0x00010504U, 0x7e0a0302U, 0xd5690006U,
	0x00010903U, 0xd5250003U, 0x00010106U, 0xe0701000U,
	0x80020503U, 0xd5250003U, 0x00010704U, 0xd5430005U,
	0x04050702U, 0xd5430005U, 0x04010905U, 0xd5690006U,
	0x00010903U, 0xd5250003U, 0x00010106U, 0xe0701000U,
	0x80020503U, 0xd5250003U, 0x00010904U, 0x7e0a0202U,
	0xd5690006U, 0x00010903U, 0xd5250003U, 0x00010106U,
	0xe0701000U, 0x80020503U, 0xd5250003U, 0x00010b04U,
	0x7e0a0203U, 0xd5690006U, 0x00010903U, 0xd5250003U,
	0x00010106U, 0xe0701000U, 0x80020503U, 0xd5250003U,
	0x00010d04U, 0x7e0a0204U, 0xd5690006U, 0x00010903U,
	0xd5250003U, 0x00010106U, 0xe0701000U, 0x80020503U,
	0xd5250003U, 0x00010f04U, 0x7e0a0280U, 0xe030d000U,
	0x80030405U, 0x7e0c0284U, 0xe030d000U, 0x80030506U,
	0xbf8c3f70U, 0xd5690006U, 0x00020a8aU, 0x4a0a0d04U,
	0x7e0c0288U, 0xe030d000U, 0x80030406U, 0x7e0c02ffU,
	0x00000064U, 0xbf8c3f70U, 0xd5690007U, 0x00020906U,
	0x4a080f05U, 0xd5690005U, 0x00010903U, 0xd5250003U,
	0x00010105U, 0xe0701000U, 0x80020403U, 0xbf810000U
};

/* Describes the check program: its code, register values, workgroup size and table. */
void
amdrdna_check_program(
	struct amdrdna_check_description *description)
{
	/* The code and the padding after it. */
	description->code = check_code;
	description->code_words = CHECK_CODE_WORDS;
	description->padded_words = (CHECK_CODE_WORDS + CHECK_LINE_WORDS - 1U) & ~(CHECK_LINE_WORDS - 1U);
	description->padded_words += CHECK_PADDING_WORDS;
	description->end_word = CHECK_END_WORD;

	/* The register values. */
	description->counts = CHECK_COUNTS;
	description->inputs = CHECK_INPUTS;

	/* The workgroup size. */
	description->local_size[0] = CHECK_LOCAL_X;
	description->local_size[1] = CHECK_LOCAL_Y;
	description->local_size[2] = CHECK_LOCAL_Z;

	/* The table. */
	description->output_at = CHECK_OUTPUT_AT;
	description->groups_at = CHECK_GROUPS_AT;
	description->table_bytes = CHECK_TABLE_BYTES;

	/* The launch word. */
	description->initiator = CHECK_INITIATOR;
}

/*
 * Writes the four words of a raw buffer descriptor: bytes bytes from
 * address, read and written as 32-bit words at byte offsets.
 */
void
amdrdna_raw_buffer_descriptor(
	uint64_t address,
	uint32_t bytes,
	uint32_t *words)
{
	/* The base, no stride, the size. */
	words[0] = (uint32_t)address;
	words[1] = (uint32_t)(address >> 32) & CHECK_DESCRIPTOR_HIGH_MASK;
	words[2] = bytes;

	/* The channels, the format, the level and the bounds check. */
	words[3] = CHECK_DESCRIPTOR_RAW;
}

/* Reports the output words a dispatch of groups workgroups writes. */
uint32_t
amdrdna_check_output_words(
	const uint32_t *groups)
{
	uint32_t invocations;

	/* Every invocation of every workgroup. */
	invocations = groups[0] * CHECK_LOCAL_X;
	invocations *= groups[1] * CHECK_LOCAL_Y;
	invocations *= groups[2] * CHECK_LOCAL_Z;

	/* Succeeded: eight words each. */
	return invocations * CHECK_WORDS_PER_INVOCATION;
}

/*
 * Compares the words a dispatch of groups workgroups wrote with what the
 * program writes.  Reports true when every word matches; otherwise false
 * with the first word that does not, the value it should hold and the one
 * it holds.
 */
bool
amdrdna_check_output(
	const uint32_t *words,
	const uint32_t *groups,
	uint32_t *bad_word,
	uint32_t *expected,
	uint32_t *found)
{
	uint32_t want[CHECK_WORDS_PER_INVOCATION];
	uint32_t total[3];
	uint32_t global[3];
	uint32_t at;
	unsigned part;

	/* The dispatch's size in invocations. */
	total[0] = groups[0] * CHECK_LOCAL_X;
	total[1] = groups[1] * CHECK_LOCAL_Y;
	total[2] = groups[2] * CHECK_LOCAL_Z;

	/* Every invocation's eight words. */
	for (global[2] = 0U; global[2] < total[2]; global[2]++) {
		for (global[1] = 0U; global[1] < total[1]; global[1]++) {
			for (global[0] = 0U; global[0] < total[0]; global[0]++) {
				/* Where the invocation writes. */
				at = global[0] + total[0] * (global[1] + total[1] * global[2]);
				at *= CHECK_WORDS_PER_INVOCATION;

				/* Its place in its workgroup, its workgroup, and the counts. */
				want[0] = global[0] % CHECK_LOCAL_X;
				want[1] = global[1] % CHECK_LOCAL_Y;
				want[2] = global[2] % CHECK_LOCAL_Z;
				want[3] = want[0] + CHECK_LOCAL_X * (want[1] + CHECK_LOCAL_Y * want[2]);
				want[4] = global[0] / CHECK_LOCAL_X;
				want[5] = global[1] / CHECK_LOCAL_Y;
				want[6] = global[2] / CHECK_LOCAL_Z;
				want[7] = groups[0] + 10U * groups[1] + 100U * groups[2];

				/* Each word must be what the program writes. */
				for (part = 0U; part < CHECK_WORDS_PER_INVOCATION; part++) {
					if (words[at + part] != want[part]) {
						*bad_word = at + part;
						*expected = want[part];
						*found = words[at + part];
						return false;
					}
				}
			}
		}
	}

	/* Succeeded: every word matches. */
	return true;
}
