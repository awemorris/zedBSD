/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X cursor shown over a window.
 *
 * The core tells the backend the cursor of the X window under the pointer
 * for its surface.  A standard cursor (one of the cursor font's shapes) is
 * shown as Windows' own cursor of that shape (the arrow for a shape
 * Windows has none of); any other is made from its image, scaled as the
 * window's pixels are.  The window answers WM_SETCURSOR with it.
 */

#include "userland/desktop/xserver/win32/internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * A standard cursor (the cursor font's glyph) and the cursor Windows has
 * of that shape.
 */
struct cursor_shape {
	int shape;
	LPCWSTR system;
};

/*
 * The standard cursors Windows has a cursor of (the arrow for the others).
 *
 * Values referenced: xorgproto 2024.1, include/X11/cursorfont.h (XC_X_cursor,
 * XC_arrow, XC_bottom_left_corner, XC_bottom_right_corner, XC_bottom_side,
 * XC_center_ptr, XC_circle, XC_cross, XC_crosshair, XC_double_arrow,
 * XC_fleur, XC_hand1, XC_hand2, XC_left_ptr, XC_left_side, XC_pirate,
 * XC_plus, XC_question_arrow, XC_right_ptr, XC_right_side,
 * XC_sb_h_double_arrow, XC_sb_v_double_arrow, XC_sizing, XC_tcross,
 * XC_top_left_arrow, XC_top_left_corner, XC_top_right_corner, XC_top_side,
 * XC_watch, XC_xterm).
 */
static const struct cursor_shape cursor_shapes[] = {
	{ 0, (LPCWSTR)IDC_NO },
	{ 2, (LPCWSTR)IDC_ARROW },
	{ 12, (LPCWSTR)IDC_SIZENESW },
	{ 14, (LPCWSTR)IDC_SIZENWSE },
	{ 16, (LPCWSTR)IDC_SIZENS },
	{ 22, (LPCWSTR)IDC_ARROW },
	{ 24, (LPCWSTR)IDC_NO },
	{ 30, (LPCWSTR)IDC_CROSS },
	{ 34, (LPCWSTR)IDC_CROSS },
	{ 42, (LPCWSTR)IDC_SIZENS },
	{ 52, (LPCWSTR)IDC_SIZEALL },
	{ 58, (LPCWSTR)IDC_HAND },
	{ 60, (LPCWSTR)IDC_HAND },
	{ 68, (LPCWSTR)IDC_ARROW },
	{ 70, (LPCWSTR)IDC_SIZEWE },
	{ 88, (LPCWSTR)IDC_NO },
	{ 90, (LPCWSTR)IDC_CROSS },
	{ 92, (LPCWSTR)IDC_HELP },
	{ 94, (LPCWSTR)IDC_ARROW },
	{ 96, (LPCWSTR)IDC_SIZEWE },
	{ 108, (LPCWSTR)IDC_SIZEWE },
	{ 116, (LPCWSTR)IDC_SIZENS },
	{ 120, (LPCWSTR)IDC_SIZEALL },
	{ 130, (LPCWSTR)IDC_CROSS },
	{ 132, (LPCWSTR)IDC_ARROW },
	{ 134, (LPCWSTR)IDC_SIZENWSE },
	{ 136, (LPCWSTR)IDC_SIZENESW },
	{ 138, (LPCWSTR)IDC_SIZENS },
	{ 150, (LPCWSTR)IDC_WAIT },
	{ 152, IDC_IBEAM }
};

static HCURSOR cursor_make(const struct x11_win32 *backend, const struct x11_cursor_image *image);
static LPCWSTR cursor_system(int shape);

/*
 * Sets the cursor shown over a surface (image NULL: the arrow), at once
 * when the pointer is over it.
 */
void
win32_surface_cursor(
	void *context,
	struct x11_surface *surface,
	const struct x11_cursor_image *image)
{
	struct x11_win32 *backend;
	HCURSOR cursor;
	int owned;

	/* The cursor: Windows' own of a standard shape, else one made from the image, else the arrow. */
	backend = context;
	cursor = NULL;
	owned = 0;
	if (image != NULL && image->shape >= 0) {
		cursor = LoadCursorW(NULL, cursor_system(image->shape));
	} else if (image != NULL && image->pixels != NULL) {
		cursor = cursor_make(backend, image);
		if (cursor != NULL)
			owned = 1;
	}
	if (cursor == NULL)
		cursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);

	/* The one it had goes, when the backend made it. */
	win32_cursor_free(surface);
	surface->cursor = cursor;
	surface->cursor_owned = owned;

	/* Shown at once while the pointer is over the window. */
	if (backend->pointer_surface == surface)
		SetCursor(cursor);
}

