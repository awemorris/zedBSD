/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "hal/hal.h"
#include "kern/kernel.h"
#include "kern/vfs.h"
#include "kern/init.h"
#include "kern/boot.h"
#include "kern/klog.h"
#include "kern/sched.h"
#include "kern/vm-commit.h"
#include "kern/vm-reclaim.h"

#include <string.h>

#ifndef ZEDBSD_INIT_PATH
#define ZEDBSD_INIT_PATH "/sbin/init"
#endif

static struct boot_handoff handoff_snapshot;
static const struct boot_device *boot_devices;
static unsigned boot_device_count;

uint8_t
kern_boot_bios_id(void)
{
	return handoff_snapshot.boot_bios_id;
}

unsigned
kern_boot_device_count(void)
{
	return boot_device_count;
}

const struct boot_device *
kern_boot_device_at(unsigned index)
{
	if (index >= boot_device_count)
		return NULL;
	return &boot_devices[index];
}

/* Validated boot handoff and top-level kernel initialization. */
void
kernel_main(const struct boot_handoff *h,
	    const struct boot_device *platform_devices,
	    unsigned platform_device_count)
{
	const struct kern_boot_parameters *boot_parameters;
	const char *boot_parameter_line;
	const char *init_path;
	int error;

	/* PC-98 Stage 1 places its handoff below 1 MiB.  User address spaces do
	 * not retain that identity mapping, so persistent kernel services must
	 * refer to a kernel-owned copy after init has started. */
	memcpy(&handoff_snapshot, h, sizeof(handoff_snapshot));
	boot_devices = platform_devices;
	boot_device_count = platform_device_count;
	vm_reclaim_init();
	boot_parameter_line = hal_get_arch_handoff("boot.command-line");
	hal_printf("boot: parameters: %s\n",
	    boot_parameter_line != NULL ? boot_parameter_line : "");
	kern_logf("boot: parameters: %s\n",
	    boot_parameter_line != NULL ? boot_parameter_line : "");
	error = kern_boot_parameters_initialize(boot_parameter_line,
	    boot_parameter_line != NULL ? KERN_BOOT_PARAMETERS_STORAGE_SIZE : 0U);
	if (error != 0) {
		hal_printf("boot: parameter parsing failed (%d); entering idle.\n",
			   error);
		kern_logf("boot: parameter parsing failed (%d); entering idle.\n",
			  error);
	} else {
		boot_parameters = kern_boot_parameters_current();
		if (kern_boot_parameters_unknown_count(boot_parameters) != 0U) {
			int truncated;
			const char *name = kern_boot_parameters_unknown_name(
			    boot_parameters, &truncated);
			unsigned count = kern_boot_parameters_unknown_count(
			    boot_parameters);

			hal_printf("boot: ignored %u unknown parameter%s; "
				   "first=%s%s\n",
			    count, count == 1U ? "" : "s", name,
			    truncated ? "..." : "");
			kern_logf("boot: ignored %u unknown parameter%s; "
				  "first=%s%s\n",
			    count, count == 1U ? "" : "s", name,
			    truncated ? "..." : "");
		}
		hal_printf("boot: VFS initialization\n");
		kern_logf("boot: VFS initialization\n");
		error = kern_vfs_init(h, platform_devices,
		    platform_device_count);
		if (error != 0) {
			hal_printf("VFS initialization failed (%d); "
				   "entering idle.\n",
				   error);
			kern_logf("VFS initialization failed (%d); "
				  "entering idle.\n",
				  error);
		} else {
			error = vm_commit_init();
			if (error != 0) {
				hal_printf("VM commit initialization failed (%d); "
					   "entering idle.\n",
					   error);
				kern_logf("VM commit initialization failed (%d); "
					  "entering idle.\n",
					  error);
			} else {
				int init_error;

				init_path = kern_boot_parameters_init_path(
				    boot_parameters);
#if !defined(HAL_ARCH_I386) && !defined(HAL_ARCH_AMD64)
				if (!kern_boot_parameters_source_present())
					init_path = ZEDBSD_INIT_PATH;
#endif
				hal_printf("boot: starting init %s\n", init_path);
				kern_logf("boot: starting init %s\n\n", init_path);
				init_error = kern_init_start(init_path);
				if (init_error != 0) {
					hal_printf("init %s not started (%d); "
						   "entering idle.\n",
					    init_path, init_error);
					kern_logf("init %s not started (%d); "
						  "entering idle.\n",
						  init_path, init_error);
				}
			}
		}
	}
	sched_idle();
}
