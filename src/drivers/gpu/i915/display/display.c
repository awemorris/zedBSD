/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display part of the device start and stop.
 *
 * The stages follow the reference's i915_driver_probe(): the end of the
 * hardware probe (OpRegion, DRAM, memory bandwidth), the display noirq
 * probe with the firmware display check before its first display write,
 * the PCH and the display half of the interrupts before the install, the
 * nogem probe (output setup, readout, sanitize), and after the GT the
 * display probe, the hotplug path and the driver registration.  The stop
 * gives back what the start built, in reverse.
 *
 * It also holds the pieces of the probe that are the display's own and
 * belong to no package: the DRM device management and per-pipe vblank
 * state (drm_dev_init(), drm_vblank_init()), the PCH detection
 * (soc/intel_pch.c), intel_display_driver_probe() and the driver
 * registration, and the query, mode and claim operations of the node's
 * display.
 */

#include "internal.h"
#include "clock.h"
#include "diagnostics.h"
#include "display.h"
#include "dmc.h"
#include "dp-sink.h"
#include "hotplug.h"
#include "interrupts.h"
#include "modeset.h"
#include "opregion.h"
#include "power.h"
#include "present.h"
#include "scanout.h"
#include "state.h"
#include "takeover.h"
#include "vbt-parse.h"
#include "watermark.h"
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
#include <hal/hal.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>

/* PCI configuration: the OpRegion base (ASLS), the revision and the subsystem ids. */
#define I915_PCI_ASLS			0xfcU
#define I915_PCI_DEVICE_ID		0x02U
#define I915_PCI_COMMAND_WORD		0x04U
#define I915_PCI_REVISION		0x08U
#define I915_PCI_SUBSYSTEM_VENDOR	0x2cU
#define I915_PCI_SUBSYSTEM_DEVICE	0x2eU

/* The OpRegion header the signature check maps. */
#define I915_OPREGION_HEADER_BYTES	0x2000U
#define I915_OPREGION_SIGNATURE_BYTES	16U
#define I915_OPREGION_SIZE_OFFSET	0x10U
#define I915_OPREGION_VERSION_OFFSET	0x14U

/* The pipes of the platform (A to D) and the ports the output setup walks (A, B, TC1 to TC4). */
#define I915_DISPLAY_PIPE_MASK		0xfU
#define I915_DISPLAY_PORT_MASK		((1U << 0) | (1U << 1) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 6))

/* drm_dev_init(): DRIVER_MODESET | DRIVER_ATOMIC. */
#define I915_DRM_DRIVER_FEATURES	0x3U

/* The FBC instances of XE_LPD: FBC A. */
#define I915_DISPLAY_FBC_MASK		0x1U

/* The DBUF slices and the ABOX mask of XE_LPD (S1 to S4, ABOX 0 to 1) and Tiger Lake (S1 to S2, ABOX 1 to 2). */
#define I915_DISPLAY_DBUF_XELPD		0x0fU
#define I915_DISPLAY_DBUF_TGL		0x03U
#define I915_DISPLAY_ABOX_XELPD		0x03U
#define I915_DISPLAY_ABOX_TGL		0x06U

/* How long the DMC stop waits for its worker (500 ticks). */
#define I915_DISPLAY_DMC_FINI_TICKS	(5U * KERN_CLOCK_HZ)

/* DC_STATE_EN, read for the registration log. */
#define I915_DISPLAY_DC_STATE_EN	0x45504U

/* The one display of the node and its generation. */
#define I915_DISPLAY_ID			1U
#define I915_DISPLAY_GENERATION		1U

/* The largest frame the node's display accepts (64 MiB). */
#define I915_DISPLAY_MAX_FRAME_BYTES	(64ULL << 20)

/* enum port: PORT_A, PORT_D, PORT_TC1 and, on XE_LPD, PORT_D_XELPD (TC5). */
#define I915_PORT_A			0
#define I915_PORT_D			3
#define I915_PORT_TC1			3
#define I915_PORT_D_XELPD		7

/* soc/intel_pch.h: the PCH device id types, already masked with 0xff80. */
#define INTEL_PCH_DEVICE_ID_MASK	0xff80U
#define INTEL_PCH_IBX_DEVICE_ID_TYPE	0x3b00U
#define INTEL_PCH_CPT_DEVICE_ID_TYPE	0x1c00U
#define INTEL_PCH_PPT_DEVICE_ID_TYPE	0x1e00U
#define INTEL_PCH_LPT_DEVICE_ID_TYPE	0x8c00U
#define INTEL_PCH_LPT_LP_DEVICE_ID_TYPE	0x9c00U
#define INTEL_PCH_WPT_DEVICE_ID_TYPE	0x8c80U
#define INTEL_PCH_WPT_LP_DEVICE_ID_TYPE	0x9c80U
#define INTEL_PCH_SPT_DEVICE_ID_TYPE	0xA100U
#define INTEL_PCH_SPT_LP_DEVICE_ID_TYPE	0x9D00U
#define INTEL_PCH_KBP_DEVICE_ID_TYPE	0xA280U
#define INTEL_PCH_CNP_DEVICE_ID_TYPE	0xA300U
#define INTEL_PCH_CNP_LP_DEVICE_ID_TYPE	0x9D80U
#define INTEL_PCH_CMP_DEVICE_ID_TYPE	0x0280U
#define INTEL_PCH_CMP2_DEVICE_ID_TYPE	0x0680U
#define INTEL_PCH_CMP_V_DEVICE_ID_TYPE	0xA380U
#define INTEL_PCH_ICP_DEVICE_ID_TYPE	0x3480U
#define INTEL_PCH_ICP2_DEVICE_ID_TYPE	0x3880U
#define INTEL_PCH_MCC_DEVICE_ID_TYPE	0x4B00U
#define INTEL_PCH_TGP_DEVICE_ID_TYPE	0xA080U
#define INTEL_PCH_TGP2_DEVICE_ID_TYPE	0x4380U
#define INTEL_PCH_JSP_DEVICE_ID_TYPE	0x4D80U
#define INTEL_PCH_ADP_DEVICE_ID_TYPE	0x7A80U
#define INTEL_PCH_ADP2_DEVICE_ID_TYPE	0x5180U
#define INTEL_PCH_ADP3_DEVICE_ID_TYPE	0x7A00U
#define INTEL_PCH_ADP4_DEVICE_ID_TYPE	0x5480U
#define INTEL_PCH_P2X_DEVICE_ID_TYPE	0x7100U
#define INTEL_PCH_P3X_DEVICE_ID_TYPE	0x7000U
#define INTEL_PCH_QEMU_DEVICE_ID_TYPE	0x2900U

/* The vendor and subsystem ids of an emulated QEMU bridge. */
#define I915_PCI_VENDOR_INTEL		0x8086U
#define I915_PCI_SUBVENDOR_QUMRANET	0x1af4U
#define I915_PCI_SUBDEVICE_QEMU		0x1100U

/* PCI class 06h/01h (bridge / ISA), matched on class and subclass. */
#define I915_PCI_CLASS_BRIDGE_ISA	0x060100U
#define I915_PCI_CLASS_BRIDGE_ISA_MASK	0xffff00U

static int i915_display_create_worlds(struct i915_display *display);
static void i915_display_destroy_worlds(struct i915_display *display);
static struct i915_display *i915_display_live(struct i915_device *device);
static void i915_display_opregion_signature(struct i915_device *device, uint32_t asls);
static int i915_display_noirq_front(struct i915_device *device);
static int i915_display_native_check(struct i915_device *device);
static int i915_display_core_init(struct i915_device *device);
static void i915_display_dmc_init(struct i915_device *device);
static int i915_display_noirq_tail(struct i915_device *device);
static int i915_display_nogem_front(struct i915_device *device);
static void i915_display_setup_outputs(struct i915_device *device);
static void i915_display_readout(struct i915_device *device);
static int i915_display_driver_probe(struct i915_display *display, int intel_irqs_enabled);
static int i915_skl_watermark_ipc_init(struct i915_mmio *m, int has_ipc, int platform_can);
static uint32_t i915_display_rmw(struct i915_mmio *m, uint32_t reg, uint32_t clear, uint32_t set);
static void i915_driver_register(struct i915_device *device);
static void i915_driver_unregister(struct i915_device *device);
static unsigned i915_display_wells_on(struct i915_display *display);
static void i915_display_log_wells(struct i915_device *device);
static void i915_drm_vblank_crtc_cleanup(void *arg);
static void i915_virt_detect_pch(int is_alderlake, uint16_t *pch_id, int *pch_type);
static int i915_next_isa_bridge(struct i915_display *display, unsigned index, uint16_t *vendor, uint16_t *device, uint16_t *svid, uint16_t *sdid);
static int i915_display_query(void *device, void *session, struct gpu_display_info *request);
static int i915_display_mode(void *device, void *session, struct gpu_display_mode *request);
static int i915_display_claim(void *device, void *session, struct gpu_display_claim *request);

/*
 * The display operations of the resident node.
 *
 * One output, the eDP panel, with one full-output plane; query, mode and
 * claim are answered here, present, wait and release by the present path,
 * and the event sequence by the hotplug path (no topology change is ever
 * published).  The table never changes.
 */
static const struct drv_gpu_display_ops i915_display_ops = {
	i915_display_query,
	i915_display_mode,
	i915_display_claim,
	drv_i915_present_display_release,
	drv_i915_present_display_present,
	drv_i915_present_display_wait,
	drv_i915_hpd_events
};

/*
 * Allocates the display of a device and its Linux environments.
 *
 * Returns 0, ENOMEM, or EBUSY when an environment is bound to another
 * display already.  Nothing stays allocated on failure.
 */
int
drv_i915_display_create(
	struct i915_device *device)
{
	struct i915_display *display;
	int error;

	/* A device has one display. */
	if (device->display != NULL)
		return EBUSY;

	/* Allocates the zeroed display: the probe's state at load time. */
	display = kern_calloc(1U, sizeof(*display));
	if (display == NULL)
		return ENOMEM;

	display->device = device;

	/* Allocates each Linux environment's state. */
	error = i915_display_create_worlds(display);
	if (error != 0) {
		i915_display_destroy_worlds(display);
		kern_free(display);
		return error;
	}

	device->display = display;

#ifdef I915_TEST_CAPTURE
	/*
	 * The capture build never touches the display hardware: every display
	 * stage is skipped, as after a firmware display check that stopped, and
	 * the node offers the capture display (capture.c) instead of the panel.
	 */
	display->absent = 1;
	kern_logf("i915: capture: capture build: the display hardware is not used (no modeset, no panel); presentations are captured into guest RAM\n");
#endif

	/* Succeeded: the display stages may run. */
	return 0;
}

/*
 * Frees the display of a device and its Linux environments.
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

	/* Frees the environments, then the display. */
	i915_display_destroy_worlds(display);
	device->display = NULL;
	kern_free(display);
}

