/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display part of the device start and stop.
 *
 * The start runs in four steps around the GT's: before the interrupts the
 * display is made, the OpRegion and its VBT are read, what the firmware
 * left is read and decided on, and the display core starts; the PCH and
 * the display's half of the interrupts are bound before the install;
 * after it the outputs are built and the firmware's leftovers corrected;
 * after the GT the display is served (the Type-C ports, the external
 * DisplayPort ports and the hotplug path start).  The stop gives back
 * what the start built, in reverse.
 *
 * It also holds the PCH detection and the query, mode and claim
 * operations of the node's display.
 */

#include "internal.h"
#include "clock.h"
#include "control.h"
#include "display.h"
#include "dmc.h"
#include "edp-device.h"
#include "xd9-firmware.h"
#include "head.h"
#include "hpd.h"
#include "interrupts.h"
#include "output.h"
#include "display-power.h"
#include "present.h"
#include "resident.h"
#include "scanout.h"
#include "vga.h"
#include "xd-core.h"
#include "xd-diag.h"
#include "xd-platform.h"
#include "xd-gmbus.h"
#include "tc-port.h"
#include "dp-ext-kern.h"
#include "bootfb.h"
#include "vbt-parse.h"
#include <kern/kcrt.h>

#ifdef I915_TEST_CAPTURE
#include "capture.h"
#endif

#include "../i915.h"
#include "../irq.h"
#include "../memory.h"
#include "../mmio.h"
#include "../pci.h"
#include "../runtime-pm.h"
#include "../trace.h"

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <drivers/gpu/gpu-scanout.h>
#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>

/* PCI configuration: the OpRegion base (ASLS) and the command word. */
#define I915_PCI_ASLS			0xfcU
#define I915_PCI_COMMAND_WORD		0x04U

/* The ports the output setup walks (A, B, TC1 to TC4). */
#define I915_DISPLAY_PORT_MASK		((1U << 0) | (1U << 1) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 6))

/* How long the DMC stop waits for its worker (500 ticks). */
#define I915_DISPLAY_DMC_FINI_TICKS	(5U * KERN_CLOCK_HZ)

/* DC_STATE_EN, read for the registration log. */
#define I915_DISPLAY_DC_STATE_EN	0x45504U

/* The one display of the node and its generation. */
#define I915_DISPLAY_ID			1U
#define I915_DISPLAY_GENERATION		1U

/*
 * The display ID of another connector of the inventory: this
 * base plus the hotplug path's connector index, the same while the device
 * lives, whatever is connected.
 */
#define I915_DISPLAY_OTHER_ID		0x100U

/* The largest frame the node's display accepts (64 MiB). */
#define I915_DISPLAY_MAX_FRAME_BYTES	(64ULL << 20)

/*
 * The mask of a PCH id, the PCH ids an emulated ISA bridge carries,
 * the Alder Lake PCH a guest of an Alder Lake GPU assumes, and the
 * vendor and subsystem of an emulated QEMU bridge.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/soc/intel_pch.h
 * (INTEL_PCH_DEVICE_ID_MASK, INTEL_PCH_P2X_DEVICE_ID_TYPE, INTEL_PCH_P3X_DEVICE_ID_TYPE,
 * INTEL_PCH_QEMU_DEVICE_ID_TYPE, INTEL_PCH_ADP_DEVICE_ID_TYPE),
 * drivers/gpu/drm/i915/soc/intel_pch.c (intel_is_virt_pch).
 */
#define I915_PCH_ID_MASK		0xff80U
#define I915_PCH_ID_EMULATED_P2X	0x7100U
#define I915_PCH_ID_EMULATED_P3X	0x7000U
#define I915_PCH_ID_EMULATED_QEMU	0x2900U
#define I915_PCH_ID_ALDER_LAKE		0x7a80U
#define I915_PCI_VENDOR_INTEL		0x8086U
#define I915_PCI_SUBVENDOR_QUMRANET	0x1af4U
#define I915_PCI_SUBDEVICE_QEMU		0x1100U

/* PCI class 06h/01h (bridge / ISA), matched on class and subclass. */
#define I915_PCI_CLASS_BRIDGE_ISA	0x060100U
#define I915_PCI_CLASS_BRIDGE_ISA_MASK	0xffff00U

static int i915_display_create(struct i915_device *device);
static int i915_display_gen9_check(struct i915_device *device);
static void i915_display_read_tables(struct i915_device *device);
static int i915_display_create_worlds(struct i915_display *display);
static void i915_display_destroy_worlds(struct i915_display *display);
static struct i915_display *i915_display_live(struct i915_device *device);
static int i915_display_prepare_state(struct i915_device *device);
static void i915_display_check_firmware(struct i915_device *device);
static int i915_display_core_init(struct i915_device *device);
static void i915_display_dmc_init(struct i915_device *device);
static int i915_display_bandwidth_start(struct i915_device *device);
static int i915_display_memory_and_clocks(struct i915_device *device);
static void i915_display_setup_outputs(struct i915_device *device);
static void i915_display_sanitize(struct i915_device *device);
static void i915_display_resume_front(struct i915_display *display);
static void i915_display_arm(struct i915_display *display, int interrupts_enabled);
static int i915_display_enable_ipc(struct i915_mmio *mmio);
static uint32_t i915_display_rmw(struct i915_mmio *m, uint32_t reg, uint32_t clear, uint32_t set);
static void i915_display_publish(struct i915_device *device);
static void i915_display_unpublish(struct i915_device *device);
static int i915_display_pch_emulated(uint16_t id, uint16_t svid, uint16_t sdid);
static void i915_display_pch_guess(int is_alderlake, uint16_t *pch_id, int *pch_type);
static int i915_next_isa_bridge(struct i915_display *display, unsigned index, uint16_t *vendor, uint16_t *device, uint16_t *svid, uint16_t *sdid);
static int i915_display_query(void *device, void *session, struct gpu_display_info *request);
static int i915_display_mode(void *device, void *session, struct gpu_display_mode *request);
static int i915_display_claim(void *device, void *session, struct gpu_display_claim *request);
static int i915_display_resident_connector(struct i915_display *display);
static unsigned i915_display_count(struct i915_display *display, int resident);
static int i915_display_other(struct i915_display *display, int resident, uint32_t index, unsigned *connector);
static void i915_display_key(struct i915_device *device, unsigned kind, int port, char *name, unsigned size);
static int i915_display_other_query(struct i915_device *device, unsigned connector, struct gpu_display_info *request);
static int i915_display_other_mode(struct i915_display *display, struct gpu_display_mode *request);
static int i915_display_power(void *device, void *session, const struct gpu_display_power *request);
static int i915_display_refresh(void *device, void *session, struct gpu_display_refresh *request);
static int i915_display_other_check(struct i915_display *display, uint32_t display_id, uint64_t generation, int *connected);
static int i915_display_move(struct i915_device *device, unsigned connector);
static int i915_display_outputs_differ(const struct i915_display_output *one, const struct i915_display_output *other);

/*
 * The display operations of the resident node.
 *
 * The resident output -- the firmware's output (the GPU scanout rule) --
 * is display 0, with one full-output plane; the hotplug path's other
 * connectors follow it in the inventory, connected or not, and are
 * described but not lit (claiming one is refused with ENOSPC, the limit
 * of outputs shown at once).  Query,
 * mode and claim are answered here, present, wait and release by the
 * present path, and the topology sequence by the hotplug path, which
 * moves it when a connector is connected or disconnected.
 * Power and refresh check the display here and are done by
 * the display control (control.c).  The cursor plane is not used yet
 * so the cursor operations are absent.  Presents
 * wait under the open's admission (no DRV_GPU_DISPLAY_UNGATED).
 * The table never changes.
 */
static const struct drv_gpu_display_ops i915_display_ops = {
	i915_display_query,
	i915_display_mode,
	i915_display_claim,
	drv_i915_present_display_release,
	drv_i915_present_display_present,
	drv_i915_present_display_wait,
	drv_i915_display_events,
	i915_display_power,
	i915_display_refresh,
	NULL,
	NULL,
	NULL,
	NULL,
	0,
	NULL,
	NULL
};

/*
 * Starts the display of a device, before the interrupts.
 *
 * Makes the display, checks what the firmware of a display 9 controller
 * scans out, reads the OpRegion with its VBT and the memory's bandwidth,
 * then reads what the firmware left and decides whether the display is
 * used.  A display that is used gets its core started and its DMC's load
 * queued.  A display the firmware check leaves unused is left as the
 * firmware left it and the start goes on.  Returns 0, or an error with
 * device->stage naming the step that failed.
 */
int
drv_i915_display_start(
	struct i915_device *device)
{
	struct i915_display *display;
	int error;

	/* Makes the display; nothing stays allocated when this fails. */
	device->stage = "display_create";
	error = i915_display_create(device);
	if (error != 0)
		return error;

	/* Checks what a display 9 controller scans out before the GT start writes its GGTT pages. */
	error = i915_display_gen9_check(device);
	if (error != 0)
		return error;

	/* A display the capture build or the display 9 check leaves unused goes no further. */
	display = i915_display_live(device);
	if (display == NULL)
		return 0;

	/* Reads the OpRegion and its VBT, the DRAM and the memory's bandwidth. */
	i915_display_read_tables(device);

	/* Prepares the VBT, the VGA client and the power wells' map. */
	error = i915_display_prepare_state(device);
	if (error != 0)
		return error;

	/* Reads what the firmware left; a display that is not used is left alone. */
	i915_display_check_firmware(device);
	if (display->absent)
		return 0;

	/* Starts the display core. */
	error = i915_display_core_init(device);
	if (error != 0)
		return error;

	/* Queues the DMC's load without waiting for it. */
	i915_display_dmc_init(device);

	/* Reads the memory's bandwidth points and keeps SAGV off where the display controls it. */
	error = i915_display_bandwidth_start(device);
	if (error != 0)
		return error;

	/* Succeeded: the display core runs and the interrupts may be installed. */
	return 0;
}

/*
 * Frees the display of a device.
 *
 * A display whose run kept resources, or whose interrupt drain failed, is
 * kept: the display or the handler may still use it.
 */
void
drv_i915_display_destroy(
	struct i915_device *device)
{
	struct i915_display *display;
	int kept;

	/* A device without a display has nothing to free. */
	display = device->display;
	if (display == NULL)
		return;

	/* A retained display is never freed. */
	kept = drv_i915_display_abandoned(device);
	if (!kept)
		kept = drv_i915_display_irq_sync_failed(device);
	if (kept) {
		kern_logf("i915: stop: the display state is kept (its resources are retained)\n");
		return;
	}

	/* Frees the parts' state, then the display. */
	i915_display_destroy_worlds(display);
	device->display = NULL;
	kern_free(display);
}

/*
 * Detects the PCH and binds the display interrupt half in place of the
 * interim hooks.
 *
 * The interrupt install resets and programs the south display block only
 * for a south display that has one, so the PCH is needed before it.
 */
void
drv_i915_display_irq_prepare(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_pch_state *pch;
	uint16_t pch_id;

	display = i915_display_live(device);
	if (display == NULL)
		return;

	pch = &display->pch;

	/* Finds the PCH: display version, Alder Lake-P, a display, running as a guest. */
	kern_memset(pch, 0, sizeof(*pch));
	drv_i915_detect_pch(display, pch, (int)device->gt.display_ver, 1, 1, 1);
	kern_logf("i915: display: PCH type=%d id=0x%04x source=%d bridges=%u bridge_dev=0x%04x subsys=%04x:%04x\n",
	    pch->type,
	    (unsigned)pch->id,
	    pch->source,
	    pch->bridges_scanned,
	    (unsigned)pch->bridge_device,
	    (unsigned)pch->bridge_svid,
	    (unsigned)pch->bridge_sdid);

	/* The xd description's south display: a PCH a bridge named, or none (a guessed one is no PCH read). */
	pch_id = 0U;
	if (pch->source == I915_PCH_SRC_REAL)
		pch_id = pch->id;
	drv_i915_xd_south(&display->xd, device->product, device->revision, pch_id);

	/* Binds the vblank delivery, the power-well hooks and the display operations. */
	drv_i915_display_irq_bind(display, &device->gt.irq);
	display->irq_bound = 1;
}

