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

#ifndef ZEDBSD_HAL_AMD64_PERCPU_H
#define ZEDBSD_HAL_AMD64_PERCPU_H

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
