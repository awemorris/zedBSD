/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Two serialized state walks validate the complete graphics recording before any native preparation callback sees a prefix. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-draw.h"
#include "drivers/gpu/bcm2711/vulkan-barrier.h"

static int walk_records(struct bcm2711_vulkan_command_buffer *command, struct bcm2711_vulkan_draw_state *state, bcm2711_vulkan_draw_prepare_t prepare, void *payload);
static void bind_sets(struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);
static void push_words(struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);

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

	/* Admit the entire ordered graph without calling any preparation hook. */
	error = walk_records(command, state, NULL, NULL);
	if (error != 0) {
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
		selected = false;

		/* Each event updates one ordinary graphics selection or emits a complete pass/draw preparation point. */
		switch (record->opcode) {
		case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
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
			selected = true;
			break;
		case GPU_OP_CMD_SET_VIEWPORT:
			state->viewport = record;
			break;
		case GPU_OP_CMD_SET_SCISSOR:
			state->scissor = record;
			break;
		case GPU_OP_CMD_BIND_PIPELINE:
			state->pipeline = record->objects[0]->payload;
			break;
		case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
			/* Apply ordinary descriptor prefix disturbance before selecting the new sets. */
			bind_sets(state, record);
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
		case GPU_OP_CMD_DRAW:
			/* Resolve exact current shader inputs and complete logical fetch ranges. */
			error = bcm2711_vulkan_draw_validate(state, record);
			if (error != 0)
				return error;
			selected = true;
			break;
		default:
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

	/* An incomplete pass is never handed to a native queue as a complete control list. */
	if (state->pass != NULL)
		return EINVAL;

	/* Succeeded: the ordered primary record graph has no incomplete render pass or refused draw. */
	return 0;
}

/* Applies descriptor prefix disturbance before installing each exact set selected by one binding command. */
static void
bind_sets(
	struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_pipeline_layout *layout;
	struct bcm2711_vulkan_draw_set *bound;
	uint32_t index;
	uint32_t number;
	uint32_t lower;
	uint32_t higher;
	int error;

	/* Descriptor disturbance follows ascending set order and remembers the disturbing layout even for now-undefined selections. */
	layout = record->objects[0]->payload;
	for (index = 0; index < record->count; index++) {
		number = record->first + index;

		/* Lower sets survive only when their own prefixes remain compatible with this newly selected layout. */
		for (lower = 0; lower < number; lower++) {
			bound = &state->sets[lower];
			if (bound->layout == NULL)
				continue;
			error = bcm2711_vulkan_layout_compatible(bound->layout, layout, lower);
			if (error != 0) {
				bound->set = NULL;
				bound->layout = layout;
			}
		}

		/* Replacing an incompatible selection also makes every higher set undefined under the disturbing layout. */
		bound = &state->sets[number];
		if (bound->layout != NULL) {
			error = bcm2711_vulkan_layout_compatible(bound->layout, layout, number);
			if (error != 0) {
				for (higher = number + 1U; higher < BCM2711_VULKAN_PIPELINE_SETS; higher++) {
					state->sets[higher].set = NULL;
					state->sets[higher].layout = layout;
				}
			}
		}

		/* This exact recorded set supersedes only its selected slot after compatibility side effects have been applied. */
		bound->set = record->objects[index + 1U]->payload;
		bound->layout = layout;
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
