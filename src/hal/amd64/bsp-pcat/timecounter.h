/* Private amd64 PC/AT timecounter ownership and SMP validation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_AMD64_TIMECOUNTER_H
#define ZEDBSD_HAL_AMD64_TIMECOUNTER_H

#include <hal/types.h>
#include "timecounter-policy.h"

struct amd64_percpu;

enum amd64_tsc_frequency_policy_result
amd64_timecounter_bsp_prepare(void);
uint64_t amd64_timecounter_sample_serialized(void);
void amd64_timecounter_pit_complete(uint64_t start, uint64_t end,
	uint64_t reference_hz, uint64_t reference_ticks);
void amd64_timecounter_bsp_abort(void);
bool amd64_timecounter_bsp_candidate_valid(void);
void amd64_timecounter_ap_probe(struct amd64_percpu *cpu);
bool amd64_timecounter_bsp_validate_ap(struct amd64_percpu *cpu);
void amd64_timecounter_ap_runtime_validate(struct amd64_percpu *cpu);
void amd64_timecounter_complete_boot_validation(bool complete_set_valid,
	unsigned cpu_count);
void amd64_timecounter_release_boot_validation(void);
bool amd64_timecounter_read(uint64_t *counter, uint64_t *frequency_hz);

#endif
