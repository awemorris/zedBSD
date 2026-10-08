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
 * A triple click on a floating title bar sends the window to the back and
 * gives the focus to the window now on top (ws079-p013, "go away"; a quick
 * two-finger flick up on it on a touch screen does the same, touch.c).  A
 * double click docks at the release of its second press (BUG-265: a double
 * tap whose second touch moves is a tap and drag, which moves the window;
 * BUG-179 had it dock at the press); a third press near it within
 * DOUBLE_CLICK_MS takes that dock back before it sends the window to the
 * back.
 *
 * The system bar has three zones: on the left the launcher (the Kei mark,
 * ws035-p117) and the docked window; towards the right four virtual
 * desktops; at the right edge the network, the battery and the clock.  The
 * network's icon opens its menu (network.c, ws035-p013); the battery shows
 * the charge the backend reads (ws132-p003), with a "+" while it charges;
 * a machine without a battery has none, and the icons left of it move
 * right into its place (ws132-p003, the 2026-10-05 user decision).
 *
 * A fullscreen window is kept whole (ws035-p119, the 2026-09-28 user
 * decision): while it is the highest of the windows that cover the top of
 * the output (fullscreen or docked), a window opened or raised over it
 * floats above it and the system bar stays away; the bar's place is the
 * fullscreen window's, and the bar is reached from the edges' gestures,
 * with App Home and Wiseview, which show it.
 *
 * A sheet (ws090-p014, sheet.c) is a window hung under its parent's title
 * bar: it has no title bar of its own, sits with its top at the bottom of
 * the parent's floating title bar (under the system bar when the parent is
 * docked, at the top of the output when it is fullscreen), in the middle of
 * the parent's width, and slides down out of the title bar for SHEET_MS.
 * It moves, comes to the front, hides and leaves Wiseview with its parent;
 * while it is open the parent's body takes no press (its title bar still
 * moves it) and the parent passes the focus on to it.
 *
 * Wiseview (p063, plan/ws035/wiseman-design.md) is the overview of the
 * windows: dragging up from the bottom edge opens it, following the pointer
 * (how far it is open is the distance moved over WISEVIEW_DISTANCE); let go
 * past WISEVIEW_THRESHOLD it opens, otherwise it closes.  Each window moves
 * from its place to a tile of a grid over the blurred, darkened wallpaper,
 * the most recently raised first, with a glass label of its title under it.
 * A click on a tile brings that window to the top, a click elsewhere closes
 * Wiseview, and a tile's close button closes its window.  Super+Tab opens
 * it from the keyboard (p014): Tab and the arrows move the current tile,
 * Enter chooses it, Esc closes Wiseview.
 *
 * The edges' gestures (the 2026-09-28 decision at the end of
 * plan/ws079/design-input-notes.md): from the top-left corner App Home,
 * from the top-right corner Notes (corner.c, also over Home), from the
 * bottom edge Wiseview (over Home the same swipe closes Home instead,
 * home.c).  Each counts only when it starts in its corner or edge, so a
 * stroke that starts inside a window never becomes one.  They work over a
 * fullscreen window too (kwl_glass_edge_button), except that there the
 * bottom edge's swipe takes the window back to a window instead of opening
 * Wiseview (ws099-p015).  A fullscreen window is composed like any other.
 */

#include "language.h"
#include "desktop.h"
#include "extras.h"
#include "menu.h"
#include "popup.h"
#include "titlebar.h"
#include "toplevel.h"
#include "subsurface.h"
#include "panels.h"
#include "touch.h"
#include "edit.h"
#include "ime.h"
#include "media.h"
#include "layout.h"
#include "edge.h"
#include "arrange.h"

#include <keiland/keiland.h>

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

/*
 * A floating window's frame (ws035-p128): the band this wide around the
 * title bar and the body resizes the window by the side it is on, and
 * within this far of a corner, along either side, by both sides of the
 * corner (a diagonal resize).
 */
#define FRAME_BAND		8
#define FRAME_CORNER		24

/*
 * Placing a new window (ws035-p092): how far each cascade step moves it
 * (a title bar and its gap, so the title bar under it shows), how many
 * steps are tried each way, and how near another window's corner is too
 * near.
 */
#define GLASS_CASCADE		48
#define GLASS_CASCADE_ROUNDS	8
#define GLASS_NEAR		32

/* How long a sheet takes to slide down out of its parent's title bar (ws090-p014). */
#define SHEET_MS		200U

/* Set while window_lower sends a sheet under the other windows, before its parent (ws090-p014). */
static unsigned sheet_lowering;

/*
 * The key whose press took a window out of fullscreen (BUG-194), so that
 * its release is not sent to the window either; 0 when none waits for its
 * release.  Only the event loop's thread touches it.
 */
static uint32_t fullscreen_leave_eaten;

/* The arrow whose press moved a window to another display (ws113-p007); its release is eaten too. */
static uint32_t display_move_eaten;

/* The arrow whose press turned the desktop with Alt+Shift (ws181-p010); its release is eaten too. */
static uint32_t desktop_key_eaten;

/* How much narrower than its parent's body a sheet is at least asked to be on each side, and the narrowest it is asked to be. */
#define SHEET_MARGIN		24
#define SHEET_NARROWEST		320

/* Where a title bar's first letters are, from its left edge (past the mark) and below its middle. */
#define GLASS_TITLE_START	72
#define GLASS_TITLE_LOW		8

/* The docked title's buttons in the system bar, in a pill of their own left of the desktops (ws099-p034). */
#define BAR_BUTTON_SPACING	34

/*
 * The launcher at the bar's left end: the Kei mark's square, its place and
 * its side in pixels (ws035-p117; App Home's press area, home.c, is the
 * bar's first 40 pixels).
 */
#define BAR_LAUNCHER_X		10
#define BAR_LAUNCHER_SIZE	26
#define BAR_LAUNCHER_Y		(KWL_GLASS_BAR_MIDDLE - BAR_LAUNCHER_SIZE / 2)

/*
 * The parts of the system bar placed from its middle line (ws099-p031): a
 * separator's top and length, the desktops' pill's top and height, and the
 * baseline of the bar's text (the clock, the docked title's "Wiseview").
 */
#define BAR_LINE_TOP		(KWL_GLASS_BAR_MIDDLE - 8)
#define BAR_LINE_LENGTH		16
#define BAR_PILL_TOP		(KWL_GLASS_BAR_MIDDLE - 13)
#define BAR_PILL_HEIGHT		26
#define BAR_BASELINE		(KWL_GLASS_BAR_MIDDLE + 5)

/*
 * The system bar's groups (ws099-p034, the 2026-10-06 user decisions): each
 * group is a pill this tall, darker than the bar; the status pill's icons
 * have a slot each, the pill a padding at both ends; the clock's pill pads
 * its text; the pills keep a gap between them and the output's edge.
 */
#define BAR_GROUP_HEIGHT	34
#define BAR_BUTTONS_HEIGHT	30
#define BAR_SLOT		34
#define BAR_STATUS_PAD		8
#define BAR_CLOCK_PAD		16
#define BAR_PILL_GAP		10
#define BAR_EDGE		8

/* The bar's change between a floating and a docked window's layout (ws099-p034b, ms), and the docked buttons' smallest scale. */
#define BAR_DOCK_MS		180U
#define BAR_DOCK_SCALE		0.9f

/*
 * The dock animation, a launched window's growing, a double click, and how
 * far a docked title is pulled to come off, in any direction (ws035-p064:
 * the window follows the pull on the way, shrinking from the docked space
 * to its own size under the pointer; WS181: a short pull, the touch and
 * drag of the 2026-10-07 UAT).  A double click shows the docked window within
 * DOCK_MS of its second press (BUG-179, the 2026-10-04 user's 0.1 s, at
 * most 0.2 s).
 */
#define DOCK_MS			120U
#define LAUNCH_MS		220U

/* The kind of the animation that is not a dock or an undock: a launched window growing from its icon (ws035-p071). */
#define ANIM_LAUNCH		2U
#define DOUBLE_CLICK_MS		400U
#define PULL_DISTANCE		48

/* How far from a double click's second press its third may be and still take the dock back (ws079-p013). */
#define TRIPLE_CLICK_SLOP	8

/* How far a docked window's title may be pulled before the window follows: a tap's jitter is no pull (BUG-247). */
#define PULL_SLOP		8

/*
 * How long a window brought back from the docked space is drawn at the size
 * it was sent while its client has not drawn that size (BUG-180): a client
 * that never does is shown as it draws after this.
 */
#define RESIZED_HOLD_MS		1000U

/* A docked body starts this far under the top of the output. */
#define DOCK_TOP		KWL_GLASS_DOCK_TOP

/*
 * The virtual desktops in the middle of the bar (ws099-p034): how many (the
 * left, the middle and the right one, ws181-p006), each one's slot (its
 * width and the gap after it), the pill's padding, and each desktop's mark:
 * the silhouette of a cat, a bird or a rabbit (the 2026-10-07 UAT), the
 * desktop shown in the accent and the others faint.  A mark's brightness
 * does not change with the windows (the 2026-10-06 user decision).
 */
#define DESKTOPS		((int)KWL_APPS_DESKTOPS)
#define DESKTOP_WIDTH		30
#define DESKTOP_GAP		4
#define DESKTOPS_PAD		10
#define DESKTOP_ICON		20U

/* The desktops' swipe: how near the edge it starts, how far it moves before it is one, and how long a slide takes. */
#define DESKTOP_EDGE		16
#define DESKTOP_START		12
#define DESKTOP_MS		220U

/* The keys of Ctrl+Alt+Left/Right, and the modifiers' bits (Control and Mod1). */
#define SHORTCUT_LEFT		105U
#define SHORTCUT_RIGHT		106U
#define MODIFIERS_CONTROL_ALT	(4U | 8U)
#define MODIFIER_SHIFT		1U

/* Alt+Shift with Left or Right turns the desktop too (ws181-p010, the 2026-10-08 user decision). */
#define MODIFIERS_ALT_SHIFT	(8U | 1U)

/* The Super (Windows) key's bit, for Super+Tab (Wiseview). */
#define MODIFIER_SUPER		0x40U

/*
 * The keys that take a fullscreen window back to a window (BUG-194: the
 * compositor, not the application, owns the way out of fullscreen; the
 * 2026-10-05 user decision: both): F11 alone, and Down with Super alone
 * (no Fn on keyboards whose top row sends media keys).  The modifiers that
 * keep F11 from being that key, and those that must be off with Super.
 */
#define FULLSCREEN_LEAVE_KEY	87U
#define FULLSCREEN_LEAVE_DOWN	108U
#define MODIFIERS_ANY		(1U | 4U | 8U | 0x40U)
#define MODIFIERS_NOT_SUPER	(1U | 4U | 8U)

/* App Home's corner, where its gesture starts over a fullscreen window (the same as home.c's). */
#define HOME_EDGE_CORNER	28

/* How far a Wiseview tile moves before it is dragged. */
#define TILE_DRAG_START		8

/*
 * How far a press on a title bar's menu item or search field moves before it
 * moves the window instead of clicking (ws099-p030): a mouse's or a pen's
 * press holds still, a finger wanders a little.
 */
#define PRESS_MOVE_POINTER	2
#define PRESS_MOVE_TOUCH	8

/* Wiseview: where the gesture starts, how far it goes, when it opens, and how long it settles. */
#define WISEVIEW_EDGE		20
#define WISEVIEW_DISTANCE	240.0f

/*
 * The touch pad's gestures (ws142-p003, D10): the finger travel that opens
 * Wiseview whole, and that slides the desktops by the output's width
 * (micrometres); the speed of a flick that finishes either (micrometres a
 * second).
 */
#define GESTURE_WISEVIEW_UM	40000
#define GESTURE_DESKTOP_UM	60000
#define GESTURE_FLICK		100000
#define WISEVIEW_THRESHOLD	0.35f
#define WISEVIEW_MS		200U

/*
 * The bottom edge's swipe over a fullscreen window (ws099-p015, the
 * 2026-09-30 user decision): a contact that starts in the bottom edge
 * (WISEVIEW_EDGE) and moves up this far takes the window back to a window.
 */
#define UNFULLSCREEN_DISTANCE	80

/* Wiseview's grid: side, top (under the header) and bottom margins, the gutter, the label under a tile. */
#define WISEVIEW_SIDE		56
#define WISEVIEW_TOP		(KWL_GLASS_BAR + 56)
#define WISEVIEW_BOTTOM		72
#define WISEVIEW_GUTTER		24
#define WISEVIEW_LABEL		46
#define WISEVIEW_RADIUS		16.0f
#define WISEVIEW_WINDOWS	64U

/*
 * The application IDs whose window bodies keep square corners (ws035-p134,
 * p136, the user's requests of 2026-09-29): a terminal's text runs into its
 * corners, where the rounding would cut it.  Their floating title bars stay
 * rounded.  Another (an X terminal) is one more line here.
 */
static const char *const shell_square_apps[] = {
	"terminal"
};

/* Where the pointer is over a window. */
enum shell_hit {
	HIT_NONE,
	HIT_TITLE,
	HIT_BODY,
	HIT_FRAME
};

/*
 * How far the glass's blur (backdrop.c) and a window's shadow reach from
 * a change: a window above that comes this near sees it through its glass.
 */
#define DAMAGE_REACH		96

/* A rectangle in output pixels. */
struct shell_rect {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
};

/*
 * Where a bar's parts are (ws099-p034): the clock's pill and text at the
 * right, the status pill left of it with each icon's left edge, the
 * desktops' pill in the middle or, while a window is docked, left of the
 * status pill (ws181-p009, p011; desktops_line: where the room left of it
 * ends), the docked window's buttons at the right end, and on
 * the left the launcher's line and the docked title (or the applications'
 * pill).  Every place is in the plane: the system bar's on the anchor, a
 * head's bar on its output (ws113-p015), whose slot and top it keeps.
 */
struct shell_bar {
	char clock[64];
	int32_t clock_x;
	int32_t clock_pill_x;
	int32_t clock_pill_width;
	int32_t status_x;
	int32_t status_width;
	int32_t battery_x;
	int32_t signal_x;
	int32_t volume_x;
	int32_t media_x;
	int32_t bluetooth_x;
	int32_t ime_x;
	int32_t desktops_x;
	int32_t desktops_width;
	int32_t desktops_line;
	int32_t buttons[BUTTON_COUNT];
	int32_t buttons_x;
	int32_t buttons_width;
	int32_t menu_line;
	int32_t title_x;
	int32_t top;
	unsigned output;
};

static void bar_layout(struct kwl_server *server, struct shell_bar *bar);
static void bar_layout_on(struct kwl_server *server, unsigned slot, struct shell_bar *bar);
static void draw_window(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, unsigned focused, const struct shell_bar *bar);
static int window_shown(struct kwl_server *server, struct kwl_object *surface, float home, float position);
static void window_layer(struct kwl_server *server, struct kwl_object *surface, float home, float position);
static void draw_backdrop(struct kwl_server *server, VkCommandBuffer command, struct kwl_object **windows, unsigned below, float position);
static void draw_window_blurred(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface);
static void draw_body(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, const struct shell_rect *body, unsigned focused);
static void draw_title_bar(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, const struct shell_rect *panel, float fade, float buttons, unsigned focused);
static void draw_title(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, int32_t x, int32_t middle, int32_t limit, const float *ink);
static int draw_picture_mark(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, int32_t x, int32_t middle, const float *ink);
static enum glass_hole mark_hole(struct kwl_server *server);
static void draw_letter_mark(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, const char *title, int32_t x, int32_t middle);
static int32_t title_end(struct kwl_server *server, struct kwl_object *surface, int32_t limit);
static const char *mark_name(const char *app_id);
static unsigned window_square(const struct kwl_object *surface);
static int glass_crowd(struct kwl_server *server, struct kwl_object *surface, int32_t x, int32_t y, int32_t width, int32_t height);
static void glass_clear_edges(struct kwl_server *server, int32_t width, int32_t height, int32_t *x, int32_t *y);
static int glass_top(struct kwl_server *server, struct kwl_object *surface, int32_t *x, int32_t *y);
static int glass_placed(struct kwl_server *server, struct kwl_object *surface, struct kwl_object *other);
static void shown_title(const struct kwl_object *surface, char *title, size_t size);
static void draw_sign(struct kwl_server *server, VkCommandBuffer command, int button, int32_t cx, int32_t cy, unsigned restore, unsigned over, float fade, const float *ink);
static void draw_system_bar(struct kwl_server *server, VkCommandBuffer command, const struct shell_bar *bar);
static void draw_bar_strip(struct kwl_server *server, VkCommandBuffer command, const struct glass_bar_colours *colours, int32_t x, int32_t y, int32_t width);
static void draw_bar_buttons(struct kwl_server *server, VkCommandBuffer command, const struct shell_bar *bar, struct kwl_object *docked, const struct glass_bar_colours *colours);
static void draw_bar_group_faded(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t width, int32_t height, float opacity);
static void draw_bar_group_at(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t bar_top, int32_t width, int32_t height, float opacity);
static void draw_head_bar(struct kwl_server *server, VkCommandBuffer command, unsigned slot);
static int head_bar_press(struct kwl_server *server, unsigned slot);
static void bar_dock_follow(struct kwl_server *server);
static void draw_bar_group(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t width, int32_t height);
static void draw_desktops(struct kwl_server *server, VkCommandBuffer command, const struct shell_bar *bar, const struct glass_bar_colours *colours);
static void draw_status(struct kwl_server *server, VkCommandBuffer command, const struct shell_bar *bar, const float *ink);
static void draw_home_status(struct kwl_server *server, VkCommandBuffer command, const struct shell_bar *bar, float opacity);
static int home_bar_passes(struct kwl_server *server, uint32_t state);
static void draw_battery(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t middle, int percent, unsigned charging, const float *ink);
static void draw_dock_hint(struct kwl_server *server, VkCommandBuffer command);
static float animation_progress(struct kwl_server *server);
static void lerp_rect(const struct shell_rect *from, const struct shell_rect *to, float t, struct shell_rect *result);
static void body_rect(struct kwl_server *server, const struct kwl_object *surface, struct shell_rect *body);
static void docked_rect(struct kwl_server *server, unsigned slot, struct shell_rect *body);
static unsigned window_slot(const struct kwl_object *surface);
static void glass_fit_on(struct kwl_server *server, unsigned slot, int32_t width, int32_t height, int32_t *x, int32_t *y);
static void pulled_rect(struct kwl_server *server, const struct kwl_object *surface, struct shell_rect *body);
static void pull_back(struct kwl_server *server);
static void window_minimize(struct kwl_server *server, struct kwl_object *surface);
static void window_to_desktop(struct kwl_server *server, struct kwl_object *surface, unsigned desktop, const char *via);
static int desktop_picture_at(struct kwl_server *server, int32_t x, int32_t y);
static void floating_title(const struct shell_rect *body, struct shell_rect *panel);
static void bar_title_slot(struct kwl_server *server, const struct shell_bar *bar, struct shell_rect *slot);
static void window_size(const struct kwl_object *surface, int32_t *width, int32_t *height);
static int damage_near(struct kwl_server *server, struct kwl_object *surface, const struct shell_rect *near);
static enum shell_hit window_hit(struct kwl_server *server, const struct kwl_object *surface, int32_t x, int32_t y);
static uint32_t frame_edges(struct kwl_server *server, const struct kwl_object *surface, int32_t x, int32_t y);
static uint32_t frame_under_pointer(struct kwl_server *server);
static int glass_motion_take(struct kwl_server *server);
static int button_at(const struct kwl_object *surface, int32_t x, int32_t y);
static void button_centre(const struct kwl_object *surface, int button, int32_t *x, int32_t *y);
static int bar_button_at(const struct shell_bar *bar, int32_t x, int32_t y);
static struct kwl_object *window_at(struct kwl_server *server, int32_t x, int32_t y, enum shell_hit *hit);
static struct kwl_object *docked_window(struct kwl_server *server, unsigned slot);
static void window_raise(struct kwl_server *server, struct kwl_object *surface);
static void sheet_place(struct kwl_server *server);
static struct kwl_object *sheet_owner(struct kwl_object *surface);
static void sheet_narrow(struct kwl_server *server, struct kwl_object *surface, struct kwl_object *parent, int32_t width, int32_t height);
static void sheet_anchor(struct kwl_server *server, struct kwl_object *parent, int32_t width, int32_t height, int32_t *x, int32_t *top);
static int sheet_centred(struct kwl_server *server, const struct kwl_object *parent);
static void draw_sheet(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, struct kwl_object *parent, unsigned focused);
static void window_dock(struct kwl_server *server, struct kwl_object *surface, int32_t restore_x, int32_t restore_y, const char *via);
static void window_undock(struct kwl_server *server, struct kwl_object *surface, int32_t x, int32_t y, const char *via);
static void window_configure(struct kwl_object *surface);
static void layout_window(const struct kwl_object *surface, struct kwl_layout_window *window);
static void layout_set(struct kwl_server *server, unsigned slot, unsigned mode, const char *via);
static void layout_match(struct kwl_server *server, struct kwl_object *surface, const char *via);
static int layout_hides(struct kwl_server *server, const struct kwl_object *surface);
static int layout_takes_press(struct kwl_server *server);
static void layout_leave(struct kwl_server *server, unsigned slot, struct kwl_object *front, int32_t x, int32_t y, const char *via);
static void window_float_quiet(struct kwl_server *server, struct kwl_object *surface);
static void layout_follow(struct kwl_server *server);
static void layout_follow_output(struct kwl_server *server, unsigned slot);
static void layout_owner_describe(const struct kwl_object *owner, struct kwl_layout_owner *seen);
static void layout_front_follow(struct kwl_server *server, unsigned slot);
static void layout_log_windows(struct kwl_server *server, unsigned slot);
static void docked_body(struct kwl_server *server, const struct kwl_object *surface, struct shell_rect *body);
static void dock_restore_default(struct kwl_server *server, struct kwl_object *surface);
static int window_centred_over(struct kwl_server *server, const struct kwl_object *surface);
static void draw_centred_cover(struct kwl_server *server, VkCommandBuffer command);
static unsigned double_click(struct kwl_server *server, struct kwl_object *surface);
static int title_tap_release(struct kwl_server *server);
static unsigned title_clicks(struct kwl_server *server, struct kwl_object *surface);
static int click_quick(struct kwl_server *server, struct kwl_object *surface, uint64_t now);
static int click_docked_third(struct kwl_server *server);
static void window_resized(struct kwl_object *surface);
static void window_lower(struct kwl_server *server, struct kwl_object *surface, const char *via);
static int bar_press(struct kwl_server *server, uint32_t button);
static struct kwl_object *bar_cover(struct kwl_server *server);
static void bar_cover_log(struct kwl_server *server, const struct kwl_object *cover);
static int home_without_bar(struct kwl_server *server, uint32_t button, uint32_t state);
static float wiseview_progress(struct kwl_server *server);
static void wiseview_settle(struct kwl_server *server, float from, float to);
static void wiseview_choose(struct kwl_server *server, const char *via);
static void wiseview_move(struct kwl_server *server, int step);
static int wiseview_pad_swipe(struct kwl_server *server, unsigned direction);
static int gesture_fullscreen(struct kwl_server *server, uint32_t gesture);
static unsigned gesture_as_swipe(uint32_t gesture);
static void gesture_wiseview(struct kwl_server *server, uint32_t phase, int32_t travel_um, int32_t speed);
static void gesture_desktop(struct kwl_server *server, uint32_t gesture, uint32_t phase, int32_t travel_um, int32_t speed);
static int gesture_may_start(struct kwl_server *server);
static const char *gesture_name(uint32_t gesture);
static const char *gesture_phase_name(uint32_t phase);
static int wiseview_showing(struct kwl_server *server);
static int fullscreen_leave_key(struct kwl_server *server, uint32_t key, uint32_t state);
static int display_move_key(struct kwl_server *server, uint32_t key, uint32_t state);
static int desktop_alt_shift_key(struct kwl_server *server, uint32_t key, uint32_t state);
static void wiseview_open_key(struct kwl_server *server);
static void wiseview_key(struct kwl_server *server, uint32_t key, uint32_t state);
static void wiseview_close_key(struct kwl_server *server);
static unsigned wiseview_windows(struct kwl_server *server, struct kwl_object **windows, unsigned capacity);
static void wiseview_layout(struct kwl_server *server, struct kwl_object **windows, unsigned count, struct shell_rect *tiles);
static void draw_wiseview(struct kwl_server *server, VkCommandBuffer command, struct kwl_object **stacked, unsigned stacked_count, float progress);
static void draw_tile(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, const struct shell_rect *tile, float progress, unsigned current, unsigned over);
static int wiseview_button(struct kwl_server *server, uint32_t button, uint32_t state);
static void wiseview_log(struct kwl_server *server);
static int home_edge_press(struct kwl_server *server, uint32_t button, uint32_t state);
static int band_button(struct kwl_server *server, uint32_t button, uint32_t state, int replays);
static int band_motion(struct kwl_server *server, int replays);
static int32_t band_depth(struct kwl_server *server);
static void band_replay(struct kwl_server *server, int release);
static void band_tick(struct kwl_server *server);
static void band_handback(struct kwl_server *server, int lifted);
static void draw_desktop_layer(struct kwl_server *server, VkCommandBuffer command, struct kwl_object **windows, unsigned count, float home, const struct shell_bar *bar);
static void home_layer_log(unsigned hidden);

/*
 * Whether where the desktops' pictures are has been logged, and where (for
 * the tests that click them): logged again when the pill moves, between
 * the middle and the docked place (ws181-p011).
 */
static unsigned shell_desktops_logged;
static int32_t shell_desktops_logged_x;

/*
 * Where each head's bar was last logged (ws113-p015): its desktops' pill's
 * left and its top, for the session.  A head's bar is logged when it is
 * first drawn and when it moves (the tests and the hardware's checks find
 * its parts by it); no bit set is never logged.
 */
static struct kwl_plane_places shell_head_bars_logged;

/*
 * Whether the last frame left out the desktop layer under App Home (its
 * opacity 0 with Home open, ws177-p033): the log says when that changes,
 * once each way.  0 at the start, when the layer is drawn.
 */
static unsigned shell_home_layer_hidden;

/*
 * Where the system bar's status pill and clock pill were last logged (the
 * tests press between the status's icons, ws177-p038): logged again when
 * either moves or changes width.  All 0 before the first.
 */
static int32_t shell_status_logged[3];

/*
 * The bottom edge's swipe over a fullscreen window: whether a contact that
 * began in the bottom edge holds it (until its release, even once the
 * window has left fullscreen), where it began, and whether it has taken the
 * window back already.  Only the event loop touches it.
 */
static struct {
	int pressing;
	int done;
	int32_t start_y;
} unfullscreen_swipe;
static struct kwl_object *fullscreen_top(struct kwl_server *server);
static int fullscreen_whole(struct kwl_server *server, float home);
static int unfullscreen_press(struct kwl_server *server, uint32_t button, uint32_t state);
static int unfullscreen_motion(struct kwl_server *server);
static float desktop_position(struct kwl_server *server);
static void desktop_turn(struct kwl_server *server, int target, const char *via);
static void desktop_release(struct kwl_server *server);
static unsigned desktop_windows(struct kwl_server *server, unsigned desktop);

/*
 * Draws the wallpaper, the windows from the bottom with their shadows and
 * title bars, and the system bar over them.
 */
void
kwl_glass_draw(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object **windows,
	unsigned count)
{
	static const float screen_black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	struct glass_shape shape;
	struct kwl_object *top;
	struct kwl_object *cover;
	struct shell_bar bar;
	unsigned hidden;
	int whole;
	float progress;
	int showing;
	int menu;
	float home;
	float position;

	/* A screen the closed lid put out is black (backend-host.c, ws132-p008). */
	if (server->screen_off) {
		glass_draw_solid(server, command, 0.0f, 0.0f, (float)server->width, (float)server->height, 0.0f, screen_black);
		return;
	}

	/* The login screen, or a session's lock screen, is all there is to draw (greeter.c). */
	if (server->greeter || server->locked) {
		kwl_greeter_draw(server, command);
		return;
	}

	/* The fullscreen window that keeps the system bar away this frame, if any (ws035-p119). */
	cover = bar_cover(server);
	bar_cover_log(server, cover);

	/* The menus' and the controls' places are those this frame draws them at (menu-shell.c, titlebar-shell.c). */
	kwl_menu_frame(server);
	kwl_titlebar_frame(server);

	/*
	 * App Home, opening, open or closing, lies under the desktop layer,
	 * which goes back into the distance over it with its shadow, fading out
	 * (home.c, ws181-p008).
	 */
	home = kwl_home_progress(server);
	hidden = 0U;
	if (home > 0.0f) {
		kwl_home_draw(server, command, home);
		kwl_home_layer(server, home, &server->layer_x, &server->layer_y, &server->layer_scale, &server->layer_opacity);

		/*
		 * Faded out (Home open past the layer's fade, ws177-p033): nothing
		 * of the layer would show, so none of it is drawn.
		 */
		if (server->layer_opacity <= 0.0f)
			hidden = 1U;
	}

	/* The log says when the layer is left out or drawn again. */
	home_layer_log(hidden);

	/* The layer's shadow over Home, while the layer shows. */
	if (home > 0.0f && !hidden) {
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
		shape.color[3] = 0.40f * home * server->layer_opacity;
		glass_shape_draw(server, command, &shape);
	}

	/* The layer's place over Home is where the wallpaper and the windows are drawn. */
	if (home > 0.0f)
		server->layer_on = 1;

	/*
	 * A fullscreen window on top that covers the output with an opaque
	 * image is all there is under the cursor (ws099-p015): the wallpaper,
	 * the desktop and the windows under it are not drawn.
	 */
	whole = fullscreen_whole(server, home);
	if (whole) {
		top = kwl_top_window(server);
		window_layer(server, top, home, (float)server->desktop);
		draw_window(server, command, top, 1U, NULL);
		server->layer_on = 0;
		kwl_backdrop_reset(server);
		kwl_popup_draw(server, command);
		kwl_power_dialog_draw(server, command);
		kwl_bluetooth_ask_draw(server, command);
		return;
	}

	/* The wallpaper over the whole output (with round corners while it is pushed aside), unless Home hides the layer. */
	if (!hidden) {
		glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
		shape.mode = MODE_IMAGE;
		shape.opaque = 1.0f;
		shape.set = glass_wallpaper_set(server);
		if (home > 0.0f)
			shape.radius = 18.0f;
		glass_shape_draw(server, command, &shape);
	}

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

	/* The desktop's icons and the windows, unless Home hides the layer. */
	position = desktop_position(server);
	if (!hidden)
		draw_desktop_layer(server, command, windows, count, home, &bar);

	/* The windows' popups over all the windows (popup.c); the glass from here is on the blurred wallpaper. */
	server->layer_on = 0;
	kwl_backdrop_reset(server);
	kwl_popup_draw(server, command);

	/*
	 * The system bar over everything but the cursor, where it always is,
	 * unless a fullscreen window is to be kept whole (ws035-p119).
	 */
	server->layer_on = 0;
	if (cover == NULL)
		draw_system_bar(server, command, &bar);

	/* The previews of an application's icon in the bar, over the windows (apps-bar.c), or the switcher in the middle (switcher-shell.c). */
	kwl_apps_bar_draw_popup(server, command);
	kwl_switch_draw(server, command);

	/* An open menu's popups over the system bar (menu-shell.c). */
	kwl_menu_draw_popups(server, command);

	/* The power dialog over everything (power-dialog.c, ws099-p037), and a pairing's question over that (bluetooth-ask.c, ws143-p006). */
	kwl_power_dialog_draw(server, command);
	kwl_bluetooth_ask_draw(server, command);

	/* The suggestions under a titlebar's field with the keyboard (titlebar-shell.c, ws127-p010). */
	kwl_titlebar_draw_suggestions(server, command);

	/* The network's menu, when open (network.c), and Bluetooth's (bluetooth-bar.c). */
	kwl_network_draw_menu(server, command);
	kwl_bluetooth_draw_menu(server, command);

	/*
	 * The arrangement menu, when it shows (arrange-shell.c), on the scene
	 * under it blurred as the windows' frosted panels (ws181-p007); solid
	 * panels need no blur.
	 */
	menu = kwl_arrange_showing(server);
	if (menu &&
	    home <= 0.0f &&
	    server->panels_opaque == 0U)
		draw_backdrop(server, command, windows, count, position);
	server->layer_on = 0;
	kwl_arrange_draw(server, command);
	kwl_backdrop_reset(server);

	/* The volume's popup, when open (volume.c). */
	kwl_volume_draw_popup(server, command);

	/* The top-right corner's hint, while its swipe is followed or settles (corner.c). */
	kwl_corner_draw(server, command);

	/* The notifications' popup (notify-popup.c). */
	kwl_notify_popup_draw(server, command);

	/*
	 * The on-screen keyboard over everything (keyboard.c, ws102).  Its glass
	 * shows the scene under it blurred when the compositor was started so
	 * (--keyboard-blur, ws075-p029), else the blurred wallpaper.
	 */
	showing = kwl_keyboard_showing();
	if (server->keyboard_blur && showing && home <= 0.0f)
		draw_backdrop(server, command, windows, count, position);
	kwl_keyboard_draw(server, command);
	kwl_backdrop_reset(server);

	/* A frame of the animation. */
	if (server->anim != NULL && server->log_frames)
		printf("KWL GLASS anim surface=%u docking=%u t=%.2f client=%llu\n", server->anim->id, server->anim_docking, (double)animation_progress(server), (unsigned long long)server->anim->client->number);
}

/*
 * Draws a head's windows in the glass look (heads.c's pass, ws113-p007):
 * each window of the desktop shown with its title bar, bottom to top, then
 * their popups.  App Home, Wiseview and the other screens are the
 * anchor's; a window's glass shows the blurred wallpaper (no blurred scene
 * under it on a head).  Its own bar is over them (ws113-p015, the
 * 2026-10-08 user decision: its docked window or its applications' icons,
 * the desktops, the status and the clock), and what that bar opened.
 * While App Home shows on the anchor, a head shows its background alone.
 */
void
kwl_glass_draw_head(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object **windows,
	unsigned count)
{
	struct kwl_object *top;
	struct shell_bar bar;
	unsigned focused;
	unsigned index;
	float home;
	int hidden;
	int layer;

	/* Nothing of the session's on the login or the lock screen. */
	if (server->greeter || server->locked || server->screen_off)
		return;

	/* App Home opening, open or closing: its dark stage over the head, and no window. */
	home = kwl_home_progress(server);
	if (home > 0.0f) {
		kwl_home_draw_head(server, command);
		return;
	}

	/* No layer of the anchor's (App Home's, the desktops' slide) on a head; its own bar, where a docking title bar slides to. */
	layer = server->layer_on;
	server->layer_on = 0;
	top = kwl_top_window(server);
	bar_layout_on(server, server->view_output, &bar);

	/* Each window of the desktop shown. */
	for (index = 0U; index < count; index++) {
		if (windows[index]->desktop != server->desktop || windows[index]->minimized)
			continue;
		hidden = layout_hides(server, windows[index]);
		if (hidden)
			continue;

		/* The window, the focused one lit. */
		focused = 0U;
		if (windows[index] == top)
			focused = 1U;
		draw_window(server, command, windows[index], focused, &bar);
	}

	/* The head's bar over them (ws113-p015), their popups over it (popup.c draws those of the output drawn). */
	draw_head_bar(server, command, server->view_output);
	kwl_popup_draw(server, command);

	/*
	 * What the head's bar opened, each drawn on the output it opened on:
	 * an application's previews (apps-bar.c), the shell's menus
	 * (menu-shell.c), the network's menu, the arrangement menu (on the
	 * blurred wallpaper, as a head's other glass) and the volume's popup.
	 */
	kwl_apps_bar_draw_popup(server, command);
	kwl_menu_draw_popups(server, command);
	kwl_network_draw_menu(server, command);
	kwl_bluetooth_draw_menu(server, command);
	kwl_arrange_draw(server, command);
	kwl_volume_draw_popup(server, command);
	server->layer_on = layer;
}

/*
 * Handles a pointer button in the glass look.  A press raises the window
 * under the pointer; on its title bar it starts a move, presses a button,
 * (twice) docks it or (three times) sends it to the back; on the system bar
 * it acts on the docked window.  A
 * release ends a move, docking the window when it ends in the system bar.
 * Returns 1 when the button is the compositor's, 0 when it goes to the client.
 */
