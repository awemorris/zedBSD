/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writeback.
 *
 * One unit holds the dirty page admission that bounds how much may be
 * dirtied, the resolution of a mounted device to its physical cache domain,
 * and the per-device syncer threads that carry the writes out.
 */

#include <kern/writeback.h>
#include <kern/cache-memory.h>
#include <kern/disk.h>
#include <kern/atomic.h>
#include <kern/page.h>
#include <hal/hal.h>
#include <errno.h>
#include <string.h>
#include <kern/loop.h>
#include <kern/io-context.h>
#include <stddef.h>
#include <kern/io-scratch.h>
#include <kern/clock.h>
#include <kern/mount.h>
#include <kern/vm-object.h>
#include <kern/thread.h>
#include <kern/sched.h>

#define WRITEBACK_GLOBAL_MAX (64ULL * 1024U * 1024U)

#define WB_OFF 0U

#define WB_LIVE 1U

#define WB_PAUSED 2U

#define WB_AGE (2U * KERN_CLOCK_HZ)

#define WB_MEMORY (WRITEBACK_TICKET_BYTES + ZEDBSD_PAGE_SIZE)

struct writeback_worker {
	struct writeback_unmount *unmount;
	struct writeback_budget budget;
	struct disk *leaf;
	struct io_scratch memory;
	struct wait_queue wake;
	unsigned state;
	unsigned started;
	unsigned attached;
	unsigned charged;
	unsigned busy;
	unsigned requested;
	uint64_t passes;
	uint64_t errors;
	int last_error;
};

struct writeback_mount_policy {
	struct mount *mount;
	struct writeback_worker *worker;
};

static atomic_uint_t budget_guard;

static struct writeback_budget *budgets[WRITEBACK_DEVICE_MAX];

static uint64_t dirty_bytes;

static uint64_t reserved_bytes;

static uint64_t live_tickets;

static uint64_t refusals;

static atomic_uint_t policy_initialized;

static atomic_uint_t policy_shutdown;

static struct mutex policy_control;

static struct spinlock policy_registry;

static struct writeback_worker workers[WRITEBACK_DEVICE_MAX];

static struct writeback_mount_policy policies[MOUNT_MAX];

static bool budget_lock(void);
static void budget_unlock(bool enabled);
static int budget_registered(struct writeback_budget *budget);
static void budget_current_limits(struct writeback_budget_stats *stats);
extern int drv_loop_backing_disk_ref(struct disk *disk, struct disk **backing) __attribute__((weak));
static int policy_initialize(void);
static struct writeback_mount_policy *policy_find(struct mount *mount);
static int worker_prepare(struct writeback_worker *worker);
static int worker_dispose(struct writeback_worker *worker);
static int policy_enable(struct mount *mount);
static int policy_disable(struct mount *mount);
static int worker_pause(struct writeback_worker *worker);
static unsigned worker_siblings(struct writeback_mount_policy *policy);
static void worker_run(void *argument);
static void worker_restore(struct writeback_worker *worker);
static int worker_sync_mount(struct mount *mount, void *scratch);

/*
 * Derives the global, low-water and per-device dirty limits from a cache
 * target.
 */
void
writeback_budget_limits(
	uint64_t target,
	uint64_t *high,
	uint64_t *low,
	uint64_t *device_high)
{
	uint64_t limit;

	/* Divide before multiplying, and cap before any unbounded arithmetic. */
	if (target >= WRITEBACK_GLOBAL_MAX * 5U / 2U)
		limit = WRITEBACK_GLOBAL_MAX;
	else
		limit = target / 5U * 2U;
	limit &= ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);

	/* Publishes page-aligned limits for the caller. */
	*high = limit;
	*low = (limit / 2U) & ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	*device_high = (limit / WRITEBACK_DEVICE_MAX) &
	    ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
}

/*
 * Registers one budget for a physical device and pins that device.
 *
 * The leaf must be a whole device rather than a partition, and neither the
 * budget nor the device may already be registered.
 */
int
writeback_budget_attach(
	struct writeback_budget *budget,
	struct disk *leaf)
{
	struct writeback_budget_stats limits;
	unsigned index;
	unsigned available;
	int error;
	bool enabled;

	/* Rejects a partition, a missing budget, and an unusable device limit. */
	if (budget == NULL || leaf == NULL || leaf->d_parent != NULL)
		return EINVAL;
	budget_current_limits(&limits);
	if (limits.device_high < WRITEBACK_TICKET_BYTES)
		return ENOMEM;

	/* Pins the device for as long as the registration lasts. */
	disk_ref(leaf);
	enabled = budget_lock();

	/* Looks for a duplicate registration and the first free slot. */
	available = WRITEBACK_DEVICE_MAX;
	error = 0;
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (budgets[index] == budget ||
		    (budgets[index] != NULL &&
		     budgets[index]->disk == leaf)) {
			error = EEXIST;
			break;
		}
		if (budgets[index] == NULL && available == WRITEBACK_DEVICE_MAX)
			available = index;
	}
	if (error == 0 && available == WRITEBACK_DEVICE_MAX)
		error = EAGAIN;

	/* Publishes an empty budget in the free slot. */
	if (error == 0) {
		memset(budget, 0, sizeof(*budget));
		budget->disk = leaf;
		budgets[available] = budget;
	}
	budget_unlock(enabled);

	/* Drops the device reference that a refused registration does not keep. */
	if (error != 0)
		disk_release(leaf);

	/* Reports why the registration failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Removes a registered budget and releases its device.
 *
 * A budget that still holds dirty bytes, reservations or tickets stays
 * registered.
 */
