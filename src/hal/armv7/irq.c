/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 numbered interrupts: the GIC's lines, their handlers and
 * their acknowledgements.
 *
 * A numbered interrupt is a GIC interrupt number.  Its handler is called
 * with the interrupt acknowledged at the CPU interface and must end it
 * with hal_irq_send_eoi(); the acknowledgement is the acknowledge
 * register's value plus one, so that no interrupt is HAL_IRQ_ACK_NONE.
 * The tick and the notifications between processors are GIC interrupts
 * too, served inside the HAL and ended the same way.  Every shared line
 * is taken by the boot processor.
 */

#include <hal/hal.h>
#include "asm.h"
#include "clock.h"
#include "gic.h"
#include "irq.h"
#include "percpu.h"
#include "spin.h"

/* The first private peripheral interrupt; the numbers below it are SGIs. */
#define IRQ_FIRST_PPI	16U

/* The interrupt numbers a slot can have: 0 to 1019. */
#define IRQ_MAX		1020

/*
 * The handler registered for one interrupt, its argument and the
 * processor mask its driver asked for.
 */
struct irq_slot {
	hal_irq_handler_t handler;
	void *argument;
	struct hal_cpu_mask requested;
};

/*
 * One slot per interrupt number.  The handler and argument are a pair
 * kept under slots_lock, so a processor that dispatches never sees a new
 * handler with an old argument.
 */
static struct irq_slot slots[IRQ_MAX];

/* Guards each slot's handler and argument; zero when free. */
static volatile unsigned slots_lock;

static int valid_line(int irq);
static void dispatch(uint32_t intid, hal_irq_ack_t acknowledge);

/*
 * Sets up the GIC with every line masked; nothing is registered yet.
 */
void
armv7_irq_init(
	void)
{
	/* The distributor and the boot processor's interface. */
	armv7_gic_init();
	hal_printf("ARMV7 GIC lines=%u\n", armv7_gic_line_count());
}

/*
 * Turns the calling processor's interrupts off and reports whether they
 * were on.
 */
bool
hal_irq_disable(
	void)
{
	bool enabled;

	/* Sets the IRQ mask, keeping what it was. */
	enabled = armv7_irq_save();

	/* Succeeded: whether the caller turns them on again. */
	return enabled;
}

/*
 * Turns the calling processor's interrupts on.
 */
void
hal_irq_enable(
	void)
{
	/* Clears the IRQ mask. */
	__asm__ volatile("cpsie i" : : : "memory");
}

/*
 * Masks one line.
 */
void
hal_irq_mask(
	int irq)
{
	int valid;

	/* A number the GIC does not have is a driver's fault. */
	valid = valid_line(irq);
	if (!valid)
		HAL_FATAL("bad armv7 IRQ mask");

	/* Disables the line at the distributor. */
	armv7_gic_mask((uint32_t)irq);
}

/*
 * Unmasks one line.
 */
void
hal_irq_unmask(
	int irq)
{
	int valid;

	/* A number the GIC does not have is a driver's fault. */
	valid = valid_line(irq);
	if (!valid)
		HAL_FATAL("bad armv7 IRQ unmask");

	/* Enables the line at the distributor. */
	armv7_gic_unmask((uint32_t)irq);
}

/*
 * Registers a handler for one line.  The line stays masked until
 * hal_irq_unmask().
 */
int
hal_irq_register(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	bool enabled;
	int valid;

	/* Requires a handler and a line the GIC has. */
	if (handler == NULL)
		return HAL_ERR_INVALID;
	valid = valid_line(irq);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Installs the pair under the lock the dispatch reads it under. */
	enabled = armv7_spin_lock(&slots_lock);

	slots[irq].handler = handler;
	slots[irq].argument = argument;

	armv7_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the next interrupt of the line goes to the handler. */
	return HAL_OK;
}

/*
 * Removes the handler registered for one line, masking it.
 */
int
hal_irq_unregister(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	bool enabled;
	int valid;

	/* Requires a plausible registration to remove. */
	if (handler == NULL)
		return HAL_ERR_INVALID;
	valid = valid_line(irq);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Stops the line before its handler goes. */
	armv7_gic_mask((uint32_t)irq);

	/* Removes the pair, which must be the caller's. */
	enabled = armv7_spin_lock(&slots_lock);

	/* Refuses a pair the slot does not hold. */
	if (slots[irq].handler != handler || slots[irq].argument != argument) {
		armv7_spin_unlock(&slots_lock, enabled);
		return HAL_ERR_INVALID;
	}

	/* Forgets the handler. */
	slots[irq].handler = NULL;
	slots[irq].argument = NULL;

	armv7_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the line has no handler. */
	return HAL_OK;
}

/*
 * Ends the interrupt this processor is handling.
 *
 * The acknowledgement must be the one this processor holds: the CPU
 * interface ends interrupts in the order they were taken.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	struct armv7_percpu *cpu;

	/* Refuses an acknowledgement this processor is not holding. */
	cpu = armv7_percpu_current();
	if (acknowledge == HAL_IRQ_ACK_NONE)
		HAL_FATAL("invalid armv7 IRQ acknowledgement");
	if (acknowledge != cpu->active_ack)
		HAL_FATAL("invalid armv7 IRQ acknowledgement");

	/* Ends the interrupt; the acknowledgement carries the acknowledge value plus one. */
	armv7_gic_eoi((uint32_t)(acknowledge - 1U));
	cpu->active_ack = HAL_IRQ_ACK_NONE;
}

