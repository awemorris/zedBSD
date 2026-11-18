/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lazy switch of the armv7 floating-point and Advanced SIMD unit.
 *
 * The unit of each processor holds the registers of one context, its
 * owner, and is turned off (FPEXC.EN clear) at every switch.  The first
 * floating-point instruction of the context that runs then is an
 * undefined instruction; the trap turns the unit on, loads the running
 * context's registers unless the unit still holds them, makes it the
 * owner and runs the same instruction again.  An instruction that is
 * undefined for another reason is undefined again with the unit on, and
 * the trap reports it then.
 *
 * Two rules keep the registers in memory current.  While the unit is off
 * on a processor, its owner's saved registers equal the unit's, so taking
 * the unit for another context never needs to save the owner first.  And
 * the unit is on only for the context that runs, so a switch that finds
 * it on saves that context's registers before turning it off: a context
 * that is moved to another processor is loaded there from memory, and a
 * context that comes back to a processor it left is loaded again unless
 * that processor's unit was the last to load it (Procedure referenced:
 * Linux v6.12 arch/arm/vfp/vfpmodule.c, vfp_notifier THREAD_NOTIFY_SWITCH
 * on SMP and vfp_state_in_hw).
 *
 * The kernel itself is built without the unit and never uses it.
 *
 * Values referenced: ARM DDI 0406C, CPACR (cp10 and cp11 access in bits
 * 23 to 20, D32DIS bit 30, ASEDIS bit 31), FPSID (SW bit 23), MVFR0
 * (A_SIMD registers in bits 3 to 0: 1 for sixteen, 2 for thirty-two
 * doubleword registers) and FPSCR (the bits an implementation without
 * trapping of floating-point exceptions and without short vectors keeps:
 * N, Z, C, V, QC, AHP, DN, FZ and RMode in bits 31 to 22, IDC in bit 7 and
 * the cumulative exception bits 4 to 0).
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "fpu.h"
#include "percpu.h"

/* CPACR: full access to cp10 and cp11 from every mode. */
#define FPU_CPACR_FULL		0x00f00000U

/* CPACR: the bits that would turn the upper registers and Advanced SIMD off. */
#define FPU_CPACR_DISABLES	0xc0000000U

/* FPSID: the unit is a software emulation, not hardware. */
#define FPU_FPSID_SOFTWARE	0x00800000U

/* MVFR0: the field and the values that count the doubleword registers. */
#define FPU_MVFR0_REGISTERS	0x0000000fU
#define FPU_MVFR0_SIXTEEN	1U
#define FPU_MVFR0_THIRTY_TWO	2U

/* FPSCR: the bits a context may hold. */
#define FPU_FPSCR_KEPT		0xffc0009fU

/*
 * Nonzero when the processor has a hardware unit the HAL switches.  Set
 * once by armv7_fpu_init() on the boot processor before any context runs.
 */
static int fpu_usable;

/*
 * Nonzero when the unit has thirty-two doubleword registers, zero when it
 * has sixteen; the save and the load move the upper sixteen only then.
 * Set with fpu_usable.
 */
static uint32_t fpu_upper_bank;

/*
 * How many times a unit loaded a context's registers from memory since
 * boot.  It only grows; the self-test reads it to see that a context the
 * unit still holds is not loaded again.
 */
static unsigned long fpu_loads;

static void save_owner_if_on(struct armv7_percpu *cpu);

_Static_assert(__builtin_offsetof(struct hal_fpregs, fpscr) == ARMV7_FPREGS_FPSCR,
	       "fpregs.S stores the status and control register at this offset");

/*
 * Gives the calling processor's modes the unit, turns it off for the
 * lazy switch and reports what it is.
 *
 * A processor whose access cannot be granted, or whose unit is a
 * software emulation, leaves the unit unused: every floating-point
 * instruction is then an illegal instruction.
 */
