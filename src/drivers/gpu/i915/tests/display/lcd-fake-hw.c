/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register and sink model of the LCD modeset (see lcd-fake-hw.h).
 *
 * Register offsets and bits are the public ones of the display engine (ICL+
 * combo PLL enable, DDI_BUF_CTL, TGL+ DP_TP_CTL / STATUS, TRANSCONF,
 * PIPEDSL, the frame counter, PLANE_CTL / SURF, DPCLKA_CFGCR0) and of the DP
 * standard's DPCD (0x100.., 0x202..), written out here independently of the
 * driver's definitions so a wrong definition on the driver side shows up as
 * a failure.
 */

#include "lcd-fake-hw.h"
#include <kern/kcrt.h>

#include "../../display/dp-sink.h"
#include "../../display/modeset.h"


/* The combo PLL enable register and its bits. */
#define I915_LCD_FAKE_DPLL_ENABLE(id)           (0x46010U + 4U * (unsigned)(id))
#define I915_LCD_FAKE_PLL_ENABLE                (1U << 31)
#define I915_LCD_FAKE_PLL_LOCK                  (1U << 30)
#define I915_LCD_FAKE_PLL_POWER_ENABLE          (1U << 27)
#define I915_LCD_FAKE_PLL_POWER_STATE           (1U << 26)

/* The DDI clock select, the DDI buffer and the DP transport. */
#define I915_LCD_FAKE_DPCLKA_CFGCR0             0x164280U
#define I915_LCD_FAKE_DDI_BUF_CTL(port)         (0x64000U + 0x100U * (unsigned)(port))
#define I915_LCD_FAKE_DDI_BUF_CTL_ENABLE        (1U << 31)
#define I915_LCD_FAKE_DDI_BUF_IS_IDLE           (1U << 7)
#define I915_LCD_FAKE_DP_TP_CTL(t)              (0x60540U + 0x1000U * (unsigned)(t))
#define I915_LCD_FAKE_DP_TP_CTL_ENABLE          (1U << 31)
#define I915_LCD_FAKE_DP_TP_CTL_TRAIN_MASK      (7U << 8)
#define I915_LCD_FAKE_DP_TP_STATUS(t)           (0x60544U + 0x1000U * (unsigned)(t))
#define I915_LCD_FAKE_DP_TP_STATUS_IDLE_DONE    (1U << 25)

/* The transcoder and pipe. */
#define I915_LCD_FAKE_TRANSCONF(p)              (0x70008U + 0x1000U * (unsigned)(p))
#define I915_LCD_FAKE_TRANSCONF_ENABLE          (1U << 31)
#define I915_LCD_FAKE_TRANSCONF_STATE           (1U << 30)
#define I915_LCD_FAKE_PIPEDSL(p)                (0x70000U + 0x1000U * (unsigned)(p))
#define I915_LCD_FAKE_PIPE_FRMCNT(p)            (0x70040U + 0x1000U * (unsigned)(p))
#define I915_LCD_FAKE_PIPESTATUS(p)             (0x70058U + 0x1000U * (unsigned)(p))
#define I915_LCD_FAKE_PIPESTATUS_UNDERRUN       (1U << 31)
#define I915_LCD_FAKE_DE_PIPE_IMR(p)            (0x44404U + 0x10U * (unsigned)(p))

/* The primary plane. */
#define I915_LCD_FAKE_PLANE_CTL(p)              (0x70180U + 0x1000U * (unsigned)(p))
#define I915_LCD_FAKE_PLANE_CTL_ENABLE          (1U << 31)
#define I915_LCD_FAKE_PLANE_SURF(p)             (0x7019cU + 0x1000U * (unsigned)(p))
#define I915_LCD_FAKE_PLANE_SURFLIVE(p)         (0x701acU + 0x1000U * (unsigned)(p))
#define I915_LCD_FAKE_PLANE_WM0(p)              (0x70240U + 0x1000U * (unsigned)(p))
#define I915_LCD_FAKE_PLANE_BUF_CFG(p)          (0x7027cU + 0x1000U * (unsigned)(p))

/* MBUS and DBUF. */
#define I915_LCD_FAKE_MBUS_CTL                  0x4438cU
#define I915_LCD_FAKE_MBUS_JOIN                 (1U << 31)
#define I915_LCD_FAKE_DBUF_POWER_REQUEST        (1U << 31)
#define I915_LCD_FAKE_DBUF_POWER_STATE          (1U << 30)

/* PP_CONTROL panel power, in the dp-fake-hw.c model. */
#define I915_LCD_FAKE_PPC_POWER_ON              1U

/*
 * DBUF_CTL of slices S1..S4.
 *
 * Constant: the registers the DBUF slice requests program.
 */
static const uint32_t i915_lcd_fake_dbuf_ctl[4] = { 0x45008U, 0x44fe8U, 0x44300U, 0x44304U };

