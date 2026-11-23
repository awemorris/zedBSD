/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing of terminal on the processor (render-cpu.c), which
 * display-cpu.c shows the terminal's frames with: the quads of the frame
 * (render-build.c) painted into a frame of 0xAARRGGBB words as the cell
 * shader (shaders/cell.frag) colours them, from an atlas in the host's
 * memory, repainting only the bands of rows whose quads changed.
 */

#ifndef TERMINAL_RENDER_CPU_H
#define TERMINAL_RENDER_CPU_H

#include "terminal.h"

/* The most bands of a frame repainted on their own (libkeiland's parts of a frame shown). */
#define TERMINAL_CPU_BANDS	KL_FRAME_PARTS_MAX

/* A band of the frame's rows, from top (inclusive) to bottom (exclusive), in pixels. */
struct terminal_cpu_band {
	int top;
	int bottom;
};

/*
 * The processor's drawing of one window.
 *
 * pixels is the frame (width by height words, a row after another),
 * atlas the glyph atlas the font draws into (B8G8R8A8, atlas_pitch bytes a
 * row), build the quads of each frame.  previous is a copy of the
 * vertices as they were last painted (previous_count of them): a quad the
 * same as then, at the same place, covers the same pixels.  The bands of
 * the frame to paint again collect in bands (band_count of them), or
 * whole asks for the whole frame (a new size, theme or font); ground is
 * the background the frame was last cleared to.
 */
struct terminal_cpu {
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	unsigned char *atlas;
	size_t atlas_pitch;
	unsigned atlas_width;
	unsigned atlas_height;
	struct terminal_build build;
	float *previous;
	size_t previous_count;
	struct terminal_cpu_band bands[TERMINAL_CPU_BANDS];
	unsigned band_count;
	int whole;
	uint32_t ground;
};

int terminal_cpu_open(struct terminal_cpu *cpu, struct terminal_font *font);
int terminal_cpu_resize(struct terminal_cpu *cpu, uint32_t width, uint32_t height);
int terminal_cpu_draw(struct terminal_cpu *cpu, struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window, struct kl_rect *parts, unsigned *count);
void terminal_cpu_close(struct terminal_cpu *cpu);

#endif
