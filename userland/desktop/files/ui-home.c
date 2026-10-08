/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Today, the dashboard of files (spec §5, §19, §28): not a listing
 * of the home folder but a place to start work from.  It was the place
 * called Home until ws127-p011 (the user's decision of 2026-10-04/05):
 * Home is now the home folder itself, and the dashboard is Today, at the
 * top of the sidebar, where Files starts.
 *
 * At the top a hero card shows the desktop's wallpaper (the picture the
 * system carries under /usr/share/keiland, or --wallpaper=, read as any
 * picture Files shows, ws138-p001; without it a quiet landscape is drawn) with a greeting and a line about the files.  Below
 * it the usual folders as cards with their item counts, the recent files
 * (the desktop's recent list, newest first) and the folders opened lately
 * (the file manager's own list).  Everything is gathered when the
 * dashboard is shown; the hero's picture is scaled once for its size.
 *
 * The hero card carries the Kei mark and word at its upper right, as the
 * boot screen does (ws035-p108); fm_mark_draw draws the mark for the empty
 * folders too.
 */

#include "files.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <keiland/keiland.h>
#include "../artwork/mark.h"


/* The dashboard's measurements. */
#define HOME_PADDING		22
#define HOME_HERO_HEIGHT	176
#define HOME_CARD_WIDTH		148
#define HOME_CARD_HEIGHT	88
#define HOME_CARD_GAP		14
#define HOME_SECTION_GAP	28
#define HOME_ROW_HEIGHT		44
#define HOME_PILL_HEIGHT	32

/* The largest Kei mark drawn, in pixels a side (its layers are kept rendered at the last size). */
#define HOME_MARK_MAX		128U

/* The first card index of the removable devices (the folders' cards are 0.., the recent folders' 100..). */
#define HOME_DEVICE_CARD	200

/* The size the hero's picture is made within (as large as the Today page's card is drawn). */
#define HOME_HERO_SOURCE_WIDTH	1920
#define HOME_HERO_SOURCE_HEIGHT	1200

/* The mark's size on the hero card. */
#define HOME_HERO_MARK		56

/* How many recent folders the file manager keeps. */
#define HOME_FOLDERS_KEPT	12

/*
 * One of the usual folders the dashboard shows as a card.
 */
struct home_folder {
	const char *label;
	const char *name;
};

/* The cards, in their order (Projects only when it exists). */
static const struct home_folder home_folders[] = {
	{ "Documents", "Documents" },
	{ "Pictures", "Pictures" },
	{ "Music", "Music" },
	{ "Movies", "Movies" },
	{ "Downloads", "Downloads" },
	{ "Projects", "Projects" }
};

/*
 * The recent list read for the dashboard.  It is large (a path an entry),
 * so it lives here rather than on the stack; it is filled when the
 * dashboard is gathered and read only then.
 */
static struct kl_recent_item home_recent_items[FM_HOME_RECENTS * 4];

static void home_hero(struct fm_app *app, struct kl_canvas *canvas, int x, int y, int width);
static void home_hero_art(struct kl_image *image);
static void home_hero_fallback(struct fm_app *app);
static void home_hero_brand(struct fm_app *app, struct kl_canvas *canvas, int x, int y, int width);
static int home_section(struct fm_app *app, struct kl_canvas *canvas, int x, int y, int width, const char *title, int link);
static int home_cards(struct fm_app *app, struct kl_canvas *canvas, int x, int y, int width);
static int home_devices(struct fm_app *app, struct kl_canvas *canvas, int x, int y, int width);
static int home_recents(struct fm_app *app, struct kl_canvas *canvas, int x, int y, int width);
static int home_folder_pills(struct fm_app *app, struct kl_canvas *canvas, int x, int y, int width);
static void home_where(struct fm_app *app, const char *path, char *text, size_t size);
static void home_folders_file(char *path, size_t size);

/*
 * Gathers what the dashboard shows: the folders' counts, the recent files
 * and folders, and the hero's words.
 */
void
fm_home_gather(
	struct fm_app *app)
{
	struct fm_dashboard *board;
	struct stat status;
	struct tm today;
	struct tm moment;
	struct tm *converted;
	char path[FM_PATH_MAX];
	char line[FM_PATH_MAX];
	const char *greeting;
	const char *named;
	char *newline;
	char *read;
	FILE *file;
	time_t now;
	size_t count;
	size_t index;
	int opened_today;
	int folder;
	int error;

	/* Nothing yet. */
	board = &app->dashboard;
	board->card_count = 0;
	board->recent_count = 0;
	board->folder_count = 0;

	/* The usual folders that exist, with their item counts. */
	for (index = 0; index < sizeof(home_folders) / sizeof(home_folders[0]); index++) {
		snprintf(path, sizeof(path), "%.1000s/%s", app->home, home_folders[index].name);
		folder = 0;
		error = stat(path, &status);
		if (error == 0)
			folder = S_ISDIR(status.st_mode);
		if (folder == 0)
			continue;

		/* The card. */
		snprintf(board->cards[board->card_count].path, sizeof(board->cards[0].path), "%s", path);
		snprintf(board->cards[board->card_count].label, sizeof(board->cards[0].label), "%s", home_folders[index].label);
		board->cards[board->card_count].count = fm_dir_count(path, app->show_hidden);
		board->card_count++;
	}

	/* The recent files that are still there (not folders), and how many were opened today. */
	now = time(NULL);
	converted = localtime(&now);
	memset(&today, 0, sizeof(today));
	if (converted != NULL)
		today = *converted;
	opened_today = 0;
	count = 0;
	error = kl_recent_list(home_recent_items, sizeof(home_recent_items) / sizeof(home_recent_items[0]), &count);
	for (index = 0; error == 0 && index < count && board->recent_count < FM_HOME_RECENTS; index++) {
		folder = 1;
		error = stat(home_recent_items[index].path, &status);
		if (error == 0)
			folder = S_ISDIR(status.st_mode);
		error = 0;
		if (folder != 0)
			continue;

		/* The file, and when it was opened. */
		snprintf(board->recents[board->recent_count].path, sizeof(board->recents[0].path), "%.1023s", home_recent_items[index].path);
		board->recents[board->recent_count].time = (time_t)home_recent_items[index].time;
		board->recent_count++;

		/* Opened today: the same calendar day. */
		converted = localtime(&board->recents[board->recent_count - 1].time);
		if (converted == NULL)
			continue;
		moment = *converted;
		if (moment.tm_year == today.tm_year && moment.tm_yday == today.tm_yday)
			opened_today++;
	}

	/* The folders opened lately, from the file manager's list. */
	home_folders_file(path, sizeof(path));
	file = fopen(path, "r");
	while (file != NULL && board->folder_count < FM_HOME_FOLDERS) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;
		newline = strchr(line, '\n');
		if (newline != NULL)
			*newline = '\0';
		folder = 0;
		error = stat(line, &status);
		if (error == 0)
			folder = S_ISDIR(status.st_mode);
		if (folder == 0)
			continue;

		/* A folder that is there. */
		snprintf(board->folders[board->folder_count], sizeof(board->folders[0]), "%s", line);
		board->folder_count++;
	}

	/* The list is read. */
	if (file != NULL)
		fclose(file);

	/* The greeting by the time of day, and the line about the files. */
	if (today.tm_hour < 12) {
		greeting = kl_tr("Good morning");
		named = kl_tr("Good morning, {1}");
	} else if (today.tm_hour < 18) {
		greeting = kl_tr("Good afternoon");
		named = kl_tr("Good afternoon, {1}");
	} else {
		greeting = kl_tr("Good evening");
		named = kl_tr("Good evening, {1}");
	}

	/* With the user's name when it is known (the language puts it where its grammar wants). */
	snprintf(board->greeting, sizeof(board->greeting), "%s", greeting);
	if (app->user[0] != '\0')
		(void)kl_tr_format(board->greeting, sizeof(board->greeting), named, app->user, (const char *)NULL);

	/*
	 * The line about the files, with the space left on the home's file
	 * system as the desktop last read it; a new reading is asked for (the
	 * main loop asks the desktop, ws188-p002), and its answer writes the
	 * line again.
	 */
	board->opened_today = opened_today;
	app->home_free_wanted = 1;
	fm_home_summary(app);
}

/*
 * Writes the dashboard's line about the files: how many were opened today,
 * and the space left on the home's file system when the desktop told it.
 */
void
fm_home_summary(
	struct fm_app *app)
{
	struct fm_dashboard *board;
	char free_text[32];
	char free_line[64];
	char number[32];
	size_t length;

	/* How many files were opened today. */
	board = &app->dashboard;
	snprintf(number, sizeof(number), "%d", board->opened_today);
	(void)kl_tr_format(board->summary, sizeof(board->summary), kl_trn("{1} file opened today", "{1} files opened today", (unsigned long)board->opened_today), number, (const char *)NULL);

	/* The space left is not known yet. */
	if (!app->home_free_known)
		return;

	/* The space left, after a dot. */
	fm_dir_size_text(app->home_free, free_text, sizeof(free_text));
	(void)kl_tr_format(free_line, sizeof(free_line), kl_tr("{1} free"), free_text, (const char *)NULL);
	length = strlen(board->summary);
	snprintf(board->summary + length, sizeof(board->summary) - length, " \xc2\xb7 %s", free_line);
}

/*
 * Keeps a folder the user opened at the top of the file manager's list of
 * recent folders.
 */
void
fm_home_folder_opened(
	const char *folder)
{
	char path[FM_PATH_MAX];
	char temporary[FM_PATH_MAX + 8];
	char line[FM_PATH_MAX];
	char *newline;
	char *read;
	FILE *in;
	FILE *out;
	int kept;
	int match;

	/* A new list: the folder first. */
	home_folders_file(path, sizeof(path));
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	out = fopen(temporary, "w");
	if (out == NULL)
		return;
	fprintf(out, "%s\n", folder);

	/* Then the old list without it, as far as the list goes. */
	kept = 1;
	in = fopen(path, "r");
	while (in != NULL && kept < HOME_FOLDERS_KEPT) {
		read = fgets(line, sizeof(line), in);
		if (read == NULL)
			break;
		newline = strchr(line, '\n');
		if (newline != NULL)
			*newline = '\0';
		match = strcmp(line, folder);
		if (match == 0 || line[0] != '/')
			continue;
		fprintf(out, "%s\n", line);
		kept++;
	}

	/* The old list is read. */
	if (in != NULL)
		fclose(in);

	/* The new list replaces the old. */
	fclose(out);
	(void)rename(temporary, path);
}

/*
 * Draws the dashboard in the content panel's inner rectangle (scrolled by
 * the tab's scroll), and records its cards and links.
 */
void
fm_home_draw(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *inner)
{
	struct fm_tab *tab;
	int width;
	int x;
	int y;

	/* From the top, scrolled. */
	tab = fm_ui_tab(app);
	x = inner->x + HOME_PADDING - 16;
	width = inner->width - 2 * (HOME_PADDING - 16);
	y = inner->y + 4 - tab->scroll;
	kl_canvas_clip_push(canvas, inner);

	/* The hero card. */
	home_hero(app, canvas, x, y, width);
	y += HOME_HERO_HEIGHT + HOME_SECTION_GAP;

	/* The removable devices, while there are some (ws132-p005). */
	if (app->places.device_count > 0) {
		y = home_section(app, canvas, x, y, width, "Devices", -1);
		y = home_devices(app, canvas, x, y, width) + HOME_SECTION_GAP;
	}

	/* The folders. */
	y = home_section(app, canvas, x, y, width, kl_tr("Folders"), 0);
	y = home_cards(app, canvas, x, y, width) + HOME_SECTION_GAP;

	/* The recent files. */
	y = home_section(app, canvas, x, y, width, kl_tr("Recent Files"), 1);
	y = home_recents(app, canvas, x, y, width) + HOME_SECTION_GAP;

	/* The recent folders. */
	if (app->dashboard.folder_count > 0) {
		y = home_section(app, canvas, x, y, width, kl_tr("Recent Folders"), -1);
		y = home_folder_pills(app, canvas, x, y, width) + HOME_SECTION_GAP;
	}

	/* The panel ends. */
	kl_canvas_clip_pop(canvas);

	/* How tall the dashboard is, for the scrolling. */
	app->layout.content_height = y + tab->scroll - inner->y + 64;
}

/*
 * Carries out a click on the dashboard: a folder card or pill opens its
 * folder, a recent file shows it in its folder, Show all opens the home
 * folder or Recents.
 */
void
fm_home_click(
	struct fm_app *app,
	unsigned kind,
	int index,
	int double_click)
{
	struct fm_location location;
	struct fm_tab *tab;
	char folder[FM_PATH_MAX];
	char *slash;
	int found;

	/* A removable device: a mounted one opens, one not mounted is mounted by a double click (ws132-p005). */
	if (kind == FM_HIT_CARD && index >= HOME_DEVICE_CARD && index - HOME_DEVICE_CARD < app->places.device_count) {
		if (app->places.devices[index - HOME_DEVICE_CARD].mounted || double_click != 0)
			fm_devices_mount(app, index - HOME_DEVICE_CARD);
		return;
	}

	/* Each region's place. */
	memset(&location, 0, sizeof(location));
	location.kind = FM_LOCATION_FOLDER;
	if (kind == FM_HIT_CARD && index >= 0 && index < app->dashboard.card_count) {
		snprintf(location.path, sizeof(location.path), "%s", app->dashboard.cards[index].path);
		fm_ui_go(app, &location);
	} else if (kind == FM_HIT_CARD && index >= 100 && index - 100 < app->dashboard.folder_count) {
		snprintf(location.path, sizeof(location.path), "%s", app->dashboard.folders[index - 100]);
		fm_ui_go(app, &location);
	} else if (kind == FM_HIT_SHOW_ALL && index == 0) {
		snprintf(location.path, sizeof(location.path), "%s", app->home);
		fm_ui_go(app, &location);
	} else if (kind == FM_HIT_SHOW_ALL && index == 1) {
		location.kind = FM_LOCATION_RECENTS;
		fm_ui_go(app, &location);
	} else if (kind == FM_HIT_RECENT && index >= 0 && index < app->dashboard.recent_count) {
		/* A recent file: its folder, with it selected (a double click opens it). */
		snprintf(folder, sizeof(folder), "%s", app->dashboard.recents[index].path);
		slash = strrchr(folder, '/');
		if (slash == NULL)
			return;
		*slash = '\0';
		if (folder[0] == '\0')
			snprintf(folder, sizeof(folder), "/");
		snprintf(location.path, sizeof(location.path), "%s", folder);
		fm_ui_go(app, &location);
		tab = fm_ui_tab(app);
		found = fm_select_find(tab, slash + 1);
		fm_select_only(tab, found);
		if (double_click != 0 && found >= 0)
			fm_ui_open(app, found);
	}
}

/* Draws the hero card: the wallpaper (or the drawn landscape) with a greeting over its left. */
static void
home_hero(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	struct kl_image view;
	struct kl_rect band;
	int crop_height;
	int error;

	/*
	 * Asks for the picture the first time, without waiting for it
	 * (ws177-p010): fm_home_tick takes it; the card is a gradient
	 * meanwhile.
	 */
	if (app->hero_source.pixels == NULL && app->hero_tried == 0) {
		app->hero_tried = 1;
		error = fm_picture_begin(FM_PICTURE_HERO, app->wallpaper, HOME_HERO_SOURCE_WIDTH, HOME_HERO_SOURCE_HEIGHT);
		if (error == 0)
			app->hero_pending = 1;
		else
			home_hero_fallback(app);
	}

	/* Scaled once for the card's size: the lower part of the picture, where the lake is, at the card's shape. */
	if (app->hero_source.pixels != NULL && (app->hero.width != width || app->hero.height != HOME_HERO_HEIGHT)) {
		kl_image_release(&app->hero);
		error = kl_image_create(&app->hero, width, HOME_HERO_HEIGHT);
		if (error == 0) {
			view = app->hero_source;
			crop_height = (int)((long)view.width * HOME_HERO_HEIGHT / width);
			if (crop_height > view.height)
				crop_height = view.height;
			view.pixels += (size_t)((view.height - crop_height) * 2 / 3) * view.stride;
			view.height = crop_height;
			kl_image_scale(&view, &app->hero);
		}
	}

	/* The card: its shadow, the picture with rounded corners, a veil at the left for the words. */
	kl_canvas_shadow(canvas, (float)x, (float)y + 6.0f, (float)width, HOME_HERO_HEIGHT, 18.0f, 16.0f, KL_RGBA(0x1f3a66, 45));
	if (app->hero.pixels != NULL) {
		kl_canvas_image(canvas, &app->hero, (float)x, (float)y, (float)width, HOME_HERO_HEIGHT, 18.0f, 1.0f);
	} else {
		kl_canvas_round_gradient(canvas, (float)x, (float)y, (float)width, HOME_HERO_HEIGHT, 18.0f, KL_RGB(0xbcd9f4), KL_RGB(0xe8f2f6));
	}

	/* A veil darkens the lower part, under the words. */
	band.x = x;
	band.y = y;
	band.width = width;
	band.height = HOME_HERO_HEIGHT;
	kl_canvas_clip_push(canvas, &band);
	kl_canvas_round_gradient(canvas, (float)x, (float)y + HOME_HERO_HEIGHT * 0.35f, (float)width, HOME_HERO_HEIGHT * 0.65f, 18.0f, KL_RGBA(0x0e1a2e, 0), KL_RGBA(0x0e1a2e, 120));
	kl_canvas_clip_pop(canvas);

	/* The greeting and the line about the files, in white at the lower left. */
	(void)kl_text_draw_fit(app->text, canvas, x + 28, y + HOME_HERO_HEIGHT - 56, app->dashboard.greeting, 26U, 1, width - 56, KL_RGB(0xffffff));
	(void)kl_text_draw_fit(app->text, canvas, x + 28, y + HOME_HERO_HEIGHT - 28, app->dashboard.summary, 14U, 0, width - 56, KL_RGBA(0xffffff, 230));

	/* The Kei mark and word at the upper right. */
	home_hero_brand(app, canvas, x, y, width);
}

/*
 * Takes the hero's picture when its child is done (the drawn landscape
 * when there is no picture).  Returns nonzero when it is, so that the
 * window is drawn again.
 */
int
fm_home_tick(
	struct fm_app *app)
{
	int finished;
	int error;

	/* No picture is being made. */
	if (app->hero_pending == 0)
		return 0;

	/* The picture, while it is made. */
	finished = fm_picture_follow(FM_PICTURE_HERO, &app->hero_source, &error);
	if (!finished)
		return 0;
	app->hero_pending = 0;

	/* A wallpaper that cannot be read gives the drawn landscape. */
	if (error != 0)
		home_hero_fallback(app);

	/* Succeeded: the picture is there, to be scaled for the card. */
	return 1;
}

/* Puts the drawn landscape in place of a wallpaper that cannot be read. */
static void
home_hero_fallback(
	struct fm_app *app)
{
	int error;

	/* The landscape, drawn once. */
	error = kl_image_create(&app->hero_source, 960, 360);
	if (error == 0)
		home_hero_art(&app->hero_source);
}

/* Draws the Kei mark and the word Kei (three letters, never a lone K) at the hero card's upper right. */
static void
home_hero_brand(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	int word;
	int left;

	/* The word's width, and where the pair starts so that it ends at the card's right margin. */
	word = kl_text_width(app->text, "Kei", 3U, 30U, 0);
	left = x + width - 24 - word - 6 - HOME_HERO_MARK;

	/* A card too narrow for the pair keeps only its words. */
	if (left < x + width / 2)
		return;

	/* The mark, then the word in slate beside its lower part. */
	fm_mark_draw(canvas, left, y + 18, (unsigned)HOME_HERO_MARK, 1.0f);
	(void)kl_text_draw(app->text, canvas, left + HOME_HERO_MARK + 6, y + 18 + HOME_HERO_MARK - 10, "Kei", 3U, 30U, 0, FM_COLOR_TITLE);
}

/*
 * Draws the Kei mark in a square of a size in pixels at (x, y), as opaque as
 * asked (0..1): its seven layers (userland/desktop/artwork/mark.c), each in
 * its colour.  The layers are rendered once for a size and kept.
 */
void
fm_mark_draw(
	struct kl_canvas *canvas,
	int x,
	int y,
	unsigned pixels,
	float opacity)
{
	/*
	 * The bar pale, its shade deeper, the leaf clearer and its shade the
	 * deep blue of the splash, the overlap deeper still, then the white
	 * light along the edges and the sheen (colour, alpha; ws035-p109).  The
	 * panes are translucent, so what is behind shows through.
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
	 * The layers at the size last drawn (zero before the first); a new size
	 * renders them again.  They live for the program's life.
	 */
	static uint8_t layers[KL_MARK_LAYERS][HOME_MARK_MAX * HOME_MARK_MAX];
	static unsigned layers_pixels;
	uint32_t alpha;
	unsigned layer;

	/* A mark larger than the kept layers is drawn at their largest. */
	if (pixels > HOME_MARK_MAX)
		pixels = HOME_MARK_MAX;
	if (pixels == 0U)
		return;

	/* The layers at this size. */
	if (layers_pixels != pixels) {
		for (layer = 0; layer < KL_MARK_LAYERS; layer++)
			kl_mark_raster(layer, pixels, layers[layer], pixels);
		layers_pixels = pixels;
	}

	/* Each layer in its colour, as opaque as asked. */
	for (layer = 0; layer < KL_MARK_LAYERS; layer++) {
		alpha = (uint32_t)((float)colours[layer][1] * opacity + 0.5f);
		kl_canvas_mask(canvas, x, y, layers[layer], (int)pixels, (int)pixels, pixels, KL_RGBA(colours[layer][0], alpha));
	}
}

/* Draws a quiet landscape into a picture in the Kei look: a bright sky, hills and a green-edged lake (when there is no wallpaper). */
static void
home_hero_art(
	struct kl_image *image)
{
	struct kl_canvas canvas;
	struct kl_rect whole;
	float far_hills[16];
	float near_hills[16];
	float width;
	float height;
	int error;

	/* A canvas over the picture. */
	error = kl_canvas_init(&canvas, image->pixels, image->stride, image->width, image->height);
	if (error != 0)
		return;
	width = (float)image->width;
	height = (float)image->height;

	/* The sky and the lake. */
	whole.x = 0;
	whole.y = 0;
	whole.width = image->width;
	whole.height = image->height * 3 / 5;
	kl_canvas_gradient(&canvas, &whole, KL_RGB(0xbcd9f4), KL_RGB(0xf1f6fb));
	whole.y = whole.height;
	whole.height = image->height - whole.y;
	kl_canvas_gradient(&canvas, &whole, KL_RGB(0xd3e6f3), KL_RGB(0xb5d3c0));

	/* The far hills, pale blue. */
	far_hills[0] = 0.0f;
	far_hills[1] = height * 0.60f;
	far_hills[2] = width * 0.12f;
	far_hills[3] = height * 0.38f;
	far_hills[4] = width * 0.30f;
	far_hills[5] = height * 0.50f;
	far_hills[6] = width * 0.52f;
	far_hills[7] = height * 0.30f;
	far_hills[8] = width * 0.74f;
	far_hills[9] = height * 0.47f;
	far_hills[10] = width * 0.90f;
	far_hills[11] = height * 0.36f;
	far_hills[12] = width;
	far_hills[13] = height * 0.44f;
	far_hills[14] = width;
	far_hills[15] = height * 0.60f;
	kl_canvas_polygon(&canvas, far_hills, 8, KL_RGB(0xc2d6ea));

	/* The near hills, young green (the Kei look, ws035-p108). */
	near_hills[0] = 0.0f;
	near_hills[1] = height * 0.60f;
	near_hills[2] = width * 0.20f;
	near_hills[3] = height * 0.48f;
	near_hills[4] = width * 0.38f;
	near_hills[5] = height * 0.56f;
	near_hills[6] = width * 0.60f;
	near_hills[7] = height * 0.46f;
	near_hills[8] = width * 0.82f;
	near_hills[9] = height * 0.55f;
	near_hills[10] = width;
	near_hills[11] = height * 0.50f;
	near_hills[12] = width;
	near_hills[13] = height * 0.60f;
	near_hills[14] = 0.0f;
	near_hills[15] = height * 0.60f;
	kl_canvas_polygon(&canvas, near_hills, 8, KL_RGB(0xa7c9a4));

	/* The canvas is let go (the pixels are the picture's). */
	kl_canvas_release(&canvas);
}

/* Draws a section's title (and a "Show all" link when link is 0 or 1), and returns where its content starts. */
static int
home_section(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width,
	const char *title,
	int link)
{
	struct kl_rect rect;
	kl_color ink;
	const char *label;
	int label_width;

	/* The title. */
	(void)kl_text_draw(app->text, canvas, x + 4, y + 16, title, strlen(title), 16U, 1, FM_COLOR_TEXT);

	/* The link at the right, when the section has one. */
	if (link >= 0) {
		label = kl_tr("Show all \xe2\x86\x92");
		label_width = kl_text_width(app->text, label, strlen(label), 13U, 0);
		rect.x = x + width - label_width - 8;
		rect.y = y;
		rect.width = label_width + 8;
		rect.height = 22;
		ink = FM_COLOR_ACCENT_TEXT;
		if (app->hover_kind == FM_HIT_SHOW_ALL && app->hover_index == link)
			ink = kl_color_mix(FM_COLOR_ACCENT_TEXT, KL_RGB(0x000000), 0.25f);
		(void)kl_text_draw(app->text, canvas, rect.x + 4, y + 16, label, strlen(label), 13U, 0, ink);
		fm_ui_hit(app, &rect, FM_HIT_SHOW_ALL, link);
	}

	/* The content starts under it. */
	return y + 30;
}

/* Draws the folder cards (wrapping to more rows when they do not fit), and returns where they end. */
static int
home_cards(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	const struct fm_home_card *card;
	struct kl_rect rect;
	char count[32];
	int per_row;
	int card_width;
	int index;

	/* As many cards a row as fit, stretched to fill it. */
	per_row = (width + HOME_CARD_GAP) / (HOME_CARD_WIDTH + HOME_CARD_GAP);
	if (per_row < 1)
		per_row = 1;
	if (per_row > app->dashboard.card_count && app->dashboard.card_count > 0)
		per_row = app->dashboard.card_count;
	card_width = (width - (per_row - 1) * HOME_CARD_GAP) / per_row;

	/* Each card: a white card with a folder, its name and how many items. */
	for (index = 0; index < app->dashboard.card_count; index++) {
		card = &app->dashboard.cards[index];
		rect.x = x + (index % per_row) * (card_width + HOME_CARD_GAP);
		rect.y = y + (index / per_row) * (HOME_CARD_HEIGHT + HOME_CARD_GAP);
		rect.width = card_width;
		rect.height = HOME_CARD_HEIGHT;
		kl_canvas_shadow(canvas, (float)rect.x, (float)rect.y + 3.0f, (float)rect.width, (float)rect.height, 14.0f, 8.0f, KL_RGBA(0x1f3a66, 22));
		kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 14.0f, FM_COLOR_PANEL);
		kl_canvas_round_border(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 14.0f, 1.0f, FM_COLOR_PANEL_EDGE);
		if (app->hover_kind == FM_HIT_CARD && app->hover_index == index)
			kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 14.0f, FM_COLOR_HOVER);
		kl_icon_folder(canvas, (float)rect.x + 12.0f, (float)rect.y + 10.0f, 44.0f, FM_COLOR_FOLDER);
		(void)kl_text_draw_fit(app->text, canvas, rect.x + 14, rect.y + 70, kl_tr(card->label), 14U, 1, rect.width - 28, FM_COLOR_TEXT);
		fm_dir_items_text((long)card->count, count, sizeof(count));
		(void)kl_text_draw_fit(app->text, canvas, rect.x + 64, rect.y + 36, count, 12U, 0, rect.width - 72, FM_COLOR_TEXT_SECONDARY);
		fm_ui_hit(app, &rect, FM_HIT_CARD, index);
	}

	/* Reports where the cards end. */
	if (app->dashboard.card_count == 0)
		return y;
	return y + ((app->dashboard.card_count + per_row - 1) / per_row) * (HOME_CARD_HEIGHT + HOME_CARD_GAP) - HOME_CARD_GAP;
}

