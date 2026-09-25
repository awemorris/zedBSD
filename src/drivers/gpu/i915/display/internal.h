/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_display_limits.h),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_global_state.h),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2020 Intel Corporation
 */

/*
 * The display part's own types: what every display file shares.
 *
 * This header is private to src/drivers/gpu/i915/display/.  It holds three
 * things, in this order:
 *
 *   the Linux base      the few integer types and register helpers whose
 *                       meaning was identical in every display translation
 *                       unit of the old tree, so that one definition serves
 *                       all of them;
 *   the display state   the zedBSD types the display files pass to each
 *                       other (power wells, clocks, the panel, the VBT, the
 *                       scanout, the resident present path);
 *   the display root    struct i915_display, which the device owns and which
 *                       holds every long-lived display object.
 *
 * The Linux display code is compiled in one of five Linux environments,
 * each a private header of its own:
 *
 *   modeset-internal.h   the modeset, DDI, DPLL, plane, colour, backlight and
 *                        vblank text (with watermark-internal.h and
 *                        takeover-internal.h layered on it);
 *   dp-internal.h        the DP AUX, panel power sequencer, DP helper and
 *                        EDID text (on vbt.h);
 *   vbt.h                the VBT parser's environment;
 *   hotplug-internal.h   the hotplug, GMBUS and HDMI detect text;
 *   opregion-internal.h  the OpRegion and ACPI text.
 *
 * The same Linux type name (struct drm_i915_private, struct intel_dp,
 * struct drm_display_mode and others) has a different layout in each
 * environment, and several Linux helper names (intel_de_read, mutex_lock,
 * the connector and crtc iterators) mean something different in each.  A
 * translation unit therefore includes at most one environment: each one
 * records itself in I915_DISPLAY_LINUX_WORLD and refuses to be included
 * next to another.  A helper name whose meaning differed between
 * environments is not defined under its Linux name at all; every meaning
 * has an explicit name of its own in the environment it belongs to.
 *
 * Nothing in this header depends on an environment, so every display file
 * includes it first, and a file that uses no Linux text includes nothing
 * else of the display part.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_INTERNAL_H
#define DRIVERS_GPU_I915_DISPLAY_INTERNAL_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#include "../sync.h"
#include "../trace.h"
#include "../workqueue.h"

/*
 * Marks a parameter a function receives by contract but does not use.
 *
 * The same definition as ../i915.h, for display files that do not include
 * it.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/*
 * The Linux base: integer types.
 *
 * u64 and s64 were unsigned long long and long long in the hotplug
 * translation units and uint64_t and int64_t everywhere else; both are 64
 * bits wide, so only the spelling of the type (and the printf length
 * modifier a caller needs) differs.  The uint64_t spelling is kept.
 */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/*
 * One display register, as the Linux register definitions name it.
 *
 * Wrapping the offset in a structure keeps a register from being passed
 * where a value is expected; every Linux register macro of the intel/ headers
 * builds one with _MMIO().
 */
typedef struct {
	/* The register's offset in the MMIO BAR. */
	u32 reg;
} i915_reg_t;

/* Builds the register value of a Linux register macro. */
#define _MMIO(r) ((const i915_reg_t){ .reg = (r) })

/* The register value that names no register. */
#define INVALID_MMIO_REG _MMIO(0)

/* Bit n of a 32-bit register. */
#define REG_BIT(n) ((u32)1u << (n))

/* The contiguous bits h down to l of a 32-bit register. */
#define REG_GENMASK(h, l) ((u32)((0xffffffffu >> (31 - (h))) & (0xffffffffu << (l))))

/*
 * Places a field value into its mask, and takes it out again.
 *
 * The modeset translation units divided by the mask's lowest bit where the
 * DP ones shifted by its position; for a nonzero 32-bit mask both give the
 * same value, so the shift is kept.
 */
#define REG_FIELD_PREP(mask, val) ((u32)((((u32)(val)) << __builtin_ctz(mask)) & (mask)))
#define REG_FIELD_GET(mask, val) ((u32)((((u32)(val)) & (mask)) >> __builtin_ctz(mask)))

/* Bit n of a mask that is not a register (the Linux BIT()). */
#define BIT(n) (1u << (n))

/* Picks the register of instance index from the first two, evenly spaced. */
#define _PICK_EVEN(index, a, b) ((a) + (index) * ((b) - (a)))

/*
 * Picks from two evenly spaced ranges, the second starting at c_index.
 *
 * The modeset translation units added the Linux BUILD_BUG_ON_ZERO() check,
 * which was defined to 0 there; the value is the same without it.
 */
#define _PICK_EVEN_2RANGES(index, c_index, a, b, c, d) \
	((index) < (c_index) ? _PICK_EVEN(index, a, b) : _PICK_EVEN((index) - (c_index), c, d))

/*
 * The register of a port, from the registers of ports A and B.
 *
 * The DP translation units spelled this through _PORT(), which is
 * _PICK_EVEN() under another name.
 */
#define _MMIO_PORT(port, a, b) _MMIO(_PICK_EVEN(port, a, b))

/* The number of elements of an array. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* n divided by d, rounded up. */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* The structure a member pointer points into. */
#define container_of(ptr, type, member) ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/*
 * The smaller and the larger of two values, each evaluated once.
 *
 * The hotplug translation units used a form that evaluated its arguments
 * twice.  Their only uses are the GMBUS transfer lengths, where the second
 * argument is a constant or gmbus_max_xfer_size(), which only reads the
 * display version; evaluating it once does not change the result.
 */
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ({ t _a = (t)(a); t _b = (t)(b); _a < _b ? _a : _b; })
#define max_t(t, a, b) ({ t _a = (t)(a); t _b = (t)(b); _a > _b ? _a : _b; })

/* The offset of a register value. */
static __inline u32
i915_mmio_reg_offset(
	i915_reg_t reg)
{
	/* Reports the offset the register value carries. */
	return reg.reg;
}

/* Tells whether a register value names a register. */
static __inline bool
i915_mmio_reg_valid(
	i915_reg_t reg)
{
	/* Offset 0 is INVALID_MMIO_REG. */
	if (reg.reg == 0U)
		return false;

	/* Succeeded: the value names a register. */
	return true;
}

/* Tells whether two register values name the same register. */
static __inline bool
i915_mmio_reg_equal(
	i915_reg_t a,
	i915_reg_t b)
{
	/* Registers are the same when their offsets are. */
	if (a.reg != b.reg)
		return false;

	/* Succeeded: both name one register. */
	return true;
}

/*
 * The Linux environment protocol.
 *
 * Each environment header starts with
 *
 *	#ifdef I915_DISPLAY_LINUX_WORLD
 *	#error "..."
 *	#endif
 *	#define I915_DISPLAY_LINUX_WORLD "<its name>"
 *
 * and defines I915_DISPLAY_WORLD_<NAME> for the headers layered on it
 * (watermark-internal.h and takeover-internal.h require
 * I915_DISPLAY_WORLD_MODESET, dp-internal.h requires I915_DISPLAY_WORLD_VBT
 * and replaces the world name with "dp").
 */

/*
 * The power wells and domains (power.c).
 */
/*
 * display power domains (structure + map), see power.c.
 *
 * Ports intel_power_domains_init(): option sanitize, allowed_dc_mask,
 * target_dc_state, lock, async-put work, and intel_display_power_map_init() --
 * the platform power-well descriptor set (ADL-P = display ver 13 -> xelpd, 30
 * wells) built into a power_wells[] array plus a per-domain -> wells map.  The
 * descriptor order, ids, hsw control indices and attributes are the reference
 * values (NOT inferred from array position).  Per-well refcount / hardware-
 * enabled / always_on are tracked SEPARATELY.  The power-well enable/disable/sync
 * MMIO is the init_hw (5.2) step; here the ops are connected by kind.
 *
 * DC state values are the reference register-bit encodings (not abstract flags).
 */

struct i915_trace;

/* enum intel_display_power_domain (v6.8 order preserved). */
enum i915_power_domain {
	I915_PW_DOMAIN_DISPLAY_CORE = 0,
	I915_PW_DOMAIN_PIPE_A, I915_PW_DOMAIN_PIPE_B,
	I915_PW_DOMAIN_PIPE_C, I915_PW_DOMAIN_PIPE_D,
	I915_PW_DOMAIN_PIPE_PANEL_FITTER_A, I915_PW_DOMAIN_PIPE_PANEL_FITTER_B,
	I915_PW_DOMAIN_PIPE_PANEL_FITTER_C, I915_PW_DOMAIN_PIPE_PANEL_FITTER_D,
	I915_PW_DOMAIN_TRANSCODER_A, I915_PW_DOMAIN_TRANSCODER_B,
	I915_PW_DOMAIN_TRANSCODER_C, I915_PW_DOMAIN_TRANSCODER_D,
	I915_PW_DOMAIN_TRANSCODER_EDP, I915_PW_DOMAIN_TRANSCODER_DSI_A,
	I915_PW_DOMAIN_TRANSCODER_DSI_C, I915_PW_DOMAIN_TRANSCODER_VDSC_PW2,
	I915_PW_DOMAIN_PORT_DDI_LANES_A, I915_PW_DOMAIN_PORT_DDI_LANES_B,
	I915_PW_DOMAIN_PORT_DDI_LANES_C, I915_PW_DOMAIN_PORT_DDI_LANES_D,
	I915_PW_DOMAIN_PORT_DDI_LANES_E, I915_PW_DOMAIN_PORT_DDI_LANES_F,
	I915_PW_DOMAIN_PORT_DDI_LANES_TC1, I915_PW_DOMAIN_PORT_DDI_LANES_TC2,
	I915_PW_DOMAIN_PORT_DDI_LANES_TC3, I915_PW_DOMAIN_PORT_DDI_LANES_TC4,
	I915_PW_DOMAIN_PORT_DDI_LANES_TC5, I915_PW_DOMAIN_PORT_DDI_LANES_TC6,
	I915_PW_DOMAIN_PORT_DDI_IO_A, I915_PW_DOMAIN_PORT_DDI_IO_B,
	I915_PW_DOMAIN_PORT_DDI_IO_C, I915_PW_DOMAIN_PORT_DDI_IO_D,
	I915_PW_DOMAIN_PORT_DDI_IO_E, I915_PW_DOMAIN_PORT_DDI_IO_F,
	I915_PW_DOMAIN_PORT_DDI_IO_TC1, I915_PW_DOMAIN_PORT_DDI_IO_TC2,
	I915_PW_DOMAIN_PORT_DDI_IO_TC3, I915_PW_DOMAIN_PORT_DDI_IO_TC4,
	I915_PW_DOMAIN_PORT_DDI_IO_TC5, I915_PW_DOMAIN_PORT_DDI_IO_TC6,
	I915_PW_DOMAIN_PORT_DSI, I915_PW_DOMAIN_PORT_CRT,
	I915_PW_DOMAIN_PORT_OTHER, I915_PW_DOMAIN_VGA,
	I915_PW_DOMAIN_AUDIO_MMIO, I915_PW_DOMAIN_AUDIO_PLAYBACK,
	I915_PW_DOMAIN_AUX_IO_A, I915_PW_DOMAIN_AUX_IO_B, I915_PW_DOMAIN_AUX_IO_C,
	I915_PW_DOMAIN_AUX_IO_D, I915_PW_DOMAIN_AUX_IO_E, I915_PW_DOMAIN_AUX_IO_F,
	I915_PW_DOMAIN_AUX_A, I915_PW_DOMAIN_AUX_B, I915_PW_DOMAIN_AUX_C,
	I915_PW_DOMAIN_AUX_D, I915_PW_DOMAIN_AUX_E, I915_PW_DOMAIN_AUX_F,
	I915_PW_DOMAIN_AUX_USBC1, I915_PW_DOMAIN_AUX_USBC2, I915_PW_DOMAIN_AUX_USBC3,
	I915_PW_DOMAIN_AUX_USBC4, I915_PW_DOMAIN_AUX_USBC5, I915_PW_DOMAIN_AUX_USBC6,
	I915_PW_DOMAIN_AUX_TBT1, I915_PW_DOMAIN_AUX_TBT2, I915_PW_DOMAIN_AUX_TBT3,
	I915_PW_DOMAIN_AUX_TBT4, I915_PW_DOMAIN_AUX_TBT5, I915_PW_DOMAIN_AUX_TBT6,
	I915_PW_DOMAIN_GMBUS, I915_PW_DOMAIN_GT_IRQ,
	I915_PW_DOMAIN_DC_OFF, I915_PW_DOMAIN_TC_COLD_OFF,
	I915_PW_DOMAIN_INIT,
	I915_PW_DOMAIN_NUM
};

/* DC state flags -- reference register-bit encodings (intel_dmc_regs / power). */
#define I915_DC_STATE_EN_UPTO_DC5   0x00000001u
#define I915_DC_STATE_EN_UPTO_DC6   0x00000002u
#define I915_DC_STATE_EN_DC9        0x00000008u
#define I915_DC_STATE_EN_DC3CO      0x40000000u

/* i915_power_well_id values (DISP_PW_ID_NONE must be 0). */
#define I915_DISP_PW_ID_NONE   0
#define I915_SKL_DISP_PW_1     8
#define I915_SKL_DISP_PW_2     9
#define I915_ICL_DISP_PW_3     10
#define I915_SKL_DISP_DC_OFF   11

/* Which ops family a well uses (bodies executed at init_hw). */
enum i915_pw_ops_kind {
	I915_PW_OPS_ALWAYS_ON = 0,
	I915_PW_OPS_HSW,        /* hsw_power_well_ops */
	I915_PW_OPS_ICL_DDI,    /* icl_ddi_power_well_ops */
	I915_PW_OPS_ICL_AUX,    /* icl_aux_power_well_ops */
	I915_PW_OPS_DC_OFF,     /* gen9_dc_off_power_well_ops */
};

/* 128-bit power-domain membership mask (I915_PW_DOMAIN_NUM <= 128). */
struct i915_pw_domain_mask {
	uint64_t bits[2];
};

/* A runtime power well (i915_power_well: desc data + separate live state). */
struct i915_power_well {
	const char *name;
	enum i915_pw_ops_kind ops;
	int always_on;
	int has_vga;
	int has_fuses;
	int is_tc_tbt;
	int fixed_enable_delay;
	unsigned enable_timeout;     /* ms; 0 = ops default */
	unsigned irq_pipe_mask;
	unsigned hsw_idx;            /* HSW/ICL power-well control index (reference) */
	int id;                     /* i915_power_well_id (0 = DISP_PW_ID_NONE) */
	struct i915_pw_domain_mask domains;
	int domains_all;            /* zero-length domain list = ALL domains */
	/* Live state, tracked separately from the descriptor: */
	unsigned refcount;          /* SW get/put reference count */
	int hw_enabled;             /* last known HW state (-1 = unknown until sync) */
};

#define I915_PW_MAX  40

/* i915_power_domains: the whole power-domain state for the device. */
struct i915_power_domains {
	uint32_t allowed_dc_mask;
	uint32_t target_dc_state;
	int disable_power_well;      /* sanitized option */
	struct mutex lock;
	int async_put_work_inited;   /* INIT_DELAYED_WORK(async_put_work) */

	struct i915_power_well power_wells[I915_PW_MAX];
	unsigned num_power_wells;
	/* per-domain -> bitmask of well indices that provide it. */
	uint64_t domain_wells[I915_PW_DOMAIN_NUM];

	int map_initialized;
	int initialized;

	/* domain_use_count[]: references per DOMAIN (the wells count separately) */
	unsigned domain_use_count[I915_PW_DOMAIN_NUM];

	/*
	 * intel_display_power_put_async(): a last reference that is put asynchronously is
	 * parked in async_put_domains[0] and released by the delayed work; while that work
	 * is outstanding further ones collect in [1] and the work re-queues itself for
	 * them.  A get of a parked domain takes the reference back without touching the
	 * hardware.  async_put_wakeref marks "the work owns parked references".
	 */
	struct i915_pw_domain_mask async_put_domains[2];
	int async_put_wakeref;
	int async_put_next_delay;
	const struct i915_pw_async_ops *async_ops;   /* NULL: put_async puts at once */
	void *async_ctx;
	struct i915_pw_ctx *async_pwc;              /* the context the delayed work uses */
	unsigned async_puts, async_parked, async_grabs, async_work_runs, async_work_empty;
	unsigned async_released, async_requeues, async_flushes, async_state_errors, use_count_errors;
};

/* the delayed work behind put_async; supplied by the embedder (real timer + worker, or a test) */
struct i915_pw_async_ops {
	int (*queue)(void *ctx, int delay_ms);    /* queue_delayed_work(): 1 newly queued */
	int (*cancel)(void *ctx, int sync);       /* cancel_delayed_work[_sync]() */
};

struct i915_mmio;
struct i915_vga_client;
struct i915_cdclk_dev;

/*
 * gen8_irq_power_well_post_enable() / gen8_irq_power_well_pre_disable(): implemented by the IRQ layer (interrupts.c) and
 * bound here, so this file does not depend on the IRQ device.  Both check intel_irqs_enabled() themselves.
 */
struct i915_pw_irq_ops {
	void (*post_enable)(void *ctx, unsigned pipe_mask);
	int (*pre_disable)(void *ctx, unsigned pipe_mask);     /* 0, or the drain failed: the well must NOT go off */
};

/*
 * Context for the power-well operation bodies: the MMIO handle, the VGA client
 * (has_vga post-enable) and the IRQ-enabled gate (intel_irqs_enabled(); 0 before
 * P4).  The *_calls counters are diagnostics the GPU-free tests observe.
 */
struct i915_pw_ctx {
	struct i915_mmio *mmio;
	struct i915_vga_client *vga;
	int irqs_enabled;
	unsigned vga_reset_calls;
	unsigned irq_post_enable_calls;
	unsigned irq_pre_disable_calls;
	/*
	 * Set when a pipe's interrupt drain failed before its well would have gone off.  From then on every well disable
	 * is refused (the handler that did not finish may still touch display registers), the well that was about to go
	 * off stays owned, and the probe's teardown keeps the IRQ handler attached and the device resources in place.
	 */
	int irq_sync_failed;
	unsigned disable_refusals;             /* well disables refused because of it */
	unsigned kept_wells;                   /* wells kept on by a refused disable */
	const struct i915_pw_irq_ops *irq_ops;   /* NULL: the IRQ layer is not bound (GPU-free tests) */
	void *irq_ctx;
	unsigned ack_timeouts;   /* real HW ACK timeouts (warn+continue) */
	/*
	 * DC_off power-well enable (gen9_disable_dc_states) needs the shared CDCLK,
	 * the saved DBUF slice mask and the target DC state -- these are the SAME
	 * device objects the parent uses (not per-well copies).
	 */
	struct i915_cdclk_dev *cd;
	uint8_t *dbuf_slices;          /* saved DBUF mask, for gen9_assert_dbuf_enabled */
	uint32_t target_dc_state;
	uint32_t allowed_dc_mask;
	unsigned dc_off_enable_calls;  /* diagnostics observed by the GPU-free tests */
	unsigned dc_off_cdclk_readouts;
	unsigned dc_off_dbuf_asserts;
	unsigned dc_off_combo_inits;
	/*
	 * DC_off power-well DISABLE (gen9_dc_off_power_well_disable): enables the
	 * target DC state, but only once the DMC firmware payload is loaded.
	 */
	int display_ver;               /* gen9_dc_mask() */
	int dmc_has_payload;           /* intel_dmc_has_payload() */
	uint32_t dc_state;             /* display.dmc.dc_state: what we last wrote */
	unsigned dc_off_disable_calls;
	unsigned dc_state_writes;
	unsigned dc_state_rewrites;
};

