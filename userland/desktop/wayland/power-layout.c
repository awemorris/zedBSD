/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power dialog's layout and its keys' order (ws099-p037; power-layout.h
 * says what the dialog is).
 */

#include "power-layout.h"

/* The card's width, its padding, a button's height and the gap between two (pixels). */
#define POWER_CARD_WIDTH	360
#define POWER_CARD_PAD		28
#define POWER_BUTTON_HEIGHT	52
#define POWER_BUTTON_GAP	12

static int power_inside(const int32_t *rect, int32_t x, int32_t y);

/*
 * Lays the card out in the middle of an output of a size: the four buttons
 * one under the other, Power Off at the top, Cancel at the bottom.
 */
void
kwl_power_layout(
	int32_t width,
	int32_t height,
	struct kwl_power_layout *layout)
{
	int32_t card_height;
	int choice;

	/* The card in the middle, as high as its buttons and padding. */
	card_height = 2 * POWER_CARD_PAD + KWL_POWER_CHOICES * POWER_BUTTON_HEIGHT + (KWL_POWER_CHOICES - 1) * POWER_BUTTON_GAP;
	layout->card[0] = (width - POWER_CARD_WIDTH) / 2;
	layout->card[1] = (height - card_height) / 2;
	layout->card[2] = POWER_CARD_WIDTH;
	layout->card[3] = card_height;

	/* The buttons inside its padding, from the top. */
	for (choice = 0; choice < KWL_POWER_CHOICES; choice++) {
		layout->buttons[choice][0] = layout->card[0] + POWER_CARD_PAD;
		layout->buttons[choice][1] = layout->card[1] + POWER_CARD_PAD + choice * (POWER_BUTTON_HEIGHT + POWER_BUTTON_GAP);
		layout->buttons[choice][2] = POWER_CARD_WIDTH - 2 * POWER_CARD_PAD;
		layout->buttons[choice][3] = POWER_BUTTON_HEIGHT;
	}
}

/*
 * Tells what is at a point: a choice's button, the card between them
 * (KWL_POWER_IN_CARD), or outside the card (KWL_POWER_OUTSIDE).
 */
int
kwl_power_hit(
	const struct kwl_power_layout *layout,
	int32_t x,
	int32_t y)
{
	int inside;
	int choice;

	/* Outside the card. */
	inside = power_inside(layout->card, x, y);
	if (!inside)
		return KWL_POWER_OUTSIDE;

	/* A button under the point. */
	for (choice = 0; choice < KWL_POWER_CHOICES; choice++) {
		inside = power_inside(layout->buttons[choice], x, y);
		if (inside)
			return choice;
	}

	/* Between the buttons. */
	return KWL_POWER_IN_CARD;
}

/*
 * Moves the keys' choice a step down (1) or up (-1), round the ends,
 * over the choices that may not be taken.  Cancel may always be taken, so
 * a step always lands.  Returns the new choice.
 */
int
kwl_power_focus_step(
	int focus,
	int step,
	unsigned enabled)
{
	unsigned bit;
	int tries;
	int next;

	/* Cancel can always be chosen. */
	enabled |= KWL_POWER_BIT(KWL_POWER_CANCEL);

	/* A focus out of range starts from Cancel. */
	next = focus;
	if (next < 0 || next >= KWL_POWER_CHOICES)
		next = KWL_POWER_CANCEL;

	/* The next one that may be taken, round the ends. */
	for (tries = 0; tries < KWL_POWER_CHOICES; tries++) {
		next = (next + step + KWL_POWER_CHOICES) % KWL_POWER_CHOICES;
		bit = KWL_POWER_BIT(next);
		if ((enabled & bit) != 0U)
			return next;
	}

	/* Succeeded: no other choice, the one it was on. */
	return focus;
}

/*
 * Names a choice for the log.
 */
const char *
kwl_power_choice_name(
	int choice)
{
	/* Each one. */
	switch (choice) {
	case KWL_POWER_POWEROFF:
		return "poweroff";
	case KWL_POWER_RESTART:
		return "restart";
	case KWL_POWER_LOGOUT:
		return "logout";
	case KWL_POWER_CANCEL:
		return "cancel";
	default:
		return "none";
	}
}

/* Tells whether a point is inside a rectangle (x, y, width, height). */
static int
power_inside(
	const int32_t *rect,
	int32_t x,
	int32_t y)
{
	/* Left of it or above it. */
	if (x < rect[0] || y < rect[1])
		return 0;

	/* Right of it or below it. */
	if (x >= rect[0] + rect[2] || y >= rect[1] + rect[3])
		return 0;

	/* Succeeded: inside. */
	return 1;
}
