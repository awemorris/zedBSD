/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reset of the GPU and its setting up again.
 *
 * A reset runs on the bring-up thread, between stages, for one of three
 * reasons: a secure OS left by an earlier driver is found when the
 * stages that write are about to run (the boot), root asks through the
 * GPU device (GPU_DEVICE_RESET), or the GPU core asks to recover a faulted
 * GPU before a fresh open.  The thread first stops what runs (the renderer takes no
 * work, the engines are halted, the power unit is told it will be
 * unloaded, the secure OS's ring is destroyed, the interrupt ring is
 * turned off), keeps what the reset loses (the PCI configuration, the
 * resizable BARs' sizes, the system-memory table, the display pipe), asks
 * the secure OS for a mode-1 reset, gives the function its configuration
 * back, waits for the boot loader, runs the video BIOS's post on the
 * driver's runner, and sets the GPU up again as far as the stages had
 * (the memory hubs and the table, the display pipe, the secure OS and
 * its programs, the power unit, the engines and the address spaces).
 * Memory training is not run again: the post initializes the memory.
 *
 * The boot reset is allowed by amdrdna.reset=1, as is the recovery the
 * GPU core asks for on its own; root's request through the GPU device
 * always runs.  The buffers the stages took are kept across a reset and only
 * written again, since VRAM is taken as lost.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-node.h"
#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"
#include "drivers/gpu/amdrdna/amdrdna-reset.h"

/*
 * The security processor's mode-1 reset command, written to its ring
 * control word once the secure OS answers there (ready, no error), and
 * the time the GPU takes for it.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (GFX_CTRL_CMD_ID_MODE1_RST
 * 0x00070000) and psp_v11_0.c (psp_v11_0_mode1_reset: the wait for
 * MBOX_TOS_READY_FLAG under MBOX_TOS_READY_MASK, the command, 500 ms).
 */
#define RESET_MODE1_COMMAND		0x00070000U
#define RESET_MODE1_US			500000U

/*
 * The waits: the secure OS's answer before the reset (100 ms, as Linux's
 * register wait), the boot loader's ready bit after it (20 times 100 ms),
 * and the VRAM size word leaving all ones (100 ms).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (amdgpu_device_mode1_reset:
 * the memory size polled for usec_timeout, AMDGPU_MAX_USEC_TIMEOUT
 * 100000) and psp_v11_0.c (psp_v11_0_wait_for_bootloader: 20 tries).
 */
#define RESET_ANSWER_POLLS		100U
#define RESET_BOOT_POLLS		2000U
#define RESET_POLL_US			1000U
#define RESET_SIZE_POLLS		1000U
#define RESET_SIZE_STEP_US		100U
#define RESET_SIZE_GONE			0xffffffffU

/*
 * The video BIOS's third scratch word, whose bit 29 says the GPU's
 * engine hung while a reset runs, and the post routine's place in the
 * command list (0).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atombios.h (ATOM_S3_ASIC_GUI_ENGINE_HUNG),
 * drivers/gpu/drm/amd/amdgpu/amdgpu_atombios.c
 * (amdgpu_atombios_scratch_regs_engine_hung) and
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct atom_master_list_of_command_functions_v2_1: asic_init).
 */
#define RESET_SCRATCH_HUNG_WORD		3U
#define RESET_SCRATCH_HUNG		0x20000000U
#define RESET_POST_ROUTINE		0x00U

/* The page each entry of the system-memory table maps. */
#define RESET_TABLE_PAGE_BYTES		4096U

/* The names of the reasons, for the marks. */
static const char *const reset_reasons[] = {
	"boot",
	"request",
	"recovery"
};

static bool reset_in_use(struct amdrdna_device *device);
static int reset_run(struct amdrdna_device *device, enum amdrdna_reset_reason reason);
static void reset_quiet(struct amdrdna_device *device, unsigned reached);
static int reset_keep_table(struct amdrdna_device *device);
static void reset_free_table(struct amdrdna_device *device);
static int reset_mode1(struct amdrdna_device *device);
static void reset_resize_save(struct amdrdna_device *device);
static void reset_resize_restore(struct amdrdna_device *device);
static int reset_post(struct amdrdna_device *device);
static int reset_stages(struct amdrdna_device *device, unsigned reached);
static void reset_display(struct amdrdna_device *device);
static void reset_hung_mark(struct amdrdna_device *device, bool hung);
static int reset_wait(struct amdrdna_device *device, uint32_t offset, uint32_t mask, unsigned polls);
static uint32_t reset_mailbox_read(struct amdrdna_device *device, uint32_t offset);
static void reset_mailbox_write(struct amdrdna_device *device, uint32_t offset, uint32_t value);

/*
 * Prepares the reset's state at the attach: its lock and whether the
 * boot line lets a reset run on its own (amdrdna.reset=1).
 */
void
amdrdna_reset_init(
	struct amdrdna_device *device)
{
	unsigned allowed;

	/* The callers' lock, nothing asked yet. */
	(void)mutex_init(&device->reset.lock, LOCK_RANK_DEVICE, "amdrdna reset");
	device->reset.requested = false;
	device->reset.running = false;
	device->reset.finished = 0U;
	device->reset.result = 0;

	/* The boot line's word. */
	allowed = amdrdna_parameter_number("amdrdna.reset", 0U);
	device->reset.allowed = false;
	if (allowed != 0U)
		device->reset.allowed = true;
}

/*
 * Reports whether a reset may run without root asking: the boot's reset
 * of a secure OS left running, and the GPU core's recovery.
 */
bool
amdrdna_reset_allowed(
	const struct amdrdna_device *device)
{
	/* The boot line's word, read at the attach. */
	if (device->reset.allowed)
		return true;

	/* Not without amdrdna.reset=1. */
	return false;
}

/*
 * Asks the bring-up thread for a reset and waits for it to end; one
 * caller at a time.  milliseconds gets how long the reset took (may be
 * NULL).
 *
 * Returns the reset's result (0 when the GPU was set up again), ENODEV
 * when the thread no longer runs, or EBUSY when a stage has not got as
 * far as the secure OS a mode-1 reset needs or, for root's request, while
 * a session of the GPU device is open.
 */
int
amdrdna_reset_request(
	struct amdrdna_device *device,
	enum amdrdna_reset_reason reason,
	uint32_t *milliseconds)
{
	unsigned long irq;
	uint64_t sequence;
	uint64_t start;
	bool busy;
	int error;

	/* Root's request waits until no session uses the GPU (the core's recovery asks only then). */
	if (reason == AMDRDNA_RESET_REQUEST) {
		busy = reset_in_use(device);
		if (busy)
			return EBUSY;
	}

	/* One caller at a time waits for the thread. */
	mutex_lock(&device->reset.lock);

	/* Asks the thread, then waits for the reset to finish or the thread to stop. */
	irq = spin_lock_irqsave(&device->state_lock);

	error = 0;
	if (device->stopping || !device->thread_running) {
		error = ENODEV;
	} else if (device->stage < AMDRDNA_STAGE_A3 && device->secure_os_word == 0U) {
		error = EBUSY;
	} else {
		start = device->reset.finished;
		device->reset.requested = true;
		device->reset.reason = reason;
		waitq_wake_all(&device->state_waitq);
		while (device->reset.finished == start && !device->stopping) {
			sequence = waitq_sequence(&device->state_waitq);
			(void)waitq_sleep(&device->state_waitq, &device->state_lock, sequence, 0U, 0U);
		}
		error = ENODEV;
		if (device->reset.finished != start)
			error = device->reset.result;
	}

	spin_unlock_irqrestore(&device->state_lock, irq);

	/* How long it took. */
	if (milliseconds != NULL)
		*milliseconds = device->reset.last_ms;

	mutex_unlock(&device->reset.lock);

	/* Reports a reset that was refused or failed. */
	if (error != 0)
		return error;

	/* Succeeded: the GPU is set up again. */
	return 0;
}

/*
 * Serves a reset asked for, on the bring-up thread, which holds the
 * device's state lock when it calls this and gets it back held; the lock
 * is let go while the reset runs.  Returns the irq word of the held lock.
 */
unsigned long
amdrdna_reset_serve(
	struct amdrdna_device *device,
	unsigned long irq)
{
	enum amdrdna_reset_reason reason;
	int error;

	/* Takes the request; the reset runs from now on. */
	reason = device->reset.reason;
	device->reset.requested = false;
	device->reset.running = true;

	spin_unlock_irqrestore(&device->state_lock, irq);

	/* Runs it without the lock: it sleeps and takes seconds. */
	error = reset_run(device, reason);

	/* Publishes the result and wakes the caller. */
	irq = spin_lock_irqsave(&device->state_lock);

	device->reset.running = false;
	device->reset.result = error;
	device->reset.finished++;
	waitq_wake_all(&device->state_waitq);

	/* Succeeded: the lock is held again. */
	return irq;
}

/*
 * Resets the GPU at the boot, before stage A2, when a secure OS left by
 * an earlier driver still runs: the reset and the post, and the display
 * pipe lit again; the stages from A2 on then run as after a cold boot.
 *
 * Returns 0, or the error of the step that failed (the stages that write
 * then do not run).
 */
int
amdrdna_reset_boot(
	struct amdrdna_device *device)
{
	int error;

	/* The reset, on this thread, which runs the stages. */
	error = reset_run(device, AMDRDNA_RESET_BOOT);
	if (error != 0)
		return error;

	/* Succeeded: the GPU is as after a cold boot. */
	return 0;
}

/* Tells whether a session uses the GPU: an address space, or the display's lease. */
static bool
reset_in_use(
	struct amdrdna_device *device)
{
	struct amdrdna_node *node;
	unsigned long irq;
	unsigned count;
	bool leased;

	/* The address spaces of the sessions. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	count = device->spaces.count;

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* A space means a session. */
	if (count != 0U)
		return true;

	/* The display's lease. */
	node = device->node;
	if (node == NULL)
		return false;
	mutex_lock(&node->display_lock);

	leased = false;
	if (node->owner != NULL)
		leased = true;

	mutex_unlock(&node->display_lock);

	/* Succeeded: whether the display is leased. */
	return leased;
}

/*
 * Runs one reset: stops what runs, keeps what the reset loses, resets,
 * posts, and sets up again what the stages had.
 */
static int
reset_run(
	struct amdrdna_device *device,
	enum amdrdna_reset_reason reason)
{
	uint64_t started_us;
	uint64_t ended_us;
	unsigned reached;
	int error;

	/* Says the reset starts, and how far the stages had got. */
	started_us = amdrdna_display_now_us();
	reached = device->stage;
	amdrdna_mark("reset begin (%s), stages up to %s, sos=0x%08x", reset_reasons[reason], amdrdna_stage_name(reached),
	    (unsigned)reset_mailbox_read(device, AMDRDNA_PSP_REG_SECURE_OS_ALIVE));

	/* A mode-1 reset is asked of the video BIOS's image and a secure OS; neither may be missing. */
	if (device->vbios == NULL) {
		amdrdna_mark("reset fail: no video BIOS image (stage A1v first)");
		return ENOENT;
	}

	/* Keeps the system-memory table; a failure here leaves the GPU as it was. */
	error = reset_keep_table(device);
	if (error != 0) {
		amdrdna_mark("reset fail: the table's copy not taken (%d)", error);
		return error;
	}

	/* Keeps the display pipe while it runs, then stops what runs on the GPU. */
	amdrdna_reset_display_save(device);
	if (reason != AMDRDNA_RESET_BOOT)
		reset_quiet(device, reached);

	/* The reset. */
	error = reset_mode1(device);

	/* The post. */
	if (error == 0)
		error = reset_post(device);

	/* The display pipe at the boot; the stages that ran, otherwise. */
	if (error == 0 && reason == AMDRDNA_RESET_BOOT)
		reset_display(device);
	else if (error == 0)
		error = reset_stages(device, reached);

	/* The table's copy is written back or no longer needed. */
	reset_free_table(device);

	/* A failed reset leaves the GPU faulted. */
	ended_us = amdrdna_display_now_us();
	device->reset.last_ms = (uint32_t)((ended_us - started_us) / 1000U);
	if (error != 0) {
		amdrdna_mark("reset fail (%d) after %ums; the GPU stays faulted", error, (unsigned)device->reset.last_ms);
		if (device->node != NULL)
			amdrdna_node_device_failed(device, error);
		return error;
	}

	/* Marks the result. */
	device->reset.resets++;
	amdrdna_mark("reset ok (%s) in %ums, resets %u, stages up to %s set up again",
	    reset_reasons[reason],
	    (unsigned)device->reset.last_ms,
	    (unsigned)device->reset.resets,
	    amdrdna_stage_name(reached));

	/* Succeeded: the GPU runs again. */
	return 0;
}

/*
 * Stops what runs on the GPU before the reset, from the last stage down:
 * the video engine stops (its restart thread ends), the renderer takes no
 * work and no address space runs, the engines are halted, the power unit
 * is told it will be unloaded, the secure OS's ring is destroyed, and the
 * hubs' fault interrupts and the interrupt ring are turned off.  The
 * other threads stay; what the stages took is kept.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
 * (amdgpu_device_pre_asic_reset, amdgpu_device_ip_suspend: the blocks
 * suspended in the reverse order of their init before the reset).
 */
static void
reset_quiet(
	struct amdrdna_device *device,
	unsigned reached)
{
	/*
	 * The video engine first, before the secure OS's region it runs its
	 * program from is torn down and before the interrupt ring stops: new
	 * work is turned away, its waiters give up, its restart thread ends.
	 */
	if (reached >= AMDRDNA_STAGE_A5)
		amdrdna_vcn_stop(device);

	/* The renderer and the address spaces take nothing new (stage A5s let them). */
	if (device->node != NULL)
		amdrdna_runtime_hold(device->node, true);
	if (reached >= AMDRDNA_STAGE_A5S)
		amdrdna_spaces_fault(device);

	/* The engines, the power unit and the secure OS's ring, as far as they ran. */
	if (reached >= AMDRDNA_STAGE_A5)
		amdrdna_engines_stop(device);
	if (reached >= AMDRDNA_STAGE_A4) {
		amdrdna_memory_display_end(device);
		amdrdna_power_stop(device);
	}
	if (reached >= AMDRDNA_STAGE_A3)
		amdrdna_psp_stop(device);

	/* The fault interrupts and the interrupt ring. */
	if (reached >= AMDRDNA_STAGE_A2) {
		amdrdna_fault_interrupts(device, false);
		amdrdna_ih_quiet(device);
	}
}

/* Keeps a copy of the system-memory table, which lives in VRAM; nothing before stage A2 built it. */
static int
reset_keep_table(
	struct amdrdna_device *device)
{
	uint64_t entries;
	int error;

	/* Nothing to keep before the table exists. */
	device->reset.table_copy = NULL;
	device->reset.table_entries = 0U;
	if (!device->memory_ready)
		return 0;

	/* The copy's memory. */
	entries = AMDRDNA_GART_BYTES / RESET_TABLE_PAGE_BYTES;
	device->reset.table_copy = kern_malloc((size_t)entries * sizeof(uint64_t));
	if (device->reset.table_copy == NULL)
		return ENOMEM;

	/* The entries, read through the window. */
	error = amdrdna_gart_save(device, device->reset.table_copy, entries);
	if (error != 0) {
		reset_free_table(device);
		return error;
	}

	/* Succeeded: the entries are kept. */
	device->reset.table_entries = entries;
	return 0;
}

/* Frees the copy of the system-memory table. */
static void
reset_free_table(
	struct amdrdna_device *device)
{
	/* Nothing held without a copy. */
	if (device->reset.table_copy == NULL)
		return;

	/* The copy goes. */
	kern_free(device->reset.table_copy);
	device->reset.table_copy = NULL;
	device->reset.table_entries = 0U;
}

/*
 * Resets the GPU through the security processor (mode 1): waits for the
 * secure OS to answer, marks the engine hung for the video BIOS, keeps
 * the PCI configuration and the resizable BARs' sizes, stops the GPU's
 * bus mastering, writes the reset command, waits, gives the function its
 * sizes and its configuration back, and waits for the boot loader and
 * the VRAM size word.  The secure OS must be gone after it.
 *
 * Returns 0, EIO when the secure OS does not answer or is still there,
 * or ETIMEDOUT when the boot loader or the VRAM do not come back.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (amdgpu_device_mode1_reset),
 * psp_v11_0.c (psp_v11_0_mode1_reset, psp_v11_0_wait_for_bootloader) and
 * amdgpu_atombios.c (amdgpu_atombios_scratch_regs_engine_hung).
 */
static int
reset_mode1(
	struct amdrdna_device *device)
{
	uint32_t word;
	unsigned poll;
	int error;

	/* The secure OS must answer on its ring control word. */
	error = reset_wait(device, AMDRDNA_PSP_REG_RING_CONTROL, AMDRDNA_PSP_READY_MASK, RESET_ANSWER_POLLS);
	if (error != 0) {
		amdrdna_mark("reset fail: the secure OS does not answer (0x%08x)",
		    (unsigned)reset_mailbox_read(device, AMDRDNA_PSP_REG_RING_CONTROL));
		return EIO;
	}

	/* The engine marked hung for the video BIOS while the reset runs. */
	reset_hung_mark(device, true);

	/* The function's configuration and its resizable BARs' sizes. */
	error = drv_pci_device_save_state(device->pci, &device->reset.pci);
	if (error != 0) {
		amdrdna_mark("reset fail: the PCI configuration not kept (%d)", error);
		reset_hung_mark(device, false);
		return error;
	}
	reset_resize_save(device);

	/* No DMA from the GPU while it resets. */
	(void)drv_pci_device_set_bus_master(device->pci, false);

	/* The reset, and the time it takes. */
	amdrdna_mark("reset: mode1 now");
	reset_mailbox_write(device, AMDRDNA_PSP_REG_RING_CONTROL, RESET_MODE1_COMMAND);
	kern_usleep_range(RESET_MODE1_US, RESET_MODE1_US + RESET_MODE1_US / 8U);

	/* The sizes first, then the configuration (the BARs, the command word last, the message interrupts). */
	reset_resize_restore(device);
	error = drv_pci_device_restore_state(device->pci, &device->reset.pci);
	if (error != 0)
		amdrdna_mark("reset note: the PCI configuration came back with an error (%d)", error);

	/* The boot loader ready again. */
	error = reset_wait(device, AMDRDNA_PSP_REG_BOOT_COMMAND, AMDRDNA_PSP_READY_MASK, RESET_BOOT_POLLS);
	if (error != 0) {
		amdrdna_mark("reset fail: the boot loader did not come back (0x%08x)",
		    (unsigned)reset_mailbox_read(device, AMDRDNA_PSP_REG_BOOT_COMMAND));
		return ETIMEDOUT;
	}

	/* The VRAM size word readable again. */
	word = RESET_SIZE_GONE;
	for (poll = 0U; poll < RESET_SIZE_POLLS; poll++) {
		/* Stops once the word reads. */
		word = amdrdna_read(device, AMDRDNA_NBIO_REG_VRAM_SIZE_MIB);
		if (word != RESET_SIZE_GONE)
			break;

		/* Sleeps one step. */
		kern_usleep_range(RESET_SIZE_STEP_US, RESET_SIZE_STEP_US * 2U);
	}
	if (word == RESET_SIZE_GONE) {
		amdrdna_mark("reset fail: the VRAM size word did not come back");
		return ETIMEDOUT;
	}

	/* The secure OS must be gone. */
	device->secure_os_word = reset_mailbox_read(device, AMDRDNA_PSP_REG_SECURE_OS_ALIVE);
	if (device->secure_os_word != 0U) {
		amdrdna_mark("reset fail: the secure OS is still there (0x%08x)", (unsigned)device->secure_os_word);
		return EIO;
	}

	/* The engine no longer marked hung. */
	reset_hung_mark(device, false);
	device->psp.booted = false;

	/* Marks the reset. */
	amdrdna_mark("reset: mode1 done, boot=0x%08x vram=%uMiB resizable=%u",
	    (unsigned)reset_mailbox_read(device, AMDRDNA_PSP_REG_BOOT_COMMAND),
	    (unsigned)word,
	    device->reset.resize_count);

	/* Succeeded: the GPU is as after power-on, before the post. */
	return 0;
}

/*
 * Keeps the control word of every resizable BAR the function lists: the
 * PCI configuration the core keeps does not hold them, and a reset
 * gives the BARs their power-on sizes.
 */
static void
reset_resize_save(
	struct amdrdna_device *device)
{
	uint32_t control;
	unsigned capability;
	unsigned count;
	unsigned entry;
	int error;

	/* A function without the capability keeps its sizes. */
	device->reset.resize_count = 0U;
	capability = device->resize_capability;
	if (capability == 0U)
		return;

	/* The number of entries, from the first entry's control word. */
	error = drv_pci_device_config_read32(device->pci, capability + AMDRDNA_PCI_RESIZE_CONTROL, &control);
	if (error != 0)
		return;
	count = (control >> AMDRDNA_PCI_RESIZE_CONTROL_COUNT_SHIFT) & AMDRDNA_PCI_RESIZE_CONTROL_COUNT_MASK;
	if (count > AMDRDNA_RESET_RESIZE_ENTRIES)
		count = AMDRDNA_RESET_RESIZE_ENTRIES;

	/* Each entry's control word. */
	for (entry = 0U; entry < count; entry++) {
		/* Reads one; a failed read keeps the ones before it. */
		error = drv_pci_device_config_read32(device->pci,
		    capability + AMDRDNA_PCI_RESIZE_CONTROL + entry * AMDRDNA_PCI_RESIZE_ENTRY_BYTES,
		    &device->reset.resize_controls[entry]);
		if (error != 0)
			break;
		device->reset.resize_count = entry + 1U;
	}
}

/*
 * Writes the resizable BARs' control words back where the reset changed
 * them, before the BARs' addresses are written back.
 *
 * Procedure referenced: Linux v6.19 drivers/pci/pci.c
 * (pci_restore_rebar_state: the size written back before the BAR).
 */
static void
reset_resize_restore(
	struct amdrdna_device *device)
{
	uint32_t control;
	unsigned offset;
	unsigned entry;
	int error;

	/* Each kept entry. */
	for (entry = 0U; entry < device->reset.resize_count; entry++) {
		/* Reads it; an unchanged one is left alone. */
		offset = device->resize_capability + AMDRDNA_PCI_RESIZE_CONTROL + entry * AMDRDNA_PCI_RESIZE_ENTRY_BYTES;
		error = drv_pci_device_config_read32(device->pci, offset, &control);
		if (error != 0 || control == device->reset.resize_controls[entry])
			continue;

		/* Writes the kept word back and says so. */
		(void)drv_pci_device_config_write32(device->pci, offset, device->reset.resize_controls[entry]);
		amdrdna_mark("reset: resizable bar %u control 0x%08x -> 0x%08x", entry, (unsigned)control,
		    (unsigned)device->reset.resize_controls[entry]);
	}
}

/*
 * Runs the video BIOS's post on the driver's runner with the boot engine
 * and memory clocks the image's firmware table names, then reads the
 * scratch word that says the board is posted.
 *
 * Returns 0, ENOENT or ENOTSUP for an image without a post routine of a
 * version the driver packs, EINVAL without the boot clocks, or the
 * runner's error.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_atomfirmware.c
 * (amdgpu_atomfirmware_asic_init) and amdgpu_device.c
 * (amdgpu_device_asic_init after a mode-1 reset).
 */
static int
reset_post(
	struct amdrdna_device *device)
{
	uint32_t words[AMDRDNA_RESET_POST_SPACE_WORDS];
	uint32_t engine;
	uint32_t memory;
	uint32_t steps;
	uint32_t word;
	uint64_t started_us;
	uint64_t ended_us;
	int error;

	/* The routine must be one whose parameters this driver packs. */
	error = amdrdna_reset_post_format(device->vbios, device->vbios_bytes, device->vbios_info.commands[AMDRDNA_VBIOS_COMMAND_POST].offset);
	if (error != 0) {
		amdrdna_mark("reset fail: no post routine of version 2.1 (%d)", error);
		return error;
	}

	/* The boot clocks. */
	engine = device->vbios_info.boot_engine_clock_10khz;
	memory = device->vbios_info.boot_memory_clock_10khz;
	if (engine == 0U || memory == 0U) {
		amdrdna_mark("reset fail: the image names no boot clocks");
		return EINVAL;
	}

	/* The post waits first for the board firmware's ready word, which comes back after the reset. */
	error = reset_wait(device, AMDRDNA_PSP_REG_PLATFORM_READY, AMDRDNA_PSP_DONE, RESET_BOOT_POLLS);
	if (error != 0) {
		amdrdna_mark("reset fail: the board firmware's ready word did not come back (0x%08x)",
		    (unsigned)reset_mailbox_read(device, AMDRDNA_PSP_REG_PLATFORM_READY));
		return ETIMEDOUT;
	}

	/* The post. */
	kern_memset(words, 0, sizeof(words));
	amdrdna_reset_post_words(words, engine, memory);
	started_us = amdrdna_display_now_us();
	error = amdrdna_bios_run(device, RESET_POST_ROUTINE, words, AMDRDNA_RESET_POST_SPACE_WORDS, &steps);
	ended_us = amdrdna_display_now_us();
	if (error != 0) {
		amdrdna_mark("reset fail: the post stopped (%d) after %u steps", error, (unsigned)steps);
		return error;
	}

	/* Whether the board says it is posted. */
	word = amdrdna_block_read(device,
	    AMDRDNA_BLOCK_NBIO,
	    AMDRDNA_NBIO_WINDOW_SCRATCH,
	    AMDRDNA_NBIO_REG_BIOS_SCRATCH_FIRST + AMDRDNA_BIOS_SCRATCH_POST);
	device->posted = false;
	if ((word & AMDRDNA_BIOS_SCRATCH_7_POSTED) != 0U)
		device->posted = true;

	/* Marks the post. */
	amdrdna_mark("reset: post ok in %llums, steps=%u engine=%uMHz memory=%uMHz posted=%u scratch7=0x%08x",
	    (unsigned long long)((ended_us - started_us) / 1000U),
	    (unsigned)steps,
	    (unsigned)(engine / 100U),
	    (unsigned)(memory / 100U),
	    device->posted ? 1U : 0U,
	    (unsigned)word);

	/* Succeeded: the GPU is initialized. */
	return 0;
}

/*
 * Sets the GPU up again as far as the stages had got: the memory hubs
 * and the table with the interrupt ring (A2), the display pipe, the
 * secure OS and its programs (A3), the power unit (A4), the engines (A5),
 * and the address spaces and the renderer (A5s).  Memory training (A3t)
 * is not run again.
 *
 * Returns 0, or the error of the stage that failed.
 */
static int
reset_stages(
	struct amdrdna_device *device,
	unsigned reached)
{
	int error;

	/* The memory hubs, the table and the interrupt ring. */
	if (reached >= AMDRDNA_STAGE_A2) {
		error = amdrdna_a2_restart(device, device->reset.table_copy, device->reset.table_entries);
		if (error != 0)
			return error;
	}

	/* The display pipe, which reads through the hubs. */
	reset_display(device);

	/* Memory training is not run again; the post initialized the memory. */
	if (reached >= AMDRDNA_STAGE_A3T)
		amdrdna_mark("reset: A3t not run again (the post trained the memory)");

	/* The secure OS and its programs. */
	if (reached >= AMDRDNA_STAGE_A3) {
		error = amdrdna_a3_psp(device);
		if (error != 0)
			return error;

		/* The display's events that came while the interrupt ring was off, acknowledged. */
		amdrdna_reset_display_rearm(device);
	}

	/* The power unit, its watch started again. */
	if (reached >= AMDRDNA_STAGE_A4) {
		error = amdrdna_power_rearm(device);
		if (error != 0) {
			amdrdna_mark("reset fail: the power watch of the stop still runs");
			return error;
		}
		error = amdrdna_a4_power(device);
		if (error != 0)
			return error;

		/* The display's part of the memory clock's levels again, when the display is the driver's. */
		mutex_lock(&device->display.mode_lock);
		amdrdna_memory_display_start(device);
		mutex_unlock(&device->display.mode_lock);
	}

	/* The engines. */
	if (reached >= AMDRDNA_STAGE_A5) {
		error = amdrdna_a5_engines(device);
		if (error != 0)
			return error;
	}

	/* The address spaces and the renderer take work again. */
	if (reached >= AMDRDNA_STAGE_A5S) {
		error = amdrdna_spaces_restart(device);
		if (error != 0)
			return error;
		if (device->node != NULL)
			amdrdna_runtime_hold(device->node, false);
	}

	/* Succeeded: the stages are set up again. */
	return 0;
}

/* Lights the display pipe again; a failure leaves the screen dark but the GPU running. */
static void
reset_display(
	struct amdrdna_device *device)
{
	int error;

	/* The pipe from its kept words. */
	error = amdrdna_reset_display_restore(device);
	if (error != 0)
		amdrdna_mark("reset note: the display stays dark (%d); the GPU runs on", error);
}

/* Sets or clears the video BIOS's mark that the engine hung. */
static void
reset_hung_mark(
	struct amdrdna_device *device,
	bool hung)
{
	uint32_t word;

	/* The third scratch word, its hung bit as asked. */
	word = amdrdna_block_read(device,
	    AMDRDNA_BLOCK_NBIO,
	    AMDRDNA_NBIO_WINDOW_SCRATCH,
	    AMDRDNA_NBIO_REG_BIOS_SCRATCH_FIRST + RESET_SCRATCH_HUNG_WORD);
	if (hung)
		word |= RESET_SCRATCH_HUNG;
	else
		word &= ~RESET_SCRATCH_HUNG;
	amdrdna_block_write(device,
	    AMDRDNA_BLOCK_NBIO,
	    AMDRDNA_NBIO_WINDOW_SCRATCH,
	    AMDRDNA_NBIO_REG_BIOS_SCRATCH_FIRST + RESET_SCRATCH_HUNG_WORD,
	    word);
}

/*
 * Waits for a mailbox word to read ready (bit 31) with the other bits of
 * mask clear (a zero status in bits 15:0 for the secure OS's and the boot
 * loader's words), one millisecond a poll.  Returns 0 or ETIMEDOUT.
 */
static int
reset_wait(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t mask,
	unsigned polls)
{
	uint32_t word;
	unsigned poll;

	/* Looks, then sleeps, until the word reads ready. */
	for (poll = 0U; poll < polls; poll++) {
		/* Stops once ready. */
		word = reset_mailbox_read(device, offset);
		if ((word & mask) == AMDRDNA_PSP_READY)
			return 0;

		/* Sleeps one step. */
		kern_usleep_range(RESET_POLL_US, RESET_POLL_US + RESET_POLL_US / 4U);
	}

	/* The word never read ready. */
	return ETIMEDOUT;
}

/* Reads one of the security processor's mailbox words. */
static uint32_t
reset_mailbox_read(
	struct amdrdna_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* The words are in the processor's first window. */
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, offset);

	/* Succeeded: the word's value. */
	return value;
}

/* Writes one of the security processor's mailbox words. */
static void
reset_mailbox_write(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* The words are in the processor's first window. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, offset, value);
}