/* pmdemand state: mutex + waitqueue, initialised early. */
struct i915_pmdemand {
	struct mutex lock;
	struct wait_queue waitqueue;
	int early_initialized;
};
/*
 * The CDCLK (clock.c).
 */
/*
 * CDCLK init (see clock.c).
 *
 * Faithful port of the ADL-P CDCLK bring-up: intel_init_cdclk_hooks() selects
 * the platform table + callbacks (ADL-P D0 -> adlp_cdclk_table + tgl funcs);
 * intel_cdclk_init_hw() -> bxt_cdclk_init_hw() reads the pre-OS state
 * (bxt_get_cdclk via the DE PLL / CDCLK_CTL), sanitises it (bxt_sanitize_cdclk),
 * and only if the state is not a legal freq/VCO for this platform reprograms it
 * (bxt_set_cdclk, using the common PCODE mailbox).  This is intel_cdclk_init_hw,
 * NOT intel_cdclk_init (which reprograms to the min cdclk later in P3).
 */

struct mutex;

/* Display stepping ordinals (subset of enum intel_step; ordering is faithful). */
enum i915_display_step {
	I915_STEP_NONE = 0,
	I915_STEP_A0, I915_STEP_A1, I915_STEP_A2, I915_STEP_A3,
	I915_STEP_B0, I915_STEP_B1, I915_STEP_B2, I915_STEP_B3,
	I915_STEP_C0, I915_STEP_C1, I915_STEP_C2, I915_STEP_C3,
	I915_STEP_D0
};

/* intel_cdclk_vals: one row of a platform CDCLK table. */
struct i915_cdclk_vals {
	uint32_t refclk;   /* kHz */
	uint32_t cdclk;    /* kHz */
	uint8_t  divider;  /* CD2X divider (2/3/... informational) */
	uint8_t  ratio;    /* PLL ratio */
	uint16_t waveform; /* squash waveform (0 on ADL-P: no squash) */
};

/*
 * intel_cdclk_config: dev_priv->display.cdclk.hw plus computed targets.
 * vco special values: 0 = PLL off (bypass), ~0u = unknown (sanitize wants a
 * full PLL disable+enable).  Never treat ~0u as a real frequency.
 */
struct i915_cdclk_config {
	uint32_t ref;      /* refclk kHz */
	uint32_t vco;      /* PLL VCO kHz; 0 = off; ~0u = unknown/force */
	uint32_t cdclk;    /* cdclk kHz; 0 = force reprogram */
	uint32_t bypass;   /* bypass kHz */
	uint8_t  voltage_level;
};

/*
 * Which set of CDCLK hooks the platform uses.
 */
enum i915_cdclk_funcs { I915_CDCLK_FUNCS_NONE = 0, I915_CDCLK_FUNCS_TGL };

/*
 * The CDCLK of the display: the platform table and hooks, the state the
 * hardware is in, and the PCODE lock the changes take.
 */
struct i915_cdclk_dev {
	struct i915_cdclk_config hw;          /* current HW state (display.cdclk.hw) */
	const struct i915_cdclk_vals *table;  /* selected platform table */
	int funcs;                               /* enum i915_cdclk_funcs */
	int display_ver;                         /* 13 for ADL-P */
	int has_cdclk_crawl;                     /* ADL-P: 1 */
	int has_cdclk_squash;                    /* ADL-P: 0 */
	struct i915_mmio *m;                    /* MMIO backend */
	struct mutex *sb_lock;                   /* PCODE sideband lock */

	/* Diagnostics captured across init_hw (observed_before ... state_after). */
	struct i915_cdclk_config diag_observed_before;
	struct i915_cdclk_config diag_sanitized;
	struct i915_cdclk_config diag_requested;
	int diag_no_change;            /* 1 = sanitize accepted HW; nothing reprogrammed */
	int diag_prepare_status;       /* PCODE prepare-for-change result (0 / -errno) */
	int diag_hw_sequence_reached;  /* 1 once the PLL/CDCLK_CTL writes ran */
	int diag_notify_status;        /* PCODE voltage-notify result */
};

/*
 * The display core (power.c).
 */
/*
 * display-core HW bring-up (see power.c).
 *
 * Ports intel_power_domains_init_hw(false) -> icl_display_core_init(false) for
 * ADL-P: DC-state disable, PCH reset handshake, combo PHY init (with the void
 * contract's fault check), PW1 enable (fuses), CDCLK init, DBUF slice enable,
 * BW_BUDDY, the xe_lpd workarounds, then hold the POWER_DOMAIN_INIT reference
 * and sync every power well.  Children share ONE device state (same MMIO, the
 * same power-domains + cdclk + PCODE sb_lock); nothing is re-initialised per
 * child.  intel_power_domains_driver_remove() cancels the init wakeref's rpm
 * side while KEEPING the wells enabled (not a domain put).
 */

struct i915_power_domains;
struct i915_pw_ctx;

/* Shared device state threaded through the display-core bring-up. */
struct i915_display_core {
	struct i915_power_domains *pd;   /* the one power-domains state */
	struct i915_cdclk_dev *cd;       /* the one cdclk state (cd->m == m) */
	struct i915_pw_ctx *pwc;         /* power-well op context (pwc->mmio == m) */
	struct i915_mmio *m;              /* the one MMIO backend */
	struct mutex *sb_lock;             /* the one PCODE sideband lock */

	/* DRAM info carried from P2 (BW_BUDDY table lookup). */
	int dram_type;                     /* enum i915_dram_type */
	unsigned dram_channels;

	/* Live state (not re-initialised per child). */
	int initializing;
	uint8_t dbuf_enabled_slices;
	/* how many DBUF slices this display has (display version 13: 4, version 12: 2) */
	uint8_t dbuf_slice_mask;
	/*
	 * which MBUS ABOX registers this display has (intel_display_device.c .abox_mask);
	 * 0 = the platform programs none (Alder Lake-P and display 14+)
	 */
	uint8_t abox_mask;
	int display_ver;                /* 12 = Tiger Lake, 13 = Alder Lake-P class */
	/*
	 * the reference gates a few display steps on the platform, not on the version
	 * (gen12_dbuf_slices_config() returns immediately on Alder Lake-P alone)
	 */
	uint8_t is_alderlake_p;
	int init_wakeref_held;             /* POWER_DOMAIN_INIT domain reference held */
	int pm_wakeref;                    /* runtime-PM side of the init wakeref */

	/* Diagnostics. */
	int fault_stop;                    /* 1 once an adaptation-layer fault stopped us */
	const char *fault_where;           /* child at which we stopped */
	int reached_init_ref;
	int reached_sync_hw;
	int last_child;                    /* ordinal of the last child entered */
};

/*
 * The DMC firmware (dmc.c).
 */
/*
 * DMC firmware parse (F1) + load (F2) + async lifecycle (F3).
 *
 * Faithful port of intel_dmc.c for ADL-P (display version 13).  This header
 * covers the parse: the firmware blob (from the read-only provider) is validated
 * (CSS / package / per-DMC headers), the entry matching the display stepping is
 * selected for each DMC id, and its payload is copied into DEVICE-OWNED storage
 * so it outlives the released firmware handle.  Length units differ per header
 * (CSS/package/v3 in dwords, v1 in bytes) and the entry offset is relative to the
 * end of the CSS+package, exactly as the reference.
 */

enum i915_dmc_id {
	I915_DMC_FW_MAIN = 0,
	I915_DMC_FW_PIPEA,
	I915_DMC_FW_PIPEB,
	I915_DMC_FW_PIPEC,
	I915_DMC_FW_PIPED,
	I915_DMC_FW_MAX
};

#define I915_DMC_MAX_MMIO 20   /* DMC_V3_MAX_MMIO_COUNT */

struct i915_dmc_info {
	int present;               /* an fw_info entry was selected for this id */
	uint32_t dmc_offset;       /* dwords (fw_info.offset), relative to CSS+package end */
	uint32_t mmio_count;
	uint32_t mmioaddr[I915_DMC_MAX_MMIO];
	uint32_t mmiodata[I915_DMC_MAX_MMIO];
	uint32_t start_mmioaddr;   /* program RAM start MMIO (v3) / DMC_V1_MMIO_START (v1) */
	uint32_t dmc_fw_size;      /* dwords (program size, excl. header) */
	const uint8_t *payload;    /* device-owned copy; NULL until saved */
	uint32_t payload_size;     /* bytes */
	int header_ver;            /* 1 or 3 */
};

/*
 * One parsed DMC firmware image: its header, the payload of each DMC id
 * and the registers the load writes.
 */
struct i915_dmc {
	uint32_t version;          /* CSS version (major<<16 | minor) */
	uint32_t max_fw_size;      /* per-platform ceiling (bytes) */
	int display_ver;           /* 13 for ADL-P */
	char stepping;             /* display stepping char, e.g. 'D' */
	char substepping;          /* e.g. '0' */
	struct i915_dmc_info dmc_info[I915_DMC_FW_MAX];
	/* Diagnostics captured by the parse (for the GPU-free tests). */
	uint32_t css_header_len_bytes;
	uint32_t package_header_ver;
	uint32_t num_entries;
	int truncated;             /* set on a refused (truncated/invalid) blob */
	/* F2 load diagnostics. */
	unsigned payload_writes;   /* DMC_PROGRAM DWORD writes performed */
	unsigned aux_writes;       /* trailing per-DMC MMIO writes */
	unsigned evt_disable_writes; /* event-handler CTL+HTP disable writes */
	int load_seq_completed;    /* 1 only after the WHOLE sequence (incl. post) ran */
	uint64_t psum;             /* order-sensitive checksum of payload (addr,val) */
	uint64_t asum;             /* order-sensitive checksum of aux (addr,transformed val) */
};

/*
 * DMC async lifecycle (F3).  intel_dmc_init takes the DMC's OWN POWER_DOMAIN_INIT
 * reference (distinct from the display-core parent's), prepares the state and
 * queues the load worker; the worker acquires the firmware, parses, loads and,
 * on success, releases the DMC reference; fini flushes the worker (flush, not
 * cancel), releases any still-held DMC reference, and frees the payload arena.
 */
struct i915_dmc_dev {
	struct i915_dmc dmc;
	struct i915_mmio *m;
	struct i915_power_domains *pd;
	struct i915_pw_ctx *pwc;
	struct i915_workqueue *wq;
	const char *fw_path;
	/* dmc_fallback_path() is a platform question, not a path question */
	int is_alderlake_p;
	struct i915_work work;
	int dmc_wakeref_held;          /* DMC's own INIT reference is held */
	uint32_t dc_state;
	/* diagnostics (kept distinct, per the reference). */
	int work_submitted;
	int worker_started;
	int firmware_acquired;
	int fallback_requested;
	int main_payload_present;      /* NOT the same as load_seq_completed */
	int load_seq_completed_flag;
	int first_fault;
};

/*
 * The DRAM and display bandwidth (watermark.c).
 */
/*
 * DRAM info + display bandwidth (hw_probe tail).
 *
 * Ports intel_dram_detect() and intel_bw_init_hw() -> tgl_get_bw_info(&adlp_sa_info)
 * for ADL-P, driven by the common PCODE mailbox.  The results are written into a
 * device-owned bandwidth state (i915_bw_state) that later stages read, mirroring
 * the reference i915->display.bw.max[] / sagv.status.  Both are void in the
 * reference: a tolerated PCODE/data failure is logged and the caller continues.
 */

#define I915_NUM_QGV_POINTS 8   /* I915_NUM_QGV_POINTS */
#define I915_NUM_PSF_POINTS 3   /* I915_NUM_PSF_GV_POINTS */
#define I915_BW_GROUPS      6   /* ARRAY_SIZE(display.bw.max) */

/* INTEL_DRAM_* subset reachable on ADL-P's gen12 decode. */
enum i915_dram_type {
	I915_DRAM_DDR4 = 0,
	I915_DRAM_DDR5,
	I915_DRAM_LPDDR5,
	I915_DRAM_LPDDR4,
	I915_DRAM_DDR3,
	I915_DRAM_LPDDR3,
	I915_DRAM_UNKNOWN
};

/* SAGV status (display.sagv.status subset). */
enum i915_sagv_status {
	I915_SAGV_UNKNOWN = 0,
	I915_SAGV_DISABLED,
	I915_SAGV_ENABLED,
	I915_SAGV_NOT_CONTROLLED
};

/* Subset of dram_info the bandwidth branch consumes. */
struct i915_dram_info {
	int type;                   /* enum i915_dram_type */
	unsigned num_channels;
	unsigned num_qgv_points;
	unsigned num_psf_gv_points;
	int wm_lv_0_adjust_needed;
	int valid;                  /* global info decoded successfully */
};

/* Mirrors struct intel_bw_info (display.bw.max[i]). */
struct i915_bw_group {
	unsigned deratedbw[I915_NUM_QGV_POINTS];
	unsigned peakbw[I915_NUM_QGV_POINTS];
	unsigned psf_bw[I915_NUM_PSF_POINTS];
	unsigned num_qgv_points;
	unsigned num_psf_gv_points;
	unsigned num_planes;
};

/* Device-owned display bandwidth state (display.bw / sagv.status). */
struct i915_bw_state {
	struct i915_bw_group max[I915_BW_GROUPS];
	int sagv_status;
	int valid;                  /* bandwidth table computed */
};
/*
 * The display software state (state.c, watermark.c).
 */
/*
 * P3 tail: intel_mode_config_init() .. intel_fbc_init().
 *
 * Ports the remainder of intel_display_driver_probe_noirq() that follows the
 * (asynchronous) DMC load and the modeset/flip workqueues:
 *
 *   intel_mode_config_init(i915);          -- void
 *   intel_cdclk_init(i915);                -- global state object
 *   intel_color_init(i915);                -- DISPLAY_VER == 10 only
 *   intel_dbuf_init(i915);                 -- global state object
 *   intel_bw_init(i915);                   -- global state object + forced SAGV disable
 *   intel_pmdemand_init(i915);             -- global state object
 *   intel_init_quirks(i915);               -- void
 *   intel_fbc_init(i915);                  -- void
 *
 * Four of them register an intel_global_obj on display.global.obj_list, so the
 * shared mechanism (intel_atomic_global_obj_init) is ported once here and the
 * insertion ORDER is observable: cdclk, dbuf, bw, pmdemand.
 *
 * Note the two distinct "bw" states the reference keeps, which the port also
 * keeps apart:
 *   - display.bw.max[] / sagv.status  -- the PCODE-probed HW table from P2,
 *     already owned by struct i915_bw_state (internal.h).
 *   - struct intel_bw_state           -- the ATOMIC global state object created
 *     here, holding qgv_points_mask (i915_bw_obj_state below).
 *
 * The reference kzalloc()s each state object and propagates -ENOMEM; the port
 * uses device-owned storage behind an explicit allocator so those failure paths
 * stay reachable and testable.
 */

struct i915_bw_state;

/*
 * ---------------------------------------------------------------- *
 * intel_global_state.h: intel_global_obj / intel_global_state
 * ----------------------------------------------------------------
 */

struct i915_global_obj;

/*
 * One global atomic state object's state, as the Linux intel_global_state
 * carries it.
 */
struct i915_global_state {
	struct i915_global_obj *obj;
	unsigned ref;                 /* kref_init() -> 1 */
	int changed;
};

/*
 * The duplicate and destroy hooks of a global atomic state object.
 */
struct i915_global_state_funcs {
	struct i915_global_state *(*duplicate_state)(struct i915_global_obj *obj);
	void (*destroy_state)(struct i915_global_obj *obj,
		struct i915_global_state *state);
};

/*
 * A global atomic state object (the Linux intel_global_obj): its hooks and
 * its current state.
 */
struct i915_global_obj {
	struct i915_global_obj *next;      /* list_head, appended at the TAIL */
	struct i915_global_state *state;
	const struct i915_global_state_funcs *funcs;
	/*
	 * Diagnostic only; the reference's intel_global_obj has no name.  Set by
	 * the caller AFTER intel_atomic_global_obj_init (which memsets the obj).
	 */
	const char *name;
};

/* Per-user global state objects (the reference's *_state->base members). */
struct i915_cdclk_obj_state {
	struct i915_global_state base;
};
/*
 * The DBUF global state: which slices are enabled.
 */
struct i915_dbuf_obj_state {
	struct i915_global_state base;
};
/*
 * The bandwidth global state: the QGV points the display may use.
 */
struct i915_bw_obj_state {
	struct i915_global_state base;
	uint16_t qgv_points_mask;
};
/*
 * The PM demand global state.
 */
struct i915_pmdemand_obj_state {
	struct i915_global_state base;
};

/*
 * The cursor/max-size ladders in intel_mode_config_init() branch on platforms
 * that predate everything this port targets.  They are kept as real branches
 * (so the ladder itself is exercised) and selected by this enum; ADL-P is
 * I915_PLAT_NONE and takes the modern "else" arms.
 */
enum i915_legacy_platform {
	I915_PLAT_NONE = 0,   /* ADL-P and every other modern part */
	I915_PLAT_I845G,
	I915_PLAT_I865G,
	I915_PLAT_I830,
	I915_PLAT_I85X,
	I915_PLAT_I915G,
	I915_PLAT_I915GM,
	I915_PLAT_BROADWELL,
	I915_PLAT_SKYLAKE,
	I915_PLAT_BROXTON
};

/*
 * The mode configuration limits of the display (the Linux drm_mode_config
 * the display fills in).
 */
struct i915_mode_config {
	unsigned min_width, min_height;
	unsigned max_width, max_height;
	unsigned cursor_width, cursor_height;
	unsigned preferred_depth;
	int prefer_shadow;
	int async_page_flip;
	int funcs_set;             /* mode_config->funcs = &intel_mode_funcs */
	int helper_private_set;    /* ->helper_private = &intel_mode_config_funcs */
	int drm_mode_config_inited;
};

/*
 * ---------------------------------------------------------------- *
 * intel_fbc_init()
 * ----------------------------------------------------------------
 */

#define I915_MAX_FBCS 4      /* I915_MAX_FBCS */

enum i915_fbc_id {
	I915_FBC_A = 0,
	I915_FBC_B,
	I915_FBC_C,
	I915_FBC_D
};

/* Which intel_fbc_funcs vtable intel_fbc_create() selected. */
enum i915_fbc_funcs_kind {
	I915_FBC_FUNCS_NONE = 0,
	I915_FBC_FUNCS_I8XX,
	I915_FBC_FUNCS_I965,
	I915_FBC_FUNCS_G4X,
	I915_FBC_FUNCS_ILK,
	I915_FBC_FUNCS_SNB,
	I915_FBC_FUNCS_IVB
};

/*
 * One frame-buffer compression instance and its lock.
 */
struct i915_fbc {
	int id;                            /* enum i915_fbc_id */
	int funcs_kind;                    /* enum i915_fbc_funcs_kind */
	struct i915_work underrun_work; /* INIT_WORK(&fbc->underrun_work, ...) */
	struct mutex lock;                 /* mutex_init(&fbc->lock) */
	int lock_inited;
	int in_use;
};

