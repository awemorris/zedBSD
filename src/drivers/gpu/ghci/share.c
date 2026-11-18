/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sharing of a GHCI device's memory between its sessions: an
 * exported resource (an image for a compositor, or any allocation) and
 * its imports into other sessions of the same device.
 *
 * An export is a record that holds the resource's memory, so the memory
 * stays while the export lives, even after the exporting session gave the
 * resource back.  An import is a blob of the importing session that shows
 * the same memory and holds it too; it reaches the importer's GPU
 * addresses only when the importer maps it.  The memory goes back once
 * the resource, every export, every import and every mapping have let it
 * go, the mappings only after their unmaps passed.
 *
 * An import for the display (GPU_IMPORT_SCANOUT) is refused with ENOTSUP:
 * the device does not drive a display yet, and the caller copies its
 * frames instead.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-share.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-scanout.h>

#include "ghci.h"

/*
 * One export: the device, the memory it holds, the blob flags an import
 * gets (the exporter's: a blob not made mappable stays so), and the image
 * the export described (has_image 0 for an allocation-only export).  It lives from
 * the export to its release, whatever happens to the sessions.
 */
struct ghci_share {
	struct ghci_device *device;
	struct ghci_allocation *allocation;
	uint32_t flags;
	int has_image;
	struct gpu_image_descriptor image;
};

static int share_export(void *private_data, void *opaque, void *object, const struct gpu_image_descriptor *image, void **result);
static void share_release(void *private_data, void *shared);
static int share_import(void *private_data, void *opaque, void *shared, uint32_t flags, void **result, uint32_t *resource_id);

/* The sharing operations; the table lives as long as the kernel. */
const struct drv_gpu_share_ops ghci_share_ops = {
	share_export,
	share_release,
	share_import,
	NULL,
	NULL
};

/*
 * Exports a session's resource: the record holds its memory and keeps the
 * image, when there is one.  The core checked the image against the
 * resource; every GHCI resource is linear memory.
 */
static int
share_export(
	void *private_data,
	void *opaque,
	void *object,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct ghci_device *device;
	struct ghci_resource *resource;
	struct ghci_share *share;

	(void)opaque;

	/* The record. */
	device = private_data;
	resource = object;
	share = kern_calloc(1U, sizeof(*share));
	if (share == NULL)
		return ENOMEM;
	share->device = device;
	share->allocation = resource->allocation;
	share->flags = GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE;
	if (resource->blob)
		share->flags = resource->flags | GPU_BLOB_SHAREABLE;
	if (image != NULL) {
		share->has_image = 1;
		share->image = *image;
	}

	/* It holds the memory. */
	mutex_lock(&device->lock);

	ghci_memory_hold(share->allocation);

	mutex_unlock(&device->lock);

	/* Succeeded: the export is the core's until it releases it. */
	*result = share;
	return 0;
}

/* Releases an export: it lets the memory go. */
static void
share_release(
	void *private_data,
	void *shared)
{
	struct ghci_device *device;
	struct ghci_share *share;

	/* The memory is let go under the device's lock. */
	device = private_data;
	share = shared;
	mutex_lock(&device->lock);

	ghci_memory_unhold(device, share->allocation);

	mutex_unlock(&device->lock);

	/* The record goes. */
	kern_free(share);
}

/*
 * Imports an export into a session: a blob of the session, mappable when
 * the exported resource was, that shows the same memory and holds it.
 */
static int
share_import(
	void *private_data,
	void *opaque,
	void *shared,
	uint32_t flags,
	void **result,
	uint32_t *resource_id)
{
	struct ghci_device *device;
	struct ghci_resource *resource;
	struct ghci_share *share;
	int error;

	/* An export of this device only. */
	device = private_data;
	share = shared;
	if (share->device != device)
		return EINVAL;

	/* No display scans it; the caller copies instead. */
	if ((flags & GPU_IMPORT_SCANOUT) != 0U)
		return ENOTSUP;

	/* The blob of the session. */
	mutex_lock(&device->lock);

	error = ghci_resource_add(device, opaque, share->allocation, 1, share->flags, &resource);

	mutex_unlock(&device->lock);
	if (error != 0)
		return error;

	/* Succeeded: the blob is the session's. */
	*result = resource;
	*resource_id = resource->resource_id;
	return 0;
}
