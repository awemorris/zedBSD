/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass look's shell (ws035-p059, p062): windows, their title bars, the
 * system bar, and what the pointer does to them.
 *
 * A window's title bar floats above its body with a gap: a rounded glass
 * panel with the application's mark, the title and the minimize, maximize
 * and close buttons.  Maximizing docks the window to the system bar: the body
 * fills the output under the bar, the floating title bar goes, and the title
 * with its buttons (restore instead of maximize) moves into the bar's left
 * zone.  A window docks by a double click on its title bar, by its maximize
 * button, or by dragging its title bar into the system bar; it comes back by
 * a double click on the title in the bar, by the restore button, or by
 * pulling the title down out of the bar, which leaves the window under the
 * pointer and goes on moving it.  Docking and coming back are animated for
 * DOCK_MS: the body's rectangle and the title bar's slide between their
 * places and the title bar's glass fades.
 *
 * The system bar has three zones: on the left the launcher, "zedBSD" and the
 * docked window; towards the right four virtual desktops; at the right edge
 * the signal, the battery and the clock.  The launcher, the desktops, the
 * status and minimize are drawn only (a mock-up).
 */

#include "glass.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The title bar's buttons, counted from the right. */
#define BUTTON_CLOSE		0
#define BUTTON_MAXIMIZE		1
#define BUTTON_MINIMIZE		2
#define BUTTON_COUNT		3
#define BUTTON_SPACING		34
#define BUTTON_WIDTH		30
#define BUTTON_HEIGHT		28

/* The docked title's buttons in the system bar are further apart. */
#define BAR_BUTTON_SPACING	46

/* The dock animation, a double click, and how far a docked title is pulled down to come off. */
#define DOCK_MS			220U
#define DOUBLE_CLICK_MS		400U
#define PULL_DISTANCE		16

/* A docked body starts this far under the top of the output. */
#define DOCK_TOP		(ZWL_GLASS_BAR + 4)

/* The virtual desktops: how many, and the size of each picture. */
#define DESKTOPS		4
#define DESKTOP_WIDTH		40
#define DESKTOP_HEIGHT		20
#define DESKTOP_GAP		6

/* Where the pointer is over a window. */
enum shell_hit {
	HIT_NONE,
	HIT_TITLE,
	HIT_BODY
};

/* A rectangle in output pixels. */
struct shell_rect {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
};

/* Where the system bar's parts are (they follow the clock's width). */
struct shell_bar {
	char clock[48];
	int32_t clock_x;
	int32_t battery_x;
	int32_t signal_x;
	int32_t status_line;
	int32_t desktops_x;
	int32_t desktops_width;
	int32_t desktops_line;
	int32_t buttons[BUTTON_COUNT];
	int32_t menu_line;
	int32_t title_x;
};

static void bar_layout(struct zwl_server *server, struct shell_bar *bar);
static void draw_window(struct zwl_server *server, VkCommandBuffer command, struct zwl_object *surface, unsigned focused, const struct shell_bar *bar);
static void draw_body(struct zwl_server *server, VkCommandBuffer command, struct zwl_object *surface, const struct shell_rect *body, unsigned docked, unsigned focused);
static void draw_title_bar(struct zwl_server *server, VkCommandBuffer command, struct zwl_object *surface, const struct shell_rect *panel, float fade, float buttons, unsigned focused);
static void draw_title(struct zwl_server *server, VkCommandBuffer command, struct zwl_object *surface, int32_t x, int32_t middle, int32_t limit, const float *ink);
static void draw_sign(struct zwl_server *server, VkCommandBuffer command, int button, int32_t cx, int32_t cy, unsigned restore, unsigned over, float fade, const float *ink);
static void draw_system_bar(struct zwl_server *server, VkCommandBuffer command, const struct shell_bar *bar);
static void draw_desktops(struct zwl_server *server, VkCommandBuffer command, const struct shell_bar *bar, const float *line);
static void draw_status(struct zwl_server *server, VkCommandBuffer command, const struct shell_bar *bar, const float *ink);
static void draw_dock_hint(struct zwl_server *server, VkCommandBuffer command);
static float animation_progress(struct zwl_server *server);
static void lerp_rect(const struct shell_rect *from, const struct shell_rect *to, float t, struct shell_rect *result);
static void body_rect(struct zwl_server *server, const struct zwl_object *surface, struct shell_rect *body);
static void docked_rect(struct zwl_server *server, struct shell_rect *body);
static void floating_title(const struct shell_rect *body, struct shell_rect *panel);
static void bar_title_slot(struct zwl_server *server, const struct shell_bar *bar, struct shell_rect *slot);
static void window_size(const struct zwl_object *surface, int32_t *width, int32_t *height);
static enum shell_hit window_hit(struct zwl_server *server, const struct zwl_object *surface, int32_t x, int32_t y);
static int button_at(const struct zwl_object *surface, int32_t x, int32_t y);
static void button_centre(const struct zwl_object *surface, int button, int32_t *x, int32_t *y);
static int bar_button_at(const struct shell_bar *bar, int32_t x, int32_t y);
static struct zwl_object *window_at(struct zwl_server *server, int32_t x, int32_t y, enum shell_hit *hit);
static struct zwl_object *docked_window(struct zwl_server *server);
static void window_raise(struct zwl_server *server, struct zwl_object *surface);
static void window_dock(struct zwl_server *server, struct zwl_object *surface, int32_t restore_x, int32_t restore_y, const char *via);
static void window_undock(struct zwl_server *server, struct zwl_object *surface, int32_t x, int32_t y, const char *via);
static void window_configure(struct zwl_object *surface);
static unsigned double_click(struct zwl_server *server, struct zwl_object *surface);
static int bar_press(struct zwl_server *server);

/*
 * Draws the wallpaper, the windows from the bottom with their shadows and
 * title bars, and the system bar over them.
 */
