/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scan-out side of a GHCI device's display: the display controller of
 * the DISP2 capability, a unit of the GPU that the ring drives.
 *
 * While the command processor runs, only the ring changes what the
 * controller shows: CMD_DISP_SET writes its registers and CMD_FLIP makes
 * an image (a 32-bit physical address) a plane's pending one, taken at
 * the next vertical sync (plane 0) or once the cursor's image is read
 * (plane 1).  At the take the GPU writes the flip's 16-byte record,
 * {value, FRAME_COUNT, timestamp}, into the record page (64 bytes past
 * the fence word of the fence page, 16 bytes a plane), latches the value
 * in TAKEN0 or TAKEN1 and raises FLIP_DONE or CURSOR_FLIP_DONE in
 * DISP_IRQ_STATUS, which IRQ_STATUS[3] sums up.  The host writes the
 * scan-out registers itself only while the processor does not run: at
 * the attach, during a reset, at the detach of a device that no longer
 * runs, and for a panic once it has halted the processor.
 *
 * Every placement passes the gate of the ring with the display's lock
 * held and does not wait for a recovery: a run the gate refuses is not
 * recorded and is placed again once the recovery has ended.  The service
 * thread's placements wait for nothing at all, as its hang check is what
 * ends every other wait: it only tries the locks, and enters the ring
 * only when it is free and has room for the run at once.  The driver
 * places a plane's next flip only once the last one's record has come,
 * so the controller's one pending flip per plane never makes the ring
 * wait; the presents in between wait in a queue of two.  A flip value is
 * 31 bits (bit 31 of a record says the flip was cancelled), and the
 * record slot is cleared before each flip, so a record names its flip
 * without any wrap.
 *
 * A lease ends by marking it ended; the controller is then turned dark by
 * the ring, and the memory it scanned is let go once the cancelled
 * records and the dark packet's fence have come.  Memory the host turned
 * away from without seeing the processor halt and the controller go idle
 * goes to the quarantine instead.  The service thread
 * reads the records, counts the scan-out errors, watches the connection
 * and finishes ended leases.  A reset of the command processor loses the
 * flips still in the ring; the driver counts them again from TAKEN0,
 * TAKEN1 and STATUS while the processor is idle, and places the lost
 * ones again.
 *
 * A boot display the firmware left running keeps being scanned until the
 * driver's own first flip is taken; what it read of local memory then
 * goes back to the allocator, unless something of the kernel may still
 * draw to it.  While the boot image is kept, the detach and a panic show
 * it again.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/poll.h>
#include <kern/text-display.h>
#include <uapi/errno.h>

#include "display-scan.h"
#include "ghci.h"
#include "ghci-cmd.h"

/* The record page: the 64 bytes past the fence word, 16 bytes a plane. */
#define RECORD_OFFSET			64U

/*
 * How long the driver waits for the controller: to be idle after it went
 * dark, for a record still to be written after a reset, for the detach's
 * last flips, and for a halt of the processor before the host writes the
 * controller.  A panic counts register reads instead of sleeping.
 */
#define DC_IDLE_MS			100U
#define RECORD_WAIT_MS			100U
#define DETACH_WAIT_MS			200U
#define PANIC_READS			1000000U

/* How often a waiter looks at the record page when no interrupt comes. */
#define FLIP_POLL_MS			10U

/* How often the service thread asks the transmitter whether a sink is there. */
#define HOTPLUG_PERIOD_MS		1000U

/*
 * The scan-out errors: past ERROR_LIMIT error interrupts in a second the
 * interrupt is masked for the rest of that second, and READ_ERROR_RUNS
 * rounds in a row with a read error turn the controller dark.
 */
#define ERROR_PERIOD_MS			1000U
#define ERROR_LIMIT			30U
#define READ_ERROR_RUNS			3U

/* The STATUS bits that count as scan-out errors (sticky, write 1 to clear). */
#define DC_ERRORS			(GHCI_DC_ST_UNDERFLOW | GHCI_DC_ST_READ_ERROR | GHCI_DC_ST_RECORD_ERROR | GHCI_DC_ST_BAD_WRITE)

/* The display events the driver takes: the flips' completions, the connection and the errors. */
#define DC_EVENTS			(GHCI_DC_IRQ_FLIP_DONE | GHCI_DC_IRQ_CURSOR_DONE | GHCI_DC_IRQ_HOTPLUG | GHCI_DC_IRQ_ERROR)

/* The largest flip value: 31 bits. */
#define FLIP_VALUE_MAX			0x7fffffffU

/* The 32-bit physical addresses the controller takes. */
#define SCAN_LIMIT			0x100000000ULL

/* The MODE registers' fields: a count in the low half, another in the high half. */
#define MODE_LOW(word)			((word) & 0xffffU)
#define MODE_HIGH(word)			((word) >> 16)

/* The register pairs of the run that lights the controller, and its dwords. */
#define LIGHT_PAIRS			12U
#define LIGHT_DWORDS			(3U + (1U + 2U * LIGHT_PAIRS) + GHCI_DW_FLIP + 3U + GHCI_DW_FLIP)

/* The dwords of a dark packet: one register pair. */
#define DARK_DWORDS			3U

static int modes_read(struct ghci_device *device);
static void boot_take(struct ghci_device *device);
static void image_clear(struct ghci_display_image *image);
static uint32_t control_word(struct ghci_device *device, uint32_t format);
static uint32_t record_address(struct ghci_device *device, unsigned plane);
static volatile uint32_t *record_slot(struct ghci_device *device, unsigned plane);
static uint32_t value_take(struct ghci_display *display);
static int ring_put(struct ghci_device *device, const uint32_t *packets, uint32_t dwords, struct ghci_ticket *ticket, int *placed);
static void records_take(struct ghci_device *device);
static void taken(struct ghci_device *device, unsigned plane, int cancelled);
static void queue_kick(struct ghci_device *device);
static void queue_push_front(struct ghci_display *display, const struct ghci_display_image *image);
static void queue_pop(struct ghci_display *display);
static uint32_t timing_pairs(const struct ghci_display_mode *timing, const struct ghci_display_image *image, uint32_t *offsets, uint32_t *values);
static int light(struct ghci_device *device, const struct ghci_display_image *image);
static int flip_primary(struct ghci_device *device, const struct ghci_display_image *image);
static int flip_cursor(struct ghci_device *device, int surface);
static void cursor_kick(struct ghci_device *device);
static void dark(struct ghci_device *device);
static int wait_idle(struct ghci_device *device, int spin);
static void dark_by_host(struct ghci_device *device, int halted);
static void light_by_host(struct ghci_device *device, const struct ghci_display_image *image, int spin);
static int halt_for_host(struct ghci_device *device, int spin);
static void flips_forget(struct ghci_device *device);
static void scan_release(struct ghci_device *device, struct ghci_allocation *memory);
static void scan_flush(struct ghci_device *device, int service);
static void boot_end(struct ghci_device *device);
static int boot_image(const struct ghci_display *display, const struct ghci_display_image *image);
static int boot_in_use(const struct ghci_display *display);
static void detach_begin(struct ghci_device *device);
static int detach_step(struct ghci_device *device, int *turned);
static void buffers_give_back(struct ghci_device *device);
static void errors_serve(struct ghci_device *device);
static void irq_mask_set(struct ghci_device *device, int masked);
static int connection_serve(struct ghci_device *device);
static void topology_move(struct ghci_device *device);
static unsigned idle_mode(const struct ghci_display *display);
static void idle_serve(struct ghci_device *device);
static int idle_frame_take(struct ghci_device *device);
static void scan_give_back(struct ghci_device *device, struct ghci_allocation *memory);

/*
 * Finds the display controller of the DISP2 capability, before the
 * command processor runs: a boot display it finds running keeps running,
 * and a controller found dark is reset and left dark.
 *
 * Returns 0, also for a device without a display (display.present stays
 * 0); a capability the driver cannot use is logged and leaves the device
 * without a display.
 */
