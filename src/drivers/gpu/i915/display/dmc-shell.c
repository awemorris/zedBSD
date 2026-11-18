/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DMC loader of the display and the pipe DMC switches of the mode
 * set (see dmc.h).
 *
 * The loader takes a power reference that keeps every well and the exit
 * from the DC states up, and queues a work on the DMC's workqueue that
 * acquires the firmware, has the xd DMC code parse and load it, and
 * gives the reference back after a complete load.  The parse and the
 * load are the xd DMC code's (xd-dmc.c); this file keeps the older
 * code's view of them (struct i915_dmc) in step for the logs and the
 * mode set, and switches the pipe DMCs through the mode set's register
 * access.
 */

#include "internal.h"
#include "dmc.h"
#include "display-power.h"
#include "xd-dmc.h"
#include "xd.h"

#include "../intel/display-regs-xd.h"

#include "../firmware.h"
#include "../workqueue.h"

#include <kern/klog.h>
#include <kern/kcrt.h>

#include <uapi/errno.h>

/*
 * The older firmware of Alder Lake-P, loaded when the platform's own is
 * missing.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (ADLP_DMC_FALLBACK_PATH).
 */
#define I915_DMC_SHELL_ADLP_FALLBACK	"i915/adlp_dmc_ver2_16.bin"

static struct i915_display *i915_dmc_shell_display(struct i915_dmc_dev *dev);
static void i915_dmc_shell_copy(struct i915_dmc_dev *dev);
static void i915_dmc_shell_log_parse(const struct i915_xd_dmc *parsed);
static void i915_dmc_shell_load(struct i915_dmc_dev *dev);
static void i915_dmc_shell_get_ref(struct i915_dmc_dev *dev);
static void i915_dmc_shell_put_ref(struct i915_dmc_dev *dev);
static void i915_dmc_shell_work(void *context);

/*
 * Tells whether the main DMC's program was taken.
 */
int
drv_i915_dmc_has_payload(
	const struct i915_dmc *dmc)
{
	/* Only a taken main program counts. */
	if (dmc->dmc_info[I915_DMC_FW_MAIN].payload == NULL)
		return 0;

	/* Succeeded: the main program is there. */
	return 1;
}

/*
 * Starts the DMC loader.
 *
 * Takes the loader's own power reference (kept when the load fails),
 * names the platform's firmware and queues the load work.  A display
 * without an xd description loads nothing.
 */
void
drv_i915_dmc_init(
	struct i915_dmc_dev *dev,
	struct i915_workqueue *wq,
	struct i915_mmio *mmio,
	struct i915_display_power *power,
	struct i915_xd *xd)
{
	/* Binds the loader and starts its diagnostics from zero. */
	dev->power = power;
	dev->xd = xd;
	dev->m = mmio;
	dev->wq = wq;
	dev->dmc_wakeref_held = 0;
	dev->dc_state = 0xffffffffu;
	dev->work_submitted = 0;
	dev->worker_started = 0;
	dev->firmware_acquired = 0;
	dev->fallback_requested = 0;
	dev->main_payload_present = 0;
	dev->load_seq_completed_flag = 0;
	dev->first_fault = 0;
	kern_memset(&dev->dmc, 0, sizeof(dev->dmc));

	/* A display without a description has no firmware to load. */
	if (xd == NULL || !xd->chosen) {
		kern_logf("i915: dmc: no platform description, no DMC firmware\n");
		return;
	}

	/* Starts the xd DMC state empty and names the platform's firmware. */
	drv_i915_xd_dmc_init(&xd->dmc);
	dev->fw_path = xd->choice.platform->dmc_path;
	dev->is_alderlake_p = 0;
	if (xd->choice.platform->kind == I915_XD_KIND_ADLP)
		dev->is_alderlake_p = 1;

	/* Takes the loader's own reference before the work can run. */
	i915_dmc_shell_get_ref(dev);

	/*
	 * Queues the work only after everything it reads is published: it may
	 * start on another CPU before this returns, and queueing orders the
	 * writes before it.
	 */
	drv_i915_work_init(&dev->work, i915_dmc_shell_work, dev);
	dev->work_submitted = 1;
	(void)drv_i915_queue_work(wq, &dev->work);
}

/*
 * Loads the DMC's programs again after a resume: DC9 lost them.
 *
 * Reports ENOENT when there is no program (the display stays without DC
 * states, as at boot), EIO when the load did not complete.
 */
