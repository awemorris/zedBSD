/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power dialog's state and layout (power-layout.c, ws099-p037,
 * BUG-235): App Home's Power Off and the power button (WS182) darken the
 * desktop and offer Power Off, Restart, Log Out and Cancel in a card in
 * the middle.  The choices the session may not take (Power Off and
 * Restart for a zedBSD user outside wheel, for one) are shown faint and
 * take no press.
 *
 * It knows nothing of the server: the caller hands it the output's size
 * and the places pressed, and draws and acts on what it says
 * (power-dialog.c).  So the host tests run it alone.
 */

#ifndef KWL_POWER_LAYOUT_H
#define KWL_POWER_LAYOUT_H

#include <stdint.h>

/* The choices, from the top of the card. */
#define KWL_POWER_POWEROFF	0
#define KWL_POWER_RESTART	1
#define KWL_POWER_LOGOUT	2
#define KWL_POWER_CANCEL	3
#define KWL_POWER_CHOICES	4

/* A place that is no choice: inside the card between the buttons, or outside the card. */
#define KWL_POWER_IN_CARD	(-1)
#define KWL_POWER_OUTSIDE	(-2)

/* A choice's bit in a set of the choices that may be taken. */
#define KWL_POWER_BIT(choice)	(1U << (unsigned)(choice))

/* The card and its buttons: x, y, width and height each. */
struct kwl_power_layout {
	int32_t card[4];
	int32_t buttons[KWL_POWER_CHOICES][4];
};

/*
 * The dialog: whether it shows, whether it is closing, when it began to
 * open or to close (milliseconds), the choices that may be taken
 * (KWL_POWER_BIT), the choice the keys are on, the button a press is on
 * (-1 none), and what opened it ("home" or "button").
 */
struct kwl_power_dialog {
	unsigned open;
	unsigned closing;
	uint64_t start_ms;
	unsigned enabled;
	int focus;
	int pressed;
	char source[8];
};

void kwl_power_layout(int32_t width, int32_t height, struct kwl_power_layout *layout);
int kwl_power_hit(const struct kwl_power_layout *layout, int32_t x, int32_t y);
int kwl_power_focus_step(int focus, int step, unsigned enabled);
const char *kwl_power_choice_name(int choice);

#endif
