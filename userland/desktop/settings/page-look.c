/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pages of the desktop's look and of the machine's screen and disks
 * (ws089-p004):
 *
 *   Appearance  light or dark (ws089-p017), a switch; the accent colour
 *               (ws179-p001), eight swatches; the windows'
 *               contents' opacity, a slider saved when let go, and
 *               whether their glass panels are frosted, a switch (BUG-214);
 *   Wallpaper   the pictures, the default first, one click to choose;
 *   Display     the screen's mode, read only (changing it comes later);
 *   Storage     each file system's use.
 *
 * What is chosen goes into the desktop's settings (look.c), which the compositor
 * puts into effect at once.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

/* The controls of the look's pages (hit indices); a picture is its index past LOOK_PICTURE_FIRST. */
#define LOOK_OPACITY		1
#define LOOK_DARK		2
#define LOOK_FROSTED		3
#define LOOK_ACCENT_FIRST	90
#define LOOK_PICTURE_FIRST	100

/* The space between two cards, a card's inner margin, and the text sizes. */
#define LOOK_GAP		16
#define LOOK_PAD		18
#define LOOK_TEXT_TITLE		15U
#define LOOK_TEXT_SMALL		13U

/* The opacity's range, in percent. */
#define LOOK_OPACITY_MIN	85
#define LOOK_OPACITY_MAX	100

/* A picture's tile: its narrowest width, the picture's proportions, and the room of its name. */
#define LOOK_TILE_WIDTH		200
#define LOOK_TILE_GAP		16
#define LOOK_TILE_NAME		30

/* A file system's bar. */
#define LOOK_BAR_HEIGHT		10

/* The card of the appearance: its height, and the switch's width and height. */
#define LOOK_DARK_HEIGHT	76

/* The card of the accent: its height, a swatch's diameter and the room between two. */
#define LOOK_ACCENT_HEIGHT	112
#define LOOK_SWATCH		26
#define LOOK_SWATCH_GAP		14

/* How far out of a swatch the keyboard's ring is (the chosen ring is 4 out; LOOK_SWATCH_GAP leaves room). */
#define LOOK_FOCUS_RING		7.0f

/* The frosted glass's row in the card of the windows (BUG-214). */
#define LOOK_FROSTED_HEIGHT	80
#define LOOK_SWITCH_WIDTH	44
#define LOOK_SWITCH_HEIGHT	24