int
drv_i915_dmc_resume(
	struct i915_dmc_dev *dev)
{
	/* A display without a program has nothing to load. */
	if (!dev->main_payload_present)
		return ENOENT;

	/* Loads the programs again. */
	i915_dmc_shell_load(dev);
	if (!dev->dmc.load_seq_completed) {
		kern_logf("i915: dmc: the program did not load again after the resume\n");
		return EIO;
	}

	/* Succeeded: the DMC runs its program again. */
	kern_logf("i915: dmc: program loaded again after the resume\n");
	return 0;
}

/*
 * Stops the DMC loader: waits for the work (a flush, not a cancel), gives
 * back a reference the work kept, and forgets the programs.
 */
void
drv_i915_dmc_fini(
	struct i915_dmc_dev *dev,
	uint64_t deadline)
{
	struct i915_display *display;

	/* Waits for a queued work to complete. */
	if (dev->work_submitted)
		(void)drv_i915_flush_work(dev->wq, &dev->work, deadline);

	/* Gives back a reference the work left held (no program, or a load that did not complete). */
	i915_dmc_shell_put_ref(dev);

	/* Forgets the programs and empties the display's storage. */
	if (dev->xd != NULL)
		drv_i915_xd_dmc_init(&dev->xd->dmc);
	display = i915_dmc_shell_display(dev);
	display->g_dmc_arena_used = 0u;
	i915_dmc_shell_copy(dev);
}

/* Finds the display a DMC loader belongs to. */
static struct i915_display *
i915_dmc_shell_display(
	struct i915_dmc_dev *dev)
{
	struct i915_display *display;

	/* The loader is the dmc_dev member of its display. */
	display = container_of(dev, struct i915_display, dmc_dev);

	/* Succeeded: the display. */
	return display;
}

/*
 * Copies what the xd DMC code found and did into the older code's view:
 * the version, each DMC's entry, program and register count, and the
 * last load's diagnostics.
 */
static void
i915_dmc_shell_copy(
	struct i915_dmc_dev *dev)
{
	const struct i915_xd_dmc *parsed;
	const struct i915_xd_dmc_program *program;
	struct i915_dmc_info *info;
	unsigned id;

	/* A loader without the xd state has nothing to copy. */
	if (dev->xd == NULL)
		return;
	parsed = &dev->xd->dmc;

	/* The firmware and its package. */
	dev->dmc.version = parsed->version;
	dev->dmc.package_header_ver = parsed->package_version;
	dev->dmc.num_entries = parsed->entries;

	/* Each DMC's entry and program. */
	for (id = 0u; id < I915_DMC_FW_MAX; id++) {
		program = &parsed->program[id];
		info = &dev->dmc.dmc_info[id];
		info->present = program->chosen;
		info->dmc_offset = program->offset;
		info->mmio_count = program->registers;
		info->start_mmioaddr = program->start;
		info->dmc_fw_size = program->dwords;
		info->payload = program->payload;
		info->payload_size = program->dwords * 4u;
		info->header_ver = (int)program->header_version;
	}

	/* The last load. */
	dev->dmc.payload_writes = parsed->program_writes;
	dev->dmc.aux_writes = parsed->register_writes;
	dev->dmc.evt_disable_writes = parsed->handler_writes;
	dev->dmc.psum = parsed->program_sum;
	dev->dmc.asum = parsed->register_sum;
	dev->dmc.load_seq_completed = parsed->loaded;
}

/* Logs why a firmware file, or a program in it, was not taken. */
static void
i915_dmc_shell_log_parse(
	const struct i915_xd_dmc *parsed)
{
	unsigned id;

	/* The file as a whole. */
	switch (parsed->file_refused) {
	case I915_XD_DMC_FILE_CSS_LENGTH:
		kern_logf("i915: dmc: wrong CSS header length (%u bytes)\n", parsed->file_found);
		break;
	case I915_XD_DMC_FILE_PACKAGE_VERSION:
		kern_logf("i915: dmc: unknown package header version %u\n", parsed->file_found);
		break;
	case I915_XD_DMC_FILE_PACKAGE_LENGTH:
		kern_logf("i915: dmc: wrong package header length (%u bytes)\n", parsed->file_found);
		break;
	default:
		break;
	}

	/* Each program that was chosen but not taken. */
	for (id = 0u; id < I915_XD_DMC_COUNT; id++) {
		if (!parsed->program[id].chosen)
			continue;
		if (parsed->program[id].refused == I915_XD_DMC_TAKEN)
			continue;

		/* Logs a program the load did not take. */
		kern_logf("i915: dmc: program %u not taken (reason %d)\n", id, parsed->program[id].refused);
	}
}

