/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD X11 input component.
 */

#include "userland/X11/xzed/input.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zedbsd/input.h>

#define BITS_PER_WORD (sizeof(unsigned long) * 8U)
#define BIT_WORDS(maximum) (((maximum) + 1U + BITS_PER_WORD - 1U) / BITS_PER_WORD)
#define XZED_INPUT_FRAME_EVENTS 256U
#define XZED_INPUT_OPEN_ATTEMPTS 5U

#define XZED_ROLE_KEYBOARD 0x01U
#define XZED_ROLE_RELATIVE 0x02U
#define XZED_ROLE_ABSOLUTE 0x04U
#define XZED_ROLE_POINTER (XZED_ROLE_RELATIVE | XZED_ROLE_ABSOLUTE)

#define MOD_LEFT_SHIFT 0x01U
#define MOD_RIGHT_SHIFT 0x02U
#define MOD_LEFT_CONTROL 0x04U
#define MOD_RIGHT_CONTROL 0x08U
#define MOD_LEFT_ALT 0x10U
#define MOD_RIGHT_ALT 0x20U

struct capability_snapshot {
	unsigned long event[BIT_WORDS(EV_MAX)];
	unsigned long key[BIT_WORDS(KEY_MAX)];
	unsigned long relative[BIT_WORDS(REL_MAX)];
	unsigned long absolute[BIT_WORDS(ABS_MAX)];
};

struct xzed_input_device {
	int fd;
	unsigned roles;
	char path[PATH_MAX];
	struct input_absinfo abs_x;
	struct input_absinfo abs_y;
	uint16_t buttons;
	uint8_t modifier_keys;
	uint8_t caps_down;
	uint8_t keycodes[KEY_MAX + 1U];
	uint8_t partial[sizeof(struct input_event)];
	size_t partial_size;
	struct input_event frame[XZED_INPUT_FRAME_EVENTS];
	size_t frame_count;
	int discarding;
	int removed;
};

struct xzed_input {
	struct xzed_input_device devices[XZED_INPUT_MAX_DEVICES];
	size_t count;
	unsigned keyboards;
	unsigned pointers;
	unsigned width;
	unsigned height;
	uint16_t buttons;
	uint16_t modifiers;
	uint8_t caps_lock;
	struct xzed_input_handlers handlers;
	void *handler_context;
	struct xzed_input_io io;
	void *io_context;
};

struct key_symbol {
	uint16_t code;
	uint8_t normal;
	uint8_t shifted;
	uint8_t letter;
};

static const struct key_symbol key_symbols[] = {
	{KEY_A, 'a', 'A', 1}, {KEY_B, 'b', 'B', 1},
	{KEY_C, 'c', 'C', 1}, {KEY_D, 'd', 'D', 1},
	{KEY_E, 'e', 'E', 1}, {KEY_F, 'f', 'F', 1},
	{KEY_G, 'g', 'G', 1}, {KEY_H, 'h', 'H', 1},
	{KEY_I, 'i', 'I', 1}, {KEY_J, 'j', 'J', 1},
	{KEY_K, 'k', 'K', 1}, {KEY_L, 'l', 'L', 1},
	{KEY_M, 'm', 'M', 1}, {KEY_N, 'n', 'N', 1},
	{KEY_O, 'o', 'O', 1}, {KEY_P, 'p', 'P', 1},
	{KEY_Q, 'q', 'Q', 1}, {KEY_R, 'r', 'R', 1},
	{KEY_S, 's', 'S', 1}, {KEY_T, 't', 'T', 1},
	{KEY_U, 'u', 'U', 1}, {KEY_V, 'v', 'V', 1},
	{KEY_W, 'w', 'W', 1}, {KEY_X, 'x', 'X', 1},
	{KEY_Y, 'y', 'Y', 1}, {KEY_Z, 'z', 'Z', 1},
	{KEY_0, '0', ')', 0}, {KEY_1, '1', '!', 0},
	{KEY_2, '2', '@', 0}, {KEY_3, '3', '#', 0},
	{KEY_4, '4', '$', 0}, {KEY_5, '5', '%', 0},
	{KEY_6, '6', '^', 0}, {KEY_7, '7', '&', 0},
	{KEY_8, '8', '*', 0}, {KEY_9, '9', '(', 0},
	{KEY_SPACE, ' ', ' ', 0}, {KEY_MINUS, '-', '_', 0},
	{KEY_EQUAL, '=', '+', 0}, {KEY_LEFTBRACE, '[', '{', 0},
	{KEY_RIGHTBRACE, ']', '}', 0}, {KEY_SEMICOLON, ';', ':', 0},
	{KEY_APOSTROPHE, '\'', '"', 0}, {KEY_GRAVE, '`', '~', 0},
	{KEY_BACKSLASH, '\\', '|', 0}, {KEY_COMMA, ',', '<', 0},
	{KEY_DOT, '.', '>', 0}, {KEY_SLASH, '/', '?', 0},
};

