/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the riscv64 UEFI loader hands to the riscv64 HAL.
 *
 * Only the loader and the HAL read this header.  The HAL turns what it
 * holds into the kernel's common handoff and its named objects.
 */

#ifndef KERN_BOOTLOADER_RISCV64_HANDOFF_H
#define KERN_BOOTLOADER_RISCV64_HANDOFF_H

#include <stddef.h>
#include <stdint.h>

#include "../../include/kern/boot.h"

/*
 * The magic the loader's handoffs start with, "ZBL6" read as a
 * little-endian word.  A flattened device tree starts with a different
 * word, so the kernel's entry tells the loader's handoff from a device tree
 * the firmware passed directly by the first word alone.
 */
#ifndef ZBL6_HANDOFF_MAGIC
#define ZBL6_HANDOFF_MAGIC		0x364c425a
#endif

/*
 * The layout of this handoff.  A later layout keeps every field of the
 * earlier ones in place and appends its own after them, raising the version
 * and the size; a reader takes the fields of the versions it knows and
 * leaves the rest.  Version 1 ends with the boot parameters; version 2
 * adds the screen the firmware left on and the RAM the firmware keeps.
 */
#define ZBL6_RISCV64_HANDOFF_VERSION	2U

/* The most ranges of RAM the firmware keeps that the record holds. */
#define ZBL6_RISCV64_RESERVED_MAX	16U

/*
 * A flag of reserved_flags: the firmware kept more ranges than the record
 * holds, so the ones it holds are not all of them.  A reader ignores the
 * flags it does not know.
 */
#define ZBL6_RISCV64_RESERVED_OVERFLOW	0x1U

/*
 * One range of RAM the firmware still uses after boot services end, which
 * the kernel must not allocate.
 */
struct zbl6_riscv64_reserved_range {
	uint64_t phys;
	uint64_t size;
} __attribute__((packed));

/*
 * The handoff record.
 *
 * The loader fills one in memory it allocated, passes its physical address
 * in a1 with the boot hart's id in a0, and enters the kernel with
 * translation off.  Every address is physical.  The HAL reads the record
 * before it sets memory up and does not keep it; the device tree and the
 * ramdisk it names stay where the loader put them.
 */
struct zbl6_riscv64_handoff {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t flags;

	/* The hart the firmware booted on, which runs the kernel's entry. */
	uint64_t boot_hart;

	/* The loader's copy of the firmware's device tree. */
	uint64_t fdt_phys;
	uint64_t fdt_size;

	/* The initial ramdisk, both zero when there is none. */
	uint64_t initrd_phys;
	uint64_t initrd_size;

	/* Where the kernel image was put. */
	uint64_t kernel_phys_start;
	uint64_t kernel_phys_end;

	/* The boot parameters zedbsd.cfg named. */
	struct kern_boot_parameter_record parameters;

	/*
	 * Version 2: the screen the firmware's graphics output left on, all
	 * zero when there is none or the loader did not take it.  The pitch is
	 * in bytes, the format a KERN_BOOT_FRAMEBUFFER_* code.
	 */
	uint64_t framebuffer_phys;
	uint64_t framebuffer_size;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_pitch;
	uint32_t framebuffer_format;

	/*
	 * Version 2: the ranges of RAM the firmware keeps, the screen's among
	 * them; reserved_flags holds ZBL6_RISCV64_RESERVED_OVERFLOW when there
	 * were more than the array holds.
	 */
	uint32_t reserved_count;
	uint32_t reserved_flags;
	struct zbl6_riscv64_reserved_range reserved[ZBL6_RISCV64_RESERVED_MAX];
} __attribute__((packed));

/* The bytes of each layout: where its last field ends. */
#define ZBL6_RISCV64_HANDOFF_V1_SIZE	(offsetof(struct zbl6_riscv64_handoff, parameters) + \
					 sizeof(struct kern_boot_parameter_record))
#define ZBL6_RISCV64_HANDOFF_V2_SIZE	sizeof(struct zbl6_riscv64_handoff)

/*
 * The first layout's end stays where version 1 put it, the record keeps
 * 8-byte fields aligned when it starts on a page, and it fits the one page
 * the loader allocates for it.
 */
_Static_assert(ZBL6_RISCV64_HANDOFF_V1_SIZE == 3160U, "version 1 ends at 3160 bytes");
_Static_assert((ZBL6_RISCV64_HANDOFF_V2_SIZE % 8U) == 0, "the record is a whole number of 64-bit words");
_Static_assert(ZBL6_RISCV64_HANDOFF_V2_SIZE <= 4096U, "the record fits one page");

#endif
