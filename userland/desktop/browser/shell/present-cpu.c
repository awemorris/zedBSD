/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU presenter: the drawing thread (render.c) draws the page with the
 * CPU (browser_view_render_pixels) into frames the size libkeiland shows
 * the window at, and the window's thread shows them (kl_window_present, a
 * window made with KL_PRESENT_SHM: libkeiland's calls are the window's
 * thread's).  It is the way for a system without a GPU's Vulkan and for
 * --no-gpu.
 */

#include "shell/internal.h"

#include <errno.h>
#include <string.h>

static int cpu_fit(struct shell_cpu *cpu, struct shell_window *window);

/*
 * Learns the size frames are shown at.  Returns 0, or an error number
 * (the one libkeiland gave, or EINVAL for an empty size).
 */
int
shell_cpu_open(
	struct shell_cpu *cpu,
	struct shell_window *window)
{
	/* Nothing is known yet. */
	memset(cpu, 0, sizeof(*cpu));

	/* The size of the window. */
	return cpu_fit(cpu, window);
}

/*
 * Learns the window's new size frames are shown at.  Returns 0, or an
 * error number.
 */
int
shell_cpu_resize(
	struct shell_cpu *cpu,
	struct shell_window *window)
{
	/* The size of the window. */
	return cpu_fit(cpu, window);
}

/*
 * Shows a frame the drawing thread drew in the window, on the window's
 * thread.  A frame of another size than the window's is not shown (it was
 * drawn before the window's last resize; the drawing thread draws one of
 * the new size).
 *
 * Returns 0 once the frame is shown, ESTALE for a frame of an old size,
 * EAGAIN when the window cannot take it now (the caller shows it again
 * after the next dispatch), or an error number.
 */
int
shell_cpu_show(
	struct shell_cpu *cpu,
	struct shell_window *window,
	const struct shell_cpu_frame *frame)
{
	int error;

	/* A frame of an old size. */
	if (frame->width != cpu->width || frame->height != cpu->height)
		return ESTALE;

	/* The memory, shown in the window. */
	error = kl_window_present(window->kui, frame->pixels, (size_t)frame->width);
	if (error != 0)
		return error;

	/* Succeeded: the frame is in the window. */
	return 0;
}

/*
 * Forgets the window's size.
 */
void
shell_cpu_close(
	struct shell_cpu *cpu)
{
	/* Nothing is known any more. */
	memset(cpu, 0, sizeof(*cpu));
}

/* Asks libkeiland for the size frames are shown at. */
static int
cpu_fit(
	struct shell_cpu *cpu,
	struct shell_window *window)
{
	uint32_t width;
	uint32_t height;
	int error;

	/* The size a frame is shown at. */
	error = kl_window_present_resize(window->kui, &width, &height);
	if (error != 0)
		return error;
	if (width == 0U || height == 0U)
		return EINVAL;

	/* Succeeded: frames of that size are shown from now on. */
	cpu->width = width;
	cpu->height = height;
	return 0;
}
