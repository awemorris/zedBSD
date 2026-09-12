/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Render one original textured cuboid through the public Vulkan API.
 */

#ifndef VKDEMO_RENDERER_H
#define VKDEMO_RENDERER_H

#include <stdint.h>

#define VKDEMO_WIDTH	320U
#define VKDEMO_HEIGHT	240U
#define VKDEMO_BYTES	(VKDEMO_WIDTH * VKDEMO_HEIGHT * 4U)

int vkdemo_initialize(uint32_t device_index, int offscreen);
int vkdemo_render(uint32_t milliseconds, uint32_t frame, char digest[65]);
int vkdemo_write_frame(const char *path);
int vkdemo_close(void);
const char *vkdemo_error_operation(void);
int32_t vkdemo_error_code(void);

#endif
