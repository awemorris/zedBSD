/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/resource-limit.h"

#include "kern/cred.h"
#include "kern/clock.h"
#include "kern/exec.h"
#include "kern/filedesc.h"
#include "kern/process.h"
#include "kern/signal.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <string.h>

void
resource_limits_default(struct process_limits *limits)
{
	uint64_t address_cap;
	if (limits == NULL)
		return;
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

static uint64_t
resource_cap(int resource)
{
	switch (resource) {
	case RLIMIT_NOFILE: return KERN_OPEN_MAX;
	case RLIMIT_STACK: return EXEC_STACK_HARD_MAX;
	case RLIMIT_AS: return vmspace_address_cap();
	case RLIMIT_CORE: return 0;
	case RLIMIT_CPU:
	case RLIMIT_DATA:
	case RLIMIT_FSIZE:
		return RLIM_INFINITY;
	default: return 0;
	}
}

int
resource_limit_get(struct process *process, int resource,
	struct rlimit_record *result)
{
	unsigned long irq;
	if (process == NULL || result == NULL || resource < 0 ||
	    resource >= RLIMIT_NLIMITS)
		return EINVAL;
	irq = spin_lock_irqsave(&process->lock);
	*result = process->limits.values[resource];
	spin_unlock_irqrestore(&process->lock, irq);
	return 0;
}

int
resource_limit_set(struct process *process, int resource,
	const struct rlimit_record *requested)
{
	struct rlimit_record old;
	uint64_t cap;
	unsigned long irq;
	int privileged, error = 0;
	if (process == NULL || requested == NULL || resource < 0 ||
	    resource >= RLIMIT_NLIMITS || requested->current > requested->maximum)
		return EINVAL;
	cap = resource_cap(resource);
	if (requested->maximum > cap)
		return EINVAL;
	mutex_lock(&process->resource_lock);
	irq = spin_lock_irqsave(&process->lock);
	old = process->limits.values[resource];
	privileged = process->cred != NULL &&
	    cred_is_superuser(process->cred);
	if (!privileged && requested->maximum > old.maximum)
		error = EPERM;
	else if (!privileged && requested->current > old.maximum)
		error = EPERM;
	spin_unlock_irqrestore(&process->lock, irq);
	if (error == 0 && resource == RLIMIT_NOFILE)
		error = filedesc_set_limit(process->fd,
		    (unsigned)requested->current);
	if (error == 0 && resource == RLIMIT_AS && process->vmspace != NULL)
		error = vmspace_set_address_limit(process->vmspace,
		    requested->current);
	if (error == 0 && resource == RLIMIT_STACK && process->vmspace != NULL)
		vmspace_set_stack_limit(process->vmspace, requested->current);
	if (error == 0 && resource == RLIMIT_DATA && process->vmspace != NULL)
		error = vmspace_set_data_limit(process->vmspace,
		    requested->current);
	irq = spin_lock_irqsave(&process->lock);
	if (error == 0) {
		process->limits.values[resource] = *requested;
		if (resource == RLIMIT_CPU)
			process->cpu_limit_signal_second = 0;
	}
	spin_unlock_irqrestore(&process->lock, irq);
	mutex_unlock(&process->resource_lock);
	return error;
}

uint64_t
resource_limit_current(struct process *process, int resource)
{
	struct rlimit_record limit;
	return resource_limit_get(process, resource, &limit) == 0 ?
	    limit.current : 0;
}

int
resource_limit_apply_vm(struct process *process, struct vmspace *vm)
{
	struct rlimit_record address, data, stack;
	unsigned long irq;
	int error;
	if (process == NULL || vm == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&process->lock);
	address = process->limits.values[RLIMIT_AS];
	data = process->limits.values[RLIMIT_DATA];
	stack = process->limits.values[RLIMIT_STACK];
	spin_unlock_irqrestore(&process->lock, irq);
	error = vmspace_set_address_limit(vm, address.current);
	if (error == 0)
		vmspace_set_stack_limit(vm, stack.current);
	if (error == 0)
		error = vmspace_set_data_limit(vm, data.current);
	return error;
}

void
resource_limit_cpu_tick(struct process *process, uint64_t total_ticks)
{
	struct rlimit_record limit;
	uint64_t elapsed_seconds;
	unsigned long irq;
	int signo = 0;

	if (process == NULL || process == &process0)
		return;
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
	if (signo != 0)
		(void)signal_send_process(process, signo);
}
