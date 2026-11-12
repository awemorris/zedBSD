/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the GHCI 1.0 driver's words
 * (src/drivers/gpu/ghci/ghci-cmd.h, ghci-regs.h): every command encoder
 * against the dwords the specification lays out, the records in memory
 * byte by byte, the bounds each encoder refuses, the 64-bit fence
 * comparison across the wrap, the ring's room and padding, the walk of
 * the extended capability list, the capability decode and the sizes the
 * driver derives.  The expected values are written out by hand from
 * docs/ghci.md, not computed from the encoders' own tables.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/ghci/ghci-cmd.h"
#include "drivers/gpu/ghci/ghci-regs.h"
#include "drivers/gpu/ghci/ghci-words.h"
#include "uapi/errno.h"

/* The words of the largest packet and of a register window copy. */
#define PACKET_WORDS			256U

/*
 * The counts of the checks that held and that failed.  Every check adds
 * to one of them; main reports both and fails when any check failed.
 */
static unsigned checks_passed;
static unsigned checks_failed;

/* The packet the encoders write into, filled with a marker before each test. */
static uint32_t packet[PACKET_WORDS];

/* The register window copy the capability tests build. */
static uint32_t window[GHCI_WINDOW_WORDS];

static void expect(int held, const char *name);
static int words_equal(const uint32_t *expected, uint32_t count);
static uint32_t byte_word(const uint8_t *bytes, uint32_t offset);
static void window_reset(void);
static void test_session_commands(void);
static void test_bind_commands(void);
static void test_draw_commands(void);
static void test_sync_commands(void);
static void test_transfer_commands(void);
static void test_display_commands(void);
static void test_records(void);
static void test_fences(void);
static void test_ring(void);
static void test_extcap(void);
static void test_caps(void);
static void test_sizes(void);

/*
 * Runs every test and reports the counts.
 */
