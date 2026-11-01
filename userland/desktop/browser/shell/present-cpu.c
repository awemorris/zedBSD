/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU presenter: the view draws the page with the CPU
 * (browser_view_draw_pixels) into memory the size of the window, and
 * libkeiland shows that memory (kl_window_present, a window made with
 * KL_PRESENT_SHM).  It is the way for a system without a GPU's Vulkan and
 * for --no-gpu.
 */

#include "shell/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int cpu_fit(struct shell_cpu *cpu, struct shell_window *window);

/*
 * Makes the frame's memory at the window's size.  Returns 0, or an error
 * number (ENOMEM, or the one libkeiland gave).
 */
int
shell_cpu_open(
	struct shell_cpu *cpu,
	struct shell_window *window)
{
	/* Nothing is owned yet. */
	memset(cpu, 0, sizeof(*cpu));

	/* The memory at the window's size. */
	return cpu_fit(cpu, window);
}

/*
 * Makes the frame's memory fit the window's new size.  Returns 0, or an
 * error number.
 */
int
shell_cpu_resize(
	struct shell_cpu *cpu,
	struct shell_window *window)
{
	/* The memory at the window's size. */
	return cpu_fit(cpu, window);
}

/*
 * Draws the view into the frame's memory and shows it in the window.
 *
 * Returns 0 once the frame is shown, EAGAIN when the window cannot take it
 * now (the caller draws it again after the next dispatch), or an error
 * number.
 */
int
shell_cpu_frame(
	struct shell_cpu *cpu,
	struct shell_window *window,
	struct browser_view *view)
{
	int error;

	/* The page, drawn into the memory. */
	error = browser_view_draw_pixels(view, cpu->pixels, cpu->width, cpu->height, (size_t)cpu->width * sizeof(uint32_t));
	if (error != 0)
		return error;

	/* The memory, shown in the window. */
	error = kl_window_present(window->kui, cpu->pixels, (size_t)cpu->width);
	if (error != 0)
		return error;

	/* Succeeded: the frame is in the window. */
	return 0;
}

/*
 * Releases the frame's memory.
 */
void
shell_cpu_close(
	struct shell_cpu *cpu)
{
	/* The memory, and nothing is owned any more. */
	free(cpu->pixels);
	memset(cpu, 0, sizeof(*cpu));
}

/* Asks libkeiland for the size frames are drawn at and makes the memory that size. */
static int
cpu_fit(
	struct shell_cpu *cpu,
	struct shell_window *window)
{
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	size_t count;
	int error;

	/* The size a frame is drawn at. */
	error = kl_window_present_resize(window->kui, &width, &height);
	if (error != 0)
		return error;
	if (width == 0U || height == 0U)
		return EINVAL;

	/* The size the memory has already. */
	if (cpu->pixels != NULL && width == cpu->width && height == cpu->height)
		return 0;

	/* Memory for the new size (a size too large to count is refused). */
	if ((size_t)width > SIZE_MAX / (size_t)height / sizeof(uint32_t))
		return ENOMEM;
	count = (size_t)width * (size_t)height;
	pixels = calloc(count, sizeof(uint32_t));
	if (pixels == NULL)
		return ENOMEM;

	/* The new memory replaces the old. */
	free(cpu->pixels);
	cpu->pixels = pixels;
	cpu->width = width;
	cpu->height = height;

	/* Succeeded: frames are drawn at the new size. */
	return 0;
}