/* enum intel_quirk_id, values preserved (used as BIT(quirk)). */
enum i915_quirk_id {
	I915_QUIRK_BACKLIGHT_PRESENT = 0,
	I915_QUIRK_INCREASE_DDI_DISABLED_TIME,
	I915_QUIRK_INCREASE_T12_DELAY,
	I915_QUIRK_INVERT_BRIGHTNESS,
	I915_QUIRK_LVDS_SSC_DISABLE,
	I915_QUIRK_NO_PPS_BACKLIGHT_POWER_HOOK,
	I915_QUIRK_FW_SYNC_LEN
};

/*
 * ---------------------------------------------------------------- *
 * Device-owned P3-tail display state
 * ----------------------------------------------------------------
 */

struct i915_display_state {
	/* display.global.obj_list (INIT_LIST_HEAD in intel_mode_config_init) */
	struct i915_global_obj *obj_list_head;
	struct i915_global_obj *obj_list_tail;
	unsigned obj_count;
	int obj_list_inited;

	struct i915_mode_config mode_config;

	/* The four global state objects + their device-owned storage. */
	struct i915_global_obj cdclk_obj, dbuf_obj, bw_obj, pmdemand_obj;
	struct i915_cdclk_obj_state cdclk_state;
	struct i915_dbuf_obj_state dbuf_state;
	struct i915_bw_obj_state bw_obj_state;
	struct i915_pmdemand_obj_state pmdemand_state;

	/* intel_color_init */
	int color_done;                 /* ran and returned 0 */

	/* intel_bw_init / icl_force_disable_sagv */
	int sagv_force_disable_attempted;
	int sagv_pcode_ret;             /* icl_pcode_restrict_qgv_points() result */
	unsigned sagv_qgv_points;       /* icl_max_bw_qgv_point_mask() */
	unsigned sagv_psf_points;       /* icl_max_bw_psf_gv_point_mask() */

	/* intel_pmdemand_init */
	int pmdemand_wa_14016740474;    /* ver-14 A0..C0 WA applied (never on ADL-P) */

	/* intel_init_quirks */
	unsigned quirk_mask;            /* display.quirks.mask */
	unsigned quirk_hooks_fired;
	int dmi_scanned;                /* the DMI list was walked */
	int dmi_available;              /* a DMI backend answered */

	/* intel_fbc_init */
	int enable_fbc_param;           /* display.params.enable_fbc IN (-1 = auto) */
	int enable_fbc_sanitized;
	unsigned fbc_mask;              /* DISPLAY_RUNTIME_INFO()->fbc_mask, post-WA */
	int fbc_vtd_wa;                 /* need_fbc_vtd_wa() fired */
	struct i915_fbc *fbc[I915_MAX_FBCS];
	struct i915_fbc fbc_store[I915_MAX_FBCS];
	unsigned fbc_created;

	/* diagnostics */
	unsigned state_allocs;          /* successful global-state allocations */
	const char *fail_where;
	int inited;
};

/*
 * The PCH detection (device-info.c).
 */
/*
 * enum intel_pch, values preserved (the enum is ordered by south-display
 * compatibility and is compared with >=).
 */
enum i915_pch {
	I915_PCH_NOP = -1,      /* PCH without south display */
	I915_PCH_NONE = 0,      /* No PCH present */
	I915_PCH_IBX,           /* 1 */
	I915_PCH_CPT,           /* 2 */
	I915_PCH_LPT,           /* 3 */
	I915_PCH_SPT,           /* 4 */
	I915_PCH_CNP,           /* 5 */
	I915_PCH_ICP,           /* 6 */
	I915_PCH_TGP,           /* 7 */
	I915_PCH_ADP,           /* 8 -- Alder Lake */
	/* Fake PCHs, functionality handled on the same PCI dev. */
	I915_PCH_DG1 = 1024,
	I915_PCH_DG2,
	I915_PCH_MTL,
	I915_PCH_LNL
};

/* How the type was reached (diagnostic; the reference only logs this). */
enum i915_pch_source {
	I915_PCH_SRC_NONE = 0,
	I915_PCH_SRC_REAL,      /* a real ISA bridge matched the id table */
	I915_PCH_SRC_VIRT,      /* an emulated bridge -> platform guess */
	I915_PCH_SRC_NO_BRIDGE  /* no ISA bridge at all -> guest guess */
};

/*
 * The PCH the display found: its type, id and how it was found.
 */
struct i915_pch_state {
	int type;                 /* enum i915_pch */
	uint16_t id;              /* masked PCH device id */
	int source;               /* enum i915_pch_source */
	unsigned bridges_scanned;
	uint16_t bridge_device;   /* raw device id of the bridge that decided it */
	uint16_t bridge_svid, bridge_sdid;
};

struct drv_pci_device;

/*
 * Test seam: the ISA-bridge walk.  Production uses the real PCI scan; the
 * GPU-free tests install a scripted list so the virtual/real/absent paths are
 * all reachable without hardware.  Returns 1 and fills the ids while entries
 * remain, 0 when the walk is finished.
 */
struct i915_pch_bridge_ops {
	int (*next)(void *ctx, unsigned index, uint16_t *vendor, uint16_t *device,
		uint16_t *svid, uint16_t *sdid);
	void *ctx;
};

/*
 * The output setup, readout and sanitize (takeover.c).
 */
/*
 * P5: intel_display_driver_probe_nogem().
 *
 * P5-a covers the front section, up to (but not including) intel_setup_outputs():
 *
 *   intel_wm_init            -> skl_wm_init: intel_sagv_init + skl_setup_wm_latency
 *   intel_panel_sanitize_ssc -> LVDS/SSC; nothing to do on ADL-P
 *   intel_pps_setup          -> pps.mmio_base
 *   intel_gmbus_setup        -> gmbus.mmio_base, the ICP pin table, GMBUS reset
 *   intel_crtc_init x4       -> per-pipe CRTC + plane records
 *   intel_plane_possible_crtcs_init / intel_shared_dpll_init /
 *   intel_fdi_pll_freq_update / intel_update_czclk
 *   intel_display_driver_init_hw -> intel_update_cdclk + adlp_display_wa_apply
 *   intel_dpll_update_ref_clks / intel_hdcp_component_init /
 *   intel_update_max_cdclk / intel_hti_init / intel_vga_disable
 *
 * The DRM object model (drm_crtc / drm_plane / drm_encoder / drm_connector) is
 * NOT brought in; CRTCs, planes and shared DPLLs are the minimal records
 * the readout and sanitize phases (P5-c / P5-d) actually consume.
 *
 * Confirmed from the reference device tables rather than assumed, for ADL-P
 * (xe_lpd, DISPLAY_VER 13, PCH_ADP):
 *   - num_scalers[pipe] = 2, num_sprites[pipe] = 4  -> 6 planes/pipe
 *     (1 primary + 4 sprites + 1 cursor)
 *   - has_hti is NOT set on xe_lpd (it is RKL/ADL-S only) -> intel_hti_init is
 *     a no-op here; HDPORT_STATE is not read.
 *   - HAS_HW_SAGV_WM (ver >= 13 && !DGFX) is true -> wm.num_levels = 6, not 8.
 *   - intel_ddi_crt_present() returns false for DISPLAY_VER >= 9.
 */

struct i915_vbt_state;

#define I915_NOGEM_MAX_PIPES    4
#define I915_NOGEM_MAX_PLANES   6    /* primary + 4 sprites + cursor */
#define I915_NOGEM_MAX_DPLLS    8    /* adlp_plls has 7 + terminator */
#define I915_NOGEM_MAX_WM_LVL   8    /* I915_MAX_WM / skl_latency[] */
#define I915_NOGEM_MAX_GMBUS    15   /* pin indices 1..14 */
#define I915_NOGEM_MAX_ENCODERS 8

/* enum port (values preserved; PORT_TC1 aliases PORT_D). */
#define I915_PORT_NONE   (-1)
#define I915_PORT_A      0
#define I915_PORT_B      1
#define I915_PORT_C      2
#define I915_PORT_D      3
#define I915_PORT_TC1    3
#define I915_PORT_TC2    4
#define I915_PORT_TC3    5
#define I915_PORT_TC4    6

/* enum phy (values preserved). */
#define I915_PHY_A 0
#define I915_PHY_B 1
#define I915_PHY_C 2
#define I915_PHY_D 3
#define I915_PHY_E 4
#define I915_PHY_F 5
#define I915_PHY_I 8

/* Why intel_ddi_init() declined to create an encoder (diagnostic). */
enum i915_ddi_skip {
	I915_DDI_OK = 0,
	I915_DDI_SKIP_PORT_NONE,
	I915_DDI_SKIP_STRAP,
	I915_DDI_SKIP_PORT_INVALID,
	I915_DDI_SKIP_PORT_IN_USE,
	I915_DDI_SKIP_DSI,
	I915_DDI_SKIP_HTI,
	I915_DDI_SKIP_NOT_DVI_HDMI_DP,
	I915_DDI_SKIP_EDP_INIT_FAILED     /* intel_ddi_init_dp_connector() failed: the encoder is dropped */
};

/* Which clock vtable intel_ddi_init() selected. */
enum i915_ddi_clk {
	I915_DDI_CLK_NONE = 0,
	I915_DDI_CLK_ICL_COMBO,
	I915_DDI_CLK_ICL_TC
};

/* intel_encoder: the subset the readout and sanitize phases consume. */
struct i915_encoder {
	int port;               /* enum port */
	int phy;                /* enum phy */
	int is_tc;
	int clk_funcs;          /* enum i915_ddi_clk */
	int power_domain;       /* POWER_DOMAIN_PORT_DDI_LANES_* */
	int init_hdmi, init_dp;
	uint8_t dvo_port;
	uint32_t device_type;
	/* readout (P5-c) */
	int crtc_linked;        /* encoder->base.crtc != NULL */
	unsigned pipe_mask;
	int is_mst;
	int in_use;
	int shared_dpll_id;     /* encoder->get_config: the PLL id feeding it (-1 = none / not read) */
	uint32_t dpclka_cfgcr0; /* the value that id was read from */
};

/* intel_plane: the subset the readout/sanitize phases use. */
enum i915_plane_type {
	I915_PLANE_PRIMARY = 0,
	I915_PLANE_SPRITE,
	I915_PLANE_CURSOR
};

/*
 * One plane record of the output setup: its id, pipe and the state the
 * readout found.
 */
struct i915_plane {
	int id;                 /* enum plane_id: PLANE_PRIMARY=0, SPRITE0.., CURSOR */
	int type;               /* enum i915_plane_type */
	unsigned pipe;
	int visible;            /* plane_state->uapi.visible (readout, P5-c) */
	int in_use;
};

/* intel_crtc + the parts of intel_crtc_state P5 fills in. */
struct i915_crtc_state {
	int active;             /* hw.active */
	int enable;             /* hw.enable */
	int cpu_transcoder;     /* -1 = INVALID_TRANSCODER */
	int inherited;
	unsigned active_planes;
	/* readout results (P5-c) */
	uint32_t transconf;
	uint32_t trans_ddi_func_ctl;
	unsigned enabled_transcoders;   /* hsw_enabled_transcoders() bitmask */
	/* intel_get_transcoder_timings() */
	unsigned hdisplay, htotal, vdisplay, vtotal;
	unsigned pipe_src_w, pipe_src_h;
	int power_gated;                /* the pipe/transcoder power was off */
};

/*
 * One crtc record of the output setup: its pipe and the state the readout
 * found.
 */
struct i915_crtc {
	unsigned pipe;
	unsigned num_scalers;
	unsigned plane_ids_mask;
	int active;             /* crtc->active */
	int enabled;            /* crtc->base.enabled */
	struct i915_plane planes[I915_NOGEM_MAX_PLANES];
	unsigned num_planes;
	struct i915_crtc_state state;
	int fifo_underrun_reporting;
	int in_use;
};

/* intel_shared_dpll (adlp_plls). */
enum i915_dpll_funcs {
	I915_DPLL_FUNCS_NONE = 0,
	I915_DPLL_FUNCS_COMBO,
	I915_DPLL_FUNCS_TBT,
	I915_DPLL_FUNCS_DKL
};

/*
 * One shared DPLL record: its id, its hooks and the state the readout
 * found.
 */
struct i915_dpll {
	const char *name;
	int id;                 /* enum intel_dpll_id */
	int funcs;              /* enum i915_dpll_funcs */
	unsigned index;
	uint32_t enable_reg;
	int on;                 /* readout (P5-c) */
	unsigned pipe_mask;
	unsigned active_mask;
	int readout_incomplete; /* an active pipe's PLL could not be read (e.g. TC get_pll not ported): never disable */
};

/* One ICP gmbus pin (the adapter itself needs an i2c core we do not have). */
struct i915_gmbus_pin {
	const char *name;
	unsigned gpio;
	unsigned reg0;          /* pin | GMBUS_RATE_100KHZ */
	int present;
};

/*
 * The records the output setup, the readout and the sanitize build and
 * read: crtcs, planes, DPLLs, encoders and their locks.
 */
struct i915_display_nogem {
	/* intel_wm_init -> skl_wm_init */
	unsigned wm_num_levels;
	uint16_t wm_skl_latency[I915_NOGEM_MAX_WM_LVL];
	int wm_latency_valid;
	int sagv_status;                /* mirrors display.sagv.status */
	uint32_t sagv_block_time_us;

	/* intel_pps_setup / intel_gmbus_setup */
	uint32_t pps_mmio_base;
	uint32_t gmbus_mmio_base;
	struct mutex gmbus_lock;
	struct wait_queue gmbus_waitq;
	struct i915_gmbus_pin gmbus_pins[I915_NOGEM_MAX_GMBUS];
	unsigned gmbus_pins_present;
	int gmbus_adapters_unimplemented;   /* i2c core absent: recorded, not faked */

	/* CRTCs / planes / DPLLs */
	struct i915_crtc crtcs[I915_NOGEM_MAX_PIPES];
	unsigned num_crtcs;
	struct i915_dpll dplls[I915_NOGEM_MAX_DPLLS];
	unsigned num_dplls;
	int dpll_mgr_present;
	struct mutex dpll_lock;
	uint32_t dpll_ref_nssc;

	/* cdclk-derived */
	uint32_t max_cdclk_freq;
	int cdclk_logical_set;          /* cdclk_state->logical = actual = hw */

	/* WAs / misc */
	int adlp_wa_applied;
	/* the xe_d arm of intel_display_wa_apply() ran (Tiger Lake and friends) */
	int xe_d_wa_applied;
	int hti_state_read;             /* has_hti is 0 on xe_lpd -> stays 0 */
	uint32_t hti_state;
	int fdi_pll_freq_updated;       /* ADL-P: the function returns early */
	int czclk_updated;              /* ADL-P: the function returns early */
	int hdcp_component_unimplemented;

	/* intel_vga_disable */
	int vga_already_disabled;
	int vga_disable_done;

	/* intel_setup_outputs (P5-b) */
	struct i915_encoder encoders[I915_NOGEM_MAX_ENCODERS];
	unsigned num_encoders;
	unsigned ddi_init_calls;
	unsigned ddi_skipped;
	int ddi_skip_reason[I915_NOGEM_MAX_ENCODERS];
	int ddi_skip_port[I915_NOGEM_MAX_ENCODERS];
	unsigned num_ddi_skips;
	int crt_present;
	int outputs_done;
	/*
	 * intel_ddi_init() -> intel_ddi_init_dp_connector() -> intel_dp_init_connector() ->
	 * intel_edp_init_connector(), for a DP-capable encoder: 1 = not an eDP port (nothing
	 * done), 0 = eDP initialised and KEPT by the hook's owner, < 0 = failed (the reference
	 * then frees the encoder).  NULL = no connector construction (the pre-behaviour).
	 */
	int (*dp_connector_init)(void *ctx, int port);
	void *dp_connector_ctx;
	int edp_port;                       /* port whose eDP connector is live, or -1 */
	int edp_init_rc;

	/* intel_modeset_readout_hw_state (P5-c) */
	unsigned active_pipes;          /* cdclk_state/dbuf_state->active_pipes */
	unsigned readout_crtcs;
	unsigned readout_planes_visible;
	unsigned readout_encoders_linked;
	unsigned readout_dplls_on;
	int readout_done;
	/*
	 * Fields the reference reads that this port deliberately does NOT: color
	 * config, DSC, VRR, bigjoiner, scaler detail, output_format, linetime,
	 * pixel_multiplier, framestart_delay.  None of them feeds a sanitize
	 * decision, and inventing them would be worse than recording the gap.
	 */
	int readout_detail_unimplemented;

	/* intel_modeset_setup_hw_state, sanitize half (P5-d) */
	int early_display_was_applied;   /* IS_DISPLAY_VER(10,12) only -> 0 on ADL-P */
	int pch_sanitize_applied;        /* HAS_PCH_IBX only -> 0 */
	int plane_mapping_sanitized;     /* DISPLAY_VER >= 4 returns -> 0 */
	unsigned vblank_resets;
	unsigned dmc_pipes_enabled;
	unsigned vblank_on_count;
	unsigned fbc_deactivated;
	unsigned crtcs_disabled_noatomic;
	unsigned encoder_clocks_gated;
	unsigned dplls_disabled;
	int cmtg_wa_applied;             /* ADL-P A0..B0 + DPLL0 only -> 0 on D0 */
	unsigned wells_disabled;
	int wm_hw_state_read;
	/*
	 * intel_crtc_disable_noatomic() would need the full modeset disable path
	 * (hsw_crtc_disable: encoder post_disable, DDI, DPLL, pipe, DBUF).  It is
	 * only reachable from an ACTIVE pipe with no encoders; when that happens
	 * this flag is raised instead of pretending the pipe was disabled.
	 */
	int crtc_disable_noatomic_unimplemented;
	int sanitize_done;

	/* diagnostics */
	unsigned mmio_writes;
	const char *fail_where;
	int inited;
};

/*
 * The Linux driver probe (display.c, hotplug.c, power.c).
 */
/*
 * P7: the rest of i915_driver_probe() after i915_gem_init().
 *
 * The reference (i915_driver.c, 6.8.12):
 *   intel_pxp_init(i915)                 see pxp.h (P7-0)
 *   intel_display_driver_probe(i915)     P7-a, display/intel_display_driver.c:
 *     intel_initial_commit()             an atomic commit that recomputes the
 *                                        plane state of every ACTIVE crtc; with
 *                                        no active crtc it commits nothing
 *     intel_overlay_setup()              HAS_OVERLAY: gen2-4 only
 *     intel_fbdev_init()                 CONFIG_DRM_FBDEV_EMULATION
 *     intel_hpd_init()                   every pin HPD_ENABLED, then
 *                                        hpd_irq_setup = gen11_hpd_irq_setup:
 *                                        GEN11_DE_HPD_IMR, TC/TBT_HOTPLUG_CTL,
 *                                        and (PCH >= ICP) SHPD_FILTER_CNT,
 *                                        SDEIMR, SHOTPLUG_CTL_DDI/TC
 *     intel_hpd_poll_disable()           poll_enabled = false + poll_init_work
 *     skl_watermark_ipc_init()           HAS_IPC: DISP_ARB_CTL2.DISP_IPC_ENABLE
 *   i915_driver_register(i915)           P7-b:
 *     gem/pmu/vgpu/drm_dev/debugfs/sysfs/perf/gt sysfs/hwmon registrations
 *     intel_display_driver_register()    opregion register, acpi video, audio
 *                                        components, debugfs, fbdev config,
 *                                        drm_kms_helper_poll_init
 *     intel_power_domains_enable()       PUT the POWER_DOMAIN_INIT reference
 *                                        held since intel_power_domains_init_hw:
 *                                        every well with no other reference
 *                                        turns off, DC_off last (DC6 allowed)
 *     intel_runtime_pm_enable()          autosuspend 10 s, drop the probe ref
 *     dsm handler, vga switcheroo
 *   and the remove path mirrors it: intel_runtime_pm_disable (take the ref
 *   back), intel_power_domains_disable (get INIT again), display unregister.
 *
 * ADAPTATIONS (recorded):
 *   - No DRM object model here: intel_initial_commit is executed only in its
 *     no-active-crtc form (an active crtc is reported as unimplemented);
 *     connectors do not exist, so the poll_init work touches no connector
 *     (the DISPLAY_CORE power get/put around it is kept); the userspace-facing
 *     registrations (drm_dev, debugfs, sysfs, pmu, perf, hwmon, audio
 *     components, fbdev, acpi video, dsm, switcheroo) are recorded as N/A.
 *   - The poll_init work runs inline where the reference queues it.
 *   - intel_runtime_pm_enable drops the probe reference but does not arm
 *     autosuspend: intel_runtime_suspend() (what the PM core would run 10 s
 *     later) is not ported, so the device stays in D0.
 *   - intel_power_domains_verify_state() is compiled only under
 *     CONFIG_DRM_I915_DEBUG_RUNTIME_PM in the reference; here its per-well
 *     refcount/HW comparison is kept as a diagnostic.
 */

