/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 numbered interrupts: the PLIC's sources, their handlers and
 * their acknowledgements.
 *
 * A numbered interrupt is a PLIC source.  Its handler is called with the
 * source claimed and must end it with hal_irq_send_eoi(); the
 * acknowledgement is the source number plus one, so that no source is
 * HAL_IRQ_ACK_NONE.  The tick and the notifications between harts are not
 * PLIC sources: their acknowledgement is RISCV32_ACK_LOCAL, which ends
 * nothing.  Every source is taken by the boot hart.
 */

#include <hal/hal.h>
#include "asm.h"
#include "irq.h"
#include "percpu.h"
#include "plic.h"
#include "spin.h"

/* The numbers a source can have: 1 to 1023. */
#define IRQ_MAX		1024

/*
 * The handler registered for one source, its argument and the hart mask
 * its driver asked for.
 */
struct irq_slot {
	hal_irq_handler_t handler;
	void *argument;
	struct hal_cpu_mask requested;
};

/*
 * One slot per source number.  The handler and argument are a pair kept
 * under slots_lock, so a hart that dispatches never sees a new handler
 * with an old argument.
 */
static struct irq_slot slots[IRQ_MAX];

/* Guards each slot's handler and argument; zero when free. */
static volatile unsigned slots_lock;

static int valid_source(int irq);
static void dispatch(uint32_t source);

/*
 * Turns the calling hart's interrupts off and reports whether they were on.
 */
bool
hal_irq_disable(
	void)
{
	bool enabled;

	/* Clears the global enable, keeping what it was. */
	enabled = riscv32_irq_save();

	/* Succeeded: whether the caller turns them on again. */
	return enabled;
}

/*
 * Turns the calling hart's interrupts on.
 */
void
hal_irq_enable(
	void)
{
	/* Sets the global enable. */
	riscv32_set_sstatus(RISCV32_SSTATUS_SIE);
}

/*
 * Masks one source: its priority zero is never above a threshold.
 */
void
hal_irq_mask(
	int irq)
{
	int valid;

	/* A number the controller does not have is a driver's fault. */
	valid = valid_source(irq);
	if (!valid)
		HAL_FATAL("bad riscv32 IRQ mask");

	/* The enable bit stays, so a claimed source can still be completed. */
	riscv32_plic_set_priority((uint32_t)irq, 0);
}

/*
 * Unmasks one source.
 */
void
hal_irq_unmask(
	int irq)
{
	int valid;

	/* A number the controller does not have is a driver's fault. */
	valid = valid_source(irq);
	if (!valid)
		HAL_FATAL("bad riscv32 IRQ unmask");

	/* Priority one is above the boot hart's open threshold. */
	riscv32_plic_set_priority((uint32_t)irq, 1);
}

/*
 * Registers a handler for one source and enables the source at the boot
 * hart's context.  The source stays masked until hal_irq_unmask().
 */
int
hal_irq_register(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	bool enabled;
	int valid;

	/* Requires a handler and a source the controller has. */
	if (handler == NULL)
		return HAL_ERR_INVALID;
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Installs the pair under the lock the dispatch reads it under. */
	enabled = riscv32_spin_lock(&slots_lock);

	slots[irq].handler = handler;
	slots[irq].argument = argument;

	riscv32_spin_unlock(&slots_lock, enabled);

	/* Routes the source to the boot hart; its priority still masks it. */
	riscv32_plic_enable((uint32_t)irq, 1);

	/* Succeeded: the next interrupt of the source goes to the handler. */
	return HAL_OK;
}

/*
 * Removes the handler registered for one source, masking and disabling it.
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
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Stops the source before its handler goes. */
	riscv32_plic_set_priority((uint32_t)irq, 0);
	riscv32_plic_enable((uint32_t)irq, 0);

	/* Removes the pair, which must be the caller's. */
	enabled = riscv32_spin_lock(&slots_lock);

	if (slots[irq].handler != handler || slots[irq].argument != argument) {
		riscv32_spin_unlock(&slots_lock, enabled);
		return HAL_ERR_INVALID;
	}

	/* Forgets the handler. */
	slots[irq].handler = NULL;
	slots[irq].argument = NULL;

	riscv32_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the source has no handler. */
	return HAL_OK;
}

