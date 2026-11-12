/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI device: what every GHCI GPU has in common, whatever its bus.
 *
 * The start identifies the GPU from its registers, resets it, checks
 * SCRATCH, reads its capabilities, starts the ring (cp.c) and registers
 * the device with the GPU core, which publishes /dev/gpuN named
 * "ghci:<PCI vendor>:<PCI device>".  The detach undoes it in the reverse
 * order once no session is open.
 *
 * Resources.  A storage resource or a blob is a run of host memory the
 * processor maps and copies to and from; it reaches the GPU only through
 * a session's mapping, which holds the memory until its unmap has
 * passed.  Local memory is not offered yet: every resource is host
 * memory.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-allocation.h>

#include "ghci.h"

/* The resources one session holds at most, and the largest. */
#define RESOURCES_MAX			4096U
#define RESOURCE_BYTES_MAX		(256ULL << 20)

/* The blob flags the device takes: mapping and sharing within the device. */
#define BLOB_FLAGS			(GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE)

static int identify(struct ghci_device *device);
static int reset_device(struct ghci_device *device);
static int check_scratch(struct ghci_device *device);
static int read_caps(struct ghci_device *device);
static void choose_policy(struct ghci_device *device);
static int register_device(struct ghci_device *device);
static void init_locks(struct ghci_device *device);
static void restart_service(struct ghci_device *device);
static int gpu_open(void *private_data, void **result);
static void gpu_close(void *private_data, void *opaque);
static int gpu_get_info(void *private_data, void *opaque, struct gpu_info *info);
static int gpu_resource_create(void *private_data, void *opaque, const struct gpu_resource_create *request, void **result);
static void gpu_resource_destroy(void *private_data, void *opaque, void *object);
static int gpu_blob_create(void *private_data, void *opaque, const struct gpu_blob_create *request, void **result, uint32_t *resource_id);
static int gpu_resource_read(void *private_data, void *opaque, void *object, uint64_t offset, void *buffer, uint32_t bytes);
static int gpu_resource_write(void *private_data, void *opaque, void *object, uint64_t offset, const void *buffer, uint32_t bytes);
static int gpu_resource_map(void *private_data, void *opaque, void *object, struct drv_gpu_mapping *mapping);
static int resource_new(struct ghci_device *device, struct ghci_session *session, uint64_t bytes, int blob, uint32_t flags, struct ghci_resource **result);
static int resource_range(const struct ghci_resource *resource, uint64_t offset, uint32_t bytes);
static int memory_blob(struct ghci_device *device, struct ghci_session *session, const struct gpu_blob_create *request, struct ghci_resource **result);

/*
 * The Vulkan executor (vk/).
 *
 * Every kernel built with the driver links it; the host test of the
 * driver's core links without it, and a null test then leaves the device
 * without Vulkan, as a device without a shader unit is.
 */
extern int ghci_vk_attach(struct ghci_device *device) __attribute__((weak));

/*
 * Starts a GHCI device whose bus fields the glue has filled.
 *
 * Returns 0 with the device registered, ENODEV for a window that is not
 * GHCI 1.0, EIO for a GPU that does not reset or run, or the error of
 * the step that failed; a failed start leaves the GPU reset and idle and
 * gives back what it took.
 */
int
ghci_device_start(
	struct ghci_device *device)
{
	int error;

	/* The locks and wait queues every later step uses. */
	init_locks(device);
	device->state = GHCI_STATE_ATTACHING;

	/* Reads what the GPU is; a window that is not GHCI 1.0 is left alone. */
	error = identify(device);
	if (error != 0)
		return error;

	/* Resets the GPU and waits for its memory; whatever a kernel before this one left running stops. */
	error = reset_device(device);
	if (error != 0)
		return error;

	/* The window answers writes as a register file should. */
	error = check_scratch(device);
	if (error != 0)
		return error;

	/* What the GPU can do, from the capability registers and the extended capability list. */
	error = read_caps(device);
	if (error != 0)
		return error;

	/* Whether the device gets the Vulkan executor, and what isolation its sessions have. */
	choose_policy(device);

	/* The local memory of a discrete GPU, the boot framebuffer in it, and the local window. */
	(void)ghci_memory_local_start(device);

