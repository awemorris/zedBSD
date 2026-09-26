/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Selected core Wayland, xdg-shell and typed GPU-buffer request semantics.
 */

#include "zwl.h"
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The registry advertises only implemented interfaces and their actual versions. */
struct zwl_global {
	uint32_t name;
	const char *interface;
	uint32_t version;
	enum zwl_kind kind;
};

/* Stable global names are scoped to one compositor process generation. */
static const struct zwl_global globals[] = {
	{ 1, "wl_compositor", 4, ZWL_COMPOSITOR },
	{ 2, "xdg_wm_base", 1, ZWL_WM },
	{ 3, "zed_gpu_buffer_v1", 1, ZWL_FACTORY },
	{ 4, "wl_output", 2, ZWL_OUTPUT },
	{ 5, "wl_seat", 5, ZWL_SEAT },
	{ 6, "wl_shm", 1, ZWL_SHM },
};

static uint32_t word_at(const unsigned char *bytes, size_t offset);
static int string_at(const unsigned char *bytes, size_t size, size_t offset, const char **text, size_t *next);
static int registry_events(struct zwl_object *registry);
static int output_events(struct zwl_object *output);
static int bind_global(struct zwl_object *registry, const unsigned char *bytes, size_t size);
static int surface_request(struct zwl_object *surface, uint32_t opcode, const unsigned char *bytes, size_t size);
static int surface_commit(struct zwl_object *surface);
static int shell_request(struct zwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
static int factory_request(struct zwl_object *factory, uint32_t opcode, const unsigned char *bytes, size_t size);
static void append_callbacks(struct zwl_object **list, struct zwl_object *callbacks);
static void add_damage(struct zwl_object *surface, int32_t x, int32_t y, int32_t width, int32_t height);
static void commit_damage(struct zwl_object *surface);

/*
 * Dispatches one validated frame through its client-local interface identity.
 */
int
zwl_dispatch(
	struct zwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct zwl_object *object;
	struct zwl_object *created;
	uint32_t new_id;
	enum zwl_kind kind;
	int error;

	/* Unknown IDs cannot select another client's protocol objects or GPU resources. */
	object = zwl_find(client, id);
	if (object == NULL) {
		error = zwl_error(client, id, "unknown object");
		return error;
	}

	/* Every method validates its exact payload length and interface opcode. */
	error = EPROTO;
	switch (object->kind) {
	case ZWL_DISPLAY:
		/* Both supported display constructors contain exactly one new_id. */
		if (size != 4U || opcode > 1U)
			break;

		/* A sync callback is completed after preceding requests have been processed. */
		new_id = word_at(bytes, 0);
		kind = ZWL_CALLBACK;
		if (opcode == 1U)
			kind = ZWL_REGISTRY;

		/* Duplicate or forbidden IDs are rejected before emitting constructor events. */
		created = zwl_create(client, new_id, kind, 1);
		if (created == NULL)
			break;

		/* Registry discovery and one-shot sync use their canonical server events. */
		if (opcode == 1U)
			error = registry_events(created);
		else {
			zwl_callbacks_done(&created);
			error = 0;
		}
		break;
	case ZWL_REGISTRY:
		/* bind is the sole registry request. */
		if (opcode == 0)
			error = bind_global(object, bytes, size);
		break;
	case ZWL_COMPOSITOR:
		/* The compositor creates independent surfaces or region identities. */
		if (size != 4U || opcode > 1U)
			break;

		/* Regions need no retained geometry for this opaque fullscreen policy. */
		kind = ZWL_SURFACE;
		if (opcode == 1U)
			kind = ZWL_REGION;

		/* The child surface inherits the negotiated compositor version. */
		new_id = word_at(bytes, 0);
		created = zwl_create(client, new_id, kind, object->version);
		if (created != NULL)
			error = 0;
		break;
	case ZWL_SURFACE:
		error = surface_request(object, opcode, bytes, size);
		break;
	case ZWL_REGION:
		/* Region destruction retires only its protocol identity. */
		if (opcode == 0 && size == 0) {
			zwl_object_destroy(object);
			error = 0;
		} else if ((opcode == 1U || opcode == 2U) && size == 16U) {
			/* Damage and input regions do not alter the single opaque scanout plane. */
			error = 0;
		}
		break;
	case ZWL_BUFFER:
		/* Buffer destruction keeps any active GPU use alive independently. */
		if (opcode == 0 && size == 0) {
			zwl_object_destroy(object);
			error = 0;
		}
		break;
	case ZWL_FACTORY:
		error = factory_request(object, opcode, bytes, size);
		break;
	case ZWL_SHM:
	case ZWL_SHM_POOL:
		error = zwl_shm_request(object, opcode, bytes, size);
		break;
	case ZWL_WM:
	case ZWL_XDG_SURFACE:
	case ZWL_TOPLEVEL:
		error = shell_request(object, opcode, bytes, size);
		break;
	case ZWL_SEAT:
	case ZWL_POINTER:
	case ZWL_KEYBOARD:
		error = zwl_seat_request(object, opcode, bytes, size);
		break;
	default:
		/* Callback objects and version-2 outputs have no client requests. */
		break;
	}

	/* Ancillary delivery may trail a complete frame; retain it without consuming state. */
	if (error == EAGAIN)
		return EAGAIN;

	/* Preserve a specific protocol error already emitted by a delegated handler. */
	if (error != 0) {
		/* A delegated terminal error owns the connection's sole final error event. */
		if (!client->fatal)
			error = zwl_error(client, id, "invalid or unsupported request");

		/* The final error remains queued for bounded flush before disconnect. */
		return error;
	}

	/* Succeeded: the complete request has been processed exactly once. */
	return 0;
}

/* Reads one possibly unaligned native-endian protocol word. */
static uint32_t
word_at(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* Callers validate the containing payload before requesting a word. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: return the decoded scalar without pointer-alignment assumptions. */
	return word;
}

/* Validates one non-null string and returns the following aligned argument position. */
static int
string_at(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	const char **text,
	size_t *next)
{
	uint32_t length;
	size_t aligned;
	size_t actual;

	/* The string length word must fit before its payload can be inspected. */
	if (offset > size || size - offset < 4U)
		return EPROTO;

	/* Non-null strings contain one terminator within the declared byte count. */
	length = word_at(bytes, offset);
	if (length == 0 || length > size - offset - 4U)
		return EPROTO;

	/* Aligned storage must also fit even when the actual text length is unaligned. */
	aligned = ((size_t)length + 3U) & ~(size_t)3U;
	if (aligned > size - offset - 4U)
		return EPROTO;

	/* Embedded NUL bytes may not conceal trailing non-string arguments. */
	*text = (const char *)bytes + offset + 4U;
	if ((*text)[length - 1U] != '\0')
		return EPROTO;

	/* The declared byte count must describe this exact terminated string. */
	actual = strlen(*text);
	if (actual + 1U != length)
		return EPROTO;

	/* Succeeded: the next argument follows the string's padded storage. */
	*next = offset + 4U + aligned;
	return 0;
}

/* Announces only the selected protocol globals in stable registry order. */
static int
registry_events(
	struct zwl_object *registry)
{
	unsigned char payload[128];
	uint32_t word;
	size_t index;
	size_t length;
	size_t offset;
	int error;

	/* Each global event carries name, interface string and supported version. */
	for (index = 0; index < sizeof(globals) / sizeof(globals[0]); index++) {
		/* Encode this advertised interface as one canonical registry global event. */
		memset(payload, 0, sizeof(payload));
		word = globals[index].name;
		memcpy(payload, &word, 4);
		length = strlen(globals[index].interface) + 1U;
		word = (uint32_t)length;
		memcpy(payload + 4, &word, 4);
		memcpy(payload + 8, globals[index].interface, length);
		offset = 8U + ((length + 3U) & ~(size_t)3U);
		word = globals[index].version;
		memcpy(payload + offset, &word, 4);
		error = zwl_emit(registry->client, registry->id, 0, payload, offset + 4U);
		if (error != 0)
			return error;
	}

	/* Succeeded: discovery events precede any following display sync callback. */
	return 0;
}

/* Supplies the version-2 output geometry, current mode, scale and completion event. */
static int
output_events(
	struct zwl_object *output)
{
	unsigned char geometry[60];
	uint32_t words[4];
	uint32_t word;
	int error;

	/* Geometry includes unknown physical dimensions and fixed make/model strings. */
	memset(geometry, 0, sizeof(geometry));
	word = 4;
	memcpy(geometry + 20, &word, 4);
	memcpy(geometry + 24, "zed", 4);
	word = 11;
	memcpy(geometry + 28, &word, 4);
	memcpy(geometry + 32, "fullscreen", 11);
	error = zwl_emit(output->client, output->id, 0, geometry, 48);
	if (error != 0)
		return error;

	/* This single mode is current and preferred in the selected fullscreen policy. */
	words[0] = 3;
	words[1] = output->client->server->width;
	words[2] = output->client->server->height;
	words[3] = output->client->server->refresh;
	error = zwl_emit(output->client, output->id, 1, words, sizeof(words));
	if (error != 0)
		return error;

	/* Scale and done were introduced in output version 2. */
	if (output->version >= 2U) {
		/* This output uses one buffer pixel per surface coordinate. */
		word = 1;
		error = zwl_emit(output->client, output->id, 3, &word, sizeof(word));
		if (error != 0)
			return error;

		/* The done event commits all preceding output properties. */
		error = zwl_emit(output->client, output->id, 2, NULL, 0);
		if (error != 0)
			return error;
	}

	/* Succeeded: output properties are complete for the negotiated version. */
	return 0;
}

/* Resolves a registry name, exact interface string and supported requested version. */
static int
bind_global(
	struct zwl_object *registry,
	const unsigned char *bytes,
	size_t size)
{
	const char *interface;
	struct zwl_object *object;
	uint32_t name;
	uint32_t version;
	uint32_t id;
	size_t offset;
	size_t index;
	int error;
	int same;

	/* The dynamic bind signature contains a name followed by string/version/new_id. */
	if (size < 16U)
		return EPROTO;

	/* Validate the variable-width string before reading its following scalar arguments. */
	name = word_at(bytes, 0);
	error = string_at(bytes, size, 4, &interface, &offset);
	if (error != 0)
		return error;

	/* No extra bytes may trail the constructor's version and new identity. */
	if (offset + 8U != size)
		return EPROTO;

	/* A global cannot be bound under an unrelated interface or newer version. */
	version = word_at(bytes, offset);
	id = word_at(bytes, offset + 4U);
	for (index = 0; index < sizeof(globals) / sizeof(globals[0]); index++) {
		/* Stable numeric names select which interface and version may be bound. */
		if (globals[index].name != name)
			continue;

		/* Names, interface strings and negotiated versions are checked together. */
		same = strcmp(interface, globals[index].interface);
		if (same != 0 || version == 0 || version > globals[index].version)
			return EPROTO;

		/* A successful binding creates exactly one independent client-side object. */
		object = zwl_create(registry->client, id, globals[index].kind, version);
		if (object == NULL)
			return EPROTO;

		/* Output bindings immediately receive the negotiated property snapshot. */
		if (object->kind == ZWL_OUTPUT) {
			/* Publish the newly bound output's complete initial property snapshot. */
			error = output_events(object);
			if (error != 0)
				return error;
		}

		/* wl_shm bindings learn the formats they may use. */
		if (object->kind == ZWL_SHM) {
			error = zwl_shm_bind(object);
			if (error != 0)
				return error;
		}

		/* Seat bindings immediately learn the present device classes and the seat name. */
		if (object->kind == ZWL_SEAT) {
			/* Publish the newly bound seat's capabilities and name. */
			error = zwl_seat_bind(object);
			if (error != 0)
				return error;
		}

		/* The selected binding needs no further global search. */
		break;
	}

	/* An exhausted search found no advertised global with the requested name. */
	if (index == sizeof(globals) / sizeof(globals[0]))
		return EPROTO;

	/* Succeeded: the registry binding owns its new protocol identity and initial events. */
	return 0;
}

/* Applies surface requests to pending state without prematurely releasing current scanout. */
static int
surface_request(
	struct zwl_object *surface,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct zwl_object *object;
	struct zwl_object *previous;
	uint32_t id;
	uint32_t scalar;
	uint32_t x;
	uint32_t y;
	int error;

	/* Surface request availability follows the bound compositor version. */
	if ((opcode == 7U && surface->version < 2U) ||
	    (opcode == 8U && surface->version < 3U) ||
	    (opcode == 9U && surface->version < 4U))
		return EPROTO;

	/* The selected surface methods preserve ordinary double-buffering semantics. */
	switch (opcode) {
	case 0:
		/* Shell roles must retire before their underlying surface identity. */
		if (size != 0 || surface->role != NULL)
			return EPROTO;

		/* Destruction keeps any front allocation alive until unscan succeeds. */
		zwl_object_destroy(surface);
		break;
	case 1:
		/* This fullscreen WSI uses zero attach offsets. */
		if (size != 12U)
			return EPROTO;

		/* Nonzero offsets cannot describe this full-output scanout contract. */
		x = word_at(bytes, 4);
		y = word_at(bytes, 8);
		if (x != 0 || y != 0)
			return EPROTO;

		/* Only a buffer created by this connection may supply pending surface content. */
		id = word_at(bytes, 0);
		object = NULL;
		if (id != 0) {
			/* Pending content cannot borrow another interface or another client's identity. */
			object = zwl_find(surface->client, id);
			if (object == NULL || object->kind != ZWL_BUFFER)
				return EPROTO;
		}

		/* Acquiring the replacement first makes repeated attachment of the same buffer safe. */
		zwl_buffer_get(object);
		previous = surface->pending;
		surface->pending = object;
		surface->attached = 1;
		zwl_buffer_put(previous);
		break;
	case 2:
	case 9:
		/* Damage is a rectangle (x, y, width, height), in buffer pixels at scale one without transform. */
		if (size != 16U)
			return EPROTO;

		/* It joins the pending damage (a wl_shm image copies only those rows). */
		add_damage(surface, (int32_t)word_at(bytes, 0), (int32_t)word_at(bytes, 4), (int32_t)word_at(bytes, 8), (int32_t)word_at(bytes, 12));
		break;
	case 3:
		/* One frame request allocates one callback in pending state. */
		if (size != 4U)
			return EPROTO;

		/* A callback ID cannot alias any existing interface. */
		id = word_at(bytes, 0);
		object = zwl_create(surface->client, id, ZWL_CALLBACK, 1);
		if (object == NULL)
			return EPROTO;

		/* The next commit owns this ordered callback, not the present front buffer. */
		append_callbacks(&surface->callbacks, object);
		break;
	case 4:
	case 5:
		/* Region references are nullable and belong to the same client. */
		if (size != 4U)
			return EPROTO;

		/* This opaque fullscreen policy needs no retained input/opaque region geometry. */
		id = word_at(bytes, 0);
		if (id != 0) {
			/* Advisory regions still require a live object of the correct interface. */
			object = zwl_find(surface->client, id);
			if (object == NULL || object->kind != ZWL_REGION)
				return EPROTO;
		}

		/* Succeeded: the region reference was valid at request time. */
		break;
	case 6:
		/* Commit itself has no arguments. */
		if (size != 0)
			return EPROTO;

		/* Publish pending state only after all role and configure checks succeed. */
		error = surface_commit(surface);
		if (error != 0)
			return error;

		/* Succeeded: the event loop may present this surface's completed commit. */
		break;
	case 7:
	case 8:
		/* The selected unscaled linear scanout cannot interpret transformed image storage. */
		if (size != 4U)
			return EPROTO;

		/* Transform zero and buffer scale one preserve the exported immutable image geometry. */
		scalar = word_at(bytes, 0);
		if ((opcode == 7U && scalar != 0U) || (opcode == 8U && scalar != 1U))
			return EPROTO;

		/* Succeeded: the requested transform is the supported identity transform. */
		break;
	default:
		/* No other surface version was advertised. */
		return EPROTO;
	}

	/* Succeeded: this supported request updated the surface's pending protocol state. */
	return 0;
}

/* Commits pending state after the initial xdg-shell configure/acknowledgment exchange. */
static int
surface_commit(
	struct zwl_object *surface)
{
	struct zwl_object *role;
	struct zwl_object *previous;
	struct zwl_server *server;
	int error;

	/*
	 * A cursor surface's content is used directly, with no configure; a
	 * surface with no role yet keeps its content the same way, unshown
	 * (a client commits its cursor surface before set_cursor names it).
	 */
	role = surface->role;
	server = surface->client->server;
	if (surface->cursor_role || role == NULL) {
		previous = surface->queued;
		if (surface->attached) {
			surface->queued = surface->pending;
			surface->pending = NULL;
		} else {
			surface->queued = surface->current;
			zwl_buffer_get(surface->queued);
		}

		/* The content and its damage are committed. */
		surface->attached = 0;
		surface->ready = 1;
		if (surface->queued != NULL)
			surface->queued->busy = 1;
		commit_damage(surface);
		zwl_buffer_put(previous);
		append_callbacks(&surface->committed_callbacks, surface->callbacks);
		surface->callbacks = NULL;
		return 0;
	}

	/* Otherwise only a toplevel supplies presentable content in this compositor. */
	if (role->top == NULL)
		return EPROTO;

	/* The first empty commit requests the compositor's configure state. */
	if (!surface->configured) {
		/* Initial configure must precede every buffer-bearing map or remap. */
		if (surface->pending != NULL)
			return EPROTO;

		/* A window chooses its size; a fullscreen one gets the output's. */
		error = zwl_window_send_configure(surface);
		if (error != 0)
			return error;

		/* Only ack_configure can authorize a later buffer-bearing commit. */
		surface->configured = 1;
		surface->attached = 0;
		return 0;
	}

	/* No buffer becomes presentable before its configure has been acknowledged. */
	if (!surface->acknowledged)
		return EPROTO;

	/* A commit without attach reuses its existing surface content. */
	if (!surface->attached) {
		/* Latest committed content may still be waiting for presentation. */
		surface->pending = surface->current;
		if (surface->ready)
			surface->pending = surface->queued;

		/* A metadata-only commit preserves the last committed, possibly queued content. */
		zwl_buffer_get(surface->pending);
	}

	/* Explicit unmap returns xdg-shell to its initial configure handshake state. */
	if (surface->attached && surface->pending == NULL) {
		surface->configured = 0;
		surface->acknowledged = 0;
		surface->configure_serial = 0;
	}

	/* New commits replace only an unpresented queued image; current scanout keeps its hold. */
	previous = surface->queued;
	surface->queued = surface->pending;
	surface->pending = NULL;
	surface->attached = 0;
	surface->ready = 1;
	server->commit_order++;
	surface->commit_order = server->commit_order;
	if (surface->queued != NULL)
		surface->queued->busy = 1;

	/* The damage goes with the commit. */
	commit_damage(surface);

	/* Dropped mailbox images are reusable once no other compositor use remains. */
	zwl_buffer_put(previous);
	append_callbacks(&surface->committed_callbacks, surface->callbacks);
	surface->callbacks = NULL;

	/* Succeeded: the scheduler owns the latest pending image and all frame callbacks. */
	return 0;
}

/* Implements the selected fullscreen xdg-shell role and configure lifetime. */
static int
shell_request(
	struct zwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct zwl_object *surface;
	struct zwl_object *created;
	struct zwl_object *other;
	const char *text;
	size_t offset;
	uint32_t id;
	uint32_t serial;
	int32_t width;
	int32_t height;
	int error;

	/* The global shell creates one xdg role for an existing role-free surface. */
	if (object->kind == ZWL_WM) {
		/* The binding may retire only when its client has no surviving shell roles. */
		if (opcode == 0 && size == 0) {
			/* A wm_base cannot disappear while this client still owns xdg surfaces. */
			for (other = object->client->objects; other != NULL; other = other->next) {
				/* A live role still depends on its global shell contract. */
				if (other->kind == ZWL_XDG_SURFACE && !other->dead)
					return EPROTO;
			}

			/* This global binding no longer has live shell children. */
			zwl_object_destroy(object);
			return 0;
		}

		/* Unsolicited pong replies have no effect on the fullscreen policy. */
		if (opcode == 3U && size == 4U)
			return 0;

		/* Positioners and popups were not included in this minimal fullscreen policy. */
		if (opcode != 2U || size != 8U)
			return EPROTO;

		/* The role cannot be attached to a foreign object or an already assigned surface. */
		id = word_at(bytes, 4);
		surface = zwl_find(object->client, id);
		if (surface == NULL || surface->kind != ZWL_SURFACE || surface->role != NULL)
			return EPROTO;

		/* Publish both directions only after the role identity is allocated. */
		id = word_at(bytes, 0);
		created = zwl_create(object->client, id, ZWL_XDG_SURFACE, 1);
		if (created == NULL)
			return EPROTO;

		/* The surface owns no additional memory reference to its protocol role. */
		created->surface = surface;
		surface->role = created;
		return 0;
	}

	/* Shell objects cannot operate after their underlying surface disappears. */
	surface = object->surface;
	if (surface == NULL)
		return EPROTO;

	/* xdg_surface owns the configure serial and the sole toplevel child. */
	if (object->kind == ZWL_XDG_SURFACE) {
		/* Parent retirement cannot invalidate a surviving toplevel child. */
		if (opcode == 0 && size == 0 && object->top == NULL) {
			zwl_object_destroy(object);
			return 0;
		}

		/* Toplevel creation is one-shot while the child remains alive. */
		if (opcode == 1U && size == 4U && object->top == NULL) {
			/* Allocate the child before publishing either shell backreference. */
			id = word_at(bytes, 0);
			created = zwl_create(object->client, id, ZWL_TOPLEVEL, 1);
			if (created == NULL)
				return EPROTO;

			/* Both shell layers refer to the same core surface. */
			created->surface = surface;
			created->role = object;
			object->top = created;
			return 0;
		}

		/* Window geometry is advisory, but its positive extent must be well formed. */
		if (opcode == 3U && size == 16U) {
			/* Geometry may be advisory, but its extent must remain strictly positive. */
			width = (int32_t)word_at(bytes, 8);
			height = (int32_t)word_at(bytes, 12);
			if (width <= 0 || height <= 0)
				return EPROTO;

			/* Fullscreen output extent remains the compositor's configured geometry. */
			return 0;
		}

		/* An acknowledgment names a configure sent to this surface (the latest, or an earlier one). */
		if (opcode == 4U && size == 4U) {
			/* An old or foreign serial cannot authorize new buffer-bearing commits. */
			serial = word_at(bytes, 0);
			if (!surface->configured || serial == 0 || serial > surface->configure_serial)
				return EPROTO;

			/* Buffer-bearing commits may now publish this configured surface. */
			surface->acknowledged = 1;
			return 0;
		}

		/* Popup construction and unknown requests have no fullscreen implementation. */
		return EPROTO;
	}

	/* Toplevel methods validate their own payload before applying this fullscreen policy. */
	switch (opcode) {
	case 0:
		/* The child must retire before its xdg_surface parent can be destroyed. */
		if (size != 0)
			return EPROTO;

		/* Retire the child identity without destroying the underlying core surface. */
		zwl_object_destroy(object);
		break;
	case 2:
	case 3:
		/* Titles and application IDs have canonical strings but no decoration output. */
		error = string_at(bytes, size, 0, &text, &offset);
		if (error != 0 || offset != size)
			return EPROTO;

		/* The selected compositor has no titlebar or task switcher to update. */
		break;
	case 11:
		/* A fullscreen target is one nullable output identity. */
		if (size != 4U)
			return EPROTO;

		/* The default output needs no explicit proxy identity from this client. */
		id = word_at(bytes, 0);
		if (id != 0) {
			/* Explicit targets must refer to this connection's own output binding. */
			other = zwl_find(object->client, id);
			if (other == NULL || other->kind != ZWL_OUTPUT)
				return EPROTO;
		}

		/* The window becomes fullscreen: at the origin, the output's size (design D0, D6). */
		if (!surface->fullscreen) {
			surface->fullscreen = 1;
			surface->window_x = surface->x;
			surface->window_y = surface->y;
			surface->window_width = 0;
			surface->window_height = 0;
			if (surface->current != NULL)
				zwl_buffer_size(surface->current, &surface->window_width, &surface->window_height);

			/* It covers the output from the origin. */
			surface->x = 0;
			surface->y = 0;
			object->client->server->dirty = 1;

			/* A window already configured is told now; otherwise the first configure says it. */
			if (surface->configured) {
				error = zwl_window_send_configure(surface);
				if (error != 0)
					return error;
			}
		}
		break;
	case 1:
		/* This fullscreen policy supports independent toplevels only. */
		if (size != 4U)
			return EPROTO;

		/* A nonzero parent would require a transient window hierarchy. */
		id = word_at(bytes, 0);
		if (id != 0)
			return EPROTO;

		/* The existing toplevel stays independent from other windows. */
		break;
	case 7:
	case 8:
		/* Size hints contain exactly one nonnegative width and height. */
		if (size != 8U)
			return EPROTO;

		/* Advisory limits cannot encode negative extents. */
		width = (int32_t)word_at(bytes, 0);
		height = (int32_t)word_at(bytes, 4);
		if (width < 0 || height < 0)
			return EPROTO;

		/* The client still receives the compositor's fixed fullscreen configure. */
		break;
	case 12:
		/* Leaving fullscreen has no payload. */
		if (size != 0)
			return EPROTO;

		/* The window returns to its place and size before fullscreen. */
		if (surface->fullscreen) {
			surface->fullscreen = 0;
			surface->x = surface->window_x;
			surface->y = surface->window_y;
			object->client->server->dirty = 1;
			if (surface->configured) {
				error = zwl_window_send_configure(surface);
				if (error != 0)
					return error;
			}
		}

		/* Succeeded: the window left fullscreen. */
		break;
	case 9:
	case 10:
	case 13:
		/* Maximize and minimize have no payload or effect yet (p011). */
		if (size != 0)
			return EPROTO;

		/* The request is valid and has no effect. */
		break;
	default:
		/* Interactive moves, resizing and window menus have no implementation in this fullscreen policy. */
		return EPROTO;
	}

	/* Succeeded: the supported toplevel request is valid for this fullscreen role. */
	return 0;
}

/* Creates an ordinary wl_buffer from exactly one typed allocation capability and metadata array. */
static int
factory_request(
	struct zwl_object *factory,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct zwl_object *buffer;
	struct gpu_image_descriptor image;
	uint32_t id;
	uint32_t length;
	int descriptor;
	int error;

	/* Destroying a binding does not destroy buffers it previously created. */
	if (opcode == 0 && size == 0) {
		zwl_object_destroy(factory);
		return 0;
	}

	/* The nha signature has new_id and array bytes; h contributes no wire word. */
	if (opcode != 1U || size != 8U + sizeof(image))
		return EPROTO;

	/* The array describes one complete immutable image record. */
	length = word_at(bytes, 4);
	if (length != sizeof(image))
		return EPROTO;

	/* Consume the fd only after the complete byte payload has passed framing checks. */
	descriptor = zwl_take_fd(factory->client);
	if (descriptor < 0)
		return EAGAIN;

	/* Creation failure still closes the request-owned descriptor immediately. */
	id = word_at(bytes, 0);
	buffer = zwl_create(factory->client, id, ZWL_BUFFER, 1);
	if (buffer == NULL) {
		close(descriptor);
		return EPROTO;
	}

	/* The consumer imports into its own context and never receives the producer session. */
	memcpy(&image, bytes + 8U, sizeof(image));
	error = zwl_gpu_import(buffer, descriptor, &image);

	/* Window mode's Vulkan image, made once for the buffer's lifetime (design D2). */
	if (error == 0)
		error = zwl_import_create(buffer, descriptor);
	close(descriptor);
	if (error != 0) {
		printf("ZWL IMPORT_ERROR client=%llu buffer=%u errno=%d\n", (unsigned long long)factory->client->number, buffer->id, error);
		zwl_object_destroy(buffer);
		return EPROTO;
	}

	/* Succeeded: the wl_buffer owns its independently imported resource. */
	return 0;
}

/*
 * Sends a toplevel's configure: the output's size and the fullscreen and
 * activated states for a fullscreen window; otherwise its size before
 * fullscreen, or 0x0 (the client chooses), and activated.  The xdg_surface
 * configure with a new serial follows.
 */
int
zwl_window_send_configure(
	struct zwl_object *surface)
{
	struct zwl_server *server;
	struct zwl_object *role;
	uint32_t configure[5];
	size_t size;
	int error;

	/* The size and states. */
	server = surface->client->server;
	role = surface->role;
	if (surface->fullscreen) {
		configure[0] = server->width;
		configure[1] = server->height;
		configure[2] = 8;
		configure[3] = 2;
		configure[4] = 4;
		size = 5U * sizeof(uint32_t);
	} else {
		configure[0] = surface->window_width;
		configure[1] = surface->window_height;
		configure[2] = 4;
		configure[3] = 4;
		size = 4U * sizeof(uint32_t);
	}

	/* The toplevel configure. */
	error = zwl_emit(surface->client, role->top->id, 0, configure, size);
	if (error != 0)
		return error;

	/* Nonzero serials distinguish an acknowledged configure from none. */
	server->serial++;
	if (server->serial == 0)
		server->serial++;

	/* The xdg_surface configure commits the toplevel state. */
	surface->configure_serial = server->serial;
	error = zwl_emit(surface->client, role->id, 0, &surface->configure_serial, 4);
	if (error != 0)
		return error;

	/* Succeeded. */
	printf("ZWL CONFIGURE client=%llu surface=%u serial=%u width=%u height=%u fullscreen=%u\n", (unsigned long long)surface->client->number, surface->id, surface->configure_serial, configure[0], configure[1], surface->fullscreen);
	return 0;
}

/* Adds a rectangle to a surface's pending damage (their bounding box). */
static void
add_damage(
	struct zwl_object *surface,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	/* An empty rectangle adds nothing. */
	if (width <= 0 || height <= 0)
		return;

	/* The first rectangle, or the box around both. */
	if (!surface->damaged) {
		surface->damage[0] = x;
		surface->damage[1] = y;
		surface->damage[2] = x + width;
		surface->damage[3] = y + height;
		surface->damaged = 1;
		return;
	}

	/* The box grows to hold the rectangle. */
	if (x < surface->damage[0])
		surface->damage[0] = x;
	if (y < surface->damage[1])
		surface->damage[1] = y;
	if (x + width > surface->damage[2])
		surface->damage[2] = x + width;
	if (y + height > surface->damage[3])
		surface->damage[3] = y + height;
}

/*
 * Moves a commit's damage to the committed damage, joined with any not yet
 * copied (two commits may come before one copy).
 */
static void
commit_damage(
	struct zwl_object *surface)
{
	/* No damage leaves the committed damage as it is. */
	if (!surface->damaged)
		return;

	/* The first, or the box around both. */
	if (!surface->committed_damaged) {
		memcpy(surface->committed_damage, surface->damage, sizeof(surface->damage));
		surface->committed_damaged = 1;
	} else {
		if (surface->damage[0] < surface->committed_damage[0])
			surface->committed_damage[0] = surface->damage[0];
		if (surface->damage[1] < surface->committed_damage[1])
			surface->committed_damage[1] = surface->damage[1];
		if (surface->damage[2] > surface->committed_damage[2])
			surface->committed_damage[2] = surface->damage[2];
		if (surface->damage[3] > surface->committed_damage[3])
			surface->committed_damage[3] = surface->damage[3];
	}

	/* The pending damage starts again. */
	surface->damaged = 0;
}

/* Preserves callback request order across pending-state commits and mailbox replacement. */
static void
append_callbacks(
	struct zwl_object **list,
	struct zwl_object *callbacks)
{
	/* Each existing callback must complete before later callbacks in the same surface stream. */
	while (*list != NULL)
		list = &(*list)->callback_next;

	/* Ownership of the supplied chain moves to this surface state. */
	*list = callbacks;

	/* Succeeded: the surface state owns the ordered callback chain. */
	return;
}
