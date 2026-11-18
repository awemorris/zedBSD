/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Storage page's analysis and Trash cards (ws089-p023), under the
 * disks' cards (page-look.c):
 *
 *   Folder use  Analyze (Stop while it counts) and Up, the folder analysed,
 *               what is counted so far, and the folders in it the largest
 *               first, each with a bar; a click on a folder analyses it.
 *   Trash       its size, and Empty Trash, which asks to be confirmed.
 *   Recent items  Keep recent items (q824, WS148 p001, in the git history): the
 *               desktop's list of the files applications opened, which
 *               Files shows as Recents; off empties the list and stops it.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

/* The controls of the cards (hit indices). */
#define STORAGE_ANALYZE		400
#define STORAGE_UP		401
#define STORAGE_EMPTY		402
#define STORAGE_CONFIRM		403
#define STORAGE_CANCEL		404
#define STORAGE_RECENT		405
#define STORAGE_ROW_FIRST	410

/* The rows shown, a row's height, the card's margin, the space between cards, and the text sizes. */
#define STORAGE_ROWS		12U
#define STORAGE_ROW		40
#define STORAGE_PAD		20
#define STORAGE_GAP		16
#define STORAGE_BAR		6
#define STORAGE_TEXT_ROW	14U
#define STORAGE_TEXT_SUB	13U

static int storage_use_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int storage_trash_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int storage_recent_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void storage_recent_toggle(struct se_app *app);
static void storage_parent(const char *path, char *parent, size_t size);

/*
 * Draws the analysis's and the Trash's cards from a top edge; returns the
 * edge below them.
 */
int
se_storage_cards(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int y;

	/* The analysis, the Trash, then the recent items. */
	y = storage_use_card(app, canvas, x, top, width);
	y = storage_trash_card(app, canvas, x, y + STORAGE_GAP, width);
	y = storage_recent_card(app, canvas, x, y + STORAGE_GAP, width);

	/* The edge below them. */
	return y;
}

/*
 * Carries out a click on a control of the Storage page.
 */
void
se_storage_press(
	struct se_app *app,
	int index)
{
	struct se_storage *storage;
	char path[SE_SCAN_PATH];
	unsigned row;
	int written;

	/* A folder's row analyses it. */
	storage = &app->storage;
	if (index >= STORAGE_ROW_FIRST && index < STORAGE_ROW_FIRST + (int)STORAGE_ROWS) {
		row = (unsigned)(index - STORAGE_ROW_FIRST);
		if (row < storage->view.group_count && storage->view.groups[row].folder) {
			written = snprintf(path, sizeof(path), "%s/%s", storage->view.root, storage->view.groups[row].name);
			if (written > 0 && (size_t)written < sizeof(path))
				se_storage_analyze(app, path);
		}

		/* Taken. */
		return;
	}

	/* The buttons. */
	switch (index) {
	case STORAGE_ANALYZE:
		/* Analyze, or Stop while it counts. */
		if (storage->view.state == SE_SCAN_RUNNING) {
			se_storage_stop(app);
		} else if (storage->root[0] != '\0') {
			se_storage_analyze(app, storage->root);
		} else {
			se_storage_analyze(app, NULL);
		}

		/* Taken. */
		return;
	case STORAGE_UP:
		/* The folder above, while it is in the home. */
		storage_parent(storage->root, path, sizeof(path));
		se_storage_analyze(app, path);
		return;
	case STORAGE_EMPTY:
		/* Empty Trash asks first. */
		storage->confirming = 1;
		app->dirty = 1;
		return;
	case STORAGE_CONFIRM:
		se_storage_empty_trash(app);
		return;
	case STORAGE_CANCEL:
		storage->confirming = 0;
		app->dirty = 1;
		return;
	case STORAGE_RECENT:
		storage_recent_toggle(app);
		return;
	default:
		break;
	}
}

