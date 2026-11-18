/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's ACPI tables: found from the RSDP, read through a reader
 * the platform supplies, and loaded into the AML interpreter.
 */

#ifndef KERN_DRIVERS_ACPI_ACPI_TABLES_H
#define KERN_DRIVERS_ACPI_ACPI_TABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Reads physical memory: length bytes at address into buffer.  It
 * returns 0, or an errno value when the memory cannot be read.
 */
typedef int (*drv_acpi_memory_reader_t)(uint64_t address, void *buffer, size_t length, void *argument);

/*
 * One table the root table (XSDT or RSDT) lists.
 *
 * copy is the table's bytes, read on first use and kept for the life of
 * the firmware record.
 */
struct drv_acpi_firmware_table {
	uint64_t address;
	uint32_t length;
	char signature[5];
	char oem_id[7];
	char oem_table_id[9];
	uint8_t *copy;
};

/*
 * What the firmware's tables say: every table the root table lists, and
 * the DSDT, the FACS and a copy of the FADT from the FADT's pointers.
 */
struct drv_acpi_firmware {
	struct drv_acpi_firmware_table *tables;
	unsigned count;
	struct drv_acpi_firmware_table dsdt;
	uint64_t facs_address;
	uint8_t *fadt;
	uint32_t fadt_length;
	uint8_t rsdp_revision;
	drv_acpi_memory_reader_t read;
	void *argument;
};

int
drv_acpi_firmware_discover(
	uint64_t rsdp_address,
	drv_acpi_memory_reader_t read,
	void *argument,
	struct drv_acpi_firmware *firmware);

int
drv_acpi_firmware_load(
	struct drv_acpi_firmware *firmware);

int
drv_acpi_firmware_find(
	struct drv_acpi_firmware *firmware,
	const char *signature,
	const char *oem_id,
	const char *oem_table_id,
	const uint8_t **data,
	size_t *length);

void
drv_acpi_firmware_release(
	struct drv_acpi_firmware *firmware);

#endif
