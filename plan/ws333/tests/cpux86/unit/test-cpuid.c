/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the CPUID decoder: every leaf it reads, built from the
 * bit positions of the Intel SDM Vol. 1 rev. 093 tables by hand (regbuild.h
 * and plain shifts), the error returns with the unchanged-output promise,
 * and 100000 random register sets that must never make a decoder leave the
 * values its header lists.  The expected values are worked out by hand.
 */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cpux86-cpuid.h"
#include "regbuild.h"

/* The pattern a test writes into an output before a call that must not change it. */
#define SENTINEL_BYTE	0xA5

/*
 * Fills one set of CPUID registers.
 */
static void
regs_set(
	struct cpux86_cpuid_regs *regs,
	uint32_t eax,
	uint32_t ebx,
	uint32_t ecx,
	uint32_t edx)
{
	/* Places the four registers. */
	regs->eax = eax;
	regs->ebx = ebx;
	regs->ecx = ecx;
	regs->edx = edx;
}

/*
 * Builds the leaf 00H registers of a processor whose vendor string is
 * "GenuineIntel" except for one register's value.
 */
static void
leaf0_with(
	struct cpux86_cpuid_regs *regs,
	uint32_t max_leaf,
	uint32_t ebx,
	uint32_t edx,
	uint32_t ecx)
{
	/* The vendor string registers are given by the caller. */
	regs->eax = max_leaf;
	regs->ebx = ebx;
	regs->edx = edx;
	regs->ecx = ecx;
}

/*
 * Checks the identification of the processor from leaves 00H and 01H.
 */
static void
test_identity(void)
{
	struct cpux86_cpuid_regs leaf0;
	struct cpux86_cpuid_regs leaf1;
	struct cpux86_identity id;
	struct cpux86_identity before;
	uint32_t family;

	/* Family 6, model 9AH (extended model 9, model AH), stepping 4. */
	regbuild_leaf0_intel(&leaf0, 0x20U);
	regs_set(&leaf1, regbuild_leaf1_eax(6U, 0xAU, 4U, 0U, 0U, 9U), 0U, 0U, 0U);
	memset(&id, SENTINEL_BYTE, sizeof(id));
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.vendor == CPUX86_VENDOR_INTEL);
	CHECK(id.max_basic_leaf == 0x20U);
	CHECK(id.max_extended_leaf == 0U);
	CHECK(id.display_family == 6U);
	CHECK(id.display_model == 0x9AU);
	CHECK(id.stepping == 4U);
	CHECK(id.processor_type == 0U);

	/* The real EAX 0x000906A4 of the 5330's first leaf: model 0x9A stepping 4. */
	regs_set(&leaf1, 0x000906A4U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.display_family == 6U);
	CHECK(id.display_model == 0x9AU);
	CHECK(id.stepping == 4U);

	/* Family 0FH adds the extended family: 0FH + 1 = 10H, and the extended model is prepended. */
	regs_set(&leaf1, regbuild_leaf1_eax(0xFU, 3U, 7U, 2U, 1U, 2U), 0U, 0U, 0U);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.display_family == 0x10U);
	CHECK(id.display_model == 0x23U);
	CHECK(id.stepping == 7U);
	CHECK(id.processor_type == 2U);

	/* The largest extended family adds up to 0FH + FFH = 10EH. */
	regs_set(&leaf1, regbuild_leaf1_eax(0xFU, 0xFU, 0xFU, 3U, 0xFFU, 0xFU), 0U, 0U, 0U);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.display_family == 0x10EU);
	CHECK(id.display_model == 0xFFU);
	CHECK(id.processor_type == 3U);

	/* Family 5 ignores both extended fields. */
	regs_set(&leaf1, regbuild_leaf1_eax(5U, 7U, 1U, 0U, 0x20U, 0xFU), 0U, 0U, 0U);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.display_family == 5U);
	CHECK(id.display_model == 7U);

	/* Families 6 and 0FH read the extended model; every other family 0..15 does not. */
	for (family = 0U;
	     family < 16U;
	     family++) {
		regs_set(&leaf1, regbuild_leaf1_eax(family, 0x3U, 2U, 0U, 0x11U, 0xCU), 0U, 0U, 0U);
		CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
		if (family == 6U) {
			CHECK(id.display_family == 6U);
			CHECK(id.display_model == 0xC3U);
		} else if (family == 0xFU) {
			CHECK(id.display_family == 0x20U);
			CHECK(id.display_model == 0xC3U);
		} else {
			CHECK(id.display_family == family);
			CHECK(id.display_model == 3U);
		}
	}

	/* Reserved bits 15:14 and 31:28 of EAX are not part of any field. */
	regs_set(&leaf1, 0xF000C000U | regbuild_leaf1_eax(6U, 0xAU, 4U, 0U, 0U, 9U), 0U, 0U, 0U);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.display_family == 6U);
	CHECK(id.display_model == 0x9AU);
	CHECK(id.stepping == 4U);
	CHECK(id.processor_type == 0U);

	/* A vendor string that differs in one character of one register is UNKNOWN. */
	regs_set(&leaf1, 0x000906A4U, 0U, 0U, 0U);
	leaf0_with(&leaf0, 0x20U, 0x756e6547U, 0x49656e69U, 0x6c65746eU);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.vendor == CPUX86_VENDOR_INTEL);
	leaf0_with(&leaf0, 0x20U, 0x756e6548U, 0x49656e69U, 0x6c65746eU);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.vendor == CPUX86_VENDOR_UNKNOWN);
	leaf0_with(&leaf0, 0x20U, 0x756e6547U, 0x49656e6aU, 0x6c65746eU);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.vendor == CPUX86_VENDOR_UNKNOWN);
	leaf0_with(&leaf0, 0x20U, 0x756e6547U, 0x49656e69U, 0x6c65746fU);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.vendor == CPUX86_VENDOR_UNKNOWN);
	CHECK(id.display_model == 0x9AU);
	leaf0_with(&leaf0, 0x20U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.vendor == CPUX86_VENDOR_UNKNOWN);

	/* The maximum leaf is copied as it is, and an earlier extended maximum is cleared. */
	regbuild_leaf0_intel(&leaf0, 0xFFFFFFFFU);
	id.max_extended_leaf = 0x80000008U;
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.max_basic_leaf == 0xFFFFFFFFU);
	CHECK(id.max_extended_leaf == 0U);
	regbuild_leaf0_intel(&leaf0, 1U);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == 0);
	CHECK(id.max_basic_leaf == 1U);

	/* MAX_LEAF 0 means leaf 01H does not exist: ENOTSUP and the output is unchanged. */
	regbuild_leaf0_intel(&leaf0, 0U);
	memset(&id, SENTINEL_BYTE, sizeof(id));
	memcpy(&before, &id, sizeof(id));
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, &id) == ENOTSUP);
	CHECK(memcmp(&before, &id, sizeof(id)) == 0);

	/* A NULL pointer is EINVAL and leaves the output alone. */
	regbuild_leaf0_intel(&leaf0, 0x20U);
	CHECK(cpux86_cpuid_identity(NULL, &leaf1, &id) == EINVAL);
	CHECK(cpux86_cpuid_identity(&leaf0, NULL, &id) == EINVAL);
	CHECK(cpux86_cpuid_identity(&leaf0, &leaf1, NULL) == EINVAL);
	CHECK(memcmp(&before, &id, sizeof(id)) == 0);
}

/*
 * Checks the extended range and the question whether a leaf may be read.
 */
static void
test_range(void)
{
	struct cpux86_cpuid_regs leaf;
	struct cpux86_identity id;
	struct cpux86_identity before;

	/* A maximum below 80000000H is not an extended maximum: 0. */
	memset(&id, 0, sizeof(id));
	regs_set(&leaf, 0x7FFFFFFFU, 0U, 0U, 0U);
	id.max_extended_leaf = 0x80000008U;
	CHECK(cpux86_cpuid_extended_range(&leaf, &id) == 0);
	CHECK(id.max_extended_leaf == 0U);
	regs_set(&leaf, 0U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_extended_range(&leaf, &id) == 0);
	CHECK(id.max_extended_leaf == 0U);

	/* 80000000H and above are taken as they are. */
	regs_set(&leaf, 0x80000000U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_extended_range(&leaf, &id) == 0);
	CHECK(id.max_extended_leaf == 0x80000000U);
	regs_set(&leaf, 0x80000008U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_extended_range(&leaf, &id) == 0);
	CHECK(id.max_extended_leaf == 0x80000008U);
	regs_set(&leaf, 0xFFFFFFFFU, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_extended_range(&leaf, &id) == 0);
	CHECK(id.max_extended_leaf == 0xFFFFFFFFU);

	/* The other fields of the identity are not touched. */
	id.vendor = 1U;
	id.max_basic_leaf = 0x1BU;
	id.display_model = 0x9AU;
	regs_set(&leaf, 0x80000004U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_extended_range(&leaf, &id) == 0);
	CHECK(id.vendor == 1U);
	CHECK(id.max_basic_leaf == 0x1BU);
	CHECK(id.display_model == 0x9AU);

	/* A NULL pointer is EINVAL and changes nothing. */
	memcpy(&before, &id, sizeof(id));
	CHECK(cpux86_cpuid_extended_range(NULL, &id) == EINVAL);
	CHECK(cpux86_cpuid_extended_range(&leaf, NULL) == EINVAL);
	CHECK(memcmp(&before, &id, sizeof(id)) == 0);

	/* A basic leaf is readable up to and including the maximum. */
	memset(&id, 0, sizeof(id));
	id.max_basic_leaf = 0x1FU;
	CHECK(cpux86_cpuid_has_leaf(&id, 0U) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x1EU) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x1FU) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x20U) == 0);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x7FFFFFFFU) == 0);

	/* With extended maximum 0 no extended leaf is readable, even 80000000H. */
	CHECK(cpux86_cpuid_has_leaf(&id, 0x80000000U) == 0);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x80000001U) == 0);
	CHECK(cpux86_cpuid_has_leaf(&id, 0xFFFFFFFFU) == 0);

	/* With extended maximum 80000008H the extended leaves up to it are readable. */
	id.max_extended_leaf = 0x80000008U;
	CHECK(cpux86_cpuid_has_leaf(&id, 0x80000000U) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x80000007U) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x80000008U) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x80000009U) == 0);
	CHECK(cpux86_cpuid_has_leaf(&id, 0xFFFFFFFFU) == 0);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x1FU) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x20U) == 0);

	/* A basic maximum above 80000000H does not make an extended leaf basic. */
	id.max_basic_leaf = 0xFFFFFFFFU;
	id.max_extended_leaf = 0U;
	CHECK(cpux86_cpuid_has_leaf(&id, 0x7FFFFFFFU) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 0x80000000U) == 0);

	/* A basic maximum of 0 still allows leaf 0 itself. */
	id.max_basic_leaf = 0U;
	CHECK(cpux86_cpuid_has_leaf(&id, 0U) == 1);
	CHECK(cpux86_cpuid_has_leaf(&id, 1U) == 0);

	/* A NULL identity allows nothing. */
	CHECK(cpux86_cpuid_has_leaf(NULL, 0U) == 0);
	CHECK(cpux86_cpuid_has_leaf(NULL, 0x80000000U) == 0);
}

