/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI device's memory: runs of host RAM the GPU reaches by DMA, runs
 * of a discrete GPU's local memory, and the quarantine of a lost device.
 *
 * Host memory is reached by the GPU at its bus address, which on the PC
 * and on the VisionFive 2's root complexes is its physical address (the
 * host bridge maps bus addresses one to one).  The driver takes every run
 * below 4 GiB (a GPU without a walker reaches no more from every unit,
 * and the rest follows for simplicity), below the
 * GPU's HOST_LIMIT when it reports one, and outside its local memory,
 * where the same addresses name the GPU's own memory.  Where the GPU
 * does not snoop the processor's caches (host_uncached), the processor's
 * view of a run is an uncached second mapping of it, made when the run is
 * taken and removed before it goes back.
 *
 * Local memory.  A discrete GPU (UMA clear, local memory in the MEM
 * window and a page-table walker) has memory of its own at
 * [MEM_VRAM_BASE, MEM_VRAM_BASE + MEM_VRAM_SIZE) of its physical
 * addresses.  The driver hands out runs of it by their offsets from
 * MEM_VRAM_BASE with a first-fit
 * allocator; a run's GPU address is its local address.  The processor
 * never reads or writes local memory: the GPU reaches it, and copies to
 * and from it, and everything the host and the GPU share (the ring,
 * fences, tables, programs, what a client maps) is host memory the GPU
 * reaches by DMA.  The local window (BAR2) is for a boot framebuffer the
 * firmware left alone: it is open while the boot console may write a
 * kept boot framebuffer through it, and closed otherwise.  The local
 * memory the firmware reserved (MEM_FW_VRAM) is kept out of the
 * allocator.
 * Nothing of local memory is cleared here: the caller clears a new run
 * with the GPU before any client sees it.
 *
 * Procedure referenced: amaterrace-gpu fe6431b1 docs/ghci.md (section
 * 11.1, the local window for the framebuffer only, and every other
 * access to memory made by the GPU; section 11.3, the ring, command
 * buffers and fences in host memory on a discrete GPU); lumiere-gpu
 * 78d1b6b7 ghci.md (section 11.3, MEM_FW_VRAM).
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "ghci.h"

/* The end of the 32-bit physical addresses every run of the driver lies below. */
#define ADDRESS_SPACE_END		0x100000000ULL

/* The tries at a run of host memory that does not fall into the local range. */
#define HOST_TRIES			8U

/* The runs of local memory the allocator's table holds at once. */
#define LOCAL_RUNS			1024U

/* The largest run of local memory one allocation takes (the allocator counts in 32 bits). */
#define LOCAL_RUN_MAX			0x80000000ULL

/*
 * The uncached views of RAM.
 *
 * A HAL without uncached views leaves them out of the kernel; they are
 * weak so that the kernel links, and a null test refuses host memory to
 * a device that needs the view (host_uncached) on such a kernel.
 */
extern int kern_pmem_map_uncached(const struct kern_pmem *run, void **mapped) __attribute__((weak));
extern int kern_pmem_unmap_uncached(void *mapped, size_t size) __attribute__((weak));

static int host_usable(const struct ghci_device *device, const struct kern_pmem *run);
static int host_view(struct ghci_device *device, struct ghci_allocation *allocation);
static void boot_ranges(struct ghci_device *device);
static void boot_framebuffer(const struct ghci_device *device, uint64_t *first, uint64_t *last);
static int boot_range(struct ghci_device *device, uint64_t address, uint64_t bytes, uint32_t *offset, uint32_t *length);
static void boot_add(struct ghci_device *device, uint32_t offset, uint32_t bytes);
static int local_reserve(struct ghci_device *device);
static void window_users_change(struct ghci_device *device, int change);

/*
 * Takes a run of host memory the GPU reaches, cleared, below limit
 * (0: the driver's own limit).
 *
 * Returns 0 with the allocation, or ENOMEM.
 */
int
ghci_memory_alloc(
	struct ghci_device *device,
	uint64_t bytes,
	uint64_t limit,
	struct ghci_allocation **result)
{
	struct ghci_allocation *allocation;
	struct kern_pmem held[HOST_TRIES];
	struct kern_pmem run;
	uint64_t end;
	unsigned held_count;
	unsigned index;
	int usable;
	int error;

