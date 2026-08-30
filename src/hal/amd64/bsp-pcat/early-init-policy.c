/* Pure policy helpers for amd64 PC/AT early interrupt bring-up. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "early-init-policy.h"

#define CPUID_APIC             (1U << 9)
#define CPUID_X2APIC           (1U << 21)
#define APIC_BASE_X2APIC       (1ULL << 10)
#define APIC_BASE_ENABLE       (1ULL << 11)
#define APIC_BASE_ADDRESS_MASK 0x000ffffffffff000ULL
#define IOAPIC_MAX_REDIRECTIONS 120U

enum amd64_apic_policy_result
amd64_apic_policy_evaluate(const struct amd64_apic_policy_input *input,
	uint32_t *apic_id)
{
	uint64_t mode, base;
	uint32_t id;

	if (input == NULL || apic_id == NULL)
		return AMD64_APIC_POLICY_INVALID_MODE;
	id = input->cpuid1_ebx >> 24;
	*apic_id = id;
	if ((input->cpuid1_edx & CPUID_APIC) == 0)
		return AMD64_APIC_POLICY_NO_CPUID_APIC;
	if (input->lock_status_valid &&
	    (input->xapic_disable_status & 1U) != 0)
		return AMD64_APIC_POLICY_LEGACY_LOCKED;
	mode = input->apic_base & (APIC_BASE_ENABLE | APIC_BASE_X2APIC);
	if (mode == 0)
		return AMD64_APIC_POLICY_DISABLED;
	if (mode == APIC_BASE_X2APIC)
		return AMD64_APIC_POLICY_INVALID_MODE;
	if (mode == (APIC_BASE_ENABLE | APIC_BASE_X2APIC))
		return (input->cpuid1_ecx & CPUID_X2APIC) != 0 ?
		    AMD64_APIC_POLICY_X2APIC :
		    AMD64_APIC_POLICY_X2APIC_UNSUPPORTED;
	if (input->madt_base == 0 || (input->madt_base & 0xfffU) != 0)
		return AMD64_APIC_POLICY_INVALID_MADT_BASE;
	base = input->apic_base & APIC_BASE_ADDRESS_MASK;
	if (base != input->madt_base)
		return AMD64_APIC_POLICY_BASE_MISMATCH;
	if (input->expected_apic_id != AMD64_APIC_EXPECT_ANY) {
		if (input->expected_apic_id > 255U)
			return AMD64_APIC_POLICY_ID_RANGE;
		if (id != input->expected_apic_id)
			return AMD64_APIC_POLICY_ID_MISMATCH;
	}
	return AMD64_APIC_POLICY_OK;
}

const char *
amd64_apic_policy_result_name(enum amd64_apic_policy_result result)
{
	switch (result) {
	case AMD64_APIC_POLICY_OK:
		return "active-xapic";
	case AMD64_APIC_POLICY_NO_CPUID_APIC:
		return "cpuid-no-apic";
	case AMD64_APIC_POLICY_DISABLED:
		return "apic-disabled";
	case AMD64_APIC_POLICY_INVALID_MODE:
		return "apic-mode-invalid";
	case AMD64_APIC_POLICY_X2APIC:
		return "x2apic-active";
	case AMD64_APIC_POLICY_X2APIC_UNSUPPORTED:
		return "x2apic-not-enumerated";
	case AMD64_APIC_POLICY_LEGACY_LOCKED:
		return "legacy-xapic-locked";
	case AMD64_APIC_POLICY_INVALID_MADT_BASE:
		return "madt-base-invalid";
	case AMD64_APIC_POLICY_BASE_MISMATCH:
		return "apic-base-mismatch";
	case AMD64_APIC_POLICY_ID_RANGE:
		return "apic-id-over-255";
	case AMD64_APIC_POLICY_ID_MISMATCH:
		return "apic-id-mismatch";
	default:
		return "unknown";
	}
}

void
amd64_pit_poll_init(struct amd64_pit_poll *poll, unsigned limit)
{
	if (poll != NULL)
		poll->remaining = limit;
}

enum amd64_pit_poll_result
amd64_pit_poll_step(struct amd64_pit_poll *poll, uint8_t port61,
	int expected_high)
{
	if (((port61 & 0x20U) != 0) == (expected_high != 0))
		return AMD64_PIT_POLL_READY;
	if (poll == NULL || poll->remaining == 0)
		return AMD64_PIT_POLL_TIMEOUT;
	poll->remaining--;
	return poll->remaining == 0 ? AMD64_PIT_POLL_TIMEOUT :
	    AMD64_PIT_POLL_WAIT;
}

enum amd64_ioapic_policy_result
amd64_ioapic_policy_evaluate(uint32_t version, uint32_t gsi_base,
	const struct amd64_ioapic_range *previous, unsigned previous_count,
	uint32_t *redirections)
{
	uint32_t count;
	unsigned index;

	if (redirections == NULL)
		return AMD64_IOAPIC_POLICY_INVALID_VERSION;
	*redirections = 0;
	if ((previous_count != 0 && previous == NULL) ||
	    version == UINT32_MAX || (version & 0xffU) == 0 ||
	    (version & 0xffU) == 0xffU)
		return AMD64_IOAPIC_POLICY_INVALID_VERSION;
	count = ((version >> 16) & 0xffU) + 1U;
	*redirections = count;
	if (count > IOAPIC_MAX_REDIRECTIONS)
		return AMD64_IOAPIC_POLICY_INVALID_PIN_COUNT;
	if (gsi_base > UINT32_MAX - count)
		return AMD64_IOAPIC_POLICY_GSI_OVERFLOW;
	for (index = 0; index < previous_count; index++) {
		uint32_t end = gsi_base + count;
		uint32_t previous_end = previous[index].gsi_base +
		    previous[index].redirections;

		if (gsi_base < previous_end && end > previous[index].gsi_base)
			return AMD64_IOAPIC_POLICY_GSI_OVERLAP;
	}
	return AMD64_IOAPIC_POLICY_OK;
}

const char *
amd64_ioapic_policy_result_name(enum amd64_ioapic_policy_result result)
{
	switch (result) {
	case AMD64_IOAPIC_POLICY_OK:
		return "valid";
	case AMD64_IOAPIC_POLICY_INVALID_VERSION:
		return "invalid-version";
	case AMD64_IOAPIC_POLICY_INVALID_PIN_COUNT:
		return "invalid-pin-count";
	case AMD64_IOAPIC_POLICY_GSI_OVERFLOW:
		return "gsi-overflow";
	case AMD64_IOAPIC_POLICY_GSI_OVERLAP:
		return "gsi-overlap";
	default:
		return "unknown";
	}
}