/*
 * Checks the feature bits and APIC numbers of leaf 01H.
 */
static void
test_features(void)
{
	struct cpux86_cpuid_regs leaf1;
	struct cpux86_features features;
	struct cpux86_features before;
	uint32_t bit;
	uint32_t expected;
	uint32_t named;

	/* Each named ECX bit alone sets exactly its flag. */
	regs_set(&leaf1, 0U, 0U, (uint32_t)1U << 3, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == CPUX86_FEATURE_MONITOR);
	CHECK(CPUX86_FEATURE_MONITOR == 1U);
	regs_set(&leaf1, 0U, 0U, (uint32_t)1U << 7, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == CPUX86_FEATURE_EIST);
	regs_set(&leaf1, 0U, 0U, (uint32_t)1U << 21, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == CPUX86_FEATURE_X2APIC);
	regs_set(&leaf1, 0U, 0U, (uint32_t)1U << 24, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == CPUX86_FEATURE_TSC_DEADLINE);
	regs_set(&leaf1, 0U, 0U, (uint32_t)1U << 31, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == CPUX86_FEATURE_HYPERVISOR);

	/* Every other ECX bit alone sets no flag; the five named ones set exactly the five flags. */
	named = ((uint32_t)1U << 3) | ((uint32_t)1U << 7) | ((uint32_t)1U << 21) |
	    ((uint32_t)1U << 24) | ((uint32_t)1U << 31);
	for (bit = 0U;
	     bit < 32U;
	     bit++) {
		regs_set(&leaf1, 0U, 0U, (uint32_t)1U << bit, 0U);
		CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
		expected = 0U;
		if (bit == 3U)
			expected = CPUX86_FEATURE_MONITOR;
		if (bit == 7U)
			expected = CPUX86_FEATURE_EIST;
		if (bit == 21U)
			expected = CPUX86_FEATURE_X2APIC;
		if (bit == 24U)
			expected = CPUX86_FEATURE_TSC_DEADLINE;
		if (bit == 31U)
			expected = CPUX86_FEATURE_HYPERVISOR;
		CHECK(features.flags == expected);
	}
	regs_set(&leaf1, 0U, 0U, named, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == 0x1FU);
	regs_set(&leaf1, 0U, 0U, ~named, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == 0U);
	regs_set(&leaf1, 0U, 0U, 0xFFFFFFFFU, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == 0x1FU);

	/* A real value: ECX 0x7FFAFBFF of an Intel part has MONITOR, EIST, x2APIC and TSC-deadline. */
	regs_set(&leaf1, 0U, 0U, 0x7FFAFBFFU, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.flags == (CPUX86_FEATURE_MONITOR | CPUX86_FEATURE_EIST |
	    CPUX86_FEATURE_X2APIC | CPUX86_FEATURE_TSC_DEADLINE));

	/* EBX[23:16] and EBX[31:24] are separate fields; EBX[15:0] is not read. */
	regs_set(&leaf1, 0U, 0x12345678U, 0U, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.apic_id_space == 0x34U);
	CHECK(features.initial_apic_id == 0x12U);
	regs_set(&leaf1, 0U, 0x0000FFFFU, 0U, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.apic_id_space == 0U);
	CHECK(features.initial_apic_id == 0U);
	regs_set(&leaf1, 0U, 0xFFFFFFFFU, 0U, 0U);
	CHECK(cpux86_cpuid_features(&leaf1, &features) == 0);
	CHECK(features.apic_id_space == 0xFFU);
	CHECK(features.initial_apic_id == 0xFFU);

	/* A NULL pointer is EINVAL and changes nothing. */
	memset(&features, SENTINEL_BYTE, sizeof(features));
	memcpy(&before, &features, sizeof(features));
	CHECK(cpux86_cpuid_features(NULL, &features) == EINVAL);
	CHECK(cpux86_cpuid_features(&leaf1, NULL) == EINVAL);
	CHECK(memcmp(&before, &features, sizeof(features)) == 0);
}

/*
 * Checks the MONITOR and MWAIT leaf.
 */
static void
test_mwait(void)
{
	struct cpux86_cpuid_regs leaf5;
	struct cpux86_mwait mwait;
	struct cpux86_mwait before;
	uint32_t index;

	/* The eight sub-state counts come from eight different 4-bit fields of EDX. */
	regs_set(&leaf5, 0x00000040U, 0x00000040U, 0x00000001U, 0x76543210U);
	memset(&mwait, SENTINEL_BYTE, sizeof(mwait));
	CHECK(cpux86_cpuid_mwait(&leaf5, &mwait) == 0);
	CHECK(mwait.smallest_line == 0x40U);
	CHECK(mwait.largest_line == 0x40U);
	CHECK(mwait.flags == CPUX86_MWAIT_EXTENSIONS);
	for (index = 0U;
	     index < CPUX86_MWAIT_CSTATE_COUNT;
	     index++)
		CHECK(mwait.substates[index] == index);

	/* A real-looking value: C0 0, C1 2, C2 1, C3 2, C4 1, C5 1, C6 1, C7 1 (EDX 0x11121120). */
	regs_set(&leaf5, 0x00000040U, 0x00000040U, 0x00000003U, 0x11121120U);
	CHECK(cpux86_cpuid_mwait(&leaf5, &mwait) == 0);
	CHECK(mwait.flags == (CPUX86_MWAIT_EXTENSIONS | CPUX86_MWAIT_INTERRUPT_BREAK));
	CHECK(mwait.substates[0] == 0U);
	CHECK(mwait.substates[1] == 2U);
	CHECK(mwait.substates[2] == 1U);
	CHECK(mwait.substates[3] == 1U);
	CHECK(mwait.substates[4] == 2U);
	CHECK(mwait.substates[5] == 1U);
	CHECK(mwait.substates[6] == 1U);
	CHECK(mwait.substates[7] == 1U);

	/* All 8 sub-states 8..15 in distinct fields. */
	regs_set(&leaf5, 0U, 0U, 0x00000001U, 0xFEDCBA98U);
	CHECK(cpux86_cpuid_mwait(&leaf5, &mwait) == 0);
	for (index = 0U;
	     index < CPUX86_MWAIT_CSTATE_COUNT;
	     index++)
		CHECK(mwait.substates[index] == 8U + index);

	/* Without ECX[0] EDX is not an enumeration: every count is 0, the other flags still show. */
	regs_set(&leaf5, 0U, 0U, 0x0000000EU, 0xFEDCBA98U);
	memset(&mwait, SENTINEL_BYTE, sizeof(mwait));
	CHECK(cpux86_cpuid_mwait(&leaf5, &mwait) == 0);
	CHECK(mwait.flags == (CPUX86_MWAIT_INTERRUPT_BREAK | CPUX86_MWAIT_MONITORLESS));
	for (index = 0U;
	     index < CPUX86_MWAIT_CSTATE_COUNT;
	     index++)
		CHECK(mwait.substates[index] == 0U);

	/* ECX bit 2 and bits 31:4 are reserved. */
	regs_set(&leaf5, 0U, 0U, 0xFFFFFFF4U, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_mwait(&leaf5, &mwait) == 0);
	CHECK(mwait.flags == 0U);
	regs_set(&leaf5, 0U, 0U, 0xFFFFFFFFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_mwait(&leaf5, &mwait) == 0);
	CHECK(mwait.flags == 0x7U);
	CHECK(mwait.substates[0] == 15U);
	CHECK(mwait.substates[7] == 15U);

	/* The line sizes are 16-bit fields; the upper half is reserved. */
	regs_set(&leaf5, 0xABCD1234U, 0x5678FEDCU, 0U, 0U);
	CHECK(cpux86_cpuid_mwait(&leaf5, &mwait) == 0);
	CHECK(mwait.smallest_line == 0x1234U);
	CHECK(mwait.largest_line == 0xFEDCU);

	/* A NULL pointer is EINVAL and changes nothing. */
	memset(&mwait, SENTINEL_BYTE, sizeof(mwait));
	memcpy(&before, &mwait, sizeof(mwait));
	CHECK(cpux86_cpuid_mwait(NULL, &mwait) == EINVAL);
	CHECK(cpux86_cpuid_mwait(&leaf5, NULL) == EINVAL);
	CHECK(memcmp(&before, &mwait, sizeof(mwait)) == 0);
}