void
zwl_glass_draw(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object **windows,
	unsigned count)
{
	struct glass_shape shape;
	struct zwl_object *top;
	struct shell_bar bar;
	unsigned index;
	unsigned focused;

	/* The wallpaper over the whole output. */
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_IMAGE;
	shape.opaque = 1.0f;
	shape.set = glass_wallpaper_set(server);
	glass_shape_draw(server, command, &shape);

	/* The system bar's layout, which a docking title bar moves to. */
	bar_layout(server, &bar);

	/* The windows; the top one has the focus; where a dragged one would dock shows just under it. */
	top = zwl_top_window(server);
	for (index = 0; index < count; index++) {
		focused = 0;
		if (windows[index] == top)
			focused = 1;
		if (windows[index] == server->drag)
			draw_dock_hint(server, command);
		draw_window(server, command, windows[index], focused, &bar);
	}

	/* The system bar over everything but the cursor. */
	draw_system_bar(server, command, &bar);

	/* A frame of the animation. */
	if (server->anim != NULL && server->log_frames)
		printf("ZWL GLASS anim surface=%u docking=%u t=%.2f\n", server->anim->id, server->anim_docking, (double)animation_progress(server));
}

/*
 * Handles a pointer button in the glass look.  A press raises the window
 * under the pointer; on its title bar it starts a move, presses a button or
 * (twice) docks it; on the system bar it acts on the docked window.  A
 * release ends a move, docking the window when it ends in the system bar.
 * Returns 1 when the button is zdesktop's, 0 when it goes to the client.
 */
int
zwl_glass_button(
	struct zwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct zwl_object *surface;
	enum shell_hit hit;
	unsigned second;
	int pressed;

	/* A release ends a move (docking in the system bar) or a pull. */
	if (state == 0) {
		if (server->pull != NULL) {
			server->pull = NULL;
			return 1;
		}

		/* Without a move the client has the release. */
		if (server->drag == NULL)
			return 0;

		/* Let go in the system bar, the window docks; it comes back to where the move started. */
		surface = server->drag;
		server->drag = NULL;
		if (server->pointer_y < ZWL_GLASS_BAR) {
			window_dock(server, surface, server->drag_start_x, server->drag_start_y, "drag");
			return 1;
		}

		/* Otherwise it stays where it was moved. */
		printf("ZWL GLASS moved surface=%u x=%d y=%d\n", surface->id, surface->x, surface->y);
		return 1;
	}

	/* The system bar is zdesktop's. */
	if (server->pointer_y < ZWL_GLASS_BAR) {
		pressed = bar_press(server);
		return pressed;
	}

	/* Only the left button acts on windows. */
	surface = window_at(server, server->pointer_x, server->pointer_y, &hit);
	if (button != ZWL_BUTTON_LEFT)
		return hit != HIT_BODY;

	/* A press on the desktop is zdesktop's. */
	if (surface == NULL)
		return 1;

	/* The window comes to the top and takes the focus. */
	window_raise(server, surface);

	/* On the body the client has the press. */
	if (hit == HIT_BODY)
		return 0;

	/* On a button, its action. */
	pressed = button_at(surface, server->pointer_x, server->pointer_y);
	if (pressed == BUTTON_CLOSE) {
		(void)zwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
		printf("ZWL GLASS close surface=%u\n", surface->id);
		return 1;
	}

	/* Maximize docks the window. */
	if (pressed == BUTTON_MAXIMIZE) {
		window_dock(server, surface, surface->x, surface->y, "button");
		return 1;
	}

	/* Minimize has no action yet. */
	if (pressed == BUTTON_MINIMIZE)
		return 1;

	/* A second press on the title bar docks the window. */
	second = double_click(server, surface);
	if (second) {
		window_dock(server, surface, surface->x, surface->y, "double-click");
		return 1;
	}

	/* Otherwise a move starts. */
	server->drag = surface;
	server->drag_dx = server->pointer_x - surface->x;
	server->drag_dy = server->pointer_y - surface->y;
	server->drag_start_x = surface->x;
	server->drag_start_y = surface->y;

	/* Succeeded: the press was zdesktop's. */
	return 1;
}

/*
 * Moves the window being moved, or pulls a docked window out of the system
 * bar.  Returns 1 when the motion is zdesktop's.
 */
int
zwl_glass_motion(
	struct zwl_server *server)
{
	struct zwl_object *surface;
	int32_t lowest;
	int32_t x;
	int32_t y;

	/* The hover of buttons, the dock hint and the moves are redrawn. */
	server->dirty = 1;

	/* A docked title pulled far enough down comes off under the pointer, and the move goes on. */
	surface = server->pull;
	if (surface != NULL) {
		if (surface->dead || !surface->mapped) {
			server->pull = NULL;
			return 1;
		}

		/* Not far enough yet. */
		if (server->pointer_y < ZWL_GLASS_BAR + PULL_DISTANCE)
			return 1;

		/* The same part of the title bar stays under the pointer. */
		x = server->pointer_x - (int32_t)((int64_t)surface->restore_width * server->pointer_x / (int32_t)server->width);
		y = server->pointer_y + ZWL_GLASS_GAP + ZWL_GLASS_TITLE / 2;
		window_undock(server, surface, x, y, "pull");
		server->pull = NULL;
		server->drag = surface;
		server->drag_dx = server->pointer_x - x;
		server->drag_dy = server->pointer_y - y;
		server->drag_start_x = surface->restore_x;
		server->drag_start_y = surface->restore_y;
		return 1;
	}

	/* Without a move the client hears the motion. */
	surface = server->drag;
	if (surface == NULL)
		return 0;

	/* A window that went away ends the move. */
	if (surface->dead || !surface->mapped) {
		server->drag = NULL;
		return 1;
	}

	/* The body follows the pointer; the title bar stays below the system bar. */
	surface->x = server->pointer_x - server->drag_dx;
	surface->y = server->pointer_y - server->drag_dy;
	lowest = ZWL_GLASS_BAR + ZWL_GLASS_GAP + ZWL_GLASS_TITLE;
	if (surface->y < lowest)
		surface->y = lowest;

	/* Succeeded: the motion was zdesktop's. */
	return 1;
}

