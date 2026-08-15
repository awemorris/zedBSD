/*
 * /dev/system
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_SYSTEM_H
#define ZEDBSD_UAPI_SYSTEM_H

#include <stdint.h>
#include <sys/ioctl.h>

#define ZEDBSD_SYSTEM_IOC_GROUP 's'
#define ZEDBSD_SYSTEM_SWAP_PAGE_SIZE 4096U

struct zedbsd_system_info {
	uint32_t boot_bios_id;
	uint32_t device_count;
	uint32_t partition_count;
	uint32_t reserved;
};

struct zedbsd_system_device {
	uint32_t index;
	uint32_t device_class;
	uint32_t flags;
	uint32_t bios_id;
	uint32_t display_index;
	uint32_t heads;
	uint32_t sectors;
	uint32_t reserved;
};

struct zedbsd_system_vmstat {
	uint64_t physical_total, physical_reserved, physical_allocated, physical_free;
	uint64_t image, heap_fixed, heap_current, heap_peak;
	uint64_t heap_largest_free, heap_largest_failed;
	uint64_t hal_tasks, hal_task_stack_bytes, hal_spaces, hal_page_tables;
	uint64_t vm_resident, vm_anonymous, vm_file, vm_wired, vm_busy, vm_dirty;
	uint64_t vm_clean, vm_swapped, vm_faults, vm_page_in, vm_page_out;
	uint64_t vm_reclaims, vm_io_errors, swap_total, swap_free, swap_extents;
	uint64_t vm_commit_limit, vm_commit_used, vm_commit_available;
};

#define ZEDBSD_SYSTEM_GET_INFO _IOR(ZEDBSD_SYSTEM_IOC_GROUP, 1, struct zedbsd_system_info)
#define ZEDBSD_SYSTEM_GET_DEVICE _IOWR(ZEDBSD_SYSTEM_IOC_GROUP, 2, struct zedbsd_system_device)
#define ZEDBSD_SYSTEM_GET_VMSTAT _IOR(ZEDBSD_SYSTEM_IOC_GROUP, 3, struct zedbsd_system_vmstat)
#define ZEDBSD_SYSTEM_HALT _IO(ZEDBSD_SYSTEM_IOC_GROUP, 4)
#define ZEDBSD_SYSTEM_REBOOT _IO(ZEDBSD_SYSTEM_IOC_GROUP, 5)

#endif
