/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
system_path_matches(const struct path *candidate, const struct path *target,
		    unsigned query_flags)
{
	if (candidate == NULL || candidate->p_inode == NULL)
		return 0;
	if ((query_flags & ZEDBSD_SYSTEM_FILE_USAGE_QUERY_MOUNT) != 0)
		return candidate->p_mount == target->p_mount;
	return path_equal(candidate, target);
}

static int
system_file_matches(struct file *candidate, const struct path *target,
		    unsigned query_flags, unsigned *socket_match)
{
	struct socket *socket;

	if (candidate == NULL)
		return 0;
	if (system_path_matches(&candidate->f_path, target, query_flags))
		return 1;
	if ((query_flags & ZEDBSD_SYSTEM_FILE_USAGE_QUERY_MOUNT) == 0 &&
	    (candidate->f_inode == target->p_inode ||
	     candidate->f_vm_inode == target->p_inode))
		return 1;
	socket = socket_from_file(candidate);
	if (socket != NULL && unix_socket_bound_path_matches(socket, target)) {
		*socket_match = 1;
		return 1;
	}
	return 0;
}

static unsigned
system_process_file_usage(struct process *process, const struct path *target,
			  unsigned query_flags)
{
	struct cwdinfo *cwdi = NULL;
	struct filedesc *files = NULL;
	struct vmspace *vmspace;
	struct vm_region *region;
	unsigned flags = 0;
	unsigned long irq;
	int descriptor;

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
	if (cwdi != NULL) {
		irq = spin_lock_irqsave(&cwdi->lock);
		if (system_path_matches(&cwdi->cwd, target, query_flags))
			flags |= ZEDBSD_SYSTEM_FILE_USAGE_CWD;
		if (system_path_matches(&cwdi->root, target, query_flags))
			flags |= ZEDBSD_SYSTEM_FILE_USAGE_ROOT;
		spin_unlock_irqrestore(&cwdi->lock, irq);
		cwdinfo_release(cwdi);
	}
	if (files != NULL) {
		for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
			struct file *candidate =
			    filedesc_get_ref(files, descriptor);
			unsigned socket_match = 0;
			if (system_file_matches(candidate, target, query_flags,
						&socket_match))
				flags |= socket_match
					     ? ZEDBSD_SYSTEM_FILE_USAGE_SOCKET
					     : ZEDBSD_SYSTEM_FILE_USAGE_OPEN;
			if (candidate != NULL)
				(void)file_close(candidate);
		}
		filedesc_destroy(files);
	}
	vmspace = process_vmspace_ref(process);
	if (vmspace != NULL) {
		mutex_lock(&vmspace->lock);
		for (region = vmspace->regions; region != NULL;
		     region = region->next) {
			unsigned socket_match = 0;
			if (!system_file_matches(region->file, target,
						 query_flags, &socket_match))
				continue;
			if ((region->prot & HAL_SPACE_EXEC) != 0)
				flags |= ZEDBSD_SYSTEM_FILE_USAGE_EXECUTABLE;
			else
				flags |= ZEDBSD_SYSTEM_FILE_USAGE_MAPPED;
		}
		mutex_unlock(&vmspace->lock);
		vmspace_put(vmspace);
	}
	return flags;
}