/*
 * Reads the OpRegion and its VBT, then the DRAM and the memory bandwidth.
 *
 * P2.9 intel_opregion_setup(): ASLS holds the OpRegion base.  The OpRegion
 * is read as data (a read-only copy and its VBT); the runtime protocol is
 * not joined (VBT_ONLY).  P2.10 intel_dram_detect() and P2.11
 * intel_bw_init_hw() are void in the reference: a tolerated PCODE or data
 * failure is logged and the probe continues.
 */
void
drv_i915_display_init_opregion(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_gt *gt;
	uint32_t asls;
	int dram_error;
	int bw_error;
	uint16_t op;

	display = i915_display_live(device);
	if (display == NULL)
		return;

	gt = &device->gt;

	/* ASLS (PCI configuration 0xFC) holds the OpRegion base. */
	asls = drv_i915_pci_read32(&gt->pci, I915_PCI_ASLS);
	kern_logf("i915: P2 opregion: ASLS=0x%08x\n", asls);

	/* Reads the OpRegion as data; a valid VBT in it is handed to the parser. */
	(void)drv_i915_opregion_read_data(display, asls, &display->opd);
	drv_i915_opregion_log(&display->opd);
	display->p2_opd = &display->opd;
	if (display->opd.vbt_valid) {
		drv_i915_bios_set_opregion_vbt(display, display->opd.vbt, display->opd.vbt_size);
		display->opregion_vbt_present = 1;
	}

	/* ASLS 0 is the legitimate absent branch; otherwise the header's signature is checked. */
	display->opregion_present = 0;
	if (asls != 0U)
		display->opregion_present = 1;

	if (asls == 0U) {
		drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_NOTE, "intel_opregion_absent", 0U, 0U);
	} else {
		i915_display_opregion_signature(device, asls);
	}

	/* The DRAM the bandwidth code is given. */
	dram_error = drv_i915_dram_detect(&gt->sb_lock, &gt->mmio, &display->dram_info);
	op = I915_TRACE_NOTE;
	if (dram_error == 0)
		op = I915_TRACE_ACQUIRE;
	drv_i915_trace_record(&gt->trace, 0U, op, "intel_dram_detect", (uint64_t)(unsigned)display->dram_info.type, (uint64_t)display->dram_info.num_channels);

	/* The display bandwidth table read from PCODE. */
	bw_error = drv_i915_bw_init_hw(&gt->sb_lock, &gt->mmio, &display->dram_info, &display->bw_state);
	op = I915_TRACE_NOTE;
	if (bw_error == 0)
		op = I915_TRACE_ACQUIRE;
	drv_i915_trace_record(&gt->trace, 0U, op, "intel_bw_init_hw", (uint64_t)(unsigned)display->bw_state.sagv_status, 0U);

	kern_logf("i915: P2 hw_probe tail: dram_detect rc=%d bw_init rc=%d sagv=%d\n",
	    dram_error,
	    bw_error,
	    display->bw_state.sagv_status);
}

/*
 * Runs the display noirq probe (intel_display_driver_probe_noirq()).
 *
 * drm_vblank_init, intel_bios_init, intel_vga_register,
 * intel_power_domains_init, intel_pmdemand_init_early, then the firmware
 * display check before the first display write, then
 * intel_power_domains_init_hw, intel_dmc_init, the modeset and flip
 * workqueues, and the software state (mode config, CDCLK, colour, DBUF,
 * bandwidth, PM demand, quirks, FBC).  A firmware display check that stops
 * leaves the device without a display and returns 0.  Returns 0, or the
 * error of the stage named in device->stage.
 */
int
drv_i915_display_init_noirq(
	struct i915_device *device)
{
	struct i915_display *display;
	int error;

	display = i915_display_live(device);
	if (display == NULL)
		return 0;

	/* The DRM device, the VBT, the VGA client, the power domains and the PM demand lock. */
	error = i915_display_noirq_front(device);
	if (error != 0)
		return error;

	/* The firmware display check: a STOP leaves the display untouched and unused. */
	error = i915_display_native_check(device);
	if (error != 0)
		return error;
	if (display->absent)
		return 0;

	/* The display core on the real device. */
	error = i915_display_core_init(device);
	if (error != 0)
		return error;

	/* The DMC firmware load, queued without waiting. */
	i915_display_dmc_init(device);

	/* The modeset and flip workqueues, created before the mode config as the reference does. */
	display->modeset_wq_ok = 0;
	error = drv_i915_workqueue_create(&display->modeset_wq, "i915-modeset");
	if (error == 0)
		display->modeset_wq_ok = 1;

	display->flip_wq_ok = 0;
	error = drv_i915_workqueue_create(&display->flip_wq, "i915-flip");
	if (error == 0)
		display->flip_wq_ok = 1;

	kern_logf("i915: P3 modeset/flip workqueues: modeset=%d flip=%d\n", display->modeset_wq_ok, display->flip_wq_ok);

	/* The software state: mode config, CDCLK, colour, DBUF, bandwidth, PM demand, quirks and FBC. */
	error = i915_display_noirq_tail(device);
	if (error != 0)
		return error;

	/* Succeeded: intel_display_driver_probe_noirq() returns 0. */
	return 0;
}

/*
 * Detects the PCH and binds the display interrupt half in place of the
 * interim hooks.
 *
 * gen11_display_irq_reset() resets the south display block and
 * gen8_de_irq_postinstall() programs it only for an ICP or later PCH, so
 * the PCH type is needed before the install.
 */
void
drv_i915_display_irq_prepare(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_pch_state *pch;

	display = i915_display_live(device);
	if (display == NULL)
		return;

	pch = &display->pch;

	/* Finds the PCH: display version, Alder Lake-P, a display, running as a guest. */
	kern_memset(pch, 0, sizeof(*pch));
	drv_i915_detect_pch(display, pch, (int)device->gt.display_ver, 1, 1, 1);
	kern_logf("i915: P4 intel_detect_pch: type=%d id=0x%04x source=%d bridges=%u bridge_dev=0x%04x subsys=%04x:%04x\n",
	    pch->type,
	    (unsigned)pch->id,
	    pch->source,
	    pch->bridges_scanned,
	    (unsigned)pch->bridge_device,
	    (unsigned)pch->bridge_svid,
	    (unsigned)pch->bridge_sdid);

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

	kern_logf("i915: P4 de masks: pipe_masked=0x%x pipe_enables=0x%x port_masked=0x%x misc_masked=0x%x de_irq_mask[A]=0x%x master_enabled=%d\n",
	    display->irq.de_pipe_masked,
	    display->irq.de_pipe_enables,
	    display->irq.de_port_masked,
	    display->irq.de_misc_masked,
	    display->irq.de_irq_mask[0],
	    device->gt.irq.reached_master_enable);
}

/*
 * Runs the survey and the display nogem probe (intel_display_driver_probe_nogem()).
 *
 * The survey records what the firmware left (diagnostics only).  P5-a is
 * the front section, P5-b intel_setup_outputs(), P5-c and P5-d the readout
 * and the sanitize of intel_modeset_setup_hw_state().  Returns 0, or the
 * front section's error with the failing step in device->stage.
 */
int
drv_i915_display_init_nogem(
	struct i915_device *device)
{
	struct i915_display *display;
	int error;

	display = i915_display_live(device);
	if (display == NULL)
		return 0;

	/* What the firmware left on (no state change). */
	drv_i915_display_survey(display);

	/* The front section: watermark latencies, crtcs, planes, DPLLs, CDCLK, the WAs, the VGA plane. */
	error = i915_display_nogem_front(device);
	if (error != 0)
		return error;

	/* The outputs, then the readout and the sanitize of the hardware state. */
	i915_display_setup_outputs(device);
	i915_display_readout(device);

	/* Succeeded: intel_display_driver_probe_nogem() returns 0. */
	return 0;
}

/*
 * Runs the display probe and the driver registration (after the GT).
 *
 * intel_display_driver_probe() (the initial commit, the hotplug init, IPC),
 * the hotplug path, intel_opregion_register() (VBT_ONLY: the runtime is not
 * joined) and i915_driver_register().  An active crtc the firmware left is
 * the firmware's display; its atomic takeover is not ported, so the start
 * stops instead of leaving it untouched and unaccounted for.  Returns 0, or
 * ENOTSUP with device->stage naming the initial commit.
 */
