/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Ordered updates stage independently retained bindings before publishing any mutable descriptor state. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-descriptor.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"

#define VULKAN_UPDATE_OPERATIONS 64U
#define VULKAN_UPDATE_SETS (2U * VULKAN_UPDATE_OPERATIONS)

/* One staged destination borrows its stable registry owner and owns a complete replacement binding array. */
struct descriptor_destination {
	struct bcm2711_vulkan_descriptor_set *set;
	struct bcm2711_vulkan_descriptor bindings[BCM2711_VULKAN_LAYOUT_BINDINGS];
};

/* One finite transaction owns every changed binding until complete validation or rollback under the controller mutex. */
struct descriptor_update {
	struct bcm2711_vulkan_session *session;
	struct bcm2711_vulkan_object *device;
	uint32_t count;
	struct descriptor_destination destinations[VULKAN_UPDATE_SETS];
};

static int update_sets(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader);
static int write_binding(struct descriptor_update *update, struct i915_wire_reader *reader);
static int copy_binding(struct descriptor_update *update, struct i915_wire_reader *reader);
static int binding_lookup(struct descriptor_update *update, uint64_t identity, uint32_t number, struct bcm2711_vulkan_descriptor_set **set, uint32_t *index);
static int destination_prepare(struct descriptor_update *update, struct bcm2711_vulkan_descriptor_set *set, struct descriptor_destination **destination);
static struct bcm2711_vulkan_descriptor *binding_current(struct descriptor_update *update, struct bcm2711_vulkan_descriptor_set *set, uint32_t index);
static int binding_replace(struct descriptor_update *update, struct bcm2711_vulkan_descriptor_set *set, uint32_t index, const struct bcm2711_vulkan_descriptor *source);
static int update_release(struct descriptor_update *update, bool publish);

/*
 * Routes complete ordered descriptor writes and copies without exposing partially updated set state.
 */
int
bcm2711_vulkan_descriptor_update_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* The stream owns the opcode echo; this void update has no additional reply parameter. */
	(void)reply;

	/* Mutable descriptor updates have no result parameter, but the client normally requests their opcode echo. */
	*handled = 1;
	if (opcode != GPU_OP_UPDATE_DESCRIPTOR_SETS) {
		*handled = 0;
		return 0;
	}

	/* Only the transport's Boolean request is meaningful for this void operation. */
	if (requested > 1)
		return EINVAL;
	error = update_sets(session, reader);
	if (error != 0)
		return error;

	/* Succeeded: every destination reflects the complete ordered transaction. */
	return 0;
}

/*
 * Clones one exact descriptor's independent owners into an empty draw snapshot or staged replacement.
 */
int
bcm2711_vulkan_descriptor_clone(
	const struct bcm2711_vulkan_descriptor *source,
	struct bcm2711_vulkan_descriptor *destination)
{
	struct bcm2711_vulkan_descriptor prepared;
	int error;
	int retired;

	/* A destination that already owns a resource cannot be overwritten without a separate retirement. */
	if (source == NULL || destination == NULL)
		return EINVAL;
	if (destination->view != NULL || destination->sampler != NULL || destination->buffer != NULL)
		return EBUSY;
	kern_memset(&prepared, 0, sizeof(prepared));

	/* Record each acquired edge only after its retain succeeds, so rollback never consumes a borrowed owner. */
	error = 0;
	if (source->view != NULL)
		error = bcm2711_vulkan_object_retain(source->view);
	if (error == 0) {
		prepared.view = source->view;
		if (source->sampler != NULL)
			error = bcm2711_vulkan_object_retain(source->sampler);
	}

	/* Preserve the sampler owner before acquiring the independent uniform-buffer edge. */
	if (error == 0) {
		prepared.sampler = source->sampler;
		if (source->buffer != NULL)
			error = bcm2711_vulkan_object_retain(source->buffer);
	}

	/* Failed acquisition retires only successfully acquired edges, leaving the destination untouched. */
	if (error != 0) {
		retired = bcm2711_vulkan_descriptor_release(&prepared);
		if (retired != 0)
			return retired;
		return error;
	}

	/* Publish immutable descriptor fields only after the complete resource graph is owned. */
	prepared.buffer = source->buffer;
	prepared.offset = source->offset;
	prepared.bytes = source->bytes;
	prepared.image_layout = source->image_layout;
	*destination = prepared;

	/* Succeeded: later set updates and public identity retirement cannot mutate this snapshot. */
	return 0;
}