	/* The record of the allocation, in whole pages. */
	allocation = kern_calloc(1U, sizeof(*allocation));
	if (allocation == NULL)
		return ENOMEM;
	allocation->kind = GHCI_MEMORY_HOST;
	allocation->bytes = (bytes + GHCI_PAGE_BYTES - 1U) & ~(uint64_t)(GHCI_PAGE_BYTES - 1U);

	/* The end of the addresses the run may take. */
	end = ghci_memory_limit(device);
	if (limit != 0U && limit < end)
		end = limit;

	/*
	 * Tries a bounded number of runs; a run that meets the local range is
	 * held so that the next try finds a different one, and every held
	 * run goes back at the end.
	 */
	held_count = 0U;
	error = ENOMEM;
	for (index = 0U; index < HOST_TRIES; index++) {
		/* A run the GPU can name. */
		error = kern_pmem_alloc_limited((size_t)allocation->bytes, GHCI_PAGE_BYTES, end - 1U, 0U, &run);
		if (error != 0) {
			error = ENOMEM;
			break;
		}

		/* A run outside the local range is taken. */
		usable = host_usable(device, &run);
		if (usable) {
			allocation->run = run;
			break;
		}

		/* Another run is held so that the next try finds a different one. */
		held[held_count] = run;
		held_count++;
		error = ENOMEM;
	}

	/* The held runs go back. */
	for (index = 0U; index < held_count; index++)
		(void)kern_pmem_free(&held[index]);

	/* No usable run. */
	if (error != 0) {
		kern_free(allocation);
		return ENOMEM;
	}

	/* The GPU's address of it is its bus address; the processor's view of it. */
	allocation->bus = allocation->run.paddr;
	error = host_view(device, allocation);
	if (error != 0) {
		(void)kern_pmem_free(&allocation->run);
		kern_free(allocation);
		return error;
	}

	/* Cleared, so that the GPU never reads what the run held before. */
	kern_memset(allocation->view, 0, (size_t)allocation->bytes);

	/* Succeeded: the run is the caller's. */
	*result = allocation;
	return 0;
}

/*
 * Gives a run back once the GPU no longer reaches it.
 *
 * Returns 0, or the error of a view that could not be removed; the run is
 * then kept, as memory that may still be reached is never handed out again.
 */
int
ghci_memory_free(
	struct ghci_device *device,
	struct ghci_allocation *allocation)
{
	int error;

	/* Nothing to give back. */
	if (allocation == NULL)
		return 0;

	/*
	 * A run of local memory goes back to the allocator; the caller holds
	 * the device's lock or owns the device alone.
	 */
	if (allocation->kind == GHCI_MEMORY_LOCAL) {
		error = 0;
		if (device->local_extents != NULL)
			error = ghci_range_free(&device->local_map, (uint32_t)(allocation->bus - device->caps.local_base));
		if (error != 0)
			kern_logf("ghci: %s: local run at %llx not in the allocator (%d)\n",
			    device->name,
			    (unsigned long long)allocation->bus,
			    error);
		kern_free(allocation);
		return 0;
	}

	/* The uncached view goes first. */
	if (device->host_uncached && kern_pmem_unmap_uncached != NULL) {
		error = kern_pmem_unmap_uncached(allocation->view, allocation->run.size);
		if (error != 0) {
			kern_logf("ghci: %s: host view not removed (%d); run kept\n", device->name, error);
			return error;
		}
	}

	/* The RAM goes back. */
	error = kern_pmem_free(&allocation->run);
	if (error != 0) {
		kern_logf("ghci: %s: host run not freed (%d)\n", device->name, error);
		return error;
	}

	/* Succeeded: the record goes too. */
	kern_free(allocation);
	return 0;
}

/*
 * Keeps a run the GPU of a lost device may still reach until bus
 * mastering is off.  The caller holds the device's lock or owns the
 * device alone.
 */
void
ghci_memory_quarantine(
	struct ghci_device *device,
	struct ghci_allocation *allocation)
{
	/* Nothing to keep. */
	if (allocation == NULL)
		return;

	/* The run joins the quarantine. */
	allocation->next = device->quarantined;
	device->quarantined = allocation;
}

/*
 * Gives back every run of the quarantine; only after bus mastering is
 * off, or the GPU reset and stopped, so that no DMA reaches them.
 */
void
ghci_memory_release_quarantine(
	struct ghci_device *device)
{
	struct ghci_allocation *allocation;
	int error;

