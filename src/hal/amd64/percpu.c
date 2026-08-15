/* amd64 CPU-local HAL state selected through IA32_GS_BASE. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "asm.h"
#include "percpu.h"

static struct amd64_percpu cpu_states[AMD64_SMP_MAX_CPUS];

void
amd64_percpu_bootstrap(void)
{
	struct amd64_percpu *cpu = &cpu_states[0];
	hal_memset(cpu, 0, sizeof(*cpu));
	cpu->logical_id = 0;
	cpu->current_space = HAL_SPACE_SYS;
	amd64_percpu_select(cpu);
}

struct amd64_percpu *
amd64_percpu_get(hal_cpu_id_t cpu)
{
	return cpu < AMD64_SMP_MAX_CPUS ? &cpu_states[cpu] : NULL;
}

struct amd64_percpu *
amd64_percpu_current(void)
{
	struct amd64_percpu *cpu =
	    (struct amd64_percpu *)(uintptr_t)asm_read_msr(AMD64_MSR_GS_BASE);
	if (cpu == NULL || cpu->self != cpu)
		HAL_FATAL("amd64 per-CPU state is not selected");
	return cpu;
}

void
amd64_percpu_select(struct amd64_percpu *cpu)
{
	if (cpu == NULL)
		HAL_FATAL("invalid amd64 per-CPU state");
	cpu->self = cpu;
	asm_write_msr(AMD64_MSR_GS_BASE, (uint64)(uintptr_t)cpu);
}

hal_irq_ack_t
amd64_irq_ack_begin(uint32 vector, int irq)
{
	struct amd64_percpu *cpu = amd64_percpu_current();
	struct amd64_irq_ack *record;
	if (cpu->acknowledgement_depth >= AMD64_IRQ_ACK_DEPTH)
		HAL_FATAL("amd64 IRQ acknowledgement stack overflow");
	record = &cpu->acknowledgements[cpu->acknowledgement_depth++];
	if (record->active)
		HAL_FATAL("stale amd64 IRQ acknowledgement");
	record->vector = vector;
	record->irq = (uint32)irq;
	record->active = 1;
	return (hal_irq_ack_t)(uintptr_t)record;
}
