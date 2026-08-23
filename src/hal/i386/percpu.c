/* Per-CPU GDT and TSS instances for i386 SMP. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "apic-topology.h"
#include "defs.h"
#include "percpu.h"

struct gdtr { uint16_t limit;uint32_t base; } __attribute__((packed));
static uint64_t gdts[I386_APIC_MAX_CPUS][6] __attribute__((aligned(16)));
static uint8_t tsses[I386_APIC_MAX_CPUS][104] __attribute__((aligned(16)));

static uint64_t tss_descriptor(uintptr_t base)
{
	uint64_t d=103U;d|=(uint64_t)(base&0xffffU)<<16;d|=(uint64_t)((base>>16)&0xffU)<<32;
	d|=(uint64_t)0x89U<<40;d|=(uint64_t)((base>>24)&0xffU)<<56;return d;
}
void i386_percpu_init(hal_cpu_id_t cpu,uintptr_t stack)
{
	struct gdtr descriptor;uint16_t selector=SEG_TSS;uint32_t *tss;
	if(cpu>=I386_APIC_MAX_CPUS)HAL_FATAL("invalid i386 per-CPU ID");
	gdts[cpu][0]=0;gdts[cpu][1]=0x00cf9a000000ffffULL;gdts[cpu][2]=0x00cf92000000ffffULL;
	gdts[cpu][3]=0x00cff8000000ffffULL;gdts[cpu][4]=0x00cff2000000ffffULL;
	hal_memset(tsses[cpu],0,sizeof(tsses[cpu]));tss=(uint32_t*)tsses[cpu];tss[1]=(uint32_t)stack;tss[2]=SEG_SYS_DATA;
	*(uint16_t*)(tsses[cpu]+102)=sizeof(tsses[cpu]);gdts[cpu][5]=tss_descriptor((uintptr_t)tsses[cpu]);
	descriptor.limit=sizeof(gdts[cpu])-1U;descriptor.base=(uint32_t)(uintptr_t)gdts[cpu];
	__asm__ volatile("lgdt %0"::"m"(descriptor):"memory");
	__asm__ volatile("ljmp $0x08,$1f;1:" ::: "memory");
	__asm__ volatile("movw %0,%%ds;movw %0,%%es;movw %0,%%fs;movw %0,%%gs;movw %0,%%ss"::"r"((uint16_t)SEG_SYS_DATA):"memory");
	__asm__ volatile("ltr %0"::"r"(selector):"memory");
}
void i386_percpu_set_kernel_stack(hal_cpu_id_t cpu,uintptr_t stack)
{
	if(cpu>=I386_APIC_MAX_CPUS)
		HAL_FATAL("invalid i386 TSS CPU");
	((uint32_t*)tsses[cpu])[1]=(uint32_t)stack;
}
