/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Selected core Wayland, xdg-shell and typed GPU-buffer request semantics.
 */

#include "desktop.h"
#include "kwl.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "menu.h"
#include "titlebar.h"
#include "inset.h"
#include "edit.h"
#include "popup.h"
#include "toplevel.h"
#include "subsurface.h"
#include "data.h"
#include "extras.h"
#include "panels.h"
#include "tablet.h"
#include "ime.h"
#include "activation.h"
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* wl_output's version 4 events: its name and its description. */
#define OUTPUT_NAME		4U
#define OUTPUT_DESCRIPTION	5U

/* The first wl_output global name of a head (heads.c), the version a head's wl_output offers, and the most heads a registry is told of. */
#define OUTPUT_HEAD_GLOBAL_FIRST	1000U
#define OUTPUT_HEAD_VERSION		4U
#define OUTPUT_HEADS_TOLD		16U

/* xdg_wm_base's error for a binding destroyed under its own live xdg_surfaces. */
#define WM_ERROR_DEFUNCT_SURFACES	1U

/* The registry advertises only implemented interfaces and their actual versions. */
struct kwl_global {
	uint32_t name;
	const char *interface;
	uint32_t version;
	enum kwl_kind kind;
};

/* Stable global names are scoped to one compositor process generation. */
static const struct kwl_global globals[] = {
	{ 1, "wl_compositor", 4, KWL_COMPOSITOR },
	{ 2, "xdg_wm_base", 4, KWL_WM },
	{ 3, NULL, 0, KWL_FACTORY },
	{ 4, "wl_output", 4, KWL_OUTPUT },
	{ 5, "wl_seat", 5, KWL_SEAT },
	{ 6, "wl_shm", 1, KWL_SHM },
	{ 7, "xdg_menu_manager_v1", 2, KWL_MENU_MANAGER },
	{ 8, "wl_subcompositor", 1, KWL_SUBCOMPOSITOR },
	{ 9, "wl_data_device_manager", 3, KWL_DATA_MANAGER },
	{ 10, "zxdg_decoration_manager_v1", 1, KWL_DECORATION_MANAGER },
	{ 11, "wp_cursor_shape_manager_v1", 1, KWL_CURSOR_SHAPE_MANAGER },
	{ 12, "wp_viewporter", 1, KWL_VIEWPORTER },
	{ 13, "zwp_text_input_manager_v3", 1, KWL_TEXT_INPUT_MANAGER },
	{ 14, "zwp_input_method_manager_v2", 1, KWL_INPUT_METHOD_MANAGER },
	{ 15, "zwp_virtual_keyboard_manager_v1", 1, KWL_VIRTUAL_KEYBOARD_MANAGER },
	{ 16, "kl_titlebar_manager_v1", 4, KWL_TITLEBAR_MANAGER },
	{ 17, "kl_glass_manager_v1", 2, KWL_GLASS_MANAGER },
	{ 18, "zwp_primary_selection_device_manager_v1", 1, KWL_PRIMARY_MANAGER },
	{ 19, "zwp_tablet_manager_v2", 1, KWL_TABLET_MANAGER },
	{ 20, "kl_ime_status_manager_v1", 2, KWL_IME_STATUS_MANAGER },
	{ 21, "kl_desktop_manager_v1", 1, KWL_DESKTOP_MANAGER },
	{ 22, "kl_keyboard_inset_manager_v1", 1, KWL_KEYBOARD_INSET_MANAGER },
	{ 23, "kl_edit_manager_v1", 1, KWL_EDIT_MANAGER },
	{ 24, "org_kde_kwin_server_decoration_manager", 1, KWL_KDE_DECORATION_MANAGER },
	{ 25, KL_SYSTEM_MANAGER_NAME, KL_SYSTEM_MANAGER_VERSION, KWL_SYSTEM_MANAGER },
	{ 26, "xdg_activation_v1", 1, KWL_ACTIVATION_MANAGER },
	{ 27, "kl_theme_v1", 2, KWL_THEME },
	{ 28, "wp_content_type_manager_v1", 1, KWL_CONTENT_TYPE_MANAGER },
};

static void global_identity(const struct kwl_global *global, const char **interface, uint32_t *version);
static uint32_t word_at(const unsigned char *bytes, size_t offset);
static int string_at(const unsigned char *bytes, size_t size, size_t offset, const char **text, size_t *next);
static int registry_events(struct kwl_object *registry);
static int output_events(struct kwl_object *output);
static int output_names(struct kwl_object *output, const struct kwl_output_view *view);
static int bind_global(struct kwl_object *registry, const unsigned char *bytes, size_t size);
static int bind_head_output(struct kwl_object *registry, uint32_t name, const char *interface, uint32_t version, uint32_t id);
static int registry_global(struct kwl_object *registry, uint32_t name, const char *interface, uint32_t version);
static int surface_request(struct kwl_object *surface, uint32_t opcode, const unsigned char *bytes, size_t size);
static int surface_commit(struct kwl_object *surface);
static int shell_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
static void append_callbacks(struct kwl_object **list, struct kwl_object *callbacks);
static void add_damage(struct kwl_object *surface, int32_t x, int32_t y, int32_t width, int32_t height);
static void commit_fence(struct kwl_object *surface, unsigned attached);
static void commit_damage(struct kwl_object *surface);
static int send_bounds(struct kwl_object *surface);
static void window_bounds(struct kwl_server *server, int32_t *width, int32_t *height);

/*
 * Dispatches one validated frame through its client-local interface identity.
 */
