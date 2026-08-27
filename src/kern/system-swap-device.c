/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
words_are_zero(const uint32_t *words, size_t count)
{
	while (count != 0) {
		if (*words != 0)
			return 0;
		words++;
		count--;
	}
	return 1;
}

static int
bounded_string_valid(const char *text, size_t capacity)
{
	return capacity != 0 && text[0] != '\0' &&
	    memchr(text, '\0', capacity) != NULL;
}

static int
control_valid(const struct system_swap_control *control)
{
	return control->version == ZEDBSD_SYSTEM_SWAP_VERSION &&
	    control->struct_size == sizeof(*control) && control->flags == 0 &&
	    control->reserved0 == 0 &&
	    words_are_zero(control->reserved,
		 sizeof(control->reserved) / sizeof(control->reserved[0])) &&
	    bounded_string_valid(control->source, sizeof(control->source));
}

static int
query_valid(const struct system_swap_source_info *query)
{
	return query->version == ZEDBSD_SYSTEM_SWAP_VERSION &&
	    query->struct_size == sizeof(*query) && query->flags == 0 &&
	    query->source_id < ZEDBSD_SYSTEM_SWAP_SOURCE_COUNT &&
	    words_are_zero(query->reserved,
		 sizeof(query->reserved) / sizeof(query->reserved[0]));
}

static int
map_source_state(uint32_t state, uint32_t *mapped)
{
	if (mapped == NULL)
		return EINVAL;
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
		return EIO;
	}
}

static int
control_ioctl(unsigned long request, uintptr_t argument, int superuser)
{
	struct system_swap_control control;
	int error;

	error = copyin(argument, &control, sizeof(control));
	if (error != 0)
		return error;
	if (!control_valid(&control))
		return EINVAL;
	if (!superuser)
		return EPERM;
	if (request == ZEDBSD_SYSTEM_SWAP_ADD)
		return kern_swap_control_add(control.source);
	return kern_swap_control_remove(control.source);
}

static int
get_source_ioctl(uintptr_t argument)
{
	struct system_swap_source_info output;
	struct kern_swap_control_source_info snapshot;
	uint32_t source_id;
	int error;

	error = copyin(argument, &output, sizeof(output));
	if (error != 0)
		return error;
	if (!query_valid(&output))
		return EINVAL;
	source_id = output.source_id;
	memset(&snapshot, 0, sizeof(snapshot));
	error = kern_swap_control_get(source_id, &snapshot);
	if (error != 0)
		return error;
	if (snapshot.source_id != source_id ||
	    snapshot.used_pages > snapshot.total_pages)
		return EIO;
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
	return copyout(&output, argument, sizeof(output));
}

int
system_swap_device_ioctl(unsigned long request, uintptr_t argument,
			 int superuser)
{
	switch (request) {
	case ZEDBSD_SYSTEM_SWAP_ADD:
	case ZEDBSD_SYSTEM_SWAP_REMOVE:
		return control_ioctl(request, argument, superuser);
	case ZEDBSD_SYSTEM_GET_SWAP_SOURCE:
		return get_source_ioctl(argument);
	default:
		return EOPNOTSUPP;
	}
}
