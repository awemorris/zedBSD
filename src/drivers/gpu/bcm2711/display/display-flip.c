/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Display lists made from the recorded plane state.
 *
 * Every change of a plane (the primary frame, the cursor) is recorded under
 * the guard with a new state number and a list made from the whole state is
 * published at once, without waiting: into a free slot when no list is
 * waiting for the hardware, over the waiting list when one is (the hardware
 * takes one complete list or the other), and otherwise at the next adoption,
 * which then carries every change made meanwhile.  The selected pixelvalve's
 * vblank IRQ records which list the hardware took and wakes the waiters.
 * Borrowed images are let go only in thread context, once a list made after
 * their replacement is adopted.
 */
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/pmem.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"

/* The boot console list R0 left in SRAM, and the empty pointer of the old submitter state. */
#define FLIP_CONSOLE_LIST 43U

/*
 * The three private list slots: 256 words each from SRAM word 256 to 1023,
 * clear of the firmware's words, the resident filter and the console list.
 */
#define FLIP_FIRST_SLOT_LIST 256U
#define FLIP_SLOT_WORDS 256U

/* One unscaled plane is eight words; a list ends with one terminator word. */
#define FLIP_PLANE_WORDS 8U
#define FLIP_LIST_END 0x80000000U

/*
 * A scaled overlay's plane: nine words, the line buffer's start, at most
 * five words of horizontal and vertical scaling, and four filter places.
 */
#define FLIP_SCALED_MAX_WORDS 19U

/* The longest list: console, primary, the scaled overlays, cursor and the terminator. */
#define FLIP_LIST_MAX_WORDS (3U * FLIP_PLANE_WORDS + BCM2711_FLIP_OVERLAYS * FLIP_SCALED_MAX_WORDS + 1U)

/* How an overlay is scaled along one axis: not at all, by the polyphase filter, or by the trapezoidal one. */
#define FLIP_SCALE_NONE 0U
#define FLIP_SCALE_PPF 1U
#define FLIP_SCALE_TPZ 2U

/*
 * The resident scaling filter's first SRAM word (R0 writes it there for
 * good, display-program.c), which a polyphase-scaled plane points at.
 */
#define FLIP_FILTER_WORD 32U

/*
 * The line buffer memory each overlay owns for vertical scaling: a fixed
 * part of the HVS's 60K words, enough for a polyphase-scaled line of 2048
 * pixels, so an overlay scaled vertically reads at most 2048 pixels a line.
 */
#define FLIP_LINE_BUFFER_WORDS 8192U
#define FLIP_LINE_BUFFER_PIXELS 2048U

/* How long a recorded state may wait for the hardware to adopt a list showing it. */
#define FLIP_ADOPTION_TICKS kern_ms_to_ticks(100U)

/*
 * The bytes per second the HVS may read for one output's planes: a
 * provisional budget until the Pi measurement (hvs-load) sets it.
 */
#define FLIP_BANDWIDTH_BUDGET 1500000000ULL

/* How many times a record raises the clock again after a restore lowered it meanwhile. */
#define FLIP_CLOCK_ATTEMPTS 3U

static int check_primary(const struct bcm2711_display *display, const struct bcm2711_flip_primary *primary);
static int encode_plane(const struct drv_bcm2711_boot_screen *extent, const struct drv_bcm2711_boot_screen *frame, uint32_t *words);
static bool console_hidden(const struct bcm2711_display *display, const struct bcm2711_flip_primary *primary);
static uint32_t required_clock(const struct bcm2711_display *display, const struct bcm2711_flip_primary *primary, const struct bcm2711_flip_overlay *overlays);
static uint64_t read_bandwidth(const struct bcm2711_display *display, const struct bcm2711_flip_primary *primary, const struct bcm2711_flip_overlay *overlays);
static int view_changes_locked(struct bcm2711_display *display, struct bcm2711_flip_change *changes, uint32_t count, struct bcm2711_flip_primary *primary, struct bcm2711_flip_overlay *overlays, uint32_t *retired);
static int check_overlay(const struct bcm2711_display *display, const struct bcm2711_flip_overlay *overlay);
static void apply_changes_locked(struct bcm2711_display *display, struct bcm2711_flip_change *changes, uint32_t count, const struct bcm2711_flip_primary *primary, const struct bcm2711_flip_overlay *overlays);
static uint32_t overlay_words(const struct bcm2711_flip_overlay *overlay, uint32_t index, uint32_t *words);
static unsigned scale_mode(uint32_t source, uint32_t destination);
static uint32_t polyphase_word(uint32_t source, uint32_t destination);
static void trapezoid_words(uint32_t source, uint32_t destination, uint32_t *words);
static bool overlay_scaled(const struct bcm2711_flip_overlay *overlay);
static void note_locked(struct bcm2711_display *display, unsigned kind, uint64_t now);
static void count_adoption_locked(struct bcm2711_display *display, uint32_t slot);
static uint32_t take_times_locked(struct bcm2711_display *display, uint32_t *replaced);
static void report_times(uint32_t count, uint32_t replaced);
static int raise_clock(struct bcm2711_display *display, uint32_t required);
static void lower_clock(struct bcm2711_display *display, uint32_t hz);
static int request_clock(uint32_t hz);
static int check_pipeline(struct bcm2711_display *display, bool recovering);
static bool frames_overlap(const struct drv_bcm2711_boot_screen *a, const struct drv_bcm2711_boot_screen *b);
static bool range_overlaps(const struct drv_bcm2711_boot_screen *frame, uint64_t physical, uint64_t bytes);
static int owner_matches_locked(const struct bcm2711_display *display, const struct bcm2711_flip_owner *owner);
static int publish_locked(struct bcm2711_display *display);
static uint32_t write_list_locked(struct bcm2711_display *display, uint32_t slot);
static void publish_list(struct bcm2711_display *display, uint32_t list, bool fresh);
static uint32_t slot_list(uint32_t slot);
static uint32_t cursor_words(const struct bcm2711_display *display, uint32_t *words);
static void adopt_slot_locked(struct bcm2711_display *display, uint32_t index, uint32_t current);
static void enable_underrun_locked(struct bcm2711_display *display);
static int wait_console(struct bcm2711_display *display);
static bool perf_enabled(struct bcm2711_display *display);
static void report_cadence(uint64_t milliseconds);
static void cadence_summary(struct bcm2711_flip_cadence *cadence, uint64_t milliseconds, uint64_t summary[4]);

/* The window the note that ends it copies and prints, owned by the one caller that set cadence_reporting. */
static struct bcm2711_flip_cadence cadence_copy[BCM2711_FLIP_CADENCE_KINDS];

/* The settled overlay record times the report that took them prints, owned by the caller that set perf_reporting. */
static struct bcm2711_flip_time times_copy[BCM2711_FLIP_TIMES];

/*
 * Initializes persistent IRQ state before installing source callback owners.
 * The enclosing display is zeroed at discovery and outlives all submissions.
 */
void
bcm2711_display_flip_init(
	struct bcm2711_display *display)
{
	int error;

	/* Initializes once, so repeated IRQ preparation cannot release DMA holds. */
	if (display->flip.initialized)
		return;

	/* The clock requests sleep, so they have a mutex of their own; without it nothing attaches. */
	error = mutex_init(&display->flip.clock_mutex, LOCK_RANK_DEVICE, "bcm2711-display-clock");
	if (error != 0)
		return;

	/* The guard, the adoption waiters and an adopted console list with nothing pending. */
	spin_init(&display->flip.guard, LOCK_RANK_DEVICE, "bcm2711-display");
	waitq_init(&display->flip.adoption_waitq, "bcm2711-display-adoption");
	display->flip.active_list = FLIP_CONSOLE_LIST;
	display->flip.active_slot = BCM2711_FLIP_NO_SLOT;
	display->flip.initialized = true;

	/* Succeeded: the private flip state starts with no pending publication. */
	return;
}

/*
 * Attaches flip ownership to the already running, selected boot output.
 * No display register or framebuffer byte is changed by attachment.
 */
int
bcm2711_display_flip_attach(
	struct bcm2711_display *display)
{
	unsigned long enabled;
	int error;

	/* Refuses attachment until R0's actual IRQ/list completion was verified. */
	if (!display->flip.initialized || !display->scanout_started)
		return ENODEV;
	if (display->port >= BCM2711_TIMING_COUNT || display->channel != 0)
		return EINVAL;

	/* The register window reaches past the last word of the third list slot. */
	if (display->compositor.mapped == NULL || display->compositor.size < 0x5000U)
		return ENODEV;
	if (display->timing[display->port].mapped == NULL || display->timing[display->port].size < 0x2cU)
		return ENODEV;

	/* Claims the boot primary only when no competing display state is present. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	if (display->flip.attached) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return EBUSY;
	}

	/* Confirms exclusive use of the established channel and both list pointers. */
	error = check_pipeline(display, false);
	if (error != 0) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return error;
	}

	/* Publishes permission to submit only after the hardware ownership check. */
	display->flip.attached = true;
	display->flip.core_hz = display->console_core_hz;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the caller may submit buffers within the unchanged boot mode. */
	return 0;
}

/*
 * Detaches a registration attempt that never left the confirmed console list.
 * No live or uncertain DMA buffer can be released through this operation.
 */
int
bcm2711_display_flip_detach(
	struct bcm2711_display *display)
{
	unsigned long enabled;
	int error;

	/* Requires the persistent initialized guard before observing attachment. */
	if (!display->flip.initialized)
		return ENODEV;

	/* Withdraws ownership only when no record, recovery or borrowed DMA exists. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	if (display->flip.uncertain ||
	    display->flip.restoring ||
	    display->flip.primary.hold != NULL ||
	    display->flip.retiring_count != 0) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return EBUSY;
	}

	/* A current console pointer alone cannot prove no pending list will replace it. */
	error = check_pipeline(display, false);
	if (error != 0 || display->flip.active_list != FLIP_CONSOLE_LIST) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return EBUSY;
	}

	/* No hardware reference is withdrawn; the console keeps its existing list. */
	display->flip.attached = false;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: an unpublished device owner may unwind its private buffers. */
	return 0;
}

/*
 * Gives the planes to a new lease.
 *
 * The caller holds the display device's controller mutex and has restored
 * the console, so no list names anything; the cursor starts hidden.
 */
void
bcm2711_display_flip_own(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner)
{
	unsigned long enabled;

	/* Later records are checked against this copy, without the controller mutex. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	display->flip.owner = owner->session;
	display->flip.lease = owner->lease;
	display->flip.revoking = false;
	kern_memset(&display->flip.cursor, 0, sizeof(display->flip.cursor));

	/* The lease's planes start hidden, with serials counted afresh. */
	kern_memset(display->flip.overlays, 0, sizeof(display->flip.overlays));
	kern_memset(display->flip.plane_serials, 0, sizeof(display->flip.plane_serials));

	spin_unlock_irqrestore(&display->flip.guard, enabled);
}

/*
 * Starts ending a lease before its owner takes the controller mutex.
 *
 * From now on the lease's records fail with EBUSY, and every waiter wakes
 * and returns EBUSY, so the console restore does not wait behind them.
 */
int
bcm2711_display_flip_revoke(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner)
{
	unsigned long enabled;

	/* Only the open holding the lease ends it. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	if (owner->session == NULL ||
	    display->flip.owner != owner->session ||
	    display->flip.lease != owner->lease) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return EINVAL;
	}

	/*
	 * revoking ends the lease for every record and waiter at once; it stays
	 * set until the planes go back to the driver, so a failed restore leaves
	 * the lease unable to show anything until its release is retried.
	 */
	display->flip.revoking = true;
	waitq_wake_all(&display->flip.adoption_waitq);

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the lease is ending. */
	return 0;
}

/* Gives the planes back to the driver once the console list is adopted. */
void
bcm2711_display_flip_disown(
	struct bcm2711_display *display)
{
	unsigned long enabled;