/*
 * Places a new window in the glass look: centred in the space below the
 * system bar, cascaded like the plain look.
 */
void
zwl_glass_place(
	struct zwl_server *server,
	struct zwl_object *surface,
	int32_t width,
	int32_t height,
	int32_t step)
{
	int32_t space;

	/* The space for bodies under the system bar and a title bar. */
	space = (int32_t)server->height - ZWL_GLASS_TOP - ZWL_GLASS_MARGIN;
	surface->x = ((int32_t)server->width - width) / 2 + step;
	surface->y = ZWL_GLASS_TOP + (space - height) / 2 + step;

	/* Never above the space, nor left of the output. */
	if (surface->x < ZWL_GLASS_MARGIN)
		surface->x = ZWL_GLASS_MARGIN;
	if (surface->y < ZWL_GLASS_TOP)
		surface->y = ZWL_GLASS_TOP;
}

/*
 * Keeps the output being redrawn: every frame while the dock animation runs
 * (ending it after DOCK_MS), and when the clock shows a new minute.
 */
void
zwl_glass_tick(
	struct zwl_server *server)
{
	float progress;
	time_t now;

	/* The animation draws every frame until it is done. */
	if (server->anim != NULL) {
		progress = animation_progress(server);
		server->dirty = 1;
		if (progress >= 1.0f)
			server->anim = NULL;
	}

	/* The minute of the clock. */
	now = time(NULL);
	if ((int64_t)now / 60 == server->clock_minute)
		return;

	/* A new minute. */
	server->clock_minute = (int64_t)now / 60;
	server->dirty = 1;
}

/*
 * Lays out the system bar from the right: the clock, the battery, the
 * signal, a line, the desktops, a line, the docked window's buttons; and
 * from the left the launcher, "zedBSD", a line and the docked title.
 */
static void
bar_layout(
	struct zwl_server *server,
	struct shell_bar *bar)
{
	struct tm local;
	time_t now;
	int button;

	/* The date and time at the right edge. */
	now = time(NULL);
	memset(&local, 0, sizeof(local));
	(void)localtime_r(&now, &local);
	bar->clock[0] = '\0';
	(void)strftime(bar->clock, sizeof(bar->clock), "%a %b %e  %H:%M", &local);
	bar->clock_x = (int32_t)server->width - 16 - glass_text_width(server, SIZE_BAR, bar->clock);

	/* The battery and the signal left of it, and a line. */
	bar->battery_x = bar->clock_x - 44;
	bar->signal_x = bar->battery_x - 36;
	bar->status_line = bar->signal_x - 18;

	/* The desktops, and a line. */
	bar->desktops_width = DESKTOPS * DESKTOP_WIDTH + (DESKTOPS - 1) * DESKTOP_GAP + 12;
	bar->desktops_x = bar->status_line - 16 - bar->desktops_width;
	bar->desktops_line = bar->desktops_x - 16;

	/* The docked window's buttons, close nearest the line. */
	for (button = 0; button < BUTTON_COUNT; button++)
		bar->buttons[button] = bar->desktops_line - 30 - button * BAR_BUTTON_SPACING;

	/* On the left, after the launcher and "zedBSD", a line and the docked title. */
	bar->menu_line = 44 + glass_text_width(server, SIZE_BAR, "zedBSD") + 16;
	bar->title_x = bar->menu_line + 17;
}

/*
 * Draws a window: its body, and its title bar floating above it, docked in
 * the system bar (drawn with the bar), or on its way between the two.
 */
static void
draw_window(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object *surface,
	unsigned focused,
	const struct shell_bar *bar)
{
	struct shell_rect body;
	struct shell_rect from;
	struct shell_rect to;
	struct shell_rect panel;
	struct shell_rect slot;
	float t;

	/* The body, where it is now. */
	body_rect(server, surface, &body);

	/* While docking or coming back, the title bar slides between its two places and its glass fades. */
	if (server->anim == surface) {
		t = animation_progress(server);
		draw_body(server, command, surface, &body, 0, focused);
		bar_title_slot(server, bar, &slot);
		memcpy(&from, server->anim_from, sizeof(from));
		memcpy(&to, server->anim_to, sizeof(to));
		if (server->anim_docking) {
			floating_title(&from, &panel);
			lerp_rect(&panel, &slot, t, &panel);
			draw_title_bar(server, command, surface, &panel, 1.0f - t, 1.0f - t, focused);
		} else {
			floating_title(&to, &panel);
			lerp_rect(&slot, &panel, t, &panel);
			draw_title_bar(server, command, surface, &panel, t, t, focused);
		}

		/* Nothing more is drawn for it. */
		return;
	}

	/* A docked window has only its body; its title is in the system bar. */
	if (surface->maximized) {
		draw_body(server, command, surface, &body, 1, focused);
		return;
	}

	/* A floating window. */
	draw_body(server, command, surface, &body, 0, focused);
	floating_title(&body, &panel);
	draw_title_bar(server, command, surface, &panel, 1.0f, 1.0f, focused);
}

/*
 * Draws a window's body in a rectangle: its shadow, the frosted glass under
 * a see-through body, and its image with rounded corners (a docked body's
 * lower corners are below the output).
 */
