/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Scanout buffers and the panel buffers of the resident node.
 *
 * A scanout buffer is a framebuffer the display engine reads.  Its layout
 * and placement follow the reference's rules for the one layout supported
 * (XRGB8888, linear):
 *
 *   pitch alignment 64        intel_fb_stride_alignment(): a linear surface
 *                             whose pitch is within the plane's maximum
 *                             stride uses 64 bytes (i915_gem_dumb_create()
 *                             rounds width * cpp up to 64 as well)
 *   stride units   pitch/64   skl_plane_stride(): a linear surface expresses
 *                             its stride in 64-byte chunks
 *   alignment      256 KiB    intel_surf_alignment() ->
 *                             intel_linear_alignment(), display version 9+
 *   guard          168 PTEs   i915_gem_object_pin_to_display_plane():
 *                             VTD_GUARD of scratch on both sides when VT-d is
 *                             active.  A guest cannot see whether the host
 *                             translates its DMA, so the guard is applied
 *                             always (a superset of the reference)
 *   cache          clflush    __i915_gem_object_flush_for_display(): dirty
 *                             CPU cache lines are flushed before the display
 *                             reads the pages
 *
 * The resident node keeps two full-panel buffers while the panel is up.
 * The present path maps both, page by page and uncached (the display does
 * not snoop the LLC), into the address space of the session presenting, so
 * the GPU can copy a frame into the one not on the panel.
 */

#include "internal.h"
#include "scanout.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../ggtt.h"
#include "../memory.h"
#include "../ppgtt.h"

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-scanout.h>
#include <kern/klog.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The stride alignment of a linear surface. */
#define I915_SCANOUT_STRIDE_ALIGN	64U

/* The surface alignment of a linear surface. */
#define I915_SCANOUT_SURF_ALIGN		(256U * 1024U)

/* The scratch pages on each side of a display binding. */
#define I915_SCANOUT_VTD_GUARD		168U

/* skl_plane_max_stride(), display version 13: 128 KiB. */
#define I915_SCANOUT_MAX_STRIDE		131072U

/* The largest buffer the layout accepts. */
#define I915_SCANOUT_MAX_WIDTH		8192U
#define I915_SCANOUT_MAX_HEIGHT		4096U

/* The one display of the resident node and its generation. */
#define I915_SCANOUT_DISPLAY_ID		1U
#define I915_SCANOUT_GENERATION		1U

/* The alignments the display asks of a foreign linear render target or sampler surface. */
#define I915_SCANOUT_IMPORT_ALIGN	64U

static int i915_scanout_device_query(void *device, void *session, struct gpu_device_info *request);
static int i915_scanout_constraints(void *device, void *session, struct gpu_scanout_constraints *request);

/*
 * The display-only pairing operations of the resident node.
 *
 * The node is its own render device and display; a foreign import is not
 * offered (the core refuses it without import_image).  The table never
 * changes.
 */
const struct drv_gpu_scanout_ops drv_i915_scanout_ops = {
	i915_scanout_device_query,
	i915_scanout_constraints,
	NULL
};

/*
 * Creates a scanout buffer: its backing and CPU view.
 *
 * Returns 0, EINVAL for a layout other than linear XRGB8888 within the
 * plane's limits, ENOMEM when the backing cannot be allocated, or EBUSY
 * when the storage still holds a buffer (it is left exactly as it was).
 * The storage must start zeroed; destroy returns it to that state, and
 * nothing stays allocated on failure.
 */
int
drv_i915_scanout_create(
	struct i915_gt_mem *gm,
	uint32_t width,
	uint32_t height,
	uint32_t format,
	uint64_t modifier,
	struct i915_scanout *so)
{
	/* Refuses a call without memory or storage. */
	if (gm == NULL || so == NULL)
		return EINVAL;

	/* A record in any other state owns backing, GGTT or a pin: it is never overwritten. */
	if (so->state != I915_SCANOUT_NONE || so->obj != NULL)
		return EBUSY;

	kern_memset(so, 0, sizeof(*so));

	/* Refuses, before any allocation, every layout but the one supported. */
	if (format != I915_FOURCC_XRGB8888 || modifier != I915_MOD_LINEAR)
		return EINVAL;
	if (width == 0U || height == 0U)
		return EINVAL;
	if (width > I915_SCANOUT_MAX_WIDTH || height > I915_SCANOUT_MAX_HEIGHT)
		return EINVAL;

