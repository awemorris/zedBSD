/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device start and stop.
 *
 * The hardware bring-up sleeps, waits on timers and takes interrupts, none of
 * which is safe in the boot device-probe context where PCI attach runs.  Attach
 * therefore only registers the device here, and the kernel reports separately
 * when regular threads and timed waits work.  Once both have happened -- in
 * either order -- one start worker per device runs the bring-up, which ends by
 * publishing the GPU node and serving its requests on the same thread.
 */

#include "i915.h"
#include "device.h"
#include "gt.h"
#include "ggtt.h"
#include "reset.h"
#include "submit.h"
#include "sync.h"
#include "worker.h"
#include "display/display.h"
#include <kern/kcrt.h>

#include <drivers/generic/dma.h>
#include <drivers/pci/pci-i915.h>
#include <drivers/pci/pci.h>
#include <hal/hal.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/gt-regs.h"
#include "intel/pci-ids.h"

/* The Alder Lake-P engines the GT is brought up with: RCS0, BCS0, VCS0, VCS2 and VECS0. */
#define I915_START_ENGINE_MASK	((1U << 0) | (1U << 1) | (1U << 8) | (1U << 10) | (1U << 16))

/* The GPU addresses 39 bits of DMA space (the reference's dma_mask_size). */
#define I915_START_DMA_MASK	((((uint64_t)1) << 39) - 1U)

/* Gen12 has 32 fence registers. */
#define I915_START_FENCES	32U

/* How many forcewake domains the running GT holds awake. */
#define I915_START_DOMAINS	5U

/* How many of the newest trace records a dump examines. */
#define I915_START_TRACE_DUMP	256U

/* The display's interrupt summary control (GEN11_DISPLAY_INT_CTL). */
#define I915_DISPLAY_INT_CTL	0x44200U

/* Turns an id-list entry into the bare product id. */
#define I915_ID_VALUE(product)	(product)

/*
 * The devices that attached and the kernel's readiness.
 *
 * Attach adds a device to the pending list; the readiness report sets ready.
 * Whichever comes second launches the start worker, and a launched device
 * leaves the list, so each device is started exactly once.
 */
struct i915_start_registry {
	/* Protects every other field; initialized on first use. */
	struct spinlock lock;

	/* Nonzero once the lock has been initialized. */
	unsigned lock_ready;

	/* Nonzero once the kernel has reported that timed waits work. */
	unsigned ready;

	/* The registered devices whose worker has not been launched yet. */
	struct i915_device *pending;
};

/*
 * The one start registry of the driver.
 *
 * It lives for the whole kernel lifetime.  Its zero value means no device has
 * registered and the kernel has not reported readiness yet.
 */
static struct i915_start_registry i915_start_registry;

/*
 * The forcewake domains the running GT holds, in the order they are taken.
 *
 * The media domains are included because the workaround list writes video
 * engine registers.  The table never changes.
 */
static const int i915_start_domains[I915_START_DOMAINS] = {
	I915_FORCEWAKE_RENDER,
	I915_FORCEWAKE_GT,
	I915_FORCEWAKE_MEDIA_VDBOX0,
	I915_FORCEWAKE_MEDIA_VDBOX2,
	I915_FORCEWAKE_MEDIA_VEBOX0
};

/*
 * The test checkpoint after the start.
 *
 * The test build defines it to run the scenario it was built for on the
 * started device, before the node is served.  It is weak so that a production
 * kernel links without it and the call site is a null test.
 */
extern void drv_i915_test_after_start(struct i915_device *device) __attribute__((weak));

static void i915_interim_display_irq_enable(void *context, int enabled);
static void i915_interim_display_irq_nothing(void *context);
static void i915_interim_display_irq_reset(void *context);
static void i915_interim_display_irq_handle(void *context, uint32_t master_ctl);

/*
 * The display interrupt hooks while the display is not connected.
 *
 * The reset keeps the display's interrupt summary off, so a display source
 * the firmware left enabled cannot reach the vector; every other hook does
 * nothing.  The table never changes.
 */
static const struct i915_irq_display_ops i915_interim_display_irq_ops = {
	i915_interim_display_irq_enable,
	i915_interim_display_irq_nothing,
	i915_interim_display_irq_reset,
	i915_interim_display_irq_nothing,
	i915_interim_display_irq_handle,
	i915_interim_display_irq_nothing
};

static void i915_start_registry_init(void);
static void i915_start_launch(void);
static void i915_start_worker(void *argument);
static int i915_start_prepare(struct i915_device *device);
static int i915_start_display_noirq(struct i915_device *device);
static int i915_start_display_nogem(struct i915_device *device);
static int i915_start_display_register(struct i915_device *device);
static int i915_start_registers(struct i915_device *device);
static int i915_start_gt_info(struct i915_device *device);
static int i915_start_memory(struct i915_device *device);
static int i915_start_ggtt(struct i915_device *device);
static int i915_start_aperture(struct i915_device *device);
static int i915_start_interrupts(struct i915_device *device);
static int i915_start_gt(struct i915_device *device);
static int i915_start_gt_awake(struct i915_device *device);
static int i915_start_gt_memory(struct i915_device *device);
static void i915_start_pxp(struct i915_device *device);
static int i915_start_publish(struct i915_device *device);
static void i915_stop_gt(struct i915_device *device);
static void i915_stop_hardware(struct i915_device *device);
static int i915_forcewake_get_all(struct i915_gt *gt, unsigned *held);
static void i915_forcewake_put_all(struct i915_gt *gt, unsigned held);
static void i915_start_dump_trace(struct i915_gt *gt);
static int i915_product_is_tigerlake(uint16_t product);
static unsigned i915_popcount32(uint32_t value);
static unsigned i915_cpu_physical_bits(void);

/*
 * Records that the kernel can run the device bring-up.
 *
 * Called once from the boot path after regular threads, timer wakeups and the
 * VFS are up.  It launches a start worker for every device that already
 * attached and returns without waiting for any of them.
 */
void
drv_i915_runtime_ready(void)
{
	/* Makes sure the registry lock exists before it is taken. */
	i915_start_registry_init();

	/* Marks the kernel ready for every current and later device. */
	spin_lock(&i915_start_registry.lock);

	i915_start_registry.ready = 1U;

	spin_unlock(&i915_start_registry.lock);

	/* Starts the devices that attached before readiness. */
	i915_start_launch();
}

/*
 * Registers an attached device for a deferred start.
 *
 * The device is started by a worker once the kernel has reported readiness;
 * if it already has, the worker is launched now.
 */
void
drv_i915_device_schedule_start(
	struct i915_device *device)
{
	/* Makes sure the registry lock exists before it is taken. */
	i915_start_registry_init();

	/* Queues the device for its start worker. */
	spin_lock(&i915_start_registry.lock);

	device->start_next = i915_start_registry.pending;
	i915_start_registry.pending = device;

	spin_unlock(&i915_start_registry.lock);

	kern_logf("i915: device 8086:%04x registered; the start waits for kernel readiness\n",
	    (unsigned)device->product);

	/* Starts the device now when readiness came first. */
	i915_start_launch();
}

/*
 * Brings the hardware up and publishes the GPU node.
 *
 * Runs on the device's start worker.  The steps follow the order of the
 * reference's i915_driver_probe(): PCI and runtime PM, the register block and
 * the GT fuses with a full GT reset, DMA, the GGTT and the display's data
 * sources, the display noirq probe, interrupts, the display nogem probe, the
 * GT (workarounds, memory, engines, default contexts), the protected-content
 * context, then the display probe and the driver registration.  A step that
 * fails names itself in device->stage; what the steps before it acquired is
 * given back by the device stop.
 */
