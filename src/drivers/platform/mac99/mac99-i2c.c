/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's I2C controller, Apple's keywest, as the system
 * controller and the I/O controller hold it, driven by polling its event
 * register.
 *
 * The controller makes a whole transfer by itself: the host sets the
 * mode (the bus's rate, how the transfer is framed and the channel, one of
 * the buses the controller drives), the device's address with the read
 * bit, and for a framed transfer the device's register (the subaddress),
 * then asks it to send the address.  The controller raises an event when
 * the address went out, one for each byte, and one when it sent the stop.
 * After the address event the status tells whether the device answered;
 * a read acknowledges each byte but the last, and a write stops after its
 * last byte.  The host clears each event by writing its bit back.
 *
 * A transfer that waits too long for an event is stopped, and one whose
 * stop does not come either leaves the controller idle.  Only one
 * transfer runs at a time: the kernel's I2C registry holds the bus while
 * the controller works.
 */

#include "drivers/platform/mac99/mac99-i2c.h"
#include <drivers/i2c/i2c.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include <stdbool.h>
#include <stddef.h>

/*
 * The registers, in steps the firmware names: mode, control, status,
 * events, events enabled, address, subaddress and data.
 *
 * Values referenced: NetBSD sys/arch/macppc/dev/ki2cvar.h (trunk, the
 * register numbers and the mode, control, status and event bits); Linux
 * v6.12 arch/powerpc/platforms/powermac/low_i2c.c (the register numbers,
 * the mode's channel in bits 4 to 7, the status's acknowledge bit, the
 * 16-byte step when the firmware names none).
 */
#define KEYWEST_MODE			0U
#define KEYWEST_CONTROL			1U
#define KEYWEST_STATUS			2U
#define KEYWEST_EVENTS			3U
#define KEYWEST_EVENTS_ENABLED		4U
#define KEYWEST_ADDRESS			5U
#define KEYWEST_SUBADDRESS		6U
#define KEYWEST_DATA			7U
#define KEYWEST_REGISTER_COUNT		8U

/* The mode's rate bits. */
#define KEYWEST_RATE_100KHZ		0x00U
#define KEYWEST_RATE_50KHZ		0x01U
#define KEYWEST_RATE_25KHZ		0x02U

/* The mode's framing: plain, with a subaddress after the address, or a subaddress write then a read. */
#define KEYWEST_FRAME_STANDARD		0x04U
#define KEYWEST_FRAME_SUBADDRESS	0x08U
#define KEYWEST_FRAME_COMBINED		0x0cU

/* The mode's channel field, and how many channels it names. */
#define KEYWEST_CHANNEL_SHIFT		4U
#define KEYWEST_CHANNELS		16U

/* The control bits: acknowledge the next byte read, send the address, send a stop. */
#define KEYWEST_CONTROL_ACK_NEXT	0x01U
#define KEYWEST_CONTROL_SEND_ADDRESS	0x02U
#define KEYWEST_CONTROL_STOP		0x04U

/* The status bits: a transfer under way, the last byte acknowledged. */
#define KEYWEST_STATUS_BUSY		0x01U
#define KEYWEST_STATUS_ACKNOWLEDGED	0x02U

/* The events: a byte moved, the address went out, the stop went out, a start (unused); all four. */
#define KEYWEST_EVENT_DATA		0x01U
#define KEYWEST_EVENT_ADDRESS		0x02U
#define KEYWEST_EVENT_STOP		0x04U
#define KEYWEST_EVENT_START		0x08U
#define KEYWEST_EVENT_ALL		0x0fU

/* The read bit of the address byte, and the highest 7-bit address. */
#define I2C_ADDRESS_READ		0x01U
#define I2C_ADDRESS_MAX			0x7fU

/* The rates the firmware names, in kHz. */
#define RATE_100_KHZ			100U
#define RATE_50_KHZ			50U

/* How long one event is waited for, and the looks at the register without a clock. */
#define KEYWEST_EVENT_WAIT_US		50000U
#define KEYWEST_EVENT_LOOKS		1000000U

/* The page the registers are mapped in. */
#define KEYWEST_PAGE			0x1000U

/* Marks a parameter a function takes but does not use. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* Where a transfer is: sending the address, reading, writing, waiting for the stop, or done. */
enum keywest_phase {
	KEYWEST_PHASE_ADDRESS,
	KEYWEST_PHASE_READ,
	KEYWEST_PHASE_WRITE,
	KEYWEST_PHASE_STOP,
	KEYWEST_PHASE_DONE
};

/*
 * The controller this driver attached: its mapped registers, their step,
 * the mode's rate bits and the channel.  One controller is attached once
 * and kept for the life of the system.
 */
struct keywest {
	volatile uint8_t *registers;
	uint32_t step;
	uint8_t rate;
	uint8_t channel;
	bool attached;
};

