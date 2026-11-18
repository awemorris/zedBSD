/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_WINDOW_H
#define KEILAND_WINDOW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kl_canvas;
struct kl_glass_panel;
struct kl_rect;
struct kl_theme;

/*
 * The window (window.c, present.c, present-shm.c, clipboard.c, primary.c): an
 * xdg-shell toplevel of its own connection, its seat's input, the frames the
 * application draws on the CPU, and the clipboard and the primary selection.
 *
 * The input arrives as events in a queue the application takes after each
 * kl_window_dispatch, in the order they came: the pointer, the wheel, the
 * keys (a held key repeats: the application calls kl_window_repeat after the
 * dispatch, so that a release read in the same dispatch stops it first), the
 * keyboard's focus, the fingers (their times turned into CLOCK_MONOTONIC
 * microseconds), a new size and the request to close.  An application posts
 * its own inputs heard through other objects during a dispatch (a System
 * Menu's shortcut, a titlebar's control) with kl_window_post, so that they
 * keep their place among the keys (typed text, then Ctrl+S); the program's
 * other threads post to a window of a kl_app the same way.  Input on the
 * program's other surfaces (a file chooser's window) is not the window's and
 * never queued.
 *
 * A frame is ordinary memory of premultiplied 0xAARRGGBB words the size
 * kl_window_present_resize reported.  KL_PRESENT_VULKAN shows it through
 * a Vulkan swapchain (see-through when the compositor offers it, the way
 * the compositor's glass needs), KL_PRESENT_SHM through wl_shm buffers (for a
 * small window of a library, or where Vulkan is missing), and
 * KL_PRESENT_NONE leaves the surface to the application's own Vulkan.
 * The menus, the titlebar's controls and the glass panels stay the
 * application's (libkeiland), on the objects the accessors give.
 */
struct kl_window;
struct wl_display;
struct wl_surface;
struct wl_seat;
struct xdg_toplevel;

/* How the frames are shown. */
#define KL_PRESENT_VULKAN	0U
#define KL_PRESENT_SHM		1U
#define KL_PRESENT_NONE	2U

/* The kinds of input. */
#define KL_WINDOW_MOTION	1U
#define KL_WINDOW_LEAVE	2U
#define KL_WINDOW_BUTTON	3U
#define KL_WINDOW_AXIS		4U
#define KL_WINDOW_KEY		5U
#define KL_WINDOW_FOCUS	6U
#define KL_WINDOW_TOUCH_DOWN	7U
#define KL_WINDOW_TOUCH_MOTION	8U
#define KL_WINDOW_TOUCH_UP	9U
#define KL_WINDOW_TOUCH_CANCEL	10U
#define KL_WINDOW_RESIZE	11U
#define KL_WINDOW_CLOSE	12U
#define KL_WINDOW_POST		13U

/*
 * The text an input method or the compositor's on-screen keyboard sends
 * through the text input (text-input-unstable-v3), while the window asks for
 * it (kl_window_text_input): text to insert at the caret in place of the
 * selection (text), the text being composed to show at the caret until it is
 * committed or replaced (text, empty when it goes; begin and end are its
 * cursor's byte offsets, -1 when hidden), and bytes to delete before and
 * after the caret first (before, after).  They come in the order of the
 * protocol's done: delete, commit, preedit.
 */
#define KL_WINDOW_TEXT_COMMIT	14U
#define KL_WINDOW_TEXT_PREEDIT	15U
#define KL_WINDOW_TEXT_DELETE	16U

/*
 * KL_VERSION 40: what a KL_WINDOW_AXIS came from (axis_source: a wheel, a
 * touch pad's fingers, or something continuous), and the end of the fingers'
 * scrolling, KL_WINDOW_AXIS_STOP, after which the content may fly on
 * (kl_ui_axis, kl_scroll_axis_stop).  It is 18 (KL_VERSION 43): KL_VERSION 40
 * gave it 17, the number KL_WINDOW_ACTION has had since KL_VERSION 26, so
 * that a lift of the fingers came to an application of kl_app as an action.
 */
#define KL_WINDOW_AXIS_STOP	18U
#define KL_AXIS_SOURCE_WHEEL		0U
#define KL_AXIS_SOURCE_FINGER		1U
#define KL_AXIS_SOURCE_CONTINUOUS	2U

/* The longest text one input carries, with its NUL (a longer one is cut at a character's start). */
#define KL_WINDOW_TEXT_MAX	256U

