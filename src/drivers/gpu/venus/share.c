/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shared host allocations and independent Venus context memberships.
 */

#include "internal.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu-share.h>
#include <kern/kmem.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <limits.h>

/*
 * One renderer context's attachment to an allocation shared by session aliases.
 * The controller mutex protects the list and its alias count; exported fds and
 * scanout holds retain allocation storage without requiring any context.
 */
struct venus_shared_context {
	struct venus_shared_context *next;
	uint32_t context;
	unsigned references;
};

static int share_export(void *device, void *private_session, void *object, const struct gpu_image_descriptor *image, void **result);
static void share_release(void *device, void *object);
static int share_import(void *device, void *private_session, void *object, void **result, uint32_t *resource_id);
static int share_promote(struct venus_controller *controller, struct venus_resource *resource, const struct gpu_image_descriptor *image);
static int share_context_add(struct venus_controller *controller, struct venus_share *share, uint32_t context);
static void share_context_drop(struct venus_controller *controller, struct venus_share *share, uint32_t context);

/* Every registered Venus instance borrows these immutable allocation operations. */
const struct drv_gpu_share_ops drv_venus_share_operations = {
	share_export, share_release, share_import, NULL
};

/*
 * Retains one allocation reference while the controller mutex is held.
 */
int
drv_venus_share_hold_locked(
	struct venus_share *share)
{
	/* An exhausted counter cannot make live host storage appear unreferenced. */
	if (share->references == UINT_MAX)
		return EOVERFLOW;

	/* The additional fd, alias or scanout now independently prevents host unref. */
	share->references++;

	/* Succeeded: this owner may retain the allocation without a producing context. */
	return 0;
}

/*
 * Releases one allocation reference and quarantines uncertain terminal cleanup.
 */
void
drv_venus_share_put_locked(
	struct venus_controller *controller,
	struct venus_share *share)
{
	struct venus_resource *storage;
	int error;

	/* This owner has finished every use before making terminal cleanup possible. */
	share->references--;
	if (share->references != 0U)
		return;

	/* Session membership references must have retired before the allocation's last hold. */
	if (share->contexts != NULL) {
		drv_venus_transport_fail(&controller->transport, EIO);
		kern_logf("venus: shared allocation %u retained with live contexts\n", share->storage->identifier);
		return;
	}

	/* The independent anchor has no originating context left to destroy. */
	storage = share->storage;
	error = drv_venus_resource_release_locked(controller, storage);
	if (error != 0) {
		drv_venus_transport_fail(&controller->transport, error);
		kern_logf("venus: shared allocation %u retained for reset: %d\n", storage->identifier, error);
	}

	/* The controller retains uncertain storage separately from this completed ownership record. */
	kern_free(share);

	/* Succeeded: terminal host ownership is retired or explicitly quarantined. */
	return;
}

/*
 * Retires one session alias without invalidating exported or scanned storage.
 */
void
drv_venus_share_resource_destroy_locked(
	struct venus_controller *controller,
	struct venus_session *session,
	struct venus_resource *resource)
{
	struct venus_share *share;

	/* Context membership and allocation ownership are separate references. */
	share = resource->share;
	share_context_drop(controller, share, session->context);

	/* The session wrapper owns no DMA memory independently from its anchor. */
	kern_free(resource);
	drv_venus_share_put_locked(controller, share);

	/* Succeeded: another process can continue using the same host allocation. */
	return;
}

