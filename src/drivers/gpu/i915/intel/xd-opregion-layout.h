/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layout of the ACPI OpRegion of the integrated display as the xd
 * display code (display/xd-vbt.c) reads it: the header with its
 * signature, version and mailbox list, the place of the VBT in mailbox
 * 4, and the raw VBT address and size of the ASLE mailbox.  The firmware
 * fills the OpRegion; this code only reads it, and only to find the VBT.
 *
 * Offsets count bytes from the start of the OpRegion; values are little
 * endian.
 */

#ifndef DRIVERS_GPU_I915_INTEL_XD_OPREGION_LAYOUT_H
#define DRIVERS_GPU_I915_INTEL_XD_OPREGION_LAYOUT_H

/*
 * The size of the OpRegion, its signature ("IntelGraphicsMem") and the
 * header fields: the size in KiB, the version bytes and the mailboxes
 * present.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_opregion.h
 * (OPREGION_SIZE) and Linux v6.8.12 drivers/gpu/drm/i915/display/intel_opregion.c
 * (OPREGION_SIGNATURE, struct opregion_header).
 */
#define I915_XD_OPREGION_BYTES			0x2000U
#define I915_XD_OPREGION_SIGNATURE		0x00U
#define I915_XD_OPREGION_SIGNATURE_BYTES	16U
#define I915_XD_OPREGION_SIZE_KIB		0x10U
#define I915_XD_OPREGION_REVISION		0x15U
#define I915_XD_OPREGION_MINOR			0x16U
#define I915_XD_OPREGION_MAJOR			0x17U
#define I915_XD_OPREGION_MAILBOXES		0x58U

/*
 * The mailbox bits of the header: the public ACPI methods, the software
 * SCI, the ASLE mailbox, the ASLE extension, and the backlight mailbox.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_opregion.c
 * (MBOX_ACPI, MBOX_SWSCI, MBOX_ASLE, MBOX_ASLE_EXT, MBOX_BACKLIGHT).
 */
#define I915_XD_OPREGION_MBOX_ACPI		0x01U
#define I915_XD_OPREGION_MBOX_SWSCI		0x02U
#define I915_XD_OPREGION_MBOX_ASLE		0x04U
#define I915_XD_OPREGION_MBOX_ASLE_EXT		0x10U
#define I915_XD_OPREGION_MBOX_BACKLIGHT		0x20U

/*
 * Where the mailboxes are: the ASLE mailbox, the VBT (mailbox 4), and
 * the ASLE extension, which ends the room the VBT may use when it is
 * present; and the raw VBT address (64 bits) and size (32 bits) in the
 * ASLE mailbox, from OpRegion version 2.0.  From version 2.1 the address
 * is an offset from the OpRegion's own address.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_opregion.c
 * (OPREGION_ASLE_OFFSET, OPREGION_VBT_OFFSET, OPREGION_ASLE_EXT_OFFSET,
 * struct opregion_asle: rvda, rvds; intel_opregion_setup).
 */
#define I915_XD_OPREGION_ASLE			0x300U
#define I915_XD_OPREGION_VBT			0x400U
#define I915_XD_OPREGION_ASLE_EXT		0x1c00U
#define I915_XD_OPREGION_ASLE_RVDA		0xbaU
#define I915_XD_OPREGION_ASLE_RVDS		0xc2U
#define I915_XD_OPREGION_RVDA_MAJOR		2U
#define I915_XD_OPREGION_RVDA_RELATIVE_MINOR	1U

#endif /* DRIVERS_GPU_I915_INTEL_XD_OPREGION_LAYOUT_H */
