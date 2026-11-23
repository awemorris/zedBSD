/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The choice of a display's scale while the compositor runs (scale.h says
 * what the steps are and how the default is made).
 *
 * The scale is kept for the monitor, as its resolution is (resolution.h:
 * the identity made from its EDID), in displays.conf's scale lines; a
 * monitor without a line is left to the default, which its density gives
 * (the EDID's physical size, else the one its driver gives through
 * VK_KHR_display).  Every opening of the output chooses the scale again
 * for the display and the mode it opens at (kwl_output_scale_choose), so
 * a choice the new mode cannot offer is shown at the largest step it
 * offers below it, the file's line kept.
 *
 * A choice is applied at once, without a trial: every step offered leaves
 * the desktop large enough to go on.  The display the desktop is shown on
 * and, in the extended mode, each display beside it can be changed; the
 * displays are then placed again around the changed one
 * (kwl_heads_rearrange: it keeps its left top corner, a display it
 * overlaps or one no longer joined goes right of the rightmost).  In the
 * mirror mode every display shows the anchor's desktop at the anchor's
 * scale, and only the anchor's can be chosen.  The login screen keeps
 * 100 % (or the default) and reads no user's choice.
 *
 * A display shown beside the output (a head) takes its own scale the same
 * way when it opens (kwl_output_scale_head): its rectangle of the plane is
 * its pixels over that scale, and an extended head draws its part of the
 * plane at it.  A mirror shows the output's picture, drawn at the output's
 * scale, fitted to the head's pixels; the head keeps its own scale for the
 * extended mode.
 */

#include "kwl.h"
#include "compose.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The longest EDID read for the physical size: the base block and an extension. */
#define OUTPUT_SCALE_EDID_MAX		256U

static void output_scale_physical(struct kwl_server *server, VkDisplayKHR display, const char *key, uint32_t *mm_width, uint32_t *mm_height);
static unsigned output_scale_chosen(struct kwl_server *server, VkDisplayKHR display, const char *key);
static unsigned output_scale_why(struct kwl_server *server, VkDisplayKHR display);
static VkDisplayKHR output_scale_display(struct kwl_server *server, const char *key);
static unsigned output_scale_viewing(const char *key);
static int output_scale_set_head(struct kwl_server *server, struct kwl_head *head, const char *key, unsigned scale120, int *saved);
static struct kwl_head *output_scale_head_of(struct kwl_server *server, VkDisplayKHR display);

/*
 * Chooses the scale the output asks for (server->scale_wanted), for the
 * display and the mode it opens at: the user's choice for the monitor, or
 * the default, each shown at a step the mode offers.  A test's --scale
 * keeps the scale it gave.  Called before kwl_output_size_set at every
 * opening.
 */
void
kwl_output_scale_choose(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	uint32_t mm_width;
	uint32_t mm_height;
	unsigned recommended;
	unsigned chosen;
	unsigned shown;
	unsigned dpi;
	unsigned viewing;

	/* Nothing to choose for before the output's display is known. */
	compose = server->compose;
	if (compose == NULL || compose->display == VK_NULL_HANDLE)
		return;

	/* The display's physical size, its density and the default it asks for. */
	output_scale_physical(server, compose->display, compose->display_name, &mm_width, &mm_height);
	compose->mm_width = mm_width;
	compose->mm_height = mm_height;
	dpi = kwl_scale_density(compose->mode_width, compose->mode_height, mm_width, mm_height);
	viewing = output_scale_viewing(compose->display_name);
	recommended = kwl_scale_recommended(compose->mode_width, compose->mode_height, mm_width, mm_height, viewing);

	/* The user's choice for the monitor, shown at a step the mode offers. */
	chosen = output_scale_chosen(server, compose->display, compose->display_name);
	shown = kwl_scale_shown(chosen, recommended, compose->mode_width, compose->mode_height);

	/* A test's --scale wins. */
	if (server->scale_given) {
		printf("KWL SCALE choose key=%s mode=%ux%u mm=%ux%u dpi=%u recommended=%u chosen=%u wanted=%u reason=given\n", compose->display_name,
		    compose->mode_width, compose->mode_height, mm_width, mm_height, dpi, recommended, chosen, server->scale_wanted);
		return;
	}

	/* Succeeded: the scale the opening shows the plane at. */
	server->scale_wanted = shown;
	printf("KWL SCALE choose key=%s mode=%ux%u mm=%ux%u dpi=%u recommended=%u chosen=%u wanted=%u\n", compose->display_name, compose->mode_width,
	    compose->mode_height, mm_width, mm_height, dpi, recommended, chosen, shown);
}