/*
 * Frees the cursor the backend made for a surface (a system cursor is
 * Windows' own), leaving the arrow.
 */
void
win32_cursor_free(
	struct x11_surface *surface)
{
	/* A cursor made from an image. */
	if (surface->cursor != NULL && surface->cursor_owned)
		DestroyCursor(surface->cursor);

	/* None: the arrow. */
	surface->cursor = NULL;
	surface->cursor_owned = 0;
}

/*
 * Makes a cursor of an image (alpha 0 or 255), scaled to the screen as
 * the windows' pixels are, with its hot spot.  Returns it, or NULL.
 */
static HCURSOR
cursor_make(
	const struct x11_win32 *backend,
	const struct x11_cursor_image *image)
{
	BITMAPINFO information;
	ICONINFO icon;
	HCURSOR cursor;
	HBITMAP color;
	HBITMAP mask;
	uint32_t *bits;
	uint8_t *mask_bits;
	void *pixels;
	size_t mask_stride;
	int width;
	int height;
	int x;
	int y;
	int source_x;
	int source_y;
	unsigned scale;

	/* The size on the screen, the image's at the backend's scale. */
	scale = backend->scale;
	if (scale == 0U)
		scale = 100U;
	if (image->width <= 0 || image->height <= 0)
		return NULL;
	width = (int)(((unsigned)image->width * scale + 50U) / 100U);
	height = (int)(((unsigned)image->height * scale + 50U) / 100U);
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;

	/* The colour bitmap: a top-down 32-bit DIB with alpha. */
	memset(&information, 0, sizeof(information));
	information.bmiHeader.biSize = sizeof(information.bmiHeader);
	information.bmiHeader.biWidth = width;
	information.bmiHeader.biHeight = -height;
	information.bmiHeader.biPlanes = 1;
	information.bmiHeader.biBitCount = 32;
	information.bmiHeader.biCompression = BI_RGB;
	pixels = NULL;
	color = CreateDIBSection(NULL, &information, DIB_RGB_COLORS, &pixels, NULL, 0);
	if (color == NULL || pixels == NULL)
		return NULL;

	/* Each pixel, the nearest of the image's (an opaque one premultiplied is itself). */
	bits = pixels;
	for (y = 0; y < height; y++) {
		source_y = y * image->height / height;
		for (x = 0; x < width; x++) {
			source_x = x * image->width / width;
			bits[(size_t)y * (size_t)width + (size_t)x] = image->pixels[(size_t)source_y * (size_t)image->width + (size_t)source_x];
		}
	}

	/* The mask bitmap: all clear, the alpha decides (rows of whole words). */
	mask_stride = (((size_t)width + 15U) / 16U) * 2U;
	mask_bits = calloc(mask_stride, (size_t)height);
	if (mask_bits == NULL) {
		DeleteObject(color);
		return NULL;
	}
	mask = CreateBitmap(width, height, 1, 1, mask_bits);
	free(mask_bits);
	if (mask == NULL) {
		DeleteObject(color);
		return NULL;
	}

	/* The cursor of both, its hot spot scaled too. */
	memset(&icon, 0, sizeof(icon));
	icon.fIcon = FALSE;
	icon.xHotspot = (DWORD)((unsigned)image->hot_x * scale / 100U);
	icon.yHotspot = (DWORD)((unsigned)image->hot_y * scale / 100U);
	icon.hbmMask = mask;
	icon.hbmColor = color;
	cursor = (HCURSOR)CreateIconIndirect(&icon);

	/* Succeeded or not, the bitmaps were copied into it. */
	DeleteObject(mask);
	DeleteObject(color);
	return cursor;
}

/* Finds the cursor Windows has of a standard shape (the arrow for one it has none of). */
static LPCWSTR
cursor_system(
	int shape)
{
	size_t index;

	/* The table's. */
	for (index = 0U; index < sizeof(cursor_shapes) / sizeof(cursor_shapes[0]); index++) {
		if (cursor_shapes[index].shape == shape)
			return cursor_shapes[index].system;
	}

	/* Another shape: the arrow. */
	return (LPCWSTR)IDC_ARROW;
}
