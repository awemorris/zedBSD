/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS self-test of the address spaces, the TLB and the lazy
 * floating-point switch.
 *
 * It runs once, in the kernel's initial task once the kernel's heap
 * exists, with interrupts off.  The kernel reaches kuseg through the TLB
 * like user code does, so each check maps pages into test spaces, selects
 * them and loads and stores through their user addresses; the TLB's
 * exceptions are counted by the HAL.  A fault the test provokes is taken
 * by its own hook, which steps past the faulting instruction.  Each
 * check stops the processor with its number on the console when it
 * fails, so that the boot's last lines show which one.
 */

#include <hal/hal.h>
#include "asm.h"
#include "cpu.h"
#include "fpu.h"
#include "selftest.h"
#include "space.h"
#include "spin.h"
#include "status.h"
#include "task.h"
#include "trap.h"

/*
 * The user addresses the checks use: an even page and its odd partner,
 * then the next pairs, all in the same leaf table.
 */
#define TEST_EVEN	0x00400000U
#define TEST_ODD	0x00401000U
#define TEST_READ_ONLY	0x00402000U
#define TEST_DIRTY	0x00404000U

/* The word a load that faulted leaves in its register. */
#define TEST_POISON	0xdeadbeefU

/* The ASID limit that makes the generations turn, and the spaces that use it up. */
#define TEST_ASID_LIMIT		4U
#define TEST_GENERATION_SPACES	5U
#define TEST_GENERATION_ROUNDS	2U

/* The most TLB invalid exceptions the half-valid pair may take. */
#define TEST_PAIR_INVALIDS_MAX	2U

/* The pages the checks map: one per role, and one per space of the generation check. */
#define TEST_PAGE_EVEN		0U
#define TEST_PAGE_ODD		1U
#define TEST_PAGE_READ_ONLY	2U
#define TEST_PAGE_DIRTY		3U
#define TEST_PAGE_OTHER		4U
#define TEST_PAGE_GENERATION	5U
#define TEST_PAGES		(TEST_PAGE_GENERATION + TEST_GENERATION_SPACES)

/*
 * The floating-point control values of the two contexts: round toward
 * zero for the first; for the second the invalid operation's Enable bit
 * with its Cause bit, which would raise the exception at the write were
 * the Cause field not cleared first.
 */
#define TEST_FCSR_FIRST		0x00000001U
#define TEST_FCSR_SECOND	0x00010800U

/*
 * The faults the hook took since the test began and what the space found
 * for the last one.  Written by the hook, inside the trap handling, and
 * read by the checks; interrupts are off throughout.
 */
static volatile uint32_t test_faults;
static volatile int test_last_fault;

/*
 * The physical pages the checks map, freed at the end.
 */
static hal_physaddr_t test_pages[TEST_PAGES];

/*
 * The two floating-point contexts of the lazy switch check: their saved
 * states, the patterns they load and what they read back.  Used once.
 */
static struct hal_fpregs test_state_first __attribute__((aligned(8)));
static struct hal_fpregs test_state_second __attribute__((aligned(8)));
static struct hal_fpregs test_pattern_first __attribute__((aligned(8)));
static struct hal_fpregs test_pattern_second __attribute__((aligned(8)));
static struct hal_fpregs test_read_first __attribute__((aligned(8)));
static struct hal_fpregs test_read_second __attribute__((aligned(8)));

static void fail(unsigned check) __attribute__((noreturn));
static int fault_hook(const struct mips_trap_frame *frame, int fault);
static uint32_t probe_load(uintptr_t address);
static void probe_store(uintptr_t address, uint32_t value);
static void fill_page(unsigned page, uint32_t value);
static uint32_t page_word(unsigned page);
static void map_page(hal_space_t space, uintptr_t address, unsigned page, uint32_t attr, unsigned check);
static uint32_t query(hal_space_t space, uintptr_t address, unsigned check);
static void check_refill(hal_space_t space);
static void check_pair(hal_space_t space);
static void check_accessed(hal_space_t space);
static void check_dirty(hal_space_t space);
static void check_protection(hal_space_t space);
static hal_space_t check_two_spaces(hal_space_t first);
static void check_generations(void);
static void check_flush(hal_space_t first, hal_space_t second);
static void check_float(void);
static void check_user_status(void);