/*
 * Chooses the scale of a head that opens, for the display and the mode it
 * opens at, as kwl_output_scale_choose does for the output: the user's
 * choice for the monitor, or the default, shown at a step the mode
 * offers.  Sets the head's physical size, its scale and its rectangle's
 * size in logical units (its pixels over the scale).  Called before the
 * head is placed.
 */
void
kwl_output_scale_head(
	struct kwl_server *server,
	struct kwl_head *head)
{
	char identity[KWL_RESOLUTION_IDENTITY];
	uint32_t mm_width;
	uint32_t mm_height;
	unsigned recommended;
	unsigned chosen;
	unsigned shown;
	unsigned dpi;
	unsigned viewing;

	/* The display's physical size, its density and the default it asks for (seen from its distance). */
	output_scale_physical(server, head->display, head->name, &mm_width, &mm_height);
	head->mm_width = mm_width;
	head->mm_height = mm_height;
	dpi = kwl_scale_density(head->pixel_width, head->pixel_height, mm_width, mm_height);
	viewing = output_scale_viewing(head->name);
	recommended = kwl_scale_recommended(head->pixel_width, head->pixel_height, mm_width, mm_height, viewing);

	/* The user's choice for the monitor, shown at a step the mode offers. */
	chosen = output_scale_chosen(server, head->display, head->name);
	shown = kwl_scale_shown(chosen, recommended, head->pixel_width, head->pixel_height);

	/* Succeeded: the head's scale and its logical size. */
	head->scale_wanted = shown;
	head->scale120 = shown;
	head->width = kwl_scale_logical(head->pixel_width, shown);
	head->height = kwl_scale_logical(head->pixel_height, shown);
	kwl_output_resolution_identity(server, head->display, head->name, identity, sizeof(identity));
	printf("KWL SCALE head key=%s id=%s mode=%ux%u mm=%ux%u dpi=%u recommended=%u chosen=%u scale=%u logical=%ux%u\n", head->name, identity,
	    head->pixel_width, head->pixel_height, mm_width, mm_height, dpi, recommended, chosen, shown, head->width, head->height);
}

/*
 * Tells a display's scale (its connector's key) as the displays objects
 * hear it: the scale it is shown at, the user's choice, the default, the
 * steps it offers now, its logical size, and why its scale cannot be
 * chosen now.
 */
void
kwl_output_scale_report(
	struct kwl_server *server,
	VkDisplayKHR display,
	const char *key,
	struct kwl_output_scale_state *state)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	uint32_t mode_width;
	uint32_t mode_height;
	uint32_t refresh;
	uint32_t mm_width;
	uint32_t mm_height;
	unsigned viewing;
	unsigned shown;
	unsigned slot;
	VkResult result;

	/* Nothing known yet. */
	memset(state, 0, sizeof(*state));
	compose = server->compose;
	if (compose == NULL)
		return;

	/* The display's mode: the anchor's, a head's, or its own. */
	head = NULL;
	for (slot = 0U; slot < KWL_HEADS; slot++) {
		if (compose->heads[slot].open && compose->heads[slot].display == display)
			head = &compose->heads[slot];
	}
	mode_width = 0U;
	mode_height = 0U;
	if (display == compose->display && compose->output_open) {
		mode_width = compose->mode_width;
		mode_height = compose->mode_height;
	} else if (head != NULL) {
		mode_width = head->pixel_width;
		mode_height = head->pixel_height;
	} else {
		/* Shown nowhere: the mode it would open at. */
		result = kwl_compose_display_read(server, display, &mode_width, &mode_height, &refresh, NULL, 0U);
		if (result != VK_SUCCESS)
			return;
	}

	/* The default its density asks for, and the steps its mode offers. */
	output_scale_physical(server, display, key, &mm_width, &mm_height);
	viewing = output_scale_viewing(key);
	state->recommended = kwl_scale_recommended(mode_width, mode_height, mm_width, mm_height, viewing);
	state->offered_count = kwl_scale_offered(mode_width, mode_height, state->offered, KWL_SCALE_STEPS);

	/* The user's choice, and the scale it would be shown at. */
	state->chosen = output_scale_chosen(server, display, key);
	shown = kwl_scale_shown(state->chosen, state->recommended, mode_width, mode_height);

	/* The scale it is shown at and its logical size: the anchor's and a head's as they are, another's as it would be. */
	if (display == compose->display && compose->output_open) {
		state->current = server->scale120;
		state->width = server->width;
		state->height = server->height;
	} else if (head != NULL && compose->display_mode == KWL_DISPLAYS_MIRROR) {
		/* A mirror shows the output's picture: the output's desktop, drawn at the output's scale. */
		state->current = server->scale120;
		state->width = server->width;
		state->height = server->height;
	} else if (head != NULL) {
		state->current = head->scale120;
		state->width = head->width;
		state->height = head->height;
	} else {
		state->current = 0U;
		state->width = kwl_scale_logical(mode_width, shown);
		state->height = kwl_scale_logical(mode_height, shown);
	}

	/* Why its scale cannot be chosen now. */
	state->why = output_scale_why(server, display);
	if (state->offered_count < 2U)
		state->why |= KWL_SCALE_WHY_FIXED;
}

