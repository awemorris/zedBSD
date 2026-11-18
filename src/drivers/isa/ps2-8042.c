/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i8042 PS/2 keyboard and mouse controller, wherever its two I/O ports
 * are (WS275: moved from the PC/AT platform so sun4u shares it).
 *
 * The board names the data and status/command ports and either the two
 * interrupt lines or none.  Without interrupts a kernel thread looks at the
 * controller every few milliseconds, and the driver writes the
 * configuration byte itself: scan-code translation on, both interrupt
 * bits off, so the set 1 table below holds on a controller that was not
 * set up by a BIOS.
 *
 * The mouse is asked for the IntelliMouse protocols when it is started
 * (BUG-156): the sample-rate knock 200, 100, 80 turns a wheel mouse (and
 * the PS/2 emulation of a laptop's touchpad, whose two-finger or edge
 * scroll comes as the wheel) into ID 3 with a fourth byte for the wheel,
 * and 200, 200, 80 then turns an IntelliMouse Explorer into ID 4 with the
 * wheel, the horizontal wheel and the side and extra buttons.  A mouse
 * that answers neither stays ID 0 with three-byte packets.
 */

#include "drivers/isa/ps2-8042.h"
#include "kern/input-device.h"
#include "kern/input-keymap.h"
#include "kern/klog.h"
#include "kern/lock.h"

#include <uapi/errno.h>
#include <limits.h>
#include <stdint.h>
#include "kern/irq.h"

#ifndef WS018_INPUT_HID_HOST_TEST
#include "kern/clock.h"
#include "kern/device-io.h"
#include "kern/sched.h"
#include "kern/thread.h"
#endif

#define I8042_STATUS_OUTPUT 0x01U
#define I8042_STATUS_INPUT 0x02U
#define I8042_STATUS_AUX 0x20U

#define I8042_READ_CONFIG 0x20U
#define I8042_WRITE_CONFIG 0x60U
#define I8042_ENABLE_AUX 0xa8U
#define I8042_DISABLE_AUX 0xa7U
#define I8042_WRITE_AUX 0xd4U
#define I8042_CONFIG_AUX_IRQ 0x02U
#define I8042_CONFIG_AUX_OFF 0x20U
#define I8042_CONFIG_KEYBOARD_OFF 0x10U
#define I8042_CONFIG_TRANSLATE 0x40U
#define I8042_ENABLE_KEYBOARD 0xaeU

#define PS2_SET_DEFAULTS 0xf6U
#define PS2_SET_SAMPLE_RATE 0xf3U
#define PS2_GET_DEVICE_ID 0xf2U
#define PS2_DISABLE_STREAM 0xf5U
#define PS2_ENABLE_STREAM 0xf4U
#define PS2_ACK 0xfaU
#define PS2_RESEND 0xfeU
#define I8042_CONFIG_KEYBOARD_IRQ 0x01U
#define I8042_SCAN_RELEASE 0x80U
#define I8042_SCAN_EXTENDED 0xe0U
#define PS2_WAIT_LOOPS 100000U

#define MOUSE_BUTTON_LEFT 0x01U
#define MOUSE_BUTTON_MIDDLE 0x02U
#define MOUSE_BUTTON_RIGHT 0x04U
#define MOUSE_BUTTON_SIDE 0x08U
#define MOUSE_BUTTON_EXTRA 0x10U

/* The device IDs: a plain mouse, the IntelliMouse (wheel) and the IntelliMouse Explorer (wheels and five buttons). */
#define PS2_ID_STANDARD 0x00U
#define PS2_ID_WHEEL 0x03U
#define PS2_ID_EXPLORER 0x04U

/* The sample rate a mouse runs at once it is identified (the rate SET_DEFAULTS gives). */
#define PS2_SAMPLE_RATE_DEFAULT 100U

/* How often a controller without interrupts is looked at. */
#define I8042_POLL_MS 10U

/* The most bytes one look takes from the controller. */
#define I8042_POLL_DRAIN 32U

/*
 * The controller's ports and interrupt lines, set once by
 * drv_isa_ps2_8042_init() before anything touches the controller.
 */
static uint16_t i8042_data_port;
static uint16_t i8042_status_port;
static int i8042_keyboard_irq;
static int i8042_mouse_irq;
static int i8042_polled;

static struct spinlock controller_lock;
static struct mutex lifecycle_lock;
static struct input_device *mouse_input;
static struct input_device *keyboard_input;
static struct input_capability keyboard_capabilities[256];
static size_t keyboard_capability_count;
static int keyboard_extended;

/*
 * The keys held down, one bit per evdev code, kept by the interrupt under
 * controller_lock.  The keyboard's typematic sends a held key's make code
 * again and again; each one after the first is published as a repeat
 * (value 2, as evdev does), not as a new press, so the compositor and the
 * applications repeat at the user's rate (BUG-191) and Caps Lock does not
 * toggle while it is held.
 */
static uint8_t keyboard_down[(KEY_MAX + 1U + 7U) / 8U];
static uint8_t packet[4];
static unsigned packet_index;

/*
 * The protocol the mouse speaks since it was last started (PS2_ID_*), and
 * the bytes of its packets: three for a plain mouse, four with a wheel.
 * Both are set while the interrupt is held off and read by it under
 * controller_lock.
 */
static uint8_t mouse_protocol;
static unsigned packet_size;
static uint32_t last_buttons;
static unsigned reader_count;
static int mouse_active;

