/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The running of a command buffer's records on the GPU.
 *
 * Each session has up to EXECUTE_SLOTS runs of the executor's memory for
 * its submissions (slots), in system memory the CPU writes through its
 * cache: each an indirect buffer of command packets, the resource tables
 * of its draws, the word its barriers write and wait for, a zero page,
 * the corners of a quad, and the rows a copy of a depth image passes
 * through (its scratch).  The records are turned into packets in order
 * (vk-state.c) and the indirect buffer is put on the graphics queue when
 * it is full and at the end, without waiting for the GPU: the next buffer
 * is written in a slot whose run has ended (a new slot is made while one
 * is to be had, else the oldest run is waited for).  The submission's
 * fence and the node's marker after it end once the GPU has done the
 * runs; what a command buffer's records name stays held by its records,
 * and the packets and tables were copied into the slot, so a command
 * buffer may be recorded again as soon as its submission returns.
 *
 * A draw writes everything it uses: for each of its two programs a
 * resource table of its own (the push constants, the buffer descriptors
 * of its storage and uniform buffers, at their bind's dynamic offsets for
 * dynamic ones, the image and sampler descriptors of its sampled images,
 * and the vertex program's vertex buffer descriptors, where the compiler
 * placed each), the target, the viewport, scissor, blend and its
 * constants, culling and depth bias, the depth and stencil control, and
 * the programs; an indexed draw reads its indices from the bound index
 * buffer.  The state a pipeline leaves dynamic is the last its command
 * buffer set.  A pass with a depth attachment sets the depth unit's
 * surface (with its stencil plane), and a pass that clears it clears it
 * with its colour in one quad whose depth is the clear value (the
 * viewport's depth range is that one value) and whose stencil test
 * replaces the stencil with the clear value.  A clear of attachments in a
 * pass draws such a quad over each of its rectangles.  A copy, a blit, a copy
 * between a buffer and an image, a clear and a pass's clear are drawn
 * too: a quad of the executor's own programs samples the source (in
 * pixel coordinates) into the destination, or writes one colour.  A copy
 * whose buffer is not laid out as a linear image can be (its start or its
 * row pitch off 256 bytes) is moved by the command processor, row by row,
 * and so is every copy of an image whose texels are not four bytes (one
 * of one or two bytes a texel, or a plane of a two-plane image).  A depth
 * image's copies are drawn through the depth unit and the texture unit,
 * which share its swizzle: into it, a quad fetches the bits of each texel
 * (of another depth image, or of the buffer's rows the command processor
 * moved into the scratch) and exports them as the depth or the stencil
 * reference; out of it, a quad packs the bits of its texels into the
 * scratch's colour pixels, whose rows are then moved into the buffer.  A
 * clear of a depth image is a pass's clear of it alone.  A reset
 * of queries marks them unavailable as the records are run.
 * Between a pass or a transfer and what follows, and at the end of a
 * command buffer's last indirect buffer, a barrier drains the programs,
 * writes the colour caches back and empties the shader caches (a buffer
 * that fills goes on its queue without one).
 *
 * A dispatch writes its kernel's resource table (from the sets bound at
 * the compute bind point), the kernel and the workgroups, then a barrier.
 * A command buffer of the compute queue family runs on a compute queue:
 * its buffers start with the compute state alone and its barriers are
 * the compute queue's; it runs dispatches only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-objects.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk-exec.h"

/* The runs (slots) a session writes its submissions in at most, one being written while the GPU reads the others. */
#define EXECUTE_SLOTS			4U

/* A run's size and where each of its parts starts. */
#define EXECUTE_RUN_BYTES		(2U * 1024U * 1024U)
#define EXECUTE_BUFFER_AT		0U
#define EXECUTE_BUFFER_BYTES		(1024U * 1024U)
#define EXECUTE_TABLES_AT		(1024U * 1024U)
#define EXECUTE_TABLES_BYTES		(768U * 1024U)
#define EXECUTE_SYNC_AT			(1792U * 1024U)
#define EXECUTE_ZERO_AT			(1796U * 1024U)
#define EXECUTE_ZERO_BYTES		4096U
#define EXECUTE_CORNERS_AT		(1800U * 1024U)
#define EXECUTE_SCRATCH_AT		(1804U * 1024U)
#define EXECUTE_SCRATCH_BYTES		(EXECUTE_RUN_BYTES - EXECUTE_SCRATCH_AT)

/* The words of the indirect buffer, and what one record writes at most into it (a transfer of AMDRDNA_VK_REGIONS_MAX quads). */
#define EXECUTE_BUFFER_WORDS		(EXECUTE_BUFFER_BYTES / 4U)
#define EXECUTE_RECORD_WORDS		16384U

/*
 * A resource table starts on 64 bytes; the most one record's tables take
 * (two tables for each of AMDRDNA_VK_REGIONS_MAX quads, each at most this
 * many bytes).
 */
#define EXECUTE_TABLE_ALIGN		64U
#define EXECUTE_TABLE_MAX		2048U
#define EXECUTE_RECORD_TABLES		(2U * AMDRDNA_VK_REGIONS_MAX * EXECUTE_TABLE_MAX)

/*
 * An indirect buffer of the graphics queue is a whole number of eight
 * words.
 *
 * Value referenced: Mesa 25.0.7 src/amd/common/ac_gpu_info.c
 * (ib_pad_dw_mask of the graphics IP: 0x7, line 650).
 */
#define EXECUTE_BUFFER_PAD		8U

/* The quad's corners: two triangles of six corners, two floats each. */
#define EXECUTE_CORNERS			6U
#define EXECUTE_CORNER_STRIDE		8U

/*
 * The float bits of -1 and 1.
 *
 * Values referenced: IEEE 754-2008 binary32 (sign 31, biased exponent
 * 30:23 with a bias of 127, fraction 22:0).
 */
#define EXECUTE_FLOAT_MINUS_ONE		0xbf800000U
#define EXECUTE_FLOAT_ONE		0x3f800000U

/*
 * The executor's quad programs: the vertex program, the textured fragment
 * program, the one-colour fragment program, the resolve's fragment
 * program, the three that write a texel's bits as a 32-bit float depth, a
 * 16-bit normalized depth or the stencil (in amdrdna_quad_depth_ir()'s
 * order), the three that pack the bits of one, two or four texels
 * into a colour pixel (a texel of 32, 16 or 8 bits), the one-colour
 * program again for each other export of a colour
 * (AMDRDNA_GFX10_COLOUR_R .. _SINT8 in order: a target of 32-bit or
 * integer channels, which takes the colour's words as they are), and the
 * three that write sample 0 of an image of samples as a 32-bit float
 * depth, a 16-bit normalized depth or the stencil (a pass's resolve of
 * its depth attachment).
 */
#define EXECUTE_QUAD_VERTEX		0U
#define EXECUTE_QUAD_TEXTURED		1U
#define EXECUTE_QUAD_COLOUR		2U
#define EXECUTE_QUAD_RESOLVE		3U
#define EXECUTE_QUAD_DEPTH_32		4U
#define EXECUTE_QUAD_DEPTH_16		5U
#define EXECUTE_QUAD_STENCIL		6U
#define EXECUTE_QUAD_PACK_1		7U
#define EXECUTE_QUAD_PACK_2		8U
#define EXECUTE_QUAD_PACK_4		9U
#define EXECUTE_QUAD_COLOUR_WIDE	10U
#define EXECUTE_QUAD_SAMPLE_ZERO	(EXECUTE_QUAD_COLOUR_WIDE + AMDRDNA_GFX10_COLOUR_KINDS - 1U)
#define EXECUTE_QUAD_PROGRAMS		(EXECUTE_QUAD_SAMPLE_ZERO + 3U)

/* The bytes of a pixel the packing programs write: four texels of 8 bits, two of 16, or one of 32. */
#define EXECUTE_PACK_BYTES		4U

/* A sample mask of every sample of a pixel. */
#define EXECUTE_SAMPLE_MASK_ALL		0xffffU

/* The stencil values a command buffer sets: the compare mask, the write mask and the reference (each face's second index). */
#define EXECUTE_STENCIL_COMPARE		0U
#define EXECUTE_STENCIL_WRITE		1U
#define EXECUTE_STENCIL_REFERENCE	2U
#define EXECUTE_STENCIL_VALUES		3U

/* A stencil clear's masks: every bit of the 8-bit stencil. */
#define EXECUTE_STENCIL_ALL		0xffU

/*
 * The executor's own trial of its draws (stage A9): two images of 64 by
 * 64 pixels, the first drawn blue with a red square from 16 to 48 inside,
 * the second a copy of it; the colours as a B8G8R8A8 pixel read as a
 * little-endian word (alpha in the top byte, blue at the bottom), and the
 * word both hold before they are drawn (any word no draw writes).
 *
 * Values referenced: the Vulkan 1.0 specification (VK_FORMAT_B8G8R8A8_UNORM:
 * B in byte 0, G in byte 1, R in byte 2, A in byte 3).
 */
#define EXECUTE_TRIAL_SIDE		64U
#define EXECUTE_TRIAL_INNER_FROM	16U
#define EXECUTE_TRIAL_INNER_TO		48U
#define EXECUTE_TRIAL_BLUE		0xff0000ffU
#define EXECUTE_TRIAL_RED		0xffff0000U
#define EXECUTE_TRIAL_UNDRAWN		0x5a5a5a5aU

/*
 * One run of the executor's memory a session writes submissions in: the
 * token that names it to the node (NULL until it is made), its two views,
 * and the GPU's run of the last indirect buffer written in it, which must
 * end before the slot is written again.
 */
struct execute_slot {
	void *token;
	uint8_t *cpu;
	uint64_t va;
	struct amdrdna_vk_ticket ticket;
};

/*
 * The executor's own programs and the session's runs.
 *
 * slots are the runs, slot the one being written; token, cpu and va are
 * that slot's.  pm4 writes its indirect buffer, tables is how much of its
 * tables' part the buffer uses, draws and quads how many application
 * draws and own quads it holds (for the node's counts), sync the last
 * value a barrier wrote (it only grows, so a slot's word never holds the
 * value a barrier waits for before the barrier writes it).  last names the
 * newest run put on each queue (0 the graphics queue, 1 the compute
 * queue), which a submission's fence waits for.  The quad programs are
 * compiled once, into a run of their own.
 */
struct amdrdna_vk_execute {
	struct execute_slot slots[EXECUTE_SLOTS];
	uint32_t slot;
	void *token;
	uint8_t *cpu;
	uint64_t va;
	struct amdrdna_vk_ticket last[2];
	struct amdrdna_vk_pm4 pm4;
	uint32_t tables;
	uint32_t draws;
	uint32_t quads;
	uint32_t sync;
	void *program_token;
	uint8_t *program_cpu;
	uint64_t program_va;
	struct amdrdna_vk_program quad[EXECUTE_QUAD_PROGRAMS];
	bool programs_ready;
	bool kernel_queue;
};

/*
 * A draw's two resource tables (each one's CPU view and GPU address) and
 * the vertex program's instance mask.
 */
struct execute_tables {
	uint8_t *vertex_cpu;
	uint64_t vertex_va;
	uint8_t *fragment_cpu;
	uint64_t fragment_va;
	uint32_t instance_mask;
};

/*
 * What a command buffer's records have bound so far (the index buffer,
 * its offset and VkIndexType among them; each set's dynamic offsets by
 * binding, at each bind point; the dynamic state set, each with whether it
 * was), and the pass they are in (its target_count colour targets by
 * colour attachment, each off with address 0, and its depth target when
 * depth_on, with stencil when stencil_on; samples its attachments'; the
 * pass and the framebuffer, for its resolves at its end; width and height
 * the size every target of it has, 0 for a pass without one).  dynamic_changed says that
 * a dynamic state changed since the last draw wrote the fixed state.  The drawn_ fields
 * are what the buffer's last draw wrote of the fixed state (its pipeline,
 * viewport, scissor and culling words) and the resource tables it used
 * (with the vertex program's instance mask), good while target_written
 * stays set (every internal draw, transfer, pass and new buffer clears it);
 * sets_changed (a bit for each set number) and buffers_changed say that
 * the descriptor sets or the vertex buffers changed since then, and
 * push_changed_from is the first byte of the push constants that changed
 * (AMDRDNA_VK_PUSH_BYTES for none; a program's table holds the bytes from 0
 * to its push_bytes); a binding of what is bound already changes nothing.  fixed_written says that the drawn_ fields of the
 * fixed state (with the topology, blend, raster and depth words) are what
 * the buffer's registers hold since the target was written, so that a draw
 * of another pipeline writes only what differs.
 */
struct execute_state {
	const struct amdrdna_vk_pipeline *pipeline;
	uint32_t viewport[6];
	bool viewport_set;
	VkRect2D scissor;
	bool scissor_set;
	const struct amdrdna_vk_descriptor_set *sets[AMDRDNA_VK_SETS_MAX];
	const struct amdrdna_vk_buffer *buffers[AMDRDNA_VK_VERTEX_BINDINGS_MAX];
	uint64_t offsets[AMDRDNA_VK_VERTEX_BINDINGS_MAX];
	uint8_t push[AMDRDNA_VK_PUSH_BYTES];
	const uint8_t *push_bytes;
	const struct amdrdna_vk_buffer *index_buffer;
	uint64_t index_offset;
	uint32_t index_type;
	uint32_t dynamic[AMDRDNA_VK_SETS_MAX][AMDRDNA_VK_DYNAMIC_MAX];
	uint32_t bias[3];
	bool bias_set;
	uint32_t constants[4];
	bool constants_set;
	uint32_t stencil[2][EXECUTE_STENCIL_VALUES];
	uint32_t stencil_set;
	bool dynamic_changed;
	bool in_pass;
	struct amdrdna_vk_target targets[AMDRDNA_VK_COLOURS_MAX];
	uint32_t target_count;
	uint32_t samples;
	const struct amdrdna_vk_render_pass *pass;
	const struct amdrdna_vk_framebuffer *framebuffer;
	uint32_t width;
	uint32_t height;
	bool target_written;
	bool depth_on;
	bool stencil_on;
	struct amdrdna_vk_target depth;
	VkRect2D area;
	const struct amdrdna_vk_pipeline *kernel_pipeline;
	const struct amdrdna_vk_descriptor_set *kernel_sets[AMDRDNA_VK_SETS_MAX];
	uint32_t kernel_dynamic[AMDRDNA_VK_SETS_MAX][AMDRDNA_VK_DYNAMIC_MAX];
	const struct amdrdna_vk_pipeline *drawn_pipeline;
	uint32_t drawn_viewport[6];
	int32_t drawn_scissor[4];
	uint32_t drawn_culling[AMDRDNA_GFX10_NGG_CULL_USER_SGPRS - AMDRDNA_GFX10_NGG_USER_SGPRS];
	struct execute_tables drawn_tables;
	uint32_t drawn_topology;
	struct amdrdna_vk_blend drawn_blend[AMDRDNA_VK_COLOURS_MAX];
	uint32_t drawn_constants[4];
	uint32_t drawn_raster[3];
	uint32_t drawn_samples[3];
	uint32_t drawn_bias[3];
	struct amdrdna_vk_depth_stencil drawn_depth;
	bool fixed_written;
	uint32_t sets_changed;
	bool buffers_changed;
	uint32_t push_changed_from;
	struct amdrdna_vk_video_query video_query;

	/*
	 * The occlusion queries begun and not ended, and whether one of them
	 * counts exactly: the draws while there are any have the depth units
	 * count their passing samples.
	 */
	uint32_t occlusion_queries;
	bool exact_counts;
};

/*
 * One quad an internal draw makes: the destination and its region; the
 * source texture (a linear image at source, its size and format), its
 * region in pixels and its filter, or the one colour (four float words)
 * when textured is clear; or with resolve set, the source an image of
 * source_samples samples (source its start, layer source_layer of it, an
 * array's when source_arrayed), its samples averaged.  A quad of a pass's clear may also clear the
 * pass's depth to depth_value (its float's bits) when depth_clear is set,
 * its stencil to stencil_value when stencil_clear is set, and write no
 * colour when colour_off is set.  A quad of a depth image's copy runs a
 * fragment program of its own (program, when not 0), reads the source
 * through the image descriptor source_words its caller made (when
 * described is set, nearest), the colour its push constants' (a packing
 * program's first texel), and writes the depth surface the caller set: its
 * depth the program exports when depth_write is set, its stencil the
 * program's reference when stencil_write is.
 */
struct execute_quad {
	struct amdrdna_vk_target target;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	bool textured;
	uint64_t source;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t source_format;
	int32_t u0;
	int32_t v0;
	int32_t u1;
	int32_t v1;
	uint32_t filter;
	bool resolve;
	uint32_t source_samples;
	uint32_t source_layer;
	bool source_arrayed;
	uint32_t colour[4];
	bool depth_clear;
	bool stencil_clear;
	bool colour_off;
	uint32_t depth_value;
	uint32_t stencil_value;
	uint32_t program;
	bool described;
	uint32_t source_words[AMDRDNA_IMAGE_WORDS];
	bool depth_write;
	bool stencil_write;
};