/* The evdev codes of the pointer's buttons. */
#define KL_BUTTON_LEFT		0x110U
#define KL_BUTTON_RIGHT	0x111U
#define KL_BUTTON_MIDDLE	0x112U

/*
 * What a window is made with.  Any pointer may be NULL: display (the
 * WAYLAND_DISPLAY one), title and application (the app_id).  width and height
 * are the size asked for until the compositor gives one.  fullscreen asks for
 * the full screen before the window is first configured, so that its first
 * configure is the full screen's.  role and token (KL_VERSION 46) make the
 * desktop's surface instead.
 */
struct kl_window_options {
	const char *display;
	const char *title;
	const char *application;
	uint32_t width;
	uint32_t height;
	unsigned present;
	int fullscreen;
	unsigned role;
	const char *token;
};

/*
 * KL_VERSION 46: a window's role (kl_window_options' role): a toplevel
 * window, or the desktop's surface under every window (Files' desktop,
 * with the token the compositor gave its program in token).  A desktop
 * surface has no title, menus, titlebar, full screen or maximized state;
 * the compositor gives its size.
 */
#define KL_WINDOW_ROLE_TOPLEVEL	0U
#define KL_WINDOW_ROLE_DESKTOP	1U

/*
 * One input: its kind (KL_WINDOW_*), where the pointer or the finger is
 * (surface pixels), a button's or a key's code and whether it is pressed (a
 * focus: 1 when it came), whether a key is a repeat, the modifiers held
 * (KL_MOD_*), the wheel's distance in pixels, a finger's id and the time it
 * happened (a finger's, and the pointer's motions and buttons, from the
 * compositor's time; otherwise when it was read), when it was read, and its
 * serial (a press's, for a popup or a selection); for the text input's, its
 * text, the composed text's cursor, and the bytes to delete around the caret;
 * for a pen tablet's (KL_VERSION 44), its tool (KL_TABLET_*), its barrel
 * buttons held (KL_TABLET_BUTTON_*), its pressure (0 to 1, -1 for a tool
 * without it) and its tilt (degrees); for KL_WINDOW_POST, the code and the
 * data posted (data from KL_VERSION 97).  The text of a KL_WINDOW_KEY press
 * (pressed 1) is the UTF-8 of the character the key types, where the
 * backend knows it (a backend that does not leaves it empty) (KL_VERSION 105).
 */
struct kl_window_event {
	unsigned kind;
	double x;
	double y;
	uint32_t code;
	int pressed;
	int repeated;
	unsigned modifiers;
	double dx;
	double dy;
	int32_t id;
	uint64_t time_us;
	uint64_t arrival_us;
	uint32_t serial;
	char text[KL_WINDOW_TEXT_MAX];
	int32_t begin;
	int32_t end;
	uint32_t before;
	uint32_t after;
	unsigned axis_source;
	unsigned tool;
	unsigned buttons;
	double pressure;
	double tilt_x;
	double tilt_y;
	void *data;
};

struct kl_window *kl_window_open(const struct kl_window_options *options);
void kl_window_close(struct kl_window *window);
int kl_window_dispatch(struct kl_window *window, int timeout_ms);
int kl_window_take(struct kl_window *window, struct kl_window_event *event);
/*
 * KL_VERSION 97: posts an input of the program's own (KL_WINDOW_POST with
 * code and data) to a window.  For a window of an application it may be
 * called from any thread and arrives as the application's KL_APP_WINDOW
 * event: the application's thread queues it at once among the window's
 * inputs, another thread's post wakes kl_app_dispatch and is queued there.
 * A post that finds the window closed arrives as KL_APP_POST (window NULL)
 * with the same code and data.  For a window of its own (kl_window_open)
 * only the window's thread calls it.  Returns 0, EINVAL, or ENOMEM (ENOSPC
 * for a window of its own whose queue is full).
 */