/*
 * Runs every check, and reports on the console that the spaces passed.
 *
 * Stops the processor at the first failed check.
 */
void
mips_paging_selftest(
	void)
{
	struct mips_space_counters counters;
	hal_space_t first;
	hal_space_t second;
	unsigned index;
	bool enabled;
	int error;

	/* The pages, each a page of RAM of its own. */
	for (index = 0; index < TEST_PAGES; index++) {
		error = hal_pmem_alloc(MIPS_PAGE_SIZE, MIPS_PAGE_SIZE, &test_pages[index]);
		if (error != HAL_OK)
			fail(0);
	}

	/* The first space. */
	first = hal_space_create();
	if (first == HAL_SPACE_SYS)
		fail(0);

	/* Runs the checks of the TLB with interrupts off and the test's fault hook in place. */
	enabled = mips_irq_save();
	mips_trap_set_fault_hook(fault_hook);

	/* Runs each check of the TLB in turn; a failed one stops the boot. */
	check_refill(first);
	check_pair(first);
	check_accessed(first);
	check_dirty(first);
	check_protection(first);
	second = check_two_spaces(first);
	check_generations();
	check_flush(first, second);

	/* Takes the hook away and goes back to the kernel's space. */
	mips_trap_set_fault_hook(NULL);
	hal_space_switch(HAL_SPACE_SYS);
	mips_irq_restore(enabled);

	/* The floating-point switch, with no switch of the kernel's between its steps. */
	enabled = mips_irq_save();
	check_float();
	mips_irq_restore(enabled);

	/* The Status of a return to user. */
	check_user_status();

	/* Gives the spaces and the pages back. */
	hal_space_destroy(first);
	hal_space_destroy(second);
	for (index = 0; index < TEST_PAGES; index++)
		(void)hal_pmem_free(&test_pages[index], MIPS_PAGE_SIZE);

	/* Reports what the checks made the TLB do. */
	mips_space_counters(&counters);
	hal_printf(MIPS_HAL_NAME " HAL PAGING PASS refills=%u invalids=%u modifies=%u generation=%llu\n",
		   counters.refills,
		   counters.invalids,
		   counters.modifies,
		   mips_space_generation());
}

/* Reports a failed check by its number, and stops. */
static void
fail(
	unsigned check)
{
	/* The number tells which check of mips_paging_selftest() failed; 0 is its setup. */
	hal_printf(MIPS_HAL_NAME " HAL PAGING FAIL %u\n", check);
	mips_halt_forever();
}

/*
 * Takes a fault the test provoked: counts it and keeps what the space
 * found, so that the faulting instruction is stepped over.  A fault in a
 * branch delay slot cannot be stepped over and is left to stop the
 * kernel.
 */
static int
fault_hook(
	const struct mips_trap_frame *frame,
	int fault)
{
	/* The step past the instruction would land on the wrong one in a delay slot. */
	if ((frame->cause & MIPS_CAUSE_BRANCH_DELAY) != 0)
		return 0;

	/* Records the fault for the check that provoked it. */
	test_faults++;
	test_last_fault = fault;

	/* Succeeded: the fault is the test's. */
	return 1;
}

/*
 * Loads a word through a user address.  The load is its own instruction
 * outside any delay slot, and a load that faults leaves the poison word.
 */
static uint32_t
probe_load(
	uintptr_t address)
{
	uint32_t value;

	/* The load into a register that holds the poison word. */
	value = TEST_POISON;
	__asm__ volatile(".set push\n\t"
			 ".set noreorder\n\t"
			 "lw %0, 0(%1)\n\t"
			 "nop\n\t"
			 ".set pop"
			 : "+r"(value)
			 : "r"(address)
			 : "memory");

	/* Succeeded: the word, or the poison word after a fault. */
	return value;
}

/* Stores a word through a user address, as its own instruction outside any delay slot. */
static void
probe_store(
	uintptr_t address,
	uint32_t value)
{
	/* The store. */
	__asm__ volatile(".set push\n\t"
			 ".set noreorder\n\t"
			 "sw %0, 0(%1)\n\t"
			 "nop\n\t"
			 ".set pop"
			 :
			 : "r"(value), "r"(address)
			 : "memory");
}

