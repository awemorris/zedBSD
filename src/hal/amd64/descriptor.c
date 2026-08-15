/* amd64 per-CPU GDT and TSS. */
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

struct descriptor_state {
	uint64 gdt[7] __attribute__((aligned(16)));
	struct amd64_tss tss __attribute__((aligned(16)));
	uint8 double_fault_stack[PAGE_SIZE * 4] __attribute__((aligned(16)));
};
static struct descriptor_state states[AMD64_SMP_MAX_CPUS];

_Static_assert(sizeof(struct amd64_tss) == 104, "amd64 TSS size");

void
amd64_descriptor_init(void)
{
	struct table_descriptor gdtr;
	struct descriptor_state *state = &states[hal_cpu_current()];
	uintptr_t base = (uintptr_t)&state->tss;
	uint64 low;

	hal_memset(state, 0, sizeof(*state));
	state->gdt[1] = 0x00af9a000000ffffULL;
	state->gdt[2] = 0x00cf92000000ffffULL;
	/* DPL3 64-bit code: L=1 and D=0. */
	state->gdt[3] = 0x00affa000000ffffULL;
	state->gdt[4] = 0x00cff2000000ffffULL;
	low = (sizeof(state->tss) - 1U) & 0xffffU;
	low |= (uint64)(base & 0xffffffU) << 16;
	low |= (uint64)0x89U << 40;
	low |= (uint64)((sizeof(state->tss) - 1U) >> 16 & 0x0fU) << 48;
	low |= (uint64)((base >> 24) & 0xffU) << 56;
	state->gdt[5] = low;
	state->gdt[6] = base >> 32;
	state->tss.ist1 = (uintptr_t)state->double_fault_stack +
	    sizeof(state->double_fault_stack);
	state->tss.iomap_base = sizeof(state->tss);
	gdtr.limit = sizeof(state->gdt) - 1U;
	gdtr.base = (uintptr_t)state->gdt;
	amd64_load_gdt(&gdtr);
}

void amd64_set_tss_rsp0(uintptr_t stack_top)
{ states[hal_cpu_current()].tss.rsp0 = stack_top; }