struct i915_rpm;
struct i915_display_core;
struct i915_display_nogem;

/* enum hpd_pin (intel_display_limits.h) */
#define I915_HPD_NONE      0
#define I915_HPD_PORT_A    4
#define I915_HPD_PORT_B    5
#define I915_HPD_PORT_C    6
#define I915_HPD_PORT_D    7
#define I915_HPD_PORT_E    8
#define I915_HPD_PORT_TC1  9
#define I915_HPD_PORT_TC6  14
#define I915_HPD_NUM_PINS  15

/* hotplug.stats[].state */
#define I915_HPD_ENABLED        0
#define I915_HPD_DISABLED       1
#define I915_HPD_MARK_DISABLED  2

/* Registers (i915_reg.h). */
#define I915_GEN11_DE_HPD_IMR       0x44474u
#define I915_GEN11_TBT_HOTPLUG_CTL  0x44030u
#define I915_GEN11_TC_HOTPLUG_CTL   0x44038u
#define I915_SDEIMR                 0xc4004u
#define I915_SHOTPLUG_CTL_DDI       0xc4030u
#define I915_SHOTPLUG_CTL_TC        0xc4034u
#define I915_SHPD_FILTER_CNT        0xc4038u
#define I915_SHPD_FILTER_CNT_500_ADJ 0x001d9u
#define I915_SHPD_FILTER_CNT_250     0x000f8u
#define I915_DISP_ARB_CTL2          0x45004u
#define I915_DISP_IPC_ENABLE        (1u << 3)

/*
 * The hotplug state of the Linux driver probe: the poll work and the pins
 * it watches.
 */
struct i915_hotplug {
	/* intel_hpd_init_pins(): hpd_gen11 / hpd_icp */
	uint32_t hpd[I915_HPD_NUM_PINS];
	uint32_t pch_hpd[I915_HPD_NUM_PINS];
	int pins_inited;

	/* encoder->hpd_pin of every encoder intel_setup_outputs() created */
	int encoder_pin[8];
	unsigned n_encoders;

	int state[I915_HPD_NUM_PINS];
	unsigned count[I915_HPD_NUM_PINS];
	int poll_enabled;          /* display.hotplug.poll_enabled */
	int kms_poll_inited;       /* drm_kms_helper_poll_init(): mode_config.poll_enabled */

	/* what the last hpd_irq_setup computed and left in the registers */
	unsigned irq_setups;
	int irq_setup_skipped;     /* !display_irqs_enabled */
	uint32_t de_enabled_irqs, de_hotplug_irqs, pch_enabled_irqs, pch_hotplug_irqs;
	uint32_t de_hpd_imr, tc_ctl, tbt_ctl, shpd_filter, sdeimr, shotplug_ddi, shotplug_tc;
	int sdeimr_skipped;        /* ibx_display_interrupt_update: !intel_irqs_enabled */
	unsigned poll_init_works;
	unsigned poll_core_gets;
};

/*
 * What the Linux display driver probe leaves after its noirq stage:
 * hotplug, registrations and the IPC setting.
 */
struct i915_driver_probe {
	struct i915_hotplug hp;

	/* intel_display_driver_probe */
	unsigned active_crtcs;
	int initial_commit_rc;
	int initial_commit_unimplemented;   /* an active crtc needs the atomic commit */
	int power_domains_enable_deferred;  /* the INIT reference is KEPT until the takeover */
	int overlay;                        /* HAS_OVERLAY */
	int fbdev;                          /* CONFIG_DRM_FBDEV_EMULATION */
	int ipc_enabled;

	/* i915_driver_register */
	int opregion_registered;
	unsigned na_registrations;          /* userspace-facing steps recorded N/A */
	unsigned wells_on_before, wells_on_after;
	int dc_state_after;                 /* DC_STATE_EN & mask after the INIT put */
	unsigned verify_mismatches;
	int rpm_usage_after;
	int registered;

	int err;
	const char *err_where;
};
/*
 * The DRM device and vblank workers (device.c).
 */
/*
 * minimal DRM device management + vblank (see device.c).
 *
 * Ports the drm_dev_init() side of DRM device state, a drmm managed-resource
 * list with the drm_add_action_or_reset() contract, and drm_vblank_init() /
 * drm_vblank_worker_init() for the display "noirq" bring-up (P3).  It does not
 * open /dev/dri, start display output, or wait on real vblanks.
 */

struct i915_device;

#define I915_DRM_MAX_PIPES  4    /* ADL-P pipe_mask = A|B|C|D */
#define I915_DRMM_MAX      16    /* managed cleanup-action slots */

/* A managed cleanup action (drmm_add_action_or_reset). */
struct i915_drmm_action {
	void (*fn)(void *arg);
	void *arg;
};

/* Per-CRTC vblank state (drm_vblank_crtc subset). */
struct i915_drm_vblank_crtc {
	struct spinlock lock;        /* per-CRTC vblank lock */
	struct wait_queue queue;     /* vblank wait queue */
	unsigned pipe;               /* CRTC/pipe index */
	uint64_t count;              /* vblank sequence (seqlock-guarded in the reference) */
	uint32_t seqlock;            /* seqlock generation (even = stable) */
	int disable_timer_inited;    /* vblank-disable timer state (modelled) */
	uint64_t disable_deadline;
	struct i915_workqueue worker;  /* per-CRTC vblank worker (drm_vblank_worker_init) */
	int worker_created;
	int inited;
};

/* DRM device management state (drm_device subset). */
struct i915_drm_device {
	struct i915_device *parent;  /* back-reference to the parent device */
	uint32_t driver_features;    /* DRIVER_* feature flags subset */
	int open_count;              /* device open refcount */

	struct spinlock managed_lock;/* drmm managed-resource list lock */
	struct i915_drmm_action drmm[I915_DRMM_MAX];
	unsigned drmm_count;

	struct spinlock event_lock;  /* event list lock */
	int minor_registered;        /* minor bookkeeping; 0 = not published */

	unsigned num_crtcs;          /* INTEL_NUM_PIPES fully initialised */
	int vblank_disable_immediate;
	struct i915_drm_vblank_crtc vblank[I915_DRM_MAX_PIPES];
	int vblank_inited;

	int inited;
};
/*
 * The VGA plane and arbiter (takeover.c).
 */
/* video/internal.h resource flags returned by the decode callback. */
#define I915_VGA_RSRC_LEGACY_IO   0x01u
#define I915_VGA_RSRC_LEGACY_MEM  0x02u
#define I915_VGA_RSRC_NORMAL_IO   0x04u
#define I915_VGA_RSRC_NORMAL_MEM  0x08u

/* VGA arbiter client state (a faithful subset of the registration). */
struct i915_vga_client {
	struct drv_pci_device *gpu;   /* display device (arbiter client) */
	struct drv_pci_device *gmch;  /* host bridge 00:00.0 (GMCH_CTRL owner) */
	unsigned display_ver;         /* selects SNB_GMCH_CTRL vs INTEL_GMCH_CTRL */
	int registered;               /* callback installed (0 after unregister) */
};

/*
 * Legacy VGA I/O accessor (intel_vga_reset_io_mem's get/in/out/put).  In
 * production these are the real arbiter + port I/O; a GPU-free test injects a
 * recorder so the arbiter get -> MIS_R read -> MIS_W write -> put sequence is
 * checked without touching real VGA ports.  get() returns 1 on success, 0 if
 * the legacy-IO resource could not be acquired (then no I/O and no put).
 */
struct i915_vga_io_ops {
	int (*get)(void *ctx, int rsrc);
	unsigned char (*in8)(void *ctx, unsigned short port);
	void (*out8)(void *ctx, unsigned short port, unsigned char val);
	void (*put)(void *ctx, int rsrc);
	void *ctx;
};

/*
 * The combo PHY (phy.c).
 */
/* ADL-P combo PHYs. */
#define I915_COMBO_PHY_A  0u
#define I915_COMBO_PHY_B  1u
#define I915_COMBO_PHY_NUM 2u
/*
 * The VBT state (vbt.c).
 */
/* Where the VBT bytes came from.  An explicit blob is NOT an OpRegion. */
enum i915_vbt_origin {
	I915_VBT_ORIGIN_NONE = 0,       /* no VBT: init_vbt_missing_defaults() */
	I915_VBT_ORIGIN_OPREGION,       /* ASLS -> mailbox 4 / RVDA (native path) */
	I915_VBT_ORIGIN_PCI_ROM,        /* $VBT in the PCI expansion ROM */
	I915_VBT_ORIGIN_EXPLICIT_BLOB,  /* a named, hash-pinned blob supplied by the test configuration */
};

#define I915_VBT_MAX_ENCODERS 12
/*
 * One VBT child device as the output setup reads it.
 */
struct i915_vbt_encoder {
	int port;                 /* enum port value (0 = A, 3 = TC1 ...), -1 if none */
	int aux_ch;               /* enum aux_ch value, -1 if none */
	uint16_t device_type;
	uint8_t dvo_port;
	uint8_t ddc_pin_raw;      /* child.ddc_pin as in the VBT */
	int ddc_pin;              /* mapped GMBUS pin (0 = none/invalid) */
	int supports_dp, supports_edp, supports_hdmi, supports_dvi;
	int supports_typec_usb, supports_tbt;
	int lane_reversal, hpd_invert;
	int dp_max_lane_count;    /* 0 = not limited by the VBT */
	int dp_max_link_rate;     /* kHz-per-lane /10 as in the reference (e.g. 270000), 0 = platform max */
	int dp_boost_level, hdmi_boost_level;
	int hdmi_level_shift;     /* -1 = not set */
};

/*
 * The panel data of the VBT: timings, power sequence and backlight.
 */
struct i915_vbt_panel {
	int initialized;
	int panel_type;
	int bpp;                          /* eDP colour depth from the VBT: 18 / 24 / 30 */
	int edp_rate;                     /* DP_LINK_BW_* code the VBT names for fast link */
	int edp_lanes;
	int edp_preemphasis, edp_vswing;
	int edp_low_vswing;
	int edp_hobl;                     /* panel->vbt.edp.hobl */
	int override_afc_startup;         /* display.vbt.override_afc_startup (general features, BDB 249+) */
	int edp_max_link_rate;            /* 0 = not limited */
	/* eDP panel power sequencing, in 100 us units as stored in the VBT */
	uint16_t t1_t3, t8, t9, t10, t11_t12;
	/* backlight */
	int bl_present;
	int bl_type;                      /* enum intel_backlight_type */
	int bl_controller;
	int bl_active_low_pwm;
	uint16_t bl_pwm_freq_hz;
	uint8_t bl_min_brightness;
	/* the VBT's own panel mode, if it carries one (the EDID mode normally wins) */
	int has_lfp_mode;
	int mode_clock_khz;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	int drrs_type, vrr;
};

/*
 * What the VBT parser produced for the rest of the display.
 */
struct i915_vbt {
	int inited;
	int origin;                       /* enum i915_vbt_origin */
	const uint8_t *bytes;             /* the validated VBT (not owned) */
	size_t size;
	uint16_t bdb_version;
	char signature[21];
	int missing_defaults_used;
	unsigned num_bdb_blocks;
	unsigned n_encoders;
	struct i915_vbt_encoder enc[I915_VBT_MAX_ENCODERS];
	/* general features the display code consumes */
	int int_lvds_support, display_clock_mode, lvds_use_ssc, lvds_ssc_freq, crt_ddc_pin;
	unsigned arena_used, arena_peak, alloc_failures;
	unsigned log_errors;              /* drm_err / WARN lines the reference emitted */
};

/*
 * The VBT provider (vbt.c).
 */
/*
 * intel_bios_init (VBT acquisition + parse), see vbt.c.
 *
 * Structure-faithful port of intel_bios_init(): initialise the VBT device/block
 * lists, apply init_vbt_defaults(), then resolve the VBT from (1) the OpRegion
 * (carried from P2), else (2) the PCI expansion ROM read for real via config
 * 0x30 + a device mapping.  A found+validated VBT is parsed (BDB blocks, general
 * features/definitions); a genuine absence takes init_vbt_missing_defaults()
 * (default child devices for the non-TC DDI ports).  DGFX SPI flash is NOT read
 * on ADL-P (not IS_DGFX).  This never fabricates a VBT and never fails the probe
 * (intel_bios_init is void): it reports which real source was used.
 */

struct i915_pci;

/*
 * Where the VBT the parser was given came from.
 */
enum i915_vbt_source {
	I915_VBT_SRC_NONE = 0,     /* no VBT found -> missing defaults */
	I915_VBT_SRC_OPREGION,     /* OpRegion mailbox #4 (from P2) */
	I915_VBT_SRC_PCI_ROM,      /* PCI expansion ROM ($VBT) */
	I915_VBT_SRC_EXPLICIT_BLOB, /* a named, hash-pinned blob the build asked for -- NOT an OpRegion */
};

/* One VBT child device (child_device_config subset used for default gen). */
struct i915_vbt_child {
	unsigned port;               /* source DDI port (diagnostic) */
	uint8_t  dvo_port;           /* DVO_PORT_* */
	uint32_t device_type;        /* DEVICE_TYPE_* bitmask */
};

/* VBT-derived display state (drm_i915_private.display.vbt subset). */
struct i915_vbt_state {
	int has_display;                 /* HAS_DISPLAY snapshot */
	uint16_t version;                /* display.vbt.version */
	int vbt_found;                   /* a real VBT was located + validated */
	int source;                      /* enum i915_vbt_source */
	int missing_defaults_used;       /* init_vbt_missing_defaults() ran */
	unsigned num_bdb_blocks;         /* bdb_blocks list length */
	unsigned num_display_devices;    /* display_devices list length */
	struct i915_vbt_child display_devices[8];

	/* the reference parser's result (vbt.c); device-owned, released by driver_remove */
	struct i915_vbt parsed;
	int parsed_live;

	/* explicit blob bookkeeping (only filled by the test build, I915_TEST_VBT; zero in production) */
	const char *blob_name;
	unsigned blob_size;
	uint8_t blob_sha256[32];
	int blob_requested, blob_found, blob_hash_ok, blob_subsys_ok, blob_valid;
	uint16_t subsys_vendor, subsys_device;
};

/*
 * Whether the explicit VBT is asked for.
 *
 * Only the test build (I915_TEST_VBT) carries one: the captured VBT of the
 * test machine, used when the PCI subsystem id and the SHA-256 match.  It
 * supplies the machine's configuration data; it does not make an OpRegion
 * exist (ASLS stays what the firmware left).  A production kernel takes the
 * VBT from the OpRegion or the PCI ROM, or the Linux defaults.
 *
 * XXX: a test crutch for the QEMU passthrough guest, whose firmware presents
 * no OpRegion.  Delete it once the GPU tests run on bare metal.
 */
#ifdef I915_TEST_VBT
#define I915_VBT_EXPLICIT 1
#else
#define I915_VBT_EXPLICIT 0
#endif

/*
 * The OpRegion ACPI service (opregion.c).
 */
/*
 * Linux-derived -- the ACPI notifier chain the i915 OpRegion code registers its receive callback on
 * (register_acpi_notifier / unregister_acpi_notifier / acpi_notifier_call_chain).  zedBSD project code.
 *
 * Contract followed (Linux v6.8.12 drivers/acpi/event.c + kernel/notifier.c, checked against the public sources; no
 * text is copied -- those files are GPL-2.0, this is an independent implementation of the interface contract):
 *   - register: priority order (higher first, equal priority after the existing ones); the same block twice -> -EEXIST.
 *   - unregister: removes the block; a block not on the chain -> -ENOENT.
 *   - call chain: each callback in order; a result with NOTIFY_STOP_MASK ends the walk; the chain's result is the last
 *     callback's (NOTIFY_DONE when nobody was called).  acpi_notifier_call_chain() returns -EINVAL when that result is
 *     NOTIFY_BAD, else 0.
 *   - blocking: callbacks run in a context that may sleep; register / unregister are serialised against a running call
 *     chain, so after unregister returns the callback is not running and is not called again.
 * ADAPTATION: one kernel mutex serialises everything (Linux uses an rwsem: concurrent dispatches may overlap there).
 * A callback must not register / unregister on this chain (it would deadlock, as with the rwsem).
 *
 * The event source is separate: today only synthetic events (tests); an ACPI (AML Notify) source is connected later
 * to the same i915_acpi_notifier_call_chain() entry.
 */

struct notifier_block {
	int (*notifier_call)(struct notifier_block *nb, unsigned long action, void *data);
	struct notifier_block *next;
	int priority;
};

#define NOTIFY_DONE      0x0000
#define NOTIFY_OK        0x0001
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD       (NOTIFY_STOP_MASK | 0x0002)

/* struct acpi_bus_event: acpi_device_class (char[20]) and acpi_bus_id (char[8]) */
struct acpi_bus_event {
	char device_class[20];
	char bus_id[8];
	uint32_t type;
	uint32_t data;
};

/* what one dispatch did, kept apart: the callbacks' result, the dispatch's return value, how many ran */
struct i915_acpi_dispatch {
	const char *event_source;       /* "SYNTHETIC" today; "ACPI" once an AML Notify source exists */
	int callback_result;            /* the chain's result (NOTIFY_*) */
	int dispatch_result;            /* acpi_notifier_call_chain(): -EINVAL for NOTIFY_BAD, else 0 */
	unsigned calls;
};

/*
 * The OpRegion VBT source (vbt.c).
 */
/*
 * Linux-derived -- where the ACPI OpRegion keeps the VBT (intel_opregion_setup(), v6.8.12).  zedBSD project code;
 * the offsets and the decision order are the reference's (struct opregion_header / struct opregion_asle,
 * OPREGION_*_OFFSET, MBOX_*).  Pure: it reads a copy of the 8 KiB OpRegion and says where the VBT is; the caller maps
 * and validates it (intel_bios_is_valid_vbt).  intel_load_vbt_firmware() and the DMI quirk list are not consulted here.
 */

#define I915_OPREGION_SIZE        0x2000u
#define I915_OPREGION_ASLE_OFFSET 0x300u
#define I915_OPREGION_VBT_OFFSET  0x400u
#define I915_OPREGION_ASLE_EXT    0x1c00u

