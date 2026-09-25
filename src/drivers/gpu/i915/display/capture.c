/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The capture display of the test build (I915_TEST_CAPTURE=y).
 *
 * The test build run under QEMU does not bring the panel up: no modeset,
 * no panel power or backlight, no link training and no scanout.  The display
 * stages are skipped as for a device whose firmware display check stopped
 * (display.c), and the node offers one virtual display instead.  Every
 * presentation is copied by the GPU into a physically contiguous area of
 * guest RAM that the QEMU host reads with the QMP command pmemsave: the iGPU
 * has no memory of its own, and guest RAM is the host's memfd.
 *
 * The display answers as the panel does: one output, connected, FIFO
 * presentation of shared (BLOB) or copied frames, one plane, BGRA or RGBA,
 * one lease at a time, with a fixed 1920x1080 mode at 60 Hz (294 x 165 mm).
 * A frame is copied unscaled to the top-left of a slot at its own extent.
 * A presentation is synchronous: when it returns, the GPU copy has
 * completed and the slot is described, so a wait never waits.  There is no
 * vblank and no pacing; each presentation is captured once, in order.
 *
 * The area (every field little-endian; offsets in bytes):
 *
 *   area + 0                              the global header (4 KiB)
 *   area + 4096 + k * slot_bytes          slot k, k = 0 .. slot_count - 1
 *   slot + 0                              the slot header (4 KiB)
 *   slot + pixel_offset (4096)            height rows of stride_bytes each
 *
 *   slot_bytes = 4096 + 1920 * 1080 * 4 rounded up to 4 KiB = 8298496
 *   total      = 4096 + 4 * slot_bytes                      = 33198080
 *
 *   the global header                     a slot header
 *   0x00 char[8] "I915CAP1"               0x00 char[8] "I915SLOT"
 *   0x08 u32 version (1)                  0x08 u64 sequence (1-based)
 *   0x0c u32 header_bytes (4096)          0x10 u32 width
 *   0x10 u32 slot_count (4)               0x14 u32 height
 *   0x14 u32 pixel_offset (4096)          0x18 u32 stride_bytes
 *   0x18 u64 slot_bytes                   0x1c u32 format (1 BGRA8888, 2 RGBA8888)
 *   0x20 u64 first_slot_offset (4096)     0x20 u32 display_id (1)
 *   0x28 u64 total_bytes                  0x24 u32 plane_index (0)
 *   0x30 u64 base (guest physical)        0x28 u64 generation (1)
 *   0x38 u32 max_width (1920)             0x30 u64 lease
 *   0x3c u32 max_height (1080)            0x38 u64 lease_sequence
 *   0x40 u64 write_count                  0x40 u64 present_time_ns
 *   0x48 u32 last_slot (~0 before any)    0x48 u32 hash_kind (0: not hashed)
 *   0x4c u32 reserved                     0x4c u32 slot_index
 *                                         0x50 u64 hash (0)
 *                                         0x58 u64 ready
 *
 * The format is the presented one: the bytes are copied as they were
 * presented (BGRA8888: B G R A in memory; RGBA8888: R G B A).
 *
 * The protocol: capture n (1-based) goes to slot (n - 1) mod 4.  Before the
 * GPU copies into the slot its ready word is set to 0; after the copy has
 * completed and the CPU's lines of the pixels are dropped, the slot's other
 * fields are written, then ready = sequence = n; then the global header's
 * last_slot, then write_count = n.  The stores keep that order (volatile
 * stores, and x86 orders stores to write-back memory), and both headers are
 * flushed afterwards.  A
 * host takes a slot whose ready equals its sequence and is not 0, and
 * checks ready again after reading the pixels: the slot is written again
 * only four captures later.  The slot headers alone are enough: the newest
 * capture is the slot with the largest ready.
 *
 * Coherency: the GPU writes the pixels through an uncached mapping
 * (PAT index 3, uncached MOCS), so they reach memory; the CPU never writes
 * the pixels (the area is zeroed and flushed once), and after each copy it
 * flushes the slot's pixel lines so no line a prefetch brought in stays
 * stale for a reader on this machine.
 *
 * XXX: one device; the area is kept by this file, not by the device.
 */

#ifdef I915_TEST_CAPTURE

