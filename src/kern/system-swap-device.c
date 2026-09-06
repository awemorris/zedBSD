/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The swap control ioctls of the system device.
 *
 * Every request is copied in, validated against its versioned layout, and
 * handed to the swap control layer; the source query maps the kernel
 * source state onto the public state vocabulary before copying out.
 */

#include <kern/system-swap-device.h>

#include <kern/swap-control.h>
#include <kern/swap.h>
#include <kern/uaccess.h>

#include <zedbsd/system.h>

#include <errno.h>
#include <string.h>

_Static_assert(sizeof(((struct kern_swap_control_source_info *)0)->uuid) ==
    ZEDBSD_SYSTEM_SWAP_UUID_SIZE, "kernel and UAPI swap UUID sizes differ");
_Static_assert(sizeof(((struct kern_swap_control_source_info *)0)->label) ==
    ZEDBSD_SYSTEM_SWAP_LABEL_SIZE,
    "kernel and UAPI swap label sizes differ");
_Static_assert(sizeof(((struct kern_swap_control_source_info *)0)->source) ==
    ZEDBSD_SYSTEM_SWAP_SOURCE_MAX,
    "kernel and UAPI swap source-string sizes differ");

static int words_are_zero(const uint32_t *words, size_t count);
static int bounded_string_valid(const char *text, size_t capacity);
static int control_valid(const struct system_swap_control *control);
static int query_valid(const struct system_swap_source_info *query);
static int map_source_state(uint32_t state, uint32_t *mapped);
static int control_ioctl(unsigned long request, uintptr_t argument, int superuser);
static int get_source_ioctl(uintptr_t argument);

/*
 * Dispatches one swap ioctl of the system device.
 */
