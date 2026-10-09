/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native FIFO pass preparation owns all allocations before list publication, with no MMIO launch or target mutation. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-native-pass.h"
#include "drivers/gpu/bcm2711/native-colour.h"

/* All padded native inputs across the enclosing submission share one finite staging budget. */
#define NATIVE_PASS_BYTES (256ULL * 1024U * 1024U)

static int prepare_clear(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_record *record);
static int prepare_target(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_prepared_event *begin);
static int collect_draws(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_prepared_event *begin, uint64_t *remaining, const struct bcm2711_vulkan_prepared_event **next);
static int allocate_storage(struct bcm2711_vulkan_native_pass *pass, uint64_t bytes, uint64_t *remaining, struct bcm2711_native_storage **storage);
static int prepare_lists(struct bcm2711_vulkan_native_pass *pass, uint64_t *remaining);
static void prepare_image(struct bcm2711_native_storage *storage, struct bcm2711_native_pass_image *image);

/*
 * Prepares one whole native graphics pass only after preceding FIFO writes retire.
 *
 * Every consumed input, command byte, binning/overflow allocation and output
 * view is independently owned.  The parent submission retains its pending
 * prepared primary.  An error unwinds the entire unpublished prefix without
 * changing the target or caller budget.  Partial clear is deferred to native
 * execution; loading each selected tile preserves samples outside its exact
 * render area, including neighbouring tiles in a grouped supertile.
 */
