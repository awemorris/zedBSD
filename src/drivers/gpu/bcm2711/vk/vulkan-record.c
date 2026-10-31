/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Reply-free graphics calls append independently owned immutable events and retain their first failure for End. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vk/vulkan-record.h"
#include "drivers/gpu/bcm2711/vk/vulkan-descriptor.h"

/* One chunk of a recording's events, in recording order, the first used of them. */
struct bcm2711_vulkan_record_chunk {
	struct bcm2711_vulkan_record_chunk *next;
	uint32_t used;
	struct bcm2711_vulkan_record records[BCM2711_VULKAN_RECORD_CHUNK];
};

static int append_record(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *decoded);
static int release_record(struct bcm2711_vulkan_command_node *node);
static struct bcm2711_vulkan_record *record_take(struct bcm2711_vulkan_command_buffer *command);

/*
 * Records one complete reply-free graphics event while preserving exact ordering and independent input ownership.
 */
int
bcm2711_vulkan_record_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	struct bcm2711_vulkan_record decoded;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	uint64_t identity;
	int error;

	/* Ordinary lifecycle and synchronization operations have separate typed native routes. */
	(void)reply;
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
	case GPU_OP_CMD_PUSH_CONSTANTS:
	case GPU_OP_CMD_SET_VIEWPORT:
	case GPU_OP_CMD_SET_SCISSOR:
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
	case GPU_OP_CMD_END_RENDER_PASS:
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Real vkCmd records request no opcode echo or parameter reply, even when batched before result-bearing End. */
	if (requested != 0)
		return EINVAL;

	/* Every reply-free event addresses one exact primary recording. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Resolve the session-local typed command before accepting any event fields. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	command = object->payload;
	if (command->pending != 0 || command->state != BCM2711_VULKAN_COMMAND_RECORDING)
		return EBUSY;

	/* Always consume a complete bounded record before acknowledging any ordinary semantic/heap refusal. */
	kern_memset(&decoded, 0, sizeof(decoded));
	decoded.opcode = opcode;
	error = bcm2711_vulkan_record_decode(session, reader, &decoded);
	if (error != 0)
		return error;

	/* A prior void failure owns End's outcome; complete later records acquire no new inputs. */
	if (command->recording_error != 0)
		return 0;

	/* Validate exact immutable interfaces before acquiring an event's independent references. */
	error = bcm2711_vulkan_record_validate(command, &decoded);
	if (error == 0)
		error = append_record(command, &decoded);
	if (error != 0) {
		/* The first void failure invalidates final End; later records are consumed without acquiring additional owners. */
		command->recording_error = error;
		return 0;
	}

	/* Successful begin/end publication alone changes the primary pass nesting protocol. */
	if (opcode == GPU_OP_CMD_BEGIN_RENDER_PASS)
		command->render_open = true;
	else if (opcode == GPU_OP_CMD_END_RENDER_PASS)
		command->render_open = false;

	/* Succeeded: this ordered event owns copied fields and exact retained typed inputs until permitted reset or final release. */
	return 0;
}

/*
 * Invalidates ordinary recordings whose retained descriptor sets were updated after binding.
 */
int
bcm2711_vulkan_record_current(
	struct bcm2711_vulkan_command_buffer *command)
{
	struct bcm2711_vulkan_command_node *node;
	struct bcm2711_vulkan_record *record;
	struct bcm2711_vulkan_descriptor_set *set;
	uint32_t index;

	/* Vulkan 1.0 layouts admit no update-after-bind exception; every bound set's exact generation must still match. */
	for (node = command->first; node != NULL; node = node->next) {
		record = (struct bcm2711_vulkan_record *)node;
		if (record->opcode != GPU_OP_CMD_BIND_DESCRIPTOR_SETS)
			continue;

		/* Repeated binds remain independently checked, including sets disturbed by later layout changes. */
		for (index = 0; index < record->count; index++) {
			set = record->objects[index + 1U]->payload;
			if (!record->objects[index + 1U]->published || record->generations[index] != set->generation) {
				/* End retains its first void failure; executable submission observes an invalid primary state immediately. */
				if (command->recording_error == 0)
					command->recording_error = EINVAL;
				if (command->state != BCM2711_VULKAN_COMMAND_RECORDING)
					command->state = BCM2711_VULKAN_COMMAND_INVALID;
				return EINVAL;
			}
		}
	}

	/* Succeeded: ordinary descriptor bindings still match their exact recorded update generations. */
	return 0;
}

