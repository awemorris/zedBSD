/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: the keyboard and the pointer of the X
 * windows as X's input.
 *
 * A key is named by its place on the keyboard (the scan code, as an evdev
 * code) whatever the layout; the core is given the X keycode of the key
 * (the evdev code plus 8) and makes the characters' keysyms and the
 * modifiers from the layout's map (layout.c).  The pointer's place on the
 * root window is counted from the X window's place the core gave, the
 * buttons are X's 1 to 9 (the wheels 4 to 7), and a button held captures
 * the pointer, as X's implicit grab does; an X client's grab keeps it too.
 * A paste or a copy (by key or by button) is noted for the clipboard's
 * policy.  The keys an input method took and the characters sent as keys
 * are not X's keys: their text reaches X through ime.c.
 */

#include "userland/desktop/xserver/win32/internal.h"
#include "userland/desktop/xserver/win32/scale.h"

#include <errno.h>
#include <imm.h>
#include <windowsx.h>

/* The wheel's notch (WHEEL_DELTA). */
#define INPUT_NOTCH	120

/* The bits of a key message's lParam: the scan code, the extended bit and the previous state (a repeat). */
#define INPUT_SCAN_SHIFT	16U
#define INPUT_SCAN_MASK		0xFFU
#define INPUT_EXTENDED_BIT	24U
#define INPUT_REPEAT_BIT	30U

static void input_method_key(struct x11_surface *surface, unsigned scan, int extended, int pressed, DWORD time);
static int input_fake_control(struct x11_surface *surface, int pressed);
static void input_note_action(struct x11_win32 *backend, uint32_t code, DWORD time);
static void input_send_key(struct x11_win32 *backend, uint32_t code, int pressed, DWORD time);
static void input_modifier(struct x11_win32 *backend, uint32_t code, int pressed);
static void input_frame(struct x11_surface *surface, unsigned button, int pressed, DWORD time);
static void input_wheel(struct x11_surface *surface, int *remainder, int delta, unsigned positive, unsigned negative, DWORD time);
static void input_enter(struct x11_surface *surface);

/*
 * Turns a key message of an X window into X's input: the key's evdev
 * code from its scan code, the left Ctrl Windows makes up before AltGr
 * dropped, a held key's repeat sent as X does (a release and a press),
 * Alt+F4 kept for closing the window.
 */
void
win32_input_key(
	struct x11_surface *surface,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	struct x11_win32 *backend;
	unsigned scan;
	int extended;
	int pressed;
	int repeat;
	uint32_t code;
	DWORD time;
	int fake;

	/* Reads the key, whether it went down, and whether it was already down. */
	backend = surface->backend;
	scan = (unsigned)(((uint32_t)lparam >> INPUT_SCAN_SHIFT) & INPUT_SCAN_MASK);
	extended = (int)(((uint32_t)lparam >> INPUT_EXTENDED_BIT) & 1U);
	pressed = 0;
	if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
		pressed = 1;
	repeat = 0;
	if (pressed && (((uint32_t)lparam >> INPUT_REPEAT_BIT) & 1U) != 0)
		repeat = 1;
	time = (DWORD)GetMessageTime();

	/* A character sent as a key is typed as text (its WM_CHAR); X hears no key. */
	if (wparam == VK_PACKET)
		return;

	/* A key the input method took is the input method's (only a key X holds is let go). */
	if (wparam == VK_PROCESSKEY) {
		input_method_key(surface, scan, extended, pressed, time);
		return;
	}

	/* The key's place on the keyboard; a key X cannot name is dropped. */
	code = x11_win32_scan_to_evdev(scan, extended, (unsigned)wparam);
	if (code == 0 || code > X11_WIN32_EVDEV_MAX)
		return;

	/* The left Ctrl Windows sends before AltGr is not a key the user pressed. */
	if (code == WIN32_KEY_LEFTCTRL && backend->keymap.altgr) {
		fake = input_fake_control(surface, pressed);
		if (fake)
			return;
	}

	/* Alt+F4 closes the window, as on every Windows window; X does not get the F4. */
	if (pressed && code == WIN32_KEY_F4 && (backend->modifiers & X11_ALT_MASK) != 0) {
		if (!repeat)
			SendMessageW(surface->hwnd, WM_CLOSE, 0, 0);
		return;
	}

	/* Notes a paste or a copy made with the keyboard. */
	if (pressed && !repeat)
		input_note_action(backend, code, time);

	/* A key held down repeats as X repeats: released and pressed again. */
	if (repeat && backend->held[code]) {
		input_send_key(backend, code, 0, time);
		input_send_key(backend, code, 1, time);
		return;
	}

	/* A release of a key not held (its press went elsewhere) is dropped. */
	if (!pressed && !backend->held[code])
		return;

	/* Succeeded: the core hears the key. */
	input_send_key(backend, code, pressed, time);
}

