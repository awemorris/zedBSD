/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amdrdna PCI driver: registration, attach and the stage machine.
 *
 * amdrdna drives AMD GPUs of the RDNA architecture.  The attach runs the
 * stages that only read (A0 identifies the GPU, A1 reads what the
 * firmware put on the display) on every AMD display function; a function
 * that is not a first-generation RDNA GPU the firmware posted is given
 * back to the firmware untouched.  A kept function gets a bring-up
 * thread, which runs the later stages the boot line names (amdrdna.auto=,
 * A9 by default), one at a time and never again after one failed.  A GPU
 * whose A1 failed is kept too, with its failure recorded; no stage runs
 * on it.  The kept function refuses a system suspend, unless its A1
 * failed and nothing ever ran on it, and is never detached.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci-amdrdna.h>
#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/sysctl.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/* The last stage this driver has; asking for a later one is refused. */
#define AMDRDNA_LAST_STAGE		AMDRDNA_STAGE_A13

/* How long the shutdown waits for the bring-up thread to end, and its step. */
#define AMDRDNA_SHUTDOWN_WAIT_US	5000000U
#define AMDRDNA_SHUTDOWN_STEP_US	10000U

static int amdrdna_attach(struct drv_pci_device *pci, const struct drv_pci_id *id);
static int amdrdna_detach(struct drv_pci_device *pci, unsigned flags);
static void amdrdna_shutdown(struct drv_pci_device *pci);
static int amdrdna_suspend(struct drv_pci_device *pci);
static int amdrdna_keep(struct amdrdna_device *device);
static void amdrdna_thread(void *argument);
static int amdrdna_run_stage(struct amdrdna_device *device, unsigned stage);
static void amdrdna_give_back(struct amdrdna_device *device);
static void amdrdna_attach_done(struct amdrdna_device *device);

/*
 * Registers the amdrdna PCI driver for AMD display-class functions.
 *
 * Every AMD function of the display class is offered to the attach, which
 * decides from the device ID and the GPU's own block table.  The audio
 * function of the same card is of another class and is not matched.
 */
int
drv_pci_amdrdna_driver_register(void)
{
	static const struct drv_pci_id identifiers[] = {
		{ AMDRDNA_PCI_VENDOR, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
		  AMDRDNA_PCI_CLASS_DISPLAY, AMDRDNA_PCI_CLASS_DISPLAY_MASK, 0U }
	};
	static struct drv_pci_driver driver = {
		"amdrdna",
		identifiers,
		sizeof(identifiers) / sizeof(identifiers[0]),
		NULL,
		amdrdna_attach,
		amdrdna_detach,
		amdrdna_shutdown,
		amdrdna_suspend,
		NULL,
		{ 0U, 0U, 0U, 0U }
	};
	int error;

	/* Hands probing and binding to PCI. */
	error = drv_pci_driver_register(&driver);
	if (error != 0)
		return error;

	/* Succeeded: matching functions are offered to the attach during PCI scanning. */
	return 0;
}

/*
 * Runs the read-only stages on one function, then keeps it or gives it
 * back.
 */