int
drv_i915_display_register(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_driver_probe *dprobe;
	const char *hotplug;
	const char *unimplemented;
	int error;
	int started;

	display = i915_display_live(device);
	if (display == NULL)
		return 0;

	dprobe = &display->dprobe;

	/* The DMC payload gates the DC_off well's disable (DC6 entry) below. */
	display->pwc.dmc_has_payload = 0;
	if (display->dmc_inited)
		display->pwc.dmc_has_payload = drv_i915_dmc_has_payload(&display->dmc_dev.dmc);

	/* intel_display_driver_probe(). */
	error = i915_display_driver_probe(display, device->gt.irq.irqs_enabled);
	unimplemented = "";
	if (dprobe->initial_commit_unimplemented)
		unimplemented = " (DRM atomic commit unimplemented)";

	kern_logf("i915: P7 display_driver_probe: rc=%d active_crtcs=%u initial_commit=%d%s overlay=%d fbdev=%d ipc_enabled=%d\n",
	    error,
	    dprobe->active_crtcs,
	    dprobe->initial_commit_rc,
	    unimplemented,
	    dprobe->overlay,
	    dprobe->fbdev,
	    dprobe->ipc_enabled);
	kern_logf("i915: P7 hpd_init: encoders=%u pins=%d,%d setups=%u skipped=%d | de: enabled=0x%x hotplug=0x%x IMR=0x%08x TC_CTL=0x%08x TBT_CTL=0x%08x | pch: enabled=0x%x hotplug=0x%x SHPD_FILTER=0x%x SDEIMR=0x%08x%s SHOTPLUG_DDI=0x%08x SHOTPLUG_TC=0x%08x | poll: works=%u core_gets=%u\n",
	    dprobe->hp.n_encoders,
	    dprobe->hp.encoder_pin[0],
	    dprobe->hp.encoder_pin[1],
	    dprobe->hp.irq_setups,
	    dprobe->hp.irq_setup_skipped,
	    dprobe->hp.de_enabled_irqs,
	    dprobe->hp.de_hotplug_irqs,
	    dprobe->hp.de_hpd_imr,
	    dprobe->hp.tc_ctl,
	    dprobe->hp.tbt_ctl,
	    dprobe->hp.pch_enabled_irqs,
	    dprobe->hp.pch_hotplug_irqs,
	    dprobe->hp.shpd_filter,
	    dprobe->hp.sdeimr,
	    dprobe->hp.sdeimr_skipped ? "(not written: irqs off)" : "",
	    dprobe->hp.shotplug_ddi,
	    dprobe->hp.shotplug_tc,
	    dprobe->hp.poll_init_works,
	    dprobe->hp.poll_core_gets);

	/*
	 * The hotplug path: intel_hpd_init_early() and the connectors of the
	 * encoders the output setup made; from here on the display interrupt
	 * handler hands SDEIIR to icp_irq_handler().  Adaptation: the reference
	 * makes the connectors in intel_ddi_init(); they are made here, once
	 * the hotplug registers are programmed.
	 */
	if (error == 0) {
		started = drv_i915_hpd_start(display, &dprobe->hp, &device->gt.mmio, &display->nogem, &display->power_domains, &display->pwc, display->pch.type, device->gt.irq.irqs_enabled, NULL);
		if (started == 0)
			display->hpd_started = 1;
	}

	hotplug = "NOT started";
	if (display->hpd_started)
		hotplug = "started";
	kern_logf("i915: P7 hotplug path: %s\n", hotplug);

	/*
	 * An active crtc at probe time is the firmware display.  The reference
	 * only logs a failed initial modeset and continues; without the
	 * takeover this start stops here, because it would otherwise leave that
	 * display untouched and unaccounted for.
	 */
	if (dprobe->initial_commit_unimplemented) {
		drv_i915_trace_record(&device->gt.trace, 0U, I915_TRACE_UNIMPLEMENTED, "intel_initial_commit", dprobe->active_crtcs, 0U);
		device->stage = "intel_initial_commit";
		return ENOTSUP;
	}

	/*
	 * i915_driver_register() -> intel_opregion_register(): VBT_ONLY -- no
	 * ACPI notifier is registered, drdy, ardy, csts, DIDL and CADL are not
	 * written and there is no ASLE service, as in the reference built
	 * without ACPI.  Recorded; the probe continues.
	 */
	if (display->opregion_present) {
		drv_i915_trace_record(&device->gt.trace, 0U, I915_TRACE_NOTE, "intel_opregion_register:runtime_disabled(vbt_only)", 0U, 0U);
		kern_logf("i915: P7 intel_opregion_register: runtime DISABLED (VBT_ONLY, ACPI_RUNTIME_UNAVAILABLE): no notifier registered, no mailbox written, no ASLE service -- the probe continues\n");
	}

	/* i915_driver_register(). */
	i915_driver_register(device);
	kern_logf("i915: P7 driver_register: na_registrations=%u opregion=%d kms_poll=%d | power_domains_enable: wells_on %u -> %u dc_state=0x%x verify_mismatches=%u | runtime_pm_enable: probe usage=%d active=%d (autosuspend not armed: intel_runtime_suspend unimplemented)\n",
	    dprobe->na_registrations,
	    dprobe->opregion_registered,
	    dprobe->hp.kms_poll_inited,
	    dprobe->wells_on_before,
	    dprobe->wells_on_after,
	    (unsigned)dprobe->dc_state_after,
	    dprobe->verify_mismatches,
	    dprobe->rpm_usage_after,
	    drv_i915_rpm_active(&device->gt.probe_pm));

	/* The wells that are held or on, and the DC state. */
	i915_display_log_wells(device);

	/* Succeeded: the display is registered. */
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
	rlcd->pd = &display->power_domains;
	rlcd->pwc = &display->pwc;
	rlcd->dcore = &display->dcore;
	rlcd->cdclk = &display->cdclk;
	rlcd->nogem = &display->nogem;
	rlcd->dstate = &display->dstate;
	rlcd->bw = &display->bw_state;
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

	/* What the serving thread needs, with the panel. */
	rctx->device = device;
	rctx->mmio = &gt->mmio;
	rctx->uncore_lock = &gt->uncore_lock;
	rctx->gm = &gt->mem;
	rctx->es = &gt->engines;
	rctx->lcd = rlcd;
}

/*
 * Stops the display's use of the hardware before the GT stop.
 *
 * Nothing may still be scanned out: the driver's own stop path is the
 * reference's, and this is the last resort (a pipe left running keeps
 * reading memory after the driver is gone).  Then the hotplug path
 * (its interrupt entry closed, its works cancelled) and
 * i915_driver_unregister().
 */
void
drv_i915_display_stop_early(
	struct i915_device *device)
{
	struct i915_display *display;
	unsigned forced;
	unsigned wells_on;

	display = device->display;
	if (display == NULL)
		return;

	/* Stops anything still scanned out. */
	forced = drv_i915_lcd_last_resort_stop(&device->gt.mmio);
	if (forced != 0U)
		kern_logf("i915: stop: LAST-RESORT stopped %u display element(s) the driver had not released\n", forced);

	/* The hotplug path first: its interrupt entry closed, the works cancelled (intel_hpd_cancel_work()). */
	if (display->hpd_started) {
		drv_i915_hpd_stop(display);
		display->hpd_started = 0;
	}

	/*
	 * i915_driver_remove() -> i915_driver_unregister(): runtime PM back to
	 * the core, the INIT power reference taken again (DC states off, wells
	 * on), display unregister.
	 */
	if (display->dprobe.registered) {
		i915_driver_unregister(device);
		wells_on = i915_display_wells_on(display);
		kern_logf("i915: stop: driver_unregister (rpm usage=%d, INIT reference re-taken: wells_on=%u DC_STATE_EN=0x%08x)\n",
		    drv_i915_rpm_usage(&device->gt.probe_pm),
		    wells_on,
		    drv_i915_read32(&device->gt.mmio, I915_DISPLAY_DC_STATE_EN));
	}
}

/*
 * Releases the panel connector and the display records (after the GT
 * stop, before the interrupt uninstall).
 *
 * The encoder destroy runs intel_pps_vdd_off_sync() and the power layer's
 * flush before the records go.
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
	if (!display->nogem_inited)
		return;

	drv_i915_edp_device_fini(&display->edp_dev);
	drv_i915_display_nogem_fini(&display->nogem);
	display->nogem_inited = 0;
	kern_logf("i915: stop: display-nogem fini (crtc/plane/dpll records released)\n");
}

/*
 * Takes the rest of the display apart in reverse order (after the
 * interrupt uninstall).
 *
 * The software state, the DMC (its worker synced, not cancelled, its
 * reference processed, its payload freed) and then the workqueues, the
 * display core, the power domains, the VBT parser, the VGA client and the
 * DRM device.
 */