#include "internal.h"
#include "capture.h"
#include "hotplug.h"
#include "present.h"
#include "scanout.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../memory.h"
#include "../ppgtt.h"
#include "../session.h"
#include "../render/blit.h"

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The one display of the node and its generation (the panel's ids). */
#define I915_CAPTURE_DISPLAY_ID		1U
#define I915_CAPTURE_GENERATION		1U

/* The largest frame the display accepts from a session (64 MiB, as the panel). */
#define I915_CAPTURE_MAX_FRAME_BYTES	(64ULL << 20)

/* The page the area is allocated and mapped in. */
#define I915_CAPTURE_PAGE_BYTES		4096U

/*
 * The capture area of the node and the address space that maps it.
 *
 * The run is allocated when the node's display is bound and kept until the
 * display stops.  The lease owner's address space maps it from the first
 * presentation of a lease to the end of that lease.
 */
struct i915_capture {
	/* The physically contiguous run and its CPU view through the direct map. */
	struct kern_pmem run;
	uint8_t *cpu;

	/* Nonzero once the run exists and its headers are written. */
	int allocated;

	/* The address space that maps the area, and where; NULL while none does. */
	struct i915_ppgtt *map_vm;
	uint64_t map_va;

	/* Completed captures; the next goes to slot write_count mod slot_count. */
	uint64_t write_count;
};

/*
 * The capture area of the one device.
 *
 * Made by the display binding on the serving thread before any session
 * exists; from then on its fields change only under the display's lease
 * mutex (the presentation, the release and a session's close), and the
 * display stop frees it.  Zero means no area.
 */
static struct i915_capture i915_capture_area;

static int i915_capture_allocate(void);
static int i915_capture_query(void *device, void *session, struct gpu_display_info *request);
static int i915_capture_mode(void *device, void *session, struct gpu_display_mode *request);
static int i915_capture_claim(void *device, void *session, struct gpu_display_claim *request);
static int i915_capture_release(void *device, void *session, const struct gpu_display_release *request);
static int i915_capture_present(void *device, void *session, void *object, struct gpu_display_present *request);
static int i915_capture_copy(struct i915_device *device, void *session, void *object, const struct gpu_display_present *request, unsigned slot, uint32_t stride_bytes);
static int i915_capture_map(struct i915_device *device, struct i915_ppgtt *vm);
static void i915_capture_unmap(struct i915_device *device);
static void i915_capture_end_lease(struct i915_device *device);
static volatile struct i915_capture_global *i915_capture_global_header(uint8_t *area);
static volatile struct i915_capture_slot *i915_capture_slot_header(uint8_t *area, unsigned slot);

/*
 * The display operations of the capture build.
 *
 * Query, mode, claim, release and present are the capture display's own;
 * the wait and the event sequence are the panel's, which read only the
 * lease and never change.  The table never changes.
 */
static const struct drv_gpu_display_ops i915_capture_ops = {
	i915_capture_query,
	i915_capture_mode,
	i915_capture_claim,
	i915_capture_release,
	i915_capture_present,
	drv_i915_present_display_wait,
	drv_i915_hpd_events
};

/*
 * Makes the capture area and binds the capture display in place of the panel.
 *
 * The scanout operations are the panel's (shared or copied frames, BGRA or
 * RGBA).  Without the area the node has no display.
 */
void
drv_i915_capture_bind_ops(
	struct i915_device *device,
	struct drv_gpu_ops *ops)
{
	int error;

	/* A device without a display offers none. */
	if (device->display == NULL)
		return;

	/* Makes the area once; its address is logged for the host. */
	error = i915_capture_allocate();
	if (error != 0) {
		kern_logf("i915: capture: XXX the capture area (%llu bytes) could not be allocated: %d; the node has no display\n",
		    (unsigned long long)I915_CAPTURE_AREA_BYTES,
		    error);
		return;
	}

	/* The capture display and the display-only pairing, with their capabilities. */
	ops->display = &i915_capture_ops;
	ops->scanout = &drv_i915_scanout_ops;
	ops->capabilities |= GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS;
}

/*
 * Ends the lease a closing session still holds.
 *
 * The area's mapping in the session's address space goes before the space
 * does.
 */
void
drv_i915_capture_session_close(
	struct i915_device *device,
	void *session)
{
	struct i915_resident_display *rd;

	/* A device without a display has no lease. */
	if (device->display == NULL)
		return;

	rd = &device->display->rd;

	/* A lease that was never prepared was never held. */
	if (!rd->inited)
		return;

	/* Ends the session's lease under the lease mutex. */
	mutex_lock(&rd->mutex);

	if (rd->owner == session) {
		kern_logf("i915: capture: the lease owner closed without releasing it\n");
		i915_capture_end_lease(device);
	}

	mutex_unlock(&rd->mutex);
}

/*
 * Frees the capture area once nothing maps it.
 *
 * An area an address space still maps is kept: the GPU could still name it.
 */
void
drv_i915_capture_fini(
	struct i915_device *device)
{
	UNUSED_PARAMETER(device);

	/* No area was made. */
	if (!i915_capture_area.allocated)
		return;

	/* A mapped area stays. */
	if (i915_capture_area.map_vm != NULL) {
		kern_logf("i915: capture: XXX the capture area is still mapped at stop; it is kept\n");
		return;
	}

	/* Gives the run back and forgets the area. */
	(void)kern_pmem_free(&i915_capture_area.run);
	kern_memset(&i915_capture_area, 0, sizeof(i915_capture_area));
	kern_logf("i915: capture: area freed\n");
}

/*
 * Reports the offset of a slot from the start of the area.
 */
uint64_t
drv_i915_capture_slot_offset(
	unsigned slot)
{
	uint64_t offset;

	/* The global header, then the slots one after another. */
	offset = (uint64_t)I915_CAPTURE_HEADER_BYTES + (uint64_t)slot * I915_CAPTURE_SLOT_BYTES;

	/* Reports the offset. */
	return offset;
}

/*
 * Reports the slot the next capture goes to after write_count completed
 * ones.
 */
unsigned
drv_i915_capture_next_slot(
	uint64_t write_count)
{
	unsigned slot;

	/* The slots are used in turn, from slot 0. */
	slot = (unsigned)(write_count % I915_CAPTURE_SLOTS);

	/* Reports the slot. */
	return slot;
}

/*
 * Writes the global header and every slot's magic into a zeroed area.
 *
 * base is the area's guest physical address; no capture is recorded yet.
 * The caller flushes the area.
 */
void
drv_i915_capture_init_header(
	uint8_t *area,
	uint64_t base)
{
	struct i915_capture_global *global;
	struct i915_capture_slot *header;
	unsigned slot;

	global = (struct i915_capture_global *)(void *)area;

	/* The magic, the version and the layout of the slots. */
	kern_memcpy(global->magic, "I915CAP1", sizeof(global->magic));
	global->version = I915_CAPTURE_VERSION;
	global->header_bytes = I915_CAPTURE_HEADER_BYTES;
	global->slot_count = I915_CAPTURE_SLOTS;
	global->pixel_offset = I915_CAPTURE_PIXEL_OFFSET;
	global->slot_bytes = I915_CAPTURE_SLOT_BYTES;
	global->first_slot_offset = I915_CAPTURE_HEADER_BYTES;
	global->total_bytes = I915_CAPTURE_AREA_BYTES;

	/* Where the area is, and the largest frame a slot holds. */
	global->base = base;
	global->max_width = I915_CAPTURE_WIDTH;
	global->max_height = I915_CAPTURE_HEIGHT;

	/* No capture yet: write_count 0 and no newest slot. */
	global->write_count = 0U;
	global->last_slot = I915_CAPTURE_NO_SLOT;
	global->reserved = 0U;

	/* Names every slot; each stays not ready until its first capture. */
	for (slot = 0U; slot < I915_CAPTURE_SLOTS; slot++) {
		header = (struct i915_capture_slot *)(void *)(area + drv_i915_capture_slot_offset(slot));
		kern_memcpy(header->magic, "I915SLOT", sizeof(header->magic));
		header->slot_index = slot;
		header->ready = 0U;
	}
}

/*
 * Marks a slot as being written before the GPU copies into it.
 *
 * ready 0 tells the host that the slot's pixels are not a complete frame.
 * The caller fences the store before the copy starts.
 */
void
drv_i915_capture_slot_open(
	uint8_t *area,
	unsigned slot)
{
	volatile struct i915_capture_slot *header;

	header = i915_capture_slot_header(area, slot);

	/* The slot holds no complete frame from here on. */
	header->ready = 0U;
}

/*
 * Describes a completed frame in its slot and publishes it.
 *
 * The fields first, then ready = sequence, then the global header's
 * last_slot and write_count.  The stores are volatile, so the compiler keeps
 * their order, and x86 makes stores to write-back memory visible to another
 * processor in program order, so a reader that sees ready sees every field
 * (and the frame, which was complete before the first store).  The caller
 * flushes the headers afterwards.
 */
void
drv_i915_capture_slot_close(
	uint8_t *area,
	unsigned slot,
	const struct i915_capture_frame *frame)
{
	volatile struct i915_capture_global *global;
	volatile struct i915_capture_slot *header;

	global = i915_capture_global_header(area);
	header = i915_capture_slot_header(area, slot);

	/* The frame's number, extent, row bytes and format. */
	header->sequence = frame->sequence;
	header->width = frame->width;
	header->height = frame->height;
	header->stride_bytes = frame->stride_bytes;
	header->format = frame->format;

	/* The display, the plane and the lease it was presented through. */
	header->display_id = I915_CAPTURE_DISPLAY_ID;
	header->plane_index = 0U;
	header->generation = I915_CAPTURE_GENERATION;
	header->lease = frame->lease;
	header->lease_sequence = frame->lease_sequence;
	header->present_time_ns = frame->present_time_ns;

	/* The pixels are not hashed here: the host hashes what it reads. */
	header->hash_kind = I915_CAPTURE_HASH_NONE;
	header->slot_index = slot;
	header->hash = 0U;

	/* ready = sequence, after every field: the slot holds a complete frame. */
	header->ready = frame->sequence;

	/* The newest slot first, then the count that names it. */
	global->last_slot = slot;
	global->write_count = frame->sequence;
}

/*
 * Checks a mode against the virtual display (the mode validation).
 *
 * Any extent up to 1920x1080 is accepted and captured unscaled; the refresh
 * is the display's 60 Hz, and a zero refresh is given it.  Returns 0, or
 * EINVAL for a larger extent or another refresh.
 */
int
drv_i915_capture_check_mode(
	uint32_t width,
	uint32_t height,
	uint32_t *refresh_millihz)
{
	/* A frame larger than a slot is refused. */
	if (width > I915_CAPTURE_WIDTH)
		return EINVAL;
	if (height > I915_CAPTURE_HEIGHT)
		return EINVAL;

	/* The display's refresh is the only one; 0 asks for it. */
	if (*refresh_millihz == 0U)
		*refresh_millihz = I915_CAPTURE_REFRESH_MILLIHZ;

	if (*refresh_millihz != I915_CAPTURE_REFRESH_MILLIHZ)
		return EINVAL;

	/* Succeeded: the mode is captured unscaled. */
	return 0;
}

/*
 * Checks a presented frame against a slot and gives its row bytes there.
 *
 * Returns 0 with the rows' bytes rounded up to 64, or EINVAL for an empty
 * frame or one larger than 1920x1080.
 */
int
drv_i915_capture_check_frame(
	uint32_t width,
	uint32_t height,
	uint32_t *stride_bytes)
{
	/* An empty frame has nothing to capture. */
	if (width == 0U || height == 0U)
		return EINVAL;

	/* A frame larger than a slot is refused. */
	if (width > I915_CAPTURE_WIDTH)
		return EINVAL;
	if (height > I915_CAPTURE_HEIGHT)
		return EINVAL;

	/* Four bytes a pixel, each row on a 64-byte boundary. */
	*stride_bytes = (width * 4U + I915_CAPTURE_STRIDE_ALIGN - 1U) & ~(I915_CAPTURE_STRIDE_ALIGN - 1U);

	/* Succeeded: the frame fits a slot. */
	return 0;
}

/* Makes the capture area once: the contiguous run, zeroed and flushed, with its headers; logs its address. */
static int
i915_capture_allocate(void)
{
	struct i915_capture *capture;
	int error;

	capture = &i915_capture_area;

	/* The area exists already. */
	if (capture->allocated)
		return 0;

	/* Allocates one physically contiguous run the GPU's address bits reach. */
	error = kern_pmem_alloc_limited((size_t)I915_CAPTURE_AREA_BYTES, I915_CAPTURE_PAGE_BYTES, I915_DMA_MAX_ADDRESS, 0U, &capture->run);
	if (error != 0)
		return error;

	/* Takes the CPU view from the direct map; a run outside managed RAM cannot be used. */
	capture->cpu = kern_pmem_to_kernel(capture->run.paddr);
	if (capture->cpu == NULL) {
		(void)kern_pmem_free(&capture->run);
		kern_memset(capture, 0, sizeof(*capture));
		return EFAULT;
	}

	/*
	 * Zeroes the run, writes the headers and flushes the whole area: from
	 * here on the CPU holds no dirty line of it, and only the headers are
	 * ever written by the CPU again.
	 */
	kern_memset(capture->cpu, 0, (size_t)I915_CAPTURE_AREA_BYTES);
	drv_i915_capture_init_header(capture->cpu, (uint64_t)capture->run.paddr);
	drv_i915_gt_clflush(capture->cpu, (size_t)I915_CAPTURE_AREA_BYTES);

	/* No capture yet, and no address space maps the area. */
	capture->write_count = 0U;
	capture->map_vm = NULL;
	capture->map_va = 0U;
	capture->allocated = 1;

	/* The line the host harness reads the area's address from. */
	kern_logf("i915: capture: base=0x%llx bytes=%llu slots=%u slot_bytes=%llu pixel_offset=%u\n",
	    (unsigned long long)capture->run.paddr,
	    (unsigned long long)I915_CAPTURE_AREA_BYTES,
	    I915_CAPTURE_SLOTS,
	    (unsigned long long)I915_CAPTURE_SLOT_BYTES,
	    I915_CAPTURE_PIXEL_OFFSET);

	/* Succeeded: the area can be mapped and written. */
	return 0;
}

/*
 * Describes the node's one display (the display query operation): the
 * virtual display, connected, FIFO presentation of shared (BLOB) frames,
 * one plane, BGRA or RGBA, the fixed mode and physical size.
 */
static int
i915_capture_query(
	void *device,
	void *session,
	struct gpu_display_info *request)
{
	struct i915_device *owner_device;
	struct i915_display *display;

	UNUSED_PARAMETER(session);

	owner_device = device;
	display = owner_device->display;

	/* One display exists. */
	request->count = 1U;

	/* Only the count was asked for. */
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;

	/* Only display 0 exists. */
	if (request->index != 0U)
		return EINVAL;

	/* The display and its state: active while a lease has frames on it. */
	request->display_id = I915_CAPTURE_DISPLAY_ID;
	request->generation = I915_CAPTURE_GENERATION;
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB;
	if (display->rd.active)
		request->flags |= GPU_DISPLAY_ACTIVE;

	/* One plane, the two packed formats, and the largest source the panel takes. */
	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = I915_CAPTURE_MAX_FRAME_BYTES;

	/* The fixed mode is the current, preferred and largest one. */
	request->current_width = I915_CAPTURE_WIDTH;
	request->current_height = I915_CAPTURE_HEIGHT;
	request->preferred_width = I915_CAPTURE_WIDTH;
	request->preferred_height = I915_CAPTURE_HEIGHT;
	request->max_width = I915_CAPTURE_WIDTH;
	request->max_height = I915_CAPTURE_HEIGHT;
	request->refresh_millihz = I915_CAPTURE_REFRESH_MILLIHZ;

	/* The physical size of a 13.3-inch 16:9 panel. */
	request->physical_width_mm = I915_CAPTURE_WIDTH_MM;
	request->physical_height_mm = I915_CAPTURE_HEIGHT_MM;

	kern_memcpy(request->name, "i915 capture", sizeof("i915 capture"));

	/* Succeeded: the display is described. */
	return 0;
}

/*
 * Enumerates or checks a mode of the node's display (the display mode
 * operation): the one fixed mode, and any smaller extent at its refresh.
 */
static int
i915_capture_mode(
	void *device,
	void *session,
	struct gpu_display_mode *request)
{
	uint32_t refresh;
	int error;

	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(session);

	/* Only the one display of this generation exists. */
	if (request->display_id != I915_CAPTURE_DISPLAY_ID)
		return ENOENT;
	if (request->generation != I915_CAPTURE_GENERATION)
		return ESTALE;

	/* Enumeration: the one fixed mode. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = 1U;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index != 0U)
			return EINVAL;

		request->width = I915_CAPTURE_WIDTH;
		request->height = I915_CAPTURE_HEIGHT;
		request->refresh_millihz = I915_CAPTURE_REFRESH_MILLIHZ;
		return 0;
	}

	/* Validation: an extent that fits, at the display's refresh. */
	refresh = request->refresh_millihz;
	error = drv_i915_capture_check_mode(request->width, request->height, &refresh);
	if (error != 0) {
		kern_logf("i915: capture: mode %ux%u@%u mHz refused: up to %ux%u at %u mHz only\n",
		    request->width,
		    request->height,
		    request->refresh_millihz,
		    I915_CAPTURE_WIDTH,
		    I915_CAPTURE_HEIGHT,
		    I915_CAPTURE_REFRESH_MILLIHZ);
		return error;
	}

	request->refresh_millihz = refresh;

	/* Succeeded: the mode is captured unscaled. */
	return 0;
}

/*
 * Gives the display's lease to a session (the display claim operation).
 *
 * One lease at a time.  Returns 0 with the lease, ENOENT or ESTALE for
 * another display, EINVAL for a plane other than 0, or EBUSY while another
 * session holds it.
 */
static int
i915_capture_claim(
	void *device,
	void *session,
	struct gpu_display_claim *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;

	owner_device = device;
	rd = &owner_device->display->rd;

	/* Only plane 0 of the one display of this generation exists. */
	if (request->display_id != I915_CAPTURE_DISPLAY_ID)
		return ENOENT;
	if (request->generation != I915_CAPTURE_GENERATION)
		return ESTALE;
	if (request->plane_index != 0U)
		return EINVAL;

	/* Takes the lease under its mutex. */
	drv_i915_present_lease_init(owner_device->display);
	mutex_lock(&rd->mutex);

	/* Another session holds it. */
	if (rd->owner != NULL) {
		mutex_unlock(&rd->mutex);
		return EBUSY;
	}

	/* The session holds a new lease with no presentation yet. */
	rd->owner = session;
	rd->lease = rd->next_lease;
	rd->next_lease++;
	rd->sequence = 0U;
	request->lease = rd->lease;

	mutex_unlock(&rd->mutex);

	kern_logf("i915: capture: lease %llu claimed\n", (unsigned long long)request->lease);

	/* Succeeded: the session holds the display. */
	return 0;
}

/*
 * Ends the lease of its holder (the display release operation).
 *
 * Returns 0, or EINVAL for a session that does not hold the lease.
 */
static int
i915_capture_release(
	void *device,
	void *session,
	const struct gpu_display_release *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;

	owner_device = device;
	rd = &owner_device->display->rd;

	/* Ends the lease under its mutex. */
	drv_i915_present_lease_init(owner_device->display);
	mutex_lock(&rd->mutex);

	/* Only the lease holder releases it. */
	if (rd->owner != session || rd->lease != request->lease) {
		mutex_unlock(&rd->mutex);
		return EINVAL;
	}

	i915_capture_end_lease(owner_device);

	mutex_unlock(&rd->mutex);

	/* Succeeded: the lease is free. */
	return 0;
}

/*
 * Captures a frame of the lease holder (the display present operation).
 *
 * The GPU copies the frame into the next slot and the slot is described.
 * Returns 0 with the completed sequence in the request, EINVAL for a
 * session that does not hold the lease, a frame larger than a slot or one
 * the GPU cannot address, ESTALE for another generation, or the copy's
 * error.
 */
static int
i915_capture_present(
	void *device,
	void *session,
	void *object,
	struct gpu_display_present *request)
{
	struct i915_device *owner_device;
	struct i915_display *display;
	struct i915_resident_display *rd;
	struct i915_capture_frame frame;
	uint8_t *slot_header;
	uint32_t stride_bytes;
	unsigned slot;
	int error;

	owner_device = device;
	display = owner_device->display;
	rd = &display->rd;

	/* Checks the lease and the frame, and captures it, under the lease mutex. */
	drv_i915_present_lease_init(display);
	mutex_lock(&rd->mutex);

	/* Only the lease holder presents. */
	if (rd->owner != session || rd->lease != request->lease) {
		mutex_unlock(&rd->mutex);
		return EINVAL;
	}

	/* Only the one generation of the display exists. */
	if (request->generation != I915_CAPTURE_GENERATION) {
		mutex_unlock(&rd->mutex);
		return ESTALE;
	}

	/* The frame must fit a slot. */
	error = drv_i915_capture_check_frame(request->width, request->height, &stride_bytes);
	if (error != 0) {
		mutex_unlock(&rd->mutex);
		return error;
	}

	/*
	 * The next slot holds no complete frame until the copy is described;
	 * the flush fences the cleared ready word before the copy starts.
	 */
	slot = drv_i915_capture_next_slot(i915_capture_area.write_count);
	slot_header = i915_capture_area.cpu + drv_i915_capture_slot_offset(slot);
	drv_i915_capture_slot_open(i915_capture_area.cpu, slot);
	drv_i915_gt_clflush(slot_header, sizeof(struct i915_capture_slot));

	/* The GPU copies the frame into the slot and has completed when this returns. */
	error = i915_capture_copy(owner_device, session, object, request, slot, stride_bytes);
	if (error != 0) {
		mutex_unlock(&rd->mutex);
		kern_logf("i915: capture: the GPU copy of a %ux%u frame into slot %u failed: %d\n",
		    request->width,
		    request->height,
		    slot,
		    error);
		return error;
	}

	/* A completed presentation: the display shows this lease's frames, and the sequence advances. */
	rd->active = 1;
	rd->sequence++;
	rd->present_tick = sched_ticks();
	request->sequence = rd->sequence;

	/* The capture's number, counted over every lease. */
	i915_capture_area.write_count++;

	/* Describes the frame in its slot and publishes it for the host. */
	kern_memset(&frame, 0, sizeof(frame));
	frame.sequence = i915_capture_area.write_count;
	frame.width = request->width;
	frame.height = request->height;
	frame.stride_bytes = stride_bytes;
	frame.format = request->format;
	frame.lease = rd->lease;
	frame.lease_sequence = rd->sequence;
	frame.present_time_ns = rd->present_tick * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ);
	drv_i915_capture_slot_close(i915_capture_area.cpu, slot, &frame);

	/* Writes both headers back to memory, fenced, for a reader outside the caches' view. */
	drv_i915_gt_clflush(slot_header, sizeof(struct i915_capture_slot));
	drv_i915_gt_clflush(i915_capture_area.cpu, sizeof(struct i915_capture_global));

	mutex_unlock(&rd->mutex);

	/* One line for every captured frame. */
	kern_logf("i915: capture: frame=%llu slot=%u %ux%u format=%u\n",
	    (unsigned long long)frame.sequence,
	    slot,
	    frame.width,
	    frame.height,
	    frame.format);

	/* Succeeded: the frame is in its slot. */
	return 0;
}