static uint8_t i915_lcd_fake_slices_of_range(const struct i915_lcd_fake_hw *hw, uint32_t start, uint32_t last);
static int i915_lcd_fake_dc_off_held(const struct i915_lcd_fake_hw *hw);
static uint32_t *i915_lcd_fake_slot(struct i915_lcd_fake_hw *hw, uint32_t reg);
static int i915_lcd_fake_pll_locked(const struct i915_lcd_fake_hw *hw);
static int i915_lcd_fake_ddi_clock_on(const struct i915_lcd_fake_hw *hw);
static uint32_t i915_lcd_fake_frame(const struct i915_lcd_fake_hw *hw);
static void i915_lcd_fake_to_next_frame(struct i915_lcd_fake_hw *hw);
static uint32_t i915_lcd_fake_read32(void *ctx, uint32_t reg);
static uint32_t i915_lcd_fake_write_pll(struct i915_lcd_fake_hw *hw, uint32_t old, uint32_t value);
static uint32_t i915_lcd_fake_write_ddi_buf(struct i915_lcd_fake_hw *hw, uint32_t old, uint32_t value);
static void i915_lcd_fake_write_tp_ctl(struct i915_lcd_fake_hw *hw, uint32_t value);
static uint32_t i915_lcd_fake_write_transconf(struct i915_lcd_fake_hw *hw, uint32_t old, uint32_t value);
static void i915_lcd_fake_check_arm(struct i915_lcd_fake_hw *hw);
static void i915_lcd_fake_write_plane_surf(struct i915_lcd_fake_hw *hw, uint32_t value);
static void i915_lcd_fake_write32(void *ctx, uint32_t reg, uint32_t value);
static uint32_t i915_lcd_fake_rmw32(void *ctx, uint32_t reg, uint32_t clear, uint32_t set);
static int i915_lcd_fake_wait_reg(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned timeout_ms);
static void i915_lcd_fake_sleep(void *ctx, unsigned us);
static long i915_lcd_fake_dpcd_read(void *ctx, unsigned offset, uint8_t *buf, size_t size);
static long i915_lcd_fake_dpcd_write(void *ctx, unsigned offset, const uint8_t *buf, size_t size);
static int i915_lcd_fake_read_dpcd_caps(void *ctx, uint8_t dpcd[15]);
static int i915_lcd_fake_panel(void *ctx, int op);
static int i915_lcd_fake_power_get(void *ctx, int domain);
static void i915_lcd_fake_power_put(void *ctx, int domain, int wakeref);
static void i915_lcd_fake_power_put_async(void *ctx, int domain, int wakeref, int delay_ms);
static void i915_lcd_fake_dbuf_slices_update(void *ctx, unsigned req_slices);
static int i915_lcd_fake_vblank_get(void *ctx, int pipe);
static void i915_lcd_fake_vblank_put(void *ctx, int pipe);
static long i915_lcd_fake_vblank_sleep(void *ctx, int pipe, long ticks);
static void i915_lcd_fake_irq_off(void *ctx);
static void i915_lcd_fake_irq_on(void *ctx);
static void i915_lcd_fake_arm_event(void *ctx, int pipe);
static int i915_lcd_fake_wait_event(void *ctx, int pipe, unsigned timeout_ms);
static void i915_lcd_fake_cancel_event(void *ctx, int pipe);
static void i915_lcd_fake_observe(void *ctx, int point);
static void i915_lcd_fake_lock(void *ctx, int which, int take);
static void i915_lcd_fake_step(void *ctx, const char *name);
static unsigned i915_lcd_fake_tp_field(unsigned pattern);
static int i915_lcd_fake_source_ok(const struct i915_lcd_fake_hw *hw, uint32_t tp_ctl);
static void i915_lcd_fake_sink_on_dpcd_write(void *ctx, unsigned addr, unsigned len);

/*
 * Starts the model as the display engine is found before a modeset, on a
 * dp-fake-hw.c model that is already initialised.
 *
 * The DDI is idle with its clock gated, the PLL off, the link not trained;
 * as the normal initialisation leaves them, DBUF slice 1 is on, MBUS is not
 * joined and every pipe interrupt is masked.  The sink's link status is
 * reset to "not trained" and its link-training behaviour is attached to the
 * AUX model.
 */
void
drv_i915_lcd_fake_init(
	struct i915_lcd_fake_hw *hw,
	struct i915_dp_fake_hw *dpf,
	int pipe,
	int port,
	int dpll_id,
	unsigned vtotal)
{
	/* Starts every register, counter and fault at zero, on the instances watched. */
	kern_memset(hw, 0, sizeof(*hw));
	hw->dpf = dpf;
	hw->pipe = pipe;
	hw->port = port;
	hw->dpll_id = dpll_id;
	hw->vtotal = vtotal;

	/* Before a modeset: DDI idle and its clock gated (PHY A and B). */
	*i915_lcd_fake_slot(hw, I915_LCD_FAKE_DDI_BUF_CTL(port)) = I915_LCD_FAKE_DDI_BUF_IS_IDLE;
	*i915_lcd_fake_slot(hw, I915_LCD_FAKE_DPCLKA_CFGCR0) = (1U << 10) | (1U << 11);

	/* After the normal initialisation: DBUF slice 1 on, MBUS not joined, every pipe interrupt masked. */
	*i915_lcd_fake_slot(hw, i915_lcd_fake_dbuf_ctl[0]) = I915_LCD_FAKE_DBUF_POWER_REQUEST | I915_LCD_FAKE_DBUF_POWER_STATE;
	hw->dbuf_enabled = 0x01U;
	*i915_lcd_fake_slot(hw, I915_LCD_FAKE_DE_PIPE_IMR(pipe)) = 0xffffffffU;

	/* The sink: link status not trained, in D0, training behaviour attached. */
	kern_memset(dpf->dpcd + 0x202, 0, 6U);
	dpf->dpcd[0x600] = 1U;
	dpf->on_dpcd_write = i915_lcd_fake_sink_on_dpcd_write;
	dpf->on_dpcd_write_ctx = hw;

	/* The backend: a model, whose discarding isolates whatever it holds. */
	hw->ops.ctx = hw;
	hw->ops.model = 1;

	/* The registers, waits and delays. */
	hw->ops.write32 = i915_lcd_fake_write32;
	hw->ops.rmw32 = i915_lcd_fake_rmw32;
	hw->ops.read32 = i915_lcd_fake_read32;
	hw->ops.wait_reg = i915_lcd_fake_wait_reg;
	hw->ops.usleep = i915_lcd_fake_sleep;
	hw->ops.udelay = i915_lcd_fake_sleep;

	/* The panel, through the resident eDP. */
	hw->ops.dpcd_read = i915_lcd_fake_dpcd_read;
	hw->ops.dpcd_write = i915_lcd_fake_dpcd_write;
	hw->ops.read_dpcd_caps = i915_lcd_fake_read_dpcd_caps;
	hw->ops.panel = i915_lcd_fake_panel;

	/* The power domains and the DBUF slices. */
	hw->ops.power_get = i915_lcd_fake_power_get;
	hw->ops.power_put = i915_lcd_fake_power_put;
	hw->ops.power_put_async = i915_lcd_fake_power_put_async;
	hw->ops.dbuf_slices_update = i915_lcd_fake_dbuf_slices_update;

	/* The observation points, the vblank, the update section and its event. */
	hw->ops.observe = i915_lcd_fake_observe;
	hw->ops.vblank_get = i915_lcd_fake_vblank_get;
	hw->ops.vblank_put = i915_lcd_fake_vblank_put;
	hw->ops.vblank_sleep = i915_lcd_fake_vblank_sleep;
	hw->ops.irq_off = i915_lcd_fake_irq_off;
	hw->ops.irq_on = i915_lcd_fake_irq_on;
	hw->ops.arm_event = i915_lcd_fake_arm_event;
	hw->ops.wait_event = i915_lcd_fake_wait_event;
	hw->ops.cancel_event = i915_lcd_fake_cancel_event;

	/* The locks and the record of unported callees. */
	hw->ops.lock = i915_lcd_fake_lock;
	hw->ops.step = i915_lcd_fake_step;
}

/*
 * Returns the value of a register of the model; one never written reads 0.
 */