void
drv_i915_display_fini(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_dmc_dev *dmc;

	display = device->display;
	if (display == NULL)
		return;

	dmc = &display->dmc_dev;

	/* The global objects and the FBC instances. */
	if (display->dstate_inited) {
		drv_i915_display_state_fini(&display->dstate);
		display->dstate_inited = 0;
		kern_logf("i915: stop: display-state fini (global objs released, fbc freed)\n");
	}

	/* The DMC first, then the workqueues. */
	if (display->dmc_inited) {
		drv_i915_dmc_fini(dmc, sched_ticks() + I915_DISPLAY_DMC_FINI_TICKS);
		kern_logf("i915: stop: intel_dmc_fini (worker synced; load_seq_completed=%d, payload_writes=%u, dmc_ref_held=%d)\n",
		    dmc->load_seq_completed_flag,
		    dmc->dmc.payload_writes,
		    dmc->dmc_wakeref_held);
		kern_logf("i915: stop: DMC ver=%u.%u ids MAIN/A/B/C/D present=%d%d%d%d%d payload=%u/%u/%u/%u/%u\n",
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

	if (display->modeset_wq_ok) {
		drv_i915_workqueue_destroy(&display->modeset_wq);
		display->modeset_wq_ok = 0;
	}

	if (display->flip_wq_ok) {
		drv_i915_workqueue_destroy(&display->flip_wq);
		display->flip_wq_ok = 0;
	}

	/* The display core: the init runtime-PM wakeref is cancelled, the wells are kept. */
	if (display->display_core_inited) {
		drv_i915_power_domains_driver_remove(&display->dcore);
		display->display_core_inited = 0;
		kern_logf("i915: stop: display-core driver_remove (init rpm wakeref cancelled, wells kept)\n");
	}

	/* The power-domain map. */
	if (display->power_domains_inited) {
		drv_i915_power_domains_cleanup(&display->power_domains);
		display->power_domains_inited = 0;
		kern_logf("i915: stop: power domains map released\n");
	}

	/* intel_bios_driver_remove(): the VBT parser's lists and arena. */
	drv_i915_bios_driver_remove(&display->vbt_state);

	/* The VGA arbiter client. */
	if (display->vga_registered) {
		drv_i915_vga_unregister(&display->vga_client);
		display->vga_registered = 0;
		kern_logf("i915: stop: VGA arbiter client unregistered\n");
	}

	/* The DRM device and its vblank state. */
	if (display->drm_inited) {
		drv_i915_drm_dev_fini(&display->drm_dev);
		display->drm_inited = 0;
		kern_logf("i915: stop: DRM device + vblank released\n");
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
	abandoned = drv_i915_lcd_kernel_abandoned(device->display);

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
	retained = drv_i915_lcd_kernel_gpu_retained(device->display);

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

	/* Reports the power-well context's flag. */
	return device->display->pwc.irq_sync_failed;
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

	/* A node without a panel offers no display. */
	display = i915_display_live(device);
	if (display == NULL)
		return;
	if (display->rctx.lcd == NULL)
		return;

	/* The display and the display-only pairing, with their capabilities. */
	ops->display = &i915_display_ops;
	ops->scanout = &drv_i915_scanout_ops;
	ops->capabilities |= GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS;
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
#ifdef I915_TEST_CAPTURE
	/* The capture display owns the lease in the capture build. */
	drv_i915_capture_session_close(device, session);
	return;
#endif

	/* The present path owns the lease. */
	drv_i915_present_lease_close(device, session);
}

/*
 * Reads the panel's mode (the resident eDP's LCD state): 0, or ENXIO when
 * the node has no panel.
 */
int
drv_i915_display_panel(
	struct i915_display *display,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_millihz)
{
	const struct i915_lcd_kernel_deps *deps;
	int error;

	/* The node has a panel only once the resident dependencies exist. */
	deps = display->rctx.lcd;
	if (deps == NULL)
		return ENXIO;

	/* The mode, derived from the panel's own timing. */
	error = drv_i915_display_panel_mode(deps, width, height, refresh_millihz);
	if (error != 0)
		return ENXIO;

	/* Succeeded: the mode is known. */
	return 0;
}

/*
 * Brings up the DRM device management state (drm_dev_init()).
 *
 * Returns 0, or EINVAL without storage.
 */
int
drv_i915_drm_dev_init(
	struct i915_drm_device *ddev,
	struct i915_device *parent,
	uint32_t driver_features)
{
	/* Refuses a call without storage. */
	if (ddev == NULL)
		return EINVAL;

	/* The parent, the feature flags and no opener. */
	ddev->parent = parent;
	ddev->driver_features = driver_features;
	ddev->open_count = 0;

	/* The managed-resource list and the event lock. */
	spin_init(&ddev->managed_lock, LOCK_RANK_DEVICE, "i915-drm-managed");
	ddev->drmm_count = 0U;
	spin_init(&ddev->event_lock, LOCK_RANK_DEVICE, "i915-drm-event");

	/* Not registered to userspace here. */
	ddev->minor_registered = 0;

	/* No vblank state yet. */
	ddev->num_crtcs = 0U;
	ddev->vblank_disable_immediate = 0;
	ddev->vblank_inited = 0;
	ddev->inited = 1;

	/* Succeeded: the device may register managed actions. */
	return 0;
}

/*
 * Registers a managed cleanup action (drmm_add_action_or_reset()).
 *
 * A registration that cannot be recorded runs the action now, the
 * reference's contract.  Returns 0, or ENOMEM when the list is full, or
 * EINVAL without a device or an action.
 */
int
drv_i915_drmm_add_action_or_reset(
	struct i915_drm_device *ddev,
	void (*fn)(void *arg),
	void *arg)
{
	/* Refuses a call without a device or an action. */
	if (ddev == NULL || fn == NULL)
		return EINVAL;

	/* A full list runs the action now. */
	if (ddev->drmm_count >= I915_DRMM_MAX) {
		fn(arg);
		return ENOMEM;
	}

	/* Records the action for the teardown. */
	ddev->drmm[ddev->drmm_count].fn = fn;
	ddev->drmm[ddev->drmm_count].arg = arg;
	ddev->drmm_count++;

	/* Succeeded: the action runs at teardown. */
	return 0;
}

/*
 * Takes the DRM device management state apart.
 *
 * The managed actions run in reverse registration order.
 */
void
drv_i915_drm_dev_fini(
	struct i915_drm_device *ddev)
{
	/* A device that was never brought up has nothing to take apart. */
	if (ddev == NULL || !ddev->inited)
		return;

	/* Runs the managed actions, newest first. */
	while (ddev->drmm_count > 0U) {
		ddev->drmm_count--;
		ddev->drmm[ddev->drmm_count].fn(ddev->drmm[ddev->drmm_count].arg);
	}

	/* No vblank state and no device any more. */
	ddev->num_crtcs = 0U;
	ddev->vblank_inited = 0;
	ddev->inited = 0;
}

/*
 * Brings up the per-crtc vblank state (drm_vblank_init()).
 *
 * Each crtc gets its lock, wait queue, index, seqlock, disable timer and
 * its own vblank worker (drm_vblank_worker_init() runs per crtc).  The
 * cleanup of a crtc is registered before its worker is created, so a
 * full managed list and a worker that will not start are both recovered
 * by the teardown without leaking.  Returns 0, EINVAL, or the registration's
 * or the worker's error.
 */
int
drv_i915_drm_vblank_init(
	struct i915_drm_device *ddev,
	unsigned num_crtcs)
{
	struct i915_drm_vblank_crtc *vc;
	unsigned i;
	int error;

	/* Refuses a device that is not up and a count the pipe array cannot hold. */
	if (ddev == NULL || !ddev->inited)
		return EINVAL;
	if (num_crtcs == 0U || num_crtcs > I915_DRM_MAX_PIPES)
		return EINVAL;

	/* Starts with no crtc initialised. */
	ddev->vblank_disable_immediate = 0;
	ddev->num_crtcs = 0U;
	ddev->vblank_inited = 0;

	/* Initialises every crtc in turn. */
	for (i = 0U; i < num_crtcs; i++) {
		vc = &ddev->vblank[i];

		/* The crtc's lock, wait queue and index. */
		spin_init(&vc->lock, LOCK_RANK_DEVICE, "i915-vblank-crtc");
		waitq_init(&vc->queue, "i915-vblank-crtc");
		vc->pipe = i;
		vc->count = 0U;
		vc->worker_created = 0;

		/* The disable timer, armed but idle, and the seqlock. */
		vc->disable_timer_inited = 1;
		vc->disable_deadline = 0U;
		vc->seqlock = 0U;
		vc->inited = 1;

		/* A full list already ran this crtc's cleanup; earlier crtcs are reclaimed by the teardown. */
		error = drv_i915_drmm_add_action_or_reset(ddev, i915_drm_vblank_crtc_cleanup, vc);
		if (error != 0)
			return error;

		/* drm_vblank_worker_init(): one worker per crtc; the registered cleanup tolerates a missing one. */
		error = drv_i915_workqueue_create(&vc->worker, "i915-vblank-crtc");
		if (error != 0) {
			ddev->num_crtcs = i;
			return error;
		}

		vc->worker_created = 1;
		ddev->num_crtcs = i + 1U;
	}

	/* Every crtc has its vblank state. */
	ddev->vblank_inited = 1;
	kern_logf("i915: P3 drm_vblank_init: vblank_slots=%u (per-pipe worker+timer+seqlock)\n", num_crtcs);

	/* Succeeded: the vblank state is complete. */
	return 0;
}

/*
 * Returns the PCH type of a masked PCH device id (intel_pch_type()), and
 * names the PCH found.
 */
int
drv_i915_pch_type(
	uint16_t id)
{
	/* Names the PCH of each id type. */
	switch (id) {
	case INTEL_PCH_IBX_DEVICE_ID_TYPE:
		kern_logf("i915: Found Ibex Peak PCH\n");
		return I915_PCH_IBX;
	case INTEL_PCH_CPT_DEVICE_ID_TYPE:
		kern_logf("i915: Found CougarPoint PCH\n");
		return I915_PCH_CPT;
	case INTEL_PCH_PPT_DEVICE_ID_TYPE:
		/* PPT is CPT compatible. */
		kern_logf("i915: Found PantherPoint PCH\n");
		return I915_PCH_CPT;
	case INTEL_PCH_LPT_DEVICE_ID_TYPE:
		kern_logf("i915: Found LynxPoint PCH\n");
		return I915_PCH_LPT;
	case INTEL_PCH_LPT_LP_DEVICE_ID_TYPE:
		kern_logf("i915: Found LynxPoint LP PCH\n");
		return I915_PCH_LPT;
	case INTEL_PCH_WPT_DEVICE_ID_TYPE:
		/* WPT is LPT compatible. */
		kern_logf("i915: Found WildcatPoint PCH\n");
		return I915_PCH_LPT;
	case INTEL_PCH_WPT_LP_DEVICE_ID_TYPE:
		/* WPT is LPT compatible. */
		kern_logf("i915: Found WildcatPoint LP PCH\n");
		return I915_PCH_LPT;
	case INTEL_PCH_SPT_DEVICE_ID_TYPE:
		kern_logf("i915: Found SunrisePoint PCH\n");
		return I915_PCH_SPT;
	case INTEL_PCH_SPT_LP_DEVICE_ID_TYPE:
		kern_logf("i915: Found SunrisePoint LP PCH\n");
		return I915_PCH_SPT;
	case INTEL_PCH_KBP_DEVICE_ID_TYPE:
		kern_logf("i915: Found Kaby Lake PCH (KBP)\n");
		return I915_PCH_SPT;
	case INTEL_PCH_CNP_DEVICE_ID_TYPE:
		kern_logf("i915: Found Cannon Lake PCH (CNP)\n");
		return I915_PCH_CNP;
	case INTEL_PCH_CNP_LP_DEVICE_ID_TYPE:
		kern_logf("i915: Found Cannon Lake LP PCH (CNP-LP)\n");
		return I915_PCH_CNP;
	case INTEL_PCH_CMP_DEVICE_ID_TYPE:
	case INTEL_PCH_CMP2_DEVICE_ID_TYPE:
		/* CMP is CNP compatible. */
		kern_logf("i915: Found Comet Lake PCH (CMP)\n");
		return I915_PCH_CNP;
	case INTEL_PCH_CMP_V_DEVICE_ID_TYPE:
		/* CMP-V is SPT compatible. */
		kern_logf("i915: Found Comet Lake V PCH (CMP-V)\n");
		return I915_PCH_SPT;
	case INTEL_PCH_ICP_DEVICE_ID_TYPE:
	case INTEL_PCH_ICP2_DEVICE_ID_TYPE:
		kern_logf("i915: Found Ice Lake PCH\n");
		return I915_PCH_ICP;
	case INTEL_PCH_MCC_DEVICE_ID_TYPE:
		/* MCC is TGP compatible. */
		kern_logf("i915: Found Mule Creek Canyon PCH\n");
		return I915_PCH_TGP;
	case INTEL_PCH_TGP_DEVICE_ID_TYPE:
	case INTEL_PCH_TGP2_DEVICE_ID_TYPE:
		kern_logf("i915: Found Tiger Lake LP PCH\n");
		return I915_PCH_TGP;
	case INTEL_PCH_JSP_DEVICE_ID_TYPE:
		/* JSP is ICP compatible. */
		kern_logf("i915: Found Jasper Lake PCH\n");
		return I915_PCH_ICP;
	case INTEL_PCH_ADP_DEVICE_ID_TYPE:
	case INTEL_PCH_ADP2_DEVICE_ID_TYPE:
	case INTEL_PCH_ADP3_DEVICE_ID_TYPE:
	case INTEL_PCH_ADP4_DEVICE_ID_TYPE:
		kern_logf("i915: Found Alder Lake PCH\n");
		return I915_PCH_ADP;
	default:
		return I915_PCH_NONE;
	}
}

/*
 * Reports whether a PCH id and subsystem name an emulated bridge
 * (intel_is_virt_pch()).
 */
int
drv_i915_is_virt_pch(
	uint16_t id,
	uint16_t svendor,
	uint16_t sdevice)
{
	/* The P2X and P3X ids of virtualised platforms. */
	if (id == INTEL_PCH_P2X_DEVICE_ID_TYPE)
		return 1;
	if (id == INTEL_PCH_P3X_DEVICE_ID_TYPE)
		return 1;

	/* QEMU's Q35 bridge with the QEMU subsystem. */
	if (id == INTEL_PCH_QEMU_DEVICE_ID_TYPE) {
		if (svendor == I915_PCI_SUBVENDOR_QUMRANET && sdevice == I915_PCI_SUBDEVICE_QEMU)
			return 1;
	}

	/* A real bridge. */
	return 0;
}

/*
 * Detects the PCH the display pairs with (intel_detect_pch()).
 *
 * The ISA bridges are scanned, not Dev31:Fun0, so that passthrough works:
 * a VMM only has to expose an ISA bridge.  All of them are scanned and the
 * first match is taken; an emulated one makes the platform guess the PCH,
 * and so does a guest with no bridge at all.
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
	uint16_t id;
	unsigned i;
	int pch_type;
	int found_bridge;
	int more;
	int virt;

	/* Starts with no PCH. */
	p->type = I915_PCH_NONE;
	p->id = 0U;
	p->source = I915_PCH_SRC_NONE;
	p->bridges_scanned = 0U;
	p->bridge_device = 0U;
	p->bridge_svid = 0U;
	p->bridge_sdid = 0U;

	/* A south display engine on the same PCI device is a fake PCH (display version 20 on). */
	if (display_ver >= 20) {
		p->type = I915_PCH_LNL;
		return;
	}

	/* Scans every ISA bridge, taking the first Intel one whose id is known. */
	found_bridge = 0;
	vendor = 0U;
	device = 0U;
	svid = 0U;
	sdid = 0U;
	for (i = 0U;; i++) {
		more = i915_next_isa_bridge(display, i, &vendor, &device, &svid, &sdid);
		if (!more)
			break;

		p->bridges_scanned++;
		found_bridge = 1;

		/* Only an Intel bridge names a PCH. */
		if (vendor != I915_PCI_VENDOR_INTEL)
			continue;

		/* A real PCH id, or an emulated bridge that makes the platform guess. */
		id = (uint16_t)(device & INTEL_PCH_DEVICE_ID_MASK);
		pch_type = drv_i915_pch_type(id);
		if (pch_type != I915_PCH_NONE) {
			p->type = pch_type;
			p->id = id;
			p->source = I915_PCH_SRC_REAL;
			p->bridge_device = device;
			p->bridge_svid = svid;
			p->bridge_sdid = sdid;
			break;
		}

		virt = drv_i915_is_virt_pch(id, svid, sdid);
		if (virt) {
			i915_virt_detect_pch(is_alderlake, &p->id, &p->type);
			p->source = I915_PCH_SRC_VIRT;
			p->bridge_device = device;
			p->bridge_svid = svid;
			p->bridge_sdid = sdid;
			break;
		}
	}

	/* A PCH without a south display is NOP; a guest without a bridge guesses. */
	if (found_bridge && !has_display) {
		kern_logf("i915: Display disabled, reverting to NOP PCH\n");
		p->type = I915_PCH_NOP;
		p->id = 0U;
	} else if (!found_bridge) {
		if (run_as_guest && has_display) {
			i915_virt_detect_pch(is_alderlake, &p->id, &p->type);
			p->source = I915_PCH_SRC_NO_BRIDGE;
		} else {
			kern_logf("i915: No PCH found.\n");
		}
	}
}

/* Allocates every Linux environment's state, stopping at the first failure. */
static int
i915_display_create_worlds(
	struct i915_display *display)
{
	int error;

	/* The modeset environment. */
	error = drv_i915_lcd_world_create(display);
	if (error != 0)
		return error;

	/* The watermark environment. */
	error = drv_i915_wm_world_create(display);
	if (error != 0)
		return error;

	/* The takeover environment. */
	error = drv_i915_takeover_world_create(display);
	if (error != 0)
		return error;

	/* The DP environment. */
	error = drv_i915_dp_world_create(display);
	if (error != 0)
		return error;

	/* The VBT environment. */
	error = drv_i915_vbt_world_create(display);
	if (error != 0)
		return error;

	/* The hotplug environment. */
	error = drv_i915_hpd_world_create(display);
	if (error != 0)
		return error;

	/* The OpRegion environment. */
	error = drv_i915_opregion_world_create(display);
	if (error != 0)
		return error;

	/* Succeeded: every environment has its state. */
	return 0;
}

/* Frees every Linux environment's state that exists, in reverse. */
static void
i915_display_destroy_worlds(
	struct i915_display *display)
{
	/* Each destroy leaves an absent world alone. */
	drv_i915_opregion_world_destroy(display);
	drv_i915_hpd_world_destroy(display);
	drv_i915_vbt_world_destroy(display);
	drv_i915_dp_world_destroy(display);
	drv_i915_takeover_world_destroy(display);
	drv_i915_wm_world_destroy(display);
	drv_i915_lcd_world_destroy(display);
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

/* Maps the OpRegion header and checks its "IntelGraphicsMem" signature (diagnostic; unmapped again). */
static void
i915_display_opregion_signature(
	struct i915_device *device,
	uint32_t asls)
{
	static const char want[I915_OPREGION_SIGNATURE_BYTES] = {
		'I', 'n', 't', 'e', 'l', 'G', 'r', 'a', 'p', 'h', 'i', 'c', 's', 'M', 'e', 'm'
	};
	const volatile uint8_t *header;
	char sign[I915_OPREGION_SIGNATURE_BYTES + 1U];
	uint32_t size_kib;
	uint32_t version;
	unsigned i;
	void *op;
	int status;
	int ok;

	/* Maps the 8 KiB header read-only. */
	op = NULL;
	status = hal_space_map_device((hal_physaddr_t)asls, I915_OPREGION_HEADER_BYTES, HAL_SPACE_READ, &op);
	if (status != HAL_OK || op == NULL) {
		kern_logf("i915: P2 opregion: map(ASLS=0x%08x,8KiB) rc=%d (not device-mappable)\n", asls, status);
		drv_i915_trace_record(&device->gt.trace, 0U, I915_TRACE_NOTE, "intel_opregion_map_unavailable", (uint64_t)asls, (uint64_t)(unsigned)status);
		return;
	}

	/* Reads the signature, the size and the version. */
	header = (const volatile uint8_t *)op;
	for (i = 0U; i < I915_OPREGION_SIGNATURE_BYTES; i++)
		sign[i] = (char)header[i];

	sign[I915_OPREGION_SIGNATURE_BYTES] = '\0';
	size_kib = *(const volatile uint32_t *)(const volatile void *)(header + I915_OPREGION_SIZE_OFFSET);
	version = *(const volatile uint32_t *)(const volatile void *)(header + I915_OPREGION_VERSION_OFFSET);
	kern_logf("i915: P2 opregion: sign='%s' size=%uKiB version=0x%08x\n", sign, size_kib, version);

	/* Compares the signature byte by byte. */
	ok = 1;
	for (i = 0U; i < I915_OPREGION_SIGNATURE_BYTES; i++) {
		if (sign[i] != want[i]) {
			ok = 0;
			break;
		}
	}

	/* Records a valid OpRegion, or the mismatch. */
	if (ok) {
		drv_i915_trace_record(&device->gt.trace, 0U, I915_TRACE_ACQUIRE, "intel_opregion_setup", (uint64_t)asls, (uint64_t)version);
	} else {
		kern_logf("i915: P2 opregion: signature mismatch\n");
		drv_i915_trace_record(&device->gt.trace, 0U, I915_TRACE_NOTE, "intel_opregion_bad_signature", (uint64_t)asls, 0U);
	}

	(void)hal_space_unmap_device(op, I915_OPREGION_HEADER_BYTES);
}

/*
 * The front of the noirq probe: the DRM device and its vblank state, the
 * VBT, the VGA client, the power-domain map and the PM demand lock.
 */
static int
i915_display_noirq_front(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_gt *gt;
	unsigned num_pipes;
	unsigned pipe_mask;
	int error;

	display = device->display;
	gt = &device->gt;

	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_NOTE, "display_driver_probe_noirq:enter", 0U, 0U);
	kern_logf("i915: P3 display_driver_probe_noirq enter (device state carried from P2, msi_kept=%u)\n", gt->msi_kept);

	/* i915_inject_probe_failure() is a debug-only no-op; HAS_DISPLAY is true. */
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_NOTE, "i915_inject_probe_failure:noop", 0U, 0U);
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_NOTE, "HAS_DISPLAY:true", 1U, 0U);

	/* P3.1 drm_dev_init, then drm_vblank_init for INTEL_NUM_PIPES = hweight8(A|B|C|D) crtcs. */
	pipe_mask = I915_DISPLAY_PIPE_MASK;
	num_pipes = 4U;
	device->stage = "drm_dev_init";
	error = drv_i915_drm_dev_init(&display->drm_dev, device, I915_DRM_DRIVER_FEATURES);
	if (error != 0)
		return error;

	display->drm_inited = 1;
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_ACQUIRE, "drm_dev_init", 0U, 0U);

	device->stage = "drm_vblank_init";
	error = drv_i915_drm_vblank_init(&display->drm_dev, num_pipes);
	if (error != 0)
		return error;

	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_ACQUIRE, "drm_vblank_init", (uint64_t)num_pipes, (uint64_t)pipe_mask);

	/*
	 * P3.2 intel_bios_init: the VBT from the explicit blob (test build
	 * only), the OpRegion or the PCI ROM, or the defaults of a genuine
	 * absence.  Void in the reference: it never fails the probe.
	 */
	(void)drv_i915_bios_init_ex(display, &display->vbt_state, &gt->pci, display->opregion_vbt_present, I915_VBT_EXPLICIT, &gt->trace);
	kern_logf("i915: P3 intel_bios_init done: source=%d vbt_found=%d version=%u child_devices=%u\n",
	    display->vbt_state.source,
	    display->vbt_state.vbt_found,
	    (unsigned)display->vbt_state.version,
	    display->vbt_state.num_display_devices);

	/* P3.3 intel_vga_register: a genuine absence is tolerated inside the call. */
	device->stage = "intel_vga_register";
	error = drv_i915_vga_register(&display->vga_client, device->pci, gt->display_ver, &gt->trace);
	if (error != 0)
		return error;

	display->vga_registered = 1;

	/* P3.4 intel_power_domains_init: options, DC masks, lock, async-put work and the power-well map. */
	device->stage = "intel_power_domains_init";
	error = drv_i915_power_domains_init(&display->power_domains, gt->display_ver, -1, -1, &gt->trace);
	if (error != 0)
		return error;

	display->power_domains_inited = 1;

	/* P3.5 intel_pmdemand_init_early: the PM demand lock and wait queue. */
	drv_i915_pmdemand_init_early(&display->pmdemand);
	drv_i915_trace_record(&gt->trace, 0U, I915_TRACE_ACQUIRE, "intel_pmdemand_init_early", 0U, 0U);

	/* Succeeded: the front of the noirq probe is up. */
	return 0;
}

