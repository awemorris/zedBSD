/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lazy switch of the MIPS floating-point unit.
 *
 * The unit keeps the registers of one context, its owner.  A switch only
 * names the context that runs next and turns Status.CU1 off; the first
 * floating-point instruction of a context that is not the owner then
 * takes a coprocessor unusable exception, and the handler saves the
 * owner's registers to its state, loads the running context's and makes
 * it the owner.  A context is identified by an address its holder
 * chooses (a task); its state is the struct hal_fpregs it names when it
 * is selected.
 *
 * The unit is used only when Config1 reports it and its implementation
 * register reports 64-bit registers: every context then runs with
 * Status.FR set.  Without both the unit is left off, and every
 * floating-point instruction is a coprocessor unusable fault.
 */

#include <hal/hal.h>
#include "asm.h"
#include "cpu.h"
#include "fpu.h"
#include "spin.h"
#include "status.h"

/*
 * Nonzero when the unit is used: Config1 reports it and its registers
 * are 64 bits wide.  Set once by mips_fpu_init().
 */
static int fpu_usable;

/*
 * The context whose registers the unit holds, and the state they are
 * saved to when another context takes the unit; NULL while the unit
 * holds no context's registers.  Changed with interrupts off.
 */
static const void *fpu_owner;
static struct hal_fpregs *fpu_owner_state;

/*
 * The context that runs now, and the state its registers are loaded
 * from when it first uses the unit.  Set by mips_fpu_select() at each
 * switch; NULL before the first.
 */
static const void *fpu_current;
static struct hal_fpregs *fpu_current_state;

static uint32_t unit_on(void);
static void unit_restore(uint32_t status);
static void write_status(uint32_t status);

/*
 * Decides whether the unit is used, and sets the register model.
 */
void
mips_fpu_init(
	void)
{
	uint32_t config1;
	uint32_t status;
	uint32_t implementation;

	/* A processor without the unit leaves it off. */
	config1 = mips_read_config1();
	if ((config1 & MIPS_CONFIG1_FLOAT) == 0) {
		hal_puts(MIPS_HAL_NAME " HAL FPU NONE\n");
		return;
	}

	/* Reads the implementation register with the unit reachable for the moment. */
	status = unit_on();
	__asm__ volatile(".set push\n\t.set hardfloat\n\tcfc1 %0, $0\n\t.set pop" : "=r"(implementation));
	unit_restore(status);

	/* A unit without 64-bit registers cannot run the FR=1 model; it stays off. */
	if ((implementation & MIPS_FIR_FLOAT_64) == 0) {
		hal_printf(MIPS_HAL_NAME " HAL FPU NONE fir=%08x\n", implementation);
		return;
	}

	/* Every context runs with 64-bit registers; FR is set once, here. */
	fpu_usable = 1;
	status = mips_read_status();
	write_status(status | MIPS_STATUS_FLOAT_64);
	hal_printf(MIPS_HAL_NAME " HAL FPU F64 fir=%08x\n", implementation);
}

/*
 * Reports whether the unit is used.
 */
bool
mips_fpu_usable(
	void)
{
	/* The decision of mips_fpu_init(). */
	if (fpu_usable)
		return true;

	/* The unit is left off. */
	return false;
}

/*
 * Names the context that runs next and turns the unit off, so that its
 * first floating-point instruction loads its own registers.
 */
void
mips_fpu_select(
	const void *context,
	struct hal_fpregs *state)
{
	uint32_t status;
	bool enabled;

	/* Names the context and closes the unit, together. */
	enabled = mips_irq_save();

	fpu_current = context;
	fpu_current_state = state;
	status = mips_read_status();
	write_status(status & ~(uint32_t)MIPS_STATUS_COPROCESSOR_1);

	mips_irq_restore(enabled);
}

/*
 * Reports whether the unit holds a context's registers, so that the
 * context may reach it without an exception.
 */
bool
mips_fpu_owned_by(
	const void *context)
{
	/* An unused unit is nobody's. */
	if (!fpu_usable || context == NULL)
		return false;

	/* The owner, and nobody else, reaches the unit directly. */
	if (fpu_owner == context)
		return true;

	/* Another context, or none, owns the unit. */
	return false;
}

