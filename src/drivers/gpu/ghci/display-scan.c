/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scan-out side of a GHCI device's display: display 0 of a GPU with
 * CAP_DISP, driven only by the display commands of the primary ring.
 *
 * The command processor takes a display command off the ring and puts
 * it into one of the display's three service queues (control, the
 * primary plane, the cursor plane), which run it later: a fence after a
 * display command says that it was queued, not that it ran.  What a
 * command did is read from memory: a control command writes a 64-bit
 * completion the driver names (the driver's value, the queue's status,
 * and bit 63 when a reset cancelled it), a FLIP writes its 16-byte record
 * {value, frame count, timestamp} once the image is taken or the flip
 * cancelled (bit 31 of the value), and the display's events (a sink's
 * connection, an underflow, a failed read) go to an event ring.  Each of
 * them may raise IRQ_STATUS[3], which wakes the waiters and the service
 * thread.  The control queue starts a command only after everything
 * before it of the display has finished, so a control command's
 * completion also says that every earlier flip's record was written.
 *
 * Every placement passes the gate of the ring with the display's lock
 * held and does not wait for a recovery: a run the gate refuses is not
 * recorded and is placed again once the recovery has ended.  The service
 * thread's placements wait for nothing at all, as its hang check is what
 * ends every other wait: it only tries the locks, and enters the ring
 * only when it is free and has room for the run at once.  The driver
 * places a plane's next flip only once the last one's record has come;
 * the presents in between wait in a queue of two.  A flip value is 31
 * bits, and the record slot is cleared before each flip, so a record
 * names its flip without any wrap.
 *
 * Lighting the display on an image is a run: DISP_SET with ENABLE off
 * when it is lit, DISP_SET_MODE when the display holds another mode, the
 * FLIP (taken at once while ENABLE is off) and DISP_SET with ENABLE.  An
 * image of the mode lit is only a FLIP, also when its format or rows
 * differ.  A lease ends by marking it ended; the display is then turned
 * dark by the ring, and the memory it scanned is let go once the
 * cancelled records and the dark command's completion have come.  The
 * host has no register of the display: when the ring does not run, the
 * display keeps scanning what it scanned, and that memory goes to the
 * quarantine.
 *
 * A reset of the command processor cancels the display commands still
 * queued (their completions and records say so) and loses the ones still
 * in the ring (nothing is written for them); a flip already taken by the
 * display is still shown.  After the reset the driver places a snapshot
 * (DISP_QUERY_STATE): once it completes, every flip that ran has written
 * its record, a flip still without one was lost and is placed again, and
 * the snapshot says whether the display is lit.  The event ring, which
 * the reset turned off, is set up again.
 *
 * A boot display the firmware left running keeps being scanned until the
 * driver's own first command takes it over; its framebuffer then goes
 * back to the allocator, unless something of the kernel may still draw to
 * it.  While the boot image is kept, the detach shows it again.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/text-display.h>
#include <uapi/errno.h>

#include "display-scan.h"
#include "ghci.h"
#include "ghci-cmd.h"

/* The display the driver drives: display 0. */
#define DISPLAY_INDEX			0U

/*
 * The display's page of host memory: the event ring (32 records), the
 * mode descriptor of a lighting, the snapshot, the completions (one
 * 8-byte word per kind of command), the two planes' flip records, the
 * DDC buffer (its header and one EDID block) and the answer of the mode
 * table (its header and GHCI_DISPLAY_MODES_MAX descriptors).
 */
#define PAGE_BYTES			4096U
#define EVENTS_OFFSET			0x000U
#define EVENTS_CAPACITY			32U
#define MODE_OFFSET			0x400U
#define STATE_OFFSET			0x440U
#define COMP_OFFSET			0x4C0U
#define RECORD_OFFSET			0x500U
#define I2C_OFFSET			0x540U
#define MODES_OFFSET			0x600U

/* The kinds of command whose completion the driver waits for: each has its word in the page. */
#define COMP_LIGHT			0U
#define COMP_DARK			1U
#define COMP_STATE			2U
#define COMP_EVENTS			3U
#define COMP_MODES			4U
#define COMP_I2C			5U

/* What a completion says: not yet written, written, or written as cancelled by a reset. */
#define COMP_PENDING			0
#define COMP_DONE			1
#define COMP_CANCELLED			2

/* What the event ring does: nothing, its setup placed, or running. */
#define EVENTS_OFF			0
#define EVENTS_PLACED			1
#define EVENTS_RUNNING			2

/*
 * How long the driver waits: for a command at the start or the detach,
 * for the records still due at the stop, for the detach's last flips.
 */
#define COMMAND_WAIT_MS			200U
#define RECORD_WAIT_MS			100U
#define DETACH_WAIT_MS			200U

/* How often a waiter looks at the page when no interrupt comes. */
#define FLIP_POLL_MS			10U

/*
 * The scan-out errors: past ERROR_LIMIT error events in a second the
 * error events are left out of the event ring for the rest of that
 * second, and READ_ERROR_RUNS rounds in a row with a read error turn the
 * display dark.
 */
#define ERROR_PERIOD_MS			1000U
#define ERROR_LIMIT			30U
#define READ_ERROR_RUNS			3U

/* The events the driver takes: the connection (when the display senses it) and the errors. */
#define EVENTS_ERRORS			(GHCI_DISP_EVENT_MASK_UNDERFLOW | GHCI_DISP_EVENT_MASK_ERROR)

/* The dwords of the run that lights the display: two DISP_SETs, a mode, the image's flip and the cursor's. */
#define LIGHT_DWORDS			(2U * GHCI_DW_DISP_SET + GHCI_DW_DISP_SET_MODE + 2U * GHCI_DW_FLIP)

/*
 * The EDID of a sink, read through the DDC bus: the base block at the
 * 7-bit address 0x50, its 8-byte header, and its four 18-byte detailed
 * timing descriptors from byte 54.
 *
 * Values referenced: VESA E-EDID standard release A2 (section 3.1, the
 * 128-byte base block and its header; section 3.10, the detailed timing
 * descriptors and their fields).
 */
#define EDID_ADDRESS			0x50U
#define EDID_BYTES			128U
#define EDID_TIMINGS			54U
#define EDID_TIMING_BYTES		18U
#define EDID_TIMING_COUNT		4U

/*
 * The modes every sink takes, offered when the display takes arbitrary
 * timings and no sink's EDID names one: VESA DMT 640x480, 800x600,
 * 1024x768, 1280x720 and 1920x1080 at 60 Hz, each as its clock in kHz,
 * the active, sync start, sync end and total of each axis, and the sync
 * polarities.
 *
 * Values referenced: VESA DMT version 1.0 revision 13 (the 640x480,
 * 800x600, 1024x768, 1280x720 and 1920x1080 timings at 60 Hz);
 * lumiere-gpu 875e54b3 ghci.md (section 13.3, 640x480 at 60 Hz with a
 * 25175 kHz clock as the boot display's fallback).
 */
#define STANDARD_MODES			5U
static const uint32_t standard_modes[STANDARD_MODES][10] = {
	{ 25175U, 640U, 656U, 752U, 800U, 480U, 490U, 492U, 525U, 0U },
	{ 40000U, 800U, 840U, 968U, 1056U, 600U, 601U, 605U, 628U,
	  GHCI_DISP_MODE_HSYNC_POSITIVE | GHCI_DISP_MODE_VSYNC_POSITIVE },
	{ 65000U, 1024U, 1048U, 1184U, 1344U, 768U, 771U, 777U, 806U, 0U },
	{ 74250U, 1280U, 1390U, 1430U, 1650U, 720U, 725U, 730U, 750U,
	  GHCI_DISP_MODE_HSYNC_POSITIVE | GHCI_DISP_MODE_VSYNC_POSITIVE },
	{ 148500U, 1920U, 2008U, 2052U, 2200U, 1080U, 1084U, 1089U, 1125U,
	  GHCI_DISP_MODE_HSYNC_POSITIVE | GHCI_DISP_MODE_VSYNC_POSITIVE }
};

static uint8_t *page_view(struct ghci_device *device, uint32_t offset);
static uint64_t page_bus(struct ghci_device *device, uint32_t offset);
static uint64_t page_read64(struct ghci_device *device, uint32_t offset);
static void comp_arm(struct ghci_device *device, unsigned slot, int irq, struct ghci_disp_comp *comp);
static int comp_state(struct ghci_device *device, unsigned slot, uint32_t *status, uint64_t *value);
static int command_wait(struct ghci_device *device, unsigned slot, uint32_t *status, uint64_t *value);
static int command_run(struct ghci_device *device, const uint32_t *packets, uint32_t dwords, unsigned slot, uint32_t *status, uint64_t *value);
static int page_take(struct ghci_device *device);
static int events_start(struct ghci_device *device);
static int state_read(struct ghci_device *device);
static int modes_read(struct ghci_device *device);
static int modes_fixed(struct ghci_device *device);
static void modes_edid(struct ghci_device *device);
static int edid_read(struct ghci_device *device, uint8_t *edid);
static void edid_timing(struct ghci_device *device, const uint8_t *timing);
static void modes_standard(struct ghci_device *device);
static void mode_add(struct ghci_display *display, const struct ghci_display_mode *mode);
static int mode_fits(const struct ghci_device *device, const struct ghci_display_mode *mode);
static void mode_refresh(struct ghci_display_mode *mode);
static void mode_read(const uint8_t *desc, struct ghci_display_mode *mode);
static void mode_write(const struct ghci_display_mode *mode, uint8_t *desc);
static int mode_same(const struct ghci_display_mode *first, const struct ghci_display_mode *second);
static void boot_take(struct ghci_device *device);
static void boot_mode_take(struct ghci_device *device, const uint8_t *state);
static void image_clear(struct ghci_display_image *image);
static uint64_t record_address(struct ghci_device *device, unsigned plane);
static volatile uint32_t *record_slot(struct ghci_device *device, unsigned plane);
static uint32_t value_take(struct ghci_display *display);
static int ring_put(struct ghci_device *device, const uint32_t *packets, uint32_t dwords, int *placed);
static void records_take(struct ghci_device *device);
static void taken(struct ghci_device *device, unsigned plane, int cancelled);
static void light_check(struct ghci_device *device);
static void queue_kick(struct ghci_device *device);
static void queue_push_front(struct ghci_display *display, const struct ghci_display_image *image);
static void queue_pop(struct ghci_display *display);
static uint32_t flip_packet(struct ghci_device *device, uint32_t *out, const struct ghci_display_image *image, int surface, uint32_t value);
static int light(struct ghci_device *device, const struct ghci_display_image *image);
static int flip_primary(struct ghci_device *device, const struct ghci_display_image *image);
static int flip_cursor(struct ghci_device *device, int surface);
static void cursor_kick(struct ghci_device *device);
static void cursor_state_kick(struct ghci_device *device);
static void dark(struct ghci_device *device);
static void host_cannot_turn(struct ghci_device *device);
static void flips_forget(struct ghci_device *device);
static void resync_serve(struct ghci_device *device);
static void resync_end(struct ghci_device *device);
static void events_serve(struct ghci_device *device);
static void events_setup(struct ghci_device *device, uint32_t mask);
static int events_take(struct ghci_device *device, int *changed);
static void event_handle(struct ghci_device *device, uint32_t type, uint32_t payload, int *changed);
static int events_stop(struct ghci_device *device);
static void scan_release(struct ghci_device *device, struct ghci_allocation *memory);
static void scan_flush(struct ghci_device *device, int service);
static void boot_end(struct ghci_device *device);
static int boot_image(const struct ghci_display *display, const struct ghci_display_image *image);
static int boot_in_use(const struct ghci_display *display);
static void detach_begin(struct ghci_device *device);
static int detach_step(struct ghci_device *device, int *turned);
static void buffers_give_back(struct ghci_device *device, int unsure);
static void errors_serve(struct ghci_device *device);
static void topology_move(struct ghci_device *device);
static unsigned idle_mode(const struct ghci_display *display);
static void idle_serve(struct ghci_device *device);
static int idle_frame_take(struct ghci_device *device);
static void scan_give_back(struct ghci_device *device, struct ghci_allocation *memory, int unsure);
static uint32_t load16(const uint8_t *bytes, uint32_t offset);
static uint32_t load32(const uint8_t *bytes, uint32_t offset);
static void store16(uint8_t *bytes, uint32_t offset, uint32_t value);
static void store32(uint8_t *bytes, uint32_t offset, uint32_t value);

/*
 * Prepares the display's record before the command processor runs, and
 * takes over a boot display the firmware left running (from the
 * BOOT_DISP window read at the start, before any display command).  The
 * display is started once the ring runs (ghci_display_start()).
 *
 * Returns 0.
 */
int
ghci_display_attach(
	struct ghci_device *device)
{
	struct ghci_display *display;

	/* The record starts empty, with its wait queue. */
	display = &device->display;
	kern_memset(display, 0, sizeof(*display));
	waitq_init(&display->flip_wake, "ghci flip");
	image_clear(&display->shown);
	image_clear(&display->boot);
	image_clear(&display->light_image);
	display->mode_set = GHCI_DISPLAY_NO_MODE;

	/* A boot display the firmware left running keeps showing its framebuffer. */
	boot_take(device);

	/* Succeeded: the display starts once the ring runs. */
	return 0;
}

/*
 * Starts the display of a device whose capability block says CAP_DISP,
 * once the ring runs: its page, the event ring, a snapshot (the sink's
 * connection and a boot display's mode), the modes (the fixed ones the
 * display names, or the sink's EDID and the standard ones), the idle
 * frame, and its interrupt.  A device without a display, or whose
 * display does not answer, is left without one (display.present 0).
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 13.1, word 23
 * and DISPLAY_COUNT; section 13.2, the service queues and the event
 * ring; section 9.2, IRQ_STATUS[3] for the completions, the records and
 * the events).
 *
 * Returns 0.
 */
int
ghci_display_start(
	struct ghci_device *device)
{
	struct ghci_display *display;
	unsigned long irq;
	uint32_t count;
	int has_display;
	int error;

	/* A device without CAP_DISP has no display. */
	display = &device->display;
	has_display = ghci_caps_has(&device->caps, GHCI_HAS_DISPLAY);
	if (!has_display)
		return 0;

	/* What the display can do, from word 23; it names at least one display. */
	display->info = device->caps.disp_info;
	count = (display->info >> GHCI_DISP_INFO_COUNT_SHIFT) & GHCI_DISP_INFO_COUNT_MASK;
	if (count == 0U) {
		kern_logf("ghci: %s: CAP_DISP without a display (word 23 %08x); no display\n", device->name, (unsigned)display->info);
		return 0;
	}
	if ((display->info & GHCI_DISP_INFO_HAS_CURSOR) != 0U)
		display->cursor = 1;
	if ((display->info & GHCI_DISP_INFO_HAS_SENSE) != 0U)
		display->sense = 1;
	if ((display->info & GHCI_DISP_INFO_HAS_I2C) != 0U)
		display->i2c = 1;

