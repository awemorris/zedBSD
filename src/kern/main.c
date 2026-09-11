/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel entry after the HAL and platform are up.
 *
 * kernel_main() retains the boot handoff, parses the boot parameters,
 * brings up the VFS and VM commit accounting, and starts init.  Every
 * failure is reported on the console and the log before the boot CPU
 * settles into the idle loop.
 */

#include "hal/hal.h"
#include "kern/kernel.h"
#include "kern/vfs.h"
#include "kern/init.h"
#include "kern/boot.h"
#include "kern/klog.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/vm-commit.h"
#include "kern/vm-reclaim.h"

#include <string.h>

#ifndef ZEDBSD_INIT_PATH
#define ZEDBSD_INIT_PATH "/sbin/init"
#endif

static struct boot_handoff handoff_snapshot;
static const struct boot_device *boot_devices;
static unsigned boot_device_count;

static void boot_worker(void *argument);
static void boot_start(const struct boot_handoff *h, const struct boot_device *platform_devices, unsigned platform_device_count);

/*
 * Reports the BIOS identifier of the device the system booted from.
 */
uint8_t
kern_boot_bios_id(
	void)
{
	/* Reports the retained handoff value. */
	return handoff_snapshot.boot_bios_id;
}

/*
 * Reports the number of boot devices the platform published.
 */
unsigned
kern_boot_device_count(
	void)
{
	/* Reports the retained count. */
	return boot_device_count;
}

/*
 * Reports one published boot device by index.
 */
const struct boot_device *
kern_boot_device_at(
	unsigned index)
{
	/* Rejects an index past the published devices. */
	if (index >= boot_device_count)
		return NULL;

	/* Reports the device. */
	return &boot_devices[index];
}

/*
 * Runs the top-level kernel initialization and then idles forever.
 *
 * The boot handoff has already been validated by the platform binding.
 */
void
kernel_main(
	const struct boot_handoff *h,
	const struct boot_device *platform_devices,
	unsigned platform_device_count)
{
	struct thread *worker;
	int error;

	/*
	 * PC-98 Stage 1 places its handoff below 1 MiB.  User address spaces do
	 * not retain that identity mapping, so persistent kernel services must
	 * refer to a kernel-owned copy after init has started.
	 */
	memcpy(&handoff_snapshot, h, sizeof(handoff_snapshot));
	boot_devices = platform_devices;
	boot_device_count = platform_device_count;

	/* Starts the reclaim machinery before any subsystem can need memory. */
	vm_reclaim_init();

	/*
	 * Mounting may wait for a storage worker that owns a mutex or URB.
	 * The bootstrap thread is CPU0's idle task and cannot serve as that
	 * sleeping waiter. Give initialization its own schedulable lifetime.
	 */
	error = kthread_create(boot_worker, NULL, SCHED_PRIORITY_DEFAULT,
	    &worker);
	if (error != 0) {
		kern_logf("boot: initialization thread failed (%d); entering idle.\n",
		    error);
		kern_logf("boot: initialization thread failed (%d); entering idle.\n",
		    error);
	} else {
		/* No joiner is needed; retirement releases this one-shot task. */
		worker->detached = 1;
		thread_start(worker);
	}
	sched_idle();
}

/* Runs blocking initialization using the retained, kernel-owned handoff. */
static void
boot_worker(
	void *argument)
{
	(void)argument;
	boot_start(&handoff_snapshot, boot_devices, boot_device_count);
}

/* Parses the boot parameters, mounts the root, and starts init. */
static void
boot_start(
	const struct boot_handoff *h,
	const struct boot_device *platform_devices,
	unsigned platform_device_count)
{
	const struct kern_boot_parameters *boot_parameters;
	const char *boot_parameter_line;
	const char *init_path;
	const char *name;
	unsigned count;
	int truncated;
	int error;
	int init_error;

	/* Reports the boot parameter line the loader handed over. */
	boot_parameter_line = hal_get_arch_handoff("boot.command-line");
	kern_logf("boot: parameters: %s\n",
	    boot_parameter_line != NULL ? boot_parameter_line : "");

	/* Parses the parameters; a malformed line leaves the system idle. */
	error = kern_boot_parameters_initialize(boot_parameter_line,
	    boot_parameter_line != NULL ? KERN_BOOT_PARAMETERS_STORAGE_SIZE : 0U);
	if (error != 0) {
		kern_logf("boot: parameter parsing failed (%d); entering idle.\n",
		    error);
		kern_logf("boot: parameter parsing failed (%d); entering idle.\n",
		    error);
		return;
	}

	/* Reports ignored parameters without failing the boot. */
	boot_parameters = kern_boot_parameters_current();
	if (kern_boot_parameters_unknown_count(boot_parameters) != 0U) {
		name = kern_boot_parameters_unknown_name(boot_parameters, &truncated);
		count = kern_boot_parameters_unknown_count(boot_parameters);
		kern_logf("boot: ignored %u unknown parameter%s; first=%s%s\n",
		    count, count == 1U ? "" : "s", name, truncated ? "..." : "");
	}

	/* Mounts the root filesystem from the published boot devices. */
	kern_logf("boot: VFS initialization\n");
	error = kern_vfs_init(h, platform_devices, platform_device_count);
	if (error != 0) {
		kern_logf("VFS initialization failed (%d); entering idle.\n",
		    error);
		kern_logf("VFS initialization failed (%d); entering idle.\n",
		    error);
		return;
	}

	/* Starts commit accounting now that swap can be known. */
	error = vm_commit_init();
	if (error != 0) {
		kern_logf("VM commit initialization failed (%d); entering idle.\n",
		    error);
		kern_logf("VM commit initialization failed (%d); entering idle.\n",
		    error);
		return;
	}

	/* Selects the init program, defaulting on platforms without a loader. */
	init_path = kern_boot_parameters_init_path(boot_parameters);
#if !defined(HAL_ARCH_I386) && !defined(HAL_ARCH_AMD64)
	if (!kern_boot_parameters_source_present())
		init_path = ZEDBSD_INIT_PATH;
#endif

	/* Starts init and reports a failure to do so. */
	kern_logf("boot: starting init %s\n\n", init_path);
	init_error = kern_init_start(init_path);
	if (init_error != 0) {
		kern_logf("init %s not started (%d); entering idle.\n",
		    init_path, init_error);
		kern_logf("init %s not started (%d); entering idle.\n",
		    init_path, init_error);
	}
}
