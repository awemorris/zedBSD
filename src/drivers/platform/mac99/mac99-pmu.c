/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's power manager (PMU), as the kernel uses it: the
 * PowerBook's own keyboard and trackpad, which sit on the PMU's ADB, as
 * an evdev keyboard and an evdev mouse.
 *
 * The firmware may have moved the ADB devices to other addresses, so the
 * driver first resets the bus, which puts each device back at its
 * default address: the keyboard at 2, the mouse (a PowerBook's trackpad)
 * at 3.  The PMU then polls them itself (autopoll) and keeps what a device
 * answered until the host takes it.  While it holds something it pulls
 * its GPIO line (extint-gpio1 in the I/O controller) low; the host then
 * asks for its interrupt bits (INT_ACK), whose reply is the bits, and,
 * when the ADB bit is set, the ADB command the data answered (the
 * device's address in the high four bits, talk register 0 in the low)
 * and the device's data.  A thread looks at the line every 10 ms and
 * asks while it is low; it also asks every 250 ms whatever the line says,
 * so a board whose line is not where the firmware said still types.
 *
 * A request is the command byte, its data (with a length byte first for
 * the commands whose length varies) and the reply (with a length byte
 * first for those whose reply varies), each byte shifted through the
 * VIA's shift register with the TREQ and TACK handshake on port B; the
 * hardware layer talks to the PMU the same way for the clock and the
 * power.  Each request runs with interrupts off, so on this uniprocessor
 * board it never interleaves with another.
 *
 * The keyboard's register 0 holds two key transitions, a byte each: bit
 * 7 set for a release, the key's code in bits 0 to 6, 0xff for none.
 * The caps lock key locks down: it reports a press when it locks and a
 * release when it unlocks, so each of its transitions is one toggle and
 * is published as a press and a release.  The power key (0x7f in both
 * bytes) is not published.  The mouse's register 0 is two bytes: the
 * button in bit 7 of the first (0 down), the vertical and horizontal
 * moves in bits 0 to 6 of the first and second, seven-bit two's
 * complement.
 */

#include "drivers/platform/mac99/mac99-pmu.h"
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/input-device.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>
#include <uapi/input.h>
#include "kern/klog.h"

/*
 * The VIA's registers, 0x200 bytes apart: port B, its direction, the
 * shift register, ACR, IFR and IER; port B's handshake lines; ACR's shift
 * register modes; the shift register's interrupt flag.
 *
 * Values referenced: the Rockwell R6522 and WDC W65C22 data sheets (the
 * register numbers, ACR bits 2 to 4, IFR and IER bit 2); QEMU 10.0
 * hw/misc/macio/pmu.c (the register stride and the TREQ and TACK bits of
 * the PowerBook's PMU99).
 */
#define VIA_B			0x0000U
#define VIA_DIRB		0x0400U
#define VIA_SR			0x1400U
#define VIA_ACR			0x1600U
#define VIA_IFR			0x1a00U
#define VIA_IER			0x1c00U
#define VIA_SIZE		0x2000U

/* Port B's handshake lines, both active low: TREQ the host's, TACK the PMU's. */
#define VIA_B_TACK		0x08U
#define VIA_B_TREQ		0x10U

/* ACR's shift register mode: its three bits, shift in and shift out on the external clock. */
#define VIA_ACR_SR_MASK		0x1cU
#define VIA_ACR_SR_IN		0x0cU
#define VIA_ACR_SR_OUT		0x1cU

/* IFR's and IER's shift register bit. */
#define VIA_SR_FLAG		0x04U

/* The GPIO register's input level: set while the line is high (the PMU has nothing). */
#define GPIO_LEVEL		0x02U

/* The page a GPIO register is mapped in. */
#define GPIO_PAGE		0x1000U

/*
 * The commands: an ADB command (its data counted), set the interrupt
 * mask (one byte), take the interrupt bits (the reply counted); the
 * interrupt bit of ADB data; the GPIO register's input level.
 *
 * Values referenced: QEMU 10.0 hw/misc/macio/pmu.c and
 * include/hw/misc/macio/pmu.h (the command numbers and the table of data
 * and reply lengths, PMU_INT_ADB, the autopoll form 0x00 0x86 and a
 * 16-bit mask, the other forms' ADB command, flags and length bytes) and
 * hw/misc/macio/gpio.c (the input level bit), as they emulate the
 * PowerBook's PMU99 and KeyLargo.
 */
#define PMU_ADB_COMMAND		0x20U
#define PMU_SET_INT_MASK	0x70U
#define PMU_INT_ACK		0x78U

/* The interrupt bit of ADB data. */
#define PMU_INT_ADB		0x10U

/*
 * The ADB bus reset, the default addresses of the keyboard and the mouse,
 * and the talk command's register 0; the keyboard's and the mouse's
 * register 0.
 *
 * Values referenced: Apple, Guide to the Macintosh Family Hardware,
 * second edition, the Apple Desktop Bus chapter (the command byte, the
 * default addresses, the keyboard's and the mouse's register 0).
 */
#define ADB_BUS_RESET		0x00U
#define ADB_KEYBOARD		2U
#define ADB_MOUSE		3U
#define ADB_TALK_0		0x0cU
#define ADB_COMMAND_MASK	0x0fU

/* The keyboard's byte for no key, its release bit, the caps lock and power keys. */
#define ADB_KEY_NONE		0xffU
#define ADB_KEY_RELEASE		0x80U
#define ADB_KEY_CODE		0x7fU
#define ADB_KEY_CAPS_LOCK	0x39U
#define ADB_KEY_POWER		0x7fU

/* The mouse's button bit (0 when down) and a move's seven bits. */
#define ADB_MOUSE_BUTTON_UP	0x80U
#define ADB_MOUSE_MOVE		0x7fU
#define ADB_MOUSE_SIGN		0x40U

/* The longest reply taken, the handshake step's time and the thread's looks. */
#define PMU_REPLY_MAX		16U
#define PMU_STEP_US		50000U
#define PMU_POLL_MS		10U
#define PMU_ASK_ANYWAY_MS	250U
#define PMU_DRAIN_MAX		8U

/*
 * The ADB key codes the keyboard sends, to the evdev keys; 0 for none.
 *
 * Values referenced: Apple, Guide to the Macintosh Family Hardware,
 * second edition (the ADB keyboard's key codes); QEMU 10.0
 * include/hw/input/adb-keys.h lists the same codes.
 */
static const uint16_t adb_keys[128] = {
	[0x00] = KEY_A,		[0x01] = KEY_S,		[0x02] = KEY_D,		[0x03] = KEY_F,
	[0x04] = KEY_H,		[0x05] = KEY_G,		[0x06] = KEY_Z,		[0x07] = KEY_X,
	[0x08] = KEY_C,		[0x09] = KEY_V,		[0x0a] = KEY_102ND,	[0x0b] = KEY_B,
	[0x0c] = KEY_Q,		[0x0d] = KEY_W,		[0x0e] = KEY_E,		[0x0f] = KEY_R,
	[0x10] = KEY_Y,		[0x11] = KEY_T,		[0x12] = KEY_1,		[0x13] = KEY_2,
	[0x14] = KEY_3,		[0x15] = KEY_4,		[0x16] = KEY_6,		[0x17] = KEY_5,
	[0x18] = KEY_EQUAL,	[0x19] = KEY_9,		[0x1a] = KEY_7,		[0x1b] = KEY_MINUS,
	[0x1c] = KEY_8,		[0x1d] = KEY_0,		[0x1e] = KEY_RIGHTBRACE, [0x1f] = KEY_O,
	[0x20] = KEY_U,		[0x21] = KEY_LEFTBRACE,	[0x22] = KEY_I,		[0x23] = KEY_P,
	[0x24] = KEY_ENTER,	[0x25] = KEY_L,		[0x26] = KEY_J,		[0x27] = KEY_APOSTROPHE,
	[0x28] = KEY_K,		[0x29] = KEY_SEMICOLON,	[0x2a] = KEY_BACKSLASH,	[0x2b] = KEY_COMMA,
	[0x2c] = KEY_SLASH,	[0x2d] = KEY_N,		[0x2e] = KEY_M,		[0x2f] = KEY_DOT,
	[0x30] = KEY_TAB,	[0x31] = KEY_SPACE,	[0x32] = KEY_GRAVE,	[0x33] = KEY_BACKSPACE,
	[0x34] = KEY_KPENTER,	[0x35] = KEY_ESC,	[0x36] = KEY_LEFTCTRL,	[0x37] = KEY_LEFTMETA,
	[0x38] = KEY_LEFTSHIFT,	[0x39] = KEY_CAPSLOCK,	[0x3a] = KEY_LEFTALT,	[0x3b] = KEY_LEFT,
	[0x3c] = KEY_RIGHT,	[0x3d] = KEY_DOWN,	[0x3e] = KEY_UP,
	[0x41] = KEY_KPDOT,	[0x43] = KEY_KPASTERISK, [0x45] = KEY_KPPLUS,	[0x47] = KEY_NUMLOCK,
	[0x4b] = KEY_KPSLASH,	[0x4c] = KEY_KPENTER,	[0x4e] = KEY_KPMINUS,	[0x51] = KEY_KPEQUAL,
	[0x52] = KEY_KP0,	[0x53] = KEY_KP1,	[0x54] = KEY_KP2,	[0x55] = KEY_KP3,
	[0x56] = KEY_KP4,	[0x57] = KEY_KP5,	[0x58] = KEY_KP6,	[0x59] = KEY_KP7,
	[0x5b] = KEY_KP8,	[0x5c] = KEY_KP9,
	[0x60] = KEY_F5,	[0x61] = KEY_F6,	[0x62] = KEY_F7,	[0x63] = KEY_F3,
	[0x64] = KEY_F8,	[0x65] = KEY_F9,	[0x67] = KEY_F11,	[0x69] = KEY_F13,
	[0x6b] = KEY_F14,	[0x6d] = KEY_F10,	[0x6f] = KEY_F12,	[0x71] = KEY_F15,
	[0x72] = KEY_INSERT,	[0x73] = KEY_HOME,	[0x74] = KEY_PAGEUP,	[0x75] = KEY_DELETE,
	[0x76] = KEY_F4,	[0x77] = KEY_END,	[0x78] = KEY_F2,	[0x79] = KEY_PAGEDOWN,
	[0x7a] = KEY_F1,	[0x7b] = KEY_RIGHTSHIFT, [0x7c] = KEY_RIGHTALT,	[0x7d] = KEY_RIGHTCTRL
};

/* The mouse's events. */
static const struct input_capability mouse_capabilities[] = {
	{EV_SYN, SYN_REPORT}, {EV_KEY, BTN_LEFT}, {EV_REL, REL_X}, {EV_REL, REL_Y}
};

/*
 * The VIA's registers and the GPIO register of the PMU's line (NULL when
 * the firmware named none), mapped once by drv_mac99_pmu_init() and kept
 * for the life of the system.
 */
static volatile uint8_t *via;
static volatile uint8_t *gpio;

/* Keeps each request whole, with interrupts off. */
static struct spinlock request_lock;

/* The thread's sleep. */
static struct spinlock sleep_lock;

/* The input devices, published once and kept. */
static struct input_device *keyboard_input;
static struct input_device *mouse_input;

/* The keyboard's events, every key the table names; filled once. */
static struct input_capability keyboard_capabilities[1U + 128U];
static unsigned keyboard_capability_count;

/* The mouse button's last state, so only its changes are published. */
static int mouse_button_down;

/* The ADB addresses data has come from, one bit each, so each is reported once. */
static uint32_t addresses_seen;

static int request(uint8_t command, const uint8_t *data, unsigned length, uint8_t *reply, unsigned *reply_length);
static int send_byte(uint8_t value);
static int receive_byte(uint8_t *value);
static int wait_flag(void);
static int wait_tack_released(void);
static uint8_t via_read(uint32_t offset);
static void via_write(uint32_t offset, uint8_t value);
static void worker(void *argument);
static void take_reports(void);
static void keyboard_byte(uint8_t value);
static void mouse_report(const uint8_t *data, unsigned length);
static int32_t mouse_move(uint8_t value);
static void build_keyboard_capabilities(void);

/*
 * Publishes the ADB keyboard and mouse behind the PMU, sets the PMU to
 * poll them and to report their data, and starts the thread that takes
 * it.
 */
int
drv_mac99_pmu_init(
	void)
{
	const struct input_device_info keyboard_info = {
		.name = "Apple ADB keyboard",
		.physical_path = "mac99/pmu/adb2",
		.id = {.bustype = BUS_HOST, .vendor = 0x05ac, .product = 0x0002, .version = 1},
		.capabilities = keyboard_capabilities,
		.capability_count = 0,
	};
	const struct input_device_info mouse_info = {
		.name = "Apple ADB mouse",
		.physical_path = "mac99/pmu/adb3",
		.id = {.bustype = BUS_HOST, .vendor = 0x05ac, .product = 0x0003, .version = 1},
		.capabilities = mouse_capabilities,
		.capability_count = sizeof(mouse_capabilities) / sizeof(mouse_capabilities[0]),
		.properties = 1U << INPUT_PROP_POINTER,
	};
	struct input_device_info keyboard;
	struct thread *thread;
	const uint32_t *table;
	uint8_t reply[PMU_REPLY_MAX];
	uint8_t data[4];
	unsigned reply_length;
	unsigned long irq;
	void *window;
	uint8_t value;
	int error;

	/* The PMU the firmware described; a board without one has none. */
	table = kern_boot_handoff("mac99.pmu");
	if (table == NULL || table[0] == 0)
		return ENODEV;
	spin_init(&request_lock, LOCK_RANK_DEVICE, "mac99 pmu");
	spin_init(&sleep_lock, LOCK_RANK_DEVICE, "mac99 pmu thread");

	/* Its VIA, and the GPIO register of its line when there is one. */
	error = kern_device_map(table[0], VIA_SIZE, KERN_DEVICE_UNCACHED, &window);
	if (error != 0)
		return error;
	via = window;
	if (table[2] != 0) {
		error = kern_device_map(table[2] & ~(uint64_t)(GPIO_PAGE - 1U), GPIO_PAGE, KERN_DEVICE_UNCACHED, &window);
		if (error == 0)
			gpio = (volatile uint8_t *)window + (table[2] & (GPIO_PAGE - 1U));
	}

	/* TREQ an output, high; TACK an input; the shift register's interrupt not to the processor. */
	irq = spin_lock_irqsave(&request_lock);
	value = via_read(VIA_DIRB);
	via_write(VIA_DIRB, (uint8_t)((value | VIA_B_TREQ) & ~VIA_B_TACK));
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) | VIA_B_TREQ));
	via_write(VIA_IER, VIA_SR_FLAG);
	spin_unlock_irqrestore(&request_lock, irq);

	/* The input devices. */
	build_keyboard_capabilities();
	keyboard = keyboard_info;
	keyboard.capability_count = keyboard_capability_count;
	error = drv_input_device_register(&keyboard, &keyboard_input);
	if (error != 0)
		return error;
	error = drv_input_device_register(&mouse_info, &mouse_input);
	if (error != 0)
		mouse_input = NULL;

	/* Reports for ADB data only; the devices back at their default addresses; autopoll of the keyboard and the mouse. */
	data[0] = PMU_INT_ADB;
	reply_length = 0;
	error = request(PMU_SET_INT_MASK, data, 1, reply, &reply_length);
	if (error == 0) {
		data[0] = ADB_BUS_RESET;
		data[1] = 0x00U;
		data[2] = 0x00U;
		error = request(PMU_ADB_COMMAND, data, 3, reply, &reply_length);
	}
	if (error == 0) {
		data[0] = 0x00U;
		data[1] = 0x86U;
		data[2] = 0x00U;
		data[3] = (uint8_t)((1U << ADB_KEYBOARD) | (1U << ADB_MOUSE));
		error = request(PMU_ADB_COMMAND, data, sizeof(data), reply, &reply_length);
	}
	if (error != 0)
		kern_logf("mac99-pmu: the PMU did not answer (%d)\n", error);

	/* The thread that takes the reports. */
	error = kthread_create(worker, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;
	thread_start(thread);
	kern_logf("mac99-pmu: PMU at %llx, line %s; ADB keyboard and mouse\n",
		  (unsigned long long)table[0], gpio != NULL ? "watched" : "not found");

	/* Succeeded: the keyboard types. */
	return 0;
}

