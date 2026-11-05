/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 local APIC control and delivery contract.
 */

#ifndef KERN_HAL_AMD64_LAPIC_H
#define KERN_HAL_AMD64_LAPIC_H

#include <hal/types.h>

struct amd64_acpi_info;

/*
 * The CPU-local interrupt sources hal_cpu_idle_suspend() quiets on its CPU:
 * each LVT entry's value before the wait, and whether the
 * entry exists on this local APIC (the thermal and the corrected
 * machine-check entries are optional).  One lives on the stack of the call.
 */
struct amd64_lapic_quiet {
	uint32_t cmci;
	uint32_t thermal;
	uint32_t performance;
	uint32_t lint0;
	uint32_t lint1;
	uint32_t error;
	unsigned has_cmci;
	unsigned has_thermal;
	unsigned has_performance;
};


int prekern_amd64_lapic_init(const struct amd64_acpi_info *acpi);
int amd64_lapic_init_secondary(uint32_t expected_apic_id, unsigned *failure_reason);
uint32_t amd64_lapic_id(void);
void amd64_lapic_eoi(void);
int amd64_lapic_timer_start(void);
void amd64_lapic_timer_stop(void);
void amd64_lapic_timer_tick(hal_cpu_id_t cpu);
void amd64_lapic_quiet_sources(struct amd64_lapic_quiet *saved);
void amd64_lapic_restore_sources(const struct amd64_lapic_quiet *saved);
int amd64_lapic_send_init(uint32_t apic_id);
int amd64_lapic_send_startup(uint32_t apic_id, uint8_t vector);
int amd64_lapic_notify(uint32_t apic_id);
int amd64_lapic_send_vector(uint32_t apic_id, uint8_t vector);
void amd64_lapic_panic_all(void) __attribute__((noreturn));

#endif
