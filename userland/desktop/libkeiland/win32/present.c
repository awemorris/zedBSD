/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the window shows on Windows: the program's frames drawn on the CPU
 * (kl_window_present: copied into a DIB section of the content's size and
 * put on the screen at WM_PAINT with BitBlt), the program's own Vulkan
 * surface over the content (vkCreateWin32SurfaceKHR, looked up through the
 * loader the program loaded: libkeiland imports nothing of Vulkan), and the
 * title bar, which libkeiland draws with its canvas (the three caption
 * buttons, their hover and press in the theme's colours).
 */

#include <vulkan/vulkan.h>

#include "userland/desktop/libkeiland/win32/window-win32.h"
#include "userland/desktop/libkeiland/win32/frame.h"

/* Vulkan's Win32 surface, which needs <windows.h>'s types. */
#include <vulkan/vulkan_win32.h>

#include <errno.h>
#include <string.h>

/* The red of a close button under the pointer, and its press (the system's caption colours). */
#define PRESENT_CLOSE_HOVER	0xffc42b1cU
#define PRESENT_CLOSE_PRESSED	0xffc8483aU

/* A caption button's mark at 96 dpi: its side, and its line's thickness. */
#define PRESENT_MARK		10U
#define PRESENT_LINE		1.0f

static int present_dib(HDC context, uint32_t width, uint32_t height, HBITMAP *bitmap, uint32_t **pixels);
static void present_copy(struct kl_window *window, const uint32_t *pixels, size_t stride);
static void present_button(struct kl_canvas *canvas, const struct kl_window *window, unsigned index, const struct keiui_win32_rect *place);

/*
 * Reports the size frames are drawn at (the content's, as last told) and
 * makes the memory they are copied into that size.
 */
int
kl_window_present_resize(
	struct kl_window *window,
	uint32_t *width,
	uint32_t *height)
{
	HBITMAP bitmap;
	uint32_t *pixels;
	HDC screen;
	int error;

	/* A window, and somewhere to write. */
	if (window == NULL || width == NULL || height == NULL)
		return EINVAL;
	*width = window->width;
	*height = window->height;

	/* The memory has the size already. */
	if (window->shown != NULL && window->shown_width == window->width && window->shown_height == window->height)
		return 0;

	/* A DIB section of the size. */
	screen = GetDC(window->content);
	error = present_dib(screen, window->width, window->height, &bitmap, &pixels);
	(void)ReleaseDC(window->content, screen);
	if (error != 0)
		return error;

	/* It replaces the old one. */
	if (window->shown != NULL)
		(void)DeleteObject(window->shown);
	window->shown = bitmap;
	window->shown_pixels = pixels;
	window->shown_width = window->width;
	window->shown_height = window->height;

	/* Succeeded. */
	return 0;
}

/*
 * Shows a frame of premultiplied 0xAARRGGBB pixels (stride words a row) of
 * the size kl_window_present_resize reported.
 */
int
kl_window_present(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride)
{
	/* A window, a frame and the memory to copy it into. */
	if (window == NULL || pixels == NULL)
		return EINVAL;
	if (window->shown == NULL)
		return EINVAL;

	/* The frame, copied and painted. */
	present_copy(window, pixels, stride);
	(void)InvalidateRect(window->content, NULL, FALSE);

	/* Succeeded. */
	return 0;
}

/*
 * Shows a frame of which one part changed (the whole frame is copied; the
 * part is what is painted).
 */
int
kl_window_present_part(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *part)
{
	int error;

	/* The parts' way, with one part. */
	error = kl_window_present_parts(window, pixels, stride, part, part == NULL ? 0U : 1U);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Shows a frame of which some parts changed (count 0: the whole frame).
 */
int
kl_window_present_parts(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *parts,
	unsigned count)
{
	RECT area;
	unsigned index;

	/* A window, a frame and the memory to copy it into. */
	if (window == NULL || pixels == NULL || (count != 0U && parts == NULL))
		return EINVAL;
	if (window->shown == NULL)
		return EINVAL;

	/* The frame, copied. */
	present_copy(window, pixels, stride);

	/* The whole content painted, or each part. */
	if (count == 0U) {
		(void)InvalidateRect(window->content, NULL, FALSE);
		return 0;
	}
	for (index = 0U; index < count; index++) {
		area.left = parts[index].x;
		area.top = parts[index].y;
		area.right = parts[index].x + parts[index].width;
		area.bottom = parts[index].y + parts[index].height;
		(void)InvalidateRect(window->content, &area, FALSE);
	}

	/* Succeeded. */
	return 0;
}

/*
 * Makes a Vulkan surface over the window's content, for the program's
 * instance (which enabled kl_window_vulkan_extensions' extensions).
 */
int
kl_window_vulkan_surface(
	struct kl_window *window,
	VkInstance instance,
	VkSurfaceKHR *surface)
{
	VkWin32SurfaceCreateInfoKHR info;
	PFN_vkGetInstanceProcAddr lookup;
	PFN_vkCreateWin32SurfaceKHR create;
	PFN_vkVoidFunction address;
	FARPROC found;
	HMODULE loader;
	VkResult result;

	/* A window, an instance and somewhere to put the surface. */
	if (window == NULL || instance == VK_NULL_HANDLE || surface == NULL)
		return EINVAL;

	/* The loader the program loaded (the system's, as the program's own loading takes it). */
	loader = GetModuleHandleW(L"vulkan-1.dll");
	if (loader == NULL)
		loader = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (loader == NULL)
		return EIO;
	found = GetProcAddress(loader, "vkGetInstanceProcAddr");
	if (found == NULL)
		return EIO;
	memcpy(&lookup, &found, sizeof(lookup));

	/* The instance's entry point for Win32 surfaces. */
	address = lookup(instance, "vkCreateWin32SurfaceKHR");
	if (address == NULL)
		return EIO;
	memcpy(&create, &address, sizeof(create));

	/* The surface over the content. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	info.hinstance = window->app->module;
	info.hwnd = window->content;
	result = create(instance, &info, NULL, surface);
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded. */
	return 0;
}

/*
 * Names the instance extensions a Vulkan instance needs for a window's
 * surface: the surface extension and Win32's.
 */
int
kl_window_vulkan_extensions(
	const char *const **names,
	uint32_t *count)
{
	static const char *const extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME
	};

	/* Somewhere to put the names. */
	if (names == NULL || count == NULL)
		return EINVAL;

	/* The names, which live for the life of the program. */
	*names = extensions;
	*count = (uint32_t)(sizeof(extensions) / sizeof(extensions[0]));

	/* Succeeded. */
	return 0;
}

/* Paints the content: the last frame shown on the CPU, or nothing (the program's Vulkan draws there). */
void
keiui_win32_paint_content(
	struct kl_window *window)
{
	PAINTSTRUCT paint;
	HDC context;
	HDC memory;
	HGDIOBJ previous;

	/* The painting, which validates the content. */
	context = BeginPaint(window->content, &paint);
	if (context == NULL)
		return;

	/* The frame shown, put on the screen. */
	if (window->shown != NULL) {
		memory = CreateCompatibleDC(context);
		if (memory != NULL) {
			previous = SelectObject(memory, window->shown);
			(void)BitBlt(context, 0, 0, (int)window->shown_width, (int)window->shown_height, memory, 0, 0, SRCCOPY);
			(void)SelectObject(memory, previous);
			(void)DeleteDC(memory);
		}
	}

	/* The painting ends. */
	(void)EndPaint(window->content, &paint);
}

/* Paints the title bar: its ground and the three caption buttons, drawn with the canvas. */
void
keiui_win32_paint_caption(
	struct kl_window *window)
{
	const struct kl_theme *theme;
	struct keiui_win32_frame frame;
	struct keiui_win32_rect buttons[3];
	struct kl_canvas canvas;
	struct kl_rect ground;
	PAINTSTRUCT paint;
	RECT client;
	HDC context;
	HDC memory;
	HGDIOBJ previous;
	HBITMAP bitmap;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	unsigned index;
	int error;

	/* The painting, which validates the frame. */
	context = BeginPaint(window->frame, &paint);
	if (context == NULL)
		return;

	/* The title bar's size: the client's width, the caption's height. */
	(void)GetClientRect(window->frame, &client);
	width = (uint32_t)(client.right - client.left);
	height = keiui_win32_caption_pixels(window);
	if (width == 0U || height == 0U) {
		(void)EndPaint(window->frame, &paint);
		return;
	}

	/* Its memory, at the size. */
	if (window->caption == NULL || window->caption_width != width || window->caption_height != height) {
		error = present_dib(context, width, height, &bitmap, &pixels);
		if (error != 0) {
			(void)EndPaint(window->frame, &paint);
			return;
		}
		if (window->caption != NULL)
			(void)DeleteObject(window->caption);
		window->caption = bitmap;
		window->caption_pixels = pixels;
		window->caption_width = width;
		window->caption_height = height;
	}

	/* The ground and the buttons, drawn with the canvas. */
	error = kl_canvas_init(&canvas, window->caption_pixels, width, (int)width, (int)height);
	if (error == 0) {
		theme = kl_theme_default();
		ground.x = 0;
		ground.y = 0;
		ground.width = (int)width;
		ground.height = (int)height;
		kl_canvas_fill(&canvas, &ground, theme->ground_top);
		keiui_win32_titlebar_draw(window, &canvas, width, height);
		memset(&frame, 0, sizeof(frame));
		frame.width = width;
		frame.height = (uint32_t)(client.bottom - client.top);
		frame.dpi = window->dpi;
		frame.maximized = window->maximized;
		frame.fullscreen = window->fullscreen;
		frame.caption_height = KEIUI_WIN32_CAPTION;
		keiui_win32_frame_buttons(&frame, buttons);
		for (index = 0U; index < 3U; index++)
			present_button(&canvas, window, index, &buttons[index]);
		kl_canvas_release(&canvas);
	}

	/* Put on the screen. */
	memory = CreateCompatibleDC(context);
	if (memory != NULL) {
		previous = SelectObject(memory, window->caption);
		(void)BitBlt(context, 0, 0, (int)width, (int)height, memory, 0, 0, SRCCOPY);
		(void)SelectObject(memory, previous);
		(void)DeleteDC(memory);
	}

	/* The painting ends. */
	(void)EndPaint(window->frame, &paint);
}

/* Frees the memory of the frames shown and of the title bar. */
void
keiui_win32_present_release(
	struct kl_window *window)
{
	/* The frames' DIB section. */
	if (window->shown != NULL)
		(void)DeleteObject(window->shown);
	window->shown = NULL;
	window->shown_pixels = NULL;

	/* The title bar's. */
	if (window->caption != NULL)
		(void)DeleteObject(window->caption);
	window->caption = NULL;
	window->caption_pixels = NULL;
}

/* The title bar's height in the frame's pixels (0 in full screen). */
uint32_t
keiui_win32_caption_pixels(
	const struct kl_window *window)
{
	uint32_t height;

	/* No title bar in full screen. */
	if (window->fullscreen)
		return 0U;

	/* The caption, scaled to the dpi. */
	height = keiui_win32_scale(KEIUI_WIN32_CAPTION, window->dpi);
	return height;
}

/* Makes a top-down 32-bit DIB section; 0, or ENOMEM. */
static int
present_dib(
	HDC context,
	uint32_t width,
	uint32_t height,
	HBITMAP *bitmap,
	uint32_t **pixels)
{
	BITMAPINFO info;
	void *bits;

	/* The bitmap's shape: 32 bits, rows from the top. */
	memset(&info, 0, sizeof(info));
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = (LONG)width;
	info.bmiHeader.biHeight = -(LONG)height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	/* The section. */
	bits = NULL;
	*bitmap = CreateDIBSection(context, &info, DIB_RGB_COLORS, &bits, NULL, 0U);
	if (*bitmap == NULL || bits == NULL)
		return ENOMEM;
	*pixels = bits;

	/* Succeeded. */
	return 0;
}

/* Copies a frame into the memory shown, as much of it as both sizes hold. */
static void
present_copy(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride)
{
	uint32_t width;
	uint32_t row;

	/* The rows, one after another (the memory's rows are its width long). */
	width = window->shown_width;
	if (stride < width)
		width = (uint32_t)stride;
	(void)GdiFlush();
	for (row = 0U; row < window->shown_height; row++)
		memcpy(window->shown_pixels + (size_t)row * window->shown_width, pixels + (size_t)row * stride, (size_t)width * sizeof(uint32_t));
}

/* Draws one caption button: its ground when under the pointer or pressed, and its mark. */
static void
present_button(
	struct kl_canvas *canvas,
	const struct kl_window *window,
	unsigned index,
	const struct keiui_win32_rect *place)
{
	const struct kl_theme *theme;
	struct kl_rect ground;
	kl_color ink;
	float side;
	float line;
	float cx;
	float cy;

	/* An empty place draws nothing. */
	if (place->width == 0U || place->height == 0U)
		return;
	theme = kl_theme_default();
	ink = theme->text;

	/* Its ground under the pointer, or pressed (the close button in red). */
	ground.x = place->x;
	ground.y = place->y;
	ground.width = (int)place->width;
	ground.height = (int)place->height;
	if (window->pressed == index || window->hover == index) {
		if (index == KEIUI_WIN32_BUTTON_CLOSE_INDEX) {
			kl_canvas_fill(canvas, &ground, window->pressed == index ? PRESENT_CLOSE_PRESSED : PRESENT_CLOSE_HOVER);
			ink = 0xffffffffU;
		} else {
			kl_canvas_fill(canvas, &ground, window->pressed == index ? theme->selection : theme->hover);
		}
	}

	/* The mark at the button's middle. */
	side = (float)keiui_win32_scale(PRESENT_MARK, window->dpi);
	line = PRESENT_LINE * (float)window->dpi / 96.0f;
	cx = (float)place->x + (float)place->width / 2.0f;
	cy = (float)place->y + (float)place->height / 2.0f;
	if (index == KEIUI_WIN32_BUTTON_MINIMIZE_INDEX) {
		kl_canvas_line(canvas, cx - side / 2.0f, cy, cx + side / 2.0f, cy, line, ink);
		return;
	}
	if (index == KEIUI_WIN32_BUTTON_MAXIMIZE_INDEX) {
		if (window->maximized) {
			kl_canvas_round_border(canvas, cx - side / 2.0f, cy - side / 2.0f + 2.0f, side - 2.0f, side - 2.0f, 0.0f, line, ink);
			kl_canvas_line(canvas, cx - side / 2.0f + 2.0f, cy - side / 2.0f, cx + side / 2.0f, cy - side / 2.0f, line, ink);
			kl_canvas_line(canvas, cx + side / 2.0f, cy - side / 2.0f, cx + side / 2.0f, cy + side / 2.0f - 2.0f, line, ink);
		} else {
			kl_canvas_round_border(canvas, cx - side / 2.0f, cy - side / 2.0f, side, side, 0.0f, line, ink);
		}
		return;
	}
	kl_canvas_line(canvas, cx - side / 2.0f, cy - side / 2.0f, cx + side / 2.0f, cy + side / 2.0f, line, ink);
	kl_canvas_line(canvas, cx - side / 2.0f, cy + side / 2.0f, cx + side / 2.0f, cy - side / 2.0f, line, ink);
}