int kl_window_post(struct kl_window *window, uint32_t code, void *data);
int kl_window_repeat(struct kl_window *window, uint64_t now_us);
int kl_window_repeat_wait(const struct kl_window *window, uint64_t now_us);
void kl_window_set_title(struct kl_window *window, const char *title);
void kl_window_size(const struct kl_window *window, uint32_t *width, uint32_t *height);
int kl_window_present_resize(struct kl_window *window, uint32_t *width, uint32_t *height);
int kl_window_present(struct kl_window *window, const uint32_t *pixels, size_t stride);
int kl_window_present_part(struct kl_window *window, const uint32_t *pixels, size_t stride, const struct kl_rect *part);
/* KL_VERSION 90: a frame shown by up to KL_FRAME_PARTS_MAX changed parts (count 0: the whole frame). */
int kl_window_present_parts(struct kl_window *window, const uint32_t *pixels, size_t stride, const struct kl_rect *parts, unsigned count);
/*
 * KL_VERSION 91: a window's pacing.  KL_PACING_MOTION: a pointer's motion
 * queued right after another of the window takes its place (the input
 * keeps the pointer's last place; a button, a key or a leave between them
 * keeps both).  Not for a window that draws strokes from every motion.
 * The default is none: every motion is queued.  max_fps must be 0.
 */
#define KL_PACING_MOTION	1U
/*
 * KL_VERSION 92: KL_PACING_FRAME: a frame is drawn only once the
 * compositor took the last one shown (its frame callback), so that what
 * came meanwhile is drawn as one frame in its latest state rather than
 * frames queued one after another.  A button, a key, the focus, a resize
 * and every input but the moves and the wheel are drawn at once, once; a
 * compositor that does not answer for a second is drawn for anyway, once.
 * ENOTSUP for a window of KL_PRESENT_NONE.  kl_window_frame_wait tells
 * when the next frame may be drawn, changing nothing: 0 now (always
 * without KL_PACING_FRAME), a number of milliseconds to wait at most, or
 * -1 to wait for the compositor alone (its answer wakes the wait).
 */
#define KL_PACING_FRAME	2U
int kl_window_set_pacing(struct kl_window *window, unsigned flags, unsigned max_fps);
int kl_window_frame_wait(const struct kl_window *window, uint64_t now_us);

/*
 * KL_VERSION 109: a new size shown by another thread of the program.  By
 * default the window acknowledges the compositor's configure as it
 * arrives, so a frame of the old size committed after it would claim the
 * new one.  A window that defers them (kl_window_set_configure_deferred)
 * leaves a configure that changes the size (or the maximized state)
 * waiting: its KL_WINDOW_RESIZE comes as before, kl_window_configure_take
 * gives its serial and size, and kl_window_configure_ack acknowledges it
 * just before the first frame of that size is committed.  The thread that
 * draws asks the window's thread for the acknowledgement (kl_window_post)
 * and presents once it is done; the requests then reach the compositor in
 * that order.  A configure that changes no size while none waits, and the
 * window's first, are acknowledged at once; one that changes no size
 * while another waits is acknowledged with it.  A size that changed again
 * after the take waits for its own take and acknowledgement.  Turning the
 * deferral off acknowledges what waits.
 *
 * The three are called on the window's thread (the application's thread
 * for a window of an application).  set: 0, EINVAL, or ENOTSUP for the
 * desktop's surface.  take: 0 with the configure, EAGAIN when none waits
 * (always on a system without configures, Windows), or EINVAL.  ack: 0,
 * EALREADY when that configure was acknowledged already, or EINVAL for a
 * serial other than the last take's.
 */
struct kl_window_configure {
	uint32_t serial;
	uint32_t width;
	uint32_t height;
};
int kl_window_set_configure_deferred(struct kl_window *window, int deferred);
int kl_window_configure_take(struct kl_window *window, struct kl_window_configure *configure);
int kl_window_configure_ack(struct kl_window *window, uint32_t serial);

/*
 * KL_VERSION 109: the size while the program cannot take its inputs.  On
 * Windows the system's modal loop of a resize (a drag of the frame's edge,
 * WM_ENTERSIZEMOVE to WM_EXITSIZEMOVE) keeps the window's thread inside
 * the system, and the KL_WINDOW_RESIZE comes only at its end.  A window
 * with a callback hears every size the content takes meanwhile, on the
 * window's thread inside the loop, and kl_window_size reports it; the
 * KL_WINDOW_RESIZE at the end still comes.  The callback only hands the
 * size on (to the thread that draws) and calls no other kl_window_*
 * function.  A NULL callback stops it.  On Wayland the inputs are taken
 * throughout a resize and the callback is never called.
 */
