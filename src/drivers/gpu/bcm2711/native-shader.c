/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent V3D 4.2 shader/fetch record serialization uses the audited packet XML, without MMIO or allocation. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-state.h"

/* Eight scalar rows across sixteen four-byte channels occupy one VPM sector. */
#define NATIVE_VPM_SECTOR 512U

static int validate_program(const struct bcm2711_native_program *program, uint32_t stage);
static int output_segments(const struct bcm2711_shader_binary *binary, uint32_t half_vpm, uint32_t *segments);
static void write_word(uint8_t *bytes, uint32_t word);
static void write_program(uint8_t *bytes, const struct bcm2711_native_program *program);

/*
 * Encodes a complete shared-segment shader record for a conservative two-batch vertex cache.
 *
 * The owner must retain and clean every referenced GPU allocation before launch.
 * No scoreboard wait occurs on an early texture switch; the compiler acquires
 * the tile scoreboard at its final fragment switch before tile reads/writes.
 */
int
bcm2711_native_shader_encode(
	const struct bcm2711_native_shader *shader,
	uint8_t *bytes,
	size_t capacity)
{
	uint32_t stage;
	uint32_t half_vpm;
	uint32_t coordinate_segments;
	uint32_t vertex_segments;
	int error;

	/* Refuses missing output storage before inspecting any borrowed program. */
	if (shader == NULL || bytes == NULL)
		return EINVAL;

	/* No failed validation modifies even a prefix of the caller's reservation. */
	if (capacity < BCM2711_NATIVE_SHADER_BYTES)
		return ENOSPC;

	/* Default float attribute values live in independently retained aligned GPU storage. */
	if (shader->defaults == 0 || (shader->defaults & 15U) != 0)
		return EINVAL;

	/* Sixteen four-component defaults occupy a complete 256-byte native interval. */
	if ((uint64_t)shader->defaults + 256U > ((uint64_t)1 << 32))
		return EINVAL;

	/* Actual IDENT1 reports VPM capacity in eight-KiB units, rather than a board-name assumption. */
	if (shader->vpm_bytes == 0 || (shader->vpm_bytes & 8191U) != 0)
		return EINVAL;

	/* Every uploaded stream matches its coordinate, vertex or fragment compiler metadata. */
	for (stage = 0; stage < 3; stage++) {
		error = validate_program(&shader->programs[stage], stage);
		if (error != 0)
			return error;
	}

	/* The two vertex stages share the same FIFO input packing and emitted varying count. */
	if (shader->programs[0].binary->input_count != shader->programs[1].binary->input_count)
		return EINVAL;

	/* Fragment inputs must consume exactly the vertex stage's varying payload. */
	if (shader->programs[1].binary->varying_count != shader->programs[2].binary->input_count)
		return EINVAL;

	/* Each stage reserves at most half of VPM, including the live output plus both cached batches. */
	half_vpm = shader->vpm_bytes / NATIVE_VPM_SECTOR / 2U;
	error = output_segments(shader->programs[0].binary, half_vpm, &coordinate_segments);
	if (error != 0)
		return error;
	error = output_segments(shader->programs[1].binary, half_vpm, &vertex_segments);
	if (error != 0)
		return error;

	/* Clipping stays enabled, early Z stays disabled, and smooth fragment interpolation receives real centre W in RF0. */
	kern_memset(bytes, 0, BCM2711_NATIVE_SHADER_BYTES);
	bytes[0] = 2U;
	bytes[1] = 0x12U;
	bytes[2] = 4U;
	bytes[3] = (uint8_t)shader->programs[2].binary->input_count;

	/* Shared input/output segments use one input segment and no additional output segment. */
	bytes[4] = (uint8_t)coordinate_segments;
	bytes[5] = 1U;
	bytes[6] = (uint8_t)vertex_segments;
	bytes[7] = 1U;

	/* Hardware orders fragment, vertex and coordinate programs after the default-value pointer. */
	write_word(bytes + 8, shader->defaults);
	write_program(bytes + 12, &shader->programs[2]);
	write_program(bytes + 20, &shader->programs[1]);
	write_program(bytes + 28, &shader->programs[0]);

	/* Succeeded: the complete record is ready for its owner's later GPU publication. */
	return 0;
}

/*
 * Encodes one complete per-vertex float fetch record after logical interval validation by its owner.
 */
