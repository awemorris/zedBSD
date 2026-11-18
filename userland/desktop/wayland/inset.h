/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard inset protocol (kl_keyboard_inset_v1, ws102-p015,
 * plan/ws102/design.md section 2.8): the part of a window the on-screen
 * keyboard covers, told to the window before the keyboard changes it.
 */

#ifndef KERN_KEILAND_INSET_H
#define KERN_KEILAND_INSET_H

#include "kwl.h"

/* The reasons of kl_keyboard_inset_v1.inset: no keyboard, the right column's (flick), the bottom row's (QWERTY, handwriting). */
#define KWL_INSET_REASON_NONE		0U
#define KWL_INSET_REASON_RIGHT		1U
#define KWL_INSET_REASON_BOTTOM		2U

int kwl_inset_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_inset_object_gone(struct kwl_object *object);

#endif