/*
 * Lets go of every key held, as releases (the window lost the keyboard,
 * and the keys' releases will go to another window).
 */
void
win32_input_release_keys(
	struct x11_win32 *backend)
{
	uint32_t code;
	DWORD time;

	/* Releases each key held. */
	time = (DWORD)GetMessageTime();
	for (code = 0; code <= X11_WIN32_EVDEV_MAX; code++) {
		if (backend->held[code])
			input_send_key(backend, code, 0, time);
	}

	/* No left Ctrl is pending as AltGr's any more. */
	backend->altgr_control = 0;
}

/*
 * Turns a pointer message of an X window into X's input: a motion, a
 * button (1 to 3, and 8 and 9 for the side buttons), or the wheels'
 * notches as presses and releases of buttons 4 to 7.
 */
void
win32_input_pointer(
	struct x11_surface *surface,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	struct x11_win32 *backend;
	DWORD time;
	int local_x;
	int local_y;
	unsigned button;
	int pressed;

	/* The wheels: the pointer stays where it was, and each notch is a click of a wheel's button. */
	backend = surface->backend;
	time = (DWORD)GetMessageTime();
	if (message == WM_MOUSEWHEEL) {
		input_wheel(surface, &backend->wheel, GET_WHEEL_DELTA_WPARAM(wparam), WIN32_BUTTON_WHEEL_UP,
		    WIN32_BUTTON_WHEEL_DOWN, time);
		return;
	}
	if (message == WM_MOUSEHWHEEL) {
		input_wheel(surface, &backend->hwheel, GET_WHEEL_DELTA_WPARAM(wparam), WIN32_BUTTON_WHEEL_RIGHT,
		    WIN32_BUTTON_WHEEL_LEFT, time);
		return;
	}

	/* The pointer's place on the root window, counted from the X window's place. */
	local_x = GET_X_LPARAM(lparam);
	local_y = GET_Y_LPARAM(lparam);
	backend->pointer_x = surface->x + x11_win32_scale_to_x(local_x, backend->scale);
	backend->pointer_y = surface->y + x11_win32_scale_to_x(local_y, backend->scale);
	input_enter(surface);

	/* Which button changed, and how. */
	button = 0;
	pressed = 0;
	switch (message) {
	case WM_LBUTTONDOWN:
		button = WIN32_BUTTON_LEFT;
		pressed = 1;
		break;
	case WM_LBUTTONUP:
		button = WIN32_BUTTON_LEFT;
		break;
	case WM_MBUTTONDOWN:
		button = WIN32_BUTTON_MIDDLE;
		pressed = 1;
		break;
	case WM_MBUTTONUP:
		button = WIN32_BUTTON_MIDDLE;
		break;
	case WM_RBUTTONDOWN:
		button = WIN32_BUTTON_RIGHT;
		pressed = 1;
		break;
	case WM_RBUTTONUP:
		button = WIN32_BUTTON_RIGHT;
		break;
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
		button = WIN32_BUTTON_BACK;
		if (GET_XBUTTON_WPARAM(wparam) == XBUTTON2)
			button = WIN32_BUTTON_FORWARD;
		if (message == WM_XBUTTONDOWN)
			pressed = 1;
		break;
	default:
		break;
	}

	/* A middle click pastes and the end of a left drag copies, for the clipboard's policy. */
	if (button == WIN32_BUTTON_MIDDLE && pressed) {
		backend->paste_time = time;
		backend->pasted = 1;
	}
	if (button == WIN32_BUTTON_LEFT && !pressed) {
		backend->copy_time = time;
		backend->copied = 1;
	}

	/* The first button down captures the pointer, the last one up lets it go (X's implicit grab) unless a grab keeps it. */
	if (button != 0 && pressed) {
		if (backend->buttons_held == 0 && !backend->grab_capture)
			SetCapture(surface->hwnd);
		backend->buttons_held++;
	} else if (button != 0 && backend->buttons_held != 0) {
		backend->buttons_held--;
		if (backend->buttons_held == 0 && !backend->grab_capture)
			ReleaseCapture();
	}

	/* Succeeded: the core hears the motion or the button. */
	input_frame(surface, button, pressed, time);
}

