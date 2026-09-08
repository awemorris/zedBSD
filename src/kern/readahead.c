/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Readahead.
 *
 * One unit holds the sequential stream prediction that proposes the next
 * missing window and the bounded worker pool that fills the windows it
 * proposes.  A proposal carries the generation of its stream, so a seek or
 * memory pressure invalidates outstanding work instead of cancelling it by
 * hand.
 */

#include <kern/readahead.h>
#include <kern/page.h>
#include <errno.h>
#include <string.h>
#include <kern/cache-memory.h>
#include <kern/io-scratch.h>
#include <kern/writeback.h>
#include <kern/file.h>
#include <kern/vm-object.h>
#include <kern/thread.h>
#include <kern/sched.h>

#define RA_WORKERS 4U

#define RA_SLOTS 2U

#define RA_MEMORY (READAHEAD_REQUEST_MAX + ZEDBSD_PAGE_SIZE)

#define RA_FREE 0U

#define RA_PREPARING 1U

#define RA_QUEUED 2U

#define RA_RUNNING 3U

#define RA_COMPLETING 4U

struct readahead_job {
	struct file *origin;
	struct mount *mount;
	struct disk *leaf;
	struct vm_object_prefetch fill;
	uint64_t order;
	unsigned state;
	unsigned canceled;
};

struct readahead_worker {
	struct readahead_job jobs[RA_SLOTS];
	struct io_scratch memory;
	struct wait_queue wake;
	unsigned started;
};

static atomic_uint_t initialized;

static struct mutex control;

static struct spinlock registry;

static struct wait_queue changed;

static struct readahead_worker workers[RA_WORKERS];

static struct readahead_boundary *boundaries;

static struct readahead_stats counters;

static uint64_t next_order;

static int initialize(void);
static int blocked(struct mount *mount);
static int prepare_worker(struct readahead_worker *worker);
static void run_worker(void *argument);
static struct readahead_job *next_job(struct readahead_worker *worker);
static void retire_job(struct readahead_worker *worker, struct readahead_job *job);
static void cancel_mount(struct mount *mount);
static int mount_busy(struct mount *mount);

/*
 * Invalidates outstanding stream work without ever reusing an exhausted
 * generation.
 */
void
readahead_reset(
	struct readahead_state *state)
{
	/* Keeps a missing optional owner harmless. */
	if (state == NULL)
		return;

	/* Forgets the stream, leaving the generation to invalidate issued work. */
	state->valid = 0;
	state->sequential = 0;
	state->window = 0;
	state->useful = 0;
	state->next = 0;
	state->issued_end = 0;

	/* Saturation permanently disables this description instead of admitting ABA. */
	if (state->generation == UINT64_MAX) {
		state->exhausted = 1;
		return;
	}
	state->generation++;
}

/*
 * Plans one bounded missing window after consecutive successful demand reads.
 *
 * The proposal is written to request; a zero length means that this read
 * justifies no prefetch.
 */
int
readahead_observe(
	struct readahead_state *state,
	uint64_t offset,
	size_t length,
	uint64_t eof,
	uint64_t useful,
	int pressure,
	struct readahead_request *request)
{
	uint64_t end;
	uint64_t first;
	uint64_t limit;
	uint64_t eof_rounded;
	uint64_t available;

	/* Rejects impossible demand ranges without overflowing endpoint arithmetic. */
	if (state == NULL || request == NULL)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	if (eof > INT64_MAX ||
	    offset > eof ||
	    (uint64_t)length > eof - offset) {
		readahead_reset(state);
		return EINVAL;
	}

	/* Drops the stream under memory pressure and on an empty read. */
	if (pressure || length == 0) {
		readahead_reset(state);
		return 0;
	}

	/* Proposes nothing once the generation space of this stream is spent. */
	if (state->exhausted)
		return 0;
	end = offset + (uint64_t)length;

	/* Starts a new stream and cancels prior work on random or reordered access. */
	if (state->valid && offset != state->next)
		readahead_reset(state);
	if (state->exhausted)
		return 0;
	if (!state->valid) {
		if (state->generation == 0)
			state->generation = 1;
		state->valid = 1;
		state->next = end;
		state->issued_end = end;
		state->sequential = 1;
		state->window = READAHEAD_MIN_WINDOW;
		return 0;
	}