int
bcm2711_native_attribute_encode(
	const struct bcm2711_native_attribute *attribute,
	uint8_t *bytes,
	size_t capacity)
{
	/* Refuses missing inputs before dereferencing the caller's immutable fetch description. */
	if (attribute == NULL || bytes == NULL)
		return EINVAL;

	/* Short output reservations remain untouched. */
	if (capacity < BCM2711_NATIVE_ATTRIBUTE_BYTES)
		return ENOSPC;

	/* Float fetches require naturally aligned, nonzero native storage. */
	if (attribute->address == 0 || (attribute->address & 3U) != 0)
		return EINVAL;

	/* Only the admitted scalar-to-four-component float formats are represented. */
	if (attribute->components == 0 || attribute->components > 4U)
		return EINVAL;

	/* Each stage can consume a leading prefix of its format, including an unused zero prefix. */
	if (attribute->coordinate_values > attribute->components)
		return EINVAL;

	/* Render-stage reads obey the same actual format width. */
	if (attribute->vertex_values > attribute->components)
		return EINVAL;

	/* At least one stage must actually fetch this record. */
	if (attribute->coordinate_values == 0 && attribute->vertex_values == 0)
		return EINVAL;

	/* A four-component vector is encoded as zero; type two selects float without integer conversion. */
	kern_memset(bytes, 0, BCM2711_NATIVE_ATTRIBUTE_BYTES);
	write_word(bytes, attribute->address);
	bytes[4] = (uint8_t)((attribute->components & 3U) | 8U);
	bytes[5] = (uint8_t)(attribute->coordinate_values | (attribute->vertex_values << 4));
	write_word(bytes + 8, attribute->stride);
	write_word(bytes + 12, attribute->maximum_index);

	/* Succeeded: no instance divisor or implicit integer interpretation was introduced. */
	return 0;
}

/* Checks the immutable scalar compiler contract and complete native stream address arithmetic. */
static int
validate_program(
	const struct bcm2711_native_program *program,
	uint32_t stage)
{
	const struct bcm2711_shader_binary *binary;
	uint64_t end;

	/* A missing compiler product cannot be substituted with a fabricated native program. */
	binary = program->binary;
	if (binary == NULL)
		return EINVAL;

	/* Each record slot uses the matching two-thread scalar stage. */
	if ((uint32_t)binary->stage != stage || binary->threads != 2U)
		return EINVAL;

	/* Exact compiler arrays, rather than counts alone, establish actual uploaded input. */
	if (binary->code == NULL || binary->code_count == 0)
		return EINVAL;

	/* A nonempty uniform stream must also have immutable compiler metadata. */
	if (binary->uniform_count != 0 && binary->uniforms == NULL)
		return EINVAL;

	/* Code low bits are reserved for threading flags in the shader record. */
	if (program->code == 0 || (program->code & 7U) != 0)
		return EINVAL;

	/* Whole uploaded instruction streams stay inside the 32-bit GPU virtual address space. */
	end = (uint64_t)program->code + (uint64_t)binary->code_count * 8U;
	if (end > ((uint64_t)1 << 32))
		return EINVAL;

	/* Even an empty uniform stream receives an owned aligned placeholder address. */
	if (program->uniforms == 0 || (program->uniforms & 3U) != 0)
		return EINVAL;

	/* Whole actual uniform streams cannot wrap their encoded GPU addresses. */
	end = (uint64_t)program->uniforms + (uint64_t)binary->uniform_count * 4U;
	if (end > ((uint64_t)1 << 32))
		return EINVAL;

	/* The finite scalar compiler interface fits both VPM and fragment varying fields. */
	if (binary->input_count > BCM2711_SHADER_INTERFACE_WORDS || binary->varying_count > BCM2711_SHADER_INTERFACE_WORDS)
		return ENOTSUP;

	/* Thread section metadata is an exact bit, not an arbitrary truthy integer. */
	if (binary->starts_final > 1U)
		return EINVAL;

	/* Fragment output must retain the explicit final-switch scoreboard protocol. */
	if (stage == BCM2711_SHADER_FRAGMENT && binary->starts_final != 0)
		return ENOTSUP;

	/* Succeeded: all consumed metadata and complete native address arithmetic are representable. */
	return 0;
}

/* Reserves shared input/output VPM sectors and both conservative cached output batches. */
static int
output_segments(
	const struct bcm2711_shader_binary *binary,
	uint32_t half_vpm,
	uint32_t *segments)
{
	uint32_t words;
	uint32_t count;

	/* Coordinate output contains clip XYZW and viewport XY; vertex output adds the exact varying words. */
	words = 6U;
	if (binary->stage == BCM2711_SHADER_VERTEX)
		words = 4U + binary->varying_count;

	/* Compiler output metadata must match the native graphics record's expected header. */
	if (binary->vpm_output_words != words)
		return EINVAL;

	/* Shared segments hold all fetched inputs before their storage is reused for outputs. */
	if (binary->input_count > words)
		words = binary->input_count;

	/* Four-bit sector sizes must leave three resident output batches in this stage's half of VPM. */
	count = (words + 7U) / 8U;
	if (count > 15U || count * 3U > half_vpm)
		return ENOTSUP;

	/* Publishes the checked shared-sector count to the record serializer. */
	*segments = count;

	/* Succeeded: a two-batch VCM configuration cannot overcommit this stage's VPM share. */
	return 0;
}

/* Stores an unaligned little-endian word without relying on host endianness. */
static void
write_word(
	uint8_t *bytes,
	uint32_t word)
{
	/* Records each byte in hardware address order. */
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);
}

/* Stores one aligned code pointer with NaN propagation and its exact final-section flag. */
static void
write_program(
	uint8_t *bytes,
	const struct bcm2711_native_program *program)
{
	uint32_t code;

	/* Two-thread code leaves the four-way bit clear and propagates NaNs through native arithmetic. */
	code = program->code | 4U;
	if (program->binary->starts_final != 0)
		code |= 2U;

	/* The adjacent word points to this exact program's independently prepared uniform stream. */
	write_word(bytes, code);
	write_word(bytes + 4, program->uniforms);
}
