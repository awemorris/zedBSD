/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_display_power.c),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

/*
 * The display power wells, domains and core (see power.h).
 *
 * intel_power_domains_init() builds the platform's power-well descriptors
 * (display version 13: the XE_LPD set, version 12: the Tiger Lake set) and
 * the map from each power domain to the wells that provide it, in the
 * reference order and with the reference ids, control indices and
 * attributes.  A well's descriptor is separate from its live state (the
 * software reference count and the last known hardware state).
 *
 * The well operations are the reference's hsw / icl / DC_off bodies: the
 * driver request bit, the fuse waits of the fused wells, the takeover of a
 * request the firmware left, and the DC state the DMC enters when DC_off is
 * released.  Domain gets take the domain's wells in ascending order and
 * puts give them back in reverse; a last reference may also be put
 * asynchronously and is then parked until the delayed work releases it.
 *
 * The display-core bring-up (icl_display_core_init) and its caller
 * intel_power_domains_init_hw() run the DC-state disable, the PCH reset
 * handshake, the combo PHYs, power well 1, the CDCLK, the DBUF slices, the
 * MBUS credits, BW_BUDDY and the workarounds, then keep POWER_DOMAIN_INIT
 * until the driver probe gives it back.
 */

#include "modeset-internal.h"
#include "power.h"
#include "clock.h"
#include "phy.h"
#include "takeover.h"
#include "modeset.h"
#include <kern/kcrt.h>

#include "../mmio.h"
#include "../sync.h"
#include "../trace.h"

#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#include <uapi/errno.h>

/* The trace stage of the display noirq bring-up (the old probe's stage P3). */
#define I915_POWER_TRACE_STAGE_NOIRQ	4U

/* The pipe bits of a well's interrupt pipe mask. */
#define BIT_PIPE_A	(1u << 0)
#define BIT_PIPE_B	(1u << 1)
#define BIT_PIPE_C	(1u << 2)
#define BIT_PIPE_D	(1u << 3)

/* The reference's HSW / ICL power-well control indices (i915_reg.h). */
#define IDX_PW_1	0u
#define IDX_PW_2	1u
#define IDX_PW_A	5u
#define IDX_PW_B	6u
#define IDX_PW_C	7u
#define IDX_PW_D	8u
#define IDX_DDI_A	0u
#define IDX_DDI_B	1u
#define IDX_DDI_C	2u
#define IDX_DDI_D	7u	/* XELPD */
#define IDX_DDI_E	8u	/* XELPD */
#define IDX_DDI_TC1	3u
#define IDX_DDI_TC2	4u
#define IDX_DDI_TC3	5u
#define IDX_DDI_TC4	6u
#define IDX_AUX_A	0u
#define IDX_AUX_B	1u
#define IDX_AUX_C	2u
#define IDX_AUX_D	7u	/* XELPD */
#define IDX_AUX_E	8u	/* XELPD */
#define IDX_AUX_TC1	3u
#define IDX_AUX_TC2	4u
#define IDX_AUX_TC3	5u
#define IDX_AUX_TC4	6u
#define IDX_AUX_TBT1	9u
#define IDX_AUX_TBT2	10u
#define IDX_AUX_TBT3	11u
#define IDX_AUX_TBT4	12u

/* Tiger Lake (display version 12) adds PW_3..PW_5. */
#define IDX_PW_3	2u
#define IDX_PW_4	3u
#define IDX_PW_5	4u

/* The driver request registers of the well families (i915_reg.h). */
#define ICL_PWR_WELL_CTL_AUX2	0x45444u
#define ICL_PWR_WELL_CTL_DDI2	0x45454u
#define HSW_PWR_WELL_CTL2	0x45404u

/* The fuse-distribution status of the power gates (gen9_wait_for_power_well_fuses). */
#define SKL_FUSE_STATUS		0x42000u
#define SKL_PG0			0u
#define SKL_PG1			1u
#define GEN8_CHICKEN_DCPR_1	0x46430u
#define DISABLE_FLR_SRC		(1u << 15)

/* The DC state register and what the DC6 check reads (i915_reg.h). */
#define DC_STATE_EN			0x45504u
#define DC_STATE_EN_UPTO_DC5_DC6	0x3u
#define DC_STATE_EN_DC3CO		0x40000000u
#define UTIL_PIN_CTL			0x48400u
#define UTIL_PIN_ENABLE			0x80000000u
#define UTIL_PIN_MODE_MASK		(0x1fu << 24)
#define UTIL_PIN_MODE_PWM		(1u << 24)

/* The PCH reset handshake and the DC power-request workarounds (i915_reg.h). */
#define HSW_NDE_RSTWRN_OPT		0x46408u
#define RESET_PCH_HANDSHAKE_ENABLE	(1u << 4)
#define GEN11_CHICKEN_DCPR_2		0x46434u
#define DCPR_CLEAR_MEMSTAT_DIS		(1u << 24)
#define DCPR_SEND_RESP_IMM		(1u << 25)
#define DCPR_MASK_LPMODE		(1u << 26)
#define DCPR_MASK_MAXLATENCY_MEMUP_CLR	(1u << 27)
#define XELPD_DISPLAY_ERR_FATAL_MASK	0x4421cu

/* The DBUF slice control bits (skl_watermark_regs.h). */
#define DBUF_POWER_REQUEST		(1u << 31)
#define DBUF_POWER_STATE		(1u << 30)
#define DBUF_TRACKER_STATE_SERVICE_MASK	(0x1fu << 19)
#define DBUF_TRACKER_STATE_SERVICE_8	(8u << 19)

/* The MBUS ABOX credit registers (i915_reg.h): ABOX0, then ABOX1 / ABOX2. */
#define MBUS_ABOX0_CTL			0x45038u
#define MBUS_ABOX1_CTL			0x45048u
#define MBUS_ABOX2_CTL			0x4504cu
#define MBUS_ABOX_BW_CREDIT_MASK	(3u << 20)
#define MBUS_ABOX_B_CREDIT_MASK		(0xfu << 16)
#define MBUS_ABOX_BT_CREDIT_POOL2_MASK	(0x1fu << 8)
#define MBUS_ABOX_BT_CREDIT_POOL1_MASK	(0x1fu << 0)
#define MBUS_ABOX_BW_CREDIT_1		(1u << 20)
#define MBUS_ABOX_B_CREDIT_1		(1u << 16)
#define MBUS_ABOX_BT_CREDIT_POOL2_16	(16u << 8)
#define MBUS_ABOX_BT_CREDIT_POOL1_16	(16u << 0)

/* BW_BUDDY, one pair per ABOX (i915_reg.h); the device info says which ABOXes exist. */
#define BW_BUDDY_CTL_BASE		0x45130u
#define BW_BUDDY_PAGE_MASK_BASE		0x45134u
#define BW_BUDDY_STRIDE			0x10u
#define BW_BUDDY_TLB_REQ_TIMER_MASK	(0x3fu << 16)
#define BW_BUDDY_TLB_REQ_TIMER_8	(0x8u << 16)
#define BW_BUDDY_DISABLE		(1u << 31)

/* The reference's delay of an asynchronous put when the caller names none. */
#define I915_POWER_ASYNC_PUT_DELAY_MS	100

/*
 * One power well of a platform's descriptor set, as
 * intel_display_power_map_init() lists it.
 *
 * An instance is one row of a constant table; the power-domain map copies
 * it into a struct i915_power_well that carries the live state as well.
 */
struct i915_pw_desc {
	/* The well's name, as the logs print it. */
	const char *name;

	/* The operations family the well uses. */
	enum i915_pw_ops_kind ops;

	/* The descriptor attributes: always on, VGA, fuses, Thunderbolt AUX, fixed enable delay. */
	int always_on;
	int has_vga;
	int has_fuses;
	int is_tc_tbt;
	int fixed_enable_delay;

	/* The enable timeout in milliseconds (0: the operations' default). */
	unsigned enable_timeout;

	/* The pipes whose interrupts live in the well. */
	unsigned irq_pipe_mask;

	/* The well's control index and its power-well id. */
	unsigned hsw_idx;
	int id;

	/* Nonzero for the zero-length domain list that means every domain. */
	int domains_all;

	/* The domains the well provides. */
	const enum i915_power_domain *domains;
	unsigned domain_count;
};

/*
 * One row of tgl_buddy_page_masks[]: the BW_BUDDY page mask of one DRAM
 * configuration.
 */
struct i915_buddy_mask {
	/* The number of DRAM channels (0 ends the table). */
	unsigned nch;

	/* The DRAM type (enum i915_dram_type). */
	int type;

	/* The page mask BW_BUDDY is given. */
	uint32_t page_mask;
};

/*
 * The DBUF slice control registers, slice S1 first (skl_watermark_regs.h:
 * _DBUF_CTL_S0 0x45008, _DBUF_CTL_S1 0x44FE8, _DBUF_CTL_S2 0x44300,
 * _DBUF_CTL_S3 0x44304).
 *
 * The table once began at 0x44FE8 and ended at 0x44308: slice 1 then
 * powered the second slice and the fourth request went to a register that
 * is not a DBUF control.  It never changes.
 */
static const uint32_t i915_dbuf_ctl_s[4] = { 0x45008u, 0x44FE8u, 0x44300u, 0x44304u };

/*
 * tgl_buddy_page_masks[]: the BW_BUDDY page mask of each known DRAM
 * configuration, ended by a row of zero channels.  It never changes.
 */
static const struct i915_buddy_mask i915_tgl_buddy[] = {
	{ 1, I915_DRAM_DDR4, 0x0Fu },
	{ 1, I915_DRAM_DDR5, 0x0Fu },
	{ 2, I915_DRAM_LPDDR4, 0x1Cu },
	{ 2, I915_DRAM_LPDDR5, 0x1Cu },
	{ 2, I915_DRAM_DDR4, 0x1Fu },
	{ 2, I915_DRAM_DDR5, 0x1Eu },
	{ 4, I915_DRAM_LPDDR4, 0x38u },
	{ 4, I915_DRAM_LPDDR5, 0x38u },
	{ 0, 0, 0u }
};

static void i915_pw_dom(struct i915_pw_domain_mask *mask, enum i915_power_domain domain);
static uint32_t i915_get_allowed_dc_mask(unsigned display_ver, int enable_dc);
static uint32_t i915_sanitize_target_dc_state(const struct i915_power_domains *pd, uint32_t target);
static int i915_sanitize_disable_power_well(int param);
static struct i915_power_well *i915_pw_add(struct i915_power_domains *pd, const struct i915_pw_desc *desc);
static void i915_power_map_build(struct i915_power_domains *pd, const struct i915_pw_desc *descs, unsigned count);
static int i915_power_map_init(struct i915_power_domains *pd);
static int i915_power_map_init_tgl(struct i915_power_domains *pd);
static unsigned i915_pw_driver_reg(enum i915_pw_ops_kind ops);
static uint32_t i915_pw_req(unsigned idx);
static uint32_t i915_pw_state(unsigned idx);
static int i915_pw_wait_fuse(struct i915_pw_ctx *pwc, unsigned pg);
static void i915_pw_post_enable(struct i915_power_well *well, struct i915_pw_ctx *pwc);
static int i915_mask_test(const struct i915_pw_domain_mask *mask, unsigned domain);
static void i915_mask_set(struct i915_pw_domain_mask *mask, unsigned domain);
static void i915_mask_clear(struct i915_pw_domain_mask *mask, unsigned domain);
static int i915_mask_empty(const struct i915_pw_domain_mask *mask);
static int i915_parked(struct i915_power_domains *pd, unsigned domain);
static int i915_parked_any(struct i915_power_domains *pd);
static void i915_verify_async_put_domains_state(struct i915_power_domains *pd);
static void i915_cancel_async_put_work(struct i915_power_domains *pd, int sync);
static int i915_grab_async_put_ref(struct i915_power_domains *pd, unsigned domain);
static int i915_get_domain_locked(struct i915_power_domains *pd, enum i915_power_domain domain, struct i915_pw_ctx *pwc);
static void i915_put_domain_locked(struct i915_power_domains *pd, enum i915_power_domain domain, struct i915_pw_ctx *pwc);
static void i915_queue_async_put_domains_work(struct i915_power_domains *pd, int delay_ms);
static void i915_release_async_put_domains(struct i915_power_domains *pd, const struct i915_pw_domain_mask *mask);
static uint32_t i915_gen9_dc_mask(int display_ver);
static void i915_gen9_write_dc_state(struct i915_pw_ctx *pwc, uint32_t state);
static void i915_power_rmw(struct i915_mmio *mmio, uint32_t reg, uint32_t clear, uint32_t set);
static struct i915_trace *i915_core_trace(struct i915_display_core *dc);
static struct i915_trace *i915_dc_off_trace(struct i915_pw_ctx *pwc);
static uint8_t i915_dbuf_slice_mask(const struct i915_display_core *dc);
static void i915_gen9_set_dc_state_disable(struct i915_display_core *dc);
static void i915_pch_reset_handshake(struct i915_display_core *dc);
static void i915_gen9_dbuf_slice_set(struct i915_display_core *dc, unsigned slice, int enable);
static void i915_gen9_dbuf_slices_update(struct i915_display_core *dc, uint8_t req_slices);
static void i915_gen12_dbuf_slices_config(struct i915_display_core *dc);
static void i915_gen9_dbuf_enable(struct i915_display_core *dc);
static void i915_icl_mbus_init(struct i915_display_core *dc);
static void i915_tgl_bw_buddy_init(struct i915_display_core *dc);
static void i915_core_fault(struct i915_display_core *dc, const char *where);
static int i915_icl_display_core_init(struct i915_display_core *dc, int resume);
static unsigned i915_wells_on(struct i915_power_domains *pd, struct i915_pw_ctx *pwc);

/*
 * Builds the power-domain state of a display (intel_power_domains_init()).
 *
 * Sanitizes the power-well and DC options, computes the allowed DC states
 * and the target DC state, prepares the lock and the asynchronous put, and
 * builds the platform's power-well map.  Returns 0, or EINVAL when there is
 * no state or no map for the display version.
 */
int
drv_i915_power_domains_init(
	struct i915_power_domains *pd,
	unsigned display_ver,
	int enable_dc_param,
	int disable_pw_param,
	struct i915_trace *trace)
{
	unsigned domain;
	int map_result;
	const char *map_name;

	/* Refuses a missing state. */
	if (pd == NULL)
		return EINVAL;

	/* Sanitizes the options and derives the DC states they allow. */
	pd->disable_power_well = i915_sanitize_disable_power_well(disable_pw_param);
	pd->allowed_dc_mask = i915_get_allowed_dc_mask(display_ver, enable_dc_param);
	pd->target_dc_state = i915_sanitize_target_dc_state(pd, I915_DC_STATE_EN_UPTO_DC6);

	/* Prepares the lock and marks the asynchronous put work as initialized. */
	(void)mutex_init(&pd->lock, LOCK_RANK_DEVICE, "i915-power-domains");
	pd->async_put_work_inited = 1;

	/* Starts the use counts empty, whatever the caller's memory held. */
	for (domain = 0u; domain < I915_PW_DOMAIN_NUM; domain++)
		pd->domain_use_count[domain] = 0u;

	/* Starts the asynchronous put with nothing parked and no work bound. */
	pd->async_put_domains[0].bits[0] = 0u;
	pd->async_put_domains[0].bits[1] = 0u;
	pd->async_put_domains[1].bits[0] = 0u;
	pd->async_put_domains[1].bits[1] = 0u;
	pd->async_put_wakeref = 0;
	pd->async_put_next_delay = 0;
	pd->async_ops = NULL;
	pd->async_ctx = NULL;
	pd->async_pwc = NULL;