	/* Each run in turn; one whose view cannot be removed stays. */
	while (device->quarantined != NULL) {
		allocation = device->quarantined;
		device->quarantined = allocation->next;
		error = ghci_memory_free(device, allocation);
		if (error != 0)
			break;
	}
}

/*
 * Adds one owner to an allocation (a resource, an export or a mapping).
 * The caller holds the device's lock.
 */
void
ghci_memory_hold(
	struct ghci_allocation *allocation)
{
	/* One more owner. */
	allocation->holds++;
}

/*
 * Lets one owner of an allocation go; the last one gives it back.  On a
 * lost device an allocation the GPU was ever given joins the quarantine
 * instead, as the GPU may still reach it, and so does one the display
 * controller may still read.  The caller holds the device's lock.
 */
void
ghci_memory_unhold(
	struct ghci_device *device,
	struct ghci_allocation *allocation)
{
	enum ghci_device_state state;
	unsigned long irq;
	int error;

	/* An allocation nobody holds is a caller's error; it is kept rather than given back twice. */
	if (allocation->holds == 0U) {
		kern_logf("ghci: %s: allocation let go more often than held\n", device->name);
		return;
	}

	/* Owners remain. */
	allocation->holds--;
	if (allocation->holds != 0U)
		return;

	/* The device's life, as the recovery last set it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	state = device->state;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Memory a lost GPU may still reach waits for bus mastering to go. */
	if (state == GHCI_STATE_LOST && allocation->reached) {
		ghci_memory_quarantine(device, allocation);
		return;
	}

	/* Memory the display controller may still read waits too. */
	if (allocation->scan_unsure) {
		ghci_memory_quarantine(device, allocation);
		return;
	}

	/* Succeeded: the run goes back, or stays in the quarantine when its view cannot go. */
	error = ghci_memory_free(device, allocation);
	if (error != 0)
		ghci_memory_quarantine(device, allocation);
}

/*
 * Takes a run of the GPU's local memory, not cleared.
 *
 * The caller clears the run with the GPU before a client can see it, as
 * the run may hold what an earlier client left.  Returns 0 with the
 * allocation, ENOTSUP on a device whose local memory the driver does not
 * hand out, or ENOMEM.
 */
int
ghci_memory_alloc_local(
	struct ghci_device *device,
	uint64_t bytes,
	struct ghci_allocation **result)
{
	struct ghci_allocation *allocation;
	uint32_t offset;
	int error;

	/* A device without local memory in use. */
	if (!device->local_ready)
		return ENOTSUP;

	/* A run the allocator can count, in whole pages. */
	bytes = (bytes + GHCI_PAGE_BYTES - 1U) & ~(uint64_t)(GHCI_PAGE_BYTES - 1U);
	if (bytes == 0U || bytes > LOCAL_RUN_MAX)
		return ENOMEM;

	/* The record of the allocation. */
	allocation = kern_calloc(1U, sizeof(*allocation));
	if (allocation == NULL)
		return ENOMEM;

	/* The run, first fit on a page. */
	mutex_lock(&device->lock);

	error = ghci_range_alloc(&device->local_map, (uint32_t)bytes, GHCI_PAGE_BYTES, &offset);

	mutex_unlock(&device->lock);

	/* No room, or no room in the allocator's table. */
	if (error != 0) {
		kern_free(allocation);
		return ENOMEM;
	}

	/* The GPU's address of it is its local address; the processor has no view of it. */
	allocation->kind = GHCI_MEMORY_LOCAL;
	allocation->bus = device->caps.local_base + offset;
	allocation->bytes = bytes;
	allocation->view = NULL;

	/* Succeeded: the run is the caller's. */
	*result = allocation;
	return 0;
}

/*
 * Takes a run of the GPU's local memory, not cleared, as
 * ghci_memory_alloc_local() does, for a caller that holds the device's
 * lock (the recovery's replay).
 */
int
ghci_memory_alloc_local_locked(
	struct ghci_device *device,
	uint64_t bytes,
	struct ghci_allocation **result)
{
	struct ghci_allocation *allocation;
	uint32_t offset;
	int error;

	/* A device without local memory in use. */
	if (!device->local_ready)
		return ENOTSUP;

	/* A run the allocator can count, in whole pages. */
	bytes = (bytes + GHCI_PAGE_BYTES - 1U) & ~(uint64_t)(GHCI_PAGE_BYTES - 1U);
	if (bytes == 0U || bytes > LOCAL_RUN_MAX)
		return ENOMEM;