	/* Grows only after useful prefetched bytes justify a larger lookahead window. */
	state->next = end;
	if (state->sequential < 2)
		state->sequential++;
	if (useful > READAHEAD_MIN_WINDOW - state->useful)
		useful = READAHEAD_MIN_WINDOW - state->useful;
	state->useful += useful;
	if (state->useful == READAHEAD_MIN_WINDOW)
		state->window = READAHEAD_MAX_WINDOW;

	/* Bounds issued bytes relative to demand progress, even for one-byte reads. */
	first = (end + ZEDBSD_PAGE_SIZE - 1U) & ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	if (first >= eof)
		return 0;
	limit = first + state->window;
	eof_rounded = (eof + ZEDBSD_PAGE_SIZE - 1U) &
	    ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	if (limit > eof_rounded)
		limit = eof_rounded;
	if (first < state->issued_end)
		first = state->issued_end;
	if (first >= limit || first >= eof)
		return 0;
	available = limit - first;

	/* Refills in half-window runs instead of submitting another page on every read. */
	if (available < READAHEAD_MIN_WINDOW / 2U && limit != eof_rounded)
		return 0;
	if (available > READAHEAD_REQUEST_MAX)
		available = READAHEAD_REQUEST_MAX;

	/* Transfers a candidate generation to the separately bounded queue owner. */
	request->generation = state->generation;
	request->offset = first;
	request->length = (size_t)available;
	state->issued_end = first + available;

	/* Reports an accepted observation, with or without a proposal. */
	return 0;
}

/*
 * Rejects work invalidated by a seek, pressure, close, or stream
 * discontinuity.
 *
 * The queue must separately revalidate current VM identity and EOF.
 */
int
readahead_current(
	const struct readahead_state *state,
	const struct readahead_request *request)
{
	/* Refuses a request that no longer belongs to the live stream. */
	if (state == NULL ||
	    request == NULL ||
	    !state->valid ||
	    state->exhausted ||
	    request->length == 0 ||
	    request->generation != state->generation)
		return 0;

	/* Reports work that the stream still owns. */
	return 1;
}

/*
 * Admits one bounded optional fill without waiting for queue space or a control owner.
 */