/* Stages all writes followed by all copies and publishes only after complete native validation. */
static int
update_sets(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader)
{
	struct descriptor_update *update;
	struct bcm2711_vulkan_object *device;
	uint64_t identity;
	uint64_t array;
	uint32_t count;
	uint32_t index;
	int error;
	int retired;
	bool publish;

	/* Resolve the exact device before allocating finite temporary destination storage. */
	identity = drv_i915_wire_read_u64(reader);
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, identity);
	if (reader->error != 0 || device == NULL)
		return EINVAL;
	update = kern_calloc(1, sizeof(*update));
	if (update == NULL)
		return ENOMEM;
	update->session = session;
	update->device = device;

	/* Decode every write first, retaining only supported complete binding replacements. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	error = 0;
	if (reader->error != 0 || count > VULKAN_UPDATE_OPERATIONS || array != count)
		error = EINVAL;
	for (index = 0; index < count && error == 0; index++)
		error = write_binding(update, reader);

	/* Decode copies only after writes; each copy sees all earlier staged changes in this transaction. */
	if (error == 0) {
		count = drv_i915_wire_read_u32(reader);
		array = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > VULKAN_UPDATE_OPERATIONS || array != count)
			error = EINVAL;
		for (index = 0; index < count && error == 0; index++)
			error = copy_binding(update, reader);
	}

	/* Whole-command success alone permits moving staged references into live sets. */
	publish = false;
	if (error == 0)
		publish = true;
	retired = update_release(update, publish);
	kern_free(update);
	if (retired != 0)
		return retired;
	if (error != 0)
		return error;

	/* Succeeded: no staged reference remains after publication. */
	return 0;
}

/* Resolves one same-device single-element binding in the retained canonical interface. */
static int
binding_lookup(
	struct descriptor_update *update,
	uint64_t identity,
	uint32_t number,
	struct bcm2711_vulkan_descriptor_set **set,
	uint32_t *index)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_set_layout *layout;
	uint32_t binding;

	/* Registry ownership and the controller mutex keep this borrowed set stable through the transaction. */
	object = bcm2711_vulkan_object_find(update->session, I915_VK_OBJ_DESCRIPTOR_SET, identity);
	if (object == NULL)
		return EINVAL;
	*set = object->payload;
	if ((*set)->owner.device != update->device)
		return EINVAL;
	layout = (*set)->layout->payload;

	/* Canonical indices are private storage; wire records address the declared numeric binding. */
	for (binding = 0; binding < layout->count; binding++) {
		if (layout->bindings[binding].number == number) {
			*index = binding;
			return 0;
		}
	}

	/* The finite interface does not supply an undeclared binding. */
	return EINVAL;
}

