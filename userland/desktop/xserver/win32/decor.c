/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: what a window manager does for an X
 * window's frame beyond its place and size.
 *
 * An X window's shape (SHAPE's bounding region) is shown as under an X
 * window manager: a window with Windows' frame keeps the frame whole and
 * the client area a rectangle, the part outside the shape showing the
 * window's background (a window region would cost it the frame of the
 * system's look); a window without a frame (a menu, or one whose client
 * asked for no decoration through _MOTIF_WM_HINTS) becomes its shape
 * (the window's region, SetWindowRgn), so that oclock -transparent
 * stands alone as on X.  Its _NET_WM_ICON becomes the window's icon, a
 * request for the user's attention flashes its taskbar button, an X
 * client's request to activate it brings it to the front (or flashes it
 * where Windows keeps a program from taking the front), and the core is
 * told the size of the frame around each window (_NET_FRAME_EXTENTS).
 */

#include "userland/desktop/xserver/win32/internal.h"
#include "userland/desktop/xserver/win32/scale.h"

#include <stdlib.h>
#include <string.h>

/* How many times a window that could not take the front flashes its button. */
#define DECOR_FLASH_COUNT	3U

static void decor_set_shape(struct x11_surface *surface, const struct x11_surface_info *info);
static void decor_set_icon(struct x11_surface *surface, const struct x11_surface_info *info);
static void decor_set_urgent(struct x11_surface *surface, int urgent);
static void decor_activate(struct x11_surface *surface);
static void decor_flash(struct x11_surface *surface, DWORD flags, UINT count);
static int decor_cut(const struct x11_surface *surface);
static HRGN decor_shape_region(const struct x11_surface *surface, const POINT *offset, const RECT *bounds);
static void decor_client_offset(const struct x11_surface *surface, RECT *window_rect, POINT *offset, RECT *client);

/*
 * Brings a window's shape, icon, attention and activation in step with
 * what the core changed (at open, every one).
 */
void
win32_decor_update(
	struct x11_surface *surface,
	const struct x11_surface_info *info,
	unsigned changed)
{
	int urgent;

	/* The shape, kept and applied. */
	if ((changed & X11_SURFACE_CHANGED_SHAPE) != 0U)
		decor_set_shape(surface, info);

	/* The icon. */
	if ((changed & X11_SURFACE_CHANGED_ICON) != 0U)
		decor_set_icon(surface, info);

	/* The request for the user's attention, on or off. */
	if ((changed & X11_SURFACE_CHANGED_FLAGS) != 0U) {
		urgent = 0;
		if ((info->flags & X11_SURFACE_URGENT) != 0U)
			urgent = 1;
		decor_set_urgent(surface, urgent);
	}

	/* Succeeded: activated when the client asked. */
	if ((changed & X11_SURFACE_CHANGED_ACTIVATE) != 0U)
		decor_activate(surface);
}

/*
 * Applies a window's shape again (its frame, its decoration, its scale or
 * its state changed).  A window without a frame (a menu, or a window
 * whose client asked for no decoration) is cut to the shape: the shape is
 * the window's region.  A framed window has no region, so that Windows
 * draws its frame as it draws every other window's, and the part of its
 * client area outside the shape shows the window's background, as an X
 * window manager's frame does around a shaped client.  A window without
 * a shape has neither.
 */
void
win32_decor_shape(
	struct x11_surface *surface)
{
	RECT window_rect;
	RECT client;
	POINT offset;
	HRGN region;
	HRGN outside;
	int cut;
	int set;

	/* The old outside goes. */
	if (surface->outside != NULL) {
		DeleteObject(surface->outside);
		surface->outside = NULL;
	}

	/* Unshaped: no region. */
	if (!surface->shaped) {
		SetWindowRgn(surface->hwnd, NULL, TRUE);
		return;
	}

	/* A window without a frame: cut to the shape, on the window (Windows owns the region once set). */
	cut = decor_cut(surface);
	if (cut) {
		decor_client_offset(surface, &window_rect, &offset, &client);
		region = decor_shape_region(surface, &offset, &window_rect);
		if (region == NULL)
			return;
		set = SetWindowRgn(surface->hwnd, region, TRUE);
		if (set == 0)
			DeleteObject(region);
		return;
	}