/* Writes a test page's first word through the kernel's view of it. */
static void
fill_page(
	unsigned page,
	uint32_t value)
{
	volatile uint32_t *word;

	/* The word through KSEG0. */
	word = hal_pmem_to_kernel(test_pages[page]);
	*word = value;
}

/* Reads a test page's first word through the kernel's view of it. */
static uint32_t
page_word(
	unsigned page)
{
	volatile uint32_t *word;

	/* The word through KSEG0. */
	word = hal_pmem_to_kernel(test_pages[page]);

	/* Succeeded: the word. */
	return *word;
}

/* Maps one test page, stopping with the check's number when it cannot. */
static void
map_page(
	hal_space_t space,
	uintptr_t address,
	unsigned page,
	uint32_t attr,
	unsigned check)
{
	int error;

	/* The mapping. */
	error = hal_space_map(space, (void *)address, test_pages[page], MIPS_PAGE_SIZE, attr);
	if (error != HAL_OK)
		fail(check);
}

/* Reports one page's recorded state, stopping with the check's number when it cannot. */
static uint32_t
query(
	hal_space_t space,
	uintptr_t address,
	unsigned check)
{
	uint32_t flags;
	int error;

	/* The space's record of the page. */
	error = hal_space_query(space, (void *)address, &flags);
	if (error != HAL_OK)
		fail(check);

	/* Succeeded: the page's flags. */
	return flags;
}

/*
 * Check 1: a mapped page is reached through one refill, and what is
 * stored through it is read back.
 */
static void
check_refill(
	hal_space_t space)
{
	struct mips_space_counters before;
	struct mips_space_counters after;
	uint32_t value;
	uint32_t stored;

	/* The even page, writable, in the selected space. */
	map_page(space, TEST_EVEN, TEST_PAGE_EVEN, HAL_SPACE_READ | HAL_SPACE_WRITE, 1);
	hal_space_switch(space);

	/* A store and a load through the user address. */
	mips_space_counters(&before);
	probe_store(TEST_EVEN, 0x11111111U);
	value = probe_load(TEST_EVEN);
	mips_space_counters(&after);
	stored = page_word(TEST_PAGE_EVEN);

	/* The value went to the page, through exactly one refill. */
	if (value != 0x11111111U || stored != 0x11111111U)
		fail(1);
	if (after.refills - before.refills != 1U)
		fail(1);
}

/*
 * Check 2: a pair the TLB holds with its odd page invalid takes the odd
 * page as soon as it is mapped, without an endless TLB invalid loop.
 */
static void
check_pair(
	hal_space_t space)
{
	struct mips_space_counters before;
	struct mips_space_counters after;
	uint32_t value;
	uint32_t stored;

	/* The pair is in the TLB from the even page's use, the odd page still invalid. */
	value = probe_load(TEST_EVEN);
	if (value != 0x11111111U)
		fail(2);

	/* Maps the odd page and stores and loads through it. */
	map_page(space, TEST_ODD, TEST_PAGE_ODD, HAL_SPACE_READ | HAL_SPACE_WRITE, 2);
	mips_space_counters(&before);
	probe_store(TEST_ODD, 0x22222222U);
	value = probe_load(TEST_ODD);
	mips_space_counters(&after);
	stored = page_word(TEST_PAGE_ODD);

	/* The value went to the odd page, after at most two invalid exceptions. */
	if (value != 0x22222222U || stored != 0x22222222U)
		fail(2);
	if (after.invalids - before.invalids > TEST_PAIR_INVALIDS_MAX)
		fail(2);
}

/*
 * Check 3: a page is recorded as accessed at its first use, and clearing
 * the record makes the next use record it again.
 */
