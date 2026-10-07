/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Engines: their setup, their kernel contexts, and the GT resume.
 *
 * This follows the reference for Gen12 (Alder Lake-P):
 *
 *   engine setup       init_status_page() -- one 4 KiB object pinned in the
 *                      GGTT and zeroed, whose GGTT address RING_HWS_PGA
 *                      takes -- and the software execlists state;
 *   engine set init    intel_engines_init(): per engine the setup, the
 *                      execlists registers (submit.h) and the pinned kernel
 *                      context with a 4 KiB ring, whose timeline lives in the
 *                      status page at I915_GEM_HWS_SEQNO_ADDR;
 *   GT resume          intel_gt_resume() in the reference order (see
 *                      drv_i915_gt_resume());
 *   stop               intel_engine_stop_cs() and the Wa_22011802037 drain of
 *                      pending MI_FORCE_WAKEs, both used by the reset
 *                      preparation.
 *
 * Facts this rests on: on Gen11+ the CSB write pointer is status-page dword
 * 0x2f and the CSB holds 12 entries; RING_MODE_GEN7 gets
 * GEN11_GFX_DISABLE_LEGACY_MODE, not GFX_RUN_LIST_ENABLE; only
 * I915_ERROR_INSTRUCTION is unmasked, because the hardware already suppresses
 * the privilege error; and with global MOCS the per-engine MOCS table is
 * skipped but the render engine's L3CC table is programmed again on every
 * resume.
 */

#ifndef DRIVERS_GPU_I915_ENGINE_H
#define DRIVERS_GPU_I915_ENGINE_H

#include <stdint.h>

#include "context.h"
#include "device-info.h"
#include "submit.h"

struct i915_gt_init;
struct i915_gt_mem;
struct i915_gt_object;
struct i915_gt_ppgtt;
struct i915_mmio;
struct spinlock;

/*
 * The GT state of one engine: its status page and its execlists registers.
 *
 * It lives in the engine set from drv_i915_engine_setup_common() to
 * drv_i915_engine_release().  The engine information it points to belongs to
 * the GT information and outlives it.
 */
struct i915_gt_engine {
	/* The engine's identity, class, instance and register base. */
	struct i915_engine_info *info;

	/*
	 * The image __engines_record_defaults() saved; every context created
	 * afterwards starts from it.  NULL until the defaults are recorded.
	 */
	struct i915_gt_object *default_state;

	/* The status page, its CPU view, and the address RING_HWS_PGA is given. */
	struct i915_gt_object *status_page;
	volatile uint32_t *hwsp;
	uint64_t hwsp_ggtt;

	/* The execlists ports in use: one port pair. */
	unsigned port_mask;

	/* RING_EXECLIST_SQ_CONTENTS and RING_EXECLIST_CONTROL of the engine. */
	uint32_t submit_reg;
	uint32_t ctrl_reg;

	/* The CSB entries and the write pointer in the status page. */
	volatile uint64_t *csb_status;
	volatile uint32_t *csb_write;

	/* The CSB length and the last entry the driver has consumed. */
	unsigned csb_size;
	unsigned csb_head;

	/* The engine class and instance, pre-shifted for the descriptor's upper dword. */
	uint32_t ccid;

	/*
	 * The two fields of the device's slice information the render power
	 * clock state reads: the whole device is used by default.
	 */
	uint8_t sseu_slice_mask;
	int sseu_has_slice_pg;

	/* Nonzero once the status page exists, and once execlists were enabled. */
	int setup_done;
	int resumed;

	/* What the last command-streamer stop reported (0 or a positive errno). */
	int stop_cs_rc;

	/* The pending MI_FORCE_WAKEs the last reset preparation saw. */
	uint32_t mi_fw_pending;

	/* RING_ESR as read while execlists were enabled. */
	uint32_t esr_at_resume;

	/* How many register writes the enable and the CSB pointer reset made. */
	unsigned enable_writes;
	unsigned csb_reset_writes;
};

/*
 * Every engine of the GT with its execlists state and kernel context.
 *
 * The device owns one; it lives from drv_i915_engines_init() to
 * drv_i915_engines_release().  n counts the engines whose setup was started.
 */
struct i915_gt_engines {
	unsigned n;
	struct i915_gt_engine ge[I915_MAX_ENGINES];
	struct i915_execlists el[I915_MAX_ENGINES];
	struct i915_gt_context kernel_ce[I915_MAX_ENGINES];

	/* The last seqno of each kernel context's timeline, kept across a status-page loss. */
	uint32_t kernel_tl_seqno[I915_MAX_ENGINES];

	/* Nonzero once every engine was set up. */
	int inited;

	/* What the GT reset of the resume reported (0 or a positive errno). */
	int reset_rc;

	/* How many engines did not stop their command streamer in time. */
	unsigned stop_cs_timeouts;

	/* How many engines were resumed. */
	unsigned resumed;

	/* How many L3CC writes the render engine's resume made. */
	unsigned l3cc_writes_rcs;

	/* How many single-engine resets were made after a hang, and how many of them failed (ws083-p007). */
	unsigned engine_resets;
	unsigned engine_reset_failures;
};

int drv_i915_engine_setup_common(struct i915_gt_engine *ge, struct i915_engine_info *info, struct i915_gt_mem *gm, const struct i915_sseu *sseu);
void drv_i915_engine_release(struct i915_gt_engine *ge, struct i915_gt_mem *gm);
int drv_i915_engine_stop_cs(struct i915_gt_engine *ge, struct i915_mmio *mmio);
void drv_i915_engine_wait_for_pending_mi_fw(struct i915_gt_engine *ge, struct i915_mmio *mmio);
void drv_i915_engine_dump(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *mmio, const char *why);

int drv_i915_engines_init(struct i915_gt_engines *es, struct i915_gt_info *gt, struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp);
void drv_i915_engines_release(struct i915_gt_engines *es, struct i915_gt_mem *gm);
int drv_i915_gt_resume(struct i915_gt_engines *es, struct i915_gt_init *gi, const struct i915_gt_info *gt, struct i915_mmio *mmio, struct spinlock *uncore_lock);
int drv_i915_engine_reset(struct i915_gt_engines *es, struct i915_gt_init *gi, const struct i915_gt_info *gt, unsigned index, struct i915_mmio *mmio, struct spinlock *uncore_lock);

#endif