void
armv7_fpu_init(
	void)
{
	uint32_t cpacr;
	uint32_t granted;
	uint32_t fpsid;
	uint32_t mvfr0;
	uint32_t registers;

	/* Grants cp10 and cp11 with the upper registers and Advanced SIMD on. */
	cpacr = armv7_read_cpacr();
	cpacr |= FPU_CPACR_FULL;
	cpacr &= ~FPU_CPACR_DISABLES;
	armv7_write_cpacr(cpacr);

	/* A processor without the unit (or a secure state that keeps it) leaves the bits clear. */
	granted = armv7_read_cpacr();
	if ((granted & FPU_CPACR_FULL) != FPU_CPACR_FULL) {
		hal_printf("ARMV7 VFP NONE cpacr=%08x\n", granted);
		return;
	}

	/* The unit starts off, so the first floating-point instruction takes it. */
	armv7_fpu_write_fpexc(0);

	/* A software emulation is no unit to switch. */
	fpsid = armv7_fpu_read_fpsid();
	if ((fpsid & FPU_FPSID_SOFTWARE) != 0) {
		hal_printf("ARMV7 VFP NONE fpsid=%08x\n", fpsid);
		return;
	}

	/* Counts the doubleword registers the save and the load move. */
	mvfr0 = armv7_fpu_read_mvfr0();
	registers = mvfr0 & FPU_MVFR0_REGISTERS;
	if (registers == FPU_MVFR0_THIRTY_TWO) {
		fpu_upper_bank = 1U;
	} else if (registers == FPU_MVFR0_SIXTEEN) {
		fpu_upper_bank = 0U;
	} else {
		hal_printf("ARMV7 VFP NONE mvfr0=%08x\n", mvfr0);
		return;
	}

	/* Succeeded: the unit is switched lazily from here on. */
	fpu_usable = 1;
	hal_printf("ARMV7 VFP fpsid=%08x mvfr0=%08x registers=%u\n",
		   fpsid,
		   mvfr0,
		   armv7_fpu_register_count());
}

/*
 * Reports whether the processor's unit is switched.
 */
int
armv7_fpu_usable(
	void)
{
	/* Succeeded: nonzero when the unit is switched, set once at boot. */
	return fpu_usable;
}

/*
 * Reports how many doubleword registers the unit has, 16 or 32.
 */
unsigned
armv7_fpu_register_count(
	void)
{
	/* Succeeded: sixteen, and sixteen more with the upper bank. */
	return 16U + 16U * fpu_upper_bank;
}

/*
 * Prepares a context's registers: all zero, with the default status and
 * control, held by no unit.
 */
void
armv7_fpu_context_init(
	struct armv7_fpu_context *context)
{
	/* Zero registers and a zero FPSCR: round to nearest, no flush, no exception. */
	hal_memset(&context->regs, 0, sizeof(context->regs));

	/* No unit has loaded the registers. */
	context->loaded_cpu = ARMV7_FPU_NOWHERE;
}

/*
 * Names the context that runs next on the calling processor, at a
 * switch, and turns the unit off for it.
 *
 * A unit found on holds the departing context's registers, which are
 * saved first so that the context can be loaded from memory wherever it
 * runs next.
 */
void
armv7_fpu_select(
	struct armv7_fpu_context *context)
{
	struct armv7_percpu *cpu;
	bool enabled;

	/* Changes the processor's unit and its record as one step. */
	enabled = armv7_irq_save();

	cpu = armv7_percpu_current();
	if (fpu_usable)
		save_owner_if_on(cpu);
	cpu->fpu_selected = context;

	armv7_irq_restore(enabled);
}

/*
 * Reports the context selected on the calling processor, or NULL.
 */
struct armv7_fpu_context *
armv7_fpu_selected(
	void)
{
	struct armv7_fpu_context *context;
	struct armv7_percpu *cpu;
	bool enabled;

	/* Reads the processor's record without being moved to another. */
	enabled = armv7_irq_save();

	cpu = armv7_percpu_current();
	context = cpu->fpu_selected;

	armv7_irq_restore(enabled);

	/* Succeeded: the selected context. */
	return context;
}

/*
 * Reports the context whose registers the calling processor's unit
 * holds, or NULL.
 */
struct armv7_fpu_context *
armv7_fpu_owner(
	void)
{
	struct armv7_fpu_context *context;
	struct armv7_percpu *cpu;
	bool enabled;

	/* Reads the processor's record without being moved to another. */
	enabled = armv7_irq_save();

	cpu = armv7_percpu_current();
	context = cpu->fpu_owner;

	armv7_irq_restore(enabled);

	/* Succeeded: the owner. */
	return context;
}

/*
 * Takes the unit for the selected context at an undefined instruction,
 * and reports whether the instruction should run again.
 *
 * Only an instruction of user code is taken, and a kernel one only while
 * the self-test asks for it.  Called from the trap with interrupts
 * masked.
 */
int
armv7_fpu_take_trap(
	int from_user)
{
	struct armv7_fpu_context *context;
	struct armv7_percpu *cpu;
	uint32_t fpexc;
	int cpu_number;

	/* Without a unit every such instruction is illegal. */
	if (!fpu_usable)
		return 0;

	/* A unit already on did not refuse the instruction: it is undefined for another reason. */
	fpexc = armv7_fpu_read_fpexc();
	if ((fpexc & ARMV7_FPEXC_EN) != 0)
		return 0;

	/* The kernel never uses the unit, but for the self-test. */
	cpu = armv7_percpu_current();
	if (!from_user && !cpu->fpu_kernel_test)
		return 0;

	/* A processor that has not switched to a context has none to take the unit for. */
	context = cpu->fpu_selected;
	if (context == NULL)
		return 0;

	/* Turns the unit on; its owner's registers are in memory while it was off. */
	armv7_fpu_write_fpexc(ARMV7_FPEXC_EN);

	/* A context this unit was the last to load still has its registers here. */
	cpu_number = (int)cpu->logical_id;
	if (cpu->fpu_owner == context && context->loaded_cpu == cpu_number)
		return 1;

	/* Loads the context's registers and makes it the owner. */
	armv7_fpu_load(&context->regs, fpu_upper_bank);
	cpu->fpu_owner = context;
	context->loaded_cpu = cpu_number;
	fpu_loads++;

	/* Succeeded: the instruction runs again with the unit on. */
	return 1;
}