typedef void (*kl_window_resize_fn)(void *data, uint32_t width, uint32_t height);
void kl_window_on_resize(struct kl_window *window, kl_window_resize_fn callback, void *data);
int kl_window_see_through(const struct kl_window *window);
struct wl_display *kl_window_display(const struct kl_window *window);
struct wl_surface *kl_window_surface(const struct kl_window *window);
struct xdg_toplevel *kl_window_toplevel(const struct kl_window *window);
uint32_t kl_window_serial(const struct kl_window *window);
uint32_t kl_window_press_serial(const struct kl_window *window);
void kl_window_set_serial(struct kl_window *window, uint32_t serial);
struct wl_seat *kl_window_seat(const struct kl_window *window);
void kl_window_copy(struct kl_window *window, const char *text, size_t length);
size_t kl_window_paste(struct kl_window *window, char *text, size_t size);
int kl_window_can_paste(const struct kl_window *window);
void kl_window_text_input(struct kl_window *window, int enabled);
void kl_window_text_cursor(struct kl_window *window, int x, int y, int width, int height);

/*
 * KL_VERSION 68: what an input method reads around the caret while the window
 * asks for text: the field's text (UTF-8, NULL when the application does not
 * tell it; a longer one than KL_TEXT_SURROUNDING_MAX bytes less one is cut
 * around the caret), the caret's and the selection's other end's byte offsets
 * in it, and the field's hints (KL_TEXT_HINT_*) and purpose
 * (KL_TEXT_PURPOSE_*), text-input-v3's numbers.  Sent when it changes, and
 * each time the text input is enabled.
 */
#define KL_TEXT_SURROUNDING_MAX		4000U
#define KL_TEXT_PURPOSE_NORMAL		0U
#define KL_TEXT_PURPOSE_DIGITS		2U
#define KL_TEXT_PURPOSE_NUMBER		3U
#define KL_TEXT_PURPOSE_PHONE		4U
#define KL_TEXT_PURPOSE_URL		5U
#define KL_TEXT_PURPOSE_EMAIL		6U
#define KL_TEXT_HINT_MULTILINE		0x200U
void kl_window_text_context(struct kl_window *window, const char *text, size_t cursor, size_t anchor, unsigned hint, unsigned purpose);
void kl_window_select(struct kl_window *window, const char *text, size_t length);

/*
 * The on-screen keyboard's inset.  The compositor tells a window how much of
 * it the keyboard covers, in the window's pixels from its right edge (the
 * flick panel's column) and from its bottom edge (the QWERTY row), when the
 * keyboard opens, closes or changes the window (before that configure); both
 * are 0 when it has closed or does not cover the window.  With a compositor
 * that does not tell, nothing happens.
 *
 * By default the caret is kept in sight: when the keyboard comes or changes,
 * the next kl_ui_end moves the scroll of the frame's text view
 * (kl_ui_text_region; the one with the keyboard's focus, else the last
 * recorded) so that the caret's line is in the middle of the part of the view
 * the keyboard leaves, as far as the scroll goes (not past the text's start
 * or end; a view that does not scroll stays).  The application may hear the
 * inset first: its callback returns 1 when it took care of it itself (the
 * default is skipped), 0 to keep the default.  The reasons are
 * KL_KEYBOARD_INSET_* of <keiland/keiland_keyboard_inset.h>.
 */
typedef int (*kl_window_keyboard_inset_fn)(void *data, int right, int bottom, unsigned reason);
void kl_window_on_keyboard_inset(struct kl_window *window, kl_window_keyboard_inset_fn callback, void *data);
void kl_window_keyboard_inset(const struct kl_window *window, int *right, int *bottom);

/*
 * The editing operations the on-screen keyboard's buttons ask for.  A window
 * tells the compositor it carries all of them out and its state, and hears
 * them.  By default each becomes the keys it stands for, queued as the
 * window's own key inputs (copy Ctrl+C, cut Ctrl+X, paste Ctrl+V, undo
 * Ctrl+Z, redo Ctrl+Shift+Z, select all Ctrl+A), and select_begin and
 * select_end start and end a selection: while it is made, the keys that move
 * the caret (the arrows, Home, End, Page Up and Page Down) come with Shift,
 * and a copy or a cut ends it.  An application that knows its state tells it
 * (kl_window_edit_state: KL_EDIT_HAS_SELECTION ...); otherwise a selection,
 * undo and redo are taken to be there and paste follows the clipboard.  The
 * application may hear an operation first: its callback returns 1 when it
 * carried it out itself (the default is skipped).  The operations and the
 * state's bits are KL_EDIT_* of <keiland/keiland_edit.h>.
 */
