/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display operations of a GHCI device: one output, the display
 * controller of the DISP2 capability, with its primary plane and, when
 * the controller has one and the GPU reports HW_CURSOR, a cursor plane
 * (display-scan.c drives the controller through the ring).
 *
 * A present records a frame and returns; the core then waits for its
 * adoption (DRV_GPU_DISPLAY_UNGATED), so presents, waits and the cursor
 * do not queue behind each other on an open.  A frame is shown one of two
 * ways, chosen once for a swapchain by libvulkan from the constraints:
 *
 *  - shared (a blob the session presents): the controller scans the
 *    session's memory where it is, by its 32-bit physical address, which
 *    the display holds until the controller no longer scans it;
 *  - copied (ordinary storage): the storage is copied into the one of the
 *    driver's two frames that is not on the screen, and is not borrowed
 *    past the call.  The GPU copies through session 0's mappings of the
 *    storage and the frame: the blitter, when the GPU has one and the
 *    frames are local memory, or else the driver's copy program (a
 *    compute dispatch of the executor) on a GPU without the blitter.  The
 *    processor copies on a GPU with the blitter whose frames are host
 *    memory, on one without a shader unit, and when the copy program
 *    cannot make the copy; it reaches local memory through the local
 *    window.
 *
 * The frame's size picks the mode among the controller's modes; a frame
 * of another mode, format or stride than the one lit lights the
 * controller again.  The end of the lease turns the controller dark and
 * lets its images go.
 *
 * The display's events are its topology sequence, which a change of the
 * connection moves; a new connection is a new generation of the output.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-scanout.h>

#include "display-scan.h"
#include "ghci.h"
#include "ghci-cmd.h"

/* The longest a display wait waits, in nanoseconds: one second. */
#define WAIT_MAX_NS			1000000000ULL

/* How long a copied present waits for the GPU's copy. */
#define COPY_WAIT_MS			1000U

/* The cursor's image: 64 x 64 pixels of 4 bytes, rows 256 bytes apart. */
#define CURSOR_STRIDE			(GHCI_DC_CURSOR_SIZE * GHCI_DISPLAY_PIXEL_BYTES)
#define CURSOR_BYTES			(CURSOR_STRIDE * GHCI_DC_CURSOR_SIZE)

/* The places CURSOR_POS takes: 16 bits a coordinate, signed. */
#define CURSOR_POSITION_MIN		(-32768)
#define CURSOR_POSITION_MAX		32767

/* The 32-bit physical addresses the controller takes. */
#define SCAN_LIMIT			0x100000000ULL

/*
 * What copies a copied present into a frame: the processor, the blitter,
 * or the driver's copy program (the executor's copy_rows).  The blitter
 * and the program reach the frames through their mappings into session 0.
 */
enum copy_engine {
	COPY_PROCESSOR = 0,
	COPY_BLITTER,
	COPY_PROGRAM
};

static int display_query(void *private_data, void *opaque, struct gpu_display_info *request);
static int display_mode(void *private_data, void *opaque, struct gpu_display_mode *request);
static int display_claim(void *private_data, void *opaque, struct gpu_display_claim *request);
static int display_release(void *private_data, void *opaque, const struct gpu_display_release *request);
static int display_present(void *private_data, void *opaque, void *object, struct gpu_display_present *request);
static int display_wait(void *private_data, void *opaque, struct gpu_display_wait *request);
static int display_events(void *private_data, void *opaque, uint64_t *sequence);
static int display_cursor_set(void *private_data, void *opaque, void *object, const struct gpu_display_cursor_image *request);
static int display_cursor_move(void *private_data, void *opaque, const struct gpu_display_cursor_move *request);
static int display_cursor_hide(void *private_data, void *opaque, const struct gpu_display_cursor_hide *request);
static int display_adoption_wait(void *private_data, void *opaque, uint64_t lease, uint64_t *sequence, uint64_t *adopted);
static int scanout_query_device(void *private_data, void *opaque, struct gpu_device_info *request);
static int scanout_constraints(void *private_data, void *opaque, struct gpu_scanout_constraints *request);

/*
 * The display operations of a GHCI device whose controller has no cursor
 * plane: query, mode, claim, release, the ungated present and wait with
 * the adoption wait, and the topology sequence.  There is no overlay and
 * no display control.  Immutable.
 */
const struct drv_gpu_display_ops ghci_display_ops = {
	display_query,
	display_mode,
	display_claim,
	display_release,
	display_present,
	display_wait,
	display_events,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	display_adoption_wait,
	DRV_GPU_DISPLAY_UNGATED,
	NULL,
	NULL
};

/*
 * The display operations of a GHCI device whose controller has a cursor
 * plane: the same, and the cursor's three operations.  Immutable.
 */
const struct drv_gpu_display_ops ghci_display_cursor_ops = {
	display_query,
	display_mode,
	display_claim,
	display_release,
	display_present,
	display_wait,
	display_events,
	NULL,
	NULL,
	display_cursor_set,
	display_cursor_move,
	display_cursor_hide,
	display_adoption_wait,
	DRV_GPU_DISPLAY_UNGATED,
	NULL,
	NULL
};

/*
 * The scan-out query of a GHCI device with a display: its roles and the
 * constraints of the shared and copied routes.  The renderer and the
 * display are one device, so an import is the ordinary share import.
 * Immutable.
 */
const struct drv_gpu_scanout_ops ghci_scanout_ops = {
	scanout_query_device,
	scanout_constraints,
	NULL
};

static int lease_check(struct ghci_display *display, void *opaque, uint64_t lease, uint64_t generation);
static int find_mode(const struct ghci_display *display, uint32_t width, uint32_t height, unsigned *mode);
static uint32_t display_formats(const struct ghci_display *display);
static enum copy_engine copy_engine(struct ghci_device *device);
static int buffer_take(struct ghci_device *device, struct ghci_allocation **slot, uint64_t bytes);
static int buffers_take(struct ghci_device *device);
static int present_shared(struct ghci_device *device, void *opaque, struct ghci_allocation *memory, unsigned mode, uint32_t format, struct gpu_display_present *request);
static int present_copied(struct ghci_device *device, void *opaque, struct ghci_allocation *memory, unsigned mode, uint32_t format, struct gpu_display_present *request);
static int copy_by_processor(struct ghci_device *device, struct ghci_allocation *memory, const struct gpu_display_present *request, struct ghci_allocation *frame);
static int copy_by_blitter(struct ghci_device *device, struct ghci_allocation *memory, const struct gpu_display_present *request, const struct ghci_mapping *frame_map);
static int copy_by_program(struct ghci_device *device, struct ghci_allocation *memory, const struct gpu_display_present *request, const struct ghci_mapping *frame_map);
static void memory_let_go(struct ghci_device *device, struct ghci_allocation *memory);
static int cursor_surface_free(const struct ghci_display *display);
static int cursor_rows(struct ghci_device *device, struct ghci_allocation *memory, const struct gpu_display_cursor_image *request, uint8_t *staging);
static void cursor_convert(const uint8_t *staging, uint8_t *surface, uint32_t width, uint32_t height, int swap);
static uint32_t cursor_coordinate(int32_t value);