/*
 * Checks the thermal and power leaf.
 */
static void
test_power(void)
{
	/* The EAX bits Table 21-16 defines, by hand (reserved: 3, 12, 21 and 31:24). */
	static const uint32_t defined_bits[] = {
		0U, 1U, 2U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U, 14U, 15U,
		16U, 17U, 18U, 19U, 20U, 22U, 23U
	};
	struct cpux86_cpuid_regs leaf6;
	struct cpux86_power power;
	struct cpux86_power before;
	uint32_t bit;
	uint32_t index;
	uint32_t mask;
	uint32_t expected;
	uint32_t row;

	/* The defined bits OR together to CPUX86_POWER_DEFINED. */
	mask = 0U;
	for (index = 0U;
	     index < sizeof(defined_bits) / sizeof(defined_bits[0]);
	     index++)
		mask |= (uint32_t)1U << defined_bits[index];
	CHECK(mask == CPUX86_POWER_DEFINED);
	CHECK(CPUX86_POWER_DEFINED == 0x00DFEFF7U);

	/* The named constants are the register bits themselves. */
	CHECK(CPUX86_POWER_DIGITAL_TEMP == ((uint32_t)1U << 0));
	CHECK(CPUX86_POWER_TURBO == ((uint32_t)1U << 1));
	CHECK(CPUX86_POWER_ARAT == ((uint32_t)1U << 2));
	CHECK(CPUX86_POWER_LIMIT_NOTIFY == ((uint32_t)1U << 4));
	CHECK(CPUX86_POWER_EXT_CLOCK_MOD == ((uint32_t)1U << 5));
	CHECK(CPUX86_POWER_PKG_THERM == ((uint32_t)1U << 6));
	CHECK(CPUX86_POWER_HWP == ((uint32_t)1U << 7));
	CHECK(CPUX86_POWER_HWP_INTERRUPT == ((uint32_t)1U << 8));
	CHECK(CPUX86_POWER_HWP_WINDOW == ((uint32_t)1U << 9));
	CHECK(CPUX86_POWER_HWP_EPP == ((uint32_t)1U << 10));
	CHECK(CPUX86_POWER_HWP_PKG == ((uint32_t)1U << 11));
	CHECK(CPUX86_POWER_HDC == ((uint32_t)1U << 13));
	CHECK(CPUX86_POWER_TURBO_MAX == ((uint32_t)1U << 14));
	CHECK(CPUX86_POWER_HWP_HIGHEST_CHANGE == ((uint32_t)1U << 15));
	CHECK(CPUX86_POWER_HWP_PECI == ((uint32_t)1U << 16));
	CHECK(CPUX86_POWER_HWP_FLEXIBLE == ((uint32_t)1U << 17));
	CHECK(CPUX86_POWER_HWP_FAST == ((uint32_t)1U << 18));
	CHECK(CPUX86_POWER_HW_FEEDBACK == ((uint32_t)1U << 19));
	CHECK(CPUX86_POWER_HWP_IGNORE_IDLE == ((uint32_t)1U << 20));
	CHECK(CPUX86_POWER_HWP_CTL == ((uint32_t)1U << 22));
	CHECK(CPUX86_POWER_THREAD_DIRECTOR == ((uint32_t)1U << 23));

	/* Each EAX bit alone: a defined bit appears in features, a reserved bit does not. */
	for (bit = 0U;
	     bit < 32U;
	     bit++) {
		regs_set(&leaf6, (uint32_t)1U << bit, 0U, 0U, 0U);
		memset(&power, SENTINEL_BYTE, sizeof(power));
		CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
		expected = 0U;
		for (index = 0U;
		     index < sizeof(defined_bits) / sizeof(defined_bits[0]);
		     index++) {
			if (defined_bits[index] == bit)
				expected = (uint32_t)1U << bit;
		}
		CHECK(power.features == expected);
	}

	/* Every EAX bit set: only the defined ones remain. */
	regs_set(&leaf6, 0xFFFFFFFFU, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.features == 0x00DFEFF7U);

	/* EBX[3:0] is the threshold count; bits 31:4 are reserved. */
	regs_set(&leaf6, 0U, 0xFFFFFFF5U, 0U, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.thermal_thresholds == 5U);
	regs_set(&leaf6, 0U, 0xFFFFFFFFU, 0U, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.thermal_thresholds == 15U);

	/* ECX[0] and ECX[3] are the counters; ECX[15:8] is the class count. */
	regs_set(&leaf6, 0U, 0U, 0x00000001U, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.counters == CPUX86_POWER_APERF_MPERF);
	CHECK(power.counters == 1U);
	regs_set(&leaf6, 0U, 0U, 0x00000008U, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.counters == CPUX86_POWER_ENERGY_BIAS);
	CHECK(power.counters == 8U);
	regs_set(&leaf6, 0U, 0U, 0xFFFFFFF6U, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.counters == 0U);
	CHECK(power.feedback_classes == 0xFFU);
	regs_set(&leaf6, 0U, 0U, 0xFFFFFFFFU, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.counters == 9U);
	regs_set(&leaf6, 0U, 0U, 0x0000A500U, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.feedback_classes == 0xA5U);
	CHECK(power.counters == 0U);

	/* EDX: capabilities [7:0] as reported, pages [11:8] + 1, row [31:16]; [15:12] is ignored. */
	regs_set(&leaf6, 0U, 0U, 0U, 0xABCD0F37U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.feedback_capabilities == 0x37U);
	CHECK(power.feedback_pages == 16U);
	CHECK(power.feedback_row == 0xABCDU);
	regs_set(&leaf6, 0U, 0U, 0U, 0x0000F0FFU);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.feedback_capabilities == 0xFFU);
	CHECK(power.feedback_pages == 1U);
	CHECK(power.feedback_row == 0U);
	regs_set(&leaf6, 0U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.feedback_pages == 1U);
	regs_set(&leaf6, 0U, 0U, 0U, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.feedback_pages == 16U);
	CHECK(power.feedback_row == 0xFFFFU);
	CHECK(power.feedback_capabilities == 0xFFU);

	/* The pages are reported whether or not EAX[19] says the interface exists. */
	regs_set(&leaf6, 0U, 0U, 0U, 0x00000300U);
	CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
	CHECK(power.features == 0U);
	CHECK(power.feedback_pages == 4U);

	/* The 5330 values worked out by hand: EAX 00DF8FF7, ECX 409, EDX rows 0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3. */
	for (row = 0U;
	     row < 4U;
	     row++) {
		regs_set(&leaf6, 0x00DF8FF7U, 0U, 0x00000409U, (row << 16) | 0x00000003U);
		CHECK(cpux86_cpuid_power(&leaf6, &power) == 0);
		CHECK(power.features == 0x00DF8FF7U);
		CHECK((power.features & CPUX86_POWER_HWP) != 0U);
		CHECK((power.features & CPUX86_POWER_HW_FEEDBACK) != 0U);
		CHECK((power.features & CPUX86_POWER_THREAD_DIRECTOR) != 0U);
		CHECK((power.features & CPUX86_POWER_TURBO_MAX) == 0U);
		CHECK(power.counters == (CPUX86_POWER_APERF_MPERF | CPUX86_POWER_ENERGY_BIAS));
		CHECK(power.feedback_classes == 4U);
		CHECK(power.feedback_capabilities == 3U);
		CHECK((power.feedback_capabilities & CPUX86_FEEDBACK_PERFORMANCE) != 0U);
		CHECK((power.feedback_capabilities & CPUX86_FEEDBACK_EFFICIENCY) != 0U);
		CHECK(power.feedback_pages == 1U);
		CHECK(power.feedback_row == row);
	}

	/* A NULL pointer is EINVAL and changes nothing. */
	memset(&power, SENTINEL_BYTE, sizeof(power));
	memcpy(&before, &power, sizeof(power));
	CHECK(cpux86_cpuid_power(NULL, &power) == EINVAL);
	CHECK(cpux86_cpuid_power(&leaf6, NULL) == EINVAL);
	CHECK(memcmp(&before, &power, sizeof(power)) == 0);
}

/*
 * Checks the hybrid flag and the core kind.
 */