	/* The record of the allocation. */
	allocation = kern_calloc(1U, sizeof(*allocation));
	if (allocation == NULL)
		return ENOMEM;

	/* The run, first fit on a page; no room is ENOMEM. */
	error = ghci_range_alloc(&device->local_map, (uint32_t)bytes, GHCI_PAGE_BYTES, &offset);
	if (error != 0) {
		kern_free(allocation);
		return ENOMEM;
	}

	/* Succeeded: the run is the caller's, at its local address. */
	allocation->kind = GHCI_MEMORY_LOCAL;
	allocation->bus = device->caps.local_base + offset;
	allocation->bytes = bytes;
	allocation->view = NULL;
	*result = allocation;
	return 0;
}

/*
 * Starts the use of the GPU's local memory and handles the boot
 * display, before the ring runs.
 *
 * The local memory the firmware reserved (MEM_FW_VRAM) is found first.
 * On a discrete device the allocator of local memory starts, with that
 * range kept out of it.  A device that is not
 * discrete, whose MEM_VRAM_BASE is not on a page, or whose allocator cannot
 * be made keeps everything in host memory.  The local window stays open
 * while the boot console may write a kept boot framebuffer through it,
 * and is closed otherwise.  The caller owns the device alone.  Returns 0; a failure
 * only leaves the device unified.
 */
int
ghci_memory_local_start(
	struct ghci_device *device)
{
	int discrete;
	int walker;
	int aligned;
	int error;

	/* Nothing in use yet. */
	device->local_ready = 0;
	device->boot_reserved = 0;
	device->boot_count = 0U;
	device->window_users = 0U;
	device->window_open = 0;

	/* The firmware's reservation of local memory, which the boot console may still write. */
	boot_ranges(device);
	if (device->boot_count != 0U)
		device->boot_reserved = 1;

	/* Discrete needs UMA clear, local memory and a walker (the flat GPU names memory by bus addresses). */
	discrete = ghci_caps_has(&device->caps, GHCI_HAS_DISCRETE);
	walker = ghci_caps_has(&device->caps, GHCI_HAS_HW_WALKER);
	if (discrete && !walker)
		kern_logf("ghci: %s: local memory not used without a page-table walker\n", device->name);

	/* Local memory is handed out in pages, from a MEM_VRAM_BASE on a page. */
	aligned = (device->caps.local_base & (GHCI_PAGE_BYTES - 1U)) == 0U;
	if (discrete && walker && !aligned)
		kern_logf("ghci: %s: local memory at %llx not on a page; local memory not used\n",
		    device->name,
		    (unsigned long long)device->caps.local_base);

	/* The allocator's table, on a discrete device. */
	if (discrete && walker && aligned) {
		device->local_extents = kern_calloc(LOCAL_RUNS, sizeof(*device->local_extents));
		if (device->local_extents == NULL)
			kern_logf("ghci: %s: no room for the local allocator; local memory not used\n", device->name);
	}

	/* The allocator over the offsets of local memory, without what the boot display reads. */
	if (device->local_extents != NULL) {
		ghci_range_init(&device->local_map, 0U, device->caps.local_bytes, device->local_extents, LOCAL_RUNS);
		error = 0;
		if (device->boot_reserved)
			error = local_reserve(device);
		if (error == 0) {
			device->local_ready = 1;
			kern_logf("ghci: %s: discrete, %llu MiB of local memory at %llx\n",
			    device->name,
			    (unsigned long long)(device->caps.local_bytes >> 20),
			    (unsigned long long)device->caps.local_base);
		} else {
			kern_logf("ghci: %s: boot framebuffer not kept out (%d); local memory not used\n", device->name, error);
			kern_free(device->local_extents);
			device->local_extents = NULL;
		}
	}

	/*
	 * The window: the kept boot framebuffer is its one user, and without
	 * one it closes.
	 *
	 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 13.3, the window kept open while the boot console may write, and
	 * closed at once when no boot framebuffer is found).
	 */
	window_users_change(device, device->boot_reserved ? 1 : 0);

	/* Succeeded: local memory is in use, or everything stays in host memory. */
	return 0;
}

/*
 * Ends the use of local memory at the detach, after the quarantine went:
 * the allocator goes when nothing but the firmware's ranges is in it.  The
 * window keeps what it was (a boot console may still use it).
 */