int
kwl_dispatch(
	struct kwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *object;
	struct kwl_object *created;
	uint32_t new_id;
	enum kwl_kind kind;
	int error;

	/* Unknown IDs cannot select another client's protocol objects or GPU resources. */
	object = kwl_find(client, id);
	if (object == NULL) {
		error = kwl_error(client, id, "unknown object");
		return error;
	}

	/* Every method validates its exact payload length and interface opcode. */
	error = EPROTO;
	switch (object->kind) {
	case KWL_DISPLAY:
		/* Both supported display constructors contain exactly one new_id. */
		if (size != 4U || opcode > 1U)
			break;

		/* A sync callback is completed after preceding requests have been processed. */
		new_id = word_at(bytes, 0);
		kind = KWL_CALLBACK;
		if (opcode == 1U)
			kind = KWL_REGISTRY;

		/* Duplicate or forbidden IDs are rejected before emitting constructor events. */
		created = kwl_create(client, new_id, kind, 1);
		if (created == NULL)
			break;

		/* Registry discovery and one-shot sync use their canonical server events. */
		if (opcode == 1U)
			error = registry_events(created);
		else {
			kwl_callbacks_done(&created);
			error = 0;
		}

		break;
	case KWL_REGISTRY:
		/* bind is the sole registry request. */
		if (opcode == 0)
			error = bind_global(object, bytes, size);
		break;
	case KWL_COMPOSITOR:
		/* The compositor creates independent surfaces or region identities. */
		if (size != 4U || opcode > 1U)
			break;

		/* Regions need no retained geometry for this opaque fullscreen policy. */
		kind = KWL_SURFACE;
		if (opcode == 1U)
			kind = KWL_REGION;

		/* The child surface inherits the negotiated compositor version. */
		new_id = word_at(bytes, 0);
		created = kwl_create(client, new_id, kind, object->version);
		if (created != NULL)
			error = 0;
		break;
	case KWL_SURFACE:
		error = surface_request(object, opcode, bytes, size);
		break;
	case KWL_REGION:
		/* Region destruction retires only its protocol identity. */
		if (opcode == 0 && size == 0) {
			kwl_object_destroy(object);
			error = 0;
		} else if ((opcode == 1U || opcode == 2U) && size == 16U) {
			/* Damage and input regions do not alter the single opaque scanout plane. */
			error = 0;
		}

		break;
	case KWL_BUFFER:
		/* Buffer destruction keeps any active GPU use alive independently. */
		if (opcode == 0 && size == 0) {
			kwl_object_destroy(object);
			error = 0;
		}

		break;
	case KWL_GPU_OBJECT:
	case KWL_FACTORY:
		error = kl_backend_gpu_request(kwl_gpu_host(), kwl_gpu_resource(object), opcode, bytes, size);
		break;
	case KWL_SHM:
	case KWL_SHM_POOL:
		error = kwl_shm_request(object, opcode, bytes, size);
		break;
	case KWL_WM:
	case KWL_XDG_SURFACE:
	case KWL_TOPLEVEL:
		error = shell_request(object, opcode, bytes, size);
		break;
	case KWL_SEAT:
	case KWL_POINTER:
	case KWL_KEYBOARD:
	case KWL_TOUCH:
		error = kwl_seat_request(object, opcode, bytes, size);
		break;
	case KWL_MENU_MANAGER:
	case KWL_MENU:
	case KWL_TOPLEVEL_MENU:
	case KWL_CONTEXT_MENU:
		/* The System Menu and its context menus (menu.c). */
		error = kwl_menu_request(object, opcode, bytes, size);
		break;
	case KWL_TITLEBAR_MANAGER:
	case KWL_TITLEBAR:
		/* The Titlebar Presentation (titlebar.c). */
		error = kwl_titlebar_request(object, opcode, bytes, size);
		break;
	case KWL_POSITIONER:
	case KWL_POPUP:
		/* xdg_positioner and xdg_popup (popup.c). */
		error = kwl_popup_request(object, opcode, bytes, size);
		break;
	case KWL_OUTPUT:
		/* release, from version 3, is the only output request. */
		if (opcode == 0 && size == 0 && object->version >= 3U) {
			kwl_object_destroy(object);
			error = 0;
		}

		/* Any other output request stays refused. */
		break;
	case KWL_DATA_MANAGER:
	case KWL_DATA_SOURCE:
	case KWL_DATA_DEVICE:
	case KWL_DATA_OFFER:
		/* The clipboard (data.c). */
		error = kwl_data_request(object, opcode, bytes, size);
		break;
	case KWL_PRIMARY_MANAGER:
	case KWL_PRIMARY_SOURCE:
	case KWL_PRIMARY_DEVICE:
	case KWL_PRIMARY_OFFER:
		/* The primary selection (primary.c). */
		error = kwl_primary_request(object, opcode, bytes, size);
		break;
	case KWL_DECORATION_MANAGER:
	case KWL_DECORATION:
	case KWL_KDE_DECORATION_MANAGER:
	case KWL_KDE_DECORATION:
		/* xdg-decoration and KDE's server decoration (decoration.c). */
		error = kwl_decoration_request(object, opcode, bytes, size);
		break;
	case KWL_CURSOR_SHAPE_MANAGER:
	case KWL_CURSOR_SHAPE_DEVICE:
		/* cursor-shape (cursor.c). */
		error = kwl_cursor_shape_request(object, opcode, bytes, size);
		break;
	case KWL_VIEWPORTER:
	case KWL_VIEWPORT:
		/* viewporter (viewport.c). */
		error = kwl_viewport_request(object, opcode, bytes, size);
		break;
	case KWL_CONTENT_TYPE_MANAGER:
	case KWL_CONTENT_TYPE:
		/* content-type (content-type.c). */
		error = kwl_content_type_request(object, opcode, bytes, size);
		break;
	case KWL_GLASS_MANAGER:
	case KWL_GLASS:
		/* A surface's glass panels (panels.c). */
		error = kwl_panels_request(object, opcode, bytes, size);
		break;
	case KWL_SUBCOMPOSITOR:
		/* wl_subcompositor (subsurface.c). */
		error = kwl_subcompositor_request(object, opcode, bytes, size);
		break;
	case KWL_SUBSURFACE:
		/* wl_subsurface (subsurface.c). */
		error = kwl_subsurface_request(object, opcode, bytes, size);
		break;
	case KWL_TEXT_INPUT_MANAGER:
	case KWL_TEXT_INPUT:
		/* The text input protocol (text-input.c). */
		error = kwl_text_input_request(object, opcode, bytes, size);
		break;
	case KWL_INPUT_METHOD_MANAGER:
	case KWL_INPUT_METHOD:
	case KWL_INPUT_POPUP:
	case KWL_KEYBOARD_GRAB:
	case KWL_VIRTUAL_KEYBOARD_MANAGER:
	case KWL_VIRTUAL_KEYBOARD:
	case KWL_IME_STATUS_MANAGER:
	case KWL_IME_STATUS:
		/* The input method's protocols (input-method.c). */
		error = kwl_ime_request(object, opcode, bytes, size);
		break;
	case KWL_TABLET_MANAGER:
	case KWL_TABLET_SEAT:
	case KWL_TABLET:
	case KWL_TABLET_TOOL:
		/* The pen tablets (tablet.c). */
		error = kwl_tablet_request(object, opcode, bytes, size);
		break;
	case KWL_DESKTOP_MANAGER:
	case KWL_DESKTOP_SURFACE:
		/* The desktop surface (desktop.c, ws094-p002). */
		error = kwl_desktop_request(object, opcode, bytes, size);
		break;
	case KWL_KEYBOARD_INSET_MANAGER:
	case KWL_KEYBOARD_INSET:
		/* The keyboard inset (inset.c, ws102-p015). */
		error = kwl_inset_request(object, opcode, bytes, size);
		break;
	case KWL_EDIT_MANAGER:
	case KWL_EDIT:
		/* The editing operations (edit.c, ws102-p017). */
		error = kwl_edit_request(object, opcode, bytes, size);
		break;
	case KWL_SYSTEM_SETTINGS:
		/* Keiland's system extension: the settings (settings.c, WS135). */
		error = kwl_settings_request(object, opcode, bytes, size);
		break;
	case KWL_SYSTEM_MANAGER:
	case KWL_SYSTEM_NETWORK:
	case KWL_SYSTEM_AUDIO:
	case KWL_SYSTEM_POWER:
	case KWL_SYSTEM_DEVICES:
	case KWL_SYSTEM_ACCOUNT:
	case KWL_SYSTEM_SHARING:
	case KWL_SYSTEM_NOTIFY:
	case KWL_SYSTEM_MAIL:
	case KWL_SYSTEM_PHONE:
	case KWL_SYSTEM_PRINTERS:
	case KWL_SYSTEM_DISPLAYS:
	case KWL_SYSTEM_MACHINE:
		/* Keiland's system extension: the manager, the network, the sound, the power, the devices, the account, Remote Login, the notifications, the arrivals of mail, the phone, the printers, the displays and the computer (system.c, WS131 p010, ws160-p002, ws089-p025, ws156-p002, ws169-p002, ws170-p004, ws145-p003, ws113-p005, ws188-p002). */
		error = kwl_system_request(object, opcode, bytes, size);
		break;
	case KWL_SYSTEM_MONITOR:
		/* Keiland's system extension: the monitor (sysmon.c, WS134 p012). */
		error = kwl_sysmon_request(object, opcode, bytes, size);
		break;
	case KWL_ACTIVATION_MANAGER:
	case KWL_ACTIVATION_TOKEN:
		/* xdg_activation_v1 and its tokens (activation.c, ws089-p016). */
		error = kwl_activation_request(object, opcode, bytes, size);
		break;
	case KWL_THEME:
		/* kl_theme_v1, the desktop's appearance (theme.c, ws089-p017). */
		error = kwl_theme_request(object, opcode, bytes, size);
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
			error = kwl_error(client, id, "invalid or unsupported request");

		/* The final error remains queued for bounded flush before disconnect. */
		return error;
	}

	/* Succeeded: the complete request has been processed exactly once. */
	return 0;
}