static void
test_hybrid_core_kind(void)
{
	struct cpux86_cpuid_regs leaf;
	struct cpux86_core_kind kind;
	struct cpux86_core_kind before;
	uint32_t hybrid;
	uint32_t bit;

	/* EDX[15] alone makes the part hybrid; no other EDX bit does. */
	regs_set(&leaf, 0U, 0U, 0U, 0x00008000U);
	hybrid = 0xFFU;
	CHECK(cpux86_cpuid_hybrid(&leaf, &hybrid) == 0);
	CHECK(hybrid == 1U);
	for (bit = 0U;
	     bit < 32U;
	     bit++) {
		regs_set(&leaf, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, (uint32_t)1U << bit);
		hybrid = 0xFFU;
		CHECK(cpux86_cpuid_hybrid(&leaf, &hybrid) == 0);
		if (bit == 15U) {
			CHECK(hybrid == 1U);
		} else {
			CHECK(hybrid == 0U);
		}
	}
	regs_set(&leaf, 0U, 0U, 0U, 0xFFFF7FFFU);
	hybrid = 0xFFU;
	CHECK(cpux86_cpuid_hybrid(&leaf, &hybrid) == 0);
	CHECK(hybrid == 0U);

	/* A NULL pointer is EINVAL and changes nothing. */
	hybrid = 0xFFU;
	CHECK(cpux86_cpuid_hybrid(NULL, &hybrid) == EINVAL);
	CHECK(cpux86_cpuid_hybrid(&leaf, NULL) == EINVAL);
	CHECK(hybrid == 0xFFU);

	/* The two named core types and their native model numbers. */
	regs_set(&leaf, 0x40000001U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_core_kind(&leaf, &kind) == 0);
	CHECK(kind.core_type == CPUX86_CORE_TYPE_CORE);
	CHECK(kind.core_type == 0x40U);
	CHECK(kind.native_model_id == 1U);
	regs_set(&leaf, 0x20000001U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_core_kind(&leaf, &kind) == 0);
	CHECK(kind.core_type == CPUX86_CORE_TYPE_ATOM);
	CHECK(kind.core_type == 0x20U);
	CHECK(kind.native_model_id == 1U);

	/* A reserved type is passed through, with the 24-bit model number. */
	regs_set(&leaf, 0x10ABCDEFU, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_core_kind(&leaf, &kind) == 0);
	CHECK(kind.core_type == 0x10U);
	CHECK(kind.native_model_id == 0xABCDEFU);
	regs_set(&leaf, 0xFF000000U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_core_kind(&leaf, &kind) == 0);
	CHECK(kind.core_type == 0xFFU);
	CHECK(kind.native_model_id == 0U);

	/* A non-zero EAX with core type 0 is still a valid leaf. */
	regs_set(&leaf, 0x00000005U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_core_kind(&leaf, &kind) == 0);
	CHECK(kind.core_type == 0U);
	CHECK(kind.native_model_id == 5U);

	/* EAX 0 is ENOTSUP and the output is unchanged. */
	memset(&kind, SENTINEL_BYTE, sizeof(kind));
	memcpy(&before, &kind, sizeof(kind));
	regs_set(&leaf, 0U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_core_kind(&leaf, &kind) == ENOTSUP);
	CHECK(memcmp(&before, &kind, sizeof(kind)) == 0);

	/* A NULL pointer is EINVAL and changes nothing. */
	CHECK(cpux86_cpuid_core_kind(NULL, &kind) == EINVAL);
	CHECK(cpux86_cpuid_core_kind(&leaf, NULL) == EINVAL);
	CHECK(memcmp(&before, &kind, sizeof(kind)) == 0);
}

/*
 * Builds one sub-leaf of leaf 04H from the fields of Table 21-14.
 */
static void
cache_leaf(
	struct cpux86_cpuid_regs *regs,
	uint32_t type,
	uint32_t level,
	uint32_t sharing_minus_one,
	uint32_t line_minus_one,
	uint32_t partitions_minus_one,
	uint32_t ways_minus_one,
	uint32_t sets_minus_one)
{
	/* Starts from clean registers. */
	regs->eax = 0U;
	regs->ebx = 0U;
	regs->ecx = sets_minus_one;
	regs->edx = 0U;

	/* Places each field at the bits the table gives. */
	regbuild_put32(&regs->eax, 0U, 5U, type);
	regbuild_put32(&regs->eax, 5U, 3U, level);
	regbuild_put32(&regs->eax, 14U, 12U, sharing_minus_one);
	regbuild_put32(&regs->ebx, 0U, 12U, line_minus_one);
	regbuild_put32(&regs->ebx, 12U, 10U, partitions_minus_one);
	regbuild_put32(&regs->ebx, 22U, 10U, ways_minus_one);
}

/*
 * Checks the cache leaf and the cache group.
 */
