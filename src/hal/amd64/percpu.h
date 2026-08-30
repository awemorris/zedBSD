#ifndef ZEDBSD_HAL_AMD64_PERCPU_H
#define ZEDBSD_HAL_AMD64_PERCPU_H

#include <hal/hal.h>
#include "defs.h"

struct amd64_irq_ack {
	uint32_t vector;
	uint32_t irq;
	unsigned active;
};

#define AMD64_IRQ_ACK_DEPTH 8U
#define AMD64_STARTUP_ERROR_TIMER 0x100U

struct amd64_task;

struct amd64_percpu {
	struct amd64_percpu *self;
	hal_cpu_id_t logical_id;
	uint32_t apic_id;
	volatile unsigned ready;
	volatile unsigned startup_error;
	struct hal_pmem bootstrap_stack;
	struct amd64_irq_ack acknowledgements[AMD64_IRQ_ACK_DEPTH];
	unsigned acknowledgement_depth;
	struct amd64_task *running_task;
	hal_space_t current_space;
};

void amd64_percpu_bootstrap(void);
struct amd64_percpu *amd64_percpu_get(hal_cpu_id_t cpu);
struct amd64_percpu *amd64_percpu_current(void);
void amd64_percpu_select(struct amd64_percpu *cpu);
hal_irq_ack_t amd64_irq_ack_begin(uint32_t vector, int irq);

#endif