/*
 * Takes the PMU's reports while its line is low, and asks anyway every
 * PMU_ASK_ANYWAY_MS; looks every PMU_POLL_MS.
 */
static void
worker(
	void *argument)
{
	uint64_t last_ask;
	uint64_t now;
	unsigned long irq;
	uint8_t level;
	bool pending;

	(void)argument;

	/* Looks for the life of the system. */
	last_ask = 0;
	for (;;) {
		/* The line low, no line to look at, or the time since the last ask. */
		pending = true;
		if (gpio != NULL) {
			level = kern_mmio_read8(gpio);
			if ((level & GPIO_LEVEL) != 0)
				pending = false;
		}
		now = sched_ticks();
		if (now >= last_ask + kern_ms_to_ticks(PMU_ASK_ANYWAY_MS))
			pending = true;
		if (pending) {
			take_reports();
			last_ask = sched_ticks();
		}

		/* Sleeps to the next look. */
		irq = spin_lock_irqsave(&sleep_lock);
		sched_sleep_locked(sched_ticks() + kern_ms_to_ticks(PMU_POLL_MS), &sleep_lock);
		spin_unlock_irqrestore(&sleep_lock, irq);
	}
}

/*
 * Asks for the interrupt bits until the PMU has nothing more (or a few
 * times), and publishes the ADB data among them.
 */
