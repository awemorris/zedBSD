/* UP interrupt implementation for sun4u. */
#include <hal/hal.h>
#include "asi.h"

#define SPARCV9_IRQ_MAX 16
struct irq_slot { void (*handler)(void *); void *argument; };
static struct irq_slot slots[SPARCV9_IRQ_MAX];
static unsigned isr_depth;

bool hal_irq_disable(void){unsigned long p=sparcv9_pstate();sparcv9_write_pstate(p&~2UL);return(p&2UL)!=0;}
void hal_irq_enable(void){sparcv9_write_pstate(sparcv9_pstate()|2UL);}
irqlock_t irq_acquire_lock(void){return hal_irq_disable()?1:0;}
void irq_unacquire_lock(irqlock_t l){if(l)hal_irq_enable();}
void hal_irq_mask(int n){(void)n;}
void hal_irq_unmask(int n){(void)n;}
void hal_irq_send_eoi(int n){(void)n;}
void hal_irq_set_handler(int n,void(*f)(void*),void*a){if(n<0||n>=SPARCV9_IRQ_MAX)HAL_FATAL("bad sun4u IRQ");slots[n].handler=f;slots[n].argument=a;}
void irq_enter_isr(int n){(void)n;isr_depth++;}
void irq_leave_isr(int n){(void)n;if(isr_depth)isr_depth--;}
int hal_get_current_cpu(void){return 0;}int hal_get_cpu_count(void){return 1;}
void hal_send_ipi_one(int c,int n){(void)c;(void)n;}void hal_send_ipi_mask(uint8*m,int n){(void)m;(void)n;}void hal_send_ipi_others(int n){(void)n;}
void hal_cpu_idle(void){hal_irq_enable();for(unsigned i=0;i<10000U;i++)__asm__ volatile("nop");(void)hal_irq_disable();}