static int event_node_name(const char *name);
static int inspect_path(struct xzed_input *input, const char *path);
static int open_candidate(struct xzed_input *input, const char *path);
static int query_capabilities(struct xzed_input *input, int fd, struct capability_snapshot *capabilities);
static unsigned classify_capabilities(const struct capability_snapshot *capabilities);
static int bit_is_set(const unsigned long *bits, unsigned code);
static int resynchronize(struct xzed_input *input, struct xzed_input_device *device, uint32_t time, int publish);
static int query_key_state(struct xzed_input *input, struct xzed_input_device *device, unsigned long *state);
static uint8_t snapshot_modifiers(const unsigned long *state);
static void recompute_modifiers(struct xzed_input *input);
static uint16_t device_modifiers(const struct xzed_input_device *device);
static uint8_t x_keycode(const struct xzed_input *input, uint16_t code);
static uint16_t snapshot_buttons(const unsigned long *state);
static uint16_t recompute_buttons(struct xzed_input *input);
static void snapshot_button_edges(struct xzed_input_pointer_frame *frame, uint16_t old, uint16_t current);
static void append_button_edge(struct xzed_input_pointer_frame *frame, uint8_t button, int pressed, uint16_t buttons);
static int scale_absolute(int32_t value, const struct input_absinfo *axis, unsigned size);
static int drain_device(struct xzed_input *input, struct xzed_input_device *device);
static int consume_bytes(struct xzed_input *input, struct xzed_input_device *device, const uint8_t *bytes, size_t length);
static int consume_event(struct xzed_input *input, struct xzed_input_device *device, const struct input_event *event);
static uint32_t event_time(const struct input_event *event);
static void process_frame(struct xzed_input *input, struct xzed_input_device *device);
static int button_info(uint16_t code, uint16_t *bit, uint8_t *button);
static void publish_button(struct xzed_input *input, struct xzed_input_device *device, const struct input_event *event, struct xzed_input_pointer_frame *frame);
static void publish_key(struct xzed_input *input, struct xzed_input_device *device, const struct input_event *event);
static int modifier_bit(uint16_t code, uint8_t *bit);
static int32_t clamp_delta(int64_t value);
static void release_device_state(struct xzed_input *input, struct xzed_input_device *device);

/*
 * Implements the xzed input open with io operation.
 */
