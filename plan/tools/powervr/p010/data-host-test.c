/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of ws269-p010h: the descriptor set's memory (descriptor-set.c)
 * and the data sequencer programs that fill a shader's registers
 * (data-program.c).
 *
 * The expected words are built here from Mesa 26.2.5's values, not from
 * the driver's encoder: the instruction fields of
 * src/imagination/vulkan/pds/pvr_rogue_pds_defs.h (DOUT opcode 0xf, DDMAD
 * 0xe, SP 0xd with HALT 6; the DOUTD, DOUTW and DDMAD control fields), the
 * constant layouts of pvr_pipeline_pds.c
 * (pvr_pds_generate_descriptor_upload_program,
 * pvr_pds_generate_vertex_primary_program) and the descriptor structures
 * of src/imagination/vulkan/pvr_common.h.
 */

#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

#include <libc/vulkan/vulkan_core.h>

#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/descriptor-set.h"

/* The checks that passed and failed. */
static int passed;
static int failed;

static void check(int condition, const char *what);
static uint32_t mesa_dout(uint32_t cc, uint32_t end, uint32_t src1, uint32_t src0, uint32_t dst);
static uint32_t mesa_ddmad(uint32_t src0, uint32_t src1, uint32_t src2, uint32_t src3);
static uint32_t load32(const uint8_t *bytes);
static uint64_t load64(const uint8_t *bytes);
static void test_layout(void);
static void test_descriptors(void);
static void test_shared(void);
static void test_vertex(void);

int
main(void)
{
	/* Each part, then the count. */
	test_layout();
	test_descriptors();
	test_shared();
	test_vertex();
	printf("data-host-test: %d passed, %d failed\n", passed, failed);
	return failed == 0 ? 0 : 1;
}

/* Counts one check, naming a failure. */
static void
check(
	int condition,
	const char *what)
{
	/* A pass is counted; a failure is named. */
	if (condition) {
		passed++;
	} else {
		failed++;
		printf("FAIL: %s\n", what);
	}
}

/* Mesa's pvr_pds_inst_encode_dout. */
static uint32_t
mesa_dout(
	uint32_t cc,
	uint32_t end,
	uint32_t src1,
	uint32_t src0,
	uint32_t dst)
{
	/* OPCODEC_DOUT 0xf at 28, cc 27, end 26, src1 16, src0 8, dst 0. */
	return (0xfU << 28) | (cc << 27) | (end << 26) | (src1 << 16) | (src0 << 8) | dst;
}

/* Mesa's pvr_pds_inst_encode_ddmad with cc and end clear. */
static uint32_t
mesa_ddmad(
	uint32_t src0,
	uint32_t src1,
	uint32_t src2,
	uint32_t src3)
{
	/* OPCODEC_DDMAD 0xe at 28, src0 18, src1 13, src2 6, src3 0. */
	return (0xeU << 28) | (src0 << 18) | (src1 << 13) | (src2 << 6) | src3;
}