int
writeback_budget_detach(
	struct writeback_budget *budget)
{
	struct disk *disk;
	unsigned index;
	bool enabled;

	/* Refuses an unregistered or still accounted budget. */
	enabled = budget_lock();
	if (!budget_registered(budget)) {
		budget_unlock(enabled);
		return ENOENT;
	}
	if (budget->dirty != 0 ||
	    budget->reserved != 0 ||
	    budget->tickets != 0) {
		budget_unlock(enabled);
		return EBUSY;
	}

	/* Unpublishes the budget and empties it under the guard. */
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (budgets[index] == budget)
			budgets[index] = NULL;
	}
	disk = budget->disk;
	memset(budget, 0, sizeof(*budget));
	budget_unlock(enabled);

	/* Releases the device only after the budget is unreachable. */
	disk_release(disk);

	/* Reports the completed removal. */
	return 0;
}

/*
 * Stops or resumes new reservations against one registered budget.
 */
int
writeback_budget_quiesce(
	struct writeback_budget *budget,
	int enabled)
{
	bool irq;

	/* Accepts only the two defined states. */
	if (enabled != 0 && enabled != 1)
		return EINVAL;

	/* Records the state on a registered budget. */
	irq = budget_lock();
	if (!budget_registered(budget)) {
		budget_unlock(irq);
		return ENOENT;
	}
	budget->quiescing = (unsigned)enabled;
	budget_unlock(irq);

	/* Reports the applied state. */
	return 0;
}

/*
 * Reserves one fixed-size dirty ticket against a budget.
 *
 * The reservation is refused, and counted as a refusal, when it would exceed
 * either the global or the per-device limit.
 */
int
writeback_ticket_reserve(
	struct writeback_budget *budget,
	struct writeback_ticket *ticket)
{
	struct writeback_budget_stats limits;
	bool enabled;
	uint64_t used;
	uint64_t device_used;

	/* Rejects a missing or already reserved ticket. */
	if (ticket == NULL)
		return EINVAL;
	if (ticket->budget != NULL)
		return EBUSY;
	memset(ticket, 0, sizeof(*ticket));

	/* Refuses a budget that is unregistered or draining. */
	budget_current_limits(&limits);
	enabled = budget_lock();
	if (!budget_registered(budget) || budget->quiescing) {
		budget_unlock(enabled);
		return EAGAIN;
	}

	/* Refuses a reservation that either limit cannot hold. */
	used = dirty_bytes + reserved_bytes;
	device_used = budget->dirty + budget->reserved;
	if (used > limits.high ||
	    WRITEBACK_TICKET_BYTES > limits.high - used ||
	    device_used > limits.device_high ||
	    WRITEBACK_TICKET_BYTES > limits.device_high - device_used) {
		budget->refusals++;
		refusals++;
		budget_unlock(enabled);
		return EAGAIN;
	}

	/* Charges the reservation globally, per device, and to the ticket. */
	reserved_bytes += WRITEBACK_TICKET_BYTES;
	live_tickets++;
	budget->reserved += WRITEBACK_TICKET_BYTES;
	budget->tickets++;
	ticket->budget = budget;
	ticket->reserved = WRITEBACK_TICKET_BYTES;
	budget_unlock(enabled);

	/* Reports the granted reservation. */
	return 0;
}

/*
 * Converts part of a reservation into dirty bytes.
 *
 * A commit that exceeds the reservation, or that is not page aligned, is a
 * programming error and traps.
 */