static int
amdrdna_attach(
	struct drv_pci_device *pci,
	const struct drv_pci_id *id)
{
	struct amdrdna_device *device;
	unsigned index;
	bool off;
	int error;

	UNUSED_PARAMETER(id);

	/* Stays away from the GPU when the boot says so. */
	off = amdrdna_parameter_off();
	if (off)
		return ENODEV;

	/* Allocates the record. */
	device = kern_calloc(1U, sizeof(*device));
	if (device == NULL)
		return ENOMEM;

	/* Records the function's identity and prepares the locks. */
	device->pci = pci;
	drv_pci_device_address(pci, &device->address);
	device->product = drv_pci_device_product(pci);
	device->revision = drv_pci_device_revision(pci);
	device->subvendor = drv_pci_device_subvendor(pci);
	device->subproduct = drv_pci_device_subproduct(pci);
	spin_init(&device->vram_window_lock, LOCK_RANK_DEVICE, "amdrdna vram window");
	spin_init(&device->far_lock, LOCK_RANK_DEVICE, "amdrdna far window");
	spin_init(&device->state_lock, LOCK_RANK_DEVICE, "amdrdna stage");
	waitq_init(&device->state_waitq, "amdrdna stage");
	(void)mutex_init(&device->memory_lock, LOCK_RANK_DEVICE, "amdrdna memory");
	spin_init(&device->interrupts.lock, LOCK_RANK_DEVICE, "amdrdna interrupts");
	waitq_init(&device->interrupts.waitq, "amdrdna interrupts");
	(void)mutex_init(&device->psp.lock, LOCK_RANK_DEVICE, "amdrdna psp");
	(void)mutex_init(&device->thermal.mailbox, LOCK_RANK_DEVICE, "amdrdna smu");
	spin_init(&device->thermal.lock, LOCK_RANK_DEVICE, "amdrdna thermal");
	waitq_init(&device->thermal.waitq, "amdrdna thermal");
	(void)mutex_init(&device->engines.select, LOCK_RANK_DEVICE, "amdrdna select");
	(void)mutex_init(&device->engines.scheduler_lock, LOCK_RANK_DEVICE, "amdrdna kiq");
	spin_init(&device->engines.scheduler.lock, LOCK_RANK_DEVICE, "amdrdna ring");
	waitq_init(&device->engines.scheduler.fences, "amdrdna fence");
	spin_init(&device->engines.graphics.lock, LOCK_RANK_DEVICE, "amdrdna ring");
	waitq_init(&device->engines.graphics.fences, "amdrdna fence");
	for (index = 0U; index < AMDRDNA_COMPUTE_QUEUES; index++) {
		spin_init(&device->engines.compute[index].lock, LOCK_RANK_DEVICE, "amdrdna ring");
		waitq_init(&device->engines.compute[index].fences, "amdrdna fence");
	}
	spin_init(&device->engines.copy.lock, LOCK_RANK_DEVICE, "amdrdna ring");
	waitq_init(&device->engines.copy.fences, "amdrdna fence");
	spin_init(&device->display.lock, LOCK_RANK_DEVICE, "amdrdna display");
	waitq_init(&device->display.waitq, "amdrdna display");
	(void)mutex_init(&device->display.mode_lock, LOCK_RANK_DEVICE, "amdrdna mode");
	(void)mutex_init(&device->hotplug_work.lock, LOCK_RANK_DEVICE, "amdrdna hotplug");
	amdrdna_vcn_init(device);
	amdrdna_spaces_init(device);
	amdrdna_reset_init(device);

	/* Stage A0: finds and identifies the GPU. */
	device->running = AMDRDNA_STAGE_A0;
	error = amdrdna_a0_identify(device);
	if (error != 0) {
		amdrdna_give_back(device);
		return ENODEV;
	}

	/* Records that A0 reported ok. */
	device->stage = AMDRDNA_STAGE_A0;

	/* Stage A1: reads the firmware's display and plans the VRAM. */
	device->running = AMDRDNA_STAGE_A1;
	error = amdrdna_a1_readout(device);

	/*
	 * Records how A1 ended.  A failed A1 wrote nothing to the GPU; the
	 * failure stops the stage machine before it starts, and the function
	 * is kept with what was read.
	 */
	device->running = AMDRDNA_STAGE_NONE;
	if (error != 0) {
		device->failed_stage = AMDRDNA_STAGE_A1;
		device->failed_error = error;
	} else {
		device->stage = AMDRDNA_STAGE_A1;
	}

	/* Keeps the function and starts the bring-up thread. */
	error = amdrdna_keep(device);
	if (error != 0) {
		amdrdna_mark("A1 done; the function is not kept (%d)", error);
		amdrdna_give_back(device);
		return ENODEV;
	}

	/* Succeeded: the function is kept and the later stages run on the thread. */
	return 0;
}

/* Refuses to detach a kept function: its thread and its GPU device hold the record. */
static int
amdrdna_detach(
	struct drv_pci_device *pci,
	unsigned flags)
{
	struct amdrdna_device *device;

	UNUSED_PARAMETER(flags);

	/* A function the attach did not keep has nothing to give back. */
	device = drv_pci_device_driver_data(pci);
	if (device == NULL)
		return 0;

	/* The record lives as long as its thread and its GPU device do. */
	return EBUSY;
}

