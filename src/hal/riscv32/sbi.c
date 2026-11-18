/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The supervisor's calls into the machine-mode firmware (the SBI).
 *
 * The kernel reaches the timer, the other harts, their TLBs and the console
 * of the boot only through the firmware: the machine-mode devices behind
 * them are not the supervisor's to touch.  The base extension is asked once
 * at boot which extensions exist, and the answers choose the calls below.
 */

#include <hal/hal.h>
#include "asm.h"
#include "sbi.h"

/*
 * The extension and function numbers.
 *
 * Values referenced: RISC-V Supervisor Binary Interface specification
 * (riscv-non-isa/riscv-sbi-doc), the base, TIME, IPI, RFENCE, HSM, SRST and
 * DBCN extension IDs and function IDs, and the legacy extension numbers.
 */
#define SBI_EXT_BASE			0x10
#define SBI_EXT_TIME			0x54494d45
#define SBI_EXT_IPI			0x735049
#define SBI_EXT_RFENCE			0x52464e43
#define SBI_EXT_HSM			0x48534d
#define SBI_EXT_SRST			0x53525354
#define SBI_EXT_DBCN			0x4442434e
#define SBI_LEGACY_CONSOLE_PUTCHAR	0x01
#define SBI_LEGACY_SHUTDOWN		0x08

#define SBI_BASE_SPEC_VERSION		0
#define SBI_BASE_IMPL_ID		1
#define SBI_BASE_IMPL_VERSION		2
#define SBI_BASE_PROBE_EXTENSION	3
#define SBI_BASE_MARCHID		5
#define SBI_BASE_MIMPID			6
#define SBI_TIME_SET_TIMER		0
#define SBI_IPI_SEND			0
#define SBI_RFENCE_FENCE_I		0
#define SBI_RFENCE_SFENCE_VMA		1
#define SBI_HSM_HART_START		0
#define SBI_HSM_HART_STATUS		2
#define SBI_SRST_RESET			0
#define SBI_SRST_SHUTDOWN		0
#define SBI_SRST_COLD_REBOOT		1
#define SBI_SRST_WARM_REBOOT		2
#define SBI_DBCN_WRITE_BYTE		2

/*
 * The extensions the firmware has, as found by riscv32_sbi_probe().
 *
 * Set once by the boot hart before any other hart starts and read without
 * a lock afterwards.  Zero means the extension is absent (or not probed
 * yet), which sends the console to the legacy call.
 */
static int sbi_has_time;
static int sbi_has_ipi;
static int sbi_has_rfence;
static int sbi_has_hsm;
static int sbi_has_srst;
static int sbi_has_dbcn;

/*
 * The specification version, implementation and implementation version
 * the firmware reports, kept for the boot log.  Zero until probed.
 */
static long sbi_spec_version;
static long sbi_impl_id;
static long sbi_impl_version;

static struct riscv32_sbi_result sbi_call(long extension, long function, unsigned long argument0, unsigned long argument1, unsigned long argument2);
static int sbi_probe_one(long extension);
static int sbi_error_to_hal(long error);

/*
 * Asks the firmware for its version and its extensions.
 *
 * The timer, the notifications, the remote fences and the hart start are
 * required: the HAL has no other way to do them.
 */