/*
 * Tells every bound wl_output the output's new size and refresh (ws113-p004a,
 * an output moved to another display), as a binding learns them: geometry,
 * mode, scale, name, description and done.  A client the events cannot be
 * queued for is left to its own failure.
 */
void
kwl_outputs_changed(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;
	int error;

	/* Each wl_output object of every live client. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			/* Only an output binding. */
			if (object->kind != KWL_OUTPUT)
				continue;

			/* The whole snapshot again; a failure is the client's. */
			error = output_events(object);
			if (error != 0)
				printf("KWL OUTPUT tell client=%llu errno=%d\n", (unsigned long long)client->number, error);
		}
	}
}

/*
 * Tells every registry of a new head's wl_output global (ws113-p004b).  A
 * client the event cannot be queued for is left to its own failure.
 */
void
kwl_output_global_add(
	struct kwl_server *server,
	uint32_t name)
{
	struct kwl_client *client;
	struct kwl_object *object;
	int error;

	/* Each live registry of every live client. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			/* Only a registry. */
			if (object->kind != KWL_REGISTRY || object->dead)
				continue;

			/* The global event; a failure is the client's. */
			error = registry_global(object, name, "wl_output", OUTPUT_HEAD_VERSION);
			if (error != 0)
				printf("KWL OUTPUT global client=%llu errno=%d\n", (unsigned long long)client->number, error);
		}
	}
}

/*
 * Tells every registry that a head's wl_output global is gone, and leaves
 * the bindings of that head inert (ws113-p004b): they are told nothing
 * more and keep only their destructor.
 */
void
kwl_output_global_remove(
	struct kwl_server *server,
	uint32_t name)
{
	struct kwl_client *client;
	struct kwl_object *object;
	uint32_t head;
	int error;

	/* The head the global names, while it is still open. */
	head = kwl_output_head_of_global(server, name);

	/* Its surfaces leave it while its bindings still name it (ws177-p001). */
	if (head != 0U)
		kwl_surface_outputs_gone(server, head);

	/* Each object of every live client. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			/* A binding of the head becomes inert. */
			if (object->kind == KWL_OUTPUT &&
			    head != 0U &&
			    object->output_head == head) {
				object->output_head = KWL_OUTPUT_GONE;
				continue;
			}

			/* Only a live registry is told. */
			if (object->kind != KWL_REGISTRY || object->dead)
				continue;

			/* The global_remove event; a failure is the client's. */
			error = kwl_emit(client, object->id, 1, &name, sizeof(name));
			if (error != 0)
				printf("KWL OUTPUT global remove client=%llu errno=%d\n", (unsigned long long)client->number, error);
		}
	}
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

/* Reports a global's interface name and version: the table's, or the OS module's for its GPU buffer global. */
static void
global_identity(
	const struct kwl_global *global,
	const char **interface,
	uint32_t *version)
{
	/* libkeiland-backend names its GPU buffer global (kl_gpu_buffer_v1 version 3 on zedBSD, zwp_linux_dmabuf_v1 elsewhere). */
	if (global->kind == KWL_FACTORY) {
		*interface = kl_backend_gpu_global_interface();
		*version = kl_backend_gpu_global_version();
		return;
	}

	/* Every other global is the table's. */
	*interface = global->interface;
	*version = global->version;

	/* Succeeded: the caller holds the advertised identity of this global. */
	return;
}

/* Announces only the selected protocol globals in stable registry order. */
static int
registry_events(
	struct kwl_object *registry)
{
	const char *interface;
	uint32_t version;
	uint32_t heads[OUTPUT_HEADS_TOLD];
	unsigned char payload[128];
	uint32_t word;
	unsigned count;
	size_t index;
	size_t length;
	size_t offset;
	int error;
	int visible;

	/* Each global event carries name, interface string and supported version. */
	for (index = 0; index < sizeof(globals) / sizeof(globals[0]); index++) {
		/* The input method's globals are shown to the input method alone (input-method.c). */
		visible = kwl_ime_global_visible(registry->client, globals[index].kind);
		if (!visible)
			continue;

		/* The system extension is shown to a session's own user alone (settings.c). */
		visible = kwl_settings_global_visible(registry->client, globals[index].kind);
		if (!visible)
			continue;

		/* Resolve the OS-owned GPU global before encoding its registry event. */
		global_identity(&globals[index], &interface, &version);
		if (interface == NULL)
			continue;

		/* Encode this advertised interface as one canonical registry global event. */
		memset(payload, 0, sizeof(payload));
		word = globals[index].name;
		memcpy(payload, &word, 4);
		length = strlen(interface) + 1U;
		word = (uint32_t)length;
		memcpy(payload + 4, &word, 4);
		memcpy(payload + 8, interface, length);
		offset = 8U + ((length + 3U) & ~(size_t)3U);
		word = version;
		memcpy(payload + offset, &word, 4);
		error = kwl_emit(registry->client, registry->id, 0, payload, offset + 4U);
		if (error != 0)
			return error;
	}

	/* Each head's wl_output (ws113-p004b). */
	count = kwl_output_head_globals(registry->client->server, heads, OUTPUT_HEADS_TOLD);
	for (index = 0; index < count; index++) {
		error = registry_global(registry, heads[index], "wl_output", OUTPUT_HEAD_VERSION);
		if (error != 0)
			return error;
	}

	/* Succeeded: discovery events precede any following display sync callback. */
	return 0;
}

/* Supplies the version-2 output geometry, current mode, scale and completion event. */
static int
output_events(
	struct kwl_object *output)
{
	struct kwl_output_view view;
	unsigned char geometry[60];
	uint32_t words[4];
	uint32_t word;
	int32_t place;
	int error;

	/* The display the binding names (ws113-p004b); a head that closed is told nothing. */
	if (output->output_head == KWL_OUTPUT_GONE)
		return 0;
	error = kwl_output_view(output->client->server, output->output_head, &view);
	if (error != 0)
		return 0;

	/*
	 * Geometry places the display in the logical plane, includes unknown
	 * physical dimensions, and the make and the model say that the display
	 * is not known (ws035-p121): the compositor draws to the GPU's scanout
	 * and never learns the monitor's EDID.
	 */
	memset(geometry, 0, sizeof(geometry));
	place = view.x;
	memcpy(geometry, &place, 4);
	place = view.y;
	memcpy(geometry + 4, &place, 4);
	word = 8;
	memcpy(geometry + 20, &word, 4);
	memcpy(geometry + 24, "Unknown", 8);
	word = 8;
	memcpy(geometry + 32, &word, 4);
	memcpy(geometry + 36, "Unknown", 8);
	error = kwl_emit(output->client, output->id, 0, geometry, 48);
	if (error != 0)
		return error;

