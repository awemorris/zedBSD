/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * App Home (ws035-p069, plan/ws035/app-home-design.md): the applications'
 * home screen, under the desktop.
 *
 * Opening it does not darken the desktop: the desktop layer (the wallpaper
 * and the windows) slides towards the bottom right and shrinks a little,
 * until only its top-left corner and its shadow are left in the output's
 * bottom-right corner, and the bright home screen under it shows: the
 * blurred wallpaper, whitened, with a grid of application icons.  The
 * launcher in the system bar, or a drag from the top-left corner towards
 * the bottom right, opens it; the drag follows the pointer.  The launcher
 * again, the corner of the desktop that is left, Esc, or starting an
 * application closes it the opposite way.
 *
 * Typing while it is open searches: the text shows at the top and only the
 * applications whose name, command or keywords contain it stay, centred.
 * Enter starts the selected (at first the first) one; the arrow keys move
 * the selection; Backspace and Esc clear the search.
 *
 * The applications come from /etc/zdesktop/apps.conf, one a line:
 * name|command|keywords|RRGGBB; without the file, a built-in list.  An
 * application is started with /bin/sh -c and the compositor's socket in
 * its environment.
 */

#include "glass.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The applications' list, and how many it may hold. */
#define HOME_APPS_PATH		"/etc/zdesktop/apps.conf"
#define HOME_APPS_MAX		48U

/* How long opening and closing take, and the least move of the corner drag. */
#define HOME_OPEN_MS		280U
#define HOME_CLOSE_MS		240U
#define HOME_DRAG_START		14
#define HOME_DRAG_DISTANCE	360.0f
#define HOME_THRESHOLD		0.30f

/* The corner the gesture starts in, and how much of the desktop stays in view when Home is open. */
#define HOME_CORNER		28
#define HOME_KEEP		26.0f
#define HOME_KEEP_NEAR		40.0f
#define HOME_NEAR		120

/* The grid: columns, a cell's size, the icon's size and corner, the label's baseline under the icon. */
#define HOME_COLUMNS		6
#define HOME_CELL_WIDTH		144
#define HOME_CELL_HEIGHT	152
#define HOME_ICON		72
#define HOME_ICON_RADIUS	18.0f
#define HOME_LABEL		26

/* The evdev codes of the keys Home takes. */
#define HOME_KEY_ESC		1U
#define HOME_KEY_BACKSPACE	14U
#define HOME_KEY_ENTER		28U
#define HOME_KEY_KPENTER	96U
#define HOME_KEY_LEFT		105U
#define HOME_KEY_RIGHT		106U
#define HOME_KEY_UP		103U
#define HOME_KEY_DOWN		108U

/* How many evdev codes the character table covers (up to the space bar). */
#define HOME_KEYS		58U

/*
 * One application Home can start.
 */
struct home_app {
	/* The name under its icon, the command that starts it, and more words search finds it by. */
	char name[40];
	char command[160];
	char keywords[80];

	/* The icon's colour. */
	float color[4];
};

/*
 * The applications, read once when Home first opens.  home_app_count is 0
 * until then; the list is not read again while zwl runs.
 */
static struct home_app home_apps[HOME_APPS_MAX];
static unsigned home_app_count;
static unsigned home_apps_read;

/*
 * The applications shown now (all, or those the search finds), as indexes
 * into home_apps, and where each one's icon is.  Laid out again on every
 * frame and every change of the search.
 */
static unsigned home_shown[HOME_APPS_MAX];
static unsigned home_shown_count;
static int32_t home_icon_x[HOME_APPS_MAX];
static int32_t home_icon_y[HOME_APPS_MAX];

/*
 * The character each key types into the search (lower case), by evdev code; 0 for none.
 */
static const char home_characters[HOME_KEYS] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', 0, 0, 0,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 0, 0, 0, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0, 0, 0, 0, 0,
	'z', 'x', 'c', 'v', 'b', 'n', 'm', 0, '.', '/', 0, 0, 0, ' '
};

