/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* CPU draw snapshots retain exact inputs and freeze ordinary recorded sets before any queue can own native DMA. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-prepared.h"

/* Finite CPU snapshots are bounded independently of the primary's one-MiB recording storage and future GPU backing. */
#define VULKAN_PREPARED_BYTES (8U * 1024U * 1024U)

static int prepare_event(void *payload, const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *record);
static int build_prepared(struct bcm2711_vulkan_prepared *prepared);
static int snapshot_descriptors(struct bcm2711_vulkan_prepared_event *event, const struct bcm2711_vulkan_draw_state *state);
static int release_event(struct bcm2711_vulkan_prepared_event *event);
static int prepare_sets(struct bcm2711_vulkan_prepared *prepared);
static int charge_pending(struct bcm2711_vulkan_prepared *prepared);

/*
 * Creates complete independently retained CPU draw snapshots and atomically freezes their ordinary primary and descriptor graph.
 */
int
bcm2711_vulkan_prepared_create(
	struct bcm2711_vulkan_object *command_object,
	struct bcm2711_vulkan_prepared **prepared)
{
	struct bcm2711_vulkan_prepared *created;
	int error;
	int released;

	/* Only a current exact primary can acquire a new pending preparation owner. */
	*prepared = NULL;
	if (command_object == NULL ||
	    command_object->kind != I915_VK_OBJ_COMMAND_BUFFER ||
	    !command_object->published ||
	    command_object->session->closing)
		return EINVAL;

