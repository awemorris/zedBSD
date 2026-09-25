/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU resources (see resource.h).
 *
 * Creation allocates a contiguous object and binds it into the session's
 * address space under the device mutex; a quarantined session or a faulted
 * device adds nothing the hardware can see.  Destroy of a quarantined
 * session's object leaves it on the device registry until the checked reset.
 *
 * Sharing is between the opens of this node only: an export is a counted
 * reference on the object, and an import is an alias object that borrows the
 * backing, is bound into the importer's address space and frees only itself.
 */

#include "i915.h"
#include "memory.h"
#include "ppgtt.h"
#include "resource.h"
#include "session.h"
#include "render/render.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-share.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

static int i915_resource_create(void *opaque, void *private_session, const struct gpu_resource_create *request, void **result);
static void i915_resource_destroy(void *opaque, void *private_session, void *private_object);
static int i915_resource_read(void *opaque, void *private_session, void *private_object, uint64_t offset, void *buffer, uint32_t bytes);
static int i915_resource_write(void *opaque, void *private_session, void *private_object, uint64_t offset, const void *buffer, uint32_t bytes);
static int i915_blob_create(void *opaque, void *private_session, const struct gpu_blob_create *request, void **result, uint32_t *resource_id);
static int i915_resource_map(void *opaque, void *private_session, void *private_object, struct drv_gpu_mapping *mapping);
static int i915_share_export(void *opaque, void *private_session, void *private_object, const struct gpu_image_descriptor *image, void **result);
static void i915_share_release(void *opaque, void *shared);
static int i915_share_import(void *opaque, void *private_session, void *shared, void **result, uint32_t *resource_id);

/*
 * The sharing operations of the node.
 *
 * The objects have no scanout backing to lend in this stage, so that
 * optional operation is absent.
 */
static const struct drv_gpu_share_ops i915_share_ops = {
	i915_share_export,
	i915_share_release,
	i915_share_import,
	NULL
};

/*
 * Binds the resource, blob, mapping and sharing operations into the GPU node's operation table.
 */
void
drv_i915_resource_bind_ops(
	struct drv_gpu_ops *ops)
{
	/* Storage objects and their CPU copies. */
	ops->resource_create = i915_resource_create;
	ops->resource_destroy = i915_resource_destroy;
	ops->resource_read = i915_resource_read;
	ops->resource_write = i915_resource_write;

	/* Blobs and their CPU mappings. */
	ops->blob_create = i915_blob_create;
	ops->resource_map = i915_resource_map;

	/* Sharing between the opens of the node. */
	ops->share = &i915_share_ops;
}

/* Allocates a storage object and binds it into the session's address space. */
static int
i915_resource_create(
	void *opaque,
	void *private_session,
	const struct gpu_resource_create *request,
	void **result)
{
	struct i915_device *device;
	struct i915_session *session;
	struct i915_gem_object *object;
	int error;

	/* A failed creation stays invisible to the core's resource table. */
	device = opaque;
	session = private_session;
	*result = NULL;

	/* Only plain storage exists; other usages have no meaning for this node yet. */
	if (request->usage != GPU_RESOURCE_USAGE_STORAGE)
		return EINVAL;

	/* Allocates and binds under the device mutex, serialized with every other session operation. */
	mutex_lock(&device->mutex);

	/* A quarantined session or a faulted device may not add hardware-visible state. */
	if (session->quarantined != 0U || device->failed != 0U) {
		mutex_unlock(&device->mutex);
		return ENODEV;
	}

	/* Allocates the zeroed contiguous object. */
	error = drv_i915_gem_create(&device->gem, request->bytes, &object);
	if (error != 0) {
		mutex_unlock(&device->mutex);
		return error;
	}

	/* Makes the object addressable by this session's contexts only. */
	error = drv_i915_gem_bind_vm(session->vm, object);
	if (error != 0) {
		drv_i915_gem_destroy(&device->gem, object);
		mutex_unlock(&device->mutex);
		return error;
	}

	/* The public handle lets a native stream name this object in a relocation. */
	object->handle = request->handle;
	object->session_next = session->objects;
	session->objects = object;

	/* Numbers the object per session for the log line the harness parses. */
	object->slot = session->next_slot;
	session->next_slot++;
	session->resources++;
	kern_logf("i915: resource session=%u slot=%u bytes=%llu phys=0x%llx va=0x%llx handle=%llu\n",
	    session->identifier,
	    object->slot,
	    (unsigned long long)object->bytes,
	    (unsigned long long)object->run.paddr,
	    (unsigned long long)object->va,
	    (unsigned long long)object->handle);

	mutex_unlock(&device->mutex);

	*result = object;

	/* Succeeded: the session owns zeroed storage the GPU can address. */
	return 0;
}