static int look_note(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, const char *text);
static int look_message(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void look_tile(struct se_app *app, struct kl_canvas *canvas, unsigned index, int x, int y, int width, int height);
static int look_volume(struct se_app *app, struct kl_canvas *canvas, const struct se_volume *volume, int x, int top, int width);
static int look_accents(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, int enabled);

/*
 * The accents' names, in the order of KL_ACCENT_*.  The table is constant
 * for the life of the program (each name reaches kl_tr through it,
 * locale/settings.keys).
 */
static const char *const look_accent_names[KL_ACCENTS] = {
	"Blue",
	"Purple",
	"Pink",
	"Red",
	"Orange",
	"Yellow",
	"Green",
	"Graphite"
};

/*
 * Draws the Appearance page: light or dark, and the windows'
 * transparency.  Returns the edge below it.
 */
int
se_appearance_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_text_line line;
	char value[32];
	float fraction;
	int enabled;
	int height;
	int card;
	int y;
	int value_width;

	/* A message about saving first, when there is one. */
	card = look_message(app, canvas, x, top, width);

	/* The card of the appearance: dark or not, a switch that works only when the settings can be changed (ws089-p017). */
	enabled = 0;
	if (app->look.writable)
		enabled = 1;
	y = se_card_begin(app, canvas, x, card, width, LOOK_DARK_HEIGHT, NULL, NULL);
	kl_text_metrics(app->text, LOOK_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, y + line.ascent, "Dark appearance", LOOK_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	kl_text_metrics(app->text, LOOK_TEXT_SMALL, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, y + 24 + line.ascent, "Dark windows and desktop, easier on the eyes at night.", LOOK_TEXT_SMALL, 0, width - 2 * LOOK_PAD - LOOK_SWITCH_WIDTH - 16, SE_COLOR_TEXT_SECONDARY);
	se_toggle_draw(app, canvas, x + width - LOOK_PAD - LOOK_SWITCH_WIDTH, card + (LOOK_DARK_HEIGHT - LOOK_SWITCH_HEIGHT) / 2, app->look.dark, enabled, LOOK_DARK);
	card += LOOK_DARK_HEIGHT + LOOK_GAP;

	/* The card of the accent colour (ws179-p001). */
	card = look_accents(app, canvas, x, card, width, enabled);

	/* The card of the windows: a title, a line, the slider with its value, and the frosted glass's switch (BUG-214). */
	height = 150 + LOOK_FROSTED_HEIGHT;
	y = se_card_begin(app, canvas, x, card, width, height, "Windows", "How much of the desktop shows through the windows.");
	kl_text_metrics(app->text, LOOK_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, y + line.ascent, "Window opacity", LOOK_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);

	/* The value at the right, as the slider shows it (the contents' opacity; the panels are the switch's). */
	(void)snprintf(value, sizeof(value), "%d%%", app->look.opacity);

	/* Drawn against the card's right margin. */
	value_width = kl_text_width(app->text, value, strlen(value), LOOK_TEXT_TITLE, 0);
	(void)kl_text_draw(app->text, canvas, x + width - LOOK_PAD - value_width, y + line.ascent, value, strlen(value), LOOK_TEXT_TITLE, 0, SE_COLOR_TEXT_SECONDARY);

	/* The slider: see-through at the left, opaque at the right; it works only when the settings can be changed. */
	fraction = (float)(app->look.opacity - LOOK_OPACITY_MIN) / (float)(LOOK_OPACITY_MAX - LOOK_OPACITY_MIN);
	se_slider_draw(app, canvas, x + LOOK_PAD + 14, y + 26, width - 2 * LOOK_PAD - 28, fraction, enabled, LOOK_OPACITY, &app->look.slider);

	/* The ends' words under the slider. */
	kl_text_metrics(app->text, LOOK_TEXT_SMALL, &line);
	(void)kl_text_draw(app->text, canvas, x + LOOK_PAD + 2, y + 66 + line.ascent, "See-through", strlen("See-through"), LOOK_TEXT_SMALL, 0, SE_COLOR_TEXT_FAINT);
	value_width = kl_text_width(app->text, "Opaque", strlen("Opaque"), LOOK_TEXT_SMALL, 0);
	(void)kl_text_draw(app->text, canvas, x + width - LOOK_PAD - value_width, y + 66 + line.ascent, "Opaque", strlen("Opaque"), LOOK_TEXT_SMALL, 0, SE_COLOR_TEXT_FAINT);

	/* The frosted glass: its name, a line, and the switch (on: the panels show the desktop blurred; off: solid). */
	y += 100;
	kl_text_metrics(app->text, LOOK_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, y + line.ascent, "Frosted glass", LOOK_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	kl_text_metrics(app->text, LOOK_TEXT_SMALL, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, y + 24 + line.ascent, "The windows' panels show the desktop blurred behind them. Off makes them solid.", LOOK_TEXT_SMALL, 0, width - 2 * LOOK_PAD - LOOK_SWITCH_WIDTH - 16, SE_COLOR_TEXT_SECONDARY);
	se_toggle_draw(app, canvas, x + width - LOOK_PAD - LOOK_SWITCH_WIDTH, y + 8, app->look.frosted, enabled, LOOK_FROSTED);

	/* The edge below the cards. */
	return card + height;
}

/*
 * Draws the Wallpaper page: the pictures as tiles, the one shown ringed in
 * the accent.  Returns the edge below it.
 */
int
se_wallpaper_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	unsigned index;
	int columns;
	int column;
	int tile;
	int picture;
	int y;

	/* The pictures, found the first time the page is shown. */
	se_look_scan(app);

	/* A message about saving first, when there is one. */
	y = look_message(app, canvas, x, top, width);

	/* How many tiles a row holds, their width, and the pictures' height (16:10). */
	columns = (width + LOOK_TILE_GAP) / (LOOK_TILE_WIDTH + LOOK_TILE_GAP);
	if (columns < 1)
		columns = 1;
	tile = (width - (columns - 1) * LOOK_TILE_GAP) / columns;
	picture = tile * 10 / 16;

	/* Each picture, a new row when one fills. */
	column = 0;
	for (index = 0; index < app->look.wallpaper_count; index++) {
		/* A full row moves down one. */
		if (column == columns) {
			column = 0;
			y += picture + LOOK_TILE_NAME + LOOK_TILE_GAP;
		}

		/* The tile in the next column. */
		look_tile(app, canvas, index, x + column * (tile + LOOK_TILE_GAP), y, tile, picture);
		column++;
	}

	/* The edge below the last row. */
	return y + picture + LOOK_TILE_NAME;
}

/*
 * Draws the Storage page: each file system's use, read now.  Returns the
 * edge below it.
 */
int
se_storage_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	unsigned index;
	int offered;
	int known;
	int y;

	/* The file systems as the desktop last read them, read again while the page shows them (ws188-p002). */
	se_look_volumes(app);

	/* Not read yet: said while the first reading is under way (a desktop that reads none says none can be read). */
	known = se_machine_known(app, KL_MACHINE_FILESYSTEMS);
	offered = se_machine_offered(app);
	if (!known && offered) {
		y = look_note(app, canvas, x, top, width, "Reading the disks...");
		y = se_storage_cards(app, canvas, x, y + LOOK_GAP, width);
		return y;
	}

	/* None can be read. */
	if (app->look.volume_count == 0U) {
		y = look_note(app, canvas, x, top, width, "No disk could be read.");
		y = se_storage_cards(app, canvas, x, y + LOOK_GAP, width);
		return y;
	}

	/* A card each. */
	y = top;
	for (index = 0; index < app->look.volume_count; index++)
		y = look_volume(app, canvas, &app->look.volumes[index], x, y, width) + LOOK_GAP;

	/* The folders' use and the Trash (ws089-p023), and the edge below them. */
	y = se_storage_cards(app, canvas, x, y, width);
	return y;
}