static void
check_accessed(
	hal_space_t space)
{
	uint32_t flags;
	uint32_t value;
	int error;

	/* A read-only page, unused after its mapping. */
	fill_page(TEST_PAGE_READ_ONLY, 0x33333333U);
	map_page(space, TEST_READ_ONLY, TEST_PAGE_READ_ONLY, HAL_SPACE_READ, 3);
	flags = query(space, TEST_READ_ONLY, 3);
	if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0)
		fail(3);

	/* The first load records the access. */
	value = probe_load(TEST_READ_ONLY);
	flags = query(space, TEST_READ_ONLY, 3);
	if (value != 0x33333333U || (flags & HAL_SPACE_PAGE_ACCESSED) == 0)
		fail(3);

	/* Cleared, the record comes back with the next load. */
	error = hal_space_clear_flags(space, (void *)TEST_READ_ONLY, HAL_SPACE_PAGE_ACCESSED);
	if (error != HAL_OK)
		fail(3);
	flags = query(space, TEST_READ_ONLY, 3);
	if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0)
		fail(3);
	value = probe_load(TEST_READ_ONLY);
	flags = query(space, TEST_READ_ONLY, 3);
	if (value != 0x33333333U || (flags & HAL_SPACE_PAGE_ACCESSED) == 0)
		fail(3);
}

/*
 * Check 4: the first store to a page that was only read takes a TLB
 * modified exception and records the page as dirty; a store to a page
 * that is not writable is a protection fault and changes nothing.
 */
static void
check_dirty(
	hal_space_t space)
{
	struct mips_space_counters before;
	struct mips_space_counters after;
	uint32_t flags;
	uint32_t faults;
	uint32_t value;
	uint32_t stored;

	/* A writable page, read first so that its pair is valid and clean. */
	fill_page(TEST_PAGE_DIRTY, 0x44444444U);
	map_page(space, TEST_DIRTY, TEST_PAGE_DIRTY, HAL_SPACE_READ | HAL_SPACE_WRITE, 4);
	value = probe_load(TEST_DIRTY);
	flags = query(space, TEST_DIRTY, 4);
	if (value != 0x44444444U || (flags & HAL_SPACE_PAGE_DIRTY) != 0)
		fail(4);

	/* The first store records the write through one modified exception. */
	mips_space_counters(&before);
	probe_store(TEST_DIRTY, 0x45454545U);
	mips_space_counters(&after);
	flags = query(space, TEST_DIRTY, 4);
	stored = page_word(TEST_PAGE_DIRTY);
	if ((flags & HAL_SPACE_PAGE_DIRTY) == 0 || stored != 0x45454545U)
		fail(4);
	if (after.modifies - before.modifies != 1U)
		fail(4);

	/* A store to the read-only page is a protection fault, and the page keeps its word. */
	faults = test_faults;
	probe_store(TEST_READ_ONLY, 0x46464646U);
	if (test_faults != faults + 1U || test_last_fault != MIPS_SPACE_FAULT_PROTECTION)
		fail(4);
	stored = page_word(TEST_PAGE_READ_ONLY);
	if (stored != 0x33333333U)
		fail(4);
}

/*
 * Check 5: a page without access is a protection fault, an unmapped one
 * a missing page, and the two are told apart.
 */
static void
check_protection(
	hal_space_t space)
{
	uint32_t faults;
	uint32_t value;
	int error;

	/* No access: the page stays mapped, and a load is a protection fault. */
	error = hal_space_prot(space, (void *)TEST_DIRTY, MIPS_PAGE_SIZE, HAL_SPACE_NONE);
	if (error != HAL_OK)
		fail(5);
	faults = test_faults;
	value = probe_load(TEST_DIRTY);
	if (value != TEST_POISON || test_faults != faults + 1U)
		fail(5);
	if (test_last_fault != MIPS_SPACE_FAULT_PROTECTION)
		fail(5);

	/* Unmapped: a load finds no page. */
	error = hal_space_unmap(space, (void *)TEST_DIRTY, MIPS_PAGE_SIZE);
	if (error != HAL_OK)
		fail(5);
	value = probe_load(TEST_DIRTY);
	if (value != TEST_POISON || test_faults != faults + 2U)
		fail(5);
	if (test_last_fault != MIPS_SPACE_FAULT_MISSING)
		fail(5);
}

/*
 * Check 6: two spaces map the same user address to different pages, and
 * each sees its own while they are selected in turn.  Reports the second
 * space.
 */
