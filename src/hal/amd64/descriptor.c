/* amd64 GDT and single-CPU TSS. */
#include <hal/hal.h>
#include "defs.h"
#include "descriptor.h"

struct amd64_tss {
	uint32 reserved0;
	uint64 rsp0, rsp1, rsp2;
	uint64 reserved1;
	uint64 ist1, ist2, ist3, ist4, ist5, ist6, ist7;
	uint64 reserved2;
	uint16 reserved3;
	uint16 iomap_base;
} __attribute__((packed));

struct table_descriptor {
	uint16 limit;
	uint64 base;
} __attribute__((packed));

static uint64 gdt[7] __attribute__((aligned(16)));
static struct amd64_tss tss __attribute__((aligned(16)));
static uint8 double_fault_stack[PAGE_SIZE * 4] __attribute__((aligned(16)));

_Static_assert(sizeof(struct amd64_tss) == 104, "amd64 TSS size");

void
amd64_descriptor_init(void)
{
	struct table_descriptor gdtr;
	uintptr_t base = (uintptr_t)&tss;
	uint64 low;

	hal_memset(gdt, 0, sizeof(gdt));
	hal_memset(&tss, 0, sizeof(tss));
	gdt[1] = 0x00af9a000000ffffULL;
	gdt[2] = 0x00cf92000000ffffULL;
	/* DPL3 64-bit code: L=1 and D=0. */
	gdt[3] = 0x00affa000000ffffULL;
	gdt[4] = 0x00cff2000000ffffULL;
	low = (sizeof(tss) - 1U) & 0xffffU;
	low |= (uint64)(base & 0xffffffU) << 16;
	low |= (uint64)0x89U << 40;
	low |= (uint64)((sizeof(tss) - 1U) >> 16 & 0x0fU) << 48;
	low |= (uint64)((base >> 24) & 0xffU) << 56;
	gdt[5] = low;
	gdt[6] = base >> 32;
	tss.ist1 = (uintptr_t)double_fault_stack + sizeof(double_fault_stack);
	tss.iomap_base = sizeof(tss);
	gdtr.limit = sizeof(gdt) - 1U;
	gdtr.base = (uintptr_t)gdt;
	amd64_load_gdt(&gdtr);
}

void amd64_set_tss_rsp0(uintptr_t stack_top) { tss.rsp0 = stack_top; }