	/* The page the display's commands write to. */
	error = page_take(device);
	if (error != 0) {
		kern_logf("ghci: %s: no memory for the display (%d); no display\n", device->name, error);
		return 0;
	}

	/* The event ring, then the snapshot: the connection, and a boot display's mode. */
	error = events_start(device);
	if (error == 0)
		error = state_read(device);
	if (error != 0) {
		kern_logf("ghci: %s: display does not answer (%d); no display\n", device->name, error);
		scan_give_back(device, display->page, 1);
		display->page = NULL;
		return 0;
	}

	/* The modes. */
	error = modes_read(device);
	if (error != 0) {
		kern_logf("ghci: %s: display names no usable mode; no display\n", device->name);
		(void)events_stop(device);
		scan_give_back(device, display->page, 0);
		display->page = NULL;
		return 0;
	}

	/* A boot display that kept running is in a mode of the table or the boot slot; a dark one has none. */
	if (!display->boot_scanning) {
		display->mode = ghci_scan_largest_mode(display);
		display->format = GHCI_FORMAT_B8G8R8A8_UNORM;
	}

	/* The idle frame, without which the display only lights for a lease. */
	error = idle_frame_take(device);
	if (error != 0)
		kern_logf("ghci: %s: no idle frame for the display (%d)\n", device->name, error);

	/* The display exists, without a lease, at its first generation and topology. */
	display->generation = GHCI_DISPLAY_FIRST_GENERATION;
	display->topology = 1U;
	display->next_lease = 1U;
	display->next_value = 1U;
	display->cursor_shown = -1;
	display->cursor_pending = -1;
	display->copy_frame_busy = -1;
	display->error_tick = clock_ticks();
	display->present = 1;

	/* Its completions, records and events raise IRQ_STATUS[3] from now. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->irq_mask |= GHCI_IRQ_DISPLAY;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	ghci_cp_irq_enable(device);

	/* Succeeded: the display is ready for a lease. */
	kern_logf("ghci: %s: display %u of %u, %u modes%s%s%s\n",
	    device->name,
	    DISPLAY_INDEX,
	    (unsigned)count,
	    display->mode_count,
	    display->cursor ? ", cursor" : "",
	    display->connected ? ", connected" : "",
	    display->boot_scanning ? ", boot display kept" : "");
	return 0;
}

/*
 * Turns the display for the detach: the lease's images go, the display
 * shows the boot image again while it is kept, or goes dark, and its
 * event ring stops.  The ring does it; when it does not run, the display
 * keeps scanning what it scanned, and that memory goes to the
 * quarantine.  The display's memory goes back.  The caller holds no lock
 * of the device; the detach waits a bounded time.
 */
void
ghci_display_detach(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint64_t deadline;
	uint64_t mark;
	uint64_t now;
	unsigned index;
	int turned;
	int unsure;
	int error;
	int done;

	/* A device without a display has nothing to turn. */
	display = &device->display;
	if (!display->present)
		return;

	/* The lease and what waits go. */
	mutex_lock(&device->display_lock);

	detach_begin(device);

	mutex_unlock(&device->display_lock);

	/*
	 * Waits a bounded time for the flips placed before, then turns the
	 * display (the boot image again, or dark) and waits for that.
	 */
	turned = 0;
	deadline = clock_ticks() + kern_ms_to_ticks(DETACH_WAIT_MS);
	for (;;) {
		mark = ghci_scan_mark(device);
		mutex_lock(&device->display_lock);

		done = detach_step(device, &turned);

		mutex_unlock(&device->display_lock);

		/* Ends once the display shows what it should, or at the deadline. */
		now = clock_ticks();
		if (done || now >= deadline)
			break;

		ghci_scan_sleep(device, mark, deadline);
	}

	/* What the ring did not finish stays as it is: what the display may still read goes to the quarantine. */
	mutex_lock(&device->display_lock);

	if (!done) {
		kern_logf("ghci: %s: display not turned by the ring; its memory is quarantined\n", device->name);
		host_cannot_turn(device);
		scan_release(device, display->shown.memory);
		image_clear(&display->shown);
	}

	/* Presents a recovery put back meanwhile are not shown. */
	for (index = 0U; index < display->queue_count; index++)
		ghci_scan_drop(device, display->queue[index].memory);
	display->queue_count = 0U;
	display->ending = 0;

	mutex_unlock(&device->display_lock);

	/* The event ring stops, so that nothing writes the page once it goes; a display not turned may still write it. */
	unsure = 1;
	if (done) {
		error = events_stop(device);
		if (error == 0)
			unsure = 0;
	}

	/* No display interrupt is taken any more. */
	mutex_lock(&device->display_lock);

	display->present = 0;

	mutex_unlock(&device->display_lock);

	/* The memory the display held, and its own buffers (to the quarantine when the display may still read them). */
	ghci_scan_flush(device);
	buffers_give_back(device, unsure);
}

/*
 * Ends a closing session's lease.  Only the mark is made here: the
 * service thread turns the display dark.  The caller holds no lock of
 * the device.
 */
void
ghci_display_session_close(
	struct ghci_device *device,
	struct ghci_session *session)
{
	struct ghci_display *display;
	int ended;

	/* Only the lease's holder has anything on the display. */
	display = &device->display;
	if (!display->present)
		return;

	/* The lease ends with its session; nothing waits for the ring here. */
	mutex_lock(&device->display_lock);

	ended = 0;
	if (display->owner == session) {
		display->owner = NULL;
		display->lease = 0U;
		display->ending = 1;
		display->dark_placed = -1;
		ended = 1;
	}

	mutex_unlock(&device->display_lock);

	/* The service thread turns the display dark and lets the memory go. */
	if (ended)
		ghci_service_wake(device, GHCI_WORK_DISPLAY);
}

/*
 * Does the display's part of a round of the service thread: the records,
 * the snapshot after a reset, the end of a lease, the events, the
 * scan-out errors and the connection.  The caller holds no lock of the
 * device.
 */
void
ghci_display_service(
	struct ghci_device *device,
	unsigned work)
{
	struct ghci_display *display;
	unsigned long irq;
	uint64_t adopted;
	uint64_t topology;
	int changed;
	int locked;

	/* A device without a display has nothing to serve. */
	(void)work;
	display = &device->display;
	if (!display->present)
		return;

	/*
	 * The display's lock is only tried: its holder may be waiting for
	 * the ring's room, which only this thread's hang check ends.  A round
	 * that does not get it leaves the events for the next.
	 */
	locked = mutex_trylock(&device->display_lock);
	if (!locked)
		return;

	/* Everything places only into a free ring with room at once; nothing here waits for the ring or a record. */
	display->service_placing = 1;
	adopted = display->adopted;

	/* The snapshot a reset asked for, and what it settles. */
	resync_serve(device);

	/* A lease a closed session ended goes dark now, and is finished once it let go. */
	if (display->ending && display->dark_placed < 0)
		ghci_scan_lease_end(device);
	if (display->ending)
		(void)ghci_scan_lease_done(device);

	/* The events (the connection, the errors), the event ring's setup, and the errors of this second. */
	changed = 0;
	events_serve(device);
	(void)events_take(device, &changed);
	errors_serve(device);

	/* The idle frame, the records and what waits. */
	idle_serve(device);
	ghci_scan_update(device);
	if (display->adopted != adopted)
		changed = 1;
	display->service_placing = 0;

	mutex_unlock(&device->display_lock);

	/* The memory let go of when the locks are free. */
	scan_flush(device, 1);

	/* A round that changed nothing has nobody to tell. */
	if (!changed)
		return;

	/* The sequence the display-event fences are compared against, read as the events operation reads it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	topology = display->topology;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* The display-event fences the sequence passed, and the waiters on the display's events. */
	drv_gpu_display_changed(device->gpu, topology);
}

/*
 * Takes the display's interrupt in the interrupt handler, under
 * irq_lock: what it says is in the page (a completion, a record, an
 * event), which the service thread reads; the handler clears
 * IRQ_STATUS[3] and wakes the flip waiters.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 9.2,
 * IRQ_DISPLAY for a completion, a flip record or an event, cleared by
 * writing 1 to IRQ_STATUS[3]).
 */
void
ghci_display_interrupt(
	struct ghci_device *device)
{
	/* A device without a display raises no display interrupt. */
	if (!device->display.present)
		return;

	/* The service thread reads the page at its next round. */
	device->service_work |= GHCI_WORK_DISPLAY;
}

/*
 * Settles the display after a reset of the command processor, while it
 * is idle; cp_lock and the display's lock are held.  The records that
 * came are taken; a command still queued was cancelled (its completion
 * or record says so) and one still in the ring was lost (nothing comes
 * of it), which only a snapshot placed once the ring runs again tells
 * apart (resync_serve()).  The event ring the reset turned off is set up
 * again, and the cursor's place placed again.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2, the
 * lifecycle matrix: CTRL.RESET cancels the commands queued with their
 * cancel writes and turns the event ring off, the flips the display
 * already took stay; section 10, the scan-out continues through a
 * reset).
 */
void
ghci_display_recover_count(
	struct ghci_device *device)
{
	struct ghci_display *display;

	/* A device without a display has nothing to count. */
	display = &device->display;
	if (!display->present)
		return;

	/* The records that came. */
	records_take(device);

	/* A lighting still placed may have been cut: its image is remembered for the snapshot. */
	display->light_lost = 0;
	if (display->flips[0].placed && display->flips[0].light) {
		display->light_lost = 1;
		display->light_image = display->flips[0].image;
	}

	/* The snapshot settles the rest once the ring runs; the event ring and the cursor's place are set again. */
	display->resync = -1;
	display->events_state = EVENTS_OFF;
	display->cursor_due = 1;
	display->comp_value[COMP_LIGHT] = 0U;
}

/*
 * Reports whether a panic's console is on the display: the boot image
 * the console draws to is still scanned.  The host has no register of
 * the display and a panic may not place commands, so nothing is turned
 * here.  Returns 0 when the console's image shows, or ENOENT.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2, a
 * panic draws to the image the display scans, in host memory or through
 * the open local window).
 */
int
ghci_display_panic(
	struct ghci_device *device)
{
	struct ghci_display *display;

	/* Only a kept boot image shows what the console drew. */
	display = &device->display;
	if (!display->present || !display->boot_kept)
		return ENOENT;

	/* The display scans something else. */
	if (!display->boot_scanning)
		return ENOENT;

	/* Succeeded: the display scans the console's image. */
	return 0;
}

/*
 * Waits a bounded time, once the stop's reset left the processor idle,
 * for the records of the flips the display took before it, which are
 * written after the reset, so that the page they go to may be given back.
 * Returns 0, also for a device without a display, or ETIMEDOUT while a
 * record may still come.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2, the
 * records of the flips a CTRL.RESET leaves to be taken are written after
 * the reset).
 */
int
ghci_display_records_settle(
	struct ghci_device *device)
{
	struct ghci_display *display;
	unsigned waited;

	/* A device without a display, or one that took nothing, owes no record. */
	display = &device->display;
	if (!display->present || display->page == NULL)
		return 0;

	/* The records, looked at every poll until none is owed. */
	for (waited = 0U; waited < RECORD_WAIT_MS; waited += FLIP_POLL_MS) {
		records_take(device);
		if (!display->flips[0].placed && !display->flips[1].placed)
			return 0;
		ghci_sleep_ms(FLIP_POLL_MS);
	}

	/* A record may still come. */
	return ETIMEDOUT;
}

/*
 * Reports the index of the display's largest mode, by its pixels.
 */
unsigned
ghci_scan_largest_mode(
	const struct ghci_display *display)
{
	uint64_t pixels;
	uint64_t largest_pixels;
	unsigned largest;
	unsigned index;

	/* The first of the modes with the most pixels. */
	largest = 0U;
	largest_pixels = 0U;
	for (index = 0U; index < display->mode_count; index++) {
		pixels = (uint64_t)display->modes[index].width * display->modes[index].height;
		if (pixels > largest_pixels) {
			largest = index;
			largest_pixels = pixels;
		}
	}

	/* Succeeded: the largest mode. */
	return largest;
}

/*
 * Reports whether the ring can take the display's packets now: a device
 * that runs or recovers (the gate makes a placement wait for the end of
 * a recovery), not one that is lost or stopped.
 */
int
ghci_scan_ring_usable(
	struct ghci_device *device)
{
	enum ghci_device_state state;
	unsigned long irq;

	/* The device's life, as the recovery last set it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	state = device->state;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A device that runs, or recovers. */
	if (state == GHCI_STATE_RUNNING)
		return 1;
	if (state == GHCI_STATE_RECOVERING)
		return 1;
	if (state == GHCI_STATE_REPLAYING)
		return 1;

	/* A device starting, lost or stopped. */
	return 0;
}

/*
 * Reports the bytes a frame of every mode fits in: the most, over the
 * modes, of the rows on the line times the lines.
 */
uint64_t
ghci_scan_frame_bytes(
	const struct ghci_display *display)
{
	uint64_t bytes;
	uint64_t most;
	unsigned index;

	/* Each mode's frame, the largest kept. */
	most = 0U;
	for (index = 0U; index < display->mode_count; index++) {
		bytes = (uint64_t)ghci_scan_stride(display->modes[index].width) * display->modes[index].height;
		if (bytes > most)
			most = bytes;
	}

	/* Succeeded: the bytes. */
	return most;
}

/*
 * Reports the bytes between the rows of a frame of a width: whole pixels,
 * rounded up to the line, as the display scans them.
 */
uint32_t
ghci_scan_stride(
	uint32_t width)
{
	uint32_t bytes;

	/* The row's pixels, rounded up to whole lines. */
	bytes = width * GHCI_DISPLAY_PIXEL_BYTES;
	bytes = (bytes + GHCI_DISPLAY_LINE_BYTES - 1U) / GHCI_DISPLAY_LINE_BYTES * GHCI_DISPLAY_LINE_BYTES;

	/* Succeeded: the stride. */
	return bytes;
}

/*
 * Reports the time a frame is shown, on the scheduler's clock in
 * nanoseconds.
 */
uint64_t
ghci_scan_now_ns(void)
{
	uint64_t ticks;
	uint64_t nanoseconds;

	/* The clock's ticks, in nanoseconds. */
	ticks = clock_ticks();
	nanoseconds = kern_ticks_to_ns(ticks);

	/* Succeeded: the time. */
	return nanoseconds;
}

/*
 * Settles a reset, reads the records the display wrote and the
 * lighting's completion, and places what waits for the planes; the lock
 * is held.
 */