/* Describes the one output. */
static int
display_query(
	void *private_data,
	void *opaque,
	struct gpu_display_info *request)
{
	struct ghci_device *device;
	struct ghci_display *display;
	const struct ghci_display_mode *current;
	const struct ghci_display_mode *largest;

	(void)opaque;

	/* One output. */
	device = private_data;
	display = &device->display;
	request->count = 1U;

	/* Only the count was asked for. */
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;

	/* Only output 0 exists. */
	if (request->index != 0U)
		return EINVAL;

	/* The output, its plane and its formats, under the display's lock. */
	mutex_lock(&device->display_lock);

	current = &display->modes[display->mode];
	largest = &display->modes[ghci_scan_largest_mode(display)];
	request->display_id = GHCI_DISPLAY_ID;
	request->generation = display->generation;
	request->flags = GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB;
	if (display->connected)
		request->flags |= GPU_DISPLAY_CONNECTED;
	if (display->enabled)
		request->flags |= GPU_DISPLAY_ACTIVE;
	if (display->cursor)
		request->flags |= GPU_DISPLAY_HAS_CURSOR;
	request->plane_count = 1U;
	request->formats = display_formats(display);
	request->max_frame_bytes = ghci_scan_frame_bytes(display);
	request->current_width = current->width;
	request->current_height = current->height;
	request->preferred_width = largest->width;
	request->preferred_height = largest->height;
	request->max_width = largest->width;
	request->max_height = largest->height;
	request->refresh_millihz = current->refresh_millihz;

	mutex_unlock(&device->display_lock);

	/* The monitor's size is not read (no EDID through the controller). */
	request->physical_width_mm = 0U;
	request->physical_height_mm = 0U;
	kern_memset(request->name, 0, sizeof(request->name));
	kern_memcpy(request->name, "ghci display", sizeof("ghci display"));

	/* Succeeded: the output is described. */
	return 0;
}

/* Enumerates or checks the controller's modes. */
static int
display_mode(
	void *private_data,
	void *opaque,
	struct gpu_display_mode *request)
{
	struct ghci_device *device;
	const struct ghci_display_mode *mode;
	uint64_t generation;
	unsigned index;
	int error;

	(void)opaque;

	/* The output's generation now. */
	device = private_data;
	mutex_lock(&device->display_lock);

	generation = device->display.generation;

	mutex_unlock(&device->display_lock);

	/* Only the one output of this generation exists. */
	if (request->display_id != GHCI_DISPLAY_ID)
		return ENOENT;
	if (request->generation != generation)
		return ESTALE;

	/* Enumeration: the modes in order (the table never changes after the attach). */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = device->display.mode_count;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index >= device->display.mode_count)
			return EINVAL;

		mode = &device->display.modes[request->index];
		request->width = mode->width;
		request->height = mode->height;
		request->refresh_millihz = mode->refresh_millihz;
		return 0;
	}

	/* Refuses an operation that does not exist. */
	if (request->operation != GPU_DISPLAY_MODE_VALIDATE)
		return EINVAL;

	/* Validation: one of the modes, at its refresh (0 asks for it). */
	error = find_mode(&device->display, request->width, request->height, &index);
	if (error != 0)
		return error;

	/* The refresh, when one is asked for, must be the mode's. */
	mode = &device->display.modes[index];
	if (request->refresh_millihz != 0U && request->refresh_millihz != mode->refresh_millihz)
		return EINVAL;

	/* Succeeded: the mode exists, at its refresh. */
	request->refresh_millihz = mode->refresh_millihz;
	return 0;
}

/* Gives the display's lease to a session; EBUSY while another holds it or a lease is still ending. */
static int
display_claim(
	void *private_data,
	void *opaque,
	struct gpu_display_claim *request)
{
	struct ghci_device *device;
	struct ghci_display *display;
	int error;

	/* Only plane 0 of the one output exists. */
	device = private_data;
	display = &device->display;
	if (request->display_id != GHCI_DISPLAY_ID)
		return ENOENT;
	if (request->plane_index != 0U)
		return EINVAL;

	/* The frames a copied present fills and the cursor's surfaces, before any lock. */
	error = buffers_take(device);
	if (error != 0)
		return error;

	/* Takes the lease when it is free and the last one has finished ending. */
	mutex_lock(&device->display_lock);

	error = 0;
	if (request->generation != display->generation) {
		error = ESTALE;
	} else if (display->owner != NULL || display->ending) {
		error = EBUSY;
	} else {
		/* The session holds a new lease with no frame yet; the controller stays as it is until the first. */
		display->owner = opaque;
		display->lease = display->next_lease;
		display->next_lease++;
		display->sequence = 0U;
		display->adopted = 0U;
		display->uncertain = 0;
		display->cursor_image = 0;
		request->lease = display->lease;

		/* The idle frame is not lit again for this connection once a lease has held it. */
		display->idle_generation = display->generation;
	}

	mutex_unlock(&device->display_lock);

	/* Reports a lease that could not be taken. */
	if (error != 0)
		return error;

	/* Succeeded: the session holds the display. */
	return 0;
}

/*
 * Ends the lease of its holder: the call returns once the controller is
 * dark and no longer scans the lease's memory, which is then let go of.
 * The wait holds no lock while it sleeps and always ends: the dark
 * packet's ticket passes, or fails at the reset that ends a hang, and a
 * ring that no longer runs is turned dark by the host.  EINVAL for
 * another session's lease.
 */
