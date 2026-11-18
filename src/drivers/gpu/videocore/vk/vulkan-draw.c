/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Two serialized state walks validate the complete graphics recording before any native preparation callback sees a prefix. */
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
#include "drivers/gpu/videocore/vk/vulkan-video.h"
#endif
#include "drivers/gpu/videocore/vk/vulkan-draw.h"
#include "drivers/gpu/videocore/vk/vulkan-barrier.h"
#include "drivers/gpu/videocore/vk/vulkan-transfer.h"
#include "drivers/gpu/videocore/vk/vulkan-buffer-copy.h"
#include "drivers/gpu/videocore/vk/vulkan-occlusion.h"

static int walk_records(struct bcm2711_vulkan_command_buffer *command, struct bcm2711_vulkan_draw_state *state, bcm2711_vulkan_draw_prepare_t prepare, void *payload);
static void bind_sets(struct bcm2711_vulkan_draw_set *sets, const struct bcm2711_vulkan_record *record);
static int dispatch_point(struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record, bcm2711_vulkan_draw_prepare_t prepare, void *payload);
static void push_words(struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);
static void set_dynamic(struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);
static void set_stencil(uint32_t *values, uint32_t faces, uint32_t value);
static int occlusion_point(struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);

/*
 * Validates all recorded draws before preparing independently owned native work under the controller mutex.
 */
int
bcm2711_vulkan_draw_walk(
	struct bcm2711_vulkan_command_buffer *command,
	bcm2711_vulkan_draw_prepare_t prepare,
	void *payload)
{
	struct bcm2711_vulkan_draw_state *state;
	int error;

	/* Ordinary descriptor mutations or public set withdrawal cannot authorize native work from an old recording. */
	if (command == NULL)
		return EINVAL;

	/*
	 * A recording that End refused is turned down with the errno of End's
	 * reply (out of host memory, else a feature not present), so that a
	 * submission travelling with that End fails as the End did.
	 */
	if (command->state == BCM2711_VULKAN_COMMAND_INVALID && command->recording_error != 0) {
		if (command->recording_error == ENOMEM)
			return ENOMEM;
		return ENOTSUP;
	}

	/* Validate the saved descriptor generations before interpreting any executable state. */
	error = bcm2711_vulkan_record_current(command);
	if (error != 0)
		return error;

	/* Only a complete executable primary can be accepted for preparation. */
	if (command->state != BCM2711_VULKAN_COMMAND_EXECUTABLE)
		return EINVAL;

	/* Another native user permits reuse only when the primary declared simultaneous use. */
	if (command->pending != 0 && (command->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) == 0)
		return EBUSY;

	/* Temporary graphics state lives on the heap instead of consuming the small arm64 kernel stack. */
	state = kern_calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* Admit the entire ordered graph without calling any preparation hook; a refusal names the command. */
	error = walk_records(command, state, NULL, NULL);
	if (error != 0) {
		kern_logf("v3d: recorded command %u refused at submit (%d)\n", state->opcode, error);
		kern_free(state);
		return error;
	}

	/* A complete valid first walk permits CPU-only preparation; callback failure still requires caller-owned prefix rollback. */
	if (prepare != NULL) {
		kern_memset(state, 0, sizeof(*state));
		error = walk_records(command, state, prepare, payload);
	}

	/* No borrowed graphics selections escape the serialized invocation. */
	kern_free(state);
	if (error != 0)
		return error;

	/* Succeeded: every complete draw state was validated before independent native preparation began. */
	return 0;
}