/* Reads a little-endian 32-bit word. */
static uint32_t
load32(
	const uint8_t *bytes)
{
	/* Lowest byte first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Reads a little-endian 64-bit word. */
static uint64_t
load64(
	const uint8_t *bytes)
{
	/* Low word first. */
	return (uint64_t)load32(bytes) | ((uint64_t)load32(bytes + 4) << 32);
}

/* The bindings' places in a set, and the refusals. */
static void
test_layout(void)
{
	struct powervr_set_binding_info infos[5];
	struct powervr_set_layout layout;
	uint32_t offset;
	int error;

	/* Out of order, one of count 0: sorted by number, no padding (pvr_CreateDescriptorSetLayout). */
	infos[0] = (struct powervr_set_binding_info){ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
	infos[1] = (struct powervr_set_binding_info){ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	infos[2] = (struct powervr_set_binding_info){ 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 };
	infos[3] = (struct powervr_set_binding_info){ 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1 };
	infos[4] = (struct powervr_set_binding_info){ 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0 };
	error = powervr_set_layout_build(infos, 5, &layout);
	check(error == 0, "layout: built");
	check(layout.bindings[0].offset == 0 && layout.bindings[0].stride == 16, "layout: buffer at 0, stride 16");
	check(layout.bindings[1].offset == 16 && layout.bindings[1].stride == 32, "layout: image at 16, stride 32");
	check(layout.bindings[2].offset == 48 && layout.bindings[2].stride == 80, "layout: combined at 48, stride 80");
	check(layout.bindings[3].offset == 208 && layout.bindings[3].stride == 48, "layout: sampler at 208, stride 48");
	check(layout.bindings[4].count == 0, "layout: count 0 takes no room");
	check(layout.bytes == 256, "layout: 256 bytes");

	/* The parts' places. */
	error = powervr_set_descriptor_offset(&layout, 2, 1, POWERVR_SHADER_PART_SAMPLER_STATE, &offset);
	check(error == 0 && offset == 48 + 80 + 32, "offset: combined element 1's sampler at 160");
	error = powervr_set_descriptor_offset(&layout, 2, 0, POWERVR_SHADER_PART_IMAGE_STATE, &offset);
	check(error == 0 && offset == 48, "offset: combined element 0's image at 48");
	error = powervr_set_descriptor_offset(&layout, 3, 0, POWERVR_SHADER_PART_SAMPLER_STATE, &offset);
	check(error == 0 && offset == 208, "offset: sampler at 208");
	error = powervr_set_descriptor_offset(&layout, 0, 0, POWERVR_SHADER_PART_BUFFER_ADDRESS, &offset);
	check(error == 0 && offset == 0, "offset: buffer at 0");
	error = powervr_set_descriptor_offset(&layout, 2, 2, POWERVR_SHADER_PART_IMAGE_STATE, &offset);
	check(error == EINVAL, "offset: element past the count refused");
	error = powervr_set_descriptor_offset(&layout, 1, 0, POWERVR_SHADER_PART_SAMPLER_STATE, &offset);
	check(error == EINVAL, "offset: a sampled image has no sampler");
	error = powervr_set_descriptor_offset(&layout, 4, 0, POWERVR_SHADER_PART_BUFFER_ADDRESS, &offset);
	check(error == EINVAL, "offset: an empty binding refused");

	/* Refusals: a binding named twice, a dynamic buffer, a number past the most. */
	infos[1].binding = 2;
	check(powervr_set_layout_build(infos, 2, &layout) == EINVAL, "layout: binding named twice refused");
	infos[1] = (struct powervr_set_binding_info){ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 };
	check(powervr_set_layout_build(&infos[1], 1, &layout) == ENOTSUP, "layout: dynamic buffer refused (p011)");
	infos[1] = (struct powervr_set_binding_info){ 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	check(powervr_set_layout_build(&infos[1], 1, &layout) == ENOTSUP, "layout: binding 8 refused");
	check(powervr_set_layout_build(NULL, 0, &layout) == 0 && layout.bytes == 0, "layout: empty");
}

/* The bytes of each descriptor and the pack information. */
static void
test_descriptors(void)
{
	static const uint64_t words[2] = { 0x0123456789abcdefULL, 0xfedcba9876543210ULL };
	uint8_t bytes[POWERVR_DESCRIPTOR_SAMPLER_BYTES];
	uint64_t gather;

	/* The pack information (setup_pck_info, pco_pipe_to_pck_format, is_unorm). */
	check(powervr_pack_info(VK_FORMAT_R8G8B8A8_UNORM) == 0x140, "pack: RGBA8 unorm u8888 | scale | unorm");
	check(powervr_pack_info(VK_FORMAT_R8_UNORM) == 0x140, "pack: R8 unorm");
	check(powervr_pack_info(VK_FORMAT_R8G8_UNORM) == 0x140, "pack: RG8 unorm");
	check(powervr_pack_info(VK_FORMAT_R8G8B8A8_SRGB) == 0x11f, "pack: RGBA8 srgb invalid | unorm");
	check(powervr_pack_info(VK_FORMAT_B8G8R8A8_UNORM) == 0x11f, "pack: BGRA8 unorm invalid | unorm");
	check(powervr_pack_info(VK_FORMAT_D16_UNORM) == 0x11f, "pack: D16 invalid | unorm");
	check(powervr_pack_info(VK_FORMAT_D32_SFLOAT) == 0x1f, "pack: D32 float invalid");
	check(powervr_pack_info(VK_FORMAT_R32G32B32A32_SFLOAT) == 0x1f, "pack: RGBA32 float invalid");

	/* A buffer: addr, size, zero offset (struct pvr_buffer_descriptor). */
	memset(bytes, 0xaa, sizeof(bytes));
	powervr_descriptor_buffer(bytes, 0x12345678abcULL, 0x400);
	check(load64(&bytes[0]) == 0x12345678abcULL && load32(&bytes[8]) == 0x400 && load32(&bytes[12]) == 0, "buffer: address, size, offset");
	check(bytes[16] == 0xaa, "buffer: 16 bytes only");

	/* An image: the words, then layer size, buffer elements, slice, pack information. */
	memset(bytes, 0xaa, sizeof(bytes));
	powervr_descriptor_image(bytes, words, 0x4000, 0x140);
	check(load64(&bytes[0]) == words[0] && load64(&bytes[8]) == words[1], "image: state words");
	check(load32(&bytes[16]) == 0x4000 && load32(&bytes[20]) == 0 && load32(&bytes[24]) == 0 && load32(&bytes[28]) == 0x140, "image: meta words");
	check(bytes[32] == 0xaa, "image: 32 bytes only");

	/* A sampler: the words, compare NEVER and three reserved, the gather words (linear, no mip filter). */
	powervr_descriptor_sampler(bytes, words, 0);
	gather = (words[0] & ~((uint64_t)0x1f << 36)) | ((uint64_t)1 << 36) | ((uint64_t)1 << 38);
	check(load64(&bytes[0]) == words[0] && load64(&bytes[8]) == words[1], "sampler: state words");
	check(load32(&bytes[16]) == 0 && load32(&bytes[20]) == 0 && load32(&bytes[24]) == 0 && load32(&bytes[28]) == 0, "sampler: meta words");
	check(load64(&bytes[32]) == gather && load64(&bytes[40]) == words[1], "sampler: gather words");
}

/* The shared register program from a request table. */
static void
test_shared(void)
{
	static struct powervr_shader_program program;
	static struct powervr_data_shared_program shared;
	struct powervr_set_binding_info infos[2];
	struct powervr_set_layout layout;
	struct powervr_data_state state;
	struct powervr_data_load loads[POWERVR_DATA_LOADS_MAX];
	uint64_t task;
	uint32_t count;
	uint32_t c32;
	int error;
	int index;

	/* Set 0: a storage buffer at 0, a combined image and sampler at 16. */
	infos[0] = (struct powervr_set_binding_info){ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	infos[1] = (struct powervr_set_binding_info){ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
	(void)powervr_set_layout_build(infos, 2, &layout);
	memset(&state, 0, sizeof(state));
	state.layouts[0] = &layout;
	state.set_addresses[0] = 0x100000ULL;
	state.push_address = 0x200000ULL;
	state.blend_address = 0x300000ULL;
	state.workgroup_id[0] = 5;
	state.workgroups[0] = 9;
	state.workgroups[1] = 1;
	state.workgroups[2] = 1;

	/* The requests: buffer address, image and sampler words, the set base at an odd register, a literal, push constants, workgroups, blend. */
	memset(&program, 0, sizeof(program));
	program.requests[0] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_BUFFER_ADDRESS, 2, 0, 0, 0, 0, 0, 0 };
	program.requests[1] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_STATE, 4, 2, 0, 1, 0, 0, 0 };
	program.requests[2] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_SAMPLER_STATE, 4, 6, 0, 1, 0, 0, 0 };
	program.requests[3] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_SET_BASE, 0, 2, 11, 0, 0, 0, 0, 0 };
	program.requests[4] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_LITERAL, 0, 1, 13, 0, 0, 0, 0, 0x3f800000U };
	program.requests[5] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_PUSH_CONSTANTS, 0, 3, 14, 0, 0, 0, 8, 0 };
	program.requests[6] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_NUM_WORKGROUPS, 0, 3, 18, 0, 0, 0, 0, 0 };
	program.requests[7] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_BLEND_CONSTANTS, 0, 4, 22, 0, 0, 0, 0, 0 };
	program.requests[8] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_WORKGROUP_ID, 0, 1, 26, 0, 0, 0, 0, 0 };
	program.requests[9] = (struct powervr_shader_request){ POWERVR_SHADER_REQUEST_COMPUTED, 0, 3, 27, 0, 0, 0, 0, 0 };
	program.request_count = 10;
	error = powervr_data_resolve(&program, &state, loads, POWERVR_DATA_LOADS_MAX, &count);
	check(error == 0 && count == 11, "resolve: 11 loads, none for the shared program's words");
	check(loads[0].kind == POWERVR_DATA_LOAD_DMA && loads[0].shared == 0 && loads[0].words == 2 && loads[0].source == 0x100000ULL, "resolve: buffer address from the set");
	check(loads[1].kind == POWERVR_DATA_LOAD_DMA && loads[1].shared == 2 && loads[1].words == 4 && loads[1].source == 0x100010ULL, "resolve: image words at the binding");
	check(loads[2].kind == POWERVR_DATA_LOAD_DMA && loads[2].shared == 6 && loads[2].source == 0x100030ULL, "resolve: sampler words 32 bytes on");
	check(loads[3].kind == POWERVR_DATA_LOAD_WRITE && loads[3].shared == 11 && loads[3].words == 1 && loads[3].source == 0x100000ULL, "resolve: set base low word alone (odd register)");
	check(loads[4].kind == POWERVR_DATA_LOAD_WRITE && loads[4].shared == 12 && loads[4].words == 1 && loads[4].source == 0, "resolve: set base high word");
	check(loads[5].kind == POWERVR_DATA_LOAD_WRITE && loads[5].shared == 13 && loads[5].source == 0x3f800000U, "resolve: literal");
	check(loads[6].kind == POWERVR_DATA_LOAD_DMA && loads[6].shared == 14 && loads[6].words == 3 && loads[6].source == 0x200008ULL, "resolve: push constants from offset 8");
	check(loads[7].kind == POWERVR_DATA_LOAD_WRITE && loads[7].shared == 18 && loads[7].words == 2 && loads[7].source == ((1ULL << 32) | 9U), "resolve: workgroup count x and y as a pair");
	check(loads[8].kind == POWERVR_DATA_LOAD_WRITE && loads[8].shared == 20 && loads[8].words == 1 && loads[8].source == 1, "resolve: workgroup count z");
	check(loads[9].kind == POWERVR_DATA_LOAD_DMA && loads[9].shared == 22 && loads[9].words == 4 && loads[9].source == 0x300000ULL, "resolve: blend constants");
	check(loads[10].kind == POWERVR_DATA_LOAD_WRITE && loads[10].shared == 26 && loads[10].source == 5, "resolve: workgroup id");

	/* The program: 64-bit constants first, 32-bit after (pvr_pds_generate_descriptor_upload_program). */
	error = powervr_data_shared_build(loads, count, &shared);
	check(error == 0 && shared.data_words == 33 && shared.code_words == 11, "shared: 33 data words, 11 code words");
	c32 = 22;
	check(shared.data[0] == 0x100000U && shared.data[1] == 0, "shared: DMA 0's address in const64 0");
	check(shared.data[c32] == ((2U << 0) | (0U << 13) | 0x10000000U), "shared: DOUTD control (2 words, shared 0, common store, cached)");
	check(shared.data[c32 + 1] == ((4U << 0) | (2U << 13) | 0x10000000U), "shared: DOUTD control (4 words to shared 2)");
	check(shared.data[6] == 0x100000U && shared.data[7] == 0, "shared: set base value in const64 3");
	check(shared.data[c32 + 3] == (0x10000000U | (11U << 13) | 0U), "shared: DOUTW control (lower to shared 11)");
	check(shared.data[c32 + 7] == (0x10000000U | (18U << 13) | 2U), "shared: DOUTW control (all 64 to shared 18)");
	check(shared.data[c32 + 10] == (0x80000000U | 0x10000000U | (26U << 13) | 0U), "shared: last DOUTW has LAST_EN");
	check(shared.code[0] == mesa_dout(0, 0, c32, 0, 0), "shared: DOUTD 0");
	check(shared.code[3] == mesa_dout(0, 0, c32 + 3, 3, 1), "shared: DOUTW 3");
	check(shared.code[10] == mesa_dout(0, 1, c32 + 10, 10, 1), "shared: last DOUTW ends the program");
	for (index = 0; index < 10; index++) {
		if ((shared.data[c32 + index] & 0x80000000U) != 0)
			break;
	}
	check(index == 10, "shared: only the last control has LAST_EN");

	/* No load: a halt (must_not_be_empty), no data. */
	error = powervr_data_shared_build(loads, 0, &shared);
	check(error == 0 && shared.code_words == 1 && shared.code[0] == ((0xdU << 28) | (6U << 23)) && shared.data_words == 0, "shared: empty is a halt");

	/*
	 * The loads and a shared task's issue (q1546, Mesa's
	 * secondary_program_present): the task word the 64-bit constant after
	 * the loads', the 32-bit constants after it, the last load marked but
	 * not ending, a DOUTU without cc ending the program.
	 */
	task = 0x0000004000000123ULL;
	error = powervr_data_shared_build_task(loads, count, &task, &shared);
	check(error == 0 && shared.data_words == 35 && shared.code_words == 12, "shared task: 35 data words, 12 code words");
	c32 = 24;
	check(shared.data[22] == 0x123U && shared.data[23] == 0x40U, "shared task: task word in const64 11");
	check(shared.data[c32] == ((2U << 0) | (0U << 13) | 0x10000000U), "shared task: DOUTD control after the task word");
	check(shared.data[c32 + 10] == (0x80000000U | 0x10000000U | (26U << 13) | 0U), "shared task: last DOUTW keeps LAST_EN");
	check(shared.code[0] == mesa_dout(0, 0, c32, 0, 0), "shared task: DOUTD 0");
	check(shared.code[10] == mesa_dout(0, 0, c32 + 10, 10, 1), "shared task: last DOUTW does not end the program");
	check(shared.code[11] == mesa_dout(0, 1, 0, 11, 2), "shared task: DOUTU ends the program");
	error = powervr_data_shared_build_task(loads, 0, &task, &shared);
	check(error == 0 && shared.data_words == 2 && shared.code_words == 1 && shared.code[0] == mesa_dout(0, 1, 0, 0, 2), "shared task: the issue alone");

	/* Refusals: an unbound set, a descriptor past the set, a two-word write to an odd register, no push buffer. */
	state.layouts[0] = NULL;
	check(powervr_data_resolve(&program, &state, loads, POWERVR_DATA_LOADS_MAX, &count) == EINVAL, "resolve: unbound set refused");
	state.layouts[0] = &layout;
	program.requests[2].words = 13;
	check(powervr_data_resolve(&program, &state, loads, POWERVR_DATA_LOADS_MAX, &count) == EINVAL, "resolve: words past the set refused");
	program.requests[2].words = 4;
	check(powervr_data_resolve(&program, &state, loads, 5, &count) == E2BIG, "resolve: table full");
	loads[0] = (struct powervr_data_load){ POWERVR_DATA_LOAD_WRITE, 3, 2, 0 };
	check(powervr_data_shared_build(loads, 1, &shared) == EINVAL, "shared: pair to an odd register refused");
	state.push_address = 0;
	check(powervr_data_resolve(&program, &state, loads, POWERVR_DATA_LOADS_MAX, &count) == EINVAL, "resolve: no push buffer refused");
}

