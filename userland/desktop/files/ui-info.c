/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The information card of files (Get Info, spec §21) and the
 * opening of files with an application (spec §14).
 *
 * The card is drawn over the window like Quick Look: a table of what
 * info.c gathered (the owner and the permissions shown fully, as a BSD
 * user expects), the checksum computed when asked for, and a pill for each
 * way the file can be opened.  Opening a file from the window (a double
 * click, Enter) takes the first of those ways; the card's pills take any
 * of them.
 */

#include "files.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The card's width, its padding, the width of its labels' column and the height of a row. */
#define INFO_WIDTH		580
#define INFO_PADDING		24
#define INFO_LABEL_WIDTH	132
#define INFO_ROW		22
#define INFO_HEADER		76

/* The height of a usual card, whose top the card keeps. */
#define INFO_USUAL_HEIGHT	540

/* The text sizes of the card. */
#define INFO_TEXT_NAME		16U
#define INFO_TEXT_ROW		12U

/* The most rows the card has (the attributes each take one). */
#define INFO_ROWS		(24 + FM_INFO_ATTRIBUTES)

/* How long a round of the checksum may take, in milliseconds. */
#define INFO_CHECKSUM_BUDGET	10U

/*
 * One row of the card being drawn: its label (empty for a row that
 * continues the one before) and its value.
 */
struct info_row {
	const char *label;
	char value[FM_PATH_MAX];
};

static int info_rows(struct fm_app *app, struct info_row *rows);
static void info_add(struct info_row *rows, int *count, const char *label, const char *value);
static void info_date(time_t when, char *text, size_t size);
static void info_bytes(uint64_t bytes, char *text, size_t size);
static void info_person(struct fm_app *app, char *owner, size_t owner_size, char *group, size_t group_size);
static void info_header(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *card);
static int info_checksum_row(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *card, int y);
static int info_openers(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *card, int y, int draw);
static int info_pill(struct fm_app *app, struct kl_canvas *canvas, int x, int y, const char *label, int index, int primary, int draw);

/*
 * Opens the information card on the item shown by the preview (the
 * cursor's, or the first selected), or on the folder shown when nothing is
 * selected; closes it when it is open.
 */
void
fm_info_open(
	struct fm_app *app)
{
	struct fm_tab *tab;
	const char *path;
	int item;

	/* Open: it closes. */
	if (app->info_open != 0) {
		fm_info_close(app);
		return;
	}

	/* The item shown, or else the folder. */
	tab = fm_ui_tab(app);
	item = fm_preview_item(app);
	path = fm_current_folder(app);
	if (item >= 0)
		path = tab->listing.entries[item].path;

	/* A place that is neither says so. */
	if (path == NULL) {
		fm_ui_message(app, "Select an item to get its information.");
		return;
	}

	/* What the card shows, over the window (Quick Look gives way). */
	(void)fm_info_gather(&app->info, path);
	app->info_open = 1;
	app->quicklook = 0;
	app->dirty = 1;
}

/*
 * Closes the information card, stopping a checksum in progress.
 */
void
fm_info_close(
	struct fm_app *app)
{
	/* Already closed. */
	if (app->info_open == 0)
		return;

	/* The card and what it showed go. */
	fm_info_release(&app->info);
	app->info_open = 0;
	app->dirty = 1;
	fm_log("INFO close");
}

/*
 * Draws the information card over the window when it is open.
 */
