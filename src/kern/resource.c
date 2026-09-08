/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Resource accounting and limits.
 *
 * One unit holds the snapshot of live kernel object counts used to detect
 * leaks and the per-process resource limits enforced against them.
 */

#include "kern/resource.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/namecache.h"
#include "kern/pipe.h"
#include "kern/process.h"
#include "kern/swap.h"
#include "kern/vm-object.h"
#include "kern/vmspace.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"
#include <string.h>
#include "kern/resource-limit.h"
#include "kern/cred.h"
#include "kern/clock.h"
#include "kern/exec.h"
#include "kern/signal.h"
#include <errno.h>

static uint64_t resource_cap(int resource);

/*
 * Records the current live object count of every subsystem.
 */
void
kern_resource_snapshot(
	struct system_resource_info *out)
{
	struct swap_backend *swap;
	uint32_t total;
	uint32_t free;

	total = 0;
	free = 0;

	/* Ignores a missing record. */
	if (out == NULL)
		return;

	/* Counts the process, file, filesystem, and memory objects. */
	memset(out, 0, sizeof(*out));
	process_resource_count(&out->process, &out->thread);
	out->filedesc = filedesc_count();
	out->file = file_count();
	out->pipe = pipe_count();
	out->mount = mount_count();
	out->inode = inode_cache_count();
	out->namecache = namecache_count();
	out->vmspace = vmspace_count();
	out->vm_object = vm_object_count();
	out->vm_page = vm_object_page_count();

	/* Counts the swap slots in use when a swap backend exists. */
	swap = swap_system_backend();
	if (swap != NULL && swap_get_stats(swap, &total, &free) == 0)
		out->swap_slot = total - free;

	/*
	 * bio requests are caller-owned and synchronous; disk inflight is the
	 * meaningful live resource count.
	 */
	out->disk = disk_count();
	out->bio = disk_inflight_count();

	/* Counts the network objects. */
	out->socket = socket_count_current();
	out->packet = packet_buf_in_use();
	out->net_device = net_device_count();
}

/*
 * Tests whether two resource snapshots are identical.
 */
int
kern_resource_equal(
	const struct system_resource_info *a,
	const struct system_resource_info *b)
{
	/* A missing snapshot equals nothing. */
	if (a == NULL || b == NULL)
		return 0;

	/* Compares every count. */
	if (memcmp(a, b, sizeof(*a)) != 0)
		return 0;

	/* Reports identical snapshots. */
	return 1;
}

/*
 * Fills a limit table with the defaults of a fresh process.
 */
void
resource_limits_default(
	struct process_limits *limits)
{
	uint64_t address_cap;

	/* Ignores a missing table. */
	if (limits == NULL)
		return;

	/* Starts every limit at its default. */
	memset(limits, 0, sizeof(*limits));
	address_cap = vmspace_address_cap();
	limits->values[RLIMIT_NOFILE].current = KERN_OPEN_MAX;
	limits->values[RLIMIT_NOFILE].maximum = KERN_OPEN_MAX;
	limits->values[RLIMIT_STACK].current = EXEC_STACK_DEFAULT_SIZE;
	limits->values[RLIMIT_STACK].maximum = EXEC_STACK_HARD_MAX;
	limits->values[RLIMIT_AS].current = address_cap;
	limits->values[RLIMIT_AS].maximum = address_cap;
	limits->values[RLIMIT_CPU].current = RLIM_INFINITY;
	limits->values[RLIMIT_CPU].maximum = RLIM_INFINITY;
	limits->values[RLIMIT_DATA].current = RLIM_INFINITY;
	limits->values[RLIMIT_DATA].maximum = RLIM_INFINITY;
	limits->values[RLIMIT_FSIZE].current = RLIM_INFINITY;
	limits->values[RLIMIT_FSIZE].maximum = RLIM_INFINITY;
}

/*
 * Reads one limit of a process.
 */
int
resource_limit_get(
	struct process *process,
	int resource,
	struct rlimit_record *result)
{
	unsigned long irq;

	/* Rejects a missing process or result, or an unknown resource. */
	if (process == NULL ||
	    result == NULL ||
	    resource < 0 ||
	    resource >= RLIMIT_NLIMITS)
		return EINVAL;

	/* Copies the limit under the process lock. */
	irq = spin_lock_irqsave(&process->lock);

	*result = process->limits.values[resource];

	spin_unlock_irqrestore(&process->lock, irq);

	/* Reports the copied limit. */
	return 0;
}

/*
 * Changes one limit of a process and pushes it to the enforcing subsystem.
 *
 * The change is serialized by the process resource mutex, so the pushed
 * limit and the recorded one never disagree.
 */