static void
take_reports(
	void)
{
	uint8_t reply[PMU_REPLY_MAX];
	unsigned reply_length;
	unsigned round;
	unsigned index;
	uint8_t command;
	uint8_t level;
	int error;

	/* Asks while the PMU has more, a few times at most. */
	for (round = 0; round < PMU_DRAIN_MAX; round++) {
		/* The bits, and with ADB data the command it answered and the data. */
		reply_length = sizeof(reply);
		error = request(PMU_INT_ACK, NULL, 0, reply, &reply_length);
		if (error != 0 || reply_length == 0)
			return;
		if ((reply[0] & PMU_INT_ADB) != 0 && reply_length >= 2U) {
			command = reply[1];
			if ((command & ADB_COMMAND_MASK) == ADB_TALK_0 && (addresses_seen & (1U << (command >> 4))) == 0) {
				addresses_seen |= 1U << (command >> 4);
				kern_logf("mac99-pmu: ADB data from address %u\n", (unsigned)(command >> 4));
			}
			if ((command & ADB_COMMAND_MASK) == ADB_TALK_0 && (command >> 4) == ADB_KEYBOARD) {
				for (index = 2; index < reply_length; index++)
					keyboard_byte(reply[index]);
			} else if ((command & ADB_COMMAND_MASK) == ADB_TALK_0 && (command >> 4) == ADB_MOUSE) {
				mouse_report(&reply[2], reply_length - 2U);
			}
		}

		/* Done once the line is back up; without the line, after one ask with nothing. */
		if (gpio != NULL) {
			level = kern_mmio_read8(gpio);
			if ((level & GPIO_LEVEL) != 0)
				return;
		} else if ((reply[0] & PMU_INT_ADB) == 0) {
			return;
		}
	}
}

