/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Display lists made from the recorded plane state, within the boot display's established mode. */
#ifndef KERN_DRIVERS_GPU_BCM2711_DISPLAY_FLIP_H
#define KERN_DRIVERS_GPU_BCM2711_DISPLAY_FLIP_H

#include <stdbool.h>
#include <stdint.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#include "drivers/gpu/bcm2711/bcm2711-gpu.h"

/*
 * Three private SRAM lists while the boot console list stays intact: the
 * adopted one, one published but not yet seen adopted, and one that may
 * replace it before the hardware takes it.
 */
#define BCM2711_FLIP_SLOTS 3U

/* The slot index that stands for the boot console list. */
#define BCM2711_FLIP_NO_SLOT 3U

/* The borrowed images waiting for the adoption that lets them go. */
#define BCM2711_FLIP_RETIRING 16U

/* The overlay planes between the primary and the cursor (planes 1 and 2 of a commit). */
#define BCM2711_FLIP_OVERLAYS 2U

/* The primary and the overlays: the planes a commit names. */
#define BCM2711_FLIP_PLANES (1U + BCM2711_FLIP_OVERLAYS)

/* A shared allocation a recorded plane borrows (buffer.h). */
struct bcm2711_buffer;

/*
 * Who may change the planes: the open holding the display lease and the
 * lease, or no open and lease zero while nothing leases the display (the
 * driver's own checks at boot use that).
 */
struct bcm2711_flip_owner {
	const void *session;
	uint64_t lease;
};

/*
 * The cursor plane drawn above a composed frame: an unscaled image of at most
 * 64x64 pixels with premultiplied alpha, in contiguous RAM below one GiB that
 * stays unchanged while a list names it.  x and y place its top-left corner
 * and may lie partly outside the output, which clips it.
 */
struct bcm2711_flip_cursor {
	bool shown;
	uint32_t physical;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	int32_t x;
	int32_t y;
};

/*
 * The primary plane: an unscaled image placed at (x, y) over the console,
 * opaque or with premultiplied alpha.  An opaque image that covers the whole
 * output hides the console, which is then left out of the list.  hold is the
 * borrowed allocation the recorded state keeps (NULL for driver-owned
 * storage); it goes when a later record replaces the plane and a list made
 * after that is adopted.
 */
struct bcm2711_flip_primary {
	bool shown;
	bool premultiplied;
	uint32_t x;
	uint32_t y;
	struct drv_bcm2711_boot_screen frame;
	struct bcm2711_buffer *hold;
};

/*
 * One overlay plane: an unscaled part of an image with premultiplied alpha.
 * frame describes the whole image (physical is its first pixel); the
 * source rectangle inside it is shown at the destination rectangle inside
 * the output.  hold is the borrowed allocation, as for the primary.
 */
struct bcm2711_flip_overlay {
	bool shown;
	struct drv_bcm2711_boot_screen frame;
	struct bcm2711_buffer *hold;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_width;
	uint32_t src_height;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_width;
	uint32_t dst_height;
};

/*
 * One plane's new state in a commit (GPU_DISPLAY_COMMIT): plane 0 is the
 * primary.  image says frame and hold name a new image (the caller's
 * reference); otherwise the plane keeps its image.  recorded is the
 * outcome: false when a later serial of the plane was recorded before
 * (DROPPED), and the caller then keeps its reference.
 */
struct bcm2711_flip_change {
	uint32_t plane;
	uint64_t serial;
	bool shown;
	bool image;
	struct drv_bcm2711_boot_screen frame;
	struct bcm2711_buffer *hold;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_width;
	uint32_t src_height;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_width;
	uint32_t dst_height;
	bool recorded;
};

/*
 * One SRAM list slot: used while the hardware may read the list (the adopted
 * list and the published ones not yet seen adopted), with the state number
 * it was made from and the images it names.  window_x and window_y place
 * the first overlay (window_shown) and cursor_x, cursor_y the cursor
 * (cursor_shown), for the count of lists that move both together.
 * records[p] is the number of the record that last changed plane p.
 */
