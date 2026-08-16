/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/system-device.h"
#include "kern/cdev.h"
#include "kern/internal.h"
#include "kern/kmem.h"
#include "kern/platform.h"
#include "kern/partition.h"
#include "kern/swap.h"
#include "kern/swap-fat.h"
#include "kern/uaccess.h"
#include "kern/vm-reclaim.h"
#include "kern/vm-commit.h"
#include "kern/resource.h"

#include <zedbsd/system.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

static int
system_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	(void)file;
	switch (request) {
	case ZEDBSD_SYSTEM_GET_INFO: {
		struct zedbsd_system_info info;
		memset(&info, 0, sizeof(info));
		info.boot_bios_id = ho != NULL ? ho->boot_bios_id : 0;
		info.device_count = device_count;
		info.partition_count = partition_count();
		return copyout(&info, argument, sizeof(info));
	}
	case ZEDBSD_SYSTEM_GET_DEVICE: {
		struct zedbsd_system_device output;
		uint32_t index;
		int error = copyin(argument, &output, sizeof(output));
		if (error != 0)
			return error;
		index = output.index;
		if (index >= device_count)
			return ENOENT;
		memset(&output, 0, sizeof(output));
		output.index = index;
		output.device_class = devs[index].device_class;
		output.flags = devs[index].flags;
		output.bios_id = devs[index].bios_id;
		output.display_index = devs[index].display_index;
		output.heads = devs[index].heads;
		output.sectors = devs[index].sectors;
		return copyout(&output, argument, sizeof(output));
	}
	case ZEDBSD_SYSTEM_GET_VMSTAT: {
		struct zedbsd_system_vmstat output;
		struct hal_memory_stats hs;
		struct kern_memory_stats ks;
		struct vm_reclaim_stats vs;
		struct vm_commit_stats cs;
		struct swap_backend *swap = swap_system_backend();
		uint32_t swap_total = 0, swap_free = 0;
		memset(&output, 0, sizeof(output));
		hal_memory_get_stats(&hs);
		kern_memory_get_stats(&ks);
		vm_reclaim_get_stats(&vs);
		vm_commit_get_stats(&cs);
		if (swap != NULL)
			(void)swap_get_stats(swap, &swap_total, &swap_free);
		output.physical_total = hs.physical_total;
		output.physical_reserved = hs.physical_reserved;
		output.physical_allocated = hs.physical_allocated;
		output.physical_free = hs.physical_free;
		output.image = ks.image_bytes;
		output.heap_fixed = ks.heap_fixed; output.heap_current = ks.heap_current;
		output.heap_peak = ks.heap_peak; output.heap_largest_free = ks.heap_largest_free;
		output.heap_largest_failed = ks.heap_largest_failed;
		output.hal_tasks = hs.task_count; output.hal_task_stack_bytes = hs.task_stack_bytes;
		output.hal_spaces = hs.space_count; output.hal_page_tables = hs.page_table_count;
		output.vm_resident = vs.resident; output.vm_anonymous = vs.anonymous_resident;
		output.vm_file = vs.file_resident; output.vm_wired = vs.wired;
		output.vm_busy = vs.busy; output.vm_dirty = vs.dirty; output.vm_clean = vs.clean;
		output.vm_swapped = vs.swapped; output.vm_faults = vs.faults;
		output.vm_page_in = vs.page_ins; output.vm_page_out = vs.page_outs;
		output.vm_reclaims = vs.reclaims; output.vm_io_errors = vs.io_errors;
		output.swap_total = swap_total;
		output.swap_free = swap_free;
		output.swap_extents = swap_fat_extent_count();
		output.vm_commit_limit = cs.limit_pages * VM_COMMIT_PAGE_SIZE;
		output.vm_commit_used = cs.used_pages * VM_COMMIT_PAGE_SIZE;
		output.vm_commit_available =
			(cs.limit_pages - cs.used_pages) * VM_COMMIT_PAGE_SIZE;
		return copyout(&output, argument, sizeof(output));
	}
	case ZEDBSD_SYSTEM_GET_RESOURCES: {
		struct zedbsd_system_resources output;
		kern_resource_snapshot(&output);
		return copyout(&output, argument, sizeof(output));
	}
	case ZEDBSD_SYSTEM_HALT:
		kern_platform_halt();
		return 0;
	case ZEDBSD_SYSTEM_REBOOT:
		kern_platform_reboot();
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static const struct cdev_ops system_ops = { .ioctl = system_ioctl };

int system_device_register(void)
{
	return cdev_register("system", 0x00010002U, &system_ops, NULL);
}
