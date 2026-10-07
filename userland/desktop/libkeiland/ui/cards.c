/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The surfaces of the library (ws090-p005): the panels a window is made
 * of (Files' sidebar and content, files/ui.c), Settings' cards with their
 * rows and a page's header (settings/widgets.c), the dialog that asks a
 * question over a part of a window (Text Editor's and the file chooser's),
 * the chip that says something for a moment, and a progress bar.
 */

#include "internal.h"

#include <string.h>

/* The panels' corners. */
#define CARDS_PANEL_RADIUS	16.0f

/* A card: its corners, its margin, its title's and subtitle's sizes, and the space its title takes. */
#define CARDS_CARD_RADIUS	14.0f
#define CARDS_CARD_PAD		18
#define CARDS_TEXT_CARD		16U
#define CARDS_TEXT_CARD_SUB	13U
#define CARDS_CARD_TITLE	46

/* A card's ground on a white panel (no glass under it). */
#define CARDS_CARD_PLAIN	KL_RGBA(0x8a96aa, 16)

/* A row: its height, its text's size, and the share of the row its label takes. */
#define CARDS_ROW_HEIGHT	40
#define CARDS_TEXT_ROW		14U
#define CARDS_LABEL_SHARE	0.34f

/* A page's header: its title's and summary's sizes. */
#define CARDS_TEXT_TITLE	30U
#define CARDS_TEXT_SUMMARY	15U

/* The dialog: its width, its margin, its title's and body's sizes, the most lines of body, and the gap between buttons. */
#define CARDS_DIALOG_WIDTH	392
#define CARDS_DIALOG_PAD	20
#define CARDS_TEXT_DIALOG	15U
#define CARDS_TEXT_BODY		13U
#define CARDS_DIALOG_LINES	3
#define CARDS_DIALOG_GAP	8

/* The body's lines: their distance, and the space more between the title and the first. */
#define CARDS_BODY_LINE		18
#define CARDS_BODY_GAP		8

/* The index that stands for the dialog itself among its records (it covers the window under it). */
#define CARDS_DIALOG_SELF	0xffffffffU

/* The chip's height, its corners and its text's size. */
#define CARDS_CHIP_HEIGHT	32
#define CARDS_CHIP_RADIUS	10.0f
#define CARDS_TEXT_CHIP		13U

/* An unknown progress's moving part: its share of the bar and the time it takes to cross, in microseconds. */
#define CARDS_PROGRESS_SPAN	0.3
#define CARDS_PROGRESS_US	1400000U

static int cards_dialog_key(uint32_t code, unsigned modifiers);

/*
 * Draws a window's panel: the sidebar's (sidebar 1) or the content's, on
 * glass a light veil, otherwise Files' opaque look.
 */
void
kl_panel(
	const struct kl_style *style,
	const struct kl_rect *rect,
	int sidebar)
{
	const struct kl_theme *theme;

	/* On glass: the veil (the compositor draws the glass under it). */
	theme = style->theme;
	if (style->glass) {
		if (sidebar)
			kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_PANEL_RADIUS, theme->glass_sidebar);
		else
			kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_PANEL_RADIUS, theme->glass_content);
		return;
	}

	/* Without glass: the sidebar's veil with a bright edge, the content's white card with its shadow. */
	if (sidebar) {
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_PANEL_RADIUS, theme->sidebar);
		kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_PANEL_RADIUS, 1.0f, kl_theme_choose(KL_RGBA(0xffffff, 170), KL_RGBA(0x3a404b, 170)));
		return;
	}

	/* The content's white panel with its shadow and edge. */
	kl_canvas_shadow(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_PANEL_RADIUS, 12.0f, theme->shadow);
	kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_PANEL_RADIUS, theme->panel);
	kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_PANEL_RADIUS, 1.0f, theme->panel_edge);
}

/*
 * Draws a card (Settings' look) with its title and subtitle (either may be
 * NULL) and reports the top of its content.
 */