void
writeback_ticket_commit(
	struct writeback_ticket *ticket,
	size_t bytes)
{
	struct writeback_budget *budget;
	bool enabled;

	/* Ignores an empty commit. */
	if (bytes == 0)
		return;

	/* Traps on a commit the ticket cannot cover. */
	enabled = budget_lock();
	if (ticket == NULL || !budget_registered(ticket->budget) ||
	    bytes > ticket->reserved || (bytes & (ZEDBSD_PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid dirty credit commit");

	/* Moves the bytes from reserved to dirty at both levels. */
	budget = ticket->budget;
	ticket->reserved -= bytes;
	budget->reserved -= bytes;
	reserved_bytes -= bytes;
	budget->dirty += bytes;
	dirty_bytes += bytes;
	budget_unlock(enabled);
}

/*
 * Returns the uncommitted remainder of a ticket and empties it.
 */
void
writeback_ticket_release(
	struct writeback_ticket *ticket)
{
	struct writeback_budget *budget;
	bool enabled;

	/* Ignores a ticket that holds no reservation. */
	if (ticket == NULL || ticket->budget == NULL)
		return;

	/* Traps when the accounting cannot account for this ticket. */
	enabled = budget_lock();
	budget = ticket->budget;
	if (!budget_registered(budget) || budget->tickets == 0 ||
	    budget->reserved < ticket->reserved || live_tickets == 0)
		HAL_FATAL("invalid dirty credit release");

	/* Gives the reservation back at both levels and empties the ticket. */
	budget->reserved -= ticket->reserved;
	reserved_bytes -= ticket->reserved;
	budget->tickets--;
	live_tickets--;
	memset(ticket, 0, sizeof(*ticket));
	budget_unlock(enabled);
}

/*
 * Retires dirty bytes that writeback has made clean again.
 */
void
writeback_budget_clean(
	struct writeback_budget *budget,
	size_t bytes)
{
	bool enabled;

	/* Ignores an empty retirement. */
	if (bytes == 0)
		return;

	/* Traps on a retirement larger than the recorded dirty quantity. */
	enabled = budget_lock();
	if (!budget_registered(budget) || bytes > budget->dirty ||
	    (bytes & (ZEDBSD_PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid dirty credit retirement");

	/* Discharges the bytes at both levels. */
	budget->dirty -= bytes;
	dirty_bytes -= bytes;
	budget_unlock(enabled);
}

/*
 * Copies the current limits and global accounting totals.
 */
void
writeback_budget_snapshot(
	struct writeback_budget_stats *stats)
{
	unsigned index;
	bool enabled;

	/* Permits callers to skip an optional snapshot. */
	if (stats == NULL)
		return;

	/* Fills the limits before entering the accounting guard. */
	memset(stats, 0, sizeof(*stats));
	budget_current_limits(stats);

	/* Copies the totals and counts the registered devices. */
	enabled = budget_lock();
	stats->dirty = dirty_bytes;
	stats->reserved = reserved_bytes;
	stats->tickets = live_tickets;
	stats->refusals = refusals;
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (budgets[index] != NULL)
			stats->devices++;
	}
	budget_unlock(enabled);
}

/*
 * Copies one registered device budget without exposing unsynchronized
 * counters.
 *
 * The disk pointer is borrowed; this snapshot never grants a lifetime
 * reference.
 */
int
writeback_budget_read(
	struct writeback_budget *budget,
	struct writeback_budget *snapshot)
{
	bool enabled;

	/* Validates the destination before entering the accounting guard. */
	if (snapshot == NULL || snapshot == budget)
		return EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));

	/* Copies the whole budget of a registered device. */
	enabled = budget_lock();
	if (!budget_registered(budget)) {
		budget_unlock(enabled);
		return ENOENT;
	}
	*snapshot = *budget;
	budget_unlock(enabled);

	/* Reports the completed copy. */
	return 0;
}

/*
 * Resolves partition and file-backed layers to one pinned physical cache
 * domain.
 *
 * All intermediate admissions stay pinned until resolution ends, closing
 * detach and partition-reload races.
 */
int
writeback_domain_acquire(
	struct disk *disk,
	struct disk **leaf)
{
	struct disk *tokens[IO_CONTEXT_DEPTH_MAX];
	struct disk *token;
	struct disk *backing;
	unsigned count;
	unsigned index;
	int error;

	/* Rejects a missing output and admits the initial mounted device. */
	if (leaf == NULL)
		return EINVAL;
	*leaf = NULL;
	count = 0;
	error = disk_cache_acquire(disk, &token);
	if (error != 0)
		return error;

	/* Holds a bounded chain while discovering each explicit backing relation. */
	for (;;) {
		/* Refuses a repeated layer or a chain longer than the bound. */
		for (index = 0; index < count; index++) {
			if (tokens[index] == token)
				break;
		}
		if (index != count || count == IO_CONTEXT_DEPTH_MAX) {
			disk_cache_release(token);
			error = ELOOP;
			break;
		}

		/* Keeps this layer admitted and asks for the one below it. */
		tokens[count] = token;
		count++;
		backing = NULL;
		if (drv_loop_backing_disk_ref != NULL)
			error = drv_loop_backing_disk_ref(token, &backing);
		else
			error = EOPNOTSUPP;

		/* A layer with no backing relation is the physical domain. */
		if (error == EOPNOTSUPP) {
			*leaf = token;
			count--;
			error = 0;
			break;
		}
		if (error != 0)
			break;

		/* Converts the temporary backing reference into lifecycle admission. */
		error = disk_cache_acquire(backing, &token);
		disk_release(backing);
		if (error != 0)
			break;
	}

	/* Retires intermediate admissions, preserving only the successful output. */
	while (count != 0) {
		count--;
		disk_cache_release(tokens[count]);
	}

	/* Reports why the resolution failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Changes one mount policy only after its resources or durability boundary exist.
 * The caller owns a mount reference and handles user authorization separately.
 */
int
writeback_mount_set(
	struct mount *mount,
	int enabled)
{
	int error;

	/* Initializes sleeping control serialization before inspecting policy state. */
	if (mount == NULL || (enabled != 0 && enabled != 1))
		return EINVAL;
	error = policy_initialize();
	if (error != 0)
		return error;
	mutex_lock(&policy_control);
	if (atomic_load_acquire(&policy_shutdown) != 0) {
		mutex_unlock(&policy_control);
		return EBUSY;
	}
	if (enabled)
		error = policy_enable(mount);
	else
		error = policy_disable(mount);
	mutex_unlock(&policy_control);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Closes optional admission and drains every enabled mount before device teardown.
 */
int
writeback_shutdown_begin(
	void)
{
	unsigned index;
	int error;

	/* Serializes the boundary against policy construction and unmount tokens. */
	error = policy_initialize();
	if (error != 0)
		return error;
	mutex_lock(&policy_control);
	if (atomic_load_acquire(&policy_shutdown) != 0) {
		mutex_unlock(&policy_control);
		return EBUSY;
	}
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (workers[index].unmount != NULL) {
			mutex_unlock(&policy_control);
			return EBUSY;
		}
	}
	atomic_store_release(&policy_shutdown, 1);

	/* Joins each worker before lending its reserved payload to the final drain. */
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (workers[index].state != WB_LIVE)
			continue;
		error = worker_pause(&workers[index]);
		if (error != 0) {
			mutex_unlock(&policy_control);
			writeback_shutdown_finish(0);
			return error;
		}
	}
	mutex_unlock(&policy_control);

	/* The shutdown gate retains the policy table without holding locks over I/O. */
	for (index = 0; index < MOUNT_MAX; index++) {
		if (policies[index].mount == NULL)
			continue;
		error = worker_sync_mount(policies[index].mount,
		    policies[index].worker->memory.vaddr);
		if (error != 0) {
			writeback_shutdown_finish(0);
			return error;
		}
	}
	return 0;
}

/*
 * Restores a failed shutdown or retires clean policy owners before hardware stops.
 */
void
writeback_shutdown_finish(
	int committed)
{
	struct mount *mount;
	unsigned index;
	unsigned long irq;
	int error;

	/* Only a successful begin owns the in-progress shutdown boundary. */
	if (atomic_load_acquire(&policy_initialized) != 2)
		return;
	mutex_lock(&policy_control);
	if (atomic_load_acquire(&policy_shutdown) != 1) {
		mutex_unlock(&policy_control);
		return;
	}
	if (!committed) {
		for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
			if (workers[index].state == WB_PAUSED)
				worker_restore(&workers[index]);
		}
		atomic_store_release(&policy_shutdown, 0);
		mutex_unlock(&policy_control);
		return;
	}

	/* Releases retained mount identities after the caller's final mount barriers. */
	for (index = 0; index < MOUNT_MAX; index++) {
		irq = spin_lock_irqsave(&policy_registry);
		mount = policies[index].mount;
		policies[index].mount = NULL;
		policies[index].worker = NULL;
		spin_unlock_irqrestore(&policy_registry, irq);
		if (mount != NULL)
			mount_release(mount);
	}
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		irq = spin_lock_irqsave(&policy_registry);
		workers[index].state = WB_OFF;
		spin_unlock_irqrestore(&policy_registry, irq);
		error = worker_dispose(&workers[index]);
		if (error != 0) {
			irq = spin_lock_irqsave(&policy_registry);
			workers[index].last_error = error;
			workers[index].errors++;
			spin_unlock_irqrestore(&policy_registry, irq);
		}
	}
	atomic_store_release(&policy_shutdown, 2);
	mutex_unlock(&policy_control);
}

/*
 * Pauses a mount policy without committing its removal before filesystem checks.
 */
int
writeback_unmount_begin(
	struct mount *mount,
	struct writeback_unmount *token)
{
	struct writeback_mount_policy *policy;
	struct writeback_worker *worker;
	int error;

	/* Leaves an inactive token for mounts that have never enabled writeback. */
	if (mount == NULL || token == NULL || token->mount != NULL || token->worker != NULL)
		return EINVAL;
	if (atomic_load_acquire(&policy_initialized) != 2)
		return 0;
	mutex_lock(&policy_control);
	if (atomic_load_acquire(&policy_shutdown) != 0) {
		mutex_unlock(&policy_control);
		return EBUSY;
	}
	policy = policy_find(mount);
	if (policy == NULL) {
		mutex_unlock(&policy_control);
		return 0;
	}
	worker = policy->worker;
	if (worker->unmount != NULL) {
		mutex_unlock(&policy_control);
		return EBUSY;
	}

	/* Joins existing owners while retaining the published policy reference. */
	error = worker_pause(worker);
	if (error != 0) {
		mutex_unlock(&policy_control);
		return error;
	}
	error = worker_sync_mount(mount, worker->memory.vaddr);
	if (error != 0) {
		worker_restore(worker);
		mutex_unlock(&policy_control);
		return error;
	}

	/* Releases sleeping serialization before potentially recursive VFS teardown. */
	token->mount = mount;
	token->worker = worker;
	worker->unmount = token;
	mutex_unlock(&policy_control);
	return 0;
}

/*
 * Restores a refused unmount or retires its policy after successful filesystem checks.
 */
void
writeback_unmount_finish(
	struct writeback_unmount *token,
	int committed)
{
	struct writeback_mount_policy *policy;
	struct writeback_worker *worker;
	struct mount *mount;
	unsigned siblings;
	unsigned long irq;
	int error;

	/* Accepts the empty token used by ordinary through mounts. */
	if (token == NULL || token->mount == NULL)
		return;
	mutex_lock(&policy_control);
	worker = token->worker;
	mount = token->mount;
	policy = policy_find(mount);
	if (worker == NULL || worker->unmount != token || policy == NULL ||
	    policy->worker != worker)
		HAL_FATAL("writeback unmount lost policy");
	worker->unmount = NULL;
	memset(token, 0, sizeof(*token));
	if (!committed) {
		worker_restore(worker);
		mutex_unlock(&policy_control);
		return;
	}

	/* Removes the retained mount before it can be destroyed by the namespace. */
	siblings = worker_siblings(policy);
	irq = spin_lock_irqsave(&policy_registry);
	policy->mount = NULL;
	policy->worker = NULL;
	if (siblings == 0)
		worker->state = WB_OFF;
	spin_unlock_irqrestore(&policy_registry, irq);
	mount_release(mount);
	if (siblings != 0) {
		worker_restore(worker);
	} else {
		/* Keeps failed clean resource releases charged and reusable without a mount. */
		error = worker_dispose(worker);
		if (error != 0) {
			irq = spin_lock_irqsave(&policy_registry);
			worker->last_error = error;
			worker->errors++;
			spin_unlock_irqrestore(&policy_registry, irq);
		}
	}
	mutex_unlock(&policy_control);
}

/*
 * Reports whether a filesystem owner may retain bounded checkpoint work.
 */
int
writeback_mount_active(struct mount *mount)
{
	struct writeback_mount_policy *policy;
	unsigned long irq;
	int active;

	if (mount == NULL || atomic_load_acquire(&policy_initialized) != 2)
		return 0;
	irq = spin_lock_irqsave(&policy_registry);
	policy = policy_find(mount);
	active = atomic_load_acquire(&policy_shutdown) == 0 &&
	    policy != NULL && policy->worker->state == WB_LIVE;
	spin_unlock_irqrestore(&policy_registry, irq);
	return active;
}

/* Reserves optional dirty ownership before the caller takes a content lease. */
int
writeback_mount_admit(
	struct mount *mount,
	struct writeback_ticket *ticket)
{
	struct writeback_mount_policy *policy;
	unsigned long irq;
	int error;

	/* Rejects incomplete caller-owned admission tokens. */
	if (mount == NULL || ticket == NULL)
		return EINVAL;

	/* Leaves through operation available while policy construction is incomplete. */
	if (atomic_load_acquire(&policy_initialized) != 2)
		return EAGAIN;
	irq = spin_lock_irqsave(&policy_registry);
	policy = policy_find(mount);
	if (atomic_load_acquire(&policy_shutdown) != 0 ||
	    policy == NULL || policy->worker->state != WB_LIVE) {
		spin_unlock_irqrestore(&policy_registry, irq);
		return EAGAIN;
	}
	error = writeback_ticket_reserve(&policy->worker->budget, ticket);
	if (error != 0) {
		policy->worker->requested = 1;
		waitq_wake_all(&policy->worker->wake);
	}
	spin_unlock_irqrestore(&policy_registry, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Requests pressure work while a live ticket still pins its budget owner.
 */
void
writeback_pressure(
	struct writeback_budget *budget)
{
	struct writeback_budget snapshot;
	struct writeback_budget_stats totals;
	unsigned index;
	unsigned long irq;

	/* Avoids turning every small write into an immediate synchronous drain. */
	if (atomic_load_acquire(&policy_initialized) != 2 || budget == NULL)
		return;
	writeback_budget_snapshot(&totals);
	if (writeback_budget_read(budget, &snapshot) != 0)
		return;
	if (snapshot.dirty < totals.device_high / 2U && totals.dirty < totals.low)
		return;
	irq = spin_lock_irqsave(&policy_registry);
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (&workers[index].budget != budget || workers[index].state != WB_LIVE)
			continue;
		workers[index].requested = 1;
		waitq_wake_all(&workers[index].wake);
		break;
	}
	spin_unlock_irqrestore(&policy_registry, irq);
}

/*
 * Copies pointer-free observations without acquiring the sleeping control mutex.
 */
void
writeback_policy_snapshot(
	struct writeback_policy_stats *stats)
{
	unsigned index;
	unsigned long irq;

	/* Reports an empty policy before lazy initialization. */
	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	if (atomic_load_acquire(&policy_initialized) != 2)
		return;
	irq = spin_lock_irqsave(&policy_registry);
	for (index = 0; index < MOUNT_MAX; index++) {
		if (policies[index].mount != NULL)
			stats->mounts++;
	}
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		stats->memory_bytes += workers[index].memory.size;
		stats->passes += workers[index].passes;
		stats->errors += workers[index].errors;
		stats->busy += workers[index].busy;
		if (workers[index].leaf != NULL)
			stats->workers++;
		if (workers[index].last_error != 0)
			stats->last_error = workers[index].last_error;
	}
	spin_unlock_irqrestore(&policy_registry, irq);
}

/*
 * Reports policy names and physical counters without exposing kernel pointers.
 * The byte-buffer interface also supports an unaligned sysctl destination.
 */
void
writeback_policy_report(
	void *buffer)
{
	struct writeback_report_header header;
	struct writeback_mount_info entry;
	struct writeback_budget_stats totals;
	struct writeback_budget budget;
	struct writeback_worker *worker;
	unsigned index;
	unsigned long irq;
	char *output;

	/* Initializes every output byte, including unused records and ABI padding. */
	if (buffer == NULL)
		return;
	output = buffer;
	memset(output, 0, sizeof(struct writeback_report));
	memset(&header, 0, sizeof(header));
	header.version = WRITEBACK_REPORT_VERSION;
	writeback_budget_snapshot(&totals);
	header.high = totals.high;
	header.low = totals.low;
	header.device_high = totals.device_high;
	header.dirty = totals.dirty;
	header.reserved = totals.reserved;
	header.tickets = totals.tickets;
	header.refusals = totals.refusals;
	if (atomic_load_acquire(&policy_initialized) != 2) {
		memcpy(output, &header, sizeof(header));
		return;
	}

	/* Copies stable policy identities while control cannot withdraw their owners. */
	irq = spin_lock_irqsave(&policy_registry);
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		worker = &workers[index];
		header.memory_bytes += worker->memory.size;
		header.passes += worker->passes;
		header.errors += worker->errors;
		header.busy += worker->busy;
		if (worker->leaf != NULL)
			header.workers++;
		if (worker->last_error != 0)
			header.last_error = worker->last_error;
	}
	for (index = 0; index < MOUNT_MAX; index++) {
		if (policies[index].mount == NULL)
			continue;
		if (header.count == WRITEBACK_REPORT_MOUNTS)
			HAL_FATAL("writeback report capacity differs from mount table");
		worker = policies[index].worker;
		memset(&entry, 0, sizeof(entry));
		memcpy(entry.path, policies[index].mount->m_path, sizeof(entry.path));
		entry.path[sizeof(entry.path) - 1U] = '\0';
		if (worker->leaf != NULL) {
			memcpy(entry.device, worker->leaf->d_name, sizeof(entry.device));
			entry.device[sizeof(entry.device) - 1U] = '\0';
		}
		if (writeback_budget_read(&worker->budget, &budget) == 0) {
			entry.device_dirty = budget.dirty;
			entry.device_reserved = budget.reserved;
			entry.device_tickets = budget.tickets;
		}
		entry.state = worker->state;
		memcpy(output + offsetof(struct writeback_report, mounts) +
		    header.count * sizeof(entry), &entry, sizeof(entry));
		header.count++;
	}
	memcpy(output, &header, sizeof(header));
	spin_unlock_irqrestore(&policy_registry, irq);
}

