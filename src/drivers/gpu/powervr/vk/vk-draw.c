/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Venus executor's graphics pipelines and draw records: a
 * vkCreateGraphicsPipelines record decoded into its fixed state, its two
 * shaders compiled with the pipeline's keys and the varyings between
 * them (render-draw.c), and the records of the state a draw reads
 * (vertex and index buffers, viewport, scissor, push constants) and of
 * the draws.  The render jobs pick them up (vk-pass.c); nothing here
 * kicks.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/render-draw.h"
#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/vk/vk-draw.h"
#include "drivers/gpu/powervr/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most bytes of one compiled shader. */
#define GRAPHICS_CODE_MAX	(64U * 1024U)

/* The stages a graphics record may carry. */
#define GRAPHICS_STAGES_MAX	4U

static int decode_info(struct powervr_vk_session *session, struct i915_wire_reader *reader, VkGraphicsPipelineCreateInfo *info, uint64_t *render_pass, const struct powervr_vk_shader **vertex, const struct powervr_vk_shader **fragment);
static int compile_stage(const struct powervr_vk_shader *shader, uint32_t stage, const struct powervr_shader_key *key, struct powervr_shader_workspace *workspace, struct powervr_shader_program *program, uint8_t **code);
static int record_vertex_buffers(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_record *entry);
static int record_index_buffer(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_record *entry);
static int record_viewport(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_record *entry, bool scissor);
static int record_push(struct i915_wire_reader *reader, struct powervr_vk_record *entry);

/*
 * Decodes one VkGraphicsPipelineCreateInfo (as libvulkan's
 * pipeline_encode_graphics sends it) into the pipeline: its fixed state
 * and keys against its render pass, its shaders compiled and the
 * varyings between them.  EINVAL when the record did not decode,
 * ENOTSUP for one the executor does not run.
 *
 * Procedure referenced: i915/vk/vk-pipeline.c (i915_gfx_decode_pipeline,
 * the same wire form).
 */
int
powervr_vk_graphics_decode(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_pipeline *pipeline)
{
	VkGraphicsPipelineCreateInfo info;
	struct powervr_shader_workspace *workspace;
	struct powervr_vk_graphics *graphics;
	const struct powervr_vk_shader *vertex;
	const struct powervr_vk_shader *fragment;
	const struct powervr_pass *pass;
	uint64_t render_pass;
	int error;

	/* The record. */
	error = decode_info(session, reader, &info, &render_pass, &vertex, &fragment);
	if (error == EINVAL)
		return EINVAL;

	/*
	 * A pipeline of a known pass and two known stages.  A module's status
	 * says only whether it has a compute entry (vk-layout.c), so the two
	 * graphics stages are judged by their compiles below, not by it.
	 */
	if (error != 0)
		return error;
	pass = powervr_vk_payload(session, I915_VK_OBJ_RENDER_PASS, render_pass);
	if (pass == NULL || vertex == NULL || fragment == NULL)
		return ENOTSUP;

	/* The graphics state. */
	graphics = kern_calloc(1, sizeof(*graphics));
	if (graphics == NULL)
		return ENOMEM;
	pipeline->graphics = graphics;
	error = powervr_draw_fixed_build(&info, pass, &graphics->fixed);
	if (error != 0) {
		kern_logf("powervr: graphics pipeline: a state the executor does not take (%d)\n", error);
		return ENOTSUP;
	}

	/* The two shaders with their keys, then the varyings between them. */
	workspace = kern_malloc(powervr_shader_workspace_size());
	if (workspace == NULL)
		return ENOMEM;
	error = compile_stage(vertex, POWERVR_SHADER_VERTEX, &graphics->fixed.vertex_key, workspace, &graphics->vertex, &graphics->vertex_code);
	if (error == 0)
		error = compile_stage(fragment, POWERVR_SHADER_FRAGMENT, &graphics->fixed.fragment_key, workspace, &graphics->fragment, &graphics->fragment_code);
	kern_free(workspace);
	if (error == 0)
		error = powervr_draw_varyings_build(&graphics->vertex, &graphics->fragment, &graphics->varyings);
	if (error != 0) {
		kern_logf("powervr: graphics pipeline: the shaders did not compile (%d)\n", error);
		return error == ENOMEM ? ENOMEM : ENOTSUP;
	}

	/* A fragment shader that may discard makes its draws punch-through. */
	graphics->fixed.discards = graphics->fragment.discards != 0;

	/* Succeeded: the pipeline is ready to draw with. */
	return 0;
}

/* Frees a graphics pipeline's state and code. */
void
powervr_vk_graphics_free(
	struct powervr_vk_graphics *graphics)
{
	/* Nothing to free. */
	if (graphics == NULL)
		return;