/* Decodes the exact selected image or uniform payload and stages one independently retained replacement. */
static int
write_binding(
	struct descriptor_update *update,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_descriptor candidate;
	struct bcm2711_vulkan_descriptor_set *set;
	struct bcm2711_vulkan_set_layout *layout;
	struct bcm2711_vulkan_image_view *view;
	struct bcm2711_vulkan_input_owner *sampler;
	struct bcm2711_vulkan_resource *resource;
	uint64_t identity;
	uint64_t chain;
	uint64_t array;
	uint64_t sampler_id;
	uint64_t resource_id;
	uint32_t structure;
	uint32_t number;
	uint32_t element;
	uint32_t count;
	uint32_t type;
	uint32_t index;
	int error;

	/* The current interface deliberately supports exactly one element per binding and no extension chain. */
	kern_memset(&candidate, 0, sizeof(candidate));
	structure = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	number = drv_i915_wire_read_u32(reader);
	element = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || structure != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || chain != 0 || element != 0 || count != 1)
		return ENOTSUP;
	error = binding_lookup(update, identity, number, &set, &index);
	if (error != 0)
		return error;
	layout = set->layout->payload;
	if (layout->bindings[index].type != type)
		return EINVAL;

	/* Image descriptors name a same-device sampled image view and either a mutable or immutable sampler. */
	array = drv_i915_wire_read_u64(reader);
	if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
		if (array != 1)
			return EINVAL;
		sampler_id = drv_i915_wire_read_u64(reader);
		resource_id = drv_i915_wire_read_u64(reader);
		candidate.image_layout = drv_i915_wire_read_u32(reader);
		candidate.view = bcm2711_vulkan_object_find(update->session, I915_VK_OBJ_IMAGE_VIEW, resource_id);
		candidate.sampler = layout->bindings[index].immutable;
		if (candidate.sampler == NULL)
			candidate.sampler = bcm2711_vulkan_object_find(update->session, I915_VK_OBJ_SAMPLER, sampler_id);
		if (reader->error != 0 || candidate.view == NULL || candidate.sampler == NULL)
			return EINVAL;
		view = candidate.view->payload;
		sampler = candidate.sampler->payload;
		resource = view->owner.parent->payload;
		if (view->owner.device != update->device || sampler->device != update->device)
			return EINVAL;
		if ((resource->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0 || resource->memory == NULL)
			return EINVAL;
		if (candidate.image_layout != VK_IMAGE_LAYOUT_GENERAL && candidate.image_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			return ENOTSUP;
	} else {
		if (array != 0)
			return EINVAL;
	}

	/* Uniform descriptors preserve an exact logical byte interval rather than the allocation's padded extent. */
	array = drv_i915_wire_read_u64(reader);
	if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
		if (array != 1)
			return EINVAL;
		resource_id = drv_i915_wire_read_u64(reader);
		candidate.offset = drv_i915_wire_read_u64(reader);
		candidate.bytes = drv_i915_wire_read_u64(reader);
		candidate.buffer = bcm2711_vulkan_object_find(update->session, I915_VK_OBJ_BUFFER, resource_id);
		if (reader->error != 0 || candidate.buffer == NULL)
			return EINVAL;
		resource = candidate.buffer->payload;
		if (resource->device != update->device || resource->memory == NULL || (resource->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) == 0)
			return EINVAL;
		if ((candidate.offset & 3U) != 0 || candidate.offset > resource->bytes)
			return EINVAL;
		if (candidate.bytes == VK_WHOLE_SIZE)
			candidate.bytes = resource->bytes - candidate.offset;
		if (candidate.bytes == 0 || candidate.bytes > 65536U || candidate.bytes > resource->bytes - candidate.offset)
			return EINVAL;
	} else {
		if (array != 0)
			return EINVAL;
	}

	/* Neither supported family has a texel-buffer array; reject absent/truncated or extraneous payload before staging. */
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array != 0)
		return EINVAL;
	error = binding_replace(update, set, index, &candidate);
	if (error != 0)
		return error;

	/* Succeeded: this borrowed candidate has an independently owned staged equivalent. */
	return 0;
}

/* Applies one copy to the staged destination after consulting all earlier writes and copies. */
static int
copy_binding(
	struct descriptor_update *update,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_descriptor_set *source;
	struct bcm2711_vulkan_descriptor_set *destination;
	struct bcm2711_vulkan_set_layout *source_layout;
	struct bcm2711_vulkan_set_layout *destination_layout;
	struct bcm2711_vulkan_descriptor *current;
	struct bcm2711_vulkan_descriptor candidate;
	uint64_t chain;
	uint64_t source_id;
	uint64_t destination_id;
	uint32_t structure;
	uint32_t source_number;
	uint32_t source_element;
	uint32_t destination_number;
	uint32_t destination_element;
	uint32_t count;
	uint32_t source_index;
	uint32_t destination_index;
	int error;

	/* Copy records use the same single-element canonical layout contract as writes. */
	structure = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	source_id = drv_i915_wire_read_u64(reader);
	source_number = drv_i915_wire_read_u32(reader);
	source_element = drv_i915_wire_read_u32(reader);
	destination_id = drv_i915_wire_read_u64(reader);
	destination_number = drv_i915_wire_read_u32(reader);
	destination_element = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || structure != VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET || chain != 0 || source_element != 0 || destination_element != 0 || count != 1)
		return ENOTSUP;
	error = binding_lookup(update, source_id, source_number, &source, &source_index);
	if (error != 0)
		return error;
	error = binding_lookup(update, destination_id, destination_number, &destination, &destination_index);
	if (error != 0)
		return error;
	source_layout = source->layout->payload;
	destination_layout = destination->layout->payload;
	if (source_layout->bindings[source_index].type != destination_layout->bindings[destination_index].type)
		return EINVAL;

	/* A destination immutable sampler overrides the copied sampler while retaining the source's exact view and image layout. */
	current = binding_current(update, source, source_index);
	candidate = *current;
	if (destination_layout->bindings[destination_index].immutable != NULL)
		candidate.sampler = destination_layout->bindings[destination_index].immutable;
	error = binding_replace(update, destination, destination_index, &candidate);
	if (error != 0)
		return error;

	/* Succeeded: the next copy sees this staged replacement. */
	return 0;
}