	/* A framed window: no region; the shape on the client area. */
	SetWindowRgn(surface->hwnd, NULL, TRUE);
	decor_client_offset(surface, &window_rect, &offset, &client);
	offset.x = 0;
	offset.y = 0;
	region = decor_shape_region(surface, &offset, &client);
	if (region == NULL)
		return;

	/* The client area less the shape. */
	outside = CreateRectRgn(0, 0, client.right, client.bottom);
	if (outside == NULL) {
		DeleteObject(region);
		return;
	}
	CombineRgn(outside, outside, region, RGN_DIFF);
	DeleteObject(region);
	surface->outside = outside;

	/* Succeeded: the background there, and the window painted again. */
	win32_decor_fill(surface);
	InvalidateRect(surface->hwnd, NULL, FALSE);
}

/*
 * Paints the window's background into the part of a framed shaped
 * window's DIB outside its shape, whose pixels X leaves undefined: the
 * system's colour of a window's background, as a window class without a
 * background of its own would erase it to.
 */
void
win32_decor_fill(
	struct x11_surface *surface)
{
	HBRUSH brush;

	/* Nothing outside a shape, or no pixels yet. */
	if (surface->outside == NULL || surface->dib_dc == NULL)
		return;

	/* The system's brush (never freed). */
	brush = GetSysColorBrush(COLOR_WINDOW);
	FillRgn(surface->dib_dc, surface->outside, brush);

	/* Succeeded: drawn before the DIB's pixels are written directly again. */
	GdiFlush();
}

/*
 * Tells the core the size of the frame around a window (left, right, top,
 * bottom, in X's pixels) when it changed.  Called from the message loop,
 * never from within a call of the core's.
 */
void
win32_decor_frame(
	struct x11_surface *surface)
{
	struct x11_win32 *backend;
	RECT window_rect;
	RECT client;
	POINT offset;
	unsigned frame[4];
	unsigned index;
	int same;

	/* A menu or tooltip has no frame the core speaks of. */
	backend = surface->backend;
	if (surface->kind != X11_SURFACE_TOPLEVEL || backend->server == NULL)
		return;

	/* The frame on each side, in X's pixels. */
	decor_client_offset(surface, &window_rect, &offset, &client);
	frame[0] = (unsigned)x11_win32_scale_to_x(offset.x, backend->scale);
	frame[1] = (unsigned)x11_win32_scale_to_x((window_rect.right - window_rect.left) - offset.x - client.right, backend->scale);
	frame[2] = (unsigned)x11_win32_scale_to_x(offset.y, backend->scale);
	frame[3] = (unsigned)x11_win32_scale_to_x((window_rect.bottom - window_rect.top) - offset.y - client.bottom, backend->scale);

	/* The same as told. */
	same = surface->frame_told;
	for (index = 0U; index < 4U; index++) {
		if (frame[index] != surface->frame[index])
			same = 0;
	}
	if (same)
		return;

	/* Succeeded: told. */
	surface->frame_told = 1;
	for (index = 0U; index < 4U; index++)
		surface->frame[index] = frame[index];
	x11server_backend_frame(backend->server, surface->window, frame[0], frame[1], frame[2], frame[3]);
}

/*
 * Frees what a closing window's decoration holds: its shape and its icon.
 */
void
win32_decor_free(
	struct x11_surface *surface)
{
	/* The shape and the part outside it. */
	free(surface->shape);
	surface->shape = NULL;
	surface->shape_count = 0U;
	surface->shaped = 0;
	if (surface->outside != NULL)
		DeleteObject(surface->outside);
	surface->outside = NULL;

	/* Succeeded: the icon (the window is destroyed before). */
	if (surface->icon != NULL)
		DestroyIcon(surface->icon);
	surface->icon = NULL;
}