/*
 * Copies a presented frame into a slot on the GPU and drops the CPU's lines
 * of the slot's pixels.
 *
 * The copy is one rectangle of the session's executor, unscaled, to the
 * top-left of the slot, in the session's render context, run to its end.
 */
static int
i915_capture_copy(
	struct i915_device *device,
	void *session,
	void *object,
	const struct gpu_display_present *request,
	unsigned slot,
	uint32_t stride_bytes)
{
	struct i915_session *owner;
	struct i915_gem_object *storage;
	struct i915_gfx_surface src;
	struct i915_gfx_surface dst;
	struct i915_gfx_rect rect;
	uint64_t pixels;
	int error;

	owner = session;
	storage = object;

	/* A session without an executor, or a frame not in its address space, cannot be copied. */
	if (owner->vk == NULL)
		return EINVAL;
	if (storage->va == 0U)
		return EINVAL;

	/* Makes the area addressable in the session's space. */
	error = i915_capture_map(device, owner->vm);
	if (error != 0)
		return error;

	/* The frame as the GPU sees it, in the order it was presented. */
	src.va = storage->va + request->offset;
	src.width = request->width;
	src.height = request->height;
	src.pitch = request->stride;
	src.format = VK_FORMAT_R8G8B8A8_UNORM;
	if (request->format == GPU_PIXEL_BGRA8888)
		src.format = VK_FORMAT_B8G8R8A8_UNORM;

	/* The slot's pixels: the same extent and format, so the bytes are copied as they are. */
	pixels = drv_i915_capture_slot_offset(slot) + I915_CAPTURE_PIXEL_OFFSET;
	dst.va = i915_capture_area.map_va + pixels;
	dst.width = request->width;
	dst.height = request->height;
	dst.pitch = stride_bytes;
	dst.format = src.format;

	/* The whole frame, unscaled, at the top-left. */
	rect.x = 0;
	rect.y = 0;
	rect.w = request->width;
	rect.h = request->height;

	/* Copies it and waits until the copy has run to its end. */
	error = drv_i915_gfx_rect(owner->vk, &dst, &rect, &src, &rect, NULL, 0);
	if (error != 0)
		return error;

	/*
	 * Drops any line of the pixels a prefetch may have brought into the
	 * CPU's caches before the GPU's uncached writes reached memory.
	 */
	drv_i915_gt_clflush(i915_capture_area.cpu + pixels, (size_t)stride_bytes * request->height);

	/* Succeeded: the slot holds the frame. */
	return 0;
}

