/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The editing operations, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_EDIT_H
#define KEILAND_EDIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct xdg_toplevel;

/*
 * The editing operations (KL_VERSION 19).
 *
 * A window says which editing operations it carries out and its state --
 * whether it has a selection, something to paste, something to undo or to
 * redo, whether a selection is being made -- and hears the operations the
 * on-screen keyboard's buttons ask for (the buttons are grey when the
 * state rules one out).  A window without it is sent the keys instead
 * (Ctrl+C, X, V, Z, Y, A).
 */
struct kl_edit;

/* The operations, as the callback hears them; a window's operations are the bits 1 << operation. */
#define KL_EDIT_COPY		0U
#define KL_EDIT_CUT		1U
#define KL_EDIT_PASTE		2U
#define KL_EDIT_UNDO		3U
#define KL_EDIT_REDO		4U
#define KL_EDIT_SELECT_ALL		5U
#define KL_EDIT_SELECT_BEGIN	6U
#define KL_EDIT_SELECT_END		7U

/* The state's bits. */
#define KL_EDIT_HAS_SELECTION	1U
#define KL_EDIT_CAN_PASTE		2U
#define KL_EDIT_CAN_UNDO		4U
#define KL_EDIT_CAN_REDO		8U
#define KL_EDIT_SELECTING		16U

/* The application's callback: the operation asked for. */
typedef void (*kl_edit_fn)(void *data, uint32_t operation);

/*
 * Asks for a window's edit object; callback runs on the application's
 * default queue.  Returns NULL with errno set: ENOTSUP for a compositor
 * without it, EINVAL, ENOMEM.
 */
struct kl_edit *kl_edit_create(struct wl_display *display, struct xdg_toplevel *toplevel, kl_edit_fn callback, void *data);

/*
 * Says the operations the window carries out (bits 1 << KL_EDIT_*) and
 * its state (KL_EDIT_HAS_SELECTION ...); an unchanged pair is not sent.
 */
void kl_edit_set_state(struct kl_edit *edit, uint32_t operations, uint32_t state);

/*
 * Stops taking operations.
 */
void kl_edit_destroy(struct kl_edit *edit);

#ifdef __cplusplus
}
#endif

#endif
