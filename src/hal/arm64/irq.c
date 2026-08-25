#include <hal/hal.h>
#include "asm.h"
#include "irq.h"
#include "bsp-rpi4/gic.h"

#define IRQ_MAX 1020
#define TIMER_INTID 30

struct irq_slot {
	hal_irq_handler_t handler;
	void *argument;
	struct hal_cpu_mask requested;
};

static struct irq_slot slots[IRQ_MAX];
static hal_irq_ack_t active_ack;

void rpi4_timer_interrupt(hal_irq_ack_t acknowledge);

bool
hal_irq_disable(void)
{
	uint64_t state = arm64_irq_save();
	return (state & (1U << 7)) == 0;
}

void
hal_irq_enable(void)
{
	arm64_irq_unmask();
}

void
hal_irq_mask(int irq)
{
	if (irq < 0 || irq >= IRQ_MAX)
		HAL_FATAL("bad IRQ mask");
	rpi4_gic_mask((uint32_t)irq);
}

void
hal_irq_unmask(int irq)
{
	if (irq < 0 || irq >= IRQ_MAX)
		HAL_FATAL("bad IRQ unmask");
	rpi4_gic_unmask((uint32_t)irq);
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge != active_ack)
		HAL_FATAL("invalid AArch64 IRQ acknowledgement");
	rpi4_gic_eoi((uint32_t)(acknowledge - 1U));
	active_ack = HAL_IRQ_ACK_NONE;
}

int
hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{
	bool enabled;

	if (irq < 0 || irq >= IRQ_MAX)
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
	if (irq < 0 || irq >= IRQ_MAX || requested == NULL ||
	    (requested->bits[0] & 1U) == 0)
		return HAL_ERR_INVALID;
	slots[irq].requested = *requested;
	return HAL_OK;
}

int
hal_irq_get_affinity(int irq, struct hal_irq_affinity *result)
{
	unsigned i;

	if (irq < 0 || irq >= IRQ_MAX || result == NULL)
		return HAL_ERR_INVALID;
	result->requested = slots[irq].requested;
	for (i = 0; i < HAL_CPU_MASK_WORDS; i++)
		result->effective.bits[i] = 0;
	result->effective.bits[0] = 1;
	return HAL_OK;
}

void
arm64_irq_dispatch(uint32_t id, hal_irq_ack_t acknowledge)
{
	if (active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested AArch64 IRQ acknowledgement");
	active_ack = acknowledge;
	if (id == TIMER_INTID) {
		rpi4_timer_interrupt(acknowledge);
		return;
	}
	if (id < IRQ_MAX && slots[id].handler != NULL) {
		slots[id].handler((int)id, acknowledge, slots[id].argument);
		return;
	}
	hal_irq_send_eoi(acknowledge);
	hal_printf("unexpected IRQ %u\n", id);
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