void
ghci_scan_update(
	struct ghci_device *device)
{
	/* The snapshot a reset asked for, the records and the lighting, then the next present, the cursor's flip and its place. */
	resync_serve(device);
	records_take(device);
	light_check(device);
	queue_kick(device);
	cursor_kick(device);
	cursor_state_kick(device);
}

/*
 * Records an image of plane 0 for the display, after every one recorded
 * before.
 */
int
ghci_scan_record(
	struct ghci_device *device,
	const struct ghci_display_image *image,
	int fifo)
{
	struct ghci_display *display;
	struct ghci_display_image *last;

	/*
	 * A mailbox present replaces the newest present still waiting: the
	 * replaced one counts as adopted once its replacement is.
	 */
	display = &device->display;
	if (!fifo && display->queue_count != 0U) {
		last = &display->queue[display->queue_count - 1U];
		ghci_scan_drop(device, last->memory);
		*last = *image;
		queue_kick(device);
		return 0;
	}

	/* A FIFO present waits for room in the queue. */
	if (display->queue_count >= GHCI_DISPLAY_QUEUE_DEPTH)
		return EAGAIN;

	/* The present waits behind the others, and goes to the ring when the plane's flip is free. */
	display->queue[display->queue_count] = *image;
	display->queue_count++;
	queue_kick(device);

	/* Succeeded: the present is recorded. */
	return 0;
}

/*
 * Shows a cursor surface: flips it now when the display is lit and
 * plane 1 has no flip placed, after the next lighting or record otherwise.
 */
void
ghci_scan_cursor(
	struct ghci_device *device,
	int surface)
{
	/* The surface waits for its flip, which goes now when it can. */
	device->display.cursor_pending = surface;
	cursor_kick(device);
}

/*
 * Places the cursor at a place and shows or hides it: DISP_SET_CURSOR in
 * the cursor plane's queue, now when the ring takes it, at the next
 * look otherwise.  The lock is held.
 */
void
ghci_scan_cursor_state(
	struct ghci_device *device,
	int32_t x,
	int32_t y,
	int show)
{
	struct ghci_display *display;

	/* The place and the visibility, still to be placed. */
	display = &device->display;
	display->cursor_x = x;
	display->cursor_y = y;
	display->cursor_show = show;
	display->cursor_due = 1;

	/* Placed now when the ring takes it. */
	cursor_state_kick(device);
}

/*
 * Marks the lease ended and has the display go dark: the presents
 * waiting are dropped, the cursor hidden and forgotten, and the dark
 * command placed (nothing is turned when the ring does not run).
 */
void
ghci_scan_lease_end(
	struct ghci_device *device)
{
	struct ghci_display *display;
	unsigned index;

	/* The lease is gone; no claim succeeds until the end is finished. */
	display = &device->display;
	display->owner = NULL;
	display->lease = 0U;
	display->ending = 1;
	display->uncertain = 0;

	/* The presents that waited are not shown. */
	for (index = 0U; index < display->queue_count; index++)
		ghci_scan_drop(device, display->queue[index].memory);
	display->queue_count = 0U;

	/* The cursor is hidden and its image forgotten. */
	display->cursor_image = 0;
	display->cursor_pending = -1;
	ghci_scan_cursor_state(device, display->cursor_x, display->cursor_y, 0);

	/* The display goes dark, which cancels every pending flip. */
	dark(device);
}

/*
 * Reports whether an ended lease has let go of everything: the dark
 * command ran and every placed flip's record came.
 */
int
ghci_scan_lease_done(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t status;
	uint64_t value;
	int usable;
	int state;

	/* A lease that did not end has nothing to finish. */
	display = &device->display;
	if (!display->ending)
		return 1;

	/* The dark command has not been placed yet: the service thread places it. */
	if (display->dark_placed < 0)
		return 0;

	/* A ring that no longer runs answers nothing: what the display scans goes to the quarantine. */
	usable = ghci_scan_ring_usable(device);
	if (!usable) {
		host_cannot_turn(device);
		display->dark_placed = 0;
	}

	/* The cancelled records of the flips placed. */
	records_take(device);
	if (display->flips[0].placed || display->flips[1].placed)
		return 0;

	/* The dark command's completion: the display no longer scans (a cancelled one is placed again). */
	if (display->dark_placed > 0) {
		state = comp_state(device, COMP_DARK, &status, &value);
		if (state == COMP_PENDING)
			return 0;
		if (state == COMP_CANCELLED) {
			display->dark_placed = -1;
			return 0;
		}
		display->host_unsure = 0;
	}

	/* The display no longer scans the image it showed: its memory goes (to the quarantine when that was not seen). */
	scan_release(device, display->shown.memory);
	image_clear(&display->shown);
	display->boot_scanning = 0;
	display->cursor_shown = -1;
	display->ending = 0;
	display->dark_placed = 0;

	/* Succeeded: the display may be leased again. */
	return 1;
}

/*
 * Lets go of held memory later, once the display's lock is let go: the
 * device's lock, which ghci_memory_unhold needs, comes before the
 * display's.
 */
void
ghci_scan_drop(
	struct ghci_device *device,
	struct ghci_allocation *memory)
{
	struct ghci_display *display;

	/* A driver's frame is not held. */
	if (memory == NULL)
		return;

	/* The list is long enough for every hold the display has at once. */
	display = &device->display;
	if (display->drop_count >= GHCI_DISPLAY_DROPS) {
		kern_logf("ghci: %s: display holds more memory than it can let go of\n", device->name);
		return;
	}

	/* The hold goes at the next flush. */
	display->drops[display->drop_count] = memory;
	display->drop_count++;
}

/*
 * Lets go of the memory dropped meanwhile, and gives the boot image back
 * once the driver's first flip was taken and nothing else draws to it.
 */
void
ghci_scan_flush(
	struct ghci_device *device)
{
	/* Waits for the locks as any thread but the service may. */
	scan_flush(device, 0);
}

/*
 * Reports the wake-up sequence of the flip waiters, read before a wait
 * looks at the records so no interrupt between is missed.
 */
uint64_t
ghci_scan_mark(
	struct ghci_device *device)
{
	unsigned long irq;
	uint64_t mark;

	/* The sequence, read with the interrupt's lock. */
	irq = spin_lock_irqsave(&device->irq_lock);

	mark = waitq_sequence(&device->display.flip_wake);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the sequence. */
	return mark;
}

/*
 * Sleeps until a display interrupt, a short poll or the deadline.
 */
void
ghci_scan_sleep(
	struct ghci_device *device,
	uint64_t mark,
	uint64_t deadline)
{
	uint64_t wake;
	unsigned long irq;

	/* The next look comes after a short poll at the latest. */
	wake = clock_ticks() + kern_ms_to_ticks(FLIP_POLL_MS);
	if (wake > deadline)
		wake = deadline;

	/* Sleeps unless an interrupt came since the mark. */
	irq = spin_lock_irqsave(&device->irq_lock);

	(void)waitq_sleep(&device->display.flip_wake, &device->irq_lock, mark, wake, 0U);

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Takes memory the display scans, the device's own, held by the
 * display: local memory on a discrete GPU when local is set (memory only
 * the GPU writes), host memory below the GPU's limit otherwise (memory
 * the processor writes, which the display reads by DMA on a discrete GPU
 * as well).  Returns 0, or ENOMEM.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3, CMD_FLIP's
 * base in local memory or below HOST_LIMIT).
 */
int
ghci_scan_alloc(
	struct ghci_device *device,
	uint64_t bytes,
	int local,
	struct ghci_allocation **result)
{
	struct ghci_allocation *memory;
	int error;

	/* Local memory on a discrete GPU when asked for, host memory otherwise. */
	if (local && device->local_ready)
		error = ghci_memory_alloc_local(device, bytes, &memory);
	else
		error = ghci_memory_alloc(device, bytes, 0U, &memory);
	if (error != 0)
		return error;

	/* The display holds it, and the GPU will be given its address. */
	mutex_lock(&device->lock);

	ghci_memory_hold(memory);
	memory->reached = 1;

	mutex_unlock(&device->lock);

	/* Succeeded: the memory is the display's. */
	*result = memory;
	return 0;
}

/*
 * Reports whether the display can scan memory: local memory, or host
 * memory wholly below the GPU's limit.  Returns 1 when it can.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3, CMD_FLIP's
 * base in local VRAM or below HOST_LIMIT).
 */
int
ghci_scan_reachable(
	const struct ghci_device *device,
	const struct ghci_allocation *memory)
{
	uint64_t limit;

	/* Local memory is the display's own to read. */
	if (memory->kind != GHCI_MEMORY_HOST)
		return 1;

	/* Host memory below the limit. */
	limit = ghci_memory_limit(device);
	if (memory->bus + memory->bytes > limit)
		return 0;

	/* Succeeded: the display reaches it. */
	return 1;
}

/*
 * Writes bytes into host memory the display scans, through the
 * processor's view.  Returns 0, EINVAL for a range outside the memory, or
 * ENOTSUP for local memory, which the processor never writes.
 */
int
ghci_scan_write(
	struct ghci_device *device,
	struct ghci_allocation *memory,
	uint64_t offset,
	const void *bytes,
	uint64_t length)
{
	(void)device;

	/* A range inside the memory. */
	if (offset > memory->bytes || length > memory->bytes - offset)
		return EINVAL;

	/* Local memory is the GPU's to write. */
	if (memory->kind != GHCI_MEMORY_HOST)
		return ENOTSUP;

	/* Succeeded: host memory, through the view. */
	kern_memcpy(memory->view + offset, bytes, (size_t)length);
	return 0;
}

/* Reports the processor's view of a place in the display's page. */
static uint8_t *
page_view(
	struct ghci_device *device,
	uint32_t offset)
{
	return device->display.page->view + offset;
}

/* Reports the bus address of a place in the display's page. */
static uint64_t
page_bus(
	struct ghci_device *device,
	uint32_t offset)
{
	return device->display.page->bus + offset;
}

/* Reads a 64-bit word the GPU writes in the display's page. */
static uint64_t
page_read64(
	struct ghci_device *device,
	uint32_t offset)
{
	volatile const uint64_t *word;

	/* One read of the aligned word, as the GPU writes it whole. */
	word = (volatile const uint64_t *)(const void *)page_view(device, offset);
	return *word;
}

/*
 * Arms the completion of one kind of command: its word in the page is
 * cleared, and the next value (never 0) is the one it waits for; comp
 * gets COMP_WRITE (and COMP_IRQ when irq is set), the word's address and
 * the value.
 */
static void
comp_arm(
	struct ghci_device *device,
	unsigned slot,
	int irq,
	struct ghci_disp_comp *comp)
{
	struct ghci_display *display;
	volatile uint64_t *word;
	uint64_t value;

	/* The next value, within the 56 bits a completion carries, and never 0. */
	display = &device->display;
	display->comp_next = (display->comp_next + 1U) & GHCI_DISP_COMP_VALUE_MASK;
	if (display->comp_next == 0U)
		display->comp_next = 1U;
	value = display->comp_next;

	/* The word reads 0 until the queue writes it. */
	word = (volatile uint64_t *)(void *)page_view(device, COMP_OFFSET + 8U * slot);
	*word = 0U;
	display->comp_value[slot] = value;

	/* What the command asks for. */
	comp->flags = GHCI_DISP_COMP_WRITE;
	if (irq)
		comp->flags |= GHCI_DISP_COMP_IRQ;
	comp->pa = page_bus(device, COMP_OFFSET + 8U * slot);
	comp->value = value;
}

/*
 * Reports what the completion of one kind of command says: COMP_PENDING
 * (not written yet), COMP_CANCELLED (a reset cancelled the command) or
 * COMP_DONE, with the queue's status and the value written.  A kind with
 * nothing armed reads as done.  The event ring's setup writes its first
 * sequence number instead of the value, never 0.
 */
static int
comp_state(
	struct ghci_device *device,
	unsigned slot,
	uint32_t *status,
	uint64_t *value)
{
	struct ghci_display *display;
	uint64_t word;

	/* Nothing armed. */
	display = &device->display;
	*status = GHCI_DISP_COMP_OK;
	*value = 0U;
	if (display->comp_value[slot] == 0U)
		return COMP_DONE;

	/* The word, as the queue wrote it (0 until then). */
	word = page_read64(device, COMP_OFFSET + 8U * slot);
	if (word == 0U)
		return COMP_PENDING;

	/* The event ring's setup carries no value of the driver's. */
	if (slot != COMP_EVENTS && (word & GHCI_DISP_COMP_VALUE_MASK) != display->comp_value[slot])
		return COMP_PENDING;

	/* The command finished: cancelled by a reset, or run with its status. */
	display->comp_value[slot] = 0U;
	*value = word & GHCI_DISP_COMP_VALUE_MASK;
	if ((word & GHCI_DISP_COMP_CANCELLED) != 0U)
		return COMP_CANCELLED;
	*status = (uint32_t)(word >> GHCI_DISP_COMP_STATUS_SHIFT) & GHCI_DISP_COMP_STATUS_MASK;

	/* Succeeded: the command ran. */
	return COMP_DONE;
}

/*
 * Waits a bounded time for the completion of one kind of command.
 * Returns 0 with the status and the value, ECANCELED for a command a
 * reset cancelled, or ETIMEDOUT.
 */
static int
command_wait(
	struct ghci_device *device,
	unsigned slot,
	uint32_t *status,
	uint64_t *value)
{
	unsigned waited;
	int state;

	/* The completion, looked at every poll. */
	for (waited = 0U; waited < COMMAND_WAIT_MS; waited++) {
		state = comp_state(device, slot, status, value);
		if (state == COMP_DONE)
			return 0;
		if (state == COMP_CANCELLED)
			return ECANCELED;
		ghci_sleep_ms(1U);
	}

	/* The command did not finish in time. */
	return ETIMEDOUT;
}

/*
 * Places a run of display commands whose last one completes in slot,
 * with the display's lock held only to place it, and waits a bounded
 * time for that completion.  Used at the start and the detach, where
 * nothing else drives the display.  Returns 0 with the status and the
 * value, or the error of the placement or the wait.
 */