	/* One zeroed root owns every partial snapshot until complete construction transfers it to the caller. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	error = bcm2711_vulkan_object_retain(command_object);
	if (error != 0) {
		kern_free(created);
		return error;
	}

	/* The independently retained primary keeps all immutable record inputs alive even if their public identities later retire. */
	created->command = command_object;
	error = build_prepared(created);
	if (error != 0) {
		released = bcm2711_vulkan_prepared_release(created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* Publication follows complete snapshots and pending charges; no actual native job has launched. */
	*prepared = created;

	/* Succeeded: the caller owns a frozen primary and exact consumed descriptors for later native lowering. */
	return 0;
}

/*
 * Releases a complete prepared graph only when no native DMA can still read any of its independent inputs.
 */
int
bcm2711_vulkan_prepared_release(
	struct bcm2711_vulkan_prepared *prepared,
	bool retired)
{
	struct bcm2711_vulkan_prepared_event *event;
	struct bcm2711_vulkan_prepared_event *next;
	struct bcm2711_vulkan_prepared_set *use;
	struct bcm2711_vulkan_prepared_set *next_use;
	struct bcm2711_vulkan_command_buffer *command;
	int error;
	int released;

	/* Uncertain native execution retains the entire graph and all mutation prohibitions for completion or confirmed reset. */
	if (prepared == NULL)
		return 0;
	if (!retired)
		return EBUSY;

	/* Restore every distinct ordinary set charge while the primary still owns each borrowed set. */
	command = prepared->command->payload;
	use = prepared->sets;
	while (use != NULL) {
		next_use = use->next;
		if (prepared->pending) {
			if (use->set->pending == 0)
				__builtin_trap();
			use->set->pending--;
		}

		/* Set bookkeeping owns no additional graph edge; the retained primary supplies its lifetime. */
		kern_free(use);
		use = next_use;
	}

	/* Pending means the primary's nodes cannot be reset through a public pool or buffer identity. */
	if (prepared->pending) {
		if (command->pending == 0)
			__builtin_trap();
		command->pending--;
		prepared->pending = false;
	}

	/* Retire every independent descriptor snapshot even when an earlier native backing requires quarantine recovery. */
	error = 0;
	event = prepared->first;
	while (event != NULL) {
		next = event->next;
		released = release_event(event);
		if (released != 0 && error == 0)
			error = released;
		event = next;
	}

	/* The primary graph retires last, after all borrowed snapshot pointers have ceased to be used. */
	released = bcm2711_vulkan_object_release(prepared->command);
	if (released != 0 && error == 0)
		error = released;
	kern_free(prepared);
	if (error != 0)
		return error;

	/* Succeeded: pending charges, descriptor copies and the independent primary graph owner retired exactly once. */
	return 0;
}

/* Completes all CPU snapshots and distinct-set bookkeeping before charging any pending mutation prohibition. */
static int
build_prepared(
	struct bcm2711_vulkan_prepared *prepared)
{
	struct bcm2711_vulkan_command_buffer *command;
	int error;

	/* Validate all recorded draws before acquiring exact consumed descriptor copies at each preparation point. */
	command = prepared->command->payload;
	error = bcm2711_vulkan_draw_walk(command, prepare_event, prepared);
	if (error != 0)
		return error;

	/* Every recorded ordinary set receives one distinct pending charge even when its bindings are not consumed. */
	error = prepare_sets(prepared);
	if (error != 0)
		return error;

	/* Complete counter validation prevents publication of a partial mutation prohibition. */
	error = charge_pending(prepared);
	if (error != 0)
		return error;

	/* Succeeded: all snapshots and pending charges belong to one complete primary owner. */
	return 0;
}

/* Copies one complete pass/draw preparation point and acquires descriptor snapshots before appending it to the owned prefix. */
static int
prepare_event(
	void *payload,
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_prepared *prepared;
	struct bcm2711_vulkan_prepared_event *event;
	int error;
	int released;

	/* Each finite native preparation point is charged before allocation; failure leaves the existing prefix untouched. */
	prepared = payload;
	if (sizeof(*event) > VULKAN_PREPARED_BYTES - prepared->bytes)
		return ENOSPC;

	/* Allocate one zeroed event whose partial descriptor clones can be retired individually. */
	event = kern_calloc(1, sizeof(*event));
	if (event == NULL)
		return ENOMEM;

	/* The primary reference keeps the exact immutable pass record and compiled pipeline graph independent of public identities. */
	event->opcode = record->opcode;
	event->pass = state->pass;
	if (record->opcode == GPU_OP_CMD_DRAW) {
		event->pipeline = state->pipeline;
		kern_memcpy(event->vertices, state->vertices, sizeof(event->vertices));
		kern_memcpy(event->offsets, state->offsets, sizeof(event->offsets));
		kern_memcpy(event->push, state->push, sizeof(event->push));
		kern_memcpy(event->viewport, state->viewport->words, sizeof(event->viewport));
		event->scissor = state->scissor->area;
		kern_memcpy(event->draw, record->words, sizeof(event->draw));
		error = snapshot_descriptors(event, state);
		if (error != 0) {
			released = release_event(event);
			if (released != 0)
				return released;
			return error;
		}
	}

	/* Only a fully constructed event enters the owned prefix; pending CPU bytes never count an unowned node. */
	if (prepared->last == NULL) {
		prepared->first = event;
	} else {
		prepared->last->next = event;
	}

	/* The tail and CPU-storage charge become visible together only after complete ownership publication. */
	prepared->last = event;
	prepared->bytes += sizeof(*event);

	/* Succeeded: the caller owns one complete immutable CPU preparation point without any native DMA launch. */
	return 0;
}

/* Clones each statically consumed descriptor once across all native stage uniform streams. */
static int
snapshot_descriptors(
	struct bcm2711_vulkan_prepared_event *event,
	const struct bcm2711_vulkan_draw_state *state)
{
	const struct bcm2711_shader_binary *program;
	const struct bcm2711_shader_uniform *uniform;
	const struct bcm2711_vulkan_descriptor *descriptor;
	VkDescriptorType type;
	uint32_t stage;
	uint32_t index;
	uint32_t slot;
	int error;

	/* Constants, dynamic viewport and pushed scalars already have exact copied values and require no descriptor owner. */
	for (stage = 0; stage < 3; stage++) {
		program = state->pipeline->programs[stage];
		for (index = 0; index < program->uniform_count; index++) {
			uniform = &program->uniforms[index];
			if (uniform->kind != BCM2711_SHADER_BLOCK &&
			    uniform->kind != BCM2711_SHADER_TEXTURE &&
			    uniform->kind != BCM2711_SHADER_SAMPLER)
				continue;

			/* Resolve the exact current canonical slot through already validated descriptor prefix compatibility. */
			type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			if (uniform->kind == BCM2711_SHADER_BLOCK)
				type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			error = bcm2711_vulkan_draw_descriptor(state, uniform, type, &descriptor);
			if (error != 0)
				return error;
			slot = (uint32_t)(descriptor - state->sets[uniform->set].set->bindings);
			if (slot >= BCM2711_VULKAN_LAYOUT_BINDINGS)
				return EINVAL;
			if ((event->used[uniform->set] & (1U << slot)) != 0)
				continue;

			/* Each successful clone owns its exact view/sampler or buffer interval independently of mutable descriptor storage. */
			error = bcm2711_vulkan_descriptor_clone(descriptor, &event->descriptors[uniform->set][slot]);
			if (error != 0)
				return error;
			event->used[uniform->set] |= 1U << slot;
		}
	}

	/* Succeeded: every shader-consumed descriptor appears once in the independent snapshot owner graph. */
	return 0;
}

/* Retires all successful descriptor clones from one complete or partially constructed preparation event. */
static int
release_event(
	struct bcm2711_vulkan_prepared_event *event)
{
	uint32_t set;
	uint32_t slot;
	int error;
	int released;

	/* A used bit exists only after clone ownership succeeded; all such edges retire even after the first native error. */
	error = 0;
	for (set = 0; set < BCM2711_VULKAN_PIPELINE_SETS; set++) {
		for (slot = 0; slot < BCM2711_VULKAN_LAYOUT_BINDINGS; slot++) {
			if ((event->used[set] & (1U << slot)) == 0)
				continue;
			released = bcm2711_vulkan_descriptor_release(&event->descriptors[set][slot]);
			if (released != 0 && error == 0)
				error = released;
		}
	}

	/* Immutable pipeline, pass and vertex pointers are borrowed from the primary root and own no separate edge here. */
	kern_free(event);
	if (error != 0)
		return error;

	/* Succeeded: this preparation event no longer owns any independent descriptor resource. */
	return 0;
}

/* Collects each distinct recorded ordinary set before applying pending mutation prohibitions. */
static int
prepare_sets(
	struct bcm2711_vulkan_prepared *prepared)
{
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_command_node *node;
	struct bcm2711_vulkan_record *record;
	struct bcm2711_vulkan_descriptor_set *set;
	struct bcm2711_vulkan_prepared_set *use;
	uint32_t index;
	bool found;

	/* Ordinary bindings cannot be updated while pending even when the current shader does not consume every binding. */
	command = prepared->command->payload;
	for (node = command->first; node != NULL; node = node->next) {
		record = (struct bcm2711_vulkan_record *)node;
		if (record->opcode != GPU_OP_CMD_BIND_DESCRIPTOR_SETS)
			continue;
		for (index = 0; index < record->count; index++) {
			set = record->objects[index + 1U]->payload;
			found = false;
			for (use = prepared->sets; use != NULL; use = use->next) {
				if (use->set == set) {
					found = true;
					break;
				}
			}

			/* Repeated bindings acquire one pending charge per prepared primary rather than one per draw or record. */
			if (found)
				continue;
			if (sizeof(*use) > VULKAN_PREPARED_BYTES - prepared->bytes)
				return ENOSPC;
			use = kern_calloc(1, sizeof(*use));
			if (use == NULL)
				return ENOMEM;

			/* The retained primary supplies this borrowed set's lifetime; no pool/primary ownership cycle is introduced. */
			use->set = set;
			use->next = prepared->sets;
			prepared->sets = use;
			prepared->bytes += sizeof(*use);
		}
	}

	/* Succeeded: complete distinct-set bookkeeping can now be validated and charged atomically. */
	return 0;
}

/* Validates every counter before making the primary and all recorded ordinary sets immutable for native preparation ownership. */
static int
charge_pending(
	struct bcm2711_vulkan_prepared *prepared)
{
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_prepared_set *use;

	/* No counter can wrap into the zero value that permits reset, update or native graph retirement. */
	command = prepared->command->payload;
	if (command->pending == 0xffffffffU)
		return EOVERFLOW;
	for (use = prepared->sets; use != NULL; use = use->next) {
		if (use->set->pending == 0xffffffffU)
			return EOVERFLOW;
	}

	/* Every distinct set remains immutable throughout queue wait, execution and any uncertain native retirement. */
	for (use = prepared->sets; use != NULL; use = use->next)
		use->set->pending++;

	/* The retained primary's nodes cannot be reset while this prepared native owner exists. */
	command->pending++;
	prepared->pending = true;

	/* Succeeded: all pending mutations are prohibited without a partial counter transaction. */
	return 0;
}