/*
 * Clears a floating-point exception the unit reports at an undefined
 * instruction of user code, and reports whether there was one.
 *
 * The Cortex-A9's unit completes every operation in hardware and is not
 * expected to report one; a unit that does stops the instruction, which
 * the kernel signals as an arithmetic fault.
 */
int
armv7_fpu_take_exception(
	void)
{
	uint32_t fpexc;

	/* Without a unit there is no exception to report. */
	if (!fpu_usable)
		return 0;

	/* Only a unit that is on can report an exception. */
	fpexc = armv7_fpu_read_fpexc();
	if ((fpexc & ARMV7_FPEXC_EN) == 0)
		return 0;

	/* No exceptional state: the instruction is simply undefined. */
	if ((fpexc & (ARMV7_FPEXC_EX | ARMV7_FPEXC_DEX)) == 0)
		return 0;

	/* Succeeded: the state is cleared and the unit stays on. */
	armv7_fpu_write_fpexc(ARMV7_FPEXC_EN);
	return 1;
}

/*
 * Brings a context's saved registers up to date with the unit.
 *
 * Only the context whose registers this processor's unit holds, while the
 * unit is on for it, has newer registers there; every other context's are
 * in memory already.
 */
void
armv7_fpu_sync(
	struct armv7_fpu_context *context)
{
	struct armv7_percpu *cpu;
	uint32_t fpexc;
	bool enabled;

	/* Without a unit the saved registers are the only ones. */
	if (!fpu_usable)
		return;

	/* Saves the unit's registers as one step against a switch. */
	enabled = armv7_irq_save();

	cpu = armv7_percpu_current();
	fpexc = armv7_fpu_read_fpexc();
	if (cpu->fpu_owner == context && (fpexc & ARMV7_FPEXC_EN) != 0)
		armv7_fpu_save(&context->regs, fpu_upper_bank);

	armv7_irq_restore(enabled);
}

/*
 * Makes the units drop a context's registers, whose saved copy was
 * replaced or is about to be freed.
 *
 * The calling processor's unit is turned off when it held them, without
 * saving them; any other unit that held them is no longer taken as
 * holding them, because the context names no processor any more.
 */
void
armv7_fpu_forget(
	struct armv7_fpu_context *context)
{
	struct armv7_percpu *cpu;
	bool enabled;

	/* Drops the context as one step against a switch. */
	enabled = armv7_irq_save();

	cpu = armv7_percpu_current();
	if (cpu->fpu_owner == context) {
		cpu->fpu_owner = NULL;
		if (fpu_usable)
			armv7_fpu_write_fpexc(0);
	}

	context->loaded_cpu = ARMV7_FPU_NOWHERE;

	armv7_irq_restore(enabled);
}

/*
 * Keeps of a status and control value only what a context may hold.
 *
 * The unit traps no floating-point exception and runs no short vector,
 * so the trap enables, the vector length and the stride are cleared.
 */
uint32_t
armv7_fpu_sanitize_fpscr(
	uint32_t fpscr)
{
	/* Succeeded: the value without the bits the unit does not keep. */
	return fpscr & FPU_FPSCR_KEPT;
}

/*
 * Reports how many times a unit loaded a context's registers since boot.
 */
unsigned long
armv7_fpu_load_count(
	void)
{
	/* Succeeded: the count, which only grows. */
	return fpu_loads;
}

/*
 * Saves the owner's registers when the unit is on, and turns it off.
 * Called with interrupts off.
 */
static void
save_owner_if_on(
	struct armv7_percpu *cpu)
{
	uint32_t fpexc;

	/* A unit that is off already holds nothing newer than memory. */
	fpexc = armv7_fpu_read_fpexc();
	if ((fpexc & ARMV7_FPEXC_EN) == 0)
		return;

	/* The unit is on only for its owner, the context that ran. */
	if (cpu->fpu_owner != NULL)
		armv7_fpu_save(&cpu->fpu_owner->regs, fpu_upper_bank);

	/* Off, so the next context's first instruction takes the unit. */
	armv7_fpu_write_fpexc(0);
}