int
kwl_glass_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct kwl_plane_rect output;
	struct kwl_object *surface;
	struct kwl_object *cover;
	struct kwl_object *sheet;
	enum shell_hit hit;
	uint32_t edges;
	unsigned clicks;
	float home;
	int pressed;
	int error;
	int open;
	int passes;
	int remote;

	/* The login screen takes every button (greeter.c). */
	if (server->greeter) {
		pressed = kwl_greeter_button(server, button, state);
		return pressed;
	}

	/* A double click is measured from the release before too (BUG-247, click_quick). */
	if (button == KWL_BUTTON_LEFT && state == 0)
		server->click_release_ms = kwl_milliseconds();

	/* A pairing's question, while it shows, takes every button (bluetooth-ask.c, ws143-p006). */
	pressed = kwl_bluetooth_ask_button(server, button, state);
	if (pressed)
		return 1;

	/* The power dialog, while it shows, takes every button (power-dialog.c, ws099-p037). */
	pressed = kwl_power_dialog_button(server, button, state);
	if (pressed)
		return 1;

	/* A press on a notification's board (notify-popup.c). */
	pressed = kwl_notify_popup_button(server, button, state);
	if (pressed)
		return 1;

	/* The switcher, while on, takes every button, and the release of a press it took (switcher-shell.c). */
	pressed = kwl_switch_button(server, button, state);
	if (pressed)
		return 1;

	/* Wiseview, open or being opened, takes every button. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving) {
		pressed = wiseview_button(server, button, state);
		return pressed;
	}

	/*
	 * A press with the pointer on a head (ws113-p007) is for the head's
	 * windows and its bar's widgets (ws113-p015) alone: the corners, the
	 * edges and App Home are the anchor's (their releases still come
	 * here).  A fullscreen window keeping the system bar away (cover)
	 * keeps no head's bar away.
	 */
	remote = 0;
	if (server->pointer_output != KWL_PLANE_ANCHOR && state != 0)
		remote = 1;

	/* A third quick press after a double click that docked a window takes the dock back (ws079-p013). */
	if (state != 0 && button == KWL_BUTTON_LEFT && !remote) {
		pressed = click_docked_third(server);
		if (pressed)
			return 1;
	}

	/*
	 * The top-right corner's swipe to Notes (corner.c) takes a press that
	 * starts in the corner, and that contact's release; before Home, so
	 * that it works over Home too (its corner is not Home's).
	 */
	pressed = 0;
	if (!remote)
		pressed = kwl_corner_button(server, button, state);
	if (pressed)
		return 1;

	/*
	 * The on-screen keyboard (keyboard.c, ws102) takes a press on its
	 * panel and one in a bottom corner (its swipe), before Wiseview's
	 * bottom edge and the desktops' side edges, which start outside the
	 * corners.
	 */
	if (!remote)
		pressed = kwl_keyboard_button(server, button, state);
	if (pressed)
		return 1;

	/*
	 * While a fullscreen window keeps the system bar away (ws035-p119),
	 * the bar's place is that window's: there is no launcher and no
	 * network icon to press, and App Home is reached from the top-left
	 * corner as over a fullscreen window.
	 */
	cover = bar_cover(server);

	/*
	 * A touch's press in the top edge's band waits to be the swipe down to
	 * Wiseview or a press of what is under it (WS181), before App Home and
	 * the bar's widgets take it; only where the bar is drawn.
	 */
	if ((cover == NULL && !remote) || server->band_press) {
		pressed = band_button(server, button, state, 1);
		if (pressed)
			return 1;
	}

	pressed = 0;
	if (remote) {
		/* A head's press is not App Home's, but while Home shows the head shows only its stage: nothing there to press (ws113-p015). */
		home = kwl_home_progress(server);
		if (home > 0.0f)
			pressed = 1;
	} else if (cover == NULL) {
		/*
		 * App Home takes the launcher, the top-left corner, and every button
		 * while it shows, except what goes to the status and the clock it
		 * leaves in the bar (the 2026-10-07 UAT, home_bar_passes).
		 */
		passes = home_bar_passes(server, state);
		if (!passes)
			pressed = kwl_home_button(server, button, state);
	} else {
		/* Only the corner's press, and the rest of one of Home's own presses. */
		open = home_without_bar(server, button, state);
		if (open)
			pressed = kwl_home_button(server, button, state);
	}

	/* A button Home took goes no further. */
	if (pressed)
		return 1;

	/* An open arrangement menu closes first when the press is on another widget of its bar (arrange-shell.c, ws177-p036). */
	if (cover == NULL || remote)
		(void)kwl_arrange_bar_press(server, button, state);

	/* The removable media's icon takes a press on it: Files on its devices (media.c). */
	if (cover == NULL || remote) {
		pressed = kwl_media_button(server, button, state);
		if (pressed)
			return 1;
	}

	/* The input method's indicator takes a press on it, on any output's bar: the next language (input-method.c). */
	if (cover == NULL || remote) {
		pressed = kwl_ime_indicator_button(server, button, state);
		if (pressed)
			return 1;
	}

	/* The volume takes a press on its icon on any output's bar, and every button while its popup is open (volume.c). */
	open = kwl_volume_is_open();
	if (cover == NULL || remote || open) {
		pressed = kwl_volume_button(server, button, state);
		if (pressed)
			return 1;
	}

	/* The network takes a press on its icon on any output's bar, and every button while its menu is open (network.c). */
	open = kwl_network_is_open();
	if (cover == NULL || remote || open) {
		pressed = kwl_network_button(server, button, state);
		if (pressed)
			return 1;
	}

	/* Bluetooth takes a press on its icon on any output's bar, and every button while its menu is open (bluetooth-bar.c, ws143-p006). */
	open = kwl_bluetooth_is_open();
	if (cover == NULL || remote || open) {
		pressed = kwl_bluetooth_button(server, button, state);
		if (pressed)
			return 1;
	}

	/* The arrangement menu takes a press on any output's desktops' pill, and every button while it is open (arrange-shell.c, WS181). */
	if (cover == NULL || remote) {
		pressed = kwl_arrange_button(server, button, state);
		if (pressed)
			return 1;
	}

	/* The menus take a press on a window's menu, and every button while one is open (menu-shell.c). */
	pressed = kwl_menu_button(server, button, state);
	if (pressed)
		return 1;

	/* The bars' applications take a press on an icon or a preview, and its release (apps-bar.c). */
	pressed = kwl_apps_bar_button(server, button, state);
	if (pressed)
		return 1;

	/* A titlebar's controls take a press on one, and its release (titlebar-shell.c). */
	pressed = kwl_titlebar_button(server, button, state);
	if (pressed)
		return 1;

	/* The end of a press at the left or right edge: the swipe switches the desktop, or goes back. */
	if (state == 0 && server->desktop_press) {
		desktop_release(server);
		return 1;
	}

	/* A left press at the left or right edge (under the system bar) may become the desktops' swipe. */
	if (state != 0 &&
	    button == KWL_BUTTON_LEFT &&
	    !remote &&
	    server->pointer_y >= KWL_GLASS_BAR &&
	    (server->pointer_x < DESKTOP_EDGE || server->pointer_x >= (int32_t)server->width - DESKTOP_EDGE)) {
		server->desktop_press = 1;
		server->desktop_dragging = 0;
		server->desktop_start_x = server->pointer_x;
		server->desktop_offset = 0;
		return 1;
	}

	/* A left press at the bottom edge starts the swipe up that opens App Home (WS181; Wiseview is the top edge's). */
	if (!remote)
		pressed = home_edge_press(server, button, state);
	if (pressed)
		return 1;

	/* A release ends a move (docking in the system bar) or a pull. */
	if (state == 0) {
		if (server->pull != NULL) {
			pull_back(server);
			return 1;
		}

		/* The release of a double click's second press: near it the window docks, moved further it was a move (BUG-265). */
		pressed = title_tap_release(server);
		if (pressed)
			return 1;

		/* A swap of arranged windows ends (arrange-shell.c). */
		pressed = kwl_arrange_move_end(server);
		if (pressed)
			return 1;

		/* Without a move the client has the release. */
		if (server->drag == NULL)
			return 0;

		/* The accepted move finishes without another widget receiving its release. */
		kwl_glass_toplevel_move_end(server, server->drag);

		/* The release belonged to this server-owned move. */
		return 1;
	}

	/* The system bar is the compositor's, where it is drawn (on the anchor). */
	if (server->pointer_y < KWL_GLASS_BAR && cover == NULL && !remote) {
		pressed = bar_press(server, button);
		return pressed;
	}

	/* A head's bar is too (ws113-p015). */
	if (remote) {
		(void)kwl_output_rect(server, server->pointer_output, &output);
		if (server->pointer_y < output.y + KWL_GLASS_BAR) {
			pressed = head_bar_press(server, server->pointer_output);
			return pressed;
		}
	}

	/* Only the left button acts on windows. */
	surface = window_at(server, server->pointer_x, server->pointer_y, &hit);

	/*
	 * In the docked mode a press beside a docked window of one size (on the
	 * dark rest of the docked space, where another application's windows
	 * are not shown) reaches nothing under it (ws142-p008).
	 */
	if (surface == NULL && !remote) {
		open = layout_takes_press(server);
		if (open)
			return 1;
	}

	/* A press where no window is goes to the desktop's icons when there are any (desktop.c); a window's press takes the keyboard back from them. */
	if (surface == NULL && !remote) {
		open = kwl_desktop_press(server);
		if (open)
			return 0;
	} else {
		kwl_desktop_unfocus(server);
	}

	/* A window with a sheet open takes no press but its title bar's, which still moves it (ws090-p014). */
	sheet = NULL;
	if (surface != NULL && hit != HIT_TITLE)
		sheet = kwl_sheet_of(surface);
	if (sheet != NULL) {
		window_raise(server, surface);
		printf("KWL GLASS sheet holds surface=%u client=%llu\n", surface->id, (unsigned long long)surface->client->number);
		return 1;
	}

	/* Another button is the client's on a body, the compositor's elsewhere. */
	if (button != KWL_BUTTON_LEFT)
		return hit != HIT_BODY;

	/* A press on the desktop is the compositor's. */
	if (surface == NULL)
		return 1;

	/* The window comes to the top and takes the focus. */
	window_raise(server, surface);

	/* On the body the client has the press. */
	if (hit == HIT_BODY)
		return 0;

	/* On the frame a resize by its side or corner starts, until the button is let go (toplevel.c). */
	if (hit == HIT_FRAME) {
		edges = frame_edges(server, surface, server->pointer_x, server->pointer_y);
		error = kwl_toplevel_resize_start(server, surface, edges);
		if (error != 0)
			printf("KWL GLASS frame refused surface=%u edges=%u\n", surface->id, edges);
		return 1;
	}

	/* On a button, its action. */
	pressed = button_at(surface, server->pointer_x, server->pointer_y);
	if (pressed == BUTTON_CLOSE) {
		(void)kwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
		printf("KWL GLASS close surface=%u client=%llu\n", surface->id, (unsigned long long)surface->client->number);
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

	/*
	 * A second quick press on the title bar may be a double click: its
	 * release decides (BUG-265, title_tap_release), and meanwhile it is a
	 * move as any press (a tap and drag); a third press near a double
	 * click's dock soon after takes the dock back and sends the window to
	 * the back (click_docked_third).
	 */
	clicks = title_clicks(server, surface);
	if (clicks == 2U) {
		kwl_title_tap_press(&server->title_tap, surface, server->pointer_x, server->pointer_y);
		printf("KWL GLASS double-click wait surface=%u\n", surface->id);
	}

	/* A third quick press on a title bar that did not dock sends the window to the back. */
	if (clicks >= 3U) {
		server->click_docked = NULL;
		server->click_surface = NULL;
		server->click_count = 0;
		window_lower(server, surface, "triple-click");
		return 1;
	}

	/* An arranged window's move is a swap (arrange-shell.c, WS181). */
	pressed = kwl_arrange_move_start(server, surface, server->pointer_x, server->pointer_y);
	if (pressed)
		return 1;

	/* Otherwise a move starts, from a title bar out of the system bar. */
	server->drag_left_bar = 1U;
	server->drag = surface;
	server->drag_dx = server->pointer_x - surface->x;
	server->drag_dy = server->pointer_y - surface->y;
	server->drag_start_x = surface->x;
	server->drag_start_y = surface->y;

	/* Succeeded: the press was the compositor's. */
	return 1;
}

/*
 * Moves the window being moved, or pulls a docked window out of the system
 * bar, and shows a window frame's resize arrow where the pointer is on one.
 * Returns 1 when the motion is the compositor's.
 */
int
kwl_glass_motion(
	struct kwl_server *server)
{
	uint32_t edges;
	int taken;

	/* The glass look's screens, gestures, menus and moves. */
	taken = glass_motion_take(server);

	/* Where the motion goes on to the client, a window's frame under the pointer shows its resize arrow. */
	edges = 0U;
	if (!taken)
		edges = frame_under_pointer(server);
	kwl_cursor_frame(server, edges);

	/* Succeeded: whether the motion was the compositor's. */
	return taken;
}

/*
 * Handles a pointer button over a fullscreen window (kwl_glass_fullscreen_input),
 * which the rest of the glass look does not see: only the edges' gestures
 * (the top-left corner's App Home, the top-right corner's Notes, the bottom
 * corners' keyboard, the bottom edge's swipe back to a window) and what
 * they opened.  Returns 1 when the button is the compositor's, 0 when it goes to
 * the fullscreen window.
 */
int
kwl_glass_edge_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	float home;
	int corner_press;
	int pressed;

	/* The login screen is never over a fullscreen window. */
	if (server->greeter)
		return 0;

	/* The bottom edge's swipe holds its contact until the release, also once the window has left fullscreen. */
	if (unfullscreen_swipe.pressing) {
		pressed = unfullscreen_press(server, button, state);
		return pressed;
	}

	/* Wiseview, opened from the keyboard (Super+Tab), takes every button. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving) {
		pressed = wiseview_button(server, button, state);
		return pressed;
	}

	/* The top-right corner's swipe to Notes (corner.c). */
	pressed = kwl_corner_button(server, button, state);
	if (pressed)
		return 1;

	/* The on-screen keyboard's panel and its bottom corners' swipe (keyboard.c), before the bottom edge's swipe. */
	pressed = kwl_keyboard_button(server, button, state);
	if (pressed)
		return 1;

	/* A touch's press in the top edge's band may be the swipe down to Wiseview (WS181); otherwise it is lost, not given to the window. */
	pressed = band_button(server, button, state, 0);
	if (pressed)
		return 1;

	/* App Home, when it shows or follows a press of its own, has the button as in window mode. */
	home = kwl_home_progress(server);
	if (home > 0.0f ||
	    server->home_to > 0.0f ||
	    server->home_press ||
	    server->home_page_press ||
	    server->home_rise_press) {
		pressed = kwl_home_button(server, button, state);
		return pressed;
	}

	/* A left press in the top-left corner may open App Home (the launcher is under the window). */
	corner_press = 0;
	if (state != 0 &&
	    button == KWL_BUTTON_LEFT &&
	    server->pointer_x < HOME_EDGE_CORNER &&
	    server->pointer_y < HOME_EDGE_CORNER)
		corner_press = 1;
	if (corner_press) {
		pressed = kwl_home_button(server, button, state);
		return pressed;
	}

	/* A left press at the bottom edge starts the swipe that takes the window back (not Wiseview, ws099-p015). */
	pressed = unfullscreen_press(server, button, state);
	if (pressed)
		return 1;

	/* Succeeded: anything else is the fullscreen window's. */
	return 0;
}

/*
 * Follows the edges' gestures over a fullscreen window.  Returns 1 when the
 * motion is the compositor's, 0 when it goes to the fullscreen window.
 */
int
kwl_glass_edge_motion(
	struct kwl_server *server)
{
	int taken;

	/* The login screen is never over a fullscreen window. */
	if (server->greeter)
		return 0;

	/* The bottom edge's swipe follows its contact. */
	taken = unfullscreen_motion(server);
	if (taken)
		return 1;

	/* A press held in the top edge's band (WS181). */
	taken = band_motion(server, 0);
	if (taken)
		return 1;

	/* Wiseview follows its gesture. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving) {
		server->dirty = 1;
		return 1;
	}

	/* The top-right corner's swipe (corner.c). */
	taken = kwl_corner_motion(server);
	if (taken)
		return 1;

	/* The on-screen keyboard's swipe and a press on its panel (keyboard.c). */
	taken = kwl_keyboard_motion(server);
	if (taken)
		return 1;

	/* App Home's gesture from the top-left corner (home.c). */
	taken = kwl_home_motion(server);
	if (taken)
		return 1;

	/* Succeeded: the motion is the fullscreen window's. */
	return 0;
}

/*
 * Tells whether the pointer and the fingers go to the fullscreen window
 * with only the edges' gestures for the compositor (kwl_glass_edge_button): the
 * top window is fullscreen and nothing shows over it, or the bottom edge's
 * swipe holds its contact.  seat.c chooses by it (until ws099-p015 it
 * chose by fullscreen mode, the direct scanout, which is gone).
 */
int
kwl_glass_fullscreen_input(
	struct kwl_server *server)
{
	struct kwl_object *top;

	/* The swipe's contact stays the edges' until its release. */
	if (unfullscreen_swipe.pressing)
		return 1;

	/* A fullscreen window on top. */
	top = fullscreen_top(server);
	if (top == NULL)
		return 0;

	/* Succeeded: the input is the fullscreen window's. */
	return 1;
}

/* Finds the top window when it is fullscreen with nothing shown over it; NULL otherwise. */
static struct kwl_object *
fullscreen_top(
	struct kwl_server *server)
{
	struct kwl_object *top;
	int overlay;

	/* Only the glass look's composed output, not the login or the lock screen. */
	if (!server->glass || !server->windowed)
		return NULL;
	if (server->greeter || server->locked)
		return NULL;

	/* An edge's gesture showing something (App Home, Wiseview, the hints, the keyboard) has the input as in window mode. */
	overlay = kwl_glass_overlay(server);
	if (overlay)
		return NULL;

	/* The top window, fullscreen with an image. */
	top = kwl_top_window(server);
	if (top == NULL || !top->fullscreen || top->current == NULL)
		return NULL;

	/* Succeeded: that window. */
	return top;
}

/*
 * Tells whether a frame draws only the fullscreen window on top
 * (kwl_glass_draw): with nothing shown over it or moving, it covers the
 * output with an opaque image, so what is under it is not seen.
 */
static int
fullscreen_whole(
	struct kwl_server *server,
	float home)
{
	struct kwl_object *top;
	const struct kwl_import *image;
	struct shell_rect body;
	unsigned panels;
	float progress;
	int still;

	/* The fullscreen window on top with nothing over it (fullscreen_top), App Home and Wiseview gone. */
	top = fullscreen_top(server);
	if (top == NULL || home > 0.0f || server->home_to > 0.0f)
		return 0;
	progress = wiseview_progress(server);
	if (progress > 0.0f)
		return 0;

	/* Nothing moves or is open over it: no animation, drag, sliding desktops, popup or menu (kwl_glass_still). */
	still = kwl_glass_still(server);
	if (!still)
		return 0;

	/* Its image is opaque, with no sub-surfaces under it and no glass panels. */
	image = kwl_compose_surface_image(top);
	if (image == NULL || image->draw != KWL_DRAW_OPAQUE)
		return 0;
	panels = kwl_panels_count(top);
	if (top->sub_children != NULL || panels > 0U)
		return 0;

	/* It covers the output. */
	body_rect(server, top, &body);
	if (body.x > 0 || body.y > 0 ||
	    body.x + body.width < (int32_t)server->width ||
	    body.y + body.height < (int32_t)server->height)
		return 0;

	/* Succeeded: only that window is drawn. */
	return 1;
}

/*
 * Starts and ends the bottom edge's swipe over a fullscreen window: a left
 * press in the bottom edge starts it, the release ends it.  Returns 1 when
 * the button was the swipe's.
 */
static int
unfullscreen_press(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	/* Another button while the swipe holds its contact is the swipe's too. */
	if (button != KWL_BUTTON_LEFT)
		return unfullscreen_swipe.pressing;

	/* The release ends it. */
	if (state == 0U) {
		if (!unfullscreen_swipe.pressing)
			return 0;
		unfullscreen_swipe.pressing = 0;
		printf("KWL GLASS unfullscreen-swipe end done=%d\n", unfullscreen_swipe.done);
		return 1;
	}

	/* Only a press in the bottom edge starts it: a stroke that starts above it is the window's. */
	if (server->pointer_y < (int32_t)server->height - WISEVIEW_EDGE)
		return 0;

	/* The contact is the swipe's from now on. */
	unfullscreen_swipe.pressing = 1;
	unfullscreen_swipe.done = 0;
	unfullscreen_swipe.start_y = server->pointer_y;
	printf("KWL GLASS unfullscreen-swipe start y=%d source=%d\n", server->pointer_y, (int)server->shell_source);

	/* Succeeded: the press is the swipe's. */
	return 1;
}

/*
 * Follows the bottom edge's swipe: once its contact has moved up
 * UNFULLSCREEN_DISTANCE, the fullscreen window goes back to a window, where
 * kwl_window_leave_fullscreen puts it (BUG-114's rule).  Returns 1 while the
 * swipe holds the contact.
 */
static int
unfullscreen_motion(
	struct kwl_server *server)
{
	struct kwl_object *top;
	int error;

	/* No swipe, or one that has done its work already. */
	if (!unfullscreen_swipe.pressing)
		return 0;
	if (unfullscreen_swipe.done)
		return 1;

	/* Not far enough up yet. */
	if (unfullscreen_swipe.start_y - server->pointer_y < UNFULLSCREEN_DISTANCE)
		return 1;

	/* The window that is fullscreen on top now; one that went away meanwhile ends the swipe's work. */
	unfullscreen_swipe.done = 1;
	top = fullscreen_top(server);
	if (top == NULL)
		return 1;

	/* It becomes a window again. */
	error = kwl_window_leave_fullscreen(top);
	printf("KWL GLASS unfullscreen surface=%u via=swipe errno=%d at_ms=%llu client=%llu\n", top->id, error, (unsigned long long)kwl_milliseconds(), (unsigned long long)top->client->number);

	/* Succeeded: the contact stays the swipe's until its release. */
	return 1;
}

/*
 * Tells whether an edge's gesture shows something over the windows (the
 * top-right corner's hint, App Home, Wiseview, the keyboard), so that the
 * input goes to it as in window mode even while the top window is
 * fullscreen (kwl_glass_fullscreen_input).
 */
int
kwl_glass_overlay(
	struct kwl_server *server)
{
	float progress;
	int showing;

	/* Only the glass look has the gestures. */
	if (!server->glass)
		return 0;

	/* The top-right corner's hint (corner.c). */
	showing = kwl_corner_showing();
	if (showing)
		return 1;

	/* The on-screen keyboard's panel or its swipe's hint (keyboard.c). */
	showing = kwl_keyboard_showing();
	if (showing)
		return 1;

	/* A notification's board (an urgent one shows over a fullscreen window, notify-popup.c). */
	showing = kwl_notify_popup_showing();
	if (showing)
		return 1;

	/* App Home, opening, open or closing. */
	progress = kwl_home_progress(server);
	if (progress > 0.0f || server->home_to > 0.0f)
		return 1;

	/* Wiseview's gesture once it has moved (a click at the bottom edge shows nothing). */
	if (server->wiseview_gesture) {
		progress = wiseview_progress(server);
		if (progress > 0.0f)
			return 1;
	}

	/* Wiseview, open or settling. */
	if (server->wiseview > 0.0f || server->wiseview_moving)
		return 1;

	/* Nothing shows. */
	return 0;
}

/*
 * Brings a window to the top and gives it the focus (for the menus, whose
 * press on a title bar does what a press on it does).
 */
void
kwl_glass_raise(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	/* The same as a press on the window. */
	window_raise(server, surface);
}

/*
 * Finds the topmost window whose title bar or body is at a point (for the
 * menus, whose items in a title bar another window may cover); NULL when
 * there is none.
 */
struct kwl_object *
kwl_glass_window_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_object *surface;
	enum shell_hit hit;

	/* The same search a press makes. */
	surface = window_at(server, x, y, &hit);

	/* Succeeded: the window, or NULL. */
	return surface;
}

/*
 * Tells whether the pointer has gone far enough from a press to move the
 * window rather than click (the menus' items and the search field,
 * ws099-p030): two pixels for a mouse or a pen, eight for a finger.
 */
int
kwl_glass_press_moved(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	enum kwl_contact_source source)
{
	int32_t least;
	int32_t dx;
	int32_t dy;

	/* The distance a press of its kind may wander and still click. */
	least = PRESS_MOVE_POINTER;
	if (source == KWL_CONTACT_TOUCH)
		least = PRESS_MOVE_TOUCH;

	/* How far the pointer is from the press. */
	dx = server->pointer_x - x;
	dy = server->pointer_y - y;
	if (dx * dx + dy * dy >= least * least)
		return 1;

	/* Succeeded: still a click. */
	return 0;
}

/*
 * Starts moving a window from a press on its title bar's menu item or
 * search field that went far enough (ws099-p030): a floating window follows
 * the pointer with the pressed point under it, as from a press on its title;
 * a docked window's title in the system bar starts a pull.
 */
void
kwl_glass_press_move(
	struct kwl_server *server,
	struct kwl_object *surface,
	unsigned docked,
	int32_t x,
	int32_t y)
{
	int swapping;

	/* Only a shown window of the desktop shown, and no other move or pull. */
	if (surface == NULL ||
	    surface->dead ||
	    !surface->mapped ||
	    surface->minimized ||
	    surface->desktop != server->desktop)
		return;
	if (server->drag != NULL || server->pull != NULL)
		return;

	/* A docked window's title is pulled out of the system bar, as its title is. */
	if (docked != 0U) {
		if (!surface->maximized)
			return;
		server->pull = surface;
		server->pull_start_x = x;
		server->pull_start_y = y;
		server->pull_distance = 0;
		printf("KWL GLASS press pull surface=%u\n", surface->id);
		return;
	}

	/* A floating window keeps the pressed point under the pointer; an arranged one swaps (WS181). */
	if (surface->maximized)
		return;
	swapping = kwl_arrange_move_start(server, surface, x, y);
	if (swapping)
		return;
	server->drag_left_bar = 1U;
	server->drag = surface;
	server->drag_dx = x - surface->x;
	server->drag_dy = y - surface->y;
	server->drag_start_x = surface->x;
	server->drag_start_y = surface->y;
	server->dirty = 1;

	/* Succeeded: the log line the tests read. */
	printf("KWL GLASS press move surface=%u x=%d y=%d\n", surface->id, x, y);
}

/*
 * Finds the window whose body (its image, not its titlebar) is on top at a
 * point in the glass look; NULL for none (a drag and drop's target,
 * data.c).
 */
struct kwl_object *
kwl_glass_body_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_object *surface;
	enum shell_hit hit;

	/* The same search a press makes; only a body counts. */
	surface = window_at(server, x, y, &hit);
	if (hit != HIT_BODY)
		return NULL;

	/* Succeeded: the window. */
	return surface;
}

/*
 * Finds the window whose floating title bar a press at a point would reach
 * (touch.c, which holds a finger on a title bar back a moment to see
 * whether a second one comes); NULL when the press would go anywhere else:
 * to a screen or a menu over the windows, to an edge's gesture, to the
 * system bar, to a body, or to the desktop.
 */
struct kwl_object *
kwl_glass_title_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_object *surface;
	enum shell_hit hit;
	float home;
	int fullscreen;
	int open;

	/* Only the glass look's window mode has floating title bars. */
	if (!server->glass || !server->windowed)
		return NULL;

	/* The login and lock screens, a drag and drop and a popup's grab take every press. */
	if (server->greeter || server->locked || server->dnd_active)
		return NULL;
	if (server->popup_grab != NULL)
		return NULL;

	/* A fullscreen window has no title bar; only the edges' gestures are over it. */
	fullscreen = kwl_glass_fullscreen_input(server);
	if (fullscreen)
		return NULL;

	/* Wiseview, open or being opened, takes every press. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving)
		return NULL;

	/* App Home takes every press while it shows or follows one. */
	home = kwl_home_progress(server);
	if (home > 0.0f || server->home_to > 0.0f)
		return NULL;

	/* The on-screen keyboard's panel and its bottom corners are the keyboard's (keyboard.c). */
	open = kwl_keyboard_at(x, y);
	if (open)
		return NULL;
	if (y >= (int32_t)server->height - KWL_KEYBOARD_ZONE &&
	    (x < KWL_KEYBOARD_ZONE || x >= (int32_t)server->width - KWL_KEYBOARD_ZONE))
		return NULL;

	/* An open menu closes on a press anywhere. */
	open = kwl_network_is_open();
	if (open)
		return NULL;
	open = kwl_bluetooth_is_open();
	if (open)
		return NULL;
	open = kwl_volume_is_open();
	if (open)
		return NULL;
	open = kwl_menu_is_open();
	if (open)
		return NULL;

	/* The system bar, the desktops' swipe at the side edges and Wiseview's bottom edge come before the windows. */
	if (y < KWL_GLASS_BAR)
		return NULL;
	if (x < DESKTOP_EDGE || x >= (int32_t)server->width - DESKTOP_EDGE)
		return NULL;
	if (y >= (int32_t)server->height - WISEVIEW_EDGE)
		return NULL;

	/* The topmost window at the point, when the point is on its title bar. */
	surface = window_at(server, x, y, &hit);
	if (surface == NULL || hit != HIT_TITLE)
		return NULL;

	/* Succeeded: the window whose title bar it is. */
	return surface;
}

/*
 * Sends a window to the back and gives the focus to the window now on top,
 * as a triple click on its title bar does (touch.c's two-finger flick up,
 * "go away").
 */
void
kwl_glass_lower(
	struct kwl_server *server,
	struct kwl_object *surface,
	const char *via)
{
	/* A run of clicks, or a double click's dock a third press could take back, on the window is over. */
	if (server->click_docked == surface)
		server->click_docked = NULL;
	if (server->click_surface == surface) {
		server->click_surface = NULL;
		server->click_count = 0;
	}

	/* Succeeded: the same as the triple click. */
	window_lower(server, surface, via);
}

/*
 * Draws the compositor's badge of a drag and drop without an icon of its own
 * (data.c): a small white page below and right of the pointer, with an
 * outline and two lines of text, so the user sees something being carried.
 */
void
kwl_glass_draw_drag_badge(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float edge[4] = { 0.12f, 0.16f, 0.24f, 0.35f };
	static const float page[4] = { 1.0f, 1.0f, 1.0f, 0.97f };
	float line[4];
	unsigned kept;
	float x;
	float y;

	/* Below and right of the pointer, clear of the arrow. */
	x = (float)server->pointer_x + 14.0f;
	y = (float)server->pointer_y + 16.0f;

	/* The page: its outline, then its face. */
	glass_draw_solid(server, command, x - 1.0f, y - 1.0f, 24.0f, 30.0f, 5.0f, edge);
	glass_draw_solid(server, command, x, y, 22.0f, 28.0f, 4.0f, page);

	/* Two lines of text on it, in the accent the user chose (as it is). */
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.75f, line);
	kept = kwl_accent_as_is(server);
	glass_draw_solid(server, command, x + 5.0f, y + 8.0f, 12.0f, 2.0f, 1.0f, line);
	glass_draw_solid(server, command, x + 5.0f, y + 14.0f, 9.0f, 2.0f, 1.0f, line);
	kwl_accent_done(server, kept);
}

/*
 * Draws the mark of what a drag's drop would do where the pointer is
 * (data.c decides it, ws189-p002, plan/ws189/phase001/phase.md section
 * 3.1): a round mark below and right of the pointer, over the drag's icon
 * or badge -- a green one with a plus for a copy, one of the accent with
 * three dots for a choice to be asked, a red one with a bar (no entry)
 * where the drop would be given up, and none for a move or nothing to say.
 */
void
kwl_glass_draw_drag_mark(
	struct kwl_server *server,
	VkCommandBuffer command,
	unsigned state)
{
	static const float copy_green[4] = { 0.19f, 0.69f, 0.31f, 0.95f };
	static const float refused_red[4] = { 0.82f, 0.19f, 0.19f, 0.95f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float rim[4] = { 1.0f, 1.0f, 1.0f, 0.9f };
	float accent[4];
	unsigned kept;
	float x;
	float y;

	/* The mark's centre, below and right of the pointer. */
	x = (float)server->pointer_x + 18.0f;
	y = (float)server->pointer_y + 18.0f;

	/* What to draw for the state. */
	switch (state) {
	case KWL_DND_STATE_COPY:
		/* A white rim, the green disc, and a plus. */
		glass_draw_solid(server, command, x - 10.0f, y - 10.0f, 20.0f, 20.0f, 10.0f, rim);
		glass_draw_solid(server, command, x - 9.0f, y - 9.0f, 18.0f, 18.0f, 9.0f, copy_green);
		glass_draw_solid(server, command, x - 5.0f, y - 1.0f, 10.0f, 2.0f, 1.0f, white);
		glass_draw_solid(server, command, x - 1.0f, y - 5.0f, 2.0f, 10.0f, 1.0f, white);
		break;
	case KWL_DND_STATE_ASK:
		/* A white rim, the accent's disc, and three dots. */
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.95f, accent);
		kept = kwl_accent_as_is(server);
		glass_draw_solid(server, command, x - 10.0f, y - 10.0f, 20.0f, 20.0f, 10.0f, rim);
		glass_draw_solid(server, command, x - 9.0f, y - 9.0f, 18.0f, 18.0f, 9.0f, accent);
		kwl_accent_done(server, kept);
		glass_draw_solid(server, command, x - 5.0f, y - 1.0f, 2.0f, 2.0f, 1.0f, white);
		glass_draw_solid(server, command, x - 1.0f, y - 1.0f, 2.0f, 2.0f, 1.0f, white);
		glass_draw_solid(server, command, x + 3.0f, y - 1.0f, 2.0f, 2.0f, 1.0f, white);
		break;
	case KWL_DND_STATE_REFUSED:
		/* A white rim, the red disc, and a bar across: no entry. */
		glass_draw_solid(server, command, x - 10.0f, y - 10.0f, 20.0f, 20.0f, 10.0f, rim);
		glass_draw_solid(server, command, x - 9.0f, y - 9.0f, 18.0f, 18.0f, 9.0f, refused_red);
		glass_draw_solid(server, command, x - 5.0f, y - 1.5f, 10.0f, 3.0f, 1.0f, white);
		break;
	default:
		/* A move, or nothing to say: no mark. */
		break;
	}
}

/*
 * Tells whether the glass look is still (ws035-p055): nothing moves or
 * fades by itself -- no window animation, move, pull or drag, no Home or
 * Wiseview, no desktop sliding, no see-through bodies -- so that a change
 * can be drawn in a rectangle of its own.
 */
int
kwl_glass_still(
	struct kwl_server *server)
{
	struct kwl_object *popups[1];
	float home;
	unsigned open;

	/* Something moves (a drag and drop's icon or badge too). */
	if (server->anim != NULL || server->drag != NULL || server->pull != NULL || server->dnd_active)
		return 0;
	if (server->desktop_moving || server->desktop_dragging)
		return 0;
	if (server->wiseview > 0.0f || server->wiseview_gesture || server->wiseview_moving)
		return 0;

	/* Home, even closing. */
	home = kwl_home_progress(server);
	if (home > 0.0f)
		return 0;

	/* The top-right corner's hint (corner.c). */
	open = (unsigned)kwl_corner_showing();
	if (open)
		return 0;

	/* A notification's board on the screen (notify-popup.c). */
	open = (unsigned)kwl_notify_popup_showing();
	if (open)
		return 0;

	/* The on-screen keyboard (keyboard.c). */
	open = (unsigned)kwl_keyboard_showing();
	if (open)
		return 0;

	/* A see-through body shows what is under it through its glass. */
	if (server->window_opacity < 1.0f)
		return 0;

	/* A popup or a menu open over the windows. */
	open = kwl_popup_collect(server, popups, 1U);
	if (open > 0U)
		return 0;
	open = kwl_menu_is_open();
	if (open)
		return 0;
	open = (unsigned)kwl_network_is_open();
	if (open)
		return 0;
	open = (unsigned)kwl_bluetooth_is_open();
	if (open)
		return 0;
	open = (unsigned)kwl_volume_is_open();
	if (open)
		return 0;

	/* Still. */
	return 1;
}

/*
 * Finds the rectangle (left, top, right, bottom) a window's body alone
 * covers, when a change of its image can be drawn there alone: the look is
 * still, the window is shown, and no window above it comes near enough
 * for its glass to blur the change in (ws035-p055).  Returns 1 with the
 * rectangle, 0 when the whole output must be drawn.
 */
int
kwl_glass_body_damage(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t *rect)
{
	struct kwl_client *client;
	struct kwl_object *other;
	struct shell_rect body;
	struct shell_rect near;
	int still;
	int close;

	/* A still look, and a window on the desktop shown. */
	still = kwl_glass_still(server);
	if (!still || !surface->mapped || surface->minimized || surface->desktop != server->desktop)
		return 0;

	/* The body, and the reach of the glass's blur around it. */
	body_rect(server, surface, &body);
	near.x = body.x - DAMAGE_REACH;
	near.y = body.y - DAMAGE_REACH;
	near.width = body.width + 2 * DAMAGE_REACH;
	near.height = body.height + 2 * DAMAGE_REACH;

	/* No window above it within the reach (its body and its title bar). */
	for (client = server->clients; client != NULL; client = client->next) {
		for (other = client->objects; other != NULL; other = other->next) {
			if (other == surface || other->kind != KWL_SURFACE || !other->mapped)
				continue;
			if (other->map_order < surface->map_order || other->minimized || other->desktop != server->desktop)
				continue;
			close = damage_near(server, other, &near);
			if (close)
				return 0;
		}
	}

	/* Succeeded: the body alone. */
	rect[0] = body.x;
	rect[1] = body.y;
	rect[2] = body.x + body.width;
	rect[3] = body.y + body.height;
	return 1;
}

/*
 * Tells whether the pointer at a point is over a window's body (its
 * client's own area) in a still look, where the compositor draws nothing that
 * follows the pointer but the cursor (ws035-p055).
 */
int
kwl_glass_pointer_calm(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_object *surface;
	enum shell_hit hit;
	int still;

	/* A still look. */
	still = kwl_glass_still(server);
	if (!still)
		return 0;

	/* Over a body. */
	surface = window_at(server, x, y, &hit);
	if (surface == NULL || hit != HIT_BODY)
		return 0;

	/* Calm. */
	return 1;
}

/*
 * Finds where a window's body (its image) is drawn now: at its place, in
 * the docked space, or on its way while animated (for its popups, popup.c).
 * Returns 0.
 */
int
kwl_glass_body_origin(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t *x,
	int32_t *y)
{
	struct shell_rect body;

	/* The rectangle the body is drawn in. */
	body_rect(server, surface, &body);

	/* Succeeded: its top-left corner. */
	*x = body.x;
	*y = body.y;
	return 0;
}

/*
 * Carries out what a toplevel asks of the shell (ws035-p076): a move the
 * client started from its own title bar, maximize (dock) and unmaximize,
 * and minimize.
 */
void
kwl_glass_toplevel_request(
	struct kwl_server *server,
	struct kwl_object *surface,
	int request)
{
	struct kwl_object *front;
	int32_t x;
	int32_t y;
	int swapping;

	/* Only a shown window of the desktop shown. */
	if (surface->dead || !surface->mapped || surface->desktop != server->desktop)
		return;

	/* What was asked. */
	switch (request) {
	case KWL_TOPLEVEL_MOVE:
		/* A move as a press on the title bar starts one, until the button is let go; none while another goes on. */
		if (server->drag != NULL || server->pull != NULL)
			break;

		/*
		 * A docked window drawing its own title (no title of it in the
		 * system bar to pull) comes off as a pull does (WS181): floating,
		 * every other window too, its title's top under the pointer.
		 */
		if (surface->maximized) {
			x = server->pointer_x - (int32_t)((int64_t)surface->restore_width * server->pointer_x / (int32_t)server->width);
			y = server->pointer_y - KWL_GLASS_GAP;
			layout_leave(server, window_slot(surface), surface, x, y, "request-move");
		}

		window_raise(server, surface);

		/* An arranged window's move is a swap (WS181). */
		swapping = kwl_arrange_move_start(server, surface, server->pointer_x, server->pointer_y);
		if (swapping)
			break;

		server->drag_left_bar = 1U;
		server->drag = surface;
		server->drag_dx = server->pointer_x - surface->x;
		server->drag_dy = server->pointer_y - surface->y;
		server->drag_start_x = surface->x;
		server->drag_start_y = surface->y;
		printf("KWL GLASS request move surface=%u\n", surface->id);
		break;
	case KWL_TOPLEVEL_MAXIMIZE:
		/* Docked where it is. */
		window_dock(server, surface, surface->x, surface->y, "request");
		break;
	case KWL_TOPLEVEL_UNMAXIMIZE:
		/*
		 * Back to its place before it docked: the docked window in front of
		 * its output ends that output's docked mode, another one hidden
		 * behind it only floats again (WS181: an application behind does
		 * not end the mode).
		 */
		if (!surface->maximized)
			break;
		front = sheet_owner(kwl_output_top_window(server, window_slot(surface)));
		if (surface != front) {
			window_float_quiet(server, surface);
			break;
		}
		layout_leave(server, window_slot(surface), surface, surface->restore_x, surface->restore_y, "request");
		break;
	case KWL_TOPLEVEL_MINIMIZE:
		/* Hidden until Wiseview brings it back. */
		window_minimize(server, surface);
		break;
	default:
		break;
	}
}

