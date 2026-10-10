/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Protocol-object ownership and deferred destruction of imported image resources.
 */

#include "desktop.h"
#include "kwl.h"
#include "compose.h"
#include "menu.h"
#include "titlebar.h"
#include "popup.h"
#include "toplevel.h"
#include "subsurface.h"
#include "data.h"
#include "extras.h"
#include "panels.h"
#include "inset.h"
#include "edit.h"
#include "tablet.h"
#include "ime.h"
#include "touch.h"
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * When a gone client's buffer may be released (BUG-239, T1-392): one
 * release (a Venus round trip, up to a second in QEMU) holds the event
 * loop, so it waits until the user has not touched anything for
 * RETIRE_INPUT_QUIET_MS and nothing is to be drawn; a list that waited
 * RETIRE_STARVE_MS since its last release goes on anyway, so a screen that
 * never stops drawing still drains it.
 */
#define RETIRE_INPUT_QUIET_MS	200U
#define RETIRE_STARVE_MS	1000U

/*
 * Where a client's teardown spends its time among its objects (BUG-239):
 * the microseconds in the Vulkan release of the imports and in the
 * backend's release of the buffers' descriptors, and how many imports
 * were released.  Zeroed by kwl_client_destroy and added to by
 * object_free; the compositor's one thread uses them.
 */
static uint64_t cleanup_import_us;
static uint64_t cleanup_backend_us;
static unsigned cleanup_imports;

/*
 * The server whose gone client kwl_client_destroy is taking apart, NULL
 * otherwise: while it is set, object_free queues a buffer with an import
 * on the server's retiring list instead of releasing it at once (BUG-239).
 */
static struct kwl_server *cleanup_retire_server;

static void object_free(struct kwl_object *object);
static void object_release(struct kwl_server *server, struct kwl_object *object);
static void object_retire(struct kwl_server *server, struct kwl_object *object);

/*
 * Finds a live identity only within its originating client namespace.
 */
