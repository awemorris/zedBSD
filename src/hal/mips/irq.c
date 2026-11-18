/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS interrupt interface of the kernel.
 *
 * Interrupts are Status.IE.  The only interrupt the kernel is handed is
 * the tick, which needs no controller: its acknowledgement is
 * MIPS_ACK_LOCAL, and its end only clears the record of the interrupt
 * being handled.  The board's controller lines are not offered to the
 * kernel's drivers yet (no driver of this port uses one), so registering
 * a line is refused.
 */

#include <hal/hal.h>
#include "asm.h"
#include "irq.h"
#include "spin.h"

/*
 * The acknowledgement of the interrupt the kernel is handling, or
 * HAL_IRQ_ACK_NONE.  Set when the kernel is handed an interrupt and
 * cleared by its end; the processor handles one at a time.
 */
static hal_irq_ack_t irq_active_ack;

/*
 * Turns external interrupts off and reports whether they were on.
 */
bool
hal_irq_disable(
	void)
{
	bool enabled;

	/* The machine state's enable bit. */
	enabled = mips_irq_save();

	/* Succeeded: whether interrupts were on. */
	return enabled;
}

/*
 * Turns external interrupts on.
 */
void
hal_irq_enable(
	void)
{
	/* The machine state's enable bit. */
	mips_irq_restore(true);
}

/* No source exists to mask. */
void
hal_irq_mask(
	int irq)
{
	UNUSED_PARAMETER(irq);
	HAL_FATAL("bad mips IRQ mask");
}

/* No source exists to unmask. */
void
hal_irq_unmask(
	int irq)
{
	UNUSED_PARAMETER(irq);
	HAL_FATAL("bad mips IRQ unmask");
}

/* No source can be registered. */
int
hal_irq_register(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(argument);
	return HAL_ERR_INVALID;
}

/* No source was registered. */
int
hal_irq_unregister(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(argument);
	return HAL_ERR_INVALID;
}

/*
 * Ends the interrupt being handled: only the tick's acknowledgement exists.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	/* Requires the acknowledgement of the interrupt being handled. */
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge != irq_active_ack)
		HAL_FATAL("invalid mips IRQ acknowledgement");
	irq_active_ack = HAL_IRQ_ACK_NONE;
}

/*
 * Records the start of an interrupt that needs no controller, before
 * the kernel is handed MIPS_ACK_LOCAL.
 */
void
mips_irq_begin_local(
	void)
{
	/* Refuses an interrupt inside another the kernel has not ended. */
	if (irq_active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested mips IRQ acknowledgement");
	irq_active_ack = MIPS_ACK_LOCAL;
}

/* No source has an affinity. */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(requested);
	return HAL_ERR_INVALID;
}

/* No source has an affinity. */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(requested);
	UNUSED_PARAMETER(effective);
	return HAL_ERR_INVALID;
}

/* No source has a mode. */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(trigger);
	UNUSED_PARAMETER(polarity);
	return HAL_ERR_INVALID;
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
