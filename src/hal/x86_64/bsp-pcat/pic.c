/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 PC/AT legacy 8259 interrupt-controller implementation.
 */

#include "../asm.h"
#include "../int.h"
#include "../pic.h"

#define MASTER_COMMAND 0x20U
#define MASTER_DATA    0x21U
#define SLAVE_COMMAND  0xa0U
#define SLAVE_DATA     0xa1U

static uint8_t master_mask;
static uint8_t slave_mask;

static int highest(uint8_t value);

/*
 * Initializes and fully masks both legacy interrupt controllers.
 */
void
pic_init(
	void)
{
	/* Programs the master controller and its cascade line. */
	asm_outb(MASTER_COMMAND, 0x11U);
	asm_outb(MASTER_DATA, INT_IRQ_BASE);
	asm_outb(MASTER_DATA, 1U << 2);
	asm_outb(MASTER_DATA, 0x01U);

	/* Programs the slave controller and its cascade identity. */
	asm_outb(SLAVE_COMMAND, 0x11U);
	asm_outb(SLAVE_DATA, INT_IRQ_BASE + 8U);
	asm_outb(SLAVE_DATA, 2U);
	asm_outb(SLAVE_DATA, 0x01U);

	/* Masks every legacy line before I/O APIC routing begins. */
	master_mask = slave_mask = 0xffU;
	asm_outb(MASTER_DATA, master_mask);
	asm_outb(SLAVE_DATA, slave_mask);
}

/*
 * Changes the mask state of one legacy IRQ line.
 */
void
pic_set_irq_mask(
	int irq,
	int masked)
{
	unsigned bit;

	/* Ignores values outside the paired-controller range. */
	if (irq < 0 || irq > 15)
		return;

	/* Updates the owning controller without reordering port writes. */
	if (irq < 8) {
		/* Applies the requested mask state to the master line. */
		if (masked)
			master_mask |= (uint8_t)(1U << irq);
		else
			master_mask &= (uint8_t)~(1U << irq);
		asm_outb(MASTER_DATA, master_mask);
	} else {
		bit = (unsigned)irq - 8U;

		/* Applies the requested mask state to the slave line. */
		if (masked) {
			slave_mask |= (uint8_t)(1U << bit);
		} else {
			slave_mask &= (uint8_t)~(1U << bit);
			master_mask &= (uint8_t)~(1U << 2);
			asm_outb(MASTER_DATA, master_mask);
		}
		asm_outb(SLAVE_DATA, slave_mask);
	}
}

/*
 * Reports the highest-priority legacy IRQ in service.
 */
int
pic_get_irq_in_service(
	void)
{
	uint8_t value;
	int master;
	int slave;

	/* Reads the master's in-service register. */
	asm_outb(MASTER_COMMAND, 0x0bU);
	value = asm_inb(MASTER_COMMAND);
	master = highest(value);

	/* Returns a non-cascade master interrupt directly. */
	if (master != 2)
		return master;

	/* Resolves the slave request behind the cascade line. */
	asm_outb(SLAVE_COMMAND, 0x0bU);
	value = asm_inb(SLAVE_COMMAND);
	slave = highest(value);

	/* Reports the cascade line when no slave bit is active. */
	if (slave < 0)
		return 2;

	/* Returns the slave IRQ in the combined numbering space. */
	return slave + 8;
}

/*
 * Acknowledges one legacy IRQ at the 8259 pair.
 */
void
pic_send_eoi(
	int irq)
{
	/* Acknowledges the slave before its master cascade. */
	if (irq >= 8)
		asm_outb(SLAVE_COMMAND, 0x20U);

	/* Always acknowledges the master controller. */
	asm_outb(MASTER_COMMAND, 0x20U);
}

/* Finds the most significant active bit in one controller byte. */
static int
highest(
	uint8_t value)
{
	int result;

	/* Counts shifts until the highest active bit is consumed. */
	result = -1;
	while (value != 0) {
		result++;
		value >>= 1;
	}

	/* Returns minus one when no bit was active. */
	return result;
}
