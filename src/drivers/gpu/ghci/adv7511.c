/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * XXX: This file should be rewritten to be a generic I2C driver, not
 * just for the ADV7511.  The ADV7511 is just an example of the KC705
 * FPGA prototype.
 *
 * An ADV7511 HDMI transmitter behind a GHCI display controller's I2C
 * master (DC_CAPS[2]): the bus to it is opened through the board's I2C
 * switch when there is one, the chip is identified by its revision, and
 * it is set up for the controller's parallel output, 16-bit YCbCr 4:2:2
 * with separate syncs, sent to the monitor as HDMI.
 *
 * The I2C master runs one byte per command: a START (repeated when the
 * bus is held), a byte written or read, and an optional STOP.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/klog.h>
#include <uapi/errno.h>

#include "ghci.h"
#include "ghci-regs.h"

/* The commands of I2C_CTRL and the bits of I2C_STATUS. */
#define I2C_START			0x01U
#define I2C_STOP			0x02U
#define I2C_WRITE			0x04U
#define I2C_READ			0x08U
#define I2C_BUSY			0x01U
#define I2C_NACK			0x02U

/* How long one byte may take at 100 kHz, with room for clock stretching. */
#define I2C_BYTE_MS			10U

/*
 * The board's I2C switch and the channel the transmitter is on, and the
 * transmitter's address and revision.
 *
 * Values referenced: Xilinx UG810 KC705 Evaluation Board (the PCA9548
 * switch at 0x74, channel 5 to the ADV7511); Analog Devices ADV7511
 * Programming Guide (the main map at 0x39, CHIP_REVISION at 0x00).
 */
#define SWITCH_ADDRESS			0x74U
#define SWITCH_CHANNEL			0x20U
#define ADV7511_ADDRESS			0x39U
#define ADV7511_REVISION_REGISTER	0x00U
#define ADV7511_REVISION		0x14U

/*
 * The transmitter's status register and its hot-plug bit: the level of
 * the HPD line from the sink.
 *
 * Value referenced: Analog Devices ADV7511 Programming Guide (register
 * 0x42, bit 6 HPD state).  Not yet checked on the board.
 */
#define ADV7511_STATUS_REGISTER		0x42U
#define ADV7511_STATUS_HPD		0x40U

/*
 * One write of the transmitter's set-up: a register and its value.
 */
struct adv7511_write {
	uint8_t reg;
	uint8_t value;
};

/*
 * The transmitter's set-up, in order: power on, the registers the
 * programming guide fixes, hot-plug detection taken as present, the
 * input (ID 1: 16-bit YCbCr 4:2:2 with separate syncs, 8 bits, style 1),
 * the aspect, and HDMI mode with the AVI InfoFrame saying YCbCr 4:4:4.
 *
 * Values referenced: Analog Devices ADV7511 Programming Guide (the
 * power-up and fixed registers 0x41, 0x98, 0x9A, 0x9C, 0x9D, 0xA2, 0xA3,
 * 0xE0, 0xF9; the input and output format 0x15, 0x16, 0x17; the HPD
 * control 0xD6; the HDMI mode 0xAF; the AVI InfoFrame 0x44, 0x55);
 * Linux v6.19 drivers/gpu/drm/bridge/adv7511/adv7511_drv.c
 * (adv7511_fixed_registers); Analog Devices' ADV7511 HDMI reference
 * design for KC705 (the 16-bit 4:2:2 input).
 */
static const struct adv7511_write adv7511_setup[] = {
	{ 0x41U, 0x10U },
	{ 0x98U, 0x03U },
	{ 0x9aU, 0xe0U },
	{ 0x9cU, 0x30U },
	{ 0x9dU, 0x61U },
	{ 0xa2U, 0xa4U },
	{ 0xa3U, 0xa4U },
	{ 0xe0U, 0xd0U },
	{ 0xf9U, 0x00U },
	{ 0xd6U, 0xc0U },
	{ 0x15U, 0x01U },
	{ 0x16U, 0x35U },
	{ 0x17U, 0x02U },
	{ 0xafU, 0x06U },
	{ 0x44U, 0x10U },
	{ 0x55U, 0x40U }
};

static int i2c_command(struct ghci_device *device, uint32_t registers, uint32_t command);
static int i2c_send(struct ghci_device *device, uint32_t registers, uint32_t byte, uint32_t command);
static int i2c_write_register(struct ghci_device *device, uint32_t registers, uint32_t address, uint32_t reg, uint32_t value);
static int i2c_read_register(struct ghci_device *device, uint32_t registers, uint32_t address, uint32_t reg, uint32_t *value);

/*
 * Finds the ADV7511 behind the display controller's I2C master and sets
 * it up.  Returns 0, ENODEV when no ADV7511 answers, or EIO when its
 * set-up failed.
 */