uint32_t
drv_i915_lcd_fake_reg(
	const struct i915_lcd_fake_hw *hw,
	uint32_t reg)
{
	unsigned i;

	/* Looks the register up among those written. */
	for (i = 0U; i < hw->nregs; i++) {
		if (hw->regs[i].reg == reg)
			return hw->regs[i].val;
	}

	/* Succeeded: never written. */
	return 0U;
}

/*
 * Returns the number of power-domain references held, all domains together.
 */
int
drv_i915_lcd_fake_power_refs_total(
	const struct i915_lcd_fake_hw *hw)
{
	int total;
	int domain;

	/* Sums the references of every domain. */
	total = 0;
	for (domain = 0; domain < I915_LCD_FAKE_MAX_DOMAINS; domain++)
		total += hw->power_refs[domain];

	/* Succeeded: reports the sum. */
	return total;
}

/*
 * Returns the number of violations the model counted: ordering mistakes,
 * reference underflows, lock errors and registers that did not fit.
 */
unsigned
drv_i915_lcd_fake_violations(
	const struct i915_lcd_fake_hw *hw)
{
	unsigned total;

	/* The ordering the hardware needs. */
	total = hw->ddi_enabled_without_pll;
	total += hw->pipe_enabled_without_link;
	total += hw->plane_armed_without_pipe;
	total += hw->pll_disabled_with_pipe_on;
	total += hw->training_without_panel_power;
	total += hw->pattern_mismatch;
	total += hw->plane_armed_without_ddb;
	total += hw->modeset_without_dc_off;
	total += hw->pipe_enabled_without_power;
	total += hw->plane_armed_outside_slices;
	total += hw->dbuf_shrunk_under_plane;
	total += hw->power_dropped_with_pipe_on;

	/* The references, the locks and the model's own capacity. */
	total += hw->power_underflows;
	total += hw->lock_errors;
	total += hw->regs_overflow;

	/* Succeeded: reports the sum. */
	return total;
}

/* Returns the DBUF slices a DDB range [start, last] lies in (four equal slices of the platform's DDB). */
static uint8_t
i915_lcd_fake_slices_of_range(
	const struct i915_lcd_fake_hw *hw,
	uint32_t start,
	uint32_t last)
{
	uint32_t per_slice;
	uint8_t mask;
	unsigned slice;

	/* Without a DDB size no slice is known. */
	per_slice = hw->dbuf_size / 4U;
	if (per_slice == 0U)
		return 0U;

	/* Collects every slice the range overlaps. */
	mask = 0U;
	for (slice = 0U; slice < 4U; slice++) {
		if (start < (slice + 1U) * per_slice && last >= slice * per_slice)
			mask |= (uint8_t)(1U << slice);
	}

	/* Succeeded: reports the slices. */
	return mask;
}

/* Tells whether POWER_DOMAIN_DC_OFF is held. */
static int
i915_lcd_fake_dc_off_held(
	const struct i915_lcd_fake_hw *hw)
{
	/* A reference on DC_OFF keeps the DC states off. */
	if (hw->power_refs[I915_PW_DOMAIN_DC_OFF] > 0)
		return 1;

	/* Succeeded: not held. */
	return 0;
}

/*
 * Returns the storage of a register, adding it when it is new.
 *
 * A register beyond the model's capacity is counted and shares the last
 * slot.
 */
static uint32_t *
i915_lcd_fake_slot(
	struct i915_lcd_fake_hw *hw,
	uint32_t reg)
{
	unsigned i;

	/* A register written before keeps its slot. */
	for (i = 0U; i < hw->nregs; i++) {
		if (hw->regs[i].reg == reg)
			return &hw->regs[i].val;
	}

	/* A full model counts the overflow. */
	if (hw->nregs >= I915_LCD_FAKE_MAX_REGS) {
		hw->regs_overflow++;
		return &hw->regs[I915_LCD_FAKE_MAX_REGS - 1U].val;
	}

	/* Adds the register, reading 0. */
	hw->regs[hw->nregs].reg = reg;
	hw->regs[hw->nregs].val = 0U;
	hw->nregs++;

	/* Succeeded: the new slot. */
	return &hw->regs[hw->nregs - 1U].val;
}

/* Tells whether the watched PLL reports lock. */
static int
i915_lcd_fake_pll_locked(
	const struct i915_lcd_fake_hw *hw)
{
	uint32_t enable;

	/* PLL_LOCK in the PLL's enable register. */
	enable = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_DPLL_ENABLE(hw->dpll_id));
	if ((enable & I915_LCD_FAKE_PLL_LOCK) != 0U)
		return 1;

	/* Succeeded: not locked. */
	return 0;
}

/* Tells whether the watched DDI's clock is ungated (ICL_DPCLKA_CFGCR0_DDI_CLK_OFF: bit 10 for PHY A, 11 for PHY B). */
static int
i915_lcd_fake_ddi_clock_on(
	const struct i915_lcd_fake_hw *hw)
{
	uint32_t cfgcr0;

	/* The clock-off bit of the port's PHY. */
	cfgcr0 = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_DPCLKA_CFGCR0);
	if ((cfgcr0 & (1U << (10 + hw->port))) == 0U)
		return 1;

	/* Succeeded: gated. */
	return 0;
}

/* Returns the number of frames the pipe has scanned out at 60 Hz since it started; 0 while it is off. */
static uint32_t
i915_lcd_fake_frame(
	const struct i915_lcd_fake_hw *hw)
{
	/* An off pipe has no frames. */
	if (hw->pipe_on_since_us == 0U)
		return 0U;

	/* Succeeded: frames of 1/60 s since the start. */
	return (uint32_t)((hw->dpf->now_us - hw->pipe_on_since_us) * 60U / 1000000U);
}

/* Moves the clock to the next frame boundary after now. */
static void
i915_lcd_fake_to_next_frame(
	struct i915_lcd_fake_hw *hw)
{
	uint32_t next;

	/* An off pipe has no frame boundary. */
	if (hw->pipe_on_since_us == 0U)
		return;

	/* The start of the next frame, rounded up to the microsecond. */
	next = i915_lcd_fake_frame(hw) + 1U;
	hw->dpf->now_us = hw->pipe_on_since_us + ((uint64_t)next * 1000000U + 59U) / 60U;
}