typedef int (*kl_window_edit_fn)(void *data, unsigned operation);
void kl_window_on_edit(struct kl_window *window, kl_window_edit_fn callback, void *data);
void kl_window_edit_state(struct kl_window *window, unsigned state);
int kl_window_selecting(const struct kl_window *window);
size_t kl_window_paste_primary(struct kl_window *window, char *text, size_t size);

/*
 * The full screen.  An application asks the compositor for it (or out of it),
 * and learns from the configure whether the window is fullscreen.
 */
void kl_window_set_fullscreen(struct kl_window *window, int fullscreen);
int kl_window_fullscreen(const struct kl_window *window);

/*
 * KL_VERSION 42: what the window shows, told to the compositor
 * (wp_content_type_v1) from the window's next frame: nothing in particular, a
 * photo, a video or a game.  A fullscreen video or game may then be shown
 * without composing (the compositor's game mode).  Without the compositor's
 * protocol nothing is told (ENOTSUP).
 */
#define KL_CONTENT_NONE		0U
#define KL_CONTENT_PHOTO	1U
#define KL_CONTENT_VIDEO	2U
#define KL_CONTENT_GAME		3U
int kl_window_set_content_type(struct kl_window *window, unsigned type);

/*
 * KL_VERSION 95: tells the compositor, from the window's next frame, that the
 * window's content is opaque everywhere (its opaque region,
 * wl_surface.set_opaque_region), or no longer.  The compositor then need not
 * draw what is under it.  A window with see-through parts (glass panels) must
 * not say so.  Returns 0, or EINVAL without a window.
 */
int kl_window_set_opaque(struct kl_window *window, int opaque);

/*
 * An application that waits for other descriptors too (a terminal's shells)
 * waits for them with the compositor, at most KL_WINDOW_FDS_MAX of them;
 * ready[i] says fds[i] has something to read or has hung up.
 */
#define KL_WINDOW_FDS_MAX	16U
int kl_window_dispatch_fds(struct kl_window *window, const int *fds, unsigned count, int timeout_ms, int *ready);
uint64_t kl_clock_us(void);

/*
 * The declarative menus, controls and glass of a window (KL_VERSION 26).
 * The application gives each as a table, as often as it likes; the
 * library compares it with the one shown and sends only what changed.
 * An item or a control chosen is queued among the window's inputs as a
 * KL_WINDOW_ACTION input: its action in code, the item's or control's ID
 * in id, and a breadcrumb's part in begin (0 otherwise).  An action's
 * state applies to every item and control of that action, in the menu,
 * the controls and later popups.  Without the compositor's System Menu,
 * Titlebar Presentation or glass, the calls return ENOTSUP and nothing is
 * shown; the window works as before.  Each returns 0 or an errno value.
 */
#define KL_WINDOW_ACTION	17U

/* An action's state (bits; 0 is enabled, unchecked and shown). */
#define KL_ACTION_DISABLED	1U
#define KL_ACTION_CHECKED	2U
#define KL_ACTION_HIDDEN	4U

/*
 * One menu item: its ID and its parent's (KL_MENU_ROOT for a top-level
 * item), its type (KL_MENU_ITEM_*), label, action, role (KL_MENU_ROLE_*)
 * and shortcut (KL_MENU_* modifiers and an XKB keysym, 0 for none) -- the
 * fields of libkeiland's menu, in one row.
 */
struct kl_menu_entry {
	uint32_t id;
	uint32_t parent;
	unsigned type;
	const char *label;
	uint32_t action;
	unsigned role;
	unsigned modifiers;
	uint32_t keysym;
};

/*
 * One titlebar control: its ID, role (KL_CONTROL_*), priority
 * (KL_PRIORITY_*), group, label, and the action its choice queues.
 */
struct kl_control_entry {
	uint32_t id;
	unsigned role;
	unsigned priority;
	unsigned group;
	const char *label;
	uint32_t action;
};

/* The window's menu in the compositor's System Menu (count 0 takes it away). */
int kl_window_set_menu(struct kl_window *window, const struct kl_menu_entry *entries, size_t count);

/* The window's titlebar controls (count 0 gives the titlebar back to the menu). */
int kl_window_set_controls(struct kl_window *window, const struct kl_control_entry *entries, size_t count);

/* The state of an action (KL_ACTION_* bits) in the window's menu, controls and popups. */
int kl_window_set_action_state(struct kl_window *window, uint32_t action, unsigned state);

/* A context menu of top-level items at (x, y) of the window, for its last press. */
int kl_window_popup_menu(struct kl_window *window, const struct kl_menu_entry *entries, size_t count, int x, int y);

