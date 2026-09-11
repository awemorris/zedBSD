/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 keyboard driver.
 *
 * The keyboard is an 8251 USART at ports 0x41 and 0x43, and it is its
 * own device: unlike the PC/AT, the mouse is a separate chip on its own
 * interrupt, so the two have nothing to share and this file registers
 * exactly one evdev device.
 *
 * A scan code arrives as one byte. Bit 7 marks a release, so there is no
 * prefix state to carry between interrupts.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "kern/device-io.h"
#include "kern/input-device.h"
#include "kern/input-keymap.h"
#include "kern/irq.h"
#include "kern/lock.h"
#include "pc98-keyboard.h"

/* The 8251 data and status ports, and the receiver-ready status bit. */
#define KEYBOARD_DATA		0x0041U
#define KEYBOARD_STATUS		0x0043U
#define KEYBOARD_RXRDY		0x02U

/* Bit 7 of a scan code marks the release of that position. */
#define KEYBOARD_RELEASE	0x80U

#define KEYBOARD_IRQ		1

/*
 * The PC-98 keyboard positions.
 *
 * The layout is JIS, so the names are the positions rather than the
 * legends: 0x1a carries @ and 0x0d carries the yen sign, which the
 * keymap turns into characters later.
 */
static const char *const scan_symbols[128] = {
	[0x00] = "esc",
	[0x01] = "1",
	[0x02] = "2",
	[0x03] = "3",
	[0x04] = "4",
	[0x05] = "5",
	[0x06] = "6",
	[0x07] = "7",
	[0x08] = "8",
	[0x09] = "9",
	[0x0a] = "0",
	[0x0b] = "minus",
	[0x0c] = "equal",
	[0x0d] = "backslash",
	[0x0e] = "backspace",
	[0x0f] = "tab",
	[0x10] = "q",
	[0x11] = "w",
	[0x12] = "e",
	[0x13] = "r",
	[0x14] = "t",
	[0x15] = "y",
	[0x16] = "u",
	[0x17] = "i",
	[0x18] = "o",
	[0x19] = "p",
	[0x1a] = "leftbrace",
	[0x1b] = "rightbrace",
	[0x1c] = "enter",
	[0x1d] = "a",
	[0x1e] = "s",
	[0x1f] = "d",
	[0x20] = "f",
	[0x21] = "g",
	[0x22] = "h",
	[0x23] = "j",
	[0x24] = "k",
	[0x25] = "l",
	[0x26] = "semicolon",
	[0x27] = "apostrophe",
	[0x28] = "grave",
	[0x29] = "z",
	[0x2a] = "x",
	[0x2b] = "c",
	[0x2c] = "v",
	[0x2d] = "b",
	[0x2e] = "n",
	[0x2f] = "m",
	[0x30] = "comma",
	[0x31] = "dot",
	[0x32] = "slash",
	[0x33] = "ro",
	[0x34] = "space",
	[0x35] = "henkan",
	[0x36] = "pageup",
	[0x37] = "pagedown",
	[0x38] = "insert",
	[0x39] = "delete",
	[0x3a] = "up",
	[0x3b] = "left",
	[0x3c] = "right",
	[0x3d] = "down",
	[0x3e] = "home",
	[0x3f] = "help",
	[0x40] = "kpminus",
	[0x41] = "kpslash",
	[0x42] = "kp7",
	[0x43] = "kp8",
	[0x44] = "kp9",
	[0x45] = "kpasterisk",
	[0x46] = "kp4",
	[0x47] = "kp5",
	[0x48] = "kp6",
	[0x49] = "kpplus",
	[0x4a] = "kp1",
	[0x4b] = "kp2",
	[0x4c] = "kp3",
	[0x4d] = "kpequal",
	[0x4e] = "kp0",
	[0x4f] = "kpcomma",
	[0x50] = "kpdot",
	[0x60] = "stop",
	[0x61] = "copy",
	[0x62] = "f1",
	[0x63] = "f2",
	[0x64] = "f3",
	[0x65] = "f4",
	[0x66] = "f5",
	[0x67] = "f6",
	[0x68] = "f7",
	[0x69] = "f8",
	[0x6a] = "f9",
	[0x6b] = "f10",
	[0x70] = "leftshift",
	[0x71] = "capslock",
	[0x72] = "katakana",
	[0x73] = "leftalt",
	[0x74] = "leftctrl",
	[0x7d] = "rightshift"
};

static struct spinlock keyboard_lock;
static struct input_device *keyboard_input;
static struct input_capability keyboard_capabilities[128];
static unsigned keyboard_capability_count;

static void keyboard_build_capabilities(void);
static void keyboard_interrupt(int interrupt, kern_irq_ack_t acknowledge,
			       void *argument);