/* Resolves each immutable graphics event in order without allowing native callbacks to mutate recording or descriptor state. */
static int
walk_records(
	struct bcm2711_vulkan_command_buffer *command,
	struct bcm2711_vulkan_draw_state *state,
	bcm2711_vulkan_draw_prepare_t prepare,
	void *payload)
{
	const struct bcm2711_vulkan_command_node *node;
	const struct bcm2711_vulkan_record *record;
	uint32_t index;
	bool selected;
	int error;

	/* Record validation already bounds every copied field; traversal supplies the state consumed by each actual draw. */
	for (node = command->first; node != NULL; node = node->next) {
		record = (const struct bcm2711_vulkan_record *)node;
		state->opcode = record->opcode;
		selected = false;

		/* Each event updates one ordinary graphics selection or emits a complete pass/draw preparation point. */
		switch (record->opcode) {
		case GPU_OP_CMD_COPY_BUFFER:
		case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
		case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
			/* Synchronous coherent transfer work has its own complete immutable owner outside graphics pass state. */
			if (state->pass != NULL)
				return EINVAL;
			error = bcm2711_vulkan_buffer_copy_validate(record, command->owner.device);
			if (error != 0)
				return error;
			selected = true;
			break;
		case GPU_OP_CMD_COPY_IMAGE:
		case GPU_OP_CMD_BLIT_IMAGE:
		case GPU_OP_CMD_RESOLVE_IMAGE:
			/* Complete transfer nodes keep their copied regions and independently retained images outside graphics pass state. */
			if (state->pass != NULL)
				return EINVAL;
			error = bcm2711_vulkan_transfer_validate((const struct bcm2711_vulkan_transfer *)record, command->owner.device);
			if (error != 0)
				return error;
			selected = true;
			break;
		case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
		case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
			/* Clear meta work retains the same typed immutable image without consuming graphics selections or nesting inside a pass. */
			if (state->pass != NULL)
				return EINVAL;
			error = bcm2711_vulkan_record_validate(command, record);
			if (error != 0)
				return error;
			selected = true;
			break;
		case GPU_OP_CMD_PIPELINE_BARRIER:
			/* Complete serial barriers execute outside a render pass and keep every typed dependency through preparation. */
			if (state->pass != NULL)
				return ENOTSUP;
			error = bcm2711_vulkan_barrier_validate((const struct bcm2711_vulkan_barrier *)record, command->owner.device);
			if (error != 0)
				return error;
			selected = true;
			break;
		case GPU_OP_CMD_BEGIN_RENDER_PASS:
			/* A new target cannot replace an unclosed render pass. */
			if (state->pass != NULL)
				return EINVAL;

			/* Even a clear-only pass needs current coherent attachment storage. */
			error = bcm2711_vulkan_draw_target(record);
			if (error != 0)
				return error;
			state->pass = record;
			selected = true;
			break;
		case GPU_OP_CMD_END_RENDER_PASS:
			if (state->pass == NULL)
				return EINVAL;

			/* An occlusion query begun in the pass has ended in it. */
			if (state->occlusion != NULL && state->occlusion_pass == state->pass)
				return EINVAL;
			selected = true;
			break;
		case GPU_OP_CMD_CLEAR_ATTACHMENTS:
			/* The clear ends one native pass of the render pass and starts the next, which loads what the first stored. */
			if (state->pass == NULL)
				return EINVAL;
			selected = true;
			break;
		case GPU_OP_CMD_SET_VIEWPORT:
			state->viewport = record;
			break;
		case GPU_OP_CMD_SET_SCISSOR:
			state->scissor = record;
			break;
		case GPU_OP_CMD_SET_LINE_WIDTH:
		case GPU_OP_CMD_SET_DEPTH_BIAS:
		case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
		case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
		case GPU_OP_CMD_SET_STENCIL_REFERENCE:
			/* The value holds until another command of the same kind replaces it. */
			set_dynamic(state, record);
			break;
		case GPU_OP_CMD_BIND_PIPELINE:
			/* Each bind point keeps its own pipeline; recording kept the compute point as the compute stage. */
			if (record->stages == VK_SHADER_STAGE_COMPUTE_BIT)
				state->compute_pipeline = record->objects[0]->payload;
			else
				state->pipeline = record->objects[0]->payload;
			break;
		case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
			/* Apply ordinary descriptor prefix disturbance before selecting the new sets of the bind point. */
			if (record->stages == VK_SHADER_STAGE_COMPUTE_BIT)
				bind_sets(state->compute_sets, record);
			else
				bind_sets(state->sets, record);
			break;
		case GPU_OP_CMD_DISPATCH:
		case GPU_OP_CMD_DISPATCH_INDIRECT:
			/* A dispatch runs outside a render pass; its view of the compute bind point is validated and prepared. */
			if (state->pass != NULL)
				return EINVAL;
			error = dispatch_point(state, record, prepare, payload);
			if (error != 0)
				return error;
			break;
		case GPU_OP_CMD_PUSH_CONSTANTS:
			/* Update only the requested graphics stages without clearing unrelated pushed values. */
			push_words(state, record);
			break;
		case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
			/* Partial binding leaves every unselected vertex stream unchanged. */
			for (index = 0; index < record->count; index++) {
				state->vertices[record->first + index] = record->objects[index]->payload;
				state->offsets[record->first + index] = record->offsets[index];
			}

			break;
		case GPU_OP_CMD_BIND_INDEX_BUFFER:
			/* Indexed draws read this buffer until another bind replaces it. */
			state->indices = record->objects[0]->payload;
			state->index_offset = record->offsets[0];
			state->index_type = record->first;
			break;
		case GPU_OP_CMD_DRAW:
		case GPU_OP_CMD_DRAW_INDEXED:
			/* Resolve exact current shader inputs and complete logical fetch ranges. */
			error = bcm2711_vulkan_draw_validate(state, record);
			if (error != 0)
				return error;
			selected = true;
			break;
		default:
			/* An occlusion query's begin changes the walk's state; its end and a reset run in the job. */
			if (bcm2711_vulkan_occlusion_record(record) != NULL) {
				error = occlusion_point(state, record);
				if (error != 0)
					return error;
				selected = record->opcode != GPU_OP_CMD_BEGIN_QUERY;
				break;
			}
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
			/* Video and status query commands run on the CPU outside any render pass. */
			if (bcm2711_vulkan_video_event(record->opcode)) {
				if (state->pass != NULL)
					return EINVAL;
				selected = true;
				break;
			}
#endif
			return ENOTSUP;
		}

		/* Native preparation sees valid borrowed state only; the callback retains every eventual DMA input independently. */
		if (selected && prepare != NULL) {
			error = prepare(payload, state, record);
			if (error != 0)
				return error;
		}

		/* End callbacks still see their complete target, then subsequent passes begin without a current attachment. */
		if (record->opcode == GPU_OP_CMD_END_RENDER_PASS)
			state->pass = NULL;
	}

	/* An incomplete pass is never handed to a native queue as a complete control list, nor an active query left open. */
	if (state->pass != NULL || state->occlusion != NULL)
		return EINVAL;

	/* Succeeded: the ordered primary record graph has no incomplete render pass or refused draw. */
	return 0;
}

