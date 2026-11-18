/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i915 PCI driver: registration, attach and detach.
 *
 * Attach creates the device state and registers the device for a deferred
 * start; the hardware is brought up later by the device start (device.c).
 * Detach refuses while the device still owns hardware or a published node.
 */

#include "i915.h"
#include "command.h"
#include "device.h"
#include "display/display.h"
#include "display/xd9-backlight.h"
#include "display/xd9-flip.h"
#include "display/xd9-node.h"
#include "display/present.h"
#include "job.h"
#include "request-queue.h"
#include "reset.h"
#include "resource.h"
#include "session.h"
#include "worker.h"
#include "vk/vk.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu.h>
#include <drivers/pci/pci-i915.h>
#include <drivers/pci/pci.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/pci-ids.h"

/* Builds one exact-match identity row for an Intel graphics product. */
#define I915_ID(product)	{ 0x8086U, (product), DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, 0U }

/* How long a suspend waits for the request worker to park, in milliseconds. */
#define I915_SUSPEND_PARK_MS	2000U

/*
 * Whether a Gen9 device (Skylake to Comet Lake) is driven when the boot line
 * does not say: on.  i915.gen9=off on the boot line leaves the device to the
 * firmware; i915.gen9=on is accepted and changes nothing.
 */
#define I915_GEN9_DEFAULT_ON	1

static int i915_attach(struct drv_pci_device *pci, const struct drv_pci_id *id);
static int i915_detach(struct drv_pci_device *pci, unsigned flags);
static void i915_shutdown(struct drv_pci_device *pci);
static int i915_suspend(struct drv_pci_device *pci);
static int i915_resume(struct drv_pci_device *pci);
static int i915_gen9_enabled(void);
static int i915_boot_line_has(const char *line, const char *token);

/*
 * Registers the i915 PCI driver for the graphics devices it covers.
 *
 * Ice Lake, Tiger Lake, Rocket Lake, Alder Lake-S/P/N and Raptor Lake-S/P/U
 * are matched, and the Gen9 parts (Skylake, Kaby Lake, Coffee Lake, Comet
 * Lake), which the attach takes unless i915.gen9 is off.  A matched device whose display this
 * driver has no platform data for says so in its start log instead of
 * programming the wrong registers.
 */
int
drv_pci_i915_driver_register(void)
{
	static const struct drv_pci_id identifiers[] = {
		I915_PCI_ICL_IDS(I915_ID),
		I915_PCI_TGL_IDS(I915_ID),
		I915_PCI_RKL_IDS(I915_ID),
		I915_PCI_ADLS_IDS(I915_ID),
		I915_PCI_RPLS_IDS(I915_ID),
		I915_PCI_ADLP_IDS(I915_ID),
		I915_PCI_ADLN_IDS(I915_ID),
		I915_PCI_RPLU_IDS(I915_ID),
		I915_PCI_RPLP_IDS(I915_ID),
		I915_PCI_SKL_IDS(I915_ID),
		I915_PCI_KBL_IDS(I915_ID),
		I915_PCI_CFL_IDS(I915_ID),
		I915_PCI_CML_IDS(I915_ID)
	};
	static struct drv_pci_driver driver = {
		"i915",
		identifiers,
		sizeof(identifiers) / sizeof(identifiers[0]),
		NULL,
		i915_attach,
		i915_detach,
		i915_shutdown,
		i915_suspend,
		i915_resume,
		{ 0U, 0U, 0U, 0U }
	};
	int error;

	/* Offers root the start of devices the boot parameter i915.start=manual holds. */
	drv_i915_device_start_ops_install();

	/* Hands probing, binding and the device lifecycle to PCI. */
	error = drv_pci_driver_register(&driver);
	if (error != 0)
		return error;

	/* Succeeded: matching devices are attached during PCI scanning. */
	return 0;
}

/*
 * Prepares the software state the GPU node's operations share.
 *
 * Runs once per device before the request worker is created and the node
 * is published: the IRQ lock, the retire wait queue and the session
 * numbering.
 */
void
drv_i915_node_init(
	struct i915_device *device)
{
	/* The IRQ lock guards the request queues; the wait queue is where drain sleeps. */
	spin_init(&device->irq_lock, LOCK_RANK_DEVICE, "i915 irq");
	waitq_init(&device->retire_waitq, "i915 retire");

