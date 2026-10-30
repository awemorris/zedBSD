/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reclaim queue of private pages and the reclaim of one page to swap
 * or to its file.
 */

#include "kern/vm-object.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/vm-reclaim.h"
#include "kern/vm-lock.h"
#include "kern/vmspace.h"
#include "kern/vm-commit.h"
#include "kern/io-pool.h"
#include "kern/disk.h"
#include "kern/cache-memory.h"
#include "kern/writeback.h"
#include <uapi/errno.h>
#include <uapi/mman.h>
#include "kern/lock.h"
#include "kern/swap.h"
#include <hal/hal.h>
#include <kern/pmem.h>
#include <limits.h>
#include "kern/sched.h"
#include "kern/clock.h"
#include "kern/thread.h"
#include "kern/process.h"
#include "kern/signal.h"
#include "kern/klog.h"
#include <uapi/signal.h>
#include <kern/kcrt.h>
#include "reclaim-internal.h"
#include "vmspace-internal.h"

#define VM_OBJECT_FAULT_RESERVE_PAGES	4U

/*
 * How long a fault waits with no frame at all before the out-of-memory
 * killer runs, in seconds.  Commit guarantees the memory exists in RAM or
 * swap, so this is reached only when swap cannot be written.
 */
#define VM_OOM_WAIT_SECONDS		10U

/*
 * How many pages a file system's data allocation reclaims before it gives
 * up (vm_reclaim_frame_private()); each pass frees one page or stops.
 */
#define VM_PRIVATE_FRAME_ATTEMPTS	64U

#define stats				vm_reclaim_counters

/*
 * The list of private pages that reclaim may write to swap.
 */
static struct vm_private_page *page_queue;

/*
 * The oldest end of the reclaim queue, where the clock hand starts.
 *
 * Pages enter at the head; one that was used since the hand last passed
 * is moved back to the head, so the tail holds the pages left alone the
 * longest.
 */
static struct vm_private_page *page_queue_tail;

/*
 * The tracked backings whose page is on swap.
 *
 * They leave the reclaim queue when they are written out, so the clock
 * never walks pages that cannot be reclaimed, and return to it when they
 * are read back in.  Draining a swap source finds its pages here.
 */
static struct vm_private_page *swapped_queue;

/*
 * The mutex that guards the private page queue above.
 */
struct mutex reclaim_lock;

/*
 * The reclaim statistics, published for the sysctl that reports them.
 */
struct vm_reclaim_stats vm_reclaim_counters;

/*
 * Optional collaborators.
 *
 * Each of these lives in a translation unit that some platforms leave out of
 * the kernel, so the reference is weak and every call site tests it first.
 */
extern size_t cache_memory_reclaim(size_t) __attribute__((weak));

static uint32_t mapping_prot(const struct vm_page *page);
static int backing_wired_or_avoided(struct vm_private_page *backing, struct vm_page *avoid);
static int backing_has_wired_mapping(struct vm_private_page *backing);
static int backing_has_busy_mapping(struct vm_private_page *backing);
static uint32_t backing_pte_flags(struct vm_private_page *backing);
static void backing_clear_accessed(struct vm_private_page *backing);
static int backing_hold_spaces(struct vm_private_page *backing);
static void backing_release_spaces(struct vm_private_page *backing);
static int reclaim_take_candidate(struct vm_private_page *backing);
static void pin_backing_mappings(struct vm_private_page *backing);
static void unpin_backing_mappings(struct vm_private_page *backing);
static int unmap_backing_ptes(struct vm_private_page *backing, uint32_t *pte_flags);
static void rollback_backing_ptes(struct vm_private_page *backing);
static int swap_out_backing_owned(struct vm_private_page *backing);
static int discard_file_backing_owned(struct vm_private_page *backing);
static int private_page_targets_source(struct vm_private_page *backing, unsigned source_id);
static void drain_mapping_pins_release(struct vm_private_page *backing);
static int reclaim_backing_owned(struct vm_private_page *backing, int file_candidate);

/*
 * Initializes the reclaim queue and statistics.
 */
void
vm_reclaim_init(
	void)
{
	vm_metadata_init();

	page_queue = NULL;
	page_queue_tail = NULL;
	swapped_queue = NULL;

	/* The page-out worker starts on first need, once threads exist. */
	spin_init(&pageout_lock, LOCK_RANK_VM_OBJECT, "VM page-out");
	waitq_init(&pageout_waitq, "VM page-out");
	waitq_init(&free_waitq, "VM free memory");
	pageout_started = 0;
	pageout_requested = 0;

	kern_memset(&stats, 0, sizeof(stats));

	(void)mutex_init(&reclaim_lock, LOCK_RANK_VMSPACE, "VM reclaim");
}

/*
 * Reclaims clean cache first, then private or writeback-backed pages.
 */
int
vm_reclaim_one(
	struct vm_page *avoid)
{
	int result;

	/* Returns disposable cache before initiating private-page swap or writeback. */
	if (cache_memory_reclaim != NULL && cache_memory_reclaim(PAGE_SIZE) != 0)
		return 0;