	/* Lays the buffer out: 4 bytes a pixel, the pitch rounded up to the stride alignment. */
	so->gm = gm;
	so->width = width;
	so->height = height;
	so->format = format;
	so->modifier = modifier;
	so->cpp = 4U;
	so->pitch = (width * so->cpp + I915_SCANOUT_STRIDE_ALIGN - 1U) & ~(I915_SCANOUT_STRIDE_ALIGN - 1U);
	if (so->pitch > I915_SCANOUT_MAX_STRIDE)
		return EINVAL;

	/* The plane's stride encoding, the size and the placement requirements. */
	so->stride_units = so->pitch / I915_SCANOUT_STRIDE_ALIGN;
	so->size = so->pitch * height;
	so->alignment = I915_SCANOUT_SURF_ALIGN;
	so->guard_pages = I915_SCANOUT_VTD_GUARD;

	/* Allocates the backing; a failure leaves the storage empty. */
	so->obj = drv_i915_gt_object_create(gm, so->size);
	if (so->obj == NULL) {
		kern_memset(so, 0, sizeof(*so));
		return ENOMEM;
	}

	/* ALLOCATED: backing and CPU view exist, nothing is bound yet. */
	so->cpu = (uint32_t *)so->obj->cpu;
	so->state = I915_SCANOUT_ALLOCATED;

	/* Succeeded: the buffer can be drawn into and pinned. */
	return 0;
}

/*
 * Pins a scanout buffer for the display.
 *
 * Reserves the GGTT range (alignment and guards), writes the PTEs and
 * flushes the CPU cache for the display.  Returns 0, EINVAL for a buffer
 * that is not allocated, EBUSY for one already pinned, ERANGE when the
 * address does not fit the plane's surface register, or the display
 * binding's error.
 */
int
drv_i915_scanout_pin(
	struct i915_scanout *so,
	const char *owner)
{
	int error;

	/* Only an allocated buffer can be pinned; a pinned one says so. */
	if (so == NULL)
		return EINVAL;
	if (so->state != I915_SCANOUT_ALLOCATED) {
		if (so->state >= I915_SCANOUT_PINNED)
			return EBUSY;

		return EINVAL;
	}

	/* Binds the buffer into the display window with its alignment and guards. */
	error = drv_i915_gt_display_bind(so->gm, so->obj, so->alignment / I915_GT_PAGE_BYTES, so->guard_pages);
	if (error != 0)
		return error;

	/* The plane's surface register holds a 32-bit, aligned GGTT offset. */
	if ((so->obj->ggtt_offset >> 32) != 0U || (so->obj->ggtt_offset & (so->alignment - 1U)) != 0U) {
		drv_i915_gt_display_unbind(so->gm, so->obj);
		return ERANGE;
	}

	/* PINNED: the surface address is valid and named by its owner. */
	so->surf = so->obj->ggtt_offset;
	so->pin_owner = owner;
	so->state = I915_SCANOUT_PINNED;

	/* Makes the pixels drawn so far visible to the display. */
	drv_i915_scanout_publish(so);

	/* Succeeded: the buffer can be handed to a plane. */
	return 0;
}

/*
 * Makes pixels the CPU changed visible to the display engine
 * (i915_gem_object_flush_if_display()).
 */
void
drv_i915_scanout_publish(
	struct i915_scanout *so)
{
	/* A buffer without backing or CPU view has nothing to flush. */
	if (so == NULL || so->state < I915_SCANOUT_ALLOCATED)
		return;
	if (so->cpu == NULL)
		return;

	/* Flushes the CPU's cache lines of the whole buffer. */
	drv_i915_gt_clflush(so->cpu, so->size);
	so->publishes++;
}

/*
 * Records that a display starts reading the buffer.
 *
 * A second display may read the same buffer: the users are counted and the
 * state is their union.  Returns 0, or EINVAL for a buffer that is neither
 * pinned nor in use.
 */
int
drv_i915_scanout_begin(
	struct i915_scanout *so)
{
	/* Only a pinned buffer, or one another display reads, can be read. */
	if (so == NULL)
		return EINVAL;
	if (so->state != I915_SCANOUT_PINNED && so->state != I915_SCANOUT_IN_USE)
		return EINVAL;

	/* IN_USE while any display reads it. */
	so->users++;
	so->state = I915_SCANOUT_IN_USE;

	/* Succeeded: the display may read the buffer. */
	return 0;
}

/*
 * Records that a display has provably stopped reading the buffer.
 *
 * The buffer goes back to PINNED when the last display has let go of it.
 */
void
drv_i915_scanout_end(
	struct i915_scanout *so)
{
	/* Only a buffer in use has users to count down. */
	if (so == NULL || so->state != I915_SCANOUT_IN_USE)
		return;
	if (so->users == 0U)
		return;