/*
 * One transfer under way: where it is, the bytes left to move, where the
 * next byte comes from or goes, and the errno it ends with.
 */
struct keywest_transfer {
	enum keywest_phase phase;
	bool reading;
	size_t remaining;
	const uint8_t *source;
	uint8_t *destination;
	int error;
};

/*
 * The controller, filled by drv_mac99_i2c_attach() before its bus is
 * registered and only read afterwards; the registry's hold on the bus
 * keeps the transfers apart.
 */
static struct keywest controller;

static int transfer(void *argument, uint16_t address, uint32_t speed, const uint8_t *write, size_t write_length, uint8_t *read, size_t read_length);
static int run(uint8_t address_byte, uint8_t frame, uint8_t subaddress, struct keywest_transfer *state);
static void take_events(struct keywest_transfer *state, uint8_t events);
static void take_address_event(struct keywest_transfer *state);
static void take_data_event(struct keywest_transfer *state);
static uint8_t wait_events(void);
static uint8_t register_read(unsigned number);
static void register_write(unsigned number, uint8_t value);

/*
 * Maps a keywest controller and registers one of its channels as an I2C
 * bus.
 */
int
drv_mac99_i2c_attach(
	uint64_t registers,
	uint32_t step,
	uint32_t channel,
	uint32_t rate_khz,
	struct drv_i2c_bus **bus)
{
	struct drv_i2c_bus_ops ops;
	uint64_t page;
	uint64_t offset;
	uint64_t size;
	void *window;
	int error;

	/* Refuses a second controller, and a step or a channel the controller cannot have. */
	if (bus == NULL)
		return EINVAL;
	if (controller.attached)
		return ENOSPC;
	if (step == 0 || (step & (step - 1U)) != 0)
		return EINVAL;
	if (step > KEYWEST_PAGE / KEYWEST_REGISTER_COUNT)
		return EINVAL;
	if (channel >= KEYWEST_CHANNELS)
		return EINVAL;

	/* Maps the pages the eight registers sit in. */
	page = registers & ~(uint64_t)(KEYWEST_PAGE - 1U);
	offset = registers - page;
	size = (offset + (uint64_t)step * KEYWEST_REGISTER_COUNT + KEYWEST_PAGE - 1U) & ~(uint64_t)(KEYWEST_PAGE - 1U);
	error = kern_device_map(page, (size_t)size, KERN_DEVICE_UNCACHED, &window);
	if (error != 0)
		return error;

	/* The registers, their step, the channel, and the rate the firmware named (25 kHz when it named another). */
	controller.registers = (volatile uint8_t *)window + offset;
	controller.step = step;
	controller.channel = (uint8_t)channel;
	if (rate_khz == RATE_100_KHZ) {
		controller.rate = KEYWEST_RATE_100KHZ;
	} else if (rate_khz == RATE_50_KHZ) {
		controller.rate = KEYWEST_RATE_50KHZ;
	} else {
		controller.rate = KEYWEST_RATE_25KHZ;
	}
	controller.attached = true;

	/* The controller raises no interrupt: the driver looks at its events. */
	register_write(KEYWEST_EVENTS_ENABLED, 0);

	/* Registers the channel; the controller is not a PCI function, so its location is all zero. */
	ops.pci_segment = 0;
	ops.pci_bus = 0;
	ops.pci_device = 0;
	ops.pci_function = 0;
	ops.transfer = transfer;
	ops.argument = &controller;
	error = drv_i2c_bus_register(&ops, bus);
	if (error != 0)
		return error;

	/* Succeeded: the bus is the caller's to transfer on. */
	return 0;
}

/*
 * Makes one transfer for the registry: a read after at most one written
 * byte (the device's register), or a write alone.
 */