	/* Starts every diagnostic counter at zero. */
	pd->async_puts = 0u;
	pd->async_parked = 0u;
	pd->async_grabs = 0u;
	pd->async_work_runs = 0u;
	pd->async_work_empty = 0u;
	pd->async_released = 0u;
	pd->async_requeues = 0u;
	pd->async_flushes = 0u;
	pd->async_state_errors = 0u;
	pd->use_count_errors = 0u;

	/* No map is built yet. */
	pd->map_initialized = 0;
	pd->num_power_wells = 0u;

	/*
	 * Builds the map of the display version: 13 is XE_LPD (Alder Lake-P),
	 * 12 is Tiger Lake.  A version without a map is refused rather than
	 * programmed with another platform's wells.
	 */
	if (display_ver >= 13u) {
		map_result = i915_power_map_init(pd);
		if (map_result != 0)
			return EINVAL;
	} else if (display_ver == 12u) {
		map_result = i915_power_map_init_tgl(pd);
		if (map_result != 0)
			return EINVAL;
	} else {
		kern_logf("i915: P3 intel_power_domains_init: no power-well map for display version %u\n",
			display_ver);
		return EINVAL;
	}

	/* Publishes the state as initialized and records it. */
	pd->initialized = 1;
	drv_i915_trace_record(trace, I915_POWER_TRACE_STAGE_NOIRQ, I915_TRACE_ACQUIRE,
		"intel_power_domains_init", (uint64_t)pd->num_power_wells,
		(uint64_t)pd->allowed_dc_mask);

	/* Names the map for the log. */
	map_name = "tgl";
	if (display_ver >= 13u)
		map_name = "xelpd";

	/* Logs what was built. */
	kern_logf("i915: P3 intel_power_domains_init: wells=%u allowed_dc=0x%x "
		"target_dc=0x%x disable_pw=%d (%s map)\n",
		pd->num_power_wells, pd->allowed_dc_mask, pd->target_dc_state,
		pd->disable_power_well, map_name);

	/* Succeeded: the map and the options are ready for the hardware init. */
	return 0;
}

/*
 * Drops the power-well map (intel_power_domains_cleanup()).
 *
 * No register is touched: this path is only valid before any power get.
 */
void
drv_i915_power_domains_cleanup(
	struct i915_power_domains *pd)
{
	/* Nothing to drop without an initialized state. */
	if (pd == NULL)
		return;
	if (!pd->initialized)
		return;

	/* Forgets the wells and the map (intel_display_power_map_cleanup()). */
	pd->num_power_wells = 0u;
	pd->map_initialized = 0;
	pd->initialized = 0;
}

/*
 * Reports the bitmask of the wells that provide a domain.
 */
uint64_t
drv_i915_power_domain_wells(
	const struct i915_power_domains *pd,
	enum i915_power_domain domain)
{
	/* A missing state or an unknown domain has no wells. */
	if (pd == NULL)
		return 0u;
	if ((unsigned)domain >= I915_PW_DOMAIN_NUM)
		return 0u;

	/* Succeeded: reports the domain's wells. */
	return pd->domain_wells[domain];
}

/*
 * Finds the index of the well with a power-well id, or -1.
 */
int
drv_i915_power_well_by_id(
	const struct i915_power_domains *pd,
	int id)
{
	unsigned index;

	/* A missing state and DISP_PW_ID_NONE name no well. */
	if (pd == NULL)
		return -1;
	if (id == I915_DISP_PW_ID_NONE)
		return -1;

	/* Looks the id up among the wells. */
	for (index = 0u; index < pd->num_power_wells; index++) {
		if (pd->power_wells[index].id == id)
			return (int)index;
	}

	/* No well has the id. */
	return -1;
}

/*
 * Turns a power well on (the well's enable operation).
 *
 * The reference's enable waits are void: a plain acknowledge timeout is
 * warned and the enable continues.  Returns 0, or EIO when the time base
 * failed and the caller must stop.
 */
int
drv_i915_power_well_enable(
	struct i915_power_well *well,
	struct i915_pw_ctx *pwc)
{
	unsigned reg;
	uint32_t req;
	uint32_t st;
	uint32_t value;
	uint32_t chicken;
	unsigned pg;
	unsigned timeout_ms;
	int wait_result;

	/* An always-on well is simply on. */
	if (well->ops == I915_PW_OPS_ALWAYS_ON) {
		well->hw_enabled = 1;
		return 0;
	}

	/* DC_off is enabled by leaving the DC states (gen9_dc_off_power_well_enable()). */
	if (well->ops == I915_PW_OPS_DC_OFF) {
		drv_i915_dc_off_enable(pwc);
		well->hw_enabled = 1;
		return 0;
	}

	/* Names the driver request register and the well's request and state bits. */
	reg = i915_pw_driver_reg(well->ops);
	req = i915_pw_req(well->hsw_idx);
	st = i915_pw_state(well->hsw_idx);

	/*
	 * hsw_power_well_enable() for a fused well: power well 1 (PG1) waits
	 * for PG0's fuses before the enable (after Wa_16013190616:adlp); every
	 * fused well waits for its own fuses after it.  The power gate is
	 * ICL_PW_CTL_IDX_TO_PG(idx) = idx + SKL_PG1 on display version 11+.
	 */
	if (well->has_fuses) {
		pg = well->hsw_idx + SKL_PG1;
		if (pg == SKL_PG1) {
			/* Wa_16013190616:adlp. */
			chicken = drv_i915_raw_read32(pwc->mmio, GEN8_CHICKEN_DCPR_1);
			drv_i915_raw_write32(pwc->mmio, GEN8_CHICKEN_DCPR_1, chicken | DISABLE_FLR_SRC);

			/* Waits for PG0's fuses, stopping on a time-base fault. */
			wait_result = i915_pw_wait_fuse(pwc, SKL_PG0);
			if (wait_result == EIO)
				return EIO;
		}
	}

	/* Sets the driver request bit (intel_de_rmw(driver, 0, REQ)). */
	value = drv_i915_raw_read32(pwc->mmio, reg);
	drv_i915_raw_write32(pwc->mmio, reg, value | req);

	/*
	 * hsw_wait_for_power_well_enable(): waits for the state bit.  A fixed
	 * enable delay is used only on DG2, so the acknowledge is waited for,
	 * with the descriptor's enable timeout when it sets one.
	 */
	timeout_ms = 2u;
	if (well->enable_timeout != 0u)
		timeout_ms = well->enable_timeout;
	wait_result = drv_i915_wait_reg(pwc->mmio, reg, st, st, 0u, timeout_ms, NULL);
	if (wait_result == EIO)
		return EIO;

	/*
	 * A real acknowledge timeout is warned and the enable continues (AUX in
	 * particular expects one); it is recorded, never unwound.
	 */
	if (wait_result != 0) {
		pwc->ack_timeouts++;
		kern_logf("i915: power well %s enable ACK timeout (continuing)\n",
			well->name);
	}

	/* A fused well waits for its own power gate's fuses. */
	if (well->has_fuses) {
		pg = well->hsw_idx + SKL_PG1;
		wait_result = i915_pw_wait_fuse(pwc, pg);
		if (wait_result == EIO)
			return EIO;
	}

	/* Records the well as on and runs its post-enable steps. */
	well->hw_enabled = 1;
	i915_pw_post_enable(well, pwc);

	/* Succeeded: the well is on. */
	return 0;
}

/*
 * Turns a power well off (the well's disable operation).
 *
 * Returns 0, or EBUSY when a pipe's interrupt drain failed: the well is
 * then left on, and so is every well after it.
 */
int
drv_i915_power_well_disable(
	struct i915_power_well *well,
	struct i915_pw_ctx *pwc)
{
	unsigned reg;
	uint32_t req;
	uint32_t st;
	uint32_t value;
	uint32_t bios_value;
	uint32_t util_pin;
	uint32_t dc_state;
	int drain_result;

	/* A failed interrupt drain refuses every later disable of a real well. */
	if (pwc->irq_sync_failed && well->ops != I915_PW_OPS_ALWAYS_ON) {
		pwc->disable_refusals++;
		return EBUSY;
	}

	/* An always-on well is never actually disabled. */
	if (well->ops == I915_PW_OPS_ALWAYS_ON) {
		well->hw_enabled = 1;
		return 0;
	}

	/* gen9_dc_off_power_well_disable(): enters the target DC state once the DMC payload is loaded. */
	if (well->ops == I915_PW_OPS_DC_OFF) {
		pwc->dc_off_disable_calls++;
		if (pwc->dmc_has_payload) {
			/* Enters the target DC state. */
			switch (pwc->target_dc_state) {
			case I915_DC_STATE_EN_DC3CO:
				/* tgl_enable_dc3co() */
				drv_i915_gen9_set_dc_state(pwc, I915_DC_STATE_EN_DC3CO);
				break;
			case I915_DC_STATE_EN_UPTO_DC6:
				/* skl_enable_dc6(): assert_can_enable_dc6() only warns. */
				util_pin = drv_i915_raw_read32(pwc->mmio, UTIL_PIN_CTL);
				if ((util_pin & (UTIL_PIN_ENABLE | UTIL_PIN_MODE_MASK)) == (UTIL_PIN_ENABLE | UTIL_PIN_MODE_PWM))
					kern_logf("i915: DC6: utility pin enabled in PWM mode\n");

				/* Warns when DC6 is already requested. */
				dc_state = drv_i915_raw_read32(pwc->mmio, DC_STATE_EN);
				if ((dc_state & I915_DC_STATE_EN_UPTO_DC6) != 0u)
					kern_logf("i915: DC6 already programmed to be enabled\n");

				/* Requests DC6. */
				drv_i915_gen9_set_dc_state(pwc, I915_DC_STATE_EN_UPTO_DC6);
				break;
			case I915_DC_STATE_EN_UPTO_DC5:
				/* gen9_enable_dc5(): Wa Display #1183 is display version 9 only. */
				drv_i915_gen9_set_dc_state(pwc, I915_DC_STATE_EN_UPTO_DC5);
				break;
			default:
				break;
			}
		}

		/* Records whether DC_off still reads as enabled. */
		well->hw_enabled = drv_i915_power_well_is_enabled(well, pwc);
		return 0;
	}

	/* Names the driver request register and the well's request and state bits. */
	reg = i915_pw_driver_reg(well->ops);
	req = i915_pw_req(well->hsw_idx);
	st = i915_pw_state(well->hsw_idx);

	/*
	 * hsw_power_well_pre_disable() -> gen8_irq_power_well_pre_disable():
	 * stops the pipe's interrupt sources and waits for a handler already
	 * running before the well goes off; afterwards the handler would read a
	 * powered-off register.
	 */
	if (well->irq_pipe_mask != 0u && pwc->irqs_enabled && !pwc->irq_sync_failed) {
		pwc->irq_pre_disable_calls++;
		if (pwc->irq_ops != NULL) {
			/* Drains the pipe's interrupts through the interrupt layer. */
			drain_result = pwc->irq_ops->pre_disable(pwc->irq_ctx, well->irq_pipe_mask);

			/* The handler was not shown to have left the pipe: this well and every later one stay on. */
			if (drain_result != 0) {
				pwc->irq_sync_failed = 1;
				kern_logf("i915: power well %s: pipe interrupt drain FAILED; the well is kept on and every later "
					"well disable is refused\n", well->name);
			}
		}
	}

	/* A failed drain leaves the request bit untouched. */
	if (pwc->irq_sync_failed) {
		pwc->disable_refusals++;
		return EBUSY;
	}

	/* Clears the driver request bit. */
	value = drv_i915_raw_read32(pwc->mmio, reg);
	drv_i915_raw_write32(pwc->mmio, reg, value & ~req);

	/*
	 * Waits for the well to turn off only when the driver was the last
	 * requester.  While the firmware still holds its request the state
	 * stays set; that is expected, not a timeout.
	 */
	bios_value = drv_i915_raw_read32(pwc->mmio, reg - 4u);
	if ((bios_value & req) == 0u)
		(void)drv_i915_wait_reg(pwc->mmio, reg, st, 0u, 0u, 2u, NULL);

	/* Records the driver's ownership (request and state both set). */
	well->hw_enabled = drv_i915_power_well_is_enabled(well, pwc);

	/* Succeeded: the driver's request is gone. */
	return 0;
}

/*
 * Tells whether a power well is on as the driver owns it (1 or 0).
 */
int
drv_i915_power_well_is_enabled(
	struct i915_power_well *well,
	struct i915_pw_ctx *pwc)
{
	unsigned reg;
	uint32_t mask;
	uint32_t dmask;
	uint32_t value;

	/* An always-on well is always on. */
	if (well->ops == I915_PW_OPS_ALWAYS_ON)
		return 1;

	/* gen9_dc_off_power_well_enabled(): DC_off is on while no DC5 / DC6 (and DC3CO) state is set. */
	if (well->ops == I915_PW_OPS_DC_OFF) {
		dmask = I915_DC_STATE_EN_UPTO_DC5 | I915_DC_STATE_EN_UPTO_DC6;
		if ((pwc->allowed_dc_mask & I915_DC_STATE_EN_DC3CO) != 0u)
			dmask |= I915_DC_STATE_EN_DC3CO;

		/* Reads the DC state. */
		value = drv_i915_raw_read32(pwc->mmio, DC_STATE_EN);
		if ((value & dmask) != 0u)
			return 0;

		/* Succeeded: no DC state is set. */
		return 1;
	}

	/* hsw_power_well_enabled(): enabled only when both the driver request and the state are set. */
	reg = i915_pw_driver_reg(well->ops);
	mask = i915_pw_req(well->hsw_idx) | i915_pw_state(well->hsw_idx);
	value = drv_i915_raw_read32(pwc->mmio, reg);
	if ((value & mask) != mask)
		return 0;

	/* Succeeded: the driver owns an enabled well. */
	return 1;
}

/*
 * Takes over a well's request from the firmware and records its state
 * (intel_power_well_sync_hw()).
 *
 * The software reference count is not touched.
 */
void
drv_i915_power_well_sync_hw(
	struct i915_power_well *well,
	struct i915_pw_ctx *pwc)
{
	unsigned driver_reg;
	unsigned bios_reg;
	uint32_t mask;
	uint32_t bios_req;
	uint32_t driver_req;

	/* An always-on well is simply on. */
	if (well->ops == I915_PW_OPS_ALWAYS_ON) {
		well->hw_enabled = 1;
		return;
	}

	/*
	 * hsw_power_well_sync_hw(): when the firmware holds the request, the
	 * driver request is set first (when it is not already) and the firmware
	 * request cleared after it -- never the reverse, and never skipped
	 * because the software count is zero.  CTL1 (the firmware's) sits just
	 * below CTL2 (the driver's).
	 */
	if (well->ops != I915_PW_OPS_DC_OFF) {
		driver_reg = i915_pw_driver_reg(well->ops);
		bios_reg = driver_reg - 4u;
		mask = i915_pw_req(well->hsw_idx);
		bios_req = drv_i915_raw_read32(pwc->mmio, bios_reg);
		if ((bios_req & mask) != 0u) {
			/* Takes the request over before the firmware's goes away. */
			driver_req = drv_i915_raw_read32(pwc->mmio, driver_reg);
			if ((driver_req & mask) == 0u)
				drv_i915_raw_write32(pwc->mmio, driver_reg, driver_req | mask);
			drv_i915_raw_write32(pwc->mmio, bios_reg, bios_req & ~mask);
		}
	}

	/* Records what the hardware now says. */
	well->hw_enabled = drv_i915_power_well_is_enabled(well, pwc);
}