/* Releases a storage object; a quarantined session's object is retained. */
static void
i915_resource_destroy(
	void *opaque,
	void *private_session,
	void *private_object)
{
	struct i915_device *device;
	struct i915_session *session;
	struct i915_gem_object *object;
	struct i915_gem_object **position;

	device = opaque;
	session = private_session;
	object = private_object;

	/* The core only destroys an object it created, so a null object is a bug, not a case. */
	if (object == NULL)
		return;

	/* Unbinds and frees under the device mutex, serialized with the session's submissions. */
	mutex_lock(&device->mutex);

	/* Tells an allocation whose storage this object was that it has none from now on. */
	if (device->vk != NULL)
		drv_i915_render_blob_detach(device->vk, object);

	/* Finds the object's link in the session's list. */
	position = &session->objects;
	while (*position != NULL && *position != object)
		position = &(*position)->session_next;

	/* The object leaves the session's list whatever happens to its backing. */
	if (*position == object)
		*position = object->session_next;

	/* The GPU may still name a quarantined session's object; the checked reset frees it. */
	if (session->quarantined != 0U) {
		object->quarantined = 1U;
	} else {
		drv_i915_gem_unbind_vm(object);
	}

	/* Frees the object, or leaves a retained one on the registry. */
	session->resources--;
	drv_i915_gem_destroy(&device->gem, object);

	mutex_unlock(&device->mutex);
}

/* Copies bytes out of a storage object. */
static int
i915_resource_read(
	void *opaque,
	void *private_session,
	void *private_object,
	uint64_t offset,
	void *buffer,
	uint32_t bytes)
{
	struct i915_device *device;
	struct i915_gem_object *object;
	int error;

	UNUSED_PARAMETER(private_session);

	/* The core resolved the object; the session is implied by its handle. */
	device = opaque;
	object = private_object;

	/* Copies under the device mutex so no concurrent destroy can free the backing. */
	mutex_lock(&device->mutex);

	error = drv_i915_gem_read(object, offset, buffer, bytes);

	mutex_unlock(&device->mutex);

	/* Reports a range outside the object. */
	if (error != 0)
		return error;

	/* Succeeded: the kernel buffer holds the object's bytes. */
	return 0;
}

/* Copies bytes into a storage object. */
static int
i915_resource_write(
	void *opaque,
	void *private_session,
	void *private_object,
	uint64_t offset,
	const void *buffer,
	uint32_t bytes)
{
	struct i915_device *device;
	struct i915_gem_object *object;
	int error;

	UNUSED_PARAMETER(private_session);

	/* The core already copied the caller's bytes into a bounded kernel buffer. */
	device = opaque;
	object = private_object;

	/* Copies under the device mutex, serialized with submissions that may read the object. */
	mutex_lock(&device->mutex);

	error = drv_i915_gem_write(object, offset, buffer, bytes);

	mutex_unlock(&device->mutex);

	/* Reports a range outside the object. */
	if (error != 0)
		return error;

	/* Succeeded: the object holds the new bytes for the GPU's next access. */
	return 0;
}

/* Allocates host storage for a memory blob and binds it into the session's address space. */
static int
i915_blob_create(
	void *opaque,
	void *private_session,
	const struct gpu_blob_create *request,
	void **result,
	uint32_t *resource_id)
{
	struct i915_device *device;
	struct i915_session *session;
	struct i915_gem_object *object;
	int error;

	/* A failed creation stays invisible to the core's resource table. */
	device = opaque;
	session = private_session;
	*result = NULL;
	*resource_id = 0U;

	/* Only mappable host storage is backed. */
	if (request->flags == 0U)
		return EOPNOTSUPP;

	/*
	 * Shareable and cross-device blobs are accepted as well; with no scanout
	 * import the core refuses any foreign import of them.
	 */
	if ((request->flags & ~(GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE)) != 0U)
		return EOPNOTSUPP;

	/* Allocates and binds under the device mutex, serialized with every other session operation. */
	mutex_lock(&device->mutex);

	/* A quarantined session or a faulted device may not add hardware-visible state. */
	if (session->quarantined != 0U || device->failed != 0U) {
		mutex_unlock(&device->mutex);
		return ENODEV;
	}

	/* Allocates the zeroed contiguous object. */
	error = drv_i915_gem_create(&device->gem, request->bytes, &object);
	if (error != 0) {
		mutex_unlock(&device->mutex);
		return error;
	}

	/* Makes the blob addressable by this session's contexts only. */
	error = drv_i915_gem_bind_vm(session->vm, object);
	if (error != 0) {
		drv_i915_gem_destroy(&device->gem, object);
		mutex_unlock(&device->mutex);
		return error;
	}

	/* The public handle and the slot let the client name this blob in its commands. */
	object->handle = request->handle;
	object->session_next = session->objects;
	session->objects = object;
	object->slot = session->next_slot;
	session->next_slot++;
	session->resources++;

	mutex_unlock(&device->mutex);

	/*
	 * Makes a blob that names a device memory allocation that allocation's
	 * storage.  XXX: a blob_id no allocation has is only logged; the blob
	 * stands alone and its creation still succeeds.
	 */
	if (request->blob_id != 0U && device->vk != NULL) {
		error = drv_i915_render_blob_attach(device->vk, request->blob_id, object);
		if (error != 0) {
			kern_logf("i915: vk: XXX blob_id %llu is not the storage of any allocation (error %d); the blob stands alone\n",
			    (unsigned long long)request->blob_id,
			    error);
		}
	}

	/* The core tracks the blob as a resource that commands and mappings reference. */
	*result = object;
	*resource_id = object->slot;

	/* Succeeded: the session owns zeroed storage the GPU can address. */
	return 0;
}

