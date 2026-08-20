/* i386 xAPIC CPU discovery and INIT-SIPI-SIPI startup. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "apic-topology.h"
#include "asm.h"
#include "int.h"
#include "interrupt-controller.h"
#include "lapic.h"
#include "percpu.h"
#include "smp.h"
#include "space.h"
#include "task.h"

#define TRAMPOLINE_PHYS 0x6000U
#define AP_STACK_SIZE 16384U
#define VECTOR_NOTIFY 0xd0U
#define VECTOR_PANIC 0xd1U
extern uint8 i386_ap_trampoline_start[],i386_ap_trampoline_end[];
extern uint8 i386_ap_trampoline_cr3[],i386_ap_trampoline_stack[];
extern uint8 i386_ap_trampoline_cpu[],i386_ap_trampoline_target[];
extern void i386_ap_high_entry(void);

struct cpu_state{uint8 apic_id;volatile unsigned ready;void*stack;};
static struct cpu_state cpus[I386_APIC_MAX_CPUS];static unsigned cpu_count=1;static volatile unsigned configured;static struct hal_cpu_mask ready_mask;
static size_t off(uint8*s){return(size_t)(s-i386_ap_trampoline_start);}
static void delay(void){volatile unsigned n;for(n=0;n<200000U;n++)__asm__ volatile("pause");}

void i386_smp_configure(const struct i386_apic_topology*t)
{
	unsigned i,target=1;uint8 bsp=i386_lapic_id();hal_memset(cpus,0,sizeof(cpus));cpus[0].apic_id=bsp;
	for(i=0;i<t->cpu_count&&target<I386_APIC_MAX_CPUS;i++)if(t->cpus[i].apic_id!=bsp)cpus[target++].apic_id=t->cpus[i].apic_id;
	cpu_count=target;hal_cpu_mask_zero(&ready_mask);hal_cpu_mask_set(&ready_mask,0);configured=1;
}
hal_cpu_id_t hal_cpu_current(void){unsigned i;if(!configured)return 0;{uint8 id=i386_lapic_id();for(i=0;i<cpu_count;i++)if(cpus[i].apic_id==id)return i;}HAL_FATAL("unknown i386 APIC ID");return 0;}
unsigned hal_cpu_count(void){return cpu_count;}
void hal_cpu_ready_mask(struct hal_cpu_mask*r){unsigned w;if(r==NULL)return;hal_rmb();for(w=0;w<HAL_CPU_MASK_WORDS;w++)r->bits[w]=ready_mask.bits[w];}
void*i386_smp_bootstrap_stack(hal_cpu_id_t cpu){return cpu<cpu_count?cpus[cpu].stack:NULL;}
int i386_smp_apic_id(hal_cpu_id_t cpu,uint8*id){if(cpu>=cpu_count||id==NULL)return HAL_ERR_INVALID;*id=cpus[cpu].apic_id;return HAL_OK;}

static int start_one(hal_cpu_id_t cpu)
{
	uint8*dst=(uint8*)(uintptr_t)(TRAMPOLINE_PHYS|0x80000000U);size_t size=(size_t)(i386_ap_trampoline_end-i386_ap_trampoline_start);unsigned timeout;
	if(size==0||size>4096U)
		return HAL_ERR_STATE;
	cpus[cpu].stack=hal_malloc(AP_STACK_SIZE);if(cpus[cpu].stack==NULL)return HAL_ERR_NOMEM;
	hal_memcpy(dst,i386_ap_trampoline_start,size);*(uint32*)(dst+off(i386_ap_trampoline_cr3))=asm_get_cr3();
	*(uint32*)(dst+off(i386_ap_trampoline_stack))=(uint32)(uintptr_t)cpus[cpu].stack+AP_STACK_SIZE;
	*(uint32*)(dst+off(i386_ap_trampoline_cpu))=cpu;*(uint32*)(dst+off(i386_ap_trampoline_target))=(uint32)(uintptr_t)i386_ap_high_entry;hal_wmb();
	if(i386_lapic_send_init(cpus[cpu].apic_id)!=HAL_OK)
		return HAL_ERR_IO;
	delay();
	if(i386_lapic_send_startup(cpus[cpu].apic_id,TRAMPOLINE_PHYS>>12)!=HAL_OK)
		return HAL_ERR_IO;
	delay();
	(void)i386_lapic_send_startup(cpus[cpu].apic_id,TRAMPOLINE_PHYS>>12);
	for(timeout=0;timeout<10000000U;timeout++){if(__atomic_load_n(&cpus[cpu].ready,__ATOMIC_ACQUIRE))return HAL_OK;__asm__ volatile("pause");}
	return HAL_ERR_TIMEOUT;
}
int hal_cpu_start_others(void){hal_cpu_id_t c;for(c=1;c<cpu_count;c++){int e=start_one(c);if(e!=HAL_OK)return e;}return HAL_OK;}
void i386_smp_ap_entry(uint32 cpu)
{
	if(cpu==0||cpu>=cpu_count)
		HAL_FATAL("invalid i386 AP entry");
	i386_percpu_init(cpu,(uintptr_t)cpus[cpu].stack+AP_STACK_SIZE);
	i386_int_load();i386_space_init_secondary();i386_task_init_secondary(cpu,(uintptr_t)cpus[cpu].stack+AP_STACK_SIZE);
	i386_lapic_init_cpu();i386_lapic_timer_start(i386_interrupt_timer_ticks());cpus[cpu].ready=1;
	ready_mask.bits[cpu/64U]|=(uint64)1U<<(cpu%64U);hal_wmb();kernel_secondary_entry(cpu);HAL_FATAL("secondary kernel entry returned");
}
int hal_cpu_notify(hal_cpu_id_t cpu){if(!i386_interrupt_uses_apic())return cpu==0?HAL_ERR_UNSUPPORTED:HAL_ERR_INVALID;if(cpu>=cpu_count)return HAL_ERR_INVALID;if(cpu!=0&&!cpus[cpu].ready)return HAL_ERR_STATE;return i386_lapic_send_fixed(cpus[cpu].apic_id,VECTOR_NOTIFY);}
int hal_cpu_notify_mask(const struct hal_cpu_mask*m){hal_cpu_id_t c;if(m==NULL)return HAL_ERR_INVALID;for(c=cpu_count;c<HAL_CPU_MAX;c++)if(hal_cpu_mask_test(m,c))return HAL_ERR_INVALID;if(!i386_interrupt_uses_apic())return HAL_ERR_UNSUPPORTED;for(c=0;c<cpu_count;c++)if(hal_cpu_mask_test(m,c)){int e=i386_lapic_send_fixed(cpus[c].apic_id,VECTOR_NOTIFY);if(e!=HAL_OK)return e;}return HAL_OK;}
_Noreturn void hal_cpu_park(void){if(i386_interrupt_uses_apic())i386_lapic_timer_stop();(void)hal_irq_disable();for(;;)asm_hlt();}
_Noreturn void hal_cpu_panic_all(void){hal_cpu_id_t self=hal_cpu_current(),c;if(i386_interrupt_uses_apic())for(c=0;c<cpu_count;c++)if(c!=self&&(c==0||cpus[c].ready))(void)i386_lapic_send_fixed(cpus[c].apic_id,VECTOR_PANIC);(void)hal_irq_disable();for(;;)asm_hlt();}
