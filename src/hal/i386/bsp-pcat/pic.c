/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT cascaded 8259 interrupt-controller implementation.
 */

#include "../asm.h"
#include "../int.h"
#include "../pic.h"

#define MASTER_COMMAND 0x20U
#define MASTER_DATA 0x21U
#define SLAVE_COMMAND 0xa0U
#define SLAVE_DATA 0xa1U

static uint8_t master_mask;
static uint8_t slave_mask;

static int highest(uint8_t value);

/*
 * Initializes and masks both PC/AT interrupt controllers.
 */
void
pic_init(
	void)
{
	/* Programs the master controller and its slave-cascade input. */
	asm_outb(MASTER_COMMAND, 0x11U);
	asm_outb(MASTER_DATA, INT_IRQ_BASE);
	asm_outb(MASTER_DATA, 1U << 2);
	asm_outb(MASTER_DATA, 0x01U);

	/* Programs the slave controller and its master-cascade identity. */
	asm_outb(SLAVE_COMMAND, 0x11U);
	asm_outb(SLAVE_DATA, INT_IRQ_BASE + 8U);
	asm_outb(SLAVE_DATA, 2U);
	asm_outb(SLAVE_DATA, 0x01U);

	/* Masks every IRQ at both controllers in hardware order. */
	slave_mask = 0xffU;
	master_mask = 0xffU;
	asm_outb(MASTER_DATA, master_mask);
	asm_outb(SLAVE_DATA, slave_mask);
}

/*
 * Changes the mask state of one PC/AT IRQ.
 */
void
pic_set_irq_mask(
	int irq,
	int masked)
{
	unsigned bit;

	/* Ignores IRQ numbers outside the cascaded controllers. */
	if (irq < 0 || irq > 15)
		return;

	/* Updates the owning controller and the slave cascade when required. */
	if (irq < 8) {
		/* Applies the requested mask bit to the master controller. */
		if (masked) {
			master_mask |= (uint8_t)(1U << irq);
		} else {
			master_mask &= (uint8_t)~(1U << irq);
		}
		asm_outb(MASTER_DATA, master_mask);
	} else {
		bit = (unsigned)irq - 8U;

		/* Masks the slave bit or unmasks it with the master cascade. */
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
 * Reports the highest-priority PC/AT IRQ currently in service.
 */
int
pic_get_irq_in_service(
	void)
{
	uint8_t in_service;
	int master;
	int slave;

	/* Reads the master controller's in-service register. */
	asm_outb(MASTER_COMMAND, 0x0bU);
	in_service = asm_inb(MASTER_COMMAND);
	master = highest(in_service);

	/* Returns a non-cascade master IRQ directly. */
	if (master != 2)
		return master;

	/* Reads the slave controller's in-service register. */
	asm_outb(SLAVE_COMMAND, 0x0bU);
	in_service = asm_inb(SLAVE_COMMAND);
	slave = highest(in_service);

	/* Treats an empty slave register as the master cascade IRQ. */
	if (slave < 0)
		return 2;

	/* Returns the slave IRQ in the combined numbering space. */
	return slave + 8;
}

/*
 * Completes one PC/AT interrupt at the cascaded controllers.
 */
void
pic_send_eoi(
	int irq)
{
	/* Completes a slave IRQ before completing its master cascade. */
	if (irq >= 8)
		asm_outb(SLAVE_COMMAND, 0x20U);
	asm_outb(MASTER_COMMAND, 0x20U);
}

/* Finds the highest set bit in one in-service register. */
static int
highest(
	uint8_t value)
{
	int result;

	/* Counts right shifts through the highest asserted bit. */
	result = -1;
	while (value != 0) {
		result++;
		value >>= 1;
	}

	/* Returns the bit index or negative one for an empty register. */
	return result;
}