/* Keeps the shape the core gave and applies it. */
static void
decor_set_shape(
	struct x11_surface *surface,
	const struct x11_surface_info *info)
{
	struct x11_rect *copy;

	/* The old shape goes. */
	free(surface->shape);
	surface->shape = NULL;
	surface->shape_count = 0U;
	surface->shaped = 0;

	/* The new one, copied (none: unshaped). */
	if (info->shaped) {
		copy = malloc(((size_t)info->shape_count + 1U) * sizeof(*copy));
		if (copy != NULL) {
			if (info->shape_count != 0U)
				memcpy(copy, info->shape, (size_t)info->shape_count * sizeof(*copy));
			surface->shape = copy;
			surface->shape_count = info->shape_count;
			surface->shaped = 1;
		}
	}

	/* Succeeded: applied. */
	win32_decor_shape(surface);
}

/*
 * Gives a window the icon the core gave (ARGB pixels; NULL: the
 * program's own), for its title bar and its taskbar button.
 */
static void
decor_set_icon(
	struct x11_surface *surface,
	const struct x11_surface_info *info)
{
	BITMAPINFO bitmap;
	ICONINFO parts;
	HBITMAP color;
	HBITMAP mask;
	HICON icon;
	HDC screen;
	void *bits;
	uint8_t *zeros;
	size_t mask_bytes;

	/* No icon: the program's own again. */
	icon = NULL;
	if (info->icon != NULL && info->icon_width != 0U && info->icon_height != 0U) {
		/* The colour: a top-down 32-bit bitmap of the pixels (ARGB is BGRA in memory, as Windows has it). */
		memset(&bitmap, 0, sizeof(bitmap));
		bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
		bitmap.bmiHeader.biWidth = (LONG)info->icon_width;
		bitmap.bmiHeader.biHeight = -(LONG)info->icon_height;
		bitmap.bmiHeader.biPlanes = 1;
		bitmap.bmiHeader.biBitCount = 32;
		bitmap.bmiHeader.biCompression = BI_RGB;
		screen = GetDC(NULL);
		color = CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, &bits, NULL, 0);
		ReleaseDC(NULL, screen);
		if (color == NULL)
			return;
		memcpy(bits, info->icon, (size_t)info->icon_width * (size_t)info->icon_height * 4U);

		/* The mask: all shown (the alpha decides). */
		mask_bytes = (((size_t)info->icon_width + 15U) / 16U) * 2U * (size_t)info->icon_height;
		zeros = calloc(1U, mask_bytes);
		if (zeros == NULL) {
			DeleteObject(color);
			return;
		}
		mask = CreateBitmap((int)info->icon_width, (int)info->icon_height, 1, 1, zeros);
		free(zeros);
		if (mask == NULL) {
			DeleteObject(color);
			return;
		}

		/* The icon of the two. */
		memset(&parts, 0, sizeof(parts));
		parts.fIcon = TRUE;
		parts.hbmMask = mask;
		parts.hbmColor = color;
		icon = CreateIconIndirect(&parts);
		DeleteObject(mask);
		DeleteObject(color);
		if (icon == NULL)
			return;
	}

	/* Succeeded: the window's large and small icon, the old one freed. */
	SendMessageW(surface->hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
	SendMessageW(surface->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
	if (surface->icon != NULL)
		DestroyIcon(surface->icon);
	surface->icon = icon;
}

/* Flashes a window's taskbar button while it asks for the user's attention, or stops. */
static void
decor_set_urgent(
	struct x11_surface *surface,
	int urgent)
{
	/* Nothing changed. */
	if (urgent == surface->urgent)
		return;
	surface->urgent = urgent;

	/* On: until the window comes to the front. */
	if (urgent) {
		decor_flash(surface, FLASHW_TRAY | FLASHW_TIMERNOFG, 0U);
		return;
	}

	/* Succeeded: off. */
	decor_flash(surface, FLASHW_STOP, 0U);
}

/*
 * Brings a window to the front for its X client: restored when minimized,
 * then made the foreground window; where Windows keeps the program from
 * taking the front, its button flashes instead.
 */
static void
decor_activate(
	struct x11_surface *surface)
{
	BOOL done;

	/* A menu is never activated. */
	if (surface->kind != X11_SURFACE_TOPLEVEL)
		return;

	/* The front. */
	done = SetForegroundWindow(surface->hwnd);
	if (done)
		return;

	/* Succeeded: marked instead. */
	decor_flash(surface, FLASHW_TRAY | FLASHW_CAPTION, DECOR_FLASH_COUNT);
}

/* Flashes a window's button and caption as the flags say. */
static void
decor_flash(
	struct x11_surface *surface,
	DWORD flags,
	UINT count)
{
	FLASHWINFO flash;

	/* Succeeded: asked of Windows. */
	memset(&flash, 0, sizeof(flash));
	flash.cbSize = sizeof(flash);
	flash.hwnd = surface->hwnd;
	flash.dwFlags = flags;
	flash.uCount = count;
	flash.dwTimeout = 0;
	FlashWindowEx(&flash);
}

/*
 * Tells whether a window is cut to its shape: one without Windows'
 * frame, a menu or tooltip, or a top-level window whose client asked for
 * no decoration.
 */
static int
decor_cut(
	const struct x11_surface *surface)
{
	/* A menu or a tooltip. */
	if (surface->kind == X11_SURFACE_POPUP)
		return 1;

	/* A top-level window without decoration. */
	if (surface->kind == X11_SURFACE_TOPLEVEL && (surface->flags & X11_SURFACE_UNDECORATED) != 0U)
		return 1;

	/* Succeeded: framed (or full-screen, which shows the whole window). */
	return 0;
}

/*
 * Makes the region of a window's shape: its rectangles scaled and moved
 * by offset, within bounds.  Returns the region, or NULL.
 */
static HRGN
decor_shape_region(
	const struct x11_surface *surface,
	const POINT *offset,
	const RECT *bounds)
{
	struct x11_win32 *backend;
	RGNDATA *data;
	RECT *rects;
	HRGN region;
	size_t bytes;
	unsigned index;

	/* The room for the rectangles. */
	backend = surface->backend;
	bytes = sizeof(RGNDATAHEADER) + ((size_t)surface->shape_count + 1U) * sizeof(RECT);
	data = calloc(1U, bytes);
	if (data == NULL)
		return NULL;

	/* The shape's rectangles, scaled and moved. */
	rects = (RECT *)data->Buffer;
	for (index = 0U; index < surface->shape_count; index++) {
		rects[index].left = offset->x + x11_win32_scale_to_screen(surface->shape[index].x, backend->scale);
		rects[index].top = offset->y + x11_win32_scale_to_screen(surface->shape[index].y, backend->scale);
		rects[index].right = offset->x + x11_win32_scale_to_screen(surface->shape[index].x + (int32_t)surface->shape[index].width, backend->scale);
		rects[index].bottom = offset->y + x11_win32_scale_to_screen(surface->shape[index].y + (int32_t)surface->shape[index].height, backend->scale);
	}

	/* The header. */
	data->rdh.dwSize = sizeof(RGNDATAHEADER);
	data->rdh.iType = RDH_RECTANGLES;
	data->rdh.nCount = surface->shape_count;
	data->rdh.nRgnSize = (DWORD)(surface->shape_count * sizeof(RECT));
	data->rdh.rcBound.left = 0;
	data->rdh.rcBound.top = 0;
	data->rdh.rcBound.right = bounds->right - bounds->left;
	data->rdh.rcBound.bottom = bounds->bottom - bounds->top;

	/* Succeeded: the region. */
	region = ExtCreateRegion(NULL, (DWORD)bytes, data);
	free(data);
	return region;
}

/*
 * Gives a window's rectangle on the screen, where its client area's
 * corner is from the window's corner, and the client area's size.
 */
static void
decor_client_offset(
	const struct x11_surface *surface,
	RECT *window_rect,
	POINT *offset,
	RECT *client)
{
	POINT corner;

	/* The window and its client area on the screen. */
	GetWindowRect(surface->hwnd, window_rect);
	GetClientRect(surface->hwnd, client);
	corner.x = 0;
	corner.y = 0;
	ClientToScreen(surface->hwnd, &corner);

	/* Succeeded: the client area's corner in the window. */
	offset->x = corner.x - window_rect->left;
	offset->y = corner.y - window_rect->top;
}
