/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard's XKB keymap (ws035-p078, keymap.c).
 */

#ifndef KWL_KEYMAP_H
#define KWL_KEYMAP_H

#include <stdint.h>

int kwl_keymap_open(void);
int kwl_keymap_descriptor(uint32_t *size);
const char *kwl_keymap_text(void);

#endif
