/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sharing and scanout operations of the nvrtx GPU device (the gpu-device
 * stage).
 *
 * A blob's VRAM can be shared between the opens of the device: an export
 * takes one reference to the blob's backing for the GPU core's shared
 * object, and an import makes a new blob of the importing open that names
 * the same backing, so a compositor can show a client's buffer as it is
 * (GPU_DISPLAY_PRESENT with BLOB, its direct scanout).  The backing goes
 * when the last blob and export naming it are gone (node.c).
 *
 * No physical view of the VRAM is lent to another device, and no other
 * device's memory is imported for scanout: the window would have to read
 * system memory or a peer's BAR, which is not verified on this hardware.
 * The scanout constraints say so (shared and copied, never foreign): a
 * shared image is a blob of this device, its rows a multiple of 64 bytes
 * apart from a 256-byte boundary; a copied one is ordinary storage.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-scanout.h>
#include <drivers/gpu/gpu-share.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-scanout.h>

#include "drivers/gpu/nvrtx/nvrtx-node.h"

/*
 * A window reads a surface from a 256-byte boundary, with rows a multiple of
 * 64 bytes apart.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c
 * (wndwc57e_image_set, the offset and the pitch the window is given).
 */
#define SHARE_OFFSET_ALIGN		256U
#define SHARE_STRIDE_ALIGN		64U

/*
 * The block-linear images the device shares: GOBs of 64 bytes by 8 rows in
 * blocks one GOB tall, of the device's ordinary page kind (layout
 * parameter 0), starting on a GOB, at most the executor's largest side.
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/nil/tiling.rs (the Fermi GOB of
 * 64 bytes by 8 rows, 512 bytes).
 */
#define SHARE_GOB_WIDTH			64U
#define SHARE_GOB_HEIGHT		8U
#define SHARE_GOB_BYTES			512U
#define SHARE_SIDE_MAX			16384U

static int share_export(void *private_data, void *session, void *object, const struct gpu_image_descriptor *image, void **result);
static void share_release(void *private_data, void *shared);
static int share_import(void *private_data, void *session, void *shared, uint32_t flags, void **result, uint32_t *resource_id);
static int share_query_device(void *private_data, void *session, struct gpu_device_info *request);
static int share_constraints(void *private_data, void *session, struct gpu_scanout_constraints *request);
static int share_image_layout(void *private_data, const struct gpu_image_descriptor *image, uint64_t *bytes);
static int share_image_layouts(void *private_data, void *session, struct gpu_image_layouts *request);
static int share_described(const struct nvrtx_backing *backing, const struct gpu_image_descriptor *image);
static int share_bound_matches(const struct nvrtx_backing *backing, const struct gpu_image_descriptor *image);
static void share_list(struct gpu_image_layouts *request, uint32_t tiling, uint32_t flags);

/*
 * The sharing operations of the nvrtx GPU device: export, release and
 * import between its opens; no physical view for another device.
 *
 * Shared by the one node; immutable.
 */
const struct drv_gpu_share_ops nvrtx_share_ops = {
	share_export,
	share_release,
	share_import,
	NULL,
	NULL
};

/*
 * The scanout operations of the nvrtx GPU device: the pairing query and
 * the display's constraints; no foreign import.
 *
 * Shared by the one node; immutable.
 */
const struct drv_gpu_scanout_ops nvrtx_scanout_ops = {
	share_query_device,
	share_constraints,
	NULL
};

/*
 * The image layouts of the nvrtx GPU device besides linear rows: NVIDIA's
 * block linear in one-GOB blocks, the layout the executor draws in fastest
 * (a colour target with a depth target is drawn only block linear).
 *
 * Shared by the one node; immutable.
 */
const struct drv_gpu_layout_ops nvrtx_layout_ops = {
	share_image_layout,
	share_image_layouts
};

/*
 * Exports a blob to the other opens of the device: the export is one more
 * reference to the blob's backing, which the core releases with
 * share_release().  An image's description (the core checked it) is kept
 * with the backing the first time and must be the same every later time,
 * and it must describe the image the executor bound to the blob, if any;
 * EINVAL otherwise.  The display shows the blob as described.
 */
static int
share_export(
	void *private_data,
	void *session,
	void *object,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct nvrtx_node *node;
	struct nvrtx_resource *resource;
	struct nvrtx_backing *backing;
	int same;
	int matches;

	UNUSED_PARAMETER(session);

	node = private_data;
	resource = object;

	/* Only a blob has VRAM to share (the core exports only blobs). */
	if (resource->kind != NVRTX_RESOURCE_BLOB)
		return EINVAL;

	/* The export holds the backing as a blob does, an image's description with it. */
	backing = resource->backing;
	mutex_lock(&node->lock);

	if (image != NULL) {
		same = share_described(backing, image);
		matches = share_bound_matches(backing, image);
		if (!same || !matches) {
			mutex_unlock(&node->lock);
			return EINVAL;
		}
		if (!backing->described) {
			backing->image = *image;
			backing->described = 1;
		}
	}
	backing->refs++;
	backing->exported = 1;

	mutex_unlock(&node->lock);

	/* Succeeded: the core holds the backing until it releases the export. */
	*result = resource->backing;
	return 0;
}