int
readahead_submit(
	struct file *origin,
	struct inode *inode,
	const struct readahead_request *request)
{
	struct thread *current;
	struct readahead_worker *worker;
	struct readahead_worker *empty;
	struct readahead_job *job;
	struct disk *leaf;
	struct mount *mount;
	off_t offset;
	unsigned index;
	unsigned slot;
	unsigned used;
	unsigned long irq;
	int error;

	/* Idle/bootstrap callers cannot sleep behind an independently running reader. */
	current = thread_current();
	if (current == NULL || (current->flags & THREAD_FLAG_IDLE) != 0)
		return EAGAIN;

	/* Validates the caller-owned identities before any asynchronous ownership. */
	if (origin == NULL || inode == NULL || request == NULL ||
	    request->generation == 0 || request->length == 0 ||
	    request->length > READAHEAD_REQUEST_MAX)
		return EINVAL;
	offset = (off_t)request->offset;
	if (offset < 0 || (uint64_t)offset != request->offset ||
	    (request->offset & (ZEDBSD_PAGE_SIZE - 1U)) != 0 ||
	    (request->length & (ZEDBSD_PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	mount = inode->i_mount;
	if (mount == NULL || mount->m_disk == NULL)
		return EOPNOTSUPP;
	error = initialize();
	if (error != 0)
		return error;
	if (!mutex_trylock(&control))
		return EAGAIN;
	error = writeback_domain_acquire(mount->m_disk, &leaf);
	if (error != 0) {
		mutex_unlock(&control);
		return error;
	}

	/* Serializes the admission identity against reset/close before publishing a slot. */
	if (!mutex_trylock(&origin->f_lock)) {
		disk_cache_release(leaf);
		mutex_unlock(&control);
		return EAGAIN;
	}
	if (!readahead_current(&origin->f_readahead, request)) {
		mutex_unlock(&origin->f_lock);
		disk_cache_release(leaf);
		mutex_unlock(&control);
		return EAGAIN;
	}

	/* Shares a physical worker while any of its jobs retain that device identity. */
	irq = spin_lock_irqsave(&registry);
	worker = NULL;
	empty = NULL;
	for (index = 0; index < RA_WORKERS; index++) {
		used = 0;
		for (slot = 0; slot < RA_SLOTS; slot++) {
			job = &workers[index].jobs[slot];
			if (job->state == RA_FREE)
				continue;
			used++;
			if (job->leaf == leaf)
				worker = &workers[index];
		}
		if (used == 0 && empty == NULL)
			empty = &workers[index];
	}
	if (worker == NULL)
		worker = empty;
	job = NULL;
	if (worker != NULL) {
		for (slot = 0; slot < RA_SLOTS; slot++) {
			if (worker->jobs[slot].state == RA_FREE) {
				job = &worker->jobs[slot];
				break;
			}
		}
	}
	if (job == NULL || blocked(mount) || next_order == UINT64_MAX) {
		counters.refusals++;
		spin_unlock_irqrestore(&registry, irq);
		mutex_unlock(&origin->f_lock);
		disk_cache_release(leaf);
		mutex_unlock(&control);
		return EAGAIN;
	}

	/* Registers preparation before allocation so cancel and teardown can join it. */
	job->origin = origin;
	job->mount = mount;
	job->leaf = leaf;
	job->order = ++next_order;
	job->state = RA_PREPARING;
	counters.jobs++;
	counters.requested_bytes += request->length;
	spin_unlock_irqrestore(&registry, irq);
	mutex_unlock(&origin->f_lock);
	error = prepare_worker(worker);
	if (error == 0)
		error = vm_object_prefetch_prepare(inode, offset, request->length, &job->fill);
	if (error != 0) {
		retire_job(worker, job);
		mutex_unlock(&control);
		return error;
	}

	/* Hands the private fill to its worker, including already canceled preparations. */
	irq = spin_lock_irqsave(&registry);
	job->state = RA_QUEUED;
	waitq_wake_all(&worker->wake);
	spin_unlock_irqrestore(&registry, irq);
	mutex_unlock(&control);
	return 0;
}

/*
 * Invalidates an origin identity without dereferencing or prematurely freeing it.
 */
void
readahead_cancel(
	struct file *origin)
{
	struct readahead_job *job;
	unsigned index;
	unsigned slot;
	unsigned long irq;

	/* Makes unused queues and repeated final-close cancellation harmless. */
	if (origin == NULL || atomic_load_acquire(&initialized) != 2)
		return;
	irq = spin_lock_irqsave(&registry);
	for (index = 0; index < RA_WORKERS; index++) {
		for (slot = 0; slot < RA_SLOTS; slot++) {
			job = &workers[index].jobs[slot];
			if (job->state == RA_FREE || job->origin != origin)
				continue;
			job->origin = NULL;
			job->canceled = 1;
		}
		waitq_wake_all(&workers[index].wake);
	}
	spin_unlock_irqrestore(&registry, irq);
}

/*
 * Gives active demand transactions priority over pending optional device reads.
 */
int
readahead_demand_begin(
	void)
{
	unsigned long irq;
	int error;

	/* Refuses optional priority tracking instead of stalling demand on setup failure. */
	error = initialize();
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&registry);
	if (counters.demand == ~0U)
		HAL_FATAL("readahead demand counter overflow");
	counters.demand++;
	spin_unlock_irqrestore(&registry, irq);
	return 0;
}

/*
 * Releases a demand transaction and wakes optional work when all demand has left.
 */
void
readahead_demand_end(
	void)
{
	unsigned index;
	unsigned long irq;

	/* Balances only transactions that called demand_begin. */
	irq = spin_lock_irqsave(&registry);
	if (counters.demand == 0)
		HAL_FATAL("readahead demand counter underflow");
	counters.demand--;
	if (counters.demand == 0) {
		for (index = 0; index < RA_WORKERS; index++)
			waitq_wake_all(&workers[index].wake);
	}
	spin_unlock_irqrestore(&registry, irq);
}

/*
 * Closes optional admission and joins all matching ownership before mount teardown.
 */
int
readahead_boundary_begin(
	struct readahead_boundary *boundary,
	struct mount *mount)
{
	uint64_t sequence;
	unsigned long irq;
	int error;

	/* Requires caller-owned, zero-initialized lifetime for a boundary token. */
	if (boundary == NULL || boundary->active)
		return EINVAL;
	error = initialize();
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&registry);
	boundary->mount = mount;
	boundary->next = boundaries;
	boundary->active = 1;
	boundaries = boundary;
	cancel_mount(mount);

	/* Joins preparation and terminal cleanup as well as non-preemptible device I/O. */
	while (mount_busy(mount)) {
		sequence = waitq_sequence(&changed);
		error = waitq_sleep(&changed, &registry, sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&registry, irq);
			readahead_boundary_end(boundary);
			return error;
		}
	}
	spin_unlock_irqrestore(&registry, irq);
	return 0;
}