	/* This single mode is current and preferred in the selected fullscreen policy. */
	words[0] = 3;
	words[1] = view.width;
	words[2] = view.height;
	words[3] = view.refresh;
	error = kwl_emit(output->client, output->id, 1, words, sizeof(words));
	if (error != 0)
		return error;

	/* Scale and done were introduced in output version 2. */
	if (output->version >= 2U) {
		/* This output uses one buffer pixel per surface coordinate. */
		word = 1;
		error = kwl_emit(output->client, output->id, 3, &word, sizeof(word));
		if (error != 0)
			return error;

		/* Version 4 names the output and describes it (ws035-p078). */
		if (output->version >= 4U) {
			error = output_names(output, &view);
			if (error != 0)
				return error;
		}

		/* The done event commits all preceding output properties. */
		error = kwl_emit(output->client, output->id, 2, NULL, 0);
		if (error != 0)
			return error;
	}

	/* Succeeded: output properties are complete for the negotiated version. */
	return 0;
}

/*
 * Sends a version 4 output its name and description: the name stays the
 * same while the display is shown (DISPLAY-1 the output's, DISPLAY-2 and
 * on the heads', ws113-p004b), the description gives its size.  Both name
 * the display, not the system (ws035-p121): the compositor does not tell
 * the connector, so the name is a neutral DISPLAY-n.
 */
static int
output_names(
	struct kwl_object *output,
	const struct kwl_output_view *view)
{
	char text[64];
	char name[24];
	unsigned char payload[80];
	uint32_t length;
	int error;

	/* The name, a string in the wire's padded form. */
	(void)snprintf(name, sizeof(name), "DISPLAY-%u", (unsigned)view->index + 1U);
	memset(payload, 0, sizeof(payload));
	length = (uint32_t)strlen(name) + 1U;
	memcpy(payload, &length, sizeof(length));
	memcpy(payload + 4, name, length);
	error = kwl_emit(output->client, output->id, OUTPUT_NAME, payload, 4U + ((length + 3U) & ~3U));
	if (error != 0)
		return error;

	/* The description. */
	(void)snprintf(text, sizeof(text), "Display %ux%u", view->width, view->height);
	memset(payload, 0, sizeof(payload));
	length = (uint32_t)strlen(text) + 1U;
	memcpy(payload, &length, sizeof(length));
	memcpy(payload + 4, text, length);
	error = kwl_emit(output->client, output->id, OUTPUT_DESCRIPTION, payload, 4U + ((length + 3U) & ~3U));
	if (error != 0)
		return error;

	/* Succeeded: the output is named. */
	return 0;
}

/* Resolves a registry name, exact interface string and supported requested version. */
static int
bind_global(
	struct kwl_object *registry,
	const unsigned char *bytes,
	size_t size)
{
	const char *interface;
	const char *offered;
	uint32_t offered_version;
	struct kwl_object *object;
	uint32_t name;
	uint32_t version;
	uint32_t id;
	size_t offset;
	size_t index;
	int error;
	int same;
	int visible;

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
		global_identity(&globals[index], &offered, &offered_version);
		if (offered == NULL)
			return EPROTO;
		same = strcmp(interface, offered);
		if (same != 0 ||
		    version == 0 ||
		    version > offered_version)
			return EPROTO;

		/* A global the connection was not shown cannot be bound (the input method's, input-method.c). */
		visible = kwl_ime_global_visible(registry->client, globals[index].kind);
		if (!visible)
			return EPROTO;

		/* Nor the system extension, for a client that was not shown it (settings.c). */
		visible = kwl_settings_global_visible(registry->client, globals[index].kind);
		if (!visible)
			return EPROTO;

		/* A successful binding creates exactly one independent client-side object. */
		object = kwl_create(registry->client, id, globals[index].kind, version);
		if (object == NULL)
			return EPROTO;

		/* Output bindings immediately receive the negotiated property snapshot. */
		if (object->kind == KWL_OUTPUT) {
			/* Publish the newly bound output's complete initial property snapshot. */
			error = output_events(object);
			if (error != 0)
				return error;

			/* Its client's surfaces already on the display name it too (ws177-p001). */
			kwl_surface_outputs_bound(object);
		}

		/* A system manager tells what it offers. */
		if (object->kind == KWL_SYSTEM_MANAGER) {
			error = kwl_system_bind(object);
			if (error != 0)
				return error;
		}

		/* GPU bindings receive libkeiland-backend's sampled buffer format snapshot. */
		if (object->kind == KWL_FACTORY) {
			error = kl_backend_gpu_bind(kwl_gpu_host(), kwl_gpu_resource(object));
			if (error != 0)
				return error;
		}

		/* wl_shm bindings learn the formats they may use. */
		if (object->kind == KWL_SHM) {
			error = kwl_shm_bind(object);
			if (error != 0)
				return error;
		}

		/* KDE's server decoration manager tells the default mode at once: the compositor's. */
		if (object->kind == KWL_KDE_DECORATION_MANAGER) {
			error = kwl_decoration_kde_bind(object);
			if (error != 0)
				return error;
		}

		/* The appearance's binding learns the appearance now (theme.c). */
		if (object->kind == KWL_THEME) {
			error = kwl_theme_bind(object);
			if (error != 0)
				return error;
		}

		/* Seat bindings immediately learn the present device classes and the seat name. */
		if (object->kind == KWL_SEAT) {
			/* Publish the newly bound seat's capabilities and name. */
			error = kwl_seat_bind(object);
			if (error != 0)
				return error;
		}

		/* The selected binding needs no further global search. */
		break;
	}

	/* A name past the fixed ones is a head's wl_output, or one that closed (ws113-p004b). */
	if (index == sizeof(globals) / sizeof(globals[0]) && name >= OUTPUT_HEAD_GLOBAL_FIRST) {
		error = bind_head_output(registry, name, interface, version, id);
		if (error != 0)
			return error;
		return 0;
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
	struct kwl_object *surface,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *object;
	struct kwl_object *previous;
	uint32_t id;
	uint32_t scalar;
	uint32_t x;
	uint32_t y;
	int error;
	int icon;

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
		kwl_object_destroy(surface);
		break;
	case 1:
		/* This fullscreen WSI uses zero attach offsets. */
		if (size != 12U)
			return EPROTO;

		/*
		 * Nonzero offsets cannot describe this full-output scanout
		 * contract; only the icon of the drag going on (ws189-p002)
		 * keeps them, and they move it from the pointer.
		 */
		x = word_at(bytes, 4);
		y = word_at(bytes, 8);
		icon = 0;
		if (surface->client->server->dnd_active && surface == surface->client->server->dnd_icon)
			icon = 1;
		if ((x != 0 || y != 0) && !icon)
			return EPROTO;
		surface->pending_dx += (int32_t)x;
		surface->pending_dy += (int32_t)y;

		/* Only a buffer created by this connection may supply pending surface content. */
		id = word_at(bytes, 0);
		object = NULL;
		if (id != 0) {
			/* Pending content cannot borrow another interface or another client's identity. */
			object = kwl_find(surface->client, id);
			if (object == NULL || object->kind != KWL_BUFFER)
				return EPROTO;
		}

		/* Acquiring the replacement first makes repeated attachment of the same buffer safe. */
		kwl_buffer_get(object);
		previous = surface->pending;
		surface->pending = object;
		surface->attached = 1;
		kwl_buffer_put(previous);
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
		object = kwl_create(surface->client, id, KWL_CALLBACK, 1);
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
			object = kwl_find(surface->client, id);
			if (object == NULL || object->kind != KWL_REGION)
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
	struct kwl_object *surface)
{
	struct kwl_object *role;
	struct kwl_object *previous;
	struct kwl_server *server;
	unsigned attached;
	uint32_t replaced;
	int error;