/*
 * Logs the display interrupt masks the install computed.
 */
void
drv_i915_display_irq_log(
	struct i915_device *device)
{
	struct i915_display *display;

	display = i915_display_live(device);
	if (display == NULL)
		return;

	/* Logs the interrupt masks the install computed. */
	kern_logf("i915: display: interrupt masks: pipe_masked=0x%x pipe_enables=0x%x port_masked=0x%x misc_masked=0x%x pipe_a_mask=0x%x master_enabled=%d\n",
	    display->irq.de_pipe_masked,
	    display->irq.de_pipe_enables,
	    display->irq.de_port_masked,
	    display->irq.de_misc_masked,
	    display->irq.de_irq_mask[0],
	    device->gt.irq.reached_master_enable);
}

/*
 * Builds the display's outputs, after the interrupts.
 *
 * The firmware's pipes, ports, PLLs and wells are logged first (no state
 * changes).  Then the memory's latencies, the GMBUS reset, the CD clock,
 * the workarounds and the VGA plane; the ports' table and the built-in
 * panel's connector; and the correction of what the firmware left on but
 * unused.  Returns 0, or an error with device->stage naming the step
 * that failed.
 */
int
drv_i915_display_outputs(
	struct i915_device *device)
{
	const struct i915_xd_platform *platform;
	struct i915_display *display;
	int error;

	display = i915_display_live(device);
	if (display == NULL)
		return 0;

	/* Logs what the firmware left on, changing nothing; a display without a description is not read. */
	platform = NULL;
	if (display->xd.chosen)
		platform = display->xd.choice.platform;
	drv_i915_xd_diag_firmware_left(&display->xd.io, platform);

	/* The memory's latencies, the GMBUS reset, the CD clock, the workarounds, the VGA plane. */
	error = i915_display_memory_and_clocks(device);
	if (error != 0)
		return error;

	/* The ports, then the correction of what the firmware left unused. */
	i915_display_setup_outputs(device);
	i915_display_sanitize(device);

	/* Succeeded: the outputs are known and the firmware's leftovers corrected. */
	return 0;
}

/*
 * Starts serving the display, after the GT.
 *
 * Arms the hotplug pins and starts the Type-C ports, the external
 * DisplayPort ports and the hotplug path.  A pipe the firmware left lit is
 * the firmware's display: the resident start takes it over when the
 * firmware check decided so; otherwise the start stops, because the
 * display would be left lit and unaccounted for.  Then the start's power
 * reference goes back (or stays for the takeover) and the probe's runtime
 * reference is returned.  Returns 0, or ENOTSUP with device->stage naming
 * the lit firmware display.
 */
int
drv_i915_display_serve(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_driver_probe *dprobe;
	const char *hotplug;
	int started;

	display = i915_display_live(device);
	if (display == NULL)
		return 0;

	dprobe = &display->dprobe;

	/* The DMC's program decides whether the release of the DC off well enters DC6 below. */
	display->power.dmc_loaded = 0;
	if (display->dmc_inited)
		display->power.dmc_loaded = drv_i915_dmc_has_payload(&display->dmc_dev.dmc);

	/* Counts the pipes the firmware left lit, arms the hotplug pins and enables IPC. */
	i915_display_arm(display, device->gt.irq.irqs_enabled);
	kern_logf("i915: display: serve: firmware pipes lit=%u ipc=%d\n",
	    dprobe->active_crtcs,
	    dprobe->ipc_enabled);

	/* The Type-C ports first: the hotplug path asks them whether something is plugged in. */
	drv_i915_tc_port_start(display);

	/* Then the external DP ports on them, whose sinks the DP connectors' detection probes. */
	drv_i915_dp_ext_start(display);

	/* Then the hotplug path, with the connectors of the ports the output setup made. */
	started = drv_i915_hpd_start(display);
	if (started == 0)
		display->hpd_started = 1;

	hotplug = "NOT started";
	if (display->hpd_started)
		hotplug = "started";
	kern_logf("i915: hotplug path: %s\n", hotplug);

	/* A lit firmware display is left to the resident start's takeover. */
	if (dprobe->active_crtcs != 0U && display->xd.firmware.take_over)
		kern_logf("i915: the firmware display (%u lit pipe) is left to the resident start's takeover\n", dprobe->active_crtcs);

	/* A lit firmware display without the takeover would be left unaccounted for: the start stops. */
	if (dprobe->active_crtcs != 0U && !display->xd.firmware.take_over) {
		drv_i915_trace_record(&device->gt.trace, 0U, I915_TRACE_UNIMPLEMENTED, "firmware_display_lit", dprobe->active_crtcs, 0U);
		device->stage = "firmware_display_lit";
		return ENOTSUP;
	}

	/* Gives the start's power reference and the probe's runtime reference back. */
	i915_display_publish(device);
	kern_logf("i915: display: serving: wells_on %u -> %u dc_state=0x%x verify_mismatches=%u rpm usage=%d active=%d\n",
	    dprobe->wells_on_before,
	    dprobe->wells_on_after,
	    (unsigned)dprobe->dc_state_after,
	    dprobe->verify_mismatches,
	    dprobe->rpm_usage_after,
	    drv_i915_rpm_active(&device->gt.probe_pm));

	/* The wells that are held or on, and the DC state. */
	drv_i915_display_power_log(&display->power);

	/* Succeeded: the display is served. */
	return 0;
}

/*
 * Fills the panel dependencies of the resident run from the objects the
 * normal initialisation built; none is copied.  The node has a panel from
 * here on.
 */
void
drv_i915_display_resident_deps(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_lcd_kernel_deps *rlcd;
	struct i915_resident_ctx *rctx;
	struct i915_gt *gt;

	display = i915_display_live(device);
	if (display == NULL)
		return;

	gt = &device->gt;
	rlcd = &display->rlcd;
	rctx = &display->rctx;

	/* What the panel runs read and use: the panel, the display state, the interrupts and the GT. */
	rlcd->edp = &display->edp_dev;
	rlcd->mmio = &gt->mmio;
	rlcd->power = &display->power;
	rlcd->cdclk = &display->cdclk;
	rlcd->bandwidth = &display->xd.bandwidth;
	rlcd->platform = display->xd.choice.platform;
	rlcd->dmc = &display->dmc_dev;
	rlcd->irq = &display->irq;
	rlcd->gm = &gt->mem;
	rlcd->ipc_enabled = display->dprobe.ipc_enabled;
	rlcd->es = &gt->engines;
	rlcd->vm = &gt->ppgtt;
	rlcd->uncore_lock = &gt->uncore_lock;
	rlcd->gmadr_base = gt->gmadr_base;
	rlcd->gmadr_size = gt->gmadr_size;
	rlcd->dprobe = &display->dprobe;
	rlcd->tc = drv_i915_tc_port_ports(display);

	/* What the serving thread needs, with the panel. */
	rctx->device = device;
	rctx->mmio = &gt->mmio;
	rctx->uncore_lock = &gt->uncore_lock;
	rctx->gm = &gt->mem;
	rctx->es = &gt->engines;
	rctx->lcd = rlcd;

	/* The output the node drives: the panel, or HDMI when display=hdmi finds a sink. */
	if (!display->absent)
		drv_i915_display_output_select(display);
}

/*
 * Stops serving the display, before the GT stop.
 *
 * Nothing may still be scanned out: the driver's own stop path darkens
 * the outputs, and this is the last resort (a pipe left running keeps
 * reading memory after the driver is gone).  Then the hotplug path (its
 * interrupt entry closed, its works cancelled), the Type-C ports, and the
 * start's power and runtime references taken back.
 */
void
drv_i915_display_stop_serving(
	struct i915_device *device)
{
	struct i915_display *display;
	unsigned forced;
	unsigned wells_on;

	display = device->display;
	if (display == NULL)
		return;

	/* Stops anything still scanned out. */
	forced = drv_i915_xd_diag_stop_scanout(&display->xd.io);
	if (forced != 0U)
		kern_logf("i915: stop: stopped %u display element(s) the driver had not released\n", forced);

	/* The hotplug path first: its interrupt entry closed, the works cancelled. */
	if (display->hpd_started) {
		drv_i915_hpd_stop(display);
		display->hpd_started = 0;
	}

	/* The Type-C ports' PHYs and TC cold blocks go back to the Type-C subsystem. */
	drv_i915_tc_port_stop(display);

	/* Takes the runtime reference and the start's power reference back (DC states off, wells on). */
	if (display->dprobe.registered) {
		i915_display_unpublish(device);
		wells_on = drv_i915_display_power_wells_on(&display->power);
		kern_logf("i915: stop: serving ended (rpm usage=%d, start reference taken again: wells_on=%u DC_STATE_EN=0x%08x)\n",
		    drv_i915_rpm_usage(&device->gt.probe_pm),
		    wells_on,
		    drv_i915_read32(&device->gt.mmio, I915_DISPLAY_DC_STATE_EN));
	}
}

/*
 * Begins the display's suspend.  The outputs were darkened when the
 * request worker left the display window for its park.  The hotplug is
 * quiesced (no interrupt is taken and no detection runs from here on),
 * the Type-C ports give their connections back, and the start's reference
 * is taken again, which keeps every pipe's wells on and the display out of DC
 * states while the suspend goes on.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/i915_driver.c (i915_drm_suspend),
 * drivers/gpu/drm/i915/display/intel_display_driver.c
 * (intel_display_driver_suspend).
 */
void
drv_i915_display_suspend_begin(
	struct i915_device *device)
{
	struct i915_display *display;

	/* A device without a display has nothing to suspend. */
	display = device->display;
	if (display == NULL || !display->power_started)
		return;

	/* Stops the hotplug before anything it reads goes down. */
	drv_i915_hpd_suspend(display);

	/* Gives the Type-C ports' PHYs and TC cold blocks back. */
	drv_i915_tc_port_suspend(display);

	/* Holds the wells for the rest of the suspend. */
	drv_i915_display_power_hold(&display->power);
}

/*
 * Ends the display's suspend with the interrupts off: the display core
 * goes down and the display enters DC9.
 * A DC9 that is refused brings the core up again and is reported.
 */
int
drv_i915_display_suspend_end(
	struct i915_device *device)
{
	struct i915_display *display;
	int error;

	/* A device without a display has nothing to suspend. */
	display = device->display;
	if (display == NULL || !display->power_started)
		return 0;

	/* Takes the display core down. */
	drv_i915_display_power_core_stop(&display->power);

	/* Enters DC9; a refusal brings the core up again. */
	error = drv_i915_display_power_dc9_enter(&display->power, 0);
	if (error != 0) {
		(void)drv_i915_display_resume_begin(device);
		drv_i915_display_resume_end(device);
		return error;
	}

	/* suspend_dc9 tells the resume to leave DC9. */
	display->suspend_dc9 = 1;

	/* Succeeded: the display is in DC9. */
	return 0;
}

/*
 * Begins the display's resume, before the interrupts come back: DC9 is
 * left, the display core comes up again with the start's reference held,
 * the DMC's program is loaded
 * again, and what the start set beside the core is set again (the VGA
 * plane off, the display's workarounds, the GMBUS controller reset).  It
 * reports the first step that failed; the later ones still run.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/i915_driver.c (i915_drm_resume_early,
 * i915_drm_resume), drivers/gpu/drm/i915/display/intel_vga.c
 * (intel_vga_redisable), drivers/gpu/drm/i915/i915_suspend.c
 * (i915_restore_display).
 */