/*
 * Stops the bring-up thread for the reboot or the shutdown of the machine,
 * then undoes the stages that wrote.
 *
 * The thread is told to stop and waited for, for a bounded time.  Once it
 * has ended, the stages are undone in reverse: stage A5s's GPU device goes
 * offline and its address spaces run nothing new, the display stages A6
 * to A8 flip back to the console's surface, stage A5's queues are
 * detached and its engines halted, stage A4's watch stops and
 * the power unit is told it will be unloaded, stage A3's secure OS unloads
 * what the driver loaded and destroys its ring, and stage A2's interrupt
 * ring is stopped and the memory hubs' contexts get back the words the
 * firmware left.  A thread still inside a stage is left alone with the
 * GPU, and so is everything it set up.
 */
static void
amdrdna_shutdown(
	struct drv_pci_device *pci)
{
	struct amdrdna_device *device;
	unsigned long irq;
	unsigned waited;
	bool running;

	/* A function the attach did not keep is the firmware's already. */
	device = drv_pci_device_driver_data(pci);
	if (device == NULL)
		return;

	/* Tells the thread to stop. */
	irq = spin_lock_irqsave(&device->state_lock);

	device->stopping = true;
	waitq_wake_all(&device->state_waitq);

	spin_unlock_irqrestore(&device->state_lock, irq);

	/* Waits for the thread to end, for a bounded time. */
	running = true;
	for (waited = 0U; waited < AMDRDNA_SHUTDOWN_WAIT_US; waited += AMDRDNA_SHUTDOWN_STEP_US) {
		/* Reads whether the thread still runs. */
		irq = spin_lock_irqsave(&device->state_lock);

		running = device->thread_running;

		spin_unlock_irqrestore(&device->state_lock, irq);

		/* Stops waiting once it has ended. */
		if (!running)
			break;

		/* Gives the thread a step to finish its stage. */
		kern_usleep_range(AMDRDNA_SHUTDOWN_STEP_US, AMDRDNA_SHUTDOWN_STEP_US * 2U);
	}

	/* The thread is still in a stage; the reset follows anyway. */
	if (running) {
		amdrdna_mark("shutdown: the bring-up thread still runs a stage; left to the reset");
		return;
	}

	/*
	 * Takes the GPU device offline (its display lease ends), gives the
	 * display back to the firmware, then undoes stage A5, A4, A3 and A2,
	 * as far as each ran.
	 */
	amdrdna_node_stop(device);
	amdrdna_memory_display_end(device);
	amdrdna_display_stop(device);
	amdrdna_engines_stop(device);
	amdrdna_power_stop(device);
	amdrdna_psp_stop(device);
	amdrdna_a2_stop(device);
}

/*
 * Refuses a system suspend while the driver keeps a function a stage may
 * run on: it has no suspend path yet.  A function whose A1 failed never
 * ran a stage after the attach and has nothing to keep across a suspend.
 */
static int
amdrdna_suspend(
	struct drv_pci_device *pci)
{
	struct amdrdna_device *device;
	unsigned long irq;
	unsigned failed_stage;

	/* A function the attach did not keep is the firmware's already. */
	device = drv_pci_device_driver_data(pci);
	if (device == NULL)
		return 0;

	/* Reads where the stage machine stopped. */
	irq = spin_lock_irqsave(&device->state_lock);

	failed_stage = device->failed_stage;

	spin_unlock_irqrestore(&device->state_lock, irq);

	/* A function stopped by its A1 lets the system sleep. */
	if (failed_stage == AMDRDNA_STAGE_A1)
		return 0;

	/* Refused: the system stays up. */
	return EBUSY;
}

/*
 * Keeps a function: starts its bring-up thread with the stages the boot names as the first target.
 */
static int
amdrdna_keep(
	struct amdrdna_device *device)
{
	struct thread *thread;
	unsigned target;
	bool allowed;
	bool display;
	int error;

	/* Makes the record PCI's to hand back at detach. */
	error = drv_pci_device_set_driver_data(device->pci, device);
	if (error != 0)
		return error;

	/* The first target: the stages the boot runs without being asked, as far as the driver has them. */
	target = amdrdna_parameter_auto();
	if (target > AMDRDNA_LAST_STAGE)
		target = AMDRDNA_LAST_STAGE;
	allowed = amdrdna_reset_allowed(device);
	if (target > AMDRDNA_STAGE_A1V &&
	    device->secure_os_word != 0U &&
	    !allowed)
		target = AMDRDNA_STAGE_A1V;
	device->target = target;

