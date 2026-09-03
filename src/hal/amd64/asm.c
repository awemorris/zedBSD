/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Low-level amd64 instruction and memory-barrier wrappers.
 */

#include "asm.h"

/*
 * Disables maskable interrupts on the current CPU.
 */
void
asm_cli(
	void)
{
	/* Prevents compiler memory operations from crossing the CLI. */
	__asm__ volatile("cli" ::: "memory");
}

/*
 * Enables maskable interrupts on the current CPU.
 */
void
asm_sti(
	void)
{
	/* Prevents compiler memory operations from crossing the STI. */
	__asm__ volatile("sti" ::: "memory");
}

/*
 * Halts the current CPU until an interrupt or reset arrives.
 */
void
asm_hlt(
	void)
{
	/* Enters the architectural halted state. */
	__asm__ volatile("hlt");
}

/*
 * Implements the public HAL halt primitive.
 */
void
hal_halt(
	void)
{
	/* Delegates to the amd64 halt instruction wrapper. */
	asm_hlt();
}

/*
 * Completes all preceding memory accesses before later accesses.
 */
void
hal_mb(
	void)
{
	/* Issues the amd64 full memory fence. */
	__asm__ volatile("mfence" ::: "memory");
}

/*
 * Completes preceding reads before later reads.
 */
void
hal_rmb(
	void)
{
	/* Issues the amd64 load fence. */
	__asm__ volatile("lfence" ::: "memory");
}

/*
 * Completes preceding writes before later writes.
 */
void
hal_wmb(
	void)
{
	/* Issues the amd64 store fence. */
	__asm__ volatile("sfence" ::: "memory");
}

/*
 * Applies the HAL full barrier to device I/O ordering.
 */
void
hal_io_mb(
	void)
{
	/* Uses the architectural full memory barrier. */
	hal_mb();
}

/*
 * Applies the HAL read barrier to device I/O ordering.
 */
void
hal_io_rmb(
	void)
{
	/* Uses the architectural read memory barrier. */
	hal_rmb();
}

/*
 * Applies the HAL write barrier to device I/O ordering.
 */
void
hal_io_wmb(
	void)
{
	/* Uses the architectural write memory barrier. */
	hal_wmb();
}

/*
 * Writes one byte to an x86 I/O port.
 */
void
asm_outb(
	uint16_t port,
	uint8_t data)
{
	/* Preserves the exact volatile port-write operation. */
	__asm__ volatile("outb %0,%w1" : : "a"(data), "Nd"(port));
}

/*
 * Writes one word to an x86 I/O port.
 */
void
asm_outw(
	uint16_t port,
	uint16_t data)
{
	/* Preserves the exact volatile port-write operation. */
	__asm__ volatile("outw %0,%w1" : : "a"(data), "Nd"(port));
}

/*
 * Reads one byte from an x86 I/O port.
 */
uint8_t
asm_inb(
	uint16_t port)
{
	uint8_t data;

	/* Samples the requested port through a volatile instruction. */
	__asm__ volatile("inb %w1,%0" : "=a"(data) : "Nd"(port));

	/* Returns the sampled byte. */
	return data;
}

/*
 * Reads one word from an x86 I/O port.
 */
uint16_t
asm_inw(
	uint16_t port)
{
	uint16_t data;

	/* Samples the requested port through a volatile instruction. */
	__asm__ volatile("inw %w1,%0" : "=a"(data) : "Nd"(port));

	/* Returns the sampled word. */
	return data;
}

/*
 * Reads the current CPU's RFLAGS register.
 */
uint64_t
asm_get_rflags(
	void)
{
	uint64_t value;

	/* Moves RFLAGS through the stack into a general-purpose register. */
	__asm__ volatile("pushfq; popq %0" : "=r"(value));

	/* Returns the complete architectural flags value. */
	return value;
}

/*
 * Reads the current CPU's page-fault linear address.
 */
uintptr_t
asm_get_cr2(
	void)
{
	uintptr_t value;

	/* Samples control register CR2. */
	__asm__ volatile("movq %%cr2,%0" : "=r"(value));

	/* Returns the sampled fault address. */
	return value;
}

/*
 * Reads the current CPU's page-table root address.
 */
uintptr_t
asm_get_cr3(
	void)
{
	uintptr_t value;

	/* Samples control register CR3. */
	__asm__ volatile("movq %%cr3,%0" : "=r"(value));

	/* Returns the sampled page-table root. */
	return value;
}

/*
 * Loads the current CPU's page-table root address.
 */
void
asm_load_cr3(
	uintptr_t address)
{
	/* Loads CR3 and prevents compiler memory reordering around it. */
	__asm__ volatile("movq %0,%%cr3" : : "r"(address) : "memory");
}

/*
 * Flushes the current CPU's non-global translation cache entries.
 */
void
asm_flush_tlb(
	void)
{
	uintptr_t value;

	/* Reloads the active CR3 value to invalidate translations. */
	value = asm_get_cr3();
	asm_load_cr3(value);
}

/*
 * Loads the current CPU's interrupt descriptor-table register.
 */
void
asm_lidt(
	const void *descriptor)
{
	/* Loads the caller-supplied packed descriptor from memory. */
	__asm__ volatile("lidt (%0)" : : "r"(descriptor) : "memory");
}

/*
 * Reads a model-specific register on the current CPU.
 */
uint64_t
asm_read_msr(
	uint32_t msr)
{
	uint32_t low;
	uint32_t high;

	/* Reads the requested MSR into its architectural halves. */
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));

	/* Reassembles and returns the 64-bit register value. */
	return ((uint64_t)high << 32) | low;
}

/*
 * Writes a model-specific register on the current CPU.
 */
void
asm_write_msr(
	uint32_t msr,
	uint64_t value)
{
	/* Splits the value across the architectural WRMSR inputs. */
	__asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
	    "d"((uint32_t)(value >> 32)));
}

/*
 * Enables the amd64 floating-point and SSE execution environment.
 */
void
amd64_cpu_init(
	void)
{
	uint64_t cr0;
	uint64_t cr4;

	/* Enables native floating-point execution and exception reporting. */
	__asm__ volatile("movq %%cr0,%0" : "=r"(cr0));
	cr0 &= ~((uint64_t)(1U << 2) | (uint64_t)(1U << 3));
	cr0 |= 1U << 1;
	__asm__ volatile("movq %0,%%cr0" : : "r"(cr0) : "memory");

	/* Enables operating-system management of FXSAVE and SIMD exceptions. */
	__asm__ volatile("movq %%cr4,%0" : "=r"(cr4));
	cr4 |= (uint64_t)(1U << 9) | (uint64_t)(1U << 10);
	__asm__ volatile("movq %0,%%cr4" : : "r"(cr4) : "memory");

	/* Initializes the x87 state after enabling the execution environment. */
	__asm__ volatile("fninit");
}