/*
 * Ends an accepted move without dispatching its release through unrelated widgets.
 */
void
kwl_glass_toplevel_move_end(
    struct kwl_server *server,
    struct kwl_object *surface)
{
	struct kwl_plane_rect output;
	unsigned start;

	/* Only the borrowed window currently moving can complete this operation. */
	if (surface == NULL || server->drag != surface)
		return;

	/* Retires the moving identity before docking or emitting diagnostics. */
	server->drag = NULL;
	(void)kwl_output_rect(server, server->pointer_output, &output);
	if (server->pointer_y < output.y + KWL_GLASS_BAR &&
	    server->drag_left_bar &&
	    server->pointer_output == surface->output) {
		/* A move from another output comes back where it was let go, on this one. */
		start = kwl_output_at(server, server->drag_start_x, server->drag_start_y);
		if (start != server->pointer_output) {
			server->drag_start_x = surface->x;
			server->drag_start_y = surface->y;
		}

		/* Docked there. */
		window_dock(server, surface, server->drag_start_x, server->drag_start_y, "drag");

		/* The bar (the system bar, or a head's, ws113-p015) keeps the previous position as the restore point. */
		return;
	}

	/* A move ending elsewhere retains the last position reached by its motion. */
	printf("KWL GLASS moved surface=%u x=%d y=%d client=%llu\n", surface->id, surface->x, surface->y, (unsigned long long)surface->client->number);

	/* Succeeded: no button, regardless of its physical code, remains a move owner. */
	return;
}

/*
 * Places a new window in the glass look (ws035-p092).  The places tried, in
 * order: the centre of the space below the system bar, then down and right
 * of the top window a title bar's height at a time, then from the space's
 * top-left corner the same way, each kept inside the space.  The first
 * place that hides no other window's title (the start of its title bar)
 * and is near no other window's corner is taken; when every place hides
 * some, the one that hides the fewest.
 */
void
kwl_glass_place(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t width,
	int32_t height,
	int32_t step)
{
	int32_t places[1 + 2 * GLASS_CASCADE_ROUNDS][2];
	struct kwl_layout_window window;
	unsigned slot;
	int centred;
	int32_t space_width;
	int32_t space_height;
	int32_t top_x;
	int32_t top_y;
	unsigned count;
	unsigned best;
	unsigned index;
	int crowd;
	int least;
	int found;
	int round;

	/* The centre of the space for bodies under the system bar and a title bar (the plain look's cascade step is not used). */
	(void)step;
	kwl_glass_space(server, &space_width, &space_height);
	places[0][0] = ((int32_t)server->width - width) / 2;
	places[0][1] = KWL_GLASS_TOP + (space_height - height) / 2;
	count = 1U;

	/* In the docked mode a dialog shows in the middle of the screen, over its docked parent (ws142-p008). */
	layout_window(surface, &window);
	slot = window_slot(surface);
	centred = kwl_layout_centred(server->layout_mode[slot], &window);
	if (centred) {
		kwl_glass_fit(server, width, height, &places[0][0], &places[0][1]);
		surface->x = places[0][0];
		surface->y = places[0][1];
		return;
	}

	/* Down and right of the top window. */
	found = glass_top(server, surface, &top_x, &top_y);
	for (round = 1; found && round <= GLASS_CASCADE_ROUNDS; round++) {
		places[count][0] = top_x + round * GLASS_CASCADE;
		places[count][1] = top_y + round * GLASS_CASCADE;
		count++;
	}

	/* Down and right of the space's top-left corner. */
	for (round = 0; round < GLASS_CASCADE_ROUNDS; round++) {
		places[count][0] = KWL_GLASS_MARGIN + round * GLASS_CASCADE;
		places[count][1] = KWL_GLASS_TOP + round * GLASS_CASCADE;
		count++;
	}

	/* The first place that hides nothing, else the one that hides least. */
	best = 0U;
	least = -1;
	for (index = 0U; index < count; index++) {
		kwl_glass_fit(server, width, height, &places[index][0], &places[index][1]);
		glass_clear_edges(server, width, height, &places[index][0], &places[index][1]);
		crowd = glass_crowd(server, surface, places[index][0], places[index][1], width, height);
		if (least < 0 || crowd < least) {
			least = crowd;
			best = index;
		}

		/* Nothing hidden: taken. */
		if (crowd == 0)
			break;
	}

	/* Succeeded: the place. */
	surface->x = places[best][0];
	surface->y = places[best][1];
}

/*
 * Moves a new window's place so that its frame is clear of the screen's
 * edge gestures, which take a press before any frame (frame_under_pointer):
 * its bottom band above Wiseview's edge and its side bands inside the
 * desktops' edges, so each side and corner can be dragged (BUG-121: a
 * window placed down to the bottom margin could not be resized from its
 * bottom corners, and dragging one opened Wiseview).  A window too large
 * for that keeps the place it has.
 */
static void
glass_clear_edges(
	struct kwl_server *server,
	int32_t width,
	int32_t height,
	int32_t *x,
	int32_t *y)
{
	int32_t lowest;
	int32_t leftmost;
	int32_t rightmost;

	/* The bottom band above Wiseview's edge, unless the title bar would go under the system bar. */
	lowest = (int32_t)server->height - WISEVIEW_EDGE - FRAME_BAND - height;
	if (*y > lowest && lowest >= KWL_GLASS_TOP)
		*y = lowest;

	/* The side bands inside the desktops' edges. */
	leftmost = DESKTOP_EDGE + FRAME_BAND;
	rightmost = (int32_t)server->width - DESKTOP_EDGE - FRAME_BAND - width;

	/* A window too wide for both keeps its place across. */
	if (rightmost < leftmost)
		return;

	/* Otherwise it comes in from whichever side it was over. */
	if (*x < leftmost)
		*x = leftmost;
	if (*x > rightmost)
		*x = rightmost;

	/* Succeeded: the place clears the edges it can. */
	return;
}


/*
 * Counts what a new window at a place would hide of the other windows of
 * the desktop shown: each title whose first letters (right of its mark)
 * it covers counts one, each corner within GLASS_NEAR of its own counts
 * four.
 */
static int
glass_crowd(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	struct kwl_client *client;
	struct kwl_object *other;
	struct shell_rect body;
	int32_t title_x;
	int32_t title_y;
	int32_t dx;
	int32_t dy;
	int placed;
	int crowd;

	/* Each shown window. */
	crowd = 0;
	for (client = server->clients; client != NULL; client = client->next) {
		for (other = client->objects; other != NULL; other = other->next) {
			placed = glass_placed(server, surface, other);
			if (!placed)
				continue;
			body_rect(server, other, &body);

			/* Its title's first letters (their lower half), under the new window and its title bar. */
			title_x = body.x + GLASS_TITLE_START;
			title_y = body.y - KWL_GLASS_GAP - KWL_GLASS_TITLE / 2 + GLASS_TITLE_LOW;
			if (title_x >= x && title_x < x + width && title_y >= y - KWL_GLASS_GAP - KWL_GLASS_TITLE && title_y < y + height)
				crowd++;

			/* Its corner, near the new one's. */
			dx = body.x - x;
			dy = body.y - y;
			if (dx > -GLASS_NEAR && dx < GLASS_NEAR && dy > -GLASS_NEAR && dy < GLASS_NEAR)
				crowd += 4;
		}
	}

	/* Succeeded: how much is hidden. */
	return crowd;
}

/* Gives the corner of the top window of the desktop shown other than surface; 0 when there is none. */
static int
glass_top(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t *x,
	int32_t *y)
{
	struct kwl_client *client;
	struct kwl_object *other;
	struct kwl_object *top;
	struct shell_rect body;
	int placed;

	/* The shown window mapped or raised last. */
	top = NULL;
	for (client = server->clients; client != NULL; client = client->next) {
		for (other = client->objects; other != NULL; other = other->next) {
			placed = glass_placed(server, surface, other);
			if (!placed)
				continue;
			if (top == NULL || other->map_order > top->map_order)
				top = other;
		}
	}

	/* Its body's corner. */
	if (top == NULL)
		return 0;
	body_rect(server, top, &body);
	*x = body.x;
	*y = body.y;
	return 1;
}

/* Tells whether an object is a window placement looks at: shown on the desktop shown, not docked, and not the new one. */
static int
glass_placed(
	struct kwl_server *server,
	struct kwl_object *surface,
	struct kwl_object *other)
{
	/* A mapped window of the desktop shown. */
	if (other == surface || other->kind != KWL_SURFACE || other->dead || !other->mapped)
		return 0;
	if (other->role == NULL || other->cursor_role || other->minimized || other->maximized || other->fullscreen)
		return 0;

	/* A new window opens on the anchor: only the anchor's windows are looked at (ws113-p007). */
	if (other->output != KWL_PLANE_ANCHOR)
		return 0;

	/* Succeeded: when on the desktop shown. */
	return other->desktop == server->desktop;
}

/*
 * Gives the largest body a window can have and still be seen whole in the
 * glass look: the output less the margins at its sides and bottom, the
 * system bar, and a floating title bar above the body.  xdg-shell's
 * configure_bounds tells windows this size (protocol.c).
 */
void
kwl_glass_space(
	struct kwl_server *server,
	int32_t *width,
	int32_t *height)
{
	int32_t space_width;
	int32_t space_height;
	int32_t right;
	int32_t bottom;

	/* What the on-screen keyboard's panel takes at the right or the bottom (keyboard.c, ws102-p007). */
	kwl_keyboard_reserved(&right, &bottom);

	/* The output less a margin at each side, and the keyboard's column. */
	space_width = (int32_t)server->width - 2 * KWL_GLASS_MARGIN - right;
	if (space_width < 0)
		space_width = 0;

	/* The output under the system bar and a title bar, less the bottom margin and the keyboard's row. */
	space_height = (int32_t)server->height - KWL_GLASS_TOP - KWL_GLASS_MARGIN - bottom;
	if (space_height < 0)
		space_height = 0;

	/* Both at once. */
	*width = space_width;
	*height = space_height;
}

/*
 * Moves a place so that a body of a size ends inside the glass look's
 * space; a body too large for it starts at the space's top-left corner and
 * overhangs right and down.  The space starts under the system bar and a
 * floating title bar, so the title bar is never under the system bar.
 */
void
kwl_glass_fit(
	struct kwl_server *server,
	int32_t width,
	int32_t height,
	int32_t *x,
	int32_t *y)
{
	int32_t space_width;
	int32_t space_height;
	int32_t right;
	int32_t bottom;

	/* Its right edge inside the space. */
	kwl_glass_space(server, &space_width, &space_height);
	right = KWL_GLASS_MARGIN + space_width;
	if (*x + width > right)
		*x = right - width;

	/* And its bottom edge. */
	bottom = KWL_GLASS_TOP + space_height;
	if (*y + height > bottom)
		*y = bottom - height;

	/* Never above the space, nor left of it. */
	if (*x < KWL_GLASS_MARGIN)
		*x = KWL_GLASS_MARGIN;
	if (*y < KWL_GLASS_TOP)
		*y = KWL_GLASS_TOP;
}

/*
 * Fits every window to an output of a new size (ws113-p004a, an output
 * moved to another display): a fullscreen window is told the output's
 * size, a docked one the docked space's, and a floating one keeps its size
 * and is moved inside the space (its title bar never under the system bar).
 * The look draws everything again.
 */
void
kwl_glass_output_resized(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct shell_rect docked;
	uint32_t width;
	uint32_t height;
	int32_t x;
	int32_t y;
	int desktop;

	/* Each live window of every client. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a mapped toplevel, not the desktop's icons. */
			if (surface->kind != KWL_SURFACE || surface->dead || !surface->mapped)
				continue;
			if (surface->role == NULL || surface->role->top == NULL)
				continue;
			desktop = kwl_desktop_is(surface);
			if (desktop)
				continue;

			/* Only the anchor's windows: a head's keep their places (ws113-p015). */
			if (surface->output != KWL_PLANE_ANCHOR)
				continue;

			/* Fullscreen: the output's new size. */
			if (surface->fullscreen) {
				window_configure(surface);
				printf("KWL OUTPUT window surface=%u state=fullscreen w=%u h=%u\n", surface->id, server->width, server->height);
				continue;
			}

			/* Docked: the docked space of the new output. */
			if (surface->maximized && server->glass) {
				docked_rect(server, KWL_PLANE_ANCHOR, &docked);
				surface->x = docked.x;
				surface->y = docked.y;
				surface->window_width = (uint32_t)docked.width;
				surface->window_height = (uint32_t)docked.height;
				window_configure(surface);
				window_resized(surface);
				printf("KWL OUTPUT window surface=%u state=docked x=%d y=%d w=%d h=%d\n", surface->id, (int)docked.x, (int)docked.y, (int)docked.width, (int)docked.height);
				continue;
			}

			/* Floating: its size kept, moved inside the space. */
			width = 0U;
			height = 0U;
			kwl_decoration_geometry(surface, &width, &height);
			x = surface->x;
			y = surface->y;
			if (server->glass) {
				kwl_glass_fit(server, (int32_t)width, (int32_t)height, &x, &y);
			} else {
				/* Its right and bottom edges inside the output. */
				if (x + (int32_t)width > (int32_t)server->width)
					x = (int32_t)server->width - (int32_t)width;
				if (y + (int32_t)height > (int32_t)server->height)
					y = (int32_t)server->height - (int32_t)height;

				/* Never left of it nor above it. */
				if (x < 0)
					x = 0;
				if (y < 0)
					y = 0;
			}

			/* The window's place. */
			surface->x = x;
			surface->y = y;
			printf("KWL OUTPUT window surface=%u state=floating x=%d y=%d w=%u h=%u\n", surface->id, (int)x, (int)y, width, height);
		}
	}

	/* The anchor's arranged desktops, arranged again in its new work area (arrange-shell.c, ws177-p036). */
	kwl_arrange_output_resized(server, KWL_PLANE_ANCHOR);

	/* Everything is drawn again at the new size. */
	server->dirty = 1;
}

/*
 * Starts the growth of a newly mapped window out of App Home's icon, when
 * it is the window of an application Home just started (ws035-p071).
 */
void
kwl_glass_mapped(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct shell_rect to;
	int32_t from[4];
	int launched;

	/* Only the glass look animates, and only the window a launch waits for. */
	if (!server->glass)
		return;

	/* A new window on an arranged desktop ends its arrangement (WS181). */
	kwl_arrange_mapped(server, surface);

	/* The launch it may be. */
	launched = kwl_home_launched(server, from);
	if (!launched)
		return;

	/* A window that came late is named as the launch's (the tests find it so) but does not grow. */
	if (launched == 2) {
		body_rect(server, surface, &to);
		printf("KWL GLASS launch-late surface=%u from=%d,%d to=%d,%d size=%dx%d client=%llu\n", surface->id, from[0], from[1], to.x, to.y, to.width, to.height, (unsigned long long)surface->client->number);
		return;
	}

	/* From the icon's rectangle to the window's own. */
	body_rect(server, surface, &to);
	memcpy(server->anim_from, from, sizeof(server->anim_from));
	memcpy(server->anim_to, &to, sizeof(server->anim_to));
	server->anim = surface;
	server->anim_docking = ANIM_LAUNCH;
	server->anim_start_ms = kwl_milliseconds();
	server->dirty = 1;
	printf("KWL GLASS launch surface=%u from=%d,%d to=%d,%d size=%dx%d client=%llu\n", surface->id, from[0], from[1], to.x, to.y, to.width, to.height, (unsigned long long)surface->client->number);
}

/*
 * Forgets a destroyed window as a desktop's docked owner (WS181): the
 * desktop is marked, so that the next frame ends the docked mode when it
 * is the desktop shown, and only forgets the owner otherwise
 * (layout_follow).
 */
void
kwl_glass_forget(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	unsigned desktop;
	unsigned slot;

	/* Each desktop it owned, on each output. */
	for (desktop = 0U; desktop < KWL_APPS_DESKTOPS; desktop++) {
		for (slot = 0U; slot < KWL_PLANE_SLOTS; slot++) {
			if (server->dock_owner[desktop][slot] != surface)
				continue;

			/* The pointer goes before the storage; the mark stays until the next frame looks at it. */
			server->dock_owner[desktop][slot] = NULL;
			server->dock_owner_gone[desktop][slot] = 1U;
		}
	}

	/* An arranged window that goes ends its desktop's arrangement (arrange-shell.c). */
	kwl_arrange_forget(server, surface);
}

/*
 * Gives where an output's desktops' pill is: its left, its bar's top and
 * its width in the plane (the system bar's on the anchor, a head's bar's,
 * ws113-p015; for the arrangement menu, arrange-shell.c).
 */
void
kwl_glass_desktops_pill(
	struct kwl_server *server,
	unsigned slot,
	int32_t *x,
	int32_t *top,
	int32_t *width)
{
	struct shell_bar bar;

	/* The bar's layout now. */
	bar_layout_on(server, slot, &bar);
	*x = bar.desktops_x;
	*top = bar.top;
	*width = bar.desktops_width;
}

/*
 * Tells whether a point is on one of the docked window's controls in the
 * system bar (close, restore, minimize): 1 or 0 (for the top-right
 * corner's swipe, corner.c, which leaves a mouse press there to the bar,
 * BUG-245).  Without a docked window, or while a fullscreen window keeps
 * the bar away, there are none.
 */
int
kwl_glass_bar_control_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct shell_bar bar;
	struct kwl_object *cover;
	struct kwl_object *docked;
	int button;

	/* A fullscreen window that keeps the bar away leaves no control to press. */
	cover = bar_cover(server);
	if (cover != NULL)
		return 0;

	/* Only a docked window has controls in the bar. */
	docked = docked_window(server, KWL_PLANE_ANCHOR);
	if (docked == NULL)
		return 0;

	/* Looks for a control at the point in the bar's layout now. */
	bar_layout(server, &bar);
	button = bar_button_at(&bar, x, y);
	if (button < 0)
		return 0;

	/* Succeeded: the point is on a control. */
	return 1;
}

/*
 * Gives the work area an arrangement fills (WS181): under the system bar,
 * less the on-screen keyboard's settled column or row, above the bottom
 * edge's strip where the swipe to App Home starts, and inside the left and
 * right edges' strips where the desktops' swipe starts (ws177-p035): a
 * window's edge there could not be pressed.
 */
void
kwl_glass_work_area(
	struct kwl_server *server,
	unsigned slot,
	struct kwl_arrange_rect *area)
{
	struct kwl_plane_rect output;
	int32_t right;
	int32_t bottom;

	/* A head's: under its bar (ws113-p015; no keyboard nor bottom strip there). */
	if (slot != KWL_PLANE_ANCHOR) {
		(void)kwl_output_rect(server, slot, &output);
		area->x = output.x;
		area->y = output.y + KWL_GLASS_BAR;
		area->width = (int32_t)output.width;
		area->height = (int32_t)output.height - KWL_GLASS_BAR;
		return;
	}

	/* What the keyboard's panel takes when it has settled. */
	kwl_keyboard_reserved(&right, &bottom);

	/* The output under the bar, the arrangement's margin keeping the slots off the bottom strip. */
	area->x = 0;
	area->y = KWL_GLASS_BAR;
	area->width = (int32_t)server->width - right;
	area->height = (int32_t)server->height - KWL_GLASS_BAR - bottom - (KWL_EDGE_BOTTOM_HEIGHT - KWL_ARRANGE_MARGIN);

	/* The margin keeps the slots off the left edge's strip. */
	area->x += DESKTOP_EDGE - KWL_ARRANGE_MARGIN;
	area->width -= DESKTOP_EDGE - KWL_ARRANGE_MARGIN;

	/* And off the right edge's, unless the keyboard's column is there instead. */
	if (right == 0)
		area->width -= DESKTOP_EDGE - KWL_ARRANGE_MARGIN;
}

/*
 * Ends an output's docked mode without any window's animation, when it is
 * on (an arrangement starting, arrange-shell.c; a head closing, heads.c).
 */
void
kwl_glass_leave_quiet(
	struct kwl_server *server,
	unsigned slot,
	const char *via)
{
	/* Only the docked mode ends. */
	if (slot >= KWL_PLANE_SLOTS || server->layout_mode[slot] != KWL_LAYOUT_DOCKED)
		return;

	/* Every window of the output floating at once. */
	layout_leave(server, slot, NULL, 0, 0, via);
}

/*
 * Places a floating window's body at a place and size, and tells it the
 * size (an arranged window, arrange-shell.c): it has a floating place of
 * its own from now on.
 */
void
kwl_glass_place_body(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	/* Its place and size, its own. */
	surface->x = x;
	surface->y = y;
	surface->window_width = (uint32_t)width;
	surface->window_height = (uint32_t)height;
	surface->placed = 1;
	surface->restore_default = 0U;
	server->dirty = 1;

	/* The client draws that size; until it does, its image is drawn at it. */
	window_configure(surface);
	window_resized(surface);
}

/* Gives the rectangle a window's body is drawn in now (x, y, width, height). */
void
kwl_glass_body(
	struct kwl_server *server,
	const struct kwl_object *surface,
	int32_t body[4])
{
	struct shell_rect rect;

	/* As the frame draws it. */
	body_rect(server, surface, &rect);
	body[0] = rect.x;
	body[1] = rect.y;
	body[2] = rect.width;
	body[3] = rect.height;
}

/* Docks a window where it floats (an arranged window let go in the system bar, arrange-shell.c). */
void
kwl_glass_dock_window(
	struct kwl_server *server,
	struct kwl_object *surface,
	const char *via)
{
	/* Back to where its slot is when it floats again. */
	window_dock(server, surface, surface->x, surface->y, via);
}

/*
 * Ends the wait of a window docked or brought back once its client has
 * drawn the size it was sent: from then on its own image is drawn at its own
 * size (BUG-180).  The log says how long the client took (BUG-179).
 */
void
kwl_glass_committed(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct shell_rect docked;
	uint32_t width;
	uint32_t height;
	uint64_t now;
	uint64_t acked;
	uint64_t committed;
	unsigned stale;

	/* Only a window waiting for an image of its new size, with an image. */
	if (surface->resized_ms == 0U)
		return;
	if (surface->current == NULL)
		return;

	/* An image committed before the client acknowledged the configure was drawn at the old size. */
	if (surface->acked_serial < surface->resized_serial)
		return;

	/* The new image's size, and the docked space's. */
	kwl_surface_size(surface, &width, &height);
	docked_rect(server, window_slot(surface), &docked);

	/*
	 * An image of the size the window had before is one drawn before the
	 * client read the configure (Files acknowledges a configure when it
	 * comes and draws later); the wait goes on.
	 */
	stale = 0U;
	if (surface->maximized) {
		/* Docked: its floating size is the old one. */
		if (width == surface->restore_width && height == surface->restore_height)
			stale = 1U;
	} else {
		/* Brought back: the docked size is the old one. */
		if ((int32_t)width == docked.width && (int32_t)height == docked.height)
			stale = 1U;

		/*
		 * Floating and sent a new size (an arranged window, WS181): the size
		 * of its image when the size was sent is the old one, unless it is
		 * also the size sent.
		 */
		if (width == surface->resized_from_width &&
		    height == surface->resized_from_height &&
		    (width != surface->window_width || height != surface->window_height))
			stale = 1U;
	}

	/* The old size: the wait goes on. */
	if (stale != 0U)
		return;

	/* The client has drawn the new size; the log says how long after it was sent, and when it acknowledged (BUG-179). */
	now = kwl_milliseconds();
	acked = 0U;
	if (surface->resized_acked_ms >= surface->resized_ms)
		acked = surface->resized_acked_ms - surface->resized_ms;
	committed = 0U;
	if (surface->resized_commit_ms >= surface->resized_ms)
		committed = surface->resized_commit_ms - surface->resized_ms;
	printf("KWL GLASS resized surface=%u docked=%u width=%u height=%u after_ms=%llu acked_ms=%llu committed_ms=%llu sent_at_ms=%llu client=%llu\n", surface->id, surface->maximized, width, height,
	       (unsigned long long)(now - surface->resized_ms), (unsigned long long)acked, (unsigned long long)committed, (unsigned long long)surface->resized_ms, (unsigned long long)surface->client->number);

	/* The wait is over: the image is drawn at its own size. */
	surface->resized_ms = 0U;
	server->dirty = 1;

	/* Succeeded: the window is drawn as its client draws it again. */
	return;
}

/*
 * Handles the compositor's shortcuts: Ctrl+Alt+Left and Right, and Alt+Shift+Left
 * and Right, switch to the desktop before and after, Super+Tab opens
 * Wiseview (and while it is open every key is Wiseview's).  Returns 1 when
 * the key is the compositor's.
 */
int
kwl_glass_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	struct kwl_object *surface;
	int showing;
	int target;
	int step;
	int taken;
	int super;

	/* A pairing's question, while it shows, takes every key (bluetooth-ask.c, ws143-p006). */
	taken = kwl_bluetooth_ask_key(server, key, state);
	if (taken)
		return 1;

	/* The power dialog, while it shows, takes every key (power-dialog.c, ws099-p037). */
	taken = kwl_power_dialog_key(server, key, state);
	if (taken)
		return 1;

	/* The switcher: Alt+Tab, and its keys while it is on (switcher-shell.c, ws142-p005). */
	taken = kwl_switch_key(server, key, state);
	if (taken)
		return 1;

	/* The volume's open popup takes every key (volume.c). */
	taken = kwl_volume_key(server, key, state);
	if (taken)
		return 1;

	/* Super+N, and the keys of the notifications' log while it is open (notify-log.c). */
	taken = kwl_notify_log_key(server, key, state);
	if (taken)
		return 1;

	/* Esc hides the previews of an application's icon (apps-bar.c). */
	taken = kwl_apps_bar_key(server, key, state);
	if (taken)
		return 1;

	/* Wiseview, open or opening, takes every key (ws035-p014). */
	showing = wiseview_showing(server);
	if (showing) {
		wiseview_key(server, key, state);
		return 1;
	}

	/* Super+Tab opens Wiseview from the keyboard, as Windows+Tab does. */
	if (key == KEY_TAB && (server->modifiers & MODIFIER_SUPER) != 0U) {
		if (state != 0U)
			wiseview_open_key(server);
		return 1;
	}

	/* Super+Alt with a letter: the editing operations and the previous application (edit.c, ws102-p017). */
	taken = kwl_edit_key(server, key, state);
	if (taken)
		return 1;

	/* The way out of fullscreen, the compositor's own (BUG-194). */
	taken = fullscreen_leave_key(server, key, state);
	if (taken)
		return 1;

	/* Super+Shift with an arrow: the focused window to the display beside (ws113-p007). */
	taken = display_move_key(server, key, state);
	if (taken)
		return 1;

	/* Alt+Shift with an arrow: the desktop before and after (ws181-p010). */
	taken = desktop_alt_shift_key(server, key, state);
	if (taken)
		return 1;

	/* Super with an arrow: the focused arranged window swaps with its neighbour that way (arrange-shell.c, ws177-p037). */
	super = 0;
	if ((server->modifiers & MODIFIERS_ANY) == MODIFIER_SUPER)
		super = 1;
	surface = sheet_owner(kwl_top_window(server));
	taken = kwl_arrange_key_swap(server, surface, key, state, super);
	if (taken)
		return 1;

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
		surface = sheet_owner(kwl_top_window(server));
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
kwl_glass_tick(
	struct kwl_server *server)
{
	uint64_t elapsed;
	uint64_t idle;
	float progress;
	time_t now;

	/* The login screen has only its clock (greeter.c) and the session manager's answers (handoff.c). */
	if (server->greeter) {
		kwl_greeter_tick(server);
		kwl_handoff_tick(server);
		return;
	}

	/* What the session manager sent the session (handoff.c). */
	kwl_handoff_tick(server);

	/* The notifications' popup moves on; while locked they go to the log unseen (notify-popup.c). */
	kwl_notify_popup_tick(server);

	/* The lock screen has only its clock (its answers came above). */
	if (server->locked) {
		kwl_greeter_tick(server);
		return;
	}

	/* A session left without input for long enough locks (ws035-p102). */
	idle = kwl_milliseconds() - server->lock_input_ms;
	if (server->lock_idle_ms != 0U && idle >= server->lock_idle_ms)
		(void)kwl_lock(server, "idle");

	/* The sheets where their parents are (ws090-p014). */
	sheet_place(server);

	/* The power dialog's darkening, and its end (power-dialog.c). */
	kwl_power_dialog_tick(server);

	/* The bar's layout follows a docked window coming or going (ws099-p034b). */
	bar_dock_follow(server);

	/*
	 * In the docked mode a desktop's docked window that closed, was
	 * minimized or was sent away ends the mode; a window that came to the
	 * front otherwise docks (WS181).
	 */
	layout_follow(server);

	/* The arrangements: a window gone, minimized, resized or fullscreen ends its desktop's (WS181). */
	kwl_arrange_tick(server);

	/* A press held in the top edge's band that rests becomes a long press (ws177-p033). */
	band_tick(server);

	/* The previews of the bar's applications show and hide in time (apps-bar.c), and the switcher goes when it may not show. */
	kwl_apps_bar_tick(server);
	kwl_switch_tick(server);

	/* App Home's animation, and the applications it started that have ended. */
	kwl_home_tick(server);

	/* The top-right corner's swipe: its time limit, its hint settling, and Notes being waited for (corner.c). */
	kwl_corner_tick(server);

	/* The on-screen keyboard's swipe and its panel's place (keyboard.c). */
	kwl_keyboard_tick(server);

	/* A finger on a title bar that has waited long enough for a second one, or two that did not flick in time (touch.c). */
	kwl_touch_tick(server);

	/* An open menu closes when what it belongs to changed (menu-shell.c). */
	kwl_menu_tick(server);

	/* What the network watch brought (network.c). */
	kwl_network_tick(server);

	/* Bluetooth's menu: the answer to its request, and its end with the lock (bluetooth-bar.c, ws143-p006). */
	kwl_bluetooth_bar_tick(server);

	/* What audiod reported, and the volume's sends held back (volume.c). */
	kwl_volume_tick(server);

