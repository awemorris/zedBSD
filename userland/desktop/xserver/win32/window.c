/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: the Windows window of each X top-level
 * window, its place, title and state, the GDI pixels, and the window
 * procedure.
 */

#include "userland/desktop/xserver/win32/internal.h"
#include "userland/desktop/xserver/win32/scale.h"
#include "userland/desktop/xserver/win32/text.h"

#include <shellscalingapi.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The class of the X windows. */
#define WINDOW_CLASS_NAME	L"zedBSD.X11Window"

/* The styles of a framed top-level window, a borderless one and a menu or tooltip. */
#define WINDOW_STYLE_FRAMED	(WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN)
#define WINDOW_STYLE_POPUP	(WS_POPUP | WS_CLIPCHILDREN)
#define WINDOW_EXSTYLE_POPUP	(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)

/* The message that tells the core a window's place and size from the loop. */
#define WINDOW_REPORT		(WM_APP + 1U)

static LRESULT CALLBACK window_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
static LRESULT window_message(struct x11_surface *surface, UINT message, WPARAM wparam, LPARAM lparam);
static void window_styles(const struct x11_surface *surface, DWORD *style, DWORD *extended);
static void window_outer(const struct x11_surface *surface, RECT *outer);
static void window_set_title(struct x11_surface *surface, const char *title);
static void window_set_owner(struct x11_surface *surface, struct x11_surface *owner);
static void window_sized(struct x11_surface *surface, WPARAM kind);
static void window_post_report(struct x11_surface *surface);
static void window_report(struct x11_surface *surface);
static void window_dpi_changed(struct x11_surface *surface, WPARAM wparam, const RECT *suggested);
static void window_paint(struct x11_surface *surface);
static int window_dib(struct x11_surface *surface, unsigned width, unsigned height);
static void window_dib_free(struct x11_surface *surface);
static unsigned window_x_size(unsigned size, unsigned percent);
static int32_t window_x_place(int32_t place, unsigned percent);

/*
 * Registers the window class of the X windows.  Returns 0, or an errno
 * value.
 */
int
win32_window_register(
	struct x11_win32 *backend)
{
	WNDCLASSEXW window_class;

	/* One class for every kind of surface; the styles tell them apart. */
	memset(&window_class, 0, sizeof(window_class));
	window_class.cbSize = sizeof(window_class);
	window_class.lpfnWndProc = window_procedure;
	window_class.hInstance = backend->instance;
	window_class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	window_class.lpszClassName = WINDOW_CLASS_NAME;
	backend->window_class = RegisterClassExW(&window_class);
	if (backend->window_class == 0)
		return EIO;

	/* Succeeded: the windows can be made. */
	return 0;
}

/*
 * Unregisters the class of the X windows, after the last window is gone.
 */
void
win32_window_unregister(
	struct x11_win32 *backend)
{
	/* Nothing to do for a class that was not registered. */
	if (backend->window_class == 0)
		return;

	UnregisterClassW(WINDOW_CLASS_NAME, backend->instance);
	backend->window_class = 0;
}

/*
 * Opens the Windows window of an X top-level window (the backend's open).
 *
 * A top-level window is framed and placed where X says, moved onto a
 * monitor's work area when its frame would leave it; an override-redirect
 * one is borderless, never activated and kept within the work area of
 * its monitor.  Returns the surface, or NULL (the core then shows the
 * window nowhere).
 */