/*
 * Maps the whole area uncached into a presenting session's address space.
 *
 * One address space holds it at a time, from the first presentation of a
 * lease to its end: the same one again is a no-op, another is refused with
 * EBUSY.  Returns 0, or the address space's error.
 */
static int
i915_capture_map(
	struct i915_device *device,
	struct i915_ppgtt *vm)
{
	struct i915_capture *capture;
	uint64_t va;
	unsigned pages;
	int error;

	capture = &i915_capture_area;
	pages = (unsigned)(I915_CAPTURE_AREA_BYTES / I915_CAPTURE_PAGE_BYTES);

	/* The area is already in this address space. */
	if (capture->map_vm == vm)
		return 0;

	/* One presenting address space at a time. */
	if (capture->map_vm != NULL)
		return EBUSY;

	/* Maps the run under the device mutex, which guards the address spaces. */
	mutex_lock(&device->mutex);

	/* Reserves a range as large as the area; ranges are never reused. */
	error = drv_i915_ppgtt_va_alloc(vm, I915_CAPTURE_AREA_BYTES, &va);
	if (error != 0) {
		mutex_unlock(&device->mutex);
		return error;
	}

	/* Maps every page uncached, so the GPU's writes reach memory. */
	error = drv_i915_ppgtt_insert_uncached(vm, va, (uint64_t)capture->run.paddr, pages);
	if (error != 0) {
		drv_i915_ppgtt_clear(vm, va, pages);
		mutex_unlock(&device->mutex);
		return error;
	}

