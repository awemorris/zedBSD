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
 * is the pointer.
 *
 * A precision touch pad's scrolling goes through DirectManipulation where
 * the system has it: the content has a viewport that takes the fingers of
 * a touch pad (DM_POINTERHITTEST, SetContact) and moves its content with
 * them, without inertia, scaling or rails.  Each move of the content, read
 * while the viewport runs (DirectManipulation is updated on a timer then),
 * is a KL_WINDOW_AXIS of the fingers (axis_source KL_AXIS_SOURCE_FINGER,
 * the distance in the content's pixels), and the fingers' lift is a
 * KL_WINDOW_AXIS_STOP, after which the program lets the content fly on
 * itself.  The viewport's content is far larger than the viewport and is
 * put back in its middle after each scroll, so that it never reaches an
 * edge.  Without DirectManipulation (or when it cannot be started) the
 * touch pad's scrolling comes as the wheel's (WM_MOUSEWHEEL, window.c),
 * and its pinch as Ctrl and the wheel, as before.
 */

#define COBJMACROS
#include "userland/desktop/libkeiland/win32/window-win32.h"
#include "userland/desktop/libkeiland/win32/input.h"

#include <directmanipulation.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The full pressure of a pen's PEN_MASK_PRESSURE. */
#define POINTER_PRESSURE_FULL	1024.0

/* The timer DirectManipulation is updated on while the viewport runs, and its period (milliseconds). */
#define POINTER_UPDATE_TIMER	0x4B4C4D31U
#define POINTER_UPDATE_PERIOD	8U

/* The side of the viewport's content (its pixels), and the side of the viewport kept in its middle. */
#define POINTER_CONTENT_SIDE	1000000.0f
#define POINTER_VIEW_SIDE	1000.0f

/*
 * The viewport's event handler: its interface, its references, and the
 * window's touch pad state (NULL once the window closed).
 */
struct pointer_handler {
	IDirectManipulationViewportEventHandler iface;
	LONG references;
	struct keiui_win32_manipulation *manipulation;
};

/*
 * A window's DirectManipulation: its window, the manager and its update
 * manager, the viewport and its content, the event handler and the cookie
 * it was added with, whether this window started COM on the thread,
 * whether the viewport runs (the timer is on), whether the content is being
 * put back in the middle (its moves are not the fingers'), and the
 * content's last offset read.
 */
struct keiui_win32_manipulation {
	struct kl_window *window;
	IDirectManipulationManager *manager;
	IDirectManipulationUpdateManager *updates;
	IDirectManipulationViewport *viewport;
	IDirectManipulationContent *content;
	struct pointer_handler *handler;
	DWORD cookie;
	int com;
	int running;
	int centering;
	float x;
	float y;
};

static int pointer_touch(struct kl_window *window, UINT message, UINT32 id);
static int pointer_pen(struct kl_window *window, UINT message, UINT32 id);
static void pointer_place(const struct kl_window *window, POINT screen, double *x, double *y);
static void pointer_end(struct keiui_win32_manipulation *manipulation);
static void pointer_center(struct keiui_win32_manipulation *manipulation);
static int pointer_offset(struct keiui_win32_manipulation *manipulation, float *x, float *y);
static void pointer_moved(struct keiui_win32_manipulation *manipulation);
static void pointer_status(struct keiui_win32_manipulation *manipulation, DIRECTMANIPULATION_STATUS current);
static HRESULT STDMETHODCALLTYPE handler_query(IDirectManipulationViewportEventHandler *iface, REFIID id, void **out);
static ULONG STDMETHODCALLTYPE handler_add(IDirectManipulationViewportEventHandler *iface);
static ULONG STDMETHODCALLTYPE handler_release(IDirectManipulationViewportEventHandler *iface);
static HRESULT STDMETHODCALLTYPE handler_status(IDirectManipulationViewportEventHandler *iface, IDirectManipulationViewport *viewport, DIRECTMANIPULATION_STATUS current, DIRECTMANIPULATION_STATUS previous);
static HRESULT STDMETHODCALLTYPE handler_viewport(IDirectManipulationViewportEventHandler *iface, IDirectManipulationViewport *viewport);
static HRESULT STDMETHODCALLTYPE handler_content(IDirectManipulationViewportEventHandler *iface, IDirectManipulationViewport *viewport, IDirectManipulationContent *content);

/* The event handler's calls; constant for the life of the program. */
static IDirectManipulationViewportEventHandlerVtbl pointer_handler_calls = {
	.QueryInterface = handler_query,
	.AddRef = handler_add,
	.Release = handler_release,
	.OnViewportStatusChanged = handler_status,
	.OnViewportUpdated = handler_viewport,
	.OnContentUpdated = handler_content
};

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

	/* The long parameter carries nothing the pointer messages need here. */
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

/*
 * Starts DirectManipulation for a window's content: a viewport that takes
 * the touch pad's fingers.  A window where it cannot be started keeps the
 * wheel's messages (window->manipulation stays NULL).
 */
void
keiui_win32_manipulation_open(
	struct kl_window *window)
{
	struct keiui_win32_manipulation *manipulation;
	struct pointer_handler *handler;
	DIRECTMANIPULATION_CONFIGURATION configuration;
	RECT view;
	HRESULT result;

	/* The state, and the handler with its first reference (the state's). */
	manipulation = calloc(1U, sizeof(*manipulation));
	handler = calloc(1U, sizeof(*handler));
	if (manipulation == NULL || handler == NULL) {
		free(manipulation);
		free(handler);
		return;
	}

	/* Joins the handler and the state, the state holding the first reference. */
	handler->iface.lpVtbl = &pointer_handler_calls;
	handler->references = 1;
	handler->manipulation = manipulation;
	manipulation->handler = handler;
	manipulation->window = window;

	/* COM on the thread (apartment-threaded, as the window's input method's). */
	result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(result)) {
		pointer_end(manipulation);
		return;
	}

	/* Records that this call started COM, so closing the window ends it. */
	manipulation->com = 1;

	/* The manager and its update manager. */
	result = CoCreateInstance(&CLSID_DirectManipulationManager, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectManipulationManager, (void **)&manipulation->manager);
	if (FAILED(result)) {
		manipulation->manager = NULL;
		pointer_end(manipulation);
		return;
	}

	/* Asks for the update manager, which the timer's ticks drive. */
	result = IDirectManipulationManager_GetUpdateManager(manipulation->manager, &IID_IDirectManipulationUpdateManager, (void **)&manipulation->updates);
	if (FAILED(result)) {
		manipulation->updates = NULL;
		pointer_end(manipulation);
		return;
	}

	/* The content's viewport, updated when this backend asks, panning only. */
	result = IDirectManipulationManager_CreateViewport(manipulation->manager, NULL, window->content, &IID_IDirectManipulationViewport, (void **)&manipulation->viewport);
	if (FAILED(result)) {
		manipulation->viewport = NULL;
		pointer_end(manipulation);
		return;
	}

	/* Chooses panning in both directions as the viewport's behaviour. */
	configuration = DIRECTMANIPULATION_CONFIGURATION_INTERACTION |
	    DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X |
	    DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y;
	result = IDirectManipulationViewport_ActivateConfiguration(manipulation->viewport, configuration);
	if (FAILED(result)) {
		pointer_end(manipulation);
		return;
	}

	/* Makes the viewport move only when this backend asks. */
	result = IDirectManipulationViewport_SetViewportOptions(manipulation->viewport, DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE);
	if (FAILED(result)) {
		pointer_end(manipulation);
		return;
	}

	/* The viewport's place, and its content far larger, the viewport in its middle. */
	view.left = 0;
	view.top = 0;
	view.right = (LONG)POINTER_VIEW_SIDE;
	view.bottom = (LONG)POINTER_VIEW_SIDE;
	result = IDirectManipulationViewport_SetViewportRect(manipulation->viewport, &view);
	if (FAILED(result)) {
		pointer_end(manipulation);
		return;
	}

	/* Takes the content that the viewport moves. */
	result = IDirectManipulationViewport_GetPrimaryContent(manipulation->viewport, &IID_IDirectManipulationContent, (void **)&manipulation->content);
	if (FAILED(result)) {
		manipulation->content = NULL;
		pointer_end(manipulation);
		return;
	}

	/* Makes the content far larger than the viewport. */
	view.right = (LONG)POINTER_CONTENT_SIDE;
	view.bottom = (LONG)POINTER_CONTENT_SIDE;
	result = IDirectManipulationContent_SetContentRect(manipulation->content, &view);
	if (FAILED(result)) {
		pointer_end(manipulation);
		return;
	}

	/* Its events heard, and the viewport and the manager started for the content. */
	result = IDirectManipulationViewport_AddEventHandler(manipulation->viewport, window->content, &handler->iface, &manipulation->cookie);
	if (FAILED(result)) {
		pointer_end(manipulation);
		return;
	}

	/* Enables the viewport. */
	result = IDirectManipulationViewport_Enable(manipulation->viewport);
	if (FAILED(result)) {
		pointer_end(manipulation);
		return;
	}

	/* Starts the manager for the content window. */
	result = IDirectManipulationManager_Activate(manipulation->manager, window->content);
	if (FAILED(result)) {
		pointer_end(manipulation);
		return;
	}

	/* Succeeded: the touch pad's fingers are the viewport's from now on. */
	pointer_center(manipulation);
	window->manipulation = manipulation;
}