	/* Session zero is reserved, so a logged identifier of zero is always a bug. */
	device->next_session = 1U;

	/* Starts the frame timing with an empty window. */
	drv_i915_perf_init(&device->perf);
}

/*
 * Publishes the GPU node.
 *
 * Fills the engine records the sessions queue requests on, binds every
 * operation of the node and registers it with the GPU core.  The hardware
 * is already up and the request worker runs the requests.  A node with a
 * panel also offers its display and the display-only pairing.
 */
int
drv_i915_publish(
	struct i915_device *device)
{
	struct drv_gpu_ops *ops;
	struct i915_engine *engine;
	unsigned index;
	int error;

	/*
	 * Fills the engine records.
	 *
	 * XXX: the records only number and queue requests; the request worker
	 * runs them, on the render engine and (synchronous batches of a video
	 * session) on the video engine.
	 */
	for (index = 0U; index < I915_ENGINE_COUNT; index++) {
		engine = &device->engines[index];
		kern_memset(engine, 0, sizeof(*engine));
		engine->device = device;
		engine->index = index;

		/* The first record is the render engine, the second the copy engine, the third the video decode engine. */
		if (index == I915_ENGINE_RCS0) {
			engine->class = I915_CLASS_RENDER;
		} else if (index == I915_ENGINE_VCS0) {
			engine->class = I915_CLASS_VIDEO;
		} else {
			engine->class = I915_CLASS_COPY;
		}

		/* Sequence number 0 is never given, and the record is ready for requests. */
		engine->next_seqno = 1U;
		engine->initialized = 1U;
	}

	/* Attaches the Vulkan executor before the node is registered, so every session can reach it. */
	error = drv_i915_render_attach(device, &device->vk);
	if (error != 0)
		return error;

	/* Describes the node: its interface, its capabilities and every operation. */
	ops = &device->gpu_ops;
	kern_memset(ops, 0, sizeof(*ops));
	ops->version = DRV_GPU_INTERFACE_VERSION;
	ops->size = sizeof(struct drv_gpu_ops);
	ops->capabilities = I915_CAPABILITIES;
	ops->reserved = 0U;

	/* Binds each part's operations. */
	drv_i915_session_bind_ops(ops);
	drv_i915_resource_bind_ops(ops);
	drv_i915_command_bind_ops(ops);
	drv_i915_job_bind_ops(ops);
	drv_i915_recovery_bind_ops(ops);

	/* The display and scanout operations and their capabilities, when the node has a panel. */
	drv_i915_display_bind_ops(device, ops);

	/* Registers the node; the GPU core borrows the table and the device until unregister. */
	error = drv_gpu_register(ops, device, &device->gpu);
	if (error != 0) {
		drv_i915_render_detach(device->vk);
		device->vk = NULL;
		return error;
	}

	/* Names the backend without claiming any rendering works. */
	kern_logf("i915: registered native GPU node (storage, native streams, jobs)\n");

	/* The node is there: the graphical login no longer waits for this device. */
	drv_i915_device_attach_settled(device);

	/* Succeeded: the GPU node accepts sessions. */
	return 0;
}

/*
 * Withdraws the GPU node.
 *
 * Returns EBUSY while sessions are still open; the node stays registered
 * for a retry.
 */
int
drv_i915_unpublish(
	struct i915_device *device)
{
	int error;

	/* A node that was never published has nothing to withdraw. */
	if (device->gpu == NULL)
		return 0;

	/* Unregisters the node; open sessions keep the registration alive. */
	error = drv_gpu_unregister(device->gpu);
	if (error != 0)
		return error;

	/* NULL tells detach and the completions that the node is gone. */
	device->gpu = NULL;

	/* Detaches the Vulkan executor, which no session can reach any more. */
	drv_i915_render_detach(device->vk);
	device->vk = NULL;

	/* Succeeded: the hardware may now be stopped. */
	return 0;
}

