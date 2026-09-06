/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system control device.
 *
 * The device answers ioctls that describe the boot devices, memory, the
 * process table, and which processes use a file or mount, forwards the
 * swap controls, and lets init halt or reboot the machine.
 */

#include "kern/system-device.h"
#include "kern/system-swap-device.h"
#include "kern/cdev.h"
#include "kern/kernel.h"
#include "kern/kmem.h"
#include "kern/platform.h"
#include "kern/partition.h"
#include "kern/swap.h"
#include "kern/swap-fat.h"
#include "kern/uaccess.h"
#include "kern/vm-reclaim.h"
#include "kern/vm-commit.h"
#include "kern/resource.h"
#include "kern/process.h"
#include "kern/thread.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/namei.h"
#include "kern/net/socket.h"
#include "kern/vmspace.h"
#include "kern/mount.h"

#include <zedbsd/system.h>
#include <zedbsd/mountinfo.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

static int system_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int system_get_info(uintptr_t argument);
static int system_get_mounts(uintptr_t argument);
static int system_get_device(uintptr_t argument);
static int system_get_vmstat(uintptr_t argument);
static int system_get_resources(uintptr_t argument);
static int system_get_process(uintptr_t argument);
static int system_get_file_usage(uintptr_t argument);
static int system_swap_ioctl(unsigned long request, uintptr_t argument);
static unsigned system_process_file_usage(struct process *process, const struct path *target, unsigned query_flags);
static int system_file_matches(struct file *candidate, const struct path *target, unsigned query_flags, unsigned *socket_match);
static int system_path_matches(const struct path *candidate, const struct path *target, unsigned query_flags);

static const struct cdev_ops system_ops = {.ioctl = system_ioctl};

/*
 * Registers the system control device.
 */
int
system_device_register(
	void)
{
	int error;

	error = cdev_register("system", 0x00010002U, &system_ops, NULL);

	/* Reports the registration result. */
	return error;
}

/* Dispatches a system device ioctl to its handler. */
static int
system_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	(void)file;

	/* Routes the request. */
	switch (request) {
	case ZEDBSD_SYSTEM_GET_MOUNTS:
		error = system_get_mounts(argument);
		break;
	case ZEDBSD_SYSTEM_GET_INFO:
		error = system_get_info(argument);
		break;
	case ZEDBSD_SYSTEM_GET_DEVICE:
		error = system_get_device(argument);
		break;
	case ZEDBSD_SYSTEM_GET_VMSTAT:
		error = system_get_vmstat(argument);
		break;
	case ZEDBSD_SYSTEM_GET_RESOURCES:
		error = system_get_resources(argument);
		break;
	case ZEDBSD_SYSTEM_GET_PROCESS:
		error = system_get_process(argument);
		break;
	case ZEDBSD_SYSTEM_GET_FILE_USAGE:
		error = system_get_file_usage(argument);
		break;
	case ZEDBSD_SYSTEM_SWAP_ADD:
	case ZEDBSD_SYSTEM_SWAP_REMOVE:
	case ZEDBSD_SYSTEM_GET_SWAP_SOURCE:
		error = system_swap_ioctl(request, argument);
		break;
	case ZEDBSD_SYSTEM_HALT:
		/* Only init may halt the machine. */
		if (curthread->proc->pid != 1)
			return EPERM;
		system_shutdown_prepare();
		kern_platform_halt();
		error = 0;
		break;
	case ZEDBSD_SYSTEM_REBOOT:
		/* Only init may reboot the machine. */
		if (curthread->proc->pid != 1)
			return EPERM;
		system_shutdown_prepare();
		kern_platform_reboot();
		error = 0;
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	/* Reports the handler result. */
	return error;
}