static void home_read_apps(void);
static void home_add_app(const char *name, const char *command, const char *keywords, uint32_t rgb);
static void home_parse_line(char *line);
static uint32_t home_hex(const char *text);
static void home_layout(struct zwl_server *server);
static int home_matches(const struct home_app *app, const char *query);
static int home_contains(const char *text, const char *query);
static int home_icon_at(struct zwl_server *server, int32_t x, int32_t y);
static void home_draw_icon(struct zwl_server *server, VkCommandBuffer command, unsigned slot, float opacity);
static void home_draw_search(struct zwl_server *server, VkCommandBuffer command, float opacity);
static void home_open(struct zwl_server *server, float from, const char *via);
static void home_close(struct zwl_server *server, float from, const char *via);
static void home_settle(struct zwl_server *server, float from, float to);
static void home_launch(struct zwl_server *server, unsigned app);
static void home_search_changed(struct zwl_server *server);
static void home_log_icons(void);
static float home_ease(float t);

/*
 * Returns how far Home is open now: 0 closed, 1 open, between while the
 * drag or the animation moves it.
 */
float
zwl_home_progress(
	struct zwl_server *server)
{
	uint64_t elapsed;
	uint64_t length;
	float t;

	/* The drag, once it is one, is followed as it is. */
	if (server->home_dragging)
		return server->home_drag;

	/* Settled: where it went. */
	if (!server->home_moving)
		return server->home;

	/* Moving: eased from where it started to where it goes, over the open or the close time. */
	length = HOME_CLOSE_MS;
	if (server->home_to > server->home_from)
		length = HOME_OPEN_MS;
	elapsed = zwl_milliseconds() - server->home_start_ms;
	t = (float)elapsed / (float)length;
	if (t > 1.0f)
		t = 1.0f;

	/* Reports the eased position. */
	return server->home_from + (server->home_to - server->home_from) * home_ease(t);
}

/*
 * Works out where the desktop layer is when Home is open by progress: slid
 * towards the bottom right and a little smaller, leaving HOME_KEEP pixels
 * (more when the pointer is near) of its corner in view.
 */
void
zwl_home_layer(
	struct zwl_server *server,
	float progress,
	float *x,
	float *y,
	float *scale)
{
	float keep;
	int32_t near_x;
	int32_t near_y;

	/* The corner left in view widens when the pointer comes near it, to show it can be taken. */
	keep = HOME_KEEP;
	near_x = (int32_t)server->width - server->pointer_x;
	near_y = (int32_t)server->height - server->pointer_y;
	if (progress >= 1.0f && near_x < HOME_NEAR && near_y < HOME_NEAR)
		keep = HOME_KEEP_NEAR;

	/* Sliding is the main motion; the shrinking only helps it. */
	*x = progress * ((float)server->width - keep);
	*y = progress * ((float)server->height - keep);
	*scale = 1.0f - 0.03f * progress;
}

/*
 * Draws Home, faded in by progress: the whitened blurred wallpaper and the
 * icons, and the search text while there is one.
 */
void
zwl_home_draw(
	struct zwl_server *server,
	VkCommandBuffer command,
	float progress)
{
	static const float tint[4] = { 0.86f, 0.92f, 1.0f, 0.22f };
	struct glass_shape shape;
	float color[4];
	unsigned slot;

	/* The applications and where they go. */
	home_read_apps();
	home_layout(server);

	/*
	 * The blurred wallpaper, much whiter, whole from the start: what the
	 * desktop uncovers is always bright (Home is never dark); only the
	 * icons fade in.
	 */
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_GLASS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.48f;
	glass_shape_draw(server, command, &shape);

	/* A faint blue over it, the colour of zedBSD. */
	memcpy(color, tint, sizeof(color));
	glass_draw_solid(server, command, 0.0f, 0.0f, (float)server->width, (float)server->height, 0.0f, color);

	/* Each icon shown, fading in with Home. */
	for (slot = 0U; slot < home_shown_count; slot++)
		home_draw_icon(server, command, slot, progress);

	/* The search text, while something has been typed. */
	if (server->home_query_length != 0U)
		home_draw_search(server, command, progress);
}

