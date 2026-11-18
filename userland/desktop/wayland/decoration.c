/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Negotiates each toplevel's decoration ownership. Configure snapshots carry
 * mode ownership through ack and surface commit.
 *
 * The compositor decorates every toplevel (SSD) unless its client declares
 * its own decoration (2026-10-03 user, ws114-p008): with xdg-decoration's
 * client_side, with KDE's org_kde_kwin_server_decoration request_mode
 * CLIENT or NONE, by destroying its xdg-decoration object, or by binding
 * KDE's manager and making no decoration object for the window (KDE's
 * protocol decorates only through objects; GTK4 declares its own
 * decoration so, gdktoplevel-wayland.c set_decorated).  An xdg-decoration
 * object's choice comes first, then KDE's object; a native kl_titlebar
 * is the compositor's decoration as well.  A client that uses neither protocol and draws its own frame is
 * not told apart (the user's decision); a fullscreen window is drawn
 * without the decoration (shell.c).
 */

#include "extras.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The manager and decoration wire requests, and the decoration configure event. */
#define MANAGER_DESTROY 0U
#define MANAGER_GET_TOPLEVEL_DECORATION 1U
#define DECORATION_DESTROY 0U
#define DECORATION_SET_MODE 1U
#define DECORATION_UNSET_MODE 2U
#define DECORATION_CONFIGURE 0U

/* KDE's server decoration: the manager's create and default_mode, the decoration's release, request_mode and mode. */
#define KDE_MANAGER_CREATE 0U
#define KDE_MANAGER_DEFAULT_MODE 0U
#define KDE_DECORATION_RELEASE 0U
#define KDE_DECORATION_REQUEST_MODE 1U
#define KDE_DECORATION_MODE 0U

/* KDE's modes: no decoration at all, the client's, the compositor's. */
#define KDE_MODE_NONE 0U
#define KDE_MODE_CLIENT 1U
#define KDE_MODE_SERVER 2U

/* The ownership modes specified by xdg-decoration v1. */
#define MODE_CLIENT_SIDE 1U
#define MODE_SERVER_SIDE 2U

/* Protocol errors distinguish already-mapped, duplicate, orphaned and invalid requests. */
#define DECORATION_ERROR_UNCONFIGURED_BUFFER 0U
#define DECORATION_ERROR_ALREADY_CONSTRUCTED 1U
#define DECORATION_ERROR_ORPHANED 2U
#define DECORATION_ERROR_INVALID_MODE 3U

/*
 * One outstanding xdg_surface configure's decoration ownership. The
 * toplevel owns this list; an acknowledgment retires the acknowledged and
 * older snapshots, and toplevel teardown frees everything still pending.
 */
struct kwl_decoration_configure {
	struct kwl_decoration_configure *next;
	uint32_t serial;
	uint32_t mode;
	uint64_t generation;
};

static int decoration_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int kde_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
static int kde_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static uint32_t decoration_wanted(const struct kwl_object *toplevel);
static int decoration_propose(struct kwl_object *toplevel);
static int decoration_answer(struct kwl_object *decoration);
static struct kwl_object *decoration_top(const struct kwl_object *surface);
static void decoration_reset(struct kwl_object *toplevel);
static void decoration_invalidate(struct kwl_object *toplevel);
static void decoration_free(struct kwl_object *toplevel);
static uint32_t decoration_word(const unsigned char *bytes, size_t offset);

/*
 * Dispatches decoration requests while keeping preferred and visible modes apart.
 */
int
kwl_decoration_request(
    struct kwl_object *object,
    uint32_t opcode,
	const unsigned char *bytes,
    size_t size)
{
	struct kwl_object *toplevel;
	uint32_t mode;
	int error;

	/* KDE's server decoration has requests of its own. */
	if (object->kind == KWL_KDE_DECORATION_MANAGER || object->kind == KWL_KDE_DECORATION) {
		error = kde_request(object, opcode, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the KDE request was carried out. */
		return 0;
	}

	/* The manager either retires or associates one decoration with a toplevel. */
	if (object->kind == KWL_DECORATION_MANAGER) {
		if (opcode == MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(object);

			/* Succeeded: existing decoration objects retain their ownership. */
			return 0;
		}

		/* Rejects every other manager operation. */
		if (opcode != MANAGER_GET_TOPLEVEL_DECORATION)
			return EPROTO;

		/* Associates and configures the requested decoration. */
		error = decoration_create(object, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the new decoration has an initial SSD proposal. */
		return 0;
	}

	/* A valid destructor withdraws this decoration at the next surface commit. */
	if (opcode == DECORATION_DESTROY) {
		if (size != 0U)
			return EPROTO;

		/* Detaches the object and stages client ownership. */
		kwl_object_destroy(object);

		/* Succeeded: the toplevel remains alive. */
		return 0;
	}

	/* An orphaned decoration cannot configure a window that has gone. */
	toplevel = object->decoration_toplevel;
	if (toplevel == NULL) {
		error = kwl_error_code(object->client, object->id, DECORATION_ERROR_ORPHANED, "the toplevel is gone");
		if (error != 0)
			return error;

		/* Refuses state changes on the orphaned object. */
		return EPROTO;
	}

	/* A preferred mode must be a complete and recognized protocol word. */
	mode = 0U;
	if (opcode == DECORATION_SET_MODE) {
		if (size != 4U)
			return EPROTO;

		/* Decodes the client's decoration ownership preference. */
		mode = decoration_word(bytes, 0U);
		if (mode != MODE_CLIENT_SIDE && mode != MODE_SERVER_SIDE) {
			error = kwl_error_code(object->client, object->id, DECORATION_ERROR_INVALID_MODE, "not a decoration mode");
			if (error != 0)
				return error;

			/* Refuses modes outside the protocol enumeration. */
			return EPROTO;
		}
	} else if (opcode == DECORATION_UNSET_MODE) {
		/* An unset preference carries no payload and leaves the compositor's default, SSD. */
		if (size != 0U)
			return EPROTO;
	} else {
		/* Refuses unknown decoration operations. */
		return EPROTO;
	}

	/* Publishes a new proposal without discarding an already acknowledged mode. */
	toplevel->decoration_preferred = mode;
	printf("KWL DECORATION asked client=%llu mode=%u\n", (unsigned long long)object->client->number, mode);

	/* Sends decoration configure before the corresponding xdg_surface configure. */
	error = decoration_answer(object);
	if (error != 0)
		return error;

	/* Succeeded: the proposal waits for acknowledgment and surface commit. */
	return 0;
}

/*
 * Detaches decoration identities and frees toplevel-owned configure snapshots.
 */
void
kwl_decoration_object_gone(
    struct kwl_object *object)
{
	struct kwl_object *toplevel;
	struct kwl_object *surface;

	/* A destroyed decoration withdraws SSD at the next commit of its surviving window. */
	if (object->kind == KWL_DECORATION) {
		toplevel = object->decoration_toplevel;
		if (toplevel != NULL) {
			toplevel->decoration = NULL;

			/* Invalidates outstanding proposals; the window keeps the client's decoration from now. */
			decoration_reset(toplevel);
			toplevel->decoration_withdrawn = 1;
		}

		/* Removes the retired object's link to its surviving toplevel. */
		object->decoration_toplevel = NULL;

		/* The surviving toplevel owns its pending mode and snapshots. */
		return;
	}

	/* A released KDE decoration leaves its surviving window to the compositor's default. */
	if (object->kind == KWL_KDE_DECORATION) {
		surface = object->kde_surface;
		object->kde_surface = NULL;
		if (surface != NULL && surface->kde_decoration == object) {
			surface->kde_decoration = NULL;

			/* The window's mode follows the default again (a failure is the next configure's). */
			toplevel = decoration_top(surface);
			if (toplevel != NULL)
				(void)decoration_propose(toplevel);
		}

		/* The surface no longer names it. */
		return;
	}

	/* A surface that goes leaves its KDE decoration orphaned. */
	if (object->kind == KWL_SURFACE) {
		if (object->kde_decoration != NULL)
			object->kde_decoration->kde_surface = NULL;
		object->kde_decoration = NULL;

		/* The surface's toplevel is retired on its own. */
		return;
	}

	/* A toplevel's pending configuration history dies with its identity. */
	if (object->kind == KWL_TOPLEVEL) {
		decoration_free(object);
		if (object->decoration != NULL)
			object->decoration->decoration_toplevel = NULL;

		/* Removes the retired toplevel's association. */
		object->decoration = NULL;
	}

	/* No decoration identity names the retired object. */
	return;
}

/*
 * Captures the decoration proposal belonging to one xdg_surface configure serial.
 */
int
kwl_decoration_configure(
    struct kwl_object *surface,
    uint32_t serial)
{
	struct kwl_decoration_configure *snapshot;
	struct kwl_decoration_configure **link;
	struct kwl_object *toplevel;

	/* Popups and surfaces without a toplevel have no decoration negotiation. */
	toplevel = decoration_top(surface);
	if (toplevel == NULL)
		return 0;

	/* Each serial preserves its own mode even if a newer proposal follows. */
	snapshot = calloc(1, sizeof(*snapshot));
	if (snapshot == NULL)
		return ENOMEM;

	/* The mode this configure offers: the one the declarations (or the default) give now. */
	toplevel->decoration_configured = decoration_wanted(toplevel);

	/* Initializes this toplevel-owned record before exposing it through the list. */
	snapshot->serial = serial;
	snapshot->mode = toplevel->decoration_configured;

	/* Records which decoration identity proposed this ownership. */
	snapshot->generation = toplevel->decoration_generation;

	/* Appends in configure order so an acknowledgment can retire its prefix. */
	link = &toplevel->decoration_configures;
	while (*link != NULL)
		link = &(*link)->next;

	/* Publishes the initialized record at the end of the history. */
	*link = snapshot;

	/* Succeeded: this serial identifies a stable decoration proposal. */
	return 0;
}

/*
 * Stages the mode from an acknowledged configure and retires older proposals.
 */
int
kwl_decoration_ack(
    struct kwl_object *surface,
    uint32_t serial)
{
	struct kwl_decoration_configure *snapshot;
	struct kwl_decoration_configure *retired;
	struct kwl_decoration_configure *after;
	struct kwl_object *toplevel;
	uint32_t mode;
	uint64_t generation;

	/* Popup acknowledgment follows its independent placement protocol. */
	toplevel = decoration_top(surface);
	if (toplevel == NULL)
		return 0;

	/* Locates the exact outstanding serial, never a foreign or consumed one. */
	snapshot = toplevel->decoration_configures;
	while (snapshot != NULL) {
		if (snapshot->serial == serial)
			break;

		/* Continues through still-outstanding proposals. */
		snapshot = snapshot->next;
	}

	/* An unknown or previously consumed configure cannot select a decoration mode. */
	if (snapshot == NULL)
		return EPROTO;

	/* Preserves the selected proposal before removing the consumed prefix. */
	mode = snapshot->mode;
	generation = snapshot->generation;

	/* Older generations remain valid xdg acknowledgments but cannot resurrect retired decorations. */
	if (generation == toplevel->decoration_generation) {
		toplevel->decoration_acked_mode = mode;
		toplevel->decoration_acked = 1;
	}

	/* Frees the acknowledged configure and every superseded predecessor. */
	after = snapshot->next;
	while (toplevel->decoration_configures != after) {
		retired = toplevel->decoration_configures;
		toplevel->decoration_configures = retired->next;
		free(retired);
	}

	/* Succeeded: the matching proposal waits for the next surface commit. */
	return 0;
}

/*
 * Applies acknowledged decoration ownership with the surface's committed state.
 */
void
kwl_decoration_commit(
    struct kwl_object *surface)
{
	struct kwl_object *toplevel;
	uint32_t mode;

	/* Only toplevels carry decoration ownership. */
	toplevel = decoration_top(surface);
	if (toplevel == NULL)
		return;

	/* A destroyed decoration withdraws SSD on this commit without a new configure. */
	mode = toplevel->decoration_committed;
	if (toplevel->decoration_reset) {
		mode = MODE_CLIENT_SIDE;
		toplevel->decoration_reset = 0;
		toplevel->decoration_acked = 0;
	} else if (toplevel->decoration_acked) {
		/* The acknowledged mode joins the content produced for that configure. */
		mode = toplevel->decoration_acked_mode;
		toplevel->decoration_acked = 0;
	}

	/* Every renderer and hit test observes the same committed ownership. */
	if (mode != toplevel->decoration_committed) {
		toplevel->decoration_committed = mode;
		surface->client->server->dirty = 1;
		printf("KWL DECORATION applied client=%llu surface=%u mode=%u\n", (unsigned long long)surface->client->number, surface->id, mode);
	}

	/* Succeeded: drawing and hit testing follow the committed content. */
	return;
}

/*
 * Reports whether the compositor owns the committed decoration of a surface.
 */
int
kwl_decoration_server(
	const struct kwl_object *surface)
{
	struct kwl_object *toplevel;

	/* Surfaces outside a toplevel's role have no server decoration. */
	toplevel = decoration_top(surface);
	if (toplevel == NULL)
		return 0;

	/* Only a committed SSD is the compositor's (nothing is committed before the first image). */
	if (toplevel->decoration_committed != MODE_SERVER_SIDE)
		return 0;

	/* Succeeded: this surface committed an SSD proposal. */
	return 1;
}

/*
 * Proposes the decoration again when a native titlebar is created or removed.
 *
 * A titlebar is the compositor's decoration, as the default is, so the mode
 * changes only when another declaration says otherwise.
 */
int
kwl_decoration_native_changed(
    struct kwl_object *toplevel)
{
	int error;

	/* The declarations decide the mode; an unchanged one sends nothing. */
	error = decoration_propose(toplevel);
	if (error != 0)
		return error;

	/* Succeeded: a changed mode waits for the client's configure acknowledgment. */
	return 0;
}

/*
 * Tells a new binding of KDE's server decoration manager the default mode,
 * the compositor's decoration.
 */
int
kwl_decoration_kde_bind(
	struct kwl_object *manager)
{
	uint32_t mode;
	int error;

	/*
	 * From now on the client's windows are decorated only through a
	 * decoration object, as KDE's protocol has it (decoration_wanted).
	 */
	manager->client->kde_bound = 1;

	/* The default a new decoration object has until the client asks for another. */
	mode = KDE_MODE_SERVER;
	error = kwl_emit(manager->client, manager->id, KDE_MANAGER_DEFAULT_MODE, &mode, sizeof(mode));
	if (error != 0)
		return error;

	/* Succeeded: the client knows the default. */
	return 0;
}

/*
 * Reports the xdg window geometry used for configure sizes and state restoration.
 */
void
kwl_decoration_geometry(
	const struct kwl_object *surface,
    uint32_t *width,
    uint32_t *height)
{
	/* Starts with the buffer or viewport extent for windows without geometry. */
	kwl_surface_size(surface, width, height);

	/* Client shadows lie outside the extent specified by xdg_surface geometry. */
	if (surface->geometry_set) {
		*width = (uint32_t)surface->geometry[2];
		*height = (uint32_t)surface->geometry[3];
	}

	/* Succeeded: configure sizes exclude client decoration shadows. */
	return;
}

/* Creates one decoration before the first committed buffer and offers CSD ownership. */
static int
decoration_create(
    struct kwl_object *manager,
	const unsigned char *bytes,
    size_t size)
{
	struct kwl_object *toplevel;
	struct kwl_object *created;
	struct kwl_object *surface;
	uint32_t id;
	uint32_t toplevel_id;
	int error;

	/* The association contains exactly the new ID and its toplevel ID. */
	if (size != 8U)
		return EPROTO;

	/* Decodes each wire identity before consulting object ownership. */
	id = decoration_word(bytes, 0U);
	toplevel_id = decoration_word(bytes, 4U);
	toplevel = kwl_find(manager->client, toplevel_id);
	if (toplevel == NULL)
		return EPROTO;

	/* Refuses roles which cannot own toplevel decoration. */
	if (toplevel->kind != KWL_TOPLEVEL)
		return EPROTO;

	/* The protocol permits exactly one decoration object on this toplevel. */
	if (toplevel->decoration != NULL) {
		error = kwl_error_code(manager->client, manager->id, DECORATION_ERROR_ALREADY_CONSTRUCTED, "the toplevel has a decoration");
		if (error != 0)
			return error;

		/* Refuses a second owner for the same toplevel. */
		return EPROTO;
	}

	/* Version one requires decoration negotiation before buffer content was committed. */
	surface = toplevel->surface;
	if (surface != NULL) {
		if (surface->current != NULL ||
		    surface->queued != NULL ||
		    surface->pending != NULL) {
			error = kwl_error_code(manager->client, manager->id, DECORATION_ERROR_UNCONFIGURED_BUFFER, "the toplevel already has content");
			if (error != 0)
				return error;

			/* Refuses the version-one association after window content exists. */
			return EPROTO;
		}
	}

	/* The object registry retains the associated decoration until its destructor. */
	created = kwl_create(manager->client, id, KWL_DECORATION, manager->version);
	if (created == NULL)
		return EPROTO;

	/* Both directions are detached when either identity is retired. */
	created->decoration_toplevel = toplevel;
	toplevel->decoration = created;
	toplevel->decoration_preferred = 0U;
	toplevel->decoration_withdrawn = 0;
	decoration_invalidate(toplevel);
	toplevel->decoration_acked = 0;
	toplevel->decoration_reset = 0;

	/* Publishes the initial ownership proposal (the compositor's until the client asks otherwise). */
	error = decoration_answer(created);
	if (error != 0)
		return error;

	/* Succeeded: ownership now follows the decoration protocol. */
	return 0;
}

/* Offers the preferred mode and sends its corresponding surface configure. */
static int
decoration_answer(
    struct kwl_object *decoration)
{
	struct kwl_object *toplevel;
	struct kwl_object *surface;
	uint32_t mode;
	int error;

	/* An orphan has no ownership proposal to publish. */
	toplevel = decoration->decoration_toplevel;
	if (toplevel == NULL)
		return EPROTO;

	/* The object's preference, or the compositor's default when it has none. */
	mode = decoration_wanted(toplevel);

	/* Retains the offered ownership for the next surface configure. */
	toplevel->decoration_configured = mode;

	/* Decoration configure precedes the matching xdg_surface configure. */
	error = kwl_emit(decoration->client, decoration->id, DECORATION_CONFIGURE, &mode, sizeof(mode));
	if (error != 0)
		return error;

	/* Records the mode actually sent on the decoration wire. */
	printf("KWL DECORATION configure client=%llu mode=%u\n", (unsigned long long)decoration->client->number, mode);

	/* The first empty surface commit sends the initial xdg configure. */
	surface = toplevel->surface;
	if (surface == NULL)
		return 0;

	/* Leaves initial configuration to the first empty commit. */
	if (!surface->configured)
		return 0;

	/* Subsequent proposals receive their own acknowledgment serial. */
	error = kwl_window_send_configure(surface);
	if (error != 0)
		return error;

	/* Succeeded: the client knows the offered ownership and its configure boundary. */
	return 0;
}

/* Carries out a request of KDE's server decoration manager or of one of its decorations. */
static int
kde_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *toplevel;
	uint32_t mode;
	int error;

	/* The manager only creates decorations. */
	if (object->kind == KWL_KDE_DECORATION_MANAGER) {
		if (opcode != KDE_MANAGER_CREATE)
			return EPROTO;

		/* A decoration for a surface. */
		error = kde_create(object, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the surface's decoration is the client's to choose. */
		return 0;
	}

	/* The release ends the decoration; its window returns to the default. */
	if (opcode == KDE_DECORATION_RELEASE) {
		if (size != 0U)
			return EPROTO;

		/* Detaches it from its surface (kwl_decoration_object_gone). */
		kwl_object_destroy(object);

		/* Succeeded: the surface survives. */
		return 0;
	}

	/* Only request_mode is left, with one known mode. */
	if (opcode != KDE_DECORATION_REQUEST_MODE || size != 4U)
		return EPROTO;
	mode = decoration_word(bytes, 0U);
	if (mode > KDE_MODE_SERVER)
		return EPROTO;

	/* The client's choice, answered with the mode event as KDE's protocol does. */
	object->kde_mode = mode;
	error = kwl_emit(object->client, object->id, KDE_DECORATION_MODE, &mode, sizeof(mode));
	if (error != 0)
		return error;
	printf("KWL DECORATION kde client=%llu mode=%u\n", (unsigned long long)object->client->number, mode);

	/* A window of an orphaned decoration, or one without a toplevel yet, takes the mode at its first configure. */
	if (object->kde_surface == NULL)
		return 0;
	toplevel = decoration_top(object->kde_surface);
	if (toplevel == NULL)
		return 0;

	/* The window's decoration follows the declaration with its next configure. */
	error = decoration_propose(toplevel);
	if (error != 0)
		return error;

	/* Succeeded: the new mode waits for the client's acknowledgment and commit. */
	return 0;
}

/* Creates KDE's server decoration of a surface; it starts in the compositor's mode. */
static int
kde_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	struct kwl_object *surface;
	struct kwl_object *toplevel;
	uint32_t id;
	uint32_t surface_id;
	uint32_t mode;
	int error;

	/* The new ID and the surface. */
	if (size != 8U)
		return EPROTO;
	id = decoration_word(bytes, 0U);
	surface_id = decoration_word(bytes, 4U);
	surface = kwl_find(manager->client, surface_id);
	if (surface == NULL || surface->kind != KWL_SURFACE)
		return EPROTO;

	/* One decoration per surface. */
	if (surface->kde_decoration != NULL)
		return EPROTO;

	/* The object, tied to its surface from both ends. */
	created = kwl_create(manager->client, id, KWL_KDE_DECORATION, manager->version);
	if (created == NULL)
		return EPROTO;
	created->kde_surface = surface;
	created->kde_mode = KDE_MODE_SERVER;
	surface->kde_decoration = created;

	/* The mode it starts in. */
	mode = KDE_MODE_SERVER;
	error = kwl_emit(created->client, created->id, KDE_DECORATION_MODE, &mode, sizeof(mode));
	if (error != 0)
		return error;

	/* A window already shown keeps its mode: the compositor's is the default anyway. */
	toplevel = decoration_top(surface);
	if (toplevel == NULL)
		return 0;
	error = decoration_propose(toplevel);
	if (error != 0)
		return error;

	/* Succeeded: the surface's decoration is declared from now. */
	return 0;
}

/* Tells the mode the declarations give a toplevel: the client's when it declared so, the compositor's otherwise. */
static uint32_t
decoration_wanted(
	const struct kwl_object *toplevel)
{
	const struct kwl_object *kde;

	/* An xdg-decoration object's choice comes first: client_side, else (server_side or unset) SSD. */
	if (toplevel->decoration != NULL) {
		if (toplevel->decoration_preferred == MODE_CLIENT_SIDE)
			return MODE_CLIENT_SIDE;
		return MODE_SERVER_SIDE;
	}

	/* A destroyed xdg-decoration object leaves the client's decoration. */
	if (toplevel->decoration_withdrawn)
		return MODE_CLIENT_SIDE;

	/* KDE's: the compositor's only when the client keeps or asks for it (CLIENT and NONE are the client's). */
	kde = NULL;
	if (toplevel->surface != NULL)
		kde = toplevel->surface->kde_decoration;
	if (kde != NULL) {
		if (kde->kde_mode == KDE_MODE_SERVER)
			return MODE_SERVER_SIDE;
		return MODE_CLIENT_SIDE;
	}

	/* A native titlebar is the compositor's decoration. */
	if (toplevel->titlebar != NULL)
		return MODE_SERVER_SIDE;

	/* A client that speaks KDE's protocol and made no object for this window decorates it itself (GTK4). */
	if (toplevel->client->kde_bound)
		return MODE_CLIENT_SIDE;

	/* Succeeded: no declaration at all: the compositor's decoration. */
	return MODE_SERVER_SIDE;
}

/* Offers a toplevel the mode its declarations give now, when it differs from the one offered last. */
static int
decoration_propose(
	struct kwl_object *toplevel)
{
	struct kwl_object *surface;
	uint32_t mode;
	int error;

	/* An xdg-decoration object proposes through its own configure event (decoration_answer). */
	if (toplevel->decoration != NULL)
		return 0;

	/* An unchanged mode needs no new configure. */
	mode = decoration_wanted(toplevel);
	if (mode == toplevel->decoration_configured)
		return 0;

	/* A new mode supersedes a pending withdrawal; it is offered with the next configure. */
	toplevel->decoration_configured = mode;
	toplevel->decoration_reset = 0;

	/* A window not configured yet takes it with its first configure. */
	surface = toplevel->surface;
	if (surface == NULL || !surface->configured)
		return 0;

	/* A shown window must acknowledge the new ownership before it is drawn so. */
	error = kwl_window_send_configure(surface);
	if (error != 0)
		return error;

	/* Succeeded: the mode waits for the client's acknowledgment and commit. */
	return 0;
}

/* Withdraws decoration proposals while preserving the last visible mode until commit. */
static void
decoration_reset(
    struct kwl_object *toplevel)
{
	/* Older snapshots must never resurrect an identity that no longer exists. */
	decoration_invalidate(toplevel);
	toplevel->decoration_preferred = 0U;
	toplevel->decoration_configured = MODE_CLIENT_SIDE;
	toplevel->decoration_acked = 0;
	toplevel->decoration_reset = 1;

	/* The next surface commit withdraws SSD. */
	return;
}

/* Invalidates old identity proposals even when the lifecycle generation wraps. */
static void
decoration_invalidate(
	struct kwl_object *toplevel)
{
	struct kwl_decoration_configure *snapshot;

	/* Zero marks retired proposals; active lifecycle generations always remain nonzero. */
	snapshot = toplevel->decoration_configures;
	while (snapshot != NULL) {
		snapshot->generation = 0U;
		snapshot = snapshot->next;
	}

	/* Unsigned wrap is defined; skipping zero prevents stale proposals becoming active. */
	toplevel->decoration_generation++;
	if (toplevel->decoration_generation == 0U)
		toplevel->decoration_generation = 1U;

	/* Every older snapshot is invalid regardless of its original generation. */
	return;
}

/* Frees every configure snapshot owned by a retired toplevel. */
static void
decoration_free(
    struct kwl_object *toplevel)
{
	struct kwl_decoration_configure *snapshot;

	/* The toplevel is the sole owner of this pending history. */
	while (toplevel->decoration_configures != NULL) {
		snapshot = toplevel->decoration_configures;
		toplevel->decoration_configures = snapshot->next;
		free(snapshot);
	}

	/* No configure history outlives the toplevel. */
	return;
}

/* Finds the toplevel whose committed surface owns decoration state. */
static struct kwl_object *
decoration_top(
	const struct kwl_object *surface)
{
	struct kwl_object *toplevel;

	/* A surface without a shell role has no toplevel identity. */
	if (surface == NULL)
		return NULL;

	/* A surface without a role cannot own decoration state. */
	if (surface->role == NULL)
		return NULL;

	/* Popup placement and decoration ownership are independent. */
	toplevel = surface->role->top;
	if (toplevel == NULL)
		return NULL;

	/* Refuses roles which cannot own toplevel decoration. */
	if (toplevel->kind != KWL_TOPLEVEL)
		return NULL;

	/* Succeeded: its toplevel owns every decoration field. */
	return toplevel;
}

/* Reads an unaligned native-endian protocol word. */
static uint32_t
decoration_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* Wire framing validates the extent; copying avoids alignment assumptions. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the decoded wire word. */
	return word;
}
