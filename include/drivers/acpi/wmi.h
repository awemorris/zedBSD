/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI-to-WMI mapping for drivers.
 *
 * A WMI mapping device (_HID PNP0C14) describes its data blocks, method
 * blocks and events in _WDG, one 20-byte entry each: a GUID that names the
 * block, a two-character identifier from which the control methods' names
 * are made (WQxx, WMxx, ...) or, for an event, the Notify value, the number
 * of instances, and flags.  A driver finds a block by its GUID and calls
 * its method (WMxx) or queries an instance (WQxx); the result is the AML
 * object the control method returned, which the caller releases.
 *
 * Events (_WED) are not delivered and nothing of WMI reaches user programs.
 */

#ifndef KERN_DRIVERS_ACPI_WMI_H
#define KERN_DRIVERS_ACPI_WMI_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/acpi/acpi.h>

/*
 * The sizes of a GUID, of one _WDG entry, and of the text form of a GUID
 * ("8A42EA14-4F2A-FD45-6422-0087F7A7E608", without the terminating NUL).
 */
#define DRV_ACPI_WMI_GUID_SIZE		16U
#define DRV_ACPI_WMI_ENTRY_SIZE		20U
#define DRV_ACPI_WMI_GUID_TEXT_LENGTH	36U

/*
 * The flags of a _WDG entry.
 *
 * EXPENSIVE: WCxx enables and disables the collection of a data block
 * (or WExx the delivery of an event).  METHOD: the GUID names a set of
 * methods (WMxx), not a data block.  STRING: the block is one string.
 * EVENT: the GUID names an event, and the identifier holds its Notify value.
 */
#define DRV_ACPI_WMI_FLAG_EXPENSIVE	0x01U
#define DRV_ACPI_WMI_FLAG_METHOD	0x02U
#define DRV_ACPI_WMI_FLAG_STRING	0x04U
#define DRV_ACPI_WMI_FLAG_EVENT		0x08U

/*
 * The largest input buffer drv_acpi_wmi_method() passes to a WMxx method.
 */
#define DRV_ACPI_WMI_INPUT_MAX		0x10000U

/*
 * One entry of a _WDG, decoded.
 *
 * guid keeps the bytes in the order _WDG stores them (the GUID structure's
 * first three fields little-endian).  id is the two-character identifier
 * of a data block or a method block; for an event, id[0] is the Notify
 * value and id[1] is reserved.
 */
struct drv_acpi_wmi_entry {
	uint8_t guid[DRV_ACPI_WMI_GUID_SIZE];
	uint8_t id[2];
	uint8_t instance_count;
	uint8_t flags;
};

/*
 * One block the WMI layer knows: the mapping device that listed it and
 * its _WDG entry.
 *
 * The layer keeps its blocks for the life of the system and never changes
 * one it has published, so a driver may keep a pointer to one.
 */
struct drv_acpi_wmi_block {
	struct drv_acpi_node *device;
	struct drv_acpi_wmi_entry entry;
};

int
drv_acpi_wmi_guid_parse(
	const char *text,
	uint8_t guid[DRV_ACPI_WMI_GUID_SIZE]);

int
drv_acpi_wmi_entries_parse(
	const uint8_t *bytes,
	size_t length,
	struct drv_acpi_wmi_entry *entries,
	unsigned capacity,
	unsigned *count);

int
drv_acpi_wmi_control_name(
	const struct drv_acpi_wmi_entry *entry,
	char prefix0,
	char prefix1,
	char name[5]);

int
drv_acpi_wmi_attach(void);

int
drv_acpi_wmi_attach_path(
	const char *path);

unsigned
drv_acpi_wmi_block_count(void);

const struct drv_acpi_wmi_block *
drv_acpi_wmi_block_at(
	unsigned index);

int
drv_acpi_wmi_find(
	const char *guid_text,
	unsigned nth,
	const struct drv_acpi_wmi_block **block);

int
drv_acpi_wmi_method(
	const struct drv_acpi_wmi_block *block,
	uint32_t instance,
	uint32_t method_id,
	const void *input,
	size_t size,
	struct drv_acpi_object **result);

int
drv_acpi_wmi_query(
	const struct drv_acpi_wmi_block *block,
	uint32_t instance,
	struct drv_acpi_object **result);

#endif
