/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Borrowed contiguous RGB32 buffers are retired only after actual HVS adoption. */
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/pmem.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The boot primary and two spare lists do not overlap the reserved filter. */
#define FLIP_CONSOLE_LIST 43U
#define FLIP_FIRST_LIST 64U
#define FLIP_LIST_STRIDE 16U
#define FLIP_PLANE_WORDS 9U
#define FLIP_NO_LIST 4096U

static int check_pipeline(struct bcm2711_display *display, bool recovering);
static int prepare_frame(const struct drv_bcm2711_boot_screen *console, const struct drv_bcm2711_boot_screen *frame, uint32_t *words);
static bool frames_overlap(const struct drv_bcm2711_boot_screen *a, const struct drv_bcm2711_boot_screen *b);
static void publish_list(struct bcm2711_display *display, uint32_t list);
static int wait_adoption(struct bcm2711_display *display);

/*
 * Initializes persistent IRQ state before installing source callback owners.
 * The enclosing display is zeroed at discovery and outlives all submissions.
 */
void
bcm2711_display_flip_init(
	struct bcm2711_display *display)
{
	/* Initializes once, so repeated IRQ preparation cannot release DMA holds. */
	if (display->flip.initialized)
		return;
	spin_init(&display->flip.guard, LOCK_RANK_DEVICE, "bcm2711-display");
	display->flip.active_list = FLIP_CONSOLE_LIST;
	display->flip.pending_list = FLIP_NO_LIST;
	display->flip.initialized = true;
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
	if (display->compositor.mapped == NULL || display->compositor.size < 0x4144U)
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

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the caller may submit buffers within the unchanged boot mode. */
	return 0;
}

/*
 * Presents one caller-owned buffer and waits for a matching selected-PV frame.
 * Errors after publication retain all possibly referenced buffers in status.
 */
int
bcm2711_display_flip_present(
	struct bcm2711_display *display,
	const struct drv_bcm2711_boot_screen *frame)
{
	uint32_t words[FLIP_PLANE_WORDS];
	uint32_t slot;
	uint32_t index;
	uint32_t list;
	unsigned long enabled;
	bool overlap;
	void *mapping;
	int error;

	/* Rejects unrepresentable geometry before touching ownership or hardware. */
	error = prepare_frame(&display->screen, frame, words);
	if (error != 0)
		return error;
	if (!display->flip.initialized)
		return ENODEV;
	mapping = kern_pmem_to_kernel(frame->physical);
	if (mapping == NULL)
		return EFAULT;

	/* Reserves a free SRAM slot without overwriting an active or uncertain list. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	if (!display->flip.attached) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return ENODEV;
	}

	/* An existing operation or uncertain DMA requires admission refusal. */
	if (display->flip.busy || display->flip.uncertain) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return EBUSY;
	}

	/* Rejects aliasing with the console and every buffer still referenced by DMA. */
	overlap = frames_overlap(frame, &display->screen);
	if (overlap) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return EINVAL;
	}

	/* Checks every retained interval before choosing another borrowed allocation. */
	for (index = 0; index < BCM2711_FLIP_SLOTS; index++) {
		/* Only retained descriptors name live storage; unused copies are historical. */
		if ((display->flip.retained_mask & (1U << index)) == 0)
			continue;
		overlap = frames_overlap(frame, &display->flip.frames[index]);
		if (overlap) {
			spin_unlock_irqrestore(&display->flip.guard, enabled);
			return EINVAL;
		}
	}

	/* Chooses an unheld slot while BUSY excludes another submitter or restorer. */
	slot = 0;
	if ((display->flip.retained_mask & 1U) != 0)
		slot = 1;

	/* Keeps another caller from publishing while candidate cache cleaning runs. */
	display->flip.busy = true;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Publishes frozen RGB bytes before any list can make them DMA-visible. */
	kern_dcache_clean_range(mapping, (size_t)frame->size);
	kern_io_write_barrier();

	/* Rechecks ownership, then installs all words before the next-list pointer. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	/* Confirms exclusive use of the established channel and both list pointers. */
	error = check_pipeline(display, false);
	if (error != 0) {
		display->flip.uncertain = true;
		display->flip.busy = false;
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return error;
	}

	/* Writes the inactive slot completely while the current slot stays intact. */
	list = FLIP_FIRST_LIST + slot * FLIP_LIST_STRIDE;
	for (index = 0; index < FLIP_PLANE_WORDS; index++) {
		/* Context placeholders are fresh even when this retired slot is reused. */
		kern_mmio_write32(display->compositor.mapped + 0x4000U + (list + index) * 4U, words[index]);
	}

	/* Retains the candidate before hardware can fetch its newly published list. */
	display->flip.frames[slot] = *frame;
	display->flip.retained_mask |= 1U << slot;
	publish_list(display, list);

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Waits without holding the IRQ guard; timeout never releases borrowed DMA. */
	error = wait_adoption(display);
	if (error != 0)
		return error;

	/* Succeeded: status now retains only the adopted framebuffer. */
	return 0;
}