/*
 * Carries out a click on a control of the look's pages: the appearance's
 * switch turns, or a picture is chosen.
 */
void
se_look_press(
	struct se_app *app,
	int index)
{
	/* The switch turns, and is saved; the compositor tells every program, this one too (window.c). */
	if (index == LOOK_DARK) {
		app->look.dark = !app->look.dark;
		se_look_set_number(app, "appearance.dark", app->look.dark, 0);
		app->dirty = 1;
		return;
	}

	/* The frosted glass's switch turns, and is saved; the compositor makes the panels solid or frosted (BUG-214). */
	if (index == LOOK_FROSTED) {
		app->look.frosted = !app->look.frosted;
		se_look_set_number(app, "window.frosted", app->look.frosted, 1);
		app->dirty = 1;
		return;
	}

	/* An accent's swatch: chosen, and saved; the compositor tells every program, this one too. */
	if (index >= LOOK_ACCENT_FIRST && index < LOOK_ACCENT_FIRST + (int)KL_ACCENTS) {
		app->look.accent = index - LOOK_ACCENT_FIRST;
		se_look_set_number(app, "appearance.accent", app->look.accent, 0);
		se_log("ACCENT index=%d", app->look.accent);
		app->dirty = 1;
		return;
	}

	/* A picture's tile. */
	if (index >= LOOK_PICTURE_FIRST)
		se_look_set_wallpaper(app, index - LOOK_PICTURE_FIRST);
}

/*
 * Takes a key on the Appearance page (ws177-p004): Tab gives the keyboard
 * to the accent's swatches (the chosen one first) and takes it back, Left
 * and Right move it along them, Space and Enter choose the one that has
 * it, Esc takes it back.  Returns 1 when the key was the page's.
 */