/*
 * Takes one software reference on a well, turning it on for the first.
 *
 * The display-core enable of a well is a different entry that does not
 * count.  Returns 0 or the enable's error; a failed first reference is not
 * counted.
 */
int
drv_i915_power_well_get(
	struct i915_power_well *well,
	struct i915_pw_ctx *pwc)
{
	int enable_result;

	/* The first reference turns the well on. */
	enable_result = 0;
	if (well->refcount == 0u)
		enable_result = drv_i915_power_well_enable(well, pwc);

	/* Reports a failed enable without counting it. */
	if (enable_result != 0)
		return enable_result;

	/* The count is the number of holders of the well. */
	well->refcount++;

	/* Succeeded: the caller holds the well. */
	return 0;
}

/*
 * Gives one software reference on a well back, turning it off with the
 * last.
 */
void
drv_i915_power_well_put(
	struct i915_power_well *well,
	struct i915_pw_ctx *pwc)
{
	int disable_result;

	/* Refuses an underflow. */
	if (well->refcount == 0u)
		return;

	/* The count is the number of holders; the last one turns the well off. */
	well->refcount--;
	if (well->refcount != 0u)
		return;

	/* Turns the well off. */
	disable_result = drv_i915_power_well_disable(well, pwc);

	/*
	 * A refused disable leaves the well on, now owned by the failed stop:
	 * it is never re-enabled on a later get.
	 */
	if (disable_result == EBUSY) {
		well->refcount = 1u;
		pwc->kept_wells++;
	}
}

/*
 * Tells whether a domain counts as enabled (__intel_display_power_is_enabled()).
 *
 * The reference walks the domain's wells in reverse against the recorded
 * state, not a fresh hardware read, so an interrupt-reset path never
 * touches a register to answer.  Always-on wells are skipped.  Returns 1 or
 * 0.
 */
int
drv_i915_display_power_is_enabled(
	struct i915_power_domains *pd,
	enum i915_power_domain domain,
	struct i915_pw_ctx *pwc)
{
	uint64_t wells;
	unsigned index;
	struct i915_power_well *well;

	UNUSED_PARAMETER(pwc);

	/* Walks the domain's wells, last one first. */
	wells = drv_i915_power_domain_wells(pd, domain);
	index = pd->num_power_wells;
	while (index-- > 0u) {
		if ((wells & ((uint64_t)1u << index)) == 0u)
			continue;

		/* Skips an always-on well. */
		well = &pd->power_wells[index];
		if (well->always_on)
			continue;

		/* A well not known to be on (-1 until the first sync) is not enabled. */
		if (well->hw_enabled != 1)
			return 0;
	}

	/* Succeeded: every well of the domain is on. */
	return 1;
}

/*
 * Tells whether every well of a domain is powered now, whoever requested
 * it (1 or 0).
 *
 * Read only: the state bit of the driver request register mirrors the
 * well's real state, and no request bit is read or written.  Always-on
 * wells count as on; DC_off gates no register access and is skipped.
 * Usable before intel_power_domains_init_hw().
 */
int
drv_i915_power_domain_hw_state_on(
	struct i915_power_domains *pd,
	enum i915_power_domain domain,
	struct i915_mmio *mmio)
{
	uint64_t wells;
	unsigned index;
	struct i915_power_well *well;
	uint32_t value;
	uint32_t state_bit;

	/* Walks the domain's wells, last one first. */
	wells = drv_i915_power_domain_wells(pd, domain);
	index = pd->num_power_wells;
	while (index-- > 0u) {
		if ((wells & ((uint64_t)1u << index)) == 0u)
			continue;

		/* Skips the wells that gate no register. */
		well = &pd->power_wells[index];
		if (well->always_on || well->ops == I915_PW_OPS_ALWAYS_ON || well->ops == I915_PW_OPS_DC_OFF)
			continue;

		/* Reads the well's state bit. */
		value = drv_i915_raw_read32(mmio, i915_pw_driver_reg(well->ops));
		state_bit = i915_pw_state(well->hsw_idx);
		if ((value & state_bit) == 0u)
			return 0;
	}

	/* Succeeded: every gating well of the domain is powered. */
	return 1;
}

/*
 * Takes one reference on a display power domain (intel_display_power_get()).
 *
 * Returns 0, or the error of the well that could not be turned on (the
 * wells already taken are given back).
 */
int
drv_i915_display_power_get(
	struct i915_power_domains *pd,
	enum i915_power_domain domain,
	struct i915_pw_ctx *pwc)
{
	int get_result;

	/* Takes the domain under the domains lock. */
	mutex_lock(&pd->lock);

	get_result = i915_get_domain_locked(pd, domain, pwc);

	mutex_unlock(&pd->lock);

	/* Reports a well that did not turn on. */
	if (get_result != 0)
		return get_result;

	/* Succeeded: the caller holds the domain. */
	return 0;
}

/*
 * Gives one reference on a display power domain back (intel_display_power_put()).
 */
void
drv_i915_display_power_put(
	struct i915_power_domains *pd,
	enum i915_power_domain domain,
	struct i915_pw_ctx *pwc)
{
	/* Puts the domain under the domains lock. */
	mutex_lock(&pd->lock);

	i915_put_domain_locked(pd, domain, pwc);

	mutex_unlock(&pd->lock);
}

/*
 * Binds the delayed work behind the asynchronous put and the well context
 * it releases with.
 */
void
drv_i915_display_power_async_bind(
	struct i915_power_domains *pd,
	const struct i915_pw_async_ops *ops,
	void *ctx,
	struct i915_pw_ctx *pwc)
{
	/* Publishes the work under the domains lock. */
	mutex_lock(&pd->lock);

	pd->async_ops = ops;
	pd->async_ctx = ctx;
	pd->async_pwc = pwc;

	mutex_unlock(&pd->lock);
}

/*
 * Gives a reference back asynchronously (__intel_display_power_put_async()).
 *
 * A last reference is parked and released by the delayed work; any other
 * reference, or any reference with no work bound, is put at once.  A
 * negative delay is the reference's default of 100 ms.
 */
void
drv_i915_display_power_put_async(
	struct i915_power_domains *pd,
	enum i915_power_domain domain,
	struct i915_pw_ctx *pwc,
	int delay_ms)
{
	/* Takes the reference's default delay when none is named. */
	if (delay_ms < 0)
		delay_ms = I915_POWER_ASYNC_PUT_DELAY_MS;

	/* Parks or puts the reference under the domains lock. */
	mutex_lock(&pd->lock);

	pd->async_puts++;
	if (pd->async_ops == NULL || pd->domain_use_count[domain] > 1u) {
		/* Not the last reference, or no delayed work to hand it to: an ordinary put. */
		i915_put_domain_locked(pd, domain, pwc);
	} else {
		/* Counts a use count that is not the single reference being parked. */
		if (pd->domain_use_count[domain] != 1u)
			pd->use_count_errors++;

		/* Lets a pending work requeue itself, or queues a new one. */
		pd->async_parked++;
		if (pd->async_put_wakeref != 0) {
			i915_mask_set(&pd->async_put_domains[1], (unsigned)domain);
			if (delay_ms > pd->async_put_next_delay)
				pd->async_put_next_delay = delay_ms;
		} else {
			i915_mask_set(&pd->async_put_domains[0], (unsigned)domain);
			i915_queue_async_put_domains_work(pd, delay_ms);
		}
	}

	/* Checks the invariants whatever happened. */
	i915_verify_async_put_domains_state(pd);

	mutex_unlock(&pd->lock);
}

/*
 * Runs the delayed work of the asynchronous put
 * (intel_display_power_put_async_work()).
 */
void
drv_i915_display_power_async_work(
	struct i915_power_domains *pd)
{
	int more_parked;

	/* Releases what is parked under the domains lock. */
	mutex_lock(&pd->lock);

	pd->async_work_runs++;
	if (pd->async_put_wakeref == 0) {
		/* Every parked reference was grabbed back by a later get or a flush. */
		pd->async_work_empty++;
	} else {
		/* The work owns the parked references until it has released them. */
		pd->async_put_wakeref = 0;
		i915_release_async_put_domains(pd, &pd->async_put_domains[0]);

		/* Requeues the work for what was parked meanwhile, or cancels a work queued after this one was dequeued. */
		more_parked = !i915_mask_empty(&pd->async_put_domains[1]);
		if (more_parked) {
			pd->async_put_domains[0] = pd->async_put_domains[1];
			pd->async_put_domains[1].bits[0] = 0u;
			pd->async_put_domains[1].bits[1] = 0u;
			pd->async_requeues++;
			i915_queue_async_put_domains_work(pd, pd->async_put_next_delay);
			pd->async_put_next_delay = 0;
		} else {
			i915_cancel_async_put_work(pd, 0);
		}
	}

	/* Checks the invariants whatever happened. */
	i915_verify_async_put_domains_state(pd);

	mutex_unlock(&pd->lock);
}

/*
 * Releases every parked reference now (intel_display_power_flush_work()).
 */
void
drv_i915_display_power_flush_work(
	struct i915_power_domains *pd)
{
	struct i915_pw_domain_mask all;

	/* Releases what is parked under the domains lock (nothing when the work owns nothing). */
	mutex_lock(&pd->lock);

	pd->async_flushes++;
	if (pd->async_put_wakeref != 0) {
		/* Takes the parked references from the work and releases both sets. */
		pd->async_put_wakeref = 0;
		all.bits[0] = pd->async_put_domains[0].bits[0] | pd->async_put_domains[1].bits[0];
		all.bits[1] = pd->async_put_domains[0].bits[1] | pd->async_put_domains[1].bits[1];
		i915_release_async_put_domains(pd, &all);
		i915_cancel_async_put_work(pd, 0);
	}

	/* Checks the invariants whatever happened. */
	i915_verify_async_put_domains_state(pd);

	mutex_unlock(&pd->lock);
}

/*
 * Releases every parked reference and waits out a running work
 * (intel_display_power_flush_work_sync()).
 */
void
drv_i915_display_power_flush_work_sync(
	struct i915_power_domains *pd)
{
	/* Releases what is parked. */
	drv_i915_display_power_flush_work(pd);

	/* Cancels the work synchronously, outside the lock its body takes. */
	if (pd->async_ops != NULL)
		(void)pd->async_ops->cancel(pd->async_ctx, 1);

	/* Checks that nothing is parked any more. */
	mutex_lock(&pd->lock);

	pd->async_put_next_delay = 0;
	i915_verify_async_put_domains_state(pd);
	if (pd->async_put_wakeref != 0)
		pd->async_state_errors++;

	mutex_unlock(&pd->lock);
}

/*
 * Prepares the PM demand lock and wait queue (intel_pmdemand_init_early()).
 */
void
drv_i915_pmdemand_init_early(
	struct i915_pmdemand *pm)
{
	/* Prepares the lock and the queue. */
	(void)mutex_init(&pm->lock, LOCK_RANK_DEVICE, "i915-pmdemand");
	waitq_init(&pm->waitqueue, "i915-pmdemand");

	/* Marks the early initialization as done. */
	pm->early_initialized = 1;
}

/*
 * Writes a DC state with the reference's checks (gen9_set_dc_state()).
 *
 * A state outside the allowed mask is reduced to it (the reference warns),
 * a DMC that ignored the last request is reported, and the software copy
 * follows the write.
 */
void
drv_i915_gen9_set_dc_state(
	struct i915_pw_ctx *pwc,
	uint32_t state)
{
	uint32_t value;
	uint32_t mask;

	/* drm_WARN_ON_ONCE in the reference: keeps only the allowed states. */
	if ((state & ~pwc->allowed_dc_mask) != 0u)
		state &= pwc->allowed_dc_mask;

	/* Reads the current state and the bits this display version has. */
	value = drv_i915_raw_read32(pwc->mmio, DC_STATE_EN);
	mask = i915_gen9_dc_mask(pwc->display_ver);

	/* Checks whether the DMC is ignoring the DC state requests. */
	if ((value & mask) != pwc->dc_state) {
		kern_logf("i915: DC state mismatch (0x%x -> 0x%x)\n",
			pwc->dc_state, value & mask);
	}

	/* Writes the new state and records it as display.dmc.dc_state. */
	value &= ~mask;
	value |= state;
	i915_gen9_write_dc_state(pwc, value);
	pwc->dc_state = value & mask;
}

/*
 * Reports the DBUF slices that are powered now.
 */
uint8_t
drv_i915_enabled_dbuf_slices_mask(
	struct i915_display_core *dc)
{
	uint8_t mask;
	unsigned slice;
	uint32_t value;

	/* Reads each slice's power state. */
	mask = 0u;
	for (slice = 0u; slice < 4u; slice++) {
		value = drv_i915_raw_read32(dc->m, i915_dbuf_ctl_s[slice]);
		if ((value & DBUF_POWER_STATE) != 0u)
			mask |= (uint8_t)(1u << slice);
	}

	/* Succeeded: reports the powered slices. */
	return mask;
}

/*
 * Reports the control register of a DBUF slice, or 0 past the fourth.
 *
 * The table itself, for an independent check against the reference's
 * macro.
 */
uint32_t
drv_i915_dbuf_ctl_reg(
	unsigned slice)
{
	/* A slice past the table has no register. */
	if (slice >= 4u)
		return 0u;

	/* Succeeded: reports the slice's register. */
	return i915_dbuf_ctl_s[slice];
}

/*
 * Powers the requested DBUF slices on and the others off, within the
 * slices the display has (the body of intel_dbuf_pre/post_plane_update()).
 */
void
drv_i915_gen9_dbuf_slices_update(
	struct i915_display_core *dc,
	uint8_t req_slices)
{
	uint8_t slices;

	/* Keeps the request within the display's slices and applies it. */
	slices = (uint8_t)(req_slices & i915_dbuf_slice_mask(dc));
	i915_gen9_dbuf_slices_update(dc, slices);
}

/*
 * Leaves the DC states when DC_off is enabled (gen9_dc_off_power_well_enable()
 * -> gen9_disable_dc_states()).
 *
 * The current CDCLK and DBUF are read and compared with the saved state --
 * never re-initialized -- and the combo PHYs are restored.  The CDCLK and
 * DBUF objects are the device's own, shared through the well context.
 */
void
drv_i915_dc_off_enable(
	struct i915_pw_ctx *pwc)
{
	struct i915_cdclk_config readout;
	struct i915_trace *trace;
	unsigned initialised;
	unsigned slice;
	uint8_t current_slices;
	uint32_t value;

	/* Counts the enable for the diagnostics. */
	pwc->dc_off_enable_calls++;

	/* A DC3CO target would take tgl_disable_dc3co(); the targets here are the DC5 / DC6 ones. */
	if (pwc->target_dc_state == DC_STATE_EN_DC3CO)
		return;

	/*
	 * gen9_set_dc_state(DC_STATE_DISABLE): the reference's own function, so
	 * the write is verified and display.dmc.dc_state follows it.  A direct
	 * register write once left the software copy behind, and the next DC
	 * enable reported a false mismatch.
	 */
	drv_i915_gen9_set_dc_state(pwc, 0u);

	/* Reads the CDCLK into a temporary and compares it with the saved state. */
	if (pwc->cd != NULL) {
		kern_memset(&readout, 0, sizeof(readout));
		drv_i915_bxt_get_cdclk(pwc->cd, &readout);
		pwc->dc_off_cdclk_readouts++;
		if (readout.cdclk != pwc->cd->hw.cdclk || readout.vco != pwc->cd->hw.vco) {
			kern_logf("i915: DC_off: cdclk needs modeset (hw %u/%u vs saved %u/%u)\n",
				readout.cdclk, readout.vco, pwc->cd->hw.cdclk, pwc->cd->hw.vco);
		}
	}