/* Tests whether one budget currently occupies a registration slot. */
static int
budget_registered(
	struct writeback_budget *budget)
{
	unsigned index;

	/* Treats a missing budget as unregistered. */
	if (budget == NULL)
		return 0;

	/* Searches the registration slots for this budget. */
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (budgets[index] == budget)
			return 1;
	}

	/* Reports a budget that no slot holds. */
	return 0;
}

/* Reads the limits that follow the current cache target. */
static void
budget_current_limits(
	struct writeback_budget_stats *stats)
{
	struct cache_memory_stats cache;

	/* Derives the limits from the cache target of this moment. */
	cache_memory_get_stats(&cache);
	writeback_budget_limits(
		cache.target_bytes,
		&stats->high,
		&stats->low,
		&stats->device_high);
}

/* Guards the accounting without sleeping or entering another owner. */
static bool
budget_lock(
	void)
{
	bool enabled;

	/* Keeps completion-context writers from interrupting an owner of this guard. */
	enabled = hal_irq_disable();
	while (!atomic_try_acquire_zero(&budget_guard))
		hal_compiler_barrier();

	/* Reports the interrupt state the unlock has to restore. */
	return enabled;
}

/* Publishes the accounting before restoring the caller's interrupt state. */
static void
budget_unlock(
	bool enabled)
{
	/* Releases the guard, then restores interrupts only when they were enabled. */
	atomic_store_release(&budget_guard, 0);
	if (enabled)
		hal_irq_enable();
}

