/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU-free kernel checks of the body of one picture on the panel.
 *
 * The same modeset commits and the same scanout management as on the real
 * GPU run with real kernel DMA allocations, a sentinel-filled table
 * standing in for the GGTT, real mutexes behind the DPLL and backlight
 * locks, and the register and sink models as the display.  Three runs:
 * the picture comes up and everything is given back; an early failure
 * (the sink never reports clock recovery) gives everything back without
 * the buffer ever being shown; a pipe that does not stop leaves the buffer
 * ABANDONED -- pages, DMA mapping, entries, pin and owner all stay,
 * releases are refused, and the outer teardown leaves it alone.  Between
 * them: a second picture in the same lifetime, a failing in-window test,
 * a buffer somebody else prepared, and the full-HD draw's fixture.
 *
 * The model runs use the started display's modeset, watermark and DP
 * worlds.  The DP world is borrowed from the resident panel
 * (edp-ktest.h); the modeset and watermark worlds are set aside for the
 * run and put back afterwards.  The retained-state latch is the display's
 * own, and the part drops it again with the model.
 */

#include "display-ktest.h"
#include "dp-fake-hw.h"
#include "dp-fixture-latitude5330.h"
#include "edp-ktest.h"
#include "lcd-fake-hw.h"
#include <kern/kcrt.h>

#include "../execution/eu-test.h"
#include "../execution/fhd-render.h"
#include "../execution/ktest.h"
#include "../fixtures/draw-fixture.h"

#include "../../display/internal.h"
#include "../../display/modeset-internal.h"
#include "../../display/watermark-internal.h"
#include "../../display/clock.h"
#include "../../display/diagnostics.h"
#include "../../display/dp-sink.h"
#include "../../display/modeset.h"
#include "../../display/scanout.h"
#include "../../display/state.h"
#include "../../display/watermark.h"
#include "../../ggtt.h"
#include "../../i915.h"
#include "../../memory.h"

#include <drivers/generic/dma.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stdint.h>

/* How many entries the stand-in GGTT has. */
#define I915_LCD_SHOW_KTEST_TABLE_ENTRIES	16384U

/* What an entry nobody wrote holds, and the scratch encoding of a free entry. */
#define I915_LCD_SHOW_KTEST_SENTINEL		0x5a5a5a5a5a5a5a5aULL
#define I915_LCD_SHOW_KTEST_SCRATCH_PTE		0x00000000dead0001ULL

/* The asymmetric picture of the run and its hash at 1920x1080. */
#define I915_LCD_SHOW_KTEST_PATTERN		110U
#define I915_LCD_SHOW_KTEST_PATTERN_FNV		0xce63f20b23f91f85ULL

/* The picture of the second run, and the picture of the prepared buffer. */
#define I915_LCD_SHOW_KTEST_PATTERN_AGAIN	111U
#define I915_LCD_SHOW_KTEST_PATTERN_PREPARED	113U

/* The panel's raw clock (kHz), the combo PLL's reference (kHz) and the VBT colour depth. */
#define I915_LCD_SHOW_KTEST_RAWCLK_KHZ		19200U
#define I915_LCD_SHOW_KTEST_REF_KHZ		38400
#define I915_LCD_SHOW_KTEST_VBT_BPP		18

/* How many pages a full-HD XRGB8888 buffer has. */
#define I915_LCD_SHOW_KTEST_FHD_PAGES		2025U

/* The draw fixture's batch and state capacity in dwords. */
#define I915_LCD_SHOW_KTEST_DWORDS		1024U

/* The GPU address of the draw's state page (the compute test's shared page). */
#define I915_LCD_SHOW_KTEST_STATE_VA		0x100400000ULL

/* The drawing-rectangle word of a 1920x1080 target (1919, 1079). */
#define I915_LCD_SHOW_KTEST_RECT_MAX		0x0437077fU

/*
 * The modeset and watermark worlds of the started display, set aside while
 * the model runs in them.
 *
 * It lives on the part's stack from the save to the restore; both copies
 * are allocated by the save and freed by the restore.
 */
struct i915_lcd_show_ktest_saved {
	/* The contents of the modeset world. */
	struct i915_lcd_world *lcd;

	/* The contents of the watermark world. */
	struct i915_wm_world *wm;
};

/*
 * The stand-in GGTT and the GT memory over it.
 *
 * They are prepared at the start of the part; the outer teardown check
 * finalises the memory.  The memory holds the object pool, too large for
 * the stack.
 */
static uint64_t i915_lcd_show_ktest_table[I915_LCD_SHOW_KTEST_TABLE_ENTRIES];
static struct i915_gt_mem i915_lcd_show_ktest_gm;

/*
 * The scanout storage of the runs: the picture that is given back, the
 * second picture and the in-window failure, the prepared buffer, the early
 * failure, and the abandoned buffer.
 *
 * Each storage must start zeroed; the abandoned one keeps its buffer for
 * ever, because the display may still read it.
 */
static struct i915_scanout i915_lcd_show_ktest_so_a;
static struct i915_scanout i915_lcd_show_ktest_so_a2;
static struct i915_scanout i915_lcd_show_ktest_so_p;
static struct i915_scanout i915_lcd_show_ktest_so_b;
static struct i915_scanout i915_lcd_show_ktest_so_c;

/*
 * The register and sink models, the DP environment and what the eDP
 * bring-up found.
 *
 * Each bring-up starts them afresh; they are static because the eDP and
 * the modeset keep pointers into them for the length of a run.
 */
static struct i915_dp_fake_hw i915_lcd_show_ktest_dpf;
static struct i915_lcd_fake_hw i915_lcd_show_ktest_lcd;
static struct i915_dp_env i915_lcd_show_ktest_env;
static struct i915_edp_result i915_lcd_show_ktest_edp;

/*
 * The panel state, the show environment and the report of the last run.
 *
 * Each bring-up rebuilds the state and the environment; the report is
 * filled by every run and read by the checks after it.
 */
static struct i915_lcd_state i915_lcd_show_ktest_state;
static struct i915_lcd_show_env i915_lcd_show_ktest_show;
static struct i915_lcd_show_report i915_lcd_show_ktest_rep;

/*
 * The DP world the model eDP lives in: the one borrowed from the resident
 * panel for the length of the part.
 */
static struct i915_dp_world *i915_lcd_show_ktest_world;

/*
 * The real kernel mutexes behind the model's DPLL and backlight locks
 * (I915_LCD_LOCK_*), and how many lock operations reached them.
 *
 * They are initialised at the start of the part; the count is reset before
 * the run whose locking is checked.
 */
static struct mutex i915_lcd_show_ktest_locks[2];
static unsigned i915_lcd_show_ktest_lock_ops;

/*
 * The model's own lock hook, which keeps the model's accounting; the part's
 * hook takes the real mutex around it.  Set by each bring-up.
 */
static void (*i915_lcd_show_ktest_model_lock)(void *ctx, int which, int take);