/* Copies a bounded mount snapshot or its required capacity to the caller. */
static int
system_get_mounts(
	uintptr_t argument)
{
	struct zedbsd_mount_query header;
	struct zedbsd_mount_query *output;
	size_t bytes;
	size_t copy_bytes;
	unsigned count;
	unsigned i;
	int error;
	int copy_error;

	/* Reads and validates the caller's versioned capacity declaration. */
	error = copyin(argument, &header, sizeof(header));
	if (error != 0)
		return error;
	if (header.version != ZEDBSD_MOUNT_INFO_VERSION ||
	    header.struct_size != sizeof(header) ||
	    header.capacity > ZEDBSD_MOUNT_INFO_MAX)
		return EINVAL;

	/* Rejects reserved input before allocating the bounded output. */
	for (i = 0; i < 4; i++) {
		if (header.reserved[i] != 0)
			return EINVAL;
	}
	bytes = sizeof(header) + header.capacity * sizeof(output->entries[0]);
	output = kern_malloc(bytes);
	if (output == NULL)
		return ENOMEM;

	/* Initializes the result and captures one checked mount-table snapshot. */
	memset(output, 0, bytes);
	output->version = header.version;
	output->struct_size = sizeof(header);
	output->capacity = header.capacity;
	count = 0;
	error = mount_info_snapshot(output->entries, header.capacity, &count);
	output->count = count;

	/* A short capacity returns only the header containing the required count. */
	if (error == 0 || error == ENOSPC) {
		copy_bytes = sizeof(header);
		if (error == 0)
			copy_bytes += count * sizeof(output->entries[0]);
		copy_error = copyout(output, argument, copy_bytes);
		if (copy_error != 0)
			error = copy_error;
	}

	/* Releases the private snapshot without replacing its operation error. */
	kern_free(output);
	return error;
}

/* Reports the boot identifiers and the device and partition counts. */
static int
system_get_info(
	uintptr_t argument)
{
	struct system_info info;
	int error;

	/* Fills the information record. */
	memset(&info, 0, sizeof(info));
	info.boot_bios_id = kern_boot_bios_id();
	info.device_count = kern_boot_device_count();
	info.partition_count = partition_count();

	/* Copies it to the caller. */
	error = copyout(&info, argument, sizeof(info));

	/* Reports the copy result. */
	return error;
}

/* Describes one boot device by index. */
static int
system_get_device(
	uintptr_t argument)
{
	const struct boot_device *device;
	struct system_device_info output;
	uint32_t index;
	int error;

	/* Reads the requested index. */
	error = copyin(argument, &output, sizeof(output));
	if (error != 0)
		return error;
	index = output.index;

	/* Finds the device. */
	device = kern_boot_device_at(index);
	if (device == NULL)
		return ENOENT;

	/* Describes it. */
	memset(&output, 0, sizeof(output));
	output.index = index;
	output.device_class = device->device_class;
	output.flags = device->flags;
	output.bios_id = device->bios_id;
	output.display_index = device->display_index;
	output.heads = device->heads;
	output.sectors = device->sectors;

	/* Copies the description to the caller. */
	error = copyout(&output, argument, sizeof(output));

	/* Reports the copy result. */
	return error;
}