int
ghci_adv7511_start(
	struct ghci_device *device,
	uint32_t registers)
{
	uint32_t revision;
	unsigned index;
	int error;

	/* Opens the transmitter's channel of the switch; a board without the switch does not answer. */
	error = i2c_send(device, registers, SWITCH_ADDRESS << 1, I2C_START | I2C_WRITE);
	if (error == 0)
		error = i2c_send(device, registers, SWITCH_CHANNEL, I2C_WRITE | I2C_STOP);
	if (error != 0)
		(void)i2c_command(device, registers, I2C_STOP);

	/* The transmitter must say it is an ADV7511. */
	error = i2c_read_register(device, registers, ADV7511_ADDRESS, ADV7511_REVISION_REGISTER, &revision);
	if (error != 0)
		return ENODEV;
	if (revision != ADV7511_REVISION) {
		kern_logf("ghci: %s: HDMI transmitter revision %02x is not an ADV7511\n", device->name, (unsigned)revision);
		return ENODEV;
	}

	/* Writes the set-up in order. */
	for (index = 0U; index < sizeof(adv7511_setup) / sizeof(adv7511_setup[0]); index++) {
		error = i2c_write_register(device, registers, ADV7511_ADDRESS, adv7511_setup[index].reg, adv7511_setup[index].value);
		if (error != 0) {
			kern_logf("ghci: %s: ADV7511 register %02x not written\n", device->name, (unsigned)adv7511_setup[index].reg);
			return EIO;
		}
	}

	/* Succeeded: the transmitter takes the controller's output. */
	return 0;
}

/*
 * Reads whether the ADV7511 sees a sink on its hot-plug line.  Returns 0
 * with connected 1 or 0, or EIO when the transmitter did not answer.
 */
int
ghci_adv7511_sensed(
	struct ghci_device *device,
	uint32_t registers,
	int *connected)
{
	uint32_t status;
	int error;

	/* The status register, through the switch channel the start opened. */
	error = i2c_read_register(device, registers, ADV7511_ADDRESS, ADV7511_STATUS_REGISTER, &status);
	if (error != 0)
		return EIO;

	/* The HPD line's level. */
	*connected = 0;
	if ((status & ADV7511_STATUS_HPD) != 0U)
		*connected = 1;

	/* Succeeded: the line was read. */
	return 0;
}

/* Runs one command of the I2C master and waits for its end; EIO when the byte was not acknowledged. */
static int
i2c_command(
	struct ghci_device *device,
	uint32_t registers,
	uint32_t command)
{
	uint32_t status;
	int error;

	/* The command runs on its own; the master is busy until it ends. */
	ghci_write(device, registers + GHCI_DC_I2C_CTRL, command);
	error = ghci_wait_register(device, registers + GHCI_DC_I2C_STATUS, I2C_BUSY, 0U, I2C_BYTE_MS);
	if (error != 0)
		return EIO;

	/* A written byte nobody acknowledged ends the transaction. */
	status = ghci_read(device, registers + GHCI_DC_I2C_STATUS);
	if ((command & I2C_WRITE) != 0U && (status & I2C_NACK) != 0U)
		return EIO;

	/* Succeeded: the command ran. */
	return 0;
}

/* Sends one byte with a command (START, STOP or both may come with it). */
static int
i2c_send(
	struct ghci_device *device,
	uint32_t registers,
	uint32_t byte,
	uint32_t command)
{
	int error;

	/* The byte, then the command that sends it. */
	ghci_write(device, registers + GHCI_DC_I2C_DATA, byte & 0xffU);
	error = i2c_command(device, registers, command);
	if (error != 0)
		return error;

	/* Succeeded: the byte was acknowledged. */
	return 0;
}

/* Writes one register of an I2C device: its address, the register and the value. */
static int
i2c_write_register(
	struct ghci_device *device,
	uint32_t registers,
	uint32_t address,
	uint32_t reg,
	uint32_t value)
{
	int error;

	/* The address for a write, the register, the value and the STOP. */
	error = i2c_send(device, registers, address << 1, I2C_START | I2C_WRITE);
	if (error == 0)
		error = i2c_send(device, registers, reg, I2C_WRITE);
	if (error == 0)
		error = i2c_send(device, registers, value, I2C_WRITE | I2C_STOP);

	/* A transaction that failed still lets the bus go. */
	if (error != 0) {
		(void)i2c_command(device, registers, I2C_STOP);
		return error;
	}

	/* Succeeded: the register is written. */
	return 0;
}

/* Reads one register of an I2C device: the register is written, then read after a repeated START. */
static int
i2c_read_register(
	struct ghci_device *device,
	uint32_t registers,
	uint32_t address,
	uint32_t reg,
	uint32_t *value)
{
	int error;

	/* The address for a write and the register, then the address for a read. */
	error = i2c_send(device, registers, address << 1, I2C_START | I2C_WRITE);
	if (error == 0)
		error = i2c_send(device, registers, reg, I2C_WRITE);
	if (error == 0)
		error = i2c_send(device, registers, (address << 1) | 1U, I2C_START | I2C_WRITE);

	/* The one byte, answered with a NACK, and the STOP. */
	if (error == 0)
		error = i2c_command(device, registers, I2C_READ | I2C_STOP);

	/* A transaction that failed still lets the bus go. */
	if (error != 0) {
		(void)i2c_command(device, registers, I2C_STOP);
		return error;
	}

	/* Succeeded: the byte read. */
	*value = ghci_read(device, registers + GHCI_DC_I2C_DATA) & 0xffU;
	return 0;
}