/* The window's glass panels, from its next frame (count 0 takes them away). */
int kl_window_set_glass(struct kl_window *window, const struct kl_glass_panel *panels, size_t count);

/*
 * KL_VERSION 43: the text of a control that takes text (a KL_CONTROL_SEARCH
 * field).  As it is typed, a KL_WINDOW_CONTROL_TEXT input brings it (the
 * control's ID in id, the text in text); when its editing ends, a
 * KL_WINDOW_CONTROL_DONE input brings the text and how it ended in code
 * (KL_TEXT_SUBMITTED and the others).  A choice of a menu's item or a control
 * also stands as the window's last input for the clipboard (its serial).
 */
#define KL_WINDOW_CONTROL_TEXT	19U
#define KL_WINDOW_CONTROL_DONE	20U

/* Sets a control's text and its placeholder (either may be NULL to leave it), after kl_window_set_controls. */
int kl_window_set_control_text(struct kl_window *window, uint32_t id, const char *text, const char *placeholder);

/* Gives a control's field the keyboard (the find field when Find is chosen). */
int kl_window_focus_control(struct kl_window *window, uint32_t id);

/*
 * KL_VERSION 44: the tabs of a window's titlebar, the selections' changes,
 * drag and drop.
 *
 * Tabs: the titlebar shows the table given (count 0 takes them away, and
 * the titlebar shows the controls again, or the menu): each tab's ID (not
 * 0), title and KL_TAB_* flags of <keiland/keiland.h>, with KL_TABS_* options.  A
 * tab chosen, its close button or the new tab's button comes as a
 * KL_WINDOW_TAB input: KL_WINDOW_TAB_* in code, the tab's ID in id (0 for
 * a new one).
 *
 * The selections: a KL_WINDOW_SELECTION input says the clipboard or the
 * primary selection changed (KL_SELECTION_* in code), and whether it has
 * text (pressed).  kl_window_selection_own tells whether the window's own
 * text is the selection (a paste then takes it directly).
 *
 * Drops: a window takes the drags of the types it accepts
 * (KL_DROP_TEXT, KL_DROP_URIS: a "text/uri-list", which comes as it is),
 * as a copy.  A drag over it comes as KL_WINDOW_DROP_ENTER (the types it
 * has that the window takes in code, where it is in x and y) and
 * KL_WINDOW_DROP_LEAVE; dropped, as KL_WINDOW_DROP (the type it is read
 * as in code: the file names when it has them), and the window then takes
 * it with kl_window_take_drop (which finishes the drop).  A drop of the
 * window's own drag is taken directly.
 *
 * A drag of text out of the window starts with kl_window_drag_text from a
 * press (its serial); its end comes as KL_WINDOW_DRAG_DONE (code 1 when it
 * was dropped, 0 when not).
 */
#define KL_WINDOW_SELECTION	21U
#define KL_WINDOW_DROP_ENTER	22U
#define KL_WINDOW_DROP_LEAVE	23U
#define KL_WINDOW_DROP		24U
#define KL_WINDOW_DRAG_DONE	25U
#define KL_WINDOW_TAB		26U

/* Which selection changed. */
#define KL_SELECTION_CLIPBOARD	1U
#define KL_SELECTION_PRIMARY	2U

/* The types a drop is taken as (bits; KL_DROP_IMAGE, "image/png", KL_VERSION 70). */
#define KL_DROP_TEXT		1U
#define KL_DROP_URIS		2U
#define KL_DROP_IMAGE		4U

/* What a tab's input asks. */
#define KL_WINDOW_TAB_CHOSEN	1U
#define KL_WINDOW_TAB_CLOSE	2U
#define KL_WINDOW_TAB_NEW	3U

/* One tab: its ID (not 0), its title and its KL_TAB_* flags. */
struct kl_tab_entry {
	uint32_t id;
	const char *title;
	unsigned flags;
};

/*
 * A pen tablet: a window whose application takes it
 * (kl_window_accept_tablet) hears the pen as KL_WINDOW_TABLET_* inputs
 * with its pressure and tilt (a contact's start, moves and end, a move
 * over the window without touching it, and leaving it); any other window
 * hears the pen as the pointer.  And a held key's repeat may be turned off
 * for a window (kl_window_set_repeat).
 */
#define KL_WINDOW_TABLET_DOWN	27U
#define KL_WINDOW_TABLET_MOTION	28U
#define KL_WINDOW_TABLET_UP	29U
#define KL_WINDOW_TABLET_HOVER	30U
#define KL_WINDOW_TABLET_LEAVE	31U