int
se_look_key(
	struct se_app *app,
	const struct se_event *event)
{
	int focus;

	/* Settings that cannot be changed have no swatch to choose. */
	if (!app->look.writable)
		return 0;

	/* Tab: into the swatches at the chosen one, or out of them. */
	if (event->key == SE_KEY_TAB) {
		if (app->look.accent_focus == 0) {
			focus = app->look.accent;
			if (focus < 0 || focus >= (int)KL_ACCENTS)
				focus = 0;
			app->look.accent_focus = focus + 1;
		} else {
			app->look.accent_focus = 0;
		}

		/* Logged for the tests (-1: none has it). */
		se_log("ACCENT focus=%d", app->look.accent_focus - 1);
		return 1;
	}

	/* The other keys are the swatches' only while one has the keyboard. */
	if (app->look.accent_focus == 0)
		return 0;
	focus = app->look.accent_focus - 1;

	/* Each key the swatches take. */
	switch (event->key) {
	case SE_KEY_LEFT:
		/* The one before, the first staying. */
		if (focus > 0)
			focus--;
		app->look.accent_focus = focus + 1;
		se_log("ACCENT focus=%d", focus);
		return 1;
	case SE_KEY_RIGHT:
		/* The one after, the last staying. */
		if (focus + 1 < (int)KL_ACCENTS)
			focus++;
		app->look.accent_focus = focus + 1;
		se_log("ACCENT focus=%d", focus);
		return 1;
	case SE_KEY_SPACE:
	case SE_KEY_ENTER:
		/* Chosen, as a click chooses it. */
		se_look_press(app, LOOK_ACCENT_FIRST + focus);
		return 1;
	case SE_KEY_ESC:
		/* The keyboard back to the page. */
		app->look.accent_focus = 0;
		se_log("ACCENT focus=-1");
		return 1;
	default:
		break;
	}

	/* Succeeded: another key is not the swatches'. */
	return 0;
}

/*
 * Follows a drag on the opacity slider: the value moves with the pointer,
 * and is saved when the button is let go.
 */
void
se_look_drag(
	struct se_app *app,
	int index,
	int x,
	unsigned phase)
{
	float fraction;
	int percent;

	/* Only the slider is dragged. */
	if (index != LOOK_OPACITY)
		return;

	/* The value under the pointer, in whole percent. */
	fraction = se_slider_fraction(&app->look.slider, x);
	percent = LOOK_OPACITY_MIN + (int)(fraction * (float)(LOOK_OPACITY_MAX - LOOK_OPACITY_MIN) + 0.5f);

	/* While held the page shows it; the release saves it. */
	app->look.opacity = percent;
	app->dirty = 1;
	app->look.dragging = 1;
	if (phase != SE_DRAG_END)
		return;

	/* Let go: saved, and the compositor follows. */
	app->look.dragging = 0;
	se_look_set_opacity(app, percent);
}

/* Draws a quiet card with one line; returns the edge below it. */
static int
look_note(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	const char *text)
{
	int height;
	int baseline;

	/* One line in a low card. */
	height = 56;
	(void)se_card_begin(app, canvas, x, top, width, height, NULL, NULL);
	baseline = kl_text_center(LOOK_TEXT_SMALL, top, height);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, baseline, text, LOOK_TEXT_SMALL, 0, width - 2 * LOOK_PAD, SE_COLOR_TEXT_SECONDARY);

	/* The edge below the card. */
	return top + height;
}

/* Draws the line about saving (a failure in red), when there is one; returns the edge below it. */
static int
look_message(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const char *text;
	kl_color ink;

	/* The message kept, or the lack of a home. */
	text = app->look.message;
	if (text[0] == '\0' && !app->look.writable)
		text = "These settings cannot be changed on this desktop.";
	if (text[0] == '\0')
		return top;

	/* One line, red for a failure. */
	ink = SE_COLOR_TEXT_SECONDARY;
	if (app->look.message_bad != 0 || !app->look.writable)
		ink = SE_COLOR_BAD;
	(void)kl_text_draw_fit(app->text, canvas, x + 2, top + 16, text, LOOK_TEXT_SMALL, 0, width, ink);

	/* The edge below the line. */
	return top + 30;
}

