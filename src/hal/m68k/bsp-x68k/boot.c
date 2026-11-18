/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000 boot handoff: the loader's record kept by the HAL, the common
 * record handed to the kernel, and the named values the kernel reads.
 */

#include <hal/hal.h>
#include "bsp.h"
#include "mmio.h"

/* Where the SRAM keeps the host adapter's own SCSI ID. */
#define BOOT_SRAM_SCSI_ID	0x00ed0070U

/*
 * The HAL's copy of the loader's handoff.
 *
 * Filled once by x68k_boot_init() before anything reads it, and not changed
 * afterwards; the loader's own record is not referenced again.
 */
static struct x68k_boot_handoff boot_handoff;

/*
 * The handoff the kernel's entry reads: the loader's common record, under
 * its own size.  Filled once with boot_handoff; what is particular to the
 * board goes by name.
 */
static struct kern_boot_handoff kernel_handoff;

/*
 * The named objects of hal_get_arch_handoff(), each a 64-bit word filled
 * once by x68k_boot_init(): the host adapter's SCSI ID
 * ("x68k.scsi.initiator") and the kernel address of the internal SCSI
 * controller's first register ("x68k.spc.registers"); its registers follow
 * every second byte.
 */
static uint64_t scsi_initiator;
static uint64_t spc_registers;

static int handoff_name_is(const char *name, const char *wanted);

/*
 * Keeps a copy of the loader's handoff and builds the kernel's from it.
 */
void
x68k_boot_init(
	const struct x68k_boot_handoff *handoff)
{
	uint8_t sram_id;
	int valid;

	/* Refuses a handoff the HAL cannot use. */
	valid = x68k_boot_handoff_valid(handoff);
	if (!valid)
		HAL_FATAL("invalid X68k boot handoff");

	/* Copies the loader's record. */
	hal_memcpy(&boot_handoff, handoff, sizeof(boot_handoff));

	/* Gives the kernel the common record alone. */
	kernel_handoff = boot_handoff.common;
	kernel_handoff.size = sizeof(kernel_handoff);

	/* Reads the host adapter's SCSI ID, which the SRAM keeps in its low three bits. */
	sram_id = hal_mmio_read8((const volatile void *)X68K_DEVICE_ADDRESS(BOOT_SRAM_SCSI_ID));
	scsi_initiator = sram_id & 7U;

	/* The controller's registers are on the odd byte lane, from its first. */
	spc_registers = (uint64_t)X68K_SPC_ADDRESS(0);
}

/*
 * Returns the HAL's copy of the loader's handoff.
 */
const struct x68k_boot_handoff *
x68k_boot_handoff(
	void)
{
	/* Reports the copy x68k_boot_init() filled. */
	return &boot_handoff;
}

/*
 * Returns the handoff the kernel's entry reads: the common record alone.
 */
const struct kern_boot_handoff *
x68k_kernel_handoff(
	void)
{
	/* Reports the record x68k_boot_init() built. */
	return &kernel_handoff;
}

/*
 * Returns one named architecture handoff object.
 *
 * "x68k.scsi.initiator" is the host adapter's SCSI ID and
 * "x68k.spc.registers" the kernel address of the internal SCSI
 * controller's first register, each a 64-bit word.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	int match;

	/* Exposes the host adapter's SCSI ID. */
	match = handoff_name_is(name, "x68k.scsi.initiator");
	if (match)
		return &scsi_initiator;

	/* Exposes the internal SCSI controller's registers. */
	match = handoff_name_is(name, "x68k.spc.registers");
	if (match)
		return &spc_registers;

	/* Reports an unavailable or unknown architecture handoff. */
	return NULL;
}

/* Tells whether a handoff name is the one wanted. */
static int
handoff_name_is(
	const char *name,
	const char *wanted)
{
	size_t index;

	/* Refuses a missing name. */
	if (name == NULL)
		return 0;

	/* Compares the names character by character. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (name[index] != wanted[index])
			return 0;
	}

	/* Reports whether the name ends where the wanted one does. */
	if (name[index] != '\0')
		return 0;
	return 1;
}