	/* A window waiting for its image of a new size: when its latest commit came (BUG-179). */
	if (surface->resized_ms != 0U)
		surface->resized_commit_ms = kwl_milliseconds();

	/* The viewport's pending source and destination apply with the commit (viewport.c). */
	kwl_viewport_commit(surface);

	/* So does the content type (content-type.c). */
	kwl_content_type_commit(surface);

	/* So do the glass panels (panels.c). */
	kwl_panels_commit(surface);

	/* A sub-surface's commit waits for its parent's when it is synchronized (subsurface.c). */
	if (surface->sub_role != NULL) {
		error = kwl_subsurface_commit(surface);
		if (error != 0)
			return error;
		return 0;
	}

	/*
	 * A cursor surface's content is used directly, with no configure; a
	 * surface with no role yet keeps its content the same way, unshown
	 * (a client commits its cursor surface before set_cursor names it).
	 * Its sub-surfaces go with it.
	 */
	role = surface->role;
	server = surface->client->server;
	attached = surface->attached;

	/* The offsets attached since the last commit move the surface from here (a drag's icon, ws189-p002). */
	surface->offset_x += surface->pending_dx;
	surface->offset_y += surface->pending_dy;
	surface->pending_dx = 0;
	surface->pending_dy = 0;
	if (surface->cursor_role || role == NULL) {
		error = kwl_surface_queue(surface);
		if (error != 0)
			return error;
		kwl_subsurface_applied(surface);

		/* The input method's candidate window is such a surface; its commit redraws the output (input-method.c). */
		kwl_ime_surface_commit(surface);
		return 0;
	}

	/* Otherwise only a toplevel supplies presentable content in this compositor. */
	if (role->top == NULL)
		return EPROTO;

	/* A window geometry set since the last commit applies from this one. */
	if (surface->pending_geometry_set) {
		memcpy(surface->geometry, surface->pending_geometry, sizeof(surface->geometry));
		surface->geometry_set = 1;
		surface->pending_geometry_set = 0;
	}