/* Draws one picture's tile: the picture (or the default's drawn stand-in), its name, a ring when it is shown. */
static void
look_tile(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned index,
	int x,
	int y,
	int width,
	int height)
{
	const struct se_wallpaper *wallpaper;
	struct kl_rect rect;
	char name[80];
	int chosen;
	int lit;
	int differs;

	/* Whether this is the picture shown: the default when no key is set, else the one whose path is the key. */
	wallpaper = &app->look.wallpapers[index];
	chosen = 0;
	if (index == 0U &&
	    app->look.has_default != 0 &&
	    app->look.wallpaper[0] == '\0')
		chosen = 1;
	if (app->look.wallpaper[0] != '\0') {
		differs = strcmp(app->look.wallpaper, wallpaper->path);
		if (differs == 0)
			chosen = 1;
	}

	/*
	 * The picture; while its small copy is still being read a plain grey
	 * stand-in (BUG-152); for one that could not be read a quiet gradient
	 * with the Kei mark.
	 */
	if (wallpaper->read != 0) {
		kl_canvas_image(canvas, &wallpaper->thumbnail, (float)x, (float)y, (float)width, (float)height, 10.0f, 1.0f);
	} else if (wallpaper->pending != 0) {
		kl_canvas_round_gradient(canvas, (float)x, (float)y, (float)width, (float)height, 10.0f, KL_RGB(0xe9edf3), KL_RGB(0xdde3ec));
	} else {
		kl_canvas_round_gradient(canvas, (float)x, (float)y, (float)width, (float)height, 10.0f, KL_RGB(0xdfeaf7), KL_RGB(0xc8dcc4));
		se_mark_draw(canvas, x + width / 2 - 24, y + height / 2 - 24, 48U, 0.9f);
	}

	/* The ring: the accent round the picture shown, a shade under the pointer. */
	lit = se_ui_lit(app, SE_HIT_CONTROL, LOOK_PICTURE_FIRST + (int)index);
	if (chosen != 0) {
		kl_canvas_round_border(canvas, (float)x - 3.0f, (float)y - 3.0f, (float)width + 6.0f, (float)height + 6.0f, 13.0f, 3.0f, SE_COLOR_ACCENT);
	} else if (lit != 0) {
		kl_canvas_round_border(canvas, (float)x - 2.0f, (float)y - 2.0f, (float)width + 4.0f, (float)height + 4.0f, 12.0f, 2.0f, KL_RGBA(0x5a6b85, 90));
	} else {
		kl_canvas_round_border(canvas, (float)x, (float)y, (float)width, (float)height, 10.0f, 1.0f, SE_COLOR_CARD_EDGE);
	}

	/* The name under it; the default says so. */
	(void)snprintf(name, sizeof(name), "%s", wallpaper->name);
	if (index == 0U && app->look.has_default != 0)
		(void)snprintf(name, sizeof(name), "%s (default)", wallpaper->name);
	(void)kl_text_draw_fit(app->text, canvas, x + 2, y + height + 20, name, LOOK_TEXT_SMALL, chosen, width - 4, SE_COLOR_TEXT);

	/* A click chooses it. */
	rect.x = x;
	rect.y = y;
	rect.width = width;
	rect.height = height + LOOK_TILE_NAME;
	se_ui_hit(app, &rect, SE_HIT_CONTROL, LOOK_PICTURE_FIRST + (int)index);
}

/* Draws one file system's card: where it is, a bar of its use, the bytes used and free. Returns the edge below it. */
static int
look_volume(
	struct se_app *app,
	struct kl_canvas *canvas,
	const struct se_volume *volume,
	int x,
	int top,
	int width)
{
	char used[32];
	char total[32];
	char available[32];
	char line[128];
	char title[80];
	float share;
	int height;
	int y;
	int bar;

	/* The card, named by where the file system is. */
	height = 118;
	(void)snprintf(title, sizeof(title), "Disk at %s", volume->path);
	y = se_card_begin(app, canvas, x, top, width, height, title, NULL);

	/* The bar: the share used in the accent, red when nearly full. */
	share = 0.0f;
	if (volume->total != 0U)
		share = (float)((double)volume->used / (double)volume->total);
	bar = width - 2 * LOOK_PAD;
	kl_canvas_round(canvas, (float)(x + LOOK_PAD), (float)y, (float)bar, (float)LOOK_BAR_HEIGHT, 5.0f, SE_COLOR_RAIL);
	if (share > 0.9f) {
		kl_canvas_round(canvas, (float)(x + LOOK_PAD), (float)y, (float)bar * share, (float)LOOK_BAR_HEIGHT, 5.0f, SE_COLOR_BAD);
	} else {
		kl_canvas_round(canvas, (float)(x + LOOK_PAD), (float)y, (float)bar * share, (float)LOOK_BAR_HEIGHT, 5.0f, SE_COLOR_ACCENT);
	}

	/* The bytes used of the whole, and what is left. */
	se_bytes_text(volume->used, used, sizeof(used));
	se_bytes_text(volume->total, total, sizeof(total));
	se_bytes_text(volume->available, available, sizeof(available));
	(void)snprintf(line, sizeof(line), "%s used of %s  \xc2\xb7  %s available", used, total, available);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, y + LOOK_BAR_HEIGHT + 26, line, LOOK_TEXT_SMALL, 0, bar, SE_COLOR_TEXT_SECONDARY);

	/* The edge below the card. */
	return top + height;
}