/* Acquires complete typed ownership before appending a node, unwinding only successfully acquired edges on ordinary failure. */
static int
append_record(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *decoded)
{
	struct bcm2711_vulkan_record *record;
	struct bcm2711_vulkan_descriptor_set *set;
	uint32_t index;
	int error;
	int retired;

	/* Transport batch flushing does not reset retained native recording capacity. */
	if (command->recorded_bytes > BCM2711_VULKAN_RECORD_BYTES - sizeof(*record))
		return ENOMEM;

	/* Takes one event from the recording's chunks without changing the primary list on ordinary OOM. */
	record = record_take(command);
	if (record == NULL)
		return ENOMEM;

	/* Scalar copies cannot turn borrowed lookup edges into falsely acquired owned references. */
	kern_memcpy(record, decoded, sizeof(*record));
	kern_memset(record->objects, 0, sizeof(record->objects));
	record->node.release = release_record;

	/* Null slots own nothing; each successful retain is published individually before another can fail. */
	for (index = 0; index < BCM2711_VULKAN_VERTEX_BINDINGS + 1U; index++) {
		if (decoded->objects[index] == NULL)
			continue;
		error = bcm2711_vulkan_object_retain(decoded->objects[index]);
		if (error != 0) {
			retired = release_record(&record->node);
			if (retired != 0)
				return retired;
			return error;
		}

		/* Only acquired references belong to the new node's eventual destructor. */
		record->objects[index] = decoded->objects[index];
	}

	/* Ordinary descriptor updates invalidate earlier recorded binds even though independent resource snapshots are acquired at submission. */
	if (record->opcode == GPU_OP_CMD_BIND_DESCRIPTOR_SETS) {
		for (index = 0; index < record->count; index++) {
			set = record->objects[index + 1U]->payload;
			record->generations[index] = set->generation;
		}
	}

	/* The primary list gains one fully owned event at its tail; retained byte charges persist until reset or final release. */
	if (command->last == NULL)
		command->first = &record->node;
	else
		command->last->next = &record->node;
	command->last = &record->node;
	command->recorded_bytes += sizeof(*record);

	/* Succeeded: the complete independently owned event is visible in exact recorded order. */
	return 0;
}

/* Releases every acquired input edge once before returning the event's immutable host storage. */
static int
release_record(
	struct bcm2711_vulkan_command_node *node)
{
	struct bcm2711_vulkan_record *record;
	uint32_t index;
	int error;
	int retired;

	/* The base node is the first member, so one event owns the entire allocation and its exact acquired object slots. */
	record = (struct bcm2711_vulkan_record *)node;
	error = 0;
	for (index = 0; index < BCM2711_VULKAN_VERTEX_BINDINGS + 1U; index++) {
		retired = bcm2711_vulkan_object_release(record->objects[index]);
		if (retired != 0 && error == 0)
			error = retired;
	}

	/* The event's storage belongs to its recording's chunk, freed when the recording is cleared. */
	if (error != 0)
		return error;

	/* Succeeded: the event owns no typed input or recording storage. */
	return 0;
}

/*
 * Frees a cleared recording's event chunks.
 */
void
bcm2711_vulkan_record_chunks_free(
	struct bcm2711_vulkan_command_buffer *command)
{
	struct bcm2711_vulkan_record_chunk *chunk;
	struct bcm2711_vulkan_record_chunk *next;

	/* Every event of every chunk has released its typed edges. */
	for (chunk = command->record_chunks; chunk != NULL; chunk = next) {
		next = chunk->next;
		kern_free(chunk);
	}
	command->record_chunks = NULL;
}

/* Takes the next zeroed event from the recording's newest chunk, adding a chunk when it is full. */
static struct bcm2711_vulkan_record *
record_take(
	struct bcm2711_vulkan_command_buffer *command)
{
	struct bcm2711_vulkan_record_chunk *chunk;

	/* A new chunk goes to the head of the recording's list; its events are zeroed. */
	chunk = command->record_chunks;
	if (chunk == NULL || chunk->used == BCM2711_VULKAN_RECORD_CHUNK) {
		chunk = kern_calloc(1, sizeof(*chunk));
		if (chunk == NULL)
			return NULL;
		chunk->next = command->record_chunks;
		command->record_chunks = chunk;
	}

	/* Reports the next unused event of the newest chunk. */
	chunk->used++;
	return &chunk->records[chunk->used - 1U];
}