/*
 * Publishes the PC-98 keyboard as one evdev device.
 */
int
drv_pc98_keyboard_init(
	void)
{
	struct input_device_info keyboard_info = {
		.name = "NEC PC-98 keyboard",
		.physical_path = "pc98/i8251/kbd0",
		.id = {.bustype = BUS_HOST, .product = 1, .version = 1},
		.capabilities = keyboard_capabilities,
		.capability_count = 0,
	};
	int error;

	/* Starts every lock and counter out empty. */
	spin_init(&keyboard_lock, LOCK_RANK_DEVICE, "pc98 keyboard");
	keyboard_input = NULL;
	keyboard_build_capabilities();
	keyboard_info.capability_count = keyboard_capability_count;

	/* Installs the handler while the line is still masked. */
	kern_irq_mask(KEYBOARD_IRQ);
	if (kern_irq_register(KEYBOARD_IRQ, keyboard_interrupt, NULL) != 0)
		return EBUSY;

	/* Publishes the device before letting the line run. */
	error = drv_input_device_register(&keyboard_info, &keyboard_input);
	if (error != 0) {
		(void)kern_irq_unregister(KEYBOARD_IRQ, keyboard_interrupt,
					  NULL);

		/* Reports the failure. */
		return error;
	}

	/* Drains a byte the firmware may have left behind. */
	if ((kern_io_in8(KEYBOARD_STATUS) & KEYBOARD_RXRDY) != 0)
		(void)kern_io_in8(KEYBOARD_DATA);

	/* Lets the keyboard raise interrupts. */
	kern_irq_unmask(KEYBOARD_IRQ);

	/* Succeeded. */
	return 0;
}

/* Lists every distinct key the scan table can produce. */
static void
keyboard_build_capabilities(
	void)
{
	unsigned scan;
	unsigned index;
	uint16_t code;
	int duplicate;

	/* Every input device reports the synchronisation event. */
	keyboard_capability_count = 0;
	keyboard_capabilities[keyboard_capability_count].type = EV_SYN;
	keyboard_capabilities[keyboard_capability_count].code = SYN_REPORT;
	keyboard_capability_count++;

	/* Adds each distinct key the scan table can produce. */
	for (scan = 0; scan < 128U; scan++) {
		/* Skips a position this board does not have. */
		if (scan_symbols[scan] == NULL)
			continue;
		code = drv_input_key_from_symbol(scan_symbols[scan]);

		/* Skips a name this build does not know. */
		if (code == KEY_RESERVED)
			continue;

		/* Skips a key an earlier position already published. */
		duplicate = 0;
		for (index = 0; index < keyboard_capability_count; index++) {
			if (keyboard_capabilities[index].type == EV_KEY &&
			    keyboard_capabilities[index].code == code) {
				duplicate = 1;
				break;
			}
		}
		if (duplicate)
			continue;

		/* Stops at the fixed capability bound. */
		if (keyboard_capability_count >=
		    sizeof(keyboard_capabilities) /
		    sizeof(keyboard_capabilities[0]))
			break;
		keyboard_capabilities[keyboard_capability_count].type = EV_KEY;
		keyboard_capabilities[keyboard_capability_count].code = code;
		keyboard_capability_count++;
	}
}

/*
 * Services one keyboard interrupt.
 *
 * The byte is read and acknowledged inside the handler so the 8251 can
 * raise the next edge immediately; the event is published afterwards.
 */
static void
keyboard_interrupt(
	int interrupt,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	const char *symbol;
	unsigned long irq;
	uint8_t value;
	uint16_t code;
	int pressed;
	int publish;

	(void)interrupt;
	(void)argument;
	code = KEY_RESERVED;
	pressed = 0;
	publish = 0;
	irq = spin_lock_irqsave(&keyboard_lock);

	/* Takes the byte only when the receiver actually holds one. */
	if ((kern_io_in8(KEYBOARD_STATUS) & KEYBOARD_RXRDY) != 0) {
		value = kern_io_in8(KEYBOARD_DATA);
		symbol = scan_symbols[value & (uint8_t)~KEYBOARD_RELEASE];
		pressed = (value & KEYBOARD_RELEASE) == 0;

		/* Publishes only a position this build understands. */
		if (symbol != NULL) {
			code = drv_input_key_from_symbol(symbol);
			publish = code != KEY_RESERVED;
		}
	}

	/* Releases the line before the evdev publication. */
	kern_irq_send_eoi(acknowledge);

	/* Publishes the decoded transition. */
	if (publish && keyboard_input != NULL) {
		drv_input_device_emit(keyboard_input, EV_KEY, code, pressed);
		drv_input_device_emit(keyboard_input, EV_SYN, SYN_REPORT, 0);
	}
	spin_unlock_irqrestore(&keyboard_lock, irq);
}