/* Releases one export of a blob's backing; the last reference gives the VRAM back. */
static void
share_release(
	void *private_data,
	void *shared)
{
	struct nvrtx_node *node;

	node = private_data;

	/* Drops the export's reference. */
	nvrtx_node_backing_put(node, shared);
}

/*
 * Imports an exported backing into a session as a blob of its own, which
 * names the same VRAM and is destroyed like any blob.
 */
static int
share_import(
	void *private_data,
	void *session,
	void *shared,
	uint32_t flags,
	void **result,
	uint32_t *resource_id)
{
	struct nvrtx_node *node;
	struct nvrtx_backing *backing;
	struct nvrtx_resource *resource;
	int block_linear;

	node = private_data;
	backing = shared;
	*result = NULL;
	*resource_id = 0U;

	/* The display scans a block-linear image only when the boot let it (its pixels are checked by eye). */
	block_linear = 0;
	mutex_lock(&node->lock);

	if (backing->described && backing->image.tiling == GPU_IMAGE_NV_BLOCK_LINEAR)
		block_linear = 1;

	mutex_unlock(&node->lock);

	if ((flags & GPU_IMPORT_SCANOUT) != 0U && block_linear && node->disp_bl == 0)
		return ENOTSUP;

	/* Allocates the importing session's blob. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* The blob holds one more reference to the backing (the export's keeps it alive meanwhile). */
	mutex_lock(&node->lock);

	backing->refs++;

	mutex_unlock(&node->lock);

	/* Describes the blob: the backing's size, as its exporter asked for it. */
	resource->kind = NVRTX_RESOURCE_BLOB;
	resource->bytes = backing->bytes;
	resource->backing = backing;

	/* Succeeded: the session has the blob, numbered for the protocol and listed. */
	*resource_id = nvrtx_node_resource_id(node);
	nvrtx_node_blob_listed(node, session, resource, *resource_id);
	*result = resource;
	return 0;
}

/*
 * Answers the pairing query: the device shows images, and renders them too
 * when the renderer is bound (nvrtx.vk=1); it has no companion.
 */
static int
share_query_device(
	void *private_data,
	void *session,
	struct gpu_device_info *request)
{
	struct nvrtx_node *node;

	UNUSED_PARAMETER(session);

	node = private_data;

	/* A display, and a renderer when bound; no preferred other device. */
	request->roles = GPU_DEVICE_DISPLAY;
	if (node->renderer != 0)
		request->roles |= GPU_DEVICE_RENDER;
	request->companion_id = 0U;

	/* Succeeded: the roles are filled in. */
	return 0;
}

/*
 * Answers the scanout constraints of the one display: shared (a blob of
 * this device) or copied (storage), BGRA or RGBA, rows a multiple of 64
 * bytes apart from a 256-byte boundary, no placement condition (every blob
 * is contiguous VRAM).  Returns 0, ENOENT for another display or ESTALE for
 * another generation.
 */
static int
share_constraints(
	void *private_data,
	void *session,
	struct gpu_scanout_constraints *request)
{
	UNUSED_PARAMETER(private_data);
	UNUSED_PARAMETER(session);

	/* Only the one display of this generation exists. */
	if (request->display_id != NVRTX_NODE_DISPLAY_ID)
		return ENOENT;
	if (request->generation != NVRTX_NODE_GENERATION)
		return ESTALE;

	/* A blob of this device is shown as it is; storage is copied by the CPU. */
	request->flags = GPU_SCANOUT_SHARED | GPU_SCANOUT_COPY;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;

	/* The window's linear surface. */
	request->stride_alignment = SHARE_STRIDE_ALIGN;
	request->offset_alignment = SHARE_OFFSET_ALIGN;
	request->placement = 0U;
	request->max_dma_address = 0U;

	/* Succeeded: the constraints are filled in. */
	return 0;
}

/*
 * Checks a description of a block-linear image the device writes and reads
 * and reports the bytes from its offset the image takes: NVIDIA's block
 * linear of one-GOB blocks and the ordinary page kind, rows a whole number
 * of GOBs wide holding the width, a start on a GOB, a side the executor
 * takes.  EOPNOTSUPP for any other layout or variant.
 */