	/* The code, then the state. */
	kern_free(graphics->vertex_code);
	kern_free(graphics->fragment_code);
	kern_free(graphics);
}

/*
 * Records a draw-state command or a draw: vkCmdBindVertexBuffers,
 * vkCmdBindIndexBuffer, vkCmdSetViewport, vkCmdSetScissor,
 * vkCmdPushConstants, vkCmdDraw, vkCmdDrawIndexed.  EINVAL for a command
 * that did not decode, ENOTSUP for one the executor does not run.
 */
int
powervr_vk_record_draw(
	struct powervr_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	int error;

	/* Each command by its arguments. */
	if (opcode == GPU_OP_CMD_BIND_VERTEX_BUFFERS) {
		error = record_vertex_buffers(session, reader, entry);
	} else if (opcode == GPU_OP_CMD_BIND_INDEX_BUFFER) {
		error = record_index_buffer(session, reader, entry);
	} else if (opcode == GPU_OP_CMD_SET_VIEWPORT) {
		error = record_viewport(session, reader, entry, false);
	} else if (opcode == GPU_OP_CMD_SET_SCISSOR) {
		error = record_viewport(session, reader, entry, true);
	} else if (opcode == GPU_OP_CMD_PUSH_CONSTANTS) {
		error = record_push(reader, entry);
	} else if (opcode == GPU_OP_CMD_DRAW) {
		/* [vertices][instances][first vertex][first instance]. */
		entry->values[0] = drv_i915_wire_read_u32(reader);
		entry->values[1] = drv_i915_wire_read_u32(reader);
		entry->values[2] = drv_i915_wire_read_u32(reader);
		error = drv_i915_wire_read_u32(reader) != 0 ? ENOTSUP : 0;
	} else if (opcode == GPU_OP_CMD_DRAW_INDEXED) {
		/* [indices][instances][first index][vertex offset][first instance]; first index and offset kept. */
		entry->values[0] = drv_i915_wire_read_u32(reader);
		entry->values[1] = drv_i915_wire_read_u32(reader);
		entry->values[2] = drv_i915_wire_read_u32(reader);
		entry->first = drv_i915_wire_read_u32(reader);
		error = drv_i915_wire_read_u32(reader) != 0 ? ENOTSUP : 0;
	} else {
		return EINVAL;
	}

	/* A command that did not decode. */
	if (reader->error != 0)
		return EINVAL;

	/* A command that cannot be run, or the record. */
	if (error != 0)
		return error;

	/* Succeeded: the command is recorded. */
	return 0;
}

/*
 * Decodes the record: [head: u32 sType][u64 next][u32 flags][u32 stages]
 * [u64 stages]{VkPipelineShaderStageCreateInfo}, each state behind its
 * presence word (vertex input, input assembly, tessellation, viewport,
 * rasterisation, multisample, depth and stencil, colour blend, dynamic),
 * then [layout][render pass][subpass][base pipeline][base index].
 */