/*
 * Draws the removable devices as cards (a volume, its name, and where it is
 * mounted or that a double click mounts it), a new one blinking, and
 * returns where they end.
 */
static int
home_devices(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	const struct fm_device *device;
	struct kl_rect rect;
	const char *where;
	float bright;
	int per_row;
	int card_width;
	int index;

	/* As many cards a row as fit, at most the folders' width each. */
	per_row = (width + HOME_CARD_GAP) / (HOME_CARD_WIDTH + HOME_CARD_GAP);
	if (per_row < 1)
		per_row = 1;
	card_width = (width - (per_row - 1) * HOME_CARD_GAP) / per_row;

	/* Each device: a white card with a volume, its name and its state. */
	for (index = 0; index < app->places.device_count; index++) {
		device = &app->places.devices[index];
		rect.x = x + (index % per_row) * (card_width + HOME_CARD_GAP);
		rect.y = y + (index / per_row) * (HOME_CARD_HEIGHT + HOME_CARD_GAP);
		rect.width = card_width;
		rect.height = HOME_CARD_HEIGHT;
		kl_canvas_shadow(canvas, (float)rect.x, (float)rect.y + 3.0f, (float)rect.width, (float)rect.height, 14.0f, 8.0f, KL_RGBA(0x1f3a66, 22));
		kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 14.0f, FM_COLOR_PANEL);
		kl_canvas_round_border(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 14.0f, 1.0f, FM_COLOR_PANEL_EDGE);
		if (app->hover_kind == FM_HIT_CARD && app->hover_index == HOME_DEVICE_CARD + index)
			kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 14.0f, FM_COLOR_HOVER);

		/* A new device blinks: the card lit and dimmed three times. */
		bright = fm_devices_blink(app, device);
		if (bright < 1.0f)
			kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 14.0f, KL_RGBA(FM_COLOR_ACCENT, (uint32_t)((1.0f - bright) * 110.0f)));

		/* The volume, the name, and where it is or how to mount it. */
		kl_icon_draw(canvas, KL_ICON_VOLUME, (float)rect.x + 14.0f, (float)rect.y + 12.0f, 40.0f, FM_COLOR_ACCENT);
		(void)kl_text_draw_fit(app->text, canvas, rect.x + 14, rect.y + 70, device->name, 14U, 1, rect.width - 28, FM_COLOR_TEXT);
		where = "Double-click to mount";
		if (device->mounted)
			where = device->path;
		(void)kl_text_draw_fit(app->text, canvas, rect.x + 64, rect.y + 36, where, 12U, 0, rect.width - 72, FM_COLOR_TEXT_SECONDARY);
		fm_ui_hit(app, &rect, FM_HIT_CARD, HOME_DEVICE_CARD + index);

		/* A card is logged once after the list changed (the tests click it). */
		if (!app->device_cards_logged)
			fm_log("DEVICE card id=%s x=%d y=%d width=%d height=%d", device->id, rect.x, rect.y, rect.width, rect.height);
	}

	/* The cards are logged. */
	app->device_cards_logged = 1;

	/* Reports where the cards end. */
	return y + ((app->places.device_count + per_row - 1) / per_row) * (HOME_CARD_HEIGHT + HOME_CARD_GAP) - HOME_CARD_GAP;
}