/*
 * Handles a pointer button for Home.  While Home is open (or opening) it
 * takes every button: the launcher and the desktop's corner close it, an
 * icon starts its application.  While it is closed, a press on the launcher
 * or in the top-left corner may open it: a click opens it, a drag towards
 * the bottom right follows the pointer.  Returns 1 when the button is Home's.
 */
int
zwl_home_button(
	struct zwl_server *server,
	uint32_t button,
	uint32_t state)
{
	float progress;
	int app;
	int32_t x;
	int32_t y;

	/* Only the left button acts. */
	x = server->pointer_x;
	y = server->pointer_y;
	progress = zwl_home_progress(server);

	/* The end of a press that may have been the gesture. */
	if (state == 0 && server->home_press) {
		server->home_press = 0;

		/* A click on the launcher or the corner toggles Home. */
		if (!server->home_dragging) {
			if (progress > 0.0f && server->home_to > 0.0f) {
				home_close(server, progress, "launcher");
			} else {
				home_open(server, progress, "launcher");
			}

			/* The click was Home's. */
			return 1;
		}

		/* A drag opens Home past the threshold, and goes back otherwise. */
		server->home_dragging = 0;
		if (server->home_drag >= HOME_THRESHOLD) {
			home_open(server, server->home_drag, "drag");
		} else {
			home_close(server, server->home_drag, "drag");
		}

		/* The drag was Home's. */
		return 1;
	}

	/* A release Home did not start belongs to Home only while Home shows. */
	if (state == 0)
		return progress > 0.0f;

	/* The other buttons do nothing while Home shows, and are the desktop's otherwise. */
	if (button != ZWL_BUTTON_LEFT)
		return progress > 0.0f;

	/* A press on the launcher or in the top-left corner: a click or the start of the gesture. */
	if ((x < 40 && y < ZWL_GLASS_BAR) || (x < HOME_CORNER && y < HOME_CORNER)) {
		server->home_press = 1;
		server->home_dragging = 0;
		server->home_start_x = x;
		server->home_start_y = y;
		return 1;
	}

	/* With Home closed, the press is the desktop's. */
	if (progress <= 0.0f && server->home_to <= 0.0f)
		return 0;

	/* The corner of the desktop that is left in view takes it back. */
	if (x >= (int32_t)server->width - (int32_t)HOME_KEEP_NEAR && y >= (int32_t)server->height - (int32_t)HOME_KEEP_NEAR) {
		home_close(server, progress, "corner");
		return 1;
	}

	/* An icon starts its application; Home closes. */
	app = home_icon_at(server, x, y);
	if (app >= 0) {
		home_launch(server, (unsigned)app);
		home_close(server, progress, "launch");
		return 1;
	}

	/* Elsewhere on Home nothing happens. */
	return 1;
}

/*
 * Follows the corner gesture.  Returns 1 when the motion is Home's.
 */
int
zwl_home_motion(
	struct zwl_server *server)
{
	int32_t dx;
	int32_t dy;
	float moved;
	float progress;

	/* Without a press Home may start from, the motion is Home's only while it shows (for the hover). */
	if (!server->home_press) {
		progress = zwl_home_progress(server);
		if (progress > 0.0f) {
			server->dirty = 1;
			return 1;
		}

		/* With Home closed the motion is the desktop's. */
		return 0;
	}

	/* The gesture starts once the pointer has moved right and down by enough. */
	dx = server->pointer_x - server->home_start_x;
	dy = server->pointer_y - server->home_start_y;
	if (!server->home_dragging) {
		if (dx < HOME_DRAG_START || dy < HOME_DRAG_START)
			return 1;
		server->home_dragging = 1;
		server->home_moving = 0;
		printf("ZWL HOME gesture\n");
	}

	/* How far along the diagonal the pointer is, from the start. */
	moved = ((float)dx + (float)dy) * 0.5f / HOME_DRAG_DISTANCE;
	if (moved < 0.0f)
		moved = 0.0f;
	if (moved > 1.0f)
		moved = 1.0f;

	/* Succeeded: Home follows the pointer. */
	server->home_drag = moved;
	server->dirty = 1;
	return 1;
}