/*
 * Reopens admission after an aborted boundary or a safely retired mount identity.
 */
void
readahead_boundary_end(
	struct readahead_boundary *boundary)
{
	struct readahead_boundary **link;
	unsigned long irq;

	/* Makes a failed or already finished begin harmless. */
	if (boundary == NULL || atomic_load_acquire(&initialized) != 2)
		return;
	irq = spin_lock_irqsave(&registry);
	for (link = &boundaries; *link != NULL; link = &(*link)->next) {
		if (*link == boundary) {
			*link = boundary->next;
			memset(boundary, 0, sizeof(*boundary));
			break;
		}
	}
	spin_unlock_irqrestore(&registry, irq);
}

/*
 * Accounts confirmed demand consumption and retired uncredited speculative bytes.
 */
void
readahead_consumed(
	size_t useful,
	size_t unused)
{
	unsigned long irq;

	/* Leaves reduced kernels and pre-service VM fixtures independent. */
	if (atomic_load_acquire(&initialized) != 2)
		return;
	irq = spin_lock_irqsave(&registry);
	counters.useful_bytes += useful;
	counters.unused_bytes += unused;
	spin_unlock_irqrestore(&registry, irq);
}

/*
 * Copies independent speculative accounting without consuming demand counters.
 */
void
readahead_snapshot(
	struct readahead_stats *stats)
{
	unsigned index;
	unsigned long irq;

	/* Returns an empty optional service before its first initialization. */
	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	if (atomic_load_acquire(&initialized) != 2)
		return;
	irq = spin_lock_irqsave(&registry);
	*stats = counters;
	for (index = 0; index < RA_WORKERS; index++)
		stats->memory_bytes += workers[index].memory.size;
	spin_unlock_irqrestore(&registry, irq);
}

/*
 * Exports a fixed-width versioned snapshot independently of kernel structure layout.
 */
void
readahead_report(
	struct readahead_report *report)
{
	struct readahead_stats stats;

	/* Initializes every output byte, including an inactive optional service. */
	if (report == NULL)
		return;
	readahead_snapshot(&stats);
	memset(report, 0, sizeof(*report));
	report->version = READAHEAD_REPORT_VERSION;
	report->requested_bytes = stats.requested_bytes;
	report->started_bytes = stats.started_bytes;
	report->published_bytes = stats.published_bytes;
	report->confirmed_useful_bytes = stats.useful_bytes;
	report->retired_uncredited_bytes = stats.unused_bytes;
	report->discarded_fill_bytes = stats.discarded_bytes;
	report->errors = stats.errors;
	report->queue_refusals = stats.refusals;
	report->memory_bytes = stats.memory_bytes;
	report->jobs = stats.jobs;
	report->running = stats.running;
	report->demand = stats.demand;
}

/*
 * Returns idle scratch while preserving charged ownership if HAL cannot free it.
 */