int
ghci_display_attach(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t registers;
	uint32_t version;
	uint32_t ctrl;
	uint32_t connected_word;
	int has_display;
	int has_cursor;
	int error;

	/* The record starts empty, with its wait queue. */
	display = &device->display;
	kern_memset(display, 0, sizeof(*display));
	waitq_init(&display->flip_wake, "ghci flip");

	/* A GPU without DISP2 (and its capability bit) has no display; DISP alone is not served. */
	has_display = ghci_caps_has(&device->caps, GHCI_HAS_DISPLAY);
	if (!has_display)
		return 0;

	/* The capability's window of 256 bytes, of the one version the driver knows. */
	registers = device->caps.block_offset[GHCI_EXTCAP_ID_DISP2];
	version = device->caps.block_version[GHCI_EXTCAP_ID_DISP2];
	if (version != GHCI_DC_BLOCK_VERSION) {
		kern_logf("ghci: %s: DISP2 version %u is not known; no display\n", device->name, (unsigned)version);
		return 0;
	}

	if (device->register_bytes < GHCI_DC_WINDOW || registers > device->register_bytes - GHCI_DC_WINDOW) {
		kern_logf("ghci: %s: DISP2 block at %x is outside the window; no display\n", device->name, (unsigned)registers);
		return 0;
	}

	/* The controller's identity and what it can do; the cursor needs the plane and HW_CURSOR. */
	display->registers = registers;
	display->controller_id = ghci_read(device, registers + GHCI_DC_ID);
	display->controller_version = ghci_read(device, registers + GHCI_DC_VERSION);
	display->controller_caps = ghci_read(device, registers + GHCI_DC_CAPS);
	has_cursor = ghci_caps_has(&device->caps, GHCI_HAS_HW_CURSOR);
	if ((display->controller_caps & GHCI_DC_CAPS_CURSOR) != 0U && has_cursor)
		display->cursor = 1;

	/* The modes, from the controller's own table. */
	error = modes_read(device);
	if (error != 0) {
		kern_logf("ghci: %s: display controller names no usable mode; no display\n", device->name);
		return 0;
	}

	/*
	 * A controller the firmware left running keeps scanning its image
	 * until the driver's first flip is taken.  One found dark is reset
	 * and left dark: the host writes it directly here, before the
	 * command processor runs; once it runs, the ring alone changes what
	 * the controller shows.
	 */
	ctrl = ghci_read(device, registers + GHCI_DC_CTRL);
	if ((ctrl & GHCI_DC_CTRL_ENABLE) != 0U) {
		boot_take(device);
	} else {
		ghci_write(device, registers + GHCI_DC_CTRL, GHCI_DC_CTRL_RESET);
		ghci_write(device, registers + GHCI_DC_CTRL, control_word(device, GHCI_DC_FMT_BGRA8));
		error = wait_idle(device, 0);
		if (error != 0)
			kern_logf("ghci: %s: display controller not idle\n", device->name);
		display->mode = ghci_scan_largest_mode(display);
		display->format = GHCI_DC_FMT_BGRA8;
		image_clear(&display->shown);
		image_clear(&display->boot);
	}

	/* The sticky errors and the events before now are cleared; the driver takes its events. */
	ghci_write(device, registers + GHCI_DC_STATUS, DC_ERRORS);
	ghci_write(device, registers + GHCI_DC_IRQ_STATUS, GHCI_DC_IRQ_ALL);
	display->irq_enable = DC_EVENTS;
	ghci_write(device, registers + GHCI_DC_IRQ_ENABLE, display->irq_enable);

	/* Whether a sink is connected, as the controller was last told (1 when nobody told it). */
	connected_word = ghci_read(device, registers + GHCI_DC_CONNECTED);
	if ((connected_word & 1U) != 0U)
		display->connected = 1;

	/* An external HDMI transmitter is set up now, unless a boot display already drives it. */
	if ((display->controller_caps & GHCI_DC_CAPS_TRANSMITTER) != 0U && !display->boot_scanning) {
		error = ghci_adv7511_start(device, registers);
		if (error != 0)
			kern_logf("ghci: %s: HDMI transmitter not set up (%d)\n", device->name, error);
	}

	/* The idle frame, without which the display only lights for a lease. */
	error = idle_frame_take(device);
	if (error != 0)
		kern_logf("ghci: %s: no idle frame for the display (%d)\n", device->name, error);

	/* The display exists, without a lease, at its first generation and topology. */
	display->generation = GHCI_DISPLAY_FIRST_GENERATION;
	display->topology = 1U;
	display->hotplug_tick = clock_ticks();
	display->next_lease = 1U;
	display->next_value = 1U;
	display->cursor_shown = -1;
	display->cursor_pending = -1;
	display->copy_frame_busy = -1;
	display->error_tick = clock_ticks();
	display->present = 1;

	/* Succeeded: the display is ready for a lease. */
	kern_logf("ghci: %s: display %08x, %u modes%s%s\n",
	    device->name,
	    (unsigned)display->controller_id,
	    display->mode_count,
	    display->cursor ? ", cursor" : "",
	    display->boot_scanning ? ", boot display kept" : "");
	return 0;
}

/*
 * Turns the display for the detach: the lease's images go, and the
 * controller shows the boot image again while it is kept, or goes dark.
 * The ring does it while it runs; the host does it otherwise, once the
 * processor has halted.  The display's memory goes back.  The caller
 * holds no lock of the device; the detach waits a bounded time.
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
	int halted;
	int turned;
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
	 * controller (the boot image again, or dark) and waits for that.
	 */
	turned = 0;
	deadline = clock_ticks() + kern_ms_to_ticks(DETACH_WAIT_MS);
	for (;;) {
		mark = ghci_scan_mark(device);
		mutex_lock(&device->display_lock);

		done = detach_step(device, &turned);

		mutex_unlock(&device->display_lock);

		/* Ends once the controller shows what it should, or at the deadline. */
		now = clock_ticks();
		if (done || now >= deadline)
			break;

		ghci_scan_sleep(device, mark, deadline);
	}

	/* What the ring did not finish, the host does once the processor has halted. */
	mutex_lock(&device->display_lock);

	if (!done) {
		kern_logf("ghci: %s: display not turned by the ring; turned by the host\n", device->name);
		error = halt_for_host(device, 0);
		if (error != 0)
			kern_logf("ghci: %s: processor did not halt for the display\n", device->name);
		halted = 0;
		if (error == 0)
			halted = 1;

		/* The boot image again, or dark; what was not seen to stop being read goes to the quarantine. */
		if (display->boot_kept) {
			light_by_host(device, &display->boot, 0);
			if (!halted)
				display->host_unsure = 1;
			flips_forget(device);
			scan_release(device, display->shown.memory);
			display->shown = display->boot;
			display->boot_scanning = 1;
		} else {
			dark_by_host(device, halted);
			flips_forget(device);
			scan_release(device, display->shown.memory);
			image_clear(&display->shown);
		}
	}

	/* Presents a recovery put back meanwhile are not shown. */
	for (index = 0U; index < display->queue_count; index++)
		ghci_scan_drop(device, display->queue[index].memory);
	display->queue_count = 0U;
	display->ending = 0;

	/* No display event comes any more (the host writes them in every state). */
	display->irq_enable = 0U;
	ghci_write(device, display->registers + GHCI_DC_IRQ_ENABLE, 0U);
	ghci_write(device, display->registers + GHCI_DC_IRQ_STATUS, GHCI_DC_IRQ_ALL);
	display->present = 0;

	mutex_unlock(&device->display_lock);

	/* The memory the display held, and its own buffers. */
	ghci_scan_flush(device);
	buffers_give_back(device);
}

/*
 * Ends a closing session's lease.  Only the mark is made here: the
 * service thread turns the controller dark.  The caller holds no lock of
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

	/* The service thread turns the controller dark and lets the memory go. */
	if (ended)
		ghci_service_wake(device, GHCI_WORK_DISPLAY);
}

/*
 * Does the display's part of a round of the service thread: the records,
 * the end of a lease, the scan-out errors and the connection.  The
 * caller holds no lock of the device.
 */