	/* A private page is the first choice. */
	result = vm_reclaim_private_one(avoid);
	if (result == 0)
		return 0;
	if (result != EAGAIN)
		return result;

	/*
	 * Object writeback can sleep in VFS I/O.  It must not inherit either the
	 * private-page queue lock or the global cross-VM metadata lock from the
	 * candidate scan above.
	 */
	result = vm_object_reclaim_one();
	if (result != 0)
		return ENOMEM;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	stats.reclaims++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Reports the reclaimed object page. */
	return 0;
}

/*
 * Allocates one page frame for a fault, reclaiming until one is free.
 *
 * A user frame is taken only while the free count is above the reserve,
 * so the page tables and records the same fault needs next can still be
 * allocated.  Below it the fault wakes the page-out worker, reclaims a
 * page itself, and when nothing could be reclaimed sleeps until memory is
 * freed.  It does not give up: commit guarantees the memory.  After a long
 * wait with nothing freed the out-of-memory killer picks a victim.
 */
int
vm_reclaim_frame(
	struct kern_pmem *memory,
	struct vm_page *avoid)
{
	uint64_t started;
	uint64_t now;
	size_t free_pages;
	size_t minimum;
	unsigned long irq;
	uint64_t sequence;
	int error;
	int owned;
	int killed;

	/* A caller inside the metadata lock must not sleep while others wait on it. */
	owned = vm_metadata_owned();
	minimum = vm_free_min_pages();
	started = sched_ticks();

	/* Tries, reclaims and waits until a frame is taken. */
	for (;;) {
		/* Takes a free frame while the reserve stays intact. */
		free_pages = vm_free_pages();
		if (free_pages > minimum || owned) {
			error = vm_private_page_alloc(memory);
			if (error == HAL_OK) {
				if (free_pages < 2U * minimum)
					vm_reclaim_pageout_kick();
				return 0;
			}
		}

		/* Asks the worker to refill and reclaims a page directly. */
		vm_reclaim_pageout_kick();
		error = vm_reclaim_one(avoid);
		if (error == 0)
			continue;

		/* A caller that cannot sleep reports the shortage. */
		if (owned)
			return ENOMEM;

		/* Sleeps until the worker or a free makes room, briefly. */
		irq = spin_lock_irqsave(&pageout_lock);
		sequence = waitq_sequence(&free_waitq);
		(void)waitq_sleep(&free_waitq, &pageout_lock, sequence,
		    sched_ticks() + VM_FREE_WAIT_TICKS, 0);
		spin_unlock_irqrestore(&pageout_lock, irq);

		/* A thread that was killed while waiting stops waiting. */
		killed = signal_kill_pending(thread_current());
		if (killed)
			return ENOMEM;

		/* Kills the largest process when nothing has been freed for long. */
		now = sched_ticks();
		if (now - started < kern_ms_to_ticks((uint64_t)VM_OOM_WAIT_SECONDS * 1000U))
			continue;
		killed = vm_oom_kill();
		if (!killed)
			return ENOMEM;
		started = now;
	}
}

/*
 * Allocates one page frame for file system data, reclaiming private pages.
 *
 * It is vm_reclaim_frame() for a caller inside a file system's write path:
 * the frame is taken only while the free count is above the reserve the
 * faults need, and below it the caller gives back disposable cache or swaps
 * out one private page itself.  It never writes back a file's page, which
 * could need the very inode lock the caller holds, and it never sleeps:
 * when nothing can be reclaimed it reports ENOMEM, having asked the
 * page-out worker to refill.
 */
int
vm_reclaim_frame_private(
	struct kern_pmem *memory)
{
	size_t free_pages;
	size_t minimum;
	size_t released;
	unsigned attempt;
	int error;

	/* The reserve a data frame must leave for the faults. */
	minimum = vm_free_min_pages();

	/* Tries, then reclaims one page, a bounded number of times. */
	for (attempt = 0; attempt < VM_PRIVATE_FRAME_ATTEMPTS; attempt++) {
		/* Takes a free frame while the reserve stays intact. */
		free_pages = vm_free_pages();
		if (free_pages > minimum) {
			error = vm_private_page_alloc(memory);
			if (error == HAL_OK) {
				if (free_pages < 2U * minimum)
					vm_reclaim_pageout_kick();
				return 0;
			}
		}

		/* Asks the worker to refill. */
		vm_reclaim_pageout_kick();

		/* Disposable cache is given back first. */
		released = 0;
		if (cache_memory_reclaim != NULL)
			released = cache_memory_reclaim(PAGE_SIZE);
		if (released != 0)
			continue;

		/* Then one private page is swapped out; none left ends the attempts. */
		error = vm_reclaim_private_one(NULL);
		if (error != 0)
			break;
	}

	/* Nothing could be reclaimed without writing back a file. */
	return ENOMEM;
}

/*
 * Counts a page fault.
 */