struct kwl_object *
kwl_find(
	struct kwl_client *client,
	uint32_t id)
{
	struct kwl_object *object;

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
struct kwl_object *
kwl_create(
	struct kwl_client *client,
	uint32_t id,
	enum kwl_kind kind,
	uint32_t version)
{
	struct kwl_object *object;

	/* Zero and the server-created ID range cannot be chosen by a client request. */
	if (id == 0 || id >= 0xff000000U || client->object_count >= KWL_OBJECT_MAX)
		return NULL;

	/* Reusing a live identity is a protocol error even when the type would match. */
	object = kwl_find(client, id);
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
 * Makes an object the compositor creates for a client (a new_id in an
 * event), with the next free ID of the server's range.  NULL when the
 * client has too many objects or no memory is left.
 */
struct kwl_object *
kwl_create_server(
	struct kwl_client *client,
	enum kwl_kind kind,
	uint32_t version)
{
	struct kwl_object *object;
	struct kwl_object *used;
	uint32_t id;
	unsigned tries;

	/* A client at its bound gets no more. */
	if (client->object_count >= KWL_OBJECT_MAX)
		return NULL;

	/* The next ID of the server's range that is not in use (the range wraps within itself). */
	id = 0;
	for (tries = 0; tries <= KWL_OBJECT_MAX; tries++) {
		/* The candidate, from the start of the range again after its end. */
		if (client->server_id_next < KWL_SERVER_ID_FIRST || client->server_id_next == UINT32_MAX)
			client->server_id_next = KWL_SERVER_ID_FIRST;
		id = client->server_id_next;
		client->server_id_next++;

		/* A free one ends the search. */
		used = kwl_find(client, id);
		if (used == NULL)
			break;
		id = 0;
	}

	/* No free ID. */
	if (id == 0)
		return NULL;

	/* The object's storage. */
	object = calloc(1, sizeof(*object));
	if (object == NULL)
		return NULL;

	/* Succeeded: the client's list owns it, like one the client made. */
	object->client = client;
	object->id = id;
	object->kind = kind;
	object->version = version;
	object->next = client->objects;
	client->objects = object;
	client->object_count++;
	return object;
}

/*
 * Retains a buffer while pending state, committed content or scanout may use it.
 */
void
kwl_buffer_get(
	struct kwl_object *buffer)
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
kwl_buffer_size(
	const struct kwl_object *buffer,
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
	*width = buffer->import->width;
	*height = buffer->import->height;
}

/*
 * Drops a compositor use and reports release only after every use is finished.
 */
void
kwl_buffer_put(
	struct kwl_object *buffer)
{
	int error;

	/* Null surface state never contributed an ownership reference. */
	if (buffer == NULL)
		return;

	/* An unbalanced owner is an internal defect rather than a client-controlled refcount. */
	if (buffer->holds == 0) {
		printf("KWL FAILED site=buffer_put client=%llu buffer=%u\n", (unsigned long long)buffer->client->number, buffer->id);
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
			printf("KWL RELEASE client=%llu buffer=%u dead=%u\n", (unsigned long long)buffer->client->number, buffer->id, buffer->dead);

		/* Only a surviving protocol identity may tell its producer to reuse storage. */
		if (!buffer->dead && !buffer->client->fatal) {
			/* Queue reuse notification before a possible final wrapper retirement. */
			error = kwl_emit(buffer->client, buffer->id, 0, NULL, 0);
			if (error != 0) {
				buffer->client->fatal = 1;
				buffer->client->fatal_time = kwl_milliseconds();
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
kwl_callbacks_done(
	struct kwl_object **callbacks)
{
	struct kwl_object *callback;
	uint32_t milliseconds;
	int error;

	/* Completion time is independent from wl_buffer release and its storage lifetime. */
	milliseconds = (uint32_t)kwl_milliseconds();
	while (*callbacks != NULL) {
		callback = *callbacks;
		*callbacks = callback->callback_next;
		callback->callback_next = NULL;

		/* A disconnected client needs cleanup but has no event recipient. */
		if (!callback->client->fatal) {
			/* Queue this callback exactly once before withdrawing its object ID. */
			error = kwl_emit(callback->client, callback->id, 0, &milliseconds, sizeof(milliseconds));
			if (error != 0) {
				callback->client->fatal = 1;
				callback->client->fatal_time = kwl_milliseconds();
			}
		}

		/* One-shot callbacks retire only after their ordered done event was queued. */
		kwl_object_destroy(callback);
	}

	/* Succeeded: the completed callback list has no remaining owners. */
	return;
}

/*
 * Retires a protocol object and detaches everything that names it.
 */
void
kwl_object_destroy(
	struct kwl_object *object)
{
	struct kwl_server *server;
	unsigned index;

	/* Repeated cleanup of an already-dead buffer changes no ownership. */
	if (object == NULL || object->dead)
		return;

	/* Seat leave events must name the surface before its identity is retired; a resize of it ends. */
	if (object->kind == KWL_SURFACE) {
		kwl_seat_surface_gone(object);
		kwl_toplevel_surface_gone(object);
		kwl_tablet_object_gone(object);
		kwl_touch_object_gone(object);
	}

	/* Destroying a toplevel role also cancels its borrowed interactive operation. */
	if (object->kind == KWL_TOPLEVEL && object->surface != NULL)
		kwl_toplevel_surface_gone(object->surface);

	/* The popups stop naming this one: a positioner's rules go, a popup's grab ends, a parent's popups close (popup.c). */
	if (object->kind == KWL_SURFACE ||
	    object->kind == KWL_POSITIONER ||
	    object->kind == KWL_POPUP)
		kwl_popup_object_gone(object);

	/* A toplevel, a surface and their decorations, a pointer and its cursor-shape devices, a surface and its viewport part (ws035-p080). */
	if (object->kind == KWL_TOPLEVEL ||
	    object->kind == KWL_DECORATION ||
	    object->kind == KWL_KDE_DECORATION ||
	    object->kind == KWL_SURFACE)
		kwl_decoration_object_gone(object);
	if (object->kind == KWL_POINTER)
		kwl_cursor_shape_object_gone(object);
	if (object->kind == KWL_SURFACE || object->kind == KWL_VIEWPORT)
		kwl_viewport_object_gone(object);

	/* A surface and its content type (content-type.c), and the game mode that shows it (scanout.c). */
	if (object->kind == KWL_SURFACE || object->kind == KWL_CONTENT_TYPE)
		kwl_content_type_object_gone(object);
	if (object->kind == KWL_SURFACE)
		kwl_scanout_surface_gone(object->client->server, object);

	/* A surface and its glass panels (panels.c). */
	if (object->kind == KWL_SURFACE || object->kind == KWL_GLASS)
		kwl_panels_object_gone(object);

	/* A network object that asked for scans asks no longer (system.c, ws089-p021). */
	if (object->kind == KWL_SYSTEM_NETWORK)
		kwl_system_network_gone(object);

	/* A machine object's queries are not answered any more (machine-shell.c, ws188-p002). */
	if (object->kind == KWL_SYSTEM_MACHINE)
		kwl_machine_gone(object);

	/* A sound stream's backend side closes with it (audio-stream.c, WS191). */
	if (object->kind == KWL_AUDIO_STREAM)
		kwl_audio_object_gone(object);

	/* A Bluetooth object lets go of its watching and scanning (bluetooth-shell.c, ws143-p006). */
	if (object->kind == KWL_SYSTEM_BLUETOOTH)
		kwl_bluetooth_gone(object);

	/* A media object no longer owns queued CLI responses or watches. */
	if (object->kind == KWL_SYSTEM_MEDIA)
		kwl_media_library_gone(object);

	/* A phone object's hearing, what it was owed and its sync waiting go (phone-shell.c, ws197-p004a). */
	if (object->kind == KWL_SYSTEM_PHONE)
		kwl_phone_gone(object);

	/* A toplevel's keyboard insets name nothing (inset.c). */
	if (object->kind == KWL_TOPLEVEL)
		kwl_inset_object_gone(object);

	/* A toplevel's edit objects name nothing (edit.c). */
	if (object->kind == KWL_TOPLEVEL)
		kwl_edit_object_gone(object);

	/* A data source leaves the clipboard and its offers, and a drag loses what goes (data.c). */
	if (object->kind == KWL_DATA_SOURCE ||
	    object->kind == KWL_DATA_OFFER ||
	    object->kind == KWL_DATA_DEVICE ||
	    object->kind == KWL_SURFACE ||
	    object->kind == KWL_TITLEBAR)
		kwl_data_object_gone(object);

	/* The text input and input method objects stop being named (text-input.c, input-method.c). */
	if (object->kind >= KWL_TEXT_INPUT_MANAGER && object->kind <= KWL_IME_STATUS)
		kwl_ime_object_gone(object);

	/* A primary selection source leaves the selection and its offers (primary.c). */
	if (object->kind == KWL_PRIMARY_SOURCE)
		kwl_primary_object_gone(object);

	/* A sub-surface leaves its parent, a parent's sub-surfaces lose it (subsurface.c). */
	if (object->kind == KWL_SURFACE || object->kind == KWL_SUBSURFACE)
		kwl_subsurface_object_gone(object);

	/* The System Menu's objects stop naming this one, and a menu open on it closes. */
	if (object->kind == KWL_SURFACE ||
	    object->kind == KWL_TOPLEVEL ||
	    object->kind == KWL_TOPLEVEL_MENU ||
	    object->kind == KWL_CONTEXT_MENU ||
	    object->kind == KWL_MENU)
		kwl_menu_object_gone(object);

	/* A displays object's held result has nobody to answer (displays-shell.c, BUG-266). */
	if (object->kind == KWL_SYSTEM_DISPLAYS)
		kwl_displays_object_gone(object);

	/* The desktop surface's role ends with its surface or its object (desktop.c). */
	if (object->kind == KWL_SURFACE || object->kind == KWL_DESKTOP_SURFACE)
		kwl_desktop_object_gone(object);

	/* The Titlebar Presentation's objects stop naming this one (titlebar.c). */
	if (object->kind == KWL_SURFACE || object->kind == KWL_TOPLEVEL || object->kind == KWL_TITLEBAR)
		kwl_titlebar_object_gone(object);

	/*
	 * Destroyed IDs become reusable only through ordered delete_id
	 * notification; an ID of the server's range is not told (the client
	 * frees it when it destroys the object).
	 */
	server = object->client->server;
	object->dead = 1;
	if (object->id < KWL_SERVER_ID_FIRST)
		kwl_delete_id(object->client, object->id);

	/* A surface's shell objects stop referring to it before its storage goes. */
	if (object->kind == KWL_SURFACE) {
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
		if (server->click_docked == object)
			server->click_docked = NULL;
		kwl_title_tap_forget(&server->title_tap, object);
		if (server->anim == object)
			server->anim = NULL;
		if (server->wiseview_current == object)
			server->wiseview_current = NULL;
		if (server->wiseview_press == object)
			server->wiseview_press = NULL;

		/* A desktop's docked owner that goes is noted, so the docked mode ends there (shell.c, WS181). */
		kwl_glass_forget(server, object);

		/* Its fences are not waited for any more. */
		for (index = 0; index < object->acquire_count; index++)
			close(object->acquire[index].fd);
		for (index = 0; index < object->fence_count; index++)
			close(object->fences[index].fd);
		object->acquire_count = 0;
		object->fence_count = 0;

		/* Its wl_shm image goes; a cursor surface gives the arrow back. */
		kwl_shm_image_destroy(server, object);
		if (server->cursor_surface == object)
			kwl_cursor_default(server);

		/* Pending state and current surface content own independent image holds. */
		kwl_buffer_put(object->pending);
		kwl_buffer_put(object->queued);
		kwl_buffer_put(object->current);
		kwl_callbacks_done(&object->callbacks);
		kwl_callbacks_done(&object->committed_callbacks);
	}

	/* Removing a shell role makes the surviving surface available for orderly destruction. */
	if (object->kind == KWL_XDG_SURFACE && object->surface != NULL)
		object->surface->role = NULL;

	/* A toplevel is the sole child retained by its xdg_surface role. */
	if (object->kind == KWL_TOPLEVEL && object->role != NULL)
		object->role->top = NULL;

	/* Destroy does not withdraw an image currently borrowed by pending or scanout state. */
	if (object->kind == KWL_BUFFER && object->holds != 0)
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
kwl_client_destroy(
	struct kwl_client *client)
{
	struct kwl_server *server;
	struct kwl_client **link;
	struct kwl_object *object;
	struct kwl_object *surface;
	struct kwl_packet *packet;
	uint64_t started;
	uint64_t quiesced;
	uint64_t surfaces_done;
	uint64_t shell_done;
	uint64_t objects_done;
	unsigned long long number;
	unsigned index;

	/* A frame in flight may hold this client's buffers and callbacks; it finishes first (each step is timed, BUG-239). */
	server = client->server;
	started = kwl_milliseconds();
	cleanup_import_us = 0U;
	cleanup_backend_us = 0U;
	cleanup_imports = 0U;
	cleanup_retire_server = server;
	kwl_compose_quiesce(server);
	quiesced = kwl_milliseconds();

	/* The input method's connection lets go of what it held (input-method.c). */
	kwl_ime_client_gone(client);

	/* Its notifications stay, with nobody to tell (notify-shell.c, ws177-p005). */
	kwl_notify_client_gone(client);

	/* The cursor this client chose goes back to the arrow (BUG-118). */
	if (server->cursor_client == client)
		kwl_cursor_default(server);

	/* Fatal status suppresses events while destructors unwind dependent objects. */
	client->fatal = 1;
	printf("KWL CLEANUP client=%llu objects=%u unread_fds=%u\n", (unsigned long long)client->number, client->object_count, client->right_count);

	/* Surfaces own all callback lists and buffer-use holds, so retire them first. */
	while (1) {
		/* Find one remaining surface whose holds must unwind before its buffers. */
		surface = NULL;
		for (object = client->objects; object != NULL; object = object->next) {
			/* Other object kinds cannot own the connection's surface-use references. */
			if (object->kind == KWL_SURFACE && !object->dead) {
				surface = object;
				break;
			}
		}

		/* No remaining surface can retain a buffer or callback from this client. */
		if (surface == NULL)
			break;

		/* The surface destructor also withdraws any active physical scanout. */
		kwl_object_destroy(surface);
	}

	/* The surfaces are gone. */
	surfaces_done = kwl_milliseconds();

	/* Toplevel and popup backreferences must retire before their xdg_surface storage. */
	while (1) {
		/* Find the next child before allowing any shell parent to retire. */
		object = client->objects;
		while (object != NULL &&
		       object->kind != KWL_TOPLEVEL &&
		       object->kind != KWL_POPUP)
			object = object->next;

		/* All remaining shell parents can retire once no child points at them. */
		if (object == NULL)
			break;

		/* The parent role remains allocated until this child is gone. */
		kwl_object_destroy(object);
	}

	/* The shell's children are gone. */
	shell_done = kwl_milliseconds();

	/* Unused imports and protocol globals have no remaining dependent owners. */
	while (client->objects != NULL) {
		/* A dead wrapper needs final free; a live identity needs protocol retirement first. */
		object = client->objects;
		if (object->dead)
			object_free(object);
		else
			kwl_object_destroy(object);
	}

	/* Every object is gone (its imported buffers wait in the retiring list). */
	objects_done = kwl_milliseconds();
	number = (unsigned long long)client->number;
	cleanup_retire_server = NULL;

	/* Rights never consumed by a valid request still belong to this connection. */
	for (index = 0; index < client->right_count; index++)
		close(client->rights[index]);

	/* Unsent events cannot extend GPU resource ownership; their descriptors close here. */
	while (client->output_head != NULL) {
		packet = client->output_head;
		client->output_head = packet->next;
		kwl_packet_free(packet);
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

	/* Where the time went (BUG-239: 20 clients took 23 s to release in QEMU). */
	printf("KWL CLEANUP done client=%llu ms=%llu quiesce=%llu surfaces=%llu shell=%llu objects=%llu queued=%u import_us=%llu backend_us=%llu\n",
	       number,
	       (unsigned long long)(kwl_milliseconds() - started),
	       (unsigned long long)(quiesced - started),
	       (unsigned long long)(surfaces_done - quiesced),
	       (unsigned long long)(shell_done - surfaces_done),
	       (unsigned long long)(objects_done - shell_done),
	       cleanup_imports,
	       (unsigned long long)cleanup_import_us,
	       (unsigned long long)cleanup_backend_us);

	/* Succeeded: the connection namespace, events and received descriptors are retired. */
	return;
}

/*
 * Releases the oldest gone client's buffer waiting in the retiring list
 * (one each pass of the event loop, after its frame, BUG-239).  Returns
 * nonzero while more wait.
 */
int
kwl_retire_tick(
	struct kwl_server *server)
{
	struct kwl_object *object;
	uint64_t started;
	uint64_t ended;

	/* Nothing waits. */
	object = server->retiring;
	if (object == NULL)
		return 0;

	/* The oldest leaves the list. */
	server->retiring = object->retire_next;
	if (server->retiring == NULL)
		server->retiring_tail = NULL;
	server->retiring_count--;

	/* Its release, timed. */
	cleanup_import_us = 0U;
	cleanup_backend_us = 0U;
	started = kwl_milliseconds();
	object_release(server, object);
	ended = kwl_milliseconds();
	server->retire_released++;

	/* The next release leaves as long as this one took to the frames and the input. */
	server->retire_last_ms = ended;
	server->retire_next_ms = ended + (ended - started);

	/* A slow one is logged with where its time went. */
	if (cleanup_import_us + cleanup_backend_us >= 100000U)
		printf("KWL RETIRE slow import_us=%llu backend_us=%llu\n", (unsigned long long)cleanup_import_us, (unsigned long long)cleanup_backend_us);

	/* More wait. */
	if (server->retiring != NULL)
		return 1;

	/* The list emptied: how many and how long since the first was queued. */
	printf("KWL RETIRE drained released=%u ms=%llu\n", server->retire_released, (unsigned long long)(kwl_milliseconds() - server->retire_started_ms));
	server->retire_started_ms = 0U;
	return 0;
}

/*
 * Tells whether a gone client's buffer may be released in this pass of the
 * event loop (BUG-239, T1-392): one waits, no frame is in flight (a
 * release then would delay its completion, and under Venus wait behind
 * it), the last release's own time has passed, and either the user has
 * been still for RETIRE_INPUT_QUIET_MS with nothing to draw, or the list
 * has waited RETIRE_STARVE_MS since its last release.
 */
int
kwl_retire_ready(
	struct kwl_server *server,
	uint64_t now)
{
	uint64_t since;

	/* Nothing waits. */
	if (server->retiring == NULL)
		return 0;

	/* A frame in flight finishes first. */
	if (server->compose != NULL && server->compose->in_flight)
		return 0;

	/* The last release's time goes to the frames and the input. */
	if (now < server->retire_next_ms)
		return 0;

	/* A list kept waiting too long goes on, whatever the input and the drawing. */
	since = server->retire_last_ms;
	if (since == 0U)
		since = server->retire_started_ms;
	if (now >= since + RETIRE_STARVE_MS)
		return 1;

	/* The user touched something a moment ago: the input comes first. */
	if (now < server->lock_input_ms + RETIRE_INPUT_QUIET_MS)
		return 0;

	/* Something is to be drawn: the frame comes first. */
	if (server->dirty != 0U)
		return 0;

	/* Succeeded: the compositor is idle. */
	return 1;
}

/* Releases every buffer still waiting in the retiring list (before the Vulkan device goes). */
void
kwl_retire_flush(
	struct kwl_server *server)
{
	int more;

	/* Each in turn until the list is empty. */
	more = kwl_retire_tick(server);
	while (more)
		more = kwl_retire_tick(server);
}

/*
 * Withdraws an unborrowed object from its client's list and releases it
 * with its Vulkan import; a gone client's buffer with an import waits in
 * the server's retiring list instead (BUG-239).
 */
static void
object_free(
	struct kwl_object *object)
{
	struct kwl_object **link;
	struct kwl_client *client;
	struct kwl_server *server;

	/* Retired IDs can coexist with a newly created object of the same numeric ID. */
	client = object->client;
	server = client->server;
	link = &client->objects;
	while (*link != NULL && *link != object)
		link = &(*link)->next;

	/* Every object contributes to the connection's resource bound until it leaves the list. */
	if (*link == object) {
		*link = object->next;
		client->object_count--;
	}

	/* A gone client's imported buffer is released later, a little at a time. */
	if (cleanup_retire_server != NULL && object->import != NULL) {
		cleanup_imports++;
		object_retire(server, object);
		return;
	}

	/* Succeeded: released now. */
	object_release(server, object);
}

/*
 * Releases an object out of every list: its Vulkan import, then its OS
 * buffer descriptors, then its share of a pool, then the wrapper (timed
 * for the cleanup's log, BUG-239).
 */
static void
object_release(
	struct kwl_server *server,
	struct kwl_object *object)
{
	uint64_t before;
	uint64_t between;

	/* Window mode's Vulkan image goes with the buffer. */
	before = kwl_microseconds();
	kwl_import_destroy_with(server->compose, object);
	between = kwl_microseconds();
	cleanup_import_us += between - before;

	/* OS buffer descriptors (libkeiland-backend's) remain alive until the final Vulkan image use has retired. */
	kl_backend_gpu_resource_free(kwl_gpu_host(), kwl_gpu_resource(object));
	cleanup_backend_us += kwl_microseconds() - between;

	/* Shared-memory buffer storage returns its separate pool reference. */
	if (object->shm != NULL) {
		kwl_pool_put(object->shm->pool);
		free(object->shm);
		object->shm = NULL;
	}

	/* A pool object drops its own reference. */
	if (object->pool != NULL) {
		kwl_pool_put(object->pool);
		object->pool = NULL;
	}

	/* No remaining owner may refer to this wrapper. */
	free(object);
}

/*
 * Queues a gone client's buffer at the end of the server's retiring list;
 * it no longer belongs to the client, whose record goes now.
 */
static void
object_retire(
	struct kwl_server *server,
	struct kwl_object *object)
{
	/* No client: the release goes through the server. */
	object->client = NULL;
	object->retire_next = NULL;

	/* At the end, oldest first. */
	if (server->retiring_tail != NULL) {
		server->retiring_tail->retire_next = object;
	} else {
		server->retiring = object;
		server->retire_started_ms = kwl_milliseconds();
		server->retire_released = 0U;
		server->retire_last_ms = 0U;
		server->retire_next_ms = 0U;
	}

	/* The new end, counted. */
	server->retiring_tail = object;
	server->retiring_count++;
}

