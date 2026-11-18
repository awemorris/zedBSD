/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 native IPL handoff implementation.
 */

#include <kern/boot.h>
#include <hal/hal.h>
#include <kern/boot.h>

#include "../../x86/boot-parameters.h"
#include "../bsp.h"
#include "../defs.h"
#include "handoff.h"

#define PC98_BOOT_DEVICE_MAX 4U
#define PC98_IDENTITY_MAP_END 0x08000000U

/*
 * The forms of the handoff a loader may pass.
 *
 * The native loader passes the common record with or without the
 * boot-parameter record after it; a Multiboot loader passes the common
 * record alone.
 */
enum pc98_handoff_form {
	PC98_HANDOFF_INVALID = 0,
	PC98_HANDOFF_LEGACY,
	PC98_HANDOFF_PARAMETERS
};

static struct kern_boot_handoff kernel_handoff;
static struct kern_boot_device kernel_boot_devices[PC98_BOOT_DEVICE_MAX];
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];
static int boot_info_valid;

static enum pc98_handoff_form classify_handoff(uint16_t version, uint16_t size);
static int handoff_name_is(const char *name, const char *expected);

/*
 * Validates and copies the PC-98 native boot handoff.
 */
void
bsp_boot_init(
	const void *raw_boot_info)
{
	const struct kern_boot_handoff *raw;
	const struct kern_boot_device *devices;
	enum x86_boot_parameters_result parameter_result;
	enum pc98_handoff_form form;
	uintptr_t raw_address;
	uint32_t table_bytes;

	/*
	 * Opens memory above 16 MB before anything counts or allocates it.
	 * The board reports that memory either way, so leaving the gate shut
	 * would put pages in the map that answer no access.
	 */
	hal_pc98_enable_high_memory();

	/* Starts with no publishable handoff and binds the raw IPL record. */
	boot_info_valid = 0;
	raw = raw_boot_info;

	/* Validates the fixed header, version, and device-table references. */
	if (raw == NULL || raw->magic != KERN_HANDOFF_MAGIC ||
	    (raw->version != PC98_HANDOFF_VERSION_NATIVE &&
	     raw->version != KERN_HANDOFF_VERSION_MULTIBOOT) ||
	    raw->device_count == 0 ||
	    raw->device_count > PC98_BOOT_DEVICE_MAX ||
	    raw->device_table == 0) {
		return;
	}

	/* Classifies and rejects an unsupported handoff version or extent. */
	form = classify_handoff(raw->version, raw->size);
	if (form == PC98_HANDOFF_INVALID)
		return;

	/* Enforces the native handoff's whole-disk LBA convention. */
	if (raw->version == PC98_HANDOFF_VERSION_NATIVE &&
	    (raw->boot_partition_scheme != KERN_PARTITION_SCHEME_LBA ||
	     raw->boot_partition_index != 0)) {
		return;
	}

	/* Enforces the Multiboot handoff's one-based MBR convention. */
	if (raw->version == KERN_HANDOFF_VERSION_MULTIBOOT &&
	    (raw->boot_partition_scheme != KERN_PARTITION_SCHEME_MBR ||
	     raw->boot_partition_index < 1 ||
	     raw->boot_partition_index > 4)) {
		return;
	}

	/* Bounds the complete raw handoff within the early identity map. */
	raw_address = (uintptr_t)raw;
	if (raw_address >= PC98_IDENTITY_MAP_END ||
	    raw->size > PC98_IDENTITY_MAP_END - raw_address) {
		return;
	}

	/* Bounds the complete boot-device table within the identity map. */
	table_bytes = (uint32_t)raw->device_count * sizeof(*devices);
	if (raw->device_table >= PC98_IDENTITY_MAP_END ||
	    table_bytes > PC98_IDENTITY_MAP_END - raw->device_table) {
		return;
	}
	devices = (const struct kern_boot_device *)(uintptr_t)raw->device_table;

	/* Copies versioned boot parameters or publishes an empty parameter set. */
	if (form == PC98_HANDOFF_PARAMETERS) {
		parameter_result = x86_boot_parameter_record_copy(
			boot_command_line,
			&((const struct pc98_parameter_handoff *)raw)->parameters,
			KERN_BOOT_PARAMETER_RECORD_SIZE);
	} else {
		parameter_result = x86_boot_parameters_copy(
			boot_command_line,
			NULL,
			0);
	}

	/* Rejects malformed boot-parameter storage. */
	if (parameter_result != X86_BOOT_PARAMETERS_OK)
		return;

	/* Copies the validated handoff and rebases its device-table pointer. */
	kernel_handoff = *raw;
	hal_memcpy(kernel_boot_devices, devices, table_bytes);

	/*
	 * Gives the kernel the common record alone, under the common version:
	 * the loader's form stays here, and the partition scheme the loader
	 * reported (a whole-disk LBA for the native loader, an MBR partition
	 * for a Multiboot one) is what tells the kernel how to find its root.
	 */
	kernel_handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	kernel_handoff.device_table =
	    (uint32_t)(uintptr_t)kernel_boot_devices;
	boot_info_valid = 1;
}

/*
 * Returns one PC-98 architecture-specific boot handoff object.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	/* Returns the copied boot command line under its stable name. */
	if (handoff_name_is(name, "boot.command-line"))
		return boot_command_line;

	/* Reports an unknown architecture handoff name. */
	return NULL;
}

/*
 * Returns the validated PC-98 kernel handoff.
 */
const void *
bsp_kernel_handoff(
	const void *raw_boot_info)
{
	UNUSED_PARAMETER(raw_boot_info);

	/* Requires a handoff accepted during early boot parsing. */
	if (!boot_info_valid)
		HAL_FATAL("invalid PC-98 boot handoff");

	/* Returns the stable copied handoff. */
	return &kernel_handoff;
}

/* Classifies a loader's handoff by its version and size. */
static enum pc98_handoff_form
classify_handoff(
	uint16_t version,
	uint16_t size)
{
	/* The native loader's record followed by the boot parameters. */
	if (version == PC98_HANDOFF_VERSION_NATIVE &&
	    size == PC98_PARAMETER_HANDOFF_SIZE)
		return PC98_HANDOFF_PARAMETERS;

	/* The common record alone, from either loader. */
	if ((version == PC98_HANDOFF_VERSION_NATIVE ||
	     version == KERN_HANDOFF_VERSION_MULTIBOOT) &&
	    size == PC98_HANDOFF_COMMON_SIZE)
		return PC98_HANDOFF_LEGACY;

	/* Any other version or extent is not a handoff the HAL knows. */
	return PC98_HANDOFF_INVALID;
}

/* Tests one optional handoff object's stable name. */
static int
handoff_name_is(
	const char *name,
	const char *expected)
{
	/* Rejects a missing requested name. */
	if (name == NULL)
		return 0;

	/* Compares both strings through the first mismatch or terminator. */
	while (*name != '\0' && *name == *expected) {
		name++;
		expected++;
	}

	/* Reports two strings ending at the same byte. */
	if (*name == *expected)
		return 1;

	/* Reports different handoff names. */
	return 0;
}
