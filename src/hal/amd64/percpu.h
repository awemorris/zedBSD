/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private amd64 per-CPU state and interrupt acknowledgement contract.
 */

#ifndef KERN_HAL_AMD64_PERCPU_H
#define KERN_HAL_AMD64_PERCPU_H

#include <hal/hal.h>
#include "defs.h"
#include "bsp-pcat/timecounter-policy.h"

#define AMD64_IRQ_ACK_DEPTH 8U
#define AMD64_STARTUP_ERROR_TIMER 0x100U

struct amd64_irq_ack {
	uint32_t vector;
	uint32_t irq;
	unsigned active;
};

struct amd64_task;

struct amd64_percpu {
	struct amd64_percpu *self;
	/* The ring-zero stack top the SYSCALL entry switches to (TSS rsp0). */
	uint64_t syscall_rsp;
	/* Where the SYSCALL entry keeps the user stack pointer meanwhile. */
	uint64_t syscall_scratch;
	hal_cpu_id_t logical_id;
	uint32_t apic_id;
	volatile unsigned ready;
	volatile unsigned startup_error;
	volatile unsigned timecounter_probe_ready;
	volatile unsigned timecounter_probe_request;
	volatile unsigned timecounter_probe_ack;
	volatile unsigned timecounter_probe_release;
	uint64_t timecounter_probe_sample;
	struct amd64_timecounter_cpu_metadata timecounter_metadata;
	int timecounter_probe_valid;
	volatile unsigned timecounter_runtime_ready;
	volatile unsigned timecounter_runtime_done;
	unsigned timecounter_runtime_status;
	unsigned timecounter_runtime_reads;
	hal_physaddr_t bootstrap_stack_paddr;
	size_t bootstrap_stack_size;
	struct amd64_irq_ack acknowledgements[AMD64_IRQ_ACK_DEPTH];
	unsigned acknowledgement_depth;
	struct amd64_task *running_task;
	hal_space_t current_space;

	/*
	 * The task whose context is still being left on this CPU.
	 *
	 * hal_task_context_switch() records the departing task here before
	 * the stack switch and the arriving context, whether resumed or
	 * started for the first time, marks it as no longer running.  Until
	 * then another CPU must not resume it: its stack is still in use.
	 */
	struct amd64_task *switching_from;
};

/* The SYSCALL entry reads these through GS at fixed offsets. */
_Static_assert(__builtin_offsetof(struct amd64_percpu, syscall_rsp) ==
	       AMD64_PERCPU_SYSCALL_RSP, "amd64 SYSCALL stack offset");
_Static_assert(__builtin_offsetof(struct amd64_percpu, syscall_scratch) ==
	       AMD64_PERCPU_SYSCALL_SCRATCH, "amd64 SYSCALL scratch offset");

/* amd64_percpu_current() reads self through GS at offset 0. */
_Static_assert(__builtin_offsetof(struct amd64_percpu, self) == 0,
	       "amd64 per-CPU self pointer must be the first field");

/*
 * hal_task_get_current() reads running_task through GS in one load.  An
 * enumerator, fixed here, because task.c defines running_task as a macro.
 */
enum {
	AMD64_PERCPU_RUNNING_TASK =
	    __builtin_offsetof(struct amd64_percpu, running_task)
};

void prekern_amd64_percpu_bootstrap(void);
struct amd64_percpu *amd64_percpu_get(hal_cpu_id_t cpu);
struct amd64_percpu *amd64_percpu_current(void);
void amd64_percpu_select(struct amd64_percpu *cpu);
hal_irq_ack_t amd64_irq_ack_begin(uint32_t vector, int irq);

#endif
