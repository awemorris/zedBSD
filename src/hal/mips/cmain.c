/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS processor's first C code, from locore.S on.
 *
 * The caches the loader leaves are emptied, the loader's tables are
 * copied before anything may overwrite them, the board's display is set
 * up, and what was found is printed on it.  Then
 * the trap handling and the context switch are checked, the caches, the
 * floating-point unit, physical memory and the TLB are set up, the
 * board's interrupt controllers are set up with every line masked, and
 * the tick is let in and seen.  The kernel starts from there; it calls
 * back into hal_cpu_start_others() once its heap and initial task exist,
 * where the address spaces are checked and the bring-up is summed up.
 *
 * The 64-bit processor first checks its direct map, xkphys, which the
 * page allocator hands out.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "cache.h"
#include "clock.h"
#include "cons.h"
#include "cpu.h"
#include "fpu.h"
#include "io.h"
#include "page.h"
#include "selftest.h"
#include "space.h"
#include "spin.h"
#include "task.h"
#include "trap.h"

/* The ticks the boot waits to see before it reports the tick as running. */
#define CMAIN_TICKS_WANTED	10U

/*
 * How long the boot waits for those ticks, in counter cycles per tick:
 * ten intervals' worth for each, so that a slow host does not fail it.
 */
#define CMAIN_TICK_WAIT_FACTOR	10U

/* The pieces of the bring-up's summary line. */
#define CMAIN_READY_PIECES	6U

#if defined(HAL_ARCH_MIPS64)
static void check_direct_map(void);
#endif
static void wait_for_ticks(void);

/*
 * Brings the processor from locore.S to the console.
 *
 * The four arguments are the loader's: the number of entries of its
 * argument table, the KSEG0 addresses of that table and of its
 * environment table, and the size of low memory.
 */
void
mips_cmain(
	uint32_t argument_count,
	uint32_t argument_table,
	uint32_t environment_table,
	uint32_t low_memory_size)
{
	uint32_t status;
	uint32_t exception_base;
	uint32_t processor_id;
	uint32_t config;

	/*
	 * Writes back and empties what the loader left in the caches, so
	 * that none of its lines is written over memory the HAL uses later.
	 */
	mips_cache_flush_all_early();
	mips_board_early_mark(MIPS_EARLY_MARK_C);

	/* Keeps the loader's tables while the memory they are in is untouched. */
	mips_board_boot(argument_count, argument_table, environment_table, low_memory_size);

	/* Shows the display, which then holds what the console printed. */
	mips_board_early_mark(MIPS_EARLY_MARK_DISPLAY);
	mips_board_init();

	/* Describes the processor as locore.S left it. */
	status = mips_read_status();
	exception_base = mips_read_exception_base();
	processor_id = mips_read_processor_id();
	config = mips_read_config();
	hal_printf(MIPS_HAL_NAME " HAL ENTRY prid=%08x status=%08x ebase=%08x config=%08x\n",
		   processor_id,
		   status,
		   exception_base,
		   config);

	/* Describes the board. */
	mips_board_describe();

#if defined(HAL_ARCH_MIPS64)
	/* Reads the image through xkphys, which Status.KX lets the kernel reach. */
	check_direct_map();
#endif

	/* Checks that an exception enters and leaves the trap handling. */
	mips_trap_selftest();

	/* Checks the context switch before any task relies on it. */
	mips_context_selftest();

	/* The caches' shape, the floating-point unit, the free pages and an empty TLB. */
	mips_cache_init();
	mips_fpu_init();
	mips_page_init();
	mips_space_init();

	/* Masks every external line; nothing is registered yet. */
	mips_board_irq_init();

	/* Arms the tick, lets its line and the board's in, and waits for it to run. */
	mips_clock_init();
	wait_for_ticks();

	/*
	 * Hands the ticks to the kernel and enters it with interrupts off:
	 * the kernel turns them on once what a tick reaches is set up.  It
	 * never returns.
	 */
	(void)mips_irq_save();
	mips_clock_start_kernel();
	kernel_entry(mips_board_kernel_handoff());
	HAL_FATAL("mips kernel_entry returned");
}

/*
 * Starts the other processors: there are none, so it checks the address
 * spaces, now that the kernel's heap exists, and sums up the bring-up,
 * which is the HAL's last step of the boot.
 */
