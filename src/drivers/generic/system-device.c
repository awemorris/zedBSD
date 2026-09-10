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
 * process table, and which processes use a file or a mount; it forwards the
 * swap controls to the swap layer, and lets init halt or reboot the machine.
 */

#include "kern/system-device.h"
#include "kern/system-swap-device.h"
#include "kern/cdev.h"
#include "kern/kernel.h"
#include "kern/kmem.h"
#include "kern/platform.h"
#include "kern/partition.h"
#include "kern/swap.h"
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
#include <kern/system-swap-device.h>
#include <kern/swap-control.h>
#include <kern/swap.h>
#include <kern/uaccess.h>

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
static int words_are_zero(const uint32_t *words, size_t count);
static int bounded_string_valid(const char *text, size_t capacity);
static int control_valid(const struct system_swap_control *control);
static int query_valid(const struct system_swap_source_info *query);
static int map_source_state(uint32_t state, uint32_t *mapped);
static int control_ioctl(unsigned long request, uintptr_t argument, int superuser);
static int get_source_ioctl(uintptr_t argument);

/* Operations published by the system control device. */
static const struct cdev_ops system_ops = {
	.ioctl = system_ioctl
};

_Static_assert(sizeof(((struct kern_swap_control_source_info *)0)->uuid) == ZEDBSD_SYSTEM_SWAP_UUID_SIZE, "kernel and UAPI swap UUID sizes differ");
_Static_assert(sizeof(((struct kern_swap_control_source_info *)0)->label) == ZEDBSD_SYSTEM_SWAP_LABEL_SIZE, "kernel and UAPI swap label sizes differ");
_Static_assert(sizeof(((struct kern_swap_control_source_info *)0)->source) == ZEDBSD_SYSTEM_SWAP_SOURCE_MAX, "kernel and UAPI swap source-string sizes differ");

/*
 * Registers the system control device.
 */