/* Creates the device state and registers the device for its deferred start. */
static int
i915_attach(
	struct drv_pci_device *pci,
	const struct drv_pci_id *id)
{
	struct i915_product_kind kind;
	struct i915_device *device;
	uint16_t product;
	int enabled;
	int error;

	UNUSED_PARAMETER(id);

	/*
	 * Leaves a Gen9 device unbound when i915.gen9 is off, so the machine
	 * behaves as before the driver knew the part: the firmware's picture
	 * stays and the PCI core never suspends the function.
	 */
	product = drv_pci_device_product(pci);
	error = drv_i915_product_kind(product, &kind);
	if (error == 0 && kind.graphics_ver == 9U) {
		enabled = i915_gen9_enabled();
		if (enabled == 0) {
			kern_logf("i915: device 8086:%04x is %s; not taken (i915.gen9=off on the boot line)\n",
			    (unsigned)product,
			    kind.name);
			return ENODEV;
		}
	}

	/* Allocates the device state before any hardware is touched. */
	device = kern_calloc(1U, sizeof(*device));
	if (device == NULL)
		return ENOMEM;

	/* Records the identity so every later log names the device. */
	device->pci = pci;
	device->product = drv_pci_device_product(pci);
	device->revision = drv_pci_device_revision(pci);
	device->stage = "attach";

	/* Creates the mutex that serializes sessions and device-wide changes. */
	error = mutex_init(&device->mutex, LOCK_RANK_DEVICE, "i915");
	if (error != 0) {
		kern_free(device);
		return error;
	}

	/* Prepares the node state: its locks, wait queue and session numbering. */
	drv_i915_node_init(device);

	/* Makes the device state PCI's to hand back at detach. */
	error = drv_pci_device_set_driver_data(pci, device);
	if (error != 0) {
		kern_free(device);
		return error;
	}

	kern_logf("i915: device 8086:%04x rev %02x attached\n",
	    (unsigned)device->product,
	    (unsigned)device->revision);

	/* Defers the bring-up to a context where it may sleep and wait. */
	drv_i915_device_schedule_start(device);

	/* Succeeded: the device is registered and starts once the kernel is ready. */
	return 0;
}

/* Stops the device and frees its state. */
static int
i915_detach(
	struct drv_pci_device *pci,
	unsigned flags)
{
	struct i915_device *device;
	int error;

	UNUSED_PARAMETER(flags);

	/* An attach that already cleaned up has nothing left to detach. */
	device = drv_pci_device_driver_data(pci);
	if (device == NULL)
		return 0;

	/* Withdraws a pending start and gives back every hardware lease the start took. */
	error = drv_i915_device_stop(device);
	if (error != 0)
		return error;

	/* Removes PCI's pointer before the state goes away. */
	error = drv_pci_device_set_driver_data(pci, NULL);
	if (error != 0)
		return error;

	kern_free(device);

	/* Succeeded: PCI may clear the driver binding. */
	return 0;
}

/*
 * Prepares the device for the machine going down: an output that only
 * holds the last picture of an ended lease is stopped, so the screen does
 * not keep a picture of nobody after a shutdown.
 */
static void
i915_shutdown(
	struct drv_pci_device *pci)
{
	struct i915_device *device;

	/* An attach that already cleaned up has nothing to prepare. */
	device = drv_pci_device_driver_data(pci);
	if (device == NULL)
		return;

	/* Stops a held picture and refuses later holds. */
	drv_i915_present_shutdown(device);
}

/*
 * Suspends the device for S0 idle: the request worker parks
 * (the output is stopped, the lease kept, the GT left idle), then the
 * hardware goes down into DC9.  A worker that does not park, or hardware
 * that does not go down, refuses the suspend with the worker serving
 * again.  A device that does not serve (its start has not run, or failed)
 * has no state to keep: its start is refused while it runs.
 */
static int
i915_suspend(
	struct drv_pci_device *pci)
{
	struct i915_product_kind kind;
	struct i915_device *device;
	int product_error;
	int gen9;
	int error;

	/* An attach that already cleaned up has nothing to suspend. */
	device = drv_pci_device_driver_data(pci);
	if (device == NULL)
		return 0;

	/*
	 * A Gen9 device's display is the firmware's, which D3hot would lose
	 * for good: it sleeps in D0, and refuses the suspend only when the PCI
	 * core cannot keep it there.
	 */
	product_error = drv_i915_product_kind(device->product, &kind);
	gen9 = product_error == 0 && kind.graphics_ver == 9U;
	if (gen9) {
		error = drv_pci_device_keep_d0(pci, true);
		if (error != 0) {
			kern_logf("i915: suspend refused: a Gen9 device keeps the firmware's display, which D3hot would lose, and it cannot stay in D0 (%d)\n", error);
			return EBUSY;
		}
	}