	/* gen9_assert_dbuf_enabled(): compares the powered slices with the saved mask. */
	if (pwc->dbuf_slices != NULL) {
		current_slices = 0u;
		for (slice = 0u; slice < 4u; slice++) {
			value = drv_i915_raw_read32(pwc->mmio, i915_dbuf_ctl_s[slice]);
			if ((value & DBUF_POWER_STATE) != 0u)
				current_slices |= (uint8_t)(1u << slice);
		}

		/* Counts the check and reports a mismatch. */
		pwc->dc_off_dbuf_asserts++;
		if (current_slices != *pwc->dbuf_slices) {
			kern_logf("i915: DC_off: DBUF mismatch (hw 0x%x vs saved 0x%x)\n",
				current_slices, *pwc->dbuf_slices);
		}
	}

	/* The DMC loses combo PHY B's context across DC on display version 11+: restores it. */
	trace = i915_dc_off_trace(pwc);
	drv_i915_trace_init(trace);
	initialised = 0u;
	(void)drv_i915_combo_phy_init(pwc->mmio, trace, &initialised);
	pwc->dc_off_combo_inits++;
}

/*
 * Brings the display core up (intel_power_domains_init_hw()).
 *
 * Runs icl_display_core_init(), then takes the POWER_DOMAIN_INIT reference
 * that keeps every well on until intel_power_domains_enable(), and syncs
 * every well with the hardware.  An adaptation-layer fault stops at the
 * child where it happened, with no reference taken and no sync run.
 */
void
drv_i915_power_domains_init_hw(
	struct i915_display_core *dc,
	int resume)
{
	struct i915_power_domains *pd;
	unsigned index;
	int core_result;

	/* Marks the bring-up as running. */
	pd = dc->pd;
	dc->initializing = 1;

	/* Shares the DC_off enable's dependencies through the well context (the same objects). */
	dc->pwc->cd = dc->cd;
	dc->pwc->dbuf_slices = &dc->dbuf_enabled_slices;
	dc->pwc->target_dc_state = pd->target_dc_state;
	dc->pwc->allowed_dc_mask = pd->allowed_dc_mask;

	/* Runs icl_display_core_init() (display version 11+). */
	core_result = i915_icl_display_core_init(dc, resume);
	if (core_result != 0) {
		/* A fault mid-child: no INIT reference and no sync; the fault position is kept. */
		dc->initializing = 0;
		return;
	}

	/*
	 * Keeps every well on for the dependent hardware access of the init and
	 * to keep what the firmware enabled powered until the readout.  The
	 * reference is dropped by intel_power_domains_enable().
	 */
	(void)drv_i915_display_power_get(pd, I915_PW_DOMAIN_INIT, dc->pwc);
	dc->init_wakeref_held = 1;
	dc->pm_wakeref = 1;
	dc->reached_init_ref = 1;

	/*
	 * disable_power_well is set on these platforms, so the reference takes
	 * no extra disable_wakeref.
	 */

	/* intel_power_domains_sync_hw(): syncs every well under the domains lock. */
	mutex_lock(&pd->lock);

	for (index = 0u; index < pd->num_power_wells; index++)
		drv_i915_power_well_sync_hw(&pd->power_wells[index], dc->pwc);

	mutex_unlock(&pd->lock);

	/* Records the end of the bring-up. */
	dc->reached_sync_hw = 1;
	dc->initializing = 0;
}

/*
 * Gives the runtime-PM side of the init reference back
 * (intel_power_domains_driver_remove()).
 *
 * The reference takes the init wakeref, puts no domain on these platforms
 * (disable_power_well is set), flushes the asynchronous put, verifies and
 * puts only the runtime-PM wakeref: the wells keep their count and stay
 * on for a reload.
 */
void
drv_i915_power_domains_driver_remove(
	struct i915_display_core *dc)
{
	int had_wakeref;

	/* wakeref = fetch_and_zero(&init_wakeref). */
	had_wakeref = dc->init_wakeref_held;
	dc->init_wakeref_held = 0;

	/*
	 * No disable_wakeref domain put, and the flush and verify have nothing
	 * to do here.  intel_runtime_pm_put(wakeref) gives the runtime-PM side
	 * back; there is intentionally no POWER_DOMAIN_INIT put.
	 */
	if (had_wakeref)
		dc->pm_wakeref = 0;
}

/*
 * Compares each well's software count with its hardware state
 * (intel_power_domains_verify_state()) and reports the mismatches.
 *
 * The reference compiles it under CONFIG_DRM_I915_DEBUG_RUNTIME_PM only;
 * here the per-well half is kept as a diagnostic.  The per-domain half
 * needs counts this port does not keep.
 */
unsigned
drv_i915_power_domains_verify_state(
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc)
{
	unsigned index;
	unsigned mismatches;
	struct i915_power_well *well;
	int enabled;
	int expect;

	/* Compares every well. */
	mismatches = 0u;
	for (index = 0u; index < pd->num_power_wells; index++) {
		well = &pd->power_wells[index];
		enabled = drv_i915_power_well_is_enabled(well, pwc);

		/* A held or always-on well is expected on, any other off. */
		expect = 0;
		if (well->refcount != 0u || well->always_on)
			expect = 1;

		/* Reports a well whose state does not follow its count. */
		if (expect != enabled) {
			kern_logf("i915: power well %s state mismatch (refcount %u/enabled %d)\n",
				well->name, well->refcount, enabled);
			mismatches++;
		}
	}

	/* Succeeded: reports how many wells disagreed. */
	return mismatches;
}

/*
 * Gives the POWER_DOMAIN_INIT reference back (intel_power_domains_enable()).
 *
 * Every well no other reference holds turns off, DC_off last, so DC6 is
 * allowed from here on.  The wells on before and after, the DC state and
 * the verification are recorded in the probe state.
 */
void
drv_i915_power_domains_enable(
	struct i915_driver_probe *probe,
	struct i915_display_core *dc)
{
	/* Counts the wells on before the put. */
	probe->wells_on_before = i915_wells_on(dc->pd, dc->pwc);

	/* wakeref = fetch_and_zero(&init_wakeref); intel_display_power_put(INIT, wakeref). */
	if (dc->init_wakeref_held) {
		dc->init_wakeref_held = 0;
		dc->pm_wakeref = 0;
		drv_i915_display_power_put(dc->pd, I915_PW_DOMAIN_INIT, dc->pwc);
	}

	/* Records what the put left. */
	probe->wells_on_after = i915_wells_on(dc->pd, dc->pwc);
	probe->dc_state_after = (int)dc->pwc->dc_state;
	probe->verify_mismatches = drv_i915_power_domains_verify_state(dc->pd, dc->pwc);
}

/*
 * Takes the POWER_DOMAIN_INIT reference again (intel_power_domains_disable()).
 */
void
drv_i915_power_domains_disable(
	struct i915_display_core *dc)
{
	/* init_wakeref = intel_display_power_get(INIT). */
	if (!dc->init_wakeref_held) {
		(void)drv_i915_display_power_get(dc->pd, I915_PW_DOMAIN_INIT, dc->pwc);
		dc->init_wakeref_held = 1;
		dc->pm_wakeref = 1;
	}

	/* Verifies the wells against their counts. */
	(void)drv_i915_power_domains_verify_state(dc->pd, dc->pwc);
}

/*
 * Takes a domain into a crtc's power-domain set (intel_display_power_get_in_set()).
 */
void
drv_i915_display_power_get_in_set(
	struct drm_i915_private *i915,
	struct intel_display_power_domain_set *power_domain_set,
	enum intel_display_power_domain domain)
{
	intel_wakeref_t wf;
	bool already_in_set;

	/* A domain already in the set is a warning of the Linux text. */
	already_in_set = test_bit(domain, power_domain_set->mask.bits);
	if (already_in_set)
		drv_i915_lcd_error("WARN_ON(test_bit(domain, power_domain_set->mask.bits))\n");

	/*
	 * Takes the reference.  The cookie is kept only under
	 * CONFIG_DRM_I915_DEBUG_RUNTIME_PM, which is not enabled.
	 */
	wf = i915_lcd_intel_display_power_get(i915, domain);
	UNUSED_PARAMETER(wf);

	/* Records the domain as held by the set. */
	i915_set_bit(domain, power_domain_set->mask.bits);
}

/*
 * Gives the domains of a mask back from a crtc's power-domain set
 * (intel_display_power_put_mask_in_set()).
 */
void
drv_i915_display_power_put_mask_in_set(
	struct drm_i915_private *i915,
	struct intel_display_power_domain_set *power_domain_set,
	struct intel_power_domain_mask *mask)
{
	enum intel_display_power_domain domain;
	bool subset;
	bool in_mask;

	/* A domain the set does not hold is a warning of the Linux text. */
	subset = i915_bitmap_subset(mask->bits, power_domain_set->mask.bits, POWER_DOMAIN_NUM);
	if (!subset)
		drv_i915_lcd_error("WARN_ON(!bitmap_subset(mask->bits, power_domain_set->mask.bits, POWER_DOMAIN_NUM))\n");

	/*
	 * Puts every domain of the mask (for_each_power_domain) with the cookie
	 * -1: per-domain cookies are kept only under
	 * CONFIG_DRM_I915_DEBUG_RUNTIME_PM.
	 */
	for (domain = 0; domain < POWER_DOMAIN_NUM; domain++) {
		in_mask = test_bit(domain, mask->bits);
		if (!in_mask)
			continue;

		/* Gives the reference back and forgets it. */
		i915_lcd_intel_display_power_put(i915, domain, -1);
		i915_clear_bit(domain, power_domain_set->mask.bits);
	}
}

/*
 * Takes a power reference for a panel run (the run's power_get hook).
 *
 * Returns the cookie domain + 1, or 0 when the domain is unknown or could
 * not be taken.  The run counts its references per domain.
 */
int
drv_i915_lcd_power_get(
	void *ctx,
	int domain)
{
	struct i915_lcd_kernel *kernel;
	int get_result;

	/* Resolves the run the hook belongs to. */
	kernel = ctx;

	/* Refuses an unknown domain. */
	if (domain < 0 || domain >= (int)POWER_DOMAIN_NUM) {
		kernel->power_get_failures++;
		return 0;
	}

	/* Takes the domain. */
	get_result = drv_i915_display_power_get(kernel->d->pd, (enum i915_power_domain)domain, kernel->d->pwc);
	if (get_result != 0) {
		kernel->power_get_failures++;
		kern_logf("i915: LCD-B power get domain=%d FAILED rc=%d\n", domain, get_result);
		return 0;
	}

	/* The run's count of the references it holds on the domain. */
	kernel->power_refs[domain]++;

	/* Succeeded: the cookie names the domain. */
	return domain + 1;
}

/*
 * Takes a power reference for a panel run only when the domain is already
 * enabled (intel_display_power_get_if_enabled()).
 *
 * A readout never turns a well on.  Returns the cookie, or 0 when the
 * domain is off or unknown.
 */
int
drv_i915_lcd_power_get_if_enabled(
	void *ctx,
	int domain)
{
	struct i915_lcd_kernel *kernel;
	int enabled;
	int cookie;

	/* Resolves the run the hook belongs to. */
	kernel = ctx;

	/* An unknown domain is not enabled. */
	if (domain < 0 || domain >= (int)POWER_DOMAIN_NUM)
		return 0;

	/* The domain counts as enabled when every one of its wells is. */
	enabled = drv_i915_display_power_is_enabled(kernel->d->pd, (enum i915_power_domain)domain, kernel->d->pwc);
	if (!enabled)
		return 0;

	/* Takes the reference. */
	cookie = drv_i915_lcd_power_get(ctx, domain);

	/* Succeeded: reports the cookie (0 when the get failed). */
	return cookie;
}

/*
 * Gives a power reference of a panel run back (the run's power_put hook).
 *
 * A put the run holds no reference for is counted and not forwarded.  A
 * well that could not be turned off (a failed pipe interrupt drain) is a
 * backend fault: the stop is not confirmed.
 */
void
drv_i915_lcd_power_put(
	void *ctx,
	int domain,
	int wakeref)
{
	struct i915_lcd_kernel *kernel;
	unsigned refusals_before;

	UNUSED_PARAMETER(wakeref);

	/* Resolves the run the hook belongs to. */
	kernel = ctx;

	/* Refuses a put without a reference of this run. */
	if (domain < 0 || domain >= (int)POWER_DOMAIN_NUM || kernel->power_refs[domain] <= 0) {
		kernel->errors++;
		kern_logf("i915: LCD-B power put domain=%d without a reference of this run: NOT forwarded\n", domain);
		return;
	}

	/* The run's count of the references it holds on the domain. */
	kernel->power_refs[domain]--;

	/* Puts the domain, watching for a refused well disable. */
	refusals_before = kernel->d->pwc->disable_refusals;
	drv_i915_display_power_put(kernel->d->pd, (enum i915_power_domain)domain, kernel->d->pwc);
	if (kernel->d->pwc->disable_refusals != refusals_before) {
		drv_i915_lcd_backend_fault("a power well was NOT turned off: the pipe interrupt drain failed (the stop is not "
			"confirmed; display power is kept)\n");
	}
}

/*
 * Gives a power reference of a panel run back after a delay (the run's
 * power_put_async hook).
 *
 * A put the run holds no reference for is counted and not forwarded.
 */
void
drv_i915_lcd_power_put_async(
	void *ctx,
	int domain,
	int wakeref,
	int delay_ms)
{
	struct i915_lcd_kernel *kernel;

	UNUSED_PARAMETER(wakeref);

	/* Resolves the run the hook belongs to. */
	kernel = ctx;

	/* Refuses a put without a reference of this run. */
	if (domain < 0 || domain >= (int)POWER_DOMAIN_NUM || kernel->power_refs[domain] <= 0) {
		kernel->errors++;
		kern_logf("i915: LCD-B async power put domain=%d without a reference of this run: NOT forwarded\n", domain);
		return;
	}

	/* The run's count of the references it holds on the domain. */
	kernel->power_refs[domain]--;

	/* Hands the reference to the asynchronous put. */
	drv_i915_display_power_put_async(kernel->d->pd, (enum i915_power_domain)domain, kernel->d->pwc, delay_ms);
}

/* Adds a domain to a well's domain mask. */
static void
i915_pw_dom(
	struct i915_pw_domain_mask *mask,
	enum i915_power_domain domain)
{
	/* Sets the domain's bit in the word that holds it. */
	mask->bits[(unsigned)domain >> 6] |= (uint64_t)1u << ((unsigned)domain & 63u);
}

/* Computes the allowed DC states (get_allowed_dc_mask(): display version 12+ allows DC9, DC3CO and up to DC6). */
static uint32_t
i915_get_allowed_dc_mask(
	unsigned display_ver,
	int enable_dc)
{
	uint32_t mask;
	uint32_t max_states;

	/* DC9 is allowed from display version 11. */
	mask = 0u;
	if (display_ver >= 11u)
		mask = I915_DC_STATE_EN_DC9;

	/* enable_dc: -1 is auto (the platform maximum), 0 disabled, anything else the maximum. */
	max_states = I915_DC_STATE_EN_DC3CO | I915_DC_STATE_EN_UPTO_DC6;
	if (enable_dc != 0)
		mask |= max_states;

	/* Succeeded: reports the allowed states. */
	return mask;
}

