/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window on Windows (kl_window): the frame and its content, the window
 * procedure, and the queue of inputs.
 *
 * The frame (WS_OVERLAPPEDWINDOW) gives its whole window to its client
 * (WM_NCCALCSIZE; a maximized frame keeps to its monitor's work area),
 * keeps the system's shadow (DwmExtendFrameIntoClientArea by one pixel at
 * the top) and answers WM_NCHITTEST with frame.c's hit test: the title bar
 * drags, the edges resize, and the three caption buttons are libkeiland's,
 * drawn by present.c and pressed through the non-client mouse messages
 * (which never reach DefWindowProcW, which would draw the old buttons).
 * The content covers the client area below the title bar and passes the
 * resize bands through to the frame (HTTRANSPARENT).
 *
 * The keys come to the frame, which keeps the focus.  A key's text is the
 * WM_CHAR (WM_SYSCHAR) units TranslateMessage queued behind it, taken at
 * once; a character without a key (Alt and the number pad) is a
 * KL_WINDOW_TEXT_COMMIT.  The pointer's messages come to the content (the
 * wheel's to the frame, with the screen's point, turned into the
 * content's).  While the frame is resized in the system's modal loop
 * (WM_ENTERSIZEMOVE to WM_EXITSIZEMOVE) the program does not draw; the last
 * size is told at the end of the loop.
 */

#include "userland/desktop/libkeiland/win32/window-win32.h"
#include "userland/desktop/libkeiland/win32/frame.h"
#include "userland/desktop/libkeiland/win32/input.h"
#include "userland/desktop/libkeiland/win32/keys-table.h"

#include <dwmapi.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The classes of the frame and of the content. */
#define WINDOW_FRAME_CLASS	L"KeilandWindow"
#define WINDOW_CONTENT_CLASS	L"KeilandContent"

/* The smallest content a frame keeps, at 96 dpi. */
#define WINDOW_MIN_WIDTH	320U
#define WINDOW_MIN_HEIGHT	160U

/* The program's icon: its first icon resource (none: the system's default). */
#define WINDOW_ICON		1

/* No caption button under the pointer or pressed. */
#define WINDOW_NO_BUTTON	3U

/* The units of text a key's characters may take, and the bytes they become. */
#define WINDOW_CHAR_UNITS	16U

static LRESULT window_frame_message(struct kl_window *window, HWND handle, UINT message, WPARAM wparam, LPARAM lparam);
static LRESULT window_content_message(struct kl_window *window, HWND handle, UINT message, WPARAM wparam, LPARAM lparam);
static void window_frame_box(const struct kl_window *window, struct keiui_win32_frame *frame);
static unsigned window_hit(const struct kl_window *window, int32_t x, int32_t y);
static unsigned window_button_of(unsigned hit);
static void window_layout(struct kl_window *window);
static void window_resized(struct kl_window *window);
static void window_key(struct kl_window *window, UINT message, WPARAM wparam, LPARAM lparam);
static void window_char(struct kl_window *window, WPARAM wparam);
static void window_pointer(struct kl_window *window, unsigned kind, LPARAM lparam, uint32_t code, int pressed);
static void window_wheel(struct kl_window *window, WPARAM wparam, LPARAM lparam, int horizontal);
static void window_caption_mouse(struct kl_window *window, UINT message, WPARAM wparam);
static void window_caption_press(struct kl_window *window, unsigned button);
static void window_copy_data(struct kl_window *window, const COPYDATASTRUCT *copy);
static unsigned window_modifiers(void);
static uint32_t window_caption(const struct kl_window *window);
static int window_set_title(struct kl_window *window, const char *title);
static void window_font(struct kl_window *window);
static void window_title_mouse(struct kl_window *window, UINT message, LPARAM lparam);

/*
 * Makes a window of the application: the frame with its title and the
 * content of the size asked for (scaled to the frame's dpi), shown.
 */
