/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display part's own shared types.
 *
 * These are the types every display implementation of the driver passes
 * around: the Gen9 display (gen9-*.c), the display 11 to 13 rewrite
 * (xd-*.c) and the files that serve the GPU node (present, head, output,
 * control, backlight, scanout, bootfb, interrupts).  None of them depends
 * on another implementation's state, so a file that needs only these
 * includes this header and nothing else of the display part.
 *
 * The header is private to src/drivers/gpu/i915/display/ and its tests.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_DISPLAY_INTERNAL_H
#define DRIVERS_GPU_I915_DISPLAY_DISPLAY_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <kern/lock.h>

#include "../sync.h"

/*
 * Marks a parameter a function receives by contract but does not use.
 *
 * The same definition as ../i915.h, for display files that do not include
 * it.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The pipes the display interrupt state has room for (pipes A to D). */
#define I915_IRQ_MAX_PIPES 4

/* The pixel format of every scanout buffer: 32 bits, the top byte unused ('XR24'). */
#define I915_FOURCC_XRGB8888 0x34325258u

/* The layout modifier of every scanout buffer: plain rows, no tiling. */
#define I915_MOD_LINEAR 0ull

/*
 * The kinds of south display (PCH) a display can sit beside.
 *
 * The order follows the generations of the south display, and the code
 * compares kinds with >= to mean "this generation or a later one", so the
 * values are kept as they are.  The kinds from 1024 on name displays
 * whose south display lives in the GPU itself rather than in a separate
 * PCH.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/soc/intel_pch.h
 * (enum intel_pch).
 */
enum i915_pch {
	I915_PCH_NOP = -1,      /* a PCH that has no south display */
	I915_PCH_NONE = 0,      /* no PCH at all */
	I915_PCH_IBX,           /* 1 */
	I915_PCH_CPT,           /* 2 */
	I915_PCH_LPT,           /* 3 */
	I915_PCH_SPT,           /* 4 */
	I915_PCH_CNP,           /* 5 */
	I915_PCH_ICP,           /* 6 */
	I915_PCH_TGP,           /* 7 */
	I915_PCH_ADP,           /* 8: Alder Lake and Raptor Lake */
	I915_PCH_DG1 = 1024,
	I915_PCH_DG2,
	I915_PCH_MTL,
	I915_PCH_LNL
};

/* How the PCH kind was decided; only the log reads it. */
enum i915_pch_source {
	I915_PCH_SRC_NONE = 0,
	I915_PCH_SRC_REAL,      /* a real ISA bridge matched the id table */
	I915_PCH_SRC_VIRT,      /* an emulated bridge -> platform guess */
	I915_PCH_SRC_NO_BRIDGE  /* no ISA bridge at all -> guest guess */
};

/* Where a scanout buffer is in its life. */
enum i915_scanout_state {
	I915_SCANOUT_NONE = 0,
	I915_SCANOUT_ALLOCATED,       /* backing + CPU mapping */
	I915_SCANOUT_PINNED,          /* + GGTT range, surf address valid */
	I915_SCANOUT_IN_USE,          /* the display engine reads it */
	I915_SCANOUT_ABANDONED        /* stop not confirmed: never released */
};

struct drv_pci_device;
struct i915_gt_mem;
struct i915_gt_object;
struct i915_irq_dev;
struct i915_mmio;
struct i915_display_power;
struct i915_ppgtt;

/*
 * One mode a display is driven in: the pixel clock, the horizontal and
 * vertical timing, the sync polarities and the physical size.
 *
 * A mode is a value: it is copied into whatever lights an output, and the
 * EDID decoder, the Gen9 display and the display 11 to 13 rewrite all fill
 * and read the same fields.
 */
struct i915_display_mode {
	int clock_khz;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	int hsync_positive, vsync_positive;
	uint16_t width_mm, height_mm;
	unsigned descriptor_index;        /* which of the base block's four descriptors */
	int edid_bpc;                     /* EDID 1.4 digital input colour depth; 0 = undefined */
};

/*
 * The hooks the power wells call around a pipe's power change.
 *
 * The interrupt layer (interrupts.c) implements them and the power-well
 * code is given them, so the power wells do not depend on the interrupt
 * device.  Each hook checks by itself whether interrupts are on.
 */
struct i915_pw_irq_ops {
	void (*post_enable)(void *ctx, unsigned pipe_mask);
	int (*pre_disable)(void *ctx, unsigned pipe_mask);     /* 0, or the drain failed: the well must NOT go off */
};