void
vm_reclaim_note_fault(
	void)
{
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	stats.faults++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/*
 * Copies the reclaim statistics, classifying the queued pages.
 */
void
vm_reclaim_get_stats(
	struct vm_reclaim_stats *output)
{
	struct vm_private_page *backing;
	struct vm_page *page;
	unsigned long irq;
	unsigned state_flags;
	uint32_t flags;

	/* Ignores a missing result. */
	if (output == NULL)
		return;

	/* Takes the counters as the starting point under the reclaim lock. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	kern_memcpy(output, &stats, sizeof(*output));

	/* Clears the fields this pass classifies for itself. */
	output->anonymous_resident = 0;
	output->file_resident = 0;
	output->wired = 0;
	output->busy = 0;
	output->dirty = 0;
	output->clean = 0;

	/* Classifies every private backing on the reclaim queue. */
	for (backing = page_queue; backing != NULL; backing = backing->queue_next) {
		/* Samples the hardware and software state of this backing. */
		flags = backing_pte_flags(backing);
		irq = spin_lock_irqsave(&backing->state_lock);
		state_flags = backing->flags;
		spin_unlock_irqrestore(&backing->state_lock, irq);

		/* Counts a backing whose page is under I/O. */
		if (state_flags & VM_PAGE_BUSY)
			output->busy++;

		/* Counts a backing that any mapping wires down. */
		for (page = backing->mappings; page != NULL;
		     page = page->private_next) {
			if (page->wire_count != 0) {
				output->wired++;
				break;
			}
		}

		/* Leaves a backing without a resident page unclassified. */
		if (!(state_flags & VM_PAGE_RESIDENT))
			continue;

		/* Attributes the resident page to anonymous or file memory. */
		page = backing->mappings;
		if (page != NULL && page->region->backing == VM_BACKING_ANON)
			output->anonymous_resident++;
		else
			output->file_resident++;

		/* Separates pages that need writing from pages that do not. */
		if ((state_flags & VM_PAGE_DIRTY) || (flags & HAL_SPACE_PAGE_DIRTY))
			output->dirty++;
		else
			output->clean++;
	}

	/* Adds the object store, whose pages are not on this queue. */
	output->file_resident += vm_object_page_count();
	output->resident += vm_object_page_count();

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/*
 * Reads every page of a draining swap source back into memory.
 *
 * A cancel callback, when given, is polled between pages and while
 * waiting for a busy backing.
 */
int
vm_reclaim_drain_swap_source_cancelable(
	unsigned source_id,
	int (*cancel)(void *),
	void *cancel_argument)
{
	struct vm_private_page *selected;
	struct vm_private_page *wait_backing;
	struct vm_private_page *backing;
	struct vm_page *accounting_page;
	struct swap_backend *backend;
	struct swap_source_stats source_stats;
	int scan_error;
	int error;

	/* Rejects a source id out of range. */
	if (source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Pages in one backing per pass until the source is empty. */
	for (;;) {
		selected = NULL;
		wait_backing = NULL;
		accounting_page = NULL;
		scan_error = 0;

		/* Honors a cancellation between passes. */
		if (cancel != NULL) {
			error = cancel(cancel_argument);
			if (error != 0)
				return error;
		}

		/*
		 * Select and pin one target while reverse mappings are stable.  No
		 * VM metadata lock survives physical allocation or backing I/O.
		 */
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		for (backing = swapped_queue; backing != NULL;
		     backing = backing->queue_next) {
			if (!private_page_targets_source(backing, source_id))
				continue;

			/*
			 * A wired page cannot legitimately have been reclaimed to
			 * swap.  Report the broken invariant instead of waiting
			 * forever.
			 */
			if (backing_has_wired_mapping(backing)) {
				scan_error = EIO;
				break;
			}

			if (backing_has_busy_mapping(backing) ||
			    vm_private_page_io_try_acquire(backing) != 0) {
				vm_private_page_ref(backing);
				wait_backing = backing;
				break;
			}

			/*
			 * A fault may have completed between the first observation
			 * and our exclusive acquisition.
			 */
			if (!private_page_targets_source(backing, source_id)) {
				vm_private_page_io_release(backing);
				vm_private_page_put(backing);
				continue;
			}

			if (backing->mappings == NULL)
				HAL_FATAL("tracked swapped backing has no mapping");
			pin_backing_mappings(backing);
			accounting_page = backing->mappings;
			selected = backing;
			break;
		}

		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
		if (scan_error != 0)
			return scan_error;

		/* Pages the selected backing in and goes around again. */
		if (selected != NULL) {
			error = vm_private_page_in_owned(selected, accounting_page);
			drain_mapping_pins_release(selected);
			vm_private_page_io_release(selected);
			vm_private_page_put(selected);
			if (error != 0)
				return error;
			if (cancel != NULL) {
				error = cancel(cancel_argument);
				if (error != 0)
					return error;
			}

			continue;
		}

		/* Waits for a busy backing to become idle. */
		if (wait_backing != NULL) {
			error = vm_private_page_wait_idle_cancelable(wait_backing,
			    cancel, cancel_argument);
			vm_private_page_put(wait_backing);
			if (error != 0 && error != EAGAIN)
				return error;
			sched_yield();
			continue;
		}

		/*
		 * A slot allocator which crossed begin_drain may not have published
		 * its backing token yet.  Source counters close that
		 * scan/publication window; yield until it finishes, then scan
		 * again.
		 */
		backend = swap_system_backend();
		if (backend == NULL)
			return ENXIO;
		error = swap_source_get_stats(backend, source_id, &source_stats);
		if (error != 0)
			return error;
		if (source_stats.state != SWAP_SOURCE_STATE_DRAINING)
			return EBUSY;
		if (source_stats.allocated_slots == 0 &&
		    source_stats.inflight == 0)
			return 0;
		sched_yield();
	}
}

/*
 * Reads every page of a draining swap source back into memory.
 */
int
vm_reclaim_drain_swap_source(
	unsigned source_id)
{
	int error;

	/* Reports why the drain failed. */
	error = vm_reclaim_drain_swap_source_cancelable(source_id, NULL, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reclaims one resident private page, avoiding a given mapping.
 *
 * The victim is chosen by a clock over the reclaim queue: the hand starts
 * at the oldest page, and a page used since the hand last passed has its
 * accessed bit cleared and goes back to the newest end, a second chance.
 * The first page found unused is taken.  The walk is bounded; when it ends
 * without one, the first usable page it passed is taken instead, so memory
 * that was all touched at once is still reclaimed in constant time per
 * page rather than by walking every page twice.
 * Reports EAGAIN when no candidate is available.
 */
int
vm_reclaim_private_one(
	struct vm_page *avoid)
{
	struct vm_private_page *selected;
	struct vm_private_page *fallback;
	struct vm_private_page *previous;
	int file_candidate;
	struct vm_private_page *backing;
	unsigned state_flags;
	unsigned long irq;
	unsigned scanned;
	uint32_t flags;
	int error;
	int busy;

	selected = NULL;
	fallback = NULL;
	file_candidate = 0;

	/* Walks the queue from its oldest end under the reclaim lock. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	/* Moves the hand from the oldest page toward the newest. */
	backing = page_queue_tail;
	for (scanned = 0; backing != NULL && scanned < VM_RECLAIM_SCAN_MAX; scanned++) {
		previous = backing->queue_prev;

		/* Skips a backing that is absent, busy, wired or avoided. */
		irq = spin_lock_irqsave(&backing->state_lock);
		state_flags = backing->flags;
		spin_unlock_irqrestore(&backing->state_lock, irq);
		busy = (state_flags & VM_PAGE_RESIDENT) == 0 ||
		    (state_flags & VM_PAGE_BUSY) != 0;
		if (!busy)
			busy = backing_wired_or_avoided(backing, avoid);
		if (busy) {
			backing = previous;
			continue;
		}

		/* Gives a recently used page a second chance at the newest end. */
		flags = backing_pte_flags(backing);
		if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0) {
			backing_clear_accessed(backing);
			vm_reclaim_queue_remove(backing);
			vm_reclaim_queue_insert(backing);
			if (fallback == NULL)
				fallback = backing;
			backing = previous;
			continue;
		}

		/* Takes the first unused page whose I/O and address spaces it can hold. */
		error = reclaim_take_candidate(backing);
		if (error == 0) {
			selected = backing;
			break;
		}

		/* Moves on past a page another reclaim owns. */
		backing = previous;
	}

	/* Falls back to the first usable page the walk passed over. */
	if (selected == NULL && fallback != NULL) {
		error = reclaim_take_candidate(fallback);
		if (error == 0)
			selected = fallback;
	}

	/* Pins the victim's mappings, then drops the check's extra references. */
	if (selected != NULL) {
		file_candidate = selected->mappings != NULL &&
		    selected->mappings->region->backing == VM_BACKING_FILE;
		pin_backing_mappings(selected);
		backing_release_spaces(selected);
	}

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Reclaims the selected backing outside the locks. */
	if (selected != NULL) {
		error = reclaim_backing_owned(selected, file_candidate);
		return error;
	}

	/* Reports no candidate. */
	return EAGAIN;
}

/*
 * Puts a backing at the head of the reclaim queue; the caller holds the
 * reclaim lock.
 */
void
vm_reclaim_queue_insert(
	struct vm_private_page *backing)
{
	/* Refuses a backing that is already on a list. */
	if (backing->queue_kind != VM_QUEUE_NONE)
		HAL_FATAL("VM private backing is already queued");

	/* Links the backing in front of the old head. */
	backing->queue_prev = NULL;
	backing->queue_next = page_queue;
	if (page_queue != NULL)
		page_queue->queue_prev = backing;
	page_queue = backing;

	/* Records the list, which is how vm_reclaim_queue_remove() knows the head to fix. */
	backing->queue_kind = VM_QUEUE_RECLAIM;

	/* The first backing is also the oldest. */
	if (page_queue_tail == NULL)
		page_queue_tail = backing;
}

/*
 * Puts a backing at the head of the swapped list; the caller holds the
 * reclaim lock.
 */
void
vm_reclaim_swapped_insert(
	struct vm_private_page *backing)
{
	/* Refuses a backing that is already on a list. */
	if (backing->queue_kind != VM_QUEUE_NONE)
		HAL_FATAL("VM private backing is already queued");

	/* Links the backing in front of the old head. */
	backing->queue_prev = NULL;
	backing->queue_next = swapped_queue;
	if (swapped_queue != NULL)
		swapped_queue->queue_prev = backing;
	swapped_queue = backing;

	/* Records the list, which is how vm_reclaim_queue_remove() knows the head to fix. */
	backing->queue_kind = VM_QUEUE_SWAPPED;
}

/*
 * Takes a backing off the reclaim queue; the caller holds the reclaim lock.
 *
 * Each backing knows its neighbours, so this does not walk the queue: a walk
 * from the head made tearing down a process cost its pages times every
 * tracked page of the system (BUG-033).  A backing that is not on the queue
 * is left alone.
 */
void
vm_reclaim_queue_remove(
	struct vm_private_page *backing)
{
	/* A backing on no list is left alone. */
	if (backing->queue_kind == VM_QUEUE_NONE)
		return;

	/* Joins the neighbours around the backing, on whichever list it is. */
	if (backing->queue_prev != NULL)
		backing->queue_prev->queue_next = backing->queue_next;
	else if (backing->queue_kind == VM_QUEUE_RECLAIM)
		page_queue = backing->queue_next;
	else
		swapped_queue = backing->queue_next;
	if (backing->queue_next != NULL)
		backing->queue_next->queue_prev = backing->queue_prev;
	else if (backing->queue_kind == VM_QUEUE_RECLAIM)
		page_queue_tail = backing->queue_prev;

	/* Leaves the backing off the lists. */
	backing->queue_kind = VM_QUEUE_NONE;
	backing->queue_next = NULL;
	backing->queue_prev = NULL;
}

/*
 * Reclaims a few private pages so a fault's backend read has scratch memory.
 */
unsigned
vm_reclaim_object_fault_reserve(
	void)
{
	unsigned reclaimed;

	/*
	 * A filesystem page read may need page-backed scratch storage after
	 * the object frame itself has consumed the page obtained by normal
	 * reclaim.  Reclaim a small bounded reserve only from the lockless
	 * fault-I/O boundary; the current object page is BUSY and therefore
	 * cannot be selected.
	 */
	for (reclaimed = 0; reclaimed < VM_OBJECT_FAULT_RESERVE_PAGES; reclaimed++) {
		if (vm_reclaim_private_one(NULL) != 0)
			break;
	}

	return reclaimed;
}

/* Reports the protection a mapping should carry, read-only when copy-on-write. */
static uint32_t
mapping_prot(
	const struct vm_page *page)
{
	uint32_t prot;

	prot = page->region->prot;

	if (page->flags & VM_MAPPING_COW)
		prot &= ~HAL_SPACE_WRITE;

	return prot;
}

/* Tests whether any mapping is wired, busy, or the one to avoid. */
static int
backing_wired_or_avoided(
	struct vm_private_page *backing,
	struct vm_page *avoid)
{
	struct vm_page *page;

	/* Looks for a mapping that reclaim must not take. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (page == avoid ||
		    page->wire_count != 0 ||
		    (page->flags & VM_MAPPING_BUSY) != 0)
			return 1;
	}

	/* Reports a backing every mapping allows. */
	return 0;
}

/* Tests whether any mapping is wired. */
static int
backing_has_wired_mapping(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (page->wire_count != 0)
			return 1;
	}

	return 0;
}

/* Tests whether any mapping is busy in a fault or reclaim. */
static int
backing_has_busy_mapping(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_BUSY) != 0)
			return 1;
	}

	return 0;
}