struct x11_surface *
win32_surface_open(
	void *context,
	uint32_t window,
	const struct x11_surface_info *info)
{
	struct x11_win32 *backend;
	struct x11_surface *surface;
	DWORD style;
	DWORD extended;
	RECT outer;
	HWND owner;
	int show;

	/* Refuses a window past the backend's limit. */
	backend = context;
	if (backend->surface_count >= X11_WIN32_SURFACES_MAX) {
		win32_log(backend, "X11 WIN32 refused window 0x%lx: %u windows open", (unsigned long)window, backend->surface_count);
		return NULL;
	}

	/* Allocates the surface. */
	surface = calloc(1, sizeof(*surface));
	if (surface == NULL)
		return NULL;

	/* Keeps what the core gave. */
	surface->backend = backend;
	surface->window = window;
	surface->kind = info->kind;
	surface->x = info->x;
	surface->y = info->y;
	surface->width = info->width;
	surface->height = info->height;
	surface->reported_x = info->x;
	surface->reported_y = info->y;
	surface->reported_width = info->width;
	surface->reported_height = info->height;
	surface->flags = info->flags;
	surface->owner = info->owner;

	/* A menu without an owner stays above the window the pointer is in. */
	if (surface->owner == NULL && surface->kind == X11_SURFACE_POPUP)
		surface->owner = backend->pointer_surface;
	owner = NULL;
	if (surface->owner != NULL)
		owner = surface->owner->hwnd;

	/* Makes the window where X places it. */
	window_styles(surface, &style, &extended);
	window_outer(surface, &outer);
	surface->hwnd = CreateWindowExW(extended, WINDOW_CLASS_NAME, L"", style, outer.left, outer.top,
	    outer.right - outer.left, outer.bottom - outer.top, owner, NULL, backend->instance, surface);
	if (surface->hwnd == NULL) {
		win32_log(backend, "X11 WIN32 CreateWindowExW failed: %lu", (unsigned long)GetLastError());
		free(surface);
		return NULL;
	}

	/* Links the surface into the backend's list. */
	surface->next = backend->surfaces;
	backend->surfaces = surface;
	backend->surface_count++;

	/* Gives it its title. */
	window_set_title(surface, info->title);

	/* Shows it: a menu without taking the focus, a top-level window as asked. */
	show = SW_SHOWNORMAL;
	if (surface->kind == X11_SURFACE_POPUP)
		show = SW_SHOWNOACTIVATE;
	if ((surface->flags & X11_SURFACE_ICONIC) != 0) {
		show = SW_SHOWMINNOACTIVE;
		surface->iconic = 1;
	}
	ShowWindow(surface->hwnd, show);

	/* Succeeded: the core holds the surface. */
	win32_log(backend, "X11 WIN32 open window=0x%lx kind=%u at %ld,%ld %ldx%ld", (unsigned long)window, surface->kind,
	    (long)outer.left, (long)outer.top, (long)(outer.right - outer.left), (long)(outer.bottom - outer.top));
	return surface;
}

/*
 * Brings a window in step with what the core changed (the backend's
 * update): its place and size, its title, its owner and its state.
 */
void
win32_surface_update(
	void *context,
	struct x11_surface *surface,
	const struct x11_surface_info *info,
	unsigned changed)
{
	RECT outer;
	int iconic;

	/* The backend is the surface's. */
	(void)context;

	/* Moves and sizes the window to the X window's new place and size. */
	if ((changed & (X11_SURFACE_CHANGED_SIZE | X11_SURFACE_CHANGED_POSITION)) != 0) {
		surface->x = info->x;
		surface->y = info->y;
		surface->width = info->width;
		surface->height = info->height;
		surface->reported_x = info->x;
		surface->reported_y = info->y;
		surface->reported_width = info->width;
		surface->reported_height = info->height;
		window_outer(surface, &outer);
		SetWindowPos(surface->hwnd, NULL, outer.left, outer.top, outer.right - outer.left, outer.bottom - outer.top,
		    SWP_NOZORDER | SWP_NOACTIVATE);
	}

	/* Gives it its new title. */
	if ((changed & X11_SURFACE_CHANGED_TITLE) != 0)
		window_set_title(surface, info->title);

	/* Gives it its new owner. */
	if ((changed & X11_SURFACE_CHANGED_OWNER) != 0)
		window_set_owner(surface, info->owner);

	/* Minimizes or restores it as the client asked (ICCCM's WM_CHANGE_STATE). */
	if ((changed & X11_SURFACE_CHANGED_FLAGS) != 0) {
		surface->flags = info->flags;
		iconic = 0;
		if ((info->flags & X11_SURFACE_ICONIC) != 0)
			iconic = 1;
		if (iconic != surface->iconic) {
			surface->iconic = iconic;
			if (iconic) {
				ShowWindow(surface->hwnd, SW_MINIMIZE);
			} else {
				ShowWindow(surface->hwnd, SW_RESTORE);
			}
		}
	}
}