int
kl_card(
	const struct kl_style *style,
	const struct kl_rect *rect,
	const char *title,
	const char *subtitle)
{
	struct kl_text_line line;
	int baseline;

	/* The card: on glass a whiter veil with a bright edge (Settings'), on a white panel a faint grey with a quiet edge. */
	if (style->glass) {
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_CARD_RADIUS, style->theme->card);
		kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_CARD_RADIUS, 1.0f, style->theme->card_edge);
	} else {
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_CARD_RADIUS, CARDS_CARD_PLAIN);
		kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CARDS_CARD_RADIUS, 1.0f, style->theme->control_edge);
	}

	/* A card without a title starts at its margin. */
	if (title == NULL)
		return rect->y + CARDS_CARD_PAD;

	/* The title, bold. */
	kl_text_metrics(style->text, CARDS_TEXT_CARD, &line);
	baseline = rect->y + CARDS_CARD_PAD + line.ascent;
	(void)kl_text_draw_fit(style->text, style->canvas, rect->x + CARDS_CARD_PAD + 2, baseline, title, CARDS_TEXT_CARD, 1, rect->width - 2 * CARDS_CARD_PAD, style->theme->text);

	/* The subtitle under it, when there is one. */
	if (subtitle != NULL) {
		baseline += line.descent + 4;
		kl_text_metrics(style->text, CARDS_TEXT_CARD_SUB, &line);
		baseline += line.ascent;
		(void)kl_text_draw_fit(style->text, style->canvas, rect->x + CARDS_CARD_PAD + 2, baseline, subtitle, CARDS_TEXT_CARD_SUB, 0, rect->width - 2 * CARDS_CARD_PAD, style->theme->text_secondary);
		return baseline + line.descent + 10;
	}

	/* The content starts under the title. */
	return rect->y + CARDS_CARD_TITLE;
}

/*
 * Draws a card's row of a label and its value from a top edge (with a
 * line under it unless it is the last) and reports the edge below it.
 */
int
kl_row(
	const struct kl_style *style,
	int x,
	int y,
	int width,
	const char *label,
	const char *value,
	int last)
{
	int baseline;
	int label_width;
	int left;
	int right;

	/* The row's text line, and the columns of the label and the value. */
	baseline = kl_text_center(CARDS_TEXT_ROW, y, CARDS_ROW_HEIGHT);
	left = x + CARDS_CARD_PAD + 2;
	right = x + width - CARDS_CARD_PAD;
	label_width = (int)((float)(right - left) * CARDS_LABEL_SHARE);

	/* The label, quiet, and the value, plain. */
	(void)kl_text_draw_fit(style->text, style->canvas, left, baseline, label, CARDS_TEXT_ROW, 0, label_width - 12, style->theme->text_secondary);
	(void)kl_text_draw_fit(style->text, style->canvas, left + label_width, baseline, value, CARDS_TEXT_ROW, 0, right - left - label_width, style->theme->text);

	/* The line under the row, unless it is the last. */
	if (!last)
		kl_canvas_line(style->canvas, (float)left, (float)(y + CARDS_ROW_HEIGHT) - 0.5f, (float)right, (float)(y + CARDS_ROW_HEIGHT) - 0.5f, 1.0f, style->theme->row_separator);

	/* Reports the edge below the row. */
	return y + CARDS_ROW_HEIGHT;
}

/*
 * Draws a page's header, its title bold and its summary under it (may be
 * NULL), and reports the edge below it.
 */
int
kl_header(
	const struct kl_style *style,
	int x,
	int y,
	int width,
	const char *title,
	const char *summary)
{
	struct kl_text_line title_line;
	struct kl_text_line summary_line;
	int baseline;

	/* The two lines' measurements. */
	kl_text_metrics(style->text, CARDS_TEXT_TITLE, &title_line);
	kl_text_metrics(style->text, CARDS_TEXT_SUMMARY, &summary_line);

	/* The title, bold. */
	baseline = y + title_line.ascent;
	(void)kl_text_draw_fit(style->text, style->canvas, x, baseline, title, CARDS_TEXT_TITLE, 1, width, style->theme->text);
	if (summary == NULL)
		return y + title_line.height;

	/* The summary under it. */
	baseline = y + title_line.height + 2 + summary_line.ascent;
	(void)kl_text_draw_fit(style->text, style->canvas, x, baseline, summary, CARDS_TEXT_SUMMARY, 0, width, style->theme->text_secondary);

	/* Reports the edge below the summary. */
	return y + title_line.height + 2 + summary_line.height;
}

