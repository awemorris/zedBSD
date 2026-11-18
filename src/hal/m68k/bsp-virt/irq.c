/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's interrupts: six chained controllers of 32 sources
 * each, controller n raising processor level n + 1, and the kernel's
 * interrupt interface on top of them.
 *
 * An interrupt number is 1 + 32 * controller + source.  The tick is a
 * source of the sixth controller and is served by the clock without the
 * kernel's table.
 *
 * Values referenced: QEMU v10.0.0 hw/intc/goldfish_pic.c (the registers:
 * pending count, pending mask, disable all, disable, enable) and
 * hw/m68k/virt.c (the controllers' levels).
 */

#include <hal/hal.h>
#include "../m68040/asm.h"
#include "../m68040/percpu.h"
#include "../m68040/spin.h"
#include "bsp.h"

/* The controller's registers, big-endian words. */
#define PIC_PENDING_MASK	0x04U
#define PIC_DISABLE_ALL		0x08U
#define PIC_DISABLE		0x0cU
#define PIC_ENABLE		0x10U

/* The controllers, their spacing and the sources of each. */
#define PIC_COUNT		6U
#define PIC_STRIDE		0x1000U
#define PIC_SOURCES		32U
#define IRQ_MAX			(1U + PIC_COUNT * PIC_SOURCES)

/*
 * A source's registered handler and argument.  Kept with interrupts off;
 * a NULL handler means none.
 */
struct irq_slot {
	hal_irq_handler_t handler;
	void *argument;
};

/* Each source's handler, by interrupt number; slot 0 is unused. */
static struct irq_slot slots[IRQ_MAX];

/* Guards the slots against a change while an interrupt reads them; zero when free. */
static volatile unsigned slots_lock;

/* The tick's interrupt number, which the clock serves. */
static unsigned clock_irq;

static int valid_source(int irq);
static volatile uint32_t *pic_register(unsigned controller, uint32_t offset);
static void dispatch(unsigned irq);

/*
 * Masks every source of every controller.
 */
void
virt_pic_init(
	void)
{
	unsigned controller;
	const struct virt_board_info *board;

	/* Nothing raises an interrupt until a source is enabled. */
	board = virt_board();
	if (board->pic_base == 0)
		HAL_FATAL("m68k virt board has no interrupt controller");
	for (controller = 0; controller < PIC_COUNT; controller++)
		*pic_register(controller, PIC_DISABLE_ALL) = 1U;
}

/*
 * Enables the tick's source and records it for the dispatch.
 */
void
virt_pic_enable_clock(
	unsigned irq)
{
	/* The clock's source is enabled with no handler of the kernel's. */
	clock_irq = irq;
	*pic_register((irq - 1U) / PIC_SOURCES, PIC_ENABLE) = 1U << ((irq - 1U) % PIC_SOURCES);
}

/*
 * Reports whether a source is raised and enabled at its controller.
 */
bool
virt_pic_pending(
	unsigned irq)
{
	unsigned number;
	uint32_t pending;

	/* The controller's mask of raised, enabled sources. */
	number = irq - 1U;
	pending = *pic_register(number / PIC_SOURCES, PIC_PENDING_MASK);
	if ((pending & (1U << (number % PIC_SOURCES))) != 0)
		return true;

	/* The source is quiet. */
	return false;
}

/*
 * Serves every pending source of the controller behind a level.
 */
void
virt_take_interrupt(
	unsigned level)
{
	uint32_t pending;
	unsigned controller;
	unsigned source;

	/* Level n is the controller n - 1; level 7 has none. */
	if (level == 0 || level > PIC_COUNT) {
		hal_printf("unexpected m68040 interrupt level %u\n", level);
		return;
	}

	/* Level n is controller n - 1. */
	controller = level - 1U;

	/* Each pending and enabled source, lowest first, until none is left. */
	for (;;) {
		pending = *pic_register(controller, PIC_PENDING_MASK);
		if (pending == 0)
			return;
		for (source = 0; source < PIC_SOURCES; source++) {
			if ((pending & (1U << source)) != 0)
				break;
		}

		/* Hands the source to its handler. */
		dispatch(1U + controller * PIC_SOURCES + source);
	}
}

/*
 * Closes the interrupt mask and reports whether it was open.
 */
bool
hal_irq_disable(
	void)
{
	bool enabled;

	/* The status register's mask. */
	enabled = m68040_irq_save();

	/* Succeeded: whether the mask was open. */
	return enabled;
}

/*
 * Opens the interrupt mask.
 */
void
hal_irq_enable(
	void)
{
	/* Every level is let in. */
	m68040_irq_restore(true);
}

/*
 * Keeps a source from raising its interrupt.
 */
void
hal_irq_mask(
	int irq)
{
	unsigned number;
	int valid;

	/* Refuses a source the controllers do not have. */
	valid = valid_source(irq);
	if (!valid)
		HAL_FATAL("bad m68k virt IRQ mask");
	number = (unsigned)irq - 1U;
	*pic_register(number / PIC_SOURCES, PIC_DISABLE) = 1U << (number % PIC_SOURCES);
}

/*
 * Lets a source raise its interrupt again.
 */
void
hal_irq_unmask(
	int irq)
{
	unsigned number;
	int valid;

	/* Refuses a source the controllers do not have. */
	valid = valid_source(irq);
	if (!valid)
		HAL_FATAL("bad m68k virt IRQ unmask");
	number = (unsigned)irq - 1U;
	*pic_register(number / PIC_SOURCES, PIC_ENABLE) = 1U << (number % PIC_SOURCES);
}

/*
 * Registers a handler for a source; the source stays masked until the
 * driver unmasks it.
 */
int
hal_irq_register(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	bool enabled;
	int valid;

	/* Requires a handler and a source that is not the tick's. */
	if (handler == NULL)
		return HAL_ERR_INVALID;
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((unsigned)irq == clock_irq)
		return HAL_ERR_INVALID;

	/* Records the handler. */
	enabled = m68040_spin_lock(&slots_lock);
	slots[irq].handler = handler;
	slots[irq].argument = argument;
	m68040_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the source is the handler's. */
	return HAL_OK;
}

/*
 * Removes a source's handler and masks the source.
 */
int
hal_irq_unregister(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	bool enabled;
	int valid;

	/* Requires the registered handler of a source. */
	if (handler == NULL)
		return HAL_ERR_INVALID;
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	enabled = m68040_spin_lock(&slots_lock);
	if (slots[irq].handler != handler || slots[irq].argument != argument) {
		m68040_spin_unlock(&slots_lock, enabled);
		return HAL_ERR_INVALID;
	}

	/* Masked only once the removal is known to succeed. */
	hal_irq_mask(irq);

	/* Forgets the handler. */
	slots[irq].handler = NULL;
	slots[irq].argument = NULL;
	m68040_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the source has no handler. */
	return HAL_OK;
}

/*
 * Ends the interrupt a handler took.  A source stays raised while its
 * device asks, so there is nothing to tell the controller.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	struct m68040_percpu *cpu;

	/* Requires the acknowledgement of the interrupt being handled. */
	cpu = m68040_percpu_current();
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge != cpu->active_ack)
		HAL_FATAL("invalid m68k virt IRQ acknowledgement");
	cpu->active_ack = HAL_IRQ_ACK_NONE;
}

