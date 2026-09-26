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
 *
 * Wiseview (p063, plan/ws035/wiseman-design.md) is the overview of the
 * windows: dragging up from the bottom edge opens it, following the pointer
 * (how far it is open is the distance moved over WISEVIEW_DISTANCE); let go
 * past WISEVIEW_THRESHOLD it opens, otherwise it closes.  Each window moves
 * from its place to a tile of a grid over the blurred, darkened wallpaper,
 * the most recently raised first, with a glass label of its title under it.
 * A click on a tile brings that window to the top, a click elsewhere closes
 * Wiseview, and a tile's close button closes its window.
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

/*
 * The dock animation, a double click, and how far a docked title is pulled
 * down to come off (ws035-p064: the window follows the pull on the way,
 * shrinking from the docked space to its own size under the pointer).
 */
#define DOCK_MS			220U

/* The kind of the animation that is not a dock or an undock: a launched window growing from its icon (ws035-p071). */
#define ANIM_LAUNCH		2U
#define DOUBLE_CLICK_MS		400U
#define PULL_DISTANCE		140

/* A docked body starts this far under the top of the output. */
#define DOCK_TOP		(ZWL_GLASS_BAR + 4)

/* The virtual desktops: how many, and the size of each picture. */
#define DESKTOPS		4
#define DESKTOP_WIDTH		40
#define DESKTOP_HEIGHT		20
#define DESKTOP_GAP		6

/* The desktops' swipe: how near the edge it starts, how far it moves before it is one, and how long a slide takes. */
#define DESKTOP_EDGE		16
#define DESKTOP_START		12
#define DESKTOP_MS		220U

/* The keys of Ctrl+Alt+Left/Right, and the modifiers' bits (Control and Mod1). */
#define SHORTCUT_LEFT		105U
#define SHORTCUT_RIGHT		106U
#define MODIFIERS_CONTROL_ALT	(4U | 8U)
#define MODIFIER_SHIFT		1U

/* How far a Wiseview tile moves before it is dragged. */
#define TILE_DRAG_START		8

/* Wiseview: where the gesture starts, how far it goes, when it opens, and how long it settles. */
#define WISEVIEW_EDGE		20
#define WISEVIEW_DISTANCE	240.0f
#define WISEVIEW_THRESHOLD	0.35f
#define WISEVIEW_MS		200U

/* Wiseview's grid: side, top (under the header) and bottom margins, the gutter, the label under a tile. */
#define WISEVIEW_SIDE		56
#define WISEVIEW_TOP		(ZWL_GLASS_BAR + 56)
#define WISEVIEW_BOTTOM		72
#define WISEVIEW_GUTTER		24
#define WISEVIEW_LABEL		46
#define WISEVIEW_RADIUS		16.0f
#define WISEVIEW_WINDOWS	64U

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
static void pulled_rect(struct zwl_server *server, const struct zwl_object *surface, struct shell_rect *body);
static void pull_back(struct zwl_server *server);
static void window_minimize(struct zwl_server *server, struct zwl_object *surface);
static void window_to_desktop(struct zwl_server *server, struct zwl_object *surface, unsigned desktop, const char *via);
static int desktop_picture_at(struct zwl_server *server, int32_t x, int32_t y);
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
static float wiseview_progress(struct zwl_server *server);
static void wiseview_settle(struct zwl_server *server, float from, float to);
static unsigned wiseview_windows(struct zwl_server *server, struct zwl_object **windows, unsigned capacity);
static void wiseview_layout(struct zwl_server *server, struct zwl_object **windows, unsigned count, struct shell_rect *tiles);
static void draw_wiseview(struct zwl_server *server, VkCommandBuffer command, struct zwl_object **stacked, unsigned stacked_count, float progress);
static void draw_tile(struct zwl_server *server, VkCommandBuffer command, struct zwl_object *surface, const struct shell_rect *tile, float progress, unsigned current, unsigned over);
static int wiseview_button(struct zwl_server *server, uint32_t button, uint32_t state);
static void wiseview_log(struct zwl_server *server);