static void
test_cache(void)
{
	struct cpux86_cpuid_regs leaf4;
	struct cpux86_cache cache;
	struct cpux86_cache before;
	uint32_t group;
	uint32_t type;
	uint32_t sharing;
	uint32_t shift;
	uint32_t index;
	uint64_t expected_size;

	/* A 48 KiB 12-way data cache: 12 * 1 * 64 * 64 = 49152, shared by 2 logical processors. */
	cache_leaf(&leaf4, 1U, 1U, 1U, 63U, 0U, 11U, 63U);
	memset(&cache, SENTINEL_BYTE, sizeof(cache));
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.type == CPUX86_CACHE_DATA);
	CHECK(cache.level == 1U);
	CHECK(cache.flags == 0U);
	CHECK(cache.sharing_ids == 2U);
	CHECK(cache.share_shift == 1U);
	CHECK(cache.line_size == 64U);
	CHECK(cache.partitions == 1U);
	CHECK(cache.ways == 12U);
	CHECK(cache.sets == 64U);
	CHECK(cache.size == 49152U);

	/* A unified 18 MiB cache: 12 * 1 * 64 * 24576 = 18874368, shared by 16, inclusive and complex. */
	cache_leaf(&leaf4, 3U, 3U, 15U, 63U, 0U, 11U, 24575U);
	leaf4.edx = 0x00000006U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.type == CPUX86_CACHE_UNIFIED);
	CHECK(cache.level == 3U);
	CHECK(cache.flags == (CPUX86_CACHE_INCLUSIVE | CPUX86_CACHE_COMPLEX_INDEX));
	CHECK(cache.sharing_ids == 16U);
	CHECK(cache.share_shift == 4U);
	CHECK(cache.sets == 24576U);
	CHECK(cache.size == 18874368U);

	/* An instruction cache with 2 partitions: 8 * 2 * 32 * 1 = 512. */
	cache_leaf(&leaf4, 2U, 2U, 0U, 31U, 1U, 7U, 0U);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.type == CPUX86_CACHE_INSTRUCTION);
	CHECK(cache.level == 2U);
	CHECK(cache.sharing_ids == 1U);
	CHECK(cache.share_shift == 0U);
	CHECK(cache.line_size == 32U);
	CHECK(cache.partitions == 2U);
	CHECK(cache.ways == 8U);
	CHECK(cache.sets == 1U);
	CHECK(cache.size == 512U);

	/* The level field is three bits: 0 and 7 are passed through. */
	cache_leaf(&leaf4, 1U, 0U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.level == 0U);
	CHECK(cache.size == 1U);
	cache_leaf(&leaf4, 1U, 7U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.level == 7U);

	/* The sharing count 1..4096 and its shift: 1 -> 0, 2 -> 1, 3 -> 2, 4 -> 2, 5 -> 3, 4096 -> 12. */
	for (index = 0U;
	     index < 6U;
	     index++) {
		static const uint32_t counts[6] = { 1U, 2U, 3U, 4U, 5U, 4096U };
		static const uint32_t shifts[6] = { 0U, 1U, 2U, 2U, 3U, 12U };

		sharing = counts[index];
		cache_leaf(&leaf4, 3U, 2U, sharing - 1U, 63U, 0U, 7U, 1023U);
		CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
		CHECK(cache.sharing_ids == sharing);
		CHECK(cache.share_shift == shifts[index]);
	}

	/* The bits above EAX[25:14] (EAX[31:26]) are not part of the sharing count. */
	cache_leaf(&leaf4, 3U, 2U, 3U, 63U, 0U, 7U, 1023U);
	leaf4.eax |= 0xFC000000U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.sharing_ids == 4U);

	/* Each flag alone, from EAX[8], EAX[9], EDX[0], EDX[1] and EDX[2]; the other bits are ignored. */
	cache_leaf(&leaf4, 1U, 1U, 0U, 63U, 0U, 7U, 63U);
	leaf4.eax |= 0x00000100U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.flags == CPUX86_CACHE_SELF_INIT);
	cache_leaf(&leaf4, 1U, 1U, 0U, 63U, 0U, 7U, 63U);
	leaf4.eax |= 0x00000200U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.flags == CPUX86_CACHE_FULLY_ASSOC);
	cache_leaf(&leaf4, 1U, 1U, 0U, 63U, 0U, 7U, 63U);
	leaf4.edx = 0x00000001U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.flags == CPUX86_CACHE_NO_LOWER_FLUSH);
	leaf4.edx = 0x00000002U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.flags == CPUX86_CACHE_INCLUSIVE);
	leaf4.edx = 0x00000004U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.flags == CPUX86_CACHE_COMPLEX_INDEX);
	leaf4.edx = 0xFFFFFFF8U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.flags == 0U);
	leaf4.edx = 0xFFFFFFFFU;
	leaf4.eax |= 0x00003C00U;
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.flags == (CPUX86_CACHE_NO_LOWER_FLUSH | CPUX86_CACHE_INCLUSIVE | CPUX86_CACHE_COMPLEX_INDEX));

	/* The largest geometry with sets 2^32 - 1: 2^32 * (2^32 - 1) = 0xFFFFFFFF00000000 fits. */
	cache_leaf(&leaf4, 3U, 3U, 0U, 0xFFFU, 0x3FFU, 0x3FFU, 0xFFFFFFFEU);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.line_size == 4096U);
	CHECK(cache.partitions == 1024U);
	CHECK(cache.ways == 1024U);
	CHECK(cache.sets == 0xFFFFFFFFU);
	CHECK(cache.size == (((uint64_t)0xFFFFFFFFU << 32)));

	/* The largest geometry with 1 set is 2^32; 2 sets is 2^33. */
	cache_leaf(&leaf4, 3U, 3U, 0U, 0xFFFU, 0x3FFU, 0x3FFU, 0U);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.size == ((uint64_t)1U << 32));
	cache_leaf(&leaf4, 3U, 3U, 0U, 0xFFFU, 0x3FFU, 0x3FFU, 1U);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.size == ((uint64_t)1U << 33));

	/* A geometry of 1 with 2^32 sets is 2^32 and fits. */
	cache_leaf(&leaf4, 3U, 3U, 0U, 0U, 0U, 0U, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.sets == ((uint64_t)1U << 32));
	CHECK(cache.size == ((uint64_t)1U << 32));

	/* Geometry 2^31 with 2^32 sets is 2^63 and fits. */
	cache_leaf(&leaf4, 3U, 3U, 0U, 0xFFFU, 0x3FFU, 0x1FFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
	CHECK(cache.size == ((uint64_t)1U << 63));

	/* Everything at its largest: 2^32 * 2^32 = 2^64 does not fit, ERANGE, output unchanged. */
	memset(&cache, SENTINEL_BYTE, sizeof(cache));
	memcpy(&before, &cache, sizeof(cache));
	cache_leaf(&leaf4, 3U, 3U, 0U, 0xFFFU, 0x3FFU, 0x3FFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == ERANGE);
	CHECK(memcmp(&before, &cache, sizeof(cache)) == 0);
	regs_set(&leaf4, 0xFFFFFFE3U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == ERANGE);
	CHECK(memcmp(&before, &cache, sizeof(cache)) == 0);

	/* Type 0 (no more caches) is ENOTSUP whatever else the registers hold, output unchanged. */
	regs_set(&leaf4, 0xFFFFFFE0U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == ENOTSUP);
	regs_set(&leaf4, 0U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_cache(&leaf4, &cache) == ENOTSUP);
	CHECK(memcmp(&before, &cache, sizeof(cache)) == 0);

	/* Reserved types 4..31 are EINVAL, output unchanged. */
	for (type = 4U;
	     type < 32U;
	     type++) {
		cache_leaf(&leaf4, type, 1U, 0U, 63U, 0U, 7U, 63U);
		CHECK(cpux86_cpuid_cache(&leaf4, &cache) == EINVAL);
		CHECK(memcmp(&before, &cache, sizeof(cache)) == 0);
	}

	/* A NULL pointer is EINVAL and changes nothing. */
	CHECK(cpux86_cpuid_cache(NULL, &cache) == EINVAL);
	CHECK(cpux86_cpuid_cache(&leaf4, NULL) == EINVAL);
	CHECK(memcmp(&before, &cache, sizeof(cache)) == 0);

	/* The group drops the low share_shift bits of the x2APIC ID. */
	memset(&cache, 0, sizeof(cache));
	group = 0xFFU;
	cache.share_shift = 0U;
	CHECK(cpux86_cache_group(0x1FU, &cache, &group) == 0);
	CHECK(group == 0x1FU);
	cache.share_shift = 1U;
	CHECK(cpux86_cache_group(0x1FU, &cache, &group) == 0);
	CHECK(group == 0xFU);
	cache.share_shift = 4U;
	CHECK(cpux86_cache_group(0x1FU, &cache, &group) == 0);
	CHECK(group == 1U);
	cache.share_shift = 12U;
	CHECK(cpux86_cache_group(0x12345678U, &cache, &group) == 0);
	CHECK(group == 0x12345U);
	cache.share_shift = 31U;
	CHECK(cpux86_cache_group(0xFFFFFFFFU, &cache, &group) == 0);
	CHECK(group == 1U);
	CHECK(cpux86_cache_group(0x7FFFFFFFU, &cache, &group) == 0);
	CHECK(group == 0U);

	/* A shift of 32 or more is EINVAL and leaves the group alone. */
	group = 0x77U;
	for (shift = 32U;
	     shift < 40U;
	     shift++) {
		cache.share_shift = shift;
		CHECK(cpux86_cache_group(0xFFFFFFFFU, &cache, &group) == EINVAL);
		CHECK(group == 0x77U);
	}
	cache.share_shift = 0xFFFFFFFFU;
	CHECK(cpux86_cache_group(0xFFFFFFFFU, &cache, &group) == EINVAL);
	CHECK(group == 0x77U);

	/* A NULL pointer is EINVAL. */
	cache.share_shift = 1U;
	CHECK(cpux86_cache_group(1U, NULL, &group) == EINVAL);
	CHECK(cpux86_cache_group(1U, &cache, NULL) == EINVAL);
	CHECK(group == 0x77U);

	/* The size equals the product of the four counts, checked by division for each shape. */
	for (index = 0U;
	     index < 32U;
	     index++) {
		cache_leaf(&leaf4, 3U, 1U, 0U, (index * 129U) & 0xFFFU, (index * 31U) & 0x3FFU,
		    (index * 17U) & 0x3FFU, index * 0x01010101U);
		CHECK(cpux86_cpuid_cache(&leaf4, &cache) == 0);
		expected_size = (uint64_t)cache.ways * cache.partitions * cache.line_size;
		CHECK(cache.size / expected_size == cache.sets);
		CHECK(cache.size % expected_size == 0U);
	}
}

/*
 * Fills one sub-leaf of an extended topology list.
 */
static void
topology_set(
	struct cpux86_cpuid_regs *list,
	uint32_t index,
	uint32_t type,
	uint32_t shift,
	uint32_t count,
	uint32_t x2apic_id)
{
	/* The level field of the sub-leaf is its own index. */
	regbuild_topology(&list[index], index, type, shift, count, x2apic_id);
}

/*
 * Checks one topology call that must fail: the return value and the
 * unchanged output.
 */
static void
topology_refused(
	uint32_t leaf,
	const struct cpux86_cpuid_regs *list,
	uint32_t count,
	int error,
	unsigned line)
{
	struct cpux86_topology topology;
	struct cpux86_topology before;
	int returned;

	/* Makes a distinctive output so a change shows. */
	memset(&topology, SENTINEL_BYTE, sizeof(topology));
	memcpy(&before, &topology, sizeof(topology));

	/* Runs the call and compares both the return value and the output. */
	returned = cpux86_cpuid_topology(leaf, list, count, &topology);
	check_report(returned == error, "topology error value", __FILE__, (int)line);
	check_report(memcmp(&before, &topology, sizeof(topology)) == 0, "topology unchanged", __FILE__, (int)line);
}

/* The call-site line is recorded with the refusal check. */
#define TOPOLOGY_REFUSED(leaf, list, count, error) topology_refused((leaf), (list), (count), (error), __LINE__)

/*
 * Checks the extended topology leaves 0BH and 1FH.
 */