/*
 * Chooses a display's scale (its connector's key) for a displays object's
 * set_scale: a step in 120ths, or 0 to leave it to the default.  The
 * choice is written for the monitor and shown at once, the anchor's or a
 * head's of the extended mode, the displays placed again around it.
 * Returns 0 (saved says whether the writing was given to the file), or
 * EINVAL for a scale that is not a step or a display mirrored by the
 * anchor, ENODEV for a display not connected, ENOTSUP for a step the
 * display does not offer or a display whose scale cannot be chosen (the
 * login screen, one shown nowhere), EBUSY during a change of the mode or
 * a game.
 */
int
kwl_output_scale_set(
	struct kwl_server *server,
	const char *key,
	unsigned scale120,
	int *saved)
{
	char identity[KWL_RESOLUTION_IDENTITY];
	struct kwl_plane_rect before[KWL_PLANE_SLOTS];
	struct kwl_compose *compose;
	struct kwl_head *head;
	VkDisplayKHR display;
	uint32_t before_width;
	uint32_t before_height;
	unsigned before_scale;
	unsigned why;
	int offers;
	int differs;
	int step;
	int error;

	/* Nothing written yet. */
	*saved = -1;
	compose = server->compose;

	/* A step, or the default. */
	step = kwl_scale_is_step(scale120);
	if (scale120 != 0U && !step)
		return EINVAL;

	/* A display connected, on a session's compositor. */
	display = output_scale_display(server, key);
	if (display == VK_NULL_HANDLE)
		return ENODEV;
	if (server->greeter)
		return ENOTSUP;

	/* Another display than the anchor: mirrored by it, a head beside it, or shown nowhere. */
	differs = strcmp(compose->display_name, key);
	if (differs != 0 || display != compose->display || !compose->output_open) {
		if (compose->display_mode == KWL_DISPLAYS_MIRROR && compose->display_count > 1U)
			return EINVAL;
		head = output_scale_head_of(server, display);
		if (head == NULL)
			return ENOTSUP;
		error = output_scale_set_head(server, head, key, scale120, saved);
		return error;
	}

	/* The anchor: not during a change of its mode or a game. */
	why = output_scale_why(server, display);
	if ((why & KWL_SCALE_WHY_BUSY) != 0U)
		return EBUSY;

	/* A step its mode offers. */
	if (scale120 != 0U) {
		offers = kwl_scale_offers(compose->mode_width, compose->mode_height, scale120);
		if (!offers)
			return ENOTSUP;
	}

	/* The choice kept for the monitor, and given to the file. */
	kwl_output_resolution_identity(server, display, key, identity, sizeof(identity));
	error = kwl_displays_set_scale(&compose->config, identity, scale120);
	if (error != 0)
		return error;
	*saved = kwl_heads_config_save(server);

	/* A choice made now replaces a test's --scale. */
	server->scale_given = 0U;

	/* The scale asked for, and the desktop at it once the drawing takes it; the outputs as they were. */
	before_width = server->width;
	before_height = server->height;
	before_scale = server->scale120;
	(void)kwl_outputs(server, before);
	kwl_output_scale_choose(server);
	kwl_output_size_set(server, compose->mode_width, compose->mode_height, compose->rotation, server->scale_wanted);
	printf("KWL SCALE set key=%s id=%s scale=%u wanted=%u shown=%u logical=%ux%u saved=%d\n", key, identity, scale120, server->scale_wanted,
	    server->scale120, server->width, server->height, *saved);

	/*
	 * The windows, the pointer and the look fitted to a new logical size or
	 * scale, the heads beside it placed again, and the clients told the
	 * outputs' new modes, scales and logical sizes (with every displays
	 * object's snapshot); each surface hears its new preferred scale on the
	 * next pass (surface-outputs.c).
	 */
	if (server->width != before_width ||
	    server->height != before_height ||
	    server->scale120 != before_scale) {
		kwl_output_fit(server, 1U);
		kwl_heads_rearrange(server, KWL_PLANE_ANCHOR, before);
		return 0;
	}

	/* Succeeded: nothing changed but the choice; every displays object hears the new snapshot. */
	kwl_displays_tell(server);
	return 0;
}