/* Initializes the bounded registry once without racing its sleeping primitives. */
static int
policy_initialize(void)
{
	unsigned expected;
	unsigned index;
	int error;

	/* Refuses a concurrent initializer instead of waiting on uninitialized locks. */
	if (atomic_load_acquire(&policy_initialized) == 2)
		return 0;
	expected = 0;
	if (!atomic_compare_exchange(&policy_initialized, &expected, 1))
		return EAGAIN;
	error = mutex_init(&policy_control, LOCK_RANK_WRITEBACK_CONTROL, "writeback control");
	if (error != 0) {
		atomic_store_release(&policy_initialized, 0);
		return error;
	}
	spin_init(&policy_registry, LOCK_RANK_WRITEBACK_REGISTRY, "writeback registry");
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++)
		waitq_init(&workers[index].wake, "writeback worker");
	atomic_store_release(&policy_initialized, 2);
	return 0;
}

/* Finds a stable policy while the registry guard or control mutex is held. */
static struct writeback_mount_policy *
policy_find(struct mount *mount)
{
	unsigned index;

	/* Searches the bounded mount table. */
	for (index = 0; index < MOUNT_MAX; index++) {
		if (policies[index].mount == mount)
			return &policies[index];
	}
	return NULL;
}

/* Allocates a private payload and control snapshot before starting its syncer. */
static int
worker_prepare(struct writeback_worker *worker)
{
	struct io_scratch memory;
	struct thread *thread;
	unsigned long irq;
	int error;

	/* Reuses resources retained after a retryable release failure. */
	if (!worker->attached) {
		error = writeback_budget_attach(&worker->budget, worker->leaf);
		if (error != 0)
			return error;
		worker->attached = 1;
	}
	if (worker->memory.size == 0) {
		memset(&memory, 0, sizeof(memory));
		error = io_scratch_alloc(WB_MEMORY, 1, &memory);
		if (error != HAL_OK || memory.vaddr == NULL || memory.size < WB_MEMORY) {
			if (memory.size != 0 && io_scratch_free(&memory) != HAL_OK)
				HAL_FATAL("writeback allocation rollback failed");
			return ENOMEM;
		}
		error = cache_memory_reserve(CACHE_MEMORY_WORKER, memory.size, 0);
		if (error != 0) {
			if (io_scratch_free(&memory) != HAL_OK)
				HAL_FATAL("writeback accounting rollback failed");
			return error;
		}
		cache_memory_commit(CACHE_MEMORY_WORKER, memory.size);
		memset(memory.vaddr, 0, memory.size);
		irq = spin_lock_irqsave(&policy_registry);
		worker->memory = memory;
		worker->charged = 1;
		spin_unlock_irqrestore(&policy_registry, irq);
	}

	/* Starts at most one permanent reusable thread for this record. */
	if (!worker->started) {
		error = kthread_create(worker_run, worker, SCHED_PRIORITY_DEFAULT, &thread);
		if (error != 0)
			return error;
		worker->started = 1;
		thread_start(thread);
	}
	return 0;
}