int
drv_i915_device_start(
	struct i915_device *device)
{
	struct i915_gt *gt;
	int error;

	gt = &device->gt;

	/* Prepares the trace, the locks and PCI access, and resumes the device. */
	error = i915_start_prepare(device);
	if (error != 0)
		return error;

	/* Maps the registers, reads the GT fuses and resets the GT. */
	error = i915_start_registers(device);
	if (error != 0)
		return error;

	/* Sets up 39-bit DMA, the GGTT, the aperture, bus mastering, the MSI vector and the display's data sources. */
	error = i915_start_memory(device);
	if (error != 0)
		return error;

	/* Brings the display up to the point before interrupts (intel_display_driver_probe_noirq()). */
	error = i915_start_display_noirq(device);
	if (error != 0)
		return error;

	/* Installs the interrupt handler with the display half bound. */
	error = i915_start_interrupts(device);
	if (error != 0)
		return error;

	/* Builds the outputs and reads back and sanitizes what the firmware left (intel_display_driver_probe_nogem()). */
	error = i915_start_display_nogem(device);
	if (error != 0)
		return error;

	/* Programs the GT and records the default contexts. */
	error = i915_start_gt(device);
	if (error != 0)
		return error;

	/* Creates the protected-content context; its failure never stops the start. */
	i915_start_pxp(device);

	/* Probes the display and registers the driver (intel_display_driver_probe(), i915_driver_register()). */
	error = i915_start_display_register(device);
	if (error != 0)
		return error;

	/* Dumps what the start did. */
	i915_start_dump_trace(gt);

	/* Keeps the GT awake, publishes the node and serves it until the device stops. */
	error = i915_start_publish(device);
	if (error != 0)
		return error;

	/* Succeeded: the GPU node was published and served. */
	return 0;
}

/*
 * Releases what the device start acquired, in reverse order.
 *
 * The engines are stopped and reset before the objects they point at are
 * released.  A lease that cannot be given back safely is kept and reported.
 */
int
drv_i915_device_stop(
	struct i915_device *device)
{
	struct i915_device **link;
	struct i915_gt *gt;
	unsigned launched;

	gt = &device->gt;

	/* Makes sure the registry lock exists before it is taken. */
	i915_start_registry_init();

	/* Withdraws the device from the start list unless its worker was launched. */
	spin_lock(&i915_start_registry.lock);

	launched = device->start_launched;

	/* Finds the link that names the device in the pending list. */
	link = &i915_start_registry.pending;
	while (*link != NULL && *link != device)
		link = &(*link)->start_next;

	/* Unlinks a device that is still pending. */
	if (*link == device) {
		*link = device->start_next;
		device->start_next = NULL;
	}

	spin_unlock(&i915_start_registry.lock);

	/*
	 * XXX: a launched start may still be running; nothing tells the stop
	 * when it has returned, so the device is kept.
	 */
	if (launched != 0U && device->start_returned == 0U)
		return EBUSY;

	/* Stops the request worker and frees its state. */
	if (device->worker != NULL) {
		drv_i915_worker_stop(device);
		drv_i915_worker_destroy(device);
	}

	/* XXX: session objects left by closed or quarantined sessions are not freed yet. */

	/* Stops the display's use of the hardware, the GT, and gives back the GT's objects. */
	i915_stop_gt(device);

	/* Releases the panel connector and the display records before the interrupts go. */
	drv_i915_display_stop_outputs(device);

	/* Gives back interrupts, the display, DMA, the GGTT and the register block. */
	i915_stop_hardware(device);

	/* Dumps what the stop did. */
	i915_start_dump_trace(gt);

	/* Succeeded: no hardware lease remains owned. */
	return 0;
}

/* Initializes the registry lock the first time the registry is used. */
static void
i915_start_registry_init(void)
{
	/*
	 * Attach and the readiness report both run on the boot thread before
	 * any worker exists, so the first use cannot race with another.
	 */
	if (i915_start_registry.lock_ready != 0U)
		return;

	spin_init(&i915_start_registry.lock, LOCK_RANK_DEVICE, "i915 start");
	i915_start_registry.lock_ready = 1U;
}

/* Launches a start worker for every pending device once the kernel is ready. */
static void
i915_start_launch(void)
{
	struct i915_device *device;
	struct thread *thread;
	int error;

	/* Hands the pending devices to workers one at a time. */
	for (;;) {
		/* Takes the next device off the list, only once the kernel is ready. */
		spin_lock(&i915_start_registry.lock);

		device = NULL;
		if (i915_start_registry.ready != 0U && i915_start_registry.pending != NULL) {
			device = i915_start_registry.pending;
			i915_start_registry.pending = device->start_next;
			device->start_next = NULL;
			device->start_launched = 1U;
		}

		spin_unlock(&i915_start_registry.lock);

		/* Nothing is left to launch, or the kernel is not ready yet. */
		if (device == NULL)
			return;

		/* Creates the worker that runs this device's bring-up. */
		error = kthread_create(i915_start_worker, device, SCHED_PRIORITY_DEFAULT, &thread);
		if (error != 0) {
			kern_logf("i915: device 8086:%04x: the start worker could not be created: %d\n",
			    (unsigned)device->product,
			    error);
			continue;
		}

		/* The worker reclaims itself when the bring-up returns. */
		thread->detached = 1U;
		thread_start(thread);
	}
}

/* Runs one device's bring-up on its own worker. */
static void
i915_start_worker(
	void *argument)
{
	struct i915_device *device;
	int error;

	/* The worker was created for exactly this device. */
	device = argument;

	/* Brings the device up; a failure names the step it stopped at. */
	error = drv_i915_device_start(device);

	/* Tells the device stop that the start no longer runs. */
	spin_lock(&i915_start_registry.lock);

	device->start_returned = 1U;

	spin_unlock(&i915_start_registry.lock);

	/* Reports where a failed start stopped. */
	if (error != 0) {
		kern_logf("i915: device 8086:%04x start stopped at %s: %d\n",
		    (unsigned)device->product,
		    device->stage,
		    error);
		return;
	}

	kern_logf("i915: device 8086:%04x started\n", (unsigned)device->product);
}

/* Prepares the trace, the locks and PCI access, and resumes the device to D0. */
static int
i915_start_prepare(
	struct i915_device *device)
{
	struct i915_gt *gt;
	uint16_t product;
	int available;
	int error;

	gt = &device->gt;

	/* Starts an empty trace and the device-owned locks. */
	drv_i915_trace_init(&gt->trace);
	spin_init(&gt->uncore_lock, LOCK_RANK_DEVICE, "i915 uncore");
	(void)mutex_init(&gt->sb_lock, LOCK_RANK_DEVICE, "i915 sideband");

	/* Refuses a kernel whose waits have no monotonic counter to measure time with. */
	device->stage = "time_base_anomaly";
	available = drv_i915_time_base_ok();
	if (available == 0)
		return EIO;

	/* Binds PCI configuration access; the MSI vector is not allocated yet. */
	gt->pci_context.pci = device->pci;
	gt->pci_context.msi_irq = -1;
	drv_i915_pci_init(&gt->pci, drv_i915_pci_device_ops(), &gt->pci_context, &gt->trace);

	/* Tells the display version from the device id, as the PCI driver matches it. */
	product = drv_i915_pci_read16(&gt->pci, 0x02U);
	gt->display_ver = 13U;
	gt->is_alderlake_p = 1;
	if (i915_product_is_tigerlake(product) != 0) {
		gt->display_ver = 12U;
		gt->is_alderlake_p = 0;
	}