/* Falls to the next DC state while the requested one is not allowed (sanitize_target_dc_state()). */
static uint32_t
i915_sanitize_target_dc_state(
	const struct i915_power_domains *pd,
	uint32_t target)
{
	static const uint32_t states[] = {
		I915_DC_STATE_EN_UPTO_DC6,
		I915_DC_STATE_EN_UPTO_DC5,
		0u	/* DC_STATE_DISABLE */
	};
	unsigned index;

	/* Walks the states from the deepest, stepping down past each one that is not allowed. */
	for (index = 0u; index + 1u < ARRAY_SIZE(states); index++) {
		if (target != states[index])
			continue;
		if ((pd->allowed_dc_mask & target) != 0u)
			break;
		target = states[index + 1u];
	}

	/* Succeeded: reports the deepest allowed state at or below the request. */
	return target;
}

/* Sanitizes the disable_power_well option (the platform default allows dynamic disabling). */
static int
i915_sanitize_disable_power_well(
	int param)
{
	/* A negative value takes the platform default. */
	if (param < 0)
		return 1;

	/* Any other value is a plain yes or no. */
	if (param != 0)
		return 1;

	/* Succeeded: dynamic disabling is off. */
	return 0;
}

/* Appends one well from its descriptor, with its live state unknown; NULL when the array is full. */
static struct i915_power_well *
i915_pw_add(
	struct i915_power_domains *pd,
	const struct i915_pw_desc *desc)
{
	struct i915_power_well *well;
	unsigned index;

	/* Refuses a well past the array. */
	if (pd->num_power_wells >= I915_PW_MAX)
		return NULL;

	/* Takes the next slot. */
	well = &pd->power_wells[pd->num_power_wells];
	pd->num_power_wells++;

	/* Copies the descriptor. */
	well->name = desc->name;
	well->ops = desc->ops;
	well->always_on = desc->always_on;
	well->has_vga = desc->has_vga;
	well->has_fuses = desc->has_fuses;
	well->is_tc_tbt = desc->is_tc_tbt;
	well->fixed_enable_delay = desc->fixed_enable_delay;
	well->enable_timeout = desc->enable_timeout;
	well->irq_pipe_mask = desc->irq_pipe_mask;
	well->hsw_idx = desc->hsw_idx;
	well->id = desc->id;
	well->domains_all = desc->domains_all;

	/* Builds the domain mask from the list. */
	well->domains.bits[0] = 0u;
	well->domains.bits[1] = 0u;
	for (index = 0u; index < desc->domain_count; index++)
		i915_pw_dom(&well->domains, desc->domains[index]);

	/* The software count is separate from the hardware state, which is unknown until the sync. */
	well->refcount = 0u;
	well->hw_enabled = -1;

	/* Succeeded: the well is in the array. */
	return well;
}

/* Adds every descriptor in order and builds the map from each domain to the wells that provide it. */
static void
i915_power_map_build(
	struct i915_power_domains *pd,
	const struct i915_pw_desc *descs,
	unsigned count)
{
	unsigned index;
	unsigned domain;
	const struct i915_power_well *well;

	/* Adds the wells in the reference order. */
	pd->num_power_wells = 0u;
	for (index = 0u; index < count; index++)
		(void)i915_pw_add(pd, &descs[index]);

	/* Starts every domain with no well. */
	for (index = 0u; index < I915_PW_DOMAIN_NUM; index++)
		pd->domain_wells[index] = 0u;

	/* Adds each well to the domains it provides (a zero-length list is every domain). */
	for (index = 0u; index < pd->num_power_wells; index++) {
		well = &pd->power_wells[index];
		if (well->domains_all) {
			for (domain = 0u; domain < I915_PW_DOMAIN_NUM; domain++)
				pd->domain_wells[domain] |= (uint64_t)1u << index;
			continue;
		}

		/* Adds the well to each domain of its list. */
		for (domain = 0u; domain < I915_PW_DOMAIN_NUM; domain++) {
			if ((well->domains.bits[domain >> 6] & ((uint64_t)1u << (domain & 63u))) != 0u)
				pd->domain_wells[domain] |= (uint64_t)1u << index;
		}
	}

	/* Publishes the map as built. */
	pd->map_initialized = 1;
}

/* Builds the XE_LPD wells and their map (intel_display_power_map_init() for display version 13). */
static int
i915_power_map_init(
	struct i915_power_domains *pd)
{
	static const enum i915_power_domain pw_a[] = {
		I915_PW_DOMAIN_PIPE_A, I915_PW_DOMAIN_PIPE_PANEL_FITTER_A,
		I915_PW_DOMAIN_INIT };
	static const enum i915_power_domain pw_b[] = {
		I915_PW_DOMAIN_PIPE_B, I915_PW_DOMAIN_PIPE_PANEL_FITTER_B,
		I915_PW_DOMAIN_TRANSCODER_B, I915_PW_DOMAIN_INIT };
	static const enum i915_power_domain pw_c[] = {
		I915_PW_DOMAIN_PIPE_C, I915_PW_DOMAIN_PIPE_PANEL_FITTER_C,
		I915_PW_DOMAIN_TRANSCODER_C, I915_PW_DOMAIN_INIT };
	static const enum i915_power_domain pw_d[] = {
		I915_PW_DOMAIN_PIPE_D, I915_PW_DOMAIN_PIPE_PANEL_FITTER_D,
		I915_PW_DOMAIN_TRANSCODER_D, I915_PW_DOMAIN_INIT };
	/* xelpd_pwdoms_pw_2 = PW_B + PW_C + PW_D + DC_OFF_PORT (the DDI lanes) + INIT. */
	static const enum i915_power_domain pw_2[] = {
		I915_PW_DOMAIN_PIPE_B, I915_PW_DOMAIN_PIPE_PANEL_FITTER_B,
		I915_PW_DOMAIN_TRANSCODER_B,
		I915_PW_DOMAIN_PIPE_C, I915_PW_DOMAIN_PIPE_PANEL_FITTER_C,
		I915_PW_DOMAIN_TRANSCODER_C,
		I915_PW_DOMAIN_PIPE_D, I915_PW_DOMAIN_PIPE_PANEL_FITTER_D,
		I915_PW_DOMAIN_TRANSCODER_D,
		I915_PW_DOMAIN_PORT_DDI_LANES_C, I915_PW_DOMAIN_PORT_DDI_LANES_D,
		I915_PW_DOMAIN_PORT_DDI_LANES_E, I915_PW_DOMAIN_PORT_DDI_LANES_TC1,
		I915_PW_DOMAIN_PORT_DDI_LANES_TC2, I915_PW_DOMAIN_PORT_DDI_LANES_TC3,
		I915_PW_DOMAIN_PORT_DDI_LANES_TC4, I915_PW_DOMAIN_INIT };
	/* xelpd_pwdoms_dc_off = DC_OFF_PORT + PW_C + PW_D + DSI + AUDIO + AUX_A/B + DC_OFF + INIT. */
	static const enum i915_power_domain dc_off[] = {
		I915_PW_DOMAIN_PORT_DDI_LANES_C, I915_PW_DOMAIN_PORT_DDI_LANES_D,
		I915_PW_DOMAIN_PORT_DDI_LANES_E, I915_PW_DOMAIN_PORT_DDI_LANES_TC1,
		I915_PW_DOMAIN_PORT_DDI_LANES_TC2, I915_PW_DOMAIN_PORT_DDI_LANES_TC3,
		I915_PW_DOMAIN_PORT_DDI_LANES_TC4,
		I915_PW_DOMAIN_PIPE_C, I915_PW_DOMAIN_PIPE_PANEL_FITTER_C,
		I915_PW_DOMAIN_TRANSCODER_C,
		I915_PW_DOMAIN_PIPE_D, I915_PW_DOMAIN_PIPE_PANEL_FITTER_D,
		I915_PW_DOMAIN_TRANSCODER_D,
		I915_PW_DOMAIN_PORT_DSI, I915_PW_DOMAIN_AUDIO_MMIO,
		I915_PW_DOMAIN_AUX_A, I915_PW_DOMAIN_AUX_B,
		I915_PW_DOMAIN_DC_OFF, I915_PW_DOMAIN_INIT };
	static const enum i915_power_domain ddi_io_a[] = { I915_PW_DOMAIN_PORT_DDI_IO_A };
	static const enum i915_power_domain ddi_io_b[] = { I915_PW_DOMAIN_PORT_DDI_IO_B };
	static const enum i915_power_domain ddi_io_c[] = { I915_PW_DOMAIN_PORT_DDI_IO_C };
	static const enum i915_power_domain ddi_io_d[] = { I915_PW_DOMAIN_PORT_DDI_IO_D };
	static const enum i915_power_domain ddi_io_e[] = { I915_PW_DOMAIN_PORT_DDI_IO_E };
	static const enum i915_power_domain ddi_io_tc1[] = { I915_PW_DOMAIN_PORT_DDI_IO_TC1 };
	static const enum i915_power_domain ddi_io_tc2[] = { I915_PW_DOMAIN_PORT_DDI_IO_TC2 };
	static const enum i915_power_domain ddi_io_tc3[] = { I915_PW_DOMAIN_PORT_DDI_IO_TC3 };
	static const enum i915_power_domain ddi_io_tc4[] = { I915_PW_DOMAIN_PORT_DDI_IO_TC4 };
	static const enum i915_power_domain aux_a[] = { I915_PW_DOMAIN_AUX_IO_A, I915_PW_DOMAIN_AUX_A };
	static const enum i915_power_domain aux_b[] = { I915_PW_DOMAIN_AUX_IO_B, I915_PW_DOMAIN_AUX_B };
	static const enum i915_power_domain aux_c[] = { I915_PW_DOMAIN_AUX_IO_C, I915_PW_DOMAIN_AUX_C };
	static const enum i915_power_domain aux_d[] = { I915_PW_DOMAIN_AUX_IO_D, I915_PW_DOMAIN_AUX_D };
	static const enum i915_power_domain aux_e[] = { I915_PW_DOMAIN_AUX_IO_E, I915_PW_DOMAIN_AUX_E };
	static const enum i915_power_domain aux_usbc1[] = { I915_PW_DOMAIN_AUX_USBC1 };
	static const enum i915_power_domain aux_usbc2[] = { I915_PW_DOMAIN_AUX_USBC2 };
	static const enum i915_power_domain aux_usbc3[] = { I915_PW_DOMAIN_AUX_USBC3 };
	static const enum i915_power_domain aux_usbc4[] = { I915_PW_DOMAIN_AUX_USBC4 };
	static const enum i915_power_domain aux_tbt1[] = { I915_PW_DOMAIN_AUX_TBT1 };
	static const enum i915_power_domain aux_tbt2[] = { I915_PW_DOMAIN_AUX_TBT2 };
	static const enum i915_power_domain aux_tbt3[] = { I915_PW_DOMAIN_AUX_TBT3 };
	static const enum i915_power_domain aux_tbt4[] = { I915_PW_DOMAIN_AUX_TBT4 };

	/*
	 * The XE_LPD wells in the reference order.  Columns: name, operations,
	 * always on, VGA, fuses, Thunderbolt AUX, fixed enable delay, enable
	 * timeout (ms), interrupt pipes, control index, id, every domain,
	 * domains.
	 */
	static const struct i915_pw_desc wells[] = {
		/* i9xx_power_wells_always_on: a zero-length domain list is every domain. */
		{ "always_on", I915_PW_OPS_ALWAYS_ON, 1, 0, 0, 0, 0, 0u, 0u, 0u, I915_DISP_PW_ID_NONE, 1, NULL, 0u },
		/* icl_power_wells_pw_1: always on, fused, a NULL domain list is no domain. */
		{ "PW_1", I915_PW_OPS_HSW, 1, 0, 1, 0, 0, 0u, 0u, IDX_PW_1, I915_SKL_DISP_PW_1, 0, NULL, 0u },
		/* xelpd_power_wells_dc_off */
		{ "DC_off", I915_PW_OPS_DC_OFF, 0, 0, 0, 0, 0, 0u, 0u, 0u, I915_SKL_DISP_DC_OFF, 0, dc_off, ARRAY_SIZE(dc_off) },
		/* xelpd_power_wells_main */
		{ "PW_2", I915_PW_OPS_HSW, 0, 1, 1, 0, 0, 0u, 0u, IDX_PW_2, I915_SKL_DISP_PW_2, 0, pw_2, ARRAY_SIZE(pw_2) },
		{ "PW_A", I915_PW_OPS_HSW, 0, 0, 1, 0, 0, 0u, BIT_PIPE_A, IDX_PW_A, I915_DISP_PW_ID_NONE, 0, pw_a, ARRAY_SIZE(pw_a) },
		{ "PW_B", I915_PW_OPS_HSW, 0, 0, 1, 0, 0, 0u, BIT_PIPE_B, IDX_PW_B, I915_DISP_PW_ID_NONE, 0, pw_b, ARRAY_SIZE(pw_b) },
		{ "PW_C", I915_PW_OPS_HSW, 0, 0, 1, 0, 0, 0u, BIT_PIPE_C, IDX_PW_C, I915_DISP_PW_ID_NONE, 0, pw_c, ARRAY_SIZE(pw_c) },
		{ "PW_D", I915_PW_OPS_HSW, 0, 0, 1, 0, 0, 0u, BIT_PIPE_D, IDX_PW_D, I915_DISP_PW_ID_NONE, 0, pw_d, ARRAY_SIZE(pw_d) },
		{ "DDI_IO_A", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_A, I915_DISP_PW_ID_NONE, 0, ddi_io_a, ARRAY_SIZE(ddi_io_a) },
		{ "DDI_IO_B", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_B, I915_DISP_PW_ID_NONE, 0, ddi_io_b, ARRAY_SIZE(ddi_io_b) },
		{ "DDI_IO_C", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_C, I915_DISP_PW_ID_NONE, 0, ddi_io_c, ARRAY_SIZE(ddi_io_c) },
		{ "DDI_IO_D", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_D, I915_DISP_PW_ID_NONE, 0, ddi_io_d, ARRAY_SIZE(ddi_io_d) },
		{ "DDI_IO_E", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_E, I915_DISP_PW_ID_NONE, 0, ddi_io_e, ARRAY_SIZE(ddi_io_e) },
		{ "DDI_IO_TC1", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_TC1, I915_DISP_PW_ID_NONE, 0, ddi_io_tc1, ARRAY_SIZE(ddi_io_tc1) },
		{ "DDI_IO_TC2", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_TC2, I915_DISP_PW_ID_NONE, 0, ddi_io_tc2, ARRAY_SIZE(ddi_io_tc2) },
		{ "DDI_IO_TC3", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_TC3, I915_DISP_PW_ID_NONE, 0, ddi_io_tc3, ARRAY_SIZE(ddi_io_tc3) },
		{ "DDI_IO_TC4", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_TC4, I915_DISP_PW_ID_NONE, 0, ddi_io_tc4, ARRAY_SIZE(ddi_io_tc4) },
		/* AUX_A..E: fixed enable delay. */
		{ "AUX_A", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 0u, 0u, IDX_AUX_A, I915_DISP_PW_ID_NONE, 0, aux_a, ARRAY_SIZE(aux_a) },
		{ "AUX_B", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 0u, 0u, IDX_AUX_B, I915_DISP_PW_ID_NONE, 0, aux_b, ARRAY_SIZE(aux_b) },
		{ "AUX_C", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 0u, 0u, IDX_AUX_C, I915_DISP_PW_ID_NONE, 0, aux_c, ARRAY_SIZE(aux_c) },
		{ "AUX_D", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 0u, 0u, IDX_AUX_D, I915_DISP_PW_ID_NONE, 0, aux_d, ARRAY_SIZE(aux_d) },
		{ "AUX_E", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 0u, 0u, IDX_AUX_E, I915_DISP_PW_ID_NONE, 0, aux_e, ARRAY_SIZE(aux_e) },
		/* AUX_USBC1..4: fixed enable delay and the 500 ms enable timeout of WA_14017248603. */
		{ "AUX_USBC1", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 500u, 0u, IDX_AUX_TC1, I915_DISP_PW_ID_NONE, 0, aux_usbc1, ARRAY_SIZE(aux_usbc1) },
		{ "AUX_USBC2", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 500u, 0u, IDX_AUX_TC2, I915_DISP_PW_ID_NONE, 0, aux_usbc2, ARRAY_SIZE(aux_usbc2) },
		{ "AUX_USBC3", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 500u, 0u, IDX_AUX_TC3, I915_DISP_PW_ID_NONE, 0, aux_usbc3, ARRAY_SIZE(aux_usbc3) },
		{ "AUX_USBC4", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 500u, 0u, IDX_AUX_TC4, I915_DISP_PW_ID_NONE, 0, aux_usbc4, ARRAY_SIZE(aux_usbc4) },
		/* AUX_TBT1..4: Thunderbolt AUX. */
		{ "AUX_TBT1", I915_PW_OPS_ICL_AUX, 0, 0, 0, 1, 0, 0u, 0u, IDX_AUX_TBT1, I915_DISP_PW_ID_NONE, 0, aux_tbt1, ARRAY_SIZE(aux_tbt1) },
		{ "AUX_TBT2", I915_PW_OPS_ICL_AUX, 0, 0, 0, 1, 0, 0u, 0u, IDX_AUX_TBT2, I915_DISP_PW_ID_NONE, 0, aux_tbt2, ARRAY_SIZE(aux_tbt2) },
		{ "AUX_TBT3", I915_PW_OPS_ICL_AUX, 0, 0, 0, 1, 0, 0u, 0u, IDX_AUX_TBT3, I915_DISP_PW_ID_NONE, 0, aux_tbt3, ARRAY_SIZE(aux_tbt3) },
		{ "AUX_TBT4", I915_PW_OPS_ICL_AUX, 0, 0, 0, 1, 0, 0u, 0u, IDX_AUX_TBT4, I915_DISP_PW_ID_NONE, 0, aux_tbt4, ARRAY_SIZE(aux_tbt4) }
	};