/*
 * The PCH the display found: its kind, its id and how it was found.
 *
 * The display fills it once at the start; the south display code (the
 * panel power, the backlight, the GMBUS, the hotplug pins) reads it for
 * the rest of the device's life.
 */
struct i915_pch_state {
	int type;                 /* enum i915_pch */
	uint16_t id;              /* masked PCH device id */
	int source;               /* enum i915_pch_source */
	unsigned bridges_scanned;
	uint16_t bridge_device;   /* raw device id of the bridge that decided it */
	uint16_t bridge_svid, bridge_sdid;
};

/*
 * The walk over the ISA bridges the PCH search makes.
 *
 * The device uses the real PCI scan; the GPU-free tests install a scripted
 * list so the virtual, real and absent paths can all be reached without
 * hardware.  next() returns 1 and fills the ids while entries remain, 0
 * when the walk is finished.
 */
struct i915_pch_bridge_ops {
	int (*next)(void *ctx, unsigned index, uint16_t *vendor, uint16_t *device,
		uint16_t *svid, uint16_t *sdid);
	void *ctx;
};

/*
 * One scanout buffer: a framebuffer the display engine reads.
 *
 * The buffer is narrow on purpose: XRGB8888, linear rows, no rotation, no
 * scaling, no compression and one colour plane.  A linear buffer needs no
 * display page table, so it is bound in the ordinary GGTT.
 *
 * It keeps apart things that have different owners:
 *   backing      DMA pages and their size                       (gt_mem object)
 *   cpu          the CPU mapping used to draw into it
 *   ggtt         the reserved GGTT range: guards + pages, and the display alignment
 *   layout       format / modifier / pitch / height            (what the plane is told)
 *   surf         the address the plane's surface register gets
 *   users        who pinned it, and whether the display is scanning it out
 *
 * The memory image is 32 bits per pixel whatever depth the link carries.
 *
 * Lifetime: create -> pin -> [publish after CPU writes] -> scanout_begin ... scanout_end
 * -> unpin -> destroy.  A buffer the display is (or may still be) reading is never
 * unpinned or freed: unpin and destroy refuse it, and when a stop could not be
 * confirmed the owner abandons it (drv_i915_scanout_abandon()), which keeps the pages
 * and the mapping for ever and records that, rather than a clean-looking teardown.
 */
struct i915_scanout {
	/* The buffer's size and how its rows are laid out. */
	uint32_t width, height;
	uint32_t format;                /* I915_FOURCC_XRGB8888 */
	uint64_t modifier;              /* I915_MOD_LINEAR */
	unsigned cpp;                   /* bytes per pixel of the MEMORY image: 4 */
	uint32_t pitch;                 /* bytes per row: width * cpp rounded up to the stride alignment */
	uint32_t stride_units;          /* pitch / 64: the PLANE_STRIDE encoding for a linear surface */
	uint32_t size;                  /* pitch * height */
	/* What the plane requires of where the buffer sits in the GGTT. */
	uint32_t alignment;             /* bytes: the surface alignment the plane requires */
	unsigned guard_pages;           /* scratch PTEs on each side: VTD_GUARD */
	/* The memory behind the buffer, its CPU mapping and its GGTT offset. */
	struct i915_gt_mem *gm;
	struct i915_gt_object *obj;
	uint32_t *cpu;
	uint64_t surf;                  /* GGTT offset for PLANE_SURF (fits 32 bits, 4 KiB aligned) */
	/* Where the buffer is in its life, who pinned it and who reads it. */
	int state;
	const char *pin_owner;
	unsigned users;                 /* how many displays read it now (begin / end); IN_USE while > 0 */
	unsigned publishes;
	unsigned refused_unpin, refused_destroy;
};

/*
 * The vblank delivery of the pipes.
 *
 * Per pipe: the number of vblank interrupts seen while enabled, a
 * completion that wakes the one waiter, and the enable references the
 * vblank users hold.  The display owns one; it is initialized before the
 * interrupt handler is installed and lives as long as the display.
 */
struct i915_irq_vblank {
	/* Protects every field below; the interrupt handler takes it too. */
	struct spinlock lock;

	/* Nonzero once the lock and the completions are initialized. */
	int inited;

	/* Wakes the waiter of a pipe on its next vblank. */
	struct i915_completion wake[I915_IRQ_MAX_PIPES];

	/* Vblank interrupts handled while the pipe's delivery was enabled. */
	volatile uint32_t count[I915_IRQ_MAX_PIPES];

