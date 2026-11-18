/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's glass in the compositor (ws091, after the file manager's
 * glass.c): the images float on one frosted card inside the window, and
 * the chip, while it is shown, on a small card of its own; the desktop
 * shows around the card.  The compositor draws the glass, its rim and the
 * cards' shadows (kl_glass_v1 through libkeiland); the frame leaves
 * its ground clear.
 *
 * The window is glass when its swapchain is see-through and the compositor has
 * glass; otherwise it keeps an opaque ground.  A fullscreen window has no
 * glass (its ground is black).  The panels are worked out from each
 * frame's layout and given to libkeiland before the frame is shown, which
 * sends them only when they changed (WS131 p017: kl_window_set_glass), so
 * that they take effect with that frame.
 */

#include "window.h"

#include <errno.h>
#include <string.h>

static size_t glass_panels(const struct iv_app *app, struct kl_glass_panel *panels, size_t capacity);
static int glass_send(struct iv_glass *glass, const struct iv_app *app);

/*
 * Makes the window glass when it can be, with the viewer's first panels:
 * returns 1 when it is, 0 when it keeps its opaque ground (a swapchain that
 * is not see-through, or a compositor without glass).
 */
int
iv_glass_open(
	struct iv_glass *glass,
	struct iv_window *window,
	const struct iv_present *present,
	const struct iv_app *app)
{
	int error;

	/* Nothing sent yet. */
	memset(glass, 0, sizeof(*glass));
	glass->window = window;

	/* A frame that compositor does not blend cannot let the desktop through. */
	if (present->premultiplied == 0) {
		iv_log("GLASS off reason=opaque");
		return 0;
	}

	/* The compositor's glass for the window, with the first panels; a compositor without glass refuses them. */
	error = glass_send(glass, app);
	if (error != 0) {
		iv_log("GLASS off reason=compositor errno=%d", error);
		return 0;
	}

	/* Logs the glass for the tests, and the window is glass. */
	glass->on = 1;
	iv_log("GLASS on");

	/* Succeeded: the window is glass. */
	return 1;
}

/*
 * Gives libkeiland the panels of the frame about to be shown (it sends
 * them only when they changed); they take effect with the frame's present.
 */
void
iv_glass_update(
	struct iv_glass *glass,
	const struct iv_app *app)
{
	int error;

	/* A window that is not glass has no panels. */
	if (!glass->on)
		return;

	/* The frame's panels; a refused list is logged and the old panels stay. */
	error = glass_send(glass, app);
	if (error != 0)
		iv_log("GLASS refused errno=%d", error);
}

/*
 * Takes the window's glass away.
 */
void
iv_glass_close(
	struct iv_glass *glass)
{
	/* No panels on the window, and the window is not glass any more. */
	if (glass->on)
		(void)kl_window_set_glass(glass->window->kui, NULL, 0U);
	memset(glass, 0, sizeof(*glass));
}

/* Gives libkeiland the frame's panels; 0 or an errno value. */
static int
glass_send(
	struct iv_glass *glass,
	const struct iv_app *app)
{
	struct kl_glass_panel panels[2];
	size_t count;
	int error;

	/* The frame's panels, sent with the frame. */
	count = glass_panels(app, panels, 2U);
	error = kl_window_set_glass(glass->window->kui, panels, count);
	if (error != 0)
		return error;

	/* A new count is logged once. */
	if (count != glass->count || !glass->sent)
		iv_log("GLASS panels count=%lu", (unsigned long)count);
	glass->count = count;
	glass->sent = 1;

	/* Succeeded. */
	return 0;
}

/* Works out the frame's panels: the card (none when fullscreen) and the chip while it is drawn; returns how many. */
static size_t
glass_panels(
	const struct iv_app *app,
	struct kl_glass_panel *panels,
	size_t capacity)
{
	size_t count;

	/* A fullscreen window has no glass. */
	if (app->fullscreen)
		return 0;

	/* The card, inset from the window's edge, when there is room for it in the list. */
	count = 0;
	if (count < capacity) {
		panels[count].x = IV_CARD_INSET;
		panels[count].y = IV_CARD_INSET;
		panels[count].width = app->window_width - 2 * IV_CARD_INSET;
		panels[count].height = app->window_height - 2 * IV_CARD_INSET;
		panels[count].radius = IV_CARD_RADIUS;
		panels[count].kind = KL_GLASS_CARD;

		/* A window too small for a card has none. */
		if (panels[count].width > 0 && panels[count].height > 0)
			count++;
	}

	/* The chip, where the frame drew it. */
	if (count < capacity &&
	    app->chip_width > 0 &&
	    app->chip_height > 0) {
		panels[count].x = app->chip_x;
		panels[count].y = app->chip_y;
		panels[count].width = app->chip_width;
		panels[count].height = app->chip_height;
		panels[count].radius = app->chip_height / 2;
		panels[count].kind = KL_GLASS_CARD;
		count++;
	}

	/* Reports how many panels the frame has. */
	return count;
}