/*
 * Takes a reclaim candidate: holds every address space that maps it, then
 * its I/O ownership.  The caller holds the metadata and reclaim locks.
 *
 * An address space whose last reference is gone is being torn down; its
 * pages stay on the reclaim queue until the teardown untracks them, and
 * pinning one would take a reference on a dead space.  Such a candidate
 * is refused.  Reports 0 when the candidate is taken, holding one extra
 * reference per mapping for the caller to drop after pinning.
 */
static int
reclaim_take_candidate(
	struct vm_private_page *backing)
{
	int held;
	int error;

	/* Refuses a page of an address space that is going away. */
	held = backing_hold_spaces(backing);
	if (!held)
		return EBUSY;

	/* Takes the I/O ownership, or lets the spaces go again. */
	error = vm_private_page_io_try_acquire(backing);
	if (error != 0) {
		backing_release_spaces(backing);
		return error;
	}

	/* Reports the taken candidate. */
	return 0;
}

/*
 * Holds every address space that maps a backing; the caller holds the
 * metadata and reclaim locks.  Reports 0, holding nothing, when one of
 * them has already lost its last reference.
 */
static int
backing_hold_spaces(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	struct vm_page *undo;
	int acquired;

	/* Tries each mapping's address space in turn. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		acquired = vmspace_tryref(page->vm);
		if (acquired)
			continue;

		/* Drops what was taken before the dead space. */
		for (undo = backing->mappings; undo != page; undo = undo->private_next)
			vmspace_put_deferred(undo->vm);
		return 0;
	}

	/* Reports every space held. */
	return 1;
}