static int
transfer(
	void *argument,
	uint16_t address,
	uint32_t speed,
	const uint8_t *write,
	size_t write_length,
	uint8_t *read,
	size_t read_length)
{
	struct keywest_transfer state;
	uint8_t address_byte;
	uint8_t frame;
	uint8_t subaddress;
	int error;

	UNUSED_PARAMETER(argument);
	UNUSED_PARAMETER(speed);

	/* Refuses a 10-bit address and a transfer that moves nothing. */
	if (address > I2C_ADDRESS_MAX)
		return EINVAL;
	if (write_length == 0 && read_length == 0)
		return EINVAL;

	/* Starts from nothing moved. */
	state.phase = KEYWEST_PHASE_ADDRESS;
	state.source = NULL;
	state.destination = NULL;
	state.remaining = 0;
	state.reading = false;
	state.error = 0;
	subaddress = 0;

	/* Frames the transfer the controller can make. */
	if (read_length != 0 && write_length == 1) {
		/* The register written as the subaddress, then the bytes read after a repeated start. */
		frame = KEYWEST_FRAME_COMBINED;
		subaddress = write[0];
		state.reading = true;
		state.destination = read;
		state.remaining = read_length;
	} else if (read_length != 0 && write_length == 0) {
		/* The bytes read alone. */
		frame = KEYWEST_FRAME_STANDARD;
		state.reading = true;
		state.destination = read;
		state.remaining = read_length;
	} else if (read_length == 0 && write_length >= 2) {
		/* The first byte as the subaddress, then the rest. */
		frame = KEYWEST_FRAME_SUBADDRESS;
		subaddress = write[0];
		state.source = write + 1;
		state.remaining = write_length - 1U;
	} else if (read_length == 0 && write_length == 1) {
		/* The one byte alone. */
		frame = KEYWEST_FRAME_STANDARD;
		state.source = write;
		state.remaining = 1;
	} else {
		/* More than one byte before a read: the controller cannot frame it. */
		return EINVAL;
	}

	/* Sends the address with the read bit and lets the events drive the rest. */
	address_byte = (uint8_t)(address << 1);
	if (state.reading)
		address_byte |= I2C_ADDRESS_READ;
	error = run(address_byte, frame, subaddress, &state);
	if (error != 0)
		return error;

	/* Succeeded: every byte moved. */
	return 0;
}

/*
 * Sets the controller up for one transfer, starts it, and follows its
 * events until the stop went out or the wait for one ran out.
 */
static int
run(
	uint8_t address_byte,
	uint8_t frame,
	uint8_t subaddress,
	struct keywest_transfer *state)
{
	uint8_t events;
	uint8_t status;

	/* Clears the events an earlier transfer left, and sets the rate, the framing and the channel. */
	events = register_read(KEYWEST_EVENTS);
	register_write(KEYWEST_EVENTS, events);
	register_write(KEYWEST_MODE, (uint8_t)(controller.rate | frame | (controller.channel << KEYWEST_CHANNEL_SHIFT)));
	register_write(KEYWEST_STATUS, 0);

	/* The address, and the subaddress of a framed transfer. */
	register_write(KEYWEST_ADDRESS, address_byte);
	if (frame != KEYWEST_FRAME_STANDARD)
		register_write(KEYWEST_SUBADDRESS, subaddress);

	/* Starts the transfer. */
	register_write(KEYWEST_CONTROL, KEYWEST_CONTROL_SEND_ADDRESS);

	/* Takes the events until the stop went out. */
	while (state->phase != KEYWEST_PHASE_DONE) {
		events = wait_events();
		if (events != 0) {
			take_events(state, events);
			continue;
		}

		/* No event in time: asks for the stop once, then gives the controller up as idle. */
		if (state->error == 0)
			state->error = ETIMEDOUT;
		if (state->phase != KEYWEST_PHASE_STOP) {
			register_write(KEYWEST_CONTROL, KEYWEST_CONTROL_STOP);
			state->phase = KEYWEST_PHASE_STOP;
			continue;
		}
		status = register_read(KEYWEST_STATUS);
		if ((status & KEYWEST_STATUS_BUSY) != 0)
			register_write(KEYWEST_STATUS, 0);
		state->phase = KEYWEST_PHASE_DONE;
	}

	/* Reports why the transfer failed. */
	if (state->error != 0)
		return state->error;

	/* Succeeded: the stop went out after every byte. */
	return 0;
}

/*
 * Moves a transfer on by the events the controller raised: the address,
 * a byte, the stop.
 */
static void
take_events(
	struct keywest_transfer *state,
	uint8_t events)
{
	/* The address went out: did the device answer? */
	if ((events & KEYWEST_EVENT_ADDRESS) != 0) {
		take_address_event(state);
		register_write(KEYWEST_EVENTS, KEYWEST_EVENT_ADDRESS);
	}

	/* A byte moved. */
	if ((events & KEYWEST_EVENT_DATA) != 0)
		take_data_event(state);

	/* The stop went out: the transfer is over, cleanly only when a stop was what it waited for. */
	if ((events & KEYWEST_EVENT_STOP) != 0) {
		register_write(KEYWEST_EVENTS, KEYWEST_EVENT_STOP);
		if (state->phase != KEYWEST_PHASE_STOP && state->error == 0)
			state->error = EIO;
		state->phase = KEYWEST_PHASE_DONE;
	}

	/* A start is raised only in a mode the driver does not use; it is cleared. */
	if ((events & KEYWEST_EVENT_START) != 0)
		register_write(KEYWEST_EVENTS, KEYWEST_EVENT_START);
}

/*
 * Moves a transfer on after its address went out: a device that did not
 * answer ends it; a read acknowledges its bytes but the last; a write
 * sends its first byte.
 *
 * Procedure referenced: Linux v6.12
 * arch/powerpc/platforms/powermac/low_i2c.c (kw_i2c_handle_interrupt(),
 * the address event: no acknowledgement leaves the controller to stop,
 * the read's acknowledge bit when more than one byte is to come).
 */