	/* The last user gives the buffer back. */
	so->users--;
	if (so->users == 0U)
		so->state = I915_SCANOUT_PINNED;
}

/*
 * Unpins a scanout buffer.
 *
 * Returns 0, EBUSY while a display reads it or after it was abandoned, or
 * EINVAL for a buffer that is not pinned.
 */
int
drv_i915_scanout_unpin(
	struct i915_scanout *so)
{
	/* Refuses a missing buffer. */
	if (so == NULL)
		return EINVAL;

	/* A buffer a display reads, or may still read, keeps its pin. */
	if (so->state == I915_SCANOUT_IN_USE || so->state == I915_SCANOUT_ABANDONED) {
		so->refused_unpin++;
		return EBUSY;
	}

	/* Only a pinned buffer can be unpinned. */
	if (so->state != I915_SCANOUT_PINNED)
		return EINVAL;

	/* Gives the GGTT range back. */
	drv_i915_gt_display_unbind(so->gm, so->obj);

	/* ALLOCATED again: no surface address, no owner. */
	so->surf = 0U;
	so->pin_owner = NULL;
	so->state = I915_SCANOUT_ALLOCATED;

	/* Succeeded: the buffer is no longer in the GGTT. */
	return 0;
}

/*
 * Destroys a scanout buffer.
 *
 * Returns 0, EBUSY while it is pinned or in use, or EINVAL for storage
 * that holds no buffer.  A destroyed buffer's storage is zeroed.
 */
int
drv_i915_scanout_destroy(
	struct i915_scanout *so)
{
	/* Refuses a missing buffer. */
	if (so == NULL)
		return EINVAL;

	/* A pinned buffer, or one in use, keeps its backing. */
	if (so->state >= I915_SCANOUT_PINNED) {
		so->refused_destroy++;
		return EBUSY;
	}

	/* Only an allocated buffer can be destroyed. */
	if (so->state != I915_SCANOUT_ALLOCATED)
		return EINVAL;

	/* Frees the backing and empties the storage. */
	drv_i915_gt_object_destroy(so->gm, so->obj);
	kern_memset(so, 0, sizeof(*so));

	/* Succeeded: the storage is empty again. */
	return 0;
}

/*
 * Abandons a scanout buffer whose display was not shown to have stopped.
 *
 * Pages, mapping and PTEs are kept for ever; the teardown leaves them
 * alone.
 */
void
drv_i915_scanout_abandon(
	struct i915_scanout *so)
{
	const char *owner;

	/* Only a pinned buffer can still be read by a display. */
	if (so == NULL || so->state < I915_SCANOUT_PINNED)
		return;

	/* KEEP makes the memory refuse to free or unbind the object. */
	so->obj->keep = 1;
	so->state = I915_SCANOUT_ABANDONED;

	/* Names the kept buffer in the log. */
	owner = "-";
	if (so->pin_owner != NULL)
		owner = so->pin_owner;

	kern_logf("i915: scanout: ABANDONED at GGTT 0x%llx (%u bytes, owner %s): the display was not shown to have stopped; pages, mapping and PTEs are kept\n",
	    (unsigned long long)so->surf,
	    so->size,
	    owner);
}

/*
 * Returns resident buffer i (0 = A, 1 = B) while the panel is up, for a
 * GPU that draws into it; NULL otherwise.
 */
struct i915_scanout *
drv_i915_lcd_resident_buffer(
	struct i915_display *display,
	unsigned i)
{
	/* The buffers exist for the GPU only while the panel is up. */
	if (!display->resident_up)
		return NULL;
	if (i >= 2U)
		return NULL;

	/* Reports the buffer. */
	return &display->resident_buf[i];
}

/*
 * Returns the resident buffer not on the panel while the panel is up; NULL
 * otherwise.
 */
struct i915_scanout *
drv_i915_lcd_resident_back(
	struct i915_display *display)
{
	/* The back buffer exists only while the panel is up. */
	if (!display->resident_up)
		return NULL;

	/* The buffer the panel does not show. */
	return &display->resident_buf[display->resident_front ^ 1U];
}

/*
 * Maps both panel buffers into a presenting session's address space.
 *
 * Page by page and uncached, because the display does not snoop the LLC.
 * The backing is the GT memory's DMA pages; the address space is the
 * session's.  One address space holds them at a time: the same one again
 * is a no-op, another is refused with EBUSY.  Returns 0, EIO when a buffer
 * or a page address is missing, or the address space's error.
 *
 * XXX: the session address space never reuses a range, so the ranges are
 * only cleared, not given back, when the display goes, and no TLB
 * invalidation is needed for a range nothing names.
 */