/* Draws the recent files as rows (icon, name, where it is, when), and returns where they end. */
static int
home_recents(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	const struct fm_home_recent *recent;
	struct fm_entry entry;
	struct kl_rect rect;
	char where[FM_PATH_MAX];
	char when[64];
	const char *empty;
	const char *name;
	int when_width;
	int index;

	/* Nothing opened yet. */
	if (app->dashboard.recent_count == 0) {
		empty = kl_tr("Files you open appear here.");
		(void)kl_text_draw(app->text, canvas, x + 4, y + 18, empty, strlen(empty), 13U, 0, FM_COLOR_TEXT_FAINT);
		return y + 28;
	}

	/* A white panel behind the rows. */
	kl_canvas_round(canvas, (float)x, (float)y, (float)width, (float)(app->dashboard.recent_count * HOME_ROW_HEIGHT + 8), 14.0f, FM_COLOR_PANEL);
	kl_canvas_round_border(canvas, (float)x, (float)y, (float)width, (float)(app->dashboard.recent_count * HOME_ROW_HEIGHT + 8), 14.0f, 1.0f, FM_COLOR_PANEL_EDGE);

	/* Each file. */
	for (index = 0; index < app->dashboard.recent_count; index++) {
		recent = &app->dashboard.recents[index];
		rect.x = x + 4;
		rect.y = y + 4 + index * HOME_ROW_HEIGHT;
		rect.width = width - 8;
		rect.height = HOME_ROW_HEIGHT;
		if (app->hover_kind == FM_HIT_RECENT && app->hover_index == index)
			kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 10.0f, FM_COLOR_HOVER);

		/* Its icon, from its name. */
		name = strrchr(recent->path, '/');
		if (name == NULL)
			name = recent->path;
		else
			name++;
		memset(&entry, 0, sizeof(entry));
		entry.name = (char *)name;
		entry.path = (char *)recent->path;
		entry.mime = fm_mime_guess(name, S_IFREG);
		fm_grid_entry_icon(app, canvas, &entry, (float)rect.x + 8.0f, (float)rect.y + 6.0f, 32.0f);

		/* Its name and where it is, and when it was opened at the right. */
		fm_time_text(recent->time, app->wall, when, sizeof(when));
		when_width = kl_text_width(app->text, when, strlen(when), 12U, 0);
		(void)kl_text_draw_fit(app->text, canvas, rect.x + 50, rect.y + 20, name, 14U, 0, rect.width - 80 - when_width, FM_COLOR_TEXT);
		home_where(app, recent->path, where, sizeof(where));
		(void)kl_text_draw_fit(app->text, canvas, rect.x + 50, rect.y + 36, where, 12U, 0, rect.width - 80 - when_width, FM_COLOR_TEXT_SECONDARY);
		(void)kl_text_draw(app->text, canvas, rect.x + rect.width - 12 - when_width, rect.y + 27, when, strlen(when), 12U, 0, FM_COLOR_TEXT_SECONDARY);
		fm_ui_hit(app, &rect, FM_HIT_RECENT, index);
	}

	/* Reports where the rows end. */
	return y + app->dashboard.recent_count * HOME_ROW_HEIGHT + 8;
}