	/* The first empty commit requests the compositor's configure state. */
	if (!surface->configured) {
		/* Initial configure must precede every buffer-bearing map or remap. */
		if (surface->pending != NULL)
			return EPROTO;

		/* A popup gets its place and size (popup.c); a window chooses its size, a fullscreen one gets the output's. */
		if (role->top->kind == KWL_POPUP) {
			error = kwl_popup_send_configure(surface);
		} else {
			/* Docked from the start when the window in front is docked (shell.c, ws099-p033). */
			if (server->glass)
				(void)kwl_glass_open_docked(server, surface);
			error = kwl_window_send_configure(surface);
		}

		/* A client that cannot take the configure is failed. */
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

	/* Decoration ownership and window geometry join the content for this commit. */
	kwl_decoration_commit(surface);

	/* A commit without attach reuses its existing surface content. */
	if (!surface->attached) {
		/* Latest committed content may still be waiting for presentation. */
		surface->pending = surface->current;
		if (surface->ready)
			surface->pending = surface->queued;

		/* A metadata-only commit preserves the last committed, possibly queued content. */
		kwl_buffer_get(surface->pending);
	}

	/* Explicit unmap returns xdg-shell to its initial configure handshake state. */
	if (surface->attached && surface->pending == NULL) {
		surface->configured = 0;
		surface->acknowledged = 0;
		surface->configure_serial = 0;
	}

	/* Names the commit when the per-frame lines were asked for (with the image it replaces, 0 for none). */
	if (server->log_frames && attached && surface->pending != NULL) {
		replaced = 0U;
		if (surface->queued != NULL)
			replaced = surface->queued->id;
		printf("KWL COMMIT client=%llu surface=%u buffer=%u queued=%u\n", (unsigned long long)surface->client->number, surface->id,
		       surface->pending->id, replaced);
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

	/* The damage goes with the commit, and so does its acquire fence. */
	commit_damage(surface);
	commit_fence(surface, attached);

	/* Dropped mailbox images are reusable once no other compositor use remains. */
	kwl_buffer_put(previous);
	append_callbacks(&surface->committed_callbacks, surface->callbacks);
	surface->callbacks = NULL;

	/* The window's sub-surfaces go with its state (subsurface.c). */
	kwl_subsurface_applied(surface);

	/* Succeeded: the scheduler owns the latest pending image and all frame callbacks. */
	return 0;
}

/*
 * Commits the pending state of a surface without a shell role now (a
 * cursor, a sub-surface, a surface waiting for its role): the attached
 * image, or the current one again, with its damage, acquire fences and
 * frame callbacks, for the scheduler.
 */
int
kwl_surface_queue(
	struct kwl_object *surface)
{
	struct kwl_object *previous;
	unsigned attached;

	/* The attached image, or the current one kept. */
	attached = surface->attached;
	previous = surface->queued;
	if (surface->attached) {
		surface->queued = surface->pending;
		surface->pending = NULL;
	} else {
		surface->queued = surface->current;
		kwl_buffer_get(surface->queued);
	}

	/* The content, its damage and its acquire fence are committed. */
	surface->attached = 0;
	surface->ready = 1;
	if (surface->queued != NULL)
		surface->queued->busy = 1;
	commit_damage(surface);
	commit_fence(surface, attached);
	kwl_buffer_put(previous);
	append_callbacks(&surface->committed_callbacks, surface->callbacks);
	surface->callbacks = NULL;

	/* Succeeded: the scheduler takes the committed image. */
	return 0;
}

/* Implements the selected fullscreen xdg-shell role and configure lifetime. */
static int
shell_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	struct kwl_object *created;
	struct kwl_object *other;
	const char *text;
	size_t offset;
	uint32_t id;
	uint32_t parent;
	uint32_t positioner;
	uint32_t serial;
	int32_t width;
	int32_t height;
	int error;

	/* The global shell creates one xdg role for an existing role-free surface. */
	if (object->kind == KWL_WM) {
		/*
		 * The binding may retire only when no live xdg_surface was made
		 * from it (xdg-shell's defunct_surfaces, BUG-112): xdg_surfaces of
		 * the client's other bindings (a library's own, a chooser's) do
		 * not keep it.
		 */
		if (opcode == 0 && size == 0) {
			/* Each live role of this binding still depends on it. */
			for (other = object->client->objects; other != NULL; other = other->next) {
				if (other->kind == KWL_XDG_SURFACE &&
				    !other->dead &&
				    other->wm_base == object) {
					error = kwl_error_code(object->client, object->id, WM_ERROR_DEFUNCT_SURFACES, "xdg_surfaces of this binding live");
					return error;
				}
			}

			/* This binding no longer has live shell children. */
			kwl_object_destroy(object);
			return 0;
		}

		/* The answer to a ping (toplevel.c); one to no ping is ignored. */
		if (opcode == 3U && size == 4U) {
			serial = word_at(bytes, 0);
			kwl_ping_pong(object->client, serial);
			return 0;
		}

		/* A positioner, for the popups (popup.c). */
		if (opcode == 1U && size == 4U) {
			id = word_at(bytes, 0);
			error = kwl_positioner_create(object, id);
			if (error != 0)
				return error;
			return 0;
		}

		/* Only get_xdg_surface is left. */
		if (opcode != 2U || size != 8U)
			return EPROTO;

		/* The role cannot be attached to a foreign object or an already assigned surface. */
		id = word_at(bytes, 4);
		surface = kwl_find(object->client, id);
		if (surface == NULL ||
		    surface->kind != KWL_SURFACE ||
		    surface->role != NULL ||
		    surface->sub_role != NULL)
			return EPROTO;

		/* Publish both directions only after the role identity is allocated. */
		id = word_at(bytes, 0);
		created = kwl_create(object->client, id, KWL_XDG_SURFACE, object->version);
		if (created == NULL)
			return EPROTO;

		/* The surface owns no additional memory reference to its protocol role; the role remembers its binding. */
		created->surface = surface;
		created->wm_base = object;
		surface->role = created;
		return 0;
	}

	/* Shell objects cannot operate after their underlying surface disappears. */
	surface = object->surface;
	if (surface == NULL)
		return EPROTO;

	/* xdg_surface owns the configure serial and the sole toplevel child. */
	if (object->kind == KWL_XDG_SURFACE) {
		/* Parent retirement cannot invalidate a surviving toplevel child. */
		if (opcode == 0 && size == 0 && object->top == NULL) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Toplevel creation is one-shot while the child remains alive. */
		if (opcode == 1U && size == 4U && object->top == NULL) {
			/* Allocate the child before publishing either shell backreference. */
			id = word_at(bytes, 0);
			created = kwl_create(object->client, id, KWL_TOPLEVEL, object->version);
			if (created == NULL)
				return EPROTO;

			/* Both shell layers refer to the same core surface. */
			created->surface = surface;
			created->role = object;
			object->top = created;
			return 0;
		}

		/* The popup role (popup.c): its id, a nullable parent xdg_surface and a positioner. */
		if (opcode == 2U && size == 12U) {
			id = word_at(bytes, 0);
			parent = word_at(bytes, 4);
			positioner = word_at(bytes, 8);
			error = kwl_popup_create(object, id, parent, positioner);
			if (error != 0)
				return error;
			return 0;
		}

		/* Window geometry is advisory, but its positive extent must be well formed. */
		if (opcode == 3U && size == 16U) {
			/* Geometry may be advisory, but its extent must remain strictly positive. */
			width = (int32_t)word_at(bytes, 8);
			height = (int32_t)word_at(bytes, 12);
			if (width <= 0 || height <= 0)
				return EPROTO;

			/* The geometry applies from the next commit (popups are placed from it). */
			surface->pending_geometry[0] = (int32_t)word_at(bytes, 0);
			surface->pending_geometry[1] = (int32_t)word_at(bytes, 4);
			surface->pending_geometry[2] = width;
			surface->pending_geometry[3] = height;
			surface->pending_geometry_set = 1;
			return 0;
		}

		/* An acknowledgment names a configure sent to this surface (the latest, or an earlier one). */
		if (opcode == 4U && size == 4U) {
			/* An old or foreign serial cannot authorize new buffer-bearing commits. */
			serial = word_at(bytes, 0);
			if (!surface->configured || serial == 0 || serial > surface->configure_serial)
				return EPROTO;

			/* Selects the decoration snapshot for this exact outstanding configure. */
			error = kwl_decoration_ack(surface, serial);
			if (error != 0)
				return error;

			/* Buffer-bearing commits may now publish this configured surface; a resize's end waits for this serial. */
			surface->acknowledged = 1;
			surface->acked_serial = serial;

			/* The acknowledgment of a resize's configure is timed (BUG-179). */
			if (surface->resized_ms != 0U &&
			    surface->resized_acked_ms == 0U &&
			    serial >= surface->resized_serial)
				surface->resized_acked_ms = kwl_milliseconds();
			return 0;
		}

		/* No other xdg_surface request exists. */
		return EPROTO;
	}

	/* Toplevel methods validate their own payload before applying this fullscreen policy. */
	switch (opcode) {
	case 0:
		/* The child must retire before its xdg_surface parent can be destroyed. */
		if (size != 0)
			return EPROTO;

		/* Retire the child identity without destroying the underlying core surface. */
		kwl_object_destroy(object);
		break;
	case 2:
	case 3:
		/* Titles and application IDs are canonical strings. */
		error = string_at(bytes, size, 0, &text, &offset);
		if (error != 0 || offset != size)
			return EPROTO;

		/* The title is kept for the glass look's title bar (cut to fit); the application ID for the mark's letter. */
		if (opcode == 2U) {
			strncpy(surface->title, text, sizeof(surface->title) - 1U);
			surface->title[sizeof(surface->title) - 1U] = '\0';
			object->client->server->dirty = 1;
		} else {
			strncpy(surface->app_id, text, sizeof(surface->app_id) - 1U);
			surface->app_id[sizeof(surface->app_id) - 1U] = '\0';
			object->client->server->dirty = 1;
		}

		break;
	case 11:
		/* A fullscreen target is one nullable output identity. */
		if (size != 4U)
			return EPROTO;

		/* The default output needs no explicit proxy identity from this client. */
		id = word_at(bytes, 0);
		if (id != 0) {
			/* Explicit targets must refer to this connection's own output binding. */
			other = kwl_find(object->client, id);
			if (other == NULL || other->kind != KWL_OUTPUT)
				return EPROTO;
		}

		/* The window becomes fullscreen: at the origin, the output's size (design D0, D6). */
		error = kwl_window_enter_fullscreen(surface);
		if (error != 0)
			return error;

		break;
	case 12:
		/* Leaving fullscreen has no payload. */
		if (size != 0)
			return EPROTO;

		/* The window returns to its place and size before fullscreen, or is centred (ws035-p138). */
		error = kwl_window_leave_fullscreen(surface);
		if (error != 0)
			return error;

		/* Succeeded: the window left fullscreen. */
		break;
	default:
		/* The requests of the window manager (toplevel.c): parent, window menu, move, resize, limits, maximize, minimize. */
		error = kwl_toplevel_request(object, surface, opcode, bytes, size);
		if (error != 0)
			return error;
		break;
	}

	/* Succeeded: the supported toplevel request is valid for this fullscreen role. */
	return 0;
}


/*
 * Tells the windows of xdg-shell version 4 new bounds when the space for
 * their bodies changed since the bounds were last sent (the glass look
 * given up when the output opened, another output size): each configured
 * window that is not fullscreen or docked hears configure_bounds and a
 * configure (in which it may choose its size again).
 */
void
kwl_window_bounds_refresh(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *top;
	int32_t width;
	int32_t height;
	int error;

	/* Unchanged bounds, or none sent yet: nothing to tell. */
	window_bounds(server, &width, &height);
	if (server->bounds_width == 0 || (width == server->bounds_width && height == server->bounds_height))
		return;
	printf("KWL BOUNDS changed width=%d height=%d was=%dx%d\n", width, height, server->bounds_width, server->bounds_height);
	server->bounds_width = width;
	server->bounds_height = height;

	/* Each configured window of version 4 that chooses its own size. */
	for (client = server->clients; client != NULL; client = client->next) {
		/* A failed client hears nothing. */
		if (client->fatal)
			continue;

		/* Each of its windows. */
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a configured toplevel's surface. */
			if (surface->kind != KWL_SURFACE || surface->dead || surface->role == NULL || !surface->configured)
				continue;
			top = surface->role->top;
			if (top == NULL || top->kind != KWL_TOPLEVEL || top->version < 4U)
				continue;

			/* Not a fullscreen or a docked one (their size is the compositor's). */
			if (surface->fullscreen || surface->maximized)
				continue;

			/* The bounds and a configure; a client that cannot take them is failed at its next request. */
			error = kwl_window_send_configure(surface);
			if (error != 0)
				printf("KWL BOUNDS configure errno=%d\n", error);
		}
	}
}