/* Draws the Folder use card; returns the edge below it. */
static int
storage_use_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_storage *storage;
	const struct se_scan_view *view;
	const struct se_scan_group *group;
	struct kl_rect hit;
	char line[SE_MESSAGE];
	char size[32];
	const char *label;
	const char *shown;
	const char *noun;
	kl_color ink;
	unsigned rows;
	unsigned index;
	float share;
	int height;
	int button;
	int right;
	int bar;
	int y;
	int up;
	int differs;

	/* How many rows, and the card. */
	storage = &app->storage;
	view = &storage->view;
	rows = view->group_count;
	if (rows > STORAGE_ROWS)
		rows = STORAGE_ROWS;
	height = 64 + 50 + 36 + (int)rows * STORAGE_ROW + 16;
	shown = view->root;
	if (shown[0] == '\0')
		shown = "Your home folder";
	y = se_card_begin(app, canvas, x, top, width, height, "Folder use", shown);

	/* Analyze or Stop, and Up while the folder is below the home. */
	right = x + width - STORAGE_PAD;
	label = "Analyze";
	if (view->state == SE_SCAN_RUNNING)
		label = "Stop";
	button = se_button_width(app, label);
	(void)se_button_draw(app, canvas, right - button, y + 6, label, 1, 1, STORAGE_ANALYZE);
	differs = strcmp(storage->root, storage->home);
	up = storage->root[0] != '\0' && storage->home[0] != '\0' && differs != 0;
	if (up)
		(void)se_button_draw(app, canvas, right - button - 8 - se_button_width(app, "Up"), y + 6, "Up", 0, view->state != SE_SCAN_RUNNING, STORAGE_UP);

	/* What is counted so far, or what to do. */
	se_bytes_text(view->bytes, size, sizeof(size));
	noun = "files";
	if (view->files == 1U)
		noun = "file";
	switch (view->state) {
	case SE_SCAN_RUNNING:
		(void)snprintf(line, sizeof(line), "Counting... %s in %llu %s", size, (unsigned long long)view->files, noun);
		break;
	case SE_SCAN_DONE:
		(void)snprintf(line, sizeof(line), "%s in %llu %s", size, (unsigned long long)view->files, noun);
		break;
	case SE_SCAN_STOPPED:
		(void)snprintf(line, sizeof(line), "Stopped: %s in %llu %s counted", size, (unsigned long long)view->files, noun);
		break;
	default:
		(void)snprintf(line, sizeof(line), "%s", "Analyze counts how much each folder takes.");
		break;
	}

	/* A failure to start says why instead, in red. */
	ink = SE_COLOR_TEXT_SECONDARY;
	if (storage->message[0] != '\0' && storage->message_bad && view->state == SE_SCAN_IDLE) {
		(void)snprintf(line, sizeof(line), "%s", storage->message);
		ink = SE_COLOR_BAD;
	}

	/* The line, left of the buttons. */
	(void)kl_text_draw_fit(app->text, canvas, x + STORAGE_PAD, kl_text_center(STORAGE_TEXT_SUB, y + 6, 36), line, STORAGE_TEXT_SUB, 0, width - 3 * STORAGE_PAD - 2 * button, ink);
	y += 50;

	/* The folders that could not be read. */
	if (view->unreadable != 0U) {
		(void)snprintf(line, sizeof(line), "%llu folders could not be read and are not counted.", (unsigned long long)view->unreadable);
		(void)kl_text_draw_fit(app->text, canvas, x + STORAGE_PAD, y + 14, line, STORAGE_TEXT_SUB, 0, width - 2 * STORAGE_PAD, SE_COLOR_TEXT_FAINT);
	}

	/* The rows start under that line. */
	y += 36;

	/* Each folder, the largest first, its bar against the largest. */
	bar = width - 2 * STORAGE_PAD;
	for (index = 0; index < rows; index++) {
		group = &view->groups[index];
		share = 0.0f;
		if (view->groups[0].bytes != 0U)
			share = (float)group->bytes / (float)view->groups[0].bytes;
		se_bytes_text(group->bytes, size, sizeof(size));
		ink = SE_COLOR_TEXT;
		if (!group->folder)
			ink = SE_COLOR_TEXT_SECONDARY;
		(void)kl_text_draw_fit(app->text, canvas, x + STORAGE_PAD, y + 16, group->name, STORAGE_TEXT_ROW, 0, bar - 120, ink);
		(void)kl_text_draw_fit(app->text, canvas, right - 110, y + 16, size, STORAGE_TEXT_ROW, 0, 110, SE_COLOR_TEXT_SECONDARY);
		kl_canvas_round(canvas, (float)(x + STORAGE_PAD), (float)(y + 24), (float)bar, (float)STORAGE_BAR, 3.0f, SE_COLOR_RAIL);
		kl_canvas_round(canvas, (float)(x + STORAGE_PAD), (float)(y + 24), (float)bar * share, (float)STORAGE_BAR, 3.0f, SE_COLOR_ACCENT);
		if (group->folder) {
			hit.x = x + STORAGE_PAD;
			hit.y = y;
			hit.width = bar;
			hit.height = STORAGE_ROW;
			se_ui_hit(app, &hit, SE_HIT_CONTROL, STORAGE_ROW_FIRST + (int)index);
		}

		/* The next row. */
		y += STORAGE_ROW;
	}

	/* The edge below the card. */
	return top + height;
}

