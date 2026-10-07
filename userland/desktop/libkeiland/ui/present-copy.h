/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The copy of a frame into the presenter's canvas by its changed part
 * (BUG-221): plain C, so the host tests can run it without Vulkan.
 */

#ifndef KEILAND_UI_PRESENT_COPY_H
#define KEILAND_UI_PRESENT_COPY_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

int keiui_present_part_clip(uint32_t width, uint32_t height, const struct kl_rect *part, struct kl_rect *clipped);
void keiui_present_copy(unsigned char *canvas, size_t pitch, const uint32_t *pixels, size_t stride, const struct kl_rect *area);

#endif