	/* The address space holds the area from here on. */
	capture->map_vm = vm;
	capture->map_va = va;

	mutex_unlock(&device->mutex);

	kern_logf("i915: capture: area mapped for the GPU at 0x%llx (%u pages, uncached)\n",
	    (unsigned long long)va,
	    pages);

	/* Succeeded: the GPU can write every slot. */
	return 0;
}

/*
 * Unmaps the area from the address space that holds it.
 *
 * XXX: the session address space never reuses a range, so the range is only
 * cleared, not given back, and no TLB invalidation is needed for a range
 * nothing names.
 */
static void
i915_capture_unmap(
	struct i915_device *device)
{
	struct i915_capture *capture;
	unsigned pages;

	capture = &i915_capture_area;
	pages = (unsigned)(I915_CAPTURE_AREA_BYTES / I915_CAPTURE_PAGE_BYTES);

	/* No address space holds the area. */
	if (capture->map_vm == NULL)
		return;

	/* Points the range back at scratch under the device mutex. */
	mutex_lock(&device->mutex);

	drv_i915_ppgtt_clear(capture->map_vm, capture->map_va, pages);

	mutex_unlock(&device->mutex);

	/* No address space holds the area from here on. */
	capture->map_vm = NULL;
	capture->map_va = 0U;
}

