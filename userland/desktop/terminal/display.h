/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing that shows terminal's frames, chosen when the program is
 * linked: display-vulkan.c draws them with Vulkan on the window's own
 * surface (KL_PRESENT_NONE), display-cpu.c paints them with the processor
 * and has libkeiland show them (KL_PRESENT_SHM).  Both draw the same quads
 * (render-build.c), and both show a frame painted elsewhere in the
 * grid's place (the settings tab's page, terminal_display_page).
 */

#ifndef TERMINAL_DISPLAY_H
#define TERMINAL_DISPLAY_H

#include "terminal.h"

/* The drawing of a window (the implementation's own). */
struct terminal_display;

int terminal_display_open(struct terminal_display **display, struct terminal_window *window, struct terminal_font *font);
int terminal_display_resize(struct terminal_display *display, uint32_t width, uint32_t height);
int terminal_display_draw(struct terminal_display *display, struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window);
int terminal_display_page(struct terminal_display *display, const uint32_t *pixels, uint32_t width, uint32_t height);
int terminal_display_idle(struct terminal_display *display);
void terminal_display_size(const struct terminal_display *display, uint32_t *width, uint32_t *height);
const char *terminal_display_operation(const struct terminal_display *display, int *result);
void terminal_display_close(struct terminal_display *display);

#endif
