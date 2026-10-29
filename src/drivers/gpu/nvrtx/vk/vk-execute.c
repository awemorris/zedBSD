/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The running of a command buffer's records on the GPU.
 *
 * Each session has up to EXECUTE_SLOTS runs (slots) of the executor's GPU
 * memory for its submissions, each with a push buffer, a constant buffer 0
 * for each draw, the texture header and sampler pools, a zero page and the
 * corners of a quad.  The records are turned into methods in order
 * (exec-push.c and draw-push.c) and the push buffer is put on the work
 * channel when it is full, when a copy must be done by the CPU, and at the
 * end, without waiting for the GPU: the next push buffer is written in a
 * slot whose run has ended (a new slot is made while one is to be had,
 * else the oldest run is waited for), so the CPU writes one while the GPU
 * runs the others.  The runs on the one work channel run in order, each
 * after the one before has finished (its fence waits for idle).  What
 * needs the GPU done with the session's work first waits for its newest
 * run: a copy by the CPU, a shadow given back, the session's close (and,
 * in the node, a memory or a run of the executor's memory given back).
 *
 * A draw writes everything it uses: its constants (the push constants and
 * its texture handles) in a constant buffer of its own, a header and a
 * sampler for each sampled image, the viewport, scissor, blend, vertex
 * input and programs.  A copy, a blit, a copy between a buffer and an
 * image, and a clear are drawn too: a quad of the executor's own programs
 * samples the source (in pixel coordinates) into the destination.  A copy
 * whose buffer is not laid out as a texture or a target can be (its start
 * or row pitch off their boundaries) is done by the CPU through the BAR1
 * window.  Between a pass or a transfer and what follows, the GPU waits
 * for its idle and its texture and constant caches are invalidated.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-exec.h"
#include "drivers/gpu/nvrtx/nvrtx-draw.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/compiler/sm75/sm75.h"

/*
 * The runs (slots) a session writes its submissions in at most: one being
 * written while the GPU runs the others.
 */
#define EXECUTE_SLOTS			3U

/* A slot's run: its size and where each of its parts starts. */
#define EXECUTE_RUN_BYTES		(2U * 1024U * 1024U)
#define EXECUTE_PUSH_AT			0U
#define EXECUTE_PUSH_BYTES		(1024U * 1024U)
#define EXECUTE_CONSTANTS_AT		(1024U * 1024U)
#define EXECUTE_CONSTANTS_BYTES		(512U * 1024U)
#define EXECUTE_HEADERS_AT		(1536U * 1024U)
#define EXECUTE_SAMPLERS_AT		(1600U * 1024U)
#define EXECUTE_ZERO_AT			(1664U * 1024U)
#define EXECUTE_CORNERS_AT		(1668U * 1024U)

/*
 * The headers and the samplers of one push buffer, each eight words.
 *
 * Value referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597tex.h (the 256-bit
 * NVC597_TEXHEAD_PITCH_* and NVC597_TEXSAMP* layouts).
 */
#define EXECUTE_POOL_ENTRIES		2048U
#define EXECUTE_POOL_ENTRY_BYTES	32U

/* One draw's constant buffer 0 (a whole number of 256-byte units): the push constants, then the texture handles; its slot in the run. */
#define EXECUTE_CONSTANTS_SIZE		0x500U
#define EXECUTE_CONSTANTS_SLOT		0x500U

/* What one record writes at most into the push buffer (a transfer of NVRTX_VK_REGIONS_MAX quads), before the run is started to make room. */
#define EXECUTE_RECORD_ROOM		32768U

/*
 * A texture handle: the header's index, and the sampler's from bit 20.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_descriptor_types.h
 * (nvk_sampled_image_descriptor).
 */
#define EXECUTE_HANDLE_SAMPLER_SHIFT	20U

/*
 * The program groups of the vertex and the fragment programs, and a
 * uniform buffer: its start on 256 bytes (minUniformBufferOffsetAlignment),
 * its size in 16-byte units up to 64 KiB (maxUniformBufferRange).
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_shader.h
 * (nvk_cbuf_binding_for_stage: the group is the stage's number).
 */
#define EXECUTE_GROUP_VERTEX		0U
#define EXECUTE_GROUP_FRAGMENT		4U
#define EXECUTE_UNIFORM_ALIGN		256U
#define EXECUTE_UNIFORM_UNIT		16U
#define EXECUTE_UNIFORM_BYTES_MAX	65536U

/* The quad's corners: two triangles of six corners, two floats each. */
#define EXECUTE_CORNERS			6U
#define EXECUTE_CORNER_STRIDE		8U

/* The float bits of -1 and 1, and the bytes of an internal draw's push constants. */
#define EXECUTE_FLOAT_MINUS_ONE		0xbf800000U
#define EXECUTE_FLOAT_ONE		0x3f800000U
#define EXECUTE_INTERNAL_PUSH_BYTES	0x20U

/*
 * A buffer written as a target: its start and its row pitch on these
 * boundaries.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/nil/image.rs (Image::new_linear,
 * the 128-byte row alignment for rendering).
 */
#define EXECUTE_BUFFER_START_ALIGN	256U
#define EXECUTE_BUFFER_PITCH_ALIGN	128U

/*
 * A buffer read as a texture: the texture header holds the address from
 * bit 5 and the pitch from bit 5, so the texture starts on the 32-byte
 * boundary at or before the region and the region begins that many pixels
 * into its rows.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597tex.h
 * (NVC597_TEXHEAD_PITCH_ADDRESS_BITS31TO5,
 * NVC597_TEXHEAD_PITCH_PITCH_BITS20TO5).
 */
#define EXECUTE_TEXTURE_START_ALIGN	32U
#define EXECUTE_TEXTURE_PITCH_ALIGN	32U

/*
 * One slot: its run (token names it to the node, cpu and va its two views)
 * and the GPU's run of the push buffer last written in it, which must end
 * before the slot is written again.
 */
struct execute_slot {
	void *token;
	uint8_t *cpu;
	uint64_t va;
	struct nvrtx_vk_ticket ticket;
};

/* The words nvrtx_exec_viewport() writes. */
#define EXECUTE_VIEWPORT_BYTES		48U

/*
 * What draws one after another reuse: the viewport's words the last draw's
 * viewport (viewport_in, as recorded) made (valid once viewport_bytes is
 * not 0), and the pool handles of the textures (view and sampler) the
 * fragment program sampled, each made in the push buffer counted by pools
 * (execute_begin() counts each new one; the pools start empty there).
 */
struct execute_reuse {
	uint32_t viewport_in[6];
	uint8_t viewport[EXECUTE_VIEWPORT_BYTES];
	size_t viewport_bytes;
	uint32_t pools;
	uint32_t texture_pools[NVRTX_VK_TEXTURES_MAX];
	const void *texture_view[NVRTX_VK_TEXTURES_MAX];
	const void *texture_sampler[NVRTX_VK_TEXTURES_MAX];
	uint32_t texture_handle[NVRTX_VK_TEXTURES_MAX];
};

/*
 * The executor's own programs (a textured quad placed by push constants,
 * the draw-test draw's) and the session's slots.
 *
 * slot is the slot the push buffer is written in, cpu and va its views;
 * started says a push buffer is begun there and not yet submitted;
 * class_ready that a run on the session's channel set the 3D class's
 * initial state and the channel keeps it; reuse is what one draw leaves
 * the next.  at is
 * where the push buffer goes on, constants, headers and samplers how much
 * of their parts the push buffer uses, draws and quads how many
 * application draws and own quads it holds (for the node's counts).  The
 * programs are compiled once, into a run of their own.
 */
struct nvrtx_vk_execute {
	struct execute_slot slots[EXECUTE_SLOTS];
	uint32_t slot;
	uint8_t *cpu;
	uint64_t va;
	size_t at;
	uint32_t constants;
	uint32_t headers;
	uint32_t samplers;
	uint32_t draws;
	uint32_t quads;
	bool started;
	bool class_ready;
	void *program_token;
	uint8_t *program_cpu;
	uint64_t program_va;
	uint64_t quad_vertex;
	uint64_t quad_fragment;
	uint32_t quad_vertex_gprs;
	uint32_t quad_fragment_gprs;
	bool programs_ready;
	void *shadow_token;
	uint64_t shadow_va;
	uint64_t shadow_bytes;
	struct execute_reuse reuse;
	/* The owner's generation: the shader model of the programs and the 3D class of the push buffers. */
	uint32_t shader_model;
	uint32_t threed_class;
};

/*
 * What a command buffer's records have bound so far, and the pass they
 * are in; push_bytes are the command buffer's push constant bytes.
 */
struct execute_state {
	const uint8_t *push_bytes;
	const struct nvrtx_vk_pipeline *pipeline;
	uint32_t viewport[6];
	bool viewport_set;
	VkRect2D scissor;
	bool scissor_set;
	const struct nvrtx_vk_descriptor_set *sets[NVRTX_VK_SETS_MAX];
	const struct nvrtx_vk_buffer *buffers[NVRTX_VK_VERTEX_BINDINGS_MAX];
	uint64_t offsets[NVRTX_VK_VERTEX_BINDINGS_MAX];
	const struct nvrtx_vk_buffer *index_buffer;
	uint64_t index_offset;
	uint32_t index_type;
	uint8_t push[NVRTX_VK_PUSH_BYTES];
	bool in_pass;
	struct nvrtx_exec_target target;
	bool target_written;
	VkRect2D area;
	struct nvrtx_exec_target resolve;
	bool shadowed;
};

/* One quad an internal draw makes: the destination, the source texture (pitch linear or tiled) and its region in pixels. */
struct execute_quad {
	struct nvrtx_exec_target target;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	uint64_t source;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t source_pitch;
	uint32_t source_format;
	bool source_tiled;
	int32_t u0;
	int32_t v0;
	int32_t u1;
	int32_t v1;
	uint32_t filter;
};