	/* No open owns the planes; the driver's own records may use them again. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	display->flip.owner = NULL;
	display->flip.lease = 0;
	display->flip.revoking = false;

	spin_unlock_irqrestore(&display->flip.guard, enabled);
}

/* Tells whether an owner may change the planes now. */
int
bcm2711_display_flip_owned(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner)
{
	unsigned long enabled;
	int error;

	/* Nothing owns the planes before the guard exists. */
	if (!display->flip.initialized)
		return ENODEV;

	/* The lease copy is read under the guard, where records check it too. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	error = owner_matches_locked(display, owner);

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Reports another open's lease, an ending one, or none. */
	if (error != 0)
		return error;

	/* Succeeded: the owner may record. */
	return 0;
}

/*
 * Records the primary plane and publishes a list showing it, without waiting.
 *
 * The clock is raised and the caches cleaned first, in this thread; the state
 * is written and the list published under the guard.  The replaced primary's
 * borrowed image waits for the adoption of a list made after this record.
 */
int
bcm2711_display_flip_record(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner,
	const struct bcm2711_flip_primary *primary,
	uint64_t *serial)
{
	struct bcm2711_flip_overlay overlays[BCM2711_FLIP_OVERLAYS];
	unsigned long enabled;
	uint32_t required;
	uint32_t attempt;
	uint64_t mark;
	uint64_t raise_ticks;
	uint64_t clean_ticks;
	void *mapping;
	void *console_mapping;
	bool console_shown;
	int error;

	/* Rejects unrepresentable geometry before touching ownership or hardware. */
	*serial = 0;
	if (!display->flip.initialized)
		return ENODEV;
	error = check_primary(display, primary);
	if (error != 0)
		return error;

	/* An ending or foreign lease does no clock or cache work; the guard checks again below. */
	error = bcm2711_display_flip_owned(display, owner);
	if (error != 0)
		return error;

	/* The frame and, when it shows through, the console are cleaned from this thread. */
	mapping = kern_pmem_to_kernel(primary->frame.physical);
	if (mapping == NULL)
		return EFAULT;
	console_shown = !console_hidden(display, primary);
	console_mapping = NULL;
	if (console_shown) {
		/* The lower console may have received newer cached text since R0. */
		console_mapping = kern_pmem_to_kernel(display->screen.physical);
		if (console_mapping == NULL)
			return EFAULT;
	}

	/* Images replaced by adopted lists go first, so the waiting room has space. */
	bcm2711_display_flip_reap(display);

	/* The shown overlays stay in the list with the new primary. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	kern_memcpy(overlays, display->flip.overlays, sizeof(overlays));

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Computes the clock the new list needs and the cache work outside the guard. */
	required = required_clock(display, primary, overlays);
	raise_ticks = 0;
	clean_ticks = 0;
	for (attempt = 0; attempt < FLIP_CLOCK_ATTEMPTS; attempt++) {
		/* Raises the clock before any list that needs it is written. */
		mark = clock_ticks();
		error = raise_clock(display, required);
		raise_ticks += clock_ticks() - mark;
		if (error != 0)
			return error;

		/* Publishes the console and frame bytes before the list can name them. */
		mark = clock_ticks();
		if (console_mapping != NULL)
			kern_dcache_clean_range(console_mapping, (size_t)display->screen.size);
		if (!primary->frame.uncached)
			kern_dcache_clean_range(mapping, (size_t)primary->frame.size);
		kern_io_write_barrier();
		clean_ticks += clock_ticks() - mark;

		/* Checks ownership and the hardware, then records the plane. */
		enabled = spin_lock_irqsave(&display->flip.guard);

		error = owner_matches_locked(display, owner);
		if (error == 0 && !display->flip.attached)
			error = ENODEV;
		if (error == 0 && display->flip.restoring)
			error = EBUSY;
		if (error == 0 && display->flip.uncertain)
			error = EBUSY;
		if (error != 0) {
			spin_unlock_irqrestore(&display->flip.guard, enabled);
			return error;
		}

		/* A restore lowered the clock after it was raised: raise it again. */
		if (display->flip.core_hz < required) {
			spin_unlock_irqrestore(&display->flip.guard, enabled);
			continue;
		}

		/* The replaced image needs a place among those waiting for adoption. */
		if (display->flip.primary.hold != NULL && display->flip.retiring_count >= BCM2711_FLIP_RETIRING) {
			spin_unlock_irqrestore(&display->flip.guard, enabled);
			return EBUSY;
		}

		/* A list goes out now unless two are out: the hardware must still be ours. */
		if (display->flip.pending_count < 2U && !display->flip.console_pending) {
			error = check_pipeline(display, false);
			if (error != 0) {
				display->flip.uncertain = true;
				waitq_wake_all(&display->flip.adoption_waitq);
				spin_unlock_irqrestore(&display->flip.guard, enabled);
				return error;
			}
		}

		/*
		 * The record's number is the state's next one; the replaced image
		 * waits for the adoption of a list made from this number or later.
		 */
		display->flip.state_serial++;
		if (display->flip.primary.hold != NULL) {
			display->flip.retiring[display->flip.retiring_count].hold = display->flip.primary.hold;
			display->flip.retiring[display->flip.retiring_count].serial = display->flip.state_serial;
			display->flip.retiring_count++;
		}

		/* The new primary is the recorded one, with the caller's reference. */
		display->flip.primary = *primary;
		display->flip.primary.shown = true;
		display->flip.plane_records[0] = display->flip.state_serial;
		display->flip.planes_generation++;
		display->flip.perf.primary_records++;
		display->flip.perf.clean += clean_ticks;
		display->flip.perf.raise += raise_ticks;
		*serial = display->flip.state_serial;

		/* The checked pipeline takes the list now, or the next adoption carries it. */
		(void)publish_locked(display);

		spin_unlock_irqrestore(&display->flip.guard, enabled);

		/* Succeeded: the frame shows from the refresh boundary that adopts a list with it. */
		return 0;
	}

	/* The clock kept going down under this record. */
	return EBUSY;
}

/*
 * Waits for the adoption of a list made from the record numbered serial or later.
 *
 * The wait holds no lock while it sleeps; the vblank IRQ wakes it.  A
 * timeout leaves the state uncertain: every borrowed image stays until the
 * console is restored, and the lease's later records fail.
 */
int
bcm2711_display_flip_wait(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner,
	uint64_t serial)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	int error;

	/* Nothing was recorded before the guard exists. */
	if (!display->flip.initialized)
		return ENODEV;

	/* Sleeps until the adoption, the lease's end, uncertainty or the deadline. */
	deadline = sched_ticks() + FLIP_ADOPTION_TICKS;
	enabled = spin_lock_irqsave(&display->flip.guard);

	for (;;) {
		/* A list showing this record, or a later one, was adopted. */
		if (display->flip.adopted_serial >= serial) {
			error = 0;
			break;
		}

		/* The lease ended, or another open owns the planes now. */
		error = owner_matches_locked(display, owner);
		if (error != 0)
			break;

		/* Another waiter's timeout left every list uncertain. */
		if (display->flip.uncertain) {
			error = ETIMEDOUT;
			break;
		}

		/* The deadline passed: the hardware did not take the list. */
		now = sched_ticks();
		if (now >= deadline) {
			display->flip.uncertain = true;
			waitq_wake_all(&display->flip.adoption_waitq);
			error = ETIMEDOUT;
			break;
		}

		/* Sleeps until the IRQ or a revocation wakes the queue, or until the deadline. */
		observed = waitq_sequence(&display->flip.adoption_waitq);
		(void)waitq_sleep(&display->flip.adoption_waitq, &display->flip.guard, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Images the adoption no longer shows go now, in this thread. */
	bcm2711_display_flip_reap(display);

	/* Reports the timeout, uncertainty or the lease's end. */
	if (error != 0)
		return error;

	/* Succeeded: the hardware shows this record or a later one. */
	return 0;
}

/*
 * Records the planes of a commit at once and publishes a list showing them, without waiting.
 *
 * The new state is checked against a view taken under the guard; the clock
 * is raised outside it; the view is applied only while no other primary or
 * overlay record came between (a cursor record may).
 */
int
bcm2711_display_flip_commit(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner,
	struct bcm2711_flip_change *changes,
	uint32_t count,
	uint64_t *serial,
	uint64_t *adopted)
{
	struct bcm2711_flip_primary primary;
	struct bcm2711_flip_overlay overlays[BCM2711_FLIP_OVERLAYS];
	unsigned long enabled;
	uint64_t generation;
	uint64_t bandwidth;
	uint32_t required;
	uint32_t retired;
	uint32_t attempt;
	uint32_t index;
	bool recorded;
	void *mapping;
	int error;

	/* Nothing is recorded before the guard exists, or for a lease that is not the owner's. */
	*serial = 0;
	*adopted = 0;
	if (!display->flip.initialized)
		return ENODEV;
	if (count == 0 || count > BCM2711_FLIP_PLANES)
		return EINVAL;
	error = bcm2711_display_flip_owned(display, owner);
	if (error != 0)
		return error;

	/* Images replaced by adopted lists go first, so the waiting room has space. */
	bcm2711_display_flip_reap(display);

	/* Checks, raises the clock, then applies; another plane record in between starts it again. */
	for (attempt = 0; attempt < FLIP_CLOCK_ATTEMPTS; attempt++) {
		/* The new state of every plane, from the recorded one and the changes. */
		enabled = spin_lock_irqsave(&display->flip.guard);

		error = owner_matches_locked(display, owner);
		if (error == 0 && !display->flip.attached)
			error = ENODEV;
		if (error == 0 && display->flip.restoring)
			error = EBUSY;
		if (error == 0 && display->flip.uncertain)
			error = EBUSY;
		if (error == 0)
			error = view_changes_locked(display, changes, count, &primary, overlays, &retired);
		generation = display->flip.planes_generation;

		spin_unlock_irqrestore(&display->flip.guard, enabled);

		/* Reports a refused lease or plane. */
		if (error != 0)
			return error;

		/* The HVS cannot read more than its budget, nor the clock run faster than its ceiling. */
		bandwidth = read_bandwidth(display, &primary, overlays);
		if (bandwidth > FLIP_BANDWIDTH_BUDGET)
			return ENOSPC;
		required = required_clock(display, &primary, overlays);
		if (required > display->max_core_hz)
			return ENOSPC;

		/* Raises the clock before any list that needs it is written. */
		error = raise_clock(display, required);
		if (error != 0)
			return error;

		/*
		 * A new cached image is cleaned from this thread, once: a commit that
		 * keeps the image, and the IRQ's later lists, show the same bytes.
		 */
		for (index = 0; index < count; index++) {
			/* Only a recorded new image. */
			if (!changes[index].recorded || !changes[index].image)
				continue;
			if (changes[index].frame.uncached)
				continue;
			mapping = kern_pmem_to_kernel(changes[index].frame.physical);
			if (mapping == NULL)
				return EFAULT;
			kern_dcache_clean_range(mapping, (size_t)changes[index].frame.size);
		}

		/* The cleaned bytes reach memory before any list names them. */
		kern_io_write_barrier();

		/* Applies the view unless another plane record or a lower clock came between. */
		enabled = spin_lock_irqsave(&display->flip.guard);

		error = owner_matches_locked(display, owner);
		if (error == 0 && !display->flip.attached)
			error = ENODEV;
		if (error == 0 && (display->flip.restoring || display->flip.uncertain))
			error = EBUSY;
		if (error != 0) {
			spin_unlock_irqrestore(&display->flip.guard, enabled);
			return error;
		}

		/* The view is stale, or the clock went down: check again. */
		if (display->flip.planes_generation != generation || display->flip.core_hz < required) {
			spin_unlock_irqrestore(&display->flip.guard, enabled);
			continue;
		}

		/* The replaced images need places among those waiting for adoption. */
		if (display->flip.retiring_count + retired > BCM2711_FLIP_RETIRING) {
			spin_unlock_irqrestore(&display->flip.guard, enabled);
			return EBUSY;
		}

		/* A list goes out now unless two are out: the hardware must still be ours. */
		if (display->flip.pending_count < 2U && !display->flip.console_pending) {
			error = check_pipeline(display, false);
			if (error != 0) {
				display->flip.uncertain = true;
				waitq_wake_all(&display->flip.adoption_waitq);
				spin_unlock_irqrestore(&display->flip.guard, enabled);
				return error;
			}
		}

		/* Every recorded change gets the state's next number; dropped ones change nothing. */
		recorded = false;
		for (index = 0; index < count; index++) {
			/* One recorded change is enough for a new state; the others are counted as dropped. */
			if (changes[index].recorded) {
				recorded = true;
			} else {
				display->flip.perf.dropped++;
			}
		}

		/* A commit with a recorded change is a new state, and a list goes out or waits for an adoption. */
		if (recorded) {
			display->flip.state_serial++;
			apply_changes_locked(display, changes, count, &primary, overlays);
			(void)publish_locked(display);
		}

		/* The record's number (or the state's, when nothing was recorded) and the adopted one. */
		*serial = display->flip.state_serial;
		*adopted = display->flip.adopted_serial;

		spin_unlock_irqrestore(&display->flip.guard, enabled);

		/* A window of overlay changes is reported even while the primary does not change. */
		bcm2711_display_flip_report(display, false, 0, 0, 0);

		/* Succeeded: the planes show from the refresh boundary that adopts a list with them. */
		return 0;
	}

	/* Other plane records kept coming between the check and the record. */
	return EBUSY;
}

/*
 * Waits until the record numbered serial is adopted (GPU_DISPLAY_WAIT).
 *
 * Unlike a present's own wait, a timeout here is the caller's choice and
 * leaves nothing uncertain.  A signal ends the wait with EINTR.
 */
int
bcm2711_display_flip_wait_number(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner,
	uint64_t serial,
	uint64_t timeout,
	uint64_t *adopted,
	uint64_t *adopted_ticks)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	int error;

	/* Nothing was recorded before the guard exists. */
	*adopted = 0;
	*adopted_ticks = 0;
	if (!display->flip.initialized)
		return ENODEV;

	/* Sleeps until the adoption, the lease's end, a signal or the deadline. */
	now = sched_ticks();
	deadline = now + timeout;
	enabled = spin_lock_irqsave(&display->flip.guard);

	for (;;) {
		/* The lease ended, or another open owns the planes now. */
		error = owner_matches_locked(display, owner);
		if (error != 0)
			break;

		/* The record, or a later one, is adopted. */
		if (display->flip.adopted_serial >= serial)
			break;

		/* No time to wait: the record is not yet adopted. */
		if (timeout == 0) {
			error = EAGAIN;
			break;
		}

		/* The deadline passed. */
		now = sched_ticks();
		if (now >= deadline) {
			error = ETIMEDOUT;
			break;
		}

		/* Sleeps until the IRQ, a revocation or a signal. */
		observed = waitq_sequence(&display->flip.adoption_waitq);
		error = waitq_sleep(&display->flip.adoption_waitq, &display->flip.guard, observed, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EINTR)
			break;
		error = 0;
	}

	/* The adopted number and time the caller reports, whatever the outcome. */
	*adopted = display->flip.adopted_serial;
	*adopted_ticks = display->flip.adopted_ticks;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Images the adoption no longer shows go now, in this thread. */
	bcm2711_display_flip_reap(display);

	/* Reports the lease's end, a signal or the time running out. */
	if (error != 0)
		return error;

	/* Succeeded: the record is adopted. */
	return 0;
}

/*
 * Presents one buffer of the driver's own over the whole output and waits for it.
 */
int
bcm2711_display_flip_present(
	struct bcm2711_display *display,
	const struct drv_bcm2711_boot_screen *frame)
{
	int error;

	/* An opaque frame at the origin covers the output and hides the console. */
	error = bcm2711_display_flip_compose(display, frame, 0, 0, false);
	if (error != 0)
		return error;

	/* Succeeded: the selected PV observed actual primary adoption. */
	return 0;
}

/*
 * Composes an unscaled frame of the driver's own over the console and waits for its adoption.
 */
int
bcm2711_display_flip_compose(
	struct bcm2711_display *display,
	const struct drv_bcm2711_boot_screen *frame,
	uint32_t x,
	uint32_t y,
	bool premultiplied)
{
	struct bcm2711_flip_primary primary;
	struct bcm2711_flip_owner owner;
	uint64_t serial;
	uint64_t mark;
	uint64_t wait_ticks;
	int error;

	/* Refuses a missing frame before describing it. */
	if (frame == NULL)
		return EINVAL;

	/* The driver's own record: no lease, no borrowed allocation. */
	kern_memset(&primary, 0, sizeof(primary));
	primary.shown = true;
	primary.premultiplied = premultiplied;
	primary.x = x;
	primary.y = y;
	primary.frame = *frame;
	owner.session = NULL;
	owner.lease = 0;

	/* Records the frame and publishes its list. */
	error = bcm2711_display_flip_record(display, &owner, &primary, &serial);
	if (error != 0)
		return error;

	/* Waits for the list's adoption. */
	mark = clock_ticks();
	error = bcm2711_display_flip_wait(display, &owner, serial);
	wait_ticks = clock_ticks() - mark;
	if (error != 0)
		return error;

	/* The adoption is a cadence event and a flip of the report. */
	bcm2711_display_flip_note(display, BCM2711_FLIP_CADENCE_FLIP);
	bcm2711_display_flip_report(display, true, 0, 0, wait_ticks);

	/* Succeeded: a selected-PV frame adopted a list with the frame. */
	return 0;
}

/*
 * Records the cursor and publishes a list with it, without waiting.
 *
 * While two lists are out, the next adoption carries the change; a change
 * that shows nothing new (a hidden cursor hidden again) makes no list.
 */
int
bcm2711_display_flip_cursor(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner,
	const struct bcm2711_flip_cursor *cursor)
{
	unsigned long enabled;
	int error;

	/* A shown image is small, unscaled and readable by the HVS below one GiB. */
	if (!display->flip.initialized)
		return ENODEV;
	if (cursor->shown) {
		if (cursor->width == 0 || cursor->width > 64U || cursor->height == 0 || cursor->height > 64U)
			return EINVAL;
		if (cursor->format > 1U || cursor->pitch < cursor->width * 4U || (cursor->pitch & 3U) != 0)
			return EINVAL;
		if (cursor->physical == 0 || (cursor->physical & 3U) != 0)
			return EINVAL;
		if ((uint64_t)cursor->physical + (uint64_t)cursor->pitch * cursor->height > 0x40000000ULL)
			return EINVAL;
	}

	/* The newest cursor replaces any change not yet in a list. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	error = owner_matches_locked(display, owner);
	if (error == 0 && !display->flip.attached)
		error = ENODEV;
	if (error == 0 && display->flip.restoring)
		error = EBUSY;
	if (error != 0) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return error;
	}

	/* A change of a cursor hidden before and after shows nothing new and makes no list. */
	if (cursor->shown || display->flip.cursor.shown) {
		display->flip.state_serial++;
		display->flip.perf.cursor_records++;
	}

	/* The newest cursor is the recorded one, shown or not. */
	display->flip.cursor = *cursor;

	/* A pipeline that is no longer ours keeps the change recorded and is left to recovery. */
	(void)publish_locked(display);

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the cursor shows from a coming refresh boundary. */
	return 0;
}

/*
 * Tells whether a cursor image may be read by the HVS or by a coming list.
 *
 * It is named while a list the hardware may read shows it or while the
 * recorded cursor (published or not) uses it; any other image may be
 * rewritten at once.
 */
bool
bcm2711_display_flip_cursor_named(
	struct bcm2711_display *display,
	uint32_t physical)
{
	unsigned long enabled;
	uint32_t slot;
	bool named;

	/* Nothing names an image before the guard exists. */
	if (!display->flip.initialized)
		return false;

	/* Lists the hardware may read and the recorded cursor are read together. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	named = false;
	if (display->flip.cursor.physical == physical)
		named = true;
	for (slot = 0; slot < BCM2711_FLIP_SLOTS; slot++) {
		/* Only a list the HVS may still fetch keeps its image. */
		if (display->flip.slots[slot].used && display->flip.slots[slot].cursor_image == physical)
			named = true;
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the answer holds while the caller excludes other cursor changes. */
	return named;
}

/*
 * Tells how a range of storage is named: by the recorded primary (it stays
 * named until a later record), only by lists the hardware may read (an
 * adoption ends that), or not at all.
 */
unsigned
bcm2711_display_flip_frame_use(
	struct bcm2711_display *display,
	uint64_t physical,
	uint64_t bytes)
{
	unsigned long enabled;
	uint32_t slot;
	unsigned use;
	bool overlaps;

	/* Nothing names storage before the guard exists. */
	if (!display->flip.initialized)
		return BCM2711_FLIP_FRAME_FREE;

	/* The recorded primary and the lists are read together. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	use = BCM2711_FLIP_FRAME_FREE;
	for (slot = 0; slot < BCM2711_FLIP_SLOTS; slot++) {
		/* A list the hardware may read keeps its frame until a later adoption. */
		if (!display->flip.slots[slot].used || !display->flip.slots[slot].has_frame)
			continue;
		overlaps = range_overlaps(&display->flip.slots[slot].frame, physical, bytes);
		if (overlaps)
			use = BCM2711_FLIP_FRAME_LISTED;
	}