int
system_swap_device_ioctl(
	unsigned long request,
	uintptr_t argument,
	int superuser)
{
	int error;

	/* Routes the request to its handler. */
	switch (request) {
	case ZEDBSD_SYSTEM_SWAP_ADD:
	case ZEDBSD_SYSTEM_SWAP_REMOVE:
		error = control_ioctl(request, argument, superuser);
		break;
	case ZEDBSD_SYSTEM_GET_SWAP_SOURCE:
		error = get_source_ioctl(argument);
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	/* Reports the handler result. */
	return error;
}

/* Tests whether every word of a reserved area is zero. */
static int
words_are_zero(
	const uint32_t *words,
	size_t count)
{
	/* Rejects the first nonzero word. */
	while (count != 0) {
		if (*words != 0)
			return 0;
		words++;
		count--;
	}

	/* Reports an all-zero area. */
	return 1;
}

/* Tests whether a fixed buffer holds a nonempty terminated string. */
static int
bounded_string_valid(
	const char *text,
	size_t capacity)
{
	/* An empty buffer or an empty string is invalid. */
	if (capacity == 0)
		return 0;
	if (text[0] == '\0')
		return 0;

	/* The terminator must lie inside the buffer. */
	if (memchr(text, '\0', capacity) == NULL)
		return 0;

	/* Reports a valid string. */
	return 1;
}

/* Validates the layout and contents of a swap control request. */
static int
control_valid(
	const struct system_swap_control *control)
{
	/* The versioned layout must match exactly. */
	if (control->version != ZEDBSD_SYSTEM_SWAP_VERSION)
		return 0;
	if (control->struct_size != sizeof(*control))
		return 0;

	/* No flag or reserved field may be set. */
	if (control->flags != 0)
		return 0;
	if (control->reserved0 != 0)
		return 0;
	if (!words_are_zero(control->reserved,
	    sizeof(control->reserved) / sizeof(control->reserved[0])))
		return 0;

	/* The source must be a terminated string. */
	if (!bounded_string_valid(control->source, sizeof(control->source)))
		return 0;

	/* Reports a valid request. */
	return 1;
}

/* Validates the layout and contents of a swap source query. */
static int
query_valid(
	const struct system_swap_source_info *query)
{
	/* The versioned layout must match exactly. */
	if (query->version != ZEDBSD_SYSTEM_SWAP_VERSION)
		return 0;
	if (query->struct_size != sizeof(*query))
		return 0;

	/* No flag or reserved field may be set. */
	if (query->flags != 0)
		return 0;
	if (!words_are_zero(query->reserved,
	    sizeof(query->reserved) / sizeof(query->reserved[0])))
		return 0;

	/* The source identifier must exist. */
	if (query->source_id >= ZEDBSD_SYSTEM_SWAP_SOURCE_COUNT)
		return 0;

	/* Reports a valid query. */
	return 1;
}

/* Maps a kernel swap source state onto the public state vocabulary. */
static int
map_source_state(
	uint32_t state,
	uint32_t *mapped)
{
	/* Rejects a missing result. */
	if (mapped == NULL)
		return EINVAL;

	/* Folds the kernel states into the three public ones. */
	switch (state) {
	case SWAP_SOURCE_STATE_INACTIVE:
	case SWAP_SOURCE_STATE_PREPARED:
		*mapped = ZEDBSD_SYSTEM_SWAP_STATE_INACTIVE;
		return 0;
	case SWAP_SOURCE_STATE_ACTIVE:
		*mapped = ZEDBSD_SYSTEM_SWAP_STATE_ACTIVE;
		return 0;
	case SWAP_SOURCE_STATE_DRAINING:
	case SWAP_SOURCE_STATE_REMOVING:
		*mapped = ZEDBSD_SYSTEM_SWAP_STATE_DRAINING;
		return 0;
	default:
		break;
	}

	/* Reports a kernel state without a public counterpart. */
	return EIO;
}

/* Handles a swap add or remove request. */
static int
control_ioctl(
	unsigned long request,
	uintptr_t argument,
	int superuser)
{
	struct system_swap_control control;
	int error;

	/* Copies the request in and validates it. */
	error = copyin(argument, &control, sizeof(control));
	if (error != 0)
		return error;
	if (!control_valid(&control))
		return EINVAL;

	/* Rejects an unprivileged caller. */
	if (!superuser)
		return EPERM;

	/* Performs the requested change. */
	if (request == ZEDBSD_SYSTEM_SWAP_ADD)
		error = kern_swap_control_add(control.source);
	else
		error = kern_swap_control_remove(control.source);

	/* Reports the change result. */
	return error;
}

/* Handles a swap source query. */
static int
get_source_ioctl(
	uintptr_t argument)
{
	struct system_swap_source_info output;
	struct kern_swap_control_source_info snapshot;
	uint32_t source_id;
	int error;

	/* Copies the query in and validates it. */
	error = copyin(argument, &output, sizeof(output));
	if (error != 0)
		return error;
	if (!query_valid(&output))
		return EINVAL;
	source_id = output.source_id;

	/* Snapshots the source. */
	memset(&snapshot, 0, sizeof(snapshot));
	error = kern_swap_control_get(source_id, &snapshot);
	if (error != 0)
		return error;

	/* Rejects a snapshot that contradicts itself. */
	if (snapshot.source_id != source_id ||
	    snapshot.used_pages > snapshot.total_pages)
		return EIO;

	/* Renders the snapshot in the public layout. */
	memset(&output, 0, sizeof(output));
	output.version = ZEDBSD_SYSTEM_SWAP_VERSION;
	output.struct_size = sizeof(output);
	output.source_id = source_id;
	error = map_source_state(snapshot.state, &output.state);
	if (error != 0)
		return error;
	output.header_version = snapshot.header_version;
	output.total_pages = snapshot.total_pages;
	output.used_pages = snapshot.used_pages;
	memcpy(output.uuid, snapshot.uuid, sizeof(output.uuid));
	memcpy(output.label, snapshot.label, sizeof(output.label));
	output.label[sizeof(output.label) - 1U] = '\0';
	memcpy(output.source, snapshot.source, sizeof(output.source));
	output.source[sizeof(output.source) - 1U] = '\0';

	/* Copies the answer out. */
	error = copyout(&output, argument, sizeof(output));

	/* Reports the copy result. */
	return error;
}