/* Drops the references backing_hold_spaces() took. */
static void
backing_release_spaces(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	/* Releases one reference per mapping, never the last. */
	for (page = backing->mappings; page != NULL; page = page->private_next)
		vmspace_put_deferred(page->vm);
}

/* Clears the accessed PTE flag of every mapping of a backing. */
static void
backing_clear_accessed(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	/* Clears the flag in each live mapping; a failure only leaves it set. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (!(page->flags & VM_MAPPING_MAPPED))
			continue;
		(void)hal_space_clear_flags(page->vm->space, (void *)page->address,
		    HAL_SPACE_PAGE_ACCESSED);
	}
}

/* Combines the accessed and dirty PTE flags of every mapping. */
static uint32_t
backing_pte_flags(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	uint32_t combined;
	uint32_t flags;

	/* Ors the hardware flags of every live mapping of this backing. */
	combined = 0;
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (!(page->flags & VM_MAPPING_MAPPED))
			continue;
		flags = 0;
		if (hal_space_query(page->vm->space, (void *)page->address,
		    &flags) == HAL_OK)
			combined |= flags;
	}

	/* Reports the combined flags. */
	return combined;
}

/* Marks every mapping busy and holds its region; called with the metadata and reclaim locks held. */
static void
pin_backing_mappings(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	/* Pins every mapping and the address space that holds it. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		vmspace_ref(page->vm);
		mutex_lock(&page->vm->lock);

		/*
		 * Traps on a mapping that cannot be pinned: one a pass
		 * already owns, or one whose region hold count would wrap.
		 */
		if ((page->flags & VM_MAPPING_BUSY) != 0 ||
		    page->region->hold_count == (unsigned)-1)
			HAL_FATAL("VM reclaim mapping pin invariant failed");

		/*
		 * BUSY marks the mapping as owned by this reclaim pass, so a
		 * fault on it waits instead of racing.  The region hold keeps
		 * the address space from retiring it underneath.
		 */
		page->flags |= VM_MAPPING_BUSY;
		page->region->hold_count++;

		mutex_unlock(&page->vm->lock);
	}
}