/* Publishes one key transition of the keyboard's register 0. */
static void
keyboard_byte(
	uint8_t value)
{
	uint16_t code;
	int pressed;

	/* No key, the power key, and keys the table does not name are not published. */
	if (value == ADB_KEY_NONE || (value & ADB_KEY_CODE) == ADB_KEY_POWER)
		return;
	code = adb_keys[value & ADB_KEY_CODE];
	if (code == 0 || keyboard_input == NULL)
		return;
	pressed = (value & ADB_KEY_RELEASE) == 0;

	/* The caps lock key's transitions are each one toggle. */
	if ((value & ADB_KEY_CODE) == ADB_KEY_CAPS_LOCK) {
		drv_input_device_emit(keyboard_input, EV_KEY, code, 1);
		drv_input_device_emit(keyboard_input, EV_SYN, SYN_REPORT, 0);
		drv_input_device_emit(keyboard_input, EV_KEY, code, 0);
		drv_input_device_emit(keyboard_input, EV_SYN, SYN_REPORT, 0);
		return;
	}

	/* The transition. */
	drv_input_device_emit(keyboard_input, EV_KEY, code, pressed);
	drv_input_device_emit(keyboard_input, EV_SYN, SYN_REPORT, 0);
}

/* Publishes the mouse's register 0: the moves, then the button when it changed. */
static void
mouse_report(
	const uint8_t *data,
	unsigned length)
{
	int down;

	/* Two bytes, or nothing to publish. */
	if (length < 2U || mouse_input == NULL)
		return;
	drv_input_device_emit(mouse_input, EV_REL, REL_X, mouse_move(data[1]));
	drv_input_device_emit(mouse_input, EV_REL, REL_Y, mouse_move(data[0]));
	down = (data[0] & ADB_MOUSE_BUTTON_UP) == 0;
	if (down != mouse_button_down) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_LEFT, down);
		mouse_button_down = down;
	}
	drv_input_device_emit(mouse_input, EV_SYN, SYN_REPORT, 0);
}