int
drv_i915_display_resume_begin(
	struct i915_device *device)
{
	struct i915_display *display;
	int first_error;
	int error;

	/* A device without a display has nothing to resume. */
	display = device->display;
	if (display == NULL || !display->power_started)
		return 0;

	/* Leaves DC9. */
	first_error = 0;
	if (display->suspend_dc9) {
		error = drv_i915_display_power_dc9_leave(&display->power);
		if (error != 0)
			first_error = error;

		/* DC9 is behind the display either way. */
		display->suspend_dc9 = 0;
	}

	/* Brings the display core up again; the start's reference is held from here. */
	if (display->power.core_suspended) {
		(void)drv_i915_display_power_core_start(&display->power, display->xd.bandwidth.dram_type, display->xd.bandwidth.channels);
		display->power.core_suspended = 0;
	}

	/* Loads the DMC's program again; a display without one has no DC states, as at boot. */
	if (display->dmc_inited) {
		error = drv_i915_dmc_resume(&display->dmc_dev);
		if (error != 0 && error != ENOENT && first_error == 0)
			first_error = error;
	}

	/* Restores what the start set beside the display core, once the start had set it. */
	if (display->front_done)
		i915_display_resume_front(display);

	/* Reports the first step that failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: the display core runs. */
	return 0;
}

/*
 * Ends the display's resume, with the interrupts back: gives the start's
 * reference back, so that DC6 is allowed
 * again, reads the Type-C ports again, and brings the hotplug back with
 * a look at every connector (one plugged or unplugged during the sleep
 * is seen).  The outputs come back with the next presentation, which
 * enters the display window again and lights them with every register
 * written (the panel's power sequencer delays and the backlight's PWM
 * among them).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_driver.c
 * (intel_display_driver_resume).
 */
void
drv_i915_display_resume_end(
	struct i915_device *device)
{
	struct i915_display *display;

	/* A device without a display has nothing to resume. */
	display = device->display;
	if (display == NULL || !display->power_started)
		return;

	/* Gives the start's reference back. */
	drv_i915_display_power_serve(&display->power, &display->dprobe);

	/* Reads the Type-C ports again, before the hotplug asks them. */
	drv_i915_tc_port_resume(display);

	/* Arms the hotplug again and looks at every connector. */
	drv_i915_hpd_resume(display);

	/* The next entry into the window is the first after the sleep. */
	display->window.after_resume = 1;
}

/*
 * Releases the panel connector and the display records (after the GT
 * stop, before the interrupt uninstall).
 *
 * The panel's device lets VDD go and gives the deferred power references
 * back before the records go.
 */
void
drv_i915_display_stop_outputs(
	struct i915_device *device)
{
	struct i915_display *display;

	display = device->display;
	if (display == NULL)
		return;

	/* The output setup never ran. */
	if (!display->front_done)
		return;

	drv_i915_edp_device_fini(&display->edp_dev);
	display->front_done = 0;
}

/*
 * Takes the rest of the display apart in reverse order (after the
 * interrupt uninstall).
 *
 * The DMC (its worker synced, not cancelled, its reference processed, its
 * payload freed) and its workqueue, the display core, the display's
 * power, the VBT parser and the VGA client.
 */
void
drv_i915_display_stop(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_dmc_dev *dmc;

	display = device->display;
	if (display == NULL)
		return;

	dmc = &display->dmc_dev;

	/* The DMC and its workqueue. */
	if (display->dmc_inited) {
		drv_i915_dmc_fini(dmc, sched_ticks() + I915_DISPLAY_DMC_FINI_TICKS);
		kern_logf("i915: dmc: stop (worker synced; load_seq_completed=%d, payload_writes=%u, dmc_ref_held=%d)\n",
		    dmc->load_seq_completed_flag,
		    dmc->dmc.payload_writes,
		    dmc->dmc_wakeref_held);
		kern_logf("i915: dmc: ver=%u.%u ids MAIN/A/B/C/D present=%d%d%d%d%d payload=%u/%u/%u/%u/%u\n",
		    (unsigned)(dmc->dmc.version >> 16),
		    (unsigned)(dmc->dmc.version & 0xffffU),
		    dmc->dmc.dmc_info[0].present,
		    dmc->dmc.dmc_info[1].present,
		    dmc->dmc.dmc_info[2].present,
		    dmc->dmc.dmc_info[3].present,
		    dmc->dmc.dmc_info[4].present,
		    dmc->dmc.dmc_info[0].dmc_fw_size,
		    dmc->dmc.dmc_info[1].dmc_fw_size,
		    dmc->dmc.dmc_info[2].dmc_fw_size,
		    dmc->dmc.dmc_info[3].dmc_fw_size,
		    dmc->dmc.dmc_info[4].dmc_fw_size);
		drv_i915_workqueue_destroy(&display->dmc_wq);
		display->dmc_inited = 0;
	}

	/* The display core: the start's reference is left with the wells, which stay on. */
	if (display->display_core_inited) {
		display->power.start_held = 0;
		display->display_core_inited = 0;
		kern_logf("i915: stop: display core released (wells kept)\n");
	}

	/* The display's power. */
	if (display->power_started) {
		drv_i915_display_power_stop(&display->power);
		display->power_started = 0;
		kern_logf("i915: stop: display power released\n");
	}

	/* The VBT parser's lists and arena. */
	drv_i915_bios_driver_remove(&display->vbt_state);

	/* The VGA arbiter client. */
	if (display->vga_registered) {
		drv_i915_vga_unregister(&display->vga_client);
		display->vga_registered = 0;
		kern_logf("i915: stop: VGA arbiter client unregistered\n");
	}

	/* The interrupt device no longer calls the display half. */
	if (display->irq_bound) {
		drv_i915_display_irq_unbind(display, &device->gt.irq);
		display->irq_bound = 0;
	}

#ifdef I915_TEST_CAPTURE
	/* The capture area, once no address space maps it. */
	drv_i915_capture_fini(device);
#endif

	/* The boot framebuffer display's staging area and view, once no address space maps them. */
	drv_i915_bootfb_fini(device);
}

/*
 * Reports whether a run kept the display's resources because its stop was
 * not confirmed: the DMA device, the scratch page, the BARs and bus
 * mastering stay.
 */
int
drv_i915_display_abandoned(
	struct i915_device *device)
{
	int abandoned;

	/* A device without a display kept nothing. */
	if (device->display == NULL)
		return 0;

	/* Asks the panel runs. */
	abandoned = drv_i915_resident_abandoned(device->display);

	/* Reports whether resources are retained. */
	return abandoned;
}

/*
 * Reports whether a run kept GPU objects the GPU was not shown to be done
 * with: the GT's engines and objects stay.
 */
int
drv_i915_display_gpu_retained(
	struct i915_device *device)
{
	int retained;

	/* A device without a display kept nothing. */
	if (device->display == NULL)
		return 0;

	/* Asks the panel runs. */
	retained = drv_i915_resident_gpu_retained(device->display);

	/* Reports whether GPU objects are retained. */
	return retained;
}

/*
 * Reports whether a pipe's interrupt drain failed: from then on the
 * handler may still touch display registers, so it stays attached and the
 * device resources stay in place.
 */
int
drv_i915_display_irq_sync_failed(
	struct i915_device *device)
{
	/* A device without a display has no pipes to drain. */
	if (device->display == NULL)
		return 0;

	/* Reports the display power's flag. */
	return device->display->power.irq_sync_failed;
}

/*
 * Binds the display and scanout operations of the node and their
 * capabilities, when the node has a panel.
 */
void
drv_i915_display_bind_ops(
	struct i915_device *device,
	struct drv_gpu_ops *ops)
{
	struct i915_display *display;

#ifdef I915_TEST_CAPTURE
	/* The capture build binds the capture display in place of the panel. */
	drv_i915_capture_bind_ops(device, ops);
	return;
#endif

	/* A Gen9 node, whose display is left to the firmware, offers the firmware's framebuffer; another without a panel offers none. */
	display = i915_display_live(device);
	if (display == NULL) {
		drv_i915_bootfb_bind_ops(device, ops);
		return;
	}

	/* A display without a panel run binds no operations. */
	if (display->rctx.lcd == NULL)
		return;

	/* The display and the display-only pairing, with their capabilities. */
	ops->display = &i915_display_ops;
	ops->scanout = &drv_i915_scanout_ops;
	ops->capabilities |= GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS | GPU_CAP_DISPLAY_CONTROL;
}

/*
 * Drops the display lease a closing session still holds; the panel is
 * stopped first.
 */
void
drv_i915_display_session_close(
	struct i915_device *device,
	void *session)
{
	int bound;

#ifdef I915_TEST_CAPTURE
	/* The capture display owns the lease in the capture build. */
	drv_i915_capture_session_close(device, session);
	return;
#endif

	/* The boot framebuffer display owns the lease on a Gen9 node. */
	bound = drv_i915_bootfb_bound(device);
	if (bound) {
		drv_i915_bootfb_session_close(device, session);
		return;
	}

	/* The present path owns the lease. */
	drv_i915_present_lease_close(device, session);
}

/*
 * Reads the mode of the node's output: the panel's (the resident eDP's
 * LCD state), or the HDMI mode display=hdmi chose.  0, or
 * ENXIO when the node has no output.
 */
int
drv_i915_display_panel(
	struct i915_display *display,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_millihz)
{
	int error;

	/* The mode of the output the node drives. */
	error = drv_i915_display_output_mode(display, width, height, refresh_millihz);
	if (error != 0)
		return ENXIO;

	/* Succeeded: the mode is known. */
	return 0;
}

/*
 * Finds the PCH the display pairs with, from the ISA bridges.
 *
 * The first Intel bridge whose id names a known PCH decides.  An emulated
 * bridge, or no bridge at all in a guest, leaves the PCH to a guess from
 * the GPU's platform.  A GPU without a display has no south display to
 * talk to (I915_PCH_NOP).  display_ver is kept for the callers' shape and
 * not read.
 */
void
drv_i915_detect_pch(
	struct i915_display *display,
	struct i915_pch_state *p,
	int display_ver,
	int is_alderlake,
	int has_display,
	int run_as_guest)
{
	uint16_t vendor;
	uint16_t device;
	uint16_t svid;
	uint16_t sdid;
	unsigned index;
	int kind;
	int found_bridge;
	int more;
	int emulated;

	UNUSED_PARAMETER(display_ver);

	/* Starts with no PCH. */
	p->type = I915_PCH_NONE;
	p->id = 0U;
	p->source = I915_PCH_SRC_NONE;
	p->bridges_scanned = 0U;
	p->bridge_device = 0U;
	p->bridge_svid = 0U;
	p->bridge_sdid = 0U;

	/* Walks the ISA bridges until one names the PCH. */
	found_bridge = 0;
	vendor = 0U;
	device = 0U;
	svid = 0U;
	sdid = 0U;
	index = 0U;
	more = 1;
	while (more) {
		/* The next bridge, or the end of the walk. */
		more = i915_next_isa_bridge(display, index, &vendor, &device, &svid, &sdid);
		if (!more)
			break;

		/* Counts the bridge and moves to the next. */
		index++;
		p->bridges_scanned++;
		found_bridge = 1;

		/* Only an Intel bridge names a PCH. */
		if (vendor != I915_PCI_VENDOR_INTEL)
			continue;

		/* A known PCH id decides. */
		kind = drv_i915_xd_pch_kind(device);
		if (kind != I915_PCH_NONE) {
			p->type = kind;
			p->id = (uint16_t)(device & I915_PCH_ID_MASK);
			p->source = I915_PCH_SRC_REAL;
			p->bridge_device = device;
			p->bridge_svid = svid;
			p->bridge_sdid = sdid;
			break;
		}

		/* An emulated bridge leaves it to the guess. */
		emulated = i915_display_pch_emulated((uint16_t)(device & I915_PCH_ID_MASK), svid, sdid);
		if (emulated) {
			i915_display_pch_guess(is_alderlake, &p->id, &p->type);
			p->source = I915_PCH_SRC_VIRT;
			p->bridge_device = device;
			p->bridge_svid = svid;
			p->bridge_sdid = sdid;
			break;
		}
	}

	/* A GPU without a display talks to no south display; a guest without a bridge guesses. */
	if (found_bridge && !has_display) {
		kern_logf("i915: display: no display: the PCH's south display is not used\n");
		p->type = I915_PCH_NOP;
		p->id = 0U;
	} else if (!found_bridge) {
		if (run_as_guest && has_display) {
			i915_display_pch_guess(is_alderlake, &p->id, &p->type);
			p->source = I915_PCH_SRC_NO_BRIDGE;
		} else {
			kern_logf("i915: display: no ISA bridge: no PCH\n");
		}
	}
}