static int
decode_info(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	VkGraphicsPipelineCreateInfo *info,
	uint64_t *render_pass,
	const struct powervr_vk_shader **vertex,
	const struct powervr_vk_shader **fragment)
{
	VkPipelineShaderStageCreateInfo *stages;
	VkPipelineVertexInputStateCreateInfo *input;
	VkPipelineInputAssemblyStateCreateInfo *assembly;
	VkPipelineTessellationStateCreateInfo *tessellation;
	VkPipelineViewportStateCreateInfo *viewport;
	VkPipelineRasterizationStateCreateInfo *raster;
	VkPipelineMultisampleStateCreateInfo *multisample;
	VkPipelineDepthStencilStateCreateInfo *depth;
	VkPipelineColorBlendStateCreateInfo *blend;
	VkPipelineDynamicStateCreateInfo *dynamic;
	uint64_t count;
	uint64_t index;
	int refused;
	uint64_t present;

	/* The head and the stages. */
	kern_memset(info, 0, sizeof(*info));
	*vertex = NULL;
	*fragment = NULL;
	refused = 0;
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	info->flags = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > GRAPHICS_STAGES_MAX)
		return EINVAL;
	stages = powervr_vk_arena_take(session, (size_t)count * sizeof(*stages));
	if (count != 0 && stages == NULL)
		return EINVAL;
	for (index = 0; index < count; index++) {
		kern_memset(&stages[index], 0, sizeof(stages[index]));
		i915_vkc_dec_VkPipelineShaderStageCreateInfo(reader, &session->arena, &stages[index]);
		if (stages[index].pSpecializationInfo != NULL && stages[index].pSpecializationInfo->mapEntryCount != 0)
			refused = ENOTSUP;
		if (stages[index].stage == VK_SHADER_STAGE_VERTEX_BIT)
			*vertex = powervr_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)stages[index].module);
		if (stages[index].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
			*fragment = powervr_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)stages[index].module);
	}
	info->stageCount = (uint32_t)count;
	info->pStages = stages;

	/* Each state behind its presence word. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		input = powervr_vk_arena_take(session, sizeof(*input));
		if (input == NULL)
			return EINVAL;
		kern_memset(input, 0, sizeof(*input));
		i915_vkc_dec_VkPipelineVertexInputStateCreateInfo(reader, &session->arena, input);
		info->pVertexInputState = input;
	}

	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		assembly = powervr_vk_arena_take(session, sizeof(*assembly));
		if (assembly == NULL)
			return EINVAL;
		kern_memset(assembly, 0, sizeof(*assembly));
		i915_vkc_dec_VkPipelineInputAssemblyStateCreateInfo(reader, &session->arena, assembly);
		info->pInputAssemblyState = assembly;
	}

	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		tessellation = powervr_vk_arena_take(session, sizeof(*tessellation));
		if (tessellation == NULL)
			return EINVAL;
		kern_memset(tessellation, 0, sizeof(*tessellation));
		i915_vkc_dec_VkPipelineTessellationStateCreateInfo(reader, &session->arena, tessellation);
		info->pTessellationState = tessellation;
	}

	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		viewport = powervr_vk_arena_take(session, sizeof(*viewport));
		if (viewport == NULL)
			return EINVAL;
		kern_memset(viewport, 0, sizeof(*viewport));
		i915_vkc_dec_VkPipelineViewportStateCreateInfo(reader, &session->arena, viewport);
		info->pViewportState = viewport;
	}

	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		raster = powervr_vk_arena_take(session, sizeof(*raster));
		if (raster == NULL)
			return EINVAL;
		kern_memset(raster, 0, sizeof(*raster));
		i915_vkc_dec_VkPipelineRasterizationStateCreateInfo(reader, &session->arena, raster);
		info->pRasterizationState = raster;
	}

	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		multisample = powervr_vk_arena_take(session, sizeof(*multisample));
		if (multisample == NULL)
			return EINVAL;
		kern_memset(multisample, 0, sizeof(*multisample));
		i915_vkc_dec_VkPipelineMultisampleStateCreateInfo(reader, &session->arena, multisample);
		info->pMultisampleState = multisample;
	}

	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		depth = powervr_vk_arena_take(session, sizeof(*depth));
		if (depth == NULL)
			return EINVAL;
		kern_memset(depth, 0, sizeof(*depth));
		i915_vkc_dec_VkPipelineDepthStencilStateCreateInfo(reader, &session->arena, depth);
		info->pDepthStencilState = depth;
	}

	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		blend = powervr_vk_arena_take(session, sizeof(*blend));
		if (blend == NULL)
			return EINVAL;
		kern_memset(blend, 0, sizeof(*blend));
		i915_vkc_dec_VkPipelineColorBlendStateCreateInfo(reader, &session->arena, blend);
		info->pColorBlendState = blend;
	}

	present = drv_i915_wire_read_u64(reader);
	if (present != 0) {
		dynamic = powervr_vk_arena_take(session, sizeof(*dynamic));
		if (dynamic == NULL)
			return EINVAL;
		kern_memset(dynamic, 0, sizeof(*dynamic));
		i915_vkc_dec_VkPipelineDynamicStateCreateInfo(reader, &session->arena, dynamic);
		info->pDynamicState = dynamic;
	}

	/* The layout, the render pass, the subpass and the base pipeline and index. */
	(void)drv_i915_wire_read_u64(reader);
	*render_pass = drv_i915_wire_read_u64(reader);
	info->subpass = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded unless a stage was refused. */
	if (refused != 0)
		return refused;
	return 0;
}

/* Compiles one stage with its key into its own copy of the code. */
static int
compile_stage(
	const struct powervr_vk_shader *shader,
	uint32_t stage,
	const struct powervr_shader_key *key,
	struct powervr_shader_workspace *workspace,
	struct powervr_shader_program *program,
	uint8_t **code)
{
	uint8_t *buffer;
	int error;

	/* Room for the code. */
	buffer = kern_malloc(GRAPHICS_CODE_MAX);
	if (buffer == NULL)
		return ENOMEM;

	/* The compile. */
	error = powervr_shader_compile(shader->words, shader->word_count, stage, key, workspace, program, buffer, GRAPHICS_CODE_MAX);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	/* Succeeded: the program and its code. */
	*code = buffer;
	return 0;
}