void
ghci_display_service(
	struct ghci_device *device,
	unsigned work)
{
	struct ghci_display *display;
	unsigned long irq;
	uint64_t adopted;
	uint32_t events;
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

	/* The events the interrupt took since the last round. */
	irq = spin_lock_irqsave(&device->irq_lock);

	events = display->irq_events;
	display->irq_events = 0U;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Everything places only into a free ring with room at once; nothing here waits for the ring or a record. */
	display->service_placing = 1;
	adopted = display->adopted;

	/* A lease a closed session ended goes dark now, and is finished once it let go. */
	if (display->ending && display->dark_placed < 0)
		ghci_scan_lease_end(device);
	if (display->ending)
		(void)ghci_scan_lease_done(device);

	/* The scan-out errors, every round, so a masked second ends on time. */
	errors_serve(device);

	/* The connection (an event or the transmitter's line), the idle frame, and the records and what waits. */
	changed = connection_serve(device);
	if ((events & GHCI_DC_IRQ_HOTPLUG) != 0U)
		changed = 1;
	idle_serve(device);
	ghci_scan_update(device);
	if (display->adopted != adopted)
		changed = 1;
	display->service_placing = 0;

	mutex_unlock(&device->display_lock);

	/* The memory let go of when the locks are free, and the waiters on the display's events. */
	scan_flush(device, 1);
	if (changed)
		poll_notify();
}

/*
 * Takes the display's events in the interrupt handler, under irq_lock:
 * DISP_IRQ_STATUS is read and cleared (write 1 to clear) before the
 * handler clears IRQ_STATUS[3], and the service thread is told.
 *
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 9.2, the display events acknowledged in DISP_IRQ_STATUS before
 * IRQ_STATUS[3]).
 */
void
ghci_display_interrupt(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t events;

	/* A device without a display raises no display event. */
	display = &device->display;
	if (!display->present)
		return;

	/* The events, acknowledged, for the service thread's next round. */
	events = ghci_read(device, display->registers + GHCI_DC_IRQ_STATUS);
	if (events != 0U)
		ghci_write(device, display->registers + GHCI_DC_IRQ_STATUS, events);
	display->irq_events |= events;
	device->service_work |= GHCI_WORK_DISPLAY;
}

/*
 * Counts the display's flips again while the reset processor is idle and
 * the host may write the controller; cp_lock and the display's lock are
 * held.  A lighting lost with the ring lights again, whatever CTRL says,
 * and so does one cut after its flip was taken but before ENABLE.
 *
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 13.2, the controller keeps scanning through a GPU reset, STATUS[31]
 * falls once the records are written in IDLE, and TAKEN0 and TAKEN1 say
 * which flips were taken).
 */
void
ghci_display_recover_count(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_flip *flip;
	struct ghci_display_image lighting_image;
	uint32_t status;
	uint32_t ctrl;
	uint32_t taken_value;
	uint32_t pending;
	unsigned plane;
	int lighting;
	int cancelled;
	int error;

	/* A device without a display has nothing to count. */
	display = &device->display;
	if (!display->present)
		return;

	/* Whether plane 0's flip still placed belongs to a lighting, and its image. */
	lighting = 0;
	image_clear(&lighting_image);
	if (display->flips[0].placed && display->flips[0].light) {
		lighting = 1;
		lighting_image = display->flips[0].image;
	}

	/* A record still to be written goes out while the processor is idle. */
	error = ghci_wait_register(device, display->registers + GHCI_DC_STATUS, GHCI_DC_ST_RECORD, 0U, RECORD_WAIT_MS);
	if (error != 0)
		kern_logf("ghci: %s: a flip's record was not written after the reset\n", device->name);

	/* The records that came. */
	records_take(device);

	/* An ended lease's controller goes dark now, by the host (the reset halted the processor): the dark packet may have been lost. */
	if (display->ending) {
		dark_by_host(device, 1);
		flips_forget(device);
	}

	/* Whether the controller is lit is what CTRL says now: a run cut by the reset may have left either. */
	ctrl = ghci_read(device, display->registers + GHCI_DC_CTRL);
	display->enabled = 0;
	if ((ctrl & GHCI_DC_CTRL_ENABLE) != 0U)
		display->enabled = 1;

	/* Each flip still placed is in the slot, was taken without its record, or was lost with the ring. */
	status = ghci_read(device, display->registers + GHCI_DC_STATUS);
	for (plane = 0U; plane < 2U; plane++) {
		flip = &display->flips[plane];
		if (!flip->placed)
			continue;

		/* A flip in the controller's slot is taken later; its record comes once the ring runs. */
		pending = GHCI_DC_ST_PENDING0;
		if (plane == 1U)
			pending = GHCI_DC_ST_PENDING1;
		if ((status & pending) != 0U)
			continue;

		/* A flip the controller took, whose record did not come, is known by TAKEN0 or TAKEN1. */
		if (plane == 0U)
			taken_value = ghci_read(device, display->registers + GHCI_DC_TAKEN0);
		else
			taken_value = ghci_read(device, display->registers + GHCI_DC_TAKEN1);
		if ((taken_value & ~GHCI_DC_TAKEN_CANCELLED) == flip->value) {
			cancelled = 0;
			if ((taken_value & GHCI_DC_TAKEN_CANCELLED) != 0U)
				cancelled = 1;
			taken(device, plane, cancelled);
			continue;
		}

		/* A flip lost with the ring is placed again: a lost lighting lights again. */
		flip->placed = 0;
		if (plane == 0U) {
			if (flip->light)
				display->enabled = 0;
			queue_push_front(display, &flip->image);
		} else {
			display->cursor_pending = flip->surface;
		}
	}

	/*
	 * A lighting cut after its flip was taken but before ENABLE leaves
	 * the image shown on a dark controller, which scans nothing: the
	 * image goes back to the head of the queue, its hold with it, and
	 * lights the controller again.
	 */
	if (lighting &&
	    !display->enabled &&
	    !display->ending &&
	    !display->flips[0].placed &&
	    display->shown.base == lighting_image.base &&
	    display->shown.sequence == lighting_image.sequence &&
	    display->shown.copy_frame == lighting_image.copy_frame) {
		queue_push_front(display, &display->shown);
		image_clear(&display->shown);
	}

	/* A fault hides the cursor (its image is kept); the connection is read again by the service thread. */
	ghci_write(device, display->registers + GHCI_DC_CURSOR_CTRL, 0U);
	ghci_write(device, display->registers + GHCI_DC_STATUS, DC_ERRORS);
}

/*
 * Shows the boot image again for a panic, while the boot image is kept
 * (the console the kernel draws on): the host halts the processor, waits
 * for HALTED by counting reads, and writes the controller's registers.
 * It takes no lock and does not sleep, as the rest of the kernel has
 * stopped.  Returns 0, or ENOENT when there is no boot image to show.
 *
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 13.2, a panic console: CTRL.HALT, HALTED, then the host's writes).
 */
int
ghci_display_panic(
	struct ghci_device *device)
{
	struct ghci_display *display;
	int error;

	/* Only a kept boot image shows what the console drew. */
	display = &device->display;
	if (!display->present || !display->boot_kept)
		return ENOENT;

	/* The processor halts (a faulted one takes the host's writes at once). */
	error = halt_for_host(device, 1);
	if (error != 0)
		return error;

	/* The boot image, lit by the host. */
	light_by_host(device, &display->boot, 1);

	/* Succeeded: the controller scans the console's image. */
	return 0;
}

/*
 * Waits a bounded time, once the stop's reset left the processor idle,
 * for the flip records the controller still owes (STATUS[31]) to be
 * written, so that the fence page they go to may be given back.  Returns
 * 0, also for a device without the controller, or ETIMEDOUT while a
 * record may still come.
 *
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 13.2, STATUS[31] falls once the records held while HALTED or FAULTED
 * are written in IDLE).
 */
int
ghci_display_records_settle(
	struct ghci_device *device)
{
	int error;

	/* A device without the controller's block owes no record. */
	if (device->display.registers == 0U)
		return 0;

	/* The records go out while the processor is idle. */
	error = ghci_wait_register(device, device->display.registers + GHCI_DC_STATUS, GHCI_DC_ST_RECORD, 0U, RECORD_WAIT_MS);
	return error;
}

/*
 * Reports the index of the controller's largest mode, by its pixels.
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
 * modes, of the rows on the memory line times the lines.
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
 * rounded up to the memory line, as the controller scans them.
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

	/* The clock's ticks, in nanoseconds. */
	ticks = clock_ticks();

	/* Succeeded: the time. */
	return kern_ticks_to_ns(ticks);
}

/*
 * Reads the records the controller wrote and places what waits for the
 * planes; the lock is held.
 */