int
resource_limit_set(
	struct process *process,
	int resource,
	const struct rlimit_record *requested)
{
	struct rlimit_record old;
	uint64_t cap;
	unsigned long irq;
	int privileged;
	int error;

	error = 0;

	/* Rejects a malformed request or an unknown resource. */
	if (process == NULL ||
	    requested == NULL ||
	    resource < 0 ||
	    resource >= RLIMIT_NLIMITS ||
	    requested->current > requested->maximum)
		return EINVAL;

	/* Rejects a hard limit above the kernel's own cap. */
	cap = resource_cap(resource);
	if (requested->maximum > cap)
		return EINVAL;

	mutex_lock(&process->resource_lock);

	/* Only the superuser may raise the hard limit or exceed it. */
	irq = spin_lock_irqsave(&process->lock);

	old = process->limits.values[resource];
	privileged = process->cred != NULL && cred_is_superuser(process->cred);
	if (!privileged && requested->maximum > old.maximum)
		error = EPERM;
	else if (!privileged && requested->current > old.maximum)
		error = EPERM;

	spin_unlock_irqrestore(&process->lock, irq);

	/* Pushes the new soft limit to the subsystem that enforces it. */
	if (error == 0 && resource == RLIMIT_NOFILE)
		error = filedesc_set_limit(process->fd, (unsigned)requested->current);
	if (error == 0 && resource == RLIMIT_AS && process->vmspace != NULL)
		error = vmspace_set_address_limit(process->vmspace, requested->current);
	if (error == 0 && resource == RLIMIT_STACK && process->vmspace != NULL)
		vmspace_set_stack_limit(process->vmspace, requested->current);
	if (error == 0 && resource == RLIMIT_DATA && process->vmspace != NULL)
		error = vmspace_set_data_limit(process->vmspace, requested->current);

	/* Records the accepted limit and re-arms the CPU limit signal. */
	irq = spin_lock_irqsave(&process->lock);

	if (error == 0) {
		process->limits.values[resource] = *requested;
		if (resource == RLIMIT_CPU)
			process->cpu_limit_signal_second = 0;
	}

	spin_unlock_irqrestore(&process->lock, irq);

	mutex_unlock(&process->resource_lock);

	/* Reports why the change failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the soft limit of one resource, or zero when it cannot be read.
 */
uint64_t
resource_limit_current(
	struct process *process,
	int resource)
{
	struct rlimit_record limit;
	int error;

	/* Reads the limit. */
	error = resource_limit_get(process, resource, &limit);
	if (error != 0)
		return 0;

	/* Reports the soft limit. */
	return limit.current;
}

/*
 * Pushes the address space limits of a process to a new address space.
 */
int
resource_limit_apply_vm(
	struct process *process,
	struct vmspace *vm)
{
	struct rlimit_record address;
	struct rlimit_record data;
	struct rlimit_record stack;
	unsigned long irq;
	int error;

	/* Rejects a missing process or address space. */
	if (process == NULL || vm == NULL)
		return EINVAL;

	/* Snapshots the three limits under the process lock. */
	irq = spin_lock_irqsave(&process->lock);

	address = process->limits.values[RLIMIT_AS];
	data = process->limits.values[RLIMIT_DATA];
	stack = process->limits.values[RLIMIT_STACK];

	spin_unlock_irqrestore(&process->lock, irq);

	/* Applies them in order, stopping at the first failure. */
	error = vmspace_set_address_limit(vm, address.current);
	if (error == 0)
		vmspace_set_stack_limit(vm, stack.current);
	if (error == 0)
		error = vmspace_set_data_limit(vm, data.current);

	/* Reports why the application failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Enforces the CPU time limit after one more tick of a process.
 *
 * Exceeding the hard limit kills the process; exceeding the soft limit
 * sends SIGXCPU once per second of further execution.
 */
void
resource_limit_cpu_tick(
	struct process *process,
	uint64_t total_ticks)
{
	struct rlimit_record limit;
	uint64_t elapsed_seconds;
	unsigned long irq;
	int signo;

	signo = 0;

	/* Never limits the kernel's own process. */
	if (process == NULL || process == &process0)
		return;

	/* Decides under the process lock which signal, if any, is due. */
	elapsed_seconds = total_ticks / KERN_CLOCK_HZ;
	irq = spin_lock_irqsave(&process->lock);

	limit = process->limits.values[RLIMIT_CPU];
	if (limit.maximum != RLIM_INFINITY &&
	    (limit.maximum <= UINT64_MAX / KERN_CLOCK_HZ) &&
	    total_ticks >= limit.maximum * KERN_CLOCK_HZ) {
		signo = SIGKILL;
	} else if (limit.current != RLIM_INFINITY &&
	    (limit.current <= UINT64_MAX / KERN_CLOCK_HZ) &&
	    total_ticks >= limit.current * KERN_CLOCK_HZ &&
	    process->cpu_limit_signal_second <= elapsed_seconds) {
		process->cpu_limit_signal_second = elapsed_seconds + 1U;
		signo = SIGXCPU;
	}

	spin_unlock_irqrestore(&process->lock, irq);

	/* Sends the signal outside the lock. */
	if (signo != 0)
		(void)signal_send_process(process, signo);
}

/* Reports the highest hard limit the kernel supports for a resource. */
static uint64_t
resource_cap(
	int resource)
{
	uint64_t cap;

	/* Selects the cap of the resource. */
	switch (resource) {
	case RLIMIT_NOFILE:
		cap = KERN_OPEN_MAX;
		break;
	case RLIMIT_STACK:
		cap = EXEC_STACK_HARD_MAX;
		break;
	case RLIMIT_AS:
		cap = vmspace_address_cap();
		break;
	case RLIMIT_CORE:
		cap = 0;
		break;
	case RLIMIT_CPU:
	case RLIMIT_DATA:
	case RLIMIT_FSIZE:
		cap = RLIM_INFINITY;
		break;
	default:
		cap = 0;
		break;
	}

	/* Reports the cap. */
	return cap;
}