void
ghci_memory_local_stop(
	struct ghci_device *device)
{
	unsigned kept;

	/* No allocator. */
	device->local_ready = 0;
	if (device->local_extents == NULL)
		return;

	/* A run still out besides the firmware's (one the quarantine kept) keeps the table, so that its free finds it. */
	kept = device->boot_count;
	if (device->boot_fb_freed)
		kept--;
	if (device->local_map.count > kept) {
		kern_logf("ghci: %s: %u local runs still out at the detach\n",
		    device->name,
		    device->local_map.count - kept);
		return;
	}

	/* Succeeded: the table goes. */
	kern_free(device->local_extents);
	device->local_extents = NULL;
}

/*
 * Reports the bytes of local memory the driver hands out: MEM_VRAM_SIZE on a
 * discrete device whose local memory is in use, else 0.
 */
uint64_t
ghci_memory_local_bytes(
	const struct ghci_device *device)
{
	/* A device that keeps everything in host memory. */
	if (!device->local_ready)
		return 0U;

	/* Succeeded: the local memory. */
	return device->caps.local_bytes;
}

/*
 * Ends the boot console's use of the local window once the driver has
 * taken the display over, closes the window, which nothing else uses,
 * and gives the boot framebuffer's whole pages back to the allocator.
 * The rest of the firmware's reservation stays out of the allocator for
 * good: the firmware keeps it, and no page table may lie in it.
 *
 * The caller holds no lock: the function takes the device's lock, and
 * the interrupt's for the window.  It is called once nothing draws to the
 * boot framebuffer any more (a framebuffer console, a panic's output) and
 * the display no longer scans it.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 13.3, the
 * window kept open until the takeover, the boot framebuffer's extent
 * reclaimed after it, and the rest of MEM_FW_VRAM reserved for the
 * device's firmware; section 11.2, page-table memory never overlaps
 * MEM_FW_VRAM).
 */
void
ghci_memory_boot_release(
	struct ghci_device *device)
{
	int released;

	/* The boot console's use, once; the ranges stay out of the allocator. */
	mutex_lock(&device->lock);

	released = 0;
	if (device->boot_reserved) {
		device->boot_reserved = 0;
		released = 1;
	}

	/* The framebuffer's pages are local memory again, once. */
	if (released && device->local_ready && device->boot_fb_bytes != 0U && !device->boot_fb_freed) {
		(void)ghci_range_free(&device->local_map, device->boot_fb_offset);
		device->boot_fb_freed = 1;
	}

	mutex_unlock(&device->lock);

	/* Succeeded: the boot console no longer uses the window. */
	if (released)
		window_users_change(device, -1);
}

/*
 * Reports the end of the host addresses the driver gives the GPU: 4 GiB,
 * or HOST_LIMIT when the GPU reports a lower one.  HOST_LIMIT 0 lets the
 * GPU reach every address of PA_BITS; the driver still keeps to the low
 * 4 GiB, which a GPU without a walker reaches from every unit.
 *
 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 11.3,
 * MEM_HOST_LIMIT, 0 for the whole of PA_BITS; section 11.2, the units of
 * a flat GPU reach max(VA_BITS, 32) bits).
 */
uint64_t
ghci_memory_limit(
	const struct ghci_device *device)
{
	/* A GPU that reports no limit, or one above 4 GiB, is given the 32-bit range. */
	if (device->caps.host_limit == 0U || device->caps.host_limit > ADDRESS_SPACE_END)
		return ADDRESS_SPACE_END;

	/* Succeeded: the limit the GPU reported. */
	return device->caps.host_limit;
}

/*
 * Reports whether a run of RAM lies outside the GPU's local range: 1 when
 * it does.  The GPU routes every physical address in [MEM_VRAM_BASE,
 * MEM_VRAM_BASE + MEM_VRAM_SIZE) to its local memory, so a host run there
 * could not be reached.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 11.3,
 * the MEM capability's routing of physical addresses).
 */
