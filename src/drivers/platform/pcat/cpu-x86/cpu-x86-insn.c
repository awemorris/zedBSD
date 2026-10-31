/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPUID, RDMSR and WRMSR instructions of the x86 processor driver.
 *
 * They are the only place the driver executes a processor instruction,
 * so a kernel built without the performance control carries CPUID alone
 * and cannot touch a model-specific register at all.
 */

#include "cpu-x86-insn.h"

/*
 * Runs CPUID for one leaf and sub-leaf on this logical processor.
 *
 * Procedure referenced: Intel SDM Vol. 2A rev. 093, CPUID (EAX names
 * the leaf, ECX the sub-leaf; the four registers come back).
 */
void
drv_cpu_x86_cpuid(
	uint32_t leaf,
	uint32_t subleaf,
	struct cpux86_cpuid_regs *regs)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	/* Asks the processor about the leaf. */
	__asm__ volatile("cpuid"
			 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
			 : "a"(leaf), "c"(subleaf));

	/* Hands the four registers back. */
	regs->eax = eax;
	regs->ebx = ebx;
	regs->ecx = ecx;
	regs->edx = edx;
}

#if CONFIG_CPU_PERF

/*
 * Reads one model-specific register of this logical processor.
 *
 * Procedure referenced: Intel SDM Vol. 2B rev. 093, RDMSR (ECX names the
 * register; EDX:EAX is the value).
 */
uint64_t
drv_cpu_x86_rdmsr(
	uint32_t msr)
{
	uint32_t low;
	uint32_t high;

	/* Reads the register as two halves. */
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));

	/* Succeeded: the halves joined. */
	return ((uint64_t)high << 32) | low;
}

/*
 * Writes one model-specific register of this logical processor.
 *
 * Procedure referenced: Intel SDM Vol. 2B rev. 093, WRMSR (ECX names the
 * register; EDX:EAX is the value).
 */
void
drv_cpu_x86_wrmsr(
	uint32_t msr,
	uint64_t value)
{
	uint32_t low;
	uint32_t high;

	/* Splits the value into the halves the instruction takes. */
	low = (uint32_t)value;
	high = (uint32_t)(value >> 32);

	/* Writes the register. */
	__asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}

#endif