/*
 * Closes the Windows window of an X window (the backend's close).  The
 * windows it owned lose their owner first, so Windows does not destroy
 * them with it.
 */
void
win32_surface_close(
	void *context,
	struct x11_surface *surface)
{
	struct x11_win32 *backend;
	struct x11_surface **link;
	struct x11_surface *other;
	HWND focus;

	/* Unlinks the surface from the backend's list. */
	backend = context;
	link = &backend->surfaces;
	while (*link != NULL && *link != surface)
		link = &(*link)->next;
	if (*link == surface) {
		*link = surface->next;
		backend->surface_count--;
	}

	/* The pointer is no longer in it. */
	if (backend->pointer_surface == surface)
		backend->pointer_surface = NULL;

	/* Frees the windows it owns from it. */
	for (other = backend->surfaces; other != NULL; other = other->next) {
		if (other->owner == surface)
			window_set_owner(other, NULL);
	}

	/* The keys held in a window that had the keyboard are let go, as its focus would. */
	focus = GetFocus();
	if (focus == surface->hwnd)
		win32_input_release_keys(backend);

	/* Destroys the window, whose messages no longer reach the surface. */
	SetWindowLongPtrW(surface->hwnd, GWLP_USERDATA, 0);
	DestroyWindow(surface->hwnd);

	/* Frees the pixels, the cursor and the surface. */
	window_dib_free(surface);
	win32_cursor_free(surface);
	win32_log(backend, "X11 WIN32 close window=0x%lx", (unsigned long)surface->window);
	free(surface);
}

/*
 * Shows X's pixels in a window through GDI (the backend's present).
 *
 * pixels has the X window's size (the core's last), rows stride pixels
 * apart; each changed part is scaled into the window's DIB (nearest at a
 * whole scale, bilinear otherwise) and that part of the window is painted
 * again.  Returns 0, or an errno value when the DIB cannot be made.
 */
int
win32_surface_present(
	void *context,
	struct x11_surface *surface,
	const uint32_t *pixels,
	size_t stride,
	const struct x11_rect *parts,
	unsigned count)
{
	struct x11_win32 *backend;
	unsigned width;
	unsigned height;
	unsigned i;
	int whole;
	int error;
	struct x11_win32_box source;
	struct x11_win32_box box;
	RECT dirty;

	/* A window the core shows through Vulkan takes no GDI pixels. */
	backend = context;
	if (surface->vulkan)
		return 0;

	/* Keeps a DIB of the window's scaled size. */
	width = x11_win32_scale_size(surface->width, backend->scale);
	height = x11_win32_scale_size(surface->height, backend->scale);
	error = window_dib(surface, width, height);
	if (error != 0)
		return error;

	/* Scales each changed part into the DIB and paints it again. */
	whole = x11_win32_scale_is_whole(backend->scale);
	for (i = 0; i < count; i++) {
		source.x0 = parts[i].x;
		source.y0 = parts[i].y;
		source.x1 = parts[i].x + (int32_t)parts[i].width;
		source.y1 = parts[i].y + (int32_t)parts[i].height;
		x11_win32_scale_box(&source, surface->width, surface->height, width, height, &box);
		x11_win32_scale_pixels(pixels, stride, surface->width, surface->height, surface->dib_pixels, width, width, height,
		    whole, &box);

		dirty.left = box.x0;
		dirty.top = box.y0;
		dirty.right = box.x1;
		dirty.bottom = box.y1;
		InvalidateRect(surface->hwnd, &dirty, FALSE);
	}

	/* Succeeded: the window paints the parts. */
	return 0;
}

/* The window procedure of the X windows: finds the surface and hands the message on. */
static LRESULT CALLBACK
window_procedure(
	HWND hwnd,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	struct x11_surface *surface;
	const CREATESTRUCTW *create;
	LRESULT answer;

	/* The surface comes with the window's creation. */
	if (message == WM_NCCREATE) {
		create = (const CREATESTRUCTW *)lparam;
		surface = create->lpCreateParams;
		surface->hwnd = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)surface);
	}

	/* A window being destroyed, or not yet made, has no surface. */
	surface = (struct x11_surface *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (surface == NULL) {
		answer = DefWindowProcW(hwnd, message, wparam, lparam);
		return answer;
	}

	/* Succeeded: the surface answers. */
	answer = window_message(surface, message, wparam, lparam);
	return answer;
}