static void
take_address_event(
	struct keywest_transfer *state)
{
	uint8_t status;

	/* An event out of turn ends the transfer. */
	if (state->phase != KEYWEST_PHASE_ADDRESS) {
		register_write(KEYWEST_CONTROL, KEYWEST_CONTROL_STOP);
		state->phase = KEYWEST_PHASE_STOP;
		state->error = EIO;
		return;
	}

	/* No device answered the address: the controller stops by itself. */
	status = register_read(KEYWEST_STATUS);
	if ((status & KEYWEST_STATUS_ACKNOWLEDGED) == 0) {
		state->phase = KEYWEST_PHASE_STOP;
		state->error = ENXIO;
		return;
	}

	/* A read: acknowledges the bytes to come while more than one is left. */
	if (state->reading) {
		state->phase = KEYWEST_PHASE_READ;
		if (state->remaining > 1)
			register_write(KEYWEST_CONTROL, KEYWEST_CONTROL_ACK_NEXT);
		return;
	}

	/* A write: sends its first byte. */
	state->phase = KEYWEST_PHASE_WRITE;
	register_write(KEYWEST_DATA, *state->source);
	state->source++;
	state->remaining--;
}

/*
 * Moves a transfer on after a byte moved: a read keeps it and stops
 * acknowledging before the last; a write sends the next or the stop.
 */
static void
take_data_event(
	struct keywest_transfer *state)
{
	uint8_t status;

	/* A read: keeps the byte, then leaves the last one unacknowledged. */
	if (state->phase == KEYWEST_PHASE_READ) {
		*state->destination = register_read(KEYWEST_DATA);
		state->destination++;
		state->remaining--;
		register_write(KEYWEST_EVENTS, KEYWEST_EVENT_DATA);
		if (state->remaining == 0) {
			state->phase = KEYWEST_PHASE_STOP;
		} else if (state->remaining == 1) {
			register_write(KEYWEST_CONTROL, 0);
		}
		return;
	}

	/* A write: a byte the device refused ends it; otherwise the next byte, or the stop after the last. */
	if (state->phase == KEYWEST_PHASE_WRITE) {
		status = register_read(KEYWEST_STATUS);
		if ((status & KEYWEST_STATUS_ACKNOWLEDGED) == 0) {
			state->phase = KEYWEST_PHASE_STOP;
			state->error = EIO;
		} else if (state->remaining != 0) {
			register_write(KEYWEST_DATA, *state->source);
			state->source++;
			state->remaining--;
		} else {
			register_write(KEYWEST_CONTROL, KEYWEST_CONTROL_STOP);
			state->phase = KEYWEST_PHASE_STOP;
		}
		register_write(KEYWEST_EVENTS, KEYWEST_EVENT_DATA);
		return;
	}

	/* A byte out of turn ends the transfer. */
	if (state->phase != KEYWEST_PHASE_STOP) {
		register_write(KEYWEST_CONTROL, KEYWEST_CONTROL_STOP);
		state->phase = KEYWEST_PHASE_STOP;
		state->error = EIO;
	}
	register_write(KEYWEST_EVENTS, KEYWEST_EVENT_DATA);
}

/*
 * Waits for the controller to raise an event; 0 when none came in time.
 * Without the time base's counter it looks a fixed number of times.
 */
static uint8_t
wait_events(
	void)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t duration;
	uint32_t looks;
	uint8_t events;
	bool counted;

	/* The start and the length of the wait in counter ticks. */
	duration = 0;
	counted = kern_rtc_read_counter(&start, &frequency);
	if (counted && frequency != 0)
		duration = frequency * KEYWEST_EVENT_WAIT_US / 1000000U + 1U;

	/* Looks at the events until one is raised or the time is up. */
	for (looks = 0; looks < KEYWEST_EVENT_LOOKS; looks++) {
		events = register_read(KEYWEST_EVENTS) & KEYWEST_EVENT_ALL;
		if (events != 0)
			return events;
		if (duration == 0)
			continue;
		counted = kern_rtc_read_counter(&now, &frequency);
		if (!counted)
			continue;
		if (now - start >= duration)
			break;
	}

	/* No event came. */
	return 0;
}

/* Reads one of the controller's registers. */
static uint8_t
register_read(
	unsigned number)
{
	uint8_t value;

	/* The register at its step. */
	value = kern_mmio_read8(controller.registers + number * controller.step);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one of the controller's registers. */
static void
register_write(
	unsigned number,
	uint8_t value)
{
	/* The register at its step. */
	kern_mmio_write8(controller.registers + number * controller.step, value);
}