static int
command_run(
	struct ghci_device *device,
	const uint32_t *packets,
	uint32_t dwords,
	unsigned slot,
	uint32_t *status,
	uint64_t *value)
{
	int placed;
	int error;

	/* The run, through the gate. */
	mutex_lock(&device->display_lock);

	error = ring_put(device, packets, dwords, &placed);

	mutex_unlock(&device->display_lock);

	/* A run not placed is not waited for. */
	if (error != 0 && !placed)
		return error;

	/* The completion. */
	error = command_wait(device, slot, status, value);
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * Takes the display's page: host memory below the GPU's limit, cleared,
 * held by the display.  Returns 0, or ENOMEM.
 */
static int
page_take(
	struct ghci_device *device)
{
	struct ghci_allocation *page;
	int error;

	/* The page, which the processor reads and the GPU writes. */
	error = ghci_scan_alloc(device, PAGE_BYTES, 0, &page);
	if (error != 0)
		return error;

	/* Succeeded: every word reads 0 until the GPU writes it. */
	kern_memset(page->view, 0, PAGE_BYTES);
	device->display.page = page;
	return 0;
}

/*
 * Sets the event ring up at the start and waits for it: the sink's
 * connection when the display senses it, the underflows and the errors.
 * Returns 0, or the error of the command.
 */
static int
events_start(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t packet[GHCI_DW_DISP_EVENT_SETUP];
	struct ghci_disp_comp comp;
	uint32_t status;
	uint64_t first;
	uint32_t mask;
	uint32_t dwords;
	int error;

	/* The events the driver takes. */
	display = &device->display;
	mask = EVENTS_ERRORS;
	if (display->sense)
		mask |= GHCI_DISP_EVENT_MASK_HOTPLUG;

	/* The setup, whose completion is the ring's first sequence number. */
	comp_arm(device, COMP_EVENTS, 0, &comp);
	dwords = ghci_cmd_disp_event_setup(packet, DISPLAY_INDEX, page_bus(device, EVENTS_OFFSET), EVENTS_CAPACITY, mask, &comp);
	error = command_run(device, packet, dwords, COMP_EVENTS, &status, &first);
	if (error != 0)
		return error;

	/* Succeeded: the ring runs from its first sequence number. */
	display->events_state = EVENTS_RUNNING;
	display->events_mask = mask;
	display->events_first = first;
	display->events_next = first;
	return 0;
}

/*
 * Reads a snapshot of the display at the start: whether a sink is
 * connected (always, for a display that does not sense it), and the mode
 * a running boot display holds.  Returns 0, or the error of the command.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2,
 * GhciDisplayState: state_flags [0] ENABLE, [4] CONNECTED, active_mode
 * at 0x40; section 13.3, the snapshot reflects a boot display before the
 * takeover).
 */
static int
state_read(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t packet[GHCI_DW_DISP_QUERY_STATE];
	struct ghci_disp_comp comp;
	const uint8_t *state;
	uint32_t status;
	uint64_t value;
	uint32_t flags;
	uint32_t dwords;
	int error;

	/* The snapshot. */
	display = &device->display;
	comp_arm(device, COMP_STATE, 0, &comp);
	dwords = ghci_cmd_disp_query_state(packet, DISPLAY_INDEX, page_bus(device, STATE_OFFSET), GHCI_DISP_STATE_BYTES, &comp);
	error = command_run(device, packet, dwords, COMP_STATE, &status, &value);
	if (error != 0)
		return error;

	/* The connection: what the display senses, or connected. */
	state = page_view(device, STATE_OFFSET);
	flags = (uint32_t)state[GHCI_DISP_STATE_FLAGS] | ((uint32_t)state[GHCI_DISP_STATE_FLAGS + 1U] << 8);
	display->connected = 1;
	if (display->sense && (flags & GHCI_DISP_STATE_CONNECTED) == 0U)
		display->connected = 0;

	/* A boot display that does not scan any more shows nothing; one that does holds its mode. */
	if (display->boot_scanning && (flags & GHCI_DISP_STATE_ENABLE) == 0U) {
		display->boot_scanning = 0;
		display->enabled = 0;
		display->mode_set = GHCI_DISPLAY_NO_MODE;
		image_clear(&display->shown);
	}

	if (display->boot_scanning)
		boot_mode_take(device, state);

	/* Succeeded: the display is known. */
	return 0;
}

/*
 * Reads the display's modes: the fixed modes it names, or (for a display
 * that takes arbitrary timings) the sink's EDID and the standard modes
 * that fit its limits.  A running boot display's mode that is one of
 * them stands for the boot slot.  Returns 0, or ENOTSUP when no mode is
 * usable.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 13.1, word 23
 * ARBITRARY_PCLK: 0 for the fixed mode table of CMD_DISP_QUERY_MODES).
 */
static int
modes_read(
	struct ghci_device *device)
{
	struct ghci_display *display;
	unsigned index;
	int error;

	/* The fixed modes, or the sink's and the standard ones. */
	display = &device->display;
	display->mode_count = 0U;
	if ((display->info & GHCI_DISP_INFO_ARBITRARY) == 0U) {
		error = modes_fixed(device);
		if (error != 0)
			return error;
	} else {
		modes_edid(device);
		modes_standard(device);
	}

	/* A display without a usable mode has none. */
	if (display->mode_count == 0U)
		return ENOTSUP;

	/* A boot display's mode of the table is that mode; the display already holds it. */
	for (index = 0U; index < display->mode_count && display->boot_scanning; index++) {
		if (!mode_same(&display->modes[index], &display->modes[GHCI_DISPLAY_BOOT_MODE]))
			continue;
		display->modes[GHCI_DISPLAY_BOOT_MODE].refresh_millihz = display->modes[index].refresh_millihz;
		display->boot.mode = index;
		display->shown.mode = index;
		display->mode = index;
		display->mode_set = index;
		break;
	}

	/* Succeeded: the modes are known. */
	return 0;
}

/*
 * Reads the fixed modes the display names (CMD_DISP_QUERY_MODES), up to
 * the most the driver keeps.  Returns 0, or the error of the command.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3,
 * CMD_DISP_QUERY_MODES: the 16-byte GhciDisplayModesHeader with
 * returned_modes at 0x04, then the 64-byte descriptors).
 */
static int
modes_fixed(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_mode mode;
	uint32_t packet[GHCI_DW_DISP_QUERY_MODES];
	struct ghci_disp_comp comp;
	const uint8_t *answer;
	uint32_t returned;
	uint32_t status;
	uint64_t value;
	uint32_t dwords;
	uint32_t index;
	int error;

	/* The table, into the page. */
	display = &device->display;
	comp_arm(device, COMP_MODES, 0, &comp);
	dwords = ghci_cmd_disp_query_modes(packet, DISPLAY_INDEX, page_bus(device, MODES_OFFSET), GHCI_DISPLAY_MODES_MAX, &comp);
	error = command_run(device, packet, dwords, COMP_MODES, &status, &value);
	if (error != 0)
		return error;

	/* The descriptors written, at most the ones asked for. */
	answer = page_view(device, MODES_OFFSET);
	returned = load32(answer, GHCI_DISP_MODES_RETURNED);
	if (returned > GHCI_DISPLAY_MODES_MAX)
		returned = GHCI_DISPLAY_MODES_MAX;

	/* Each mode with a size and a clock. */
	for (index = 0U; index < returned; index++) {
		mode_read(answer + GHCI_DISP_MODES_HEADER_BYTES + GHCI_DISP_MODE_BYTES * index, &mode);
		if (mode.width == 0U || mode.height == 0U || mode.clock_khz == 0U || mode.htotal == 0U || mode.vtotal == 0U)
			continue;
		mode_refresh(&mode);
		mode_add(display, &mode);
	}

	/* Succeeded: the fixed modes are read. */
	return 0;
}

/*
 * Adds the detailed timings of the sink's EDID that fit the display, when
 * the display has the DDC bus and a sink answers.
 */
static void
modes_edid(
	struct ghci_device *device)
{
	uint8_t edid[EDID_BYTES];
	unsigned index;
	int error;

	/* Without the bus or a sink, no EDID. */
	if (!device->display.i2c || !device->display.connected)
		return;

	/* The base block. */
	error = edid_read(device, edid);
	if (error != 0) {
		kern_logf("ghci: %s: no EDID from the sink (%d)\n", device->name, error);
		return;
	}

	/* Each detailed timing (a descriptor with no clock is something else). */
	for (index = 0U; index < EDID_TIMING_COUNT; index++)
		edid_timing(device, edid + EDID_TIMINGS + EDID_TIMING_BYTES * index);
}

/*
 * Reads the sink's EDID base block through the DDC bus: a read of 128
 * bytes from offset 0 at address 0x50, then its header and checksum.
 * Returns 0, EIO for a transfer that did not finish, or EINVAL for a
 * block that is not an EDID.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3,
 * CMD_DISP_I2C_XFER: a read with HAS_OFFSET writes the offset, then reads
 * after a repeated START; the buffer's header status [15:0] and
 * bytes_done [31:16]).  VESA E-EDID standard release A2 (section 3.1,
 * the header 00 FF FF FF FF FF FF 00 and the checksum of the 128 bytes).
 */
static int
edid_read(
	struct ghci_device *device,
	uint8_t *edid)
{
	static const uint8_t header[8] = { 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U };
	uint32_t packet[GHCI_DW_DISP_I2C_XFER];
	struct ghci_disp_comp comp;
	struct ghci_i2c_args args;
	const uint8_t *buffer;
	uint32_t transfer;
	uint32_t status;
	uint64_t value;
	uint32_t dwords;
	uint32_t index;
	uint8_t sum;
	int error;

	/* The read of the base block. */
	kern_memset(&args, 0, sizeof(args));
	args.display = DISPLAY_INDEX;
	args.flags = GHCI_DISP_I2C_HAS_OFFSET;
	args.slave = EDID_ADDRESS;
	args.offset = 0U;
	args.bytes = EDID_BYTES;
	args.buffer = page_bus(device, I2C_OFFSET);
	comp_arm(device, COMP_I2C, 0, &comp);
	dwords = ghci_cmd_disp_i2c_xfer(packet, &args, &comp);
	error = command_run(device, packet, dwords, COMP_I2C, &status, &value);
	if (error != 0)
		return error;

	/* Every byte, transferred. */
	buffer = page_view(device, I2C_OFFSET);
	transfer = load32(buffer, 0U);
	if ((transfer & 0xFFFFU) != GHCI_DISP_I2C_OK || (transfer >> 16) != EDID_BYTES)
		return EIO;
	kern_memcpy(edid, buffer + GHCI_DISP_I2C_HEADER_BYTES, EDID_BYTES);

	/* The header. */
	if (kern_memcmp(edid, header, sizeof(header)) != 0)
		return EINVAL;

	/* The bytes sum to 0. */
	sum = 0U;
	for (index = 0U; index < EDID_BYTES; index++)
		sum = (uint8_t)(sum + edid[index]);
	if (sum != 0U)
		return EINVAL;

	/* Succeeded: the block is the sink's EDID. */
	return 0;
}

/*
 * Adds one 18-byte detailed timing of an EDID as a mode, when it is a
 * progressive timing the display's limits take.
 *
 * Values referenced: VESA E-EDID standard release A2 (section 3.10.2,
 * the detailed timing: the clock in 10 kHz at bytes 0 and 1, the active
 * and blanking sizes, the sync offsets and widths split across bytes 2
 * to 11, and byte 17: [7] interlaced, [4:3] 11 digital separate sync,
 * [2] vertical and [1] horizontal polarity).
 */
static void
edid_timing(
	struct ghci_device *device,
	const uint8_t *timing)
{
	struct ghci_display_mode mode;
	uint32_t clock;
	uint32_t hblank;
	uint32_t vblank;
	uint32_t hfront;
	uint32_t hsync;
	uint32_t vfront;
	uint32_t vsync;
	int fits;

	/* A descriptor without a clock is not a timing. */
	clock = (uint32_t)timing[0] | ((uint32_t)timing[1] << 8);
	if (clock == 0U)
		return;

	/* An interlaced timing is not taken: the display scans progressively. */
	if ((timing[17] & 0x80U) != 0U)
		return;

	/* The active sizes and the blanking. */
	kern_memset(&mode, 0, sizeof(mode));
	mode.clock_khz = clock * 10U;
	mode.width = (uint32_t)timing[2] | (((uint32_t)timing[4] >> 4) << 8);
	hblank = (uint32_t)timing[3] | (((uint32_t)timing[4] & 0xFU) << 8);
	mode.height = (uint32_t)timing[5] | (((uint32_t)timing[7] >> 4) << 8);
	vblank = (uint32_t)timing[6] | (((uint32_t)timing[7] & 0xFU) << 8);

	/* The porches and syncs. */
	hfront = (uint32_t)timing[8] | ((((uint32_t)timing[11] >> 6) & 0x3U) << 8);
	hsync = (uint32_t)timing[9] | ((((uint32_t)timing[11] >> 4) & 0x3U) << 8);
	vfront = ((uint32_t)timing[10] >> 4) | ((((uint32_t)timing[11] >> 2) & 0x3U) << 4);
	vsync = ((uint32_t)timing[10] & 0xFU) | (((uint32_t)timing[11] & 0x3U) << 4);

	/* Where each sync starts and ends, and the totals. */
	mode.hsync_start = mode.width + hfront;
	mode.hsync_end = mode.hsync_start + hsync;
	mode.htotal = mode.width + hblank;
	mode.vsync_start = mode.height + vfront;
	mode.vsync_end = mode.vsync_start + vsync;
	mode.vtotal = mode.height + vblank;

	/* The polarities of a digital separate sync; any other sync is negative. */
	if ((timing[17] & 0x18U) == 0x18U) {
		if ((timing[17] & 0x04U) != 0U)
			mode.flags |= GHCI_DISP_MODE_VSYNC_POSITIVE;
		if ((timing[17] & 0x02U) != 0U)
			mode.flags |= GHCI_DISP_MODE_HSYNC_POSITIVE;
	}
	mode.mode_id = GHCI_DISP_MODE_ARBITRARY;

	/* Taken when the display's limits take it. */
	fits = mode_fits(device, &mode);
	if (!fits)
		return;
	mode_refresh(&mode);
	mode_add(&device->display, &mode);
}

/* Adds the standard modes that fit the display's limits. */
static void
modes_standard(
	struct ghci_device *device)
{
	struct ghci_display_mode mode;
	unsigned index;
	int fits;

	/* Each standard mode, as the display's limits take it. */
	for (index = 0U; index < STANDARD_MODES; index++) {
		kern_memset(&mode, 0, sizeof(mode));
		mode.clock_khz = standard_modes[index][0];
		mode.width = standard_modes[index][1];
		mode.hsync_start = standard_modes[index][2];
		mode.hsync_end = standard_modes[index][3];
		mode.htotal = standard_modes[index][4];
		mode.height = standard_modes[index][5];
		mode.vsync_start = standard_modes[index][6];
		mode.vsync_end = standard_modes[index][7];
		mode.vtotal = standard_modes[index][8];
		mode.flags = standard_modes[index][9];
		mode.mode_id = GHCI_DISP_MODE_ARBITRARY;
		fits = mode_fits(device, &mode);
		if (!fits)
			continue;
		mode_refresh(&mode);
		mode_add(&device->display, &mode);
	}
}

/* Adds a mode to the table, unless the table has it or is full. */
static void
mode_add(
	struct ghci_display *display,
	const struct ghci_display_mode *mode)
{
	unsigned index;

	/* A mode the table has already. */
	for (index = 0U; index < display->mode_count; index++) {
		if (mode_same(&display->modes[index], mode))
			return;
	}

	/* A full table. */
	if (display->mode_count >= GHCI_DISPLAY_MODES_MAX)
		return;

	/* The mode at the end. */
	display->modes[display->mode_count] = *mode;
	display->mode_count++;
}

/*
 * Reports whether a timing passes the display's validation of an
 * arbitrary timing, which it then must accept: ordered syncs within the
 * totals, the active and total sizes within words 24 and 25, the porches
 * and syncs at least words 28 and 29, and the clock within words 21 and
 * 22.  Returns 1 when it does.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2, the
 * mode validation table and its acceptance guarantee).
 */
static int
mode_fits(
	const struct ghci_device *device,
	const struct ghci_display_mode *mode)
{
	const struct ghci_caps *caps;
	uint32_t least;

	/* The syncs in order, within the totals, after a non-empty active part. */
	if (mode->clock_khz == 0U || mode->width == 0U || mode->height == 0U)
		return 0;
	if (mode->width > mode->hsync_start || mode->hsync_start >= mode->hsync_end || mode->hsync_end > mode->htotal)
		return 0;
	if (mode->height > mode->vsync_start || mode->vsync_start >= mode->vsync_end || mode->vsync_end > mode->vtotal)
		return 0;

	/* The active and total sizes within the limits; a descriptor's fields are 16 bits. */
	caps = &device->caps;
	if (mode->htotal > 0xFFFFU || mode->vtotal > 0xFFFFU)
		return 0;
	if (mode->width > (caps->disp_limits_h & GHCI_DISP_LIMIT_ACTIVE_MASK))
		return 0;
	if (mode->htotal > (caps->disp_limits_h >> GHCI_DISP_LIMIT_TOTAL_SHIFT))
		return 0;
	if (mode->height > (caps->disp_limits_v & GHCI_DISP_LIMIT_ACTIVE_MASK))
		return 0;
	if (mode->vtotal > (caps->disp_limits_v >> GHCI_DISP_LIMIT_TOTAL_SHIFT))
		return 0;

	/* The horizontal porches and sync at least the least ones. */
	least = (caps->disp_blank_h >> GHCI_DISP_BLANK_FRONT_SHIFT) & GHCI_DISP_BLANK_MASK;
	if (mode->hsync_start - mode->width < least)
		return 0;
	least = (caps->disp_blank_h >> GHCI_DISP_BLANK_SYNC_SHIFT) & GHCI_DISP_BLANK_MASK;
	if (mode->hsync_end - mode->hsync_start < least)
		return 0;
	least = (caps->disp_blank_h >> GHCI_DISP_BLANK_BACK_SHIFT) & GHCI_DISP_BLANK_MASK;
	if (mode->htotal - mode->hsync_end < least)
		return 0;

	/* The vertical ones. */
	least = (caps->disp_blank_v >> GHCI_DISP_BLANK_FRONT_SHIFT) & GHCI_DISP_BLANK_MASK;
	if (mode->vsync_start - mode->height < least)
		return 0;
	least = (caps->disp_blank_v >> GHCI_DISP_BLANK_SYNC_SHIFT) & GHCI_DISP_BLANK_MASK;
	if (mode->vsync_end - mode->vsync_start < least)
		return 0;
	least = (caps->disp_blank_v >> GHCI_DISP_BLANK_BACK_SHIFT) & GHCI_DISP_BLANK_MASK;
	if (mode->vtotal - mode->vsync_end < least)
		return 0;

	/* The clock within the range. */
	if (mode->clock_khz < caps->disp_clock_min || mode->clock_khz > caps->disp_clock_max)
		return 0;

	/* Succeeded: the display takes the timing. */
	return 1;
}

/* Fills a mode's refresh: its clock over its totals, in millihertz, rounded. */
static void
mode_refresh(
	struct ghci_display_mode *mode)
{
	uint64_t total;

	/* The pixel clocks of one whole frame, the blanking included. */
	total = (uint64_t)mode->htotal * mode->vtotal;
	mode->refresh_millihz = 0U;
	if (total == 0U)
		return;

	/* The refresh. */
	mode->refresh_millihz = (uint32_t)(((uint64_t)mode->clock_khz * 1000000U + total / 2U) / total);
}

/*
 * Reads a 64-byte mode descriptor into a mode.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2,
 * GhciDisplayModeDesc: pixel_clock_khz at 0x04, the 16-bit timing from
 * 0x08 to 0x16, flags at 0x18, mode_id at 0x24).
 */
static void
mode_read(
	const uint8_t *desc,
	struct ghci_display_mode *mode)
{
	/* The clock and the horizontal timing. */
	kern_memset(mode, 0, sizeof(*mode));
	mode->clock_khz = load32(desc, GHCI_DISP_MODE_PIXEL_CLOCK);
	mode->width = load16(desc, GHCI_DISP_MODE_HACTIVE);
	mode->hsync_start = load16(desc, GHCI_DISP_MODE_HSYNC_START);
	mode->hsync_end = load16(desc, GHCI_DISP_MODE_HSYNC_END);
	mode->htotal = load16(desc, GHCI_DISP_MODE_HTOTAL);

	/* The vertical timing, the polarities and the fixed mode's number. */
	mode->height = load16(desc, GHCI_DISP_MODE_VACTIVE);
	mode->vsync_start = load16(desc, GHCI_DISP_MODE_VSYNC_START);
	mode->vsync_end = load16(desc, GHCI_DISP_MODE_VSYNC_END);
	mode->vtotal = load16(desc, GHCI_DISP_MODE_VTOTAL);
	mode->flags = load32(desc, GHCI_DISP_MODE_FLAGS) & (GHCI_DISP_MODE_HSYNC_POSITIVE | GHCI_DISP_MODE_VSYNC_POSITIVE);
	mode->mode_id = load32(desc, GHCI_DISP_MODE_ID);
}

/* Writes a mode as a 64-byte mode descriptor, its status and the rest 0. */
static void
mode_write(
	const struct ghci_display_mode *mode,
	uint8_t *desc)
{
	/* Everything 0, then the size, the clock and the horizontal timing. */
	kern_memset(desc, 0, GHCI_DISP_MODE_BYTES);
	store32(desc, GHCI_DISP_MODE_STRUCT_BYTES, GHCI_DISP_MODE_BYTES);
	store32(desc, GHCI_DISP_MODE_PIXEL_CLOCK, mode->clock_khz);
	store16(desc, GHCI_DISP_MODE_HACTIVE, mode->width);
	store16(desc, GHCI_DISP_MODE_HSYNC_START, mode->hsync_start);
	store16(desc, GHCI_DISP_MODE_HSYNC_END, mode->hsync_end);
	store16(desc, GHCI_DISP_MODE_HTOTAL, mode->htotal);

	/* The vertical timing, the polarities and the fixed mode's number. */
	store16(desc, GHCI_DISP_MODE_VACTIVE, mode->height);
	store16(desc, GHCI_DISP_MODE_VSYNC_START, mode->vsync_start);
	store16(desc, GHCI_DISP_MODE_VSYNC_END, mode->vsync_end);
	store16(desc, GHCI_DISP_MODE_VTOTAL, mode->vtotal);
	store32(desc, GHCI_DISP_MODE_FLAGS, mode->flags);
	store32(desc, GHCI_DISP_MODE_ID, mode->mode_id);
}

/* Reports whether two modes have the same timing: 1 when they do. */
static int
mode_same(
	const struct ghci_display_mode *first,
	const struct ghci_display_mode *second)
{
	/* The clock and the horizontal timing. */
	if (first->clock_khz != second->clock_khz)
		return 0;
	if (first->width != second->width || first->hsync_start != second->hsync_start)
		return 0;
	if (first->hsync_end != second->hsync_end || first->htotal != second->htotal)
		return 0;

	/* The vertical timing and the polarities. */
	if (first->height != second->height || first->vsync_start != second->vsync_start)
		return 0;
	if (first->vsync_end != second->vsync_end || first->vtotal != second->vtotal)
		return 0;
	if (first->flags != second->flags)
		return 0;

	/* Succeeded: the same timing. */
	return 1;
}

/*
 * Takes over a boot display the firmware left running, as the BOOT_DISP
 * window said before any display command: the image it scans (in local
 * memory at the framebuffer's offset), its rows and its format, in the
 * boot slot of the modes, whose timing the start's snapshot gives.
 * Nothing is placed.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 13.3, the
 * driver reads every BOOT_DISP register before a takeover command; the
 * boot framebuffer at MEM_VRAM_BASE + BOOT_DISP_FB_OFFSET).
 */
static void
boot_take(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_mode *boot;
	const struct ghci_caps *caps;

	/* A boot display that is running. */
	display = &device->display;
	caps = &device->caps;
	if ((caps->boot_disp_status & GHCI_BOOT_DISP_ACTIVE) == 0U)
		return;

	/* Its size, until the snapshot gives the whole timing. */
	boot = &display->modes[GHCI_DISPLAY_BOOT_MODE];
	kern_memset(boot, 0, sizeof(*boot));
	boot->width = caps->boot_disp_width;
	boot->height = caps->boot_disp_height;

	/* The boot image, which nothing of the driver's holds. */
	image_clear(&display->boot);
	display->boot.base = caps->local_base + caps->boot_disp_offset;
	display->boot.stride = caps->boot_disp_stride;
	display->boot.format = caps->boot_disp_format;
	display->boot.mode = GHCI_DISPLAY_BOOT_MODE;

	/*
	 * The display is lit on the boot image until the driver's first
	 * command takes it over; until the kernel can say that nothing draws
	 * to the image any more, it is kept for the console.
	 */
	display->enabled = 1;
	display->mode = GHCI_DISPLAY_BOOT_MODE;
	display->mode_set = GHCI_DISPLAY_BOOT_MODE;
	display->format = display->boot.format;
	display->stride = display->boot.stride;
	display->shown = display->boot;
	display->boot_scanning = 1;
	display->boot_kept = 1;
	display->boot_console = 1;
}

/* Takes the boot display's mode from the snapshot's active mode into the boot slot. */
static void
boot_mode_take(
	struct ghci_device *device,
	const uint8_t *state)
{
	struct ghci_display_mode mode;

	/* The active mode, when the snapshot has one. */
	mode_read(state + GHCI_DISP_STATE_MODE, &mode);
	if (mode.width == 0U || mode.height == 0U)
		return;

	/* Succeeded: the boot slot holds the whole timing. */
	mode_refresh(&mode);
	device->display.modes[GHCI_DISPLAY_BOOT_MODE] = mode;
}

/* Empties an image record: no present, no memory, no frame. */
static void
image_clear(
	struct ghci_display_image *image)
{
	kern_memset(image, 0, sizeof(*image));
	image->copy_frame = -1;
}

/* Reports the bus address of a plane's flip record in the page. */
static uint64_t
record_address(
	struct ghci_device *device,
	unsigned plane)
{
	return page_bus(device, RECORD_OFFSET + GHCI_FLIP_RECORD_BYTES * plane);
}

/* Reports the processor's view of the value word of a plane's flip record. */
static volatile uint32_t *
record_slot(
	struct ghci_device *device,
	unsigned plane)
{
	return (volatile uint32_t *)(void *)page_view(device, RECORD_OFFSET + GHCI_FLIP_RECORD_BYTES * plane);
}

/* Takes the next flip value: 1 to 2^31 - 1, never 0, so a cleared record names no flip. */
static uint32_t
value_take(
	struct ghci_display *display)
{
	uint32_t value;

	/* The value, and the next one after it with the wrap past 31 bits. */
	value = display->next_value;
	display->next_value++;
	if (display->next_value > GHCI_FLIP_VALUE_MAX)
		display->next_value = 1U;

	/* Succeeded: the value. */
	return value;
}

/*
 * Places a run of the display's commands and a fence after them through
 * the gate, without waiting for a recovery; the display's lock is held
 * throughout, so the state read before stays.  The service thread's
 * placement waits for nothing: it enters only a free ring with room for
 * the run and its fence now.
 *
 * Returns 0; EAGAIN when a recovery runs (or, for the service, the ring
 * is in use or full), with nothing placed, or with *placed set when the
 * part placed before it began is lost to the reset (the snapshot after
 * the recovery settles it); EIO when the device is lost; or EINVAL for a
 * run an encoder refused (0 dwords).
 */
static int
ring_put(
	struct ghci_device *device,
	const uint32_t *packets,
	uint32_t dwords,
	int *placed)
{
	struct ghci_ticket ticket;
	enum ghci_gate gate;
	uint32_t start;
	int error;

	/* A run an encoder refused is not placed. */
	*placed = 0;
	if (dwords == 0U)
		return EINVAL;

	/* The gate, which keeps the display's lock held when it does not open. */
	if (device->display.service_placing)
		gate = ghci_cp_try_enter(device, dwords + GHCI_DW_FENCE);
	else
		gate = ghci_cp_enter(device, GHCI_HOLD_DISPLAY, 0);
	if (gate == GHCI_GATE_LOST)
		return EIO;
	if (gate != GHCI_GATE_OPEN)
		return EAGAIN;

	/* The commands, the fence after them, and the doorbell for what was placed. */
	start = device->ring.wptr;
	error = ghci_cp_place(device, packets, dwords);
	if (error == 0)
		error = ghci_cp_place_fence(device, NULL, start, &ticket);
	if (device->ring.wptr != start)
		*placed = 1;
	ghci_cp_doorbell(device);
	ghci_cp_leave(device);

	/* A recovery that began in the middle took what was placed. */
	if (error != 0)
		return EAGAIN;

	/* Succeeded: the run is in the ring. */
	return 0;
}

/* Reads the page's record of each placed flip and takes the flips whose records came. */
static void
records_take(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_flip *flip;
	uint32_t value;
	unsigned plane;
	int cancelled;

	/* Records exist only once the page does. */
	display = &device->display;
	if (display->page == NULL)
		return;

	/* Each plane's record names its flip by the value, with bit 31 saying it was cancelled. */
	for (plane = 0U; plane < 2U; plane++) {
		flip = &display->flips[plane];
		if (!flip->placed)
			continue;

		value = *record_slot(device, plane);
		if ((value & ~GHCI_FLIP_CANCELLED) != flip->value)
			continue;

		cancelled = 0;
		if ((value & GHCI_FLIP_CANCELLED) != 0U)
			cancelled = 1;
		taken(device, plane, cancelled);
	}
}

/*
 * Takes a plane's flip: shown, or cancelled.
 *
 * A shown image of plane 0 replaces the one shown before, whose memory
 * goes, and its present (with every lower one) is adopted; the first
 * image of the driver's ends the boot display's scan-out.  A cancelled
 * image is let go of without being shown.  A cursor surface shown
 * becomes the display's; a cancelled one is flipped again once the
 * display is lit.
 */
static void
taken(
	struct ghci_device *device,
	unsigned plane,
	int cancelled)
{
	struct ghci_display *display;
	struct ghci_display_flip *flip;
	int boot;

	display = &device->display;
	flip = &display->flips[plane];
	flip->placed = 0;

	/* The cursor's plane. */
	if (plane == 1U) {
		if (cancelled) {
			/* Not read: the surface is flipped again while the lease keeps its image. */
			if (display->cursor_image && display->cursor_pending < 0)
				display->cursor_pending = flip->surface;
		} else {
			/* The display has read the surface and shows it from now. */
			display->cursor_shown = flip->surface;
		}

		return;
	}

	/* A cancelled image is not shown; its memory goes. */
	if (cancelled) {
		ghci_scan_drop(device, flip->image.memory);
		return;
	}

	/* The image is on the screen; the one before is no longer scanned. */
	ghci_scan_drop(device, display->shown.memory);
	display->shown = flip->image;
	display->shown_time_ns = ghci_scan_now_ns();

	/* The boot image is the driver's own image at the boot display's address. */
	boot = boot_image(display, &flip->image);

	/*
	 * The first image of the driver's ends the boot display; the boot
	 * image goes back unless the console may draw to it, which is told it
	 * leaves the display first (once).
	 */
	if (display->boot_scanning && !boot) {
		display->boot_scanning = 0;
		if (display->boot_kept && !display->boot_console)
			display->boot_release = 1;
		if (display->boot_kept && display->boot_console && !display->boot_end_tried)
			display->boot_end_due = 1;
	}

	if (boot && display->boot_kept)
		display->boot_scanning = 1;

	/* The present is adopted, with every lower one (the ones it replaced included). */
	if (flip->image.sequence > display->adopted)
		display->adopted = flip->image.sequence;
}

/*
 * Reads the completion of the last lighting: a display that did not take
 * the mode (no valid mode, INVALID_STATE) stays dark, and holds no mode.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2, DISP_SET
 * with ENABLE and no valid mode leaves ENABLE 0 with INVALID_STATE; a
 * mode descriptor's status at 0x1C).
 */
static void
light_check(
	struct ghci_device *device)
{
	struct ghci_display *display;
	const uint8_t *desc;
	uint32_t status;
	uint64_t value;
	int state;

	/* Nothing armed, or not written yet; a cancelled one is the snapshot's after the reset. */
	display = &device->display;
	if (display->page == NULL)
		return;
	state = comp_state(device, COMP_LIGHT, &status, &value);
	if (state != COMP_DONE || status == GHCI_DISP_COMP_OK)
		return;

	/* The display stays dark: the mode was refused. */
	desc = page_view(device, MODE_OFFSET);
	kern_logf("ghci: %s: display not lit (status %u, mode status %u)\n",
	    device->name,
	    (unsigned)status,
	    (unsigned)load32(desc, GHCI_DISP_MODE_STATUS));
	display->enabled = 0;
	display->mode_set = GHCI_DISPLAY_NO_MODE;
}

/* Places the oldest present waiting, when plane 0 has no flip placed: a flip, or a new lighting. */
static void
queue_kick(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_image *head;
	int relight;
	int usable;
	int error;

	/* Nothing waits, the plane's flip is in use, the lease is ending, a reset is being settled, or the ring does not run. */
	display = &device->display;
	if (display->queue_count == 0U || display->flips[0].placed || display->ending || display->resync != 0)
		return;
	usable = ghci_scan_ring_usable(device);
	if (!usable)
		return;

	/* An image of another mode than the one lit lights the display again; a format or rows of their own do not. */
	head = &display->queue[0];
	relight = 0;
	if (!display->enabled)
		relight = 1;
	if (head->mode != display->mode)
		relight = 1;

	/* The run that lights the display on the image, or one flip; a refused one waits for the next look. */
	if (relight)
		error = light(device, head);
	else
		error = flip_primary(device, head);
	if (error != 0)
		return;

	/* The present has left the queue: its flip is placed. */
	queue_pop(display);
}

/* Puts an image back at the head of the queue, for a flip a reset lost. */
static void
queue_push_front(
	struct ghci_display *display,
	const struct ghci_display_image *image)
{
	unsigned index;

	/* The queue has room for one more than its depth: the flip the plane had. */
	if (display->queue_count >= GHCI_DISPLAY_QUEUE)
		return;

	/* The others move back by one. */
	for (index = display->queue_count; index > 0U; index--)
		display->queue[index] = display->queue[index - 1U];
	display->queue[0] = *image;
	display->queue_count++;
}

/* Takes the oldest present out of the queue. */
static void
queue_pop(
	struct ghci_display *display)
{
	unsigned index;

	/* The others move forward by one. */
	for (index = 1U; index < display->queue_count; index++)
		display->queue[index - 1U] = display->queue[index];
	display->queue_count--;
}

/*
 * Writes one FLIP: of plane 0 to an image (taken at the next vertical
 * sync once the GPU's writes before it are in memory), or of plane 1 to a
 * cursor surface (image NULL), with its record cleared first and its
 * interrupt.  Returns the dwords, 0 for one the encoder refused.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3,
 * CMD_FLIP's flags; section 13.2, WAIT_GPU_IDLE: the display queue holds
 * the flip until the writes of every earlier command are visible, as the
 * processor only queues it).
 */
static uint32_t
flip_packet(
	struct ghci_device *device,
	uint32_t *out,
	const struct ghci_display_image *image,
	int surface,
	uint32_t value)
{
	struct ghci_flip_args args;
	unsigned plane;

	/* The image of plane 0, or the cursor's surface. */
	kern_memset(&args, 0, sizeof(args));
	args.display = DISPLAY_INDEX;
	args.value = value;
	args.flags = GHCI_FLIP_VSYNC_WAIT | GHCI_FLIP_RECORD_ENABLE | GHCI_FLIP_RECORD_IRQ;
	if (image != NULL) {
		plane = 0U;
		args.base = image->base;
		args.stride = image->stride;
		args.format = image->format;
		args.flags |= GHCI_FLIP_WAIT_GPU_IDLE;
	} else {
		plane = 1U;
		args.base = device->display.surfaces[surface]->bus;
		args.flags |= GHCI_FLIP_PLANE_CURSOR;
	}

	/* The record, cleared, so that only this flip's names it. */
	*record_slot(device, plane) = 0U;
	args.record = record_address(device, plane);

	/* Succeeded: the packet. */
	return ghci_cmd_flip(out, &args);
}

/*
 * Places the run that lights the display on an image: DISP_SET with
 * ENABLE off when it is lit (the scan-out stops at the frame's end),
 * DISP_SET_MODE when the display holds another mode, the flip (taken at
 * once while ENABLE is off), DISP_SET with ENABLE and its completion, and
 * the cursor's flip when one waits; then the driver's fence.  Returns 0
 * once the run (or a part a recovery cut) is in the ring, or the error of
 * a run not placed.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2, the
 * control queue starts after every earlier display command, SET_MODE
 * only while ENABLE is 0, a plane 0 flip taken at once while ENABLE is 0,
 * ENABLE 0 to 1 scans the plane's image from the next frame).
 */
static int
light(
	struct ghci_device *device,
	const struct ghci_display_image *image)
{
	struct ghci_display *display;
	uint32_t packets[LIGHT_DWORDS];
	struct ghci_disp_comp none;
	struct ghci_disp_comp comp;
	uint32_t value;
	uint32_t cursor_value;
	uint32_t dwords;
	uint32_t written;
	int cursor;
	int placed;
	int error;

	/* No completion for the steps before the last. */
	display = &device->display;
	kern_memset(&none, 0, sizeof(none));
	dwords = 0U;
	error = 0;

	/* A lit display goes dark first. */
	if (display->enabled) {
		written = ghci_cmd_disp_set(&packets[dwords], DISPLAY_INDEX, 0U, &none);
		if (written == 0U)
			error = EINVAL;
		dwords += written;
	}

	/* The image's mode, when the display holds another. */
	if (error == 0 && display->mode_set != image->mode) {
		mode_write(&display->modes[image->mode], page_view(device, MODE_OFFSET));
		written = ghci_cmd_disp_set_mode(&packets[dwords], DISPLAY_INDEX, 0U, page_bus(device, MODE_OFFSET), &none);
		if (written == 0U)
			error = EINVAL;
		dwords += written;
	}

	/* The image's flip, taken at once while ENABLE is off. */
	value = value_take(display);
	if (error == 0) {
		written = flip_packet(device, &packets[dwords], image, -1, value);
		if (written == 0U)
			error = EINVAL;
		dwords += written;
	}

	/* ENABLE, whose completion says whether the display took the mode. */
	if (error == 0) {
		comp_arm(device, COMP_LIGHT, 1, &comp);
		written = ghci_cmd_disp_set(&packets[dwords], DISPLAY_INDEX, GHCI_DISP_SET_ENABLE, &comp);
		if (written == 0U)
			error = EINVAL;
		dwords += written;
	}

	/* The cursor's surface that waited for the lighting, when plane 1 has no flip placed. */
	cursor = 0;
	cursor_value = 0U;
	if (error == 0 && display->cursor_pending >= 0 && !display->flips[1].placed) {
		cursor = 1;
		cursor_value = value_take(display);
		written = flip_packet(device, &packets[dwords], NULL, display->cursor_pending, cursor_value);
		if (written == 0U)
			error = EINVAL;
		dwords += written;
	}

	/* An image the encoders refused is not placed. */
	if (error != 0) {
		kern_logf("ghci: %s: display image not placed (base %llx, stride %u, format %u)\n",
		    device->name,
		    (unsigned long long)image->base,
		    (unsigned)image->stride,
		    (unsigned)image->format);
		return error;
	}

	/* The run, with the driver's fence after it; nothing is recorded unless some of it was placed. */
	error = ring_put(device, packets, dwords, &placed);
	if (error != 0 && !placed)
		return error;

	/* The display is lit on the image's mode, format and stride once the run is queued. */
	display->enabled = 1;
	display->mode = image->mode;
	display->mode_set = image->mode;
	display->format = image->format;
	display->stride = image->stride;
	display->flips[0].placed = 1;
	display->flips[0].value = value;
	display->flips[0].light = 1;
	display->flips[0].image = *image;

	/* The cursor's flip is placed too. */
	if (cursor) {
		display->flips[1].placed = 1;
		display->flips[1].value = cursor_value;
		display->flips[1].light = 0;
		display->flips[1].surface = display->cursor_pending;
		display->cursor_pending = -1;
	}

	/* Succeeded: the run is in the ring. */
	return 0;
}

/* Places one flip of plane 0 to an image of the mode lit (its format and rows may differ), with the driver's fence. */
static int
flip_primary(
	struct ghci_device *device,
	const struct ghci_display_image *image)
{
	struct ghci_display *display;
	uint32_t packet[GHCI_DW_FLIP];
	uint32_t dwords;
	uint32_t value;
	int placed;
	int error;

	/* The flip at the next vertical sync. */
	display = &device->display;
	value = value_take(display);
	dwords = flip_packet(device, packet, image, -1, value);
	error = ring_put(device, packet, dwords, &placed);
	if (error != 0 && !placed)
		return error;

	/* The plane's flip is the image's until its record comes; the format and rows are its. */
	display->format = image->format;
	display->stride = image->stride;
	display->flips[0].placed = 1;
	display->flips[0].value = value;
	display->flips[0].light = 0;
	display->flips[0].image = *image;

	/* Succeeded: the flip is in the ring. */
	return 0;
}

/* Places one flip of plane 1 to a cursor surface, with the driver's fence. */
static int
flip_cursor(
	struct ghci_device *device,
	int surface)
{
	struct ghci_display *display;
	uint32_t packet[GHCI_DW_FLIP];
	uint32_t dwords;
	uint32_t value;
	int placed;
	int error;

	/* The flip at the next frame. */
	display = &device->display;
	value = value_take(display);
	dwords = flip_packet(device, packet, NULL, surface, value);
	error = ring_put(device, packet, dwords, &placed);
	if (error != 0 && !placed)
		return error;

	/* The cursor's flip is the surface's until its record comes. */
	display->flips[1].placed = 1;
	display->flips[1].value = value;
	display->flips[1].light = 0;
	display->flips[1].surface = surface;

	/* Succeeded: the flip is in the ring. */
	return 0;
}

/* Places the cursor surface that waits, when the display is lit and plane 1 has no flip placed. */
static void
cursor_kick(
	struct ghci_device *device)
{
	struct ghci_display *display;
	int usable;
	int error;

	/* Nothing waits, the display is dark, the plane's flip is in use, the lease is ending, or a reset is being settled. */
	display = &device->display;
	if (display->cursor_pending < 0 || !display->enabled || display->flips[1].placed || display->ending || display->resync != 0)
		return;
	usable = ghci_scan_ring_usable(device);
	if (!usable)
		return;

	/* The surface's flip; a refused one waits for the next look. */
	error = flip_cursor(device, display->cursor_pending);
	if (error != 0)
		return;

	/* The surface no longer waits. */
	display->cursor_pending = -1;
}

/*
 * Places the cursor's place and visibility that wait (DISP_SET_CURSOR,
 * which a display without the cursor plane does not take); a refused one
 * waits for the next look.
 */
static void
cursor_state_kick(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t packet[GHCI_DW_DISP_SET_CURSOR];
	uint32_t dwords;
	int usable;
	int placed;
	int error;

	/* Nothing waits, no cursor plane, a reset is being settled, or the ring does not run. */
	display = &device->display;
	if (!display->cursor_due || !display->cursor || display->resync != 0)
		return;
	usable = ghci_scan_ring_usable(device);
	if (!usable)
		return;

	/* The place and the visibility. */
	dwords = ghci_cmd_disp_set_cursor(packet, DISPLAY_INDEX, display->cursor_x, display->cursor_y, (uint32_t)display->cursor_show);
	error = ring_put(device, packet, dwords, &placed);
	if (error != 0 && !placed)
		return;

	/* Succeeded: nothing waits. */
	display->cursor_due = 0;
}

/*
 * Turns the display dark through the ring: DISP_SET with ENABLE off,
 * whose completion says the scan-out stopped (every pending flip is
 * cancelled with it).  A placement a recovery refused is placed again by
 * the service thread; when the ring does not run, nothing is turned.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2,
 * ENABLE 1 to 0 stops the scan-out at the frame's end and cancels the
 * pending flips).
 */
static void
dark(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t packet[GHCI_DW_DISP_SET];
	struct ghci_disp_comp comp;
	uint32_t dwords;
	int placed;
	int usable;
	int error;

	/* A ring that does not run turns nothing. */
	display = &device->display;
	usable = ghci_scan_ring_usable(device);
	if (!usable) {
		host_cannot_turn(device);
		display->dark_placed = 0;
		return;
	}

	/* ENABLE off, with its completion. */
	comp_arm(device, COMP_DARK, 1, &comp);
	dwords = ghci_cmd_disp_set(packet, DISPLAY_INDEX, 0U, &comp);
	error = ring_put(device, packet, dwords, &placed);
	if (error == EAGAIN && !placed) {
		/* A recovery runs: the service thread places it again once the recovery ends. */
		display->dark_placed = -1;
		return;
	}

	if (error != 0 && !placed) {
		/* A lost device turns nothing. */
		host_cannot_turn(device);
		display->dark_placed = 0;
		return;
	}

	/* The display is dark once the command runs; its completion says when. */
	display->enabled = 0;
	display->dark_placed = 1;
}

/*
 * Records that the ring cannot turn the display: what it scans now and
 * what the placed flips would have shown may still be read, and goes to
 * the quarantine (host_unsure).
 */
static void
host_cannot_turn(
	struct ghci_device *device)
{
	/* Nothing seen to stop. */
	device->display.host_unsure = 1;
	flips_forget(device);
}

/* Forgets the flips placed whose records will not come: their images are let go of, not shown. */
static void
flips_forget(
	struct ghci_device *device)
{
	struct ghci_display *display;

	/* Plane 0's image goes (to the quarantine when the display was not seen to stop); the cursor's surface is free again. */
	display = &device->display;
	if (display->flips[0].placed)
		scan_release(device, display->flips[0].image.memory);
	display->flips[0].placed = 0;
	display->flips[1].placed = 0;
}

/*
 * Places the snapshot a reset asked for, and settles the display once it
 * completes (the service thread's round, the lock held).
 */
static void
resync_serve(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t packet[GHCI_DW_DISP_QUERY_STATE];
	struct ghci_disp_comp comp;
	uint32_t dwords;
	uint32_t status;
	uint64_t value;
	int usable;
	int placed;
	int state;
	int error;

	/* Nothing to settle. */
	display = &device->display;
	if (display->resync == 0)
		return;

	/* The snapshot, once the ring takes it. */
	if (display->resync < 0) {
		usable = ghci_scan_ring_usable(device);
		if (!usable)
			return;
		comp_arm(device, COMP_STATE, 1, &comp);
		dwords = ghci_cmd_disp_query_state(packet, DISPLAY_INDEX, page_bus(device, STATE_OFFSET), GHCI_DISP_STATE_BYTES, &comp);
		error = ring_put(device, packet, dwords, &placed);
		if (error != 0 && !placed)
			return;
		display->resync = 1;
		return;
	}

	/* Its completion; a cancelled one (another reset) is placed again. */
	state = comp_state(device, COMP_STATE, &status, &value);
	if (state == COMP_PENDING)
		return;
	if (state == COMP_CANCELLED) {
		display->resync = -1;
		return;
	}

	/* Succeeded: what the reset left is known. */
	resync_end(device);
}

/*
 * Settles the display with the snapshot placed after a reset: every
 * display command placed before it has finished, so a flip whose record
 * did not come was lost with the ring and is placed again (plane 0's
 * image back at the head of the queue, the cursor's surface waiting),
 * a dark command whose completion did not come is placed again, and the
 * snapshot says whether the display is lit and which mode it holds.  A
 * lighting cut after its flip was taken but before ENABLE leaves the
 * image shown on a dark display, which scans nothing: the image goes
 * back to the head of the queue and lights the display again.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 13.2, a
 * control command starts after every earlier display command finished,
 * the records written included; GhciDisplayState's ENABLE and
 * active_mode, unset as size 0).
 */
static void
resync_end(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_mode active;
	struct ghci_display_flip *flip;
	const uint8_t *state;
	uint32_t status;
	uint64_t value;
	uint32_t flags;
	unsigned plane;
	int dark_state;

	/* The records that came; the flips without one were lost. */
	display = &device->display;
	records_take(device);
	for (plane = 0U; plane < 2U; plane++) {
		flip = &display->flips[plane];
		if (!flip->placed)
			continue;
		flip->placed = 0;
		if (plane == 0U)
			queue_push_front(display, &flip->image);
		else
			display->cursor_pending = flip->surface;
	}

	/* A dark command that did not run is placed again. */
	if (display->dark_placed > 0) {
		dark_state = comp_state(device, COMP_DARK, &status, &value);
		if (dark_state != COMP_DONE)
			display->dark_placed = -1;
	}

	/* Whether the display is lit, and the mode it holds. */
	state = page_view(device, STATE_OFFSET);
	flags = (uint32_t)state[GHCI_DISP_STATE_FLAGS] | ((uint32_t)state[GHCI_DISP_STATE_FLAGS + 1U] << 8);
	display->enabled = 0;
	if ((flags & GHCI_DISP_STATE_ENABLE) != 0U)
		display->enabled = 1;
	mode_read(state + GHCI_DISP_STATE_MODE, &active);
	if (active.width == 0U || active.height == 0U)
		display->mode_set = GHCI_DISPLAY_NO_MODE;
	else if (display->mode_set != GHCI_DISPLAY_NO_MODE && !mode_same(&display->modes[display->mode_set], &active))
		display->mode_set = GHCI_DISPLAY_NO_MODE;

	/* A lighting cut before ENABLE: the image shown on the dark display lights it again. */
	if (display->light_lost &&
	    !display->enabled &&
	    !display->ending &&
	    display->shown.base == display->light_image.base &&
	    display->shown.sequence == display->light_image.sequence &&
	    display->shown.copy_frame == display->light_image.copy_frame) {
		queue_push_front(display, &display->shown);
		image_clear(&display->shown);
	}

	/* Settled. */
	display->light_lost = 0;
	display->resync = 0;
}

/*
 * Keeps the event ring set up as the driver wants it: the connection
 * when the display senses it, and the errors unless too many came this
 * second.  The setup's completion is the ring's first sequence number;
 * the records of the ring before it are read first (the service thread's
 * round, the lock held).
 */
static void
events_serve(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t status;
	uint64_t first;
	uint32_t want;
	int changed;
	int state;

	/* The events wanted. */
	display = &device->display;
	want = EVENTS_ERRORS;
	if (display->error_masked)
		want = 0U;
	if (display->sense)
		want |= GHCI_DISP_EVENT_MASK_HOTPLUG;

	/* A setup placed: its completion, after the records of the ring before it. */
	if (display->events_state == EVENTS_PLACED) {
		state = comp_state(device, COMP_EVENTS, &status, &first);
		if (state == COMP_PENDING)
			return;
		if (state == COMP_CANCELLED) {
			display->events_state = EVENTS_OFF;
			return;
		}
		changed = 0;
		(void)events_take(device, &changed);
		if (changed)
			topology_move(device);
		display->events_state = EVENTS_RUNNING;
		display->events_first = first;
		display->events_next = first;
	}

	/* A ring that runs as wanted. */
	if (display->events_state == EVENTS_RUNNING && display->events_mask == want)
		return;

	/* A new setup. */
	events_setup(device, want);
}

/* Places a setup of the event ring taking the events of mask; a refused one is placed at the next round. */
static void
events_setup(
	struct ghci_device *device,
	uint32_t mask)
{
	struct ghci_display *display;
	uint32_t packet[GHCI_DW_DISP_EVENT_SETUP];
	struct ghci_disp_comp comp;
	uint32_t dwords;
	int usable;
	int placed;
	int error;

	/* The ring must take it. */
	display = &device->display;
	usable = ghci_scan_ring_usable(device);
	if (!usable)
		return;

	/* The setup, its completion with an interrupt. */
	comp_arm(device, COMP_EVENTS, 1, &comp);
	dwords = ghci_cmd_disp_event_setup(packet, DISPLAY_INDEX, page_bus(device, EVENTS_OFFSET), EVENTS_CAPACITY, mask, &comp);
	error = ring_put(device, packet, dwords, &placed);
	if (error != 0 && !placed)
		return;

	/* Succeeded: the setup waits for its completion. */
	display->events_state = EVENTS_PLACED;
	display->events_mask = mask;
}

/*
 * Reads the records of the event ring from the next sequence number on,
 * until one is not written yet: a record further on than the next says
 * that the ring went round and the events between were lost.  changed is
 * set when the output's connection changed.  Returns the records read.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3, the
 * record of sequence seq at slot (seq - setup_next_seq) % capacity;
 * section 13.2, GhciDisplayEventRecord, a ring gone round seen by its
 * sequence numbers).
 */
static int
events_take(
	struct ghci_device *device,
	int *changed)
{
	struct ghci_display *display;
	const uint8_t *record;
	uint64_t slot;
	uint64_t seq;
	uint32_t type;
	uint32_t payload;
	int count;

	/* A ring that runs, or whose new setup is placed. */
	display = &device->display;
	if (display->events_state == EVENTS_OFF)
		return 0;

	/* Each record written, in order, at most a ring of them. */
	for (count = 0; count < (int)EVENTS_CAPACITY; count++) {
		slot = (display->events_next - display->events_first) % EVENTS_CAPACITY;
		record = page_view(device, EVENTS_OFFSET + GHCI_DISP_EVENT_RECORD_BYTES * (uint32_t)slot);
		seq = page_read64(device, EVENTS_OFFSET + GHCI_DISP_EVENT_RECORD_BYTES * (uint32_t)slot);
		if (seq == 0U || seq < display->events_next)
			break;

		/* The ring went round: the events between are lost. */
		if (seq > display->events_next) {
			kern_logf("ghci: %s: %llu display events lost\n", device->name, (unsigned long long)(seq - display->events_next));
			display->events_next = seq;
		}

		/* The event. */
		type = load32(record, GHCI_DISP_EVENT_TYPE);
		payload = load32(record, GHCI_DISP_EVENT_PAYLOAD);
		event_handle(device, type, payload, changed);
		display->events_next++;
	}

	/* Succeeded: the records read. */
	return count;
}

/*
 * Takes one display event: a sink's connection or disconnection (a new
 * connection is a new generation of the output), an underflow, or an
 * error (a scan-out read that failed, a record or a completion that
 * could not be written).
 */
static void
event_handle(
	struct ghci_device *device,
	uint32_t type,
	uint32_t payload,
	int *changed)
{
	struct ghci_display *display;
	int connected;

	/* Picks the event's kind. */
	display = &device->display;
	switch (type) {
	case GHCI_DISP_EVENT_HOTPLUG:
		/* The connection, when it changed. */
		connected = 0;
		if ((payload & 1U) != 0U)
			connected = 1;
		if (connected == display->connected)
			break;
		display->connected = connected;
		if (connected)
			display->generation++;
		kern_logf("ghci: %s: display %s\n", device->name, connected ? "connected" : "disconnected");
		topology_move(device);
		*changed = 1;
		break;
	case GHCI_DISP_EVENT_UNDERFLOW:
		/* The memory did not keep up; it is counted only. */
		display->underflows++;
		display->error_count++;
		display->errors_total++;
		break;
	case GHCI_DISP_EVENT_ERROR:
		/* A read that failed counts toward turning the display dark; the others are logged. */
		display->error_count++;
		display->errors_total++;
		if (payload == GHCI_DISP_ERROR_SCANOUT_READ)
			display->read_errors++;
		else if (payload == GHCI_DISP_ERROR_RECORD_WRITE)
			kern_logf("ghci: %s: a flip's record was not written\n", device->name);
		else if (payload == GHCI_DISP_ERROR_COMP_WRITE)
			kern_logf("ghci: %s: a display command's completion was not written\n", device->name);
		break;
	default:
		/* A vertical sync, or an event the driver does not know. */
		break;
	}
}

/*
 * Stops the event ring and waits for it, so that nothing writes the page
 * once it goes.  The caller holds no lock of the device.  Returns 0, or
 * the error of the command (the page may then still be written).
 */
static int
events_stop(
	struct ghci_device *device)
{
	uint32_t packet[GHCI_DW_DISP_EVENT_SETUP];
	struct ghci_disp_comp comp;
	uint32_t status;
	uint64_t value;
	uint32_t dwords;
	int error;

	/* The setup without a ring, its completion waited for. */
	comp_arm(device, COMP_EVENTS, 0, &comp);
	dwords = ghci_cmd_disp_event_setup(packet, DISPLAY_INDEX, 0U, 0U, 0U, &comp);
	error = command_run(device, packet, dwords, COMP_EVENTS, &status, &value);
	if (error != 0)
		return error;

	/* Succeeded: the ring is off. */
	device->display.events_state = EVENTS_OFF;
	return 0;
}

/*
 * Lets go of memory the display scanned: at the next flush when the
 * driver saw it stop reading it, through the quarantine otherwise (the
 * ring could not turn the display).  The lock is held.
 */
static void
scan_release(
	struct ghci_device *device,
	struct ghci_allocation *memory)
{
	struct ghci_display *display;

	/* A driver's frame is not held. */
	if (memory == NULL)
		return;

	/* Seen to stop: the ordinary drop. */
	display = &device->display;
	if (!display->host_unsure) {
		ghci_scan_drop(device, memory);
		return;
	}

	/* The list is as long as the drops'. */
	if (display->unsure_count >= GHCI_DISPLAY_DROPS) {
		kern_logf("ghci: %s: display holds more memory than it can let go of\n", device->name);
		return;
	}

	/* The hold goes at the next flush, and the memory to the quarantine with its last owner. */
	display->unsure[display->unsure_count] = memory;
	display->unsure_count++;
}

/*
 * Lets go of the memory dropped meanwhile, and gives the boot image back
 * once the driver's first flip was taken and nothing else draws to it.
 * The service thread only tries the locks (a holder may be waiting for
 * the ring's room, which only its hang check ends) and leaves the boot
 * image for a later flush; what it cannot take now waits.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 13.3, the
 * boot framebuffer reclaimed after the takeover, once no console writes
 * remain).
 */
static void
scan_flush(
	struct ghci_device *device,
	int service)
{
	struct ghci_display *display;
	struct ghci_allocation *drops[GHCI_DISPLAY_DROPS];
	struct ghci_allocation *unsure[GHCI_DISPLAY_DROPS];
	unsigned count;
	unsigned unsure_count;
	unsigned index;
	int release;
	int locked;
	int end;

	/* The device's lock, then the display's to take the lists and the release. */
	display = &device->display;
	if (service) {
		locked = mutex_trylock(&device->lock);
		if (!locked)
			return;
		locked = mutex_trylock(&device->display_lock);
		if (!locked) {
			mutex_unlock(&device->lock);
			return;
		}
	} else {
		mutex_lock(&device->lock);
		mutex_lock(&device->display_lock);
	}

	count = display->drop_count;
	for (index = 0U; index < count; index++)
		drops[index] = display->drops[index];
	display->drop_count = 0U;
	unsure_count = display->unsure_count;
	for (index = 0U; index < unsure_count; index++)
		unsure[index] = display->unsure[index];
	display->unsure_count = 0U;

	/* The boot image goes back unless a flip of it was placed again meanwhile (its next own image retries). */
	release = 0;
	if (display->boot_release && !service) {
		display->boot_release = 0;
		if (!boot_in_use(display)) {
			release = 1;
			display->boot_kept = 0;
		}
	}

	/* The console is told once, for a device still here with a boot image out of the allocator. */
	end = 0;
	if (display->boot_end_due && !service) {
		display->boot_end_due = 0;
		display->boot_end_tried = 1;
		if (display->present && device->boot_reserved && device->bar_count != 0U)
			end = 1;
	}

	mutex_unlock(&device->display_lock);

	/* Each hold goes; memory nothing else holds goes back. */
	for (index = 0U; index < count; index++)
		ghci_memory_unhold(device, drops[index]);

	/* Memory the display may still read is marked first, so that its last owner puts it in the quarantine. */
	for (index = 0U; index < unsure_count; index++) {
		unsure[index]->scan_unsure = 1;
		ghci_memory_unhold(device, unsure[index]);
	}

	mutex_unlock(&device->lock);

	/* The boot image goes back to the allocator, with no lock held. */
	if (release) {
		ghci_memory_boot_release(device);
		kern_logf("ghci: %s: boot framebuffer given back\n", device->name);
	}

	/* The console leaves the display, with no lock held. */
	if (end)
		boot_end(device);
}

/*
 * Tells the console it leaves the display for good, with the device's
 * whole BARs as the memory about to go to others, and gives the boot image
 * back once it did (0), the device is still here, and no flip of the boot
 * image is placed or shown; otherwise the image is kept as it was.  A read
 * of a register first lets every write the console posted to the device
 * reach it before the memory goes.  Called with no lock held.
 */
static void
boot_end(
	struct ghci_device *device)
{
	struct kern_text_range ranges[GHCI_BUS_RANGES];
	struct ghci_display *display;
	unsigned count;
	unsigned index;
	int release;
	int error;

	/* The device's BARs, as the bus glue found them. */
	display = &device->display;
	count = device->bar_count;
	for (index = 0U; index < count; index++) {
		ranges[index].physical = device->bars[index].physical;
		ranges[index].bytes = device->bars[index].bytes;
	}

	/* The console stops drawing on the display and removes what it maps of it. */
	error = kern_text_end(ranges, count);
	if (error != 0) {
		kern_logf("ghci: %s: boot framebuffer kept: the console cannot leave it (%d)\n", device->name, error);
		return;
	}

	mutex_lock(&device->lock);
	mutex_lock(&device->display_lock);

	/* Nothing of the console draws to the boot image any more; it goes back when nothing reads it. */
	display->boot_console = 0;
	release = 0;
	if (display->present) {
		/* The console's last posted writes reach the device before its memory is reused. */
		(void)ghci_read(device, GHCI_REG_MAGIC);
		if (display->boot_kept && !boot_in_use(display)) {
			display->boot_kept = 0;
			release = 1;
		}
	}

	mutex_unlock(&device->display_lock);
	mutex_unlock(&device->lock);

	/* The boot image goes back to the allocator, with no lock held. */
	if (release) {
		ghci_memory_boot_release(device);
		kern_logf("ghci: %s: boot framebuffer given back\n", device->name);
	}

	/* Succeeded: the console left the display. */
	return;
}

/* Reports whether an image is the boot image: the driver's own, at the boot display's address. */
static int
boot_image(
	const struct ghci_display *display,
	const struct ghci_display_image *image)
{
	/* Memory of the driver's own, not a copied frame, at the boot base. */
	if (image->memory == NULL && image->copy_frame < 0 && display->boot.base != 0U && image->base == display->boot.base)
		return 1;

	/* Any other image. */
	return 0;
}

/*
 * Reports whether the boot image is scanned, or a flip of it is placed
 * (the end of a lease lights it again).  The caller holds display_lock.
 */
static int
boot_in_use(
	const struct ghci_display *display)
{
	/* The display still scans it. */
	if (display->boot_scanning)
		return 1;

	/* A flip of it waits to be taken. */
	if (display->flips[0].placed && boot_image(display, &display->flips[0].image))
		return 1;

	/* Nothing reads it. */
	return 0;
}

/*
 * Begins the detach's turn of the display: the lease and the presents
 * waiting go, and the cursor is hidden.  The lock is held.
 */
static void
detach_begin(
	struct ghci_device *device)
{
	struct ghci_display *display;
	unsigned index;

	/* No lease, and nothing more placed for it. */
	display = &device->display;
	display->owner = NULL;
	display->lease = 0U;
	display->ending = 1;

	/* The presents that waited are not shown. */
	for (index = 0U; index < display->queue_count; index++)
		ghci_scan_drop(device, display->queue[index].memory);
	display->queue_count = 0U;

	/*
	 * No cursor.  A dark command an ended lease placed (or one a
	 * recovery refused) stays: its completion comes before the image it
	 * turned off is let go of.
	 */
	display->cursor_image = 0;
	display->cursor_pending = -1;
	ghci_scan_cursor_state(device, display->cursor_x, display->cursor_y, 0);
}

/*
 * Does one step of the detach's turn of the display, the lock held: once
 * the records of every flip placed before came and the dark command of an
 * ended lease ran, the display is turned (turned set) to the boot image
 * again while it is kept, or dark; a boot display still scanned, or a
 * dark display without a boot image, needs nothing.  The image a dark
 * display showed is let go of.  Returns 1 once the turn is done, 0 while
 * it waits.
 */
static int
detach_step(
	struct ghci_device *device,
	int *turned)
{
	struct ghci_display *display;
	uint32_t status;
	uint64_t value;
	int state;
	int error;

	/* The records of the flips placed, and a lighting's completion. */
	display = &device->display;
	records_take(device);
	light_check(device);
	if (display->flips[0].placed || display->flips[1].placed)
		return 0;

	/* A dark command a recovery refused is placed again. */
	if (display->dark_placed < 0) {
		dark(device);
		return 0;
	}

	/* The dark command's completion: the display no longer scans; a cancelled one is placed again. */
	if (display->dark_placed > 0) {
		state = comp_state(device, COMP_DARK, &status, &value);
		if (state == COMP_PENDING)
			return 0;
		if (state == COMP_CANCELLED) {
			display->dark_placed = -1;
			return 0;
		}
		display->host_unsure = 0;
		display->dark_placed = 0;
	}

	/* A dark display scans nothing: the image it showed goes. */
	if (!display->enabled && display->shown.memory != NULL) {
		scan_release(device, display->shown.memory);
		image_clear(&display->shown);
	}

	/* The turn, once. */
	if (*turned)
		return 1;
	*turned = 1;
	if (display->boot_scanning && display->enabled)
		return 1;

	/* The boot image again, while it is kept; a lighting the gate refused is tried at the next step. */
	if (display->boot_kept && display->boot.base != 0U) {
		error = light(device, &display->boot);
		if (error != 0)
			*turned = 0;
		return 0;
	}

	/* Dark, unless it is. */
	if (display->enabled) {
		dark(device);
		return 0;
	}

	/* Succeeded: the display is dark. */
	return 1;
}

/*
 * Gives back the display's own memory at the detach: the frames' mappings
 * into session 0 first (each holds its frame until its unmap's fence),
 * then the frames, the cursor's surfaces, the idle frame and the page
 * (to the quarantine when the display may still read or write them).  The
 * caller holds no lock of the device.
 */
static void
buffers_give_back(
	struct ghci_device *device,
	int unsure)
{
	struct ghci_display *display;
	unsigned index;

	/* The mappings the blitter or the copy program copied through. */
	display = &device->display;
	for (index = 0U; index < 2U; index++) {
		if (display->frame_maps[index] != NULL)
			ghci_vm_unmap(device, display->frame_maps[index]);
		display->frame_maps[index] = NULL;
	}

	/* The frames, the surfaces, the idle frame and the page. */
	for (index = 0U; index < 2U; index++) {
		scan_give_back(device, display->frames[index], unsure);
		display->frames[index] = NULL;
		scan_give_back(device, display->surfaces[index], unsure);
		display->surfaces[index] = NULL;
	}

	scan_give_back(device, display->idle_frame, unsure);
	display->idle_frame = NULL;
	scan_give_back(device, display->page, unsure);
	display->page = NULL;
}

/*
 * Counts the scan-out errors of this second: too many leave the error
 * events out of the event ring until the next second (events_serve()
 * sets it up), and a failed read in round after round turns the display
 * dark (the image it scans is bad).
 */
static void
errors_serve(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint64_t period;
	uint64_t now;

	/* A new second takes the errors again and starts the count again. */
	display = &device->display;
	now = clock_ticks();
	period = kern_ms_to_ticks(ERROR_PERIOD_MS);
	if (now - display->error_tick >= period) {
		display->error_tick = now;
		display->error_count = 0U;
		display->error_masked = 0;
	}

	/* Too many errors this second: the ring leaves them out until the next. */
	if (display->error_count > ERROR_LIMIT && !display->error_masked)
		display->error_masked = 1;

	/* A round without a failed read ends the run of them. */
	if (display->read_errors == 0U) {
		display->read_error_runs = 0U;
		return;
	}

	/* A failed read round after round: the image is bad, and the display goes dark until the next present. */
	display->read_errors = 0U;
	display->read_error_runs++;
	if (display->read_error_runs < READ_ERROR_RUNS)
		return;
	if (!display->enabled || display->flips[0].placed || display->ending)
		return;
	kern_logf("ghci: %s: display reads fail; turning it off\n", device->name);
	display->read_error_runs = 0U;
	dark(device);
	display->dark_placed = 0;
}

/* Moves the output's event sequence, which the events operation reads under the interrupt's lock. */
static void
topology_move(
	struct ghci_device *device)
{
	unsigned long irq;

	/* The sequence only grows; a reader sees the change at its next look. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->display.topology++;

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/* Reports the mode the idle frame is scanned out in: 1280 x 720 when the display has it, else its largest. */
static unsigned
idle_mode(
	const struct ghci_display *display)
{
	unsigned index;

	/* The HDMI mode every monitor takes. */
	for (index = 0U; index < display->mode_count; index++) {
		if (display->modes[index].width == 1280U && display->modes[index].height == 720U)
			return index;
	}

	/* Otherwise the largest. */
	index = ghci_scan_largest_mode(display);
	return index;
}

/*
 * Lights the display on the driver's idle frame once a sink is connected
 * and no lease holds the display, once for each generation of the output
 * (a connection): a lease that ends leaves it dark, and a boot display
 * keeps its image.  The lock is held.
 */
static void
idle_serve(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_image image;
	unsigned mode;
	int usable;
	int error;

	/* A connected sink, no lease, nothing lit, an idle frame, not yet for this connection, nothing to settle. */
	display = &device->display;
	if (!display->connected || display->owner != NULL || display->ending || display->enabled || display->resync != 0)
		return;
	if (display->idle_frame == NULL || display->idle_generation == display->generation)
		return;

	/* The ring must take the display's commands. */
	usable = ghci_scan_ring_usable(device);
	if (!usable)
		return;

	/* The idle frame in its mode, rows on the line, blue first; the driver's frame is not held. */
	mode = idle_mode(display);
	image_clear(&image);
	image.base = display->idle_frame->bus;
	image.stride = ghci_scan_stride(display->modes[mode].width);
	image.format = GHCI_FORMAT_B8G8R8A8_UNORM;
	image.mode = mode;

	/* Lit once for this connection; a lighting a recovery refused is tried at the next round. */
	error = light(device, &image);
	if (error != 0)
		return;
	display->idle_generation = display->generation;
}

/*
 * Takes the idle frame (one any mode fits in), black: host memory below the
 * GPU's limit, which comes cleared and which the display scans on a
 * discrete GPU as well, so that it needs neither local memory nor the
 * window.  Returns 0, or ENOMEM.
 */
static int
idle_frame_take(
	struct ghci_device *device)
{
	struct ghci_allocation *frame;
	uint64_t bytes;
	int error;

	/* A frame any mode fits in, rows on the line. */
	bytes = ghci_scan_frame_bytes(&device->display);
	error = ghci_scan_alloc(device, bytes, 0, &frame);
	if (error != 0)
		return error;

	/* Succeeded: the idle frame is the display's. */
	device->display.idle_frame = frame;
	return 0;
}

/*
 * Lets go of the display's hold on memory of its own; NULL is nothing.
 * unsure marks it first so that it goes to the quarantine, as the
 * display may still read or write it.
 */
static void
scan_give_back(
	struct ghci_device *device,
	struct ghci_allocation *memory,
	int unsure)
{
	/* Nothing to give. */
	if (memory == NULL)
		return;

	/* The hold goes under the device's lock; the memory goes back once nothing else holds it. */
	mutex_lock(&device->lock);

	if (unsure)
		memory->scan_unsure = 1;
	ghci_memory_unhold(device, memory);

	mutex_unlock(&device->lock);
}

/* Reads a little-endian 16-bit field of a record in memory. */
static uint32_t
load16(
	const uint8_t *bytes,
	uint32_t offset)
{
	return (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1U] << 8);
}

/* Reads a little-endian 32-bit field of a record in memory. */
static uint32_t
load32(
	const uint8_t *bytes,
	uint32_t offset)
{
	uint32_t value;

	/* The four bytes, the lowest first. */
	value = (uint32_t)bytes[offset];
	value |= (uint32_t)bytes[offset + 1U] << 8;
	value |= (uint32_t)bytes[offset + 2U] << 16;
	value |= (uint32_t)bytes[offset + 3U] << 24;

	/* Succeeded: the field. */
	return value;
}

/* Writes a little-endian 16-bit field of a record in memory. */
static void
store16(
	uint8_t *bytes,
	uint32_t offset,
	uint32_t value)
{
	bytes[offset] = (uint8_t)value;
	bytes[offset + 1U] = (uint8_t)(value >> 8);
}

/* Writes a little-endian 32-bit field of a record in memory. */
static void
store32(
	uint8_t *bytes,
	uint32_t offset,
	uint32_t value)
{
	bytes[offset] = (uint8_t)value;
	bytes[offset + 1U] = (uint8_t)(value >> 8);
	bytes[offset + 2U] = (uint8_t)(value >> 16);
	bytes[offset + 3U] = (uint8_t)(value >> 24);
}