/* Undoes pin_backing_mappings(), publishing unmapped PTEs; called with the metadata and reclaim locks held. */
static void
unpin_backing_mappings(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	struct vmspace *vm;

	/* Releases the pin this reclaim pass took on every mapping. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		/* Retires the reclaim state the pass left on the mapping. */
		vm = page->vm;
		mutex_lock(&vm->lock);

		/*
		 * A mapping this pass unmapped is published as no longer
		 * mapped, so the next fault reads it in again.
		 */
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
			    VM_MAPPING_RECLAIM_PROTECTED | VM_MAPPING_MAPPED);
		}

		/*
		 * Drops the write protection the pass installed, and then the
		 * ownership itself, which lets a waiting fault proceed.
		 */
		page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
		page->flags &= ~VM_MAPPING_BUSY;

		/* Releases the region and wakes the faults that waited. */
		if (page->region->hold_count == 0)
			HAL_FATAL("VM reclaim region hold underflow");

		/* Releases the hold that kept the region from retiring. */
		page->region->hold_count--;

		/*
		 * Advances the address space's generation so a fault that
		 * sampled it before this change knows to look again.  Zero is
		 * skipped, because it is the never-sampled value.
		 */
		vm->generation++;
		if (vm->generation == 0)
			vm->generation++;

		vmspace_fault_wake_locked(vm);

		mutex_unlock(&vm->lock);

		/* Never run final vmspace destruction under either VM metadata lock. */
		vmspace_put_deferred(vm);
	}
}