	/* The desktops' slide draws every frame until it is done. */
	if (server->desktop_moving) {
		server->dirty = 1;
		elapsed = kwl_milliseconds() - server->desktop_start_ms;
		if (elapsed >= DESKTOP_MS) {
			server->desktop_moving = 0;
			printf("KWL GLASS desktop settled desktop=%u windows=%u\n", server->desktop + 1U, desktop_windows(server, server->desktop));
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
		elapsed = kwl_milliseconds() - server->wiseview_start_ms;
		if (elapsed >= WISEVIEW_MS) {
			server->wiseview_moving = 0;
			server->wiseview = server->wiseview_to;
			if (server->wiseview > 0.0f)
				wiseview_log(server);
			else
				printf("KWL WISEVIEW closed at_ms=%llu\n", (unsigned long long)kwl_milliseconds());
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

/* Lays out the system bar on the anchor (bar_layout_on). */
static void
bar_layout(
	struct kwl_server *server,
	struct shell_bar *bar)
{
	/* The anchor's. */
	bar_layout_on(server, KWL_PLANE_ANCHOR, bar);
}

/*
 * Lays out an output's bar (ws099-p034; a head's, ws113-p015, the
 * 2026-10-08 user decision): from the right the clock's pill and the
 * status pill (the input method's language, the removable media, the
 * network, the volume and the battery, each in a slot); the desktops' pill
 * in the middle of the bar, and just left of the status pill while a window
 * is docked (ws181-p011), and the docked window's buttons at the right
 * end; from the left the launcher, a line and the docked title.
 * The system bar makes room for the buttons as its docked layout comes in
 * (animated); a head's bar at once while a window is docked on the head.
 */
static void
bar_layout_on(
	struct kwl_server *server,
	unsigned slot,
	struct shell_bar *bar)
{
	struct kwl_plane_rect output;
	struct kwl_object *docked;
	struct tm local;
	time_t now;
	int32_t place;
	int32_t right;
	int32_t side;
	int32_t middle;
	float dock;
	int32_t battery_slot;
	int32_t slots;
	int32_t shift;
	int ime_width;
	int media_width;
	int bluetooth_width;
	int button;

	/* The output's rectangle, the bar along its top. */
	(void)kwl_output_rect(server, slot, &output);
	right = output.x + (int32_t)output.width;
	bar->output = slot;
	bar->top = output.y;

	/* The date and time in a pill at the right edge. */
	now = time(NULL);
	memset(&local, 0, sizeof(local));
	(void)localtime_r(&now, &local);
	bar->clock[0] = '\0';
	kwl_language_date(&local, KWL_LANGUAGE_DATE_SHORT, bar->clock, sizeof(bar->clock));
	bar->clock_pill_width = glass_text_width(server, SIZE_BAR, bar->clock) + 2 * BAR_CLOCK_PAD;
	bar->clock_pill_x = right - BAR_EDGE - bar->clock_pill_width;
	bar->clock_x = bar->clock_pill_x + BAR_CLOCK_PAD;

	/*
	 * A docked window's buttons in a pill at the right end (ws099-p034b, the
	 * 2026-10-06 user decision); the status and the clock make room left of
	 * it as the docked layout comes in (bar_dock, animated).
	 */
	bar->buttons_width = BUTTON_COUNT * BAR_BUTTON_SPACING + 6;
	bar->buttons_x = right - BAR_EDGE - bar->buttons_width;
	for (button = 0; button < BUTTON_COUNT; button++)
		bar->buttons[button] = bar->buttons_x + 3 + BAR_BUTTON_SPACING / 2 + (BUTTON_COUNT - 1 - button) * BAR_BUTTON_SPACING;

	/* How far the docked layout has come in: the system bar's animation, a head's at once. */
	dock = server->bar_dock;
	if (slot != KWL_PLANE_ANCHOR) {
		docked = docked_window(server, slot);
		dock = 0.0f;
		if (docked != NULL)
			dock = 1.0f;
	}

	/* The status and the clock make that much room. */
	shift = (int32_t)(dock * (float)(bar->buttons_width + BAR_PILL_GAP) + 0.5f);
	bar->clock_pill_x -= shift;
	bar->clock_x -= shift;

	/*
	 * The status pill's slots: the network and the volume always, the input
	 * method's language and the removable media while they show, and the
	 * battery on a machine that has one (the 2026-10-05 user decision), a
	 * little wider while it charges for the "+".
	 */
	ime_width = kwl_ime_indicator_width(server);
	media_width = kwl_media_width();
	bluetooth_width = kwl_bluetooth_bar_width();
	slots = 2 * BAR_SLOT;
	if (ime_width > 0)
		slots += BAR_SLOT;
	if (media_width > 0)
		slots += BAR_SLOT;
	if (bluetooth_width > 0)
		slots += BAR_SLOT;
	battery_slot = 0;
	if (server->power.percent >= 0) {
		battery_slot = BAR_SLOT;
		if (server->power.charging != 0U)
			battery_slot += 10;
	}

	/* The pill: the slots and a padding at both ends, left of the clock's pill. */
	slots += battery_slot;
	bar->status_width = slots + 2 * BAR_STATUS_PAD;
	bar->status_x = bar->clock_pill_x - BAR_PILL_GAP - bar->status_width;

	/* Each icon centred in its slot, from the left: the language, the media, Bluetooth (ws143-p006), the network, the volume, the battery. */
	place = bar->status_x + BAR_STATUS_PAD;
	bar->ime_x = place + (BAR_SLOT - 26) / 2;
	if (ime_width > 0)
		place += BAR_SLOT;
	bar->media_x = place + (BAR_SLOT - 20) / 2;
	if (media_width > 0)
		place += BAR_SLOT;
	bar->bluetooth_x = place + (BAR_SLOT - 20) / 2;
	if (bluetooth_width > 0)
		place += BAR_SLOT;
	bar->signal_x = place + (BAR_SLOT - 20) / 2;
	place += BAR_SLOT;
	bar->volume_x = place + (BAR_SLOT - 20) / 2;
	place += BAR_SLOT;
	bar->battery_x = place + (BAR_SLOT - 26) / 2;

	/*
	 * The desktops' pill in the middle of the bar (ws181-p011, the
	 * 2026-10-08 UAT: "dockバーの中央がいい"), and just left of the status
	 * pill while a window is docked (ws181-p009: a docked window's menus run
	 * across the middle), moving between the two as the docked layout comes
	 * in.  The middle is never right of the status side (a narrow screen).
	 * The room for the docked title and menus ends a gap before it.
	 */
	bar->desktops_width = 2 * DESKTOPS_PAD + DESKTOPS * DESKTOP_WIDTH + (DESKTOPS - 1) * DESKTOP_GAP;
	side = bar->status_x - BAR_PILL_GAP - bar->desktops_width;
	middle = output.x + ((int32_t)output.width - bar->desktops_width) / 2;
	if (middle > side)
		middle = side;
	bar->desktops_x = middle + (int32_t)(dock * (float)(side - middle) + 0.5f);
	bar->desktops_line = bar->desktops_x - 12;

	/* On the left, after the launcher, a line and the docked title (ws035-p117: no word after the mark). */
	bar->menu_line = output.x + BAR_LAUNCHER_X + BAR_LAUNCHER_SIZE + 10;
	bar->title_x = bar->menu_line + 14;
}

/*
 * Tells whether a window is drawn in this frame: not minimized, on the
 * desktop shown or on one sliding in beside it (only the desktop shown
 * while Home is open).
 */
static int
window_shown(
	struct kwl_server *server,
	struct kwl_object *surface,
	float home,
	float position)
{
	float shift;
	int hidden;

	/* A minimized window, or one a screen or more to the side. */
	shift = ((float)surface->desktop - position) * (float)server->width;
	if (shift <= -(float)server->width || shift >= (float)server->width || surface->minimized)
		return 0;

	/* With Home open, only the desktop shown. */
	if (home > 0.0f && surface->desktop != server->desktop)
		return 0;

	/* In the docked mode another application's window is not drawn (ws142-p008). */
	hidden = layout_hides(server, surface);
	if (hidden)
		return 0;

	/* The window is drawn. */
	return 1;
}

/* Moves the layer with a window's desktop while the desktops slide (Home, when open, has the layer instead). */
static void
window_layer(
	struct kwl_server *server,
	struct kwl_object *surface,
	float home,
	float position)
{
	float shift;

	/* Home keeps its own layer. */
	if (home > 0.0f)
		return;

	/* The window's desktop's place beside the one shown. */
	shift = ((float)surface->desktop - position) * (float)server->width;
	server->layer_on = 0;
	if (shift != 0.0f)
		server->layer_on = 1;
	server->layer_x = shift;
	server->layer_y = 0.0f;
	server->layer_scale = 1.0f;
	server->layer_opacity = 1.0f;
}

/*
 * Draws the scene under a window into the backdrop (backdrop.c): the
 * wallpaper and the windows below it, whose own glass is on the blurred
 * wallpaper.  The glass drawn after it is on this scene, blurred.
 */
static void
draw_backdrop(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object **windows,
	unsigned below,
	float position)
{
	struct glass_shape shape;
	unsigned index;
	int started;
	int shown;

	/* The backdrop's pass; a device without it keeps the blurred wallpaper. */
	started = kwl_backdrop_begin(server, command);
	if (!started)
		return;

	/* The wallpaper, not moved. */
	server->layer_on = 0;
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_IMAGE;
	shape.opaque = 1.0f;
	shape.set = glass_wallpaper_set(server);
	glass_shape_draw(server, command, &shape);

	/* The desktop's icons on it (desktop.c). */
	kwl_desktop_draw(server, command);

	/* The windows below, as the blur will show them. */
	for (index = 0; index < below; index++) {
		shown = window_shown(server, windows[index], 0.0f, position);
		if (!shown)
			continue;
		window_layer(server, windows[index], 0.0f, position);
		draw_window_blurred(server, command, windows[index]);
	}

	/* The output's pass again, and the scene blurred for the glass. */
	kwl_backdrop_end(server, command);
}

/*
 * Draws a window for the backdrop, where it is only seen blurred: its body
 * and a floating title bar's glass, without the title, the menu, the
 * controls or the buttons (which would also record their places twice).
 */
static void
draw_window_blurred(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface)
{
	struct shell_rect body;
	struct shell_rect panel;
	struct glass_shape shape;
	int decorated;

	/* The body where it is now. */
	body_rect(server, surface, &body);
	draw_body(server, command, surface, &body, 0U);
	if (surface->maximized)
		return;

	/* Blurred scenes must not recreate the titlebar removed from CSD windows. */
	decorated = kwl_decoration_server(surface);
	if (!decorated)
		return;

	/* A floating title bar's glass (rounded even on a window whose body keeps square corners). */
	floating_title(&body, &panel);
	glass_shape_init(&shape, (float)panel.x, (float)panel.y, (float)panel.width, (float)panel.height);
	shape.mode = MODE_GLASS;
	shape.radius = GLASS_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.38f;
	shape.edge = 0.85f;
	glass_shape_draw(server, command, &shape);
}

/*
 * Draws a window: its body, and its title bar floating above it, docked in
 * the system bar (drawn with the bar), or on its way between the two.
 */
static void
draw_window(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	unsigned focused,
	const struct shell_bar *bar)
{
	struct shell_rect body;
	struct shell_rect from;
	struct shell_rect to;
	struct shell_rect panel;
	struct shell_rect slot;
	struct kwl_object *parent;
	float t;
	int decorated;

	/* A sheet has only its body, under its parent's title bar (ws090-p014). */
	parent = kwl_sheet_parent(surface);
	if (parent != NULL) {
		draw_sheet(server, command, surface, parent, focused);
		return;
	}

	/* The body, where it is now. */
	body_rect(server, surface, &body);

	/* Client-decorated windows supply their own frame, controls and animation content. */
	decorated = kwl_decoration_server(surface);
	if (!decorated) {
		draw_body(server, command, surface, &body, focused);

		/* The client image is the entire decorated window. */
		return;
	}

	/* A window a launch from Home started grows out of the icon; its title bar fades in on it. */
	if (server->anim == surface && server->anim_docking == ANIM_LAUNCH) {
		t = animation_progress(server);
		draw_body(server, command, surface, &body, focused);
		floating_title(&body, &panel);
		draw_title_bar(server, command, surface, &panel, t, t, focused);
		return;
	}

	/* While docking or coming back, the title bar slides between its two places and its glass fades. */
	if (server->anim == surface) {
		t = animation_progress(server);
		draw_body(server, command, surface, &body, focused);
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
		draw_body(server, command, surface, &body, focused);
		floating_title(&body, &panel);
		draw_title_bar(server, command, surface, &panel, t, t, focused);
		return;
	}

	/*
	 * A docked window has only its body; its title is in the system bar.
	 * One of one size is in the middle of the docked space, a whole body
	 * over the blurred, darkened scene under it (ws142-p008, p008b:
	 * draw_centred_cover).
	 */
	if (surface->maximized) {
		draw_body(server, command, surface, &body, focused);
		return;
	}

	/* A fullscreen window has only its body, whole (draw_body). */
	if (surface->fullscreen) {
		draw_body(server, command, surface, &body, focused);
		return;
	}

	/* A floating window. */
	draw_body(server, command, surface, &body, focused);
	floating_title(&body, &panel);
	draw_title_bar(server, command, surface, &panel, 1.0f, 1.0f, focused);
}

/*
 * Draws a window's body in a rectangle: its shadow, the frosted glass under
 * a see-through body, and its image with rounded corners (a docked body's
 * too: it keeps KWL_GLASS_DOCK_PAD from the output's edges, ws099-p038).  A window with glass panels
 * (panels.c) is not one slab: its panels cast the shadows and stand on the
 * glass, and its image is blended over them by its alpha.  A sheet's upper
 * corners are square, flush with its parent's title bar (ws090-p014).
 */
static void
draw_body(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	const struct shell_rect *body,
	unsigned focused)
{
	const struct kwl_import *image;
	struct kwl_object *parent;
	struct glass_shape shape;
	float place[4];
	unsigned panels;
	int32_t width;
	int32_t height;
	float soft;
	float radius;
	unsigned square;
	int whole;
	int decorated;
	float scale_x;
	float scale_y;
	float raise;
	int32_t shown_width;
	int32_t shown_height;
	int unscaled;

	/* The window's size (its viewport's, else its image's), whose scale to the rectangle stretches it while it changes size. */
	image = kwl_compose_surface_image(surface);
	window_size(surface, &width, &height);
	if (width <= 0 || height <= 0) {
		width = (int32_t)image->width;
		height = (int32_t)image->height;
	}

	/* The body's scale from that size, for the panels and the sub-surfaces (in surface coordinates). */
	scale_x = (float)body->width / (float)width;
	scale_y = (float)body->height / (float)height;

	/*
	 * A docked window whose image is not the docked space's size (the
	 * on-screen keyboard's panel came or went, and its client has not drawn
	 * the size it was sent yet, or never does) is not stretched: its image
	 * is drawn at its own size from the space's top-left corner and cut at
	 * the space's edges (BUG-243: a full keyboard squeezed the window's
	 * picture).  Its input is already taken at that scale (from the body's
	 * origin).  Docking and pulling animations still stretch.
	 */
	shown_width = body->width;
	shown_height = body->height;
	unscaled = 0;
	if (surface->maximized && server->anim != surface && server->pull != surface && (width != body->width || height != body->height))
		unscaled = 1;
	if (unscaled) {
		scale_x = 1.0f;
		scale_y = 1.0f;
		if (width < shown_width)
			shown_width = width;
		if (height < shown_height)
			shown_height = height;
	}

	/* A window with glass panels: their shadows and glass instead of the body's (panels.c). */
	panels = kwl_panels_count(surface);
	if (panels > 0U) {
		place[0] = (float)body->x;
		place[1] = (float)body->y;
		place[2] = scale_x;
		place[3] = scale_y;
		kwl_panels_draw(server, command, surface, place, server->window_opacity, 1U);
	}

	/* The corners: rounded, or square for a window that keeps them (window_square). */
	radius = GLASS_RADIUS;
	square = window_square(surface);
	if (square)
		radius = 0.0f;

	/*
	 * A fullscreen window (not while it animates) is its image alone, as
	 * the output's (ws099-p015, as its direct scanout showed it before):
	 * square corners, no shadow, no frosted glass, not see-through.
	 */
	whole = 0;
	if (surface->fullscreen && server->anim != surface && panels == 0U) {
		whole = 1;
		radius = 0.0f;
	}

	/* CSD owns its alpha, shadows and corners; server embellishments must not clip them. */
	decorated = kwl_decoration_server(surface);
	if (!decorated) {
		whole = 1;
		radius = 0.0f;
	}

	/* A sheet's rounding reaches above its top, so that its upper corners are square. */
	raise = 0.0f;
	parent = kwl_sheet_parent(surface);
	if (parent != NULL)
		raise = 2.0f * radius;

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
	shape.radius = radius;
	shape.soft = soft;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.20f;
	if (panels == 0U && !whole)
		glass_shape_draw(server, command, &shape);

	/* A see-through body lies on frosted glass (a window with panels has its own). */
	if (server->window_opacity < 1.0f && panels == 0U && !whole) {
		glass_shape_init(&shape, (float)body->x, (float)body->y, (float)body->width, (float)body->height);
		shape.box[1] -= raise;
		shape.box[3] += raise;
		shape.mode = MODE_GLASS;
		shape.radius = radius;
		shape.color[0] = 1.0f;
		shape.color[1] = 1.0f;
		shape.color[2] = 1.0f;
		shape.color[3] = 0.30f;
		shape.edge = 0.75f;
		glass_shape_draw(server, command, &shape);
	}

	/* The sub-surfaces below the image, scaled with it from the window's size (subsurface.c). */
	kwl_subsurface_draw(server, command, surface, (float)body->x, (float)body->y, scale_x, scale_y, 0U);

	/* The image (its viewport's source), stretched to the rectangle while it changes (or cut, unscaled), as opaque as asked. */
	glass_shape_init(&shape, (float)body->x, (float)body->y, (float)shown_width, (float)shown_height);
	shape.box[1] -= raise;
	shape.box[3] += raise;
	kwl_viewport_source(surface, shape.uv);
	if (unscaled) {
		/* The part of the source the space shows, from its top-left corner. */
		shape.uv[2] = shape.uv[0] + (shape.uv[2] - shape.uv[0]) * (float)shown_width / (float)width;
		shape.uv[3] = shape.uv[1] + (shape.uv[3] - shape.uv[1]) * (float)shown_height / (float)height;
	}

	/* As opaque as asked; a whole window, and an opaque image, fully. */
	shape.opacity = server->window_opacity;
	if (whole)
		shape.opacity = 1.0f;
	shape.mode = MODE_IMAGE;
	shape.radius = radius;
	shape.set = image->set;
	if (image->draw == KWL_DRAW_OPAQUE)
		shape.opaque = 1.0f;
	glass_shape_draw(server, command, &shape);

	/* The sub-surfaces above the image. */
	kwl_subsurface_draw(server, command, surface, (float)body->x, (float)body->y, scale_x, scale_y, 1U);
}

/*
 * Draws a title bar in a rectangle: its shadow and glass faded by fade, the
 * mark and title, and its buttons faded by buttons (a docking title bar
 * keeps its title while its glass and buttons fade).
 */
static void
draw_title_bar(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	const struct shell_rect *panel,
	float fade,
	float buttons,
	unsigned focused)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float faint[4] = { 0.30f, 0.35f, 0.44f, 1.0f };
	struct kwl_menu_area area;
	struct glass_shape shape;
	const float *ink;
	int32_t available;
	int32_t limit;
	int32_t end;
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

	/*
	 * The glass, whiter for the focused window; white enough for the
	 * others that their titles read over a dark window under them
	 * (ws035-p092).
	 */
	glass_shape_init(&shape, (float)panel->x, (float)panel->y, (float)panel->width, (float)panel->height);
	shape.mode = MODE_GLASS;
	shape.radius = GLASS_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.54f;
	if (focused)
		shape.color[3] = 0.64f;
	shape.edge = 0.85f;
	shape.opacity = fade;
	glass_shape_draw(server, command, &shape);

	/* The mark and the title, darker for the focused window, cut short to share the room before the buttons with the menu. */
	ink = faint;
	if (focused)
		ink = dark;
	available = panel->width - 44 - BUTTON_SPACING * BUTTON_COUNT - 12;
	limit = kwl_titlebar_title_limit(server, surface, available);
	draw_title(server, command, surface, panel->x + 14, panel->y + panel->height / 2, limit, ink);

	/* The window's presentation after the title (its menu or its controls), faded with the buttons (titlebar-shell.c). */
	end = title_end(server, surface, limit);
	area.x = panel->x + 44 + end + 18;
	area.top = panel->y;
	area.right = panel->x + 44 + available;
	area.height = panel->height;
	area.origin = panel->x;
	kwl_titlebar_draw(server, command, surface, 0, &area, ink, buttons);

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
 * Draws the application's mark at x and the title after it, centred on
 * middle: a known application's tile (ws035-p124, ws128-p012), else a blue
 * rounded square with a letter.
 */
static void
draw_title(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	int32_t x,
	int32_t middle,
	int32_t limit,
	const float *ink)
{
	char title[KWL_TITLE_MAX + 24];
	int drawn;

	/* The title as the title bar shows it. */
	shown_title(surface, title, sizeof(title));

	/* The mark: the application's picture when it has one, else its letter. */
	drawn = draw_picture_mark(server, command, surface, x, middle, ink);
	if (!drawn)
		draw_letter_mark(server, command, surface, title, x, middle);

	/* The title. */
	glass_draw_text(server, command, SIZE_TITLE, x + 30, middle + 6, title, limit, ink);
}

/*
 * Draws a known application's mark: its tile (the banded square with its
 * picture cut out, ws128-p012), 20 pixels a side, as faded as the title's
 * ink.  Returns 1, or 0 (drawing nothing) for an application ID without a
 * picture.
 */
static int
draw_picture_mark(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	int32_t x,
	int32_t middle,
	const float *ink)
{
	enum glass_hole hole;
	int picture;

	/* The picture that belongs to the window's application ID. */
	picture = kwl_icon_for_app_id(surface->app_id);
	if (picture < 0)
		return 0;

	/* Its tile, its picture showing through the glass it is on (on the bar, the wallpaper). */
	hole = mark_hole(server);
	glass_draw_app_tile(server, command, (unsigned)picture, (float)x, (float)(middle - 10), 20.0f, ink[3], 0.0f, hole);

	/* Succeeded: the mark is drawn. */
	return 1;
}

/*
 * Finds what an application's cut-out picture shows on the glass of the
 * title bars, Wiseview and Alt+Tab (BUG-237): in the light appearance that
 * glass is near white, so the picture shows the blurred scene the glass
 * frosts; the dark appearance's glass shows through itself; the system
 * bar's tiles (drawn while keep_colours is set) show the wallpaper, in
 * either appearance (ws099-p034b).
 */
static enum glass_hole
mark_hole(
	struct kwl_server *server)
{
	/* The system bar's tiles are holes through to the wallpaper, in both appearances (ws099-p034b, the 2026-10-06 user decision). */
	if (server->keep_colours != 0U)
		return GLASS_HOLE_WALLPAPER;

	/* Dark glass reads as a hole by itself. */
	if (server->dark != 0)
		return GLASS_HOLE_GROUND;

	/* Light glass: a window onto the scene. */
	return GLASS_HOLE_SCENE;
}

/*
 * Draws the mark of an application without a picture: a blue rounded
 * square with a letter, the application ID's last word's, else the
 * title's first.
 */
static void
draw_letter_mark(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	const char *title,
	int32_t x,
	int32_t middle)
{
	static const float mark[4] = { 0.29f, 0.55f, 1.0f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const char *source;
	char letter[5];
	size_t length;
	size_t index;
	int32_t width;

	/* The mark. */
	glass_draw_solid(server, command, (float)x, (float)(middle - 10), 20.0f, 20.0f, 6.0f, mark);

	/* Its letter: the application ID's (it stays when the title changes), else the title's. */
	source = mark_name(surface->app_id);
	if (source == NULL)
		source = title;

	/* The first character, all its UTF-8 bytes (a lead byte says how many). */
	length = 1;
	if (((unsigned char)source[0] & 0xe0U) == 0xc0U)
		length = 2;
	else if (((unsigned char)source[0] & 0xf0U) == 0xe0U)
		length = 3;
	else if (((unsigned char)source[0] & 0xf8U) == 0xf0U)
		length = 4;
	for (index = 0; index < length && source[index] != '\0'; index++)
		letter[index] = source[index];
	letter[index] = '\0';

	/* A letter in capitals. */
	if (letter[0] >= 'a' && letter[0] <= 'z')
		letter[0] = (char)(letter[0] - 'a' + 'A');
	width = glass_text_width(server, SIZE_BAR, letter);
	glass_draw_text(server, command, SIZE_BAR, x + 10 - width / 2, middle + 5, letter, 20, white);
}

/* Tells whether a window's body keeps square corners (its application ID is in shell_square_apps). */
static unsigned
window_square(
	const struct kwl_object *surface)
{
	unsigned index;
	int same;

	/* Each application ID of the list. */
	for (index = 0; index < sizeof(shell_square_apps) / sizeof(shell_square_apps[0]); index++) {
		same = strcmp(surface->app_id, shell_square_apps[index]);
		if (same == 0)
			return 1U;
	}

	/* Succeeded: the window's corners are rounded. */
	return 0U;
}

/*
 * Gives the word of an application ID the mark's letter comes from: the
 * last one after a dot or a hyphen ("files" is "files",
 * "org.example.Viewer" is "Viewer"), or NULL when there is none that starts
 * with a letter or a digit.
 */
static const char *
mark_name(
	const char *app_id)
{
	const char *word;
	const char *at;
	char first;

	/* The text after the last dot or hyphen. */
	word = app_id;
	for (at = app_id; *at != '\0'; at++) {
		if (*at == '.' || *at == '-')
			word = at + 1;
	}

	/* A word that starts with a letter or a digit. */
	first = word[0];
	if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9'))
		return word;

	/* Succeeded: none (the title gives the letter). */
	return NULL;
}

/* Tells how far a window's title, drawn by draw_title within a limit, reaches after its start. */
static int32_t
title_end(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t limit)
{
	char title[KWL_TITLE_MAX + 24];
	int32_t width;

	/* The title as draw_title shows it. */
	shown_title(surface, title, sizeof(title));

	/* Its width, or the limit it is cut at. */
	width = glass_text_width(server, SIZE_TITLE, title);
	if (width > limit)
		width = limit;

	/* Succeeded: where the title ends. */
	return width;
}

/*
 * Makes the title a title bar shows: the client's ("Window" without one),
 * saying so when the client is not responding to pings (toplevel.c).
 */
static void
shown_title(
	const struct kwl_object *surface,
	char *title,
	size_t size)
{
	const char *name;

	/* A window without a title is called "Window". */
	name = surface->title;
	if (name[0] == '\0')
		name = kl_tr("Window");

	/* A client that does not answer its pings. */
	if (surface->client->unresponsive) {
		(void)kl_tr_format(title, size, kl_tr("{1} (not responding)"), name, (const char *)NULL);
		return;
	}

	/* Otherwise the name alone. */
	(void)snprintf(title, size, "%s", name);
}

/*
 * Draws one button's sign centred on (cx, cy): a line (minimize), a square
 * (maximize) or two squares (restore), or the multiplication sign (close);
 * with its background when the pointer is over it (red for close).
 */
static void
draw_sign(
	struct kwl_server *server,
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
 * Draws the system bar (ws099-p034, the 2026-10-06 user decisions): dark
 * glass along the top in both appearances, a little lighter in its middle;
 * the launcher and a line; the applications' pill, or the docked window's
 * title and its buttons' pill; the desktops' pill in the middle; the status
 * pill and the clock's pill at the right.  Its ink is light; the glass's
 * colours are kept from the dark appearance's mapping while it is drawn.
 * Over App Home only the status and the clock are drawn, in white (the
 * 2026-10-07 UAT); while Home opens or closes the bar fades into them as
 * the desktop layer fades (ws177-p038).
 */
static void
draw_system_bar(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct shell_bar *bar)
{
	struct glass_bar_colours colours;
	struct kwl_menu_area area;
	struct kwl_object *docked;
	int32_t available;
	int32_t limit;
	int32_t end;
	float progress;
	float home;
	float shown;
	float depth[3];
	float layer[4];
	unsigned layer_on;

	/*
	 * Over App Home the bar is not drawn: only the status and the clock,
	 * in white without their pills (the 2026-10-07 UAT).  While Home opens
	 * or closes, the bar shows as much as the desktop layer does, and the
	 * white status the rest (ws177-p038).
	 */
	home = kwl_home_progress(server);
	shown = 1.0f;
	if (home > 0.0f) {
		kwl_home_layer(server, home, &depth[0], &depth[1], &depth[2], &shown);
		draw_home_status(server, command, bar, 1.0f - shown);
		if (shown <= 0.0f)
			return;
	}

	/*
	 * A bar fading: drawn through a layer that only fades (in place, its
	 * own size), the layer the pass had kept to be put back after.
	 */
	layer_on = server->layer_on;
	layer[0] = server->layer_x;
	layer[1] = server->layer_y;
	layer[2] = server->layer_scale;
	layer[3] = server->layer_opacity;
	if (shown < 1.0f) {
		server->layer_on = 1;
		server->layer_x = 0.0f;
		server->layer_y = 0.0f;
		server->layer_scale = 1.0f;
		server->layer_opacity = shown;
	}

	/* The docked window, if one is on top and not moving. */
	docked = docked_window(server, KWL_PLANE_ANCHOR);

	/*
	 * The bar keeps its own colours (ws099-p034b): light ink on the dark
	 * glass in the dark appearance, the floating title bar's dark ink on its
	 * white glass in the light one.
	 */
	kwl_glass_bar_colours(server, &colours);
	server->keep_colours = 1U;

	/* The strip. */
	draw_bar_strip(server, command, &colours, 0, 0, (int32_t)server->width);

	/* The launcher: the Kei mark (ws035-p117) in the bar's deeper colours (ws035-p118). */
	glass_draw_mark(server, command, BAR_LAUNCHER_X, BAR_LAUNCHER_Y, BAR_LAUNCHER_SIZE, GLASS_MARK_BAR, 1.0f);

	/* The line after the launcher. */
	glass_draw_solid(server, command, (float)bar->menu_line, (float)BAR_LINE_TOP, 1.0f, (float)BAR_LINE_LENGTH, 0.0f, colours.line);

	/*
	 * The docked window: its mark and title in a pill, its menu or controls
	 * after it, up to the desktops (its buttons are at the right end,
	 * draw_bar_buttons).
	 */
	if (docked != NULL) {
		/* The title shares the room before the desktops with the window's menu. */
		available = bar->desktops_line - 12 - bar->title_x - 30;
		limit = kwl_titlebar_title_limit(server, docked, available);
		end = title_end(server, docked, limit);
		draw_bar_group(server, command, bar->title_x - 7, 30 + end + 7 + 12, BAR_GROUP_HEIGHT);
		draw_title(server, command, docked, bar->title_x, KWL_GLASS_BAR / 2, limit, colours.ink);

		/* The presentation after the title: its menu or its controls (titlebar-shell.c). */
		area.x = bar->title_x + 30 + end + 24;
		area.top = 0;
		area.right = bar->title_x + 30 + available;
		area.height = KWL_GLASS_BAR;
		area.origin = 0;
		kwl_titlebar_draw(server, command, docked, 1, &area, colours.ink, 1.0f);
	}

	/* The docked window's buttons at the right end, as far as the docked layout has come in (ws099-p034b). */
	draw_bar_buttons(server, command, bar, docked, &colours);

	/* Wiseview shows no name of its own, in the bar either (BUG-216: the name stays in the code and the documents). */
	progress = wiseview_progress(server);

	/* Without a docked title, the applications' pill in its place (apps-bar.c, ws142-p004). */
	if (docked == NULL && progress <= 0.0f)
		(void)kwl_apps_bar_draw(server, command, KWL_PLANE_ANCHOR);

	/* The desktops, then the status in its pills. */
	draw_desktops(server, command, bar, &colours);
	draw_bar_group(server, command, bar->status_x, bar->status_width, BAR_GROUP_HEIGHT);
	draw_bar_group(server, command, bar->clock_pill_x, bar->clock_pill_width, BAR_GROUP_HEIGHT);
	draw_status(server, command, bar, colours.ink);

	/* The rest of the frame is drawn in the appearance's colours again, and with the layer it had. */
	server->keep_colours = 0U;
	server->layer_on = layer_on;
	server->layer_x = layer[0];
	server->layer_y = layer[1];
	server->layer_scale = layer[2];
	server->layer_opacity = layer[3];
}

/*
 * Draws a head's bar (heads.c's pass, ws113-p015, the 2026-10-08 user
 * decision): as the system bar, its strip, the launcher and a line; for a
 * docked window in front on the head its mark and title in a pill, its
 * menu or controls after it and its buttons at the right end, otherwise
 * the icons of the applications whose windows are on the head; the
 * desktops' pill, the status pill and the clock's pill at the right.
 */
static void
draw_head_bar(
	struct kwl_server *server,
	VkCommandBuffer command,
	unsigned slot)
{
	struct glass_bar_colours colours;
	struct kwl_plane_rect output;
	struct kwl_menu_area area;
	struct kwl_object *docked;
	struct shell_bar bar;
	int32_t available;
	int32_t limit;
	int32_t end;
	int32_t middle;
	int32_t logged_x;
	int32_t logged_top;
	int logged;
	int button;
	int over;

	/* The bar's colours and its strip. */
	(void)kwl_output_rect(server, slot, &output);
	bar_layout_on(server, slot, &bar);
	kwl_glass_bar_colours(server, &colours);
	server->keep_colours = 1U;
	draw_bar_strip(server, command, &colours, output.x, output.y, (int32_t)output.width);

	/* Where its parts are, logged when it is new or has moved. */
	logged = kwl_plane_placed(&shell_head_bars_logged, slot, &logged_x, &logged_top);
	if (!logged ||
	    logged_x != bar.desktops_x ||
	    logged_top != bar.top) {
		kwl_plane_place(&shell_head_bars_logged, slot, bar.desktops_x, bar.top);
		printf("KWL GLASS head bar output=%u top=%d launcher=%d desktops=%d status=%d clock=%d\n", slot, bar.top, output.x + BAR_LAUNCHER_X, bar.desktops_x, bar.status_x, bar.clock_pill_x);
	}

	/* The launcher, which opens App Home on the anchor, and the line after it. */
	glass_draw_mark(server, command, output.x + BAR_LAUNCHER_X, output.y + BAR_LAUNCHER_Y, BAR_LAUNCHER_SIZE, GLASS_MARK_BAR, 1.0f);
	glass_draw_solid(server, command, (float)bar.menu_line, (float)(output.y + BAR_LINE_TOP), 1.0f, (float)BAR_LINE_LENGTH, 0.0f, colours.line);

	/* The docked window in front on the head, if any. */
	docked = docked_window(server, slot);
	if (docked != NULL) {
		/* Its mark and title, sharing the room before the desktops with its menu. */
		middle = output.y + KWL_GLASS_BAR / 2;
		available = bar.desktops_line - 12 - bar.title_x - 30;
		limit = kwl_titlebar_title_limit(server, docked, available);
		end = title_end(server, docked, limit);
		draw_bar_group_at(server, command, bar.title_x - 7, output.y, 30 + end + 7 + 12, BAR_GROUP_HEIGHT, 1.0f);
		draw_title(server, command, docked, bar.title_x, middle, limit, colours.ink);

		/* Its menu or its controls after the title (titlebar-shell.c). */
		area.x = bar.title_x + 30 + end + 24;
		area.top = output.y;
		area.right = bar.title_x + 30 + available;
		area.height = KWL_GLASS_BAR;
		area.origin = output.x;
		kwl_titlebar_draw(server, command, docked, 1, &area, colours.ink, 1.0f);

		/* Its buttons in their pill, the one under the pointer lit. */
		draw_bar_group_at(server, command, bar.buttons_x, output.y, bar.buttons_width, BAR_BUTTONS_HEIGHT, 1.0f);
		over = -1;
		if (server->pointer_output == slot)
			over = bar_button_at(&bar, server->pointer_x, server->pointer_y);
		for (button = 0; button < BUTTON_COUNT; button++)
			draw_sign(server, command, button, bar.buttons[button], middle, 1, over == button, 1.0f, colours.ink);
	}

	/* Without a docked title, the icons of the applications on the head in its place (apps-bar.c). */
	if (docked == NULL)
		(void)kwl_apps_bar_draw(server, command, slot);

	/* The desktops, then the status and the clock in their pills. */
	draw_desktops(server, command, &bar, &colours);
	draw_bar_group_at(server, command, bar.status_x, output.y, bar.status_width, BAR_GROUP_HEIGHT, 1.0f);
	draw_bar_group_at(server, command, bar.clock_pill_x, output.y, bar.clock_pill_width, BAR_GROUP_HEIGHT, 1.0f);
	draw_status(server, command, &bar, colours.ink);

	/* The rest is drawn in the appearance's colours again. */
	server->keep_colours = 0U;
}

/*
 * Handles a press on a head's bar (ws113-p015): the launcher opens App
 * Home (on the anchor), the clock Calendar, as on the system bar; a docked
 * window's buttons (close, restore, minimize), and on its title a double
 * click restores it and a single press may become a pull out of the bar.
 * The desktops' pill, the status and the applications' icons have taken
 * theirs before (arrange-shell.c, volume.c, network.c, input-method.c,
 * apps-bar.c).  Every press on the bar is taken.  Returns 1.
 */
static int
head_bar_press(
	struct kwl_server *server,
	unsigned slot)
{
	struct kwl_plane_rect output;
	struct kwl_object *surface;
	struct shell_bar bar;
	unsigned second;
	int pressed;
	int running;
	int error;

	/* The launcher, as wide as the system bar's: App Home opens on the anchor, or closes. */
	(void)kwl_output_rect(server, slot, &output);
	bar_layout_on(server, slot, &bar);
	if (server->pointer_x < output.x + KWL_EDGE_LAUNCHER_WIDTH) {
		kwl_home_toggle(server, "head-launcher");
		return 1;
	}

	/* The clock opens Calendar (ws155-p004), joining the anchor's arrangement as from the system bar's clock. */
	if (server->pointer_x >= bar.clock_pill_x && server->pointer_x < bar.clock_pill_x + bar.clock_pill_width) {
		kwl_arrange_join_prepare(server);
		error = kwl_home_open_app(server, "Calendar", "clock", &running);
		kwl_arrange_join_opened(server, error, running);
		return 1;
	}

	/* Otherwise only a docked window acts. */
	surface = docked_window(server, slot);
	if (surface == NULL)
		return 1;

	/* Its buttons. */
	pressed = bar_button_at(&bar, server->pointer_x, server->pointer_y);
	if (pressed == BUTTON_CLOSE) {
		(void)kwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
		printf("KWL GLASS close surface=%u output=%u client=%llu\n", surface->id, slot, (unsigned long long)surface->client->number);
		return 1;
	}

	/* Restore brings it back where it was. */
	if (pressed == BUTTON_MAXIMIZE) {
		layout_leave(server, slot, surface, surface->restore_x, surface->restore_y, "button");
		return 1;
	}

	/* Minimize hides it. */
	if (pressed == BUTTON_MINIMIZE) {
		window_minimize(server, surface);
		return 1;
	}

	/* Its title: up to the buttons. */
	if (server->pointer_x < bar.menu_line || server->pointer_x >= bar.buttons[BUTTON_MINIMIZE] - BUTTON_WIDTH / 2)
		return 1;

	/* A double click brings it back where it was. */
	second = double_click(server, surface);
	if (second) {
		layout_leave(server, slot, surface, surface->restore_x, surface->restore_y, "double-click");
		return 1;
	}

	/* A single press may become a pull. */
	server->pull = surface;
	server->pull_start_x = server->pointer_x;
	server->pull_start_y = server->pointer_y;
	server->pull_distance = 0;
	return 1;
}

/*
 * Draws the bar's strip: dark glass over the blurred scene (the dark
 * appearance's dark glass, which darkens a bright wallpaper further so the
 * light ink keeps its contrast), lighter in a broad band about the middle,
 * a hairline along its bottom; lighter still while a dragged window is over
 * it (it would dock).  It lies at (x, y) of the plane, as wide as given
 * (the anchor's top, or a head's, ws113-p015).
 */
static void
draw_bar_strip(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct glass_bar_colours *colours,
	int32_t x,
	int32_t y,
	int32_t width)
{
	static const float hairline[4] = { 1.0f, 1.0f, 1.0f, 0.09f };
	static const float light_hairline[4] = { 0.12f, 0.16f, 0.24f, 0.10f };
	struct glass_shape shape;
	float left;
	float top;
	float wide;
	int over;

	/*
	 * The light appearance's bar is the floating title bar's white glass
	 * (ws099-p034b), a dark hairline along its bottom; a dragged window over
	 * it (it would dock) darkens it a little.
	 */
	left = (float)x;
	top = (float)y;
	wide = (float)width;
	over = 0;
	if (server->drag != NULL && server->pointer_y >= y && server->pointer_y < y + KWL_GLASS_BAR &&
	    server->pointer_x >= x && server->pointer_x < x + width)
		over = 1;
	if (colours->light) {
		glass_shape_init(&shape, left, top, wide, (float)KWL_GLASS_BAR);
		shape.mode = MODE_GLASS;
		shape.soft = 1.0f;
		shape.color[0] = 1.0f;
		shape.color[1] = 1.0f;
		shape.color[2] = 1.0f;
		shape.color[3] = 0.64f;
		glass_shape_draw(server, command, &shape);
		if (over)
			glass_draw_solid(server, command, left, top, wide, (float)KWL_GLASS_BAR, 0.0f, colours->lit);
		glass_draw_solid(server, command, left, top + (float)(KWL_GLASS_BAR - 1), wide, 1.0f, 0.0f, light_hairline);
		return;
	}

	/* The dark glass, a little bluish. */
	glass_shape_init(&shape, left, top, wide, (float)KWL_GLASS_BAR);
	shape.mode = MODE_GLASS;
	shape.dark_glass = 1U;
	shape.soft = 1.0f;
	shape.color[0] = 0.07f;
	shape.color[1] = 0.094f;
	shape.color[2] = 0.14f;
	shape.color[3] = 0.64f;
	glass_shape_draw(server, command, &shape);

	/*
	 * The lighter middle: a soft white band, whole over the middle third and
	 * fading out over a third of the width on each side, clipped to the bar.
	 */
	glass_shape_init(&shape, left + wide / 3.0f, top - 200.0f, wide / 3.0f, (float)KWL_GLASS_BAR + 400.0f);
	shape.quad[0] = left;
	shape.quad[1] = top;
	shape.quad[2] = wide;
	shape.quad[3] = (float)KWL_GLASS_BAR;
	shape.mode = MODE_SHADOW;
	shape.soft = wide / 3.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.07f;
	if (over)
		shape.color[3] = 0.16f;
	glass_shape_draw(server, command, &shape);

	/* The hairline. */
	glass_draw_solid(server, command, left, top + (float)(KWL_GLASS_BAR - 1), wide, 1.0f, 0.0f, hairline);
}

/* Draws one of the bar's group pills: a little darker than the bar, with a faint edge, centred on the bar's middle. */
static void
draw_bar_group(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t width,
	int32_t height)
{
	/* Whole. */
	draw_bar_group_faded(server, command, x, width, height, 1.0f);
}

/* Draws one of the bar's group pills at an opacity (the bar's colours in the appearance shown, ws099-p034b). */
static void
draw_bar_group_faded(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t width,
	int32_t height,
	float opacity)
{
	/* In the system bar. */
	draw_bar_group_at(server, command, x, 0, width, height, opacity);
}

/* Draws a pill of a bar whose top is at bar_top of the plane (the system bar's, or a head's, ws113-p015), faded by an opacity. */
static void
draw_bar_group_at(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t bar_top,
	int32_t width,
	int32_t height,
	float opacity)
{
	struct glass_bar_colours colours;
	struct glass_shape shape;
	float fill[4];
	float top;

	/* The pill. */
	kwl_glass_bar_colours(server, &colours);
	memcpy(fill, colours.fill, sizeof(fill));
	fill[3] *= opacity;
	top = (float)(bar_top + KWL_GLASS_BAR_MIDDLE - height / 2);
	glass_draw_solid(server, command, (float)x, top, (float)width, (float)height, (float)height * 0.5f, fill);

	/* Its edge. */
	glass_shape_init(&shape, (float)x, top, (float)width, (float)height);
	shape.mode = MODE_RING;
	shape.radius = (float)height * 0.5f;
	shape.soft = 1.0f;
	memcpy(shape.color, colours.edge, sizeof(shape.color));
	shape.color[3] *= opacity;
	glass_shape_draw(server, command, &shape);
}

/*
 * Draws a docked window's buttons in their pill at the bar's right end
 * (ws099-p034b): faded in and grown from BAR_DOCK_SCALE as the docked
 * layout comes in, out as it goes; the one under the pointer lit while a
 * window is docked.
 */
static void
draw_bar_buttons(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct shell_bar *bar,
	struct kwl_object *docked,
	const struct glass_bar_colours *colours)
{
	float ink[4];
	float scale;
	float t;
	int32_t width;
	int32_t x;
	int32_t cx;
	int32_t middle;
	int button;
	int over;

	/* Nothing while the floating layout shows. */
	t = server->bar_dock;
	if (t <= 0.0f)
		return;

	/* The pill, as large as the layout has come in, about its middle. */
	scale = BAR_DOCK_SCALE + (1.0f - BAR_DOCK_SCALE) * t;
	width = (int32_t)((float)bar->buttons_width * scale + 0.5f);
	middle = bar->buttons_x + bar->buttons_width / 2;
	x = middle - width / 2;
	draw_bar_group_faded(server, command, x, width, (int32_t)((float)BAR_BUTTONS_HEIGHT * scale + 0.5f), t);

	/* The buttons, faded with it, the one under the pointer lit while a window is docked. */
	memcpy(ink, colours->ink, sizeof(ink));
	over = -1;
	if (docked != NULL)
		over = bar_button_at(bar, server->pointer_x, server->pointer_y);
	for (button = 0; button < BUTTON_COUNT; button++) {
		cx = middle + (int32_t)((float)(bar->buttons[button] - middle) * scale);
		draw_sign(server, command, button, cx, KWL_GLASS_BAR / 2, 1, over == button, t, ink);
	}
}

/*
 * Draws the virtual desktops in a bar: a pill with each desktop's
 * silhouette, the shown one in the accent; on the system bar Wiseview
 * brings the pill forward with a blue edge.
 */
static void
draw_desktops(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct shell_bar *bar,
	const struct glass_bar_colours *colours)
{
	struct glass_shape shape;
	float current[4];
	const float *ink;
	float progress;
	int32_t x;
	int desktop;

	/* The current desktop's colour: the accent the user chose, on the bar's ground. */
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, current);

	/* The pill. */
	draw_bar_group_at(server, command, bar->desktops_x, bar->top, bar->desktops_width, BAR_PILL_HEIGHT, 1.0f);

	/* Wiseview (the anchor's) brings the system bar's desktops forward with a blue edge. */
	progress = 0.0f;
	if (bar->output == KWL_PLANE_ANCHOR)
		progress = wiseview_progress(server);
	if (progress > 0.0f) {
		glass_shape_init(&shape, (float)bar->desktops_x, (float)BAR_PILL_TOP, (float)bar->desktops_width, (float)BAR_PILL_HEIGHT);
		shape.quad[0] -= 1.0f;
		shape.quad[1] -= 1.0f;
		shape.quad[2] += 2.0f;
		shape.quad[3] += 2.0f;
		shape.mode = MODE_RING;
		shape.radius = (float)BAR_PILL_HEIGHT * 0.5f;
		shape.soft = 1.5f;
		memcpy(shape.color, current, sizeof(shape.color));
		shape.light = 1U;
		shape.opacity = progress * 0.8f;
		glass_shape_draw(server, command, &shape);
	}

	/* Where the system bar's slots are, when they first show and when they moved (the tests click a desktop's slot). */
	if (bar->output == KWL_PLANE_ANCHOR && (!shell_desktops_logged || shell_desktops_logged_x != bar->desktops_x)) {
		shell_desktops_logged = 1U;
		shell_desktops_logged_x = bar->desktops_x;
		printf("KWL GLASS desktops x=%d step=%d width=%d\n", bar->desktops_x + DESKTOPS_PAD, DESKTOP_WIDTH + DESKTOP_GAP, DESKTOP_WIDTH);
	}

	/* Each desktop's silhouette in the middle of its slot: the cat, the bird and the rabbit, the desktop shown in the accent. */
	for (desktop = 0; desktop < DESKTOPS; desktop++) {
		x = bar->desktops_x + DESKTOPS_PAD + desktop * (DESKTOP_WIDTH + DESKTOP_GAP) + (DESKTOP_WIDTH - (int32_t)DESKTOP_ICON) / 2;
		ink = colours->faint;
		if (desktop == (int)server->desktop)
			ink = current;
		glass_draw_icon(server, command, (unsigned)GLASS_ICON_DESKTOP_CAT + (unsigned)desktop, x, bar->top + KWL_GLASS_BAR_MIDDLE - (int32_t)DESKTOP_ICON / 2, DESKTOP_ICON, ink);
	}
}

/*
 * Draws a bar's status at the right in an ink: the input method's
 * language, the removable media, the network, the volume and the battery
 * when the machine has one, and the clock (their pills are the caller's).
 */
static void
draw_status(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct shell_bar *bar,
	const float *ink)
{
	/* Where the system bar's status and clock are, when they first show and when they moved. */
	if (bar->output == KWL_PLANE_ANCHOR &&
	    (shell_status_logged[0] != bar->status_x ||
	     shell_status_logged[1] != bar->status_width ||
	     shell_status_logged[2] != bar->clock_pill_x)) {
		shell_status_logged[0] = bar->status_x;
		shell_status_logged[1] = bar->status_width;
		shell_status_logged[2] = bar->clock_pill_x;
		printf("KWL GLASS status left=%d width=%d clock=%d\n", bar->status_x, bar->status_width, bar->clock_pill_x);
	}

	/* The date and time. */
	glass_draw_text(server, command, SIZE_BAR, bar->clock_x, bar->top + BAR_BASELINE, bar->clock, 400, ink);

	/* The battery, when the machine has one (ws132-p003). */
	if (server->power.percent >= 0)
		draw_battery(server, command, bar->battery_x, bar->top + KWL_GLASS_BAR_MIDDLE, server->power.percent, server->power.charging, ink);

	/* The network: Wi-Fi's fan or the wired tree, which opens its menu (network.c). */
	kwl_network_draw_icon(server, command, bar->signal_x, bar->top, ink);

	/* Bluetooth's rune while there is a controller, which opens its menu (bluetooth-bar.c, ws143-p006). */
	kwl_bluetooth_draw_icon(server, command, bar->bluetooth_x, bar->top, ink);

	/* The volume's speaker, which opens its popup (volume.c, ws100-p004). */
	kwl_volume_draw_icon(server, command, bar->volume_x, bar->top, ink);

	/* The removable media's stick, which starts Files (media.c). */
	kwl_media_draw_icon(server, command, bar->media_x, ink);

	/* The input method's language (A, あ), which a click changes (input-method.c). */
	kwl_ime_indicator_draw(server, command, bar->ime_x, bar->top, ink);
}

/*
 * Draws what the bar keeps over App Home (the 2026-10-07 UAT): the status
 * and the clock where they always are, in white, without the strip and
 * without their pills; as far as opacity says while the bar fades into
 * them (ws177-p038).
 */
static void
draw_home_status(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct shell_bar *bar,
	float opacity)
{
	float white[4];

	/* Nothing while the bar shows whole. */
	if (opacity <= 0.0f)
		return;

	/* White, faded in with Home. */
	white[0] = 1.0f;
	white[1] = 1.0f;
	white[2] = 1.0f;
	white[3] = 0.96f * opacity;

	/* White as it is, not mapped by the appearance, while it is drawn. */
	server->keep_colours = 1U;
	draw_status(server, command, bar, white);
	server->keep_colours = 0U;
}

/*
 * Tells whether a button over App Home goes past Home to the status and the
 * clock the bar keeps there (the 2026-10-07 UAT): a press on the status or
 * the clock, and every button while the volume's popup or the network's
 * menu is open, as outside Home.  A press Home started keeps its buttons.
 * Returns 1 when it goes past, 0 when it is Home's (or Home is not shown).
 */
static int
home_bar_passes(
	struct kwl_server *server,
	uint32_t state)
{
	struct shell_bar bar;
	float home;
	int open;

	/* Only over App Home, opening, open or closing. */
	home = kwl_home_progress(server);
	if (home <= 0.0f && server->home_to <= 0.0f)
		return 0;

	/* A press Home started has its release and its motion. */
	if (server->home_press || server->home_page_press || server->home_rise_press)
		return 0;

	/* The volume's popup, the network's menu and Bluetooth's, while open, have every button. */
	open = kwl_volume_is_open();
	if (open)
		return 1;
	open = kwl_network_is_open();
	if (open)
		return 1;
	open = kwl_bluetooth_is_open();
	if (open)
		return 1;

	/* Only a press in the bar's height goes to the status or the clock. */
	if (state == 0 || server->pointer_y >= KWL_GLASS_BAR)
		return 0;

	/* The status pill's place. */
	bar_layout(server, &bar);
	if (server->pointer_x >= bar.status_x && server->pointer_x < bar.status_x + bar.status_width)
		return 1;

	/* The clock's place. */
	if (server->pointer_x >= bar.clock_pill_x && server->pointer_x < bar.clock_pill_x + bar.clock_pill_width)
		return 1;

	/* Succeeded: the button is Home's. */
	return 0;
}

/*
 * Draws a bar's battery at x about the bar's middle: an outline, its charge
 * (the inside filled in proportion to percent, at least a sliver while
 * above zero), its terminal, and a "+" right of it while it charges.
 */
static void
draw_battery(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t middle,
	int percent,
	unsigned charging,
	const float *ink)
{
	struct glass_shape shape;
	float fill;

	/* The outline. */
	glass_shape_init(&shape, (float)x, (float)(middle - 6), 22.0f, 12.0f);
	shape.quad[0] -= 1.0f;
	shape.quad[1] -= 1.0f;
	shape.quad[2] += 2.0f;
	shape.quad[3] += 2.0f;
	shape.mode = MODE_RING;
	shape.radius = 3.5f;
	shape.soft = 1.3f;
	memcpy(shape.color, ink, sizeof(shape.color));
	glass_shape_draw(server, command, &shape);

	/* The charge: 16 pixels at 100 %, a sliver above 0 %, nothing at 0 %. */
	if (percent > 100)
		percent = 100;
	fill = 16.0f * (float)percent / 100.0f;
	if (percent > 0 && fill < 2.0f)
		fill = 2.0f;
	if (fill > 0.0f)
		glass_draw_solid(server, command, (float)(x + 3), (float)(middle - 3), fill, 6.0f, 1.5f, ink);

	/* The terminal. */
	glass_draw_solid(server, command, (float)(x + 23), (float)(middle - 2), 2.0f, 4.0f, 1.0f, ink);

	/* Charging: a "+" between the terminal and the clock. */
	if (charging != 0U) {
		glass_draw_solid(server, command, (float)(x + 29), (float)middle - 0.75f, 7.0f, 1.5f, 0.5f, ink);
		glass_draw_solid(server, command, (float)(x + 31.75f), (float)(middle - 3) - 0.5f, 1.5f, 7.0f, 0.5f, ink);
	}
}

/*
 * Shows where a dragged window would dock while the pointer is in the
 * system bar: a pale rounded rectangle with a blue edge over the space the
 * docked body would take.
 */
static void
draw_dock_hint(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float fill[4] = { 1.0f, 1.0f, 1.0f, 0.22f };
	struct glass_shape shape;
	struct shell_rect body;

	/* Only while a move is in the system bar. */
	if (server->drag == NULL || server->pointer_y >= KWL_GLASS_BAR)
		return;

	/* The space, filled and outlined. */
	docked_rect(server, KWL_PLANE_ANCHOR, &body);
	glass_draw_solid(server, command, (float)(body.x + 6), (float)(body.y + 6), (float)(body.width - 12), (float)(body.height - 12), GLASS_RADIUS, fill);
	glass_shape_init(&shape, (float)(body.x + 6), (float)(body.y + 6), (float)(body.width - 12), (float)(body.height - 12));
	shape.quad[0] -= 1.0f;
	shape.quad[1] -= 1.0f;
	shape.quad[2] += 2.0f;
	shape.quad[3] += 2.0f;
	shape.mode = MODE_RING;
	shape.radius = GLASS_RADIUS;
	shape.soft = 2.0f;
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.9f, shape.color);
	shape.light = 1U;
	glass_shape_draw(server, command, &shape);
}

/* How far the dock animation is, from 0 to 1, eased (1 without an animation). */
static float
animation_progress(
	struct kwl_server *server)
{
	uint64_t duration;
	uint64_t elapsed;
	float t;

	/* No animation is finished. */
	if (server->anim == NULL)
		return 1.0f;

	/* A launched window grows for LAUNCH_MS; a dock and an undock take DOCK_MS. */
	duration = DOCK_MS;
	if (server->anim_docking == ANIM_LAUNCH)
		duration = LAUNCH_MS;

	/* The time since it started, as a fraction of its duration. */
	elapsed = kwl_milliseconds() - server->anim_start_ms;
	if (elapsed >= duration)
		return 1.0f;
	t = (float)elapsed / (float)duration;

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

/* Tells whether a window (its body and floating title bar) comes into a rectangle. */
static int
damage_near(
	struct kwl_server *server,
	struct kwl_object *surface,
	const struct shell_rect *near)
{
	struct shell_rect body;
	int32_t top;

	/* The body, with its floating title bar above it. */
	body_rect(server, surface, &body);
	top = body.y;
	if (!surface->maximized)
		top = body.y - KWL_GLASS_TITLE - KWL_GLASS_GAP;

	/* Apart across or down. */
	if (body.x + body.width <= near->x || near->x + near->width <= body.x)
		return 0;
	if (body.y + body.height <= near->y || near->y + near->height <= top)
		return 0;

	/* It comes in. */
	return 1;
}

/* Where a window's body is drawn: on its way while animated, the docked space, or its own place and size. */
static void
body_rect(
	struct kwl_server *server,
	const struct kwl_object *surface,
	struct shell_rect *body)
{
	struct shell_rect from;
	struct shell_rect to;
	int32_t glide[4];
	uint64_t held;
	float t;
	int gliding;

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

	/* Docked: the docked space, or the middle of it for a window of one size (ws142-p008). */
	if (surface->maximized) {
		docked_body(server, surface, body);
		return;
	}

	/* An arranged window gliding into its slot (arrange-shell.c, WS181). */
	gliding = kwl_arrange_glide(server, surface, glide);
	if (gliding) {
		body->x = glide[0];
		body->y = glide[1];
		body->width = glide[2];
		body->height = glide[3];
		return;
	}

	/*
	 * Brought back, but its client has not drawn that size yet: its old
	 * docked image is drawn at the size it was sent, so the window never
	 * shows the docked size again on its way back (BUG-180).
	 */
	if (surface->resized_ms != 0U) {
		held = kwl_milliseconds() - surface->resized_ms;
		if (held < RESIZED_HOLD_MS) {
			body->x = surface->x;
			body->y = surface->y;
			body->width = (int32_t)surface->window_width;
			body->height = (int32_t)surface->window_height;
			return;
		}
	}

	/* Its place, its image's size. */
	body->x = surface->x;
	body->y = surface->y;
	window_size(surface, &body->width, &body->height);
}

/*
 * The space a docked body takes on an output: under its bar (the system
 * bar on the anchor, the head's own bar on a head, ws113-p015).
 */
static void
docked_rect(
	struct kwl_server *server,
	unsigned slot,
	struct shell_rect *body)
{
	struct kwl_plane_rect output;
	int32_t right;
	int32_t bottom;

	/* A head: under its bar, KWL_GLASS_DOCK_PAD in from every side (no on-screen keyboard there). */
	if (slot != KWL_PLANE_ANCHOR) {
		(void)kwl_output_rect(server, slot, &output);
		body->x = output.x + KWL_GLASS_DOCK_PAD;
		body->y = output.y + DOCK_TOP;
		body->width = (int32_t)output.width - 2 * KWL_GLASS_DOCK_PAD;
		body->height = (int32_t)output.height - DOCK_TOP - KWL_GLASS_DOCK_PAD;
		return;
	}

	/* What the on-screen keyboard's panel takes now, as it slides (keyboard.c, ws102-p007). */
	kwl_keyboard_reserved_now(&right, &bottom);

	/* Under the bar to the bottom, less the keyboard's column or row, KWL_GLASS_DOCK_PAD in from every side (ws099-p038). */
	body->x = KWL_GLASS_DOCK_PAD;
	body->y = DOCK_TOP;
	body->width = (int32_t)server->width - right - 2 * KWL_GLASS_DOCK_PAD;
	body->height = (int32_t)server->height - DOCK_TOP - bottom - KWL_GLASS_DOCK_PAD;
}

/* Gives the output a window is on (plane.h's slot), the anchor for one out of range. */
static unsigned
window_slot(
	const struct kwl_object *surface)
{
	/* A slot of the plane, else the anchor. */
	if (surface->output >= KWL_PLANE_SLOTS)
		return KWL_PLANE_ANCHOR;
	return surface->output;
}

/*
 * Moves a place so that a body of a size ends inside the space of an
 * output for floating windows (kwl_glass_fit's on the anchor; on a head
 * its rectangle less the margins, under a title bar at its top,
 * ws113-p015).
 */
static void
glass_fit_on(
	struct kwl_server *server,
	unsigned slot,
	int32_t width,
	int32_t height,
	int32_t *x,
	int32_t *y)
{
	struct kwl_plane_rect output;
	int32_t right;
	int32_t bottom;
	int32_t left;
	int32_t top;
	int shown;

	/* The anchor's space. */
	if (slot == KWL_PLANE_ANCHOR) {
		kwl_glass_fit(server, width, height, x, y);
		return;
	}

	/* A head going away keeps the place (heads.c carries it to the anchor). */
	shown = kwl_output_rect(server, slot, &output);
	if (!shown)
		return;

	/* The head's: its right and bottom edges inside, then never above nor left of it. */
	left = output.x + KWL_GLASS_MARGIN;
	top = output.y + kwl_output_top(server, slot);
	right = output.x + (int32_t)output.width - KWL_GLASS_MARGIN;
	bottom = output.y + (int32_t)output.height - KWL_GLASS_MARGIN;
	if (*x + width > right)
		*x = right - width;
	if (*y + height > bottom)
		*y = bottom - height;
	if (*x < left)
		*x = left;
	if (*y < top)
		*y = top;
}

/* The floating title bar of a body: as wide as it, a gap above it. */
static void
floating_title(
	const struct shell_rect *body,
	struct shell_rect *panel)
{
	/* Above the body. */
	panel->x = body->x;
	panel->y = body->y - KWL_GLASS_GAP - KWL_GLASS_TITLE;
	panel->width = body->width;
	panel->height = KWL_GLASS_TITLE;
}

/*
 * The place in the system bar a docking title bar slides to: its mark lands
 * where the docked title's mark is drawn, and it ends at the docked buttons.
 */
static void
bar_title_slot(
	struct kwl_server *server,
	const struct shell_bar *bar,
	struct shell_rect *slot)
{
	/* The bar's height, from the mark's place to past the buttons. */
	(void)server;
	slot->x = bar->title_x - 14;
	slot->y = bar->top;
	slot->width = bar->buttons[BUTTON_CLOSE] + 26 - slot->x;
	slot->height = KWL_GLASS_BAR;
}

/* The size of a window's image: its viewport's size, else its buffer's (viewport.c); 0 by 0 without an image. */
static void
window_size(
	const struct kwl_object *surface,
	int32_t *width,
	int32_t *height)
{
	uint32_t surface_width;
	uint32_t surface_height;

	/* No image, no size. */
	*width = 0;
	*height = 0;
	if (surface->current == NULL)
		return;

	/* The surface's size (ws035-p081: a viewport's destination or source). */
	kwl_surface_size(surface, &surface_width, &surface_height);
	*width = (int32_t)surface_width;
	*height = (int32_t)surface_height;
}

/* Tells whether a point is on a window's title bar, its body, its frame, or none of them (the gap is none). */
static enum shell_hit
window_hit(
	struct kwl_server *server,
	const struct kwl_object *surface,
	int32_t x,
	int32_t y)
{
	struct shell_rect body;
	struct kwl_object *parent;
	uint32_t edges;
	int32_t top;
	int decorated;

	/* A sheet has only its body: no title bar, no frame (ws090-p014). */
	body_rect(server, surface, &body);
	parent = kwl_sheet_parent(surface);
	if (parent != NULL) {
		if (x >= body.x &&
		    x < body.x + body.width &&
		    y >= body.y &&
		    y < body.y + body.height)
			return HIT_BODY;
		return HIT_NONE;
	}

	/* A CSD surface has no hidden titlebar or server resize band to consume input. */
	decorated = kwl_decoration_server(surface);
	if (!decorated) {
		if (x >= body.x &&
		    x < body.x + body.width &&
		    y >= body.y &&
		    y < body.y + body.height)
			return HIT_BODY;

		/* The client owns only its surface extent. */
		return HIT_NONE;
	}

	/* Within the window's columns: its body, or its floating title bar (a docked window's title is in the system bar). */
	if (x >= body.x && x < body.x + body.width) {
		if (y >= body.y && y < body.y + body.height)
			return HIT_BODY;
		top = body.y - KWL_GLASS_GAP - KWL_GLASS_TITLE;
		if (!surface->maximized && y >= top && y < top + KWL_GLASS_TITLE)
			return HIT_TITLE;
	}

	/* The frame around a floating window resizes it. */
	edges = frame_edges(server, surface, x, y);
	if (edges != 0U)
		return HIT_FRAME;

	/* Neither (the gap between the title bar and the body is neither too). */
	return HIT_NONE;
}

/*
 * Tells which edges of a floating window's frame are at a point (KWL_EDGE_*
 * bits of toplevel.h: one side, or a corner's two), or 0 off its frame.
 *
 * The frame is the band FRAME_BAND wide around the title bar and the body.
 * On the band above or below them, within FRAME_CORNER of the left or right
 * end, the side's edge joins the corner's; on the band left or right of
 * them, within FRAME_CORNER of the top or bottom, likewise.
 */
static uint32_t
frame_edges(
	struct kwl_server *server,
	const struct kwl_object *surface,
	int32_t x,
	int32_t y)
{
	struct shell_rect body;
	struct kwl_object *parent;
	uint32_t edges;
	int32_t left;
	int32_t right;
	int32_t top;
	int32_t bottom;
	int decorated;

	/* CSD clients request interactive resize from their own visible edges. */
	decorated = kwl_decoration_server(surface);
	if (!decorated)
		return 0U;

	/* Only a floating toplevel window has a frame: not a docked one, nor one moving, animated or without an image. */
	if (surface->maximized ||
	    surface->fullscreen ||
	    surface->role == NULL ||
	    surface->role->top == NULL ||
	    surface->current == NULL ||
	    server->anim == surface)
		return 0U;

	/* Nor a sheet (ws090-p014). */
	parent = kwl_sheet_parent(surface);
	if (parent != NULL)
		return 0U;

	/* The window's outline: from the title bar's top to the body's bottom. */
	body_rect(server, surface, &body);
	left = body.x;
	right = body.x + body.width;
	top = body.y - KWL_GLASS_GAP - KWL_GLASS_TITLE;
	bottom = body.y + body.height;

	/* Nothing beyond the band around the outline. */
	if (x < left - FRAME_BAND || x >= right + FRAME_BAND)
		return 0U;
	if (y < top - FRAME_BAND || y >= bottom + FRAME_BAND)
		return 0U;

	/* Nothing within the outline (the title bar, the body, the gap between them). */
	if (x >= left && x < right && y >= top && y < bottom)
		return 0U;

	/* The side the point is beyond, or the two sides at a corner. */
	edges = 0U;
	if (x < left)
		edges |= KWL_EDGE_LEFT;
	if (x >= right)
		edges |= KWL_EDGE_RIGHT;
	if (y < top)
		edges |= KWL_EDGE_TOP;
	if (y >= bottom)
		edges |= KWL_EDGE_BOTTOM;

	/* Above or below the outline, near its left or right end: that corner. */
	if ((edges & (KWL_EDGE_TOP | KWL_EDGE_BOTTOM)) != 0U) {
		if (x < left + FRAME_CORNER)
			edges |= KWL_EDGE_LEFT;
		if (x >= right - FRAME_CORNER)
			edges |= KWL_EDGE_RIGHT;
	}

	/* Left or right of the outline, near its top or bottom: that corner. */
	if ((edges & (KWL_EDGE_LEFT | KWL_EDGE_RIGHT)) != 0U) {
		if (y < top + FRAME_CORNER)
			edges |= KWL_EDGE_TOP;
		if (y >= bottom - FRAME_CORNER)
			edges |= KWL_EDGE_BOTTOM;
	}

	/* A window narrower or shorter than two corners keeps one side of each pair: the right, the bottom. */
	if ((edges & (KWL_EDGE_LEFT | KWL_EDGE_RIGHT)) == (KWL_EDGE_LEFT | KWL_EDGE_RIGHT))
		edges &= ~KWL_EDGE_LEFT;
	if ((edges & (KWL_EDGE_TOP | KWL_EDGE_BOTTOM)) == (KWL_EDGE_TOP | KWL_EDGE_BOTTOM))
		edges &= ~KWL_EDGE_TOP;

	/* Succeeded: the frame's edges at the point. */
	return edges;
}

/* Tells which frame edges of the top window at the pointer the pointer is on, or 0. */
static uint32_t
frame_under_pointer(
	struct kwl_server *server)
{
	struct kwl_object *surface;
	enum shell_hit hit;
	uint32_t edges;

	/*
	 * The anchor's own strips take a press before any frame (kwl_glass_button):
	 * the system bar, the desktops' swipe at the left and right edges, and
	 * Wiseview's at the bottom.  A frame there shows no resize arrow.  A
	 * head has none of them (ws113-p015: its frames showed no arrow, since
	 * all of a head is beyond the anchor's right edge).
	 */
	if (server->pointer_output == KWL_PLANE_ANCHOR) {
		if (server->pointer_y < KWL_GLASS_BAR)
			return 0U;
		if (server->pointer_y >= (int32_t)server->height - WISEVIEW_EDGE)
			return 0U;
		if (server->pointer_x < DESKTOP_EDGE)
			return 0U;
		if (server->pointer_x >= (int32_t)server->width - DESKTOP_EDGE)
			return 0U;
	}

	/* The top window at the pointer, when it is its frame that is there. */
	surface = window_at(server, server->pointer_x, server->pointer_y, &hit);
	if (surface == NULL || hit != HIT_FRAME)
		return 0U;

	/* Succeeded: that frame's edges. */
	edges = frame_edges(server, surface, server->pointer_x, server->pointer_y);
	return edges;
}

/* Returns the floating title bar button at a point, or -1. */
static int
button_at(
	const struct kwl_object *surface,
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
	const struct kwl_object *surface,
	int button,
	int32_t *x,
	int32_t *y)
{
	int32_t width;
	int32_t height;

	/* From the bar's right edge, in the middle of its height. */
	window_size(surface, &width, &height);
	*x = surface->x + width - 26 - button * BUTTON_SPACING;
	*y = surface->y - KWL_GLASS_GAP - KWL_GLASS_TITLE / 2;
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
	if (y < bar->top || y >= bar->top + KWL_GLASS_BAR)
		return -1;
	for (button = 0; button < BUTTON_COUNT; button++) {
		if (x >= bar->buttons[button] - BUTTON_WIDTH / 2 && x < bar->buttons[button] + BUTTON_WIDTH / 2)
			return button;
	}

	/* None. */
	return -1;
}

/* Finds the topmost window whose title bar or body is at a point. */
static struct kwl_object *
window_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	enum shell_hit *hit)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *found;
	enum shell_hit place;
	unsigned output;
	int hidden;

	/* The hit with the highest map order, of the windows the output at the point shows (ws113-p007). */
	found = NULL;
	*hit = HIT_NONE;
	output = kwl_output_at(server, x, y);
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only mapped windows of the desktop shown, on that output. */
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role ||
			    surface->desktop != server->desktop ||
			    surface->minimized ||
			    surface->output != output)
				continue;

			/* Not a window the docked mode leaves out of the scene (ws142-p008). */
			hidden = layout_hides(server, surface);
			if (hidden)
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

/*
 * The docked window whose title an output's bar shows (the system bar's,
 * or a head's, ws113-p015): the output's top window, docked and not
 * moving.
 */
static struct kwl_object *
docked_window(
	struct kwl_server *server,
	unsigned slot)
{
	struct kwl_object *parent;
	struct kwl_object *top;
	int decorated;

	/* None while Wiseview shows. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving)
		return NULL;

	/* The output's top window (a sheet's parent for a sheet, ws090-p014). */
	top = kwl_output_top_window(server, slot);
	parent = kwl_sheet_parent(top);
	if (parent != NULL)
		top = parent;
	if (top == NULL || !top->maximized || server->anim == top)
		return NULL;
	if (server->pull == top && server->pull_distance > 0)
		return NULL;

	/* CSD windows retain their own controls when maximized; the system bar adds none. */
	decorated = kwl_decoration_server(top);
	if (!decorated)
		return NULL;

	/* Succeeded: this maximized SSD window supplies the system bar's titlebar. */
	return top;
}

/* Brings a window to the top and gives it the focus. */
static void
window_raise(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct kwl_object *top;
	struct kwl_object *parent;
	struct kwl_object *sheet;

	/* A sheet comes with its parent, and a parent with its sheet, which has the focus (ws090-p014). */
	parent = kwl_sheet_parent(surface);
	if (parent != NULL) {
		sheet = surface;
		surface = parent;
	} else {
		sheet = kwl_sheet_of(surface);
	}

	/* Already on top. */
	top = kwl_top_window(server);
	if (surface == top || (sheet != NULL && sheet == top))
		return;

	/* The highest map order (the sheet's above its parent's), and the focus follows. */
	server->map_order++;
	surface->map_order = server->map_order;
	server->front_surface = surface;
	if (sheet != NULL) {
		server->map_order++;
		sheet->map_order = server->map_order;
		server->front_surface = sheet;
	}

	/* The keyboard goes to the front window. */
	kwl_seat_focus(server);
	server->dirty = 1;
}

/*
 * Keeps each sheet where its parent is (ws090-p014): its top at the bottom
 * of the parent's title bar (sliding down out of it for SHEET_MS after it
 * began to show), in the middle of the parent's width, on the parent's
 * desktop and hidden with it.
 */
static void
sheet_place(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *parent;
	uint64_t now;
	int32_t width;
	int32_t height;
	int32_t x;
	int32_t y;
	int32_t top;
	int moved;
	float t;

	/* Every window that is a sheet now. */
	now = kwl_milliseconds();
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != KWL_SURFACE)
				continue;
			parent = kwl_sheet_parent(surface);
			if (parent == NULL)
				continue;

			/* Its time from when it began to show, eased out; one wider than its parent is asked to be narrower. */
			window_size(surface, &width, &height);
			if (surface->sheet_ms == 0U) {
				surface->sheet_ms = now;
				sheet_narrow(server, surface, parent, width, height);
				printf("KWL GLASS sheet surface=%u parent=%u width=%d height=%d client=%llu\n", surface->id, parent->id, width, height, (unsigned long long)surface->client->number);
			}

			/* How far it has slid out, eased out, and frames asked for until it has. */
			t = (float)(now - surface->sheet_ms) / (float)SHEET_MS;
			if (t < 1.0f)
				server->dirty = 1;
			if (t > 1.0f)
				t = 1.0f;
			t = 1.0f - (1.0f - t) * (1.0f - t);

			/* Its place: under the parent's title bar, risen by what has not slid out yet. */
			sheet_anchor(server, parent, width, height, &x, &top);
			y = top - (int32_t)((1.0f - t) * (float)height);
			moved = surface->x != x || surface->y != y;
			if (moved)
				server->dirty = 1;
			surface->x = x;
			surface->y = y;

			/* Its place once it has slid out, whenever it changes (the tests read it). */
			if (moved && t >= 1.0f)
				printf("KWL GLASS sheet at x=%d y=%d parent=%u\n", x, y, parent->id);

			/* The parent's desktop, shown or hidden with it. */
			surface->desktop = parent->desktop;
			surface->minimized = parent->minimized;
		}
	}
}