static int
display_release(
	void *private_data,
	void *opaque,
	const struct gpu_display_release *request)
{
	struct ghci_device *device;
	struct ghci_display *display;
	uint64_t mark;
	int done;

	/* Only the holder's own lease ends. */
	device = private_data;
	display = &device->display;
	mutex_lock(&device->display_lock);

	if (display->owner != opaque || display->lease != request->lease) {
		mutex_unlock(&device->display_lock);
		return EINVAL;
	}

	ghci_scan_lease_end(device);

	mutex_unlock(&device->display_lock);

	/* Waits for the dark packet and the cancelled records; a dark packet a recovery refused is placed again. */
	for (;;) {
		mark = ghci_scan_mark(device);
		mutex_lock(&device->display_lock);

		if (display->ending && display->dark_placed < 0)
			ghci_scan_lease_end(device);
		done = ghci_scan_lease_done(device);

		mutex_unlock(&device->display_lock);

		/* Ends once everything was let go. */
		ghci_scan_flush(device);
		if (done)
			break;

		ghci_scan_sleep(device, mark, clock_ticks() + kern_ms_to_ticks(GHCI_HANG_MS));
	}

	/* Succeeded: the lease's memory is no longer scanned, and the display may be claimed again. */
	return 0;
}

/*
 * Records a frame of the lease holder and returns its number; the core
 * then waits for its adoption.  Returns 0, EINVAL for a frame the
 * controller cannot show, EBUSY for another session's lease or an
 * uncertain output, ESTALE for an old generation, ENXIO when
 * disconnected, or the copy's error.
 */
static int
display_present(
	void *private_data,
	void *opaque,
	void *object,
	struct gpu_display_present *request)
{
	struct ghci_device *device;
	struct ghci_display *display;
	struct ghci_resource *storage;
	uint64_t end;
	uint32_t format;
	uint32_t formats;
	unsigned mode;
	int error;

	device = private_data;
	display = &device->display;
	storage = object;

	/* The packed formats the controller scans. */
	formats = display_formats(display);
	if (request->format == GPU_PIXEL_BGRA8888 && (formats & GPU_DISPLAY_FORMAT_BGRA8888) != 0U) {
		format = GHCI_DC_FMT_BGRA8;
	} else if (request->format == GPU_PIXEL_RGBA8888 && (formats & GPU_DISPLAY_FORMAT_RGBA8888) != 0U) {
		format = GHCI_DC_FMT_RGBA8;
	} else {
		return EINVAL;
	}

	/* The frame's size is one of the modes (the table never changes after the attach). */
	error = find_mode(display, request->width, request->height, &mode);
	if (error != 0)
		return error;

	/* Rows of whole pixels, each at least a row wide, inside the storage. */
	if ((request->stride % GHCI_DISPLAY_PIXEL_BYTES) != 0U || (request->offset % GHCI_DISPLAY_PIXEL_BYTES) != 0U)
		return EINVAL;
	if (request->stride < request->width * GHCI_DISPLAY_PIXEL_BYTES)
		return EINVAL;
	end = request->offset + (uint64_t)request->stride * (request->height - 1U) + (uint64_t)request->width * GHCI_DISPLAY_PIXEL_BYTES;
	if (end > storage->allocation->bytes)
		return EINVAL;

	/* A blob is scanned where it is; ordinary storage is copied. */
	if ((request->flags & GPU_DISPLAY_PRESENT_BLOB) != 0U)
		error = present_shared(device, opaque, storage->allocation, mode, format, request);
	else
		error = present_copied(device, opaque, storage->allocation, mode, format, request);
	if (error != 0)
		return error;

	/* Succeeded: the frame is recorded with its number in the request. */
	return 0;
}

/*
 * Reports the newest frame adopted under the lease, waiting up to the
 * request's time (at most a second) for its sequence.  EBUSY for another
 * session's lease, EINVAL for a sequence not presented yet, EAGAIN when
 * the time is zero and the sequence is not adopted, ETIMEDOUT, ENXIO when
 * disconnected.
 */
static int
display_wait(
	void *private_data,
	void *opaque,
	struct gpu_display_wait *request)
{
	struct ghci_device *device;
	struct ghci_display *display;
	uint64_t timeout_ns;
	uint64_t deadline;
	uint64_t mark;
	uint64_t now;
	int error;

	/* The wait ends at most a second from now. */
	device = private_data;
	display = &device->display;
	timeout_ns = request->timeout_ns;
	if (timeout_ns > WAIT_MAX_NS)
		timeout_ns = WAIT_MAX_NS;
	deadline = clock_ticks() + kern_ms_to_ticks((unsigned)((timeout_ns + 999999U) / 1000000U));

	/* Looks at the records, sleeping between looks, until the sequence is adopted. */
	for (;;) {
		mark = ghci_scan_mark(device);
		mutex_lock(&device->display_lock);

		ghci_scan_update(device);
		error = EAGAIN;
		if (display->owner != opaque || display->lease != request->lease) {
			error = EBUSY;
		} else if (!display->connected) {
			error = ENXIO;
		} else if (request->sequence > display->sequence) {
			error = EINVAL;
		} else if (display->adopted != 0U && display->adopted >= request->sequence) {
			error = 0;
			request->completed_sequence = display->adopted;
			request->present_time_ns = display->shown_time_ns;
			request->generation = display->generation;
		}

		mutex_unlock(&device->display_lock);

		/* Ends at an answer, at once for a zero time, or at the deadline. */
		ghci_scan_flush(device);
		now = clock_ticks();
		if (error != EAGAIN || timeout_ns == 0U)
			break;
		if (now >= deadline) {
			error = ETIMEDOUT;
			break;
		}

		ghci_scan_sleep(device, mark, deadline);
	}

	/* Reports a wait the lease does not allow, or one that ran out. */
	if (error != 0)
		return error;

	/* Succeeded: the sequence has been adopted. */
	return 0;
}

/* Reports the topology sequence, under the interrupt's lock only.  No hardware access. */
static int
display_events(
	void *private_data,
	void *opaque,
	uint64_t *sequence)
{
	struct ghci_device *device;
	unsigned long irq;

	(void)opaque;

	/* The sequence a change of the connection moves. */
	device = private_data;
	irq = spin_lock_irqsave(&device->irq_lock);

	*sequence = device->display.topology;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the sequence is read. */
	return 0;
}