/*
 * Follows the pointer out of a window: when it went to no other X window,
 * the core hears that it left the X windows.
 */
void
win32_input_leave(
	struct x11_surface *surface)
{
	struct x11_win32 *backend;

	/* The pointer went into another X window first. */
	backend = surface->backend;
	if (backend->pointer_surface != surface)
		return;

	/* Succeeded: in no X window now. */
	backend->pointer_surface = NULL;
	if (backend->server != NULL)
		x11server_backend_leave(backend->server, surface->window, 0);
}

/*
 * Keeps the pointer's events on a surface while an X client grabs the
 * pointer (on nonzero), or lets them go (on 0), so that a click outside
 * the X windows reaches the grabbing client (it closes a menu).
 */
void
win32_input_capture(
	void *context,
	struct x11_surface *surface,
	int on)
{
	struct x11_win32 *backend;

	/* Let go, unless a button held keeps it for its own release. */
	backend = context;
	if (!on) {
		backend->grab_capture = 0;
		if (backend->buttons_held == 0)
			ReleaseCapture();
		return;
	}

	/* Succeeded: captured on the surface. */
	if (surface == NULL)
		return;
	backend->grab_capture = 1;
	SetCapture(surface->hwnd);
}

/*
 * Follows the pointer's capture leaving a window (WM_CAPTURECHANGED): a
 * capture the backend kept for an X client (a grab, or the buttons held)
 * that went to no X window was taken by another program (it was
 * activated): the buttons held are let go where the pointer was last, and
 * the core ends the client's grab.  A capture the backend let go itself,
 * or moved to another X window, is not lost.
 */
void
win32_input_capture_changed(
	struct x11_surface *surface,
	HWND taker)
{
	struct x11_win32 *backend;
	struct x11_surface *other;
	unsigned button;
	DWORD time;

	/* The backend kept no capture (it let it go). */
	backend = surface->backend;
	if (!backend->grab_capture && backend->buttons_held == 0)
		return;

	/* Another X window took it. */
	for (other = backend->surfaces; other != NULL; other = other->next) {
		if (taker != NULL && other->hwnd == taker)
			return;
	}

	/* Lost: nothing keeps it now. */
	time = (DWORD)GetMessageTime();
	backend->grab_capture = 0;
	backend->buttons_held = 0;
	win32_log(backend, "X11 WIN32 capture lost window=0x%lx", (unsigned long)surface->window);

	/* The buttons held are let go, ending the implicit grab. */
	for (button = WIN32_BUTTON_LEFT; button <= WIN32_BUTTON_RIGHT; button++) {
		if ((backend->buttons & (1U << (button - 1U))) != 0)
			input_frame(surface, button, 0, time);
	}

	/* Succeeded: the core ends a client's grab. */
	if (backend->server != NULL)
		x11server_backend_capture_lost(backend->server, (uint32_t)time);
}

/*
 * Tells where the system's pointer is on the root window (X's pixels from
 * the virtual screen's corner), inside an X window or not.  Returns 0, or
 * an errno value.
 */
int
win32_input_place(
	void *context,
	int *x,
	int *y)
{
	struct x11_win32 *backend;
	POINT point;
	BOOL found;

	/* The pointer on the screen. */
	backend = context;
	found = GetCursorPos(&point);
	if (!found)
		return EIO;

	/* Succeeded: on the root window. */
	*x = x11_win32_scale_to_x(point.x - backend->virtual_screen.left, backend->scale);
	*y = x11_win32_scale_to_x(point.y - backend->virtual_screen.top, backend->scale);
	return 0;
}

/*
 * Moves the system's pointer to a place on the root window (the core asks
 * only for a place in the asking client's own window).  Returns 0, or an
 * errno value.
 */