/*
 * Chooses the scale of a head (kwl_output_scale_set for a display beside
 * the anchor in the extended mode): written for its monitor and shown at
 * once, the head's part of the plane at its new logical size, the
 * displays placed again around it.  Returns 0, ENOTSUP for a step its
 * mode does not offer, EBUSY during a change of a mode or a game, or the
 * choice's error.
 */
static int
output_scale_set_head(
	struct kwl_server *server,
	struct kwl_head *head,
	const char *key,
	unsigned scale120,
	int *saved)
{
	char identity[KWL_RESOLUTION_IDENTITY];
	struct kwl_plane_rect before[KWL_PLANE_SLOTS];
	struct kwl_compose *compose;
	uint32_t before_width;
	uint32_t before_height;
	unsigned before_scale;
	unsigned why;
	unsigned slot;
	int offers;
	int error;

	/* Not during a change of a mode or a game. */
	compose = server->compose;
	why = output_scale_why(server, head->display);
	if ((why & KWL_SCALE_WHY_BUSY) != 0U)
		return EBUSY;

	/* A step its mode offers. */
	if (scale120 != 0U) {
		offers = kwl_scale_offers(head->pixel_width, head->pixel_height, scale120);
		if (!offers)
			return ENOTSUP;
	}

	/* The choice kept for the monitor, and given to the file. */
	kwl_output_resolution_identity(server, head->display, key, identity, sizeof(identity));
	error = kwl_displays_set_scale(&compose->config, identity, scale120);
	if (error != 0)
		return error;
	*saved = kwl_heads_config_save(server);

	/* The head at the scale its choice gives now; the outputs as they were. */
	before_width = head->width;
	before_height = head->height;
	before_scale = head->scale120;
	(void)kwl_outputs(server, before);
	kwl_output_scale_head(server, head);
	head->dirty = 1U;
	slot = (unsigned)(head - compose->heads) + 1U;
	printf("KWL SCALE set key=%s id=%s scale=%u shown=%u logical=%ux%u head=%u saved=%d\n", key, identity, scale120, head->scale120, head->width,
	    head->height, slot, *saved);

	/* The displays placed again around its new size, the windows and the pointer with them, the clients told. */
	if (head->width != before_width ||
	    head->height != before_height ||
	    head->scale120 != before_scale) {
		kwl_heads_rearrange(server, slot, before);
		return 0;
	}

	/* Succeeded: nothing changed but the choice; every displays object hears the new snapshot. */
	kwl_displays_tell(server);
	return 0;
}

/* Finds the open head of a display, or NULL. */
static struct kwl_head *
output_scale_head_of(
	struct kwl_server *server,
	VkDisplayKHR display)
{
	struct kwl_compose *compose;
	unsigned slot;

	/* Each open head (handles compare only for equality). */
	compose = server->compose;
	for (slot = 0U; slot < KWL_HEADS; slot++) {
		if (compose->heads[slot].open && !compose->heads[slot].lost && compose->heads[slot].display == display)
			return &compose->heads[slot];
	}

	/* Succeeded: none. */
	return NULL;
}

/*
 * Reads a display's physical size in millimetres: its EDID's, else the one
 * its driver gives through VK_KHR_display (0 by 0 when neither is known).
 */