/*
 * Sets the cursor's image from session storage: the processor reads the
 * rows (through the local window for local memory) and puts them in a
 * free cursor surface (red and blue swapped for RGBA8888, the rest of the
 * 64 x 64 image cleared), which is then flipped.  EBUSY when both
 * surfaces are in use (the call may succeed a refresh later).
 */
static int
display_cursor_set(
	void *private_data,
	void *opaque,
	void *object,
	const struct gpu_display_cursor_image *request)
{
	struct ghci_device *device;
	struct ghci_display *display;
	struct ghci_resource *storage;
	uint8_t *staging;
	int surface;
	int swap;
	int error;

	device = private_data;
	display = &device->display;
	storage = object;

	/* An image that fits the plane. */
	if (request->width == 0U || request->height == 0U ||
	    request->width > GHCI_DC_CURSOR_SIZE || request->height > GHCI_DC_CURSOR_SIZE)
		return EINVAL;

	/* The staging of the rows and the converted image. */
	staging = kern_malloc(2U * CURSOR_BYTES);
	if (staging == NULL)
		return ENOMEM;

	/* A free surface of the lease's display takes the image and is flipped. */
	mutex_lock(&device->display_lock);

	error = lease_check(display, opaque, request->lease, request->generation);
	surface = -1;
	if (error == 0) {
		surface = cursor_surface_free(display);
		if (surface < 0)
			error = EBUSY;
	}

	if (error == 0)
		error = cursor_rows(device, storage->allocation, request, staging);
	if (error == 0) {
		swap = 0;
		if (request->format == GPU_PIXEL_RGBA8888)
			swap = 1;
		cursor_convert(staging, staging + CURSOR_BYTES, request->width, request->height, swap);
		error = ghci_scan_write(device, display->surfaces[surface], 0U, staging + CURSOR_BYTES, CURSOR_BYTES);
	}

	if (error == 0) {
		display->cursor_image = 1;
		ghci_scan_cursor(device, surface);
	}

	mutex_unlock(&device->display_lock);

	/* The staging goes; a refused image is reported. */
	kern_free(staging);
	if (error != 0)
		return error;

	/* Succeeded: the image shows from the controller's next read of it. */
	return 0;
}

/* Shows the cursor at a place, from the next frame; EINVAL before an image was set. */
static int
display_cursor_move(
	void *private_data,
	void *opaque,
	const struct gpu_display_cursor_move *request)
{
	struct ghci_device *device;
	struct ghci_display *display;
	uint32_t position;
	int error;

	/* The cursor's registers are the host's in every state of the processor. */
	device = private_data;
	display = &device->display;
	mutex_lock(&device->display_lock);

	error = lease_check(display, opaque, request->lease, request->generation);
	if (error == 0 && !display->cursor_image)
		error = EINVAL;
	if (error == 0) {
		position = cursor_coordinate(request->x) | (cursor_coordinate(request->y) << 16);
		ghci_write(device, display->registers + GHCI_DC_CURSOR_POS, position);
		ghci_write(device, display->registers + GHCI_DC_CURSOR_CTRL, 1U);
	}

	mutex_unlock(&device->display_lock);

	/* Reports a lease that does not allow the move. */
	if (error != 0)
		return error;

	/* Succeeded: the controller takes the place at the start of the next frame. */
	return 0;
}

/* Hides the cursor from the next frame; its image is kept. */
static int
display_cursor_hide(
	void *private_data,
	void *opaque,
	const struct gpu_display_cursor_hide *request)
{
	struct ghci_device *device;
	struct ghci_display *display;
	int error;

	/* The cursor's register is the host's in every state of the processor. */
	device = private_data;
	display = &device->display;
	mutex_lock(&device->display_lock);

	error = lease_check(display, opaque, request->lease, request->generation);
	if (error == 0)
		ghci_write(device, display->registers + GHCI_DC_CURSOR_CTRL, 0U);

	mutex_unlock(&device->display_lock);

	/* Reports a lease that does not allow the change. */
	if (error != 0)
		return error;

	/* Succeeded: the controller hides the cursor from the next frame. */
	return 0;
}

/*
 * Waits for the adoption of a recorded present (the ungated core's second
 * step), holding no pin and no lock while it sleeps.  sequence holds the
 * record's number, which is also the number the caller sees; adopted,
 * when given, receives the highest adopted number.  EBUSY when the lease
 * ended, EIO when the GPU no longer runs, ETIMEDOUT when the adoption did
 * not come (the lease's later presents then fail with EBUSY).
 */
static int
display_adoption_wait(
	void *private_data,
	void *opaque,
	uint64_t lease,
	uint64_t *sequence,
	uint64_t *adopted)
{
	struct ghci_device *device;
	struct ghci_display *display;
	uint64_t deadline;
	uint64_t mark;
	uint64_t now;
	int usable;
	int error;

	/* The adoption comes within a bounded time. */
	device = private_data;
	display = &device->display;
	deadline = clock_ticks() + kern_ms_to_ticks(GHCI_DISPLAY_ADOPTION_MS);

	/* Looks at the records, sleeping between looks, until the record is adopted. */
	for (;;) {
		mark = ghci_scan_mark(device);
		mutex_lock(&device->display_lock);

		ghci_scan_update(device);
		usable = ghci_scan_ring_usable(device);
		error = EAGAIN;
		if (display->owner != opaque || display->lease != lease) {
			error = EBUSY;
		} else if (display->adopted >= *sequence) {
			error = 0;
		} else if (display->uncertain) {
			error = ETIMEDOUT;
		} else if (!usable) {
			error = EIO;
		}

		if (adopted != NULL)
			*adopted = display->adopted;

		mutex_unlock(&device->display_lock);

		/* Ends at an answer, or at the deadline: the output is then uncertain. */
		ghci_scan_flush(device);
		now = clock_ticks();
		if (error != EAGAIN)
			break;
		if (now >= deadline) {
			mutex_lock(&device->display_lock);

			display->uncertain = 1;

			mutex_unlock(&device->display_lock);

			kern_logf("ghci: %s: present %llu not adopted in time\n", device->name, (unsigned long long)*sequence);
			error = ETIMEDOUT;
			break;
		}

		ghci_scan_sleep(device, mark, deadline);
	}

	/* Reports a lease that ended, a lost GPU or an adoption that did not come. */
	if (error != 0)
		return error;

	/* Succeeded: the record is shown, or replaced by one that is. */
	return 0;
}