/* Returns idle resources, preserving a failed HAL release as a retryable owner. */
static int
worker_dispose(struct writeback_worker *worker)
{
	struct io_scratch memory;
	struct disk *leaf;
	unsigned long irq;
	int error;

	/* The caller has stopped admission and waited for every active owner. */
	memory = worker->memory;
	if (memory.size != 0) {
		if (io_scratch_free(&memory) != HAL_OK)
			return EIO;
		if (worker->charged)
			cache_memory_release(CACHE_MEMORY_WORKER, worker->memory.size);
		irq = spin_lock_irqsave(&policy_registry);
		memset(&worker->memory, 0, sizeof(worker->memory));
		worker->charged = 0;
		spin_unlock_irqrestore(&policy_registry, irq);
	}
	if (worker->attached) {
		error = writeback_budget_detach(&worker->budget);
		if (error != 0)
			HAL_FATAL("writeback disposal retained dirty owners");
		worker->attached = 0;
	}
	irq = spin_lock_irqsave(&policy_registry);
	leaf = worker->leaf;
	worker->leaf = NULL;
	worker->state = WB_OFF;
	worker->requested = 0;
	spin_unlock_irqrestore(&policy_registry, irq);
	if (leaf != NULL)
		disk_cache_release(leaf);
	return 0;
}