/* Acquires a complete retained replacement array when a destination is first touched. */
static int
destination_prepare(
	struct descriptor_update *update,
	struct bcm2711_vulkan_descriptor_set *set,
	struct descriptor_destination **destination)
{
	uint32_t entry;
	uint32_t index;
	int error;

	/* Ordinary Vulkan 1.0 bindings cannot be updated while native work uses them; generation wrap must never revive an old recording. */
	if (set->pending != 0)
		return EBUSY;

	/* An update generation must never wrap into a stale ordinary recording's saved value. */
	if (set->generation == UINT64_MAX)
		return EOVERFLOW;

	/* Repeated writes and copies share one staged state for each destination. */
	for (entry = 0; entry < update->count; entry++) {
		if (update->destinations[entry].set == set) {
			*destination = &update->destinations[entry];
			return 0;
		}
	}

	/* The declared finite operation counts bound distinct destinations even when every operation changes a different set. */
	if (update->count == VULKAN_UPDATE_SETS)
		return E2BIG;
	*destination = &update->destinations[update->count];
	(*destination)->set = set;
	update->count++;
	for (index = 0; index < BCM2711_VULKAN_LAYOUT_BINDINGS; index++) {
		error = bcm2711_vulkan_descriptor_clone(&set->bindings[index], &(*destination)->bindings[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: rollback owns every successfully cloned edge, including any partially initialized destination. */
	return 0;
}

/* Selects the latest transaction-local source without introducing another ownership edge. */
static struct bcm2711_vulkan_descriptor *
binding_current(
	struct descriptor_update *update,
	struct bcm2711_vulkan_descriptor_set *set,
	uint32_t index)
{
	uint32_t entry;

	/* Earlier destination changes take precedence over the original live set state. */
	for (entry = 0; entry < update->count; entry++) {
		if (update->destinations[entry].set == set)
			return &update->destinations[entry].bindings[index];
	}

	/* Untouched sets still supply their controller-mutex-protected original descriptor. */
	return &set->bindings[index];
}

/* Retains a candidate before replacing staged state, including a copy whose source and destination are the same slot. */
static int
binding_replace(
	struct descriptor_update *update,
	struct bcm2711_vulkan_descriptor_set *set,
	uint32_t index,
	const struct bcm2711_vulkan_descriptor *source)
{
	struct descriptor_destination *destination;
	struct bcm2711_vulkan_descriptor prepared;
	int error;
	int retired;

	/* Obtain destination state before taking a separate candidate owner. */
	error = destination_prepare(update, set, &destination);
	if (error != 0)
		return error;
	kern_memset(&prepared, 0, sizeof(prepared));
	error = bcm2711_vulkan_descriptor_clone(source, &prepared);
	if (error != 0)
		return error;

	/* Release the previous staged slot only after its exact replacement is independently owned. */
	retired = bcm2711_vulkan_descriptor_release(&destination->bindings[index]);
	destination->bindings[index] = prepared;
	if (retired != 0)
		return retired;

	/* Succeeded: transaction rollback or commit owns the replacement. */
	return 0;
}

/* Moves complete successful arrays into live sets or retires every staged reference after a failed transaction. */
static int
update_release(
	struct descriptor_update *update,
	bool publish)
{
	struct descriptor_destination *destination;
	struct bcm2711_vulkan_descriptor previous;
	uint32_t entry;
	uint32_t index;
	int error;
	int retired;

	/* Continue every retirement even if one native owner reports uncertainty, preserving the first error. */
	error = 0;
	for (entry = 0; entry < update->count; entry++) {
		destination = &update->destinations[entry];

		/* One committed transaction advances each changed set once, invalidating its earlier ordinary recorded binds. */
		if (publish)
			destination->set->generation++;

		/* Every replaced or staged binding retires independently even if another native owner reports uncertainty. */
		for (index = 0; index < BCM2711_VULKAN_LAYOUT_BINDINGS; index++) {
			if (publish) {
				previous = destination->set->bindings[index];
				destination->set->bindings[index] = destination->bindings[index];
				kern_memset(&destination->bindings[index], 0, sizeof(destination->bindings[index]));
				retired = bcm2711_vulkan_descriptor_release(&previous);
			} else {
				retired = bcm2711_vulkan_descriptor_release(&destination->bindings[index]);
			}

			/* Preserve the native error without abandoning other staged or replaced owners. */
			if (retired != 0 && error == 0)
				error = retired;
		}
	}

	/* Failed native retirement remains visible even after logical descriptor ownership transfers. */
	if (error != 0)
		return error;

	/* Succeeded: this transaction owns no descriptor edge. */
	return 0;
}