void
riscv32_sbi_probe(
	void)
{
	struct riscv32_sbi_result result;

	/* The specification version; a firmware too old to answer has none of the extensions. */
	result = sbi_call(SBI_EXT_BASE, SBI_BASE_SPEC_VERSION, 0, 0, 0);
	if (result.error != 0)
		HAL_FATAL("riscv32 firmware has no SBI base extension");
	sbi_spec_version = result.value;

	/* The implementation and its version, for the log. */
	result = sbi_call(SBI_EXT_BASE, SBI_BASE_IMPL_ID, 0, 0, 0);
	if (result.error == 0)
		sbi_impl_id = result.value;
	result = sbi_call(SBI_EXT_BASE, SBI_BASE_IMPL_VERSION, 0, 0, 0);
	if (result.error == 0)
		sbi_impl_version = result.value;

	/* Asks for each extension the HAL calls. */
	sbi_has_time = sbi_probe_one(SBI_EXT_TIME);
	sbi_has_ipi = sbi_probe_one(SBI_EXT_IPI);
	sbi_has_rfence = sbi_probe_one(SBI_EXT_RFENCE);
	sbi_has_hsm = sbi_probe_one(SBI_EXT_HSM);
	sbi_has_srst = sbi_probe_one(SBI_EXT_SRST);
	sbi_has_dbcn = sbi_probe_one(SBI_EXT_DBCN);

	/* Refuses a firmware without an extension the HAL cannot do without. */
	if (!sbi_has_time)
		HAL_FATAL("riscv32 firmware has no SBI timer extension");
	if (!sbi_has_ipi)
		HAL_FATAL("riscv32 firmware has no SBI IPI extension");
	if (!sbi_has_rfence)
		HAL_FATAL("riscv32 firmware has no SBI remote fence extension");
	if (!sbi_has_hsm)
		HAL_FATAL("riscv32 firmware has no SBI hart state extension");
}

/*
 * Writes the firmware's version and extensions to the boot log.
 */
void
riscv32_sbi_describe(
	void)
{
	unsigned long major;
	unsigned long minor;

	/* Splits the specification version into its two numbers. */
	riscv32_sbi_version(&major, &minor);

	/* One line names the version, the implementation and the optional extensions. */
	hal_printf("RISCV32 SBI v%lu.%lu impl %lu version %lx srst %u dbcn %u\n",
		   (unsigned long)major,
		   (unsigned long)minor,
		   (unsigned long)sbi_impl_id,
		   (unsigned long)sbi_impl_version,
		   (uint32_t)sbi_has_srst,
		   (uint32_t)sbi_has_dbcn);
}

/*
 * Reports the major and minor numbers of the SBI specification the firmware follows.
 */
void
riscv32_sbi_version(
	unsigned long *major,
	unsigned long *minor)
{
	/*
	 * The version packs the major number above bit 24 and the minor below.
	 *
	 * Values referenced: RISC-V Supervisor Binary Interface specification
	 * (riscv-non-isa/riscv-sbi-doc), sbi_get_spec_version().
	 */
	*major = ((unsigned long)sbi_spec_version >> 24) & 0x7fUL;
	*minor = (unsigned long)sbi_spec_version & 0xffffffUL;
}

/*
 * Writes one byte to the firmware's console.
 *
 * The console extension takes the byte itself; a firmware without it has
 * the legacy call, which every firmware of this era keeps.
 */
void
riscv32_sbi_putchar(
	int character)
{
	/* The console extension, when the firmware has it. */
	if (sbi_has_dbcn) {
		(void)sbi_call(SBI_EXT_DBCN, SBI_DBCN_WRITE_BYTE, (unsigned long)(unsigned char)character, 0, 0);
		return;
	}

	/* The legacy console call. */
	(void)sbi_call(SBI_LEGACY_CONSOLE_PUTCHAR, 0, (unsigned long)(unsigned char)character, 0, 0);
}

/*
 * Arms the calling hart's timer for a counter value, which also clears a
 * timer interrupt already pending.
 */
void
riscv32_sbi_set_timer(
	uint64_t deadline)
{
	/* The firmware writes the machine-mode comparator of this hart; a 32-bit hart passes the value in two halves, low first. */
	(void)sbi_call(SBI_EXT_TIME, SBI_TIME_SET_TIMER, (unsigned long)deadline, (unsigned long)(deadline >> 32), 0);
}

/*
 * Raises the supervisor software interrupt on the harts of a mask.
 */
int
riscv32_sbi_send_ipi(
	unsigned long hart_mask,
	unsigned long hart_mask_base)
{
	struct riscv32_sbi_result result;
	int error;

	/* The firmware sets the pending bit on each named hart. */
	result = sbi_call(SBI_EXT_IPI, SBI_IPI_SEND, hart_mask, hart_mask_base, 0);
	error = sbi_error_to_hal(result.error);

	/* Reports a mask the firmware refused. */
	if (error != HAL_OK)
		return error;

	/* Succeeded: every named hart has the interrupt pending. */
	return HAL_OK;
}