/* Reads a register of the model. */
static uint32_t
i915_lcd_fake_read32(
	void *ctx,
	uint32_t reg)
{
	struct i915_lcd_fake_hw *hw;
	uint32_t *status;

	hw = ctx;

	/* PLANE_SURFLIVE: the pending surface becomes live once its frame has passed. */
	if (reg == I915_LCD_FAKE_PLANE_SURFLIVE(hw->pipe)) {
		if (hw->surf_pending_valid &&
		    !hw->fault_flip_never_latch &&
		    i915_lcd_fake_frame(hw) > hw->surf_pending_frame) {
			hw->surf_live = hw->surf_pending;
			hw->surf_pending_valid = 0;
		}

		return hw->surf_live;
	}

	/* PIPEDSL: a held scanline, or one that moves while the pipe runs. */
	if (reg == I915_LCD_FAKE_PIPEDSL(hw->pipe)) {
		if (hw->scanline_hold_reads != 0U) {
			hw->scanline_hold_reads--;
			hw->scanline = hw->scanline_hold;
			return hw->scanline;
		}

		if (hw->pipe_on_since_us != 0U) {
			if (hw->vtotal != 0U) {
				hw->scanline = (hw->scanline + 97U) % hw->vtotal;
			} else {
				/* A mode without a vertical total has only line 0. */
				hw->scanline = 0U;
			}
		}

		return hw->scanline;
	}

	/* The frame counter; the n-th read after the arm may raise the steady underrun. */
	if (reg == I915_LCD_FAKE_PIPE_FRMCNT(hw->pipe)) {
		if (hw->plane_arms != 0U) {
			hw->frame_reads_after_arm++;
			if (hw->frame_reads_after_arm == (unsigned)hw->fault_underrun_steady_after) {
				status = i915_lcd_fake_slot(hw, I915_LCD_FAKE_PIPESTATUS(hw->pipe));
				*status |= I915_LCD_FAKE_PIPESTATUS_UNDERRUN;
			}
		}

		if (hw->fault_frame_counter_frozen)
			return 7U;

		return i915_lcd_fake_frame(hw);
	}

	/* The pipe's interrupt registers live in its power well: off, they read 0 (the well's enable programs the mask). */
	if (reg == I915_LCD_FAKE_DE_PIPE_IMR(hw->pipe) && hw->power_refs[I915_PW_DOMAIN_PIPE_A + hw->pipe] <= 0)
		return 0U;

	/* Succeeded: any other register reads what was written. */
	return drv_i915_lcd_fake_reg(hw, reg);
}

/* Returns the stored PLL enable word: power state follows power enable, lock follows enable. */
static uint32_t
i915_lcd_fake_write_pll(
	struct i915_lcd_fake_hw *hw,
	uint32_t old,
	uint32_t value)
{
	/* A PLL enable needs DC_OFF. */
	if ((value & I915_LCD_FAKE_PLL_ENABLE) != 0U &&
	    (old & I915_LCD_FAKE_PLL_ENABLE) == 0U &&
	    !i915_lcd_fake_dc_off_held(hw))
		hw->modeset_without_dc_off++;

	/* The status bits are the model's, not the writer's. */
	value &= ~(I915_LCD_FAKE_PLL_LOCK | I915_LCD_FAKE_PLL_POWER_STATE);
	if ((value & I915_LCD_FAKE_PLL_POWER_ENABLE) != 0U)
		value |= I915_LCD_FAKE_PLL_POWER_STATE;

	/* An enabled, powered PLL locks at once unless the fault says otherwise. */
	if ((value & I915_LCD_FAKE_PLL_ENABLE) != 0U &&
	    (value & I915_LCD_FAKE_PLL_POWER_STATE) != 0U &&
	    !hw->fault_pll_no_lock)
		value |= I915_LCD_FAKE_PLL_LOCK;

	/* A PLL switched off under a running pipe. */
	if ((old & I915_LCD_FAKE_PLL_ENABLE) != 0U &&
	    (value & I915_LCD_FAKE_PLL_ENABLE) == 0U &&
	    hw->pipe_on_since_us != 0U)
		hw->pll_disabled_with_pipe_on++;

	/* Succeeded: the word to store. */
	return value;
}

/* Returns the stored DDI_BUF_CTL word: idle follows enable. */
static uint32_t
i915_lcd_fake_write_ddi_buf(
	struct i915_lcd_fake_hw *hw,
	uint32_t old,
	uint32_t value)
{
	int pll_locked;
	int clock_on;

	/* A disabled buffer goes idle. */
	if ((value & I915_LCD_FAKE_DDI_BUF_CTL_ENABLE) == 0U)
		return value | I915_LCD_FAKE_DDI_BUF_IS_IDLE;

	/* An enabled buffer is not idle; it needs a locked PLL and an ungated clock. */
	value &= ~I915_LCD_FAKE_DDI_BUF_IS_IDLE;
	pll_locked = i915_lcd_fake_pll_locked(hw);
	clock_on = i915_lcd_fake_ddi_clock_on(hw);
	if ((old & I915_LCD_FAKE_DDI_BUF_CTL_ENABLE) == 0U) {
		if (!pll_locked || !clock_on)
			hw->ddi_enabled_without_pll++;
	}

	/* Succeeded: the word to store. */
	return value;
}

/* Answers a DP_TP_CTL write: the idle pattern requested, the hardware reports "idle done". */
static void
i915_lcd_fake_write_tp_ctl(
	struct i915_lcd_fake_hw *hw,
	uint32_t value)
{
	uint32_t *status;

	/* The idle pattern is field value 2. */
	status = i915_lcd_fake_slot(hw, I915_LCD_FAKE_DP_TP_STATUS(hw->pipe));
	if ((value & I915_LCD_FAKE_DP_TP_CTL_ENABLE) != 0U &&
	    (value & I915_LCD_FAKE_DP_TP_CTL_TRAIN_MASK) == (2U << 8)) {
		*status |= I915_LCD_FAKE_DP_TP_STATUS_IDLE_DONE;
	} else {
		*status &= ~I915_LCD_FAKE_DP_TP_STATUS_IDLE_DONE;
	}
}