static int
share_image_layout(
	void *private_data,
	const struct gpu_image_descriptor *image,
	uint64_t *bytes)
{
	uint64_t rows;

	UNUSED_PARAMETER(private_data);

	/* The one variant the executor lays out. */
	if (image->tiling != GPU_IMAGE_NV_BLOCK_LINEAR || image->layout_parameter != 0U)
		return EOPNOTSUPP;

	/* A size the executor and its colour target take. */
	if (image->width == 0U || image->width > SHARE_SIDE_MAX || image->height == 0U || image->height > SHARE_SIDE_MAX)
		return EOPNOTSUPP;

	/* Whole GOBs a row, holding the row's pixels, from a GOB. */
	if ((image->stride % SHARE_GOB_WIDTH) != 0U || image->stride < image->width * 4U)
		return EOPNOTSUPP;
	if ((image->offset % SHARE_GOB_BYTES) != 0U)
		return EOPNOTSUPP;

	/* Succeeded: the rows of GOBs the image takes, its height in whole GOBs. */
	rows = ((uint64_t)image->height + SHARE_GOB_HEIGHT - 1U) & ~(uint64_t)(SHARE_GOB_HEIGHT - 1U);
	*bytes = (uint64_t)image->stride * rows;
	return 0;
}

/*
 * Answers the layouts of an image: block linear first (the executor draws
 * in it fastest), then linear rows.  Both are shared between the device's
 * opens; the renderer, when bound, draws and samples both; the display,
 * when asked about, shows linear rows, and block linear only when the boot
 * let it (nvrtx.disp.bl=1).  Only entries with every use asked for.
 */
static int
share_image_layouts(
	void *private_data,
	void *session,
	struct gpu_image_layouts *request)
{
	struct nvrtx_node *node;
	uint32_t uses;
	uint32_t block_linear;
	uint32_t linear;

	UNUSED_PARAMETER(session);

	node = private_data;

	/* What every layout of the device has: sharing, and drawing and sampling with the renderer. */
	uses = GPU_LAYOUT_SHARE;
	if (node->renderer != 0)
		uses |= GPU_LAYOUT_RENDER | GPU_LAYOUT_SAMPLE;

	/* The display's scanout: linear rows always, block linear when the boot let it. */
	block_linear = uses;
	linear = uses;
	if (request->display_id == NVRTX_NODE_DISPLAY_ID) {
		linear |= GPU_LAYOUT_SCANOUT;
		if (node->disp_bl != 0)
			block_linear |= GPU_LAYOUT_SCANOUT;
	}

	/* Block linear only for a renderer that writes it and a size it takes. */
	if (node->renderer != 0 && request->width <= SHARE_SIDE_MAX && request->height <= SHARE_SIDE_MAX)
		share_list(request, GPU_IMAGE_NV_BLOCK_LINEAR, block_linear);
	share_list(request, GPU_IMAGE_LINEAR, linear);

	/* Succeeded: the layouts with every use asked for, fastest first. */
	return 0;
}

/*
 * Tells whether a description is the one the backing was first exported
 * with, or the backing has none yet.  The caller holds the node's lock.
 */
static int
share_described(
	const struct nvrtx_backing *backing,
	const struct gpu_image_descriptor *image)
{
	const struct gpu_image_descriptor *kept;

	/* A first description is the backing's own. */
	if (!backing->described)
		return 1;

	/* Every field a reader lays the image out by. */
	kept = &backing->image;
	if (kept->width != image->width || kept->height != image->height || kept->format != image->format)
		return 0;
	if (kept->stride != image->stride || kept->offset != image->offset)
		return 0;
	if (kept->tiling != image->tiling || kept->layout_parameter != image->layout_parameter)
		return 0;

	/* Succeeded: the same image. */
	return 1;
}

/*
 * Tells whether a description is the shape of the image the executor
 * bound to the backing first, or none is bound.  The caller holds the
 * node's lock.
 */
static int
share_bound_matches(
	const struct nvrtx_backing *backing,
	const struct gpu_image_descriptor *image)
{
	int tiled;

	/* Nothing bound yet: the description stands alone. */
	if (!backing->bound)
		return 1;

	/* The layout, the rows, the size and the place of the bound image. */
	tiled = 0;
	if (image->tiling == GPU_IMAGE_NV_BLOCK_LINEAR)
		tiled = 1;
	if (tiled != backing->bound_tiled)
		return 0;
	if (image->stride != backing->bound_pitch || image->offset != backing->bound_offset)
		return 0;
	if (image->width != backing->bound_width || image->height != backing->bound_height)
		return 0;

	/* Succeeded: the description is the bound image's. */
	return 1;
}

/* Adds a layout to an answer when it has every use asked for and there is room. */
static void
share_list(
	struct gpu_image_layouts *request,
	uint32_t tiling,
	uint32_t flags)
{
	uint32_t index;

	/* A layout without a use asked for is left out. */
	if ((flags & request->uses) != request->uses)
		return;

	/* The answer is full. */
	if (request->count >= GPU_IMAGE_LAYOUTS_MAX)
		return;

	/* The next entry. */
	index = request->count;
	request->entry[index].tiling = tiling;
	request->entry[index].parameter = 0U;
	request->entry[index].flags = flags;
	request->count = index + 1U;
}