static int
host_usable(
	const struct ghci_device *device,
	const struct kern_pmem *run)
{
	uint64_t start;
	uint64_t end;
	uint64_t limit;
	uint64_t local_end;

	/* The run's bus addresses must lie below the limit. */
	start = run->paddr;
	end = start + run->size;
	limit = ghci_memory_limit(device);
	if (end > limit)
		return 0;

	/* A GPU without local memory has no range to avoid. */
	if (device->caps.local_bytes == 0U)
		return 1;

	/* The run must not meet the local range. */
	local_end = device->caps.local_base + device->caps.local_bytes;
	if (end <= device->caps.local_base)
		return 1;
	if (start >= local_end)
		return 1;

	/* Succeeded: the run meets the local range and is not usable. */
	return 0;
}

/*
 * Gives the processor its view of a host run: the direct map where the
 * GPU snoops the caches, else an uncached mapping, so that neither side
 * needs cache maintenance.
 */
static int
host_view(
	struct ghci_device *device,
	struct ghci_allocation *allocation)
{
	void *mapped;
	int error;

	/* A GPU that snoops the caches shares the direct map with the processor. */
	if (!device->host_uncached) {
		allocation->view = kern_pmem_to_kernel(allocation->run.paddr);
		return 0;
	}

	/* A kernel without uncached views cannot give one. */
	if (kern_pmem_map_uncached == NULL) {
		kern_logf("ghci: %s: no uncached view of host memory on this kernel\n", device->name);
		return ENOTSUP;
	}

	/* The uncached view; the cached lines of the run are written back and dropped first. */
	error = kern_pmem_map_uncached(&allocation->run, &mapped);
	if (error != 0) {
		kern_logf("ghci: %s: no uncached view of host memory (%d)\n", device->name, error);
		return error;
	}

	/* Succeeded: the processor reaches the run through the uncached view. */
	allocation->view = (uint8_t *)mapped;
	return 0;
}

/*
 * Keeps the local memory the firmware reserved (MEM_FW_VRAM, a boot
 * framebuffer the firmware's display may still read and the boot console
 * may still write) in the device's boot ranges, so that the allocator
 * never hands it out.  A device without it keeps nothing.  When a boot
 * display runs, the whole pages of its framebuffer are a range of their
 * own (boot_fb_offset), which the takeover may give back; the rest of
 * the reservation is the firmware's for good.
 *
 * Procedure referenced: lumiere-gpu 875e54b3 ghci.md (section 11.3, the
 * MEM window's MEM_FW_VRAM_BASE and MEM_FW_VRAM_SIZE; section 11.2, page
 * table memory never overlaps it; section 13.3, only the boot
 * framebuffer's extent, MEM_VRAM_BASE + BOOT_DISP_FB_OFFSET for
 * BOOT_DISP_STRIDE x BOOT_DISP_HEIGHT bytes, may be reclaimed after the
 * takeover).
 */
static void
boot_ranges(
	struct ghci_device *device)
{
	uint64_t fw_first;
	uint64_t fw_last;
	uint64_t fb_first;
	uint64_t fb_last;
	uint32_t offset;
	uint32_t length;
	int kept;

	/* Without local memory or a firmware reservation, nothing to keep. */
	device->boot_fb_bytes = 0U;
	device->boot_fb_freed = 0;
	if (device->caps.local_bytes == 0U || device->caps.fw_bytes == 0U)
		return;

	/* The reservation's part of local memory, in whole pages. */
	kept = boot_range(device, device->caps.fw_base, device->caps.fw_bytes, &offset, &length);
	if (!kept) {
		kern_logf("ghci: %s: firmware VRAM at %llx outside local memory\n",
		    device->name,
		    (unsigned long long)device->caps.fw_base);
		return;
	}
	fw_first = offset;
	fw_last = (uint64_t)offset + length;

	/* The boot framebuffer's whole pages inside the reservation, when a boot display runs. */
	boot_framebuffer(device, &fb_first, &fb_last);
	if (fb_first < fw_first || fb_last > fw_last || fb_first >= fb_last) {
		fb_first = fw_last;
		fb_last = fw_last;
	}

	/* The reservation before the framebuffer, the framebuffer, and the reservation after it. */
	if (fb_first > fw_first)
		boot_add(device, (uint32_t)fw_first, (uint32_t)(fb_first - fw_first));
	if (fb_last > fb_first) {
		boot_add(device, (uint32_t)fb_first, (uint32_t)(fb_last - fb_first));
		device->boot_fb_offset = (uint32_t)fb_first;
		device->boot_fb_bytes = (uint32_t)(fb_last - fb_first);
	}
	if (fw_last > fb_last)
		boot_add(device, (uint32_t)fb_last, (uint32_t)(fw_last - fb_last));