int
hal_cpu_start_others(
	void)
{
	const char *display;
	const char *timer;
	const char *pieces[CMAIN_READY_PIECES];
	bool shown;
	uint32_t ticks;

	/* Stops at the first check that fails; returns only when all passed. */
	mips_paging_selftest();

	/* Words the display's state and whether the tick has arrived. */
	display = "NONE";
	shown = mips_board_display_ready();
	if (shown)
		display = mips_board_display_name();
	timer = "NO TICK";
	ticks = mips_clock_ticks();
	if (ticks != 0)
		timer = "TICK";

	/*
	 * The line stays in view after the kernel's own lines; on a narrow
	 * display it breaks between words.
	 */
	pieces[0] = MIPS_HAL_NAME " HAL READY";
	pieces[1] = "DISPLAY";
	pieces[2] = display;
	pieces[3] = "PAGING PASS";
	pieces[4] = "TIMER";
	pieces[5] = timer;
	mips_cons_print_words(pieces, CMAIN_READY_PIECES);
	hal_puts(MIPS_HAL_NAME " CPU 1 ONLINE\n");

	/* Succeeded: the one processor is up. */
	return HAL_OK;
}

/*
 * Reports an exception taken before the trap handling exists, and stops.
 */
void
mips_early_exception(
	uint32_t cause,
	uint32_t exception_pc,
	uint32_t bad_address,
	uint32_t status)
{
	uint32_t code;

	/* The exception's code, from Cause. */
	code = (cause >> MIPS_CAUSE_CODE_SHIFT) & MIPS_CAUSE_CODE_MASK;

	/* Prints the processor's state at the exception. */
	hal_printf(MIPS_HAL_NAME " EARLY EXCEPTION code=%u cause=%08x epc=%08x badvaddr=%08x status=%08x\n",
		   code,
		   cause,
		   exception_pc,
		   bad_address,
		   status);

	/* Shows the code on a board whose console may not show yet. */
	mips_board_early_fault(code);

	/* Stops for good. */
	mips_halt_forever();
}

/*
 * Stops the processor for good, waiting with interrupts off.
 */
void
mips_halt_forever(
	void)
{
	uint32_t status;

	/* Turns interrupts off, so that the wait below never ends. */
	status = mips_read_status();
	status &= ~(uint32_t)MIPS_STATUS_INTERRUPTS;
	__asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(status) : "memory");

	/* Waits for good. */
	for (;;)
		__asm__ volatile("wait" ::: "memory");
}

/* Lets the tick in and waits a bounded time for the first ticks. */
static void
wait_for_ticks(
	void)
{
	uint64_t start;
	uint64_t limit;
	uint64_t now;
	uint32_t status;
	uint32_t ticks;
	uint32_t frequency;

	/* Opens the lines the board's tick and interrupts come on, then interrupts. */
	status = mips_read_status();
	status |= mips_board_interrupt_mask();
	__asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(status) : "memory");
	mips_irq_restore(true);

	/* Waits, by the counter rather than by wait, so that a missing tick is reported. */
	start = mips_clock_read_counter();
	frequency = (uint32_t)mips_clock_frequency();
	limit = (uint64_t)frequency / hal_rtc_get_tick_hz() * CMAIN_TICK_WAIT_FACTOR * CMAIN_TICKS_WANTED;
	for (;;) {
		ticks = mips_clock_ticks();
		if (ticks >= CMAIN_TICKS_WANTED)
			break;
		now = mips_clock_read_counter();
		if (now - start > limit) {
			hal_printf(MIPS_HAL_NAME " HAL TIMER FAIL ticks=%u\n", ticks);
			mips_halt_forever();
		}
	}

	/* Reports the tick with the rate it runs on. */
	hal_printf(MIPS_HAL_NAME " HAL TIMER TICK ticks=%u count=%u Hz\n", ticks, frequency);
}

#if defined(HAL_ARCH_MIPS64)
/* The first word of the exception vectors, which the image holds. */
extern const uint32_t mips_vectors[];

/*
 * Reads the first word of the vectors through CKSEG0, where the image
 * runs, and through the cached and the uncached window of xkphys at its
 * physical address, and prints whether the three agree.
 */
static void
check_direct_map(
	void)
{
	uint64_t physical;
	uint32_t linked;
	uint32_t cached;
	uint32_t uncached;

	/* The image's CKSEG0 address less the segment's base is its physical address. */
	physical = mips_image_physical((uintptr_t)mips_vectors);

	/* The same word through each segment. */
	linked = *(const volatile uint32_t *)mips_vectors;
	cached = *(const volatile uint32_t *)mips_cached_address(physical);
	uncached = *(const volatile uint32_t *)mips_uncached_address(physical);

	/* Reports the physical address and whether the reads agree. */
	if (cached == linked && uncached == linked) {
		hal_printf(MIPS_HAL_NAME " XKPHYS PASS physical=%08llx word=%08x\n", (unsigned long long)physical, linked);
		return;
	}

	/* The reads disagree: names every value read. */
	hal_printf(MIPS_HAL_NAME " XKPHYS FAIL physical=%08llx ckseg0=%08x cached=%08x uncached=%08x\n",
		   (unsigned long long)physical,
		   linked,
		   cached,
		   uncached);
}
#endif