/* Loads the taken programs through the xd DMC code and copies what the load did. */
static void
i915_dmc_shell_load(
	struct i915_dmc_dev *dev)
{
	struct i915_xd *xd;

	/* Loads the programs; the load leaves no DC state written. */
	xd = dev->xd;
	drv_i915_xd_dmc_load(&xd->dmc, &xd->io, xd->choice.platform);
	dev->dc_state = 0u;
	i915_dmc_shell_copy(dev);
}

/* Takes the loader's own power reference, which keeps every well and the exit from DC states up. */
static void
i915_dmc_shell_get_ref(
	struct i915_dmc_dev *dev)
{
	/* Takes every pipe's wells and the exit from the DC states; the flag says the loader holds them. */
	(void)drv_i915_display_power_get(dev->power, I915_DISPLAY_POWER_START);
	dev->dmc_wakeref_held = 1;
}

/* Gives the loader's own power reference back, when it is held. */
static void
i915_dmc_shell_put_ref(
	struct i915_dmc_dev *dev)
{
	/* Only a held reference is given back. */
	if (!dev->dmc_wakeref_held)
		return;

	/* Gives them back; the flag says the loader no longer holds them. */
	drv_i915_display_power_put(dev->power, I915_DISPLAY_POWER_START);
	dev->dmc_wakeref_held = 0;
}

/*
 * The load work: acquires the firmware (Alder Lake-P falls back to its
 * older one), parses it into the display's storage, loads it, and gives
 * the loader's reference back only after a complete load.
 */
static void
i915_dmc_shell_work(
	void *context)
{
	struct i915_dmc_dev *dev;
	struct i915_display *display;
	struct i915_xd *xd;
	struct i915_firmware fw;
	struct i915_xd_dmc_store store;
	int request_result;

	/* Resolves the loader and its display. */
	dev = context;
	dev->worker_started = 1;
	display = i915_dmc_shell_display(dev);
	xd = dev->xd;

	/* Acquires the platform's firmware. */
	request_result = drv_i915_firmware_request(&fw, dev->fw_path);

	/* Alder Lake-P alone has an older firmware to fall back to. */
	if (request_result == ENOENT && dev->is_alderlake_p) {
		dev->fallback_requested = 1;
		request_result = drv_i915_firmware_request(&fw, I915_DMC_SHELL_ADLP_FALLBACK);
		if (request_result == 0)
			dev->fw_path = I915_DMC_SHELL_ADLP_FALLBACK;
	}

	/* Parses what was acquired into the display's storage. */
	if (request_result == 0) {
		dev->firmware_acquired = 1;
		display->g_dmc_arena_used = 0u;
		store.bytes = display->g_dmc_arena;
		store.room = sizeof(display->g_dmc_arena);
		store.used = 0u;
		(void)drv_i915_xd_dmc_parse(&xd->dmc, xd->choice.platform, xd->choice.step, fw.data, fw.size, &store);
		display->g_dmc_arena_used = (unsigned)store.used;
		i915_dmc_shell_log_parse(&xd->dmc);
	}

	/* Records whether the main program came out. */
	dev->main_payload_present = drv_i915_xd_dmc_has_main(&xd->dmc);
	i915_dmc_shell_copy(dev);
	if (dev->main_payload_present)
		kern_logf("i915: dmc worker: proceeding to load\n");

	/*
	 * Loads the programs.  A complete load gives the loader's reference
	 * back; a load that did not complete keeps it.  Without a main
	 * program (absent or refused) the reference is kept too, which keeps
	 * the display out of the DC states.
	 */
	if (dev->main_payload_present) {
		i915_dmc_shell_load(dev);
		dev->load_seq_completed_flag = dev->dmc.load_seq_completed;
		if (dev->dmc.load_seq_completed) {
			i915_dmc_shell_put_ref(dev);
		} else {
			dev->first_fault = 1;
		}
	}

	/* Releases the firmware bytes; the programs are the display's copy. */
	if (request_result == 0)
		drv_i915_firmware_release(&fw);
}
