/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_KEY_H
#define KEILAND_KEY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The keys (input.c).  The compositor forwards evdev key codes with no
 * keymap; the library carries the US layout, as the desktop's programs do,
 * until an input method arrives.
 */
#define KL_KEY_ESC		1U
#define KL_KEY_BACKSPACE	14U
#define KL_KEY_TAB		15U
#define KL_KEY_ENTER		28U
#define KL_KEY_SPACE		57U
#define KL_KEY_KPENTER	96U
#define KL_KEY_HOME		102U
#define KL_KEY_UP		103U
#define KL_KEY_PAGEUP		104U
#define KL_KEY_LEFT		105U
#define KL_KEY_RIGHT		106U
#define KL_KEY_END		107U
#define KL_KEY_DOWN		108U
#define KL_KEY_PAGEDOWN	109U
#define KL_KEY_DELETE		111U

/* The modifiers held. */
#define KL_MOD_SHIFT		1U
#define KL_MOD_CTRL		2U
#define KL_MOD_ALT		4U
#define KL_MOD_SUPER		8U

uint32_t kl_key_character(uint32_t key, unsigned modifiers);

#ifdef __cplusplus
}
#endif

#endif
