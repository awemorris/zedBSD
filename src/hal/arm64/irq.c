#include <hal/hal.h>
#include "asm.h"
#include "irq.h"
#include "percpu.h"
#include "spin.h"
#include "watch.h"
#include "clock.h"
#include "gic.h"

#define IRQ_MAX 1020
#define TIMER_INTID 30

struct irq_slot {
	hal_irq_handler_t handler;
	void *argument;
	struct hal_cpu_mask requested;
};

static struct irq_slot slots[IRQ_MAX];

/*
 * Guards each slot's handler and argument as a pair, so a CPU that
 * dispatches never sees a new handler with an old argument.  Zero when free.
 */
static volatile unsigned slots_lock;

/*
 * Turns the calling CPU's interrupts off and reports whether they were on.
 *
 * The change from on to off is noted for the stall watch with the place
 * of the code that made it, so a CPU that stops with its interrupts off
 * is described by where they went off.
 */
bool
hal_irq_disable(
	void)
{
	uint64_t state;
	uint64_t pc;

	/* Masks IRQs and keeps the mask bit as it was. */
	state = arm64_irq_save();

	/* Interrupts that were off already were noted when they went off. */
	if ((state & (1U << 7)) != 0)
		return false;

	/* Notes when and where they went off: this code, inlined into its caller, and the caller's caller. */
	__asm__ volatile("adr %0, ." : "=r"(pc));
	arm64_watch_note_irq_off(pc, (uint64_t)(uintptr_t)__builtin_return_address(0));

	/* Succeeded: interrupts were on, and the caller turns them on again. */
	return true;
}

/*
 * Turns the calling CPU's interrupts on.
 */
void
hal_irq_enable(
	void)
{
	/* The stall watch learns first that interrupts are on again. */
	arm64_watch_note_irq_on();

	/* Unmasks IRQs. */
	arm64_irq_unmask();
}

void
hal_irq_mask(int irq)
{
	if (irq < 0 || irq >= IRQ_MAX)
		HAL_FATAL("bad IRQ mask");
	arm64_gic_mask((uint32_t)irq);
}

void
hal_irq_unmask(int irq)
{
	if (irq < 0 || irq >= IRQ_MAX)
		HAL_FATAL("bad IRQ unmask");
	arm64_gic_unmask((uint32_t)irq);
}

/*
 * Ends the interrupt this CPU is handling at the interrupt controller.
 *
 * The acknowledgement must be the one this CPU took last: the GIC's CPU
 * interface ends interrupts in the order they were taken, per CPU.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	struct arm64_percpu *cpu;

	/* Refuses an acknowledgement this CPU is not holding. */
	cpu = arm64_percpu_current();
	if (acknowledge == HAL_IRQ_ACK_NONE)
		HAL_FATAL("invalid AArch64 IRQ acknowledgement");
	if (acknowledge != cpu->active_ack)
		HAL_FATAL("invalid AArch64 IRQ acknowledgement");

	/* Ends the interrupt; the acknowledgement carries the IAR value plus one. */
	arm64_gic_eoi((uint32_t)(acknowledge - 1U));
	cpu->active_ack = HAL_IRQ_ACK_NONE;
}

/*
 * Installs or removes the handler of one numbered line.
 */
int
hal_irq_set_handler(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	bool enabled;

	/* Refuses a line the table does not have. */
	if (irq < 0 || irq >= IRQ_MAX)
		return HAL_ERR_INVALID;

	/* Replaces the pair under the lock the dispatch reads it under. */
	enabled = arm64_spin_lock(&slots_lock);

	slots[irq].handler = handler;
	slots[irq].argument = NULL;
	if (handler != NULL)
		slots[irq].argument = argument;

	arm64_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the next interrupt of the line goes to the handler. */
	return HAL_OK;
}

int
hal_irq_service_wait(int irq, hal_irq_ack_t *acknowledge)
{
	(void)irq;
	(void)acknowledge;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_set_affinity(int irq, const struct hal_cpu_mask *requested)
{
	if (irq < 0 || irq >= IRQ_MAX || requested == NULL ||
	    (requested->bits[0] & 1U) == 0)
		return HAL_ERR_INVALID;
	slots[irq].requested = *requested;
	return HAL_OK;
}

/*
 * Sets the trigger mode of one masked Raspberry Pi 4 interrupt.
 *
 * GIC inputs use active-high polarity; inversion belongs to the source.
 */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	int error;

	/* Requires a numbered line within the HAL interrupt table. */
	if (irq < 0 || irq >= IRQ_MAX)
		return HAL_ERR_INVALID;

	/* Accepts only the two trigger encodings of the HAL contract. */
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;

	/* Distinguishes an invalid polarity from a supported encoding. */
	if (polarity != HAL_IRQ_POLARITY_HIGH && polarity != HAL_IRQ_POLARITY_LOW)
		return HAL_ERR_INVALID;

	/* Refuses polarity inversion that the GIC cannot represent. */
	if (polarity != HAL_IRQ_POLARITY_HIGH)
		return HAL_ERR_UNSUPPORTED;

	/* Programs the masked line and checks the controller's readback. */
	error = arm64_gic_set_trigger((uint32_t)irq, trigger);
	if (error != HAL_OK)
		return error;

	/* Succeeded: enabling the line uses the requested trigger mode. */
	return HAL_OK;
}

