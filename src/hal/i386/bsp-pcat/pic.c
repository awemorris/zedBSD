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

#define CASCADE_IRQ 2U

/* The 8259 command that reads the in-service register next. */
#define PIC_READ_ISR 0x0bU

/* The 8259 specific end-of-interrupt command. */
#define PIC_SPECIFIC_EOI 0x60U

/* The mask each controller holds, written under disabled interrupts. */
static uint8_t master_mask;
static uint8_t slave_mask;

static uint8_t read_in_service(uint16_t command_port);

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
	asm_outb(MASTER_DATA, 1U << CASCADE_IRQ);
	asm_outb(MASTER_DATA, 0x01U);

	/* Programs the slave controller and its master-cascade identity. */
	asm_outb(SLAVE_COMMAND, 0x11U);
	asm_outb(SLAVE_DATA, INT_IRQ_BASE + 8U);
	asm_outb(SLAVE_DATA, CASCADE_IRQ);
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
			master_mask &= (uint8_t)~(1U << CASCADE_IRQ);
			asm_outb(MASTER_DATA, master_mask);
		}
		asm_outb(SLAVE_DATA, slave_mask);
	}
}

/*
 * Reports whether one PC/AT IRQ is in service at its controller.
 *
 * A delivery whose line is not in service is a spurious interrupt: the
 * request went away before the acknowledge cycle, and the controller
 * answered with its lowest-priority vector without marking it.
 */
int
pic_irq_in_service(
	int irq)
{
	uint8_t in_service;

	/* Ignores IRQ numbers outside the cascaded controllers. */
	if (irq < 0 || irq > 15)
		return 0;

	/* Reads the register of the controller that owns the line. */
	if (irq < 8) {
		in_service = read_in_service(MASTER_COMMAND);
	} else {
		in_service = read_in_service(SLAVE_COMMAND);
	}

	/* Reports the line's in-service bit. */
	if ((in_service & (1U << ((unsigned)irq & 7U))) != 0)
		return 1;

	/* Reports a line the controller did not mark. */
	return 0;
}

/*
 * Completes the cascade of a spurious PC/AT delivery.
 *
 * A spurious slave interrupt still marks the cascade line in service at
 * the master, and that mark is cleared once no slave IRQ remains in
 * service.  A spurious master interrupt marks nothing.
 */
void
pic_complete_spurious(
	int irq)
{
	uint8_t master_in_service;
	uint8_t slave_in_service;

	/* Leaves the master's own spurious deliveries alone. */
	if (irq < 8)
		return;

	/* Samples both controllers to see whether the cascade stays busy. */
	master_in_service = read_in_service(MASTER_COMMAND);
	slave_in_service = read_in_service(SLAVE_COMMAND);

	/*
	 * Clears only the cascade mark, by a specific end of interrupt, so a
	 * master IRQ nested under the cascade stays in service.
	 */
	if ((master_in_service & (1U << CASCADE_IRQ)) != 0 &&
	    slave_in_service == 0)
		asm_outb(MASTER_COMMAND, PIC_SPECIFIC_EOI | CASCADE_IRQ);
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

/* Reads one controller's in-service register. */
static uint8_t
read_in_service(
	uint16_t command_port)
{
	uint8_t in_service;

	/* Selects the in-service register and reads it back. */
	asm_outb(command_port, PIC_READ_ISR);
	in_service = asm_inb(command_port);

	/* Returns the controller's in-service bits. */
	return in_service;
}
