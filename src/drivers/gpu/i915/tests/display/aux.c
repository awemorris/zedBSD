/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AUX diagnostics of the resident eDP panel, and the scanout buffer on
 * the real GGTT next to it.
 *
 * The scenario runs once the device has started, with the eDP brought up by
 * the output setup and its delayed VDD-off reserved.  It compares what that
 * bring-up kept (DPCD, eDP capabilities, EDID and the first modeset slice)
 * with the capture made through Linux on the target, lets the delayed VDD-off
 * run by itself from the tick, re-acquires the panel before a deadline, and
 * leaves a fresh reservation for the device stop to cancel.
 *
 * With GT memory up it then claims one full-HD XRGB8888 buffer in the
 * display window, checks its placement, every PTE and the GT window, draws
 * the LCD-B picture, computes the plane words for it and releases it.  The
 * display engine is never pointed at the buffer: no plane, pipe or link
 * register is written.
 *
 * The verdicts are log lines ("i915: AUX-TEST verdict:" and "i915:
 * SCANOUT-TEST verdict:"); the stop path's result is the "edp fini" line
 * the device stop prints.
 */

#include "../../i915.h"
#include "../../ggtt.h"
#include "../../memory.h"
#include "../../display/internal.h"
#include "../../display/diagnostics.h"
#include "../../display/dp-sink.h"
#include "../../display/scanout.h"
#include "../../display/state.h"
#include "dp-fixture-latitude5330.h"
#include "scenarios.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stdint.h>

/* How many transcoder words Linux's register dump holds for transcoder A. */
#define I915_AUX_TRANSCODER_WORDS 13u

/* How many operations hsw_configure_cpu_transcoder() records. */
#define I915_AUX_CPU_TRANSCODER_WORDS 17u

/* TRANS_DDI_FUNC_CTL of transcoder A, and the value Linux programmed there. */
#define I915_AUX_TRANS_DDI_FUNC_CTL_A 0x60400u
#define I915_AUX_TRANS_DDI_FUNC_CTL_A_LINUX 0x8a210002u

/* DDI_BUF_CTL of port A as Linux left it: the computed value plus the enable bit. */
#define I915_AUX_DDI_BUF_CTL_ENABLE 0x80000000u
#define I915_AUX_DDI_BUF_CTL_A_LINUX 0x80000002u

/* The plane registers of pipe A's primary plane. */
#define I915_AUX_PLANE_CTL_1_A 0x70180u
#define I915_AUX_PLANE_STRIDE_1_A 0x70188u
#define I915_AUX_PLANE_SIZE_1_A 0x70190u
#define I915_AUX_PLANE_SURF_1_A 0x7019cu
#define I915_AUX_PLANE_COLOR_CTL_1_A 0x701ccu

/* The plane words of Linux's register dump for the full-HD XRGB8888 buffer. */
#define I915_AUX_PLANE_CTL_LINUX 0x94000000u
#define I915_AUX_PLANE_STRIDE_LINUX 0x78u
#define I915_AUX_PLANE_SIZE_LINUX 0x0437077fu
#define I915_AUX_PLANE_COLOR_CTL_LINUX 0x2000u

/* The LCD-B picture and its FNV-1a 64 hash, pinned from the host render (lcd-pattern-host.c). */
#define I915_AUX_PATTERN_ID 110u
#define I915_AUX_PATTERN_110_FNV 0xce63f20b23f91f85ull

/* The size of the scanout buffer: the panel's full HD. */
#define I915_AUX_SCANOUT_WIDTH 1920u
#define I915_AUX_SCANOUT_HEIGHT 1080u

/* What a bound GGTT entry holds besides the page address: the present bit. */
#define I915_AUX_PTE_ADDRESS_MASK (~0xfffull)
#define I915_AUX_PTE_PRESENT 1ull

/*
 * One register word of Linux's dump.
 *
 * An instance names a register and the value Linux left in it on the
 * target; a table of them is compared with the words the driver computed.
 */
struct i915_aux_word {
	/* The register offset. */
	uint32_t reg;

	/* The value Linux programmed. */
	uint32_t value;
};

/*
 * What the AUX diagnostics observed on the resident panel.
 *
 * One instance lives on the scenario's stack; each phase fills its part and
 * the verdict is decided from all of them together.
 */
struct i915_aux_observation {
	/* Whether the kept DPCD, eDP capabilities and EDID equal the capture. */
	int dpcd_match;
	int edp_match;
	int edid_match;

	/* Whether the first modeset slice equals what Linux programmed. */
	int lcd_match;

	/* How long the delayed VDD-off took to run by itself, or -1 when it did not. */
	int auto_off_ms;

	/* Whether a re-acquisition kept VDD on past the old deadline. */
	int race_kept;

	/* How long the new reservation took to run by itself, or -1 when it did not. */
	int race_off_ms;

	/* The last DPCD read's result and its first two bytes. */
	long read;
	uint8_t bytes[2];
};

/*
 * Linux's register dump of transcoder A on the target
 * (display-ref/regs-selected.txt), in the order Linux writes the words.
 *
 * It is constant and shared by every run.
 */
static const struct i915_aux_word i915_aux_transcoder_linux[I915_AUX_TRANSCODER_WORDS] = {
	{ 0x60030u, 0x7e4b17e4u },
	{ 0x60034u, 0x00800000u },
	{ 0x60040u, 0x00042bfeu },
	{ 0x60044u, 0x00080000u },
	{ 0x6007cu, 0x00000000u },
	{ 0x60028u, 0x00000000u },
	{ 0x60000u, 0x081f077fu },
	{ 0x60004u, 0x081f077fu },
	{ 0x60008u, 0x079f078fu },
	{ 0x6000cu, 0x04670437u },
	{ 0x60010u, 0x04670000u },
	{ 0x60014u, 0x0448043au },
	{ 0x6001cu, 0x077f0437u },
};

/*
 * The scanout buffer of the GGTT check.
 *
 * The buffer storage must start zeroed and destroy returns it to that
 * state, so one storage serves every run; only the scenario uses it, and a
 * run leaves it empty unless the destroy was refused.
 */
static struct i915_scanout i915_aux_scanout;

/*
 * The GT window's PTEs as found before the GGTT check.
 *
 * Filled at the start of each check and compared twice: with the buffer
 * pinned and after it was released.  Too large for the stack.
 */
static uint64_t i915_aux_gt_before[I915_GT_GGTT_PAGES];

/*
 * The plane words computed for the scanout buffer.
 *
 * Filled by each check and only read back by it; too large for the stack.
 */
static struct i915_lcd_words i915_aux_plane_words;

static void i915_aux_run(struct i915_display *display);
static unsigned i915_aux_well_refs(const struct i915_power_domains *pd);
static int i915_aux_vdd_is_on(struct i915_dp_world *world, struct i915_edp_device *dev);
static int i915_aux_wait_vdd_off(struct i915_dp_world *world, struct i915_edp_device *dev, unsigned limit_ms);
static int i915_aux_lcd_matches(const struct i915_edp_device *dev);
static int i915_aux_lcd_words_match(const struct i915_edp_device *dev);
static int i915_aux_passed(const struct i915_edp_device *dev, const struct i915_aux_observation *obs);
static void i915_aux_scanout_hw_check(struct i915_gt_mem *gm, struct i915_lcd_world *lcd_world);
static void i915_aux_record_gt_window(const struct i915_gt_mem *gm);
static unsigned i915_aux_count_gt_changed(const struct i915_gt_mem *gm);
static unsigned i915_aux_count_pte_bad(const struct i915_gt_mem *gm, const struct i915_scanout *so);
static unsigned i915_aux_count_guard_bad(const struct i915_gt_mem *gm, const struct i915_scanout *so);
static unsigned i915_aux_count_not_scratch(const struct i915_gt_mem *gm, unsigned first_page, unsigned pages);
static int i915_aux_plane_words_match(const struct i915_scanout *so, int plane_rc, uint32_t *ctl, uint32_t *stride, uint32_t *size, uint32_t *color, uint32_t *surf);

/*
 * Runs the AUX diagnostics of the resident eDP panel and, with GT memory
 * up, the scanout buffer check on the real GGTT.
 *
 * Both report their verdict in the log.  The reservation of the delayed
 * VDD-off left pending here is cancelled by the device stop.
 */
void
drv_i915_test_display_aux(
	struct i915_device *device)
{
	struct i915_display *display;

	/* A device started without a display has no resident panel. */
	display = device->display;
	if (display == NULL) {
		kern_logf("i915: AUX-TEST verdict: FAIL (no display: the device started without one)\n");
		return;
	}

	/* Examines the resident panel and exercises its delayed VDD-off. */
	i915_aux_run(display);

	/* Places a scanout buffer next to the live GT resources; nothing scans it out. */
	if (device->gt.mem.inited)
		i915_aux_scanout_hw_check(&device->gt.mem, display->lcd_world);
}

/* Compares the resident panel with the capture and exercises the delayed VDD-off. */
static void
i915_aux_run(
	struct i915_display *display)
{
	struct i915_edp_device *dev;
	struct i915_edp_result *res;
	struct i915_dp_world *world;
	struct i915_aux_observation obs;
	unsigned ran_before_wait;
	unsigned ran_after_read;
	unsigned off_delay_ms;
	unsigned well_refs;
	int compared;
	int vdd_on;
	int passed;

	/* Only a panel whose normal initialisation succeeded can be examined. */
	dev = &display->edp_dev;
	res = &dev->res;
	world = display->dp_world;
	if (!dev->connector_live) {
		kern_logf("i915: AUX-TEST verdict: FAIL (no resident eDP: init_rc=%d; a real VBT with an "
			  "eDP child is required)\n", dev->init_rc);
		return;
	}

	/* Starts with nothing observed. */
	kern_memset(&obs, 0, sizeof(obs));

	/* The receiver capabilities the bring-up kept, against the capture made through Linux. */
	if (res->dpcd_ok) {
		compared = kern_memcmp(res->dpcd, i915_dp_fixture_dpcd_000, sizeof(res->dpcd));
		if (compared == 0)
			obs.dpcd_match = 1;
	}

	/* The eDP display control capabilities, against the capture. */
	if (res->edp_dpcd_ok) {
		compared = kern_memcmp(res->edp_dpcd, i915_dp_fixture_dpcd_700, sizeof(res->edp_dpcd));
		if (compared == 0)
			obs.edp_match = 1;
	}

	/* The one-block EDID, against the capture. */
	if (res->edid_ok && res->edid_blocks == 1u) {
		compared = kern_memcmp(res->edid, i915_dp_fixture_edid, 128u);
		if (compared == 0)
			obs.edid_match = 1;
	}

	/* The first modeset slice, against what Linux programmed on this machine. */
	obs.lcd_match = i915_aux_lcd_matches(dev);

	/*
	 * The delayed VDD-off runs by itself: nobody drives the worker here.
	 * It was reserved at five times the power-cycle delay.
	 */
	off_delay_ms = (unsigned)res->delay_power_cycle_ms * 5u;
	ran_before_wait = (unsigned)dev->k.vdd_off_work.work.ran_count;
	obs.auto_off_ms = i915_aux_wait_vdd_off(world, dev, off_delay_ms + 2000u);
	drv_i915_edp_snapshot(world, res);
	well_refs = i915_aux_well_refs(dev->k.pd);
	kern_logf("i915: AUX-TEST auto-off: reserved_ms=%u waited_here_ms=%d vdd_hw=%d vdd_wakeref=%d "
		  "worker_ran=%u->%d timer_fired=%u refs core=%d aux=%d well_refs=%u\n", off_delay_ms, obs.auto_off_ms,
		  res->vdd_on_hw, res->vdd_wakeref_held, ran_before_wait, dev->k.vdd_off_work.work.ran_count,
		  dev->k.vdd_off_work.fired_count, res->power_refs_core, res->power_refs_aux, well_refs);

	/*
	 * Re-acquires the panel before the deadline: a read turns VDD on again
	 * and reserves its off; a second read halfway cancels that reservation
	 * and reserves anew, so the old deadline must not drop the VDD in use.
	 */
	obs.read = drv_i915_edp_dpcd_read(world, 0x000u, obs.bytes, sizeof(obs.bytes));
	ran_after_read = (unsigned)dev->k.vdd_off_work.work.ran_count;
	drv_i915_dp_kernel_sleep_us(&dev->k, (off_delay_ms / 2u) * 1000u);
	if (obs.read == 2)
		obs.read = drv_i915_edp_dpcd_read(world, 0x000u, obs.bytes, sizeof(obs.bytes));

	/* Sleeps past the old deadline and looks whether VDD is still on and the worker did not run. */
	drv_i915_dp_kernel_sleep_us(&dev->k, (off_delay_ms / 2u + 300u) * 1000u);
	vdd_on = i915_aux_vdd_is_on(world, dev);
	if (vdd_on && (unsigned)dev->k.vdd_off_work.work.ran_count == ran_after_read)
		obs.race_kept = 1;

	/* The new reservation then fires by itself. */
	obs.race_off_ms = i915_aux_wait_vdd_off(world, dev, off_delay_ms + 2000u);
	drv_i915_edp_snapshot(world, res);
	kern_logf("i915: AUX-TEST re-acquire: reads_ok=%d dpcd=%02x %02x kept_past_old_deadline=%d "
		  "new_off_after_ms=%d cancelled(timer/queue)=%u/%u worker_ran=%d vdd_hw=%d refs core=%d aux=%d\n",
		  obs.read == 2, obs.bytes[0], obs.bytes[1], obs.race_kept, obs.race_off_ms,
		  dev->k.vdd_off_work.cancelled_armed, dev->k.vdd_off_work.cancelled_pending,
		  dev->k.vdd_off_work.work.ran_count, res->vdd_on_hw, res->power_refs_core, res->power_refs_aux);

	/* Leaves a reservation pending: the stop path must cancel it synchronously and force VDD off. */
	if (obs.read == 2)
		obs.read = drv_i915_edp_dpcd_read(world, 0x700u, obs.bytes, sizeof(obs.bytes));

	drv_i915_edp_snapshot(world, res);
	kern_logf("i915: AUX-TEST left for the stop path: vdd_hw=%d off_reserved=%d vdd_wakeref=%d "
		  "refs aux=%d | sleeps: tick=%u (%llu us) short=%u (%llu us) | lock_errors=%u log_errors=%u\n",
		  res->vdd_on_hw, res->vdd_work_pending, res->vdd_wakeref_held, res->power_refs_aux,
		  dev->k.tick_sleeps, (unsigned long long)dev->k.tick_slept_us, dev->k.busy_sleeps,
		  (unsigned long long)dev->k.busy_slept_us, dev->env.lock_errors, res->log_errors);

	/* Decides and reports the verdict. */
	passed = i915_aux_passed(dev, &obs);
	kern_logf("i915: AUX-TEST verdict: %s (resident=1 dpcd_match=%d edp_dpcd_match=%d edid_match=%d "
		  "lcd_a_match=%d auto_off=%d reacquire_kept=%d reacquire_off=%d; the stop-path result is the \"edp fini\" line)\n",
		  passed ? "PASS" : "FAIL", obs.dpcd_match, obs.edp_match, obs.edid_match, obs.lcd_match,
		  obs.auto_off_ms >= 0, obs.race_kept, obs.race_off_ms >= 0);
}

/* Reports the references all power wells hold. */
static unsigned
i915_aux_well_refs(
	const struct i915_power_domains *pd)
{
	unsigned wi;
	unsigned refs;

	/* Adds up every well's count. */
	refs = 0u;
	for (wi = 0u; wi < pd->num_power_wells; wi++)
		refs += pd->power_wells[wi].refcount;

	/* Succeeded: reports the sum. */
	return refs;
}

/* Reports whether the panel's VDD is on now, refreshing the kept result. */
static int
i915_aux_vdd_is_on(
	struct i915_dp_world *world,
	struct i915_edp_device *dev)
{
	/* Reads the sequencer and who holds what. */
	drv_i915_edp_snapshot(world, &dev->res);

	/* Succeeded: reports the VDD bit the hardware shows. */
	return dev->res.vdd_on_hw;
}

/* Waits, yielding, until VDD reads off; reports the milliseconds waited, or -1 past the limit. */
static int
i915_aux_wait_vdd_off(
	struct i915_dp_world *world,
	struct i915_edp_device *dev,
	unsigned limit_ms)
{
	unsigned waited;
	int vdd_on;

	/* Looks every 50 ms until VDD is off or the limit has passed. */
	waited = 0u;
	vdd_on = i915_aux_vdd_is_on(world, dev);
	while (vdd_on) {
		if (waited >= limit_ms)
			return -1;

		drv_i915_dp_kernel_sleep_us(&dev->k, 50000u);
		waited += 50u;
		vdd_on = i915_aux_vdd_is_on(world, dev);
	}

	/* Succeeded: VDD went off after this long. */
	return (int)waited;
}

/*
 * Reports whether the first modeset slice equals what Linux programmed on
 * this machine (plan/ws031/display-ref/): transcoder A timings,
 * PIPE_DATA/LINK_M1/N1 = 0x7e4b17e4 / 0x800000 / 273406 / 524288, DPLL0
 * CFGCR0/1 = 0x00e001a5 / 0x88, DDI A x2 HBR 6 bpc.
 */
static int
i915_aux_lcd_matches(
	const struct i915_edp_device *dev)
{
	const struct i915_lcd_state *lcd;
	int words_match;

	/* The calculation must have succeeded. */
	lcd = &dev->lcd;
	if (dev->lcd_rc != 0)
		return 0;

	/* The mode: 1920x1080 at 140.8 MHz with Linux's sync positions and totals. */
	if (lcd->mode.clock_khz != 140800)
		return 0;
	if (lcd->mode.hdisplay != 1920u)
		return 0;
	if (lcd->mode.hsync_start != 1936u)
		return 0;
	if (lcd->mode.hsync_end != 1952u)
		return 0;
	if (lcd->mode.htotal != 2080u)
		return 0;
	if (lcd->mode.vdisplay != 1080u)
		return 0;
	if (lcd->mode.vsync_start != 1083u)
		return 0;
	if (lcd->mode.vsync_end != 1097u)
		return 0;
	if (lcd->mode.vtotal != 1128u)
		return 0;

	/* The link: HBR x2 at 18 bpp, one transfer unit of 64. */
	if (lcd->link.rate_khz != 270000)
		return 0;
	if (lcd->link.lanes != 2)
		return 0;
	if (lcd->link.bpp != 18)
		return 0;
	if (lcd->link.tu != 64u)
		return 0;

	/* The data and link M/N. */
	if (lcd->link.data_m != 0x4b17e4u)
		return 0;
	if (lcd->link.data_n != 0x800000u)
		return 0;
	if (lcd->link.link_m != 273406u)
		return 0;
	if (lcd->link.link_n != 524288u)
		return 0;

	/* The DPLL configuration words. */
	if (lcd->pll.cfgcr0 != 0x00e001a5u)
		return 0;
	if (lcd->pll.cfgcr1 != 0x88u)
		return 0;

	/* The transcoder, CPU transcoder and DDI words. */
	words_match = i915_aux_lcd_words_match(dev);
	if (!words_match)
		return 0;

	/* Succeeded: every value equals Linux's. */
	return 1;
}

/*
 * Reports whether the computed words equal Linux's register dump: the
 * transcoder words in Linux's write order, PIPE_DDI_FUNC_CTL_A 0x8a210002
 * and DDI_BUF_CTL_A 0x80000002 (the computed value plus the enable bit).
 */
static int
i915_aux_lcd_words_match(
	const struct i915_edp_device *dev)
{
	uint32_t func;
	unsigned found;
	unsigned wi;

	/* The transcoder words must all have been computed. */
	if (dev->lcd_words_rc != 0)
		return 0;
	if (dev->lcd_words.n != I915_AUX_TRANSCODER_WORDS)
		return 0;

	/* The CPU transcoder operations must all have been computed. */
	if (dev->lcd_cpu_words_rc != 0)
		return 0;
	if (dev->lcd_cpu_words.n != I915_AUX_CPU_TRANSCODER_WORDS)
		return 0;

	/* TRANS_DDI_FUNC_CTL must be among the DDI words and equal Linux's. */
	if (dev->lcd_ddi_words_rc != 0)
		return 0;

	func = 0u;
	found = drv_i915_lcd_words_find(&dev->lcd_ddi_words, I915_AUX_TRANS_DDI_FUNC_CTL_A, &func);
	if (found != 1u)
		return 0;
	if (func != I915_AUX_TRANS_DDI_FUNC_CTL_A_LINUX)
		return 0;

	/* DDI_BUF_CTL with the enable bit that comes with link training. */
	if ((dev->lcd_ddi_buf_ctl | I915_AUX_DDI_BUF_CTL_ENABLE) != I915_AUX_DDI_BUF_CTL_A_LINUX)
		return 0;

	/* Every transcoder word, in order. */
	for (wi = 0u; wi < I915_AUX_TRANSCODER_WORDS; wi++) {
		if (dev->lcd_words.w[wi].reg != i915_aux_transcoder_linux[wi].reg)
			return 0;
		if (dev->lcd_words.w[wi].value != i915_aux_transcoder_linux[wi].value)
			return 0;
	}

	/* Succeeded: every word equals Linux's. */
	return 1;
}

/* Decides the AUX verdict from what the scenario observed and the device's counters. */
static int
i915_aux_passed(
	const struct i915_edp_device *dev,
	const struct i915_aux_observation *obs)
{
	const struct i915_edp_result *res;

	/* The late step of the bring-up must have succeeded. */
	res = &dev->res;
	if (dev->late_rc != 0)
		return 0;

	/* What the bring-up kept must equal the capture and Linux's values. */
	if (!obs->dpcd_match)
		return 0;
	if (!obs->edp_match)
		return 0;
	if (!obs->edid_match)
		return 0;
	if (!obs->lcd_match)
		return 0;

	/* The delayed VDD-off must have run by itself. */
	if (obs->auto_off_ms < 0)
		return 0;

	/* The last read must have returned the eDP revision of the capture. */
	if (obs->read != 2)
		return 0;
	if (obs->bytes[0] != i915_dp_fixture_dpcd_700[0])
		return 0;

	/* The re-acquisition must have kept VDD on, and its reservation fired later. */
	if (!obs->race_kept)
		return 0;
	if (obs->race_off_ms < 0)
		return 0;

	/* A reservation must be left pending for the stop path, with VDD on. */
	if (res->vdd_on_hw != 1)
		return 0;
	if (res->vdd_work_pending != 1)
		return 0;

	/* No lock, message, time-base or power-layer error may have been counted. */
	if (dev->env.lock_errors != 0u)
		return 0;
	if (res->log_errors != 0u)
		return 0;
	if (dev->k.time_faults != 0u)
		return 0;
	if (dev->k.pd->async_state_errors != 0u)
		return 0;
	if (dev->k.pd->use_count_errors != 0u)
		return 0;

	/* Succeeded: every observation passed. */
	return 1;
}

/*
 * Claims a full-HD XRGB8888 buffer in the display window next to the live
 * GT resources, checks its placement, every PTE and the GT window, draws
 * the LCD-B picture, computes its plane words and releases it.
 *
 * The display window may already be claimed by the resident display, and
 * the resident buffers may be pinned in it: the check then shares the window
 * and expects the pages in use to return to what they were before.
 */
static void
i915_aux_scanout_hw_check(
	struct i915_gt_mem *gm,
	struct i915_lcd_world *lcd_world)
{
	struct i915_scanout *so;
	uint64_t hash;
	uint32_t verify_bad;
	uint32_t first_x;
	uint32_t first_y;
	uint32_t plane_ctl;
	uint32_t plane_stride;
	uint32_t plane_size;
	uint32_t plane_color;
	uint32_t plane_surf;
	unsigned gt_changed;
	unsigned pte_bad;
	unsigned guard_bad;
	unsigned back_bad;
	unsigned first_page;
	unsigned pages;
	unsigned display_pages_before;
	int error;
	int pin_rc;
	int unpin_rc;
	int destroy_rc;
	int plane_rc;
	int plane_ok;
	int passed;

	/* Records the GT window's PTEs, which the check must leave alone. */
	so = &i915_aux_scanout;
	i915_aux_record_gt_window(gm);

	/* Claims the display window; EBUSY means the resident display claimed it earlier, which is fine. */
	error = drv_i915_gt_display_window_init(gm, I915_GT_DISPLAY_PAGES);
	kern_logf("i915: SCANOUT-TEST window: rc=%d ggtt_entries=%u gt_window=[%u,+%u) display_window=[%u,+%u) "
		  "gt_pages_in_use=%u objects_live=%u display_pages_in_use=%u\n", error, gm->entries, gm->window_first,
		  gm->window_pages, gm->display_first, gm->display_pages, gm->allocated_pages, gm->objects_live,
		  gm->display_allocated_pages);
	if (error != 0 && error != EBUSY) {
		kern_logf("i915: SCANOUT-TEST verdict: FAIL (display window rc=%d)\n", error);
		return;
	}

	/* Creates the buffer; its backing is 8294400 bytes. */
	display_pages_before = gm->display_allocated_pages;
	error = drv_i915_scanout_create(gm, I915_AUX_SCANOUT_WIDTH, I915_AUX_SCANOUT_HEIGHT, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);
	if (error != 0) {
		kern_logf("i915: SCANOUT-TEST verdict: FAIL (create rc=%d: backing of 8294400 bytes)\n", error);
		return;
	}

	/* Pins it in the display window with its alignment and guards. */
	pin_rc = drv_i915_scanout_pin(so, "scanout-hw-check");
	kern_logf("i915: SCANOUT-TEST buffer: format=XR24 modifier=linear %ux%u cpp=%u pitch=%u (stride units %u) "
		  "size=%u pages=%u align=0x%x guard=%u | pin rc=%d surf=0x%08llx ggtt_page=%u contiguous=%d\n",
		  so->width, so->height, so->cpp, so->pitch, so->stride_units, so->size, so->obj->pages, so->alignment,
		  so->guard_pages, pin_rc, (unsigned long long)so->surf, so->obj->ggtt_page, so->obj->contiguous);
	if (pin_rc != 0) {
		(void)drv_i915_scanout_destroy(so);
		kern_logf("i915: SCANOUT-TEST verdict: FAIL (pin rc=%d)\n", pin_rc);
		return;
	}

	/* Reads every PTE back from the real table, the guards on both sides and the GT window. */
	pte_bad = i915_aux_count_pte_bad(gm, so);
	guard_bad = i915_aux_count_guard_bad(gm, so);
	gt_changed = i915_aux_count_gt_changed(gm);

	/* Draws the LCD-B picture with the CPU, makes it visible to the display and reads it back. */
	first_x = 0u;
	first_y = 0u;
	hash = drv_i915_lcd_pattern_fill(so->cpu, so->pitch, so->width, so->height, I915_AUX_PATTERN_ID);
	drv_i915_scanout_publish(so);
	verify_bad = drv_i915_lcd_pattern_verify(so->cpu, so->pitch, so->width, so->height, I915_AUX_PATTERN_ID, &first_x, &first_y);
	kern_logf("i915: SCANOUT-TEST check: pte_bad=%u/%u guard_bad=%u/%u gt_window_ptes_changed=%u | pattern id=110 "
		  "fnv=%016llx (pinned %016llx) readback_bad=%u publishes=%u | first pte=0x%llx last pte=0x%llx\n",
		  pte_bad, so->obj->pages, guard_bad, so->guard_pages, gt_changed, (unsigned long long)hash,
		  (unsigned long long)I915_AUX_PATTERN_110_FNV, verify_bad, so->publishes,
		  (unsigned long long)drv_i915_gt_ggtt_read_pte(gm, so->obj->ggtt_page),
		  (unsigned long long)drv_i915_gt_ggtt_read_pte(gm, so->obj->ggtt_page + so->obj->pages - 1u));

	/* Computes the plane words for this buffer with Linux's plane writers; nothing is written. */
	plane_ctl = 0u;
	plane_stride = 0u;
	plane_size = 0u;
	plane_color = 0u;
	plane_surf = 0u;
	plane_rc = drv_i915_lcd_emit_plane(lcd_world, 0, 0, so->format, so->modifier, so->width, so->height, so->pitch,
					   (uint32_t)so->surf, &i915_aux_plane_words);
	plane_ok = i915_aux_plane_words_match(so, plane_rc, &plane_ctl, &plane_stride, &plane_size, &plane_color, &plane_surf);
	kern_logf("i915: SCANOUT-TEST plane words (computed; NOT written): rc=%d n=%u PLANE_CTL=0x%08x STRIDE=0x%x SIZE=0x%08x "
		  "COLOR_CTL=0x%x SURF=0x%08x (Linux dump: 0x94000000 0x78 0x0437077f 0x2000, its own surf) match=%d\n",
		  plane_rc, i915_aux_plane_words.n, plane_ctl, plane_stride, plane_size, plane_color, plane_surf, plane_ok);

	/* Releases the buffer in the ordinary order, since nothing scans it out, and reads the PTEs back. */
	first_page = so->obj->ggtt_page;
	pages = so->obj->pages;
	unpin_rc = drv_i915_scanout_unpin(so);
	back_bad = i915_aux_count_not_scratch(gm, first_page, pages);
	destroy_rc = drv_i915_scanout_destroy(so);
	gt_changed += i915_aux_count_gt_changed(gm);

	/* Every PTE, the picture, the release and the plane words must be right. */
	passed = 0;
	if (pte_bad == 0u &&
	    guard_bad == 0u &&
	    gt_changed == 0u &&
	    hash == I915_AUX_PATTERN_110_FNV &&
	    verify_bad == 0u &&
	    unpin_rc == 0 &&
	    destroy_rc == 0 &&
	    back_bad == 0u &&
	    gm->display_allocated_pages == display_pages_before &&
	    plane_ok)
		passed = 1;

	/* Reports the verdict with the release's results. */
	kern_logf("i915: SCANOUT-TEST verdict: %s (unpin=%d destroy=%d ptes_back_to_scratch_bad=%u "
		  "display_pages_in_use=%u (before %u) gt_window_ptes_changed=%u display_pte_writes=%u)\n", passed ? "PASS" : "FAIL",
		  unpin_rc, destroy_rc, back_bad, gm->display_allocated_pages, display_pages_before, gt_changed,
		  gm->display_pte_writes);
}

/* Records every PTE of the GT window. */
static void
i915_aux_record_gt_window(
	const struct i915_gt_mem *gm)
{
	unsigned i;

	/* Reads each entry of the window from the real table. */
	for (i = 0u; i < I915_GT_GGTT_PAGES; i++)
		i915_aux_gt_before[i] = drv_i915_gt_ggtt_read_pte(gm, gm->window_first + i);
}

/* Counts the PTEs of the GT window that differ from the recorded ones. */
static unsigned
i915_aux_count_gt_changed(
	const struct i915_gt_mem *gm)
{
	uint64_t pte;
	unsigned changed;
	unsigned i;

	/* Compares each entry of the window with its record. */
	changed = 0u;
	for (i = 0u; i < I915_GT_GGTT_PAGES; i++) {
		pte = drv_i915_gt_ggtt_read_pte(gm, gm->window_first + i);
		if (pte != i915_aux_gt_before[i])
			changed++;
	}

	/* Succeeded: reports how many changed. */
	return changed;
}

/* Counts the buffer's PTEs that do not map the page's own DMA address as present. */
static unsigned
i915_aux_count_pte_bad(
	const struct i915_gt_mem *gm,
	const struct i915_scanout *so)
{
	uint64_t dma;
	uint64_t pte;
	unsigned bad;
	unsigned i;
	int error;

	/* Compares every entry of the buffer with its backing page. */
	bad = 0u;
	for (i = 0u; i < so->obj->pages; i++) {
		dma = 0u;
		error = drv_i915_gt_object_page_dma(so->obj, i, &dma);
		if (error != 0) {
			bad++;
			continue;
		}

		pte = drv_i915_gt_ggtt_read_pte(gm, so->obj->ggtt_page + i);
		if (pte != ((dma & I915_AUX_PTE_ADDRESS_MASK) | I915_AUX_PTE_PRESENT))
			bad++;
	}

	/* Succeeded: reports how many entries are wrong. */
	return bad;
}

/* Counts the guard pairs around the buffer where either guard does not hold the scratch PTE. */
static unsigned
i915_aux_count_guard_bad(
	const struct i915_gt_mem *gm,
	const struct i915_scanout *so)
{
	uint64_t below;
	uint64_t above;
	unsigned bad;
	unsigned i;

	/* Compares each guard below and above the buffer with the scratch entry. */
	bad = 0u;
	for (i = 0u; i < so->guard_pages; i++) {
		below = drv_i915_gt_ggtt_read_pte(gm, so->obj->ggtt_page - so->guard_pages + i);
		above = drv_i915_gt_ggtt_read_pte(gm, so->obj->ggtt_page + so->obj->pages + i);
		if (below != gm->scratch_pte) {
			bad++;
		} else if (above != gm->scratch_pte) {
			bad++;
		}
	}

	/* Succeeded: reports how many guard pairs are wrong. */
	return bad;
}

/* Counts the entries of a GGTT range that do not hold the scratch PTE. */
static unsigned
i915_aux_count_not_scratch(
	const struct i915_gt_mem *gm,
	unsigned first_page,
	unsigned pages)
{
	uint64_t pte;
	unsigned bad;
	unsigned i;

	/* Compares each entry with the scratch entry. */
	bad = 0u;
	for (i = 0u; i < pages; i++) {
		pte = drv_i915_gt_ggtt_read_pte(gm, first_page + i);
		if (pte != gm->scratch_pte)
			bad++;
	}

	/* Succeeded: reports how many entries still map something. */
	return bad;
}

/*
 * Looks the plane words up and reports whether PLANE_CTL, STRIDE, SIZE and
 * COLOR_CTL equal Linux's dump, PLANE_SURF names the buffer, and PLANE_SURF
 * is the last word.
 */
static int
i915_aux_plane_words_match(
	const struct i915_scanout *so,
	int plane_rc,
	uint32_t *ctl,
	uint32_t *stride,
	uint32_t *size,
	uint32_t *color,
	uint32_t *surf)
{
	const struct i915_lcd_words *words;
	unsigned found;

	/* The plane writers must have succeeded. */
	words = &i915_aux_plane_words;
	if (plane_rc != 0)
		return 0;

	/* PLANE_CTL: enabled, XRGB8888, linear. */
	found = drv_i915_lcd_words_find(words, I915_AUX_PLANE_CTL_1_A, ctl);
	if (found != 1u)
		return 0;
	if (*ctl != I915_AUX_PLANE_CTL_LINUX)
		return 0;

	/* PLANE_STRIDE: the buffer's own stride units, which are Linux's. */
	found = drv_i915_lcd_words_find(words, I915_AUX_PLANE_STRIDE_1_A, stride);
	if (found != 1u)
		return 0;
	if (*stride != so->stride_units)
		return 0;
	if (*stride != I915_AUX_PLANE_STRIDE_LINUX)
		return 0;

	/* PLANE_SIZE: 1920x1080 less one. */
	found = drv_i915_lcd_words_find(words, I915_AUX_PLANE_SIZE_1_A, size);
	if (found != 1u)
		return 0;
	if (*size != I915_AUX_PLANE_SIZE_LINUX)
		return 0;

	/* PLANE_COLOR_CTL as Linux left it. */
	found = drv_i915_lcd_words_find(words, I915_AUX_PLANE_COLOR_CTL_1_A, color);
	if (found != 1u)
		return 0;
	if (*color != I915_AUX_PLANE_COLOR_CTL_LINUX)
		return 0;

	/* PLANE_SURF: this buffer's GGTT offset. */
	found = drv_i915_lcd_words_find(words, I915_AUX_PLANE_SURF_1_A, surf);
	if (found != 1u)
		return 0;
	if (*surf != (uint32_t)so->surf)
		return 0;

	/* PLANE_SURF is written last, since it arms the update. */
	if (words->n < 2u)
		return 0;
	if (words->w[words->n - 1u].reg != I915_AUX_PLANE_SURF_1_A)
		return 0;

	/* Succeeded: the words equal Linux's. */
	return 1;
}