/* Answers one message of a surface's window. */
static LRESULT
window_message(
	struct x11_surface *surface,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	struct x11_win32 *backend;
	LRESULT answer;

	/* Answers what the X window needs; the rest is Windows'. */
	backend = surface->backend;
	switch (message) {
	case WM_CLOSE:
		/* The close button and Alt+F4 ask the X client (the core decides how). */
		if (backend->server != NULL)
			x11server_backend_close(backend->server, surface->window);
		return 0;
	case WM_SIZE:
		window_sized(surface, wparam);
		return 0;
	case WM_MOVE:
		window_post_report(surface);
		return 0;
	case WINDOW_REPORT:
		window_report(surface);
		return 0;
	case WM_ENTERSIZEMOVE:
		/* Windows runs its own loop while the user moves or sizes: the server keeps going on a timer. */
		SetTimer(surface->hwnd, WIN32_SERVICE_TIMER, WIN32_SERVICE_PERIOD, NULL);
		return 0;
	case WM_EXITSIZEMOVE:
		KillTimer(surface->hwnd, WIN32_SERVICE_TIMER);
		return 0;
	case WM_TIMER:
		if (wparam == WIN32_SERVICE_TIMER && backend->options.service != NULL)
			backend->options.service(backend->options.service_context);
		return 0;
	case WM_DPICHANGED:
		window_dpi_changed(surface, wparam, (const RECT *)lparam);
		return 0;
	case WM_PAINT:
		window_paint(surface);
		return 0;
	case WM_ERASEBKGND:
		/* The pixels cover the whole window; erasing would flicker. */
		return 1;
	case WM_KEYDOWN:
	case WM_KEYUP:
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
		/*
		 * Not given to DefWindowProc: Alt alone and F10 would enter the
		 * window menu's mode and keep the next key from X.
		 */
		win32_input_key(surface, message, wparam, lparam);
		return 0;
	case WM_CHAR:
	case WM_SYSCHAR:
	case WM_DEADCHAR:
	case WM_SYSDEADCHAR:
		/* X clients make characters from the keysyms; none of these is translated anyway. */
		return 0;
	case WM_SYSCOMMAND:
		/* The keyboard's way into the window menu is X's key. */
		if ((wparam & 0xFFF0U) == SC_KEYMENU)
			return 0;
		break;
	case WM_SETFOCUS:
		if (backend->server != NULL)
			x11server_backend_enter(backend->server, surface->window, 1);
		return 0;
	case WM_KILLFOCUS:
		/* The keys held are let go (their releases go elsewhere), and the X focus leaves the window. */
		win32_input_release_keys(backend);
		if (backend->server != NULL)
			x11server_backend_leave(backend->server, surface->window, 1);
		return 0;
	case WM_INPUTLANGCHANGE:
		/* Another layout: the keysyms are read again, and the core's clients hear of them. */
		win32_layout_read(backend);
		win32_layout_give(backend);
		break;
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		win32_input_pointer(surface, message, wparam, lparam);
		if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP)
			return TRUE;
		return 0;
	case WM_MOUSELEAVE:
		win32_input_leave(surface);
		return 0;
	case WM_MOUSEACTIVATE:
		/* A menu or tooltip never takes the focus from the window it belongs to. */
		if (surface->kind == X11_SURFACE_POPUP)
			return MA_NOACTIVATE;
		break;
	case WM_SETCURSOR:
		/* Over the X pixels, the X window's cursor (the arrow until the core gives one). */
		if (LOWORD(lparam) == HTCLIENT) {
			if (surface->cursor != NULL) {
				SetCursor(surface->cursor);
			} else {
				SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
			}
			return TRUE;
		}
		break;
	default:
		break;
	}

	/* Everything else is Windows'. */
	answer = DefWindowProcW(surface->hwnd, message, wparam, lparam);
	return answer;
}