int
main(void)
{
	/* Runs the tests in the order of the specification's sections. */
	test_session_commands();
	test_bind_commands();
	test_draw_commands();
	test_sync_commands();
	test_transfer_commands();
	test_display_commands();
	test_records();
	test_fences();
	test_ring();
	test_extcap();
	test_caps();
	test_sizes();

	/* Reports the counts; any failed check fails the program. */
	printf("ghci-words-host-test: %u passed, %u failed\n", checks_passed, checks_failed);
	if (checks_failed != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts one check and names it when it failed. */
static void
expect(
	int held,
	const char *name)
{
	/* Counts a held check. */
	if (held) {
		checks_passed++;
		return;
	}

	/* Names the failed check. */
	checks_failed++;
	printf("FAIL %s\n", name);
}

/* Reports whether the packet starts with the expected words. */
static int
words_equal(
	const uint32_t *expected,
	uint32_t count)
{
	int differs;

	/* Compares the words the encoder wrote. */
	differs = memcmp(packet, expected, count * sizeof(uint32_t));
	if (differs != 0)
		return 0;

	/* Succeeded: the words are the expected ones. */
	return 1;
}

/* Reads a little-endian word of a record. */
static uint32_t
byte_word(
	const uint8_t *bytes,
	uint32_t offset)
{
	/* Assembles the four bytes, the low byte first. */
	return (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1U] << 8) |
	    ((uint32_t)bytes[offset + 2U] << 16) | ((uint32_t)bytes[offset + 3U] << 24);
}

/* Builds a minimal GHCI 1.0 register window without extended capabilities. */
static void
window_reset(void)
{
	/* Clears the window and writes the identification. */
	memset(window, 0, sizeof(window));
	window[GHCI_REG_MAGIC / 4U] = 0x47484349U;
	window[GHCI_REG_VERSION / 4U] = 0x00010000U;
}

/* Tests VM_MAP, VM_UNMAP and the session commands. */
static void
test_session_commands(void)
{
	static const uint32_t vm_map[] = {
		0x00000A10U, 3U, 0x0000000BU, 0U, 0x00200000U, 0U, 0x40001000U, 0x00000001U, 0x00004000U, 0U
	};
	static const uint32_t vm_unmap[] = { 0x00000811U, 3U, 0U, 0U, 0x00200000U, 0U, 0x00004000U, 0U };
	static const uint32_t open[] = { 0x00000412U, 15U, 0U, 0U };
	static const uint32_t close[] = { 0x00000213U, 15U };
	static const uint32_t bind[] = { 0x00000214U, 0U };
	uint32_t dwords;

	/* VM_MAP: 10 dwords, session, flags (R W COHERENT), reserved, VA, phys, size. */
	dwords = ghci_cmd_vm_map(packet, 3U, 0x0BU, 0x200000U, 0x140001000ULL, 0x4000U);
	expect(dwords == 10U && words_equal(vm_map, 10U), "vm_map words");

	/* VM_MAP refuses a session past 15, an empty range and an address off a page. */
	expect(ghci_cmd_vm_map(packet, 16U, 1U, 0U, 0U, 0x1000U) == 0U, "vm_map session 16");
	expect(ghci_cmd_vm_map(packet, 1U, 1U, 0U, 0U, 0U) == 0U, "vm_map empty");
	expect(ghci_cmd_vm_map(packet, 1U, 1U, 0x800U, 0U, 0x1000U) == 0U, "vm_map unaligned va");
	expect(ghci_cmd_vm_map(packet, 1U, 1U, 0U, 0x10U, 0x1000U) == 0U, "vm_map unaligned bus");

	/* VM_UNMAP: 8 dwords, session, flags, reserved, VA, size. */
	dwords = ghci_cmd_vm_unmap(packet, 3U, 0x200000U, 0x4000U);
	expect(dwords == 8U && words_equal(vm_unmap, 8U), "vm_unmap words");

	/* SESSION_OPEN, SESSION_CLOSE and BIND_SESSION, with session 0 refused by the first two. */
	dwords = ghci_cmd_session_open(packet, 15U, 0U);
	expect(dwords == 4U && words_equal(open, 4U), "session_open words");
	expect(ghci_cmd_session_open(packet, 0U, 0U) == 0U, "session_open 0");
	dwords = ghci_cmd_session_close(packet, 15U);
	expect(dwords == 2U && words_equal(close, 2U), "session_close words");
	expect(ghci_cmd_session_close(packet, 0U) == 0U, "session_close 0");
	dwords = ghci_cmd_bind_session(packet, 0U);
	expect(dwords == 2U && words_equal(bind, 2U), "bind_session words");
	expect(ghci_cmd_bind_session(packet, 16U) == 0U, "bind_session 16");
}

/* Tests the bind and state commands. */
static void
test_bind_commands(void)
{
	static const uint32_t pipeline[] = { 0x00000320U, 0x00010040U, 0x00000002U };
	static const uint32_t vbufs[] = {
		0x00000A21U, 0x0002000EU, 0x1000U, 0U, 0x300U, 12U, 0x2000U, 0U, 0x400U, 0xFFFFFFFFU
	};
	static const uint32_t index[] = { 0x00000422U, 0x5000U, 0U, 1U };
	static const uint32_t table[] = { 0x00000423U, 0x6000U, 0U, 0x00100002U };
	static const uint32_t viewport[] = { 0x00000724U, 1U, 2U, 3U, 4U, 5U, 6U };
	static const uint32_t scissor[] = { 0x00000525U, 0xFFFFFFFFU, 2U, 640U, 480U };
	static const uint32_t stencil[] = { 0x00000626U, 3U, 0x0011FFFFU, 0x0022FF0FU, 0U, 0U };
	static const uint32_t constants[] = { 0x0000062DU, 24U, 3U, 7U, 8U, 9U };
	struct ghci_vertex_binding bindings[2];
	uint32_t floats[6];
	uint32_t payload[4];
	uint32_t data[GHCI_CONST_LOAD_MAX + 1U];
	uint32_t dwords;
	uint32_t index_value;

	/* BIND_PIPELINE: the PSO's 64-bit VA on 64 bytes, a misaligned one refused. */
	dwords = ghci_cmd_bind_pipeline(packet, 0x200010040ULL);
	expect(dwords == 3U && words_equal(pipeline, 3U), "bind_pipeline words");
	expect(ghci_cmd_bind_pipeline(packet, 0x20U) == 0U, "bind_pipeline unaligned");

	/* BIND_VERTEX_BUFFERS: first 14, two bindings, the second with the PSO's stride. */
	bindings[0].va = 0x1000U;
	bindings[0].bytes = 0x300U;
	bindings[0].stride = 12U;
	bindings[1].va = 0x2000U;
	bindings[1].bytes = 0x400U;
	bindings[1].stride = GHCI_STRIDE_FROM_PSO;
	dwords = ghci_cmd_bind_vertex_buffers(packet, 14U, 2U, bindings);
	expect(dwords == 10U && words_equal(vbufs, 10U), "bind_vertex_buffers words");

	/* BIND_VERTEX_BUFFERS refuses past binding 15 and an empty run; 16 at 0 is the edge. */
	expect(ghci_cmd_bind_vertex_buffers(packet, 15U, 2U, bindings) == 0U, "bind_vertex_buffers past 16");
	expect(ghci_cmd_bind_vertex_buffers(packet, 0U, 0U, bindings) == 0U, "bind_vertex_buffers empty");

	/* BIND_INDEX_BUFFER with 32-bit indices, BIND_DESCRIPTOR_TABLE of set 2 with 16 descriptors. */
	dwords = ghci_cmd_bind_index_buffer(packet, 0x5000U, 1U);
	expect(dwords == 4U && words_equal(index, 4U), "bind_index_buffer words");
	dwords = ghci_cmd_bind_descriptor_table(packet, 2U, 0x6000U, 16U);
	expect(dwords == 4U && words_equal(table, 4U), "bind_descriptor_table words");
	expect(ghci_cmd_bind_descriptor_table(packet, 4U, 0x6000U, 1U) == 0U, "bind_descriptor_table set 4");

	/* SET_VIEWPORT passes six float bit patterns; SET_SCISSOR a signed corner. */
	for (index_value = 0U; index_value < 6U; index_value++)
		floats[index_value] = index_value + 1U;
	dwords = ghci_cmd_set_viewport(packet, floats);
	expect(dwords == 7U && words_equal(viewport, 7U), "set_viewport words");
	dwords = ghci_cmd_set_scissor(packet, -1, 2, 640U, 480U);
	expect(dwords == 5U && words_equal(scissor, 5U), "set_scissor words");

	/* SET_DYNAMIC_STATE of the stencil parameters, and an ID the GPU does not have. */
	payload[0] = 0x0011FFFFU;
	payload[1] = 0x0022FF0FU;
	payload[2] = 0U;
	payload[3] = 0U;
	dwords = ghci_cmd_set_dynamic_state(packet, GHCI_DYNAMIC_STENCIL_PARAMS, payload);
	expect(dwords == 6U && words_equal(stencil, 6U), "set_dynamic_state words");
	expect(ghci_cmd_set_dynamic_state(packet, 5U, payload) == 0U, "set_dynamic_state 5");
	expect(ghci_cmd_set_dynamic_state(packet, 0x11U, payload) == 0U, "set_dynamic_state 0x11");

	/* LOAD_CONST of three words at c24; 32 words load, 33 and a load past c511 do not. */
	data[0] = 7U;
	data[1] = 8U;
	data[2] = 9U;
	dwords = ghci_cmd_load_const(packet, 24U, 3U, data);
	expect(dwords == 6U && words_equal(constants, 6U), "load_const words");
	for (index_value = 0U; index_value <= GHCI_CONST_LOAD_MAX; index_value++)
		data[index_value] = index_value;
	expect(ghci_cmd_load_const(packet, 0U, 32U, data) == 35U && packet[0] == 0x0000232DU, "load_const 32");
	expect(ghci_cmd_load_const(packet, 0U, 33U, data) == 0U, "load_const 33");
	expect(ghci_cmd_load_const(packet, 500U, 13U, data) == 0U, "load_const past the bank");
	expect(ghci_cmd_load_const(packet, 480U, 32U, data) == 35U, "load_const to c511");
}

/* Tests the draw, dispatch and render pass commands. */
static void
test_draw_commands(void)
{
	static const uint32_t draw[] = { 0x00000527U, 3U, 1U, 4U, 5U };
	static const uint32_t indexed[] = { 0x00000628U, 6U, 2U, 7U, 0xFFFFFFFDU, 8U };
	static const uint32_t indirect[] = { 0x00010729U, 0x7000U, 0U, 16U, 4U, 20U, 0U };
	static const uint32_t indirect_count[] = { 0x00000829U, 0x7000U, 0U, 0U, 8U, 16U, 0x8000U, 1U };
	static const uint32_t pass[] = { 0x00000A2BU, 0x9000U, 0U, 0x00200010U, 0x01E00280U, 0xFF000000U, 0x3F800000U, 0x80U, 0x301U, 0U };
	static const uint32_t dispatch[] = { 0x00000430U, 8U, 4U, 1U };
	static const uint32_t dispatch_indirect[] = { 0x0000062EU, 0xA000U, 0U, 12U, 0U, 0U };
	uint32_t area[4];
	uint32_t dwords;

	/* DRAW and DRAW_INDEXED with a negative vertex offset. */
	dwords = ghci_cmd_draw(packet, 3U, 1U, 4U, 5U);
	expect(dwords == 5U && words_equal(draw, 5U), "draw words");
	dwords = ghci_cmd_draw_indexed(packet, 6U, 2U, 7U, -3, 8U);
	expect(dwords == 6U && words_equal(indexed, 6U), "draw_indexed words");

	/* DRAW_INDIRECT: INDEXED in the header's flags[0] and dword 6 reserved, in both forms. */
	dwords = ghci_cmd_draw_indirect(packet, 1U, 0x7000U, 16U, 4U, 20U);
	expect(dwords == 7U && words_equal(indirect, 7U), "draw_indirect words");
	dwords = ghci_cmd_draw_indirect_count(packet, 0U, 0x7000U, 0U, 8U, 16U, 0x100008000ULL);
	expect(dwords == 8U && words_equal(indirect_count, 8U), "draw_indirect_count words");

	/* BEGIN_RENDER_PASS: the area packed x|y<<16 and width|height<<16, and END_RENDER_PASS. */
	area[0] = 16U;
	area[1] = 32U;
	area[2] = 640U;
	area[3] = 480U;
	dwords = ghci_cmd_begin_render_pass(packet, 0x9000U, area, 0xFF000000U, 0x3F800000U, 0x180U, 0x301U);
	expect(dwords == 10U && words_equal(pass, 10U), "begin_render_pass words");
	expect(ghci_cmd_begin_render_pass(packet, 0x9010U, area, 0U, 0U, 0U, 0U) == 0U, "begin_render_pass unaligned");
	dwords = ghci_cmd_end_render_pass(packet);
	expect(dwords == 1U && packet[0] == 0x0000012CU, "end_render_pass word");

	/* DISPATCH and DISPATCH_INDIRECT; an offset off a dword is refused. */
	dwords = ghci_cmd_dispatch(packet, 8U, 4U, 1U);
	expect(dwords == 4U && words_equal(dispatch, 4U), "dispatch words");
	dwords = ghci_cmd_dispatch_indirect(packet, 0xA000U, 12U);
	expect(dwords == 6U && words_equal(dispatch_indirect, 6U), "dispatch_indirect words");
	expect(ghci_cmd_dispatch_indirect(packet, 0xA000U, 2U) == 0U, "dispatch_indirect unaligned");

	/* DRAW_MESH_TASKS. */
	dwords = ghci_cmd_draw_mesh_tasks(packet, 1U, 2U, 3U);
	expect(dwords == 4U && packet[0] == 0x0000042AU && packet[3] == 3U, "draw_mesh_tasks words");
}

/* Tests BARRIER, FENCE, WAIT_FENCE, WRITE_TIMESTAMP and the queries. */
static void
test_sync_commands(void)
{
	static const uint32_t barrier[] = { 0x00000431U, 0x18U, 0x01U, 0x41U };
	static const uint32_t fence[] = { 0x00000633U, 0x1000U, 0x2U, 0xFFFFFFFFU, 0x00000001U, 3U };
	static const uint32_t wait[] = { 0x00000534U, 0x1008U, 0U, 1U, 0U };
	static const uint32_t stamp[] = { 0x00010435U, 0x2000U, 0U, 2U };
	static const uint32_t query[] = { 0x00000437U, 0x3000U, 0U, 0x03000005U };
	uint32_t dwords;

	/* BARRIER: drains the compute and ROP stages, holds VF, waits idle and flushes L2. */
	dwords = ghci_cmd_barrier(packet, GHCI_STAGE_CS | GHCI_STAGE_ROP, GHCI_STAGE_VF, GHCI_BARRIER_WAIT_IDLE | GHCI_BARRIER_FLUSH_L2);
	expect(dwords == 4U && words_equal(barrier, 4U), "barrier words");

	/* FENCE: a 64-bit bus address and sequence, IRQ and MEM_WRITE; an address off 8 bytes is refused. */
	dwords = ghci_cmd_fence(packet, 0x200001000ULL, 0x1FFFFFFFFULL, GHCI_FENCE_FLAG_IRQ | GHCI_FENCE_FLAG_MEM_WRITE);
	expect(dwords == 6U && words_equal(fence, 6U), "fence words");
	expect(ghci_cmd_fence(packet, 0x1004U, 1U, 0U) == 0U, "fence unaligned");

	/* WAIT_FENCE: 5 dwords. */
	dwords = ghci_cmd_wait_fence(packet, 0x1008U, 1U);
	expect(dwords == 5U && words_equal(wait, 5U), "wait_fence words");

	/* WRITE_TIMESTAMP to the host at bottom of pipe; stage 3 is refused. */
	dwords = ghci_cmd_write_timestamp(packet, 1U, 0x2000U, 2U);
	expect(dwords == 4U && words_equal(stamp, 4U), "write_timestamp words");
	expect(ghci_cmd_write_timestamp(packet, 0U, 0x2000U, 3U) == 0U, "write_timestamp stage 3");

	/* END_QUERY of query 5, PRECISE and AVAILABILITY in [24] and [25]. */
	dwords = ghci_cmd_query(packet, 1U, 0x3000U, 5U, GHCI_QUERY_PRECISE | GHCI_QUERY_AVAILABILITY);
	expect(dwords == 4U && words_equal(query, 4U), "end_query words");
	dwords = ghci_cmd_query(packet, 0U, 0x3000U, 5U, 0U);
	expect(dwords == 4U && packet[0] == 0x00000436U && packet[3] == 5U, "begin_query words");
	expect(ghci_cmd_query(packet, 0U, 0x3000U, 0x01000000U, 0U) == 0U, "query index past 24 bits");
}

/* Tests the transfer commands. */
static void
test_transfer_commands(void)
{
	static const uint32_t copy[] = { 0x00000840U, 0x1000U, 0U, 0x2000U, 0U, 64U, 0x2U, 0xDEADBEEFU };
	static const uint32_t image[] = {
		0x00000B41U, 0x1000U, 0U, 0x2000U, 0U, 256U, 512U, 0x00020001U, 0x00040003U, 0x00100008U, 0x00000225U
	};
	static const uint32_t blit[] = {
		0x00000C42U, 0x1000U, 0U, 0x2000U, 0U, 256U, 512U, 0x00020001U, 0x00400020U, 0U, 0x00100008U, 0x00052C25U
	};
	static const uint32_t fill[] = { 0x00000543U, 0x4000U, 0U, 128U, 0U };
	static const uint32_t resolve[] = {
		0x00000B44U, 0x1000U, 0U, 0x2000U, 0U, 256U, 512U, 0x00020001U, 0x00040003U, 0x00100008U, 0x00000825U
	};
	static const uint32_t to_image[] = { 0x00000A45U, 0x1000U, 0U, 0x2000U, 0U, 256U, 512U, 0x00040003U, 0x00100008U, 0x00000125U };
	static const uint32_t to_buffer[] = { 0x00000A46U, 0x1000U, 0U, 0x2000U, 0U, 256U, 512U, 0x00020001U, 0x00100008U, 0x00000125U };
	struct ghci_transfer transfer;
	uint32_t dwords;

	/* COPY_BUFFER with the fill flag; a size off a dword is refused. */
	dwords = ghci_cmd_copy_buffer(packet, 0x1000U, 0x2000U, 64U, GHCI_COPY_FILL_PATTERN, 0xDEADBEEFU);
	expect(dwords == 8U && words_equal(copy, 8U), "copy_buffer words");
	expect(ghci_cmd_copy_buffer(packet, 0x1000U, 0x2000U, 66U, 0U, 0U) == 0U, "copy_buffer unaligned");

	/* A transfer of R8G8B8A8_UNORM from (1, 2) to (3, 4), 8 by 16, into a tiled image. */
	memset(&transfer, 0, sizeof(transfer));
	transfer.src.va = 0x1000U;
	transfer.src.pitch = 256U;
	transfer.src.x = 1U;
	transfer.src.y = 2U;
	transfer.dst.va = 0x2000U;
	transfer.dst.pitch = 512U;
	transfer.dst.x = 3U;
	transfer.dst.y = 4U;
	transfer.dst.tiled = 1U;
	transfer.width = 8U;
	transfer.height = 16U;
	transfer.format = 37U;

	/* COPY_IMAGE: the format with dst_tiled in [9]. */
	dwords = ghci_cmd_copy_image(packet, &transfer);
	expect(dwords == 11U && words_equal(image, 11U), "copy_image words");

	/* BLIT: source (1, 2)-(32, 64), destination (0, 0)-(8, 16), linear, R8G8B8A8_UNORM to B8G8R8A8_UNORM. */
	transfer.src_x1 = 32U;
	transfer.src_y1 = 64U;
	transfer.dst.x = 0U;
	transfer.dst.y = 0U;
	transfer.dst_x1 = 8U;
	transfer.dst_y1 = 16U;
	transfer.dst_format = 44U;
	transfer.filter = 1U;
	dwords = ghci_cmd_blit(packet, &transfer);
	expect(dwords == 12U && words_equal(blit, 12U), "blit words");

	/* FILL_BUFFER. */
	dwords = ghci_cmd_fill_buffer(packet, 0x4000U, 128U, 0U);
	expect(dwords == 5U && words_equal(fill, 5U), "fill_buffer words");

	/* RESOLVE_IMAGE: 4 samples coded 2 in [11:10], the average mode; 3 samples are refused. */
	transfer.dst.x = 3U;
	transfer.dst.y = 4U;
	transfer.samples = 4U;
	transfer.resolve_mode = 0U;
	dwords = ghci_cmd_resolve_image(packet, &transfer);
	expect(dwords == 11U && words_equal(resolve, 11U), "resolve_image words");
	transfer.samples = 3U;
	expect(ghci_cmd_resolve_image(packet, &transfer) == 0U, "resolve_image 3 samples");

	/* COPY_BUFFER_TO_IMAGE: the image's corner and tiling; COPY_IMAGE_TO_BUFFER the source's. */
	transfer.src.tiled = 0U;
	dwords = ghci_cmd_copy_buffer_to_image(packet, &transfer);
	expect(dwords == 10U && words_equal(to_image, 10U), "copy_buffer_to_image words");
	transfer.src.tiled = 1U;
	transfer.dst.tiled = 0U;
	dwords = ghci_cmd_copy_image_to_buffer(packet, &transfer);
	expect(dwords == 10U && words_equal(to_buffer, 10U), "copy_image_to_buffer words");
}

/* Tests NOP, DISP_SET and FLIP. */
static void
test_display_commands(void)
{
	static const uint32_t disp[] = { 0x00010500U | 0x50U, 0x10U, 1U, 0x28U, 2560U };
	static const uint32_t flip[] = { 0x00000851U, 0U, 0x80000000U, 0U, 0x9U, 42U, 0x1000U, 0U };
	uint32_t offsets[GHCI_DISP_SET_PAIRS_MAX + 1U];
	uint32_t values[GHCI_DISP_SET_PAIRS_MAX + 1U];
	uint32_t dwords;
	uint32_t index;

	/* NOP: 1 to 255 dwords with a zero payload; 0 and 256 are refused. */
	memset(packet, 0xA5, sizeof(packet));
	dwords = ghci_cmd_nop(packet, 255U);
	expect(dwords == 255U && packet[0] == 0x0000FF00U && packet[254] == 0U && packet[255] == 0xA5A5A5A5U, "nop 255");
	expect(ghci_cmd_nop(packet, 0U) == 0U, "nop 0");
	expect(ghci_cmd_nop(packet, 256U) == 0U, "nop 256");

	/* DISP_SET of two pairs with the idle wait: 1 + 2N dwords. */
	offsets[0] = 0x10U;
	values[0] = 1U;
	offsets[1] = 0x28U;
	values[1] = 2560U;
	dwords = ghci_cmd_disp_set(packet, 1U, 2U, offsets, values);
	expect(dwords == 5U && words_equal(disp, 5U), "disp_set words");

	/* DISP_SET of 127 pairs fills a packet; 128 do not fit. */
	for (index = 0U; index <= GHCI_DISP_SET_PAIRS_MAX; index++) {
		offsets[index] = index;
		values[index] = index;
	}
	expect(ghci_cmd_disp_set(packet, 0U, 127U, offsets, values) == 255U, "disp_set 127");
	expect(ghci_cmd_disp_set(packet, 0U, 128U, offsets, values) == 0U, "disp_set 128");

	/* FLIP of plane 0 at vsync with a record; display 1 is refused. */
	dwords = ghci_cmd_flip(packet, 0U, 0x80000000U, GHCI_FLIP_VSYNC_WAIT | GHCI_FLIP_RECORD_ENABLE, 42U, 0x1000U);
	expect(dwords == 8U && words_equal(flip, 8U), "flip words");
	expect(ghci_cmd_flip(packet, 1U, 0x80000000U, 0U, 0U, 0U) == 0U, "flip display 1");
}

/* Tests the pipeline state, the descriptors and the render pass description. */
static void
test_records(void)
{
	struct ghci_pso pso;
	struct ghci_texture_desc texture;
	struct ghci_sampler_desc sampler;
	struct ghci_buffer_desc buffer;
	struct ghci_render_pass_desc pass;
	uint8_t bytes[GHCI_RENDER_PASS_BYTES];
	int error;

	/* A triangle list with depth test LESS, alpha blending and two attributes. */
	memset(&pso, 0, sizeof(pso));
	pso.vs_code = 0x10000U;
	pso.fs_code = 0x10100U;
	pso.topology = 3U;
	pso.sample_mask = 0xFU;
	pso.cull_mode = 2U;
	pso.line_width_fx4 = 16U;
	pso.depth_test = 1U;
	pso.depth_write = 1U;
	pso.depth_op = 1U;
	pso.blend_enable = 1U;
	pso.src_color = 6U;
	pso.dst_color = 7U;
	pso.src_alpha = 1U;
	pso.dst_alpha = 0U;
	pso.write_mask = 0xFU;
	pso.strides[0] = 20U;
	pso.regs_vs = 62U;
	pso.regs_fs = 10U;
	pso.varyings = 2U;
	pso.attribute_count = 2U;
	pso.attributes[0].format = 106U;
	pso.attributes[1].format = 103U;
	pso.attributes[1].offset = 12U;
	pso.attributes[1].binding = 1U;
	pso.attributes[1].per_instance = 1U;
	memset(bytes, 0xA5, sizeof(bytes));
	error = ghci_pso_encode(bytes, &pso);
	expect(error == 0, "pso encode");

	/* dw8 = topology 3 | mask 0xF << 12; dw9 = cull 2 | 16 << 6; dw10 = test | write | LESS << 2. */
	expect(byte_word(bytes, 0x00U) == 0x10000U && byte_word(bytes, 0x08U) == 0x10100U, "pso code");
	expect(byte_word(bytes, 0x20U) == 0x0000F003U, "pso dw8");
	expect(byte_word(bytes, 0x24U) == 0x00000402U, "pso dw9");
	expect(byte_word(bytes, 0x28U) == 0x00000007U, "pso dw10");

	/* dw11 = enable | SRC_ALPHA 6 << 1 | ONE_MINUS_SRC_ALPHA 7 << 6 | ONE 1 << 14 | 0xF << 27 = 0x780041CD. */
	expect(byte_word(bytes, 0x2CU) == 0x780041CDU, "pso dw11");
	expect(byte_word(bytes, 0x30U) == 20U && byte_word(bytes, 0x34U) == 0x02000A3EU, "pso dw12 dw13");

	/* Attribute 1: format 103, binding 1, per instance, offset 12 << 13; unused ones are 0. */
	expect(byte_word(bytes, 0x40U) == 106U && byte_word(bytes, 0x44U) == 0x00019167U, "pso attributes");
	expect(byte_word(bytes, 0x48U) == 0U && byte_word(bytes, 0x7CU) == 0U, "pso unused attributes");

	/* A pipeline state field past its bits is refused. */
	pso.topology = 5U;
	expect(ghci_pso_encode(bytes, &pso) == EINVAL, "pso topology 5");
	pso.topology = 3U;
	pso.attributes[0].offset = 524288U;
	expect(ghci_pso_encode(bytes, &pso) == EINVAL, "pso offset past 19 bits");

	/* A 256 x 128 tiled texture of 9 mips with an R,G,B,ONE swizzle. */
	memset(&texture, 0, sizeof(texture));
	texture.base = 0x100020000ULL;
	texture.width = 256U;
	texture.height = 128U;
	texture.depth = 1U;
	texture.format = 37U;
	texture.mip_levels = 9U;
	texture.row_pitch = 1024U;
	texture.samples = 1U;
	texture.dimension = 1U;
	texture.flags = GHCI_TEX_FLAG_TILED;
	texture.swizzle = 2U | (3U << 3) | (4U << 6) | (1U << 9);
	memset(bytes, 0xA5, sizeof(bytes));
	error = ghci_texture_desc_encode(bytes, &texture);
	expect(error == 0, "texture encode");
	expect(byte_word(bytes, 0x00U) == 0x00020000U && byte_word(bytes, 0x04U) == 1U, "texture base");
	expect(byte_word(bytes, 0x08U) == 0x00800100U && byte_word(bytes, 0x0CU) == 0x09250001U, "texture extent format mips");
	expect(byte_word(bytes, 0x18U) == 0x00010101U && byte_word(bytes, 0x1CU) == 0x0000031AU, "texture samples flags swizzle");
	texture.samples = 3U;
	expect(ghci_texture_desc_encode(bytes, &texture) == EINVAL, "texture 3 samples");

	/* A linear, clamped, unnormalized sampler with a bias of -1.5. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.min_filter = 1U;
	sampler.mag_filter = 1U;
	sampler.address[0] = 2U;
	sampler.address[1] = 2U;
	sampler.address[2] = 2U;
	sampler.lod_bias_fx8 = -384;
	sampler.max_lod_fx4 = 0xF0U;
	sampler.max_anisotropy = 1U;
	sampler.unnormalized = 1U;
	error = ghci_sampler_desc_encode(bytes, &sampler);
	expect(error == 0, "sampler encode");
	expect(byte_word(bytes, 0x00U) == 0x02000101U && byte_word(bytes, 0x04U) == 0x00000202U, "sampler modes");
	expect(byte_word(bytes, 0x08U) == 0xF000FE80U && byte_word(bytes, 0x0CU) == 0x01000001U, "sampler bias lod unnormalized");

	/* A read-only buffer of 4096 bytes. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.base = 0x30000U;
	buffer.bytes = 4096U;
	buffer.flags = 1U;
	error = ghci_buffer_desc_encode(bytes, &buffer);
	expect(error == 0, "buffer encode");
	expect(byte_word(bytes, 0x00U) == 0x30000U && byte_word(bytes, 0x08U) == 4096U && byte_word(bytes, 0x0CU) == 0x01000000U, "buffer fields");

	/* A 640 x 480 pass with colour 0 cleared and stored and a D32 depth attachment. */
	memset(&pass, 0, sizeof(pass));
	pass.colors[0].va = 0x40000U;
	pass.colors[0].row_pitch = 2560U;
	pass.colors[0].format = 37U;
	pass.colors[0].ops = 1U;
	pass.depth_stencil.va = 0x80000U;
	pass.depth_stencil.row_pitch = 2560U;
	pass.depth_stencil.format = 126U;
	pass.depth_stencil.ops = 0x5U;
	pass.width = 640U;
	pass.height = 480U;
	pass.layers = 1U;
	pass.samples = 1U;
	pass.clear_table = 0x5000U;
	memset(bytes, 0xA5, sizeof(bytes));
	error = ghci_render_pass_desc_encode(bytes, &pass);
	expect(error == 0, "render pass encode");
	expect(byte_word(bytes, 0x00U) == 0x40000U && byte_word(bytes, 0x08U) == 2560U && byte_word(bytes, 0x0CU) == 0x00000125U, "render pass colour 0");
	expect(byte_word(bytes, 0x10U) == 0U && byte_word(bytes, 0x1CU) == 0U, "render pass colour 1 empty");
	expect(byte_word(bytes, 0x80U) == 0x80000U && byte_word(bytes, 0x8CU) == 0x0000057EU, "render pass depth");
	expect(byte_word(bytes, 0xA0U) == 0x01E00280U && byte_word(bytes, 0xA4U) == 0x00010001U, "render pass extent");
	expect(byte_word(bytes, 0xA8U) == 0x5000U && byte_word(bytes, 0xBCU) == 0U, "render pass clear table and reserved");

	/* Layers other than 1 are refused (attachments are linear subresources). */
	pass.layers = 2U;
	expect(ghci_render_pass_desc_encode(bytes, &pass) == EINVAL, "render pass 2 layers");
}

/* Tests the 64-bit fence comparison across the wrap. */
static void
test_fences(void)
{
	/* Equal, behind and ahead. */
	expect(ghci_fence64_passed(5U, 5U) == 1, "fence equal");
	expect(ghci_fence64_passed(4U, 5U) == 0, "fence behind");
	expect(ghci_fence64_passed(6U, 5U) == 1, "fence ahead");

	/* Across the wrap of 64 bits. */
	expect(ghci_fence64_passed(2U, 0xFFFFFFFFFFFFFFFEULL) == 1, "fence past the wrap");
	expect(ghci_fence64_passed(0xFFFFFFFFFFFFFFFEULL, 2U) == 0, "fence before the wrap");

	/* A value that passed 32 bits but not 64 is still behind. */
	expect(ghci_fence64_passed(0x100000000ULL, 0x100000001ULL) == 0, "fence above 32 bits");
}

/* Tests the ring's room and the padding before its end. */
static void
test_ring(void)
{
	uint32_t pad;
	int error;

	/* An empty ring of 1024 dwords leaves 1023 free; a run of 10 at 0 needs no padding. */
	pad = 99U;
	error = ghci_ring_plan(0U, 0U, 1024U, 10U, &pad);
	expect(error == 0 && pad == 0U, "ring plan empty");

	/* A run of 10 at 1020 pads the last 4 dwords. */
	error = ghci_ring_plan(1020U, 1020U, 1024U, 10U, &pad);
	expect(error == 0 && pad == 4U, "ring plan pad");

	/* A full ring (one dword unused) has no room. */
	error = ghci_ring_plan(1023U, 0U, 1024U, 1U, &pad);
	expect(error == EAGAIN, "ring plan full");

	/* A run that fits only without the padding waits. */
	error = ghci_ring_plan(1020U, 8U, 1024U, 10U, &pad);
	expect(error == EAGAIN, "ring plan pad does not fit");
}

/* Tests the walk of the extended capability list. */
static void
test_extcap(void)
{
	uint32_t offsets[GHCI_EXTCAP_IDS];
	uint32_t versions[GHCI_EXTCAP_IDS];
	uint32_t found;
	int error;

	/* No list at all. */
	window_reset();
	error = ghci_extcap_walk(window, offsets, versions, &found);
	expect(error == 0 && found == 0U, "extcap none");

	/* FMT at 0x140 (next 2 dwords), MEM at 0x1E0 (next 2), DISP2 v1 at 0x200 (last). */
	window_reset();
	window[GHCI_REG_EXTCAP_PTR / 4U] = 0x100U;
	window[0x100U / 4U] = 0x0002010AU;
	window[0x104U / 4U] = 0x140U;
	window[0x108U / 4U] = 0x00020108U;
	window[0x10CU / 4U] = 0x1E0U;
	window[0x110U / 4U] = 0x0000010BU;
	window[0x114U / 4U] = 0x200U;
	error = ghci_extcap_walk(window, offsets, versions, &found);
	expect(error == 0, "extcap three");
	expect(found == ((1U << 10) | (1U << 8) | (1U << 11)), "extcap three found");
	expect(offsets[10] == 0x140U && offsets[8] == 0x1E0U && offsets[11] == 0x200U && versions[11] == 1U, "extcap three offsets");

	/* A list that loops back to its first entry is refused. */
	window[0x110U / 4U] = 0xFFFC010BU;
	error = ghci_extcap_walk(window, offsets, versions, &found);
	expect(error == EINVAL, "extcap loop");

	/* A block outside the window is refused. */
	window[0x110U / 4U] = 0x0000010BU;
	window[0x114U / 4U] = 0x400U;
	error = ghci_extcap_walk(window, offsets, versions, &found);
	expect(error == EINVAL, "extcap block outside");

	/* An entry outside the list area is refused. */
	window_reset();
	window[GHCI_REG_EXTCAP_PTR / 4U] = 0x140U;
	error = ghci_extcap_walk(window, offsets, versions, &found);
	expect(error == EINVAL, "extcap entry outside");

	/* A second entry of the same ID is refused. */
	window_reset();
	window[GHCI_REG_EXTCAP_PTR / 4U] = 0x100U;
	window[0x100U / 4U] = 0x00020108U;
	window[0x104U / 4U] = 0x1E0U;
	window[0x108U / 4U] = 0x00000108U;
	window[0x10CU / 4U] = 0x1E0U;
	error = ghci_extcap_walk(window, offsets, versions, &found);
	expect(error == EINVAL, "extcap duplicate");
}

/* Tests the identification and the decode of the capability registers. */
static void
test_caps(void)
{
	struct ghci_caps caps;
	int error;

	/* Versions 1.0 and 1.3 are GHCI 1.0; 2.0, 0 and a wrong magic are not. */
	window_reset();
	expect(ghci_caps_identify(window) == 0, "identify 1.0");
	window[GHCI_REG_VERSION / 4U] = 0x00010003U;
	expect(ghci_caps_identify(window) == 0, "identify 1.3");
	window[GHCI_REG_VERSION / 4U] = 0x00020000U;
	expect(ghci_caps_identify(window) == ENODEV, "identify 2.0");
	window[GHCI_REG_VERSION / 4U] = 0U;
	expect(ghci_caps_identify(window) == ENODEV, "identify 0");
	window_reset();
	window[GHCI_REG_MAGIC / 4U] = 0x47484348U;
	expect(ghci_caps_identify(window) == ENODEV, "identify magic");

	/*
	 * A device with HW_WALKER, GSI1, one shader unit, BLT, DISP and MSAA;
	 * FEATURE: VA 40 (0xA), PA 48 (8 << 4), MRT_BLEND, MAX_MRT 8, LDS 4
	 * KiB, and FP16 set in the mirror only.
	 */
	window_reset();
	window[GHCI_REG_DEVICE_CAPS / 4U] = 0xE0010084U;
	window[GHCI_REG_EXT_CAP_FEATURE / 4U] = 0x1048808AU;
	window[GHCI_REG_RESIDENT_WARPS / 4U] = 0x003F0010U;
	window[GHCI_REG_GCLK_HZ / 4U] = 100000000U;
	error = ghci_caps_decode(window, &caps);
	expect(error == 0, "caps decode");
	expect(caps.va_bits == 40U && caps.pa_bits == 48U, "caps address widths");
	expect(caps.num_su == 1U && caps.max_mrt == 8U && caps.lds_bytes == 4096U, "caps units mrt lds");
	expect(caps.resident_warps == 16U && caps.lane_registers == 63U, "caps warps registers");
	expect(caps.mismatches == GHCI_CAP_FP16, "caps mirror mismatch");
	expect(ghci_caps_has(&caps, GHCI_HAS_HW_WALKER) == 1 && ghci_caps_has(&caps, GHCI_HAS_BLT) == 1, "caps has walker blt");
	expect(ghci_caps_has(&caps, GHCI_HAS_MRT_BLEND) == 1 && ghci_caps_has(&caps, GHCI_HAS_PROT) == 0, "caps has mrt blend, no prot");

	/* DISP needs the DISP2 block too; without the list the display is absent. */
	expect(ghci_caps_has(&caps, GHCI_HAS_DISPLAY) == 0, "caps display without DISP2");

	/* With MEM showing local memory and UMA clear, the device is discrete; MAX_MRT 0 reads 1. */
	window[GHCI_REG_EXT_CAP_FEATURE / 4U] = 0x00000000U;
	window[GHCI_REG_EXTCAP_PTR / 4U] = 0x100U;
	window[0x100U / 4U] = 0x00020108U;
	window[0x104U / 4U] = 0x1E0U;
	window[0x108U / 4U] = 0x0000010BU;
	window[0x10CU / 4U] = 0x200U;
	window[0x1E0U / 4U] = 0x80000000U;
	window[0x1E4U / 4U] = 0x40000000U;
	window[0x1E8U / 4U] = 0x01000000U;
	error = ghci_caps_decode(window, &caps);
	expect(error == 0 && caps.max_mrt == 1U && caps.va_bits == 30U && caps.pa_bits == 40U, "caps zero feature");
	expect(caps.local_base == 0x80000000U && caps.local_bytes == 0x40000000U && caps.window_bytes == 0x01000000U, "caps mem block");
	expect(ghci_caps_has(&caps, GHCI_HAS_DISCRETE) == 1 && ghci_caps_has(&caps, GHCI_HAS_DISPLAY) == 1, "caps discrete display");

	/* A malformed list leaves the device decoded without blocks. */
	window[0x108U / 4U] = 0xFFFE010BU;
	error = ghci_caps_decode(window, &caps);
	expect(error == 0 && caps.blocks == 0U && ghci_caps_has(&caps, GHCI_HAS_DISCRETE) == 0, "caps malformed list");

	/*
	 * VA_BITS_ENC is VA - 30: 0 is 30 bits, 2 is 32, 0xA is 40, 0xF 45
	 * (section 3.2.2).
	 */
	window_reset();
	window[GHCI_REG_EXT_CAP_FEATURE / 4U] = 0x00000002U;
	error = ghci_caps_decode(window, &caps);
	expect(error == 0 && caps.va_bits == 32U, "caps VA 32");
	window[GHCI_REG_EXT_CAP_FEATURE / 4U] = 0x0000000FU;
	error = ghci_caps_decode(window, &caps);
	expect(error == 0 && caps.va_bits == 45U, "caps VA 45");

	/*
	 * The optional blocks: CLIP_POLY [30] and BLT_SCALE [31] of FEATURE;
	 * FFILT_FP32 [11], MAX_COLOR [15:12], TU_BILINEAR [16], BLT_BLEND
	 * [17] and BLT_TILE [18] of FMT0.  The blitter's bits read 0 without
	 * CAP_BLT, FFILT_FP32 without CAP_TEX_FFILT; FMT0's FFILT_FP16 [10]
	 * mirrors CAP_TEX_FFILT.
	 */
	window_reset();
	window[GHCI_REG_DEVICE_CAPS / 4U] = GHCI_CAP_BLT | GHCI_CAP_TEX_FFILT;
	window[GHCI_REG_EXT_CAP_FEATURE / 4U] = 0xC0000000U;
	window[GHCI_REG_EXT_CAP_FMT0 / 4U] = 0x00073C00U;
	error = ghci_caps_decode(window, &caps);
	expect(error == 0 && caps.max_color == 3U && caps.mismatches == 0U, "caps FMT0 colour attachments, FFILT mirror");
	expect(ghci_caps_has(&caps, GHCI_HAS_CLIP_POLY) == 1 && ghci_caps_has(&caps, GHCI_HAS_BLT_SCALE) == 1 &&
	       ghci_caps_has(&caps, GHCI_HAS_TU_BILINEAR) == 1 && ghci_caps_has(&caps, GHCI_HAS_BLT_BLEND) == 1 &&
	       ghci_caps_has(&caps, GHCI_HAS_BLT_TILE) == 1 && ghci_caps_has(&caps, GHCI_HAS_FFILT_FP32) == 1,
	       "caps optional blocks present");
	window[GHCI_REG_DEVICE_CAPS / 4U] = 0U;
	error = ghci_caps_decode(window, &caps);
	expect(error == 0 && caps.mismatches == GHCI_CAP_TEX_FFILT, "caps FFILT_FP16 without CAP_TEX_FFILT");
	expect(ghci_caps_has(&caps, GHCI_HAS_CLIP_POLY) == 1 && ghci_caps_has(&caps, GHCI_HAS_TU_BILINEAR) == 1 &&
	       ghci_caps_has(&caps, GHCI_HAS_BLT_SCALE) == 0 && ghci_caps_has(&caps, GHCI_HAS_BLT_BLEND) == 0 &&
	       ghci_caps_has(&caps, GHCI_HAS_BLT_TILE) == 0 && ghci_caps_has(&caps, GHCI_HAS_FFILT_FP32) == 0,
	       "caps blitter bits without CAP_BLT, FP32 filter without CAP_TEX_FFILT");
	window[GHCI_REG_EXT_CAP_FEATURE / 4U] = 0U;
	window[GHCI_REG_EXT_CAP_FMT0 / 4U] = 0U;
	error = ghci_caps_decode(window, &caps);
	expect(error == 0 && ghci_caps_has(&caps, GHCI_HAS_CLIP_POLY) == 0 && ghci_caps_has(&caps, GHCI_HAS_TU_BILINEAR) == 0,
	       "caps no clipper, no bilinear filter");
}

/* Tests the page table heap, watchdog and reset wait sizes. */
static void
test_sizes(void)
{
	/* The root of each walk, with four level 1 tables over 4 GiB for 40 bits. */
	expect(ghci_heap_session_bytes(32U) == 16384U, "heap session 32");
	expect(ghci_heap_session_bytes(40U) == 8192U + 4U * 4096U, "heap session 40");

	/* 16 sessions, 2048 level 0 tables and 64 KiB spare. */
	expect(ghci_heap_device_bytes(32U) == 16U * 16384U + 2048U * 4096U + 65536U, "heap device 32");
	expect(ghci_heap_device_bytes(40U) == 16U * 24576U + 2048U * 4096U + 65536U, "heap device 40");

	/* A 30-bit VA: the 2-level root, and the 512 chunks of 1 GiB. */
	expect(ghci_heap_session_bytes(30U) == 16384U, "heap session 30");
	expect(ghci_va_chunks(30U) == 512U && ghci_va_chunks(31U) == 1024U && ghci_va_chunks(32U) == 2048U &&
	       ghci_va_chunks(40U) == 2048U, "VA chunks");
	expect(ghci_heap_device_bytes(30U) == 16U * 16384U + 512U * 4096U + 65536U, "heap device 30");

	/* Two seconds at 100 MHz; an unknown clock; a time the register cannot hold. */
	expect(ghci_watchdog_cycles(100000000U, 2000U) == 200000000U, "watchdog 2 s");
	expect(ghci_watchdog_cycles(0U, 2000U) == 0U, "watchdog unknown clock");
	expect(ghci_watchdog_cycles(4000000000U, 2000U) == 0xFFFFFFFFU, "watchdog clamped");

	/* 4 x 2^28 cycles at 100 MHz is 10737 ms: clamped to 10 s; 2 s gives 8.5 s; a short one 1 s. */
	expect(ghci_reset_wait_ms(100000000U, 0x10000000U) == 11237U, "reset wait default");
	expect(ghci_reset_wait_ms(100000000U, 200000000U) == 8500U, "reset wait 2 s");
	expect(ghci_reset_wait_ms(100000000U, 1000U) == 1000U, "reset wait minimum");
	expect(ghci_reset_wait_ms(0U, 0U) == 10000U, "reset wait unknown clock");
}
