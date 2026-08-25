/* UP interrupt implementation for sun4u. */
#include <hal/hal.h>
#include "asi.h"
#include "irq.h"

#define SPARCV9_IRQ_MAX 16

struct irq_slot {
	hal_irq_handler_t handler;
	void *argument;
	struct hal_cpu_mask requested;
};

static struct irq_slot slots[SPARCV9_IRQ_MAX];
static hal_irq_ack_t active_ack;

bool
hal_irq_disable(void)
{
	unsigned long pstate = sparcv9_pstate();
	sparcv9_write_pstate(pstate & ~2UL);
	return (pstate & 2UL) != 0;
}

void
hal_irq_enable(void)
{
	sparcv9_write_pstate(sparcv9_pstate() | 2UL);
}

void hal_irq_mask(int irq) { (void)irq; }
void hal_irq_unmask(int irq) { (void)irq; }

hal_irq_ack_t
sparcv9_irq_begin(int irq)
{
	if (active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested SPARC V9 IRQ acknowledgement");
	active_ack = (hal_irq_ack_t)irq + 1U;
	return active_ack;
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge != active_ack)
		HAL_FATAL("invalid SPARC V9 IRQ acknowledgement");
	active_ack = HAL_IRQ_ACK_NONE;
}

int
hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{
	bool enabled;

	if (irq < 0 || irq >= SPARCV9_IRQ_MAX)
		return HAL_ERR_INVALID;
	enabled = hal_irq_disable();
	slots[irq].handler = handler;
	slots[irq].argument = handler != NULL ? argument : NULL;
	if (enabled)
		hal_irq_enable();
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
	if (irq < 0 || irq >= SPARCV9_IRQ_MAX || requested == NULL ||
	    (requested->bits[0] & 1U) == 0)
		return HAL_ERR_INVALID;
	slots[irq].requested = *requested;
	return HAL_OK;
}

int
hal_irq_get_affinity(int irq, struct hal_irq_affinity *result)
{
	unsigned i;

	if (irq < 0 || irq >= SPARCV9_IRQ_MAX || result == NULL)
		return HAL_ERR_INVALID;
	result->requested = slots[irq].requested;
	for (i = 0; i < HAL_CPU_MASK_WORDS; i++)
		result->effective.bits[i] = 0;
	result->effective.bits[0] = 1;
	return HAL_OK;
}

void
hal_cpu_idle(void)
{
	/* sun4u has no halt instruction in this HAL contract.  Keep the
	 * interrupt window short so a wakeup published by the timer is observed
	 * on the next scheduler pass instead of after an arbitrary busy delay. */
	hal_irq_enable();
	__asm__ volatile("nop" : : : "memory");
	(void)hal_irq_disable();
}

int
hal_irq_register_msi(const char *source, hal_irq_handler_t handler,
	void *handler_arg, int *mapped_irq, paddr_t *mapped_addr,
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