/* Reports the device's roles: a renderer when it runs commands, and a display. */
static int
scanout_query_device(
	void *private_data,
	void *opaque,
	struct gpu_device_info *request)
{
	struct ghci_device *device;

	(void)opaque;

	/* The display always; the renderer with the command operation. */
	device = private_data;
	request->roles = GPU_DEVICE_DISPLAY;
	if ((device->ops.capabilities & GPU_CAP_COMMAND) != 0U)
		request->roles |= GPU_DEVICE_RENDER;

	/* Succeeded: the roles are known (no companion: the device renders for itself). */
	return 0;
}

/*
 * Reports the constraints of the output's generation: the shared and
 * copied routes, the packed formats, rows and images on the memory line,
 * no placement (host and local memory are both scanned), and the 32-bit
 * physical addresses the controller takes.
 */
static int
scanout_constraints(
	void *private_data,
	void *opaque,
	struct gpu_scanout_constraints *request)
{
	struct ghci_device *device;
	uint64_t generation;

	(void)opaque;

	/* The output's generation now. */
	device = private_data;
	mutex_lock(&device->display_lock);

	generation = device->display.generation;

	mutex_unlock(&device->display_lock);

	/* Only the one output of this generation exists. */
	if (request->display_id != GHCI_DISPLAY_ID)
		return ENOENT;
	if (request->generation != generation)
		return ESTALE;

	/* Succeeded: the routes and their layout. */
	request->flags = GPU_SCANOUT_SHARED | GPU_SCANOUT_COPY;
	request->formats = display_formats(&device->display);
	request->stride_alignment = GHCI_DISPLAY_LINE_BYTES;
	request->offset_alignment = GHCI_DISPLAY_LINE_BYTES;
	request->placement = 0U;
	request->max_dma_address = SCAN_LIMIT - 1U;
	return 0;
}

/*
 * Checks that a request names this open's lease of the output's current
 * generation, connected and not uncertain.  Returns 0, EBUSY, ESTALE or
 * ENXIO; the display's lock is held.
 */
static int
lease_check(
	struct ghci_display *display,
	void *opaque,
	uint64_t lease,
	uint64_t generation)
{
	/* Another open's lease, none, or one whose adoption did not come. */
	if (display->owner != opaque || display->lease != lease || display->uncertain)
		return EBUSY;

	/* An old generation of the output, or one without a sink. */
	if (generation != display->generation)
		return ESTALE;
	if (!display->connected)
		return ENXIO;

	/* Succeeded: the lease may change the output. */
	return 0;
}

/* Finds the mode of a size among the controller's.  Returns 0 or EINVAL. */
static int
find_mode(
	const struct ghci_display *display,
	uint32_t width,
	uint32_t height,
	unsigned *mode)
{
	unsigned index;

	/* Looks at each mode the controller has. */
	for (index = 0U; index < display->mode_count; index++) {
		if (display->modes[index].width != width || display->modes[index].height != height)
			continue;

		*mode = index;
		return 0;
	}

	/* No mode has the size. */
	return EINVAL;
}

/* Reports the packed formats the controller scans, from DC_CAPS. */
static uint32_t
display_formats(
	const struct ghci_display *display)
{
	uint32_t formats;

	/* BGRA8 and RGBA8, as the controller names them. */
	formats = 0U;
	if ((display->controller_caps & GHCI_DC_CAPS_BGRA8) != 0U)
		formats |= GPU_DISPLAY_FORMAT_BGRA8888;
	if ((display->controller_caps & GHCI_DC_CAPS_RGBA8) != 0U)
		formats |= GPU_DISPLAY_FORMAT_RGBA8888;

	/* Succeeded: the formats. */
	return formats;
}

/*
 * Chooses what copies a copied present into a frame: the blitter when
 * the GPU has one and the frames are local memory; the driver's copy
 * program on a GPU without the blitter whose executor makes copies of
 * rows (it runs on the shader unit); the processor otherwise.
 */
static enum copy_engine
copy_engine(
	struct ghci_device *device)
{
	/* A GPU with the blitter: the blitter fills local frames, the processor host ones. */
	if (ghci_caps_has(&device->caps, GHCI_HAS_BLT)) {
		if (device->local_ready)
			return COPY_BLITTER;
		return COPY_PROCESSOR;
	}

	/* Without the blitter: the copy program, where the executor runs it. */
	if (device->executor_ops != NULL && device->executor_ops->copy_rows != NULL)
		return COPY_PROGRAM;

	/* Succeeded: no engine of the GPU copies; the processor does. */
	return COPY_PROCESSOR;
}

/*
 * Fills one slot of the display's buffers with memory the controller
 * scans, unless it is filled already; a slot another claim filled
 * meanwhile keeps its memory.  The caller holds no lock of the device.
 */
static int
buffer_take(
	struct ghci_device *device,
	struct ghci_allocation **slot,
	uint64_t bytes)
{
	struct ghci_allocation *memory;
	int filled;
	int error;

	/* A slot already filled. */
	mutex_lock(&device->display_lock);

	filled = 0;
	if (*slot != NULL)
		filled = 1;

	mutex_unlock(&device->display_lock);
	if (filled)
		return 0;

	/* The memory, taken without the display's lock. */
	error = ghci_scan_alloc(device, bytes, &memory);
	if (error != 0)
		return error;

	/* The slot takes it, unless another claim was first. */
	mutex_lock(&device->display_lock);

	filled = 1;
	if (*slot == NULL) {
		*slot = memory;
		filled = 0;
	}

	mutex_unlock(&device->display_lock);

	/* Memory a slot did not take goes back. */
	if (filled) {
		mutex_lock(&device->lock);

		ghci_memory_unhold(device, memory);

		mutex_unlock(&device->lock);
	}

	/* Succeeded: the slot is filled. */
	return 0;
}

/*
 * Takes the driver's buffers once: two frames any mode fits in that a
 * copied present fills (local memory on a discrete GPU, mapped into
 * session 0 when the GPU fills them), and, with a cursor plane, two
 * cursor surfaces.  They stay until the detach.  The caller holds no lock
 * of the device.
 */
static int
buffers_take(
	struct ghci_device *device)
{
	struct ghci_display *display;
	struct ghci_mapping *mapping;
	uint64_t bytes;
	unsigned index;
	int gpu;
	int mapped;
	int error;

