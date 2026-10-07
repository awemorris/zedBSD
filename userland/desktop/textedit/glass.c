/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's glass in the compositor (Files' glass.c): the editor's card floats
 * on the compositor's frosted glass, and the desktop shows around it.  The compositor
 * draws the glass, its rim and the card's shadow (kl_glass_v1 through
 * libkeiland); the frame leaves its ground clear around the card.
 *
 * The window is glass when its swapchain is see-through and the compositor has
 * glass; otherwise it keeps its own opaque ground.  The card is sent
 * before the frame is shown; libkeiland sends it only when it changed (WS131
 * p016: kl_window_set_glass).
 */

#include "window.h"

#include <errno.h>
#include <string.h>

static int glass_send(struct te_glass *glass, const struct te_app *app);

/*
 * Makes the window glass when it can be, with the editor's first card:
 * returns 1 when it is, 0 when it keeps its opaque ground (a swapchain
 * that is not see-through, or a compositor without glass).
 */
int
te_glass_open(
	struct te_glass *glass,
	struct te_window *window,
	const struct te_app *app,
	int see_through)
{
	int error;

	/* Nothing sent yet. */
	memset(glass, 0, sizeof(*glass));
	glass->window = window;

	/* A frame that compositor does not blend cannot let the desktop through. */
	if (!see_through) {
		te_log("GLASS off reason=opaque");
		return 0;
	}

	/* The compositor's glass for the window, with the first card; a compositor without glass refuses it. */
	error = glass_send(glass, app);
	if (error != 0) {
		te_log("GLASS off reason=compositor errno=%d", error);
		return 0;
	}

	/* Succeeded: the window is glass. */
	glass->on = 1;
	te_log("GLASS on");
	return 1;
}

/*
 * Sends the card of the frame just drawn when it differs from the one
 * sent last; it takes effect with the frame's present.
 */
void
te_glass_refresh(
	struct te_glass *glass,
	const struct te_app *app)
{
	int error;

	/* A window that is not glass has no card. */
	if (!glass->on)
		return;

	/* The card; a refused one is logged and the old one stays. */
	error = glass_send(glass, app);
	if (error != 0)
		te_log("GLASS refused errno=%d", error);
}

/*
 * Takes the window's glass away.
 */
void
te_glass_close(
	struct te_glass *glass)
{
	/* No panel on the window. */
	if (glass->on)
		(void)kl_window_set_glass(glass->window->kui, NULL, 0U);
	memset(glass, 0, sizeof(*glass));
}

/* Gives libkeiland the frame's card as the window's one panel (it sends only a change); 0 or an errno value. */
static int
glass_send(
	struct te_glass *glass,
	const struct te_app *app)
{
	struct kl_glass_panel panel;
	struct te_rect card;
	int same;
	int error;

	/* The frame's card in the compositor's terms. */
	te_app_card(app, &card);
	memset(&panel, 0, sizeof(panel));
	panel.x = card.x;
	panel.y = card.y;
	panel.width = card.width;
	panel.height = card.height;
	panel.radius = TE_CARD_RADIUS;
	panel.kind = KL_GLASS_CARD;

	/* Sent with the frame. */
	error = kl_window_set_glass(glass->window->kui, &panel, 1U);
	if (error != 0)
		return error;

	/* The log names a new card once. */
	same = memcmp(&card, &glass->shown, sizeof(card));
	if (same != 0)
		te_log("GLASS card width=%d height=%d", card.width, card.height);
	glass->shown = card;

	/* Succeeded. */
	return 0;
}
