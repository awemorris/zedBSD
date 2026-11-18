/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board's external interrupts: the pair of 8259 controllers in
 * the PIIX4, whose output is the processor's interrupt line 2.
 *
 * The number of a pending interrupt is taken with the controllers' poll
 * command, which also acknowledges it, rather than through the system
 * controller's acknowledge cycle.  Every line starts masked but the
 * master's cascade input; a registered handler unmasks its line.  A
 * level-triggered line (the PCI interrupts routed through the PIIX4) is
 * marked in the edge/level control registers when it is registered.
 * The processor's interrupt line 7 is coprocessor 0's timer, which
 * timer.c serves.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../bsp.h"
#include "../io.h"
#include "../spin.h"
#include "timer.h"

/*
 * The physical address of the PCI I/O window, where ISA port n is at
 * the window's base plus n.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (the PCI0 I/O window
 * the boot loader sets at 0x18000000) and hw/pci-host/gt64120.c (the
 * window's decoding).
 */
#define IRQ_ISA_IO_BASE		0x18000000U

/*
 * The ports of the master and the slave controller (command, then data),
 * and the two edge/level control registers.
 *
 * Values referenced: Intel 82371AB PIIX4 datasheet (the 8259 pair at
 * 0x20-0x21 and 0xa0-0xa1, ELCR1 at 0x4d0 and ELCR2 at 0x4d1).
 */
#define IRQ_MASTER_COMMAND	0x20U
#define IRQ_MASTER_DATA		0x21U
#define IRQ_SLAVE_COMMAND	0xa0U
#define IRQ_SLAVE_DATA		0xa1U
#define IRQ_MASTER_LEVEL	0x4d0U
#define IRQ_SLAVE_LEVEL		0x4d1U

/*
 * The initialization words and the operation commands the board sends:
 * ICW1 (edge, cascaded, ICW4 follows), the vector bases of ICW2 (unused,
 * since the number comes from the poll), ICW3 (the slave on the master's
 * input 2), ICW4 (8086 mode, normal end of interrupt), OCW3's poll
 * command and OCW2's specific end of interrupt.  A poll answer has bit 7
 * set when an interrupt was pending and its input in the low three bits.
 *
 * Values referenced: Intel 8259A Programmable Interrupt Controller data
 * sheet (ICW1-ICW4, OCW2, OCW3 and the poll word); QEMU v10.0.11
 * hw/intc/i8259.c (pic_ioport_write, pic_ioport_read).
 */
#define IRQ_ICW1		0x11U
#define IRQ_ICW2_MASTER		0x08U
#define IRQ_ICW2_SLAVE		0x70U
#define IRQ_ICW3_MASTER		0x04U
#define IRQ_ICW3_SLAVE		0x02U
#define IRQ_ICW4		0x01U
#define IRQ_OCW3_POLL		0x0cU
#define IRQ_OCW2_SPECIFIC_EOI	0x60U
#define IRQ_POLL_PENDING	0x80U
#define IRQ_POLL_INPUT_MASK	0x07U

/* The master's input the slave is cascaded on, and the inputs of one controller. */
#define IRQ_CASCADE_INPUT	2U
#define IRQ_INPUTS		8U

/* The lines of the pair: the master's inputs 0-7, then the slave's as 8-15. */
#define IRQ_LINES		16U

/*
 * One line's handler, as registered.
 *
 * An entry with no handler is a line nothing serves, and stays masked.
 */
struct irq_source {
	void (*handler)(void *argument);
	void *argument;
};

/*
 * The handlers of the sixteen lines.  Written under irq_lock with
 * interrupts off; read by the interrupt with interrupts off.
 */
static struct irq_source irq_sources[IRQ_LINES];

/*
 * The controllers' masks as last written: a set bit is a masked line.
 * Kept under irq_lock.  The master's cascade input is always open.
 */
static uint8_t irq_master_mask;
static uint8_t irq_slave_mask;

/* Guards irq_sources and the two masks; zero when free. */
static volatile unsigned irq_lock;

/*
 * The interrupts whose poll found nothing, or a line with no handler,
 * since boot.  Kept for the debugger; it only grows.
 */
