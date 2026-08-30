/* Pure policy helpers for amd64 PC/AT early interrupt bring-up. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_AMD64_EARLY_INIT_POLICY_H
#define ZEDBSD_HAL_AMD64_EARLY_INIT_POLICY_H

#include <hal/types.h>

#define AMD64_APIC_EXPECT_ANY UINT32_MAX

enum amd64_apic_policy_result {
	AMD64_APIC_POLICY_OK = 0,
	AMD64_APIC_POLICY_NO_CPUID_APIC,
	AMD64_APIC_POLICY_DISABLED,
	AMD64_APIC_POLICY_INVALID_MODE,
	AMD64_APIC_POLICY_X2APIC,
	AMD64_APIC_POLICY_X2APIC_UNSUPPORTED,
	AMD64_APIC_POLICY_LEGACY_LOCKED,
	AMD64_APIC_POLICY_INVALID_MADT_BASE,
	AMD64_APIC_POLICY_BASE_MISMATCH,
	AMD64_APIC_POLICY_ID_RANGE,
	AMD64_APIC_POLICY_ID_MISMATCH
};

struct amd64_apic_policy_input {
	uint32_t cpuid1_ebx;
	uint32_t cpuid1_ecx;
	uint32_t cpuid1_edx;
	uint64_t apic_base;
	uint64_t madt_base;
	uint64_t xapic_disable_status;
	uint32_t expected_apic_id;
	int lock_status_valid;
};

enum amd64_apic_policy_result amd64_apic_policy_evaluate(
	const struct amd64_apic_policy_input *input, uint32_t *apic_id);
const char *amd64_apic_policy_result_name(
	enum amd64_apic_policy_result result);

enum amd64_pit_poll_result {
	AMD64_PIT_POLL_WAIT = 0,
	AMD64_PIT_POLL_READY,
	AMD64_PIT_POLL_TIMEOUT
};

struct amd64_pit_poll {
	unsigned remaining;
};

void amd64_pit_poll_init(struct amd64_pit_poll *poll, unsigned limit);
enum amd64_pit_poll_result amd64_pit_poll_step(
	struct amd64_pit_poll *poll, uint8_t port61, int expected_high);

enum amd64_ioapic_policy_result {
	AMD64_IOAPIC_POLICY_OK = 0,
	AMD64_IOAPIC_POLICY_INVALID_VERSION,
	AMD64_IOAPIC_POLICY_INVALID_PIN_COUNT,
	AMD64_IOAPIC_POLICY_GSI_OVERFLOW,
	AMD64_IOAPIC_POLICY_GSI_OVERLAP
};

struct amd64_ioapic_range {
	uint32_t gsi_base;
	uint32_t redirections;
};

enum amd64_ioapic_policy_result amd64_ioapic_policy_evaluate(
	uint32_t version, uint32_t gsi_base,
	const struct amd64_ioapic_range *previous, unsigned previous_count,
	uint32_t *redirections);
const char *amd64_ioapic_policy_result_name(
	enum amd64_ioapic_policy_result result);

#endif