static void
test_topology(void)
{
	struct cpux86_cpuid_regs list[CPUX86_TOPOLOGY_SUBLEAF_MAX + 1U];
	struct cpux86_topology topology;
	struct cpux86_topology_level level;
	struct cpux86_cpuid_regs raw;
	uint32_t leaf;
	uint32_t index;
	uint32_t x2apic_id;

	/* A single sub-leaf is decoded field by field; EAX[31:5] and EBX[31:16] are reserved. */
	regs_set(&raw, 0xFFFFFFE3U, 0xFFFF0123U, 0xFFFF0305U, 0xCAFEBABEU);
	memset(&level, SENTINEL_BYTE, sizeof(level));
	CHECK(cpux86_cpuid_topology_level(&raw, &level) == 0);
	CHECK(level.level == 5U);
	CHECK(level.type == 3U);
	CHECK(level.shift == 3U);
	CHECK(level.logical_count == 0x123U);
	CHECK(level.x2apic_id == 0xCAFEBABEU);
	regs_set(&raw, 0x1FU, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_topology_level(&raw, &level) == 0);
	CHECK(level.shift == 31U);
	CHECK(level.type == CPUX86_DOMAIN_INVALID);
	CHECK(cpux86_cpuid_topology_level(NULL, &level) == EINVAL);
	CHECK(cpux86_cpuid_topology_level(&raw, NULL) == EINVAL);

	/* The 5330 shape, x2APIC ID 09H: thread 1, core 4, package 0, on both leaves. */
	for (leaf = CPUX86_LEAF_TOPOLOGY;
	     leaf <= CPUX86_LEAF_TOPOLOGY_V2;
	     leaf += CPUX86_LEAF_TOPOLOGY_V2 - CPUX86_LEAF_TOPOLOGY) {
		topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
		topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x09U);
		topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
		memset(&topology, SENTINEL_BYTE, sizeof(topology));
		CHECK(cpux86_cpuid_topology(leaf, list, 3U, &topology) == 0);
		CHECK(topology.x2apic_id == 0x09U);
		CHECK(topology.present == (((uint32_t)1U << CPUX86_DOMAIN_THREAD) | ((uint32_t)1U << CPUX86_DOMAIN_CORE)));
		CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 1U);
		CHECK(topology.low[CPUX86_DOMAIN_THREAD] == 0U);
		CHECK(topology.high[CPUX86_DOMAIN_THREAD] == 1U);
		CHECK(topology.id[CPUX86_DOMAIN_CORE] == 4U);
		CHECK(topology.low[CPUX86_DOMAIN_CORE] == 1U);
		CHECK(topology.high[CPUX86_DOMAIN_CORE] == 6U);
		CHECK(topology.package == 0U);
		CHECK(topology.package_shift == 6U);
		for (index = 0U;
		     index < CPUX86_DOMAIN_COUNT;
		     index++) {
			if (index == CPUX86_DOMAIN_THREAD || index == CPUX86_DOMAIN_CORE)
				continue;
			CHECK(topology.id[index] == 0U);
			CHECK(topology.low[index] == 0U);
			CHECK(topology.high[index] == 0U);
		}
		CHECK(topology.id[0] == 0U);

		/* x2APIC ID 1EH: thread 0, core 15. */
		topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x1EU);
		topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x1EU);
		topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x1EU);
		CHECK(cpux86_cpuid_topology(leaf, list, 3U, &topology) == 0);
		CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 0U);
		CHECK(topology.id[CPUX86_DOMAIN_CORE] == 15U);
		CHECK(topology.package == 0U);

		/* x2APIC ID 4EH: thread 0, core 7, package 1 (bit 6). */
		topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x4EU);
		topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x4EU);
		topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x4EU);
		CHECK(cpux86_cpuid_topology(leaf, list, 3U, &topology) == 0);
		CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 0U);
		CHECK(topology.id[CPUX86_DOMAIN_CORE] == 7U);
		CHECK(topology.package == 1U);
	}

	/* Four domains of leaf 1FH: THREAD 1, CORE 4, MODULE 6, DIE 8 bits, ID 2B5H: 1, 2, 3, 2 and package 2. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x2B5U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 4U, 16U, 0x2B5U);
	topology_set(list, 2U, CPUX86_DOMAIN_MODULE, 6U, 32U, 0x2B5U);
	topology_set(list, 3U, CPUX86_DOMAIN_DIE, 8U, 64U, 0x2B5U);
	topology_set(list, 4U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x2B5U);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY_V2, list, 5U, &topology) == 0);
	CHECK(topology.present == ((1U << 1) | (1U << 2) | (1U << 3) | (1U << 5)));
	CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 1U);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 2U);
	CHECK(topology.id[CPUX86_DOMAIN_MODULE] == 3U);
	CHECK(topology.id[CPUX86_DOMAIN_DIE] == 2U);
	CHECK(topology.low[CPUX86_DOMAIN_MODULE] == 4U);
	CHECK(topology.high[CPUX86_DOMAIN_MODULE] == 6U);
	CHECK(topology.low[CPUX86_DOMAIN_DIE] == 6U);
	CHECK(topology.high[CPUX86_DOMAIN_DIE] == 8U);
	CHECK(topology.id[CPUX86_DOMAIN_TILE] == 0U);
	CHECK(topology.id[CPUX86_DOMAIN_DIEGRP] == 0U);
	CHECK(topology.package == 2U);
	CHECK(topology.package_shift == 8U);

	/* The same list read as leaf 0BH: types above CORE are unknown and extend the core up to bit 8. */
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 5U, &topology) == 0);
	CHECK(topology.present == ((1U << 1) | (1U << 2)));
	CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 1U);
	CHECK(topology.low[CPUX86_DOMAIN_CORE] == 1U);
	CHECK(topology.high[CPUX86_DOMAIN_CORE] == 8U);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 0x5AU);
	CHECK(topology.id[CPUX86_DOMAIN_MODULE] == 0U);
	CHECK(topology.id[CPUX86_DOMAIN_DIE] == 0U);
	CHECK(topology.package == 2U);
	CHECK(topology.package_shift == 8U);

	/* A tile (type 4) is a known domain of leaf 1FH between core and die. */
	x2apic_id = 0xB5U;
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, x2apic_id);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 3U, 8U, x2apic_id);
	topology_set(list, 2U, CPUX86_DOMAIN_TILE, 5U, 16U, x2apic_id);
	topology_set(list, 3U, CPUX86_DOMAIN_DIEGRP, 8U, 32U, x2apic_id);
	topology_set(list, 4U, CPUX86_DOMAIN_INVALID, 0U, 0U, x2apic_id);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY_V2, list, 5U, &topology) == 0);
	CHECK(topology.present == ((1U << 1) | (1U << 2) | (1U << 4) | (1U << 6)));
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 2U);
	CHECK(topology.id[CPUX86_DOMAIN_TILE] == 2U);
	CHECK(topology.id[CPUX86_DOMAIN_DIEGRP] == 5U);

	/* A reserved type 7 in leaf 1FH between CORE and DIE extends the core field: ID B5H. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, x2apic_id);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 3U, 8U, x2apic_id);
	topology_set(list, 2U, 7U, 5U, 16U, x2apic_id);
	topology_set(list, 3U, CPUX86_DOMAIN_DIE, 8U, 32U, x2apic_id);
	topology_set(list, 4U, CPUX86_DOMAIN_INVALID, 0U, 0U, x2apic_id);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY_V2, list, 5U, &topology) == 0);
	CHECK(topology.present == ((1U << 1) | (1U << 2) | (1U << 5)));
	CHECK(topology.low[CPUX86_DOMAIN_CORE] == 1U);
	CHECK(topology.high[CPUX86_DOMAIN_CORE] == 5U);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 10U);
	CHECK(topology.low[CPUX86_DOMAIN_DIE] == 5U);
	CHECK(topology.high[CPUX86_DOMAIN_DIE] == 8U);
	CHECK(topology.id[CPUX86_DOMAIN_DIE] == 5U);
	CHECK(topology.package == 0U);
	CHECK(topology.package_shift == 8U);

	/* Unknown types after the die extend the die; two unknown types in a row extend it twice. */
	topology_set(list, 3U, CPUX86_DOMAIN_DIE, 6U, 32U, x2apic_id);
	topology_set(list, 4U, 9U, 7U, 32U, x2apic_id);
	topology_set(list, 5U, 200U, 8U, 32U, x2apic_id);
	topology_set(list, 6U, CPUX86_DOMAIN_INVALID, 0U, 0U, x2apic_id);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY_V2, list, 7U, &topology) == 0);
	CHECK(topology.low[CPUX86_DOMAIN_DIE] == 5U);
	CHECK(topology.high[CPUX86_DOMAIN_DIE] == 8U);
	CHECK(topology.id[CPUX86_DOMAIN_DIE] == 5U);
	CHECK(topology.package_shift == 8U);

	/* A type 3 in leaf 0BH is unknown and widens the core (same bits as the 1FH reading's core + module). */
	x2apic_id = 0x7DU;
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, x2apic_id);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 4U, 8U, x2apic_id);
	topology_set(list, 2U, CPUX86_DOMAIN_MODULE, 6U, 16U, x2apic_id);
	topology_set(list, 3U, CPUX86_DOMAIN_INVALID, 0U, 0U, x2apic_id);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 4U, &topology) == 0);
	CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 1U);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 30U);
	CHECK(topology.package == 1U);
	CHECK(topology.package_shift == 6U);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY_V2, list, 4U, &topology) == 0);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 6U);
	CHECK(topology.id[CPUX86_DOMAIN_MODULE] == 3U);
	CHECK(topology.package == 1U);

	/* Shift 31 and the largest ID: the core owns bits 1..30, the package is bit 31. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0xFFFFFFFFU);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 31U, 8U, 0xFFFFFFFFU);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 3U, &topology) == 0);
	CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 1U);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 0x3FFFFFFFU);
	CHECK(topology.package == 1U);
	CHECK(topology.package_shift == 31U);
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x7FFFFFFFU);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 31U, 8U, 0x7FFFFFFFU);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x7FFFFFFFU);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 3U, &topology) == 0);
	CHECK(topology.package == 0U);

	/* Shift 0 for the thread (no thread bits) leaves the thread field empty and the core at bit 0. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 0U, 1U, 0x13U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 5U, 8U, 0x13U);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x13U);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 3U, &topology) == 0);
	CHECK(topology.present == ((1U << 1) | (1U << 2)));
	CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 0U);
	CHECK(topology.low[CPUX86_DOMAIN_THREAD] == 0U);
	CHECK(topology.high[CPUX86_DOMAIN_THREAD] == 0U);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 0x13U);
	CHECK(topology.package == 0U);

	/* Equal shifts (an empty core field) are allowed. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 2U, 4U, 0x0DU);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 2U, 4U, 0x0DU);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x0DU);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 3U, &topology) == 0);
	CHECK(topology.id[CPUX86_DOMAIN_THREAD] == 1U);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 0U);
	CHECK(topology.package == 3U);

	/* Entries after the first INVALID one are not read: even a mismatching level and ID do not matter. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	topology_set(list, 3U, CPUX86_DOMAIN_THREAD, 0U, 1U, 0xFFU);
	list[3].ecx = 0xFFFFFFFFU;
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 4U, &topology) == 0);
	CHECK(topology.id[CPUX86_DOMAIN_CORE] == 4U);
	CHECK(topology.package_shift == 6U);

	/* The decoder reads only up to the first INVALID sub-leaf: ASan sees an array that ends there. */
	{
		struct cpux86_cpuid_regs *exact;

		exact = (struct cpux86_cpuid_regs *)malloc(3U * sizeof(*exact));
		CHECK(exact != NULL);
		if (exact != NULL) {
			exact[0] = list[0];
			exact[1] = list[1];
			exact[2] = list[2];
			CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY_V2, exact, 8U, &topology) == 0);
			free(exact);
		}
	}

	/* Arguments: a NULL pointer, a leaf that is not 0BH or 1FH, and a count outside 1..8 are EINVAL. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, NULL, 3U, EINVAL);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 3U, NULL) == EINVAL);
	TOPOLOGY_REFUSED(0U, list, 3U, EINVAL);
	TOPOLOGY_REFUSED(0x0AU, list, 3U, EINVAL);
	TOPOLOGY_REFUSED(0x0CU, list, 3U, EINVAL);
	TOPOLOGY_REFUSED(0x1EU, list, 3U, EINVAL);
	TOPOLOGY_REFUSED(0x20U, list, 3U, EINVAL);
	TOPOLOGY_REFUSED(0xFFFFFFFFU, list, 3U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 0U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 9U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 0xFFFFFFFFU, EINVAL);

	/* No INVALID within count: two valid entries only, and eight valid entries. */
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 2U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 1U, EINVAL);
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 2U, 2U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_MODULE, 3U, 2U, 0x09U);
	topology_set(list, 3U, CPUX86_DOMAIN_TILE, 4U, 2U, 0x09U);
	topology_set(list, 4U, CPUX86_DOMAIN_DIE, 5U, 2U, 0x09U);
	topology_set(list, 5U, CPUX86_DOMAIN_DIEGRP, 6U, 2U, 0x09U);
	topology_set(list, 6U, 7U, 7U, 2U, 0x09U);
	topology_set(list, 7U, 8U, 8U, 2U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 8U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 7U, EINVAL);
	topology_set(list, 7U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY_V2, list, 8U, &topology) == 0);
	CHECK(topology.package_shift == 7U);

	/* Only the THREAD domain is valid: sub-leaf 1 INVALID, or a list of 1 with no terminator. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 2U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 8U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 1U, EINVAL);

	/* ECX[7:0] must be the sub-leaf's own index, for valid entries and for the terminator. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	list[1].ecx = ((uint32_t)CPUX86_DOMAIN_CORE << 8) | 2U;
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 3U, EINVAL);
	list[1].ecx = ((uint32_t)CPUX86_DOMAIN_CORE << 8) | 1U;
	list[0].ecx = ((uint32_t)CPUX86_DOMAIN_THREAD << 8) | 1U;
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 3U, EINVAL);
	list[0].ecx = ((uint32_t)CPUX86_DOMAIN_THREAD << 8) | 0U;
	list[2].ecx = 5U;
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 3U, EINVAL);
	list[2].ecx = 2U;
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 3U, &topology) == 0);

	/* A shift that goes down. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 3U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 2U, 12U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 3U, EINVAL);
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_DIE, 5U, 12U, 0x09U);
	topology_set(list, 3U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 4U, EINVAL);

	/* An x2APIC ID that differs between valid sub-leaves; the terminator's is not compared. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x08U);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 3U, EINVAL);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x55U);
	CHECK(cpux86_cpuid_topology(CPUX86_LEAF_TOPOLOGY, list, 3U, &topology) == 0);
	CHECK(topology.x2apic_id == 0x09U);

	/* Order: sub-leaf 0 must be THREAD, sub-leaf 1 must be CORE. */
	topology_set(list, 0U, CPUX86_DOMAIN_CORE, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_THREAD, 6U, 12U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 3U, EINVAL);
	topology_set(list, 0U, 7U, 1U, 2U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 3U, EINVAL);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 3U, EINVAL);
	topology_set(list, 0U, CPUX86_DOMAIN_MODULE, 1U, 2U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 3U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 3U, EINVAL);
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_MODULE, 6U, 12U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 3U, EINVAL);
	topology_set(list, 1U, 9U, 6U, 12U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 3U, EINVAL);

	/* Known types must be unique and ascending: repeated, reversed and a THREAD again. */
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 2U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 4U, 12U, 0x09U);
	topology_set(list, 2U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x09U);
	topology_set(list, 3U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 4U, EINVAL);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 4U, EINVAL);
	topology_set(list, 2U, CPUX86_DOMAIN_THREAD, 6U, 12U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 4U, EINVAL);
	topology_set(list, 2U, CPUX86_DOMAIN_DIE, 6U, 12U, 0x09U);
	topology_set(list, 3U, CPUX86_DOMAIN_MODULE, 7U, 12U, 0x09U);
	topology_set(list, 4U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 5U, EINVAL);
	topology_set(list, 3U, CPUX86_DOMAIN_DIE, 7U, 12U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 5U, EINVAL);

	/* A leaf the processor does not implement: ENOTSUP, whatever follows sub-leaf 0. */
	topology_set(list, 0U, CPUX86_DOMAIN_INVALID, 0U, 0U, 0x09U);
	topology_set(list, 1U, CPUX86_DOMAIN_CORE, 6U, 12U, 0x08U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 1U, ENOTSUP);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 8U, ENOTSUP);
	topology_set(list, 0U, CPUX86_DOMAIN_THREAD, 1U, 0U, 0x09U);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 2U, ENOTSUP);
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY_V2, list, 1U, ENOTSUP);
	list[0].ebx = 0xFFFF0000U;
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 2U, ENOTSUP);
	list[0].ebx = 1U;
	list[0].ecx = 0U;
	TOPOLOGY_REFUSED(CPUX86_LEAF_TOPOLOGY, list, 2U, ENOTSUP);
}