	kern_logf("i915: display device: 8086:%04x -> display version %u, %s\n",
	    (unsigned)product,
	    gt->display_ver,
	    gt->is_alderlake_p != 0 ? "Alder Lake-P class (XE_LPD)" : "Tiger Lake");

	/*
	 * Takes the PCI core's runtime PM hold and resumes the device to D0
	 * before any driver work.  The resume leaves the hold taken even when it
	 * fails, so the stop always gives it back.
	 */
	drv_i915_rpm_init_early(&gt->probe_pm, drv_i915_rpm_pci_probe_ops(), &gt->pci, &gt->trace);
	error = drv_i915_rpm_get_sync(&gt->probe_pm);
	gt->probe_pm_held = 1U;
	kern_logf("i915: PM probe get_sync: cfg_vendor=0x%04x usage=%d active=%d resume_rc=%d\n",
	    (unsigned)drv_i915_pci_read16(&gt->pci, 0x00U),
	    drv_i915_rpm_usage(&gt->probe_pm),
	    drv_i915_rpm_active(&gt->probe_pm),
	    error);

	/* Reports a device that did not resume. */
	device->stage = "pci_probe_runtime_pm";
	if (error != 0)
		return error;

	/* Succeeded: the device is in D0 and its configuration space answers. */
	return 0;
}

/*
 * Runs the display noirq probe; a start without a display skips it.
 *
 * A firmware display check that stops leaves the device without a display
 * and the start continues.
 */
static int
i915_start_display_noirq(
	struct i915_device *device)
{
	int error;

	/* The DRM device, the VBT, the power domains, the firmware check, the display core, DMC and software state. */
	error = drv_i915_display_init_noirq(device);
	if (error != 0)
		return error;

	/* Succeeded: intel_display_driver_probe_noirq() returned 0. */
	return 0;
}

/* Runs the display survey and nogem probe; a start without a display skips it. */
static int
i915_start_display_nogem(
	struct i915_device *device)
{
	int error;

	/* The survey, the output setup, the readout and the sanitize. */
	error = drv_i915_display_init_nogem(device);
	if (error != 0)
		return error;

	/* Succeeded: intel_display_driver_probe_nogem() returned 0. */
	return 0;
}

/* Runs the display probe and the driver registration; a start without a display skips it. */
static int
i915_start_display_register(
	struct i915_device *device)
{
	int error;

	/* The display probe, the hotplug path, the OpRegion and driver registration. */
	error = drv_i915_display_register(device);
	if (error != 0)
		return error;

	/* Succeeded: the driver is registered. */
	return 0;
}

/* Maps the registers, reads the GT fuses and resets the whole GT. */
static int
i915_start_registers(
	struct i915_device *device)
{
	const struct i915_mmio_range *ranges;
	struct drv_pci_bar bar;
	struct i915_gt *gt;
	unsigned range_count;
	uint32_t slice;
	uint32_t dss;
	int error;

	gt = &device->gt;

	/* Enables the device's memory decoding; the command word is restored at stop. */
	device->stage = "pci_enable_device";
	error = drv_i915_pci_enable_device(&gt->pci);
	if (error != 0)
		return error;

	gt->pci_enabled = 1U;
	kern_logf("i915: pci_enable_device ok (command=0x%04x)\n",
	    (unsigned)drv_i915_pci_read16(&gt->pci, I915_PCI_COMMAND));

	/* Prepares the device's runtime PM record; it is never enabled. */
	drv_i915_rpm_init_early(&gt->rpm, drv_i915_rpm_device_ops(), device, &gt->trace);

	/* Claims BAR0 and maps its lower half, the registers, uncached. */
	device->stage = "pci_bar";
	error = drv_pci_device_bar(device->pci, GEN4_GTTMMADR_BAR, &bar);
	if (error != 0)
		return error;

	device->stage = "claim_bar";
	error = drv_pci_device_claim_bar(device->pci, GEN4_GTTMMADR_BAR);
	if (error != 0)
		return error;

	gt->bar_claimed = 1U;

	device->stage = "map_bar";
	error = drv_pci_device_map_bar_region(
		device->pci,
		GEN4_GTTMMADR_BAR,
		0U,
		(size_t)(bar.size / 2U),
		DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
		&gt->regs);
	if (error != 0)
		return error;

	gt->regs_mapped = 1U;

	/* Binds register access and the Gen12 forcewake map to the mapping. */
	gt->regs_window.base = gt->regs.address;
	gt->regs_window.size = (unsigned long)gt->regs.size;
	ranges = drv_i915_mmio_gen12_ranges(&range_count);
	drv_i915_mmio_init(&gt->mmio, drv_i915_mmio_window_ops(), &gt->regs_window, ranges, range_count, &gt->trace);
	kern_logf("i915: uncore BAR mapped (%llu bytes)\n", (unsigned long long)gt->regs.size);

	/* Reads the slice and DSS fuses, which sleep with the GT domain. */
	device->stage = "forcewake_gt";
	error = drv_i915_forcewake_get(&gt->mmio, I915_FORCEWAKE_GT);
	if (error != 0)
		return error;

	slice = drv_i915_read32(&gt->mmio, 0x9138U);
	dss = drv_i915_read32(&gt->mmio, 0x913cU);
	(void)drv_i915_forcewake_put(&gt->mmio, I915_FORCEWAKE_GT);
	kern_logf("i915: device_info: slice_fuse=0x%08x(%u) dss_fuse=0x%08x(%u DSS)\n",
	    slice,
	    i915_popcount32(slice & 0xffU),
	    dss,
	    i915_popcount32(dss));

	/* Points the replicated-register selector at multicast (intel_gt_init_mmio). */
	drv_i915_write32(&gt->mmio, 0x0fdcU, 0x80000000U);
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_NOTE, "intel_gt_init_mmio:mcr_multicast", 0U, 0U);

	/* Reads the clock, the SSEU, the steering mask and the engine set, which need GT and render awake. */
	error = i915_start_gt_info(device);
	if (error != 0)
		return error;

	/* Resets every engine, discarding whatever the firmware left running. */
	device->stage = "gt_reset";
	error = drv_i915_gt_reset_all(&gt->uncore_lock, &gt->mmio, I915_GT_RESET_ACK_US);
	if (error != 0)
		return error;

	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_ACQUIRE, "sanitize_gpu:__intel_gt_reset", 1U, 0U);
	kern_logf("i915: sanitize_gpu __intel_gt_reset(ALL_ENGINES) ok\n");

	/* Succeeded: registers answer and the GT is reset. */
	return 0;
}

/* Reads the timestamp clock, the SSEU, the steering mask and the engine set. */
static int
i915_start_gt_info(
	struct i915_device *device)
{
	struct i915_gt *gt;
	unsigned index;
	int error;

	gt = &device->gt;

	/* Wakes GT and render: the fuse and clock registers read all-ones while asleep. */
	device->stage = "forcewake_gt_init_mmio";
	error = drv_i915_forcewake_get(&gt->mmio, I915_FORCEWAKE_GT);
	if (error != 0)
		return error;

	error = drv_i915_forcewake_get(&gt->mmio, I915_FORCEWAKE_RENDER);
	if (error != 0) {
		(void)drv_i915_forcewake_put(&gt->mmio, I915_FORCEWAKE_GT);
		return error;
	}