/*
 * Handles a key while Home shows: typing searches, Enter starts the
 * selected application, the arrows move the selection, Backspace erases,
 * Esc clears the search or closes Home.  Returns 1 when the key is Home's
 * (every key while Home shows), 0 otherwise.
 */
int
zwl_home_key(
	struct zwl_server *server,
	uint32_t key,
	uint32_t state)
{
	float progress;
	char character;

	/* Home takes the keys only while it shows or is opening. */
	progress = zwl_home_progress(server);
	if (progress <= 0.0f && server->home_to <= 0.0f)
		return 0;

	/* Releases do nothing, but are Home's too. */
	if (state == 0U)
		return 1;

	/* Routes the key by its code. */
	switch (key) {
	case HOME_KEY_ESC:
		/* Esc clears the search first, and closes Home when there is none. */
		if (server->home_query_length != 0U) {
			server->home_query_length = 0U;
			server->home_query[0] = '\0';
			home_search_changed(server);
		} else {
			home_close(server, progress, "escape");
		}

		/* The key was Home's. */
		return 1;
	case HOME_KEY_BACKSPACE:
		/* Backspace erases the last character. */
		if (server->home_query_length != 0U) {
			server->home_query_length--;
			server->home_query[server->home_query_length] = '\0';
			home_search_changed(server);
		}

		/* The key was Home's. */
		return 1;
	case HOME_KEY_ENTER:
	case HOME_KEY_KPENTER:
		/* Enter starts the selected application and closes Home. */
		home_layout(server);
		if (server->home_selected >= 0 && (unsigned)server->home_selected < home_shown_count) {
			home_launch(server, home_shown[server->home_selected]);
			home_close(server, progress, "launch");
		}

		/* The key was Home's. */
		return 1;
	case HOME_KEY_RIGHT:
	case HOME_KEY_DOWN:
		/* The selection moves to the next application. */
		if (server->home_selected + 1 < (int)home_shown_count)
			server->home_selected++;
		server->dirty = 1;
		return 1;
	case HOME_KEY_LEFT:
	case HOME_KEY_UP:
		/* And to the one before. */
		if (server->home_selected > 0)
			server->home_selected--;
		server->dirty = 1;
		return 1;
	default:
		break;
	}

	/* Any other key that types a character adds it to the search. */
	if (key >= HOME_KEYS)
		return 1;
	character = home_characters[key];
	if (character == 0)
		return 1;

	/* The search holds what fits. */
	if (server->home_query_length + 1U >= sizeof(server->home_query))
		return 1;

	/* Succeeded: the search changes. */
	server->home_query[server->home_query_length] = character;
	server->home_query_length++;
	server->home_query[server->home_query_length] = '\0';
	home_search_changed(server);
	return 1;
}

/*
 * Keeps Home's animation drawing until it settles, and collects the
 * applications Home started that have ended.
 */
void
zwl_home_tick(
	struct zwl_server *server)
{
	uint64_t elapsed;
	uint64_t length;
	pid_t child;
	int status;

	/* Every ended child is collected, so none is left a zombie. */
	for (;;) {
		child = waitpid(-1, &status, WNOHANG);
		if (child <= 0)
			break;
		printf("ZWL HOME ended pid=%d status=%d\n", (int)child, status);
	}

	/* No animation: nothing to draw. */
	if (!server->home_moving)
		return;

	/* The animation draws every frame; at its end Home is where it went. */
	server->dirty = 1;
	length = HOME_CLOSE_MS;
	if (server->home_to > server->home_from)
		length = HOME_OPEN_MS;
	elapsed = zwl_milliseconds() - server->home_start_ms;
	if (elapsed < length)
		return;

	/* Settled: opened or closed. */
	server->home_moving = 0;
	server->home = server->home_to;
	if (server->home > 0.0f) {
		home_layout(server);
		printf("ZWL HOME opened apps=%u\n", home_shown_count);
		home_log_icons();
	} else {
		printf("ZWL HOME closed\n");
	}
}