/*
 * Checks the extended leaves 80000007H and 80000008H.
 */
static void
test_extended(void)
{
	struct cpux86_cpuid_regs leaf7;
	struct cpux86_cpuid_regs leaf8;
	struct cpux86_extended extended;
	struct cpux86_extended before;

	/* Both leaves NULL: both fields 0. */
	memset(&extended, SENTINEL_BYTE, sizeof(extended));
	CHECK(cpux86_cpuid_extended(NULL, NULL, &extended) == 0);
	CHECK(extended.invariant_tsc == 0U);
	CHECK(extended.physical_address_bits == 0U);

	/* EDX[8] of 80000007H and EAX[7:0] of 80000008H, with everything else set. */
	regs_set(&leaf7, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0x00000100U);
	regs_set(&leaf8, 0xFFFFFF27U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU);
	CHECK(cpux86_cpuid_extended(&leaf7, &leaf8, &extended) == 0);
	CHECK(extended.invariant_tsc == 1U);
	CHECK(extended.physical_address_bits == 0x27U);
	regs_set(&leaf7, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFEFFU);
	regs_set(&leaf8, 0x00000030U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_extended(&leaf7, &leaf8, &extended) == 0);
	CHECK(extended.invariant_tsc == 0U);
	CHECK(extended.physical_address_bits == 0x30U);

	/* One leaf NULL leaves its field 0 and the other still decodes. */
	regs_set(&leaf7, 0U, 0U, 0U, 0x00000100U);
	CHECK(cpux86_cpuid_extended(&leaf7, NULL, &extended) == 0);
	CHECK(extended.invariant_tsc == 1U);
	CHECK(extended.physical_address_bits == 0U);
	regs_set(&leaf8, 0x00000026U, 0U, 0U, 0U);
	CHECK(cpux86_cpuid_extended(NULL, &leaf8, &extended) == 0);
	CHECK(extended.invariant_tsc == 0U);
	CHECK(extended.physical_address_bits == 0x26U);

	/* A NULL output is EINVAL. */
	memset(&extended, SENTINEL_BYTE, sizeof(extended));
	memcpy(&before, &extended, sizeof(extended));
	CHECK(cpux86_cpuid_extended(&leaf7, &leaf8, NULL) == EINVAL);
	CHECK(memcmp(&before, &extended, sizeof(extended)) == 0);
}

/* The state of the pseudo-random generator of the random-input check. */
static uint32_t random_state = 0x2545F491U;

/*
 * Gives the next pseudo-random 32-bit value (xorshift32, a fixed seed).
 */
static uint32_t
random_next(void)
{
	/* Advances the generator by three shifts. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Reports the new state. */
	return random_state;
}

/*
 * Gives a random 32-bit value that is often sparse or dense, so fields
 * hit their extremes as well as their middles.
 */
static uint32_t
random_biased(void)
{
	uint32_t value;
	uint32_t pick;

	/* Picks a flavour of value: uniform, sparse, dense or one bit. */
	value = random_next();
	pick = random_next() & 3U;
	if (pick == 1U)
		value = value & random_next() & random_next();
	if (pick == 2U)
		value = value | random_next() | random_next();
	if (pick == 3U)
		value = (uint32_t)1U << (random_next() & 31U);

	/* Reports the value. */
	return value;
}

/*
 * Fills random registers.
 */
static void
random_regs(
	struct cpux86_cpuid_regs *regs)
{
	/* Each register gets its own flavour of random value. */
	regs->eax = random_biased();
	regs->ebx = random_biased();
	regs->ecx = random_biased();
	regs->edx = random_biased();
}

/*
 * Checks that every decoder, fed random registers, answers only with the
 * values its header lists, leaves an output alone on an error, and gives
 * self-consistent results on success.
 */