/*
 * Makes the display of a device: its xd state over the device's registers
 * and description, and the state of the parts it still keeps apart.
 * Returns 0, ENOMEM, or EBUSY when the device has a display already.
 * Nothing stays allocated on failure.
 */
static int
i915_display_create(
	struct i915_device *device)
{
	struct i915_display *display;
	int error;

	/* A device has one display. */
	if (device->display != NULL)
		return EBUSY;

	/* Allocates the zeroed display. */
	display = kern_calloc(1U, sizeof(*display));
	if (display == NULL)
		return ENOMEM;

	/* The display belongs to its device. */
	display->device = device;

	/* The xd display: its register window and the description of the device. */
	drv_i915_xd_start(&display->xd, &device->gt.mmio, &device->gt.sb_lock, device->product, device->revision);

	/* The resident output's refresh boundaries start with nothing counted. */
	drv_i915_display_refresh_init(display);

	/* Allocates the state of the VBT parser and of the hotplug. */
	error = i915_display_create_worlds(display);
	if (error != 0) {
		i915_display_destroy_worlds(display);
		kern_free(display);
		return error;
	}

	/* Publishes the display in the device. */
	device->display = display;

#ifdef I915_TEST_CAPTURE
	/*
	 * The capture build never touches the display hardware: every display
	 * step is skipped, as after a firmware display check that stopped, and
	 * the node offers the capture display (capture.c) instead of the panel.
	 */
	display->absent = 1;
	kern_logf("i915: capture: capture build: the display hardware is not used (no mode set, no panel); presentations are captured into guest RAM\n");
#endif

	/* Succeeded: the display's start may go on. */
	return 0;
}

/*
 * Checks what the firmware of a display 9 controller scans out.
 *
 * Such a controller is not driven: the firmware's picture is kept and the
 * later display steps are skipped.  The check refuses a start whose GT
 * memory would overwrite the firmware's scanout.  Returns 0 for any other
 * device, or the check's error.
 */
static int
i915_display_gen9_check(
	struct i915_device *device)
{
	struct i915_gt *gt;
	unsigned reserved;
	unsigned driver_ggtt_first;
	int error;

	/* Resolves the GT whose GGTT the check reads. */
	gt = &device->gt;

	/* Only a display 9 controller is checked here. */
	if (gt->graphics_ver != 9U || device->display == NULL)
		return 0;

	/* The GGTT pages the driver writes: the top window and the display window. */
	reserved = I915_GT_GGTT_PAGES + I915_GT_DISPLAY_PAGES;
	driver_ggtt_first = 0U;
	if (gt->ggtt_entries > reserved)
		driver_ggtt_first = gt->ggtt_entries - reserved;

	/* Checks what the firmware scans out before the GT start writes its GGTT pages. */
	device->stage = "xd9_firmware_check";
	error = drv_i915_xd9_firmware_check(device->display, &gt->mmio, driver_ggtt_first);
	if (error != 0)
		return error;

	/* Succeeded: the firmware's scanout is outside the driver's pages. */
	return 0;
}

/*
 * Reads the OpRegion and its VBT, then the DRAM and the memory's
 * bandwidth.
 *
 * ASLS holds the OpRegion's base.  The OpRegion is read as data (a
 * read-only copy and its VBT); the OpRegion's runtime protocol is not
 * joined.  A DRAM or bandwidth read that fails is logged and the start
 * goes on.
 */
static void
i915_display_read_tables(
	struct i915_device *device)
{
	const struct i915_xd_platform *platform;
	struct i915_display *display;
	struct i915_gt *gt;
	uint32_t asls;
	int bw_error;
	int error;
	uint16_t op;

	/* Resolves the display, the GT and the platform's description. */
	display = device->display;
	gt = &device->gt;
	platform = NULL;
	if (display->xd.chosen)
		platform = display->xd.choice.platform;

	/* ASLS (PCI configuration 0xFC) holds the OpRegion base. */
	asls = drv_i915_pci_read32(&gt->pci, I915_PCI_ASLS);
	kern_logf("i915: opregion: ASLS=0x%08x\n", asls);

	/* Reads the OpRegion and the VBT it keeps into the display's copies. */
	error = drv_i915_xd_read_opregion(&display->xd,
					  asls,
					  display->i915_opregion_read_data_opcopy,
					  display->i915_opregion_read_data_vbtcopy,
					  sizeof(display->i915_opregion_read_data_vbtcopy));

	/* A VBT that checks is the one the parser reads. */
	if (error == 0 && display->xd.opregion.vbt_valid) {
		drv_i915_bios_set_opregion_vbt(display, display->i915_opregion_read_data_vbtcopy, display->xd.opregion.vbt_size);
		display->opregion_vbt_present = 1;
	}

	/* ASLS 0 is a machine without an OpRegion. */
	if (asls == 0U)
		drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_NOTE, "opregion_absent", 0U, 0U);

	/* The memory and the display's share of its bandwidth; a memory the firmware does not describe leaves no limits. */
	bw_error = drv_i915_xd_bw_read(&display->xd.io, platform, &display->xd.bandwidth);
	op = I915_TRACE_NOTE;
	if (bw_error == 0)
		op = I915_TRACE_ACQUIRE;
	drv_i915_trace_record(&gt->trace, 0U, op, "bandwidth_read", (uint64_t)(unsigned)display->xd.bandwidth.sagv, (uint64_t)display->xd.bandwidth.dram_type);
}

/* Allocates the state of the VBT parser and of the hotplug, stopping at the first failure. */
static int
i915_display_create_worlds(
	struct i915_display *display)
{
	int error;

	/* The VBT environment. */
	error = drv_i915_vbt_world_create(display);
	if (error != 0)
		return error;

	/* The hotplug environment. */
	error = drv_i915_hpd_world_create(display);
	if (error != 0)
		return error;

	/* Succeeded: every environment has its state. */
	return 0;
}

/*
 * Sets again, after a resume, what the start's front set beside the
 * display core: the VGA plane off, the display's workarounds, and the
 * GMBUS controller's pin let go with its interrupts silenced.
 */
static void
i915_display_resume_front(
	struct i915_display *display)
{
	const struct i915_xd_platform *platform;
	int vga_result;

	/* The platform's description, when the display has one. */
	platform = NULL;
	if (display->xd.chosen)
		platform = display->xd.choice.platform;

	/* The VGA plane the firmware may have turned on again goes off. */
	vga_result = drv_i915_vga_disable(display);

	/* The display's workarounds, which the core's power-down lost. */
	if (platform != NULL)
		drv_i915_xd_display_wa(&display->xd.io, platform);

	/* The GMBUS controller lets its pin go and silences its interrupts. */
	drv_i915_xd_gmbus_reset(&display->xd.io);

	/* Logs the VGA plane's state. */
	kern_logf("i915: resume: vga %d, workarounds %s\n", vga_result, platform != NULL ? "applied" : "none");
}

/* Frees the hotplug's, the VBT parser's, the external DP ports' and the Type-C ports' state that exists, in reverse. */
static void
i915_display_destroy_worlds(
	struct i915_display *display)
{
	/* Each destroy leaves an absent state alone. */
	drv_i915_hpd_world_destroy(display);
	drv_i915_vbt_world_destroy(display);
	drv_i915_dp_ext_destroy(display);
	drv_i915_tc_port_destroy(display);
}

/* Returns the device's display when it exists and the firmware display check kept it; NULL otherwise. */
static struct i915_display *
i915_display_live(
	struct i915_device *device)
{
	/* A device without a display. */
	if (device->display == NULL)
		return NULL;

	/* A display the firmware display check left unused. */
	if (device->display->absent)
		return NULL;

	/* The display the stages work on. */
	return device->display;
}

/*
 * Prepares the display's state before its first write: the VBT parser,
 * the VGA arbiter client and the power wells' map.  Returns 0, or an
 * error with device->stage naming the step that failed.
 */
static int
i915_display_prepare_state(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_gt *gt;
	int error;

	display = device->display;
	gt = &device->gt;

	/*
	 * The VBT the parser reads: the explicit blob (when one is built in), the
	 * OpRegion's or the PCI ROM's, or the defaults when there is none.  A
	 * VBT that cannot be read never stops the start.
	 */
	(void)drv_i915_bios_init_ex(display, &display->vbt_state, &gt->pci, display->opregion_vbt_present, I915_VBT_EXPLICIT, &gt->trace);
	kern_logf("i915: display: VBT: source=%d found=%d version=%u child_devices=%u\n",
	    display->vbt_state.source,
	    display->vbt_state.vbt_found,
	    (unsigned)display->vbt_state.version,
	    display->vbt_state.num_display_devices);

	/* The VGA arbiter client; a machine without one is tolerated inside the call. */
	device->stage = "vga_register";
	error = drv_i915_vga_register(&display->vga_client, device->pci, gt->display_ver, &gt->trace);
	if (error != 0)
		return error;

	display->vga_registered = 1;

	/* The display's power: its lock, the DC states and the counts of its wells. */
	device->stage = "display_power";
	error = drv_i915_display_power_start(&display->power, &display->xd, &gt->trace);
	if (error != 0)
		return error;

	/* The stop releases the display's power from here on. */
	display->power_started = 1;

	/* Succeeded: the display's state is ready for its first write. */
	return 0;
}

/*
 * Reads what the firmware left through the xd parts and decides whether
 * the display is used (xd-takeover.c), before the first display write.
 * Read-only.  A display that is not used is left as the firmware left it.
 */
static void
i915_display_check_firmware(
	struct i915_device *device)
{
	struct i915_display *display;
	const struct i915_xd_firmware *firmware;
	struct i915_gt *gt;
	uint32_t reserved;
	uint32_t driver_first;
	uint32_t asls;
	char name[48];

	display = device->display;
	gt = &device->gt;
	firmware = &display->xd.firmware;

	/* The GGTT pages the driver writes: the top window and the display window. */
	reserved = I915_GT_GGTT_PAGES + I915_GT_DISPLAY_PAGES;
	driver_first = 0U;
	if (gt->ggtt_entries > reserved)
		driver_first = gt->ggtt_entries - reserved;

	/* Reads what the firmware left on the display and decides whether the driver may use it. */
	asls = drv_i915_pci_read32(&gt->pci, I915_PCI_ASLS);
	(void)drv_i915_xd_check_firmware(&display->xd, gt->gmadr_base, gt->gmadr_size, gt->ggtt_entries, driver_first, asls);
	kern_logf("i915: firmware: PCI COMMAND firmware=0x%04x now=0x%04x (memory and bus master set by the start, restored on teardown); no display register, GGTT entry or D-state written yet\n",
	    (unsigned)gt->pci.saved_command,
	    (unsigned)drv_i915_pci_read16(&gt->pci, I915_PCI_COMMAND_WORD));