/*
 * Where in the OpRegion the VBT was found.
 */
enum i915_opregion_vbt_src { I915_OPVBT_NONE = 0, I915_OPVBT_RVDA, I915_OPVBT_MAILBOX4 };

/*
 * What the OpRegion check found: its header, mailboxes and VBT location.
 */
struct i915_opregion_info {
	int signature_ok;
	uint32_t size_kib, mboxes;
	uint8_t major, minor, revision;
	uint64_t rvda;                  /* as stored */
	uint32_t rvds;
	int rvda_relative;              /* 2.1+: an offset from the OpRegion base */
	int rvda_inside;                /* 2.1+ and rvda < OPREGION_SIZE: the reference WARNs */
	/* the candidate the reference tries first: RVDA (physical address), else mailbox #4 (offset in the OpRegion) */
	int src;
	uint64_t vbt_phys;              /* RVDA */
	uint32_t vbt_offset, vbt_max;   /* mailbox #4: offset and the size the reference allows */
	/* runtime mailboxes as the firmware (or a previous driver) left them -- OBSERVED, never written by this driver */
	uint32_t acpi_drdy, acpi_csts, acpi_cevt, acpi_chpd, acpi_clid;   /* struct opregion_acpi at 0x100 */
	uint32_t asle_ardy, asle_aslc, asle_tche;                         /* struct opregion_asle at 0x300 */
};
/*
 * The firmware display check (takeover.c).
 */
/*
 * Linux-derived -- N0: what the firmware left, recorded before the driver's first display write, and whether the
 * prepared start path applies.  zedBSD project code.  READ-ONLY: nothing here writes a register, the OpRegion or the
 * VT-d unit.  Registers of a power well that is off are "not readable" (never read as "disabled").
 *
 * Decision: PROCEED only when (a) no pipe is active (a pipe whose power domain is off is not_readable and counted inactive), (b) the VT-d unit that translates the
 * GPU is not translating and has no protected memory region enabled (zedBSD has no IOMMU driver), (c) the firmware
 * framebuffer's GGTT pages do not overlap the pages this driver writes.  Otherwise STOP, with the exact reason, before
 * any display write (N1 -- the takeover of a firmware display -- is not ported yet).
 */

enum i915_native_pipe_class {
	I915_N0_POWER_OFF = 0,        /* the wells' STATE bits read validly as off: the pipe cannot run */
	I915_N0_READ_ERROR,           /* the power state or a pipe register did not read as a register value */
	I915_N0_READABLE_INACTIVE,
	I915_N0_READABLE_ACTIVE
};

/* every condition N0 observed (primary_stop is the first in decision order) */
#define I915_N0_C_ACTIVE_PIPE     (1u << 0)
#define I915_N0_C_PIPE_READ_ERROR (1u << 1)
#define I915_N0_C_GGTT_OVERLAP    (1u << 2)
#define I915_N0_C_VTD_UNREADABLE  (1u << 3)
#define I915_N0_C_VTD_TRANSLATION (1u << 4)
#define I915_N0_C_VTD_PMR         (1u << 5)
#define I915_N0_C_VTD_IR_ENABLED  (1u << 6)   /* interrupt remapping on: the MSI path must be checked (not a DMA stop) */
#define I915_N0_C_OPREGION_REGISTER (1u << 7) /* ASLS != 0: the probe stops later at intel_opregion_register (not ported) */
#define I915_N0_C_VBT_DIFFERS     (1u << 8)   /* the OpRegion VBT is not the bytes the parser consumed */

struct i915_native_pipe {
	int readable;                   /* pipe or transcoder power domain on */
	int cls;                        /* enum i915_native_pipe_class */
	uint32_t transconf, trans_ddi_func, pipesrc, plane_ctl, plane_surf, plane_stride, plane_size;
};

/*
 * What the firmware left on the display before the first display write,
 * and the decision taken from it.
 */
struct i915_native_report {
	int hypervisor;                 /* CPUID.1:ECX[31] */
	/* OpRegion / VBT */
	uint32_t asls;
	int opregion_mapped;
	struct i915_opregion_info op;
	int vbt_mapped, vbt_valid, vbt_matches_pin;
	uint32_t vbt_size;
	uint8_t vbt_sha256[32];
	/* the VT-d unit of the GPU (GFXVTBAR through the MCHBAR mirror) */
	uint64_t gfxvtbar;
	int vtd_enabled, vtd_readable;
	uint32_t vtd_ver, vtd_gsts, vtd_pmen;
	/* the firmware framebuffer */
	int fb_present;
	uint64_t fb_base, fb_size;
	int fb_in_aperture;
	uint32_t fb_ggtt_first, fb_ggtt_pages;
	/* display */
	struct i915_native_pipe pipe[4];
	unsigned active_pipes, unreadable_pipes;
	uint32_t pll_enable[2], ddi_buf_ctl_a, pp_status, pp_control, blc_ctl, blc_duty;
	uint32_t dpclka_cfgcr0;         /* ICL_DPCLKA_CFGCR0: the PHY -> PLL clock select */
	/* the pages this driver will write in the GGTT */
	uint32_t driver_ggtt_first, ggtt_pages;
	int overlap;
	int proceed;
	const char *reason;
	/* */
	uint32_t pwr_well_ctl;          /* the driver request register (STATE bits) as read: all-ones = READ_ERROR */
	uint32_t conditions;            /* I915_N0_C_* observed */
	uint32_t primary_stop;          /* the one that decided STOP (0 when PROCEED) */
	int parser_src;                 /* enum i915_vbt_source the parser consumed */
	uint32_t parser_size;
	uint8_t parser_sha256[32];
	int parser_sha_known, vbt_same_bytes;
};

/*
 * What the firmware display check reads through: registers and power-well
 * state.
 */
struct i915_native_deps {
	struct i915_mmio *mmio;
	uint32_t asls;
	uint64_t gmadr_base, gmadr_size;
	uint32_t ggtt_pages;            /* entries in the GGTT */
	uint32_t driver_ggtt_first;     /* the first GGTT page the driver writes (top window + display window) */
	/* the reference's readout gate: is the pipe's / transcoder's power domain on (no write) */
	int (*pipe_powered)(void *ctx, unsigned pipe);
	void *ctx;
	const uint8_t *vbt_pin;         /* the test build's explicit blob pinned sha256 (NULL in production) */
	/* what the VBT parser actually consumed (intel_bios_init ran before N0) */
	int parser_src;
	uint32_t parser_size;
	const uint8_t *parser_sha256;   /* 0 when not known */
};

/*
 * The OpRegion as DATA (VBT_ONLY, ): P2 maps it read-only, copies the 8 KiB and the VBT it names, validates the
 * VBT.  The runtime protocol (ACPI notifier, drdy / ardy / chpd / csts / DIDL / CADL, ASLE) is NOT joined: runtime =
 * DISABLED, reason ACPI_RUNTIME_UNAVAILABLE.  The mailboxes' current values are observed only.
 */
struct i915_opregion_data {
	int present;                    /* ASLS != 0 */
	int mapped;
	struct i915_opregion_info op;
	int vbt_valid;
	const uint8_t *vbt;             /* the copy handed to the parser (0 = none) */
	uint32_t vbt_size;
	uint8_t vbt_sha256[32];
	int runtime_enabled;            /* always 0 here */
	const char *runtime_reason;
};

/*
 * The eDP sink bring-up (dp-sink.c).
 */
/*
 * eDP panel bring-up, first stage: panel power sequencer
 * (PPS / VDD) ownership and the AUX channel, up to the sink's DPCD and EDID.
 *
 * The work is done by the reference's own functions (generated files in this
 * directory: panel.c, aux.c, dp-sink.c,
 * edid-read.c) in the order of intel_edp_init_connector().  This header is
 * the zedBSD-facing surface: plain C types only, so the probe and the GPU-free
 * tests can use it without the reference's environment.
 *
 * Every hardware and time dependency goes through struct i915_dp_env, so the
 * same production code runs on the real GPU (dp-sink.c binds it to the
 * MMIO / wait / power-domain layers) and on the register model (dp_fake_hw.c).
 */

struct i915_dp_env {
	void *ctx;
	uint32_t (*read32)(void *ctx, uint32_t reg);
	void (*write32)(void *ctx, uint32_t reg, uint32_t value);
	/* __intel_wait_for_register(): 0, or a negative errno (-ETIMEDOUT); *out (may be NULL) = last value */
	int (*wait_reg)(void *ctx, uint32_t reg, uint32_t mask, uint32_t value,
		unsigned fast_us, unsigned slow_ms, uint32_t *out);
	void (*sleep_us)(void *ctx, unsigned us);
	uint64_t (*now_ms)(void *ctx);                  /* monotonic */
	int (*power_get)(void *ctx, int domain);        /* enum i915_power_domain value; 0 = ok */
	void (*power_put)(void *ctx, int domain);
	/*
	 * intel_display_power_put_async(): the reference leaves the DP layer now; the power layer
	 * parks it, hands it back to the next get of that domain, or releases it ~100 ms later
	 */
	void (*power_put_async)(void *ctx, int domain);
	/* the PPS mutex and the AUX hardware mutex (I915_DP_LOCK_*): real mutual exclusion */
	void (*lock)(void *ctx, int which);
	void (*unlock)(void *ctx, int which);
	/*
	 * delayed work (I915_DP_WORK_*): queue = 1 when newly queued (0: already pending);
	 * cancel = 1 when it was pending; with sync != 0 it also waits until the work body is
	 * not running -- the caller then must not hold a lock the body takes.  The backend runs
	 * the body by calling i915_edp_work_run(which) from a context that may sleep.
	 */
	int (*delayed_queue)(void *ctx, int which, unsigned delay_ms);
	int (*delayed_cancel)(void *ctx, int which, int sync);
	int (*delayed_pending)(void *ctx, int which);
	/* bookkeeping kept by the DP layer (read by the tests and the run log) */
	int power_refs[2];                              /* [0] DISPLAY_CORE, [1] the AUX domain */
	unsigned power_get_failures;
	unsigned power_put_underflows;
	unsigned sleeps;
	uint64_t slept_us;
	unsigned lock_errors;                           /* recursion / unlock of a free lock */
	unsigned async_puts;
};

#define I915_DP_LOCK_PPS 0
#define I915_DP_LOCK_AUX 1
#define I915_DP_WORK_VDD_OFF 0        /* edp_panel_vdd_work */

#define I915_EDP_MAX_EDID_BLOCKS 4u

/*
 * Result codes are the reference's negative errnos in LINUX numbering (zedBSD's
 * <errno.h> numbers differ): rc == -I915_EDP_ETIMEDOUT and so on.
 */
#define I915_EDP_EIO 5
#define I915_EDP_ENXIO 6
#define I915_EDP_E2BIG 7
#define I915_EDP_EBUSY 16
#define I915_EDP_EINVAL 22
#define I915_EDP_EPROTO 71
#define I915_EDP_ETIMEDOUT 110
#define I915_EDP_EREMOTEIO 121

/*
 * How to reach the eDP panel: port, AUX channel, raw clock and the VBT
 * power sequence.
 */
struct i915_edp_config {
	int port;                 /* enum port: 0 = A */
	int aux_ch;               /* enum aux_ch from the VBT child: 0 = A */
	uint32_t rawclk_khz;
	/*
	 * the VBT panel's eDP power sequence (100 us units) and PPS / backlight controller index;
	 * all zero when the panel type is not known before the EDID (the reference's "early" state)
	 */
	uint16_t t1_t3, t8, t9, t10, t11_t12;
	int bl_controller;
	int log_level;            /* 0 errors, 1 +info, 2 +debug */
};

/*
 * The four panel power sequencer registers of one sequencer.
 */
struct i915_edp_pps_regs { uint32_t pp_status, pp_control, pp_on_delays, pp_off_delays; };

/*
 * How far the eDP sink bring-up got.
 */
enum i915_edp_stage {
	I915_EDP_STAGE_NONE = 0,
	I915_EDP_STAGE_PPS_INIT,        /* intel_pps_init() */
	I915_EDP_STAGE_DPCD,            /* intel_edp_init_dpcd(): receiver caps + eDP display control */
	I915_EDP_STAGE_EDID,            /* drm_edid_read_ddc(): I2C-over-AUX */
	I915_EDP_STAGE_ACQUIRED,        /* waiting for i915_edp_init_late() */
	I915_EDP_STAGE_LATE,            /* intel_pps_init_late() done: VDD-off is scheduled */
	I915_EDP_STAGE_ENDED,
};

/*
 * What the eDP sink bring-up found and did: the PPS registers, DPCD, EDID
 * and who holds what.
 */
struct i915_edp_result {
	int stage;                        /* the last stage COMPLETED */
	int rc;                           /* 0, or the negative errno of the stage that failed */
	int failed_stage;

	int pps_idx, pps_valid;
	struct i915_edp_pps_regs before;       /* as found */
	struct i915_edp_pps_regs after_init;   /* after intel_pps_init() programmed the delays */
	struct i915_edp_pps_regs after_acquire;/* VDD is expected ON here */
	struct i915_edp_pps_regs after_end;    /* after intel_pps_vdd_off_sync() */
	int delay_power_up_ms, delay_power_down_ms, delay_power_cycle_ms, delay_bl_on_ms, delay_bl_off_ms;

	uint8_t dpcd[15];                 /* DP_RECEIVER_CAP_SIZE */
	int dpcd_ok;
	uint8_t edp_dpcd[3];              /* EDP_DISPLAY_CTL_CAP_SIZE, from 0x700 */
	int edp_dpcd_ok;
	uint8_t link_cfg[2];              /* DP_LINK_BW_SET / DP_LANE_COUNT_SET as found (not written) */
	int link_cfg_ok;
	uint8_t edid[128u * I915_EDP_MAX_EDID_BLOCKS];
	unsigned edid_blocks;             /* valid blocks read */
	unsigned edid_extensions;         /* the base block's extension count */
	int edid_ok;

	/* ownership at the end of each phase */
	int vdd_wanted, vdd_on_hw, vdd_wakeref_held, vdd_work_pending;
	int power_refs_core, power_refs_aux;
	unsigned power_get_failures, power_put_underflows;
	unsigned i2c_defers, i2c_nacks;
	unsigned log_errors;              /* drm_err / WARN lines the reference emitted */
	uint64_t elapsed_ms;
};

/*
 * The modeset hooks (modeset.c).
 */
/* panel power sequencer / backlight-power operations, executed by the resident eDP (dp-sink.c) */
enum i915_lcd_panel_op {
	I915_LCD_PANEL_ON = 0,          /* intel_pps_on() */
	I915_LCD_PANEL_OFF,             /* intel_pps_off() */
	I915_LCD_PANEL_VDD_ON,          /* intel_pps_vdd_on() */
	I915_LCD_PANEL_VDD_OFF_SYNC,    /* intel_pps_vdd_off_sync() */
	I915_LCD_PANEL_BACKLIGHT_ON,    /* intel_pps_backlight_on(): the PPS's backlight-enable bit */
	I915_LCD_PANEL_BACKLIGHT_OFF,   /* intel_pps_backlight_off() */
};

#define I915_LCD_LOCK_DPLL 0           /* i915->display.dpll.lock */
#define I915_LCD_LOCK_BACKLIGHT 1      /* i915->display.backlight.lock */

/* points of the commit at which the caller may look at the hardware (no writes of the path depend on them) */
enum i915_lcd_observe {
	I915_LCD_OBS_COMMIT_BEGIN = 0,        /* DC_OFF held, nothing written yet */
	I915_LCD_OBS_UNDERRUN_ARM,            /* where the reference clears the pipe's underrun status and unmasks its interrupt */
	I915_LCD_OBS_PIPE_ENABLED,            /* the crtc enable returned */
	I915_LCD_OBS_PLANE_ARMED,             /* PLANE_SURF written */
	I915_LCD_OBS_PLANE_DISABLED,          /* the plane disable was armed */
	I915_LCD_OBS_UNDERRUN_DISARM,         /* where the reference masks the underrun interrupt again */
	I915_LCD_OBS_PIPE_DISABLED,           /* the crtc disable returned */
	I915_LCD_OBS_COMMIT_END,              /* before DC_OFF is dropped */
	I915_LCD_OBS_NUM
};

/*
 * wait_reg results (Linux numbering, as the reference's callers compare them):
 *   0                    the condition held
 *   I915_LCD_ETIMEDOUT the device did not reach the condition in time
 *   I915_LCD_EIO       the time source / the wait primitive / MMIO access failed: NOT a timeout; the backend has
 * also reported it through i915_lcd_backend_fault() so it is the run's first anomaly
 */
#define I915_LCD_ETIMEDOUT (-110)
#define I915_LCD_EIO       (-5)

/*
 * The hooks the modeset code reaches the outside through: registers,
 * waits, time, DPCD, panel power, display power, vblank, locks and errors.
 */
struct i915_lcd_emit {
	void *ctx;
	int model;              /* 1: a register / sink MODEL (discarding it isolates whatever it holds); 0: real hardware */
	void (*write32)(void *ctx, uint32_t reg, uint32_t value);
	uint32_t (*rmw32)(void *ctx, uint32_t reg, uint32_t clear, uint32_t set);   /* returns the old value */
	void (*posting_read)(void *ctx, uint32_t reg);                              /* may be NULL */
	void (*step)(void *ctx, const char *name);      /* a callee of the reference that is not ported */
	/* ---- may be NULL in the pure word recorder ---- */
	uint32_t (*read32)(void *ctx, uint32_t reg);
	/* intel_de_wait_for_set / _clear / _register: 0, or -110 (-ETIMEDOUT, Linux numbering) */
	int (*wait_reg)(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned timeout_ms);
	void (*usleep)(void *ctx, unsigned us);         /* usleep_range / msleep: may yield */
	void (*udelay)(void *ctx, unsigned us);         /* udelay: short, does not yield */
	/* drm_dp_dpcd_read / _write on the panel's AUX channel: bytes transferred, or a negative errno */
	long (*dpcd_read)(void *ctx, unsigned offset, uint8_t *buf, size_t size);
	long (*dpcd_write)(void *ctx, unsigned offset, const uint8_t *buf, size_t size);
	/* drm_dp_read_dpcd_caps(): receiver caps incl. the extended field; 0, or a negative errno */
	int (*read_dpcd_caps)(void *ctx, uint8_t dpcd[15]);
	int (*panel)(void *ctx, int op);                /* enum i915_lcd_panel_op; 0 = done */
	/*
	 * intel_display_power_get / _put; `domain` = the reference's enum intel_display_power_domain
	 * value.  get returns a non-zero wakeref cookie (0 = failed); put takes it back.
	 */
	int (*power_get)(void *ctx, int domain);
	/*
	 * intel_display_power_get_if_enabled(): the cookie, or 0 when the well is OFF (a readout never
	 * turns a well on).  NULL: the backend has no well state of its own (a model) -- the caller then
	 * falls back to power_get.
	 */
	int (*power_get_if_enabled)(void *ctx, int domain);
	void (*power_put)(void *ctx, int domain, int wakeref);
	/* intel_display_power_put_async_delay(): the reference drops DC_OFF this way at the end of a commit */
	void (*power_put_async)(void *ctx, int domain, int wakeref, int delay_ms);
	/* gen9_dbuf_slices_update(): request exactly these DBUF slices (bit n = slice n+1) */
	void (*dbuf_slices_update)(void *ctx, unsigned req_slices);
	/*
	 * The synchronous plane update (intel_pipe_update_start / _end) and its completion event:
	 *   vblank_get / vblank_put   drm_crtc_vblank_get / _put of the pipe (0 / -EINVAL)
	 *   vblank_sleep              schedule_timeout() on the pipe's vblank wait queue: sleep until the pipe's next vblank
	 *                             interrupt or `ticks` display ticks of 10 ms (not kernel ticks); returns the ticks left (0 = timed out)
	 *   irq_off / irq_on          local_irq_disable / _enable around the short update section
	 *   arm_event                 drm_crtc_arm_vblank_event(): the event completes at the pipe's next vblank after now
	 *   wait_event                wait for that completion (0; -110 not within timeout_ms; -5 time base / wait fault)
	 */
	int (*vblank_get)(void *ctx, int pipe);
	void (*vblank_put)(void *ctx, int pipe);
	long (*vblank_sleep)(void *ctx, int pipe, long ticks);
	void (*irq_off)(void *ctx);
	void (*irq_on)(void *ctx);
	void (*arm_event)(void *ctx, int pipe);
	int (*wait_event)(void *ctx, int pipe, unsigned timeout_ms);
	/* drm_crtc_vblank_off() on a pending event: it will never be waited for again (no completion after this) */
	void (*cancel_event)(void *ctx, int pipe);
	/* optional: a named point of the commit was reached (enum i915_lcd_observe); the real device samples here */
	void (*observe)(void *ctx, int point);
	void (*lock)(void *ctx, int which, int take);   /* I915_LCD_LOCK_*; take = 1 lock, 0 unlock */
	/* drm_err / drm_WARN in the reference text: the FIRST one is what a failed run is read from */
	void (*error)(void *ctx, const char *what);
	void (*debug)(void *ctx, const char *what);     /* drm_dbg_kms: the format string only */
};
/*
 * The state calculation (state.c).
 */