/* Reads a seven-bit two's complement move. */
static int32_t
mouse_move(
	uint8_t value)
{
	int32_t move;

	/* The seven bits, negative when the sign bit is set. */
	move = (int32_t)(value & ADB_MOUSE_MOVE);
	if ((value & ADB_MOUSE_SIGN) != 0)
		move -= (int32_t)(ADB_MOUSE_MOVE + 1U);
	return move;
}

/* Lists the keyboard's events: the report, and every key the table names. */
static void
build_keyboard_capabilities(
	void)
{
	unsigned scan;

	/* The report first, then each key once (the two enter keys share one). */
	keyboard_capability_count = 0;
	keyboard_capabilities[keyboard_capability_count].type = EV_SYN;
	keyboard_capabilities[keyboard_capability_count].code = SYN_REPORT;
	keyboard_capability_count++;
	for (scan = 0; scan < 128U; scan++) {
		if (adb_keys[scan] == 0 || scan == 0x34U)
			continue;
		keyboard_capabilities[keyboard_capability_count].type = EV_KEY;
		keyboard_capabilities[keyboard_capability_count].code = adb_keys[scan];
		keyboard_capability_count++;
	}
}

/*
 * Gives the PMU one command with its data and takes its reply, with
 * interrupts off throughout.  The data's and the reply's lengths are the
 * command's: the commands this driver gives are an ADB command (counted
 * data, no reply), set the interrupt mask (one byte, no reply) and take
 * the interrupt bits (no data, counted reply).  On entry reply_length is
 * the reply's room; on return the bytes taken.
 */
