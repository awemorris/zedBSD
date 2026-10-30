/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installer's screens, drawn with libkeiland's widgets in one panel:
 * the screen's title at the top with Cancel at its right, its content in
 * a scrolled pane, and a bar at the foot with Back, a dot a step, the
 * reason Next is not offered, and Next.  A dialog stands over the whole
 * window while it shows.  Every control is recorded for the log
 * (inst_control) as it is drawn.
 */

#include "installer.h"

#include "userland/desktop/artwork/mark.h"

#include <uapi/block.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The panel's margin, the pane's inset, the bar's height and the room between the parts. */
#define SCREEN_MARGIN		12
#define SCREEN_INSET		28
#define SCREEN_BAR		64
#define SCREEN_GAP		14

/* The rows of a choice, a card's padding, the dots of the steps and the mark. */
#define SCREEN_ROW		58
#define SCREEN_ROW_SMALL	40
#define SCREEN_PAD		16
#define SCREEN_DOT		8
#define SCREEN_DOT_GAP		10
#define SCREEN_MARK		88U
#define SCREEN_MARK_MAX		128U

/* The text sizes: the body, the small, a row's title. */
#define SCREEN_TEXT		14U
#define SCREEN_TEXT_SMALL	13U
#define SCREEN_TEXT_TITLE	15U

/* The lines of a licence a page shows at most. */
#define SCREEN_LICENSE_LINES	64

/* The widgets' ids: each kind of control has its own, rows an index each. */
enum screen_id {
	SCREEN_ID_NEXT = 1,
	SCREEN_ID_BACK,
	SCREEN_ID_CANCEL,
	SCREEN_ID_RETRY,
	SCREEN_ID_DISK,
	SCREEN_ID_RELOAD,
	SCREEN_ID_HANDLING,
	SCREEN_ID_PART,
	SCREEN_ID_DELETE,
	SCREEN_ID_SIZE,
	SCREEN_ID_CREATE,
	SCREEN_ID_ROOT,
	SCREEN_ID_SWAP,
	SCREEN_ID_ITEM,
	SCREEN_ID_LICENSES,
	SCREEN_ID_AGREE,
	SCREEN_ID_LICENSE_BEFORE,
	SCREEN_ID_LICENSE_AFTER,
	SCREEN_ID_PAGE_BEFORE,
	SCREEN_ID_PAGE_AFTER,
	SCREEN_ID_LICENSE_CLOSE,
	SCREEN_ID_FIELD,
	SCREEN_ID_AUTOLOGIN,
	SCREEN_ID_ERASE,
	SCREEN_ID_START_OVER,
	SCREEN_ID_CLOSE,
	SCREEN_ID_REBOOT,
	SCREEN_ID_DIALOG,
	SCREEN_ID_BLOCK,
	SCREEN_ID_PANE
};

/* The steps of an installation (admin/session.c's names) and their words. */
static const char *const screen_steps[][2] = {
	{ "check", "Checking the plan" },
	{ "claim", "Preparing the disk" },
	{ "table", "Writing the partition table" },
	{ "format", "Formatting the partitions" },
	{ "mount", "Mounting the system's partition" },
	{ "unpack", "Copying the system" },
	{ "settings", "Writing the settings" },
	{ "sync", "Writing everything to the disk" },
	{ "esp", "Making the disk start" },
	{ "finish", "Finishing" }
};

/* The dots of the steps: the screens a person walks (S9 to S11 are the end's). */
#define SCREEN_WALKED		9

static void screen_ground(struct inst_app *app, struct kl_rect *panel);
static int screen_title(struct inst_app *app, const struct kl_rect *panel, const char *title, const char *summary);
static void screen_bar(struct inst_app *app, const struct kl_rect *panel);
static void screen_pane(struct inst_app *app, uint64_t now_us);
static int screen_body(struct inst_app *app, int x, int top, int width, uint64_t now_us);
static void screen_dialog(struct inst_app *app);
static int screen_lines(struct inst_app *app, int x, int top, int width, const char *text, unsigned pixels, int bold, kl_color color, int most);
static int screen_card(struct inst_app *app, int x, int top, int width, const char *title, const char *text, kl_color edge);
static int screen_choice(struct inst_app *app, uint32_t id, uint32_t index, const struct kl_rect *rect, int selected, int enabled, const char *title, const char *detail, const char *name);
static int screen_button(struct inst_app *app, uint32_t id, int x, int y, const char *label, unsigned flags, const char *name, int *width);
static void screen_mark(struct inst_app *app, int x, int y, unsigned pixels);
static int screen_welcome(struct inst_app *app, int x, int top, int width);
static int screen_disks(struct inst_app *app, int x, int top, int width);
static int screen_handling(struct inst_app *app, int x, int top, int width);
static int screen_edit(struct inst_app *app, int x, int top, int width);
static int screen_root(struct inst_app *app, int x, int top, int width);
static int screen_swap(struct inst_app *app, int x, int top, int width);
static int screen_items(struct inst_app *app, int x, int top, int width);
static int screen_license(struct inst_app *app, int x, int top, int width);
static int screen_user(struct inst_app *app, int x, int top, int width);
static int screen_confirm(struct inst_app *app, int x, int top, int width);
static int screen_progress(struct inst_app *app, int x, int top, int width, uint64_t now_us);
static int screen_failed(struct inst_app *app, int x, int top, int width);
static int screen_done(struct inst_app *app, int x, int top, int width);
static void screen_titles(const struct inst_app *app, const char **title, const char **summary, char *named, size_t size);
static const struct inst_disk *screen_disk(const struct inst_app *app);
static void screen_disk_line(const struct inst_app *app, const struct inst_disk *disk, char *text, size_t size);
static void screen_part_line(const struct inst_disk *disk, const struct inst_part *part, char *text, size_t size);
static int screen_part_rows(struct inst_app *app, uint32_t id, int x, int top, int width, int swap);
static int screen_field_row(struct inst_app *app, uint32_t index, int x, int top, int width, const char *label, struct kl_field *field, const char *placeholder, const char *name);
static int screen_item_row(struct inst_app *app, unsigned item, int x, int top, int width, int depth);
static const char *screen_step_words(const char *name);
static void screen_summary_row(struct inst_app *app, int x, int *top, int width, const char *label, const char *value);

/*
 * Draws the frame: the ground, the panel, the screen's title and
 * content, the bar, and a dialog over them.
 */
void
inst_draw(
	struct inst_app *app,
	uint64_t now_us)
{
	const char *summary;
	const char *title;
	char named[192];
	struct kl_rect panel;
	struct kl_rect block;

	/* The ground and the panel. */
	screen_ground(app, &panel);

	/* The pane between the title and the bar; its controls are recorded first (under what stands on its edges). */
	screen_titles(app, &title, &summary, named, sizeof(named));
	app->content.x = panel.x + SCREEN_INSET;
	app->content.y = panel.y + 22 + 62;
	app->content.width = panel.width - 2 * SCREEN_INSET;
	app->content.height = panel.y + panel.height - SCREEN_BAR - 6 - app->content.y;
	screen_pane(app, now_us);

	/* The title's and the bar's ground take the presses meant for nothing (a control scrolled under them). */
	block.x = panel.x;
	block.y = panel.y;
	block.width = panel.width;
	block.height = app->content.y - panel.y;
	(void)kl_ui_hit_still(app->ui, SCREEN_ID_BLOCK, 0U, &block);
	block.y = app->content.y + app->content.height;
	block.height = panel.y + panel.height - block.y;
	(void)kl_ui_hit_still(app->ui, SCREEN_ID_BLOCK, 1U, &block);

	/* The title, and the bar. */
	(void)screen_title(app, &panel, title, summary);
	screen_bar(app, &panel);

	/* A dialog over everything. */
	screen_dialog(app);
}

/* Draws the window's ground and the panel; gives the panel. */
static void
screen_ground(
	struct inst_app *app,
	struct kl_rect *panel)
{
	const struct kl_theme *theme;
	struct kl_rect whole;

	/* A quiet gradient under the panel. */
	theme = app->style.theme;
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)app->width;
	whole.height = (int)app->height;
	kl_canvas_gradient(&app->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The panel. */
	panel->x = SCREEN_MARGIN;
	panel->y = SCREEN_MARGIN;
	panel->width = (int)app->width - 2 * SCREEN_MARGIN;
	panel->height = (int)app->height - 2 * SCREEN_MARGIN;
	kl_panel(&app->style, panel, 0);
}

/* Draws the screen's title and summary, and Cancel at the right; returns the edge below. */
static int
screen_title(
	struct inst_app *app,
	const struct kl_rect *panel,
	const char *title,
	const char *summary)
{
	unsigned flags;
	int cancel_width;
	int bottom;
	int enabled;
	int pressed;

	/* Cancel, at the top right, while there is something to cancel. */
	cancel_width = 0;
	if (app->screen != INST_S10 && app->screen != INST_S11) {
		enabled = 1;
		if (app->screen == INST_S9 && app->stop_asked)
			enabled = 0;
		(void)screen_button(app, SCREEN_ID_CANCEL, 0, 0, kl_tr("Cancel"), 0U, NULL, &cancel_width);

		/* A Cancel that cannot be pressed is faded. */
		flags = 0U;
		if (!enabled)
			flags = KL_BUTTON_DISABLED;
		pressed = screen_button(app, SCREEN_ID_CANCEL, panel->x + panel->width - SCREEN_INSET - cancel_width, panel->y + 22, kl_tr("Cancel"), flags, "cancel", NULL);
		if (pressed)
			inst_cancel_asked(app);
	}

	/* The title and its summary. */
	bottom = kl_header(&app->style, panel->x + SCREEN_INSET, panel->y + 22, panel->width - 2 * SCREEN_INSET - cancel_width - 16, title, summary);

	/* Reports the edge below them. */
	return bottom;
}