	/* Builds the wells and the map. */
	i915_power_map_build(pd, wells, ARRAY_SIZE(wells));

	/* Succeeded: the XE_LPD map is built. */
	return 0;
}

/*
 * Builds the Tiger Lake wells and their map (intel_display_power_map_init()
 * for display version 12: always_on, PW_1, tgl_power_wells_main and
 * tgl_power_wells_aux).
 *
 * XXX: the reference's TC cold-off well is not built.  This driver never
 * takes a Type-C port, and its operations (tgl_tc_cold_off_ops) are a
 * PCODE handshake that is not ported; a Type-C display on this platform
 * needs both before it can work.
 */
static int
i915_power_map_init_tgl(
	struct i915_power_domains *pd)
{
	/* TGL_PW_5_POWER_DOMAINS */
	static const enum i915_power_domain pw_5[] = {
		I915_PW_DOMAIN_PIPE_D, I915_PW_DOMAIN_PIPE_PANEL_FITTER_D,
		I915_PW_DOMAIN_TRANSCODER_D, I915_PW_DOMAIN_INIT };
	/* TGL_PW_4 = PW_5 + pipe C */
	static const enum i915_power_domain pw_4[] = {
		I915_PW_DOMAIN_PIPE_D, I915_PW_DOMAIN_PIPE_PANEL_FITTER_D,
		I915_PW_DOMAIN_TRANSCODER_D,
		I915_PW_DOMAIN_PIPE_C, I915_PW_DOMAIN_PIPE_PANEL_FITTER_C,
		I915_PW_DOMAIN_TRANSCODER_C, I915_PW_DOMAIN_INIT };
	/* TGL_PW_3 = PW_4 + pipe B + the four Type-C lane / AUX domains + VGA + audio */
	static const enum i915_power_domain pw_3[] = {
		I915_PW_DOMAIN_PIPE_D, I915_PW_DOMAIN_PIPE_PANEL_FITTER_D,
		I915_PW_DOMAIN_TRANSCODER_D,
		I915_PW_DOMAIN_PIPE_C, I915_PW_DOMAIN_PIPE_PANEL_FITTER_C,
		I915_PW_DOMAIN_TRANSCODER_C,
		I915_PW_DOMAIN_PIPE_B, I915_PW_DOMAIN_PIPE_PANEL_FITTER_B,
		I915_PW_DOMAIN_TRANSCODER_B,
		I915_PW_DOMAIN_PORT_DDI_LANES_TC1, I915_PW_DOMAIN_PORT_DDI_LANES_TC2,
		I915_PW_DOMAIN_PORT_DDI_LANES_TC3, I915_PW_DOMAIN_PORT_DDI_LANES_TC4,
		I915_PW_DOMAIN_VGA, I915_PW_DOMAIN_AUDIO_MMIO, I915_PW_DOMAIN_AUDIO_PLAYBACK,
		I915_PW_DOMAIN_AUX_USBC1, I915_PW_DOMAIN_AUX_USBC2,
		I915_PW_DOMAIN_AUX_USBC3, I915_PW_DOMAIN_AUX_USBC4,
		I915_PW_DOMAIN_AUX_TBT1, I915_PW_DOMAIN_AUX_TBT2,
		I915_PW_DOMAIN_AUX_TBT3, I915_PW_DOMAIN_AUX_TBT4,
		I915_PW_DOMAIN_INIT };
	/*
	 * tgl_pwdoms_pw_2 is PW_3 plus the VDSC of PW_2, not a domain this
	 * driver takes, so PW_2 carries pw_3.  tgl_pwdoms_dc_off = PW_3 +
	 * AUX_A/B/C + DC_OFF.
	 */
	static const enum i915_power_domain dc_off[] = {
		I915_PW_DOMAIN_PIPE_D, I915_PW_DOMAIN_PIPE_PANEL_FITTER_D,
		I915_PW_DOMAIN_TRANSCODER_D,
		I915_PW_DOMAIN_PIPE_C, I915_PW_DOMAIN_PIPE_PANEL_FITTER_C,
		I915_PW_DOMAIN_TRANSCODER_C,
		I915_PW_DOMAIN_PIPE_B, I915_PW_DOMAIN_PIPE_PANEL_FITTER_B,
		I915_PW_DOMAIN_TRANSCODER_B,
		I915_PW_DOMAIN_PORT_DDI_LANES_TC1, I915_PW_DOMAIN_PORT_DDI_LANES_TC2,
		I915_PW_DOMAIN_PORT_DDI_LANES_TC3, I915_PW_DOMAIN_PORT_DDI_LANES_TC4,
		I915_PW_DOMAIN_VGA, I915_PW_DOMAIN_AUDIO_MMIO, I915_PW_DOMAIN_AUDIO_PLAYBACK,
		I915_PW_DOMAIN_AUX_USBC1, I915_PW_DOMAIN_AUX_USBC2,
		I915_PW_DOMAIN_AUX_USBC3, I915_PW_DOMAIN_AUX_USBC4,
		I915_PW_DOMAIN_AUX_TBT1, I915_PW_DOMAIN_AUX_TBT2,
		I915_PW_DOMAIN_AUX_TBT3, I915_PW_DOMAIN_AUX_TBT4,
		I915_PW_DOMAIN_AUX_A, I915_PW_DOMAIN_AUX_B, I915_PW_DOMAIN_AUX_C,
		I915_PW_DOMAIN_DC_OFF, I915_PW_DOMAIN_INIT };
	static const enum i915_power_domain ddi_io_a[] = { I915_PW_DOMAIN_PORT_DDI_IO_A };
	static const enum i915_power_domain ddi_io_b[] = { I915_PW_DOMAIN_PORT_DDI_IO_B };
	static const enum i915_power_domain ddi_io_c[] = { I915_PW_DOMAIN_PORT_DDI_IO_C };
	static const enum i915_power_domain ddi_io_tc1[] = { I915_PW_DOMAIN_PORT_DDI_IO_TC1 };
	static const enum i915_power_domain ddi_io_tc2[] = { I915_PW_DOMAIN_PORT_DDI_IO_TC2 };
	static const enum i915_power_domain ddi_io_tc3[] = { I915_PW_DOMAIN_PORT_DDI_IO_TC3 };
	static const enum i915_power_domain ddi_io_tc4[] = { I915_PW_DOMAIN_PORT_DDI_IO_TC4 };
	static const enum i915_power_domain aux_a[] = { I915_PW_DOMAIN_AUX_IO_A, I915_PW_DOMAIN_AUX_A };
	static const enum i915_power_domain aux_b[] = { I915_PW_DOMAIN_AUX_IO_B, I915_PW_DOMAIN_AUX_B };
	static const enum i915_power_domain aux_c[] = { I915_PW_DOMAIN_AUX_IO_C, I915_PW_DOMAIN_AUX_C };
	static const enum i915_power_domain aux_usbc1[] = { I915_PW_DOMAIN_AUX_USBC1 };
	static const enum i915_power_domain aux_usbc2[] = { I915_PW_DOMAIN_AUX_USBC2 };
	static const enum i915_power_domain aux_usbc3[] = { I915_PW_DOMAIN_AUX_USBC3 };
	static const enum i915_power_domain aux_usbc4[] = { I915_PW_DOMAIN_AUX_USBC4 };
	static const enum i915_power_domain aux_tbt1[] = { I915_PW_DOMAIN_AUX_TBT1 };
	static const enum i915_power_domain aux_tbt2[] = { I915_PW_DOMAIN_AUX_TBT2 };
	static const enum i915_power_domain aux_tbt3[] = { I915_PW_DOMAIN_AUX_TBT3 };
	static const enum i915_power_domain aux_tbt4[] = { I915_PW_DOMAIN_AUX_TBT4 };

	/* The Tiger Lake wells in the reference order (columns as in i915_power_map_init()). */
	static const struct i915_pw_desc wells[] = {
		/* i9xx_power_wells_always_on: a zero-length domain list is every domain. */
		{ "always_on", I915_PW_OPS_ALWAYS_ON, 1, 0, 0, 0, 0, 0u, 0u, 0u, I915_DISP_PW_ID_NONE, 1, NULL, 0u },
		/* icl_power_wells_pw_1 */
		{ "PW_1", I915_PW_OPS_HSW, 1, 0, 1, 0, 0, 0u, 0u, IDX_PW_1, I915_SKL_DISP_PW_1, 0, NULL, 0u },
		/* tgl_power_wells_main */
		{ "DC_off", I915_PW_OPS_DC_OFF, 0, 0, 0, 0, 0, 0u, 0u, 0u, I915_SKL_DISP_DC_OFF, 0, dc_off, ARRAY_SIZE(dc_off) },
		{ "PW_2", I915_PW_OPS_HSW, 0, 0, 1, 0, 0, 0u, 0u, IDX_PW_2, I915_SKL_DISP_PW_2, 0, pw_3, ARRAY_SIZE(pw_3) },
		{ "PW_3", I915_PW_OPS_HSW, 0, 1, 1, 0, 0, 0u, BIT_PIPE_B, IDX_PW_3, I915_ICL_DISP_PW_3, 0, pw_3, ARRAY_SIZE(pw_3) },
		{ "DDI_IO_A", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_A, I915_DISP_PW_ID_NONE, 0, ddi_io_a, ARRAY_SIZE(ddi_io_a) },
		{ "DDI_IO_B", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_B, I915_DISP_PW_ID_NONE, 0, ddi_io_b, ARRAY_SIZE(ddi_io_b) },
		{ "DDI_IO_C", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_C, I915_DISP_PW_ID_NONE, 0, ddi_io_c, ARRAY_SIZE(ddi_io_c) },
		{ "DDI_IO_TC1", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_TC1, I915_DISP_PW_ID_NONE, 0, ddi_io_tc1, ARRAY_SIZE(ddi_io_tc1) },
		{ "DDI_IO_TC2", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_TC2, I915_DISP_PW_ID_NONE, 0, ddi_io_tc2, ARRAY_SIZE(ddi_io_tc2) },
		{ "DDI_IO_TC3", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_TC3, I915_DISP_PW_ID_NONE, 0, ddi_io_tc3, ARRAY_SIZE(ddi_io_tc3) },
		{ "DDI_IO_TC4", I915_PW_OPS_ICL_DDI, 0, 0, 0, 0, 0, 0u, 0u, IDX_DDI_TC4, I915_DISP_PW_ID_NONE, 0, ddi_io_tc4, ARRAY_SIZE(ddi_io_tc4) },
		{ "PW_4", I915_PW_OPS_HSW, 0, 0, 1, 0, 0, 0u, BIT_PIPE_C, IDX_PW_4, I915_DISP_PW_ID_NONE, 0, pw_4, ARRAY_SIZE(pw_4) },
		{ "PW_5", I915_PW_OPS_HSW, 0, 0, 1, 0, 0, 0u, BIT_PIPE_D, IDX_PW_5, I915_DISP_PW_ID_NONE, 0, pw_5, ARRAY_SIZE(pw_5) },
		/* tgl_power_wells_aux */
		{ "AUX_A", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 0u, 0u, IDX_AUX_A, I915_DISP_PW_ID_NONE, 0, aux_a, ARRAY_SIZE(aux_a) },
		{ "AUX_B", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 0u, 0u, IDX_AUX_B, I915_DISP_PW_ID_NONE, 0, aux_b, ARRAY_SIZE(aux_b) },
		{ "AUX_C", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 0u, 0u, IDX_AUX_C, I915_DISP_PW_ID_NONE, 0, aux_c, ARRAY_SIZE(aux_c) },
		{ "AUX_USBC1", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 500u, 0u, IDX_AUX_TC1, I915_DISP_PW_ID_NONE, 0, aux_usbc1, ARRAY_SIZE(aux_usbc1) },
		{ "AUX_USBC2", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 500u, 0u, IDX_AUX_TC2, I915_DISP_PW_ID_NONE, 0, aux_usbc2, ARRAY_SIZE(aux_usbc2) },
		{ "AUX_USBC3", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 500u, 0u, IDX_AUX_TC3, I915_DISP_PW_ID_NONE, 0, aux_usbc3, ARRAY_SIZE(aux_usbc3) },
		{ "AUX_USBC4", I915_PW_OPS_ICL_AUX, 0, 0, 0, 0, 1, 500u, 0u, IDX_AUX_TC4, I915_DISP_PW_ID_NONE, 0, aux_usbc4, ARRAY_SIZE(aux_usbc4) },
		{ "AUX_TBT1", I915_PW_OPS_ICL_AUX, 0, 0, 0, 1, 0, 0u, 0u, IDX_AUX_TBT1, I915_DISP_PW_ID_NONE, 0, aux_tbt1, ARRAY_SIZE(aux_tbt1) },
		{ "AUX_TBT2", I915_PW_OPS_ICL_AUX, 0, 0, 0, 1, 0, 0u, 0u, IDX_AUX_TBT2, I915_DISP_PW_ID_NONE, 0, aux_tbt2, ARRAY_SIZE(aux_tbt2) },
		{ "AUX_TBT3", I915_PW_OPS_ICL_AUX, 0, 0, 0, 1, 0, 0u, 0u, IDX_AUX_TBT3, I915_DISP_PW_ID_NONE, 0, aux_tbt3, ARRAY_SIZE(aux_tbt3) },
		{ "AUX_TBT4", I915_PW_OPS_ICL_AUX, 0, 0, 0, 1, 0, 0u, 0u, IDX_AUX_TBT4, I915_DISP_PW_ID_NONE, 0, aux_tbt4, ARRAY_SIZE(aux_tbt4) }
	};