/*
 * Ends the interrupt this hart is handling.
 *
 * The acknowledgement must be the one this hart holds.  A source is
 * completed at the controller; the local acknowledgement ends nothing.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	struct riscv32_percpu *cpu;

	/* Refuses an acknowledgement this hart is not holding. */
	cpu = riscv32_percpu_current();
	if (acknowledge == HAL_IRQ_ACK_NONE)
		HAL_FATAL("invalid riscv32 IRQ acknowledgement");
	if (acknowledge != cpu->active_ack)
		HAL_FATAL("invalid riscv32 IRQ acknowledgement");

	/* A source is completed; it carries the source number plus one. */
	if (acknowledge != RISCV32_ACK_LOCAL)
		riscv32_plic_complete((uint32_t)(acknowledge - 1U));
	cpu->active_ack = HAL_IRQ_ACK_NONE;
}

/*
 * Records the harts a source should go to; every source goes to the boot
 * hart, which the request must include.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	int valid;
	int boot_hart;

	/* Requires a source and a request that includes the boot hart. */
	valid = valid_source(irq);
	if (!valid || requested == NULL)
		return HAL_ERR_INVALID;
	boot_hart = hal_cpu_mask_test(requested, 0);
	if (!boot_hart)
		return HAL_ERR_INVALID;

	/* Succeeded: the request is kept for hal_irq_get_affinity(). */
	slots[irq].requested = *requested;
	return HAL_OK;
}

/*
 * Reports the harts asked for and the hart a source actually goes to.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	int valid;

	/* Refuses a source the controller does not have. */
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;

	/* The request, when wanted, and the boot hart as the one that takes it. */
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
 * Accepts the trigger of a level-high source, which is all the PLIC
 * takes; its gateways are fixed by the platform and cannot be changed.
 */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	int valid;

	/* Requires a source and the encodings of the HAL contract. */
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;
	if (polarity != HAL_IRQ_POLARITY_HIGH && polarity != HAL_IRQ_POLARITY_LOW)
		return HAL_ERR_INVALID;

	/* Refuses what the controller cannot represent. */
	if (trigger != HAL_IRQ_TRIGGER_LEVEL || polarity != HAL_IRQ_POLARITY_HIGH)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the source already behaves so. */
	return HAL_OK;
}

/*
 * Takes every pending source at the boot hart's context, one at a time.
 *
 * Each claimed source is dispatched before the next claim, so its
 * handler's completion lets the controller present the next one.
 */
void
riscv32_irq_take_external(
	void)
{
	uint32_t source;

	/* Claims until nothing is pending. */
	for (;;) {
		source = riscv32_plic_claim();
		if (source == 0)
			return;
		dispatch(source);
	}
}

/*
 * Refuses a message-signalled interrupt: the PLIC has none.
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

/* Reports whether a number is a source the controller has. */
static int
valid_source(
	int irq)
{
	uint32_t sources;

	/* Sources are numbered from 1 up to the controller's count. */
	sources = riscv32_plic_sources();
	if (irq <= 0 || irq >= IRQ_MAX)
		return 0;
	if ((uint32_t)irq > sources)
		return 0;

	/* Succeeded: the source exists. */
	return 1;
}

/*
 * Hands one claimed source to its handler, which ends it; a source nobody
 * serves is masked, ended and reported.
 */
static void
dispatch(
	uint32_t source)
{
	struct riscv32_percpu *cpu;
	hal_irq_handler_t handler;
	hal_irq_ack_t acknowledge;
	void *argument;
	bool enabled;

	/* Refuses a second interrupt while this hart still holds one. */
	cpu = riscv32_percpu_current();
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested riscv32 IRQ acknowledgement");
	acknowledge = (hal_irq_ack_t)source + 1U;
	cpu->active_ack = acknowledge;

	/* Takes the source's handler and argument as one pair. */
	handler = NULL;
	argument = NULL;
	if (source < IRQ_MAX) {
		enabled = riscv32_spin_lock(&slots_lock);

		handler = slots[source].handler;
		argument = slots[source].argument;

		riscv32_spin_unlock(&slots_lock, enabled);
	}

	/* A source with a handler is that handler's to end. */
	if (handler != NULL) {
		handler((int)source, acknowledge, argument);
		return;
	}

	/* A source nobody serves is masked so it does not come back, and ended. */
	riscv32_plic_set_priority(source, 0);
	hal_irq_send_eoi(acknowledge);
	hal_printf("unexpected riscv32 IRQ %u\n", source);
}