/* Draws the Trash card; returns the edge below it. */
static int
storage_trash_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_storage *storage;
	const struct se_scan_view *view;
	char line[SE_MESSAGE];
	char size[32];
	const char *noun;
	kl_color ink;
	int empty;
	int height;
	int right;
	int button;
	int y;

	/* The card. */
	storage = &app->storage;
	view = &storage->trash_view;
	height = 64 + 50 + 30;
	y = se_card_begin(app, canvas, x, top, width, height, "Trash", "What Files moved to the Trash.");

	/* Its size, or what it does now. */
	empty = view->files == 0U && view->group_count <= 1U;
	se_bytes_text(view->bytes, size, sizeof(size));
	noun = "files";
	if (view->files == 1U)
		noun = "file";
	(void)snprintf(line, sizeof(line), "%s in %llu %s", size, (unsigned long long)view->files, noun);
	if (view->state == SE_SCAN_RUNNING)
		(void)snprintf(line, sizeof(line), "%s", "Counting...");
	else if (empty)
		(void)snprintf(line, sizeof(line), "%s", "The Trash is empty.");
	if (storage->trash.started)
		(void)snprintf(line, sizeof(line), "%s", "Emptying the Trash...");
	(void)kl_text_draw_fit(app->text, canvas, x + STORAGE_PAD, kl_text_center(STORAGE_TEXT_ROW, y + 6, 36), line, STORAGE_TEXT_ROW, 0, width / 2, SE_COLOR_TEXT);

	/* Empty Trash, or its confirmation. */
	right = x + width - STORAGE_PAD;
	if (storage->confirming) {
		button = se_button_width(app, "Cancel");
		(void)se_button_draw(app, canvas, right - button, y + 6, "Cancel", 0, 1, STORAGE_CANCEL);
		(void)se_button_draw(app, canvas, right - button - 8 - se_button_width(app, "Empty"), y + 6, "Empty", 1, 1, STORAGE_CONFIRM);
	} else {
		button = se_button_width(app, "Empty Trash");
		(void)se_button_draw(app, canvas, right - button, y + 6, "Empty Trash", 0, !empty && !storage->trash.started && view->state != SE_SCAN_RUNNING, STORAGE_EMPTY);
	}

	/* The line under them. */
	y += 50;

	/* The confirmation's question, or the last message. */
	ink = SE_COLOR_TEXT_SECONDARY;
	line[0] = '\0';
	if (storage->confirming) {
		(void)snprintf(line, sizeof(line), "%s", "Empty the Trash? Its items are removed for good.");
		ink = SE_COLOR_BAD;
	} else if (storage->message[0] != '\0' && view->state != SE_SCAN_IDLE) {
		(void)snprintf(line, sizeof(line), "%s", storage->message);
		if (storage->message_bad)
			ink = SE_COLOR_BAD;
	}

	/* Drawn when there is one. */
	if (line[0] != '\0')
		(void)kl_text_draw_fit(app->text, canvas, x + STORAGE_PAD, y + 12, line, STORAGE_TEXT_SUB, 0, width - 2 * STORAGE_PAD, ink);

	/* The edge below the card. */
	return top + height;
}