	/* Builds the wells and the map (as the XE_LPD map does). */
	i915_power_map_build(pd, wells, ARRAY_SIZE(wells));

	/* Succeeded: the Tiger Lake map is built. */
	return 0;
}

/* Names the driver request register of a well family (i915_reg.h). */
static unsigned
i915_pw_driver_reg(
	enum i915_pw_ops_kind ops)
{
	/* Picks the family's register. */
	switch (ops) {
	case I915_PW_OPS_ICL_AUX:
		return ICL_PWR_WELL_CTL_AUX2;
	case I915_PW_OPS_ICL_DDI:
		return ICL_PWR_WELL_CTL_DDI2;
	default:
		break;
	}

	/* Every other family uses HSW_PWR_WELL_CTL2 (the driver's). */
	return HSW_PWR_WELL_CTL2;
}

/* The request bit of a control index (HSW_PWR_WELL_CTL_REQ). */
static uint32_t
i915_pw_req(
	unsigned idx)
{
	/* Each index owns two bits, the request the upper one. */
	return 0x2u << (idx * 2u);
}

/* The state bit of a control index (HSW_PWR_WELL_CTL_STATE). */
static uint32_t
i915_pw_state(
	unsigned idx)
{
	/* Each index owns two bits, the state the lower one. */
	return 0x1u << (idx * 2u);
}

/*
 * Waits for a power gate's fuses (gen9_wait_for_power_well_fuses()): 0, or
 * EIO on a time-base fault.
 */
static int
i915_pw_wait_fuse(
	struct i915_pw_ctx *pwc,
	unsigned pg)
{
	uint32_t bit;
	int wait_result;

	/*
	 * intel_de_wait_for_set(..., 1): 2 us atomic and 1 ms sleeping for every
	 * power gate (the 5 us / 1 us of the reference is only a comment).  PG0
	 * is bit 27, PG1 bit 26 and so on (SKL_FUSE_PG_DIST).
	 */
	bit = 1u << (27u - pg);
	wait_result = drv_i915_wait_reg(pwc->mmio, SKL_FUSE_STATUS, bit, bit, 2u, 1u, NULL);

	/* A time-base anomaly: the caller must stop. */
	if (wait_result == EIO)
		return EIO;

	/* A plain timeout is warned and the enable continues. */
	if (wait_result != 0)
		kern_logf("i915: power well fuse PG%u wait timeout (continuing)\n", pg);

	/* Succeeded: the wait is over. */
	return 0;
}

/* Runs a well's post-enable steps (hsw_power_well_post_enable()): the VGA reset and the pipe interrupts. */
static void
i915_pw_post_enable(
	struct i915_power_well *well,
	struct i915_pw_ctx *pwc)
{
	/* intel_vga_reset_io_mem(): keeps vgacon sane after the well is on. */
	if (well->has_vga) {
		drv_i915_vga_reset_io_mem(pwc->vga);
		pwc->vga_reset_calls++;
	}

	/*
	 * gen8_irq_power_well_post_enable(): restores the pipe's interrupt
	 * registers, which live in the well.  It acts only once interrupts are
	 * enabled; before they are installed it does nothing.
	 */
	if (well->irq_pipe_mask != 0u) {
		if (pwc->irqs_enabled) {
			pwc->irq_post_enable_calls++;
			if (pwc->irq_ops != NULL)
				pwc->irq_ops->post_enable(pwc->irq_ctx, well->irq_pipe_mask);
		}
	}
}

/* Tells whether a domain is in a mask (1 or 0). */
static int
i915_mask_test(
	const struct i915_pw_domain_mask *mask,
	unsigned domain)
{
	/* Reports the domain's bit. */
	return (int)((mask->bits[domain >> 6] >> (domain & 63u)) & 1u);
}

/* Adds a domain to a mask. */
static void
i915_mask_set(
	struct i915_pw_domain_mask *mask,
	unsigned domain)
{
	/* Sets the domain's bit. */
	mask->bits[domain >> 6] |= (uint64_t)1u << (domain & 63u);
}

/* Removes a domain from a mask. */
static void
i915_mask_clear(
	struct i915_pw_domain_mask *mask,
	unsigned domain)
{
	/* Clears the domain's bit. */
	mask->bits[domain >> 6] &= ~((uint64_t)1u << (domain & 63u));
}

/* Tells whether a mask holds no domain (1 or 0). */
static int
i915_mask_empty(
	const struct i915_pw_domain_mask *mask)
{
	/* Any bit in either word is a domain. */
	if (mask->bits[0] != 0u)
		return 0;
	if (mask->bits[1] != 0u)
		return 0;

	/* Succeeded: the mask is empty. */
	return 1;
}

/* Tells whether a domain is parked in either set of the asynchronous put (1 or 0). */
static int
i915_parked(
	struct i915_power_domains *pd,
	unsigned domain)
{
	int in_set;

	/* Looks in the set the work releases next. */
	in_set = i915_mask_test(&pd->async_put_domains[0], domain);
	if (in_set)
		return 1;

	/* Looks in the set parked while the work was pending. */
	in_set = i915_mask_test(&pd->async_put_domains[1], domain);
	if (in_set)
		return 1;

	/* Succeeded: the domain is not parked. */
	return 0;
}

/* Tells whether anything is parked in either set of the asynchronous put (1 or 0). */
static int
i915_parked_any(
	struct i915_power_domains *pd)
{
	int empty;

	/* Looks at the set the work releases next. */
	empty = i915_mask_empty(&pd->async_put_domains[0]);
	if (!empty)
		return 1;

	/* Looks at the set parked while the work was pending. */
	empty = i915_mask_empty(&pd->async_put_domains[1]);
	if (!empty)
		return 1;

	/* Succeeded: nothing is parked. */
	return 0;
}

/*
 * Checks the asynchronous put's invariants (verify_async_put_domains_state()):
 * the two masks are disjoint, the wakeref flag matches "something is
 * parked", and every parked domain holds exactly one reference.
 */
static void
i915_verify_async_put_domains_state(
	struct i915_power_domains *pd)
{
	unsigned domain;
	int error;
	int parked;
	int wakeref_held;

	/* The two sets must not share a domain. */
	error = 0;
	if ((pd->async_put_domains[0].bits[0] & pd->async_put_domains[1].bits[0]) != 0u)
		error = 1;
	else if ((pd->async_put_domains[0].bits[1] & pd->async_put_domains[1].bits[1]) != 0u)
		error = 1;

	/* The wakeref flag must be set exactly while something is parked. */
	parked = i915_parked_any(pd);
	wakeref_held = 0;
	if (pd->async_put_wakeref != 0)
		wakeref_held = 1;
	if (wakeref_held != parked)
		error = 1;

	/* Every parked domain must hold exactly one reference. */
	for (domain = 0u; domain < I915_PW_DOMAIN_NUM; domain++) {
		parked = i915_parked(pd, domain);
		if (parked && pd->domain_use_count[domain] != 1u)
			error = 1;
	}

	/* Reports an inconsistent state. */
	if (error) {
		pd->async_state_errors++;
		kern_logf("i915: power: async put state inconsistent (wakeref=%d [0]=%016llx:%016llx "
			"[1]=%016llx:%016llx)\n", pd->async_put_wakeref,
			(unsigned long long)pd->async_put_domains[0].bits[1],
			(unsigned long long)pd->async_put_domains[0].bits[0],
			(unsigned long long)pd->async_put_domains[1].bits[1],
			(unsigned long long)pd->async_put_domains[1].bits[0]);
	}
}

/* Cancels the delayed work of the asynchronous put (cancel_async_put_work()). */
static void
i915_cancel_async_put_work(
	struct i915_power_domains *pd,
	int sync)
{
	/* Cancels the work when one is bound. */
	if (pd->async_ops != NULL)
		(void)pd->async_ops->cancel(pd->async_ctx, sync);

	/* No delay is pending any more. */
	pd->async_put_next_delay = 0;
}

/*
 * Takes a parked reference back for a get
 * (intel_display_power_grab_async_put_ref()): 1 when it did.
 */
static int
i915_grab_async_put_ref(
	struct i915_power_domains *pd,
	unsigned domain)
{
	int grabbed;
	int parked;

	/* Looks for the domain among the parked ones. */
	grabbed = 0;
	parked = i915_parked(pd, domain);

	/* Takes the parked reference back, cancelling the work when nothing else is parked. */
	if (parked) {
		i915_mask_clear(&pd->async_put_domains[0], domain);
		i915_mask_clear(&pd->async_put_domains[1], domain);
		grabbed = 1;
		pd->async_grabs++;
		parked = i915_parked_any(pd);
		if (!parked) {
			i915_cancel_async_put_work(pd, 0);
			pd->async_put_wakeref = 0;
		}
	}

	/* Checks the invariants whatever happened. */
	i915_verify_async_put_domains_state(pd);

	/* Succeeded: reports whether a parked reference was taken back. */
	return grabbed;
}

/*
 * Takes a domain with the domains lock held (__intel_display_power_get_domain()):
 * 0, or the error of the well that did not turn on.
 */
static int
i915_get_domain_locked(
	struct i915_power_domains *pd,
	enum i915_power_domain domain,
	struct i915_pw_ctx *pwc)
{
	uint64_t wells;
	unsigned index;
	unsigned taken;
	int grabbed;
	int get_result;

	/* A parked reference is taken back without touching the hardware. */
	wells = drv_i915_power_domain_wells(pd, domain);
	grabbed = i915_grab_async_put_ref(pd, (unsigned)domain);
	if (grabbed)
		return 0;

	/* Gets the domain's wells in ascending (the reference's enabling) order. */
	for (index = 0u; index < pd->num_power_wells; index++) {
		if ((wells & ((uint64_t)1u << index)) == 0u)
			continue;

		/* Takes the well. */
		get_result = drv_i915_power_well_get(&pd->power_wells[index], pwc);
		if (get_result != 0) {
			/* Gives the wells already taken back, in reverse. */
			taken = index;
			while (taken-- > 0u) {
				if ((wells & ((uint64_t)1u << taken)) != 0u)
					drv_i915_power_well_put(&pd->power_wells[taken], pwc);
			}

			/* Reports the well that did not turn on. */
			return get_result;
		}
	}

	/* Counts the reference on the domain. */
	pd->domain_use_count[domain]++;

	/* Succeeded: every well of the domain is held. */
	return 0;
}

/* Puts a domain with the domains lock held (__intel_display_power_put_domain()). */
static void
i915_put_domain_locked(
	struct i915_power_domains *pd,
	enum i915_power_domain domain,
	struct i915_pw_ctx *pwc)
{
	uint64_t wells;
	unsigned index;
	int parked;

	/* The domain's count of references; a put at zero is reported, not counted. */
	wells = drv_i915_power_domain_wells(pd, domain);
	if (pd->domain_use_count[domain] == 0u) {
		pd->use_count_errors++;
		kern_logf("i915: power: use count on domain %u is already zero\n", (unsigned)domain);
	} else {
		pd->domain_use_count[domain]--;
	}

	/* A domain still parked for the asynchronous put should not be put again. */
	parked = i915_parked(pd, (unsigned)domain);
	if (parked) {
		pd->async_state_errors++;
		kern_logf("i915: power: async disabling of domain %u is pending\n", (unsigned)domain);
	}

	/* Puts the domain's wells in reverse order. */
	index = pd->num_power_wells;
	while (index-- > 0u) {
		if ((wells & ((uint64_t)1u << index)) != 0u)
			drv_i915_power_well_put(&pd->power_wells[index], pwc);
	}
}

/* Queues the delayed work for the parked references (queue_async_put_domains_work()). */
static void
i915_queue_async_put_domains_work(
	struct i915_power_domains *pd,
	int delay_ms)
{
	int queued;

	/* The work owns the parked references from here; it must not own any already. */
	if (pd->async_put_wakeref != 0)
		pd->async_state_errors++;
	pd->async_put_wakeref = 1;

	/* Queues the work; one already queued is an inconsistency. */
	queued = pd->async_ops->queue(pd->async_ctx, delay_ms);
	if (!queued) {
		pd->async_state_errors++;
		kern_logf("i915: power: async put work was already queued\n");
	}
}

/* Puts every parked domain of a mask (release_async_put_domains()). */
static void
i915_release_async_put_domains(
	struct i915_power_domains *pd,
	const struct i915_pw_domain_mask *mask)
{
	struct i915_pw_domain_mask release;
	unsigned domain;
	int in_mask;

	/* Copies the mask: it may be one of the two sets being cleared. */
	release = *mask;

	/* Puts each domain of the mask. */
	for (domain = 0u; domain < I915_PW_DOMAIN_NUM; domain++) {
		in_mask = i915_mask_test(&release, domain);
		if (!in_mask)
			continue;

		/* Unparks the domain before the put, so the put's check is satisfied. */
		i915_mask_clear(&pd->async_put_domains[0], domain);
		i915_mask_clear(&pd->async_put_domains[1], domain);
		i915_put_domain_locked(pd, (enum i915_power_domain)domain, pd->async_pwc);
		pd->async_released++;
	}
}

/* The DC state bits a display version has (gen9_dc_mask()). */
static uint32_t
i915_gen9_dc_mask(
	int display_ver)
{
	uint32_t mask;

	/* DC5 always; DC6 and DC9 by version, DC3CO from 12. */
	mask = I915_DC_STATE_EN_UPTO_DC5;
	if (display_ver >= 12) {
		mask |= I915_DC_STATE_EN_DC3CO | I915_DC_STATE_EN_UPTO_DC6 |
			I915_DC_STATE_EN_DC9;
	} else if (display_ver == 11) {
		mask |= I915_DC_STATE_EN_UPTO_DC6 | I915_DC_STATE_EN_DC9;
	} else {
		mask |= I915_DC_STATE_EN_UPTO_DC6;
	}

	/* Succeeded: reports the version's bits. */
	return mask;
}

/* Writes the DC state until it sticks (gen9_write_dc_state(): the DMC may keep returning the old value). */
static void
i915_gen9_write_dc_state(
	struct i915_pw_ctx *pwc,
	uint32_t state)
{
	unsigned rewrites;
	unsigned rereads;
	uint32_t value;

	/* Writes the state once. */
	drv_i915_raw_write32(pwc->mmio, DC_STATE_EN, state);
	pwc->dc_state_writes++;

	/*
	 * Rewrites while the register reads back another value, up to 100
	 * times, and stops once the value was read back unchanged more than 5
	 * times in a row.
	 */
	rewrites = 0u;
	rereads = 0u;
	do {
		value = drv_i915_raw_read32(pwc->mmio, DC_STATE_EN);
		if (value != state) {
			drv_i915_raw_write32(pwc->mmio, DC_STATE_EN, state);
			rewrites++;
			rereads = 0u;
		} else {
			/* The value stuck for more than 5 reads in a row. */
			if (rereads > 5u)
				break;
			rereads++;
		}
	} while (rewrites < 100u);

	/* Counts the rewrites and reports a state that never stuck. */
	pwc->dc_state_rewrites += rewrites;
	if (value != state)
		kern_logf("i915: Writing dc state to 0x%x failed, now 0x%x\n", state, value);
}

/* Clears and sets bits of a display-core register with raw accesses. */
static void
i915_power_rmw(
	struct i915_mmio *mmio,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	uint32_t value;

	/* Reads, changes and writes the register back. */
	value = drv_i915_raw_read32(mmio, reg);
	drv_i915_raw_write32(mmio, reg, (value & ~clear) | set);
}