/* Reads the applications' list once: the file, or the built-in list when there is none. */
static void
home_read_apps(void)
{
	char line[320];
	FILE *file;
	char *end;
	char *got;

	/* Read once. */
	if (home_apps_read)
		return;
	home_apps_read = 1;

	/* The file, a line an application; blank lines and # comments are skipped. */
	file = fopen(HOME_APPS_PATH, "r");
	if (file != NULL) {
		for (;;) {
			got = fgets(line, sizeof(line), file);
			if (got == NULL)
				break;

			/* The line without its newline; blank and comment lines are skipped. */
			end = strchr(line, '\n');
			if (end != NULL)
				*end = '\0';
			if (line[0] == '\0' || line[0] == '#')
				continue;
			home_parse_line(line);
		}

		/* The file is not needed again. */
		fclose(file);
	}

	/* Without a usable file, the applications zdesktop has. */
	if (home_app_count == 0U) {
		home_add_app("Terminal", "/bin/zdesktop-terminal", "term shell console sh", 0x323a4eU);
		home_add_app("Model viewer", "/bin/mview --windowed --size=960x640", "3d mview model vulkan viewer", 0xe07a5aU);
		home_add_app("Vulkan test", "/bin/wltest --windowed --size=640x420 --frames=3600 --delay-ms=30", "wltest gpu test", 0x5a8de0U);
		home_add_app("Shared memory", "/bin/wlshm --size=480x320 --frames=6000", "wlshm shm test", 0x5aa87aU);
	}
}

/* Adds an application to the list, when there is room. */
static void
home_add_app(
	const char *name,
	const char *command,
	const char *keywords,
	uint32_t rgb)
{
	struct home_app *app;

	/* A full list takes no more. */
	if (home_app_count >= HOME_APPS_MAX)
		return;

	/* The application's texts and its colour. */
	app = &home_apps[home_app_count];
	snprintf(app->name, sizeof(app->name), "%s", name);
	snprintf(app->command, sizeof(app->command), "%s", command);
	snprintf(app->keywords, sizeof(app->keywords), "%s", keywords);
	app->color[0] = (float)((rgb >> 16) & 0xffU) / 255.0f;
	app->color[1] = (float)((rgb >> 8) & 0xffU) / 255.0f;
	app->color[2] = (float)(rgb & 0xffU) / 255.0f;
	app->color[3] = 1.0f;
	home_app_count++;
}

/* Reads one line of the list: name|command|keywords|RRGGBB (the last two may be left out). */
static void
home_parse_line(
	char *line)
{
	static char empty[1];
	char *fields[4];
	char *bar;
	unsigned count;

	/* Splits the line at the bars; a field left out is empty. */
	fields[0] = line;
	fields[1] = empty;
	fields[2] = empty;
	fields[3] = empty;
	count = 1U;
	while (count < 4U) {
		bar = strchr(fields[count - 1U], '|');
		if (bar == NULL)
			break;
		*bar = '\0';
		fields[count] = bar + 1;
		count++;
	}

	/* A line needs a name and a command. */
	if (count < 2U || fields[0][0] == '\0' || fields[1][0] == '\0')
		return;

	/* Succeeded: the application joins the list (grey without a colour). */
	home_add_app(fields[0], fields[1], fields[2], home_hex(fields[3]));
}

/* Reads an RRGGBB colour; a malformed one is a mid grey. */
static uint32_t
home_hex(
	const char *text)
{
	unsigned long value;
	size_t length;
	char *end;

	/* Six hexadecimal digits and nothing more. */
	length = strlen(text);
	value = strtoul(text, &end, 16);
	if (end == text || *end != '\0' || length != 6U)
		return 0x707888U;

	/* Reports the colour. */
	return (uint32_t)value;
}

/* Works out which applications show and where their icons go: a centred grid of up to six columns. */
static void
home_layout(
	struct zwl_server *server)
{
	unsigned index;
	unsigned columns;
	unsigned rows;
	unsigned slot;
	int32_t left;
	int32_t top;
	int32_t space;
	int found;

	/* The applications the search finds (all, without one). */
	home_shown_count = 0U;
	for (index = 0U; index < home_app_count; index++) {
		found = home_matches(&home_apps[index], server->home_query);
		if (found)
			home_shown[home_shown_count++] = index;
	}