/* Draws the recent folders as pills in a row (wrapping), and returns where they end. */
static int
home_folder_pills(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	struct fm_location location;
	struct kl_rect rect;
	const char *name;
	int pen;
	int row;
	int label_width;
	int index;

	/* Each folder, as wide as its name. */
	pen = x;
	row = y;
	for (index = 0; index < app->dashboard.folder_count; index++) {
		location.kind = FM_LOCATION_FOLDER;
		snprintf(location.path, sizeof(location.path), "%s", app->dashboard.folders[index]);
		name = kl_tr(fm_location_name(&location, app->home));
		label_width = kl_text_width(app->text, name, strlen(name), 13U, 0);
		if (label_width > 220)
			label_width = 220;
		rect.width = label_width + 50;
		rect.height = HOME_PILL_HEIGHT;

		/* A pill that does not fit starts a new row. */
		if (pen > x && pen + rect.width > x + width) {
			pen = x;
			row += HOME_PILL_HEIGHT + 10;
		}

		/* Its place. */
		rect.x = pen;
		rect.y = row;

		/* The pill: a folder and the name. */
		kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, HOME_PILL_HEIGHT * 0.5f, FM_COLOR_PANEL);
		kl_canvas_round_border(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, HOME_PILL_HEIGHT * 0.5f, 1.0f, FM_COLOR_PANEL_EDGE);
		if (app->hover_kind == FM_HIT_CARD && app->hover_index == 100 + index)
			kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, HOME_PILL_HEIGHT * 0.5f, FM_COLOR_HOVER);
		kl_icon_folder(canvas, (float)rect.x + 10.0f, (float)rect.y + 5.0f, 22.0f, FM_COLOR_FOLDER);
		(void)kl_text_draw_fit(app->text, canvas, rect.x + 38, kl_text_center(13U, rect.y, rect.height), name, 13U, 0, label_width, FM_COLOR_TEXT);
		fm_ui_hit(app, &rect, FM_HIT_CARD, 100 + index);
		pen += rect.width + 10;
	}

	/* Reports where the pills end. */
	return row + HOME_PILL_HEIGHT;
}