/* Exposes a blob's system-RAM backing as a CPU mapping for the client. */
static int
i915_resource_map(
	void *opaque,
	void *private_session,
	void *private_object,
	struct drv_gpu_mapping *mapping)
{
	struct i915_device *device;
	struct i915_gem_object *backing;
	void *address;

	UNUSED_PARAMETER(private_session);

	/* The mapping is of the blob's own pages, independent of the session. */
	device = opaque;
	backing = private_object;

	/* Describes the backing under the device mutex. */
	mutex_lock(&device->mutex);

	/* Finds the backing's kernel alias; managed RAM is direct-mapped. */
	address = kern_pmem_to_kernel(backing->run.paddr);
	if (address == NULL) {
		mutex_unlock(&device->mutex);
		return EFAULT;
	}

	/* Hands the client the exact page-aligned extent of the blob. */
	kern_memset(mapping, 0, sizeof(*mapping));
	mapping->physical = (uint64_t)backing->run.paddr;
	mapping->address = address;
	mapping->bytes = backing->bytes;
	mapping->attributes = 0U;

	mutex_unlock(&device->mutex);

	/* Succeeded: the core can map and pin this blob's storage. */
	return 0;
}

/*
 * Exports an object to the other opens of this node.
 *
 * XXX: same device only; there is no cross-device buffer sharing and no image
 * metadata beyond what the core keeps.
 */
static int
i915_share_export(
	void *opaque,
	void *private_session,
	void *private_object,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct i915_device *device;
	struct i915_gem_object *source;

	UNUSED_PARAMETER(private_session);
	UNUSED_PARAMETER(image);

	device = opaque;
	source = private_object;

	/*
	 * The export is one reference on the object: its backing outlives its
	 * own destroy until the last export and alias are gone.
	 */
	mutex_lock(&device->mutex);

	source->share_refs++;

	mutex_unlock(&device->mutex);

	*result = source;

	/* Succeeded: the core holds the export until it releases it. */
	return 0;
}

/* Releases one export of an object. */
static void
i915_share_release(
	void *opaque,
	void *shared)
{
	struct i915_device *device;

	device = opaque;

	/* Drops the export's reference; the last one frees a destroyed object's backing. */
	mutex_lock(&device->mutex);

	drv_i915_gem_share_put(&device->gem, shared);

	mutex_unlock(&device->mutex);
}

/* Imports an exported object into a session as an alias that borrows its backing. */
static int
i915_share_import(
	void *opaque,
	void *private_session,
	void *shared,
	void **result,
	uint32_t *resource_id)
{
	struct i915_device *device;
	struct i915_session *session;
	struct i915_gem_object *source;
	struct i915_gem_object *alias;
	int error;

	/* A failed import stays invisible to the core's resource table. */
	device = opaque;
	session = private_session;
	source = shared;
	*result = NULL;
	*resource_id = 0U;

	/* Allocates the alias object. */
	alias = kern_calloc(1U, sizeof(*alias));
	if (alias == NULL)
		return ENOMEM;

	/* Binds and records the alias under the device mutex. */
	mutex_lock(&device->mutex);

	/* The alias borrows the exported object's backing. */
	alias->run = source->run;
	alias->address = source->address;
	alias->bytes = source->bytes;
	alias->pages = source->pages;

	/* Makes the backing addressable in the importer's address space. */
	error = drv_i915_gem_bind_vm(session->vm, alias);
	if (error != 0) {
		mutex_unlock(&device->mutex);
		kern_free(alias);
		return error;
	}

	/* The alias is one more reference that keeps the exported backing alive. */
	alias->alias_of = source;
	source->share_refs++;

	/* Adds the alias to the session's objects under a new slot. */
	alias->session_next = session->objects;
	session->objects = alias;
	alias->slot = session->next_slot;
	session->next_slot++;
	session->resources++;

	mutex_unlock(&device->mutex);

	*result = alias;
	*resource_id = alias->slot;

	/* Succeeded: the session addresses the shared backing through the alias. */
	return 0;
}