	/* Decodes the fuses for the Alder Lake-P engine set RCS0|BCS0|VCS0|VCS2|VECS0. */
	(void)drv_i915_gt_init_mmio(&gt->info, 12, I915_START_ENGINE_MASK, &gt->mmio);
	(void)drv_i915_forcewake_put(&gt->mmio, I915_FORCEWAKE_RENDER);
	(void)drv_i915_forcewake_put(&gt->mmio, I915_FORCEWAKE_GT);

	kern_logf("i915: gt_init_mmio: clock=%uHz period=%uns sseu(slice=0x%x dss=0x%x eu/ss=%u total=%u) l3bank=0x%x engine_mask=0x%x engines=%u fault=0x%x\n",
	    gt->info.clock_frequency,
	    gt->info.clock_period_ns,
	    gt->info.sseu.slice_mask,
	    gt->info.sseu.subslice_mask,
	    gt->info.sseu.eu_per_subslice,
	    gt->info.sseu.eu_total,
	    gt->info.l3bank_mask,
	    gt->info.engine_mask,
	    gt->info.num_engines,
	    gt->info.fault_reg);

	/* Names every engine the fuses left. */
	for (index = 0U; index < gt->info.num_engines; index++) {
		kern_logf("i915: engine[%u] %s: class=%d inst=%d base=0x%05x reset_domain=0x%x ctx_size=%u caps=0x%x\n",
		    index,
		    gt->info.engines[index].name,
		    gt->info.engines[index].class,
		    gt->info.engines[index].instance,
		    gt->info.engines[index].mmio_base,
		    gt->info.engines[index].reset_domain,
		    gt->info.engines[index].context_size,
		    gt->info.engines[index].uabi_capabilities);
	}

	/* Succeeded: the GT information is known. */
	return 0;
}

/* Sets up 39-bit DMA, the GGTT, the aperture, bus mastering and the MSI vector. */
static int
i915_start_memory(
	struct i915_device *device)
{
	static const struct drv_dma_constraints constraints = {
		39U,
		0xffffffffU,
		0U,
		1
	};
	struct i915_gt *gt;
	int error;

	gt = &device->gt;

	/*
	 * Declares the GPU's own 39-bit DMA capability (dma_set_mask).  The PCI
	 * bus default is a conservative 32 bits, which would bounce every page
	 * the GPU addresses above 4 GiB.
	 */
	device->stage = "dma_device_create";
	error = drv_dma_device_create(&constraints, &gt->dma_device);
	if (error != 0)
		return error;

	gt->dma_created = 1U;

	/* Records the requested mask and segment size in the DMA bookkeeping. */
	drv_i915_dma_init(&gt->dma, drv_i915_dma_device_ops(), gt->dma_device, &gt->trace);
	device->stage = "set_dma_info";
	error = drv_i915_dma_set_info(&gt->dma, 39U, 0xffffffffU);
	if (error != 0)
		return error;

	kern_logf("i915: dma_request: streaming_mask=0x%llx coherent_mask=0x%llx max_seg=0x%x\n",
	    (unsigned long long)I915_START_DMA_MASK,
	    (unsigned long long)I915_START_DMA_MASK,
	    0xffffffffU);

	/* Sizes and maps the GGTT table, and makes the scratch page every free entry points at. */
	error = i915_start_ggtt(device);
	if (error != 0)
		return error;

	/* Maps the CPU's view of the aperture and clears the fence registers. */
	error = i915_start_aperture(device);
	if (error != 0)
		return error;

	/* Enables bus mastering, only after the GGTT exists (pci_set_master). */
	drv_i915_pci_set_bus_master(&gt->pci, 1);
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_ACQUIRE, "pci_set_master", drv_i915_pci_read16(&gt->pci, I915_PCI_COMMAND), 0U);
	kern_logf("i915: pci_set_master ok (command=0x%04x)\n", (unsigned)drv_i915_pci_read16(&gt->pci, I915_PCI_COMMAND));

	/* Allocates and programs the MSI vector; its handler is attached with the interrupts. */
	device->stage = "pci_enable_msi";
	error = drv_i915_pci_setup_msi(&gt->pci);
	if (error != 0) {
		kern_logf("i915: pci_enable_msi failed rc=%d\n", error);
		return error;
	}

	gt->msi_kept = 1U;
	kern_logf("i915: pci_enable_msi ok (vector programmed, msi_enabled=%d, handler unattached)\n",
	    drv_i915_pci_msi_enabled(&gt->pci));

	/* Creates the display and its Linux environments; a failure stops the start. */
	device->stage = "display_create";
	error = drv_i915_display_create(device);
	if (error != 0)
		return error;

	/* The end of the hardware probe: the OpRegion and its VBT, the DRAM and the display bandwidth. */
	drv_i915_display_init_opregion(device);

	/* Succeeded: DMA, the GGTT, the MSI vector and the display's data sources are ready. */
	return 0;
}

/* Sizes and maps the GGTT table and makes the scratch page. */
static int
i915_start_ggtt(
	struct i915_device *device)
{
	struct drv_dma_segment segment;
	struct drv_pci_bar bar;
	struct i915_gt *gt;
	uint64_t table_bytes;
	uint64_t window_bytes;
	uint16_t gmch;
	unsigned ggms;
	unsigned segments;
	void *scratch;
	int encoded;
	int error;

	gt = &device->gt;

	/* Reads the GGTT size from the graphics control word (BDW_GMCH_GGMS). */
	gmch = drv_i915_pci_read16(&gt->pci, 0x50U);
	ggms = ((unsigned)gmch >> 6) & 0x3U;
	device->stage = "ggtt_disabled";
	if (ggms == 0U)
		return ENODEV;

	/* The table fills the upper half of BAR0, at most. */
	table_bytes = (uint64_t)(1U << ggms) << 20;
	device->stage = "ggtt_bar";
	error = drv_pci_device_bar(device->pci, GEN4_GTTMMADR_BAR, &bar);
	if (error != 0)
		return error;

	window_bytes = bar.size / 2U;
	if (table_bytes > window_bytes)
		table_bytes = window_bytes;

	/* Maps the table uncached. */
	device->stage = "ggtt_map";
	error = drv_pci_device_map_bar_region(
		device->pci,
		GEN4_GTTMMADR_BAR,
		window_bytes,
		(size_t)table_bytes,
		DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
		&gt->gtt);
	if (error != 0)
		return error;

	gt->gtt_mapped = 1U;
	gt->ggtt_entries = (unsigned)(table_bytes / 8U);
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_ACQUIRE, "ggtt_table_window", table_bytes, (uint64_t)gt->ggtt_entries);
	kern_logf("i915: ggtt_probe: ggms=%u entries=%u (%llu MiB GPU VA window)\n",
	    ggms,
	    gt->ggtt_entries,
	    (unsigned long long)((uint64_t)gt->ggtt_entries * I915_GT_PAGE_BYTES >> 20));

	/*
	 * Allocates the scratch page as an ordinary DMA-mapped page; its DMA
	 * address encodes the scratch entry.  The table is not filled here.
	 */
	device->stage = "scratch_alloc";
	error = drv_dma_vector_create(gt->dma_device, I915_GT_PAGE_BYTES, &gt->scratch);
	if (error != 0)
		return error;

	gt->scratch_created = 1U;

	/* One page must map as one contiguous segment. */
	scratch = drv_dma_vector_address(gt->scratch);
	segments = drv_dma_vector_count(gt->scratch);
	device->stage = "scratch_layout";
	if (scratch == NULL || segments != 1U)
		return EINVAL;

	/* Zeroes the page and finds its DMA address. */
	kern_memset(scratch, 0, I915_GT_PAGE_BYTES);
	device->stage = "scratch_segment";
	error = drv_dma_vector_segment(gt->scratch, 0U, &segment);
	if (error != 0)
		return error;

	/* Encodes the GGTT entry, refusing an address the GPU cannot reach rather than masking it. */
	encoded = drv_i915_ggtt_pte_encode(drv_i915_dma_addr(segment.address), (uint64_t)segment.length, I915_START_DMA_MASK, &gt->scratch_pte);
	device->stage = "scratch_pte_range";
	if (encoded == 0)
		return EINVAL;

	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_MAP, "scratch_page", segment.address, gt->scratch_pte);
	kern_logf("i915: scratch: cpu=0x%llx dma=0x%llx len=0x%llx ggtt_pte=0x%llx\n",
	    (unsigned long long)(uintptr_t)scratch,
	    (unsigned long long)segment.address,
	    (unsigned long long)segment.length,
	    (unsigned long long)gt->scratch_pte);

	/* Succeeded: the table is mapped and the scratch entry is known. */
	return 0;
}