/* Exports a linear image allocation after preserving its original session alias. */
static int
share_export(
	void *device,
	void *private_session,
	void *object,
	const struct gpu_image_descriptor *image,
	void **result)
{
	unsigned failed;
	struct venus_controller *controller;
	struct venus_session *session;
	struct venus_resource *resource;
	int different;
	int error;

	/* The callback never transfers partial ownership to the common GPU layer. */
	controller = device;
	session = private_session;
	resource = object;
	*result = NULL;

	/* Allocation promotion and all context attachments use the same controller serialization. */
	mutex_lock(&controller->mutex);

	/* A failed transport cannot create a new independently usable capability. */
	failed = atomic_raw_load_acquire(&controller->transport.failed);
	if (failed != 0U) {
		mutex_unlock(&controller->mutex);
		return ENODEV;
	}

	/* Source ownership is checked independently of the common resource table. */
	if (resource->controller != controller ||
	    resource->context != session->context ||
	    resource->kind != VENUS_RESOURCE_BLOB) {
		mutex_unlock(&controller->mutex);
		return EINVAL;
	}

	/* External allocations need renderer exportability even when no display layout is requested. */
	if ((resource->blob_flags & GPU_BLOB_SHAREABLE) == 0U) {
		mutex_unlock(&controller->mutex);
		return EOPNOTSUPP;
	}

	/* Native opaque allocations share within this GPU but cannot become DMA scanout images. */
	if (image != NULL && (resource->blob_flags & GPU_BLOB_CROSS_DEVICE) == 0U) {
		mutex_unlock(&controller->mutex);
		return EOPNOTSUPP;
	}

	/* Allocation-only capabilities impose no linear image or scanout geometry. */
	if (image != NULL) {
		/* Native scanout geometry has a finite extent independent of allocation size. */
		if (image->width < 16U ||
		    image->height < 16U ||
		    image->width > 4096U ||
		    image->height > 4096U) {
			mutex_unlock(&controller->mutex);
			return EOPNOTSUPP;
		}
	}

	/* The first export separates the retained allocation from its original session wrapper. */
	if (resource->share == NULL) {
		error = share_promote(controller, resource, image);
		if (error != 0) {
			mutex_unlock(&controller->mutex);
			return error;
		}
	}

	/* Display metadata is assigned only by a separately validated image export. */
	if (image != NULL) {
		/* The first image capability may describe an allocation already shared without a layout. */
		if (resource->share->image.version == 0U)
			resource->share->image = *image;

		/* Later image capabilities must preserve the same native display interpretation. */
		different = kern_memcmp(&resource->share->image, image, sizeof(*image));
		if (different != 0) {
			mutex_unlock(&controller->mutex);
			return EINVAL;
		}
	}

	/* The common capability now acquires its own reference beside the source alias. */
	error = drv_venus_share_hold_locked(resource->share);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* No renderer context pointer escapes in this independently retained object. */
	*result = resource->share;

	mutex_unlock(&controller->mutex);

	/* Succeeded: the capability survives closure of every source descriptor. */
	return 0;
}

/* Drops one exported capability without requiring a current process or context. */
static void
share_release(
	void *device,
	void *object)
{
	struct venus_controller *controller;

	/* Device withdrawal remains blocked until this callback finishes. */
	controller = device;
	mutex_lock(&controller->mutex);

	drv_venus_share_put_locked(controller, object);

	mutex_unlock(&controller->mutex);

	/* Succeeded: the exporting process is irrelevant to terminal ownership. */
	return;
}