/* The window a surface is shown with: a sheet's parent, or the surface itself (ws090-p014). */
static struct kwl_object *
sheet_owner(
	struct kwl_object *surface)
{
	struct kwl_object *parent;

	/* A sheet is its parent's. */
	parent = kwl_sheet_parent(surface);
	if (parent != NULL)
		return parent;
	return surface;
}

/*
 * Asks a sheet as wide as its parent or wider to be SHEET_MARGIN narrower
 * on each side than the parent's body, when that is not too narrow for it
 * (its smallest width, or SHEET_NARROWEST).
 */
static void
sheet_narrow(
	struct kwl_server *server,
	struct kwl_object *surface,
	struct kwl_object *parent,
	int32_t width,
	int32_t height)
{
	struct shell_rect body;
	int32_t wanted;

	/* The width the parent leaves it. */
	body_rect(server, parent, &body);
	wanted = body.width - 2 * SHEET_MARGIN;
	if (width <= wanted || wanted < SHEET_NARROWEST || wanted < surface->min_width)
		return;

	/* Its new size, told to it. */
	surface->window_width = (uint32_t)wanted;
	surface->window_height = (uint32_t)height;
	window_configure(surface);
	printf("KWL GLASS sheet narrower surface=%u width=%d client=%llu\n", surface->id, wanted, (unsigned long long)surface->client->number);
}

/*
 * The place of a sheet of a size under its parent: the left of its middle
 * over the parent's body, and its top at the bottom of the parent's title
 * bar (the top of the body of a docked or fullscreen parent).  In the
 * docked mode a docked parent's sheet (the File Chooser among them) is in
 * the middle of the screen instead (ws142-p008, the 2026-10-06 user
 * decision).
 */
static void
sheet_anchor(
	struct kwl_server *server,
	struct kwl_object *parent,
	int32_t width,
	int32_t height,
	int32_t *x,
	int32_t *top)
{
	struct shell_rect body;
	int centred;

	/* The parent's body where it is now. */
	body_rect(server, parent, &body);
	*x = body.x + (body.width - width) / 2;

	/* The middle of the docked parent's body, in the docked mode. */
	centred = sheet_centred(server, parent);
	if (centred) {
		kwl_layout_centre(body.x, body.y, body.width, body.height, width, height, x, top);
		return;
	}

	/* A floating parent's title bar ends a gap above its body; a docked or fullscreen one has none there. */
	*top = body.y - KWL_GLASS_GAP;
	if (parent->maximized || parent->fullscreen)
		*top = body.y;
}

/* Tells whether a parent's sheet is in the middle of the screen: the parent docked, in the docked mode. */
static int
sheet_centred(
	struct kwl_server *server,
	const struct kwl_object *parent)
{
	unsigned slot;

	/* A floating or fullscreen parent hangs its sheet under its title bar. */
	if (!parent->maximized)
		return 0;

	/* So does a docked parent left behind in the windowed mode of its output. */
	slot = window_slot(parent);
	if (server->layout_mode[slot] != KWL_LAYOUT_DOCKED)
		return 0;

	/* Succeeded: the sheet is in the middle. */
	return 1;
}

/*
 * Draws a sheet (ws090-p014): its body only, cut at the bottom of its
 * parent's title bar (its shadow does not fall on the title bar, and it
 * slides down out of it), with a hairline along that seam.
 */
static void
draw_sheet(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	struct kwl_object *parent,
	unsigned focused)
{
	static const float seam[4] = { 0.12f, 0.16f, 0.24f, 0.14f };
	struct shell_rect body;
	VkRect2D saved;
	VkRect2D cut;
	int32_t x;
	int32_t top;
	int32_t cut_top;
	int32_t bottom;
	int centred;

	/* A sheet in the middle of the screen (the docked mode, ws142-p008) is a whole body with its shadow, cut nowhere. */
	body_rect(server, surface, &body);
	centred = sheet_centred(server, parent);
	if (centred) {
		draw_body(server, command, surface, &body, focused);
		return;
	}

	/* Where the parent's title bar ends. */
	sheet_anchor(server, parent, body.width, body.height, &x, &top);

	/* Only below the title bar (its shadow and, while it slides, itself): the frame's scissor cut there, in the pass's pixels (a head's, ws113-p007). */
	saved = server->compose->scissor_now;
	cut = saved;
	bottom = saved.offset.y + (int32_t)saved.extent.height;
	cut_top = top;
	if (server->view_width != 0U)
		cut_top = top - server->view_y;
	if (cut.offset.y < cut_top)
		cut.offset.y = cut_top;
	if (bottom < cut.offset.y)
		bottom = cut.offset.y;
	cut.extent.height = (uint32_t)(bottom - cut.offset.y);
	server->compose->scissor_now = cut;
	vkCmdSetScissor(command, 0U, 1U, &cut);

	/* The body, and a hairline along the seam with the title bar. */
	draw_body(server, command, surface, &body, focused);
	glass_draw_solid(server, command, (float)body.x, (float)top, (float)body.width, 1.0f, 0.0f, seam);

	/* The frame's scissor back. */
	server->compose->scissor_now = saved;
	vkCmdSetScissor(command, 0U, 1U, &saved);
}

/*
 * Docks a window to the system bar: its body takes the space under the bar
 * and it is told that size; it comes back to (restore_x, restore_y) at its
 * present size.  The change is animated.
 */
static void
window_dock(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t restore_x,
	int32_t restore_y,
	const char *via)
{
	struct shell_rect from;
	struct shell_rect to;
	struct shell_bar bar;
	uint32_t geometry_width;
	uint32_t geometry_height;

	/* A docked window stays docked. */
	if (surface->maximized)
		return;

	/* A fullscreen window is not docked; it is docked on leaving only when it was docked before (BUG-208). */
	if (surface->fullscreen)
		return;

	/* The place and size to come back to (its own, not a made-up one), and where the body is now. */
	surface->restore_default = 0U;
	surface->restore_x = restore_x;
	surface->restore_y = restore_y;
	kwl_decoration_geometry(surface, &geometry_width, &geometry_height);
	surface->restore_width = geometry_width;
	surface->restore_height = geometry_height;
	body_rect(server, surface, &from);

	/* Docked: told the docked space, drawn there (a window of one size in its middle). */
	surface->maximized = 1;
	docked_rect(server, window_slot(surface), &to);
	surface->x = to.x;
	surface->y = to.y;
	surface->window_width = (uint32_t)to.width;
	surface->window_height = (uint32_t)to.height;
	docked_body(server, surface, &to);

	/* The animation from the floating body to the docked space. */
	memcpy(server->anim_from, &from, sizeof(server->anim_from));
	memcpy(server->anim_to, &to, sizeof(server->anim_to));
	server->anim = surface;
	server->anim_docking = 1;
	server->anim_start_ms = kwl_milliseconds();
	server->dirty = 1;

	/* The client draws the new size; the log gives where the bar's buttons are (close, restore, minimize) and the docked body. */
	bar_layout(server, &bar);
	printf("KWL GLASS dock surface=%u via=%s buttons=%d,%d,%d title=%d x=%d y=%d w=%d h=%d client=%llu\n", surface->id, via,
	       bar.buttons[BUTTON_CLOSE], bar.buttons[BUTTON_MAXIMIZE], bar.buttons[BUTTON_MINIMIZE], bar.title_x,
	       (int)to.x, (int)to.y, (int)to.width, (int)to.height, (unsigned long long)surface->client->number);
	window_configure(surface);

	/* Until the client draws the docked size, the log waits for its image (BUG-179). */
	window_resized(surface);

	/*
	 * A shown window docked owns its desktop at once (WS181), so that it
	 * closing before the next frame ends the docked mode too; one opened
	 * docked is seen as the owner once it is mapped (layout_front_follow).
	 */
	if (surface->mapped && !surface->dead)
		server->dock_owner[surface->desktop][window_slot(surface)] = surface;

	/* A window docked makes the session's mode docked (ws142-p008, BUG-217); the log says what every window is now (WS181). */
	layout_set(server, window_slot(surface), KWL_LAYOUT_DOCKED, via);
	layout_log_windows(server, window_slot(surface));
}

/*
 * Brings a docked window back: its body at (x, y) at the size it had, and
 * it is told that size.  The change is animated.  The session's mode is
 * not changed here: layout_leave ends the docked mode around it (WS181).
 */
