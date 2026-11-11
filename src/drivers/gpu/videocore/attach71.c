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
 * the driver honors rpi4gpu.off=1, runs discovery, which reads nothing
 * behind the engine's power domain, and then starts the display and the
 * engine on the boot thread before user space, so that the graphical login
 * finds the display and render nodes; a failure is logged and the boot
 * goes on.
 *
 * The boot parameter rpi5gpu.start=hold leaves the engine unpowered after
 * discovery until root writes hw.gpu.start=1, which then runs the same
 * start on the writer's thread.  An engine that hangs the bus is then
 * recovered by a power cycle with the next boot leaving it untouched.
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

#include "drivers/gpu/videocore/bcm2712-gpu.h"
#include "drivers/gpu/videocore/display/hvs6.h"
#if CONFIG_DRIVER_RPI5_GPU
#include "drivers/gpu/videocore/hevc-decoder.h"
#endif
#include "drivers/gpu/videocore/render-device.h"
#include "drivers/gpu/videocore/v3d71-private.h"

/*
 * How much of memory at the device tree's address may be read.
 *
 * The HAL checks the tree against the same bound before the kernel starts,
 * and the tree's header states its real size inside it.
 */
#define ATTACH_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/* The whole token that holds the engine and the display until root starts them. */
#define ATTACH_HOLD_TOKEN	"rpi5gpu.start=hold"

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
 * Finds the V3D 7.1 engine and starts it, unless the boot holds it for root.
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
	int hold;
	int error;

	/* Stays out entirely when the boot asked for it. */
	off = bcm2711_stage_driver_off();
	if (off) {
		kern_logf("v3d: off by boot parameter\n");
		return ENODEV;
	}

	/* Finds the device tree in the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (fdt_phys == 0 || blob == NULL) {
		kern_logf("v3d: no device tree\n");
		return ENODEV;
	}

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, ATTACH_FDT_READ_LIMIT);
	if (error != 0) {
		kern_logf("v3d: unreadable device tree (%d)\n", error);
		return ENODEV;
	}

	/* Prepares the firmware mailbox the clock questions go through. */
	error = bcm2712_firmware_init(&fdt);
	if (error != 0)
		kern_logf("v3d: firmware mailbox unavailable (%d)\n", error);

	/* Finds the display path; the firmware's screen is what it will show. */
	screen_error = kern_boot_framebuffer("rpi5.framebuffer", &screen);
	display_error = ENODEV;
	if (screen_error == 0)
		display_error = bcm2712_display_discover(&fdt, &attach_display, &screen);
	else
		bcm2712_screen_report(&fdt);

#if CONFIG_DRIVER_RPI5_GPU
	/* Finds the HEVC decoder, which Vulkan Video decodes with; a board without it decodes nothing. */
	error = bcm2712_hevc_decoder_discover(&fdt);
	if (error != 0)
		kern_logf("v3d: no HEVC decoder (%d)\n", error);
#endif

	/* Runs discovery, which reads nothing behind the engine's power domain. */
	error = bcm2712_v3d_discover(&fdt, &attach_v3d);
	if (error != 0 && display_error != 0)
		return ENODEV;

	/* Offers the start to root. */
	kern_gpu_start_ops_set(&attach_start_ops);

	/* Holds the bring-up until root asks for it when the boot says so. */
	line = kern_boot_handoff("boot.command-line");
	hold = kern_boot_parameters_token_present(line, ATTACH_HOLD_TOKEN);
	if (hold) {
		kern_logf("v3d: held; root starts it with: sysctl hw.gpu.start=1\n");
		return 0;
	}

	/* Starts at once; a failure is logged and the boot goes on. */
	error = start_engine();
	if (error != 0)
		kern_logf("v3d: start at boot failed (%d); the boot goes on\n", error);

	/* Succeeded: the engine was found. */
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
 * Starts the display and powers the engine, once.
 *
 * Returns ENODEV when nothing is held, and otherwise the first step's
 * error, after logging which step it was.
 */
static int
start_engine(void)
{
	struct bcm2711_v3d *engine;
	bool tfu_off;
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

	/* Starts the display first; its result does not stop the engine's. */
	display_error = start_display();

	/* Leaves the engine alone when it was not found. */
	if (!engine->present)
		return display_error;

	/* Powers the engine and wakes its sequencer. */
	error = bcm2712_v3d_power_start(&attach_v3d);
	if (error != 0) {
		kern_logf("v3d: power not ready (%d); engine left off\n", error);
		return error;
	}

	/* Identifies the engine and starts its MMU and interrupts. */
	error = bcm2711_v3d_hardware_start(engine);
	if (error != 0) {
		kern_logf("v3d: engine not ready (%d)\n", error);
		return error;
	}

	/* The texture formatting unit converts textures unless the boot kept the CPU's conversion. */
	tfu_off = bcm2711_stage_tfu_off();
	if (!tfu_off)
		engine->hardware.tfu_uif_ready = true;

	/* Publishes the render node once the engine is ready. */
	error = bcm2711_render_register(engine);
	if (error != 0) {
		kern_logf("v3d: render registration failed (%d)\n", error);
		return error;
	}

	/* A display failure is the start's result. */
	if (display_error != 0)
		return display_error;

	/* Succeeded: the display and the engine are up. */
	return 0;
}

/*
 * Starts the display: the readout, the takeover of the firmware's list, the
 * refresh measurement and the display device.
 *
 * Returns 0 when there is no display to start, otherwise the first step's
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

	/* Measures the refresh from the vertical blanks; the display device needs it. */
	error = bcm2712_display_vblank_test(&attach_display);
	if (error != 0)
		kern_logf("hvs6: no vertical blank (%d)\n", error);

	/* Publishes the display device on the console list. */
	error = bcm2712_display_attach_device(&attach_display);
	if (error != 0)
		return error;

	/* Succeeded: the display is up. */
	return 0;
}