	/* A wall: nothing on the display was touched, and nothing will be. */
	if (!firmware->use) {
		kern_logf("i915: firmware: the display is not used (%s); the GPU node has no display\n", firmware->reason);
		display->absent = 1;
	}

	/* The firmware's output, the only one the driver lights by itself (the GPU scanout rule). */
	drv_i915_gop_output_read(display->xd.choice.platform, firmware, &display->gop);
	drv_i915_gop_output_name(&display->gop, name, sizeof(name));
	kern_logf("i915: firmware: the firmware's output: %s (lit pipes 0x%x)\n", name, display->gop.pipes);

	/* One on an interface this driver cannot light keeps the firmware's picture: nothing on the display is touched. */
	if (firmware->use && display->gop.kind == I915_GOP_OTHER) {
		kern_logf("i915: firmware: the firmware's output (%s) is not one this driver lights: the firmware's picture is kept and the GPU node has no display\n", name);
		display->absent = 1;
	}
}

/*
 * Starts the display core: DC states off, the PCH reset handshake, the
 * combo PHYs, power well 1, the CD clock, the display buffer, the buddy
 * masks and the workarounds, then the start's power reference held and
 * every well's state read.  Returns 0, or EIO with device->stage naming
 * the step that met a fault.
 */
static int
i915_display_core_init(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_display_power *power;
	struct i915_gt *gt;
	int error;

	display = device->display;
	gt = &device->gt;
	power = &display->power;

	/* The CDCLK state of this display, over its xd description. */
	kern_memset(&display->cdclk, 0, sizeof(display->cdclk));
	drv_i915_cdclk_prepare(&display->cdclk, &display->xd);
	display->cdclk.m = &gt->mmio;
	display->cdclk.sb_lock = &gt->sb_lock;

	/* What the wells' changes use: the VGA client, the Type-C PHYs' windows, the CD clock; interrupts not enabled yet. */
	power->vga = &display->vga_client;
	power->dkl = &display->xd.dkl;
	power->cdclk = &display->cdclk;
	power->trace = &display->dc_trace;
	power->irqs_enabled = 0;

	/* Brings the display core up. */
	error = drv_i915_display_power_core_start(power, display->xd.bandwidth.dram_type, display->xd.bandwidth.channels);
	display->display_core_inited = power->start_held;

	/* A fault stops the start at the step that met it. */
	if (error != 0) {
		device->stage = "display_core";
		if (power->core_fault != NULL)
			device->stage = power->core_fault;

		/* Logs the step the fault stopped at. */
		kern_logf("i915: display: core: fault at %s\n", device->stage);
		return EIO;
	}

	/* Logs what the core started with. */
	kern_logf("i915: display: core started: cdclk=%u vco=%u dbuf=0x%x start reference %s\n",
	    display->cdclk.hw.cdclk,
	    display->cdclk.hw.vco,
	    power->dbuf_slices,
	    power->start_held ? "held" : "not held");

	/* Succeeded: the display core is up. */
	return 0;
}

/*
 * Queues the load of the DMC's firmware.  The DMC holds the start's power
 * reference until its worker has loaded the program; the start goes on
 * without waiting for the load.
 */
static void
i915_display_dmc_init(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_gt *gt;
	int error;

	display = device->display;
	gt = &device->gt;

	/* The DMC's workqueue; without it the DMC is not loaded. */
	error = drv_i915_workqueue_create(&display->dmc_wq, "i915-dmc");
	if (error != 0)
		return;

	/* Queues the load of the platform's firmware for the display's stepping. */
	kern_memset(&display->dmc_dev, 0, sizeof(display->dmc_dev));
	drv_i915_dmc_init(&display->dmc_dev, &display->dmc_wq, &gt->mmio, &display->power, &display->xd);
	display->dmc_inited = 1;
	kern_logf("i915: dmc: step=0x%02x load queued (path=%s, work_submitted=%d)\n",
	    (unsigned)display->xd.choice.step,
	    display->dmc_dev.fw_path != NULL ? display->dmc_dev.fw_path : "-",
	    display->dmc_dev.work_submitted);
}

/*
 * Keeps the memory at its points of the most bandwidth, where the display
 * controls SAGV, so that a picture never waits for the memory to change
 * its point.  A refusal of the firmware is logged and the start goes on.
 * Returns 0.
 */
static int
i915_display_bandwidth_start(
	struct i915_device *device)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_bandwidth *bw;
	struct i915_display *display;

	display = device->display;
	bw = &display->xd.bandwidth;
	platform = NULL;
	if (display->xd.chosen)
		platform = display->xd.choice.platform;

	/* Keeps SAGV off; a refusal is in the record and in the log. */
	(void)drv_i915_xd_bw_hold_sagv_off(&display->xd.io, platform, bw);
	kern_logf("i915: display: bandwidth: restricted=%d qgv=0x%x psf=0x%x mask=0x%x answer=%d sagv=%d\n",
	    bw->restricted,
	    bw->kept_qgv,
	    bw->kept_psf,
	    (unsigned)bw->restriction,
	    bw->restrict_error,
	    bw->sagv);

	/* Succeeded: the memory's points are settled. */
	return 0;
}

/*
 * The front of the display's start after the interrupts: the memory's
 * watermark latencies and SAGV block time, the GMBUS controller's reset,
 * the CD clock the hardware runs at, the start's workarounds, and the
 * VGA plane switched off.  A display without a description reads no
 * latencies and applies no workaround.
 */
static int
i915_display_memory_and_clocks(
	struct i915_device *device)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_memory *memory;
	struct i915_display *display;
	int sagv_controlled;
	int vga_result;
	int error;

	display = device->display;
	platform = display->xd.choice.platform;
	memory = &display->xd.memory;

	/* The SAGV's block time and the memory's watermark latencies; a display whose SAGV is not controlled blocks nothing. */
	sagv_controlled = 1;
	if (display->xd.bandwidth.sagv == I915_XD_SAGV_NOT_CONTROLLED)
		sagv_controlled = 0;

	/* Reads the latencies through the platform's mailbox and logs a read that failed. */
	kern_memset(memory, 0, sizeof(*memory));
	error = 0;
	if (platform != NULL)
		error = drv_i915_xd_memory_read(&display->xd.io, platform, sagv_controlled, 0, memory);
	if (error != 0)
		kern_logf("i915: wm: the memory's latencies could not be read (%d)\n", error);

	/* The GMBUS controller lets its pin go and silences its interrupts. */
	drv_i915_xd_gmbus_reset(&display->xd.io);

	/* The CD clock the hardware runs at. */
	drv_i915_cdclk_refresh(&display->cdclk);
	kern_logf("i915: display: CD clock: cdclk=%u vco=%u ref=%u voltage=%u\n", display->cdclk.hw.cdclk, display->cdclk.hw.vco, display->cdclk.hw.ref, (unsigned)display->cdclk.hw.voltage_level);

	/* The start's workarounds of the display's version. */
	if (platform != NULL)
		drv_i915_xd_display_wa(&display->xd.io, platform);

	/* The VGA plane the driver never uses goes off. */
	vga_result = drv_i915_vga_disable(display);

	/* The front is done; the stop takes the panel's device apart from here on. */
	display->front_done = 1;
	kern_logf("i915: wm: levels=%u latency=%u/%u/%u/%u/%u/%u/%u/%u valid=%d sagv_status=%d block_time=%uus (mailbox %d) | vga %d\n",
	    memory->levels,
	    memory->latency[0],
	    memory->latency[1],
	    memory->latency[2],
	    memory->latency[3],
	    memory->latency[4],
	    memory->latency[5],
	    memory->latency[6],
	    memory->latency[7],
	    memory->valid,
	    display->xd.bandwidth.sagv,
	    memory->sagv_block_us,
	    memory->sagv_error,
	    vga_result);

	/* Succeeded: the front is done. */
	return 0;
}

/*
 * Makes the table of the display's ports from the VBT and builds the
 * built-in panel's connector on the port that carries it.  A panel that
 * could not be taken drops its port; the hotplug path and the Type-C
 * ports read the table that is left.
 */
static void
i915_display_setup_outputs(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_xd_ports *ports;
	struct i915_xd_port *port;
	unsigned index;
	unsigned kept;
	int connector;

	display = device->display;
	ports = &display->xd.ports;

	/* The ports the VBT describes on the DDIs this driver's hotplug and Type-C paths handle. */
	drv_i915_xd_ports(display->xd.choice.platform, &display->xd.vbt, I915_DISPLAY_PORT_MASK, ports);

	/* The panel's device is prepared; its connector is built on the first DisplayPort port the VBT gives the panel. */
	drv_i915_edp_device_prepare(&display->edp_dev, &device->gt.mmio, &display->power, &display->vbt_state);
	kept = 0U;
	for (index = 0U; index < ports->n; index++) {
		port = &ports->port[index];

		/* Builds the panel's connector on a DisplayPort port; a port without the panel answers 1. */
		connector = 1;
		if (port->dp)
			connector = drv_i915_edp_device_init_connector(&display->edp_dev, (int)port->ddi);

		/* A panel that could not be taken drops its port. */
		if (connector < 0) {
			kern_logf("i915: display: outputs: the panel on DDI %c could not be taken (%d): its port is dropped\n", (char)('A' + port->ddi), connector);
			ports->skipped++;
			continue;
		}

		/* Keeps the port. */
		ports->port[kept] = *port;
		kept++;
	}

	/* The ports kept. */
	ports->n = kept;

	/* Names every port. */
	kern_logf("i915: display: outputs: vbt outputs=%u ports=%u skipped=%u\n", display->xd.vbt.nchildren, ports->n, ports->skipped);
	for (index = 0U; index < ports->n; index++) {
		port = &ports->port[index];
		kern_logf("i915: port[%u]: DDI %c tc=%u dp=%d hdmi=%d edp=%d aux=%u type=0x%x\n",
		    index,
		    (char)('A' + port->ddi),
		    (unsigned)port->tc,
		    port->dp,
		    port->hdmi,
		    port->edp,
		    (unsigned)port->aux,
		    (unsigned)port->device_type);
	}
}

/*
 * Corrects what the firmware left on but unused through the xd takeover's
 * record: a lit
 * pipe's DMC, the frame buffer compressor, the clocks of the ports and
 * the PLLs no lit pipe uses; and, when nothing is lit, the power wells
 * nothing references.  The lit pipes are left to the takeover.
 */
static void
i915_display_sanitize(
	struct i915_device *device)
{
	struct i915_xd_sanitize_report report;
	struct i915_display *display;
	unsigned wells;

	display = device->display;

	/* A device without a description has no xd parts to correct with. */
	kern_memset(&report, 0, sizeof(report));
	if (display->xd.chosen)
		drv_i915_xd_firmware_sanitize(&display->xd.io, display->xd.choice.platform, &display->xd.dmc, &display->xd.firmware, &report);

	/*
	 * The wells nothing references: with a lit pipe its DDI IO and AUX
	 * wells look unused, and turning them off under the running pipe
	 * would keep the takeover from stopping it, so they wait for the
	 * takeover.
	 */
	wells = 0U;
	if (display->xd.firmware.lit_pipes != 0U)
		kern_logf("i915: power wells: kept (lit pipes 0x%x: the firmware display is left to the takeover)\n", display->xd.firmware.lit_pipes);
	else
		wells = drv_i915_display_power_sanitize(&display->power);

	/* Logs what the sanitize corrected. */
	kern_logf("i915: sanitize: lit pipes 0x%x, dmc pipes 0x%x, fbc off %d, clocks gated 0x%x, PLLs off 0x%x%s, wells off %u\n",
	    display->xd.firmware.lit_pipes,
	    report.dmc_pipes,
	    report.fbc_off,
	    report.clocks_gated,
	    report.plls_off,
	    report.plls_kept ? " (kept: a lit port's PLL is unknown)" : "",
	    wells);
}