static int i915_lcd_show_ktest_dma_create(struct drv_dma_device **dma);
static int i915_lcd_show_ktest_worlds_save(struct i915_display *display, struct i915_lcd_show_ktest_saved *saved);
static void i915_lcd_show_ktest_worlds_restore(struct i915_display *display, struct i915_lcd_show_ktest_saved *saved);
static void i915_lcd_show_ktest_skip_model(struct i915_ktest *ktest, const char *reason);
static void i915_lcd_show_ktest_locked(void *ctx, int which, int take);
static int i915_lcd_show_ktest_bring_up(struct i915_display *display, struct i915_scanout *so);
static void i915_lcd_show_ktest_fill_cfg(struct i915_lcd_modeset_cfg *cfg);
static int i915_lcd_show_ktest_edp_released(void);
static unsigned i915_lcd_show_ktest_live_ptes(unsigned first, unsigned pages);
static int i915_lcd_show_ktest_fail_in_window(void *ctx, struct i915_lcd_observer *o);
static void i915_lcd_show_ktest_stick_the_pipe(void *ctx, int stage);
static void i915_lcd_show_ktest_model_runs(struct i915_ktest *ktest, struct i915_display *display);
static void i915_lcd_show_ktest_picture(struct i915_ktest *ktest, struct i915_display *display);
static void i915_lcd_show_ktest_again(struct i915_ktest *ktest, struct i915_display *display);
static void i915_lcd_show_ktest_fixture(struct i915_ktest *ktest);
static void i915_lcd_show_ktest_verify(struct i915_ktest *ktest);
static void i915_lcd_show_ktest_prepared(struct i915_ktest *ktest, struct i915_display *display);
static void i915_lcd_show_ktest_not_started(struct i915_ktest *ktest, struct i915_display *display);
static void i915_lcd_show_ktest_early_failure(struct i915_ktest *ktest, struct i915_display *display);
static void i915_lcd_show_ktest_stuck(struct i915_ktest *ktest, struct i915_display *display);
static void i915_lcd_show_ktest_abandoned(struct i915_ktest *ktest, struct i915_display *display);

/*
 * The checks of the model runs, reported as skipped when the model cannot
 * run on the started device.
 */
static const char *const i915_lcd_show_ktest_model_checks[] = {
	"lcd-show: A-PASS buffer -> prepare -> enable commit -> frames -> window -> disable commit -> stop confirmed -> released",
	"lcd-show: A-BUFFER the plane was armed with THIS object's GGTT address; the picture is the pinned pattern before and after",
	"lcd-show: A-OBSERVED the sink's link status, the advancing frame counter during the window, the standing counter after the stop",
	"lcd-show: A-STATUS underrun status sampled at every point of both commits and in the window; vblank stayed masked; no unresolved step",
	"lcd-show: A-RETURNED no model violation; power references, DBUF slices, real mutexes and the eDP all back to the start state",
	"lcd-show: A-MEMORY the scanout object and its GGTT range are gone",
	"lcd-show: A2-AGAIN a second show / stop in the same lifetime with another picture passes and gives everything back",
	"lcd-show: A2-HOOK a failing in-window test is the first anomaly; the stop path still runs and gives everything back",
	"lcd-g: G-PREPARED an unpinned buffer is refused; the buffer is left as it was",
	"lcd-g: G-PREPARED shown and stopped; the buffer comes back PINNED to its owner with its pixels untouched",
	"lcd-g: G-PREPARED the OWNER releases it afterwards",
	"lcd-g: G-NOTSTARTED prepare refused: display_acquired=0 (not 'stop confirmed'), no register written, nothing retained",
	"lcd-g: G-NOTSTARTED the owner reclaims the buffer at once, and the next run is not blocked",
	"lcd-show: B-FAIL the sink never reports clock recovery: no success, the first anomaly is that one, the plane was never armed",
	"lcd-show: B-CLEANUP the reference's disable path ran; its result is recorded apart and did not replace the first anomaly",
	"lcd-show: C-STUCK a pipe that does not stop: the run ends ABANDONED; the anomaly is recorded at the disable, with the cleanup's own error index",
	"lcd-show: C-KEPT backing pages (still holding the picture), CPU mapping, every PTE, the pin and its owner are all still there",
	"lcd-show: C-REFUSED unpin and destroy are refused for an abandoned buffer",
	"lcd-show: C-HELD DC_OFF, the crtc's power domains and the DBUF slices were not taken from under the pipe",
	"lcd-show: C-NEXT a further run on the abandoned storage is refused before anything is touched",
	"lcd-show: C-LATCH a re-call with fresh storage is refused before anything is allocated; the latch stays set",
	"lcd-show: C-CREATE create on an abandoned object is refused and leaves its record untouched",
	"lcd-show: C-BELOW the object layer below the scanout wrapper refuses to destroy / unbind the kept object too",
	"lcd-show: C-TEARDOWN drv_i915_gt_mem_fini() leaves the abandoned buffer alone: PTEs live, pages readable, object kept",
	"lcd-show: C-DISCARD the latch outlives the teardown; only discarding the model run clears it"
};

/*
 * Checks the body of one picture on the panel on the register and sink
 * models.
 *
 * The draw fixture's checks need no display; the model runs borrow the
 * started display's worlds and are skipped when that is not possible.
 */
void
drv_i915_display_ktest_lcd_show(
	struct i915_ktest *ktest)
{
	struct drv_dma_device *dma;
	struct i915_display *display;
	unsigned index;
	int error;

	/* Creates the DMA device the backing pages come from. */
	dma = NULL;
	error = i915_lcd_show_ktest_dma_create(&dma);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "lcd-show: SETUP gt_mem");
		return;
	}

	/* Marks every entry of the stand-in GGTT as not written. */
	for (index = 0U; index < I915_LCD_SHOW_KTEST_TABLE_ENTRIES; index++)
		i915_lcd_show_ktest_table[index] = I915_LCD_SHOW_KTEST_SENTINEL;

	/* The real mutexes behind the model's locks. */
	(void)mutex_init(&i915_lcd_show_ktest_locks[I915_LCD_LOCK_DPLL], LOCK_RANK_DEVICE, "lcd-show-ktest-dpll");
	(void)mutex_init(&i915_lcd_show_ktest_locks[I915_LCD_LOCK_BACKLIGHT], LOCK_RANK_DEVICE, "lcd-show-ktest-backlight");

	/* Prepares the GT memory over the stand-in GGTT. */
	error = drv_i915_gt_mem_init(&i915_lcd_show_ktest_gm, dma, I915_DMA_MAX_ADDRESS, i915_lcd_show_ktest_table, I915_LCD_SHOW_KTEST_TABLE_ENTRIES, I915_LCD_SHOW_KTEST_SCRATCH_PTE, NULL);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "lcd-show: SETUP gt_mem");
		(void)drv_dma_device_destroy(dma);
		return;
	}

	/* The full-HD draw's generated state, address layout and verifier. */
	i915_lcd_show_ktest_fixture(ktest);

	/* The runs on the models need the started display. */
	display = NULL;
	if (ktest->device != NULL)
		display = ktest->device->display;

	/* Runs them, or names each of their checks as not run. */
	if (display != NULL &&
	    display->lcd_world != NULL &&
	    display->wm_world != NULL) {
		i915_lcd_show_ktest_model_runs(ktest, display);
	} else {
		i915_lcd_show_ktest_skip_model(ktest, "the device has no display with modeset worlds");
		drv_i915_gt_mem_fini(&i915_lcd_show_ktest_gm);
	}

	/*
	 * Gives the DMA device back.  An abandoned buffer stays allocated for
	 * ever, so the device may stay open.
	 */
	error = drv_dma_device_destroy(dma);
	if (error != 0)
		kern_logf("i915: lcd-show ktest: DMA device kept (error %d): it still holds the buffer abandoned on purpose\n", error);
}