/* A tablet's tool, and its barrel buttons (bits). */
#define KL_TABLET_PEN		0U
#define KL_TABLET_ERASER	1U
#define KL_TABLET_BUTTON_STYLUS	1U
#define KL_TABLET_BUTTON_STYLUS2	2U

int kl_window_accept_tablet(struct kl_window *window);

/*
 * KL_VERSION 45: whether the window is maximized, maximizing or bringing it
 * back and minimizing it, the first screen's current mode (its refresh in
 * millihertz; ENOENT while unknown), the Vulkan device that shows the frames
 * and the last frame's times, a breadcrumb control's parts (one chosen comes
 * as the control's KL_WINDOW_ACTION with the part in begin), and the glass
 * blurring what is under the window.
 */
struct kl_present_times {
	unsigned copy_ms;
	unsigned acquire_ms;
	unsigned present_ms;
	unsigned wait_ms;
};

int kl_window_maximized(const struct kl_window *window);
void kl_window_set_maximized(struct kl_window *window, int maximized);
void kl_window_minimize(struct kl_window *window);
int kl_window_output_mode(const struct kl_window *window, int32_t *width, int32_t *height, int32_t *refresh);
const char *kl_window_device_name(const struct kl_window *window);
void kl_window_present_times(const struct kl_window *window, struct kl_present_times *times);
int kl_window_set_control_parts(struct kl_window *window, uint32_t id, const char *const *parts, size_t count);
int kl_window_set_glass_blur(struct kl_window *window, int enabled);

/*
 * KL_VERSION 46: the drag over a window as it moves (KL_WINDOW_DROP_MOTION)
 * and the compositor's choice of action for it (KL_WINDOW_DROP_ACTION, in
 * code); the application answers which actions it takes and the one it
 * prefers (kl_window_answer_drop; until it answers, a drag of a type it
 * accepts is taken as a copy), reads what was dropped
 * (kl_window_receive_drop, in memory the caller frees) and finishes the drop
 * with the action carried out, or gives it up with 0 (kl_window_finish_drop).
 * A drag out of the window offers the data of a few types with the actions it
 * allows (kl_window_start_drag); its KL_WINDOW_DRAG_DONE has the compositor's
 * choice of action in begin.  A drag over a control of the titlebar comes as
 * KL_WINDOW_CONTROL_DROP (the control's ID in id, the part in begin, id 0
 * when it left the controls).  KL_DROP_ENTER's pressed says the drag is the
 * window's own.
 */
#define KL_WINDOW_DROP_MOTION	32U
#define KL_WINDOW_DROP_ACTION	33U
#define KL_WINDOW_CONTROL_DROP	34U

/* The context menu (kl_window_popup_menu) closed, chosen from or not (KL_VERSION 46). */
#define KL_WINDOW_POPUP_DONE	35U

/* The actions of drag and drop (bits, as wl_data_device_manager's). */
#define KL_DND_COPY		1U
#define KL_DND_MOVE		2U
#define KL_DND_ASK		4U

/* One type a drag offers, and its data. */
struct kl_drag_data {
	const char *type;
	const void *data;
	size_t length;
};

void kl_window_answer_drop(struct kl_window *window, unsigned actions, unsigned preferred);
int kl_window_receive_drop(struct kl_window *window, char **data, size_t *length, unsigned *type);
void kl_window_finish_drop(struct kl_window *window, unsigned action);
int kl_window_start_drag(struct kl_window *window, const struct kl_drag_data *data, size_t count, unsigned actions, uint32_t serial);

/* A control's value (a progress's share, a breadcrumb's KL_BREADCRUMB_BOXED), a text control's suggestions, and the keyboard given to a control as KL_FOCUS_FIELD or _EDIT (KL_VERSION 46). */
int kl_window_set_control_value(struct kl_window *window, uint32_t id, unsigned value);
int kl_window_set_control_suggestions(struct kl_window *window, uint32_t id, const char *const *labels, const char *const *texts, size_t count);
int kl_window_focus_control_mode(struct kl_window *window, uint32_t id, unsigned mode);

/* Where the desktop's surface is on the screen (KL_VERSION 46). */
int kl_window_desktop_place(const struct kl_window *window, int32_t *x, int32_t *y);