/* Scan-code set 1 positions as stable key symbols. */
static const char *const scan_symbols[128] = {
	[0x01] = "esc",
	[0x02] = "1",
	[0x03] = "2",
	[0x04] = "3",
	[0x05] = "4",
	[0x06] = "5",
	[0x07] = "6",
	[0x08] = "7",
	[0x09] = "8",
	[0x0a] = "9",
	[0x0b] = "0",
	[0x0c] = "minus",
	[0x0d] = "equal",
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
	[0x1d] = "leftctrl",
	[0x1e] = "a",
	[0x1f] = "s",
	[0x20] = "d",
	[0x21] = "f",
	[0x22] = "g",
	[0x23] = "h",
	[0x24] = "j",
	[0x25] = "k",
	[0x26] = "l",
	[0x27] = "semicolon",
	[0x28] = "apostrophe",
	[0x29] = "grave",
	[0x2a] = "leftshift",
	[0x2b] = "backslash",
	[0x2c] = "z",
	[0x2d] = "x",
	[0x2e] = "c",
	[0x2f] = "v",
	[0x30] = "b",
	[0x31] = "n",
	[0x32] = "m",
	[0x33] = "comma",
	[0x34] = "dot",
	[0x35] = "slash",
	[0x36] = "rightshift",
	[0x38] = "leftalt",
	[0x39] = "space",
	[0x3a] = "capslock",
	[0x3b] = "f1",
	[0x3c] = "f2",
	[0x3d] = "f3",
	[0x3e] = "f4",
	[0x3f] = "f5",
	[0x40] = "f6",
	[0x41] = "f7",
	[0x42] = "f8",
	[0x43] = "f9",
	[0x44] = "f10",
};

/*
 * What the mouse can report.  The wheels and the side and extra buttons
 * are declared before the mouse is identified (the device is registered
 * at boot and started only when it is opened); a plain mouse simply never
 * sends them.
 */
static const struct input_capability mouse_capabilities[] = {
	{EV_SYN, SYN_REPORT}, {EV_REL, REL_X},	   {EV_REL, REL_Y},
	{EV_REL, REL_WHEEL},  {EV_REL, REL_HWHEEL},
	{EV_KEY, BTN_LEFT},   {EV_KEY, BTN_RIGHT}, {EV_KEY, BTN_MIDDLE},
	{EV_KEY, BTN_SIDE},   {EV_KEY, BTN_EXTRA},
};

/*
 * The host test's 8042 (WS018, BUG-156): with WS018_INPUT_HID_HOST_TEST the
 * ports are a model of the controller and the mouse instead of the I/O
 * instructions.
 */
#ifdef WS018_INPUT_HID_HOST_TEST
uint8_t ws018_input_hid_test_inb(uint16_t);
void ws018_input_hid_test_outb(uint16_t, uint8_t);
#endif

static uint8_t inb(uint16_t port);

static void outb(uint16_t port, uint8_t value);
static int wait_input_empty(void);
static int write_command(uint8_t command);
static int write_data(uint8_t value);
static int read_output(uint8_t *value, int auxiliary);
static void flush_output(void);
static int read_config(uint8_t *configuration);
static int write_config(uint8_t configuration);
static int mouse_command(uint8_t command);
static int mouse_command_value(uint8_t command, uint8_t value);
static int mouse_read_id(uint8_t *id);
static int mouse_knock(uint8_t first, uint8_t second, uint8_t third, uint8_t *id);
static int mouse_identify(void);
static int mouse_identify_current(void);
static int consume_byte(uint8_t value, int32_t *dx, int32_t *dy, int32_t *wheel, int32_t *hwheel, uint32_t *buttons);
static void publish_sample(int32_t dx, int32_t dy, int32_t wheel, int32_t hwheel, uint32_t buttons, uint32_t changed_buttons);
static void mouse_interrupt(int interrupt, kern_irq_ack_t acknowledge, void *argument);
static int mouse_probe(void);
static int mouse_start(void);
static void mouse_stop(void);
static int mouse_input_open(void *context);
static const char *scan_symbol(uint8_t scan, int extended);
static void keyboard_interrupt(int interrupt, kern_irq_ack_t acknowledge,
			       void *argument);
static void keyboard_build_capabilities(void);
static int polled_configure(void);
static int poll_start(void);
#ifndef WS018_INPUT_HID_HOST_TEST
static void poll_worker(void *argument);
#endif
static void mouse_input_close(void *context);

