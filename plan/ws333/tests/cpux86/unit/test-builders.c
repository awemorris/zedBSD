/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the unit test builders themselves: each builder makes the
 * values the specification gives, so the module tests can trust them.
 */

#include "check.h"
#include "feedbackbuild.h"
#include "regbuild.h"

/*
 * Runs the builder checks.
 */
int
main(void)
{
	struct cpux86_cpuid_regs regs;
	uint32_t reg;
	uint64_t value;
	size_t index;

	/* The Intel vendor string, as Table 21-4 spells the three registers. */
	regbuild_leaf0_intel(&regs, 0x20U);
	CHECK(regs.eax == 0x20U);
	CHECK(regs.ebx == 0x756e6547U);
	CHECK(regs.edx == 0x49656e69U);
	CHECK(regs.ecx == 0x6c65746eU);

	/* Family 6, extended model 9, model A, stepping 4: a model 9AH part. */
	CHECK(regbuild_leaf1_eax(6U, 0xAU, 4U, 0U, 0U, 9U) == 0x000906A4U);

	/* Field placement keeps the other bits and reaches both ends. */
	reg = 0xFFFFFFFFU;
	regbuild_put32(&reg, 8U, 8U, 0U);
	CHECK(reg == 0xFFFF00FFU);
	reg = 0U;
	regbuild_put32(&reg, 0U, 32U, 0xDEADBEEFU);
	CHECK(reg == 0xDEADBEEFU);
	value = 0U;
	regbuild_put64(&value, 63U, 1U, 1U);
	CHECK(value == ((uint64_t)1U << 63));
	value = 0U;
	regbuild_put64(&value, 32U, 10U, 0x3FFU);
	CHECK(value == ((uint64_t)0x3FFU << 32));

	/* A sub-leaf of the topology leaf, as Table 21-70 lays it out. */
	regbuild_topology(&regs, 1U, 2U, 6U, 12U, 0x1EU);
	CHECK(regs.eax == 6U);
	CHECK(regs.ebx == 12U);
	CHECK(regs.ecx == 0x0201U);
	CHECK(regs.edx == 0x1EU);

	/* One class of two capabilities is the plain table: rows at 16 + 8 n (Table 17-4). */
	for (index = 0U; index < 4U; index++)
		CHECK(feedbackbuild_cell_offset(2U, 1U, index, 0U, 0U) == 16U + 8U * index);

	/* Four classes of two capabilities fill 8 bytes per row with no padding. */
	CHECK(feedbackbuild_cell_offset(2U, 4U, 0U, 3U, 1U) == 16U + 7U);
	CHECK(feedbackbuild_cell_offset(2U, 4U, 1U, 0U, 0U) == 24U);

	/* Five classes of two capabilities pad each row from 10 to 16 bytes. */
	CHECK(feedbackbuild_cell_offset(2U, 5U, 0U, 0U, 0U) == 24U);
	CHECK(feedbackbuild_cell_offset(2U, 5U, 1U, 0U, 0U) == 40U);

	/* Reports the summary line. */
	return check_finish("builders");
}