	/* Each frame holds a frame of any mode, with rows on the memory line. */
	display = &device->display;
	bytes = ghci_scan_frame_bytes(display);
	for (index = 0U; index < 2U; index++) {
		error = buffer_take(device, &display->frames[index], bytes);
		if (error != 0)
			return error;
	}

	/* Frames the blitter or the copy program fills are mapped into session 0. */
	gpu = 0;
	if (copy_engine(device) != COPY_PROCESSOR)
		gpu = 1;
	for (index = 0U; index < 2U && gpu; index++) {
		mutex_lock(&device->display_lock);

		mapped = 0;
		if (display->frame_maps[index] != NULL)
			mapped = 1;

		mutex_unlock(&device->display_lock);
		if (mapped)
			continue;

		error = ghci_vm_map(device, &device->vm0, display->frames[index], 0U, &mapping);
		if (error != 0)
			return error;

		/* The slot takes the mapping, unless another claim was first. */
		mutex_lock(&device->display_lock);

		mapped = 1;
		if (display->frame_maps[index] == NULL) {
			display->frame_maps[index] = mapping;
			mapped = 0;
		}

		mutex_unlock(&device->display_lock);
		if (mapped)
			ghci_vm_unmap(device, mapping);
	}

	/* The two cursor surfaces, with a cursor plane. */
	for (index = 0U; index < 2U && display->cursor; index++) {
		error = buffer_take(device, &display->surfaces[index], CURSOR_BYTES);
		if (error != 0)
			return error;
	}

	/* Succeeded: the buffers are the display's. */
	return 0;
}

/*
 * Records a shared present: the blob's memory, held by the display, is
 * scanned where it is, by its 32-bit physical address, its first row and
 * its stride on the memory line.  A FIFO present waits for room in the
 * queue.
 */
static int
present_shared(
	struct ghci_device *device,
	void *opaque,
	struct ghci_allocation *memory,
	unsigned mode,
	uint32_t format,
	struct gpu_display_present *request)
{
	struct ghci_display *display;
	struct ghci_display_image image;
	uint64_t base;
	uint64_t deadline;
	uint64_t mark;
	uint64_t now;
	int usable;
	int fifo;
	int error;

	/* The image lies below 4 GiB, its first row and its stride on the memory line. */
	display = &device->display;
	base = memory->bus + request->offset;
	if (memory->bus + memory->bytes > SCAN_LIMIT)
		return EINVAL;
	if ((base % GHCI_DISPLAY_LINE_BYTES) != 0U || (request->stride % GHCI_DISPLAY_LINE_BYTES) != 0U)
		return EINVAL;

	/* The image the controller scans. */
	kern_memset(&image, 0, sizeof(image));
	image.base = (uint32_t)base;
	image.stride = request->stride;
	image.format = format;
	image.mode = mode;
	image.memory = memory;
	image.copy_frame = -1;
	fifo = 0;
	if ((request->flags & GPU_DISPLAY_PRESENT_FIFO) != 0U)
		fifo = 1;

	/* The display holds the memory until the controller no longer scans it. */
	mutex_lock(&device->lock);

	ghci_memory_hold(memory);
	memory->reached = 1;

	mutex_unlock(&device->lock);

	/* Records the image, waiting for room in the queue when it is full. */
	deadline = clock_ticks() + kern_ms_to_ticks(GHCI_DISPLAY_ADOPTION_MS);
	for (;;) {
		mark = ghci_scan_mark(device);
		mutex_lock(&device->display_lock);

		ghci_scan_update(device);
		error = lease_check(display, opaque, request->lease, request->generation);
		usable = ghci_scan_ring_usable(device);
		if (error == 0 && !usable)
			error = EIO;
		if (error == 0 && display->copy_frame_busy >= 0)
			error = EAGAIN;
		if (error == 0) {
			image.sequence = display->sequence + 1U;
			error = ghci_scan_record(device, &image, fifo);
		}

		if (error == 0) {
			display->sequence = image.sequence;
			request->sequence = image.sequence;
		}

		mutex_unlock(&device->display_lock);

		/* Ends once recorded or refused; a full queue is waited for, a bounded time. */
		ghci_scan_flush(device);
		now = clock_ticks();
		if (error != EAGAIN)
			break;
		if (now >= deadline) {
			error = EBUSY;
			break;
		}

		ghci_scan_sleep(device, mark, deadline);
	}

	/* A present that was not recorded lets the memory go. */
	if (error != 0) {
		memory_let_go(device, memory);
		return error;
	}

	/* Succeeded: the image is recorded. */
	return 0;
}

/*
 * Records a copied present: once every present before it was placed and
 * plane 0 has no flip placed, the storage is copied into the driver's
 * frame not on the screen, which is then recorded.  The storage is not
 * borrowed after the call.
 */
static int
present_copied(
	struct ghci_device *device,
	void *opaque,
	struct ghci_allocation *memory,
	unsigned mode,
	uint32_t format,
	struct gpu_display_present *request)
{
	struct ghci_display *display;
	struct ghci_display_image image;
	struct ghci_mapping *frame_map;
	enum copy_engine engine;
	uint64_t deadline;
	uint64_t mark;
	uint64_t now;
	int usable;
	int frame;
	int error;

	/* Waits until the frame not on the screen is free: nothing waits and no flip is placed. */
	display = &device->display;
	deadline = clock_ticks() + kern_ms_to_ticks(GHCI_DISPLAY_ADOPTION_MS);
	frame = -1;
	frame_map = NULL;
	for (;;) {
		mark = ghci_scan_mark(device);
		mutex_lock(&device->display_lock);

		ghci_scan_update(device);
		error = lease_check(display, opaque, request->lease, request->generation);
		usable = ghci_scan_ring_usable(device);
		if (error == 0 && !usable)
			error = EIO;
		if (error == 0 && (display->queue_count != 0U || display->flips[0].placed || display->copy_frame_busy >= 0))
			error = EAGAIN;
		if (error == 0) {
			/* The frame the controller does not scan; it is the copy's until recorded. */
			frame = 0;
			if (display->shown.copy_frame == 0)
				frame = 1;
			display->copy_frame_busy = frame;
			frame_map = display->frame_maps[frame];
		}

		mutex_unlock(&device->display_lock);

		/* Ends once a frame is the copy's or the present is refused, or at the deadline. */
		ghci_scan_flush(device);
		now = clock_ticks();
		if (error != EAGAIN)
			break;
		if (now >= deadline) {
			error = EBUSY;
			break;
		}

		ghci_scan_sleep(device, mark, deadline);
	}

