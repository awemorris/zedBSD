/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's interrupts.  The HAL does not drive the board's interrupt
 * controller yet, so no source can be registered; the tick is the
 * processor's own decrementer and needs none.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../percpu.h"
#include "../board.h"

/*
 * Reports an external interrupt the HAL cannot attribute.
 */
void
ppc64_take_external(
	void)
{
	/* Nothing enables a source, so this is unexpected. */
	hal_puts("unexpected ppc64 external interrupt\n");
}

/*
 * Turns external interrupts off and reports whether they were on.
 */
bool
hal_irq_disable(
	void)
{
	bool enabled;

	/* The machine state's enable bit. */
	enabled = ppc64_irq_save();

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
	ppc64_irq_restore(true);
}

/* No source exists to mask. */
void
hal_irq_mask(
	int irq)
{
	UNUSED_PARAMETER(irq);
	HAL_FATAL("bad ppc64 IRQ mask");
}

/* No source exists to unmask. */
void
hal_irq_unmask(
	int irq)
{
	UNUSED_PARAMETER(irq);
	HAL_FATAL("bad ppc64 IRQ unmask");
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
	struct ppc64_percpu *cpu;

	/* Requires the acknowledgement of the interrupt being handled. */
	cpu = ppc64_percpu_current();
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge != cpu->active_ack)
		HAL_FATAL("invalid ppc64 IRQ acknowledgement");
	cpu->active_ack = HAL_IRQ_ACK_NONE;
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
