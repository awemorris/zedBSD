#include <hal/hal.h>
#include "asm.h"
#include "irq.h"
#include "bsp-rpi4/gic.h"

#define IRQ_MAX 1020
#define TIMER_INTID 30
struct irq_slot{void(*handler)(void *);void *arg;};
static struct irq_slot slots[IRQ_MAX];
static unsigned isr_depth;
void rpi4_timer_interrupt(void);

bool hal_irq_disable(void){uint64 s=arm64_irq_save();return(s&(1U<<7))==0;}
void hal_irq_enable(void){arm64_irq_unmask();}
void hal_irq_mask(int irq){if(irq<0||irq>=IRQ_MAX)HAL_FATAL("bad IRQ mask");rpi4_gic_mask((uint32)irq);}
void hal_irq_unmask(int irq){if(irq<0||irq>=IRQ_MAX)HAL_FATAL("bad IRQ unmask");rpi4_gic_unmask((uint32)irq);}
void hal_irq_send_eoi(int irq){rpi4_gic_eoi((uint32)irq);}
void hal_irq_set_handler(int irq,void(*fn)(void *),void *arg)
{if(irq<0||irq>=IRQ_MAX)HAL_FATAL("bad IRQ handler");slots[irq].handler=fn;slots[irq].arg=arg;}
irqlock_t irq_acquire_lock(void){return hal_irq_disable()?1:0;}
void irq_unacquire_lock(irqlock_t lock){if(lock)hal_irq_enable();}
void irq_enter_isr(int irq){(void)irq;isr_depth++;}
void irq_leave_isr(int irq){(void)irq;if(isr_depth)isr_depth--;}
void arm64_irq_dispatch(uint32 id)
{
	if(id==TIMER_INTID){rpi4_timer_interrupt();return;}
	if(id<IRQ_MAX&&slots[id].handler)slots[id].handler(slots[id].arg);
	else hal_printf("unexpected IRQ %u\n",id);
}
int hal_get_current_cpu(void){return 0;}
int hal_get_cpu_count(void){return 1;}
void hal_send_ipi_one(int cpu,int n){(void)cpu;(void)n;}
void hal_send_ipi_mask(uint8_t *mask,int n){(void)mask;(void)n;}
void hal_send_ipi_others(int n){(void)n;}