static void
draw_body(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object *surface,
	const struct shell_rect *body,
	unsigned docked,
	unsigned focused)
{
	const struct zwl_import *image;
	struct glass_shape shape;
	float soft;

	/* The shadow, deeper for the focused window. */
	soft = 22.0f;
	if (focused)
		soft = 30.0f;
	glass_shape_init(&shape, (float)body->x, (float)body->y + 8.0f, (float)body->width, (float)body->height);
	shape.quad[0] -= 2.0f * soft;
	shape.quad[1] -= 2.0f * soft;
	shape.quad[2] += 4.0f * soft;
	shape.quad[3] += 4.0f * soft;
	shape.mode = MODE_SHADOW;
	shape.radius = GLASS_RADIUS;
	shape.soft = soft;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.20f;
	glass_shape_draw(server, command, &shape);

	/* A see-through body lies on frosted glass. */
	if (server->window_opacity < 1.0f) {
		glass_shape_init(&shape, (float)body->x, (float)body->y, (float)body->width, (float)body->height);
		if (docked)
			shape.box[3] += 2.0f * GLASS_RADIUS;
		shape.mode = MODE_GLASS;
		shape.radius = GLASS_RADIUS;
		shape.color[0] = 1.0f;
		shape.color[1] = 1.0f;
		shape.color[2] = 1.0f;
		shape.color[3] = 0.30f;
		shape.edge = 0.75f;
		glass_shape_draw(server, command, &shape);
	}

	/* The image, stretched to the rectangle while it changes, as opaque as asked. */
	image = zwl_compose_surface_image(surface);
	glass_shape_init(&shape, (float)body->x, (float)body->y, (float)body->width, (float)body->height);
	if (docked)
		shape.box[3] += 2.0f * GLASS_RADIUS;
	shape.opacity = server->window_opacity;
	shape.mode = MODE_IMAGE;
	shape.radius = GLASS_RADIUS;
	shape.set = image->set;
	if (image->draw == ZWL_DRAW_OPAQUE)
		shape.opaque = 1.0f;
	glass_shape_draw(server, command, &shape);
}

/*
 * Draws a title bar in a rectangle: its shadow and glass faded by fade, the
 * mark and title, and its buttons faded by buttons (a docking title bar
 * keeps its title while its glass and buttons fade).
 */
static void
draw_title_bar(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object *surface,
	const struct shell_rect *panel,
	float fade,
	float buttons,
	unsigned focused)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float faint[4] = { 0.40f, 0.46f, 0.56f, 1.0f };
	struct glass_shape shape;
	const float *ink;
	int32_t cx;
	int32_t cy;
	int button;
	int over;

	/* Its shadow. */
	glass_shape_init(&shape, (float)panel->x, (float)panel->y + 4.0f, (float)panel->width, (float)panel->height);
	shape.quad[0] -= 36.0f;
	shape.quad[1] -= 36.0f;
	shape.quad[2] += 72.0f;
	shape.quad[3] += 72.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = GLASS_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.12f;
	shape.opacity = fade;
	glass_shape_draw(server, command, &shape);

	/* The glass, whiter for the focused window. */
	glass_shape_init(&shape, (float)panel->x, (float)panel->y, (float)panel->width, (float)panel->height);
	shape.mode = MODE_GLASS;
	shape.radius = GLASS_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.38f;
	if (focused)
		shape.color[3] = 0.55f;
	shape.edge = 0.85f;
	shape.opacity = fade;
	glass_shape_draw(server, command, &shape);

	/* The mark and the title, darker for the focused window, cut short before the buttons. */
	ink = faint;
	if (focused)
		ink = dark;
	draw_title(server, command, surface, panel->x + 14, panel->y + panel->height / 2, panel->width - 44 - BUTTON_SPACING * BUTTON_COUNT - 12, ink);

	/* The buttons, as they fade. */
	if (buttons <= 0.0f)
		return;
	for (button = 0; button < BUTTON_COUNT; button++) {
		cx = panel->x + panel->width - 26 - button * BUTTON_SPACING;
		cy = panel->y + panel->height / 2;
		over = button_at(surface, server->pointer_x, server->pointer_y);
		draw_sign(server, command, button, cx, cy, 0, over == button && server->anim == NULL, buttons, ink);
	}
}

/*
 * Draws the application's mark (a blue rounded square with the title's
 * first letter) at x and the title after it, centred on middle.
 */
static void
draw_title(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object *surface,
	int32_t x,
	int32_t middle,
	int32_t limit,
	const float *ink)
{
	static const float mark[4] = { 0.29f, 0.55f, 1.0f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const char *title;
	char letter[2];
	int32_t width;

	/* A window without a title is called "Window". */
	title = surface->title;
	if (title[0] == '\0')
		title = "Window";

	/* The mark with the first letter, in capitals. */
	glass_draw_solid(server, command, (float)x, (float)(middle - 10), 20.0f, 20.0f, 6.0f, mark);
	letter[0] = title[0];
	letter[1] = '\0';
	if (letter[0] >= 'a' && letter[0] <= 'z')
		letter[0] = (char)(letter[0] - 'a' + 'A');
	width = glass_text_width(server, SIZE_BAR, letter);
	glass_draw_text(server, command, SIZE_BAR, x + 10 - width / 2, middle + 5, letter, 20, white);

	/* The title. */
	glass_draw_text(server, command, SIZE_TITLE, x + 30, middle + 6, title, limit, ink);
}

/*
 * Draws one button's sign centred on (cx, cy): a line (minimize), a square
 * (maximize) or two squares (restore), or the multiplication sign (close);
 * with its background when the pointer is over it (red for close).
 */
static void
draw_sign(
	struct zwl_server *server,
	VkCommandBuffer command,
	int button,
	int32_t cx,
	int32_t cy,
	unsigned restore,
	unsigned over,
	float fade,
	const float *ink)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	struct glass_shape shape;
	const float *sign;
	float hover[4];
	float colour[4];

	/* The background under the pointer. */
	sign = ink;
	if (over && button == BUTTON_CLOSE) {
		hover[0] = 0.91f;
		hover[1] = 0.30f;
		hover[2] = 0.28f;
		hover[3] = 0.95f * fade;
		glass_draw_solid(server, command, (float)(cx - BUTTON_WIDTH / 2), (float)(cy - BUTTON_HEIGHT / 2), (float)BUTTON_WIDTH, (float)BUTTON_HEIGHT, 8.0f, hover);
		sign = white;
	} else if (over) {
		hover[0] = 1.0f;
		hover[1] = 1.0f;
		hover[2] = 1.0f;
		hover[3] = 0.70f * fade;
		glass_draw_solid(server, command, (float)(cx - BUTTON_WIDTH / 2), (float)(cy - BUTTON_HEIGHT / 2), (float)BUTTON_WIDTH, (float)BUTTON_HEIGHT, 8.0f, hover);
	}

	/* The sign's color, faded. */
	memcpy(colour, sign, sizeof(colour));
	colour[3] *= fade;

	/* Minimize: a short line. */
	if (button == BUTTON_MINIMIZE) {
		glass_draw_solid(server, command, (float)(cx - 6), (float)cy - 0.75f, 12.0f, 1.5f, 0.75f, colour);
		return;
	}

	/* Maximize: a small rounded square; restore: a second one behind it. */
	if (button == BUTTON_MAXIMIZE) {
		glass_shape_init(&shape, (float)(cx - 5), (float)(cy - 5), 10.0f, 10.0f);
		if (restore) {
			shape.box[0] -= 2.0f;
			shape.box[1] += 2.0f;
			shape.box[2] = 9.0f;
			shape.box[3] = 9.0f;
		}

		/* The quad leaves room for the outline's edge. */
		shape.quad[0] -= 3.0f;
		shape.quad[1] -= 3.0f;
		shape.quad[2] += 6.0f;
		shape.quad[3] += 6.0f;
		shape.mode = MODE_RING;
		shape.radius = 2.5f;
		shape.soft = 1.4f;
		memcpy(shape.color, colour, sizeof(shape.color));
		glass_shape_draw(server, command, &shape);

		/* The square behind: its top and right edges show above and beside the front one. */
		if (restore) {
			glass_draw_solid(server, command, (float)(cx - 4), (float)(cy - 5), 9.0f, 1.4f, 0.7f, colour);
			glass_draw_solid(server, command, (float)(cx + 4) - 0.4f, (float)(cy - 5), 1.4f, 9.0f, 0.7f, colour);
		}

		/* Done. */
		return;
	}

	/* Close: the multiplication sign, centred. */
	glass_draw_glyph(server, command, SIZE_SIGN, GLASS_CLOSE_GLYPH, cx - glass_glyph_advance(server, SIZE_SIGN, GLASS_CLOSE_GLYPH) / 2, cy + 7, colour);
}