int
xzed_input_open_with_io(
	struct xzed_input **result,
	const char *directory_name,
	unsigned width,
	unsigned height,
	const struct xzed_input_handlers *handlers,
	void *handler_context,
	const struct xzed_input_io *io,
	void *io_context)
{
	char path[PATH_MAX];
	int length;
	struct xzed_input *input;
	struct dirent *entry;
	DIR *directory;
	int error;

	error = 0;

	/* Handles the result availability. */
	if (result != NULL)
		*result = NULL;
	/* Handles the result availability. */
	if (result == NULL || directory_name == NULL || width == 0 || height == 0 ||
	    width > INT_MAX || height > INT_MAX ||
	    handlers == NULL || handlers->key == NULL || handlers->pointer == NULL ||
	    io == NULL || io->open == NULL || io->get_bits == NULL ||
	    io->get_key_state == NULL || io->get_abs == NULL || io->read == NULL ||
	    io->close == NULL || io->pause == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	input = calloc(1, sizeof(*input));

	/* Handles the input availability. */
	if (input == NULL)
		return -1;
	input->width = width;
	input->height = height;
	input->handlers = *handlers;
	input->handler_context = handler_context;
	input->io = *io;
	input->io_context = io_context;
	directory = opendir(directory_name);

	/* Handles the directory availability. */
	if (directory == NULL) {
		error = errno;
		goto fail;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		errno = 0;
		entry = readdir(directory);

		/* Handles the entry availability. */
		if (entry == NULL) {
			/* Handles the reported system error. */
			if (errno != 0)
				error = errno;
			break;
		}

		/* Handles a failed event node name operation. */
		if (!event_node_name(entry->d_name))
			continue;
		length = snprintf(path, sizeof(path), "%s/%s", directory_name,
		    entry->d_name);

		/* Checks the current data length. */
		if (length < 0 || (size_t)length >= sizeof(path)) {
			error = ENAMETOOLONG;
			break;
		}

		/* Handles a failed inspect path operation. */
		if (inspect_path(input, path) != 0) {
			error = errno;
			break;
		}
	}

	/* Handles an operation failure. */
	if (closedir(directory) != 0 && error == 0)
		error = errno;

	/* Handles an operation failure. */
	if (error == 0 && (input->keyboards == 0 || input->pointers == 0)) {
		fprintf(stderr, "Xzed: input discovery found %u keyboard(s), "
		    "%u pointer(s)\n", input->keyboards, input->pointers);
		error = ENODEV;
	}

	/* Handles an operation failure. */
	if (error != 0)
		goto fail;
	*result = input;
	/* Reports successful completion. */
	return 0;

fail:
	xzed_input_close(input);
	errno = error != 0 ? error : EIO;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the xzed input close operation.
 */
void
xzed_input_close(
	struct xzed_input *input)
{
	size_t index;

	/* Handles the input availability. */
	if (input == NULL)
		return;

	/* Process each remaining element. */
	for (index = 0; index < input->count; index++)
		(void)input->io.close(input->io_context, input->devices[index].fd);
	free(input);
}

/*
 * Implements the xzed input pollfds operation.
 */
size_t
xzed_input_pollfds(
	const struct xzed_input *input,
	struct pollfd *descriptors,
	size_t capacity)
{
	size_t index;

	/* Handles the input availability. */
	if (input == NULL || descriptors == NULL || capacity < input->count) {
		errno = EOVERFLOW;

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (index = 0; index < input->count; index++)
		descriptors[index] = (struct pollfd){input->devices[index].fd,
		    POLLIN, 0};

	/* Returns the computed result. */
	return input->count;
}

/*
 * Implements the xzed input dispatch operation.
 */
int
xzed_input_dispatch(
	struct xzed_input *input,
	const struct pollfd *descriptors,
	size_t count)
{
	short events;
	int drained;
	struct xzed_input_device *device;
	size_t index, write_index;

	/* Handles the input availability. */
	if (input == NULL || descriptors == NULL || count != input->count) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
				events = descriptors[index].revents;
				drained = 0;

		/* Handles the events condition. */
		if (events & (POLLIN | POLLHUP))
			drained = drain_device(input, &input->devices[index]);

		/* Handles the drained condition. */
		if (drained < 0)
			fprintf(stderr, "Xzed: input read %s: %s\n",
			    input->devices[index].path, strerror(errno));

		/* Handles the drained condition. */
		if (drained != 0 || (events & (POLLERR | POLLHUP | POLLNVAL)))
			input->devices[index].removed = 1;
	}

	/* Process each remaining element. */
	write_index = 0;
	for (index = 0; index < input->count; index++) {
				device = &input->devices[index];

		/* Handles the device condition. */
		if (device->removed) {
			fprintf(stderr, "Xzed: input disconnected %s\n", device->path);
			release_device_state(input, device);

			/* Handles the device condition. */
			if (device->roles & XZED_ROLE_KEYBOARD)
				input->keyboards--;

			/* Handles the device condition. */
			if (device->roles & XZED_ROLE_POINTER)
				input->pointers--;
			(void)input->io.close(input->io_context, device->fd);
			continue;
		}

		/* Handles the write index condition. */
		if (write_index != index)
			input->devices[write_index] = *device;
		write_index++;
	}
	input->count = write_index;

	/* Validates the current input. */
	if (input->keyboards == 0 || input->pointers == 0) {
		fprintf(stderr, "Xzed: required input role disappeared\n");
		errno = ENODEV;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the xzed input device count operation.
 */
size_t
xzed_input_device_count(
	const struct xzed_input *input)
{
	/* Returns the computed result. */
	return input != NULL ? input->count : 0;
}

/*
 * Implements the xzed input buttons operation.
 */
uint16_t
xzed_input_buttons(
	const struct xzed_input *input)
{
	/* Returns the computed result. */
	return input != NULL ? input->buttons : 0;
}

/*
 * Implements the xzed input modifiers operation.
 */
uint16_t
xzed_input_modifiers(
	const struct xzed_input *input)
{
	/* Returns the computed result. */
	return input != NULL ? input->modifiers : 0;
}

/* Supports the event node name operation. */
static int
event_node_name(
	const char *name)
{
	const char *cursor;

	/* Selects the matching prefix. */
	if (strncmp(name, "event", 5) != 0 || name[5] == '\0')
		return 0;

	/* Process each element required by the operation. */
	for (cursor = name + 5; *cursor != '\0'; cursor++)

		/* Checks the current cursor position. */
		if (*cursor < '0' || *cursor > '9')
			return 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the inspect path operation. */
static int
inspect_path(
	struct xzed_input *input,
	const char *path)
{
	struct capability_snapshot capabilities;
	struct xzed_input_device device;
	unsigned roles;
	int fd;

	fd = open_candidate(input, path);

	/* Checks the file descriptor. */
	if (fd < 0) {
		fprintf(stderr, "Xzed: input open %s: %s\n", path, strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed query capabilities operation. */
	if (query_capabilities(input, fd, &capabilities) != 0) {
		fprintf(stderr, "Xzed: input capabilities %s: %s\n", path,
		    strerror(errno));
		(void)input->io.close(input->io_context, fd);

		/* Reports successful completion. */
		return 0;
	}
	roles = classify_capabilities(&capabilities);
	memset(&device, 0, sizeof(device));
	device.fd = fd;
	device.roles = roles;

	/* Handles the roles condition. */
	if (roles & XZED_ROLE_ABSOLUTE) {
		/* Handles a failed get abs operation. */
		if (input->io.get_abs(input->io_context, fd, ABS_X,
		    &device.abs_x) != 0 || input->io.get_abs(input->io_context, fd,
		    ABS_Y, &device.abs_y) != 0 ||
		    device.abs_x.minimum >= device.abs_x.maximum ||
		    device.abs_y.minimum >= device.abs_y.maximum)
			device.roles &= ~XZED_ROLE_ABSOLUTE;
	}

	/* Handles the device condition. */
	if (device.roles == 0) {
		(void)input->io.close(input->io_context, fd);

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the current input. */
	if (input->count == XZED_INPUT_MAX_DEVICES) {
		(void)input->io.close(input->io_context, fd);
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed strlen operation. */
	if (strlen(path) >= sizeof(device.path)) {
		(void)input->io.close(input->io_context, fd);
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	strcpy(device.path, path);
	input->devices[input->count++] = device;

	/* Handles a failed resynchronize operation. */
	if (resynchronize(input, &input->devices[input->count - 1U], 0, 0) != 0) {
		fprintf(stderr, "Xzed: input state %s: %s\n", path, strerror(errno));
		input->count--;
		recompute_modifiers(input);
		(void)recompute_buttons(input);
		(void)input->io.close(input->io_context, fd);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the device condition. */
	if (device.roles & XZED_ROLE_KEYBOARD)
		input->keyboards++;

	/* Handles the device condition. */
	if (device.roles & XZED_ROLE_POINTER)
		input->pointers++;
	fprintf(stderr, "Xzed: input %s%s%s%s\n", path,
	    (device.roles & XZED_ROLE_KEYBOARD) ? " keyboard" : "",
	    (device.roles & XZED_ROLE_RELATIVE) ? " relative-pointer" : "",
	    (device.roles & XZED_ROLE_ABSOLUTE) ? " absolute-pointer" : "");

	/* Reports successful completion. */
	return 0;
}

/* Supports the open candidate operation. */
static int
open_candidate(
	struct xzed_input *input,
	const char *path)
{
	unsigned attempt;
	int fd;

	/* Process each element required by the operation. */
	fd = -1;
	for (attempt = 0; attempt < XZED_INPUT_OPEN_ATTEMPTS; attempt++) {
		fd = input->io.open(input->io_context, path);

		/* Handles the reported system error. */
		if (fd >= 0 || errno != ENODEV)
			break;

		/* Handles the attempt condition. */
		if (attempt + 1U < XZED_INPUT_OPEN_ATTEMPTS)
			(void)input->io.pause(input->io_context, 1);
	}

	/* Returns the computed result. */
	return fd;
}

/* Supports the query capabilities operation. */
static int
query_capabilities(
	struct xzed_input *input,
	int fd,
	struct capability_snapshot *capabilities)
{
	int function_result;

	memset(capabilities, 0, sizeof(*capabilities));

	/* Computes the function result. */
	function_result = input->io.get_bits(input->io_context, fd, 0,
	    capabilities->event, sizeof(capabilities->event)) != 0 ||
	    input->io.get_bits(input->io_context, fd, EV_KEY,
	    capabilities->key, sizeof(capabilities->key)) != 0 ||
	    input->io.get_bits(input->io_context, fd, EV_REL,
	    capabilities->relative, sizeof(capabilities->relative)) != 0 ||
	    input->io.get_bits(input->io_context, fd, EV_ABS,
	    capabilities->absolute, sizeof(capabilities->absolute)) != 0 ? -1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the classify capabilities operation. */
static unsigned
classify_capabilities(
	const struct capability_snapshot *capabilities)
{
	unsigned roles;
	int three_buttons;

	roles = 0;
	three_buttons = bit_is_set(capabilities->key, BTN_LEFT) &&
	    bit_is_set(capabilities->key, BTN_RIGHT) &&
	    bit_is_set(capabilities->key, BTN_MIDDLE);

	/* Handles a failed bit is set operation. */
	if (bit_is_set(capabilities->event, EV_KEY) &&
	    bit_is_set(capabilities->key, KEY_A) &&
	    bit_is_set(capabilities->key, KEY_Z) &&
	    bit_is_set(capabilities->key, KEY_1) &&
	    bit_is_set(capabilities->key, KEY_0) &&
	    bit_is_set(capabilities->key, KEY_ENTER) &&
	    bit_is_set(capabilities->key, KEY_SPACE) &&
	    bit_is_set(capabilities->key, KEY_LEFTSHIFT) &&
	    bit_is_set(capabilities->key, KEY_LEFTCTRL))
		roles |= XZED_ROLE_KEYBOARD;

	/* Handles a failed bit is set operation. */
	if (bit_is_set(capabilities->event, EV_KEY) &&
	    bit_is_set(capabilities->event, EV_REL) && three_buttons &&
	    bit_is_set(capabilities->relative, REL_X) &&
	    bit_is_set(capabilities->relative, REL_Y))
		roles |= XZED_ROLE_RELATIVE;

	/* Handles a failed bit is set operation. */
	if (bit_is_set(capabilities->event, EV_KEY) &&
	    bit_is_set(capabilities->event, EV_ABS) && three_buttons &&
	    bit_is_set(capabilities->absolute, ABS_X) &&
	    bit_is_set(capabilities->absolute, ABS_Y))
		roles |= XZED_ROLE_ABSOLUTE;

	/* Returns the computed result. */
	return roles;
}

/* Supports the bit is set operation. */
static int
bit_is_set(
	const unsigned long *bits,
	unsigned code)
{
	/* Returns the computed result. */
	return (bits[code / BITS_PER_WORD] &
	    (1UL << (code % BITS_PER_WORD))) != 0;
}

/* Supports the resynchronize operation. */
static int
resynchronize(
	struct xzed_input *input,
	struct xzed_input_device *device,
	uint32_t time,
	int publish)
{
	struct input_absinfo x, y;
	uint8_t old_keycode;
	int down;
	uint8_t current_keycode;
	unsigned long state[BIT_WORDS(KEY_MAX)];
	struct xzed_input_pointer_frame pointer;
	uint16_t old_buttons;
	uint16_t old_modifiers;
	unsigned code;

	old_buttons = input->buttons;
	old_modifiers = input->modifiers;

	/* Handles a failed query key state operation. */
	if (query_key_state(input, device, state) != 0)
		return -1;

	/* Handles the device condition. */
	if (device->roles & XZED_ROLE_ABSOLUTE) {
		/* Handles a failed get abs operation. */
		if (input->io.get_abs(input->io_context, device->fd, ABS_X, &x) != 0 ||
		    input->io.get_abs(input->io_context, device->fd, ABS_Y, &y) != 0)

			/* Reports operation failure. */
			return -1;

		/* Checks the current horizontal value. */
		if (x.minimum >= x.maximum || y.minimum >= y.maximum) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		device->abs_x = x;
		device->abs_y = y;
	}
	device->modifier_keys = snapshot_modifiers(state);
	device->caps_down = (uint8_t)bit_is_set(state, KEY_CAPSLOCK);
	recompute_modifiers(input);

	/* Handles the publish condition. */
	if (publish && input->modifiers != old_modifiers)
		input->handlers.key(input->handler_context, 0, 1, time,
		    input->modifiers);

	/* Handles the device condition. */
	if (device->roles & XZED_ROLE_KEYBOARD)

		/* Process each element required by the operation. */
		for (code = 0; code <= KEY_MAX; code++) {
						old_keycode = device->keycodes[code];
						down = bit_is_set(state, code);

			/* Handles the old keycode condition. */
			if (old_keycode != 0 && !down) {
				device->keycodes[code] = 0;

				/* Handles the publish condition. */
				if (publish)
					input->handlers.key(input->handler_context,
					    old_keycode, 0, time, input->modifiers);
			} else if (old_keycode == 0 && down &&
			    (current_keycode = x_keycode(input, (uint16_t)code)) != 0) {
				device->keycodes[code] = current_keycode;

				/* Handles the publish condition. */
				if (publish)
					input->handlers.key(input->handler_context,
					    current_keycode, 1, time, input->modifiers);
			}
		}
	memset(&pointer, 0, sizeof(pointer));
	pointer.time = time;
	pointer.buttons_before = old_buttons;

	/* Handles the device condition. */
	if (device->roles & XZED_ROLE_POINTER)
		device->buttons = snapshot_buttons(state);
	pointer.buttons_after = recompute_buttons(input);

	/* Handles the publish condition. */
	if (publish)
		snapshot_button_edges(&pointer, old_buttons, pointer.buttons_after);

	/* Handles the publish condition. */
	if (publish && (device->roles & XZED_ROLE_ABSOLUTE)) {
		pointer.absolute_x = scale_absolute(device->abs_x.value,
		    &device->abs_x, input->width);
		pointer.absolute_y = scale_absolute(device->abs_y.value,
		    &device->abs_y, input->height);
		pointer.absolute = 1;
	}

	/* Handles the publish condition. */
	if (publish && (pointer.absolute || pointer.edge_count != 0))
		input->handlers.pointer(input->handler_context, &pointer);

	/* Reports successful completion. */
	return 0;
}

/* Supports the query key state operation. */
static int
query_key_state(
	struct xzed_input *input,
	struct xzed_input_device *device,
	unsigned long *state)
{
	int function_result;

	memset(state, 0, BIT_WORDS(KEY_MAX) * sizeof(*state));

	/* Computes the function result. */
	function_result = input->io.get_key_state(input->io_context, device->fd, state,
	    BIT_WORDS(KEY_MAX) * sizeof(*state));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the snapshot modifiers operation. */
static uint8_t
snapshot_modifiers(
	const unsigned long *state)
{
	uint8_t result;

	result = 0;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, KEY_LEFTSHIFT)) result |= MOD_LEFT_SHIFT;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, KEY_RIGHTSHIFT)) result |= MOD_RIGHT_SHIFT;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, KEY_LEFTCTRL)) result |= MOD_LEFT_CONTROL;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, KEY_RIGHTCTRL)) result |= MOD_RIGHT_CONTROL;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, KEY_LEFTALT)) result |= MOD_LEFT_ALT;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, KEY_RIGHTALT)) result |= MOD_RIGHT_ALT;
	return result;
}

/* Supports the recompute modifiers operation. */
static void
recompute_modifiers(
	struct xzed_input *input)
{
	size_t index;
	uint16_t result;

	/* Process each remaining element. */
	result = 0;
	for (index = 0; index < input->count; index++)
		result |= device_modifiers(&input->devices[index]);
	input->modifiers = result;
}

/* Supports the device modifiers operation. */
static uint16_t
device_modifiers(
	const struct xzed_input_device *device)
{
	uint16_t result;

	result = 0;

	/* Handles the device condition. */
	if (device->modifier_keys & (MOD_LEFT_SHIFT | MOD_RIGHT_SHIFT))
		result |= XZED_INPUT_SHIFT_MASK;

	/* Handles the device condition. */
	if (device->modifier_keys & (MOD_LEFT_CONTROL | MOD_RIGHT_CONTROL))
		result |= XZED_INPUT_CONTROL_MASK;

	/* Handles the device condition. */
	if (device->modifier_keys & (MOD_LEFT_ALT | MOD_RIGHT_ALT))
		result |= XZED_INPUT_ALT_MASK;

	/* Returns the computed result. */
	return result;
}

/* Supports the x keycode operation. */
static uint8_t
x_keycode(
	const struct xzed_input *input,
	uint16_t code)
{
	int use_shift;
	size_t index;
	int shifted = (input->modifiers & XZED_INPUT_SHIFT_MASK) != 0;
	uint8_t symbol;

	symbol = 0;

	/* Dispatch the selected operation case. */
	switch (code) {
	case KEY_UP:
		/* Returns the computed result. */
		return 0xe0U;
	case KEY_DOWN:
		/* Returns the computed result. */
		return 0xe1U;
	case KEY_LEFT:
		/* Returns the computed result. */
		return 0xe2U;
	case KEY_RIGHT:
		/* Returns the computed result. */
		return 0xe3U;
	case KEY_HOME:
		/* Returns the computed result. */
		return 0xe4U;
	case KEY_END:
		/* Returns the computed result. */
		return 0xe5U;
	case KEY_PAGEUP:
		/* Returns the computed result. */
		return 0xe6U;
	case KEY_PAGEDOWN:
		/* Returns the computed result. */
		return 0xe7U;
	case KEY_INSERT:
		/* Returns the computed result. */
		return 0xe8U;
	case KEY_DELETE:
		/* Returns the computed result. */
		return 0xe9U;
	case KEY_ESC:
		symbol = 0x1bU; break;
	case KEY_BACKSPACE:
		symbol = 0x08U; break;
	case KEY_TAB:
		symbol = 0x09U; break;
	case KEY_ENTER:
		symbol = 0x0dU; break;
	default:
		/* Process each remaining element. */
		for (index = 0; index < sizeof(key_symbols) /
		    sizeof(key_symbols[0]); index++)

			/* Handles the key symbols condition. */
			if (key_symbols[index].code == code) {
								use_shift = key_symbols[index].letter ?
				    shifted ^ input->caps_lock : shifted;
				symbol = use_shift ? key_symbols[index].shifted :
				    key_symbols[index].normal;
				break;
			}
		break;
	}

	/* Returns the computed result. */
	return symbol != 0 && symbol < 248U ? (uint8_t)(symbol + 8U) : 0;
}

/* Supports the snapshot buttons operation. */
static uint16_t
snapshot_buttons(
	const unsigned long *state)
{
	uint16_t result;

	result = 0;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, BTN_LEFT)) result |= 1U << 0;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, BTN_MIDDLE)) result |= 1U << 1;

	/* Handles the bit is set condition. */
	if (bit_is_set(state, BTN_RIGHT)) result |= 1U << 2;
	return result;
}