/*
 * Accepts an affinity that includes the one processor.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	int valid;
	bool member;

	/* Every source is taken by processor 0. */
	valid = valid_source(irq);
	if (!valid || requested == NULL)
		return HAL_ERR_INVALID;
	member = hal_cpu_mask_test(requested, 0);
	if (!member)
		return HAL_ERR_INVALID;

	/* Succeeded: the source stays on processor 0. */
	return HAL_OK;
}

/*
 * Reports a source's affinity: processor 0.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	int valid;

	/* Requires a source the controllers have. */
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	if (requested != NULL) {
		hal_cpu_mask_zero(requested);
		hal_cpu_mask_set(requested, 0);
	}

	/* The effective affinity is the same. */
	if (effective != NULL) {
		hal_cpu_mask_zero(effective);
		hal_cpu_mask_set(effective, 0);
	}

	/* Succeeded: the affinity is processor 0. */
	return HAL_OK;
}

/*
 * Accepts the one trigger the controllers have: level, active high.
 */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	int valid;

	/* Requires a source and a known trigger and polarity. */
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;
	if (polarity != HAL_IRQ_POLARITY_HIGH && polarity != HAL_IRQ_POLARITY_LOW)
		return HAL_ERR_INVALID;
	if (trigger != HAL_IRQ_TRIGGER_LEVEL || polarity != HAL_IRQ_POLARITY_HIGH)
		return HAL_ERR_UNSUPPORTED;
	return HAL_OK;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_register_msi(
	const char *source,
	hal_irq_handler_t handler,
	void *handler_arg,
	int *mapped_irq,
	hal_physaddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_unregister_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_alloc_msi(
	const char *source,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_attach_msi(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_detach_msi_sync(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_free_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);
	return HAL_ERR_UNSUPPORTED;
}

/* Reports whether an interrupt number names a source of the controllers. */
static int
valid_source(
	int irq)
{
	/* One to the last source of the sixth controller. */
	if (irq <= 0 || (unsigned)irq >= IRQ_MAX)
		return 0;
	return 1;
}

/* Reports a register of one controller. */
static volatile uint32_t *
pic_register(
	unsigned controller,
	uint32_t offset)
{
	/* The controllers follow each other a page apart. */
	return (volatile uint32_t *)(uintptr_t)(virt_board()->pic_base + controller * PIC_STRIDE + offset);
}

/*
 * Hands one source to the clock or to its handler.  A source with no
 * handler is masked, so that it does not raise its interrupt forever.
 */
static void
dispatch(
	unsigned irq)
{
	struct m68040_percpu *cpu;
	hal_irq_handler_t handler;
	hal_irq_ack_t acknowledge;
	void *argument;

	/* The tick goes to the clock. */
	if (irq == clock_irq) {
		virt_clock_interrupt();
		return;
	}

	/* Refuses an interrupt inside another one. */
	cpu = m68040_percpu_current();
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested m68k virt IRQ acknowledgement");

	/* The handler ends the interrupt with hal_irq_send_eoi(). */
	acknowledge = (hal_irq_ack_t)irq;
	handler = slots[irq].handler;
	argument = slots[irq].argument;
	if (handler != NULL) {
		cpu->active_ack = acknowledge;
		handler((int)irq, acknowledge, argument);
		return;
	}

	/* No handler: the source is masked and reported. */
	hal_irq_mask((int)irq);
	hal_printf("unexpected m68k virt IRQ %u\n", irq);
}