void
fm_info_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	static struct info_row rows[INFO_ROWS];
	struct kl_rect whole;
	struct kl_rect card;
	int count;
	int index;
	int height;
	int baseline;
	int y;

	/* Closed, nothing is drawn. */
	if (app->info_open == 0)
		return;

	/* The window dimmed; a click on it closes the card. */
	whole.x = 0;
	whole.y = 0;
	whole.width = app->width;
	whole.height = app->height;
	kl_canvas_fill(canvas, &whole, KL_RGBA(0x1b2233, 90));
	fm_ui_hit(app, &whole, FM_HIT_OVERLAY, FM_OVERLAY_INFO_GROUND);

	/* The rows, and the card's height: the header, the rows, the checksum and the openers. */
	count = info_rows(app, rows);
	card.width = INFO_WIDTH;
	if (card.width > app->width - 40)
		card.width = app->width - 40;
	card.x = (app->width - card.width) / 2;
	height = INFO_HEADER + count * INFO_ROW + 16;
	if (app->info.error == 0 && app->info.folder == 0) {
		height += 2 * INFO_ROW + 8;
		height += info_openers(app, canvas, &card, 0, 0) + 8;
	}

	/*
	 * The card's top where a card of the usual height would be centred,
	 * so that its rows stay in place whatever its length, moved up when
	 * it would pass the window's bottom.
	 */
	height += INFO_PADDING;
	card.height = height;
	card.y = (app->height - INFO_USUAL_HEIGHT) / 2;
	if (card.y + card.height > app->height - 10)
		card.y = app->height - 10 - card.height;
	if (card.y < 10)
		card.y = 10;

	/* The card; a click on it stays there. */
	kl_canvas_shadow(canvas, (float)card.x, (float)card.y + 10.0f, (float)card.width, (float)card.height, 18.0f, 30.0f, KL_RGBA(0x0f1a33, 90));
	kl_canvas_round(canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 18.0f, FM_COLOR_PANEL);
	fm_ui_hit(app, &card, FM_HIT_OVERLAY, FM_OVERLAY_CARD);
	info_header(app, canvas, &card);

	/* Each row: its label in the quiet column, its value beside it. */
	y = card.y + INFO_HEADER;
	for (index = 0; index < count; index++) {
		baseline = kl_text_center(INFO_TEXT_ROW, y, INFO_ROW);
		(void)kl_text_draw(app->text, canvas, card.x + INFO_PADDING, baseline, rows[index].label, strlen(rows[index].label), INFO_TEXT_ROW, 0, FM_COLOR_TEXT_SECONDARY);
		(void)kl_text_draw_fit(app->text, canvas, card.x + INFO_PADDING + INFO_LABEL_WIDTH, baseline, rows[index].value, INFO_TEXT_ROW, 0, card.width - 2 * INFO_PADDING - INFO_LABEL_WIDTH, FM_COLOR_TEXT);
		y += INFO_ROW;
	}

	/* A file's checksum and the ways it opens. */
	if (app->info.error != 0 || app->info.folder != 0)
		return;
	y = info_checksum_row(app, canvas, &card, y + 8);
	(void)info_openers(app, canvas, &card, y + 8, 1);
}

/*
 * Moves the checksum on while the card is open.
 */
void
fm_info_tick(
	struct fm_app *app)
{
	int moved;

	/* Only an open card computes. */
	if (app->info_open == 0)
		return;

	/* A round of the checksum; its progress shows in a new frame. */
	moved = fm_info_checksum_step(&app->info, INFO_CHECKSUM_BUDGET);
	if (moved != 0)
		app->dirty = 1;
}

/*
 * Carries out a button of the card: close, compute the checksum, or open
 * the file with one of its ways.
 */
void
fm_info_button(
	struct fm_app *app,
	int index)
{
	char path[FM_PATH_MAX];
	struct fm_opener opener;
	int error;

	/* The close button. */
	if (index == FM_BUTTON_INFO_CLOSE) {
		fm_info_close(app);
		return;
	}

	/* The checksum's button starts it. */
	if (index == FM_BUTTON_INFO_CHECKSUM) {
		error = fm_info_checksum_start(&app->info);
		if (error != 0)
			fm_ui_message(app, "The checksum can't be computed.");
		app->dirty = 1;
		return;
	}

	/* An opener's pill: the card closes and the file opens with it. */
	if (index < FM_BUTTON_OPENER || index >= FM_BUTTON_OPENER + app->info.opener_count)
		return;
	opener = app->info.openers[index - FM_BUTTON_OPENER];
	snprintf(path, sizeof(path), "%s", app->info.path);
	fm_info_close(app);
	fm_open_with(app, &opener, path);
}