/*
 * Counts the pipes the firmware left lit, arms the hotplug pins of the
 * display's ports, and enables IPC.
 */
static void
i915_display_arm(
	struct i915_display *display,
	int interrupts_enabled)
{
	struct i915_driver_probe *dprobe;

	/* The probe state the counts go to. */
	dprobe = &display->dprobe;

	/* The pipes the firmware left lit: the resident start's takeover or a stop of the start. */
	dprobe->active_crtcs = (unsigned)__builtin_popcount(display->xd.firmware.lit_pipes);

	/* The connectors of the display's ports, and their hotplug pins armed. */
	drv_i915_hpd_arm(display, interrupts_enabled);

	/* IPC, which every display 12 and 13 controller has. */
	dprobe->ipc_enabled = i915_display_enable_ipc(&display->device->gt.mmio);
}

/*
 * Enables IPC (the isochronous priority control of the display's memory
 * requests).  Returns 1: it is enabled.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (DISP_ARB_CTL2, DISP_IPC_ENABLE).
 */
static int
i915_display_enable_ipc(
	struct i915_mmio *mmio)
{
	/* Sets the enable bit, writing only a change. */
	(void)i915_display_rmw(mmio, I915_DISP_ARB_CTL2, I915_DISP_IPC_ENABLE, I915_DISP_IPC_ENABLE);

	/* Succeeded: IPC is enabled. */
	return 1;
}

/* Writes a register only when the value changes; returns the new value. */
static uint32_t
i915_display_rmw(
	struct i915_mmio *m,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	uint32_t old;
	uint32_t val;

	/* Computes the new value from the old one. */
	old = drv_i915_read32(m, reg);
	val = (old & ~clear) | set;

	/* Writes only a change. */
	if (val != old)
		drv_i915_write32(m, reg, val);

	/* Reports the new value. */
	return val;
}

/*
 * Gives the start's power reference back, so that every well nothing
 * holds powers down, and gives the runtime reference the PCI start took
 * back to the runtime power.  With the firmware's display left to the
 * takeover nothing holds its wells yet, so the start's power reference is
 * kept until the takeover has taken them.
 */
static void
i915_display_publish(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_driver_probe *dprobe;

	display = device->display;
	dprobe = &display->dprobe;

	/*
	 * The start's power reference, or its deferral to the takeover (the
	 * wells then stay as they are: the same count before and after).
	 */
	if (display->xd.firmware.take_over) {
		dprobe->power_release_deferred = 1;
		dprobe->wells_on_before = drv_i915_display_power_wells_on(&display->power);
		dprobe->wells_on_after = dprobe->wells_on_before;
		dprobe->dc_state_after = (int)display->xd.power.dc_state;
		kern_logf("i915: display: the start's power reference is kept until the takeover\n");
	} else {
		drv_i915_display_power_serve(&display->power, dprobe);
	}

	/* The runtime reference goes back. */
	drv_i915_rpm_put(&device->gt.probe_pm);
	dprobe->rpm_usage_after = drv_i915_rpm_usage(&device->gt.probe_pm);
	dprobe->registered = 1;
}

/*
 * Takes the runtime reference back from the runtime power and the start's
 * power reference again, before the display stops.
 */
static void
i915_display_unpublish(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_driver_probe *dprobe;

	display = device->display;
	dprobe = &display->dprobe;

	/* Nothing was given back. */
	if (!dprobe->registered)
		return;

	/* The runtime reference is taken back. */
	(void)drv_i915_rpm_get_sync(&device->gt.probe_pm);

	/* The start's power reference is taken again. */
	drv_i915_display_power_hold(&display->power);
	dprobe->registered = 0;
}

/* Tells whether a masked PCH id and a subsystem name an emulated bridge. */
static int
i915_display_pch_emulated(
	uint16_t id,
	uint16_t svid,
	uint16_t sdid)
{
	/* The ids of virtualized platforms' bridges. */
	if (id == I915_PCH_ID_EMULATED_P2X)
		return 1;
	if (id == I915_PCH_ID_EMULATED_P3X)
		return 1;

	/* QEMU's bridge with the QEMU subsystem. */
	if (id == I915_PCH_ID_EMULATED_QEMU &&
	    svid == I915_PCI_SUBVENDOR_QUMRANET &&
	    sdid == I915_PCI_SUBDEVICE_QEMU)
		return 1;

	/* A real bridge. */
	return 0;
}

/*
 * Guesses the PCH of a guest whose ISA bridge was not passed through:
 * the Alder Lake PCH beside an Alder Lake GPU, none otherwise.
 */
static void
i915_display_pch_guess(
	int is_alderlake,
	uint16_t *pch_id,
	int *pch_type)
{
	uint16_t id;

	/* The Alder Lake PCH, or none. */
	id = 0U;
	if (is_alderlake)
		id = I915_PCH_ID_ALDER_LAKE;

	/* Names the guess. */
	kern_logf("i915: display: PCH assumed: id 0x%04x\n", (unsigned)id);

	/* Reports the kind and the id. */
	*pch_type = drv_i915_xd_pch_kind(id);
	*pch_id = id;
}

/*
 * The ISA-bridge walk: a PCI class scan.  The identity is read from the
 * configuration space rather than the cached PCI header, which reads back
 * all-ones for the passthrough function on this host.  Returns 1 with the
 * ids filled while bridges remain, 0 when the walk is finished.
 */
static int
i915_next_isa_bridge(
	struct i915_display *display,
	unsigned index,
	uint16_t *vendor,
	uint16_t *device,
	uint16_t *svid,
	uint16_t *sdid)
{
	struct drv_pci_device *bridge;
	uint16_t v;
	uint16_t p;
	uint16_t sv;
	uint16_t sd;

	/* A new walk starts from the first bridge. */
	if (index == 0U)
		display->next_isa_bridge_cursor = NULL;

	/* The next bridge after the cursor. */
	bridge = drv_pci_find_class(I915_PCI_CLASS_BRIDGE_ISA, I915_PCI_CLASS_BRIDGE_ISA_MASK, display->next_isa_bridge_cursor);
	if (bridge == NULL)
		return 0;

	display->next_isa_bridge_cursor = bridge;

	/* Reads its identity from the configuration space. */
	v = 0U;
	p = 0U;
	sv = 0U;
	sd = 0U;
	(void)drv_pci_device_config_read16(bridge, 0x00U, &v);
	(void)drv_pci_device_config_read16(bridge, 0x02U, &p);
	(void)drv_pci_device_config_read16(bridge, 0x2cU, &sv);
	(void)drv_pci_device_config_read16(bridge, 0x2eU, &sd);
	*vendor = v;
	*device = p;
	*svid = sv;
	*sdid = sd;

	/* A bridge was found. */
	return 1;
}

/*
 * Describes the node's one display (the display query operation): the
 * eDP panel, connected, FIFO presentation of shared (BLOB) frames, one
 * plane, BGRA or RGBA, the panel's own mode and physical size.
 */
static int
i915_display_query(
	void *device,
	void *session,
	struct gpu_display_info *request)
{
	struct i915_hpd_output output;
	struct i915_device *owner_device;
	struct i915_display *display;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	uint32_t width_mm;
	uint32_t height_mm;
	unsigned connector;
	unsigned kind;
	int resident;
	int connected;
	int port;
	int error;
	int size_error;

	UNUSED_PARAMETER(session);

	owner_device = device;
	display = owner_device->display;

	/* The resident output while its mode is known, then the other connectors. */
	error = drv_i915_display_panel(display, &width, &height, &refresh);
	resident = i915_display_resident_connector(display);
	request->count = 0U;
	if (error == 0)
		request->count = i915_display_count(display, resident);

	/* Only the count was asked for. */
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;

	/* Nothing without the resident output, or past the inventory. */
	if (error != 0 || request->index >= request->count)
		return EINVAL;

	/* Another connector of the inventory. */
	if (request->index != 0U) {
		error = i915_display_other(display, resident, request->index, &connector);
		if (error != 0)
			return error;
		error = i915_display_other_query(owner_device, connector, request);
		return error;
	}

	/* The display, by its connector's ID and generation, its state and its formats. */
	(void)drv_i915_display_resident_identity(display, &request->display_id, &request->generation, &connected);
	/* Frames are presented from shared resources (present.c), so BLOB is offered. */
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB;
	if (display->rd.active)
		request->flags |= GPU_DISPLAY_ACTIVE;

	/* The pipe's frames are its refresh boundaries; the panel's light is its power (not an HDMI display's). */
	request->flags |= GPU_DISPLAY_REFRESH_COUNTER;
	if (display->output.kind == I915_OUTPUT_KIND_PANEL && !display->output.none)
		request->flags |= GPU_DISPLAY_POWER_CONTROL;
	if (display->rd.power_off)
		request->flags |= GPU_DISPLAY_POWERED_OFF;

	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = I915_DISPLAY_MAX_FRAME_BYTES;

	/* The panel's own mode is the current, preferred and largest one. */
	request->current_width = width;
	request->current_height = height;
	request->preferred_width = width;
	request->preferred_height = height;
	request->max_width = width;
	request->max_height = height;
	request->refresh_millihz = refresh;

	/* The physical size, when the output reports one. */
	size_error = drv_i915_display_output_size_mm(display, &width_mm, &height_mm);
	if (size_error == 0) {
		request->physical_width_mm = width_mm;
		request->physical_height_mm = height_mm;
	}

	/* The output's name: its port's key: the panel's DDI A, or the other output's port. */
	kind = I915_HPD_OUTPUT_EDP;
	port = 0;
	if (display->output.kind == I915_OUTPUT_KIND_HDMI) {
		kind = I915_HPD_OUTPUT_HDMI;
		port = display->output.port;
	} else if (display->output.kind == I915_OUTPUT_KIND_DP_EXT) {
		kind = I915_HPD_OUTPUT_DP;
	}

	/* The port of its connector, when the hotplug path has one. */
	if (resident >= 0) {
		error = drv_i915_hpd_output(display, (unsigned)resident, &output);
		if (error == 0)
			port = output.port;
	}

	/* The key. */
	i915_display_key(owner_device, kind, port, request->name, sizeof(request->name));

	/* Succeeded: the display is described. */
	return 0;
}

/*
 * Enumerates or checks a mode of the node's display (the display mode
 * operation).
 *
 * One native mode, the panel's.  A smaller frame at the panel's refresh is
 * accepted and shown scaled into the panel's own timing: the display is
 * never re-timed.  XXX: another refresh rate is refused rather than
 * pretended.
 */
static int
i915_display_mode(
	void *device,
	void *session,
	struct gpu_display_mode *request)
{
	struct i915_device *owner_device;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	int resident;
	int connected;
	int error;

	UNUSED_PARAMETER(session);

	owner_device = device;

	/* The resident display of this generation, or another connector. */
	error = drv_i915_display_which(owner_device->display, request->display_id, request->generation, &resident, &connected);
	if (error != 0)
		return error;

	/* Another connector of the inventory: its preferred mode, which it is not lit with. */
	if (!resident) {
		error = i915_display_other_mode(owner_device->display, request);
		return error;
	}

	/* The panel's mode. */
	error = drv_i915_display_panel(owner_device->display, &width, &height, &refresh);
	if (error != 0)
		return error;

	/* Enumeration: the one native mode. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = 1U;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index != 0U)
			return EINVAL;

		request->width = width;
		request->height = height;
		request->refresh_millihz = refresh;
		return 0;
	}

	/* A frame larger than the panel is refused. */
	if (request->width > width || request->height > height)
		return EINVAL;

	/* The panel's refresh is the only one; 0 asks for it. */
	if (request->refresh_millihz == 0U)
		request->refresh_millihz = refresh;

	if (request->refresh_millihz != refresh) {
		kern_logf("i915: resident display: XXX mode %ux%u@%u mHz refused: the panel runs at %u mHz only\n",
		    request->width,
		    request->height,
		    request->refresh_millihz,
		    refresh);
		return EINVAL;
	}

