/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD X11 input interface.
 */

#ifndef XZED_INPUT_H
#define XZED_INPUT_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define XZED_INPUT_MAX_DEVICES 8U
#define XZED_INPUT_MAX_BUTTON_EDGES 256U

#define XZED_INPUT_SHIFT_MASK (1U << 0)
#define XZED_INPUT_CONTROL_MASK (1U << 2)
#define XZED_INPUT_ALT_MASK (1U << 3)

struct xzed_input;
struct input_absinfo;

struct xzed_input_button_edge {
	uint8_t button;
	uint8_t pressed;
	uint16_t buttons;
};

struct xzed_input_pointer_frame {
	int32_t relative_x;
	int32_t relative_y;
	int absolute;
	int absolute_x;
	int absolute_y;
	uint32_t time;
	uint16_t buttons_before;
	uint16_t buttons_after;
	size_t edge_count;
	struct xzed_input_button_edge edges[XZED_INPUT_MAX_BUTTON_EDGES];
};

struct xzed_input_handlers {
	void (*key)(void *, uint8_t, int, uint32_t, uint16_t);
	void (*pointer)(void *, const struct xzed_input_pointer_frame *);
};

/*
 * This narrow I/O seam keeps the production enumeration/parser in focused
 * host tests.  Directory traversal itself is intentionally not abstracted.
 */
struct xzed_input_io {
	int (*open)(void *, const char *);
	int (*get_bits)(void *, int, unsigned, void *, size_t);
	int (*get_key_state)(void *, int, void *, size_t);
	int (*get_abs)(void *, int, unsigned, struct input_absinfo *);
	ssize_t (*read)(void *, int, void *, size_t);
	int (*close)(void *, int);
	unsigned (*pause)(void *, unsigned);
};

int xzed_input_open(struct xzed_input **, unsigned, unsigned,
	const struct xzed_input_handlers *, void *);
int xzed_input_open_with_io(struct xzed_input **, const char *, unsigned,
	unsigned, const struct xzed_input_handlers *, void *,
	const struct xzed_input_io *, void *);
void xzed_input_close(struct xzed_input *);
size_t xzed_input_pollfds(const struct xzed_input *, struct pollfd *, size_t);
int xzed_input_dispatch(struct xzed_input *, const struct pollfd *, size_t);
size_t xzed_input_device_count(const struct xzed_input *);
uint16_t xzed_input_buttons(const struct xzed_input *);
uint16_t xzed_input_modifiers(const struct xzed_input *);

#endif