static hal_space_t
check_two_spaces(
	hal_space_t first)
{
	hal_space_t second;
	uint32_t value;
	unsigned turn;

	/* The second space, its page at the first space's even address. */
	second = hal_space_create();
	if (second == HAL_SPACE_SYS)
		fail(6);
	fill_page(TEST_PAGE_OTHER, 0x66666666U);
	map_page(second, TEST_EVEN, TEST_PAGE_OTHER, HAL_SPACE_READ, 6);

	/* First, second, first: each load sees the selected space's page. */
	for (turn = 0; turn < 3U; turn++) {
		if ((turn & 1U) == 0) {
			hal_space_switch(first);
			value = probe_load(TEST_EVEN);
			if (value != 0x11111111U)
				fail(6);
		} else {
			hal_space_switch(second);
			value = probe_load(TEST_EVEN);
			if (value != 0x66666666U)
				fail(6);
		}
	}

	/* Succeeded: the second space, for the flush check. */
	return second;
}

/*
 * Check 7: with the ASIDs of a generation lowered to four, five spaces
 * selected in turn make the generation turn, and every load still sees
 * its own space's page.
 */
static void
check_generations(
	void)
{
	hal_space_t spaces[TEST_GENERATION_SPACES];
	uint64_t generation;
	uint64_t turned;
	uint32_t previous_limit;
	uint32_t value;
	unsigned index;
	unsigned round;

	/* Five spaces, each with its own page at the same address. */
	for (index = 0; index < TEST_GENERATION_SPACES; index++) {
		spaces[index] = hal_space_create();
		if (spaces[index] == HAL_SPACE_SYS)
			fail(7);
		fill_page(TEST_PAGE_GENERATION + index, 0x70000000U + index);
		map_page(spaces[index], TEST_EVEN, TEST_PAGE_GENERATION + index, HAL_SPACE_READ, 7);
	}

	/* Two rounds over the five with four ASIDs a generation. */
	generation = mips_space_generation();
	previous_limit = mips_space_set_asid_limit(TEST_ASID_LIMIT);
	for (round = 0; round < TEST_GENERATION_ROUNDS; round++) {
		for (index = 0; index < TEST_GENERATION_SPACES; index++) {
			hal_space_switch(spaces[index]);
			value = probe_load(TEST_EVEN);
			if (value != 0x70000000U + index)
				fail(7);
		}
	}

	/* Puts back the ASID limit of the boot. */
	(void)mips_space_set_asid_limit(previous_limit);

	/* The generation turned at least once. */
	turned = mips_space_generation();
	if (turned == generation)
		fail(7);

	/* Gives the five spaces back, none of them selected. */
	hal_space_switch(HAL_SPACE_SYS);
	for (index = 0; index < TEST_GENERATION_SPACES; index++)
		hal_space_destroy(spaces[index]);
}

/*
 * Check 8: a space flushed while not selected takes a new ASID when it is
 * selected again and still sees its page; a range flush makes the next
 * use refill; and a destroyed space's ASID is not given to the next space
 * in the same generation.
 */
static void
check_flush(
	hal_space_t first,
	hal_space_t second)
{
	struct mips_space_counters before;
	struct mips_space_counters after;
	hal_space_t doomed;
	hal_space_t fresh;
	uint64_t generation;
	uint64_t later;
	uint32_t doomed_asid;
	uint32_t asid;
	uint32_t value;

	/* The second space, used and then not selected, is flushed and gives up its ASID. */
	hal_space_switch(second);
	(void)probe_load(TEST_EVEN);
	hal_space_switch(first);
	hal_space_flush_tlb(second);
	asid = mips_space_asid(second);
	if (asid != 0)
		fail(8);

	/* Selected again, it has a new ASID and its own page. */
	hal_space_switch(second);
	value = probe_load(TEST_EVEN);
	asid = mips_space_asid(second);
	if (value != 0x66666666U || asid == 0)
		fail(8);

	/* A range flush drops the pair: the next load refills it. */
	mips_space_counters(&before);
	hal_space_flush_tlb_range(second, (void *)TEST_EVEN, MIPS_PAGE_SIZE);
	value = probe_load(TEST_EVEN);
	mips_space_counters(&after);
	if (value != 0x66666666U || after.refills - before.refills != 1U)
		fail(8);

	/* A destroyed space's ASID stays out of use for the rest of its generation. */
	doomed = hal_space_create();
	if (doomed == HAL_SPACE_SYS)
		fail(8);
	hal_space_switch(doomed);
	doomed_asid = mips_space_asid(doomed);
	generation = mips_space_generation();
	hal_space_switch(HAL_SPACE_SYS);
	hal_space_destroy(doomed);
	fresh = hal_space_create();
	if (fresh == HAL_SPACE_SYS)
		fail(8);
	hal_space_switch(fresh);
	later = mips_space_generation();
	asid = mips_space_asid(fresh);
	if (later == generation && asid == doomed_asid)
		fail(8);
	hal_space_switch(HAL_SPACE_SYS);
	hal_space_destroy(fresh);
}

