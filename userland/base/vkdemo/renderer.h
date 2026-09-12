/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Render one original textured cuboid through the finite Venus Vulkan API. */

#ifndef ZEDBSD_VKDEMO_RENDERER_H
#define ZEDBSD_VKDEMO_RENDERER_H

#include <stdint.h>

#define VKDEMO_WIDTH 320U
#define VKDEMO_HEIGHT 240U
#define VKDEMO_BYTES (VKDEMO_WIDTH * VKDEMO_HEIGHT * 4U)

int vkdemo_initialize(const char *device);
int vkdemo_render(uint32_t milliseconds, uint32_t frame, char digest[65]);
int vkdemo_close(void);
uint32_t vkdemo_active_command(void);

#endif