/*
 * The firmware display check before the first display write: records what
 * the firmware left and decides whether the prepared start applies.
 * Read-only.  A STOP leaves the display untouched and unused.
 */
static int
i915_display_native_check(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_native_deps nd;
	struct i915_gt *gt;
	uint32_t reserved;

	display = device->display;
	gt = &device->gt;

	/* The registers, the OpRegion base, the aperture and the GGTT the driver writes. */
	kern_memset(&nd, 0, sizeof(nd));
	nd.mmio = &gt->mmio;
	nd.asls = drv_i915_pci_read32(&gt->pci, I915_PCI_ASLS);
	nd.gmadr_base = gt->gmadr_base;
	nd.gmadr_size = gt->gmadr_size;
	nd.ggtt_pages = gt->ggtt_entries;
	reserved = I915_GT_GGTT_PAGES + I915_GT_DISPLAY_PAGES;
	nd.driver_ggtt_first = 0U;
	if (gt->ggtt_entries > reserved)
		nd.driver_ggtt_first = gt->ggtt_entries - reserved;

	/* The pipes' real power state, read through the display's power domains. */
	nd.pipe_powered = drv_i915_n0_pipe_powered;
	nd.ctx = display;

	/* What intel_bios_init actually handed to the parser. */
	nd.parser_src = display->vbt_state.source;
	nd.parser_size = 0U;
	nd.parser_sha256 = NULL;

#ifdef I915_TEST_VBT
	/*
	 * The test build's explicit blob: its pin, and its bytes when the
	 * parser took them.
	 *
	 * XXX: a test crutch for the QEMU passthrough guest without an
	 * OpRegion; delete it once the GPU tests run on bare metal.
	 */
	nd.vbt_pin = drv_i915_vbt_explicit_pin(display);
	if (display->vbt_state.source == I915_VBT_SRC_EXPLICIT_BLOB) {
		nd.parser_size = display->vbt_state.blob_size;
		nd.parser_sha256 = display->vbt_state.blob_sha256;
	}
#endif

	/* The OpRegion copy the hardware probe made is what the parser consumed. */
	if (display->vbt_state.source == I915_VBT_SRC_OPREGION && display->p2_opd != NULL && display->p2_opd->vbt_valid) {
		nd.parser_size = display->p2_opd->vbt_size;
		nd.parser_sha256 = display->p2_opd->vbt_sha256;
	}

	/* Checks and logs. */
	display->n0_mmio = &gt->mmio;
	(void)drv_i915_native_precheck(display, &nd, &display->n0);
	drv_i915_native_log(display, &display->n0);
	kern_logf("i915: N0 before-N0: PCI COMMAND firmware=0x%04x now=0x%04x (BME/MEM set at P2; restored on teardown) | done already: GT reset (GDRST full, display untouched on ADL-P), GT fault/error-register clears, fence clears (0..31), PCODE reads, MSI enabled (no handler), WC CPU view of the aperture | not done: any display register, any GGTT PTE, any D-state change\n",
	    (unsigned)gt->pci.saved_command,
	    (unsigned)drv_i915_pci_read16(&gt->pci, I915_PCI_COMMAND_WORD));

	/* A STOP: nothing on the display was touched, and nothing will be. */
	if (!display->n0.proceed) {
		kern_logf("i915: N0: the display is not used (native-precheck before any display write); the GPU node has no display\n");
		display->absent = 1;
	}

	/* Succeeded: the check ran. */
	return 0;
}