/* Maps the CPU's write-combining view of the aperture and clears the fence registers. */
static int
i915_start_aperture(
	struct i915_device *device)
{
	struct drv_pci_bar aperture;
	struct i915_gt *gt;
	uint64_t limit;
	unsigned physical_bits;
	unsigned fence;
	void *view;
	int error;

	gt = &device->gt;

	/* Records the GGTT address space (i915_address_space_init). */
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_NOTE, "address_space_init:GGTT", (uint64_t)gt->gtt.size * 512U, 0U);

	/* A device without a GMADR BAR has no aperture to map. */
	error = drv_pci_device_bar(device->pci, GEN4_GMADR_BAR, &aperture);
	if (error == 0 && aperture.size != 0U) {
		/* Checks that the aperture lies within the CPU's physical address range. */
		physical_bits = i915_cpu_physical_bits();
		limit = ~(uint64_t)0;
		if (physical_bits < 64U)
			limit = (((uint64_t)1) << physical_bits) - 1U;

		gt->gmadr_base = aperture.bus_address;
		gt->gmadr_size = aperture.size;
		kern_logf("i915: ggtt_init_hw: gmadr=0x%llx mappable_end=0x%llx\n",
		    (unsigned long long)gt->gmadr_base,
		    (unsigned long long)gt->gmadr_size);

		device->stage = "ggtt_aperture_range";
		if (gt->gmadr_base > limit)
			return EINVAL;
		if (gt->gmadr_size - 1U > limit - gt->gmadr_base)
			return EINVAL;

		/*
		 * Maps the whole aperture write-combining for the device's
		 * lifetime (io_mapping_init_wc).  Only page tables back the view.
		 */
		drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_ACQUIRE, "ggtt_aperture_gmadr", gt->gmadr_base, gt->gmadr_size);
		view = NULL;
		error = hal_space_map_device((hal_physaddr_t)gt->gmadr_base, (size_t)gt->gmadr_size, HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_WC, &view);
		device->stage = "ggtt_aperture_wc";
		if (error != HAL_OK || view == NULL) {
			kern_logf("i915: ggtt_init_hw: WC aperture map(base=0x%llx size=0x%llx) failed rc=%d\n",
			    (unsigned long long)gt->gmadr_base,
			    (unsigned long long)gt->gmadr_size,
			    error);
			return EIO;
		}

		gt->aperture = view;
		kern_logf("i915: ggtt_init_hw: WC aperture mapped base=0x%llx size=0x%llx va=%p attr=RW|WC\n",
		    (unsigned long long)gt->gmadr_base,
		    (unsigned long long)gt->gmadr_size,
		    view);
	}

	/* Clears the 32 fence registers with the GT domain awake (intel_ggtt_init_fences). */
	device->stage = "forcewake_fences";
	error = drv_i915_forcewake_get(&gt->mmio, I915_FORCEWAKE_GT);
	if (error != 0)
		return error;

	for (fence = 0U; fence < I915_START_FENCES; fence++) {
		drv_i915_raw_write32(&gt->mmio, 0x100000U + fence * 8U, 0U);
		drv_i915_raw_write32(&gt->mmio, 0x100000U + fence * 8U + 4U, 0U);
	}

	(void)drv_i915_forcewake_put(&gt->mmio, I915_FORCEWAKE_GT);
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_NOTE, "intel_ggtt_init_fences", I915_START_FENCES, 0U);
	kern_logf("i915: ggtt_init_hw: %u fence registers initialized\n", I915_START_FENCES);

	/* Succeeded: the aperture view exists and the fences are clear. */
	return 0;
}

/* Attaches the interrupt handler and enables the GT interrupts. */
static int
i915_start_interrupts(
	struct i915_device *device)
{
	struct i915_gt *gt;
	int enabled;
	int error;

	gt = &device->gt;

	/* The handler needs the vector programmed with the memory setup. */
	enabled = drv_i915_pci_msi_enabled(&gt->pci);
	device->stage = "intel_irq_install";
	if (gt->msi_kept == 0U || enabled == 0) {
		kern_logf("i915: intel_irq_install: no MSI vector retained (msi_kept=%u) -- cannot attach a handler\n", gt->msi_kept);
		return ENODEV;
	}

	/* Describes the interrupt device: the registers, the engines and the vector. */
	gt->irq.m = &gt->mmio;
	gt->irq.gt = &gt->info;
	gt->irq.submission = I915_SUBMISSION_EXECLISTS;
	gt->irq.msi_irq = gt->pci_context.msi_irq;

	/*
	 * Without a display the interim hooks only keep the display's
	 * interrupt summary switched off, so a source the firmware left enabled
	 * cannot storm the vector.  A display detects its PCH and binds its own
	 * half in their place.
	 */
	gt->irq.display_ops = &i915_interim_display_irq_ops;
	gt->irq.display_context = &gt->mmio;
	drv_i915_display_irq_prepare(device);

	/* Resets every source, attaches the handler and enables the GT sources. */
	error = drv_i915_irq_install(&gt->irq);
	kern_logf("i915: intel_irq_install: rc=%d msi_irq=%d attached=%d gt_irqs=0x%x dmask=0x%x smask=0x%x reset_writes=%u post_writes=%u\n",
	    error,
	    gt->irq.msi_irq,
	    gt->irq.handler_attached,
	    gt->irq.gt_irqs,
	    gt->irq.gt_dmask,
	    gt->irq.gt_smask,
	    gt->irq.reset_writes,
	    gt->irq.postinstall_writes);
	drv_i915_display_irq_log(device);
	if (error != 0)
		return error;

	gt->irq_installed = 1U;

	/* Gives the device a brief window to show that its vector reaches the handler. */
	kern_usleep_range(20000U, 30000U);
	kern_logf("i915: irq observed: count=%u handled=%u none=%u gt=%u display=%u last_master_ctl=0x%x gu_misc_iir=0x%x\n",
	    gt->irq.irq_count,
	    gt->irq.irq_handled_count,
	    gt->irq.irq_none_count,
	    gt->irq.gt_irq_count,
	    gt->irq.display_irq_count,
	    gt->irq.last_master_ctl,
	    gt->irq.last_gu_misc_iir);

	/* Succeeded: interrupts are delivered. */
	return 0;
}

/*
 * Builds and programs the GT (i915_gem_init -> intel_gt_init).
 *
 * The reference holds every forcewake domain across the whole of it; on
 * Gen12 that includes the media domains, because the workaround list writes
 * video engine registers.
 */