/* Gives the window styles of a surface's kind and state. */
static void
window_styles(
	const struct x11_surface *surface,
	DWORD *style,
	DWORD *extended)
{
	/* A menu or tooltip: borderless and never activated, not on the taskbar. */
	if (surface->kind == X11_SURFACE_POPUP) {
		*style = WINDOW_STYLE_POPUP;
		*extended = WINDOW_EXSTYLE_POPUP;
		return;
	}

	/* A top-level window or the root window: Windows' own frame. */
	*style = WINDOW_STYLE_FRAMED;
	*extended = 0;
}

/*
 * Gives the outer rectangle of a surface's window: the client area where
 * X places the window, scaled, with the frame around it, moved (and for a
 * menu also cut) onto the work area of the monitor nearest to it.
 */
static void
window_outer(
	const struct x11_surface *surface,
	RECT *outer)
{
	struct x11_win32 *backend;
	DWORD style;
	DWORD extended;
	HMONITOR monitor;
	MONITORINFO information;
	UINT dpi_x;
	UINT dpi_y;
	HRESULT read;
	LONG width;
	LONG height;
	BOOL found;

	/* The client area on the screen: the virtual screen's corner is the root window's. */
	backend = surface->backend;
	outer->left = backend->virtual_screen.left + x11_win32_scale_to_screen(surface->x, backend->scale);
	outer->top = backend->virtual_screen.top + x11_win32_scale_to_screen(surface->y, backend->scale);
	outer->right = outer->left + (LONG)x11_win32_scale_size(surface->width, backend->scale);
	outer->bottom = outer->top + (LONG)x11_win32_scale_size(surface->height, backend->scale);

	/* The frame around it, at the DPI of the monitor it falls on. */
	window_styles(surface, &style, &extended);
	monitor = MonitorFromRect(outer, MONITOR_DEFAULTTONEAREST);
	read = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
	if (FAILED(read))
		dpi_x = USER_DEFAULT_SCREEN_DPI;
	AdjustWindowRectExForDpi(outer, style, FALSE, extended, dpi_x);

	/* The work area of the monitor the window is nearest to. */
	information.cbSize = sizeof(information);
	found = GetMonitorInfoW(monitor, &information);
	if (!found)
		return;

	/* A menu larger than the work area is cut to it. */
	width = outer->right - outer->left;
	height = outer->bottom - outer->top;
	if (surface->kind == X11_SURFACE_POPUP) {
		if (width > information.rcWork.right - information.rcWork.left)
			width = information.rcWork.right - information.rcWork.left;
		if (height > information.rcWork.bottom - information.rcWork.top)
			height = information.rcWork.bottom - information.rcWork.top;
	}

	/* Moves the window in from the right and bottom edges, then from the left and top ones. */
	if (outer->left + width > information.rcWork.right)
		outer->left = information.rcWork.right - width;
	if (outer->top + height > information.rcWork.bottom)
		outer->top = information.rcWork.bottom - height;
	if (outer->left < information.rcWork.left)
		outer->left = information.rcWork.left;
	if (outer->top < information.rcWork.top)
		outer->top = information.rcWork.top;
	outer->right = outer->left + width;
	outer->bottom = outer->top + height;
}

/* Gives a window its title: the X title with the host's name in brackets in front. */
static void
window_set_title(
	struct x11_surface *surface,
	const char *title)
{
	uint16_t text[X11_WIN32_TITLE_MAX];

	/* Composes and sets the title. */
	x11_win32_title(surface->backend->host, title, text, X11_WIN32_TITLE_MAX);
	SetWindowTextW(surface->hwnd, (const WCHAR *)text);
}

/* Gives a window another owner (NULL: none): it stays above the owner and is minimized with it. */
static void
window_set_owner(
	struct x11_surface *surface,
	struct x11_surface *owner)
{
	HWND owner_window;

	/* The owner's window, or none. */
	surface->owner = owner;
	owner_window = NULL;
	if (owner != NULL)
		owner_window = owner->hwnd;

	SetWindowLongPtrW(surface->hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner_window);
}