/*
 * P3.6 intel_power_domains_init_hw(i915, false): the display core on the
 * real device -- DC states off, the PCH reset handshake, the combo PHYs,
 * PW1, CDCLK, DBUF, BW_BUDDY and the XE_LPD WAs, then POWER_DOMAIN_INIT
 * held and every well synced.
 */
static int
i915_display_core_init(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_display_core *dcore;
	struct i915_gt *gt;
	uint8_t revid;
	int step;

	display = device->display;
	gt = &device->gt;
	dcore = &display->dcore;

	/* The stepping from the live configuration space (the cached header reads all-ones here). */
	revid = drv_i915_pci_read8(&gt->pci, I915_PCI_REVISION);
	step = drv_i915_adlp_display_step(revid);

	/* The CDCLK state and hooks of this display. */
	kern_memset(&display->cdclk, 0, sizeof(display->cdclk));
	drv_i915_init_cdclk_hooks(&display->cdclk, (int)gt->display_ver, step, gt->is_alderlake_p);
	display->cdclk.m = &gt->mmio;
	display->cdclk.sb_lock = &gt->sb_lock;

	/* The power-well context: registers, the VGA client, interrupts not enabled yet. */
	kern_memset(&display->pwc, 0, sizeof(display->pwc));
	display->pwc.mmio = &gt->mmio;
	display->pwc.vga = &display->vga_client;
	display->pwc.irqs_enabled = 0;

	/* The display core over the one power, CDCLK, MMIO and sideband state. */
	kern_memset(dcore, 0, sizeof(*dcore));
	dcore->pd = &display->power_domains;
	dcore->cd = &display->cdclk;
	dcore->pwc = &display->pwc;
	dcore->m = &gt->mmio;
	dcore->sb_lock = &gt->sb_lock;

	/* The DBUF slices and ABOX of this display: XE_LPD S1 to S4, Tiger Lake S1 to S2. */
	dcore->dbuf_slice_mask = I915_DISPLAY_DBUF_TGL;
	dcore->abox_mask = I915_DISPLAY_ABOX_TGL;
	if (gt->display_ver >= 13U) {
		dcore->dbuf_slice_mask = I915_DISPLAY_DBUF_XELPD;
		dcore->abox_mask = I915_DISPLAY_ABOX_XELPD;
	}

	/* The platform, for the steps the reference gates on it. */
	dcore->display_ver = (int)gt->display_ver;
	dcore->is_alderlake_p = 0U;
	if (gt->is_alderlake_p)
		dcore->is_alderlake_p = 1U;

	/* The LCD path picks its tables from the display version. */
	drv_i915_lcd_set_display_ver(display, (int)gt->display_ver);
	dcore->dram_type = display->dram_info.type;
	dcore->dram_channels = display->dram_info.num_channels;

	/* Brings the display core up. */
	drv_i915_power_domains_init_hw(dcore, 0);
	display->display_core_inited = dcore->reached_init_ref;

	/* An adaptation-layer fault stops the start at the child that met it. */
	if (dcore->fault_stop) {
		device->stage = "intel_power_domains_init_hw";
		if (dcore->fault_where != NULL)
			device->stage = dcore->fault_where;

		kern_logf("i915: P3 intel_power_domains_init_hw(false): adaptation-layer fault at %s\n", device->stage);
		return EIO;
	}

	kern_logf("i915: P3 intel_power_domains_init_hw(false) done: cdclk=%u vco=%u dbuf=0x%x init_ref=%d sync_hw=%d\n",
	    display->cdclk.hw.cdclk,
	    display->cdclk.hw.vco,
	    dcore->dbuf_enabled_slices,
	    dcore->init_wakeref_held,
	    dcore->reached_sync_hw);

	/* Succeeded: the display core is up. */
	return 0;
}

/*
 * P3.7 intel_dmc_init: acquires, parses and loads the DMC firmware
 * asynchronously.  A DMC-owned POWER_DOMAIN_INIT reference is taken
 * (released by the worker on a successful load); the probe continues
 * without waiting for the load.
 */
static void
i915_display_dmc_init(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_gt *gt;
	const char *path;
	uint8_t revid;
	int step;
	char stepping;
	char substepping;
	int error;

	display = device->display;
	gt = &device->gt;

	/* The DMC's workqueue; without it the DMC is not loaded. */
	error = drv_i915_workqueue_create(&display->dmc_wq, "i915-dmc");
	if (error != 0)
		return;

	/* The stepping from the live configuration space, as a letter and a digit. */
	revid = drv_i915_pci_read8(&gt->pci, I915_PCI_REVISION);
	step = drv_i915_adlp_display_step(revid);
	stepping = (char)('A' + (step - I915_STEP_A0) / 4);
	substepping = (char)('0' + (step - I915_STEP_A0) % 4);

	/* The DMC of this display (intel_dmc.c: ADLP_DMC_PATH, TGL_DMC_PATH). */
	path = "i915/tgl_dmc_ver2_12.bin";
	if (gt->display_ver >= 13U)
		path = "i915/adlp_dmc.bin";

	/* Queues the load. */
	kern_memset(&display->dmc_dev, 0, sizeof(display->dmc_dev));
	drv_i915_dmc_init(&display->dmc_dev, &display->dmc_wq, &gt->mmio, &display->power_domains, &display->pwc, (int)gt->display_ver, gt->is_alderlake_p, stepping, substepping, path);
	display->dmc_inited = 1;
	display->pwc.display_ver = (int)gt->display_ver;
	kern_logf("i915: P3 intel_dmc_init: revid=0x%x step=%c%c DMC load queued (path=%s, work_submitted=%d)\n",
	    (unsigned)revid,
	    stepping,
	    substepping,
	    display->dmc_dev.fw_path,
	    display->dmc_dev.work_submitted);
}

/*
 * P3.9 to P3.16, the tail of intel_display_driver_probe_noirq():
 * intel_mode_config_init, intel_cdclk_init, intel_color_init,
 * intel_dbuf_init, intel_bw_init (the bandwidth software state and the
 * forced SAGV disable), intel_pmdemand_init, intel_init_quirks and
 * intel_fbc_init.  The five that return an error share the reference's one
 * error label, which the device stop's order reproduces.
 */
static int
i915_display_noirq_tail(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_display_state *dstate;
	struct i915_gt *gt;
	uint16_t q_dev;
	uint16_t q_svid;
	uint16_t q_sdid;
	int funcs;
	int error;

	display = device->display;
	gt = &device->gt;
	dstate = &display->dstate;

	/* The mode config, with FBC's module parameter at its default. */
	kern_memset(dstate, 0, sizeof(*dstate));
	dstate->enable_fbc_param = -1;
	drv_i915_mode_config_init(dstate, (int)gt->display_ver, I915_PLAT_NONE);
	display->dstate_inited = 1;

	/* The CDCLK global object. */
	device->stage = "intel_cdclk_init";
	error = drv_i915_cdclk_init(dstate);
	if (error != 0)
		return error;

	/* The colour init (display version 10 only; nothing here). */
	device->stage = "intel_color_init";
	error = drv_i915_color_init(dstate, (int)gt->display_ver);
	if (error != 0)
		return error;

	/* The DBUF global object. */
	device->stage = "intel_dbuf_init";
	error = drv_i915_dbuf_init(dstate);
	if (error != 0)
		return error;

	/* The bandwidth global object and the forced SAGV disable. */
	device->stage = "intel_bw_init";
	error = drv_i915_bw_init(dstate, (int)gt->display_ver, &display->bw_state, &gt->sb_lock, &gt->mmio);
	if (error != 0)
		return error;

	kern_logf("i915: P3 intel_bw_init: obj=%u sagv_forced=%d qgv=0x%x psf=0x%x mask=0x%x pcode=%d sagv_status=%d\n",
	    dstate->obj_count,
	    dstate->sagv_force_disable_attempted,
	    dstate->sagv_qgv_points,
	    dstate->sagv_psf_points,
	    (unsigned)dstate->bw_obj_state.qgv_points_mask,
	    dstate->sagv_pcode_ret,
	    display->bw_state.sagv_status);

	/* The PM demand global object. */
	device->stage = "intel_pmdemand_init";
	error = drv_i915_pmdemand_init(dstate, (int)gt->display_ver, &gt->mmio, -1);
	if (error != 0)
		return error;

	/* The quirks, by the live configuration space's ids. */
	q_dev = drv_i915_pci_read16(&gt->pci, I915_PCI_DEVICE_ID);
	q_svid = drv_i915_pci_read16(&gt->pci, I915_PCI_SUBSYSTEM_VENDOR);
	q_sdid = drv_i915_pci_read16(&gt->pci, I915_PCI_SUBSYSTEM_DEVICE);

	/* Diagnostic only: the cached PCI header next to a live read of the same fields. */
	kern_logf("i915: PCIID cached: ven=0x%04x dev=0x%04x rev=0x%02x subsys=%04x:%04x | live: ven=0x%04x dev=0x%04x rev=0x%02x subsys=%04x:%04x\n",
	    (unsigned)drv_pci_device_vendor(device->pci),
	    (unsigned)drv_pci_device_product(device->pci),
	    (unsigned)drv_pci_device_revision(device->pci),
	    (unsigned)drv_pci_device_subvendor(device->pci),
	    (unsigned)drv_pci_device_subproduct(device->pci),
	    (unsigned)drv_i915_pci_read16(&gt->pci, 0x00U),
	    (unsigned)drv_i915_pci_read16(&gt->pci, I915_PCI_DEVICE_ID),
	    (unsigned)drv_i915_pci_read8(&gt->pci, I915_PCI_REVISION),
	    (unsigned)drv_i915_pci_read16(&gt->pci, I915_PCI_SUBSYSTEM_VENDOR),
	    (unsigned)drv_i915_pci_read16(&gt->pci, I915_PCI_SUBSYSTEM_DEVICE));
	drv_i915_init_quirks(dstate, q_dev, q_svid, q_sdid);
	kern_logf("i915: P3 intel_init_quirks: dev=0x%x subsys=%04x:%04x quirk_mask=0x%x hooks=%u dmi_available=%d\n",
	    (unsigned)q_dev,
	    (unsigned)q_svid,
	    (unsigned)q_sdid,
	    dstate->quirk_mask,
	    dstate->quirk_hooks_fired,
	    dstate->dmi_available);

	/* FBC: the XE_LPD runtime fbc_mask is FBC A. */
	drv_i915_fbc_init(dstate, (int)gt->display_ver, I915_DISPLAY_FBC_MASK, 0, I915_PLAT_NONE);
	funcs = -1;
	if (dstate->fbc[0] != NULL)
		funcs = dstate->fbc[0]->funcs_kind;

	kern_logf("i915: P3 intel_fbc_init: fbc_mask=0x%x enable_fbc=%d created=%u funcs=%d\n",
	    dstate->fbc_mask,
	    dstate->enable_fbc_sanitized,
	    dstate->fbc_created,
	    funcs);
	kern_logf("i915: P3 probe_noirq COMPLETE: global objs=%u (order: %s,%s,%s,%s) mode_config max=%ux%u cursor=%ux%u async_flip=%d\n",
	    dstate->obj_count,
	    dstate->cdclk_obj.name,
	    dstate->dbuf_obj.name,
	    dstate->bw_obj.name,
	    dstate->pmdemand_obj.name,
	    dstate->mode_config.max_width,
	    dstate->mode_config.max_height,
	    dstate->mode_config.cursor_width,
	    dstate->mode_config.cursor_height,
	    dstate->mode_config.async_page_flip);

	/* Succeeded: the software state is complete. */
	return 0;
}