/* Draws the bar at the foot: Back, the dots, the reason Next waits, and Next (or the last screens' buttons). */
static void
screen_bar(
	struct inst_app *app,
	const struct kl_rect *panel)
{
	const struct kl_theme *theme;
	const char *reason;
	const char *label;
	struct kl_rect line;
	kl_color color;
	unsigned flags;
	int button_y;
	int next_width;
	int left;
	int right;
	int dots_width;
	int dots_x;
	int step;
	int width;
	int y;
	int pressed;

	/* A thin line over the bar. */
	theme = app->style.theme;
	y = panel->y + panel->height - SCREEN_BAR;
	line.x = panel->x + SCREEN_INSET;
	line.y = y;
	line.width = panel->width - 2 * SCREEN_INSET;
	line.height = 1;
	kl_canvas_fill(&app->canvas, &line, theme->separator);
	button_y = y + (SCREEN_BAR - theme->control_height) / 2;
	left = panel->x + SCREEN_INSET;
	right = panel->x + panel->width - SCREEN_INSET;

	/* The last screens: their own buttons at the right. */
	if (app->screen == INST_S10 || app->screen == INST_S11) {
		(void)screen_button(app, SCREEN_ID_CLOSE, 0, 0, kl_tr("Close"), 0U, NULL, &width);
		pressed = screen_button(app, SCREEN_ID_CLOSE, right - width, button_y, kl_tr("Close"), 0U, "close", NULL);
		if (pressed)
			app->quit = 1;
		right -= width + 10;
		if (app->screen == INST_S10) {
			(void)screen_button(app, SCREEN_ID_START_OVER, 0, 0, kl_tr("Start over"), 0U, NULL, &width);
			pressed = screen_button(app, SCREEN_ID_START_OVER, right - width, button_y, kl_tr("Start over"), KL_BUTTON_PRIMARY, "start-over", NULL);
			if (pressed)
				inst_start_over(app);
		} else {
			(void)screen_button(app, SCREEN_ID_REBOOT, 0, 0, kl_tr("Restart"), 0U, NULL, &width);
			pressed = screen_button(app, SCREEN_ID_REBOOT, right - width, button_y, kl_tr("Restart"), KL_BUTTON_PRIMARY, "reboot", NULL);
			if (pressed)
				inst_reboot(app);
		}

		/* The last screens have no Back nor Next. */
		return;
	}

	/* The progress has no Back nor Next. */
	if (app->screen == INST_S9)
		return;

	/* Back, but on the first screen. */
	if (app->screen != INST_S1) {
		pressed = screen_button(app, SCREEN_ID_BACK, left, button_y, kl_tr("Back"), 0U, "back", &width);
		if (pressed)
			inst_back(app);
		left += width + 12;
	}

	/* Next ("Install" on S8, which writes), faded with its reason when the screen is not ready. */
	reason = inst_next_refusal(app);
	label = kl_tr("Next");
	flags = KL_BUTTON_PRIMARY;
	if (app->screen == INST_S8) {
		label = kl_tr("Install");
		flags = KL_BUTTON_DANGER;
	}

	/* A screen that is not ready fades Next. */
	if (reason != NULL)
		flags |= KL_BUTTON_DISABLED;

	/* Measures the button, then draws it at the right. */
	(void)screen_button(app, SCREEN_ID_NEXT, 0, 0, label, 0U, NULL, &next_width);
	pressed = screen_button(app, SCREEN_ID_NEXT, right - next_width, button_y, label, flags, "next", NULL);
	if (pressed)
		inst_next(app);
	right -= next_width + 14;

	/* The reason, in the middle's place. */
	if (reason != NULL && reason[0] != '\0') {
		(void)screen_lines(app, left, y + 14, right - left, reason, SCREEN_TEXT_SMALL, 0, theme->text_secondary, 2);
		return;
	}

	/* The dots, in the middle, when they fit. */
	dots_width = SCREEN_WALKED * SCREEN_DOT + (SCREEN_WALKED - 1) * SCREEN_DOT_GAP;
	dots_x = panel->x + (panel->width - dots_width) / 2;
	if (dots_x < left || dots_x + dots_width > right)
		return;
	for (step = 0; step < SCREEN_WALKED; step++) {
		color = theme->text_faint;
		if (step == (int)app->screen)
			color = theme->accent;
		kl_canvas_circle(&app->canvas, (float)(dots_x + step * (SCREEN_DOT + SCREEN_DOT_GAP) + SCREEN_DOT / 2), (float)(y + SCREEN_BAR / 2),
				 (float)SCREEN_DOT / 2.0f, color);
	}
}

/* Draws the pane: its content scrolled, clipped to it, with the scroll's bar. */
static void
screen_pane(
	struct inst_app *app,
	uint64_t now_us)
{
	int bottom;
	int top;

	/* The viewport takes the wheel and a finger's drag; the content stands on it. */
	kl_ui_scroll_region(app->ui, SCREEN_ID_PANE, &app->content, &app->scroll);
	kl_canvas_clip_push(&app->canvas, &app->content);
	top = app->content.y - (int)app->scroll.y;
	bottom = screen_body(app, app->content.x, top, app->content.width - 12, now_us);
	kl_canvas_clip_pop(&app->canvas);

	/* The content's height for the next frame's scroll, and its bar. */
	app->content_height = bottom - top + SCREEN_GAP;
	kl_scroll_set_size(&app->scroll, (double)app->content.width, (double)app->content_height, (double)app->content.width,
			   (double)app->content.height);
	(void)kl_scroll_draw_bars(&app->scroll, &app->canvas, &app->content, app->style.theme, now_us);
}

/* Draws the content of the screen shown; returns the edge below it. */
static int
screen_body(
	struct inst_app *app,
	int x,
	int top,
	int width,
	uint64_t now_us)
{
	/* By the screen. */
	switch (app->screen) {
	case INST_S1:
		return screen_welcome(app, x, top, width);
	case INST_S2:
		return screen_disks(app, x, top, width);
	case INST_S3:
		return screen_handling(app, x, top, width);
	case INST_S4:
		return screen_edit(app, x, top, width);
	case INST_S5:
		return screen_root(app, x, top, width);
	case INST_S6:
		return screen_swap(app, x, top, width);
	case INST_S7:
		if (app->license_view)
			return screen_license(app, x, top, width);
		return screen_items(app, x, top, width);
	case INST_S7A:
		return screen_user(app, x, top, width);
	case INST_S8:
		return screen_confirm(app, x, top, width);
	case INST_S9:
		return screen_progress(app, x, top, width, now_us);
	case INST_S10:
		return screen_failed(app, x, top, width);
	default:
		break;
	}

	/* S11. */
	return screen_done(app, x, top, width);
}

/* Draws the dialog shown, and takes its answer. */
static void
screen_dialog(
	struct inst_app *app)
{
	static const char *const labels_cancel[] = { "Cancel installation", "Keep going" };
	static const char *const labels_notice[] = { "OK" };
	static const char *const labels_delete[] = { "Delete", "Keep" };
	static const char *const labels_erase[] = { "Erase", "Keep" };
	static const char *const labels_continue[] = { "Continue", "Back" };
	static const char *const labels_leave[] = { "Leave out", "Keep" };
	static const char *const labels_install[] = { "Install", "Not yet" };
	static const char *const labels_stop[] = { "Stop", "Keep going" };
	const struct inst_disk *disk;
	const struct inst_part *part;
	const char *const *english;
	const char *labels[2];
	const char *title;
	char heading[160];
	char body[512];
	char line[256];
	struct kl_rect whole;
	int chosen;
	int count;
	int i;

	/* The dialog shown, its words. */
	disk = screen_disk(app);
	body[0] = '\0';
	heading[0] = '\0';
	count = 2;
	switch (app->dialog) {
	case INST_DIALOG_CANCEL:
		title = kl_tr("Cancel the installation?");
		(void)snprintf(body, sizeof(body), "%s", kl_tr("Nothing more is written. What was already written to the disk stays."));
		english = labels_cancel;
		break;
	case INST_DIALOG_EDIT_NOTICE:
		title = kl_tr("Changes here are written at once");
		(void)snprintf(body, sizeof(body), "%s",
			       kl_tr("Deleting or creating a partition on this screen writes to the disk when you press the button. Cancelling later does not undo it."));
		english = labels_notice;
		count = 1;
		break;
	case INST_DIALOG_DELETE:
	case INST_DIALOG_ERASE_ROOT:
	case INST_DIALOG_ERASE_SWAP:
		/* The partition, as the screen shows it. */
		part = NULL;
		if (app->dialog == INST_DIALOG_DELETE && app->has_selected)
			part = inst_part_find(&app->probe, &app->selected);
		if (app->dialog != INST_DIALOG_DELETE)
			part = inst_part_find(&app->probe, &app->pending);
		line[0] = '\0';
		if (part != NULL && disk != NULL)
			screen_part_line(disk, part, line, sizeof(line));
		if (app->dialog == INST_DIALOG_DELETE) {
			(void)snprintf(heading, sizeof(heading), "%s", kl_tr("Delete this partition?"));
			(void)kl_tr_format(body, sizeof(body), kl_tr("{1}. The partition and everything on it are erased."), line, (const char *)NULL);
			english = labels_delete;
		} else {
			(void)snprintf(heading, sizeof(heading), "%s", kl_tr("Erase this partition?"));
			(void)kl_tr_format(body, sizeof(body), kl_tr("{1}. It is formatted: everything on it is erased."), line, (const char *)NULL);
			english = labels_erase;
		}

		/* The heading was written into the buffer above. */
		title = heading;
		break;
	case INST_DIALOG_NO_SWAP:
		title = kl_tr("No swap partition");
		(void)snprintf(body, sizeof(body), "%s", kl_tr("A swap file is made on the system's partition instead."));
		english = labels_continue;
		break;
	case INST_DIALOG_DROP_DESKTOP:
		title = kl_tr("Leave the desktop out?");
		(void)snprintf(body, sizeof(body), "%s", kl_tr("The installed system then has only the console's login, without the graphical desktop."));
		english = labels_leave;
		break;
	case INST_DIALOG_DROP_FIRMWARE:
		title = kl_tr("Leave the firmware out?");
		(void)snprintf(body, sizeof(body), "%s", kl_tr("Devices that need firmware, such as Wi-Fi and graphics, may not work."));
		english = labels_leave;
		break;
	case INST_DIALOG_START:
		title = kl_tr("Write to the disk now?");
		line[0] = '\0';
		if (disk != NULL)
			(void)snprintf(line, sizeof(line), "%s", disk->name);
		(void)kl_tr_format(body, sizeof(body), kl_tr("The installation starts and writes to {1}."), line, (const char *)NULL);
		english = labels_install;
		break;
	case INST_DIALOG_STOP:
		title = kl_tr("Stop the installation?");
		(void)snprintf(body, sizeof(body), "%s", kl_tr("It stops at the end of the step it is in. The disk is left partly written and does not start."));
		english = labels_stop;
		break;
	default:
		return;
	}

	/* The buttons in the language. */
	for (i = 0; i < count; i++)
		labels[i] = kl_tr(english[i]);