/*
 * Stops a window's DirectManipulation (the window closes).
 */
void
keiui_win32_manipulation_close(
	struct kl_window *window)
{
	struct keiui_win32_manipulation *manipulation;

	/* A window without it. */
	manipulation = window->manipulation;
	if (manipulation == NULL)
		return;
	window->manipulation = NULL;

	/* The timer and the manager's hold on the content, then the rest. */
	if (manipulation->running)
		(void)KillTimer(window->content, POINTER_UPDATE_TIMER);
	manipulation->running = 0;
	(void)IDirectManipulationManager_Deactivate(manipulation->manager, window->content);
	pointer_end(manipulation);
}

/*
 * Takes one of the content's messages that is DirectManipulation's: a
 * touch pad's contact to give to the viewport (DM_POINTERHITTEST), or the
 * timer that updates it.  Returns 1 when the message was taken (the
 * window procedure answers 0), 0 when it is not this backend's.
 */
int
keiui_win32_manipulation_message(
	struct kl_window *window,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	struct keiui_win32_manipulation *manipulation;
	POINTER_INPUT_TYPE type;
	UINT32 id;
	BOOL known;
	HRESULT result;

	/* The long parameter carries nothing the manipulation messages need here. */
	(void)lparam;

	/* A window with DirectManipulation. */
	manipulation = window->manipulation;
	if (manipulation == NULL)
		return 0;

	/* The timer: DirectManipulation moves the content, whose events tell the moves. */
	if (message == WM_TIMER && wparam == (WPARAM)POINTER_UPDATE_TIMER) {
		(void)IDirectManipulationUpdateManager_Update(manipulation->updates, NULL);
		return 1;
	}

	/* Only a touch pad's contact goes to the viewport. */
	if (message != DM_POINTERHITTEST)
		return 0;
	id = GET_POINTERID_WPARAM(wparam);
	type = PT_POINTER;
	known = GetPointerType(id, &type);
	if (!known || type != KEIUI_WIN32_POINTER_TOUCHPAD)
		return 0;
	result = IDirectManipulationViewport_SetContact(manipulation->viewport, id);
	if (FAILED(result))
		return 0;

	/* Taken: the fingers are the viewport's. */
	return 1;
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

/* Releases what a window's DirectManipulation holds, whatever was made of it, and frees it. */
static void
pointer_end(
	struct keiui_win32_manipulation *manipulation)
{
	struct pointer_handler *handler;

	/* The viewport, its handler taken off first. */
	if (manipulation->viewport != NULL) {
		if (manipulation->cookie != 0U)
			(void)IDirectManipulationViewport_RemoveEventHandler(manipulation->viewport, manipulation->cookie);
		(void)IDirectManipulationViewport_Abandon(manipulation->viewport);
	}

	/* Releases the interfaces the state holds. */
	if (manipulation->content != NULL)
		IDirectManipulationContent_Release(manipulation->content);
	if (manipulation->viewport != NULL)
		IDirectManipulationViewport_Release(manipulation->viewport);

	/* The managers. */
	if (manipulation->updates != NULL)
		IDirectManipulationUpdateManager_Release(manipulation->updates);
	if (manipulation->manager != NULL)
		IDirectManipulationManager_Release(manipulation->manager);

	/* The handler forgets the state; DirectManipulation may hold it a little longer. */
	handler = manipulation->handler;
	if (handler != NULL) {
		handler->manipulation = NULL;
		IDirectManipulationViewportEventHandler_Release(&handler->iface);
	}

	/* COM, as this window started it, and the state. */
	if (manipulation->com)
		CoUninitialize();
	free(manipulation);
}

/* Puts the viewport back in the middle of its content, and takes the content's offset there as the last read. */
static void
pointer_center(
	struct keiui_win32_manipulation *manipulation)
{
	float middle;
	float x;
	float y;
	int found;

	/* The middle, at once (its moves are not the fingers'). */
	middle = (POINTER_CONTENT_SIDE - POINTER_VIEW_SIDE) / 2.0f;
	manipulation->centering = 1;
	(void)IDirectManipulationViewport_ZoomToRect(manipulation->viewport, middle, middle, middle + POINTER_VIEW_SIDE, middle + POINTER_VIEW_SIDE, FALSE);
	(void)IDirectManipulationUpdateManager_Update(manipulation->updates, NULL);
	manipulation->centering = 0;

	/* The offset there. */
	found = pointer_offset(manipulation, &x, &y);
	if (!found)
		return;
	manipulation->x = x;
	manipulation->y = y;
}

/* Reads the content's offset (its transform's translation); 1 when read. */
static int
pointer_offset(
	struct keiui_win32_manipulation *manipulation,
	float *x,
	float *y)
{
	float transform[6];
	HRESULT result;

	/* The content's transform. */
	result = IDirectManipulationContent_GetContentTransform(manipulation->content, transform, 6U);
	if (FAILED(result))
		return 0;

	/* Its translation. */
	*x = transform[4];
	*y = transform[5];
	return 1;
}

/* Tells the program how far the fingers moved the content since the last read, as a scroll of the fingers. */
static void
pointer_moved(
	struct keiui_win32_manipulation *manipulation)
{
	struct kl_window_event *input;
	struct kl_window *window;
	POINT cursor;
	float x;
	float y;
	float dx;
	float dy;
	int found;

	/* Not the fingers' (the content put back), or not read. */
	if (manipulation->centering || !manipulation->running)
		return;
	found = pointer_offset(manipulation, &x, &y);
	if (!found)
		return;

	/* The content follows the fingers; the scroll is the other way (a positive distance shows what is below). */
	dx = manipulation->x - x;
	dy = manipulation->y - y;
	manipulation->x = x;
	manipulation->y = y;
	if (dx == 0.0f && dy == 0.0f)
		return;

	/* The scroll, at the pointer's place on the content. */
	window = manipulation->window;
	input = keiui_win32_push(window, KL_WINDOW_AXIS);
	if (input == NULL)
		return;
	cursor.x = 0;
	cursor.y = 0;
	(void)GetCursorPos(&cursor);
	pointer_place(window, cursor, &input->x, &input->y);
	input->axis_source = KL_AXIS_SOURCE_FINGER;
	input->dx = (double)dx;
	input->dy = (double)dy;
}

/* Follows the viewport's state: the timer runs while it does, and the fingers' lift ends the scroll. */
static void
pointer_status(
	struct keiui_win32_manipulation *manipulation,
	DIRECTMANIPULATION_STATUS current)
{
	struct kl_window *window;
	UINT_PTR timer;

	/* The content put back in the middle is not the fingers'. */
	window = manipulation->window;
	if (manipulation->centering)
		return;

	/* The fingers start: the timer updates DirectManipulation from now on. */
	if (current == DIRECTMANIPULATION_RUNNING) {
		if (manipulation->running)
			return;
		timer = SetTimer(window->content, POINTER_UPDATE_TIMER, POINTER_UPDATE_PERIOD, NULL);
		if (timer == 0U)
			return;
		manipulation->running = 1;
		return;
	}

	/* The fingers lift (no inertia is asked for): the scroll ends, and the content goes back to the middle. */
	if (!manipulation->running)
		return;
	if (current != DIRECTMANIPULATION_READY && current != DIRECTMANIPULATION_INERTIA)
		return;
	(void)KillTimer(window->content, POINTER_UPDATE_TIMER);
	manipulation->running = 0;
	(void)keiui_win32_push(window, KL_WINDOW_AXIS_STOP);
	pointer_center(manipulation);
}

/* Gives the handler's interfaces. */
static HRESULT STDMETHODCALLTYPE
handler_query(
	IDirectManipulationViewportEventHandler *iface,
	REFIID id,
	void **out)
{
	struct pointer_handler *handler;
	BOOL offered;

	/* Somewhere to put it. */
	if (out == NULL)
		return E_POINTER;
	*out = NULL;
	handler = (struct pointer_handler *)iface;

	/* The handler's, or nothing. */
	offered = IsEqualIID(id, &IID_IUnknown);
	if (!offered)
		offered = IsEqualIID(id, &IID_IDirectManipulationViewportEventHandler);
	if (!offered)
		return E_NOINTERFACE;
	*out = &handler->iface;

	/* Succeeded: one more reference. */
	(void)InterlockedIncrement(&handler->references);
	return S_OK;
}

/* Takes a reference. */
static ULONG STDMETHODCALLTYPE
handler_add(
	IDirectManipulationViewportEventHandler *iface)
{
	struct pointer_handler *handler;
	LONG count;

	/* One more. */
	handler = (struct pointer_handler *)iface;
	count = InterlockedIncrement(&handler->references);
	return (ULONG)count;
}

/* Lets a reference go; the last frees the handler. */
static ULONG STDMETHODCALLTYPE
handler_release(
	IDirectManipulationViewportEventHandler *iface)
{
	struct pointer_handler *handler;
	LONG count;

	/* One fewer. */
	handler = (struct pointer_handler *)iface;
	count = InterlockedDecrement(&handler->references);
	if (count != 0)
		return (ULONG)count;

	/* The last. */
	free(handler);
	return 0U;
}

/* The viewport's state changed. */
static HRESULT STDMETHODCALLTYPE
handler_status(
	IDirectManipulationViewportEventHandler *iface,
	IDirectManipulationViewport *viewport,
	DIRECTMANIPULATION_STATUS current,
	DIRECTMANIPULATION_STATUS previous)
{
	struct pointer_handler *handler;

	/* The viewport and the previous status are not needed. */
	(void)viewport;
	(void)previous;

	/* Followed, while the window is open. */
	handler = (struct pointer_handler *)iface;
	if (handler->manipulation != NULL)
		pointer_status(handler->manipulation, current);
	return S_OK;
}

/* The viewport was updated: nothing to do (the content's moves are what count). */
static HRESULT STDMETHODCALLTYPE
handler_viewport(
	IDirectManipulationViewportEventHandler *iface,
	IDirectManipulationViewport *viewport)
{
	(void)iface;
	(void)viewport;

	/* Nothing. */
	return S_OK;
}

/* The content moved. */
static HRESULT STDMETHODCALLTYPE
handler_content(
	IDirectManipulationViewportEventHandler *iface,
	IDirectManipulationViewport *viewport,
	IDirectManipulationContent *content)
{
	struct pointer_handler *handler;

	/* The viewport and the content are not needed; the state knows what moved. */
	(void)viewport;
	(void)content;

	/* Told, while the window is open. */
	handler = (struct pointer_handler *)iface;
	if (handler->manipulation != NULL)
		pointer_moved(handler->manipulation);
	return S_OK;
}