struct bcm2711_flip_slot {
	bool used;
	uint64_t serial;
	bool has_frame;
	struct drv_bcm2711_boot_screen frame;
	uint32_t cursor_image;
	bool window_shown;
	uint32_t window_x;
	uint32_t window_y;
	bool cursor_shown;
	int32_t cursor_x;
	int32_t cursor_y;
	uint64_t records[BCM2711_FLIP_PLANES];
};

/*
 * One overlay record's times (v3d.perf=1 only): its plane and plane serial,
 * the state number it got, when it was recorded and when a list showing it
 * was adopted (zero until then, and for good when a later record replaced
 * it before any adoption), in scheduler ticks.
 */
#define BCM2711_FLIP_TIMES 128U
struct bcm2711_flip_time {
	uint32_t plane;
	uint64_t plane_serial;
	uint64_t state_serial;
	uint64_t recorded;
	uint64_t adopted;
};

/* A borrowed image replaced by the record numbered serial; it goes once that record's list is adopted. */
struct bcm2711_flip_retiring {
	struct bcm2711_buffer *hold;
	uint64_t serial;
};

/*
 * The intervals between events of one kind in a cadence window (v3d.perf=1
 * only): the time of the last event, the events of the window, and the
 * first intervals of the window in microseconds.
 */
#define BCM2711_FLIP_CADENCE_SAMPLES	256U
struct bcm2711_flip_cadence {
	uint64_t last;
	uint32_t count;
	uint32_t stored;
	uint32_t intervals[BCM2711_FLIP_CADENCE_SAMPLES];
};

/* The kinds of display event whose cadence is measured: cursor sets, cursor moves, adopted flips and adopted lists that moved the first overlay. */
#define BCM2711_FLIP_CADENCE_SET	0U
#define BCM2711_FLIP_CADENCE_MOVE	1U
#define BCM2711_FLIP_CADENCE_FLIP	2U
#define BCM2711_FLIP_CADENCE_OVERLAY	3U
#define BCM2711_FLIP_CADENCE_KINDS	4U

/* Whether the cadence is measured: the boot line is read at the first event. */
#define BCM2711_FLIP_CADENCE_UNREAD	0U
#define BCM2711_FLIP_CADENCE_OFF	1U
#define BCM2711_FLIP_CADENCE_ON		2U

/*
 * The flip report's counts of one window (v3d.perf=1 only), kept under the
 * guard: the stage times of adopted primary flips in ticks, the records of
 * each plane, the lists written, the replacements of a list the hardware had
 * not yet taken, the changes left for the next adoption because two lists
 * were already out, and the underruns.
 */
struct bcm2711_flip_perf {
	uint64_t start;
	uint64_t flips;
	uint64_t clean;
	uint64_t raise;
	uint64_t wait;
	uint64_t primary_records;
	uint64_t cursor_records;
	uint64_t lists;
	uint64_t replaced;
	uint64_t coalesced;
	uint64_t underruns;
	uint64_t overlay_records;
	uint64_t dropped;
	uint64_t pair_window;
	uint64_t pair_both;
};

/*
 * One display's plane state and lists, alive until shutdown.
 *
 * guard protects every field below it against the vblank IRQ and the
 * recording threads.  The recorded state (owner, primary, cursor) is the
 * truth; a list is made from all of it, so a change is a new state number
 * (state_serial) and lists carry the number they were made from.
 * list_serial is the number of the newest list written, adopted_serial the
 * highest number shown by an adopted list (every lower record is then shown
 * or replaced by a shown one).  clock_mutex serializes firmware clock
 * requests, which sleep, and is taken before the guard.
 */
struct bcm2711_flip_state {
	struct spinlock guard;
	struct mutex clock_mutex;
	struct wait_queue adoption_waitq;
	bool initialized;
	bool attached;
	bool uncertain;
	bool restoring;
	const void *owner;
	uint64_t lease;
	bool revoking;
	struct bcm2711_flip_primary primary;
	struct bcm2711_flip_overlay overlays[BCM2711_FLIP_OVERLAYS];
	struct bcm2711_flip_cursor cursor;