/*
 * Opens an item of the listing with one of its ways (0 is the default):
 * Quick Look shows it in the window, any other way starts its program.
 */
void
fm_open_entry(
	struct fm_app *app,
	int index,
	int opener)
{
	struct fm_opener openers[FM_OPENERS];
	const struct fm_mime *mime;
	struct fm_entry *entry;
	struct fm_tab *tab;
	int count;
	int quicklook;

	/* The item, and the type its contents tell. */
	tab = fm_ui_tab(app);
	if (index < 0 || (size_t)index >= tab->listing.count)
		return;
	entry = &tab->listing.entries[index];
	mime = fm_mime_sniff(entry->path, entry->mime);

	/* The ways it opens, and the one asked for. */
	count = fm_apps_for(entry->path, mime, entry->mode, openers, FM_OPENERS);
	if (opener < 0 || opener >= count)
		return;

	/* Quick Look shows the item, which becomes the selection. */
	quicklook = fm_apps_is_quicklook(&openers[opener]);
	if (quicklook != 0) {
		fm_select_only(tab, index);
		app->quicklook = 0;
		fm_look_toggle(app);
		fm_log("OPEN path=%s app=%s", entry->path, openers[opener].name);
		fm_recent_add(entry->path);
		return;
	}

	/* Any other way starts its program. */
	fm_open_with(app, &openers[opener], entry->path);
}

/*
 * Opens a path with a way that starts a program, keeps it among the recent
 * files, and says so in the status pill.
 */
void
fm_open_with(
	struct fm_app *app,
	const struct fm_opener *opener,
	const char *path)
{
	const char *name;
	char message[160];
	int quicklook;
	int error;

	/* Quick Look is the window's own, not a program's. */
	quicklook = fm_apps_is_quicklook(opener);
	if (quicklook != 0) {
		fm_look_toggle(app);
		return;
	}

	/* The program, on its way. */
	error = fm_apps_launch(opener, path);
	fm_log("OPEN path=%s app=%s error=%d", path, opener->name, error);

	/* The name of the file, for the message. */
	name = strrchr(path, '/');
	if (name == NULL)
		name = path;
	else
		name++;

	/* A program that could not start says so. */
	if (error != 0) {
		snprintf(message, sizeof(message), "Couldn't open \"%s\".", name);
		fm_ui_message(app, message);
		return;
	}

	/* The file is kept among the recent ones, and the pill says what opens it. */
	fm_recent_add(path);
	snprintf(message, sizeof(message), "Opening \"%s\" with %s", name, opener->name);
	fm_ui_message(app, message);
}

/* Fills the card's rows from what was gathered; returns how many there are. */
static int
info_rows(
	struct fm_app *app,
	struct info_row *rows)
{
	const struct fm_info *info;
	char value[FM_PATH_MAX];
	char owner[128];
	char group[128];
	const char *slash;
	int count;
	int index;
	int is_link;

	/* A path that could not be read says why, and nothing more. */
	info = &app->info;
	count = 0;
	if (info->error != 0) {
		snprintf(value, sizeof(value), "%s", strerror(info->error));
		info_add(rows, &count, "Can't read", value);
		return count;
	}

	/* Where it is: the folder that holds it. */
	slash = strrchr(info->path, '/');
	snprintf(value, sizeof(value), "/");
	if (slash != NULL && slash != info->path)
		snprintf(value, sizeof(value), "%.*s", (int)(slash - info->path), info->path);
	info_add(rows, &count, "Where", value);

	/* Its kind and its MIME type. */
	info_add(rows, &count, "Kind", info->mime->kind);
	info_add(rows, &count, "MIME type", info->mime->type);

