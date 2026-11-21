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

/*
 * Initialization command word 4 of each controller.  The master runs in
 * special fully nested, buffered master and 8086 mode, and the slave in
 * buffered slave and 8086 mode.  PC-98 boards are wired for buffered
 * mode, in which the SP/EN pin enables the data-bus buffer and this word,
 * not the pin, says which controller is the master.
 *
 * Values referenced: FreeBSD 11.4 sys/x86/isa/icu.h (BASE_MASTER_MODE,
 * BASE_SLAVE_MODE under PC98); Linux 2.6.7 PC-9800
 * include/asm-i386/mach-pc9800/io_ports.h (MASTER_ICW4_DEFAULT,
 * SLAVE_ICW4_DEFAULT).
 */
#define PIC_MASTER_ICW4 0x1d
#define PIC_SLAVE_ICW4 0x09

/* The 8259 command that reads the in-service register next. */
#define PIC_READ_ISR 0x0b

/* The 8259 non-specific and specific end-of-interrupt commands. */
#define PIC_EOI 0x20
#define PIC_SPECIFIC_EOI 0x60

/*
 * A write to this port takes at least 0.6 microseconds, which spaces
 * consecutive commands to one controller.
 *
 * Value referenced: Linux 2.6.7 PC-9800
 * include/asm-i386/mach-pc9800/mach_time.h (the 0x5f delay port).
 */
#define PC98_WAIT_PORT 0x005f

/* The mask each controller holds, written under disabled interrupts. */
static uint8_t master_mask;
static uint8_t slave_mask;

static void pic_write(uint16_t port, uint8_t value);
static uint8_t pic_read_in_service(uint16_t port);
static void pic_update_cascade_mask(void);

/*
 * Initializes and masks both PC-98 interrupt controllers.
 */
void
pic_init(
	void)
{
	/* Programs the master controller and its IRQ7 slave cascade. */
	pic_write(PIC_MASTER_PORT1, 0x11);
	pic_write(PIC_MASTER_PORT2, INT_IRQ_BASE);
	pic_write(PIC_MASTER_PORT2, 1 << SLAVE_IRQ);
	pic_write(PIC_MASTER_PORT2, PIC_MASTER_ICW4);

	/* Programs the slave controller and its master-cascade identity. */
	pic_write(PIC_SLAVE_PORT1, 0x11);
	pic_write(PIC_SLAVE_PORT2, INT_IRQ_BASE + 8);
	pic_write(PIC_SLAVE_PORT2, SLAVE_IRQ);
	pic_write(PIC_SLAVE_PORT2, PIC_SLAVE_ICW4);

	/* Masks every IRQ at both controllers in hardware order. */
	master_mask = 0xffU;
	slave_mask = 0xffU;
	pic_write(PIC_MASTER_PORT2, master_mask);
	pic_write(PIC_SLAVE_PORT2, slave_mask);
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
 * Reports whether one PC-98 IRQ is in service at its controller.
 *
 * A delivery whose line is not in service is a spurious interrupt: the
 * request went away before the acknowledge cycle, and the controller
 * answered with its lowest-priority vector without marking it.
 */
int
pic_irq_in_service(
	int irq_num)
{
	uint8_t in_service;

	/* Ignores IRQ numbers outside the cascaded controllers. */
	if (irq_num < 0 || irq_num > 15)
		return 0;

	/* Reads the register of the controller that owns the line. */
	if (irq_num < 8) {
		in_service = pic_read_in_service(PIC_MASTER_PORT1);
	} else {
		in_service = pic_read_in_service(PIC_SLAVE_PORT1);
	}

	/* Reports the line's in-service bit. */
	if ((in_service & (1U << (irq_num & 7))) != 0)
		return 1;

	/* Reports a line the controller did not mark. */
	return 0;
}

/*
 * Completes the cascade of a spurious PC-98 delivery.
 *
 * A spurious slave interrupt still marks the cascade line in service at
 * the master when the master acknowledged it, and that mark is cleared
 * once no slave IRQ remains in service.  A spurious master interrupt
 * marks nothing and needs no command.
 */
void
pic_complete_spurious(
	int irq_num)
{
	uint8_t master_in_service;
	uint8_t slave_in_service;

	/* Leaves the master's own spurious deliveries alone. */
	if (irq_num < 8)
		return;

	/* Samples both controllers to see whether the cascade stays busy. */
	master_in_service = pic_read_in_service(PIC_MASTER_PORT1);
	slave_in_service = pic_read_in_service(PIC_SLAVE_PORT1);

	/*
	 * Clears only the cascade mark, by a specific end of interrupt, so a
	 * master IRQ nested under the cascade stays in service.
	 */
	if ((master_in_service & (1U << SLAVE_IRQ)) != 0 &&
	    slave_in_service == 0)
		pic_write(PIC_MASTER_PORT1, PIC_SPECIFIC_EOI | SLAVE_IRQ);
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
		asm_outb(PIC_MASTER_PORT1, PIC_EOI);
	} else {
		/* Completes the slave IRQ before examining remaining slave work. */
		asm_outb(PIC_SLAVE_PORT1, PIC_EOI);
		in_service = pic_read_in_service(PIC_SLAVE_PORT1);

		/* Completes the master cascade only after the final slave IRQ. */
		if (in_service == 0)
			asm_outb(PIC_MASTER_PORT1, PIC_EOI);
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

/* Writes one controller register and waits out its recovery time. */
static void
pic_write(
	uint16_t port,
	uint8_t value)
{
	/* Issues the command, then spaces it from the next one. */
	asm_outb(port, value);
	asm_outb(PC98_WAIT_PORT, 0);
}

/* Reads one controller's in-service register. */
static uint8_t
pic_read_in_service(
	uint16_t port)
{
	uint8_t in_service;

	/* Selects the in-service register and reads it back. */
	asm_outb(port, PIC_READ_ISR);
	in_service = asm_inb(port);

	/* Returns the controller's in-service bits. */
	return in_service;
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