int
readahead_trim(
	void)
{
	struct io_scratch memory;
	struct readahead_worker *worker;
	unsigned index;
	unsigned slot;
	unsigned busy;
	unsigned long irq;
	int error;

	/* Serializes allocation and disposal without holding a spinlock over HAL. */
	if (atomic_load_acquire(&initialized) != 2)
		return 0;
	mutex_lock(&control);
	error = 0;
	for (index = 0; index < RA_WORKERS; index++) {
		worker = &workers[index];
		irq = spin_lock_irqsave(&registry);
		busy = 0;
		for (slot = 0; slot < RA_SLOTS; slot++) {
			if (worker->jobs[slot].state != RA_FREE)
				busy = 1;
		}
		memory = worker->memory;
		spin_unlock_irqrestore(&registry, irq);
		if (busy || memory.size == 0)
			continue;
		if (io_scratch_free(&memory) != HAL_OK) {
			error = EIO;
			continue;
		}
		cache_memory_release(CACHE_MEMORY_WORKER, worker->memory.size);
		irq = spin_lock_irqsave(&registry);
		memset(&worker->memory, 0, sizeof(worker->memory));
		spin_unlock_irqrestore(&registry, irq);
	}
	mutex_unlock(&control);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Initializes the fixed registry before publishing any locks or wait queues. */
static int
initialize(
	void)
{
	unsigned expected;
	unsigned index;
	int error;

	/* Refuses optional races with an initializer whose mutex is not usable yet. */
	if (atomic_load_acquire(&initialized) == 2)
		return 0;
	expected = 0;
	if (!atomic_compare_exchange(&initialized, &expected, 1))
		return EAGAIN;
	error = mutex_init(&control, LOCK_RANK_READAHEAD_CONTROL, "readahead control");
	if (error != 0) {
		atomic_store_release(&initialized, 0);
		return error;
	}
	spin_init(&registry, LOCK_RANK_READAHEAD_REGISTRY, "readahead registry");
	waitq_init(&changed, "readahead retirement");
	for (index = 0; index < RA_WORKERS; index++)
		waitq_init(&workers[index].wake, "readahead worker");
	atomic_store_release(&initialized, 2);
	return 0;
}

/* Tests the bounded teardown gates while the registry guard is held. */
static int
blocked(
	struct mount *mount)
{
	struct readahead_boundary *boundary;

	/* Any global boundary dominates a matching mount boundary. */
	for (boundary = boundaries; boundary != NULL; boundary = boundary->next) {
		if (boundary->mount == NULL || boundary->mount == mount)
			return 1;
	}
	return 0;
}

/* Reserves actual payload/control allocation and starts one reusable kernel thread. */
static int
prepare_worker(
	struct readahead_worker *worker)
{
	struct io_scratch memory;
	struct thread *thread;
	unsigned long irq;
	int error;

	/* Uses optional shared-budget admission for the actual HAL allocation size. */
	if (worker->memory.size == 0) {
		memset(&memory, 0, sizeof(memory));
		error = io_scratch_alloc(RA_MEMORY, 1, &memory);
		if (error != HAL_OK || memory.vaddr == NULL || memory.size < RA_MEMORY) {
			if (memory.size != 0 && io_scratch_free(&memory) != HAL_OK)
				HAL_FATAL("readahead allocation rollback failed");
			return ENOMEM;
		}
		error = cache_memory_reserve(CACHE_MEMORY_WORKER, memory.size, 1);
		if (error != 0) {
			if (io_scratch_free(&memory) != HAL_OK)
				HAL_FATAL("readahead accounting rollback failed");
			return error;
		}
		cache_memory_commit(CACHE_MEMORY_WORKER, memory.size);
		irq = spin_lock_irqsave(&registry);
		worker->memory = memory;
		spin_unlock_irqrestore(&registry, irq);
	}

	/* Reuses permanent thread records after scratch trimming or a device change. */
	if (!worker->started) {
		error = kthread_create(run_worker, worker, SCHED_PRIORITY_DEFAULT, &thread);
		if (error != 0)
			return error;
		worker->started = 1;
		thread_start(thread);
	}
	return 0;
}

/* Selects FIFO optional work, allowing canceled jobs to retire despite demand. */
static struct readahead_job *
next_job(
	struct readahead_worker *worker)
{
	struct readahead_job *best;
	struct readahead_job *job;
	unsigned slot;

	/* Demand delays only new device I/O, never cancellation cleanup. */
	best = NULL;
	for (slot = 0; slot < RA_SLOTS; slot++) {
		job = &worker->jobs[slot];
		if (job->state != RA_QUEUED || (counters.demand != 0 && !job->canceled))
			continue;
		if (best == NULL || job->order < best->order)
			best = job;
	}
	return best;
}

/* Reads through the independent VM read owner and conditionally adopts completion. */
static void
run_worker(
	void *argument)
{
	struct readahead_worker *worker;
	struct readahead_job *job;
	uint64_t sequence;
	unsigned long irq;
	unsigned canceled;
	size_t requested;
	size_t published;
	ssize_t received;
	int error;

	/* Waits on a bounded reusable worker record without retaining a user file. */
	worker = argument;
	for (;;) {
		irq = spin_lock_irqsave(&registry);
		job = next_job(worker);
		while (job == NULL) {
			sequence = waitq_sequence(&worker->wake);
			(void)waitq_sleep(&worker->wake, &registry, sequence, 0, 0);
			job = next_job(worker);
		}
		job->state = RA_RUNNING;
		canceled = job->canceled;
		requested = job->fill.length;
		counters.running++;
		if (!canceled)
			counters.started_bytes += requested;
		spin_unlock_irqrestore(&registry, irq);

		/* Runs no backend request for a canceled queued fill. */
		received = 0;
		if (!canceled) {
			received = file_pread_internal(job->fill.object->file,
			    worker->memory.vaddr, requested, job->fill.offset, FILE_IO_VM_OBJECT);
		}

		/* Linearizes adoption before dropping the guard for VM and disk retirement. */
		irq = spin_lock_irqsave(&registry);
		canceled = job->canceled;
		job->state = RA_COMPLETING;
		job->origin = NULL;
		spin_unlock_irqrestore(&registry, irq);
		published = 0;
		error = 0;
		if (!canceled)
			error = vm_object_prefetch_complete(&job->fill,
			    worker->memory.vaddr, received, &published);

		/* Separates transport/short-read errors from ordinary stale optional results. */
		irq = spin_lock_irqsave(&registry);
		counters.published_bytes += published;
		counters.discarded_bytes += requested - published;
		if (error != 0 && error != EAGAIN)
			counters.errors++;
		counters.running--;
		spin_unlock_irqrestore(&registry, irq);
		retire_job(worker, job);
	}
}

/* Releases every owned resource before advertising a reusable slot or drained gate. */
static void
retire_job(
	struct readahead_worker *worker,
	struct readahead_job *job)
{
	struct disk *leaf;
	unsigned long irq;

	/* Keeps the visible slot busy throughout potentially sleeping cleanup. */
	vm_object_prefetch_abort(&job->fill);
	leaf = job->leaf;
	if (leaf != NULL)
		disk_cache_release(leaf);
	irq = spin_lock_irqsave(&registry);
	memset(job, 0, sizeof(*job));
	counters.jobs--;
	waitq_wake_all(&changed);
	waitq_wake_all(&worker->wake);
	spin_unlock_irqrestore(&registry, irq);
}

/* Cancels all matching states without freeing in-flight resources. */
static void
cancel_mount(
	struct mount *mount)
{
	struct readahead_job *job;
	unsigned index;
	unsigned slot;

	/* Teardown also joins terminally claimed adoption before retiring the mount. */
	for (index = 0; index < RA_WORKERS; index++) {
		for (slot = 0; slot < RA_SLOTS; slot++) {
			job = &workers[index].jobs[slot];
			if (job->state == RA_FREE || (mount != NULL && job->mount != mount))
				continue;
			job->canceled = 1;
			job->origin = NULL;
		}
		waitq_wake_all(&workers[index].wake);
	}
}

/* Tests whether every matching preparation, read and terminal cleanup has left. */
static int
mount_busy(
	struct mount *mount)
{
	struct readahead_job *job;
	unsigned index;
	unsigned slot;

	/* A slot remains occupied until its disk and VM ownership have been retired. */
	for (index = 0; index < RA_WORKERS; index++) {
		for (slot = 0; slot < RA_SLOTS; slot++) {
			job = &workers[index].jobs[slot];
			if (job->state != RA_FREE && (mount == NULL || job->mount == mount))
				return 1;
		}
	}
	return 0;
}