/* Creates a DMA device with the GPU's DMA constraints. */
static int
i915_lcd_show_ktest_dma_create(
	struct drv_dma_device **dma)
{
	static const struct drv_dma_constraints constraints = {
		39U,
		0xffffffffU,
		0U,
		1
	};
	int error;

	/* Declares the GPU's 39-bit DMA capability, as the device start does. */
	error = drv_dma_device_create(&constraints, dma);
	if (error != 0)
		return error;

	/* Succeeded: the device hands out pages the GPU can address. */
	return 0;
}

/* Sets the display's modeset and watermark worlds aside: 0, or ENOMEM. */
static int
i915_lcd_show_ktest_worlds_save(
	struct i915_display *display,
	struct i915_lcd_show_ktest_saved *saved)
{
	/* Allocates the copy of the modeset world. */
	saved->lcd = kern_malloc(sizeof(*saved->lcd));
	if (saved->lcd == NULL)
		return ENOMEM;

	/* Allocates the copy of the watermark world. */
	saved->wm = kern_malloc(sizeof(*saved->wm));
	if (saved->wm == NULL) {
		kern_free(saved->lcd);
		saved->lcd = NULL;
		return ENOMEM;
	}

	/* Copies both worlds as the display start left them. */
	kern_memcpy(saved->lcd, display->lcd_world, sizeof(*saved->lcd));
	kern_memcpy(saved->wm, display->wm_world, sizeof(*saved->wm));

	/* Succeeded: the model may change the worlds. */
	return 0;
}

/* Puts the display's modeset and watermark worlds back and frees the copies. */
static void
i915_lcd_show_ktest_worlds_restore(
	struct i915_display *display,
	struct i915_lcd_show_ktest_saved *saved)
{
	/*
	 * The worlds get their bytes back at the same addresses, so the
	 * pointers they hold into themselves are valid again.
	 */
	kern_memcpy(display->lcd_world, saved->lcd, sizeof(*saved->lcd));
	kern_memcpy(display->wm_world, saved->wm, sizeof(*saved->wm));

	/* Frees the copies. */
	kern_free(saved->lcd);
	kern_free(saved->wm);
	saved->lcd = NULL;
	saved->wm = NULL;
}

/* Names each check of the model runs as not run, with the reason. */
static void
i915_lcd_show_ktest_skip_model(
	struct i915_ktest *ktest,
	const char *reason)
{
	unsigned count;
	unsigned index;

	/* One skip line per check. */
	count = sizeof(i915_lcd_show_ktest_model_checks) / sizeof(i915_lcd_show_ktest_model_checks[0]);
	for (index = 0U; index < count; index++)
		drv_i915_ktest_skip(ktest, i915_lcd_show_ktest_model_checks[index], reason);
}

/* Takes or drops the real mutex around the model's own lock accounting. */
static void
i915_lcd_show_ktest_locked(
	void *ctx,
	int which,
	int take)
{
	int real;

	/* Only the DPLL and backlight locks have a real mutex. */
	real = 0;
	if (which >= 0 && which <= 1)
		real = 1;

	/* Takes the real mutex first, and counts every operation on it. */
	if (real) {
		/* A take waits for the mutex; a drop releases it below. */
		if (take)
			mutex_lock(&i915_lcd_show_ktest_locks[which]);

		i915_lcd_show_ktest_lock_ops++;
	}

	/* The model keeps its own accounting. */
	i915_lcd_show_ktest_model_lock(ctx, which, take);

	/* Drops the real mutex last. */
	if (real && !take)
		mutex_unlock(&i915_lcd_show_ktest_locks[which]);
}

/* Brings a fresh device up on the models for one run into so: 0, or the first failure. */
static int
i915_lcd_show_ktest_bring_up(
	struct i915_display *display,
	struct i915_scanout *so)
{
	struct i915_edp_config config;
	struct i915_lcd_show_env *show;
	struct i915_lcd_fake_hw *lcd;
	int error;

	show = &i915_lcd_show_ktest_show;
	lcd = &i915_lcd_show_ktest_lcd;

	/* A fresh device for this case: the shared DPLLs and the DBUF state start over. */
	drv_i915_lcd_dplls_reset(display->lcd_world);
	drv_i915_lcd_dbuf_forget(display->wm_world);

	/* The panel's VBT power sequence and raw clock; the eDP logs errors only. */
	kern_memset(&config, 0, sizeof(config));
	config.rawclk_khz = I915_LCD_SHOW_KTEST_RAWCLK_KHZ;
	config.t1_t3 = 2000;
	config.t8 = 800;
	config.t9 = 2000;
	config.t10 = 1100;
	config.t11_t12 = 5000;
	config.log_level = -1;

	/* The sink model holds the laptop panel's DPCD and EDID. */
	drv_i915_dp_fake_init(&i915_lcd_show_ktest_dpf, i915_dp_fixture_dpcd_000, i915_dp_fixture_dpcd_100, i915_dp_fixture_dpcd_700, i915_dp_fixture_edid, 128U);
	drv_i915_dp_fake_bind_env(&i915_lcd_show_ktest_dpf, &i915_lcd_show_ktest_env, i915_lcd_show_ktest_world);

	/* Brings the eDP up to its DPCD and EDID. */
	error = drv_i915_edp_begin(i915_lcd_show_ktest_world, &i915_lcd_show_ktest_env, &config, &i915_lcd_show_ktest_edp);
	if (error != 0)
		return error;

	/* Finishes the eDP's late initialisation. */
	error = drv_i915_edp_init_late(i915_lcd_show_ktest_world, &config, &i915_lcd_show_ktest_edp);
	if (error != 0)
		return error;

	/* Computes the mode, the link and the PLL words from what the sink reported. */
	error = drv_i915_lcd_compute(display->lcd_world,
				     i915_lcd_show_ktest_edp.edid,
				     i915_lcd_show_ktest_edp.dpcd,
				     i915_lcd_show_ktest_edp.edp_dpcd,
				     I915_LCD_SHOW_KTEST_VBT_BPP,
				     I915_LCD_SHOW_KTEST_REF_KHZ,
				     &i915_lcd_show_ktest_state);
	if (error != 0)
		return error;

	/* The register model of pipe A, port A and DPLL 0, its locks behind real mutexes. */
	drv_i915_lcd_fake_init(lcd, &i915_lcd_show_ktest_dpf, 0, 0, 0, i915_lcd_show_ktest_state.mode.vtotal);
	lcd->dbuf_size = 4096U;
	i915_lcd_show_ktest_model_lock = lcd->ops.lock;
	lcd->ops.lock = i915_lcd_show_ktest_locked;

	/* The show environment: the model, the memory, the storage and the picture. */
	kern_memset(show, 0, sizeof(*show));
	show->hw = &lcd->ops;
	show->gm = &i915_lcd_show_ktest_gm;
	show->so = so;
	show->lcd = &i915_lcd_show_ktest_state;
	show->pipe = 0;
	show->pattern_id = I915_LCD_SHOW_KTEST_PATTERN;
	show->pattern_fnv = I915_LCD_SHOW_KTEST_PATTERN_FNV;
	show->first_frames_ms = 1000U;
	show->window_ms = 2000U;

	/* The configuration the normal initialisation would have read. */
	i915_lcd_show_ktest_fill_cfg(&show->cfg);

	/* Succeeded: a run can start. */
	return 0;
}