	/* A failed A1 leaves the target unreached; the thread only waits for the shutdown. */
	if (device->failed_stage != AMDRDNA_STAGE_NONE)
		device->target = device->stage;

	/*
	 * The stage that registers the GPU device: A8 with the display, A5s
	 * without.  A boot whose stages reach it counts the GPU as attaching
	 * until it is registered or a stage failed, so the graphical login
	 * waits for the display instead of giving the console back.
	 */
	device->attach_stage = AMDRDNA_STAGE_A5S;
	display = amdrdna_display_wanted();
	if (display)
		device->attach_stage = AMDRDNA_STAGE_A8;
	if (device->target >= device->attach_stage) {
		device->attach_counted = true;
		kern_gpu_attach_begin();
	}

	/* Creates the bring-up thread. */
	error = kthread_create(amdrdna_thread, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		amdrdna_attach_done(device);
		(void)drv_pci_device_set_driver_data(device->pci, NULL);
		return error;
	}

	/*
	 * thread_running tells the shutdown that the thread may still touch
	 * the GPU; the thread clears it last.  Nobody joins the thread; its
	 * end releases it.
	 */
	device->thread_running = true;
	thread->detached = 1;
	thread_start(thread);

	/* Says how far the kept function goes: no further after a failed stage. */
	if (device->failed_stage != AMDRDNA_STAGE_NONE) {
		amdrdna_mark("kept %s for its status; %s failed (%d), no stage runs",
		    device->chip,
		    amdrdna_stage_name(device->failed_stage),
		    device->failed_error);
	} else {
		amdrdna_mark("kept %s; stages up to %s run now", device->chip, amdrdna_stage_name(target));
	}

	/* Succeeded: the function is kept. */
	return 0;
}

/* Runs the stages the target asks for, one at a time, until told to stop. */
static void
amdrdna_thread(
	void *argument)
{
	struct amdrdna_device *device;
	unsigned long irq;
	uint64_t sequence;
	unsigned next;
	int error;

	/* The GPU the thread brings up. */
	device = argument;

	/* Runs one stage per turn of the loop. */
	for (;;) {
		/* Sleeps until there is a stage to run or the machine stops. */
		irq = spin_lock_irqsave(&device->state_lock);

		while (!device->stopping &&
		    !device->reset.requested &&
		    !device->hotplug_work.requested &&
		    (device->stage >= device->target ||
		     device->failed_stage != AMDRDNA_STAGE_NONE)) {
			sequence = waitq_sequence(&device->state_waitq);
			(void)waitq_sleep(&device->state_waitq, &device->state_lock, sequence, 0U, 0U);
		}

		/* Leaves the loop, with the lock let go, when the machine stops. */
		if (device->stopping) {
			spin_unlock_irqrestore(&device->state_lock, irq);
			break;
		}

		/* A reset asked for runs between stages; the lock is let go while it runs. */
		if (device->reset.requested) {
			irq = amdrdna_reset_serve(device, irq);
			spin_unlock_irqrestore(&device->state_lock, irq);
			continue;
		}

		/* A change of the monitor is acted on between stages too. */
		if (device->hotplug_work.requested) {
			irq = amdrdna_hotplug_serve(device, irq);
			spin_unlock_irqrestore(&device->state_lock, irq);
			continue;
		}

		/* Claims the next stage; the status report shows it as running. */
		next = device->stage + 1U;
		device->running = next;

		spin_unlock_irqrestore(&device->state_lock, irq);

		/* Runs the stage without the lock; it may sleep and take long. */
		error = amdrdna_run_stage(device, next);

		/* Records the result; a failed stage stops the machine. */
		irq = spin_lock_irqsave(&device->state_lock);

		device->running = AMDRDNA_STAGE_NONE;
		if (error != 0) {
			device->failed_stage = next;
			device->failed_error = error;
		} else {
			device->stage = next;
		}

		/*
		 * A3 is always followed by A4 in the same run: a secure OS that
		 * started the power unit's firmware must not be left without the
		 * fan and the watch.
		 */
		if (error == 0 &&
		    next == AMDRDNA_STAGE_A3 &&
		    device->target < AMDRDNA_STAGE_A4)
			device->target = AMDRDNA_STAGE_A4;

		spin_unlock_irqrestore(&device->state_lock, irq);

		/* The GPU device is registered by now, or never will be by the boot's stages. */
		if (error != 0 || next >= device->attach_stage)
			amdrdna_attach_done(device);
	}

	/* A thread told to stop before the registration no longer counts the GPU as attaching. */
	amdrdna_attach_done(device);

	/* Tells the shutdown, and a caller waiting for a reset, that the thread no longer touches the GPU. */
	irq = spin_lock_irqsave(&device->state_lock);

	device->thread_running = false;
	waitq_wake_all(&device->state_waitq);

	spin_unlock_irqrestore(&device->state_lock, irq);
}