static void
window_undock(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t x,
	int32_t y,
	const char *via)
{
	struct shell_rect from;
	struct shell_rect to;

	/* Only a docked window comes back. */
	if (!surface->maximized)
		return;

	/* The place asked for, inside the space of its output: its title bar never under the bar (ws035-p138). */
	glass_fit_on(server, window_slot(surface), (int32_t)surface->restore_width, (int32_t)surface->restore_height, &x, &y);

	/*
	 * From where the body is drawn now -- the docked space, part of the way
	 * of a dock still animated, or the end of a pull, where it already has
	 * its own size under the pointer (BUG-180) -- to that place, at the size
	 * it had.
	 */
	body_rect(server, surface, &from);
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
	server->anim_start_ms = kwl_milliseconds();
	server->dirty = 1;

	/* The client draws the size it had. */
	printf("KWL GLASS undock surface=%u via=%s x=%d y=%d client=%llu\n", surface->id, via, x, y, (unsigned long long)surface->client->number);
	window_configure(surface);

	/* Until the client draws that size, its docked image is drawn at it, never at the docked size (BUG-180); the caller sets the mode (layout_leave). */
	window_resized(surface);
}

/* Tells a window its new size. */
static void
window_configure(
	struct kwl_object *surface)
{
	int error;

	/* A failure is reported; the window keeps drawing its old size. */
	error = kwl_window_send_configure(surface);
	if (error != 0)
		printf("KWL GLASS configure errno=%d\n", error);
}

/*
 * Marks a window docked or brought back as waiting for its client's image
 * of the size the configure just sent asked for.
 */
static void
window_resized(
	struct kwl_object *surface)
{
	/* An image drawn after this configure was acknowledged ends the wait (kwl_glass_committed). */
	surface->resized_ms = kwl_milliseconds();
	surface->resized_serial = surface->configure_serial;
	surface->resized_acked_ms = 0U;
	surface->resized_commit_ms = 0U;

	/* The size of the image it has now, which an image drawn before the client read the configure still has. */
	surface->resized_from_width = 0U;
	surface->resized_from_height = 0U;
	if (surface->current != NULL)
		kwl_surface_size(surface, &surface->resized_from_width, &surface->resized_from_height);

	/* Succeeded: the window waits for its image of the new size. */
	return;
}

/* Describes a window to the layout's rules (layout.c): docked, fullscreen, with a parent, of one size. */
static void
layout_window(
	const struct kwl_object *surface,
	struct kwl_layout_window *window)
{
	/* Docked and fullscreen as the shell keeps them. */
	window->docked = surface->maximized;
	window->fullscreen = surface->fullscreen;

	/* A dialog or a sheet has a parent. */
	window->child = 0U;
	if (surface->parent_window != NULL)
		window->child = 1U;

	/* A window of one size has the same smallest and largest sizes. */
	window->fixed = 0U;
	if (surface->min_width > 0 &&
	    surface->min_width == surface->max_width &&
	    surface->min_height == surface->max_height)
		window->fixed = 1U;
}

/*
 * Sets an output's layout mode (ws142-p008; each output its own,
 * ws113-p015): every window is drawn again, the other applications' shown
 * or hidden, and the log says why it changed (a head's line names it).
 */
static void
layout_set(
	struct kwl_server *server,
	unsigned slot,
	unsigned mode,
	const char *via)
{
	/* The same mode: nothing changes. */
	if (server->layout_mode[slot] == mode)
		return;

	/* The new mode, drawn from the next frame (another application's windows show or go). */
	server->layout_mode[slot] = mode;
	server->dirty = 1;
	if (slot == KWL_PLANE_ANCHOR) {
		printf("KWL LAYOUT mode=%s reason=%s at_ms=%llu\n", kwl_layout_name(mode), via, (unsigned long long)kwl_milliseconds());
	} else {
		printf("KWL LAYOUT mode=%s reason=%s output=%u at_ms=%llu\n", kwl_layout_name(mode), via, slot, (unsigned long long)kwl_milliseconds());
	}

	/* No desktop of the output stays arranged in the docked mode (WS181 I3). */
	if (mode == KWL_LAYOUT_DOCKED)
		kwl_arrange_end_all(server, slot, "dock");
}

/*
 * Makes a window switched to follow the session's layout mode: docked in
 * the docked mode, floating again at its place before in the windowed
 * mode.  A dialog or a sheet does it through its parent.
 */
static void
layout_match(
	struct kwl_server *server,
	struct kwl_object *surface,
	const char *via)
{
	static const char *const actions[] = { "keep", "dock", "float" };
	struct kwl_layout_window window;
	struct kwl_object *owner;
	unsigned action;

	/* A dialog or a sheet follows the mode through its parent, while the parent is shown. */
	owner = surface;
	if (surface->parent_window != NULL &&
	    !surface->parent_window->dead &&
	    surface->parent_window->mapped)
		owner = surface->parent_window;

	/* What the mode makes of it. */
	layout_window(owner, &window);
	action = kwl_layout_switch_action(server->layout_mode[window_slot(owner)], &window);

	/* Docked where it floats, or floating again where it was before it docked. */
	if (action == KWL_LAYOUT_DOCK) {
		window_dock(server, owner, owner->x, owner->y, via);
	} else if (action == KWL_LAYOUT_FLOAT) {
		window_undock(server, owner, owner->restore_x, owner->restore_y, via);
	}

	/* The log says what the switch did (the tests read it). */
	printf("KWL LAYOUT switch surface=%u action=%s mode=%s via=%s client=%llu\n", owner->id, actions[action], kwl_layout_name(server->layout_mode[window_slot(owner)]), via, (unsigned long long)owner->client->number);
}

/*
 * Tells whether a window is left out of the scene (not drawn, no press): in
 * the docked mode a window of another application than the top window's on
 * the desktop shown, while no overview (App Home, Wiseview, the switcher)
 * shows them all.  Returns 1 when it is hidden.
 */
static int
layout_hides(
	struct kwl_server *server,
	const struct kwl_object *surface)
{
	struct kwl_object *top;
	unsigned slot;
	int desktop_surface;
	int same_application;
	int overview;
	int hidden;

	/* The windowed mode of the window's output hides nothing (and needs no search for the top window). */
	slot = window_slot(surface);
	if (server->layout_mode[slot] != KWL_LAYOUT_DOCKED)
		return 0;

	/* Only the desktop shown has a current application; a neighbour sliding in shows whole. */
	if (surface->desktop != server->desktop)
		return 0;

	/* The desktop's icons are no application's window. */
	desktop_surface = kwl_desktop_is(surface);
	if (desktop_surface)
		return 0;

	/* The current application is the top window's client on that output (none: every window is its). */
	top = kwl_output_top_window(server, slot);
	same_application = 0;
	if (top == NULL || top->client == surface->client)
		same_application = 1;

	/*
	 * Wiseview and the switcher show every application.  App Home does not
	 * (WS181): it is a mode of its own, and the desktop going up off the
	 * output as it opens is the desktop as it was, its hidden windows
	 * hidden.
	 */
	overview = 0;
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving) {
		overview = 1;
	} else if (server->switcher.on) {
		overview = 1;
	}

	/* The rule (layout.c). */
	hidden = kwl_layout_hidden(server->layout_mode[slot], same_application, overview);

	/* Succeeded: whether the window is hidden. */
	return hidden;
}

/*
 * Tells whether a press where no window is shown is taken without effect:
 * in the docked mode, on the docked space of a docked window in front (the
 * dark rest of the space beside a window of one size, where the other
 * applications' windows are hidden).  Returns 1 when it is taken.
 */
static int
layout_takes_press(
	struct kwl_server *server)
{
	struct kwl_object *top;
	struct shell_rect space;
	unsigned slot;

	/* Only the docked mode of the output under the pointer hides windows under the docked one. */
	slot = server->pointer_output;
	if (slot >= KWL_PLANE_SLOTS)
		slot = KWL_PLANE_ANCHOR;
	if (server->layout_mode[slot] != KWL_LAYOUT_DOCKED)
		return 0;

	/* The window in front there (a sheet's parent for a sheet), docked. */
	top = sheet_owner(kwl_output_top_window(server, slot));
	if (top == NULL || !top->maximized)
		return 0;

	/* A press outside the docked space is the desktop's. */
	docked_rect(server, slot, &space);
	if (server->pointer_x < space.x ||
	    server->pointer_x >= space.x + space.width ||
	    server->pointer_y < space.y ||
	    server->pointer_y >= space.y + space.height)
		return 0;

	/* Succeeded: the press is taken. */
	return 1;
}

/*
 * Ends an output's docked mode (WS181, the 2026-10-07 UAT; each output its
 * own, ws113-p015): the window the person brings back (front; NULL when
 * the docked window closed, was minimized or was sent away) floats again
 * at (x, y) with its animation, every other docked window of the output on
 * every desktop -- minimized ones and ones not mapped yet too -- floats
 * again at once, the desktops' owners there are forgotten, and the
 * output's mode becomes windowed.  So no window is left docked behind a
 * floating one, and a window hidden by docking shows again while one the
 * person minimized stays minimized.
 */
static void
layout_leave(
	struct kwl_server *server,
	unsigned slot,
	struct kwl_object *front,
	int32_t x,
	int32_t y,
	const char *via)
{
	struct kwl_layout_window window;
	struct kwl_client *client;
	struct kwl_object *surface;
	unsigned front_id;
	unsigned action;
	unsigned quiet;
	unsigned desktop;
	unsigned pass;

	/* The window brought back is seen coming back. */
	front_id = 0U;
	if (front != NULL) {
		front_id = front->id;
		window_undock(server, front, x, y, via);
	}

	/*
	 * Every other docked window floats again at once (it was not shown):
	 * first those with a place of their own, then those whose place was
	 * made up, which are placed as new windows among the others already
	 * floating (pass 0, then pass 1).
	 */
	quiet = 0U;
	for (pass = 0U; pass < 2U; pass++) {
		for (client = server->clients; client != NULL; client = client->next) {
			if (client->fatal)
				continue;
			for (surface = client->objects; surface != NULL; surface = surface->next) {
				/* Only live windows, of this pass. */
				if (surface->kind != KWL_SURFACE ||
				    surface->dead ||
				    surface->role == NULL ||
				    surface->cursor_role)
					continue;
				if (surface->restore_default != pass)
					continue;

				/* Only the output's. */
				if (surface->output != slot)
					continue;

				/* What the end of the mode does to it. */
				layout_window(surface, &window);
				action = kwl_layout_leave_action(&window, 0);
				if (action != KWL_LAYOUT_QUIET)
					continue;

				/* Floating at its place before, told so. */
				window_float_quiet(server, surface);
				quiet++;
			}
		}
	}

	/* No desktop keeps a docked owner on the output in the windowed mode. */
	for (desktop = 0U; desktop < KWL_APPS_DESKTOPS; desktop++) {
		server->dock_owner[desktop][slot] = NULL;
		server->dock_owner_gone[desktop][slot] = 0U;
	}

	/* The output's mode is windowed; the log says why, and what every window is now (the tests read it; a head's line names it). */
	layout_set(server, slot, KWL_LAYOUT_WINDOWED, via);
	if (slot == KWL_PLANE_ANCHOR) {
		printf("KWL LAYOUT leave via=%s front=%u quiet=%u\n", via, front_id, quiet);
	} else {
		printf("KWL LAYOUT leave via=%s front=%u quiet=%u output=%u\n", via, front_id, quiet, slot);
	}

	/* What every window of the output is now. */
	layout_log_windows(server, slot);
}

/*
 * Brings a docked window that is not shown back to floating at once, as
 * the docked mode ends (WS181): at its place and size before it docked, or
 * placed as a new window is when it never floated (several windows opened
 * docked are not left on one spot), and told its size.  Nothing of the
 * shell's (an animation, a pull, a move, a double click's dock) goes on
 * with it.
 */
static void
window_float_quiet(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	/* Not docked; what the shell was doing with it ends. */
	surface->maximized = 0;
	if (server->anim == surface)
		server->anim = NULL;
	if (server->pull == surface) {
		server->pull = NULL;
		server->pull_distance = 0;
	}
	if (server->click_docked == surface)
		server->click_docked = NULL;
	if (server->drag == surface)
		server->drag = NULL;

	/* Its size before it docked. */
	surface->window_width = surface->restore_width;
	surface->window_height = surface->restore_height;

	/* Its place before, or a new window's place when the place was made up; inside the space either way. */
	if (surface->restore_default) {
		kwl_glass_place(server, surface, (int32_t)surface->restore_width, (int32_t)surface->restore_height, 0);
		surface->restore_default = 0U;
	} else {
		surface->x = surface->restore_x;
		surface->y = surface->restore_y;
		glass_fit_on(server, window_slot(surface), (int32_t)surface->restore_width, (int32_t)surface->restore_height, &surface->x, &surface->y);
	}

	/* The client draws that size; until it does, its docked image is drawn at it (BUG-180). */
	server->dirty = 1;
	printf("KWL LAYOUT float-quiet surface=%u x=%d y=%d w=%u h=%u client=%llu\n", surface->id, surface->x, surface->y,
	       surface->window_width, surface->window_height, (unsigned long long)surface->client->number);
	window_configure(surface);
	window_resized(surface);
}

/*
 * Follows the docked mode of each output every frame (WS181, replacing
 * ws142-p008's rule that the next window docks; ws113-p015 each output
 * its own): first each desktop's docked owner is
 * checked -- one closed, minimized, floating or sent away from the desktop
 * shown ends the docked mode, one gone on a desktop not shown is only
 * forgotten, one carried along goes on owning the desktop shown -- and only
 * then the window in front of the desktop shown is looked at
 * (layout_front_follow).  The order matters: a closed owner ends the mode
 * before the window that came forward could be docked.
 */
static void
layout_follow(
	struct kwl_server *server)
{
	unsigned slot;

	/* Each output in its docked mode (ws113-p015), the anchor first. */
	for (slot = 0U; slot < KWL_PLANE_SLOTS; slot++) {
		if (server->layout_mode[slot] == KWL_LAYOUT_DOCKED)
			layout_follow_output(server, slot);
	}
}

/*
 * Follows one output's docked mode (layout_follow): its owners on each
 * desktop checked, then the window in front of the desktop shown there.
 */
static void
layout_follow_output(
	struct kwl_server *server,
	unsigned slot)
{
	struct kwl_layout_owner seen;
	struct kwl_object *owner;
	const char *reason;
	unsigned desktop;
	unsigned found;

	/* Each desktop's owner, checked. */
	for (desktop = 0U; desktop < KWL_APPS_DESKTOPS; desktop++) {
		/* A desktop with no owner, and none gone since, has nothing to check. */
		owner = server->dock_owner[desktop][slot];
		if (owner == NULL && !server->dock_owner_gone[desktop][slot])
			continue;

		/* The owner as the rule sees it: destroyed, or as it is now (one moved to another output is gone from this one). */
		memset(&seen, 0, sizeof(seen));
		seen.gone = server->dock_owner_gone[desktop][slot];
		if (owner != NULL)
			layout_owner_describe(owner, &seen);
		if (owner != NULL && owner->output != slot)
			seen.gone = 1U;
		server->dock_owner_gone[desktop][slot] = 0U;

		/* What the rule finds (layout.c). */
		found = kwl_layout_owner_check(&seen, desktop, server->desktop, &reason);

		/* The docked mode ends, is forgotten on this desktop, or follows the owner to the desktop shown. */
		switch (found) {
		case KWL_LAYOUT_OWNER_LEAVE:
			layout_leave(server, slot, NULL, 0, 0, reason);
			return;
		case KWL_LAYOUT_OWNER_FORGET:
			/* An owner gone between desktops not shown has no reason of its own. */
			if (reason == NULL)
				reason = "away";
			server->dock_owner[desktop][slot] = NULL;
			printf("KWL LAYOUT owner desktop=%u surface=0 forgotten=%s\n", desktop + 1U, reason);
			break;
		case KWL_LAYOUT_OWNER_MOVED:
			server->dock_owner[desktop][slot] = NULL;
			server->dock_owner[owner->desktop][slot] = owner;
			printf("KWL LAYOUT owner desktop=%u surface=%u carried\n", owner->desktop + 1U, owner->id);
			break;
		default:
			break;
		}
	}

	/* The window in front of the desktop shown. */
	layout_front_follow(server, slot);
}

/* Describes a desktop's docked owner to the owner's rule (layout.c): mapped, minimized, docked, fullscreen, its desktop. */
static void
layout_owner_describe(
	const struct kwl_object *owner,
	struct kwl_layout_owner *seen)
{
	/* A window destroyed is marked dead before it goes (kwl_glass_forget clears the owner then). */
	if (owner->dead)
		seen->gone = 1U;

	/* As the shell keeps it now. */
	seen->mapped = owner->mapped;
	seen->minimized = owner->minimized;
	seen->docked = owner->maximized;
	seen->fullscreen = owner->fullscreen;
	seen->desktop = owner->desktop;
}

/*
 * Looks at the window in front of the desktop shown in the docked mode
 * (WS181): a docked one becomes the desktop's owner (one opened docked is
 * seen here once it is mapped), a fullscreen one leaves the owner as it
 * is, and a floating one that came forward without a switch (Super+Alt+P,
 * the Notes corner, the docked window sent to the back, a desktop turned
 * to) docks as a window switched to does.  Not while an overview shows,
 * nor while a window is moved or pulled.
 */
static void
layout_front_follow(
	struct kwl_server *server,
	unsigned slot)
{
	struct kwl_layout_window window;
	struct kwl_object *top;
	unsigned action;
	float home;
	int showing;

	/* App Home, Wiseview and the switcher show every window as it is. */
	home = kwl_home_progress(server);
	if (home > 0.0f)
		return;
	showing = wiseview_showing(server);
	if (showing || server->wiseview_moving)
		return;
	if (server->switcher.on)
		return;

	/* A window being moved or pulled out of the dock is the person's. */
	if (server->drag != NULL || server->pull != NULL)
		return;

	/* The window in front on the output (a sheet's parent for a sheet), shown with an image. */
	top = sheet_owner(kwl_output_top_window(server, slot));
	if (top == NULL || top->dead || !top->mapped || top->current == NULL)
		return;

	/* A dialog in front stands for its shown parent when that is docked; a floating parent is left as it is. */
	if (top->parent_window != NULL &&
	    !top->parent_window->dead &&
	    top->parent_window->mapped) {
		top = top->parent_window;
		if (!top->maximized)
			return;
	}

	/* A docked window in front owns the desktop. */
	if (top->maximized) {
		if (server->dock_owner[server->desktop][slot] == top)
			return;
		server->dock_owner[server->desktop][slot] = top;
		printf("KWL LAYOUT owner desktop=%u surface=%u client=%llu\n", server->desktop + 1U, top->id, (unsigned long long)top->client->number);
		layout_log_windows(server, slot);
		return;
	}

	/* A fullscreen window in front leaves the owner under it. */
	if (top->fullscreen)
		return;

	/* What a switch to it would do; only a floating window that docks changes. */
	layout_window(top, &window);
	action = kwl_layout_switch_action(server->layout_mode[slot], &window);
	if (action != KWL_LAYOUT_DOCK)
		return;

	/* Docked where it floats, owning the desktop, and the log says why. */
	window_dock(server, top, top->x, top->y, "front");
	server->dock_owner[server->desktop][slot] = top;
	printf("KWL LAYOUT front surface=%u action=dock client=%llu\n", top->id, (unsigned long long)top->client->number);
}

/*
 * Logs what each window of the desktop shown on an output is (WS181, for
 * the tests): how many float, are docked, hidden by docking, minimized or
 * fullscreen (a head's line names it, ws113-p015).
 */
static void
layout_log_windows(
	struct kwl_server *server,
	unsigned slot)
{
	unsigned counts[KWL_LAYOUT_STATE_FULLSCREEN + 1U];
	struct kwl_layout_window window;
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *top;
	unsigned state;
	int desktop_surface;
	int front;

	/* The application in front: the output's top window's (its minimized windows are not in front). */
	memset(counts, 0, sizeof(counts));
	top = kwl_output_top_window(server, slot);

	/* Each mapped window without a parent on the desktop shown, counted by its state. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only live mapped windows of the desktop shown, not a dialog or a sheet. */
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role ||
			    surface->parent_window != NULL ||
			    surface->desktop != server->desktop ||
			    surface->output != slot)
				continue;

			/* Not the desktop's icons. */
			desktop_surface = kwl_desktop_is(surface);
			if (desktop_surface)
				continue;

			/* Its state. */
			front = 0;
			if (top == NULL || top->client == surface->client)
				front = 1;
			layout_window(surface, &window);
			state = kwl_layout_state(server->layout_mode[slot], &window, (int)surface->minimized, front);
			counts[state]++;
		}
	}

	/* The summary (the anchor's line as before; a head's names it). */
	printf("KWL LAYOUT windows desktop=%u mode=%s floating=%u docked=%u dock_hidden=%u minimized=%u fullscreen=%u",
	       server->desktop + 1U, kwl_layout_name(server->layout_mode[slot]),
	       counts[KWL_LAYOUT_STATE_FLOATING], counts[KWL_LAYOUT_STATE_DOCKED], counts[KWL_LAYOUT_STATE_DOCK_HIDDEN],
	       counts[KWL_LAYOUT_STATE_MINIMIZED], counts[KWL_LAYOUT_STATE_FULLSCREEN]);
	if (slot != KWL_PLANE_ANCHOR)
		printf(" output=%u", slot);
	printf("\n");
}

/*
 * The body of a docked window: the docked space, or its middle at the
 * window's size for a window of one size whose image is smaller than the
 * space (ws142-p008: its client does not draw the docked size).
 */
static void
docked_body(
	struct kwl_server *server,
	const struct kwl_object *surface,
	struct shell_rect *body)
{
	struct kwl_layout_window window;
	int32_t width;
	int32_t height;
	int32_t x;
	int32_t y;
	int centred;

	/* The docked space. */
	docked_rect(server, window_slot(surface), body);

	/* Only a docked window of one size is centred. */
	layout_window(surface, &window);
	window.docked = 1U;
	centred = kwl_layout_centred(server->layout_mode[window_slot(surface)], &window);
	if (!centred)
		return;

	/* Its image's size; without an image, or one not smaller than the space, the space. */
	window_size(surface, &width, &height);
	if (width <= 0 || height <= 0)
		return;
	if (width > body->width || height > body->height)
		return;

	/* The middle of the space at that size. */
	kwl_layout_centre(body->x, body->y, body->width, body->height, width, height, &x, &y);
	body->x = x;
	body->y = y;
	body->width = width;
	body->height = height;
}

/*
 * Tells whether a window is drawn in the middle of the docked space over
 * the scene under it blurred (ws142-p008b): a docked window of one size
 * smaller than the space, and in the docked mode a dialog or a sheet over
 * its docked parent.  Returns 1 when it is.
 */
static int
window_centred_over(
	struct kwl_server *server,
	const struct kwl_object *surface)
{
	struct shell_rect body;
	struct shell_rect space;
	struct kwl_object *parent;
	unsigned slot;

	/* A docked window: centred when its body is smaller than the docked space. */
	if (surface->maximized) {
		docked_body(server, surface, &body);
		docked_rect(server, window_slot(surface), &space);
		if (body.width != space.width || body.height != space.height)
			return 1;
		return 0;
	}

	/* Otherwise only the docked mode of its output centres anything. */
	slot = window_slot(surface);
	if (server->layout_mode[slot] != KWL_LAYOUT_DOCKED)
		return 0;

	/* A dialog or a sheet over a docked parent that is shown. */
	parent = surface->parent_window;
	if (parent == NULL || parent->dead || !parent->mapped)
		return 0;
	if (!parent->maximized || parent->minimized)
		return 0;

	/* Succeeded: it is centred over the blurred scene. */
	return 1;
}

/*
 * Covers the output with the scene drawn into the backdrop, blurred, and a
 * little darker, under a window in the middle of the docked space
 * (ws142-p008b).  The system bar is drawn over it later, sharp.
 */
static void
draw_centred_cover(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float shade[4] = { 0.02f, 0.03f, 0.06f, 0.32f };
	struct glass_shape shape;

	/* The blurred scene over the whole output, where the output is (not with a desktop's layer). */
	server->layer_on = 0;
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_GLASS;
	shape.opacity = 1.0f;
	glass_shape_draw(server, command, &shape);

	/* Darker, so that the window in the middle stands out. */
	glass_draw_solid(server, command, 0.0f, 0.0f, (float)server->width, (float)server->height, 0.0f, shade);
}

/*
 * Follows the bar's layout towards a docked window's (1) while one is on
 * top, the floating one's (0) otherwise (ws099-p034b): a change starts
 * from where it is and eases out over BAR_DOCK_MS; drawn every frame
 * meanwhile.
 */
static void
bar_dock_follow(
	struct kwl_server *server)
{
	struct kwl_object *top;
	uint64_t now;
	uint64_t elapsed;
	float target;
	float t;
	float home;

	/* Docked: a docked window on top of the anchor (a head's are in its own bar, ws113-p015), not fullscreen, outside App Home and Wiseview. */
	target = 0.0f;
	top = sheet_owner(kwl_output_top_window(server, KWL_PLANE_ANCHOR));
	home = kwl_home_progress(server);
	if (top != NULL && top->maximized && !top->fullscreen && home <= 0.0f && server->wiseview <= 0.0f && !server->wiseview_gesture)
		target = 1.0f;

	/* A new target: from where the layout is now. */
	now = kwl_milliseconds();
	if (target != server->bar_dock_to) {
		server->bar_dock_from = server->bar_dock;
		server->bar_dock_to = target;
		server->bar_dock_ms = now;
	}

	/* Settled. */
	if (server->bar_dock == server->bar_dock_to)
		return;

	/* On its way, eased out (1 - (1 - t)^3), drawn every frame. */
	elapsed = now - server->bar_dock_ms;
	t = (float)elapsed / (float)BAR_DOCK_MS;
	if (t > 1.0f)
		t = 1.0f;
	t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
	server->bar_dock = server->bar_dock_from + (server->bar_dock_to - server->bar_dock_from) * t;
	if (elapsed >= BAR_DOCK_MS)
		server->bar_dock = server->bar_dock_to;
	server->dirty = 1;
}

/*
 * Gives a window that is docked without a floating place of its own a
 * place to come back to: seven tenths of the docked space, in its middle.
 */
static void
dock_restore_default(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct shell_rect docked;
	int32_t restore_x;
	int32_t restore_y;

	/* Seven tenths of the docked space. */
	docked_rect(server, window_slot(surface), &docked);
	surface->restore_width = (uint32_t)(docked.width * 7 / 10);
	surface->restore_height = (uint32_t)(docked.height * 7 / 10);

	/* In its middle, inside the space for floating windows. */
	restore_x = docked.x + (docked.width - (int32_t)surface->restore_width) / 2;
	restore_y = docked.y + (docked.height - (int32_t)surface->restore_height) / 2;
	glass_fit_on(server, window_slot(surface), (int32_t)surface->restore_width, (int32_t)surface->restore_height, &restore_x, &restore_y);
	surface->restore_x = restore_x;
	surface->restore_y = restore_y;

	/* A place made up: the end of the docked mode places the window as a new one instead (window_float_quiet). */
	surface->restore_default = 1U;
}

/*
 * Tells whether this press on a window's title is the second of a double
 * click (within DOUBLE_CLICK_MS of one on the same window); otherwise it
 * remembers this press as a first one.
 */
static unsigned
double_click(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	uint64_t now;
	int quick;

	/* The second press of a pair. */
	now = kwl_milliseconds();
	quick = click_quick(server, surface, now);
	if (quick) {
		server->click_surface = NULL;
		server->click_count = 0;
		return 1;
	}

	/* A first press. */
	server->click_surface = surface;
	server->click_ms = now;
	server->click_count = 1;
	return 0;
}

/*
 * Counts this press on a window's floating title bar into the run of quick
 * presses on it: 1 for a press that starts a run, 2 for the second of a
 * double click, 3 for the third of a triple click.  A press more than
 * DOUBLE_CLICK_MS after the one before, or on another window, starts a new
 * run.
 */
static unsigned
title_clicks(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	uint64_t now;
	int quick;

	/* A quick press after the one before on the same window goes on the run. */
	now = kwl_milliseconds();
	quick = click_quick(server, surface, now);
	if (quick) {
		server->click_count++;
	} else {
		server->click_surface = surface;
		server->click_count = 1;
	}

	/*
	 * The time of this press is where the next one is measured from, so
	 * the third press has DOUBLE_CLICK_MS after the second.
	 */
	server->click_ms = now;

	/* Succeeded: how many presses the run has. */
	return server->click_count;
}

/*
 * Takes the release of a double click's second press on a floating title
 * bar (BUG-265): near the press, the move it started ends where it began
 * and the window docks (a third press near it soon after takes that back,
 * click_docked_third); moved further, it was a move (a tap and drag),
 * which ends as any move, and the run of clicks ends.  Returns 1 when the
 * release was taken here (the dock).
 */
static int
title_tap_release(
	struct kwl_server *server)
{
	struct kwl_object *surface;
	const void *waited;
	int decided;

	/* What the release was. */
	decided = kwl_title_tap_release(&server->title_tap, server->pointer_x, server->pointer_y, &waited);
	if (decided == TITLE_TAP_NONE)
		return 0;
	surface = (struct kwl_object *)(uintptr_t)waited;

	/* Moved: a move's end as any, with no third click after it. */
	if (decided == TITLE_TAP_MOVED) {
		server->click_surface = NULL;
		server->click_count = 0;
		printf("KWL GLASS double-click moved surface=%u\n", surface->id);
		return 0;
	}

	/* The move the press started ends where it began (a swap of arranged windows ends as it would). */
	if (server->drag == surface) {
		server->drag = NULL;
		surface->x = server->drag_start_x;
		surface->y = server->drag_start_y;
	} else {
		(void)kwl_arrange_move_end(server);
	}

	/* A window that went, was docked or left the desktop meanwhile stays as it is. */
	if (surface->dead || !surface->mapped || surface->maximized || surface->desktop != server->desktop)
		return 1;

	/* Succeeded: docked, and a third press near it soon takes it back. */
	server->click_docked = surface;
	server->click_docked_due_ms = kwl_milliseconds() + DOUBLE_CLICK_MS;
	server->click_docked_x = server->pointer_x;
	server->click_docked_y = server->pointer_y;
	window_dock(server, surface, surface->x, surface->y, "double-click");
	return 1;
}

/*
 * Tells whether a press on a window's title is quick after the one before
 * on the same window: within DOUBLE_CLICK_MS of that press, or of its
 * release.  A touch pad's tap gives its press at the lift and holds it
 * until the next tap lifts (touchpad.c), so a double tap's second press
 * comes a whole tap after the first but right after its release (BUG-247).
 */
static int
click_quick(
	struct kwl_server *server,
	struct kwl_object *surface,
	uint64_t now)
{
	/* Only a press on the window of the run. */
	if (server->click_surface != surface)
		return 0;

	/* Within the time of the press before. */
	if (now - server->click_ms < DOUBLE_CLICK_MS)
		return 1;

	/* Within the time of that press's release. */
	if (server->click_release_ms >= server->click_ms && now - server->click_release_ms < DOUBLE_CLICK_MS)
		return 1;

	/* Succeeded: a slow press, which starts a new run. */
	return 0;
}

/*
 * Takes a third quick press after a double click that docked a window
 * (ws079-p013's triple click, BUG-179): the dock is taken back and the
 * window goes to the back.  Returns 1 when the press was that third one.
 */
static int
click_docked_third(
	struct kwl_server *server)
{
	struct kwl_object *surface;
	uint64_t now;
	int32_t dx;
	int32_t dy;

	/* No double click docked a window lately. */
	surface = server->click_docked;
	if (surface == NULL)
		return 0;

	/* The time a third press had is over. */
	now = kwl_milliseconds();
	if (now >= server->click_docked_due_ms) {
		server->click_docked = NULL;
		return 0;
	}

	/* A press away from the second one is no third click (it acts on what it is on). */
	dx = server->pointer_x - server->click_docked_x;
	dy = server->pointer_y - server->click_docked_y;
	if (dx * dx + dy * dy >= TRIPLE_CLICK_SLOP * TRIPLE_CLICK_SLOP) {
		server->click_docked = NULL;
		return 0;
	}

	/* The run of clicks ends with this press. */
	server->click_docked = NULL;
	server->click_surface = NULL;
	server->click_count = 0;

	/* A window that was brought back, hidden or left the desktop meanwhile leaves the press to what it is on. */
	if (!surface->mapped ||
	    !surface->maximized ||
	    surface->minimized ||
	    surface->desktop != server->desktop)
		return 0;

	/* The window floats where it was and goes to the back, the next one coming forward. */
	layout_leave(server, window_slot(surface), surface, surface->restore_x, surface->restore_y, "triple-click");
	window_lower(server, surface, "triple-click");

	/* Succeeded: the press was the triple click's. */
	return 1;
}

/*
 * Sends a window to the back of the stacking order and gives the focus to
 * the window now on top.  Every other window keeps its place relative to
 * the others.
 */
static void
window_lower(
	struct kwl_server *server,
	struct kwl_object *surface,
	const char *via)
{
	struct kwl_client *client;
	struct kwl_object *other;
	struct kwl_object *top;
	uint64_t lowest;
	unsigned found;
	uint64_t next_client;
	uint32_t next;
	uint64_t focus_client;
	uint32_t focus;
	struct kwl_object *parent;
	struct kwl_object *sheet;

	/*
	 * A sheet goes with its parent, and a parent with its sheet (ws090-p014):
	 * the sheet goes under every window first, then the parent under it.
	 */
	if (!sheet_lowering) {
		parent = kwl_sheet_parent(surface);
		if (parent != NULL)
			surface = parent;
		sheet = kwl_sheet_of(surface);
		if (sheet != NULL) {
			sheet_lowering = 1;
			window_lower(server, sheet, via);
			sheet_lowering = 0;
		}
	}

	/* The lowest place any other mapped surface holds. */
	found = 0;
	lowest = 0;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (other = client->objects; other != NULL; other = other->next) {
			/* Only the other live mapped surfaces. */
			if (other == surface ||
			    other->kind != KWL_SURFACE ||
			    other->dead ||
			    !other->mapped)
				continue;

			/* Below what was found so far. */
			if (found && other->map_order >= lowest)
				continue;
			found = 1;
			lowest = other->map_order;
		}
	}

	/* A window alone stays where it is. */
	if (!found) {
		printf("KWL GLASS lower client=%llu surface=%u via=%s next=none\n", (unsigned long long)surface->client->number, surface->id, via);
		return;
	}

	/*
	 * With no place free under the lowest (map orders start at 1), every
	 * other mapped surface moves up one place, which keeps their order.
	 */
	if (lowest <= 1U) {
		for (client = server->clients; client != NULL; client = client->next) {
			for (other = client->objects; other != NULL; other = other->next) {
				/* Only the other mapped surfaces hold a place. */
				if (other == surface ||
				    other->kind != KWL_SURFACE ||
				    !other->mapped)
					continue;
				other->map_order++;
			}
		}

		/* The next window mapped or raised still comes above them all. */
		server->map_order++;
		lowest++;
	}

	/* Under every other window. */
	surface->map_order = lowest - 1U;

	/* The window now on top takes the focus. */
	top = kwl_top_window(server);
	server->front_surface = top;
	kwl_seat_focus(server);
	server->dirty = 1;

	/*
	 * The log names the window that came forward and the surface that has
	 * the keyboard now, each as client:surface (surface numbers are each
	 * client's own).
	 */
	next_client = 0;
	next = 0;
	if (top != NULL) {
		next_client = top->client->number;
		next = top->id;
	}

	/* The keyboard's surface, when there is one. */
	focus_client = 0;
	focus = 0;
	if (server->focus != NULL) {
		focus_client = server->focus->client->number;
		focus = server->focus->id;
	}

	/* Written as one line for the tests. */
	printf("KWL GLASS lower client=%llu surface=%u via=%s next=%llu:%u focus=%llu:%u\n",
	       (unsigned long long)surface->client->number,
	       surface->id,
	       via,
	       (unsigned long long)next_client,
	       next,
	       (unsigned long long)focus_client,
	       focus);
}

/*
 * Handles a press in the system bar: on the docked window's buttons their
 * action, on its title a double click (back) or the start of a pull.  The
 * clock opens Calendar (ws155-p004), not over App Home (ws181-p009).  The
 * launcher and the status do nothing yet.  The press is always the compositor's.
 */
static int
bar_press(
	struct kwl_server *server,
	uint32_t button)
{
	struct kwl_object *surface;
	struct shell_bar bar;
	unsigned second;
	int pressed;
	int running;
	int error;
	float home;

	/* The desktops' pill is the arrangement menu's (arrange-shell.c, WS181: its pictures switch desktops there). */
	bar_layout(server, &bar);

	/*
	 * Over App Home the bar has only the status and the clock, and nothing
	 * acts, the clock neither (the 2026-10-07 UATs, ws181-p009): a press
	 * the status's icons did not take (between them, on the clock) is
	 * Home's, as a press beside them is (ws177-p038).
	 */
	home = kwl_home_progress(server);
	if (home > 0.0f || server->home_to > 0.0f) {
		printf("KWL HOME bar gap x=%d\n", server->pointer_x);
		(void)kwl_home_button(server, button, 1U);
		return 1;
	}

	/*
	 * The clock opens Calendar (ws155-p004, the 2026-10-04 user request);
	 * on a desktop in the arrangement mode its window joins the
	 * arrangement (ws181-p009, the 2026-10-07 UAT).
	 */
	if (server->pointer_x >= bar.clock_pill_x && server->pointer_x < bar.clock_pill_x + bar.clock_pill_width) {
		kwl_arrange_join_prepare(server);
		error = kwl_home_open_app(server, "Calendar", "clock", &running);
		kwl_arrange_join_opened(server, error, running);
		return 1;
	}

	/* Otherwise only a docked window acts. */
	surface = docked_window(server, KWL_PLANE_ANCHOR);
	if (surface == NULL)
		return 1;

	/* Its buttons. */
	pressed = bar_button_at(&bar, server->pointer_x, server->pointer_y);
	if (pressed == BUTTON_CLOSE) {
		(void)kwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
		printf("KWL GLASS close surface=%u client=%llu\n", surface->id, (unsigned long long)surface->client->number);
		return 1;
	}

	/* Restore brings it back where it was. */
	if (pressed == BUTTON_MAXIMIZE) {
		layout_leave(server, window_slot(surface), surface, surface->restore_x, surface->restore_y, "button");
		return 1;
	}

	/* Minimize hides it. */
	if (pressed == BUTTON_MINIMIZE) {
		window_minimize(server, surface);
		return 1;
	}

	/* Its title: between the line after the launcher and the buttons. */
	if (server->pointer_x < bar.menu_line || server->pointer_x >= bar.buttons[BUTTON_MINIMIZE] - BUTTON_WIDTH / 2)
		return 1;

	/* A double click brings it back where it was. */
	second = double_click(server, surface);
	if (second) {
		layout_leave(server, window_slot(surface), surface, surface->restore_x, surface->restore_y, "double-click");
		return 1;
	}

	/* A single press may become a pull. */
	server->pull = surface;
	server->pull_start_x = server->pointer_x;
	server->pull_start_y = server->pointer_y;
	server->pull_distance = 0;
	return 1;
}