	/* The recorded primary keeps its frame until a later record replaces it. */
	if (display->flip.primary.shown) {
		overlaps = range_overlaps(&display->flip.primary.frame, physical, bytes);
		if (overlaps)
			use = BCM2711_FLIP_FRAME_RECORDED;
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the answer holds while the caller excludes other records of this storage. */
	return use;
}

/*
 * Waits until no list the hardware may read names a range of storage.
 *
 * The caller has seen it named only by lists, and excludes records naming
 * it; an adoption of a later list ends that.  At most 100 ms.
 */
int
bcm2711_display_flip_wait_unlisted(
	struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner,
	uint64_t physical,
	uint64_t bytes)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	uint32_t slot;
	bool listed;
	bool overlaps;
	int error;

	/* Nothing names storage before the guard exists. */
	if (!display->flip.initialized)
		return 0;

	/* Sleeps from adoption to adoption until no list names the storage. */
	deadline = sched_ticks() + FLIP_ADOPTION_TICKS;
	enabled = spin_lock_irqsave(&display->flip.guard);

	for (;;) {
		/* Looks for a list the hardware may read that names the storage. */
		listed = false;
		for (slot = 0; slot < BCM2711_FLIP_SLOTS; slot++) {
			/* Only a used list with a frame can name it. */
			if (!display->flip.slots[slot].used || !display->flip.slots[slot].has_frame)
				continue;
			overlaps = range_overlaps(&display->flip.slots[slot].frame, physical, bytes);
			if (overlaps)
				listed = true;
		}

		/* No list names it: the storage may be written. */
		if (!listed) {
			error = 0;
			break;
		}

		/* The lease ended, or the lists will not move on their own. */
		error = owner_matches_locked(display, owner);
		if (error == 0 && display->flip.uncertain)
			error = EBUSY;
		if (error != 0)
			break;

		/* The hardware did not take a later list in time. */
		now = sched_ticks();
		if (now >= deadline) {
			error = ETIMEDOUT;
			break;
		}

		/* Sleeps until the next adoption or the deadline. */
		observed = waitq_sequence(&display->flip.adoption_waitq);
		(void)waitq_sleep(&display->flip.adoption_waitq, &display->flip.guard, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Reports the lease's end, uncertainty or the timeout. */
	if (error != 0)
		return error;

	/* Succeeded: no list the hardware may read names the storage. */
	return 0;
}

/*
 * Lets go of borrowed images whose replacement has been adopted.
 *
 * Called in thread context only (a release may free the allocation); while
 * the state is uncertain nothing goes, as no adoption can be trusted.
 */
void
bcm2711_display_flip_reap(
	struct bcm2711_display *display)
{
	struct bcm2711_buffer *released[BCM2711_FLIP_RETIRING];
	unsigned long enabled;
	uint32_t count;
	uint32_t index;
	uint32_t kept;

	/* Nothing waits before the guard exists. */
	if (!display->flip.initialized)
		return;

	/* Takes the images whose replacing record is shown by an adopted list. */
	count = 0;
	enabled = spin_lock_irqsave(&display->flip.guard);

	kept = 0;
	for (index = 0; index < display->flip.retiring_count; index++) {
		/* An entry still waiting moves down over the ones taken. */
		if (display->flip.uncertain || display->flip.retiring[index].serial > display->flip.adopted_serial) {
			display->flip.retiring[kept] = display->flip.retiring[index];
			kept++;
			continue;
		}

		/* The replacing record is shown: the image goes. */
		released[count] = display->flip.retiring[index].hold;
		count++;
	}

	/* The entries still waiting are the first kept ones. */
	display->flip.retiring_count = kept;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Releases each taken image outside the guard. */
	for (index = 0; index < count; index++)
		bcm2711_buffer_release(released[index]);
}

/*
 * Restores the retained boot console without changing mode or routing.
 *
 * Records stop while it runs.  A fresh matching PV interrupt is required even
 * after a late uncertain flip; the console's adoption lets every borrowed
 * image go and lowers the clock again.
 */
int
bcm2711_display_flip_restore(
	struct bcm2711_display *display)
{
	struct bcm2711_buffer *released[BCM2711_FLIP_RETIRING + BCM2711_FLIP_PLANES];
	unsigned long enabled;
	uint32_t count;
	uint32_t index;
	void *mapping;
	int error;

	/* Requires the persistent owner and the existing console CPU mapping. */
	if (!display->flip.initialized)
		return ENODEV;
	mapping = kern_pmem_to_kernel(display->screen.physical);
	if (mapping == NULL)
		return EFAULT;

	/* Stops records and publications while the console is prepared and published. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	if (!display->flip.attached) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return ENODEV;
	}

	/* One restore at a time; the display device's controller mutex makes this the rule. */
	if (display->flip.restoring) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return EBUSY;
	}

	/* restoring keeps records and IRQ publications away until the console is back. */
	display->flip.restoring = true;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Raises the provider clock before restoring a potentially heavier active list. */
	error = raise_clock(display, display->console_core_hz);
	if (error != 0) {
		enabled = spin_lock_irqsave(&display->flip.guard);
		display->flip.restoring = false;
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return error;
	}

	/* Makes current console contents visible before reinstating its original list. */
	kern_dcache_clean_range(mapping, (size_t)display->screen.size);
	kern_io_write_barrier();

	/* Allows recovery from uncertain current/next pointers only on this pipeline. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	error = check_pipeline(display, true);
	if (error != 0) {
		display->flip.uncertain = true;
		display->flip.restoring = false;
		waitq_wake_all(&display->flip.adoption_waitq);
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return error;
	}

	/* The planes go: the primary, the overlays and the cursor are hidden from now on. */
	display->flip.state_serial++;
	display->flip.primary.shown = false;
	for (index = 0; index < BCM2711_FLIP_OVERLAYS; index++)
		display->flip.overlays[index].shown = false;
	kern_memset(&display->flip.cursor, 0, sizeof(display->flip.cursor));
	display->flip.planes_generation++;

	/* Requeues the preserved console list and demands a fresh matching source. */
	display->flip.console_pending = true;
	publish_list(display, FLIP_CONSOLE_LIST, true);

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Waits for the console's adoption, which is the only proof every list is gone. */
	error = wait_console(display);
	if (error != 0)
		return error;

	/* Every borrowed image goes: the console list names none. */
	count = 0;
	enabled = spin_lock_irqsave(&display->flip.guard);

	if (display->flip.primary.hold != NULL) {
		released[count] = display->flip.primary.hold;
		count++;
	}

	/* The primary was hidden by the restore and borrows nothing more. */
	kern_memset(&display->flip.primary, 0, sizeof(display->flip.primary));

	/* Neither do the overlays. */
	for (index = 0; index < BCM2711_FLIP_OVERLAYS; index++) {
		/* A hidden overlay's image, if it had one. */
		if (display->flip.overlays[index].hold != NULL) {
			released[count] = display->flip.overlays[index].hold;
			count++;
		}
	}

	/* The overlays are empty for the next lease. */
	kern_memset(display->flip.overlays, 0, sizeof(display->flip.overlays));

	/* Every image waiting for an adoption goes too. */
	for (index = 0; index < display->flip.retiring_count; index++) {
		released[count] = display->flip.retiring[index].hold;
		count++;
	}

	/* Nothing waits any more. */
	display->flip.retiring_count = 0;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Releases them outside the guard. */
	for (index = 0; index < count; index++)
		bcm2711_buffer_release(released[index]);

	/* Lowers bandwidth only after adoption, while records are still excluded. */
	lower_clock(display, display->console_core_hz);

	/* Records may come again. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	display->flip.restoring = false;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: no caller-provided framebuffer remains retained. */
	return 0;
}

/*
 * Copies the state's progress and what its lists name.
 * An uninitialized display reports an unattached, empty status.
 */
void
bcm2711_display_flip_snapshot(
	struct bcm2711_display *display,
	struct bcm2711_flip_status *status)
{
	unsigned long enabled;
	uint32_t slot;