/*
 * Drops every translation the harts of a mask cached, and waits for them.
 */
int
riscv32_sbi_remote_sfence_vma(
	unsigned long hart_mask,
	unsigned long hart_mask_base)
{
	struct riscv32_sbi_result result;
	int error;

	/*
	 * A start of zero and a size of all ones mean the whole address space.
	 *
	 * Values referenced: RISC-V Supervisor Binary Interface specification
	 * (riscv-non-isa/riscv-sbi-doc), sbi_remote_sfence_vma().
	 */
	result = sbi_call(SBI_EXT_RFENCE, SBI_RFENCE_SFENCE_VMA, hart_mask, hart_mask_base, 0);
	error = sbi_error_to_hal(result.error);

	/* Reports a mask the firmware refused. */
	if (error != HAL_OK)
		return error;

	/* Succeeded: the named harts hold no stale translation. */
	return HAL_OK;
}

/*
 * Makes the harts of a mask fetch instructions stored as data.
 */
int
riscv32_sbi_remote_fence_i(
	unsigned long hart_mask,
	unsigned long hart_mask_base)
{
	struct riscv32_sbi_result result;
	int error;

	/* The firmware runs fence.i on each named hart. */
	result = sbi_call(SBI_EXT_RFENCE, SBI_RFENCE_FENCE_I, hart_mask, hart_mask_base, 0);
	error = sbi_error_to_hal(result.error);

	/* Reports a mask the firmware refused. */
	if (error != HAL_OK)
		return error;

	/* Succeeded: the named harts see the new instructions. */
	return HAL_OK;
}

/*
 * Starts a stopped hart in supervisor mode at a physical address.
 *
 * The hart enters with its translation off, its interrupts off, a0 its
 * hart id and a1 the opaque value.
 */
int
riscv32_sbi_hart_start(
	unsigned long hart,
	uintptr_t start_phys,
	uintptr_t opaque)
{
	struct riscv32_sbi_result result;
	int error;

	/* Asks the firmware to release the hart. */
	result = sbi_call(SBI_EXT_HSM, SBI_HSM_HART_START, hart, start_phys, opaque);
	error = sbi_error_to_hal(result.error);

	/* Reports a hart the firmware would not start. */
	if (error != HAL_OK)
		return error;

	/* Succeeded: the hart is on its way to the entry. */
	return HAL_OK;
}

/*
 * Reports a hart's state as the firmware sees it, or a negative error.
 */
long
riscv32_sbi_hart_status(
	unsigned long hart)
{
	struct riscv32_sbi_result result;

	/* Asks the firmware for the hart's state. */
	result = sbi_call(SBI_EXT_HSM, SBI_HSM_HART_STATUS, hart, 0, 0);

	/* Reports a hart the firmware does not know. */
	if (result.error != 0)
		return result.error;

	/* Succeeded: one of the hart states. */
	return result.value;
}

/*
 * Resets the system; a cold reset when asked, else a warm one.
 */
void
riscv32_sbi_reset(
	int cold)
{
	unsigned long type;

	/* Interrupts stay off: nothing should run after the request. */
	(void)riscv32_irq_save();

	/* Chooses the reset the caller asked for. */
	type = SBI_SRST_COLD_REBOOT;
	if (!cold)
		type = SBI_SRST_WARM_REBOOT;

	/* The system reset extension, when the firmware has it. */
	if (sbi_has_srst)
		(void)sbi_call(SBI_EXT_SRST, SBI_SRST_RESET, type, 0, 0);

	/* A firmware that cannot reset leaves the hart halted. */
	riscv32_halt_forever();
}

/*
 * Turns the system off, halting the hart when the firmware cannot.
 */
void
riscv32_sbi_shutdown(
	void)
{
	/* Interrupts stay off: nothing should run after the request. */
	(void)riscv32_irq_save();

	/* The system reset extension, or else the legacy call. */
	if (sbi_has_srst)
		(void)sbi_call(SBI_EXT_SRST, SBI_SRST_RESET, SBI_SRST_SHUTDOWN, 0, 0);
	(void)sbi_call(SBI_LEGACY_SHUTDOWN, 0, 0, 0, 0);

	/* A firmware that cannot power off leaves the hart halted. */
	riscv32_halt_forever();
}