/*
 * Draws the system bar: a glass strip along the top (a little whiter while a
 * window is docked), the launcher and "zedBSD", the docked window's title and
 * buttons, the desktops, and the status at the right.
 */
static void
draw_system_bar(
	struct zwl_server *server,
	VkCommandBuffer command,
	const struct shell_bar *bar)
{
	static const float blue[4] = { 0.25f, 0.52f, 0.98f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float edge[4] = { 1.0f, 1.0f, 1.0f, 0.55f };
	static const float line[4] = { 0.12f, 0.16f, 0.24f, 0.18f };
	struct glass_shape shape;
	struct zwl_object *docked;
	int button;
	int over;

	/* The docked window, if one is on top and not moving. */
	docked = docked_window(server);

	/* The strip, with a light line under it; whiter while a window is docked, or would dock. */
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)ZWL_GLASS_BAR);
	shape.mode = MODE_GLASS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.55f;
	if (docked != NULL)
		shape.color[3] = 0.63f;
	if (server->drag != NULL && server->pointer_y < ZWL_GLASS_BAR)
		shape.color[3] = 0.75f;
	glass_shape_draw(server, command, &shape);
	glass_draw_solid(server, command, 0.0f, (float)(ZWL_GLASS_BAR - 1), (float)server->width, 1.0f, 0.0f, edge);

	/* The launcher: a blue rounded square with four small squares. */
	glass_draw_solid(server, command, 12.0f, 6.0f, 22.0f, 22.0f, 6.0f, blue);
	glass_draw_solid(server, command, 17.0f, 11.0f, 5.0f, 5.0f, 1.5f, white);
	glass_draw_solid(server, command, 24.0f, 11.0f, 5.0f, 5.0f, 1.5f, white);
	glass_draw_solid(server, command, 17.0f, 18.0f, 5.0f, 5.0f, 1.5f, white);
	glass_draw_solid(server, command, 24.0f, 18.0f, 5.0f, 5.0f, 1.5f, white);

	/* The system menu. */
	glass_draw_text(server, command, SIZE_BAR, 44, 22, "zedBSD", 200, dark);

	/* The docked window: a line, its mark and title, and its buttons with restore for maximize. */
	if (docked != NULL) {
		glass_draw_solid(server, command, (float)bar->menu_line, 9.0f, 1.0f, 16.0f, 0.0f, line);
		draw_title(server, command, docked, bar->title_x, ZWL_GLASS_BAR / 2, bar->buttons[BUTTON_MINIMIZE] - 24 - bar->title_x - 30, dark);
		over = bar_button_at(bar, server->pointer_x, server->pointer_y);
		for (button = 0; button < BUTTON_COUNT; button++)
			draw_sign(server, command, button, bar->buttons[button], ZWL_GLASS_BAR / 2, 1, over == button, 1.0f, dark);
	}

	/* The desktops, then the status. */
	draw_desktops(server, command, bar, line);
	draw_status(server, command, bar, dark);
}

/*
 * Draws the virtual desktops (a mock-up): a light pill with a small picture
 * of the wallpaper for each, the first one marked as the current one, and
 * lines on both sides.
 */
