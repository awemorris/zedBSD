/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass panels (ws035-p083, plan/ws035/glass-design.md): the wrapper
 * of zdesktop's kl_glass_v1 protocol.
 *
 * A list of panels is checked here against the compositor's bounds before
 * it is sent, so that a list the compositor would refuse -- with a
 * protocol error that ends the whole connection -- is refused as one
 * failed call instead.  The protocol has no events.
 */

#include <keiland/keiland.h>

#include "ui/internal.h"

#include <wayland-client.h>
#include "userland/desktop/libwayland/keiland-glass-v1-client-protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The version of the protocol this library speaks (2: set_blur, ws075-p029). */
#define GLASS_VERSION		2U

/* One panel on the wire: six words. */
#define GLASS_PANEL_WORDS	6U

/*
 * One surface's glass: its kl_glass_v1.
 */
struct kl_glass {
	struct kl_glass_v1 *proxy;
	uint32_t version;
};

static struct kl_glass_manager_v1 *glass_bind(struct wl_display *display, uint32_t *version);
static int glass_check(const struct kl_glass_panel *panel);

/*
 * Gives a surface its glass, with no panels yet.  Returns NULL with errno
 * set: ENOTSUP for a compositor without glass, ENOMEM when the objects
 * cannot be made.
 */
struct kl_glass *
kl_glass_create(
	struct wl_display *display,
	struct wl_surface *surface)
{
	struct kl_glass_manager_v1 *manager;
	struct kl_glass *glass;
	uint32_t version;

	/* zdesktop's manager, bound for this surface. */
	manager = glass_bind(display, &version);
	if (manager == NULL)
		return NULL;

	/* The record. */
	glass = calloc(1, sizeof(*glass));
	if (glass == NULL) {
		kl_glass_manager_v1_destroy(manager);
		errno = ENOMEM;
		return NULL;
	}

	/* The protocol object (of the manager's version); the binding is not needed after it (the glass stays). */
	glass->version = version;
	glass->proxy = kl_glass_manager_v1_get_glass(manager, surface);
	kl_glass_manager_v1_destroy(manager);
	if (glass->proxy == NULL) {
		free(glass);
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: the surface can be given panels. */
	return glass;
}

/*
 * Sets the surface's panels for its next commit.
 */
int
kl_glass_set_panels(
	struct kl_glass *glass,
	const struct kl_glass_panel *panels,
	size_t count)
{
	int32_t words[KL_GLASS_PANELS_MAX * GLASS_PANEL_WORDS];
	struct wl_array array;
	size_t index;
	int error;

	/* No more panels than the compositor keeps. */
	if (count > KL_GLASS_PANELS_MAX)
		return E2BIG;

	/* Each panel checked and laid out as its six words. */
	for (index = 0; index < count; index++) {
		error = glass_check(&panels[index]);
		if (error != 0)
			return error;
		words[index * GLASS_PANEL_WORDS] = panels[index].x;
		words[index * GLASS_PANEL_WORDS + 1U] = panels[index].y;
		words[index * GLASS_PANEL_WORDS + 2U] = panels[index].width;
		words[index * GLASS_PANEL_WORDS + 3U] = panels[index].height;
		words[index * GLASS_PANEL_WORDS + 4U] = panels[index].radius;
		words[index * GLASS_PANEL_WORDS + 5U] = (int32_t)panels[index].kind;
	}

	/* The list as the request's array (the marshaller copies it). */
	array.size = count * GLASS_PANEL_WORDS * sizeof(words[0]);
	array.alloc = array.size;
	array.data = words;
	kl_glass_v1_set_panels(glass->proxy, &array);

	/* Succeeded: the panels go with the next commit. */
	return 0;
}

/*
 * Chooses whether the surface's glass shows the windows under it blurred
 * (enabled) or only the blurred wallpaper (the default, which costs the
 * compositor nothing per frame), from the surface's next commit
 * (ws075-p029).  Returns ENOTSUP when the compositor's glass is older.
 */
int
kl_glass_set_blur(
	struct kl_glass *glass,
	int enabled)
{
	uint32_t value;

	/* A compositor whose glass has no choice. */
	if (glass->version < KL_GLASS_V1_SET_BLUR_SINCE_VERSION)
		return ENOTSUP;

	/* The choice, for the next commit. */
	value = 0U;
	if (enabled != 0)
		value = 1U;
	kl_glass_v1_set_blur(glass->proxy, value);

	/* Succeeded. */
	return 0;
}

/*
 * Takes the glass away; the surface's next commit shows it without panels.
 */
void
kl_glass_destroy(
	struct kl_glass *glass)
{
	/* No glass, nothing to destroy. */
	if (glass == NULL)
		return;

	/* The protocol object, then the record. */
	kl_glass_v1_destroy(glass->proxy);
	free(glass);
}

/* Binds zdesktop's glass manager at the version both speak: from an application's registry, or found by a search of the library's own. */
static struct kl_glass_manager_v1 *
glass_bind(
	struct wl_display *display,
	uint32_t *version)
{
	struct kl_glass_manager_v1 *manager;
	struct keiui_global_search search;
	int error;

	/* The manager's global. */
	error = keiui_global_find(&search, display, "kl_glass_manager_v1");
	if (error != 0) {
		keiui_global_end(&search);
		errno = error;
		return NULL;
	}

	/* The manager, bound when announced at the version both speak; the search ends. */
	*version = search.version;
	if (*version > GLASS_VERSION)
		*version = GLASS_VERSION;
	manager = keiui_global_bind(&search, &kl_glass_manager_v1_interface, *version);
	keiui_global_end(&search);

	/* A compositor without glass. */
	if (search.name == 0U) {
		errno = ENOTSUP;
		return NULL;
	}

	/* The binding could not be made. */
	if (manager == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: the manager. */
	return manager;
}

/* Checks one panel against the compositor's bounds. */
static int
glass_check(
	const struct kl_glass_panel *panel)
{
	/* An empty panel. */
	if (panel->width <= 0 || panel->height <= 0)
		return EINVAL;

	/* A radius that is negative or past the largest. */
	if (panel->radius < 0 || panel->radius > KL_GLASS_RADIUS_MAX)
		return EINVAL;

	/* A kind that does not exist. */
	if (panel->kind != KL_GLASS_CARD)
		return EINVAL;

	/* Succeeded. */
	return 0;
}