/* Supports the recompute buttons operation. */
static uint16_t
recompute_buttons(
	struct xzed_input *input)
{
	size_t index;
	uint16_t result;

	/* Process each remaining element. */
	result = 0;
	for (index = 0; index < input->count; index++)
		result |= input->devices[index].buttons;
	input->buttons = result;

	/* Returns the computed result. */
	return result;
}

/* Supports the snapshot button edges operation. */
static void
snapshot_button_edges(
	struct xzed_input_pointer_frame *frame,
	uint16_t old,
	uint16_t current)
{
	uint16_t mask;
	uint16_t state;
	unsigned bit;

	/* Process each element required by the operation. */
	state = old;
	for (bit = 0; bit < 3U; bit++) {

		mask = (uint16_t)(1U << bit);

		/* Handles the old condition. */
		if (!((old ^ current) & mask))
			continue;

		/* Handles the current condition. */
		if (current & mask)
			state |= mask;
		else
			state &= (uint16_t)~mask;
		append_button_edge(frame, (uint8_t)(bit + 1U),
		    (current & mask) != 0, state);
	}
}

/* Supports the append button edge operation. */
static void
append_button_edge(
	struct xzed_input_pointer_frame *frame,
	uint8_t button,
	int pressed,
	uint16_t buttons)
{
	struct xzed_input_button_edge *edge;

	/* Handles the frame condition. */
	if (frame->edge_count == XZED_INPUT_MAX_BUTTON_EDGES)
		return;
	edge = &frame->edges[frame->edge_count++];
	edge->button = button;
	edge->pressed = (uint8_t)pressed;
	edge->buttons = buttons;
}