/*
 * Reports the processor's implementation id (mimpid), 0 when unknown.
 */
uint64_t
riscv32_sbi_mimpid(
	void)
{
	struct riscv32_sbi_result result;

	/* The firmware reads the machine-mode register for the supervisor. */
	result = sbi_call(SBI_EXT_BASE, SBI_BASE_MIMPID, 0, 0, 0);

	/* Reports an unknown value. */
	if (result.error != 0)
		return 0;

	/* Succeeded: the register's value. */
	return (uint64_t)result.value;
}

/*
 * Reports the processor's architecture id (marchid), 0 when unknown.
 */
uint64_t
riscv32_sbi_marchid(
	void)
{
	struct riscv32_sbi_result result;

	/* The firmware reads the machine-mode register for the supervisor. */
	result = sbi_call(SBI_EXT_BASE, SBI_BASE_MARCHID, 0, 0, 0);

	/* Reports an unknown value. */
	if (result.error != 0)
		return 0;

	/* Succeeded: the register's value. */
	return (uint64_t)result.value;
}

/*
 * Makes one firmware call.
 *
 * Values referenced: RISC-V Supervisor Binary Interface specification
 * (riscv-non-isa/riscv-sbi-doc), the calling convention: the extension in
 * a7, the function in a6, the arguments from a0, the error back in a0 and
 * the value in a1.  The remote fence's start and size are a2 and a3,
 * given as zero and all ones for the whole address space.
 */
static struct riscv32_sbi_result
sbi_call(
	long extension,
	long function,
	unsigned long argument0,
	unsigned long argument1,
	unsigned long argument2)
{
	register unsigned long a0 __asm__("a0");
	register unsigned long a1 __asm__("a1");
	register unsigned long a2 __asm__("a2");
	register unsigned long a3 __asm__("a3");
	register unsigned long a6 __asm__("a6");
	register unsigned long a7 __asm__("a7");
	struct riscv32_sbi_result result;

	/* Loads the call's registers; a3 is the remote fence's size, the whole space. */
	a0 = argument0;
	a1 = argument1;
	a2 = argument2;
	a3 = ~0UL;
	a6 = (unsigned long)function;
	a7 = (unsigned long)extension;

	/* Traps into the firmware, which keeps every register but a0 and a1. */
	__asm__ volatile("ecall"
			 : "+r"(a0), "+r"(a1)
			 : "r"(a2), "r"(a3), "r"(a6), "r"(a7)
			 : "memory");

	/* Succeeded: what the firmware answered. */
	result.error = (long)a0;
	result.value = (long)a1;
	return result;
}

/*
 * Reports whether the firmware has one extension.
 */
static int
sbi_probe_one(
	long extension)
{
	struct riscv32_sbi_result result;

	/* The probe answers zero for an absent extension. */
	result = sbi_call(SBI_EXT_BASE, SBI_BASE_PROBE_EXTENSION, (unsigned long)extension, 0, 0);

	/* A failed probe means the extension is not there. */
	if (result.error != 0)
		return 0;
	if (result.value == 0)
		return 0;

	/* Succeeded: the extension is present. */
	return 1;
}

/*
 * Translates a firmware error to a HAL error.
 *
 * Values referenced: RISC-V Supervisor Binary Interface specification
 * (riscv-non-isa/riscv-sbi-doc), the standard SBI errors: -2 not supported,
 * -3 invalid parameter, -4 denied, -5 invalid address, -6 already available,
 * -7 already started.
 */
static int
sbi_error_to_hal(
	long error)
{
	/* Chooses the HAL's word for the firmware's answer. */
	switch (error) {
	case 0:
		return HAL_OK;
	case -2:
		return HAL_ERR_UNSUPPORTED;
	case -3:
	case -5:
		return HAL_ERR_INVALID;
	case -6:
	case -7:
		return HAL_ERR_STATE;
	default:
		break;
	}

	/* Any other failure is the firmware's. */
	return HAL_ERR_IO;
}