	/* The selection stays on something shown. */
	if (server->home_selected >= (int)home_shown_count)
		server->home_selected = (int)home_shown_count - 1;
	if (server->home_selected < 0 && home_shown_count != 0U)
		server->home_selected = 0;

	/* Nothing shown, nothing to place. */
	if (home_shown_count == 0U)
		return;

	/* The grid, centred in the space under the system bar (a little above the middle). */
	columns = home_shown_count;
	if (columns > HOME_COLUMNS)
		columns = HOME_COLUMNS;
	rows = (home_shown_count + columns - 1U) / columns;
	left = ((int32_t)server->width - (int32_t)columns * HOME_CELL_WIDTH) / 2;
	space = (int32_t)server->height - ZWL_GLASS_BAR;
	top = ZWL_GLASS_BAR + (space - (int32_t)rows * HOME_CELL_HEIGHT) * 2 / 5;

	/* Each icon's top-left corner, centred in its cell. */
	for (slot = 0U; slot < home_shown_count; slot++) {
		home_icon_x[slot] = left + (int32_t)(slot % columns) * HOME_CELL_WIDTH + (HOME_CELL_WIDTH - HOME_ICON) / 2;
		home_icon_y[slot] = top + (int32_t)(slot / columns) * HOME_CELL_HEIGHT + 20;
	}
}

/* Tells whether an application is found by the search (an empty search finds all). */
static int
home_matches(
	const struct home_app *app,
	const char *query)
{
	int found;

	/* Nothing typed finds everything. */
	if (query[0] == '\0')
		return 1;

	/* The name, then the command, then the keywords. */
	found = home_contains(app->name, query);
	if (found)
		return 1;
	found = home_contains(app->command, query);
	if (found)
		return 1;
	found = home_contains(app->keywords, query);
	if (found)
		return 1;

	/* Not found. */
	return 0;
}

/* Tells whether a text contains the query, ignoring the case of letters. */
static int
home_contains(
	const char *text,
	const char *query)
{
	size_t start;
	size_t index;
	char a;
	char b;

	/* Tries the query at each place of the text. */
	for (start = 0U; text[start] != '\0'; start++) {
		for (index = 0U; query[index] != '\0'; index++) {
			a = text[start + index];
			b = query[index];
			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (a != b)
				break;
		}

		/* Every character of the query matched here. */
		if (query[index] == '\0')
			return 1;
	}

	/* No place matches. */
	return 0;
}

/* Returns the application whose icon (or label) is under a point, or -1. */
static int
home_icon_at(
	struct zwl_server *server,
	int32_t x,
	int32_t y)
{
	unsigned slot;

	/* Each icon's cell: the icon and its label under it. */
	home_layout(server);
	for (slot = 0U; slot < home_shown_count; slot++) {
		if (x < home_icon_x[slot] - 20 || x >= home_icon_x[slot] + HOME_ICON + 20)
			continue;
		if (y < home_icon_y[slot] || y >= home_icon_y[slot] + HOME_ICON + HOME_LABEL + 12)
			continue;
		return (int)home_shown[slot];
	}

	/* Nothing there. */
	return -1;
}