/* Supports the scale absolute operation. */
static int
scale_absolute(
	int32_t value,
	const struct input_absinfo *axis,
	unsigned size)
{
	int64_t current, minimum, maximum;
	uint64_t offset, range, extent;

	current = value;
	minimum = axis->minimum;
	maximum = axis->maximum;

	/* Checks the current data size. */
	if (size <= 1U || current <= minimum)
		return 0;

	/* Handles the current condition. */
	if (current >= maximum)
		return (int)size - 1;
	offset = (uint64_t)(current - minimum);
	range = (uint64_t)(maximum - minimum);
	extent = (uint64_t)size - 1U;

	/* Returns the computed result. */
	return (int)((offset * extent) / range);
}

/* Supports the drain device operation. */
static int
drain_device(
	struct xzed_input *input,
	struct xzed_input_device *device)
{
	ssize_t length;
	uint8_t bytes[32U * sizeof(struct input_event)];

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		length = input->io.read(input->io_context, device->fd,
		    bytes, sizeof(bytes));

		/* Checks the current data length. */
		if (length > 0) {
			/* Checks the current data length. */
			if ((size_t)length > sizeof(bytes)) {
				errno = EIO;

				/* Reports operation failure. */
				return -1;
			}

			/* Handles a failed consume bytes operation. */
			if (consume_bytes(input, device, bytes, (size_t)length) != 0)
				return -1;
			continue;
		}