struct kl_window *
kl_app_window_create(
	struct kl_app *app,
	const struct kl_window_options *options)
{
	struct kl_window *window;
	MARGINS margins;
	BOOL dark;
	uint32_t width;
	uint32_t height;
	uint32_t caption;
	int error;

	/* An application to make it on. */
	if (app == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* The window's state. */
	window = calloc(1U, sizeof(*window));
	if (window == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	window->app = app;
	window->present = KL_PRESENT_NONE;
	window->dpi = 96U;
	window->hover = WINDOW_NO_BUTTON;
	window->pressed = WINDOW_NO_BUTTON;
	window->control_hover = KEIUI_WIN32_NO_CONTROL;
	window->control_pressed = KEIUI_WIN32_NO_CONTROL;
	window_font(window);
	width = 800U;
	height = 600U;
	if (options != NULL) {
		window->present = options->present;
		if (options->width != 0U)
			width = options->width;
		if (options->height != 0U)
			height = options->height;
	}

	/* The frame, at 96 dpi's size for now. */
	window->frame = CreateWindowExW(0U, WINDOW_FRAME_CLASS, L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
	    CW_USEDEFAULT, CW_USEDEFAULT, (int)width, (int)(height + KEIUI_WIN32_CAPTION), NULL, NULL, app->module, window);
	if (window->frame == NULL) {
		if (window->text_open)
			kl_text_close(&window->text);
		free(window);
		errno = ENOMEM;
		return NULL;
	}

	/* The content, laid out below the title bar. */
	window->content = CreateWindowExW(0U, WINDOW_CONTENT_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
	    0, 0, (int)width, (int)height, window->frame, NULL, app->module, window);
	if (window->content == NULL) {
		(void)DestroyWindow(window->frame);
		if (window->text_open)
			kl_text_close(&window->text);
		free(window);
		errno = ENOMEM;
		return NULL;
	}

	/* One of the application's windows, found by a second start of the program by its property. */
	window->next = app->windows;
	app->windows = window;
	(void)SetPropW(window->frame, app->application_wide, (HANDLE)1);

	/* Its title. */
	if (options != NULL && options->title != NULL) {
		error = window_set_title(window, options->title);
		if (error != 0) {
			kl_window_close(window);
			errno = error;
			return NULL;
		}
	}

	/* The system's shadow, and the dark title bar in the dark appearance. */
	memset(&margins, 0, sizeof(margins));
	margins.cyTopHeight = 1;
	(void)DwmExtendFrameIntoClientArea(window->frame, &margins);
	dark = FALSE;
	if (app->dark)
		dark = TRUE;
	(void)DwmSetWindowAttribute(window->frame, 20U, &dark, sizeof(dark));

	/* The frame's size at its dpi, its client the whole window from now on. */
	window->dpi = GetDpiForWindow(window->frame);
	if (window->dpi == 0U)
		window->dpi = 96U;
	caption = window_caption(window);
	(void)SetWindowPos(window->frame, NULL, 0, 0, (int)keiui_win32_scale(width, window->dpi),
	    (int)(keiui_win32_scale(height, window->dpi) + caption), SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);

	/* Shown, with the content laid out and its size known. */
	(void)ShowWindow(window->frame, SW_SHOWNORMAL);
	(void)UpdateWindow(window->frame);
	window_layout(window);

	/* Succeeded: the window is shown (its first size, focus and the rest are queued). */
	return window;
}

/*
 * Closes a window: out of its application, its HWNDs destroyed, its
 * frames' memory freed.
 */
void
kl_window_close(
	struct kl_window *window)
{
	struct kl_window **link;
	struct kl_app *app;

	/* No window. */
	if (window == NULL)
		return;
	app = window->app;

	/* Out of the application's list. */
	for (link = &app->windows; *link != NULL; link = &(*link)->next) {
		if (*link == window) {
			*link = window->next;
			break;
		}
	}

	/* The HWNDs forget the window, then go. */
	if (window->content != NULL)
		(void)SetWindowLongPtrW(window->content, GWLP_USERDATA, 0);
	if (window->frame != NULL) {
		(void)SetWindowLongPtrW(window->frame, GWLP_USERDATA, 0);
		(void)RemovePropW(window->frame, app->application_wide);
		(void)DestroyWindow(window->frame);
	}

	/* The frames' memory, the title bar's font, the title and the state. */
	keiui_win32_present_release(window);
	if (window->text_open)
		kl_text_close(&window->text);
	free(window->title);
	free(window);
}

/*
 * Reports the content's size as last told (the size the program draws
 * at).
 */
void
kl_window_size(
	const struct kl_window *window,
	uint32_t *width,
	uint32_t *height)
{
	/* No window: nothing. */
	if (window == NULL) {
		if (width != NULL)
			*width = 0U;
		if (height != NULL)
			*height = 0U;
		return;
	}

	/* The size. */
	if (width != NULL)
		*width = window->width;
	if (height != NULL)
		*height = window->height;
}

/*
 * Changes the frame's title (the taskbar's, and the title bar's).
 */
void
kl_window_set_title(
	struct kl_window *window,
	const char *title)
{
	/* A window and a title. */
	if (window == NULL || title == NULL)
		return;

	/* The title, kept and shown. */
	(void)window_set_title(window, title);
}

/*
 * Queues an input of a window and gives it to fill (zeroed but for its
 * kind, its times and the modifiers held): at the queue's end, or, when
 * the queue is full, in place of its oldest motion.  NULL when no motion
 * is queued either: the input is dropped and counted.
 */
struct kl_window_event *
keiui_win32_push(
	struct kl_window *window,
	unsigned kind)
{
	struct keiui_win32_input *slot;
	unsigned index;
	unsigned from;
	unsigned to;
	unsigned found;

	/* A full queue gives up its oldest motion. */
	if (window->event_count == KEIUI_WIN32_EVENTS) {
		/* The oldest motion. */
		found = KEIUI_WIN32_EVENTS;
		for (index = 0U; index < window->event_count; index++) {
			from = (window->event_head + index) % KEIUI_WIN32_EVENTS;
			if (window->events[from].input.kind == KL_WINDOW_MOTION) {
				found = index;
				break;
			}
		}

		/* None: the new input is dropped. */
		if (found == KEIUI_WIN32_EVENTS) {
			window->dropped++;
			return NULL;
		}

		/* The inputs after it move up by one. */
		for (index = found; index + 1U < window->event_count; index++) {
			to = (window->event_head + index) % KEIUI_WIN32_EVENTS;
			from = (window->event_head + index + 1U) % KEIUI_WIN32_EVENTS;
			window->events[to] = window->events[from];
		}
		window->event_count--;
	}

	/* The slot at the queue's end. */
	slot = &window->events[(window->event_head + window->event_count) % KEIUI_WIN32_EVENTS];
	memset(slot, 0, sizeof(*slot));
	slot->order = keiui_win32_next_order(window->app);
	slot->input.kind = kind;
	slot->input.time_us = kl_clock_us();
	slot->input.arrival_us = slot->input.time_us;
	slot->input.modifiers = window_modifiers();
	slot->input.begin = -1;
	slot->input.end = -1;
	slot->input.pressure = -1.0;
	window->event_count++;

	/* Succeeded: the input to fill. */
	return &slot->input;
}

/* Takes a window's oldest input and its place: 1, or 0 when none is queued. */
int
keiui_win32_take(
	struct kl_window *window,
	uint64_t *order,
	struct kl_window_event *input)
{
	struct keiui_win32_input *slot;

	/* An empty queue. */
	if (window->event_count == 0U)
		return 0;

	/* The oldest input, and the queue moves on. */
	slot = &window->events[window->event_head];
	*order = slot->order;
	*input = slot->input;
	window->event_head = (window->event_head + 1U) % KEIUI_WIN32_EVENTS;
	window->event_count--;

	/* Succeeded: one input taken. */
	return 1;
}

/* Reports the place of a window's oldest input: 1, or 0 when none is queued. */
int
keiui_win32_peek(
	const struct kl_window *window,
	uint64_t *order)
{
	/* An empty queue. */
	if (window->event_count == 0U)
		return 0;

	/* The oldest input's place. */
	*order = window->events[window->event_head].order;
	return 1;
}

/* Registers the classes of the frame and of the content; 0, or ENOMEM. */
int
keiui_win32_classes(
	struct kl_app *app)
{
	WNDCLASSEXW frame;
	WNDCLASSEXW content;
	ATOM registered;
	DWORD error;

	/* The frame's class. */
	memset(&frame, 0, sizeof(frame));
	frame.cbSize = sizeof(frame);
	frame.style = CS_DBLCLKS;
	frame.lpfnWndProc = keiui_win32_window_procedure;
	frame.hInstance = app->module;
	frame.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	frame.hIcon = LoadIconW(app->module, MAKEINTRESOURCEW(WINDOW_ICON));
	frame.lpszClassName = WINDOW_FRAME_CLASS;
	registered = RegisterClassExW(&frame);
	if (registered == 0) {
		error = GetLastError();
		if (error != ERROR_CLASS_ALREADY_EXISTS)
			return ENOMEM;
	}

	/* The content's class. */
	memset(&content, 0, sizeof(content));
	content.cbSize = sizeof(content);
	content.style = CS_DBLCLKS;
	content.lpfnWndProc = keiui_win32_window_procedure;
	content.hInstance = app->module;
	content.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	content.lpszClassName = WINDOW_CONTENT_CLASS;
	registered = RegisterClassExW(&content);
	if (registered == 0) {
		error = GetLastError();
		if (error != ERROR_CLASS_ALREADY_EXISTS) {
			(void)UnregisterClassW(WINDOW_FRAME_CLASS, app->module);
			return ENOMEM;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Unregisters the classes (a class still used by a window stays). */
void
keiui_win32_classes_drop(
	struct kl_app *app)
{
	/* Both classes. */
	(void)UnregisterClassW(WINDOW_CONTENT_CLASS, app->module);
	(void)UnregisterClassW(WINDOW_FRAME_CLASS, app->module);
}

/*
 * The window procedure of the frames and the contents: the window comes
 * with the creation's parameter and stays in the HWND's user data.
 */
LRESULT CALLBACK
keiui_win32_window_procedure(
	HWND handle,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	const CREATESTRUCTW *create;
	struct kl_window *window;
	LRESULT result;

	/* The window, kept at the HWND's creation. */
	if (message == WM_NCCREATE) {
		create = (const CREATESTRUCTW *)lparam;
		(void)SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
	}
	window = (struct kl_window *)GetWindowLongPtrW(handle, GWLP_USERDATA);

	/* An HWND whose window is gone, or not made yet: the system's handling. */
	if (window == NULL) {
		result = DefWindowProcW(handle, message, wparam, lparam);
		return result;
	}

	/* The content's messages. */
	if (handle == window->content) {
		result = window_content_message(window, handle, message, wparam, lparam);
		return result;
	}

	/* The frame's. */
	result = window_frame_message(window, handle, message, wparam, lparam);
	return result;
}

/* The frame's messages. */
static LRESULT
window_frame_message(
	struct kl_window *window,
	HWND handle,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	NCCALCSIZE_PARAMS *sizes;
	MINMAXINFO *limits;
	const RECT *suggested;
	MONITORINFO monitor;
	HMONITOR place;
	POINT point;
	uint32_t caption;
	unsigned hit;
	unsigned button;
	LRESULT result;
	BOOL zoomed;
	int handled;

	/* The input method's messages. */
	handled = keiui_win32_ime_message(window, message, wparam, lparam, &result);
	if (handled)
		return result;

	/* What the message is. */
	switch (message) {
	case WM_MOUSEMOVE:
	case WM_MOUSELEAVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_LBUTTONUP:
		/* The pointer over the title bar's controls. */
		window_title_mouse(window, message, lparam);
		return 0;
	case WM_NCCALCSIZE:
		/* The client is the whole window; a maximized one keeps to its monitor's work area. */
		if (!wparam)
			break;
		sizes = (NCCALCSIZE_PARAMS *)lparam;
		zoomed = IsZoomed(handle);
		if (zoomed) {
			place = MonitorFromRect(&sizes->rgrc[0], MONITOR_DEFAULTTONEAREST);
			memset(&monitor, 0, sizeof(monitor));
			monitor.cbSize = sizeof(monitor);
			if (GetMonitorInfoW(place, &monitor))
				sizes->rgrc[0] = monitor.rcWork;
		}
		return 0;
	case WM_NCHITTEST:
		/* frame.c's answer at the point, in the client's pixels. */
		point.x = (LONG)(short)LOWORD(lparam);
		point.y = (LONG)(short)HIWORD(lparam);
		(void)ScreenToClient(handle, &point);
		hit = window_hit(window, (int32_t)point.x, (int32_t)point.y);
		return (LRESULT)hit;
	case WM_NCMOUSEMOVE:
	case WM_NCLBUTTONDOWN:
	case WM_NCLBUTTONUP:
	case WM_NCLBUTTONDBLCLK:
		/* The caption buttons are libkeiland's; the rest is the system's (the drag, the resize). */
		button = window_button_of((unsigned)wparam);
		window_caption_mouse(window, message, wparam);
		if (button != WINDOW_NO_BUTTON)
			return 0;
		break;
	case WM_NCMOUSELEAVE:
		/* The pointer left the title bar's buttons. */
		window->frame_tracking = 0;
		window->hover = WINDOW_NO_BUTTON;
		window->pressed = WINDOW_NO_BUTTON;
		(void)InvalidateRect(handle, NULL, FALSE);
		return 0;
	case WM_GETMINMAXINFO:
		/* The smallest frame: a little content below the title bar. */
		limits = (MINMAXINFO *)lparam;
		caption = window_caption(window);
		limits->ptMinTrackSize.x = (LONG)keiui_win32_scale(WINDOW_MIN_WIDTH, window->dpi);
		limits->ptMinTrackSize.y = (LONG)(keiui_win32_scale(WINDOW_MIN_HEIGHT, window->dpi) + caption);
		return 0;
	case WM_DPICHANGED:
		/* The new dpi, and the rectangle the system suggests for it. */
		window->dpi = HIWORD(wparam);
		window->resize_waiting = 1;
		suggested = (const RECT *)lparam;
		(void)SetWindowPos(handle, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
		    suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
		window_layout(window);
		return 0;
	case WM_SIZE:
		/* The content follows the frame; its size is told now, or at the end of the modal loop. */
		if (wparam == SIZE_MINIMIZED)
			return 0;
		window->maximized = 0;
		if (wparam == SIZE_MAXIMIZED)
			window->maximized = 1;
		window_layout(window);
		return 0;
	case WM_ENTERSIZEMOVE:
		/* The system's modal loop: the size is told at its end. */
		window->sizing = 1;
		return 0;
	case WM_EXITSIZEMOVE:
		/* The loop's end, and the last size. */
		window->sizing = 0;
		if (window->resize_waiting)
			window_resized(window);
		return 0;
	case WM_PAINT:
		/* The title bar. */
		keiui_win32_paint_caption(window);
		return 0;
	case WM_ERASEBKGND:
		/* Everything is painted over. */
		return 1;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		/* The keyboard's focus. */
		window_pointer(window, KL_WINDOW_FOCUS, 0, 0U, message == WM_SETFOCUS);
		return 0;
	case WM_KEYDOWN:
	case WM_KEYUP:
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
		/* A key; Alt+F4 still closes through the system. */
		window_key(window, message, wparam, lparam);
		if (message == WM_SYSKEYDOWN && wparam == VK_F4)
			break;
		return 0;
	case WM_CHAR:
	case WM_SYSCHAR:
		/* A character without its key (Alt and the number pad). */
		window_char(window, wparam);
		return 0;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		/* The wheel, at the pointer's place on the content. */
		window_wheel(window, wparam, lparam, message == WM_MOUSEHWHEEL);
		return 0;
	case WM_CLOSE:
		/* The program decides; the window stays until it is closed. */
		(void)keiui_win32_push(window, KL_WINDOW_CLOSE);
		return 0;
	case WM_COPYDATA:
		/* A second start of the program's URLs. */
		window_copy_data(window, (const COPYDATASTRUCT *)lparam);
		return TRUE;
	default:
		break;
	}

	/* The system's handling of the rest. */
	result = DefWindowProcW(handle, message, wparam, lparam);
	return result;
}

/* The content's messages. */
static LRESULT
window_content_message(
	struct kl_window *window,
	HWND handle,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	TRACKMOUSEEVENT track;
	POINT point;
	unsigned hit;
	unsigned button;
	LRESULT result;

	/* What the message is. */
	switch (message) {
	case WM_NCHITTEST:
		/* The frame's resize bands over the content go to the frame. */
		point.x = (LONG)(short)LOWORD(lparam);
		point.y = (LONG)(short)HIWORD(lparam);
		(void)ScreenToClient(window->frame, &point);
		hit = window_hit(window, (int32_t)point.x, (int32_t)point.y);
		if (hit >= KEIUI_WIN32_HIT_LEFT && hit <= KEIUI_WIN32_HIT_BOTTOM_RIGHT)
			return HTTRANSPARENT;
		return HTCLIENT;
	case WM_MOUSEMOVE:
		/* The pointer's move; its leaving is asked for once. */
		if (!window->tracking) {
			memset(&track, 0, sizeof(track));
			track.cbSize = sizeof(track);
			track.dwFlags = TME_LEAVE;
			track.hwndTrack = handle;
			if (TrackMouseEvent(&track))
				window->tracking = 1;
		}
		window_pointer(window, KL_WINDOW_MOTION, lparam, 0U, 0);
		return 0;
	case WM_MOUSELEAVE:
		/* The pointer left. */
		window->tracking = 0;
		window_pointer(window, KL_WINDOW_LEAVE, 0, 0U, 0);
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDOWN:
	case WM_XBUTTONDBLCLK:
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
	case WM_XBUTTONUP:
		/* Which button. */
		button = KEIUI_WIN32_BUTTON_LEFT;
		if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK || message == WM_RBUTTONUP)
			button = KEIUI_WIN32_BUTTON_RIGHT;
		if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK || message == WM_MBUTTONUP)
			button = KEIUI_WIN32_BUTTON_MIDDLE;
		if (message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK || message == WM_XBUTTONUP) {
			button = KEIUI_WIN32_BUTTON_X1;
			if (HIWORD(wparam) == XBUTTON2)
				button = KEIUI_WIN32_BUTTON_X2;
		}

		/* A press on the content ends the title bar's field. */
		if (window->editing && message != WM_LBUTTONUP && message != WM_RBUTTONUP && message != WM_MBUTTONUP && message != WM_XBUTTONUP)
			keiui_win32_field_end(window, KL_TEXT_CANCELLED);

		/* Pressed: the pointer is captured while a button is held. */
		if (message != WM_LBUTTONUP && message != WM_RBUTTONUP && message != WM_MBUTTONUP && message != WM_XBUTTONUP) {
			if (window->buttons == 0U)
				(void)SetCapture(handle);
			window->buttons |= 1U << button;
			window_pointer(window, KL_WINDOW_BUTTON, lparam, keiui_win32_button_code(button), 1);
			if (message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK)
				return TRUE;
			return 0;
		}

		/* Let go: the capture ends with the last button. */
		window->buttons &= ~(1U << button);
		if (window->buttons == 0U)
			(void)ReleaseCapture();
		window_pointer(window, KL_WINDOW_BUTTON, lparam, keiui_win32_button_code(button), 0);
		if (message == WM_XBUTTONUP)
			return TRUE;
		return 0;
	case WM_CAPTURECHANGED:
		/* Another window took the pointer: no button is held any more. */
		window->buttons = 0U;
		return 0;
	case WM_SETFOCUS:
		/* The frame keeps the keyboard. */
		(void)SetFocus(window->frame);
		return 0;
	case WM_PAINT:
		/*
		 * The frame shown on the CPU.  Without one the program's Vulkan
		 * draws, and the content must be drawn again (a frame shown before
		 * the window was on the screen, under X11, is lost): the size is
		 * told again, which the program draws a frame for.
		 */
		keiui_win32_paint_content(window);
		if (window->shown == NULL && window->present == KL_PRESENT_NONE && !window->sizing)
			(void)keiui_win32_push(window, KL_WINDOW_RESIZE);
		return 0;
	case WM_ERASEBKGND:
		/* Everything is painted over. */
		return 1;
	default:
		break;
	}

	/* The system's handling of the rest. */
	result = DefWindowProcW(handle, message, wparam, lparam);
	return result;
}

/* The frame's geometry for frame.c: its client, its dpi and its states. */
static void
window_frame_box(
	const struct kl_window *window,
	struct keiui_win32_frame *frame)
{
	RECT client;

	/* The client area, which is the whole window. */
	memset(frame, 0, sizeof(*frame));
	(void)GetClientRect(window->frame, &client);
	frame->width = (uint32_t)(client.right - client.left);
	frame->height = (uint32_t)(client.bottom - client.top);
	frame->dpi = window->dpi;
	frame->maximized = window->maximized;
	frame->fullscreen = window->fullscreen;
	frame->caption_height = KEIUI_WIN32_CAPTION;
	frame->controls = window->control_places;
	frame->control_count = window->control_place_count;
}

/* frame.c's hit test at a point of the frame's client. */
static unsigned
window_hit(
	const struct kl_window *window,
	int32_t x,
	int32_t y)
{
	struct keiui_win32_frame frame;
	unsigned hit;

	/* The geometry, and the answer. */
	window_frame_box(window, &frame);
	hit = keiui_win32_frame_hit(&frame, x, y);
	return hit;
}

/* The caption button a hit names (KEIUI_WIN32_BUTTON_*_INDEX), or WINDOW_NO_BUTTON. */
static unsigned
window_button_of(
	unsigned hit)
{
	/* Each button's hit. */
	if (hit == KEIUI_WIN32_HIT_MINIMIZE)
		return KEIUI_WIN32_BUTTON_MINIMIZE_INDEX;
	if (hit == KEIUI_WIN32_HIT_MAXIMIZE)
		return KEIUI_WIN32_BUTTON_MAXIMIZE_INDEX;
	if (hit == KEIUI_WIN32_HIT_CLOSE)
		return KEIUI_WIN32_BUTTON_CLOSE_INDEX;

	/* Not a button. */
	return WINDOW_NO_BUTTON;
}

/* Lays the content out below the title bar, and tells its new size (now, or after the modal loop). */
static void
window_layout(
	struct kl_window *window)
{
	RECT client;
	uint32_t caption;
	int32_t width;
	int32_t height;

	/* A frame whose content is not made yet (its creation's first sizes). */
	if (window->content == NULL)
		return;

	/* The client area less the title bar. */
	(void)GetClientRect(window->frame, &client);
	caption = window_caption(window);
	width = (int32_t)(client.right - client.left);
	height = (int32_t)(client.bottom - client.top) - (int32_t)caption;
	if (height < 1)
		height = 1;
	if (width < 1)
		width = 1;

	/* The content there, the title bar's controls laid out, and the title bar painted again. */
	(void)MoveWindow(window->content, 0, (int)caption, (int)width, (int)height, TRUE);
	keiui_win32_titlebar_layout(window, (uint32_t)(client.right - client.left), caption);
	(void)InvalidateRect(window->frame, NULL, FALSE);

	/* The size, unless it is the one told already. */
	if ((uint32_t)width == window->width && (uint32_t)height == window->height && !window->resize_waiting)
		return;
	window->resize_waiting = 1;
	if (!window->sizing)
		window_resized(window);
}

/* Tells the content's size (a KL_WINDOW_RESIZE). */
static void
window_resized(
	struct kl_window *window)
{
	RECT client;

	/* The content's size now. */
	(void)GetClientRect(window->content, &client);
	window->width = (uint32_t)(client.right - client.left);
	window->height = (uint32_t)(client.bottom - client.top);
	window->resize_waiting = 0;

	/* The input. */
	(void)keiui_win32_push(window, KL_WINDOW_RESIZE);
}

/* A key pressed or let go, with the characters TranslateMessage queued behind a press. */
static void
window_key(
	struct kl_window *window,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	struct keiui_win32_key_flags flags;
	struct kl_window_event *input;
	uint16_t units[WINDOW_CHAR_UNITS];
	char text[KL_WINDOW_TEXT_MAX];
	uint32_t code_point;
	uint32_t code;
	size_t count;
	MSG next;
	BOOL found;
	int complete;

	/* A key the input method takes is its own. */
	if (wparam == VK_PROCESSKEY)
		return;

	/* The key's code. */
	keiui_win32_key_flags((int64_t)lparam, &flags);
	code = keiui_win32_key_code(flags.scan, flags.extended, (unsigned)wparam);

	/* The characters it types, queued right behind it (control characters are not text). */
	count = 0U;
	if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
		for (;;) {
			found = PeekMessageW(&next, window->frame, WM_CHAR, WM_CHAR, PM_REMOVE);
			if (!found)
				found = PeekMessageW(&next, window->frame, WM_SYSCHAR, WM_SYSCHAR, PM_REMOVE);
			if (!found)
				break;
			complete = keiui_win32_char_unit(&window->surrogate, (uint16_t)next.wParam, &code_point);
			if (!complete)
				continue;
			if (code_point < 0x20U || code_point == 0x7fU)
				continue;
			if (code_point > 0xffffU && count + 2U <= WINDOW_CHAR_UNITS) {
				units[count] = (uint16_t)(0xd800U + ((code_point - 0x10000U) >> 10));
				units[count + 1U] = (uint16_t)(0xdc00U + ((code_point - 0x10000U) & 0x3ffU));
				count += 2U;
			} else if (code_point <= 0xffffU && count < WINDOW_CHAR_UNITS) {
				units[count] = (uint16_t)code_point;
				count++;
			}
		}
	}

	/* While the title bar's field is edited, its keys are the field's. */
	if (window->editing) {
		if (!flags.released) {
			text[0] = '\0';
			if (count != 0U)
				(void)keiui_win32_utf8_from_utf16(units, count, text, sizeof(text), NULL);
			(void)keiui_win32_field_key(window, code, window_modifiers(), text);
		}
		return;
	}

	/* An unknown key with no text tells nothing. */
	if (code == 0U && count == 0U)
		return;

	/* The input. */
	input = keiui_win32_push(window, KL_WINDOW_KEY);
	if (input == NULL)
		return;
	input->code = code;
	input->pressed = 0;
	if (!flags.released)
		input->pressed = 1;
	input->repeated = 0;
	if (input->pressed && flags.repeated)
		input->repeated = 1;
	if (count != 0U)
		(void)keiui_win32_utf8_from_utf16(units, count, input->text, sizeof(input->text), NULL);
}

/* A character that came without its key: the text input's commit. */
static void
window_char(
	struct kl_window *window,
	WPARAM wparam)
{
	struct kl_window_event *input;
	uint16_t units[2];
	char text[16];
	uint32_t code_point;
	size_t count;
	int complete;

	/* The character, once whole; control characters are not text. */
	complete = keiui_win32_char_unit(&window->surrogate, (uint16_t)wparam, &code_point);
	if (!complete)
		return;
	if (code_point < 0x20U || code_point == 0x7fU)
		return;

	/* Its units. */
	count = 1U;
	units[0] = (uint16_t)code_point;
	if (code_point > 0xffffU) {
		units[0] = (uint16_t)(0xd800U + ((code_point - 0x10000U) >> 10));
		units[1] = (uint16_t)(0xdc00U + ((code_point - 0x10000U) & 0x3ffU));
		count = 2U;
	}

	/* The title bar's field takes it while edited. */
	if (window->editing) {
		(void)keiui_win32_utf8_from_utf16(units, count, text, sizeof(text), NULL);
		keiui_win32_field_insert(window, text);
		return;
	}

	/* The input. */
	input = keiui_win32_push(window, KL_WINDOW_TEXT_COMMIT);
	if (input == NULL)
		return;
	(void)keiui_win32_utf8_from_utf16(units, count, input->text, sizeof(input->text), NULL);
}

/* A pointer's input at a point of the content (a focus input has none: lparam 0). */
static void
window_pointer(
	struct kl_window *window,
	unsigned kind,
	LPARAM lparam,
	uint32_t code,
	int pressed)
{
	struct kl_window_event *input;
	int32_t x;
	int32_t y;

	/* The input. */
	input = keiui_win32_push(window, kind);
	if (input == NULL)
		return;
	keiui_win32_point((int64_t)lparam, &x, &y);
	input->x = (double)x;
	input->y = (double)y;
	input->code = code;
	input->pressed = pressed;
}

/* The wheel: its distance in pixels at the pointer's place on the content. */
static void
window_wheel(
	struct kl_window *window,
	WPARAM wparam,
	LPARAM lparam,
	int horizontal)
{
	struct kl_window_event *input;
	POINT point;
	int delta;

	/* The point, from the screen's to the content's. */
	point.x = (LONG)(short)LOWORD(lparam);
	point.y = (LONG)(short)HIWORD(lparam);
	(void)ScreenToClient(window->content, &point);
	delta = (int)(short)HIWORD(wparam);

	/* The input. */
	input = keiui_win32_push(window, KL_WINDOW_AXIS);
	if (input == NULL)
		return;
	input->x = (double)point.x;
	input->y = (double)point.y;
	input->axis_source = KL_AXIS_SOURCE_WHEEL;
	if (horizontal)
		input->dx = keiui_win32_wheel_pixels(delta, 1);
	else
		input->dy = keiui_win32_wheel_pixels(delta, 0);
}

/* The pointer over the caption buttons: the one under it, the one pressed, and a click. */
static void
window_caption_mouse(
	struct kl_window *window,
	UINT message,
	WPARAM wparam)
{
	TRACKMOUSEEVENT track;
	unsigned button;
	unsigned hover;
	unsigned pressed;

	/* The button under the pointer, and its leaving asked for once. */
	button = window_button_of((unsigned)wparam);
	hover = window->hover;
	pressed = window->pressed;
	window->hover = button;
	if (!window->frame_tracking) {
		memset(&track, 0, sizeof(track));
		track.cbSize = sizeof(track);
		track.dwFlags = TME_LEAVE | TME_NONCLIENT;
		track.hwndTrack = window->frame;
		if (TrackMouseEvent(&track))
			window->frame_tracking = 1;
	}

	/* A press on a button. */
	if ((message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONDBLCLK) && button != WINDOW_NO_BUTTON)
		window->pressed = button;

	/* A release: on the button pressed, its click. */
	if (message == WM_NCLBUTTONUP) {
		window->pressed = WINDOW_NO_BUTTON;
		if (button != WINDOW_NO_BUTTON && button == pressed)
			window_caption_press(window, button);
	}

	/* The title bar again when its look changed. */
	if (hover != window->hover || pressed != window->pressed)
		(void)InvalidateRect(window->frame, NULL, FALSE);
}

/* A caption button's click: minimize, maximize or restore, close. */
static void
window_caption_press(
	struct kl_window *window,
	unsigned button)
{
	BOOL zoomed;

	/* Minimize. */
	if (button == KEIUI_WIN32_BUTTON_MINIMIZE_INDEX) {
		(void)ShowWindow(window->frame, SW_MINIMIZE);
		return;
	}

	/* Maximize, or restore a maximized frame. */
	if (button == KEIUI_WIN32_BUTTON_MAXIMIZE_INDEX) {
		zoomed = IsZoomed(window->frame);
		if (zoomed)
			(void)ShowWindow(window->frame, SW_RESTORE);
		else
			(void)ShowWindow(window->frame, SW_MAXIMIZE);
		return;
	}

	/* Close, as the system's close asks. */
	(void)PostMessageW(window->frame, WM_SYSCOMMAND, SC_CLOSE, 0);
}

/* A second start's URLs (WM_COPYDATA): a KL_WINDOW_POST with their copy, NUL-terminated, for the program to free. */
static void
window_copy_data(
	struct kl_window *window,
	const COPYDATASTRUCT *copy)
{
	struct kl_window_event *input;
	char *data;

	/* Only the URLs' kind. */
	if (copy == NULL || copy->dwData != KEIUI_WIN32_COPY_URLS)
		return;

	/* The copy. */
	data = malloc((size_t)copy->cbData + 1U);
	if (data == NULL)
		return;
	if (copy->cbData != 0U)
		memcpy(data, copy->lpData, copy->cbData);
	data[copy->cbData] = '\0';

	/* The input; a full queue drops it with its copy. */
	input = keiui_win32_push(window, KL_WINDOW_POST);
	if (input == NULL) {
		free(data);
		return;
	}
	input->code = KEIUI_WIN32_COPY_URLS;
	input->data = data;
}

/* The modifiers held now (KL_MOD_*), from the keys' states. */
static unsigned
window_modifiers(void)
{
	unsigned held;
	SHORT state;
	SHORT other;
	unsigned modifiers;

	/* Each modifier key. */
	held = 0U;
	state = GetKeyState(VK_SHIFT);
	if (state < 0)
		held |= KEIUI_WIN32_HELD_SHIFT;
	state = GetKeyState(VK_CONTROL);
	if (state < 0)
		held |= KEIUI_WIN32_HELD_CONTROL;
	state = GetKeyState(VK_MENU);
	if (state < 0)
		held |= KEIUI_WIN32_HELD_ALT;
	state = GetKeyState(VK_LWIN);
	other = GetKeyState(VK_RWIN);
	if (state < 0 || other < 0)
		held |= KEIUI_WIN32_HELD_WIN;

	/* AltGr: the right Alt with the left Control the system makes with it. */
	state = GetKeyState(VK_RMENU);
	other = GetKeyState(VK_LCONTROL);
	if (state < 0 && other < 0)
		held |= KEIUI_WIN32_HELD_ALTGR;

	/* Keiland's bits. */
	modifiers = keiui_win32_modifiers(held);
	return modifiers;
}

/* The title bar's height at the frame's dpi (none in full screen). */
static uint32_t
window_caption(
	const struct kl_window *window)
{
	uint32_t caption;

	/* No title bar in full screen. */
	if (window->fullscreen)
		return 0U;

	/* The height, scaled. */
	caption = keiui_win32_scale(KEIUI_WIN32_CAPTION, window->dpi);
	return caption;
}

/* Keeps a title and shows it as the frame's text; 0, ENOMEM or EILSEQ. */
static int
window_set_title(
	struct kl_window *window,
	const char *title)
{
	char *kept;
	uint16_t *units;
	size_t length;
	size_t count;
	int error;

	/* The UTF-8 title, kept for the title bar. */
	length = strlen(title);
	kept = malloc(length + 1U);
	if (kept == NULL)
		return ENOMEM;
	memcpy(kept, title, length + 1U);
	free(window->title);
	window->title = kept;

	/* Its UTF-16 form, the frame's text. */
	units = calloc(length + 1U, sizeof(*units));
	if (units == NULL)
		return ENOMEM;
	error = keiui_win32_utf16_from_utf8(title, length, units, length + 1U, &count);
	if (error != 0) {
		free(units);
		return error;
	}
	(void)SetWindowTextW(window->frame, (LPCWSTR)units);
	free(units);

	/* The title bar again. */
	(void)InvalidateRect(window->frame, NULL, FALSE);

	/* Succeeded. */
	return 0;
}

/* Opens the title bar's font: the bundled data's fonts (none: the title bar draws no text). */
static void
window_font(
	struct kl_window *window)
{
	char directory[MAX_PATH * 3];
	char primary[MAX_PATH * 3 + 32];
	char fallback[MAX_PATH * 3 + 32];
	int error;

	/* The bundled data's directory. */
	window->text_open = 0;
	error = kl_data_directory(directory, sizeof(directory));
	if (error != 0)
		return;

	/* The sans font and its fallback. */
	(void)snprintf(primary, sizeof(primary), "%s\\fonts\\keiland.ttf", directory);
	(void)snprintf(fallback, sizeof(fallback), "%s\\fonts\\keiland-fallback.ttf", directory);
	error = kl_text_open(&window->text, primary, fallback);
	if (error == 0)
		window->text_open = 1;
}

/* The pointer over the title bar's client (its controls): the one under it, a press and a release. */
static void
window_title_mouse(
	struct kl_window *window,
	UINT message,
	LPARAM lparam)
{
	TRACKMOUSEEVENT track;
	int32_t x;
	int32_t y;
	unsigned index;

	/* Leaving: nothing under the pointer or held. */
	if (message == WM_MOUSELEAVE) {
		window->title_tracking = 0;
		keiui_win32_titlebar_release_all(window);
		return;
	}

	/* The control under the pointer, and its leaving asked for once. */
	keiui_win32_point((int64_t)lparam, &x, &y);
	index = keiui_win32_titlebar_at(window, x, y);
	if (index != window->control_hover) {
		window->control_hover = index;
		(void)InvalidateRect(window->frame, NULL, FALSE);
	}
	if (!window->title_tracking) {
		memset(&track, 0, sizeof(track));
		track.cbSize = sizeof(track);
		track.dwFlags = TME_LEAVE;
		track.hwndTrack = window->frame;
		if (TrackMouseEvent(&track))
			window->title_tracking = 1;
	}

	/* A press, and a release. */
	if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK)
		keiui_win32_titlebar_press(window, index);
	if (message == WM_LBUTTONUP)
		keiui_win32_titlebar_release(window, index);
}