#ifdef WS018_INPUT_HID_HOST_TEST
/* Supports the inb operation. */
static uint8_t
inb(
	uint16_t port)
{
	uint8_t function_result;

	/* Obtains the ws018 input hid test inb result. */
	function_result = ws018_input_hid_test_inb(port);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the outb operation. */
static void
outb(
	uint16_t port,
	uint8_t value)
{
	ws018_input_hid_test_outb(port, value);
}
#else
/* Reads one port of the controller. */
static uint8_t
inb(
	uint16_t port)
{
	uint8_t value;

	/* Reads through the kernel's port access. */
	value = kern_io_in8(port);

	/* Returns the byte read. */
	return value;
}

/* Writes one port of the controller. */
static void
outb(
	uint16_t port,
	uint8_t value)
{
	/* Writes through the kernel's port access. */
	kern_io_out8(port, value);
}
#endif

/* Supports the wait input empty operation. */
static int
wait_input_empty(
	void)
{
	unsigned spin;

	/* Process each element required by the operation. */
	for (spin = 0; spin < PS2_WAIT_LOOPS; spin++) {
		/* Checks the inb result. */
		if ((inb(i8042_status_port) & I8042_STATUS_INPUT) == 0)
			return 0;
	}

	/* Failed. */
	return ETIMEDOUT;
}

/* Supports the write command operation. */
static int
write_command(
	uint8_t command)
{
	int error = wait_input_empty();

	/* Checks the operation status. */
	if (error != 0)
		return error;
	outb(i8042_status_port, command);

	/* Succeeded. */
	return 0;
}

/* Supports the write data operation. */
static int
write_data(
	uint8_t value)
{
	int error = wait_input_empty();

	/* Checks the operation status. */
	if (error != 0)
		return error;
	outb(i8042_data_port, value);

	/* Succeeded. */
	return 0;
}

/* Supports the read output operation. */
static int
read_output(
	uint8_t *value,
	int auxiliary)
{
	uint8_t data;
	uint8_t status;
	unsigned spin;

	/* Process each element required by the operation. */
	for (spin = 0; spin < PS2_WAIT_LOOPS; spin++) {
		/* Checks the operation status. */
		status = inb(i8042_status_port);
		if ((status & I8042_STATUS_OUTPUT) != 0) {
			/* Checks the operation status. */
			data = inb(i8042_data_port);
			if (((status & I8042_STATUS_AUX) != 0) == auxiliary) {
				*value = data;
				/* Succeeded. */
				return 0;
			}

			/*
			 * A byte from the other 8042 port cannot be left in
			 * front of the response being polled.  Controller
			 * transactions are short, so at most a concurrently
			 * typed scan code is lost.
			 */
		}

	}

	/* Failed. */
	return ETIMEDOUT;
}

/* Supports the flush output operation. */
static void
flush_output(
	void)
{
	unsigned count;

	/* Process each remaining element. */
	for (count = 0; count < 64U; count++) {
		/* Checks the inb result. */
		if ((inb(i8042_status_port) & I8042_STATUS_OUTPUT) == 0)
			break;
		(void)inb(i8042_data_port);
	}
}

/* Supports the read config operation. */
static int
read_config(
	uint8_t *configuration)
{
	int function_result;
	int error = write_command(I8042_READ_CONFIG);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Obtains the read output result. */
	function_result = read_output(configuration, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the write config operation. */
static int
write_config(
	uint8_t configuration)
{
	int function_result;
	int error = write_command(I8042_WRITE_CONFIG);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Obtains the write data result. */
	function_result = write_data(configuration);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the mouse command operation. */
static int
mouse_command(
	uint8_t command)
{
	uint8_t response;
	int error;
	unsigned attempt;

	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 3U; attempt++) {
		/* Checks the operation status. */
		error = write_command(I8042_WRITE_AUX);
		if (error == 0)
			error = write_data(command);
		if (error == 0)
			error = read_output(&response, 1);
		if (error != 0)
			return error;

		/* Handles the response condition. */
		if (response == PS2_ACK)
			return 0;

		/* Handles the response condition. */
		if (response != PS2_RESEND)
			return EIO;
	}

	/* Failed. */
	return EIO;
}

/*
 * Sends a command with its one-byte argument (such as a sample rate); both
 * bytes must be acknowledged.  Returns 0 or an errno value.
 */
static int
mouse_command_value(
	uint8_t command,
	uint8_t value)
{
	int error;

	/* The command. */
	error = mouse_command(command);
	if (error != 0)
		return error;

	/* Its argument, acknowledged like a command. */
	error = mouse_command(value);
	if (error != 0)
		return error;

	/* Succeeded: the mouse took both bytes. */
	return 0;
}

/* Asks the mouse for its device ID (PS2_ID_*); returns 0 or an errno value. */
static int
mouse_read_id(
	uint8_t *id)
{
	int error;

	/* The request, acknowledged. */
	error = mouse_command(PS2_GET_DEVICE_ID);
	if (error != 0)
		return error;

	/* The ID follows the acknowledgement. */
	error = read_output(id, 1);
	if (error != 0)
		return error;

	/* Succeeded: *id is the protocol the mouse speaks now. */
	return 0;
}

/*
 * Sets three sample rates in a row, the knock a mouse answers by changing
 * its protocol, and reads the ID it has after them.  Returns 0 or an errno
 * value.
 */
static int
mouse_knock(
	uint8_t first,
	uint8_t second,
	uint8_t third,
	uint8_t *id)
{
	int error;

	/* The three rates, in order. */
	error = mouse_command_value(PS2_SET_SAMPLE_RATE, first);
	if (error != 0)
		return error;
	error = mouse_command_value(PS2_SET_SAMPLE_RATE, second);
	if (error != 0)
		return error;
	error = mouse_command_value(PS2_SET_SAMPLE_RATE, third);
	if (error != 0)
		return error;

	/* The ID the knock left. */
	error = mouse_read_id(id);
	if (error != 0)
		return error;

	/* Succeeded: *id says whether the knock was taken. */
	return 0;
}

/*
 * Finds the richest protocol the mouse speaks (BUG-156): the IntelliMouse
 * knock, then the Explorer's, each kept only when the ID says the mouse
 * took it, and the default sample rate back at the end.  Sets
 * mouse_protocol and packet_size.  A mouse that refuses a knock is taken
 * in the protocol its ID says it speaks.  Returns 0 or an errno value; a
 * mouse that cannot even say its ID is not used.
 */
static int
mouse_identify(
	void)
{
	uint8_t id;
	int error;

	/* A plain mouse until a knock says otherwise. */
	mouse_protocol = PS2_ID_STANDARD;
	packet_size = 3U;

	/* The IntelliMouse knock: a wheel mouse answers ID 3 and sends a fourth byte. */
	error = mouse_knock(200U, 100U, 80U, &id);
	if (error != 0)
		return mouse_identify_current();
	if (id == PS2_ID_WHEEL) {
		mouse_protocol = PS2_ID_WHEEL;
		packet_size = 4U;

		/* The Explorer's knock, which only a wheel mouse is asked: ID 4 adds the second wheel and two buttons. */
		error = mouse_knock(200U, 200U, 80U, &id);
		if (error != 0)
			return mouse_identify_current();
		if (id == PS2_ID_EXPLORER)
			mouse_protocol = PS2_ID_EXPLORER;
	}

	/*
	 * The rate the knocks changed, back to the default.  A refusal is not
	 * a failure: a mouse left at 80 reports a second still works, and
	 * nothing is logged here under the controller's lock.
	 */
	(void)mouse_command_value(PS2_SET_SAMPLE_RATE, PS2_SAMPLE_RATE_DEFAULT);

	/* Succeeded: the protocol and the packet size are known. */
	return 0;
}

/*
 * Takes the protocol the mouse's ID says it speaks now, after a knock it
 * refused part of (a knock taken before the refusal may already have
 * changed it).  Returns 0 or an errno value.
 */
static int
mouse_identify_current(
	void)
{
	uint8_t id;
	int error;

	/* The ID as it is now. */
	error = mouse_read_id(&id);
	if (error != 0)
		return error;

	/* Four bytes for either wheel protocol, three otherwise. */
	mouse_protocol = PS2_ID_STANDARD;
	packet_size = 3U;
	if (id == PS2_ID_WHEEL || id == PS2_ID_EXPLORER) {
		mouse_protocol = id;
		packet_size = 4U;
	}

	/* Succeeded: the protocol the mouse speaks is known. */
	return 0;
}

/*
 * Collects one byte of a packet; returns 1 when a whole packet is decoded
 * into the motion (dx, dy, evdev's directions), the wheels' notches
 * (wheel, positive up; hwheel, positive right) and the buttons held
 * (MOUSE_BUTTON_*), 0 otherwise.
 */
static int
consume_byte(
	uint8_t value,
	int32_t *dx,
	int32_t *dy,
	int32_t *wheel,
	int32_t *hwheel,
	uint32_t *buttons)
{
	uint8_t first;
	uint8_t extra;

	/* A packet starts with a byte whose bit 3 is always set; anything else is skipped to resynchronise. */
	if (packet_index == 0 && (value & 0x08U) == 0)
		return 0;
	packet[packet_index] = value;
	packet_index++;

	/* A packet not whole yet. */
	if (packet_index != packet_size)
		return 0;
	packet_index = 0;

	/* A packet whose motion overflowed is dropped. */
	first = packet[0];
	if ((first & 0xc0U) != 0)
		return 0;

	/*
	 * The motion is nine bits: the byte and its sign in the first byte
	 * (bit 4 for X, bit 5 for Y), so a fast stroke past 127 counts is not
	 * read as a move back.  PS/2 positive Y is upwards; evdev REL_Y is
	 * positive downwards.
	 */
	*dx = (int32_t)packet[1];
	if ((first & 0x10U) != 0)
		*dx -= 256;
	*dy = (int32_t)packet[2];
	if ((first & 0x20U) != 0)
		*dy -= 256;
	*dy = -*dy;

	/* The three buttons of every mouse. */
	*buttons = 0;
	if ((first & 0x01U) != 0)
		*buttons |= MOUSE_BUTTON_LEFT;
	if ((first & 0x04U) != 0)
		*buttons |= MOUSE_BUTTON_MIDDLE;
	if ((first & 0x02U) != 0)
		*buttons |= MOUSE_BUTTON_RIGHT;

	/* The wheels, from the fourth byte of a wheel mouse's packet. */
	*wheel = 0;
	*hwheel = 0;
	extra = packet[3];

	/* The IntelliMouse: a signed byte, positive toward the user (evdev's wheel is positive up). */
	if (mouse_protocol == PS2_ID_WHEEL)
		*wheel = -(int32_t)(int8_t)extra;

	/*
	 * The Explorer: bits 7 and 6 say what the byte carries (as Linux's
	 * psmouse reads it): 10 a vertical and 01 a horizontal scroll of six
	 * bits; 00 or 11 a four-bit vertical scroll with the side (bit 4) and
	 * extra (bit 5) buttons.
	 */
	if (mouse_protocol == PS2_ID_EXPLORER) {
		switch (extra & 0xc0U) {
		case 0x80U:
			/* A scroll packet carries no side buttons: they stay as they were. */
			*wheel = (int32_t)(extra & 0x20U) - (int32_t)(extra & 0x1fU);
			*buttons |= last_buttons & (MOUSE_BUTTON_SIDE | MOUSE_BUTTON_EXTRA);
			break;
		case 0x40U:
			*hwheel = (int32_t)(extra & 0x20U) - (int32_t)(extra & 0x1fU);
			*buttons |= last_buttons & (MOUSE_BUTTON_SIDE | MOUSE_BUTTON_EXTRA);
			break;
		default:
			*wheel = (int32_t)(extra & 0x08U) - (int32_t)(extra & 0x07U);
			if ((extra & 0x10U) != 0)
				*buttons |= MOUSE_BUTTON_SIDE;
			if ((extra & 0x20U) != 0)
				*buttons |= MOUSE_BUTTON_EXTRA;
			break;
		}
	}

	/* Succeeded: a whole packet is decoded. */
	return 1;
}

/* Publishes one decoded packet: the motion, the wheels and the buttons that changed, then the report's end. */
static void
publish_sample(
	int32_t dx,
	int32_t dy,
	int32_t wheel,
	int32_t hwheel,
	uint32_t buttons,
	uint32_t changed_buttons)
{
	/* The motion. */
	if (dx != 0)
		drv_input_device_emit(mouse_input, EV_REL, REL_X, dx);
	if (dy != 0)
		drv_input_device_emit(mouse_input, EV_REL, REL_Y, dy);

	/* The wheels' notches. */
	if (wheel != 0)
		drv_input_device_emit(mouse_input, EV_REL, REL_WHEEL, wheel);
	if (hwheel != 0)
		drv_input_device_emit(mouse_input, EV_REL, REL_HWHEEL, hwheel);

	/* Each button that changed. */
	if ((changed_buttons & MOUSE_BUTTON_LEFT) != 0) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_LEFT,
				      (buttons & MOUSE_BUTTON_LEFT) != 0);
	}
	if ((changed_buttons & MOUSE_BUTTON_RIGHT) != 0) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_RIGHT,
				      (buttons & MOUSE_BUTTON_RIGHT) != 0);
	}
	if ((changed_buttons & MOUSE_BUTTON_MIDDLE) != 0) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_MIDDLE,
				      (buttons & MOUSE_BUTTON_MIDDLE) != 0);
	}
	if ((changed_buttons & MOUSE_BUTTON_SIDE) != 0) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_SIDE,
				      (buttons & MOUSE_BUTTON_SIDE) != 0);
	}
	if ((changed_buttons & MOUSE_BUTTON_EXTRA) != 0) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_EXTRA,
				      (buttons & MOUSE_BUTTON_EXTRA) != 0);
	}

	/* The end of the report. */
	drv_input_device_emit(mouse_input, EV_SYN, SYN_REPORT, 0);
}