/* vkCmdBindVertexBuffers: [first][u32][u64 count]{buffer}[u64 count]{offset}; the buffers held. */
static int
record_vertex_buffers(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct powervr_vk_vertex_bind *data;
	struct powervr_vk_object *object;
	uint64_t identity;
	uint64_t count;
	uint64_t offsets;
	uint64_t index;
	int refused;
	int retained;

	/* The data. */
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL)
		return ENOMEM;
	entry->data = data;

	/* The first binding and the buffers, held. */
	data->first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > 64U)
		return EINVAL;
	refused = 0;
	if (count > POWERVR_DRAW_VERTEX_BUFFERS || data->first >= POWERVR_DRAW_VERTEX_BUFFERS || count > POWERVR_DRAW_VERTEX_BUFFERS - data->first)
		refused = ENOTSUP;
	for (index = 0; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (refused != 0)
			continue;
		object = powervr_vk_object_find(session, I915_VK_OBJ_BUFFER, identity);
		if (object == NULL) {
			refused = EINVAL;
			continue;
		}

		/* The record holds the buffer. */
		retained = powervr_vk_object_retain(object);
		if (retained != 0) {
			refused = EINVAL;
			continue;
		}
		entry->objects[entry->object_count++] = object;
	}
	data->count = entry->object_count;

	/* The offsets. */
	offsets = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || offsets != count)
		return EINVAL;
	for (index = 0; index < offsets; index++) {
		if (index < POWERVR_DRAW_VERTEX_BUFFERS)
			data->offsets[index] = drv_i915_wire_read_u64(reader);
		else
			(void)drv_i915_wire_read_u64(reader);
	}

	/* A bind that cannot be run. */
	if (refused != 0)
		return refused;

	/* Succeeded: the bind is recorded. */
	return 0;
}

/* vkCmdBindIndexBuffer: [buffer][offset][index type]; 16 and 32-bit indices. */
static int
record_index_buffer(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct powervr_vk_object *object;
	uint64_t identity;
	uint64_t offset;
	uint32_t type;
	int retained;

	/* The buffer, the offset and the type. */
	identity = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	if (type != VK_INDEX_TYPE_UINT16 && type != VK_INDEX_TYPE_UINT32)
		return ENOTSUP;

	/* The buffer, held; the offset's halves and the size. */
	object = powervr_vk_object_find(session, I915_VK_OBJ_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	retained = powervr_vk_object_retain(object);
	if (retained != 0)
		return EINVAL;
	entry->objects[entry->object_count++] = object;
	entry->values[0] = (uint32_t)offset;
	entry->values[1] = (uint32_t)(offset >> 32);
	entry->values[2] = type == VK_INDEX_TYPE_UINT16 ? 2U : 4U;

	/* Succeeded: the bind is recorded. */
	return 0;
}

/* vkCmdSetViewport and vkCmdSetScissor: [first][u32][u64 count]{record}; viewport or scissor 0 only. */
static int
record_viewport(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry,
	bool scissor)
{
	struct powervr_vk_state_record *data;
	VkViewport viewport;
	VkRect2D rect;
	uint32_t first;
	uint64_t count;
	uint64_t index;

	/* The data. */
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL)
		return ENOMEM;
	entry->data = data;

	/* The first and the records; the first record of number 0 is kept. */
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > 16U)
		return EINVAL;
	for (index = 0; index < count; index++) {
		if (scissor) {
			i915_vkc_dec_VkRect2D(reader, &session->arena, &rect);
			if (index == 0)
				data->scissor = rect;
		} else {
			i915_vkc_dec_VkViewport(reader, &session->arena, &viewport);
			if (index == 0)
				__builtin_memcpy(&data->viewport, &viewport, sizeof(viewport));
		}
	}
	if (reader->error != 0)
		return EINVAL;

	/* Only number 0 is run. */
	if (first != 0 || count != 1)
		return ENOTSUP;

	/* Succeeded: the state is recorded. */
	return 0;
}

/* vkCmdPushConstants: [layout][stages][offset][size][u64 size][bytes]; within 128 bytes. */
static int
record_push(
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct powervr_vk_state_record *data;
	uint64_t size;
	uint32_t offset;
	uint32_t bytes;

	/* The data. */
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL)
		return ENOMEM;
	entry->data = data;

	/* The range. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u32(reader);
	bytes = drv_i915_wire_read_u32(reader);
	size = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || size != bytes)
		return EINVAL;

	/* Within the 128 bytes (the bytes cannot be skipped otherwise; as i915 does). */
	if (offset > POWERVR_DRAW_PUSH_BYTES || bytes > POWERVR_DRAW_PUSH_BYTES - offset)
		return EINVAL;
	i915_vkc_read_bytes(reader, data->push, (size_t)bytes);
	if (reader->error != 0)
		return EINVAL;
	data->push_offset = offset;
	data->push_bytes = bytes;

	/* Succeeded: the update is recorded. */
	return 0;
}