	/*
	 * The display controller, while the processor is idle and the host
	 * may write it: a boot display keeps running, a dark one stays dark.
	 */
	error = ghci_display_attach(device);
	if (error != 0)
		kern_logf("ghci: %s: display not started (%d)\n", device->name, error);

	/* CP_HEAP, the ring, the fence page, the interrupt and the first fence. */
	error = ghci_cp_start(device);
	if (error != 0) {
		ghci_display_detach(device);
		ghci_memory_local_stop(device);
		return error;
	}

	/* The service thread, which recovers from faults and hangs. */
	error = ghci_service_start(device);
	if (error != 0) {
		ghci_display_detach(device);
		(void)ghci_cp_stop(device);
		return error;
	}

	/* The poll thread of a device without an interrupt. */
	error = ghci_poll_start(device);
	if (error != 0) {
		ghci_service_stop(device);
		ghci_display_detach(device);
		(void)ghci_cp_stop(device);
		return error;
	}

	/* Publishes /dev/gpuN. */
	error = register_device(device);
	if (error != 0) {
		ghci_service_stop(device);
		ghci_display_detach(device);
		(void)ghci_cp_stop(device);
		return error;
	}

	/* Succeeded: the device runs and is published. */
	kern_logf("ghci: %s: GHCI %u.%u, %s\n",
	    device->name,
	    (unsigned)(device->version >> GHCI_VERSION_MAJOR_SHIFT),
	    (unsigned)(device->version & 0xffffU),
	    device->polled ? "polled" : "interrupt");
	return 0;
}

/*
 * Stops a started device once no session is open.
 *
 * Returns 0 once the GPU is reset, its interrupt let go and its memory
 * given back, or EBUSY while a session is open or the interrupt cannot be
 * let go yet; the bus glue then keeps the device and may try again.
 */
int
ghci_device_stop(
	struct ghci_device *device)
{
	int error;

	/* Refuses while a session is open, and refuses new opens from here. */
	mutex_lock(&device->lock);

	error = 0;
	if (device->session_count != 0U)
		error = EBUSY;
	else
		device->withdrawing = 1;

	mutex_unlock(&device->lock);

	/* A session still open keeps the device. */
	if (error != 0)
		return error;

	/* A recovery in progress ends first, so that the detach and the recovery do not both drive CTRL. */
	ghci_recover_wait(device);

	/*
	 * The service thread leaves before the node goes: it reports a lost
	 * device to the core through the registration, which the core frees
	 * once it is unregistered.
	 */
	ghci_service_stop(device);

	/* The node goes; the core keeps the device while a session it opened lingers. */
	if (device->gpu != NULL) {
		error = drv_gpu_unregister(device->gpu);
		if (error != 0) {
			restart_service(device);
			return error;
		}
		device->gpu = NULL;
	}

	/* The executor's record goes with the node; no session of it is left. */
	if (device->executor_ops != NULL)
		device->executor_ops->detach(device);

	/*
	 * The display shows the boot image again while it is kept, or goes
	 * dark, before the processor stops; its memory goes back.
	 */
	ghci_display_detach(device);

	/*
	 * The processor halts and resets, the interrupt and bus mastering go,
	 * the poll thread leaves, and the memory goes with them; an interrupt
	 * that cannot be let go yet keeps the device for another try.
	 */
	error = ghci_cp_stop(device);
	if (error != 0)
		return error;

	/* Succeeded: the device holds nothing of the GPU. */
	return 0;
}

/*
 * Reads one register of the window.
 */
uint32_t
ghci_read(
	struct ghci_device *device,
	uint32_t offset)
{
	uint32_t word;

	/* One aligned 32-bit access; reading a GHCI register has no side effect. */
	word = kern_mmio_read32(device->registers + offset);

	/* Succeeded: the register's value. */
	return word;
}

/*
 * Writes one register of the window.
 */
void
ghci_write(
	struct ghci_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* One aligned 32-bit access. */
	kern_mmio_write32(device->registers + offset, value);
}

/*
 * Sleeps about the given milliseconds.
 */
void
ghci_sleep_ms(
	unsigned milliseconds)
{
	uint64_t deadline;

	/* Sleeps until the tick the time ends at. */
	deadline = clock_ticks() + kern_ms_to_ticks(milliseconds);
	sched_sleep(deadline);
}

/*
 * Waits up to a time for a register's masked bits to hold a value.
 *
 * Returns 0 once they do, or ETIMEDOUT.
 */