/* Reports the physical, heap, virtual memory, and swap statistics. */
static int
system_get_vmstat(
	uintptr_t argument)
{
	struct vm_statistics output;
	struct hal_memory_stats hs;
	struct kern_memory_stats ks;
	struct vm_reclaim_stats vs;
	struct vm_commit_stats cs;
	struct swap_backend *swap;
	uint32_t swap_total;
	uint32_t swap_free;
	int error;

	swap = swap_system_backend();
	swap_total = 0;
	swap_free = 0;

	/* Samples every statistics source. */
	memset(&output, 0, sizeof(output));
	hal_memory_get_stats(&hs);
	kern_memory_get_stats(&ks);
	vm_reclaim_get_stats(&vs);
	vm_commit_get_stats(&cs);
	if (swap != NULL)
		(void)swap_get_stats(swap, &swap_total, &swap_free);

	/* Assembles the report. */
	output.physical_total = hs.physical_total;
	output.physical_reserved = hs.physical_reserved;
	output.physical_allocated = hs.physical_allocated;
	output.physical_free = hs.physical_free;
	output.image = ks.image_bytes;
	output.heap_fixed = ks.heap_fixed;
	output.heap_current = ks.heap_current;
	output.heap_peak = ks.heap_peak;
	output.heap_largest_free = ks.heap_largest_free;
	output.heap_largest_failed = ks.heap_largest_failed;
	output.hal_tasks = hs.task_count;
	output.hal_task_stack_bytes = hs.task_stack_bytes;
	output.hal_spaces = hs.space_count;
	output.hal_page_tables = hs.page_table_count;
	output.vm_resident = vs.resident;
	output.vm_anonymous = vs.anonymous_resident;
	output.vm_file = vs.file_resident;
	output.vm_wired = vs.wired;
	output.vm_busy = vs.busy;
	output.vm_dirty = vs.dirty;
	output.vm_clean = vs.clean;
	output.vm_swapped = vs.swapped;
	output.vm_faults = vs.faults;
	output.vm_page_in = vs.page_ins;
	output.vm_page_out = vs.page_outs;
	output.vm_reclaims = vs.reclaims;
	output.vm_io_errors = vs.io_errors;
	output.swap_total = swap_total;
	output.swap_free = swap_free;
	output.swap_extents = swap_fat_extent_count();
	output.vm_commit_limit = cs.limit_pages * VM_COMMIT_PAGE_SIZE;
	output.vm_commit_used = cs.used_pages * VM_COMMIT_PAGE_SIZE;
	output.vm_commit_available =
	    (cs.limit_pages - cs.used_pages) * VM_COMMIT_PAGE_SIZE;

	/* Copies the report to the caller. */
	error = copyout(&output, argument, sizeof(output));

	/* Reports the copy result. */
	return error;
}

/* Reports the kernel resource accounting snapshot. */
static int
system_get_resources(
	uintptr_t argument)
{
	struct system_resource_info output;
	int error;

	/* Takes the snapshot and copies it to the caller. */
	kern_resource_snapshot(&output);
	error = copyout(&output, argument, sizeof(output));

	/* Reports the copy result. */
	return error;
}

/* Describes the first process at or after a process identifier. */
static int
system_get_process(
	uintptr_t argument)
{
	struct process_info output;
	struct process *process;
	struct vmspace *vmspace;
	struct ucred *caller_credential;
	struct ucred *target_credential;
	unsigned long irq;
	int error;

	/* Reads the cursor. */
	error = copyin(argument, &output, sizeof(output));
	if (error != 0)
		return error;

	/* Finds the process. */
	process = process_find_next_ref(output.pid);
	if (process == NULL)
		return ENOENT;

	/* Samples the process under its lock; the cleared record reads zero. */
	caller_credential = cred_process_ref(curthread->proc);
	target_credential = cred_process_ref(process);
	memset(&output, 0, sizeof(output));
	irq = spin_lock_irqsave(&process->lock);
	output.pid = process->pid;
	if (target_credential != NULL) {
		output.uid = target_credential->euid;
		output.gid = target_credential->egid;
	}
	output.state = process->state;
	output.threads = process->thread_count;
	output.process_group = process->pgrp;
	output.session = process->session;
	output.nice_value = process->nice_value;
	output.has_controlling_terminal =
	    process->controlling_tty != NULL;
	output.cpu_ticks = process->cpu_ticks;
	output.user_ticks = process->user_ticks;
	output.system_ticks = process->system_ticks;
	memcpy(output.command, process->command,
	       sizeof(output.command));
	output.command[sizeof(output.command) - 1U] = '\0';
	spin_unlock_irqrestore(&process->lock, irq);

	/* Hides the command and terminal from a caller with another identity. */
	if (caller_credential == NULL ||
	    target_credential == NULL ||
	    (caller_credential->euid != 0 &&
	     caller_credential->euid != target_credential->euid)) {
		output.command[0] = '\0';
		output.has_controlling_terminal = 0;
	}
	cred_release(target_credential);
	cred_release(caller_credential);

	/* Adds the parent and the mapped virtual size. */
	output.ppid = process_parent_pid(process);
	output.version = ZEDBSD_SYSTEM_PROCESS_INFO_VERSION;
	output.struct_size = sizeof(output);
	vmspace = process_vmspace_ref(process);
	if (vmspace != NULL) {
		mutex_lock(&vmspace->lock);
		output.virtual_bytes = vmspace->mapped_virtual_bytes;
		mutex_unlock(&vmspace->lock);
		vmspace_put(vmspace);
	}
	process_release(process);

	/* Copies the description to the caller. */
	error = copyout(&output, argument, sizeof(output));

	/* Reports the copy result. */
	return error;
}

