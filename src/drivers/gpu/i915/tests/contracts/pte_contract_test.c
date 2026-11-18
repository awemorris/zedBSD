/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GGTT and PPGTT entry encoding contract, checked on the host.
 *
 * Runs the encoders of ggtt.c and ppgtt.c: the overflow-safe range check,
 * the GGTT entry with its local-memory bit clear, the PPGTT leaf whose bit 1
 * means writable instead, the difference between the two for one address,
 * addresses above 4 GiB kept whole, and the GGTT refusal of a misaligned or
 * out-of-range address instead of masking it into range.
 */

#include "contract.h"

#include "../../dma.h"
#include "../../ggtt.h"
#include "../../ppgtt.h"

#include <stdint.h>

/* A 39-bit DMA mask, the width the Alder Lake-P GGTT reaches. */
#define PTE_TEST_MASK39		((((uint64_t)1) << 39) - 1U)

/* The PAT index whose three bits are all clear. */
#define PTE_TEST_PAT_WB		0U

static void pte_check_range(void);
static void pte_check_ggtt(void);
static void pte_check_ppgtt(void);
static void pte_check_distinct(void);
static void pte_check_wide(void);
static void pte_check_refusals(void);

/*
 * Runs the entry encoding contract checks.
 */
int
main(void)
{
	int status;

	contract_begin("GGTT/PPGTT PTE encoder contract tests (GPU-free)");

	/* Runs each contract group in the order the old suite ran them. */
	pte_check_range();
	pte_check_ggtt();
	pte_check_ppgtt();
	pte_check_distinct();
	pte_check_wide();
	pte_check_refusals();

	/* Reports whether every check held. */
	status = contract_end();
	if (status != 0)
		return status;

	/* Succeeded: the encoders keep their contract. */
	return 0;
}

/* Checks that the whole range, not only its start, must fit under the mask. */
static void
pte_check_range(void)
{
	int in_range;

	contract_section("range: whole [addr,addr+len) must fit under the mask");

	/* A page well inside the mask fits. */
	in_range = drv_i915_dma_in_range(0x1000U, 0x1000U, PTE_TEST_MASK39);
	contract_check(in_range == 1, "in range");

	/* An empty range names nothing. */
	in_range = drv_i915_dma_in_range(0U, 0U, PTE_TEST_MASK39);
	contract_check(in_range == 0, "zero length rejected");

	/* A page that ends exactly at the mask fits; one that crosses it does not. */
	in_range = drv_i915_dma_in_range(PTE_TEST_MASK39 - 0xfffU, 0x1000U, PTE_TEST_MASK39);
	contract_check(in_range == 1, "ends exactly at mask ok");
	in_range = drv_i915_dma_in_range(PTE_TEST_MASK39 - 0xfffU, 0x2000U, PTE_TEST_MASK39);
	contract_check(in_range == 0, "crossing mask rejected");

	/* A range starting at the mask's last byte overflows it. */
	in_range = drv_i915_dma_in_range(PTE_TEST_MASK39, 0x1000U, PTE_TEST_MASK39);
	contract_check(in_range == 0, "addr at mask + len rejected");
}

/* Checks the system-memory GGTT entry. */
static void
pte_check_ggtt(void)
{
	i915_dma_addr_t address;
	uint64_t pte;
	int encoded;

	contract_section("ggtt: SMEM PTE = dma | PRESENT, LM bit clear");

	/* The entry is the address with only PRESENT set. */
	pte = 0U;
	address = drv_i915_dma_addr(0x12345000U);
	encoded = drv_i915_ggtt_pte_encode(address, 0x1000U, PTE_TEST_MASK39, &pte);
	contract_check(encoded == 1, "encode ok");
	contract_check(pte == (0x12345000U | I915_GEN8_PAGE_PRESENT), "PTE = addr | PRESENT");
	contract_check((pte & I915_GEN12_GGTT_PTE_LM) == 0U, "GGTT LM bit (bit1) is clear for SMEM");
}