static int execute_open(struct nvrtx_vk_session *session, struct nvrtx_vk_execute **execute);
static int execute_programs(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static int execute_compile(struct nvrtx_draw_ir *program, uint32_t shader_model, uint8_t *cpu, uint32_t *gprs, uint32_t *bytes);
static int execute_slot_make(struct nvrtx_vk_session *session, struct execute_slot *slot);
static int execute_start(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static void execute_begin(struct nvrtx_vk_execute *execute);
static int execute_submit(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static int execute_flush(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static int execute_drain(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static int execute_room(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state);
static int execute_record(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static void execute_bind(struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_begin_pass(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state,
			      const struct nvrtx_vk_record *record);
static int execute_shadow(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state,
			  bool load);
static int execute_end_pass(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state);
static void execute_shadow_copy(const struct nvrtx_exec_target *to, const struct nvrtx_exec_target *from, int32_t x0, int32_t y0,
				int32_t x1, int32_t y1, struct execute_quad *quad);
static int execute_draw(struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_textures(struct nvrtx_vk_execute *execute, const struct execute_state *state, uint8_t *constants);
static int execute_texture(struct nvrtx_vk_execute *execute, const struct nvrtx_vk_view *view, const struct nvrtx_vk_sampler *sampler, uint32_t *handle);
static int execute_pool_entry(struct nvrtx_vk_execute *execute, const uint32_t *header, const uint32_t *sampler, uint32_t *handle);
static int execute_constants(struct nvrtx_vk_execute *execute, uint8_t **cpu, uint64_t *va);
static void execute_vertex_input(const struct nvrtx_vk_pipeline *pipeline, const struct execute_state *state, struct nvrtx_exec_vertex *vertex);
static int execute_transfer(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_decode(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_video_copy(struct nvrtx_vk_execute *execute, const struct nvrtx_vk_image *image, uint64_t image_va, const struct nvrtx_vk_buffer *buffer, uint64_t buffer_va, const struct nvrtx_vk_record *record);
static int execute_copy_images(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_buffer_image(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_cpu_copy(struct nvrtx_vk_session *session, const struct nvrtx_vk_buffer *buffer, const struct nvrtx_vk_image *image, const VkBufferImageCopy *region, bool to_image);
static int execute_clear_image(struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_quad(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct execute_quad *quad);
static int execute_image_target(const struct nvrtx_vk_image *image, uint32_t level, uint32_t format, struct nvrtx_exec_target *target);
static int execute_image_source(const struct nvrtx_vk_image *image, uint32_t level, struct execute_quad *quad);
static int execute_depth_target(struct nvrtx_vk_session *session, struct nvrtx_vk_image *image, struct nvrtx_exec_target *target);
static int execute_uniforms(const struct execute_state *state, const struct nvrtx_vk_program *program, uint32_t group, uint8_t *push, size_t *at);
static int execute_copy_buffer(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, const struct nvrtx_vk_record *record);
static uint64_t execute_gob_offset(uint32_t pitch, uint32_t x, uint32_t y);
static bool execute_region_inside(int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t image_width, uint32_t image_height);
static void execute_full_viewport(uint32_t *viewport, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

/*
 * Puts an executable command buffer's records on the GPU without waiting
 * for them.  Returns 0, EINVAL for a command buffer that is not
 * executable, or the error of the submission or the wait that failed.
 */
int
nvrtx_vk_execute(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_command_buffer *command)
{
	struct nvrtx_vk_execute *execute;
	struct execute_state *state;
	uint32_t index;
	int error;

	/* Only an executable command buffer runs. */
	if (command->state != NVRTX_VK_COMMAND_EXECUTABLE)
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
	state->push_bytes = command->push_bytes;

	/* A push buffer begun before and never submitted (its command buffer failed) is begun again; else a slot is taken. */
	error = 0;
	if (execute->started) {
		execute_begin(execute);
	} else {
		error = execute_start(session, execute);
	}

	/* Each record in order, the push buffer put on the GPU when it fills. */
	for (index = 0U; index < command->record_count && error == 0; index++) {
		error = execute_room(session, execute, state);
		if (error != 0)
			break;
		error = execute_record(session, execute, state, &command->records[index]);
		if (error != 0)
			break;
	}

	/* The rest of the push buffer. */
	if (error == 0)
		error = execute_submit(session, execute);
	kern_free(state);

	/* Reports a run that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the GPU runs the command buffer. */
	return 0;
}

/* Gives back a closing session's slots and programs once its runs have ended. */
void
nvrtx_vk_execute_close(
	struct nvrtx_vk_session *session)
{
	struct nvrtx_vk_execute *execute;
	uint32_t index;

	execute = session->execute;
	if (execute == NULL)
		return;

	/* The runs (the node waits for the session's runs first), then the record. */
	for (index = 0U; index < EXECUTE_SLOTS; index++) {
		if (execute->slots[index].token != NULL)
			nvrtx_vk_gpu_free(session->owner, execute->slots[index].token);
	}
	if (execute->program_token != NULL)
		nvrtx_vk_gpu_free(session->owner, execute->program_token);
	if (execute->shadow_token != NULL)
		nvrtx_vk_gpu_free(session->owner, execute->shadow_token);
	kern_free(execute);
	session->execute = NULL;
}

/*
 * Gives the session's slots, made at its first submission with the first
 * slot, and the executor's programs.
 */
static int
execute_open(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute **execute)
{
	struct nvrtx_vk_execute *opened;
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

	/* The record and its first slot, the one written first. */
	opened = kern_calloc(1U, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;
	error = execute_slot_make(session, &opened->slots[0]);
	if (error != 0) {
		kern_free(opened);
		return error;
	}
	opened->slot = 0U;
	opened->cpu = opened->slots[0].cpu;
	opened->va = opened->slots[0].va;

	/* The owner's generation, read once: the programs and the push buffers follow it. */
	opened->shader_model = nvrtx_vk_shader_model(session->owner);
	opened->threed_class = nvrtx_vk_threed_class(session->owner);

	/* The executor's programs. */
	session->execute = opened;
	error = execute_programs(session, opened);
	if (error != 0)
		return error;

	/* Succeeded: the slots are the session's. */
	*execute = opened;
	return 0;
}

/* Makes a slot: its run, the zero page and the quad's corners written once. */
static int
execute_slot_make(
	struct nvrtx_vk_session *session,
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
	uint32_t index;
	int error;

	/* The run, in system memory the CPU writes through its cache (else in VRAM). */
	error = nvrtx_vk_gpu_alloc_system(session->owner, EXECUTE_RUN_BYTES, &slot->token, &slot->cpu, &slot->va);
	if (error != 0)
		error = nvrtx_vk_gpu_alloc(session->owner, EXECUTE_RUN_BYTES, &slot->token, &slot->cpu, &slot->va);
	if (error != 0) {
		slot->token = NULL;
		return error;
	}

	/* The zero page, and the corners: (0,0) (1,0) (0,1), (1,0) (1,1) (0,1). */
	kern_memset(slot->cpu + EXECUTE_ZERO_AT, 0, NVRTX_GSP_PAGE_SIZE);
	for (index = 0U; index < EXECUTE_CORNERS * 2U; index++)
		nvrtx_put32(slot->cpu, EXECUTE_CORNERS_AT + index * 4U, corners[index]);

	/* Succeeded: no run reads the slot yet. */
	slot->ticket.fence = 0U;
	slot->ticket.submitted_us = 0U;
	return 0;
}

/*
 * Compiles the executor's quad programs (draw-test's textured quad: the
 * corners placed by the push constants' rectangle, the texture read at the
 * push constants' texture rectangle) into a run of their own.
 */
static int
execute_programs(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	struct nvrtx_draw_ir *program;
	uint8_t *cpu;
	uint64_t va;
	uint32_t bytes;
	int error;

	/* The run: a page for each program (kept from a try that did not compile). */
	if (execute->program_token == NULL) {
		error = nvrtx_vk_gpu_alloc(session->owner, 2U * NVRTX_GSP_PAGE_SIZE, &execute->program_token, &execute->program_cpu,
					   &execute->program_va);
		if (error != 0)
			return error;
	}
	cpu = execute->program_cpu;
	va = execute->program_va;

	/* The IR's storage. */
	program = kern_malloc(sizeof(*program));
	if (program == NULL)
		return ENOMEM;

	/* The vertex program in the first page, the fragment program in the second. */
	nvrtx_draw_vertex_ir(program, 1);
	error = execute_compile(program, execute->shader_model, cpu, &execute->quad_vertex_gprs, &bytes);
	if (error == 0) {
		nvrtx_draw_fragment_ir(program, 1);
		error = execute_compile(program, execute->shader_model, cpu + NVRTX_GSP_PAGE_SIZE, &execute->quad_fragment_gprs, &bytes);
	}
	kern_free(program);
	if (error != 0)
		return error;

	/* Succeeded: the programs are in place. */
	execute->quad_vertex = va;
	execute->quad_fragment = va + NVRTX_GSP_PAGE_SIZE;
	execute->programs_ready = true;
	return 0;
}

/* Compiles one quad program into a page: its header, then its code. */
static int
execute_compile(
	struct nvrtx_draw_ir *program,
	uint32_t shader_model,
	uint8_t *cpu,
	uint32_t *gprs,
	uint32_t *bytes)
{
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	uint32_t code;
	int error;

	/* Compiled for the owner's shader model. */
	binary = NULL;
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_ir_sm(&program->ir, shader_model, &binary, &diagnostic);
	if (error != 0) {
		kern_logf("nvrtx: vk: a quad program did not compile (%d)\n", error);
		return error;
	}

	/* The header and the code must fit the page. */
	code = binary->code_words * 4U;
	if (binary->graphics == 0U || NVRTX_SM75_SPH_BYTES + code > NVRTX_GSP_PAGE_SIZE) {
		nvrtx_sm75_binary_free(binary);
		return EINVAL;
	}

	/* Copies them. */
	kern_memcpy(cpu, binary->sph, NVRTX_SM75_SPH_BYTES);
	kern_memcpy(cpu + NVRTX_SM75_SPH_BYTES, binary->code, code);
	*gprs = binary->gpr_count;
	*bytes = NVRTX_SM75_SPH_BYTES + code;
	nvrtx_sm75_binary_free(binary);

	/* Succeeded: the program is in place. */
	return 0;
}

/*
 * Starts a push buffer in a slot whose run has ended: the current slot,
 * another one made earlier, a new one while one is to be had, or else the
 * slot of the oldest run once it ended.  Returns 0, or the error of the
 * wait for that run.
 */
static int
execute_start(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	struct execute_slot *slot;
	uint32_t chosen;
	uint32_t index;
	uint32_t at;
	bool passed;
	int error;

	/* The current slot when its run ended, else another made one whose run ended. */
	chosen = EXECUTE_SLOTS;
	for (index = 0U; index < EXECUTE_SLOTS && chosen == EXECUTE_SLOTS; index++) {
		at = (execute->slot + index) % EXECUTE_SLOTS;
		slot = &execute->slots[at];
		if (slot->token == NULL)
			continue;
		passed = nvrtx_vk_gpu_passed(session->owner, &slot->ticket);
		if (passed)
			chosen = at;
	}

	/* Else a new slot, while one is to be had (one that cannot be made is waited for instead). */
	for (index = 0U; index < EXECUTE_SLOTS && chosen == EXECUTE_SLOTS; index++) {
		if (execute->slots[index].token != NULL)
			continue;
		error = execute_slot_make(session, &execute->slots[index]);
		if (error == 0)
			chosen = index;
		break;
	}

	/* Else the slot of the oldest run, once it ended (the first slot is always made). */
	if (chosen == EXECUTE_SLOTS) {
		chosen = 0U;
		for (index = 1U; index < EXECUTE_SLOTS; index++) {
			if (execute->slots[index].token == NULL)
				continue;
			if (execute->slots[index].ticket.submitted_us < execute->slots[chosen].ticket.submitted_us)
				chosen = index;
		}
		error = nvrtx_vk_gpu_wait(session->owner, &execute->slots[chosen].ticket);
		if (error != 0)
			return error;
	}

	/* The slot's run is over: counted and forgotten; the push buffer is written there. */
	slot = &execute->slots[chosen];
	nvrtx_vk_gpu_retire(session->owner, &slot->ticket);
	execute->slot = chosen;
	execute->cpu = slot->cpu;
	execute->va = slot->va;
	execute_begin(execute);

	/* Succeeded: the push buffer is begun. */
	return 0;
}

/*
 * Begins a push buffer in the current slot: the 3D class's state a queue
 * starts with (once on a channel that keeps it from one run to the next;
 * its vertex stream substitute is the first slot's zero page, which lives
 * as long as the session's runs), the header and sampler pools, and the
 * shader caches emptied; nothing of the slot's parts is used yet.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_queue.c
 * (nvk_queue_init_context_state) and src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_push_draw_state_init).
 */
static void
execute_begin(
	struct nvrtx_vk_execute *execute)
{
	uint8_t *push;

	/* The state and the pools. */
	push = execute->cpu + EXECUTE_PUSH_AT;
	execute->at = 0U;
	if (!execute->class_ready)
		execute->at = nvrtx_push_draw_init_class(push, 0U, execute->va + EXECUTE_ZERO_AT, execute->threed_class);
	execute->at = nvrtx_push_draw_textures(push, execute->at, execute->va + EXECUTE_HEADERS_AT, EXECUTE_POOL_ENTRIES,
					       execute->va + EXECUTE_SAMPLERS_AT, EXECUTE_POOL_ENTRIES);

	/* The programs and constants the CPU wrote since the last run are read anew, once for every draw of this push buffer. */
	execute->at = nvrtx_push_draw_invalidate(push, execute->at);

	/* Nothing of the parts is used: no texture handle of an earlier push buffer is good here. */
	execute->reuse.pools++;
	execute->constants = 0U;
	execute->headers = 0U;
	execute->samplers = 0U;
	execute->draws = 0U;
	execute->quads = 0U;
	execute->started = true;
}

/*
 * Puts the push buffer written so far on the GPU without waiting for it;
 * no push buffer is begun after it.  Returns the submission's error.
 */
static int
execute_submit(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	struct execute_slot *slot;
	int error;

	/* Counts its draws, then submits it; the slot keeps its run. */
	slot = &execute->slots[execute->slot];
	nvrtx_vk_gpu_drawn(session->owner, execute->draws, execute->quads);
	error = nvrtx_vk_gpu_submit(session->owner, execute->va + EXECUTE_PUSH_AT, (uint32_t)execute->at, &slot->ticket);
	if (error != 0)
		return error;

	/* Succeeded: the GPU runs it; the class's state it set stays for the next run on a channel that keeps it. */
	execute->started = false;
	execute->class_ready = nvrtx_vk_gpu_state_kept(session->owner);
	return 0;
}

/*
 * Puts the push buffer written so far on the GPU, then starts the next
 * one.  Returns the submission's error or the wait's for a slot.
 */
static int
execute_flush(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	int error;

	/* The push buffer, then the next one. */
	error = execute_submit(session, execute);
	if (error == 0)
		error = execute_start(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the next push buffer is begun. */
	return 0;
}

/*
 * Puts the push buffer written so far on the GPU and waits until the
 * session's runs have ended (a copy by the CPU follows), then starts the
 * next one.  Returns the error of the submission or the wait.
 */
static int
execute_drain(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	int error;

	/* The push buffer, the wait for it, then the next one. */
	error = execute_submit(session, execute);
	if (error == 0)
		error = nvrtx_vk_gpu_idle(session->owner);
	if (error == 0)
		error = execute_start(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the GPU has done the work before; the next push buffer is begun. */
	return 0;
}

/*
 * Makes room for one record: runs the push buffer when the record could
 * pass its end or the run's other parts are nearly used.  The target is
 * written again in the next push buffer.
 */
static int
execute_room(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state)
{
	bool full;
	int error;

	/* Full when the push buffer, the constants or the pools could not take a record. */
	full = false;
	if (execute->at + EXECUTE_RECORD_ROOM > EXECUTE_PUSH_BYTES)
		full = true;
	if (execute->constants + NVRTX_VK_REGIONS_MAX * EXECUTE_CONSTANTS_SLOT > EXECUTE_CONSTANTS_BYTES)
		full = true;
	if (execute->headers + NVRTX_VK_REGIONS_MAX + NVRTX_VK_TEXTURES_MAX > EXECUTE_POOL_ENTRIES)
		full = true;
	if (!full)
		return 0;

	/* Runs what there is. */
	error = execute_flush(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the next push buffer has room; its target is written anew. */
	state->target_written = false;
	return 0;
}

/* Runs one record. */
static int
execute_record(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	int error;

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
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		error = execute_begin_pass(session, execute, state, record);
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
		error = execute_end_pass(session, execute, state);
		break;
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
		error = execute_draw(execute, state, record);
		break;
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
		error = execute_transfer(session, execute, state, record);
		break;
	case GPU_OP_CMD_DECODE_VIDEO:
		error = execute_decode(session, execute, state, record);
		break;
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
	case GPU_OP_CMD_END_VIDEO_CODING:
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
	case GPU_OP_CMD_RESET_QUERY_POOL:
		error = nvrtx_vk_video_run(session, record);
		break;
	default:
		break;
	}

	/* Reports a record that could not run. */
	if (error != 0)
		return error;

	/* Succeeded: the record ran (or is bound for the draws after it). */
	return 0;
}

/* Keeps what a binding record binds for the draws after it. */
static void
execute_bind(
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	uint32_t index;

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
		for (index = 0U; index < record->object_count; index++) {
			if (record->objects[index] != NULL)
				state->sets[record->first + index] = record->objects[index]->payload;
		}
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		for (index = 0U; index < record->object_count; index++) {
			state->buffers[record->first + index] = record->objects[index]->payload;
			state->offsets[record->first + index] = record->offsets[index];
		}
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		state->index_buffer = record->objects[0]->payload;
		state->index_offset = record->offsets[0];
		state->index_type = record->values[0];
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		kern_memcpy(state->push + record->first, state->push_bytes + record->push_at, record->count);
		break;
	default:
		break;
	}
}

/*
 * Begins a pass: its framebuffer's colour view's image (at the view's
 * level) is the target from now on, with its depth view's image as the
 * depth target, and a pass whose attachment loads by clearing clears its
 * render area.
 */
static int
execute_begin_pass(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_render_pass *pass;
	const struct nvrtx_vk_framebuffer *framebuffer;
	const struct nvrtx_vk_view *view;
	bool clear_colour;
	bool clear_depth;
	bool clear_stencil;
	int32_t x1;
	int32_t y1;
	int error;

	pass = record->objects[0]->payload;
	framebuffer = record->objects[1]->payload;

	/* A pass that writes no colour attachment draws nothing that stays. */
	state->in_pass = true;
	state->target_written = false;
	state->area = record->rect;
	state->shadowed = false;
	kern_memset(&state->target, 0, sizeof(state->target));
	if (pass->colour == 0U || pass->colour_attachment >= framebuffer->view_count)
		return 0;

	/* The target: the colour view's image at its level, in the view's format. */
	view = framebuffer->views[pass->colour_attachment]->payload;
	error = execute_image_target(view->image->payload, view->base_level, view->format, &state->target);
	if (error != 0)
		return error;

	/* The depth target: the depth view's image. */
	if (pass->depth != 0U && pass->depth_attachment < framebuffer->view_count) {
		view = framebuffer->views[pass->depth_attachment]->payload;
		error = execute_depth_target(session, view->image->payload, &state->target);
		if (error != 0)
			return error;
	}

	/* A pitch-linear colour target with a depth target is drawn in a block-linear shadow. */
	if (state->target.depth_address != 0U && state->target.block_linear == 0U) {
		error = execute_shadow(session, execute, state, pass->load_op == VK_ATTACHMENT_LOAD_OP_LOAD);
		if (error != 0)
			return error;
	}

	/* A pass loading by clearing clears its area, within the framebuffer and the image. */
	clear_colour = (pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
	clear_depth = (state->target.depth_address != 0U && pass->depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
	clear_stencil = (state->target.depth_stencil != 0U && pass->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
	if (!clear_colour && !clear_depth && !clear_stencil)
		return 0;
	execute->at = nvrtx_exec_target(execute->cpu + EXECUTE_PUSH_AT, execute->at, &state->target);
	state->target_written = true;
	x1 = state->area.offset.x + (int32_t)state->area.extent.width;
	y1 = state->area.offset.y + (int32_t)state->area.extent.height;
	if (x1 > (int32_t)framebuffer->width)
		x1 = (int32_t)framebuffer->width;
	if (y1 > (int32_t)framebuffer->height)
		y1 = (int32_t)framebuffer->height;
	if (x1 > (int32_t)state->target.width)
		x1 = (int32_t)state->target.width;
	if (y1 > (int32_t)state->target.height)
		y1 = (int32_t)state->target.height;
	if (state->area.offset.x < 0 || state->area.offset.y < 0 || x1 <= state->area.offset.x || y1 <= state->area.offset.y)
		return 0;
	if (clear_colour)
		execute->at = nvrtx_exec_clear(execute->cpu + EXECUTE_PUSH_AT, execute->at, record->values,
					       (uint32_t)state->area.offset.x, (uint32_t)state->area.offset.y, (uint32_t)x1, (uint32_t)y1);
	if (clear_depth)
		execute->at = nvrtx_exec_clear_depth(execute->cpu + EXECUTE_PUSH_AT, execute->at, record->values[4],
						     (uint32_t)state->area.offset.x, (uint32_t)state->area.offset.y, (uint32_t)x1, (uint32_t)y1);
	if (clear_stencil)
		execute->at = nvrtx_exec_clear_stencil(execute->cpu + EXECUTE_PUSH_AT, execute->at, record->values[5],
						       (uint32_t)state->area.offset.x, (uint32_t)state->area.offset.y, (uint32_t)x1, (uint32_t)y1);

	/* Succeeded: the pass draws into its target. */
	return 0;
}

/*
 * Puts a block-linear shadow of the pass's pitch-linear colour target in
 * its place (the 3D engine draws with a depth target only into a
 * block-linear colour target 0): the shadow is the executor's, grown after
 * the work before it ran when too small, and a pass that loads its colour
 * starts the shadow with the target's pixels.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_linear_render_copy, rendering a linear image through a tiled shadow
 * copied at the start and the end of rendering).
 */
static int
execute_shadow(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	bool load)
{
	struct execute_quad quad;
	uint8_t *cpu;
	uint64_t bytes;
	uint32_t pitch;
	uint32_t height;
	int error;

	/* The shadow's rows in whole GOBs, its height in whole GOBs. */
	pitch = (state->target.width * NVRTX_VK_PIXEL_BYTES + NVRTX_VK_GOB_WIDTH - 1U) & ~(NVRTX_VK_GOB_WIDTH - 1U);
	height = (state->target.height + NVRTX_VK_GOB_HEIGHT - 1U) & ~(NVRTX_VK_GOB_HEIGHT - 1U);
	bytes = (uint64_t)pitch * height;

	/* A shadow too small is replaced once the work that may draw in it has run. */
	if (execute->shadow_bytes < bytes) {
		if (execute->shadow_token != NULL) {
			error = execute_flush(session, execute);
			if (error != 0)
				return error;
			nvrtx_vk_gpu_free(session->owner, execute->shadow_token);
			execute->shadow_token = NULL;
			execute->shadow_bytes = 0U;
		}
		error = nvrtx_vk_gpu_alloc(session->owner, (bytes + NVRTX_GSP_PAGE_SIZE - 1U) & ~(uint64_t)(NVRTX_GSP_PAGE_SIZE - 1U),
					   &execute->shadow_token, &cpu, &execute->shadow_va);
		if (error != 0)
			return error;
		execute->shadow_bytes = bytes;
	}

	/* The pass draws into the shadow; the target gets it at the pass's end. */
	state->resolve = state->target;
	state->resolve.depth_address = 0U;
	state->target.address = execute->shadow_va;
	state->target.pitch = pitch;
	state->target.block_linear = 1U;
	state->shadowed = true;
	if (!load)
		return 0;

	/* A loading pass: the target's pixels into the shadow. */
	execute_shadow_copy(&state->target, &state->resolve, 0, 0, (int32_t)state->target.width, (int32_t)state->target.height,
			    &quad);
	quad.target.depth_address = 0U;
	quad.source_tiled = false;
	error = execute_quad(session, execute, state, &quad);
	if (error != 0)
		return error;
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);

	/* Succeeded: the shadow holds the target's pixels. */
	return 0;
}

/*
 * Ends a pass: its work is ordered before what follows, and a pass drawn in
 * a shadow copies its render area from the shadow to the target.
 */
static int
execute_end_pass(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state)
{
	struct execute_quad quad;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	int error;

	/* The pass's work before what follows. */
	state->in_pass = false;
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);
	if (!state->shadowed)
		return 0;
	state->shadowed = false;

	/* The render area, within the target. */
	x0 = state->area.offset.x;
	y0 = state->area.offset.y;
	x1 = x0 + (int32_t)state->area.extent.width;
	y1 = y0 + (int32_t)state->area.extent.height;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)state->resolve.width)
		x1 = (int32_t)state->resolve.width;
	if (y1 > (int32_t)state->resolve.height)
		y1 = (int32_t)state->resolve.height;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	/* The shadow's area onto the target, then ordered before what follows. */
	execute_shadow_copy(&state->resolve, &state->target, x0, y0, x1, y1, &quad);
	quad.source_tiled = true;
	error = execute_quad(session, execute, state, &quad);
	if (error != 0)
		return error;
	state->target_written = false;
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);

	/* Succeeded: the target has the pass's pixels. */
	return 0;
}

/* Describes a one-to-one, nearest copy of a region between two colour targets of one size as a quad. */
static void
execute_shadow_copy(
	const struct nvrtx_exec_target *to,
	const struct nvrtx_exec_target *from,
	int32_t x0,
	int32_t y0,
	int32_t x1,
	int32_t y1,
	struct execute_quad *quad)
{
	/* The destination and its region. */
	kern_memset(quad, 0, sizeof(*quad));
	quad->target = *to;
	quad->target.depth_address = 0U;
	quad->x0 = x0;
	quad->y0 = y0;
	quad->x1 = x1;
	quad->y1 = y1;

	/* The source, the same region, nearest. */
	quad->source = from->address;
	quad->source_width = from->width;
	quad->source_height = from->height;
	quad->source_pitch = from->pitch;
	quad->source_format = from->format;
	quad->u0 = x0;
	quad->v0 = y0;
	quad->u1 = x1;
	quad->v1 = y1;
	quad->filter = VK_FILTER_NEAREST;
}

/*
 * Runs a draw (indexed or not) in a pass: its constants (the push
 * constants and the texture handles of the fragment program's samples),
 * the target when not written yet, the viewport and scissor, the culling,
 * the depth test, the blend, the vertex input, the index buffer, the
 * constant buffers, the programs and the draw.
 */
static int
execute_draw(
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_pipeline *pipeline;
	struct execute_reuse *reuse;
	struct nvrtx_exec_vertex vertex;
	struct nvrtx_exec_blend blend;
	struct nvrtx_exec_depth depth;
	const uint32_t *viewport;
	const VkRect2D *scissor;
	uint8_t *push;
	uint8_t *constants;
	uint64_t constants_va;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	uint64_t index_va;
	uint8_t *index_cpu;
	bool indexed;
	int error;

	/* A draw in a pass with a target, with a pipeline; an indexed one with an index buffer. */
	pipeline = state->pipeline;
	indexed = (record->opcode == GPU_OP_CMD_DRAW_INDEXED);
	if (!state->in_pass || pipeline == NULL)
		return EINVAL;
	if (indexed && state->index_buffer == NULL)
		return EINVAL;
	if (state->target.address == 0U || record->values[0] == 0U || record->values[1] == 0U)
		return 0;
	index_va = 0U;
	if (indexed) {
		error = nvrtx_vk_buffer_address(state->index_buffer, &index_va, &index_cpu);
		if (error != 0)
			return error;
		if (state->index_offset >= state->index_buffer->size)
			return EINVAL;
	}

	/* The draw's constants: the push constants, then the handles of its textures. */
	error = execute_constants(execute, &constants, &constants_va);
	if (error != 0)
		return error;
	kern_memcpy(constants, state->push, NVRTX_VK_PUSH_BYTES);
	error = execute_textures(execute, state, constants);
	if (error != 0)
		return error;

	/* The target, once in a push buffer. */
	push = execute->cpu + EXECUTE_PUSH_AT;
	if (!state->target_written) {
		execute->at = nvrtx_exec_target(push, execute->at, &state->target);
		state->target_written = true;
	}

	/* The viewport and the scissor: dynamic or the pipeline's, the scissor within the target. */
	viewport = pipeline->viewport;
	if (pipeline->dynamic_viewport != 0U && state->viewport_set)
		viewport = state->viewport;
	scissor = &pipeline->scissor;
	if (pipeline->dynamic_scissor != 0U && state->scissor_set)
		scissor = &state->scissor;
	x0 = scissor->offset.x;
	y0 = scissor->offset.y;
	x1 = x0 + (int32_t)scissor->extent.width;
	y1 = y0 + (int32_t)scissor->extent.height;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)state->target.width)
		x1 = (int32_t)state->target.width;
	if (y1 > (int32_t)state->target.height)
		y1 = (int32_t)state->target.height;
	if (x1 <= x0 || y1 <= y0)
		return 0;
	reuse = &execute->reuse;
	if (reuse->viewport_bytes == 0U || kern_memcmp(reuse->viewport_in, viewport, sizeof(reuse->viewport_in)) != 0) {
		kern_memcpy(reuse->viewport_in, viewport, sizeof(reuse->viewport_in));
		reuse->viewport_bytes = nvrtx_exec_viewport(reuse->viewport, 0U, viewport);
	}
	kern_memcpy(push + execute->at, reuse->viewport, reuse->viewport_bytes);
	execute->at += reuse->viewport_bytes;
	execute->at = nvrtx_exec_scissor(push, execute->at, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);

	/* The culling, the depth test when the pass has a depth target, and the stencil test when it has a stencil. */
	execute->at = nvrtx_exec_raster(push, execute->at, pipeline->cull_mode, pipeline->front_face);
	kern_memset(&depth, 0, sizeof(depth));
	if (state->target.depth_address != 0U) {
		depth.test = pipeline->depth_test;
		depth.write = pipeline->depth_write;
		depth.compare = pipeline->depth_compare;
	}
	if (state->target.depth_stencil != 0U) {
		depth.stencil = pipeline->stencil_test;
		depth.front = pipeline->stencil_front;
		depth.back = pipeline->stencil_back;
	}
	execute->at = nvrtx_exec_depth(push, execute->at, &depth);

	/* The blend. */
	blend.enable = pipeline->blend_enable;
	blend.colour_op = pipeline->blend_colour_op;
	blend.colour_source = pipeline->blend_src_colour;
	blend.colour_destination = pipeline->blend_dst_colour;
	blend.alpha_op = pipeline->blend_alpha_op;
	blend.alpha_source = pipeline->blend_src_alpha;
	blend.alpha_destination = pipeline->blend_dst_alpha;
	blend.write_mask = pipeline->write_mask;
	execute->at = nvrtx_exec_blend(push, execute->at, &blend);

	/* The vertex input, the index buffer, the constants, the uniform buffers and the programs. */
	execute_vertex_input(pipeline, state, &vertex);
	execute->at = nvrtx_exec_vertex(push, execute->at, &vertex);
	if (indexed)
		execute->at = nvrtx_exec_index_buffer(push, execute->at, index_va + state->index_offset,
						      state->index_buffer->size - state->index_offset, state->index_type);
	execute->at = nvrtx_push_draw_constants(push, execute->at, constants_va, EXECUTE_CONSTANTS_SIZE);
	error = execute_uniforms(state, &pipeline->vertex, EXECUTE_GROUP_VERTEX, push, &execute->at);
	if (error == 0)
		error = execute_uniforms(state, &pipeline->fragment, EXECUTE_GROUP_FRAGMENT, push, &execute->at);
	if (error != 0)
		return error;
	execute->at = nvrtx_push_draw_pipeline(push, execute->at, pipeline->vertex.va, pipeline->vertex.gprs,
					       pipeline->fragment.va, pipeline->fragment.gprs);

	/* The draw, one more in the push buffer. */
	if (indexed) {
		execute->at = nvrtx_exec_draw_indexed(push, execute->at, pipeline->topology, record->values[2], record->values[0],
						      record->values[1], record->values[4], (int32_t)record->values[3]);
	} else {
		execute->at = nvrtx_exec_draw(push, execute->at, pipeline->topology, record->values[2], record->values[0],
					      record->values[1], record->values[3]);
	}
	execute->draws++;

	/* Succeeded: the draw is written. */
	return 0;
}

/*
 * Binds a program's uniform buffers to its group: the k-th one, from its
 * set's binding, as constant buffer 1 + k, its range rounded up to whole
 * 16-byte units and cut at the largest constant buffer.  A binding with
 * nothing bound, or a range off the 256-byte boundary, refuses the draw.
 */
static int
execute_uniforms(
	const struct execute_state *state,
	const struct nvrtx_vk_program *program,
	uint32_t group,
	uint8_t *push,
	size_t *at)
{
	const struct nvrtx_vk_descriptor_set *set;
	const struct nvrtx_vk_descriptor_slot *slot;
	const struct nvrtx_vk_buffer *buffer;
	uint64_t range;
	uint64_t va;
	uint8_t *cpu;
	uint32_t index;
	int error;

	/* Each uniform buffer of the program. */
	for (index = 0U; index < program->uniform_count; index++) {
		set = state->sets[program->uniforms[index].set];
		if (set == NULL)
			return EINVAL;
		slot = &set->slots[program->uniforms[index].binding];
		if (slot->buffer == NULL)
			return EINVAL;
		buffer = slot->buffer->payload;
		error = nvrtx_vk_buffer_address(buffer, &va, &cpu);
		if (error != 0)
			return error;
		if (slot->offset >= buffer->size || ((va + slot->offset) % EXECUTE_UNIFORM_ALIGN) != 0U)
			return EINVAL;

		/* Its range: the rest of the buffer for the whole size. */
		range = slot->range;
		if (range == VK_WHOLE_SIZE || range > buffer->size - slot->offset)
			range = buffer->size - slot->offset;
		range = (range + EXECUTE_UNIFORM_UNIT - 1U) & ~(uint64_t)(EXECUTE_UNIFORM_UNIT - 1U);
		if (range > EXECUTE_UNIFORM_BYTES_MAX)
			range = EXECUTE_UNIFORM_BYTES_MAX;
		*at = nvrtx_exec_uniform(push, *at, group, NVRTX_SM75_CB_UNIFORM_FIRST + index, va + slot->offset, (uint32_t)range);
	}

	/* Succeeded: every uniform buffer is bound. */
	return 0;
}

/*
 * Writes the handles of the fragment program's sampled images into a
 * draw's constants: each one's header and sampler from the bound set's
 * binding, put in the pools once a push buffer for a view and a sampler.
 * A binding with nothing bound refuses the draw.
 */
static int
execute_textures(
	struct nvrtx_vk_execute *execute,
	const struct execute_state *state,
	uint8_t *constants)
{
	const struct nvrtx_vk_program *program;
	const struct nvrtx_vk_descriptor_set *set;
	const struct nvrtx_vk_descriptor_slot *slot;
	struct execute_reuse *reuse;
	uint32_t handle;
	uint32_t index;
	int error;

	/* Each sampled image of the fragment program. */
	program = &state->pipeline->fragment;
	for (index = 0U; index < program->texture_count; index++) {
		set = state->sets[program->textures[index].set];
		if (set == NULL)
			return EINVAL;
		slot = &set->slots[program->textures[index].binding];
		if (slot->view == NULL || slot->sampler == NULL)
			return EINVAL;

		/* Its handle at its slot: the one an earlier draw of this push buffer made for the same view and sampler, or a new one. */
		reuse = &execute->reuse;
		if (index < NVRTX_VK_TEXTURES_MAX && reuse->texture_pools[index] == reuse->pools &&
		    reuse->texture_view[index] == slot->view->payload && reuse->texture_sampler[index] == slot->sampler->payload) {
			handle = reuse->texture_handle[index];
		} else {
			error = execute_texture(execute, slot->view->payload, slot->sampler->payload, &handle);
			if (error != 0)
				return error;
			if (index < NVRTX_VK_TEXTURES_MAX) {
				reuse->texture_pools[index] = reuse->pools;
				reuse->texture_view[index] = slot->view->payload;
				reuse->texture_sampler[index] = slot->sampler->payload;
				reuse->texture_handle[index] = handle;
			}
		}
		nvrtx_put32(constants, NVRTX_SM75_CB_TEXTURES + index * 4U, handle);
	}

	/* Succeeded: every handle is in place. */
	return 0;
}

/* Writes a view's header and a sampler into the pools and gives their handle. */
static int
execute_texture(
	struct nvrtx_vk_execute *execute,
	const struct nvrtx_vk_view *view,
	const struct nvrtx_vk_sampler *sampler,
	uint32_t *handle)
{
	const struct nvrtx_vk_image *image;
	struct nvrtx_exec_sampler words;
	uint32_t header[8];
	uint32_t sampled[8];
	uint64_t va;
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint8_t *cpu;
	int error;

	/* The view's image, with storage, in a colour format or a plane of a video image (a depth image is not sampled). */
	image = view->image->payload;
	if (image->depth || (image->video && view->plane == 0U))
		return EINVAL;
	error = nvrtx_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;

	/*
	 * The header: a video plane's (the luma plane full size, the chroma
	 * plane's pairs half each way), a tiled image's with the view's levels,
	 * a pitch-linear image's of its one level; the sampler.
	 */
	if (image->video) {
		if (view->plane == 1U) {
			nvrtx_exec_texture_header_plane(header, va + image->plane_offset[0], image->width, image->height, 1U,
							view->swizzle);
		} else {
			nvrtx_exec_texture_header_plane(header, va + image->plane_offset[1], image->width / 2U,
							image->height / 2U, 2U, view->swizzle);
		}
	} else if (image->tiled) {
		nvrtx_exec_texture_header_tiled(header, va, image->width, image->height, image->levels, view->base_level,
						view->level_count, view->format, view->swizzle);
	} else {
		nvrtx_vk_image_level(image, 0U, &offset, &width, &height, &pitch);
		nvrtx_exec_texture_header(header, va + offset, width, height, pitch, view->format, view->swizzle);
	}
	kern_memset(&words, 0, sizeof(words));
	words.mag_filter = sampler->mag_filter;
	words.min_filter = sampler->min_filter;
	words.mipmap_mode = sampler->mipmap_mode;
	words.min_lod = sampler->min_lod;
	words.max_lod = sampler->max_lod;
	words.address_u = sampler->address_u;
	words.address_v = sampler->address_v;
	words.address_w = sampler->address_w;
	words.unnormalized = sampler->unnormalized;
	nvrtx_exec_sampler_words(sampled, &words);

	/* Into the pools. */
	error = execute_pool_entry(execute, header, sampled, handle);
	if (error != 0)
		return error;

	/* Succeeded: the handle names the header and the sampler. */
	return 0;
}

/* Puts a header and a sampler into the next entries of the pools and gives their handle. */
static int
execute_pool_entry(
	struct nvrtx_vk_execute *execute,
	const uint32_t *header,
	const uint32_t *sampler,
	uint32_t *handle)
{
	uint32_t index;

	/* Room in the pools (execute_room keeps some for each record). */
	if (execute->headers >= EXECUTE_POOL_ENTRIES || execute->samplers >= EXECUTE_POOL_ENTRIES)
		return ENOSPC;

	/* The two entries' words. */
	for (index = 0U; index < 8U; index++) {
		nvrtx_put32(execute->cpu, EXECUTE_HEADERS_AT + execute->headers * EXECUTE_POOL_ENTRY_BYTES + index * 4U, header[index]);
		nvrtx_put32(execute->cpu, EXECUTE_SAMPLERS_AT + execute->samplers * EXECUTE_POOL_ENTRY_BYTES + index * 4U, sampler[index]);
	}

	/* The handle: the header's index, the sampler's above it. */
	*handle = execute->headers | (execute->samplers << EXECUTE_HANDLE_SAMPLER_SHIFT);
	execute->headers++;
	execute->samplers++;

	/* Succeeded: the entries are the draw's. */
	return 0;
}

/* Gives a draw's constant buffer: the next slot of the run's constants, cleared. */
static int
execute_constants(
	struct nvrtx_vk_execute *execute,
	uint8_t **cpu,
	uint64_t *va)
{
	/* Room for one more (execute_room keeps some for each record). */
	if (execute->constants + EXECUTE_CONSTANTS_SLOT > EXECUTE_CONSTANTS_BYTES)
		return ENOSPC;

	/* The slot. */
	*cpu = execute->cpu + EXECUTE_CONSTANTS_AT + execute->constants;
	*va = execute->va + EXECUTE_CONSTANTS_AT + execute->constants;
	kern_memset(*cpu, 0, EXECUTE_CONSTANTS_SIZE);
	execute->constants += EXECUTE_CONSTANTS_SLOT;

	/* Succeeded: the slot is the draw's. */
	return 0;
}

/*
 * Fills a draw's vertex input: the pipeline's attributes, each from the
 * stream of its binding, and each binding's stream at its bound buffer
 * (a binding without storage stays off: its attributes read the zero
 * page).
 */
static void
execute_vertex_input(
	const struct nvrtx_vk_pipeline *pipeline,
	const struct execute_state *state,
	struct nvrtx_exec_vertex *vertex)
{
	const struct nvrtx_vk_vertex_binding *binding;
	const struct nvrtx_vk_buffer *buffer;
	struct nvrtx_exec_stream *stream;
	uint64_t va;
	uint8_t *cpu;
	uint32_t index;
	int error;

	/* The attributes: the stream is the binding's number. */
	kern_memset(vertex, 0, sizeof(*vertex));
	vertex->attribute_count = pipeline->attribute_count;
	for (index = 0U; index < pipeline->attribute_count; index++) {
		vertex->attributes[index].location = pipeline->attributes[index].location;
		vertex->attributes[index].stream = pipeline->attributes[index].binding;
		vertex->attributes[index].offset = pipeline->attributes[index].offset;
		vertex->attributes[index].format = pipeline->attributes[index].format;
	}

	/* The streams of the pipeline's bindings. */
	for (index = 0U; index < pipeline->binding_count; index++) {
		binding = &pipeline->bindings[index];
		buffer = state->buffers[binding->binding];
		if (buffer == NULL)
			continue;
		error = nvrtx_vk_buffer_address(buffer, &va, &cpu);
		if (error != 0 || state->offsets[binding->binding] >= buffer->size)
			continue;

		stream = &vertex->streams[binding->binding];
		stream->enabled = 1U;
		stream->address = va + state->offsets[binding->binding];
		stream->bytes = buffer->size - state->offsets[binding->binding];
		stream->stride = binding->stride;
		stream->instance = binding->instance;
	}
}

/*
 * Runs a transfer outside a pass: the barrier before it (the work before
 * may have written its source), the transfer, the barrier after it (the
 * target written anew in the next pass).
 */
static int
execute_transfer(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	int error;

	/* What wrote the source has finished. */
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);

	/* The transfer. */
	if (record->opcode == GPU_OP_CMD_COPY_BUFFER) {
		error = execute_copy_buffer(session, execute, record);
	} else if (record->opcode == GPU_OP_CMD_COPY_IMAGE || record->opcode == GPU_OP_CMD_BLIT_IMAGE) {
		error = execute_copy_images(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
		error = execute_clear_image(execute, state, record);
	} else {
		error = execute_buffer_image(session, execute, state, record);
	}
	if (error != 0)
		return error;

	/* What follows reads what it wrote; the pass's target is written again. */
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);
	state->target_written = false;

	/* Succeeded: the transfer is written. */
	return 0;
}

/*
 * Runs a video decode in its place among the records: the push buffer so
 * far is run and waited for (the graphics work before has ended), the
 * decoder runs the picture and is waited for, and the next push buffer
 * is begun, so what follows reads the decoded picture.
 */
static int
execute_decode(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	int error;

	/* The graphics work before the decode. */
	error = execute_drain(session, execute);
	if (error != 0)
		return error;

	/* The decode, waited for; the next push buffer's target is written anew. */
	error = nvrtx_vk_video_run(session, record);
	state->target_written = false;
	if (error != 0)
		return error;

	/* Succeeded: the picture is decoded. */
	return 0;
}

/*
 * Copies regions of a video image's planes out to a buffer with the copy
 * engine, which reads the block-linear planes: each region's aspect names
 * its plane (the luma plane of one byte a pixel, the chroma plane of two
 * at half the size), the buffer's rows are its row length's pixels of the
 * plane's bytes.
 */
static int
execute_video_copy(
	struct nvrtx_vk_execute *execute,
	const struct nvrtx_vk_image *image,
	uint64_t image_va,
	const struct nvrtx_vk_buffer *buffer,
	uint64_t buffer_va,
	const struct nvrtx_vk_record *record)
{
	const VkBufferImageCopy *regions;
	const VkBufferImageCopy *region;
	struct nvrtx_exec_plane_copy copy;
	uint32_t index;
	uint32_t plane;
	uint32_t pixel;
	uint32_t plane_width;
	uint32_t plane_height;
	uint32_t row;
	uint64_t span;
	bool inside;

	/* Each region. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		region = &regions[index];

		/* The plane its aspect names, its pixel's bytes and its size. */
		if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT) {
			plane = 0U;
			pixel = 1U;
			plane_width = image->width;
			plane_height = image->height;
		} else if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT) {
			plane = 1U;
			pixel = 2U;
			plane_width = image->width / 2U;
			plane_height = image->height / 2U;
		} else {
			/* A region of any other aspect is refused. */
			return EINVAL;
		}

		/* Inside the plane, not empty. */
		inside = execute_region_inside(region->imageOffset.x, region->imageOffset.y, region->imageExtent.width,
					       region->imageExtent.height, plane_width, plane_height);
		if (!inside ||
		    region->imageExtent.width == 0U ||
		    region->imageExtent.height == 0U)
			return EINVAL;

		/* The buffer's rows: tightly packed unless the region names a row length. */
		row = region->bufferRowLength;
		if (row == 0U)
			row = region->imageExtent.width;

		/* Inside the buffer: the bytes from its first to its last row's end. */
		span = (uint64_t)row * pixel * (region->imageExtent.height - 1U) + (uint64_t)region->imageExtent.width * pixel;
		if (region->bufferOffset > buffer->size || span > buffer->size - region->bufferOffset)
			return EINVAL;

		/* The copy of the region. */
		copy.source = image_va + image->plane_offset[plane];
		copy.source_pitch = image->pitch;
		copy.source_rows = image->plane_rows[plane];
		copy.x_bytes = (uint32_t)region->imageOffset.x * pixel;
		copy.y = (uint32_t)region->imageOffset.y;
		copy.destination = buffer_va + region->bufferOffset;
		copy.destination_pitch = row * pixel;
		copy.line_bytes = region->imageExtent.width * pixel;
		copy.lines = region->imageExtent.height;
		execute->at = nvrtx_exec_copy_plane(execute->cpu + EXECUTE_PUSH_AT, execute->at, &copy);
	}

	/* Succeeded: the regions are written. */
	return 0;
}

/*
 * Copies or blits regions between two images by drawing quads: the
 * destination region's quad samples the source region (a copy one to one
 * and nearest, a blit scaled and filtered).  A blit's region given
 * backwards mirrors it.
 */
static int
execute_copy_images(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_image *source;
	const struct nvrtx_vk_image *destination;
	const VkImageCopy *copies;
	const VkImageBlit *blits;
	struct execute_quad quad;
	uint32_t index;
	uint32_t source_level;
	uint32_t destination_level;
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

	/* A copy one to one and nearest, a blit filtered. */
	kern_memset(&quad, 0, sizeof(quad));
	quad.filter = VK_FILTER_NEAREST;
	if (record->opcode == GPU_OP_CMD_BLIT_IMAGE)
		quad.filter = record->values[0];

	/* Each region. */
	copies = record->regions;
	blits = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		/* The two images at the region's levels, with storage. */
		if (record->opcode == GPU_OP_CMD_COPY_IMAGE) {
			source_level = copies[index].srcSubresource.mipLevel;
			destination_level = copies[index].dstSubresource.mipLevel;
		} else {
			source_level = blits[index].srcSubresource.mipLevel;
			destination_level = blits[index].dstSubresource.mipLevel;
		}
		if (source_level >= source->levels || destination_level >= destination->levels)
			return EINVAL;
		error = execute_image_target(destination, destination_level, destination->format, &quad.target);
		if (error == 0)
			error = execute_image_source(source, source_level, &quad);
		if (error != 0)
			return error;

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
		} else {
			dx0 = blits[index].dstOffsets[0].x;
			dy0 = blits[index].dstOffsets[0].y;
			dx1 = blits[index].dstOffsets[1].x;
			dy1 = blits[index].dstOffsets[1].y;
			sx0 = blits[index].srcOffsets[0].x;
			sy0 = blits[index].srcOffsets[0].y;
			sx1 = blits[index].srcOffsets[1].x;
			sy1 = blits[index].srcOffsets[1].y;
		}

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

		error = execute_quad(session, execute, state, &quad);
		if (error != 0)
			return error;
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies regions between a buffer and an image.  A buffer region laid out
 * as a texture can be (to an image) or as a target can be (from an image)
 * is drawn; another is copied by the CPU after the work before it ran.
 */
static int
execute_buffer_image(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *buffer;
	const struct nvrtx_vk_image *image;
	const VkBufferImageCopy *regions;
	const VkBufferImageCopy *region;
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
	uint32_t skew;
	uint32_t level;
	uint32_t level_width;
	uint32_t level_height;
	uint32_t level_pitch;
	uint64_t level_offset;
	bool inside;
	bool drawn;
	int error;

	/* The buffer and the image, with storage. */
	to_image = (record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE);
	if (to_image) {
		buffer = record->objects[0]->payload;
		image = record->objects[1]->payload;
	} else {
		image = record->objects[0]->payload;
		buffer = record->objects[1]->payload;
	}
	error = nvrtx_vk_buffer_address(buffer, &buffer_va, &buffer_cpu);
	if (error == 0)
		error = nvrtx_vk_image_address(image, &image_va, &image_cpu);
	if (error == 0 && image->depth)
		error = EINVAL;

	/* Refuses a copy into a video image, which only the decoder writes. */
	if (error == 0 &&
	    image->video &&
	    to_image)
		error = EINVAL;

	/* Propagates a refusal. */
	if (error != 0)
		return error;

	/* A video image's planes are read out by the copy engine. */
	if (image->video) {
		error = execute_video_copy(execute, image, image_va, buffer, buffer_va, record);
		if (error != 0)
			return error;
		return 0;
	}

	/* Each region. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		region = &regions[index];

		/* Its level; inside it; the buffer's rows (a row length of zero is the region's width). */
		level = region->imageSubresource.mipLevel;
		if (level >= image->levels)
			return EINVAL;
		nvrtx_vk_image_level(image, level, &level_offset, &level_width, &level_height, &level_pitch);
		inside = execute_region_inside(region->imageOffset.x, region->imageOffset.y, region->imageExtent.width,
					       region->imageExtent.height, level_width, level_height);
		if (!inside || region->imageExtent.width == 0U || region->imageExtent.height == 0U)
			return EINVAL;
		row = region->bufferRowLength;
		if (row == 0U)
			row = region->imageExtent.width;
		pitch = row * NVRTX_VK_PIXEL_BYTES;
		if (region->bufferOffset > buffer->size ||
		    (uint64_t)pitch * (region->imageExtent.height - 1U) + (uint64_t)region->imageExtent.width * NVRTX_VK_PIXEL_BYTES >
		    buffer->size - region->bufferOffset)
			return EINVAL;
		start = buffer_va + region->bufferOffset;

		/*
		 * The GPU draws the region when the buffer can be its texture (whole
		 * pixels, rows on 32 bytes) or its target (start on 256 bytes, rows
		 * on 128).
		 */
		drawn = false;
		if (to_image) {
			if ((start % NVRTX_VK_PIXEL_BYTES) == 0U && (pitch % EXECUTE_TEXTURE_PITCH_ALIGN) == 0U)
				drawn = true;
		} else if ((start % EXECUTE_BUFFER_START_ALIGN) == 0U && (pitch % EXECUTE_BUFFER_PITCH_ALIGN) == 0U) {
			drawn = true;
		}

		/* Any other region is copied by the CPU, after the work before it. */
		if (!drawn) {
			error = execute_drain(session, execute);
			if (error == 0)
				error = execute_cpu_copy(session, buffer, image, region, to_image);
			if (error != 0)
				return error;

			/* Counted for nvrtx.vk.perf=1; the next draw writes its target anew. */
			nvrtx_vk_gpu_copied(session->owner, (uint64_t)region->imageExtent.width * NVRTX_VK_PIXEL_BYTES * region->imageExtent.height);
			state->target_written = false;
			continue;
		}

		/* Drawn: the destination's region samples the source's, one to one. */
		kern_memset(&quad, 0, sizeof(quad));
		quad.filter = VK_FILTER_NEAREST;
		if (to_image) {
			/* The texture from the 32-byte boundary at or before the region; the region skew pixels into its rows. */
			skew = (uint32_t)(start % EXECUTE_TEXTURE_START_ALIGN) / NVRTX_VK_PIXEL_BYTES;
			error = execute_image_target(image, level, image->format, &quad.target);
			quad.x0 = region->imageOffset.x;
			quad.y0 = region->imageOffset.y;
			quad.source = start - (uint64_t)skew * NVRTX_VK_PIXEL_BYTES;
			quad.source_width = skew + region->imageExtent.width;
			quad.source_height = region->imageExtent.height;
			quad.source_pitch = pitch;
			quad.source_format = image->format;
			quad.u0 = (int32_t)skew;
			quad.v0 = 0;
		} else {
			quad.target.address = start;
			quad.target.width = region->imageExtent.width;
			quad.target.height = region->imageExtent.height;
			quad.target.pitch = pitch;
			quad.target.format = image->format;
			quad.x0 = 0;
			quad.y0 = 0;
			error = execute_image_source(image, level, &quad);
			quad.u0 = region->imageOffset.x;
			quad.v0 = region->imageOffset.y;
		}
		if (error != 0)
			return error;
		quad.x1 = quad.x0 + (int32_t)region->imageExtent.width;
		quad.y1 = quad.y0 + (int32_t)region->imageExtent.height;
		quad.u1 = quad.u0 + (int32_t)region->imageExtent.width;
		quad.v1 = quad.v0 + (int32_t)region->imageExtent.height;
		error = execute_quad(session, execute, state, &quad);
		if (error != 0)
			return error;
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies one region between a buffer and an image by the CPU, row by row
 * through the BAR1 window (the GPU is idle: the work before has run); the
 * two storages take their place in the window now if they have none.
 */
static int
execute_cpu_copy(
	struct nvrtx_vk_session *session,
	const struct nvrtx_vk_buffer *buffer,
	const struct nvrtx_vk_image *image,
	const VkBufferImageCopy *region,
	bool to_image)
{
	uint8_t *buffer_cpu;
	uint8_t *image_cpu;
	uint8_t *buffer_row;
	uint8_t *image_row;
	uint64_t level_offset;
	uint32_t level_width;
	uint32_t level_height;
	uint32_t level_pitch;
	uint32_t row_pixels;
	uint32_t bytes;
	uint32_t x;
	uint32_t y;
	int error;

	/* The two views. */
	error = nvrtx_vk_buffer_cpu(session, buffer, &buffer_cpu);
	if (error == 0)
		error = nvrtx_vk_image_cpu(session, image, &image_cpu);
	if (error != 0)
		return error;

	/* The rows: the buffer's at its row length, the image's at its level's pitch. */
	nvrtx_vk_image_level(image, region->imageSubresource.mipLevel, &level_offset, &level_width, &level_height, &level_pitch);
	row_pixels = region->bufferRowLength;
	if (row_pixels == 0U)
		row_pixels = region->imageExtent.width;
	bytes = region->imageExtent.width * NVRTX_VK_PIXEL_BYTES;
	for (y = 0U; y < region->imageExtent.height; y++) {
		buffer_row = buffer_cpu + region->bufferOffset + (uint64_t)y * row_pixels * NVRTX_VK_PIXEL_BYTES;

		/* A tiled image's pixels each where its GOB keeps it. */
		if (image->tiled) {
			for (x = 0U; x < region->imageExtent.width; x++) {
				image_row = image_cpu + level_offset +
				    execute_gob_offset(level_pitch, ((uint32_t)region->imageOffset.x + x) * NVRTX_VK_PIXEL_BYTES,
						       (uint32_t)region->imageOffset.y + y);
				if (to_image) {
					kern_memcpy(image_row, buffer_row + x * NVRTX_VK_PIXEL_BYTES, NVRTX_VK_PIXEL_BYTES);
				} else {
					kern_memcpy(buffer_row + x * NVRTX_VK_PIXEL_BYTES, image_row, NVRTX_VK_PIXEL_BYTES);
				}
			}
			continue;
		}

		/* A pitch-linear image's row at once. */
		image_row = image_cpu + level_offset + (uint64_t)(region->imageOffset.y + (int32_t)y) * level_pitch +
		    (uint64_t)region->imageOffset.x * NVRTX_VK_PIXEL_BYTES;
		if (to_image) {
			kern_memcpy(image_row, buffer_row, bytes);
		} else {
			kern_memcpy(buffer_row, image_row, bytes);
		}
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Reports where a byte of a block-linear surface in one-GOB blocks lies: its
 * GOB (rows of GOBs, pitch bytes a row wide), then its 16-byte piece within
 * the GOB and its place in the piece.
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/nil/copy.rs
 * (CopyGOBTuring2D::for_each_16b, the 16-byte pieces of a 64-byte by 8-row
 * GOB).
 */
static uint64_t
execute_gob_offset(
	uint32_t pitch,
	uint32_t x,
	uint32_t y)
{
	uint64_t gob;
	uint32_t inside;

	/* The GOB, then the byte within it. */
	gob = (uint64_t)(y / NVRTX_VK_GOB_HEIGHT) * (pitch / NVRTX_VK_GOB_WIDTH) + x / NVRTX_VK_GOB_WIDTH;
	x %= NVRTX_VK_GOB_WIDTH;
	y %= NVRTX_VK_GOB_HEIGHT;
	inside = (x & 15U) | ((y & 3U) << 4) | ((x & 16U) << 2) | ((y & 4U) << 5) | ((x & 32U) << 3);

	/* Succeeded: the byte's offset. */
	return gob * NVRTX_VK_GOB_BYTES + inside;
}

/*
 * Copies regions between two buffers by the CPU through the BAR1 window,
 * after the work before it ran (the copy is a one-off upload: a model's
 * vertices and indices from a staging buffer).
 */
static int
execute_copy_buffer(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *source;
	const struct nvrtx_vk_buffer *destination;
	const VkBufferCopy *regions;
	uint8_t *source_cpu;
	uint8_t *destination_cpu;
	uint32_t index;
	int error;

	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;

	/* Each region inside both buffers. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		if (regions[index].srcOffset > source->size || regions[index].size > source->size - regions[index].srcOffset)
			return EINVAL;
		if (regions[index].dstOffset > destination->size || regions[index].size > destination->size - regions[index].dstOffset)
			return EINVAL;
	}

	/* The work before has run; the two views. */
	error = execute_drain(session, execute);
	if (error == 0)
		error = nvrtx_vk_buffer_cpu(session, source, &source_cpu);
	if (error == 0)
		error = nvrtx_vk_buffer_cpu(session, destination, &destination_cpu);
	if (error != 0)
		return error;

	/* The regions (kern_memmove: they may overlap within one buffer). */
	for (index = 0U; index < record->region_count; index++) {
		kern_memmove(destination_cpu + regions[index].dstOffset, source_cpu + regions[index].srcOffset, (size_t)regions[index].size);
		nvrtx_vk_gpu_copied(session->owner, regions[index].size);
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/* Clears a whole image, every level, to a colour (it has one layer). */
static int
execute_clear_image(
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_image *image;
	struct nvrtx_exec_target target;
	uint32_t level;
	uint8_t *push;
	int error;

	image = record->objects[0]->payload;

	/* Each level as a target, then its whole rectangle cleared. */
	push = execute->cpu + EXECUTE_PUSH_AT;
	for (level = 0U; level < image->levels; level++) {
		error = execute_image_target(image, level, image->format, &target);
		if (error != 0)
			return error;
		execute->at = nvrtx_exec_target(push, execute->at, &target);
		execute->at = nvrtx_exec_clear(push, execute->at, record->values, 0U, 0U, target.width, target.height);
	}
	state->target_written = false;

	/* Succeeded: the clear is written. */
	return 0;
}

/*
 * Draws one internal quad: the destination as the target, the viewport
 * and scissor its region, no blending, the corners, the constants (the
 * quad over the whole viewport and the source's region in pixels) with
 * the source's handle, and the executor's programs.
 */
static int
execute_quad(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct execute_quad *quad)
{
	struct nvrtx_exec_sampler sampler;
	struct nvrtx_exec_vertex vertex;
	struct nvrtx_exec_blend blend;
	struct nvrtx_exec_depth depth;
	static const uint32_t identity[4] = {
		VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A
	};
	uint32_t viewport[6];
	uint32_t header[8];
	uint32_t sampled[8];
	uint32_t handle;
	uint8_t *push;
	uint8_t *constants;
	uint64_t constants_va;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	bool inside;
	int error;

	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(state);

	/* A region inside the target; nothing to draw for an empty one. */
	inside = execute_region_inside(quad->x0, quad->y0, (uint32_t)(quad->x1 - quad->x0), (uint32_t)(quad->y1 - quad->y0),
				       quad->target.width, quad->target.height);
	if (quad->x1 <= quad->x0 || quad->y1 <= quad->y0)
		return 0;
	if (!inside)
		return EINVAL;

	/* The source's header (a tiled source's level as a surface of its own) and a sampler in pixels, clamped. */
	if (quad->source_tiled) {
		nvrtx_exec_texture_header_tiled(header, quad->source, quad->source_width, quad->source_height, 1U, 0U, 1U,
						quad->source_format, identity);
	} else {
		nvrtx_exec_texture_header(header, quad->source, quad->source_width, quad->source_height, quad->source_pitch,
					  quad->source_format, identity);
	}
	kern_memset(&sampler, 0, sizeof(sampler));
	sampler.mag_filter = quad->filter;
	sampler.min_filter = quad->filter;
	sampler.address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.unnormalized = 1U;
	nvrtx_exec_sampler_words(sampled, &sampler);
	error = execute_pool_entry(execute, header, sampled, &handle);
	if (error != 0)
		return error;

	/* The constants: the quad from -1 to 1, the source's region, the handle. */
	error = execute_constants(execute, &constants, &constants_va);
	if (error != 0)
		return error;
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_RECT, EXECUTE_FLOAT_MINUS_ONE);
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_RECT + 4U, EXECUTE_FLOAT_MINUS_ONE);
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_RECT + 8U, EXECUTE_FLOAT_ONE);
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_RECT + 12U, EXECUTE_FLOAT_ONE);
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_TEXTURE, nvrtx_exec_float(quad->u0));
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_TEXTURE + 4U, nvrtx_exec_float(quad->v0));
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_TEXTURE + 8U, nvrtx_exec_float(quad->u1));
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_TEXTURE + 12U, nvrtx_exec_float(quad->v1));
	nvrtx_put32(constants, NVRTX_SM75_CB_TEXTURES, handle);

	/* The target, the viewport and the scissor. */
	push = execute->cpu + EXECUTE_PUSH_AT;
	x0 = quad->x0;
	y0 = quad->y0;
	x1 = quad->x1;
	y1 = quad->y1;
	execute->at = nvrtx_exec_target(push, execute->at, &quad->target);
	execute_full_viewport(viewport, x0, y0, x1, y1);
	execute->at = nvrtx_exec_viewport(push, execute->at, viewport);
	execute->at = nvrtx_exec_scissor(push, execute->at, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);

	/* No blending, every channel. */
	kern_memset(&blend, 0, sizeof(blend));
	blend.write_mask = 0xfU;
	execute->at = nvrtx_exec_blend(push, execute->at, &blend);

	/* No culling and no depth test (a pass's draws may have left them on). */
	execute->at = nvrtx_exec_raster(push, execute->at, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	kern_memset(&depth, 0, sizeof(depth));
	execute->at = nvrtx_exec_depth(push, execute->at, &depth);

	/* The corners at location 0, the constants, the programs, two triangles. */
	kern_memset(&vertex, 0, sizeof(vertex));
	vertex.attribute_count = 1U;
	vertex.attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
	vertex.streams[0].enabled = 1U;
	vertex.streams[0].address = execute->va + EXECUTE_CORNERS_AT;
	vertex.streams[0].bytes = EXECUTE_CORNERS * EXECUTE_CORNER_STRIDE;
	vertex.streams[0].stride = EXECUTE_CORNER_STRIDE;
	execute->at = nvrtx_exec_vertex(push, execute->at, &vertex);
	execute->at = nvrtx_push_draw_constants(push, execute->at, constants_va, EXECUTE_CONSTANTS_SIZE);
	execute->at = nvrtx_push_draw_pipeline(push, execute->at, execute->quad_vertex, execute->quad_vertex_gprs,
					       execute->quad_fragment, execute->quad_fragment_gprs);
	execute->at = nvrtx_exec_draw(push, execute->at, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0U, EXECUTE_CORNERS, 1U, 0U);
	execute->quads++;

	/* Succeeded: the quad is written. */
	return 0;
}

/*
 * Describes a level of an image as a colour target in a format, with no
 * depth target; EIO without storage, EINVAL for a format the executor does
 * not render to.
 */
static int
execute_image_target(
	const struct nvrtx_vk_image *image,
	uint32_t level,
	uint32_t format,
	struct nvrtx_exec_target *target)
{
	uint64_t va;
	uint64_t offset;
	uint8_t *cpu;
	uint32_t code;
	bool renderable;
	int error;

	/* The image's storage. */
	kern_memset(target, 0, sizeof(*target));
	error = nvrtx_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;

	/* A format rendered to, of a colour image. */
	renderable = nvrtx_exec_target_format(format, &code);
	if (!renderable || image->depth)
		return EINVAL;

	/* Succeeded: the target is the image's level. */
	nvrtx_vk_image_level(image, level, &offset, &target->width, &target->height, &target->pitch);
	target->address = va + offset;
	target->format = format;
	target->block_linear = 0U;
	if (image->tiled)
		target->block_linear = 1U;
	return 0;
}

/*
 * Puts a depth image beside a colour target as its depth target; one with
 * a stencil has its pages given the S8Z24 memory kind the first time (the
 * 3D engine refuses a stencil target in pages of another kind).  EIO
 * without storage, EINVAL for a colour image, or the kind's error.
 */
static int
execute_depth_target(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_image *image,
	struct nvrtx_exec_target *target)
{
	const struct nvrtx_vk_memory *memory;
	uint8_t *cpu;
	int error;

	/* A depth image with storage. */
	if (!image->depth)
		return EINVAL;
	error = nvrtx_vk_image_address(image, &target->depth_address, &cpu);
	if (error != 0)
		return error;

	/* A stencil image's pages of the S8Z24 kind, once. */
	if (image->stencil && !image->kinded) {
		memory = image->memory->payload;
		error = nvrtx_vk_memory_kind(session->owner, memory->backing, image->offset, target->depth_address, image->bytes,
					     NVRTX_MMU_KIND_S8Z24);
		if (error != 0)
			return error;
		image->kinded = true;
	}

	/* Succeeded: the target has its depth target, and its stencil when the format has one. */
	target->depth_pitch = image->pitch;
	target->depth_height = image->height;
	target->depth_stencil = 0U;
	if (image->stencil)
		target->depth_stencil = 1U;
	return 0;
}

/* Describes a level of an image as a quad's source; EIO without storage, EINVAL for a depth image. */
static int
execute_image_source(
	const struct nvrtx_vk_image *image,
	uint32_t level,
	struct execute_quad *quad)
{
	uint64_t va;
	uint64_t offset;
	uint8_t *cpu;
	int error;

	/* A colour image with storage (a depth or video image is no quad's source). */
	if (image->depth || image->video)
		return EINVAL;
	error = nvrtx_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;

	/* Succeeded: the source is the image's level. */
	nvrtx_vk_image_level(image, level, &offset, &quad->source_width, &quad->source_height, &quad->source_pitch);
	quad->source = va + offset;
	quad->source_format = image->format;
	quad->source_tiled = image->tiled;
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
	/* x, y, width, height, min depth, max depth. */
	viewport[0] = nvrtx_exec_float(x0);
	viewport[1] = nvrtx_exec_float(y0);
	viewport[2] = nvrtx_exec_float(x1 - x0);
	viewport[3] = nvrtx_exec_float(y1 - y0);
	viewport[4] = 0U;
	viewport[5] = EXECUTE_FLOAT_ONE;
}
