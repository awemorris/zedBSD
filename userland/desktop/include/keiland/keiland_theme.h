/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The theme and the accent, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_THEME_H
#define KEILAND_THEME_H

#include <keiland/keiland_canvas.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The theme: the colours and sizes of the Kei look, which every widget draws
 * with (the file manager's values), in the light or the dark appearance with
 * the accent the user chose (KL_VERSION 57); an application reads it and does
 * not change it.
 */
struct kl_theme {
	/* The window's ground (a vertical gradient) and the cards on it. */
	kl_color ground_top;
	kl_color ground_bottom;
	kl_color panel;
	kl_color panel_edge;
	kl_color shadow;

	/* A sidebar and a content card standing on glass, and on the plain ground. */
	kl_color glass_sidebar;
	kl_color glass_content;
	kl_color sidebar;

	/* Text: the main ink, the secondary and the faint, and an icon's ink. */
	kl_color text;
	kl_color text_secondary;
	kl_color text_faint;
	kl_color icon;

	/* The accent, a selection with and without the keyboard, the pointer's hover, a separator, a folder and danger. */
	kl_color accent;
	kl_color selection;
	kl_color selection_inactive;
	kl_color hover;
	kl_color separator;
	kl_color folder;
	kl_color danger;

	/* A card's and a control's corner radius, a list row's height, and the text sizes of body, secondary and title text. */
	float card_radius;
	float control_radius;
	int row_height;
	unsigned text_body;
	unsigned text_small;
	unsigned text_title;

	/*
	 * Settings' values: a card within a page and its edge, the line between a
	 * card's rows, a control's ground and edge, a switch's track when off, good
	 * and bad news, a control's height, and a switch's size.
	 */
	kl_color card;
	kl_color card_edge;
	kl_color row_separator;
	kl_color control;
	kl_color control_edge;
	kl_color track;
	kl_color good;
	kl_color bad;
	int control_height;
	int switch_width;
	int switch_height;

	/*
	 * KL_VERSION 57: the ink of text and marks drawn on the accent (white or
	 * nearly black, whichever reads), and the accent as the colour of text on
	 * the window's ground.
	 */
	kl_color accent_ink;
	kl_color accent_text;
};

/* The theme (theme.c). */
const struct kl_theme *kl_theme_default(void);

/*
 * One of two colours by the desktop's appearance the program was told last
 * (KL_VERSION 35): light in the light appearance, dark in the dark one -- for
 * a program's colours of its own beside the theme's.
 */
kl_color kl_theme_choose(kl_color light, kl_color dark);

/*
 * The accent the user chose in Settings (KL_VERSION 57): one of eight
 * colours, told with the appearance (kl_theme_v1 version 2); the theme's
 * accent, selection, accent_ink and accent_text follow it, and a watch's
 * callback is called when it changes.  A compositor that does not tell it
 * leaves blue.
 */
#define KL_ACCENT_BLUE		0U
#define KL_ACCENT_PURPLE	1U
#define KL_ACCENT_PINK		2U
#define KL_ACCENT_RED		3U
#define KL_ACCENT_ORANGE	4U
#define KL_ACCENT_YELLOW	5U
#define KL_ACCENT_GREEN		6U
#define KL_ACCENT_GRAPHITE	7U
#define KL_ACCENTS		8U

/* An accent's colours in one appearance: the accent, the ink on it, the accent as text, and the selection's ground. */
struct kl_accent {
	kl_color accent;
	kl_color ink;
	kl_color text;
	kl_color selection;
};

/* The accent (KL_ACCENT_*) the program was told last (blue before any). */
unsigned kl_accent_get(void);

/* An accent's colours in an appearance (KL_APPEARANCE_*), for a program that shows the choices. */
void kl_accent_values(unsigned accent, unsigned appearance, struct kl_accent *values);

#ifdef __cplusplus
}
#endif

#endif