/* Supports the mouse interrupt operation. */
static void
mouse_interrupt(
	int interrupt,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	uint8_t value;
	int complete;
	unsigned long irq;
	uint8_t status;
	int32_t dx = 0, dy = 0, wheel = 0, hwheel = 0;
	uint32_t buttons = 0, changed_buttons = 0;
	int report = 0;

	(void)interrupt;
	(void)argument;
	irq = spin_lock_irqsave(&controller_lock);

	/* Checks the operation status. */
	status = inb(i8042_status_port);
	if ((status & (I8042_STATUS_OUTPUT | I8042_STATUS_AUX)) ==
	    (I8042_STATUS_OUTPUT | I8042_STATUS_AUX)) {
		value = inb(i8042_data_port);

		/* Handles the complete condition. */
		complete = mouse_active
				   ? consume_byte(value, &dx, &dy, &wheel, &hwheel, &buttons)
				   : 0;
		if (complete) {
			/* A packet with motion, a wheel's notch or a button that changed is reported. */
			changed_buttons = buttons ^ last_buttons;
			if (dx != 0 ||
			    dy != 0 ||
			    wheel != 0 ||
			    hwheel != 0 ||
			    changed_buttons != 0) {
				last_buttons = buttons;
				report = 1;
			}
		}
	}

	/*
	 * Read one byte per edge.  The 8042 lowers IRQ12 when its output
	 * buffer is read, allowing the next packet byte to create a fresh edge
	 * after EOI instead of being stranded while a task IRQ is masked.
	 */
	if (!i8042_polled)
		kern_irq_send_eoi(acknowledge);

	/* Handles the report condition. */
	if (report)
		publish_sample(dx, dy, wheel, hwheel, buttons, changed_buttons);

	spin_unlock_irqrestore(&controller_lock, irq);
}