/*
 * KL_VERSION 46: a window whose keys mean something else (a terminal's
 * Ctrl+C) takes the keyboard's editing buttons as the keys the compositor
 * chooses for it (a terminal's Ctrl+Shift+C and V), not as operations:
 * its edit object goes, and kl_window_on_edit's callback hears nothing.
 */
void kl_window_edit_by_keys(struct kl_window *window);
int kl_window_set_repeat(struct kl_window *window, int enabled);
int kl_window_set_tabs(struct kl_window *window, const struct kl_tab_entry *tabs, size_t count, unsigned options);
int kl_window_selection_own(const struct kl_window *window, unsigned which);
int kl_window_accept_drops(struct kl_window *window, unsigned types);
size_t kl_window_take_drop(struct kl_window *window, char *text, size_t size, unsigned *type);
int kl_window_drag_text(struct kl_window *window, const char *text, size_t length, uint32_t serial);

/*
 * KL_VERSION 70: drag and drop of pictures between applications.
 *
 * A window that accepts KL_DROP_IMAGE takes a drag of "image/png"; a drag
 * with several of the types a window accepts is read as its file names
 * first, then its picture, then its text.  A drop is read up to 1 MiB of
 * file names, 64 MiB of a picture and 16 MiB of text (E2BIG past it).
 *
 * kl_window_answer_drop answers for the place the drag is over: an
 * application may answer at each KL_WINDOW_DROP_ENTER and _MOTION (only a
 * changed answer is sent), and actions 0 says the drop is not taken there,
 * which the compositor shows on the drag.
 *
 * A drag out of the window may carry a picture under the pointer
 * (kl_window_start_drag_icon): premultiplied pixels, shown at most
 * KL_DRAG_ICON_MAX pixels on its longer side and a little see-through,
 * with the point hot_x, hot_y of them under the pointer.  Without shared
 * memory the drag goes without its picture.  A type whose data must be
 * made (a picture encoded) may start with NULL data and be filled with
 * kl_window_drag_fill before the program dispatches again: the drag
 * starts while the button is held, and its data is asked for only after
 * a drop.  A drop of a drag of another window of the same program is read
 * directly, as the window's own is.
 *
 * kl_ui_pointer_cancel forgets the press a widget holds when the press
 * became a drag (the compositor keeps its release).
 *
 * A window lights where a drop would land in the same way as every other
 * application: kl_drop_frame around a place (a picture's frame, a file's
 * cell), kl_drop_caret at the point where text would go, both in the
 * theme's accent.  An application that draws its own pixels uses the same
 * sizes (KL_DROP_RING, KL_DROP_CARET) and the fill's alpha
 * (KL_DROP_FILL_ALPHA of 255).
 */
#define KL_DRAG_ICON_MAX	160
#define KL_DROP_RING		2
#define KL_DROP_CARET		2
#define KL_DROP_FILL_ALPHA	31U

/* A drag's picture: premultiplied 0xAARRGGBB pixels without padding, and the point of them under the pointer. */
struct kl_drag_icon {
	const uint32_t *pixels;
	int width;
	int height;
	int hot_x;
	int hot_y;
};

int kl_window_start_drag_icon(struct kl_window *window, const struct kl_drag_data *data, size_t count, unsigned actions, uint32_t serial, const struct kl_drag_icon *icon);
int kl_window_drag_fill(struct kl_window *window, const char *type, const void *data, size_t length);
void kl_drop_frame(struct kl_canvas *canvas, const struct kl_theme *theme, float x, float y, float width, float height, float radius);
void kl_drop_caret(struct kl_canvas *canvas, const struct kl_theme *theme, float x, float y, float height);

/*
 * A Vulkan surface over a window shown with KL_PRESENT_NONE, for an
 * application drawing with its own Vulkan instance (which enabled the
 * extensions kl_window_vulkan_extensions names).  Declared for a program
 * that included the Vulkan header first.  Returns 0, EINVAL, or EIO when
 * Vulkan refused.
 */
#if defined(VK_VERSION_1_0)
int kl_window_vulkan_surface(struct kl_window *window, VkInstance instance, VkSurfaceKHR *surface);
#endif

/*
 * The instance extensions a Vulkan instance needs for kl_window_vulkan_surface
 * on this system (KL_VERSION 105): VK_KHR_surface and the system's surface
 * extension.  names receives a static array of count names.  Returns 0,
 * EINVAL for NULL arguments, or EIO when the system has no Vulkan loader
 * (Windows without vulkan-1.dll).
 */
int kl_window_vulkan_extensions(const char *const **names, uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif
