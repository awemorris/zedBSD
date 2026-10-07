/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input method program's Wayland side (ws095-p004, plan/ws095/design.md
 * section 5): the connection zdesktop gave it, the input method, its
 * keyboard grab and virtual keyboard, zdesktop's status, and the language
 * engines (engine.h) it drives.
 */

#ifndef IME_PROGRAM_H
#define IME_PROGRAM_H

#include "engine.h"

#include <keiland/keiland.h>
#include <wayland/wayland-client.h>
#include <wayland/input-method-unstable-v2-client-protocol.h>
#include <wayland/virtual-keyboard-unstable-v1-client-protocol.h>
#include "userland/desktop/libwayland/keiland-ime-status-v1-client-protocol.h"

/* The most languages the program keeps, and the evdev codes whose presses it remembers. */
#define PROGRAM_ENGINES_MAX	4U
#define PROGRAM_KEYS		768U

/*
 * How long no key may come before the engines save what they learned
 * (BUG-143, 2026-10-04 user: save when there has been no input for three
 * minutes, not at each commit).
 */
#define PROGRAM_SAVE_IDLE_MS	180000ULL

/* The candidate window's buffers: one shown while the next is drawn. */
#define PROGRAM_POPUP_BUFFERS	2U

/*
 * One buffer of the candidate window: its place in the shared memory both
 * are in, and the wl_buffer of the window's size made over it at each change.
 *
 * Busy from its attach until the compositor gives it back.
 */
struct program_popup_buffer {
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int32_t offset;
	unsigned busy;
};

/*
 * The candidate window (popup.c): the input popup surface, its buffers and
 * the fonts it draws with.
 *
 * Made once after the input method; ready stays zero when it could not be
 * made, and then no candidates are shown.
 */
struct program_popup {
	struct wl_shm *shm;
	struct wl_shm_pool *pool;
	struct wl_surface *surface;
	struct zwp_input_popup_surface_v2 *role;
	struct program_popup_buffer buffers[PROGRAM_POPUP_BUFFERS];
	struct kl_text text;
	unsigned text_open;
	unsigned ready;
	unsigned shown;
};

/*
 * The key the input method repeats while it is held (zdesktop gives the
 * grab no repeated presses, only the rate and the delay to make them by).
 *
 * The key is zero when none is held; next_ms is when the next press is due.
 */
struct program_repeat {
	int32_t rate;
	int32_t delay;
	uint32_t key;
	uint32_t time;
	unsigned long long next_ms;
};

/*
 * The input method program's whole state.
 *
 * It lives in main's frame for the program's lifetime.  The active flags
 * follow the protocol's double buffering: an activate or deactivate is
 * pending until done.  save_ms is when the engines are next told to save:
 * each key an engine is given moves it to PROGRAM_SAVE_IDLE_MS later, and
 * it is zero when no key came since the last save.
 */
struct program {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwp_input_method_manager_v2 *method_manager;
	struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
	struct kl_ime_status_manager_v1 *status_manager;
	struct zwp_input_method_v2 *method;
	struct zwp_input_method_keyboard_grab_v2 *grab;
	struct zwp_virtual_keyboard_v1 *keyboard;
	struct kl_ime_status_v1 *status;
	unsigned keymap_sent;
	struct ime_engine engines[PROGRAM_ENGINES_MAX];
	unsigned engine_count;
	unsigned current;
	char announced[32];
	unsigned active;
	unsigned pending_active;
	unsigned pending_active_set;
	uint32_t pending_hint;
	uint32_t pending_purpose;
	uint32_t done_count;
	uint32_t modifiers;
	unsigned composing;
	unsigned char passed[PROGRAM_KEYS];
	unsigned unavailable;
	struct ime_output *out;
	struct program_popup popup;
	struct program_repeat repeat;
	unsigned long long save_ms;
};

/* method.c */
int program_method_start(struct program *program);
void program_announce_language(struct program *program);
int program_repeat_timeout(const struct program *program, unsigned long long now_ms);
void program_repeat_due(struct program *program, unsigned long long now_ms);
unsigned long long program_clock_ms(void);
int program_save_timeout(const struct program *program, unsigned long long now_ms);
void program_save_due(struct program *program, unsigned long long now_ms);

/* popup.c */
int program_popup_start(struct program *program);
void program_popup_update(struct program *program, const struct ime_output *out);

/* keys.c */
uint32_t program_key_character(uint32_t code, uint32_t modifiers);
uint32_t program_key_modifiers(uint32_t modifiers);

#endif