/*
 * Follows a window's change of size or state: minimized and restored are
 * ICCCM's Iconic and Normal states, which the core is told of; a size the
 * user gave is told to the core once, in X's pixels.
 */
static void
window_sized(
	struct x11_surface *surface,
	WPARAM kind)
{
	struct x11_win32 *backend;

	/* Minimized: the window becomes Iconic. */
	backend = surface->backend;
	if (kind == SIZE_MINIMIZED) {
		if (!surface->iconic) {
			surface->iconic = 1;
			if (backend->server != NULL)
				x11server_backend_state(backend->server, surface->window, 1);
		}
		return;
	}

	/* Restored: the window is Normal again. */
	if (surface->iconic) {
		surface->iconic = 0;
		if (backend->server != NULL)
			x11server_backend_state(backend->server, surface->window, 0);
	}

	/* The size the user gave is told from the loop (not from within a call of the core's). */
	window_post_report(surface);
}

/*
 * Asks for the window's place and size to be told to the core from the
 * message loop: a move or a size Windows makes while the core is calling
 * the backend (a window placed onto the work area) must not call the core
 * back from within that call.
 */
static void
window_post_report(
	struct x11_surface *surface)
{
	/* One request at a time. */
	if (surface->report_posted)
		return;

	surface->report_posted = 1;
	PostMessageW(surface->hwnd, WINDOW_REPORT, 0, 0);
}

/*
 * Tells the core the place and size of a window the user (or Windows)
 * gave it, in X's pixels, once: a place and size the core already has,
 * or was told of, is not told again.  The core takes the place as the X
 * window's, and the pointer is counted from it from then on.
 */
static void
window_report(
	struct x11_surface *surface)
{
	struct x11_win32 *backend;
	RECT client;
	POINT corner;
	unsigned width;
	unsigned height;
	int32_t x;
	int32_t y;

	/* Nothing to tell of a minimized window. */
	backend = surface->backend;
	surface->report_posted = 0;
	if (surface->iconic)
		return;

	/* The client area's place on the root window and its size, in X's pixels. */
	GetClientRect(surface->hwnd, &client);
	corner.x = 0;
	corner.y = 0;
	ClientToScreen(surface->hwnd, &corner);
	x = window_x_place(corner.x - backend->virtual_screen.left, backend->scale);
	y = window_x_place(corner.y - backend->virtual_screen.top, backend->scale);
	width = window_x_size((unsigned)(client.right - client.left), backend->scale);
	height = window_x_size((unsigned)(client.bottom - client.top), backend->scale);

	/* What the core already has, or was told, is not told again. */
	if (x == surface->reported_x && y == surface->reported_y && width == surface->reported_width &&
	    height == surface->reported_height)
		return;

	/* Tells the core; the pointer is counted from the new place. */
	surface->reported_x = x;
	surface->reported_y = y;
	surface->reported_width = width;
	surface->reported_height = height;
	surface->x = x;
	surface->y = y;
	if (backend->server != NULL)
		x11server_backend_configure(backend->server, surface->window, x, y, (int)width, (int)height);
}

/*
 * Follows a window to a monitor of another DPI: X's pixels keep their
 * size, so the client area keeps its size and only the frame changes.
 */
