/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's glass in the compositor (ws071-p015): the sidebar, the content
 * (with its tabs) and the preview float as cards on the compositor's frosted glass,
 * and the desktop shows between them.  The compositor draws the glass, its rim
 * and the cards' shadows (through libkeiland's window, WS131 p020); the frame
 * leaves its ground clear and only tints the cards.
 *
 * The window is glass when its swapchain is see-through and the compositor has
 * glass; otherwise it keeps its own opaque ground.  The panels are worked
 * out from each frame's layout and sent before the frame is shown, only
 * when they changed, so that they take effect with that frame.
 */

#include "window.h"

#include <errno.h>
#include <string.h>

static int glass_same(const struct fm_glass *glass, const struct fm_panel *panels, size_t count);

/*
 * Makes the window glass when it can be: returns 1 when it is, 0 when it
 * keeps its opaque ground (a swapchain that is not see-through, or a
 * compositor without glass).
 */
int
fm_glass_open(
	struct fm_glass *glass,
	struct fm_window *window,
	const struct fm_present *present)
{
	int error;

	/* Nothing sent yet. */
	memset(glass, 0, sizeof(*glass));
	glass->window = window;

	/* A frame that compositor does not blend cannot let the desktop through. */
	if (present->premultiplied == 0) {
		fm_log("GLASS off reason=opaque");
		return 0;
	}

	/* The compositor's glass for the window's surface, over the wallpaper. */
	error = kl_window_set_glass_blur(window->kui, 0);
	if (error == ENODEV) {
		fm_log("GLASS off reason=compositor errno=%d", error);
		return 0;
	}

	/* Succeeded: the window is glass. */
	glass->on = 1;
	fm_log("GLASS on");
	return 1;
}

/*
 * Sends the panels of the frame just drawn, when they differ from those
 * sent last; they take effect with the frame's present.
 */
void
fm_glass_refresh(
	struct fm_glass *glass,
	struct fm_app *app)
{
	struct kl_glass_panel sent[FM_PANELS];
	struct fm_panel panels[FM_PANELS];
	size_t count;
	size_t index;
	int same;
	int error;

	/* A window that is not glass has no panels. */
	if (!glass->on)
		return;

	/* The frame's panels, unless they are the ones the compositor has. */
	count = fm_ui_panels(app, panels, FM_PANELS);
	same = glass_same(glass, panels, count);
	if (same != 0)
		return;

	/* Each panel in the compositor's terms. */
	memset(sent, 0, sizeof(sent));
	for (index = 0; index < count; index++) {
		sent[index].x = panels[index].rect.x;
		sent[index].y = panels[index].rect.y;
		sent[index].width = panels[index].rect.width;
		sent[index].height = panels[index].rect.height;
		sent[index].radius = panels[index].radius;
		sent[index].kind = KL_GLASS_CARD;
	}

	/* Sent with the frame; a refused list is logged and the old panels stay. */
	error = kl_window_set_glass(glass->window->kui, sent, count);
	if (error != 0) {
		fm_log("GLASS refused errno=%d count=%lu", error, (unsigned long)count);
		return;
	}

	/* Remembered, to send again only what changes. */
	memcpy(glass->shown, panels, sizeof(panels[0]) * count);
	glass->shown_count = count;
	glass->sent = 1;
	fm_log("GLASS panels count=%lu", (unsigned long)count);
}

/*
 * Takes the window's glass away.
 */
void
fm_glass_close(
	struct fm_glass *glass)
{
	/* The panels, when the window is glass (the glass goes with the window). */
	if (glass->on &&
	    glass->window != NULL &&
	    glass->window->kui != NULL)
		(void)kl_window_set_glass(glass->window->kui, NULL, 0U);
	glass->on = 0;
}

/* Tells whether a list of panels is the one sent last. */
static int
glass_same(
	const struct fm_glass *glass,
	const struct fm_panel *panels,
	size_t count)
{
	size_t index;

	/* Nothing sent yet, or another count. */
	if (glass->sent == 0)
		return 0;
	if (glass->shown_count != count)
		return 0;

	/* Any panel with another place, radius or kind. */
	for (index = 0; index < count; index++) {
		if (panels[index].rect.x != glass->shown[index].rect.x ||
		    panels[index].rect.y != glass->shown[index].rect.y ||
		    panels[index].rect.width != glass->shown[index].rect.width ||
		    panels[index].rect.height != glass->shown[index].rect.height)
			return 0;
		if (panels[index].radius != glass->shown[index].radius)
			return 0;
		if (panels[index].kind != glass->shown[index].kind)
			return 0;
	}

	/* The same list. */
	return 1;
}