/*
 * Restores the retained boot console without changing mode, routing or clocks.
 * A fresh matching PV interrupt is required even after a late uncertain flip.
 */
int
bcm2711_display_flip_restore(
	struct bcm2711_display *display)
{
	unsigned long enabled;
	void *mapping;
	int error;

	/* Requires the persistent owner and the existing console CPU mapping. */
	if (!display->flip.initialized)
		return ENODEV;
	mapping = kern_pmem_to_kernel(display->screen.physical);
	if (mapping == NULL)
		return EFAULT;

	/* Excludes ordinary submissions while console bytes are prepared. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	if (!display->flip.attached) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return ENODEV;
	}

	/* An in-flight submitter owns both publication and its synchronous result. */
	if (display->flip.busy) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return EBUSY;
	}

	/* Keeps another caller from publishing while console cache cleaning runs. */
	display->flip.busy = true;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Makes current console contents visible before reinstating its original list. */
	kern_dcache_clean_range(mapping, (size_t)display->screen.size);
	kern_io_write_barrier();

	/* Allows recovery from uncertain current/next pointers only on this pipeline. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	error = check_pipeline(display, true);
	if (error != 0) {
		display->flip.uncertain = true;
		display->flip.busy = false;
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return error;
	}

	/* Requeues the preserved console list and demands a fresh matching source. */
	publish_list(display, FLIP_CONSOLE_LIST);

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Returns holds only after the selected PV proves boot-console adoption. */
	error = wait_adoption(display);
	if (error != 0)
		return error;

	/* Succeeded: no caller-provided framebuffer remains retained. */
	return 0;
}

/*
 * Copies buffer holds and progress under the IRQ guard for the sole buffer owner.
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
	memset(status, 0, sizeof(*status));
	if (!display->flip.initialized)
		return;

	/* Copies immutable descriptors alongside the mask that defines their lifetime. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	status->attached = display->flip.attached;
	status->busy = display->flip.busy;
	status->uncertain = display->flip.uncertain;
	status->active_list = display->flip.active_list;
	status->pending_list = display->flip.pending_list;
	status->retained_mask = display->flip.retained_mask;
	status->frame_sequence = display->flip.frame_sequence;
	for (slot = 0; slot < BCM2711_FLIP_SLOTS; slot++) {
		/* Masked-out copies do not grant DMA ownership and may name retired memory. */
		status->frames[slot] = display->flip.frames[slot];
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);
}

/*
 * Observes the selected PV's acknowledged frame with the caller's guard held.
 * A timeout stays uncertain until actual console adoption permits all retirement.
 */
void
bcm2711_display_flip_vblank_locked(
	struct bcm2711_display *display,
	uint32_t current)
{
	uint32_t slot;
	uint32_t control;

	/* Counts real selected-PV frames independently of submissions and old lists. */
	display->flip.frame_sequence++;
	if (!display->flip.attached || display->flip.pending_list == FLIP_NO_LIST)
		return;
	if (current != display->flip.pending_list)
		return;

	/* Records adoption before marking completion visible to the synchronous caller. */
	display->flip.active_list = current;
	display->flip.pending_list = FLIP_NO_LIST;
	if (current == FLIP_CONSOLE_LIST) {
		/* Fresh console adoption makes every caller framebuffer safe to release. */
		display->flip.retained_mask = 0;
		display->flip.uncertain = false;
	} else if (!display->flip.uncertain) {
		/* Ordinary adoption releases the preceding list's borrowed storage. */
		slot = (current - FLIP_FIRST_LIST) / FLIP_LIST_STRIDE;
		display->flip.retained_mask = 1U << slot;
	}

	/* Clears stale underrun before enabling the adopted channel's source. */
	kern_mmio_write32(display->compositor.mapped + 0x04, 0x200);
	control = kern_mmio_read32(display->compositor.mapped);
	kern_mmio_write32(display->compositor.mapped, control | 0x200U);
	display->flip.completed = true;
}