	/* Clears the caller's result without dereferencing an uninitialized guard. */
	kern_memset(status, 0, sizeof(*status));
	if (!display->flip.initialized)
		return;

	/* Copies one coherent observation under the guard. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	status->attached = display->flip.attached;
	status->uncertain = display->flip.uncertain;
	status->restoring = display->flip.restoring;
	status->active_list = display->flip.active_list;
	status->pending_count = display->flip.pending_count;
	status->retiring_count = display->flip.retiring_count;
	status->frame_sequence = display->flip.frame_sequence;
	status->state_serial = display->flip.state_serial;
	status->list_serial = display->flip.list_serial;
	status->adopted_serial = display->flip.adopted_serial;
	status->core_hz = display->flip.core_hz;
	status->clock_error = display->flip.clock_error;
	status->primary = display->flip.primary;
	kern_memcpy(status->overlays, display->flip.overlays, sizeof(status->overlays));
	for (slot = 0; slot < BCM2711_FLIP_SLOTS; slot++) {
		/* A slot's frame means something only while the slot is used. */
		if (display->flip.slots[slot].used)
			status->used_mask |= 1U << slot;
		status->frames[slot] = display->flip.slots[slot].frame;
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the caller owns one coherent flip-state observation. */
	return;
}

/*
 * Observes the selected PV's frame with the caller's guard held.
 *
 * The current-list register says which published list the hardware took:
 * it and every older list are settled, its state number is adopted, the
 * waiters wake and changes made meanwhile go out in a new list.  Nothing is
 * released here; the woken threads do that.
 */
void
bcm2711_display_flip_vblank_locked(
	struct bcm2711_display *display,
	uint32_t current)
{
	uint32_t index;
	uint32_t found;
	uint32_t list;

	/* Counts real selected-PV frames independently of submissions and old lists. */
	display->flip.frame_sequence++;
	if (!display->flip.attached)
		return;

	/* An underrun seen in the last frame is counted once more from now on. */
	enable_underrun_locked(display);

	/* The console's adoption retires every list, even after uncertainty. */
	if (display->flip.console_pending && current == FLIP_CONSOLE_LIST) {
		display->flip.console_pending = false;
		display->flip.active_list = FLIP_CONSOLE_LIST;
		display->flip.active_slot = BCM2711_FLIP_NO_SLOT;
		display->flip.pending_count = 0;
		for (index = 0; index < BCM2711_FLIP_SLOTS; index++)
			display->flip.slots[index].used = false;
		display->flip.adopted_serial = display->flip.state_serial;
		display->flip.list_serial = display->flip.state_serial;
		display->flip.adopted_ticks = sched_ticks();
		display->flip.uncertain = false;
		waitq_wake_all(&display->flip.adoption_waitq);
		return;
	}

	/* Uncertain lists stay held until the console comes back. */
	if (display->flip.uncertain)
		return;

	/* Finds the published list the hardware took, if it took one. */
	found = display->flip.pending_count;
	for (index = 0; index < display->flip.pending_count; index++) {
		/* The register holds one complete list's address. */
		list = slot_list(display->flip.pending[index]);
		if (list == current)
			found = index;
	}

	/* The hardware still shows the adopted list. */
	if (found == display->flip.pending_count)
		return;

	/* Settles the adopted list and every older one, and wakes the waiters. */
	adopt_slot_locked(display, found, current);
	display->flip.adopted_ticks = sched_ticks();
	count_adoption_locked(display, display->flip.active_slot);
	waitq_wake_all(&display->flip.adoption_waitq);

	/* Changes made while two lists were out go in a list of their own now. */
	(void)publish_locked(display);
}

/*
 * Counts one cursor image set (GPU_DISPLAY_CURSOR_SET) for the next flip
 * report: copied when its rows were written into a plane image, otherwise
 * shown again from an image that holds the same unchanged rows.
 */
void
bcm2711_display_flip_count_cursor(
	struct bcm2711_display *display,
	bool copied)
{
	/* The display device's plane mutex is not the guard, so the counts are atomic. */
	__atomic_fetch_add(&display->flip.perf_cursor_sets, 1U, __ATOMIC_RELAXED);
	if (copied)
		__atomic_fetch_add(&display->flip.perf_cursor_copies, 1U, __ATOMIC_RELAXED);
}

/*
 * Notes one display event of a cadence kind: its interval from the
 * previous one of the kind, and every two seconds the window's report.
 */
void
bcm2711_display_flip_note(
	struct bcm2711_display *display,
	unsigned kind)
{
	struct bcm2711_flip_cadence *cadence;
	unsigned long enabled;
	uint64_t milliseconds;
	uint64_t interval;
	uint64_t now;
	uint32_t index;
	bool report;
	bool measured;

	/* Nothing is kept without v3d.perf=1. */
	measured = perf_enabled(display);
	if (!measured || kind >= BCM2711_FLIP_CADENCE_KINDS)
		return;

	/* The interval joins the window's samples while they fit. */
	now = clock_ticks();
	report = false;
	milliseconds = 0;
	enabled = spin_lock_irqsave(&display->flip.guard);
	cadence = &display->flip.cadence[kind];
	if (cadence->last != 0 && cadence->stored < BCM2711_FLIP_CADENCE_SAMPLES) {
		interval = kern_ticks_to_rate(now - cadence->last, 1000000U);
		if (interval > UINT32_MAX)
			interval = UINT32_MAX;
		cadence->intervals[cadence->stored] = (uint32_t)interval;
		cadence->stored++;
	}
	cadence->last = now;
	cadence->count++;

	/* The first event opens the window; one caller takes a window of two seconds or more to print. */
	if (display->flip.cadence_start == 0)
		display->flip.cadence_start = now;
	milliseconds = kern_ticks_to_ms(now - display->flip.cadence_start);
	if (milliseconds >= 2000U && !display->flip.cadence_reporting) {
		display->flip.cadence_reporting = true;
		for (index = 0; index < BCM2711_FLIP_CADENCE_KINDS; index++) {
			/* The copy keeps the samples; the window starts empty, each kind keeping its last time. */
			cadence_copy[index] = display->flip.cadence[index];
			display->flip.cadence[index].count = 0;
			display->flip.cadence[index].stored = 0;
		}
		display->flip.cadence_start = now;
		report = true;
	}
	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* The copied window is sorted and printed without the guard. */
	if (!report)
		return;
	report_cadence(milliseconds);
	enabled = spin_lock_irqsave(&display->flip.guard);
	display->flip.cadence_reporting = false;
	spin_unlock_irqrestore(&display->flip.guard, enabled);
}

/*
 * Adds an event to the flip report and prints a window of two seconds or more (v3d.perf=1 only).
 *
 * flip says the caller saw an adopted primary flip, with clean and raise
 * the record's stages when the caller measured them (zero when the record
 * added them) and wait the adoption wait; a commit calls it with flip false
 * so a window of overlay changes alone is printed too.
 */
void
bcm2711_display_flip_report(
	struct bcm2711_display *display,
	bool flip,
	uint64_t clean,
	uint64_t raise,
	uint64_t wait)
{
	struct bcm2711_flip_perf window;
	unsigned long enabled;
	uint64_t elapsed;
	uint64_t now;
	uint64_t underruns;
	uint64_t sets;
	uint64_t copies;
	uint64_t count;
	uint32_t times;
	uint32_t replaced;
	bool report;
	bool measured;

	/* Nothing is kept without v3d.perf=1. */
	measured = perf_enabled(display);
	if (!measured)
		return;

	/* Adds this flip, and takes a full window for one caller to print. */
	now = clock_ticks();
	report = false;
	times = 0;
	replaced = 0;
	enabled = spin_lock_irqsave(&display->flip.guard);

	if (flip) {
		display->flip.perf.flips++;
		display->flip.perf.clean += clean;
		display->flip.perf.raise += raise;
		display->flip.perf.wait += wait;
	}

	/* The window's age, and whether it is full. */
	elapsed = 0;
	if (display->flip.perf.start != 0)
		elapsed = kern_ticks_to_ms(now - display->flip.perf.start);
	if (display->flip.perf.start == 0) {
		/* The first event opens the window; underruns are counted from the total at its start. */
		display->flip.perf.start = now;
		display->flip.perf.underruns = display->underruns;
	} else if (elapsed >= 2000U && !display->flip.perf_reporting) {
		/* The window and its settled overlay times are copied; the window starts again empty. */
		window = display->flip.perf;
		kern_memset(&display->flip.perf, 0, sizeof(display->flip.perf));
		display->flip.perf.start = now;
		display->flip.perf.underruns = display->underruns;
		display->flip.perf_reporting = true;
		times = take_times_locked(display, &replaced);
		report = true;
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* A window shorter than two seconds keeps accumulating. */
	if (!report)
		return;

	/* The window's cursor images and underruns, taken with the window. */
	sets = __atomic_exchange_n(&display->flip.perf_cursor_sets, 0, __ATOMIC_RELAXED);
	copies = __atomic_exchange_n(&display->flip.perf_cursor_copies, 0, __ATOMIC_RELAXED);
	underruns = display->underruns - window.underruns;

	/* Prints microseconds per flip for each stage and the window's counts. */
	count = window.flips;
	if (count == 0)
		count = 1;
	kern_logf(
		"bcm2711: flip perf ms=%llu flips=%llu avg_us clean=%llu raise=%llu wait=%llu cursor sets=%llu copies=%llu records primary=%llu cursor=%llu overlay=%llu dropped=%llu lists=%llu replaced=%llu coalesced=%llu underruns=%llu pair=%llu/%llu\n",
		(unsigned long long)kern_ticks_to_ms(now - window.start),
		(unsigned long long)window.flips,
		(unsigned long long)(kern_ticks_to_rate(window.clean, 1000000U) / count),
		(unsigned long long)(kern_ticks_to_rate(window.raise, 1000000U) / count),
		(unsigned long long)(kern_ticks_to_rate(window.wait, 1000000U) / count),
		(unsigned long long)sets,
		(unsigned long long)copies,
		(unsigned long long)window.primary_records,
		(unsigned long long)window.cursor_records,
		(unsigned long long)window.overlay_records,
		(unsigned long long)window.dropped,
		(unsigned long long)window.lists,
		(unsigned long long)window.replaced,
		(unsigned long long)window.coalesced,
		(unsigned long long)underruns,
		(unsigned long long)window.pair_both,
		(unsigned long long)window.pair_window);

	/* Prints the overlay records adopted in the window, and how many were replaced before any adoption. */
	report_times(times, replaced);

	/* Another caller may print the next window. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	display->flip.perf_reporting = false;

	spin_unlock_irqrestore(&display->flip.guard, enabled);
}

/* Checks that a primary plane fits the output unscaled, in an encodable buffer away from the console. */
static int
check_primary(
	const struct bcm2711_display *display,
	const struct bcm2711_flip_primary *primary)
{
	struct drv_bcm2711_boot_screen extent;
	uint32_t words[FLIP_PLANE_WORDS];
	const struct drv_bcm2711_boot_screen *frame;
	bool overlap;
	int error;

	/* Refuses any crop, scaling or overflow of the established output. */
	frame = &primary->frame;
	if (primary->x > display->screen.width || frame->width > display->screen.width - primary->x)
		return EINVAL;
	if (primary->y > display->screen.height || frame->height > display->screen.height - primary->y)
		return EINVAL;

	/* Validates the buffer using its unscaled dimensions and native byte order. */
	extent = display->screen;
	extent.width = frame->width;
	extent.height = frame->height;
	error = encode_plane(&extent, frame, words);
	if (error != 0)
		return error;

	/* The console is never scanned out from storage that also holds a frame. */
	overlap = frames_overlap(frame, &display->screen);
	if (overlap)
		return EINVAL;

	/* Succeeded: the plane can be encoded into any later list. */
	return 0;
}

/* Generates one independently encoded unscaled RGB32 plane at the origin with fixed alpha. */
static int
encode_plane(
	const struct drv_bcm2711_boot_screen *extent,
	const struct drv_bcm2711_boot_screen *frame,
	uint32_t *words)
{
	uint32_t order;

	/* Requires the same dimensions with either independently encoded RGB byte order. */
	if (frame->width != extent->width || frame->height != extent->height)
		return EINVAL;
	if (frame->width == 0 || frame->width > 4096U)
		return EINVAL;
	if (frame->height == 0 || frame->height > 4096U)
		return EINVAL;
	if (frame->format > 1U)
		return ENOTSUP;

	/* Bounds every DMA row and keeps the bus alias representable below one GiB. */
	if ((frame->physical & 3U) != 0 || frame->physical >= 0x40000000ULL)
		return EINVAL;
	if (frame->size == 0 || frame->size > 0x40000000ULL - frame->physical)
		return EINVAL;
	if (frame->pitch < frame->width * 4U || frame->pitch > 65535U)
		return EINVAL;
	if ((frame->pitch & 3U) != 0 || (uint64_t)frame->pitch * frame->height > frame->size)
		return EINVAL;

	/* Encodes a fresh eight-word plane without inherited contexts. */
	order = 2;
	if (frame->format == 1)
		order = 3;
	words[0] = 0x48009807U | (order << 13);
	words[1] = 0;
	words[2] = 0x4000fff0U;
	words[3] = (frame->height << 16) | frame->width;
	words[4] = 0xc0c0c0c0U;
	words[5] = 0xc0000000U | (uint32_t)frame->physical;
	words[6] = 0xc0c0c0c0U;
	words[7] = frame->pitch;

	/* Succeeded: the completed words may be copied into an unused SRAM slot. */
	return 0;
}

/* Tells whether an opaque primary at the origin covers the whole output, so the console cannot show. */
static bool
console_hidden(
	const struct bcm2711_display *display,
	const struct bcm2711_flip_primary *primary)
{
	/* A hidden or translucent primary lets the console through. */
	if (!primary->shown || primary->premultiplied)
		return false;

	/* A primary away from the origin or smaller than the output leaves console pixels. */
	if (primary->x != 0 || primary->y != 0)
		return false;
	if (primary->frame.width != display->screen.width || primary->frame.height != display->screen.height)
		return false;

	/* Succeeded: no console pixel can show. */
	return true;
}

/*
 * Computes the core clock a list of these planes needs: sixty percent of
 * the planes' pixels per second, at four pixels per compositor cycle for an
 * unscaled plane and two for a scaled one, and never less than the console's.
 */
static uint32_t
required_clock(
	const struct bcm2711_display *display,
	const struct bcm2711_flip_primary *primary,
	const struct bcm2711_flip_overlay *overlays)
{
	uint64_t quarters;
	uint64_t load;
	uint32_t required;
	uint32_t index;
	bool hidden;
	bool scaled;

	/* The primary, and the console when it shows through, at four pixels a cycle. */
	quarters = 0;
	if (primary->shown)
		quarters = (uint64_t)primary->frame.width * primary->frame.height;
	hidden = console_hidden(display, primary);
	if (!hidden)
		quarters += (uint64_t)display->screen.width * display->screen.height;

	/* Every shown overlay's shown part, a scaled one counting twice (two pixels a cycle). */
	for (index = 0; index < BCM2711_FLIP_OVERLAYS; index++) {
		/* A hidden overlay is not in the list. */
		if (!overlays[index].shown)
			continue;
		scaled = overlay_scaled(&overlays[index]);
		load = (uint64_t)overlays[index].dst_width * overlays[index].dst_height;
		if (scaled)
			load *= 2U;
		quarters += load;
	}

	/* One output uses sixty percent of the aggregate plane cycle demand. */
	load = quarters * ((display->refresh_millihz + 500U) / 1000U) / 4U;
	load = load * 60U / 100U;
	required = display->console_core_hz;
	if (load > 0xffffffffULL) {
		/* More than any clock: the caller refuses it. */
		required = 0xffffffffU;
	} else if (load > required) {
		/* Above the console's floor. */
		required = (uint32_t)load;
	}

	/* Succeeded: the floor the list needs. */
	return required;
}

/*
 * Computes the bytes per second the HVS reads for a list of these planes:
 * each shown plane's source pixels, four bytes each, every refresh, and a
 * vertically reduced overlay's as many times as its reduction rounded up
 * (it reads that many source rows in one output row's time).
 */
static uint64_t
read_bandwidth(
	const struct bcm2711_display *display,
	const struct bcm2711_flip_primary *primary,
	const struct bcm2711_flip_overlay *overlays)
{
	uint64_t pixels;
	uint64_t rows;
	uint32_t index;
	bool hidden;

	/* The primary, and the console when it shows through. */
	pixels = 0;
	if (primary->shown)
		pixels = (uint64_t)primary->frame.width * primary->frame.height;
	hidden = console_hidden(display, primary);
	if (!hidden)
		pixels += (uint64_t)display->screen.width * display->screen.height;

	/* Every shown overlay's source rectangle, times its vertical reduction rounded up. */
	for (index = 0; index < BCM2711_FLIP_OVERLAYS; index++) {
		/* A hidden overlay is not read. */
		if (!overlays[index].shown)
			continue;
		rows = (overlays[index].src_height + overlays[index].dst_height - 1U) / overlays[index].dst_height;
		pixels += (uint64_t)overlays[index].src_width * overlays[index].src_height * rows;
	}

	/* Four bytes a pixel at the mode's refresh rate. */
	return pixels * 4U * display->refresh_millihz / 1000U;
}

/*
 * Raises the transition floor for a list that needs it.
 *
 * The clock never goes down while a lease shows lists, so a record only ever
 * raises it; the mutex serializes the firmware requests, which sleep.
 */
static int
raise_clock(
	struct bcm2711_display *display,
	uint32_t required)
{
	unsigned long enabled;
	uint32_t hz;
	uint32_t current;
	int error;

	/* Refuses bandwidth outside the ceiling captured before firmware teardown. */
	if (required == 0 || required > display->max_core_hz)
		return ENOTSUP;

	/* One firmware request at a time, never under the guard. */
	mutex_lock(&display->flip.clock_mutex);

	enabled = spin_lock_irqsave(&display->flip.guard);

	current = display->flip.core_hz;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* The same temporary floor as R0, at least, and never below the clock already running. */
	hz = 500000000U;
	if (hz < required)
		hz = required;
	if (hz < current)
		hz = current;
	if (hz > display->max_core_hz)
		hz = display->max_core_hz;

	/* A clock already at the floor needs no request. */
	error = 0;
	if (hz != current)
		error = request_clock(hz);

	/* Publishes the outcome for later records and the snapshot. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	display->flip.clock_error = error;
	if (error == 0)
		display->flip.core_hz = hz;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	mutex_unlock(&display->flip.clock_mutex);

	/* Refuses publication if the provider could not establish its bandwidth. */
	if (error != 0)
		return error;

	/* Succeeded: the transition clock precedes every list that needs it. */
	return 0;
}

/* Lowers the clock to the console's once the console list is adopted; a refusal keeps the higher, safe floor. */
static void
lower_clock(
	struct bcm2711_display *display,
	uint32_t hz)
{
	unsigned long enabled;
	uint32_t current;
	int error;

	/* One firmware request at a time, never under the guard. */
	mutex_lock(&display->flip.clock_mutex);

	enabled = spin_lock_irqsave(&display->flip.guard);

	current = display->flip.core_hz;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* An unchanged rate needs no request. */
	error = 0;
	if (hz != current)
		error = request_clock(hz);

	/* Publishes the clock diagnostic and the new rate. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	display->flip.clock_error = error;
	if (error == 0)
		display->flip.core_hz = hz;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	mutex_unlock(&display->flip.clock_mutex);
}

/*
 * Sets the display's floor of the core clock.
 *
 * The firmware runs the highest floor of its owners: while a video decoder
 * holds the highest rate, the display's lowering and raising change no
 * clock under the decode.
 */
static int
request_clock(
	uint32_t hz)
{
	int error;

	/* Leaves rounding to the same firmware provider used by R0 and Linux. */
	error = drv_rpi4_firmware_core_clock(DRV_RPI4_FIRMWARE_CORE_DISPLAY, hz);
	if (error != 0)
		return error;

	/* Succeeded: the firmware accepted this core-clock floor. */
	return 0;
}

/*
 * Checks sole ownership and unchanged timing before any list publication.
 *
 * Ordinarily the current list must be one the state knows the hardware may
 * read (the adopted one or a published one) and the next list the newest
 * published; recovery accepts any of this driver's lists on both.
 */
static int
check_pipeline(
	struct bcm2711_display *display,
	bool recovering)
{
	volatile uint8_t *hvs;
	volatile uint8_t *pv;
	uint32_t control;
	uint32_t route;
	uint32_t current;
	uint32_t next;
	uint32_t newest;
	uint32_t port;
	uint32_t slot;
	uint32_t index;
	uint32_t list;
	bool known;

	/* Requires the exact active geometry and no enabled competing HVS channel. */
	hvs = display->compositor.mapped;
	pv = display->timing[display->port].mapped;
	control = kern_mmio_read32(hvs + 0x40);
	if (control != (0x80000000U | (display->screen.width << 16) | display->screen.height))
		return EBUSY;
	control = kern_mmio_read32(hvs + 0x50);
	if ((control & 0x80000000U) != 0)
		return EBUSY;
	control = kern_mmio_read32(hvs + 0x60);
	if ((control & 0x80000000U) != 0)
		return EBUSY;

	/* Keeps the selected boot output on channel zero and the other disconnected. */
	for (port = 0; port < BCM2711_HDMI_COUNT; port++) {
		/* HDMI outputs use different mux registers, with identical two-bit fields. */
		route = kern_mmio_read32(hvs + 0x18U - port * 4U);
		route >>= 30;
		if (port == display->port) {
			/* The boot port must still feed channel zero. */
			if (route != 0)
				return EBUSY;
		} else {
			/* An independent display owner cannot share these spare SRAM slots. */
			if (route != BCM2711_NO_CHANNEL)
				return EBUSY;
		}
	}

	/* Requires ongoing pixel flow; a stopped source cannot prove buffer retirement. */
	control = kern_mmio_read32(pv);
	if ((control & 1U) == 0)
		return EBUSY;
	control = kern_mmio_read32(pv + 0x04);
	if ((control & 1U) == 0)
		return EBUSY;

	/* Recovery may overwrite only our own known lists, never a foreign owner. */
	current = kern_mmio_read32(hvs + 0x30);
	next = kern_mmio_read32(hvs + 0x20);
	if (recovering) {
		/* The console and the three slots are the only lists this component writes. */
		known = false;
		if (current == FLIP_CONSOLE_LIST)
			known = true;
		for (slot = 0; slot < BCM2711_FLIP_SLOTS; slot++) {
			/* A slot's list address. */
			list = slot_list(slot);
			if (current == list)
				known = true;
		}

		/* A foreign current list is never overwritten. */
		if (!known)
			return EBUSY;

		/* The next list is ours too. */
		known = false;
		if (next == FLIP_CONSOLE_LIST)
			known = true;
		for (slot = 0; slot < BCM2711_FLIP_SLOTS; slot++) {
			/* A slot's list address. */
			list = slot_list(slot);
			if (next == list)
				known = true;
		}

		/* A foreign next list is never replaced. */
		if (!known)
			return EBUSY;
	} else {
		/* The hardware shows the adopted list or one published since. */
		known = false;
		if (current == display->flip.active_list)
			known = true;
		for (index = 0; index < display->flip.pending_count; index++) {
			/* A published list the hardware may have taken already. */
			list = slot_list(display->flip.pending[index]);
			if (current == list)
				known = true;
		}

		/* Any other current list means another owner wrote the pointers. */
		if (!known)
			return EBUSY;

		/* The next pointer still holds the newest list this component published. */
		newest = display->flip.active_list;
		if (display->flip.pending_count != 0)
			newest = slot_list(display->flip.pending[display->flip.pending_count - 1U]);
		if (next != newest)
			return EBUSY;
	}

	/* Succeeded: spare SRAM and channel-zero publication belong to this owner. */
	return 0;
}

/* Tests physical intervals already bounded below one GiB by preparation or R0. */
static bool
frames_overlap(
	const struct drv_bcm2711_boot_screen *a,
	const struct drv_bcm2711_boot_screen *b)
{
	/* Rejects adjacent intervals only when they share an actual byte. */
	if (a->physical >= b->physical + b->size)
		return false;
	if (b->physical >= a->physical + a->size)
		return false;

	/* Succeeded: both descriptions refer to at least one common DMA byte. */
	return true;
}

/* Tests whether a frame shares a byte with a range of storage. */
static bool
range_overlaps(
	const struct drv_bcm2711_boot_screen *frame,
	uint64_t physical,
	uint64_t bytes)
{
	/* The range ends before the frame, or starts after it. */
	if (physical + bytes <= frame->physical)
		return false;
	if (physical >= frame->physical + frame->size)
		return false;

	/* Succeeded: they share at least one byte. */
	return true;
}

/* Checks an owner against the lease copy; the caller holds the guard. */
static int
owner_matches_locked(
	const struct bcm2711_display *display,
	const struct bcm2711_flip_owner *owner)
{
	/* Another open's lease, a different lease of the same open, or none. */
	if (display->flip.owner != owner->session || display->flip.lease != owner->lease)
		return EBUSY;

	/* A lease being released records nothing more. */
	if (display->flip.revoking)
		return EBUSY;

	/* Succeeded: the owner may change the planes. */
	return 0;
}

/*
 * Publishes a list made from the recorded state, when one can go out.
 *
 * Called with the guard held, by records and by the vblank IRQ.  No list
 * goes out while the state is uncertain, the console is coming back or the
 * newest list already shows the state.  With no list out, the new one goes
 * to a free slot; with one out, the new one replaces it (the hardware takes
 * one complete list or the other); with two out, the change waits for the
 * next adoption.
 */
static int
publish_locked(
	struct bcm2711_display *display)
{
	uint32_t slot;
	uint32_t list;
	bool fresh;
	int error;

	/* Only an attached, certain pipeline outside a restore takes lists. */
	if (!display->flip.attached || display->flip.uncertain || display->flip.restoring)
		return 0;
	if (display->flip.console_pending)
		return 0;

	/* The newest list already shows every record. */
	if (display->flip.list_serial == display->flip.state_serial)
		return 0;

	/* Two lists are out: the next adoption carries the change. */
	if (display->flip.pending_count == 2U) {
		display->flip.perf.coalesced++;
		return 0;
	}

	/* The channel and the list pointers must still be ours. */
	error = check_pipeline(display, false);
	if (error != 0) {
		display->flip.uncertain = true;
		waitq_wake_all(&display->flip.adoption_waitq);
		return error;
	}

	/* At most two slots are used now (the adopted list and one out), so one is free. */
	for (slot = 0; slot < BCM2711_FLIP_SLOTS; slot++) {
		/* An unused slot's list is never read by the hardware. */
		if (!display->flip.slots[slot].used)
			break;
	}

	/* A fourth list cannot be out: the count above makes this impossible. */
	if (slot == BCM2711_FLIP_SLOTS)
		__builtin_trap();

	/* Writes the whole list, then points the hardware at it. */
	list = write_list_locked(display, slot);
	fresh = true;
	if (display->flip.pending_count != 0) {
		/* A replacement must not drop a vblank the list it replaces may still need. */
		fresh = false;
		display->flip.perf.replaced++;
	}

	/* The hardware follows the pointer at a coming frame start. */
	publish_list(display, list, fresh);

	/* The list is out, the newest, and shows every record so far. */
	display->flip.pending[display->flip.pending_count] = slot;
	display->flip.pending_count++;
	display->flip.list_serial = display->flip.state_serial;
	display->flip.perf.lists++;

	/* Succeeded: the hardware takes the list at a coming frame start. */
	return 0;
}

/*
 * Writes a list made from the recorded state into an unused slot.
 *
 * The list is the console (unless an opaque full-screen primary hides it),
 * the primary, the cursor and the terminator, in that order from the bottom.
 * Returns the list's address; the slot records what the list names.
 */
static uint32_t
write_list_locked(
	struct bcm2711_display *display,
	uint32_t slot)
{
	struct drv_bcm2711_boot_screen extent;
	struct bcm2711_flip_primary *primary;
	uint32_t words[FLIP_LIST_MAX_WORDS];
	uint32_t count;
	uint32_t cursor;
	uint32_t index;
	uint32_t list;
	bool hidden;
	int error;

	/* The console below everything, unless the primary covers it. */
	primary = &display->flip.primary;
	count = 0;
	hidden = console_hidden(display, primary);
	if (!hidden) {
		error = encode_plane(&display->screen, &display->screen, words);
		if (error != 0)
			__builtin_trap();
		count += FLIP_PLANE_WORDS;
	}

	/* The primary at its place, opaque or with premultiplied alpha; it was checked when recorded. */
	if (primary->shown) {
		extent = display->screen;
		extent.width = primary->frame.width;
		extent.height = primary->frame.height;
		error = encode_plane(&extent, &primary->frame, words + count);
		if (error != 0)
			__builtin_trap();
		words[count + 1U] = (primary->y << 16) | primary->x;
		if (primary->premultiplied)
			words[count + 2U] = 0x2000fff0U;
		count += FLIP_PLANE_WORDS;
	}

	/* The overlays above the primary, in plane order. */
	for (index = 0; index < BCM2711_FLIP_OVERLAYS; index++)
		count += overlay_words(&display->flip.overlays[index], index, words + count);

	/* The cursor on top, clipped to the output; it adds nothing when hidden or wholly outside. */
	cursor = cursor_words(display, words + count);
	count += cursor;

	/* The terminator follows every plane. */
	words[count] = FLIP_LIST_END;
	count++;

	/* Fresh context words go into the unused slot; the hardware does not read it yet. */
	list = slot_list(slot);
	for (index = 0; index < count; index++)
		kern_mmio_write32(display->compositor.mapped + 0x4000U + (list + index) * 4U, words[index]);

	/* The slot names the frame and the cursor image until a later adoption retires it. */
	display->flip.slots[slot].used = true;
	display->flip.slots[slot].serial = display->flip.state_serial;
	display->flip.slots[slot].has_frame = primary->shown;
	kern_memset(&display->flip.slots[slot].frame, 0, sizeof(display->flip.slots[slot].frame));
	if (primary->shown)
		display->flip.slots[slot].frame = primary->frame;
	display->flip.slots[slot].cursor_image = 0;
	if (cursor != 0)
		display->flip.slots[slot].cursor_image = display->flip.cursor.physical;

	/* The places and record numbers the adoption's counts compare. */
	display->flip.slots[slot].window_shown = display->flip.overlays[0].shown;
	display->flip.slots[slot].window_x = display->flip.overlays[0].dst_x;
	display->flip.slots[slot].window_y = display->flip.overlays[0].dst_y;
	display->flip.slots[slot].cursor_shown = display->flip.cursor.shown;
	display->flip.slots[slot].cursor_x = display->flip.cursor.x;
	display->flip.slots[slot].cursor_y = display->flip.cursor.y;
	kern_memcpy(display->flip.slots[slot].records, display->flip.plane_records, sizeof(display->flip.slots[slot].records));

	/* Succeeded: the complete list waits in its slot. */
	return list;
}

/*
 * Points the hardware's next-list register at a complete list, with the guard held.
 *
 * A fresh publication (no list of ours out) discards a stale vblank status
 * first; a replacement leaves it, as the list it replaces may have been
 * taken at that very frame start.
 */
static void
publish_list(
	struct bcm2711_display *display,
	uint32_t list,
	bool fresh)
{
	uint32_t background;

	/* The background fill is requested once and stays on. */
	background = kern_mmio_read32(display->compositor.mapped + 0x44);
	if ((background & 0x01000000U) == 0)
		kern_mmio_write32(display->compositor.mapped + 0x44, background | 0x01000000U);

	/* A fresh publication starts from a quiet vblank source. */
	if (fresh)
		kern_mmio_write32(display->timing[display->port].mapped + 0x28, 0x80);

	/* The complete SRAM list precedes the pointer the hardware follows. */
	kern_io_write_barrier();
	kern_mmio_write32(display->compositor.mapped + 0x20, list);
	kern_io_write_barrier();

	/* Succeeded: the hardware takes the list at a coming frame start. */
	return;
}

/* Returns a list slot's address in SRAM words. */
static uint32_t
slot_list(
	uint32_t slot)
{
	/* Slots follow one another from the first private list word. */
	return FLIP_FIRST_SLOT_LIST + slot * FLIP_SLOT_WORDS;
}

/*
 * Encodes the cursor plane clipped to the output, returning its word count (0 when nothing shows).
 *
 * The plane is unscaled with per-pixel premultiplied alpha, like a
 * translucent primary; clipping at the left and top moves the start of the
 * image rather than the plane.
 */
static uint32_t
cursor_words(
	const struct bcm2711_display *display,
	uint32_t *words)
{
	const struct bcm2711_flip_cursor *cursor;
	int64_t x;
	int64_t y;
	int64_t width;
	int64_t height;
	uint32_t offset;
	uint32_t order;

	/* A hidden cursor adds no plane. */
	cursor = &display->flip.cursor;
	if (!cursor->shown)
		return 0;
	x = cursor->x;
	y = cursor->y;
	width = cursor->width;
	height = cursor->height;
	offset = 0;

	/* Columns left of the output and rows above it are skipped in the image. */
	if (x < 0) {
		if (-x >= width)
			return 0;
		offset += (uint32_t)(-x) * 4U;
		width += x;
		x = 0;
	}
	if (y < 0) {
		if (-y >= height)
			return 0;
		offset += (uint32_t)(-y) * cursor->pitch;
		height += y;
		y = 0;
	}

	/* Columns and rows beyond the right and bottom are cut off. */
	if (x >= display->screen.width || y >= display->screen.height)
		return 0;
	if (x + width > display->screen.width)
		width = display->screen.width - x;
	if (y + height > display->screen.height)
		height = display->screen.height - y;

	/* The same eight words as a frame plane, at its position and with per-pixel alpha. */
	order = 2;
	if (cursor->format == 1)
		order = 3;
	words[0] = 0x48009807U | (order << 13);
	words[1] = ((uint32_t)y << 16) | (uint32_t)x;
	words[2] = 0x2000fff0U;
	words[3] = ((uint32_t)height << 16) | (uint32_t)width;
	words[4] = 0xc0c0c0c0U;
	words[5] = 0xc0000000U | (cursor->physical + offset);
	words[6] = 0xc0c0c0c0U;
	words[7] = cursor->pitch;

	/* Succeeded: one plane follows the frame's. */
	return FLIP_PLANE_WORDS;
}

/*
 * Settles the published list at pending index found as adopted; the caller holds the guard.
 *
 * The lists older than it (the adopted one and those published before it)
 * can no longer be read, so their slots are free; the record numbers the
 * adopted list shows are adopted.
 */
static void
adopt_slot_locked(
	struct bcm2711_display *display,
	uint32_t found,
	uint32_t current)
{
	uint32_t slot;
	uint32_t index;

	/* The list adopted before and the ones published before this one are gone. */
	if (display->flip.active_slot != BCM2711_FLIP_NO_SLOT)
		display->flip.slots[display->flip.active_slot].used = false;
	for (index = 0; index < found; index++)
		display->flip.slots[display->flip.pending[index]].used = false;

	/* The adopted list is the active one; later published ones stay out. */
	slot = display->flip.pending[found];
	display->flip.active_slot = slot;
	display->flip.active_list = current;
	for (index = found + 1U; index < display->flip.pending_count; index++)
		display->flip.pending[index - found - 1U] = display->flip.pending[index];
	display->flip.pending_count -= found + 1U;

	/* A list is made from the whole state, so every record up to its number is shown or replaced. */
	if (display->flip.slots[slot].serial > display->flip.adopted_serial)
		display->flip.adopted_serial = display->flip.slots[slot].serial;
}

/*
 * Counts the next underrun of the attached channel: the HVS IRQ masks the
 * source when it counts one, so it does not recur every line; this clears
 * the reported status and unmasks it again once a frame.  The caller holds
 * the guard.
 */
static void
enable_underrun_locked(
	struct bcm2711_display *display)
{
	uint32_t control;

	/* An unmasked source needs nothing. */
	control = kern_mmio_read32(display->compositor.mapped);
	if ((control & 0x200U) != 0)
		return;

	/* Clears the stale status before unmasking the channel-zero source. */
	kern_mmio_write32(display->compositor.mapped + 0x04, 0x200);
	kern_mmio_write32(display->compositor.mapped, control | 0x200U);
}

/* Waits for the console list's adoption after a restore published it, at most 100 ms. */
static int
wait_console(
	struct bcm2711_display *display)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	int error;

	/* Sleeps until the IRQ sees the console adopted, or the deadline. */
	deadline = sched_ticks() + FLIP_ADOPTION_TICKS;
	enabled = spin_lock_irqsave(&display->flip.guard);

	for (;;) {
		/* The console is the current list again. */
		if (!display->flip.console_pending) {
			error = 0;
			break;
		}

		/* The deadline passed: every image stays until a later restore succeeds. */
		now = sched_ticks();
		if (now >= deadline) {
			display->flip.uncertain = true;
			display->flip.restoring = false;
			waitq_wake_all(&display->flip.adoption_waitq);
			error = ETIMEDOUT;
			break;
		}

		/* Sleeps until the next adoption or the deadline. */
		observed = waitq_sequence(&display->flip.adoption_waitq);
		(void)waitq_sleep(&display->flip.adoption_waitq, &display->flip.guard, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Reports the missing adoption. */
	if (error != 0)
		return error;

	/* Succeeded: the hardware shows the console list. */
	return 0;
}

/*
 * Makes the new state of every plane from the recorded one and a commit's changes; the caller holds the guard.
 *
 * A change whose serial is not above its plane's last one is dropped
 * (recorded false) and changes nothing.  retired counts the borrowed images
 * the recorded changes replace or hide.  Nothing of the state changes here.
 */
static int
view_changes_locked(
	struct bcm2711_display *display,
	struct bcm2711_flip_change *changes,
	uint32_t count,
	struct bcm2711_flip_primary *primary,
	struct bcm2711_flip_overlay *overlays,
	uint32_t *retired)
{
	struct bcm2711_flip_change *change;
	struct bcm2711_flip_overlay *overlay;
	struct bcm2711_buffer *old_hold;
	uint32_t index;
	uint32_t shown;
	int error;

	/* Starts from the recorded planes. */
	*primary = display->flip.primary;
	kern_memcpy(overlays, display->flip.overlays, sizeof(display->flip.overlays));
	*retired = 0;

	/* Applies each change to the view, in the order given (each plane once). */
	for (index = 0; index < count; index++) {
		/* A plane this output has. */
		change = &changes[index];
		change->recorded = false;
		if (change->plane >= BCM2711_FLIP_PLANES)
			return EINVAL;

		/* A new image is only for a shown plane. */
		if (change->image && !change->shown)
			return EINVAL;

		/* A serial at or below the plane's last one was overtaken: dropped. */
		if (change->serial <= display->flip.plane_serials[change->plane])
			continue;
		change->recorded = true;

		/* The primary: the whole output, unscaled and opaque, from its new or kept image. */
		if (change->plane == 0) {
			old_hold = primary->hold;
			if (change->shown) {
				/* A kept image needs one recorded before. */
				if (change->image) {
					primary->frame = change->frame;
					primary->hold = change->hold;
				} else if (primary->frame.width == 0) {
					return EINVAL;
				}

				/* The source is the whole image and the destination the whole output. */
				if (change->src_x != 0 || change->src_y != 0 || change->dst_x != 0 || change->dst_y != 0)
					return EINVAL;
				if (change->src_width != primary->frame.width || change->src_height != primary->frame.height)
					return EINVAL;
				if (change->dst_width != display->screen.width || change->dst_height != display->screen.height)
					return EINVAL;
				primary->shown = true;
				primary->premultiplied = false;
				primary->x = 0;
				primary->y = 0;
				error = check_primary(display, primary);
				if (error != 0)
					return error;
			} else {
				/* A hidden primary borrows no image; the console shows. */
				kern_memset(primary, 0, sizeof(*primary));
			}

			/* A new image, or hiding, lets the old one go once adopted. */
			if (old_hold != NULL && (change->image || !change->shown))
				(*retired)++;
			continue;
		}

		/* An overlay: an unscaled part of its new or kept image, inside the output. */
		overlay = &overlays[change->plane - 1U];
		old_hold = overlay->hold;
		if (change->shown) {
			/* A kept image needs one recorded before. */
			if (change->image) {
				overlay->frame = change->frame;
				overlay->hold = change->hold;
			} else if (overlay->frame.width == 0) {
				return EINVAL;
			}

			/* The overlay's rectangles, checked against its image and the output. */
			overlay->shown = true;
			overlay->src_x = change->src_x;
			overlay->src_y = change->src_y;
			overlay->src_width = change->src_width;
			overlay->src_height = change->src_height;
			overlay->dst_x = change->dst_x;
			overlay->dst_y = change->dst_y;
			overlay->dst_width = change->dst_width;
			overlay->dst_height = change->dst_height;
			error = check_overlay(display, overlay);
			if (error != 0)
				return error;
		} else {
			/* A hidden overlay borrows no image and keeps none. */
			kern_memset(overlay, 0, sizeof(*overlay));
		}

		/* A new image, or hiding, lets the old one go once adopted. */
		if (old_hold != NULL && (change->image || !change->shown))
			(*retired)++;
	}

	/* An overlay shows over a primary, never over the console alone. */
	shown = 0;
	for (index = 0; index < BCM2711_FLIP_OVERLAYS; index++) {
		/* Counts the shown overlays. */
		if (overlays[index].shown)
			shown++;
	}

	/* A shown overlay needs the primary under it. */
	if (shown != 0 && !primary->shown)
		return EINVAL;

	/* Succeeded: the view is the state the commit would record. */
	return 0;
}

/* Checks that an overlay shows a part of an encodable image, inside the output and within the scaling limits. */
static int
check_overlay(
	const struct bcm2711_display *display,
	const struct bcm2711_flip_overlay *overlay)
{
	const struct drv_bcm2711_boot_screen *frame;
	uint32_t line;
	unsigned mode;
	bool overlap;

	/* An encodable image: a known byte order, aligned rows inside storage below one GiB. */
	frame = &overlay->frame;
	if (frame->format > 1U)
		return EINVAL;
	if (frame->width == 0 || frame->width > 4096U || frame->height == 0 || frame->height > 4096U)
		return EINVAL;
	if ((frame->physical & 3U) != 0 || frame->physical >= 0x40000000ULL)
		return EINVAL;
	if (frame->size == 0 || frame->size > 0x40000000ULL - frame->physical)
		return EINVAL;
	if (frame->pitch < frame->width * 4U || frame->pitch > 65535U || (frame->pitch & 3U) != 0)
		return EINVAL;
	if ((uint64_t)frame->pitch * frame->height > frame->size)
		return EINVAL;

	/* The console is never scanned out from storage that also holds a frame. */
	overlap = frames_overlap(frame, &display->screen);
	if (overlap)
		return EINVAL;

	/* The source rectangle lies inside the image. */
	if (overlay->src_width == 0 || overlay->src_height == 0)
		return EINVAL;
	if (overlay->src_x > frame->width || overlay->src_width > frame->width - overlay->src_x)
		return EINVAL;
	if (overlay->src_y > frame->height || overlay->src_height > frame->height - overlay->src_y)
		return EINVAL;

	/* The destination rectangle lies inside the output. */
	if (overlay->dst_width == 0 || overlay->dst_height == 0)
		return EINVAL;
	if (overlay->dst_x > display->screen.width || overlay->dst_width > display->screen.width - overlay->dst_x)
		return EINVAL;
	if (overlay->dst_y > display->screen.height || overlay->dst_height > display->screen.height - overlay->dst_y)
		return EINVAL;

	/* The plane's size fields hold 13 bits, its position 12. */
	if (overlay->dst_width > 8191U || overlay->dst_height > 8191U)
		return ENOSPC;
	if (overlay->dst_x > 4095U || overlay->dst_y > 4095U)
		return ENOSPC;

	/* The trapezoidal ratio field holds a reduction below 32 on each axis. */
	if (overlay->src_width >= 32U * overlay->dst_width || overlay->src_height >= 32U * overlay->dst_height)
		return ENOSPC;

	/* A vertically scaled overlay's line fits its part of the line buffer. */
	if (overlay->src_height != overlay->dst_height) {
		line = overlay->src_width;
		mode = scale_mode(overlay->src_width, overlay->dst_width);
		if (mode == FLIP_SCALE_TPZ)
			line = overlay->dst_width;
		if (line > FLIP_LINE_BUFFER_PIXELS)
			return ENOSPC;
	}

	/* Succeeded: the overlay can be encoded into any later list. */
	return 0;
}

/*
 * Records a checked commit view as the state; the caller holds the guard and has advanced the state's number.
 *
 * Each recorded change's plane takes its new state and serial; a replaced
 * or hidden borrowed image waits for the adoption of a list made from this
 * number or later.
 */
static void
apply_changes_locked(
	struct bcm2711_display *display,
	struct bcm2711_flip_change *changes,
	uint32_t count,
	const struct bcm2711_flip_primary *primary,
	const struct bcm2711_flip_overlay *overlays)
{
	struct bcm2711_buffer *old_hold;
	struct bcm2711_flip_time *time;
	uint32_t index;
	uint32_t plane;

	/* Moves the replaced images to the waiting room and the plane serials forward. */
	for (index = 0; index < count; index++) {
		/* A dropped change leaves its plane alone. */
		if (!changes[index].recorded)
			continue;
		plane = changes[index].plane;
		old_hold = display->flip.primary.hold;
		if (plane != 0)
			old_hold = display->flip.overlays[plane - 1U].hold;
		if (old_hold != NULL && (changes[index].image || !changes[index].shown)) {
			display->flip.retiring[display->flip.retiring_count].hold = old_hold;
			display->flip.retiring[display->flip.retiring_count].serial = display->flip.state_serial;
			display->flip.retiring_count++;
		}

		/*
		 * The plane's serial: later commits with a serial at or below it are
		 * dropped.  Its record number tells which record a list shows.
		 */
		display->flip.plane_serials[plane] = changes[index].serial;
		display->flip.plane_records[plane] = display->flip.state_serial;

		/* An overlay record's time joins the ring (v3d.perf=1 only). */
		if (plane != 0 && display->flip.cadence_state == BCM2711_FLIP_CADENCE_ON) {
			time = &display->flip.times[display->flip.times_next];
			time->plane = plane;
			time->plane_serial = changes[index].serial;
			time->state_serial = display->flip.state_serial;
			time->recorded = sched_ticks();
			time->adopted = 0;
			display->flip.times_next = (display->flip.times_next + 1U) % BCM2711_FLIP_TIMES;
		}

		/* The report counts the plane's record. */
		if (plane == 0) {
			display->flip.perf.primary_records++;
		} else {
			display->flip.perf.overlay_records++;
		}
	}

	/* The view is the state now; another commit's view taken before is stale. */
	display->flip.primary = *primary;
	kern_memcpy(display->flip.overlays, overlays, sizeof(display->flip.overlays));
	display->flip.planes_generation++;
}

/*
 * Encodes one overlay's plane: its source rectangle at its destination, with
 * per-pixel premultiplied alpha, unscaled in eight words or scaled with the
 * polyphase or trapezoidal filter on each axis (index picks the overlay's
 * part of the line buffer).
 */
static uint32_t
overlay_words(
	const struct bcm2711_flip_overlay *overlay,
	uint32_t index,
	uint32_t *words)
{
	uint32_t order;
	uint32_t start;
	uint32_t count;
	uint32_t method;
	unsigned horizontal;
	unsigned vertical;
	bool scaled;

	/* A hidden overlay adds no plane. */
	if (!overlay->shown)
		return 0;

	/* The source rectangle starts that many rows and columns into the image. */
	start = (uint32_t)overlay->frame.physical + overlay->src_y * overlay->frame.pitch + overlay->src_x * 4U;
	order = 2;
	if (overlay->frame.format == 1)
		order = 3;

	/* An unscaled overlay: the same eight words as the cursor's, at the destination and with the source's size. */
	scaled = overlay_scaled(overlay);
	if (!scaled) {
		words[0] = 0x48009807U | (order << 13);
		words[1] = (overlay->dst_y << 16) | overlay->dst_x;
		words[2] = 0x2000fff0U;
		words[3] = (overlay->src_height << 16) | overlay->src_width;
		words[4] = 0xc0c0c0c0U;
		words[5] = 0xc0000000U | start;
		words[6] = 0xc0c0c0c0U;
		words[7] = overlay->frame.pitch;
		return FLIP_PLANE_WORDS;
	}

	/* The method of each axis, and the method field's value for the pair. */
	horizontal = scale_mode(overlay->src_width, overlay->dst_width);
	vertical = scale_mode(overlay->src_height, overlay->dst_height);
	method = 0;
	if (horizontal == FLIP_SCALE_PPF && vertical == FLIP_SCALE_PPF)
		method = 0;
	else if (horizontal == FLIP_SCALE_TPZ && vertical == FLIP_SCALE_PPF)
		method = 1;
	else if (horizontal == FLIP_SCALE_PPF && vertical == FLIP_SCALE_TPZ)
		method = 2;
	else if (horizontal == FLIP_SCALE_TPZ && vertical == FLIP_SCALE_TPZ)
		method = 3;
	else if (horizontal == FLIP_SCALE_PPF && vertical == FLIP_SCALE_NONE)
		method = 4;
	else if (horizontal == FLIP_SCALE_NONE && vertical == FLIP_SCALE_PPF)
		method = 5;
	else if (horizontal == FLIP_SCALE_NONE && vertical == FLIP_SCALE_TPZ)
		method = 6;
	else
		method = 7;

	/* Placement, alpha, the scaled and the source sizes, and the image's rows. */
	words[1] = (overlay->dst_y << 16) | overlay->dst_x;
	words[2] = 0x2000fff0U;
	words[3] = (overlay->dst_height << 16) | overlay->dst_width;
	words[4] = (overlay->src_height << 16) | overlay->src_width;
	words[5] = 0xc0c0c0c0U;
	words[6] = 0xc0000000U | start;
	words[7] = 0xc0c0c0c0U;
	words[8] = overlay->frame.pitch;
	count = 9;

	/* Vertical scaling keeps lines in the overlay's own part of the line buffer. */
	if (vertical != FLIP_SCALE_NONE) {
		words[count] = index * FLIP_LINE_BUFFER_WORDS;
		count++;
	}

	/* The horizontal polyphase step, then the vertical one with its context. */
	if (horizontal == FLIP_SCALE_PPF) {
		words[count] = polyphase_word(overlay->src_width, overlay->dst_width);
		count++;
	}

	/* The vertical polyphase step keeps a context word after it. */
	if (vertical == FLIP_SCALE_PPF) {
		words[count] = polyphase_word(overlay->src_height, overlay->dst_height);
		words[count + 1U] = 0xc0c0c0c0U;
		count += 2U;
	}

	/* The horizontal trapezoidal step, then the vertical one with its context. */
	if (horizontal == FLIP_SCALE_TPZ) {
		trapezoid_words(overlay->src_width, overlay->dst_width, words + count);
		count += 2U;
	}

	/* The vertical trapezoidal step keeps a context word after its two. */
	if (vertical == FLIP_SCALE_TPZ) {
		trapezoid_words(overlay->src_height, overlay->dst_height, words + count);
		words[count + 2U] = 0xc0c0c0c0U;
		count += 3U;
	}

	/* A polyphase step reads the resident filter, once for each of four uses. */
	if (horizontal == FLIP_SCALE_PPF || vertical == FLIP_SCALE_PPF) {
		words[count] = FLIP_FILTER_WORD;
		words[count + 1U] = FLIP_FILTER_WORD;
		words[count + 2U] = FLIP_FILTER_WORD;
		words[count + 3U] = FLIP_FILTER_WORD;
		count += 4U;
	}

	/*
	 * The control word: valid, its word count, byte order, alpha and RGB
	 * expansion, the method for both channels, and the RGB32 pixel format.
	 */
	words[0] = 0x40000000U | (count << 24) | (order << 13) | 0x1000U | 0x800U | (method << 8) | (method << 5) | 7U;

	/* Succeeded: one scaled plane. */
	return count;
}

/* Chooses an axis's scaling: none at the same size, the polyphase filter down to two thirds, the trapezoidal one below. */
static unsigned
scale_mode(
	uint32_t source,
	uint32_t destination)
{
	/* The same size needs no filter. */
	if (source == destination)
		return FLIP_SCALE_NONE;

	/* Any enlargement, and a reduction to two thirds or more. */
	if (3ULL * destination >= 2ULL * source)
		return FLIP_SCALE_PPF;

	/* Succeeded: a stronger reduction. */
	return FLIP_SCALE_TPZ;
}

/*
 * Encodes a polyphase step: the source-to-destination ratio in 16.16 with
 * its lowest bit clear, and the starting phase half a pixel in, corrected by
 * half of the ratio's rounding error, in 6 fractional bits.
 */
static uint32_t
polyphase_word(
	uint32_t source,
	uint32_t destination)
{
	uint32_t ratio;
	int64_t error;
	int32_t phase;

	/* The ratio, even, as the luma channel of a YUV image would use it. */
	ratio = (uint32_t)(((uint64_t)source << 16) / destination);
	ratio &= ~1U;

	/* Half a pixel in, plus half the ratio's error over the whole line. */
	error = ((int64_t)source << 16) - (int64_t)destination * ratio;
	error >>= 10;
	phase = -32 + (int32_t)(error >> 1);
	if (phase >= 64)
		phase = 63;

	/* Automatic gain control, the ratio and the phase's low seven bits. */
	return 0x40000000U | ((ratio << 8) & 0x01ffff00U) | ((uint32_t)phase & 0x7fU);
}

/* Encodes a trapezoidal step: the ratio in 16.16 with phase zero, then its reciprocal's low 16 bits. */
static void
trapezoid_words(
	uint32_t source,
	uint32_t destination,
	uint32_t *words)
{
	uint32_t ratio;

	/* The ratio, above one and a half here. */
	ratio = (uint32_t)(((uint64_t)source << 16) / destination);

	/* The ratio's field, then the reciprocal, which ~0 / ratio approximates closely enough. */
	words[0] = (ratio << 8) & 0x1fffff00U;
	words[1] = (0xffffffffU / ratio) & 0xffffU;
}

/* Tells whether an overlay is shown at another size than its source. */
static bool
overlay_scaled(
	const struct bcm2711_flip_overlay *overlay)
{
	/* A different width or height. */
	if (overlay->src_width != overlay->dst_width)
		return true;
	if (overlay->src_height != overlay->dst_height)
		return true;

	/* Succeeded: unscaled. */
	return false;
}

/* Adds one event of a cadence kind to the window (v3d.perf=1 only); the caller holds the guard and prints nothing. */
static void
note_locked(
	struct bcm2711_display *display,
	unsigned kind,
	uint64_t now)
{
	struct bcm2711_flip_cadence *cadence;
	uint64_t interval;

	/* The interval joins the window's samples while they fit. */
	cadence = &display->flip.cadence[kind];
	if (cadence->last != 0 && cadence->stored < BCM2711_FLIP_CADENCE_SAMPLES) {
		interval = kern_ticks_to_rate(now - cadence->last, 1000000U);
		if (interval > UINT32_MAX)
			interval = UINT32_MAX;
		cadence->intervals[cadence->stored] = (uint32_t)interval;
		cadence->stored++;
	}

	/* The event is the kind's last, and one more of the window. */
	cadence->last = now;
	cadence->count++;

	/* The first event opens the window. */
	if (display->flip.cadence_start == 0)
		display->flip.cadence_start = now;
}

/*
 * Counts what an adopted list moved (v3d.perf=1 only); the caller holds the guard.
 *
 * A list that moved the first overlay counts for the overlay cadence and the
 * pair, and as a pair too when it also moved the cursor; every overlay
 * record the list shows gets its adoption time.
 */
static void
count_adoption_locked(
	struct bcm2711_display *display,
	uint32_t slot)
{
	struct bcm2711_flip_slot *list;
	struct bcm2711_flip_time *time;
	uint64_t now;
	uint32_t index;
	bool window_moved;
	bool cursor_moved;

	/* Nothing is kept without v3d.perf=1 (read at the first note). */
	if (display->flip.cadence_state != BCM2711_FLIP_CADENCE_ON)
		return;

	/* The first overlay's and the cursor's places against the last adopted list. */
	list = &display->flip.slots[slot];
	now = clock_ticks();
	window_moved = false;
	if (list->window_shown != display->flip.last_window_shown ||
	    list->window_x != display->flip.last_window_x ||
	    list->window_y != display->flip.last_window_y)
		window_moved = true;
	cursor_moved = false;
	if (list->cursor_shown != display->flip.last_cursor_shown ||
	    list->cursor_x != display->flip.last_cursor_x ||
	    list->cursor_y != display->flip.last_cursor_y)
		cursor_moved = true;

	/* A moved window is an overlay adoption and a pair candidate. */
	if (window_moved && list->window_shown) {
		note_locked(display, BCM2711_FLIP_CADENCE_OVERLAY, now);
		display->flip.perf.pair_window++;
		if (cursor_moved)
			display->flip.perf.pair_both++;
	}

	/* The places the next adoption compares with. */
	display->flip.last_window_shown = list->window_shown;
	display->flip.last_window_x = list->window_x;
	display->flip.last_window_y = list->window_y;
	display->flip.last_cursor_shown = list->cursor_shown;
	display->flip.last_cursor_x = list->cursor_x;
	display->flip.last_cursor_y = list->cursor_y;

	/* The overlay records this list shows are adopted now. */
	for (index = 0; index < BCM2711_FLIP_TIMES; index++) {
		/* A record of a plane, shown by this list, not yet adopted. */
		time = &display->flip.times[index];
		if (time->plane == 0 || time->adopted != 0 || time->recorded == 0)
			continue;
		if (list->records[time->plane] != time->state_serial)
			continue;
		time->adopted = sched_ticks();
	}
}

/*
 * Copies the settled overlay record times out of the ring and frees their entries; the caller holds the guard.
 *
 * Settled is adopted, or replaced before any adoption (no time, and its
 * number already adopted).  Returns how many adopted ones were copied to
 * times_copy; replaced counts the others.
 */
static uint32_t
take_times_locked(
	struct bcm2711_display *display,
	uint32_t *replaced)
{
	struct bcm2711_flip_time *time;
	uint32_t index;
	uint32_t copied;

	/* Walks the whole ring once. */
	copied = 0;
	*replaced = 0;
	for (index = 0; index < BCM2711_FLIP_TIMES; index++) {
		/* An empty entry, or one whose record is still waiting. */
		time = &display->flip.times[index];
		if (time->recorded == 0)
			continue;
		if (time->adopted == 0 && time->state_serial > display->flip.adopted_serial)
			continue;

		/* A replaced record has no time to print; an adopted one is copied. */
		if (time->adopted == 0) {
			(*replaced)++;
		} else {
			times_copy[copied] = *time;
			copied++;
		}

		/* The entry is free again. */
		kern_memset(time, 0, sizeof(*time));
	}

	/* Succeeded: the number of adopted records copied. */
	return copied;
}

/*
 * Prints the copied overlay record times (v3d.perf=1 only): per record its
 * plane, plane serial and milliseconds from record to adoption, sixteen to
 * a line, after the count of records replaced before any adoption.
 */
static void
report_times(
	uint32_t count,
	uint32_t replaced)
{
	char line[512];
	uint32_t index;
	uint32_t length;
	int written;

	/* A window without overlay records prints nothing. */
	if (count == 0 && replaced == 0)
		return;
	kern_logf("bcm2711: overlay times adopted=%u replaced=%u\n", (unsigned)count, (unsigned)replaced);

	/* Lines of plane:serial:milliseconds. */
	length = 0;
	for (index = 0; index < count; index++) {
		/* One record. */
		written = kern_snprintf(
			line + length,
			sizeof(line) - length,
			" %u:%llu:%llu",
			(unsigned)times_copy[index].plane,
			(unsigned long long)times_copy[index].plane_serial,
			(unsigned long long)kern_ticks_to_ms(times_copy[index].adopted - times_copy[index].recorded));
		if (written > 0 && (uint32_t)written < sizeof(line) - length)
			length += (uint32_t)written;

		/* Sixteen records, or the last, end a line. */
		if ((index + 1U) % 16U == 0 || index + 1U == count) {
			kern_logf("bcm2711: overlay times%s\n", line);
			length = 0;
			line[0] = '\0';
		}
	}
}

/* Tells whether the flip report and the cadence are kept: the boot line is read at the first use. */
static bool
perf_enabled(
	struct bcm2711_display *display)
{
	bool measured;

	/* The boot line is read once. */
	if (display->flip.cadence_state == BCM2711_FLIP_CADENCE_UNREAD) {
		display->flip.cadence_state = BCM2711_FLIP_CADENCE_OFF;
		measured = bcm2711_stage_perf_on();
		if (measured)
			display->flip.cadence_state = BCM2711_FLIP_CADENCE_ON;
	}

	/* Reports v3d.perf=1. */
	if (display->flip.cadence_state != BCM2711_FLIP_CADENCE_ON)
		return false;

	/* Succeeded: the report is kept. */
	return true;
}

/* Prints the copied cadence window: per kind, events per second and the interval minimum, median and maximum. */
static void
report_cadence(
	uint64_t milliseconds)
{
	uint64_t set[4];
	uint64_t move[4];
	uint64_t flip[4];
	uint64_t overlay[4];

	/* Each kind's summary from its own samples. */
	cadence_summary(&cadence_copy[BCM2711_FLIP_CADENCE_SET], milliseconds, set);
	cadence_summary(&cadence_copy[BCM2711_FLIP_CADENCE_MOVE], milliseconds, move);
	cadence_summary(&cadence_copy[BCM2711_FLIP_CADENCE_FLIP], milliseconds, flip);
	cadence_summary(&cadence_copy[BCM2711_FLIP_CADENCE_OVERLAY], milliseconds, overlay);
	kern_logf(
		"bcm2711: cadence ms=%llu per_s/min/med/max_us set=%llu/%llu/%llu/%llu move=%llu/%llu/%llu/%llu flip=%llu/%llu/%llu/%llu overlay=%llu/%llu/%llu/%llu\n",
		(unsigned long long)milliseconds,
		(unsigned long long)set[0], (unsigned long long)set[1], (unsigned long long)set[2], (unsigned long long)set[3],
		(unsigned long long)move[0], (unsigned long long)move[1], (unsigned long long)move[2], (unsigned long long)move[3],
		(unsigned long long)flip[0], (unsigned long long)flip[1], (unsigned long long)flip[2], (unsigned long long)flip[3],
		(unsigned long long)overlay[0], (unsigned long long)overlay[1], (unsigned long long)overlay[2], (unsigned long long)overlay[3]);
}

/*
 * Summarizes one copied kind (summary: events per second, then the interval
 * minimum, median and maximum in microseconds, zero without an interval).
 * The samples are sorted in place.
 */
static void
cadence_summary(
	struct bcm2711_flip_cadence *cadence,
	uint64_t milliseconds,
	uint64_t summary[4])
{
	uint32_t *samples;
	uint32_t value;
	uint32_t index;
	uint32_t position;

	/* The rate counts every event of the window, sampled or not. */
	summary[0] = 0;
	if (milliseconds != 0)
		summary[0] = (uint64_t)cadence->count * 1000U / milliseconds;
	summary[1] = 0;
	summary[2] = 0;
	summary[3] = 0;
	if (cadence->stored == 0)
		return;

	/* Insertion sort of at most the window's sample bound. */
	samples = cadence->intervals;
	for (index = 1; index < cadence->stored; index++) {
		/* Moves each sample left past the larger ones. */
		value = samples[index];
		position = index;
		while (position > 0 && samples[position - 1U] > value) {
			samples[position] = samples[position - 1U];
			position--;
		}
		samples[position] = value;
	}
	summary[1] = samples[0];
	summary[2] = samples[cadence->stored / 2U];
	summary[3] = samples[cadence->stored - 1U];
}