static void
test_random(void)
{
	struct cpux86_cpuid_regs leaf0;
	struct cpux86_cpuid_regs leaf1;
	struct cpux86_cpuid_regs *list;
	struct cpux86_identity id;
	struct cpux86_identity id_before;
	struct cpux86_features features;
	struct cpux86_mwait mwait;
	struct cpux86_power power;
	struct cpux86_core_kind kind;
	struct cpux86_core_kind kind_before;
	struct cpux86_cache cache;
	struct cpux86_cache cache_before;
	struct cpux86_topology_level level;
	struct cpux86_topology topology;
	struct cpux86_topology topology_before;
	struct cpux86_extended extended;
	uint32_t iteration;
	uint32_t hybrid;
	uint32_t group;
	uint32_t index;
	uint32_t count;
	uint32_t leaf;
	uint32_t pick;
	uint32_t valid;
	uint32_t shift;
	uint32_t domain;
	uint32_t width;
	uint64_t geometry;
	int error;
	int has;
	unsigned successes_topology;
	unsigned successes_cache;

	successes_topology = 0U;
	successes_cache = 0U;

	/* Runs every decoder over 100000 random register sets. */
	for (iteration = 0U;
	     iteration < 100000U;
	     iteration++) {
		random_regs(&leaf0);
		random_regs(&leaf1);

		/* The identity: 0, EINVAL or ENOTSUP, and an unchanged output on an error. */
		memset(&id, SENTINEL_BYTE, sizeof(id));
		memcpy(&id_before, &id, sizeof(id));
		error = cpux86_cpuid_identity(&leaf0, &leaf1, &id);
		CHECK(error == 0 || error == ENOTSUP);
		if (error == ENOTSUP) {
			CHECK(leaf0.eax == 0U);
			CHECK(memcmp(&id, &id_before, sizeof(id)) == 0);
		} else {
			CHECK(id.max_basic_leaf == leaf0.eax);
			CHECK(id.max_extended_leaf == 0U);
			CHECK(id.vendor == CPUX86_VENDOR_INTEL || id.vendor == CPUX86_VENDOR_UNKNOWN);
			CHECK(id.display_family <= 0x10EU);
			CHECK(id.display_model <= 0xFFU);
			CHECK(id.stepping <= 15U);
			CHECK(id.processor_type <= 3U);
		}

		/* The extended range and the leaf question, for any maximum. */
		error = cpux86_cpuid_extended_range(&leaf1, &id);
		CHECK(error == 0);
		CHECK(id.max_extended_leaf == 0U || id.max_extended_leaf >= CPUX86_EXTENDED_BASE);
		leaf = random_biased();
		has = cpux86_cpuid_has_leaf(&id, leaf);
		CHECK(has == 0 || has == 1);
		if (has == 1 && leaf >= CPUX86_EXTENDED_BASE)
			CHECK(id.max_extended_leaf != 0U && leaf <= id.max_extended_leaf);
		if (has == 1 && leaf < CPUX86_EXTENDED_BASE)
			CHECK(leaf <= id.max_basic_leaf);

		/* The simple leaves: 0 or EINVAL (a non-NULL input is never refused). */
		error = cpux86_cpuid_features(&leaf1, &features);
		CHECK(error == 0);
		CHECK((features.flags & ~0x1FU) == 0U);
		error = cpux86_cpuid_mwait(&leaf1, &mwait);
		CHECK(error == 0);
		CHECK((mwait.flags & ~0x7U) == 0U);
		error = cpux86_cpuid_power(&leaf1, &power);
		CHECK(error == 0);
		CHECK((power.features & ~CPUX86_POWER_DEFINED) == 0U);
		CHECK(power.feedback_pages >= 1U && power.feedback_pages <= 16U);
		error = cpux86_cpuid_hybrid(&leaf1, &hybrid);
		CHECK(error == 0);
		CHECK(hybrid == 0U || hybrid == 1U);
		error = cpux86_cpuid_extended(&leaf0, &leaf1, &extended);
		CHECK(error == 0);
		CHECK(extended.invariant_tsc <= 1U);
		CHECK(extended.physical_address_bits <= 0xFFU);

		/* The core kind: 0 or ENOTSUP only, an unchanged output on ENOTSUP. */
		memset(&kind, SENTINEL_BYTE, sizeof(kind));
		memcpy(&kind_before, &kind, sizeof(kind));
		error = cpux86_cpuid_core_kind(&leaf1, &kind);
		CHECK(error == 0 || error == ENOTSUP);
		if (error == ENOTSUP) {
			CHECK(leaf1.eax == 0U);
			CHECK(memcmp(&kind, &kind_before, sizeof(kind)) == 0);
		}

		/* The cache: the four values the header lists; on success the size is the product. */
		memset(&cache, SENTINEL_BYTE, sizeof(cache));
		memcpy(&cache_before, &cache, sizeof(cache));
		error = cpux86_cpuid_cache(&leaf1, &cache);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP || error == ERANGE);
		if (error != 0) {
			CHECK(memcmp(&cache, &cache_before, sizeof(cache)) == 0);
		} else {
			successes_cache++;
			geometry = (uint64_t)cache.ways * cache.partitions * cache.line_size;
			CHECK(cache.size / geometry == cache.sets);
			CHECK(cache.size % geometry == 0U);
			CHECK(cache.share_shift <= 12U);
			CHECK(((uint64_t)1U << cache.share_shift) >= cache.sharing_ids);
			CHECK(cache.type >= 1U && cache.type <= 3U);
			error = cpux86_cache_group(leaf0.eax, &cache, &group);
			CHECK(error == 0);
			CHECK(group == (leaf0.eax >> cache.share_shift));
		}

		/* One sub-leaf of a topology leaf: always 0. */
		error = cpux86_cpuid_topology_level(&leaf0, &level);
		CHECK(error == 0);
		CHECK(level.type <= 0xFFU);
		CHECK(level.shift <= 31U);

		/*
		 * A topology list: a fully random one half the time, a
		 * structured one (own index, equal x2APIC IDs, rising shifts)
		 * otherwise, with a terminator, in a heap array of exactly the
		 * count so a read past it is caught.
		 */
		pick = random_next();
		count = random_next() % 11U;
		leaf = CPUX86_LEAF_TOPOLOGY;
		if ((pick & 1U) != 0U)
			leaf = CPUX86_LEAF_TOPOLOGY_V2;
		if ((pick & 0x70U) == 0U)
			leaf = random_next();
		list = (struct cpux86_cpuid_regs *)malloc((count + 1U) * sizeof(*list));
		CHECK(list != NULL);
		if (list == NULL)
			continue;
		shift = 0U;
		for (index = 0U;
		     index < count;
		     index++) {
			if ((pick & 0x100U) != 0U) {
				random_regs(&list[index]);
				continue;
			}
			shift += random_next() % 4U;
			if (shift > 31U)
				shift = 31U;
			regbuild_topology(&list[index], index, random_next() % 10U, shift,
			    1U + (random_next() % 5U), 0x1000U);
			if (index == 0U)
				regbuild_topology(&list[index], index, 1U, shift, 1U + (random_next() % 5U), 0x1000U);
			if (index == 1U)
				regbuild_topology(&list[index], index, 2U, shift, 1U + (random_next() % 5U), 0x1000U);
			if (index + 1U == count && count > 2U && (random_next() & 3U) != 0U)
				regbuild_topology(&list[index], index, 0U, 0U, 0U, 0x1000U);
		}
		memset(&topology, SENTINEL_BYTE, sizeof(topology));
		memcpy(&topology_before, &topology, sizeof(topology));
		error = cpux86_cpuid_topology(leaf, list, count, &topology);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
		if (error != 0) {
			CHECK(memcmp(&topology, &topology_before, sizeof(topology)) == 0);
		} else {
			successes_topology++;
			CHECK(leaf == CPUX86_LEAF_TOPOLOGY || leaf == CPUX86_LEAF_TOPOLOGY_V2);
			CHECK((topology.present & 6U) == 6U);
			CHECK((topology.present & ~0x7EU) == 0U);
			CHECK(topology.package_shift <= 31U);
			CHECK(topology.package == (topology.x2apic_id >> topology.package_shift));
			CHECK(topology.id[0] == 0U);
			valid = 0U;
			for (domain = 1U;
			     domain < CPUX86_DOMAIN_COUNT;
			     domain++) {
				if ((topology.present & ((uint32_t)1U << domain)) == 0U) {
					CHECK(topology.id[domain] == 0U);
					CHECK(topology.low[domain] == 0U);
					CHECK(topology.high[domain] == 0U);
					continue;
				}
				valid++;
				CHECK(topology.low[domain] <= topology.high[domain]);
				CHECK(topology.high[domain] <= topology.package_shift);
				width = topology.high[domain] - topology.low[domain];
				if (width < 32U)
					CHECK(((uint64_t)topology.id[domain]) < ((uint64_t)1U << width));
			}
			CHECK(valid >= 2U);
		}
		free(list);
	}

	/* The random lists must have reached the success paths, or the checks above prove little. */
	CHECK(successes_topology > 500U);
	CHECK(successes_cache > 1000U);
}

/*
 * Runs the CPUID decoder checks.
 */
int
main(void)
{
	/* Runs each group of checks. */
	test_identity();
	test_range();
	test_features();
	test_mwait();
	test_power();
	test_hybrid_core_kind();
	test_cache();
	test_topology();
	test_extended();
	test_random();

	/* Reports the summary line. */
	return check_finish("cpuid");
}
