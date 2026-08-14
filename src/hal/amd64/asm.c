/* amd64 low-level instruction wrappers. */
#include "asm.h"

void asm_cli(void) { __asm__ volatile("cli" ::: "memory"); }
void asm_sti(void) { __asm__ volatile("sti" ::: "memory"); }
void asm_hlt(void) { __asm__ volatile("hlt"); }

void asm_outb(uint16 port, uint8 data)
{
	__asm__ volatile("outb %0,%w1" : : "a"(data), "Nd"(port));
}

void asm_outw(uint16 port, uint16 data)
{
	__asm__ volatile("outw %0,%w1" : : "a"(data), "Nd"(port));
}

uint8 asm_inb(uint16 port)
{
	uint8 data;
	__asm__ volatile("inb %w1,%0" : "=a"(data) : "Nd"(port));
	return data;
}

uint16 asm_inw(uint16 port)
{
	uint16 data;
	__asm__ volatile("inw %w1,%0" : "=a"(data) : "Nd"(port));
	return data;
}

uint64 asm_get_rflags(void)
{
	uint64 value;
	__asm__ volatile("pushfq; popq %0" : "=r"(value));
	return value;
}

uintptr_t asm_get_cr2(void)
{
	uintptr_t value;
	__asm__ volatile("movq %%cr2,%0" : "=r"(value));
	return value;
}

uintptr_t asm_get_cr3(void)
{
	uintptr_t value;
	__asm__ volatile("movq %%cr3,%0" : "=r"(value));
	return value;
}

void asm_load_cr3(uintptr_t address)
{
	__asm__ volatile("movq %0,%%cr3" : : "r"(address) : "memory");
}

void asm_flush_tlb(void)
{
	uintptr_t value = asm_get_cr3();
	asm_load_cr3(value);
}

void asm_lidt(const void *descriptor)
{
	__asm__ volatile("lidt (%0)" : : "r"(descriptor) : "memory");
}

uint64 asm_read_msr(uint32 msr)
{
	uint32 low, high;
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64)high << 32) | low;
}

void asm_write_msr(uint32 msr, uint64 value)
{
	__asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32)value),
	    "d"((uint32)(value >> 32)));
}

void
amd64_cpu_init(void)
{
	uint64 cr0, cr4;

	__asm__ volatile("movq %%cr0,%0" : "=r"(cr0));
	cr0 &= ~((uint64)(1U << 2) | (uint64)(1U << 3));
	cr0 |= 1U << 1;
	__asm__ volatile("movq %0,%%cr0" : : "r"(cr0) : "memory");
	__asm__ volatile("movq %%cr4,%0" : "=r"(cr4));
	cr4 |= (uint64)(1U << 9) | (uint64)(1U << 10);
	__asm__ volatile("movq %0,%%cr4" : : "r"(cr4) : "memory");
	__asm__ volatile("fninit");
}
