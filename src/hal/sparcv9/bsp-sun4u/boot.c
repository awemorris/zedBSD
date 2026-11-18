/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u boot handoff: the loader's record kept by the HAL, the common
 * record handed to the kernel, and the named values the kernel reads.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "framebuffer.h"

/*
 * The HAL's copy of the loader's handoff.
 *
 * Filled once by sun4u_boot_init() before anything reads it, and not changed
 * afterwards; the loader's own record is not referenced again.
 */
static struct sun4u_boot_handoff boot_handoff;

/*
 * The handoff the kernel's entry reads: the loader's common record, under
 * the common version and size.  Filled once with boot_handoff; what is
 * particular to the board goes by name.
 */
static struct kern_boot_handoff kernel_handoff;

/*
 * The named objects of hal_get_arch_handoff(): the I/O port offsets of the
 * CMD646 IDE controller's primary channel, its command block
 * ("sun4u.cmd646.primary.command") and its control block
 * ("sun4u.cmd646.primary.control"), and of the serial console's 16550
 * ("sun4u.serial.port"), each a 64-bit word.  Filled once with
 * boot_handoff, which the HAL accepts only for a CMD646, and not changed
 * afterwards.
 */
static uint64_t cmd646_primary_command;
static uint64_t cmd646_primary_control;
static uint64_t serial_port;

/*
 * The 8042 keyboard controller's data ("sun4u.i8042.data") and command
 * ("sun4u.i8042.command") port offsets, each a 64-bit word, zero when the
 * loader found no 8042.  Filled once with boot_handoff, and not changed
 * afterwards.
 */
static uint64_t i8042_data;
static uint64_t i8042_command;

static int handoff_name_is(const char *name, const char *wanted);
static void *framebuffer_named(const char *name);

/*
 * Keeps a copy of the loader's handoff and builds the kernel's from it.
 */
void
sun4u_boot_init(
	const struct sun4u_boot_handoff *handoff)
{
	/* Copies the loader's record, which the HAL has already checked. */
	hal_memcpy(&boot_handoff, handoff, sizeof(boot_handoff));

	/*
	 * Gives the kernel the common record alone, under the common version;
	 * the loader's own version tells the HAL only that the sun4u part
	 * follows.
	 */
	kernel_handoff = boot_handoff.common;
	kernel_handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);

	/* Keeps the IDE controller's primary channel for its names. */
	cmd646_primary_command = boot_handoff.ide_primary_command;
	cmd646_primary_control = boot_handoff.ide_primary_control;

	/* Keeps the serial console's port for its name. */
	serial_port = boot_handoff.serial_io_offset;

	/* Keeps the 8042's ports for their names. */
	i8042_data = boot_handoff.i8042_data;
	i8042_command = boot_handoff.i8042_command;
}

/*
 * Returns the HAL's copy of the loader's handoff.
 */
const struct sun4u_boot_handoff *
sun4u_boot_handoff(
	void)
{
	/* Reports the copy sun4u_boot_init() filled. */
	return &boot_handoff;
}

/*
 * Returns the handoff the kernel's entry reads: the common record alone.
 */
const struct kern_boot_handoff *
sun4u_kernel_handoff(
	void)
{
	/* Reports the record sun4u_boot_init() built. */
	return &kernel_handoff;
}

/*
 * Returns one named architecture handoff object.
 *
 * "sun4u.cmd646.primary.command" and "sun4u.cmd646.primary.control" are the
 * I/O port offsets of the CMD646 IDE controller's primary channel, and
 * "sun4u.serial.port" the I/O port offset of the serial console, each a
 * 64-bit word.
 *
 * "sun4u.framebuffer.phys", ".size", ".width", ".height", ".pitch" and
 * ".format" describe the framebuffer the HAL shows, from the loader or the
 * HAL's own setting of the standard VGA, its format a
 * KERN_BOOT_FRAMEBUFFER_* value, and ".pci.vendor" and ".pci.device" the
 * display's PCI IDs; all are NULL when the HAL shows no framebuffer, and
 * the PCI IDs when the display is not on PCI.  "sun4u.i8042.data" and
 * "sun4u.i8042.command" are the I/O port offsets of the 8042 keyboard
 * controller, NULL when there is none.  Each is a 64-bit word.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	void *framebuffer;
	int match;

	/* Exposes the IDE controller's primary command block. */
	match = handoff_name_is(name, "sun4u.cmd646.primary.command");
	if (match)
		return &cmd646_primary_command;

	/* Exposes the IDE controller's primary control block. */
	match = handoff_name_is(name, "sun4u.cmd646.primary.control");
	if (match)
		return &cmd646_primary_control;

	/* Exposes the serial console's port. */
	match = handoff_name_is(name, "sun4u.serial.port");
	if (match)
		return &serial_port;

	/* Exposes the 8042's data port, when the loader found one. */
	match = handoff_name_is(name, "sun4u.i8042.data");
	if (match && i8042_data != 0)
		return &i8042_data;

	/* Exposes the 8042's command port, when the loader found one. */
	match = handoff_name_is(name, "sun4u.i8042.command");
	if (match && i8042_command != 0)
		return &i8042_command;

	/* Exposes the framebuffer the HAL shows. */
	framebuffer = framebuffer_named(name);
	if (framebuffer != NULL)
		return framebuffer;

	/* Reports an unavailable or unknown architecture handoff. */
	return NULL;
}

/*
 * Returns one value of the framebuffer the HAL shows, by name, or NULL
 * when the name is not one of them or the value is unavailable.
 */
static void *
framebuffer_named(
	const char *name)
{
	struct sun4u_framebuffer_names *shown;
	int match;

	/* Nothing is named when the HAL shows no framebuffer. */
	shown = sun4u_framebuffer_shown();
	if (shown == NULL)
		return NULL;

	/* Exposes the physical address of the first pixel. */
	match = handoff_name_is(name, "sun4u.framebuffer.phys");
	if (match)
		return &shown->phys;

	/* Exposes the bytes of the screen. */
	match = handoff_name_is(name, "sun4u.framebuffer.size");
	if (match)
		return &shown->size;

	/* Exposes the width in pixels. */
	match = handoff_name_is(name, "sun4u.framebuffer.width");
	if (match)
		return &shown->width;

	/* Exposes the height in pixels. */
	match = handoff_name_is(name, "sun4u.framebuffer.height");
	if (match)
		return &shown->height;

	/* Exposes the bytes of a row. */
	match = handoff_name_is(name, "sun4u.framebuffer.pitch");
	if (match)
		return &shown->pitch;

	/* Exposes the pixel format. */
	match = handoff_name_is(name, "sun4u.framebuffer.format");
	if (match)
		return &shown->format;

	/* Exposes the display's PCI vendor, when it is on PCI. */
	match = handoff_name_is(name, "sun4u.framebuffer.pci.vendor");
	if (match && shown->vendor != 0)
		return &shown->vendor;

	/* Exposes the display's PCI device, when it is on PCI. */
	match = handoff_name_is(name, "sun4u.framebuffer.pci.device");
	if (match && shown->vendor != 0)
		return &shown->device;

	/* Reports a name that is not the framebuffer's, or an unavailable value. */
	return NULL;
}

/*
 * Tells whether a handoff name is the one wanted.
 */
static int
handoff_name_is(
	const char *name,
	const char *wanted)
{
	size_t index;

	/* Refuses a missing name. */
	if (name == NULL)
		return 0;

	/* Compares the names character by character, terminator included. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (name[index] != wanted[index])
			return 0;
	}

	/* Reports whether the name ends where the wanted one does. */
	if (name[index] != '\0')
		return 0;
	return 1;
}