/* Publishes an enabled mount only after all device resources are usable. */
static int
policy_enable(struct mount *mount)
{
	struct writeback_mount_policy *policy;
	struct writeback_worker *worker;
	struct disk *leaf;
	unsigned index;
	unsigned long irq;
	int error;

	/* Restricts delayed data to a writable backend with an allocation proof. */
	if (mount->m_state != MOUNT_STATE_LIVE || mount->m_type == NULL ||
	    mount->m_type->writeback_range == NULL || mount->m_disk == NULL)
		return EOPNOTSUPP;
	if ((mount->m_flags & MOUNT_READ_ONLY) != 0)
		return EROFS;
	policy = policy_find(mount);
	if (policy != NULL)
		return policy->worker->unmount != NULL ? EBUSY : 0;
	policy = NULL;
	for (index = 0; index < MOUNT_MAX; index++) {
		if (policies[index].mount == NULL) {
			policy = &policies[index];
			break;
		}
	}
	if (policy == NULL)
		return ENOSPC;
	error = writeback_domain_acquire(mount->m_disk, &leaf);
	if (error != 0)
		return error;

	/* Shares the physical worker, including across independent loop mounts. */
	worker = NULL;
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (workers[index].leaf == leaf) {
			worker = &workers[index];
			break;
		}
	}
	if (worker == NULL) {
		for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
			if (workers[index].leaf == NULL) {
				worker = &workers[index];
				break;
			}
		}
		if (worker == NULL) {
			disk_cache_release(leaf);
			return EAGAIN;
		}
		irq = spin_lock_irqsave(&policy_registry);
		worker->leaf = leaf;
		spin_unlock_irqrestore(&policy_registry, irq);
	} else {
		disk_cache_release(leaf);
		if (worker->unmount != NULL)
			return EBUSY;
	}
	error = worker_prepare(worker);
	if (error != 0) {
		(void)worker_dispose(worker);
		irq = spin_lock_irqsave(&policy_registry);
		worker->last_error = error;
		worker->errors++;
		spin_unlock_irqrestore(&policy_registry, irq);
		return error;
	}

	/* Reopens any clean budget retained after a committed teardown release failure. */
	if (writeback_budget_quiesce(&worker->budget, 0) != 0)
		HAL_FATAL("writeback enable lost budget");

	/* Publishes the retained mount after worker construction succeeds. */
	mount_ref(mount);
	irq = spin_lock_irqsave(&policy_registry);
	policy->mount = mount;
	policy->worker = worker;
	worker->state = WB_LIVE;
	waitq_wake_all(&worker->wake);
	spin_unlock_irqrestore(&policy_registry, irq);
	return 0;
}

/* Reopens a paused physical budget after success or a retryable failure. */
static void
worker_restore(struct writeback_worker *worker)
{
	unsigned long irq;

	/* Restores budget admission before publishing a usable policy. */
	if (writeback_budget_quiesce(&worker->budget, 0) != 0)
		HAL_FATAL("writeback restore lost budget");
	irq = spin_lock_irqsave(&policy_registry);
	worker->state = WB_LIVE;
	waitq_wake_all(&worker->wake);
	spin_unlock_irqrestore(&policy_registry, irq);
}