int
ghci_wait_register(
	struct ghci_device *device,
	uint32_t offset,
	uint32_t mask,
	uint32_t value,
	unsigned milliseconds)
{
	uint32_t word;
	unsigned waited;

	/* Reads the register every millisecond until it holds the value or the time is up. */
	for (waited = 0U; waited <= milliseconds; waited++) {
		word = ghci_read(device, offset);
		if ((word & mask) == value)
			return 0;

		/* Sleeps for a millisecond before the next read. */
		ghci_sleep_ms(1U);
	}

	/* The register never held the value. */
	return ETIMEDOUT;
}

/*
 * Reads the 64-bit fence the device writes into the fence page.
 */
uint64_t
ghci_fence_read(
	struct ghci_device *device)
{
	volatile uint32_t *word;
	uint64_t low;
	uint64_t high;
	uint64_t again;

	/*
	 * The two halves of the little-endian word: the high half is read
	 * around the low one, so that a write between the reads is seen.
	 */
	word = (volatile uint32_t *)(void *)device->fence_page->view;
	high = word[1];
	low = word[0];
	again = word[1];
	if (again != high)
		low = word[0];

	/*
	 * Nothing the caller reads after the fence (the GPU's writes it
	 * orders) is read before it, on a processor that reorders loads.
	 *
	 * Values referenced: amaterrace-gpu c74accc docs/ghci.md (section
	 * 9.1, a fence's memory write follows the writes of the work before
	 * it).
	 */
	kern_io_read_barrier();

	/* Succeeded: the fence as the device last wrote it. */
	return (again << 32) | low;
}

/*
 * Writes the 64-bit fence of the fence page, as the reset's completion does.
 */
void
ghci_fence_write(
	struct ghci_device *device,
	uint64_t value)
{
	volatile uint32_t *word;

	/* The low half, then the high half; the processor does not run while the driver writes it. */
	word = (volatile uint32_t *)(void *)device->fence_page->view;
	word[0] = (uint32_t)value;
	word[1] = (uint32_t)(value >> 32);
	kern_io_write_barrier();
}

/*
 * Adds a resource of a session that shows an allocation and holds it,
 * with a new resource id.  The caller holds the device's lock.
 */
int
ghci_resource_add(
	struct ghci_device *device,
	struct ghci_session *session,
	struct ghci_allocation *allocation,
	int blob,
	uint32_t flags,
	struct ghci_resource **result)
{
	struct ghci_resource *resource;

	/* The record. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* A new nonzero id, the allocation it holds, and the session's list. */
	device->resource_ids++;
	if (device->resource_ids == 0U)
		device->resource_ids = 1U;
	resource->resource_id = device->resource_ids;
	resource->allocation = allocation;
	resource->blob = blob;
	resource->flags = flags;
	ghci_memory_hold(allocation);
	resource->next = session->resources;
	session->resources = resource;

	/* Succeeded: the resource is the session's. */
	*result = resource;
	return 0;
}

/* Sets up the locks and wait queues of the device. */
static void
init_locks(
	struct ghci_device *device)
{
	/* The mutexes, in the order they are taken, and the interrupt's spinlock. */
	(void)mutex_init(&device->lock, LOCK_RANK_DEVICE, "ghci device");
	(void)mutex_init(&device->display_lock, LOCK_RANK_DEVICE, "ghci display");
	(void)mutex_init(&device->cp_lock, LOCK_RANK_DEVICE, "ghci ring");
	spin_init(&device->irq_lock, LOCK_RANK_DEVICE, "ghci interrupt");

	/* The wait queues: tickets, the ring's room, the gate, the service and the poll. */
	waitq_init(&device->fence_wake, "ghci fence");
	waitq_init(&device->ring_wake, "ghci room");
	waitq_init(&device->gate_wake, "ghci gate");
	waitq_init(&device->service_wake, "ghci service");
	waitq_init(&device->poll_wake, "ghci poll");
}

/*
 * Starts the service thread again and opens the device again after a
 * detach the core refused.
 */
static void
restart_service(
	struct ghci_device *device)
{
	int error;

	/* The thread that recovers from faults. */
	error = ghci_service_start(device);
	if (error != 0)
		kern_logf("ghci: %s: service thread not restarted (%d)\n", device->name, error);

	/* Opens are taken again. */
	mutex_lock(&device->lock);