static void
draw_desktops(
	struct zwl_server *server,
	VkCommandBuffer command,
	const struct shell_bar *bar,
	const float *line)
{
	static const float pill[4] = { 1.0f, 1.0f, 1.0f, 0.45f };
	static const float current[4] = { 0.25f, 0.52f, 0.98f, 1.0f };
	struct glass_shape shape;
	int32_t x;
	int desktop;

	/* The lines, and the pill. */
	glass_draw_solid(server, command, (float)bar->desktops_line, 9.0f, 1.0f, 16.0f, 0.0f, line);
	glass_draw_solid(server, command, (float)bar->status_line, 9.0f, 1.0f, 16.0f, 0.0f, line);
	glass_draw_solid(server, command, (float)bar->desktops_x, 4.0f, (float)bar->desktops_width, (float)(ZWL_GLASS_BAR - 8), 10.0f, pill);

	/* Each desktop's picture; the others are paler. */
	for (desktop = 0; desktop < DESKTOPS; desktop++) {
		x = bar->desktops_x + 6 + desktop * (DESKTOP_WIDTH + DESKTOP_GAP);
		glass_shape_init(&shape, (float)x, (float)((ZWL_GLASS_BAR - DESKTOP_HEIGHT) / 2), (float)DESKTOP_WIDTH, (float)DESKTOP_HEIGHT);
		shape.mode = MODE_IMAGE;
		shape.opaque = 1.0f;
		shape.radius = 4.0f;
		shape.set = glass_wallpaper_set(server);
		if (desktop != 0)
			shape.opacity = 0.45f;
		glass_shape_draw(server, command, &shape);

		/* The current one is outlined in blue. */
		if (desktop == 0) {
			glass_shape_init(&shape, (float)(x - 2), (float)((ZWL_GLASS_BAR - DESKTOP_HEIGHT) / 2 - 2), (float)(DESKTOP_WIDTH + 4), (float)(DESKTOP_HEIGHT + 4));
			shape.quad[0] -= 1.0f;
			shape.quad[1] -= 1.0f;
			shape.quad[2] += 2.0f;
			shape.quad[3] += 2.0f;
			shape.mode = MODE_RING;
			shape.radius = 6.0f;
			shape.soft = 1.6f;
			memcpy(shape.color, current, sizeof(shape.color));
			glass_shape_draw(server, command, &shape);
		}
	}
}

/*
 * Draws the status at the right edge (a mock-up but for the clock): the
 * signal as four rising bars, the battery, and the date and time.
 */
static void
draw_status(
	struct zwl_server *server,
	VkCommandBuffer command,
	const struct shell_bar *bar,
	const float *ink)
{
	struct glass_shape shape;
	int step;

	/* The date and time. */
	glass_draw_text(server, command, SIZE_BAR, bar->clock_x, 22, bar->clock, 400, ink);

	/* The battery: an outline, its charge and its terminal. */
	glass_shape_init(&shape, (float)bar->battery_x, 11.0f, 22.0f, 12.0f);
	shape.quad[0] -= 1.0f;
	shape.quad[1] -= 1.0f;
	shape.quad[2] += 2.0f;
	shape.quad[3] += 2.0f;
	shape.mode = MODE_RING;
	shape.radius = 3.5f;
	shape.soft = 1.3f;
	memcpy(shape.color, ink, sizeof(shape.color));
	glass_shape_draw(server, command, &shape);
	glass_draw_solid(server, command, (float)(bar->battery_x + 3), 14.0f, 14.0f, 6.0f, 1.5f, ink);
	glass_draw_solid(server, command, (float)(bar->battery_x + 23), 15.0f, 2.0f, 4.0f, 1.0f, ink);

	/* The signal. */
	for (step = 0; step < 4; step++)
		glass_draw_solid(server, command, (float)(bar->signal_x + step * 5), (float)(19 - step * 3), 3.0f, (float)(4 + step * 3), 1.0f, ink);
}

/*
 * Shows where a dragged window would dock while the pointer is in the
 * system bar: a pale rounded rectangle with a blue edge over the space the
 * docked body would take.
 */
static void
draw_dock_hint(
	struct zwl_server *server,
	VkCommandBuffer command)
{
	static const float fill[4] = { 1.0f, 1.0f, 1.0f, 0.22f };
	static const float edge[4] = { 0.25f, 0.52f, 0.98f, 0.9f };
	struct glass_shape shape;
	struct shell_rect body;

	/* Only while a move is in the system bar. */
	if (server->drag == NULL || server->pointer_y >= ZWL_GLASS_BAR)
		return;

	/* The space, filled and outlined. */
	docked_rect(server, &body);
	glass_draw_solid(server, command, (float)(body.x + 6), (float)(body.y + 6), (float)(body.width - 12), (float)(body.height - 12), GLASS_RADIUS, fill);
	glass_shape_init(&shape, (float)(body.x + 6), (float)(body.y + 6), (float)(body.width - 12), (float)(body.height - 12));
	shape.quad[0] -= 1.0f;
	shape.quad[1] -= 1.0f;
	shape.quad[2] += 2.0f;
	shape.quad[3] += 2.0f;
	shape.mode = MODE_RING;
	shape.radius = GLASS_RADIUS;
	shape.soft = 2.0f;
	memcpy(shape.color, edge, sizeof(shape.color));
	glass_shape_draw(server, command, &shape);
}

/* How far the dock animation is, from 0 to 1, eased (1 without an animation). */
static float
animation_progress(
	struct zwl_server *server)
{
	uint64_t elapsed;
	float t;

	/* No animation is finished. */
	if (server->anim == NULL)
		return 1.0f;

	/* The time since it started, as a fraction of DOCK_MS. */
	elapsed = zwl_milliseconds() - server->anim_start_ms;
	if (elapsed >= DOCK_MS)
		return 1.0f;
	t = (float)elapsed / (float)DOCK_MS;

	/* Eased out: quick at first, slow at the end. */
	return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

/* The rectangle a fraction t of the way from one to another. */
static void
lerp_rect(
	const struct shell_rect *from,
	const struct shell_rect *to,
	float t,
	struct shell_rect *result)
{
	/* Each side moves in a straight line. */
	result->x = from->x + (int32_t)((float)(to->x - from->x) * t);
	result->y = from->y + (int32_t)((float)(to->y - from->y) * t);
	result->width = from->width + (int32_t)((float)(to->width - from->width) * t);
	result->height = from->height + (int32_t)((float)(to->height - from->height) * t);
}

/* Where a window's body is drawn: on its way while animated, the docked space, or its own place and size. */
static void
body_rect(
	struct zwl_server *server,
	const struct zwl_object *surface,
	struct shell_rect *body)
{
	struct shell_rect from;
	struct shell_rect to;
	float t;

	/* Between the two while animated. */
	if (server->anim == surface) {
		t = animation_progress(server);
		memcpy(&from, server->anim_from, sizeof(from));
		memcpy(&to, server->anim_to, sizeof(to));
		lerp_rect(&from, &to, t, body);
		return;
	}

