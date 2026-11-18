/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's framebuffer: guest memory a virtio display device
 * shows once the changed rows are sent to it.
 */

#ifndef KERN_HAL_M68K_VIRT_FRAMEBUFFER_H
#define KERN_HAL_M68K_VIRT_FRAMEBUFFER_H

#include <hal/types.h>

/* The grid of text cells the framebuffer holds exactly. */
#define VIRT_CONSOLE_COLUMNS	80U
#define VIRT_CONSOLE_ROWS	25U

int virt_framebuffer_init(void);
int virt_framebuffer_ready(void);
void virt_framebuffer_cell(unsigned row, unsigned column, int character, uint8_t attribute);
void virt_framebuffer_cursor(unsigned row, unsigned column);
void virt_framebuffer_flush(void);
void virt_framebuffer_describe(void);

#endif