static int
request(
	uint8_t command,
	const uint8_t *data,
	unsigned length,
	uint8_t *reply,
	unsigned *reply_length)
{
	unsigned long irq;
	unsigned room;
	unsigned count;
	unsigned index;
	uint8_t byte;
	int error;

	/* The command, the count when the data's length varies, and the data. */
	room = *reply_length;
	*reply_length = 0;
	irq = spin_lock_irqsave(&request_lock);
	error = send_byte(command);
	if (error == 0 && command == PMU_ADB_COMMAND)
		error = send_byte((uint8_t)length);
	for (index = 0; error == 0 && index < length; index++)
		error = send_byte(data[index]);

	/* The reply: only INT_ACK has one, counted. */
	if (error == 0 && command == PMU_INT_ACK) {
		error = receive_byte(&byte);
		count = byte;
		for (index = 0; error == 0 && index < count; index++) {
			error = receive_byte(&byte);
			if (index < room)
				reply[index] = byte;
		}
		if (error == 0)
			*reply_length = count < room ? count : room;
	}
	spin_unlock_irqrestore(&request_lock, irq);
	return error;
}

/* Shifts one byte out to the PMU. */
static int
send_byte(
	uint8_t value)
{
	int error;

	/* Shift out, the byte, then the request. */
	via_write(VIA_ACR, (uint8_t)((via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_OUT));
	via_write(VIA_SR, value);
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) & ~VIA_B_TREQ));
	error = wait_flag();

	/* The request let go, and the PMU's acknowledgement back up. */
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) | VIA_B_TREQ));
	if (error != 0)
		return error;
	return wait_tack_released();
}

