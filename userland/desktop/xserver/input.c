/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard, the pointer, the focus and the grabs.
 *
 * The backend reports keys by their place on the keyboard (an evdev code
 * plus 8), the pointer's place and its physical buttons.  Each report is
 * held in a ring and handled in order unless a grab froze its device:
 * a key goes to the client grabbing the keyboard, else to a passive key
 * grab it starts, else from the focus window; a button to the client
 * grabbing the pointer, else to a passive button grab it starts, else up
 * the tree from the window under the pointer, where it starts the
 * implicit grab.  The pointer's moves between windows, and the windows'
 * changes under it, make the crossing events, and each change of the
 * focus makes the focus events, with the details the core protocol lays
 * down.
 *
 * The keyboard's map (the keysyms of each keycode and the modifiers' keys)
 * is the backend's or the US layout's; the modifier bits of an event's
 * state are made of the keys held and the locking keys' state.  Requests:
 * the grabs (GrabPointer to AllowEvents), QueryPointer, GetMotionEvents,
 * WarpPointer, SetInputFocus, GetInputFocus, QueryKeymap, and the
 * keyboard's and the pointer's maps and controls (ChangeKeyboardMapping
 * to GetModifierMapping).  XKEYBOARD is not offered: a client reads the
 * core protocol's map.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The requests answered here. */
#define REQUEST_GRAB_POINTER			26U
#define REQUEST_UNGRAB_POINTER			27U
#define REQUEST_GRAB_BUTTON			28U
#define REQUEST_UNGRAB_BUTTON			29U
#define REQUEST_CHANGE_ACTIVE_POINTER_GRAB	30U
#define REQUEST_GRAB_KEYBOARD			31U
#define REQUEST_UNGRAB_KEYBOARD			32U
#define REQUEST_GRAB_KEY			33U
#define REQUEST_UNGRAB_KEY			34U
#define REQUEST_ALLOW_EVENTS			35U
#define REQUEST_QUERY_POINTER			38U
#define REQUEST_GET_MOTION_EVENTS		39U
#define REQUEST_WARP_POINTER			41U
#define REQUEST_SET_INPUT_FOCUS			42U
#define REQUEST_GET_INPUT_FOCUS			43U
#define REQUEST_QUERY_KEYMAP			44U
#define REQUEST_CHANGE_KEYBOARD_MAPPING		100U
#define REQUEST_GET_KEYBOARD_MAPPING		101U
#define REQUEST_CHANGE_KEYBOARD_CONTROL		102U
#define REQUEST_GET_KEYBOARD_CONTROL		103U
#define REQUEST_BELL				104U
#define REQUEST_CHANGE_POINTER_CONTROL		105U
#define REQUEST_GET_POINTER_CONTROL		106U
#define REQUEST_SET_POINTER_MAPPING		116U
#define REQUEST_GET_POINTER_MAPPING		117U
#define REQUEST_SET_MODIFIER_MAPPING		118U
#define REQUEST_GET_MODIFIER_MAPPING		119U

/* A grab's answer: done, another client holds it, a time out of order, the window not viewable, the device frozen. */
#define GRAB_SUCCESS		0U
#define GRAB_ALREADY		1U
#define GRAB_INVALID_TIME	2U
#define GRAB_NOT_VIEWABLE	3U
#define GRAB_FROZEN		4U

/* A device's mode while a grab lasts: frozen until AllowEvents, or as usual. */
#define MODE_SYNC		0U
#define MODE_ASYNC		1U

/* A passive grab of any modifiers, of any button or key. */
#define ANY_MODIFIER		0x8000U
#define ANY_DETAIL		0U

/* The modifier bits of a state, and the bits a pointer grab's event mask may have. */
#define MODIFIER_BITS		0x00ffU
#define POINTER_EVENT_BITS	0x7ffcU

/* The bits of a state that are buttons 1 to 5 (the other buttons have none). */
#define STATE_BUTTON_SHIFT	8U
#define STATE_BUTTONS		0x1fU

/* The details of the crossing and focus events. */
#define DETAIL_ANCESTOR		0U
#define DETAIL_VIRTUAL		1U
#define DETAIL_INFERIOR		2U
#define DETAIL_NONLINEAR	3U
#define DETAIL_NONLINEAR_VIRTUAL 4U
#define DETAIL_POINTER		5U
#define DETAIL_POINTER_ROOT	6U
#define DETAIL_NONE		7U

/* The modes of the crossing and focus events. */
#define NOTIFY_NORMAL		0U
#define NOTIFY_GRAB		1U
#define NOTIFY_UNGRAB		2U
#define NOTIFY_WHILE_GRABBED	3U

/* The flags of a crossing event: the window has the focus (or is in it), and the pointer is on this screen. */
#define CROSSING_FOCUS		0x01U
#define CROSSING_SAME_SCREEN	0x02U

/* Where the focus goes when its window is no longer viewable. */
#define REVERT_NONE		0U
#define REVERT_POINTER_ROOT	1U
#define REVERT_PARENT		2U

/* AllowEvents' modes. */
#define ALLOW_ASYNC_POINTER	0U
#define ALLOW_SYNC_POINTER	1U
#define ALLOW_REPLAY_POINTER	2U
#define ALLOW_ASYNC_KEYBOARD	3U
#define ALLOW_SYNC_KEYBOARD	4U
#define ALLOW_REPLAY_KEYBOARD	5U
#define ALLOW_SYNC_BOTH		6U
#define ALLOW_ASYNC_BOTH	7U

/* MappingNotify's requests, and the answers of SetModifierMapping and SetPointerMapping. */
#define MAPPING_MODIFIER	0U
#define MAPPING_KEYBOARD	1U
#define MAPPING_POINTER		2U
#define MAPPING_SUCCESS		0U
#define MAPPING_BUSY		1U
#define MAPPING_FAILED		2U

/* The keysyms of the locking keys. */
#define KEYSYM_NUM_LOCK		0xff7fU
#define KEYSYM_CAPS_LOCK	0xffe5U
#define KEYSYM_SHIFT_LOCK	0xffe6U

/* The keycodes the keyboard has. */
#define KEYCODE_MIN		8U
#define KEYCODE_MAX		255U

/* ChangeKeyboardControl's values, and its auto-repeat modes. */
#define CONTROL_KEY_CLICK	0U
#define CONTROL_BELL_PERCENT	1U
#define CONTROL_BELL_PITCH	2U
#define CONTROL_BELL_DURATION	3U
#define CONTROL_LED		4U
#define CONTROL_LED_MODE	5U
#define CONTROL_KEY		6U
#define CONTROL_AUTO_REPEAT	7U
#define CONTROL_COUNT		8U
#define REPEAT_OFF		0U
#define REPEAT_ON		1U
#define REPEAT_DEFAULT		2U

/* The keyboard's and the pointer's controls before a client changes them. */
#define DEFAULT_KEY_CLICK	0
#define DEFAULT_BELL_PERCENT	50
#define DEFAULT_BELL_PITCH	400
#define DEFAULT_BELL_DURATION	100
#define DEFAULT_ACCELERATION	2
#define DEFAULT_THRESHOLD	4

/* The LEDs the keyboard has (1 to 32). */
#define LED_COUNT		32U

/* WM_HINTS (a predefined atom), its flag that the input field is set, and that field's index. */
#define ATOM_WM_HINTS		35U
#define WM_HINTS_INPUT		0x1U