int
drv_system_device_register(
	void)
{
	int error;

	/* Reports why the registration failed. */
	error = cdev_register("system", 0x00010002U, &system_ops, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Dispatches one swap ioctl of the system device.
 */
int
drv_system_swap_device_ioctl(
	unsigned long request,
	uintptr_t argument,
	int superuser)
{
	int error;

	/* Routes the request to its handler. */
	switch (request) {
	case ZEDBSD_SYSTEM_SWAP_ADD:
	case ZEDBSD_SYSTEM_SWAP_REMOVE:
		error = control_ioctl(request, argument, superuser);
		break;
	case ZEDBSD_SYSTEM_GET_SWAP_SOURCE:
		error = get_source_ioctl(argument);
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	/* Reports why the handler failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
		error = system_shutdown_prepare();
		if (error != 0)
			return error;
		kern_platform_halt();
		error = 0;
		break;
	case ZEDBSD_SYSTEM_REBOOT:
		/* Only init may reboot the machine. */
		if (curthread->proc->pid != 1)
			return EPERM;
		error = system_shutdown_prepare();
		if (error != 0)
			return error;
		kern_platform_reboot();
		error = 0;
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	/* Reports why the handler failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* Reports why the copy failed. */
	error = copyout(&info, argument, sizeof(info));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* Reports why the copy failed. */
	error = copyout(&output, argument, sizeof(output));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the physical, heap, virtual memory, and swap statistics. */
static int
system_get_vmstat(
	uintptr_t argument)
{
	struct vm_statistics output;
	struct hal_pmem_stats hs;
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
	hal_pmem_get_stats(&hs);
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

	/* Reports why the copy failed. */
	error = copyout(&output, argument, sizeof(output));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* Reports why the copy failed. */
	error = copyout(&output, argument, sizeof(output));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* Reports why the copy failed. */
	error = copyout(&output, argument, sizeof(output));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	error = drv_system_swap_device_ioctl(request, argument, superuser);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

/* Tests whether every word of a reserved area is zero. */
static int
words_are_zero(
	const uint32_t *words,
	size_t count)
{
	/* Rejects the first nonzero word. */
	while (count != 0) {
		if (*words != 0)
			return 0;
		words++;
		count--;
	}

	/* Reports an all-zero area. */
	return 1;
}

/* Tests whether a fixed buffer holds a nonempty terminated string. */
static int
bounded_string_valid(
	const char *text,
	size_t capacity)
{
	/* An empty buffer or an empty string is invalid. */
	if (capacity == 0)
		return 0;
	if (text[0] == '\0')
		return 0;

	/* The terminator must lie inside the buffer. */
	if (memchr(text, '\0', capacity) == NULL)
		return 0;

	/* Reports a valid string. */
	return 1;
}

/* Validates the layout and contents of a swap control request. */
static int
control_valid(
	const struct system_swap_control *control)
{
	/* The versioned layout must match exactly. */
	if (control->version != ZEDBSD_SYSTEM_SWAP_VERSION)
		return 0;
	if (control->struct_size != sizeof(*control))
		return 0;

	/* No flag or reserved field may be set. */
	if (control->flags != 0)
		return 0;
	if (control->reserved0 != 0)
		return 0;
	if (!words_are_zero(control->reserved,
	    sizeof(control->reserved) / sizeof(control->reserved[0])))
		return 0;

	/* The source must be a terminated string. */
	if (!bounded_string_valid(control->source, sizeof(control->source)))
		return 0;

	/* Reports a valid request. */
	return 1;
}

/* Validates the layout and contents of a swap source query. */
static int
query_valid(
	const struct system_swap_source_info *query)
{
	/* The versioned layout must match exactly. */
	if (query->version != ZEDBSD_SYSTEM_SWAP_VERSION)
		return 0;
	if (query->struct_size != sizeof(*query))
		return 0;

	/* No flag or reserved field may be set. */
	if (query->flags != 0)
		return 0;
	if (!words_are_zero(query->reserved,
	    sizeof(query->reserved) / sizeof(query->reserved[0])))
		return 0;

	/* The source identifier must exist. */
	if (query->source_id >= ZEDBSD_SYSTEM_SWAP_SOURCE_COUNT)
		return 0;

	/* Reports a valid query. */
	return 1;
}

/* Maps a kernel swap source state onto the public state vocabulary. */
static int
map_source_state(
	uint32_t state,
	uint32_t *mapped)
{
	/* Rejects a missing result. */
	if (mapped == NULL)
		return EINVAL;

	/* Folds the kernel states into the three public ones. */
	switch (state) {
	case SWAP_SOURCE_STATE_INACTIVE:
	case SWAP_SOURCE_STATE_PREPARED:
		*mapped = ZEDBSD_SYSTEM_SWAP_STATE_INACTIVE;
		return 0;
	case SWAP_SOURCE_STATE_ACTIVE:
		*mapped = ZEDBSD_SYSTEM_SWAP_STATE_ACTIVE;
		return 0;
	case SWAP_SOURCE_STATE_DRAINING:
	case SWAP_SOURCE_STATE_REMOVING:
		*mapped = ZEDBSD_SYSTEM_SWAP_STATE_DRAINING;
		return 0;
	default:
		break;
	}

	/* Reports a kernel state without a public counterpart. */
	return EIO;
}

/* Handles a swap add or remove request. */
static int
control_ioctl(
	unsigned long request,
	uintptr_t argument,
	int superuser)
{
	struct system_swap_control control;
	int error;

	/* Copies the request in and validates it. */
	error = copyin(argument, &control, sizeof(control));
	if (error != 0)
		return error;
	if (!control_valid(&control))
		return EINVAL;

	/* Rejects an unprivileged caller. */
	if (!superuser)
		return EPERM;

	/* Performs the requested change. */
	if (request == ZEDBSD_SYSTEM_SWAP_ADD)
		error = kern_swap_control_add(control.source);
	else
		error = kern_swap_control_remove(control.source);

	/* Reports why the change failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Handles a swap source query. */
static int
get_source_ioctl(
	uintptr_t argument)
{
	struct system_swap_source_info output;
	struct kern_swap_control_source_info snapshot;
	uint32_t source_id;
	int error;

	/* Copies the query in and validates it. */
	error = copyin(argument, &output, sizeof(output));
	if (error != 0)
		return error;
	if (!query_valid(&output))
		return EINVAL;
	source_id = output.source_id;

	/* Snapshots the source. */
	memset(&snapshot, 0, sizeof(snapshot));
	error = kern_swap_control_get(source_id, &snapshot);
	if (error != 0)
		return error;

	/* Rejects a snapshot that contradicts itself. */
	if (snapshot.source_id != source_id ||
	    snapshot.used_pages > snapshot.total_pages)
		return EIO;

	/* Renders the snapshot in the public layout. */
	memset(&output, 0, sizeof(output));
	output.version = ZEDBSD_SYSTEM_SWAP_VERSION;
	output.struct_size = sizeof(output);
	output.source_id = source_id;
	error = map_source_state(snapshot.state, &output.state);
	if (error != 0)
		return error;
	output.header_version = snapshot.header_version;
	output.total_pages = snapshot.total_pages;
	output.used_pages = snapshot.used_pages;
	memcpy(output.uuid, snapshot.uuid, sizeof(output.uuid));
	memcpy(output.label, snapshot.label, sizeof(output.label));
	output.label[sizeof(output.label) - 1U] = '\0';
	memcpy(output.source, snapshot.source, sizeof(output.source));
	output.source[sizeof(output.source) - 1U] = '\0';

	/* Copies the answer out. */

	/* Reports why the copy failed. */
	error = copyout(&output, argument, sizeof(output));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