	/* Succeeded: the mode is shown scaled into the panel's timing. */
	return 0;
}

/*
 * Gives a display's lease to a session (the display claim operation).
 *
 * The resident output has one lease at a time; another connected connector
 * becomes the second output while the resident lease is held,
 * or the resident output moves to it while none is.  Returns
 * 0 with the lease, ENOENT or ESTALE for another display, EINVAL for a
 * plane other than 0, EBUSY while another session holds the resident
 * lease, ENOSPC for the limit of the outputs shown at once, or ENXIO for
 * a connector with nothing connected.
 */
static int
i915_display_claim(
	void *device,
	void *session,
	struct gpu_display_claim *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;
	unsigned long irq;
	uint64_t head_lease;
	unsigned connector;
	int resident;
	int connected;
	int error;

	owner_device = device;
	rd = &owner_device->display->rd;

	/* The resident display of this generation, or another connector. */
	error = drv_i915_display_which(owner_device->display, request->display_id, request->generation, &resident, &connected);
	if (error != 0)
		return error;

	/* Only plane 0 exists. */
	if (request->plane_index != 0U)
		return EINVAL;

	/*
	 * Another connector.  With the resident output's lease held it becomes
	 * the second output, the head, or the claim is the limit
	 * of the outputs shown at once, told as one (ENOSPC), not a failure.
	 * With no lease held the output moves to it (Keiland's explicit claim).
	 */
	if (!resident) {
		if (!connected)
			return ENXIO;
		connector = request->display_id - I915_DISPLAY_OTHER_ID;
		error = drv_i915_head_claim(owner_device, session, connector, request->generation, &head_lease);
		if (error == 0) {
			request->lease = head_lease;
			return 0;
		}

		/* EAGAIN: no lease is held, and the resident output moves instead. */
		if (error != EAGAIN)
			return error;
		error = i915_display_move(owner_device, connector);
		if (error != 0)
			return error;
	}

	/* Takes the lease under its mutex. */
	drv_i915_present_lease_init(owner_device->display);
	mutex_lock(&rd->mutex);

	/* Another session holds it. */
	if (rd->owner != NULL) {
		mutex_unlock(&rd->mutex);
		return EBUSY;
	}

	/* The session holds a new lease with no presentation yet. */
	rd->owner = session;
	rd->lease = rd->next_lease;
	rd->next_lease++;
	rd->sequence = 0U;
	request->lease = rd->lease;

	/*
	 * A moved output that failed failed only the lease that lit it:
	 * this lease's first frame lights an output again.
	 */
	irq = spin_lock_irqsave(&owner_device->irq_lock);

	owner_device->display->window.lease_failed = 0;

	spin_unlock_irqrestore(&owner_device->irq_lock, irq);

	mutex_unlock(&rd->mutex);

	kern_logf("i915: resident display: lease %llu claimed\n", (unsigned long long)request->lease);

	/* Succeeded: the session holds the display. */
	return 0;
}

/*
 * Gives the resident output's display ID and generation, and whether its
 * connector is connected: its connector's ID and generation
 * in the inventory, so that a display keeps one identity whichever output
 * the node lights; display 1 of generation 1 without the hotplug path.
 */
int
drv_i915_display_resident_identity(
	struct i915_display *display,
	uint32_t *display_id,
	uint64_t *generation,
	int *connected)
{
	struct i915_hpd_output output;
	int resident;
	int error;

	/* Without the hotplug path's connector: the fixed identity. */
	*display_id = I915_DISPLAY_ID;
	*generation = I915_DISPLAY_GENERATION;
	*connected = 1;
	resident = i915_display_resident_connector(display);
	if (resident < 0)
		return 0;

	/*
	 * The connector as the topology last took it, the built-in panel
	 * connected while the node has it (output.c): the hotplug path never
	 * detects an eDP connector (taken as unplugged, every frame
	 * was refused with ENXIO).
	 */
	error = drv_i915_display_output_connector(display, (unsigned)resident, &output);
	if (error != 0)
		return 0;

	/* The connector's identity and generation, and whether it is connected. */
	*display_id = I915_DISPLAY_OTHER_ID + (uint32_t)resident;
	*generation = output.generation;
	*connected = output.connected;

	/* Succeeded: the connector's identity. */
	return 0;
}

/*
 * Tells whether a display ID and generation name the resident output
 * (resident 1) or another connector (resident 0, with whether it is
 * connected).  Returns 0, ENOENT for no display of the ID, or ESTALE for
 * another generation.
 */
int
drv_i915_display_which(
	struct i915_display *display,
	uint32_t display_id,
	uint64_t generation,
	int *resident,
	int *connected)
{
	uint32_t resident_id;
	uint64_t resident_generation;
	int error;

	/* The resident output's identity. */
	(void)drv_i915_display_resident_identity(display, &resident_id, &resident_generation, connected);
	if (display_id == resident_id) {
		*resident = 1;
		if (generation != resident_generation)
			return ESTALE;
		return 0;
	}

	/* Another connector of the inventory. */
	*resident = 0;
	if (display_id < I915_DISPLAY_OTHER_ID)
		return ENOENT;
	error = i915_display_other_check(display, display_id, generation, connected);
	if (error != 0)
		return error;

	/* Succeeded: another connector. */
	return 0;
}

/*
 * Brings the firmware's output back as the resident output when a moved
 * output's last hold ended with no lease held (D-RELEASE):
 * the output is not lit until the next claim and frame.  Runs on the
 * worker after it left the display window, before the hold is said to be
 * over, so that a claim waiting for the hold's end moves the output after
 * this.
 */
void
drv_i915_display_output_back(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_resident_display *rd;
	unsigned long irq;
	void *owner;
	int differ;
	int locked;

	/* The firmware's output already. */
	display = device->display;
	differ = i915_display_outputs_differ(&display->output, &display->gop_output);
	if (!differ)
		return;

	/*
	 * A lease held keeps its output.  The worker does not wait for the
	 * lease mutex: a presentation or a release holds it while it waits for
	 * the worker.  A mutex in use is a lease in use, and the output stays;
	 * the next claim of the firmware's connector moves it back.
	 */
	rd = &display->rd;
	drv_i915_present_lease_init(display);
	locked = mutex_trylock(&rd->mutex);
	if (!locked)
		return;

	/* The lease's holder, under the mutex taken. */
	owner = rd->owner;

	mutex_unlock(&rd->mutex);

	/* A lease is held: nothing comes back. */
	if (owner != NULL)
		return;

	/*
	 * A second output still claimed keeps the resident output where it is:
	 * the firmware's output may be the second
	 * output's connector or pipe.  Its release ends that, and the next
	 * claim of the firmware's connector moves the output back.
	 */
	if (display->head.claimed)
		return;

	/* The firmware's output, under the lock the worker's readers share. */
	irq = spin_lock_irqsave(&device->irq_lock);

	display->output = display->gop_output;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the firmware's output is the resident output again. */
	kern_logf("i915: resident display: the firmware's output (%s) is the output again\n", drv_i915_display_output_name(display));
}

/*
 * Brings the firmware's output back as the resident output after a moved
 * output's run failed (a failed move gives the earlier output
 * back), with a lease held or not.
 *
 * Runs on the worker after it left the display window.  The rest of the
 * lease's presentations fail (lease_failed) instead of lighting the
 * firmware's output for a session that claimed another one; the next
 * claimed lease lights an output again.  The output is not lit here.
 */
void
drv_i915_display_output_fail_back(
	struct i915_device *device)
{
	struct i915_display *display;
	unsigned long irq;

	/* The display whose output comes back. */
	display = device->display;

	/*
	 * The firmware's output, under the lock the worker's readers share;
	 * lease_failed makes the presentations of the lease that failed fail
	 * outside the window, so they do not run the failed output again.
	 */
	irq = spin_lock_irqsave(&device->irq_lock);

	display->output = display->gop_output;
	display->window.lease_failed = 1;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the firmware's output is the resident output again. */
	kern_logf("i915: resident display: the moved output failed; the firmware's output (%s) is the output again, and the lease's presentations fail until the next claim\n", drv_i915_display_output_name(display));
}

/*
 * Gives up the firmware's own output after its display was unplugged while
 * it was lit: the driver does
 * not light another output in its place (Keiland
 * decides), and the rest of the lease's presentations fail (ENXIO).
 *
 * The built-in panel, when the machine has one, stands as the resident
 * output from here on, not lit: the display sessions list it, and
 * Keiland's claim and first frame light it, as after a moved output's
 * fail-back.  It becomes the firmware's output too, so that a moved
 * output's end comes back to it and not to the display that went.
 * Without a panel the node has no output (none), as at a start whose
 * firmware's output cannot be driven.  Runs on the worker after it left
 * the display window.
 */
void
drv_i915_display_output_lost(
	struct i915_device *device)
{
	struct i915_display_output panel;
	struct i915_display *display;
	unsigned long irq;

	/* The display whose output is given up, and the panel that may stand for it. */
	display = device->display;
	drv_i915_display_output_panel(display, &panel);

	/* A machine without the built-in panel's connector has no output from here on. */
	if (!panel.has_connector) {
		kern_memset(&panel, 0, sizeof(panel));
		panel.none = 1;
	}

	/*
	 * The output and the firmware's output, under the lock the worker's
	 * readers share; lease_failed makes the lease's presentations fail
	 * outside the window until the next claim.
	 */
	irq = spin_lock_irqsave(&device->irq_lock);

	display->output = panel;
	display->gop_output = panel;
	display->window.lease_failed = 1;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: nothing is lit until a display is claimed. */
	if (panel.none) {
		kern_logf("i915: resident display: the firmware's output was unplugged; the node has no output (no built-in panel), and the lease's presentations fail\n");
	} else {
		kern_logf("i915: resident display: the firmware's output was unplugged; the built-in panel is the output, not lit until a display is claimed, and the lease's presentations fail\n");
	}
}

/*
 * Reports whether the resident output is a moved one, not the output the
 * firmware left lit.
 */
int
drv_i915_display_output_moved(
	const struct i915_display *display)
{
	int differ;

	/* Another kind or connector than the firmware's output. */
	differ = i915_display_outputs_differ(&display->output, &display->gop_output);
	if (differ)
		return 1;

	/* Succeeded: the firmware's output. */
	return 0;
}

/*
 * Reports the display topology's sequence (the display events operation):
 * the hotplug path moves it when a connector is connected or disconnected.
 * Any context: no hardware access, no wait.
 */
int
drv_i915_display_events(
	void *device,
	void *session,
	uint64_t *sequence)
{
	struct i915_device *owner_device;

	UNUSED_PARAMETER(session);

	/* The sequence of the device's display, or the first snapshot without one. */
	owner_device = device;
	*sequence = 1U;
	if (owner_device->display != NULL)
		*sequence = drv_i915_hpd_topology_sequence(owner_device->display);

	/* Succeeded: the sequence is sampled. */
	return 0;
}

/*
 * Finds the hotplug path's connector of the resident output: the HDMI
 * connector when it drives HDMI, the eDP connector otherwise; -1 when the
 * path has none.
 */
static int
i915_display_resident_connector(
	struct i915_display *display)
{
	struct i915_hpd_summary summary;
	struct i915_hpd_output output;
	unsigned count;
	unsigned index;
	int error;

	/* Nothing without the hotplug path. */
	if (!display->hpd_started)
		return -1;

	/* The connector the output names. */
	if (display->output.has_connector)
		return (int)display->output.connector;

	/* The HDMI connector. */
	if (display->output.kind == I915_OUTPUT_KIND_HDMI) {
		drv_i915_hpd_summary(display, &summary);
		return summary.hdmi_connector;
	}