/* Shifts one byte in from the PMU. */
static int
receive_byte(
	uint8_t *value)
{
	int error;

	/* Shift in, the flag cleared by reading the register, then the request. */
	via_write(VIA_ACR, (uint8_t)((via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_IN));
	(void)via_read(VIA_SR);
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) & ~VIA_B_TREQ));
	error = wait_flag();

	/* The request let go, the byte, and the PMU's acknowledgement back up. */
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) | VIA_B_TREQ));
	if (error != 0)
		return error;
	*value = via_read(VIA_SR);
	return wait_tack_released();
}

/* Waits for the shift register's interrupt flag, and clears it. */
static int
wait_flag(
	void)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint8_t flags;
	bool counted;

	/* Until the flag rises or the step's time is up. */
	counted = kern_rtc_read_counter(&start, &frequency);
	if (!counted)
		return EIO;
	for (;;) {
		flags = via_read(VIA_IFR);
		if ((flags & VIA_SR_FLAG) != 0)
			break;
		counted = kern_rtc_read_counter(&now, &frequency);
		if (!counted)
			return EIO;
		if ((now - start) * 1000000U / frequency > PMU_STEP_US)
			return ETIMEDOUT;
	}

	/* Succeeded: the flag, cleared. */
	via_write(VIA_IFR, VIA_SR_FLAG);
	return 0;
}

/* Waits for the PMU's TACK to go back up. */
static int
wait_tack_released(
	void)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint8_t lines;
	bool counted;

	/* Until the line is high or the step's time is up. */
	counted = kern_rtc_read_counter(&start, &frequency);
	if (!counted)
		return EIO;
	for (;;) {
		lines = via_read(VIA_B);
		if ((lines & VIA_B_TACK) != 0)
			break;
		counted = kern_rtc_read_counter(&now, &frequency);
		if (!counted)
			return EIO;
		if ((now - start) * 1000000U / frequency > PMU_STEP_US)
			return ETIMEDOUT;
	}

	/* Succeeded: the PMU is ready for the next byte. */
	return 0;
}

/* Reads a VIA register. */
static uint8_t
via_read(
	uint32_t offset)
{
	uint8_t value;

	/* One byte, ordered against the accesses around it. */
	value = kern_mmio_read8(&via[offset]);
	kern_io_barrier();
	return value;
}

/* Writes a VIA register. */
static void
via_write(
	uint32_t offset,
	uint8_t value)
{
	/* One byte, ordered against the accesses around it. */
	kern_mmio_write8(&via[offset], value);
	kern_io_barrier();
}