/*
 * P5-a: the front section of the nogem probe -- watermark and SAGV
 * latencies, the PPS and GMBUS bases, the crtc and plane records, the
 * shared DPLL table, CDCLK init_hw and the ADL-P WAs, and the VGA plane
 * disable.
 */
static int
i915_display_nogem_front(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_display_nogem *nogem;
	struct i915_gt *gt;
	int error;

	display = device->display;
	gt = &device->gt;
	nogem = &display->nogem;

	/* Runs the front section. */
	error = drv_i915_display_nogem_front(display, nogem, (int)gt->display_ver, I915_DISPLAY_PIPE_MASK, &gt->mmio, &gt->sb_lock, &display->cdclk, &display->bw_state, &display->vga_client);
	if (error != 0) {
		device->stage = "intel_display_driver_probe_nogem";
		if (nogem->fail_where != NULL)
			device->stage = nogem->fail_where;

		return error;
	}

	display->nogem_inited = 1;
	kern_logf("i915: P5a wm: levels=%u latency=%u/%u/%u/%u/%u/%u/%u/%u valid=%d sagv_status=%d block_time=%uus\n",
	    nogem->wm_num_levels,
	    nogem->wm_skl_latency[0],
	    nogem->wm_skl_latency[1],
	    nogem->wm_skl_latency[2],
	    nogem->wm_skl_latency[3],
	    nogem->wm_skl_latency[4],
	    nogem->wm_skl_latency[5],
	    nogem->wm_skl_latency[6],
	    nogem->wm_skl_latency[7],
	    nogem->wm_latency_valid,
	    nogem->sagv_status,
	    nogem->sagv_block_time_us);
	kern_logf("i915: P5a objects: crtcs=%u planes/crtc=%u scalers=%u dplls=%u(mgr=%d) gmbus_pins=%u pps_base=0x%x gmbus_base=0x%x\n",
	    nogem->num_crtcs,
	    nogem->crtcs[0].num_planes,
	    nogem->crtcs[0].num_scalers,
	    nogem->num_dplls,
	    nogem->dpll_mgr_present,
	    nogem->gmbus_pins_present,
	    nogem->pps_mmio_base,
	    nogem->gmbus_mmio_base);
	kern_logf("i915: P5a hw: max_cdclk=%u nssc_ref=%u adlp_wa=%d hti_read=%d vga_already_off=%d vga_disabled=%d writes=%u (gmbus_adapters/hdcp_component unimplemented: %d/%d)\n",
	    nogem->max_cdclk_freq,
	    nogem->dpll_ref_nssc,
	    nogem->adlp_wa_applied,
	    nogem->hti_state_read,
	    nogem->vga_already_disabled,
	    nogem->vga_disable_done,
	    nogem->mmio_writes,
	    nogem->gmbus_adapters_unimplemented,
	    nogem->hdcp_component_unimplemented);

	/* Succeeded: the front section is done. */
	return 0;
}

/*
 * P5-b intel_setup_outputs(): HAS_DDI -> intel_ddi_crt_present() (false on
 * version 9 and later) -> intel_bios_for_each_encoder(intel_ddi_init).  The
 * eDP connector is built inside intel_ddi_init, as the reference does.
 */
static void
i915_display_setup_outputs(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_display_nogem *nogem;
	struct i915_gt *gt;
	unsigned i;

	display = device->display;
	gt = &device->gt;
	nogem = &display->nogem;

	/* The resident eDP is prepared; the DDI init builds its connector. */
	drv_i915_edp_device_prepare(&display->edp_dev, &gt->mmio, &display->power_domains, &display->pwc, &display->vbt_state);
	nogem->dp_connector_init = drv_i915_edp_device_init_connector;
	nogem->dp_connector_ctx = &display->edp_dev;

	/* Runs the DDI decision chain over the ports A, B, TC1 to TC4. */
	drv_i915_setup_outputs(nogem, (int)gt->display_ver, I915_DISPLAY_PORT_MASK, &display->vbt_state, &gt->mmio);
	kern_logf("i915: P5b setup_outputs: vbt_children=%u ddi_init=%u encoders=%u skipped=%u crt_present=%d\n",
	    display->vbt_state.num_display_devices,
	    nogem->ddi_init_calls,
	    nogem->num_encoders,
	    nogem->ddi_skipped,
	    nogem->crt_present);

	/* Names every encoder made. */
	for (i = 0U; i < nogem->num_encoders; i++) {
		kern_logf("i915: P5b encoder[%u]: port=%c phy=%d tc=%d clk=%d pd=%d dvo=0x%02x dev_type=0x%x dp=%d hdmi=%d\n",
		    i,
		    (char)('A' + nogem->encoders[i].port),
		    nogem->encoders[i].phy,
		    nogem->encoders[i].is_tc,
		    nogem->encoders[i].clk_funcs,
		    nogem->encoders[i].power_domain,
		    (unsigned)nogem->encoders[i].dvo_port,
		    nogem->encoders[i].device_type,
		    nogem->encoders[i].init_dp,
		    nogem->encoders[i].init_hdmi);
	}

	/* Names every port skipped and why. */
	for (i = 0U; i < nogem->num_ddi_skips; i++)
		kern_logf("i915: P5b ddi skip[%u]: port=%d reason=%d\n", i, nogem->ddi_skip_port[i], nogem->ddi_skip_reason[i]);
}

/*
 * P5-c and P5-d: intel_modeset_setup_hw_state(), readout and sanitize.
 *
 * The reference takes a POWER_DOMAIN_INIT reference around the whole of
 * it; the init reference is still held here.  The sanitize turns back off
 * what the firmware left unused or inconsistent; on a quiescent device
 * every step is a no-op.
 */
static void
i915_display_readout(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_display_nogem *nogem;
	struct i915_gt *gt;
	uint8_t revid;

	display = device->display;
	gt = &device->gt;
	nogem = &display->nogem;

	/* The readout half. */
	drv_i915_modeset_readout_hw_state(nogem, (int)gt->display_ver, &gt->mmio, &display->power_domains, &display->pwc);
	kern_logf("i915: P5c readout: crtcs=%u active_pipes=0x%x planes_visible=%u encoders_linked=%u dplls_on=%u (detail readout unimplemented: %d)\n",
	    nogem->readout_crtcs,
	    nogem->active_pipes,
	    nogem->readout_planes_visible,
	    nogem->readout_encoders_linked,
	    nogem->readout_dplls_on,
	    nogem->readout_detail_unimplemented);

	/* The sanitize half, with the stepping from the live configuration space. */
	revid = drv_i915_pci_read8(&gt->pci, I915_PCI_REVISION);
	drv_i915_modeset_sanitize_hw_state(nogem, (int)gt->display_ver, drv_i915_adlp_display_step(revid), display->dstate.fbc_mask, &gt->mmio, &display->power_domains, &display->pwc);
	kern_logf("i915: P5d sanitize: vblank_resets=%u dmc_pipes=%u vblank_on=%u fbc_deact=%u enc_clk_gated=%u dplls_disabled=%u wells_disabled=%u cmtg_wa=%d early_was=%d wm_read=%d (crtc_disable_noatomic needed: %d)\n",
	    nogem->vblank_resets,
	    nogem->dmc_pipes_enabled,
	    nogem->vblank_on_count,
	    nogem->fbc_deactivated,
	    nogem->encoder_clocks_gated,
	    nogem->dplls_disabled,
	    nogem->wells_disabled,
	    nogem->cmtg_wa_applied,
	    nogem->early_display_was_applied,
	    nogem->wm_hw_state_read,
	    nogem->crtc_disable_noatomic_unimplemented);
}

/*
 * intel_display_driver_probe(): the initial commit (an active crtc needs
 * the atomic commit, which is not ported), no overlay, no fbdev, the
 * hotplug init over the encoders' pins, the poll disable and IPC.
 * Returns 0, or EINVAL.
 */