static volatile uint32_t irq_spurious;

static void port_write(uint32_t port, uint8_t value);
static uint8_t port_read(uint32_t port);
static unsigned poll_controller(uint32_t command_port, bool *found);
static void end_interrupt(unsigned line);
static void serve_controllers(void);

/*
 * Initializes both controllers with every line masked but the cascade.
 */
void
mips_board_irq_init(
	void)
{
	/* The master: edge triggered, cascaded, vector base, the slave on input 2, 8086 mode. */
	port_write(IRQ_MASTER_COMMAND, IRQ_ICW1);
	port_write(IRQ_MASTER_DATA, IRQ_ICW2_MASTER);
	port_write(IRQ_MASTER_DATA, IRQ_ICW3_MASTER);
	port_write(IRQ_MASTER_DATA, IRQ_ICW4);

	/* The slave: the same, cascaded on the master's input 2. */
	port_write(IRQ_SLAVE_COMMAND, IRQ_ICW1);
	port_write(IRQ_SLAVE_DATA, IRQ_ICW2_SLAVE);
	port_write(IRQ_SLAVE_DATA, IRQ_ICW3_SLAVE);
	port_write(IRQ_SLAVE_DATA, IRQ_ICW4);

	/* Masks every line; only the cascade stays open, for the slave's lines later. */
	irq_master_mask = (uint8_t)(0xffU & ~(1U << IRQ_CASCADE_INPUT));
	irq_slave_mask = 0xffU;
	port_write(IRQ_MASTER_DATA, irq_master_mask);
	port_write(IRQ_SLAVE_DATA, irq_slave_mask);

	/* Reports the masks the pair starts with. */
	hal_printf(MIPS_HAL_NAME " MALTA PIC 8259 mask=%02x%02x\n", irq_slave_mask, irq_master_mask);
}

/*
 * Registers the handler of a line and unmasks it.
 *
 * Returns -1 for the cascade input, a line out of range, no handler or a
 * line that already has one.
 */
int
mips_board_irq_register(
	unsigned line,
	bool level,
	void (*handler)(void *),
	void *argument)
{
	uint8_t triggers;
	bool enabled;

	/* Refuses what cannot be a line's handler. */
	if (line >= IRQ_LINES)
		return -1;
	if (line == IRQ_CASCADE_INPUT)
		return -1;
	if (handler == NULL)
		return -1;

	/* Records the handler, sets the line's trigger and lets it in. */
	enabled = mips_spin_lock(&irq_lock);

	/* Refuses a line that already has a handler. */
	if (irq_sources[line].handler != NULL) {
		mips_spin_unlock(&irq_lock, enabled);

		return -1;
	}

	/* Records the handler the interrupt path calls. */
	irq_sources[line].handler = handler;
	irq_sources[line].argument = argument;

	/* Sets the trigger and unmasks the line on the controller that owns it. */
	if (line < IRQ_INPUTS) {
		/* A level-triggered line is marked in the master's trigger register. */
		if (level) {
			triggers = port_read(IRQ_MASTER_LEVEL);
			port_write(IRQ_MASTER_LEVEL, (uint8_t)(triggers | (1U << line)));
		}

		/* Lets the line in at the master. */
		irq_master_mask = (uint8_t)(irq_master_mask & ~(1U << line));
		port_write(IRQ_MASTER_DATA, irq_master_mask);
	} else {
		/* A level-triggered line is marked in the slave's trigger register. */
		if (level) {
			triggers = port_read(IRQ_SLAVE_LEVEL);
			port_write(IRQ_SLAVE_LEVEL, (uint8_t)(triggers | (1U << (line - IRQ_INPUTS))));
		}

		/* Lets the line in at the slave. */
		irq_slave_mask = (uint8_t)(irq_slave_mask & ~(1U << (line - IRQ_INPUTS)));
		port_write(IRQ_SLAVE_DATA, irq_slave_mask);
	}

	mips_spin_unlock(&irq_lock, enabled);

	/* Succeeded: the line is served. */
	return 0;
}

