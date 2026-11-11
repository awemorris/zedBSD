/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure policy helpers for amd64 PC/AT early interrupt bring-up.
 */

#include "early-init-policy.h"

#define CPUID_APIC              (1U << 9)
#define CPUID_X2APIC            (1U << 21)
#define APIC_BASE_X2APIC        (1ULL << 10)
#define APIC_BASE_ENABLE        (1ULL << 11)
#define APIC_BASE_ADDRESS_MASK  0x000ffffffffff000ULL
#define IOAPIC_MAX_REDIRECTIONS 120U

/*
 * Classifies whether one CPU can use the required legacy xAPIC mode.
 */
enum amd64_apic_policy_result
amd64_apic_policy_evaluate(
	const struct amd64_apic_policy_input *input,
	uint32_t *apic_id)
{
	uint64_t mode;
	uint64_t base;
	uint32_t id;

	/* Requires both policy input and an APIC-identifier output. */
	if (input == NULL || apic_id == NULL)
		return AMD64_APIC_POLICY_INVALID_MODE;

	/* Publishes the CPUID xAPIC identifier before classifying the mode. */
	id = input->cpuid1_ebx >> 24;
	*apic_id = id;

	/* Requires architectural APIC support from CPUID leaf one. */
	if ((input->cpuid1_edx & CPUID_APIC) == 0)
		return AMD64_APIC_POLICY_NO_CPUID_APIC;

	/* Rejects firmware which locked legacy xAPIC access off. */
	if (input->lock_status_valid &&
	    (input->xapic_disable_status & 1U) != 0)
		return AMD64_APIC_POLICY_LEGACY_LOCKED;

	/* Classifies the enable and x2APIC bits as an architectural pair. */
	mode = input->apic_base & (APIC_BASE_ENABLE | APIC_BASE_X2APIC);
	if (mode == 0)
		return AMD64_APIC_POLICY_DISABLED;
	if (mode == APIC_BASE_X2APIC)
		return AMD64_APIC_POLICY_INVALID_MODE;
	if (mode == (APIC_BASE_ENABLE | APIC_BASE_X2APIC)) {
		/* Distinguishes valid x2APIC from inconsistent enumeration. */
		if ((input->cpuid1_ecx & CPUID_X2APIC) != 0)
			return AMD64_APIC_POLICY_X2APIC;
		return AMD64_APIC_POLICY_X2APIC_UNSUPPORTED;
	}

	/* Requires an aligned MADT local-APIC address. */
	if (input->madt_base == 0 || (input->madt_base & 0xfffU) != 0)
		return AMD64_APIC_POLICY_INVALID_MADT_BASE;

	/* Requires the enabled hardware base to match the MADT base. */
	base = input->apic_base & APIC_BASE_ADDRESS_MASK;
	if (base != input->madt_base)
		return AMD64_APIC_POLICY_BASE_MISMATCH;

	/* Applies a caller-supplied expected xAPIC identifier when present. */
	if (input->expected_apic_id != AMD64_APIC_EXPECT_ANY) {
		/* Rejects identifiers which cannot be represented by xAPIC. */
		if (input->expected_apic_id > 255U)
			return AMD64_APIC_POLICY_ID_RANGE;

		/* Requires CPUID to identify the expected processor. */
		if (id != input->expected_apic_id)
			return AMD64_APIC_POLICY_ID_MISMATCH;
	}

	/* Accepts an enabled and consistent legacy xAPIC configuration. */
	return AMD64_APIC_POLICY_OK;
}

/*
 * Returns a stable diagnostic name for an xAPIC policy result.
 */
const char *
amd64_apic_policy_result_name(
	enum amd64_apic_policy_result result)
{
	/* Maps every defined result and preserves an unknown fallback. */
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

/*
 * Initializes a bounded PIT polling budget.
 */
void
amd64_pit_poll_init(
	struct amd64_pit_poll *poll,
	unsigned limit)
{
	/* Publishes the caller's limit when a poll state was supplied. */
	if (poll != NULL)
		poll->remaining = limit;
}

/*
 * Advances a bounded PIT output-level poll by one sample.
 */
enum amd64_pit_poll_result
amd64_pit_poll_step(
	struct amd64_pit_poll *poll,
	uint8_t port61,
	int expected_high)
{
	/* Accepts a sampled output which already has the expected level. */
	if (((port61 & 0x20U) != 0) == (expected_high != 0))
		return AMD64_PIT_POLL_READY;

	/* Times out without consuming an absent or exhausted poll state. */
	if (poll == NULL || poll->remaining == 0)
		return AMD64_PIT_POLL_TIMEOUT;

	/* Consumes one failed sample from the bounded budget. */
	poll->remaining--;
	if (poll->remaining == 0)
		return AMD64_PIT_POLL_TIMEOUT;

	/* Requests another sample while budget remains. */
	return AMD64_PIT_POLL_WAIT;
}

/*
 * Validates one I/O APIC's version and nonoverlapping GSI range.
 */
enum amd64_ioapic_policy_result
amd64_ioapic_policy_evaluate(
	uint32_t version,
	uint32_t gsi_base,
	const struct amd64_ioapic_range *previous,
	unsigned previous_count,
	uint32_t *redirections)
{
	uint32_t count;
	uint32_t end;
	uint32_t previous_end;
	unsigned index;

	/* Requires an output for the decoded redirection count. */
	if (redirections == NULL)
		return AMD64_IOAPIC_POLICY_INVALID_VERSION;
	*redirections = 0;

	/* Rejects missing prior ranges and nonsensical version samples. */
	if ((previous_count != 0 && previous == NULL) ||
	    version == UINT32_MAX || (version & 0xffU) == 0 ||
	    (version & 0xffU) == 0xffU)
		return AMD64_IOAPIC_POLICY_INVALID_VERSION;

	/* Decodes and reports the hardware redirection-entry count. */
	count = ((version >> 16) & 0xffU) + 1U;
	*redirections = count;
	if (count > IOAPIC_MAX_REDIRECTIONS)
		return AMD64_IOAPIC_POLICY_INVALID_PIN_COUNT;

	/* Rejects a GSI range whose exclusive end would overflow. */
	if (gsi_base > UINT32_MAX - count)
		return AMD64_IOAPIC_POLICY_GSI_OVERFLOW;

	/* Compares the candidate range with every accepted earlier range. */
	for (index = 0; index < previous_count; index++) {
		end = gsi_base + count;
		previous_end = previous[index].gsi_base +
		    previous[index].redirections;

		/* Rejects any intersection between the half-open ranges. */
		if (gsi_base < previous_end &&
		    end > previous[index].gsi_base)
			return AMD64_IOAPIC_POLICY_GSI_OVERLAP;
	}

	/* Accepts a bounded, representable, nonoverlapping GSI range. */
	return AMD64_IOAPIC_POLICY_OK;
}

/*
 * Returns a stable diagnostic name for an I/O APIC policy result.
 */
const char *
amd64_ioapic_policy_result_name(
	enum amd64_ioapic_policy_result result)
{
	/* Maps every defined result and preserves an unknown fallback. */
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