static void
output_scale_physical(
	struct kwl_server *server,
	VkDisplayKHR display,
	const char *key,
	uint32_t *mm_width,
	uint32_t *mm_height)
{
	VkDisplayPropertiesKHR properties[KWL_COMPOSE_DISPLAYS];
	uint8_t edid[OUTPUT_SCALE_EDID_MAX];
	uint32_t count;
	uint32_t index;
	size_t length;
	VkResult result;
	int found;

	/* None known yet. */
	*mm_width = 0U;
	*mm_height = 0U;

	/* The EDID's, when the system gives one with a size. */
	length = kwl_os_display_edid(server, display, edid, sizeof(edid));
	found = kwl_resolution_physical(edid, length, mm_width, mm_height);
	if (found)
		return;

	/* Else the driver's, for the display among those connected. */
	count = KWL_COMPOSE_DISPLAYS;
	result = vkGetPhysicalDeviceDisplayPropertiesKHR(server->compose->physical, &count, properties);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		printf("KWL SCALE physical key=%s result=%d\n", key, (int)result);
		return;
	}
	for (index = 0U; index < count; index++) {
		/* Handles compare only for equality. */
		if (properties[index].display != display)
			continue;
		*mm_width = properties[index].physicalDimensions.width;
		*mm_height = properties[index].physicalDimensions.height;
		return;
	}
}

/* Finds the user's choice of a display's monitor (0: the default; always 0 on the login screen or before the file is read). */
static unsigned
output_scale_chosen(
	struct kwl_server *server,
	VkDisplayKHR display,
	const char *key)
{
	char identity[KWL_RESOLUTION_IDENTITY];
	unsigned chosen;

	/* No user's choice on the login screen, nor before the file is read. */
	if (!server->compose->config_loaded || server->greeter)
		return 0U;

	/* The monitor's line. */
	kwl_output_resolution_identity(server, display, key, identity, sizeof(identity));
	chosen = kwl_displays_scale(&server->compose->config, identity);

	/* Succeeded: the choice, or 0. */
	return chosen;
}

/* Tells why a display's scale cannot be chosen now (KWL_SCALE_WHY_*, without FIXED; 0: it can). */
static unsigned
output_scale_why(
	struct kwl_server *server,
	VkDisplayKHR display)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	unsigned anchor;
	unsigned why;

	/* Shown: the display the desktop is shown on, or a head beside it. */
	compose = server->compose;
	why = 0U;
	anchor = 0U;
	if (display == compose->display && compose->output_open && !compose->output_lost)
		anchor = 1U;
	head = output_scale_head_of(server, display);
	if (!anchor && head == NULL)
		why |= KWL_SCALE_WHY_ANCHOR;

	/* A display the mirror shows the anchor's desktop on takes the anchor's scale. */
	if (!anchor && compose->display_mode == KWL_DISPLAYS_MIRROR && compose->display_count > 1U)
		why |= KWL_SCALE_WHY_ALONE;

	/* Not during a change of the mode, nor under a game's own frames. */
	if (compose->resolution.state != KWL_RESOLUTION_IDLE)
		why |= KWL_SCALE_WHY_BUSY;
	if (server->scanout != NULL)
		why |= KWL_SCALE_WHY_BUSY;

	/* Succeeded: what stands in the way. */
	return why;
}

/* Finds the connected display of a connector's key, or VK_NULL_HANDLE. */
static VkDisplayKHR
output_scale_display(
	struct kwl_server *server,
	const char *key)
{
	struct kwl_compose *compose;
	unsigned index;
	int differs;

	/* Each display of the last enumeration. */
	compose = server->compose;
	if (compose == NULL)
		return VK_NULL_HANDLE;
	for (index = 0U; index < compose->display_count; index++) {
		differs = strcmp(compose->display_names[index], key);
		if (differs == 0)
			return compose->displays[index];
	}

	/* Succeeded: not connected. */
	return VK_NULL_HANDLE;
}

/*
 * Tells the distance a display is seen from (tenths of an inch, scale.h):
 * a laptop's for the machine's built-in panel (a connector's key that says
 * eDP), a desktop's for any other.  The display the compositor started on
 * is not taken for a built-in panel without such a key, as
 * kwl_output_display_internal takes it: a desktop's only monitor would be
 * shown too small.
 */
static unsigned
output_scale_viewing(
	const char *key)
{
	const char *found;

	/* A key that says it is the built-in panel. */
	found = strstr(key, ":edp:");
	if (found != NULL)
		return KWL_SCALE_VIEW_LAPTOP;

	/* Succeeded: any other display is a desktop's. */
	return KWL_SCALE_VIEW_DESKTOP;
}