	/* Its size: a folder's items, a file's bytes. */
	if (info->folder != 0) {
		fm_dir_items_text(info->child_count, value, sizeof(value));
	} else {
		info_bytes(info->size, value, sizeof(value));
	}

	/* The size's row. */
	info_add(rows, &count, "Size", value);

	/* Its times; the file system keeps no time of creation. */
	info_add(rows, &count, "Created", "\xe2\x80\x94");
	info_date(info->modified, value, sizeof(value));
	info_add(rows, &count, "Modified", value);
	info_date(info->changed, value, sizeof(value));
	info_add(rows, &count, "Changed", value);
	info_date(info->accessed, value, sizeof(value));
	info_add(rows, &count, "Accessed", value);

	/* Who owns it and what each may do with it. */
	info_person(app, owner, sizeof(owner), group, sizeof(group));
	info_add(rows, &count, "Owner", owner);
	info_add(rows, &count, "Group", group);
	fm_mode_text(info->mode, value, sizeof(value));
	info_add(rows, &count, "Permissions", value);
	snprintf(value, sizeof(value), "%ld", info->links);
	info_add(rows, &count, "Links", value);

	/* A link's target. */
	is_link = S_ISLNK(info->mode);
	if (is_link != 0)
		info_add(rows, &count, "Link to", info->target);

	/* Its extended attributes' names, one a row. */
	if (info->attribute_count == 0)
		info_add(rows, &count, "Attributes", "None");
	for (index = 0; index < info->attribute_count; index++) {
		snprintf(value, sizeof(value), "%s", info->attributes[index].name);
		info_add(rows, &count, "", value);
		if (index == 0)
			rows[count - 1].label = "Attributes";
	}

	/* More than the card lists. */
	if (info->attributes_more != 0)
		info_add(rows, &count, "", "\xe2\x80\xa6");

	/* Reports how many rows there are. */
	return count;
}

/* Adds a row to the card's table, when there is room. */
static void
info_add(
	struct info_row *rows,
	int *count,
	const char *label,
	const char *value)
{
	/* The table is full. */
	if (*count >= INFO_ROWS)
		return;

	/* The row. */
	rows[*count].label = label;
	snprintf(rows[*count].value, sizeof(rows[*count].value), "%s", value);
	(*count)++;
}

/* Writes a time in full, in local time: "2026-09-27 16:20:05". */
static void
info_date(
	time_t when,
	char *text,
	size_t size)
{
	struct tm *moment;

	/* The local time. */
	moment = localtime(&when);
	if (moment == NULL) {
		snprintf(text, size, "\xe2\x80\x94");
		return;
	}

	/* Written out. */
	snprintf(text, size, "%04d-%02d-%02d %02d:%02d:%02d", moment->tm_year + 1900, moment->tm_mon + 1, moment->tm_mday, moment->tm_hour, moment->tm_min, moment->tm_sec);
}

/* Writes a size shortly and in bytes with thousands separated: "122.9 KB (122,895 bytes)". */
static void
info_bytes(
	uint64_t bytes,
	char *text,
	size_t size)
{
	char digits[32];
	char grouped[48];
	char shortly[32];
	size_t length;
	size_t done;
	size_t index;

	/* A size under a thousand bytes is said in bytes once. */
	if (bytes < 1000U) {
		fm_dir_size_text(bytes, text, size);
		return;
	}

	/* The digits, and a comma before each group of three from the right. */
	snprintf(digits, sizeof(digits), "%llu", (unsigned long long)bytes);
	length = strlen(digits);
	done = 0;
	for (index = 0; index < length; index++) {
		if (index != 0U && (length - index) % 3U == 0U) {
			grouped[done] = ',';
			done++;
		}

		/* The digit itself. */
		grouped[done] = digits[index];
		done++;
	}

	/* The grouped digits end there. */
	grouped[done] = '\0';

	/* The short size, and the bytes after it. */
	fm_dir_size_text(bytes, shortly, sizeof(shortly));
	snprintf(text, size, "%s (%s bytes)", shortly, grouped);
}