static int
i915_start_gt(
	struct i915_device *device)
{
	struct i915_gt *gt;
	unsigned held;
	int error;

	gt = &device->gt;

	/* Wakes every domain the GT programming reaches. */
	device->stage = "forcewake_gt_init";
	error = i915_forcewake_get_all(gt, &held);
	if (error != 0) {
		i915_forcewake_put_all(gt, held);
		return error;
	}

	/* Programs the GT with every domain awake. */
	error = i915_start_gt_awake(device);

	/* Lets the domains sleep again. */
	i915_forcewake_put_all(gt, held);

	/* Reports the step the GT programming stopped at. */
	if (error != 0)
		return error;

	/* Succeeded: the GT is initialized (intel_gt_init). */
	return 0;
}

/* Programs the GT while the caller holds every forcewake domain. */
static int
i915_start_gt_awake(
	struct i915_device *device)
{
	struct i915_gt *gt;
	unsigned index;
	int error;

	gt = &device->gt;

	/* Builds the workaround, whitelist, MOCS, RC6 and RPS tables. */
	kern_memset(&gt->init, 0, sizeof(gt->init));
	drv_i915_gt_init_tables(&gt->init, &gt->info, 12, &gt->sb_lock, &gt->mmio);
	kern_logf("i915: gt tables: gt_wa=%u mocs(uc_index=%u entries=%u) rps(rp0=%u rp1=%u min=%u eff=%u pcode=%d) rc6_supported=%d\n",
	    gt->init.gt_wa.count,
	    gt->init.mocs.uc_index,
	    gt->init.mocs.n_entries,
	    gt->init.rps.rp0_freq,
	    gt->init.rps.rp1_freq,
	    gt->init.rps.min_freq,
	    gt->init.rps.efficient_freq,
	    gt->init.rps.pcode_ok,
	    gt->init.rc6.supported);
	drv_i915_wa_list_dump(&gt->init.gt_wa, "GT");

	/* Dumps the render engine's lists, which are the ones that matter for drawing. */
	for (index = 0U; index < gt->info.num_engines && index < I915_WA_ENGINES; index++) {
		kern_logf("i915: gt tables %s: engine_wa=%u ctx_wa=%u whitelist=%u\n",
		    gt->info.engines[index].name,
		    gt->init.engine_wa[index].count,
		    gt->init.ctx_wa[index].count,
		    gt->init.whitelist[index].count);
		if (gt->info.engines[index].class != I915_RENDER_CLASS)
			continue;

		drv_i915_wa_list_dump(&gt->init.engine_wa[index], "ENGINE");
		drv_i915_wa_list_dump(&gt->init.ctx_wa[index], "CTX");
		drv_i915_wa_list_dump(&gt->init.whitelist[index], "WHITELIST");
	}

	/* Creates the GT objects' window at the top of the GGTT, the scratch object and the kernel address space. */
	error = i915_start_gt_memory(device);
	if (error != 0)
		return error;

	/* Creates the status pages, execlists state and kernel contexts (intel_engines_init). */
	device->stage = "intel_engines_init";
	error = drv_i915_engines_init(&gt->engines, &gt->info, &gt->mem, &gt->ppgtt);
	if (error != 0)
		return error;

	gt->engines_inited = 1U;

	/* Programs the hardware in the reference order (intel_gt_resume). */
	device->stage = "intel_gt_resume";
	error = drv_i915_gt_resume(&gt->engines, &gt->init, &gt->info, &gt->mmio, &gt->uncore_lock);
	kern_logf("i915: gt resume: rc=%d reset_engines=%d stop_cs_timeouts=%u engines_resumed=%u l3cc(rcs)=%u\n",
	    error,
	    gt->engines.reset_rc,
	    gt->engines.stop_cs_timeouts,
	    gt->engines.resumed,
	    gt->engines.l3cc_writes_rcs);
	kern_logf("i915: gt hw: pat=%d gt_wa(w=%u skip=%u ok=%u mismatch=%u noverify=%u) mocs(global=%u l3cc=%u) whitelist_writes=%u engines=%u\n",
	    gt->init.pat_programmed,
	    gt->init.gt_wa_applied.written,
	    gt->init.gt_wa_applied.skipped_unchanged,
	    gt->init.gt_wa_applied.verified,
	    gt->init.gt_wa_applied.mismatched,
	    gt->init.gt_wa_applied.not_verifiable,
	    gt->init.mocs_global_writes,
	    gt->init.mocs_l3cc_writes,
	    gt->init.whitelist_writes,
	    gt->init.engines_resumed);
	kern_logf("i915: gt pm: rps_enabled=%d rc6_enabled=%d rc6_ctl=0x%x pg_enable=0x%x\n",
	    gt->init.rps.enabled,
	    gt->init.rc6.enabled,
	    gt->init.rc6.ctl_enable,
	    gt->init.rc6.pg_enable);
	if (error != 0)
		return error;

	gt->engines_resumed = 1U;

	/* Records the default context images from the GPU (__engines_record_defaults). */
	device->stage = "__engines_record_defaults";
	error = drv_i915_engines_record_defaults(&gt->defaults, &gt->engines, &gt->init, &gt->mem, &gt->ppgtt, &gt->mmio, &gt->uncore_lock, 200U);
	gt->defaults_inited = 1U;
	kern_logf("i915: record_defaults: rc=%d where=%s polls=%u timed_out=%d wedged=%d\n",
	    error,
	    gt->defaults.err_where != NULL ? gt->defaults.err_where : "-",
	    gt->defaults.polls,
	    gt->defaults.timed_out,
	    gt->defaults.wedged);
	if (error != 0)
		return error;

	/* Every later context starts from the recorded image. */
	for (index = 0U; index < gt->engines.n; index++)
		gt->engines.ge[index].default_state = gt->defaults.default_state[index];

	/* Checks the engine workarounds from the GPU's own stores (__engines_verify_workarounds). */
	device->stage = "__engines_verify_workarounds";
	error = drv_i915_engines_verify_workarounds(&gt->verify, &gt->engines, &gt->init, &gt->mem, &gt->mmio, 200U);
	gt->verify_inited = 1U;
	kern_logf("i915: verify_workarounds: rc=%d where=%s polls=%u timed_out=%d\n",
	    error,
	    gt->verify.err_where != NULL ? gt->verify.err_where : "-",
	    gt->verify.polls,
	    gt->verify.timed_out);
	if (error != 0)
		return error;

	/* Creates the migration context; like the reference, a failure is only logged. */
	error = drv_i915_migrate_init(&gt->migrate, &gt->engines, &gt->mem);
	gt->migrate_inited = 1U;
	kern_logf("i915: migrate_init: rc=%d where=%s\n",
	    error,
	    gt->migrate.err_where != NULL ? gt->migrate.err_where : "-");

	/* Succeeded: the GT is programmed and its default contexts exist. */
	return 0;
}

/* Creates the GT objects' GGTT window, the scratch object and the kernel address space. */
static int
i915_start_gt_memory(
	struct i915_device *device)
{
	struct i915_gt *gt;
	int error;

	gt = &device->gt;

	/* Opens the window at the top of the GGTT (intel_gt_init_scratch, kernel_vm). */
	device->stage = "intel_gt_init_scratch/kernel_vm";
	error = drv_i915_gt_mem_init(&gt->mem, gt->dma_device, I915_START_DMA_MASK, gt->gtt.address, gt->ggtt_entries, gt->scratch_pte, &gt->mmio);
	if (error != 0)
		return error;

	gt->mem_inited = 1U;