/* Checks sole ownership and unchanged timing before any list publication. */
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
	uint32_t port;

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
		/* Known private pointers are the only uncertainty this component owns. */
		if (current != FLIP_CONSOLE_LIST && current != 64U && current != 80U)
			return EBUSY;
		if (next != FLIP_CONSOLE_LIST && next != 64U && next != 80U)
			return EBUSY;
	} else {
		/* Ordinary publication requires the last completed list on both pointers. */
		if (current != display->flip.active_list || next != display->flip.active_list)
			return EBUSY;
	}

	/* Succeeded: spare SRAM and channel-zero publication belong to this owner. */
	return 0;
}

/* Generates one independently encoded unscaled RGB32 primary plus its end word. */
static int
prepare_frame(
	const struct drv_bcm2711_boot_screen *console,
	const struct drv_bcm2711_boot_screen *frame,
	uint32_t *words)
{
	uint32_t order;

	/* Requires the same dimensions and RGB byte order as the established mode. */
	if (frame == NULL)
		return EINVAL;
	if (frame->width != console->width || frame->height != console->height)
		return EINVAL;
	if (frame->width == 0 || frame->width > 4096U)
		return EINVAL;
	if (frame->height == 0 || frame->height > 4096U)
		return EINVAL;
	if (frame->format != console->format || frame->format > 1U)
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

	/* Encodes a fresh eight-word plane and terminator without inherited contexts. */
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
	words[8] = 0x80000000U;

	/* Succeeded: the completed words may be copied into an inactive SRAM slot. */
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

/* Publishes completion state and the next pointer with the display guard held. */
static void
publish_list(
	struct bcm2711_display *display,
	uint32_t list)
{
	uint32_t control;
	uint32_t background;

	/* Masks underrun while installing the new list and background fill request. */
	control = kern_mmio_read32(display->compositor.mapped);
	kern_mmio_write32(display->compositor.mapped, control & ~0x200U);
	background = kern_mmio_read32(display->compositor.mapped + 0x44);
	kern_mmio_write32(display->compositor.mapped + 0x44, background | 0x01000000U);
	kern_mmio_write32(display->timing[display->port].mapped + 0x28, 0x80);

	/* Arms completion before publication; the selected source uses the same guard. */
	display->flip.completed = false;
	display->flip.pending_list = list;
	kern_io_write_barrier();
	kern_mmio_write32(display->compositor.mapped + 0x20, list);
	kern_io_write_barrier();
}

/* Waits for IRQ-confirmed adoption, retaining all holds across uncertain timeout. */
static int
wait_adoption(
	struct bcm2711_display *display)
{
	uint32_t waited;
	unsigned long enabled;
	int error;

	/* Bounds the wait while the selected PV can retire the published list. */
	waited = 0;
	for (;;) {
		/* Samples completion and latches timeout atomically with the IRQ observer. */
		enabled = spin_lock_irqsave(&display->flip.guard);

		error = 0;
		if (display->flip.completed) {
			/* The operation relinquishes BUSY only after its caller sees completion. */
			display->flip.busy = false;
		} else if (waited >= 100000U) {
			/* Both old and new buffers remain held even if adoption arrives later. */
			display->flip.uncertain = true;
			display->flip.busy = false;
			error = ETIMEDOUT;
		} else {
			/* EAGAIN is internal to this bounded wait, never a caller outcome. */
			error = EAGAIN;
		}

		spin_unlock_irqrestore(&display->flip.guard, enabled);

		/* Leaves the wait when adoption or its deadline has a durable outcome. */
		if (error != EAGAIN)
			break;
		kern_usleep_range(10, 10);
		waited += 10;
	}

	/* A timed-out operation supplies no permission to free borrowed buffers. */
	if (error != 0)
		return error;

	/* Succeeded: a real selected-PV frame matched the requested current list. */
	return 0;
}