/* Writes the owner and the group by name, each with its number: "awe (1000)". */
static void
info_person(
	struct fm_app *app,
	char *owner,
	size_t owner_size,
	char *group,
	size_t group_size)
{
	struct passwd *account;
	struct group *team;

	/* The owner's name when the accounts know it. */
	snprintf(owner, owner_size, "%lu", (unsigned long)app->info.uid);
	account = getpwuid(app->info.uid);
	if (account != NULL && account->pw_name != NULL)
		snprintf(owner, owner_size, "%s (%lu)", account->pw_name, (unsigned long)app->info.uid);

	/* The group's likewise. */
	snprintf(group, group_size, "%lu", (unsigned long)app->info.gid);
	team = getgrgid(app->info.gid);
	if (team != NULL && team->gr_name != NULL)
		snprintf(group, group_size, "%s (%lu)", team->gr_name, (unsigned long)app->info.gid);
}

/* Draws the card's header: the item's icon, name and kind, and the close button. */
static void
info_header(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *card)
{
	struct fm_entry entry;
	struct kl_rect close;
	const char *name;

	/* The item's icon, as the listing draws it. */
	name = strrchr(app->info.path, '/');
	if (name == NULL || name[1] == '\0')
		name = app->info.path;
	else
		name++;
	memset(&entry, 0, sizeof(entry));
	entry.name = (char *)name;
	entry.path = app->info.path;
	entry.mime = app->info.mime;
	entry.modified = app->info.modified;
	entry.folder = app->info.folder;
	fm_grid_entry_icon(app, canvas, &entry, (float)(card->x + INFO_PADDING), (float)card->y + 16.0f, 44.0f);

	/* Its name and kind beside it. */
	(void)kl_text_draw_fit(app->text, canvas, card->x + INFO_PADDING + 56, card->y + 36, name, INFO_TEXT_NAME, 1, card->width - 2 * INFO_PADDING - 100, FM_COLOR_TEXT);
	(void)kl_text_draw_fit(app->text, canvas, card->x + INFO_PADDING + 56, card->y + 56, "Information", 12U, 0, card->width - 2 * INFO_PADDING - 100, FM_COLOR_TEXT_SECONDARY);

	/* The close button at the right, lit under the pointer. */
	close.x = card->x + card->width - 44;
	close.y = card->y + 16;
	close.width = 28;
	close.height = 28;
	fm_icon_button(app, canvas, FM_WIDGET_ICON, FM_BUTTON_INFO_CLOSE, &close, KL_ICON_CLOSE, 16, KL_BUTTON_ROUND);
	fm_ui_hit(app, &close, FM_HIT_BUTTON, FM_BUTTON_INFO_CLOSE);

	/* A rule under the header. */
	kl_canvas_round(canvas, (float)(card->x + INFO_PADDING), (float)(card->y + INFO_HEADER - 8), (float)(card->width - 2 * INFO_PADDING), 1.0f, 0.0f, FM_COLOR_SEPARATOR);
}