/*
 * LCD-A, first slice: from what the panel and the VBT report
 * (EDID, DPCD, VBT colour depth) to the values the display hardware will be
 * programmed with: the panel mode, the link configuration and its bandwidth check,
 * the link / data M/N pair and the combo PLL configuration words.
 *
 * Calculation only: nothing is written to hardware.  The arithmetic is the
 * reference's own (generated files in this directory); the comparison target is
 * what Linux programmed on the same machine (plan/ws031/display-ref/).
 *
 * NOT computed yet (LCD-A, next slices): DDI buffer translation / voltage swing,
 * transcoder timing register words, plane + scanout layout, CDCLK / bandwidth /
 * DBUF / watermark requirements, the enable and disable state sequences.
 */

struct i915_lcd_mode {
	int clock_khz;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	int hsync_positive, vsync_positive;
	uint16_t width_mm, height_mm;
	unsigned descriptor_index;        /* which of the base block's four descriptors */
	int edid_bpc;                     /* EDID 1.4 digital input colour depth; 0 = undefined */
};

/*
 * The link parameters of one modeset: rate, lanes and the M/N values.
 */
struct i915_lcd_link {
	int sink_max_rate_khz, sink_max_lanes;    /* DPCD 0x001 / 0x002: the capability */
	int rate_khz, lanes;                      /* the selected configuration */
	int use_max_params;                       /* eDP < 1.4: the reference trains at the maximum */
	int bpp;                                  /* pipe bits per pixel */
	int required_kbps, available_kbps;        /* intel_dp_link_required / intel_dp_max_data_rate */
	uint32_t tu, data_m, data_n, link_m, link_n;
};

/*
 * The PLL state of one modeset.
 */
struct i915_lcd_pll {
	int ref_khz;                              /* display.dpll.ref_clks.nssc */
	uint32_t cfgcr0, cfgcr1, div0;
};

/*
 * The computed state of one modeset: mode, link and PLL.
 */
struct i915_lcd_state {
	struct i915_lcd_mode mode;
	struct i915_lcd_link link;
	struct i915_lcd_pll pll;
	unsigned notes;                           /* messages the reference text emitted */
};

/* one register write, in the order the reference's writer functions issue them */
struct i915_lcd_regwrite {
	uint32_t reg, value;          /* a write: the value.  A read-modify-write: the bits SET */
	uint32_t clear;               /* read-modify-write only: the bits cleared first */
	uint8_t rmw;                  /* 1 = read-modify-write (the resulting word depends on the hardware) */
	const char *step;             /* not a register operation: a reference callee that is not ported yet, at its position */
};
#define I915_LCD_MAX_REGWRITES 96u
/*
 * The register words one modeset writes, recorded in order.
 */
struct i915_lcd_words {
	unsigned n;
	unsigned overflow;
	struct i915_lcd_regwrite w[I915_LCD_MAX_REGWRITES];
};

/*
 * The commit observation (diagnostics.c).
 */
/*
 * Linux-derived -- what the LCD test LOOKS AT while the reference's modeset runs: the pipe's underrun status
 * (ICL_PIPESTATUS), its frame counter and its interrupt mask.  zedBSD project code; plain C over i915_lcd_ops,
 * so the same code runs on the register model and on the real GPU.
 *
 * Why it exists: this private, synchronous, one-buffer test does not connect the DRM vblank machinery nor the
 * underrun interrupt (see modeset-internal.h).  In exchange it OWNS the status register for the run:
 *   - the value found before the run is saved (not judged: it belongs to whoever ran before);
 *   - the status is cleared where the reference clears it (intel_set_cpu_fifo_underrun_reporting(true));
 *   - it is sampled at every observe() point of the commits and by the caller during the steady picture; bits seen
 *     are RECORDED first and only then cleared, so that a later period can be told from an earlier one and no
 *     occurrence is lost;
 *   - start / stop periods and the steady period are accumulated apart: an underrun while the pipe starts is a
 *     different statement from one while a picture stands.
 * The pipe's vblank interrupt must stay masked for the whole run (nothing here could service it): every sample also
 * reads GEN8_DE_PIPE_IMR and remembers if the vblank bit was ever found unmasked -- judged only while the pipe's
 * power well is held (the register lives in that well: before, it reads 0; the power-well enable programs the mask).
 * "Frames advance" is decided from the hardware frame counter alone -- never from elapsed time.
 */

#define I915_LCD_OBS_STEADY 100               /* sample points of the caller: the steady picture */
#define I915_LCD_OBS_MAX_SAMPLES 96u

/*
 * One sample the commit observer took at a named point of the commit.
 */
struct i915_lcd_obs_sample {
	int point;                      /* enum i915_lcd_observe, or I915_LCD_OBS_STEADY */
	uint32_t status;                /* ICL_PIPESTATUS as read */
	uint32_t frame;                 /* PIPE_FRMCOUNT_G4X as read */
	uint32_t imr;                   /* GEN8_DE_PIPE_IMR as read */
	int steady;
};

/*
 * The observer of a commit: the samples it took at the named points.
 */
struct i915_lcd_observer {
	struct i915_lcd_emit *hw;     /* the backend that is read (NOT the recorder: these reads are not part of the path) */
	int pipe;
	uint32_t status_reg, frame_reg, imr_reg;
	uint32_t underrun_mask;         /* the bits the reference treats as underrun status on this display version */
	uint32_t vblank_bit;
	uint32_t saved_before;          /* status found before the run */
	int begun, steady;
	int powered;                    /* between the enable's underrun-arm point and the disable's pipe-disabled point the crtc holds
	                                 * the pipe's power domain; outside, pipe A's registers (power well A) read 0 and say nothing */
	uint32_t seen_transition;       /* underrun bits seen while the pipe was started / stopped */
	uint32_t seen_steady;           /* underrun bits seen while the picture stood */
	int vblank_unmasked_seen;
	/* the stop evidence, taken at the disable's pipe-disabled point -- the last moment the pipe's power well is on */
	int stop_checked; uint32_t stop_transconf, stop_frame0, stop_frame1;
	unsigned n, dropped;
	struct i915_lcd_obs_sample s[I915_LCD_OBS_MAX_SAMPLES];
};

/* the registers the run log reads back, by the reference's names (diagnostics.c) */
struct i915_lcd_named_reg {
	const char *name;
	uint32_t reg;
	int has_linux;                  /* Linux's dump of this machine has a value for it */
	uint32_t linux_value, compare_mask;
};

       /* the reference's DBUF_CTL_S(slice), slice 0 = S1 */      /* pipe A / port A / DPLL 0; 0 = unknown name */

/*
 * The modeset trace (diagnostics.c).
 */
/*
 * a recorder stacked on any i915_lcd_ops backend (the register / sink model or the
 * real GPU): every operation is forwarded, and logged in order with what the backend answered.  It is
 * the run log of a modeset ("which phase, which write, what the wait returned, the first anomaly") and
 * the source of the sequence tables; it never replays anything.  zedBSD project code; plain C.
 */

enum i915_lcd_trace_kind {
	I915_LCD_T_WRITE = 1,     /* a = reg, b = value */
	I915_LCD_T_RMW,           /* a = reg, b = set, c = clear, d = old value */
	I915_LCD_T_READ,          /* a = reg, b = value, n = how many identical reads in a row (polling) */
	I915_LCD_T_WAIT,          /* a = reg, b = value wanted, c = mask, rc */
	I915_LCD_T_DPCD_WRITE,    /* a = offset, b = first bytes (little end first), c = size, rc */
	I915_LCD_T_DPCD_READ,     /* a = offset, b = first bytes, c = size, rc */
	I915_LCD_T_PANEL,         /* a = op, rc */
	I915_LCD_T_POWER_GET,     /* a = domain, rc = wakeref */
	I915_LCD_T_POWER_PUT,     /* a = domain, b = wakeref; asynchronous: d = 1, c = delay in ms */
	I915_LCD_T_STEP,          /* name: a reference callee that is not ported */
	I915_LCD_T_ERROR,         /* name: a drm_err / WARN of the reference text */
	I915_LCD_T_PHASE,         /* name: a marker written by the caller */
	I915_LCD_T_DECIDED,       /* name: a reference callee deliberately not connected (reason in modeset-internal.h) */
	I915_LCD_T_DBUF,          /* a = the DBUF slices requested (gen9_dbuf_slices_update) */
	I915_LCD_T_OBSERVE,       /* a = enum i915_lcd_observe: a point of the commit */
};

/*
 * One recorded operation of a modeset run.
 */
struct i915_lcd_trace_entry {
	uint8_t kind;
	uint16_t n;
	int32_t rc;
	uint32_t a, b, c, d;
	const char *name;
};

#define I915_LCD_TRACE_MAX 2048u
/*
 * The ordered record of what a modeset run did, stacked on the hooks it
 * records.
 */
struct i915_lcd_trace {
	struct i915_lcd_emit *backend;
	struct i915_lcd_emit ops;             /* hand THIS to the modeset */
	unsigned n, dropped;
	unsigned writes, rmws, waits, wait_timeouts, steps, decided, errors, sleeps;
	uint64_t slept_us;
	int first_error_at;                     /* index of the first ERROR entry, or -1 */
	void (*tap)(void *ctx, int point);      /* optional: called at every observe() point, before the backend */
	void *tap_ctx;
	struct i915_lcd_trace_entry e[I915_LCD_TRACE_MAX];
};

/*
 * The scanout buffers (scanout.c).
 */
/*
 * the scanout buffer: a framebuffer the DISPLAY ENGINE reads.
 *
 * First form, deliberately narrow: XRGB8888, FORMAT_MOD_LINEAR, rotation 0, no
 * scaling, no compression / aux planes, one colour plane.  With a linear modifier the
 * reference does not use a display page table even on hardware that has one
 * (intel_fb_modifier_uses_dpt(): HAS_DPT && modifier != LINEAR), so the ordinary GGTT
 * path is the reference's own path for this buffer, not a shortcut.  A tiled buffer
 * must revisit that decision from the DPT test on.
 *
 * What this object keeps apart (they are different things with different owners):
 *   backing      DMA pages and their size                       (gt_mem object)
 *   cpu          the CPU mapping used to draw into it
 *   ggtt         the reserved GGTT range: guards + pages, and the display alignment
 *   layout       format / modifier / pitch / height            (what the plane is told)
 *   surf         the address the plane's surface register gets
 *   users        who pinned it, and whether the display is scanning it out
 *
 * The memory image is 32 bits per pixel.  The LINK carries 18 bpp to this panel; that
 * number belongs to the link computation and never sizes this buffer.
 *
 * Lifetime: create -> pin -> [publish after CPU writes] -> scanout_begin ... scanout_end
 * -> unpin -> destroy.  A buffer the display is (or may still be) reading is never
 * unpinned or freed: unpin/destroy refuse it, and if a stop could not be confirmed the
 * owner calls i915_scanout_abandon(), which keeps the pages and the mapping for ever
 * and records that, rather than producing a clean-looking teardown.
 */

struct i915_gt_mem;
struct i915_gt_object;

#define I915_FOURCC_XRGB8888 0x34325258u      /* 'XR24' */
#define I915_MOD_LINEAR 0ull

/*
 * Where a scanout buffer is in its life.
 */
enum i915_scanout_state {
	I915_SCANOUT_NONE = 0,
	I915_SCANOUT_ALLOCATED,       /* backing + CPU mapping */
	I915_SCANOUT_PINNED,          /* + GGTT range, surf address valid */
	I915_SCANOUT_IN_USE,          /* the display engine reads it */
	I915_SCANOUT_ABANDONED        /* stop not confirmed: never released */
};

/*
 * One scanout buffer: its GPU object, the GGTT window it is bound in, its
 * CPU view and its format.
 */
struct i915_scanout {
	/* layout */
	uint32_t width, height;
	uint32_t format;                /* I915_FOURCC_XRGB8888 */
	uint64_t modifier;              /* I915_MOD_LINEAR */
	unsigned cpp;                   /* bytes per pixel of the MEMORY image: 4 */
	uint32_t pitch;                 /* bytes per row: width * cpp rounded up to the stride alignment */
	uint32_t stride_units;          /* pitch / 64: the PLANE_STRIDE encoding for a linear surface */
	uint32_t size;                  /* pitch * height */
	/* placement requirements */
	uint32_t alignment;             /* bytes: intel_surf_alignment() */
	unsigned guard_pages;           /* scratch PTEs on each side: VTD_GUARD */
	/* backing, mapping, GGTT */
	struct i915_gt_mem *gm;
	struct i915_gt_object *obj;
	uint32_t *cpu;
	uint64_t surf;                  /* GGTT offset for PLANE_SURF (fits 32 bits, 4 KiB aligned) */
	/* users */
	int state;
	const char *pin_owner;
	unsigned users;                 /* how many displays read it now (begin / end); IN_USE while > 0 */
	unsigned publishes;
	unsigned refused_unpin, refused_destroy;
};

/*
 * The modeset objects (modeset.c).
 */
/*
 * one-screen modeset of the internal panel: prepare, enable (the reference's
 * hsw_crtc_enable with the DDI / DP callees below it), plane update, plane disable, disable (the
 * reference's hsw_crtc_disable).  zedBSD-facing surface: plain C types only.
 *
 * It is NOT an atomic-commit framework: one crtc, one eDP encoder on a combo-PHY port, one shared DPLL,
 * one primary plane on one linear XRGB8888 framebuffer.  What it keeps from the reference's commit is
 * the meaning of the steps: the new state is prepared before anything is written; the enable, the plane
 * update and the disable are the reference's own callers run on ONE set of objects; ownership that the
 * enable took (power references, the PLL, panel power, the armed plane) is what the disable gives back;
 * "the call returned" is never reported as "it worked" -- status carries what was read back.
 */

struct i915_lcd_modeset_cfg {
	int output_hdmi;                /* 0 = the eDP panel (DP SST), 1 = an HDMI sink on a combo-PHY DDI */
	int vbt_hdmi_level_shift;       /* the VBT child's HDMI level shift for this port; < 0 = the VBT has none */
	unsigned also_active_pipes;     /* the other pipes this configuration lights (BIT(pipe)): the DDB is
	                                 * computed for the whole set, as the reference's atomic check would */
	int port;                       /* enum port: 0 = A, 1 = B (combo PHY only) */
	int pipe, cpu_transcoder;       /* 0 = A */
	int dpll_id;                    /* 0 = DPLL 0, 1 = DPLL 1 */
	int aux_ch;                     /* enum aux_ch: 0 = A */
	uint32_t saved_port_bits;       /* DDI_BUF_CTL readout & DDI_BUF_PORT_REVERSAL, | the VBT lane-reversal flag */
	uint8_t dpcd[15];               /* the sink's receiver capabilities (from the resident eDP) */
	uint8_t edp_dpcd[3];
	int vbt_low_vswing, vbt_hobl;   /* the panel's VBT eDP block */
	/* the VBT backlight block of the panel, and the raw clock the PCH PWM divides (kHz, read out by the caller) */
	int vbt_backlight_present, vbt_backlight_active_low, vbt_backlight_controller;
	uint16_t vbt_backlight_pwm_freq_hz;
	uint8_t vbt_backlight_min_brightness;
	uint32_t rawclk_khz;
	/* watermark / DDB inputs, as the normal initialisation read and keeps them */
	uint16_t wm_latency[8];         /* skl_setup_wm_latency(): usec per level */
	uint8_t wm_num_levels;
	int wm_ipc_enabled;
	uint8_t sagv_block_time_us;
	uint32_t dbuf_size;             /* DISPLAY_INFO()->dbuf.size / slice_mask of the platform */
	uint8_t dbuf_slice_mask;
	uint8_t dbuf_enabled_slices;    /* the slices enabled now (the old global DBUF state) */
	int mbus_joined;                /* MBUS_CTL as found (the old global DBUF state) */
	/* the CDCLK state the normal initialisation left (display.cdclk.hw) and the platform limit */
	uint32_t cdclk_khz, cdclk_vco_khz, cdclk_ref_khz, cdclk_bypass_khz, cdclk_max_khz;
	uint8_t cdclk_voltage_level;
	/* memory bandwidth (MB/s) of the QGV point the initialisation left allowed (SAGV is kept off); 0 = unknown = refuse */
	uint32_t qgv_allowed_bw;
	uint32_t dmc_fw_mask;           /* bit n: DMC firmware id n is loaded (1 = pipe A, 2 = pipe B ...; from the DMC loader) */
	int vbt_override_afc_startup;   /* VBT general feature; the value is already in the state's pll.div0 */
	/* the framebuffer to show: what the scanout object reports */
	uint32_t fb_fourcc;
	uint64_t fb_modifier;
	uint32_t fb_width, fb_height, fb_pitch, fb_surf;
};

/*
 * What a modeset run reports: how far it got and what it holds.
 */
