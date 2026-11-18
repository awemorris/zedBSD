/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tells QEMU's raspi4b from a Raspberry Pi 4 by the device tree it hands
 * over.
 *
 * QEMU loads the vendor's tree given with -dtb and, before it passes the
 * tree to the kernel, removes the nodes of the devices it does not emulate:
 * every node compatible with "brcm,bcm2711-pcie", "brcm,bcm2711-rng200",
 * "brcm,bcm2711-thermal" or "brcm,bcm2711-genet-v5" is overwritten with NOP
 * tokens.  It does not mark them disabled: the nodes are gone.
 *
 * A board's firmware hands over the same vendor tree with its own additions:
 * the command line and the board's serial and revision, the memory it
 * found, the dtparam and overlay settings.  Those add properties and nodes
 * or change a status; none removes a node.  Every Pi 4 model's tree (4 B,
 * 400, CM4, CM4S) describes both the PCIe
 * controller and the GENET Ethernet.
 *
 * So a BCM2711 tree in which neither the PCIe controller nor the GENET node
 * exists at all, whatever their status would have been, is QEMU's.  Both
 * are required, so that one node missing from an unusual board tree is not
 * taken for the emulator.
 */

#include "drivers/platform/rpi4/rpi4-board.h"
#include <uapi/errno.h>

static bool node_absent(const struct drv_fdt *fdt, const char *compatible);

/*
 * Reports whether the device tree comes from QEMU's raspi4b.
 *
 * Procedure referenced: QEMU hw/arm/raspi4b.c (raspi4_modify_dtb), the
 * nodes it removes.
 */

bool
drv_rpi4_board_emulated(
	const struct drv_fdt *fdt)
{
	bool absent;

	/* A tree that is not the BCM2711's says nothing about the emulator. */
	absent = node_absent(fdt, "brcm,bcm2711-gpio");
	if (absent)
		return false;

	/* A board's tree always keeps the PCIe controller, even disabled. */
	absent = node_absent(fdt, "brcm,bcm2711-pcie");
	if (!absent)
		return false;

	/* It always keeps the GENET Ethernet too, which QEMU also removes. */
	absent = node_absent(fdt, "brcm,bcm2711-genet-v5");
	if (!absent)
		return false;

	/* Succeeded: QEMU removed both nodes, so the board is emulated. */
	return true;
}

/*
 * Reports whether a complete walk of the tree finds no node of a kind.
 */
static bool
node_absent(
	const struct drv_fdt *fdt,
	const char *compatible)
{
	uint32_t node;
	int error;

	/*
	 * Looks for the first node that lists the name; a match, or a tree
	 * that could not be walked to its end, proves no absence.
	 */
	error = drv_fdt_find_compatible(fdt, compatible, DRV_FDT_NO_NODE, &node);
	if (error != ENOENT)
		return false;

	/* Succeeded: the walk reached the end without a match. */
	return true;
}