/* Attaches the shared allocation to one independent destination context. */
static int
share_import(
	void *device,
	void *private_session,
	void *object,
	void **result,
	uint32_t *resource_id)
{
	unsigned failed;
	struct venus_controller *controller;
	struct venus_session *session;
	struct venus_share *share;
	struct venus_resource *alias;
	int error;

	/* Failed import exposes neither a destination wrapper nor a resource identity. */
	controller = device;
	session = private_session;
	share = object;
	*result = NULL;
	*resource_id = 0U;

	/* Membership creation and allocation lifetime move together under the controller mutex. */
	mutex_lock(&controller->mutex);

	/* Device withdrawal or a foreign controller cannot create a usable import. */
	failed = atomic_raw_load_acquire(&controller->transport.failed);
	if (failed != 0U || share->storage->controller != controller) {
		mutex_unlock(&controller->mutex);
		return ENODEV;
	}

	/* Allocate a destination wrapper before submitting any attachment command. */
	alias = kern_calloc(1U, sizeof(*alias));
	if (alias == NULL) {
		mutex_unlock(&controller->mutex);
		return ENOMEM;
	}

	/* Reserve allocation ownership before a new context may refer to the host resource. */
	error = drv_venus_share_hold_locked(share);
	if (error != 0) {
		kern_free(alias);
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Repeated imports into one context share one actual host attachment. */
	error = share_context_add(controller, share, session->context);
	if (error != 0) {
		drv_venus_share_put_locked(controller, share);
		kern_free(alias);
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Aliases borrow immutable allocation views and own only their context membership. */
	*alias = *share->storage;
	alias->next = NULL;
	alias->share = share;
	alias->context = session->context;
	*result = alias;
	*resource_id = alias->identifier;

	mutex_unlock(&controller->mutex);

	/* Succeeded: renderer commands can import this resource in the destination namespace. */
	return 0;
}

/* Separates one session resource into an independent anchor and an ordinary alias. */
static int
share_promote(
	struct venus_controller *controller,
	struct venus_resource *resource,
	const struct gpu_image_descriptor *image)
{
	struct venus_resource **link;
	struct venus_resource *storage;
	struct venus_share *share;
	struct venus_shared_context *member;

	/* Only a currently controller-owned original allocation can be promoted. */
	link = &controller->resources;
	while (*link != NULL && *link != resource)
		link = &(*link)->next;

	/* Missing ownership must not create an untracked host resource. */
	if (*link == NULL)
		return EINVAL;

	/* Allocate every lifetime record before changing the existing resource. */
	share = kern_calloc(1U, sizeof(*share));
	if (share == NULL)
		return ENOMEM;

	/* The anchor retains host resource, mapping and aperture ownership after source close. */
	storage = kern_calloc(1U, sizeof(*storage));
	if (storage == NULL) {
		kern_free(share);
		return ENOMEM;
	}

	/* The existing owner contributes one already-established host context membership. */
	member = kern_calloc(1U, sizeof(*member));
	if (member == NULL) {
		kern_free(storage);
		kern_free(share);
		return ENOMEM;
	}

	/* The anchor has no context attachment; the membership list owns every attachment instead. */
	*storage = *resource;
	storage->context = 0U;
	storage->attached = 0U;
	storage->share = NULL;

	/* The source alias is the first allocation owner until the export adds its independent hold. */
	member->context = resource->context;
	member->references = 1U;

	/* The lifetime record owns storage and the first session membership without requiring an image. */
	share->storage = storage;
	share->contexts = member;
	share->references = 1U;

	/* Only an image export establishes immutable scanout metadata. */
	if (image != NULL)
		share->image = *image;

	/* Atomically replace controller ownership while preserving the core's original alias address. */
	*link = storage;
	resource->next = NULL;
	resource->share = share;

	/* Succeeded: no source-session lifetime owns the host allocation exclusively. */
	return 0;
}

/* Adds one alias to a context, attaching the host resource only on the first import. */
static int
share_context_add(
	struct venus_controller *controller,
	struct venus_share *share,
	uint32_t context)
{
	struct venus_shared_context *member;
	int error;

	/* Reuse an acknowledged membership without repeating the renderer attachment. */
	member = share->contexts;
	while (member != NULL) {
		/* Multiple aliases in one destination must not detach each other prematurely. */
		if (member->context == context) {
			/* Exhaustion must not wrap a live context into a detachable membership. */
			if (member->references == UINT_MAX)
				return EOVERFLOW;

			/* One more alias prevents native detach until that alias is destroyed. */
			member->references++;

			/* Succeeded: this alias reuses an acknowledged context attachment. */
			return 0;
		}

		/* Every membership is scoped to its actual renderer context identifier. */
		member = member->next;
	}

	/* Reserve membership metadata before the host can acquire its own reference. */
	member = kern_calloc(1U, sizeof(*member));
	if (member == NULL)
		return ENOMEM;

	/* Acknowledged attach enables VkImportMemoryResourceInfoMESA in this context. */
	error = drv_venus_resource_request_locked(controller, 0x0202U, context, share->storage->identifier);
	if (error != 0) {
		drv_venus_transport_fail(&controller->transport, error);
		kern_free(member);
		return error;
	}

	/* One alias now retains this context's attachment independently of the producer. */
	member->context = context;
	member->references = 1U;
	member->next = share->contexts;
	share->contexts = member;

	/* Succeeded: the same allocation is visible in the destination renderer context. */
	return 0;
}

/* Detaches the final alias in one context without unreferencing shared host storage. */
static void
share_context_drop(
	struct venus_controller *controller,
	struct venus_share *share,
	uint32_t context)
{
	unsigned failed;
	struct venus_shared_context **link;
	struct venus_shared_context *member;
	int error;

	/* Find only the closing alias's actual context membership. */
	link = &share->contexts;
	while (*link != NULL && (*link)->context != context)
		link = &(*link)->next;

	/* An internal missing membership cannot justify global host storage release. */
	if (*link == NULL) {
		drv_venus_transport_fail(&controller->transport, EIO);
		return;
	}

	/* Remaining aliases still require the same native context attachment. */
	member = *link;
	member->references--;
	if (member->references != 0U)
		return;

	/* Unknown command completion leaves numeric hardware state quarantined for reset. */
	failed = atomic_raw_load_acquire(&controller->transport.failed);
	if (failed == 0U) {
		error = drv_venus_resource_request_locked(controller, 0x0203U, context, share->storage->identifier);
		if (error != 0)
			drv_venus_transport_fail(&controller->transport, error);
	}

	/* No live alias remains in this context, even when the failed transport needs reset. */
	*link = member->next;
	kern_free(member);

	/* Succeeded: the context no longer contributes local allocation ownership. */
	return;
}