static void
window_dpi_changed(
	struct x11_surface *surface,
	WPARAM wparam,
	const RECT *suggested)
{
	RECT client;
	RECT outer;
	DWORD style;
	DWORD extended;

	/* The same client area with the frame of the new DPI, at the corner Windows suggests. */
	GetClientRect(surface->hwnd, &client);
	window_styles(surface, &style, &extended);
	outer = client;
	AdjustWindowRectExForDpi(&outer, style, FALSE, extended, HIWORD(wparam));
	SetWindowPos(surface->hwnd, NULL, suggested->left, suggested->top, outer.right - outer.left,
	    outer.bottom - outer.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Paints the part of a window Windows asks for from the DIB (black before any pixels). */
static void
window_paint(
	struct x11_surface *surface)
{
	PAINTSTRUCT paint;
	HDC device;

	/* Copies the DIB, or paints black before the first frame; a Vulkan window paints itself. */
	device = BeginPaint(surface->hwnd, &paint);
	if (surface->vulkan) {
		EndPaint(surface->hwnd, &paint);
		return;
	}
	if (surface->dib_dc != NULL) {
		BitBlt(device, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right - paint.rcPaint.left,
		    paint.rcPaint.bottom - paint.rcPaint.top, surface->dib_dc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
	} else {
		FillRect(device, &paint.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
	}
	EndPaint(surface->hwnd, &paint);
}

/*
 * Keeps a surface's DIB at a size: a top-down 32-bit DIB in a memory DC,
 * made again when the size changes.  Returns 0, or ENOMEM.
 */
static int
window_dib(
	struct x11_surface *surface,
	unsigned width,
	unsigned height)
{
	BITMAPINFO information;
	void *bits;
	HDC screen;

	/* The DIB of this size is there already. */
	if (surface->dib != NULL && surface->dib_width == width && surface->dib_height == height)
		return 0;

	/* Lets the old one go. */
	window_dib_free(surface);
	if (width == 0 || height == 0)
		return 0;

	/* Describes a top-down DIB of 32-bit pixels (X's depth 24 TrueColor in memory). */
	memset(&information, 0, sizeof(information));
	information.bmiHeader.biSize = sizeof(information.bmiHeader);
	information.bmiHeader.biWidth = (LONG)width;
	information.bmiHeader.biHeight = -(LONG)height;
	information.bmiHeader.biPlanes = 1;
	information.bmiHeader.biBitCount = 32;
	information.bmiHeader.biCompression = BI_RGB;

	/* Makes the DIB and a memory DC that holds it. */
	screen = GetDC(NULL);
	surface->dib = CreateDIBSection(screen, &information, DIB_RGB_COLORS, &bits, NULL, 0);
	surface->dib_dc = CreateCompatibleDC(screen);
	ReleaseDC(NULL, screen);
	if (surface->dib == NULL || surface->dib_dc == NULL) {
		window_dib_free(surface);
		return ENOMEM;
	}
	surface->dib_old = SelectObject(surface->dib_dc, surface->dib);
	surface->dib_pixels = bits;
	surface->dib_width = width;
	surface->dib_height = height;

	/* Succeeded: a black DIB of the size. */
	memset(bits, 0, (size_t)width * height * 4U);
	return 0;
}

/* Lets a surface's DIB and its DC go. */
static void
window_dib_free(
	struct x11_surface *surface)
{
	/* The DC gives the DIB back before both go. */
	if (surface->dib_dc != NULL) {
		if (surface->dib_old != NULL)
			SelectObject(surface->dib_dc, surface->dib_old);
		DeleteDC(surface->dib_dc);
	}
	if (surface->dib != NULL)
		DeleteObject(surface->dib);

	surface->dib_dc = NULL;
	surface->dib = NULL;
	surface->dib_old = NULL;
	surface->dib_pixels = NULL;
	surface->dib_width = 0;
	surface->dib_height = 0;
}

/*
 * Gives the X size of a screen size: rounded, so that the screen size of
 * an X size gives that X size back, and at least one pixel.
 */
static unsigned
window_x_size(
	unsigned size,
	unsigned percent)
{
	unsigned x_size;

	/* Divides out the scale, rounding to the nearest. */
	x_size = (unsigned)(((uint64_t)size * 100U + percent / 2U) / percent);

	/* An X window has one pixel at least. */
	if (x_size == 0)
		return 1;

	/* Succeeded: the X size. */
	return x_size;
}

/*
 * Gives the X place of a screen place on the root window: rounded to the
 * nearest (half up, the same on both sides of zero), so that the screen
 * place of an X place gives that X place back.
 */
static int32_t
window_x_place(
	int32_t place,
	unsigned percent)
{
	int64_t numerator;
	int64_t quotient;

	/* Divides out the scale with half a pixel added, rounding down. */
	numerator = (int64_t)place * 100 + (int64_t)(percent / 2U);
	quotient = numerator / (int64_t)percent;
	if (numerator % (int64_t)percent != 0 && numerator < 0)
		quotient--;

	/* Succeeded: the X place. */
	return (int32_t)quotient;
}