	device->withdrawing = 0;

	mutex_unlock(&device->lock);
}

/* Refuses a window whose magic is not GHCI or whose major version is not 1. */
static int
identify(
	struct ghci_device *device)
{
	uint32_t window[2];
	int error;

	/* The two identification registers, read only. */
	window[0] = ghci_read(device, GHCI_REG_MAGIC);
	window[1] = ghci_read(device, GHCI_REG_VERSION);
	device->version = window[1];

	/* GHCI 1.0, any minor version. */
	error = ghci_caps_identify(window);
	if (error != 0) {
		kern_logf("ghci: %04x:%04x: not GHCI 1.0 (magic %08x, version %08x)\n",
		    (unsigned)device->pci_vendor,
		    (unsigned)device->pci_device,
		    (unsigned)window[0],
		    (unsigned)window[1]);
		return ENODEV;
	}

	/* Names the device after its function, for the log and the node. */
	(void)kern_snprintf(device->name, sizeof(device->name), "ghci:%04x:%04x",
	    (unsigned)device->pci_vendor,
	    (unsigned)device->pci_device);

	/* Succeeded: the window is GHCI 1.0. */
	return 0;
}

/*
 * Resets the GPU and waits for the reset to end and the memory to be
 * ready.  The wait covers the quiesce, which may take four watchdog
 * periods of whatever WDT_CYCLES a kernel before this one left.
 */
static int
reset_device(
	struct ghci_device *device)
{
	uint32_t gclk_hz;
	uint32_t wdt_cycles;
	uint32_t status;
	int error;

	/*
	 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 10, quiesce within 4 x WDT_CYCLES; Appendix A step 3, MEM_READY
	 * before the ring is set).
	 */
	gclk_hz = ghci_read(device, GHCI_REG_GCLK_HZ);
	wdt_cycles = ghci_read(device, GHCI_REG_WDT_CYCLES);
	device->reset_wait_ms = ghci_reset_wait_ms(gclk_hz, wdt_cycles);

	/* RESET is self-clearing and falls when the reset is done. */
	ghci_write(device, GHCI_REG_CTRL, GHCI_CTRL_RESET);
	error = ghci_wait_register(device, GHCI_REG_CTRL, GHCI_CTRL_RESET, 0U, device->reset_wait_ms);
	status = ghci_read(device, GHCI_REG_STATUS);
	if (error != 0 || (status & GHCI_STATUS_AXI_STUCK) != 0U) {
		kern_logf("ghci: %s: reset did not end (status %08x)\n", device->name, (unsigned)status);
		return EIO;
	}

	/* The memory must be ready before the ring is set. */
	error = ghci_wait_register(device, GHCI_REG_STATUS, GHCI_STATUS_MEM_READY, GHCI_STATUS_MEM_READY, GHCI_MEMORY_WAIT_MS);
	if (error != 0) {
		status = ghci_read(device, GHCI_REG_STATUS);
		kern_logf("ghci: %s: memory not ready (status %08x)\n", device->name, (unsigned)status);
		return EIO;
	}

	/* Succeeded: the GPU is idle with its memory ready. */
	return 0;
}

/* Writes two patterns to SCRATCH and reads each back. */
static int
check_scratch(
	struct ghci_device *device)
{
	uint32_t first;
	uint32_t second;

	/* Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 14, the SCRATCH check). */
	ghci_write(device, GHCI_REG_SCRATCH, GHCI_SCRATCH_FIRST);
	first = ghci_read(device, GHCI_REG_SCRATCH);
	ghci_write(device, GHCI_REG_SCRATCH, GHCI_SCRATCH_SECOND);
	second = ghci_read(device, GHCI_REG_SCRATCH);

	/* A window that does not keep what is written is not a working GPU. */
	if (first != GHCI_SCRATCH_FIRST || second != GHCI_SCRATCH_SECOND) {
		kern_logf("ghci: %s: SCRATCH reads %08x and %08x\n", device->name, (unsigned)first, (unsigned)second);
		return EIO;
	}

	/* Succeeded: the window answers. */
	return 0;
}

/*
 * Reads the capabilities from a copy of the first 1 KiB of the window:
 * the capability registers, the extended capability list and the blocks
 * it names.
 */
static int
read_caps(
	struct ghci_device *device)
{
	uint32_t *window;
	uint32_t offset;
	int error;

