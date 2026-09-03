/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 native IPL handoff implementation.
 */

#include <boot/pc98-handoff.h>
#include <hal/hal.h>
#include <kern/boot.h>

#include "../../x86/boot-parameters.h"
#include "../bsp.h"
#include "../defs.h"

#define PC98_BOOT_DEVICE_MAX 4U
#define PC98_IDENTITY_MAP_END 0x08000000U

static struct boot_handoff kernel_handoff;
static struct boot_device kernel_boot_devices[PC98_BOOT_DEVICE_MAX];
static char boot_command_line[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE];
static int boot_info_valid;

static int handoff_name_is(const char *name, const char *expected);

/*
 * Validates and copies the PC-98 native boot handoff.
 */
void
bsp_boot_init(
	const void *raw_boot_info)
{
	const struct boot_handoff *raw;
	const struct boot_device *devices;
	enum x86_boot_parameters_result parameter_result;
	enum x86_pc98_handoff_form form;
	uintptr_t raw_address;
	uint32_t table_bytes;

	/* Starts with no publishable handoff and binds the raw IPL record. */
	boot_info_valid = 0;
	raw = raw_boot_info;

	/* Validates the fixed header, version, and device-table references. */
	if (raw == NULL || raw->magic != ZEDBSD_HANDOFF_MAGIC ||
	    (raw->version != ZEDBSD_HANDOFF_VERSION_PC98 &&
	     raw->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT) ||
	    raw->device_count == 0 ||
	    raw->device_count > PC98_BOOT_DEVICE_MAX ||
	    raw->device_table == 0) {
		return;
	}

	/* Classifies and rejects an unsupported handoff version or extent. */
	form = x86_pc98_handoff_classify(raw->version, raw->size);
	if (form == X86_PC98_HANDOFF_INVALID)
		return;

	/* Enforces the native handoff's whole-disk LBA convention. */
	if (raw->version == ZEDBSD_HANDOFF_VERSION_PC98 &&
	    (raw->boot_partition_scheme != ZEDBSD_PARTITION_SCHEME_LBA ||
	     raw->boot_partition_index != 0)) {
		return;
	}

	/* Enforces the Multiboot handoff's one-based MBR convention. */
	if (raw->version == ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
	    (raw->boot_partition_scheme != ZEDBSD_PARTITION_SCHEME_MBR ||
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
	devices = (const struct boot_device *)(uintptr_t)raw->device_table;

	/* Copies versioned boot parameters or publishes an empty parameter set. */
	if (form == X86_PC98_HANDOFF_PARAMETERS) {
		parameter_result = x86_boot_parameter_record_copy(
			boot_command_line,
			&((const struct zedbsd_pc98_parameter_handoff *)raw)->parameters,
			ZEDBSD_BOOT_PARAMETER_RECORD_SIZE);
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

	/* Normalizes a parameter-bearing handoff to the kernel's base record. */
	if (form == X86_PC98_HANDOFF_PARAMETERS)
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
