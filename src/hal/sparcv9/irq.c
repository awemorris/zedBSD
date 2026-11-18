/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's interrupts on a single processor.
 *
 * The only interrupt the HAL takes is the processor's own tick, which the
 * timer hands to the kernel.  The board's devices are polled: the HAL does
 * not route their interrupts, so a numbered IRQ cannot be registered and a
 * message-signalled one cannot be allocated.
 */

#include <hal/hal.h>
#include "defs.h"
#include "asi.h"
#include "irq.h"

/* The numbered IRQs the kernel may name: the sixteen processor levels. */
#define SPARCV9_IRQ_MAX	16

/*
 * The acknowledgement of the interrupt being handled, or HAL_IRQ_ACK_NONE.
 *
 * Set by sparcv9_irq_begin() and cleared by hal_irq_send_eoi(); interrupts
 * are off in between, so no second interrupt can begin meanwhile.
 */
static hal_irq_ack_t active_acknowledge;

/*
 * The affinity each numbered IRQ was last asked for.
 *
 * Kept only to be reported back; with one processor every IRQ goes to it.
 */
static struct hal_cpu_mask requested_affinity[SPARCV9_IRQ_MAX];

/*
 * Disables interrupts and reports whether they were enabled.
 */
bool
hal_irq_disable(
	void)
{
	uint64_t state;

	/* Clears the interrupt enable bit of the processor state. */
	state = sparcv9_read_pstate();
	sparcv9_write_pstate(state & ~SPARCV9_PSTATE_IE);

	/* Reports whether interrupts were enabled before. */
	if ((state & SPARCV9_PSTATE_IE) != 0)
		return true;
	return false;
}

/*
 * Enables interrupts.
 */
void
hal_irq_enable(
	void)
{
	uint64_t state;

	/* Sets the interrupt enable bit of the processor state. */
	state = sparcv9_read_pstate();
	sparcv9_write_pstate(state | SPARCV9_PSTATE_IE);
}

/*
 * Masks a numbered IRQ; no device interrupt is routed, so nothing changes.
 */
void
hal_irq_mask(
	int irq_num)
{
	UNUSED_PARAMETER(irq_num);
}

/*
 * Unmasks a numbered IRQ; no device interrupt is routed, so nothing changes.
 */
void
hal_irq_unmask(
	int irq_num)
{
	UNUSED_PARAMETER(irq_num);
}

/*
 * Refuses a handler for a numbered IRQ: the HAL routes no device interrupt.
 */
int
hal_irq_register(
	int irq_num,
	hal_irq_handler_t func,
	void *arg)
{
	UNUSED_PARAMETER(func);
	UNUSED_PARAMETER(arg);

	/* Refuses an IRQ that does not exist. */
	if (irq_num < 0 || irq_num >= SPARCV9_IRQ_MAX)
		return HAL_ERR_INVALID;

	/* Failed: the board's devices are polled. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to remove a handler, which hal_irq_register() never took.
 */
int
hal_irq_unregister(
	int irq_num,
	hal_irq_handler_t func,
	void *arg)
{
	UNUSED_PARAMETER(func);
	UNUSED_PARAMETER(arg);

	/* Refuses an IRQ that does not exist. */
	if (irq_num < 0 || irq_num >= SPARCV9_IRQ_MAX)
		return HAL_ERR_INVALID;

	/* Failed: no handler was ever registered. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Starts the handling of one interrupt and returns its acknowledgement.
 */
hal_irq_ack_t
sparcv9_irq_begin(
	int irq)
{
	/* One interrupt is handled at a time. */
	if (active_acknowledge != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested SPARC V9 IRQ acknowledgement");

	/*
	 * The acknowledgement names the IRQ, offset by one so that IRQ 0 is not
	 * HAL_IRQ_ACK_NONE; hal_irq_send_eoi() checks it against this one.
	 */
	active_acknowledge = (hal_irq_ack_t)irq + 1U;

	/* Succeeded: the acknowledgement the handler passes back. */
	return active_acknowledge;
}

/*
 * Ends the handling of the interrupt an acknowledgement names.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	/* Only the interrupt being handled can be ended. */
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge != active_acknowledge)
		HAL_FATAL("invalid SPARC V9 IRQ acknowledgement");

	/* No interrupt is being handled any more; the next may begin. */
	active_acknowledge = HAL_IRQ_ACK_NONE;
}

/*
 * Records the affinity asked for a numbered IRQ, which must include the
 * only processor.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	bool includes_boot_cpu;

	/* Refuses an IRQ that does not exist, or no mask. */
	if (irq < 0 || irq >= SPARCV9_IRQ_MAX)
		return HAL_ERR_INVALID;
	if (requested == NULL)
		return HAL_ERR_INVALID;

	/* Refuses a mask without the only processor. */
	includes_boot_cpu = hal_cpu_mask_test(requested, 0);
	if (!includes_boot_cpu)
		return HAL_ERR_INVALID;

	/* Succeeded: the mask is kept to be reported back. */
	requested_affinity[irq] = *requested;
	return HAL_OK;
}

