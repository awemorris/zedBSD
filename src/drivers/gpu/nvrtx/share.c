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

static int share_export(void *private_data, void *session, void *object, const struct gpu_image_descriptor *image, void **result);
static void share_release(void *private_data, void *shared);
static int share_import(void *private_data, void *session, void *shared, uint32_t flags, void **result, uint32_t *resource_id);
static int share_query_device(void *private_data, void *session, struct gpu_device_info *request);
static int share_constraints(void *private_data, void *session, struct gpu_scanout_constraints *request);

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
 * Exports a blob to the other opens of the device: the export is one more
 * reference to the blob's backing, which the core releases with
 * share_release().  The image description is the core's to keep; the
 * display checks its own conditions when the image is presented.
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

	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(image);

	node = private_data;
	resource = object;

	/* Only a blob has VRAM to share (the core exports only blobs). */
	if (resource->kind != NVRTX_RESOURCE_BLOB)
		return EINVAL;

	/* The export holds the backing as a blob does. */
	mutex_lock(&node->lock);

	resource->backing->refs++;
	resource->backing->exported = 1;

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

	(void)flags;

	node = private_data;
	backing = shared;
	*result = NULL;
	*resource_id = 0U;

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