static int
i915_display_driver_probe(
	struct i915_display *display,
	int intel_irqs_enabled)
{
	struct i915_driver_probe *p;
	const struct i915_display_nogem *nogem;
	struct i915_mmio *m;
	int display_ver;
	unsigned i;
	unsigned pipe;

	p = &display->dprobe;
	nogem = &display->nogem;
	m = &display->device->gt.mmio;
	display_ver = (int)display->device->gt.display_ver;

	/*
	 * intel_initial_commit(): an active crtc gets its planes added and its
	 * state recomputed by an atomic commit.  With no active crtc the commit
	 * carries nothing and returns 0.
	 */
	p->active_crtcs = 0U;
	for (pipe = 0U; pipe < 32U; pipe++) {
		if ((nogem->active_pipes & (1U << pipe)) != 0U)
			p->active_crtcs++;
	}

	/* The reference only logs a failed initial modeset and continues. */
	if (p->active_crtcs != 0U) {
		p->initial_commit_unimplemented = 1;
		p->initial_commit_rc = -EOPNOTSUPP;
	} else {
		p->initial_commit_rc = 0;
	}

	/* intel_overlay_setup(): HAS_OVERLAY is gen2 to gen4.  intel_fbdev_init(): the emulation is off. */
	p->overlay = 0;
	p->fbdev = 0;

	/* intel_hpd_init(): the encoders' pins are the ones intel_ddi_init() set. */
	p->hp.n_encoders = 0U;
	for (i = 0U; i < nogem->num_encoders && i < 8U; i++) {
		p->hp.encoder_pin[p->hp.n_encoders] = drv_i915_ddi_hpd_pin(display_ver, nogem->encoders[i].port);
		p->hp.n_encoders++;
	}

	drv_i915_hpd_init_pins(&p->hp, display_ver, display->pch.type);
	drv_i915_hpd_init(&p->hp, m, display_ver, display->pch.type, 1, intel_irqs_enabled);

	/* intel_hpd_poll_disable(). */
	drv_i915_hpd_poll_disable(&p->hp, &display->power_domains, &display->pwc);

	/* skl_watermark_ipc_init(): XE_LPD has IPC; ADL-P is not SKL, KBL, CFL or CML. */
	p->ipc_enabled = i915_skl_watermark_ipc_init(m, 1, 1);

	/* Succeeded: the display probe is done. */
	return 0;
}

/*
 * skl_watermark_ipc_init(): enables IPC where the platform can
 * (skl_watermark_ipc_can_enable(): SKL never, KBL/CFL/CML by DRAM
 * symmetry) and writes DISP_ARB_CTL2 (skl_watermark_ipc_update()).
 */
static int
i915_skl_watermark_ipc_init(
	struct i915_mmio *m,
	int has_ipc,
	int platform_can)
{
	int enabled;
	uint32_t set;

	/* A platform without IPC. */
	if (!has_ipc)
		return 0;

	/* Whether this platform enables it. */
	enabled = 0;
	if (platform_can)
		enabled = 1;

	/* Programs DISP_IPC_ENABLE. */
	set = 0U;
	if (enabled)
		set = I915_DISP_IPC_ENABLE;
	(void)i915_display_rmw(m, I915_DISP_ARB_CTL2, I915_DISP_IPC_ENABLE, set);

	/* Reports whether IPC is enabled. */
	return enabled;
}

/* intel_uncore_rmw(): writes only when the value changes; returns the new value. */
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
 * i915_driver_register(): the userspace-facing registrations (N/A here),
 * intel_display_driver_register(), intel_power_domains_enable(), and
 * intel_runtime_pm_enable() (the probe reference goes back; autosuspend
 * is not armed because intel_runtime_suspend is not ported).
 */
static void
i915_driver_register(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_driver_probe *p;

	display = device->display;
	p = &display->dprobe;

	/*
	 * i915_gem_driver_register, i915_pmu_register, intel_vgpu_register (not
	 * vGPU), drm_dev_register, i915_debugfs_register, i915_setup_sysfs,
	 * i915_perf_register, intel_gt_driver_register (sysfs/debugfs),
	 * intel_pxp_debugfs_register, i915_hwmon_register (DGFX): userspace.
	 */
	p->na_registrations = 9U;

	/*
	 * intel_display_driver_register(): intel_opregion_register() acts only
	 * with an OpRegion header, whose runtime is not registered (VBT_ONLY);
	 * the ACPI video, audio components, debugfs and fbdev config are N/A;
	 * then drm_kms_helper_poll_init().
	 */
	p->opregion_registered = 0;
	p->na_registrations += 4U;
	p->hp.kms_poll_inited = 1;

	/*
	 * intel_power_domains_enable(): the INIT reference taken during
	 * init_hw is released, and every well nothing references powers down.
	 */
	drv_i915_power_domains_enable(p, &display->dcore);

	/* intel_runtime_pm_enable(): the probe reference the PCI core took goes back. */
	drv_i915_rpm_put(&device->gt.probe_pm);
	p->rpm_usage_after = drv_i915_rpm_usage(&device->gt.probe_pm);

	/* intel_register_dsm_handler (ACPI _DSM) and i915_switcheroo_register: N/A. */
	p->na_registrations += 2U;
	p->registered = 1;
}

/*
 * i915_driver_unregister(): intel_runtime_pm_disable() (the ownership goes
 * back to the core), intel_power_domains_disable(), and
 * intel_display_driver_unregister() (fbdev and audio N/A,
 * drm_kms_helper_poll_fini, nothing active to shut down, the OpRegion
 * unregister).
 */
static void
i915_driver_unregister(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_driver_probe *p;

	display = device->display;
	p = &display->dprobe;

	/* Nothing was registered. */
	if (!p->registered)
		return;

	/* The probe reference is taken back from the core. */
	(void)drv_i915_rpm_get_sync(&device->gt.probe_pm);

	/* The INIT reference is taken again. */
	drv_i915_power_domains_disable(&display->dcore);

	/* The display is unregistered. */
	p->hp.kms_poll_inited = 0;
	p->opregion_registered = 0;
	p->registered = 0;
}

/* Counts the power wells that are on. */
static unsigned
i915_display_wells_on(
	struct i915_display *display)
{
	struct i915_power_domains *pd;
	unsigned count;
	unsigned i;
	int enabled;

	pd = &display->power_domains;

	/* Asks every well. */
	count = 0U;
	for (i = 0U; i < pd->num_power_wells; i++) {
		enabled = drv_i915_power_well_is_enabled(&pd->power_wells[i], &display->pwc);
		if (enabled)
			count++;
	}

	/* Reports the count. */
	return count;
}

/* Logs every power well that is held or on, and the DC state. */
static void
i915_display_log_wells(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_power_well *well;
	unsigned i;
	int enabled;

	display = device->display;

	/* Names every well that is held or on. */
	for (i = 0U; i < display->power_domains.num_power_wells; i++) {
		well = &display->power_domains.power_wells[i];
		enabled = drv_i915_power_well_is_enabled(well, &display->pwc);
		if (well->refcount != 0U || enabled)
			kern_logf("i915: P7 well %s: refcount=%u hw_enabled=%d\n", well->name, well->refcount, enabled);
	}

	kern_logf("i915: P7 DC_STATE_EN=0x%08x DC_off disable_calls=%u dc_state_writes=%u rewrites=%u dmc_has_payload=%d\n",
	    drv_i915_read32(&device->gt.mmio, I915_DISPLAY_DC_STATE_EN),
	    display->pwc.dc_off_disable_calls,
	    display->pwc.dc_state_writes,
	    display->pwc.dc_state_rewrites,
	    display->pwc.dmc_has_payload);
}

/*
 * The per-crtc cleanup (drm_vblank_init_release and
 * drm_vblank_worker_fini): the worker goes first, so neither it nor its
 * disable timer can still touch the crtc; a crtc whose worker never
 * started is fine.
 */
static void
i915_drm_vblank_crtc_cleanup(
	void *arg)
{
	struct i915_drm_vblank_crtc *vc;

	vc = arg;

	/* A crtc that was never initialised. */
	if (!vc->inited)
		return;

	/* Stops and joins the worker. */
	if (vc->worker_created) {
		drv_i915_workqueue_destroy(&vc->worker);
		vc->worker_created = 0;
	}

	/* The timer and the seqlock go with it. */
	vc->disable_timer_inited = 0;
	vc->disable_deadline = 0U;
	vc->seqlock = 0U;
	vc->inited = 0;
}

/*
 * intel_virt_detect_pch(): in a passthrough guest the ISA bridge may not be
 * passed through, so the PCH is guessed from the graphics platform.  Only
 * the Alder Lake arm is reachable here.
 */
static void
i915_virt_detect_pch(
	int is_alderlake,
	uint16_t *pch_id,
	int *pch_type)
{
	uint16_t id;

	/* The Alder Lake PCH, or none. */
	id = 0U;
	if (is_alderlake)
		id = INTEL_PCH_ADP_DEVICE_ID_TYPE;

	/* Names the guess. */
	if (id != 0U) {
		kern_logf("i915: Assuming PCH ID %04x\n", (unsigned)id);
	} else {
		kern_logf("i915: Assuming no PCH\n");
	}

	/* Reports the type and the id. */
	*pch_type = drv_i915_pch_type(id);
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
	struct i915_device *owner_device;
	struct i915_display *display;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	uint32_t width_mm;
	uint32_t height_mm;
	int error;
	int size_error;

	UNUSED_PARAMETER(session);

	owner_device = device;
	display = owner_device->display;

	/* One display exists while the panel is known. */
	error = drv_i915_display_panel(display, &width, &height, &refresh);
	request->count = 0U;
	if (error == 0)
		request->count = 1U;

	/* Only the count was asked for. */
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;

	/* Only display 0 exists. */
	if (error != 0 || request->index != 0U)
		return EINVAL;

	/* The display, its state and its formats. */
	request->display_id = I915_DISPLAY_ID;
	request->generation = I915_DISPLAY_GENERATION;
	/* Frames are presented from shared resources (present.c), so BLOB is offered. */
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO | GPU_DISPLAY_BLOB;
	if (display->rd.active)
		request->flags |= GPU_DISPLAY_ACTIVE;

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

	/* The physical size, when the panel reports one. */
	size_error = drv_i915_display_panel_size_mm(display->rctx.lcd, &width_mm, &height_mm);
	if (size_error == 0) {
		request->physical_width_mm = width_mm;
		request->physical_height_mm = height_mm;
	}

	kern_memcpy(request->name, "eDP panel", sizeof("eDP panel"));

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
	int error;

	UNUSED_PARAMETER(session);

	owner_device = device;

	/* Only the one display of this generation exists. */
	if (request->display_id != I915_DISPLAY_ID)
		return ENOENT;
	if (request->generation != I915_DISPLAY_GENERATION)
		return ESTALE;

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
 * Gives the display's lease to a session (the display claim operation).
 *
 * One lease at a time.  Returns 0 with the lease, ENOENT or ESTALE for
 * another display, EINVAL for a plane other than 0, or EBUSY while another
 * session holds it.
 */
static int
i915_display_claim(
	void *device,
	void *session,
	struct gpu_display_claim *request)
{
	struct i915_device *owner_device;
	struct i915_resident_display *rd;

	owner_device = device;
	rd = &owner_device->display->rd;

	/* Only plane 0 of the one display of this generation exists. */
	if (request->display_id != I915_DISPLAY_ID)
		return ENOENT;
	if (request->generation != I915_DISPLAY_GENERATION)
		return ESTALE;
	if (request->plane_index != 0U)
		return EINVAL;

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

	mutex_unlock(&rd->mutex);

	kern_logf("i915: resident display: lease %llu claimed\n", (unsigned long long)request->lease);

	/* Succeeded: the session holds the display. */
	return 0;
}