/* Revokes and unmaps every PTE of an owned backing, collecting the dirty flags. */
static int
unmap_backing_ptes(
	struct vm_private_page *backing,
	uint32_t *pte_flags)
{
	struct vm_page *page;
	int error;
	uint32_t flags;
	uint32_t readonly;

	error = 0;

	/* Rejects a missing result. */
	if (pte_flags == NULL)
		return EINVAL;
	*pte_flags = 0;

	/*
	 * A pre-unmap query is not a stable dirty snapshot: userspace can write
	 * between the query and shootdown.  First make every mapping read-only
	 * and wait for that protection change, then collect flags and remove
	 * the PTEs.
	 */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		flags = 0;

		if ((page->flags & VM_MAPPING_MAPPED) == 0)
			continue;

		readonly = mapping_prot(page) & ~HAL_SPACE_WRITE;
		if (readonly == 0) {
			/*
			 * No non-writable representation exists for a write-only
			 * PTE.  Synchronous unmap is the revoke; conservatively
			 * write it back.
			 */
			if (hal_space_unmap(page->vm->space,
			    (void *)page->address, PAGE_SIZE) != HAL_OK) {
				error = EIO;
				break;
			}

			page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
			*pte_flags |= HAL_SPACE_PAGE_DIRTY;
			continue;
		}

		if (hal_space_prot_query(page->vm->space,
		    (void *)page->address, PAGE_SIZE, readonly, &flags) != HAL_OK) {
			error = EIO;
			break;
		}

		page->flags |= VM_MAPPING_RECLAIM_PROTECTED;

		*pte_flags |= flags;
	}

	if (error != 0)
		goto rollback;

	/* Removes the protected PTEs. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_MAPPED) == 0 ||
		    (page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0)
			continue;

		if (hal_space_unmap(page->vm->space, (void *)page->address, PAGE_SIZE) != HAL_OK) {
			error = EIO;
			break;
		}

		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}

	if (error == 0)
		return 0;

rollback:
	/* Roll back outside all VM locks.  A failed remap remains lazy. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			if (hal_space_map(page->vm->space, (void *)page->address,
			    backing->pmem.paddr, PAGE_SIZE,
			    mapping_prot(page)) == HAL_OK)
				page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
				    VM_MAPPING_RECLAIM_PROTECTED);
			continue;
		}

		if ((page->flags & VM_MAPPING_RECLAIM_PROTECTED) == 0)
			continue;

		if (hal_space_prot(page->vm->space, (void *)page->address,
		    PAGE_SIZE, mapping_prot(page)) == HAL_OK) {
			page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
			continue;
		}

		/* A stale read-only PTE would livelock a later write fault. */
		if (hal_space_unmap(page->vm->space, (void *)page->address, PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM reclaim protection rollback failed");

		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Restores the PTEs of a backing whose reclaim was abandoned. */
static void
rollback_backing_ptes(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		/* A failed remap remains lazy until the next fault. */
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			if (hal_space_map(page->vm->space, (void *)page->address,
			    backing->pmem.paddr, PAGE_SIZE,
			    mapping_prot(page)) == HAL_OK)
				page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
				    VM_MAPPING_RECLAIM_PROTECTED);
			continue;
		}

		if ((page->flags & VM_MAPPING_RECLAIM_PROTECTED) == 0)
			continue;

		/* Restores the protection reclaim took away from this mapping. */
		if (hal_space_prot(page->vm->space,
				  (void *)page->address,
				  PAGE_SIZE,
				  mapping_prot(page)) == HAL_OK) {
			page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
			continue;
		}

		/* A stale read-only PTE is removed rather than left behind. */
		if (hal_space_unmap(page->vm->space,
				   (void *)page->address,
				   PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM reclaim PTE rollback failed");

		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}
}

/* Writes an owned, unmapped backing to swap and frees its page. */
static int
swap_out_backing_owned(
	struct vm_private_page *backing)
{
	struct swap_backend *backend;
	struct kern_pmem memory;
	uint32_t slot;
	unsigned long irq;
	int slot_allocated;
	int error;

	backend = swap_system_backend();
	slot_allocated = 0;

	/* Allocates a slot and writes the page. */
	if (PAGE_SIZE != SWAP_PAGE_SIZE || backend == NULL) {
		error = ENOSPC;
	} else {
		error = swap_alloc_slot(backend, &slot);
		if (error == 0)
			slot_allocated = 1;
	}

	if (error == 0) {
		error = swap_write_page(backend,
					slot,
					(const void *)hal_pmem_to_kernel(backing->pmem.paddr));
	}

	/* A failure restores the PTEs and releases everything. */
	if (error != 0) {
		rollback_backing_ptes(backing);

		if (slot_allocated)
			swap_free_slot(backend, slot);

		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		unpin_backing_mappings(backing);

		if (error != ENOSPC)
			stats.io_errors++;

		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();

		vm_private_page_io_release(backing);
		vm_private_page_put(backing);

		return error;
	}

	/* PTE shootdown and swap I/O are complete before the state is published. */
	memory = backing->pmem;
	(void)hal_pmem_free(&memory.paddr, memory.size);

	/* Publishes the backing as swapped out rather than resident. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	irq = spin_lock_irqsave(&backing->state_lock);

	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) != (VM_PAGE_BUSY | VM_PAGE_RESIDENT) ||
	    (backing->flags & VM_PAGE_SWAPPED) != 0)
		HAL_FATAL("VM swap-out state changed under I/O owner");

	kern_memset(&backing->pmem, 0, sizeof(backing->pmem));
	backing->swap_slot = slot;
	backing->flags &= ~(VM_PAGE_RESIDENT | VM_PAGE_DIRTY);
	backing->flags |= VM_PAGE_SWAPPED;

	vm_reclaim_private_page_advance_locked(backing);

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Releases the mappings and moves the page between the counters. */
	unpin_backing_mappings(backing);

	/* Moves the backing from the reclaim queue to the swapped list. */
	vm_reclaim_queue_remove(backing);
	vm_reclaim_swapped_insert(backing);

	if (stats.resident)
		stats.resident--;

	stats.swapped++;
	stats.page_outs++;
	stats.reclaims++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Releases the ownership this reclaim pass held. */
	vm_private_page_io_release(backing);
	vm_private_page_put(backing);

	/* Reports the swapped-out backing. */
	return 0;
}

/* Discards a clean file-backed page, removing its mappings from their regions. */
static int
discard_file_backing_owned(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	struct vm_page *next;
	struct kern_pmem memory;
	unsigned long irq;
	struct vm_page **link;
	struct vmspace *vm;

	/* Detaches every mapping from its region and frees it. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	for (page = backing->mappings; page != NULL; page = next) {
		/* Splices the page out of its region. */
		vm = page->vm;
		next = page->private_next;
		mutex_lock(&vm->lock);

		link = &page->region->pages;
		while (*link != NULL && *link != page)
			link = &(*link)->next;
		if (*link != page)
			HAL_FATAL("discarded VM page left its region");
		*link = page->next;

		/* Releases the region and wakes the faults that waited. */
		if (page->region->hold_count == 0)
			HAL_FATAL("VM discard region hold underflow");

		page->region->hold_count--;

		vm->generation++;
		if (vm->generation == 0)
			vm->generation++;

		vmspace_fault_wake_locked(vm);

		mutex_unlock(&vm->lock);

		/* Frees the mapping and its share of the backing. */
		page->private_page = NULL;
		page->private_next = NULL;

		if (refcount_put(&backing->refs))
			HAL_FATAL("VM discard lost I/O lifetime hold");

		vm_page_free_metadata(page);
		vmspace_put_deferred(vm);
	}

	/* Takes the backing off the queue and drops its page. */
	backing->mappings = NULL;
	vm_reclaim_queue_remove(backing);
	irq = spin_lock_irqsave(&backing->state_lock);

	if (backing->mapping_count == 0 ||
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_TRACKED))
		HAL_FATAL("invalid VM file-discard backing state");

	backing->mapping_count = 0;
	backing->flags &= ~(VM_PAGE_TRACKED | VM_PAGE_RESIDENT | VM_PAGE_DIRTY);

	memory = backing->pmem;

	kern_memset(&backing->pmem, 0, sizeof(backing->pmem));

	vm_reclaim_private_page_advance_locked(backing);

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Discharges the page from the reclaim counters. */
	if (stats.resident)
		stats.resident--;

	stats.reclaims++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Frees the memory and releases the ownership this pass held. */
	(void)hal_pmem_free(&memory.paddr, memory.size);
	vm_private_page_io_release(backing);
	vm_private_page_put(backing);

	/* Reports the discarded backing. */
	return 0;
}