/* Applies descriptor prefix disturbance before installing each exact set selected by one binding command at one bind point. */
static void
bind_sets(
	struct bcm2711_vulkan_draw_set *sets,
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_pipeline_layout *layout;
	const struct bcm2711_vulkan_set_layout *set_layout;
	struct bcm2711_vulkan_draw_set *bound;
	uint32_t index;
	uint32_t number;
	uint32_t lower;
	uint32_t higher;
	uint32_t slot;
	uint32_t consumed;
	int error;

	/* Descriptor disturbance follows ascending set order and remembers the disturbing layout even for now-undefined selections. */
	layout = record->objects[0]->payload;
	consumed = 0;
	for (index = 0; index < record->count; index++) {
		number = record->first + index;

		/* Lower sets survive only when their own prefixes remain compatible with this newly selected layout. */
		for (lower = 0; lower < number; lower++) {
			bound = &sets[lower];
			if (bound->layout == NULL)
				continue;
			error = bcm2711_vulkan_layout_compatible(bound->layout, layout, lower);
			if (error != 0) {
				bound->set = NULL;
				bound->layout = layout;
			}
		}

		/* Replacing an incompatible selection also makes every higher set undefined under the disturbing layout. */
		bound = &sets[number];
		if (bound->layout != NULL) {
			error = bcm2711_vulkan_layout_compatible(bound->layout, layout, number);
			if (error != 0) {
				for (higher = number + 1U; higher < BCM2711_VULKAN_PIPELINE_SETS; higher++) {
					sets[higher].set = NULL;
					sets[higher].layout = layout;
				}
			}
		}

		/* This exact recorded set supersedes only its selected slot after compatibility side effects have been applied. */
		bound->set = record->objects[index + 1U]->payload;
		bound->layout = layout;

		/* Its dynamic uniform buffers take the next offsets in binding order, which recording checked are all given. */
		set_layout = bound->set->layout->payload;
		kern_memset(bound->dynamic, 0, sizeof(bound->dynamic));
		for (slot = 0; slot < set_layout->count; slot++) {
			if (set_layout->bindings[slot].type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
				continue;
			if (consumed < record->dynamic_count)
				bound->dynamic[slot] = record->words[consumed];
			consumed++;
		}
	}

	/* Succeeded: later draws can distinguish unbound, disturbed and compatible descriptor selections. */
	return;
}

/* Updates each requested graphics stage's exact words without disturbing values selected for another stage or compatible pipeline. */
static void
push_words(
	struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *record)
{
	VkShaderStageFlags flag;
	uint32_t stage;
	uint32_t index;
	uint32_t number;

	/* The compute stage's words are its own, kept apart from the graphics stages'. */
	if ((record->stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0) {
		for (index = 0; index < record->count / 4U; index++) {
			number = record->first / 4U + index;
			state->compute_push[number] = record->words[index];
			state->compute_push_layouts[number] = record->objects[0]->payload;
		}
	}

	/* Coordinate and vertex programs consume the vertex stage; fragment programs preserve their own independent pushed words. */
	for (stage = 0; stage < 2; stage++) {
		flag = VK_SHADER_STAGE_VERTEX_BIT;
		if (stage == 1)
			flag = VK_SHADER_STAGE_FRAGMENT_BIT;
		if ((record->stages & flag) == 0)
			continue;

		/* Each written word remembers the exact range definition used by its own push operation. */
		for (index = 0; index < record->count / 4U; index++) {
			number = record->first / 4U + index;
			state->push[stage][number] = record->words[index];
			state->push_layouts[stage][number] = record->objects[0]->payload;
		}
	}

	/* Succeeded: every selected word has an exact stage-specific value and source layout. */
	return;
}

/*
 * Validates one dispatch and hands it to the preparation callback through a
 * view of the state whose pipeline, sets and vertex-stage words are the
 * compute bind point's.
 */
static int
dispatch_point(
	struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *record,
	bcm2711_vulkan_draw_prepare_t prepare,
	void *payload)
{
	struct bcm2711_vulkan_draw_state *view;
	int error;

	/* The view lives on the heap like the state, as the small kernel stack cannot hold it. */
	view = kern_calloc(1, sizeof(*view));
	if (view == NULL)
		return ENOMEM;
	view->pipeline = state->compute_pipeline;
	kern_memcpy(view->sets, state->compute_sets, sizeof(view->sets));
	kern_memcpy(view->push[0], state->compute_push, sizeof(view->push[0]));
	kern_memcpy(view->push_layouts[0], state->compute_push_layouts, sizeof(view->push_layouts[0]));

	/* The pipeline, descriptors and push words the program reads, then the preparation of the dispatch. */
	error = bcm2711_vulkan_dispatch_validate(view, record);
	if (error == 0 && prepare != NULL)
		error = prepare(payload, view, record);
	kern_free(view);
	if (error != 0)
		return error;

	/* Succeeded: the dispatch is valid, and prepared when asked. */
	return 0;
}

/* Keeps the value of one of the dynamic-state commands other than the viewport and the scissor. */
static void
set_dynamic(
	struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *record)
{
	/* Each command replaces only its own value; a stencil command only the faces it names. */
	switch (record->opcode) {
	case GPU_OP_CMD_SET_LINE_WIDTH:
		state->dynamic.line_width = record->words[0];
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		kern_memcpy(state->dynamic.depth_bias, record->words, sizeof(state->dynamic.depth_bias));
		break;
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		kern_memcpy(state->dynamic.blend_constants, record->words, sizeof(state->dynamic.blend_constants));
		break;
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
		set_stencil(state->dynamic.compare_mask, record->first, record->words[0]);
		break;
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
		set_stencil(state->dynamic.write_mask, record->first, record->words[0]);
		break;
	default:
		set_stencil(state->dynamic.reference, record->first, record->words[0]);
		break;
	}

	/* Succeeded: later draws read the new value. */
	return;
}

/* Stores a stencil value for the faces a VkStencilFaceFlags mask names. */
static void
set_stencil(
	uint32_t *values,
	uint32_t faces,
	uint32_t value)
{
	/* The front face is index 0, the back face index 1. */
	if ((faces & VK_STENCIL_FACE_FRONT_BIT) != 0)
		values[0] = value;
	if ((faces & VK_STENCIL_FACE_BACK_BIT) != 0)
		values[1] = value;

	/* Succeeded: the named faces hold the value. */
	return;
}

/*
 * Follows an occlusion query's begin, end or reset: one occlusion query is
 * active at a time (Vulkan allows one per query type), it ends in the
 * render pass it began in (or outside one when it began outside), and a
 * reset outside a render pass leaves the active query alone.
 */
static int
occlusion_point(
	struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_occlusion_pool *pool;

	/* Recording checked the pool and the range. */
	pool = bcm2711_vulkan_occlusion_record(record);
	switch (record->opcode) {
	case GPU_OP_CMD_BEGIN_QUERY:
		if (state->occlusion != NULL)
			return EINVAL;
		state->occlusion = pool;
		state->occlusion_query = record->first;
		state->occlusion_pass = state->pass;
		break;
	case GPU_OP_CMD_END_QUERY:
		if (state->occlusion != pool || state->occlusion_query != record->first || state->occlusion_pass != state->pass)
			return EINVAL;
		state->occlusion = NULL;
		state->occlusion_pass = NULL;
		break;
	default:
		if (state->pass != NULL)
			return EINVAL;
		if (state->occlusion == pool && state->occlusion_query >= record->first && state->occlusion_query - record->first < record->count)
			return EINVAL;
		break;
	}

	/* Succeeded: the walk follows the query. */
	return 0;
}