	/* The first eDP connector. */
	count = drv_i915_hpd_output_count(display);
	for (index = 0U; index < count; index++) {
		error = drv_i915_hpd_output(display, index, &output);
		if (error == 0 && output.kind == I915_HPD_OUTPUT_EDP)
			return (int)index;
	}

	/* None. */
	return -1;
}

/* Counts the inventory: the resident output and the hotplug path's other connectors. */
static unsigned
i915_display_count(
	struct i915_display *display,
	int resident)
{
	unsigned count;

	/* The other connectors, without the resident output's. */
	count = drv_i915_hpd_output_count(display);
	if (resident >= 0 && count > 0U)
		count--;

	/* And the resident output. */
	return count + 1U;
}

/* Finds the connector of an inventory index past 0 (the connectors in order, the resident output's left out); 0 or EINVAL. */
static int
i915_display_other(
	struct i915_display *display,
	int resident,
	uint32_t index,
	unsigned *connector)
{
	unsigned count;
	unsigned slot;
	unsigned found;

	/* Walks the connectors, counting those that are not the resident output's. */
	count = drv_i915_hpd_output_count(display);
	found = 0U;
	for (slot = 0U; slot < count; slot++) {
		if ((int)slot == resident)
			continue;
		found++;
		if (found == index) {
			*connector = slot;
			return 0;
		}
	}

	/* Past the inventory. */
	return EINVAL;
}

/*
 * Writes a connector's key (main's adoption A2): the version,
 * the GPU's PCI segment, bus, device and function, the kind and the port,
 * for example "zedbsd-port-v1:pci:0000:00:02.0:edp:A".
 */
static void
i915_display_key(
	struct i915_device *device,
	unsigned kind,
	int port,
	char *name,
	unsigned size)
{
	static const char *const kinds[] = { "other", "edp", "hdmi", "dp" };
	struct drv_pci_address address;
	char letter[8];

	/* The GPU's place on the PCI bus. */
	drv_pci_device_address(device->pci, &address);

	/* The port: A, B, C, then the Type-C ports. */
	if (port >= 3)
		kern_snprintf(letter, sizeof(letter), "TC%d", port - 2);
	else if (port >= 0)
		kern_snprintf(letter, sizeof(letter), "%c", 'A' + port);
	else
		kern_snprintf(letter, sizeof(letter), "none");

	/* The key. */
	kern_snprintf(name, size, "zedbsd-port-v1:pci:%04x:%02x:%02x.%x:%s:%s",
	    (unsigned)address.segment,
	    (unsigned)address.bus,
	    (unsigned)address.device,
	    (unsigned)address.function,
	    kinds[kind & 3U],
	    letter);
}

/*
 * Describes another connector of the inventory: connected or not, its
 * preferred mode (from its EDID) as the current, preferred and largest
 * one, one plane, not active (this driver does not light it).
 */
static int
i915_display_other_query(
	struct i915_device *device,
	unsigned connector,
	struct gpu_display_info *request)
{
	struct i915_hpd_output output;
	int is_head;
	int lit;
	int limited;
	int error;

	/* The connector as the topology last took it (the panel connected, with its mode, while the node has it). */
	error = drv_i915_display_output_connector(device->display, connector, &output);
	if (error != 0)
		return EINVAL;

	/* Its identity and state. */
	request->display_id = I915_DISPLAY_OTHER_ID + connector;
	request->generation = output.generation;
	request->flags = GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB;

	/*
	 * A connected one: the second output when it is the head, lit or not;
	 * otherwise the limit of outputs shown at once only when
	 * its claim now answers ENOSPC (D-LIMIT).
	 */
	drv_i915_head_connector_state(device->display, connector, &is_head, &lit, &limited);
	if (output.connected) {
		request->flags |= GPU_DISPLAY_CONNECTED;
		if (lit)
			request->flags |= GPU_DISPLAY_ACTIVE;
		if (limited)
			request->flags |= GPU_DISPLAY_LIMITED;
	}

	/* One plane, the formats and the frame size of every output. */
	request->plane_count = 1U;
	request->formats = GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = I915_DISPLAY_MAX_FRAME_BYTES;

	/* Its preferred mode and size, when its EDID gave them. */
	request->current_width = output.width;
	request->current_height = output.height;
	request->preferred_width = output.width;
	request->preferred_height = output.height;
	request->max_width = output.width;
	request->max_height = output.height;
	request->refresh_millihz = output.refresh_millihz;
	request->physical_width_mm = output.width_mm;
	request->physical_height_mm = output.height_mm;

	/* Its key. */
	i915_display_key(device, output.kind, output.port, request->name, sizeof(request->name));

	/* Succeeded: the connector is described. */
	return 0;
}

/*
 * Enumerates or checks the mode of another connector: its preferred mode
 * only, of its current generation, while it is connected.
 */
static int
i915_display_other_mode(
	struct i915_display *display,
	struct gpu_display_mode *request)
{
	struct i915_hpd_output output;
	int error;

	/* The connector of the ID, of this generation, connected and with a mode (the panel's own while the node has it). */
	error = drv_i915_display_output_connector(display, request->display_id - I915_DISPLAY_OTHER_ID, &output);
	if (error != 0)
		return ENOENT;
	if (request->generation != output.generation)
		return ESTALE;
	if (!output.connected || output.width == 0U)
		return ENOENT;

	/* Enumeration: the one preferred mode. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		request->count = 1U;
		if (request->index == GPU_DISPLAY_COUNT_ONLY)
			return 0;
		if (request->index != 0U)
			return EINVAL;
		request->width = output.width;
		request->height = output.height;
		request->refresh_millihz = output.refresh_millihz;
		return 0;
	}

	/* Validation: the preferred mode only (0 asks for its refresh). */
	if (request->width != output.width || request->height != output.height)
		return EINVAL;
	if (request->refresh_millihz == 0U)
		request->refresh_millihz = output.refresh_millihz;
	if (request->refresh_millihz != output.refresh_millihz)
		return EINVAL;

	/* Succeeded: the mode is the connector's. */
	return 0;
}

/*
 * Powers the resident display's panel off or on for the open holding its
 * lease (the display power operation).
 *
 * Another connector is not lit and has no lease: EBUSY (ENOENT or ESTALE
 * for an unknown one).  An HDMI display in the panel's place has no light
 * to switch: EOPNOTSUPP.
 */
static int
i915_display_power(
	void *device,
	void *session,
	const struct gpu_display_power *request)
{
	struct i915_device *owner_device;
	struct i915_display *display;
	int resident;
	int connected;
	int is_head;
	int lit;
	int limited;
	int off;
	int error;

	/* The device's display. */
	owner_device = device;
	display = owner_device->display;

	/* The resident display of this generation, or another connector. */
	error = drv_i915_display_which(display, request->display_id, request->generation, &resident, &connected);
	if (error != 0)
		return error;

	/*
	 * Another connector: the second output has no light of ours to switch,
	 * and no other connector is leased.
	 */
	if (!resident) {
		if (!connected)
			return ENXIO;
		drv_i915_head_connector_state(display, request->display_id - I915_DISPLAY_OTHER_ID, &is_head, &lit, &limited);
		if (is_head)
			return EOPNOTSUPP;
		return EBUSY;
	}

	/* Only the eDP panel's light is switched. */
	if (display->output.kind != I915_OUTPUT_KIND_PANEL || display->output.none)
		return EOPNOTSUPP;

	/* The display control switches it for the lease holder. */
	off = 0;
	if (request->state != GPU_DISPLAY_POWER_ON)
		off = 1;
	error = drv_i915_display_power_set(owner_device, session, off);
	if (error != 0)
		return error;

	/* Succeeded: the panel has the requested power. */
	return 0;
}

/*
 * Waits for a refresh boundary of a display (the display refresh
 * operation): the resident output's pipe frames.  Another
 * connector is not lit and makes no boundary.
 */
static int
i915_display_refresh(
	void *device,
	void *session,
	struct gpu_display_refresh *request)
{
	struct i915_device *owner_device;
	struct i915_display *display;
	int resident;
	int connected;
	int lit_possible;
	int error;

	UNUSED_PARAMETER(session);

	/* The device's display. */
	owner_device = device;
	display = owner_device->display;

	/* The resident display of this generation, or another connector, which is not lit. */
	error = drv_i915_display_which(display, request->display_id, request->generation, &resident, &connected);
	if (error != 0)
		return error;
	lit_possible = 1;
	if (!resident) {
		if (!connected)
			return ENXIO;
		lit_possible = 0;
	}

	/* The display control waits for the boundary. */
	error = drv_i915_display_refresh_wait(owner_device, lit_possible, request);
	if (error != 0)
		return error;

	/* Succeeded: the boundary after the cursor. */
	return 0;
}

/* Checks another connector's ID and generation; ENOENT or ESTALE, with whether it is connected. */
static int
i915_display_other_check(
	struct i915_display *display,
	uint32_t display_id,
	uint64_t generation,
	int *connected)
{
	struct i915_hpd_output output;
	int error;

	/* The connector of the ID, of this generation (the panel connected while the node has it). */
	error = drv_i915_display_output_connector(display, display_id - I915_DISPLAY_OTHER_ID, &output);
	if (error != 0)
		return ENOENT;
	if (generation != output.generation)
		return ESTALE;

	/* Succeeded: the connector is known. */
	*connected = output.connected;
	return 0;
}

/*
 * Moves the resident output to another connected connector (Keiland's
 * explicit claim): only while no lease is held (otherwise the
 * limit of the outputs shown at once, ENOSPC).  The connector's kind
 * prepares its output (output.c), the hold of the last picture, if any,
 * ends and the output that showed it is stopped, and the prepared output
 * becomes the one the next frame lights.  Returns 0, ENOSPC, EBUSY when
 * the hold did not end in time, or the preparation's error (ENXIO,
 * EOPNOTSUPP, ...).
 */
static int
i915_display_move(
	struct i915_device *device,
	unsigned connector)
{
	struct i915_display_output next;
	struct i915_display *display;
	struct i915_resident_display *rd;
	const char *reason;
	unsigned long irq;
	void *owner;
	int error;

	/* A lease held: the limit of the outputs shown at once. */
	display = device->display;
	rd = &display->rd;
	drv_i915_present_lease_init(display);
	mutex_lock(&rd->mutex);

	owner = rd->owner;

	mutex_unlock(&rd->mutex);

	/* Refused for the limit. */
	if (owner != NULL) {
		kern_logf("i915: resident display: claim of connector %u refused: the limit of outputs shown at once (1) is reached\n", connector);
		return ENOSPC;
	}

	/* The connector's own preparation (no hardware is written). */
	reason = "";
	error = drv_i915_display_output_prepare(display, connector, &next, &reason);
	if (error != 0) {
		kern_logf("i915: resident display: connector %u cannot be the output: %s (%d)\n", connector, reason, error);
		return error;
	}

	/* The last picture's hold ends, and the output that showed it stops. */
	error = drv_i915_present_cut(device);
	if (error != 0) {
		kern_logf("i915: resident display: connector %u: the held picture was not stopped in time\n", connector);
		return EBUSY;
	}

	/* The prepared output, under the lock the worker's readers share. */
	irq = spin_lock_irqsave(&device->irq_lock);

	display->output = next;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the next frame lights the connector. */
	kern_logf("i915: resident display: the output moves to connector %u (%s, Keiland's claim)\n", connector, drv_i915_display_output_name(display));
	return 0;
}

/* Tells whether two outputs are different ones (kind, or connector). */
static int
i915_display_outputs_differ(
	const struct i915_display_output *one,
	const struct i915_display_output *other)
{
	/* Another kind. */
	if (one->kind != other->kind || one->none != other->none)
		return 1;

	/* Another connector, when both name one. */
	if (one->has_connector && other->has_connector && one->connector != other->connector)
		return 1;

	/* Succeeded: the same output. */
	return 0;
}
