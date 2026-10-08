/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PNG writer shared by the programs that hand a picture to another
 * program (ws189-p003, plan/ws189/phase001/phase.md section 4.0): the
 * image of a drag and drop.  It needs libz-compat only, so a program that
 * reads no pictures itself can compile it in.
 */

#ifndef KEILAND_PNG_WRITE_H
#define KEILAND_PNG_WRITE_H

#include <stddef.h>
#include <stdint.h>

/* The longest side a written picture may have. */
#define KL_PICTURE_PNG_SIDE_MAX	16384

int kl_picture_png(const uint32_t *pixels, int width, int height, size_t stride, unsigned char **png, size_t *size);

#endif