/*
 * Makes a window fullscreen, when the client asks or the compositor decides
 * (the top-right corner's swipe, corner.c): it covers the output from the
 * origin, keeping its place and size to come back to, and a window already
 * configured is told now.  A fullscreen window is left as it is.  Returns 0,
 * or the error of sending the configure.
 */
int
kwl_window_enter_fullscreen(
	struct kwl_object *surface)
{
	struct kwl_server *server;
	uint32_t sent_width;
	uint32_t sent_height;
	uint32_t width;
	uint32_t height;
	int error;

	/* Already fullscreen: nothing changes. */
	if (surface->fullscreen)
		return 0;

	/* A window on a head goes fullscreen on the anchor (ws113-p007): it is carried there first. */
	server = surface->client->server;
	if (surface->output != KWL_PLANE_ANCHOR)
		kwl_window_to_output(server, surface, KWL_PLANE_ANCHOR, "fullscreen");

	/* Its place and its image's size before fullscreen, to come back to. */
	sent_width = surface->window_width;
	sent_height = surface->window_height;
	surface->fullscreen = 1;
	surface->window_x = surface->x;
	surface->window_y = surface->y;
	width = 0U;
	height = 0U;
	if (surface->current != NULL)
		kwl_decoration_geometry(surface, &width, &height);

	/*
	 * An image still of the output's size is the last fullscreen one: the
	 * window left fullscreen and has not drawn the size it was sent yet
	 * (T1-235: Esc, then Alt+Enter within a second, came back at the
	 * output's size).  The size it was sent is kept instead.
	 */
	if (width == server->width && height == server->height && sent_width != 0U && sent_height != 0U) {
		width = sent_width;
		height = sent_height;
	}

	/* The size to come back to. */
	surface->window_width = width;
	surface->window_height = height;

	/*
	 * A docked window is not docked while it is fullscreen: it covers the
	 * output, not the space under the system bar, and keeps its floating
	 * place to come back to (BUG-208); leaving fullscreen follows the
	 * session's layout mode (ws142-p008).  A dock still being animated ends
	 * here.
	 */
	if (surface->maximized) {
		surface->fullscreen_docked = 1;
		surface->maximized = 0;
		if (server->anim == surface)
			server->anim = NULL;
	}

	/* It covers the output from the origin. */
	surface->x = 0;
	surface->y = 0;
	server->dirty = 1;

	/* A window not configured yet learns it from its first configure. */
	if (!surface->configured)
		return 0;

	/* A window already configured is told now. */
	error = kwl_window_send_configure(surface);
	if (error != 0)
		return error;

	/* Succeeded: the window is fullscreen and knows it. */
	return 0;
}

/*
 * Takes a window out of fullscreen (the client asks, xdg_toplevel's
 * unset_fullscreen): back to the place and size it had as a window, kept
 * inside the space so that its title bar is not under the system bar; a
 * window that started fullscreen and was never placed is centred in the
 * space, now at its fullscreen size and again at its first image of
 * another size (ws035-p138, BUG-114).  A window already configured is told.
 * Returns 0, or the error of sending the configure.
 */
int
kwl_window_leave_fullscreen(
	struct kwl_object *surface)
{
	struct kwl_server *server;
	int docks;
	int error;

	/* Not fullscreen: nothing changes. */
	if (!surface->fullscreen)
		return 0;

	/*
	 * Docked when the session's layout mode is docked (shell.c, ws142-p008;
	 * it replaces BUG-208's memory of the window's own state).  Otherwise a
	 * window that was docked before comes back to its floating place, and
	 * any other to its place, or the space's centre when it never had one.
	 */
	server = surface->client->server;
	surface->fullscreen = 0;
	docks = kwl_glass_unfullscreen_docks(server, surface);
	if (docks) {
		surface->fullscreen_docked = 0;
	} else if (surface->fullscreen_docked) {
		surface->fullscreen_docked = 0;
		surface->x = surface->restore_x;
		surface->y = surface->restore_y;
		surface->window_width = surface->restore_width;
		surface->window_height = surface->restore_height;
		kwl_glass_fit(server, (int32_t)surface->window_width, (int32_t)surface->window_height, &surface->x, &surface->y);
	} else if (surface->placed) {
		surface->x = surface->window_x;
		surface->y = surface->window_y;
		if (server->glass && surface->window_width != 0U)
			kwl_glass_fit(server, (int32_t)surface->window_width, (int32_t)surface->window_height, &surface->x, &surface->y);
	} else {
		kwl_window_centre(server, surface);
		surface->place_pending = 1;
	}

	/* The output is drawn again; the log names where the window went. */
	server->dirty = 1;
	printf("KWL WINDOW unfullscreen surface=%u x=%d y=%d placed=%u docked=%u client=%llu\n", surface->id, surface->x, surface->y, surface->placed, surface->maximized, (unsigned long long)surface->client->number);

	/* A window not configured yet learns it from its first configure. */
	if (!surface->configured)
		return 0;

	/* A window already configured is told now. */
	error = kwl_window_send_configure(surface);
	if (error != 0)
		return error;

	/* Succeeded: the window is a window again. */
	return 0;
}

/*
 * Sends a toplevel's configure: the output's size and the fullscreen and
 * activated states for a fullscreen window; otherwise its size before
 * fullscreen, or 0x0 (the client chooses), and activated.  The xdg_surface
 * configure with a new serial follows.
 */
int
kwl_window_send_configure(
	struct kwl_object *surface)
{
	struct kwl_server *server;
	struct kwl_object *role;
	uint32_t configure[5];
	size_t size;
	unsigned resizing;
	int error;

	/* The size and states; a window being resized (toplevel.c) has the resizing state too. */
	server = surface->client->server;
	role = surface->role;
	resizing = kwl_toplevel_resizing(server, surface);
	if (surface->fullscreen) {
		configure[0] = server->width;
		configure[1] = server->height;
		configure[2] = 8;
		configure[3] = 2;
		configure[4] = 4;
		size = 5U * sizeof(uint32_t);
	} else if (surface->maximized) {
		configure[0] = surface->window_width;
		configure[1] = surface->window_height;
		configure[2] = 8;
		configure[3] = 1;
		configure[4] = 4;
		size = 5U * sizeof(uint32_t);
	} else if (resizing) {
		configure[0] = surface->window_width;
		configure[1] = surface->window_height;
		configure[2] = 8;
		configure[3] = 3;
		configure[4] = 4;
		size = 5U * sizeof(uint32_t);
	} else {
		configure[0] = surface->window_width;
		configure[1] = surface->window_height;
		configure[2] = 4;
		configure[3] = 4;
		size = 4U * sizeof(uint32_t);
	}