/* Checks the PPGTT leaf with the PAT index whose bits are all clear. */
static void
pte_check_ppgtt(void)
{
	uint64_t pte;

	contract_section("ppgtt: leaf PTE = dma | PRESENT | RW (bit1 = RW, not LM)");

	/* The leaf is the address with PRESENT and RW set and no PAT bit. */
	pte = drv_i915_ppgtt_leaf_encode(0x12345000U, PTE_TEST_PAT_WB);
	contract_check(pte == (0x12345000U | I915_PPGTT_BIT_PRESENT | I915_PPGTT_BIT_RW), "PTE = addr | PRESENT | RW");
}

/* Checks that the two encoders give one address different entries. */
static void
pte_check_distinct(void)
{
	i915_dma_addr_t address;
	uint64_t ggtt_pte;
	uint64_t ppgtt_pte;
	int encoded;

	contract_section("distinct: GGTT and PPGTT encode the same address differently");

	/* Encodes one address both ways. */
	ggtt_pte = 0U;
	address = drv_i915_dma_addr(0x40000000U);
	encoded = drv_i915_ggtt_pte_encode(address, 0x1000U, PTE_TEST_MASK39, &ggtt_pte);
	contract_check(encoded == 1, "GGTT encode ok");
	ppgtt_pte = drv_i915_ppgtt_leaf_encode(0x40000000U, PTE_TEST_PAT_WB);

	/* Bit 1 is the local-memory bit in one and the writable bit in the other. */
	contract_check(ggtt_pte != ppgtt_pte, "GGTT != PPGTT for the same DMA address");
	contract_check((ggtt_pte & 0x2U) == 0U, "bit1 clear in the GGTT entry (LM)");
	contract_check((ppgtt_pte & 0x2U) != 0U, "bit1 set in the PPGTT leaf (RW)");
}

/* Checks that an address above 4 GiB keeps its high bits in both encoders. */
static void
pte_check_wide(void)
{
	i915_dma_addr_t address;
	uint64_t high;
	uint64_t pte;
	int encoded;

	contract_section("wide: a DMA address above 4 GiB keeps its high bits");

	/* The GGTT entry keeps every address bit. */
	high = 0x0000001234567000ULL;
	pte = 0U;
	address = drv_i915_dma_addr(high);
	encoded = drv_i915_ggtt_pte_encode(address, 0x1000U, PTE_TEST_MASK39, &pte);
	contract_check(encoded == 1, "encode ok");
	contract_check(pte == (high | I915_GEN8_PAGE_PRESENT), "high bits preserved (no 32-bit truncation)");
	contract_check((pte >> 32) != 0U, "PTE really carries bits above 32");

	/* So does the PPGTT leaf. */
	pte = drv_i915_ppgtt_leaf_encode(high, PTE_TEST_PAT_WB);
	contract_check(pte == (high | I915_PPGTT_BIT_PRESENT | I915_PPGTT_BIT_RW), "PPGTT leaf keeps the high bits");
}

/* Checks that the GGTT encoder refuses instead of correcting. */
static void
pte_check_refusals(void)
{
	i915_dma_addr_t address;
	uint64_t sentinel;
	uint64_t over;
	uint64_t pte;
	int encoded;

	contract_section("align: a misaligned DMA address is rejected (not masked into range)");

	/* A misaligned address is refused and the entry left as it was. */
	sentinel = 0xdeadbeefU;
	pte = sentinel;
	address = drv_i915_dma_addr(0x12345800U);
	encoded = drv_i915_ggtt_pte_encode(address, 0x1000U, PTE_TEST_MASK39, &pte);
	contract_check(encoded == 0, "misaligned rejected");
	contract_check(pte == sentinel, "pte_out untouched on rejection");

	contract_section("oob: an out-of-range DMA address is rejected, not corrected");

	/* An address beyond the mask is refused, not ANDed into range. */
	over = ((uint64_t)1) << 40;
	pte = sentinel;
	address = drv_i915_dma_addr(over);
	encoded = drv_i915_ggtt_pte_encode(address, 0x1000U, PTE_TEST_MASK39, &pte);
	contract_check(encoded == 0, "out-of-range rejected");
	contract_check(pte == sentinel, "pte_out untouched on out-of-range");
}