/* Whether where the desktops' pictures are has been logged (once, for the tests that click them). */
static unsigned shell_desktops_logged;
static float desktop_position(struct zwl_server *server);
static void desktop_turn(struct zwl_server *server, int target, const char *via);
static void desktop_release(struct zwl_server *server);
static unsigned desktop_windows(struct zwl_server *server, unsigned desktop);

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
	float progress;
	float home;
	float position;
	float shift;

	/*
	 * App Home, opening, open or closing, lies under the desktop layer,
	 * which slides aside over it with its shadow (home.c).
	 */
	home = zwl_home_progress(server);
	if (home > 0.0f) {
		zwl_home_draw(server, command, home);
		zwl_home_layer(server, home, &server->layer_x, &server->layer_y, &server->layer_scale);
		glass_shape_init(&shape, server->layer_x, server->layer_y, (float)server->width * server->layer_scale, (float)server->height * server->layer_scale);
		shape.quad[0] -= 80.0f;
		shape.quad[1] -= 80.0f;
		shape.quad[2] += 160.0f;
		shape.quad[3] += 160.0f;
		shape.mode = MODE_SHADOW;
		shape.radius = 18.0f;
		shape.soft = 36.0f;
		shape.color[0] = 0.08f;
		shape.color[1] = 0.12f;
		shape.color[2] = 0.24f;
		shape.color[3] = 0.40f * home;
		glass_shape_draw(server, command, &shape);
		server->layer_on = 1;
	}

	/* The wallpaper over the whole output (with round corners while it is pushed aside). */
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_IMAGE;
	shape.opaque = 1.0f;
	shape.set = glass_wallpaper_set(server);
	if (home > 0.0f)
		shape.radius = 18.0f;
	glass_shape_draw(server, command, &shape);

	/* The system bar's layout, which a docking title bar moves to. */
	bar_layout(server, &bar);

	/* Wiseview, opening, open or closing, takes the windows' place. */
	progress = wiseview_progress(server);
	if (progress > 0.0f) {
		draw_wiseview(server, command, windows, count, progress);
		server->layer_on = 0;
		draw_system_bar(server, command, &bar);
		return;
	}

	/*
	 * The windows; the top one has the focus; where a dragged one would
	 * dock shows just under it.  The desktop shown's windows, and while
	 * the desktops slide or are swiped, the neighbour's too, a screen's
	 * width to the side (Home, when it shows, has the desktop shown only).
	 */
	top = zwl_top_window(server);
	position = desktop_position(server);
	for (index = 0; index < count; index++) {
		shift = ((float)windows[index]->desktop - position) * (float)server->width;
		if (shift <= -(float)server->width || shift >= (float)server->width || windows[index]->minimized)
			continue;
		if (home > 0.0f && windows[index]->desktop != server->desktop)
			continue;

		/* Shifted with the layer, when Home does not have it. */
		if (home <= 0.0f) {
			server->layer_on = 0;
			if (shift != 0.0f)
				server->layer_on = 1;
			server->layer_x = shift;
			server->layer_y = 0.0f;
			server->layer_scale = 1.0f;
		}

		/* The window. */
		focused = 0;
		if (windows[index] == top)
			focused = 1;
		if (windows[index] == server->drag)
			draw_dock_hint(server, command);
		draw_window(server, command, windows[index], focused, &bar);
	}

	/* The system bar over everything but the cursor, where it always is. */
	server->layer_on = 0;
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

	/* Wiseview, open or being opened, takes every button. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving) {
		pressed = wiseview_button(server, button, state);
		return pressed;
	}

	/* App Home takes the launcher, the top-left corner, and every button while it shows. */
	pressed = zwl_home_button(server, button, state);
	if (pressed)
		return 1;

	/* The end of a press at the left or right edge: the swipe switches the desktop, or goes back. */
	if (state == 0 && server->desktop_press) {
		desktop_release(server);
		return 1;
	}

	/* A left press at the left or right edge (under the system bar) may become the desktops' swipe. */
	if (state != 0 &&
	    button == ZWL_BUTTON_LEFT &&
	    server->pointer_y >= ZWL_GLASS_BAR &&
	    (server->pointer_x < DESKTOP_EDGE || server->pointer_x >= (int32_t)server->width - DESKTOP_EDGE)) {
		server->desktop_press = 1;
		server->desktop_dragging = 0;
		server->desktop_start_x = server->pointer_x;
		server->desktop_offset = 0;
		return 1;
	}

	/* A left press at the bottom edge starts opening Wiseview. */
	if (state != 0 && button == ZWL_BUTTON_LEFT && server->pointer_y >= (int32_t)server->height - WISEVIEW_EDGE) {
		server->wiseview_gesture = 1;
		server->wiseview_start_y = server->pointer_y;
		server->wiseview_current = zwl_top_window(server);
		server->dirty = 1;
		return 1;
	}

	/* A release ends a move (docking in the system bar) or a pull. */
	if (state == 0) {
		if (server->pull != NULL) {
			pull_back(server);
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

	/* Minimize hides it. */
	if (pressed == BUTTON_MINIMIZE) {
		window_minimize(server, surface);
		return 1;
	}

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
	int32_t dx;
	int taken;

	/* The hover of buttons, the dock hint and the moves are redrawn. */
	server->dirty = 1;

	/* Wiseview follows the gesture, and hears the pointer while it is open; a pressed tile that moves is dragged. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving) {
		if (server->wiseview_press != NULL && !server->wiseview_dragging) {
			x = server->pointer_x - server->wiseview_press_x;
			y = server->pointer_y - server->wiseview_press_y;
			if (x * x + y * y >= TILE_DRAG_START * TILE_DRAG_START) {
				server->wiseview_dragging = 1;
				printf("ZWL WISEVIEW drag surface=%u\n", server->wiseview_press->id);
			}
		}

		/* The motion is Wiseview's. */
		return 1;
	}

	/* App Home follows its gesture, and hears the pointer while it shows. */
	taken = zwl_home_motion(server);
	if (taken)
		return 1;

	/* The desktops' swipe: past DESKTOP_START the windows follow the pointer (with resistance where there is no neighbour). */
	if (server->desktop_press) {
		dx = server->pointer_x - server->desktop_start_x;
		if (!server->desktop_dragging && (dx >= DESKTOP_START || dx <= -DESKTOP_START)) {
			server->desktop_dragging = 1;
			server->desktop_moving = 0;
			printf("ZWL GLASS desktop swipe\n");
		}

		/* The offset follows the pointer. */
		if (server->desktop_dragging) {
			server->desktop_offset = dx;
			if ((dx > 0 && server->desktop == 0U) || (dx < 0 && server->desktop + 1U >= (unsigned)DESKTOPS))
				server->desktop_offset = dx / 4;
		}

		/* The motion was the swipe's. */
		return 1;
	}

	/* A docked title pulled far enough down comes off under the pointer, and the move goes on. */
	surface = server->pull;
	if (surface != NULL) {
		if (surface->dead || !surface->mapped) {
			server->pull = NULL;
			return 1;
		}

		/* Not far enough yet: the window follows the pull. */
		server->pull_distance = server->pointer_y - server->pull_start_y;
		if (server->pull_distance < 0)
			server->pull_distance = 0;
		if (server->pull_distance < PULL_DISTANCE)
			return 1;
		server->pull_distance = 0;

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
 * Starts the growth of a newly mapped window out of App Home's icon, when
 * it is the window of an application Home just started (ws035-p071).
 */
void
zwl_glass_mapped(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	struct shell_rect to;
	int32_t from[4];
	int launched;

	/* Only the glass look animates, and only the window a launch waits for. */
	if (!server->glass)
		return;
	launched = zwl_home_launched(server, from);
	if (!launched)
		return;

	/* From the icon's rectangle to the window's own. */
	body_rect(server, surface, &to);
	memcpy(server->anim_from, from, sizeof(server->anim_from));
	memcpy(server->anim_to, &to, sizeof(server->anim_to));
	server->anim = surface;
	server->anim_docking = ANIM_LAUNCH;
	server->anim_start_ms = zwl_milliseconds();
	server->dirty = 1;
	printf("ZWL GLASS launch surface=%u from=%d,%d to=%d,%d\n", surface->id, from[0], from[1], to.x, to.y);
}

/*
 * Handles zdesktop's shortcuts: Ctrl+Alt+Left and Right switch to the
 * desktop before and after.  Returns 1 when the key is zdesktop's.
 */
int
zwl_glass_key(
	struct zwl_server *server,
	uint32_t key,
	uint32_t state)
{
	struct zwl_object *surface;
	int target;
	int step;

	/* Only with Control and Alt held, and only the two arrows. */
	if ((server->modifiers & MODIFIERS_CONTROL_ALT) != MODIFIERS_CONTROL_ALT)
		return 0;
	if (key != SHORTCUT_LEFT && key != SHORTCUT_RIGHT)
		return 0;

	/* With Shift, the window on top goes along to the neighbour. */
	step = 1;
	if (key == SHORTCUT_LEFT)
		step = -1;
	if (state != 0U && (server->modifiers & MODIFIER_SHIFT) != 0U) {
		surface = zwl_top_window(server);
		target = (int)server->desktop + step;
		if (surface != NULL && target >= 0 && target < DESKTOPS)
			window_to_desktop(server, surface, (unsigned)target, "key");
	}

	/* A press switches; the release is the shortcut's too. */
	if (state != 0U)
		desktop_turn(server, (int)server->desktop + step, "key");
	return 1;
}

/*
 * Keeps the output being redrawn: every frame while the dock animation runs
 * (ending it after DOCK_MS), and when the clock shows a new minute.
 */
void
zwl_glass_tick(
	struct zwl_server *server)
{
	uint64_t elapsed;
	float progress;
	time_t now;

	/* App Home's animation, and the applications it started that have ended. */
	zwl_home_tick(server);

	/* The desktops' slide draws every frame until it is done. */
	if (server->desktop_moving) {
		server->dirty = 1;
		elapsed = zwl_milliseconds() - server->desktop_start_ms;
		if (elapsed >= DESKTOP_MS) {
			server->desktop_moving = 0;
			printf("ZWL GLASS desktop settled desktop=%u windows=%u\n", server->desktop + 1U, desktop_windows(server, server->desktop));
		}
	}

	/* The animation draws every frame until it is done. */
	if (server->anim != NULL) {
		progress = animation_progress(server);
		server->dirty = 1;
		if (progress >= 1.0f)
			server->anim = NULL;
	}

	/* Wiseview draws every frame while it settles; at the end its value is where it went. */
	if (server->wiseview_moving) {
		server->dirty = 1;
		elapsed = zwl_milliseconds() - server->wiseview_start_ms;
		if (elapsed >= WISEVIEW_MS) {
			server->wiseview_moving = 0;
			server->wiseview = server->wiseview_to;
			if (server->wiseview > 0.0f)
				wiseview_log(server);
			else
				printf("ZWL WISEVIEW closed\n");
		}
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

	/* A window a launch from Home started grows out of the icon; its title bar fades in on it. */
	if (server->anim == surface && server->anim_docking == ANIM_LAUNCH) {
		t = animation_progress(server);
		draw_body(server, command, surface, &body, 0, focused);
		floating_title(&body, &panel);
		draw_title_bar(server, command, surface, &panel, t, t, focused);
		return;
	}

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

	/* A docked window being pulled has round corners and its floating title bar, fading in with the pull. */
	if (surface->maximized && surface == server->pull && server->pull_distance > 0) {
		t = (float)server->pull_distance / (float)PULL_DISTANCE;
		draw_body(server, command, surface, &body, 0, focused);
		floating_title(&body, &panel);
		draw_title_bar(server, command, surface, &panel, t, t, focused);
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
	float label[4];
	float progress;
	float home;
	int button;
	int over;

	/* The docked window, if one is on top and not moving (not while App Home shows). */
	docked = docked_window(server);
	home = zwl_home_progress(server);
	if (home > 0.0f)
		docked = NULL;

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

	/* In App Home the launcher is marked by a ring. */
	if (home > 0.0f) {
		glass_shape_init(&shape, 9.0f, 3.0f, 28.0f, 28.0f);
		shape.mode = MODE_RING;
		shape.radius = 8.0f;
		shape.soft = 2.0f;
		shape.color[0] = 0.25f;
		shape.color[1] = 0.52f;
		shape.color[2] = 0.98f;
		shape.color[3] = home;
		glass_shape_draw(server, command, &shape);
	}

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

	/* Wiseview's name where a docked title would be. */
	progress = wiseview_progress(server);
	if (progress > 0.0f) {
		memcpy(label, dark, sizeof(label));
		label[3] = progress;
		glass_draw_solid(server, command, (float)bar->menu_line, 9.0f, 1.0f, 16.0f, 0.0f, line);
		glass_draw_text(server, command, SIZE_TITLE, bar->title_x, 23, "Wiseview", 200, label);
	}

	/* The desktops, then the status. */
	draw_desktops(server, command, bar, line);
	draw_status(server, command, bar, dark);
}

/*
 * Draws the virtual desktops: a light pill with a small picture of the
 * wallpaper for each (the others paler, a dot under those with windows),
 * the one shown outlined, and lines on both sides.
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
	float progress;
	unsigned windows;
	int32_t x;
	int desktop;

	/* The lines, and the pill. */
	glass_draw_solid(server, command, (float)bar->desktops_line, 9.0f, 1.0f, 16.0f, 0.0f, line);
	glass_draw_solid(server, command, (float)bar->status_line, 9.0f, 1.0f, 16.0f, 0.0f, line);
	glass_draw_solid(server, command, (float)bar->desktops_x, 4.0f, (float)bar->desktops_width, (float)(ZWL_GLASS_BAR - 8), 10.0f, pill);

	/* Wiseview brings the desktops forward with a blue edge. */
	progress = wiseview_progress(server);
	if (progress > 0.0f) {
		glass_shape_init(&shape, (float)bar->desktops_x, 4.0f, (float)bar->desktops_width, (float)(ZWL_GLASS_BAR - 8));
		shape.quad[0] -= 1.0f;
		shape.quad[1] -= 1.0f;
		shape.quad[2] += 2.0f;
		shape.quad[3] += 2.0f;
		shape.mode = MODE_RING;
		shape.radius = 10.0f;
		shape.soft = 1.5f;
		memcpy(shape.color, current, sizeof(shape.color));
		shape.opacity = progress * 0.6f;
		glass_shape_draw(server, command, &shape);
	}

	/* Where the pictures are, once. */
	if (!shell_desktops_logged) {
		shell_desktops_logged = 1U;
		printf("ZWL GLASS desktops x=%d step=%d width=%d\n", bar->desktops_x + 6, DESKTOP_WIDTH + DESKTOP_GAP, DESKTOP_WIDTH);
	}

	/* Each desktop's picture; the others are paler. */
	for (desktop = 0; desktop < DESKTOPS; desktop++) {
		x = bar->desktops_x + 6 + desktop * (DESKTOP_WIDTH + DESKTOP_GAP);
		glass_shape_init(&shape, (float)x, (float)((ZWL_GLASS_BAR - DESKTOP_HEIGHT) / 2), (float)DESKTOP_WIDTH, (float)DESKTOP_HEIGHT);
		shape.mode = MODE_IMAGE;
		shape.opaque = 1.0f;
		shape.radius = 4.0f;
		shape.set = glass_wallpaper_set(server);
		if (desktop != (int)server->desktop)
			shape.opacity = 0.45f;
		glass_shape_draw(server, command, &shape);

		/* A desktop with windows has a small dot under its picture. */
		windows = desktop_windows(server, (unsigned)desktop);
		if (windows != 0U)
			glass_draw_solid(server, command, (float)(x + DESKTOP_WIDTH / 2 - 2), (float)(ZWL_GLASS_BAR - 5), 4.0f, 3.0f, 1.5f, current);

		/* The current one is outlined in blue. */
		if (desktop == (int)server->desktop) {
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

	/* Docked and being pulled: on its way from the docked space to its own size under the pointer. */
	if (surface->maximized && surface == server->pull && server->pull_distance > 0) {
		pulled_rect(server, surface, body);
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
			/* Only mapped windows of the desktop shown. */
			if (surface->kind != ZWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role ||
			    surface->desktop != server->desktop ||
			    surface->minimized)
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

	/* None while Wiseview shows. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving)
		return NULL;

	/* The top window. */
	top = zwl_top_window(server);
	if (top == NULL || !top->maximized || server->anim == top)
		return NULL;
	if (server->pull == top && server->pull_distance > 0)
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

	/* The client draws the new size; the log gives where the bar's buttons are (close, restore, minimize) and the docked body. */
	bar_layout(server, &bar);
	printf("ZWL GLASS dock surface=%u via=%s buttons=%d,%d,%d title=%d x=%d y=%d w=%d h=%d\n", surface->id, via,
	       bar.buttons[BUTTON_CLOSE], bar.buttons[BUTTON_MAXIMIZE], bar.buttons[BUTTON_MINIMIZE], bar.title_x,
	       (int)to.x, (int)to.y, (int)to.width, (int)to.height);
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
	int32_t picture;
	int pressed;

	/* A desktop's picture switches to it. */
	bar_layout(server, &bar);
	picture = server->pointer_x - (bar.desktops_x + 6);
	if (picture >= 0 && picture < DESKTOPS * (DESKTOP_WIDTH + DESKTOP_GAP)) {
		desktop_turn(server, picture / (DESKTOP_WIDTH + DESKTOP_GAP), "bar");
		return 1;
	}

	/* Otherwise only a docked window acts. */
	surface = docked_window(server);
	if (surface == NULL)
		return 1;

	/* Its buttons. */
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

	/* Minimize hides it. */
	if (pressed == BUTTON_MINIMIZE) {
		window_minimize(server, surface);
		return 1;
	}

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
	server->pull_start_y = server->pointer_y;
	server->pull_distance = 0;
	return 1;
}

/*
 * How far Wiseview is open, from 0 to 1: following the gesture, on its way
 * to where it settles (eased), or settled.
 */
static float
wiseview_progress(
	struct zwl_server *server)
{
	uint64_t elapsed;
	float t;
	float value;

	/* The gesture: the distance moved up from where it started. */
	if (server->wiseview_gesture) {
		value = (float)(server->wiseview_start_y - server->pointer_y) / WISEVIEW_DISTANCE;
		if (value < 0.0f)
			value = 0.0f;
		if (value > 1.0f)
			value = 1.0f;
		return value;
	}

	/* Settled. */
	if (!server->wiseview_moving)
		return server->wiseview;

	/* Settling, eased out. */
	elapsed = zwl_milliseconds() - server->wiseview_start_ms;
	t = 1.0f;
	if (elapsed < WISEVIEW_MS)
		t = (float)elapsed / (float)WISEVIEW_MS;
	t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
	return server->wiseview_from + (server->wiseview_to - server->wiseview_from) * t;
}

/* Starts Wiseview settling from one value to another (0 closed, 1 open). */
static void
wiseview_settle(
	struct zwl_server *server,
	float from,
	float to)
{
	/* The animation, drawn every frame by zwl_glass_tick. */
	server->wiseview_from = from;
	server->wiseview_to = to;
	server->wiseview_start_ms = zwl_milliseconds();
	server->wiseview_moving = 1;
	server->wiseview = from;
	server->dirty = 1;
}

/*
 * Collects the windows Wiseview shows, the most recently raised first:
 * mapped toplevel windows with an image.
 */
static unsigned
wiseview_windows(
	struct zwl_server *server,
	struct zwl_object **windows,
	unsigned capacity)
{
	struct zwl_client *client;
	struct zwl_object *surface;
	unsigned count;
	unsigned index;
	unsigned at;

	/* Every window, by falling map order. */
	count = 0;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a window with an image, of the desktop shown. */
			if (surface->kind != ZWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role ||
			    surface->current == NULL ||
			    surface->desktop != server->desktop)
				continue;

			/* Inserted after those raised later. */
			if (count == capacity)
				break;
			at = count;
			while (at > 0 && windows[at - 1]->map_order < surface->map_order)
				at--;
			for (index = count; index > at; index--)
				windows[index] = windows[index - 1];
			windows[at] = surface;
			count++;
		}
	}

	/* Succeeded. */
	return count;
}

/*
 * Lays the windows out as Wiseview's grid: 1 column for one window, 2 for up
 * to 4, 3 for up to 9, 4 beyond; each tile keeps its window's shape, fills
 * its cell at most (and at most 42 % of the output's width and 36 % of its
 * height), and sits in the middle of its cell; a short last row is centred.
 * The window that was on top is 4 % larger.
 */
static void
wiseview_layout(
	struct zwl_server *server,
	struct zwl_object **windows,
	unsigned count,
	struct shell_rect *tiles)
{
	int32_t columns;
	int32_t rows;
	int32_t width;
	int32_t height;
	int32_t cell_width;
	int32_t cell_height;
	int32_t row;
	int32_t column;
	int32_t in_row;
	int32_t row_x;
	float scale;
	float limit;
	unsigned index;

	/* The columns and rows. */
	if (count == 0)
		return;
	columns = 4;
	if (count <= 9U)
		columns = 3;
	if (count <= 4U)
		columns = 2;
	if (count == 1U)
		columns = 1;
	rows = ((int32_t)count + columns - 1) / columns;

	/* The cells, with room for the label under each tile. */
	cell_width = ((int32_t)server->width - 2 * WISEVIEW_SIDE - (columns - 1) * WISEVIEW_GUTTER) / columns;
	cell_height = ((int32_t)server->height - WISEVIEW_TOP - WISEVIEW_BOTTOM - (rows - 1) * WISEVIEW_GUTTER) / rows - WISEVIEW_LABEL;

	/* Each tile. */
	for (index = 0; index < count; index++) {
		window_size(windows[index], &width, &height);
		if (width <= 0 || height <= 0) {
			width = 1;
			height = 1;
		}

		/* The largest scale that fits the cell and the limits. */
		scale = (float)cell_width / (float)width;
		limit = (float)cell_height / (float)height;
		if (limit < scale)
			scale = limit;
		limit = 0.42f * (float)server->width / (float)width;
		if (limit < scale)
			scale = limit;
		limit = 0.36f * (float)server->height / (float)height;
		if (limit < scale)
			scale = limit;
		if (windows[index] == server->wiseview_current)
			scale *= 1.04f;

		/* Its cell; a short last row is centred. */
		row = (int32_t)index / columns;
		column = (int32_t)index % columns;
		in_row = columns;
		if (row == rows - 1)
			in_row = (int32_t)count - row * columns;
		row_x = ((int32_t)server->width - in_row * cell_width - (in_row - 1) * WISEVIEW_GUTTER) / 2;

		/* In the middle of the cell. */
		tiles[index].width = (int32_t)((float)width * scale);
		tiles[index].height = (int32_t)((float)height * scale);
		tiles[index].x = row_x + column * (cell_width + WISEVIEW_GUTTER) + (cell_width - tiles[index].width) / 2;
		tiles[index].y = WISEVIEW_TOP + row * (cell_height + WISEVIEW_LABEL + WISEVIEW_GUTTER) + (cell_height - tiles[index].height) / 2;
	}
}

/*
 * Draws Wiseview as far as it is open: the wallpaper blurred and darkened,
 * each window on its way from its place to its tile (the stacking order
 * kept, so the top window stays in front while they move), the labels, the
 * header and the footer.
 */
static void
draw_wiseview(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object **stacked,
	unsigned stacked_count,
	float progress)
{
	static const float shade[4] = { 0.0f, 0.02f, 0.06f, 0.08f };
	static const float dark[4] = { 0.10f, 0.14f, 0.22f, 1.0f };
	struct zwl_object *windows[WISEVIEW_WINDOWS];
	struct shell_rect tiles[WISEVIEW_WINDOWS];
	struct glass_shape shape;
	struct shell_rect body;
	struct shell_rect rect;
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	char header[48];
	float ink[4];
	float wash[4];
	unsigned count;
	unsigned index;
	unsigned slot;
	unsigned over;
	int32_t width;

	/* The blurred wallpaper over the sharp one, and a little darker. */
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_GLASS;
	shape.opacity = progress;
	glass_shape_draw(server, command, &shape);
	glass_draw_solid(server, command, 0.0f, 0.0f, (float)server->width, (float)server->height, 0.0f, shade);

	/* The windows and their tiles. */
	count = wiseview_windows(server, windows, WISEVIEW_WINDOWS);
	wiseview_layout(server, windows, count, tiles);

	/* In stacking order, each window between its place and its tile. */
	for (index = 0; index < stacked_count; index++) {
		/* Its tile. */
		for (slot = 0; slot < count; slot++) {
			if (windows[slot] == stacked[index])
				break;
		}

		/* A window without a tile is not shown. */
		if (slot == count)
			continue;

		/* The dragged tile is drawn last, over the others. */
		if (server->wiseview_dragging && stacked[index] == server->wiseview_press)
			continue;

		/* On its way; a minimized window's tile is washed paler. */
		body_rect(server, stacked[index], &body);
		lerp_rect(&body, &tiles[slot], progress, &rect);
		over = 0;
		if (progress >= 1.0f &&
		    server->pointer_x >= tiles[slot].x && server->pointer_x < tiles[slot].x + tiles[slot].width &&
		    server->pointer_y >= tiles[slot].y && server->pointer_y < tiles[slot].y + tiles[slot].height)
			over = 1;
		draw_tile(server, command, stacked[index], &rect, progress, stacked[index] == server->wiseview_current, over);
		if (stacked[index]->minimized) {
			memcpy(wash, white, sizeof(wash));
			wash[3] = 0.5f * progress;
			glass_draw_solid(server, command, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 16.0f, wash);
		}
	}

	/* The dragged tile follows the pointer. */
	for (slot = 0; server->wiseview_dragging && slot < count; slot++) {
		if (windows[slot] != server->wiseview_press)
			continue;
		rect = tiles[slot];
		rect.x += server->pointer_x - server->wiseview_press_x;
		rect.y += server->pointer_y - server->wiseview_press_y;
		draw_tile(server, command, windows[slot], &rect, progress, 0, 1);
	}

	/* The header: what is shown, and how many. */
	memcpy(ink, dark, sizeof(ink));
	ink[3] = progress;
	if (count == 1U)
		(void)snprintf(header, sizeof(header), "Wiseview  -  1 window");
	else
		(void)snprintf(header, sizeof(header), "Wiseview  -  %u windows", count);
	glass_draw_text(server, command, SIZE_TITLE, WISEVIEW_SIDE, ZWL_GLASS_BAR + 34, header, 400, ink);

	/* One window alone: say there are no others. */
	if (count == 1U)
		glass_draw_text(server, command, SIZE_BAR, WISEVIEW_SIDE, ZWL_GLASS_BAR + 54, "No other windows", 400, ink);

	/* The footer: a handle and how to go back. */
	ink[3] = progress * 0.35f;
	glass_draw_solid(server, command, (float)((int32_t)server->width / 2 - 24), (float)((int32_t)server->height - 44), 48.0f, 5.0f, 2.5f, ink);
	ink[3] = progress * 0.7f;
	width = glass_text_width(server, SIZE_BAR, "Swipe down to return to your window");
	glass_draw_text(server, command, SIZE_BAR, ((int32_t)server->width - width) / 2, (int32_t)server->height - 18, "Swipe down to return to your window", 400, ink);

	/* A frame of the way. */
	if (server->log_frames)
		printf("ZWL WISEVIEW frame progress=%.2f windows=%u\n", (double)progress, count);
}

/*
 * Draws one window in Wiseview: its shadow, its image with rounded corners
 * (sampled linearly, as it is smaller), a blue glow for the window that was
 * on top or the one under the pointer, its floating title bar fading as it
 * goes, and its label and (under the pointer) close button fading in.
 */
static void
draw_tile(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object *surface,
	const struct shell_rect *tile,
	float progress,
	unsigned current,
	unsigned over)
{
	static const float glow[4] = { 0.25f, 0.52f, 0.98f, 0.45f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float button[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	const struct zwl_import *image;
	struct glass_shape shape;
	struct shell_rect panel;
	int32_t label_width;
	int32_t label_x;
	int32_t label_y;
	int32_t cx;
	int32_t cy;
	float colour[4];
	float appear;

	/* The shadow, or a blue glow for the window that was on top or is under the pointer. */
	glass_shape_init(&shape, (float)tile->x, (float)tile->y + 6.0f, (float)tile->width, (float)tile->height);
	shape.quad[0] -= 48.0f;
	shape.quad[1] -= 48.0f;
	shape.quad[2] += 96.0f;
	shape.quad[3] += 96.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = WISEVIEW_RADIUS;
	shape.soft = 24.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.22f;
	if (current || over) {
		memcpy(shape.color, glow, sizeof(shape.color));
		shape.opacity = progress;
		if (over)
			shape.color[3] = 0.70f;
	}

	/* Drawn under the tile. */
	glass_shape_draw(server, command, &shape);

	/* The image, sampled linearly. */
	image = zwl_compose_surface_image(surface);
	glass_shape_init(&shape, (float)tile->x, (float)tile->y, (float)tile->width, (float)tile->height);
	shape.mode = MODE_IMAGE;
	shape.radius = WISEVIEW_RADIUS;
	shape.set = image->linear_set;
	if (shape.set == VK_NULL_HANDLE)
		shape.set = image->set;
	if (image->draw == ZWL_DRAW_OPAQUE)
		shape.opaque = 1.0f;
	glass_shape_draw(server, command, &shape);

	/* A blue edge on the window that was on top, or under the pointer. */
	if (current || over) {
		glass_shape_init(&shape, (float)(tile->x - 3), (float)(tile->y - 3), (float)(tile->width + 6), (float)(tile->height + 6));
		shape.quad[0] -= 1.0f;
		shape.quad[1] -= 1.0f;
		shape.quad[2] += 2.0f;
		shape.quad[3] += 2.0f;
		shape.mode = MODE_RING;
		shape.radius = WISEVIEW_RADIUS + 3.0f;
		shape.soft = 2.0f;
		memcpy(shape.color, glow, sizeof(shape.color));
		shape.color[3] = 0.9f;
		shape.opacity = progress;
		glass_shape_draw(server, command, &shape);
	}

	/* A floating title bar fades as the window goes. */
	if (!surface->maximized && progress < 1.0f) {
		floating_title(tile, &panel);
		draw_title_bar(server, command, surface, &panel, 1.0f - progress, 1.0f - progress, 0);
	}

	/* The label comes in over the last part of the way, when the tile is nearly in place. */
	appear = (progress - 0.6f) / 0.4f;
	if (appear <= 0.0f)
		return;

	/* The label under the tile: a glass pill with the mark and the title. */
	label_width = glass_text_width(server, SIZE_TITLE, surface->title) + 64;
	if (label_width > tile->width)
		label_width = tile->width;
	if (label_width < 120)
		label_width = 120;
	label_x = tile->x + (tile->width - label_width) / 2;
	label_y = tile->y + tile->height + 10;
	glass_shape_init(&shape, (float)label_x, (float)label_y, (float)label_width, 32.0f);
	shape.mode = MODE_GLASS;
	shape.radius = 16.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.60f;
	shape.edge = 0.8f;
	shape.opacity = appear;
	glass_shape_draw(server, command, &shape);
	memcpy(colour, dark, sizeof(colour));
	colour[3] = appear;
	draw_title(server, command, surface, label_x + 10, label_y + 16, label_width - 50, colour);

	/* Under the pointer, the close button at the top right. */
	if (!over)
		return;
	cx = tile->x + tile->width - 14;
	cy = tile->y + 14;
	glass_draw_solid(server, command, (float)(cx - 12), (float)(cy - 12), 24.0f, 24.0f, 12.0f, button);
	glass_draw_glyph(server, command, SIZE_SIGN, GLASS_CLOSE_GLYPH, cx - glass_glyph_advance(server, SIZE_SIGN, GLASS_CLOSE_GLYPH) / 2, cy + 7, dark);
}

/*
 * Handles a button while Wiseview is open or opening: the release of the
 * gesture opens or closes it; a press on a tile's close button closes that
 * window, on a tile selects it (to the top, and Wiseview closes), elsewhere
 * closes Wiseview.  Every button is zdesktop's.
 */
static int
wiseview_button(
	struct zwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct zwl_object *windows[WISEVIEW_WINDOWS];
	struct shell_rect tiles[WISEVIEW_WINDOWS];
	struct zwl_object *surface;
	unsigned count;
	unsigned index;
	float progress;
	int target;

	/* The end of the gesture: past the threshold it opens, otherwise it closes. */
	if (server->wiseview_gesture) {
		if (state != 0)
			return 1;
		progress = wiseview_progress(server);
		server->wiseview_gesture = 0;
		if (progress > WISEVIEW_THRESHOLD) {
			printf("ZWL WISEVIEW opening from=%.2f\n", (double)progress);
			wiseview_settle(server, progress, 1.0f);
		} else {
			printf("ZWL WISEVIEW cancel from=%.2f\n", (double)progress);
			wiseview_settle(server, progress, 0.0f);
		}

		/* The release was zdesktop's. */
		return 1;
	}

	/* The release of a press on a tile: a drag let go on a desktop's picture moves the window there, a click selects it. */
	if (state == 0 && server->wiseview_press != NULL) {
		surface = server->wiseview_press;
		server->wiseview_press = NULL;
		if (server->wiseview_dragging) {
			server->wiseview_dragging = 0;
			server->dirty = 1;
			target = desktop_picture_at(server, server->pointer_x, server->pointer_y);
			if (target >= 0 && (unsigned)target != surface->desktop && !surface->dead)
				window_to_desktop(server, surface, (unsigned)target, "wiseview");
			return 1;
		}

		/* A click: a minimized window comes back; it comes to the top and Wiseview closes. */
		if (surface->dead || !surface->mapped)
			return 1;
		surface->minimized = 0;
		window_raise(server, surface);
		printf("ZWL WISEVIEW select surface=%u\n", surface->id);
		wiseview_settle(server, 1.0f, 0.0f);
		return 1;
	}

	/* Only a left press on the settled Wiseview acts. */
	if (state == 0 || button != ZWL_BUTTON_LEFT || server->wiseview_moving)
		return 1;

	/* The tile under the pointer. */
	count = wiseview_windows(server, windows, WISEVIEW_WINDOWS);
	wiseview_layout(server, windows, count, tiles);
	surface = NULL;
	for (index = 0; index < count; index++) {
		if (server->pointer_x >= tiles[index].x && server->pointer_x < tiles[index].x + tiles[index].width &&
		    server->pointer_y >= tiles[index].y && server->pointer_y < tiles[index].y + tiles[index].height) {
			surface = windows[index];
			break;
		}
	}

	/* A desktop's picture in the bar switches Wiseview's desktop (the bar stays zdesktop's). */
	target = desktop_picture_at(server, server->pointer_x, server->pointer_y);
	if (surface == NULL && target >= 0) {
		desktop_turn(server, target, "wiseview");
		return 1;
	}

	/* Elsewhere, Wiseview closes. */
	if (surface == NULL) {
		printf("ZWL WISEVIEW close\n");
		wiseview_settle(server, 1.0f, 0.0f);
		return 1;
	}

	/* Its close button closes the window. */
	if (server->pointer_x >= tiles[index].x + tiles[index].width - 26 && server->pointer_y < tiles[index].y + 26) {
		(void)zwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
		printf("ZWL WISEVIEW close-window surface=%u\n", surface->id);
		return 1;
	}

	/* Otherwise the press may be a click or the tile's drag: the release decides. */
	server->wiseview_press = surface;
	server->wiseview_press_x = server->pointer_x;
	server->wiseview_press_y = server->pointer_y;
	server->wiseview_dragging = 0;
	return 1;
}

/* Reports that Wiseview is open, and where each window's tile is. */
static void
wiseview_log(
	struct zwl_server *server)
{
	struct zwl_object *windows[WISEVIEW_WINDOWS];
	struct shell_rect tiles[WISEVIEW_WINDOWS];
	unsigned count;
	unsigned index;

	/* The tiles as they are laid out now. */
	count = wiseview_windows(server, windows, WISEVIEW_WINDOWS);
	wiseview_layout(server, windows, count, tiles);
	printf("ZWL WISEVIEW open windows=%u\n", count);
	for (index = 0; index < count; index++)
		printf("ZWL WISEVIEW tile client=%llu surface=%u x=%d y=%d width=%d height=%d\n", (unsigned long long)windows[index]->client->number, windows[index]->id, tiles[index].x, tiles[index].y, tiles[index].width, tiles[index].height);
}

/* Returns where the desktops are, as a desktop number: the one shown, swiped by the pointer, or sliding. */
static float
desktop_position(
	struct zwl_server *server)
{
	uint64_t elapsed;
	float t;

	/* Swiped: the desktop shown, moved by the swipe (a swipe to the left brings the next one). */
	if (server->desktop_dragging && server->desktop_offset != 0)
		return (float)server->desktop - (float)server->desktop_offset / (float)server->width;

	/* Settled. */
	if (!server->desktop_moving)
		return (float)server->desktop;

	/* Sliding: eased (cubic ease-out) from where the desktops were to the desktop. */
	elapsed = zwl_milliseconds() - server->desktop_start_ms;
	t = (float)elapsed / (float)DESKTOP_MS;
	if (t > 1.0f)
		t = 1.0f;
	t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
	return server->desktop_from + (server->desktop_to - server->desktop_from) * t;
}

/* Switches to a desktop (clamped to those there are), sliding from where the desktops are; its top window takes the focus. */
static void
desktop_turn(
	struct zwl_server *server,
	int target,
	const char *via)
{
	float from;

	/* One of the desktops. */
	if (target < 0)
		target = 0;
	if (target >= DESKTOPS)
		target = DESKTOPS - 1;

	/* From where they are to it. */
	from = desktop_position(server);
	server->desktop = (unsigned)target;
	server->desktop_from = from;
	server->desktop_to = (float)target;
	server->desktop_start_ms = zwl_milliseconds();
	server->desktop_moving = 1;
	server->desktop_dragging = 0;
	server->desktop_offset = 0;
	server->drag = NULL;
	server->pull = NULL;
	server->dirty = 1;

	/* The focus goes to the desktop's top window (or nobody). */
	server->front_surface = zwl_top_window(server);
	zwl_seat_focus(server);
	printf("ZWL GLASS desktop=%u via=%s\n", server->desktop + 1U, via);
}

/* Ends a press at the edge: a swipe of a quarter of the output switches to the neighbour, a shorter one goes back. */
static void
desktop_release(
	struct zwl_server *server)
{
	int32_t dx;

	/* The press is over; without a swipe nothing happens. */
	server->desktop_press = 0;
	if (!server->desktop_dragging)
		return;

	/* Far enough: the neighbour on that side; else back. */
	dx = server->pointer_x - server->desktop_start_x;
	if (dx >= (int32_t)server->width / 4) {
		desktop_turn(server, (int)server->desktop - 1, "swipe");
	} else if (dx <= -(int32_t)server->width / 4) {
		desktop_turn(server, (int)server->desktop + 1, "swipe");
	} else {
		desktop_turn(server, (int)server->desktop, "swipe");
	}
}

/* Counts the mapped windows of a desktop. */
static unsigned
desktop_windows(
	struct zwl_server *server,
	unsigned desktop)
{
	struct zwl_client *client;
	struct zwl_object *surface;
	unsigned count;

	/* Every live, mapped window with a role on that desktop. */
	count = 0U;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != ZWL_SURFACE || surface->dead || !surface->mapped || surface->role == NULL || surface->cursor_role)
				continue;
			if (surface->desktop == desktop)
				count++;
		}
	}

	/* The count. */
	return count;
}

/*
 * Where a docked window being pulled is: a fraction of the way (the pull
 * over PULL_DISTANCE, eased) from the docked space to its own size under
 * the pointer, where it comes off at PULL_DISTANCE.
 */
static void
pulled_rect(
	struct zwl_server *server,
	const struct zwl_object *surface,
	struct shell_rect *body)
{
	struct shell_rect docked;
	struct shell_rect own;
	float t;

	/* The docked space. */
	docked_rect(server, &docked);

	/* Its own size, placed as the pull leaves it (the same part of the title under the pointer). */
	own.width = (int32_t)surface->restore_width;
	own.height = (int32_t)surface->restore_height;
	own.x = server->pointer_x - (int32_t)((int64_t)surface->restore_width * server->pointer_x / (int32_t)server->width);
	own.y = server->pointer_y + ZWL_GLASS_GAP + ZWL_GLASS_TITLE / 2;

	/* Eased out along the pull. */
	t = (float)server->pull_distance / (float)PULL_DISTANCE;
	if (t > 1.0f)
		t = 1.0f;
	t = 1.0f - (1.0f - t) * (1.0f - t);
	lerp_rect(&docked, &own, t, body);
}

/*
 * Ends a pull short of coming off: the window springs back from where it
 * was pulled to the docked space (the dock animation), or, not pulled at
 * all, stays.
 */
static void
pull_back(
	struct zwl_server *server)
{
	struct zwl_object *surface;
	struct shell_rect from;
	struct shell_rect to;

	/* The pull ends. */
	surface = server->pull;
	server->pull = NULL;
	if (surface == NULL || surface->dead || !surface->mapped || server->pull_distance <= 0) {
		server->pull_distance = 0;
		return;
	}

	/* From where it was pulled back to the docked space. */
	server->pull = surface;
	body_rect(server, surface, &from);
	server->pull = NULL;
	server->pull_distance = 0;
	docked_rect(server, &to);
	memcpy(server->anim_from, &from, sizeof(server->anim_from));
	memcpy(server->anim_to, &to, sizeof(server->anim_to));
	server->anim = surface;
	server->anim_docking = 1;
	server->anim_start_ms = zwl_milliseconds();
	server->dirty = 1;
	printf("ZWL GLASS pull back surface=%u\n", surface->id);
}

/* Hides a window (it keeps its place, and comes back from Wiseview); the next window takes the focus. */
static void
window_minimize(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	/* Hidden, not moved or pulled any more. */
	surface->minimized = 1;
	if (server->drag == surface)
		server->drag = NULL;
	if (server->pull == surface)
		server->pull = NULL;

	/* The focus goes to the window under it. */
	server->front_surface = zwl_top_window(server);
	zwl_seat_focus(server);
	server->dirty = 1;
	printf("ZWL GLASS minimize surface=%u\n", surface->id);
}

/* Moves a window to another desktop (shown when that desktop is), and gives the focus to the top window of the desktop shown. */
static void
window_to_desktop(
	struct zwl_server *server,
	struct zwl_object *surface,
	unsigned desktop,
	const char *via)
{
	/* The window's desktop; on top of it there. */
	surface->desktop = desktop;
	server->map_order++;
	surface->map_order = server->map_order;

	/* The focus on the desktop shown. */
	server->front_surface = zwl_top_window(server);
	zwl_seat_focus(server);
	server->dirty = 1;
	printf("ZWL GLASS move-desktop surface=%u desktop=%u via=%s\n", surface->id, desktop + 1U, via);
}

/* Returns the desktop whose picture in the system bar is under a point, or -1. */
static int
desktop_picture_at(
	struct zwl_server *server,
	int32_t x,
	int32_t y)
{
	struct shell_bar bar;
	int32_t offset;

	/* In the bar, over the pictures. */
	if (y < 0 || y >= ZWL_GLASS_BAR)
		return -1;
	bar_layout(server, &bar);
	offset = x - (bar.desktops_x + 6);
	if (offset < 0 || offset >= DESKTOPS * (DESKTOP_WIDTH + DESKTOP_GAP))
		return -1;

	/* The picture (its gap counts as its own). */
	return offset / (DESKTOP_WIDTH + DESKTOP_GAP);
}