/*
 * Records the processors a line should go to; every shared line goes to
 * the boot processor, which the request must include.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	int valid;
	int boot_cpu;

	/* Requires a line and a request that includes the boot processor. */
	valid = valid_line(irq);
	if (!valid || requested == NULL)
		return HAL_ERR_INVALID;
	boot_cpu = hal_cpu_mask_test(requested, 0);
	if (!boot_cpu)
		return HAL_ERR_INVALID;

	/* Succeeded: the request is kept for hal_irq_get_affinity(). */
	slots[irq].requested = *requested;
	return HAL_OK;
}

/*
 * Reports the processors asked for and the processor a line actually goes
 * to.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	int valid;

	/* Refuses a line the GIC does not have. */
	valid = valid_line(irq);
	if (!valid)
		return HAL_ERR_INVALID;

	/* The request, when wanted, and the boot processor as the one that takes it. */
	if (requested != NULL)
		*requested = slots[irq].requested;
	if (effective != NULL) {
		hal_cpu_mask_zero(effective);
		hal_cpu_mask_set(effective, 0);
	}

	/* Succeeded: both masks are filled. */
	return HAL_OK;
}

/*
 * Sets the trigger mode of one masked line.  The GIC's inputs are active
 * high; an inverted line is the source's to invert.
 */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	int valid;
	int error;

	/* Requires a line and the encodings of the HAL contract. */
	valid = valid_line(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;
	if (polarity != HAL_IRQ_POLARITY_HIGH && polarity != HAL_IRQ_POLARITY_LOW)
		return HAL_ERR_INVALID;

	/* Refuses the polarity the GIC cannot represent. */
	if (polarity != HAL_IRQ_POLARITY_HIGH)
		return HAL_ERR_UNSUPPORTED;

	/* Programs the masked line and checks the controller's readback. */
	error = armv7_gic_set_trigger((uint32_t)irq, trigger);
	if (error != HAL_OK)
		return error;

	/* Succeeded: enabling the line uses the mode. */
	return HAL_OK;
}

/*
 * Takes the highest pending interrupt at this processor's interface and
 * serves it.  Another pending one interrupts again once this one returns.
 */
void
armv7_irq_take(
	void)
{
	struct armv7_percpu *cpu;
	hal_irq_ack_t acknowledge;
	uint32_t value;
	uint32_t intid;

	/* A spurious acknowledgement has nothing to end. */
	value = armv7_gic_ack();
	intid = value & ARMV7_GIC_INTID_MASK;
	if (intid >= IRQ_MAX)
		return;

	/* Refuses a second interrupt while this processor still holds one. */
	cpu = armv7_percpu_current();
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested armv7 IRQ acknowledgement");
	acknowledge = (hal_irq_ack_t)value + 1U;
	cpu->active_ack = acknowledge;

	/* Hands the interrupt to the code that serves it. */
	dispatch(intid, acknowledge);
}

/*
 * Refuses a message-signalled interrupt: the GICs of these boards have no
 * message-based interrupts.
 */
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

	/* Failed: no message-signalled interrupts on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to remove a message-signalled interrupt, which never exists.
 */
int
hal_irq_unregister_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);

	/* Failed: no message-signalled interrupts on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to allocate a message-signalled interrupt.
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

	/* Failed: no message-signalled interrupts on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to attach to a message-signalled interrupt.
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

	/* Failed: no message-signalled interrupts on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to detach from a message-signalled interrupt.
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

	/* Failed: no message-signalled interrupts on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to free a message-signalled interrupt.
 */
int
hal_irq_free_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);

	/* Failed: no message-signalled interrupts on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/* Reports whether a number is a private or shared line the GIC has. */
static int
valid_line(
	int irq)
{
	uint32_t lines;

	/* The SGIs are the HAL's own; the lines end at the distributor's count. */
	lines = armv7_gic_line_count();
	if (irq < (int)IRQ_FIRST_PPI || irq >= IRQ_MAX)
		return 0;
	if ((uint32_t)irq >= lines)
		return 0;

	/* Succeeded: the line exists. */
	return 1;
}

/*
 * Hands one acknowledged interrupt to the code that serves it, which ends
 * it; a line nobody serves is masked, ended and reported.
 */
static void
dispatch(
	uint32_t intid,
	hal_irq_ack_t acknowledge)
{
	hal_irq_handler_t handler;
	void *argument;
	uint32_t tick;
	bool enabled;

	/* A notification between processors. */
	if (intid < IRQ_FIRST_PPI) {
		armv7_smp_take_ipi(intid, acknowledge);
		return;
	}

	/* The scheduling tick. */
	tick = armv7_clock_tick_irq();
	if (intid == tick) {
		armv7_clock_interrupt(acknowledge);
		return;
	}

	/* Takes the line's handler and argument as one pair. */
	enabled = armv7_spin_lock(&slots_lock);

	handler = slots[intid].handler;
	argument = slots[intid].argument;

	armv7_spin_unlock(&slots_lock, enabled);

	/* A line with a handler is that handler's to end. */
	if (handler != NULL) {
		handler((int)intid, acknowledge, argument);
		return;
	}

	/* A line nobody serves is masked so it does not come back, and ended. */
	armv7_gic_mask(intid);
	hal_irq_send_eoi(acknowledge);
	hal_printf("unexpected armv7 IRQ %u\n", intid);
}
