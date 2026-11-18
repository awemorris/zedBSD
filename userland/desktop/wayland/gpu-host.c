/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's side of the GPU buffers' protocol (WS131 p009,
 * libkeiland-backend/keiland-backend-gpu.h): the wire objects, the
 * buffers' images and the surfaces' acquire fences lent to the backend,
 * which owns the protocol itself.  A struct kl_backend_resource is a
 * struct kwl_object; nothing of the backend's records is read here.
 */

#include "kwl.h"

#include <errno.h>

static struct kl_backend_resource *host_create(struct kl_backend_resource *parent, uint32_t id, unsigned role, uint32_t version);
static struct kl_backend_resource *host_create_server(struct kl_backend_resource *parent, unsigned role, uint32_t version);
static struct kl_backend_resource *host_find(struct kl_backend_resource *any, uint32_t id, unsigned role);
static void host_destroy(struct kl_backend_resource *resource);
static unsigned host_role(const struct kl_backend_resource *resource);
static uint32_t host_id(const struct kl_backend_resource *resource);
static uint32_t host_version(const struct kl_backend_resource *resource);
static uint64_t host_client_number(const struct kl_backend_resource *resource);
static void **host_private(struct kl_backend_resource *resource);
static int host_emit(struct kl_backend_resource *resource, uint32_t opcode, const void *payload, size_t size);
static int host_post_error(struct kl_backend_resource *resource, uint32_t code, const char *reason);
static int host_take_fd(struct kl_backend_resource *resource);
static VkResult host_buffer_adopt(struct kl_backend_resource *buffer, VkImage image, VkDeviceMemory memory, uint32_t width, uint32_t height, VkFormat format);
static void host_buffer_set_alpha(struct kl_backend_resource *buffer, uint32_t alpha);
static int host_surface_fence(struct kl_backend_resource *surface, int fd, uint64_t generation);
static int host_surface_fence_full(const struct kl_backend_resource *surface);
static const struct kl_backend_gpu_device *host_device(const struct kl_backend_resource *any);
static int host_log_frames(const struct kl_backend_resource *any);
static enum kwl_kind host_kind_of(unsigned role);
static struct kwl_object *host_object(const struct kl_backend_resource *resource);

/*
 * The functions the backend reaches the compositor's objects through; one
 * table for the process's life.
 */
static const struct kl_backend_protocol_host gpu_host = {
	host_create,
	host_create_server,
	host_find,
	host_destroy,
	host_role,
	host_id,
	host_version,
	host_client_number,
	host_private,
	host_emit,
	host_post_error,
	host_take_fd,
	host_buffer_adopt,
	host_buffer_set_alpha,
	host_surface_fence,
	host_surface_fence_full,
	host_device,
	host_log_frames
};

/*
 * Gives the table the GPU buffers' calls take.
 */
const struct kl_backend_protocol_host *
kwl_gpu_host(
	void)
{
	/* The one table. */
	return &gpu_host;
}

/*
 * Gives an object as the backend sees it.
 */
struct kl_backend_resource *
kwl_gpu_resource(
	struct kwl_object *object)
{
	/* The same object behind an opaque name. */
	return (struct kl_backend_resource *)object;
}

/* Gives the object behind a resource. */
static struct kwl_object *
host_object(
	const struct kl_backend_resource *resource)
{
	/* The resource is the object (kwl_gpu_resource). */
	return (struct kwl_object *)resource;
}

/* Gives the compositor's kind of a role. */
static enum kwl_kind
host_kind_of(
	unsigned role)
{
	/* Each role the backend makes or looks for. */
	switch (role) {
	case KL_BACKEND_ROLE_FACTORY:
		return KWL_FACTORY;
	case KL_BACKEND_ROLE_GPU_OBJECT:
		return KWL_GPU_OBJECT;
	case KL_BACKEND_ROLE_BUFFER:
		return KWL_BUFFER;
	case KL_BACKEND_ROLE_SURFACE:
		return KWL_SURFACE;
	default:
		break;
	}

	/* No kind the backend may make. */
	return KWL_DISPLAY;
}

/* Makes a client's new object of a role (kwl_create). */
static struct kl_backend_resource *
host_create(
	struct kl_backend_resource *parent,
	uint32_t id,
	unsigned role,
	uint32_t version)
{
	struct kwl_object *object;

	/* The object of the parent's client. */
	object = kwl_create(host_object(parent)->client, id, host_kind_of(role), version);

	/* The new object, or NULL for an id the client may not use. */
	return kwl_gpu_resource(object);
}

/* Makes an object the server numbers (kwl_create_server). */
static struct kl_backend_resource *
host_create_server(
	struct kl_backend_resource *parent,
	unsigned role,
	uint32_t version)
{
	struct kwl_object *object;

	/* The object of the parent's client. */
	object = kwl_create_server(host_object(parent)->client, host_kind_of(role), version);

	/* The new object, or NULL. */
	return kwl_gpu_resource(object);
}

/* Finds a client's live object of a role; a shared-memory buffer is not a GPU buffer. */
static struct kl_backend_resource *
host_find(
	struct kl_backend_resource *any,
	uint32_t id,
	unsigned role)
{
	struct kwl_object *object;
	enum kwl_kind kind;

	/* The client's object of that id. */
	object = kwl_find(host_object(any)->client, id);
	if (object == NULL)
		return NULL;

	/* Another kind is not the one asked for. */
	kind = host_kind_of(role);
	if (object->kind != kind)
		return NULL;

	/* A buffer of shared memory is not a GPU buffer. */
	if (role == KL_BACKEND_ROLE_BUFFER && object->shm != NULL)
		return NULL;

	/* Succeeded: the object. */
	return kwl_gpu_resource(object);
}