/*
 * The trace ring of the display core's combo PHY init: the display's own
 * (the display core is a member of the display).
 *
 * The ring is far larger than a kernel thread stack, so it is never an
 * automatic variable; the display-core init and the DC_off enable are
 * serialized (one start at a time, the domains lock), so one ring serves
 * both.
 */
static struct i915_trace *
i915_core_trace(
	struct i915_display_core *dc)
{
	struct i915_display *display;

	/* The display core is the dcore member of its display. */
	display = container_of(dc, struct i915_display, dcore);

	/* Succeeded: reports the display's ring. */
	return &display->dc_trace;
}

/* The same trace ring, reached from the well context (the pwc member of the display). */
static struct i915_trace *
i915_dc_off_trace(
	struct i915_pw_ctx *pwc)
{
	struct i915_display *display;

	/* The well context is the pwc member of its display. */
	display = container_of(pwc, struct i915_display, pwc);

	/* Succeeded: reports the display's ring. */
	return &display->dc_trace;
}

/*
 * The DBUF slices the display has: the device's mask (XE_LPD S1..S4, Tiger
 * Lake S1..S2), or all four when none was set.
 */
static uint8_t
i915_dbuf_slice_mask(
	const struct i915_display_core *dc)
{
	/* A display without a mask is treated as having all four. */
	if (dc->dbuf_slice_mask == 0u)
		return 0xFu;

	/* Succeeded: reports the display's slices. */
	return dc->dbuf_slice_mask;
}

/* Leaves every DC state (gen9_set_dc_state(DC_STATE_DISABLE) of the core init). */
static void
i915_gen9_set_dc_state_disable(
	struct i915_display_core *dc)
{
	uint32_t mask;
	uint32_t value;

	/* The DC5 / DC6 bits, and DC3CO when it is allowed. */
	mask = DC_STATE_EN_UPTO_DC5_DC6;
	if ((dc->pd->allowed_dc_mask & DC_STATE_EN_DC3CO) != 0u)
		mask |= DC_STATE_EN_DC3CO;

	/* Clears them (state = DC_STATE_DISABLE, 0). */
	value = drv_i915_raw_read32(dc->m, DC_STATE_EN);
	value &= ~mask;
	drv_i915_raw_write32(dc->m, DC_STATE_EN, value);
}

/* Enables the PCH reset handshake (intel_pch_reset_handshake(enable): no PICA bit before display version 14). */
static void
i915_pch_reset_handshake(
	struct i915_display_core *dc)
{
	/* Sets the handshake bit. */
	i915_power_rmw(dc->m, HSW_NDE_RSTWRN_OPT, RESET_PCH_HANDSHAKE_ENABLE, RESET_PCH_HANDSHAKE_ENABLE);
}

/* Powers one DBUF slice on or off (gen9_dbuf_slice_set(): request, posting read, 10 us, state). */
static void
i915_gen9_dbuf_slice_set(
	struct i915_display_core *dc,
	unsigned slice,
	int enable)
{
	uint32_t reg;
	uint32_t request;
	uint32_t value;
	int state;
	const char *action;

	/* Writes the request bit. */
	reg = i915_dbuf_ctl_s[slice];
	request = 0u;
	if (enable)
		request = DBUF_POWER_REQUEST;
	i915_power_rmw(dc->m, reg, DBUF_POWER_REQUEST, request);

	/* Posts the write and gives the slice 10 us. */
	(void)drv_i915_raw_read32(dc->m, reg);
	(void)drv_i915_udelay(10u);

	/* Reads the slice's power state back. */
	value = drv_i915_raw_read32(dc->m, reg);
	state = 0;
	if ((value & DBUF_POWER_STATE) != 0u)
		state = 1;

	/* Reports a slice that did not follow the request. */
	if ((enable ? 1 : 0) != state) {
		action = "disable";
		if (enable)
			action = "enable";
		kern_logf("i915: DBUF slice %u power %s timeout\n", slice, action);
	}
}

/* Sets every DBUF slice to the request under the domains lock and records the result. */
static void
i915_gen9_dbuf_slices_update(
	struct i915_display_core *dc,
	uint8_t req_slices)
{
	unsigned slice;
	int enable;

	/* Programs the four slices under the domains lock. */
	mutex_lock(&dc->pd->lock);

	for (slice = 0u; slice < 4u; slice++) {
		enable = 0;
		if ((req_slices & (1u << slice)) != 0u)
			enable = 1;
		i915_gen9_dbuf_slice_set(dc, slice, enable);
	}

	/* Records the slices now requested. */
	dc->dbuf_enabled_slices = req_slices;

	mutex_unlock(&dc->pd->lock);
}

/*
 * Gives every DBUF slice of the display tracker service level 8
 * (gen12_dbuf_slices_config()).
 *
 * Alder Lake-P returns at the top of the reference's function and keeps
 * what the firmware left.
 */
static void
i915_gen12_dbuf_slices_config(
	struct i915_display_core *dc)
{
	uint8_t mask;
	unsigned slice;
	unsigned done;

	/* Alder Lake-P keeps the firmware's service levels. */
	if (dc->is_alderlake_p)
		return;

	/* Programs every slice of the display. */
	mask = i915_dbuf_slice_mask(dc);
	done = 0u;
	for (slice = 0u; slice < 4u; slice++) {
		if ((mask & (1u << slice)) == 0u)
			continue;
		i915_power_rmw(dc->m, i915_dbuf_ctl_s[slice], DBUF_TRACKER_STATE_SERVICE_MASK,
			DBUF_TRACKER_STATE_SERVICE_8);
		done++;
	}

	/* Logs what was programmed. */
	kern_logf("i915: P3 gen12_dbuf_slices_config: tracker state service 8 on %u slice(s) of mask 0x%x\n",
		done, mask);
}

/* Enables the DBUF (gen9_dbuf_enable(): what the firmware left, plus at least slice S1). */
static void
i915_gen9_dbuf_enable(
	struct i915_display_core *dc)
{
	uint8_t enabled;
	uint8_t slices_mask;

	/* Keeps what the firmware left and adds BIT(DBUF_S1). */
	enabled = drv_i915_enabled_dbuf_slices_mask(dc);
	slices_mask = (uint8_t)(1u | enabled);

	/* Powers the slices within the display's own. */
	i915_gen9_dbuf_slices_update(dc, (uint8_t)(slices_mask & i915_dbuf_slice_mask(dc)));
}

/*
 * Gives the MBUS ABOX credit pools their credits (icl_mbus_init()).
 *
 * The reference returns for Alder Lake-P and display 14+, and otherwise
 * walks the device's ABOX mask -- plus ABOX0 on display version 12: the
 * gen12 platforms that read pixel data through ABOX1 and ABOX2 still expect
 * ABOX0 to be given credits, although their other instance-0 registers
 * (BW_BUDDY) are left alone.
 */
static void
i915_icl_mbus_init(
	struct i915_display_core *dc)
{
	static const uint32_t abox_ctl[3] = { MBUS_ABOX0_CTL, MBUS_ABOX1_CTL, MBUS_ABOX2_CTL };
	uint32_t mask;
	uint32_t value;
	unsigned abox;
	unsigned index;
	unsigned done;

	/* Alder Lake-P and display 14+ program none. */
	if (dc->is_alderlake_p || dc->display_ver >= 14)
		return;

	/* The ABOXes of the device, and ABOX0 on display version 12. */
	abox = dc->abox_mask;
	if (dc->display_ver == 12)
		abox |= 1u;

	/* POOL1 16, POOL2 16, B 1, BW 1 on each ABOX. */
	mask = MBUS_ABOX_BT_CREDIT_POOL1_MASK | MBUS_ABOX_BT_CREDIT_POOL2_MASK |
		MBUS_ABOX_B_CREDIT_MASK | MBUS_ABOX_BW_CREDIT_MASK;
	value = MBUS_ABOX_BT_CREDIT_POOL1_16 | MBUS_ABOX_BT_CREDIT_POOL2_16 |
		MBUS_ABOX_B_CREDIT_1 | MBUS_ABOX_BW_CREDIT_1;
	done = 0u;
	for (index = 0u; index < 3u; index++) {
		if ((abox & (1u << index)) == 0u)
			continue;
		i915_power_rmw(dc->m, abox_ctl[index], mask, value);
		done++;
	}

	/* Logs what was programmed. */
	kern_logf("i915: P3 icl_mbus_init: %u ABOX register(s) of mask 0x%x given the "
		"reference credits (POOL1 16, POOL2 16, B 1, BW 1)\n", done, abox);
}

/* Programs the arbiter's BW_BUDDY from the DRAM configuration (tgl_bw_buddy_init()). */
static void
i915_tgl_bw_buddy_init(
	struct i915_display_core *dc)
{
	const struct i915_buddy_mask *row;
	unsigned index;
	unsigned abox;
	unsigned buddy;
	const char *wa_note;

	/* Looks the DRAM configuration up. */
	row = NULL;
	for (index = 0u; i915_tgl_buddy[index].nch != 0u; index++) {
		if (i915_tgl_buddy[index].nch == dc->dram_channels &&
		    i915_tgl_buddy[index].type == dc->dram_type) {
			row = &i915_tgl_buddy[index];
			break;
		}
	}

	/* The ABOXes of this display (its device info's abox_mask). */
	abox = dc->abox_mask;

	/* An unknown memory configuration disables the address buddy logic. */
	if (row == NULL) {
		for (buddy = 0u; buddy < 3u; buddy++) {
			if ((abox & (1u << buddy)) != 0u)
				drv_i915_raw_write32(dc->m, BW_BUDDY_CTL_BASE + buddy * BW_BUDDY_STRIDE, BW_BUDDY_DISABLE);
		}

		/* Logs the disabled buddy logic. */
		kern_logf("i915: BW_BUDDY: unknown DRAM config (ch=%u type=%d); disabled\n",
			dc->dram_channels, dc->dram_type);
		return;
	}

	/* Gives each ABOX the page mask, and on display version 12 the TLB request timer (Wa_22010178259). */
	for (buddy = 0u; buddy < 3u; buddy++) {
		if ((abox & (1u << buddy)) == 0u)
			continue;
		drv_i915_raw_write32(dc->m, BW_BUDDY_PAGE_MASK_BASE + buddy * BW_BUDDY_STRIDE, row->page_mask);
		if (dc->display_ver == 12) {
			i915_power_rmw(dc->m, BW_BUDDY_CTL_BASE + buddy * BW_BUDDY_STRIDE,
				BW_BUDDY_TLB_REQ_TIMER_MASK, BW_BUDDY_TLB_REQ_TIMER_8);
		}
	}

	/* Logs what was programmed. */
	wa_note = "";
	if (dc->display_ver == 12)
		wa_note = " + TLB request timer 0x8 (Wa_22010178259)";
	kern_logf("i915: BW_BUDDY: ABOX mask 0x%x page_mask 0x%x%s\n", abox, row->page_mask, wa_note);
}

/* Records the first adaptation-layer fault of the display-core init and where it stopped. */
static void
i915_core_fault(
	struct i915_display_core *dc,
	const char *where)
{
	/* Keeps the first fault and its position. */
	if (!dc->fault_stop) {
		dc->fault_stop = 1;
		dc->fault_where = where;
	}

	/* Logs every stop. */
	kern_logf("i915: display_core: stop at %s (adaptation-layer fault)\n", where);
}

/*
 * Brings the display core up (icl_display_core_init(resume = false)): 0, or
 * EIO when an adaptation-layer fault stopped it.
 */
static int
i915_icl_display_core_init(
	struct i915_display_core *dc,
	int resume)
{
	struct i915_trace *trace;
	unsigned combo_initialised;
	int index;
	int enable_result;
	int faulted;

	/* The DMC reload of a resume is done later on these platforms, not here. */
	UNUSED_PARAMETER(resume);

	/* Starts the trace ring of the combo PHY init. */
	trace = i915_core_trace(dc);
	drv_i915_trace_init(trace);

	/* gen9_set_dc_state(DC_STATE_DISABLE). */
	dc->last_child = 1;
	i915_gen9_set_dc_state_disable(dc);

	/* 1. Enables the PCH reset handshake. */
	dc->last_child = 2;
	i915_pch_reset_handshake(dc);

	/* 2. Initializes every combo PHY; the reference is void, so the fault state is checked after. */
	dc->last_child = 3;
	combo_initialised = 0u;
	(void)drv_i915_combo_phy_init(dc->m, trace, &combo_initialised);
	faulted = drv_i915_time_base_faulted();
	if (faulted != 0) {
		i915_core_fault(dc, "intel_combo_phy_init");
		return EIO;
	}

	/* 3. Enables power well 1 (PG1); the AUX wells enable on demand. */
	dc->last_child = 4;
	index = drv_i915_power_well_by_id(dc->pd, I915_SKL_DISP_PW_1);
	if (index < 0) {
		i915_core_fault(dc, "lookup_power_well(PW_1)");
		return EIO;
	}

	/* Enables the well under the domains lock. */
	mutex_lock(&dc->pd->lock);

	enable_result = drv_i915_power_well_enable(&dc->pd->power_wells[index], dc->pwc);

	mutex_unlock(&dc->pd->lock);

	/* Stops on a time-base fault of the enable. */
	if (enable_result == EIO) {
		i915_core_fault(dc, "power_well_1_enable");
		return EIO;
	}

	/* 4. Enables the CDCLK; the fault state is checked after. */
	dc->last_child = 5;
	drv_i915_cdclk_init_hw(dc->cd);
	faulted = drv_i915_time_base_faulted();
	if (faulted != 0) {
		i915_core_fault(dc, "intel_cdclk_init_hw");
		return EIO;
	}

	/*
	 * gen12_dbuf_slices_config(): run on every display 12+, returning at
	 * once on Alder Lake-P.  Tiger Lake needs it: without service level 8
	 * the display's data path underruns continuously while a picture is up.
	 */
	if (dc->display_ver >= 12)
		i915_gen12_dbuf_slices_config(dc);

	/* 5. Enables the DBUF. */
	dc->last_child = 6;
	i915_gen9_dbuf_enable(dc);

	/*
	 * 6. icl_mbus_init(): the MBUS ABOX credit pools.  Without it Tiger
	 * Lake's display starves and the pipe underruns while a picture is up.
	 */
	i915_icl_mbus_init(dc);

	/* 7. Programs the arbiter's BW_BUDDY registers. */
	dc->last_child = 7;
	i915_tgl_bw_buddy_init(dc);

	/* Wa_14011508470 (display IP 12.0..13.0). */
	i915_power_rmw(dc->m, GEN11_CHICKEN_DCPR_2, 0u,
		DCPR_CLEAR_MEMSTAT_DIS | DCPR_SEND_RESP_IMM |
		DCPR_MASK_LPMODE | DCPR_MASK_MAXLATENCY_MEMUP_CLR);

	/* Wa_14011503030:xelpd. */
	drv_i915_raw_write32(dc->m, XELPD_DISPLAY_ERR_FATAL_MASK, ~0u);

	/* Succeeded: the display core is up. */
	return 0;
}

/* Counts the wells that are on as the driver owns them. */
static unsigned
i915_wells_on(
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc)
{
	unsigned index;
	unsigned count;
	int enabled;

	/* Asks every well. */
	count = 0u;
	for (index = 0u; index < pd->num_power_wells; index++) {
		enabled = drv_i915_power_well_is_enabled(&pd->power_wells[index], pwc);
		if (enabled)
			count++;
	}

	/* Succeeded: reports how many are on. */
	return count;
}