	/* Creates the GT scratch object. */
	error = drv_i915_gt_init_scratch(&gt->mem, &gt->gt_scratch);
	if (error != 0)
		return error;

	/* Creates the kernel contexts' address space. */
	error = drv_i915_gt_ppgtt_create(&gt->mem, &gt->ppgtt);
	if (error != 0)
		return error;

	kern_logf("i915: gt mem: ggtt window first=%u pages=%u (of %u) scratch ggtt=0x%llx ppgtt top_pd dma=0x%llx objects=%u\n",
	    gt->mem.window_first,
	    gt->mem.window_pages,
	    gt->ggtt_entries,
	    (unsigned long long)gt->gt_scratch->ggtt_offset,
	    (unsigned long long)gt->ppgtt.top_pd_dma,
	    gt->mem.objects_live);

	/* Succeeded: the GT's memory is ready. */
	return 0;
}

/* Creates the protected-content context; a failure is only logged, as in the reference. */
static void
i915_start_pxp(
	struct i915_device *device)
{
	struct i915_gt *gt;
	int error;

	gt = &device->gt;

	/* Creates the context on the video engine (intel_pxp_init). */
	error = drv_i915_pxp_init(&gt->pxp, &gt->engines, &gt->ppgtt, &gt->mem, 1);
	gt->pxp_inited = 1U;
	kern_logf("i915: pxp_init: rc=%d where=%s full=%d\n",
	    error,
	    gt->pxp.err_where != NULL ? gt->pxp.err_where : "-",
	    gt->pxp.full_feature);
}

/*
 * Keeps the GT awake for the device's life and publishes the node.
 *
 * The render path holds every forcewake domain while it runs, the way the
 * hardware tests held it.
 */
static int
i915_start_publish(
	struct i915_device *device)
{
	struct i915_gt *gt;
	unsigned held;
	int error;

	gt = &device->gt;

	/* Holds every domain for as long as the node is published. */
	device->stage = "forcewake_resident";
	error = i915_forcewake_get_all(gt, &held);
	if (error != 0) {
		i915_forcewake_put_all(gt, held);
		return error;
	}

	gt->forcewake_held = held;

	/* Creates the request worker's state. */
	device->stage = "worker_create";
	error = drv_i915_worker_create(device);
	if (error != 0)
		return error;

	/* The panel the node's display drives, over the objects the start built. */
	drv_i915_display_resident_deps(device);

	/* Lets a test build run its scenario on the started device before anything is served. */
	if (drv_i915_test_after_start != NULL)
		drv_i915_test_after_start(device);

	/*
	 * Publishes the node and serves its requests on this thread until the
	 * device stops; the node is withdrawn before this returns.
	 */
	device->stage = "gpu-publication";
	error = drv_i915_worker_serve(device);
	if (error != 0)
		return error;

	/* Succeeded: the node was published and served until the device stopped. */
	return 0;
}

/* Stops the engines, then gives back the GT's objects. */
static void
i915_stop_gt(
	struct i915_device *device)
{
	struct i915_gt *gt;
	unsigned held;
	unsigned index;
	int gpu_retained;
	int error;

	gt = &device->gt;

	/* Gives back the domains the published node held. */
	i915_forcewake_put_all(gt, gt->forcewake_held);
	gt->forcewake_held = 0U;

	/* Stops anything still scanned out, the hotplug path, and unregisters the driver. */
	drv_i915_display_stop_early(device);

	/* Frees the protected-content context. */
	if (gt->pxp_inited != 0U) {
		drv_i915_pxp_fini(&gt->pxp, &gt->mem);
		gt->pxp_inited = 0U;
		kern_logf("i915: stop: intel_pxp_fini (VCS context + stream page released)\n");
	}

	/*
	 * Stops the command streamers and resets the engines before the objects
	 * they point at are released (intel_gt_driver_remove -> gt_sanitize).
	 */
	if (gt->engines_resumed != 0U) {
		error = i915_forcewake_get_all(gt, &held);
		if (error == 0) {
			for (index = 0U; index < gt->engines.n; index++)
				drv_i915_execlists_reset_prepare(&gt->engines.ge[index], &gt->mmio);

			error = drv_i915_gt_reset_all(&gt->uncore_lock, &gt->mmio, I915_GT_RESET_ACK_US);
			kern_logf("i915: stop: engines stopped and reset (rc=%d) before release\n", error);
		} else {
			kern_logf("i915: stop: forcewake for the remove reset failed rc=%d\n", error);
		}

		i915_forcewake_put_all(gt, held);
		gt->engines_resumed = 0U;
	}

	/* Frees the migration, workaround-check and default-context objects. */
	if (gt->migrate_inited != 0U) {
		drv_i915_migrate_fini(&gt->migrate, &gt->mem);
		gt->migrate_inited = 0U;
	}

	if (gt->verify_inited != 0U) {
		drv_i915_engines_verify_wa_release(&gt->verify, &gt->mem);
		gt->verify_inited = 0U;
	}

	if (gt->defaults_inited != 0U) {
		for (index = 0U; index < gt->engines.n; index++)
			gt->engines.ge[index].default_state = NULL;

		drv_i915_engines_defaults_release(&gt->defaults, &gt->mem);
		gt->defaults_inited = 0U;
	}

	/* A kept buffer the GPU was not shown to be done with keeps the engines and every GT object. */
	gpu_retained = drv_i915_display_gpu_retained(device);

	/* Frees the engines' status pages and kernel contexts. */
	if (gt->engines_inited != 0U && gpu_retained) {
		kern_logf("i915: stop: engines NOT released (the GPU was not shown to be done with a kept buffer)\n");
	} else if (gt->engines_inited != 0U) {
		drv_i915_engines_release(&gt->engines, &gt->mem);
		gt->engines_inited = 0U;
		kern_logf("i915: stop: engines released (status pages, kernel contexts)\n");
	}

	/* Frees the kernel address space and every GT object, pointing the window back at scratch. */
	if (gt->mem_inited != 0U && gpu_retained) {
		kern_logf("i915: stop: the GPU was not shown to be done with a kept buffer -- the vm, its tables and every GT object stay (resources_retained=1)\n");
	} else if (gt->mem_inited != 0U) {
		drv_i915_gt_ppgtt_destroy(&gt->mem, &gt->ppgtt);
		drv_i915_gt_mem_fini(&gt->mem);
		gt->mem_inited = 0U;
		kern_logf("i915: stop: GT objects released, GGTT window back to scratch (pte_writes=%u live=%u)\n",
		    gt->mem.pte_writes,
		    gt->mem.objects_live);
	}
}

/* Gives back interrupts, the aperture, DMA, the GGTT, the registers and PCI. */
static void
i915_stop_hardware(
	struct i915_device *device)
{
	struct i915_gt *gt;
	int sync_failed;
	int kept;
	int enabled;
	int error;

	gt = &device->gt;

	/*
	 * Resets the sources and detaches the handler, unless a pipe's
	 * interrupt drain failed: the handler that did not finish may still
	 * touch display registers, so it stays attached.
	 */
	sync_failed = drv_i915_display_irq_sync_failed(device);
	if (gt->irq_installed != 0U && sync_failed) {
		kern_logf("i915: stop: intel_irq_uninstall NOT run: a pipe interrupt drain failed earlier (irq_attached=1 resources_retained=1; irq_count=%u handled=%u none=%u)\n",
		    gt->irq.irq_count,
		    gt->irq.irq_handled_count,
		    gt->irq.irq_none_count);
	} else if (gt->irq_installed != 0U) {
		drv_i915_irq_uninstall(&gt->irq);
		gt->irq_installed = 0U;
		kern_logf("i915: stop: intel_irq_uninstall (sources reset, handler detached; irq_count=%u handled=%u none=%u)\n",
		    gt->irq.irq_count,
		    gt->irq.irq_handled_count,
		    gt->irq.irq_none_count);
	}