/*
 * Check 9: two contexts load their own floating-point registers and
 * control values in turn through the lazy switch, and each reads its own
 * back; the second context's control value, whose Cause bit is set with
 * its Enable bit, goes in without an exception and with its Cause field
 * clear.
 */
static void
check_float(
	void)
{
	struct mips_task *task;
	uint32_t expected;
	unsigned index;
	bool usable;

	/* A processor without a usable unit has nothing to check. */
	usable = mips_fpu_usable();
	if (!usable) {
		hal_puts(MIPS_HAL_NAME " HAL PAGING FPU NONE\n");
		return;
	}

	/* Two patterns that differ in every register. */
	for (index = 0; index < 32U; index++) {
		test_pattern_first.f[index] = 0x1000000000000000ULL + index;
		test_pattern_second.f[index] = 0x2000000000000000ULL + index;
	}

	/* Two control registers that differ too. */
	test_pattern_first.fcsr = TEST_FCSR_FIRST;
	test_pattern_second.fcsr = TEST_FCSR_SECOND;

	/*
	 * Each context loads its pattern while the unit is off for it: the
	 * first instruction takes the unit, saving the other context's
	 * registers to its state.
	 */
	mips_fpu_select(&test_state_first, &test_state_first);
	mips_fpu_restore(&test_pattern_first);
	mips_fpu_select(&test_state_second, &test_state_second);
	mips_fpu_restore(&test_pattern_second);

	/* Each reads its registers back after the other ran. */
	mips_fpu_select(&test_state_first, &test_state_first);
	mips_fpu_save(&test_read_first);
	mips_fpu_select(&test_state_second, &test_state_second);
	mips_fpu_save(&test_read_second);

	/* Every register is the context's own. */
	for (index = 0; index < 32U; index++) {
		if (test_read_first.f[index] != test_pattern_first.f[index])
			fail(9);
		if (test_read_second.f[index] != test_pattern_second.f[index])
			fail(9);
	}

	/* The control values, the second without its Cause field. */
	if (test_read_first.fcsr != TEST_FCSR_FIRST)
		fail(9);
	expected = mips_float_control_sanitize(TEST_FCSR_SECOND);
	if (test_read_second.fcsr != expected)
		fail(9);

	/* The unit goes back to the running task, holding nothing of the test's. */
	mips_fpu_forget(&test_state_first);
	mips_fpu_forget(&test_state_second);
	task = mips_task_running();
	mips_fpu_select(task, &task->fpregs);
}

/*
 * Check 10: the Status of a return to user carries none of the kernel's
 * privileges whatever the current Status holds: user mode at the
 * exception level, interrupts on, and only the interrupt mask taken over.
 */
static void
check_user_status(
	void)
{
	uint32_t status;

	/* Every bit set in the current Status, the unit used but not owned. */
	status = mips_user_status(0xffffffffU, false, true);

	/* No coprocessor 0, error level or boot vectors; no coprocessor 1 for a non-owner. */
	if ((status & (MIPS_STATUS_COPROCESSOR_0 | MIPS_STATUS_ERROR_LEVEL | MIPS_STATUS_BOOT_VECTORS)) != 0)
		fail(10);
	if ((status & MIPS_STATUS_COPROCESSOR_1) != 0)
		fail(10);

	/* User mode at the exception level with interrupts on, under the current mask. */
	if ((status & MIPS_STATUS_MODE_MASK) != MIPS_STATUS_MODE_USER)
		fail(10);
	if ((status & MIPS_STATUS_EXCEPTION_LEVEL) == 0 || (status & MIPS_STATUS_INTERRUPTS) == 0)
		fail(10);
	if ((status & MIPS_STATUS_INTERRUPT_MASK) != MIPS_STATUS_INTERRUPT_MASK)
		fail(10);
}