/* Tests whether a backing's swap slot lies in a source. */
static int
private_page_targets_source(
	struct vm_private_page *backing,
	unsigned source_id)
{
	uint32_t slot;
	unsigned decoded_source;
	unsigned flags;
	unsigned long irq;

	/* Samples the swap state. */
	irq = spin_lock_irqsave(&backing->state_lock);

	flags = backing->flags;
	slot = backing->swap_slot;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports no match for a page that is not on swap at all. */
	if ((flags & VM_PAGE_SWAPPED) == 0)
		return 0;

	/* Reports no match for a page that holds no slot. */
	if (slot == SWAP_SLOT_NONE)
		return 0;

	/* Reports no match for a slot that does not decode. */
	if (swap_slot_decode(slot, &decoded_source, NULL) != 0)
		return 0;

	/* Reports no match for a slot on some other source. */
	if (decoded_source != source_id)
		return 0;

	/* Succeeded: the page is on the source the caller named. */
	return 1;
}

/* Unpins a drained backing's mappings under the locks. */
static void
drain_mapping_pins_release(
	struct vm_private_page *backing)
{
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	unpin_backing_mappings(backing);

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/* Reclaims an owned, pinned backing by discard or swap-out. */
static int
reclaim_backing_owned(
	struct vm_private_page *backing,
	int file_candidate)
{
	uint32_t pte_flags;
	unsigned state_flags;
	unsigned long irq;
	int error;

	/* Revokes the mappings, giving up on a shootdown failure. */
	error = unmap_backing_ptes(backing, &pte_flags);
	if (error != 0) {
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);

		unpin_backing_mappings(backing);
		stats.io_errors++;

		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();

		/*
		 * Gives up the I/O ownership and the reference this pass
		 * held, leaving the backing exactly as it was found.
		 */
		vm_private_page_io_release(backing);
		vm_private_page_put(backing);

		/* Reports the shootdown failure. */
		return error;
	}

	/* Samples the state that decides between discard and swap-out. */
	irq = spin_lock_irqsave(&backing->state_lock);

	state_flags = backing->flags;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/*
	 * A file page neither the software nor the hardware recorded as
	 * written still matches the file, so it is discarded rather than
	 * written to swap.
	 */
	if (file_candidate &&
	    (state_flags & VM_PAGE_DIRTY) == 0 &&
	    (pte_flags & HAL_SPACE_PAGE_DIRTY) == 0) {
		/* Reports why the page could not be discarded. */
		error = discard_file_backing_owned(backing);
		if (error != 0)
			return error;

		/* Succeeded: the page was dropped without any write. */
		return 0;
	}

	/* Anything else keeps its only copy on swap. */
	error = swap_out_backing_owned(backing);
	if (error != 0)
		return error;

	/* Succeeded: the page now lives on swap. */
	return 0;
}
