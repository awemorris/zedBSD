/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Protocol-object ownership and deferred destruction of imported image resources.
 */

#include "zwl.h"
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void object_free(struct zwl_object *object);

/*
 * Finds a live identity only within its originating client namespace.
 */
struct zwl_object *
zwl_find(
	struct zwl_client *client,
	uint32_t id)
{
	struct zwl_object *object;

	/* Dead buffer wrappers retain GPU ownership but no longer own protocol IDs. */
	for (object = client->objects; object != NULL; object = object->next) {
		/* Numeric ID reuse must not revive a retired buffer wrapper. */
		if (object->id == id && !object->dead)
			return object;
	}

	/* No live object owns this client-local identity. */
	return NULL;
}

/*
 * Creates one checked client-side protocol identity with no implicit GPU allocation.
 */
struct zwl_object *
zwl_create(
	struct zwl_client *client,
	uint32_t id,
	enum zwl_kind kind,
	uint32_t version)
{
	struct zwl_object *object;

	/* Zero and the server-created ID range cannot be chosen by a client request. */
	if (id == 0 || id >= 0xff000000U || client->object_count >= ZWL_OBJECT_MAX)
		return NULL;

	/* Reusing a live identity is a protocol error even when the type would match. */
	object = zwl_find(client, id);
	if (object != NULL)
		return NULL;

	/* Object storage is independent from the allocation carried by a wl_buffer. */
	object = calloc(1, sizeof(*object));
	if (object == NULL)
		return NULL;

	/* Client-list ownership lasts until normal retirement or disconnect cleanup. */
	object->client = client;
	object->id = id;
	object->kind = kind;
	object->version = version;
	object->next = client->objects;
	client->objects = object;
	client->object_count++;

	/* Succeeded: this client owns the new protocol identity. */
	return object;
}

/*
 * Retains a buffer while pending state, committed content or scanout may use it.
 */
void
zwl_buffer_get(
	struct zwl_object *buffer)
{
	/* A null attach denotes an unmapped surface and retains no allocation. */
	if (buffer == NULL)
		return;

	/* Every internal owner balances its own hold independently of protocol destroy. */
	buffer->holds++;

	/* Succeeded: one additional compositor use retains this image. */
	return;
}

/*
 * Reports a buffer's size: a wl_shm buffer's, or a GPU image's.
 */
void
zwl_buffer_size(
	const struct zwl_object *buffer,
	uint32_t *width,
	uint32_t *height)
{
	/* A wl_shm buffer knows its size in its pool. */
	if (buffer->shm != NULL) {
		*width = buffer->shm->width;
		*height = buffer->shm->height;
		return;
	}

	/* A GPU buffer's size is its image's. */
	*width = buffer->image.image.width;
	*height = buffer->image.image.height;
}

/*
 * Drops a compositor use and reports release only after every use is finished.
 */
void
zwl_buffer_put(
	struct zwl_object *buffer)
{
	int error;

	/* Null surface state never contributed an ownership reference. */
	if (buffer == NULL)
		return;

	/* An unbalanced owner is an internal defect rather than a client-controlled refcount. */
	if (buffer->holds == 0) {
		printf("ZWL FAILED site=buffer_put client=%llu buffer=%u\n", (unsigned long long)buffer->client->number, buffer->id);
		buffer->client->server->failed = 1;
		return;
	}

	/* Another pending, current or front owner still prevents reuse by the producer. */
	buffer->holds--;
	if (buffer->holds != 0)
		return;

	/* Only committed buffers receive release, and destroyed identities receive no event. */
	if (buffer->busy) {
		buffer->busy = 0;

		/* Names the release when the per-frame lines were asked for. */
		if (buffer->client->server->log_frames)
			printf("ZWL RELEASE client=%llu buffer=%u resource=%u dead=%u\n", (unsigned long long)buffer->client->number, buffer->id, buffer->image.resource_id, buffer->dead);

		/* Only a surviving protocol identity may tell its producer to reuse storage. */
		if (!buffer->dead && !buffer->client->fatal) {
			/* Queue reuse notification before a possible final wrapper retirement. */
			error = zwl_emit(buffer->client, buffer->id, 0, NULL, 0);
			if (error != 0) {
				buffer->client->fatal = 1;
				buffer->client->fatal_time = zwl_milliseconds();
			}
		}
	}

	/* A protocol-destroyed wrapper can now retire its independently imported resource. */
	if (buffer->dead)
		object_free(buffer);

	/* Succeeded: this compositor use no longer retains the image. */
	return;
}

/*
 * Completes an ordered set of one-shot frame callbacks after presentation progress.
 */