int
win32_input_warp(
	void *context,
	int x,
	int y)
{
	struct x11_win32 *backend;
	BOOL moved;
	int screen_x;
	int screen_y;

	/* The place on the screen. */
	backend = context;
	screen_x = backend->virtual_screen.left + x11_win32_scale_to_screen(x, backend->scale);
	screen_y = backend->virtual_screen.top + x11_win32_scale_to_screen(y, backend->scale);

	/* Succeeded: moved there. */
	moved = SetCursorPos(screen_x, screen_y);
	if (!moved)
		return EIO;
	return 0;
}

/*
 * Rings the system's bell (the volume is the system's own; a silent bell
 * at -100 percent is not rung).
 */
void
win32_input_bell(
	void *context,
	int percent)
{
	/* A bell at no volume. */
	(void)context;
	if (percent <= -100)
		return;

	/* Succeeded: the system's default sound. */
	MessageBeep(MB_OK);
}

/*
 * Follows a key an input method took (VK_PROCESSKEY): X hears nothing of
 * its press, which composes text; its release lets go a key X holds (the
 * input method turned on while it was down), so that no key stays down
 * in X.
 */
static void
input_method_key(
	struct x11_surface *surface,
	unsigned scan,
	int extended,
	int pressed,
	DWORD time)
{
	struct x11_win32 *backend;
	UINT virtual_key;
	uint32_t code;

	/* A press composes. */
	backend = surface->backend;
	if (pressed)
		return;

	/* The key's place, with the virtual key the input method was given. */
	virtual_key = ImmGetVirtualKey(surface->hwnd);
	code = x11_win32_scan_to_evdev(scan, extended, (unsigned)virtual_key);
	if (code == 0 || code > X11_WIN32_EVDEV_MAX)
		return;

	/* Succeeded: a key X holds is let go. */
	if (backend->held[code])
		input_send_key(backend, code, 0, time);
}

/*
 * Tells whether a left Ctrl message is the one Windows makes up before
 * AltGr: the next key message is the right Alt with the same time.  The
 * release of a press that was dropped is dropped too.  Returns 1 to drop.
 */
static int
input_fake_control(
	struct x11_surface *surface,
	int pressed)
{
	struct x11_win32 *backend;
	MSG next;
	BOOL found;
	int altgr;

	/* Looks at the next key message without taking it. */
	backend = surface->backend;
	altgr = 0;
	found = PeekMessageW(&next, surface->hwnd, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE);
	if (found && next.wParam == VK_MENU && (((uint32_t)next.lParam >> INPUT_EXTENDED_BIT) & 1U) != 0 &&
	    next.time == (DWORD)GetMessageTime())
		altgr = 1;

	/* A press before AltGr is dropped and remembered. */
	if (pressed) {
		if (altgr)
			backend->altgr_control = 1;
		return altgr;
	}

	/* The release of a dropped press is dropped. */
	if (backend->altgr_control) {
		backend->altgr_control = 0;
		return 1;
	}

	/* A release before AltGr's is dropped too. */
	return altgr;
}

/* Notes a paste (Ctrl+V, Shift+Insert) or a copy (Ctrl+C, Ctrl+Insert), with or without Shift. */
static void
input_note_action(
	struct x11_win32 *backend,
	uint32_t code,
	DWORD time)
{
	/* With Ctrl: C copies, V and nothing else pastes, Insert copies. */
	if ((backend->modifiers & X11_CONTROL_MASK) != 0) {
		if (code == WIN32_KEY_C || code == WIN32_KEY_INSERT) {
			backend->copy_time = time;
			backend->copied = 1;
		} else if (code == WIN32_KEY_V) {
			backend->paste_time = time;
			backend->pasted = 1;
		}
		return;
	}

	/* Shift+Insert pastes. */
	if ((backend->modifiers & X11_SHIFT_MASK) != 0 && code == WIN32_KEY_INSERT) {
		backend->paste_time = time;
		backend->pasted = 1;
	}
}

/*
 * Sends a key to the core by its place on the keyboard: the keycode is
 * the evdev code plus 8, and the core makes the state and the keysyms.
 * The backend keeps which keys are held, and its own Shift, Ctrl and Alt
 * bits.
 */