	/* The dialog over the window; Enter chooses the main button, Esc the last. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)app->width;
	whole.height = (int)app->height;
	chosen = kl_dialog(app->ui, &app->style, SCREEN_ID_DIALOG, &whole, title, body, labels, count);
	if (chosen >= 0)
		inst_dialog_answer(app, chosen);
}

/*
 * Draws a text broken into lines of a column, at most so many; returns the
 * edge below the last line.
 */
static int
screen_lines(
	struct inst_app *app,
	int x,
	int top,
	int width,
	const char *text,
	unsigned pixels,
	int bold,
	kl_color color,
	int most)
{
	struct kl_text_line metrics;
	size_t done;
	size_t length;
	int baseline;
	int lines;

	/* Each line of the column, its words kept whole. */
	kl_text_metrics(&app->text, pixels, &metrics);
	baseline = top + metrics.ascent;
	done = 0;
	for (lines = 0; text[done] != '\0' && lines < most; lines++) {
		length = kl_text_break(&app->text, text + done, pixels, bold, width);
		if (length == 0U)
			break;
		(void)kl_text_draw(&app->text, &app->canvas, x, baseline, text + done, length, pixels, bold, color);
		done += length;
		while (text[done] == ' ' || text[done] == '\n')
			done++;
		baseline += metrics.height + 4;
	}

	/* Reports the edge below the lines. */
	return baseline - metrics.ascent;
}

/* Draws a card with a bold title and a text (an edge of a colour for a warning); returns the edge below. */
static int
screen_card(
	struct inst_app *app,
	int x,
	int top,
	int width,
	const char *title,
	const char *text,
	kl_color edge)
{
	const struct kl_theme *theme;
	struct kl_rect nowhere;
	int height;
	int y;

	/* The card's height, from its text's lines (measured by drawing them where the clip shows nothing). */
	theme = app->style.theme;
	nowhere.x = 0;
	nowhere.y = 0;
	nowhere.width = 0;
	nowhere.height = 0;
	kl_canvas_clip_push(&app->canvas, &nowhere);
	y = screen_lines(app, x + SCREEN_PAD, top + SCREEN_PAD, width - 2 * SCREEN_PAD, title, SCREEN_TEXT_TITLE, 1, theme->text, 3);
	y = screen_lines(app, x + SCREEN_PAD, y + 6, width - 2 * SCREEN_PAD, text, SCREEN_TEXT, 0, theme->text_secondary, 12);
	kl_canvas_clip_pop(&app->canvas);
	height = y - top + SCREEN_PAD;

	/* The card, then its words. */
	kl_canvas_round(&app->canvas, (float)x, (float)top, (float)width, (float)height, theme->card_radius, theme->card);
	kl_canvas_round_border(&app->canvas, (float)x, (float)top, (float)width, (float)height, theme->card_radius, 1.5f, edge);
	y = screen_lines(app, x + SCREEN_PAD, top + SCREEN_PAD, width - 2 * SCREEN_PAD, title, SCREEN_TEXT_TITLE, 1, theme->text, 3);
	(void)screen_lines(app, x + SCREEN_PAD, y + 6, width - 2 * SCREEN_PAD, text, SCREEN_TEXT, 0, theme->text_secondary, 12);

	/* Reports the edge below the card. */
	return top + height;
}

/*
 * Draws a row of a choice: a ring (filled when selected), its title and a
 * line of detail, lit under the pointer; faded and unpressable when not
 * enabled.  Reports 1 when it was clicked.
 */
static int
screen_choice(
	struct inst_app *app,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect,
	int selected,
	int enabled,
	const char *title,
	const char *detail,
	const char *name)
{
	const struct kl_theme *theme;
	kl_color ground;
	kl_color edge;
	kl_color ink;
	kl_color secondary;
	unsigned hit;
	float border;
	float cx;
	float cy;

	/* Its input, while it is enabled. */
	theme = app->style.theme;
	hit = 0;
	if (enabled)
		hit = kl_ui_hit(app->ui, id, index, rect);
	inst_control(app, name, rect, enabled);

	/* Its ground: the card's, lit under the pointer, the accent's edge when selected. */
	ground = theme->card;
	if ((hit & KL_HIT_HOT) != 0U)
		ground = kl_color_mix(theme->card, theme->accent, 0.08f);
	edge = theme->card_edge;
	border = 1.0f;
	if (selected) {
		edge = theme->accent;
		border = 2.0f;
	}

	/* The row's body and its border. */
	kl_canvas_round(&app->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, ground);
	kl_canvas_round_border(&app->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, border, edge);

	/* The ring at the left, faded when the row cannot be chosen. */
	cx = (float)(rect->x + 22);
	cy = (float)(rect->y + rect->height / 2);
	secondary = theme->text_secondary;
	if (!enabled)
		secondary = theme->text_faint;
	kl_canvas_ring(&app->canvas, cx, cy, 8.0f, 1.5f, 1.0f, secondary);
	if (selected)
		kl_canvas_circle(&app->canvas, cx, cy, 5.0f, theme->accent);

	/* The title in full ink, and the detail under it in the secondary ink; both faint when not enabled. */
	ink = theme->text;
	if (!enabled)
		ink = theme->text_faint;
	if (detail == NULL || detail[0] == '\0') {
		(void)kl_text_draw_fit(&app->text, &app->canvas, rect->x + 42, kl_text_center(SCREEN_TEXT_TITLE, rect->y, rect->height), title,
				       SCREEN_TEXT_TITLE, 1, rect->width - 54, ink);
	} else {
		(void)kl_text_draw_fit(&app->text, &app->canvas, rect->x + 42, rect->y + rect->height / 2 - 4, title, SCREEN_TEXT_TITLE, 1,
				       rect->width - 54, ink);
		(void)kl_text_draw_fit(&app->text, &app->canvas, rect->x + 42, rect->y + rect->height / 2 + 15, detail, SCREEN_TEXT_SMALL, 0,
				       rect->width - 54, secondary);
	}

	/* Reports a click (the frame it changes is drawn next). */
	if ((hit & KL_HIT_CLICKED) != 0U) {
		app->dirty = 1;
		return 1;
	}

	/* Not clicked. */
	return 0;
}

/*
 * Draws a button of its own width at a place (recorded for the log when
 * named, measured only when name is NULL and width asked); reports 1 when
 * it was pressed.
 */
static int
screen_button(
	struct inst_app *app,
	uint32_t id,
	int x,
	int y,
	const char *label,
	unsigned flags,
	const char *name,
	int *width)
{
	struct kl_rect rect;
	int pressed;

	/* Its size. */
	rect.x = x;
	rect.y = y;
	rect.width = kl_button_width(&app->style, label);
	rect.height = app->style.theme->control_height;
	if (width != NULL)
		*width = rect.width;

	/* Only measured. */
	if (name == NULL)
		return 0;

	/* The button, and its place for the log. */
	pressed = kl_button(app->ui, &app->style, id, &rect, label, flags);
	inst_control(app, name, &rect, (flags & KL_BUTTON_DISABLED) == 0U);
	if ((flags & KL_BUTTON_DISABLED) != 0U)
		return 0;

	/* Reports a press (the frame it changes is drawn next). */
	if (pressed)
		app->dirty = 1;
	return pressed;
}

/* Draws the system's mark (the same colours as Settings' About). */
static void
screen_mark(
	struct inst_app *app,
	int x,
	int y,
	unsigned pixels)
{
	/*
	 * The layers' colours and opacities, the bar's first.  The table is
	 * constant for the life of the program.
	 */
	static const uint32_t colours[KL_MARK_LAYERS][2] = {
		{ 0xa9c3f6U, 175U },
		{ 0x7fa2f0U, 90U },
		{ 0xa3d8faU, 170U },
		{ 0x3a86f5U, 170U },
		{ 0x2f7cf3U, 200U },
		{ 0xffffffU, 170U },
		{ 0xffffffU, 60U }
	};

	/*
	 * The layers at the size drawn last (zero before the first); another
	 * size renders them again.  They live for the program's run.
	 */
	static uint8_t layers[KL_MARK_LAYERS][SCREEN_MARK_MAX * SCREEN_MARK_MAX];
	static unsigned layers_pixels;
	unsigned layer;

	/* A size the layers hold. */
	if (pixels > SCREEN_MARK_MAX)
		pixels = SCREEN_MARK_MAX;

	/* The layers at this size. */
	if (layers_pixels != pixels) {
		for (layer = 0; layer < KL_MARK_LAYERS; layer++)
			kl_mark_raster(layer, pixels, layers[layer], pixels);
		layers_pixels = pixels;
	}

	/* Each layer in its colour. */
	for (layer = 0; layer < KL_MARK_LAYERS; layer++)
		kl_canvas_mask(&app->canvas, x, y, layers[layer], (int)pixels, (int)pixels, pixels, KL_RGBA(colours[layer][0], colours[layer][1]));
}

/* S1: the mark, what the installer does, the warning, the rule of the image, and the probe's state. */
static int
screen_welcome(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	char words[256];
	int pressed;
	int y;

	/* The mark and the greeting beside it. */
	theme = app->style.theme;
	screen_mark(app, x, top, SCREEN_MARK);
	(void)kl_tr_format(words, sizeof(words), kl_tr("{1} will be installed on a disk of this computer."), app->os_name, (const char *)NULL);
	y = screen_lines(app, x + (int)SCREEN_MARK + 24, top + 18, width - (int)SCREEN_MARK - 24, words, 20U, 1, theme->text, 2);
	(void)screen_lines(app, x + (int)SCREEN_MARK + 24, y + 6, width - (int)SCREEN_MARK - 24,
			   kl_tr("The installer asks which disk, how to use it and what to install, then writes it. Nothing is written before you confirm."),
			   SCREEN_TEXT, 0, theme->text_secondary, 4);
	y = top + (int)SCREEN_MARK + SCREEN_GAP + 6;

	/* The warning. */
	y = screen_card(app, x, y, width, kl_tr("Back up your data first"),
			kl_tr("Installing may erase what is on the disk you choose. Copy anything you want to keep to another place before you go on."),
			theme->card_edge);

	/* The rule of the image. */
	y = screen_card(app, x, y + SCREEN_GAP, width, kl_tr("This image is for installing only"),
			kl_tr("Do not use it as your everyday system: do not keep starting the computer from it, copy it to an internal disk, offer network services from it or keep your files on it. In a system started from this image every program has the same power as root."),
			theme->danger);
	y += SCREEN_GAP;

	/* The probe: running, failed (with Retry), or not on UEFI. */
	if (app->probe_state == 1) {
		y = screen_lines(app, x, y, width, kl_tr("Looking at the disks..."), SCREEN_TEXT, 0, theme->text_secondary, 1);
	} else if (app->probe_state == 3) {
		(void)snprintf(words, sizeof(words), "%s %s", kl_tr("The disks could not be read:"), app->probe_message);
		y = screen_lines(app, x, y, width, words, SCREEN_TEXT, 0, theme->bad, 3);
		pressed = screen_button(app, SCREEN_ID_RETRY, x, y + 8, kl_tr("Try again"), 0U, "retry", NULL);
		if (pressed)
			inst_probe_again(app);
		y += 8 + theme->control_height;
	} else if (app->probe_state == 2 && !app->probe.uefi) {
		y = screen_lines(app, x, y, width, kl_tr("This computer was not started with UEFI. The installer installs only on a computer started with UEFI."),
				 SCREEN_TEXT, 0, theme->bad, 3);
	}

	/* The language. */
	y = screen_lines(app, x, y + 6, width, kl_tr("The installer's words follow the desktop's language (Settings, Languages)."), SCREEN_TEXT_SMALL, 0,
			 theme->text_faint, 2);

	/* Reports the edge below. */
	return y;
}

/* S2: a row each disk, with why one cannot be chosen; Reload. */
static int
screen_disks(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct inst_disk *disk;
	const char *reason;
	struct kl_rect rect;
	char title[192];
	char detail[256];
	char name[32];
	const char *table;
	int none_differs;
	unsigned flags;
	unsigned i;
	int clicked;
	int selected;
	int enabled;
	int pressed;
	int y;

	/* Each disk. */
	y = top;
	for (i = 0; i < app->probe.disk_count; i++) {
		disk = &app->probe.disk[i];
		screen_disk_line(app, disk, title, sizeof(title));

		/* The detail: why the disk cannot be chosen, else its partition table. */
		reason = inst_disk_refusal(app, disk);
		if (reason != NULL) {
			(void)snprintf(detail, sizeof(detail), "%s", kl_tr(reason));
		} else {
			/* The table's name, or "none" in the language shown when the disk has no table. */
			table = disk->table;
			none_differs = strcmp(disk->table, "none");
			if (disk->table[0] == '\0' || none_differs == 0)
				table = kl_tr("none");

			/* The line under the disk's name. */
			(void)snprintf(detail, sizeof(detail), "%s: %s, %u %s", kl_tr("Partition table"), table, disk->entries, kl_tr("partitions"));
		}

		/* The row, chosen when it is the disk now selected. */
		rect.x = x;
		rect.y = y;
		rect.width = width;
		rect.height = SCREEN_ROW;
		(void)snprintf(name, sizeof(name), "disk-%s", disk->name);
		selected = app->disk == disk->registration;
		enabled = reason == NULL;
		clicked = screen_choice(app, SCREEN_ID_DISK, i, &rect, selected, enabled, title, detail, name);
		if (clicked) {
			app->disk = disk->registration;
			inst_log("DISK chosen name=%s registration=%u", disk->name, (unsigned)disk->registration);
		}

		/* The next row, below this one. */
		y += SCREEN_ROW + 8;
	}

	/* No disk at all. */
	if (app->probe.disk_count == 0U && app->probe_state == 2)
		y = screen_lines(app, x, y, width, kl_tr("No disk was found. Connect one, then press Reload."), SCREEN_TEXT, 0, app->style.theme->text_secondary, 2);

	/* Reload: the disks asked for again. */
	flags = 0U;
	enabled = inst_helper_busy(app);
	if (enabled)
		flags = KL_BUTTON_DISABLED;
	pressed = screen_button(app, SCREEN_ID_RELOAD, x, y + 6, kl_tr("Reload"), flags, "reload", NULL);
	if (pressed)
		inst_probe_again(app);

	/* Reports the edge below. */
	return y + 6 + app->style.theme->control_height;
}

/* S3: the whole disk, or partitions chosen (with why not), and what the whole disk erases. */
static int
screen_handling(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	const struct inst_disk *disk;
	const struct inst_part *part;
	const char *reason;
	const char *detail;
	struct kl_rect rect;
	char line[256];
	unsigned i;
	int clicked;
	int enabled;
	int y;

	/* The disk. */
	theme = app->style.theme;
	disk = screen_disk(app);
	if (disk == NULL)
		return top;

	/* The whole disk. */
	rect.x = x;
	rect.y = top;
	rect.width = width;
	rect.height = SCREEN_ROW;
	clicked = screen_choice(app, SCREEN_ID_HANDLING, 0U, &rect, app->whole == 1, 1, kl_tr("Use the whole disk"),
				kl_tr("Every partition on the disk is erased; an EFI system partition, a swap partition and the system's partition are made."), "whole");
	if (clicked)
		app->whole = 1;
	y = top + SCREEN_ROW + 8;

	/* What it erases. */
	if (disk->part_count > 0U) {
		y = screen_lines(app, x + 42, y + 4, width - 42, kl_tr("These partitions are erased:"), SCREEN_TEXT_SMALL, 1, theme->bad, 1);
		for (i = 0; i < disk->part_count; i++) {
			part = &app->probe.part[disk->first_part + i];
			screen_part_line(disk, part, line, sizeof(line));
			y = screen_lines(app, x + 54, y + 2, width - 54, line, SCREEN_TEXT_SMALL, 0, theme->text_secondary, 1);
		}

		/* Room between the list and the next choice. */
		y += 8;
	}

	/* Partitions chosen, or why not. */
	reason = inst_chosen_refusal(app, disk);
	rect.y = y + 6;
	enabled = reason == NULL;
	if (reason != NULL)
		detail = kl_tr(reason);
	else
		detail = kl_tr("Keep the partitions there are; delete or create some, then choose where the system goes.");
	clicked = screen_choice(app, SCREEN_ID_HANDLING, 1U, &rect, app->whole == 0, enabled, kl_tr("Choose partitions"), detail, "chosen");
	if (clicked)
		app->whole = 0;

	/* Reports the edge below. */
	return rect.y + SCREEN_ROW;
}

/* S4: the table, Delete, the size and Create, and what the last write said. */
static int
screen_edit(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	const struct inst_disk *disk;
	const struct inst_part *part;
	struct inst_tuple tuple;
	struct kl_rect rect;
	char title[256];
	char detail[256];
	char name[32];
	char size[32];
	kl_color ink;
	uint64_t gap_start;
	uint64_t gap_sectors;
	unsigned changes;
	unsigned flags;
	unsigned i;
	int selected;
	int busy;
	int esp;
	int error;
	int width_button;
	int is_esp;
	int unassigned;
	int clicked;
	int pressed;
	int create;
	int y;

	/* The disk. */
	theme = app->style.theme;
	disk = screen_disk(app);
	if (disk == NULL)
		return top;
	busy = inst_helper_busy(app);

	/* Each partition, selectable. */
	y = top;
	esp = 0;
	for (i = 0; i < disk->part_count; i++) {
		part = &app->probe.part[disk->first_part + i];
		is_esp = inst_part_esp(part);
		if (is_esp)
			esp = 1;

		/* The detail: a partition made here, or one the system holds mounted. */
		screen_part_line(disk, part, title, sizeof(title));
		detail[0] = '\0';
		unassigned = inst_part_unassigned(part);
		if (unassigned)
			(void)snprintf(detail, sizeof(detail), "%s", kl_tr("New, not used yet"));
		else if (part->mounted)
			(void)snprintf(detail, sizeof(detail), "%s", kl_tr("Mounted now (it is unmounted before a write)"));

		/* A row with a detail is taller. */
		rect.x = x;
		rect.y = y;
		rect.width = width;
		rect.height = SCREEN_ROW_SMALL;
		if (detail[0] != '\0')
			rect.height = SCREEN_ROW;

		/* The row is selected when it is the partition now selected. */
		selected = 0;
		if (app->has_selected) {
			inst_tuple_of(part, &tuple);
			selected = inst_tuple_same(&tuple, &app->selected);
		}

		/* Selects the partition when its row is clicked. */
		(void)snprintf(name, sizeof(name), "part-%u", part->slot);
		clicked = screen_choice(app, SCREEN_ID_PART, i, &rect, selected, !busy, title, detail, name);
		if (clicked) {
			inst_tuple_of(part, &app->selected);
			app->has_selected = 1;
		}

		/* The next row, below this one. */
		y += rect.height + 6;
	}

	/* The largest free room. */
	error = inst_largest_gap(disk, &app->probe, &gap_start, &gap_sectors);
	if (error == 0)
		inst_bytes_text(gap_sectors * disk->sector_bytes, size, sizeof(size));
	else
		(void)snprintf(size, sizeof(size), "%s", "0 MiB");
	(void)kl_tr_format(title, sizeof(title), kl_tr("Free space: {1} at most in one place"), size, (const char *)NULL);
	y = screen_lines(app, x, y + 6, width, title, SCREEN_TEXT, 0, theme->text_secondary, 1);

	/* Delete: the partition selected. */
	flags = KL_BUTTON_DANGER;
	if (!app->has_selected || busy)
		flags |= KL_BUTTON_DISABLED;
	y += 10;
	pressed = screen_button(app, SCREEN_ID_DELETE, x, y, kl_tr("Delete partition"), flags, "delete", &width_button);
	if (pressed)
		app->dialog = INST_DIALOG_DELETE;

	/* The size, its default the largest room (less the ESP's when one is made), and Create. */
	if (app->create_size.length == 0U && error == 0 && !busy) {
		gap_sectors = gap_sectors * disk->sector_bytes / INST_MIB;
		if (!esp && gap_sectors > INST_ESP_BYTES / INST_MIB)
			gap_sectors -= INST_ESP_BYTES / INST_MIB;
		(void)snprintf(size, sizeof(size), "%llu MiB", (unsigned long long)gap_sectors);
		kl_field_set(&app->create_size, size);
	}

	/* The size field beside Delete. */
	rect.x = x + width_button + 24;
	rect.y = y;
	rect.width = 160;
	rect.height = theme->control_height;
	changes = kl_field(app->ui, &app->style, SCREEN_ID_SIZE, &rect, &app->create_size, kl_tr("Size"));
	inst_control(app, "size", &rect, 1);
	flags = 0;
	if (busy || error != 0)
		flags |= KL_BUTTON_DISABLED;

	/* Create is pressed, or Enter is pressed in the size field while Create is offered. */
	create = screen_button(app, SCREEN_ID_CREATE, rect.x + rect.width + 10, y, kl_tr("Create partition"), flags, "create", NULL);
	if (!create && (changes & KL_FIELD_SUBMITTED) != 0U && flags == 0U)
		create = 1;
	if (create)
		inst_edit_create(app);

	/* The row of controls is passed. */
	y += theme->control_height + 10;

	/* A new ESP is made with the first partition. */
	if (!esp) {
		y = screen_lines(app, x, y, width, kl_tr("The disk has no EFI system partition: one of 512 MiB is made before the first new partition."),
				 SCREEN_TEXT_SMALL, 0, theme->text_secondary, 2);
	}

	/* The write, or what the last one said. */
	if (busy) {
		y = screen_lines(app, x, y + 4, width, kl_tr("Writing to the disk..."), SCREEN_TEXT, 0, theme->text_secondary, 1);
	} else if (app->edit_message[0] != '\0') {
		ink = theme->good;
		if (app->edit_failed)
			ink = theme->bad;
		y = screen_lines(app, x, y + 4, width, app->edit_message, SCREEN_TEXT, 0, ink, 3);
	}

	/* A restart is needed before the new partitions can be used. */
	if (app->reload_failed) {
		y = screen_lines(app, x, y + 4, width, kl_tr("The table was written, but the new partitions can be used only after the computer restarts."),
				 SCREEN_TEXT, 1, theme->bad, 3);
	}

	/* Reports the edge below. */
	return y;
}

/* S5: the partitions the system may go on, the size it needs. */
static int
screen_root(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	char size[32];
	char words[192];
	int y;

	/* What the system needs. */
	inst_bytes_text(inst_root_needed(app, 1), size, sizeof(size));
	(void)kl_tr_format(words, sizeof(words), kl_tr("The system needs at least {1}. The partition chosen is formatted (UFS)."), size, (const char *)NULL);
	y = screen_lines(app, x, top, width, words, SCREEN_TEXT, 0, app->style.theme->text_secondary, 2);

	/* The partitions. */
	return screen_part_rows(app, SCREEN_ID_ROOT, x, y + 10, width, 0);
}

/* S6: no swap partition, or one of the partitions left. */
static int
screen_swap(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	struct kl_rect rect;
	char size[32];
	char words[192];
	int clicked;
	int selected;
	int y;

	/* None: a swap file on the root. */
	inst_bytes_text(inst_swapfile_bytes(app), size, sizeof(size));
	(void)kl_tr_format(words, sizeof(words), kl_tr("A swap file of {1} is made on the system's partition."), size, (const char *)NULL);
	rect.x = x;
	rect.y = top;
	rect.width = width;
	rect.height = SCREEN_ROW;
	selected = app->swap_none && !app->has_swap;
	clicked = screen_choice(app, SCREEN_ID_SWAP, 0U, &rect, selected, 1, kl_tr("No swap partition"), words, "swap-none");
	if (clicked)
		inst_choose_swap(app, NULL);
	y = top + SCREEN_ROW + 8;

	/* The partitions (the root's left out). */
	return screen_part_rows(app, SCREEN_ID_SWAP, x, y, width, 1);
}

/*
 * Draws the partitions S5 or S6 may choose: every one but the ESP (and the
 * root on S6), the unassigned first, a mounted one faded.  Returns the
 * edge below.
 */
static int
screen_part_rows(
	struct inst_app *app,
	uint32_t id,
	int x,
	int top,
	int width,
	int swap)
{
	const struct inst_disk *disk;
	const struct inst_part *part;
	struct inst_tuple tuple;
	struct kl_rect rect;
	char title[256];
	char detail[256];
	char name[32];
	const char *contents;
	const char *role;
	unsigned pass;
	unsigned i;
	int selected;
	int enabled;
	int clicked;
	int is_esp;
	int unassigned;
	int same;
	int y;

	/* The disk. */
	disk = screen_disk(app);
	if (disk == NULL)
		return top;

	/* The unassigned partitions first, then the others. */
	y = top;
	for (pass = 0; pass < 2U; pass++) {
		for (i = 0; i < disk->part_count; i++) {
			part = &app->probe.part[disk->first_part + i];

			/* An ESP is never offered. */
			is_esp = inst_part_esp(part);
			if (is_esp)
				continue;

			/* The first pass takes the unassigned partitions, the second the others. */
			unassigned = inst_part_unassigned(part);
			if ((pass == 0U) != (unassigned != 0))
				continue;

			/* The swap list leaves out the root's partition. */
			inst_tuple_of(part, &tuple);
			same = inst_tuple_same(&tuple, &app->root);
			if (swap && app->has_root && same)
				continue;

			/* The row: chosen, or faded while the system uses it. */
			selected = 0;
			if (!swap && app->has_root)
				selected = same;
			if (swap && app->has_swap)
				selected = inst_tuple_same(&tuple, &app->swap);
			enabled = !part->mounted && !part->swap;
			screen_part_line(disk, part, title, sizeof(title));
			if (unassigned) {
				(void)snprintf(detail, sizeof(detail), "%s", kl_tr("New, not used yet"));
			} else if (!enabled) {
				(void)snprintf(detail, sizeof(detail), "%s", kl_tr("In use by the running system"));
			} else {
				contents = kl_tr("unknown contents");
				if (part->fstype[0] != '\0')
					contents = part->fstype;
				(void)snprintf(detail, sizeof(detail), "%s %s", kl_tr("Its contents are erased:"), contents);
			}

			/* The row's place, and its name for the log. */
			rect.x = x;
			rect.y = y;
			rect.width = width;
			rect.height = SCREEN_ROW;
			role = "root";
			if (swap)
				role = "swap";
			(void)snprintf(name, sizeof(name), "%s-%u", role, part->slot);

			/* A click chooses the partition for the screen's role. */
			clicked = screen_choice(app, id, i + 1U, &rect, selected, enabled, title, detail, name);
			if (clicked) {
				if (swap)
					inst_choose_swap(app, part);
				else
					inst_choose_root(app, part);
			}

			/* The next row, below this one. */
			y += SCREEN_ROW + 8;
		}
	}

	/* Reports the edge below. */
	return y;
}

/* S7: the items (the base required, the others on and off), the firmware's licences, the sizes. */
static int
screen_items(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	const struct inst_item *item;
	const struct inst_disk *disk;
	const struct inst_part *part;
	struct kl_rect rect;
	char needed[32];
	char room[32];
	char words[256];
	const char *agree_text;
	uint64_t bytes;
	uint64_t disk_bytes;
	uint64_t swap_bytes;
	unsigned i;
	int firmware;
	int depth;
	int pressed;
	int loaded;
	int changed;
	int y;

	/* The top items and the packages' under them ("Other packages" open or closed). */
	theme = app->style.theme;
	y = top;
	for (i = 0; i < app->catalog.item_count; i++) {
		item = &app->catalog.item[i];
		depth = 0;
		if (item->parent >= 0) {
			depth = 1;
			if (app->catalog.item[item->parent].parent >= 0) {
				depth = 2;
				if (!app->other_open)
					continue;
			}
		}

		/* The row, and the edge below it. */
		y = screen_item_row(app, i, x, y, width, depth);
	}

	/* The firmware's licences, read and agreed. */
	firmware = inst_catalog_has(&app->catalog, "firmware");
	if (firmware && app->catalog.license_count > 0U) {
		y += 10;
		y = screen_lines(app, x, y, width, kl_tr("The firmware comes under its makers' licences. Read them, and agree to install the firmware."), SCREEN_TEXT, 0,
				 theme->text_secondary, 2);
		pressed = screen_button(app, SCREEN_ID_LICENSES, x, y + 6, kl_tr("Read the licences"), 0U, "licenses", NULL);
		if (pressed) {
			loaded = inst_license_load(app, 0U);
			if (loaded == 0) {
				app->license_view = 1;
				kl_scroll_move_to(&app->scroll, 0.0, 0.0, 0, kl_clock_us());
			}
		}

		/* The switch that agrees to the licences, beside the button. */
		rect.x = x + 240;
		rect.y = y + 6 + (theme->control_height - theme->switch_height) / 2;
		rect.width = theme->switch_width;
		rect.height = theme->switch_height;
		changed = kl_switch(app->ui, &app->style, SCREEN_ID_AGREE, rect.x, rect.y, &app->license_agreed, 0U);
		if (changed)
			app->dirty = 1;
		inst_control(app, "license-agree", &rect, 1);

		/* The words beside the switch. */
		agree_text = kl_tr("I agree to the firmware's licences");
		(void)kl_text_draw(&app->text, &app->canvas, rect.x + rect.width + 12, kl_text_center(SCREEN_TEXT, y + 6, theme->control_height),
				   agree_text, strlen(agree_text), SCREEN_TEXT, 0, theme->text);
		y += 6 + theme->control_height;
	}

	/* What it takes, and the room there is. */
	inst_bytes_text(inst_root_needed(app, 0), needed, sizeof(needed));
	(void)snprintf(room, sizeof(room), "%s", "-");
	disk = screen_disk(app);
	if (disk != NULL) {
		disk_bytes = inst_disk_bytes(disk);
		swap_bytes = inst_whole_swap_bytes(app, disk);
		bytes = 0;
		if (app->whole == 1 && disk_bytes > INST_ESP_BYTES + swap_bytes + 2U * INST_MIB)
			bytes = disk_bytes - INST_ESP_BYTES - swap_bytes - 2U * INST_MIB;
		part = NULL;
		if (app->whole == 0 && app->has_root)
			part = inst_part_find(&app->probe, &app->root);
		if (part != NULL)
			bytes = inst_part_bytes(disk, part);
		inst_bytes_text(bytes, room, sizeof(room));
	}

	/* The sentence that sets the need against the room. */
	(void)kl_tr_format(words, sizeof(words), kl_tr("Needs {1} (with room to spare); the system's partition has {2}."), needed, room, (const char *)NULL);
	y = screen_lines(app, x, y + 14, width, words, SCREEN_TEXT, 1, theme->text, 2);

	/* Reports the edge below. */
	return y;
}

/*
 * Draws an item's row: its label, what it takes, a switch (fixed for the
 * base and for an item another needs), and "Show" for a heading that
 * opens.  Returns the edge below.
 */
static int
screen_item_row(
	struct inst_app *app,
	unsigned item,
	int x,
	int top,
	int width,
	int depth)
{
	const struct kl_theme *theme;
	const struct inst_item *entry;
	struct kl_rect rect;
	char detail[192];
	char size[32];
	char name[80];
	const char *toggle_label;
	uint64_t bytes;
	unsigned flags;
	unsigned j;
	int dependency;
	int on;
	int changed;
	int indent;
	int same;
	int pressed;

	/* The heading of the packages: a title, and "Other packages" opens. */
	theme = app->style.theme;
	entry = &app->catalog.item[item];
	indent = depth * 24;
	(void)snprintf(name, sizeof(name), "item-%s", entry->id);
	if (entry->unit_count == 0U) {
		same = strcmp(entry->id, "other");
		if (same != 0) {
			(void)screen_lines(app, x + indent, top + 14, width - indent, kl_tr(entry->label), SCREEN_TEXT_TITLE, 1, theme->text, 1);
			return top + 40;
		}

		/* "Other packages": its title, and a button that opens or closes the list. */
		(void)kl_text_draw_fit(&app->text, &app->canvas, x + indent, kl_text_center(SCREEN_TEXT, top, SCREEN_ROW_SMALL), kl_tr(entry->label),
				       SCREEN_TEXT, 0, width - indent - 120, theme->text);

		/* The button says what pressing it does to the list. */
		toggle_label = kl_tr("Show");
		if (app->other_open)
			toggle_label = kl_tr("Hide");
		pressed = screen_button(app, SCREEN_ID_ITEM, x + width - 100, top + (SCREEN_ROW_SMALL - theme->control_height) / 2, toggle_label, 0U, name, NULL);
		if (pressed)
			inst_item_toggle(app, item);
		return top + SCREEN_ROW_SMALL;
	}

	/* What it takes, and why it cannot be turned off. */
	bytes = 0;
	for (j = 0; j < entry->unit_count; j++)
		bytes += app->catalog.unit[entry->unit[j]].bytes;
	inst_bytes_text(bytes, size, sizeof(size));
	dependency = inst_item_by_dependency(&app->catalog, item);
	if (entry->fallback == 2)
		(void)snprintf(detail, sizeof(detail), "%s, %s", size, kl_tr("required"));
	else if (dependency)
		(void)snprintf(detail, sizeof(detail), "%s, %s", size, kl_tr("comes with another item"));
	else
		(void)snprintf(detail, sizeof(detail), "%s", size);

	/* The label and the detail. */
	(void)kl_text_draw_fit(&app->text, &app->canvas, x + indent, top + 18, kl_tr(entry->label), SCREEN_TEXT, depth == 0, width - indent - 220,
			       theme->text);
	(void)kl_text_draw_fit(&app->text, &app->canvas, x + width - 210, top + 18, detail, SCREEN_TEXT_SMALL, 0, 150, theme->text_secondary);

	/* The switch. */
	on = entry->chosen || dependency;
	flags = 0;
	if (entry->fallback == 2 || dependency)
		flags = KL_BUTTON_DISABLED;
	rect.x = x + width - theme->switch_width - 4;
	rect.y = top + (SCREEN_ROW_SMALL - theme->switch_height) / 2;
	rect.width = theme->switch_width;
	rect.height = theme->switch_height;
	changed = kl_switch(app->ui, &app->style, SCREEN_ID_ITEM + 1000U + item, rect.x, rect.y, &on, flags);
	inst_control(app, name, &rect, flags == 0U);
	if (changed && flags == 0U)
		inst_item_toggle(app, item);

	/* Reports the edge below. */
	return top + SCREEN_ROW_SMALL;
}

/* S7's licence viewer: the licence, a page at a time, the licences before and after, Close. */
static int
screen_license(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	const struct inst_license *license;
	const char *base;
	char words[256];
	char number[32];
	char count[32];
	struct kl_text_line metrics;
	char piece[512];
	size_t fitted;
	size_t done;
	size_t length;
	size_t end;
	unsigned flags;
	int baseline;
	int lines;
	int bx;
	int w;
	int y;
	int pressed;

	/* Which licence. */
	theme = app->style.theme;
	license = &app->catalog.license[app->license_index];
	base = strrchr(license->path, '/');
	if (base != NULL)
		base = base + 1;
	else
		base = license->path;
	(void)snprintf(number, sizeof(number), "%u", app->license_index + 1U);
	(void)snprintf(count, sizeof(count), "%u", app->catalog.license_count);
	(void)kl_tr_format(words, sizeof(words), kl_tr("Licence {1} of {2}: {3}, {4}"), number, count, license->package, base, (const char *)NULL);
	y = screen_lines(app, x, top, width, words, SCREEN_TEXT_TITLE, 1, theme->text, 2);

	/* The buttons: the licence before and after, the page before and after, Close. */
	bx = x;

	/* The licence before, faded on the first. */
	flags = 0U;
	if (app->license_index == 0U)
		flags = KL_BUTTON_DISABLED;
	pressed = screen_button(app, SCREEN_ID_LICENSE_BEFORE, bx, y + 6, kl_tr("Previous licence"), flags, "license-before", &w);
	if (pressed)
		(void)inst_license_load(app, app->license_index - 1U);
	bx += w + 8;

	/* The licence after, faded on the last. */
	flags = 0U;
	if (app->license_index + 1U >= app->catalog.license_count)
		flags = KL_BUTTON_DISABLED;
	pressed = screen_button(app, SCREEN_ID_LICENSE_AFTER, bx, y + 6, kl_tr("Next licence"), flags, "license-after", &w);
	if (pressed)
		(void)inst_license_load(app, app->license_index + 1U);
	bx += w + 8;

	/* The page before, faded on the first page. */
	flags = 0U;
	if (app->license_depth == 0U)
		flags = KL_BUTTON_DISABLED;
	pressed = screen_button(app, SCREEN_ID_PAGE_BEFORE, bx, y + 6, kl_tr("Previous page"), flags, "page-before", &w);
	bx += w + 8;
	if (pressed) {
		app->license_depth--;
		app->license_offset = app->license_history[app->license_depth];
	}

	/* The page after, faded on the last page. */
	flags = 0U;
	if (app->license_page_end >= app->license_length)
		flags = KL_BUTTON_DISABLED;
	pressed = screen_button(app, SCREEN_ID_PAGE_AFTER, bx, y + 6, kl_tr("Next page"), flags, "page-after", &w);
	bx += w + 8;
	if (pressed) {
		if (app->license_depth < sizeof(app->license_history) / sizeof(app->license_history[0])) {
			app->license_history[app->license_depth] = app->license_offset;
			app->license_depth++;
		}

		/* The next page starts where this one ended. */
		app->license_offset = app->license_page_end;
	}

	/* Back to the items: the viewer closes. */
	pressed = screen_button(app, SCREEN_ID_LICENSE_CLOSE, bx, y + 6, kl_tr("Back to the items"), KL_BUTTON_PRIMARY, "license-close", &w);
	if (pressed) {
		app->license_view = 0;
		inst_license_close(app);
		return y;
	}

	/* The page starts below the buttons. */
	y += 6 + theme->control_height + 12;

	/* The page: lines broken to the column, a newline ending one. */
	if (app->license_text == NULL)
		return y;
	kl_text_metrics(&app->text, SCREEN_TEXT_SMALL, &metrics);
	baseline = y + metrics.ascent;
	done = app->license_offset;
	for (lines = 0; done < app->license_length && lines < SCREEN_LICENSE_LINES; lines++) {
		/* The rest of the text's line, at most a piece. */
		end = done;
		while (end < app->license_length && app->license_text[end] != '\n')
			end++;
		length = end - done;
		if (length >= sizeof(piece))
			length = sizeof(piece) - 1U;
		memcpy(piece, app->license_text + done, length);
		piece[length] = '\0';

		/* As much of it as the column takes (a word longer than the column is cut). */
		if (length > 0U) {
			fitted = kl_text_break(&app->text, piece, SCREEN_TEXT_SMALL, 0, width);
			if (fitted == 0U || fitted > length)
				fitted = length;
			(void)kl_text_draw(&app->text, &app->canvas, x, baseline, piece, fitted, SCREEN_TEXT_SMALL, 0, theme->text_secondary);
			done += fitted;
			while (done < end && app->license_text[done] == ' ')
				done++;
		}

		/* The end of the text's line is passed. */
		if (done == end && done < app->license_length)
			done++;
		baseline += metrics.height + 2;
	}

	/* Where the next page starts. */
	app->license_page_end = done;

	/* Reports the edge below. */
	return baseline;
}

/* S7a: the user, the passwords, the automatic login and the computer's name. */
static int
screen_user(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	struct kl_rect rect;
	unsigned flags;
	int desktop;
	int changed;
	int y;

	/* The fields. */
	theme = app->style.theme;
	y = screen_field_row(app, 0U, x, top, width, kl_tr("User name"), &app->user, kl_tr("small letters, such as alice"), "user");
	y = screen_field_row(app, 1U, x, y, width, kl_tr("Full name"), &app->fullname, kl_tr("Optional"), "fullname");
	y = screen_field_row(app, 2U, x, y, width, kl_tr("Password"), &app->password, kl_tr("The user's password"), "password");
	y = screen_field_row(app, 3U, x, y, width, kl_tr("Password again"), &app->password_again, kl_tr("The same again"), "password-again");
	y = screen_field_row(app, 4U, x, y, width, kl_tr("Root's password"), &app->root_password, kl_tr("The administrator's password"), "root-password");
	y = screen_field_row(app, 5U, x, y, width, kl_tr("Root's password again"), &app->root_password_again, kl_tr("The same again"), "root-password-again");
	y = screen_field_row(app, 6U, x, y, width, kl_tr("Computer's name"), &app->hostname, "zedbsd", "hostname");

	/* The automatic login, only with the desktop. */
	desktop = inst_catalog_has(&app->catalog, "desktop");
	(void)kl_row(&app->style, x - 20, y, width + 20, kl_tr("Log in automatically"), "", 1);
	rect.x = x + 220;
	rect.y = y + (40 - theme->switch_height) / 2;
	rect.width = theme->switch_width;
	rect.height = theme->switch_height;
	flags = 0U;
	if (!desktop) {
		flags = KL_BUTTON_DISABLED;
		app->autologin = 0;
	}

	/* The switch, and a redraw when it was turned. */
	changed = kl_switch(app->ui, &app->style, SCREEN_ID_AUTOLOGIN, rect.x, rect.y, &app->autologin, flags);
	if (changed)
		app->dirty = 1;
	inst_control(app, "autologin", &rect, desktop);
	y += 40;
	if (!desktop) {
		y = screen_lines(app, x, y + 4, width, kl_tr("Without the desktop the system starts at the console's login."), SCREEN_TEXT_SMALL, 0,
				 theme->text_secondary, 2);
	}

	/* The user's rights. */
	y = screen_lines(app, x, y + 8, width, kl_tr("The user is an administrator of the installed system (the wheel group)."), SCREEN_TEXT_SMALL, 0,
			 theme->text_secondary, 2);

	/* Reports the edge below. */
	return y;
}

/* Draws a row of a label and a field; returns the edge below. */
static int
screen_field_row(
	struct inst_app *app,
	uint32_t index,
	int x,
	int top,
	int width,
	const char *label,
	struct kl_field *field,
	const char *placeholder,
	const char *name)
{
	struct kl_rect rect;
	unsigned changes;

	/* The label at the left, the field at its right. */
	(void)kl_text_draw_fit(&app->text, &app->canvas, x, kl_text_center(SCREEN_TEXT, top, 44), label, SCREEN_TEXT, 0, 210, app->style.theme->text);
	rect.x = x + 220;
	rect.y = top + (44 - app->style.theme->control_height) / 2;
	rect.width = width - 220;
	if (rect.width > 420)
		rect.width = 420;
	rect.height = app->style.theme->control_height;
	changes = kl_field(app->ui, &app->style, SCREEN_ID_FIELD + 100U * (index + 1U), &rect, field, placeholder);
	inst_control(app, name, &rect, 1);
	if (changes != 0U)
		app->dirty = 1;
	if ((changes & KL_FIELD_SUBMITTED) != 0U)
		inst_next(app);

	/* Reports the edge below. */
	return top + 44;
}

/* S8: everything the installation does, the disk's erasure agreed, before Install. */
static int
screen_confirm(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	const struct inst_disk *disk;
	const struct inst_part *part;
	const struct inst_item *item;
	struct kl_rect rect;
	char line[512];
	char text[256];
	char esp_size[32];
	char swap_size[32];
	const char *separator;
	const char *automatic;
	const char *erase_text;
	size_t used;
	size_t room;
	unsigned i;
	int dependency;
	int changed;
	int y;

	/* The disk. */
	theme = app->style.theme;
	disk = screen_disk(app);
	if (disk == NULL)
		return top;
	y = top;
	screen_disk_line(app, disk, line, sizeof(line));
	screen_summary_row(app, x, &y, width, kl_tr("Disk"), line);

	/* The layout. */
	if (app->whole == 1) {
		inst_bytes_text(INST_ESP_BYTES, esp_size, sizeof(esp_size));
		inst_bytes_text(inst_whole_swap_bytes(app, disk), swap_size, sizeof(swap_size));
		(void)kl_tr_format(line, sizeof(line), kl_tr("The whole disk: an EFI system partition of {1}, a swap partition of {2}, the system's partition in the rest"),
				   esp_size, swap_size, (const char *)NULL);
		screen_summary_row(app, x, &y, width, kl_tr("Partitions"), line);
		for (i = 0; i < disk->part_count; i++) {
			part = &app->probe.part[disk->first_part + i];
			screen_part_line(disk, part, text, sizeof(text));
			(void)snprintf(line, sizeof(line), "%s %s", kl_tr("Erased:"), text);
			y = screen_lines(app, x + 220, y, width - 220, line, SCREEN_TEXT_SMALL, 0, theme->bad, 2) + 2;
		}
	} else {
		part = inst_disk_esp(app, disk);
		line[0] = '\0';
		if (part != NULL)
			screen_part_line(disk, part, line, sizeof(line));
		screen_summary_row(app, x, &y, width, kl_tr("EFI system partition"), line);
		part = inst_part_find(&app->probe, &app->root);
		line[0] = '\0';
		if (part != NULL)
			screen_part_line(disk, part, text, sizeof(text));
		if (part != NULL)
			(void)snprintf(line, sizeof(line), "%s (%s)", text, kl_tr("formatted"));
		screen_summary_row(app, x, &y, width, kl_tr("System's partition"), line);
		if (app->has_swap) {
			part = inst_part_find(&app->probe, &app->swap);
			line[0] = '\0';
			if (part != NULL)
				screen_part_line(disk, part, text, sizeof(text));
			if (part != NULL)
				(void)snprintf(line, sizeof(line), "%s (%s)", text, kl_tr("formatted"));
		} else {
			inst_bytes_text(inst_swapfile_bytes(app), swap_size, sizeof(swap_size));
			(void)kl_tr_format(line, sizeof(line), kl_tr("A swap file of {1} on the system's partition"), swap_size, (const char *)NULL);
		}

		/* The swap partition, or the swap file that takes its place. */
		screen_summary_row(app, x, &y, width, kl_tr("Swap"), line);
	}

	/* The loader's place. */
	(void)snprintf(line, sizeof(line), "%s", "\\EFI\\BOOT\\BOOTX64.EFI");
	part = NULL;
	if (app->whole != 1)
		part = inst_disk_esp(app, disk);
	if (part != NULL && part->boot_loader != INST_NO)
		(void)snprintf(line, sizeof(line), "%s (%s)", "\\EFI\\ZEDBSD\\BOOTX64.EFI", kl_tr("the loader there is kept"));
	screen_summary_row(app, x, &y, width, kl_tr("Loader"), line);

	/* The items. */
	used = 0;
	line[0] = '\0';
	for (i = 0; i < app->catalog.item_count; i++) {
		item = &app->catalog.item[i];

		/* A heading, and an item that is neither chosen nor needed, are not listed. */
		if (item->unit_count == 0U)
			continue;
		dependency = inst_item_by_dependency(&app->catalog, i);
		if (!item->chosen && !dependency)
			continue;

		/* The label, after a comma unless it is the first. */
		separator = "";
		if (used > 0U)
			separator = ", ";
		room = 0U;
		if (used < sizeof(line))
			room = sizeof(line) - used;
		used += (size_t)snprintf(line + used, room, "%s%s", separator, kl_tr(item->label));
		if (used >= sizeof(line))
			break;
	}

	/* The items installed. */
	screen_summary_row(app, x, &y, width, kl_tr("Install"), line);

	/* The user, and whether the desktop logs in by itself. */
	separator = "";
	automatic = "";
	if (app->autologin) {
		separator = ", ";
		automatic = kl_tr("logs in automatically");
	}

	/* The user's line. */
	(void)snprintf(line, sizeof(line), "%s%s%s", app->user.text, separator, automatic);
	screen_summary_row(app, x, &y, width, kl_tr("User"), line);
	screen_summary_row(app, x, &y, width, kl_tr("Computer's name"), app->hostname.text);

	/* The whole disk's erasure, agreed. */
	if (app->whole == 1) {
		y += 8;
		y = screen_lines(app, x, y, width, kl_tr("Everything on the disk is erased."), SCREEN_TEXT_TITLE, 1, theme->bad, 1);
		rect.x = x;
		rect.y = y + 8;
		rect.width = theme->switch_width;
		rect.height = theme->switch_height;
		changed = kl_switch(app->ui, &app->style, SCREEN_ID_ERASE, rect.x, rect.y, &app->erase_agreed, 0U);
		if (changed)
			app->dirty = 1;
		inst_control(app, "erase-agree", &rect, 1);

		/* The words beside the switch. */
		erase_text = kl_tr("Erase the disk");
		(void)kl_text_draw(&app->text, &app->canvas, rect.x + rect.width + 12, kl_text_center(SCREEN_TEXT, rect.y, rect.height),
				   erase_text, strlen(erase_text), SCREEN_TEXT, 0, theme->text);
		y = rect.y + rect.height + 6;
	}

	/* Reports the edge below. */
	return y;
}

/* Draws a row of a summary: its label and its value broken into lines. */
static void
screen_summary_row(
	struct inst_app *app,
	int x,
	int *top,
	int width,
	const char *label,
	const char *value)
{
	int bottom;

	/* The label, and the value at its right. */
	(void)kl_text_draw_fit(&app->text, &app->canvas, x, *top + 16, label, SCREEN_TEXT, 1, 210, app->style.theme->text);
	bottom = screen_lines(app, x + 220, *top + 2, width - 220, value, SCREEN_TEXT, 0, app->style.theme->text_secondary, 4);
	if (bottom < *top + 28)
		bottom = *top + 28;
	*top = bottom + 6;
}

/* S9: the step, the bar, the unit, the time, the warning. */
static int
screen_progress(
	struct inst_app *app,
	int x,
	int top,
	int width,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect bar;
	char words[256];
	char number[16];
	char total[16];
	char done[32];
	char all[32];
	char clock[32];
	const char *step_words;
	double fraction;
	uint64_t seconds;
	unsigned done_steps;
	size_t length;
	int same;
	int y;

	/* The step. */
	theme = app->style.theme;
	(void)snprintf(number, sizeof(number), "%u", app->step);
	(void)snprintf(total, sizeof(total), "%u", app->steps);
	step_words = kl_tr("Starting");
	if (app->step_name[0] != '\0')
		step_words = kl_tr(screen_step_words(app->step_name));
	(void)kl_tr_format(words, sizeof(words), kl_tr("Step {1} of {2}: {3}"), number, total, step_words, (const char *)NULL);
	if (app->stop_asked) {
		length = strlen(words);
		(void)snprintf(words + length, sizeof(words) - length, " (%s)", kl_tr("stopping"));
	}

	/* The step, drawn large. */
	y = screen_lines(app, x, top + 10, width, words, 18U, 1, theme->text, 2);

	/* The bar: the steps, the unpacking by its bytes within its step. */
	fraction = 0.0;
	if (app->steps > 0U) {
		done_steps = 0U;
		if (app->step > 0U)
			done_steps = app->step - 1U;
		fraction = ((double)done_steps) / (double)app->steps;
	}

	/* Unpacking adds the bytes done within its step. */
	same = strcmp(app->step_name, "unpack");
	if (app->bytes_total > 0U && same == 0)
		fraction += ((double)app->bytes_done / (double)app->bytes_total) / (double)app->steps;
	bar.x = x;
	bar.y = y + 16;
	bar.width = width;
	bar.height = 8;
	kl_progress(&app->style, &bar, fraction, now_us);
	y = bar.y + bar.height + 16;

	/* The unit, the bytes and the time it has taken. */
	if (same == 0 && app->unit_name[0] != '\0') {
		inst_bytes_text(app->bytes_done, done, sizeof(done));
		inst_bytes_text(app->bytes_total, all, sizeof(all));
		(void)kl_tr_format(words, sizeof(words), kl_tr("{1}: {2} of {3}"), app->unit_name, done, all, (const char *)NULL);
		y = screen_lines(app, x, y, width, words, SCREEN_TEXT, 0, theme->text_secondary, 1);
	}

	/* The time since the installation was sent. */
	seconds = (now_us - app->started_us) / 1000000U;
	(void)snprintf(clock, sizeof(clock), "%llu:%02llu", (unsigned long long)(seconds / 60U), (unsigned long long)(seconds % 60U));
	(void)kl_tr_format(words, sizeof(words), kl_tr("Time so far: {1}"), clock, (const char *)NULL);
	y = screen_lines(app, x, y + 4, width, words, SCREEN_TEXT, 0, theme->text_secondary, 1);

	/* The warning. */
	y = screen_card(app, x, y + SCREEN_GAP, width, kl_tr("Do not turn off or restart the computer"),
			kl_tr("The disk is being written. Turning the computer off now leaves the disk partly written. Making the swap file can take several minutes."),
			theme->card_edge);

	/* Reports the edge below. */
	return y;
}

/* S10: failed or cancelled, the step and its reason, and what the disk holds now. */
static int
screen_failed(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	char words[512];
	unsigned i;
	int same_check;
	int same_start;
	int y;

	/* What happened. */
	theme = app->style.theme;
	y = top;
	if (app->fail_step[0] != '\0') {
		(void)kl_tr_format(words, sizeof(words), kl_tr("At the step {1}: {2}"), kl_tr(screen_step_words(app->fail_step)), app->fail_text, (const char *)NULL);
		if (!app->canceled)
			y = screen_lines(app, x, y, width, words, SCREEN_TEXT, 0, theme->bad, 4) + 8;
		else
			y = screen_lines(app, x, y, width, words, SCREEN_TEXT, 0, theme->text_secondary, 4) + 8;
	}

	/* The changes S4 wrote, or none. */
	y = screen_lines(app, x, y + 4, width, kl_tr("What the disk holds now"), SCREEN_TEXT_TITLE, 1, theme->text, 1) + 4;
	if (app->write_count == 0U && app->fail_step[0] == '\0')
		y = screen_lines(app, x + 12, y, width - 12, kl_tr("The disk was not changed."), SCREEN_TEXT, 0, theme->text_secondary, 2);

	/* Each change S4 wrote. */
	for (i = 0; i < app->write_count; i++)
		y = screen_lines(app, x + 12, y + 2, width - 12, app->writes[i], SCREEN_TEXT, 0, theme->text_secondary, 2);

	/* An installation that began: the table and the partitions may have changed. */
	same_check = strcmp(app->fail_step, "check");
	same_start = strcmp(app->fail_step, "start");
	if (app->fail_step[0] != '\0' && same_check != 0 && same_start != 0) {
		y = screen_lines(app, x + 12, y + 2, width - 12,
				 kl_tr("The installation began: the partition table and the partitions may have been changed and formatted. The installed system does not start, for its start was not written. A restart may be needed before the disk is used again."),
				 SCREEN_TEXT, 0, theme->text_secondary, 5);
	}

	/* What the disk holds, as the helper reported it. */
	for (i = 0; i < app->state_count; i++)
		y = screen_lines(app, x + 12, y + 2, width - 12, app->states[i], SCREEN_TEXT_SMALL, 0, theme->text_faint, 2);

	/* What to do. */
	y = screen_lines(app, x, y + 12, width, kl_tr("Start over to choose again, or close the installer and keep using the desktop."), SCREEN_TEXT, 0,
			 theme->text_secondary, 2);

	/* Reports the edge below. */
	return y;
}

/* S11: done; how to start the installed system, the user, the rule of the image. */
static int
screen_done(
	struct inst_app *app,
	int x,
	int top,
	int width)
{
	const struct kl_theme *theme;
	char words[512];
	int zedbsd;
	int entry;
	int same;
	int y;

	/* How to start it: the firmware's boot entry, or else the loader's place, decides the words. */
	theme = app->style.theme;
	zedbsd = strstr(app->loader_path, "ZEDBSD") != NULL;
	entry = 0;
	same = strcmp(app->boot_entry, "none");
	if (app->boot_entry[0] != '\0' && same != 0)
		entry = 1;
	screen_mark(app, x, top, 64U);
	(void)kl_tr_format(words, sizeof(words), kl_tr("{1} is installed."), app->os_name, (const char *)NULL);
	y = screen_lines(app, x + 84, top + 20, width - 84, words, 20U, 1, theme->text, 1);
	y = top + 64 + SCREEN_GAP;
	if (entry) {
		(void)kl_tr_format(words, sizeof(words),
				   kl_tr("{1} was added to the firmware's boot menu, at the end of its boot order. Restart; if another system starts first, choose {1} in the firmware's boot menu."),
				   app->os_name, (const char *)NULL);
	} else if (zedbsd) {
		(void)kl_tr_format(words, sizeof(words),
				   kl_tr("The disk already had a loader, which was kept. Restart and choose {1} in the firmware's boot menu (\\EFI\\ZEDBSD\\BOOTX64.EFI on the disk)."),
				   app->os_name, (const char *)NULL);
	} else {
		(void)kl_tr_format(words, sizeof(words),
				   kl_tr("Restart and choose the disk in the firmware's boot menu; the firmware's settings can make it the first to start."),
				   app->os_name, (const char *)NULL);
	}

	/* The card that says how to start it. */
	y = screen_card(app, x, y, width, kl_tr("Start the installed system"), words, theme->accent);

	/* The user. */
	(void)kl_tr_format(words, sizeof(words), kl_tr("Log in as {1} with the password you chose."), app->installed_user, (const char *)NULL);
	y = screen_lines(app, x, y + SCREEN_GAP, width, words, SCREEN_TEXT, 0, theme->text, 2);
	same = strcmp(app->swap_kind, "none");
	if (same == 0)
		y = screen_lines(app, x, y + 4, width, kl_tr("The system has no swap: the swap file could not be used."), SCREEN_TEXT, 0, theme->text_secondary, 2);

	/* The rule of the image. */
	y = screen_card(app, x, y + SCREEN_GAP, width, kl_tr("Remove the installer's USB"),
			kl_tr("The installer's image is for installing only: once the installed system starts, remove it and do not use it as your system. Until it is removed the firmware may start from it again."),
			theme->danger);

	/* A restart that failed. */
	if (app->power_message[0] != '\0')
		y = screen_lines(app, x, y + 8, width, app->power_message, SCREEN_TEXT, 0, theme->bad, 2);

	/* What Close does. */
	y = screen_lines(app, x, y + 8, width, kl_tr("Close keeps the desktop of the installer's image for now."), SCREEN_TEXT_SMALL, 0, theme->text_faint, 2);

	/* Reports the edge below. */
	return y;
}

/* Gives the title and the summary of the screen shown. */
static void
screen_titles(
	const struct inst_app *app,
	const char **title,
	const char **summary,
	char *named,
	size_t size)
{
	/* By the screen. */
	*summary = NULL;
	switch (app->screen) {
	case INST_S1:
		*title = kl_tr("Welcome");
		(void)kl_tr_format(named, size, kl_tr("Install {1} on this computer"), app->os_name, (const char *)NULL);
		*summary = named;
		break;
	case INST_S2:
		*title = kl_tr("Choose a disk");
		(void)kl_tr_format(named, size, kl_tr("The disk {1} is installed on"), app->os_name, (const char *)NULL);
		*summary = named;
		break;
	case INST_S3:
		*title = kl_tr("How to use the disk");
		*summary = kl_tr("The whole disk, or partitions you choose");
		break;
	case INST_S4:
		*title = kl_tr("Partitions");
		*summary = kl_tr("Delete or create partitions (written at once)");
		break;
	case INST_S5:
		*title = kl_tr("The system's partition");
		*summary = kl_tr("Where the system goes");
		break;
	case INST_S6:
		*title = kl_tr("Swap");
		*summary = kl_tr("A swap partition, or a swap file");
		break;
	case INST_S7:
		*title = kl_tr("What to install");
		*summary = kl_tr("The base system, the desktop, the firmware and packages");
		break;
	case INST_S7A:
		*title = kl_tr("User");
		*summary = kl_tr("The user, the passwords and the computer's name");
		break;
	case INST_S8:
		*title = kl_tr("Confirm");
		*summary = kl_tr("Nothing is written before Install");
		break;
	case INST_S9:
		*title = kl_tr("Installing");
		(void)kl_tr_format(named, size, kl_tr("{1} is being written to the disk"), app->os_name, (const char *)NULL);
		*summary = named;
		break;
	case INST_S10:
		if (app->canceled)
			*title = kl_tr("The installation was cancelled");
		else
			*title = kl_tr("The installation failed");
		*summary = kl_tr("The installed system does not start");
		if (app->fail_step[0] == '\0')
			*summary = kl_tr("Nothing was installed");
		break;
	default:
		*title = kl_tr("Done");
		*summary = kl_tr("The installation has finished");
		break;
	}
}

/* Gives the disk chosen, as the last probe told it, or NULL. */
static const struct inst_disk *
screen_disk(
	const struct inst_app *app)
{
	/* None chosen. */
	if (app->disk == 0U)
		return NULL;

	/* The disk, unless it went. */
	return inst_disk_find(&app->probe, app->disk);
}

/* Writes a disk's line: its name, its kind, its size, removable. */
static void
screen_disk_line(
	const struct inst_app *app,
	const struct inst_disk *disk,
	char *text,
	size_t size)
{
	const char *kind;
	const char *sectors;
	char bytes[32];
	uint64_t disk_bytes;
	int same;

	/* The application is not needed to write the line. */
	(void)app;

	/* The kind as people know it (AHCI's disks are IDE's kind). */
	kind = kl_tr("SATA/IDE");
	same = strcmp(disk->kind, "nvme");
	if (same == 0) {
		kind = "NVMe";
	} else {
		same = strcmp(disk->kind, "removable");
		if (same == 0)
			kind = kl_tr("USB or removable");
	}

	/* Its size, and the sector size when it is the large one. */
	disk_bytes = inst_disk_bytes(disk);
	inst_bytes_text(disk_bytes, bytes, sizeof(bytes));
	sectors = "";
	if (disk->sector_bytes == 4096U)
		sectors = "  (4096-byte sectors)";
	(void)snprintf(text, size, "%s  %s  %s%s", disk->name, kind, bytes, sectors);
}

/* Writes a partition's line: its slot, its size, its type, its labels. */
static void
screen_part_line(
	const struct inst_disk *disk,
	const struct inst_part *part,
	char *text,
	size_t size)
{
	char bytes[32];
	const char *type_name;
	uint64_t part_bytes;
	size_t used;
	size_t name_length;

	/* Slot, size, type, then the GPT name and the file system's label when they say something. */
	part_bytes = inst_part_bytes(disk, part);
	inst_bytes_text(part_bytes, bytes, sizeof(bytes));
	type_name = inst_type_name(part->type);
	(void)snprintf(text, size, "%u  %s  %s", part->slot, bytes, type_name);

	/* The GPT name, when it fits. */
	used = strlen(text);
	name_length = strlen(part->name);
	if (part->name[0] != '\0' && used + name_length + 5U < size) {
		(void)strcat(text, "  \"");
		(void)strcat(text, part->name);
		(void)strcat(text, "\"");
	}

	/* The file system's label, when it fits. */
	used = strlen(text);
	name_length = strlen(part->fslabel);
	if (part->fslabel[0] != '\0' && used + name_length + 4U < size) {
		(void)strcat(text, "  [");
		(void)strcat(text, part->fslabel);
		(void)strcat(text, "]");
	}
}

/* Gives the words of an installation's step by its name, or the name. */
static const char *
screen_step_words(
	const char *name)
{
	unsigned i;
	int same;

	/* Each step. */
	for (i = 0; i < sizeof(screen_steps) / sizeof(screen_steps[0]); i++) {
		same = strcmp(name, screen_steps[i][0]);
		if (same == 0)
			return screen_steps[i][1];
	}

	/* A step without words. */
	return name;
}