/*
 * Asks the auxiliary port once whether a mouse is on it: the port enabled
 * with its interrupt off (a controller without the port leaves it
 * disabled) and a command the mouse must acknowledge; the port is disabled
 * again.  The controller's own test of the port (0xa9) is not asked: some
 * controllers answer it wrongly while their mouse works, and only the
 * mouse's acknowledgement tells that one is there.  Returns 1 when a mouse
 * answered, 0 when there is no port or nothing answered on it.
 */
static int
mouse_probe(
	void)
{
	unsigned long irq;
	uint8_t configuration;
	int present;
	int error;

	irq = spin_lock_irqsave(&controller_lock);

	/* The port's interrupt off while the probe reads its answers itself. */
	present = 0;
	flush_output();
	error = read_config(&configuration);
	if (error == 0) {
		configuration &= (uint8_t)~I8042_CONFIG_AUX_IRQ;
		error = write_config(configuration);
	}

	/* A controller with the port clears the port's disabled bit when told to enable it. */
	if (error == 0)
		error = write_command(I8042_ENABLE_AUX);
	if (error == 0)
		error = read_config(&configuration);
	if (error == 0 && (configuration & I8042_CONFIG_AUX_OFF) != 0)
		error = ENODEV;

	/* A mouse acknowledges a command (stopping a stream it never started changes nothing). */
	if (error == 0)
		error = mouse_command(PS2_DISABLE_STREAM);
	if (error == 0)
		present = 1;

	/* The port off again until a reader starts the mouse; nothing it sent is left for the keyboard. */
	(void)write_command(I8042_DISABLE_AUX);
	flush_output();

	spin_unlock_irqrestore(&controller_lock, irq);

	/* Whether a mouse answered. */
	return present;
}