static void
input_send_key(
	struct x11_win32 *backend,
	uint32_t code,
	int pressed,
	DWORD time)
{
	/* The key is held or not from now on. */
	backend->held[code] = (uint8_t)pressed;
	input_modifier(backend, code, pressed);

	/* Succeeded: the core hears it. */
	if (backend->server != NULL)
		x11server_backend_key(backend->server, (uint8_t)(code + X11_WIN32_KEYCODE_FIRST), pressed, (uint32_t)time);
}

/* Keeps X's modifier bits as the Shift, Ctrl and Alt keys go down and up (AltGr is not Alt). */
static void
input_modifier(
	struct x11_win32 *backend,
	uint32_t code,
	int pressed)
{
	uint16_t bit;

	/* The modifier of the key, if it is one. */
	bit = 0;
	if (code == WIN32_KEY_LEFTSHIFT || code == WIN32_KEY_RIGHTSHIFT) {
		bit = X11_SHIFT_MASK;
	} else if (code == WIN32_KEY_LEFTCTRL || code == WIN32_KEY_RIGHTCTRL) {
		bit = X11_CONTROL_MASK;
	} else if (code == WIN32_KEY_LEFTALT) {
		bit = X11_ALT_MASK;
	} else if (code == WIN32_KEY_RIGHTALT && !backend->keymap.altgr) {
		bit = X11_ALT_MASK;
	}
	if (bit == 0)
		return;

	/* Sets or clears it. */
	if (pressed) {
		backend->modifiers |= bit;
	} else {
		backend->modifiers &= (uint16_t)~bit;
	}
}

/* Sends the pointer's place and at most one button to the core. */
static void
input_frame(
	struct x11_surface *surface,
	unsigned button,
	int pressed,
	DWORD time)
{
	struct x11_win32 *backend;
	struct x11_pointer_frame frame;
	uint16_t bit;

	/* The bit of buttons 1 to 3 (the others have none in the state). */
	backend = surface->backend;
	bit = 0;
	if (button >= WIN32_BUTTON_LEFT && button <= WIN32_BUTTON_RIGHT)
		bit = (uint16_t)(1U << (button - 1U));

	/* The frame: the place, the buttons before and after. */
	frame.x = backend->pointer_x;
	frame.y = backend->pointer_y;
	frame.time = (uint32_t)time;
	frame.buttons_before = backend->buttons;
	frame.button = (uint8_t)button;
	frame.pressed = pressed;
	if (pressed) {
		backend->buttons |= bit;
	} else {
		backend->buttons &= (uint16_t)~bit;
	}
	frame.buttons_after = backend->buttons;

	/* Succeeded: the core hears it. */
	if (backend->server != NULL)
		x11server_backend_pointer(backend->server, &frame);
}

/*
 * Turns a wheel's turn into clicks of its buttons: each whole notch one
 * press and release, the rest kept for the next turn.
 */
static void
input_wheel(
	struct x11_surface *surface,
	int *remainder,
	int delta,
	unsigned positive,
	unsigned negative,
	DWORD time)
{
	/* The wheels act on the window the pointer is in. */
	if (surface->backend->pointer_surface != NULL)
		surface = surface->backend->pointer_surface;

	/* Clicks a button for each whole notch. */
	*remainder += delta;
	while (*remainder >= INPUT_NOTCH) {
		input_frame(surface, positive, 1, time);
		input_frame(surface, positive, 0, time);
		*remainder -= INPUT_NOTCH;
	}
	while (*remainder <= -INPUT_NOTCH) {
		input_frame(surface, negative, 1, time);
		input_frame(surface, negative, 0, time);
		*remainder += INPUT_NOTCH;
	}
}

/*
 * Follows the pointer into a window: the core hears which X window it
 * entered, and Windows is asked to say when it leaves.
 */
static void
input_enter(
	struct x11_surface *surface)
{
	struct x11_win32 *backend;
	TRACKMOUSEEVENT track;

	/* Nothing to do while the pointer stays in the window. */
	backend = surface->backend;
	if (backend->pointer_surface == surface)
		return;

	/* Asks for WM_MOUSELEAVE. */
	backend->pointer_surface = surface;
	track.cbSize = sizeof(track);
	track.dwFlags = TME_LEAVE;
	track.hwndTrack = surface->hwnd;
	track.dwHoverTime = 0;
	TrackMouseEvent(&track);

	/* The core hears that the X window was entered. */
	if (backend->server != NULL)
		x11server_backend_enter(backend->server, surface->window, 0);
}