	/* Docked. */
	if (surface->maximized) {
		docked_rect(server, body);
		return;
	}

	/* Its place, its image's size. */
	body->x = surface->x;
	body->y = surface->y;
	window_size(surface, &body->width, &body->height);
}

/* The space a docked body takes: the output under the system bar. */
static void
docked_rect(
	struct zwl_server *server,
	struct shell_rect *body)
{
	/* Edge to edge, from just under the bar to the bottom. */
	body->x = 0;
	body->y = DOCK_TOP;
	body->width = (int32_t)server->width;
	body->height = (int32_t)server->height - DOCK_TOP;
}

/* The floating title bar of a body: as wide as it, a gap above it. */
static void
floating_title(
	const struct shell_rect *body,
	struct shell_rect *panel)
{
	/* Above the body. */
	panel->x = body->x;
	panel->y = body->y - ZWL_GLASS_GAP - ZWL_GLASS_TITLE;
	panel->width = body->width;
	panel->height = ZWL_GLASS_TITLE;
}

/*
 * The place in the system bar a docking title bar slides to: its mark lands
 * where the docked title's mark is drawn, and it ends at the docked buttons.
 */
static void
bar_title_slot(
	struct zwl_server *server,
	const struct shell_bar *bar,
	struct shell_rect *slot)
{
	/* The bar's height, from the mark's place to past the buttons. */
	(void)server;
	slot->x = bar->title_x - 14;
	slot->y = 0;
	slot->width = bar->buttons[BUTTON_CLOSE] + 26 - slot->x;
	slot->height = ZWL_GLASS_BAR;
}

/* The size of a window's image. */
static void
window_size(
	const struct zwl_object *surface,
	int32_t *width,
	int32_t *height)
{
	uint32_t buffer_width;
	uint32_t buffer_height;

	/* No image, no size. */
	*width = 0;
	*height = 0;
	if (surface->current == NULL)
		return;

	/* The buffer's. */
	zwl_buffer_size(surface->current, &buffer_width, &buffer_height);
	*width = (int32_t)buffer_width;
	*height = (int32_t)buffer_height;
}

/* Tells whether a point is on a window's title bar, its body, or neither (the gap is neither). */
static enum shell_hit
window_hit(
	struct zwl_server *server,
	const struct zwl_object *surface,
	int32_t x,
	int32_t y)
{
	struct shell_rect body;
	int32_t top;

	/* Outside the window's columns. */
	body_rect(server, surface, &body);
	if (x < body.x || x >= body.x + body.width)
		return HIT_NONE;

	/* The body. */
	if (y >= body.y && y < body.y + body.height)
		return HIT_BODY;

	/* A docked window's title is in the system bar. */
	if (surface->maximized)
		return HIT_NONE;

	/* The floating title bar. */
	top = body.y - ZWL_GLASS_GAP - ZWL_GLASS_TITLE;
	if (y >= top && y < top + ZWL_GLASS_TITLE)
		return HIT_TITLE;

	/* Neither. */
	return HIT_NONE;
}

/* Returns the floating title bar button at a point, or -1. */
static int
button_at(
	const struct zwl_object *surface,
	int32_t x,
	int32_t y)
{
	int32_t cx;
	int32_t cy;
	int button;

	/* A docked window's buttons are in the bar. */
	if (surface->maximized)
		return -1;

	/* Each button's box around its centre. */
	for (button = 0; button < BUTTON_COUNT; button++) {
		button_centre(surface, button, &cx, &cy);
		if (x >= cx - BUTTON_WIDTH / 2 && x < cx + BUTTON_WIDTH / 2 &&
		    y >= cy - BUTTON_HEIGHT / 2 && y < cy + BUTTON_HEIGHT / 2)
			return button;
	}

	/* None. */
	return -1;
}

/* The centre of a floating title bar button (counted from the right edge). */
static void
button_centre(
	const struct zwl_object *surface,
	int button,
	int32_t *x,
	int32_t *y)
{
	int32_t width;
	int32_t height;

	/* From the bar's right edge, in the middle of its height. */
	window_size(surface, &width, &height);
	*x = surface->x + width - 26 - button * BUTTON_SPACING;
	*y = surface->y - ZWL_GLASS_GAP - ZWL_GLASS_TITLE / 2;
}

/* Returns the docked window's button in the system bar at a point, or -1. */
static int
bar_button_at(
	const struct shell_bar *bar,
	int32_t x,
	int32_t y)
{
	int button;

	/* Each button's box in the bar. */
	if (y < 0 || y >= ZWL_GLASS_BAR)
		return -1;
	for (button = 0; button < BUTTON_COUNT; button++) {
		if (x >= bar->buttons[button] - BUTTON_WIDTH / 2 && x < bar->buttons[button] + BUTTON_WIDTH / 2)
			return button;
	}

	/* None. */
	return -1;
}

/* Finds the topmost window whose title bar or body is at a point. */
static struct zwl_object *
window_at(
	struct zwl_server *server,
	int32_t x,
	int32_t y,
	enum shell_hit *hit)
{
	struct zwl_client *client;
	struct zwl_object *surface;
	struct zwl_object *found;
	enum shell_hit place;

	/* The hit with the highest map order. */
	found = NULL;
	*hit = HIT_NONE;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only mapped windows. */
			if (surface->kind != ZWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role)
				continue;

			/* Above what was found so far. */
			place = window_hit(server, surface, x, y);
			if (place == HIT_NONE)
				continue;
			if (found != NULL && surface->map_order < found->map_order)
				continue;
			found = surface;
			*hit = place;
		}
	}

	/* Succeeded: the window, or NULL. */
	return found;
}

/* The docked window whose title the system bar shows: the top window, docked and not moving. */
static struct zwl_object *
docked_window(
	struct zwl_server *server)
{
	struct zwl_object *top;

	/* The top window. */
	top = zwl_top_window(server);
	if (top == NULL || !top->maximized || server->anim == top)
		return NULL;

	/* Succeeded. */
	return top;
}