static int execute_open(struct amdrdna_vk_session *session, struct amdrdna_vk_execute **execute);
static int execute_programs(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute);
static int execute_compile(struct amdrdna_quad_ir *program, const struct amdrdna_gfx10_compile_options *options, struct amdrdna_vk_program *compiled);
static int execute_start(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute);
static int execute_slot_make(struct amdrdna_vk_session *session, struct execute_slot *slot);
static int execute_flush(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute);
static int execute_put(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute);
static int execute_room(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state);
static void execute_barrier(struct amdrdna_vk_execute *execute);
static int execute_record(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static void execute_bind(struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_video(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static void execute_bind_sets(const struct amdrdna_vk_record *record, const struct amdrdna_vk_descriptor_set **sets, uint32_t (*dynamic)[AMDRDNA_VK_DYNAMIC_MAX], uint32_t *changed);
static uint32_t execute_dynamic(const struct amdrdna_vk_descriptor_set *set, const uint32_t *offsets, uint32_t binding, uint32_t element);
static void execute_bind_dynamic(struct execute_state *state, const struct amdrdna_vk_record *record);
static void execute_bind_stencil(struct execute_state *state, const struct amdrdna_vk_record *record);
static bool execute_kernel_binds(const struct amdrdna_vk_record *record);
static void execute_bind_kernel(struct execute_state *state, const struct amdrdna_vk_record *record);
static bool execute_graphics_only(uint32_t opcode);
static int execute_dispatch(struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_wait(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute);
static int execute_read_back(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record, uint32_t *values);
static int execute_indirect(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_buffer_write(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_query_copy(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_kernel_resources(const struct amdrdna_gfx10_program *program, const struct execute_state *state, uint64_t groups_va, uint8_t *table);
static int execute_part(struct amdrdna_vk_execute *execute, uint32_t bytes, uint8_t **cpu, uint64_t *va);
static int execute_begin_pass(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_draw(struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_views(struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_view_targets(struct execute_state *state, uint32_t view);
static int execute_draw_fixed(struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_pipeline *pipeline, const uint32_t *viewport, const int32_t *scissor);
static void execute_draw_dynamic(const struct execute_state *state, const struct amdrdna_vk_pipeline *pipeline, uint32_t *bias, uint32_t *constants, struct amdrdna_vk_depth_stencil *depth);
static int execute_clear_attachments(struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static uint32_t execute_program_sets(const struct amdrdna_gfx10_program *program);
static int execute_draw_tables(struct amdrdna_vk_execute *execute, const struct execute_state *state, const struct amdrdna_vk_pipeline *pipeline, struct execute_tables *tables);
static int execute_table_copy(struct amdrdna_vk_execute *execute, const struct amdrdna_gfx10_program *program, const struct execute_state *state, const uint8_t *earlier, uint8_t **cpu, uint64_t *va);
static int execute_draw_end(struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record, const struct execute_tables *tables);
static int execute_draw_call(struct amdrdna_vk_execute *execute, const struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_pass_view(const struct amdrdna_vk_framebuffer *framebuffer, uint32_t attachment, const struct amdrdna_vk_view **view);
static bool execute_pass_targets(const struct execute_state *state);
static int execute_clear_quads(struct amdrdna_vk_execute *execute, struct execute_state *state, const uint32_t *numbers, const struct amdrdna_vk_clear_colours *colours, uint32_t cleared, bool clear_depth, bool clear_stencil, uint32_t depth, uint32_t stencil, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
static int execute_table(struct amdrdna_vk_execute *execute, const struct amdrdna_gfx10_program *program, bool clear, uint8_t **cpu, uint64_t *va);
static int execute_resources(const struct amdrdna_gfx10_program *program, const struct execute_state *state, uint8_t *table);
static int execute_buffer_slot(const struct amdrdna_vk_descriptor_slot *slot, uint32_t dynamic, uint32_t size, uint32_t *words);
static int execute_texture_slot(const struct amdrdna_vk_descriptor_slot *slot, uint32_t *image_words, uint32_t *sampler_words);
static int execute_texel_slot(const struct amdrdna_vk_descriptor_slot *slot, uint32_t *words);
static int execute_storage_images(const struct amdrdna_gfx10_program *program, const struct amdrdna_vk_descriptor_set *const *sets, uint8_t *table);
static int execute_region_slice(const struct amdrdna_vk_image *image, const VkImageSubresourceLayers *layers, int32_t z, struct amdrdna_vk_image *part);
static void execute_vertex_input(const struct amdrdna_vk_execute *execute, const struct amdrdna_vk_pipeline *pipeline, const struct execute_state *state, uint8_t *table, uint32_t *instance_mask);
static int execute_query_reset(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_timestamp(struct amdrdna_vk_execute *execute, const struct amdrdna_vk_record *record);
static void execute_occlusion(struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static void execute_depth_counts(struct amdrdna_vk_execute *execute, const struct execute_state *state);
static int execute_transfer_parts(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static size_t execute_region_bytes(uint32_t opcode);
static int execute_transfer(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_copy_buffers(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_copy_images(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_buffer_image(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_move_rows(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_image *image, uint64_t buffer_va, const VkBufferImageCopy *region, bool to_image);
static bool execute_texel_copy(const struct amdrdna_vk_record *record);
static int execute_copy_texels(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_texel_buffer(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record, const VkBufferImageCopy *region);
static int execute_texel_images(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record, const VkImageCopy *region);
static int execute_move(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, uint64_t source, uint64_t destination, uint64_t bytes);
static int execute_clear_image(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static bool execute_depth_copy(const struct amdrdna_vk_record *record);
static int execute_depth_images(struct amdrdna_vk_execute *execute, const struct amdrdna_vk_record *record);
static int execute_depth_buffer(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_record *record);
static int execute_depth_into(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_image *image, uint32_t aspect, uint64_t start, uint32_t pitch, const VkBufferImageCopy *region);
static int execute_depth_out(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state, const struct amdrdna_vk_image *image, uint32_t aspect, uint64_t start, uint32_t pitch, const VkBufferImageCopy *region);
static int execute_band_room(struct amdrdna_vk_session *session, struct amdrdna_vk_execute *execute, struct execute_state *state);
static int execute_clear_depth(struct amdrdna_vk_execute *execute, const struct amdrdna_vk_record *record);
static int execute_depth_target(const struct amdrdna_vk_image *image, struct amdrdna_vk_target *target);
static uint32_t execute_depth_program(const struct amdrdna_vk_image *image, uint32_t aspect);
static uint32_t execute_aspect_bytes(const struct amdrdna_vk_image *image, uint32_t aspect);
static int execute_quad(struct amdrdna_vk_execute *execute, const struct execute_quad *quad);
static int execute_image_target(const struct amdrdna_vk_image *image, uint32_t format, struct amdrdna_vk_target *target);
static int execute_slice_target(const struct amdrdna_vk_image *image, uint32_t format, uint32_t level, uint32_t slice, struct amdrdna_vk_target *target);
static int execute_resolve_images(struct amdrdna_vk_execute *execute, const struct amdrdna_vk_record *record);
static int execute_pass_resolves(struct amdrdna_vk_execute *execute, struct execute_state *state);
static int execute_pass_depth_resolve(struct amdrdna_vk_execute *execute, struct execute_state *state, int32_t x1, int32_t y1);
static int execute_resolve_quad(struct amdrdna_vk_execute *execute, const struct amdrdna_vk_image *source, uint32_t layer, int32_t source_x, int32_t source_y, const struct amdrdna_vk_target *target, int32_t x, int32_t y, uint32_t width, uint32_t height);
static bool execute_region_inside(int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t image_width, uint32_t image_height);
static void execute_full_viewport(uint32_t *viewport, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
static void execute_put32(uint8_t *cpu, uint32_t at, uint32_t value);
static void execute_put_words(uint8_t *cpu, uint32_t at, const uint32_t *words, uint32_t count);
static void execute_trial_compare(const uint8_t *cpu, uint32_t pitch, struct amdrdna_vk_trial *trial);
static uint32_t execute_get32(const uint8_t *cpu, uint32_t at);

/*
 * Runs an executable command buffer's records on the GPU and waits for
 * them.  Returns 0, the error of the End that left the command buffer
 * invalid, EINVAL for one not executable otherwise, or the error of the
 * run that failed (the GPU then did not finish).
 */
int
amdrdna_vk_execute(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_command_buffer *command)
{
	const struct amdrdna_vk_command_pool *pool;
	struct amdrdna_vk_execute *execute;
	struct execute_state *state;
	uint32_t index;
	int error;

	/*
	 * Only an executable command buffer runs.  One whose End failed is
	 * turned down with that End's error, so that the submission's result
	 * is the End's.
	 */
	if (command->state == AMDRDNA_VK_COMMAND_INVALID && command->error != 0)
		return command->error;
	if (command->state != AMDRDNA_VK_COMMAND_EXECUTABLE)
		return EINVAL;
	if (command->record_count == 0U)
		return 0;

	/* The session's run and programs. */
	error = execute_open(session, &execute);
	if (error != 0)
		return error;

	/* The state starts empty. */
	state = kern_calloc(1U, sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* The push constant records' bytes. */
	state->push_bytes = command->push_bytes;

	/* The queue of the command buffer's family: a compute queue for the compute family. */
	pool = command->pool;
	execute->kernel_queue = (pool != NULL && pool->family == AMDRDNA_VK_FAMILY_COMPUTE);

	/* Each record in order, the indirect buffer started over when it fills. */
	error = execute_start(session, execute);
	for (index = 0U; error == 0 && index < command->record_count; index++) {
		error = execute_room(session, execute, state);
		if (error != 0)
			break;
		error = execute_record(session, execute, state, &command->records[index]);
		if (error != 0)
			break;
	}

	/* The rest of the indirect buffer, put on its queue; the next buffer is the graphics queue's unless said. */
	if (error == 0)
		error = execute_flush(session, execute);
	kern_free(state);
	execute->kernel_queue = false;

	/* Reports a run that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the command buffer's runs are on their queue. */
	return 0;
}

/*
 * Gives the newest run a session's executor put on each queue (0 the
 * graphics queue, 1 the compute queue); no run when it put none.
 */
void
amdrdna_vk_execute_last(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_ticket *last)
{
	/* No executor yet, no run. */
	kern_memset(last, 0, 2U * sizeof(*last));
	if (session->execute == NULL)
		return;

	/* The two queues' newest runs. */
	kern_memcpy(last, session->execute->last, sizeof(session->execute->last));
}

/* Gives back a closing session's runs and programs, once the GPU has done what they hold. */
void
amdrdna_vk_execute_close(
	struct amdrdna_vk_session *session)
{
	struct amdrdna_vk_execute *execute;
	uint32_t index;

	execute = session->execute;
	if (execute == NULL)
		return;

	/* Each slot once its last run ended (a run that does not end keeps its memory with the node). */
	for (index = 0U; index < EXECUTE_SLOTS; index++) {
		if (execute->slots[index].token == NULL)
			continue;
		(void)amdrdna_vk_gpu_wait(session->owner, &execute->slots[index].ticket);
		amdrdna_vk_gpu_free(session->owner, execute->slots[index].token);
	}

	/* The compiled quad programs, then the record. */
	if (execute->program_token != NULL)
		amdrdna_vk_gpu_free(session->owner, execute->program_token);
	for (index = 0U; index < EXECUTE_QUAD_PROGRAMS; index++) {
		if (execute->quad[index].compiled != NULL)
			amdrdna_gfx10_program_free(execute->quad[index].compiled);
	}
	kern_free(execute);
	session->execute = NULL;
}

/*
 * Checks the executor's draws on the GPU (stage A9): in a run of its own,
 * a quad of one colour over the first image and another over a square
 * inside it, then, after a barrier, a quad that samples the first image
 * into the second; the indirect buffer is run and waited for, and every
 * pixel of both images is compared.  Returns 0 with the counts in trial
 * (wrong pixels are counted, not an error), or the error of the run or
 * of the memory.
 */
int
amdrdna_vk_execute_trial(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_trial *trial)
{
	static const uint32_t outside_colour[4] = { 0U, 0U, EXECUTE_FLOAT_ONE, EXECUTE_FLOAT_ONE };
	static const uint32_t square_colour[4] = { EXECUTE_FLOAT_ONE, 0U, 0U, EXECUTE_FLOAT_ONE };
	struct amdrdna_vk_execute *execute;
	struct execute_quad quad;
	void *token;
	uint8_t *cpu;
	uint64_t va;
	uint32_t pitch;
	uint32_t image_bytes;
	uint32_t at;
	int error;

	/* Nothing compared yet. */
	kern_memset(trial, 0, sizeof(*trial));
	trial->pixels = EXECUTE_TRIAL_SIDE * EXECUTE_TRIAL_SIDE;
	trial->bad_at = trial->pixels * 2U;

	/* The session's run and programs. */
	error = execute_open(session, &execute);
	if (error != 0)
		return error;

	/* The two images, every pixel a word the draws do not write. */
	pitch = amdrdna_vk_pitch(EXECUTE_TRIAL_SIDE);
	image_bytes = pitch * EXECUTE_TRIAL_SIDE;
	error = amdrdna_vk_gpu_alloc(session->owner, (uint64_t)image_bytes * 2U, &token, &cpu, &va);
	if (error != 0)
		return error;
	for (at = 0U; at < image_bytes * 2U; at += 4U)
		execute_put32(cpu, at, EXECUTE_TRIAL_UNDRAWN);

	/* The first image blue, then the red square. */
	error = execute_start(session, execute);
	if (error != 0) {
		amdrdna_vk_gpu_free(session->owner, token);
		return error;
	}
	kern_memset(&quad, 0, sizeof(quad));
	quad.target.address = va;
	quad.target.width = EXECUTE_TRIAL_SIDE;
	quad.target.height = EXECUTE_TRIAL_SIDE;
	quad.target.format = VK_FORMAT_B8G8R8A8_UNORM;
	quad.x1 = (int32_t)EXECUTE_TRIAL_SIDE;
	quad.y1 = (int32_t)EXECUTE_TRIAL_SIDE;
	kern_memcpy(quad.colour, outside_colour, sizeof(quad.colour));
	error = execute_quad(execute, &quad);
	if (error == 0) {
		quad.x0 = (int32_t)EXECUTE_TRIAL_INNER_FROM;
		quad.y0 = (int32_t)EXECUTE_TRIAL_INNER_FROM;
		quad.x1 = (int32_t)EXECUTE_TRIAL_INNER_TO;
		quad.y1 = (int32_t)EXECUTE_TRIAL_INNER_TO;
		kern_memcpy(quad.colour, square_colour, sizeof(quad.colour));
		error = execute_quad(execute, &quad);
	}

	/* After a barrier, the first image sampled pixel for pixel into the second. */
	if (error == 0) {
		execute_barrier(execute);
		quad.target.address = va + image_bytes;
		quad.x0 = 0;
		quad.y0 = 0;
		quad.x1 = (int32_t)EXECUTE_TRIAL_SIDE;
		quad.y1 = (int32_t)EXECUTE_TRIAL_SIDE;
		quad.textured = true;
		quad.source = va;
		quad.source_width = EXECUTE_TRIAL_SIDE;
		quad.source_height = EXECUTE_TRIAL_SIDE;
		quad.source_format = VK_FORMAT_B8G8R8A8_UNORM;
		quad.u1 = (int32_t)EXECUTE_TRIAL_SIDE;
		quad.v1 = (int32_t)EXECUTE_TRIAL_SIDE;
		quad.filter = VK_FILTER_NEAREST;
		error = execute_quad(execute, &quad);
	}

	/* The quads and the words that draw them, then the run and its wait. */
	trial->quads = execute->quads;
	trial->words = execute->pm4.count;
	if (error == 0)
		error = execute_flush(session, execute);

	/* The GPU has drawn them. */
	if (error == 0)
		error = amdrdna_vk_gpu_wait(session->owner, &execute->last[0]);

	/* Every pixel of both images compared. */
	if (error == 0)
		execute_trial_compare(cpu, pitch, trial);
	amdrdna_vk_gpu_free(session->owner, token);
	if (error != 0)
		return error;

	/* Succeeded: the counts are in trial. */
	return 0;
}

/*
 * Gives the session's runs and programs, made at its first submission:
 * the first slot and the executor's programs.
 */
static int
execute_open(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute **execute)
{
	struct amdrdna_vk_execute *opened;
	int error;

	/* Made once; programs that did not compile are tried again. */
	if (session->execute != NULL) {
		opened = session->execute;
		if (!opened->programs_ready) {
			error = execute_programs(session, opened);
			if (error != 0)
				return error;
		}
		*execute = opened;
		return 0;
	}

	/* The record and its first slot. */
	opened = kern_calloc(1U, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;
	error = execute_slot_make(session, &opened->slots[0]);
	if (error != 0) {
		kern_free(opened);
		return error;
	}

	/* The executor's programs. */
	session->execute = opened;
	error = execute_programs(session, opened);
	if (error != 0)
		return error;

	/* Succeeded: the run is the session's. */
	*execute = opened;
	return 0;
}

/*
 * Compiles the executor's quad programs (the corners placed by the push
 * constants' rectangle, the texture read at the push constants' texture
 * rectangle, the push constants' colour, the resolve, and the depth
 * copies' writing and packing of texels' bits) into a run of their own,
 * each on a 256-byte boundary.  The vertex program runs as the pipelines' do: a
 * primitive shader when the node says so, so that the draws never change
 * between the two kinds of vertex stage.
 */
static int
execute_programs(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute)
{
	struct amdrdna_gfx10_compile_options options;
	struct amdrdna_quad_ir *program;
	uint32_t geometry;
	uint32_t index;
	uint32_t at;
	uint32_t bytes;
	int error;

	/* The IR's storage. */
	program = kern_malloc(sizeof(*program));
	if (program == NULL)
		return ENOMEM;

	/* The vertex program a primitive shader when the node's pipelines' are. */
	kern_memset(&options, 0, sizeof(options));
	geometry = amdrdna_vk_geometry_mode(session->owner);
	if (geometry != 0U)
		options.primitive_shader = 1U;

	/* The programs (kept from a try that did not finish). */
	error = 0;
	if (execute->quad[EXECUTE_QUAD_VERTEX].compiled == NULL) {
		amdrdna_quad_vertex_ir(program, 1);
		error = execute_compile(program, &options, &execute->quad[EXECUTE_QUAD_VERTEX]);
	}
	if (error == 0 && execute->quad[EXECUTE_QUAD_TEXTURED].compiled == NULL) {
		amdrdna_quad_fragment_ir(program, 1);
		error = execute_compile(program, NULL, &execute->quad[EXECUTE_QUAD_TEXTURED]);
	}
	if (error == 0 && execute->quad[EXECUTE_QUAD_COLOUR].compiled == NULL) {
		amdrdna_quad_fragment_ir(program, 0);
		error = execute_compile(program, NULL, &execute->quad[EXECUTE_QUAD_COLOUR]);
	}
	if (error == 0 && execute->quad[EXECUTE_QUAD_RESOLVE].compiled == NULL) {
		amdrdna_quad_resolve_ir(program);
		error = execute_compile(program, NULL, &execute->quad[EXECUTE_QUAD_RESOLVE]);
	}
	for (index = EXECUTE_QUAD_DEPTH_32; error == 0 && index <= EXECUTE_QUAD_STENCIL; index++) {
		if (execute->quad[index].compiled != NULL)
			continue;
		amdrdna_quad_depth_ir(program, AMDRDNA_QUAD_DEPTH_32 + (index - EXECUTE_QUAD_DEPTH_32));
		error = execute_compile(program, NULL, &execute->quad[index]);
	}
	for (index = EXECUTE_QUAD_PACK_1; error == 0 && index <= EXECUTE_QUAD_PACK_4; index++) {
		if (execute->quad[index].compiled != NULL)
			continue;
		amdrdna_quad_pack_ir(program, 1U << (index - EXECUTE_QUAD_PACK_1));
		error = execute_compile(program, NULL, &execute->quad[index]);
	}
	for (index = EXECUTE_QUAD_COLOUR_WIDE; error == 0 && index < EXECUTE_QUAD_SAMPLE_ZERO; index++) {
		if (execute->quad[index].compiled != NULL)
			continue;
		kern_memset(&options, 0, sizeof(options));
		options.color_exports = AMDRDNA_GFX10_COLOUR_R + (index - EXECUTE_QUAD_COLOUR_WIDE);
		amdrdna_quad_fragment_ir(program, 0);
		error = execute_compile(program, &options, &execute->quad[index]);
	}
	for (index = EXECUTE_QUAD_SAMPLE_ZERO; error == 0 && index < EXECUTE_QUAD_PROGRAMS; index++) {
		if (execute->quad[index].compiled != NULL)
			continue;
		amdrdna_quad_depth_ir(program, (AMDRDNA_QUAD_DEPTH_32 + (index - EXECUTE_QUAD_SAMPLE_ZERO)) | AMDRDNA_QUAD_SAMPLE_ZERO);
		error = execute_compile(program, NULL, &execute->quad[index]);
	}
	kern_free(program);
	if (error != 0)
		return error;

	/* The run: each program's code rounded up to 256 bytes. */
	bytes = 0U;
	for (index = 0U; index < EXECUTE_QUAD_PROGRAMS; index++)
		bytes += (execute->quad[index].compiled->code_words * 4U + AMDRDNA_VK_RESOURCE_ALIGN - 1U) & ~(AMDRDNA_VK_RESOURCE_ALIGN - 1U);
	if (execute->program_token == NULL) {
		error = amdrdna_vk_gpu_alloc(session->owner, bytes, &execute->program_token, &execute->program_cpu, &execute->program_va);
		if (error != 0)
			return error;
	}

	/* Each program's code in its place. */
	at = 0U;
	for (index = 0U; index < EXECUTE_QUAD_PROGRAMS; index++) {
		kern_memcpy(execute->program_cpu + at, execute->quad[index].compiled->code, (size_t)execute->quad[index].compiled->code_words * 4U);
		execute->quad[index].va = execute->program_va + at;
		at += (execute->quad[index].compiled->code_words * 4U + AMDRDNA_VK_RESOURCE_ALIGN - 1U) & ~(AMDRDNA_VK_RESOURCE_ALIGN - 1U);
	}

	/* Succeeded: the programs are in place. */
	execute->programs_ready = true;
	return 0;
}

/* Compiles one quad program with the options (NULL for none) and its register values. */
static int
execute_compile(
	struct amdrdna_quad_ir *program,
	const struct amdrdna_gfx10_compile_options *options,
	struct amdrdna_vk_program *compiled)
{
	struct amdrdna_gfx10_diagnostic diagnostic;
	int error;

	/* Compiled for GFX10. */
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = amdrdna_gfx10_compile_ir(&program->ir, options, &compiled->compiled, &diagnostic);
	if (error != 0) {
		kern_logf("amdrdna: vk: a quad program did not compile (%d: %s)\n", error,
			  (diagnostic.reason != NULL) ? diagnostic.reason : "?");
		compiled->compiled = NULL;
		return error;
	}

	/* The values a draw writes for it. */
	amdrdna_gfx10_program_registers(compiled->compiled, &compiled->registers);

	/* Succeeded: the program is compiled. */
	return 0;
}

/*
 * Starts an indirect buffer in a slot whose run has ended: the current
 * slot, another one made earlier, a new one while one is to be had, or
 * else the current one once its run ends.  The buffer starts with the
 * state every draw shares and the caches emptied (vk-state.c); nothing of
 * the tables is used yet.  Returns 0, or the error of the slot's making or
 * of the wait (the run did not end).
 */
static int
execute_start(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute)
{
	struct execute_slot *slot;
	uint32_t index;
	uint32_t chosen;
	bool passed;
	int error;

	/* The current slot when its run ended, else another made one whose run ended. */
	chosen = EXECUTE_SLOTS;
	for (index = 0U; index < EXECUTE_SLOTS; index++) {
		slot = &execute->slots[(execute->slot + index) % EXECUTE_SLOTS];
		if (slot->token == NULL)
			continue;
		passed = amdrdna_vk_gpu_passed(session->owner, &slot->ticket);
		if (passed) {
			chosen = (execute->slot + index) % EXECUTE_SLOTS;
			break;
		}
	}

	/* Else a new slot, while one is to be had. */
	for (index = 0U; chosen == EXECUTE_SLOTS && index < EXECUTE_SLOTS; index++) {
		if (execute->slots[index].token != NULL)
			continue;
		error = execute_slot_make(session, &execute->slots[index]);
		if (error == 0)
			chosen = index;
		break;
	}

	/* Else the first made slot from the current one on (the first slot is always made), once its run ended. */
	if (chosen == EXECUTE_SLOTS) {
		chosen = execute->slot;
		while (execute->slots[chosen].token == NULL)
			chosen = (chosen + 1U) % EXECUTE_SLOTS;
		error = amdrdna_vk_gpu_wait(session->owner, &execute->slots[chosen].ticket);
		if (error != 0)
			return error;
	}

	/* The slot is the one written now; its run is over. */
	slot = &execute->slots[chosen];
	execute->slot = chosen;
	execute->token = slot->token;
	execute->cpu = slot->cpu;
	execute->va = slot->va;
	kern_memset(&slot->ticket, 0, sizeof(slot->ticket));

	/* The buffer over its part of the slot. */
	amdrdna_vk_pm4_start(&execute->pm4, (uint32_t *)(void *)(execute->cpu + EXECUTE_BUFFER_AT), EXECUTE_BUFFER_WORDS);
	if (execute->kernel_queue)
		amdrdna_vk_state_start_kernel(&execute->pm4);
	else
		amdrdna_vk_state_start(&execute->pm4);

	/* Nothing of the tables is used. */
	execute->tables = 0U;
	execute->draws = 0U;
	execute->quads = 0U;

	/* Succeeded: the buffer is started. */
	return 0;
}

/*
 * Makes a slot: a run of system memory with its zero page, the barrier's
 * word and the quad's corners written once.  Returns 0 or the node's
 * error.
 */
static int
execute_slot_make(
	struct amdrdna_vk_session *session,
	struct execute_slot *slot)
{
	static const uint32_t corners[EXECUTE_CORNERS * 2U] = {
		0U, 0U,
		EXECUTE_FLOAT_ONE, 0U,
		0U, EXECUTE_FLOAT_ONE,
		EXECUTE_FLOAT_ONE, 0U,
		EXECUTE_FLOAT_ONE, EXECUTE_FLOAT_ONE,
		0U, EXECUTE_FLOAT_ONE
	};
	int error;

	/* The run. */
	error = amdrdna_vk_gpu_alloc_system(session->owner, EXECUTE_RUN_BYTES, &slot->token, &slot->cpu, &slot->va);
	if (error != 0) {
		slot->token = NULL;
		return error;
	}

	/* The zero page, the barrier's word, and the corners: (0,0) (1,0) (0,1), (1,0) (1,1) (0,1). */
	kern_memset(slot->cpu + EXECUTE_ZERO_AT, 0, EXECUTE_ZERO_BYTES);
	execute_put32(slot->cpu, EXECUTE_SYNC_AT, 0U);
	execute_put_words(slot->cpu, EXECUTE_CORNERS_AT, corners, EXECUTE_CORNERS * 2U);

	/* No run of it yet. */
	kern_memset(&slot->ticket, 0, sizeof(slot->ticket));

	/* Succeeded: the slot is made. */
	return 0;
}

/*
 * Puts the indirect buffer written so far on its queue, after a barrier
 * that leaves everything it wrote in memory, without waiting for it; the
 * run is the slot's and the queue's newest, and the next slot is the
 * current one (execute_start() starts the next buffer).  Returns the
 * error of the run's putting.
 */
static int
execute_flush(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute)
{
	int error;

	/* The barrier at the end, then the buffer on its queue. */
	execute_barrier(execute);
	error = execute_put(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the run is on its queue. */
	return 0;
}

/*
 * Puts the indirect buffer written so far on its queue as it is, padded,
 * without waiting for it: the run is the slot's and the queue's newest,
 * and the next slot is the current one.  A buffer that fills in the middle
 * of a command buffer goes without a barrier, as the draws of one buffer
 * do: the ring's fence after the run (an end-of-pipe event that writes
 * the level-2 cache back) is what frees the slot, the run after it starts
 * by emptying the caches, and the draws that follow keep the order of
 * draws in one pass.  Returns the error of the run's putting.
 */
static int
execute_put(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute)
{
	struct amdrdna_vk_ticket ticket;
	int error;

	/* The buffer padded. */
	amdrdna_vk_pm4_pad(&execute->pm4, EXECUTE_BUFFER_PAD);
	if (execute->pm4.full)
		return ENOSPC;

	/* Counts its draws, then puts it on the queue. */
	amdrdna_vk_gpu_drawn(session->owner, execute->draws, execute->quads);
	error = amdrdna_vk_gpu_submit(session->owner, execute->va + EXECUTE_BUFFER_AT, execute->pm4.count * 4U, execute->kernel_queue, &ticket);
	if (error != 0)
		return error;

	/* The slot's run, and its queue's newest. */
	execute->slots[execute->slot].ticket = ticket;
	execute->last[ticket.queue] = ticket;

	/* The next buffer goes in the next slot, unless another one's run ends first. */
	execute->slot = (execute->slot + 1U) % EXECUTE_SLOTS;

	/* Succeeded: the run is on its queue. */
	return 0;
}

/*
 * Makes room for one record: runs the indirect buffer when the record
 * could pass its end or the tables' part could not take its tables.  The
 * target is written again in the next buffer.
 */
static int
execute_room(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state)
{
	bool full;
	int error;

	/* Full when the buffer or the tables could not take a record. */
	full = false;
	if (execute->pm4.count + EXECUTE_RECORD_WORDS > EXECUTE_BUFFER_WORDS)
		full = true;
	if (execute->tables + EXECUTE_RECORD_TABLES > EXECUTE_TABLES_BYTES)
		full = true;
	if (!full)
		return 0;

	/* Puts what there is on the queue, without a barrier, and starts the next buffer. */
	error = execute_put(session, execute);
	if (error == 0)
		error = execute_start(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the next buffer has room; its target is written anew. */
	state->target_written = false;
	return 0;
}

/* Writes a barrier with the next value of the session's barrier word. */
static void
execute_barrier(
	struct amdrdna_vk_execute *execute)
{
	/* A value the word has not held since the last run began; the compute queue's own form there. */
	execute->sync++;
	if (execute->kernel_queue)
		amdrdna_vk_state_barrier_kernel(&execute->pm4, execute->va + EXECUTE_SYNC_AT, execute->sync);
	else
		amdrdna_vk_state_barrier(&execute->pm4, execute->va + EXECUTE_SYNC_AT, execute->sync);
}

/* Runs one record. */
static int
execute_record(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_query_pool *pool;
	int error;

	/* What the compute bind point binds is kept apart from the graphics state. */
	if (execute_kernel_binds(record)) {
		execute_bind_kernel(state, record);
		return 0;
	}

	/*
	 * A compute queue runs no graphics work; the copies the command
	 * processor's moves make (those of images whose texels are not four
	 * bytes, as the planes of a video picture) are transfer work it runs.
	 */
	if (execute->kernel_queue && execute_graphics_only(record->opcode) && !execute_texel_copy(record)) {
		kern_logf("amdrdna: vk: command %u is not run on the compute queue\n", (unsigned)record->opcode);
		return ENOTSUP;
	}

	/* The record's command. */
	error = 0;
	switch (record->opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
	case GPU_OP_CMD_SET_VIEWPORT:
	case GPU_OP_CMD_SET_SCISSOR:
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
	case GPU_OP_CMD_PUSH_CONSTANTS:
		execute_bind(state, record);
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		execute_bind_dynamic(state, record);
		break;
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
		error = execute_views(execute, state, record);
		break;
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		error = execute_begin_pass(session, execute, state, record);
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
		state->in_pass = false;
		execute_barrier(execute);
		error = execute_pass_resolves(execute, state);
		break;
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
		error = execute_views(execute, state, record);
		break;
	case GPU_OP_CMD_DISPATCH:
		error = execute_dispatch(execute, state, record);
		break;
	case GPU_OP_CMD_DRAW_INDIRECT:
	case GPU_OP_CMD_DRAW_INDEXED_INDIRECT:
	case GPU_OP_CMD_DISPATCH_INDIRECT:
		error = execute_indirect(session, execute, state, record);
		break;
	case GPU_OP_CMD_FILL_BUFFER:
	case GPU_OP_CMD_UPDATE_BUFFER:
		error = execute_buffer_write(session, execute, state, record);
		break;
	case GPU_OP_CMD_COPY_QUERY_POOL_RESULTS:
		error = execute_query_copy(session, execute, state, record);
		break;
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
	case GPU_OP_CMD_RESOLVE_IMAGE:
		error = execute_transfer_parts(session, execute, state, record);
		break;
	case GPU_OP_CMD_RESET_QUERY_POOL:
		error = execute_query_reset(session, execute, state, record);
		break;
	case GPU_OP_CMD_WRITE_TIMESTAMP:
		error = execute_timestamp(execute, record);
		break;
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
		pool = record->objects[0]->payload;
		if (pool->type == VK_QUERY_TYPE_OCCLUSION) {
			execute_occlusion(execute, state, record);
		} else {
			error = execute_video(session, execute, state, record);
		}
		break;
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
	case GPU_OP_CMD_END_VIDEO_CODING:
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
	case GPU_OP_CMD_DECODE_VIDEO:
		error = execute_video(session, execute, state, record);
		break;
	default:
		break;
	}

	/* Reports a record that could not run, or a buffer that overflowed. */
	if (error != 0)
		return error;
	if (execute->pm4.full)
		return ENOSPC;

	/* Succeeded: the record ran (or is bound for the draws after it). */
	return 0;
}

/* Keeps what a binding record binds for the draws after it. */
static void
execute_bind(
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const uint8_t *pushed;
	uint32_t index;
	int compared;

	/* The state each command binds. */
	switch (record->opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
		state->pipeline = record->objects[0]->payload;
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
		kern_memcpy(state->viewport, record->values, sizeof(state->viewport));
		state->viewport_set = true;
		break;
	case GPU_OP_CMD_SET_SCISSOR:
		state->scissor = record->rect;
		state->scissor_set = true;
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		execute_bind_sets(record, state->sets, state->dynamic, &state->sets_changed);
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		for (index = 0U; index < record->object_count; index++) {
			if (state->buffers[record->first + index] != record->objects[index]->payload ||
			    state->offsets[record->first + index] != record->offsets[index]) {
				state->buffers[record->first + index] = record->objects[index]->payload;
				state->offsets[record->first + index] = record->offsets[index];
				state->buffers_changed = true;
			}
		}
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		state->index_buffer = record->objects[0]->payload;
		state->index_offset = record->offsets[0];
		state->index_type = record->values[0];
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		pushed = state->push_bytes + record->push_at;
		compared = kern_memcmp(state->push + record->first, pushed, record->count);
		if (compared == 0)
			break;

		/* The first byte that changes, then the bytes. */
		for (index = 0U; index < record->count; index++) {
			if (state->push[record->first + index] != pushed[index])
				break;
		}

		if (record->first + index < state->push_changed_from)
			state->push_changed_from = record->first + index;
		kern_memcpy(state->push + record->first, pushed, record->count);
		break;
	default:
		break;
	}
}

/* Reports whether a record binds at the compute bind point (its value 0). */
static bool
execute_kernel_binds(
	const struct amdrdna_vk_record *record)
{
	/* A pipeline or sets, at the compute bind point. */
	if (record->opcode != GPU_OP_CMD_BIND_PIPELINE && record->opcode != GPU_OP_CMD_BIND_DESCRIPTOR_SETS)
		return false;
	return record->values[0] == VK_PIPELINE_BIND_POINT_COMPUTE;
}

/* Keeps what a record binds at the compute bind point for the dispatches after it. */
static void
execute_bind_kernel(
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	uint32_t changed;

	/* The compute pipeline, or the sets from the first with their dynamic offsets. */
	if (record->opcode == GPU_OP_CMD_BIND_PIPELINE) {
		state->kernel_pipeline = record->objects[0]->payload;
		return;
	}
	changed = 0U;
	execute_bind_sets(record, state->kernel_sets, state->kernel_dynamic, &changed);
}

/*
 * Keeps the sets a bind binds from its first set on, and each one's
 * dynamic offsets in order (each dynamic binding takes as many of the
 * bind's offsets as it has descriptors, in the order of the sets and their
 * binding numbers; execute_dynamic() finds an element's).  A null set
 * leaves its slot as it was.  A bit of changed is set for each set number
 * whose set or offsets differ from what was bound.
 */
static void
execute_bind_sets(
	const struct amdrdna_vk_record *record,
	const struct amdrdna_vk_descriptor_set **sets,
	uint32_t (*dynamic)[AMDRDNA_VK_DYNAMIC_MAX],
	uint32_t *changed)
{
	const struct amdrdna_vk_descriptor_set *set;
	uint32_t offsets[AMDRDNA_VK_DYNAMIC_MAX];
	uint32_t number;
	uint32_t index;
	uint32_t binding;
	uint32_t taken;
	uint32_t next;
	int compared;

	/* Each set held, its dynamic offsets taken in order. */
	next = 0U;
	for (index = 0U; index < record->object_count; index++) {
		if (record->objects[index] == NULL)
			continue;
		set = record->objects[index]->payload;
		number = record->first + index;

		/* The set's offsets, the rest none. */
		kern_memset(offsets, 0, sizeof(offsets));
		taken = 0U;
		for (binding = 0U; binding < AMDRDNA_VK_BINDINGS_MAX; binding++)
			taken += set->dynamic[binding];
		for (binding = 0U; binding < taken && binding < AMDRDNA_VK_DYNAMIC_MAX; binding++) {
			if (next + binding < record->values[1] && next + binding < AMDRDNA_VK_DYNAMIC_MAX)
				offsets[binding] = record->dynamic[next + binding];
		}
		next += taken;

		/* What changed. */
		compared = kern_memcmp(dynamic[number], offsets, sizeof(offsets));
		if (sets[number] != set || compared != 0)
			*changed |= 1U << number;
		sets[number] = set;
		kern_memcpy(dynamic[number], offsets, sizeof(offsets));
	}
}

/*
 * Reports the dynamic offset of a bound set's binding's element: its
 * place among the set's offsets bound with it (offsets), or 0 for an
 * element that takes none.
 */
static uint32_t
execute_dynamic(
	const struct amdrdna_vk_descriptor_set *set,
	const uint32_t *offsets,
	uint32_t binding,
	uint32_t element)
{
	uint32_t place;

	/* A dynamic binding's element. */
	if (binding >= AMDRDNA_VK_BINDINGS_MAX || element >= set->dynamic[binding])
		return 0U;
	place = (uint32_t)set->dynamic_first[binding] + element;
	if (place >= AMDRDNA_VK_DYNAMIC_MAX)
		return 0U;

	/* Succeeded: its offset. */
	return offsets[place];
}

/*
 * Keeps a dynamic state a command buffer sets for the draws after it: the
 * depth bias, the blend constants, or a stencil value of the faces it
 * names.  dynamic_changed is set when it differs from what was set.
 */
static void
execute_bind_dynamic(
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	int compared;

	/* The depth bias's three factors, or the four constants, or a stencil value. */
	switch (record->opcode) {
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		compared = kern_memcmp(state->bias, record->values, sizeof(state->bias));
		if (compared != 0 || !state->bias_set)
			state->dynamic_changed = true;
		kern_memcpy(state->bias, record->values, sizeof(state->bias));
		state->bias_set = true;
		break;
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		compared = kern_memcmp(state->constants, record->values, sizeof(state->constants));
		if (compared != 0 || !state->constants_set)
			state->dynamic_changed = true;
		kern_memcpy(state->constants, record->values, sizeof(state->constants));
		state->constants_set = true;
		break;
	default:
		execute_bind_stencil(state, record);
		break;
	}
}

/* Keeps a stencil compare mask, write mask or reference (value 1) of the faces a record names (value 0). */
static void
execute_bind_stencil(
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	uint32_t kind;
	uint32_t face;
	uint32_t bit;

	/* Which value the command sets. */
	kind = EXECUTE_STENCIL_REFERENCE;
	if (record->opcode == GPU_OP_CMD_SET_STENCIL_COMPARE_MASK)
		kind = EXECUTE_STENCIL_COMPARE;
	if (record->opcode == GPU_OP_CMD_SET_STENCIL_WRITE_MASK)
		kind = EXECUTE_STENCIL_WRITE;

	/* Each face named: the front (bit 0) and the back (bit 1). */
	for (face = 0U; face < 2U; face++) {
		if ((record->values[0] & (1U << face)) == 0U)
			continue;
		bit = 1U << (face * EXECUTE_STENCIL_VALUES + kind);
		if (state->stencil[face][kind] != record->values[1] || (state->stencil_set & bit) == 0U)
			state->dynamic_changed = true;
		state->stencil[face][kind] = record->values[1];
		state->stencil_set |= bit;
	}
}

/* Reports whether a command draws or is drawn (a pass, a draw, a transfer), which a compute queue cannot run. */
static bool
execute_graphics_only(
	uint32_t opcode)
{
	/* The commands the graphics queue's draws run. */
	switch (opcode) {
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
	case GPU_OP_CMD_END_RENDER_PASS:
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
	case GPU_OP_CMD_DRAW_INDIRECT:
	case GPU_OP_CMD_DRAW_INDEXED_INDIRECT:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
	case GPU_OP_CMD_RESOLVE_IMAGE:
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
	case GPU_OP_CMD_END_VIDEO_CODING:
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
	case GPU_OP_CMD_DECODE_VIDEO:
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
		return true;
	default:
		return false;
	}
}

/*
 * Runs a video or query record (vk-video.inc).  A decode reads what the
 * GPU's earlier work wrote and writes what later work reads, and the video
 * engine does not see the GPU's caches: the indirect buffer so far goes on
 * its queue with the barrier that writes the caches back, the session's
 * newest runs on both queues are waited for, the decode runs to its end,
 * and the work after it starts a new buffer, whose start drops the caches
 * (so it reads what the engine wrote).  Returns 0, or the error of a run
 * that did not end.
 */
static int
execute_video(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	uint64_t started_us;
	uint64_t wait_us;
	bool waits;
	int error;

	/* Only a decode waits for the GPU's earlier work. */
	waits = amdrdna_vk_video_waits(record);
	wait_us = 0U;
	if (waits) {
		/* The work so far on its queue, then both queues' newest runs ended, timed. */
		started_us = amdrdna_vk_clock_us(session->owner);
		error = execute_flush(session, execute);
		if (error == 0)
			error = amdrdna_vk_gpu_wait(session->owner, &execute->last[0]);
		if (error == 0)
			error = amdrdna_vk_gpu_wait(session->owner, &execute->last[1]);
		if (error != 0)
			return error;
		wait_us = amdrdna_vk_clock_us(session->owner) - started_us;
	}

	/* The record's command, the decode run to its end. */
	error = amdrdna_vk_video_run(session, record, &state->video_query, wait_us);
	if (error != 0)
		return error;

	/* The work after a decode goes in a new buffer, its target written anew. */
	if (waits) {
		error = execute_start(session, execute);
		if (error != 0)
			return error;
		state->target_written = false;
	}

	/* Succeeded: the record ran. */
	return 0;
}

/*
 * Runs a dispatch: the workgroup counts where the kernel reads them, the
 * kernel's resource table from the sets bound at the compute bind point
 * and the push constants, the kernel, the workgroups, then a barrier so
 * the next command reads what it wrote.
 */
static int
execute_dispatch(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_pipeline *pipeline;
	uint8_t *groups;
	uint64_t groups_va;
	uint8_t *table;
	uint64_t table_va;
	int error;

	/* A dispatch with a compute pipeline; no workgroup is nothing to run. */
	pipeline = state->kernel_pipeline;
	if (pipeline == NULL)
		return EINVAL;
	if (record->values[0] == 0U || record->values[1] == 0U || record->values[2] == 0U)
		return 0;

	/* The three counts and the first workgroup (DRV_GPU_SPIRV_SYSTEM_BASE_BYTES), then the table. */
	error = execute_part(execute, DRV_GPU_SPIRV_SYSTEM_BASE_BYTES, &groups, &groups_va);
	if (error != 0)
		return error;
	execute_put_words(groups, 0U, record->values, 6U);
	error = execute_table(execute, pipeline->kernel.compiled, true, &table, &table_va);
	if (error == 0)
		error = execute_kernel_resources(pipeline->kernel.compiled, state, groups_va, table);
	if (error != 0)
		return error;

	/* The kernel with its table, the workgroups, and the barrier after them. */
	amdrdna_vk_state_kernel(&execute->pm4, &pipeline->kernel, table_va);
	amdrdna_vk_pm4_dispatch(&execute->pm4, record->values[0], record->values[1], record->values[2]);
	execute_barrier(execute);

	/* Succeeded: the dispatch is written. */
	return 0;
}

/*
 * Puts the indirect buffer so far on its queue after a barrier that
 * leaves everything it wrote in memory, and waits until the newest runs
 * of both queues have ended.  The caller starts the next buffer
 * (execute_start), whose target is written anew.  Returns the error of
 * the putting or of a run that did not end.
 */
static int
execute_wait(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute)
{
	int error;

	/* The work so far on its queue, then both queues' newest runs ended. */
	error = execute_flush(session, execute);
	if (error == 0)
		error = amdrdna_vk_gpu_wait(session->owner, &execute->last[0]);
	if (error == 0)
		error = amdrdna_vk_gpu_wait(session->owner, &execute->last[1]);
	if (error != 0)
		return error;

	/* Succeeded: the GPU has done the work before. */
	return 0;
}

/*
 * Reads an indirect command's record (record->count bytes of its buffer,
 * object 0, at offsets[0]) into values once the work before has written
 * it: a move of the bytes into a part of the tables (through the level-2
 * cache, so memory outside the CPU's window is read the same way), the
 * buffer so far run to its end, the bytes copied out, and the next buffer
 * started (its target written anew).
 */
static int
execute_read_back(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record,
	uint32_t *values)
{
	uint8_t *staged;
	uint64_t staged_va;
	uint64_t va;
	uint8_t *cpu;
	int error;

	/* The buffer's address and a part to move the record into. */
	error = amdrdna_vk_buffer_address(record->objects[0]->payload, &va, &cpu);
	if (error == 0)
		error = execute_part(execute, record->count, &staged, &staged_va);
	if (error != 0)
		return error;

	/* What wrote the record has finished; the move; the run to its end. */
	execute_barrier(execute);
	amdrdna_vk_pm4_move(&execute->pm4, va + record->offsets[0], staged_va, record->count);
	if (execute->pm4.full)
		return ENOSPC;
	error = execute_wait(session, execute);
	if (error != 0)
		return error;

	/* The record, then the next buffer. */
	kern_memcpy(values, staged, record->count);
	error = execute_start(session, execute);
	if (error != 0)
		return error;
	state->target_written = false;

	/* Succeeded: the record's counts. */
	return 0;
}

/*
 * Runs vkCmdDrawIndirect, vkCmdDrawIndexedIndirect or
 * vkCmdDispatchIndirect: the counts read from the buffer once the work
 * before has written them (execute_read_back), then the draw or the
 * dispatch they give, as the direct command runs.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_CmdDrawIndirect, radv_CmdDispatchIndirect: the record's counts
 * give one draw's or dispatch's parameters, the command processor reading
 * them; here the CPU reads them once the work before is done).
 */
static int
execute_indirect(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	struct amdrdna_vk_record direct;
	uint32_t values[5];
	int error;

	/* A draw in a pass with a pipeline, a dispatch with a compute pipeline; a draw of no record draws nothing. */
	if (record->opcode == GPU_OP_CMD_DISPATCH_INDIRECT) {
		if (state->kernel_pipeline == NULL)
			return EINVAL;
	} else if (!state->in_pass || state->pipeline == NULL) {
		return EINVAL;
	}
	if (record->values[0] == 0U)
		return 0;

	/* The record's counts. */
	kern_memset(values, 0, sizeof(values));
	error = execute_read_back(session, execute, state, record, values);
	if (error != 0)
		return error;

	/* The direct command of them (a dispatch's first workgroup 0). */
	kern_memset(&direct, 0, sizeof(direct));
	kern_memcpy(direct.values, values, sizeof(values));
	if (record->opcode == GPU_OP_CMD_DISPATCH_INDIRECT) {
		direct.opcode = GPU_OP_CMD_DISPATCH;
		direct.values[3] = 0U;
		direct.values[4] = 0U;
		return execute_dispatch(execute, state, &direct);
	}
	direct.opcode = GPU_OP_CMD_DRAW;
	if (record->opcode == GPU_OP_CMD_DRAW_INDEXED_INDIRECT)
		direct.opcode = GPU_OP_CMD_DRAW_INDEXED;
	return execute_views(execute, state, &direct);
}

/*
 * Runs vkCmdFillBuffer or vkCmdUpdateBuffer after what the work before
 * wrote: a fill by the command processor's fills of the word (at most
 * AMDRDNA_VK_MOVE_MAX bytes each); an update's bytes put in a part of the
 * tables and moved from there, in one buffer with room for them (the
 * buffer so far run first when it has none); then the barrier after it.
 */
static int
execute_buffer_write(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	uint8_t *staged;
	uint64_t staged_va;
	uint64_t address;
	uint64_t bytes;
	uint64_t va;
	uint8_t *cpu;
	uint32_t step;
	int error;

	/* Where the bytes go. */
	error = amdrdna_vk_buffer_address(record->objects[0]->payload, &va, &cpu);
	if (error != 0)
		return error;
	address = va + record->offsets[0];
	bytes = record->offsets[1];

	/* An update's bytes in one buffer with room for them. */
	staged = NULL;
	staged_va = 0U;
	if (record->opcode == GPU_OP_CMD_UPDATE_BUFFER) {
		if (execute->tables + bytes + EXECUTE_TABLE_ALIGN > EXECUTE_TABLES_BYTES ||
		    execute->pm4.count + EXECUTE_RECORD_WORDS > EXECUTE_BUFFER_WORDS) {
			error = execute_flush(session, execute);
			if (error == 0)
				error = execute_start(session, execute);
			if (error != 0)
				return error;
		}
		error = execute_part(execute, (uint32_t)bytes, &staged, &staged_va);
		if (error != 0)
			return error;
		kern_memcpy(staged, record->regions, (size_t)bytes);
	}

	/* What the work before wrote has finished. */
	execute_barrier(execute);

	/* The update's move, or the fills, a buffer near its end run first. */
	if (record->opcode == GPU_OP_CMD_UPDATE_BUFFER) {
		amdrdna_vk_pm4_move(&execute->pm4, staged_va, address, (uint32_t)bytes);
	} else {
		while (bytes != 0U) {
			if (execute->pm4.count + AMDRDNA_PM4_MOVE_WORDS > EXECUTE_BUFFER_WORDS - EXECUTE_RECORD_WORDS) {
				error = execute_flush(session, execute);
				if (error == 0)
					error = execute_start(session, execute);
				if (error != 0)
					return error;
			}
			step = AMDRDNA_VK_MOVE_MAX & ~3U;
			if (bytes < step)
				step = (uint32_t)bytes;
			amdrdna_vk_pm4_fill(&execute->pm4, address, record->values[0], step);
			address += step;
			bytes -= step;
		}
	}

	/* What follows reads what it wrote; the target is written anew. */
	execute_barrier(execute);
	state->target_written = false;
	if (execute->pm4.full)
		return ENOSPC;

	/* Succeeded: the write is in the buffer. */
	return 0;
}

/*
 * Runs vkCmdCopyQueryPoolResults: once the work before has run to its end
 * (which ends the queries it ended), each query's record as
 * vkGetQueryPoolResults lays it out (amdrdna_vk_query_record) is put in a
 * part of the tables and moved to its place in the buffer: the result when
 * the query is available or partial results are asked for, and the
 * availability word when asked for; then the barrier after it.
 *
 * XXX A query not available when the copy runs is not waited for, even
 * with VK_QUERY_RESULT_WAIT_BIT: the work before it has ended, so such a
 * query is one no submitted work ends.
 *
 * Procedure referenced: the Vulkan 1.0 specification
 * (vkCmdCopyQueryPoolResults: without VK_QUERY_RESULT_PARTIAL_BIT the
 * result of an unavailable query is not written, the availability word
 * is) and Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_CmdCopyQueryPoolResults).
 */
static int
execute_query_copy(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_query_pool *pool;
	uint8_t *staged;
	uint64_t staged_va;
	uint64_t address;
	uint64_t value;
	uint64_t va;
	uint8_t *cpu;
	uint8_t *at;
	uint32_t available;
	uint32_t flags;
	uint32_t width;
	uint32_t bytes;
	uint32_t index;
	int error;

	/* No query, nothing to copy. */
	if (record->count == 0U)
		return 0;
	pool = record->objects[0]->payload;
	flags = record->values[0];
	width = 4U;
	if ((flags & VK_QUERY_RESULT_64_BIT) != 0U)
		width = 8U;

	/* The work before run to its end; the next buffer. */
	error = execute_wait(session, execute);
	if (error == 0)
		error = execute_start(session, execute);
	if (error != 0)
		return error;
	state->target_written = false;

	/* The buffer's address and a part for the records (16 bytes at most each). */
	error = amdrdna_vk_buffer_address(record->objects[1]->payload, &va, &cpu);
	if (error == 0)
		error = execute_part(execute, record->count * 16U, &staged, &staged_va);
	if (error != 0)
		return error;

	/* Each query's record, and the moves of what is written of it. */
	for (index = 0U; index < record->count; index++) {
		amdrdna_vk_query_value(pool, record->first + index, &value, &available);
		at = staged + (size_t)index * 16U;
		bytes = amdrdna_vk_query_record(value, available, flags, at);
		address = va + record->offsets[0] + (uint64_t)index * record->offsets[1];
		if (available != 0U || (flags & VK_QUERY_RESULT_PARTIAL_BIT) != 0U)
			amdrdna_vk_pm4_move(&execute->pm4, staged_va + (uint64_t)index * 16U, address, bytes);
		else if ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0U)
			amdrdna_vk_pm4_move(&execute->pm4, staged_va + (uint64_t)index * 16U + width, address + width, width);
	}

	/* What follows reads what it wrote. */
	execute_barrier(execute);
	if (execute->pm4.full)
		return ENOSPC;

	/* Succeeded: the records are moved. */
	return 0;
}

/*
 * Writes a kernel's resource table as execute_resources() writes a draw
 * program's, from the sets bound at the compute bind point; the storage
 * buffer of the system set (the compiler's for the workgroup counts) reads
 * the three counts at groups_va and the dispatch's first workgroup after
 * them.  A binding with nothing bound refuses the dispatch.
 */
static int
execute_kernel_resources(
	const struct amdrdna_gfx10_program *program,
	const struct execute_state *state,
	uint64_t groups_va,
	uint8_t *table)
{
	const struct amdrdna_vk_descriptor_set *set;
	const struct amdrdna_gfx10_binding *binding;
	uint32_t image_words[AMDRDNA_IMAGE_WORDS];
	uint32_t sampler_words[AMDRDNA_SAMPLER_WORDS];
	uint32_t words[4];
	uint32_t index;
	int error;

	/* The push constants the kernel reads. */
	kern_memcpy(table, state->push, program->push_bytes);

	/* The storage buffers: the workgroup counts, or a binding's whole range (from its dynamic offset). */
	for (index = 0U; index < program->storage_used; index++) {
		binding = &program->storage[index];
		if (binding->set == DRV_GPU_IR_SYSTEM_SET) {
			amdrdna_gfx10_raw_buffer_descriptor(groups_va, DRV_GPU_SPIRV_SYSTEM_BASE_BYTES, words);
		} else {
			set = state->kernel_sets[binding->set];
			if (set == NULL)
				return EINVAL;
			error = execute_buffer_slot(amdrdna_vk_set_slot(set, binding->binding, binding->element),
						    execute_dynamic(set, state->kernel_dynamic[binding->set], binding->binding, binding->element), 0U, words);
			if (error != 0)
				return error;
		}
		execute_put_words(table, binding->table_offset, words, 4U);
	}

	/* The uniform buffers: the bytes the kernel reads, within the range (from its dynamic offset). */
	for (index = 0U; index < program->uniform_used; index++) {
		binding = &program->uniforms[index];
		set = state->kernel_sets[binding->set];
		if (set == NULL)
			return EINVAL;
		error = execute_buffer_slot(amdrdna_vk_set_slot(set, binding->binding, binding->element),
					    execute_dynamic(set, state->kernel_dynamic[binding->set], binding->binding, binding->element), binding->size, words);
		if (error != 0)
			return error;
		execute_put_words(table, binding->table_offset, words, 4U);
	}

	/* The sampled images: the image descriptor, then the sampler's; a texel buffer's buffer descriptor in the image's place. */
	for (index = 0U; index < program->texture_used; index++) {
		binding = &program->textures[index];
		set = state->kernel_sets[binding->set];
		if (set == NULL)
			return EINVAL;
		if (binding->texel_buffer != 0U) {
			error = execute_texel_slot(amdrdna_vk_set_slot(set, binding->binding, binding->element), words);
			if (error != 0)
				return error;
			execute_put_words(table, binding->table_offset, words, 4U);
			continue;
		}
		error = execute_texture_slot(amdrdna_vk_set_slot(set, binding->binding, binding->element), image_words, sampler_words);
		if (error != 0)
			return error;
		execute_put_words(table, binding->table_offset, image_words, AMDRDNA_IMAGE_WORDS);
		execute_put_words(table, binding->table_offset + AMDRDNA_IMAGE_WORDS * 4U, sampler_words, AMDRDNA_SAMPLER_WORDS);
	}

	/* The storage images and storage texel buffers. */
	error = execute_storage_images(program, state->kernel_sets, table);
	if (error != 0)
		return error;

	/* Succeeded: the table holds every resource. */
	return 0;
}

/* Gives bytes of the run's tables' part on the table boundary, cleared. */
static int
execute_part(
	struct amdrdna_vk_execute *execute,
	uint32_t bytes,
	uint8_t **cpu,
	uint64_t *va)
{
	uint32_t rounded;

	/* The bytes on the boundary, within the part. */
	rounded = (bytes + EXECUTE_TABLE_ALIGN - 1U) & ~(EXECUTE_TABLE_ALIGN - 1U);
	if (execute->tables + rounded > EXECUTE_TABLES_BYTES)
		return ENOSPC;

	/* The part. */
	*cpu = execute->cpu + EXECUTE_TABLES_AT + execute->tables;
	*va = execute->va + EXECUTE_TABLES_AT + execute->tables;
	kern_memset(*cpu, 0, rounded);
	execute->tables += rounded;

	/* Succeeded: the part is the caller's. */
	return 0;
}

/*
 * Begins a pass: its colour targets (the views of its colour attachments,
 * by colour attachment) and its depth target (the view of its depth
 * attachment, with its stencil plane), the size they all have, and the
 * clear of each that loads by clearing: a quad over the area for each
 * colour target cleared, that writes its clear colour, and with the
 * depth unit's surface set the clear depth (the viewport's depth range
 * the clear value) and the clear stencil (replaced by the test) with the
 * first quad (or a quad of its own writing no colour).
 */
static int
execute_begin_pass(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_render_pass *pass;
	const struct amdrdna_vk_framebuffer *framebuffer;
	const struct amdrdna_vk_view *view;
	const struct amdrdna_vk_image *image;
	uint32_t cleared;
	uint32_t attachment;
	uint32_t index;
	uint32_t views;
	bool clear_depth;
	bool clear_stencil;
	int32_t x1;
	int32_t y1;
	int error;

	UNUSED_PARAMETER(session);

	pass = record->objects[0]->payload;
	framebuffer = record->objects[1]->payload;

	/* In the pass; nothing written yet, no target yet. */
	state->in_pass = true;
	state->pass = pass;
	state->framebuffer = framebuffer;
	state->target_written = false;
	state->area = record->rect;
	kern_memset(state->targets, 0, sizeof(state->targets));
	state->target_count = pass->colour_count;
	state->width = framebuffer->width;
	state->height = framebuffer->height;
	state->depth_on = false;
	state->stencil_on = false;

	/* Each colour target: its view's first level of its first layer, in the view's format; the size every target has. */
	state->samples = pass->samples;
	for (index = 0U; index < pass->colour_count; index++) {
		attachment = pass->colour_attachments[index];
		if (attachment == VK_ATTACHMENT_UNUSED)
			continue;
		error = execute_pass_view(framebuffer, attachment, &view);
		if (error != 0)
			return error;
		image = view->image->payload;
		if (image->samples != pass->samples)
			return EINVAL;
		error = execute_slice_target(image, view->format, view->base_level, view->base_layer, &state->targets[index]);
		if (error != 0)
			return error;
		if (state->width > state->targets[index].width)
			state->width = state->targets[index].width;
		if (state->height > state->targets[index].height)
			state->height = state->targets[index].height;
	}

	/* The depth target: the depth image, whole. */
	if (pass->depth != 0U) {
		error = execute_pass_view(framebuffer, pass->depth_attachment, &view);
		if (error != 0)
			return error;
		image = view->image->payload;
		if (!image->depth || image->samples != pass->samples)
			return EINVAL;
		error = execute_depth_target(image, &state->depth);
		if (error != 0)
			return error;
		state->stencil_on = image->stencil;
		state->depth_on = true;
		if (state->width > image->width)
			state->width = image->width;
		if (state->height > image->height)
			state->height = image->height;
	}

	/*
	 * A pass without any target has nothing to clear; its draws raster at
	 * the framebuffer's size and the pipeline's samples (what they write
	 * to memory, and what the depth units count, is all they do).
	 *
	 * Values referenced: the Vulkan 1.3 specification (Render Pass: a
	 * subpass without attachments rasterizes at the framebuffer's width
	 * and height, and the pipeline's rasterizationSamples).
	 */
	if (!execute_pass_targets(state))
		return 0;

	/* What loads by clearing: the colour targets with a clear colour, the depth, the stencil. */
	cleared = 0U;
	for (index = 0U; index < pass->colour_count; index++) {
		attachment = pass->colour_attachments[index];
		if (state->targets[index].address == 0U || pass->load_ops[index] != VK_ATTACHMENT_LOAD_OP_CLEAR)
			continue;
		if (record->colours != NULL && attachment < AMDRDNA_VK_ATTACHMENTS_MAX && (record->colours->mask & (1U << attachment)) != 0U)
			cleared |= 1U << index;
	}
	clear_depth = false;
	if (state->depth_on && pass->depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR && record->values[5] != 0U)
		clear_depth = true;
	clear_stencil = false;
	if (state->stencil_on && pass->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR && record->values[5] != 0U)
		clear_stencil = true;
	if (cleared == 0U && !clear_depth && !clear_stencil)
		return 0;

	/* The clear's area, within the framebuffer and the targets. */
	x1 = state->area.offset.x + (int32_t)state->area.extent.width;
	y1 = state->area.offset.y + (int32_t)state->area.extent.height;
	if (x1 > (int32_t)state->width)
		x1 = (int32_t)state->width;
	if (y1 > (int32_t)state->height)
		y1 = (int32_t)state->height;
	if (state->area.offset.x < 0 || state->area.offset.y < 0 || x1 <= state->area.offset.x || y1 <= state->area.offset.y)
		return 0;

	/* The quads, in each view's layer of a multiview pass. */
	views = pass->view_mask;
	if (views == 0U)
		views = 1U;
	for (index = 0U; index < AMDRDNA_VK_VIEWS_MAX; index++) {
		if ((views & (1U << index)) == 0U)
			continue;
		if (pass->view_mask != 0U) {
			error = execute_view_targets(state, index);
			if (error != 0)
				return error;
			state->target_written = false;
		}
		error = execute_clear_quads(execute, state, pass->colour_attachments, record->colours, cleared, clear_depth, clear_stencil,
					    record->values[4], record->count, state->area.offset.x, state->area.offset.y, x1, y1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the pass draws into its targets; the draws write them again. */
	state->target_written = false;
	return 0;
}

/*
 * Runs a draw or a clear of the attachments: once, or under a multiview
 * pass once for each view of the subpass, its colour targets at their
 * views' layers plus the view and the draw with the pipeline's programs
 * of the view (amdrdna_vk_pipeline views).  Each view's instances are
 * numbered as without multiview.
 */
static int
execute_views(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_pipeline *pipeline;
	uint32_t view;
	int error;

	/* Outside a multiview pass, the record as it is. */
	if (!state->in_pass || state->pass == NULL || state->pass->view_mask == 0U) {
		if (record->opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS)
			error = execute_clear_attachments(execute, state, record);
		else
			error = execute_draw(execute, state, record);
		return error;
	}

	/* Each view: its targets, its programs, the record. */
	pipeline = state->pipeline;
	error = 0;
	for (view = 0U; view < AMDRDNA_VK_VIEWS_MAX && error == 0; view++) {
		if ((state->pass->view_mask & (1U << view)) == 0U)
			continue;
		error = execute_view_targets(state, view);
		if (error != 0)
			break;
		state->target_written = false;
		if (pipeline != NULL && pipeline->views[view] != NULL)
			state->pipeline = pipeline->views[view];
		if (record->opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS)
			error = execute_clear_attachments(execute, state, record);
		else
			error = execute_draw(execute, state, record);
		state->pipeline = pipeline;
	}

	/* The next record writes its targets again. */
	state->target_written = false;
	return error;
}

/*
 * Points the colour targets of a multiview pass at one view: each at its
 * attachment view's first level of its layer plus the view (the
 * framebuffer's views have a layer for each view).  The depth target is
 * one layer (depth images have one), which only view 0 may draw.
 */
static int
execute_view_targets(
	struct execute_state *state,
	uint32_t view)
{
	const struct amdrdna_vk_render_pass *pass;
	const struct amdrdna_vk_view *image_view;
	const struct amdrdna_vk_image *image;
	uint32_t attachment;
	uint32_t index;
	int error;

	/* A depth target of one layer has no layer for a view past 0. */
	pass = state->pass;
	if (state->depth_on && view != 0U)
		return ENOTSUP;

	/* Each colour target at the view's layer. */
	for (index = 0U; index < pass->colour_count; index++) {
		attachment = pass->colour_attachments[index];
		if (attachment == VK_ATTACHMENT_UNUSED)
			continue;
		error = execute_pass_view(state->framebuffer, attachment, &image_view);
		if (error != 0)
			return error;
		if (view >= image_view->layer_count)
			return EINVAL;
		image = image_view->image->payload;
		error = execute_slice_target(image, image_view->format, image_view->base_level, image_view->base_layer + view, &state->targets[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the targets are the view's. */
	return 0;
}

/* Reports whether the pass the records are in has a target: a colour target, or the depth unit's surface. */
static bool
execute_pass_targets(
	const struct execute_state *state)
{
	uint32_t index;

	/* The depth unit's surface. */
	if (state->depth_on)
		return true;

	/* A colour target. */
	for (index = 0U; index < state->target_count; index++) {
		if (state->targets[index].address != 0U)
			return true;
	}

	/* Succeeded: none. */
	return false;
}

/*
 * Clears targets of the pass over a rectangle (x0 y0 to x1 y1, within
 * them): a quad for each colour target with a bit in cleared (1 << colour
 * attachment), writing its colour from colours (by attachment number
 * through numbers when numbers is not NULL, else by colour attachment),
 * and the depth (depth, its float's bits) and the stencil (stencil) when
 * clear_depth and clear_stencil say so, with the first quad, or a quad of
 * their own writing no colour; the depth unit's surface set first.
 */
static int
execute_clear_quads(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const uint32_t *numbers,
	const struct amdrdna_vk_clear_colours *colours,
	uint32_t cleared,
	bool clear_depth,
	bool clear_stencil,
	uint32_t depth,
	uint32_t stencil,
	int32_t x0,
	int32_t y0,
	int32_t x1,
	int32_t y1)
{
	struct execute_quad quad;
	uint32_t index;
	uint32_t number;
	int error;

	/* The depth unit's surface for the clear (the draws set it again). */
	if (clear_depth || clear_stencil) {
		error = amdrdna_vk_state_depth_target(&execute->pm4, &state->depth);
		if (error != 0)
			return error;
	}

	/* The quad's rectangle; the depth and the stencil with the first. */
	kern_memset(&quad, 0, sizeof(quad));
	quad.x0 = x0;
	quad.y0 = y0;
	quad.x1 = x1;
	quad.y1 = y1;
	quad.depth_clear = clear_depth;
	quad.depth_value = depth;
	quad.stencil_clear = clear_stencil;
	quad.stencil_value = stencil;

	/* A quad for each colour target cleared, in its colour. */
	for (index = 0U; index < state->target_count; index++) {
		if ((cleared & (1U << index)) == 0U)
			continue;
		number = index;
		if (numbers != NULL)
			number = numbers[index];
		quad.target = state->targets[index];
		kern_memcpy(quad.colour, colours->words[number], sizeof(quad.colour));
		quad.colour_off = false;
		error = execute_quad(execute, &quad);
		if (error != 0)
			return error;
		quad.depth_clear = false;
		quad.stencil_clear = false;
	}

	/* The depth and the stencil alone, with no colour target written (the pass's samples rasterized). */
	if (quad.depth_clear || quad.stencil_clear) {
		kern_memset(&quad.target, 0, sizeof(quad.target));
		quad.target.width = state->width;
		quad.target.height = state->height;
		quad.target.samples = state->samples;
		quad.colour_off = true;
		error = execute_quad(execute, &quad);
		if (error != 0)
			return error;
	}

	/* Succeeded: the targets are cleared over the rectangle. */
	return 0;
}

/*
 * Clears attachments of the pass in place: over each rectangle (within
 * the render area and the targets) a quad for each colour target cleared
 * in its colour, and the clear depth and stencil when they are and the
 * pass has them, as execute_clear_quads() draws them.  The draws after it
 * write their targets and state again.  EINVAL outside a pass; a pass
 * without a target clears nothing.
 */
static int
execute_clear_attachments(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const VkClearRect *rects;
	uint32_t cleared;
	uint32_t index;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	bool clear_depth;
	bool clear_stencil;
	int error;

	/* A clear in a pass with a target. */
	if (!state->in_pass)
		return EINVAL;
	if (state->width == 0U || state->height == 0U)
		return 0;

	/* What the clear writes: the colour targets it names that the pass has, the depth and the stencil the pass has. */
	cleared = 0U;
	if ((record->first & VK_IMAGE_ASPECT_COLOR_BIT) != 0U && record->colours != NULL) {
		for (index = 0U; index < state->target_count; index++) {
			if (state->targets[index].address != 0U && (record->colours->mask & (1U << index)) != 0U)
				cleared |= 1U << index;
		}
	}
	clear_depth = state->depth_on && (record->first & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U;
	clear_stencil = state->stencil_on && (record->first & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U;
	if (cleared == 0U && !clear_depth && !clear_stencil)
		return 0;

	/* Each rectangle within the render area and the targets. */
	rects = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		x0 = rects[index].rect.offset.x;
		y0 = rects[index].rect.offset.y;
		x1 = x0 + (int32_t)rects[index].rect.extent.width;
		y1 = y0 + (int32_t)rects[index].rect.extent.height;
		if (x0 < state->area.offset.x)
			x0 = state->area.offset.x;
		if (y0 < state->area.offset.y)
			y0 = state->area.offset.y;
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > state->area.offset.x + (int32_t)state->area.extent.width)
			x1 = state->area.offset.x + (int32_t)state->area.extent.width;
		if (y1 > state->area.offset.y + (int32_t)state->area.extent.height)
			y1 = state->area.offset.y + (int32_t)state->area.extent.height;
		if (x1 > (int32_t)state->width)
			x1 = (int32_t)state->width;
		if (y1 > (int32_t)state->height)
			y1 = (int32_t)state->height;
		if (x1 <= x0 || y1 <= y0)
			continue;

		/* The rectangle's quads. */
		error = execute_clear_quads(execute, state, NULL, record->colours, cleared, clear_depth, clear_stencil, record->values[4],
					    record->values[5], x0, y0, x1, y1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the attachments are cleared; the draws after it write their targets and state again. */
	state->target_written = false;
	return 0;
}

/*
 * Gives the view of a framebuffer's attachment by number.  Returns 0, or
 * EINVAL for an attachment the framebuffer does not have.
 */
static int
execute_pass_view(
	const struct amdrdna_vk_framebuffer *framebuffer,
	uint32_t attachment,
	const struct amdrdna_vk_view **view)
{
	/* An attachment the framebuffer holds. */
	if (attachment >= framebuffer->count || framebuffer->views[attachment] == NULL)
		return EINVAL;

	/* Succeeded: the attachment's view. */
	*view = framebuffer->views[attachment]->payload;
	return 0;
}

/*
 * Runs a draw or an indexed draw in a pass: the resource tables of its two
 * programs, the targets when not written yet, the viewport and scissor,
 * the blend, culling and depth control, the programs and the draw.
 */
static int
execute_draw(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_pipeline *pipeline;
	const uint32_t *viewport;
	const VkRect2D *scissor;
	struct execute_tables tables;
	int32_t clipped[4];
	uint32_t base_vertex;
	uint32_t first_instance;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	int error;

	/* A draw in a pass with a target, with a pipeline; an indexed one with an index buffer. */
	pipeline = state->pipeline;
	if (!state->in_pass || pipeline == NULL)
		return EINVAL;
	if (record->opcode == GPU_OP_CMD_DRAW_INDEXED && state->index_buffer == NULL)
		return EINVAL;
	if (state->width == 0U || state->height == 0U || record->values[0] == 0U || record->values[1] == 0U)
		return 0;

	/* The first vertex (an indexed draw's vertex offset) and the first instance. */
	base_vertex = record->values[2];
	first_instance = record->values[3];
	if (record->opcode == GPU_OP_CMD_DRAW_INDEXED) {
		base_vertex = record->values[3];
		first_instance = record->values[4];
	}

	/* The two programs' resource tables. */
	error = execute_draw_tables(execute, state, pipeline, &tables);
	if (error != 0)
		return error;

	/* The scissor: dynamic or the pipeline's, within the target; nothing to draw outside it. */
	scissor = &pipeline->scissor;
	if ((pipeline->dynamic & (1U << VK_DYNAMIC_STATE_SCISSOR)) != 0U && state->scissor_set)
		scissor = &state->scissor;
	x0 = scissor->offset.x;
	y0 = scissor->offset.y;
	x1 = x0 + (int32_t)scissor->extent.width;
	y1 = y0 + (int32_t)scissor->extent.height;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)state->width)
		x1 = (int32_t)state->width;
	if (y1 > (int32_t)state->height)
		y1 = (int32_t)state->height;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	/* The viewport: dynamic or the pipeline's. */
	viewport = pipeline->viewport;
	if ((pipeline->dynamic & (1U << VK_DYNAMIC_STATE_VIEWPORT)) != 0U && state->viewport_set)
		viewport = state->viewport;

	/*
	 * The draw after another of the same pipeline, viewport and scissor in
	 * this buffer, with no dynamic state changed between them, writes only
	 * its tables and first vertex and instance: the rest the other draw
	 * wrote stays.
	 */
	if (state->target_written && state->drawn_pipeline == pipeline && !state->dynamic_changed &&
	    kern_memcmp(state->drawn_viewport, viewport, sizeof(state->drawn_viewport)) == 0 &&
	    state->drawn_scissor[0] == x0 && state->drawn_scissor[1] == y0 && state->drawn_scissor[2] == x1 && state->drawn_scissor[3] == y1) {
		amdrdna_vk_state_program_data(&execute->pm4, &pipeline->vertex, tables.vertex_va, tables.fragment_va, base_vertex, first_instance,
					      tables.instance_mask, state->drawn_culling);
		return execute_draw_end(execute, state, record, &tables);
	}

	/* The targets, once in a buffer: the colour targets, and the depth unit's surface or none. */
	if (!state->target_written) {
		error = amdrdna_vk_state_targets(&execute->pm4, state->targets, state->target_count);
		if (error == 0 && state->depth_on)
			error = amdrdna_vk_state_depth_target(&execute->pm4, &state->depth);
		if (error == 0 && !state->depth_on)
			error = amdrdna_vk_state_depth_target(&execute->pm4, NULL);
		if (error != 0)
			return error;
		state->target_written = true;
		state->fixed_written = false;
	}

	/* The viewport, the scissor, the blend, the raster and the depth test that differ from what the buffer holds. */
	clipped[0] = x0;
	clipped[1] = y0;
	clipped[2] = x1;
	clipped[3] = y1;
	error = execute_draw_fixed(execute, state, pipeline, viewport, clipped);
	if (error != 0)
		return error;

	/* The programs with their tables, the first vertex and instance, the culling of a primitive shader that culls. */
	amdrdna_vk_state_programs(&execute->pm4, &pipeline->vertex, &pipeline->fragment, tables.vertex_va, tables.fragment_va, base_vertex,
				  first_instance, tables.instance_mask, state->drawn_culling);

	/* What the next draw of the same may leave as it is. */
	state->drawn_pipeline = pipeline;

	/* The draw. */
	return execute_draw_end(execute, state, record, &tables);
}

/*
 * Writes the fixed state of a draw of a pipeline other than the last
 * one's, or the first in a buffer, or after a dynamic state changed: the
 * viewport and its guard band, the scissor (clipped, x0 y0 x1 y1), the
 * blend and its constants, the raster with its depth bias, the sampling
 * (the pass's samples, the pipeline's mask and alpha to coverage), and the depth
 * and stencil test, each only when it differs from what the buffer's
 * registers hold (all of it when fixed_written is clear), and the culling
 * words of a primitive shader that culls.  The drawn_ fields then hold
 * what the registers do.  Returns 0 or the blend's error.
 */
static int
execute_draw_fixed(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_pipeline *pipeline,
	const uint32_t *viewport,
	const int32_t *scissor)
{
	struct amdrdna_vk_depth_stencil depth;
	struct amdrdna_vk_blend blend[AMDRDNA_VK_COLOURS_MAX];
	uint32_t blend_count;
	uint32_t constants[4];
	uint32_t raster[3];
	uint32_t samples[3];
	uint32_t bias[3];
	uint32_t depth_format;
	bool same_samples;
	bool same_viewport;
	bool same_scissor;
	bool same_blend;
	bool same_constants;
	bool same_raster;
	bool same_bias;
	bool same_depth;
	uint32_t index;
	int compared;
	int error;

	/*
	 * The pipeline's blend of each colour target (the first, writing
	 * nothing, for a pass without one) and raster words; the depth bias,
	 * the constants and the depth and stencil test as dynamic state
	 * leaves them.
	 */
	kern_memset(blend, 0, sizeof(blend));
	blend_count = state->target_count;
	if (blend_count == 0U)
		blend_count = 1U;
	for (index = 0U; index < state->target_count; index++) {
		if (state->targets[index].address != 0U)
			blend[index] = pipeline->blend[index];
	}
	raster[0] = pipeline->cull_mode;
	raster[1] = pipeline->front_face;
	raster[2] = pipeline->depth_bias_enable;
	samples[0] = state->samples;
	if (!execute_pass_targets(state))
		samples[0] = pipeline->samples;
	samples[1] = pipeline->sample_mask;
	samples[2] = pipeline->alpha_to_coverage;
	execute_draw_dynamic(state, pipeline, bias, constants, &depth);

	/* Which of them the buffer's registers hold already (none before its first full draw). */
	same_viewport = false;
	same_scissor = false;
	same_blend = false;
	same_constants = false;
	same_raster = false;
	same_samples = false;
	same_bias = false;
	same_depth = false;
	if (state->fixed_written) {
		compared = kern_memcmp(state->drawn_viewport, viewport, sizeof(state->drawn_viewport));
		if (compared == 0 && state->drawn_topology == pipeline->topology)
			same_viewport = true;
		compared = kern_memcmp(state->drawn_scissor, scissor, sizeof(state->drawn_scissor));
		if (compared == 0)
			same_scissor = true;
		compared = kern_memcmp(state->drawn_blend, blend, sizeof(blend));
		if (compared == 0)
			same_blend = true;
		compared = kern_memcmp(state->drawn_constants, constants, sizeof(constants));
		if (compared == 0)
			same_constants = true;
		compared = kern_memcmp(state->drawn_raster, raster, sizeof(raster));
		if (compared == 0)
			same_raster = true;
		compared = kern_memcmp(state->drawn_samples, samples, sizeof(samples));
		if (compared == 0)
			same_samples = true;
		compared = kern_memcmp(state->drawn_bias, bias, sizeof(bias));
		if (compared == 0)
			same_bias = true;
		compared = kern_memcmp(&state->drawn_depth, &depth, sizeof(depth));
		if (compared == 0)
			same_depth = true;
	}

	/* The viewport with its guard band (which the topology widens), and the scissor. */
	if (!same_viewport) {
		amdrdna_vk_state_viewport(&execute->pm4, viewport);
		amdrdna_vk_state_guard_band(&execute->pm4, viewport, pipeline->topology);
	}

	if (!same_scissor)
		amdrdna_vk_state_scissor(&execute->pm4, (uint32_t)scissor[0], (uint32_t)scissor[1], (uint32_t)scissor[2], (uint32_t)scissor[3]);

	/* The blend and its constants. */
	if (!same_blend) {
		error = amdrdna_vk_state_blends(&execute->pm4, blend, blend_count);
		if (error != 0)
			return error;
	}

	if (!same_constants)
		amdrdna_vk_state_blend_constants(&execute->pm4, constants);

	/* The raster with its depth bias, the bias for the pass's depth surface (none without one), the depth and stencil test. */
	if (!same_raster)
		amdrdna_vk_state_raster(&execute->pm4, pipeline->cull_mode, pipeline->front_face, pipeline->depth_bias_enable != 0U);
	if (!same_samples)
		amdrdna_vk_state_samples(&execute->pm4, samples[0], pipeline->sample_mask, pipeline->alpha_to_coverage != 0U);
	if (!same_bias) {
		depth_format = 0U;
		if (state->depth_on)
			depth_format = state->depth.format;
		amdrdna_vk_state_depth_bias(&execute->pm4, bias, depth_format);
	}

	if (!same_depth)
		amdrdna_vk_state_depth(&execute->pm4, &depth);

	/* The culling words, which only the viewport, the topology and the raster words change. */
	if (!same_viewport || !same_raster)
		amdrdna_vk_state_culling(viewport, pipeline->topology, pipeline->cull_mode, pipeline->front_face, state->drawn_culling);

	/* What the registers hold now. */
	kern_memcpy(state->drawn_viewport, viewport, sizeof(state->drawn_viewport));
	kern_memcpy(state->drawn_scissor, scissor, sizeof(state->drawn_scissor));
	state->drawn_topology = pipeline->topology;
	kern_memcpy(state->drawn_blend, blend, sizeof(blend));
	kern_memcpy(state->drawn_constants, constants, sizeof(constants));
	kern_memcpy(state->drawn_raster, raster, sizeof(raster));
	kern_memcpy(state->drawn_samples, samples, sizeof(samples));
	kern_memcpy(state->drawn_bias, bias, sizeof(bias));
	state->drawn_depth = depth;
	state->fixed_written = true;
	state->dynamic_changed = false;

	/* Succeeded: the fixed state is the draw's. */
	return 0;
}

/*
 * Gives a draw's depth bias (constant, clamp, slope), blend constants and
 * depth and stencil test: the pipeline's, with the state it leaves
 * dynamic as its command buffer last set it.  The depth test is off
 * without a depth surface, the stencil test without a stencil plane.
 */
static void
execute_draw_dynamic(
	const struct execute_state *state,
	const struct amdrdna_vk_pipeline *pipeline,
	uint32_t *bias,
	uint32_t *constants,
	struct amdrdna_vk_depth_stencil *depth)
{
	struct amdrdna_vk_stencil_face *faces[2];
	uint32_t *values[EXECUTE_STENCIL_VALUES];
	uint32_t face;
	uint32_t kind;
	uint32_t taken[EXECUTE_STENCIL_VALUES];

	/* The depth bias and the blend constants. */
	kern_memcpy(bias, pipeline->depth_bias, sizeof(pipeline->depth_bias));
	if ((pipeline->dynamic & (1U << VK_DYNAMIC_STATE_DEPTH_BIAS)) != 0U && state->bias_set)
		kern_memcpy(bias, state->bias, sizeof(state->bias));
	kern_memcpy(constants, pipeline->blend_constants, sizeof(pipeline->blend_constants));
	if ((pipeline->dynamic & (1U << VK_DYNAMIC_STATE_BLEND_CONSTANTS)) != 0U && state->constants_set)
		kern_memcpy(constants, state->constants, sizeof(state->constants));

	/* The depth test, on a depth surface. */
	kern_memset(depth, 0, sizeof(*depth));
	if (state->depth_on && pipeline->depth_test != 0U) {
		depth->test = 1U;
		depth->compare = pipeline->depth_compare;
	}
	if (state->depth_on && pipeline->depth_write != 0U)
		depth->write = 1U;

	/* The stencil test, on a stencil plane. */
	if (!state->stencil_on || pipeline->stencil_test == 0U)
		return;
	depth->stencil = 1U;
	depth->front = pipeline->stencil[0];
	depth->back = pipeline->stencil[1];

	/* Each face's masks and reference the command buffer set, for those the pipeline leaves dynamic. */
	taken[EXECUTE_STENCIL_COMPARE] = pipeline->dynamic & (1U << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
	taken[EXECUTE_STENCIL_WRITE] = pipeline->dynamic & (1U << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
	taken[EXECUTE_STENCIL_REFERENCE] = pipeline->dynamic & (1U << VK_DYNAMIC_STATE_STENCIL_REFERENCE);
	faces[0] = &depth->front;
	faces[1] = &depth->back;
	for (face = 0U; face < 2U; face++) {
		values[EXECUTE_STENCIL_COMPARE] = &faces[face]->compare_mask;
		values[EXECUTE_STENCIL_WRITE] = &faces[face]->write_mask;
		values[EXECUTE_STENCIL_REFERENCE] = &faces[face]->reference;
		for (kind = 0U; kind < EXECUTE_STENCIL_VALUES; kind++) {
			if (taken[kind] == 0U || (state->stencil_set & (1U << (face * EXECUTE_STENCIL_VALUES + kind))) == 0U)
				continue;
			*values[kind] = state->stencil[face][kind];
		}
	}
}

/* Gives the set numbers a program's resource table reads, a bit each. */
static uint32_t
execute_program_sets(
	const struct amdrdna_gfx10_program *program)
{
	uint32_t sets;
	uint32_t index;

	/* The storage buffers', the uniform buffers', the sampled images' and the storage images' sets. */
	sets = 0U;
	for (index = 0U; index < program->storage_used; index++)
		sets |= 1U << program->storage[index].set;
	for (index = 0U; index < program->uniform_used; index++)
		sets |= 1U << program->uniforms[index].set;
	for (index = 0U; index < program->texture_used; index++)
		sets |= 1U << program->textures[index].set;
	for (index = 0U; index < program->image_used; index++)
		sets |= 1U << program->images[index].set;

	/* Succeeded: the sets are known. */
	return sets;
}

/*
 * Gives a draw's two resource tables: for the buffer's next draw of the
 * same pipeline, a table the last one used while nothing it reads changed
 * (the sets its program reads, the vertex buffers of the vertex program's,
 * the push constants), or a copy of it with the push constants written
 * again when only they changed; else a new table.
 */
static int
execute_draw_tables(
	struct amdrdna_vk_execute *execute,
	const struct execute_state *state,
	const struct amdrdna_vk_pipeline *pipeline,
	struct execute_tables *tables)
{
	const struct amdrdna_gfx10_program *vertex;
	const struct amdrdna_gfx10_program *fragment;
	const struct execute_tables *drawn;
	uint32_t vertex_sets;
	uint32_t fragment_sets;
	bool vertex_kept;
	bool fragment_kept;
	int error;

	vertex = pipeline->vertex.compiled;
	fragment = pipeline->fragment.compiled;
	drawn = &state->drawn_tables;

	/* Each program's table is the last draw's while the sets it reads did not change. */
	vertex_kept = false;
	fragment_kept = false;
	if (state->target_written && state->drawn_pipeline == pipeline) {
		vertex_sets = execute_program_sets(vertex);
		fragment_sets = execute_program_sets(fragment);
		if ((state->sets_changed & vertex_sets) == 0U && !state->buffers_changed)
			vertex_kept = true;
		if ((state->sets_changed & fragment_sets) == 0U)
			fragment_kept = true;
	}

	/* The vertex program's: kept, copied, or new with its vertex input. */
	if (vertex_kept && state->push_changed_from >= vertex->push_bytes) {
		tables->vertex_cpu = drawn->vertex_cpu;
		tables->vertex_va = drawn->vertex_va;
		tables->instance_mask = drawn->instance_mask;
	} else if (vertex_kept) {
		error = execute_table_copy(execute, vertex, state, drawn->vertex_cpu, &tables->vertex_cpu, &tables->vertex_va);
		if (error != 0)
			return error;
		tables->instance_mask = drawn->instance_mask;
	} else {
		error = execute_table(execute, vertex, true, &tables->vertex_cpu, &tables->vertex_va);
		if (error == 0)
			error = execute_resources(vertex, state, tables->vertex_cpu);
		if (error != 0)
			return error;
		execute_vertex_input(execute, pipeline, state, tables->vertex_cpu, &tables->instance_mask);
	}

	/* The pixel program's: kept, copied, or new. */
	if (fragment_kept && state->push_changed_from >= fragment->push_bytes) {
		tables->fragment_cpu = drawn->fragment_cpu;
		tables->fragment_va = drawn->fragment_va;
	} else if (fragment_kept) {
		error = execute_table_copy(execute, fragment, state, drawn->fragment_cpu, &tables->fragment_cpu, &tables->fragment_va);
		if (error != 0)
			return error;
	} else {
		error = execute_table(execute, fragment, true, &tables->fragment_cpu, &tables->fragment_va);
		if (error == 0)
			error = execute_resources(fragment, state, tables->fragment_cpu);
		if (error != 0)
			return error;
	}

	/* Succeeded: both tables are the draw's. */
	return 0;
}

/*
 * Gives a program's resource table as a copy of an earlier one of the
 * same program (at earlier) with the push constants written again.
 */
static int
execute_table_copy(
	struct amdrdna_vk_execute *execute,
	const struct amdrdna_gfx10_program *program,
	const struct execute_state *state,
	const uint8_t *earlier,
	uint8_t **cpu,
	uint64_t *va)
{
	uint32_t bytes;
	int error;

	/* A new part (not cleared: the earlier table covers it), the earlier table over it, then the push constants. */
	error = execute_table(execute, program, false, cpu, va);
	if (error != 0)
		return error;
	bytes = (program->table_bytes + EXECUTE_TABLE_ALIGN) & ~(EXECUTE_TABLE_ALIGN - 1U);
	kern_memcpy(*cpu, earlier, bytes);
	kern_memcpy(*cpu, state->push, program->push_bytes);

	/* Succeeded: the copy is the program's table. */
	return 0;
}

/*
 * Writes a draw's packets after its state and counts it; the tables it
 * used are kept for the next draw of the same.
 */
static int
execute_draw_end(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record,
	const struct execute_tables *tables)
{
	int error;

	/* The depth units' counting while an occlusion query is open: the passing samples of the draw's sample rate. */
	if (state->occlusion_queries != 0U)
		execute_depth_counts(execute, state);

	/* The draw. */
	error = execute_draw_call(execute, state, record);
	if (error != 0)
		return error;
	execute->draws++;

	/* Its tables. */
	state->drawn_tables = *tables;
	state->sets_changed = 0U;
	state->buffers_changed = false;
	state->push_changed_from = AMDRDNA_VK_PUSH_BYTES;

	/* Succeeded: the draw is written. */
	return 0;
}

/*
 * Writes a draw's packets: the generated vertices of a draw, or the
 * indices of an indexed draw from the bound index buffer (from its first
 * index on; the buffer's indices from there are the most it may read).
 * Returns 0, or EINVAL for an index buffer without storage or a first
 * index past its end.
 */
static int
execute_draw_call(
	struct amdrdna_vk_execute *execute,
	const struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_pipeline *pipeline;
	uint64_t indices;
	uint64_t room;
	uint64_t start;
	uint8_t *cpu;
	uint32_t size;
	bool primitive_shader;
	int error;

	/* A draw of generated vertices, by a primitive shader or a legacy vertex program. */
	pipeline = state->pipeline;
	primitive_shader = pipeline->vertex.registers.primitive_shader != 0U;
	if (record->opcode != GPU_OP_CMD_DRAW_INDEXED) {
		error = amdrdna_vk_state_draw(&execute->pm4, pipeline->topology, primitive_shader, record->values[0], record->values[1]);
		if (error != 0)
			return error;
		return 0;
	}

	/* The index buffer's storage, the first index's address and the indices from it. */
	error = amdrdna_vk_buffer_address(state->index_buffer, &indices, &cpu);
	if (error != 0)
		return error;
	size = 4U;
	if (state->index_type == VK_INDEX_TYPE_UINT16)
		size = 2U;
	start = state->index_offset + (uint64_t)record->values[2] * size;
	if (start >= state->index_buffer->size)
		return EINVAL;
	room = (state->index_buffer->size - start) / size;
	if (room > 0xffffffffU)
		room = 0xffffffffU;

	/* The indexed draw. */
	error = amdrdna_vk_state_draw_indexed(&execute->pm4, pipeline->topology, primitive_shader, state->index_type, indices + start, (uint32_t)room,
					      record->values[0], record->values[1]);
	if (error != 0)
		return error;

	/* Succeeded: the draw's packets are written. */
	return 0;
}

/*
 * Gives a program's resource table: the next part of the run's tables,
 * cleared when clear is set (else the caller writes all of it).
 */
static int
execute_table(
	struct amdrdna_vk_execute *execute,
	const struct amdrdna_gfx10_program *program,
	bool clear,
	uint8_t **cpu,
	uint64_t *va)
{
	uint32_t bytes;

	/* The table's bytes on the boundary (at least one, so that every table has an address). */
	bytes = (program->table_bytes + EXECUTE_TABLE_ALIGN) & ~(EXECUTE_TABLE_ALIGN - 1U);
	if (bytes > EXECUTE_TABLE_MAX)
		return ENOSPC;
	if (execute->tables + bytes > EXECUTE_TABLES_BYTES)
		return ENOSPC;

	/* The part. */
	*cpu = execute->cpu + EXECUTE_TABLES_AT + execute->tables;
	*va = execute->va + EXECUTE_TABLES_AT + execute->tables;
	if (clear)
		kern_memset(*cpu, 0, bytes);
	execute->tables += bytes;

	/* Succeeded: the part is the program's. */
	return 0;
}

/*
 * Writes a program's resource table: the push constants, then each
 * storage and uniform buffer's descriptor, each sampled image's image
 * and sampler descriptors and each storage image's descriptor from the
 * bound sets' bindings.  A binding with nothing bound refuses the draw.
 */
static int
execute_resources(
	const struct amdrdna_gfx10_program *program,
	const struct execute_state *state,
	uint8_t *table)
{
	const struct amdrdna_vk_descriptor_set *set;
	const struct amdrdna_gfx10_binding *binding;
	uint32_t image_words[AMDRDNA_IMAGE_WORDS];
	uint32_t sampler_words[AMDRDNA_SAMPLER_WORDS];
	uint32_t words[4];
	uint32_t index;
	int error;

	/* The push constants the program reads. */
	kern_memcpy(table, state->push, program->push_bytes);

	/* The storage buffers: their whole range (from a dynamic one's offset). */
	for (index = 0U; index < program->storage_used; index++) {
		binding = &program->storage[index];
		set = state->sets[binding->set];
		if (set == NULL)
			return EINVAL;
		error = execute_buffer_slot(amdrdna_vk_set_slot(set, binding->binding, binding->element),
					    execute_dynamic(set, state->dynamic[binding->set], binding->binding, binding->element), 0U, words);
		if (error != 0)
			return error;
		execute_put_words(table, binding->table_offset, words, 4U);
	}

	/* The uniform buffers: the bytes the program reads, within the range (from a dynamic one's offset). */
	for (index = 0U; index < program->uniform_used; index++) {
		binding = &program->uniforms[index];
		set = state->sets[binding->set];
		if (set == NULL)
			return EINVAL;
		error = execute_buffer_slot(amdrdna_vk_set_slot(set, binding->binding, binding->element),
					    execute_dynamic(set, state->dynamic[binding->set], binding->binding, binding->element), binding->size, words);
		if (error != 0)
			return error;
		execute_put_words(table, binding->table_offset, words, 4U);
	}

	/* The sampled images: the image descriptor, then the sampler's; a texel buffer's buffer descriptor in the image's place. */
	for (index = 0U; index < program->texture_used; index++) {
		binding = &program->textures[index];
		set = state->sets[binding->set];
		if (set == NULL)
			return EINVAL;
		if (binding->texel_buffer != 0U) {
			error = execute_texel_slot(amdrdna_vk_set_slot(set, binding->binding, binding->element), words);
			if (error != 0)
				return error;
			execute_put_words(table, binding->table_offset, words, 4U);
			continue;
		}
		error = execute_texture_slot(amdrdna_vk_set_slot(set, binding->binding, binding->element), image_words, sampler_words);
		if (error != 0)
			return error;
		execute_put_words(table, binding->table_offset, image_words, AMDRDNA_IMAGE_WORDS);
		execute_put_words(table, binding->table_offset + AMDRDNA_IMAGE_WORDS * 4U, sampler_words, AMDRDNA_SAMPLER_WORDS);
	}

	/* The storage images and storage texel buffers. */
	error = execute_storage_images(program, state->sets, table);
	if (error != 0)
		return error;

	/* Succeeded: the table holds every resource. */
	return 0;
}

/*
 * Writes the raw buffer descriptor of a buffer binding: its range from its
 * offset and a dynamic offset (0 for a binding that is not dynamic), or
 * the first size bytes of it when size is given.  EINVAL for a binding
 * with no buffer or a start past the buffer's end, EIO for a buffer
 * without storage.
 */
static int
execute_buffer_slot(
	const struct amdrdna_vk_descriptor_slot *slot,
	uint32_t dynamic,
	uint32_t size,
	uint32_t *words)
{
	const struct amdrdna_vk_buffer *buffer;
	uint64_t start;
	uint64_t range;
	uint64_t va;
	uint8_t *cpu;
	int error;

	/* The binding's element, its buffer, with storage. */
	if (slot == NULL || slot->buffer == NULL)
		return EINVAL;
	buffer = slot->buffer->payload;
	error = amdrdna_vk_buffer_address(buffer, &va, &cpu);
	if (error != 0)
		return error;
	start = slot->offset + dynamic;
	if (start > buffer->size)
		return EINVAL;

	/* The range within the buffer (the whole size names the rest of it), then the bytes asked for. */
	range = buffer->size - start;
	if (slot->range < range)
		range = slot->range;
	if (size != 0U && size < range)
		range = size;
	if (range > 0xffffffffU)
		range = 0xffffffffU;

	/* Succeeded: the descriptor reads the range. */
	amdrdna_gfx10_raw_buffer_descriptor(va + start, (uint32_t)range, words);
	return 0;
}

/*
 * Writes the buffer descriptor of a uniform or storage texel buffer binding: its
 * view's range as records of the view's format, one a texel (the index
 * the program fetches), the channels the format lacks read as 0, 0, 0, 1,
 * and a texel past the range read as zeros.  EINVAL for a binding without
 * a view, EIO for a buffer without storage.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_buffer_view.c
 * (radv_make_texel_buffer_descriptor: the address at the view's offset,
 * the stride the format's texel and the records the range over it on
 * GFX10).
 */
static int
execute_texel_slot(
	const struct amdrdna_vk_descriptor_slot *slot,
	uint32_t *words)
{
	const struct amdrdna_vk_buffer_view *view;
	const struct amdrdna_vk_buffer *buffer;
	uint32_t code;
	uint32_t components;
	uint32_t bytes;
	uint64_t va;
	uint8_t *cpu;
	bool fetched;
	int error;

	/* An element naming a view, of a buffer with storage, in a format fetched. */
	if (slot == NULL || slot->texel == NULL)
		return EINVAL;
	view = slot->texel->payload;
	buffer = view->buffer->payload;
	error = amdrdna_vk_buffer_address(buffer, &va, &cpu);
	if (error != 0)
		return error;
	fetched = amdrdna_vk_texel_format(view->format, &code, &components, &bytes);
	if (!fetched)
		return EINVAL;

	/* Succeeded: the descriptor reads the view's texels. */
	amdrdna_gfx10_vertex_descriptor(va + view->offset, bytes, (uint32_t)(view->range / bytes), code, components, words);
	return 0;
}

/*
 * Writes the descriptors of a program's storage images (the image
 * descriptor of each binding's view) and storage texel buffers (a buffer
 * descriptor as a texel buffer's, in the image descriptor's place) from
 * the bound sets.  A binding with nothing bound refuses the draw or the
 * dispatch.
 */
static int
execute_storage_images(
	const struct amdrdna_gfx10_program *program,
	const struct amdrdna_vk_descriptor_set *const *sets,
	uint8_t *table)
{
	const struct amdrdna_vk_descriptor_slot *slot;
	const struct amdrdna_gfx10_binding *binding;
	uint32_t words[AMDRDNA_IMAGE_WORDS];
	uint32_t index;
	int error;

	/* Each binding's element of its bound set. */
	for (index = 0U; index < program->image_used; index++) {
		binding = &program->images[index];
		if (sets[binding->set] == NULL)
			return EINVAL;
		slot = amdrdna_vk_set_slot(sets[binding->set], binding->binding, binding->element);
		if (binding->texel_buffer != 0U) {
			error = execute_texel_slot(slot, words);
			if (error != 0)
				return error;
			execute_put_words(table, binding->table_offset, words, 4U);
			continue;
		}
		error = amdrdna_vk_storage_descriptor(slot, words);
		if (error != 0)
			return error;
		execute_put_words(table, binding->table_offset, words, AMDRDNA_IMAGE_WORDS);
	}

	/* Succeeded: the table holds every storage image. */
	return 0;
}

/*
 * Writes the image and the sampler descriptors of a combined image
 * sampler binding: the ones made when it was written, else made now.
 * EINVAL for a binding without both.
 */
static int
execute_texture_slot(
	const struct amdrdna_vk_descriptor_slot *slot,
	uint32_t *image_words,
	uint32_t *sampler_words)
{
	int error;

	/* An element of the binding. */
	if (slot == NULL)
		return EINVAL;

	/* The binding's own copy, made when it was written. */
	if (slot->words_ready) {
		kern_memcpy(image_words, slot->image_words, sizeof(slot->image_words));
		kern_memcpy(sampler_words, slot->sampler_words, sizeof(slot->sampler_words));
		return 0;
	}

	/* Else made from the view, its image and the sampler now. */
	error = amdrdna_vk_texture_descriptors(slot, image_words, sampler_words);
	if (error != 0)
		return error;

	/* Succeeded: the binding's descriptors are written. */
	return 0;
}

/*
 * Writes the vertex program's vertex buffer descriptors: each location it
 * fetches reads the pipeline's attribute there from the buffer its binding
 * names (the records after the attribute's start, the binding's stride
 * apart); a location without an attribute, or whose binding has no
 * storage, reads no records (zeros).  instance_mask gets the locations
 * read by instance.
 */
static void
execute_vertex_input(
	const struct amdrdna_vk_execute *execute,
	const struct amdrdna_vk_pipeline *pipeline,
	const struct execute_state *state,
	uint8_t *table,
	uint32_t *instance_mask)
{
	const struct amdrdna_gfx10_program *program;
	const struct amdrdna_vk_vertex_attribute *attribute;
	const struct amdrdna_vk_vertex_binding *binding;
	const struct amdrdna_vk_buffer *buffer;
	uint32_t words[4];
	uint32_t location;
	uint32_t index;
	uint32_t code;
	uint32_t components;
	uint32_t bytes;
	uint32_t records;
	uint64_t start;
	uint64_t room;
	uint64_t va;
	uint8_t *cpu;
	int error;

	program = pipeline->vertex.compiled;
	*instance_mask = 0U;
	for (location = 0U; location < AMDRDNA_GFX10_LOCATIONS; location++) {
		if ((program->fetched_locations & (1U << location)) == 0U)
			continue;

		/* No records until an attribute with storage is found. */
		amdrdna_gfx10_vertex_descriptor(execute->va + EXECUTE_ZERO_AT, 0U, 0U, AMDRDNA_GFX10_VERTEX_FLOAT4, 4U, words);

		/* The pipeline's attribute at the location, and its binding. */
		attribute = NULL;
		for (index = 0U; index < pipeline->attribute_count; index++) {
			if (pipeline->attributes[index].location == location)
				attribute = &pipeline->attributes[index];
		}
		binding = NULL;
		for (index = 0U; attribute != NULL && index < pipeline->binding_count; index++) {
			if (pipeline->bindings[index].binding == attribute->binding)
				binding = &pipeline->bindings[index];
		}

		/* The records of the bound buffer after the attribute's start. */
		buffer = NULL;
		if (binding != NULL)
			buffer = state->buffers[binding->binding];
		error = EINVAL;
		if (buffer != NULL)
			error = amdrdna_vk_buffer_address(buffer, &va, &cpu);
		if (error == 0 && amdrdna_vk_vertex_format(attribute->format, &code, &components, &bytes)) {
			start = state->offsets[binding->binding] + attribute->offset;
			if (start + bytes <= buffer->size) {
				/* Every record that holds the attribute whole (a stride of 0 reads the first record for every vertex). */
				room = buffer->size - start - bytes;
				records = 0xffffffffU;
				if (binding->stride != 0U && room / binding->stride < 0xffffffffU)
					records = (uint32_t)(room / binding->stride) + 1U;
				amdrdna_gfx10_vertex_descriptor(va + start, binding->stride, records, code, components, words);
				if (binding->instance != 0U)
					*instance_mask |= 1U << location;
			}
		}

		/* The location's descriptor. */
		execute_put_words(table, program->vertex_table_offset + location * 16U, words, 4U);
	}
}

/*
 * Runs a transfer outside a pass: the barrier before it (the work before
 * may have written its source), the transfer, the barrier after it (the
 * target written anew in the next pass).
 */
static int
execute_transfer(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	bool texels;
	bool depth;
	int error;

	/* What wrote the source has finished. */
	execute_barrier(execute);

	/*
	 * The transfer: a copy of an image whose texels are not four bytes is
	 * moved; a copy of a depth image drawn through the depth unit, between
	 * two images or with a buffer.
	 */
	texels = execute_texel_copy(record);
	depth = execute_depth_copy(record);
	if (texels) {
		error = execute_copy_texels(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
		error = execute_clear_depth(execute, record);
	} else if (depth && (record->opcode == GPU_OP_CMD_COPY_IMAGE || record->opcode == GPU_OP_CMD_BLIT_IMAGE)) {
		error = execute_depth_images(execute, record);
	} else if (depth) {
		error = execute_depth_buffer(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_COPY_BUFFER) {
		error = execute_copy_buffers(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_COPY_IMAGE || record->opcode == GPU_OP_CMD_BLIT_IMAGE) {
		error = execute_copy_images(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
		error = execute_clear_image(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_RESOLVE_IMAGE) {
		error = execute_resolve_images(execute, record);
	} else {
		error = execute_buffer_image(session, execute, state, record);
	}
	if (error != 0)
		return error;

	/* What follows reads what it wrote; the pass's target is written again. */
	execute_barrier(execute);
	state->target_written = false;

	/* Succeeded: the transfer is written. */
	return 0;
}

/*
 * Resets queries of a pool: a result status pool's on the CPU as the
 * record runs; an occlusion or timestamp pool's in its GPU memory, in the
 * order of the work around them (moves of the zeros, which leave every
 * query unwritten), after what wrote them before has finished.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_CmdResetQueryPool: the queries' bytes filled after the earlier
 * work's writes; query_clear_value 0 for an occlusion query).
 */
static int
execute_query_reset(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	struct amdrdna_vk_query_pool *pool;
	uint64_t address;
	uint64_t bytes;
	uint64_t step;
	int error;

	/* A result status pool's, on the CPU. */
	pool = record->objects[0]->payload;
	if (pool->token == NULL) {
		amdrdna_vk_query_reset(pool, record->first, record->count);
		return 0;
	}

	/* The queries' bytes zeroed once the work before has written them, in runs of the zeros there are. */
	execute_barrier(execute);
	address = pool->va + (uint64_t)record->first * pool->stride;
	bytes = (uint64_t)record->count * pool->stride;
	while (bytes != 0U) {
		step = bytes;
		if (step > EXECUTE_ZERO_BYTES)
			step = EXECUTE_ZERO_BYTES;
		error = execute_move(session, execute, state, execute->va + EXECUTE_ZERO_AT, address, step);
		if (error != 0)
			return error;
		address += step;
		bytes -= step;
	}

	/* What follows writes the queries after the zeros. */
	execute_barrier(execute);
	return 0;
}

/*
 * Writes a timestamp query: the GPU's timestamp once the work before it
 * has finished (every stage at the bottom of the pipe).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_CmdWriteTimestamp2, radv_write_timestamp: the end-of-pipe
 * timestamp for a stage after the top of the pipe).
 */
static int
execute_timestamp(
	struct amdrdna_vk_execute *execute,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_query_pool *pool;

	/* The query's 64 bits. */
	pool = record->objects[0]->payload;
	amdrdna_vk_pm4_timestamp(&execute->pm4, pool->va + (uint64_t)record->first * pool->stride);

	/* Succeeded: the timestamp is written when its turn comes. */
	return 0;
}

/*
 * Begins or ends an occlusion query: the depth units write their counts of
 * passing samples, the begin's at the query's start (8 bytes a backend
 * after the backend's 16), the end's 8 bytes on; while a query is open the
 * draws have them count (exactly when a query asks for precision), and
 * after the last one ends they stop.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_begin_occlusion_query, radv_end_occlusion_query: ZPASS_DONE at the
 * query's address and 8 bytes on; the precise flag's perfect counts while
 * a query is active) and src/amd/vulkan/radv_cmd_buffer.c
 * (radv_flush_occlusion_query_state: ZPASS_INCREMENT_DISABLE without
 * active queries).
 */
static void
execute_occlusion(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_query_pool *pool;
	uint64_t address;
	uint32_t views;
	uint32_t query;

	pool = record->objects[0]->payload;
	address = pool->va + (uint64_t)record->first * pool->stride;

	/* A begin: one more query open, counting from now. */
	if (record->opcode == GPU_OP_CMD_BEGIN_QUERY) {
		state->occlusion_queries++;
		if ((record->values[0] & VK_QUERY_CONTROL_PRECISE_BIT) != 0U)
			state->exact_counts = true;
		execute_depth_counts(execute, state);
		amdrdna_vk_pm4_event_address(&execute->pm4, AMDRDNA_PM4_EVENT_DEPTH_COUNTS, AMDRDNA_PM4_EVENT_INDEX_DEPTH_COUNTS, address);
		return;
	}

	/* An end: the counts at its end, one query fewer open, the counting off after the last. */
	amdrdna_vk_pm4_event_address(&execute->pm4, AMDRDNA_PM4_EVENT_DEPTH_COUNTS, AMDRDNA_PM4_EVENT_INDEX_DEPTH_COUNTS,
				     address + AMDRDNA_VK_QUERY_BACKEND_BYTES / 2U);

	/*
	 * In a multiview pass the query takes one query of the pool for each
	 * view: the views' draws counted in the first, each further one begins
	 * and ends at once, counting nothing.
	 */
	if (state->in_pass && state->pass != NULL) {
		views = (uint32_t)__builtin_popcount(state->pass->view_mask);
		for (query = 1U; query < views && record->first + query < pool->count; query++) {
			address = pool->va + (uint64_t)(record->first + query) * pool->stride;
			amdrdna_vk_pm4_event_address(&execute->pm4, AMDRDNA_PM4_EVENT_DEPTH_COUNTS, AMDRDNA_PM4_EVENT_INDEX_DEPTH_COUNTS, address);
			amdrdna_vk_pm4_event_address(&execute->pm4, AMDRDNA_PM4_EVENT_DEPTH_COUNTS, AMDRDNA_PM4_EVENT_INDEX_DEPTH_COUNTS,
						     address + AMDRDNA_VK_QUERY_BACKEND_BYTES / 2U);
		}
	}
	if (state->occlusion_queries != 0U)
		state->occlusion_queries--;
	if (state->occlusion_queries == 0U)
		state->exact_counts = false;
	execute_depth_counts(execute, state);
}

/*
 * Writes the depth units' counting: off without an open occlusion query;
 * else perfect counts of the passing samples on both slices, exact when
 * asked, at the power of the pass's samples.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_flush_occlusion_query_state: PERFECT_ZPASS_COUNTS,
 * DISABLE_CONSERVATIVE_ZPASS_COUNTS for a precise query, ZPASS_ENABLE,
 * SLICE_EVEN_ENABLE, SLICE_ODD_ENABLE, SAMPLE_RATE the log2 of the
 * rasterization samples).
 */
static void
execute_depth_counts(
	struct amdrdna_vk_execute *execute,
	const struct execute_state *state)
{
	uint32_t control;
	uint32_t samples;
	uint32_t power;

	/* No query open: no counting. */
	if (state->occlusion_queries == 0U) {
		amdrdna_vk_pm4_set_context(&execute->pm4, AMDRDNA_CX_DEPTH_COUNT, AMDRDNA_CX_DEPTH_COUNT_DISABLED);
		return;
	}

	/* The samples a pixel rasterizes: the pass's, or a pass without attachments the pipeline's. */
	samples = state->samples;
	if (!execute_pass_targets(state) && state->pipeline != NULL)
		samples = state->pipeline->samples;
	power = 0U;
	while (samples > 1U) {
		samples >>= 1;
		power++;
	}

	/* Perfect counts of the passing samples, exact when asked. */
	control = AMDRDNA_CX_DEPTH_COUNT_PERFECT | AMDRDNA_CX_DEPTH_COUNT_PASSED | (power << AMDRDNA_CX_DEPTH_COUNT_SAMPLES_SHIFT);
	if (state->exact_counts)
		control |= AMDRDNA_CX_DEPTH_COUNT_EXACT;
	amdrdna_vk_pm4_set_context(&execute->pm4, AMDRDNA_CX_DEPTH_COUNT, control);
}

/*
 * Runs a transfer of more regions than one record's room holds
 * (AMDRDNA_VK_REGIONS_MAX) as transfers of that many regions at most,
 * the buffer made room for before each one after the first.
 */
static int
execute_transfer_parts(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	struct amdrdna_vk_record part;
	size_t element;
	uint32_t first;
	uint32_t count;
	int error;

	/* A transfer of few enough regions at once. */
	if (record->region_count <= AMDRDNA_VK_REGIONS_MAX)
		return execute_transfer(session, execute, state, record);

	/* Each part of its regions in order, with room of its own. */
	element = execute_region_bytes(record->opcode);
	part = *record;
	for (first = 0U; first < record->region_count; first += count) {
		count = record->region_count - first;
		if (count > AMDRDNA_VK_REGIONS_MAX)
			count = AMDRDNA_VK_REGIONS_MAX;
		if (first != 0U) {
			error = execute_room(session, execute, state);
			if (error != 0)
				return error;
		}
		part.regions = (uint8_t *)record->regions + (size_t)first * element;
		part.region_count = count;
		error = execute_transfer(session, execute, state, &part);
		if (error != 0)
			return error;
	}

	/* Succeeded: every region is written. */
	return 0;
}

/* Reports the bytes of one region of a transfer's command (a clear's range). */
static size_t
execute_region_bytes(
	uint32_t opcode)
{
	/* The region each command records. */
	switch (opcode) {
	case GPU_OP_CMD_COPY_BUFFER:
		return sizeof(VkBufferCopy);
	case GPU_OP_CMD_COPY_IMAGE:
		return sizeof(VkImageCopy);
	case GPU_OP_CMD_BLIT_IMAGE:
		return sizeof(VkImageBlit);
	case GPU_OP_CMD_RESOLVE_IMAGE:
		return sizeof(VkImageResolve);
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
		return sizeof(VkBufferImageCopy);
	default:
		break;
	}

	/* Succeeded: a clear's range of levels and layers. */
	return sizeof(VkImageSubresourceRange);
}

/*
 * Copies regions between two buffers (vkCmdCopyBuffer) with the command
 * processor's moves.  Returns 0, or EINVAL for a buffer without storage or
 * a region outside a buffer.
 */
static int
execute_copy_buffers(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_buffer *source;
	const struct amdrdna_vk_buffer *destination;
	const VkBufferCopy *regions;
	uint64_t source_va;
	uint64_t destination_va;
	uint8_t *cpu;
	uint32_t index;
	int error;

	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;

	/* The two buffers, with storage. */
	error = amdrdna_vk_buffer_address(source, &source_va, &cpu);
	if (error == 0)
		error = amdrdna_vk_buffer_address(destination, &destination_va, &cpu);
	if (error != 0)
		return error;

	/* Each region, inside both buffers, moved. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		if (regions[index].srcOffset > source->size || regions[index].size > source->size - regions[index].srcOffset)
			return EINVAL;
		if (regions[index].dstOffset > destination->size || regions[index].size > destination->size - regions[index].dstOffset)
			return EINVAL;
		error = execute_move(session, execute, state, source_va + regions[index].srcOffset, destination_va + regions[index].dstOffset,
				     regions[index].size);
		if (error != 0)
			return error;

		/* Counted for the node. */
		amdrdna_vk_gpu_copied(session->owner, regions[index].size);
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies or blits regions between two images by drawing quads: the
 * destination region's quad samples the source region (a copy one to one
 * and nearest, a blit scaled and filtered), each region of one slice of
 * each.  A blit's region given backwards mirrors it.
 */
static int
execute_copy_images(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *source;
	const struct amdrdna_vk_image *destination;
	const VkImageCopy *copies;
	const VkImageBlit *blits;
	const VkImageSubresourceLayers *source_layers;
	const VkImageSubresourceLayers *destination_layers;
	struct amdrdna_vk_image source_level;
	struct amdrdna_vk_image destination_level;
	int32_t source_z;
	int32_t destination_z;
	struct execute_quad quad;
	uint8_t *cpu;
	uint32_t index;
	uint32_t from;
	uint32_t to;
	int32_t dx0;
	int32_t dy0;
	int32_t dx1;
	int32_t dy1;
	int32_t sx0;
	int32_t sy0;
	int32_t sx1;
	int32_t sy1;
	int error;

	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(state);

	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;

	/* Copied by quads that sample the source, nearest unless a blit says. */
	kern_memset(&quad, 0, sizeof(quad));
	quad.textured = true;
	quad.filter = VK_FILTER_NEAREST;
	if (record->opcode == GPU_OP_CMD_BLIT_IMAGE)
		quad.filter = record->values[0];

	/* Each region. */
	copies = record->regions;
	blits = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		/* The destination's and the source's corners. */
		if (record->opcode == GPU_OP_CMD_COPY_IMAGE) {
			dx0 = copies[index].dstOffset.x;
			dy0 = copies[index].dstOffset.y;
			dx1 = dx0 + (int32_t)copies[index].extent.width;
			dy1 = dy0 + (int32_t)copies[index].extent.height;
			sx0 = copies[index].srcOffset.x;
			sy0 = copies[index].srcOffset.y;
			sx1 = sx0 + (int32_t)copies[index].extent.width;
			sy1 = sy0 + (int32_t)copies[index].extent.height;
			from = copies[index].srcSubresource.mipLevel;
			to = copies[index].dstSubresource.mipLevel;
			source_layers = &copies[index].srcSubresource;
			destination_layers = &copies[index].dstSubresource;
			source_z = copies[index].srcOffset.z;
			destination_z = copies[index].dstOffset.z;
		} else {
			dx0 = blits[index].dstOffsets[0].x;
			dy0 = blits[index].dstOffsets[0].y;
			dx1 = blits[index].dstOffsets[1].x;
			dy1 = blits[index].dstOffsets[1].y;
			sx0 = blits[index].srcOffsets[0].x;
			sy0 = blits[index].srcOffsets[0].y;
			sx1 = blits[index].srcOffsets[1].x;
			sy1 = blits[index].srcOffsets[1].y;
			from = blits[index].srcSubresource.mipLevel;
			to = blits[index].dstSubresource.mipLevel;
			source_layers = &blits[index].srcSubresource;
			destination_layers = &blits[index].dstSubresource;
			source_z = blits[index].srcOffsets[0].z;
			destination_z = blits[index].dstOffsets[0].z;
		}

		/* The two levels of their slices, with storage: the destination's a target, the source's a texture. */
		error = amdrdna_vk_image_level(destination, to, &destination_level);
		if (error == 0)
			error = amdrdna_vk_image_level(source, from, &source_level);
		if (error == 0)
			error = execute_region_slice(destination, destination_layers, destination_z, &destination_level);
		if (error == 0)
			error = execute_region_slice(source, source_layers, source_z, &source_level);
		if (error == 0)
			error = execute_image_target(&destination_level, destination->format, &quad.target);
		if (error == 0)
			error = amdrdna_vk_image_address(&source_level, &quad.source, &cpu);
		if (error != 0)
			return error;
		quad.source_width = source_level.width;
		quad.source_height = source_level.height;
		quad.source_format = source->format;

		/* The quad covers the destination's region in order; a reversed edge swaps the source's. */
		quad.x0 = dx0;
		quad.x1 = dx1;
		quad.u0 = sx0;
		quad.u1 = sx1;
		if (dx1 < dx0) {
			quad.x0 = dx1;
			quad.x1 = dx0;
			quad.u0 = sx1;
			quad.u1 = sx0;
		}
		quad.y0 = dy0;
		quad.y1 = dy1;
		quad.v0 = sy0;
		quad.v1 = sy1;
		if (dy1 < dy0) {
			quad.y0 = dy1;
			quad.y1 = dy0;
			quad.v0 = sy1;
			quad.v1 = sy0;
		}

		error = execute_quad(execute, &quad);
		if (error != 0)
			return error;
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies regions between a buffer and an image.  A buffer region laid out
 * as a linear image (its start and its row pitch on 256 bytes) is drawn,
 * as a texture to an image or as a target from one; another is copied by
 * the CPU after the work before it ran.
 */
static int
execute_buffer_image(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_buffer *buffer;
	const struct amdrdna_vk_image *image;
	const VkBufferImageCopy *regions;
	const VkBufferImageCopy *region;
	const struct amdrdna_vk_image *whole;
	struct amdrdna_vk_image level;
	struct execute_quad quad;
	bool to_image;
	uint64_t buffer_va;
	uint64_t image_va;
	uint64_t start;
	uint8_t *buffer_cpu;
	uint8_t *image_cpu;
	uint32_t index;
	uint32_t row;
	uint32_t pitch;
	bool inside;
	int error;

	/* The buffer and the image, the buffer with storage. */
	to_image = (record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE);
	if (to_image) {
		buffer = record->objects[0]->payload;
		whole = record->objects[1]->payload;
	} else {
		whole = record->objects[0]->payload;
		buffer = record->objects[1]->payload;
	}
	error = amdrdna_vk_buffer_address(buffer, &buffer_va, &buffer_cpu);
	if (error != 0)
		return error;

	/* Each region, of a level of a slice of the image (with storage). */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		region = &regions[index];
		error = amdrdna_vk_image_level(whole, region->imageSubresource.mipLevel, &level);
		if (error == 0)
			error = execute_region_slice(whole, &region->imageSubresource, region->imageOffset.z, &level);
		if (error == 0)
			error = amdrdna_vk_image_address(&level, &image_va, &image_cpu);
		if (error != 0)
			return error;
		image = &level;

		/* Inside the image; the buffer's rows (a row length of zero is the region's width). */
		inside = execute_region_inside(region->imageOffset.x, region->imageOffset.y, region->imageExtent.width,
					       region->imageExtent.height, image->width, image->height);
		if (!inside || region->imageExtent.width == 0U || region->imageExtent.height == 0U)
			return EINVAL;
		row = region->bufferRowLength;
		if (row == 0U)
			row = region->imageExtent.width;
		pitch = row * AMDRDNA_VK_PIXEL_BYTES;
		if (region->bufferOffset > buffer->size ||
		    (uint64_t)pitch * (region->imageExtent.height - 1U) + (uint64_t)region->imageExtent.width * AMDRDNA_VK_PIXEL_BYTES >
		    buffer->size - region->bufferOffset)
			return EINVAL;
		start = buffer_va + region->bufferOffset;

		/*
		 * A region not laid out as a linear image is moved by the command
		 * processor, row by row, in the indirect buffer's order (the
		 * barriers around the transfer order it with the draws).
		 */
		if ((start % AMDRDNA_VK_RESOURCE_ALIGN) != 0U || (pitch % AMDRDNA_VK_PITCH_ALIGN) != 0U ||
		    pitch / AMDRDNA_VK_PIXEL_BYTES > AMDRDNA_VK_IMAGE_SIDE_MAX) {
			error = execute_move_rows(session, execute, state, image, buffer_va, region, to_image);
			if (error != 0)
				return error;

			/* Counted for the node. */
			amdrdna_vk_gpu_copied(session->owner, (uint64_t)region->imageExtent.width * AMDRDNA_VK_PIXEL_BYTES * region->imageExtent.height);
			continue;
		}

		/* Drawn: the destination's region samples the source's, one to one; the buffer is an image of its row length. */
		kern_memset(&quad, 0, sizeof(quad));
		quad.textured = true;
		quad.filter = VK_FILTER_NEAREST;
		if (to_image) {
			error = execute_image_target(image, image->format, &quad.target);
			quad.x0 = region->imageOffset.x;
			quad.y0 = region->imageOffset.y;
			quad.source = start;
			quad.source_width = row;
			quad.source_height = region->imageExtent.height;
			quad.source_format = image->format;
			quad.u0 = 0;
			quad.v0 = 0;
		} else {
			quad.target.address = start;
			quad.target.width = row;
			quad.target.height = region->imageExtent.height;
			quad.target.format = image->format;
			quad.x0 = 0;
			quad.y0 = 0;
			quad.source = image_va;
			quad.source_width = image->width;
			quad.source_height = image->height;
			quad.source_format = image->format;
			quad.u0 = region->imageOffset.x;
			quad.v0 = region->imageOffset.y;
		}
		if (error != 0)
			return error;
		quad.x1 = quad.x0 + (int32_t)region->imageExtent.width;
		quad.y1 = quad.y0 + (int32_t)region->imageExtent.height;
		quad.u1 = quad.u0 + (int32_t)region->imageExtent.width;
		quad.v1 = quad.v0 + (int32_t)region->imageExtent.height;
		error = execute_quad(execute, &quad);
		if (error != 0)
			return error;
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Moves one region between a buffer and a linear image with the command
 * processor: one move per row, the buffer's rows at its row length and
 * the image's at its pitch; to an image whose whole rows the region
 * covers at the buffer's pitch, one move for all of them (the bytes past
 * a row's pixels are the image's padding).  A move from the image never
 * writes the buffer between its rows.
 */
static int
execute_move_rows(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_image *image,
	uint64_t buffer_va,
	const VkBufferImageCopy *region,
	bool to_image)
{
	uint64_t image_va;
	uint64_t buffer_row;
	uint64_t image_row;
	uint64_t whole;
	uint8_t *image_cpu;
	uint32_t row_pixels;
	uint32_t buffer_pitch;
	uint32_t bytes;
	uint32_t y;
	int error;

	/* The image's address. */
	error = amdrdna_vk_image_address(image, &image_va, &image_cpu);
	if (error != 0)
		return error;

	/* The rows: the buffer's at its row length, the image's at its pitch, both of the image's texels. */
	row_pixels = region->bufferRowLength;
	if (row_pixels == 0U)
		row_pixels = region->imageExtent.width;
	buffer_pitch = row_pixels * image->texel_bytes;
	bytes = region->imageExtent.width * image->texel_bytes;
	buffer_row = buffer_va + region->bufferOffset;
	image_row = image_va + (uint64_t)region->imageOffset.y * image->pitch + (uint64_t)region->imageOffset.x * image->texel_bytes;

	/* Whole rows of the image at the buffer's pitch: one run of bytes. */
	if (to_image &&
	    region->imageOffset.x == 0 &&
	    region->imageExtent.width == image->width &&
	    buffer_pitch == image->pitch) {
		whole = (uint64_t)buffer_pitch * (region->imageExtent.height - 1U) + bytes;
		error = execute_move(session, execute, state, buffer_row, image_row, whole);
		if (error != 0)
			return error;

		/* Succeeded: the rows are moved. */
		return 0;
	}

	/* One move per row. */
	for (y = 0U; y < region->imageExtent.height; y++) {
		if (to_image) {
			error = execute_move(session, execute, state, buffer_row, image_row, bytes);
		} else {
			error = execute_move(session, execute, state, image_row, buffer_row, bytes);
		}
		if (error != 0)
			return error;

		/* The next row of each. */
		buffer_row += buffer_pitch;
		image_row += image->pitch;
	}

	/* Succeeded: the rows are moved. */
	return 0;
}

/*
 * Reports whether a copy names an image whose texels are not four bytes
 * (amdrdna_vk_image_moved): such a copy is the command processor's moves.
 * Blits and clears of such images were refused when they were recorded.
 */
static bool
execute_texel_copy(
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *first;
	const struct amdrdna_vk_image *second;

	/* The copies that name an image: between two, or between one and a buffer. */
	switch (record->opcode) {
	case GPU_OP_CMD_COPY_IMAGE:
		first = record->objects[0]->payload;
		second = record->objects[1]->payload;
		break;
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
		first = record->objects[1]->payload;
		second = first;
		break;
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
		first = record->objects[0]->payload;
		second = first;
		break;
	default:
		return false;
	}

	/* Either image copied only by moves. */
	if (amdrdna_vk_image_moved(first))
		return true;
	if (amdrdna_vk_image_moved(second))
		return true;

	/* Both images are drawn and sampled. */
	return false;
}

/*
 * Copies the regions of a copy between a buffer and an image, or between
 * two images, whose texels are not four bytes, with the command
 * processor's moves: each region a part of an image (a level, or a plane
 * named by its aspect) row by row, in the indirect buffer's order.
 */
static int
execute_copy_texels(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const VkBufferImageCopy *buffer_regions;
	const VkImageCopy *image_regions;
	uint32_t index;
	int error;

	/* Each region, between two images or between an image and the buffer. */
	buffer_regions = record->regions;
	image_regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		if (record->opcode == GPU_OP_CMD_COPY_IMAGE) {
			error = execute_texel_images(session, execute, state, record, &image_regions[index]);
		} else {
			error = execute_texel_buffer(session, execute, state, record, &buffer_regions[index]);
		}
		if (error != 0)
			return error;
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Moves one region between a buffer and a part of an image (a level, or a
 * plane named by its aspect): inside the part, the buffer's rows (its row
 * length in the part's texels, the region's width when zero) inside the
 * buffer.  Returns 0, EINVAL for a region outside either, or EIO for a
 * resource without storage.
 */
static int
execute_texel_buffer(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record,
	const VkBufferImageCopy *region)
{
	const struct amdrdna_vk_buffer *buffer;
	const struct amdrdna_vk_image *whole;
	struct amdrdna_vk_image part;
	uint64_t buffer_va;
	uint64_t pitch;
	uint64_t span;
	uint8_t *buffer_cpu;
	uint32_t row;
	bool to_image;
	bool inside;
	int error;

	/* The buffer, with storage, and the image. */
	to_image = (record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE);
	if (to_image) {
		buffer = record->objects[0]->payload;
		whole = record->objects[1]->payload;
	} else {
		whole = record->objects[0]->payload;
		buffer = record->objects[1]->payload;
	}
	error = amdrdna_vk_buffer_address(buffer, &buffer_va, &buffer_cpu);
	if (error != 0)
		return error;

	/* The part of the image the region names, of its slice. */
	error = amdrdna_vk_image_part(whole, region->imageSubresource.aspectMask, region->imageSubresource.mipLevel, &part);
	if (error == 0)
		error = execute_region_slice(whole, &region->imageSubresource, region->imageOffset.z, &part);
	if (error != 0)
		return error;

	/* Inside the part, of some size. */
	inside = execute_region_inside(region->imageOffset.x, region->imageOffset.y, region->imageExtent.width,
				       region->imageExtent.height, part.width, part.height);
	if (!inside || region->imageExtent.width == 0U || region->imageExtent.height == 0U)
		return EINVAL;

	/* The buffer's rows, every one inside the buffer. */
	row = region->bufferRowLength;
	if (row == 0U)
		row = region->imageExtent.width;
	if (row < region->imageExtent.width)
		return EINVAL;
	pitch = (uint64_t)row * part.texel_bytes;
	span = pitch * (region->imageExtent.height - 1U) + (uint64_t)region->imageExtent.width * part.texel_bytes;
	if (region->bufferOffset > buffer->size || span > buffer->size - region->bufferOffset)
		return EINVAL;

	/* The rows moved. */
	error = execute_move_rows(session, execute, state, &part, buffer_va, region, to_image);
	if (error != 0)
		return error;

	/* Counted for the node. */
	amdrdna_vk_gpu_copied(session->owner, (uint64_t)region->imageExtent.width * part.texel_bytes * region->imageExtent.height);

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Moves one region between parts of two images (each a level, or a plane
 * named by its aspect) of the same bytes a texel: row by row, or the
 * whole rows at once when the region is the full width of both at the
 * same pitch.  Returns 0, EINVAL for a region outside either part or parts
 * of different texels, or EIO for an image without storage.
 */
static int
execute_texel_images(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record,
	const VkImageCopy *region)
{
	struct amdrdna_vk_image source;
	struct amdrdna_vk_image destination;
	uint64_t source_row;
	uint64_t destination_row;
	uint64_t whole;
	uint8_t *cpu;
	uint32_t bytes;
	uint32_t y;
	bool inside;
	int error;

	/* The two parts the region names, of their slices, of the same texels. */
	error = amdrdna_vk_image_part(record->objects[0]->payload, region->srcSubresource.aspectMask, region->srcSubresource.mipLevel, &source);
	if (error == 0)
		error = execute_region_slice(record->objects[0]->payload, &region->srcSubresource, region->srcOffset.z, &source);
	if (error != 0)
		return error;
	error = amdrdna_vk_image_part(record->objects[1]->payload, region->dstSubresource.aspectMask, region->dstSubresource.mipLevel, &destination);
	if (error == 0)
		error = execute_region_slice(record->objects[1]->payload, &region->dstSubresource, region->dstOffset.z, &destination);
	if (error != 0)
		return error;
	if (source.texel_bytes != destination.texel_bytes)
		return EINVAL;

	/* Inside both, of some size. */
	if (region->extent.width == 0U || region->extent.height == 0U)
		return EINVAL;
	inside = execute_region_inside(region->srcOffset.x, region->srcOffset.y, region->extent.width, region->extent.height,
				       source.width, source.height);
	if (!inside)
		return EINVAL;
	inside = execute_region_inside(region->dstOffset.x, region->dstOffset.y, region->extent.width, region->extent.height,
				       destination.width, destination.height);
	if (!inside)
		return EINVAL;

	/* Their addresses, at the region's first row. */
	error = amdrdna_vk_image_address(&source, &source_row, &cpu);
	if (error != 0)
		return error;
	error = amdrdna_vk_image_address(&destination, &destination_row, &cpu);
	if (error != 0)
		return error;
	bytes = region->extent.width * source.texel_bytes;
	source_row += (uint64_t)region->srcOffset.y * source.pitch + (uint64_t)region->srcOffset.x * source.texel_bytes;
	destination_row += (uint64_t)region->dstOffset.y * destination.pitch + (uint64_t)region->dstOffset.x * destination.texel_bytes;

	/* Whole rows of both at one pitch: one run of bytes (the padding past a row's texels goes with it). */
	if (region->srcOffset.x == 0 &&
	    region->dstOffset.x == 0 &&
	    region->extent.width == source.width &&
	    region->extent.width == destination.width &&
	    source.pitch == destination.pitch) {
		whole = (uint64_t)source.pitch * (region->extent.height - 1U) + bytes;
		error = execute_move(session, execute, state, source_row, destination_row, whole);
		if (error != 0)
			return error;

		/* Counted for the node. */
		amdrdna_vk_gpu_copied(session->owner, (uint64_t)bytes * region->extent.height);

		/* Succeeded: the rows are copied at once. */
		return 0;
	}

	/* One move per row. */
	for (y = 0U; y < region->extent.height; y++) {
		error = execute_move(session, execute, state, source_row, destination_row, bytes);
		if (error != 0)
			return error;

		/* The next row of each. */
		source_row += source.pitch;
		destination_row += destination.pitch;
	}

	/* Counted for the node. */
	amdrdna_vk_gpu_copied(session->owner, (uint64_t)bytes * region->extent.height);

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Moves bytes between two addresses of the session's space in moves of
 * at most AMDRDNA_VK_MOVE_MAX, running the indirect buffer first when a
 * move would leave less than a record's room in it.
 */
static int
execute_move(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	uint64_t source,
	uint64_t destination,
	uint64_t bytes)
{
	uint32_t step;
	int error;

	/* Each move of the run. */
	while (bytes != 0U) {
		/* A buffer near its end runs first; the next one starts with its caches emptied. */
		if (execute->pm4.count + AMDRDNA_PM4_MOVE_WORDS > EXECUTE_BUFFER_WORDS - EXECUTE_RECORD_WORDS) {
			error = execute_flush(session, execute);
			if (error == 0)
				error = execute_start(session, execute);
			if (error != 0)
				return error;
			state->target_written = false;
		}

		/* The move. */
		step = AMDRDNA_VK_MOVE_MAX;
		if (bytes < step)
			step = (uint32_t)bytes;
		amdrdna_vk_pm4_move(&execute->pm4, source, destination, step);
		source += step;
		destination += step;
		bytes -= step;
	}

	/* Reports a buffer that overflowed. */
	if (execute->pm4.full)
		return ENOSPC;

	/* Succeeded: the moves are written. */
	return 0;
}

/*
 * Clears the levels and layers of an image its ranges name (every depth
 * slice of a 3D image's level; VK_REMAINING_* the rest of the image's) to
 * a colour: a quad of the colour over each level of each slice.
 */
static int
execute_clear_image(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *image;
	const VkImageSubresourceRange *ranges;
	struct execute_quad quad;
	uint32_t index;
	uint32_t levels;
	uint32_t layers;
	uint32_t at;
	uint32_t slice;
	uint32_t last;
	int error;

	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(state);

	image = record->objects[0]->payload;
	ranges = record->regions;

	/* Each range: its levels, and of each its slices. */
	kern_memset(&quad, 0, sizeof(quad));
	kern_memcpy(quad.colour, record->values, sizeof(quad.colour));
	for (index = 0U; index < record->region_count; index++) {
		if (ranges[index].baseMipLevel >= image->levels)
			return EINVAL;
		levels = ranges[index].levelCount;
		if (levels == VK_REMAINING_MIP_LEVELS || levels > image->levels - ranges[index].baseMipLevel)
			levels = image->levels - ranges[index].baseMipLevel;
		for (at = ranges[index].baseMipLevel; at < ranges[index].baseMipLevel + levels; at++) {
			/* The slices of the level: a 3D image's depth there, else the range's layers. */
			slice = 0U;
			last = image->slices;
			if (image->type == VK_IMAGE_TYPE_3D) {
				last = image->slices >> at;
				if (last == 0U)
					last = 1U;
			} else {
				slice = ranges[index].baseArrayLayer;
				layers = ranges[index].layerCount;
				if (slice >= image->slices)
					return EINVAL;
				if (layers == VK_REMAINING_ARRAY_LAYERS || layers > image->slices - slice)
					layers = image->slices - slice;
				last = slice + layers;
			}

			/* Each slice's level as a target (every sample of an image of samples), its whole rectangle. */
			for (; slice < last; slice++) {
				error = execute_slice_target(image, image->format, at, slice, &quad.target);
				if (error != 0)
					return error;
				quad.x0 = 0;
				quad.y0 = 0;
				quad.x1 = (int32_t)quad.target.width;
				quad.y1 = (int32_t)quad.target.height;
				error = execute_quad(execute, &quad);
				if (error != 0)
					return error;
			}
		}
	}

	/* Succeeded: the clear is written. */
	return 0;
}

/*
 * Reports whether a copy, a blit or a copy between a buffer and an image
 * names a depth image: such a copy is drawn through the depth unit
 * (vk-record.c took only those between a depth image's aspects and a
 * buffer, or between two depth images of one format).
 */
static bool
execute_depth_copy(
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *image;

	/* The image the command names: the source, or the destination of a copy from a buffer. */
	switch (record->opcode) {
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
		image = record->objects[0]->payload;
		break;
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
		image = record->objects[1]->payload;
		break;
	default:
		return false;
	}

	/* Succeeded: whether it is a depth image. */
	return image->depth;
}

/*
 * Copies or blits regions between two depth images of one format by
 * drawing quads into the destination's depth surface: for each aspect of
 * a region, a quad over the destination's region fetches the source's
 * texel bits (nearest, its texel coordinate the interpolated one cut to
 * an integer; a region given backwards mirrors it) and exports them as the
 * depth or as the stencil reference.  Returns 0, or EINVAL for a level or
 * layer the images do not have (one of each) or a region outside the
 * destination.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/meta/radv_meta_copy.c
 * and radv_meta_blit.c (a depth/stencil image copied and blitted by a
 * draw into its depth or stencil, one aspect at a time).
 */
static int
execute_depth_images(
	struct amdrdna_vk_execute *execute,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *source;
	const struct amdrdna_vk_image *destination;
	const VkImageCopy *copies;
	const VkImageBlit *blits;
	const VkImageSubresourceLayers *source_layers;
	const VkImageSubresourceLayers *destination_layers;
	struct amdrdna_vk_target surface;
	struct execute_quad quad;
	uint32_t index;
	uint32_t aspect;
	int32_t dx0;
	int32_t dy0;
	int32_t dx1;
	int32_t dy1;
	int32_t sx0;
	int32_t sy0;
	int32_t sx1;
	int32_t sy1;
	int error;

	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;

	/* The destination's depth surface, drawn into by quads of no colour. */
	error = execute_depth_target(destination, &surface);
	if (error != 0)
		return error;
	kern_memset(&quad, 0, sizeof(quad));
	quad.target.width = destination->width;
	quad.target.height = destination->height;
	quad.target.samples = 1U;
	quad.colour_off = true;
	quad.described = true;
	quad.filter = VK_FILTER_NEAREST;

	/* Each region. */
	copies = record->regions;
	blits = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		/* The destination's and the source's corners. */
		if (record->opcode == GPU_OP_CMD_COPY_IMAGE) {
			dx0 = copies[index].dstOffset.x;
			dy0 = copies[index].dstOffset.y;
			dx1 = dx0 + (int32_t)copies[index].extent.width;
			dy1 = dy0 + (int32_t)copies[index].extent.height;
			sx0 = copies[index].srcOffset.x;
			sy0 = copies[index].srcOffset.y;
			sx1 = sx0 + (int32_t)copies[index].extent.width;
			sy1 = sy0 + (int32_t)copies[index].extent.height;
			source_layers = &copies[index].srcSubresource;
			destination_layers = &copies[index].dstSubresource;
		} else {
			dx0 = blits[index].dstOffsets[0].x;
			dy0 = blits[index].dstOffsets[0].y;
			dx1 = blits[index].dstOffsets[1].x;
			dy1 = blits[index].dstOffsets[1].y;
			sx0 = blits[index].srcOffsets[0].x;
			sy0 = blits[index].srcOffsets[0].y;
			sx1 = blits[index].srcOffsets[1].x;
			sy1 = blits[index].srcOffsets[1].y;
			source_layers = &blits[index].srcSubresource;
			destination_layers = &blits[index].dstSubresource;
		}

		/* The images' one level and layer. */
		if (source_layers->mipLevel != 0U ||
		    source_layers->baseArrayLayer != 0U ||
		    destination_layers->mipLevel != 0U ||
		    destination_layers->baseArrayLayer != 0U)
			return EINVAL;

		/* The quad covers the destination's region in order; a reversed edge swaps the source's. */
		quad.x0 = dx0;
		quad.x1 = dx1;
		quad.u0 = sx0;
		quad.u1 = sx1;
		if (dx1 < dx0) {
			quad.x0 = dx1;
			quad.x1 = dx0;
			quad.u0 = sx1;
			quad.u1 = sx0;
		}
		quad.y0 = dy0;
		quad.y1 = dy1;
		quad.v0 = sy0;
		quad.v1 = sy1;
		if (dy1 < dy0) {
			quad.y0 = dy1;
			quad.y1 = dy0;
			quad.v0 = sy1;
			quad.v1 = sy0;
		}

		/* A quad for the depth, then one for the stencil, of those the region names. */
		for (aspect = VK_IMAGE_ASPECT_DEPTH_BIT; aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) {
			if ((source_layers->aspectMask & aspect) == 0U)
				continue;
			error = amdrdna_vk_depth_descriptor(source, aspect, true, NULL, quad.source_words);
			if (error != 0)
				return error;
			quad.program = execute_depth_program(destination, aspect);
			quad.depth_write = (aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
			quad.stencil_write = (aspect == VK_IMAGE_ASPECT_STENCIL_BIT);
			error = amdrdna_vk_state_depth_target(&execute->pm4, &surface);
			if (error == 0)
				error = execute_quad(execute, &quad);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies regions between a buffer and a depth image, each of one aspect:
 * the buffer's rows of that aspect's texels (four bytes of a 32-bit float
 * depth, two of a 16-bit depth, one of the stencil; a row length of zero
 * the region's width), drawn into the image or packed out of it through
 * the scratch.  Returns 0, or EINVAL for a buffer without storage, a
 * region outside the buffer or the image, or a level or layer the image
 * does not have.
 *
 * Values referenced: the Vulkan 1.3 specification (Copying Data Between
 * Buffers and Images: a depth aspect of D16_UNORM two bytes a texel, of
 * D32_SFLOAT and D32_SFLOAT_S8_UINT four, a stencil aspect one, packed
 * tightly in the buffer).
 */
static int
execute_depth_buffer(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_buffer *buffer;
	const struct amdrdna_vk_image *image;
	const VkBufferImageCopy *regions;
	const VkBufferImageCopy *region;
	uint64_t buffer_va;
	uint64_t start;
	uint64_t span;
	uint8_t *buffer_cpu;
	uint32_t index;
	uint32_t aspect;
	uint32_t texel_bytes;
	uint32_t row;
	uint32_t pitch;
	bool inside;
	bool to_image;
	int error;

	/* The buffer, with storage, and the depth image. */
	to_image = (record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE);
	if (to_image) {
		buffer = record->objects[0]->payload;
		image = record->objects[1]->payload;
	} else {
		image = record->objects[0]->payload;
		buffer = record->objects[1]->payload;
	}
	error = amdrdna_vk_buffer_address(buffer, &buffer_va, &buffer_cpu);
	if (error != 0)
		return error;

	/* Each region. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		region = &regions[index];

		/* The image's one level and layer, the region inside it. */
		if (region->imageSubresource.mipLevel != 0U || region->imageSubresource.baseArrayLayer != 0U)
			return EINVAL;
		inside = execute_region_inside(region->imageOffset.x, region->imageOffset.y, region->imageExtent.width,
					       region->imageExtent.height, image->width, image->height);
		if (!inside || region->imageExtent.width == 0U || region->imageExtent.height == 0U)
			return EINVAL;

		/* The aspect's texels in the buffer, the region's rows inside it. */
		aspect = region->imageSubresource.aspectMask;
		texel_bytes = execute_aspect_bytes(image, aspect);
		row = region->bufferRowLength;
		if (row == 0U)
			row = region->imageExtent.width;
		if (row < region->imageExtent.width)
			return EINVAL;
		pitch = row * texel_bytes;
		span = (uint64_t)pitch * (region->imageExtent.height - 1U) + (uint64_t)region->imageExtent.width * texel_bytes;
		if (region->bufferOffset > buffer->size || span > buffer->size - region->bufferOffset)
			return EINVAL;
		start = buffer_va + region->bufferOffset;

		/* Drawn into the image, or packed out of it. */
		if (to_image) {
			error = execute_depth_into(session, execute, state, image, aspect, start, pitch, region);
		} else {
			error = execute_depth_out(session, execute, state, image, aspect, start, pitch, region);
		}
		if (error != 0)
			return error;

		/* Counted for the node. */
		amdrdna_vk_gpu_copied(session->owner, (uint64_t)region->imageExtent.width * texel_bytes * region->imageExtent.height);
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies one region of a buffer (its rows from start, pitch bytes apart)
 * into an aspect of a depth image, band by band: the command processor
 * moves as many of its rows as the scratch holds there (rows of the
 * texels' bytes rounded up to 256), then a quad over the band's rows of
 * the image's region fetches each texel's bits from the scratch and
 * exports them as the depth or the stencil reference.  The band's moves
 * and quad go in one indirect buffer (the scratch is its slot's), with a
 * barrier between them and after the quad.
 */
static int
execute_depth_into(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_image *image,
	uint32_t aspect,
	uint64_t start,
	uint32_t pitch,
	const VkBufferImageCopy *region)
{
	struct amdrdna_vk_target surface;
	struct execute_quad quad;
	uint64_t scratch;
	uint32_t texel_bytes;
	uint32_t bytes;
	uint32_t scratch_pitch;
	uint32_t band;
	uint32_t rows;
	uint32_t first;
	uint32_t y;
	int error;

	/* The rows: the region's texels' bytes, in the scratch on 256 bytes. */
	texel_bytes = execute_aspect_bytes(image, aspect);
	bytes = region->imageExtent.width * texel_bytes;
	scratch_pitch = (bytes + AMDRDNA_VK_PITCH_ALIGN - 1U) & ~(AMDRDNA_VK_PITCH_ALIGN - 1U);
	band = EXECUTE_SCRATCH_BYTES / scratch_pitch;

	/* The quad: the image's depth surface, of no colour, the scratch's texels written as the aspect. */
	error = execute_depth_target(image, &surface);
	if (error != 0)
		return error;
	kern_memset(&quad, 0, sizeof(quad));
	quad.target.width = image->width;
	quad.target.height = image->height;
	quad.target.samples = 1U;
	quad.colour_off = true;
	quad.described = true;
	quad.filter = VK_FILTER_NEAREST;
	quad.program = execute_depth_program(image, aspect);
	quad.depth_write = (aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
	quad.stencil_write = (aspect == VK_IMAGE_ASPECT_STENCIL_BIT);

	/* Each band of rows. */
	for (first = 0U; first < region->imageExtent.height; first += rows) {
		rows = region->imageExtent.height - first;
		if (rows > band)
			rows = band;

		/* Room for the band in this indirect buffer, whose slot's scratch it uses. */
		error = execute_band_room(session, execute, state);
		if (error != 0)
			return error;
		scratch = execute->va + EXECUTE_SCRATCH_AT;

		/* The band's rows moved into the scratch (at once when the pitches agree). */
		if (pitch == scratch_pitch) {
			error = execute_move(session, execute, state, start + (uint64_t)first * pitch, scratch,
					     (uint64_t)scratch_pitch * (rows - 1U) + bytes);
			if (error != 0)
				return error;
		} else {
			for (y = 0U; y < rows; y++) {
				error = execute_move(session, execute, state, start + (uint64_t)(first + y) * pitch,
						     scratch + (uint64_t)y * scratch_pitch, bytes);
				if (error != 0)
					return error;
			}
		}
		execute_barrier(execute);

		/* The quad over the band's rows of the region, the scratch's texels from (0, 0). */
		error = amdrdna_vk_bits_descriptor(scratch, region->imageExtent.width, rows, texel_bytes, quad.source_words);
		if (error != 0)
			return error;
		quad.x0 = region->imageOffset.x;
		quad.y0 = region->imageOffset.y + (int32_t)first;
		quad.x1 = quad.x0 + (int32_t)region->imageExtent.width;
		quad.y1 = quad.y0 + (int32_t)rows;
		quad.u0 = 0;
		quad.v0 = 0;
		quad.u1 = (int32_t)region->imageExtent.width;
		quad.v1 = (int32_t)rows;
		error = amdrdna_vk_state_depth_target(&execute->pm4, &surface);
		if (error == 0)
			error = execute_quad(execute, &quad);
		if (error != 0)
			return error;

		/* The scratch is moved into again only after the quad read it. */
		execute_barrier(execute);
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Copies one aspect of a depth image's region into a buffer (its rows
 * from start, pitch bytes apart), band by band: a quad packs the bits of
 * the band's texels into the scratch as a colour target of four-byte
 * pixels (EXECUTE_PACK_BYTES / the texels' bytes texels a pixel, rows of
 * as many pixels as the region's width needs), then the command processor
 * moves each of its rows' texels' bytes into the buffer.  The band's quad
 * and moves go in one indirect buffer, with a barrier between them (the
 * moves wait for one another, so the next band's quad starts after them).
 */
static int
execute_depth_out(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	const struct amdrdna_vk_image *image,
	uint32_t aspect,
	uint64_t start,
	uint32_t pitch,
	const VkBufferImageCopy *region)
{
	struct execute_quad quad;
	uint64_t scratch;
	uint32_t texel_bytes;
	uint32_t texels;
	uint32_t pixels;
	uint32_t bytes;
	uint32_t scratch_pitch;
	uint32_t band;
	uint32_t rows;
	uint32_t first;
	uint32_t y;
	int error;

	/* The pixels of a row in the scratch: the region's texels packed by the pixel's bytes. */
	texel_bytes = execute_aspect_bytes(image, aspect);
	texels = EXECUTE_PACK_BYTES / texel_bytes;
	pixels = (region->imageExtent.width + texels - 1U) / texels;
	bytes = region->imageExtent.width * texel_bytes;
	scratch_pitch = amdrdna_vk_pitch(pixels);
	band = EXECUTE_SCRATCH_BYTES / scratch_pitch;

	/*
	 * The quad: the scratch a colour target of four 8-bit channels, the
	 * aspect's bits read from the region's first texel (the push
	 * constants' first word) and row (the texture rectangle's).
	 */
	kern_memset(&quad, 0, sizeof(quad));
	quad.target.format = VK_FORMAT_R8G8B8A8_UNORM;
	quad.target.samples = 1U;
	quad.target.layers = 1U;
	quad.described = true;
	quad.filter = VK_FILTER_NEAREST;
	quad.program = EXECUTE_QUAD_PACK_1;
	if (texels == 2U)
		quad.program = EXECUTE_QUAD_PACK_2;
	if (texels == 4U)
		quad.program = EXECUTE_QUAD_PACK_4;
	quad.colour[0] = (uint32_t)region->imageOffset.x;
	error = amdrdna_vk_depth_descriptor(image, aspect, true, NULL, quad.source_words);
	if (error != 0)
		return error;

	/* Each band of rows. */
	for (first = 0U; first < region->imageExtent.height; first += rows) {
		rows = region->imageExtent.height - first;
		if (rows > band)
			rows = band;

		/* Room for the band in this indirect buffer, whose slot's scratch it uses. */
		error = execute_band_room(session, execute, state);
		if (error != 0)
			return error;
		scratch = execute->va + EXECUTE_SCRATCH_AT;

		/* The quad over the scratch's rows, the band's rows of the image. */
		quad.target.address = scratch;
		quad.target.width = pixels;
		quad.target.height = rows;
		quad.x0 = 0;
		quad.y0 = 0;
		quad.x1 = (int32_t)pixels;
		quad.y1 = (int32_t)rows;
		quad.u0 = 0;
		quad.v0 = region->imageOffset.y + (int32_t)first;
		quad.u1 = (int32_t)pixels;
		quad.v1 = quad.v0 + (int32_t)rows;
		error = execute_quad(execute, &quad);
		if (error != 0)
			return error;
		execute_barrier(execute);

		/* Each row's texels moved into the buffer (at once when the rows fill both pitches, never between the buffer's rows). */
		if (pitch == scratch_pitch && bytes == pitch) {
			error = execute_move(session, execute, state, scratch, start + (uint64_t)first * pitch,
					     (uint64_t)scratch_pitch * (rows - 1U) + bytes);
			if (error != 0)
				return error;
		} else {
			for (y = 0U; y < rows; y++) {
				error = execute_move(session, execute, state, scratch + (uint64_t)y * scratch_pitch,
						     start + (uint64_t)(first + y) * pitch, bytes);
				if (error != 0)
					return error;
			}
		}
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Makes room for one band of a depth image's copy in the indirect buffer
 * being written: its moves (a band's rows, fewer than a record's room of
 * words), its quad's tables and its barriers, so that no move of it runs
 * the buffer in the middle (the scratch is the slot's).  The target is
 * written again in the next buffer.
 */
static int
execute_band_room(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_execute *execute,
	struct execute_state *state)
{
	bool full;
	int error;

	/* Full when the buffer could not take two records' words, or the tables a quad's two. */
	full = false;
	if (execute->pm4.count + 2U * EXECUTE_RECORD_WORDS > EXECUTE_BUFFER_WORDS)
		full = true;
	if (execute->tables + 2U * EXECUTE_TABLE_MAX > EXECUTE_TABLES_BYTES)
		full = true;
	if (!full)
		return 0;

	/* What there is goes on the queue with its barrier, and the next buffer starts. */
	error = execute_flush(session, execute);
	if (error == 0)
		error = execute_start(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the next buffer has room; its target is written anew. */
	state->target_written = false;
	return 0;
}

/*
 * Clears the aspects of a depth image its ranges name
 * (vkCmdClearDepthStencilImage, the image's one level and layer): a quad
 * over the whole image clears its depth to the depth value (value 0, its
 * float's bits) and its stencil to the stencil value (value 1), as a
 * pass's clear does, every sample of an image of samples.  Returns 0, or
 * EINVAL for a level or layer the image does not have.
 */
static int
execute_clear_depth(
	struct amdrdna_vk_execute *execute,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *image;
	const VkImageSubresourceRange *ranges;
	struct amdrdna_vk_target surface;
	struct execute_quad quad;
	uint32_t index;
	int error;

	image = record->objects[0]->payload;
	ranges = record->regions;

	/* The image's depth surface, cleared by quads of no colour over all of it. */
	error = execute_depth_target(image, &surface);
	if (error != 0)
		return error;
	kern_memset(&quad, 0, sizeof(quad));
	quad.target.width = image->width;
	quad.target.height = image->height;
	quad.target.samples = image->samples;
	quad.colour_off = true;
	quad.x1 = (int32_t)image->width;
	quad.y1 = (int32_t)image->height;
	quad.depth_value = record->values[0];
	quad.stencil_value = record->values[1] & EXECUTE_STENCIL_ALL;

	/* Each range: the image's level and layer, the aspects it names. */
	for (index = 0U; index < record->region_count; index++) {
		if (ranges[index].baseMipLevel != 0U || ranges[index].baseArrayLayer != 0U)
			return EINVAL;
		quad.depth_clear = false;
		if ((ranges[index].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U)
			quad.depth_clear = true;
		quad.stencil_clear = false;
		if ((ranges[index].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U && image->stencil)
			quad.stencil_clear = true;
		if (!quad.depth_clear && !quad.stencil_clear)
			continue;
		error = amdrdna_vk_state_depth_target(&execute->pm4, &surface);
		if (error == 0)
			error = execute_quad(execute, &quad);
		if (error != 0)
			return error;
	}

	/* Succeeded: the clear is written. */
	return 0;
}

/*
 * Describes a depth image as the depth unit's surface: the image whole,
 * its stencil plane's address when it has one.  EIO without storage.
 */
static int
execute_depth_target(
	const struct amdrdna_vk_image *image,
	struct amdrdna_vk_target *target)
{
	uint8_t *cpu;
	int error;

	/* The image's storage. */
	kern_memset(target, 0, sizeof(*target));
	error = amdrdna_vk_image_address(image, &target->address, &cpu);
	if (error != 0)
		return error;

	/* Succeeded: the surface is the image, its stencil plane after its depth. */
	target->width = image->width;
	target->height = image->height;
	target->format = image->format;
	target->samples = image->samples;
	target->slice = 0U;
	target->layers = 1U;
	if (image->stencil)
		target->stencil_address = target->address + image->stencil_offset;
	return 0;
}

/* Reports the quad program that writes an aspect of a depth image: its stencil, its 16-bit depth, or its 32-bit float depth. */
static uint32_t
execute_depth_program(
	const struct amdrdna_vk_image *image,
	uint32_t aspect)
{
	/* The stencil, else the depth of the image's format. */
	if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT)
		return EXECUTE_QUAD_STENCIL;
	if (image->format == VK_FORMAT_D16_UNORM)
		return EXECUTE_QUAD_DEPTH_16;
	return EXECUTE_QUAD_DEPTH_32;
}

/* Reports the bytes of a texel of an aspect of a depth image: the stencil's one, a 16-bit depth's two, a 32-bit float depth's four. */
static uint32_t
execute_aspect_bytes(
	const struct amdrdna_vk_image *image,
	uint32_t aspect)
{
	/* The stencil, else the depth of the image's format. */
	if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT)
		return 1U;
	if (image->format == VK_FORMAT_D16_UNORM)
		return 2U;
	return 4U;
}

/*
 * Resolves regions of an image of samples into an image of one sample
 * (vkCmdResolveImage, a region a slice): each region's samples averaged
 * by a quad over the destination's region of its level and layer, from
 * the source's layer at its offset.  Returns 0, or EINVAL for a region
 * outside an image or a subresource it does not have.
 */
static int
execute_resolve_images(
	struct amdrdna_vk_execute *execute,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *source;
	const struct amdrdna_vk_image *destination;
	const VkImageResolve *regions;
	struct amdrdna_vk_target target;
	uint32_t index;
	int error;

	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;
	regions = record->regions;

	/* Each region: the destination's level and layer, the source's layer, the quad. */
	for (index = 0U; index < record->region_count; index++) {
		error = execute_slice_target(destination, destination->format, regions[index].dstSubresource.mipLevel,
					     regions[index].dstSubresource.baseArrayLayer, &target);
		if (error != 0)
			return error;
		error = execute_resolve_quad(execute, source, regions[index].srcSubresource.baseArrayLayer, regions[index].srcOffset.x,
					     regions[index].srcOffset.y, &target, regions[index].dstOffset.x, regions[index].dstOffset.y,
					     regions[index].extent.width, regions[index].extent.height);
		if (error != 0)
			return error;
	}

	/* Succeeded: the regions are resolved. */
	return 0;
}

/*
 * Resolves the pass that ended: each colour attachment with a resolve
 * attachment, over the render area (within the targets), into the
 * resolve attachment's view, and the depth attachment's sample 0 when the
 * subpass resolves it; then what follows reads what the quads wrote.  Nothing for a pass without a resolve or without a target.
 *
 * Procedure referenced: the Vulkan 1.3 specification (VkSubpassDescription:
 * pResolveAttachments resolved at the end of the subpass, over the render
 * area) and Mesa 25.0.7 src/amd/vulkan/meta/radv_meta_resolve_fs.c (the
 * fragment resolve: the samples of the source's pixel averaged).
 */
static int
execute_pass_resolves(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state)
{
	const struct amdrdna_vk_render_pass *pass;
	const struct amdrdna_vk_view *source;
	const struct amdrdna_vk_view *destination;
	struct amdrdna_vk_target target;
	uint32_t index;
	uint32_t resolved;
	int32_t x1;
	int32_t y1;
	int error;

	/* A pass with a target and a render area within it. */
	pass = state->pass;
	if (pass == NULL || state->width == 0U || state->height == 0U)
		return 0;
	x1 = state->area.offset.x + (int32_t)state->area.extent.width;
	y1 = state->area.offset.y + (int32_t)state->area.extent.height;
	if (x1 > (int32_t)state->width)
		x1 = (int32_t)state->width;
	if (y1 > (int32_t)state->height)
		y1 = (int32_t)state->height;
	if (state->area.offset.x < 0 || state->area.offset.y < 0 || x1 <= state->area.offset.x || y1 <= state->area.offset.y)
		return 0;

	/* Each colour attachment resolved: its view into the resolve attachment's. */
	resolved = 0U;
	for (index = 0U; index < pass->colour_count; index++) {
		if (pass->colour_attachments[index] == VK_ATTACHMENT_UNUSED || pass->resolve_attachments[index] == VK_ATTACHMENT_UNUSED)
			continue;
		error = execute_pass_view(state->framebuffer, pass->colour_attachments[index], &source);
		if (error == 0)
			error = execute_pass_view(state->framebuffer, pass->resolve_attachments[index], &destination);
		if (error == 0)
			error = execute_slice_target(destination->image->payload, destination->format, destination->base_level,
						     destination->base_layer, &target);
		if (error == 0)
			error = execute_resolve_quad(execute, source->image->payload, source->base_layer, state->area.offset.x,
						     state->area.offset.y, &target, state->area.offset.x, state->area.offset.y,
						     (uint32_t)(x1 - state->area.offset.x), (uint32_t)(y1 - state->area.offset.y));
		if (error != 0)
			return error;
		resolved++;
	}

	/* The depth attachment's sample 0, when the subpass resolves it. */
	if (pass->depth != 0U && pass->depth_resolve_attachment != VK_ATTACHMENT_UNUSED) {
		error = execute_pass_depth_resolve(execute, state, x1, y1);
		if (error != 0)
			return error;
		resolved++;
	}

	/* What follows reads the resolved images; a later pass writes its targets again. */
	if (resolved != 0U) {
		execute_barrier(execute);
		state->target_written = false;
	}

	/* Succeeded: the pass's attachments are resolved. */
	return 0;
}

/*
 * Resolves sample 0 of the pass's depth attachment (its depth, and its
 * stencil when its format has one: the device reports no independent
 * resolve) into the resolve attachment over the render area up to
 * (x1, y1): for each aspect a quad over the area of the resolve
 * attachment's depth surface fetches sample 0's bits of the attachment's
 * texel and exports them as the depth or as the stencil reference, as a
 * copy between depth images does.  Returns 0, or EINVAL for views that
 * are not a depth image of samples and one of one sample of its format.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/meta/radv_meta_resolve_fs.c
 * (radv_depth_stencil_resolve_rendering_fs: the depth, then the stencil,
 * each drawn into the destination's aspect by a fragment program that
 * fetches the source's sample 0 for VK_RESOLVE_MODE_SAMPLE_ZERO_BIT).
 */
static int
execute_pass_depth_resolve(
	struct amdrdna_vk_execute *execute,
	struct execute_state *state,
	int32_t x1,
	int32_t y1)
{
	const struct amdrdna_vk_render_pass *pass;
	const struct amdrdna_vk_view *source_view;
	const struct amdrdna_vk_view *destination_view;
	const struct amdrdna_vk_image *source;
	const struct amdrdna_vk_image *destination;
	struct amdrdna_vk_target surface;
	struct execute_quad quad;
	uint32_t aspect;
	int error;

	/* The two views: a depth image of samples, into one of one sample of its format. */
	pass = state->pass;
	error = execute_pass_view(state->framebuffer, pass->depth_attachment, &source_view);
	if (error == 0)
		error = execute_pass_view(state->framebuffer, pass->depth_resolve_attachment, &destination_view);
	if (error != 0)
		return error;
	source = source_view->image->payload;
	destination = destination_view->image->payload;
	if (!source->depth || !destination->depth || source->samples <= 1U || destination->samples > 1U ||
	    source->format != destination->format)
		return EINVAL;
	if (x1 > (int32_t)destination->width || y1 > (int32_t)destination->height || x1 > (int32_t)source->width ||
	    y1 > (int32_t)source->height)
		return EINVAL;

	/* The resolve attachment's depth surface, drawn into by quads of no colour over the area. */
	error = execute_depth_target(destination, &surface);
	if (error != 0)
		return error;
	kern_memset(&quad, 0, sizeof(quad));
	quad.target.width = destination->width;
	quad.target.height = destination->height;
	quad.target.samples = 1U;
	quad.colour_off = true;
	quad.described = true;
	quad.filter = VK_FILTER_NEAREST;
	quad.x0 = state->area.offset.x;
	quad.y0 = state->area.offset.y;
	quad.x1 = x1;
	quad.y1 = y1;
	quad.u0 = quad.x0;
	quad.v0 = quad.y0;
	quad.u1 = x1;
	quad.v1 = y1;

	/* A quad for the depth, then one for the stencil when the format has it. */
	for (aspect = VK_IMAGE_ASPECT_DEPTH_BIT; aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) {
		if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT && !source->stencil)
			continue;
		error = amdrdna_vk_depth_descriptor(source, aspect, true, NULL, quad.source_words);
		if (error != 0)
			return error;
		quad.program = EXECUTE_QUAD_SAMPLE_ZERO + (execute_depth_program(destination, aspect) - EXECUTE_QUAD_DEPTH_32);
		quad.depth_write = (aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
		quad.stencil_write = (aspect == VK_IMAGE_ASPECT_STENCIL_BIT);
		error = amdrdna_vk_state_depth_target(&execute->pm4, &surface);
		if (error == 0)
			error = execute_quad(execute, &quad);
		if (error != 0)
			return error;
	}

	/* Succeeded: sample 0 is in the resolve attachment. */
	return 0;
}

/*
 * Draws the quad of a resolve: the destination target's region from
 * (x, y), width by height, the average of the samples of the source
 * image's layer from (source_x, source_y).  Returns 0, or EINVAL for an
 * image of one sample, a layer it does not have or a region outside an
 * image.
 */
static int
execute_resolve_quad(
	struct amdrdna_vk_execute *execute,
	const struct amdrdna_vk_image *source,
	uint32_t layer,
	int32_t source_x,
	int32_t source_y,
	const struct amdrdna_vk_target *target,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height)
{
	struct execute_quad quad;
	uint8_t *cpu;
	bool inside;
	int error;

	/* An image of samples, its layer and the region inside it. */
	if (source->samples <= 1U || layer >= source->slices)
		return EINVAL;
	inside = execute_region_inside(source_x, source_y, width, height, source->width, source->height);
	if (!inside)
		return EINVAL;

	/* The quad: the destination's region, the source's samples there. */
	kern_memset(&quad, 0, sizeof(quad));
	error = amdrdna_vk_image_address(source, &quad.source, &cpu);
	if (error != 0)
		return error;
	quad.target = *target;
	quad.x0 = x;
	quad.y0 = y;
	quad.x1 = x + (int32_t)width;
	quad.y1 = y + (int32_t)height;
	quad.resolve = true;
	quad.source_width = source->width;
	quad.source_height = source->height;
	quad.source_format = source->format;
	quad.source_samples = source->samples;
	quad.source_layer = layer;
	quad.source_arrayed = source->slices > 1U;
	quad.u0 = source_x;
	quad.v0 = source_y;
	quad.u1 = source_x + (int32_t)width;
	quad.v1 = source_y + (int32_t)height;
	error = execute_quad(execute, &quad);
	if (error != 0)
		return error;

	/* Succeeded: the region is resolved. */
	return 0;
}

/*
 * Draws one internal quad: the destination as the target, the viewport
 * and scissor its region, no blending, the corners, and the executor's
 * programs with their tables (the quad over the whole viewport, and the
 * source's region in pixels with its descriptors, the colour, or both
 * for a packing program).
 */
static int
execute_quad(
	struct amdrdna_vk_execute *execute,
	const struct execute_quad *quad)
{
	static const uint32_t identity[4] = {
		VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A
	};
	const struct amdrdna_vk_program *vertex;
	const struct amdrdna_vk_program *fragment;
	struct amdrdna_vk_depth_stencil depth;
	struct amdrdna_vk_sampler sampler;
	struct amdrdna_vk_blend blend;
	uint32_t image_words[AMDRDNA_IMAGE_WORDS];
	uint32_t sampler_words[AMDRDNA_SAMPLER_WORDS];
	uint32_t corners[4];
	uint32_t viewport[6];
	uint8_t *vertex_table;
	uint8_t *fragment_table;
	uint64_t vertex_va;
	uint64_t fragment_va;
	uint32_t component;
	uint32_t kind;
	bool inside;
	int error;

	/* A region inside the target; nothing to draw for an empty one. */
	if (quad->x1 <= quad->x0 || quad->y1 <= quad->y0)
		return 0;
	inside = execute_region_inside(quad->x0, quad->y0, (uint32_t)(quad->x1 - quad->x0), (uint32_t)(quad->y1 - quad->y0),
				       quad->target.width, quad->target.height);
	if (!inside)
		return EINVAL;

	/* The programs and their tables. */
	vertex = &execute->quad[EXECUTE_QUAD_VERTEX];
	fragment = &execute->quad[EXECUTE_QUAD_COLOUR];
	kind = amdrdna_vk_target_export(quad->target.format, false);
	if (kind != AMDRDNA_GFX10_COLOUR_HALVES)
		fragment = &execute->quad[EXECUTE_QUAD_COLOUR_WIDE + kind - AMDRDNA_GFX10_COLOUR_R];
	if (quad->textured)
		fragment = &execute->quad[EXECUTE_QUAD_TEXTURED];
	if (quad->resolve)
		fragment = &execute->quad[EXECUTE_QUAD_RESOLVE];
	if (quad->program != 0U)
		fragment = &execute->quad[quad->program];
	error = execute_table(execute, vertex->compiled, true, &vertex_table, &vertex_va);
	if (error == 0)
		error = execute_table(execute, fragment->compiled, true, &fragment_table, &fragment_va);
	if (error != 0)
		return error;

	/* The vertex program's push constants: the quad from -1 to 1, the source's region in pixels; the corners at location 0. */
	execute_put32(vertex_table, AMDRDNA_QUAD_PUSH_RECT, EXECUTE_FLOAT_MINUS_ONE);
	execute_put32(vertex_table, AMDRDNA_QUAD_PUSH_RECT + 4U, EXECUTE_FLOAT_MINUS_ONE);
	execute_put32(vertex_table, AMDRDNA_QUAD_PUSH_RECT + 8U, EXECUTE_FLOAT_ONE);
	execute_put32(vertex_table, AMDRDNA_QUAD_PUSH_RECT + 12U, EXECUTE_FLOAT_ONE);
	execute_put32(vertex_table, AMDRDNA_QUAD_PUSH_TEXTURE, amdrdna_vk_float(quad->u0));
	execute_put32(vertex_table, AMDRDNA_QUAD_PUSH_TEXTURE + 4U, amdrdna_vk_float(quad->v0));
	execute_put32(vertex_table, AMDRDNA_QUAD_PUSH_TEXTURE + 8U, amdrdna_vk_float(quad->u1));
	execute_put32(vertex_table, AMDRDNA_QUAD_PUSH_TEXTURE + 12U, amdrdna_vk_float(quad->v1));
	amdrdna_gfx10_vertex_descriptor(execute->va + EXECUTE_CORNERS_AT, EXECUTE_CORNER_STRIDE, EXECUTE_CORNERS, AMDRDNA_GFX10_VERTEX_FLOAT2,
					2U, corners);
	execute_put_words(vertex_table, vertex->compiled->vertex_table_offset, corners, 4U);

	/*
	 * The fragment program's: the source's descriptors (the caller's, or
	 * made here; a sampler in pixels, clamped; an image of samples read as
	 * one), the colour, or both.
	 */
	if (quad->described) {
		kern_memcpy(image_words, quad->source_words, sizeof(image_words));
	} else if (quad->textured || quad->resolve) {
		error = amdrdna_vk_image_descriptor(quad->source, quad->source_width, quad->source_height, quad->source_format, identity,
						    image_words);
		if (error != 0)
			return error;
		if (quad->resolve)
			amdrdna_vk_image_descriptor_samples(image_words, quad->source_samples, quad->source_arrayed, quad->source_layer, 1U);
	}

	/* The image and its sampler in the fragment program's table. */
	if (quad->described ||
	    quad->textured ||
	    quad->resolve) {
		kern_memset(&sampler, 0, sizeof(sampler));
		sampler.mag_filter = quad->filter;
		sampler.min_filter = quad->filter;
		sampler.address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.unnormalized = 1U;
		amdrdna_vk_sampler_descriptor(&sampler, sampler_words);
		execute_put_words(fragment_table, fragment->compiled->textures[0].table_offset, image_words, AMDRDNA_IMAGE_WORDS);
		execute_put_words(fragment_table, fragment->compiled->textures[0].table_offset + AMDRDNA_IMAGE_WORDS * 4U, sampler_words,
				  AMDRDNA_SAMPLER_WORDS);
	}

	/* The colour in its push constants: a one-colour program's, or a packing program's first texel. */
	if ((quad->program == 0U && !quad->textured && !quad->resolve) || quad->program >= EXECUTE_QUAD_PACK_1) {
		for (component = 0U; component < 4U; component++)
			execute_put32(fragment_table, AMDRDNA_QUAD_PUSH_COLOUR + component * 4U, quad->colour[component]);
	}

	/*
	 * The target with its samples (every one written), no depth surface
	 * unless the quad clears or writes it (one of the same samples, set by
	 * the caller), the viewport (its depth range the clear depth, for a
	 * clear of depth) and the scissor its region.
	 */
	error = amdrdna_vk_state_target(&execute->pm4, &quad->target);
	if (error == 0 &&
	    !quad->depth_clear &&
	    !quad->stencil_clear &&
	    !quad->depth_write &&
	    !quad->stencil_write)
		error = amdrdna_vk_state_depth_target(&execute->pm4, NULL);
	if (error != 0)
		return error;
	amdrdna_vk_state_samples(&execute->pm4, quad->target.samples, EXECUTE_SAMPLE_MASK_ALL, false);
	execute_full_viewport(viewport, quad->x0, quad->y0, quad->x1, quad->y1);
	if (quad->depth_clear) {
		viewport[4] = quad->depth_value;
		viewport[5] = quad->depth_value;
	}
	amdrdna_vk_state_viewport(&execute->pm4, viewport);
	amdrdna_vk_state_guard_band(&execute->pm4, viewport, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	amdrdna_vk_state_scissor(&execute->pm4, (uint32_t)quad->x0, (uint32_t)quad->y0, (uint32_t)quad->x1, (uint32_t)quad->y1);

	/* No blending, every channel (none for a clear of depth alone), no culling. */
	kern_memset(&blend, 0, sizeof(blend));
	blend.write_mask = 0xfU;
	if (quad->colour_off)
		blend.write_mask = 0U;
	error = amdrdna_vk_state_blend(&execute->pm4, &blend);
	if (error != 0)
		return error;
	amdrdna_vk_state_raster(&execute->pm4, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false);

	/*
	 * The depth written everywhere for a clear of depth or a write of the
	 * program's, the stencil replaced everywhere by the clear value for a
	 * clear of stencil or by the program's reference for its write
	 * (whatever the masks, as a clear is); both left alone by any other
	 * quad.
	 */
	kern_memset(&depth, 0, sizeof(depth));
	depth.compare = AMDRDNA_CX_DEPTH_COMPARE_ALWAYS;
	if (quad->depth_clear || quad->depth_write) {
		depth.test = 1U;
		depth.write = 1U;
	}
	if (quad->stencil_clear || quad->stencil_write) {
		depth.stencil = 1U;
		depth.front.fail = VK_STENCIL_OP_REPLACE;
		depth.front.pass = VK_STENCIL_OP_REPLACE;
		depth.front.depth_fail = VK_STENCIL_OP_REPLACE;
		depth.front.compare = VK_COMPARE_OP_ALWAYS;
		depth.front.compare_mask = EXECUTE_STENCIL_ALL;
		depth.front.write_mask = EXECUTE_STENCIL_ALL;
		depth.front.reference = quad->stencil_value;
		depth.back = depth.front;
	}
	amdrdna_vk_state_depth(&execute->pm4, &depth);

	/* The programs, two triangles. */
	amdrdna_vk_state_programs(&execute->pm4, vertex, fragment, vertex_va, fragment_va, 0U, 0U, 0U, NULL);
	error = amdrdna_vk_state_draw(&execute->pm4,
				     VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				     vertex->registers.primitive_shader != 0U,
				     EXECUTE_CORNERS,
				     1U);
	if (error != 0)
		return error;
	execute->quads++;

	/* Succeeded: the quad is written. */
	return 0;
}

/*
 * Moves a part of an image (a level, or a plane) to the slice a region
 * names of it: the z of a 3D image's region, else its subresource's first
 * layer (a region of several slices was made one a slice when it was
 * recorded).  Returns 0, or EINVAL for a slice the level does not have.
 */
static int
execute_region_slice(
	const struct amdrdna_vk_image *image,
	const VkImageSubresourceLayers *layers,
	int32_t z,
	struct amdrdna_vk_image *part)
{
	int error;

	/* A 3D image's depth slice, or a layer. */
	if (image->type == VK_IMAGE_TYPE_3D) {
		if (z < 0)
			return EINVAL;
		error = amdrdna_vk_image_slice(image, layers->mipLevel, (uint32_t)z, part);
	} else {
		error = amdrdna_vk_image_slice(image, layers->mipLevel, layers->baseArrayLayer, part);
	}
	if (error != 0)
		return error;

	/* Succeeded: the part is the slice's. */
	return 0;
}

/*
 * Describes an image as a colour target in a format; EIO without storage,
 * EINVAL for a format the executor does not draw into.
 */
static int
execute_image_target(
	const struct amdrdna_vk_image *image,
	uint32_t format,
	struct amdrdna_vk_target *target)
{
	uint8_t *cpu;
	uint32_t layout;
	int error;

	/* The image's storage. */
	error = amdrdna_vk_image_address(image, &target->address, &cpu);
	if (error != 0)
		return error;

	/* A format drawn into. */
	if (!amdrdna_vk_target_format(format, &layout))
		return EINVAL;

	/* Succeeded: the target is the image (a linear one of one sample). */
	target->width = image->width;
	target->height = image->height;
	target->format = format;
	target->samples = 1U;
	target->slice = 0U;
	target->layers = 1U;
	target->stencil_address = 0U;
	return 0;
}

/*
 * Describes a level and a slice of an image as a colour target in a
 * format: a linear image's level of the slice, or an image of samples
 * (its one level) from its start with the slice drawn.  EIO without
 * storage, EINVAL for a level or slice the image does not have or a format
 * the executor does not draw into.
 */
static int
execute_slice_target(
	const struct amdrdna_vk_image *image,
	uint32_t format,
	uint32_t level,
	uint32_t slice,
	struct amdrdna_vk_target *target)
{
	struct amdrdna_vk_image part;
	int error;

	/* An image of one sample: the level of the slice, linear. */
	if (image->samples <= 1U) {
		error = amdrdna_vk_image_level(image, level, &part);
		if (error == 0)
			error = amdrdna_vk_image_slice(image, level, slice, &part);
		if (error == 0)
			error = execute_image_target(&part, format, target);
		if (error != 0)
			return error;

		/* Succeeded: the target is the level's slice. */
		return 0;
	}

	/* An image of samples: its start, the slice drawn. */
	if (level != 0U || slice >= image->slices)
		return EINVAL;
	error = execute_image_target(image, format, target);
	if (error != 0)
		return error;
	target->samples = image->samples;
	target->slice = slice;
	target->layers = image->slices;

	/* Succeeded: the target is the slice of the image of samples. */
	return 0;
}

/* Reports whether a region lies inside an image of a size. */
static bool
execute_region_inside(
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t image_width,
	uint32_t image_height)
{
	/* On the image, not past its edges. */
	if (x < 0 || y < 0)
		return false;
	if ((uint32_t)x > image_width || width > image_width - (uint32_t)x)
		return false;
	if ((uint32_t)y > image_height || height > image_height - (uint32_t)y)
		return false;

	/* Succeeded: the region is inside. */
	return true;
}

/* Fills a VkViewport's floats (as bits) for a whole-pixel rectangle, depths 0 to 1. */
static void
execute_full_viewport(
	uint32_t *viewport,
	int32_t x0,
	int32_t y0,
	int32_t x1,
	int32_t y1)
{
	/* x, y, width, height, minimum depth, maximum depth. */
	viewport[0] = amdrdna_vk_float(x0);
	viewport[1] = amdrdna_vk_float(y0);
	viewport[2] = amdrdna_vk_float(x1 - x0);
	viewport[3] = amdrdna_vk_float(y1 - y0);
	viewport[4] = 0U;
	viewport[5] = EXECUTE_FLOAT_ONE;
}

/* Writes a 32-bit word at a byte offset (the GPU reads it little-endian). */
static void
execute_put32(
	uint8_t *cpu,
	uint32_t at,
	uint32_t value)
{
	/* The four bytes, low first. */
	cpu[at] = (uint8_t)value;
	cpu[at + 1U] = (uint8_t)(value >> 8);
	cpu[at + 2U] = (uint8_t)(value >> 16);
	cpu[at + 3U] = (uint8_t)(value >> 24);
}

/* Writes count 32-bit words from a byte offset. */
static void
execute_put_words(
	uint8_t *cpu,
	uint32_t at,
	const uint32_t *words,
	uint32_t count)
{
	uint32_t index;

	/* Each word in turn. */
	for (index = 0U; index < count; index++)
		execute_put32(cpu, at + index * 4U, words[index]);
}

/*
 * Compares the trial's two images (pitch bytes a row, the second after
 * the first) with what the draws should have left: blue with the red
 * square in both.
 */
static void
execute_trial_compare(
	const uint8_t *cpu,
	uint32_t pitch,
	struct amdrdna_vk_trial *trial)
{
	uint32_t image;
	uint32_t x;
	uint32_t y;
	uint32_t at;
	uint32_t expected;
	uint32_t value;

	/* Each pixel of each image. */
	for (image = 0U; image < 2U; image++) {
		for (y = 0U; y < EXECUTE_TRIAL_SIDE; y++) {
			for (x = 0U; x < EXECUTE_TRIAL_SIDE; x++) {
				/* Red inside the square, blue around it. */
				expected = EXECUTE_TRIAL_BLUE;
				if (x >= EXECUTE_TRIAL_INNER_FROM && x < EXECUTE_TRIAL_INNER_TO) {
					if (y >= EXECUTE_TRIAL_INNER_FROM && y < EXECUTE_TRIAL_INNER_TO)
						expected = EXECUTE_TRIAL_RED;
				}

				/* The pixel as the GPU left it. */
				at = (image * EXECUTE_TRIAL_SIDE + y) * pitch + x * 4U;
				value = execute_get32(cpu, at);

				/* A pixel as expected counts for its image; the first wrong one is kept. */
				if (value == expected && image == 0U) {
					trial->drawn++;
				} else if (value == expected) {
					trial->copied++;
				} else if (trial->bad_at == trial->pixels * 2U) {
					trial->bad_at = (image * EXECUTE_TRIAL_SIDE + y) * EXECUTE_TRIAL_SIDE + x;
					trial->bad_value = value;
				}
			}
		}
	}
}

/* Reads a 32-bit word at a byte offset (little-endian, as the GPU wrote it). */
static uint32_t
execute_get32(
	const uint8_t *cpu,
	uint32_t at)
{
	/* The four bytes, low first. */
	return (uint32_t)cpu[at] | ((uint32_t)cpu[at + 1U] << 8) | ((uint32_t)cpu[at + 2U] << 16) | ((uint32_t)cpu[at + 3U] << 24);
}