struct i915_lcd_modeset_status {
	int prepared, crtc_active, plane_armed;
	/* link */
	int link_rate, lane_count;
	int link_trained_flag;          /* intel_dp->link_trained: set by intel_dp_stop_link_train() whatever the outcome */
	int link_status_rc;             /* reading DPCD 0x202.. after the enable: 0, or a positive errno */
	uint8_t link_status[6];
	int cr_ok, eq_ok;               /* drm_dp_clock_recovery_ok / drm_dp_channel_eq_ok on that readout */
	uint8_t train_set[4];
	uint32_t ddi_buf_ctl_value;     /* intel_dp->DP */
	/* backlight (PWM): what intel_backlight_setup() derived and whether it is on */
	int backlight_present, backlight_enabled, backlight_setup_rc;
	uint32_t backlight_pwm_max, backlight_level;
	/* watermarks / DDB computed for the plane (software range [start, end) in DDB blocks) */
	int wm_rc;
	uint16_t ddb_start, ddb_end;
	uint16_t wm0_blocks, wm0_lines; int wm0_enable;
	uint8_t dbuf_slices_wanted; int mbus_joined;
	/* the commit's outer part */
	int cdclk_rc, cdclk_crtc_min, cdclk_bw_min, cdclk_required_khz, cdclk_required_vco, cdclk_required_level, cdclk_change_needed;
	unsigned bw_data_rate;          /* MB/s this crtc needs */
	int dc_off_held;                /* POWER_DOMAIN_DC_OFF is held (inside a commit, or kept after an unconfirmed stop) */
	unsigned crtc_domains_held;     /* how many domains of get_crtc_power_domains() the crtc holds */
	uint8_t dbuf_slices_now; int mbus_joined_now;   /* the current global DBUF state */
	int stop_unconfirmed, retained;
	int dither;
	uint32_t cur_surf, pend_surf;       /* displayed / pending (valid while flip_pending) */
	int flip_pending, flip_stuck; unsigned flip_gen;
	int flip_event_ref;                     /* the pending event still holds its vblank reference */
	unsigned events_cancelled;              /* events settled by the stop path (drm_crtc_vblank_off) */
	uint32_t backlight_min;         /* panel->backlight.min (hw units) */
	uint32_t backlight_max;
	uint32_t backlight_user, backlight_user_max;   /* the user brightness (restore THIS, not the hw level) */                     /* crtc state: derived from pipe_bpp (intel_modeset_pipe_config) */
	unsigned commits;
	/* ownership */
	int pll_on, pll_active_mask, pll_wakeref;
	int pll_id;                     /* the shared DPLL this crtc was given (0 / 1) */
	int pll_pipe_mask;              /* the pipes that hold a reference on it */
	int ddi_io_wakeref, aux_wakeref;
	/* the reference's drm_err / WARN lines since prepare */
	unsigned errors;
	const char *first_error;
};

/* results of _enable / _disable */
#define I915_LCD_MS_OK 0
#define I915_LCD_MS_NOT_PREPARED (-1)
#define I915_LCD_MS_ERRORS (-2)            /* the reference reported an error (status.first_error) */
#define I915_LCD_MS_LINK_NOT_TRAINED (-3)  /* the sink's link status does not show CR + EQ + symbol lock + alignment */
#define I915_LCD_MS_STILL_OWNED (-4)       /* after the disable something is still held (status says what) */

/*
 * Flip the running picture to another buffer (same format / size / pitch): the reference's update of a running crtc
 * (plane noarm, intel_pipe_update_start -- vblank evasion --, plane arm, intel_pipe_update_end -- the event is armed).
 * COMPLETE only when the event completed AND the pipe's live surface (PLANE_SURFLIVE) is the new buffer; then the old
 * buffer is no longer displayed.  Otherwise both stay protected and further flips are refused (flip_stuck) until the
 * display is stopped.  One flip pending at a time; one display owner.
 */
struct i915_lcd_flip_result {
	unsigned gen;
	uint32_t old_surf, new_surf, live_before, live_after;
	uint32_t frame_before, frame_after;
	int event_rc;
	int update_errors;                      /* reference errors during the update (e.g. "Atomic update failure") */
	int result;                             /* I915_LCD_FLIP_* */
};
#define I915_LCD_FLIP_DONE        0       /* the new buffer is displayed; the old one is released */
#define I915_LCD_FLIP_NOT_LATCHED 1       /* the event came but the live surface is not the new one: both kept */
#define I915_LCD_FLIP_TIMEOUT     2       /* no completion: both kept */
#define I915_LCD_FLIP_REFUSED     3       /* nothing written (not running, a flip stuck, same buffer, bad address) */
#define I915_LCD_FLIP_ARMED       4       /* armed without waiting: latches at the next vblank, both kept until then */

/*
 * The one picture on the panel (modeset.c).
 */
/*
 * Linux-derived -- LCD-B: show ONE known picture on the panel and stop safely.  zedBSD project code.
 *
 * This is the production body shared by the real GPU (modeset.c) and the GPU-free kernel test
 * (the display tests): the scanout object's life (create -> pin -> draw -> publish -> begin ... end -> unpin ->
 * destroy, or ABANDON), the two commits of the modeset object, the observation between them, and the decision
 * whether the buffer may be given back.  Only the backend (i915_lcd_ops) and the GGTT behind the gt_mem differ.
 *
 * Three records are kept APART, because they answer different questions:
 *   first anomaly     the first thing that went wrong, where, and how far the run had got -- never overwritten
 *                     by anything the cleanup reports afterwards;
 *   enabled state     what was really switched on when the enable commit returned (software state + what the sink
 *                     and the frame counter said);
 *   cleanup result    what the disable commit and the stop confirmation returned, and what is still held.
 * And three kinds of evidence are not mixed: software flags ("the arm was written"), hardware observation (frame
 * counter, underrun status, sink link status) and -- outside this code -- the photograph of the panel.
 */

enum i915_lcd_show_stage {
	I915_LCD_SHOW_NONE = 0,
	I915_LCD_SHOW_BUFFER_READY,           /* scanout pinned, picture drawn, published and read back */
	I915_LCD_SHOW_PREPARED,               /* the modeset's check phase accepted the state */
	I915_LCD_SHOW_ENABLE_RETURNED,        /* the enable commit returned (successfully or not) */
	I915_LCD_SHOW_PICTURE_UP,             /* enable OK and the frame counter advances */
	I915_LCD_SHOW_WINDOW_DONE,            /* the finite observation window passed */
	I915_LCD_SHOW_DISABLE_RETURNED,
	I915_LCD_SHOW_STOP_CONFIRMED,         /* pipe reported off AND the frame counter stands */
	I915_LCD_SHOW_RELEASED,               /* buffer unpinned and destroyed */
	I915_LCD_SHOW_ABANDONED               /* stop not confirmed: buffer and everything it needs kept for ever */
};

/*
 * What one picture on the panel needs: the modeset configuration, the
 * hooks, the GPU memory and the panel.
 */
struct i915_lcd_show_env {
	struct i915_lcd_emit *hw;             /* the backend */
	struct i915_gt_mem *gm;               /* where the scanout buffer lives */
	struct i915_scanout *so;              /* caller-owned storage: it must outlive an abandoned buffer */
	const struct i915_lcd_state *lcd;     /* LCD-A: mode, link, M/N, PLL words (from the resident eDP's DPCD / EDID) */
	struct i915_lcd_modeset_cfg cfg;      /* everything but fb_*: filled by the caller from ITS sources */
	int pipe;
	unsigned pattern_id;
	uint64_t pattern_fnv;                   /* pinned hash of that picture at this size; 0 = do not compare */
	unsigned first_frames_ms;               /* how long to wait for the first frames */
	unsigned window_ms;                     /* the finite observation window (for the camera) */
	/* optional: called when a stage is reached (register readbacks for the log; fault injection in tests) */
	/*
	 * optional: a test that runs while the picture is up (after the first frames, before the window); 0 = passed.
	 * A failure is recorded as the first anomaly (if none yet) and the reference stop path follows.
	 */
	int (*in_window)(void *ctx, struct i915_lcd_observer *o);
	void *in_window_ctx;
	void (*at_stage)(void *ctx, int stage);
	void *at_stage_ctx;
};

/*
 * What one picture on the panel did: stages, samples and what was given
 * back.
 */
struct i915_lcd_show_report {
	int output_hdmi;                        /* the run drove an HDMI sink: no DP link to judge it by */
	int stage;                              /* the furthest stage reached */
	/* first anomaly */
	const char *first_anomaly;              /* 0 = none */
	int first_anomaly_stage;
	int first_anomaly_rc;
	int first_error_trace_at;               /* index in the run log of the first reference error; -1 = none */
	/* return codes, in order */
	int window_rc, create_rc, pin_rc, prepare_rc, begin_rc, enable_rc, first_frames_rc, steady_rc, disable_rc, stopped_rc;
	int unpin_rc, destroy_rc;
	/* enabled state / cleanup result */
	struct i915_lcd_modeset_status at_enable, at_window_end, at_disable;
	unsigned enable_errors, cleanup_errors;
	int cleanup_first_error_trace_at;
	/* hardware observation */
	uint32_t frame_first, frame_last, steady_frame_first, steady_frame_last, stop_frame_first, stop_frame_last;
	unsigned steady_rounds;
	int window_hook_rc;
	uint32_t transconf_after_stop;
	/* the buffer */
	uint64_t surf;
	uint64_t pattern_hash;
	uint32_t readback_bad_before, readback_bad_after;
	int released, abandoned;
	int display_acquired;                   /* the buffer was handed to the display (the enable commit was attempted) */
	int display_released;                   /* the display has provably stopped reading it (back to PINNED) */
	struct i915_lcd_observer obs;
	struct i915_lcd_trace *trace;         /* the run log (static storage of the show body) */
	int pass;
};

/*
 * The panel runs (modeset.c, present.c).
 */
/*
 * A panel run on the real GPU: one known picture on the panel, a finite
 * observation window, the reference's stop path, everything given back.
 */

struct i915_edp_device;
struct i915_display_state;
struct i915_dmc_dev;
struct i915_irq_dev;
struct i915_display_irq;
struct i915_gt_engines;
struct i915_gt_ppgtt;
struct spinlock;

/* the objects of the normal initialisation this run reads and uses; none is copied, none is re-created */
struct i915_driver_probe;
/*
 * The objects of the normal initialisation a panel run reads and uses;
 * none is copied, none is re-created.
 */
struct i915_lcd_kernel_deps {
	struct i915_edp_device *edp;          /* resident panel: DPCD / EDID / LCD-A state, PPS, AUX, tick sleeps, VBT */
	struct i915_mmio *mmio;
	struct i915_power_domains *pd;
	struct i915_pw_ctx *pwc;
	struct i915_display_core *dcore;      /* DBUF slices */
	struct i915_cdclk_dev *cdclk;
	struct i915_display_nogem *nogem;     /* watermark latencies, SAGV block time */
	struct i915_display_state *dstate;    /* the bandwidth object: QGV mask */
	struct i915_bw_state *bw;             /* QGV / PSF bandwidth table */
	struct i915_dmc_dev *dmc;
	/*
	 * The display half of the interrupt device (vblank references and
	 * waits).  It was the whole interrupt device before the GT half moved
	 * to struct i915_irq_dev.
	 */
	struct i915_display_irq *irq;
	struct i915_gt_mem *gm;
	int ipc_enabled;                        /* skl_watermark_ipc_init()'s result */
	/* LCD-G only: the GT the draw is submitted to (forcewake is held by the caller) */
	struct i915_gt_engines *es;
	struct i915_gt_ppgtt *vm;
	struct spinlock *uncore_lock;
	/* the CPU-mappable aperture, and the probe state whose INIT reference the run returns */
	uint64_t gmadr_base, gmadr_size;
	struct i915_driver_probe *dprobe;
};
/* the release decision after a draw and (maybe) a show -- exported for the GPU-free test */
struct i915_fhd_render;
struct i915_scanout;
struct i915_gt_tlb;

/* what the runner reports next to the probe result (the probe outcome itself is not changed by the LCD test) */
struct i915_lcd_test_summary {
	int ran, pass;
	const char *stage;                      /* furthest stage */
	const char *first_anomaly, *first_anomaly_stage;
	int cleanup_rc;                         /* the disable commit's result */
	unsigned cleanup_errors;
	int retained;                           /* resources kept because the stop was not confirmed */
};

/*
 * The resident eDP device (dp-sink.c).
 */
/* the env's kernel backend: real mutexes, tick-driven sleeps, timer + worker threads */
struct i915_dp_kernel {
	struct i915_mmio *mmio;
	struct i915_power_domains *pd;
	struct i915_pw_ctx *pwc;
	struct mutex locks[2];                  /* I915_DP_LOCK_* */
	struct i915_workqueue wq;            /* the display worker: VDD-off and async power put */
	struct i915_timer_queue tq;
	struct i915_delayed_work vdd_off_work;
	struct i915_delayed_work async_put_work;
	int sync_started;
	unsigned wait_timeouts, time_faults;
	unsigned tick_sleeps;                   /* ordinary sleeps: tick + wait queue (kern_usleep_range) */
	unsigned busy_sleeps;                   /* always 0 since no busy remainder is left */
	uint64_t tick_slept_us, busy_slept_us;
	uint64_t last_ms;
};

/*
 * The resident eDP panel: its AUX and PPS environment, the locks and the
 * delayed work, and what the bring-up found.
 */
struct i915_edp_device {
	int started;                            /* locks and threads exist */
	int connector_live;                     /* the normal initialisation succeeded; state retained */
	int init_rc, late_rc;
	struct i915_dp_kernel k;
	struct i915_dp_env env;
	struct i915_edp_config cfg;
	struct i915_edp_result res;           /* kept for the later display stages */
	struct i915_lcd_state lcd;            /* LCD-A: mode, link, M/N, PLL words computed from `res` */
	int lcd_rc;
	struct i915_lcd_words lcd_words;      /* transcoder A M/N, timing and pipe-source words (computed, not written) */
	int lcd_words_rc;
	struct i915_lcd_words lcd_cpu_words;  /* hsw_configure_cpu_transcoder's operations (computed, not written) */
	struct i915_lcd_words lcd_ddi_words;  /* TRANS_MSA_MISC, TRANS_DDI_FUNC_CTL2, TRANS_DDI_FUNC_CTL (computed, not written) */
	uint32_t lcd_ddi_buf_ctl;               /* intel_dp->DP as intel_ddi_init_dp_buf_reg leaves it */
	uint32_t ddi_buf_ctl_readout;           /* DDI_BUF_CTL of the eDP port when the connector was initialised */
	int lcd_cpu_words_rc, lcd_ddi_words_rc;
	int vbt_bpp;
	struct i915_vbt_state *vbt;
};

/*
 * The hotplug (hotplug.c).
 */
/*
 * Linux-derived -- the HDMI hotplug receive path, the zedBSD side of the generated reference text.
 * zedBSD project code.  No Linux types here: device.c, interrupts.c and the tests use this interface.
 *
 *   start   intel_hpd_init_early() + the connectors intel_setup_outputs() would have made (one per encoder: eDP-1,
 *           HDMI-A-1, DP-1, DP-2), each encoder's ->hotplug = intel_ddi_hotplug and ->connected =
 *           lpt_digital_port_connected / TC (the latter recorded as unported), then the IRQ entry is opened
 *   irq     from gen8_de_irq_handler(): the acked SDEIIR value -> icp_irq_handler() (IRQ context)
 *   stop    the IRQ entry is closed, in-flight entries drained, then intel_hpd_cancel_work()
 */

struct i915_hotplug;

#define I915_HPD_MAX_CONNECTORS 8u
#define I915_HPD_MAX_IRQ_RECORDS 64u
#define I915_HPD_MAX_HOTPLUG_RECORDS 64u

/*
 * The register model a test starts the hotplug path on instead of the MMIO
 * BAR.  Its layout belongs to the test build (tests/display/hpd-model.h);
 * production only passes the pointer on.
 */
struct i915_hpd_fake_regs;

/* the last EDID read over DDC (drm_edid_read_ddc) */
struct i915_hpd_edid_info {
	int rc;                         /* blocks read (>= 1) or a negative errno */
	unsigned blocks, extensions, product, serial, version, revision, digital, checksum;
	unsigned pixel_clock_khz, hactive, vactive;     /* the first detailed timing descriptor */
	unsigned reads, fails;
	char mfg[4];
};

/*
 * One hotplug interrupt as the hotplug path recorded it.
 */
struct i915_hpd_irq_record {
	uint64_t tick;
	uint32_t sde_iir;               /* the acked SDEIIR value handed to icp_irq_handler() */
	uint32_t shotplug_ddi;          /* what its rmw of SHOTPLUG_CTL_DDI read (0 when the DDI trigger was clear) */
	uint32_t event_bits_after;      /* display.hotplug.event_bits when the handler returned */
};

/* one encoder->hotplug() call of the hotplug work (the reference's "Connector %s (pin %i) received hotplug event") */
struct i915_hpd_hotplug_record {
	uint64_t tick;
	unsigned connector;
	int pin, retries;
	int old_status, new_status;     /* enum connector_status (1 connected, 2 disconnected, 3 unknown) */
	int state;                      /* enum intel_hotplug_state (0 UNCHANGED, 1 CHANGED, 2 RETRY) */
	int live;                       /* SDEISR & pch_hpd[pin] at the detection */
	int edid_rc;                    /* the EDID read of this detection: blocks read, or a negative errno */
	unsigned edid_blocks;
};

/*
 * What the hotplug path saw and did.
 */
struct i915_hpd_summary {
	int started, live;
	unsigned num_connectors;
	unsigned irq_entries, irq_dropped, ddi_triggers, gmbus_irqs;
	unsigned hotplug_works, digport_works, reenable_works, retries_armed;
	unsigned hpd_pulse_steps, hotplug_events, irq_setups, storms, warnings;
	unsigned irq_records, hotplug_records;
	unsigned to_connected, to_disconnected;     /* HDMI-A connector status transitions */
	int hdmi_connector;                         /* index, -1 = none */
	int hdmi_status;                            /* enum connector_status, now */
	unsigned long long hdmi_epoch;
};

/*
 * The OpRegion (opregion.c).
 */
/*
 * Linux-derived -- the OpRegion service instance as seen from outside the extracted text.  zedBSD project code.
 * Backend SHADOW only (driver-owned RAM in the OpRegion format); production runs VBT_ONLY and never joins the
 * firmware's runtime protocol.
 */

struct i915_workqueue;

/* the sanitized encoder of the readout (N1): port = enum port, output_type = enum intel_output_type */
#define I915_OUTPUT_ANALOG  1
#define I915_OUTPUT_DP      7
#define I915_OUTPUT_EDP     8
#define I915_OUTPUT_DSI     9
#define I915_OUTPUT_DDI     10
#define I915_OUTPUT_HDMI    6
#define I915_OUTPUT_DP_MST  11

/* the worker queue, the backlight policy (acpi_video_get_backlight_type), the connectors */
#define I915_OPREGION_POLICY_VIDEO  1     /* acpi_backlight_video: BCLP requests are served */
#define I915_OPREGION_POLICY_VENDOR 2
#define I915_OPREGION_POLICY_NATIVE 3     /* acpi_backlight_native: the reference ignores ASLE backlight requests */

/*
 * The takeover report (takeover.c).
 */
/*
 * Linux-derived -- N1: the display the FIRMWARE left running.
 *
 * Two steps, each the reference's own text (intel_modeset_setup.c):
 *   readout   intel_modeset_setup_hw_state(): what the hardware says, and the sanitize the reference runs on it.
 *             Nothing of the firmware's picture changes.
 *   takeover  intel_crtc_disable_noatomic() for every crtc the readout found active: the firmware's picture
 *             stops and the pipe is the driver's.
 * After the takeover the caller lights the panel again through the ordinary modeset path; pointing that
 * plane at the firmware's own framebuffer keeps the console readable on a machine whose only console is
 * the screen.
 */

struct i915_lcd_modeset_cfg;
struct i915_lcd_emit;

/*
 * What the firmware display takeover found and stopped.
 */