	/* Reports a present that may not be recorded. */
	if (error != 0)
		return error;

	/* The copy into the frame, outside the display's lock: the blitter's, the copy program's, or the processor's. */
	engine = COPY_PROCESSOR;
	if (frame_map != NULL)
		engine = copy_engine(device);
	if (engine == COPY_BLITTER)
		error = copy_by_blitter(device, memory, request, frame_map);
	else if (engine == COPY_PROGRAM)
		error = copy_by_program(device, memory, request, frame_map);
	else
		error = copy_by_processor(device, memory, request, display->frames[frame]);

	/* A copy the program cannot make is the processor's. */
	if (error == ENOTSUP && engine == COPY_PROGRAM)
		error = copy_by_processor(device, memory, request, display->frames[frame]);

	/* The frame is recorded while the lease is still the caller's. */
	mutex_lock(&device->display_lock);

	display->copy_frame_busy = -1;
	if (error == 0)
		error = lease_check(display, opaque, request->lease, request->generation);
	if (error == 0) {
		kern_memset(&image, 0, sizeof(image));
		image.sequence = display->sequence + 1U;
		image.base = (uint32_t)display->frames[frame]->bus;
		image.stride = ghci_scan_stride(request->width);
		image.format = format;
		image.mode = mode;
		image.memory = NULL;
		image.copy_frame = frame;
		error = ghci_scan_record(device, &image, 1);
	}

	if (error == 0) {
		display->sequence = image.sequence;
		request->sequence = image.sequence;
	}

	mutex_unlock(&device->display_lock);

	/* Reports a copy that failed, or a lease that ended meanwhile. */
	ghci_scan_flush(device);
	if (error != 0)
		return error;

	/* Succeeded: the copied frame is recorded. */
	return 0;
}

/*
 * Copies a present's rows into a frame by the processor: through the
 * views of host memory, and through the local window for local memory
 * (held open for the copy; the last word written is read back so that
 * the rows reached local memory before the flip).  Returns 0, or ENOTSUP
 * when the window does not show the memory.
 */
static int
copy_by_processor(
	struct ghci_device *device,
	struct ghci_allocation *memory,
	const struct gpu_display_present *request,
	struct ghci_allocation *frame)
{
	uint64_t source;
	uint64_t target;
	uint32_t stride;
	uint32_t row_bytes;
	uint32_t row;
	int window;
	int error;

	/* The window, open for the copy when either side is local memory. */
	window = 0;
	if (memory->kind == GHCI_MEMORY_LOCAL || frame->kind == GHCI_MEMORY_LOCAL)
		window = 1;
	if (window) {
		error = ghci_memory_window_hold(device);
		if (error != 0)
			return error;
	}

	/* Each row, from the storage's stride to the frame's. */
	error = 0;
	if (memory->view == NULL || frame->view == NULL)
		error = ENOTSUP;
	stride = ghci_scan_stride(request->width);
	row_bytes = request->width * GHCI_DISPLAY_PIXEL_BYTES;
	for (row = 0U; row < request->height && error == 0; row++) {
		source = request->offset + (uint64_t)row * request->stride;
		target = (uint64_t)row * stride;
		kern_memcpy(frame->view + target, memory->view + source, row_bytes);
	}

	/* The last word written comes back through the window before it closes. */
	if (window) {
		if (error == 0 && frame->kind == GHCI_MEMORY_LOCAL) {
			target = (uint64_t)(request->height - 1U) * stride + row_bytes - 4U;
			(void)*(volatile uint32_t *)(void *)(frame->view + target);
		}

		ghci_memory_window_unhold(device);
	}

	/* Reports a copy the views did not allow. */
	if (error != 0)
		return error;

	/* Succeeded: the rows are in the frame. */
	return 0;
}

/*
 * Copies a present's rows into a frame by the blitter: the storage is
 * mapped into session 0 for the copy, CMD_COPY_BUFFER copies the rows
 * (one copy when the strides agree), and the copy's fence is waited for
 * with no lock held.  Returns 0, EIO when a reset or the loss of the
 * device ended the copy, ETIMEDOUT, or the mapping's error.
 *
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 6.0, N12: transfers between host memory and local memory by
 * CMD_COPY_BUFFER between mappings of a session).
 */
static int
copy_by_blitter(
	struct ghci_device *device,
	struct ghci_allocation *memory,
	const struct gpu_display_present *request,
	const struct ghci_mapping *frame_map)
{
	struct ghci_mapping *source;
	struct ghci_ticket ticket;
	uint32_t *packets;
	uint64_t from;
	uint64_t to;
	uint32_t stride;
	uint32_t row_bytes;
	uint32_t copies;
	uint32_t bytes;
	uint32_t dwords;
	uint32_t row;
	int error;

	/* The storage, readable in session 0 for the copy. */
	error = ghci_vm_map(device, &device->vm0, memory, GHCI_VM_MAP_NO_WRITE, &source);
	if (error != 0)
		return error;

	/* One copy when the rows lie as the frame's do, one a row otherwise. */
	stride = ghci_scan_stride(request->width);
	row_bytes = request->width * GHCI_DISPLAY_PIXEL_BYTES;
	copies = request->height;
	bytes = row_bytes;
	if (request->stride == stride) {
		copies = 1U;
		bytes = stride * (request->height - 1U) + row_bytes;
	}

	packets = kern_malloc((size_t)copies * GHCI_DW_COPY_BUFFER * sizeof(*packets));
	if (packets == NULL) {
		ghci_vm_unmap(device, source);
		return ENOMEM;
	}

	/* Each copy, from the storage's mapping to the frame's. */
	dwords = 0U;
	for (row = 0U; row < copies; row++) {
		from = source->va + request->offset + (uint64_t)row * request->stride;
		to = frame_map->va + (uint64_t)row * stride;
		dwords += ghci_cmd_copy_buffer(&packets[dwords], from, to, bytes, GHCI_COPY_FLUSH_DST, 0U);
	}

	/* The copies as the driver's own work, and their fence, waited for with no lock held. */
	error = ghci_cp_submit(device, NULL, packets, dwords, &ticket);
	kern_free(packets);
	if (error == 0)
		error = ghci_cp_wait(device, &ticket, COPY_WAIT_MS);

	/* The mapping goes once the GPU is done with it. */
	ghci_vm_unmap(device, source);

	/* Reports a copy that did not finish. */
	if (error != 0)
		return error;

	/* Succeeded: the rows are in the frame. */
	return 0;
}