/* Draws one icon: its rounded square with the name's first letter, the selection's ring, and the name under it. */
static void
home_draw_icon(
	struct zwl_server *server,
	VkCommandBuffer command,
	unsigned slot,
	float opacity)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float ink[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	const struct home_app *app;
	struct glass_shape shape;
	float color[4];
	char letter[2];
	int32_t x;
	int32_t y;
	int32_t width;
	int over;

	/* Where the icon is, and whether the pointer is on it. */
	app = &home_apps[home_shown[slot]];
	x = home_icon_x[slot];
	y = home_icon_y[slot];
	over = 0;
	if (server->pointer_x >= x && server->pointer_x < x + HOME_ICON && server->pointer_y >= y && server->pointer_y < y + HOME_ICON)
		over = 1;

	/* A soft shadow under the icon. */
	glass_shape_init(&shape, (float)x, (float)y + 6.0f, (float)HOME_ICON, (float)HOME_ICON);
	shape.quad[0] -= 24.0f;
	shape.quad[1] -= 24.0f;
	shape.quad[2] += 48.0f;
	shape.quad[3] += 48.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = HOME_ICON_RADIUS;
	shape.soft = 14.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.22f;
	shape.opacity = opacity;
	glass_shape_draw(server, command, &shape);

	/* The icon's square in its colour, lighter under the pointer. */
	memcpy(color, app->color, sizeof(color));
	if (over) {
		color[0] = color[0] + (1.0f - color[0]) * 0.15f;
		color[1] = color[1] + (1.0f - color[1]) * 0.15f;
		color[2] = color[2] + (1.0f - color[2]) * 0.15f;
	}

	/* The square, faded in with Home. */
	color[3] = opacity;
	glass_draw_solid(server, command, (float)x, (float)y, (float)HOME_ICON, (float)HOME_ICON, HOME_ICON_RADIUS, color);

	/* A light sheen on its upper half. */
	color[0] = 1.0f;
	color[1] = 1.0f;
	color[2] = 1.0f;
	color[3] = 0.12f * opacity;
	glass_draw_solid(server, command, (float)x, (float)y, (float)HOME_ICON, (float)HOME_ICON / 2.0f, HOME_ICON_RADIUS, color);

	/* The name's first letter, large and white, in the middle. */
	letter[0] = app->name[0];
	letter[1] = '\0';
	memcpy(color, white, sizeof(color));
	color[3] = opacity;
	width = glass_text_width(server, SIZE_ICON, letter);
	glass_draw_text(server, command, SIZE_ICON, x + (HOME_ICON - width) / 2, y + HOME_ICON / 2 + 13, letter, HOME_ICON, color);

	/* The selection (with the keyboard, or the first search result) has a blue ring. */
	if ((int)slot == server->home_selected && (server->home_query_length != 0U || server->home_selected > 0)) {
		glass_shape_init(&shape, (float)x - 5.0f, (float)y - 5.0f, (float)HOME_ICON + 10.0f, (float)HOME_ICON + 10.0f);
		shape.mode = MODE_RING;
		shape.radius = HOME_ICON_RADIUS + 5.0f;
		shape.soft = 2.5f;
		shape.color[0] = 0.25f;
		shape.color[1] = 0.52f;
		shape.color[2] = 0.98f;
		shape.color[3] = opacity;
		glass_shape_draw(server, command, &shape);
	}

	/* The name under the icon, centred on it. */
	memcpy(color, ink, sizeof(color));
	color[3] = opacity;
	width = glass_text_width(server, SIZE_TITLE, app->name);
	if (width > HOME_CELL_WIDTH - 8)
		width = HOME_CELL_WIDTH - 8;
	glass_draw_text(server, command, SIZE_TITLE, x + (HOME_ICON - width) / 2, y + HOME_ICON + HOME_LABEL, app->name, HOME_CELL_WIDTH - 8, color);
}

