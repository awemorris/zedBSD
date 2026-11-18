/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VisionFive 2's HDMI transmitter (an Innosilicon one), set to a mode
 * by the display driver when it lights the output itself.
 *
 * The transmitter's registers are bytes, each in its own 32-bit word.
 * Its PHY makes the pixel clock the display controller's HDMI panel runs
 * on, so it is set before the panel is started.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/display/dc8200-hdmi.h"
#include <kern/clock.h>
#include <kern/device-io.h>
#include <uapi/errno.h>

/*
 * The transmitter's registers outside the PLLs and the timing: the system
 * control with its power-down bit, the PHY's power word and its two
 * values, the TMDS driver, the PLL bias and the lock flags of the two
 * PLLs, the LDO, the serializer, and the PHY's sync that latches the
 * setting.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.h (HDMI_SYS_CTRL 0x00, its power
 * bit 1 set for low power; HDMI_PHY_SYNC 0xce) and inno_hdmi.c
 * (inno_hdmi_setup: 0x1b0 bit 2 and 0x1cc 0x0f before the PLLs, the
 * locks at 0x1a9 and 0x1af bit 0, 0x1b4 0x07, 0x1be 0x71; the PHY's
 * power word 0x00 written 0x63 and then 0x61; 0x1b2 0x8f; 0xce written 0
 * and then 1).
 */
#define HDMI_SYSTEM_CONTROL	0x00U
#define HDMI_SYSTEM_LOW_POWER	(1U << 1)
#define HDMI_PHY_DOWN		0x63U
#define HDMI_PHY_UP		0x61U
#define HDMI_PLL_BIAS		0x1b0U
#define HDMI_PLL_BIAS_ON	(1U << 2)
#define HDMI_PLL_CONTROL	0x1ccU
#define HDMI_PLL_CONTROL_VALUE	0x0fU
#define HDMI_PRE_PLL_LOCK	0x1a9U
#define HDMI_POST_PLL_LOCK	0x1afU
#define HDMI_PLL_LOCKED		(1U << 0)
#define HDMI_LDO		0x1b4U
#define HDMI_LDO_ON		0x07U
#define HDMI_SERIALIZER		0x1beU
#define HDMI_SERIALIZER_ON	0x71U
#define HDMI_TMDS_DRIVER	0x1b2U
#define HDMI_TMDS_DRIVER_ON	0x8fU
#define HDMI_PHY_SYNC		0xceU

/* The bytes' spacing: each register is a 32-bit word. */
#define HDMI_REGISTER_STEP	4U

/* How long each PLL may take to lock, in steps of a millisecond. */
#define HDMI_LOCK_TRIES		20U

static uint32_t read_byte(volatile uint8_t *registers, uint32_t reg);
static void write_byte(volatile uint8_t *registers, uint32_t reg, uint32_t value);
static void write_list(volatile uint8_t *registers, const struct powervr_dc_hdmi_write *writes, unsigned count);
static int wait_lock(volatile uint8_t *registers, uint32_t reg);

/*
 * Sets the transmitter's PLLs, timing and drive for a mode and starts its
 * output.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.c (inno_hdmi_setup: the bias,
 * the PLLs and their locks, the LDO and serializer, the drive, the PHY
 * down while the timing is written and up after, the TMDS driver, the
 * PHY's sync).
 */
int
powervr_dc_hdmi_start(
	volatile uint8_t *registers,
	const struct powervr_dc_mode *mode)
{
	struct powervr_dc_hdmi_write writes[POWERVR_DC_HDMI_WRITES_MAX];
	struct powervr_dc_pll pll;
	unsigned count;
	uint32_t value;
	int locked;
	int error;

	/* Refuses a clock the PLL table lacks. */
	error = powervr_dc_pll_find(mode->clock_khz, &pll);
	if (error != 0)
		return error;

	/* Leaves low power. */
	value = read_byte(registers, HDMI_SYSTEM_CONTROL);
	write_byte(registers, HDMI_SYSTEM_CONTROL, value & ~HDMI_SYSTEM_LOW_POWER);

	/* Turns the PLLs' bias on. */
	value = read_byte(registers, HDMI_PLL_BIAS);
	write_byte(registers, HDMI_PLL_BIAS, value | HDMI_PLL_BIAS_ON);
	write_byte(registers, HDMI_PLL_CONTROL, HDMI_PLL_CONTROL_VALUE);

	/* Sets both PLLs for the pixel clock. */
	count = powervr_dc_hdmi_pll_writes(&pll, writes);
	write_list(registers, writes, count);

	/* Waits for both PLLs to lock; a late lock is reported but the output is still started. */
	locked = 1;
	error = wait_lock(registers, HDMI_PRE_PLL_LOCK);
	if (error != 0)
		locked = 0;
	error = wait_lock(registers, HDMI_POST_PLL_LOCK);
	if (error != 0)
		locked = 0;

	/* Turns the LDO and the serializer on, and sets the drive the mode asks for. */
	write_byte(registers, HDMI_LDO, HDMI_LDO_ON);
	write_byte(registers, HDMI_SERIALIZER, HDMI_SERIALIZER_ON);
	count = powervr_dc_hdmi_drive_writes(mode, writes);
	write_list(registers, writes, count);

	/* Writes the timing with the PHY down. */
	write_byte(registers, HDMI_SYSTEM_CONTROL, HDMI_PHY_DOWN);
	count = powervr_dc_hdmi_timing_writes(mode, writes);
	write_list(registers, writes, count);

	/* Brings the PHY up, starts the TMDS driver and latches the setting. */
	write_byte(registers, HDMI_SYSTEM_CONTROL, HDMI_PHY_UP);
	write_byte(registers, HDMI_TMDS_DRIVER, HDMI_TMDS_DRIVER_ON);
	write_byte(registers, HDMI_PHY_SYNC, 0U);
	write_byte(registers, HDMI_PHY_SYNC, 1U);

	if (!locked)
		return ETIMEDOUT;

	/* Succeeded: the output runs at the mode. */
	return 0;
}

/* Reads one of the transmitter's byte registers. */
static uint32_t
read_byte(
	volatile uint8_t *registers,
	uint32_t reg)
{
	/* Each byte register sits in the low byte of its own word. */
	return kern_mmio_read32(registers + reg * HDMI_REGISTER_STEP) & 0xffU;
}

/* Writes one of the transmitter's byte registers. */
static void
write_byte(
	volatile uint8_t *registers,
	uint32_t reg,
	uint32_t value)
{
	/* Each byte register sits in the low byte of its own word. */
	kern_mmio_write32(registers + reg * HDMI_REGISTER_STEP, value & 0xffU);
}

/* Writes a list of byte registers in its order. */
static void
write_list(
	volatile uint8_t *registers,
	const struct powervr_dc_hdmi_write *writes,
	unsigned count)
{
	unsigned index;

	/* One write after another. */
	for (index = 0; index < count; index++)
		write_byte(registers, writes[index].reg, writes[index].value);
}

/* Waits a bounded time for a PLL's lock flag; ETIMEDOUT when it stays clear. */
static int
wait_lock(
	volatile uint8_t *registers,
	uint32_t reg)
{
	unsigned tries;
	uint32_t value;

	/* Reads the flag about once a millisecond. */
	for (tries = 0; tries < HDMI_LOCK_TRIES; tries++) {
		value = read_byte(registers, reg);
		if ((value & HDMI_PLL_LOCKED) != 0)
			return 0;
		kern_usleep_range(1000U, 1200U);
	}

	/* The PLL did not lock in time. */
	return ETIMEDOUT;
}