	/* Parks the request worker. */
	error = drv_i915_worker_park(device, I915_SUSPEND_PARK_MS);
	if (error == ENODEV) {
		/* A start that is running cannot be suspended under it. */
		if (device->start_launched != 0U && device->start_returned == 0U)
			return EBUSY;

		/* A device that does not serve has nothing to keep. */
		return 0;
	}

	/* Refuses a worker that did not park. */
	if (error != 0)
		return error;

	/* A Gen9 device in D0 keeps everything: only the panel's light and the external displays go dark. */
	if (gen9) {
		drv_i915_xd9_node_suspend(device);
		drv_i915_xd9_backlight_suspend(device);
		device->suspended = 1U;
		device->suspended_d0 = 1U;
		kern_logf("i915: suspend: a Gen9 device sleeps in D0 (the display keeps its state; the panel's light and the external displays are off)\n");
		return 0;
	}

	/* Takes the hardware down; a refusal lets the worker serve again. */
	error = drv_i915_device_suspend_hw(device);
	if (error != 0) {
		drv_i915_worker_unpark(device);
		return error;
	}

	/* suspended tells the resume to bring the hardware back. */
	device->suspended = 1U;

	/* Succeeded: the device may go to D3hot. */
	return 0;
}

/*
 * Resumes the device after S0 idle: the hardware comes back, then the
 * request worker leaves its park; the next presentation lights the output
 * again.  The worker serves even when a step of the hardware failed, and
 * the failure is reported.
 */
static int
i915_resume(
	struct drv_pci_device *pci)
{
	struct i915_device *device;
	int error;

	/* A device that was not suspended has nothing to resume. */
	device = drv_pci_device_driver_data(pci);
	if (device == NULL || device->suspended == 0U)
		return 0;

	/* A Gen9 device that slept in D0: the panel is checked, its light and the external displays come back, then the worker serves. */
	if (device->suspended_d0 != 0U) {
		drv_i915_xd9_flip_resume(device);
		drv_i915_xd9_backlight_resume(device);
		drv_i915_xd9_node_resume(device);
		drv_i915_worker_unpark(device);
		device->suspended = 0U;
		device->suspended_d0 = 0U;
		kern_logf("i915: resume: the Gen9 display is back\n");
		return 0;
	}

	/* Brings the hardware back, then lets the worker serve. */
	error = drv_i915_device_resume_hw(device);
	drv_i915_worker_unpark(device);
	device->suspended = 0U;
	if (error != 0)
		return error;

	/* Succeeded: the device serves again. */
	return 0;
}

/*
 * Reports nonzero when Gen9 devices are to be driven: i915.gen9=on or
 * i915.gen9=off on the boot line, else I915_GEN9_DEFAULT_ON.
 *
 * The PCI attach runs before the kernel parses the boot parameters, so the
 * line the loader handed over is read directly.
 */
static int
i915_gen9_enabled(void)
{
	const char *line;
	int found;

	/* Without a boot line the default holds. */
	line = kern_boot_handoff("boot.command-line");
	if (line == NULL)
		return I915_GEN9_DEFAULT_ON;

	/* An explicit on wins. */
	found = i915_boot_line_has(line, "i915.gen9=on");
	if (found != 0)
		return 1;

	/* An explicit off wins over the default. */
	found = i915_boot_line_has(line, "i915.gen9=off");
	if (found != 0)
		return 0;

	/* Otherwise the default holds. */
	return I915_GEN9_DEFAULT_ON;
}

/* Reports nonzero when a whitespace-separated word of the boot line is exactly the token. */
static int
i915_boot_line_has(
	const char *line,
	const char *token)
{
	size_t length;
	size_t start;
	size_t end;
	int different;

	/* Compares the token with each word in turn. */
	length = kern_strlen(token);
	start = 0U;
	for (;;) {
		/* Skips the spaces before the word. */
		while (line[start] == ' ' || line[start] == '\t')
			start++;

		/* The end of the line ends the search. */
		if (line[start] == '\0')
			break;

		/* The word runs to the next space or to the end. */
		end = start;
		while (line[end] != '\0' && line[end] != ' ' && line[end] != '\t')
			end++;

		/* The word is the token when it has its length and characters. */
		if (end - start == length) {
			different = kern_memcmp(line + start, token, length);
			if (different == 0)
				return 1;
		}

		/* The next word starts after this one. */
		start = end;
	}

	/* No word is the token. */
	return 0;
}
