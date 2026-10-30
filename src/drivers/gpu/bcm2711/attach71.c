/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The attachment of the BCM2712 graphics driver (the Raspberry Pi 5's V3D
 * 7.1).
 *
 * The platform calls the driver once, late in device discovery.  At boot
 * the driver honors rpi4gpu.off=1, runs discovery (stage V0), which reads
 * nothing behind the engine's power domain, and then holds the engine:
 * nothing powers it until root writes hw.gpu.start=1.  The start then runs
 * the power-up (V1), identification, MMU and interrupts (V2-V4) and the
 * self-test jobs (V5-V9) on the writer's thread, logs one line per stage
 * and returns the first error.  A board where the engine hangs the bus is
 * therefore recovered by a power cycle, and the next boot comes up with the
 * engine untouched.
 *
 * The boot parameter rpi5gpu.start=boot runs the same start at once, on the
 * boot thread before user space, so the graphical login finds the display
 * and render nodes; a failure there is logged and the boot goes on.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/atomic.h>
#include <kern/boot.h>
#include <kern/klog.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sysctl.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2712-gpu.h"
#include "drivers/gpu/bcm2711/display/hvs6.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/v3d71-private.h"

/*
 * How much of memory at the device tree's address may be read.
 *
 * The HAL checks the tree against the same bound before the kernel starts,
 * and the tree's header states its real size inside it.
 */
#define ATTACH_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/* The whole token that starts the engine and the display at boot. */
#define ATTACH_START_BOOT_TOKEN	"rpi5gpu.start=boot"

/*
 * The V3D 7.1 engine, as discovery found it.
 *
 * Filled once during attachment on the boot thread, before user space
 * runs; the start, which runs at most once (attach_starting), fills the
 * rest.  engine.present is false until discovery succeeds.
 */
static struct bcm2712_v3d attach_v3d;

/*
 * The HVS6 display path, as discovery found it.
 *
 * Filled by discovery during attachment and by the display stages of the
 * start, which runs once, like attach_v3d.  present is false until
 * discovery succeeds.
 */
static struct bcm2712_display attach_display;

/*
 * Whether the start has been claimed: zero until the first hw.gpu.start=1.
 *
 * Taken with an atomic test-and-set, so two writers cannot run the bring-up
 * at once, and never given back: the bring-up runs once per boot.
 */
static volatile unsigned attach_starting;

static uint64_t start_held(void);
static int start_engine(void);
static int start_display(void);

/*
 * The operations behind hw.gpu.start on this board, installed once by the
 * attachment and kept for the kernel's lifetime.
 */
static const struct kern_gpu_start_ops attach_start_ops = {
	start_held,
	start_engine
};

/*
 * Finds the V3D 7.1 engine and holds its bring-up for root.
 */
int
drv_bcm2712_gpu_attach(
	uint64_t fdt_phys)
{
	struct drv_fdt fdt;
	struct kern_boot_framebuffer screen;
	const void *blob;
	const char *line;
	bool off;
	int screen_error;
	int display_error;
	int start_at_boot;
	int error;

	/* Stays out entirely when the boot asked for it. */
	off = bcm2711_stage_driver_off();
	if (off) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "off by boot parameter");
		return ENODEV;
	}

	/* Finds the device tree in the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (fdt_phys == 0 || blob == NULL) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "no device tree");
		return ENODEV;
	}

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, ATTACH_FDT_READ_LIMIT);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "unreadable device tree (%d)", error);
		return ENODEV;
	}

	/* Prepares the firmware mailbox the clock questions go through; the stages report a missing one. */
	error = bcm2712_firmware_init(&fdt);
	if (error != 0)
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "firmware mailbox unavailable (%d)", error);

	/* Finds the display path; the firmware's screen is what it will show. */
	screen_error = kern_boot_framebuffer("rpi5.framebuffer", &screen);
	display_error = ENODEV;
	if (screen_error == 0)
		display_error = bcm2712_display_discover(&fdt, &attach_display, &screen);

	/* Runs discovery, which reads nothing behind the engine's power domain. */
	error = bcm2712_v3d_discover(&fdt, &attach_v3d);
	if (error != 0 && display_error != 0)
		return ENODEV;

	/* Offers the start to root. */
	kern_gpu_start_ops_set(&attach_start_ops);

	/* Starts at once when the boot asked for it; the result is logged either way. */
	line = kern_boot_handoff("boot.command-line");
	start_at_boot = kern_boot_parameters_token_present(line, ATTACH_START_BOOT_TOKEN);
	if (start_at_boot) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "%s: starting at boot", ATTACH_START_BOOT_TOKEN);
		error = start_engine();
		if (error != 0)
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "start at boot failed (%d); the boot goes on", error);
		return 0;
	}

	/* Holds the bring-up until root asks for it. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "held; root starts it with: sysctl hw.gpu.start=1");

	/* Succeeded: the engine was found and waits for its start. */
	return 0;
}