/*
 * Ends the current lease; the area's mapping goes first.  The caller holds
 * the lease mutex.
 */
static void
i915_capture_end_lease(
	struct i915_device *device)
{
	struct i915_resident_display *rd;

	rd = &device->display->rd;

	/* The GPU no longer names the area through the owner's space. */
	i915_capture_unmap(device);

	/* The display shows no frame of a lease any more. */
	rd->active = 0;

	kern_logf("i915: capture: lease %llu released after %llu frame(s) (%llu captured in all)\n",
	    (unsigned long long)rd->lease,
	    (unsigned long long)rd->sequence,
	    (unsigned long long)i915_capture_area.write_count);

	/* The display is free. */
	rd->owner = NULL;
	rd->lease = 0U;
}

/* Returns the global header of an area, for the stores the host observes. */
static volatile struct i915_capture_global *
i915_capture_global_header(
	uint8_t *area)
{
	volatile struct i915_capture_global *global;

	/* The global header is at the start of the area. */
	global = (volatile struct i915_capture_global *)(volatile void *)area;

	/* Reports the header. */
	return global;
}

/* Returns the header of one slot of an area, for the stores the host observes. */
static volatile struct i915_capture_slot *
i915_capture_slot_header(
	uint8_t *area,
	unsigned slot)
{
	volatile struct i915_capture_slot *header;

	/* A slot's header is at the start of the slot. */
	header = (volatile struct i915_capture_slot *)(volatile void *)(area + drv_i915_capture_slot_offset(slot));

	/* Reports the header. */
	return header;
}

#endif /* I915_TEST_CAPTURE */