	/* Succeeded: the reservation is kept. */
	kern_logf("ghci: %s: firmware VRAM at %llx (%u bytes) kept\n",
	    device->name,
	    (unsigned long long)device->caps.fw_base,
	    (unsigned)length);
}

/*
 * Reports the whole pages of the boot framebuffer a running boot display
 * scans, as offsets of local memory (first == last for none): the pages
 * inside [MEM_VRAM_BASE + FB_OFFSET, + STRIDE x HEIGHT), so that giving
 * them back never takes a page the firmware keeps.
 */
static void
boot_framebuffer(
	const struct ghci_device *device,
	uint64_t *first,
	uint64_t *last)
{
	uint64_t start;
	uint64_t end;

	/* No boot display, or one that is not running. */
	*first = 0U;
	*last = 0U;
	if ((device->caps.boot_disp_status & GHCI_BOOT_DISP_ACTIVE) == 0U)
		return;

	/* The framebuffer's bytes from MEM_VRAM_BASE, within local memory. */
	start = device->caps.boot_disp_offset;
	end = start + (uint64_t)device->caps.boot_disp_stride * device->caps.boot_disp_height;
	if (end <= start || end > device->caps.local_bytes)
		return;

	/* Succeeded: its whole pages, rounded inward. */
	*first = (start + GHCI_PAGE_BYTES - 1U) & ~(uint64_t)(GHCI_PAGE_BYTES - 1U);
	*last = end & ~(uint64_t)(GHCI_PAGE_BYTES - 1U);
	if (*last < *first)
		*last = *first;
}

/*
 * Reports the part of [address, address + bytes) in local memory, in
 * whole pages from MEM_VRAM_BASE and cut to local memory, in offset and
 * length.  Returns 1 when the range meets local memory, else 0.
 */
static int
boot_range(
	struct ghci_device *device,
	uint64_t address,
	uint64_t bytes,
	uint32_t *offset,
	uint32_t *length)
{
	uint64_t start;
	uint64_t end;
	uint64_t local_end;
	uint64_t first;
	uint64_t last;

	/* An empty range reads nothing. */
	if (bytes == 0U)
		return 0;

	/* A range wholly outside local memory is not the driver's to keep. */
	start = address;
	end = address + bytes;
	local_end = device->caps.local_base + device->caps.local_bytes;
	if (end <= device->caps.local_base || start >= local_end)
		return 0;

	/* Its part in local memory. */
	if (start < device->caps.local_base)
		start = device->caps.local_base;
	if (end > local_end)
		end = local_end;

	/* In whole pages from MEM_VRAM_BASE, cut to local memory. */
	first = (start - device->caps.local_base) & ~(uint64_t)(GHCI_PAGE_BYTES - 1U);
	last = (end - device->caps.local_base + GHCI_PAGE_BYTES - 1U) & ~(uint64_t)(GHCI_PAGE_BYTES - 1U);
	if (last > device->caps.local_bytes)
		last = device->caps.local_bytes;

	/* Succeeded: the range meets local memory. */
	*offset = (uint32_t)first;
	*length = (uint32_t)(last - first);
	return 1;
}

/*
 * Adds a range to the device's boot ranges, keeping them in address order
 * and joining ranges that overlap, so that each is taken out of the
 * allocator, and given back, as one run (ranges that only touch stay
 * apart: the boot framebuffer is given back without the rest).  The caller owns the device
 * alone, and adds at most GHCI_BOOT_RANGES ranges.
 */