/*
 * Finds the fullscreen window that keeps the system bar away (ws035-p119,
 * the 2026-09-28 user decision): of the windows on the desktop shown that
 * cover the output's top, fullscreen or docked, the highest, when it is
 * fullscreen.  A window opened or raised over it floats above it and the
 * bar stays away, so that the fullscreen window (Notes) is kept whole; the
 * bar is reached through App Home and Wiseview, which show it.  A docked
 * window above it has the bar, which holds its title.  Returns NULL when
 * the bar is drawn.
 */
static struct kwl_object *
bar_cover(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *cover;
	float progress;

	/* App Home shows the bar, opening, open or closing. */
	progress = kwl_home_progress(server);
	if (progress > 0.0f || server->home_to > 0.0f)
		return NULL;

	/* So does Wiseview. */
	progress = wiseview_progress(server);
	if (progress > 0.0f || server->wiseview_moving)
		return NULL;

	/* The highest fullscreen or docked window of the desktop shown. */
	cover = NULL;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only mapped windows of the desktop shown. */
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role ||
			    surface->desktop != server->desktop ||
			    surface->minimized)
				continue;

			/* A floating window leaves the top of the output to what is under it. */
			if (!surface->fullscreen && !surface->maximized)
				continue;

			/* Above what was found so far. */
			if (cover != NULL && surface->map_order < cover->map_order)
				continue;
			cover = surface;
		}
	}

	/* No such window, or a docked one on top: the bar is drawn. */
	if (cover == NULL || !cover->fullscreen)
		return NULL;

	/* Succeeded: the fullscreen window the bar keeps away from. */
	return cover;
}

/* Logs the system bar leaving or coming back for a fullscreen window, once per change (for the tests). */
static void
bar_cover_log(
	struct kwl_server *server,
	const struct kwl_object *cover)
{
	/* The bar went away. */
	if (cover != NULL && !server->bar_hidden) {
		server->bar_hidden = 1U;
		printf("KWL GLASS bar hidden fullscreen=%u\n", cover->id);
		return;
	}

	/* The bar came back. */
	if (cover == NULL && server->bar_hidden) {
		server->bar_hidden = 0U;
		printf("KWL GLASS bar shown\n");
	}
}

/*
 * Tells whether a button is App Home's while the system bar is kept away
 * (ws035-p119): the rest of a press Home started, or a left press in the
 * top-left corner, as over a fullscreen window.
 */
static int
home_without_bar(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	/* A press Home follows goes on being Home's. */
	if (server->home_press || server->home_page_press || server->home_rise_press)
		return 1;

	/* Only a left press starts one. */
	if (state == 0 || button != KWL_BUTTON_LEFT)
		return 0;

	/* In the corner, where Home's gesture starts. */
	if (server->pointer_x < HOME_EDGE_CORNER && server->pointer_y < HOME_EDGE_CORNER)
		return 1;

	/* Anything else is the windows'. */
	return 0;
}

/*
 * How far Wiseview is open, from 0 to 1: following the gesture, on its way
 * to where it settles (eased), or settled.
 */
static float
wiseview_progress(
	struct kwl_server *server)
{
	uint64_t elapsed;
	float t;
	float value;

	/* The touch pad's gesture: the fingers' travel. */
	if (server->wiseview_gesture && server->wiseview_pad)
		return server->wiseview_pad_progress;

	/* The gesture: the distance moved up from where it started, or down for the top edge's (WS181). */
	if (server->wiseview_gesture) {
		value = (float)(server->wiseview_start_y - server->pointer_y) / WISEVIEW_DISTANCE;
		if (server->wiseview_top)
			value = -value;
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
	elapsed = kwl_milliseconds() - server->wiseview_start_ms;
	t = 1.0f;
	if (elapsed < WISEVIEW_MS)
		t = (float)elapsed / (float)WISEVIEW_MS;
	t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
	return server->wiseview_from + (server->wiseview_to - server->wiseview_from) * t;
}

/*
 * Handles a button for the top edge's band (WS181, edge.c): a touch's left
 * press in the band is held until its motion tells (band_motion); the
 * release of a held press that never moved gives the press and the release
 * again where it was pressed (replays: the bar is drawn), or is lost (over
 * a fullscreen window, whose client never had the press).  Returns 1 when
 * the button is the band's.
 */
static int
band_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state,
	int replays)
{
	unsigned edge;
	int32_t depth;

	/* A press being given again goes past the band. */
	if (server->band_replay)
		return 0;

	/* The release of a held press: a tap of what is under it, or of the fullscreen window's client. */
	if (state == 0 && server->band_press) {
		server->band_press = 0;
		if (replays) {
			band_replay(server, 1);
		} else {
			band_handback(server, 1);
		}

		/* The release was the band's. */
		return 1;
	}

	/* Only a left press. */
	if (state == 0 || button != KWL_BUTTON_LEFT)
		return 0;

	/* Only a touch's press (the 2026-10-07 user decision: a touch only). */
	if (server->shell_source != KWL_CONTACT_TOUCH)
		return 0;

	/* Only in the band, as deep as the bar where it holds nothing a finger drags (edge.c, BUG-270). */
	depth = band_depth(server);
	edge = kwl_edge_classify_band(server->pointer_x, server->pointer_y, (int32_t)server->width, (int32_t)server->height, depth);
	if (edge != KWL_EDGE_TOP_BAND)
		return 0;

	/*
	 * Held, where and when it was, and over what: the bar, or a fullscreen
	 * window (replays 0), whose client is given a press that is no swipe.
	 */
	server->band_press = 1;
	server->band_start_x = server->pointer_x;
	server->band_start_y = server->pointer_y;
	server->band_since_ms = kwl_milliseconds();
	server->band_time = server->input_time;
	server->band_fullscreen = 0U;
	if (!replays)
		server->band_fullscreen = 1U;
	printf("KWL EDGE band press x=%d y=%d\n", server->pointer_x, server->pointer_y);

	/* Succeeded: the press is the band's for now. */
	return 1;
}

/*
 * Gives the top edge's band's depth at the pointer for a finger (BUG-270):
 * the whole bar's height, except over a docked window's menus, title and
 * buttons, where a finger pulls the window or opens a menu and the band
 * keeps the edge's own KWL_EDGE_BAND.
 */
static int32_t
band_depth(
	struct kwl_server *server)
{
	struct kwl_object *docked;
	struct shell_bar bar;

	/* Without a docked window the bar holds nothing a finger drags down. */
	docked = docked_window(server, KWL_PLANE_ANCHOR);
	if (docked == NULL)
		return KWL_EDGE_BAND_DEEP;

	/* Over the docked window's part of the bar: from the line after the launcher to past its buttons. */
	bar_layout(server, &bar);
	if (server->pointer_x >= bar.menu_line && server->pointer_x < bar.buttons[BUTTON_CLOSE] + BUTTON_WIDTH)
		return KWL_EDGE_BAND;

	/* Succeeded: elsewhere in the bar, its whole height. */
	return KWL_EDGE_BAND_DEEP;
}

/*
 * Follows a press held in the top edge's band (WS181): down far enough it
 * becomes the swipe that opens Wiseview from the top (App Home, open,
 * closes at once), across or up it is given again to what is under it
 * (replays) or lost over a fullscreen window.  Returns 1 when the motion is
 * the band's, 0 when it goes on to what the press was given to.
 */
static int
band_motion(
	struct kwl_server *server,
	int replays)
{
	unsigned kind;

	/* Only a held press. */
	if (!server->band_press)
		return 0;

	/* What the press is after this motion (edge.c). */
	kind = kwl_edge_band_motion(server->pointer_x - server->band_start_x, server->pointer_y - server->band_start_y);
	if (kind == KWL_EDGE_BAND_WAIT)
		return 1;
	server->band_press = 0;

	/* The swipe down: Wiseview opens from the top, following the pointer. */
	if (kind == KWL_EDGE_BAND_WISEVIEW) {
		kwl_home_close_now(server, "top-edge");
		server->wiseview_gesture = 1;
		server->wiseview_top = 1;
		server->wiseview_start_y = server->band_start_y;
		server->wiseview_current = sheet_owner(kwl_top_window(server));
		server->dirty = 1;
		printf("KWL WISEVIEW gesture via=top-edge\n");
		return 1;
	}

	/* Over a fullscreen window the press goes to its client, and the finger with it (this motion too). */
	if (!replays) {
		band_handback(server, 0);
		return 1;
	}

	/* Not the swipe: the press is given to what is under it, and this motion goes on to it. */
	band_replay(server, 0);
	return 0;
}

/*
 * Follows a press held in the top edge's band without a motion
 * (ws177-p033): resting KWL_EDGE_BAND_HOLD_MS where it touched, it is a
 * long press of what is under it, given again there as a press that is
 * still down (the bar's applications show their previews); over a
 * fullscreen window its client has the finger from then.
 */
static void
band_tick(
	struct kwl_server *server)
{
	uint64_t held_ms;
	int held;

	/* Only a held press. */
	if (!server->band_press)
		return;

	/* Not yet long enough. */
	held_ms = kwl_milliseconds() - server->band_since_ms;
	held = kwl_edge_band_held(held_ms);
	if (!held)
		return;
	server->band_press = 0;
	printf("KWL EDGE band hold ms=%llu fullscreen=%u\n", (unsigned long long)held_ms, server->band_fullscreen);

	/* Over a fullscreen window the finger is its client's. */
	if (server->band_fullscreen) {
		band_handback(server, 0);
		return;
	}

	/* Over the bar the press is given again as a long one (band_held while it is). */
	server->band_held = 1U;
	band_replay(server, 0);
	server->band_held = 0U;
}

/*
 * Gives a press held in the top edge's band over a fullscreen window to
 * that window's client (ws177-p034), at the point it touched and with the
 * time it touched: the finger is the client's from then (touch.c), and
 * lifted already (a tap) the client hears it lift too.
 */
static void
band_handback(
	struct kwl_server *server,
	int lifted)
{
	uint32_t time;
	int given;

	/* The events' time now, counted on from the press's (a long press is given without an event). */
	time = server->band_time + (uint32_t)(kwl_milliseconds() - server->band_since_ms);

	/* The finger, given to the window under where it touched. */
	given = kwl_touch_shell_handback(server, server->band_start_x, server->band_start_y, time, lifted);
	printf("KWL EDGE band handback lifted=%d given=%d\n", lifted, given);
}

/*
 * Gives a press held in the top edge's band again, at the point it was
 * pressed, to what is under it (the bar's widgets, App Home), and its
 * release too for a tap; the pointer is back where it is after.
 */
static void
band_replay(
	struct kwl_server *server,
	int release)
{
	int32_t x;
	int32_t y;

	/* The pointer at the press's point while the press is given. */
	x = server->pointer_x;
	y = server->pointer_y;
	server->pointer_x = server->band_start_x;
	server->pointer_y = server->band_start_y;
	server->band_replay = 1;
	printf("KWL EDGE band replay release=%d\n", release);

	/* The press, and for a tap its release, as the shell takes them. */
	(void)kwl_glass_button(server, KWL_BUTTON_LEFT, 1U);
	if (release)
		(void)kwl_glass_button(server, KWL_BUTTON_LEFT, 0U);

	/* The pointer back where it is. */
	server->band_replay = 0;
	server->pointer_x = x;
	server->pointer_y = y;
}

/* Starts the swipe up from the bottom edge that opens App Home (WS181), for a left press in that edge.  Returns 1 when it started. */
static int
home_edge_press(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int taken;

	/* Only a left press. */
	if (state == 0 || button != KWL_BUTTON_LEFT)
		return 0;

	/* Home takes it when it is in the bottom edge (home.c). */
	taken = kwl_home_edge_press(server);
	if (!taken)
		return 0;

	/* Succeeded: the press is the swipe's. */
	return 1;
}

/* Starts Wiseview settling from one value to another (0 closed, 1 open). */
static void
wiseview_settle(
	struct kwl_server *server,
	float from,
	float to)
{
	/* The animation, drawn every frame by kwl_glass_tick. */
	server->wiseview_from = from;
	server->wiseview_to = to;
	server->wiseview_start_ms = kwl_milliseconds();
	server->wiseview_moving = 1;
	server->wiseview = from;
	server->dirty = 1;
}

/* Tells whether Wiseview is open or on its way open (while it closes, keys go to the windows again). */
static int
wiseview_showing(
	struct kwl_server *server)
{
	/* The gesture from the bottom edge holds it open. */
	if (server->wiseview_gesture)
		return 1;

	/* On its way, it shows only when it is going open. */
	if (server->wiseview_moving) {
		if (server->wiseview_to > 0.0f)
			return 1;
		return 0;
	}

	/* Settled open. */
	if (server->wiseview > 0.0f)
		return 1;

	/* Settled closed. */
	return 0;
}

/* Opens Wiseview from the keyboard (Super+Tab), with the window on top as the current tile. */
static void
wiseview_open_key(
	struct kwl_server *server)
{
	/* The window on top is the one Enter comes back to (a sheet's parent for a sheet); a swipe in it begins afresh. */
	server->wiseview_current = sheet_owner(kwl_top_window(server));
	kwl_swipe_end(&server->pad_swipe);

	/* Wiseview opens as it does at the end of the gesture. */
	printf("KWL WISEVIEW opening key at_ms=%llu\n", (unsigned long long)kwl_milliseconds());
	kwl_transition_request(server, "wiseview-open");
	wiseview_settle(server, 0.0f, 1.0f);
}

/*
 * Carries out a key while Wiseview shows: Tab (Shift+Tab back), the arrows
 * move the current tile, Enter and Space choose it, Esc and Super+Tab close
 * Wiseview.  A release and any other key do nothing.
 */
static void
wiseview_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	int step;

	/* Only a press acts. */
	if (state == 0U)
		return;

	/* Esc closes Wiseview where it is. */
	if (key == KEY_ESC) {
		wiseview_close_key(server);
		return;
	}

	/* So does Super+Tab again. */
	if (key == KEY_TAB && (server->modifiers & MODIFIER_SUPER) != 0U) {
		wiseview_close_key(server);
		return;
	}

	/* Enter and Space choose the current tile. */
	if (key == KEY_ENTER || key == KEY_SPACE) {
		wiseview_choose(server, "key");
		return;
	}

	/* The direction: on for Tab, Right and Down (back for Shift+Tab), back for Left and Up. */
	step = 0;
	if (key == KEY_TAB) {
		step = 1;
		if ((server->modifiers & MODIFIER_SHIFT) != 0U)
			step = -1;
	} else if (key == KEY_RIGHT || key == KEY_DOWN) {
		step = 1;
	} else if (key == KEY_LEFT || key == KEY_UP) {
		step = -1;
	}

	/* Any other key does nothing. */
	if (step == 0)
		return;

	/* The current tile moves. */
	wiseview_move(server, step);
}

/*
 * Chooses Wiseview's current tile (Enter, Space, a swipe of two fingers
 * down): its window comes back from minimized, to the top as the layout
 * mode is, and Wiseview closes; without a current window it only closes.
 */
static void
wiseview_choose(
	struct kwl_server *server,
	const char *via)
{
	struct kwl_object *surface;
	float progress;

	/* Without a current window there is nothing to come back to: Wiseview only closes. */
	surface = server->wiseview_current;
	if (surface == NULL || surface->dead || !surface->mapped) {
		wiseview_close_key(server);
		return;
	}

	/* A minimized window comes back; it comes to the top as the layout mode is, and Wiseview closes. */
	kwl_glass_switch_to(server, surface, "wiseview");
	printf("KWL WISEVIEW select surface=%u via=%s client=%llu\n", surface->id, via, (unsigned long long)surface->client->number);
	progress = wiseview_progress(server);
	wiseview_settle(server, progress, 0.0f);
}

/* Moves Wiseview's current tile a step on (1) or back (-1), round the ends; without a current tile, to the first. */
static void
wiseview_move(
	struct kwl_server *server,
	int step)
{
	struct kwl_object *windows[WISEVIEW_WINDOWS];
	unsigned count;
	unsigned index;
	int position;

	/* The tiles, in the order Wiseview lays them out; none, nothing to move. */
	count = wiseview_windows(server, windows, WISEVIEW_WINDOWS);
	if (count == 0U)
		return;

	/* The current tile's place (count when the current window is not among them). */
	for (index = 0; index < count; index++) {
		/* The current window's tile. */
		if (windows[index] == server->wiseview_current)
			break;
	}

	/* The next tile, round the ends; without a current tile, the first. */
	position = 0;
	if (index < count)
		position = (int)index + step;
	if (position < 0)
		position = (int)count - 1;
	if (position >= (int)count)
		position = 0;

	/* It becomes the current tile, which Wiseview draws marked. */
	server->wiseview_current = windows[position];
	server->dirty = 1;
	printf("KWL WISEVIEW current surface=%u\n", windows[position]->id);
}

/* Closes Wiseview from the keyboard, from wherever it is on its way. */
static void
wiseview_close_key(
	struct kwl_server *server)
{
	float progress;

	/* The gesture, if one was under way, ends with it. */
	progress = wiseview_progress(server);
	server->wiseview_gesture = 0;

	/* Wiseview settles closed. */
	printf("KWL WISEVIEW close key at_ms=%llu\n", (unsigned long long)kwl_milliseconds());
	kwl_transition_request(server, "wiseview-close");
	wiseview_settle(server, progress, 0.0f);
}

/*
 * Collects the windows Wiseview shows, the most recently raised first:
 * mapped toplevel windows with an image.
 */
static unsigned
wiseview_windows(
	struct kwl_server *server,
	struct kwl_object **windows,
	unsigned capacity)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *parent;
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
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role ||
			    surface->current == NULL ||
			    surface->desktop != server->desktop)
				continue;

			/* A sheet is not a window of its own there: it comes back with its parent (ws090-p014). */
			parent = kwl_sheet_parent(surface);
			if (parent != NULL)
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
	struct kwl_server *server,
	struct kwl_object **windows,
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
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object **stacked,
	unsigned stacked_count,
	float progress)
{
	static const float shade[4] = { 0.0f, 0.02f, 0.06f, 0.08f };
	static const float dark[4] = { 0.10f, 0.14f, 0.22f, 1.0f };
	struct kwl_object *windows[WISEVIEW_WINDOWS];
	struct shell_rect tiles[WISEVIEW_WINDOWS];
	struct glass_shape shape;
	struct shell_rect body;
	struct shell_rect rect;
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const char *footer;
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

	/*
	 * No title (BUG-216: Wiseview's name is not drawn); one window alone
	 * says there are no others.
	 */
	memcpy(ink, dark, sizeof(ink));
	ink[3] = progress;
	if (count == 1U)
		glass_draw_text(server, command, SIZE_BAR, WISEVIEW_SIDE, KWL_GLASS_BAR + 34, kl_tr("No other windows"), 400, ink);

	/* The footer: a handle and how to choose (two fingers across, then down, ws142-p009). */
	ink[3] = progress * 0.35f;
	glass_draw_solid(server, command, (float)((int32_t)server->width / 2 - 24), (float)((int32_t)server->height - 44), 48.0f, 5.0f, 2.5f, ink);
	ink[3] = progress * 0.7f;
	footer = kl_tr("Swipe left or right to choose · Swipe down to open");
	width = glass_text_width(server, SIZE_BAR, footer);
	glass_draw_text(server, command, SIZE_BAR, ((int32_t)server->width - width) / 2, (int32_t)server->height - 18, footer, 400, ink);

	/* A frame of the way. */
	if (server->log_frames)
		printf("KWL WISEVIEW frame progress=%.2f windows=%u\n", (double)progress, count);
}

/*
 * Draws one window in Wiseview: its shadow, its image with rounded corners
 * (sampled linearly, as it is smaller), a blue glow for the window that was
 * on top or the one under the pointer, its floating title bar fading as it
 * goes, and its label and (under the pointer) close button fading in.
 */
static void
draw_tile(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	const struct shell_rect *tile,
	float progress,
	unsigned current,
	unsigned over)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float button[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	float glow[4];
	const struct kwl_import *image;
	struct glass_shape shape;
	struct shell_rect panel;
	float place[4];
	unsigned panels;
	int32_t width;
	int32_t height;
	int32_t label_width;
	int32_t label_x;
	int32_t label_y;
	int32_t cx;
	int32_t cy;
	float colour[4];
	float appear;
	float radius;
	unsigned square;
	int decorated;

	/* The glow's colour: the accent the user chose, for the overview's darkened ground. */
	kwl_accent_colour(server, 1, KWL_ACCENT_FILL, 0.45f, glow);

	/* The tile's corners: rounded, or square for a window that keeps them (window_square). */
	radius = WISEVIEW_RADIUS;
	square = window_square(surface);
	if (square)
		radius = 0.0f;

	/* The shadow, or a glow of the accent for the window that was on top or is under the pointer. */
	glass_shape_init(&shape, (float)tile->x, (float)tile->y + 6.0f, (float)tile->width, (float)tile->height);
	shape.quad[0] -= 48.0f;
	shape.quad[1] -= 48.0f;
	shape.quad[2] += 96.0f;
	shape.quad[3] += 96.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = radius;
	shape.soft = 24.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.22f;
	if (current || over) {
		memcpy(shape.color, glow, sizeof(shape.color));
		shape.light = 1U;
		shape.opacity = progress;
		if (over)
			shape.color[3] = 0.70f;
	}

	/* Drawn under the tile, unless the window is glass panels, whose own glass shows under the image. */
	image = kwl_compose_surface_image(surface);
	panels = kwl_panels_count(surface);
	if (panels == 0U || current || over)
		glass_shape_draw(server, command, &shape);

	/* The window's size (its viewport's, else its image's), for its glass panels' scale. */
	window_size(surface, &width, &height);
	if (width <= 0 || height <= 0) {
		width = (int32_t)image->width;
		height = (int32_t)image->height;
	}

	/* A window's glass panels, small with the tile. */
	place[0] = (float)tile->x;
	place[1] = (float)tile->y;
	place[2] = (float)tile->width / (float)width;
	place[3] = (float)tile->height / (float)height;
	kwl_panels_draw(server, command, surface, place, 1.0f, 0U);

	/* The image (its viewport's source), sampled linearly. */
	glass_shape_init(&shape, (float)tile->x, (float)tile->y, (float)tile->width, (float)tile->height);
	kwl_viewport_source(surface, shape.uv);
	shape.mode = MODE_IMAGE;
	shape.radius = radius;
	shape.set = image->linear_set;
	if (shape.set == VK_NULL_HANDLE)
		shape.set = image->set;
	if (image->draw == KWL_DRAW_OPAQUE)
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
		shape.radius = radius + 3.0f;
		shape.soft = 2.0f;
		memcpy(shape.color, glow, sizeof(shape.color));
		shape.light = 1U;
		shape.color[3] = 0.9f;
		shape.opacity = progress;
		glass_shape_draw(server, command, &shape);
	}

	/* Only an SSD title bar fades as its window goes to a tile. */
	decorated = kwl_decoration_server(surface);
	if (decorated &&
	    !surface->maximized &&
	    progress < 1.0f) {
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
 * closes Wiseview.  Every button is the compositor's.
 */
static int
wiseview_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct kwl_object *windows[WISEVIEW_WINDOWS];
	struct shell_rect tiles[WISEVIEW_WINDOWS];
	struct kwl_object *surface;
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
		server->wiseview_top = 0;
		if (progress > WISEVIEW_THRESHOLD) {
			printf("KWL WISEVIEW opening from=%.2f\n", (double)progress);
			wiseview_settle(server, progress, 1.0f);
		} else {
			printf("KWL WISEVIEW cancel from=%.2f\n", (double)progress);
			wiseview_settle(server, progress, 0.0f);
		}

		/* The release was the compositor's. */
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

		/* A click: a minimized window comes back; it comes to the top as the layout mode is, and Wiseview closes. */
		if (surface->dead || !surface->mapped)
			return 1;
		kwl_glass_switch_to(server, surface, "wiseview");
		printf("KWL WISEVIEW select surface=%u client=%llu\n", surface->id, (unsigned long long)surface->client->number);
		wiseview_settle(server, 1.0f, 0.0f);
		return 1;
	}

	/* Only a left press on the settled Wiseview acts. */
	if (state == 0 || button != KWL_BUTTON_LEFT || server->wiseview_moving)
		return 1;

	/* A press at the bottom edge closes Wiseview at once and starts the swipe up to App Home (WS181). */
	if (server->pointer_y >= (int32_t)server->height - WISEVIEW_EDGE) {
		server->wiseview = 0.0f;
		server->wiseview_moving = 0;
		server->wiseview_press = NULL;
		server->wiseview_dragging = 0;
		server->dirty = 1;
		printf("KWL WISEVIEW close via=bottom-edge\n");
		(void)kwl_home_edge_press(server);
		return 1;
	}

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

	/* A desktop's picture in the bar switches Wiseview's desktop (the bar stays the compositor's). */
	target = desktop_picture_at(server, server->pointer_x, server->pointer_y);
	if (surface == NULL && target >= 0) {
		desktop_turn(server, target, "wiseview");
		return 1;
	}

	/* Elsewhere, Wiseview closes. */
	if (surface == NULL) {
		printf("KWL WISEVIEW close\n");
		wiseview_settle(server, 1.0f, 0.0f);
		return 1;
	}

	/* Its close button closes the window. */
	if (server->pointer_x >= tiles[index].x + tiles[index].width - 26 && server->pointer_y < tiles[index].y + 26) {
		(void)kwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
		printf("KWL WISEVIEW close-window surface=%u client=%llu\n", surface->id, (unsigned long long)surface->client->number);
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
	struct kwl_server *server)
{
	struct kwl_object *windows[WISEVIEW_WINDOWS];
	struct shell_rect tiles[WISEVIEW_WINDOWS];
	unsigned count;
	unsigned index;

	/* The tiles as they are laid out now. */
	count = wiseview_windows(server, windows, WISEVIEW_WINDOWS);
	wiseview_layout(server, windows, count, tiles);
	printf("KWL WISEVIEW open windows=%u at_ms=%llu\n", count, (unsigned long long)kwl_milliseconds());
	for (index = 0; index < count; index++)
		printf("KWL WISEVIEW tile client=%llu surface=%u x=%d y=%d width=%d height=%d\n", (unsigned long long)windows[index]->client->number, windows[index]->id, tiles[index].x, tiles[index].y, tiles[index].width, tiles[index].height);
}

/* Returns where the desktops are, as a desktop number: the one shown, swiped by the pointer, or sliding. */
static float
desktop_position(
	struct kwl_server *server)
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
	elapsed = kwl_milliseconds() - server->desktop_start_ms;
	t = (float)elapsed / (float)DESKTOP_MS;
	if (t > 1.0f)
		t = 1.0f;
	t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
	return server->desktop_from + (server->desktop_to - server->desktop_from) * t;
}

/* Switches to a desktop (clamped to those there are), sliding from where the desktops are; its top window takes the focus. */
static void
desktop_turn(
	struct kwl_server *server,
	int target,
	const char *via)
{
	float from;

	/* One of the desktops. */
	if (target < 0)
		target = 0;
	if (target >= DESKTOPS)
		target = DESKTOPS - 1;

	/* A swap of arranged windows being dragged is given up (arrange-shell.c, ws177-p036). */
	kwl_arrange_swap_cancel(server, "desktop");

	/* From where they are to it. */
	from = desktop_position(server);
	server->desktop = (unsigned)target;
	server->desktop_from = from;
	server->desktop_to = (float)target;
	server->desktop_start_ms = kwl_milliseconds();
	server->desktop_moving = 1;
	server->desktop_dragging = 0;
	server->desktop_offset = 0;
	server->drag = NULL;
	server->pull = NULL;
	server->dirty = 1;

	/* The focus goes to the desktop's top window (or nobody). */
	server->front_surface = kwl_top_window(server);
	kwl_seat_focus(server);
	printf("KWL GLASS desktop=%u via=%s\n", server->desktop + 1U, via);
}

/*
 * Tells whether an output's bar has room for the applications' icons
 * (apps-bar.c, ws142-p004; a head's bar, ws113-p015), and where: the glass
 * look's windows, no login or lock screen, no docked window's title in it
 * (D6), not App Home; on the system bar also no fullscreen window over it
 * and not Wiseview (the anchor's).  The room is from after the launcher's
 * line to before the desktops' line, along the bar's top in the plane.
 */
int
kwl_glass_apps_room(
	struct kwl_server *server,
	unsigned slot,
	int32_t *left,
	int32_t *right,
	int32_t *top)
{
	struct kwl_plane_rect output;
	struct shell_bar bar;
	struct kwl_object *cover;
	struct kwl_object *docked;
	float home;
	int shown;

	/* Only the glass look's window mode has the bar, and not over the login or lock screen. */
	if (!server->glass ||
	    !server->windowed ||
	    server->greeter ||
	    server->locked)
		return 0;

	/* A head not shown now (unplugged, or the mirror mode) has no bar. */
	shown = kwl_output_rect(server, slot, &output);
	if (!shown && slot != KWL_PLANE_ANCHOR)
		return 0;

	/* A docked window's title. */
	docked = docked_window(server, slot);
	if (docked != NULL)
		return 0;

	/* App Home (open, opening or closing), over which no bar shows its icons. */
	home = kwl_home_progress(server);
	if (home > 0.0f || server->home_to > 0.0f)
		return 0;

	/* On the system bar, a fullscreen window over it, or Wiseview (open, opening or closing). */
	if (slot == KWL_PLANE_ANCHOR) {
		cover = bar_cover(server);
		if (cover != NULL)
			return 0;
		if (server->wiseview_gesture ||
		    server->wiseview > 0.0f ||
		    server->wiseview_moving)
			return 0;
	}

	/* The span: the applications' pill starts just after the launcher's line, and ends a gap before the desktops. */
	bar_layout_on(server, slot, &bar);
	*left = bar.title_x - 2;
	*right = bar.desktops_line - 16;
	*top = bar.top;

	/* Succeeded: there is room. */
	return 1;
}

/*
 * Switches to a window (an application switch: the bar's icon or preview,
 * the switcher, Wiseview, an activation, a press on a docked window of
 * another application): back from minimized, on top with the focus, and
 * made to follow the session's layout mode (ws142-p008, BUG-217): docked
 * in the docked mode, floating again in the windowed mode.
 */
void
kwl_glass_switch_to(
	struct kwl_server *server,
	struct kwl_object *surface,
	const char *via)
{
	/* Back, on top. */
	surface->minimized = 0;
	window_raise(server, surface);
	server->front_surface = kwl_top_window(server);
	kwl_seat_focus(server);
	server->dirty = 1;
	printf("KWL APPS raise surface=%u via=%s at_ms=%llu client=%llu\n", surface->id, via, (unsigned long long)kwl_milliseconds(), (unsigned long long)surface->client->number);

	/* Docked or floating as the mode is. */
	layout_match(server, surface, via);
}

/*
 * Brings a window to the front for an activation (xdg_activation_v1,
 * activation.c): App Home gives way, the window's desktop is shown when it
 * is another, and the window comes back from minimized, on top, with the
 * focus.
 */
void
kwl_glass_activate(
	struct kwl_server *server,
	struct kwl_object *surface,
	const char *via)
{
	/* App Home, when it shows, closes the way its launcher closes it. */
	kwl_home_dismiss(server, via);

	/* A window on another desktop: that desktop slides in. */
	if (surface->desktop != server->desktop)
		desktop_turn(server, (int)surface->desktop, via);

	/* Back, on top, with the focus, as the session's layout mode is. */
	kwl_glass_switch_to(server, surface, via);
}

/*
 * Opens a new window docked (ws099-p033, the 2026-10-05 UAT: a tablet used
 * over the whole screen) in the session's docked mode (ws142-p008): its
 * first configure is the docked space.  A window with a parent (a dialog
 * or a sheet) and a fullscreen one open as they would; one of a fixed size
 * is docked too, drawn at its size in the middle of the docked space.  The
 * place to come back to is the middle of the space at seven tenths of it.
 * Returns 1 when the window opens docked.
 */
int
kwl_glass_open_docked(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct kwl_layout_window window;
	struct kwl_object *front;
	struct shell_rect docked;
	unsigned long long front_client;
	unsigned front_id;
	int opens;

	/* Only the glass look's windows, not over the login or lock screen. */
	if (!server->glass ||
	    !server->windowed ||
	    server->greeter ||
	    server->locked)
		return 0;

	/* Docked when the session's mode is and the window is one that docks. */
	layout_window(surface, &window);
	opens = kwl_layout_opens_docked(server->layout_mode[window_slot(surface)], &window);
	if (!opens)
		return 0;

	/* The place to come back to: seven tenths of the docked space, in its middle. */
	dock_restore_default(server, surface);

	/* Docked: the first configure gives the docked space and the maximized state. */
	docked_rect(server, window_slot(surface), &docked);
	surface->maximized = 1;
	surface->x = docked.x;
	surface->y = docked.y;
	surface->window_width = (uint32_t)docked.width;
	surface->window_height = (uint32_t)docked.height;

	/* The window in front it opens over (none: 0). */
	front = sheet_owner(kwl_top_window(server));
	front_client = 0ULL;
	front_id = 0U;
	if (front != NULL) {
		front_client = (unsigned long long)front->client->number;
		front_id = front->id;
	}

	/* The log names where it opens (the tests read it). */
	printf("KWL GLASS open-docked client=%llu surface=%u front_client=%llu front=%u x=%d y=%d w=%d h=%d\n",
	       (unsigned long long)surface->client->number, surface->id, front_client, front_id,
	       (int)docked.x, (int)docked.y, (int)docked.width, (int)docked.height);

	/* Succeeded: the window opens docked. */
	return 1;
}

/*
 * Docks a window leaving fullscreen when the session's layout mode is
 * docked (ws142-p008: the mode decides, not what the window was before,
 * BUG-208), with the docked space, and keeps its floating place to come
 * back to: its place before fullscreen when it floated then, its restore
 * place when it was docked then, the middle of the space when it never had
 * a place.  Returns 1 when it is docked (the caller tells it), 0 when it
 * floats (the caller places it).
 */
int
kwl_glass_unfullscreen_docks(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct kwl_layout_window window;
	struct shell_rect docked;
	int docks;

	/* Only the glass look docks. */
	if (!server->glass)
		return 0;

	/* Docked when the mode is and the window is one that docks. */
	layout_window(surface, &window);
	docks = kwl_layout_unfullscreen_docked(server->layout_mode[window_slot(surface)], &window);
	if (!docks)
		return 0;

	/* A window that floated before fullscreen comes back to that place; one never placed, to the middle. */
	if (!surface->fullscreen_docked) {
		if (surface->placed && surface->window_width != 0U) {
			surface->restore_x = surface->window_x;
			surface->restore_y = surface->window_y;
			surface->restore_width = surface->window_width;
			surface->restore_height = surface->window_height;
		} else {
			dock_restore_default(server, surface);
		}
	}

	/* Docked: the space under the system bar, less what the on-screen keyboard's panel takes. */
	docked_rect(server, window_slot(surface), &docked);
	surface->maximized = 1;
	surface->x = docked.x;
	surface->y = docked.y;
	surface->window_width = (uint32_t)docked.width;
	surface->window_height = (uint32_t)docked.height;

	/* Succeeded: the window is docked. */
	return 1;
}

/*
 * Gives the system bar's colours in the appearance shown (ws099-p034b, the
 * 2026-10-06 user decision): the dark appearance keeps p034's light ink on
 * dark glass, the light one takes the floating title bar's dark ink.
 */
void
kwl_glass_bar_colours(
	struct kwl_server *server,
	struct glass_bar_colours *colours)
{
	static const float dark_ink[4] = { 1.0f, 1.0f, 1.0f, 0.94f };
	static const float dark_line[4] = { 1.0f, 1.0f, 1.0f, 0.18f };
	static const float dark_fill[4] = { 0.0f, 0.0f, 0.0f, 0.25f };
	static const float dark_edge[4] = { 1.0f, 1.0f, 1.0f, 0.12f };
	static const float dark_lit[4] = { 1.0f, 1.0f, 1.0f, 0.20f };
	static const float dark_faint[4] = { 1.0f, 1.0f, 1.0f, 0.42f };
	static const float light_ink[4] = { 0.12f, 0.16f, 0.24f, 0.94f };
	static const float light_line[4] = { 0.12f, 0.16f, 0.24f, 0.16f };
	static const float light_fill[4] = { 0.12f, 0.16f, 0.24f, 0.06f };
	static const float light_edge[4] = { 0.12f, 0.16f, 0.24f, 0.10f };
	static const float light_lit[4] = { 0.12f, 0.16f, 0.24f, 0.10f };
	static const float light_faint[4] = { 0.12f, 0.16f, 0.24f, 0.36f };

	/* The dark appearance's. */
	if (server->dark != 0) {
		memcpy(colours->ink, dark_ink, sizeof(colours->ink));
		memcpy(colours->line, dark_line, sizeof(colours->line));
		memcpy(colours->fill, dark_fill, sizeof(colours->fill));
		memcpy(colours->edge, dark_edge, sizeof(colours->edge));
		memcpy(colours->lit, dark_lit, sizeof(colours->lit));
		memcpy(colours->faint, dark_faint, sizeof(colours->faint));
		colours->light = 0U;
		return;
	}

	/* The light appearance's: the title bar's. */
	memcpy(colours->ink, light_ink, sizeof(colours->ink));
	memcpy(colours->line, light_line, sizeof(colours->line));
	memcpy(colours->fill, light_fill, sizeof(colours->fill));
	memcpy(colours->edge, light_edge, sizeof(colours->edge));
	memcpy(colours->lit, light_lit, sizeof(colours->lit));
	memcpy(colours->faint, light_faint, sizeof(colours->faint));
	colours->light = 1U;
}

/*
 * Opens Wiseview for the bar's "+N" place, as Super+Tab does.
 */
void
kwl_glass_open_wiseview(
	struct kwl_server *server,
	const char *via)
{
	/* The window on top is the one Enter comes back to. */
	server->wiseview_current = sheet_owner(kwl_top_window(server));
	printf("KWL WISEVIEW opening via=%s at_ms=%llu\n", via, (unsigned long long)kwl_milliseconds());
	kwl_transition_request(server, "wiseview-open");
	wiseview_settle(server, 0.0f, 1.0f);
}

