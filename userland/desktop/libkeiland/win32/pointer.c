/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fingers and the pen on Windows: the content's WM_POINTER* messages
 * (Windows 8 and later) of a touch screen and of a pen.
 *
 * The mouse stays on the mouse messages (EnableMouseInPointer is not
 * called: it would move the mouse of every window of the process to the
 * pointer messages).  A finger is a KL_WINDOW_TOUCH_* input with the
 * pointer's id; a lost capture cancels the fingers down.  A pen is the
 * KL_WINDOW_TABLET_* inputs (its pressure 0 to 1024 as 0 to 1, its tilt in
 * degrees, its barrel button and its eraser) for a window that takes the
 * tablet (kl_window_accept_tablet); for any other window its messages go
 * to DefWindowProcW, which makes the mouse's messages of them, so the pen
 * is the pointer.  A touch pad's scrolling comes as the wheel's
 * (WM_MOUSEWHEEL, window.c) and its pinch as Ctrl and the wheel; the
 * fingers' own scrolling with its inertia (DirectManipulation) is not
 * taken here.
 */

#include "userland/desktop/libkeiland/win32/window-win32.h"
#include "userland/desktop/libkeiland/win32/input.h"

#include <errno.h>
#include <string.h>

/* The full pressure of a pen's PEN_MASK_PRESSURE. */
#define POINTER_PRESSURE_FULL	1024.0

static int pointer_touch(struct kl_window *window, UINT message, UINT32 id);
static int pointer_pen(struct kl_window *window, UINT message, UINT32 id);
static void pointer_place(const struct kl_window *window, POINT screen, double *x, double *y);

/*
 * Lets a window take the pen as a tablet's inputs.  Returns 0 or EINVAL.
 */
int
kl_window_accept_tablet(
	struct kl_window *window)
{
	/* A window. */
	if (window == NULL)
		return EINVAL;

	/* The pen's messages are the window's from now on. */
	window->tablet = 1;
	return 0;
}

/*
 * Takes one of the content's messages when it is a finger's or a pen's
 * the window uses.  Returns 1 when the message was taken (the window
 * procedure answers 0), 0 when DefWindowProcW is to have it.
 */
int
keiui_win32_pointer_message(
	struct kl_window *window,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	POINTER_INPUT_TYPE type;
	UINT32 id;
	BOOL known;
	int taken;

	(void)lparam;

	/* Only the pointer messages. */
	if (message != WM_POINTERDOWN &&
	    message != WM_POINTERUPDATE &&
	    message != WM_POINTERUP &&
	    message != WM_POINTERLEAVE &&
	    message != WM_POINTERCAPTURECHANGED)
		return 0;

	/* The pointer and its kind. */
	id = GET_POINTERID_WPARAM(wparam);
	type = PT_POINTER;
	known = GetPointerType(id, &type);
	if (!known)
		return 0;

	/* A finger, a pen, or the rest (the mouse) for the system. */
	taken = 0;
	if (type == KEIUI_WIN32_POINTER_TOUCH)
		taken = pointer_touch(window, message, id);
	if (type == KEIUI_WIN32_POINTER_PEN && window->tablet)
		taken = pointer_pen(window, message, id);

	/* Whether the message was taken. */
	return taken;
}

/* A finger's message as a touch input; 1 when it was taken. */
static int
pointer_touch(
	struct kl_window *window,
	UINT message,
	UINT32 id)
{
	struct kl_window_event *input;
	POINTER_INFO info;
	unsigned kind;
	BOOL read;

	/* What happened to the finger. */
	kind = KL_WINDOW_TOUCH_MOTION;
	if (message == WM_POINTERDOWN)
		kind = KL_WINDOW_TOUCH_DOWN;
	if (message == WM_POINTERUP)
		kind = KL_WINDOW_TOUCH_UP;
	if (message == WM_POINTERCAPTURECHANGED)
		kind = KL_WINDOW_TOUCH_CANCEL;

	/* A finger that leaves without lifting says nothing more. */
	if (message == WM_POINTERLEAVE)
		return 1;

	/* Where it is (a cancel has no place). */
	memset(&info, 0, sizeof(info));
	read = TRUE;
	if (kind != KL_WINDOW_TOUCH_CANCEL)
		read = GetPointerInfo(id, &info);
	if (!read)
		return 0;

	/* The input. */
	input = keiui_win32_push(window, kind);
	if (input == NULL)
		return 1;
	input->id = (int32_t)id;
	if (kind != KL_WINDOW_TOUCH_CANCEL)
		pointer_place(window, info.ptPixelLocation, &input->x, &input->y);

	/* Taken. */
	return 1;
}

/* A pen's message as a tablet's input; 1 when it was taken. */
static int
pointer_pen(
	struct kl_window *window,
	UINT message,
	UINT32 id)
{
	struct kl_window_event *input;
	POINTER_PEN_INFO pen;
	unsigned kind;
	BOOL read;

	/* The pen's state. */
	memset(&pen, 0, sizeof(pen));
	read = GetPointerPenInfo(id, &pen);
	if (!read)
		return 0;

	/* What happened: a contact's start, its moves and end, a hover, or leaving. */
	kind = KL_WINDOW_TABLET_HOVER;
	if ((pen.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0U)
		kind = KL_WINDOW_TABLET_MOTION;
	if (message == WM_POINTERDOWN)
		kind = KL_WINDOW_TABLET_DOWN;
	if (message == WM_POINTERUP)
		kind = KL_WINDOW_TABLET_UP;
	if (message == WM_POINTERLEAVE || message == WM_POINTERCAPTURECHANGED)
		kind = KL_WINDOW_TABLET_LEAVE;

	/* The input: the place, the tool, its buttons, its pressure and its tilt. */
	input = keiui_win32_push(window, kind);
	if (input == NULL)
		return 1;
	input->id = (int32_t)id;
	pointer_place(window, pen.pointerInfo.ptPixelLocation, &input->x, &input->y);
	input->tool = KL_TABLET_PEN;
	if ((pen.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0U)
		input->tool = KL_TABLET_ERASER;
	input->buttons = 0U;
	if ((pen.penFlags & PEN_FLAG_BARREL) != 0U)
		input->buttons |= KL_TABLET_BUTTON_STYLUS;
	input->pressure = -1.0;
	if ((pen.penMask & PEN_MASK_PRESSURE) != 0U)
		input->pressure = (double)pen.pressure / POINTER_PRESSURE_FULL;
	if ((pen.penMask & PEN_MASK_TILT_X) != 0U)
		input->tilt_x = (double)pen.tiltX;
	if ((pen.penMask & PEN_MASK_TILT_Y) != 0U)
		input->tilt_y = (double)pen.tiltY;

	/* Taken. */
	return 1;
}

/* A point of the screen as the content's pixels. */
static void
pointer_place(
	const struct kl_window *window,
	POINT screen,
	double *x,
	double *y)
{
	POINT point;

	/* The content's point. */
	point = screen;
	(void)ScreenToClient(window->content, &point);
	*x = (double)point.x;
	*y = (double)point.y;
}