/* Reports whether the engine is still held: one until its start was claimed. */
static uint64_t
start_held(void)
{
	unsigned claimed;

	/* Nothing found is never held. */
	if (!attach_v3d.engine.present && !attach_display.present)
		return 0;

	/* A claimed start no longer holds the engine. */
	claimed = atomic_raw_load_acquire(&attach_starting);
	if (claimed != 0)
		return 0;

	/* The engine waits for its start. */
	return 1;
}

/*
 * Powers the engine and runs its self-test, once.
 *
 * Returns ENODEV when nothing is held, and otherwise the first stage's
 * error, after logging which stage it was.
 */
static int
start_engine(void)
{
	struct bcm2711_v3d *engine;
	int taken;
	int display_error;
	int error;

	/* Refuses a start when nothing was found or the start already ran. */
	if (!attach_v3d.engine.present && !attach_display.present)
		return ENODEV;
	taken = atomic_raw_try_acquire_zero(&attach_starting);
	if (!taken)
		return ENODEV;
	engine = &attach_v3d.engine;
	attach_v3d.started = true;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "start requested");

	/* Runs the display's stages first; their result does not stop the engine's. */
	display_error = start_display();

	/* Leaves the engine alone when it was not found. */
	if (!engine->present)
		return display_error;

	/* Powers the engine and wakes its sequencer (V1). */
	error = bcm2712_v3d_power_start(&attach_v3d);
	if (error != 0) {
		attach_v3d.stage_failed = "V1";
		attach_v3d.error = error;
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 not ready (%d); engine left off", error);
		return error;
	}

	/* Identifies the engine and starts its MMU and interrupts (V2-V4). */
	error = bcm2711_v3d_hardware_start(engine);
	if (error != 0) {
		attach_v3d.stage_failed = "V2-V4";
		attach_v3d.error = error;
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V2-V4 not ready (%d)", error);
		return error;
	}

	/* Runs the self-test jobs (V5-V9). */
	error = bcm2711_v3d_diagnostic(engine);
	if (error != 0) {
		attach_v3d.stage_failed = "V5-V9";
		attach_v3d.error = error;
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "self-test failed (%d)", error);
		return error;
	}

	/* Publishes the render node only after the self-test retired its storage. */
	error = bcm2711_render_register(engine);
	if (error != 0) {
		attach_v3d.stage_failed = "render";
		attach_v3d.error = error;
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "render registration failed (%d)", error);
		return error;
	}

	/* Shows the engine's state; a display failure is the start's result. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "ready: V3D %u.%u.%u, %u QPUs, %u Hz",
			   attach_v3d.version / 10U,
			   attach_v3d.version % 10U,
			   attach_v3d.revision,
			   attach_v3d.qpus,
			   attach_v3d.clock_hz);
	if (display_error != 0)
		return display_error;

	/* Succeeded: the display and the engine are up and every self-test passed. */
	return 0;
}

/*
 * Runs the display's stages: the readout (P1), the takeover (R0), the
 * vertical blank (R1), the overlay flips (R2) and the display device (R3).
 *
 * Returns 0 when there is no display to start, otherwise the first stage's
 * error after logging it.  The channel keeps the driver's own copy of the
 * firmware's list afterwards, which shows the same console.
 */
static int
start_display(void)
{
	int error;

	/* Leaves a board without the display path alone. */
	if (!attach_display.present)
		return 0;

	/* Reads what the firmware shows. */
	error = bcm2712_display_readout(&attach_display);
	if (error != 0)
		return error;

	/* Takes the firmware's list over. */
	error = bcm2712_display_adopt(&attach_display);
	if (error != 0)
		return error;

	/* Counts the vertical blanks. */
	error = bcm2712_display_vblank_test(&attach_display);
	if (error != 0)
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R1 failed (%d)", error);

	/* Moves an overlay with flips. */
	error = bcm2712_display_overlay_test(&attach_display);
	if (error != 0) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R2 failed (%d)", error);
		return error;
	}

	/* Publishes the display device on the console list. */
	error = bcm2712_display_attach_device(&attach_display);
	if (error != 0)
		return error;

	/* Succeeded: the display stages passed. */
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "display self-test ok");
	return 0;
}