/*
 * Reports the requested and effective affinity of one IRQ.
 */
int
hal_irq_get_affinity(int irq, struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	unsigned i;

	/* Rejects an invalid IRQ number. */
	if (irq < 0 || irq >= IRQ_MAX)
		return HAL_ERR_INVALID;

	/* Reports the mask the caller asked for, when wanted. */
	if (requested != NULL)
		*requested = slots[irq].requested;

	/* This board delivers every IRQ to the boot CPU. */
	if (effective != NULL) {
		for (i = 0; i < HAL_CPU_MASK_WORDS; i++)
			effective->bits[i] = 0;
		effective->bits[0] = 1;
	}
	return HAL_OK;
}

/*
 * Registers a handler for one numbered IRQ.
 */
int
hal_irq_register(int irq_num, hal_irq_handler_t func, void *arg)
{
	/* Requires a handler; removal goes through hal_irq_unregister(). */
	if (func == NULL)
		return HAL_ERR_INVALID;

	/* Installs the callback for this line. */
	return hal_irq_set_handler(irq_num, func, arg);
}

/*
 * Removes the handler registered for one numbered IRQ.
 */
int
hal_irq_unregister(int irq_num, hal_irq_handler_t func, void *arg)
{
	/* Requires a plausible registration to remove. */
	if (func == NULL || irq_num < 0 || irq_num >= IRQ_MAX)
		return HAL_ERR_INVALID;

	/* Rejects a removal the caller does not own. */
	if (slots[irq_num].handler != func || slots[irq_num].argument != arg)
		return HAL_ERR_INVALID;

	/* Removes the confirmed callback. */
	return hal_irq_set_handler(irq_num, NULL, NULL);
}

/*
 * Hands an acknowledged interrupt to the code that serves it.
 *
 * The scheduling tick goes to the timer; a numbered line goes to its
 * registered handler, which ends it; anything else is ended here.
 */
void
arm64_irq_dispatch(
	uint32_t id,
	hal_irq_ack_t acknowledge)
{
	struct arm64_percpu *cpu;
	hal_irq_handler_t handler;
	void *argument;
	bool enabled;

	/* Refuses a second interrupt while this CPU still holds one. */
	cpu = arm64_percpu_current();
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested AArch64 IRQ acknowledgement");
	cpu->active_ack = acknowledge;

	/* The scheduling tick of this CPU. */
	if (id == TIMER_INTID) {
		arm64_timer_interrupt(acknowledge);
		return;
	}

	/* Another CPU asks this one to look for work. */
	if (id == ARM64_SGI_NOTIFY) {
		kernel_cpu_notify_handler(cpu->logical_id, acknowledge);
		return;
	}

	/* Another CPU panicked: this one stops where it is. */
	if (id == ARM64_SGI_STOP) {
		hal_irq_send_eoi(acknowledge);
		hal_cpu_park();
	}

	/* Takes the line's handler and argument as one pair. */
	handler = NULL;
	argument = NULL;
	if (id < IRQ_MAX) {
		enabled = arm64_spin_lock(&slots_lock);

		handler = slots[id].handler;
		argument = slots[id].argument;

		arm64_spin_unlock(&slots_lock, enabled);
	}

	/* A line with a handler is that handler's to end. */
	if (handler != NULL) {
		handler((int)id, acknowledge, argument);
		return;
	}

	/* A line nobody serves is ended and reported. */
	hal_irq_send_eoi(acknowledge);
	hal_printf("unexpected IRQ %u\n", id);
}

int
hal_irq_register_msi(const char *source, hal_irq_handler_t handler,
	void *handler_arg, int *mapped_irq, hal_physaddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	(void)source;
	(void)handler;
	(void)handler_arg;
	(void)mapped_irq;
	(void)mapped_addr;
	(void)mapped_event;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_unregister_msi(int mapped_irq)
{
	(void)mapped_irq;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_alloc_msi(const char *source, int *mapped_irq,
	paddr_t *mapped_addr, uint32_t *mapped_event)
{
	(void)source;
	(void)mapped_irq;
	(void)mapped_addr;
	(void)mapped_event;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_attach_msi(int mapped_irq, hal_irq_handler_t handler, void *handler_arg)
{
	(void)mapped_irq;
	(void)handler;
	(void)handler_arg;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_detach_msi_sync(int mapped_irq, hal_irq_handler_t handler,
	void *handler_arg)
{
	(void)mapped_irq;
	(void)handler;
	(void)handler_arg;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_free_msi(int mapped_irq)
{
	(void)mapped_irq;
	return HAL_ERR_UNSUPPORTED;
}