/* Draws the checksum's two rows: a Compute button, its progress, or the digest in two halves; returns the next row's top. */
static int
info_checksum_row(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *card,
	int y)
{
	const struct fm_info *info;
	char text[160];
	int baseline;
	int x;
	int percent;

	/* The label. */
	info = &app->info;
	x = card->x + INFO_PADDING + INFO_LABEL_WIDTH;
	baseline = kl_text_center(INFO_TEXT_ROW, y, INFO_ROW);
	(void)kl_text_draw(app->text, canvas, card->x + INFO_PADDING, baseline, "SHA-256", 7, INFO_TEXT_ROW, 0, FM_COLOR_TEXT_SECONDARY);

	/* Not asked for yet: the button that asks. */
	if (info->checksum_state == FM_CHECKSUM_NONE) {
		(void)info_pill(app, canvas, x, y - 2, "Compute", FM_BUTTON_INFO_CHECKSUM, 0, 1);
		return y + 2 * INFO_ROW;
	}

	/* Being computed: how far. */
	if (info->checksum_state == FM_CHECKSUM_RUNNING) {
		percent = 100;
		if (info->size != 0U)
			percent = (int)(info->checksum_done * 100U / info->size);
		snprintf(text, sizeof(text), "Computing\xe2\x80\xa6 %d%%", percent);
		(void)kl_text_draw(app->text, canvas, x, baseline, text, strlen(text), INFO_TEXT_ROW, 0, FM_COLOR_TEXT_SECONDARY);
		return y + 2 * INFO_ROW;
	}

	/* Failed: why. */
	if (info->checksum_state == FM_CHECKSUM_FAILED) {
		snprintf(text, sizeof(text), "Can't compute: %s", strerror(info->checksum_error));
		(void)kl_text_draw_fit(app->text, canvas, x, baseline, text, INFO_TEXT_ROW, 0, card->width - 2 * INFO_PADDING - INFO_LABEL_WIDTH, FM_COLOR_TEXT_SECONDARY);
		return y + 2 * INFO_ROW;
	}

	/* Done: the digest's two halves, one a line. */
	(void)kl_text_draw(app->text, canvas, x, baseline, info->checksum, 32, INFO_TEXT_ROW, 0, FM_COLOR_TEXT);
	(void)kl_text_draw(app->text, canvas, x, baseline + INFO_ROW, info->checksum + 32, strlen(info->checksum + 32), INFO_TEXT_ROW, 0, FM_COLOR_TEXT);

	/* Reports where the next row starts. */
	return y + 2 * INFO_ROW;
}

/*
 * Draws the pills of the ways the file opens, the default first and lit,
 * wrapping onto more lines as needed (or only measures them when draw is
 * zero); returns their height.
 */
static int
info_openers(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *card,
	int y,
	int draw)
{
	const char *label;
	int baseline;
	int right;
	int left;
	int width;
	int x;
	int top;
	int index;

	/* The label. */
	left = card->x + INFO_PADDING + INFO_LABEL_WIDTH;
	right = card->x + card->width - INFO_PADDING;
	if (draw != 0) {
		baseline = kl_text_center(INFO_TEXT_ROW, y, INFO_ROW);
		label = "Open with";
		(void)kl_text_draw(app->text, canvas, card->x + INFO_PADDING, baseline, label, strlen(label), INFO_TEXT_ROW, 0, FM_COLOR_TEXT_SECONDARY);
	}

	/* Each way's pill, a new line when one does not fit. */
	x = left;
	top = y;
	for (index = 0; index < app->info.opener_count; index++) {
		width = info_pill(app, canvas, x, top - 2, app->info.openers[index].name, FM_BUTTON_OPENER + index, index == 0, 0);
		if (x != left && x + width > right) {
			x = left;
			top += INFO_ROW + 8;
		}

		/* The pill where it fits. */
		(void)info_pill(app, canvas, x, top - 2, app->info.openers[index].name, FM_BUTTON_OPENER + index, index == 0, draw);
		x += width + 8;
	}

	/* Reports how tall the pills are. */
	return top + INFO_ROW + 4 - y;
}

/* Draws a pill button (the primary one in the accent) at a point, or only measures it; returns its width. */
static int
info_pill(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	const char *label,
	int index,
	int primary,
	int draw)
{
	struct kl_rect rect;
	unsigned flags;
	int width;

	/* libkeiland's button fitting its label (ws090-p023); measured only when not drawn. */
	width = fm_button_width(app, label);
	if (draw == 0)
		return width;

	/* The primary one in the accent. */
	rect.x = x;
	rect.y = y;
	rect.width = width;
	rect.height = INFO_ROW + 4;
	flags = 0U;
	if (primary != 0)
		flags = KL_BUTTON_PRIMARY;
	fm_button(app, canvas, &rect, label, index, flags);

	/* Reports its width. */
	return width;
}