/* Brings a window to the top and gives it the focus. */
static void
window_raise(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	struct zwl_object *top;

	/* Already on top. */
	top = zwl_top_window(server);
	if (surface == top)
		return;

	/* The highest map order, and the focus follows. */
	server->map_order++;
	surface->map_order = server->map_order;
	server->front_surface = surface;
	zwl_seat_focus(server);
	server->dirty = 1;
}

/*
 * Docks a window to the system bar: its body takes the space under the bar
 * and it is told that size; it comes back to (restore_x, restore_y) at its
 * present size.  The change is animated.
 */
static void
window_dock(
	struct zwl_server *server,
	struct zwl_object *surface,
	int32_t restore_x,
	int32_t restore_y,
	const char *via)
{
	struct shell_rect from;
	struct shell_rect to;
	struct shell_bar bar;
	int32_t width;
	int32_t height;

	/* A docked window stays docked. */
	if (surface->maximized)
		return;

	/* The place and size to come back to, and where the body is now. */
	window_size(surface, &width, &height);
	surface->restore_x = restore_x;
	surface->restore_y = restore_y;
	surface->restore_width = (uint32_t)width;
	surface->restore_height = (uint32_t)height;
	body_rect(server, surface, &from);

	/* Docked. */
	surface->maximized = 1;
	docked_rect(server, &to);
	surface->x = to.x;
	surface->y = to.y;
	surface->window_width = (uint32_t)to.width;
	surface->window_height = (uint32_t)to.height;

	/* The animation from the floating body to the docked space. */
	memcpy(server->anim_from, &from, sizeof(server->anim_from));
	memcpy(server->anim_to, &to, sizeof(server->anim_to));
	server->anim = surface;
	server->anim_docking = 1;
	server->anim_start_ms = zwl_milliseconds();
	server->dirty = 1;

	/* The client draws the new size; the log gives where the bar's buttons are (close, restore, minimize). */
	bar_layout(server, &bar);
	printf("ZWL GLASS dock surface=%u via=%s buttons=%d,%d,%d title=%d\n", surface->id, via, bar.buttons[BUTTON_CLOSE], bar.buttons[BUTTON_MAXIMIZE], bar.buttons[BUTTON_MINIMIZE], bar.title_x);
	window_configure(surface);
}

/*
 * Brings a docked window back: its body at (x, y) at the size it had, and
 * it is told that size.  The change is animated.
 */
static void
window_undock(
	struct zwl_server *server,
	struct zwl_object *surface,
	int32_t x,
	int32_t y,
	const char *via)
{
	struct shell_rect from;
	struct shell_rect to;

	/* Only a docked window comes back. */
	if (!surface->maximized)
		return;

	/* From the docked space to the place asked for, at the size it had. */
	docked_rect(server, &from);
	surface->maximized = 0;
	surface->x = x;
	surface->y = y;
	surface->window_width = surface->restore_width;
	surface->window_height = surface->restore_height;
	to.x = x;
	to.y = y;
	to.width = (int32_t)surface->restore_width;
	to.height = (int32_t)surface->restore_height;

	/* The animation back. */
	memcpy(server->anim_from, &from, sizeof(server->anim_from));
	memcpy(server->anim_to, &to, sizeof(server->anim_to));
	server->anim = surface;
	server->anim_docking = 0;
	server->anim_start_ms = zwl_milliseconds();
	server->dirty = 1;

	/* The client draws the size it had. */
	printf("ZWL GLASS undock surface=%u via=%s x=%d y=%d\n", surface->id, via, x, y);
	window_configure(surface);
}

/* Tells a window its new size. */
static void
window_configure(
	struct zwl_object *surface)
{
	int error;

	/* A failure is reported; the window keeps drawing its old size. */
	error = zwl_window_send_configure(surface);
	if (error != 0)
		printf("ZWL GLASS configure errno=%d\n", error);
}

/*
 * Tells whether this press on a window's title is the second of a double
 * click (within DOUBLE_CLICK_MS of one on the same window); otherwise it
 * remembers this press as a first one.
 */
static unsigned
double_click(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	uint64_t now;

	/* The second press of a pair. */
	now = zwl_milliseconds();
	if (server->click_surface == surface && now - server->click_ms < DOUBLE_CLICK_MS) {
		server->click_surface = NULL;
		return 1;
	}

	/* A first press. */
	server->click_surface = surface;
	server->click_ms = now;
	return 0;
}

/*
 * Handles a press in the system bar: on the docked window's buttons their
 * action, on its title a double click (back) or the start of a pull.  The
 * launcher, the desktops and the status do nothing yet.  The press is
 * always zdesktop's.
 */
static int
bar_press(
	struct zwl_server *server)
{
	struct zwl_object *surface;
	struct shell_bar bar;
	unsigned second;
	int pressed;

	/* Only a docked window acts. */
	surface = docked_window(server);
	if (surface == NULL)
		return 1;

	/* Its buttons. */
	bar_layout(server, &bar);
	pressed = bar_button_at(&bar, server->pointer_x, server->pointer_y);
	if (pressed == BUTTON_CLOSE) {
		(void)zwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
		printf("ZWL GLASS close surface=%u\n", surface->id);
		return 1;
	}

	/* Restore brings it back where it was. */
	if (pressed == BUTTON_MAXIMIZE) {
		window_undock(server, surface, surface->restore_x, surface->restore_y, "button");
		return 1;
	}

	/* Minimize has no action yet. */
	if (pressed == BUTTON_MINIMIZE)
		return 1;

	/* Its title: between the line after "zedBSD" and the buttons. */
	if (server->pointer_x < bar.menu_line || server->pointer_x >= bar.buttons[BUTTON_MINIMIZE] - BUTTON_WIDTH / 2)
		return 1;

	/* A double click brings it back where it was. */
	second = double_click(server, surface);
	if (second) {
		window_undock(server, surface, surface->restore_x, surface->restore_y, "double-click");
		return 1;
	}

	/* A single press may become a pull. */
	server->pull = surface;
	return 1;
}