/* Returns the stored TRANSCONF word: the state bit follows enable, unless the pipe is stuck on. */
static uint32_t
i915_lcd_fake_write_transconf(
	struct i915_lcd_fake_hw *hw,
	uint32_t old,
	uint32_t value)
{
	uint32_t ddi_buf;

	/* The state bit is the model's. */
	value &= ~I915_LCD_FAKE_TRANSCONF_STATE;

	/* A pipe that does not stop keeps its state bit. */
	if ((value & I915_LCD_FAKE_TRANSCONF_ENABLE) == 0U) {
		if (hw->fault_pipe_stuck_on && (old & I915_LCD_FAKE_TRANSCONF_STATE) != 0U)
			return value | I915_LCD_FAKE_TRANSCONF_STATE;

		hw->pipe_on_since_us = 0U;
		return value;
	}

	/* An enabled pipe runs; an enable edge starts its frames. */
	value |= I915_LCD_FAKE_TRANSCONF_STATE;
	if ((old & I915_LCD_FAKE_TRANSCONF_ENABLE) != 0U)
		return value;

	hw->pipe_on_since_us = hw->dpf->now_us;
	if (hw->pipe_on_since_us == 0U)
		hw->pipe_on_since_us = 1U;

	/* The pipe needs the DDI buffer on. */
	ddi_buf = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_DDI_BUF_CTL(hw->port));
	if ((ddi_buf & I915_LCD_FAKE_DDI_BUF_CTL_ENABLE) == 0U)
		hw->pipe_enabled_without_link++;

	/* The pipe needs DC_OFF. */
	if (!i915_lcd_fake_dc_off_held(hw))
		hw->modeset_without_dc_off++;

	/* The pipe needs its pipe, transcoder, DDI lanes and display core domains. */
	if (hw->power_refs[I915_PW_DOMAIN_PIPE_A + hw->pipe] <= 0 ||
	    hw->power_refs[I915_PW_DOMAIN_TRANSCODER_A + hw->pipe] <= 0 ||
	    hw->power_refs[I915_PW_DOMAIN_PORT_DDI_LANES_A + hw->port] <= 0 ||
	    hw->power_refs[I915_PW_DOMAIN_DISPLAY_CORE] <= 0)
		hw->pipe_enabled_without_power++;

	/* Succeeded: the word to store. */
	return value;
}

/* Checks what a plane arm needs: a valid DDB in powered slices, watermark level 0, DC_OFF. */
static void
i915_lcd_fake_check_arm(
	struct i915_lcd_fake_hw *hw)
{
	uint32_t buf_cfg;
	uint32_t wm0;
	uint32_t mbus;
	uint32_t start;
	uint32_t last;
	uint8_t slices;
	uint32_t *status;

	/* PLANE_BUF_CFG: start bits 11:0, end (last block) bits 27:16 -- 12-bit fields on display version 13. */
	buf_cfg = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_PLANE_BUF_CFG(hw->pipe));
	wm0 = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_PLANE_WM0(hw->pipe));
	mbus = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_MBUS_CTL);
	start = buf_cfg & 0xfffU;
	last = (buf_cfg >> 16) & 0xfffU;
	slices = i915_lcd_fake_slices_of_range(hw, start, last);

	/*
	 * An empty or reversed range, one beyond the DDB, or watermark level 0
	 * off is an arm without DDB; otherwise the range must lie in powered
	 * slices, and pipe A's upper two slices need MBUS joining.
	 */
	if (buf_cfg == 0U ||
	    last < start ||
	    (hw->dbuf_size != 0U && last >= hw->dbuf_size) ||
	    (wm0 & (1U << 31)) == 0U) {
		hw->plane_armed_without_ddb++;
	} else if ((slices & ~hw->dbuf_enabled) != 0U) {
		hw->plane_armed_outside_slices++;
	} else if ((slices & 0x0cU) != 0U &&
		   hw->pipe == 0 &&
		   (mbus & I915_LCD_FAKE_MBUS_JOIN) == 0U) {
		hw->plane_armed_outside_slices++;
	}

	/* An arm needs DC_OFF. */
	if (!i915_lcd_fake_dc_off_held(hw))
		hw->modeset_without_dc_off++;

	/* The fault raises the underrun status at the arm. */
	if (hw->fault_underrun_at_arm) {
		status = i915_lcd_fake_slot(hw, I915_LCD_FAKE_PIPESTATUS(hw->pipe));
		*status |= I915_LCD_FAKE_PIPESTATUS_UNDERRUN;
	}
}

/* Answers a PLANE_SURF write: the double-buffered surface, and the arm or disarm of the plane. */
static void
i915_lcd_fake_write_plane_surf(
	struct i915_lcd_fake_hw *hw,
	uint32_t value)
{
	uint32_t ctl;

	/* Double buffered: live at the next frame boundary; the first arm of a starting pipe latches at once. */
	if (hw->surf_live == 0U || hw->pipe_on_since_us == 0U) {
		hw->surf_live = value;
	} else {
		hw->surf_pending = value;
		hw->surf_pending_valid = 1;
		hw->surf_pending_frame = i915_lcd_fake_frame(hw);
	}

	/* An arm belongs inside the update section (interrupts off). */
	if (!hw->irq_off)
		hw->arm_outside_section++;

	/* A disabled plane is a disarm. */
	ctl = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_PLANE_CTL(hw->pipe));
	if ((ctl & I915_LCD_FAKE_PLANE_CTL_ENABLE) == 0U) {
		hw->plane_disarms++;
		return;
	}

	/* An enabled plane is armed: records what it holds and checks what it needs. */
	hw->plane_arms++;
	hw->plane_ctl_at_arm = ctl;
	hw->plane_surf_at_arm = value;
	if (hw->pipe_on_since_us == 0U)
		hw->plane_armed_without_pipe++;

	i915_lcd_fake_check_arm(hw);
}

/* Writes a register of the model and runs the behaviour it has. */
static void
i915_lcd_fake_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	struct i915_lcd_fake_hw *hw;
	uint32_t *stored;
	uint32_t old;

	hw = ctx;

	/* A lost write (a test of the model) changes nothing. */
	if (hw->fault_drop_write_reg != 0U && reg == hw->fault_drop_write_reg)
		return;

	/* Finds the register and its old value. */
	stored = i915_lcd_fake_slot(hw, reg);
	old = *stored;

	/* ICL_PIPESTATUS is write-one-to-clear. */
	if (reg == I915_LCD_FAKE_PIPESTATUS(hw->pipe)) {
		*stored = old & ~value;
		return;
	}

	/* Runs the behaviour of the register. */
	if (reg == I915_LCD_FAKE_DPLL_ENABLE(hw->dpll_id)) {
		value = i915_lcd_fake_write_pll(hw, old, value);
	} else if (reg == I915_LCD_FAKE_DDI_BUF_CTL(hw->port)) {
		value = i915_lcd_fake_write_ddi_buf(hw, old, value);
	} else if (reg == I915_LCD_FAKE_DP_TP_CTL(hw->pipe)) {
		i915_lcd_fake_write_tp_ctl(hw, value);
	} else if (reg == I915_LCD_FAKE_DP_TP_STATUS(hw->pipe)) {
		/* DP_TP_STATUS is write-one-to-clear. */
		value = old & ~value;
	} else if (reg == I915_LCD_FAKE_TRANSCONF(hw->pipe)) {
		value = i915_lcd_fake_write_transconf(hw, old, value);
	} else if (reg == I915_LCD_FAKE_PLANE_SURF(hw->pipe)) {
		i915_lcd_fake_write_plane_surf(hw, value);
	}

	*stored = value;
}