static void input_queue(struct x11server *server, unsigned kind, uint8_t detail, int pressed, uint32_t time, int x, int y);
static void input_drain(struct x11server *server);
static void input_handle(struct x11server *server, const struct x11_device_event *event, const struct x11_window *ignore);
static void input_handle_key(struct x11server *server, const struct x11_device_event *event, const struct x11_window *ignore);
static void input_handle_button(struct x11server *server, const struct x11_device_event *event, const struct x11_window *ignore);
static void input_handle_motion(struct x11server *server, const struct x11_device_event *event);
static void input_pointer_moves(struct x11server *server, int x, int y, uint32_t time);
static void input_pointer_check(struct x11server *server, uint32_t time);
static int input_key_normal(struct x11server *server, uint8_t type, uint32_t mask, uint8_t keycode, uint32_t time, uint16_t state, unsigned only);
static void input_key_to_grab(struct x11server *server, uint8_t type, uint32_t mask, uint8_t keycode, uint32_t time, uint16_t state);
static int input_pointer_to_grab(struct x11server *server, uint8_t type, uint32_t mask, uint8_t detail, uint32_t time, uint16_t state);
static void input_reported(struct x11server *server, int keyboard, const struct x11_device_event *event);
static void input_pointer_grab_start(struct x11server *server, const struct x11_grab *grab);
static void input_pointer_grab_end(struct x11server *server, uint32_t time);
static void input_keyboard_grab_start(struct x11server *server, const struct x11_grab *grab);
static void input_keyboard_grab_end(struct x11server *server);
static void input_grab_from_passive(const struct x11_passive_grab *passive, uint32_t time, struct x11_grab *grab);
static struct x11_passive_grab *input_passive_find(struct x11server *server, const struct x11_window *window, int keyboard, uint8_t detail, uint16_t modifiers);
static struct x11_passive_grab *input_passive_key(struct x11server *server, uint8_t keycode, uint16_t modifiers, const struct x11_window *ignore);
static struct x11_passive_grab *input_passive_button(struct x11server *server, uint8_t button, uint16_t modifiers, const struct x11_window *ignore);
static int input_ignored(const struct x11_window *window, const struct x11_window *ignore);
static void input_passive_remove(struct x11server *server, unsigned index);
static void input_set_focus(struct x11server *server, uint32_t focus, uint8_t revert, uint32_t time);
static void input_revert_focus(struct x11server *server, struct x11_window *lost);
static void input_focus_change(struct x11server *server, uint32_t from, uint32_t to, uint8_t mode);
static void input_focus_up(struct x11server *server, uint8_t type, struct x11_window *from, const struct x11_window *until, uint8_t detail, uint8_t mode);
static void input_focus_down(struct x11server *server, uint8_t type, const struct x11_window *top, struct x11_window *to, uint8_t detail, uint8_t mode);
static void input_focus_event(struct x11server *server, uint8_t type, const struct x11_window *window, uint8_t detail, uint8_t mode);
static void input_crossing(struct x11server *server, struct x11_window *from, struct x11_window *to, uint8_t mode, uint32_t time);
static void input_cross(struct x11server *server, uint8_t type, struct x11_window *window, uint8_t detail, uint8_t mode, uint32_t time, const struct x11_window *pointer);
static void input_keymap_notify(struct x11server *server, struct x11_client *client);
static void input_keymap_notify_selectors(struct x11server *server, const struct x11_window *window, uint32_t mask);
static unsigned input_distance(const struct x11_window *window, const struct x11_window *top);
static struct x11_window *input_ancestor(struct x11_window *window, unsigned steps);
static struct x11_window *input_common(struct x11_window *first, struct x11_window *second);
static int input_strictly_inferior(const struct x11_window *window, const struct x11_window *ancestor);
static struct x11_window *input_focus_window(struct x11server *server);
static int input_key_down(const struct x11server *server, unsigned keycode);
static void input_key_set(struct x11server *server, unsigned keycode, int down);
static int input_locking(const struct x11server *server, unsigned keycode);
static void input_lock_toggle(struct x11server *server, unsigned keycode);
static void input_mapping_notify(struct x11server *server, uint8_t request, uint8_t first, uint8_t count);
static int input_time_valid(const struct x11server *server, uint32_t time, uint32_t since);
static uint32_t input_time(const struct x11server *server, uint32_t time);
static void input_capture(struct x11server *server, const struct x11_grab *grab, int on);
static unsigned input_grab_pointer(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_ungrab_pointer(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_grab_passive(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_ungrab_passive(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_change_active_grab(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_grab_keyboard(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_ungrab_keyboard(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_allow_events(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static void input_replay_pointer(struct x11server *server);
static void input_replay_keyboard(struct x11server *server);
static unsigned input_query_pointer(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_get_motion_events(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_warp_pointer(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_set_input_focus(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_get_input_focus(struct x11server *server, unsigned index, size_t length);
static unsigned input_query_keymap(struct x11server *server, unsigned index, size_t length);
static unsigned input_change_keyboard_mapping(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_get_keyboard_mapping(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_change_keyboard_control(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_get_keyboard_control(struct x11server *server, unsigned index, size_t length);
static unsigned input_bell(struct x11server *server, const uint8_t *request, size_t length);
static unsigned input_change_pointer_control(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_get_pointer_control(struct x11server *server, unsigned index, size_t length);
static unsigned input_set_pointer_mapping(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_get_pointer_mapping(struct x11server *server, unsigned index, size_t length);
static unsigned input_set_modifier_mapping(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned input_get_modifier_mapping(struct x11server *server, unsigned index, size_t length);
static int input_modifiers_busy(const struct x11server *server, const uint8_t *keycodes, unsigned count);
static void input_keymap_apply(struct x11server *server, const struct x11_keymap *keymap);
static int input_accepts_focus(struct x11server *server, struct x11_window *window);

/*
 * Starts the server's input: the US keyboard map, the buttons each
 * itself, the controls' defaults, the focus PointerRoot, the pointer in
 * the root window, and no grab.
 */
void
x11_input_init(
	struct x11server *server)
{
	struct x11_keymap keymap;
	unsigned button;

	/* The US layout, until the backend gives its own. */
	x11_keymap_us(&keymap);
	input_keymap_apply(server, &keymap);

	/* Each button is itself. */
	for (button = 0U; button < X11_BUTTONS; button++)
		server->button_map[button] = (uint8_t)(button + 1U);

	/* The controls' defaults: the pointer's acceleration, the click, the bell, every key repeating. */
	server->acceleration_numerator = DEFAULT_ACCELERATION;
	server->acceleration_denominator = 1;
	server->acceleration_threshold = DEFAULT_THRESHOLD;
	server->key_click_percent = DEFAULT_KEY_CLICK;
	server->bell_percent = DEFAULT_BELL_PERCENT;
	server->bell_pitch = DEFAULT_BELL_PITCH;
	server->bell_duration = DEFAULT_BELL_DURATION;
	server->leds = 0U;
	server->global_auto_repeat = 1U;
	memset(server->auto_repeats, 0xff, sizeof(server->auto_repeats));
	server->auto_repeats[0] = 0U;

	/* The focus follows the pointer, which is in the root window; nothing is grabbed or frozen. */
	server->focus = X11_FOCUS_POINTER_ROOT;
	server->focus_revert = REVERT_NONE;
	server->focus_time = server->now;
	server->pointer_window = X11_ROOT_XID;
	server->pointer_grab.client = X11_NO_CLIENT;
	server->keyboard_grab.client = X11_NO_CLIENT;
	server->pointer_grab_time = server->now;
	server->keyboard_grab_time = server->now;
	server->pointer_freeze = X11_FREEZE_NONE;
	server->keyboard_freeze = X11_FREEZE_NONE;
}

/*
 * Ends the server's input: the passive grabs go.
 */
void
x11_input_fini(
	struct x11server *server)
{
	/* The table of passive grabs. */
	free(server->passive_grabs);
	server->passive_grabs = NULL;
	server->passive_count = 0U;
	server->passive_capacity = 0U;
}

/*
 * Does the input's work of a tick: the device events a thaw let go, and
 * the crossing events of windows that changed under the pointer.
 */
void
x11_input_tick(
	struct x11server *server)
{
	/* The events held, while their devices are not frozen. */
	input_drain(server);

	/* A frozen pointer does not seem to move into another window. */
	if (server->pointer_freeze != X11_FREEZE_NONE)
		return;

	/* The window under the pointer, which a map, an unmap or a move may have changed. */
	input_pointer_check(server, server->now);
}

/*
 * Takes a key the backend reported (its keycode: its evdev code plus 8).
 */
void
x11_input_key(
	struct x11server *server,
	uint8_t keycode,
	int pressed,
	uint32_t time)
{
	/* Held in order, and handled unless the keyboard is frozen. */
	input_queue(server, X11_DEVICE_KEY, keycode, pressed, time, 0, 0);
	input_drain(server);
}

/*
 * Takes a physical button the backend reported (1 to 9), with the place
 * of the pointer on the root window.
 */
void
x11_input_button(
	struct x11server *server,
	uint8_t button,
	int pressed,
	uint32_t time,
	int x,
	int y)
{
	/* Held in order, and handled unless the pointer is frozen. */
	input_queue(server, X11_DEVICE_BUTTON, button, pressed, time, x, y);
	input_drain(server);
}

/*
 * Takes a move of the pointer to a place on the root window.
 */
void
x11_input_motion(
	struct x11server *server,
	uint32_t time,
	int x,
	int y)
{
	/* Held in order, and handled unless the pointer is frozen. */
	input_queue(server, X11_DEVICE_MOTION, 0U, 0, time, x, y);
	input_drain(server);
}

/*
 * Takes the pointer leaving the X windows (onto the system's own): it is
 * in the root window from then on.
 */
void
x11_input_leave(
	struct x11server *server,
	uint32_t time)
{
	/* Held in order, and handled unless the pointer is frozen. */
	input_queue(server, X11_DEVICE_LEAVE, 0U, 0, time, server->pointer_x, server->pointer_y);
	input_drain(server);
}

/*
 * Takes the backend's keyboard map: the keysyms and the modifiers' keys
 * from now on, and MappingNotify to every client when they changed.
 */
void
x11_input_keymap(
	struct x11server *server,
	const struct x11_keymap *keymap)
{
	uint32_t keysyms[X11_KEYMAP_KEYCODES][X11_KEYSYMS_MAX];
	uint8_t modifiers[X11_MODIFIERS][X11_MODIFIER_KEYS_MAX];
	int keysyms_changed;
	int modifiers_changed;

	/* The map before. */
	memcpy(keysyms, server->keysyms, sizeof(keysyms));
	memcpy(modifiers, server->modifier_keys, sizeof(modifiers));

	/* The new map. */
	input_keymap_apply(server, keymap);

	/* The clients hear of each part that changed. */
	keysyms_changed = memcmp(keysyms, server->keysyms, sizeof(keysyms));
	modifiers_changed = memcmp(modifiers, server->modifier_keys, sizeof(modifiers));
	if (modifiers_changed != 0)
		input_mapping_notify(server, MAPPING_MODIFIER, 0U, 0U);
	if (keysyms_changed != 0)
		input_mapping_notify(server, MAPPING_KEYBOARD, KEYCODE_MIN, (uint8_t)(KEYCODE_MAX - KEYCODE_MIN + 1U));
}

/*
 * Gives the focus to a top-level window the user chose (a click, or the
 * system's activation), as its window manager does: not to an
 * override-redirect window, nor to one whose WM_HINTS say it takes no
 * input.
 */
void
x11_input_focus_top_level(
	struct x11server *server,
	struct x11_window *window,
	uint32_t time)
{
	uint32_t when;
	int accepts;

	/* A window that takes no focus. */
	accepts = input_accepts_focus(server, window);
	if (!accepts)
		return;

	/* Nothing changes when it has it. */
	if (server->focus == window->id)
		return;

	/* Succeeded: the focus moves, back to the pointer's window should the window go. */
	when = input_time(server, time);
	input_set_focus(server, window->id, REVERT_POINTER_ROOT, when);
}

/*
 * Hears that the system's keyboard focus left a top-level window: when
 * the X focus is in it, no X window has the focus any more.
 */
void
x11_input_focus_lost(
	struct x11server *server,
	struct x11_window *window,
	uint32_t time)
{
	struct x11_window *focus;
	uint32_t when;
	int inside;

	/* The focus is PointerRoot, None, or in another window. */
	focus = x11_window_find(server, server->focus);
	if (focus == NULL)
		return;
	inside = x11_window_inferior(focus, window);
	if (!inside)
		return;

	/* Succeeded: None. */
	when = input_time(server, time);
	input_set_focus(server, X11_FOCUS_NONE, REVERT_NONE, when);
}

/*
 * Returns the window under the pointer: the deepest viewable window
 * there, or the root window while the pointer is outside every X window.
 */
struct x11_window *
x11_input_window_under(
	struct x11server *server)
{
	struct x11_window *window;

	/* Outside the X windows. */
	if (server->pointer_outside)
		return server->root;

	/* Succeeded: the window at the pointer's place. */
	window = x11_window_at(server, server->pointer_x, server->pointer_y);
	return window;
}

/*
 * Returns the state of a key or button event: the modifiers held (the
 * keys of each modifier down, and the modifiers locking keys hold on) and
 * buttons 1 to 5 held.
 */
uint16_t
x11_input_state(
	const struct x11server *server)
{
	uint16_t state;
	unsigned modifier;
	unsigned slot;
	unsigned keycode;
	int down;
	int locking;

	/* The locked modifiers, and the buttons. */
	state = server->locks;
	state = (uint16_t)(state | ((server->buttons & STATE_BUTTONS) << STATE_BUTTON_SHIFT));

	/* Each modifier one of whose (non-locking) keys is down. */
	for (modifier = 0U; modifier < X11_MODIFIERS; modifier++) {
		for (slot = 0U; slot < server->keys_per_modifier; slot++) {
			keycode = server->modifier_keys[modifier][slot];
			if (keycode == 0U)
				continue;
			down = input_key_down(server, keycode);
			locking = input_locking(server, keycode);
			if (down && !locking)
				state = (uint16_t)(state | (1U << modifier));
		}
	}

	/* Succeeded: the state. */
	return state;
}

/*
 * Hears that a window is no longer viewable (it or an ancestor was
 * unmapped): the focus in it reverts, and a grab on it or confined to it
 * ends.
 */
void
x11_input_window_hidden(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_window *focus;
	struct x11_window *grabbed;
	struct x11_window *confine;
	int inside;
	int viewable;

	/* The focus in the window or one of its inferiors goes where its revert-to says. */
	focus = x11_window_find(server, server->focus);
	if (focus != NULL) {
		inside = x11_window_inferior(focus, window);
		if (inside)
			input_revert_focus(server, focus);
	}

	/* A pointer grab whose window or confining window is not viewable now ends. */
	if (server->pointer_grab.client != X11_NO_CLIENT) {
		grabbed = x11_window_find(server, server->pointer_grab.window);
		viewable = 0;
		if (grabbed != NULL)
			viewable = x11_window_viewable(grabbed);
		if (viewable && server->pointer_grab.confine_to != 0U) {
			confine = x11_window_find(server, server->pointer_grab.confine_to);
			viewable = 0;
			if (confine != NULL)
				viewable = x11_window_viewable(confine);
		}
		if (!viewable)
			input_pointer_grab_end(server, server->now);
	}

	/* A keyboard grab whose window is not viewable now ends. */
	if (server->keyboard_grab.client != X11_NO_CLIENT) {
		grabbed = x11_window_find(server, server->keyboard_grab.window);
		viewable = 0;
		if (grabbed != NULL)
			viewable = x11_window_viewable(grabbed);
		if (!viewable)
			input_keyboard_grab_end(server);
	}

	/* Succeeded: the events a thaw let go are handled. */
	input_drain(server);
}

/*
 * Hears that a window is being destroyed (its inferiors are gone): the
 * focus on it reverts, a grab on it ends, its passive grabs go, and the
 * pointer, when it was there, is in its parent.
 */
void
x11_input_window_destroyed(
	struct x11server *server,
	struct x11_window *window)
{
	unsigned index;

	/* The focus on it goes where its revert-to says. */
	if (server->focus == window->id)
		input_revert_focus(server, window);

	/* A grab on it, or confined to it, ends. */
	if (server->pointer_grab.client != X11_NO_CLIENT) {
		if (server->pointer_grab.window == window->id || server->pointer_grab.confine_to == window->id)
			input_pointer_grab_end(server, server->now);
	}
	if (server->keyboard_grab.client != X11_NO_CLIENT && server->keyboard_grab.window == window->id)
		input_keyboard_grab_end(server);

	/* Its passive grabs go (from the end, each removal moving the last into its place). */
	index = server->passive_count;
	while (index > 0U) {
		index--;
		if (server->passive_grabs[index].window == window->id)
			input_passive_remove(server, index);
	}

	/* The pointer that was in it is in its parent; no crossing event reaches a window going away. */
	if (server->pointer_window == window->id) {
		server->pointer_window = X11_ROOT_XID;
		if (window->parent != NULL)
			server->pointer_window = window->parent->id;
	}

	/* Succeeded: the events a thaw let go are handled. */
	input_drain(server);
}

/*
 * Forgets a client that is going: its grabs end and its passive grabs go.
 */
void
x11_input_forget_client(
	struct x11server *server,
	unsigned client)
{
	unsigned index;

	/* Its active grabs. */
	if (server->pointer_grab.client == client)
		input_pointer_grab_end(server, server->now);
	if (server->keyboard_grab.client == client)
		input_keyboard_grab_end(server);

	/* Its passive grabs (from the end, each removal moving the last into its place). */
	index = server->passive_count;
	while (index > 0U) {
		index--;
		if (server->passive_grabs[index].client == client)
			input_passive_remove(server, index);
	}

	/* Succeeded: the events a thaw let go are handled. */
	input_drain(server);
}

/*
 * Answers an input request: the grabs, the pointer's queries and warp,
 * the focus, and the keyboard's and the pointer's maps and controls.
 * Returns 0, or the error.
 */
unsigned
x11_input_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	unsigned code;

	/* The request's own handler. */
	switch (request[0]) {
	case REQUEST_GRAB_POINTER:
		code = input_grab_pointer(server, index, request, length);
		break;
	case REQUEST_UNGRAB_POINTER:
		code = input_ungrab_pointer(server, index, request, length);
		break;
	case REQUEST_GRAB_BUTTON:
	case REQUEST_GRAB_KEY:
		code = input_grab_passive(server, index, request, length);
		break;
	case REQUEST_UNGRAB_BUTTON:
	case REQUEST_UNGRAB_KEY:
		code = input_ungrab_passive(server, index, request, length);
		break;
	case REQUEST_CHANGE_ACTIVE_POINTER_GRAB:
		code = input_change_active_grab(server, index, request, length);
		break;
	case REQUEST_GRAB_KEYBOARD:
		code = input_grab_keyboard(server, index, request, length);
		break;
	case REQUEST_UNGRAB_KEYBOARD:
		code = input_ungrab_keyboard(server, index, request, length);
		break;
	case REQUEST_ALLOW_EVENTS:
		code = input_allow_events(server, index, request, length);
		break;
	case REQUEST_QUERY_POINTER:
		code = input_query_pointer(server, index, request, length);
		break;
	case REQUEST_GET_MOTION_EVENTS:
		code = input_get_motion_events(server, index, request, length);
		break;
	case REQUEST_WARP_POINTER:
		code = input_warp_pointer(server, index, request, length);
		break;
	case REQUEST_SET_INPUT_FOCUS:
		code = input_set_input_focus(server, index, request, length);
		break;
	case REQUEST_GET_INPUT_FOCUS:
		code = input_get_input_focus(server, index, length);
		break;
	case REQUEST_QUERY_KEYMAP:
		code = input_query_keymap(server, index, length);
		break;
	case REQUEST_CHANGE_KEYBOARD_MAPPING:
		code = input_change_keyboard_mapping(server, index, request, length);
		break;
	case REQUEST_GET_KEYBOARD_MAPPING:
		code = input_get_keyboard_mapping(server, index, request, length);
		break;
	case REQUEST_CHANGE_KEYBOARD_CONTROL:
		code = input_change_keyboard_control(server, index, request, length);
		break;
	case REQUEST_GET_KEYBOARD_CONTROL:
		code = input_get_keyboard_control(server, index, length);
		break;
	case REQUEST_BELL:
		code = input_bell(server, request, length);
		break;
	case REQUEST_CHANGE_POINTER_CONTROL:
		code = input_change_pointer_control(server, index, request, length);
		break;
	case REQUEST_GET_POINTER_CONTROL:
		code = input_get_pointer_control(server, index, length);
		break;
	case REQUEST_SET_POINTER_MAPPING:
		code = input_set_pointer_mapping(server, index, request, length);
		break;
	case REQUEST_GET_POINTER_MAPPING:
		code = input_get_pointer_mapping(server, index, length);
		break;
	case REQUEST_SET_MODIFIER_MAPPING:
		code = input_set_modifier_mapping(server, index, request, length);
		break;
	case REQUEST_GET_MODIFIER_MAPPING:
		code = input_get_modifier_mapping(server, index, length);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/* Holds a device event at the end of the ring (a full ring drops it). */
static void
input_queue(
	struct x11server *server,
	unsigned kind,
	uint8_t detail,
	int pressed,
	uint32_t time,
	int x,
	int y)
{
	struct x11_device_event *event;
	unsigned slot;

	/* A ring full of events a frozen device holds drops the newest. */
	if (server->device_count == X11_DEVICE_QUEUE) {
		x11_log(server, "X11 INPUT dropped kind=%u (the frozen devices hold %u events)", kind, server->device_count);
		return;
	}

	/* Succeeded: the event after the others. */
	slot = (server->device_head + server->device_count) % X11_DEVICE_QUEUE;
	event = &server->device_queue[slot];
	event->kind = kind;
	event->detail = detail;
	event->pressed = 0U;
	if (pressed)
		event->pressed = 1U;
	event->time = time;
	event->x = x;
	event->y = y;
	server->device_count++;
}

/*
 * Handles the device events held, in order, until one whose device is
 * frozen (or none is left).
 */
static void
input_drain(
	struct x11server *server)
{
	struct x11_device_event event;
	int frozen;

	/* A drain already running (an event's handling thawed a device) takes the rest. */
	if (server->draining)
		return;
	server->draining = 1;

	/* Each event while its device is not frozen. */
	while (server->device_count > 0U) {
		/* The first event, whose device must not be frozen. */
		event = server->device_queue[server->device_head];
		frozen = 0;
		if (event.kind == X11_DEVICE_KEY && server->keyboard_freeze != X11_FREEZE_NONE)
			frozen = 1;
		if (event.kind != X11_DEVICE_KEY && server->pointer_freeze != X11_FREEZE_NONE)
			frozen = 1;
		if (frozen)
			break;

		/* Taken from the ring, and handled. */
		server->device_head = (server->device_head + 1U) % X11_DEVICE_QUEUE;
		server->device_count--;
		input_handle(server, &event, NULL);
	}

	/* The drain is over. */
	server->draining = 0;
}

/*
 * Handles one device event; passive grabs at or above the ignored window
 * (a replayed event's grab) are not started.
 */
static void
input_handle(
	struct x11server *server,
	const struct x11_device_event *event,
	const struct x11_window *ignore)
{
	/* The event's kind. */
	switch (event->kind) {
	case X11_DEVICE_KEY:
		input_handle_key(server, event, ignore);
		break;
	case X11_DEVICE_BUTTON:
		input_handle_button(server, event, ignore);
		break;
	case X11_DEVICE_MOTION:
		input_handle_motion(server, event);
		break;
	case X11_DEVICE_LEAVE:
		/* Outside every X window: the pointer is in the root window. */
		server->pointer_outside = 1;
		input_pointer_check(server, event->time);
		break;
	default:
		break;
	}
}

/*
 * Handles a key: the keys down and the locks change, and the press or
 * release goes to the keyboard's grab, to a passive grab it starts, or
 * from the focus window.  Its state is the one before it.
 */
static void
input_handle_key(
	struct x11server *server,
	const struct x11_device_event *event,
	const struct x11_window *ignore)
{
	struct x11_passive_grab *passive;
	struct x11_grab grab;
	uint32_t mask;
	uint16_t state;
	uint8_t keycode;
	uint8_t type;
	int down;
	int locking;

	/* A keycode the keyboard has, and a release of a key that is down. */
	keycode = event->detail;
	if (keycode < KEYCODE_MIN)
		return;
	down = input_key_down(server, keycode);
	if (!event->pressed && !down)
		return;

	/* The state before the key, then the key down or up (a locking key's press turns its modifiers over). */
	state = x11_input_state(server);
	input_key_set(server, keycode, event->pressed);
	locking = input_locking(server, keycode);
	if (event->pressed && !down && locking)
		input_lock_toggle(server, keycode);

	/* The press or the release. */
	type = X11_EVENT_KEY_RELEASE;
	mask = X11_MASK_KEY_RELEASE;
	if (event->pressed) {
		type = X11_EVENT_KEY_PRESS;
		mask = X11_MASK_KEY_PRESS;
	}

	/* A grabbed keyboard speaks to its client; a passive grab ends with its key's release. */
	if (server->keyboard_grab.client != X11_NO_CLIENT) {
		input_key_to_grab(server, type, mask, keycode, event->time, state);
		input_reported(server, 1, event);
		if (!event->pressed && server->keyboard_grab.passive && server->keyboard_grab.key == keycode)
			input_keyboard_grab_end(server);
		return;
	}

	/* A press that starts a passive grab goes to its client. */
	if (event->pressed) {
		passive = input_passive_key(server, keycode, (uint16_t)(state & MODIFIER_BITS), ignore);
		if (passive != NULL) {
			input_grab_from_passive(passive, event->time, &grab);
			grab.key = keycode;
			input_keyboard_grab_start(server, &grab);
			input_key_to_grab(server, type, mask, keycode, event->time, state);

			/* The event the keyboard froze on, for ReplayKeyboard. */
			if (server->keyboard_freeze != X11_FREEZE_NONE) {
				server->keyboard_replay = *event;
				server->keyboard_replay_valid = 1;
			}
			return;
		}
	}

	/* Succeeded: from the focus window. */
	(void)input_key_normal(server, type, mask, keycode, event->time, state, X11_NO_CLIENT);
}

/*
 * Handles a button: the physical button is the logical one of the
 * pointer's map, and the press or release goes to the pointer's grab, to
 * a passive grab it starts, or up the tree from the window under the
 * pointer, starting the implicit grab.  Its state is the one before it.
 */
static void
input_handle_button(
	struct x11server *server,
	const struct x11_device_event *event,
	const struct x11_window *ignore)
{
	struct x11_passive_grab *passive;
	struct x11_window *under;
	struct x11_window *reached;
	struct x11_grab grab;
	uint32_t mask;
	uint32_t selected;
	uint16_t physical_bit;
	uint16_t state;
	uint8_t physical;
	uint8_t logical;
	uint8_t type;
	int reported;

	/* A button the pointer has. */
	physical = event->detail;
	if (physical == 0U || physical > X11_BUTTONS)
		return;
	physical_bit = (uint16_t)(1U << (physical - 1U));

	/* The pointer is where the button changed. */
	input_pointer_moves(server, event->x, event->y, event->time);

	/* The logical button: a press is taken as its map says, its release as the press was. */
	if (event->pressed) {
		if ((server->physical_buttons & physical_bit) != 0U)
			return;
		server->physical_buttons = (uint16_t)(server->physical_buttons | physical_bit);
		logical = server->button_map[physical - 1U];
		server->pressed_as[physical - 1U] = logical;
	} else {
		if ((server->physical_buttons & physical_bit) == 0U)
			return;
		server->physical_buttons = (uint16_t)(server->physical_buttons & ~physical_bit);
		logical = server->pressed_as[physical - 1U];
		server->pressed_as[physical - 1U] = 0U;
	}

	/* A button the map turned off says nothing. */
	if (logical == 0U)
		return;

	/* The state before the button, then the button down or up. */
	state = x11_input_state(server);
	type = X11_EVENT_BUTTON_RELEASE;
	mask = X11_MASK_BUTTON_RELEASE;
	if (event->pressed) {
		type = X11_EVENT_BUTTON_PRESS;
		mask = X11_MASK_BUTTON_PRESS;
		server->buttons = (uint16_t)(server->buttons | (1U << (logical - 1U)));
	} else {
		server->buttons = (uint16_t)(server->buttons & ~(1U << (logical - 1U)));
	}

	/* A grabbed pointer speaks to its client; a passive or implicit grab ends with the last button's release. */
	if (server->pointer_grab.client != X11_NO_CLIENT) {
		reported = input_pointer_to_grab(server, type, mask, logical, event->time, state);
		if (reported)
			input_reported(server, 0, event);
		if (!event->pressed && server->pointer_grab.passive && server->buttons == 0U)
			input_pointer_grab_end(server, event->time);
		return;
	}

	/* A press that starts a passive grab goes to its client. */
	if (event->pressed) {
		passive = input_passive_button(server, logical, (uint16_t)(state & MODIFIER_BITS), ignore);
		if (passive != NULL) {
			input_grab_from_passive(passive, event->time, &grab);
			input_pointer_grab_start(server, &grab);
			(void)input_pointer_to_grab(server, type, mask, logical, event->time, state);

			/* The event the pointer froze on, for ReplayPointer. */
			if (server->pointer_freeze != X11_FREEZE_NONE) {
				server->pointer_replay = *event;
				server->pointer_replay_valid = 1;
			}
			return;
		}
	}

	/* Up the tree from the window under the pointer. */
	under = x11_input_window_under(server);
	reached = x11_event_propagate(server, under, NULL, mask, type, logical, event->time, state, X11_NO_CLIENT);
	if (!event->pressed || reached == NULL)
		return;

	/*
	 * Succeeded: the press grabs the pointer for the client that selected
	 * ButtonPress on the window it reached, with that client's events
	 * there, its own windows hearing theirs when it asked OwnerGrabButton.
	 */
	memset(&grab, 0, sizeof(grab));
	grab.client = x11_event_selector(reached, X11_MASK_BUTTON_PRESS);
	grab.window = reached->id;
	selected = x11_event_mask_of(reached, grab.client);
	grab.mask = selected & POINTER_EVENT_BITS;
	if ((selected & X11_MASK_OWNER_GRAB_BUTTON) != 0U)
		grab.owner_events = 1U;
	grab.pointer_mode = MODE_ASYNC;
	grab.keyboard_mode = MODE_ASYNC;
	grab.passive = 1U;
	grab.implicit = 1U;
	grab.time = event->time;
	input_pointer_grab_start(server, &grab);
}

/*
 * Handles a move of the pointer: the crossing events into the window it
 * went to, then the motion to the pointer's grab or up the tree from the
 * window under it, to the clients that asked for motion (with these
 * buttons held).
 */
static void
input_handle_motion(
	struct x11server *server,
	const struct x11_device_event *event)
{
	struct x11_window *under;
	uint32_t mask;
	uint32_t held;
	uint16_t state;

	/* The pointer is there. */
	input_pointer_moves(server, event->x, event->y, event->time);

	/* Any motion, and motion with each of buttons 1 to 5 held. */
	state = x11_input_state(server);
	mask = X11_MASK_POINTER_MOTION;
	held = server->buttons & STATE_BUTTONS;
	if (held != 0U)
		mask |= X11_MASK_BUTTON_MOTION | (held << STATE_BUTTON_SHIFT);

	/* A grabbed pointer speaks to its client. */
	if (server->pointer_grab.client != X11_NO_CLIENT) {
		(void)input_pointer_to_grab(server, X11_EVENT_MOTION_NOTIFY, mask, 0U, event->time, state);
		return;
	}

	/* Succeeded: up the tree from the window under the pointer. */
	under = x11_input_window_under(server);
	(void)x11_event_propagate(server, under, NULL, mask, X11_EVENT_MOTION_NOTIFY, 0U, event->time, state, X11_NO_CLIENT);
}

/* Puts the pointer at a place inside the X windows, with the crossing events of the window it went into. */
static void
input_pointer_moves(
	struct x11server *server,
	int x,
	int y,
	uint32_t time)
{
	/* The place. */
	server->pointer_outside = 0;
	server->pointer_x = x;
	server->pointer_y = y;

	/* Succeeded: the window under it. */
	input_pointer_check(server, time);
}

/*
 * Follows the window under the pointer: when it is another than the one
 * the pointer was last found in, the crossing events between them.
 */
static void
input_pointer_check(
	struct x11server *server,
	uint32_t time)
{
	struct x11_window *under;
	struct x11_window *last;

	/* The window it is in now, and the one it was in. */
	under = x11_input_window_under(server);
	last = x11_window_find(server, server->pointer_window);
	if (last == NULL)
		last = server->root;
	if (last == under)
		return;

	/* Succeeded: it left the one and entered the other. */
	server->pointer_window = under->id;
	input_crossing(server, last, under, NOTIFY_NORMAL, time);
}

/*
 * Sends a key from the focus window: from the window under the pointer
 * when it is in the focus window, up to the focus window.  With a client
 * named, only that client hears it (as it would without the grab).
 * Returns nonzero when a client heard it.
 */
static int
input_key_normal(
	struct x11server *server,
	uint8_t type,
	uint32_t mask,
	uint8_t keycode,
	uint32_t time,
	uint16_t state,
	unsigned only)
{
	struct x11_window *focus;
	struct x11_window *source;
	struct x11_window *reached;
	int inside;

	/* No focus drops the key; PointerRoot is the root window, whose inferiors take what is under the pointer. */
	focus = input_focus_window(server);
	if (focus == NULL)
		return 0;

	/* The window under the pointer, when it is the focus window's or one of its inferiors'. */
	source = x11_input_window_under(server);
	inside = x11_window_inferior(source, focus);
	if (!inside)
		source = focus;

	/* Sent up to the focus window. */
	reached = x11_event_propagate(server, source, focus, mask, type, keycode, time, state, only);
	if (reached == NULL)
		return 0;

	/* Succeeded: heard. */
	return 1;
}

/*
 * Sends a key to the client grabbing the keyboard: as usual when its own
 * windows are to hear theirs and one does, else as seen from the grab's
 * window (a grab hears every key, whatever it selected).
 */
static void
input_key_to_grab(
	struct x11server *server,
	uint8_t type,
	uint32_t mask,
	uint8_t keycode,
	uint32_t time,
	uint16_t state)
{
	struct x11_window *window;
	struct x11_window *under;
	struct x11_client *client;
	uint8_t event[32];
	uint32_t child;
	int heard;

	/* Its own windows first, when it asked. */
	if (server->keyboard_grab.owner_events) {
		heard = input_key_normal(server, type, mask, keycode, time, state, server->keyboard_grab.client);
		if (heard)
			return;
	}

	/* The grab's window and client. */
	window = x11_window_find(server, server->keyboard_grab.window);
	client = x11_client_of(server, server->keyboard_grab.client);
	if (window == NULL || client == NULL)
		return;

	/* Succeeded: as seen from the grab's window, the child toward the window under the pointer. */
	under = x11_input_window_under(server);
	child = x11_event_child_toward(window, under);
	x11_event_device(event, server, type, keycode, time, window, child, state);
	x11_event_send(server, client, event);
}

/*
 * Sends a pointer event to the client grabbing the pointer: as usual when
 * its own windows are to hear theirs and one does, else as seen from the
 * grab's window when the grab asked for it.  Returns nonzero when the
 * client heard it.
 */
static int
input_pointer_to_grab(
	struct x11server *server,
	uint8_t type,
	uint32_t mask,
	uint8_t detail,
	uint32_t time,
	uint16_t state)
{
	struct x11_window *window;
	struct x11_window *under;
	struct x11_window *reached;
	struct x11_client *client;
	uint8_t event[32];
	uint32_t child;

	/* Its own windows first, when it asked. */
	under = x11_input_window_under(server);
	if (server->pointer_grab.owner_events) {
		reached = x11_event_propagate(server, under, NULL, mask, type, detail, time, state, server->pointer_grab.client);
		if (reached != NULL)
			return 1;
	}

	/* The grab must have asked for it. */
	if ((server->pointer_grab.mask & mask) == 0U)
		return 0;

	/* The grab's window and client. */
	window = x11_window_find(server, server->pointer_grab.window);
	client = x11_client_of(server, server->pointer_grab.client);
	if (window == NULL || client == NULL)
		return 0;

	/* Succeeded: as seen from the grab's window, the child toward the window under the pointer. */
	child = x11_event_child_toward(window, under);
	x11_event_device(event, server, type, detail, time, window, child, state);
	x11_event_send(server, client, event);
	return 1;
}

/*
 * Notes that a button (keyboard 0) or a key (keyboard 1) reached the
 * grabbing client: a device AllowEvents let go until then (SyncPointer,
 * SyncKeyboard, SyncBoth) freezes again on it.
 */
static void
input_reported(
	struct x11server *server,
	int keyboard,
	const struct x11_device_event *event)
{
	/* SyncPointer: the pointer freezes on this button. */
	if (!keyboard && server->pointer_sync) {
		server->pointer_sync = 0;
		server->pointer_freeze = X11_FREEZE_POINTER;
		server->pointer_replay = *event;
		server->pointer_replay_valid = 1;
	}

	/* SyncKeyboard: the keyboard freezes on this key. */
	if (keyboard && server->keyboard_sync) {
		server->keyboard_sync = 0;
		server->keyboard_freeze = X11_FREEZE_KEYBOARD;
		server->keyboard_replay = *event;
		server->keyboard_replay_valid = 1;
	}

	/* SyncBoth: both freeze again, by the grab of the device the event came from. */
	if (server->both_sync) {
		server->both_sync = 0;
		if (keyboard) {
			server->keyboard_freeze = X11_FREEZE_KEYBOARD;
			server->pointer_freeze = X11_FREEZE_KEYBOARD;
			server->keyboard_replay = *event;
			server->keyboard_replay_valid = 1;
		} else {
			server->pointer_freeze = X11_FREEZE_POINTER;
			server->keyboard_freeze = X11_FREEZE_POINTER;
			server->pointer_replay = *event;
			server->pointer_replay_valid = 1;
		}
	}
}

/*
 * Starts a grab of the pointer: the crossing events into the grab's
 * window (but for an implicit grab), the system's pointer captured, and
 * the devices its modes freeze.
 */
static void
input_pointer_grab_start(
	struct x11server *server,
	const struct x11_grab *grab)
{
	struct x11_window *under;
	struct x11_window *window;

	/* As if the pointer went into the grab's window. */
	window = x11_window_find(server, grab->window);
	under = x11_input_window_under(server);
	if (!grab->implicit && window != NULL)
		input_crossing(server, under, window, NOTIFY_GRAB, grab->time);

	/* The grab, and when the pointer was last grabbed. */
	server->pointer_grab = *grab;
	server->pointer_grab_time = grab->time;

	/* The system keeps the pointer for the grab's windows (an implicit grab's button already does). */
	if (!grab->implicit)
		input_capture(server, grab, 1);

	/* Succeeded: the devices its modes freeze. */
	if (grab->pointer_mode == MODE_SYNC)
		server->pointer_freeze = X11_FREEZE_POINTER;
	if (grab->keyboard_mode == MODE_SYNC)
		server->keyboard_freeze = X11_FREEZE_POINTER;
}

/*
 * Ends the grab of the pointer: the devices it froze thaw, the system's
 * pointer is let go, and the crossing events back to the window under the
 * pointer.  The caller handles the events the thaw lets go.
 */
static void
input_pointer_grab_end(
	struct x11server *server,
	uint32_t time)
{
	struct x11_window *window;
	struct x11_window *under;
	struct x11_grab grab;

	/* No grab. */
	if (server->pointer_grab.client == X11_NO_CLIENT)
		return;

	/* The grab goes, with what AllowEvents left of it. */
	grab = server->pointer_grab;
	memset(&server->pointer_grab, 0, sizeof(server->pointer_grab));
	server->pointer_grab.client = X11_NO_CLIENT;
	server->pointer_sync = 0;
	server->pointer_replay_valid = 0;
	if (server->keyboard_grab.client == X11_NO_CLIENT)
		server->both_sync = 0;

	/* The devices it froze thaw. */
	if (server->pointer_freeze == X11_FREEZE_POINTER)
		server->pointer_freeze = X11_FREEZE_NONE;
	if (server->keyboard_freeze == X11_FREEZE_POINTER)
		server->keyboard_freeze = X11_FREEZE_NONE;

	/* The system lets the pointer go. */
	if (!grab.implicit)
		input_capture(server, &grab, 0);

	/* Succeeded: as if the pointer came back from the grab's window. */
	window = x11_window_find(server, grab.window);
	under = x11_input_window_under(server);
	if (window != NULL)
		input_crossing(server, window, under, NOTIFY_UNGRAB, time);
}

/*
 * Starts a grab of the keyboard: the focus events into the grab's window,
 * and the devices its modes freeze.
 */
static void
input_keyboard_grab_start(
	struct x11server *server,
	const struct x11_grab *grab)
{
	/* As if the focus went to the grab's window. */
	input_focus_change(server, server->focus, grab->window, NOTIFY_GRAB);

	/* The grab, and when the keyboard was last grabbed. */
	server->keyboard_grab = *grab;
	server->keyboard_grab_time = grab->time;

	/* Succeeded: the devices its modes freeze. */
	if (grab->keyboard_mode == MODE_SYNC)
		server->keyboard_freeze = X11_FREEZE_KEYBOARD;
	if (grab->pointer_mode == MODE_SYNC)
		server->pointer_freeze = X11_FREEZE_KEYBOARD;
}

/*
 * Ends the grab of the keyboard: the devices it froze thaw, and the focus
 * events back to the focus window.  The caller handles the events the
 * thaw lets go.
 */
static void
input_keyboard_grab_end(
	struct x11server *server)
{
	uint32_t window;

	/* No grab. */
	if (server->keyboard_grab.client == X11_NO_CLIENT)
		return;

	/* The grab goes, with what AllowEvents left of it. */
	window = server->keyboard_grab.window;
	memset(&server->keyboard_grab, 0, sizeof(server->keyboard_grab));
	server->keyboard_grab.client = X11_NO_CLIENT;
	server->keyboard_sync = 0;
	server->keyboard_replay_valid = 0;
	if (server->pointer_grab.client == X11_NO_CLIENT)
		server->both_sync = 0;

	/* The devices it froze thaw. */
	if (server->keyboard_freeze == X11_FREEZE_KEYBOARD)
		server->keyboard_freeze = X11_FREEZE_NONE;
	if (server->pointer_freeze == X11_FREEZE_KEYBOARD)
		server->pointer_freeze = X11_FREEZE_NONE;

	/* Succeeded: as if the focus came back from the grab's window. */
	input_focus_change(server, window, server->focus, NOTIFY_UNGRAB);
}

/* Makes the active grab a passive grab starts, at a time. */
static void
input_grab_from_passive(
	const struct x11_passive_grab *passive,
	uint32_t time,
	struct x11_grab *grab)
{
	/* The passive grab's client, window, events and modes; it ends with its button's or key's release. */
	memset(grab, 0, sizeof(*grab));
	grab->client = passive->client;
	grab->window = passive->window;
	grab->mask = passive->mask;
	grab->confine_to = passive->confine_to;
	grab->cursor = passive->cursor;
	grab->time = time;
	grab->owner_events = passive->owner_events;
	grab->pointer_mode = passive->pointer_mode;
	grab->keyboard_mode = passive->keyboard_mode;
	grab->passive = 1U;
}

/*
 * Returns the passive grab on a window that a button or key (keyboard
 * nonzero) with these modifiers starts, or NULL.
 */
static struct x11_passive_grab *
input_passive_find(
	struct x11server *server,
	const struct x11_window *window,
	int keyboard,
	uint8_t detail,
	uint16_t modifiers)
{
	struct x11_passive_grab *passive;
	unsigned index;

	/* Each passive grab of the window and the device whose button or key and modifiers match. */
	for (index = 0U; index < server->passive_count; index++) {
		passive = &server->passive_grabs[index];
		if (passive->window != window->id)
			continue;
		if ((int)passive->keyboard != keyboard)
			continue;
		if (passive->detail != ANY_DETAIL && passive->detail != detail)
			continue;
		if (passive->modifiers != ANY_MODIFIER && passive->modifiers != modifiers)
			continue;
		return passive;
	}

	/* None. */
	return NULL;
}

/*
 * Returns the passive grab a key press starts: the one on the window
 * nearest the root from the root down to the focus window, then down from
 * the focus window to the window under the pointer when it is inside the
 * focus window.  Windows at or above the ignored one are passed over.
 */
static struct x11_passive_grab *
input_passive_key(
	struct x11server *server,
	uint8_t keycode,
	uint16_t modifiers,
	const struct x11_window *ignore)
{
	struct x11_passive_grab *passive;
	struct x11_window *focus;
	struct x11_window *under;
	struct x11_window *window;
	unsigned steps;
	int ignored;
	int inside;

	/* No focus starts none. */
	focus = input_focus_window(server);
	if (focus == NULL)
		return NULL;

	/* From the root window down to the focus window. */
	steps = input_distance(focus, NULL);
	while (steps > 0U) {
		steps--;
		window = input_ancestor(focus, steps);
		ignored = input_ignored(window, ignore);
		if (ignored)
			continue;
		passive = input_passive_find(server, window, 1, keycode, modifiers);
		if (passive != NULL)
			return passive;
	}

	/* From below the focus window down to the window under the pointer, when it is inside. */
	under = x11_input_window_under(server);
	inside = input_strictly_inferior(under, focus);
	if (!inside)
		return NULL;
	steps = input_distance(under, focus);
	while (steps > 0U) {
		steps--;
		window = input_ancestor(under, steps);
		ignored = input_ignored(window, ignore);
		if (ignored)
			continue;
		passive = input_passive_find(server, window, 1, keycode, modifiers);
		if (passive != NULL)
			return passive;
	}

	/* None. */
	return NULL;
}

/*
 * Returns the passive grab a button press starts: the one on the window
 * nearest the root from the root down to the window under the pointer
 * whose confining window (if any) is viewable.  Windows at or above the
 * ignored one are passed over.
 */
static struct x11_passive_grab *
input_passive_button(
	struct x11server *server,
	uint8_t button,
	uint16_t modifiers,
	const struct x11_window *ignore)
{
	struct x11_passive_grab *passive;
	struct x11_window *under;
	struct x11_window *window;
	struct x11_window *confine;
	unsigned steps;
	int ignored;
	int viewable;

	/* From the root window down to the window under the pointer. */
	under = x11_input_window_under(server);
	steps = input_distance(under, NULL);
	while (steps > 0U) {
		steps--;
		window = input_ancestor(under, steps);
		ignored = input_ignored(window, ignore);
		if (ignored)
			continue;
		passive = input_passive_find(server, window, 0, button, modifiers);
		if (passive == NULL)
			continue;

		/* Its confining window must be viewable. */
		if (passive->confine_to != 0U) {
			confine = x11_window_find(server, passive->confine_to);
			viewable = 0;
			if (confine != NULL)
				viewable = x11_window_viewable(confine);
			if (!viewable)
				continue;
		}
		return passive;
	}

	/* None. */
	return NULL;
}

/* Tells whether a window is the ignored one or one of its ancestors (none ignored when it is NULL). */
static int
input_ignored(
	const struct x11_window *window,
	const struct x11_window *ignore)
{
	int above;

	/* Nothing is ignored. */
	if (ignore == NULL)
		return 0;

	/* Succeeded: ignored when the ignored window is in its tree. */
	above = x11_window_inferior(ignore, window);
	return above;
}

/* Removes a passive grab (the last takes its place), giving back what its client was charged. */
static void
input_passive_remove(
	struct x11server *server,
	unsigned index)
{
	/* The client's charge. */
	x11_memory_release(server, server->passive_grabs[index].client, sizeof(struct x11_passive_grab));

	/* Succeeded: the last in its place. */
	server->passive_grabs[index] = server->passive_grabs[server->passive_count - 1U];
	server->passive_count--;
}

/*
 * Moves the focus (a window, PointerRoot or None) with its revert-to and
 * time, and sends the focus events (WhileGrabbed while the keyboard is
 * grabbed).
 */
static void
input_set_focus(
	struct x11server *server,
	uint32_t focus,
	uint8_t revert,
	uint32_t time)
{
	uint32_t old;
	uint8_t mode;

	/* The mode, by the keyboard's grab. */
	mode = NOTIFY_NORMAL;
	if (server->keyboard_grab.client != X11_NO_CLIENT)
		mode = NOTIFY_WHILE_GRABBED;

	/* The focus. */
	old = server->focus;
	server->focus = focus;
	server->focus_revert = revert;
	server->focus_time = time;

	/* Succeeded: the events of the change. */
	input_focus_change(server, old, focus, mode);
}

/*
 * Moves the focus off a window that is no longer viewable (or is being
 * destroyed): to its nearest viewable ancestor (revert-to Parent, which
 * becomes None), PointerRoot or None.
 */
static void
input_revert_focus(
	struct x11server *server,
	struct x11_window *lost)
{
	struct x11_window *ancestor;
	int viewable;

	/* PointerRoot and None. */
	if (server->focus_revert == REVERT_POINTER_ROOT) {
		input_set_focus(server, X11_FOCUS_POINTER_ROOT, REVERT_POINTER_ROOT, server->now);
		return;
	}
	if (server->focus_revert == REVERT_NONE) {
		input_set_focus(server, X11_FOCUS_NONE, REVERT_NONE, server->now);
		return;
	}

	/* Parent: the nearest viewable ancestor (the root window at the least). */
	ancestor = lost->parent;
	while (ancestor != NULL) {
		viewable = x11_window_viewable(ancestor);
		if (viewable)
			break;
		ancestor = ancestor->parent;
	}
	if (ancestor == NULL)
		ancestor = server->root;

	/* Succeeded: there, reverting to None from then on. */
	input_set_focus(server, ancestor->id, REVERT_NONE, server->now);
}

/*
 * Sends the focus events of a change of the focus from one window (or
 * PointerRoot, None) to another, as the core protocol lays them down,
 * with the window under the pointer as P.
 */
static void
input_focus_change(
	struct x11server *server,
	uint32_t from,
	uint32_t to,
	uint8_t mode)
{
	struct x11_window *old;
	struct x11_window *new;
	struct x11_window *pointer;
	struct x11_window *common;
	struct x11_window *root;
	uint8_t old_detail;
	uint8_t new_detail;
	int pointer_in_old;
	int pointer_in_new;
	int related;

	/* No change. */
	if (from == to)
		return;

	/* The windows (NULL for PointerRoot and None), the root window, and P. */
	old = NULL;
	if (from != X11_FOCUS_NONE && from != X11_FOCUS_POINTER_ROOT)
		old = x11_window_find(server, from);
	new = NULL;
	if (to != X11_FOCUS_NONE && to != X11_FOCUS_POINTER_ROOT)
		new = x11_window_find(server, to);
	root = server->root;
	pointer = x11_input_window_under(server);
	old_detail = DETAIL_NONE;
	if (from == X11_FOCUS_POINTER_ROOT)
		old_detail = DETAIL_POINTER_ROOT;
	new_detail = DETAIL_NONE;
	if (to == X11_FOCUS_POINTER_ROOT)
		new_detail = DETAIL_POINTER_ROOT;

	/* From a window to a window. */
	if (old != NULL && new != NULL) {
		/* The new window is an inferior of the old. */
		related = input_strictly_inferior(new, old);
		if (related) {
			pointer_in_old = input_strictly_inferior(pointer, old);
			if (pointer_in_old) {
				related = x11_window_inferior(pointer, new);
				if (!related)
					related = x11_window_inferior(new, pointer);
				if (!related)
					input_focus_up(server, X11_EVENT_FOCUS_OUT, pointer, old, DETAIL_POINTER, mode);
			}
			input_focus_event(server, X11_EVENT_FOCUS_OUT, old, DETAIL_INFERIOR, mode);
			input_focus_down(server, X11_EVENT_FOCUS_IN, old, new->parent, DETAIL_VIRTUAL, mode);
			input_focus_event(server, X11_EVENT_FOCUS_IN, new, DETAIL_ANCESTOR, mode);
			return;
		}

		/* The old window is an inferior of the new. */
		related = input_strictly_inferior(old, new);
		if (related) {
			input_focus_event(server, X11_EVENT_FOCUS_OUT, old, DETAIL_ANCESTOR, mode);
			input_focus_up(server, X11_EVENT_FOCUS_OUT, old->parent, new, DETAIL_VIRTUAL, mode);
			input_focus_event(server, X11_EVENT_FOCUS_IN, new, DETAIL_INFERIOR, mode);
			pointer_in_new = input_strictly_inferior(pointer, new);
			if (pointer_in_new) {
				related = x11_window_inferior(pointer, old);
				if (!related)
					related = x11_window_inferior(old, pointer);
				if (!related)
					input_focus_down(server, X11_EVENT_FOCUS_IN, new, pointer, DETAIL_POINTER, mode);
			}
			return;
		}

		/* Neither: through their nearest common ancestor. */
		common = input_common(old, new);
		pointer_in_old = input_strictly_inferior(pointer, old);
		if (pointer_in_old)
			input_focus_up(server, X11_EVENT_FOCUS_OUT, pointer, old, DETAIL_POINTER, mode);
		input_focus_event(server, X11_EVENT_FOCUS_OUT, old, DETAIL_NONLINEAR, mode);
		input_focus_up(server, X11_EVENT_FOCUS_OUT, old->parent, common, DETAIL_NONLINEAR_VIRTUAL, mode);
		input_focus_down(server, X11_EVENT_FOCUS_IN, common, new->parent, DETAIL_NONLINEAR_VIRTUAL, mode);
		input_focus_event(server, X11_EVENT_FOCUS_IN, new, DETAIL_NONLINEAR, mode);
		pointer_in_new = input_strictly_inferior(pointer, new);
		if (pointer_in_new)
			input_focus_down(server, X11_EVENT_FOCUS_IN, new, pointer, DETAIL_POINTER, mode);
		return;
	}

	/* From a window to PointerRoot or None. */
	if (old != NULL) {
		pointer_in_old = input_strictly_inferior(pointer, old);
		if (pointer_in_old)
			input_focus_up(server, X11_EVENT_FOCUS_OUT, pointer, old, DETAIL_POINTER, mode);
		input_focus_event(server, X11_EVENT_FOCUS_OUT, old, DETAIL_NONLINEAR, mode);
		if (old != root)
			input_focus_up(server, X11_EVENT_FOCUS_OUT, old->parent, NULL, DETAIL_NONLINEAR_VIRTUAL, mode);
		input_focus_event(server, X11_EVENT_FOCUS_IN, root, new_detail, mode);
		if (to == X11_FOCUS_POINTER_ROOT)
			input_focus_down(server, X11_EVENT_FOCUS_IN, NULL, pointer, DETAIL_POINTER, mode);
		return;
	}

	/* From PointerRoot or None to a window. */
	if (new != NULL) {
		if (from == X11_FOCUS_POINTER_ROOT)
			input_focus_up(server, X11_EVENT_FOCUS_OUT, pointer, NULL, DETAIL_POINTER, mode);
		input_focus_event(server, X11_EVENT_FOCUS_OUT, root, old_detail, mode);
		if (new != root)
			input_focus_down(server, X11_EVENT_FOCUS_IN, NULL, new->parent, DETAIL_NONLINEAR_VIRTUAL, mode);
		input_focus_event(server, X11_EVENT_FOCUS_IN, new, DETAIL_NONLINEAR, mode);
		pointer_in_new = input_strictly_inferior(pointer, new);
		if (pointer_in_new)
			input_focus_down(server, X11_EVENT_FOCUS_IN, new, pointer, DETAIL_POINTER, mode);
		return;
	}

	/* Succeeded: between PointerRoot and None (a window gone counts as None). */
	if (from == X11_FOCUS_POINTER_ROOT)
		input_focus_up(server, X11_EVENT_FOCUS_OUT, pointer, NULL, DETAIL_POINTER, mode);
	input_focus_event(server, X11_EVENT_FOCUS_OUT, root, old_detail, mode);
	input_focus_event(server, X11_EVENT_FOCUS_IN, root, new_detail, mode);
	if (to == X11_FOCUS_POINTER_ROOT)
		input_focus_down(server, X11_EVENT_FOCUS_IN, NULL, pointer, DETAIL_POINTER, mode);
}

/* Sends a focus event to each window from one up to (not including) another (NULL: through the root window). */
static void
input_focus_up(
	struct x11server *server,
	uint8_t type,
	struct x11_window *from,
	const struct x11_window *until,
	uint8_t detail,
	uint8_t mode)
{
	struct x11_window *window;

	/* Each window on the way up. */
	for (window = from; window != NULL && window != until; window = window->parent)
		input_focus_event(server, type, window, detail, mode);
}

/*
 * Sends a focus event to each window below one (NULL: from the root
 * window itself) down to and including another, from the top.
 */
static void
input_focus_down(
	struct x11server *server,
	uint8_t type,
	const struct x11_window *top,
	struct x11_window *to,
	uint8_t detail,
	uint8_t mode)
{
	struct x11_window *window;
	unsigned steps;

	/* Each window on the way down, by its distance from the last. */
	steps = input_distance(to, top);
	while (steps > 0U) {
		steps--;
		window = input_ancestor(to, steps);
		input_focus_event(server, type, window, detail, mode);
	}
}

/*
 * Sends a focus event to the clients that asked for focus changes on a
 * window, and after a FocusIn the keys' state to those that asked for it.
 */
static void
input_focus_event(
	struct x11server *server,
	uint8_t type,
	const struct x11_window *window,
	uint8_t detail,
	uint8_t mode)
{
	uint8_t event[32];

	/* The detail, the window and the mode. */
	memset(event, 0, sizeof(event));
	event[0] = type;
	event[1] = detail;
	x11_write32(event + 4, window->id, 0);
	event[8] = mode;
	(void)x11_event_deliver(server, window, X11_MASK_FOCUS_CHANGE, event);

	/* Succeeded: KeymapNotify follows a FocusIn. */
	if (type == X11_EVENT_FOCUS_IN)
		input_keymap_notify_selectors(server, window, X11_MASK_FOCUS_CHANGE);
}

/*
 * Sends the crossing events of the pointer's going from one window to
 * another, as the core protocol lays them down.
 */
static void
input_crossing(
	struct x11server *server,
	struct x11_window *from,
	struct x11_window *to,
	uint8_t mode,
	uint32_t time)
{
	struct x11_window *window;
	struct x11_window *common;
	unsigned steps;
	int related;

	/* No change. */
	if (from == to)
		return;

	/* Into an inferior: out of the old one, through the windows between, into the new one. */
	related = x11_window_inferior(to, from);
	if (related) {
		input_cross(server, X11_EVENT_LEAVE_NOTIFY, from, DETAIL_INFERIOR, mode, time, to);
		steps = input_distance(to, from);
		while (steps > 1U) {
			steps--;
			window = input_ancestor(to, steps);
			input_cross(server, X11_EVENT_ENTER_NOTIFY, window, DETAIL_VIRTUAL, mode, time, to);
		}
		input_cross(server, X11_EVENT_ENTER_NOTIFY, to, DETAIL_ANCESTOR, mode, time, to);
		return;
	}

	/* Out to an ancestor: out of the old one and the windows between, into the new one. */
	related = x11_window_inferior(from, to);
	if (related) {
		input_cross(server, X11_EVENT_LEAVE_NOTIFY, from, DETAIL_ANCESTOR, mode, time, to);
		for (window = from->parent; window != NULL && window != to; window = window->parent)
			input_cross(server, X11_EVENT_LEAVE_NOTIFY, window, DETAIL_VIRTUAL, mode, time, to);
		input_cross(server, X11_EVENT_ENTER_NOTIFY, to, DETAIL_INFERIOR, mode, time, to);
		return;
	}

	/* Succeeded: across, through their nearest common ancestor. */
	common = input_common(from, to);
	input_cross(server, X11_EVENT_LEAVE_NOTIFY, from, DETAIL_NONLINEAR, mode, time, to);
	for (window = from->parent; window != NULL && window != common; window = window->parent)
		input_cross(server, X11_EVENT_LEAVE_NOTIFY, window, DETAIL_NONLINEAR_VIRTUAL, mode, time, to);
	steps = input_distance(to, common);
	while (steps > 1U) {
		steps--;
		window = input_ancestor(to, steps);
		input_cross(server, X11_EVENT_ENTER_NOTIFY, window, DETAIL_NONLINEAR_VIRTUAL, mode, time, to);
	}
	input_cross(server, X11_EVENT_ENTER_NOTIFY, to, DETAIL_NONLINEAR, mode, time, to);
}

/*
 * Sends one crossing event on a window: to the clients that asked for it
 * there, or while the pointer is grabbed only to the grabbing client (on
 * the grab's window when the grab asked for it, on its own windows when
 * they are to hear theirs).  KeymapNotify follows an EnterNotify for the
 * clients that asked for the keys' state.
 */
static void
input_cross(
	struct x11server *server,
	uint8_t type,
	struct x11_window *window,
	uint8_t detail,
	uint8_t mode,
	uint32_t time,
	const struct x11_window *pointer)
{
	struct x11_window *focus;
	struct x11_client *client;
	uint8_t event[32];
	uint32_t child;
	uint32_t bit;
	uint32_t selected;
	uint16_t state;
	uint8_t flags;
	int inside;

	/* The event, as seen from the window, with the child toward the pointer. */
	child = x11_event_child_toward(window, pointer);
	state = x11_input_state(server);
	x11_event_device(event, server, type, detail, time, window, child, state);
	event[30] = mode;

	/* On this screen, and in the focus when the window is the focus window or an inferior of it. */
	flags = CROSSING_SAME_SCREEN;
	focus = input_focus_window(server);
	if (focus != NULL) {
		inside = x11_window_inferior(window, focus);
		if (inside)
			flags |= CROSSING_FOCUS;
	}
	event[31] = flags;

	/* EnterWindow or LeaveWindow. */
	bit = X11_MASK_LEAVE_WINDOW;
	if (type == X11_EVENT_ENTER_NOTIFY)
		bit = X11_MASK_ENTER_WINDOW;

	/* A grabbed pointer: the grabbing client alone. */
	if (server->pointer_grab.client != X11_NO_CLIENT) {
		selected = 0U;
		if (window->id == server->pointer_grab.window)
			selected = server->pointer_grab.mask;
		if (server->pointer_grab.owner_events)
			selected |= x11_event_mask_of(window, server->pointer_grab.client);
		client = x11_client_of(server, server->pointer_grab.client);
		if ((selected & bit) == 0U || client == NULL)
			return;
		x11_event_send(server, client, event);
		if (type == X11_EVENT_ENTER_NOTIFY && (selected & X11_MASK_KEYMAP_STATE) != 0U)
			input_keymap_notify(server, client);
		return;
	}

	/* Succeeded: every client that asked, and the keys' state after an EnterNotify. */
	(void)x11_event_deliver(server, window, bit, event);
	if (type == X11_EVENT_ENTER_NOTIFY)
		input_keymap_notify_selectors(server, window, X11_MASK_ENTER_WINDOW);
}

/* Sends KeymapNotify (the keys down, keycodes 8 to 255) to a client. */
static void
input_keymap_notify(
	struct x11server *server,
	struct x11_client *client)
{
	uint8_t event[32];

	/* The keys from keycode 8: the bytes after the first of the keys' bits. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_KEYMAP_NOTIFY;
	memcpy(event + 1, server->keys_down + 1, sizeof(event) - 1U);

	/* Succeeded: sent. */
	x11_event_send(server, client, event);
}

/* Sends KeymapNotify to each client that asked on a window for KeymapState and for the event it follows. */
static void
input_keymap_notify_selectors(
	struct x11server *server,
	const struct x11_window *window,
	uint32_t mask)
{
	struct x11_client *client;
	uint32_t selected;
	unsigned index;

	/* Each client that asked for both. */
	for (index = 0U; index < window->select_count; index++) {
		selected = window->selects[index].mask;
		if ((selected & mask) == 0U || (selected & X11_MASK_KEYMAP_STATE) == 0U)
			continue;
		client = x11_client_of(server, window->selects[index].client);
		if (client != NULL)
			input_keymap_notify(server, client);
	}
}

/* Returns how many steps up from a window reach another (NULL: past the root window). */
static unsigned
input_distance(
	const struct x11_window *window,
	const struct x11_window *top)
{
	unsigned steps;

	/* Up to it. */
	steps = 0U;
	while (window != NULL && window != top) {
		steps++;
		window = window->parent;
	}

	/* Succeeded: the steps. */
	return steps;
}

/* Returns the ancestor of a window some steps up (the window itself for none). */
static struct x11_window *
input_ancestor(
	struct x11_window *window,
	unsigned steps)
{
	/* Up the steps. */
	while (steps > 0U && window != NULL) {
		window = window->parent;
		steps--;
	}

	/* Succeeded: there. */
	return window;
}

/* Returns the nearest window both windows are in (themselves included). */
static struct x11_window *
input_common(
	struct x11_window *first,
	struct x11_window *second)
{
	struct x11_window *window;
	int inside;

	/* Up from the first until the second is inside. */
	for (window = first; window != NULL; window = window->parent) {
		inside = x11_window_inferior(second, window);
		if (inside)
			return window;
	}

	/* Succeeded: the root window, which every window is in. */
	return second;
}

/* Tells whether a window is an inferior of another, not the other itself. */
static int
input_strictly_inferior(
	const struct x11_window *window,
	const struct x11_window *ancestor)
{
	int inside;

	/* The window itself is not its own inferior. */
	if (window == ancestor)
		return 0;

	/* Succeeded: inside its tree. */
	inside = x11_window_inferior(window, ancestor);
	return inside;
}

/* Returns the focus window: the root window for PointerRoot, NULL for None. */
static struct x11_window *
input_focus_window(
	struct x11server *server)
{
	struct x11_window *focus;

	/* None. */
	if (server->focus == X11_FOCUS_NONE)
		return NULL;

	/* Succeeded: the window, or the root window (PointerRoot, or a window gone). */
	focus = x11_window_find(server, server->focus);
	if (focus == NULL)
		focus = server->root;
	return focus;
}

/* Tells whether a key is down. */
static int
input_key_down(
	const struct x11server *server,
	unsigned keycode)
{
	/* Its bit. */
	if ((server->keys_down[keycode / 8U] & (1U << (keycode % 8U))) != 0U)
		return 1;

	/* Up. */
	return 0;
}

/* Marks a key down or up. */
static void
input_key_set(
	struct x11server *server,
	unsigned keycode,
	int down)
{
	uint8_t bit;

	/* Its bit, set or cleared. */
	bit = (uint8_t)(1U << (keycode % 8U));
	if (down) {
		server->keys_down[keycode / 8U] = (uint8_t)(server->keys_down[keycode / 8U] | bit);
	} else {
		server->keys_down[keycode / 8U] = (uint8_t)(server->keys_down[keycode / 8U] & ~bit);
	}
}

/* Tells whether a key locks its modifiers (its first keysym Caps_Lock, Shift_Lock or Num_Lock). */
static int
input_locking(
	const struct x11server *server,
	unsigned keycode)
{
	uint32_t keysym;

	/* The key's own keysym. */
	keysym = server->keysyms[keycode][0];
	if (keysym == KEYSYM_CAPS_LOCK)
		return 1;
	if (keysym == KEYSYM_SHIFT_LOCK)
		return 1;
	if (keysym == KEYSYM_NUM_LOCK)
		return 1;

	/* Not a locking key. */
	return 0;
}

/* Turns over the modifiers a locking key belongs to. */
static void
input_lock_toggle(
	struct x11server *server,
	unsigned keycode)
{
	unsigned modifier;
	unsigned slot;

	/* Each modifier that has the key. */
	for (modifier = 0U; modifier < X11_MODIFIERS; modifier++) {
		for (slot = 0U; slot < server->keys_per_modifier; slot++) {
			if (server->modifier_keys[modifier][slot] == keycode) {
				server->locks = (uint16_t)(server->locks ^ (1U << modifier));
				break;
			}
		}
	}
}

/* Sends MappingNotify (which map changed, and the keycodes of the keyboard's) to every client. */
static void
input_mapping_notify(
	struct x11server *server,
	uint8_t request,
	uint8_t first,
	uint8_t count)
{
	struct x11_client *client;
	uint8_t event[32];
	unsigned index;

	/* The map, and the keycodes. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_MAPPING_NOTIFY;
	event[4] = request;
	event[5] = first;
	event[6] = count;

	/* Succeeded: to every client set up. */
	for (index = 0U; index < server->clients_max; index++) {
		client = x11_client_of(server, index);
		if (client == NULL || !client->setup)
			continue;
		x11_event_send(server, client, event);
	}
}

/*
 * Tells whether a request's time (CurrentTime: now) may change a state
 * last changed at another time: not before it, nor after now.
 */
static int
input_time_valid(
	const struct x11server *server,
	uint32_t time,
	uint32_t since)
{
	/* CurrentTime. */
	if (time == 0U)
		return 1;

	/* Later than the server's time. */
	if ((int32_t)(time - server->now) > 0)
		return 0;

	/* Earlier than the last change. */
	if ((int32_t)(time - since) < 0)
		return 0;

	/* Succeeded: in order. */
	return 1;
}

/* Returns a request's time, CurrentTime being now. */
static uint32_t
input_time(
	const struct x11server *server,
	uint32_t time)
{
	/* CurrentTime. */
	if (time == 0U)
		return server->now;

	/* Succeeded: the time given. */
	return time;
}

/*
 * Asks the backend to keep the system's pointer for a grab's windows (on
 * nonzero) or to let it go: the surface of the grab window's top-level
 * window, or of the one under the pointer for a grab of the root window.
 */
static void
input_capture(
	struct x11server *server,
	const struct x11_grab *grab,
	int on)
{
	struct x11_window *window;
	struct x11_window *top;

	/* A backend that cannot. */
	if (server->ops->capture == NULL)
		return;

	/* Let go. */
	if (!on) {
		server->ops->capture(server->backend, NULL, 0);
		return;
	}

	/* The grab window's top-level window, or the one under the pointer. */
	window = x11_window_find(server, grab->window);
	if (window == NULL || window == server->root)
		window = x11_input_window_under(server);
	top = x11_window_top_level(window);

	/* Succeeded: its surface keeps the pointer (none: nothing to keep it on). */
	if (top->surface != NULL)
		server->ops->capture(server->backend, top->surface, 1);
}

/*
 * GrabPointer: the pointer grabbed for the client, unless another client
 * has it, the windows are not viewable, another client's grab froze it,
 * or the time is out of order.  A client that has it already changes its
 * grab.
 */
static unsigned
input_grab_pointer(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	struct x11_window *confine;
	struct x11_window *old;
	struct x11_grab grab;
	uint8_t reply[32];
	uint32_t confine_id;
	uint32_t mask;
	uint32_t time;
	unsigned status;
	unsigned freezer;
	int viewable;
	int valid;
	int msb;

	/* The request: owner-events, the window, the events, the modes, the confining window, the cursor, the time. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 24U)
		return X11_BAD_LENGTH;
	if (request[1] > 1U || request[10] > MODE_ASYNC || request[11] > MODE_ASYNC)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	mask = x11_read16(request + 8, msb);
	if ((mask & ~POINTER_EVENT_BITS) != 0U)
		return X11_BAD_VALUE;
	confine = NULL;
	confine_id = x11_read32(request + 12, msb);
	if (confine_id != 0U) {
		confine = x11_window_find(server, confine_id);
		if (confine == NULL)
			return X11_BAD_WINDOW;
	}
	time = x11_read32(request + 20, msb);

	/* The grab asked for (the server keeps no cursors yet: any cursor is taken). */
	memset(&grab, 0, sizeof(grab));
	grab.client = index;
	grab.window = window->id;
	grab.mask = mask;
	if (confine != NULL)
		grab.confine_to = confine->id;
	grab.cursor = x11_read32(request + 16, msb);
	grab.time = input_time(server, time);
	grab.owner_events = request[1];
	grab.pointer_mode = request[10];
	grab.keyboard_mode = request[11];

	/* Whose grab froze the pointer, if any. */
	freezer = X11_NO_CLIENT;
	if (server->pointer_freeze == X11_FREEZE_POINTER)
		freezer = server->pointer_grab.client;
	if (server->pointer_freeze == X11_FREEZE_KEYBOARD)
		freezer = server->keyboard_grab.client;

	/* Another client's grab, windows not viewable, another client's freeze, a time out of order. */
	status = GRAB_SUCCESS;
	viewable = x11_window_viewable(window);
	if (viewable && confine != NULL)
		viewable = x11_window_viewable(confine);
	valid = input_time_valid(server, time, server->pointer_grab_time);
	if (server->pointer_grab.client != X11_NO_CLIENT && server->pointer_grab.client != index) {
		status = GRAB_ALREADY;
	} else if (!viewable) {
		status = GRAB_NOT_VIEWABLE;
	} else if (freezer != X11_NO_CLIENT && freezer != index) {
		status = GRAB_FROZEN;
	} else if (!valid) {
		status = GRAB_INVALID_TIME;
	}

	/* The grab: a client's own grab changes in place (into another window with the crossing events). */
	if (status == GRAB_SUCCESS && server->pointer_grab.client == index) {
		old = x11_window_find(server, server->pointer_grab.window);
		if (old != NULL && old != window)
			input_crossing(server, old, window, NOTIFY_GRAB, grab.time);
		server->pointer_grab = grab;
		server->pointer_grab_time = grab.time;
		input_capture(server, &grab, 1);
		if (grab.pointer_mode == MODE_SYNC)
			server->pointer_freeze = X11_FREEZE_POINTER;
		if (grab.keyboard_mode == MODE_SYNC)
			server->keyboard_freeze = X11_FREEZE_POINTER;
	} else if (status == GRAB_SUCCESS) {
		input_pointer_grab_start(server, &grab);
	}

	/* Succeeded: the status. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)status;
	x11_reply(server, client, reply, sizeof(reply));
	input_drain(server);
	return 0U;
}

/* UngrabPointer: the client's grab of the pointer ends, unless the time is out of order. */
static unsigned
input_ungrab_pointer(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t time;
	int valid;

	/* The time. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	time = x11_read32(request + 4, server->clients[index].order);

	/* Only the client's own grab, at a time in order. */
	if (server->pointer_grab.client != index)
		return 0U;
	valid = input_time_valid(server, time, server->pointer_grab_time);
	if (!valid)
		return 0U;

	/* Succeeded: ended, and the events the thaw let go handled. */
	time = input_time(server, time);
	input_pointer_grab_end(server, time);
	input_drain(server);
	return 0U;
}

/*
 * GrabButton and GrabKey: a passive grab on a window of a button or key
 * (any) with modifiers (any).  Another client's grab of the same
 * combination on the window is BadAccess; the client's own is replaced.
 */
static unsigned
input_grab_passive(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_passive_grab passive;
	struct x11_passive_grab *other;
	struct x11_passive_grab *grown;
	struct x11_window *window;
	struct x11_window *confine;
	unsigned capacity;
	unsigned slot;
	int msb;
	int error;

	/* The request: owner-events and the window, then the rest by the request. */
	msb = server->clients[index].order;
	memset(&passive, 0, sizeof(passive));
	passive.client = index;
	passive.owner_events = request[1];
	if (request[1] > 1U)
		return X11_BAD_VALUE;
	if (request[0] == REQUEST_GRAB_BUTTON) {
		/* GrabButton: the events, the modes, the confining window, the cursor, the button, the modifiers. */
		if (length != 24U)
			return X11_BAD_LENGTH;
		passive.mask = x11_read16(request + 8, msb);
		passive.pointer_mode = request[10];
		passive.keyboard_mode = request[11];
		passive.confine_to = x11_read32(request + 12, msb);
		passive.cursor = x11_read32(request + 16, msb);
		passive.detail = request[20];
		passive.modifiers = x11_read16(request + 22, msb);
		if ((passive.mask & ~POINTER_EVENT_BITS) != 0U)
			return X11_BAD_VALUE;
	} else {
		/* GrabKey: the modifiers, the key, the modes. */
		if (length != 16U)
			return X11_BAD_LENGTH;
		passive.keyboard = 1U;
		passive.modifiers = x11_read16(request + 8, msb);
		passive.detail = request[10];
		passive.pointer_mode = request[11];
		passive.keyboard_mode = request[12];
		if (passive.detail != ANY_DETAIL && passive.detail < KEYCODE_MIN)
			return X11_BAD_VALUE;
	}
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	passive.window = window->id;

	/* The modes, the modifiers, and the confining window. */
	if (passive.pointer_mode > MODE_ASYNC || passive.keyboard_mode > MODE_ASYNC)
		return X11_BAD_VALUE;
	if (passive.modifiers != ANY_MODIFIER && (passive.modifiers & ~MODIFIER_BITS) != 0U)
		return X11_BAD_VALUE;
	if (passive.confine_to != 0U) {
		confine = x11_window_find(server, passive.confine_to);
		if (confine == NULL)
			return X11_BAD_WINDOW;
	}

	/* Another client's grab of the same combination on the window (any matching any). */
	for (slot = 0U; slot < server->passive_count; slot++) {
		other = &server->passive_grabs[slot];
		if (other->window != passive.window || other->keyboard != passive.keyboard)
			continue;
		if (other->detail != passive.detail && other->detail != ANY_DETAIL && passive.detail != ANY_DETAIL)
			continue;
		if (other->modifiers != passive.modifiers &&
		    other->modifiers != ANY_MODIFIER &&
		    passive.modifiers != ANY_MODIFIER)
			continue;
		if (other->client != index)
			return X11_BAD_ACCESS;
	}

	/* The client's own grab of exactly the combination is replaced. */
	for (slot = 0U; slot < server->passive_count; slot++) {
		other = &server->passive_grabs[slot];
		if (other->client != index || other->window != passive.window || other->keyboard != passive.keyboard)
			continue;
		if (other->detail != passive.detail || other->modifiers != passive.modifiers)
			continue;
		*other = passive;
		return 0U;
	}

	/* Room for one more. */
	if (server->passive_count == server->passive_capacity) {
		capacity = 8U;
		if (server->passive_capacity != 0U)
			capacity = server->passive_capacity * 2U;
		grown = realloc(server->passive_grabs, (size_t)capacity * sizeof(*grown));
		if (grown == NULL)
			return X11_BAD_ALLOC;
		server->passive_grabs = grown;
		server->passive_capacity = capacity;
	}

	/* The client is charged for it. */
	error = x11_memory_charge(server, index, sizeof(passive));
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Succeeded: kept. */
	server->passive_grabs[server->passive_count] = passive;
	server->passive_count++;
	return 0U;
}

/*
 * UngrabButton and UngrabKey: the client's passive grabs on a window of a
 * button or key (any: every one) with modifiers (any: every one) go.
 */
static unsigned
input_ungrab_passive(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_passive_grab *passive;
	struct x11_window *window;
	uint16_t modifiers;
	uint8_t detail;
	uint8_t keyboard;
	unsigned slot;
	int msb;

	/* The button or key, the window, the modifiers. */
	msb = server->clients[index].order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	keyboard = 0U;
	if (request[0] == REQUEST_UNGRAB_KEY)
		keyboard = 1U;
	detail = request[1];
	if (keyboard && detail != ANY_DETAIL && detail < KEYCODE_MIN)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	modifiers = x11_read16(request + 8, msb);
	if (modifiers != ANY_MODIFIER && (modifiers & ~MODIFIER_BITS) != 0U)
		return X11_BAD_VALUE;

	/* Each of the client's grabs the request covers (from the end, each removal moving the last into its place). */
	slot = server->passive_count;
	while (slot > 0U) {
		slot--;
		passive = &server->passive_grabs[slot];
		if (passive->client != index || passive->window != window->id || passive->keyboard != keyboard)
			continue;
		if (detail != ANY_DETAIL && passive->detail != detail)
			continue;
		if (modifiers != ANY_MODIFIER && passive->modifiers != modifiers)
			continue;
		input_passive_remove(server, slot);
	}

	/* Succeeded: gone. */
	return 0U;
}

/* ChangeActivePointerGrab: the events and the cursor of the client's grab of the pointer, at a time in order. */
static unsigned
input_change_active_grab(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t mask;
	uint32_t time;
	int valid;
	int msb;

	/* The cursor, the time, the events. */
	msb = server->clients[index].order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	time = x11_read32(request + 8, msb);
	mask = x11_read16(request + 12, msb);
	if ((mask & ~POINTER_EVENT_BITS) != 0U)
		return X11_BAD_VALUE;

	/* Only the client's own grab, at a time in order. */
	if (server->pointer_grab.client != index)
		return 0U;
	valid = input_time_valid(server, time, server->pointer_grab_time);
	if (!valid)
		return 0U;

	/* Succeeded: changed. */
	server->pointer_grab.mask = mask;
	server->pointer_grab.cursor = x11_read32(request + 4, msb);
	return 0U;
}

/*
 * GrabKeyboard: the keyboard grabbed for the client, unless another
 * client has it, the window is not viewable, another client's grab froze
 * it, or the time is out of order.  A client that has it already changes
 * its grab.
 */
static unsigned
input_grab_keyboard(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	struct x11_grab grab;
	uint8_t reply[32];
	uint32_t time;
	uint32_t old;
	unsigned status;
	unsigned freezer;
	int viewable;
	int valid;
	int msb;

	/* The request: owner-events, the window, the time, the modes. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	if (request[1] > 1U || request[12] > MODE_ASYNC || request[13] > MODE_ASYNC)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	time = x11_read32(request + 8, msb);

	/* The grab asked for. */
	memset(&grab, 0, sizeof(grab));
	grab.client = index;
	grab.window = window->id;
	grab.time = input_time(server, time);
	grab.owner_events = request[1];
	grab.pointer_mode = request[12];
	grab.keyboard_mode = request[13];

	/* Whose grab froze the keyboard, if any. */
	freezer = X11_NO_CLIENT;
	if (server->keyboard_freeze == X11_FREEZE_POINTER)
		freezer = server->pointer_grab.client;
	if (server->keyboard_freeze == X11_FREEZE_KEYBOARD)
		freezer = server->keyboard_grab.client;

	/* Another client's grab, a window not viewable, another client's freeze, a time out of order. */
	status = GRAB_SUCCESS;
	viewable = x11_window_viewable(window);
	valid = input_time_valid(server, time, server->keyboard_grab_time);
	if (server->keyboard_grab.client != X11_NO_CLIENT && server->keyboard_grab.client != index) {
		status = GRAB_ALREADY;
	} else if (!viewable) {
		status = GRAB_NOT_VIEWABLE;
	} else if (freezer != X11_NO_CLIENT && freezer != index) {
		status = GRAB_FROZEN;
	} else if (!valid) {
		status = GRAB_INVALID_TIME;
	}

	/* The grab: a client's own grab changes in place (to another window with the focus events). */
	if (status == GRAB_SUCCESS && server->keyboard_grab.client == index) {
		old = server->keyboard_grab.window;
		input_focus_change(server, old, window->id, NOTIFY_GRAB);
		server->keyboard_grab = grab;
		server->keyboard_grab_time = grab.time;
		if (grab.keyboard_mode == MODE_SYNC)
			server->keyboard_freeze = X11_FREEZE_KEYBOARD;
		if (grab.pointer_mode == MODE_SYNC)
			server->pointer_freeze = X11_FREEZE_KEYBOARD;
	} else if (status == GRAB_SUCCESS) {
		input_keyboard_grab_start(server, &grab);
	}

	/* Succeeded: the status. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)status;
	x11_reply(server, client, reply, sizeof(reply));
	input_drain(server);
	return 0U;
}

/* UngrabKeyboard: the client's grab of the keyboard ends, unless the time is out of order. */
static unsigned
input_ungrab_keyboard(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t time;
	int valid;

	/* The time. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	time = x11_read32(request + 4, server->clients[index].order);

	/* Only the client's own grab, at a time in order. */
	if (server->keyboard_grab.client != index)
		return 0U;
	valid = input_time_valid(server, time, server->keyboard_grab_time);
	if (!valid)
		return 0U;

	/* Succeeded: ended, and the events the thaw let go handled. */
	input_keyboard_grab_end(server);
	input_drain(server);
	return 0U;
}

/*
 * AllowEvents: the devices the client's grabs froze go on (Async), go on
 * until the next event reaches the client (Sync), or replay the event
 * they froze on without the grab (Replay).
 */
static unsigned
input_allow_events(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t time;
	uint32_t since;
	int pointer_held;
	int keyboard_held;
	int valid;

	/* The mode and the time. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	if (request[1] > ALLOW_ASYNC_BOTH)
		return X11_BAD_VALUE;
	time = x11_read32(request + 4, server->clients[index].order);

	/* A time before the client's grab, or after now, does nothing. */
	since = server->keyboard_grab_time;
	if (server->pointer_grab.client == index)
		since = server->pointer_grab_time;
	valid = input_time_valid(server, time, since);
	if (!valid)
		return 0U;

	/* Which devices the client's grabs froze. */
	pointer_held = 0;
	if (server->pointer_freeze == X11_FREEZE_POINTER && server->pointer_grab.client == index)
		pointer_held = 1;
	if (server->pointer_freeze == X11_FREEZE_KEYBOARD && server->keyboard_grab.client == index)
		pointer_held = 1;
	keyboard_held = 0;
	if (server->keyboard_freeze == X11_FREEZE_KEYBOARD && server->keyboard_grab.client == index)
		keyboard_held = 1;
	if (server->keyboard_freeze == X11_FREEZE_POINTER && server->pointer_grab.client == index)
		keyboard_held = 1;

	/* The mode. */
	switch (request[1]) {
	case ALLOW_ASYNC_POINTER:
		if (pointer_held)
			server->pointer_freeze = X11_FREEZE_NONE;
		break;
	case ALLOW_SYNC_POINTER:
		if (pointer_held && server->pointer_grab.client == index) {
			server->pointer_freeze = X11_FREEZE_NONE;
			server->pointer_sync = 1;
		}
		break;
	case ALLOW_REPLAY_POINTER:
		if (pointer_held && server->pointer_grab.client == index && server->pointer_replay_valid)
			input_replay_pointer(server);
		break;
	case ALLOW_ASYNC_KEYBOARD:
		if (keyboard_held)
			server->keyboard_freeze = X11_FREEZE_NONE;
		break;
	case ALLOW_SYNC_KEYBOARD:
		if (keyboard_held && server->keyboard_grab.client == index) {
			server->keyboard_freeze = X11_FREEZE_NONE;
			server->keyboard_sync = 1;
		}
		break;
	case ALLOW_REPLAY_KEYBOARD:
		if (keyboard_held && server->keyboard_grab.client == index && server->keyboard_replay_valid)
			input_replay_keyboard(server);
		break;
	case ALLOW_SYNC_BOTH:
		if (pointer_held && keyboard_held) {
			server->pointer_freeze = X11_FREEZE_NONE;
			server->keyboard_freeze = X11_FREEZE_NONE;
			server->both_sync = 1;
		}
		break;
	default:
		/* AsyncBoth: whichever the client froze. */
		if (pointer_held)
			server->pointer_freeze = X11_FREEZE_NONE;
		if (keyboard_held)
			server->keyboard_freeze = X11_FREEZE_NONE;
		break;
	}

	/* Succeeded: the events the thaw let go are handled. */
	input_drain(server);
	return 0U;
}

/*
 * ReplayPointer: the pointer's grab ends and the button it froze on is
 * handled again, the grab's window and its ancestors starting no passive
 * grab this time.
 */
static void
input_replay_pointer(
	struct x11server *server)
{
	struct x11_device_event event;
	struct x11_window *ignore;
	uint8_t physical;
	uint8_t logical;

	/* The event, and the window whose passive grabs it passes over. */
	event = server->pointer_replay;
	server->pointer_replay_valid = 0;
	ignore = x11_window_find(server, server->pointer_grab.window);

	/* The button is as it was before the event. */
	physical = event.detail;
	if (physical >= 1U && physical <= X11_BUTTONS) {
		logical = server->pressed_as[physical - 1U];
		if (event.pressed) {
			server->physical_buttons = (uint16_t)(server->physical_buttons & ~(1U << (physical - 1U)));
			if (logical != 0U)
				server->buttons = (uint16_t)(server->buttons & ~(1U << (logical - 1U)));
		} else {
			logical = server->button_map[physical - 1U];
			server->physical_buttons = (uint16_t)(server->physical_buttons | (1U << (physical - 1U)));
			server->pressed_as[physical - 1U] = logical;
			if (logical != 0U)
				server->buttons = (uint16_t)(server->buttons | (1U << (logical - 1U)));
		}
	}

	/* The grab ends without its own release. */
	input_pointer_grab_end(server, event.time);

	/* Succeeded: handled again. */
	input_handle(server, &event, ignore);
}

/*
 * ReplayKeyboard: the keyboard's grab ends and the key it froze on is
 * handled again, the grab's window and its ancestors starting no passive
 * grab this time.
 */
static void
input_replay_keyboard(
	struct x11server *server)
{
	struct x11_device_event event;
	struct x11_window *ignore;
	int locking;

	/* The event, and the window whose passive grabs it passes over. */
	event = server->keyboard_replay;
	server->keyboard_replay_valid = 0;
	ignore = x11_window_find(server, server->keyboard_grab.window);

	/* The key is as it was before the event. */
	input_key_set(server, event.detail, !event.pressed);
	locking = input_locking(server, event.detail);
	if (event.pressed && locking)
		input_lock_toggle(server, event.detail);

	/* The grab ends without its own release. */
	input_keyboard_grab_end(server);

	/* Succeeded: handled again. */
	input_handle(server, &event, ignore);
}

/*
 * QueryPointer: the pointer on the root window (the system's own place
 * when the backend tells it, so the pointer is followed outside the X
 * windows) and in a window, the window's child it is in, and the state.
 */
static unsigned
input_query_pointer(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	struct x11_window *under;
	uint8_t reply[32];
	uint32_t child;
	int x;
	int y;
	int error;
	int msb;

	/* The window. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The place: the system's when the backend tells it, else the server's. */
	x = server->pointer_x;
	y = server->pointer_y;
	error = 1;
	if (server->ops->pointer != NULL)
		error = server->ops->pointer(server->backend, &x, &y);
	if (error != 0) {
		x = server->pointer_x;
		y = server->pointer_y;
	}

	/* The window's child the pointer is in (None away from the server's place, or in none). */
	child = 0U;
	if (x == server->pointer_x && y == server->pointer_y) {
		under = x11_input_window_under(server);
		child = x11_event_child_toward(window, under);
	}

	/* Succeeded: on this screen, the places, and the state. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 1U;
	x11_write32(reply + 8, X11_ROOT_XID, msb);
	x11_write32(reply + 12, child, msb);
	x11_write16(reply + 16, (uint16_t)x, msb);
	x11_write16(reply + 18, (uint16_t)y, msb);
	x11_write16(reply + 20, (uint16_t)(x - window->x), msb);
	x11_write16(reply + 22, (uint16_t)(y - window->y), msb);
	x11_write16(reply + 24, x11_input_state(server), msb);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetMotionEvents: the server keeps no motion history, so none. */
static unsigned
input_get_motion_events(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	uint8_t reply[32];

	/* The window and the times. */
	client = &server->clients[index];
	if (length != 16U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Succeeded: no events. */
	memset(reply, 0, sizeof(reply));
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * WarpPointer: the pointer moved by an offset or to a place in a window,
 * when it is in the source rectangle.  The system's pointer is moved only
 * into a window of the client's own (a remote client must not move the
 * user's pointer elsewhere) and only when the backend can; the server
 * then hears the move as any other.
 */
static unsigned
input_warp_pointer(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *source;
	struct x11_window *destination;
	struct x11_window *target;
	uint32_t id;
	int source_x;
	int source_y;
	int source_width;
	int source_height;
	int x;
	int y;
	int error;
	int msb;

	/* The source and destination windows (None: none). */
	msb = server->clients[index].order;
	if (length != 24U)
		return X11_BAD_LENGTH;
	source = NULL;
	id = x11_read32(request + 4, msb);
	if (id != 0U) {
		source = x11_window_find(server, id);
		if (source == NULL)
			return X11_BAD_WINDOW;
	}
	destination = NULL;
	id = x11_read32(request + 8, msb);
	if (id != 0U) {
		destination = x11_window_find(server, id);
		if (destination == NULL)
			return X11_BAD_WINDOW;
	}

	/* The pointer must be in the source rectangle (a width or height of 0 reaching the window's edge). */
	if (source != NULL) {
		source_x = source->x + (int16_t)x11_read16(request + 12, msb);
		source_y = source->y + (int16_t)x11_read16(request + 14, msb);
		source_width = x11_read16(request + 16, msb);
		source_height = x11_read16(request + 18, msb);
		if (source_width == 0)
			source_width = source->x + source->width - source_x;
		if (source_height == 0)
			source_height = source->y + source->height - source_y;
		if (server->pointer_x < source_x ||
		    server->pointer_y < source_y ||
		    server->pointer_x >= source_x + source_width ||
		    server->pointer_y >= source_y + source_height)
			return 0U;
	}

	/* The place: in the destination, or moved by the offset; within the root window. */
	x = server->pointer_x + (int16_t)x11_read16(request + 20, msb);
	y = server->pointer_y + (int16_t)x11_read16(request + 22, msb);
	if (destination != NULL) {
		x = destination->x + (int16_t)x11_read16(request + 20, msb);
		y = destination->y + (int16_t)x11_read16(request + 22, msb);
	}
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= (int)server->width)
		x = (int)server->width - 1;
	if (y >= (int)server->height)
		y = (int)server->height - 1;

	/* Only into a top-level window of the client's own, and only through the backend. */
	target = x11_window_at(server, x, y);
	target = x11_window_top_level(target);
	if (target == server->root || target->owner != index)
		return 0U;
	if (server->ops->warp == NULL)
		return 0U;
	error = server->ops->warp(server->backend, x, y);
	if (error != 0)
		return 0U;

	/* Succeeded: the server hears the move. */
	x11_input_motion(server, server->now, x, y);
	return 0U;
}

/*
 * SetInputFocus: the keyboard's focus to a viewable window, PointerRoot
 * or None, with where it reverts to; a time out of order does nothing.
 */
static unsigned
input_set_input_focus(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	uint32_t focus;
	uint32_t time;
	int viewable;
	int valid;
	int msb;

	/* The revert-to, the focus and the time. */
	msb = server->clients[index].order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	if (request[1] > REVERT_PARENT)
		return X11_BAD_VALUE;
	focus = x11_read32(request + 4, msb);
	time = x11_read32(request + 8, msb);

	/* A window must be there and be seen. */
	if (focus != X11_FOCUS_NONE && focus != X11_FOCUS_POINTER_ROOT) {
		window = x11_window_find(server, focus);
		if (window == NULL)
			return X11_BAD_WINDOW;
		viewable = x11_window_viewable(window);
		if (!viewable)
			return X11_BAD_MATCH;
	}

	/* A time before the last change of the focus, or after now, does nothing. */
	valid = input_time_valid(server, time, server->focus_time);
	if (!valid)
		return 0U;

	/* Succeeded: the focus. */
	time = input_time(server, time);
	input_set_focus(server, focus, request[1], time);
	return 0U;
}

/* GetInputFocus: the focus (a window, PointerRoot or None) and where it reverts to. */
static unsigned
input_get_input_focus(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	reply[1] = server->focus_revert;
	x11_write32(reply + 8, server->focus, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* QueryKeymap: the keys down, a bit for each keycode. */
static unsigned
input_query_keymap(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[40];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the bits (two words past the reply's first 32 bytes). */
	memset(reply, 0, sizeof(reply));
	memcpy(reply + 8, server->keys_down, X11_KEY_BYTES);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * ChangeKeyboardMapping: the keysyms of some keycodes (keysyms past those
 * the server keeps must be NoSymbol), and MappingNotify to every client.
 */
static unsigned
input_change_keyboard_mapping(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t keysym;
	unsigned count;
	unsigned first;
	unsigned per;
	unsigned slot;
	unsigned column;
	int msb;

	/* The keycodes, the keysyms each, and room for them all. */
	msb = server->clients[index].order;
	if (length < 8U)
		return X11_BAD_LENGTH;
	count = request[1];
	first = request[4];
	per = request[5];
	if (length != 8U + (size_t)count * per * 4U)
		return X11_BAD_LENGTH;
	if (first < KEYCODE_MIN || first + count - 1U > KEYCODE_MAX || per == 0U)
		return X11_BAD_VALUE;

	/* Keysyms past those kept must be NoSymbol. */
	for (slot = 0U; slot < count; slot++) {
		for (column = X11_KEYSYMS_MAX; column < per; column++) {
			keysym = x11_read32(request + 8 + ((size_t)slot * per + column) * 4U, msb);
			if (keysym != 0U)
				return X11_BAD_ALLOC;
		}
	}

	/* Each keycode's keysyms. */
	for (slot = 0U; slot < count; slot++) {
		memset(server->keysyms[first + slot], 0, sizeof(server->keysyms[0]));
		for (column = 0U; column < per && column < X11_KEYSYMS_MAX; column++)
			server->keysyms[first + slot][column] = x11_read32(request + 8 + ((size_t)slot * per + column) * 4U, msb);
	}
	if (per > server->keysyms_per_keycode && per <= X11_KEYSYMS_MAX)
		server->keysyms_per_keycode = per;
	if (per > X11_KEYSYMS_MAX)
		server->keysyms_per_keycode = X11_KEYSYMS_MAX;

	/* Succeeded: every client hears of it. */
	input_mapping_notify(server, MAPPING_KEYBOARD, (uint8_t)first, (uint8_t)count);
	return 0U;
}

/* GetKeyboardMapping: the keysyms of some keycodes. */
static unsigned
input_get_keyboard_mapping(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32 + 4 * (KEYCODE_MAX - KEYCODE_MIN + 1U) * X11_KEYSYMS_MAX];
	struct x11_client *client;
	unsigned first;
	unsigned count;
	unsigned per;
	unsigned slot;
	unsigned column;
	size_t offset;

	/* The first keycode and the count, within the keyboard's keycodes. */
	client = &server->clients[index];
	if (length != 8U)
		return X11_BAD_LENGTH;
	first = request[4];
	count = request[5];
	if (first < KEYCODE_MIN || first + count > KEYCODE_MAX + 1U)
		return X11_BAD_VALUE;

	/* The keysyms each keycode carries, column by column. */
	per = server->keysyms_per_keycode;
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)per;
	offset = 32U;
	for (slot = 0U; slot < count; slot++) {
		for (column = 0U; column < per; column++) {
			x11_write32(reply + offset, server->keysyms[first + slot][column], client->order);
			offset += 4U;
		}
	}

	/* Succeeded: the reply. */
	x11_reply(server, client, reply, offset);
	return 0U;
}

/*
 * ChangeKeyboardControl: the click, the bell, the LEDs and auto-repeat
 * (kept for GetKeyboardControl; the system's keyboard is its own).  The
 * values are all checked before any is taken.
 */
static unsigned
input_change_keyboard_control(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	int32_t values[CONTROL_COUNT];
	const uint8_t *value;
	uint32_t mask;
	unsigned bit;
	unsigned led;
	unsigned key;
	int fits;
	int msb;

	/* The mask and one value for each of its bits. */
	msb = server->clients[index].order;
	if (length < 8U)
		return X11_BAD_LENGTH;
	mask = x11_read32(request + 4, msb);
	if ((mask >> CONTROL_COUNT) != 0U)
		return X11_BAD_VALUE;
	fits = x11_values_fit(mask, 8U, length);
	if (!fits)
		return X11_BAD_LENGTH;

	/* The values, each in the low bits of its word. */
	memset(values, 0, sizeof(values));
	value = request + 8;
	for (bit = 0U; bit < CONTROL_COUNT; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;
		values[bit] = (int32_t)x11_read32(value, msb);
		value += 4;
	}

	/* The click and the bell's volume: -1 (the default) to 100; its pitch and length: -1 or more. */
	if ((mask & (1U << CONTROL_KEY_CLICK)) != 0U) {
		values[CONTROL_KEY_CLICK] = (int8_t)values[CONTROL_KEY_CLICK];
		if (values[CONTROL_KEY_CLICK] < -1 || values[CONTROL_KEY_CLICK] > 100)
			return X11_BAD_VALUE;
	}
	if ((mask & (1U << CONTROL_BELL_PERCENT)) != 0U) {
		values[CONTROL_BELL_PERCENT] = (int8_t)values[CONTROL_BELL_PERCENT];
		if (values[CONTROL_BELL_PERCENT] < -1 || values[CONTROL_BELL_PERCENT] > 100)
			return X11_BAD_VALUE;
	}
	if ((mask & (1U << CONTROL_BELL_PITCH)) != 0U) {
		values[CONTROL_BELL_PITCH] = (int16_t)values[CONTROL_BELL_PITCH];
		if (values[CONTROL_BELL_PITCH] < -1)
			return X11_BAD_VALUE;
	}
	if ((mask & (1U << CONTROL_BELL_DURATION)) != 0U) {
		values[CONTROL_BELL_DURATION] = (int16_t)values[CONTROL_BELL_DURATION];
		if (values[CONTROL_BELL_DURATION] < -1)
			return X11_BAD_VALUE;
	}

	/* An LED (1 to 32) only with its mode; a key only with its auto-repeat mode. */
	led = (uint8_t)values[CONTROL_LED];
	key = (uint8_t)values[CONTROL_KEY];
	if ((mask & (1U << CONTROL_LED)) != 0U && (mask & (1U << CONTROL_LED_MODE)) == 0U)
		return X11_BAD_MATCH;
	if ((mask & (1U << CONTROL_LED)) != 0U && (led < 1U || led > LED_COUNT))
		return X11_BAD_VALUE;
	if ((mask & (1U << CONTROL_LED_MODE)) != 0U && (uint8_t)values[CONTROL_LED_MODE] > 1U)
		return X11_BAD_VALUE;
	if ((mask & (1U << CONTROL_KEY)) != 0U && (mask & (1U << CONTROL_AUTO_REPEAT)) == 0U)
		return X11_BAD_MATCH;
	if ((mask & (1U << CONTROL_KEY)) != 0U && key < KEYCODE_MIN)
		return X11_BAD_VALUE;
	if ((mask & (1U << CONTROL_AUTO_REPEAT)) != 0U && (uint8_t)values[CONTROL_AUTO_REPEAT] > REPEAT_DEFAULT)
		return X11_BAD_VALUE;

	/* The click and the bell (-1: the default). */
	if ((mask & (1U << CONTROL_KEY_CLICK)) != 0U) {
		server->key_click_percent = (int8_t)values[CONTROL_KEY_CLICK];
		if (values[CONTROL_KEY_CLICK] < 0)
			server->key_click_percent = DEFAULT_KEY_CLICK;
	}
	if ((mask & (1U << CONTROL_BELL_PERCENT)) != 0U) {
		server->bell_percent = (int8_t)values[CONTROL_BELL_PERCENT];
		if (values[CONTROL_BELL_PERCENT] < 0)
			server->bell_percent = DEFAULT_BELL_PERCENT;
	}
	if ((mask & (1U << CONTROL_BELL_PITCH)) != 0U) {
		server->bell_pitch = (int16_t)values[CONTROL_BELL_PITCH];
		if (values[CONTROL_BELL_PITCH] < 0)
			server->bell_pitch = DEFAULT_BELL_PITCH;
	}
	if ((mask & (1U << CONTROL_BELL_DURATION)) != 0U) {
		server->bell_duration = (int16_t)values[CONTROL_BELL_DURATION];
		if (values[CONTROL_BELL_DURATION] < 0)
			server->bell_duration = DEFAULT_BELL_DURATION;
	}

	/* The LEDs: one, or all. */
	if ((mask & (1U << CONTROL_LED_MODE)) != 0U) {
		if ((mask & (1U << CONTROL_LED)) != 0U && values[CONTROL_LED_MODE] != 0) {
			server->leds |= 1U << (led - 1U);
		} else if ((mask & (1U << CONTROL_LED)) != 0U) {
			server->leds &= ~(1U << (led - 1U));
		} else if (values[CONTROL_LED_MODE] != 0) {
			server->leds = 0xffffffffU;
		} else {
			server->leds = 0U;
		}
	}

	/* Auto-repeat: one key's, or the keyboard's (Default: on). */
	if ((mask & (1U << CONTROL_AUTO_REPEAT)) != 0U) {
		if ((mask & (1U << CONTROL_KEY)) != 0U && values[CONTROL_AUTO_REPEAT] == REPEAT_OFF) {
			server->auto_repeats[key / 8U] = (uint8_t)(server->auto_repeats[key / 8U] & ~(1U << (key % 8U)));
		} else if ((mask & (1U << CONTROL_KEY)) != 0U) {
			server->auto_repeats[key / 8U] = (uint8_t)(server->auto_repeats[key / 8U] | (1U << (key % 8U)));
		} else if (values[CONTROL_AUTO_REPEAT] == REPEAT_OFF) {
			server->global_auto_repeat = 0U;
		} else {
			server->global_auto_repeat = 1U;
		}
	}

	/* Succeeded: kept. */
	return 0U;
}

/* GetKeyboardControl: the click, the bell, the LEDs and auto-repeat. */
static unsigned
input_get_keyboard_control(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[52];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	reply[1] = server->global_auto_repeat;
	x11_write32(reply + 8, server->leds, client->order);
	reply[12] = (uint8_t)server->key_click_percent;
	reply[13] = (uint8_t)server->bell_percent;
	x11_write16(reply + 14, (uint16_t)server->bell_pitch, client->order);
	x11_write16(reply + 16, (uint16_t)server->bell_duration, client->order);
	memcpy(reply + 20, server->auto_repeats, X11_KEY_BYTES);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* Bell: the backend rings at -100 to 100 percent of the bell's volume. */
static unsigned
input_bell(
	struct x11server *server,
	const uint8_t *request,
	size_t length)
{
	int percent;

	/* The volume. */
	if (length != 4U)
		return X11_BAD_LENGTH;
	percent = (int8_t)request[1];
	if (percent < -100 || percent > 100)
		return X11_BAD_VALUE;

	/* Succeeded: rung, by a backend that can. */
	if (server->ops->bell != NULL)
		server->ops->bell(server->backend, percent);
	return 0U;
}

/* ChangePointerControl: the acceleration and its threshold (kept for GetPointerControl). */
static unsigned
input_change_pointer_control(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	int numerator;
	int denominator;
	int threshold;
	int msb;

	/* The values and whether each is to be taken. */
	msb = server->clients[index].order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	if (request[10] > 1U || request[11] > 1U)
		return X11_BAD_VALUE;
	numerator = (int16_t)x11_read16(request + 4, msb);
	denominator = (int16_t)x11_read16(request + 6, msb);
	threshold = (int16_t)x11_read16(request + 8, msb);

	/* The acceleration (-1: the default; no zero denominator). */
	if (request[10] != 0U) {
		if (numerator < -1 || denominator < -1 || denominator == 0)
			return X11_BAD_VALUE;
		if (numerator == -1)
			numerator = DEFAULT_ACCELERATION;
		if (denominator == -1)
			denominator = 1;
	}

	/* The threshold (-1: the default). */
	if (request[11] != 0U) {
		if (threshold < -1)
			return X11_BAD_VALUE;
		if (threshold == -1)
			threshold = DEFAULT_THRESHOLD;
	}

	/* Succeeded: kept. */
	if (request[10] != 0U) {
		server->acceleration_numerator = (int16_t)numerator;
		server->acceleration_denominator = (int16_t)denominator;
	}
	if (request[11] != 0U)
		server->acceleration_threshold = (int16_t)threshold;
	return 0U;
}

/* GetPointerControl: the acceleration and its threshold. */
static unsigned
input_get_pointer_control(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, (uint16_t)server->acceleration_numerator, client->order);
	x11_write16(reply + 10, (uint16_t)server->acceleration_denominator, client->order);
	x11_write16(reply + 12, (uint16_t)server->acceleration_threshold, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * SetPointerMapping: the logical button of each physical one (0: off, the
 * others each once).  Busy while a button whose map changes is down.
 */
static unsigned
input_set_pointer_mapping(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];
	unsigned count;
	unsigned button;
	unsigned other;
	unsigned status;

	/* A map of every button. */
	client = &server->clients[index];
	if (length < 4U)
		return X11_BAD_LENGTH;
	count = request[1];
	if (length != 4U + (((size_t)count + 3U) & ~(size_t)3U))
		return X11_BAD_LENGTH;
	if (count != X11_BUTTONS)
		return X11_BAD_VALUE;

	/* No logical button twice. */
	for (button = 0U; button < count; button++) {
		if (request[4 + button] == 0U)
			continue;
		for (other = button + 1U; other < count; other++) {
			if (request[4 + other] == request[4 + button])
				return X11_BAD_VALUE;
		}
	}

	/* Busy while a button whose map changes is down; else the map, which every client hears of. */
	status = MAPPING_SUCCESS;
	for (button = 0U; button < count; button++) {
		if (request[4 + button] != server->button_map[button] && (server->physical_buttons & (1U << button)) != 0U)
			status = MAPPING_BUSY;
	}
	if (status == MAPPING_SUCCESS) {
		memcpy(server->button_map, request + 4, X11_BUTTONS);
		input_mapping_notify(server, MAPPING_POINTER, 0U, 0U);
	}

	/* Succeeded: the status. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)status;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetPointerMapping: the logical button of each physical one. */
static unsigned
input_get_pointer_mapping(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32 + ((X11_BUTTONS + 3U) & ~3U)];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the map. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)X11_BUTTONS;
	memcpy(reply + 32, server->button_map, X11_BUTTONS);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * SetModifierMapping: the keycodes of each modifier (0: none), every
 * client hearing of it.  Busy while a key of a modifier that changes is
 * down; Failed for more keycodes a modifier than the server keeps.
 */
static unsigned
input_set_modifier_mapping(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];
	unsigned per;
	unsigned slot;
	unsigned status;
	unsigned modifier;
	int busy;

	/* The keycodes, each 0 or one the keyboard has. */
	client = &server->clients[index];
	if (length < 4U)
		return X11_BAD_LENGTH;
	per = request[1];
	if (length != 4U + (size_t)per * X11_MODIFIERS)
		return X11_BAD_LENGTH;
	for (slot = 0U; slot < per * X11_MODIFIERS; slot++) {
		if (request[4 + slot] != 0U && request[4 + slot] < KEYCODE_MIN)
			return X11_BAD_VALUE;
	}

	/* Too many to keep, or a key of a changing modifier down. */
	status = MAPPING_SUCCESS;
	busy = input_modifiers_busy(server, request + 4, per);
	if (per > X11_MODIFIER_KEYS_MAX) {
		status = MAPPING_FAILED;
	} else if (busy) {
		status = MAPPING_BUSY;
	}

	/* The map, which every client hears of. */
	if (status == MAPPING_SUCCESS) {
		memset(server->modifier_keys, 0, sizeof(server->modifier_keys));
		for (modifier = 0U; modifier < X11_MODIFIERS; modifier++) {
			for (slot = 0U; slot < per; slot++)
				server->modifier_keys[modifier][slot] = request[4 + modifier * per + slot];
		}
		server->keys_per_modifier = per;
		input_mapping_notify(server, MAPPING_MODIFIER, 0U, 0U);
	}

	/* Succeeded: the status. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)status;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetModifierMapping: the keycodes of each modifier. */
static unsigned
input_get_modifier_mapping(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32 + X11_MODIFIERS * X11_MODIFIER_KEYS_MAX];
	unsigned per;
	unsigned modifier;

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* The keycodes, modifier by modifier. */
	per = server->keys_per_modifier;
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)per;
	for (modifier = 0U; modifier < X11_MODIFIERS; modifier++)
		memcpy(reply + 32 + modifier * per, server->modifier_keys[modifier], per);

	/* Succeeded: the reply. */
	x11_reply(server, client, reply, 32U + (size_t)per * X11_MODIFIERS);
	return 0U;
}

/*
 * Tells whether a new modifier map would change a modifier one of whose
 * keys (old or new) is down.
 */
static int
input_modifiers_busy(
	const struct x11server *server,
	const uint8_t *keycodes,
	unsigned count)
{
	unsigned modifier;
	unsigned slot;
	unsigned keycode;
	int changed;
	int down;

	/* Each modifier. */
	for (modifier = 0U; modifier < X11_MODIFIERS; modifier++) {
		/* Whether its keys change. */
		changed = 0;
		for (slot = 0U; slot < X11_MODIFIER_KEYS_MAX; slot++) {
			keycode = 0U;
			if (slot < count)
				keycode = keycodes[modifier * count + slot];
			if (keycode != server->modifier_keys[modifier][slot])
				changed = 1;
		}
		if (!changed)
			continue;

		/* A key of it, old or new, down. */
		for (slot = 0U; slot < X11_MODIFIER_KEYS_MAX; slot++) {
			down = input_key_down(server, server->modifier_keys[modifier][slot]);
			if (server->modifier_keys[modifier][slot] != 0U && down)
				return 1;
			if (slot >= count)
				continue;
			down = input_key_down(server, keycodes[modifier * count + slot]);
			if (keycodes[modifier * count + slot] != 0U && down)
				return 1;
		}
	}

	/* Nothing down changes. */
	return 0;
}

/*
 * Makes a backend's map the server's: the keysyms (two to a keycode when
 * no key has a second group, else four) and the modifiers' keys.
 */
static void
input_keymap_apply(
	struct x11server *server,
	const struct x11_keymap *keymap)
{
	unsigned keycode;
	unsigned column;
	unsigned modifier;
	unsigned slot;
	unsigned per;

	/* The keysyms, and how many columns any key fills. */
	memset(server->keysyms, 0, sizeof(server->keysyms));
	per = 2U;
	for (keycode = KEYCODE_MIN; keycode < X11_KEYMAP_KEYCODES; keycode++) {
		for (column = 0U; column < X11_KEYMAP_COLUMNS; column++) {
			server->keysyms[keycode][column] = keymap->keysyms[keycode][column];
			if (keymap->keysyms[keycode][column] != 0U && column >= per)
				per = X11_KEYMAP_COLUMNS;
		}
	}
	server->keysyms_per_keycode = per;

	/* The modifiers' keys, and how many the fullest modifier has. */
	memset(server->modifier_keys, 0, sizeof(server->modifier_keys));
	per = 0U;
	for (modifier = 0U; modifier < X11_MODIFIERS; modifier++) {
		for (slot = 0U; slot < X11_KEYMAP_MODIFIER_KEYS; slot++) {
			server->modifier_keys[modifier][slot] = keymap->modifiers[modifier][slot];
			if (keymap->modifiers[modifier][slot] != 0U && slot + 1U > per)
				per = slot + 1U;
		}
	}
	server->keys_per_modifier = per;
}

/*
 * Tells whether a top-level window takes the focus from its window
 * manager: not an override-redirect window, nor one whose WM_HINTS say it
 * takes no input.
 */
static int
input_accepts_focus(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_property *hints;
	uint32_t flags;
	uint32_t input;

	/* A menu or a tooltip. */
	(void)server;
	if (window->override_redirect)
		return 0;

	/* WM_HINTS: its flags, and the input field when they say it is set (32-bit values, least significant byte first). */
	hints = x11_property_find(window, ATOM_WM_HINTS);
	if (hints == NULL || hints->format != 32U || hints->length < 8U)
		return 1;
	flags = x11_read32(hints->data, 0);
	input = x11_read32(hints->data + 4, 0);
	if ((flags & WM_HINTS_INPUT) != 0U && input == 0U)
		return 0;

	/* Succeeded: it takes the focus. */
	return 1;
}
