/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Home page: every page as a tile, by group, so that a page can be
 * found by its picture.  The titlebar's Home control and the breadcrumb's
 * first part open it.  A page that works shows its state now on its tile
 * ("Connected · ue0", ws089-p008), with a dot when the state is a
 * connection; the others show their summary.
 */

#include "settings.h"

#include "include/kern/os-name.h"

#include <stdio.h>
#include <string.h>

/* A tile's narrowest width (tiles widen to fill a row), its height and the space between two, and the texts' sizes. */
#define HOME_TILE_WIDTH		200
#define HOME_TILE_HEIGHT	92
#define HOME_TILE_GAP		14
#define HOME_TEXT_GROUP		13U
#define HOME_TEXT_NAME		15U
#define HOME_TEXT_SUMMARY	12U

/* The space above a group's title, and between the title and its tiles. */
#define HOME_GROUP_GAP		22
#define HOME_GROUP_TITLE	26

/* The bytes of a tile's state line with its NUL. */
#define HOME_STATE		96

static int home_group(struct se_app *app, struct kl_canvas *canvas, unsigned group, const char *title, int x, int top, int width);
static void home_tile(struct se_app *app, struct kl_canvas *canvas, const struct se_page *page, int x, int y, int width);
static int home_state(const struct se_app *app, unsigned page, char *text, size_t size, int *dot);
static int home_wifi_state(const struct kl_network_state *state, char *text, size_t size, int *dot);
static const char *home_address(const struct se_network *network, const char *name);

/*
 * Draws the Home page's groups of tiles from a top edge; returns the edge
 * below them.
 */
int
se_home_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int y;

	/* The disks' use now, for Storage's tile. */
	se_look_volumes(app);

	/* Each group in the list's order. */
	y = home_group(app, canvas, SE_GROUP_CONNECTIVITY, kl_tr("Connectivity"), x, top, width);
	y = home_group(app, canvas, SE_GROUP_PERSONALIZATION, kl_tr("Personalization"), x, y + HOME_GROUP_GAP, width);
	y = home_group(app, canvas, SE_GROUP_DEVICES, kl_tr("Devices"), x, y + HOME_GROUP_GAP, width);
	y = home_group(app, canvas, SE_GROUP_SYSTEM, kl_tr("System"), x, y + HOME_GROUP_GAP, width);

	/* The edge below the last group. */
	return y;
}

/* Draws a group's title and its tiles in rows as wide as the column allows; returns the edge below them. */
static int
home_group(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned group,
	const char *title,
	int x,
	int top,
	int width)
{
	struct kl_text_line line;
	unsigned id;
	int columns;
	int column;
	int tile;
	int y;

	/* The title, quiet and bold. */
	kl_text_metrics(app->text, HOME_TEXT_GROUP, &line);
	(void)kl_text_draw(app->text, canvas, x + 2, top + line.ascent, title, strlen(title), HOME_TEXT_GROUP, 1, SE_COLOR_TEXT_SECONDARY);

	/* How many tiles a row holds (at least one), and their width to fill the row. */
	columns = (width + HOME_TILE_GAP) / (HOME_TILE_WIDTH + HOME_TILE_GAP);
	if (columns < 1)
		columns = 1;
	tile = (width - (columns - 1) * HOME_TILE_GAP) / columns;

	/* Each page of the group, a new row when one fills. */
	column = 0;
	y = top + HOME_GROUP_TITLE;
	for (id = SE_PAGE_HOME + 1; id < SE_PAGES; id++) {
		if (se_pages[id].group != group)
			continue;

		/* A full row moves down one. */
		if (column == columns) {
			column = 0;
			y += HOME_TILE_HEIGHT + HOME_TILE_GAP;
		}

		/* The tile in the next column. */
		home_tile(app, canvas, &se_pages[id], x + column * (tile + HOME_TILE_GAP), y, tile);
		column++;
	}

	/* The edge below the last row. */
	return y + HOME_TILE_HEIGHT;
}

/* Draws one page's tile: its picture, name and summary, lit under the pointer. */
static void
home_tile(
	struct se_app *app,
	struct kl_canvas *canvas,
	const struct se_page *page,
	int x,
	int y,
	int width)
{
	struct kl_rect tile;
	char state[HOME_STATE];
	char summary_text[SE_SUMMARY_TEXT];
	const char *line;
	kl_color ground;
	kl_color glyph;
	kl_color summary;
	int live;
	int dot;
	int left;
	int lit;

	/* The tile, a little darker under the pointer. */
	tile.x = x;
	tile.y = y;
	tile.width = width;
	tile.height = HOME_TILE_HEIGHT;
	lit = se_ui_lit(app, SE_HIT_TILE, (int)page->id);
	ground = SE_COLOR_TILE;
	if (lit != 0)
		ground = kl_color_mix(SE_COLOR_TILE, SE_COLOR_TILE_HOVER, 0.8f);
	kl_canvas_round(canvas, (float)x, (float)y, (float)tile.width, (float)tile.height, 14.0f, ground);
	kl_canvas_round_border(canvas, (float)x, (float)y, (float)tile.width, (float)tile.height, 14.0f, 1.0f, SE_COLOR_CARD_EDGE);

	/* The picture in the accent for a page that works, faint for one that is coming. */
	glyph = SE_COLOR_ACCENT;
	summary = SE_COLOR_TEXT_SECONDARY;
	if (page->ready == 0) {
		glyph = SE_COLOR_ICON;
		summary = SE_COLOR_TEXT_FAINT;
	}

	/* The picture at the tile's upper left. */
	se_glyph_draw(canvas, page->glyph, (float)x + 16.0f, (float)y + 14.0f, 26.0f, glyph);

	/* The name. */
	(void)kl_text_draw_fit(app->text, canvas, x + 16, y + 62, kl_tr(page->name), HOME_TEXT_NAME, 1, tile.width - 28, SE_COLOR_TEXT);

	/* Under it the page's state now, when it has one, else its summary. */
	line = se_named(summary_text, sizeof(summary_text), kl_tr(page->summary));
	dot = 0;
	live = home_state(app, page->id, state, sizeof(state), &dot);
	if (live != 0)
		line = state;

	/* A connection's state starts with a green or grey dot. */
	left = x + 16;
	if (dot > 0) {
		se_dot_draw(canvas, (float)left + 4.0f, (float)y + 76.0f, SE_COLOR_GOOD);
		left += 14;
	} else if (dot < 0) {
		se_dot_draw(canvas, (float)left + 4.0f, (float)y + 76.0f, SE_COLOR_TEXT_FAINT);
		left += 14;
	}

	/* The line itself, after the dot. */
	(void)kl_text_draw_fit(app->text, canvas, left, y + 80, line, HOME_TEXT_SUMMARY, 0, tile.width - 12 - (left - x), summary);

	/* A click opens the page. */
	se_ui_hit(app, &tile, SE_HIT_TILE, (int)page->id);
}

/*
 * Says a page's state now for its tile: the network's pages from what the
 * daemon reported, About from the machine.  Returns 1 with the text (and
 * dot 1 for a connection, -1 for none, 0 when no dot fits), or 0 for a
 * page that shows its summary.
 */
static int
home_state(
	const struct se_app *app,
	unsigned page,
	char *text,
	size_t size,
	int *dot)
{
	const struct kl_network_state *state;
	const char *address;
	char free_text[32];
	int running;
	int available;
	int live;

	/* No dot unless a connection is described. */
	state = &app->network.state;
	*dot = 0;

	/* Each page that has a state. */
	switch (page) {
	case SE_PAGE_WIFI:
		live = home_wifi_state(state, text, size, dot);
		return live;
	case SE_PAGE_ETHERNET:
		/* The wired interface in use, or none. */
		if (state->reachable != 0 && state->wired[0] != '\0') {
			(void)snprintf(text, size, "Connected \xc2\xb7 %s", state->wired);
			*dot = 1;
		} else {
			(void)snprintf(text, size, "%s", "Not connected");
			*dot = -1;
		}

		/* The state is in the text. */
		return 1;
	case SE_PAGE_NETWORK:
		/* Online with the address in use, or offline. */
		address = home_address(&app->network, state->interface);
		if (state->reachable != 0 &&
		    state->connected != 0 &&
		    address != NULL) {
			(void)snprintf(text, size, "Online \xc2\xb7 %s", address);
			*dot = 1;
		} else if (state->reachable != 0 && state->connected != 0) {
			(void)snprintf(text, size, "Online \xc2\xb7 %s", state->interface);
			*dot = 1;
		} else {
			(void)snprintf(text, size, "%s", "Offline");
			*dot = -1;
		}

		/* The state is in the text. */
		return 1;
	case SE_PAGE_APPEARANCE:
		/* The windows' opacity. */
		if (app->look.opacity >= 100) {
			(void)snprintf(text, size, "%s", "Opaque windows");
		} else {
			(void)snprintf(text, size, "Window opacity %d%%", app->look.opacity);
		}

		/* The state is in the text. */
		return 1;
	case SE_PAGE_WALLPAPER:
		/* The picture's name. */
		(void)snprintf(text, size, "%s", se_look_wallpaper_name(app));
		return 1;
	case SE_PAGE_DISPLAY:
		/* The screen's mode, when known. */
		if (app->about.display[0] == '\0')
			return 0;
		(void)snprintf(text, size, "%s", app->about.display);
		return 1;
	case SE_PAGE_STORAGE:
		/* What is left on the first disk. */
		if (app->look.volume_count == 0U)
			return 0;
		se_bytes_text(app->look.volumes[0].available, free_text, sizeof(free_text));
		(void)snprintf(text, size, "%s available", free_text);
		return 1;
	case SE_PAGE_SOUND:
		/*
		 * Whether the sound service runs, and then what the Sound page shows of
		 * it in the same words: no output, or the volume (ws089-p012 C4).
		 */
		running = se_sound_running(app);
		available = se_sound_available(app);
		if (!app->sound.live) {
			(void)snprintf(text, size, "%s", "Not available on this desktop");
		} else if (running == 0) {
			(void)snprintf(text, size, "%s", "No sound service");
		} else if (app->sound.state.reachable != 0 && app->sound.state.device == 0) {
			(void)snprintf(text, size, "%s", "Running, no sound output");
		} else if (available != 0 && app->sound.muted != 0) {
			(void)snprintf(text, size, "%s", "Muted");
		} else if (available != 0) {
			(void)snprintf(text, size, "Volume %d%%", app->sound.value);
		} else {
			(void)snprintf(text, size, "%s", "Sound service running");
		}

		/* The state is in the text. */
		return 1;
	case SE_PAGE_MOUSE:
		/* A mouse's speed, and natural scrolling when on. */
		if (app->look.mouse_natural != 0) {
			(void)snprintf(text, size, "Speed %d%% \xc2\xb7 natural scrolling", app->look.mouse_speed);
		} else {
			(void)snprintf(text, size, "Speed %d%%", app->look.mouse_speed);
		}

		/* The state is in the text. */
		return 1;
	case SE_PAGE_TOUCHPAD:
		/* The touch pads' speed, and natural scrolling when on (ws089-p024). */
		if (app->look.touchpad_natural != 0) {
			(void)snprintf(text, size, "Speed %d%% \xc2\xb7 natural scrolling", app->look.touchpad_speed);
		} else {
			(void)snprintf(text, size, "Speed %d%%", app->look.touchpad_speed);
		}

		/* The state is in the text. */
		return 1;
	case SE_PAGE_KEYBOARD:
		/* The repeat. */
		(void)snprintf(text, size, "Repeat %d a second after %d ms", app->look.repeat_rate, app->look.repeat_delay);
		return 1;
	case SE_PAGE_ABOUT:
		/* The name of the system and the machine, when known. */
		if (app->about.machine[0] == '\0')
			return 0;
		(void)snprintf(text, size, ZEDBSD_OS_NAME " \xc2\xb7 %s", app->about.machine);
		return 1;
	default:
		break;
	}

	/* The page shows its summary. */
	return 0;
}

/* Says the Wi-Fi's state for its tile; returns 1 (the Wi-Fi always has a state). */
static int
home_wifi_state(
	const struct kl_network_state *state,
	char *text,
	size_t size,
	int *dot)
{
	/* The daemon out of reach. */
	if (state->reachable == 0) {
		(void)snprintf(text, size, "%s", "Unavailable");
		return 1;
	}

	/* Each state of the radio. */
	switch (state->wifi) {
	case KL_WIFI_ABSENT:
		(void)snprintf(text, size, "%s", "No Wi-Fi radio");
		break;
	case KL_WIFI_OFF:
		(void)snprintf(text, size, "%s", "Off");
		*dot = -1;
		break;
	case KL_WIFI_SEARCHING:
		(void)snprintf(text, size, "%s", "Searching");
		*dot = -1;
		break;
	case KL_WIFI_CONNECTING:
		(void)snprintf(text, size, "Joining %s", state->ssid);
		*dot = -1;
		break;
	case KL_WIFI_CONNECTED:
		(void)snprintf(text, size, "Connected \xc2\xb7 %s", state->ssid);
		*dot = 1;
		break;
	default:
		(void)snprintf(text, size, "%s", "On, not connected");
		*dot = -1;
		break;
	}

	/* Succeeded: the state is in the text. */
	return 1;
}

/* Finds an interface's IPv4 address, or NULL when it has none or is not known. */
static const char *
home_address(
	const struct se_network *network,
	const char *name)
{
	size_t index;
	int differs;

	/* An empty name is no interface. */
	if (name[0] == '\0')
		return NULL;

	/* Each interface's name. */
	for (index = 0; index < network->link_count; index++) {
		differs = strcmp(network->links[index].name, name);
		if (differs != 0)
			continue;

		/* The interface without an address has none to show. */
		if (network->links[index].address[0] == '\0')
			return NULL;

		/* Succeeded: its address. */
		return network->links[index].address;
	}

	/* No interface has that name. */
	return NULL;
}