	/* A copy of the window the decode reads; reading a register has no side effect. */
	window = kern_calloc(GHCI_WINDOW_WORDS, sizeof(*window));
	if (window == NULL)
		return ENOMEM;

	/* Every word of the first 1 KiB, the window GHCI 1.0 uses. */
	for (offset = 0U; offset < GHCI_WINDOW_BYTES; offset += 4U)
		window[offset / 4U] = ghci_read(device, offset);

	/* The capabilities, with DEVICE_CAPS above its mirrors. */
	error = ghci_caps_decode(window, &device->caps);
	kern_free(window);
	if (error != 0) {
		kern_logf("ghci: %s: capabilities not read (%d)\n", device->name, error);
		return error;
	}

	/* A mirror that disagrees with DEVICE_CAPS or MAX_MRT is only logged. */
	if (device->caps.mismatches != 0U)
		kern_logf("ghci: %s: EXT_CAP_FEATURE or EXT_CAP_FMT0 disagrees with DEVICE_CAPS (bits %08x); DEVICE_CAPS used\n",
		    device->name,
		    (unsigned)device->caps.mismatches);
	if (device->caps.max_color != device->caps.max_mrt)
		kern_logf("ghci: %s: EXT_CAP_FMT0 says %u colour attachments, MAX_MRT %u; MAX_MRT used\n",
		    device->name,
		    (unsigned)device->caps.max_color,
		    (unsigned)device->caps.max_mrt);

	/* The address widths, the colour attachments and the shared memory. */
	kern_logf("ghci: %s: VA %u bits, PA %u bits, %u colour attachments, %u KiB of shared memory\n",
	    device->name,
	    (unsigned)device->caps.va_bits,
	    (unsigned)device->caps.pa_bits,
	    (unsigned)device->caps.max_mrt,
	    (unsigned)(device->caps.lds_bytes / 1024U));

	/* The GPU's own identification, when it has the VEND block. */
	if (device->caps.vendor[0] != 0U || device->caps.vendor[1] != 0U)
		kern_logf("ghci: %s: vendor %08x device %08x\n",
		    device->name,
		    (unsigned)device->caps.vendor[0],
		    (unsigned)device->caps.vendor[1]);

	/* Succeeded: the capabilities are read. */
	return 0;
}

/*
 * Decides whether the device gets the Vulkan executor, and says what
 * isolation its sessions have.
 */
static void
choose_policy(
	struct ghci_device *device)
{
	int walker;
	int protection;
	int shader;
	int units;

	/* The capabilities the decision reads. */
	walker = ghci_caps_has(&device->caps, GHCI_HAS_HW_WALKER);
	protection = ghci_caps_has(&device->caps, GHCI_HAS_PROT);
	shader = ghci_caps_has(&device->caps, GHCI_HAS_GSI1);
	units = ghci_caps_has(&device->caps, GHCI_HAS_SHADER_UNITS);

	/* A GPU without a shader unit or GSI1 runs the display only. */
	device->executor = 0;
	if (shader && units)
		device->executor = 1;
	if (!device->executor)
		kern_logf("ghci: %s: no shader unit or no GSI1; display only\n", device->name);

	/*
	 * Neither a GPU without a walker (every session reaches all of
	 * memory) nor one without PROT (the sessions share one address
	 * space) keeps the sessions apart; the executor is offered all the
	 * same, and the log says so.
	 */
	if (!walker)
		kern_logf("ghci: %s: GPU memory isolation: none (flat)\n", device->name);
	else if (!protection)
		kern_logf("ghci: %s: GPU memory isolation: shared VA (PROT=0)\n", device->name);
}

/* Fills the operations table and registers the device with the GPU core. */
static int
register_device(
	struct ghci_device *device)
{
	int error;

	/*
	 * The table every device has: the sessions, the information, the
	 * resources and blobs with their copies and mappings, and their
	 * sharing within the device.  No other capability is offered until
	 * its callbacks are all there.
	 */
	kern_memset(&device->ops, 0, sizeof(device->ops));
	device->ops.version = DRV_GPU_INTERFACE_VERSION;
	device->ops.size = sizeof(device->ops);
	device->ops.capabilities = GPU_CAP_RESOURCE | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_MAPPING |
	    GPU_CAP_SHARE | GPU_CAP_ALLOCATION_SHARE;
	device->ops.open = gpu_open;
	device->ops.close = gpu_close;
	device->ops.get_info = gpu_get_info;
	device->ops.resource_create = gpu_resource_create;
	device->ops.resource_destroy = gpu_resource_destroy;
	device->ops.blob_create = gpu_blob_create;
	device->ops.resource_read = gpu_resource_read;
	device->ops.resource_write = gpu_resource_write;
	device->ops.resource_map = gpu_resource_map;
	device->ops.share = &ghci_share_ops;