/* Runs one stage of the bring-up thread. */
static int
amdrdna_run_stage(
	struct amdrdna_device *device,
	unsigned stage)
{
	int error;

	/* Each stage the thread runs. */
	switch (stage) {
	case AMDRDNA_STAGE_A1V:
		error = amdrdna_a1v_vbios(device);
		break;
	case AMDRDNA_STAGE_A2:
		/* A secure OS left by an earlier driver is reset away first, when the boot allows it. */
		error = 0;
		if (device->secure_os_word != 0U)
			error = amdrdna_reset_boot(device);
		if (error == 0)
			error = amdrdna_a2_memory(device);
		break;
	case AMDRDNA_STAGE_A3T:
		error = amdrdna_a3t_training(device);
		break;
	case AMDRDNA_STAGE_A3:
		error = amdrdna_a3_psp(device);
		break;
	case AMDRDNA_STAGE_A4:
		error = amdrdna_a4_power(device);
		break;
	case AMDRDNA_STAGE_A5:
		error = amdrdna_a5_engines(device);
		break;
	case AMDRDNA_STAGE_A5S:
		error = amdrdna_a5s_sessions(device);
		break;
	case AMDRDNA_STAGE_A6:
		error = amdrdna_a6_takeover(device);
		if (error == 0) {
			/* The display's part of the memory clock's levels, once the display is the driver's. */
			mutex_lock(&device->display.mode_lock);
			amdrdna_memory_display_start(device);
			mutex_unlock(&device->display.mode_lock);
		}
		break;
	case AMDRDNA_STAGE_A7:
		error = amdrdna_a7_cursor(device);
		break;
	case AMDRDNA_STAGE_A8:
		error = amdrdna_a8_register(device);
		break;
	case AMDRDNA_STAGE_A9:
		error = amdrdna_a9_draw(device);
		break;
	case AMDRDNA_STAGE_A5C:
		/* The queue resets are armed by A5 and A9; the stage says so and passes. */
		amdrdna_mark("A5c ok queue resets: graphics through the scheduler, compute with the waves killed; resets so far %u",
		    (unsigned)device->spaces.resets);
		error = 0;
		break;
	case AMDRDNA_STAGE_A10:
		error = amdrdna_a10_sink(device);
		break;
	case AMDRDNA_STAGE_A11:
		error = amdrdna_a11_relight(device, 0U);
		break;
	case AMDRDNA_STAGE_A12:
		error = amdrdna_a12_mode(device);
		break;
	case AMDRDNA_STAGE_A13:
		error = amdrdna_a13_hotplug(device);
		break;
	default:
		error = ENOTSUP;
		break;
	}

	/* Reports why the stage failed. */
	if (error != 0)
		return error;

	/* Succeeded: the stage reported ok. */
	return 0;
}

/* Stops counting the GPU as attaching, once: its device is registered or never will be by the boot. */
static void
amdrdna_attach_done(
	struct amdrdna_device *device)
{
	/* Only a counted GPU is uncounted, and only once. */
	if (!device->attach_counted)
		return;

	/* The count goes down; sessiond stops waiting for this GPU. */
	device->attach_counted = false;
	kern_gpu_attach_end();
}

/* Gives back what the read-only stages took, and the record. */
static void
amdrdna_give_back(
	struct amdrdna_device *device)
{
	/* Frees the video BIOS image, if a stage kept one. */
	amdrdna_vbios_release(device);

	/* Unmaps the registers and restores the command bits. */
	amdrdna_a0_release(device);

	/* Frees the record. */
	kern_free(device);
}