/*
 * Serves the processor's interrupt lines the board wires: the timer of
 * coprocessor 0 and the pair of controllers.  pending holds the lines of
 * Cause.IP that are pending and let in.
 *
 * Reports whether any of them was the board's.  Called by the trap
 * handling with interrupts off.
 */
bool
mips_board_interrupt(
	uint32_t pending)
{
	bool served;

	/* The tick of coprocessor 0. */
	served = false;
	if ((pending & MIPS_INTERRUPT_TIMER) != 0) {
		malta_timer_interrupt();
		served = true;
	}

	/* The pair of controllers. */
	if ((pending & MIPS_INTERRUPT_BOARD) != 0) {
		serve_controllers();
		served = true;
	}

	/* Succeeded: whether a line was the board's. */
	return served;
}

/*
 * Reports the lines of Status.IM the board's interrupts come on: the
 * timer's and the controllers'.
 */
uint32_t
mips_board_interrupt_mask(
	void)
{
	/* Succeeded: the two lines. */
	return MIPS_INTERRUPT_TIMER | MIPS_INTERRUPT_BOARD;
}

/* Writes an ISA port of the PIIX4. */
static void
port_write(
	uint32_t port,
	uint8_t value)
{
	/* The port through the PCI I/O window. */
	mips_io_write8(IRQ_ISA_IO_BASE + port, value);
}

/* Reads an ISA port of the PIIX4. */
static uint8_t
port_read(
	uint32_t port)
{
	uint8_t value;

	/* The port through the PCI I/O window. */
	value = mips_io_read8(IRQ_ISA_IO_BASE + port);

	/* Succeeded: the port's value. */
	return value;
}

/* Polls one controller, which acknowledges its highest pending input. */
static unsigned
poll_controller(
	uint32_t command_port,
	bool *found)
{
	uint8_t answer;

	/* The poll command makes the next read the poll word. */
	port_write(command_port, IRQ_OCW3_POLL);
	answer = port_read(command_port);

	/* Reports that nothing was pending. */
	if ((answer & IRQ_POLL_PENDING) == 0) {
		*found = false;
		return 0;
	}

	/* Succeeded: the controller's pending input. */
	*found = true;
	return answer & IRQ_POLL_INPUT_MASK;
}

/* Ends the interrupt of a line on its controller and, for a slave line, the cascade. */
static void
end_interrupt(
	unsigned line)
{
	/* A slave line is in service on the slave and, through the cascade, on the master. */
	if (line >= IRQ_INPUTS) {
		port_write(IRQ_SLAVE_COMMAND, (uint8_t)(IRQ_OCW2_SPECIFIC_EOI | (line - IRQ_INPUTS)));
		port_write(IRQ_MASTER_COMMAND, (uint8_t)(IRQ_OCW2_SPECIFIC_EOI | IRQ_CASCADE_INPUT));
	} else {
		port_write(IRQ_MASTER_COMMAND, (uint8_t)(IRQ_OCW2_SPECIFIC_EOI | line));
	}
}

/*
 * Serves the pair of controllers: polls them for the pending line, runs
 * its handler and ends the interrupt.
 */
static void
serve_controllers(
	void)
{
	unsigned line;
	bool found;

	/* The master's pending input; the cascade means the slave holds the line. */
	line = poll_controller(IRQ_MASTER_COMMAND, &found);
	if (!found) {
		irq_spurious++;
		return;
	}

	/* The cascade input means the slave holds the line: polls it there. */
	if (line == IRQ_CASCADE_INPUT) {
		line = poll_controller(IRQ_SLAVE_COMMAND, &found);
		if (!found) {
			irq_spurious++;
			end_interrupt(IRQ_CASCADE_INPUT);
			return;
		}

		/* Numbers the slave's input after the master's. */
		line += IRQ_INPUTS;
	}

	/* Runs the line's handler; a line with none is counted and only ended. */
	if (irq_sources[line].handler != NULL) {
		irq_sources[line].handler(irq_sources[line].argument);
	} else {
		irq_spurious++;
	}

	/* Lets the line, and the cascade for a slave line, interrupt again. */
	end_interrupt(line);
}