	/*
	 * plane_serials[p] is the last serial a commit recorded for plane p (zero
	 * at a lease's start); plane_records[p] the state number of the record
	 * that last changed plane p; planes_generation counts primary and
	 * overlay records, so a commit can tell its checked view is still the
	 * state (cursor records do not move it).
	 */
	uint64_t plane_serials[BCM2711_FLIP_PLANES];
	uint64_t plane_records[BCM2711_FLIP_PLANES];
	uint64_t planes_generation;
	uint64_t state_serial;
	uint64_t list_serial;
	uint64_t adopted_serial;
	uint32_t active_list;
	uint32_t active_slot;
	uint32_t pending[2];
	uint32_t pending_count;
	bool console_pending;
	struct bcm2711_flip_slot slots[BCM2711_FLIP_SLOTS];
	struct bcm2711_flip_retiring retiring[BCM2711_FLIP_RETIRING];
	uint32_t retiring_count;
	uint64_t frame_sequence;
	uint64_t adopted_ticks;
	uint32_t core_hz;
	int clock_error;
	struct bcm2711_flip_perf perf;

	/*
	 * The ring of overlay record times and the next entry to write, and the
	 * first overlay's and the cursor's places in the last adopted list.
	 */
	struct bcm2711_flip_time times[BCM2711_FLIP_TIMES];
	uint32_t times_next;
	bool last_window_shown;
	uint32_t last_window_x;
	uint32_t last_window_y;
	bool last_cursor_shown;
	int32_t last_cursor_x;
	int32_t last_cursor_y;

	/*
	 * The cursor images set in the window and those that needed their rows
	 * copied (the others were shown again unchanged); added atomically by
	 * the display device under its plane mutex, read and cleared by the report.
	 */
	uint64_t perf_cursor_sets;
	uint64_t perf_cursor_copies;

	/*
	 * The cadence of display events, kept under guard; reporting
	 * says one caller is printing the copied window, so no other starts.
	 */
	unsigned cadence_state;
	uint64_t cadence_start;
	bool cadence_reporting;
	struct bcm2711_flip_cadence cadence[BCM2711_FLIP_CADENCE_KINDS];
	bool perf_reporting;
};

/* A caller-owned copy of the state's progress and of what the lists name. */
struct bcm2711_flip_status {
	bool attached;
	bool uncertain;
	bool restoring;
	uint32_t active_list;
	uint32_t pending_count;
	uint32_t used_mask;
	uint32_t retiring_count;
	uint64_t frame_sequence;
	uint64_t state_serial;
	uint64_t list_serial;
	uint64_t adopted_serial;
	uint32_t core_hz;
	int clock_error;
	struct bcm2711_flip_primary primary;
	struct bcm2711_flip_overlay overlays[BCM2711_FLIP_OVERLAYS];
	struct drv_bcm2711_boot_screen frames[BCM2711_FLIP_SLOTS];
};

/* The driver keeps this owner and its mapped registers alive throughout use. */
struct bcm2711_display;

/* Initialization precedes IRQ publication; it must never reset a live owner. */
void bcm2711_display_flip_init(struct bcm2711_display *display);

/* Attachment requires a successful R0 and sole ownership of HVS channel zero. */
int bcm2711_display_flip_attach(struct bcm2711_display *display);

/* Registration unwind may detach only at the confirmed, unheld console list. */
int bcm2711_display_flip_detach(struct bcm2711_display *display);

/* Gives the planes to a new lease, with the cursor hidden. */
void bcm2711_display_flip_own(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner);

/* Starts ending a lease: later records fail and waiters return; EINVAL when the lease is not the owner's. */
int bcm2711_display_flip_revoke(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner);

/* Gives the planes back to the driver once the console is restored. */
void bcm2711_display_flip_disown(struct bcm2711_display *display);

/* Tells whether an owner may change the planes now: 0, or EBUSY. */
int bcm2711_display_flip_owned(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner);

/*
 * Records the primary plane and returns the record's number without waiting.
 * On success the state takes the caller's reference in primary->hold.
 * May sleep for the clock; never call from IRQ or while holding a lock.
 */
int bcm2711_display_flip_record(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner, const struct bcm2711_flip_primary *primary, uint64_t *serial);