/* Draws the search text at the top, on a faint pill (no search box). */
static void
home_draw_search(
	struct zwl_server *server,
	VkCommandBuffer command,
	float opacity)
{
	static const float pill[4] = { 1.0f, 1.0f, 1.0f, 0.55f };
	static const float ink[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	float color[4];
	int32_t width;
	int32_t x;
	int32_t y;

	/* The text's width, and the pill a little wider, centred under the system bar. */
	width = glass_text_width(server, SIZE_SEARCH, server->home_query);
	x = ((int32_t)server->width - width) / 2;
	y = ZWL_GLASS_BAR + 44;
	memcpy(color, pill, sizeof(color));
	color[3] = pill[3] * opacity;
	glass_draw_solid(server, command, (float)(x - 22), (float)y, (float)(width + 44), 40.0f, 20.0f, color);

	/* The text itself. */
	memcpy(color, ink, sizeof(color));
	color[3] = opacity;
	glass_draw_text(server, command, SIZE_SEARCH, x, y + 28, server->home_query, (int32_t)server->width, color);
}

/* Opens Home, from where it is now. */
static void
home_open(
	struct zwl_server *server,
	float from,
	const char *via)
{
	/* The search starts empty, the first application selected. */
	server->home_query_length = 0U;
	server->home_query[0] = '\0';
	server->home_selected = 0;
	server->drag = NULL;
	printf("ZWL HOME open via=%s\n", via);
	home_settle(server, from, 1.0f);
}

/* Closes Home, from where it is now; the search is forgotten. */
static void
home_close(
	struct zwl_server *server,
	float from,
	const char *via)
{
	/* The search goes with it. */
	server->home_query_length = 0U;
	server->home_query[0] = '\0';
	printf("ZWL HOME close via=%s\n", via);
	home_settle(server, from, 0.0f);
}

/* Starts the animation from one position to another. */
static void
home_settle(
	struct zwl_server *server,
	float from,
	float to)
{
	/* The animation's ends and its start; the frames follow in zwl_home_tick. */
	server->home_from = from;
	server->home_to = to;
	server->home_start_ms = zwl_milliseconds();
	server->home_moving = 1;
	server->dirty = 1;
}

/* Starts an application with /bin/sh -c, with the compositor's socket in its environment. */
static void
home_launch(
	struct zwl_server *server,
	unsigned app)
{
	char directory[108];
	const char *name;
	char *slash;
	pid_t child;
	int descriptor;

	/* The child. */
	child = fork();
	if (child < 0) {
		printf("ZWL HOME launch name=%s error=%d\n", home_apps[app].name, errno);
		return;
	}

	/* The child: its own session, none of zwl's descriptors, the socket's place, and the command. */
	if (child == 0) {
		(void)setsid();
		for (descriptor = 3; descriptor < 1024; descriptor++)
			(void)close(descriptor);
		descriptor = open("/dev/null", O_RDONLY);
		if (descriptor >= 0 && descriptor != 0) {
			(void)dup2(descriptor, 0);
			(void)close(descriptor);
		}

		/* The socket as XDG_RUNTIME_DIR and WAYLAND_DISPLAY, the way clients look for it. */
		snprintf(directory, sizeof(directory), "%s", server->socket_path);
		slash = strrchr(directory, '/');
		name = directory;
		if (slash != NULL) {
			*slash = '\0';
			name = slash + 1;
			(void)setenv("XDG_RUNTIME_DIR", directory, 1);
		}

		/* The socket's name within that directory. */
		(void)setenv("WAYLAND_DISPLAY", name, 1);

		/* Only a failed exec comes back. */
		(void)execl("/bin/sh", "sh", "-c", home_apps[app].command, (char *)NULL);
		_exit(127);
	}

	/* Succeeded: the application is starting. */
	printf("ZWL HOME launch name=%s pid=%d\n", home_apps[app].name, (int)child);
}

/* Lays the icons out again for a changed search, selects the first result and says what was found. */
static void
home_search_changed(
	struct zwl_server *server)
{
	unsigned slot;

	/* The first result is selected. */
	server->home_selected = 0;
	home_layout(server);
	server->dirty = 1;

	/* The search and its results, for whoever reads the log. */
	printf("ZWL HOME search query=\"%s\" results=%u", server->home_query, home_shown_count);
	for (slot = 0U; slot < home_shown_count; slot++)
		printf(" [%s]", home_apps[home_shown[slot]].name);
	printf("\n");
	home_log_icons();
}

/* Says where each icon shown is (its centre), for whoever reads the log (the tests click there). */
static void
home_log_icons(void)
{
	unsigned slot;

	/* One line an icon. */
	for (slot = 0U; slot < home_shown_count; slot++)
		printf("ZWL HOME icon name=\"%s\" x=%d y=%d\n", home_apps[home_shown[slot]].name, home_icon_x[slot] + HOME_ICON / 2, home_icon_y[slot] + HOME_ICON / 2);
}

/* Eases an animation: quick at first, settling gently (cubic ease-out). */
static float
home_ease(
	float t)
{
	float remaining;

	/* 1 - (1 - t)^3. */
	remaining = 1.0f - t;
	return 1.0f - remaining * remaining * remaining;
}