/* Draws the Recent items card; returns the edge below it. */
static int
storage_recent_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_storage *storage;
	const char *line;
	kl_color ink;
	int height;
	int error;
	int keep;
	int y;

	/* Whether the list is kept, read the first time the card is drawn. */
	storage = &app->storage;
	if (!storage->recent_known) {
		error = kl_recent_keep(&keep);
		storage->recent_keep = keep;
		storage->recent_known = 1;
		se_log("STORAGE recent keep=%d error=%d", keep, error);
	}

	/* The card, its switch and its label. */
	height = 64 + 50 + 30;
	y = se_card_begin(app, canvas, x, top, width, height, "Recent items", "The files applications opened lately, shown in Files' Recents.");
	(void)kl_text_draw_fit(app->text, canvas, x + STORAGE_PAD, kl_text_center(STORAGE_TEXT_ROW, y + 6, 36), "Keep recent items", STORAGE_TEXT_ROW, 0, width / 2, SE_COLOR_TEXT);
	se_toggle_draw(app, canvas, x + width - STORAGE_PAD - 44, y + 16, storage->recent_keep, 1, STORAGE_RECENT);
	y += 50;

	/* What the switch does, or why it did not. */
	line = "Turning this off clears the list.";
	ink = SE_COLOR_TEXT_SECONDARY;
	if (storage->recent_message[0] != '\0') {
		line = storage->recent_message;
		if (storage->recent_bad)
			ink = SE_COLOR_BAD;
	}

	/* The line under the switch. */
	(void)kl_text_draw_fit(app->text, canvas, x + STORAGE_PAD, y + 12, line, STORAGE_TEXT_SUB, 0, width - 2 * STORAGE_PAD, ink);

	/* The edge below the card. */
	return top + height;
}

/* Turns Keep recent items over: off empties the desktop's recent list and stops it, on starts it again. */
static void
storage_recent_toggle(
	struct se_app *app)
{
	struct se_storage *storage;
	int keep;
	int error;

	/* The other choice, made (the log the tests read). */
	storage = &app->storage;
	keep = !storage->recent_keep;
	error = kl_recent_set_keep(keep);
	se_log("STORAGE recent set keep=%d error=%d", keep, error);
	app->dirty = 1;

	/* A failure is said and the switch stays. */
	if (error != 0) {
		(void)snprintf(storage->recent_message, sizeof(storage->recent_message), "The recent list could not be changed (%s).", strerror(error));
		storage->recent_bad = 1;
		return;
	}

	/* Succeeded: the switch as it now is. */
	storage->recent_keep = keep;
	storage->recent_bad = 0;
	storage->recent_message[0] = '\0';
	if (!keep)
		(void)snprintf(storage->recent_message, sizeof(storage->recent_message), "%s", "The recent list was cleared and is not kept.");
}

/* Gives the folder above a path ("/" for the top). */
static void
storage_parent(
	const char *path,
	char *parent,
	size_t size)
{
	const char *slash;
	size_t length;

	/* Up to the last slash. */
	slash = strrchr(path, '/');
	length = 0U;
	if (slash != NULL)
		length = (size_t)(slash - path);
	if (length == 0U)
		length = 1U;
	if (length >= size)
		length = size - 1U;
	memcpy(parent, path, length);
	parent[length] = '\0';
}