		/* Checks the current data length. */
		if (length == 0) {
			/* Handles the device condition. */
			if (device->partial_size != 0) {
				errno = EIO;

				/* Reports operation failure. */
				return -1;
			}

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the reported system error. */
		if (errno == EINTR)
			continue;

		/* Handles the reported system error. */
		if (errno == EAGAIN)
			return 0;

		/* Reports operation failure. */
		return -1;
	}
}

/* Supports the consume bytes operation. */
static int
consume_bytes(
	struct xzed_input *input,
	struct xzed_input_device *device,
	const uint8_t *bytes,
	size_t length)
{
	struct input_event event;
	size_t count;

	/* Handles the device condition. */
	if (device->partial_size != 0) {
		count = sizeof(event) - device->partial_size;

		/* Checks the remaining item count. */
		if (count > length)
			count = length;
		memcpy(device->partial + device->partial_size, bytes, count);
		device->partial_size += count;
		bytes += count;
		length -= count;

		/* Handles the device condition. */
		if (device->partial_size == sizeof(event)) {
			memcpy(&event, device->partial, sizeof(event));
			device->partial_size = 0;

			/* Handles a failed consume event operation. */
			if (consume_event(input, device, &event) != 0)
				return -1;
		}
	}
	while (length >= sizeof(event)) {
		memcpy(&event, bytes, sizeof(event));

		/* Handles a failed consume event operation. */
		if (consume_event(input, device, &event) != 0)
			return -1;
		bytes += sizeof(event);
		length -= sizeof(event);
	}

	/* Checks the current data length. */
	if (length != 0) {
		memcpy(device->partial, bytes, length);
		device->partial_size = length;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the consume event operation. */
static int
consume_event(
	struct xzed_input *input,
	struct xzed_input_device *device,
	const struct input_event *event)
{
	int function_result;

	/* Handles the event condition. */
	if (event->type == EV_SYN && event->code == SYN_DROPPED) {
		device->frame_count = 0;
		device->discarding = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the event condition. */
	if (event->type == EV_SYN && event->code == SYN_REPORT) {
		/* Handles the device condition. */
		if (device->discarding) {
			device->discarding = 0;
			device->frame_count = 0;

			/* Obtains the resynchronize result. */
			function_result = resynchronize(input, device, event_time(event), 1);

			/* Returns the computed result. */
			return function_result;
		}
		process_frame(input, device);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the device condition. */
	if (device->discarding || event->type == EV_SYN)
		return 0;

	/* Handles the device condition. */
	if (device->frame_count == XZED_INPUT_FRAME_EVENTS) {
		device->frame_count = 0;
		device->discarding = 1;

		/* Reports successful completion. */
		return 0;
	}
	device->frame[device->frame_count++] = *event;

	/* Reports successful completion. */
	return 0;
}

/* Supports the event time operation. */
static uint32_t
event_time(
	const struct input_event *event)
{
	uint64_t seconds, microseconds;

	/* Handles the event condition. */
	if (event->time.tv_sec < 0)
		return 0;
	seconds = (uint64_t)event->time.tv_sec;
	microseconds = event->time.tv_usec < 0 ? 0U :
	    (uint64_t)event->time.tv_usec;

	/* Handles the microseconds condition. */
	if (microseconds > 999999U)
		microseconds = 999999U;

	/* Returns the computed result. */
	return (uint32_t)(seconds * 1000U + microseconds / 1000U);
}

/* Supports the process frame operation. */
static void
process_frame(
	struct xzed_input *input,
	struct xzed_input_device *device)
{
	const struct input_event *event;
	uint16_t button_bit;
	uint8_t button;
	struct xzed_input_pointer_frame pointer;
	int64_t relative_x, relative_y;
	int absolute_seen, pointer_event;
	size_t index;

	relative_x = 0;
	relative_y = 0;
	absolute_seen = 0;
	pointer_event = 0;
	memset(&pointer, 0, sizeof(pointer));

	/* Process each remaining element. */
	pointer.buttons_before = input->buttons;
	for (index = 0; index < device->frame_count; index++) {
				event = &device->frame[index];

		/* Handles a failed button info operation. */
		if (event->type == EV_KEY &&
		    (device->roles & XZED_ROLE_POINTER) &&
		    button_info(event->code, &button_bit, &button)) {
			publish_button(input, device, event, &pointer);
			pointer.time = event_time(event);
			pointer_event = 1;
		} else if (event->type == EV_KEY &&
		    (device->roles & XZED_ROLE_KEYBOARD)) {
			publish_key(input, device, event);
		} else if (event->type == EV_REL &&
		    (device->roles & XZED_ROLE_RELATIVE)) {
			/* Handles the event condition. */
			if (event->code == REL_X)
				relative_x += event->value;
			else if (event->code == REL_Y)
				relative_y += event->value;
			else
				continue;
			pointer.time = event_time(event);
			pointer_event = 1;
		} else if (event->type == EV_ABS &&
		    (device->roles & XZED_ROLE_ABSOLUTE)) {
			/* Handles the event condition. */
			if (event->code == ABS_X) {
				absolute_seen = 1;
				device->abs_x.value = event->value;
			} else if (event->code == ABS_Y) {
				absolute_seen = 1;
				device->abs_y.value = event->value;
			} else
				continue;
			pointer.time = event_time(event);
			pointer_event = 1;
		}
	}
	pointer.relative_x = clamp_delta(relative_x);
	pointer.relative_y = clamp_delta(relative_y);

	/* Handles the absolute seen condition. */
	if (absolute_seen) {
		pointer.absolute = 1;
		pointer.absolute_x = scale_absolute(device->abs_x.value,
		    &device->abs_x, input->width);
		pointer.absolute_y = scale_absolute(device->abs_y.value,
		    &device->abs_y, input->height);
	}
	pointer.buttons_after = input->buttons;

	/* Handles the pointer event condition. */
	if (pointer_event && (pointer.relative_x != 0 || pointer.relative_y != 0 ||
	    pointer.absolute || pointer.edge_count != 0))
		input->handlers.pointer(input->handler_context, &pointer);
	device->frame_count = 0;
}

/* Supports the button info operation. */
static int
button_info(
	uint16_t code,
	uint16_t *bit,
	uint8_t *button)
{
	/* Dispatch the selected operation case. */
	switch (code) {
	case BTN_LEFT:
		*bit = 1U << 0; *button = 1; return 1;
	case BTN_MIDDLE:
		*bit = 1U << 1; *button = 2; return 1;
	case BTN_RIGHT:
		*bit = 1U << 2; *button = 3; return 1;
	default:
		/* Reports successful completion. */
		return 0;
	}
}

/* Supports the publish button operation. */
static void
publish_button(
	struct xzed_input *input,
	struct xzed_input_device *device,
	const struct input_event *event,
	struct xzed_input_pointer_frame *frame)
{
	uint16_t bit, previous, current;
	uint8_t button;
	int pressed;

	/* Handles a failed button info operation. */
	if (!button_info(event->code, &bit, &button) ||
	    event->value < 0 || event->value > 2)

		/* Returns the computed result. */
		return;
	pressed = event->value != 0;

	/* Handles the device condition. */
	if (((device->buttons & bit) != 0) == pressed)
		return;
	previous = input->buttons;

	/* Handles the pressed condition. */
	if (pressed)
		device->buttons |= bit;
	else
		device->buttons &= (uint16_t)~bit;
	current = recompute_buttons(input);

	/* Handles the previous condition. */
	if ((previous ^ current) & bit)
		append_button_edge(frame, button, (current & bit) != 0, current);
}

/* Supports the publish key operation. */
static void
publish_key(
	struct xzed_input *input,
	struct xzed_input_device *device,
	const struct input_event *event)
{
	uint8_t bit, keycode;
	uint16_t old_modifiers;

	/* Handles the event condition. */
	if (event->value < 0 || event->value > 2 || event->code > KEY_MAX)
		return;

	/* Handles a failed modifier bit operation. */
	if (modifier_bit(event->code, &bit)) {
		old_modifiers = input->modifiers;

		/* Handles the event condition. */
		if (event->value == 0)
			device->modifier_keys &= (uint8_t)~bit;
		else if (event->value == 1)
			device->modifier_keys |= bit;
		recompute_modifiers(input);

		/* Validates the current input. */
		if (input->modifiers != old_modifiers)
			input->handlers.key(input->handler_context, 0,
			    event->value, event_time(event), input->modifiers);

		/* Returns the computed result. */
		return;
	}

	/* Handles the event condition. */
	if (event->code == KEY_CAPSLOCK) {
		/* Handles the event condition. */
		if (event->value == 0)
			device->caps_down = 0;
		else if (event->value == 1 && !device->caps_down) {
			device->caps_down = 1;
			input->caps_lock ^= 1U;
		}

		/* Returns the computed result. */
		return;
	}

	/* Handles the event condition. */
	if (event->value == 1) {
		keycode = x_keycode(input, event->code);
		device->keycodes[event->code] = keycode;
	} else {
		keycode = device->keycodes[event->code];

		/* Handles the keycode condition. */
		if (keycode == 0)
			keycode = x_keycode(input, event->code);

		/* Handles the event condition. */
		if (event->value == 0)
			device->keycodes[event->code] = 0;
	}

	/* Handles the keycode condition. */
	if (keycode != 0)
		input->handlers.key(input->handler_context, keycode,
		    event->value, event_time(event), input->modifiers);
}

/* Supports the modifier bit operation. */
static int
modifier_bit(
	uint16_t code,
	uint8_t *bit)
{
	/* Dispatch the selected operation case. */
	switch (code) {
	case KEY_LEFTSHIFT:
		*bit = MOD_LEFT_SHIFT; return 1;
	case KEY_RIGHTSHIFT:
		*bit = MOD_RIGHT_SHIFT; return 1;
	case KEY_LEFTCTRL:
		*bit = MOD_LEFT_CONTROL; return 1;
	case KEY_RIGHTCTRL:
		*bit = MOD_RIGHT_CONTROL; return 1;
	case KEY_LEFTALT:
		*bit = MOD_LEFT_ALT; return 1;
	case KEY_RIGHTALT:
		*bit = MOD_RIGHT_ALT; return 1;
	default:
		/* Reports successful completion. */
		return 0;
	}
}

/* Supports the clamp delta operation. */
static int32_t
clamp_delta(
	int64_t value)
{
	/* Validates the current value. */
	if (value < INT32_MIN)
		return INT32_MIN;

	/* Validates the current value. */
	if (value > INT32_MAX)
		return INT32_MAX;

	/* Returns the computed result. */
	return (int32_t)value;
}

/* Supports the release device state operation. */
static void
release_device_state(
	struct xzed_input *input,
	struct xzed_input_device *device)
{
	struct xzed_input_pointer_frame pointer;
	uint16_t old_buttons;
	uint16_t old_modifiers;
	unsigned code;

	old_buttons = input->buttons;
	old_modifiers = input->modifiers;
	device->modifier_keys = 0;
	recompute_modifiers(input);

	/* Validates the current input. */
	if (input->modifiers != old_modifiers)
		input->handlers.key(input->handler_context, 0, 0, 0,
		    input->modifiers);

	/* Process each element required by the operation. */
	for (code = 0; code <= KEY_MAX; code++)

		/* Handles the device condition. */
		if (device->keycodes[code] != 0) {
			input->handlers.key(input->handler_context,
			    device->keycodes[code], 0, 0, input->modifiers);
			device->keycodes[code] = 0;
		}
	memset(&pointer, 0, sizeof(pointer));
	pointer.buttons_before = old_buttons;
	device->buttons = 0;
	pointer.buttons_after = recompute_buttons(input);
	snapshot_button_edges(&pointer, old_buttons, pointer.buttons_after);

	/* Handles the pointer condition. */
	if (pointer.edge_count != 0)
		input->handlers.pointer(input->handler_context, &pointer);
}