void
ghci_scan_update(
	struct ghci_device *device)
{
	/* The records, then the next present and the cursor's flip. */
	records_take(device);
	queue_kick(device);
	cursor_kick(device);
}

/*
 * Records an image of plane 0 for the controller, after every one
 * recorded before.
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

	/* The present waits behind the others, and goes to the ring when the plane's slot is free. */
	display->queue[display->queue_count] = *image;
	display->queue_count++;
	queue_kick(device);

	/* Succeeded: the present is recorded. */
	return 0;
}

/*
 * Shows a cursor surface: flips it now when the controller is lit and
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
 * Marks the lease ended and has the controller go dark: the presents
 * waiting are dropped, the cursor hidden and forgotten, and the dark
 * packet placed (by the host when the ring does not run).
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

	/* The cursor is hidden at once (the host writes it in every state) and its image forgotten. */
	ghci_write(device, display->registers + GHCI_DC_CURSOR_CTRL, 0U);
	display->cursor_image = 0;
	display->cursor_pending = -1;

	/* The controller goes dark, which cancels every pending flip. */
	dark(device);
}

/*
 * Reports whether an ended lease has let go of everything: the dark
 * packet ran and every placed flip's record came.
 */
int
ghci_scan_lease_done(
	struct ghci_device *device)
{
	struct ghci_display *display;
	enum ghci_ticket_state state;
	int usable;
	int error;

	/* A lease that did not end has nothing to finish. */
	display = &device->display;
	if (!display->ending)
		return 1;

	/* The dark packet has not been placed yet: the service thread places it. */
	if (display->dark_placed < 0)
		return 0;

	/* A ring that no longer runs answers nothing: the host turns the controller dark. */
	usable = ghci_scan_ring_usable(device);
	if (!usable) {
		error = halt_for_host(device, 0);
		dark_by_host(device, error == 0);
		flips_forget(device);
		display->dark_placed = 0;
	}

	/* The cancelled records of the flips placed. */
	records_take(device);
	if (display->flips[0].placed || display->flips[1].placed)
		return 0;

	/* The dark packet, whose fence passes once the controller is idle (a failed one leaves it to the reset's count). */
	if (display->dark_placed > 0) {
		state = ghci_cp_ticket(device, &display->dark_ticket);
		if (state == GHCI_TICKET_PENDING)
			return 0;
		if (state == GHCI_TICKET_DONE)
			display->host_unsure = 0;
	}

	/* The controller no longer scans the image it showed: its memory goes (to the quarantine when that was not seen). */
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
 * Sleeps until a flip's interrupt, a short poll or the deadline.
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
 * Takes memory the controller scans, the device's own: local memory on a
 * discrete GPU, host memory below the GPU's limit otherwise, held by the
 * display.  Its address must fit the controller's 32 bits.  Returns 0, or
 * ENOMEM.
 */
int
ghci_scan_alloc(
	struct ghci_device *device,
	uint64_t bytes,
	struct ghci_allocation **result)
{
	struct ghci_allocation *memory;
	int error;

	/* Local memory on a discrete GPU, host memory otherwise. */
	if (device->local_ready)
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

	/* An address past 32 bits cannot be scanned. */
	if (memory->bus + memory->bytes > SCAN_LIMIT) {
		scan_give_back(device, memory);
		return ENOMEM;
	}