/* Writes where a file is, from the home folder: "Documents / Projects" (or its folder's path elsewhere). */
static void
home_where(
	struct fm_app *app,
	const char *path,
	char *text,
	size_t size)
{
	char folder[FM_PATH_MAX];
	char *slash;
	char *part;
	size_t length;
	size_t used;
	int match;

	/* The folder the file is in. */
	snprintf(folder, sizeof(folder), "%s", path);
	slash = strrchr(folder, '/');
	if (slash != NULL)
		*slash = '\0';

	/* Outside the home folder: the folder's path. */
	length = strlen(app->home);
	match = strncmp(folder, app->home, length);
	if (match != 0 || (folder[length] != '/' && folder[length] != '\0')) {
		snprintf(text, size, "%s", folder);
		return;
	}

	/* The home folder itself. */
	if (folder[length] == '\0') {
		snprintf(text, size, "Home");
		return;
	}

	/* Its parts under the home folder, joined by " / ". */
	used = 0;
	text[0] = '\0';
	part = folder + length + 1;
	while (part != NULL && *part != '\0' && used + 4U < size) {
		slash = strchr(part, '/');
		if (slash != NULL)
			*slash = '\0';
		if (used != 0)
			used += (size_t)snprintf(text + used, size - used, " / ");
		if (used < size)
			used += (size_t)snprintf(text + used, size - used, "%s", part);
		part = NULL;
		if (slash != NULL)
			part = slash + 1;
	}
}

/* Writes the path of the file manager's list of recent folders ($XDG_DATA_HOME/files/recent-folders), making its folder. */
static void
home_folders_file(
	char *path,
	size_t size)
{
	char folder[FM_PATH_MAX - 32];
	const char *data;
	const char *home;
	size_t index;

	/* $XDG_DATA_HOME, or ~/.local/share. */
	data = getenv("XDG_DATA_HOME");
	home = getenv("HOME");
	if (home == NULL)
		home = "";
	if (data != NULL && data[0] == '/')
		snprintf(folder, sizeof(folder), "%s/files", data);
	else
		snprintf(folder, sizeof(folder), "%s/.local/share/files", home);

	/* The folder and those above it, then the list in it. */
	for (index = 1; folder[index] != '\0'; index++) {
		if (folder[index] != '/')
			continue;
		folder[index] = '\0';
		(void)mkdir(folder, 0700);
		folder[index] = '/';
	}

	/* The folder itself. */
	(void)mkdir(folder, 0700);
	snprintf(path, size, "%s/recent-folders", folder);
}