	/* Nonzero while the pipe's vblank interrupt is enabled. */
	volatile int enabled[I915_IRQ_MAX_PIPES];

	/* Vblank references held per pipe; the last put disables the interrupt at once. */
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
 * struct i915_irq_display_ops.  The handler writes the counters; the rest
 * is set before the handler is installed and only read afterwards.
 */
struct i915_display_irq {
	/* The interrupt device whose display half this is. */
	struct i915_irq_dev *irq;

	/* The register access of the device. */
	struct i915_mmio *m;

	/* The display's power, whose wells enable and disable pipe interrupts as they change. */
	struct i915_display_power *power;

	/* The south display block the reset and the postinstall reach. */
	const struct i915_pch_state *pch;

	/* The display version, the runtime pipe and transcoder masks, and whether there is a display. */
	int display_ver;
	unsigned pipe_mask;
	unsigned cpu_transcoder_mask;
	int has_display;

	/* Nonzero when the VBT names a DSI panel, which adds the DSI port interrupts. */
	int dsi_present;

	/* The pipe interrupt masks, kept under the vblank lock. */
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

	/* Nonzero while the lease holder has the panel's light off (GPU_DISPLAY_POWER); the lease's end clears it. */
	int power_off;
};

/*
 * The refresh boundaries of the resident output (GPU_DISPLAY_REFRESH):
 * the pipe's hardware frame counter, read while the display
 * window is up and extended to a count that never goes back.  The lock
 * covers the window's display_up too: the worker lowers it under the lock
 * before the pipe stops, so the counter is never read from a stopped pipe.
 */
struct i915_display_refresh {
	struct spinlock lock;

	/* Boundaries counted so far, and the time the count last moved (ns, scheduler clock). */
	uint64_t count;
	uint64_t time_ns;

	/* The frame counter last read, and whether it was read since the pipe was lit. */
	uint32_t frame;
	int frame_valid;
};

/*
 * The serving thread's side of the display window.
 *
 * The request worker runs inside the display window while the panel is up
 * (the display enable calls back into the serving loop).  These are the
 * display fields of the old serving state; only the serving thread writes
 * them (but for what a field says of itself), and it reads display_failed
 * and lease_failed under the device IRQ lock when it decides whether a
 * presentation enters the window.
 */
struct i915_present_window {
	/* Nonzero while the serving thread is inside the display window. */
	int display_up;

	/*
	 * Nonzero once the firmware's output failed to come up or to stop
	 * cleanly: presentations fail from then on.  A moved output that fails
	 * sets lease_failed instead.
	 */
	int display_failed;

	/*
	 * Nonzero once a moved output's run failed and the firmware's output
	 * became the resident output again: the rest of the
	 * lease's presentations fail, and the next claimed lease clears it, so
	 * its first frame lights an output again.  The worker sets it and the
	 * claim clears it, both under the device IRQ lock.
	 */
	int lease_failed;

	/*
	 * Nonzero from a resume until the window is next entered:
	 * an HDMI display that does not come up then is taken to have been
	 * unplugged during the sleep, and the built-in panel is tried in its
	 * place.  The resume sets it while the worker is parked; only the
	 * worker reads and clears it afterwards.
	 */
	int after_resume;

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

	/*
	 * The hold between two leases.  A release does not leave
	 * the window: the output stays lit with the last picture of the lease
	 * that ended, so the next lease's first frame is a flip and not a
	 * modeset.  holding is nonzero from the release until the next lease's
	 * first frame, or until the window is left because the hold ran out
	 * (hold_until, in ticks) or the machine shuts down (hold_ended, which
	 * also refuses every later hold).  hold_since is the tick of the
	 * release.  The device IRQ lock guards all four: the worker writes
	 * them and the shutdown reads holding and sets hold_ended.
	 */
	int holding;
	int hold_ended;
	uint64_t hold_since;
	uint64_t hold_until;

	/*
	 * Nonzero asks the hold in progress to end now, once (a claim that moves
	 * the output); the worker clears it when it leaves the
	 * window.  The device IRQ lock guards it.
	 */
	int hold_cut;

	/*
	 * Bit i is set while resident buffer i still holds a picture of a
	 * lease that ended.  A frame that does not cover the whole buffer
	 * clears it first, so nothing of the ended lease shows around it.
	 * Only the worker reads and writes it.
	 */
	unsigned stale_buffers;

	/*
	 * The pipes besides the resident output's that the run lit the
	 * resident output for (its DBUF share leaves them room):
	 * BIT(the head's pipe) when a head was claimed as the run began, else
	 * 0.  Only the worker reads and writes it.
	 */
	unsigned run_pipes;