/*
 * Waits until a list showing the record numbered serial, or a later one, is
 * adopted (at most 100 ms, ETIMEDOUT then, and the borrowed images stay
 * until the console is restored); EBUSY when the lease ends meanwhile.
 */
int bcm2711_display_flip_wait(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner, uint64_t serial);

/*
 * Records the planes of a commit at once and returns the record's number
 * (the state's number when nothing was recorded) and the adopted number,
 * without waiting.  On success the state takes the reference of every
 * recorded change's hold; ENOSPC when the planes would exceed the output's
 * bandwidth or the overlays' limits, and then nothing changed.
 */
int bcm2711_display_flip_commit(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner, struct bcm2711_flip_change *changes, uint32_t count, uint64_t *serial, uint64_t *adopted);

/*
 * Waits until the record numbered serial is adopted (GPU_DISPLAY_WAIT): at
 * most timeout ticks, interruptibly; EAGAIN with no time and no adoption.
 * A timeout leaves nothing uncertain.  Reports the adopted number and the
 * time of the last adoption.
 */
int bcm2711_display_flip_wait_number(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner, uint64_t serial, uint64_t timeout, uint64_t *adopted, uint64_t *adopted_ticks);

/* Records a full-screen opaque frame of the driver's own and waits for its adoption. */
int bcm2711_display_flip_present(struct bcm2711_display *display, const struct drv_bcm2711_boot_screen *frame);

/* Records an unscaled frame of the driver's own over the console and waits for its adoption. */
int bcm2711_display_flip_compose(struct bcm2711_display *display, const struct drv_bcm2711_boot_screen *frame, uint32_t x, uint32_t y, bool premultiplied);

/*
 * Records the cursor and shows it from a coming refresh boundary without
 * waiting.  The image must stay unchanged while
 * bcm2711_display_flip_cursor_named says it is named.
 */
int bcm2711_display_flip_cursor(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner, const struct bcm2711_flip_cursor *cursor);

/* Counts one cursor image set for the flip report, copied or shown again unchanged. */
void bcm2711_display_flip_count_cursor(struct bcm2711_display *display, bool copied);

/*
 * Notes the time of one display event of a cadence kind, and prints the
 * cadence window every two seconds; nothing without v3d.perf=1.  The
 * caller holds no spinlock.
 */
void bcm2711_display_flip_note(struct bcm2711_display *display, unsigned kind);

/* Adds an event (an adopted primary flip with its stage times, or a commit) to the flip report, printing a full window. */
void bcm2711_display_flip_report(struct bcm2711_display *display, bool flip, uint64_t clean, uint64_t raise, uint64_t wait);

/* Tells whether a list the hardware may read or the recorded cursor names a cursor image. */
bool bcm2711_display_flip_cursor_named(struct bcm2711_display *display, uint32_t physical);

/* How a frame's storage is named: not at all, only by lists the hardware may read, or by the recorded primary. */
#define BCM2711_FLIP_FRAME_FREE		0U
#define BCM2711_FLIP_FRAME_LISTED	1U
#define BCM2711_FLIP_FRAME_RECORDED	2U
unsigned bcm2711_display_flip_frame_use(struct bcm2711_display *display, uint64_t physical, uint64_t bytes);

/* Waits, at most 100 ms, until no list the hardware may read names the storage. */
int bcm2711_display_flip_wait_unlisted(struct bcm2711_display *display, const struct bcm2711_flip_owner *owner, uint64_t physical, uint64_t bytes);

/* Lets go of the borrowed images whose replacement has been adopted; thread context only. */
void bcm2711_display_flip_reap(struct bcm2711_display *display);

/* Only confirmed boot-console adoption releases buffers after uncertain DMA. */
int bcm2711_display_flip_restore(struct bcm2711_display *display);
void bcm2711_display_flip_snapshot(struct bcm2711_display *display, struct bcm2711_flip_status *status);

/* Only the selected PV service calls this, with flip.guard held after W1C. */
void bcm2711_display_flip_vblank_locked(struct bcm2711_display *display, uint32_t current);

#endif