void
zwl_callbacks_done(
	struct zwl_object **callbacks)
{
	struct zwl_object *callback;
	uint32_t milliseconds;
	int error;

	/* Completion time is independent from wl_buffer release and its storage lifetime. */
	milliseconds = (uint32_t)zwl_milliseconds();
	while (*callbacks != NULL) {
		callback = *callbacks;
		*callbacks = callback->callback_next;
		callback->callback_next = NULL;

		/* A disconnected client needs cleanup but has no event recipient. */
		if (!callback->client->fatal) {
			/* Queue this callback exactly once before withdrawing its object ID. */
			error = zwl_emit(callback->client, callback->id, 0, &milliseconds, sizeof(milliseconds));
			if (error != 0) {
				callback->client->fatal = 1;
				callback->client->fatal_time = zwl_milliseconds();
			}
		}

		/* One-shot callbacks retire only after their ordered done event was queued. */
		zwl_object_destroy(callback);
	}

	/* Succeeded: the completed callback list has no remaining owners. */
	return;
}

/*
 * Retires a protocol object while preserving any outstanding scanout ownership.
 */
void
zwl_object_destroy(
	struct zwl_object *object)
{
	struct zwl_server *server;
	unsigned index;
	int error;

	/* Repeated cleanup of an already-dead buffer changes no ownership. */
	if (object == NULL || object->dead)
		return;

	/* Seat leave events must name the surface before its identity is retired. */
	if (object->kind == ZWL_SURFACE)
		zwl_seat_surface_gone(object);

	/* Destroyed IDs become reusable only through ordered delete_id notification. */
	server = object->client->server;
	object->dead = 1;
	zwl_delete_id(object->client, object->id);

	/* A surface's scanout must be disabled before its imported buffers can retire. */
	if (object->kind == ZWL_SURFACE) {
		/* The owning front must stop scanout before this surface loses its references. */
		if (server->front_surface == object) {
			/* Native release establishes that current storage can retire safely. */
			error = zwl_unscan(server);
			if (error != 0) {
				printf("ZWL FAILED site=surface_unscan errno=%d\n", error);
				server->failed = 1;
			}
		}

		/* Detach surviving shell objects before this surface storage disappears. */
		if (object->role != NULL) {
			object->role->surface = NULL;

			/* A surviving toplevel cannot retain the soon-freed core surface either. */
			if (object->role->top != NULL)
				object->role->top->surface = NULL;
		}

		/* Window mode draws the output again without it, and without its wl_shm image or cursor. */
		server->dirty = 1;
		if (object->awaited) {
			object->awaited = 0;
			server->awaiting--;
		}

		/* A window being moved, pulled, clicked or animated is not any more. */
		if (server->drag == object)
			server->drag = NULL;
		if (server->pull == object)
			server->pull = NULL;
		if (server->click_surface == object)
			server->click_surface = NULL;
		if (server->anim == object)
			server->anim = NULL;
		if (server->wiseview_current == object)
			server->wiseview_current = NULL;
		if (server->wiseview_press == object)
			server->wiseview_press = NULL;

		/* Its fences are not waited for any more. */
		for (index = 0; index < object->acquire_count; index++)
			close(object->acquire[index].fd);
		for (index = 0; index < object->fence_count; index++)
			close(object->fences[index].fd);
		object->acquire_count = 0;
		object->fence_count = 0;

		/* Its wl_shm image goes; a cursor surface gives the arrow back. */
		zwl_shm_image_destroy(server, object);
		if (server->cursor_surface == object)
			zwl_cursor_default(server);

		/* Pending state and current surface content own independent image holds. */
		zwl_buffer_put(object->pending);
		zwl_buffer_put(object->queued);
		zwl_buffer_put(object->current);
		zwl_callbacks_done(&object->callbacks);
		zwl_callbacks_done(&object->committed_callbacks);
	}

	/* Removing a shell role makes the surviving surface available for orderly destruction. */
	if (object->kind == ZWL_XDG_SURFACE && object->surface != NULL)
		object->surface->role = NULL;

	/* A toplevel is the sole child retained by its xdg_surface role. */
	if (object->kind == ZWL_TOPLEVEL && object->role != NULL)
		object->role->top = NULL;

	/* Destroy does not withdraw an image currently borrowed by pending or scanout state. */
	if (object->kind == ZWL_BUFFER && object->holds != 0)
		return;

	/* Unborrowed objects need no deferred lifetime beyond their protocol ID. */
	object_free(object);

	/* Succeeded: the unborrowed protocol object is retired. */
	return;
}

/*
 * Releases every connection-owned descriptor, protocol object and unsent event.
 */