/*
 * Draws a dialog over an area of the window: a veil, and a card with a
 * question, its explanation and buttons (labels[0] the main one, at the
 * right; the last one cancels).  It takes the keyboard while it shows:
 * Enter chooses the main button and Esc the last.  Reports the index of
 * the button chosen since the last frame, or -1.
 */
int
kl_dialog(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *area,
	const char *title,
	const char *body,
	const char *const *labels,
	int count)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect button;
	uint32_t code;
	unsigned modifiers;
	unsigned flags;
	size_t done;
	size_t length;
	int chosen;
	int lines;
	int index;
	int right;
	int taken;
	int baseline;
	int pressed;
	int focused;
	uint32_t focus_id;
	uint32_t focus_index;

	/* The veil, which also takes every press under the dialog and shuts what is under it out of Tab. */
	theme = style->theme;
	kl_canvas_round(style->canvas, (float)area->x, (float)area->y, (float)area->width, (float)area->height, CARDS_PANEL_RADIUS, KL_RGBA(0x0f172a, 46));
	(void)keiui_ui_widget(ui, id, CARDS_DIALOG_SELF, area, KEIUI_FOCUSABLE | KEIUI_MODAL);

	/* The dialog (itself or one of its buttons) has the keyboard while it shows. */
	focused = keiui_ui_focused(ui, &focus_id, &focus_index);
	if (!focused || focus_id != id)
		kl_ui_set_focus(ui, id, CARDS_DIALOG_SELF);

	/* The card's size: the title, up to three lines of body, and the buttons. */
	card.width = CARDS_DIALOG_WIDTH;
	if (card.width > area->width - 32)
		card.width = area->width - 32;
	card.height = CARDS_DIALOG_PAD + 16 + CARDS_BODY_GAP + CARDS_DIALOG_LINES * CARDS_BODY_LINE + 24 + theme->control_height + 16;
	card.x = area->x + (area->width - card.width) / 2;
	card.y = area->y + (area->height - card.height) / 2;

	/* The card with its soft shadow. */
	kl_canvas_shadow(style->canvas, (float)card.x, (float)card.y + 2.0f, (float)card.width, (float)card.height, 14.0f, 10.0f, KL_RGBA(0x1f3a66, 40));
	kl_canvas_round(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, theme->panel);
	kl_canvas_round_border(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, 1.0f, theme->panel_edge);

	/* The question, bold, and its explanation broken into lines. */
	baseline = card.y + CARDS_DIALOG_PAD + 16;
	(void)kl_text_draw_fit(style->text, style->canvas, card.x + CARDS_DIALOG_PAD, baseline, title, CARDS_TEXT_DIALOG, 1, card.width - 2 * CARDS_DIALOG_PAD, theme->text);
	done = 0;
	for (lines = 0; body != NULL && body[done] != '\0' && lines < CARDS_DIALOG_LINES; lines++) {
		length = kl_text_break(style->text, body + done, CARDS_TEXT_BODY, 0, card.width - 2 * CARDS_DIALOG_PAD);
		if (length == 0U)
			break;
		baseline += CARDS_BODY_LINE;
		if (lines == 0)
			baseline += CARDS_BODY_GAP;
		(void)kl_text_draw(style->text, style->canvas, card.x + CARDS_DIALOG_PAD, baseline, body + done, length, CARDS_TEXT_BODY, 0, theme->text_secondary);
		done += length;
		while (body[done] == ' ')
			done++;
	}

	/* The buttons from the right: the main one first. */
	chosen = -1;
	right = card.x + card.width - CARDS_DIALOG_PAD + 2;
	for (index = 0; index < count; index++) {
		button.width = kl_button_width(style, labels[index]);
		button.height = theme->control_height;
		button.x = right - button.width;
		button.y = card.y + card.height - 16 - button.height;
		flags = 0;
		if (index == 0)
			flags = KL_BUTTON_PRIMARY;
		pressed = keiui_button(ui, style, id, (uint32_t)index, &button, labels[index], flags);
		if (pressed)
			chosen = index;
		right = button.x - CARDS_DIALOG_GAP;
	}

	/* Enter (on none of the buttons) chooses the main button, Esc the last. */
	for (;;) {
		taken = keiui_ui_take_key(ui, id, KEIUI_ANY, cards_dialog_key, &code, &modifiers);
		if (!taken)
			break;
		if (code == KL_KEY_ESC)
			chosen = count - 1;
		else
			chosen = 0;
	}

	/* Reports the button chosen, or -1. */
	return chosen;
}

