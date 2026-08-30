/* BR-T52 host regression for amd64 early APIC/PIT policy. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "src/hal/amd64/bsp-pcat/early-init-policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CPUID_APIC   (1U << 9)
#define CPUID_X2APIC (1U << 21)
#define APIC_ENABLE  (1ULL << 11)
#define APIC_X2      (1ULL << 10)

static struct amd64_apic_policy_input
valid_input(void)
{
	struct amd64_apic_policy_input input = {
		.cpuid1_ebx = 6U << 24,
		.cpuid1_ecx = CPUID_X2APIC,
		.cpuid1_edx = CPUID_APIC,
		.apic_base = 0xfee00000ULL | APIC_ENABLE | (1U << 8),
		.madt_base = 0xfee00000ULL,
		.expected_apic_id = AMD64_APIC_EXPECT_ANY
	};

	return input;
}

static void
expect_policy(struct amd64_apic_policy_input *input,
	enum amd64_apic_policy_result expected)
{
	uint32_t id = UINT32_MAX;
	enum amd64_apic_policy_result actual =
	    amd64_apic_policy_evaluate(input, &id);

	assert(actual == expected);
	assert(id == 6U);
	assert(strlen(amd64_apic_policy_result_name(actual)) != 0);
}

static void
test_apic_policy(void)
{
	struct amd64_apic_policy_input input = valid_input();
	uint32_t id;

	expect_policy(&input, AMD64_APIC_POLICY_OK);
	assert(amd64_apic_policy_evaluate(&input, &id) ==
	    AMD64_APIC_POLICY_OK && id == 6U);
	input.expected_apic_id = 6U;
	expect_policy(&input, AMD64_APIC_POLICY_OK);

	input = valid_input();
	input.cpuid1_edx = 0;
	expect_policy(&input, AMD64_APIC_POLICY_NO_CPUID_APIC);

	input = valid_input();
	input.apic_base &= ~APIC_ENABLE;
	expect_policy(&input, AMD64_APIC_POLICY_DISABLED);

	input = valid_input();
	input.apic_base = 0xfee00000ULL | APIC_X2;
	expect_policy(&input, AMD64_APIC_POLICY_INVALID_MODE);

	input = valid_input();
	input.apic_base |= APIC_X2;
	expect_policy(&input, AMD64_APIC_POLICY_X2APIC);

	input = valid_input();
	input.cpuid1_ecx = 0;
	input.apic_base |= APIC_X2;
	expect_policy(&input, AMD64_APIC_POLICY_X2APIC_UNSUPPORTED);

	input = valid_input();
	input.lock_status_valid = 1;
	input.xapic_disable_status = 1;
	expect_policy(&input, AMD64_APIC_POLICY_LEGACY_LOCKED);

	input = valid_input();
	input.xapic_disable_status = 1;
	expect_policy(&input, AMD64_APIC_POLICY_OK);

	input = valid_input();
	input.madt_base = 0;
	expect_policy(&input, AMD64_APIC_POLICY_INVALID_MADT_BASE);
	input.madt_base = 0xfee00001ULL;
	expect_policy(&input, AMD64_APIC_POLICY_INVALID_MADT_BASE);

	input = valid_input();
	input.madt_base = 0xfec00000ULL;
	expect_policy(&input, AMD64_APIC_POLICY_BASE_MISMATCH);

	input = valid_input();
	input.expected_apic_id = 256U;
	expect_policy(&input, AMD64_APIC_POLICY_ID_RANGE);
	input.expected_apic_id = 7U;
	expect_policy(&input, AMD64_APIC_POLICY_ID_MISMATCH);

	assert(amd64_apic_policy_evaluate(NULL, &id) ==
	    AMD64_APIC_POLICY_INVALID_MODE);
	assert(amd64_apic_policy_evaluate(&input, NULL) ==
	    AMD64_APIC_POLICY_INVALID_MODE);
}

static void
test_pit_poll(void)
{
	struct amd64_pit_poll poll;

	amd64_pit_poll_init(&poll, 3);
	assert(amd64_pit_poll_step(&poll, 0x20U, 1) ==
	    AMD64_PIT_POLL_READY);
	assert(poll.remaining == 3);

	amd64_pit_poll_init(&poll, 3);
	assert(amd64_pit_poll_step(&poll, 0x20U, 0) ==
	    AMD64_PIT_POLL_WAIT);
	assert(poll.remaining == 2);
	assert(amd64_pit_poll_step(&poll, 0, 0) ==
	    AMD64_PIT_POLL_READY);

	amd64_pit_poll_init(&poll, 2);
	assert(amd64_pit_poll_step(&poll, 0, 1) ==
	    AMD64_PIT_POLL_WAIT);
	assert(amd64_pit_poll_step(&poll, 0, 1) ==
	    AMD64_PIT_POLL_TIMEOUT);
	assert(poll.remaining == 0);
	assert(amd64_pit_poll_step(&poll, 0x20U, 1) ==
	    AMD64_PIT_POLL_READY);

	amd64_pit_poll_init(&poll, 0);
	assert(amd64_pit_poll_step(&poll, 0, 1) ==
	    AMD64_PIT_POLL_TIMEOUT);
	assert(amd64_pit_poll_step(NULL, 0, 1) ==
	    AMD64_PIT_POLL_TIMEOUT);
}

static void
expect_ioapic(uint32_t version, uint32_t gsi_base,
	const struct amd64_ioapic_range *previous, unsigned previous_count,
	enum amd64_ioapic_policy_result expected, uint32_t expected_pins)
{
	uint32_t pins = 0;
	enum amd64_ioapic_policy_result actual =
	    amd64_ioapic_policy_evaluate(version, gsi_base, previous,
	    previous_count, &pins);

	assert(actual == expected);
	assert(pins == expected_pins);
	assert(strlen(amd64_ioapic_policy_result_name(actual)) != 0);
}

static void
test_ioapic_policy(void)
{
	const struct amd64_ioapic_range previous[] = {
		{ .gsi_base = 0, .redirections = 24 },
		{ .gsi_base = 48, .redirections = 24 }
	};

	expect_ioapic(0x00170011U, 0, NULL, 0,
	    AMD64_IOAPIC_POLICY_OK, 24);
	expect_ioapic(UINT32_MAX, 0, NULL, 0,
	    AMD64_IOAPIC_POLICY_INVALID_VERSION, 0);
	expect_ioapic(0x00170000U, 0, NULL, 0,
	    AMD64_IOAPIC_POLICY_INVALID_VERSION, 0);
	expect_ioapic(0x00780011U, 0, NULL, 0,
	    AMD64_IOAPIC_POLICY_INVALID_PIN_COUNT, 121);
	expect_ioapic(0x00170011U, UINT32_MAX - 22U, NULL, 0,
	    AMD64_IOAPIC_POLICY_GSI_OVERFLOW, 24);
	expect_ioapic(0x00170011U, 24, previous, 2,
	    AMD64_IOAPIC_POLICY_OK, 24);
	expect_ioapic(0x00170011U, 16, previous, 2,
	    AMD64_IOAPIC_POLICY_GSI_OVERLAP, 24);
	expect_ioapic(0x00170011U, 40, previous, 2,
	    AMD64_IOAPIC_POLICY_GSI_OVERLAP, 24);
	expect_ioapic(0x00170011U, 72, previous, 2,
	    AMD64_IOAPIC_POLICY_OK, 24);
}

int
main(void)
{
	test_apic_policy();
	test_pit_poll();
	test_ioapic_policy();
	puts("BR-T52 amd64 early-init policy test: PASS");
	return 0;
}