	/* Takes the rest of the display apart in reverse order. */
	drv_i915_display_fini(device);

	/* Frees the MSI vector. */
	enabled = drv_i915_pci_msi_enabled(&gt->pci);
	if (gt->msi_kept != 0U && enabled != 0) {
		drv_i915_pci_teardown_msi(&gt->pci);
		kern_logf("i915: stop: MSI resource released\n");
	}

	gt->msi_kept = 0U;

	/* Unmaps the aperture view. */
	if (gt->aperture != NULL) {
		error = hal_space_unmap_device(gt->aperture, (size_t)gt->gmadr_size);
		kern_logf("i915: stop: WC aperture unmap va=%p size=0x%llx rc=%d\n",
		    gt->aperture,
		    (unsigned long long)gt->gmadr_size,
		    error);
		gt->aperture = NULL;
	}

	/*
	 * A scanout buffer may still be read by the display: its pages, the
	 * scratch page behind its guard PTEs, the DMA device (IOMMU mappings),
	 * the GGTT and bus mastering all stay as they are -- a leak is the safe
	 * side.
	 */
	kept = drv_i915_display_abandoned(device);
	if (!kept)
		kept = sync_failed;
	if (kept) {
		kern_logf("i915: stop: a scanout buffer was ABANDONED -- DMA device, scratch page, BARs and bus mastering are kept\n");
		return;
	}

	/* Frees the display and its Linux environments. */
	drv_i915_display_destroy(device);

	/* Frees the scratch page and the DMA device. */
	if (gt->scratch_created != 0U) {
		(void)drv_dma_vector_free(gt->scratch);
		gt->scratch_created = 0U;
	}

	if (gt->dma_created != 0U) {
		(void)drv_dma_device_destroy(gt->dma_device);
		gt->dma_device = NULL;
		gt->dma_created = 0U;
	}

	/* Unmaps the table and the registers and returns BAR0. */
	if (gt->gtt_mapped != 0U) {
		drv_pci_device_unmap_bar(device->pci, &gt->gtt);
		gt->gtt_mapped = 0U;
	}

	if (gt->regs_mapped != 0U) {
		drv_pci_device_unmap_bar(device->pci, &gt->regs);
		gt->regs_mapped = 0U;
	}

	if (gt->bar_claimed != 0U) {
		drv_pci_device_release_bar(device->pci, GEN4_GTTMMADR_BAR);
		gt->bar_claimed = 0U;
	}

	/* Restores the PCI command word the firmware left. */
	if (gt->pci_enabled != 0U) {
		drv_i915_pci_restore(&gt->pci);
		gt->pci_enabled = 0U;
	}

	/* Gives back the PCI core's runtime PM hold last, after every register access. */
	if (gt->probe_pm_held != 0U) {
		drv_i915_rpm_put(&gt->probe_pm);
		gt->probe_pm_held = 0U;
		kern_logf("i915: stop: PCI probe runtime PM released (usage=%d)\n", drv_i915_rpm_usage(&gt->probe_pm));
	}
}

/* Wakes every forcewake domain in order and reports how many are held. */
static int
i915_forcewake_get_all(
	struct i915_gt *gt,
	unsigned *held)
{
	int error;

	/* Takes the domains one at a time, stopping at the first that fails. */
	*held = 0U;
	while (*held < I915_START_DOMAINS) {
		error = drv_i915_forcewake_get(&gt->mmio, i915_start_domains[*held]);
		if (error != 0)
			return error;

		(*held)++;
	}

	/* Succeeded: every domain is awake. */
	return 0;
}

/* Releases the first held forcewake domains, in reverse order. */
static void
i915_forcewake_put_all(
	struct i915_gt *gt,
	unsigned held)
{
	/* Puts back only what was taken, last first. */
	while (held > 0U) {
		held--;
		(void)drv_i915_forcewake_put(&gt->mmio, i915_start_domains[held]);
	}
}

/* Dumps the newest trace records and what was overwritten. */
static void
i915_start_dump_trace(
	struct i915_gt *gt)
{
	static struct i915_trace_record records[I915_START_TRACE_DUMP];
	uint32_t count;
	uint32_t index;

	/* Copies the newest records out of the ring. */
	count = drv_i915_trace_snapshot(&gt->trace, records, I915_START_TRACE_DUMP);
	kern_logf("i915: trace: %u records (dropped %u)\n", count, gt->trace.dropped);

	/* Prints only failures and unimplemented steps; the rest is in the ring. */
	for (index = 0U; index < count; index++) {
		if (records[index].op != I915_TRACE_FAIL && records[index].op != I915_TRACE_UNIMPLEMENTED)
			continue;

		kern_logf("i915: trace #%u %s %s 0x%llx 0x%llx\n",
		    records[index].sequence,
		    drv_i915_trace_op_name(records[index].op),
		    records[index].what != NULL ? records[index].what : "?",
		    (unsigned long long)records[index].argument0,
		    (unsigned long long)records[index].argument1);
	}
}

/* Reports nonzero when a PCI device id is a Tiger Lake part. */
static int
i915_product_is_tigerlake(
	uint16_t product)
{
	static const uint16_t tigerlake[] = { INTEL_TGL_IDS(I915_ID_VALUE) };
	unsigned index;

	/* Looks the id up in the same list the PCI driver matches on. */
	for (index = 0U; index < sizeof(tigerlake) / sizeof(tigerlake[0]); index++) {
		if (tigerlake[index] == product)
			return 1;
	}

	/* Not a Tiger Lake part. */
	return 0;
}

/* Counts the set bits of a word. */
static unsigned
i915_popcount32(
	uint32_t value)
{
	unsigned count;

	/* Counts one bit per iteration. */
	count = 0U;
	while (value != 0U) {
		count += value & 1U;
		value >>= 1;
	}

	/* Reports the number of set bits. */
	return count;
}

/* Reads the CPU's physical address width from CPUID 0x80000008. */
static unsigned
i915_cpu_physical_bits(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	/* Asks for the highest extended leaf. */
	eax = 0x80000000U;
	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));

	/* Without the address-size leaf the architectural minimum of 36 bits applies. */
	if (eax < 0x80000008U)
		return 36U;

	/* Reads the address-size leaf. */
	eax = 0x80000008U;
	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));

	/* Reports the physical address bits. */
	return (unsigned)(eax & 0xffU);
}

/* Keeps the display interrupt summary switched off while the display is not connected. */
static void
i915_interim_display_irq_reset(
	void *context)
{
	struct i915_mmio *mmio;

	/* Only the summary: a device without a display acknowledges no display source. */
	mmio = context;
	drv_i915_raw_write32(mmio, I915_DISPLAY_INT_CTL, 0U);
	drv_i915_posting_read32(mmio, I915_DISPLAY_INT_CTL);
}

/* Ignores the interrupt enable state while no display is connected. */
static void
i915_interim_display_irq_enable(
	void *context,
	int enabled)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(enabled);
}

/* Does nothing for a display hook while no display is connected. */
static void
i915_interim_display_irq_nothing(
	void *context)
{
	UNUSED_PARAMETER(context);
}

/* Ignores a display interrupt; the summary is off, so none is expected. */
static void
i915_interim_display_irq_handle(
	void *context,
	uint32_t master_ctl)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(master_ctl);
}