/* Reads, modifies and writes a register; returns the old value. */
static uint32_t
i915_lcd_fake_rmw32(
	void *ctx,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	uint32_t old;

	/* Reads the register as the hardware answers, then writes the new word. */
	old = i915_lcd_fake_read32(ctx, reg);
	i915_lcd_fake_write32(ctx, reg, (old & ~clear) | set);

	/* Succeeded: reports the old value. */
	return old;
}

/*
 * Waits for a register value.  Status follows control at once in this
 * model: either it is there, or the whole timeout passes.  0, -ETIMEDOUT, or
 * -EIO on a time-base fault (Linux numbering).
 */
static int
i915_lcd_fake_wait_reg(
	void *ctx,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned timeout_ms)
{
	struct i915_lcd_fake_hw *hw;
	uint32_t current;

	hw = ctx;

	/* A time-base fault is reported as the run's anomaly, not as a timeout. */
	if (hw->fault_time_base_reg != 0U && reg == hw->fault_time_base_reg) {
		drv_i915_lcd_backend_fault("model: time base fault during a register wait (not a timeout)" "\n");
		return I915_LCD_EIO;
	}

	/* The value is there at once. */
	current = i915_lcd_fake_read32(ctx, reg);
	if ((current & mask) == value)
		return 0;

	/* Or only after the whole timeout. */
	hw->dpf->now_us += (uint64_t)timeout_ms * 1000U;
	current = i915_lcd_fake_read32(ctx, reg);
	if ((current & mask) != value)
		return I915_LCD_ETIMEDOUT;

	/* Succeeded: the value came. */
	return 0;
}

/* Sleeps in model time. */
static void
i915_lcd_fake_sleep(
	void *ctx,
	unsigned us)
{
	struct i915_lcd_fake_hw *hw;

	/* Advances the shared clock. */
	hw = ctx;
	hw->dpf->now_us += us;
}

/* Reads DPCD bytes through the resident eDP. */
static long
i915_lcd_fake_dpcd_read(
	void *ctx,
	unsigned offset,
	uint8_t *buf,
	size_t size)
{
	struct i915_lcd_fake_hw *hw;
	long transferred;

	hw = ctx;

	/* The eDP's AUX channel runs on the dp-fake-hw.c model. */
	transferred = drv_i915_edp_dpcd_read(hw->dpf->world, offset, buf, size);
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes read. */
	return transferred;
}

/* Writes DPCD bytes through the resident eDP. */
static long
i915_lcd_fake_dpcd_write(
	void *ctx,
	unsigned offset,
	const uint8_t *buf,
	size_t size)
{
	struct i915_lcd_fake_hw *hw;
	long transferred;

	hw = ctx;

	/* The eDP's AUX channel runs on the dp-fake-hw.c model. */
	transferred = drv_i915_edp_dpcd_write(hw->dpf->world, offset, buf, size);
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes written. */
	return transferred;
}

/* Reads the receiver capabilities through the resident eDP. */
static int
i915_lcd_fake_read_dpcd_caps(
	void *ctx,
	uint8_t dpcd[15])
{
	struct i915_lcd_fake_hw *hw;
	int error;

	hw = ctx;

	/* The eDP's AUX channel runs on the dp-fake-hw.c model. */
	error = drv_i915_edp_read_dpcd_caps(hw->dpf->world, dpcd);
	if (error != 0)
		return error;

	/* Succeeded: the capabilities are read. */
	return 0;
}

/* Runs a panel power operation through the resident eDP's power sequencer. */
static int
i915_lcd_fake_panel(
	void *ctx,
	int op)
{
	struct i915_lcd_fake_hw *hw;
	int error;

	hw = ctx;

	/* The PPS runs on the dp-fake-hw.c model. */
	error = drv_i915_edp_panel_op(hw->dpf->world, op);
	if (error != 0)
		return error;

	/* Succeeded: the operation is done. */
	return 0;
}

/* Takes a power-domain reference; returns the wakeref cookie (domain + 1), or 0 when it fails. */
static int
i915_lcd_fake_power_get(
	void *ctx,
	int domain)
{
	struct i915_lcd_fake_hw *hw;

	hw = ctx;

	/* A domain outside the model, or the failing one, is refused. */
	if (domain < 0 || domain >= I915_LCD_FAKE_MAX_DOMAINS)
		return 0;
	if (hw->fault_power_get == domain + 1)
		return 0;

	/* Counts the reference. */
	hw->power_refs[domain]++;
	hw->power_gets++;

	/* Succeeded: the cookie. */
	return domain + 1;
}

/*
 * Returns a power-domain reference.  The refused domain stays held like a
 * power well kept on; an underflow is counted.
 */
static void
i915_lcd_fake_power_put(
	void *ctx,
	int domain,
	int wakeref)
{
	struct i915_lcd_fake_hw *hw;

	UNUSED_PARAMETER(wakeref);

	hw = ctx;
	hw->power_puts++;

	/* A refused put keeps the reference and is the run's anomaly. */
	if (hw->fault_put_refused_domain == domain + 1) {
		drv_i915_lcd_backend_fault("model: power well kept on (pipe interrupt drain failed)" "\n");
		return;
	}

	/* A put without a reference is an underflow. */
	if (domain < 0 || domain >= I915_LCD_FAKE_MAX_DOMAINS || hw->power_refs[domain] <= 0) {
		hw->power_underflows++;
		return;
	}

	hw->power_refs[domain]--;

	/* The last reference of the running pipe's or transcoder's domain must not go. */
	if (hw->power_refs[domain] == 0 && hw->pipe_on_since_us != 0U) {
		if (domain == I915_PW_DOMAIN_PIPE_A + hw->pipe || domain == I915_PW_DOMAIN_TRANSCODER_A + hw->pipe)
			hw->power_dropped_with_pipe_on++;
	}
}

/* Returns a power-domain reference asynchronously: counted, then an ordinary put. */
static void
i915_lcd_fake_power_put_async(
	void *ctx,
	int domain,
	int wakeref,
	int delay_ms)
{
	struct i915_lcd_fake_hw *hw;

	/* Records the asynchronous put and its delay. */
	hw = ctx;
	hw->async_puts++;
	hw->last_async_delay_ms = delay_ms;

	i915_lcd_fake_power_put(ctx, domain, wakeref);
}