	/* Succeeded: the memory is the display's. */
	*result = memory;
	return 0;
}

/*
 * Writes bytes into memory the controller scans: through the processor's
 * view of host memory, or through the local window.  Returns 0, EINVAL for
 * a range outside the memory, or ENOTSUP when the window does not show it.
 */
int
ghci_scan_write(
	struct ghci_device *device,
	struct ghci_allocation *memory,
	uint64_t offset,
	const void *bytes,
	uint64_t length)
{
	int error;

	/* A range inside the memory. */
	if (offset > memory->bytes || length > memory->bytes - offset)
		return EINVAL;

	/* Local memory, through the window. */
	if (memory->kind == GHCI_MEMORY_LOCAL) {
		error = ghci_memory_local_write(device, memory, offset, bytes, length);
		return error;
	}

	/* Succeeded: host memory, through the view. */
	kern_memcpy(memory->view + offset, bytes, (size_t)length);
	return 0;
}

/*
 * Reads the controller's mode table into the display.  Returns 0, or
 * ENOTSUP when the controller names no mode the driver can use.
 */
static int
modes_read(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_mode *mode;
	uint32_t registers;
	uint32_t count;
	uint32_t word;
	uint32_t flags;
	uint64_t clock_khz;
	uint64_t total;
	uint32_t index;

	display = &device->display;
	registers = display->registers;

	/* The number of modes the controller names, at most what the display keeps. */
	count = (display->controller_caps >> GHCI_DC_CAPS_MODES_SHIFT) & GHCI_DC_CAPS_MODES_MASK;
	if (count > GHCI_DISPLAY_MODES_MAX)
		count = GHCI_DISPLAY_MODES_MAX;

	/* Reads each mode through MODE_INDEX; a mode without a size or a clock is skipped. */
	display->mode_count = 0U;
	for (index = 0U; index < count; index++) {
		mode = &display->modes[display->mode_count];
		ghci_write(device, registers + GHCI_DC_MODE_INDEX, index);

		/* The horizontal timing: active pixels and sync, then the porches. */
		word = ghci_read(device, registers + GHCI_DC_MODE_H0);
		mode->width = MODE_LOW(word);
		mode->h_sync = MODE_HIGH(word);
		word = ghci_read(device, registers + GHCI_DC_MODE_H1);
		mode->h_front = MODE_LOW(word);
		mode->h_back = MODE_HIGH(word);

		/* The vertical timing, in lines. */
		word = ghci_read(device, registers + GHCI_DC_MODE_V0);
		mode->height = MODE_LOW(word);
		mode->v_sync = MODE_HIGH(word);
		word = ghci_read(device, registers + GHCI_DC_MODE_V1);
		mode->v_front = MODE_LOW(word);
		mode->v_back = MODE_HIGH(word);

		/* The sync polarities (SYNC_POL's two bits) and the clock's PCLK_SEL value. */
		flags = ghci_read(device, registers + GHCI_DC_MODE_FLAGS);
		mode->sync_polarity = flags & GHCI_DC_MODE_SYNC_MASK;
		mode->clock_select = (flags >> GHCI_DC_MODE_PCLK_SHIFT) & GHCI_DC_MODE_PCLK_MASK;

		/* A mode without a size or a clock cannot be shown. */
		clock_khz = ghci_read(device, registers + GHCI_DC_MODE_CLOCK);
		if (mode->width == 0U || mode->height == 0U || clock_khz == 0U)
			continue;

		/* The pixel clocks of one whole frame, the blanking included. */
		total = (uint64_t)(mode->width + mode->h_front + mode->h_sync + mode->h_back) *
		    (mode->height + mode->v_front + mode->v_sync + mode->v_back);

		/* The refresh the clock gives over the frame's totals, in millihertz, rounded. */
		mode->refresh_millihz = (uint32_t)((clock_khz * 1000000U + total / 2U) / total);
		display->mode_count++;
	}

	/* A controller without a usable mode has no display. */
	if (display->mode_count == 0U)
		return ENOTSUP;

	/* Succeeded: the modes are known. */
	return 0;
}

/*
 * Takes over a boot display the firmware left running: the image it
 * scans, its rows and format, and its timing, which is one of the modes
 * of the table when every value matches (a first flip in that mode then
 * needs no new lighting) or the boot slot otherwise.  Nothing is written.
 *
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 13.3, the boot framebuffer read from FB_BASE, FB_STRIDE, FB_SIZE and
 * CTRL.FORMAT and kept scanning).
 */
static void
boot_take(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_mode *boot;
	const struct ghci_display_mode *mode;
	uint32_t registers;
	uint32_t ctrl;
	uint32_t size;
	unsigned index;

	/* The image, its rows, its size and its format. */
	display = &device->display;
	registers = display->registers;
	ctrl = ghci_read(device, registers + GHCI_DC_CTRL);
	size = ghci_read(device, registers + GHCI_DC_FB_SIZE);
	boot = &display->modes[GHCI_DISPLAY_BOOT_MODE];
	kern_memset(boot, 0, sizeof(*boot));
	boot->width = size & 0xffffU;
	boot->height = size >> GHCI_DC_SIZE_HEIGHT_SHIFT;

	/* The timing the firmware set. */
	boot->h_front = ghci_read(device, registers + GHCI_DC_H_FP);
	boot->h_sync = ghci_read(device, registers + GHCI_DC_H_SYNC);
	boot->h_back = ghci_read(device, registers + GHCI_DC_H_BP);
	boot->v_front = ghci_read(device, registers + GHCI_DC_V_FP);
	boot->v_sync = ghci_read(device, registers + GHCI_DC_V_SYNC);
	boot->v_back = ghci_read(device, registers + GHCI_DC_V_BP);
	boot->sync_polarity = ghci_read(device, registers + GHCI_DC_SYNC_POL) & GHCI_DC_MODE_SYNC_MASK;
	boot->clock_select = ghci_read(device, registers + GHCI_DC_PCLK_SEL);

	/* The boot image, which nothing of the driver's holds. */
	image_clear(&display->boot);
	display->boot.base = ghci_read(device, registers + GHCI_DC_FB_BASE);
	display->boot.stride = ghci_read(device, registers + GHCI_DC_FB_STRIDE);
	display->boot.format = (ctrl >> GHCI_DC_CTRL_FORMAT_SHIFT) & GHCI_DC_CTRL_FORMAT_MASK;
	display->boot.mode = GHCI_DISPLAY_BOOT_MODE;

	/* A mode of the table with the same timing stands for the boot slot. */
	for (index = 0U; index < display->mode_count; index++) {
		mode = &display->modes[index];
		if (mode->width == boot->width &&
		    mode->height == boot->height &&
		    mode->h_front == boot->h_front &&
		    mode->h_sync == boot->h_sync &&
		    mode->h_back == boot->h_back &&
		    mode->v_front == boot->v_front &&
		    mode->v_sync == boot->v_sync &&
		    mode->v_back == boot->v_back &&
		    mode->sync_polarity == boot->sync_polarity &&
		    mode->clock_select == boot->clock_select) {
			boot->refresh_millihz = mode->refresh_millihz;
			display->boot.mode = index;
			break;
		}
	}

	/*
	 * The controller is lit on the boot image until the driver's first
	 * flip is taken; until the kernel can say that nothing draws to the
	 * image any more, it is kept for the console.
	 */
	display->enabled = 1;
	display->mode = display->boot.mode;
	display->format = display->boot.format;
	display->stride = display->boot.stride;
	display->shown = display->boot;
	display->boot_scanning = 1;
	display->boot_kept = 1;
	display->boot_console = 1;
}

/* Empties an image record: no present, no memory, no frame. */
static void
image_clear(
	struct ghci_display_image *image)
{
	kern_memset(image, 0, sizeof(*image));
	image->copy_frame = -1;
}

/* Reports CTRL's word for a format with ENABLE off: the format and the output (HDMI when there is one). */
static uint32_t
control_word(
	struct ghci_device *device,
	uint32_t format)
{
	uint32_t control;

	/* The format, and HDMI when the controller drives a TMDS link or a transmitter. */
	control = (format & GHCI_DC_CTRL_FORMAT_MASK) << GHCI_DC_CTRL_FORMAT_SHIFT;
	if ((device->display.controller_caps & (GHCI_DC_CAPS_TMDS | GHCI_DC_CAPS_TRANSMITTER)) != 0U)
		control |= GHCI_DC_CTRL_OUTPUT_HDMI;

	/* Succeeded: the word. */
	return control;
}

/* Reports the 32-bit physical address of a plane's record in the fence page. */
static uint32_t
record_address(
	struct ghci_device *device,
	unsigned plane)
{
	return (uint32_t)(device->fence_page->bus + RECORD_OFFSET + GHCI_DC_RECORD_BYTES * plane);
}

/* Reports the processor's view of the value word of a plane's record. */
static volatile uint32_t *
record_slot(
	struct ghci_device *device,
	unsigned plane)
{
	return (volatile uint32_t *)(void *)(device->fence_page->view + RECORD_OFFSET + GHCI_DC_RECORD_BYTES * plane);
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
	if (display->next_value > FLIP_VALUE_MAX)
		display->next_value = 1U;

	/* Succeeded: the value. */
	return value;
}

/*
 * Places a run of the display's packets and a fence after them through
 * the gate, without waiting for a recovery; the display's lock is held
 * throughout, so the state read before stays.  The service thread's
 * placement waits for nothing: it enters only a free ring with room for
 * the run and its fence now.
 *
 * Returns 0 with the fence's ticket; EAGAIN when a recovery runs (or,
 * for the service, the ring is in use or full), with nothing placed, or
 * with *placed set when the part placed before it began is lost to the
 * reset (the recovery counts its flips again); or EIO when the device
 * is lost.
 */
static int
ring_put(
	struct ghci_device *device,
	const uint32_t *packets,
	uint32_t dwords,
	struct ghci_ticket *ticket,
	int *placed)
{
	enum ghci_gate gate;
	uint32_t start;
	int error;

	/* The gate, which keeps the display's lock held when it does not open. */
	*placed = 0;
	if (device->display.service_placing)
		gate = ghci_cp_try_enter(device, dwords + GHCI_DW_FENCE);
	else
		gate = ghci_cp_enter(device, GHCI_HOLD_DISPLAY, 0);
	if (gate == GHCI_GATE_LOST)
		return EIO;
	if (gate != GHCI_GATE_OPEN)
		return EAGAIN;

	/* The packets, the fence after them, and the doorbell for what was placed. */
	start = device->ring.wptr;
	error = ghci_cp_place(device, packets, dwords);
	if (error == 0)
		error = ghci_cp_place_fence(device, NULL, start, ticket);
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

/* Reads the record page for each placed flip and takes the flips whose records came. */
static void
records_take(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_display_flip *flip;
	uint32_t value;
	unsigned plane;
	int cancelled;

	/* Records exist only once the ring and its fence page do. */
	display = &device->display;
	if (device->fence_page == NULL)
		return;

	/* Each plane's record names its flip by the value, with bit 31 saying it was cancelled. */
	for (plane = 0U; plane < 2U; plane++) {
		flip = &display->flips[plane];
		if (!flip->placed)
			continue;

		value = *record_slot(device, plane);
		if ((value & ~GHCI_DC_TAKEN_CANCELLED) != flip->value)
			continue;

		cancelled = 0;
		if ((value & GHCI_DC_TAKEN_CANCELLED) != 0U)
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
 * becomes the controller's; a cancelled one is flipped again once the
 * controller is lit.
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
			/* The controller has read the surface and shows it from now. */
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

	/* Nothing waits, the plane's slot is in use, the lease is ending, or the ring does not run. */
	display = &device->display;
	if (display->queue_count == 0U || display->flips[0].placed || display->ending)
		return;
	usable = ghci_scan_ring_usable(device);
	if (!usable)
		return;

	/* An image of another mode, format or stride than the one lit lights the controller again. */
	head = &display->queue[0];
	relight = 0;
	if (!display->enabled)
		relight = 1;
	if (head->mode != display->mode ||
	    head->format != display->format ||
	    head->stride != display->stride)
		relight = 1;

	/* The run that lights the controller on the image, or one flip; a refused one waits for the next look. */
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

	/* The queue has room for one more than its depth: the flip the slot had. */
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
 * Fills the register pairs of a mode's timing, the sync polarity, the
 * pixel clock and an image's rows and size.  Returns the pairs.
 */
static uint32_t
timing_pairs(
	const struct ghci_display_mode *timing,
	const struct ghci_display_image *image,
	uint32_t *offsets,
	uint32_t *values)
{
	/* The horizontal and vertical timing. */
	offsets[0] = GHCI_DC_H_ACTIVE;
	values[0] = timing->width;
	offsets[1] = GHCI_DC_H_FP;
	values[1] = timing->h_front;
	offsets[2] = GHCI_DC_H_SYNC;
	values[2] = timing->h_sync;
	offsets[3] = GHCI_DC_H_BP;
	values[3] = timing->h_back;
	offsets[4] = GHCI_DC_V_ACTIVE;
	values[4] = timing->height;
	offsets[5] = GHCI_DC_V_FP;
	values[5] = timing->v_front;
	offsets[6] = GHCI_DC_V_SYNC;
	values[6] = timing->v_sync;
	offsets[7] = GHCI_DC_V_BP;
	values[7] = timing->v_back;

	/* The sync polarity, the pixel clock, and the image's rows and size. */
	offsets[8] = GHCI_DC_SYNC_POL;
	values[8] = timing->sync_polarity;
	offsets[9] = GHCI_DC_PCLK_SEL;
	values[9] = timing->clock_select;
	offsets[10] = GHCI_DC_FB_STRIDE;
	values[10] = image->stride;
	offsets[11] = GHCI_DC_FB_SIZE;
	values[11] = timing->width | (timing->height << GHCI_DC_SIZE_HEIGHT_SHIFT);

	/* Succeeded: the pairs. */
	return LIGHT_PAIRS;
}

/*
 * Places the run that lights the controller on an image: CTRL off (the
 * processor waits until the controller is idle), the mode's timing, the
 * stride and size, the flip (taken at once while ENABLE is 0), CTRL with
 * ENABLE, and the cursor's flip when one waits; then the driver's fence.
 * Returns 0 once the run (or a part a recovery cut) is in the ring, or
 * the error of a run not placed.
 *
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 13.2, starting the controller; section 7.3, CMD_DISP_SET and CMD_FLIP
 * with its 16-byte record at a 32-bit physical address).
 */
static int
light(
	struct ghci_device *device,
	const struct ghci_display_image *image)
{
	struct ghci_display *display;
	struct ghci_ticket ticket;
	uint32_t packets[LIGHT_DWORDS];
	uint32_t offsets[LIGHT_PAIRS];
	uint32_t values[LIGHT_PAIRS];
	uint32_t control;
	uint32_t value;
	uint32_t cursor_value;
	uint32_t dwords;
	uint32_t pairs;
	int cursor;
	int placed;
	int error;

	display = &device->display;
	control = control_word(device, image->format);

	/* CTRL with ENABLE off, waiting for the controller's idle. */
	offsets[0] = GHCI_DC_CTRL;
	values[0] = control;
	dwords = ghci_cmd_disp_set(&packets[0], 1U, 1U, offsets, values);

	/* The timing, the sync polarity, the pixel clock and the image's rows and size. */
	pairs = timing_pairs(&display->modes[image->mode], image, offsets, values);
	dwords += ghci_cmd_disp_set(&packets[dwords], 0U, pairs, offsets, values);

	/* The image's flip, taken at once while ENABLE is off; its record clears first. */
	value = value_take(display);
	*record_slot(device, 0U) = 0U;
	dwords += ghci_cmd_flip(&packets[dwords],
	    0U,
	    image->base,
	    GHCI_FLIP_VSYNC_WAIT | GHCI_FLIP_RECORD_ENABLE,
	    value,
	    record_address(device, 0U));

	/* CTRL with ENABLE: the first frame starts at the next vsync. */
	offsets[0] = GHCI_DC_CTRL;
	values[0] = control | GHCI_DC_CTRL_ENABLE;
	dwords += ghci_cmd_disp_set(&packets[dwords], 0U, 1U, offsets, values);

	/* The cursor's surface that waited for the lighting, when plane 1 has no flip placed. */
	cursor = 0;
	cursor_value = 0U;
	if (display->cursor_pending >= 0 && !display->flips[1].placed) {
		cursor = 1;
		cursor_value = value_take(display);
		*record_slot(device, 1U) = 0U;
		dwords += ghci_cmd_flip(&packets[dwords],
		    0U,
		    (uint32_t)display->surfaces[display->cursor_pending]->bus,
		    GHCI_FLIP_PLANE_CURSOR | GHCI_FLIP_RECORD_ENABLE,
		    cursor_value,
		    record_address(device, 1U));
	}

	/* The run, with the driver's fence after it; nothing is recorded unless some of it was placed. */
	error = ring_put(device, packets, dwords, &ticket, &placed);
	if (error != 0 && !placed)
		return error;

	/* The controller is lit on the image's mode, format and stride once the run is parsed. */
	display->enabled = 1;
	display->mode = image->mode;
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

/* Places one flip of plane 0 to an image of the mode lit, with the driver's fence. */
static int
flip_primary(
	struct ghci_device *device,
	const struct ghci_display_image *image)
{
	struct ghci_display *display;
	struct ghci_ticket ticket;
	uint32_t packet[GHCI_DW_FLIP];
	uint32_t value;
	int placed;
	int error;

	/* The flip at the next vsync, whose record clears first. */
	display = &device->display;
	value = value_take(display);
	*record_slot(device, 0U) = 0U;
	(void)ghci_cmd_flip(packet,
	    0U,
	    image->base,
	    GHCI_FLIP_VSYNC_WAIT | GHCI_FLIP_RECORD_ENABLE,
	    value,
	    record_address(device, 0U));
	error = ring_put(device, packet, GHCI_DW_FLIP, &ticket, &placed);
	if (error != 0 && !placed)
		return error;

	/* The plane's slot is the image's until its record comes. */
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
	struct ghci_ticket ticket;
	uint32_t packet[GHCI_DW_FLIP];
	uint32_t value;
	int placed;
	int error;

	/* The flip, whose record clears first. */
	display = &device->display;
	value = value_take(display);
	*record_slot(device, 1U) = 0U;
	(void)ghci_cmd_flip(packet,
	    0U,
	    (uint32_t)display->surfaces[surface]->bus,
	    GHCI_FLIP_PLANE_CURSOR | GHCI_FLIP_RECORD_ENABLE,
	    value,
	    record_address(device, 1U));
	error = ring_put(device, packet, GHCI_DW_FLIP, &ticket, &placed);
	if (error != 0 && !placed)
		return error;

	/* The cursor's slot is the surface's until its record comes. */
	display->flips[1].placed = 1;
	display->flips[1].value = value;
	display->flips[1].light = 0;
	display->flips[1].surface = surface;

	/* Succeeded: the flip is in the ring. */
	return 0;
}

/* Places the cursor surface that waits, when the controller is lit and plane 1 has no flip placed. */
static void
cursor_kick(
	struct ghci_device *device)
{
	struct ghci_display *display;
	int usable;
	int error;

	/* Nothing waits, the controller is dark, the slot is in use, the lease is ending, or the ring does not run. */
	display = &device->display;
	if (display->cursor_pending < 0 || !display->enabled || display->flips[1].placed || display->ending)
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
 * Turns the controller dark through the ring: CTRL off, the processor
 * waiting for the controller's idle, then the driver's fence.  A placement
 * a recovery refused is placed again by the service thread; on a lost
 * device the host turns the controller dark.
 */
static void
dark(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t packets[DARK_DWORDS];
	uint32_t offsets[1];
	uint32_t values[1];
	uint32_t dwords;
	int placed;
	int usable;
	int error;

	/* A ring that does not run leaves the controller to the host. */
	display = &device->display;
	usable = ghci_scan_ring_usable(device);
	if (!usable) {
		error = halt_for_host(device, 0);
		dark_by_host(device, error == 0);
		flips_forget(device);
		display->dark_placed = 0;
		return;
	}

	/* CTRL with ENABLE off and the wait for the idle; every pending flip is taken as cancelled. */
	offsets[0] = GHCI_DC_CTRL;
	values[0] = control_word(device, display->format);
	dwords = ghci_cmd_disp_set(packets, 1U, 1U, offsets, values);
	error = ring_put(device, packets, dwords, &display->dark_ticket, &placed);
	if (error == EAGAIN) {
		/* A recovery runs: the service thread places it again once the recovery ends. */
		display->dark_placed = -1;
		return;
	}

	if (error != 0) {
		/* A lost device: the host turns the controller dark. */
		error = halt_for_host(device, 0);
		dark_by_host(device, error == 0);
		flips_forget(device);
		display->dark_placed = 0;
		return;
	}

	/* The controller is dark once the packet runs; its fence says when. */
	display->enabled = 0;
	display->dark_placed = 1;
}

/*
 * Waits for the controller to be idle (ENABLE 0 and no read outstanding):
 * sleeping a bounded time, or counting reads (for a panic).  Returns 0 or
 * ETIMEDOUT.
 */
static int
wait_idle(
	struct ghci_device *device,
	int spin)
{
	uint32_t status;
	unsigned reads;
	int error;

	/* Sleeping between reads. */
	if (!spin) {
		error = ghci_wait_register(device, device->display.registers + GHCI_DC_STATUS, GHCI_DC_ST_IDLE, GHCI_DC_ST_IDLE, DC_IDLE_MS);
		return error;
	}

	/* Counting reads. */
	for (reads = 0U; reads < PANIC_READS; reads++) {
		status = ghci_read(device, device->display.registers + GHCI_DC_STATUS);
		if ((status & GHCI_DC_ST_IDLE) != 0U)
			return 0;
	}

	/* The controller never went idle. */
	return ETIMEDOUT;
}

/*
 * Turns the controller dark by the host, which may write it only while
 * the command processor does not run, and waits until it reads no memory.
 * halted says the processor was seen to halt (or was reset); without it,
 * or when the controller does not go idle, what it scanned may still be
 * read, and is let go of through the quarantine (host_unsure).
 */
static void
dark_by_host(
	struct ghci_device *device,
	int halted)
{
	struct ghci_display *display;
	int error;

	/* ENABLE off; the controller then finishes its reads. */
	display = &device->display;
	ghci_write(device, display->registers + GHCI_DC_CTRL, control_word(device, display->format));
	error = wait_idle(device, 0);
	if (error != 0)
		kern_logf("ghci: %s: display controller not idle\n", device->name);
	display->enabled = 0;

	/* Whether the host saw everything stop. */
	display->host_unsure = 0;
	if (!halted || error != 0)
		display->host_unsure = 1;
}

/*
 * Lights the controller on an image by the host, as the ring's run does:
 * CTRL off and the wait for the idle, the timing, the rows and size,
 * FB_NEXT (taken at once while ENABLE is 0) and CTRL with ENABLE.  The
 * command processor does not run.  spin counts reads instead of sleeping.
 */
static void
light_by_host(
	struct ghci_device *device,
	const struct ghci_display_image *image,
	int spin)
{
	struct ghci_display *display;
	uint32_t offsets[LIGHT_PAIRS];
	uint32_t values[LIGHT_PAIRS];
	uint32_t registers;
	uint32_t control;
	uint32_t pairs;
	uint32_t index;
	int error;

	/* CTRL off, and the controller idle; one that is not may still read what it scanned. */
	display = &device->display;
	registers = display->registers;
	control = control_word(device, image->format);
	ghci_write(device, registers + GHCI_DC_CTRL, control);
	error = wait_idle(device, spin);
	if (error != 0) {
		display->host_unsure = 1;
		return;
	}

	/* The timing, the rows and the size. */
	pairs = timing_pairs(&display->modes[image->mode], image, offsets, values);
	for (index = 0U; index < pairs; index++)
		ghci_write(device, registers + offsets[index], values[index]);

	/* The image at once, then the scan-out. */
	ghci_write(device, registers + GHCI_DC_FB_NEXT, image->base);
	ghci_write(device, registers + GHCI_DC_CTRL, control | GHCI_DC_CTRL_ENABLE);
	display->host_unsure = 0;
	display->enabled = 1;
	display->mode = image->mode;
	display->format = image->format;
	display->stride = image->stride;
}

/*
 * Halts a running command processor so that the host may write the
 * controller's scan-out registers; an idle, halted or faulted one takes
 * them as it is.  spin counts reads instead of sleeping.  Returns 0, or
 * ETIMEDOUT when the processor did not halt.
 */
static int
halt_for_host(
	struct ghci_device *device,
	int spin)
{
	uint32_t status;
	uint32_t ctrl;
	unsigned reads;
	int error;

	/* Only a running processor needs the halt. */
	status = ghci_read(device, GHCI_REG_STATUS);
	if ((status & GHCI_STATUS_RUNNING) == 0U || (status & (GHCI_STATUS_HALTED | GHCI_STATUS_FAULTED)) != 0U)
		return 0;

	/* HALT, then HALTED. */
	ctrl = ghci_read(device, GHCI_REG_CTRL);
	ghci_write(device, GHCI_REG_CTRL, ctrl | GHCI_CTRL_HALT);
	if (!spin) {
		error = ghci_wait_register(device, GHCI_REG_STATUS, GHCI_STATUS_HALTED, GHCI_STATUS_HALTED, GHCI_HALT_WAIT_MS);
		return error;
	}

	/* Counting reads, for a panic. */
	for (reads = 0U; reads < PANIC_READS; reads++) {
		status = ghci_read(device, GHCI_REG_STATUS);
		if ((status & GHCI_STATUS_HALTED) != 0U)
			return 0;
	}

	/* The processor did not halt. */
	return ETIMEDOUT;
}

/* Forgets the flips placed whose records will not come: their images are let go of, not shown. */
static void
flips_forget(
	struct ghci_device *device)
{
	struct ghci_display *display;

	/* Plane 0's image goes (to the quarantine when the host did not see the controller stop); the cursor's surface is free again. */
	display = &device->display;
	if (display->flips[0].placed)
		scan_release(device, display->flips[0].image.memory);
	display->flips[0].placed = 0;
	display->flips[1].placed = 0;
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
	 * No cursor; the host writes it in every state.  A dark packet an
	 * ended lease placed (or one a recovery refused) stays: its ticket
	 * comes before the image it turned off is let go of.
	 */
	ghci_write(device, display->registers + GHCI_DC_CURSOR_CTRL, 0U);
	display->cursor_image = 0;
	display->cursor_pending = -1;
}

/*
 * Does one step of the detach's turn of the display, the lock held: once
 * the records of every flip placed before came and the dark packet of an
 * ended lease ran, the controller is turned (turned set) to the boot
 * image again while it is kept, or dark; a boot display still scanned,
 * or a dark controller without a boot image, needs nothing.  The image a
 * dark controller showed is let go of.  Returns 1 once the turn is done,
 * 0 while it waits.
 */
static int
detach_step(
	struct ghci_device *device,
	int *turned)
{
	struct ghci_display *display;
	enum ghci_ticket_state state;
	int error;

	/* The records of the flips placed. */
	display = &device->display;
	records_take(device);
	if (display->flips[0].placed || display->flips[1].placed)
		return 0;

	/* A dark packet a recovery refused is placed again. */
	if (display->dark_placed < 0) {
		dark(device);
		return 0;
	}

	/* The dark packet, whose fence passes once the controller is idle; a failed one is the host's. */
	if (display->dark_placed > 0) {
		state = ghci_cp_ticket(device, &display->dark_ticket);
		if (state != GHCI_TICKET_DONE)
			return 0;
		display->host_unsure = 0;
		display->dark_placed = 0;
	}

	/* A dark controller scans nothing: the image it showed goes. */
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
	if (display->boot_kept) {
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

	/* Succeeded: the controller is dark. */
	return 1;
}

/*
 * Gives back the display's own memory at the detach: the frames' mappings
 * into session 0 first (each holds its frame until its unmap's fence),
 * then the frames, the cursor's surfaces and the idle frame.  The caller
 * holds no lock of the device.
 */
static void
buffers_give_back(
	struct ghci_device *device)
{
	struct ghci_display *display;
	unsigned index;

	/* The mappings the blitter copied through. */
	display = &device->display;
	for (index = 0U; index < 2U; index++) {
		if (display->frame_maps[index] != NULL)
			ghci_vm_unmap(device, display->frame_maps[index]);
		display->frame_maps[index] = NULL;
	}

	/* The frames, the surfaces and the idle frame. */
	for (index = 0U; index < 2U; index++) {
		scan_give_back(device, display->frames[index]);
		display->frames[index] = NULL;
		scan_give_back(device, display->surfaces[index]);
		display->surfaces[index] = NULL;
	}

	scan_give_back(device, display->idle_frame);
	display->idle_frame = NULL;
}

/*
 * Counts the scan-out errors the controller reports and clears them: too
 * many in a second mask the error interrupt for the rest of it, a read
 * error in round after round turns the controller dark (the image it
 * scans is bad), and a refused register write is the driver's own error.
 */
static void
errors_serve(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint32_t status;
	uint32_t errors;
	uint64_t period;
	uint64_t now;

	/* A new second unmasks the interrupt and starts the count again. */
	display = &device->display;
	now = clock_ticks();
	period = kern_ms_to_ticks(ERROR_PERIOD_MS);
	if (now - display->error_tick >= period) {
		display->error_tick = now;
		display->error_count = 0U;
		if (display->error_masked)
			irq_mask_set(device, 0);
	}

	/* The sticky errors, cleared once read (the host clears them in every state). */
	status = ghci_read(device, display->registers + GHCI_DC_STATUS);
	errors = status & DC_ERRORS;
	if (errors == 0U) {
		display->read_errors = 0U;
		return;
	}

	ghci_write(device, display->registers + GHCI_DC_STATUS, errors);

	/* Too many errors this second: the interrupt stays off until the next. */
	display->error_count++;
	display->errors_total++;
	if (display->error_count > ERROR_LIMIT && !display->error_masked)
		irq_mask_set(device, 1);

	/* An underflow means the memory did not keep up; it is counted only. */
	if ((errors & GHCI_DC_ST_UNDERFLOW) != 0U)
		display->underflows++;

	/* A register write the controller refused is a fault of the driver. */
	if ((errors & GHCI_DC_ST_BAD_WRITE) != 0U)
		kern_logf("ghci: %s: display controller refused a register write\n", device->name);

	/* A record the GPU could not write. */
	if ((errors & GHCI_DC_ST_RECORD_ERROR) != 0U)
		kern_logf("ghci: %s: a flip's record was not written\n", device->name);

	/* A read error round after round: the image is bad, and the controller goes dark until the next present. */
	if ((errors & GHCI_DC_ST_READ_ERROR) == 0U) {
		display->read_errors = 0U;
		return;
	}

	display->read_errors++;
	if (display->read_errors < READ_ERROR_RUNS)
		return;
	if (!display->enabled || display->flips[0].placed || display->ending)
		return;
	kern_logf("ghci: %s: display reads fail; turning it off\n", device->name);
	display->read_errors = 0U;
	dark(device);
	display->dark_placed = 0;
}

/* Masks or unmasks the scan-out error event in DISP_IRQ_ENABLE, which the host writes in every state. */
static void
irq_mask_set(
	struct ghci_device *device,
	int masked)
{
	struct ghci_display *display;

	/* The events taken, with or without the errors. */
	display = &device->display;
	if (masked)
		display->irq_enable &= ~GHCI_DC_IRQ_ERROR;
	else
		display->irq_enable |= GHCI_DC_IRQ_ERROR;
	ghci_write(device, display->registers + GHCI_DC_IRQ_ENABLE, display->irq_enable);

	/* The display remembers which way it is. */
	display->error_masked = masked;
}

/*
 * Watches the connection: the transmitter's hot-plug line is read once a
 * second and told to the controller (CONNECTED), whose change raises
 * HOTPLUG; a change of CONNECTED changes the output's connection, and a
 * new connection is a new generation with the transmitter set up again.
 * Returns 1 when the output changed.
 */
static int
connection_serve(
	struct ghci_device *device)
{
	struct ghci_display *display;
	uint64_t period;
	uint64_t now;
	uint32_t connected_word;
	int sensed;
	int connected;
	int error;

	/* The transmitter's line, once a second, when the controller does not sense the sink itself. */
	display = &device->display;
	now = clock_ticks();
	period = kern_ms_to_ticks(HOTPLUG_PERIOD_MS);
	if ((display->controller_caps & GHCI_DC_CAPS_TRANSMITTER) != 0U &&
	    (display->controller_caps & GHCI_DC_CAPS_SENSE) == 0U &&
	    now - display->hotplug_tick >= period) {
		display->hotplug_tick = now;
		error = ghci_adv7511_sensed(device, display->registers, &sensed);
		connected_word = ghci_read(device, display->registers + GHCI_DC_CONNECTED) & 1U;
		if (error == 0 && connected_word != (uint32_t)sensed)
			ghci_write(device, display->registers + GHCI_DC_CONNECTED, (uint32_t)sensed);
	}

	/* What the controller says now. */
	connected_word = ghci_read(device, display->registers + GHCI_DC_CONNECTED);
	connected = 0;
	if ((connected_word & 1U) != 0U)
		connected = 1;
	if (connected == display->connected)
		return 0;

	/* A new connection is a new generation of the output, whose transmitter is set up again. */
	display->connected = connected;
	if (connected) {
		display->generation++;
		if ((display->controller_caps & GHCI_DC_CAPS_TRANSMITTER) != 0U) {
			error = ghci_adv7511_start(device, display->registers);
			if (error != 0)
				kern_logf("ghci: %s: HDMI transmitter not set up again (%d)\n", device->name, error);
		}
	}

	/* The output's topology changed. */
	kern_logf("ghci: %s: display %s\n", device->name, connected ? "connected" : "disconnected");
	topology_move(device);

	/* Succeeded: the output changed. */
	return 1;
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

/* Reports the mode the idle frame is scanned out in: 1280 x 720 when the controller has it, else its largest. */
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
 * Lights the controller on the driver's idle frame once a sink is
 * connected and no lease holds the display, once for each generation of
 * the output (a connection): a lease that ends leaves it dark, and a boot
 * display keeps its image.  The lock is held.
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

	/* A connected sink, no lease, nothing lit, an idle frame, not yet for this connection. */
	display = &device->display;
	if (!display->connected || display->owner != NULL || display->ending || display->enabled)
		return;
	if (display->idle_frame == NULL || display->idle_generation == display->generation)
		return;

	/* The ring must take the controller's packets. */
	usable = ghci_scan_ring_usable(device);
	if (!usable)
		return;

	/* The idle frame in its mode, rows on the memory line, the controller's own format; the driver's frame is not held. */
	mode = idle_mode(display);
	image_clear(&image);
	image.base = (uint32_t)display->idle_frame->bus;
	image.stride = ghci_scan_stride(display->modes[mode].width);
	image.format = GHCI_DC_FMT_BGRA8;
	image.mode = mode;

	/* Lit once for this connection; a lighting a recovery refused is tried at the next round. */
	error = light(device, &image);
	if (error != 0)
		return;
	display->idle_generation = display->generation;
}

/*
 * Takes the idle frame (one any mode fits in), black: host memory below the
 * GPU's limit, which comes cleared and which the controller scans on a
 * discrete GPU as well, so that it needs neither local memory nor the
 * window before the ring runs.  Returns 0, or ENOMEM.
 */
static int
idle_frame_take(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_allocation *frame;
	uint64_t bytes;
	int error;

	/* A frame any mode fits in, rows on the memory line. */
	display = &device->display;
	bytes = ghci_scan_frame_bytes(display);
	error = ghci_memory_alloc(device, bytes, 0U, &frame);
	if (error != 0)
		return error;

	/* The display holds it, and the GPU will be given its address. */
	mutex_lock(&device->lock);

	ghci_memory_hold(frame);
	frame->reached = 1;

	mutex_unlock(&device->lock);

	/* An address past 32 bits cannot be scanned. */
	if (frame->bus + frame->bytes > SCAN_LIMIT) {
		scan_give_back(device, frame);
		return ENOMEM;
	}

	/* Succeeded: the idle frame is the display's. */
	display->idle_frame = frame;
	return 0;
}

/* Lets go of the display's hold on memory of its own; NULL is nothing. */
static void
scan_give_back(
	struct ghci_device *device,
	struct ghci_allocation *memory)
{
	/* Nothing to give. */
	if (memory == NULL)
		return;

	/* The hold goes under the device's lock; the memory goes back once nothing else holds it. */
	mutex_lock(&device->lock);

	ghci_memory_unhold(device, memory);

	mutex_unlock(&device->lock);
}

/*
 * Lets go of memory the controller scanned: at the next flush when the
 * driver saw it stop reading it, through the quarantine otherwise (the
 * host turned the controller without seeing the processor halt or the
 * controller go idle).  The lock is held.
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
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 13.3, the boot framebuffer released once the first flip is taken).
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

	/* Memory the controller may still read is marked first, so that its last owner puts it in the quarantine. */
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
	if (image->memory == NULL && image->copy_frame < 0 && image->base == display->boot.base)
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
	/* The controller still scans it. */
	if (display->boot_scanning)
		return 1;

	/* A flip of it waits to be taken. */
	if (display->flips[0].placed && boot_image(display, &display->flips[0].image))
		return 1;

	/* Nothing reads it. */
	return 0;
}