/*
 * Tells whether the switcher may show (switcher-shell.c, ws142-p005), and
 * where: the glass look's windows, no login or lock screen, no fullscreen
 * window (D6), neither App Home nor Wiseview; in the middle with a docked
 * window (its title has the bar), otherwise at the bar's icons.
 */
int
kwl_glass_switch_place(
	struct kwl_server *server,
	unsigned *placement)
{
	struct kwl_object *cover;
	struct kwl_object *docked;
	float home;

	/* Only the glass look's window mode, and not over the login or lock screen. */
	if (!server->glass ||
	    !server->windowed ||
	    server->greeter ||
	    server->locked)
		return 0;

	/* Not over a fullscreen window. */
	cover = bar_cover(server);
	if (cover != NULL)
		return 0;

	/* Not with App Home, or Wiseview. */
	home = kwl_home_progress(server);
	if (home > 0.0f || server->home_to > 0.0f)
		return 0;
	if (server->wiseview_gesture ||
	    server->wiseview > 0.0f ||
	    server->wiseview_moving)
		return 0;

	/* In the middle with a docked window. */
	docked = docked_window(server, KWL_PLANE_ANCHOR);
	*placement = KWL_SWITCHER_BAR;
	if (docked != NULL)
		*placement = KWL_SWITCHER_CENTER;

	/* Succeeded: it may. */
	return 1;
}

/*
 * Draws an application's mark at a size (the bar's icons, Alt+Tab): its
 * tile with the picture cut out (ws128-p012), or a blue square with a
 * letter.
 */
void
kwl_glass_draw_app_mark(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	int32_t x,
	int32_t middle,
	int32_t size,
	float alpha)
{
	float square[4];
	float white[4];
	char title[128];
	char letter[5];
	const char *source;
	int32_t width;
	size_t length;
	size_t index;
	enum glass_hole hole;
	int picture;

	/* White, at the mark's opacity. */
	white[0] = 1.0f;
	white[1] = 1.0f;
	white[2] = 1.0f;
	white[3] = alpha;

	/* The tile of the picture that belongs to the window's application ID, its picture showing through the glass it is on. */
	picture = kwl_icon_for_app_id(surface->app_id);
	if (picture >= 0) {
		hole = mark_hole(server);
		glass_draw_app_tile(server, command, (unsigned)picture, (float)x, (float)(middle - size / 2), (float)size, alpha, 0.0f, hole);
		return;
	}

	/* Otherwise a blue square. */
	square[0] = 0.29f;
	square[1] = 0.55f;
	square[2] = 1.0f;
	square[3] = alpha;
	glass_draw_solid(server, command, (float)x, (float)(middle - size / 2), (float)size, (float)size, (float)size * 0.3f, square);

	/* Its letter: the application ID's, else the title's first character (all its UTF-8 bytes). */
	shown_title(surface, title, sizeof(title));
	source = mark_name(surface->app_id);
	if (source == NULL)
		source = title;
	length = 1;
	if (((unsigned char)source[0] & 0xe0U) == 0xc0U)
		length = 2;
	else if (((unsigned char)source[0] & 0xf0U) == 0xe0U)
		length = 3;
	else if (((unsigned char)source[0] & 0xf8U) == 0xf0U)
		length = 4;
	for (index = 0; index < length && source[index] != '\0'; index++)
		letter[index] = source[index];
	letter[index] = '\0';

	/* In capitals, in the middle. */
	if (letter[0] >= 'a' && letter[0] <= 'z')
		letter[0] = (char)(letter[0] - 'a' + 'A');
	width = glass_text_width(server, SIZE_TITLE, letter);
	glass_draw_text(server, command, SIZE_TITLE, x + size / 2 - width / 2, middle + 6, letter, size, white);
}

/*
 * Draws a window's preview for the bar's applications: Wiseview's tile, settled.
 */
void
kwl_glass_draw_preview(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height,
	int over)
{
	struct shell_rect tile;

	/* The tile, with its label, lit and with its close button under the pointer. */
	tile.x = x;
	tile.y = y;
	tile.width = width;
	tile.height = height;
	draw_tile(server, command, surface, &tile, 1.0f, 0U, (unsigned)over);
}

/*
 * Takes the touch pad's two-finger scroll while the switcher or Wiseview
 * shows (ws142-p009): it is no client's, and one swipe (from the fingers
 * landing until they lift) is one step, or a swipe down that brings or
 * chooses (swipe.c).  Returns 1 when the scroll was taken.
 */
int
kwl_glass_pad_scroll(
	struct kwl_server *server,
	int32_t vertical,
	int32_t horizontal,
	int natural)
{
	unsigned direction;
	int32_t across_um;
	int32_t down_um;
	int showing;
	int dialog;
	int taken;
	int down;

	/* Only while the power dialog, the switcher or Wiseview (its opening gesture done) shows. */
	dialog = kwl_power_dialog_showing(server);
	showing = wiseview_showing(server);
	if (server->wiseview_gesture)
		showing = 0;
	if (!dialog && !server->switcher.on && !showing)
		return 0;

	/* The fingers' way (natural scrolling turned back), notches back to travel. */
	across_um = horizontal * KWL_TOUCHPAD_NOTCH_UM;
	down_um = vertical * KWL_TOUCHPAD_NOTCH_UM;
	if (natural) {
		across_um = -across_um;
		down_um = -down_um;
	}

	/* What the swipe decided now, if anything. */
	direction = kwl_swipe_take(&server->pad_swipe, across_um, down_um);
	if (direction != KWL_SWIPE_NONE)
		printf("KWL SWIPE %s via=pad\n", kwl_swipe_name(direction));

	/* The power dialog first: a swipe down cancels it. */
	if (dialog) {
		down = 0;
		if (direction == KWL_SWIPE_DOWN)
			down = 1;
		(void)kwl_power_dialog_swipe(server, down);
		return 1;
	}

	/* The switcher, else Wiseview. */
	taken = kwl_switch_pad_swipe(server, direction);
	if (!taken)
		(void)wiseview_pad_swipe(server, direction);

	/* Succeeded: the scroll is taken. */
	return 1;
}

/*
 * Carries out a touch pad gesture (touchpad.c, ws142-p003): two fingers up
 * from the pad's bottom edge or three fingers up open Wiseview, two
 * fingers in from the left or the right edge switch to the desktop on that
 * side, each following the fingers as the pointer's edge drags do (D10);
 * two fingers down from the top edge open App Home following them
 * (ws181-p008) and on App Home two fingers up from the bottom edge close it
 * (ws181-p009); a tap of three fingers shows the switcher (ws142-p005).
 * Nothing starts over the login and lock screens, a fullscreen window
 * (D6), App Home, an open Wiseview or another swipe.
 */
void
kwl_glass_gesture(
	struct kwl_server *server,
	uint32_t gesture,
	uint32_t phase,
	int32_t travel_um,
	int32_t speed)
{
	const char *name;
	const char *phase_name;
	unsigned direction;
	float home;
	int switching;
	int dialog;
	int taken;
	int may;

	/* Logged, for the tests. */
	name = gesture_name(gesture);
	phase_name = gesture_phase_name(phase);
	printf("KWL GESTURE kind=%s phase=%s travel_um=%d speed=%d\n", name, phase_name, travel_um, speed);

	/* A scrolling touch lifted: the next swipe of Wiseview or the switcher is another (ws142-p009). */
	if (gesture == KWL_TOUCHPAD_GESTURE_SWIPE2) {
		kwl_swipe_end(&server->pad_swipe);
		return;
	}

	/* While the power dialog shows, two fingers down from the top edge cancel it, and nothing else starts. */
	dialog = kwl_power_dialog_showing(server);
	if (dialog) {
		direction = gesture_as_swipe(gesture);
		if (phase == KWL_TOUCHPAD_PHASE_BEGIN && direction == KWL_SWIPE_DOWN)
			(void)kwl_power_dialog_swipe(server, 1);
		return;
	}

	/*
	 * While the switcher is on, a tap of three fingers moves it on, an
	 * edge's swipe of two fingers is a swipe of it (ws142-p009), and
	 * nothing else starts (switcher-shell.c).
	 */
	switching = kwl_switch_on(server);
	if (switching) {
		if (gesture == KWL_TOUCHPAD_GESTURE_TAP3) {
			kwl_switch_step(server, 1, "tap3");
		} else if (phase == KWL_TOUCHPAD_PHASE_BEGIN) {
			direction = gesture_as_swipe(gesture);
			(void)kwl_switch_pad_swipe(server, direction);
		}

		/* Nothing else while the switcher is on. */
		return;
	}

	/* A Wiseview gesture under way goes on. */
	if (server->wiseview_pad) {
		gesture_wiseview(server, phase, travel_um, speed);
		return;
	}

	/* A desktop swipe under way goes on. */
	if (server->desktop_pad) {
		gesture_desktop(server, gesture, phase, travel_um, speed);
		return;
	}

	/* App Home following two fingers from the top edge goes on (home.c, ws181-p008). */
	if (server->home_pad) {
		kwl_home_pad(server, phase, travel_um, speed);
		return;
	}

	/* Otherwise only a beginning, or a tap, starts anything. */
	if (phase != KWL_TOUCHPAD_PHASE_BEGIN && gesture != KWL_TOUCHPAD_GESTURE_TAP3)
		return;

	/*
	 * On App Home, two fingers up from the bottom edge bring the desktop
	 * back following them (home.c, ws181-p009, the 2026-10-07 UAT; one
	 * finger on the edge is enough, as for every edge).
	 */
	home = kwl_home_progress(server);
	if (gesture == KWL_TOUCHPAD_GESTURE_BOTTOM2 &&
	    (home > 0.0f || server->home_to > 0.0f)) {
		kwl_home_pad(server, phase, travel_um, speed);
		return;
	}

	/* While Wiseview shows, an edge's swipe of two fingers is a swipe of its tiles (from the top edge down chooses). */
	if (phase == KWL_TOUCHPAD_PHASE_BEGIN) {
		direction = gesture_as_swipe(gesture);
		taken = wiseview_pad_swipe(server, direction);
		if (taken)
			return;
	}

	/* A fullscreen window: two fingers from the bottom or the top edge dock it (BUG-228, ws142-p009). */
	taken = gesture_fullscreen(server, gesture);
	if (taken)
		return;

	/* Every other gesture starts only where a gesture may. */
	may = gesture_may_start(server);
	if (!may)
		return;

	/*
	 * Two fingers from the top edge down: App Home opens following them
	 * (ws181-p008, the 2026-10-07 UAT; a docked window floats again by its
	 * title in the bar instead, BUG-224's way before).
	 */
	if (gesture == KWL_TOUCHPAD_GESTURE_TOP2) {
		kwl_home_pad(server, phase, travel_um, speed);
		return;
	}

	/* Each gesture. */
	switch (gesture) {
	case KWL_TOUCHPAD_GESTURE_BOTTOM2:
	case KWL_TOUCHPAD_GESTURE_UP3:
		/* Wiseview starts opening; the window on top is the current tile; a swipe in it begins afresh. */
		server->wiseview_gesture = 1;
		server->wiseview_pad = 1;
		server->wiseview_current = sheet_owner(kwl_top_window(server));
		kwl_swipe_end(&server->pad_swipe);
		printf("KWL WISEVIEW gesture via=pad\n");
		gesture_wiseview(server, phase, travel_um, speed);
		break;
	case KWL_TOUCHPAD_GESTURE_LEFT2:
	case KWL_TOUCHPAD_GESTURE_RIGHT2:
		/* The desktops start sliding. */
		server->desktop_pad = 1;
		server->desktop_dragging = 1;
		server->desktop_moving = 0;
		printf("KWL GLASS desktop swipe via=pad\n");
		gesture_desktop(server, gesture, phase, travel_um, speed);
		break;
	default:
		/* TAP3: the switcher (D1). */
		(void)kwl_switch_open(server, KWL_SWITCHER_VIA_PAD);
		break;
	}
}

/* Follows the touch pad's Wiseview gesture: opening with the travel, and open or closed at its end. */
static void
gesture_wiseview(
	struct kwl_server *server,
	uint32_t phase,
	int32_t travel_um,
	int32_t speed)
{
	float progress;

	/* A button may have ended the gesture already: the rest of it does nothing. */
	if (!server->wiseview_gesture) {
		if (phase == KWL_TOUCHPAD_PHASE_END || phase == KWL_TOUCHPAD_PHASE_CANCEL)
			server->wiseview_pad = 0;
		return;
	}

	/* How far it is open. */
	progress = (float)travel_um / (float)GESTURE_WISEVIEW_UM;
	if (progress < 0.0f)
		progress = 0.0f;
	if (progress > 1.0f)
		progress = 1.0f;
	server->wiseview_pad_progress = progress;
	server->dirty = 1;

	/* Under way: it follows. */
	if (phase == KWL_TOUCHPAD_PHASE_BEGIN || phase == KWL_TOUCHPAD_PHASE_UPDATE)
		return;

	/* The end: past the threshold or flicked up it opens, otherwise (or given up) it closes. */
	server->wiseview_gesture = 0;
	server->wiseview_pad = 0;
	if (phase == KWL_TOUCHPAD_PHASE_END &&
	    (progress > WISEVIEW_THRESHOLD || speed >= GESTURE_FLICK)) {
		printf("KWL WISEVIEW opening from=%.2f\n", (double)progress);
		wiseview_settle(server, progress, 1.0f);
	} else {
		printf("KWL WISEVIEW cancel from=%.2f\n", (double)progress);
		wiseview_settle(server, progress, 0.0f);
	}
}

/*
 * Follows the touch pad's desktop gesture: the desktops slide with the
 * travel (with resistance where there is no neighbour), and at its end
 * half the output's width or a flick switches to the neighbour, anything
 * less goes back.
 */
static void
gesture_desktop(
	struct kwl_server *server,
	uint32_t gesture,
	uint32_t phase,
	int32_t travel_um,
	int32_t speed)
{
	int32_t travel;
	int32_t offset;
	int target;

	/* Something else may have ended the slide already: the rest of the gesture does nothing. */
	if (!server->desktop_dragging) {
		if (phase == KWL_TOUCHPAD_PHASE_END || phase == KWL_TOUCHPAD_PHASE_CANCEL)
			server->desktop_pad = 0;
		return;
	}

	/* The travel in pixels, inward; from the left edge (the fingers going right) the offset is positive, as the pointer's. */
	travel = (int32_t)((int64_t)travel_um * (int32_t)server->width / GESTURE_DESKTOP_UM);
	offset = travel;
	target = (int)server->desktop - 1;
	if (gesture == KWL_TOUCHPAD_GESTURE_RIGHT2) {
		offset = -travel;
		target = (int)server->desktop + 1;
	}

	/* No neighbour on that side resists. */
	if ((offset > 0 && server->desktop == 0U) ||
	    (offset < 0 && server->desktop + 1U >= (unsigned)DESKTOPS))
		offset /= 4;
	server->desktop_offset = offset;
	server->dirty = 1;

	/* Under way: it follows. */
	if (phase == KWL_TOUCHPAD_PHASE_BEGIN || phase == KWL_TOUCHPAD_PHASE_UPDATE)
		return;

	/* The end: far enough or flicked, the neighbour (desktop_turn keeps the end desktops); otherwise, or given up, back. */
	server->desktop_pad = 0;
	if (phase == KWL_TOUCHPAD_PHASE_END &&
	    (travel >= (int32_t)server->width / 2 || (speed >= GESTURE_FLICK && travel > 0))) {
		desktop_turn(server, target, "pad");
	} else {
		desktop_turn(server, (int)server->desktop, "pad");
	}
}

/*
 * Takes a swipe of two fingers while Wiseview shows (ws142-p009, BUG-216):
 * to the right the next tile, to the left the one before, down chooses the
 * current tile.  Returns 1 when Wiseview took it (it shows, its own opening
 * gesture done).
 */
static int
wiseview_pad_swipe(
	struct kwl_server *server,
	unsigned direction)
{
	int showing;

	/* Only Wiseview shown, not while its opening gesture holds it. */
	showing = wiseview_showing(server);
	if (!showing || server->wiseview_gesture)
		return 0;

	/* Each swipe's work; up, or no decision yet, does nothing. */
	if (direction == KWL_SWIPE_RIGHT) {
		printf("KWL WISEVIEW select step=+1 via=swipe\n");
		wiseview_move(server, 1);
	} else if (direction == KWL_SWIPE_LEFT) {
		printf("KWL WISEVIEW select step=-1 via=swipe\n");
		wiseview_move(server, -1);
	} else if (direction == KWL_SWIPE_DOWN) {
		wiseview_choose(server, "swipe");
	}

	/* Succeeded: the swipe is Wiseview's. */
	return 1;
}

/*
 * Takes two fingers from the bottom or the top edge over a fullscreen
 * window (BUG-228, the 2026-10-06 user decision, one step at a time): the
 * window leaves fullscreen docked, and the session's layout mode becomes
 * docked.  Returns 1 when it took the gesture.
 */
static int
gesture_fullscreen(
	struct kwl_server *server,
	uint32_t gesture)
{
	struct kwl_object *top;
	float home;
	int error;

	/* Only the edges' two fingers up from the bottom and down from the top. */
	if (gesture != KWL_TOUCHPAD_GESTURE_BOTTOM2 && gesture != KWL_TOUCHPAD_GESTURE_TOP2)
		return 0;

	/* Not over the login and lock screens, nor App Home. */
	if (server->greeter || server->locked)
		return 0;
	home = kwl_home_progress(server);
	if (home > 0.0f)
		return 0;

	/* The window on top, fullscreen. */
	top = kwl_top_window(server);
	if (top == NULL || !top->fullscreen)
		return 0;

	/* Docked from now on: the window leaves fullscreen into the docked space (kwl_glass_unfullscreen_docks). */
	layout_set(server, window_slot(top), KWL_LAYOUT_DOCKED, gesture_name(gesture));
	error = kwl_window_leave_fullscreen(top);
	printf("KWL GLASS fullscreen-leave surface=%u via=%s error=%d client=%llu\n", top->id, gesture_name(gesture), error, (unsigned long long)top->client->number);

	/* Succeeded: the gesture was the fullscreen window's way out. */
	return 1;
}

/*
 * Gives what an edge's gesture of two fingers is as a swipe (while Wiseview
 * or the switcher shows): from the left edge the fingers go right, from the
 * right edge left, from the top edge down, from the bottom edge up.
 */
static unsigned
gesture_as_swipe(
	uint32_t gesture)
{
	/* Each edge's way. */
	switch (gesture) {
	case KWL_TOUCHPAD_GESTURE_LEFT2:
		return KWL_SWIPE_RIGHT;
	case KWL_TOUCHPAD_GESTURE_RIGHT2:
		return KWL_SWIPE_LEFT;
	case KWL_TOUCHPAD_GESTURE_TOP2:
		return KWL_SWIPE_DOWN;
	case KWL_TOUCHPAD_GESTURE_BOTTOM2:
		return KWL_SWIPE_UP;
	default:
		return KWL_SWIPE_NONE;
	}
}

/* Tells whether a touch pad gesture may start: not over the login and lock screens, a fullscreen window (D6), App Home, Wiseview or another swipe. */
static int
gesture_may_start(
	struct kwl_server *server)
{
	struct kwl_object *cover;
	float home;

	/* The login and lock screens. */
	if (server->greeter || server->locked)
		return 0;

	/* A fullscreen window: the compositor's gestures are away (D6). */
	cover = bar_cover(server);
	if (cover != NULL)
		return 0;

	/* Wiseview shown or moving, a desktop swipe or slide. */
	if (server->wiseview_gesture ||
	    server->wiseview > 0.0f ||
	    server->wiseview_moving)
		return 0;
	if (server->desktop_press ||
	    server->desktop_dragging ||
	    server->desktop_moving)
		return 0;

	/* App Home shown or following its gesture. */
	home = kwl_home_progress(server);
	if (home > 0.0f || server->home_to > 0.0f)
		return 0;

	/* Succeeded: it may. */
	return 1;
}

/* Names a touch pad gesture for the log. */
static const char *
gesture_name(
	uint32_t gesture)
{
	/* Each one. */
	switch (gesture) {
	case KWL_TOUCHPAD_GESTURE_BOTTOM2:
		return "bottom2";
	case KWL_TOUCHPAD_GESTURE_UP3:
		return "up3";
	case KWL_TOUCHPAD_GESTURE_LEFT2:
		return "left2";
	case KWL_TOUCHPAD_GESTURE_RIGHT2:
		return "right2";
	case KWL_TOUCHPAD_GESTURE_TAP3:
		return "tap3";
	case KWL_TOUCHPAD_GESTURE_TOP2:
		return "top2";
	case KWL_TOUCHPAD_GESTURE_SWIPE2:
		return "swipe2";
	default:
		return "none";
	}
}

/* Names a gesture's phase for the log. */
static const char *
gesture_phase_name(
	uint32_t phase)
{
	/* Each one. */
	switch (phase) {
	case KWL_TOUCHPAD_PHASE_BEGIN:
		return "begin";
	case KWL_TOUCHPAD_PHASE_UPDATE:
		return "update";
	case KWL_TOUCHPAD_PHASE_END:
		return "end";
	default:
		return "cancel";
	}
}

/* Ends a press at the edge: a swipe of a quarter of the output switches to the neighbour, a shorter one goes back. */
static void
desktop_release(
	struct kwl_server *server)
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
	struct kwl_server *server,
	unsigned desktop)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	unsigned count;

	/* Every live, mapped window with a role on that desktop. */
	count = 0U;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != KWL_SURFACE || surface->dead || !surface->mapped || surface->role == NULL || surface->cursor_role)
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
	struct kwl_server *server,
	const struct kwl_object *surface,
	struct shell_rect *body)
{
	struct kwl_plane_rect output;
	struct shell_rect docked;
	struct shell_rect own;
	float t;

	/* The docked space of its output. */
	docked_rect(server, window_slot(surface), &docked);
	(void)kwl_output_rect(server, window_slot(surface), &output);

	/* Its own size, placed as the pull leaves it (the same part of the title under the pointer, across its output). */
	own.width = (int32_t)surface->restore_width;
	own.height = (int32_t)surface->restore_height;
	own.x = server->pointer_x - (int32_t)((int64_t)surface->restore_width * (server->pointer_x - output.x) / (int32_t)output.width);
	own.y = server->pointer_y + KWL_GLASS_GAP + KWL_GLASS_TITLE / 2;

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
	struct kwl_server *server)
{
	struct kwl_object *surface;
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
	docked_rect(server, window_slot(surface), &to);
	memcpy(server->anim_from, &from, sizeof(server->anim_from));
	memcpy(server->anim_to, &to, sizeof(server->anim_to));
	server->anim = surface;
	server->anim_docking = 1;
	server->anim_start_ms = kwl_milliseconds();
	server->dirty = 1;
	printf("KWL GLASS pull back surface=%u\n", surface->id);
}

/* Hides a window (it keeps its place, and comes back from Wiseview); the next window takes the focus. */
static void
window_minimize(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	/* Hidden, not moved or pulled any more. */
	surface->minimized = 1;
	if (server->drag == surface)
		server->drag = NULL;
	if (server->pull == surface)
		server->pull = NULL;

	/* The focus goes to the window under it. */
	server->front_surface = kwl_top_window(server);
	kwl_seat_focus(server);
	server->dirty = 1;
	printf("KWL GLASS minimize surface=%u client=%llu\n", surface->id, (unsigned long long)surface->client->number);

	/* A docked window minimized ends the docked mode now, not at the next frame (WS181). */
	layout_follow(server);
}

/* Moves a window to another desktop (shown when that desktop is), and gives the focus to the top window of the desktop shown. */
static void
window_to_desktop(
	struct kwl_server *server,
	struct kwl_object *surface,
	unsigned desktop,
	const char *via)
{
	unsigned from;

	/* The window's desktop; on top of it there. */
	from = surface->desktop;
	surface->desktop = desktop;
	server->map_order++;
	surface->map_order = server->map_order;

	/* The focus on the desktop shown. */
	server->front_surface = kwl_top_window(server);
	kwl_seat_focus(server);
	server->dirty = 1;
	printf("KWL GLASS move-desktop surface=%u desktop=%u via=%s client=%llu\n", surface->id, desktop + 1U, via, (unsigned long long)surface->client->number);

	/* The arrangements of the desktop it left and the one it came to end (WS181). */
	kwl_arrange_moved(server, surface, from);
}

/* Returns the desktop whose picture in the system bar is under a point, or -1. */
static int
desktop_picture_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct shell_bar bar;
	int32_t offset;

	/* In the bar, over the pictures. */
	if (y < 0 || y >= KWL_GLASS_BAR)
		return -1;
	bar_layout(server, &bar);
	offset = x - (bar.desktops_x + DESKTOPS_PAD);
	if (offset < 0 || offset >= DESKTOPS * (DESKTOP_WIDTH + DESKTOP_GAP))
		return -1;

	/* The picture (its gap counts as its own). */
	return offset / (DESKTOP_WIDTH + DESKTOP_GAP);
}

/* Follows the pointer for the glass look's screens, gestures, menus and moves; returns 1 when the motion is theirs. */
static int
glass_motion_take(
	struct kwl_server *server)
{
	struct kwl_plane_rect output;
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct kwl_object *surface;
	int32_t lowest;
	int32_t x;
	int32_t y;
	int32_t dx;
	int taken;
	int still;
	int calm;

	/* The hover of buttons, the dock hint and the moves are redrawn (over a window's own area only the cursor is, damage.c). */
	calm = kwl_glass_pointer_calm(server, server->pointer_x, server->pointer_y);
	if (!calm)
		server->dirty = 1;

	/* The login screen lights its buttons under the pointer, and takes the motion. */
	if (server->greeter) {
		server->dirty = 1;
		return 1;
	}

	/* So do a pairing's question and the power dialog while they show (bluetooth-ask.c, power-dialog.c). */
	taken = kwl_bluetooth_ask_motion(server);
	if (taken)
		return 1;
	taken = kwl_power_dialog_motion(server);
	if (taken)
		return 1;

	/* Wiseview follows the gesture, and hears the pointer while it is open; a pressed tile that moves is dragged. */
	if (server->wiseview_gesture || server->wiseview > 0.0f || server->wiseview_moving) {
		if (server->wiseview_press != NULL && !server->wiseview_dragging) {
			x = server->pointer_x - server->wiseview_press_x;
			y = server->pointer_y - server->wiseview_press_y;
			if (x * x + y * y >= TILE_DRAG_START * TILE_DRAG_START) {
				server->wiseview_dragging = 1;
				printf("KWL WISEVIEW drag surface=%u client=%llu\n", server->wiseview_press->id, (unsigned long long)server->wiseview_press->client->number);
			}
		}

		/* The motion is Wiseview's. */
		return 1;
	}

	/* A press held in the top edge's band: the swipe down to Wiseview, or given again to what is under it (WS181). */
	taken = band_motion(server, 1);
	if (taken)
		return 1;

	/* The top-right corner's swipe follows the pointer (corner.c). */
	taken = kwl_corner_motion(server);
	if (taken)
		return 1;

	/* The on-screen keyboard's swipe and a press on its panel (keyboard.c). */
	taken = kwl_keyboard_motion(server);
	if (taken)
		return 1;

	/* App Home follows its gesture, and hears the pointer while it shows. */
	taken = kwl_home_motion(server);
	if (taken)
		return 1;

	/* The network's open menu lights the row under the pointer (network.c), and so does Bluetooth's (bluetooth-bar.c). */
	taken = kwl_network_motion(server);
	if (taken)
		return 1;
	taken = kwl_bluetooth_motion(server);
	if (taken)
		return 1;

	/* The arrangement menu lights its item, and a swap follows the pointer (arrange-shell.c). */
	taken = kwl_arrange_motion(server);
	if (taken)
		return 1;

	/* The volume's open popup follows a drag of its slider (volume.c). */
	taken = kwl_volume_motion(server);
	if (taken)
		return 1;

	/* An open menu follows the pointer (menu-shell.c). */
	taken = kwl_menu_motion(server);
	if (taken)
		return 1;

	/* The bar's applications: the rest on an icon, its drag, the previews (apps-bar.c). */
	taken = kwl_apps_bar_motion(server);
	if (taken)
		return 1;

	/* A press on a titlebar's search field selects text, or waits to be a click or a move (titlebar-shell.c). */
	taken = kwl_titlebar_motion(server);
	if (taken)
		return 1;

	/* The desktops' swipe: past DESKTOP_START the windows follow the pointer (with resistance where there is no neighbour). */
	if (server->desktop_press) {
		dx = server->pointer_x - server->desktop_start_x;
		if (!server->desktop_dragging && (dx >= DESKTOP_START || dx <= -DESKTOP_START)) {
			server->desktop_dragging = 1;
			server->desktop_moving = 0;
			printf("KWL GLASS desktop swipe\n");
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

		/* Not far enough yet, in any direction (WS181): the window follows the pull, past a tap's jitter (BUG-247). */
		server->pull_distance = kwl_edge_distance(server->pointer_x - server->pull_start_x, server->pointer_y - server->pull_start_y);
		if (server->pull_distance < PULL_SLOP)
			server->pull_distance = 0;
		if (server->pull_distance < PULL_DISTANCE)
			return 1;

		/*
		 * The same part of the title bar stays under the pointer.  The
		 * window comes back from where the pull left it (its own size
		 * there), not from the docked space (BUG-180), so the pull's
		 * distance is forgotten only after.
		 */
		(void)kwl_output_rect(server, window_slot(surface), &output);
		x = server->pointer_x - (int32_t)((int64_t)surface->restore_width * (server->pointer_x - output.x) / (int32_t)output.width);
		y = server->pointer_y + KWL_GLASS_GAP + KWL_GLASS_TITLE / 2;
		layout_leave(server, window_slot(surface), surface, x, y, "pull");
		server->pull_distance = 0;
		server->pull = NULL;
		server->drag = surface;
		server->drag_left_bar = 0U;
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

	/* A move that has been out of the bar of the output the pointer is on (or crossed to another) may dock the window by a release back in it. */
	(void)kwl_outputs(server, outputs);
	if (server->pointer_y >= outputs[server->pointer_output].y + KWL_GLASS_BAR || server->pointer_output != surface->output)
		server->drag_left_bar = 1U;

	/*
	 * The window is the output's the pointer is on: across a shared edge
	 * it goes there whole at once, drawn by that output alone from the next
	 * frame (D-ATOMIC, ws113-p007).
	 */
	if (surface->output != server->pointer_output)
		kwl_window_set_output(server, surface, server->pointer_output, "drag");

	/* A double click's second press does not move the window within its slop (BUG-265). */
	still = kwl_title_tap_still(&server->title_tap, surface, server->pointer_x, server->pointer_y);
	if (still)
		return 1;

	/* The body follows the pointer; the title bar stays below the system bar (below the head's top on a head). */
	surface->x = server->pointer_x - server->drag_dx;
	surface->y = server->pointer_y - server->drag_dy;
	(void)kwl_outputs(server, outputs);
	lowest = outputs[surface->output].y + kwl_output_top(server, surface->output);
	if (surface->y < lowest)
		surface->y = lowest;

	/* Succeeded: the motion was the compositor's. */
	return 1;
}

/*
 * Takes the focused window out of fullscreen with F11 alone or Super+Down
 * (BUG-194): the compositor gives it its window's place and size back
 * whether the application answers or not.  A window that is not fullscreen
 * leaves both keys to its menus and to itself (Terminal's Fullscreen item).
 * Returns 1 when the key was taken, its release with it.
 */
static int
fullscreen_leave_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	struct kwl_object *surface;
	const char *via;
	int error;

	/* The release of the press taken goes no further. */
	if (state == 0U) {
		if (fullscreen_leave_eaten == 0U || key != fullscreen_leave_eaten)
			return 0;
		fullscreen_leave_eaten = 0U;
		return 1;
	}

	/* F11 alone, or Down with Super and nothing else. */
	if (key == FULLSCREEN_LEAVE_KEY && (server->modifiers & MODIFIERS_ANY) == 0U) {
		via = "f11";
	} else if (key == FULLSCREEN_LEAVE_DOWN && (server->modifiers & MODIFIER_SUPER) != 0U &&
		   (server->modifiers & MODIFIERS_NOT_SUPER) == 0U) {
		via = "super-down";
	} else {
		return 0;
	}

	/* The focused window, when it is fullscreen. */
	surface = server->focus;
	if (surface == NULL || surface->dead || !surface->fullscreen)
		return 0;

	/* A window again, told so (a refused configure leaves the window as the compositor draws it). */
	error = kwl_window_leave_fullscreen(surface);
	fullscreen_leave_eaten = key;
	printf("KWL GLASS fullscreen-leave surface=%u via=%s error=%d client=%llu\n", surface->id, via, error, (unsigned long long)surface->client->number);

	/* Succeeded: the key was the compositor's. */
	return 1;
}

/*
 * Moves the focused window to the display beside it with Super+Shift and
 * Left or Right (ws113-p007, as Windows does): in the extended mode, to
 * the output whose middle is nearest that way, at the same share of the
 * way across.  A docked or fullscreen window stays (it is the anchor's).
 * Returns 1 when the key was taken, its release with it.
 */
static int
display_move_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct kwl_object *surface;
	unsigned count;
	int direction;
	int target;

	/* The release of the press taken goes no further. */
	if (state == 0U) {
		if (display_move_eaten == 0U || key != display_move_eaten)
			return 0;
		display_move_eaten = 0U;
		return 1;
	}

	/* Left or Right with Super and Shift, and nothing else. */
	if (key != SHORTCUT_LEFT && key != SHORTCUT_RIGHT)
		return 0;
	if ((server->modifiers & MODIFIERS_ANY) != (MODIFIER_SUPER | MODIFIER_SHIFT))
		return 0;
	display_move_eaten = key;
	direction = 1;
	if (key == SHORTCUT_LEFT)
		direction = -1;

	/* The focused window (a sheet's parent for a sheet), floating. */
	surface = sheet_owner(kwl_top_window(server));
	if (surface == NULL || surface->maximized || surface->fullscreen) {
		printf("KWL WINDOW output none direction=%d why=key\n", direction);
		return 1;
	}

	/* The display beside it, if there is one that way. */
	count = kwl_outputs(server, outputs);
	target = kwl_plane_neighbour(outputs, count, surface->output, direction);
	if (target < 0) {
		printf("KWL WINDOW output none surface=%u direction=%d why=key\n", surface->id, direction);
		return 1;
	}

	/* Succeeded: carried there. */
	kwl_window_to_output(server, surface, (unsigned)target, "key");
	return 1;
}

/*
 * Switches to the desktop before or after with Alt+Shift and Left or Right
 * (ws181-p010, the 2026-10-08 user decision: Ctrl+Shift is the applications'
 * word selection).  Only Alt and Shift are held; Ctrl+Alt+Shift stays the
 * key that takes the window on top along.  At the first or last desktop the
 * key is still taken and nothing turns.  Returns 1 when the key was taken,
 * its release with it.
 */
static int
desktop_alt_shift_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	int target;

	/* The release of the press taken goes no further. */
	if (state == 0U) {
		if (desktop_key_eaten == 0U || key != desktop_key_eaten)
			return 0;
		desktop_key_eaten = 0U;
		return 1;
	}

	/* Left or Right with Alt and Shift, and nothing else. */
	if (key != SHORTCUT_LEFT && key != SHORTCUT_RIGHT)
		return 0;
	if ((server->modifiers & MODIFIERS_ANY) != MODIFIERS_ALT_SHIFT)
		return 0;
	desktop_key_eaten = key;

	/* The neighbour that way. */
	target = (int)server->desktop + 1;
	if (key == SHORTCUT_LEFT)
		target = (int)server->desktop - 1;

	/* None beyond the first or the last desktop. */
	if (target < 0 || target >= DESKTOPS) {
		printf("KWL GLASS desktop stays=%u via=alt-shift\n", server->desktop + 1U);
		return 1;
	}

	/* Succeeded: the desktop turns. */
	desktop_turn(server, target, "alt-shift");
	return 1;
}

/*
 * Draws the desktop layer's content over its wallpaper: the desktop's
 * icons, then the windows bottom to top (kwl_glass_draw), the one on top
 * focused.
 */
static void
draw_desktop_layer(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object **windows,
	unsigned count,
	float home,
	const struct shell_bar *bar)
{
	struct kwl_object *top;
	unsigned index;
	unsigned focused;
	unsigned drawn;
	unsigned blur;
	int shown;
	int centred;
	float position;

	/* The desktop's icons over the wallpaper, with the layer (desktop.c). */
	kwl_desktop_draw(server, command);

	/*
	 * The windows; the top one has the focus; where a dragged one would
	 * dock shows just under it.  The desktop shown's windows, and while
	 * the desktops slide or are swiped, the neighbour's too, a screen's
	 * width to the side (Home, when it shows, has the desktop shown only).
	 */
	top = kwl_top_window(server);
	position = desktop_position(server);
	drawn = 0;
	for (index = 0; index < count; index++) {
		shown = window_shown(server, windows[index], home, position);
		if (!shown)
			continue;

		/*
		 * The glass of a window over others that asked for it (set_blur,
		 * ws075-p029) shows them blurred (backdrop.c), not while Home has the
		 * layer; any other window's glass shows the blurred wallpaper alone,
		 * which costs nothing more (the default).
		 */
		blur = kwl_panels_blur(windows[index]);
		centred = window_centred_over(server, windows[index]);
		if (centred && home <= 0.0f) {
			/*
			 * A window in the middle of the docked space (ws142-p008b, the
			 * 2026-10-06 user decision) stands on the scene under it blurred
			 * as one layer: the wallpaper, its application's other windows,
			 * its docked parent.
			 */
			draw_backdrop(server, command, windows, index, position);
			draw_centred_cover(server, command);
		} else if (drawn > 0U && home <= 0.0f && blur) {
			draw_backdrop(server, command, windows, index, position);
		} else {
			kwl_backdrop_reset(server);
		}

		/* One more window drawn. */
		drawn++;

		/* Shifted with the layer, when Home does not have it. */
		window_layer(server, windows[index], home, position);

		/* The window. */
		focused = 0;
		if (windows[index] == top)
			focused = 1;
		if (windows[index] == server->drag)
			draw_dock_hint(server, command);
		draw_window(server, command, windows[index], focused, bar);
	}

	/* Succeeded: the layer is drawn. */
	return;
}

/* Logs the desktop layer left out under App Home, or drawn again (ws177-p033), when that changes. */
static void
home_layer_log(
	unsigned hidden)
{
	/* Only a change. */
	if (hidden == shell_home_layer_hidden)
		return;

	/* The new state, logged once. */
	shell_home_layer_hidden = hidden;
	printf("KWL HOME layer hidden=%u\n", hidden);
}
