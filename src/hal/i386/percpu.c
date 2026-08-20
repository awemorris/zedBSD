/* Per-CPU GDT and TSS instances for i386 SMP. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "apic-topology.h"
#include "defs.h"
#include "percpu.h"

struct gdtr { uint16 limit;uint32 base; } __attribute__((packed));
static uint64 gdts[I386_APIC_MAX_CPUS][6] __attribute__((aligned(16)));
static uint8 tsses[I386_APIC_MAX_CPUS][104] __attribute__((aligned(16)));

static uint64 tss_descriptor(uintptr_t base)
{
	uint64 d=103U;d|=(uint64)(base&0xffffU)<<16;d|=(uint64)((base>>16)&0xffU)<<32;
	d|=(uint64)0x89U<<40;d|=(uint64)((base>>24)&0xffU)<<56;return d;
}
void i386_percpu_init(hal_cpu_id_t cpu,uintptr_t stack)
{
	struct gdtr descriptor;uint16 selector=SEG_TSS;uint32 *tss;
	if(cpu>=I386_APIC_MAX_CPUS)HAL_FATAL("invalid i386 per-CPU ID");
	gdts[cpu][0]=0;gdts[cpu][1]=0x00cf9a000000ffffULL;gdts[cpu][2]=0x00cf92000000ffffULL;
	gdts[cpu][3]=0x00cff8000000ffffULL;gdts[cpu][4]=0x00cff2000000ffffULL;
	hal_memset(tsses[cpu],0,sizeof(tsses[cpu]));tss=(uint32*)tsses[cpu];tss[1]=(uint32)stack;tss[2]=SEG_SYS_DATA;
	*(uint16*)(tsses[cpu]+102)=sizeof(tsses[cpu]);gdts[cpu][5]=tss_descriptor((uintptr_t)tsses[cpu]);
	descriptor.limit=sizeof(gdts[cpu])-1U;descriptor.base=(uint32)(uintptr_t)gdts[cpu];
	__asm__ volatile("lgdt %0"::"m"(descriptor):"memory");
	__asm__ volatile("ljmp $0x08,$1f;1:" ::: "memory");
	__asm__ volatile("movw %0,%%ds;movw %0,%%es;movw %0,%%fs;movw %0,%%gs;movw %0,%%ss"::"r"((uint16)SEG_SYS_DATA):"memory");
	__asm__ volatile("ltr %0"::"r"(selector):"memory");
}
void i386_percpu_set_kernel_stack(hal_cpu_id_t cpu,uintptr_t stack)
{
	if(cpu>=I386_APIC_MAX_CPUS)
		HAL_FATAL("invalid i386 TSS CPU");
	((uint32*)tsses[cpu])[1]=(uint32)stack;
}