/*
 * Reports the affinity asked for a numbered IRQ and the one in effect,
 * which is the only processor.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	/* Refuses an IRQ that does not exist. */
	if (irq < 0 || irq >= SPARCV9_IRQ_MAX)
		return HAL_ERR_INVALID;

	/* Reports the mask asked for. */
	if (requested != NULL)
		*requested = requested_affinity[irq];

	/* Reports the only processor as the one in effect. */
	if (effective != NULL) {
		hal_cpu_mask_zero(effective);
		hal_cpu_mask_set(effective, 0);
	}

	/* Succeeded. */
	return HAL_OK;
}

/*
 * Refuses a change of trigger mode or polarity: no interrupt controller is
 * driven.
 */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	/* Refuses an IRQ, a trigger mode or a polarity that does not exist. */
	if (irq < 0 || irq >= SPARCV9_IRQ_MAX)
		return HAL_ERR_INVALID;
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;
	if (polarity != HAL_IRQ_POLARITY_HIGH && polarity != HAL_IRQ_POLARITY_LOW)
		return HAL_ERR_INVALID;

	/* Failed: the configuration is fixed. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Waits for the next interrupt with interrupts enabled, then disables them.
 *
 * The UltraSPARC has no instruction that waits for an interrupt, so the
 * window is opened briefly: a tick that is pending is taken in it, and the
 * kernel's idle loop comes back here otherwise.
 */
void
hal_cpu_idle(
	void)
{
	/* Lets a pending interrupt in. */
	hal_irq_enable();
	__asm__ volatile("nop" : : : "memory");

	/* Returns with interrupts disabled, as the contract asks. */
	(void)hal_irq_disable();
}

/*
 * Refuses a message-signalled IRQ: the board's PCI bridge's messages are
 * not routed.
 */
int
hal_irq_register_msi(
	const char *source,
	hal_irq_handler_t handler,
	void *handler_arg,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);

	/* Failed: no message-signalled IRQ exists on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to remove a message-signalled IRQ, which is never made.
 */
int
hal_irq_unregister_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);

	/* Failed: no message-signalled IRQ exists on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to allocate a message-signalled IRQ.
 */
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

	/* Failed: no message-signalled IRQ exists on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to attach a handler to a message-signalled IRQ.
 */
int
hal_irq_attach_msi(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);

	/* Failed: no message-signalled IRQ exists on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to detach a handler from a message-signalled IRQ.
 */
int
hal_irq_detach_msi_sync(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);

	/* Failed: no message-signalled IRQ exists on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to free a message-signalled IRQ.
 */
int
hal_irq_free_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);

	/* Failed: no message-signalled IRQ exists on this port. */
	return HAL_ERR_UNSUPPORTED;
}