/* Drains one target while preserving sibling policy and failed dirty ownership. */
static int
policy_disable(struct mount *mount)
{
	struct writeback_mount_policy *policy;
	struct writeback_worker *worker;
	struct writeback_budget snapshot;
	unsigned long irq;
	unsigned siblings;
	int error;

	/* Makes repeated disable idempotent. */
	policy = policy_find(mount);
	if (policy == NULL)
		return 0;
	worker = policy->worker;
	if (worker->unmount != NULL)
		return EBUSY;
	error = worker_pause(worker);
	if (error != 0)
		return error;
	error = worker_sync_mount(mount, worker->memory.vaddr);
	if (error != 0) {
		worker_restore(worker);
		return error;
	}

	/* Releases the final reserve before committing policy removal. */
	siblings = worker_siblings(policy);
	if (siblings == 0) {
		error = writeback_budget_read(&worker->budget, &snapshot);
		if (error != 0 || snapshot.dirty != 0) {
			worker_restore(worker);
			return error != 0 ? error : EBUSY;
		}
		error = worker_dispose(worker);
		if (error != 0) {
			worker_restore(worker);
			return error;
		}
	}
	irq = spin_lock_irqsave(&policy_registry);
	policy->mount = NULL;
	policy->worker = NULL;
	spin_unlock_irqrestore(&policy_registry, irq);
	mount_release(mount);
	if (siblings != 0)
		worker_restore(worker);
	return 0;
}

/* Performs bounded age/pressure passes above all lower BIO submission workers. */
static void
worker_run(void *argument)
{
	struct writeback_worker *worker;
	struct mount **snapshot;
	unsigned index;
	unsigned count;
	unsigned long irq;
	uint64_t deadline;
	uint64_t sequence;
	int error;
	int first_error;

	/* Keeps the static thread record reusable across successful off/on cycles. */
	worker = argument;
	deadline = clock_ticks() + WB_AGE;
	for (;;) {
		irq = spin_lock_irqsave(&policy_registry);
		while (worker->state != WB_LIVE ||
		    (!worker->requested && clock_ticks() < deadline)) {
			sequence = waitq_sequence(&worker->wake);
			(void)waitq_sleep(&worker->wake, &policy_registry, sequence,
			    worker->state == WB_LIVE ? deadline : 0, 0);
		}
		worker->requested = 0;
		worker->busy = 1;
		snapshot = (struct mount **)((char *)worker->memory.vaddr + WRITEBACK_TICKET_BYTES);
		count = 0;
		for (index = 0; index < MOUNT_MAX; index++) {
			if (policies[index].mount == NULL || policies[index].worker != worker)
				continue;
			snapshot[count] = policies[index].mount;
			mount_ref(snapshot[count++]);
		}
		spin_unlock_irqrestore(&policy_registry, irq);

		/* Drains independent mounts with scratch that cannot be borrowed recursively. */
		first_error = 0;
		for (index = 0; index < count; index++) {
			error = worker_sync_mount(snapshot[index], worker->memory.vaddr);
			if (error != 0 && first_error == 0)
				first_error = error;
			mount_release(snapshot[index]);
		}
		irq = spin_lock_irqsave(&policy_registry);
		worker->passes++;
		if (first_error != 0) {
			worker->errors++;
			worker->last_error = first_error;
		}
		worker->busy = 0;
		deadline = clock_ticks() + WB_AGE;
		waitq_wake_all(&worker->wake);
		spin_unlock_irqrestore(&policy_registry, irq);
	}
}

/* Drains background data without consuming a public mount error observer. */
static int
worker_sync_mount(
	struct mount *mount,
	void *scratch)
{
	int error;

	/* Keeps dirty generations and their writer on a failed VM persistence attempt. */
	error = vm_object_sync_mount_buffer(mount, scratch, WRITEBACK_TICKET_BYTES);
	if (error != 0)
		return error;

	/* Flushes backend metadata without certifying unrelated concurrent VM writes. */
	error = mount_sync_backend(mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Closes admission and joins every ticket and active pass before borrowing scratch. */
static int
worker_pause(struct writeback_worker *worker)
{
	struct writeback_budget snapshot;
	uint64_t sequence;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&policy_registry);
	worker->state = WB_PAUSED;
	spin_unlock_irqrestore(&policy_registry, irq);
	error = writeback_budget_quiesce(&worker->budget, 1);
	if (error != 0)
		HAL_FATAL("writeback pause lost budget");

	/* Waits without holding a file/content lease; a signal restores the old policy. */
	irq = spin_lock_irqsave(&policy_registry);
	for (;;) {
		error = writeback_budget_read(&worker->budget, &snapshot);
		if (error != 0)
			HAL_FATAL("writeback wait lost budget");
		if (!worker->busy && snapshot.tickets == 0)
			break;
		sequence = waitq_sequence(&worker->wake);
		error = waitq_sleep(&worker->wake, &policy_registry, sequence,
		    clock_ticks() + 1U, WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN && error != ETIMEDOUT) {
			spin_unlock_irqrestore(&policy_registry, irq);
			worker_restore(worker);
			return error;
		}
	}
	spin_unlock_irqrestore(&policy_registry, irq);
	return 0;
}

/* Counts other policies sharing the selected physical worker under control serialization. */
static unsigned
worker_siblings(struct writeback_mount_policy *policy)
{
	unsigned index;
	unsigned count;

	/* Retains sibling policy resources during a single-mount operation. */
	count = 0;
	for (index = 0; index < MOUNT_MAX; index++) {
		if (policies[index].mount != NULL && &policies[index] != policy &&
		    policies[index].worker == policy->worker)
			count++;
	}
	return count;
}