/* Destroys an object (kwl_object_destroy). */
static void
host_destroy(
	struct kl_backend_resource *resource)
{
	/* The object goes (a buffer stays until the frames that hold it are done). */
	kwl_object_destroy(host_object(resource));
}

/* Gives an object's role. */
static unsigned
host_role(
	const struct kl_backend_resource *resource)
{
	/* Each kind the backend knows. */
	switch (host_object(resource)->kind) {
	case KWL_FACTORY:
		return KL_BACKEND_ROLE_FACTORY;
	case KWL_GPU_OBJECT:
		return KL_BACKEND_ROLE_GPU_OBJECT;
	case KWL_BUFFER:
		return KL_BACKEND_ROLE_BUFFER;
	case KWL_SURFACE:
		return KL_BACKEND_ROLE_SURFACE;
	default:
		break;
	}

	/* Any other object. */
	return KL_BACKEND_ROLE_OTHER;
}

/* Gives an object's wire id. */
static uint32_t
host_id(
	const struct kl_backend_resource *resource)
{
	/* The id. */
	return host_object(resource)->id;
}

/* Gives an object's version. */
static uint32_t
host_version(
	const struct kl_backend_resource *resource)
{
	/* The version. */
	return host_object(resource)->version;
}

/* Gives the number of an object's client (for the log lines). */
static uint64_t
host_client_number(
	const struct kl_backend_resource *resource)
{
	/* The client's number. */
	return host_object(resource)->client->number;
}

/* Gives the backend's record slot of an object. */
static void **
host_private(
	struct kl_backend_resource *resource)
{
	/* The slot, NULL until the backend sets it. */
	return &host_object(resource)->gpu_private;
}

/* Sends an event of an object (kwl_emit). */
static int
host_emit(
	struct kl_backend_resource *resource,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	struct kwl_object *object;
	int error;

	/* The event, queued for the client. */
	object = host_object(resource);
	error = kwl_emit(object->client, object->id, opcode, payload, size);
	if (error != 0)
		return error;

	/* Succeeded: the event is queued. */
	return 0;
}

/* Sends a protocol error that ends the client (kwl_error_code, or kwl_error for the generic code). */
static int
host_post_error(
	struct kl_backend_resource *resource,
	uint32_t code,
	const char *reason)
{
	struct kwl_object *object;
	int error;

	/* The generic invalid request, or the interface's own code. */
	object = host_object(resource);
	if (code == KL_BACKEND_ERROR_INVALID) {
		error = kwl_error(object->client, object->id, reason);
	} else {
		error = kwl_error_code(object->client, object->id, code, reason);
	}

	/* The error the dispatcher returns. */
	return error;
}

/* Takes the next fd the client sent (kwl_take_fd). */
static int
host_take_fd(
	struct kl_backend_resource *resource)
{
	int descriptor;

	/* The fd, or -1 when it has not arrived. */
	descriptor = kwl_take_fd(host_object(resource)->client);
	return descriptor;
}

/* Gives a buffer its imported image (kwl_import_adopt). */
static VkResult
host_buffer_adopt(
	struct kl_backend_resource *buffer,
	VkImage image,
	VkDeviceMemory memory,
	uint32_t width,
	uint32_t height,
	VkFormat format)
{
	VkResult status;

	/* The view, the descriptor sets and the buffer's import record; the image and memory go on failure. */
	status = kwl_import_adopt(host_object(buffer), image, memory, width, height, format);
	return status;
}

/* Sets a buffer's blending, and has the next frame drawn. */
static void
host_buffer_set_alpha(
	struct kl_backend_resource *buffer,
	uint32_t alpha)
{
	struct kwl_object *object;

	/* The window's drawing blends the buffer by its alpha, or covers what is under it (import.c). */
	object = host_object(buffer);
	kwl_import_set_alpha(object, alpha);

	/* The next frame shows it. */
	object->client->server->dirty = 1;
}

/* Joins a fence to a surface's next commit; ENOSPC when the surface has as many as it takes. */
static int
host_surface_fence(
	struct kl_backend_resource *surface,
	int fd,
	uint64_t generation)
{
	struct kwl_object *object;

	/* A full list keeps the fd the caller's. */
	object = host_object(surface);
	if (object->acquire_count >= KWL_FENCE_MAX)
		return ENOSPC;

	/* The fence is the surface's; the common commit waits for it and closes it. */
	object->acquire[object->acquire_count].fd = fd;
	object->acquire[object->acquire_count].generation = generation;
	object->acquire_count++;

	/* Succeeded: the surface owns the fence. */
	return 0;
}

/* Tells whether a surface takes no more fences. */
static int
host_surface_fence_full(
	const struct kl_backend_resource *surface)
{
	struct kwl_object *object;

	/* As many as a commit takes. */
	object = host_object(surface);
	if (object->acquire_count >= KWL_FENCE_MAX)
		return 1;

	/* Room for another. */
	return 0;
}

/* Gives the compositor's Vulkan device, NULL before it is made. */
static const struct kl_backend_gpu_device *
host_device(
	const struct kl_backend_resource *any)
{
	struct kwl_server *server;

	/* Before the device is made there is nothing to import into. */
	server = host_object(any)->client->server;
	if (server->compose == NULL)
		return NULL;

	/* The device and its limits (compose.c fills them). */
	return &server->gpu_device;
}

/* Tells whether the per-frame log lines were asked for. */
static int
host_log_frames(
	const struct kl_backend_resource *any)
{
	struct kwl_object *object;

	/* --log-frames. */
	object = host_object(any);
	if (object->client->server->log_frames)
		return 1;

	/* Not asked for. */
	return 0;
}