/*
 * Copies a present's rows into a frame by the driver's copy program: the
 * storage is mapped into session 0 for the copy, the executor places the
 * program's dispatch, and its fence is waited for with no lock held.
 * Returns 0, ENOTSUP when the program cannot make the copy (nothing was
 * placed; the processor copies instead), EIO when a reset or the loss of
 * the device ended the copy, ETIMEDOUT, or the mapping's error.
 */
static int
copy_by_program(
	struct ghci_device *device,
	struct ghci_allocation *memory,
	const struct gpu_display_present *request,
	const struct ghci_mapping *frame_map)
{
	struct ghci_mapping *source;
	struct ghci_ticket ticket;
	struct ghci_rows rows;
	int error;

	/* The storage, readable in session 0 for the copy. */
	error = ghci_vm_map(device, &device->vm0, memory, GHCI_VM_MAP_NO_WRITE, &source);
	if (error != 0)
		return error;

	/* The rows, from the storage's stride to the frame's. */
	kern_memset(&rows, 0, sizeof(rows));
	rows.source = source->va + request->offset;
	rows.source_stride = request->stride;
	rows.destination = frame_map->va;
	rows.destination_stride = ghci_scan_stride(request->width);
	rows.bytes = request->width * GHCI_DISPLAY_PIXEL_BYTES;
	rows.rows = request->height;

	/* The copy as the driver's own work, and its fence, waited for with no lock held. */
	error = device->executor_ops->copy_rows(device, &rows, &ticket);
	if (error == 0)
		error = ghci_cp_wait(device, &ticket, COPY_WAIT_MS);

	/* The mapping goes once the GPU is done with it. */
	ghci_vm_unmap(device, source);

	/* Reports a copy that did not finish. */
	if (error != 0)
		return error;

	/* Succeeded: the rows are in the frame. */
	return 0;
}

/* Lets go of memory the display held for a present that was not recorded. */
static void
memory_let_go(
	struct ghci_device *device,
	struct ghci_allocation *memory)
{
	/* The hold goes under the device's lock; memory nothing else holds goes back. */
	mutex_lock(&device->lock);

	ghci_memory_unhold(device, memory);

	mutex_unlock(&device->lock);
}

/* Reports a cursor surface neither shown nor flipped, or -1 when both are in use. */
static int
cursor_surface_free(
	const struct ghci_display *display)
{
	int surface;

	/* The first surface the controller does not have and no flip names. */
	for (surface = 0; surface < 2; surface++) {
		if (surface == display->cursor_shown)
			continue;
		if (display->flips[1].placed && display->flips[1].surface == surface)
			continue;
		if (display->cursor_pending == surface)
			continue;

		return surface;
	}

	/* Both are the controller's for now. */
	return -1;
}

/*
 * Reads a cursor image's rows from session storage into the staging, rows
 * 256 bytes apart: through the view of host memory, or through the local
 * window.  Returns 0, or the window's error.
 */
static int
cursor_rows(
	struct ghci_device *device,
	struct ghci_allocation *memory,
	const struct gpu_display_cursor_image *request,
	uint8_t *staging)
{
	uint64_t source;
	uint32_t row_bytes;
	uint32_t row;
	int error;

	/* The rows, each its width of pixels. */
	row_bytes = request->width * GHCI_DISPLAY_PIXEL_BYTES;
	for (row = 0U; row < request->height; row++) {
		source = request->offset + (uint64_t)row * request->stride;
		if (source > memory->bytes || row_bytes > memory->bytes - source)
			return EINVAL;
		if (memory->kind == GHCI_MEMORY_LOCAL) {
			error = ghci_memory_local_read(device, memory, source, &staging[row * CURSOR_STRIDE], row_bytes);
			if (error != 0)
				return error;
		} else {
			kern_memcpy(&staging[row * CURSOR_STRIDE], memory->view + source, row_bytes);
		}
	}

	/* Succeeded: the rows are in the staging. */
	return 0;
}

/*
 * Puts a cursor image's rows from the staging into a surface: BGRA8
 * pixels (red and blue swapped from RGBA8 when swap is set), the part
 * past the image's width and height cleared.
 */
static void
cursor_convert(
	const uint8_t *staging,
	uint8_t *surface,
	uint32_t width,
	uint32_t height,
	int swap)
{
	const uint8_t *source;
	uint8_t *target;
	uint32_t row;
	uint32_t column;

	/* Each pixel of the 64 x 64 surface. */
	for (row = 0U; row < GHCI_DC_CURSOR_SIZE; row++) {
		for (column = 0U; column < GHCI_DC_CURSOR_SIZE; column++) {
			source = &staging[row * CURSOR_STRIDE + column * GHCI_DISPLAY_PIXEL_BYTES];
			target = &surface[row * CURSOR_STRIDE + column * GHCI_DISPLAY_PIXEL_BYTES];

			/* Outside the image: transparent black. */
			if (row >= height || column >= width) {
				target[0] = 0U;
				target[1] = 0U;
				target[2] = 0U;
				target[3] = 0U;
				continue;
			}

			/* The pixel, in the controller's BGRA order. */
			target[1] = source[1];
			target[3] = source[3];
			if (swap) {
				target[0] = source[2];
				target[2] = source[0];
			} else {
				target[0] = source[0];
				target[2] = source[2];
			}
		}
	}
}

/*
 * Reports a coordinate of the cursor as CURSOR_POS takes it: 16 bits,
 * signed, a place past either end held at that end (the parts of the
 * image outside the screen are clipped).
 */
static uint32_t
cursor_coordinate(
	int32_t value)
{
	/* Held to the 16-bit range. */
	if (value < CURSOR_POSITION_MIN)
		value = CURSOR_POSITION_MIN;
	if (value > CURSOR_POSITION_MAX)
		value = CURSOR_POSITION_MAX;

	/* Succeeded: the two's complement in the low 16 bits. */
	return (uint32_t)value & 0xffffU;
}