/* Requests exactly these DBUF slices (gen9_dbuf_slices_update()): request bit, then state bit, slice by slice. */
static void
i915_lcd_fake_dbuf_slices_update(
	void *ctx,
	unsigned req_slices)
{
	struct i915_lcd_fake_hw *hw;
	uint32_t buf_cfg;
	uint32_t word;
	uint8_t slices;
	unsigned slice;

	hw = ctx;
	hw->dbuf_updates++;

	/* A lost request (a test of the model) changes nothing. */
	if (hw->fault_drop_dbuf_update)
		return;

	/* A slice the armed plane of a running pipe needs must not go off. */
	if (hw->plane_arms > hw->plane_disarms && hw->pipe_on_since_us != 0U) {
		buf_cfg = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_PLANE_BUF_CFG(hw->pipe));
		slices = i915_lcd_fake_slices_of_range(hw, buf_cfg & 0xfffU, (buf_cfg >> 16) & 0xfffU);
		if ((slices & ~req_slices) != 0U)
			hw->dbuf_shrunk_under_plane++;
	}

	/* Sets or clears each slice's request and state together. */
	for (slice = 0U; slice < 4U; slice++) {
		word = drv_i915_lcd_fake_reg(hw, i915_lcd_fake_dbuf_ctl[slice]);
		word &= ~(I915_LCD_FAKE_DBUF_POWER_REQUEST | I915_LCD_FAKE_DBUF_POWER_STATE);
		if ((req_slices & (1U << slice)) != 0U)
			word |= I915_LCD_FAKE_DBUF_POWER_REQUEST | I915_LCD_FAKE_DBUF_POWER_STATE;

		*i915_lcd_fake_slot(hw, i915_lcd_fake_dbuf_ctl[slice]) = word;
	}

	hw->dbuf_enabled = (uint8_t)(req_slices & 0x0fU);
}

/* Takes a vblank reference of the pipe. */
static int
i915_lcd_fake_vblank_get(
	void *ctx,
	int pipe)
{
	struct i915_lcd_fake_hw *hw;

	UNUSED_PARAMETER(pipe);

	/* Counts the reference. */
	hw = ctx;
	hw->vblank_refs++;

	/* Succeeded: the vblank interrupt is on. */
	return 0;
}

/* Returns a vblank reference; an underflow is counted. */
static void
i915_lcd_fake_vblank_put(
	void *ctx,
	int pipe)
{
	struct i915_lcd_fake_hw *hw;

	UNUSED_PARAMETER(pipe);

	/* A put without a reference is an underflow. */
	hw = ctx;
	if (hw->vblank_refs > 0) {
		hw->vblank_refs--;
	} else {
		hw->power_underflows++;
	}
}

/*
 * Sleeps on the pipe's vblank wait queue until the next vblank, or ticks
 * 10 ms ticks without one; returns the ticks left (0 = timed out).
 */
static long
i915_lcd_fake_vblank_sleep(
	void *ctx,
	int pipe,
	long ticks)
{
	struct i915_lcd_fake_hw *hw;

	UNUSED_PARAMETER(pipe);

	hw = ctx;
	hw->vblank_sleeps++;

	/* The reference enables IRQs before schedule_timeout(). */
	if (hw->irq_off)
		hw->sleep_irq_off++;

	/* Without a vblank the whole timeout passes. */
	if (hw->fault_no_vblank) {
		hw->dpf->now_us += (uint64_t)ticks * 10000U;
		return 0;
	}

	/* Wakes just after the next vblank: far from the evasion window. */
	i915_lcd_fake_to_next_frame(hw);
	hw->scanline = 0U;
	hw->scanline_hold_reads = 0U;
	if (ticks > 1)
		return ticks - 1;

	/* Succeeded: no tick left. */
	return 0;
}

/* Enters the update section: local interrupts off; a nested entry is counted. */
static void
i915_lcd_fake_irq_off(
	void *ctx)
{
	struct i915_lcd_fake_hw *hw;

	hw = ctx;

	/* Interrupts already off is an unbalanced section. */
	if (hw->irq_off)
		hw->lock_errors++;

	hw->irq_off = 1;
	hw->irq_off_calls++;
}

/* Leaves the update section: local interrupts on; an unbalanced exit is counted. */
static void
i915_lcd_fake_irq_on(
	void *ctx)
{
	struct i915_lcd_fake_hw *hw;

	hw = ctx;

	/* Interrupts already on is an unbalanced section. */
	if (!hw->irq_off)
		hw->lock_errors++;

	hw->irq_off = 0;
}

/* Arms the completion event of the pipe's next vblank. */
static void
i915_lcd_fake_arm_event(
	void *ctx,
	int pipe)
{
	struct i915_lcd_fake_hw *hw;

	UNUSED_PARAMETER(pipe);

	/* One event is armed. */
	hw = ctx;
	hw->event_armed = 1;
	hw->events_armed++;
}

/*
 * Waits for the armed event: 0 at the next vblank, -ETIMEDOUT without one,
 * -EINVAL without an armed event (Linux numbering).
 */
static int
i915_lcd_fake_wait_event(
	void *ctx,
	int pipe,
	unsigned timeout_ms)
{
	struct i915_lcd_fake_hw *hw;

	UNUSED_PARAMETER(pipe);

	hw = ctx;

	/* Nothing armed: nothing to wait for. */
	if (!hw->event_armed) {
		hw->waits_refused++;
		return I915_LCD_FAKE_EINVAL;
	}

	/* Without a vblank the whole timeout passes. */
	if (hw->fault_no_vblank) {
		hw->dpf->now_us += (uint64_t)timeout_ms * 1000U;
		return I915_LCD_ETIMEDOUT;
	}

	/* The event completes at the next vblank, or at once when it is early. */
	if (!hw->fault_early_event)
		i915_lcd_fake_to_next_frame(hw);

	hw->event_armed = 0;
	hw->events_done++;

	/* Succeeded: the event completed. */
	return 0;
}

/* Cancels the armed event (drm_crtc_vblank_off() on a pending event). */
static void
i915_lcd_fake_cancel_event(
	void *ctx,
	int pipe)
{
	struct i915_lcd_fake_hw *hw;

	UNUSED_PARAMETER(pipe);

	/* Counts an armed event that is cancelled. */
	hw = ctx;
	if (hw->event_armed)
		hw->events_cancelled++;

	hw->event_armed = 0;
}

/* Records a named point of the commit and tells the observer. */
static void
i915_lcd_fake_observe(
	void *ctx,
	int point)
{
	struct i915_lcd_fake_hw *hw;

	hw = ctx;

	/* Records the point while there is room. */
	if (hw->nobs < sizeof(hw->obs)) {
		hw->obs[hw->nobs] = (uint8_t)point;
		hw->nobs++;
	}

	/* The observer samples at the point. */
	if (hw->on_observe != NULL)
		hw->on_observe(hw->on_observe_ctx, point);
}