/* Supports the mouse start operation. */
static int
mouse_start(
	void)
{
	unsigned long irq;
	uint8_t configuration = 0;
	int error;

	/* Enables the auxiliary port with the interrupt held off. */
	if (!i8042_polled)
		kern_irq_mask(i8042_mouse_irq);
	irq = spin_lock_irqsave(&controller_lock);

	mouse_active = 0;
	packet_index = 0;
	flush_output();

	/* Checks the operation status. */
	error = write_command(I8042_ENABLE_AUX);
	if (error == 0)
		error = read_config(&configuration);
	if (error == 0) {
		configuration &=
			(uint8_t)~(I8042_CONFIG_AUX_IRQ | I8042_CONFIG_AUX_OFF);
		error = write_config(configuration);
	}

	/* Defaults, the richest protocol the mouse speaks (BUG-156), then the stream. */
	if (error == 0)
		error = mouse_command(PS2_SET_DEFAULTS);
	if (error == 0)
		error = mouse_identify();
	if (error == 0)
		error = mouse_command(PS2_ENABLE_STREAM);
	if (error == 0 && !i8042_polled) {
		configuration |= I8042_CONFIG_AUX_IRQ;
		error = write_config(configuration);
	}

	/* Checks the operation status. */
	if (error == 0) {
		mouse_active = 1;
	} else {
		(void)write_command(I8042_DISABLE_AUX);

		/* Checks the operation status. */
		if (error == ETIMEDOUT)
			error = ENODEV;
	}

	spin_unlock_irqrestore(&controller_lock, irq);

	/* The protocol found, for a machine's log (BUG-156). */
	if (error == 0) {
		kern_logf("i8042: mouse id=%u packet=%u\n", (unsigned)mouse_protocol, packet_size);
		if (!i8042_polled)
			kern_irq_unmask(i8042_mouse_irq);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the mouse stop operation. */
static void
mouse_stop(
	void)
{
	unsigned long irq;
	uint8_t configuration;

	if (!i8042_polled)
		kern_irq_mask(i8042_mouse_irq);
	irq = spin_lock_irqsave(&controller_lock);

	mouse_active = 0;
	packet_index = 0;
	(void)mouse_command(PS2_DISABLE_STREAM);

	/* Checks the read config result. */
	if (read_config(&configuration) == 0) {
		configuration &= (uint8_t)~I8042_CONFIG_AUX_IRQ;
		(void)write_config(configuration);
	}

	(void)write_command(I8042_DISABLE_AUX);
	flush_output();

	/* Handles the last buttons condition. */
	if (last_buttons != 0) {
		publish_sample(0, 0, 0, 0, 0, last_buttons);
		last_buttons = 0;
	}

	spin_unlock_irqrestore(&controller_lock, irq);
}

/* Supports the mouse input open operation. */
static int
mouse_input_open(
	void *context)
{
	int error = 0;

	(void)context;
	mutex_lock(&lifecycle_lock);

	/* Handles the reader count condition. */
	if (reader_count == UINT_MAX) {
		error = EMFILE;
	} else if (reader_count != 0) {
		reader_count++;
	} else {
		/* Checks the operation status. */
		error = mouse_start();
		if (error == 0)
			reader_count = 1;
	}

	mutex_unlock(&lifecycle_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the mouse input close operation. */
static void
mouse_input_close(
	void *context)
{
	(void)context;
	mutex_lock(&lifecycle_lock);

	/* Handles the reader count condition. */
	if (reader_count != 0 && --reader_count == 0)
		mouse_stop();

	mutex_unlock(&lifecycle_lock);
}

/*
 * Implements the drv pcat ps2 mouse init operation.
 */

/* Resolves one scan-code position to its stable key symbol. */
/* Resolves a physical scan position to its key symbol. */
static const char *
scan_symbol(
	uint8_t scan,
	int extended)
{
	const char *symbol;

	/* Resolves a non-extended scan through the sparse identity table. */
	if (!extended)
		return scan_symbols[scan];

	/* Starts with no symbol for the extended scan position. */
	symbol = NULL;

	/* Resolves supported E0-prefixed scan positions. */
	switch (scan) {
	case 0x1d:
		symbol = "rightctrl";
		break;
	case 0x38:
		symbol = "rightalt";
		break;
	case 0x47:
		symbol = "home";
		break;
	case 0x48:
		symbol = "up";
		break;
	case 0x49:
		symbol = "pageup";
		break;
	case 0x4b:
		symbol = "left";
		break;
	case 0x4d:
		symbol = "right";
		break;
	case 0x4f:
		symbol = "end";
		break;
	case 0x50:
		symbol = "down";
		break;
	case 0x51:
		symbol = "pagedown";
		break;
	case 0x52:
		symbol = "insert";
		break;
	case 0x53:
		symbol = "delete";
		break;
	case 0x5b:
		symbol = "leftmeta";
		break;
	case 0x5c:
		symbol = "rightmeta";
		break;
	default:
		break;
	}

	/* Reports the resolved symbol or an unsupported position. */
	return symbol;
}

/*
 * Reports every key this controller can publish.
 *
 * The capability set is derived from the scan-code tables, plain and
 * E0-prefixed, so it follows them instead of being maintained twice.  A key
 * left out of it would be dropped by the input layer, which publishes only
 * the codes a device declared.
 */
static void
keyboard_build_capabilities(
	void)
{
	const char *symbol;
	unsigned scan;
	unsigned index;
	uint16_t code;
	int extended;
	int duplicate;

	/* Every input device reports the synchronisation event. */
	keyboard_capability_count = 0;
	keyboard_capabilities[keyboard_capability_count].type = EV_SYN;
	keyboard_capabilities[keyboard_capability_count].code = SYN_REPORT;
	keyboard_capability_count++;

	/* Adds each distinct key the plain and the E0-prefixed positions can produce. */
	for (extended = 0; extended < 2; extended++) {
		for (scan = 0; scan < 128U; scan++) {
			/* A position this build understands, and the code its symbol has. */
			symbol = scan_symbol((uint8_t)scan, extended);
			if (symbol == NULL)
				continue;
			code = drv_input_key_from_symbol(symbol);
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

			/* A key already in the set is not added twice. */
			if (duplicate)
				continue;

			/* Stops at the fixed capability bound. */
			if (keyboard_capability_count >=
			    sizeof(keyboard_capabilities) /
			    sizeof(keyboard_capabilities[0]))
				return;
			keyboard_capabilities[keyboard_capability_count].type = EV_KEY;
			keyboard_capabilities[keyboard_capability_count].code = code;
			keyboard_capability_count++;
		}
	}
}

/*
 * Services one keyboard interrupt.
 *
 * The byte is read and acknowledged inside the handler so the controller can
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
	uint8_t status;
	uint8_t value;
	uint16_t code;
	uint8_t bit;
	int pressed;
	int publish;
	int held;

	(void)interrupt;
	(void)argument;
	symbol = NULL;
	code = KEY_RESERVED;
	pressed = 0;
	publish = 0;
	irq = spin_lock_irqsave(&controller_lock);

	/* Takes the byte only when it belongs to the keyboard channel. */
	status = inb(i8042_status_port);
	if ((status & (I8042_STATUS_OUTPUT | I8042_STATUS_AUX)) ==
	    I8042_STATUS_OUTPUT) {
		value = inb(i8042_data_port);

		/* Remembers an E0 prefix for the position that follows. */
		if (value == I8042_SCAN_EXTENDED) {
			keyboard_extended = 1;
		} else {
			symbol = scan_symbol(
				(uint8_t)(value & ~I8042_SCAN_RELEASE),
				keyboard_extended);
			pressed = (value & I8042_SCAN_RELEASE) == 0;
			keyboard_extended = 0;

			/* Publishes only a position this build understands. */
			if (symbol != NULL) {
				code = drv_input_key_from_symbol(symbol);
				publish = code != KEY_RESERVED;
			}
		}
	}

	/*
	 * The 8042 lowers IRQ1 once its output buffer is read, so the next
	 * byte can create a fresh edge as soon as this acknowledgement lands.
	 */
	if (!i8042_polled)
		kern_irq_send_eoi(acknowledge);

	/* A make code of a key already down is the typematic's repeat. */
	if (publish && code <= KEY_MAX) {
		bit = (uint8_t)(1U << (code % 8U));
		held = (keyboard_down[code / 8U] & bit) != 0;
		if (pressed == 0)
			keyboard_down[code / 8U] &= (uint8_t)~bit;
		else
			keyboard_down[code / 8U] |= bit;
		if (pressed != 0 && held)
			pressed = 2;
	}

	/* Publishes the decoded transition. */
	if (publish && keyboard_input != NULL) {
		drv_input_device_emit(keyboard_input, EV_KEY, code, pressed);
		drv_input_device_emit(keyboard_input, EV_SYN, SYN_REPORT, 0);
	}
	spin_unlock_irqrestore(&controller_lock, irq);
}

/*
 * Sets up a controller without interrupts: both channels on, scan-code
 * translation on and both interrupt bits off, whatever the firmware left.
 */
static int
polled_configure(
	void)
{
	uint8_t configuration;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller_lock);

	/* Drops what the firmware left in the output buffer. */
	flush_output();

	/* Turns the keyboard channel on. */
	error = write_command(I8042_ENABLE_KEYBOARD);

	/* Reads the configuration byte the firmware left. */
	if (error == 0)
		error = read_config(&configuration);

	/* Rewrites it for polling and set 1. */
	if (error == 0) {
		configuration &= (uint8_t)~(I8042_CONFIG_KEYBOARD_IRQ |
					    I8042_CONFIG_AUX_IRQ |
					    I8042_CONFIG_KEYBOARD_OFF);
		configuration |= I8042_CONFIG_TRANSLATE;
		error = write_config(configuration);
	}

	spin_unlock_irqrestore(&controller_lock, irq);

	/* Reports a controller that did not answer. */
	if (error != 0)
		return error;

	/* Succeeded: the controller is set up for polling. */
	return 0;
}

#ifndef WS018_INPUT_HID_HOST_TEST
/*
 * Takes what a controller without interrupts received, for the life of
 * the system.
 *
 * Each byte goes to the handler of its channel, which reads it again under
 * the controller's lock; a byte that changed channel in between is taken
 * by the next turn.
 */
static void
poll_worker(
	void *argument)
{
	unsigned drained;
	uint8_t status;
	uint64_t wake;

	/* The worker is given no argument. */
	(void)argument;

	/* Looks at the controller for the life of the system. */
	for (;;) {
		/* Hands at most one batch of bytes to their channels. */
		for (drained = 0; drained < I8042_POLL_DRAIN; drained++) {
			/* Stops when nothing was received. */
			status = inb(i8042_status_port);
			if ((status & I8042_STATUS_OUTPUT) == 0)
				break;

			/* Gives the byte to the mouse or the keyboard. */
			if ((status & I8042_STATUS_AUX) != 0)
				mouse_interrupt(-1, KERN_IRQ_ACK_NONE, NULL);
			else
				keyboard_interrupt(-1, KERN_IRQ_ACK_NONE, NULL);
		}

		/* Waits until the next look. */
		wake = sched_ticks() + kern_ms_to_ticks(I8042_POLL_MS);
		sched_sleep(wake);
	}
}

/* Starts the thread that looks at a controller without interrupts. */
static int
poll_start(
	void)
{
	struct thread *worker;
	int error;

	/* Creates the worker. */
	error = kthread_create(poll_worker, NULL, SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0)
		return error;

	/* Lets it run. */
	thread_start(worker);

	/* Succeeded. */
	return 0;
}
#else
/* The host test drives the handlers itself. */
static int
poll_start(
	void)
{
	/* Succeeded: nothing to start. */
	return 0;
}
#endif

/*
 * Registers one controller's keyboard and mouse.
 */
int
drv_isa_ps2_8042_init(
	const struct drv_isa_ps2_8042_config *config)
{
	struct input_device_info keyboard_info = {
		.id = {.bustype = BUS_HOST, .product = 1, .version = 1},
		.capabilities = keyboard_capabilities,
		.capability_count = 0,
	};
	struct input_device_info mouse_info = {
		.id = {.bustype = BUS_HOST, .product = 2, .version = 1},
		.capabilities = mouse_capabilities,
		.capability_count = sizeof(mouse_capabilities) /
				    sizeof(mouse_capabilities[0]),
		.open = mouse_input_open,
		.close = mouse_input_close,
	};
	unsigned long irq;
	size_t index;
	int mouse_present;
	int mouse_irq_taken;
	int keyboard_irq_taken;
	int registered;
	int error;

	/* Refuses a missing description. */
	if (config == NULL)
		return EINVAL;

	/* Records where the controller is and how it reports. */
	i8042_data_port = config->data_port;
	i8042_status_port = config->status_port;
	i8042_keyboard_irq = config->keyboard_irq;
	i8042_mouse_irq = config->mouse_irq;
	i8042_polled = 0;
	if (config->keyboard_irq < 0 || config->mouse_irq < 0)
		i8042_polled = 1;
	keyboard_info.name = config->keyboard_name;
	keyboard_info.physical_path = config->keyboard_path;
	mouse_info.name = config->mouse_name;
	mouse_info.physical_path = config->mouse_path;

	/* Starts every lock and counter out empty. */
	spin_init(&controller_lock, LOCK_RANK_DEVICE, "i8042");
	(void)mutex_init(&lifecycle_lock, LOCK_RANK_DEVICE,
			 "i8042 mouse lifecycle");
	mouse_input = NULL;
	keyboard_input = NULL;
	packet_index = 0;
	mouse_protocol = PS2_ID_STANDARD;
	packet_size = 3U;
	last_buttons = 0;
	reader_count = 0;
	mouse_active = 0;
	mouse_irq_taken = 0;
	keyboard_irq_taken = 0;
	registered = 0;

	/* A controller without interrupts is set up by the driver itself. */
	if (i8042_polled) {
		error = polled_configure();
		if (error == ETIMEDOUT)
			error = ENODEV;
		if (error != 0)
			return error;
	}

	/*
	 * Asks once whether a mouse is on the auxiliary port: without one no
	 * mouse is published, so no reader's open waits for a mouse that
	 * cannot answer.  A mouse plugged in later is not seen (PS/2 is not
	 * hot-plugged).
	 */
	if (!i8042_polled)
		kern_irq_mask(i8042_mouse_irq);
	mouse_present = mouse_probe();
	if (!mouse_present)
		kern_logf("i8042: no mouse on the auxiliary port\n");

	/* Takes the mouse's interrupt. */
	if (!i8042_polled && mouse_present) {
		error = kern_irq_register(i8042_mouse_irq, mouse_interrupt, NULL);
		if (error != 0)
			return EBUSY;
		mouse_irq_taken = 1;
	}

	/* Publishes the mouse channel. */
	if (mouse_present) {
		error = drv_input_device_register(&mouse_info, &mouse_input);
		if (error != 0)
			goto fail;
		registered = 1;
	}

	/* Publishes the keyboard channel of the same controller, no key down. */
	keyboard_extended = 0;
	for (index = 0; index < sizeof(keyboard_down); index++)
		keyboard_down[index] = 0;
	keyboard_build_capabilities();
	keyboard_info.capability_count = keyboard_capability_count;

	/* Takes the keyboard's interrupt. */
	if (!i8042_polled) {
		kern_irq_mask(i8042_keyboard_irq);
		error = kern_irq_register(i8042_keyboard_irq, keyboard_interrupt, NULL);
		if (error != 0) {
			error = EBUSY;
			goto fail;
		}
		keyboard_irq_taken = 1;
	}

	/* Publishes the keyboard. */
	error = drv_input_device_register(&keyboard_info, &keyboard_input);
	if (error != 0)
		goto fail;

	/* Lets the controller raise keyboard interrupts. */
	if (!i8042_polled)
		kern_irq_unmask(i8042_keyboard_irq);

	/*
	 * Empties the output buffer of the bytes typed before the handler was
	 * in place (BUG-251).  A key held through the boot -- Ctrl or Shift
	 * for the loader's choice -- leaves a scan code there that nobody
	 * read: IRQ1 stays raised, the masked edge was lost, and with the
	 * buffer full the 8042 makes no new edge, so the keyboard would never
	 * interrupt again.  Reading the buffer after the unmask lowers the
	 * line, and the next byte raises an edge the handler sees.  The
	 * mouse is not started yet, so a byte of its port is not wanted
	 * either.
	 */
	irq = spin_lock_irqsave(&controller_lock);

	flush_output();

	spin_unlock_irqrestore(&controller_lock, irq);

	/* Starts looking at a controller without interrupts. */
	if (i8042_polled) {
		error = poll_start();
		if (error != 0)
			return error;
	}

	/* Succeeded: the keyboard and the mouse are published. */
	return 0;

fail:
	/* Gives back what was taken before the failure. */
	if (keyboard_irq_taken)
		(void)kern_irq_unregister(i8042_keyboard_irq, keyboard_interrupt, NULL);
	if (registered)
		drv_input_device_unregister(mouse_input);
	if (mouse_irq_taken)
		(void)kern_irq_unregister(i8042_mouse_irq, mouse_interrupt, NULL);

	/* Reports why the controller could not be published. */
	return error;
}