/*
 * Draws a chip with a message, its middle at centre_x and its bottom at
 * bottom (the message of a moment: saved, not found).
 */
void
kl_chip(
	const struct kl_style *style,
	int centre_x,
	int bottom,
	const char *message)
{
	struct kl_rect chip;
	int width;

	/* As wide as its words. */
	width = kl_text_width(style->text, message, strlen(message), CARDS_TEXT_CHIP, 0);
	chip.width = width + 28;
	chip.height = CARDS_CHIP_HEIGHT;
	chip.x = centre_x - chip.width / 2;
	chip.y = bottom - chip.height;

	/* A white chip with an edge and a soft shadow, the words in the middle. */
	kl_canvas_shadow(style->canvas, (float)chip.x, (float)chip.y + 2.0f, (float)chip.width, (float)chip.height, CARDS_CHIP_RADIUS, 8.0f, style->theme->shadow);
	kl_canvas_round(style->canvas, (float)chip.x, (float)chip.y, (float)chip.width, (float)chip.height, CARDS_CHIP_RADIUS, kl_theme_choose(KL_RGBA(0xffffff, 240), KL_RGBA(0x2c313b, 240)));
	kl_canvas_round_border(style->canvas, (float)chip.x, (float)chip.y, (float)chip.width, (float)chip.height, CARDS_CHIP_RADIUS, 1.0f, style->theme->panel_edge);
	(void)kl_text_draw(style->text, style->canvas, chip.x + 14, kl_text_center(CARDS_TEXT_CHIP, chip.y, chip.height), message, strlen(message), CARDS_TEXT_CHIP, 0, style->theme->text);
}

/*
 * Draws a progress bar: a share done (0 to 1), or, below 0, work of an
 * unknown length (a part that crosses the bar with the time).
 */
void
kl_progress(
	const struct kl_style *style,
	const struct kl_rect *rect,
	double fraction,
	uint64_t now_us)
{
	float radius;
	float start;
	float span;
	double phase;

	/* The track. */
	radius = (float)rect->height * 0.5f;
	kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, radius, style->theme->track);

	/* A share done: the accent up to it. */
	if (fraction >= 0.0) {
		if (fraction > 1.0)
			fraction = 1.0;
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)((double)rect->width * fraction), (float)rect->height, radius, style->theme->accent);
		return;
	}

	/* Unknown: a part of the accent crossing the bar, inside it. */
	phase = (double)(now_us % CARDS_PROGRESS_US) / (double)CARDS_PROGRESS_US;
	span = (float)((double)rect->width * CARDS_PROGRESS_SPAN);
	start = (float)rect->x + (float)(phase * ((double)rect->width + (double)span)) - span;
	kl_canvas_clip_push(style->canvas, rect);
	kl_canvas_round(style->canvas, start, (float)rect->y, span, (float)rect->height, radius, style->theme->accent);
	kl_canvas_clip_pop(style->canvas);
}

/* Tells whether a dialog takes a key: Enter and Esc. */
static int
cards_dialog_key(
	uint32_t code,
	unsigned modifiers)
{
	/* A command is the application's. */
	(void)modifiers;

	/* Enter and Esc answer the dialog. */
	if (code == KL_KEY_ENTER || code == KL_KEY_KPENTER || code == KL_KEY_ESC)
		return 1;
	return 0;
}