	/*
	 * Nonzero asks the window to be left and the resident output lit again
	 * at once, for two pipes, because a head's first frame came while the
	 * run had no room for it (the first output is lit again when the
	 * second is added).  keep_buffers keeps
	 * the resident buffers, and their picture, across that new lighting.
	 * Only the worker reads and writes both.
	 */
	int relight;
	int keep_buffers;

	/*
	 * Nonzero asks for the same, for the resident output alone, because
	 * its external DP link lost its lock and must be trained again:
	 * the hotplug path sets it, the worker takes it at the
	 * next frame and lights the output again.  Both hold the device IRQ
	 * lock for it.
	 */
	int retrain;

	/*
	 * Nonzero asks the window to be left at once because the display the
	 * resident output was moved to was unplugged: the hotplug
	 * path sets it, the worker leaves the window, the output is stopped
	 * and the firmware's output comes back at the window's end, which
	 * clears it.  The device IRQ lock guards it.
	 */
	int unplugged;
};

/*
 * The link an output's plan chose for its mode.
 *
 * One instance belongs to an output the resident node or the second
 * output drives: the link rate, the lanes and the pipe's bits per pixel,
 * the data and link M/N values with their transfer unit, the reference
 * clock of the port PLLs and, on a combo PHY port, the DPLL's three
 * configuration words.  An HDMI output's rate is its TMDS clock on four
 * lanes; a Type-C port's PLL words are the picture's own, so the combo
 * words stay 0 there.
 */
struct i915_display_link {
	uint32_t rate_khz;
	unsigned lanes;
	unsigned bpp;
	uint32_t tu;
	uint32_t data_m;
	uint32_t data_n;
	uint32_t link_m;
	uint32_t link_n;
	uint32_t ref_khz;
	uint32_t cfgcr0;
	uint32_t cfgcr1;
	uint32_t div0;
};

struct i915_edp_device;
struct i915_mmio;
struct i915_display_power;
struct i915_cdclk_dev;
struct i915_xd_bandwidth;
struct i915_xd_platform;
struct i915_dmc_dev;
struct i915_gt_mem;
struct i915_gt_engines;
struct i915_gt_ppgtt;
struct i915_driver_probe;
struct i915_xd_tc_ports;
struct spinlock;

/*
 * The objects of the display's start that a resident run reads and
 * uses.
 *
 * One instance lives in the display for the device's lifetime and is
 * filled once the start has built the panel, the display's power and the
 * GT memory; a run only reads it, and none of the objects it names is
 * copied or made again by a run.
 */
struct i915_lcd_kernel_deps {
	/* The built-in panel: its sink, its mode and link, its panel power and its VBT. */
	struct i915_edp_device *edp;

	/* The registers, and the display's power (its wells, the display buffer's slices). */
	struct i915_mmio *mmio;
	struct i915_display_power *power;

	/* The CD clock, the bandwidth and the DMC. */
	struct i915_cdclk_dev *cdclk;
	const struct i915_xd_bandwidth *bandwidth;
	const struct i915_xd_platform *platform;
	struct i915_dmc_dev *dmc;

	/* The display half of the interrupt device: vblank references and waits. */
	struct i915_display_irq *irq;

	/* The GT memory the scanout buffers live in. */
	struct i915_gt_mem *gm;

	/* Nonzero when the start enabled the isochronous priority control. */
	int ipc_enabled;

	/* The GT a draw is submitted to and the lock of its registers. */
	struct i915_gt_engines *es;
	struct i915_gt_ppgtt *vm;
	struct spinlock *uncore_lock;

	/* The CPU-mappable aperture, and the probe state whose start reference the takeover returns. */
	uint64_t gmadr_base;
	uint64_t gmadr_size;
	struct i915_driver_probe *dprobe;

	/* The display's Type-C ports, whose links an output's dark gives back. */
	struct i915_xd_tc_ports *tc;
};

/*
 * What a resident run drives besides the built-in panel.
 *
 * The resident run of another output and the second output's run point
 * their run at one; the resident run of the panel points at none.  It
 * names the kind of the output (HDMI or an external DisplayPort sink),
 * its DDI port and its pipe; the vertical blank hooks read the pipe.
 */
struct i915_lcd_run_params {
	int output_hdmi;
	int output_dp_ext;
	int port;
	int pipe;
};

#endif /* DRIVERS_GPU_I915_DISPLAY_DISPLAY_INTERNAL_H */