	/*
	 * The display controller's operations, on a device with DISP2: the
	 * display and its events, the cursor's three with the cursor plane,
	 * and the scan-out query.
	 */
	if (device->display.present) {
		device->ops.capabilities |= GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS;
		device->ops.display = &ghci_display_ops;
		if (device->display.cursor)
			device->ops.display = &ghci_display_cursor_ops;
		device->ops.scanout = &ghci_scanout_ops;
	}

	/* The Vulkan executor adds its operations and capabilities, on a device that runs shaders. */
	if (device->executor && ghci_vk_attach != NULL) {
		error = ghci_vk_attach(device);
		if (error != 0) {
			kern_logf("ghci: %s: no Vulkan executor (%d)\n", device->name, error);
			return error;
		}
	}

	/* Publishes the node. */
	error = drv_gpu_register(&device->ops, device, &device->gpu);
	if (error != 0) {
		kern_logf("ghci: %s: not registered (%d)\n", device->name, error);
		if (device->executor_ops != NULL)
			device->executor_ops->detach(device);
		return error;
	}

	/* Succeeded: /dev/gpuN is the device's. */
	return 0;
}

/* Opens a session of the device. */
static int
gpu_open(
	void *private_data,
	void **result)
{
	struct ghci_device *device;
	struct ghci_session *session;
	enum ghci_device_state state;
	unsigned long irq;
	int error;

	/* The session's record. */
	device = private_data;
	session = kern_calloc(1U, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* The device's life, as the interrupt and the recovery last set it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	state = device->state;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Refuses an open once the detach began or the hardware is lost, and lists the session. */
	mutex_lock(&device->lock);

	error = 0;
	if (device->withdrawing) {
		error = ENODEV;
	} else if (state == GHCI_STATE_LOST) {
		error = EIO;
	} else {
		session->device = device;
		session->next = device->sessions;
		device->sessions = session;
		device->session_count++;
	}

	mutex_unlock(&device->lock);

	/* An open that was refused leaves nothing behind. */
	if (error != 0) {
		kern_free(session);
		return error;
	}

	/* The executor's side of the open; one that fails takes the session off the list again. */
	if (device->executor_ops != NULL) {
		error = device->executor_ops->session_open(device, session);
		if (error != 0) {
			gpu_close(device, session);
			return error;
		}
	}

	/* Succeeded: the session is the caller's. */
	*result = session;
	return 0;
}

/*
 * Closes a session; nothing waits.  The core destroyed its resources
 * first; any left are let go here.
 */
static void
gpu_close(
	void *private_data,
	void *opaque)
{
	struct ghci_device *device;
	struct ghci_session *session;
	struct ghci_session **position;
	struct ghci_resource *resource;

	/* The device and the session. */
	device = private_data;
	session = opaque;

	/* The display lease ends first, so its frame is no longer scanned from the session's memory. */
	ghci_display_session_close(device, session);

	/* The executor's objects go: its devices' sessions close and its memory is let go. */
	if (device->executor_ops != NULL && session->vk != NULL)
		device->executor_ops->session_close(device, session);

	/* The session leaves the device's list, and its resources let their memory go. */
	mutex_lock(&device->lock);

	while (session->resources != NULL) {
		resource = session->resources;
		session->resources = resource->next;
		ghci_memory_unhold(device, resource->allocation);
		kern_free(resource);
	}

	position = &device->sessions;
	while (*position != NULL && *position != session)
		position = &(*position)->next;
	if (*position == session) {
		*position = session->next;
		device->session_count--;
	}

	mutex_unlock(&device->lock);

	/* The record goes. */
	kern_free(session);
}

/* Reports the device's capabilities, limits and name. */
static int
gpu_get_info(
	void *private_data,
	void *opaque,
	struct gpu_info *info)
{
	struct ghci_device *device;

	(void)opaque;

	/* The capability snapshot of the device. */
	device = private_data;
	info->capabilities = device->ops.capabilities;
	info->max_resources = RESOURCES_MAX;
	info->max_resource_bytes = RESOURCE_BYTES_MAX;
	kern_memset(info->driver_name, 0, sizeof(info->driver_name));
	kern_memcpy(info->driver_name, device->name, sizeof(device->name) - 1U);

	/* Succeeded: the snapshot is filled. */
	return 0;
}

/* Makes a storage resource: cleared host memory of the session's. */
static int
gpu_resource_create(
	void *private_data,
	void *opaque,
	const struct gpu_resource_create *request,
	void **result)
{
	struct ghci_resource *resource;
	int error;

	/* The memory and its record. */
	error = resource_new(private_data, opaque, request->bytes, 0, 0U, &resource);
	if (error != 0)
		return error;

	/* Succeeded: the resource is the session's. */
	*result = resource;
	return 0;
}

/* Destroys a resource: it lets its memory go, which goes back once nothing else holds it. */
static void
gpu_resource_destroy(
	void *private_data,
	void *opaque,
	void *object)
{
	struct ghci_device *device;
	struct ghci_session *session;
	struct ghci_resource *resource;
	struct ghci_resource **position;

	/* The device, the session and the resource. */
	device = private_data;
	session = opaque;
	resource = object;

	/* The resource leaves its session and lets its memory go. */
	mutex_lock(&device->lock);

	position = &session->resources;
	while (*position != NULL && *position != resource)
		position = &(*position)->next;
	if (*position == resource)
		*position = resource->next;
	ghci_memory_unhold(device, resource->allocation);

	mutex_unlock(&device->lock);

	/* The record goes. */
	kern_free(resource);
}

/*
 * Makes a blob: cleared host memory that the processor maps when the
 * blob is mappable and that the device's sessions share when it is
 * shareable.  Its resource id names it to the session's renderer.
 */
static int
gpu_blob_create(
	void *private_data,
	void *opaque,
	const struct gpu_blob_create *request,
	void **result,
	uint32_t *resource_id)
{
	struct ghci_resource *resource;
	int error;

	/* Mapping and sharing within the device; another device cannot import this memory. */
	if ((request->flags & ~BLOB_FLAGS) != 0U)
		return ENOTSUP;

	/* A blob of a VkDeviceMemory shows that memory; it needs the executor. */
	if (request->blob_id != 0U) {
		error = memory_blob(private_data, opaque, request, &resource);
		if (error != 0)
			return error;
		*result = resource;
		*resource_id = resource->resource_id;
		return 0;
	}

	/* The memory and its record. */
	error = resource_new(private_data, opaque, request->bytes, 1, request->flags, &resource);
	if (error != 0)
		return error;

	/* Succeeded: the blob is the session's. */
	*result = resource;
	*resource_id = resource->resource_id;
	return 0;
}

/* Copies bytes out of a resource of host memory; a client never reads local memory. */
static int
gpu_resource_read(
	void *private_data,
	void *opaque,
	void *object,
	uint64_t offset,
	void *buffer,
	uint32_t bytes)
{
	struct ghci_resource *resource;
	int error;

	(void)private_data;
	(void)opaque;

	/* A range inside the resource. */
	resource = object;
	error = resource_range(resource, offset, bytes);
	if (error != 0)
		return error;

	/* Local memory is not the client's to read: it is reached by copies on the GPU. */
	if (resource->allocation->kind != GHCI_MEMORY_HOST)
		return ENOTSUP;

	/* Succeeded: the bytes are copied from the processor's view. */
	kern_memcpy(buffer, resource->allocation->view + offset, bytes);
	return 0;
}

/* Copies bytes into a resource of host memory; a client never writes local memory. */
static int
gpu_resource_write(
	void *private_data,
	void *opaque,
	void *object,
	uint64_t offset,
	const void *buffer,
	uint32_t bytes)
{
	struct ghci_resource *resource;
	int error;

	(void)private_data;
	(void)opaque;

	/* A range inside the resource. */
	resource = object;
	error = resource_range(resource, offset, bytes);
	if (error != 0)
		return error;

	/* Local memory is not the client's to write: it is reached by copies on the GPU. */
	if (resource->allocation->kind != GHCI_MEMORY_HOST)
		return ENOTSUP;

	/* Succeeded: the bytes are copied into the processor's view. */
	kern_memcpy(resource->allocation->view + offset, buffer, bytes);
	return 0;
}

/*
 * Gives the processor's view of a resource for a user mapping: RAM,
 * uncached where the GPU does not snoop the processor's caches.  A blob
 * maps only when it was made mappable.  Local memory never maps: the
 * local window is for the driver's own uses alone.
 *
 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 11.1, the local window for the framebuffer only, and uploads and
 * readbacks of local memory made by copy commands).
 */
static int
gpu_resource_map(
	void *private_data,
	void *opaque,
	void *object,
	struct drv_gpu_mapping *mapping)
{
	struct ghci_device *device;
	struct ghci_resource *resource;

	(void)opaque;

	/* A blob not made mappable has no view to give. */
	device = private_data;
	resource = object;
	if (resource->blob && (resource->flags & GPU_BLOB_MAPPABLE) == 0U)
		return EINVAL;

	/* Local memory has no view a client may map. */
	if (resource->allocation->kind != GHCI_MEMORY_HOST)
		return ENOTSUP;

	/* Succeeded: the run and how the processor sees it. */
	mapping->physical = resource->allocation->bus;
	mapping->address = resource->allocation->view;
	mapping->bytes = resource->allocation->bytes;
	mapping->attributes = 0U;
	if (device->host_uncached)
		mapping->attributes = DRV_GPU_MAPPING_UNCACHED_RAM;
	return 0;
}

/* Makes the cleared host memory of a new resource of a session. */
static int
resource_new(
	struct ghci_device *device,
	struct ghci_session *session,
	uint64_t bytes,
	int blob,
	uint32_t flags,
	struct ghci_resource **result)
{
	struct ghci_allocation *allocation;
	int error;

	/* An empty or too large resource is refused. */
	if (bytes == 0U || bytes > RESOURCE_BYTES_MAX)
		return EINVAL;

	/* The memory, below the GPU's limit. */
	error = ghci_memory_alloc(device, bytes, 0U, &allocation);
	if (error != 0)
		return error;

	/* Its record, which holds it. */
	mutex_lock(&device->lock);

	error = ghci_resource_add(device, session, allocation, blob, flags, result);

	mutex_unlock(&device->lock);

	/* A record that could not be made gives the memory back. */
	if (error != 0) {
		mutex_lock(&device->lock);

		error = ghci_memory_free(device, allocation);
		if (error != 0)
			ghci_memory_quarantine(device, allocation);

		mutex_unlock(&device->lock);
		return ENOMEM;
	}

	/* Succeeded: the resource is the session's. */
	return 0;
}

/* Checks a range of a resource.  Returns 0, or EINVAL for one past its end. */
static int
resource_range(
	const struct ghci_resource *resource,
	uint64_t offset,
	uint32_t bytes)
{
	/* The range lies inside the memory. */
	if (offset > resource->allocation->bytes || bytes > resource->allocation->bytes - offset)
		return EINVAL;

	/* Succeeded. */
	return 0;
}

/*
 * Makes the blob of one of the session's VkDeviceMemory objects: a
 * resource that shows the object's memory and holds it.  Returns 0,
 * ENOTSUP on a device without the executor, ENOENT for a blob id that
 * names no memory of the session, EINVAL for a size past the memory's, or
 * ENOMEM.
 */
static int
memory_blob(
	struct ghci_device *device,
	struct ghci_session *session,
	const struct gpu_blob_create *request,
	struct ghci_resource **result)
{
	struct ghci_allocation *allocation;
	int error;

	/* Only the executor makes memory objects. */
	if (device->executor_ops == NULL || session->vk == NULL)
		return ENOTSUP;

	/* The memory the id names, and a resource of it, under the device's lock. */
	mutex_lock(&device->lock);

	allocation = device->executor_ops->memory_blob(device, session, request->blob_id);
	error = 0;
	if (allocation == NULL) {
		error = ENOENT;
	} else if (request->bytes == 0U || request->bytes > allocation->bytes) {
		error = EINVAL;
	} else {
		error = ghci_resource_add(device, session, allocation, 1, request->flags, result);
	}

	mutex_unlock(&device->lock);

	/* No memory of that id, or no record for the resource. */
	if (error != 0)
		return error;

	/* Succeeded: the resource shows the memory. */
	return 0;
}