/* Fills the modeset configuration as the normal initialisation of this machine reads it. */
static void
i915_lcd_show_ktest_fill_cfg(
	struct i915_lcd_modeset_cfg *cfg)
{
	static const uint16_t latency[8] = {
		3,
		54,
		83,
		102,
		147,
		147,
		144,
		144
	};

	/* The sink's capabilities from the eDP. */
	kern_memcpy(cfg->dpcd, i915_lcd_show_ktest_edp.dpcd, sizeof(cfg->dpcd));
	kern_memcpy(cfg->edp_dpcd, i915_lcd_show_ktest_edp.edp_dpcd, sizeof(cfg->edp_dpcd));

	/* The watermark latencies, IPC and the SAGV block time. */
	kern_memcpy(cfg->wm_latency, latency, sizeof(latency));
	cfg->wm_num_levels = 6;
	cfg->wm_ipc_enabled = 1;
	cfg->sagv_block_time_us = 35;

	/* Four DBUF slices, of which slice 1 is on. */
	cfg->dbuf_size = 4096U;
	cfg->dbuf_slice_mask = 0x0f;
	cfg->dbuf_enabled_slices = 0x01;

	/* The DMC firmware of pipe A is loaded. */
	cfg->dmc_fw_mask = 1U << 1;

	/* The VBT backlight: PWM at 200 Hz, minimum 6. */
	cfg->vbt_backlight_present = 1;
	cfg->vbt_backlight_pwm_freq_hz = 200;
	cfg->vbt_backlight_min_brightness = 6;
	cfg->rawclk_khz = I915_LCD_SHOW_KTEST_RAWCLK_KHZ;

	/* The CDCLK the initialisation left and the allowed QGV bandwidth. */
	cfg->cdclk_khz = 179200U;
	cfg->cdclk_vco_khz = 537600U;
	cfg->cdclk_ref_khz = 38400U;
	cfg->cdclk_bypass_khz = 19200U;
	cfg->cdclk_max_khz = 652800U;
	cfg->qgv_allowed_bw = 11707U;
}

/* Ends the model eDP and reports whether nothing is held any more. */
static int
i915_lcd_show_ktest_edp_released(void)
{
	struct i915_lcd_fake_hw *lcd;
	struct i915_dp_fake_hw *dpf;
	int ended;

	lcd = &i915_lcd_show_ktest_lcd;
	dpf = &i915_lcd_show_ktest_dpf;

	/* Ends the eDP and runs its pending asynchronous power releases. */
	ended = drv_i915_edp_end(i915_lcd_show_ktest_world, &i915_lcd_show_ktest_edp);
	drv_i915_dp_fake_flush_async(dpf);

	/* No lock, DP power reference or panel power is left. */
	if (ended != 0)
		return 0;
	if (lcd->lock_held[0] != 0 || lcd->lock_held[1] != 0)
		return 0;
	if (dpf->refs_core != 0 || dpf->refs_aux != 0)
		return 0;
	if ((dpf->pp_control & 9U) != 0U)
		return 0;

	/* Succeeded: the eDP gave everything back. */
	return 1;
}

/* Counts the entries of a range that map a page (present, neither scratch nor unwritten). */
static unsigned
i915_lcd_show_ktest_live_ptes(
	unsigned first,
	unsigned pages)
{
	uint64_t pte;
	unsigned index;
	unsigned count;

	/* Looks at each entry of the range. */
	count = 0U;
	for (index = 0U; index < pages; index++) {
		pte = i915_lcd_show_ktest_table[first + index];

		/* A present entry that is neither the scratch encoding nor the sentinel maps a page. */
		if ((pte & 1ULL) != 0U &&
		    pte != I915_LCD_SHOW_KTEST_SCRATCH_PTE &&
		    pte != I915_LCD_SHOW_KTEST_SENTINEL)
			count++;
	}

	/* Reports how many entries still map a page. */
	return count;
}

/* The in-window test of the second run: it fails. */
static int
i915_lcd_show_ktest_fail_in_window(
	void *ctx,
	struct i915_lcd_observer *o)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(o);

	/* Reports a failed test. */
	return EIO;
}

/* The stage hook of the stuck run: the pipe stops obeying once the window has passed. */
static void
i915_lcd_show_ktest_stick_the_pipe(
	void *ctx,
	int stage)
{
	UNUSED_PARAMETER(ctx);

	/* From the end of the window on, TRANSCONF's state bit never clears. */
	if (stage == I915_LCD_SHOW_WINDOW_DONE)
		i915_lcd_show_ktest_lcd.fault_pipe_stuck_on = 1;
}

/* Runs every model run in the borrowed worlds of the started display. */
static void
i915_lcd_show_ktest_model_runs(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_lcd_show_ktest_saved saved;
	struct i915_edp_ktest_world borrow;
	int error;

	/* Borrows the DP world from the resident panel. */
	error = drv_i915_display_ktest_edp_world_enter(ktest, &borrow);
	if (error != 0) {
		i915_lcd_show_ktest_skip_model(ktest, "the DP world could not be borrowed");
		drv_i915_gt_mem_fini(&i915_lcd_show_ktest_gm);
		return;
	}

	/* Sets the modeset and watermark worlds aside. */
	error = i915_lcd_show_ktest_worlds_save(display, &saved);
	if (error != 0) {
		drv_i915_display_ktest_edp_world_leave(&borrow);
		i915_lcd_show_ktest_skip_model(ktest, "no memory to set the modeset worlds aside");
		drv_i915_gt_mem_fini(&i915_lcd_show_ktest_gm);
		return;
	}

	i915_lcd_show_ktest_world = borrow.world;

	/* The picture comes up and everything is given back; then again, and with a failing in-window test. */
	i915_lcd_show_ktest_picture(ktest, display);
	i915_lcd_show_ktest_again(ktest, display);

	/* A buffer somebody else prepared, shown and not shown. */
	i915_lcd_show_ktest_prepared(ktest, display);
	i915_lcd_show_ktest_not_started(ktest, display);

	/* An early failure: nothing is shown and everything is given back. */
	i915_lcd_show_ktest_early_failure(ktest, display);

	/* The pipe does not stop: the buffer is abandoned and nothing later frees it. */
	i915_lcd_show_ktest_stuck(ktest, display);
	i915_lcd_show_ktest_abandoned(ktest, display);

	/* Puts the worlds back as the display start left them. */
	i915_lcd_show_ktest_world = NULL;
	i915_lcd_show_ktest_worlds_restore(display, &saved);
	drv_i915_display_ktest_edp_world_leave(&borrow);
}