struct i915_n1_report {
	int live;                       /* the registry is built */
	unsigned active_pipes;          /* BIT(pipe) for every pipe the readout found active */
	int pipe;                       /* the first active pipe, or -1 */
	int cpu_transcoder;             /* of that pipe */
	int dpll_id;                    /* the shared DPLL that pipe uses, or -1 */
	int port;                       /* the port of the bound encoder */
	unsigned mode_h, mode_v, clock_khz, port_clock_khz;
	int pipe_bpp;
	unsigned output_types;
	unsigned plane_visible;         /* the primary plane of that pipe scans out */
	unsigned active_planes;
	int encoder_on_crtc;            /* the readout linked the encoder to a crtc */
	int connector_dpms;             /* 0 = on, 3 = off */
	unsigned takeovers;             /* crtcs stopped by the takeover */
	unsigned still_active;          /* BIT(pipe) of what the takeover did NOT stop (0 = all stopped) */
};

/*
 * The resident node (device.c, display.c).
 */
/*
 * Resident mode: the device start stays up and serves /dev/gpuN.
 *
 * The start does not return while it serves: it publishes the GPU node,
 * executes the requests the ops layer queues, and when told to stop it falls
 * into the device stop.  What the serving thread needs lives in the display
 * (struct i915_display), so nothing has to stay on the start's stack.
 */

struct i915_lcd_kernel_deps;

/*
 * What the resident node's serving thread needs: the device, the
 * registers, the GT memory and engines, and the panel.
 */
struct i915_resident_ctx {
	struct i915_device *device;
	struct i915_mmio *mmio;
	struct spinlock *uncore_lock;
	struct i915_gt_mem *gm;
	struct i915_gt_engines *es;
	const struct i915_lcd_kernel_deps *lcd;	/* the panel (0 = the node has no display) */
};

/*
 * one frame the GPU copies into the panel's back buffer.  The serving thread maps both panel buffers into
 * `vm` (once), asks `build` for a batch that writes the frame into the back buffer (dst_va is the buffer's address
 * in `vm`), runs it in `context` and flips.  No CPU touches the pixels.
 */
struct i915_context;
struct i915_ppgtt;
typedef int (*i915_shim_blit_fn)(void *ctx, uint64_t dst_va, uint32_t width, uint32_t height, uint32_t pitch,
	uint64_t *batch_va);

/*
 * The resident display operations (display.c).
 */
/*
 * the display operations of the resident node (display.c).
 */

struct drv_gpu_display_ops;
struct drv_gpu_scanout_ops;


/*
 * The display half of the interrupt state.
 */

/* The pipes the display interrupt state has room for (pipes A to D). */
#define I915_IRQ_MAX_PIPES 4

/*
 * The vblank delivery of the pipes.
 *
 * A minimal stand-in for the Linux DRM vblank core: per pipe, the number
 * of vblank interrupts seen while enabled, a completion that wakes the
 * one waiter, and the enable reference that bdw_enable_vblank() and
 * bdw_disable_vblank() keep.  The display owns one; it is initialized
 * before the interrupt handler is installed and lives as long as the
 * display.
 */
struct i915_irq_vblank {
	/* Protects every field below (the Linux dev_priv->irq_lock). */
	struct spinlock lock;

	/* Nonzero once the lock and the completions are initialized. */
	int inited;

	/* Wakes the waiter of a pipe on its next vblank. */
	struct i915_completion wake[I915_IRQ_MAX_PIPES];

	/* Vblank interrupts handled while the pipe's delivery was enabled. */
	volatile uint32_t count[I915_IRQ_MAX_PIPES];

	/* Nonzero while the pipe's vblank interrupt is enabled (drm vblank->enabled). */
	volatile int enabled[I915_IRQ_MAX_PIPES];

	/* drm_vblank_get() references held per pipe; the last put disables at once. */
	unsigned refs[I915_IRQ_MAX_PIPES];

	/* Enable and disable calls per pipe, for the diagnostics. */
	unsigned enable_calls[I915_IRQ_MAX_PIPES], disable_calls[I915_IRQ_MAX_PIPES];

	/* The power-well hooks, observed. */
	unsigned post_enable_calls, pre_disable_calls, skipped_irqs_disabled;

	/* The pipe masks the power-well post-enable hook wrote. */
	uint32_t post_imr[I915_IRQ_MAX_PIPES], post_ier[I915_IRQ_MAX_PIPES];

	/* Waits that timed out or found the time base broken. */
	unsigned sync_calls, sync_timeouts, sync_time_faults, drain_timeouts, drain_time_faults;

	/* Nonzero while a waiter sleeps on the pipe; one waiter per pipe. */
	int waiting[I915_IRQ_MAX_PIPES];

	/* Waits refused because the pipe already had a waiter. */
	unsigned second_waiter_refusals;
};

/*
 * The display half of the interrupt device.
 *
 * The GT half and the top-level handler live in struct i915_irq_dev
 * (../irq.h), which reaches this structure as its display context through
 * struct i915_irq_display_ops.  The fields are the display fields of the
 * old single interrupt device, under their old names; the handler writes
 * the counters, the rest is set before install and read afterwards.
 */
struct i915_display_irq {
	/* The interrupt device whose display half this is. */
	struct i915_irq_dev *irq;

	/* The register access of the device. */
	struct i915_mmio *m;

	/* The power wells, which enable and disable pipe interrupts as they change. */
	struct i915_power_domains *pd;
	struct i915_pw_ctx *pwc;

	/* The south display block the reset and the postinstall reach. */
	const struct i915_pch_state *pch;

	/* The display version, the runtime pipe and transcoder masks, and whether there is a display. */
	int display_ver;
	unsigned pipe_mask;
	unsigned cpu_transcoder_mask;
	int has_display;

	/* intel_bios_is_dsi_present(), which selects the DSI port interrupts. */
	int dsi_present;

	/* The pipe interrupt masks the reference keeps under irq_lock. */
	uint32_t de_irq_mask[I915_IRQ_MAX_PIPES];

	/* The masks the postinstall computed, kept for the diagnostics. */
	uint32_t de_pipe_masked, de_pipe_enables;
	uint32_t de_port_masked, de_port_enables;
	uint32_t de_misc_masked;

	/*
	 * Display source accounting.  The handler acknowledges every asserted
	 * source -- an enabled source left set would re-assert forever -- and
	 * counts what it saw; "lied" counts a master bit with an empty IIR.
	 */
	volatile unsigned de_misc_acks, de_hpd_acks, de_port_acks, de_pch_acks;
	volatile unsigned de_pipe_iir_acks[I915_IRQ_MAX_PIPES];
	volatile unsigned de_vblank_count[I915_IRQ_MAX_PIPES];
	volatile unsigned de_flip_done_count, de_underrun_count, de_fault_count;
	volatile unsigned de_lied_count;

	/* The last display interrupt control and pipe IIR values the handler read. */
	volatile uint32_t last_disp_ctl;
	volatile uint32_t last_de_pipe_iir[I915_IRQ_MAX_PIPES];

	/*
	 * Per-pipe admission.  The handler enters a pipe's registers only while
	 * the pipe is open and counts itself in pipe_inflight; the power-well
	 * pre-disable closes the pipe and drains that count instead of relying
	 * on a whole-handler synchronize.
	 */
	volatile int pipe_closed[I915_IRQ_MAX_PIPES];
	volatile unsigned pipe_inflight[I915_IRQ_MAX_PIPES];

	/* Master bits seen while the pipe was closed. */
	volatile unsigned pipe_refused[I915_IRQ_MAX_PIPES];

	/* The vblank delivery; NULL means vblank delivery is not bound. */
	struct i915_irq_vblank *vbl;
};

/*
 * The resident present path.
 */

/*
 * The display lease of the resident node.
 *
 * One session at a time holds the panel; a presentation names the lease it
 * was given, so a stale presentation after a release is refused.
 */
struct i915_resident_display {
	/* Serializes the lease, the sequence and the active flag. */
	struct mutex mutex;

	/* Nonzero once the mutex is initialized. */
	int inited;

	/* The session holding the lease; NULL while the panel is free. */
	void *owner;

	/* The lease the owner was given, and the one the next claim gets. */
	uint64_t lease;
	uint64_t next_lease;

	/* Completed presentations of the current lease. */
	uint64_t sequence;

	/* The tick of the last completed presentation. */
	uint64_t present_tick;

	/* Nonzero while the panel shows this node's frames. */
	int active;
};

/*
 * The serving thread's side of the display window.
 *
 * The request worker runs inside the display window while the panel is up
 * (the display enable calls back into the serving loop).  These are the
 * display fields of the old serving state; only the serving thread writes
 * them, and it reads display_failed under the device IRQ lock when it
 * decides whether a presentation enters the window.
 */
struct i915_present_window {
	/* Nonzero while the serving thread is inside the display window. */
	int display_up;

	/* Nonzero once bringing the panel up failed: presentations fail from then on. */
	int display_failed;

	/* Presentations completed. */
	unsigned presents;

	/*
	 * The panel buffers as the GPU sees them: mapped into one address
	 * space at a time, the one of the session presenting.  NULL while no
	 * address space holds them.
	 */
	struct i915_ppgtt *map_vm;
	uint64_t map_va[2];
	unsigned map_pages[2];
};

/*
 * One run of the panel from the resident path or a scenario.
 *
 * The run's parameters live with the modeset code; the rest is what the
 * run took (power references, events) and what it saw, kept so the
 * teardown gives back exactly what the run holds.
 */
struct i915_lcd_run_params;

struct i915_lcd_kernel {
	/* The parameters of the run; NULL before it starts. */
	const struct i915_lcd_run_params *p;

	/* What the run drives. */
	const struct i915_lcd_kernel_deps *d;

	/* The two modeset locks (dpll, backlight): lcdb_locks of the display. */
	struct mutex *locks;
	int locks_live;

	/* The register, wait, panel, power and vblank hooks of the run. */
	struct i915_lcd_emit ops;

	/* Power references this run took and has not returned, per domain. */
	int power_refs[I915_PW_DOMAIN_NUM];

	/* What went wrong, counted. */
	unsigned power_get_failures, wait_timeouts, time_faults, unresolved_steps, decided, errors;

	/* Nonzero once the run is in its cleanup. */
	int phase_cleanup;

	/* The picture and the timing of the run. */
	unsigned pattern_id, window_ms, post_before, pre_before, vbt_min;

	/* The backlight PWM frequency found at the start. */
	uint32_t bl_freq0;

	/* The interrupt and vblank event state of the run. */
	int irq_was_on, event_armed, event_pipe;
	uint32_t event_frame;
	unsigned vblank_sleeps, sleep_irq_off, events_cancelled;

	/* The two flip buffers and what the flips did. */
	struct i915_scanout *flip_a, *flip_b;
	unsigned flips_done, draws_ok, probe_flips, cross_ok;

	/* Set when the disable commit starts; errors are counted apart from then on. */
	int probe_watch;
	uint32_t probe_first_dsl;
	unsigned cleanup_errors;
};

/* The size of the VBT copy the parser reads (the largest VBT the provider accepts). */
#define I915_VBT_MAX 8192u

/* The size of the DMC firmware arena (the Linux DISPLAY_VER13_DMC_MAX_FW_SIZE). */
#define I915_DMC_ARENA_SIZE 0x20000u

/* The size of the VBT copy the firmware display check takes out of the OpRegion. */
#define I915_NATIVE_VBT_COPY_SIZE 16384u

/*
 * The display part of one device.
 *
 * The device owns exactly one; the device start fills it in the order of
 * the Linux display probe and the device stop takes it apart in reverse.
 * It holds every long-lived display object: the former static locals of
 * the probe, the former file-scope state of the display files, and one
 * pointer per Linux environment to that environment's own state (whose
 * types are complete only inside the environment, so the owner file of
 * the environment allocates it).
 *
 * Unless a field says otherwise it is written by the device start or stop
 * only, which run on the start worker one at a time; after the start the
 * resident serving thread is the only other writer.
 */
struct i915_lcd_world;
struct i915_wm_world;
struct i915_takeover_world;
struct i915_dp_world;
struct i915_vbt_world;
struct i915_hpd_world;
struct i915_opregion_world;

struct i915_display {
	/* The device this display belongs to. */
	struct i915_device *device;

	/*
	 * Probe state (the probe's static locals, device lifetime).
	 */

	/* The DRM device management and the per-pipe vblank workers (drm_dev_init, drm_vblank_init). */
	struct i915_drm_device drm_dev;

	/* The VBT the parser was given and what it found (intel_bios_init). */
	struct i915_vbt_state vbt_state;

	/* The VGA arbiter client (intel_vga_register). */
	struct i915_vga_client vga_client;

	/* The power wells and domains; its own lock protects the reference counts. */
	struct i915_power_domains power_domains;

	/* The PM demand lock and wait queue (intel_pmdemand_init_early). */
	struct i915_pmdemand pmdemand;

	/* The CDCLK state and hooks. */
	struct i915_cdclk_dev cdclk;

	/* The context the power-well operations run in (MMIO, interrupt hooks, DMC). */
	struct i915_pw_ctx pwc;

	/* The resident eDP panel: outlives the output setup, released in the stop. */
	struct i915_edp_device edp_dev;

	/* The display core (DBUF slices, the init power reference). */
	struct i915_display_core dcore;

	/* The DMC loader; its work runs on dmc_wq. */
	struct i915_dmc_dev dmc_dev;

	/* The display software state (mode config, global objects, FBC). */
	struct i915_display_state dstate;

	/* The PCH the display found (intel_detect_pch). */
	struct i915_pch_state pch;

	/* The crtc, plane, DPLL and encoder records the output setup and the readout build. */
	struct i915_display_nogem nogem;

	/* The Linux driver-probe state after the noirq stage (hotplug, registration, IPC). */
	struct i915_driver_probe dprobe;

	/* The DRAM the bandwidth code was given, and the bandwidth state (intel_bw_init_hw). */
	struct i915_dram_info dram_info;
	struct i915_bw_state bw_state;

	/* The display half of the interrupt device and its vblank delivery. */
	struct i915_display_irq irq;
	struct i915_irq_vblank irq_vblank;

	/* The DMC, modeset and flip workqueues (intel_modeset_wq, intel_flip_wq). */
	struct i915_workqueue dmc_wq, modeset_wq, flip_wq;

	/* The OpRegion data read at the start (read-only acquisition); p2_opd points at opd or is NULL. */
	struct i915_opregion_data opd;
	const struct i915_opregion_data *p2_opd;

	/* Nonzero when the start found an OpRegion, and one with a usable VBT. */
	int opregion_present;
	int opregion_vbt_present;

	/* The firmware display the start found before its first display write, and the MMIO it read with. */
	struct i915_native_report n0;
	struct i915_mmio *n0_mmio;

	/* Nonzero once the display core was initialized (intel_power_domains_init_hw). */
	int display_core_inited;

	/* Nonzero while the hotplug path runs (started after the display probe). */
	int hpd_started;

	/* The resident run: what the serving thread needs, and the panel dependencies it uses. */
	struct i915_resident_ctx rctx;
	struct i915_lcd_kernel_deps rlcd;

	/*
	 * The resident present path.
	 */

	/* The display lease; its own mutex protects it. */
	struct i915_resident_display rd;

	/* The serving thread's side of the display window. */
	struct i915_present_window window;

	/* The panel run of the resident path; written by the serving thread only. */
	struct i915_lcd_kernel lk;

	/* The dpll and backlight locks of the panel runs; lcdb_locks_live once initialized. */
	struct mutex lcdb_locks[2];
	int lcdb_locks_live;

	/*
	 * The two full-panel buffers of the resident path.  They outlive a run
	 * whose stop was not confirmed; the serving thread flips between them.
	 */
	struct i915_scanout resident_buf[2];

	/* The serving loop the display window calls back, and its context. */
	int (*resident_serve)(void *ctx);
	void *resident_serve_ctx;

	/* Which resident buffer is on the panel, and whether the panel is up. */
	unsigned resident_front;
	int resident_up;

	/* The show environment and report of the resident run (large, kept off the stack). */
	struct i915_lcd_show_env resident_run_env;
	struct i915_lcd_show_report resident_run_rep;

	/* The panel data the run configuration was filled from (kept off the stack). */
	struct i915_vbt_panel fill_cfg_pn;

	/* Nonzero logs every register access of a panel run (a diagnostic switch). */
	int i915_lcd_reg_trace;

	/*
	 * What a run kept because its stop was not confirmed: the modeset
	 * trace, the hooks of the hardware left running, and the GPU buffer the
	 * GPU was not shown to be done with.  The stop reads them to decide
	 * what it may release.
	 */
	struct i915_lcd_trace show_trace;
	int show_retained;
	const struct i915_lcd_emit *show_retained_hw;
	int show_gpu_retained;
	const void *show_gpu_retained_gm;
	const char *show_gpu_retained_why;

	/*
	 * Former file-scope state of the world-neutral display files.
	 */

	/* The VBT bytes the parser reads, the OpRegion's VBT, and the pinned VBT's PCI ids (vbt.c; the ids only in the test build). */
	uint8_t i915_vbt_buf[I915_VBT_MAX];
	const void *opregion_vbt_buf;
	size_t opregion_vbt_size;
	uint16_t pinned_vendor, pinned_device;

	/* The DMC firmware image as loaded, and how much of it is used (dmc.c). */
	uint8_t g_dmc_arena[I915_DMC_ARENA_SIZE];
	unsigned g_dmc_arena_used;

	/* The display core's own trace (power.c). */
	struct i915_trace dc_trace;

	/* The last firmware-display report, and the OpRegion and VBT copies the check reads (takeover.c). */
	const struct i915_native_report *n0_last;
	uint8_t read_opregion_copy[I915_OPREGION_SIZE];
	uint8_t read_opregion_vcopy[I915_NATIVE_VBT_COPY_SIZE];
	uint8_t i915_opregion_read_data_opcopy[I915_OPREGION_SIZE];
	uint8_t i915_opregion_read_data_vbtcopy[I915_NATIVE_VBT_COPY_SIZE];

	/* The ACPI notifier chain of the OpRegion service; chain_lock protects chain_head (opregion.c). */
	struct mutex chain_lock;
	int chain_lock_live;
	struct notifier_block *chain_head;

	/* The last ISA bridge the PCH search looked at (device-info.c). */
	struct drv_pci_device *next_isa_bridge_cursor;

	/* The VGA I/O port operations (takeover.c); NULL until the VGA part is bound. */
	const struct i915_vga_io_ops *g_vga_io;

	/*
	 * The Linux environments' own state.  Each is allocated and freed by
	 * the owner file of its environment and is NULL outside that window.
	 */
	struct i915_lcd_world *lcd_world;
	struct i915_wm_world *wm_world;
	struct i915_takeover_world *takeover_world;
	struct i915_dp_world *dp_world;
	struct i915_vbt_world *vbt_world;
	struct i915_hpd_world *hpd_world;
	struct i915_opregion_world *opregion_world;

	/*
	 * The stages of the display start that completed, so the stop takes
	 * apart exactly what the start built (the probe's local flags).
	 */
	int drm_inited;
	int vga_registered;
	int power_domains_inited;
	int dmc_inited;
	int modeset_wq_ok;
	int flip_wq_ok;
	int dstate_inited;
	int nogem_inited;

	/* Nonzero while the display half is bound to the interrupt device. */
	int irq_bound;

	/*
	 * Nonzero once the firmware display check decided the display is not
	 * to be used: every later display stage is skipped and the node has no
	 * panel.
	 */
	int absent;
};

#endif