static int
system_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	(void)file;
	switch (request) {
	case ZEDBSD_SYSTEM_GET_MOUNTS: {
		struct zedbsd_mount_query header, *output;
		size_t bytes;
		unsigned count = 0, i;
		int error = copyin(argument, &header, sizeof(header));
		if (error != 0)
			return error;
		if (header.version != ZEDBSD_MOUNT_INFO_VERSION ||
		    header.struct_size != sizeof(header) ||
		    header.capacity > ZEDBSD_MOUNT_INFO_MAX)
			return EINVAL;
		for (i = 0; i < 4; i++)
			if (header.reserved[i] != 0)
				return EINVAL;
		bytes = sizeof(header) + header.capacity * sizeof(output->entries[0]);
		output = kern_malloc(bytes);
		if (output == NULL)
			return ENOMEM;
		memset(output, 0, bytes);
		output->version = header.version;
		output->struct_size = sizeof(header);
		output->capacity = header.capacity;
		error = mount_info_snapshot(output->entries, header.capacity, &count);
		output->count = count;
		if (error == 0 || error == ENOSPC) {
			int copy_error = copyout(output, argument, error == 0 ?
			    sizeof(header) + count * sizeof(output->entries[0]) :
			    sizeof(header));
			if (copy_error != 0)
				error = copy_error;
		}
		kern_free(output);
		return error;
	}
	case ZEDBSD_SYSTEM_GET_INFO: {
		struct system_info info;
		memset(&info, 0, sizeof(info));
		info.boot_bios_id = kern_boot_bios_id();
		info.device_count = kern_boot_device_count();
		info.partition_count = partition_count();
		return copyout(&info, argument, sizeof(info));
	}
	case ZEDBSD_SYSTEM_GET_DEVICE: {
		const struct boot_device *device;
		struct system_device_info output;
		uint32_t index;
		int error = copyin(argument, &output, sizeof(output));
		if (error != 0)
			return error;
		index = output.index;
		device = kern_boot_device_at(index);
		if (device == NULL)
			return ENOENT;
		memset(&output, 0, sizeof(output));
		output.index = index;
		output.device_class = device->device_class;
		output.flags = device->flags;
		output.bios_id = device->bios_id;
		output.display_index = device->display_index;
		output.heads = device->heads;
		output.sectors = device->sectors;
		return copyout(&output, argument, sizeof(output));
	}
	case ZEDBSD_SYSTEM_GET_VMSTAT: {
		struct vm_statistics output;
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
		return copyout(&output, argument, sizeof(output));
	}
	case ZEDBSD_SYSTEM_GET_RESOURCES: {
		struct system_resource_info output;
		kern_resource_snapshot(&output);
		return copyout(&output, argument, sizeof(output));
	}
	case ZEDBSD_SYSTEM_GET_PROCESS: {
		struct process_info output;
		struct process *process;
		struct vmspace *vmspace;
		struct ucred *caller_credential;
		struct ucred *target_credential;
		unsigned long irq;
		int error = copyin(argument, &output, sizeof(output));
		if (error != 0)
			return error;
		process = process_find_next_ref(output.pid);
		if (process == NULL)
			return ENOENT;
		caller_credential = cred_process_ref(curthread->proc);
		target_credential = cred_process_ref(process);
		memset(&output, 0, sizeof(output));
		irq = spin_lock_irqsave(&process->lock);
		output.pid = process->pid;
		output.uid =
		    target_credential != NULL ? target_credential->euid : 0;
		output.gid =
		    target_credential != NULL ? target_credential->egid : 0;
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
		if (caller_credential == NULL || target_credential == NULL ||
		    (caller_credential->euid != 0 &&
		     caller_credential->euid != target_credential->euid)) {
			output.command[0] = '\0';
			output.has_controlling_terminal = 0;
		}
		cred_release(target_credential);
		cred_release(caller_credential);
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
		return copyout(&output, argument, sizeof(output));
	}
	case ZEDBSD_SYSTEM_GET_FILE_USAGE: {
		struct system_file_usage output;
		struct process *process;
		struct ucred *credential;
		struct path target;
		struct ucred *caller_credential;
		unsigned flags;
		int error = copyin(argument, &output, sizeof(output));
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
		path_init(&target);
		error =
		    namei_path_at(curthread->proc->cwdi, output.path, &target);
		if (error != 0)
			return error;
		caller_credential = cred_process_ref(curthread->proc);
		for (;;) {
			process = process_find_next_ref(output.cursor_pid);
			if (process == NULL) {
				cred_release(caller_credential);
				path_release(&target);
				return ENOENT;
			}
			credential = cred_process_ref(process);
			if (caller_credential != NULL && credential != NULL &&
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
		memset(output.reserved, 0, sizeof(output.reserved));
		output.reserved0 = 0;
		output.pid = process->pid;
		output.cursor_pid = process->pid;
		output.usage_flags = flags;
		credential = cred_process_ref(process);
		output.uid = credential != NULL ? credential->euid : 0;
		cred_release(credential);
		process_release(process);
		cred_release(caller_credential);
		path_release(&target);
		return copyout(&output, argument, sizeof(output));
	}
	case ZEDBSD_SYSTEM_SWAP_ADD:
	case ZEDBSD_SYSTEM_SWAP_REMOVE:
	case ZEDBSD_SYSTEM_GET_SWAP_SOURCE: {
		struct ucred *credential = cred_current_ref();
		int superuser = cred_is_superuser(credential);

		cred_release(credential);
		return system_swap_device_ioctl(request, argument, superuser);
	}
	case ZEDBSD_SYSTEM_HALT:
		if (curthread->proc->pid != 1)
			return EPERM;
		system_shutdown_prepare();
		kern_platform_halt();
		return 0;
	case ZEDBSD_SYSTEM_REBOOT:
		if (curthread->proc->pid != 1)
			return EPERM;
		system_shutdown_prepare();
		kern_platform_reboot();
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static const struct cdev_ops system_ops = {.ioctl = system_ioctl};

int
system_device_register(void)
{
	return cdev_register("system", 0x00010002U, &system_ops, NULL);
}