	/* A window of xdg-shell version 4 learns first how large it may make itself. */
	if (!surface->fullscreen && !surface->maximized && role->top->version >= 4U) {
		error = send_bounds(surface);
		if (error != 0)
			return error;
	}

	/* The toplevel configure. */
	error = kwl_emit(surface->client, role->top->id, 0, configure, size);
	if (error != 0)
		return error;

	/* Nonzero serials distinguish an acknowledged configure from none. */
	server->serial++;
	if (server->serial == 0)
		server->serial++;

	/* The xdg_surface configure commits the toplevel state. */
	surface->configure_serial = server->serial;

	/* Retains the decoration ownership associated with this configure serial. */
	error = kwl_decoration_configure(surface, surface->configure_serial);
	if (error != 0)
		return error;

	/* Publishes the serial after its associated state has a durable owner. */
	error = kwl_emit(surface->client, role->id, 0, &surface->configure_serial, 4);
	if (error != 0)
		return error;

	/* Succeeded. */
	printf("KWL CONFIGURE client=%llu surface=%u serial=%u width=%u height=%u fullscreen=%u\n", (unsigned long long)surface->client->number, surface->id, surface->configure_serial, configure[0], configure[1], surface->fullscreen);
	return 0;
}

/*
 * Sends a window its bounds (xdg_toplevel.configure_bounds, version 4): the
 * largest size it should choose for itself, the space for window bodies in
 * the glass look (under the system bar and a floating title bar, shell.c)
 * and the output otherwise.  A client that chooses its own size keeps within
 * it, so that its window is seen whole.
 */
static int
send_bounds(
	struct kwl_object *surface)
{
	struct kwl_server *server;
	int32_t width;
	int32_t height;
	int32_t bounds[2];
	int error;

	/* The space the look leaves for a body. */
	server = surface->client->server;
	window_bounds(server, &width, &height);

	/* The width and the height, in that order. */
	bounds[0] = width;
	bounds[1] = height;
	error = kwl_emit(surface->client, surface->role->top->id, 2, bounds, sizeof(bounds));
	if (error != 0)
		return error;

	/* The bounds the windows know now. */
	server->bounds_width = width;
	server->bounds_height = height;

	/* Succeeded. */
	printf("KWL BOUNDS client=%llu surface=%u width=%d height=%d\n", (unsigned long long)surface->client->number, surface->id, width, height);
	return 0;
}

/* Gives the space the look leaves for a window's body: under the system bar and a floating title bar in the glass look, the output otherwise. */
static void
window_bounds(
	struct kwl_server *server,
	int32_t *width,
	int32_t *height)
{
	/* The output. */
	*width = (int32_t)server->width;
	*height = (int32_t)server->height;

	/* Less the glass look's bars and margins (shell.c). */
	if (server->glass)
		kwl_glass_space(server, width, height);
}

/* Adds a rectangle to a surface's pending damage (their bounding box). */
static void
add_damage(
	struct kwl_object *surface,
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
 * Moves a commit's acquire fences to the queued image.  A commit that
 * attached a buffer replaces the queued fences (with none when it gave
 * none); one that did not keeps the fences of the image it reuses.
 */
static void
commit_fence(
	struct kwl_object *surface,
	unsigned attached)
{
	unsigned index;

	/* Give libkeiland-backend the attached GPU buffer before moving commit fences. */
	if (attached &&
	    surface->queued != NULL &&
	    surface->queued->import != NULL &&
	    surface->queued->shm == NULL)
		kl_backend_gpu_commit(kwl_gpu_host(), kwl_gpu_resource(surface), kwl_gpu_resource(surface->queued));

	/* The reused image still waits for its own fences. */
	if (!attached && surface->acquire_count == 0)
		return;

	/* The replaced image's fences are no longer waited for. */
	for (index = 0; index < surface->fence_count; index++)
		close(surface->fences[index].fd);

	/* The commit's fences are the queued image's. */
	for (index = 0; index < surface->acquire_count; index++)
		surface->fences[index] = surface->acquire[index];
	surface->fence_count = surface->acquire_count;
	surface->acquire_count = 0;
	surface->fence_ms = kwl_milliseconds();
	surface->fence_waited = 0;
}

/*
 * Moves a commit's damage to the committed damage, joined with any not yet
 * copied (two commits may come before one copy).
 */
static void
commit_damage(
	struct kwl_object *surface)
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
	struct kwl_object **list,
	struct kwl_object *callbacks)
{
	/* Each existing callback must complete before later callbacks in the same surface stream. */
	while (*list != NULL)
		list = &(*list)->callback_next;

	/* Ownership of the supplied chain moves to this surface state. */
	*list = callbacks;

	/* Succeeded: the surface state owns the ordered callback chain. */
	return;
}

/*
 * Binds a head's wl_output (ws113-p004b), or an inert one for a head that
 * closed while the client bound it (the global was removed after the
 * client last read the registry).  Returns 0, or EPROTO for another
 * interface or version.
 */
static int
bind_head_output(
	struct kwl_object *registry,
	uint32_t name,
	const char *interface,
	uint32_t version,
	uint32_t id)
{
	struct kwl_object *object;
	uint32_t head;
	int same;
	int error;

	/* Only wl_output, at a version a head offers. */
	same = strcmp(interface, "wl_output");
	if (same != 0 ||
	    version == 0U ||
	    version > OUTPUT_HEAD_VERSION)
		return EPROTO;

	/* One binding, of the head or inert. */
	object = kwl_create(registry->client, id, KWL_OUTPUT, version);
	if (object == NULL)
		return EPROTO;
	head = kwl_output_head_of_global(registry->client->server, name);
	object->output_head = KWL_OUTPUT_GONE;
	if (head != 0U)
		object->output_head = head;

	/* The binding of an open head learns its display. */
	error = output_events(object);
	if (error != 0)
		return error;

	/* Its client's surfaces already on the display name it too (ws177-p001). */
	kwl_surface_outputs_bound(object);

	/* Succeeded: the binding exists. */
	return 0;
}

/* Sends a registry one global event: the name, the interface and the version. */
static int
registry_global(
	struct kwl_object *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	unsigned char payload[128];
	size_t length;
	size_t offset;
	uint32_t word;
	int error;

	/* The name, the interface string in the wire's padded form, the version. */
	memset(payload, 0, sizeof(payload));
	memcpy(payload, &name, 4);
	length = strlen(interface) + 1U;
	word = (uint32_t)length;
	memcpy(payload + 4, &word, 4);
	memcpy(payload + 8, interface, length);
	offset = 8U + ((length + 3U) & ~(size_t)3U);
	memcpy(payload + offset, &version, 4);
	error = kwl_emit(registry->client, registry->id, 0, payload, offset + 4U);
	if (error != 0)
		return error;

	/* Succeeded: the registry knows of the global. */
	return 0;
}