/* Finds the next process that uses a path or a mount. */
static int
system_get_file_usage(
	uintptr_t argument)
{
	struct system_file_usage output;
	struct process *process;
	struct ucred *credential;
	struct ucred *caller_credential;
	struct path target;
	unsigned flags;
	int error;

	/* Reads and validates the query. */
	error = copyin(argument, &output, sizeof(output));
	if (error != 0)
		return error;
	if (output.version != ZEDBSD_SYSTEM_FILE_USAGE_VERSION ||
	    output.struct_size != sizeof(output) ||
	    (output.query_flags &
	     ~ZEDBSD_SYSTEM_FILE_USAGE_QUERY_MOUNT) != 0)
		return EINVAL;
	output.path[sizeof(output.path) - 1U] = '\0';
	if (output.path[0] == '\0')
		return EINVAL;

	/* Resolves the queried path. */
	path_init(&target);
	error = namei_path_at(curthread->proc->cwdi, output.path, &target);
	if (error != 0)
		return error;

	/* Scans the processes after the cursor for one that uses the path. */
	caller_credential = cred_process_ref(curthread->proc);
	for (;;) {
		process = process_find_next_ref(output.cursor_pid);
		if (process == NULL) {
			cred_release(caller_credential);
			path_release(&target);
			return ENOENT;
		}

		/* Only a process the caller may inspect is examined. */
		credential = cred_process_ref(process);
		if (caller_credential != NULL &&
		    credential != NULL &&
		    (caller_credential->euid == 0 ||
		     caller_credential->euid == credential->euid))
			flags = system_process_file_usage(
			    process, &target, output.query_flags);
		else
			flags = 0;
		cred_release(credential);
		if (flags != 0)
			break;
		output.cursor_pid = process->pid;
		process_release(process);
	}

	/* Describes the user found. */
	memset(output.reserved, 0, sizeof(output.reserved));
	output.reserved0 = 0;
	output.pid = process->pid;
	output.cursor_pid = process->pid;
	output.usage_flags = flags;
	credential = cred_process_ref(process);
	if (credential != NULL)
		output.uid = credential->euid;
	else
		output.uid = 0;
	cred_release(credential);
	process_release(process);
	cred_release(caller_credential);
	path_release(&target);

	/* Copies the description to the caller. */
	error = copyout(&output, argument, sizeof(output));

	/* Reports the copy result. */
	return error;
}

/* Forwards a swap control request with the caller's privilege. */
static int
system_swap_ioctl(
	unsigned long request,
	uintptr_t argument)
{
	struct ucred *credential;
	int superuser;
	int error;

	/* Samples the caller's privilege. */
	credential = cred_current_ref();
	superuser = cred_is_superuser(credential);
	cred_release(credential);

	/* Forwards the request to the swap device. */
	error = system_swap_device_ioctl(request, argument, superuser);

	/* Reports the swap device result. */
	return error;
}