/*
 * Draws the card of the accent colour from a top edge: a title, the eight
 * swatches in the appearance's colours (the chosen one ringed with a dot of
 * its ink), and the chosen one's name; a swatch works only when the
 * settings can be changed.  Returns the edge below the card and its gap.
 */
static int
look_accents(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	int enabled)
{
	struct kl_text_line line;
	struct kl_accent values;
	struct kl_rect box;
	unsigned appearance;
	unsigned index;
	float cx;
	float cy;
	float radius;
	int chosen;
	int y;

	/* The card and its title. */
	y = se_card_begin(app, canvas, x, top, width, LOOK_ACCENT_HEIGHT, NULL, NULL);
	kl_text_metrics(app->text, LOOK_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, y + line.ascent, kl_tr("Accent color"), LOOK_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);

	/* The chosen accent (one out of the table is blue), in the appearance the page is drawn in. */
	chosen = app->look.accent;
	if (chosen < 0 || chosen >= (int)KL_ACCENTS)
		chosen = 0;
	appearance = KL_APPEARANCE_LIGHT;
	if (app->look.dark)
		appearance = KL_APPEARANCE_DARK;

	/*
	 * Each swatch on one row under the title: the keyboard's ring in the
	 * accent round the one that has it (ws177-p004), its disc, a ring and
	 * its ink's dot when chosen, and its click.
	 */
	radius = (float)LOOK_SWATCH * 0.5f;
	cy = (float)(y + 32) + radius;
	for (index = 0; index < KL_ACCENTS; index++) {
		cx = (float)(x + LOOK_PAD + 2) + radius + (float)(index * (LOOK_SWATCH + LOOK_SWATCH_GAP));
		kl_accent_values(index, appearance, &values);
		if ((int)index + 1 == app->look.accent_focus) {
			kl_canvas_circle(canvas, cx, cy, radius + LOOK_FOCUS_RING, SE_COLOR_ACCENT);
			kl_canvas_circle(canvas, cx, cy, radius + LOOK_FOCUS_RING - 2.0f, SE_COLOR_CARD);
		}

		/* The chosen one's ring. */
		if ((int)index == chosen) {
			kl_canvas_circle(canvas, cx, cy, radius + 4.0f, SE_COLOR_TEXT_SECONDARY);
			kl_canvas_circle(canvas, cx, cy, radius + 2.0f, SE_COLOR_CARD);
		}
		kl_canvas_circle(canvas, cx, cy, radius, values.accent);
		if ((int)index == chosen)
			kl_canvas_circle(canvas, cx, cy, 4.0f, values.ink);

		/* A click on it chooses it. */
		box.x = (int)(cx - radius) - 4;
		box.y = (int)(cy - radius) - 4;
		box.width = LOOK_SWATCH + 8;
		box.height = LOOK_SWATCH + 8;
		if (enabled)
			se_ui_hit(app, &box, SE_HIT_CONTROL, LOOK_ACCENT_FIRST + (int)index);
	}

	/* The chosen one's name under the row. */
	kl_text_metrics(app->text, LOOK_TEXT_SMALL, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + LOOK_PAD + 2, y + 32 + LOOK_SWATCH + 12 + line.ascent, kl_tr(look_accent_names[chosen]), LOOK_TEXT_SMALL, 0, width - 2 * LOOK_PAD, SE_COLOR_TEXT_SECONDARY);

	/* The edge below the card, and the gap to the next. */
	return top + LOOK_ACCENT_HEIGHT + LOOK_GAP;
}