/*
 * Gives the unit to the running context after a coprocessor unusable
 * exception for coprocessor 1.
 *
 * Reports false when the unit is not used or no context runs, so that
 * the exception is a fault.  Otherwise the owner's registers are saved,
 * the running context's are loaded and Status.CU1 is left on; the caller
 * sets CU1 in the Status it returns with.  Called with interrupts off.
 */
bool
mips_fpu_take(
	void)
{
	uint32_t status;

	/* Without the unit or a context, the instruction is a fault. */
	if (!fpu_usable || fpu_current == NULL)
		return false;

	/* The unit stays reachable from here on for the running context. */
	status = mips_read_status();
	write_status(status | MIPS_STATUS_COPROCESSOR_1 | MIPS_STATUS_FLOAT_64);

	/* The running context already owns the unit: only CU1 was off. */
	if (fpu_owner == fpu_current)
		return true;

	/* Keeps the previous owner's registers in its state. */
	if (fpu_owner != NULL)
		mips_fpu_save(fpu_owner_state);

	/*
	 * Loads the running context's registers and makes it the owner, so
	 * that the next switch away and back finds them where they are.
	 */
	mips_fpu_restore(fpu_current_state);
	fpu_owner = fpu_current;
	fpu_owner_state = fpu_current_state;

	/* Succeeded: the instruction is retried with the unit on. */
	return true;
}

/*
 * Writes the unit's registers to the state of a context that owns it,
 * so that its state is current; nothing is done for any other context.
 */
void
mips_fpu_sync(
	const void *context)
{
	uint32_t status;
	bool enabled;

	/* Saves the owner's registers with the unit reachable for the moment. */
	enabled = mips_irq_save();

	/* Only the owner of a used unit has registers to save. */
	if (fpu_usable &&
	    context != NULL &&
	    fpu_owner == context) {
		status = unit_on();
		mips_fpu_save(fpu_owner_state);
		unit_restore(status);
	}

	mips_irq_restore(enabled);
}

/*
 * Drops a context's claim on the unit, after its state was replaced or
 * before it goes away: its next floating-point instruction loads the
 * state again, and the unit's registers are not saved over it.
 */
void
mips_fpu_forget(
	const void *context)
{
	uint32_t status;
	bool enabled;

	/* Takes the unit away from the context, and closes it when the context runs. */
	enabled = mips_irq_save();

	/* The owner gives up the unit without a save. */
	if (fpu_owner == context) {
		fpu_owner = NULL;
		fpu_owner_state = NULL;
	}

	/* The running context finds the unit closed at its next instruction. */
	if (fpu_current == context) {
		status = mips_read_status();
		write_status(status & ~(uint32_t)MIPS_STATUS_COPROCESSOR_1);
	}

	mips_irq_restore(enabled);
}

/*
 * Clears the Cause field of the unit's control and status register after
 * a floating-point exception, and reports the register as it was.
 *
 * The running context owns the unit, since its instruction raised the
 * exception.  Called with interrupts off.
 */
uint32_t
mips_fpu_clear_cause(
	void)
{
	uint32_t control;
	uint32_t sanitized;
	uint32_t status;

	/* Reads and rewrites the register with the unit reachable for the moment. */
	status = unit_on();
	__asm__ volatile(".set push\n\t.set hardfloat\n\tcfc1 %0, $31\n\t.set pop" : "=r"(control));
	sanitized = mips_float_control_sanitize(control);
	__asm__ volatile(".set push\n\t.set hardfloat\n\tctc1 %0, $31\n\t.set pop" : : "r"(sanitized) : "memory");
	unit_restore(status);

	/* Succeeded: the register at the exception. */
	return control;
}

/* Turns the unit on for the HAL itself and reports the Status to put back. */
static uint32_t
unit_on(
	void)
{
	uint32_t status;

	/* CU1 on; FR as the unit is used (only set when it is). */
	status = mips_read_status();
	write_status(status | MIPS_STATUS_COPROCESSOR_1);

	/* Succeeded: the Status before. */
	return status;
}

/* Puts back the Status unit_on() reported. */
static void
unit_restore(
	uint32_t status)
{
	/* The Status as it was, CU1 included. */
	write_status(status);
}

/* Writes Status and clears the hazard before the next instruction relies on it. */
static void
write_status(
	uint32_t status)
{
	/* The write, then the hazard barrier. */
	__asm__ volatile("mtc0 %0, $12\n\tehb" : : "r"(status) : "memory");
}