/* Checks run A: the picture comes up, stays for the window, and everything is given back. */
static void
i915_lcd_show_ktest_picture(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_lcd_show_report *rep;
	struct i915_lcd_fake_hw *lcd;
	unsigned objects_before;
	unsigned violations;
	unsigned steps;
	unsigned dropped;
	int power_refs;
	int released;
	int passed;
	int error;

	rep = &i915_lcd_show_ktest_rep;
	lcd = &i915_lcd_show_ktest_lcd;
	objects_before = i915_lcd_show_ktest_gm.objects_live;

	/* Brings the models up and runs the show body. */
	error = i915_lcd_show_ktest_bring_up(display, &i915_lcd_show_ktest_so_a);
	i915_lcd_show_ktest_lock_ops = 0U;
	if (error == 0)
		error = drv_i915_lcd_show_run(display, &i915_lcd_show_ktest_show, rep);

	/* The whole way through to the release, with no anomaly. */
	passed = 0;
	if (error == 0 &&
	    rep->pass &&
	    rep->stage == I915_LCD_SHOW_RELEASED &&
	    rep->first_anomaly == NULL &&
	    rep->released &&
	    !rep->abandoned)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: A-PASS buffer -> prepare -> enable commit -> frames -> window -> disable commit -> stop confirmed -> released");

	/* The plane was armed once with this buffer, which held the pinned picture before and after. */
	passed = 0;
	if (rep->pattern_hash == I915_LCD_SHOW_KTEST_PATTERN_FNV &&
	    rep->readback_bad_before == 0U &&
	    rep->readback_bad_after == 0U &&
	    lcd->plane_surf_at_arm == (uint32_t)rep->surf &&
	    rep->surf != 0U &&
	    lcd->plane_arms == 1U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: A-BUFFER the plane was armed with THIS object's GGTT address; the picture is the pinned pattern before and after");

	/* The sink's link, the frames of the window and the standing counter after the stop. */
	passed = 0;
	if (rep->at_enable.cr_ok &&
	    rep->at_enable.eq_ok &&
	    rep->at_window_end.cr_ok &&
	    rep->first_frames_rc == 0 &&
	    rep->steady_rc == 0 &&
	    rep->steady_rounds == 4U &&
	    rep->steady_frame_last > rep->frame_first &&
	    rep->stopped_rc == 0 &&
	    (rep->transconf_after_stop & 0xc0000000U) == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: A-OBSERVED the sink's link status, the advancing frame counter during the window, the standing counter after the stop");

	/* Reads the run log's unresolved steps and dropped entries. */
	steps = 1U;
	dropped = 1U;
	if (rep->trace != NULL) {
		steps = rep->trace->steps;
		dropped = rep->trace->dropped;
	}

	/* No underrun at any sample, vblank masked throughout, every step resolved. */
	passed = 0;
	if (rep->obs.seen_transition == 0U &&
	    rep->obs.seen_steady == 0U &&
	    rep->obs.vblank_unmasked_seen == 0 &&
	    rep->obs.n >= 12U &&
	    rep->obs.dropped == 0U &&
	    steps == 0U &&
	    dropped == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: A-STATUS underrun status sampled at every point of both commits and in the window; vblank stayed masked; no unresolved step");

	/* Reads the model's accounting and ends the eDP. */
	violations = drv_i915_lcd_fake_violations(lcd);
	power_refs = drv_i915_lcd_fake_power_refs_total(lcd);
	released = i915_lcd_show_ktest_edp_released();

	/* Everything is back to the start state. */
	passed = 0;
	if (violations == 0U &&
	    power_refs == 0 &&
	    lcd->dbuf_enabled == 0x01U &&
	    rep->at_disable.crtc_domains_held == 0U &&
	    !rep->at_disable.dc_off_held &&
	    i915_lcd_show_ktest_lock_ops >= 4U &&
	    released)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: A-RETURNED no model violation; power references, DBUF slices, real mutexes and the eDP all back to the start state");

	/* The buffer and its GGTT range went back. */
	passed = 0;
	if (i915_lcd_show_ktest_gm.objects_live == objects_before &&
	    i915_lcd_show_ktest_gm.display_allocated_pages == 0U &&
	    i915_lcd_show_ktest_so_a.state == I915_SCANOUT_NONE)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: A-MEMORY the scanout object and its GGTT range are gone");
}

/* Checks run A2: another picture in the same lifetime, then a failing in-window test. */
static void
i915_lcd_show_ktest_again(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_lcd_show_report *rep;
	unsigned objects_before;
	int power_refs;
	int released;
	int passed;
	int error;

	rep = &i915_lcd_show_ktest_rep;
	objects_before = i915_lcd_show_ktest_gm.objects_live;

	/* The same body again with another picture, not compared with a pinned hash. */
	error = i915_lcd_show_ktest_bring_up(display, &i915_lcd_show_ktest_so_a2);
	i915_lcd_show_ktest_show.pattern_id = I915_LCD_SHOW_KTEST_PATTERN_AGAIN;
	i915_lcd_show_ktest_show.pattern_fnv = 0U;
	if (error == 0)
		error = drv_i915_lcd_show_run(display, &i915_lcd_show_ktest_show, rep);
	released = i915_lcd_show_ktest_edp_released();

	/* It passes and gives everything back. */
	passed = 0;
	if (error == 0 &&
	    rep->pass &&
	    rep->released &&
	    i915_lcd_show_ktest_gm.objects_live == objects_before &&
	    i915_lcd_show_ktest_gm.display_allocated_pages == 0U &&
	    released)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: A2-AGAIN a second show / stop in the same lifetime with another picture passes and gives everything back");

	/* Once more, with an in-window test that fails. */
	error = i915_lcd_show_ktest_bring_up(display, &i915_lcd_show_ktest_so_a2);
	i915_lcd_show_ktest_show.in_window = i915_lcd_show_ktest_fail_in_window;
	if (error == 0) {
		error = drv_i915_lcd_show_run(display, &i915_lcd_show_ktest_show, rep);
	} else {
		error = 0;
	}
	power_refs = drv_i915_lcd_fake_power_refs_total(&i915_lcd_show_ktest_lcd);
	released = i915_lcd_show_ktest_edp_released();

	/* The failure is the first anomaly, and the stop path still gave everything back. */
	passed = 0;
	if (error != 0 &&
	    rep->window_hook_rc == EIO &&
	    rep->first_anomaly != NULL &&
	    rep->first_anomaly_stage == I915_LCD_SHOW_PICTURE_UP &&
	    rep->steady_rounds == 0U &&
	    rep->released &&
	    power_refs == 0 &&
	    released)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: A2-HOOK a failing in-window test is the first anomaly; the stop path still runs and gives everything back");
	i915_lcd_show_ktest_show.in_window = NULL;
}

/* Checks the full-HD draw's address layout, generated state and batch, and its verifier. */
static void
i915_lcd_show_ktest_fixture(
	struct i915_ktest *ktest)
{
	static uint32_t state[I915_LCD_SHOW_KTEST_DWORDS];
	static uint32_t batch[I915_LCD_SHOW_KTEST_DWORDS];
	const struct i915_test_fhd_va *layout;
	const uint32_t *rss;
	unsigned count;
	unsigned dwords;
	unsigned index;
	unsigned rectangles;
	uint32_t mocs;
	unsigned ps_bytes;
	int same_texture;
	int passed;
	int error;

	/* Reads the draw's layout table and its overlap check. */
	layout = NULL;
	count = 0U;
	error = drv_i915_test_fhd_va_layout(&layout, &count);

	/* State, batch, texture and two render targets, each of at least the full-HD size. */
	passed = 0;
	if (error == 0 &&
	    layout != NULL &&
	    count == 5U &&
	    layout[3].va == I915_TEX_FHD_RT_VA &&
	    layout[3].len >= I915_TEX_FHD_RT_BYTES &&
	    layout[4].va == I915_TEX_FHD_RT_B_VA &&
	    layout[4].len >= I915_TEX_FHD_RT_BYTES)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-VA the draw's GPU VA ranges (state, batch, texture, two 8 MB render targets) are aligned and do not overlap");

	/* Reads the generated render-target state and what the texture state shares with the 8x8 draw. */
	rss = drv_i915_tex_fixture_fhd_rt_rss();
	same_texture = drv_i915_tex_fixture_fhd_same_texture_state();
	mocs = drv_i915_draw_fixture_mocs();
	ps_bytes = drv_i915_tex_fixture_fhd_ps_bytes();

	/* A 1920x1080 B8G8R8A8 target, pitch 7680, uncached MOCS, and the same texture, sampler and packets. */
	passed = 0;
	if (same_texture &&
	    rss[2] == I915_LCD_SHOW_KTEST_RECT_MAX &&
	    rss[3] == I915_TEX_FHD_PITCH - 1U &&
	    ((rss[0] >> 18) & 0x1ffU) == 0xc0U &&
	    ((rss[1] >> 24) & 0x7fU) == mocs &&
	    ps_bytes <= 1024U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-STATE isl render target 1920x1080 B8G8R8A8 pitch 7680 MOCS uncached; texture / sampler / packets = the T1 ones");

	/* Writes the state page and builds the batch. */
	drv_i915_tex_fixture_fhd_write_state(state, I915_TEX_FHD_RT_VA, I915_TEX_FIXTURE_TEX_VA, mocs);
	dwords = drv_i915_tex_fixture_fhd_build_batch(batch, I915_LCD_SHOW_KTEST_DWORDS, I915_LCD_SHOW_KTEST_STATE_VA, mocs);

	/* Counts the drawing rectangles: a zero word followed by the full-HD maximum. */
	rectangles = 0U;
	for (index = 0U; index + 1U < dwords; index++) {
		/* A rectangle from (0, 0) to (1919, 1079). */
		if (batch[index + 1U] == I915_LCD_SHOW_KTEST_RECT_MAX && batch[index] == 0U)
			rectangles++;
	}

	/* One rectangle, the RECTLIST vertices (1920, 1080), and the target state naming the 8 MB range. */
	passed = 0;
	if (dwords > 0U &&
	    dwords < I915_LCD_SHOW_KTEST_DWORDS &&
	    rectangles == 1U &&
	    state[2048U / 4U] == 0x44f00000U &&
	    state[2048U / 4U + 1U] == 0x44870000U &&
	    state[64U / 4U + 8U] == (uint32_t)I915_TEX_FHD_RT_VA &&
	    state[64U / 4U + 9U] == (uint32_t)(I915_TEX_FHD_RT_VA >> 32))
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-BATCH drawing rectangle 1919x1079, RECTLIST (1920,1080), render target state names the 8 MB range");

	/* The verifier on a full-HD image the CPU drew. */
	i915_lcd_show_ktest_verify(ktest);
}

/* Checks that the full-HD verifier counts every wrong pixel of an image, and only those. */
static void
i915_lcd_show_ktest_verify(
	struct i915_ktest *ktest)
{
	static uint8_t pattern[I915_TEX_FIXTURE_TEX_BYTES];
	struct i915_gt_object *image;
	uint32_t *pixels;
	uint32_t x;
	uint32_t y;
	uint32_t bad_before;
	uint32_t bad_after;
	int passed;

	/* An object the size of the render target. */
	image = drv_i915_gt_object_create(&i915_lcd_show_ktest_gm, I915_TEX_FHD_RT_BYTES);
	bad_before = 1U;
	bad_after = 0U;
	if (image != NULL) {
		pixels = (uint32_t *)image->cpu;

		/* Draws the expected image: each texel covers 240 x 135 pixels. */
		drv_i915_tex_fixture_pattern(pattern, 0U);
		for (y = 0U; y < I915_TEX_FHD_HEIGHT; y++) {
			for (x = 0U; x < I915_TEX_FHD_WIDTH; x++)
				pixels[y * I915_TEX_FHD_WIDTH + x] = drv_i915_tex_fixture_expected_pixel(pattern, 4U * (x / 240U), 4U * (y / 135U));
		}

		/* Verifies it, then breaks the last and the first pixel and verifies again. */
		bad_before = drv_i915_test_fhd_render_verify(NULL, pixels, I915_TEX_FHD_PITCH);
		pixels[1079U * I915_TEX_FHD_WIDTH + 1919U] ^= 0x00010000U;
		pixels[0] ^= 0x1U;
		bad_after = drv_i915_test_fhd_render_verify(NULL, pixels, I915_TEX_FHD_PITCH);
		drv_i915_gt_object_destroy(&i915_lcd_show_ktest_gm, image);
	}

	/* A correct image has no wrong pixel; two wrong pixels count as two. */
	passed = 0;
	if (image != NULL && bad_before == 0U && bad_after == 2U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-VERIFY all 2,073,600 pixels against texel (x/240, y/135); two wrong pixels are counted as two");
}

/* Checks a buffer the test owns: the display part never creates, fills or frees it. */
static void
i915_lcd_show_ktest_prepared(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_lcd_show_report *rep;
	struct i915_scanout *so;
	unsigned live;
	uint32_t wrong;
	int unpin_error;
	int destroy_error;
	int released;
	int shown;
	int passed;
	int error;
	int error2;

	rep = &i915_lcd_show_ktest_rep;
	so = &i915_lcd_show_ktest_so_p;

	/* Brings the models up and makes a buffer, not yet pinned, and offers it. */
	error = i915_lcd_show_ktest_bring_up(display, so);
	error2 = drv_i915_scanout_create(&i915_lcd_show_ktest_gm, 1920U, 1080U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);
	shown = 0;
	if (error == 0 && error2 == 0)
		shown = drv_i915_lcd_show_prepared(display, &i915_lcd_show_ktest_show, so, NULL, NULL, rep);

	/* The unpinned buffer is refused and left as it was. */
	passed = 0;
	if (error == 0 &&
	    error2 == 0 &&
	    shown == EINVAL &&
	    so->state == I915_SCANOUT_ALLOCATED)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-PREPARED an unpinned buffer is refused; the buffer is left as it was");

	/* The owner pins it, draws its picture and publishes it; then it is shown. */
	(void)drv_i915_scanout_pin(so, "owner");
	(void)drv_i915_lcd_pattern_fill(so->cpu, so->pitch, so->width, so->height, I915_LCD_SHOW_KTEST_PATTERN_PREPARED);
	drv_i915_scanout_publish(so);
	live = i915_lcd_show_ktest_gm.objects_live;
	error2 = drv_i915_lcd_show_prepared(display, &i915_lcd_show_ktest_show, so, NULL, NULL, rep);
	wrong = drv_i915_lcd_pattern_verify(so->cpu, so->pitch, so->width, so->height, I915_LCD_SHOW_KTEST_PATTERN_PREPARED, NULL, NULL);

	/* Shown and stopped; the buffer is back PINNED to its owner with its pixels untouched. */
	passed = 0;
	if (error2 == 0 &&
	    rep->pass &&
	    rep->display_released &&
	    so->state == I915_SCANOUT_PINNED &&
	    so->pin_owner != NULL &&
	    i915_lcd_show_ktest_gm.objects_live == live &&
	    i915_lcd_show_ktest_lcd.plane_surf_at_arm == (uint32_t)so->surf &&
	    wrong == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-PREPARED shown and stopped; the buffer comes back PINNED to its owner with its pixels untouched");

	/* The owner releases it, and the eDP ends. */
	unpin_error = drv_i915_scanout_unpin(so);
	destroy_error = EINVAL;
	if (unpin_error == 0)
		destroy_error = drv_i915_scanout_destroy(so);
	released = i915_lcd_show_ktest_edp_released();

	/* Both releases succeed and nothing is held. */
	passed = 0;
	if (unpin_error == 0 && destroy_error == 0 && released)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-PREPARED the OWNER releases it afterwards");
}

/* Checks a prepared buffer the display part refuses at its check phase. */
static void
i915_lcd_show_ktest_not_started(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_lcd_show_report *rep;
	struct i915_scanout *so;
	unsigned live;
	unsigned arms;
	unsigned writes;
	int unpin_error;
	int destroy_error;
	int retained;
	int released;
	int passed;
	int error;

	rep = &i915_lcd_show_ktest_rep;
	so = &i915_lcd_show_ktest_so_p;

	/* A pinned buffer, and a configuration the check phase refuses: an unknown memory bandwidth. */
	(void)i915_lcd_show_ktest_bring_up(display, so);
	(void)drv_i915_scanout_create(&i915_lcd_show_ktest_gm, 1920U, 1080U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);
	(void)drv_i915_scanout_pin(so, "owner");
	i915_lcd_show_ktest_show.cfg.qgv_allowed_bw = 0U;
	live = i915_lcd_show_ktest_gm.objects_live;
	arms = i915_lcd_show_ktest_lcd.plane_arms;
	writes = i915_lcd_show_ktest_lcd.nregs;

	/* Offers it. */
	error = drv_i915_lcd_show_prepared(display, &i915_lcd_show_ktest_show, so, NULL, NULL, rep);
	retained = drv_i915_lcd_show_retained(display);

	/* The display never acquired it: nothing written, nothing retained, the buffer still the owner's. */
	passed = 0;
	if (error != 0 &&
	    rep->display_acquired == 0 &&
	    rep->display_released == 0 &&
	    !rep->abandoned &&
	    rep->prepare_rc != 0 &&
	    so->state == I915_SCANOUT_PINNED &&
	    i915_lcd_show_ktest_lcd.plane_arms == arms &&
	    i915_lcd_show_ktest_lcd.nregs == writes &&
	    !retained &&
	    i915_lcd_show_ktest_gm.objects_live == live)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-NOTSTARTED prepare refused: display_acquired=0 (not 'stop confirmed'), no register written, nothing retained");

	/* The owner reclaims it at once, and the eDP ends. */
	unpin_error = drv_i915_scanout_unpin(so);
	destroy_error = EINVAL;
	if (unpin_error == 0)
		destroy_error = drv_i915_scanout_destroy(so);
	released = i915_lcd_show_ktest_edp_released();

	/* Both releases succeed and nothing is held. */
	passed = 0;
	if (unpin_error == 0 && destroy_error == 0 && released)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-g: G-NOTSTARTED the owner reclaims the buffer at once, and the next run is not blocked");
}

/* Checks run B: the sink never reports clock recovery. */
static void
i915_lcd_show_ktest_early_failure(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_lcd_show_report *rep;
	struct i915_lcd_fake_hw *lcd;
	int power_refs;
	int released;
	int passed;
	int error;

	rep = &i915_lcd_show_ktest_rep;
	lcd = &i915_lcd_show_ktest_lcd;

	/* The training never gets clock recovery. */
	error = i915_lcd_show_ktest_bring_up(display, &i915_lcd_show_ktest_so_b);
	lcd->fault_cr_never = 1;
	if (error == 0) {
		error = drv_i915_lcd_show_run(display, &i915_lcd_show_ktest_show, rep);
	} else {
		error = 0;
	}

	/* No success; the training is the first anomaly; the plane was never armed. */
	passed = 0;
	if (error != 0 &&
	    !rep->pass &&
	    rep->enable_rc == I915_LCD_MS_ERRORS &&
	    rep->first_anomaly != NULL &&
	    rep->first_anomaly_stage == I915_LCD_SHOW_PREPARED &&
	    rep->first_error_trace_at >= 0 &&
	    lcd->plane_arms == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: B-FAIL the sink never reports clock recovery: no success, the first anomaly is that one, the plane was never armed");

	/* Reads the model's references and ends the eDP. */
	power_refs = drv_i915_lcd_fake_power_refs_total(lcd);
	released = i915_lcd_show_ktest_edp_released();

	/* The disable path ran cleanly, apart from the first anomaly, and gave everything back. */
	passed = 0;
	if (rep->disable_rc == I915_LCD_MS_OK &&
	    rep->cleanup_errors == 0U &&
	    rep->stopped_rc == 0 &&
	    rep->released &&
	    !rep->abandoned &&
	    power_refs == 0 &&
	    lcd->dbuf_enabled == 0x01U &&
	    i915_lcd_show_ktest_gm.display_allocated_pages == 0U &&
	    released)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: B-CLEANUP the reference's disable path ran; its result is recorded apart and did not replace the first anomaly");
}

/* Checks run C: a pipe that does not stop leaves the buffer abandoned with everything it needs. */
static void
i915_lcd_show_ktest_stuck(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_lcd_show_report *rep;
	struct i915_lcd_fake_hw *lcd;
	struct i915_scanout *so;
	unsigned display_before;
	unsigned first;
	unsigned pages;
	unsigned live;
	unsigned live_ptes;
	uint32_t wrong;
	int unpin_error;
	int destroy_error;
	int retained;
	int passed;
	int error;

	rep = &i915_lcd_show_ktest_rep;
	lcd = &i915_lcd_show_ktest_lcd;
	so = &i915_lcd_show_ktest_so_c;

	/* The pipe stops obeying once the window has passed. */
	error = i915_lcd_show_ktest_bring_up(display, so);
	i915_lcd_show_ktest_show.at_stage = i915_lcd_show_ktest_stick_the_pipe;
	display_before = i915_lcd_show_ktest_gm.display_allocated_pages;
	if (error == 0) {
		error = drv_i915_lcd_show_run(display, &i915_lcd_show_ktest_show, rep);
	} else {
		error = 0;
	}

	/* Where the buffer is mapped. */
	first = 0U;
	pages = 0U;
	if (so->obj != NULL) {
		first = so->obj->ggtt_page;
		pages = so->obj->pages;
	}

	/* The run ends ABANDONED; the anomaly is at the disable, with the cleanup's own error index. */
	passed = 0;
	if (error != 0 &&
	    rep->stage == I915_LCD_SHOW_ABANDONED &&
	    rep->abandoned &&
	    !rep->released &&
	    rep->disable_rc == I915_LCD_MS_ERRORS &&
	    rep->cleanup_errors >= 1U &&
	    rep->cleanup_first_error_trace_at > 0 &&
	    rep->first_anomaly_stage == I915_LCD_SHOW_WINDOW_DONE)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-STUCK a pipe that does not stop: the run ends ABANDONED; the anomaly is recorded at the disable, with the cleanup's own error index");

	/* Reads the picture back from the kept pages, and counts the entries that still map them. */
	wrong = 1U;
	if (so->cpu != NULL)
		wrong = drv_i915_lcd_pattern_verify(so->cpu, so->pitch, so->width, so->height, I915_LCD_SHOW_KTEST_PATTERN, NULL, NULL);
	live_ptes = i915_lcd_show_ktest_live_ptes(first, pages);

	/* Pages holding the picture, mapping, every entry, the pin and its owner are all still there. */
	passed = 0;
	if (so->state == I915_SCANOUT_ABANDONED &&
	    so->obj != NULL &&
	    so->obj->keep == 1 &&
	    so->obj->in_use &&
	    so->obj->bound &&
	    so->pin_owner != NULL &&
	    so->surf == rep->surf &&
	    pages == I915_LCD_SHOW_KTEST_FHD_PAGES &&
	    live_ptes == pages &&
	    i915_lcd_show_ktest_gm.display_allocated_pages > display_before &&
	    wrong == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-KEPT backing pages (still holding the picture), CPU mapping, every PTE, the pin and its owner are all still there");

	/* Tries to release the abandoned buffer. */
	unpin_error = drv_i915_scanout_unpin(so);
	destroy_error = 0;
	if (unpin_error == EBUSY)
		destroy_error = drv_i915_scanout_destroy(so);
	live_ptes = i915_lcd_show_ktest_live_ptes(first, pages);

	/* Both are refused and counted; every entry stays. */
	passed = 0;
	if (unpin_error == EBUSY &&
	    destroy_error == EBUSY &&
	    so->refused_unpin == 1U &&
	    so->refused_destroy == 1U &&
	    live_ptes == pages)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-REFUSED unpin and destroy are refused for an abandoned buffer");

	/* DC_OFF, the crtc's power domains and the DBUF slices stayed under the running pipe. */
	passed = 0;
	if (rep->at_disable.stop_unconfirmed == 1 &&
	    rep->at_disable.dc_off_held == 1 &&
	    rep->at_disable.crtc_domains_held == 4U &&
	    lcd->dbuf_enabled == 0x0fU &&
	    lcd->power_dropped_with_pipe_on == 0U &&
	    lcd->dbuf_shrunk_under_plane == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-HELD DC_OFF, the crtc's power domains and the DBUF slices were not taken from under the pipe");

	/* A further run on the abandoned storage. */
	i915_lcd_show_ktest_show.at_stage = NULL;
	i915_lcd_show_ktest_show.so = so;
	error = drv_i915_lcd_show_run(display, &i915_lcd_show_ktest_show, rep);

	/* It is refused before anything is touched. */
	passed = 0;
	if (error == EBUSY)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-NEXT a further run on the abandoned storage is refused before anything is touched");

	/* A run with fresh storage: the device latch still refuses it. */
	live = i915_lcd_show_ktest_gm.objects_live;
	display_before = i915_lcd_show_ktest_gm.display_allocated_pages;
	i915_lcd_show_ktest_show.so = &i915_lcd_show_ktest_so_b;
	error = drv_i915_lcd_show_run(display, &i915_lcd_show_ktest_show, rep);
	retained = drv_i915_lcd_show_retained(display);

	/* Refused before anything is allocated, and the latch stays. */
	passed = 0;
	if (error == EBUSY &&
	    retained == 1 &&
	    i915_lcd_show_ktest_gm.objects_live == live &&
	    i915_lcd_show_ktest_gm.display_allocated_pages == display_before &&
	    i915_lcd_show_ktest_so_b.state == I915_SCANOUT_NONE)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-LATCH a re-call with fresh storage is refused before anything is allocated; the latch stays set");
	i915_lcd_show_ktest_show.so = so;
}

/* Checks that nothing below the scanout wrapper, not even the outer teardown, frees the abandoned buffer. */
static void
i915_lcd_show_ktest_abandoned(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_scanout *so;
	struct i915_gt_object *object;
	struct i915_gt_mem *gm;
	unsigned object_page;
	unsigned first;
	unsigned pages;
	unsigned live_ptes;
	uint32_t wrong;
	int created;
	int retained;
	int discarded;
	int retained_after;
	int modeset_retained;
	int passed;

	so = &i915_lcd_show_ktest_so_c;
	gm = &i915_lcd_show_ktest_gm;
	object = so->obj;

	/* Nothing else can be checked without the abandoned buffer. */
	if (object == NULL) {
		drv_i915_ktest_check(ktest, 0, "lcd-show: C-CREATE create on an abandoned object is refused and leaves its record untouched");
		drv_i915_ktest_check(ktest, 0, "lcd-show: C-BELOW the object layer below the scanout wrapper refuses to destroy / unbind the kept object too");
		drv_i915_ktest_check(ktest, 0, "lcd-show: C-TEARDOWN drv_i915_gt_mem_fini() leaves the abandoned buffer alone: PTEs live, pages readable, object kept");
		drv_i915_ktest_check(ktest, 0, "lcd-show: C-DISCARD the latch outlives the teardown; only discarding the model run clears it");
		(void)drv_i915_edp_end(i915_lcd_show_ktest_world, &i915_lcd_show_ktest_edp);
		drv_i915_dp_fake_flush_async(&i915_lcd_show_ktest_dpf);
		drv_i915_gt_mem_fini(gm);
		return;
	}

	object_page = object->ggtt_page;
	first = object->ggtt_page;
	pages = object->pages;

	/* Creates a buffer in the abandoned storage. */
	created = drv_i915_scanout_create(gm, 1920U, 1080U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);

	/* Refused, and the record is untouched. */
	passed = 0;
	if (created == EBUSY &&
	    so->state == I915_SCANOUT_ABANDONED &&
	    so->obj == object &&
	    so->pin_owner != NULL)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-CREATE create on an abandoned object is refused and leaves its record untouched");

	/* Asks the object layer itself to destroy and to unbind the kept object. */
	drv_i915_gt_object_destroy(gm, object);
	drv_i915_gt_display_unbind(gm, object);
	live_ptes = i915_lcd_show_ktest_live_ptes(first, pages);

	/* Both are refused and counted; every entry stays. */
	passed = 0;
	if (object->in_use &&
	    object->bound &&
	    object->ggtt_page == object_page &&
	    gm->keep_refusals == 2U &&
	    live_ptes == pages)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-BELOW the object layer below the scanout wrapper refuses to destroy / unbind the kept object too");

	/* The eDP ends. */
	(void)drv_i915_edp_end(i915_lcd_show_ktest_world, &i915_lcd_show_ktest_edp);
	drv_i915_dp_fake_flush_async(&i915_lcd_show_ktest_dpf);

	/* The outer teardown: everything else goes, the abandoned buffer stays mapped. */
	drv_i915_gt_mem_fini(gm);
	wrong = drv_i915_lcd_pattern_verify(so->cpu, so->pitch, so->width, so->height, I915_LCD_SHOW_KTEST_PATTERN, NULL, NULL);
	live_ptes = i915_lcd_show_ktest_live_ptes(first, pages);

	/* Entries live, pages readable, object kept. */
	passed = 0;
	if (gm->kept_objects == 1U &&
	    live_ptes == pages &&
	    object->in_use &&
	    wrong == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-TEARDOWN drv_i915_gt_mem_fini() leaves the abandoned buffer alone: PTEs live, pages readable, object kept");

	/* The latch before and after discarding the model run. */
	retained = drv_i915_lcd_show_retained(display);
	discarded = drv_i915_lcd_show_discard_model(display, &i915_lcd_show_ktest_show);
	retained_after = drv_i915_lcd_show_retained(display);
	modeset_retained = drv_i915_lcd_modeset_retained(display);

	/* Only the discard clears it, the modeset object's with it. */
	passed = 0;
	if (retained == 1 &&
	    discarded == 0 &&
	    retained_after == 0 &&
	    modeset_retained == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-show: C-DISCARD the latch outlives the teardown; only discarding the model run clears it");
}
