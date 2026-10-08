/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of a /dev/hid-host setup (ws143-p005), apart from the node so
 * that the host test can run it: pure, no lock, no allocation.
 */

#include <drivers/generic/hid-host.h>
#include <kern/kcrt.h>
#include <uapi/hid-host.h>
#include <uapi/input.h>

static int hid_host_text_ended(const char *text);

/*
 * Reports whether a setup is well formed: its magic and version, a bus a
 * program may claim (Bluetooth or virtual, never USB), a descriptor size
 * in 1..HID_HOST_DESCRIPTOR_MAX, zero reserved words, and texts that end
 * within their fields.  The descriptor itself is the parser's to judge.
 */
int
drv_hid_host_setup_valid(
	const struct hid_host_setup *setup)
{
	size_t index;
	int ended;

	/* The form. */
	if (setup->magic != HID_HOST_MAGIC)
		return 0;
	if (setup->version != HID_HOST_VERSION)
		return 0;

	/* A bus a program may claim. */
	if (setup->bus != BUS_BLUETOOTH && setup->bus != BUS_VIRTUAL)
		return 0;

	/* A descriptor that fits its field. */
	if (setup->descriptor_size == 0U)
		return 0;
	if (setup->descriptor_size > HID_HOST_DESCRIPTOR_MAX)
		return 0;

	/* The reserved words, zero. */
	for (index = 0; index < sizeof(setup->reserved) / sizeof(setup->reserved[0]); index++) {
		/* A word set is a later version's field. */
		if (setup->reserved[index] != 0U)
			return 0;
	}

	/* Each text ends within its field. */
	ended = hid_host_text_ended(setup->name);
	if (!ended)
		return 0;
	ended = hid_host_text_ended(setup->physical_path);
	if (!ended)
		return 0;
	ended = hid_host_text_ended(setup->unique_id);
	if (!ended)
		return 0;

	/* Succeeded: the setup is well formed. */
	return 1;
}

/* Reports whether a setup's text ends with a NUL within its field. */
static int
hid_host_text_ended(
	const char *text)
{
	size_t length;

	/* The text's length within the field. */
	length = kern_strnlen(text, HID_HOST_TEXT_MAX);
	if (length >= HID_HOST_TEXT_MAX)
		return 0;

	/* Succeeded: the text ends in its field. */
	return 1;
}