/* Takes or releases a lock (I915_LCD_LOCK_*); a lock taken twice or released free is counted. */
static void
i915_lcd_fake_lock(
	void *ctx,
	int which,
	int take)
{
	struct i915_lcd_fake_hw *hw;
	int taking;

	hw = ctx;

	/* The request as a flag: 1 to take, 0 to release. */
	taking = 0;
	if (take != 0)
		taking = 1;

	/* An unknown lock, or one already in the requested state, is a lock error. */
	if (which < 0 || which > 1 || hw->lock_held[which] == taking) {
		hw->lock_errors++;
		return;
	}

	hw->lock_held[which] = taking;
}

/* Records a callee of the Linux text that is not ported; the model has nothing to do. */
static void
i915_lcd_fake_step(
	void *ctx,
	const char *name)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(name);
}

/* Returns the DP_TP_CTL pattern field of a DPCD training pattern: PAT1 = 0, PAT2 = 1, PAT3 = 4, PAT4 = 5. */
static unsigned
i915_lcd_fake_tp_field(
	unsigned pattern)
{
	/* Maps the pattern to its field value. */
	switch (pattern) {
	case 1U:
		return 0U;
	case 2U:
		return 1U;
	case 3U:
		return 4U;
	default:
		break;
	}

	/* Succeeded: pattern 4. */
	return 5U;
}

/* Tells whether the source transmits: PLL locked, DDI clock on, DDI buffer and transport on, panel power on. */
static int
i915_lcd_fake_source_ok(
	const struct i915_lcd_fake_hw *hw,
	uint32_t tp_ctl)
{
	uint32_t ddi_buf;
	int locked;
	int clock_on;

	/* The PLL and the DDI clock. */
	locked = i915_lcd_fake_pll_locked(hw);
	if (!locked)
		return 0;

	clock_on = i915_lcd_fake_ddi_clock_on(hw);
	if (!clock_on)
		return 0;

	/* The DDI buffer and the transport. */
	ddi_buf = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_DDI_BUF_CTL(hw->port));
	if ((ddi_buf & I915_LCD_FAKE_DDI_BUF_CTL_ENABLE) == 0U)
		return 0;
	if ((tp_ctl & I915_LCD_FAKE_DP_TP_CTL_ENABLE) == 0U)
		return 0;

	/* The panel's power. */
	if ((hw->dpf->pp_control & I915_LCD_FAKE_PPC_POWER_ON) == 0U)
		return 0;

	/* Succeeded: the source transmits. */
	return 1;
}

/*
 * The sink's side of link training, as the DP standard describes the
 * receiver: told about every DPCD write, it answers a TRAINING_PATTERN_SET or
 * TRAINING_LANEx_SET write with the lane status and the adjustment it wants.
 */
static void
i915_lcd_fake_sink_on_dpcd_write(
	void *ctx,
	unsigned addr,
	unsigned len)
{
	struct i915_lcd_fake_hw *hw;
	uint8_t *dpcd;
	unsigned lanes;
	unsigned lane;
	unsigned pattern;
	unsigned all_eq;
	unsigned vswing;
	unsigned preemph;
	unsigned tp_field;
	uint32_t tp_ctl;
	uint8_t set;
	uint8_t lane_status;
	uint8_t adjust;
	int source_ok;

	hw = ctx;
	dpcd = hw->dpf->dpcd;

	/* Only TRAINING_PATTERN_SET (0x102) and TRAINING_LANEx_SET (0x103..0x106) matter. */
	if (addr > 0x106U || addr + len <= 0x102U)
		return;

	if (addr <= 0x102U)
		hw->training_pattern_writes++;

	/* The pattern and the lane count as the source set them. */
	pattern = dpcd[0x102] & 0x0fU;
	lanes = dpcd[0x101] & 0x1fU;
	if (lanes > 4U)
		lanes = 4U;

	/* Training over: the status stays as it is. */
	if (pattern == 0U)
		return;

	/* Training needs the panel's power, and the pattern the source transmits. */
	if ((hw->dpf->pp_control & I915_LCD_FAKE_PPC_POWER_ON) == 0U)
		hw->training_without_panel_power++;

	tp_ctl = drv_i915_lcd_fake_reg(hw, I915_LCD_FAKE_DP_TP_CTL(hw->pipe));
	tp_field = i915_lcd_fake_tp_field(pattern);
	if (((tp_ctl & I915_LCD_FAKE_DP_TP_CTL_TRAIN_MASK) >> 8) != tp_field)
		hw->pattern_mismatch++;

	source_ok = i915_lcd_fake_source_ok(hw, tp_ctl);

	/* Each lane reports CR at the wanted swing, then EQ and symbol lock at the wanted pre-emphasis (not in pattern 1). */
	dpcd[0x202] = 0U;
	dpcd[0x203] = 0U;
	all_eq = 1U;
	for (lane = 0U; lane < lanes; lane++) {
		set = dpcd[0x103U + lane];
		vswing = set & 3U;
		preemph = (set >> 3) & 3U;
		lane_status = 0U;
		if (source_ok && !hw->fault_cr_never && vswing >= hw->sink_want_vswing)
			lane_status |= 1U;
		if ((lane_status & 1U) != 0U &&
		    pattern != 1U &&
		    !hw->fault_eq_never &&
		    preemph >= hw->sink_want_preemph)
			lane_status |= 2U | 4U;
		if ((lane_status & 2U) == 0U)
			all_eq = 0U;

		dpcd[0x202U + lane / 2U] |= (uint8_t)(lane_status << (4U * (lane & 1U)));
	}

	/* DP_INTERLANE_ALIGN_DONE once every lane equalised past pattern 1. */
	dpcd[0x204] = 0U;
	if (pattern != 1U && all_eq != 0U && lanes != 0U)
		dpcd[0x204] = 1U;

	/* ADJUST_REQUEST: what the sink wants, for every lane. */
	adjust = (uint8_t)((hw->sink_want_vswing & 3U) |
			   ((hw->sink_want_preemph & 3U) << 2) |
			   ((hw->sink_want_vswing & 3U) << 4) |
			   ((hw->sink_want_preemph & 3U) << 6));
	dpcd[0x206] = adjust;
	dpcd[0x207] = adjust;

	/* Counts the status reports. */
	if ((dpcd[0x202] & 1U) != 0U)
		hw->link_status_cr_done++;
	if ((dpcd[0x204] & 1U) != 0U)
		hw->link_status_eq_done++;
}
