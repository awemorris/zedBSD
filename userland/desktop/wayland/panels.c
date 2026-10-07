/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * kl_glass_v1 (ws035-p083, plan/ws035/glass-design.md): a surface names
 * the parts of itself that stand on the system's frosted glass -- cards
 * floating in the window -- and the compositor draws the glass under them: the
 * desktop behind, blurred and lightened, with a bright rim, and the card's
 * shadow.  The surface's own image, with
 * its alpha, goes over the glass; what it leaves clear between the panels
 * shows the desktop as it is.
 *
 * The panels are double-buffered like the surface's other state:
 * set_panels changes the pending list, and the surface's commit applies
 * it (kwl_panels_commit), so a window's panels move with the frame drawn
 * for them.  The client says what the parts are, not how glass looks.
 *
 * Version 2 (ws075-p029): set_blur chooses whether the window's glass (its
 * panels and its title bar) shows the windows under it blurred, which
 * costs a drawing of the scene under the window every frame (backdrop.c),
 * or only the blurred wallpaper (the default).  It is double-buffered too.
 *
 * The gaps between a window's panels (q838, the UAT of 2026-10-07) are
 * glass too, only not whitened and without a rim: the blurred desktop, so
 * that the window under it does not show sharp between the cards.
 */

#include "panels.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The requests of kl_glass_manager_v1 and of a surface's kl_glass_v1. */
#define GLASS_MANAGER_DESTROY		0U
#define GLASS_MANAGER_GET_GLASS		1U
#define GLASS_DESTROY			0U
#define GLASS_SET_PANELS		1U
#define GLASS_SET_BLUR			2U

/* The errors: a surface with glass already; a list of panels that is not one; glass whose surface has gone. */
#define GLASS_MANAGER_ERROR_EXISTS	0U
#define GLASS_ERROR_BAD_PANELS		0U
#define GLASS_ERROR_NO_SURFACE		1U

/* One panel on the wire: x, y, width, height, radius and kind, a word each. */
#define GLASS_PANEL_WORDS		6U
#define GLASS_PANEL_BYTES		(GLASS_PANEL_WORDS * 4U)

/* How white the glass is, how bright its rim, and how far a card's shadow reaches and how dark it is. */
#define GLASS_WHITE			0.34f
#define GLASS_RIM			0.70f
#define GLASS_SHADOW_SOFT		16.0f
#define GLASS_SHADOW_DROP		6.0f
#define GLASS_SHADOW_ALPHA		0.16f

static int panels_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int panels_set(struct kwl_object *glass, struct kwl_object *surface, const unsigned char *bytes, size_t size);
static int panels_check(const struct kwl_panel *panel);
static void panels_shadow(struct kwl_server *server, VkCommandBuffer command, const struct kwl_panel *panel, const float *place, float opacity);
static void panels_glass(struct kwl_server *server, VkCommandBuffer command, const struct kwl_panel *panel, const float *place, float opacity, unsigned light);
static void panels_gaps(struct kwl_server *server, VkCommandBuffer command, const struct kwl_panels *panels, const float *place, float opacity, unsigned light);
static uint32_t panels_word(const unsigned char *bytes, size_t offset);

/*
 * Carries out a request of kl_glass_manager_v1 or of a surface's
 * kl_glass_v1.
 */