/* The vertex program. */
static void
test_vertex(void)
{
	static struct powervr_data_vertex_program vertex;
	struct powervr_data_vertex_dma dmas[2];
	uint64_t task;
	uint32_t control;
	int error;

	/* Two attributes, the vertex index to register 6, the instance index to register 7. */
	dmas[0] = (struct powervr_data_vertex_dma){ 0x400000ULL, 20, 3, 0, false };
	dmas[1] = (struct powervr_data_vertex_dma){ 0x40000cULL, 20, 2, 3, false };
	task = 0x0000001234567890ULL;
	error = powervr_data_vertex_build(dmas, 2, 6, 7, task, &vertex);
	check(error == 0, "vertex: built");
	check(vertex.data[0] == 0x34567890U && vertex.data[1] == 0x12, "vertex: task word in constants 0 and 1");
	check(vertex.data[3] == 20 && vertex.data[4] == 0x400000U && vertex.data[5] == 0, "vertex: DMA 0 stride and address");
	control = (3U << 0) | (0U << 13);
	check(vertex.data[6] == control && vertex.data[7] == 0, "vertex: DMA 0 control (3 words to register 0, unified, no test)");
	check(vertex.data[11] == 20 && vertex.data[12] == 0x40000cU, "vertex: DMA 1 stride and address");
	check(vertex.data[14] == ((2U << 0) | (3U << 13)), "vertex: DMA 1 control (not last)");
	check(vertex.data[2] == (0U | (6U << 13)), "vertex: vertex index control in constant 2 (lower)");
	check(vertex.data[8] == (0x80000000U | 1U | (7U << 13)), "vertex: instance index control in constant 8, the first free of group 1 (upper, last)");
	check(vertex.code_words == 6, "vertex: 6 code words");
	check(vertex.code[0] == mesa_ddmad(3, 0, 2, 3), "vertex: DDMAD 0");
	check(vertex.code[1] == mesa_ddmad(11, 0, 6, 7), "vertex: DDMAD 1");
	check(vertex.code[2] == mesa_dout(0, 0, 2, 64, 1), "vertex: DOUTW vertex index from temp64 0");
	check(vertex.code[3] == mesa_dout(0, 0, 8, 64, 1), "vertex: DOUTW instance index");
	check(vertex.code[4] == mesa_dout(1, 1, 0, 0, 2), "vertex: DOUTU cc end");
	check(vertex.code[5] == ((0xdU << 28) | (6U << 23)), "vertex: halt");
	check(vertex.data_words == 16 && vertex.temps == 2, "vertex: 16 data words, 2 temps");

	/* The second attribute stepped per instance: its DDMAD reads the instance temporary (32-bit temp 1). */
	dmas[1].instance = true;
	error = powervr_data_vertex_build(dmas, 2, 6, 7, task, &vertex);
	check(error == 0 && vertex.code[0] == mesa_ddmad(3, 0, 2, 3) && vertex.code[1] == mesa_ddmad(11, 1, 6, 7), "vertex: instance rate DDMAD from temp 1");
	dmas[1].instance = false;

	/* One attribute, no index: its DMA is the last. */
	error = powervr_data_vertex_build(dmas, 1, POWERVR_SHADER_NO_REG, POWERVR_SHADER_NO_REG, task, &vertex);
	check(error == 0 && vertex.data[6] == (0x80000000U | 3U) && vertex.code_words == 3 && vertex.data_words == 8, "vertex: lone DMA is last, 8 data words");

	/* No attribute, the vertex index only: its control is constant 2, the data 3 words. */
	error = powervr_data_vertex_build(NULL, 0, 4, POWERVR_SHADER_NO_REG, task, &vertex);
	check(error == 0 && vertex.data[2] == (0x80000000U | (4U << 13)) && vertex.code_words == 3 && vertex.data_words == 3, "vertex: index only");
	check(vertex.code[0] == mesa_dout(0, 0, 2, 64, 1), "vertex: index only DOUTW");
}