/* Reports how one process uses a path: directories, descriptors, mappings. */
static unsigned
system_process_file_usage(
	struct process *process,
	const struct path *target,
	unsigned query_flags)
{
	struct cwdinfo *cwdi;
	struct filedesc *files;
	struct vmspace *vmspace;
	struct vm_region *region;
	struct file *candidate;
	unsigned flags;
	unsigned long irq;
	int descriptor;
	unsigned socket_match;
	unsigned region_socket_match;

	cwdi = NULL;
	files = NULL;
	flags = 0;

	/* Retains the directory and descriptor tables under the process lock. */
	irq = spin_lock_irqsave(&process->lock);
	if (process->cwdi != NULL) {
		cwdi = process->cwdi;
		cwdinfo_retain(cwdi);
	}
	if (process->fd != NULL) {
		files = process->fd;
		filedesc_ref(files);
	}
	spin_unlock_irqrestore(&process->lock, irq);

	/* Checks the working and root directories. */
	if (cwdi != NULL) {
		irq = spin_lock_irqsave(&cwdi->lock);
		if (system_path_matches(&cwdi->cwd, target, query_flags))
			flags |= ZEDBSD_SYSTEM_FILE_USAGE_CWD;
		if (system_path_matches(&cwdi->root, target, query_flags))
			flags |= ZEDBSD_SYSTEM_FILE_USAGE_ROOT;
		spin_unlock_irqrestore(&cwdi->lock, irq);
		cwdinfo_release(cwdi);
	}

	/* Checks every open descriptor, telling bound sockets apart. */
	if (files != NULL) {
		for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
			candidate = filedesc_get_ref(files, descriptor);
			socket_match = 0;
			if (system_file_matches(candidate, target, query_flags,
						&socket_match)) {
				if (socket_match)
					flags |= ZEDBSD_SYSTEM_FILE_USAGE_SOCKET;
				else
					flags |= ZEDBSD_SYSTEM_FILE_USAGE_OPEN;
			}
			if (candidate != NULL)
				(void)file_close(candidate);
		}
		filedesc_destroy(files);
	}

	/* Checks every mapped file, telling executable mappings apart. */
	vmspace = process_vmspace_ref(process);
	if (vmspace != NULL) {
		mutex_lock(&vmspace->lock);
		for (region = vmspace->regions; region != NULL;
		     region = region->next) {
			region_socket_match = 0;
			if (!system_file_matches(region->file, target,
						 query_flags, &region_socket_match))
				continue;
			if ((region->prot & HAL_SPACE_EXEC) != 0)
				flags |= ZEDBSD_SYSTEM_FILE_USAGE_EXECUTABLE;
			else
				flags |= ZEDBSD_SYSTEM_FILE_USAGE_MAPPED;
		}
		mutex_unlock(&vmspace->lock);
		vmspace_put(vmspace);
	}

	/* Reports the usage flags. */
	return flags;
}

/* Tests whether an open file refers to the target path or mount. */
static int
system_file_matches(
	struct file *candidate,
	const struct path *target,
	unsigned query_flags,
	unsigned *socket_match)
{
	struct socket *socket;

	/* A missing file matches nothing. */
	if (candidate == NULL)
		return 0;

	/* The file's own path may match. */
	if (system_path_matches(&candidate->f_path, target, query_flags))
		return 1;

	/* For a path query the backing or mapped inode may match. */
	if ((query_flags & ZEDBSD_SYSTEM_FILE_USAGE_QUERY_MOUNT) == 0 &&
	    (candidate->f_inode == target->p_inode ||
	     candidate->f_vm_inode == target->p_inode))
		return 1;

	/* A socket bound to the path matches as a socket. */
	socket = socket_from_file(candidate);
	if (socket != NULL && unix_socket_bound_path_matches(socket, target)) {
		*socket_match = 1;
		return 1;
	}

	/* Reports no match. */
	return 0;
}

/* Tests whether a path refers to the target path or mount. */
static int
system_path_matches(
	const struct path *candidate,
	const struct path *target,
	unsigned query_flags)
{
	int equal;

	/* An empty path matches nothing. */
	if (candidate == NULL || candidate->p_inode == NULL)
		return 0;

	/* A mount query matches any path on the target's mount. */
	if ((query_flags & ZEDBSD_SYSTEM_FILE_USAGE_QUERY_MOUNT) != 0) {
		if (candidate->p_mount == target->p_mount)
			return 1;
		return 0;
	}

	/* A path query needs the same path. */
	equal = path_equal(candidate, target);

	/* Reports the path comparison. */
	return equal;
}