int
bcm2711_vulkan_native_pass_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_prepared_event *begin,
	uint64_t *available,
	struct bcm2711_vulkan_native_pass **pass,
	const struct bcm2711_vulkan_prepared_event **next)
{
	struct bcm2711_vulkan_native_pass *created;
	const struct bcm2711_vulkan_prepared_event *following;
	uint64_t remaining;
	int error;
	int released;

	/* No partially acquired pass or event cursor may escape on refusal. */
	if (pass == NULL || next == NULL)
		return EINVAL;
	*pass = NULL;
	*next = NULL;

	/* Immutable prepared pass events and a ready native owner supply all lifetime prerequisites. */
	if (space == NULL ||
	    space->native == NULL ||
	    begin == NULL ||
	    begin->opcode != GPU_OP_CMD_BEGIN_RENDER_PASS ||
	    begin->pass == NULL)
		return EINVAL;

	/* Only the existing submission-wide padded staging budget can authorize native allocations. */
	if (available == NULL || *available > NATIVE_PASS_BYTES)
		return EINVAL;
	remaining = *available;
	following = NULL;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->job.kind = BCM2711_V3D_JOB_CL;

	/* Complete output ownership precedes every draw or command-storage allocation. */
	error = prepare_target(created, begin);

	/* Draw inputs are copied only after the independent framebuffer hold has succeeded. */
	if (error == 0)
		error = collect_draws(created, begin, &remaining, &following);

	/* Native command storage follows the complete independent draw prefix. */
	if (error == 0)
		error = prepare_lists(created, &remaining);

	/* Preparation never launched DMA; each owner retires once even when a late translation teardown fails. */
	if (error != 0) {
		released = bcm2711_vulkan_native_pass_release(&created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* The caller receives one complete numerical native job and the next borrowed immutable preparation cursor. */
	*available = remaining;
	*pass = created;
	*next = following;

	/* Succeeded: every possible device reference belongs to this root or its linked independent draw roots. */
	return 0;
}

/*
 * Prepares an independently owned full-image native clear without CPU framebuffer mutation.
 *
 * A zero-draw bin list and native tile clear/store implement every selected
 * single-subresource range.  The prepared primary retains the logical image;
 * this root independently retains its exact backing view through uncertain
 * native completion, using the ordinary whole-pass retirement mechanism.
 */
int
bcm2711_vulkan_native_clear_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t *available,
	struct bcm2711_vulkan_native_pass **pass)
{
	struct bcm2711_vulkan_native_pass *created;
	uint64_t remaining;
	int error;
	int released;

	/* Every refusal leaves the caller's native owner and budget unchanged. */
	if (pass == NULL)
		return EINVAL;
	*pass = NULL;

	/* Only an exact prepared clear and native address-space owner supply the complete input graph. */
	if (space == NULL ||
	    space->native == NULL ||
	    event == NULL ||
	    event->record == NULL ||
	    event->opcode != GPU_OP_CMD_CLEAR_COLOR_IMAGE)
		return EINVAL;

	/* Native storage cannot exceed the enclosing submission budget. */
	if (available == NULL || *available > NATIVE_PASS_BYTES)
		return EINVAL;

	/* One CPU root owns all independently retained output and list allocations before publication. */
	remaining = *available;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->job.kind = BCM2711_V3D_JOB_CL;
	error = prepare_clear(created, event->record);
	if (error == 0)
		error = prepare_lists(created, &remaining);

	/* No native work launched, so every complete unpublished prefix can retire without changing the output. */
	if (error != 0) {
		released = bcm2711_vulkan_native_pass_release(&created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* Publication consumes only successfully prepared padded storage, exactly as an ordinary graphics pass does. */
	*available = remaining;
	*pass = created;

	/* Succeeded: native tile clear/store owns every eventual DMA reference and reads no user graphics state. */
	return 0;
}

/*
 * Builds the complete native lists for an unpublished independently owned graphics or internal transfer pass.
 */
int
bcm2711_vulkan_native_pass_build_lists(
	struct bcm2711_vulkan_native_pass *pass,
	uint64_t *available)
{
	uint64_t remaining;
	int error;

	/* Only a fresh native root with an exact independent output may acquire command and overflow storage. */
	if (pass == NULL ||
	    pass->space == NULL ||
	    pass->output == NULL ||
	    pass->count != 0 ||
	    pass->executed)
		return EINVAL;
	if (available == NULL || *available > NATIVE_PASS_BYTES)
		return EINVAL;

	/* A partial allocation failure keeps every mapping on the root while leaving the enclosing caller budget unchanged. */
	remaining = *available;
	error = prepare_lists(pass, &remaining);
	if (error != 0)
		return error;
	*available = remaining;

	/* Succeeded: all lists are cleaned and every native address belongs to the same complete unpublished pass. */
	return 0;
}

/*
 * Retires a whole native pass only after no-launch, completion or checked global reset proves DMA stopped.
 */
int
bcm2711_vulkan_native_pass_release(
	struct bcm2711_vulkan_native_pass **pass,
	bool retired)
{
	struct bcm2711_vulkan_native_pass *owned;
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_vulkan_native_draw *next;
	uint32_t index;
	int first;
	int error;

	/* A missing pointer cannot express explicit ownership consumption. */
	if (pass == NULL)
		return EINVAL;
	owned = *pass;
	if (owned == NULL)
		return 0;

	/* Callback completion never proves the device has stopped reading any input or writing the framebuffer. */
	if (!retired)
		return EBUSY;

	/* The whole root is consumed once; failed mapping teardown is held independently by the native address space. */
	*pass = NULL;
	first = 0;
	draw = owned->first;
	while (draw != NULL) {
		/* Preserve the linked successor before consuming each complete independent draw. */
		next = draw->next;
		error = bcm2711_vulkan_native_draw_release(&draw, true);
		if (first == 0 && error != 0)
			first = error;
		draw = next;
	}

	/* Every command/tile/overflow owner retires even if an earlier draw teardown failed. */
	for (index = 0; index < owned->count; index++) {
		error = bcm2711_native_storage_release(owned->space, &owned->storage[index], true);
		if (first == 0 && error != 0)
			first = error;
	}

	/* The independent framebuffer hold remains through all referenced native-input retirement. */
	if (owned->output != NULL) {
		error = bcm2711_v3d_memory_release(owned->space, owned->output);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Only the consumed CPU root disappears; uncertain failed-unmap storage remains native-space owned. */
	kern_free(owned);
	if (first != 0)
		return first;

	/* Succeeded: all independent references of the complete pass have retired. */
	return 0;
}

/* Retains the actual coherent output view and copies immutable numerical area/lifecycle state without modifying any target sample. */
static int
prepare_target(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_prepared_event *begin)
{
	const struct bcm2711_vulkan_record *record;
	const struct bcm2711_vulkan_pass *description;
	const struct bcm2711_vulkan_framebuffer *framebuffer;
	const struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	uint32_t end_x;
	uint32_t end_y;
	void *cpu;
	int error;

	/* The retained primary supplies exact typed pass/framebuffer ownership rather than a public-handle relookup. */
	record = begin->pass;
	if (record->opcode != GPU_OP_CMD_BEGIN_RENDER_PASS ||
	    record->objects[0] == NULL ||
	    record->objects[1] == NULL)
		return EINVAL;

	/* The implemented single-colour target requires complete retained immutable descriptions. */
	if (record->objects[0]->kind != I915_VK_OBJ_RENDER_PASS || record->objects[1]->kind != I915_VK_OBJ_FRAMEBUFFER)
		return EINVAL;
	description = record->objects[0]->payload;
	framebuffer = record->objects[1]->payload;
	if (description == NULL ||
	    framebuffer == NULL ||
	    framebuffer->view == NULL ||
	    framebuffer->view->kind != I915_VK_OBJ_IMAGE_VIEW)
		return EINVAL;
	image_view = framebuffer->view->payload;
	if (image_view == NULL ||
	    image_view->owner.parent == NULL ||
	    image_view->owner.parent->kind != I915_VK_OBJ_IMAGE)
		return EINVAL;
	image = image_view->owner.parent->payload;
	if (image == NULL ||
	    (image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ||
	    image->bytes > 0xffffffffU)
		return EINVAL;

	/* Only the independently implemented RGBA/BGRA single-subresource raster targets may enter the native list builder. */
	if (image->format != VK_FORMAT_R8G8B8A8_UNORM && image->format != VK_FORMAT_B8G8R8A8_UNORM)
		return ENOTSUP;

	/* Exact nonempty render bounds are checked before narrowing any tile endpoint. */
	if (framebuffer->width == 0 ||
	    framebuffer->width > image->width ||
	    framebuffer->height == 0 ||
	    framebuffer->height > image->height)
		return EINVAL;
	if (record->area.offset.x < 0 ||
	    record->area.offset.y < 0 ||
	    record->area.extent.width == 0 ||
	    record->area.extent.height == 0)
		return EINVAL;
	if ((uint64_t)record->area.offset.x + record->area.extent.width > framebuffer->width || (uint64_t)record->area.offset.y + record->area.extent.height > framebuffer->height)
		return EINVAL;

	/* Attachment lifecycle must exactly match the currently implemented single-colour pass choices. */
	if (description->colour.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD &&
	    description->colour.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR &&
	    description->colour.loadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		return ENOTSUP;
	if (description->colour.storeOp != VK_ATTACHMENT_STORE_OP_STORE && description->colour.storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOTSUP;
	if (description->colour.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && record->count == 0)
		return EINVAL;

	/* Actual coherent logical image storage provides a native interval and CPU clear alias protected by its independent view hold. */
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* The checked reference is acquired before any copied CPU alias or numerical address enters the pass. */
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	bcm2711_v3d_memory_retain(view);
	pass->output = view;
	pass->cpu = cpu;

	/* The retained primary keeps the logical attachment alive while native execution commits its implicit layout lifecycle. */
	pass->target = image;
	pass->initial_layout = description->colour.initialLayout;
	pass->final_layout = description->colour.finalLayout;

	/* Every selected tile loads existing samples, preserving partial render areas and extra grouped supertile tiles. */
	pass->state.width = framebuffer->width;
	pass->state.height = framebuffer->height;
	pass->state.pitch = image->pitch;
	pass->state.output = address;
	pass->state.output_bytes = (uint32_t)image->bytes;
	pass->state.load = 1;
	if (description->colour.storeOp == VK_ATTACHMENT_STORE_OP_STORE)
		pass->state.store = 1;
	end_x = (uint32_t)record->area.offset.x + record->area.extent.width - 1U;
	end_y = (uint32_t)record->area.offset.y + record->area.extent.height - 1U;
	pass->state.first_x = (uint32_t)record->area.offset.x / 64U;
	pass->state.last_x = end_x / 64U;
	pass->state.first_y = (uint32_t)record->area.offset.y / 64U;
	pass->state.last_y = end_y / 64U;

	/* Clear state is copied without touching the actual target before whole-pass preparation succeeds. */
	pass->area = record->area;
	pass->format = image->format;
	pass->load = description->colour.loadOp;
	kern_memcpy(pass->clear, record->words, sizeof(pass->clear));

	/* Succeeded: all later target references borrow this independently retained view, never a public framebuffer identity. */
	return 0;
}

/* Builds complete linked native draw owners and locates the exact end of this immutable prepared pass. */
static int
collect_draws(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_prepared_event *begin,
	uint64_t *remaining,
	const struct bcm2711_vulkan_prepared_event **next)
{
	const struct bcm2711_vulkan_prepared_event *event;
	struct bcm2711_vulkan_native_draw *draw;
	int error;

	/* Only ordered draw events belonging to the same immutable BEGIN record can enter this native pass. */
	event = begin->next;
	while (event != NULL && event->opcode == GPU_OP_CMD_DRAW) {
		/* A foreign pass pointer cannot extend one native framebuffer's lifetime or command list. */
		if (event->pass != begin->pass)
			return EINVAL;

		/* Zero vertices or instances perform no GPU reads and therefore allocate no shader/fetch/TMU owners. */
		if (event->draw[0] == 0 || event->draw[1] == 0) {
			event = event->next;
			continue;
		}

		/* Complete native draw preparation occurs at FIFO execution after preceding GPU output becomes CPU-visible. */
		error = bcm2711_vulkan_native_draw_create(pass->space, event, remaining, &draw);
		if (error != 0)
			return error;

		/* Attach immediately so every later error retires the whole draw prefix. */
		if (pass->last == NULL)
			pass->first = draw;
		else
			pass->last->next = draw;
		pass->last = draw;
		pass->draws++;
		pass->bytes += draw->bytes;
		event = event->next;
	}

	/* The complete immutable pass must end explicitly before a following pass or transfer may execute. */
	if (event == NULL ||
	    event->opcode != GPU_OP_CMD_END_RENDER_PASS ||
	    event->pass != begin->pass)
		return EINVAL;
	*next = event->next;

	/* Succeeded: the complete native draw chain owns no borrowed command or descriptor record. */
	return 0;
}

/* Acquires one padded native interval and immediately attaches it to the pass before any subsequent failure. */
static int
allocate_storage(
	struct bcm2711_vulkan_native_pass *pass,
	uint64_t bytes,
	uint64_t *remaining,
	struct bcm2711_native_storage **storage)
{
	uint64_t padded;
	int error;

	/* A refused interval leaves the existing owned prefix unchanged. */
	*storage = NULL;
	if (bytes == 0 || bytes > NATIVE_PASS_BYTES)
		return EINVAL;
	padded = (bytes + 4095U) & ~4095ULL;

	/* Every physical page, including command padding and overflow reserves, consumes the shared job budget. */
	if (pass->count == BCM2711_VULKAN_PASS_STORAGE || padded > *remaining)
		return ENOMEM;
	error = bcm2711_native_storage_create(pass->space, bytes, storage);
	if (error != 0)
		return error;
	pass->storage[pass->count] = *storage;
	pass->count++;
	pass->bytes += padded;
	*remaining -= padded;

	/* Succeeded: every complete physical/mapped owner is reachable from the whole pass before bytes are published. */
	return 0;
}

/* Allocates, assembles and cleans every native list/binning/overflow input before publishing the complete CL job. */
static int
prepare_lists(
	struct bcm2711_vulkan_native_pass *pass,
	uint64_t *remaining)
{
	struct bcm2711_native_pass_sizes sizes;
	struct bcm2711_native_pass_image bin;
	struct bcm2711_native_pass_image render;
	struct bcm2711_native_pass_image tile;
	struct bcm2711_native_storage *storage;
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_v3d_cl_job *job;
	uint32_t index;
	uint32_t cursor;
	int error;

	/* Exact lengths and complete tile geometry precede physical command-storage acquisition. */
	error = bcm2711_native_pass_measure(&pass->state, pass->draws, &sizes);
	if (error != 0)
		return error;

	/* Three independent list images precede the pool and tile state in the fixed whole-pass owner vector. */
	error = allocate_storage(pass, sizes.bin, remaining, &storage);
	if (error != 0)
		return error;
	prepare_image(storage, &bin);
	error = allocate_storage(pass, sizes.render, remaining, &storage);
	if (error != 0)
		return error;
	prepare_image(storage, &render);
	error = allocate_storage(pass, sizes.tile, remaining, &storage);
	if (error != 0)
		return error;
	prepare_image(storage, &tile);
	error = allocate_storage(pass, sizes.pool, remaining, &storage);
	if (error != 0)
		return error;
	pass->state.pool = storage->view->address;
	pass->state.pool_bytes = (uint32_t)storage->view->bytes;
	error = allocate_storage(pass, sizes.state, remaining, &storage);
	if (error != 0)
		return error;
	pass->state.state = storage->view->address;
	pass->state.state_bytes = (uint32_t)storage->view->bytes;

	/* Each complete numerical draw sequence references only its attached independently mapped inputs. */
	cursor = BCM2711_NATIVE_PASS_BIN_PREFIX;
	draw = pass->first;
	while (draw != NULL) {
		/* An incomplete numerical draw cannot enter the enclosing command image. */
		if (draw->bin_bytes != BCM2711_NATIVE_BIN_BYTES)
			return EINVAL;
		kern_memcpy(bin.bytes + cursor, draw->bin, draw->bin_bytes);
		cursor += draw->bin_bytes;
		draw = draw->next;
	}

	/* The whole three-list serializer checks all complete native intervals before any sequence becomes active. */
	error = bcm2711_native_pass_encode(&pass->state, pass->draws, &bin, &render, &tile);
	if (error != 0)
		return error;

	/* Four independently mapped overflow reserves support supervised native binning without allocation in IRQ service. */
	job = &pass->job.command.cl;
	for (index = 0; index < 4U; index++) {
		error = allocate_storage(pass, 256U * 1024U, remaining, &storage);
		if (error != 0)
			return error;
		job->overflow[index].address = storage->view->address;
		job->overflow[index].bytes = (uint32_t)storage->view->bytes;
	}

	/* Every initialized command, tile/pool and overflow allocation becomes visible before its job address is published. */
	for (index = 0; index < pass->count; index++) {
		error = bcm2711_native_storage_clean(pass->storage[index]);
		if (error != 0)
			return error;
	}

	/* Complete bin/render endpoints borrow only this root's independently retained mapped owners. */
	job->bin_start = bin.address;
	job->bin_end = bin.address + bin.used;
	job->render_start = render.address;
	job->render_end = render.address + render.used;
	job->pool_address = pass->state.pool;
	job->pool_bytes = pass->state.pool_bytes;
	job->state_address = pass->state.state;
	job->state_bytes = pass->state.state_bytes;
	job->overflow_count = 4;
	job->clean_output = true;

	/* Succeeded: the caller owns a complete cleaned native graphics job without any target mutation or launch. */
	return 0;
}

/* Copies one complete owned mapping's numerical and CPU interval into an unpublished serializer descriptor. */
static void
prepare_image(
	struct bcm2711_native_storage *storage,
	struct bcm2711_native_pass_image *image)
{
	/* Native-storage creation bounds all complete allocation sizes before this narrowing and exposes initialized cached CPU bytes. */
	image->bytes = storage->view->buffer->address;
	image->address = storage->view->address;
	image->capacity = (uint32_t)storage->view->bytes;
	image->used = 0;

	/* Succeeded: the descriptor borrows exactly one complete independently retained native interval. */
	return;
}

/* Copies one full-image numerical target and native packed clear colour before allocating any command storage. */
static int
prepare_clear(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	uint32_t colour;
	uint32_t swap;
	void *cpu;
	int error;

	/* The independently pending primary supplies a typed immutable full-subresource clear selection. */
	if (record->opcode != GPU_OP_CMD_CLEAR_COLOR_IMAGE ||
	    record->semantic_error != 0 ||
	    record->objects[0] == NULL)
		return EINVAL;

	/* Every selected range names the same implemented colour image subresource. */
	if (record->objects[0]->kind != I915_VK_OBJ_IMAGE ||
	    record->count == 0 ||
	    record->count > 64U)
		return EINVAL;

	/* The clear preserves its explicit general or transfer destination layout. */
	if (record->layout != VK_IMAGE_LAYOUT_GENERAL && record->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return EINVAL;

	/* A complete typed binding and destination usage precede all physical backing access. */
	image = record->objects[0]->payload;
	if (image == NULL ||
	    image->memory == NULL ||
	    (image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		return EINVAL;

	/* Only complete native raster geometry and implemented UNORM formats enter the integer colour conversion. */
	if (image->width == 0 ||
	    image->width > 4096U ||
	    image->height == 0 ||
	    image->height > 4096U ||
	    image->bytes > 0xffffffffU)
		return EINVAL;

	/* Other formats have no implemented native clear interpretation. */
	if (image->format != VK_FORMAT_R8G8B8A8_UNORM && image->format != VK_FORMAT_B8G8R8A8_UNORM)
		return ENOTSUP;

	/* Packed clear bytes match the same raw RGBA8 store packet used for ordinary native output. */
	swap = 0;
	if (image->format == VK_FORMAT_B8G8R8A8_UNORM)
		swap = 1;
	error = bcm2711_native_colour_pack(record->words, swap, &colour);
	if (error != 0)
		return error;

	/* Exact coherent output backing gains an independent view reference before any numerical alias enters the root. */
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* The independent output edge must remain representable through all later preparation failures. */
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	bcm2711_v3d_memory_retain(view);
	pass->output = view;
	pass->cpu = cpu;
	pass->target = image;
	pass->initial_layout = record->layout;
	pass->final_layout = record->layout;

	/* Full target tile coverage requires no load, and hardware clear/store writes only valid framebuffer pixels. */
	pass->state.width = image->width;
	pass->state.height = image->height;
	pass->state.pitch = image->pitch;
	pass->state.output = address;
	pass->state.output_bytes = (uint32_t)image->bytes;
	pass->state.last_x = (image->width - 1U) / 64U;
	pass->state.last_y = (image->height - 1U) / 64U;
	pass->state.store = 1;
	pass->state.clear_colour = colour;
	pass->area.extent.width = image->width;
	pass->area.extent.height = image->height;
	pass->format = image->format;
	pass->load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

	/* Succeeded: the existing CPU partial-clear path is disabled for this independently owned native full-image clear. */
	return 0;
}