int
kwl_panels_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	int error;

	/* The manager: it goes, or it gives a surface its glass. */
	if (object->kind == KWL_GLASS_MANAGER) {
		if (opcode == GLASS_MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Only get_glass is left. */
		if (opcode != GLASS_MANAGER_GET_GLASS)
			return EPROTO;
		error = panels_create(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The glass goes; the surface's next commit shows it without panels (kwl_panels_object_gone). */
	if (opcode == GLASS_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* set_panels needs the surface. */
	surface = object->surface;
	if (surface == NULL) {
		(void)kwl_error_code(object->client, object->id, GLASS_ERROR_NO_SURFACE, "the surface has gone");
		return EPROTO;
	}

	/* The choice of blur for the next commit (version 2): one word, 0 or 1. */
	if (opcode == GLASS_SET_BLUR) {
		if (size != 4U || surface->panels == NULL)
			return EPROTO;
		surface->panels->pending_blur = panels_word(bytes, 0U) != 0U;
		surface->panels->changed = 1;
		return 0;
	}

	/* The panels for the next commit. */
	if (opcode != GLASS_SET_PANELS)
		return EPROTO;
	error = panels_set(object, surface, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the pending panels changed. */
	return 0;
}

/*
 * Applies a surface's pending panels with its commit.
 */
void
kwl_panels_commit(
	struct kwl_object *surface)
{
	struct kwl_panels *panels;
	const struct kwl_panel *panel;
	unsigned index;

	/* A surface without glass, or whose panels did not change. */
	panels = surface->panels;
	if (panels == NULL || !panels->changed)
		return;

	/* A change of the blur, logged on its own line. */
	if (panels->blur != panels->pending_blur)
		printf("KWL GLASS client=%llu surface=%u blur=%u\n", (unsigned long long)surface->client->number, surface->id, panels->pending_blur);

	/* The pending panels and blur become the surface's, drawn from the next frame. */
	memcpy(panels->current, panels->pending, sizeof(panels->current));
	panels->count = panels->pending_count;
	panels->blur = panels->pending_blur;
	panels->changed = 0;
	surface->client->server->dirty = 1;

	/* The log the tests read: the count, then each panel. */
	printf("KWL GLASS client=%llu surface=%u panels=%u", (unsigned long long)surface->client->number, surface->id, panels->count);
	for (index = 0; index < panels->count; index++) {
		panel = &panels->current[index];
		printf(" card:%d,%d,%d,%d,%d", panel->x, panel->y, panel->width, panel->height, panel->radius);
	}

	/* The line ends. */
	printf("\n");
}

/*
 * Unties an object that is going from the glass: a kl_glass_v1's surface
 * loses its panels with its next commit; a surface's glass names nothing,
 * and its panels' record goes with it.
 */
void
kwl_panels_object_gone(
	struct kwl_object *object)
{
	struct kwl_object *surface;

	/* The glass: its surface's pending list empties. */
	if (object->kind == KWL_GLASS) {
		surface = object->surface;
		object->surface = NULL;
		if (surface == NULL)
			return;
		surface->glass = NULL;
		if (surface->panels != NULL) {
			surface->panels->pending_count = 0;
			surface->panels->pending_blur = 0U;
			surface->panels->changed = 1;
		}

		/* Nothing more goes with the glass. */
		return;
	}

	/* A surface: its glass names nothing, and the record is freed. */
	if (object->kind != KWL_SURFACE)
		return;
	if (object->glass != NULL) {
		object->glass->surface = NULL;
		object->glass = NULL;
	}

	/* The record goes with the surface. */
	free(object->panels);
	object->panels = NULL;
}

/*
 * Returns how many glass panels a surface's last commit gave it.
 */
unsigned
kwl_panels_count(
	const struct kwl_object *surface)
{
	/* No glass, no panels. */
	if (surface->panels == NULL)
		return 0;

	/* The committed panels. */
	return surface->panels->count;
}

/*
 * Returns whether a surface's last commit asked its glass to show the
 * windows under it blurred (set_blur); 0 for a surface without glass.
 */
unsigned
kwl_panels_blur(
	const struct kwl_object *surface)
{
	/* No glass, the blurred wallpaper. */
	if (surface->panels == NULL)
		return 0;

	/* The committed choice. */
	return surface->panels->blur;
}

/*
 * Draws a surface's glass panels where its image goes: place is the
 * image's left and top on the output and its scale across and down (a
 * window being resized is stretched, a Wiseview tile is small).  The
 * cards' shadows come first, when asked for, so that no shadow falls on
 * another panel's glass.
 */
void
kwl_panels_draw(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_object *surface,
	const float *place,
	float opacity,
	unsigned shadows)
{
	const struct kwl_panels *panels;
	unsigned index;
	unsigned light;

	/* A surface without panels has no glass. */
	panels = surface->panels;
	if (panels == NULL || panels->count == 0U)
		return;

	/* The gaps between the panels, blurred, under the shadows and the panels. */
	light = 0U;
	if (surface->client->theme_bound == 0U)
		light = 1U;
	panels_gaps(server, command, panels, place, opacity, light);

	/* The cards' shadows, under every panel, when asked for. */
	for (index = 0; index < panels->count; index++) {
		if (shadows != 0U)
			panels_shadow(server, command, &panels->current[index], place, opacity);
	}

	/*
	 * The glass of each panel: in the dark appearance dark, unless the
	 * client does not know the appearance and draws light (ws089-p017).
	 */
	for (index = 0; index < panels->count; index++)
		panels_glass(server, command, &panels->current[index], place, opacity, light);
}

/* Gives a surface its kl_glass_v1 (one per surface) and the record of its panels. */
static int
panels_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	struct kwl_object *created;
	uint32_t id;

	/* The new ID and the client's surface. */
	if (size != 8U)
		return EPROTO;
	id = panels_word(bytes, 0U);
	surface = kwl_find(manager->client, panels_word(bytes, 4U));
	if (surface == NULL || surface->kind != KWL_SURFACE)
		return EPROTO;

	/* One glass per surface. */
	if (surface->glass != NULL) {
		(void)kwl_error_code(manager->client, manager->id, GLASS_MANAGER_ERROR_EXISTS, "the surface has glass");
		return EPROTO;
	}

	/* The record, made once for the surface's life. */
	if (surface->panels == NULL) {
		surface->panels = calloc(1, sizeof(*surface->panels));
		if (surface->panels == NULL)
			return ENOMEM;
	}

	/* The glass, tied to its surface both ways. */
	created = kwl_create(manager->client, id, KWL_GLASS, manager->version);
	if (created == NULL)
		return EPROTO;
	created->surface = surface;
	surface->glass = created;

	/* Succeeded: the surface can be given panels. */
	return 0;
}

/* Takes a list of panels for the surface's next commit (an array of six words a panel). */
static int
panels_set(
	struct kwl_object *glass,
	struct kwl_object *surface,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_panel list[KWL_PANELS_MAX];
	uint32_t length;
	unsigned count;
	unsigned index;
	size_t offset;
	int error;

	/* The array's length, and its bytes padded to a whole word. */
	if (size < 4U)
		return EPROTO;
	length = panels_word(bytes, 0U);
	if (size != 4U + (((size_t)length + 3U) & ~(size_t)3U))
		return EPROTO;

	/* Whole panels, no more than a surface may have. */
	count = length / GLASS_PANEL_BYTES;
	if (length % GLASS_PANEL_BYTES != 0U || count > KWL_PANELS_MAX) {
		(void)kwl_error_code(glass->client, glass->id, GLASS_ERROR_BAD_PANELS, "not a list of panels");
		return EPROTO;
	}

	/* Each panel, word by word, checked. */
	for (index = 0; index < count; index++) {
		offset = 4U + (size_t)index * GLASS_PANEL_BYTES;
		list[index].x = (int32_t)panels_word(bytes, offset);
		list[index].y = (int32_t)panels_word(bytes, offset + 4U);
		list[index].width = (int32_t)panels_word(bytes, offset + 8U);
		list[index].height = (int32_t)panels_word(bytes, offset + 12U);
		list[index].radius = (int32_t)panels_word(bytes, offset + 16U);
		list[index].kind = panels_word(bytes, offset + 20U);
		error = panels_check(&list[index]);
		if (error != 0) {
			(void)kwl_error_code(glass->client, glass->id, GLASS_ERROR_BAD_PANELS, "a panel is out of range");
			return EPROTO;
		}
	}

	/* The list for the next commit. */
	memset(surface->panels->pending, 0, sizeof(surface->panels->pending));
	memcpy(surface->panels->pending, list, sizeof(list[0]) * count);
	surface->panels->pending_count = count;
	surface->panels->changed = 1;

	/* Succeeded. */
	return 0;
}

/* Checks one panel: a positive size, a radius in range and a known kind. */
static int
panels_check(
	const struct kwl_panel *panel)
{
	/* An empty panel. */
	if (panel->width <= 0 || panel->height <= 0)
		return EINVAL;

	/* A radius that is negative or past the largest. */
	if (panel->radius < 0 || panel->radius > KWL_PANEL_RADIUS_MAX)
		return EINVAL;

	/* A kind that does not exist. */
	if (panel->kind != KWL_PANEL_CARD)
		return EINVAL;

	/* Succeeded. */
	return 0;
}

/* Draws a card's shadow, a little below it, soft around its edges. */
static void
panels_shadow(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_panel *panel,
	const float *place,
	float opacity)
{
	struct glass_shape shape;
	float x;
	float y;
	float width;
	float height;

	/* The card on the output. */
	x = place[0] + (float)panel->x * place[2];
	y = place[1] + (float)panel->y * place[3];
	width = (float)panel->width * place[2];
	height = (float)panel->height * place[3];

	/* The shadow's quad reaches past the card as far as it fades. */
	glass_shape_init(&shape, x, y + GLASS_SHADOW_DROP, width, height);
	shape.quad[0] -= 2.0f * GLASS_SHADOW_SOFT;
	shape.quad[1] -= 2.0f * GLASS_SHADOW_SOFT;
	shape.quad[2] += 4.0f * GLASS_SHADOW_SOFT;
	shape.quad[3] += 4.0f * GLASS_SHADOW_SOFT;
	shape.mode = MODE_SHADOW;
	shape.radius = (float)panel->radius * place[2];
	shape.soft = GLASS_SHADOW_SOFT;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = GLASS_SHADOW_ALPHA;
	shape.opacity = opacity;
	glass_shape_draw(server, command, &shape);
}

/*
 * Draws one panel's glass.  The glass is flat: no sheen from the top, which
 * would make a tall card's lower part look darker than the title bars'.
 * With the panels opaque (window.opacity chosen at 100, BUG-171) the glass
 * is fully its colour, so nothing of the desktop shows through it.
 */
static void
panels_glass(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_panel *panel,
	const float *place,
	float opacity,
	unsigned light)
{
	struct glass_shape shape;
	float x;
	float y;
	float width;
	float height;
	float radius;

	/* The panel on the output. */
	x = place[0] + (float)panel->x * place[2];
	y = place[1] + (float)panel->y * place[3];
	width = (float)panel->width * place[2];
	height = (float)panel->height * place[3];
	radius = (float)panel->radius * place[2];

	/* The frosted glass: the blurred desktop, whitened, with a bright rim and no sheen. */
	glass_shape_init(&shape, x, y, width, height);
	shape.mode = MODE_GLASS;
	shape.radius = radius;
	shape.soft = 1.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = GLASS_WHITE;
	if (server->panels_opaque != 0U)
		shape.color[3] = 1.0f;
	shape.edge = GLASS_RIM;
	shape.opacity = opacity;
	shape.light = light;
	glass_shape_draw(server, command, &shape);
}

/*
 * Draws the glass under the gaps between a surface's panels: the box round
 * all of them (rounded like the most rounded panel) as the blurred desktop
 * alone, neither whitened nor rimmed; the panels are drawn over it.  With
 * the panels opaque (BUG-171) the gaps are opaque too.  One panel leaves
 * no gap.
 */
static void
panels_gaps(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_panels *panels,
	const float *place,
	float opacity,
	unsigned light)
{
	struct glass_shape shape;
	const struct kwl_panel *panel;
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
	int32_t radius;
	unsigned index;

	/* One panel has no gap. */
	if (panels->count < 2U)
		return;

	/* The box round every panel, and the largest corner. */
	left = panels->current[0].x;
	top = panels->current[0].y;
	right = panels->current[0].x + panels->current[0].width;
	bottom = panels->current[0].y + panels->current[0].height;
	radius = panels->current[0].radius;
	for (index = 1; index < panels->count; index++) {
		panel = &panels->current[index];
		if (panel->x < left)
			left = panel->x;
		if (panel->y < top)
			top = panel->y;
		if (panel->x + panel->width > right)
			right = panel->x + panel->width;
		if (panel->y + panel->height > bottom)
			bottom = panel->y + panel->height;
		if (panel->radius > radius)
			radius = panel->radius;
	}

	/*
	 * The blurred desktop: glass of no colour (not white, so the shader
	 * does not lift it) and no rim; flat like the panels.
	 */
	glass_shape_init(&shape, place[0] + (float)left * place[2], place[1] + (float)top * place[3], (float)(right - left) * place[2], (float)(bottom - top) * place[3]);
	shape.mode = MODE_GLASS;
	shape.radius = (float)radius * place[2];
	shape.soft = 1.0f;
	shape.color[0] = 0.0f;
	shape.color[1] = 0.0f;
	shape.color[2] = 0.0f;
	shape.color[3] = 0.0f;
	shape.edge = 0.0f;
	shape.opacity = opacity;
	shape.light = light;

	/* Opaque panels: the gaps are their colour, so nothing shows through the window. */
	if (server->panels_opaque != 0U) {
		shape.color[0] = 1.0f;
		shape.color[1] = 1.0f;
		shape.color[2] = 1.0f;
		shape.color[3] = 1.0f;
	}

	/* Drawn. */
	glass_shape_draw(server, command, &shape);
}

/* Reads one native-endian word of a request. */
static uint32_t
panels_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word, copied out of the byte stream (it may not be aligned). */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}