void
zwl_client_destroy(
	struct zwl_client *client)
{
	struct zwl_server *server;
	struct zwl_client **link;
	struct zwl_object *object;
	struct zwl_object *surface;
	struct zwl_packet *packet;
	unsigned index;

	/* A frame in flight may hold this client's buffers and callbacks; it finishes first. */
	server = client->server;
	zwl_compose_quiesce(server);

	/* Fatal status suppresses events while destructors unwind dependent objects. */
	client->fatal = 1;
	printf("ZWL CLEANUP client=%llu objects=%u unread_fds=%u\n", (unsigned long long)client->number, client->object_count, client->right_count);

	/* Surfaces own all callback lists and buffer-use holds, so retire them first. */
	while (1) {
		/* Find one remaining surface whose holds must unwind before its buffers. */
		surface = NULL;
		for (object = client->objects; object != NULL; object = object->next) {
			/* Other object kinds cannot own the connection's surface-use references. */
			if (object->kind == ZWL_SURFACE && !object->dead) {
				surface = object;
				break;
			}
		}

		/* No remaining surface can retain a buffer or callback from this client. */
		if (surface == NULL)
			break;

		/* The surface destructor also withdraws any active physical scanout. */
		zwl_object_destroy(surface);
	}

	/* Toplevel backreferences must retire before their xdg_surface storage. */
	while (1) {
		/* Find the next child before allowing any shell parent to retire. */
		object = client->objects;
		while (object != NULL && object->kind != ZWL_TOPLEVEL)
			object = object->next;

		/* All remaining shell parents can retire once no child points at them. */
		if (object == NULL)
			break;

		/* The parent role remains allocated until this child is gone. */
		zwl_object_destroy(object);
	}

	/* Unused imports and protocol globals have no remaining dependent owners. */
	while (client->objects != NULL) {
		/* A dead wrapper needs final free; a live identity needs protocol retirement first. */
		object = client->objects;
		if (object->dead)
			object_free(object);
		else
			zwl_object_destroy(object);
	}

	/* Rights never consumed by a valid request still belong to this connection. */
	for (index = 0; index < client->right_count; index++)
		close(client->rights[index]);

	/* Unsent events cannot extend GPU resource ownership; their descriptors close here. */
	while (client->output_head != NULL) {
		packet = client->output_head;
		client->output_head = packet->next;
		zwl_packet_free(packet);
	}

	/* Unlink the client before closing its descriptor so reuse cannot select this generation. */
	link = &server->clients;
	while (*link != NULL && *link != client)
		link = &(*link)->next;

	/* Only the current generation of this allocated connection is withdrawn. */
	if (*link == client)
		*link = client->next;

	/* The socket close also releases rights still unread in the kernel receive queue. */
	close(client->fd);
	free(client);

	/* Succeeded: the connection namespace, events and received descriptors are retired. */
	return;
}

/* Releases an unborrowed object and its independent GPU import, then withdraws list ownership. */
static void
object_free(
	struct zwl_object *object)
{
	struct zwl_object **link;
	struct gpu_resource_destroy request;
	struct zwl_client *client;
	int error;

	/* Window mode's Vulkan image goes with the buffer; a wl_shm buffer or pool drops its pool's memory. */
	client = object->client;
	zwl_import_destroy(object);
	if (object->shm != NULL) {
		zwl_pool_put(object->shm->pool);
		free(object->shm);
		object->shm = NULL;
	}

	/* A pool object drops its own reference. */
	if (object->pool != NULL) {
		zwl_pool_put(object->pool);
		object->pool = NULL;
	}

	/* Imported resource handles belong exclusively to the compositor's GPU open. */
	if (object->image.handle != 0 && client->server->gpu >= 0) {
		/* The import handle is local to this compositor open and no longer borrowed. */
		memset(&request, 0, sizeof(request));
		request.version = GPU_ABI_VERSION;
		request.size = sizeof(request);
		request.handle = object->image.handle;
		error = ioctl(client->server->gpu, GPU_RESOURCE_DESTROY, &request);
		if (error != 0) {
			printf("ZWL GPU_ERROR operation=destroy errno=%d\n", errno);
			client->server->failed = 1;
		}
	}

	/* Retired IDs can coexist with a newly created object of the same numeric ID. */
	link = &client->objects;
	while (*link != NULL && *link != object)
		link = &(*link)->next;

	/* Every object contributes to the connection's resource bound until final free. */
	if (*link == object) {
		*link = object->next;
		client->object_count--;
	}

	/* No remaining owner may refer to this wrapper. */
	free(object);

	/* Succeeded: the wrapper and its independent import ownership are retired. */
	return;
}
