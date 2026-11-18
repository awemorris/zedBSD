/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Read-only access to a flattened device tree.
 *
 * The firmware of a board without ACPI describes its devices in a flattened
 * device tree, and the HAL hands its physical address to the kernel.  These
 * calls let a driver find the node of its device and read the addresses,
 * windows and interrupts the node describes.  Nothing is copied or cached:
 * every call walks the blob, which stays owned by whoever mapped it.
 *
 * A node is named by the offset of its begin-node token from the start of
 * the blob.  The root node's offset is never zero, so zero means "no node".
 */

#ifndef KERN_DRIVERS_GENERIC_FDT_H
#define KERN_DRIVERS_GENERIC_FDT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The offset that names no node. */
#define DRV_FDT_NO_NODE		0U

/*
 * One opened device tree blob.
 *
 * It records where the structure and strings blocks are after the header
 * has been checked, so that later calls only need to bound their reads by
 * these limits.  It borrows the blob; the blob must outlive it.
 */
struct drv_fdt {
	const uint8_t *blob;
	uint32_t size;
	uint32_t structure_offset;
	uint32_t structure_size;
	uint32_t strings_offset;
	uint32_t strings_size;
};

int
drv_fdt_open(
	struct drv_fdt *fdt,
	const void *blob,
	size_t available);

int
drv_fdt_find_compatible(
	const struct drv_fdt *fdt,
	const char *compatible,
	uint32_t after,
	uint32_t *node);

int
drv_fdt_find_phandle(
	const struct drv_fdt *fdt,
	uint32_t phandle,
	uint32_t *node);

int
drv_fdt_parent(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint32_t *parent);

int
drv_fdt_property(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	const uint8_t **value,
	uint32_t *length);

bool
drv_fdt_node_enabled(
	const struct drv_fdt *fdt,
	uint32_t node);

uint32_t
drv_fdt_node_cells(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint32_t fallback);

uint64_t
drv_fdt_cells_value(
	const uint8_t *value,
	uint32_t cell_index,
	uint32_t cell_count);

int
drv_fdt_translate(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint64_t address,
	uint64_t *result);

int
drv_fdt_reg(
	const struct drv_fdt *fdt,
	uint32_t node,
	unsigned index,
	uint64_t *address,
	uint64_t *size);

int
drv_fdt_child(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint32_t after,
	uint32_t *child);

int
drv_fdt_string_index(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	const char *wanted,
	unsigned *index);

int
drv_fdt_dma_coherent(
	const struct drv_fdt *fdt,
	uint32_t node,
	int board_default);

#endif
