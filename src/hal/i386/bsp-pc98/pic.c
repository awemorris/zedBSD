/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 cascaded 8259 and memory-control implementation.
 */

#include <hal/hal.h>

#include "../asm.h"
#include "../int.h"
#include "../pic.h"

#define PIC_MASTER_PORT1 0x0000
#define PIC_MASTER_PORT2 0x0002
#define PIC_SLAVE_PORT1 0x0008
#define PIC_SLAVE_PORT2 0x000a

#define SLAVE_IRQ 7

static uint8_t master_mask;
static uint8_t slave_mask;

static void pic_update_cascade_mask(void);

/*
 * Initializes and masks both PC-98 interrupt controllers.
 */
void
pic_init(
	void)
{
	/* Programs the master controller and its IRQ7 slave cascade. */
	asm_outb(PIC_MASTER_PORT1, 0x11);
	asm_outb(PIC_MASTER_PORT2, INT_IRQ_BASE);
	asm_outb(PIC_MASTER_PORT2, 1 << SLAVE_IRQ);
	asm_outb(PIC_MASTER_PORT2, 0x01);

	/* Programs the slave controller and its master-cascade identity. */
	asm_outb(PIC_SLAVE_PORT1, 0x11);
	asm_outb(PIC_SLAVE_PORT2, INT_IRQ_BASE + 8);
	asm_outb(PIC_SLAVE_PORT2, SLAVE_IRQ);
	asm_outb(PIC_SLAVE_PORT2, 0x01);

	/* Masks every IRQ at both controllers in hardware order. */
	master_mask = 0xffU;
	slave_mask = 0xffU;
	asm_outb(PIC_MASTER_PORT2, master_mask);
	asm_outb(PIC_SLAVE_PORT2, slave_mask);
}

/*
 * Changes the mask state of one PC-98 IRQ.
 */
void
pic_set_irq_mask(
	int irq_num,
	int mask)
{
	unsigned bit;

	/* Ignores IRQ numbers outside the cascaded controllers. */
	if (irq_num < 0 || irq_num > 15)
		return;

	/* Updates a master IRQ while deriving the reserved cascade mask. */
	if (irq_num < 8) {
		/* Reserves the cascade line for slave-mask derivation. */
		if (irq_num != SLAVE_IRQ) {
			/* Applies the requested mask state to the master line. */
			if (mask) {
				master_mask |= (uint8_t)(1U << irq_num);
			} else {
				master_mask &= (uint8_t)~(1U << irq_num);
			}
		}
		pic_update_cascade_mask();
		return;
	}

	/* Updates the selected slave mask bit. */
	bit = (unsigned)irq_num - 8U;

	/* Applies the requested mask state to the slave line. */
	if (mask) {
		slave_mask |= (uint8_t)(1U << bit);
	} else {
		slave_mask &= (uint8_t)~(1U << bit);
	}

	/* Preserves cascade ordering while closing or opening slave delivery. */
	if (mask) {
		asm_outb(PIC_SLAVE_PORT2, slave_mask);
		pic_update_cascade_mask();
	} else {
		pic_update_cascade_mask();
		asm_outb(PIC_SLAVE_PORT2, slave_mask);
	}
}

/*
 * Reports the highest-priority PC-98 IRQ currently in service.
 */
int
pic_get_irq_in_service(
	void)
{
	uint8_t in_service;
	int irq_num;

	/* Reads the master controller's in-service register. */
	asm_outb(PIC_MASTER_PORT1, 0x0b);
	in_service = asm_inb(PIC_MASTER_PORT1);

	/* Finds the highest asserted master in-service bit. */
	irq_num = -1;
	while (in_service != 0) {
		irq_num++;
		in_service >>= 1;
	}

	/* Resolves the slave IRQ when the master reports its cascade. */
	if (irq_num == SLAVE_IRQ) {
		asm_outb(PIC_SLAVE_PORT1, 0x0b);
		in_service = asm_inb(PIC_SLAVE_PORT1);

		/* Finds the highest asserted slave bit in combined numbering. */
		irq_num = 7;
		while (in_service != 0) {
			irq_num++;
			in_service >>= 1;
		}
	}

	/* Returns the resolved in-service IRQ number. */
	return irq_num;
}

/*
 * Completes one PC-98 interrupt at the cascaded controllers.
 */
void
pic_send_eoi(
	int irq_num)
{
	uint8_t in_service;

	/* Completes a master IRQ directly or a slave IRQ through the cascade. */
	if (irq_num <= 7) {
		asm_outb(PIC_MASTER_PORT1, 0x20);
	} else {
		/* Completes the slave IRQ before examining remaining slave work. */
		asm_outb(PIC_SLAVE_PORT1, 0x20);
		asm_outb(PIC_SLAVE_PORT1, 0x0b);
		in_service = asm_inb(PIC_SLAVE_PORT1);

		/* Completes the master cascade only after the final slave IRQ. */
		if (in_service == 0)
			asm_outb(PIC_MASTER_PORT1, 0x20);
	}
}

/*
 * Reports the PC-98 physical-memory size from the BIOS work area.
 */
uint32_t
bsp_mem_probe(
	void)
{
	uint32_t low_extended;
	uint32_t high;

	/* Reads memory below and above 16 MiB in BIOS-defined units. */
	low_extended =
	    (uint32_t)(*(volatile uint8_t *)(SYS_START + 0x401)) << 17;
	high = (uint32_t)(*(volatile uint16_t *)(SYS_START + 0x594)) << 20;

	/* Returns conventional memory plus both extended-memory regions. */
	return 0x100000 + low_extended + high;
}

/*
 * Reports the raw PC-98 extended-memory segments.
 */
void
hal_pc98_memory_segments(
	uint32_t *low_extended,
	uint32_t *high_mib)
{
	/* Samples the below-16-MiB BIOS field when requested. */
	if (low_extended != NULL) {
		*low_extended =
		    (uint32_t)(*(volatile uint8_t *)(SYS_START + 0x401)) << 17;
	}

	/* Samples the above-16-MiB BIOS field when requested. */
	if (high_mib != NULL) {
		*high_mib =
		    (uint32_t)(*(volatile uint16_t *)(SYS_START + 0x594));
	}
}

/*
 * Enables the PC-98 high-memory window.
 */
void
hal_pc98_enable_high_memory(
	void)
{
	uint8_t value;

	/* Programs the high-memory control sequence in board-defined order. */
	asm_outb(0x00f2, 0x00);
	asm_outb(0x00f6, 0x02);
	value = asm_inb(0x0439);
	asm_outb(0x0439, (uint8_t)(value & 0xfb));
	asm_outb(0x00f8, 0x00);
	asm_outb(0x043b, 0x04);
}

/* Derives and writes the master cascade mask from the slave state. */
static void
pic_update_cascade_mask(
	void)
{
	/* Closes the cascade only while every slave IRQ is masked. */
	if (slave_mask == 0xffU) {
		master_mask |= (uint8_t)(1U << SLAVE_IRQ);
	} else {
		master_mask &= (uint8_t)~(1U << SLAVE_IRQ);
	}

	/* Publishes the derived master mask. */
	asm_outb(PIC_MASTER_PORT2, master_mask);
}