int
drv_i915_scanout_map_panel(
	struct i915_display *display,
	struct i915_ppgtt *vm)
{
	struct i915_device *device;
	struct i915_present_window *window;
	struct i915_scanout *so;
	uint64_t va;
	uint64_t dma;
	unsigned i;
	unsigned page;
	int error;

	device = display->device;
	window = &display->window;

	/* The buffers are already in this address space. */
	if (window->map_vm == vm)
		return 0;

	/* XXX: one presenting address space at a time. */
	if (window->map_vm != NULL)
		return EBUSY;

	/* Maps each buffer under the device mutex, which guards the address spaces. */
	mutex_lock(&device->mutex);

	for (i = 0U; i < 2U; i++) {
		/* The buffer must be up and backed. */
		so = drv_i915_lcd_resident_buffer(display, i);
		if (so == NULL || so->obj == NULL) {
			mutex_unlock(&device->mutex);
			return EIO;
		}

		/* Reserves a range as large as the buffer's pages. */
		error = drv_i915_ppgtt_va_alloc(vm, (uint64_t)so->obj->pages * I915_GT_PAGE_BYTES, &va);

		/* Maps every page uncached, in order. */
		for (page = 0U; error == 0 && page < so->obj->pages; page++) {
			error = drv_i915_gt_object_page_dma(so->obj, page, &dma);
			if (error != 0) {
				error = EIO;
				break;
			}

			error = drv_i915_ppgtt_insert_uncached(vm, va + (uint64_t)page * I915_GT_PAGE_BYTES, dma, 1U);
		}

		if (error != 0) {
			mutex_unlock(&device->mutex);
			return error;
		}

		window->map_va[i] = va;
		window->map_pages[i] = so->obj->pages;
	}

	/* The address space holds the buffers from here on. */
	window->map_vm = vm;

	mutex_unlock(&device->mutex);

	kern_logf("i915: resident display: panel buffers mapped for the GPU at 0x%llx / 0x%llx (%u pages each)\n",
	    (unsigned long long)window->map_va[0],
	    (unsigned long long)window->map_va[1],
	    window->map_pages[0]);

	/* Succeeded: the GPU can write either buffer. */
	return 0;
}

/*
 * Unmaps both panel buffers from the address space that holds them.
 *
 * The GPU's mappings go before the buffers do.
 */
void
drv_i915_scanout_unmap_panel(
	struct i915_display *display)
{
	struct i915_device *device;
	struct i915_present_window *window;
	unsigned i;

	device = display->device;
	window = &display->window;

	/* No address space holds the buffers. */
	if (window->map_vm == NULL)
		return;

	/* Clears both ranges under the device mutex. */
	mutex_lock(&device->mutex);

	for (i = 0U; i < 2U; i++)
		drv_i915_ppgtt_clear(window->map_vm, window->map_va[i], window->map_pages[i]);

	mutex_unlock(&device->mutex);

	/* No address space holds the buffers from here on. */
	window->map_vm = NULL;
}

/* Answers the pairing query: the node renders and displays, with no companion. */
static int
i915_scanout_device_query(
	void *device,
	void *session,
	struct gpu_device_info *request)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(session);

	/* The node is both the render device and the display. */
	request->roles = GPU_DEVICE_RENDER | GPU_DEVICE_DISPLAY;
	request->companion_id = 0U;

	/* Succeeded: the roles are filled in. */
	return 0;
}

/* Answers the scanout constraints of the one display: shared or copied, BGRA or RGBA, 64-byte alignments. */
static int
i915_scanout_constraints(
	void *device,
	void *session,
	struct gpu_scanout_constraints *request)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(session);

	/* Only the one display of this generation exists. */
	if (request->display_id != I915_SCANOUT_DISPLAY_ID)
		return ENOENT;
	if (request->generation != I915_SCANOUT_GENERATION)
		return ESTALE;

	/* A shared blob is copied by the GPU; a copied frame by the CPU. */
	request->flags = GPU_SCANOUT_SHARED | GPU_SCANOUT_COPY;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;

	/* A linear render target or sampler surface. */
	request->stride_alignment = I915_SCANOUT_IMPORT_ALIGN;
	request->offset_alignment = I915_SCANOUT_IMPORT_ALIGN;
	request->placement = 0U;
	request->max_dma_address = 0U;

	/* Succeeded: the constraints are filled in. */
	return 0;
}
