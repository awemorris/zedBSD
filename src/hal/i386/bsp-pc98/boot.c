/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include <kern/boot.h>
#include "../bsp.h"

#define PC98_BOOT_DEVICE_MAX 4U
#define PC98_IDENTITY_MAP_END 0x08000000U

static struct boot_handoff kernel_handoff;
static struct boot_device kernel_boot_devices[PC98_BOOT_DEVICE_MAX];
static int boot_info_valid;

void
bsp_boot_init(const void *raw_boot_info)
{
	const struct boot_handoff *raw = raw_boot_info;
	const struct boot_device *devices;
	uint32_t table_bytes;

	boot_info_valid = 0;
	if (raw == NULL || raw->magic != ZEDBSD_HANDOFF_MAGIC ||
	    (raw->version != ZEDBSD_HANDOFF_VERSION_PC98 &&
	     raw->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT) ||
	    raw->size < sizeof(*raw) || raw->device_count == 0 ||
	    raw->device_count > PC98_BOOT_DEVICE_MAX ||
	    raw->device_table == 0)
		return;
	if (raw->version == ZEDBSD_HANDOFF_VERSION_PC98 &&
	    (raw->boot_partition_scheme != ZEDBSD_PARTITION_SCHEME_LBA ||
	     raw->boot_partition_index != 0))
		return;
	if (raw->version == ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
	    (raw->boot_partition_scheme != ZEDBSD_PARTITION_SCHEME_MBR ||
	     raw->boot_partition_index < 1 ||
	     raw->boot_partition_index > 4))
		return;
	table_bytes = (uint32_t)raw->device_count * sizeof(*devices);
	if (raw->device_table >= PC98_IDENTITY_MAP_END ||
	    table_bytes > PC98_IDENTITY_MAP_END - raw->device_table)
		return;

	devices = (const struct boot_device *)(uintptr_t)raw->device_table;
	kernel_handoff = *raw;
	hal_memcpy(kernel_boot_devices, devices, table_bytes);
	kernel_handoff.device_table =
	    (uint32_t)(uintptr_t)kernel_boot_devices;
	boot_info_valid = 1;
}

void *
hal_get_arch_handoff(const char *name)
{
	(void)name;
	return NULL;
}

const void *bsp_kernel_handoff(const void *raw_boot_info)
{
	(void)raw_boot_info;
	if (!boot_info_valid)
		HAL_FATAL("invalid PC-98 boot handoff");
	return &kernel_handoff;
}