static void
boot_add(
	struct ghci_device *device,
	uint32_t offset,
	uint32_t bytes)
{
	uint64_t end;
	uint64_t next_end;
	uint32_t swap;
	unsigned count;
	unsigned index;
	unsigned slot;

	/* The new range at the end. */
	count = device->boot_count;
	if (count >= GHCI_BOOT_RANGES)
		return;
	device->boot_offset[count] = offset;
	device->boot_bytes[count] = bytes;
	count++;

	/* Moved down to its place in address order. */
	for (index = count - 1U; index > 0U; index--) {
		if (device->boot_offset[index - 1U] <= device->boot_offset[index])
			break;
		swap = device->boot_offset[index - 1U];
		device->boot_offset[index - 1U] = device->boot_offset[index];
		device->boot_offset[index] = swap;
		swap = device->boot_bytes[index - 1U];
		device->boot_bytes[index - 1U] = device->boot_bytes[index];
		device->boot_bytes[index] = swap;
	}

	/* A range that starts inside or at the end of the one before joins it. */
	slot = 0U;
	for (index = 1U; index < count; index++) {
		end = (uint64_t)device->boot_offset[slot] + device->boot_bytes[slot];
		if (device->boot_offset[index] < end) {
			next_end = (uint64_t)device->boot_offset[index] + device->boot_bytes[index];
			if (next_end > end)
				device->boot_bytes[slot] = (uint32_t)(next_end - device->boot_offset[slot]);
			continue;
		}
		slot++;
		device->boot_offset[slot] = device->boot_offset[index];
		device->boot_bytes[slot] = device->boot_bytes[index];
	}

	/* Succeeded: the ranges are in order and apart. */
	device->boot_count = slot + 1U;
}

/*
 * Takes the boot ranges out of the empty local allocator.  First fit on a
 * table filled from offset 0 takes the next run right after the last
 * one, so each range is reached by first taking the gap before it; the
 * gaps go back at the end.  Returns 0, or the allocator's error, with no
 * range taken.
 */
static int
local_reserve(
	struct ghci_device *device)
{
	uint32_t gaps[GHCI_BOOT_RANGES];
	uint32_t next;
	uint32_t taken;
	unsigned gap_count;
	unsigned range_count;
	unsigned index;
	int error;

	/* Each range in address order, with the gap before it. */
	next = 0U;
	gap_count = 0U;
	range_count = 0U;
	error = 0;
	for (index = 0U; index < device->boot_count && error == 0; index++) {
		/* The gap before the range, which first fit gives from next. */
		if (device->boot_offset[index] > next) {
			error = ghci_range_alloc(&device->local_map, device->boot_offset[index] - next, GHCI_PAGE_BYTES, &taken);
			if (error != 0)
				break;
			gaps[gap_count] = taken;
			gap_count++;
			if (taken != next) {
				error = EINVAL;
				break;
			}
		}

		/* The range, which first fit now puts right after the gap. */
		error = ghci_range_alloc(&device->local_map, device->boot_bytes[index], GHCI_PAGE_BYTES, &taken);
		if (error != 0)
			break;
		if (taken != device->boot_offset[index]) {
			(void)ghci_range_free(&device->local_map, taken);
			error = EINVAL;
			break;
		}
		range_count++;
		next = device->boot_offset[index] + device->boot_bytes[index];
	}

	/* The gaps go back either way, and on a failure the ranges taken too. */
	for (index = 0U; index < gap_count; index++)
		(void)ghci_range_free(&device->local_map, gaps[index]);
	if (error != 0) {
		for (index = 0U; index < range_count; index++)
			(void)ghci_range_free(&device->local_map, device->boot_offset[index]);
		return error;
	}

	/* Succeeded: the ranges are out of the allocator. */
	return 0;
}

/*
 * Changes the count of the local window's users by change (1, -1, or 0
 * to write the state again) and writes WINDOW_CTRL.ENABLE: open while
 * the kept boot framebuffer uses the window, closed otherwise.  The write
 * is read back, so that it has reached the device before the caller goes
 * on.  A device without a window has nothing to write.  Takes the
 * interrupt's lock.
 */
static void
window_users_change(
	struct ghci_device *device,
	int change)
{
	unsigned long irq;
	int has_window;
	int unbalanced;
	int open;

	/* The MEM window, with a local window. */
	has_window = device->caps.window_bytes != 0U;

	/* The count and the register, in one section. */
	irq = spin_lock_irqsave(&device->irq_lock);

	unbalanced = 0;
	if (change > 0)
		device->window_users++;
	if (change < 0 && device->window_users == 0U)
		unbalanced = 1;
	if (change < 0 && device->window_users != 0U)
		device->window_users--;
	open = device->window_users != 0U;
	if (has_window) {
		ghci_write(device, GHCI_REG_MEM_WINDOW_CTRL, open ? GHCI_MEM_WINDOW_ENABLE : 0U);
		(void)ghci_read(device, GHCI_REG_MEM_WINDOW_CTRL);
		device->window_open = open;
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A user let go more often than it held is a caller's error; the count stays at 0. */
	if (unbalanced)
		kern_logf("ghci: %s: local window let go more often than held\n", device->name);
}
