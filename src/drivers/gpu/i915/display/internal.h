/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display part's own types: what the display files that are not xd
 * files share.
 *
 * This header is private to src/drivers/gpu/i915/display/.  It holds the
 * state the display files pass to each other (the CD clock, the DMC
 * loader, the panel, the VBT, the scanout, the resident present path) and
 * the display root, struct i915_display, which the device owns and which
 * holds every long-lived display object.  The xd files do not include it.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_INTERNAL_H
#define DRIVERS_GPU_I915_DISPLAY_INTERNAL_H

/*
 * The xd display files are compiled with I915_XD_STRICT by their check, so
 * a path by which one of them reaches this header stops the compile.
 */
#ifdef I915_XD_STRICT
#error "an xd display file includes internal.h, the older display code's header"
#endif

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
#include "display-internal.h"
#include "display-power.h"
#include "xd-dp.h"
#include "xd.h"
#include "xd-backend.h"
#include "xd-edp.h"

/*
 * The old name of struct i915_display_mode (display-internal.h), which
 * host tests written against the older display code still spell.
 */
#define i915_lcd_mode i915_display_mode

/* The structure a member pointer points into. */
#define container_of(ptr, type, member) ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/*
 * The CDCLK (clock.c).
 */

struct mutex;

/*
 * One CD clock state: the reference, the PLL's VCO (0 when the PLL is
 * off and the clock bypasses it, ~0 when it is not known), the CD clock
 * and the bypass, in kHz, and the voltage level the clock needs.
 */
struct i915_cdclk_config {
	uint32_t ref;      /* refclk kHz */
	uint32_t vco;      /* PLL VCO kHz; 0 = off; ~0u = unknown/force */
	uint32_t cdclk;    /* cdclk kHz; 0 = force reprogram */
	uint32_t bypass;   /* bypass kHz */
	uint8_t  voltage_level;
};

/*
 * The CDCLK of the display as the older code reads it: the state the
 * hardware is in (a copy of what the xd clock read or set), the xd state
 * the CDCLK is read and set through, the platform's facts, and the
 * diagnostics of the last change.
 */
struct i915_cdclk_dev {
	struct i915_cdclk_config hw;          /* the hardware's current state */
	struct i915_xd *xd;                   /* the xd window and platform description */
	int display_ver;                         /* 13 for ADL-P */
	int has_cdclk_crawl;                     /* ADL-P: 1 */
	int has_cdclk_squash;                    /* ADL-P: 0 */
	struct i915_mmio *m;                    /* MMIO backend */
	struct mutex *sb_lock;                   /* PCODE sideband lock */

	/* What the last start or change met. */
	struct i915_cdclk_config diag_observed_before;
	struct i915_cdclk_config diag_sanitized;
	struct i915_cdclk_config diag_requested;
	int diag_no_change;            /* 1 = the firmware's state was kept; nothing written */
	int diag_prepare_status;       /* PCODE prepare-for-change result (0 / -errno) */
	int diag_hw_sequence_reached;  /* 1 once the PLL/CDCLK_CTL writes ran */
	int diag_notify_status;        /* PCODE voltage-notify result */
};

/*
 * The DMC firmware (dmc-shell.c, xd-dmc.c).
 *
 * The image of the display's DMC programs as the loader keeps it: for
 * each DMC (the main one and one per pipe), the entry chosen for the
 * display's stepping, the registers its load writes and a copy of its
 * program in storage the display owns, so that it outlives the firmware
 * file.  Lengths are in 32-bit words unless a field says bytes.
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
	/* What the load wrote. */
	unsigned payload_writes;   /* DMC_PROGRAM DWORD writes performed */
	unsigned aux_writes;       /* trailing per-DMC MMIO writes */
	unsigned evt_disable_writes; /* event-handler CTL+HTP disable writes */
	int load_seq_completed;    /* 1 only after the WHOLE sequence (incl. post) ran */
	uint64_t psum;             /* order-sensitive checksum of payload (addr,val) */
	uint64_t asum;             /* order-sensitive checksum of aux (addr,transformed val) */
};

/*
 * The DMC loader of a display (dmc-shell.c).
 *
 * The start queues the load on the loader's workqueue and holds a power
 * reference of its own (every pipe's wells and the exit from the DC
 * states) until the load completes; the load reads the firmware, parses
 * it and writes the programs.  The stop waits for the work, gives a
 * reference still held back, and frees the payload's storage.
 */
struct i915_dmc_dev {
	struct i915_dmc dmc;
	struct i915_mmio *m;
	struct i915_display_power *power;
	struct i915_xd *xd;
	struct i915_workqueue *wq;
	const char *fw_path;
	/* dmc_fallback_path() is a platform question, not a path question */
	int is_alderlake_p;
	struct i915_work work;
	int dmc_wakeref_held;          /* the loader's own power reference is held */
	uint32_t dc_state;
	/* What the load met, for the logs. */
	int work_submitted;
	int worker_started;
	int firmware_acquired;
	int fallback_requested;
	int main_payload_present;      /* NOT the same as load_seq_completed */
	int load_seq_completed_flag;
	int first_fault;
};

/*
 * The port numbers the hotplug, Type-C and external DisplayPort code use:
 * the combo ports A and B, and the first Type-C port, which is also port
 * D's number.
 */
#define I915_PORT_A      0
#define I915_PORT_D      3
#define I915_PORT_TC1    3

struct i915_rpm;
struct i915_display_nogem;

/* The display arbiter control and its isochronous priority control enable. */
#define I915_DISP_ARB_CTL2          0x45004u
#define I915_DISP_IPC_ENABLE        (1u << 3)

/*
 * What the display's serve step leaves: the pipes the firmware left lit,
 * IPC, and the start's power and runtime references given back (or the
 * power reference kept for the takeover), with the wells counted around
 * the power reference's give-back.
 */
struct i915_driver_probe {
	unsigned active_crtcs;
	int power_release_deferred;
	int ipc_enabled;
	unsigned wells_on_before, wells_on_after;
	int dc_state_after;
	unsigned verify_mismatches;
	int rpm_usage_after;
	int registered;
};
/*
 * The VGA plane and arbiter (vga.c).
 */
/* video/internal.h resource flags returned by the decode callback. */
#define I915_VGA_RSRC_LEGACY_IO   0x01u

/* VGA arbiter client state (a faithful subset of the registration). */
struct i915_vga_client {
	struct drv_pci_device *gpu;   /* display device (arbiter client) */
	struct drv_pci_device *gmch;  /* host bridge 00:00.0 (GMCH_CTRL owner) */
	unsigned display_ver;         /* selects I915_GT_SNB_GMCH_CTRL vs INTEL_GMCH_CTRL */
	int registered;               /* callback installed (0 after unregister) */
};

/*
 * Legacy VGA I/O accessor (get, in, out, put).  In
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
 * The VBT state (vbt-shell.c).
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
	int bl_type;                      /* the VBT's backlight type */
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
	unsigned log_errors;              /* error lines the parse logged */
	/* The xd parse the values come from (vbt-shell.c), or NULL before a parse. */
	const struct i915_xd_vbt *xd;
};

/*
 * The VBT provider (vbt-shell.c).
 */
/*
 * The VBT acquisition and parse (vbt-shell.c): the VBT is taken from (1) the
 * OpRegion (read at the start), else (2) the PCI expansion ROM read through
 * config 0x30 and a device mapping.  A found and validated VBT is parsed
 * (BDB blocks, general features and definitions); an absent one takes the
 * default child devices of the non-Type-C DDI ports.  The SPI flash of a
 * discrete part is not read.  It never makes a VBT up and never fails the
 * probe: it reports which source was used.
 */

struct i915_pci;

/*
 * Where the VBT the parser was given came from.
 */
enum i915_vbt_source {
	I915_VBT_SRC_NONE = 0,     /* no VBT found -> missing defaults */
	I915_VBT_SRC_OPREGION,     /* OpRegion mailbox #4 (read at the start) */
	I915_VBT_SRC_PCI_ROM,      /* PCI expansion ROM ($VBT) */
	I915_VBT_SRC_EXPLICIT_BLOB, /* a named, hash-pinned blob the build asked for -- NOT an OpRegion */
};

/* One VBT child device (child_device_config subset used for default gen). */
struct i915_vbt_child {
	unsigned port;               /* source DDI port (diagnostic) */
	uint8_t  dvo_port;           /* DVO_PORT_* */
	uint32_t device_type;        /* DEVICE_TYPE_* bitmask */
};

/* The display state the VBT gives. */
struct i915_vbt_state {
	int has_display;                 /* the device has a display */
	uint16_t version;                /* the VBT's version */
	int vbt_found;                   /* a real VBT was located + validated */
	int source;                      /* enum i915_vbt_source */
	int missing_defaults_used;       /* the default child devices were used */
	unsigned num_bdb_blocks;         /* bdb_blocks list length */
	unsigned num_display_devices;    /* display_devices list length */
	struct i915_vbt_child display_devices[8];

	/* The parser's result (vbt-shell.c), which the device owns until the display forgets it at its stop. */
	struct i915_vbt parsed;
	int parsed_live;

	/* explicit blob bookkeeping (no build fills it; always zero) */
	const char *blob_name;
	unsigned blob_size;
	uint8_t blob_sha256[32];
	int blob_requested, blob_found, blob_hash_ok, blob_subsys_ok, blob_valid;
	uint16_t subsys_vendor, subsys_device;
};

/*
 * Whether the explicit VBT is asked for.
 *
 * Never: no VBT blob is built in, so the kernel takes the VBT from the
 * OpRegion or the PCI ROM, or the defaults.
 *
 * XXX: the explicit-blob bookkeeping it leaves (the EXPLICIT_BLOB source and
 * origin, the blob fields) is dead and can go with the next change of vbt-shell.c.
 */
#define I915_VBT_EXPLICIT 0

/*
 * The OpRegion VBT source (vbt-shell.c).
 */
/*
 * Where the ACPI OpRegion keeps the VBT.  It reads a copy of the 8 KiB
 * OpRegion and says where the VBT is; the caller maps and validates it.  No
 * VBT firmware file and no DMI quirk list is consulted.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/display/intel_opregion.c
 * (struct opregion_header, struct opregion_asle, OPREGION_*_OFFSET, MBOX_*).
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
 * The firmware display check (output.c).
 */
/*
 * The output the firmware (GOP) was scanning out when the driver started
 * (the GPU scanout rule): the kind of interface, the pipe and
 * the port (enum port, PORT_NONE when none), the transcoder's mode select
 * (TRANS_DDI_FUNC_CTL bits 26:24), and every pipe the firmware had lit.
 */
#define I915_GOP_NONE		0U	/* no pipe was lit */
#define I915_GOP_EDP		1U	/* DDI A in DP SST mode: the built-in panel */
#define I915_GOP_HDMI		2U	/* a combo PHY port in HDMI or DVI mode: the driver's HDMI path */
#define I915_GOP_OTHER		3U	/* an interface this driver cannot light yet */
#define I915_GOP_DP_TC		4U	/* another port of the platform in DP SST mode: the external DP path */

struct i915_gop_output {
	unsigned kind;
	unsigned pipe;
	int port;
	unsigned mode;
	unsigned pipes;
};

/*
 * The built-in panel's device (edp-device.c): the kernel backend it runs
 * on (struct i915_dp_env, bound to the MMIO, the waits, the power domains,
 * the locks and the delayed work), its configuration and what its start
 * found.
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
	/*
	 * the register window the AUX transactions (xd-aux) and the panel power
	 * sequencer (xd-panel) run through, both under the PPS mutex: the same
	 * registers as read32/write32, with waits of its own; the binding that
	 * sets the hooks above sets it too
	 */
	struct i915_xd_io *aux_io;
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
	unsigned sleeps;
	uint64_t slept_us;
	unsigned lock_errors;                           /* recursion / unlock of a free lock */
};

#define I915_DP_LOCK_PPS 0
#define I915_DP_LOCK_AUX 1

#define I915_EDP_MAX_EDID_BLOCKS 4u

/*
 * Result codes are negative errnos in a numbering of their own (zedBSD's
 * <errno.h> numbers differ): rc == -I915_EDP_ETIMEDOUT and so on.
 */
#define I915_EDP_EIO 5
#define I915_EDP_ENXIO 6
#define I915_EDP_EBUSY 16
#define I915_EDP_EINVAL 22
#define I915_EDP_ETIMEDOUT 110

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
	 * all zero when the panel type is not known before the EDID (the early state)
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
 * What the eDP sink bring-up found and did: the PPS registers, DPCD, EDID
 * and who holds what.
 */
struct i915_edp_result {
	int stage;                        /* the last stage COMPLETED */
	int rc;                           /* 0, or the negative errno of the stage that failed */
	int failed_stage;

	int pps_idx, pps_valid;
	struct i915_edp_pps_regs before;       /* as found */
	struct i915_edp_pps_regs after_init;   /* after the PPS delays were programmed */
	struct i915_edp_pps_regs after_acquire;/* VDD is expected ON here */
	struct i915_edp_pps_regs after_end;    /* after VDD was turned off and waited for */
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
	unsigned i2c_defers, i2c_nacks;
	unsigned log_errors;              /* error lines the run logged */
	uint64_t elapsed_ms;
};

/*
 * The modeset hooks (edp-device.c).
 */
/* The panel power sequencer's and the backlight power's operations, which the resident eDP runs (edp-device.c). */
enum i915_lcd_panel_op {
	I915_LCD_PANEL_ON = 0,          /* panel power on */
	I915_LCD_PANEL_OFF,             /* panel power off */
	I915_LCD_PANEL_VDD_ON,          /* VDD forced on */
	I915_LCD_PANEL_VDD_OFF_SYNC,    /* VDD off, waited for */
	I915_LCD_PANEL_BACKLIGHT_ON,    /* the PPS's backlight-enable bit set */
	I915_LCD_PANEL_BACKLIGHT_OFF,   /* the PPS's backlight-enable bit cleared */
};

#define I915_LCD_LOCK_DPLL 0           /* i915->display.dpll.lock */
#define I915_LCD_LOCK_BACKLIGHT 1      /* i915->display.backlight.lock */

/* points of the commit at which the caller may look at the hardware (no writes of the path depend on them) */
enum i915_lcd_observe {
	I915_LCD_OBS_COMMIT_BEGIN = I915_XD_OBSERVE_COMMIT_BEGIN,
	I915_LCD_OBS_UNDERRUN_ARM = I915_XD_OBSERVE_UNDERRUN_ARM,
	I915_LCD_OBS_PIPE_ENABLED = I915_XD_OBSERVE_PIPE_ENABLED,
	I915_LCD_OBS_PLANE_ARMED = I915_XD_OBSERVE_PLANE_ARMED,
	I915_LCD_OBS_PLANE_DISABLED = I915_XD_OBSERVE_PLANE_DISABLED,
	I915_LCD_OBS_UNDERRUN_DISARM = I915_XD_OBSERVE_UNDERRUN_DISARM,
	I915_LCD_OBS_PIPE_DISABLED = I915_XD_OBSERVE_PIPE_DISABLED,
	I915_LCD_OBS_COMMIT_END = I915_XD_OBSERVE_COMMIT_END,
	I915_LCD_OBS_NUM = I915_XD_OBSERVE_POINTS
};

/*
 * wait_reg results (a numbering of their own, as the callers compare them):
 *   0                    the condition held
 *   I915_LCD_ETIMEDOUT the device did not reach the condition in time
 *   I915_LCD_EIO       the time source / the wait primitive / MMIO access failed: NOT a timeout; the backend has
 * also reported it through i915_lcd_backend_fault() so it is the run's first anomaly
 */
#define I915_LCD_ETIMEDOUT (-110)
#define I915_LCD_EIO       (-5)

/*
 * The sink hooks of an AUX channel of its own and the backend of a
 * screen are the xd ones (xd-backend.h); the older code names them so.
 */
#define i915_lcd_aux_emit i915_xd_sink_hooks
#define i915_lcd_emit i915_xd_backend

/*
 * The commit observation (run-log.c).
 */
/*
 * What the LCD check looks at while a modeset runs: the pipe's underrun status (ICL_PIPESTATUS), its frame counter
 * and its interrupt mask.  Plain C over i915_lcd_ops, so the same code runs on the register model and on the real
 * GPU.
 *
 * Why it exists: this private, synchronous, one-buffer test does not connect the DRM vblank machinery nor the
 * underrun interrupt.  In exchange it OWNS the status register for the run:
 *   - the value found before the run is saved (not judged: it belongs to whoever ran before);
 *   - the status is cleared where underrun reporting is turned on;
 *   - it is sampled at every observe() point of the commits and by the caller during the steady picture; bits seen
 *     are RECORDED first and only then cleared, so that a later period can be told from an earlier one and no
 *     occurrence is lost;
 *   - start / stop periods and the steady period are accumulated apart: an underrun while the pipe starts is a
 *     different statement from one while a picture stands.
 * The pipe's vblank interrupt must stay masked for the whole run (nothing here could service it): every sample also
 * reads I915_GT_GEN8_DE_PIPE_IMR and remembers if the vblank bit was ever found unmasked -- judged only while the pipe's
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
	uint32_t imr;                   /* I915_GT_GEN8_DE_PIPE_IMR as read */
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
	/*
	 * Between the enable's underrun-arm point and the disable's
	 * pipe-disabled point the pipe holds its power wells; outside, pipe
	 * A's registers (power well A) read 0 and say nothing.
	 */
	int powered;
	uint32_t seen_transition;       /* underrun bits seen while the pipe was started / stopped */
	uint32_t seen_steady;           /* underrun bits seen while the picture stood */
	int vblank_unmasked_seen;
	/* the stop evidence, taken at the disable's pipe-disabled point -- the last moment the pipe's power well is on */
	int stop_checked; uint32_t stop_transconf, stop_frame0, stop_frame1;
	unsigned n, dropped;
	struct i915_lcd_obs_sample s[I915_LCD_OBS_MAX_SAMPLES];
};

/*
 * The modeset trace (run-log.c).
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
	I915_LCD_T_STEP,          /* name: a step that is not implemented */
	I915_LCD_T_ERROR,         /* name: an error line */
	I915_LCD_T_PHASE,         /* name: a marker written by the caller */
	I915_LCD_T_DECIDED,       /* name: a step deliberately not connected */
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
 * What a screen holds and what a flip did are the pictures' records
 * (xd-picture.h); the older code names them and their results so.
 */
#define i915_lcd_modeset_status i915_xd_picture_status

/*
 * The show of a resident run (resident.c): the buffer's life (create, pin,
 * draw, publish, begin ... end, unpin, destroy, or abandon), the light and
 * the dark of the screen, the observation between them, and the decision
 * whether the buffer may be given back.
 *
 * Three records are kept apart, because they answer different questions:
 * the first anomaly (the first thing that went wrong, where, and how far
 * the run had got, never overwritten by what the cleanup reports), the
 * state the light left (software state and what the sink and the frame
 * counter said), and what the dark and the stop confirmation returned and
 * what is still held.  Software flags ("the arm was written") and hardware
 * observation (frame counter, underrun status, sink link status) are not
 * mixed.
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
 * What one show needs: the screen's configuration, the backend, the GPU
 * memory, the pipe it watches, and the run's window and stage hooks.
 */
struct i915_lcd_show_env {
	struct i915_lcd_emit *hw;             /* the backend */
	struct i915_gt_mem *gm;               /* where the scanout buffer lives */
	struct i915_xd_picture_config cfg;    /* everything but the framebuffer: filled by the caller */
	int pipe;
	unsigned first_frames_ms;               /* how long to wait for the first frames */
	unsigned window_ms;                     /* the finite window after the in-window work */
	/*
	 * Optional: the work that runs while the picture is up (after the first frames, before the window); 0 = done.
	 * A failure is recorded as the first anomaly (if none yet) and the dark follows.  at_stage is told every stage.
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
	int released, abandoned;
	int display_acquired;                   /* the buffer was handed to the display (the enable commit was attempted) */
	int display_released;                   /* the display has provably stopped reading it (back to PINNED) */
	struct i915_lcd_observer obs;
	struct i915_lcd_trace *trace;         /* the run log (static storage of the show body) */
	int pass;
};

/*
 * The resident runs (resident.c, present.c, head.c).
 */

struct i915_edp_device;
struct i915_dmc_dev;
struct i915_irq_dev;
struct i915_display_irq;
struct i915_gt_engines;
struct i915_gt_ppgtt;
struct spinlock;

/* the release decision after a draw and (maybe) a show -- exported for the GPU-free test */
struct i915_fhd_render;
struct i915_scanout;
struct i915_gt_tlb;

/*
 * The resident eDP device (edp-device.c).
 */
/* the env's kernel backend: real mutexes, tick-driven sleeps, timer + worker threads */
struct i915_dp_kernel {
	struct i915_mmio *mmio;
	struct i915_display_power *power;
	struct mutex locks[2];                  /* I915_DP_LOCK_* */
	struct i915_xd_io aux_io;               /* the AUX channels' and the PPS's window on mmio, under the PPS mutex */
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
	struct i915_display_mode mode;        /* the panel's mode, from its EDID */
	struct i915_display_link link;        /* the link and the combo PLL the screen's plan chose for it */
	int link_rc;                          /* 0, or why the mode or the link could not be planned */
	uint32_t ddi_buf_ctl_readout;           /* DDI_BUF_CTL of the eDP port when the connector was initialised */
	int vbt_bpp;
	struct i915_vbt_state *vbt;
	struct i915_xd_edp xe;                /* the panel's sink (xd-edp.c): its sequencer, DPCD and EDID */
};

struct i915_workqueue;

struct i915_lcd_emit;

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
 * One run of the resident output or of the second output (resident.c).
 *
 * The run's parameters (display-internal.h) name its output; the rest is
 * what the run took (power references, events) and what it saw, kept so
 * the teardown gives back exactly what the run holds.
 */

/*
 * The power numbers a run takes references by (resident.c): one per power
 * function of the xd power wells (I915_XD_POWERS_*), and the last for a
 * use that needs no well.
 */
#define I915_LCD_POWER_SLOTS	65

struct i915_lcd_kernel {
	/* The parameters of the run; NULL before it starts. */
	const struct i915_lcd_run_params *p;

	/* The display the run belongs to. */
	struct i915_display *display;

	/* What the run drives. */
	const struct i915_lcd_kernel_deps *d;

	/* The two modeset locks (dpll, backlight): lcdb_locks of the display. */
	struct mutex *locks;

	/* The register, wait, panel, power and vblank hooks of the run. */
	struct i915_lcd_emit ops;

	/* Power references this run took and has not returned, per power number (resident.c). */
	int power_refs[I915_LCD_POWER_SLOTS];

	/* What went wrong, counted. */
	unsigned power_get_failures, wait_timeouts, time_faults, unresolved_steps, decided, errors;

	/* Nonzero once the run is in its cleanup. */
	int phase_cleanup;

	/* The window of the run (ms). */
	unsigned window_ms;

	/* The interrupt and vblank event state of the run. */
	int irq_was_on, event_armed, event_pipe;
	uint32_t event_frame;
	unsigned vblank_sleeps, sleep_irq_off, events_cancelled;

	/* The two flip buffers and what the flips did. */
	struct i915_scanout *flip_a, *flip_b;
	unsigned flips_done;

	/* Set when the disable commit starts; errors are counted apart from then on. */
	int probe_watch;
	uint32_t probe_first_dsl;
	unsigned cleanup_errors;
};

/* The size of the VBT copy the parser reads (the largest VBT the provider accepts). */
#define I915_VBT_MAX 8192u

/*
 * The size of the DMC firmware arena.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/i915/display/intel_dmc.c
 * (DISPLAY_VER13_DMC_MAX_FW_SIZE).
 */
#define I915_DMC_ARENA_SIZE 0x20000u

/* The size of the VBT copy the firmware display check takes out of the OpRegion. */
#define I915_NATIVE_VBT_COPY_SIZE 16384u

/*
 * The display part of one device.
 *
 * The device owns exactly one; the device start fills it in the order of
 * the display probe and the device stop takes it apart in reverse.
 * It holds every long-lived display object: the former static locals of
 * the probe, the former file-scope state of the display files, and one
 * pointer per environment to that environment's own state (whose
 * types are complete only inside the environment, so the owner file of
 * the environment allocates it).
 *
 * Unless a field says otherwise it is written by the device start or stop
 * only, which run on the start worker one at a time; after the start the
 * resident serving thread is the only other writer.
 */

/*
 * The kinds of output the resident run lights: the built-in
 * eDP panel, an HDMI display, or an external DisplayPort display (a Type-C
 * port's, which fills its part).  Zero is the panel.
 */
enum i915_output_kind {
	I915_OUTPUT_KIND_PANEL = 0,
	I915_OUTPUT_KIND_HDMI,
	I915_OUTPUT_KIND_DP_EXT,
	I915_OUTPUT_KIND_COUNT
};

/*
 * The output the resident node drives (output.c).
 *
 * Chosen when the node's panel dependencies are filled at the device start:
 * the firmware's output (the GPU scanout rule).  Keiland moves it to another
 * connected output while no lease is held (display.c's claim),
 * and the end of such an output's last hold brings the firmware's back.
 * Zeroed, it is the eDP panel of an unknown connector.
 */
struct i915_display_output {
	/* What it is: the panel, an HDMI display (on a combo PHY port, DVI mode), an external DP display. */
	enum i915_output_kind kind;

	/* The pipe and transcoder it is lit on. */
	unsigned pipe;

	/* Nonzero: the node drives no output (the firmware's output could not be driven). */
	int none;

	/* Nonzero once connector names the hotplug path's connector of the output (its ID and generation follow it). */
	int has_connector;
	unsigned connector;

	/* The HDMI or DP mode (its physical size included, 0 when unknown), and the link and PLL its plan chose. */
	struct i915_display_mode mode;
	struct i915_display_link link;

	/*
	 * The DDI port of an HDMI or an external DP display; an external DP
	 * display's Type-C port (0 = TC1) and what its sink's probe found
	 * when the output was prepared.  Unused for the panel.
	 */
	unsigned tc_port;
	int port;
	struct i915_xd_dp_probe sink;

	/* Where the mode came from, for the log: "EDID", "display.mode", "CEA 4". */
	const char *mode_source;
};

/*
 * The second output, the head:
 * an HDMI or an external DisplayPort display shown beside the resident
 * output while Keiland holds both leases.
 *
 * The claim (display.c) gives the head its lease and prepares its output;
 * the head's first frame lights it on modeset screen 1, from inside the
 * display window the resident output keeps; its release, and the window's
 * end, stop it.  The lease fields are guarded by the head's own mutex, so
 * that a frame waiting for the head's flip never holds up the resident
 * output's.  The worker reads claimed and pipe under the device IRQ lock
 * when it decides whether the resident output is lit again for two pipes;
 * everything else of the lit head is the worker's alone.
 */
struct i915_display_head {
	/* Serializes the lease, the sequence and the claim; inited once the mutex is. */
	struct mutex mutex;
	int inited;

	/* The session holding the head's lease, NULL while there is none, and the lease (numbered with the resident output's). */
	void *owner;
	uint64_t lease;

	/* Completed presentations of the lease, and the tick of the last one. */
	uint64_t sequence;
	uint64_t present_tick;

	/* Nonzero while the head is claimed; the connector, its generation at the claim, and the head's pipe. */
	int claimed;
	unsigned connector;
	uint64_t generation;
	unsigned pipe;

	/* The output the claim prepared (its mode, link and PLL); the worker lights it. */
	struct i915_display_output output;

	/* Nonzero while the head is lit (the worker writes it). */
	int up;

	/* The head's two buffers at its mode's size, the one it shows, and what the presenting space maps of them. */
	struct i915_scanout buf[2];
	unsigned front;
	struct i915_ppgtt *map_vm;
	uint64_t map_va[2];
	unsigned map_pages[2];

	/* The head's run: its hooks, its vblank event, its power references, the parameters its pipe is read from, its configuration (kept off the stack). */
	struct i915_lcd_kernel k;
	struct i915_lcd_run_params params;
	struct i915_xd_picture_config cfg;

	/*
	 * A connector that could not be lit beside the resident output, and the
	 * connector's generation it was latched at (D-LIMIT): its claims are the
	 * limit until it is plugged again.  The device IRQ lock guards all three.
	 */
	int limited;
	unsigned limited_connector;
	uint64_t limited_generation;

	/*
	 * Nonzero while the lit head's buffers, and its last picture, are kept
	 * over a display window's end (a sleep, a hold that ran out): the next
	 * window lights the head again with them at its start.
	 * Only the worker reads and writes it.
	 */
	int dormant;

	/* Nonzero once a head's stop was not confirmed: its buffers are abandoned and no head is lit again. */
	int broken;
};

struct i915_lcd_world;
struct kern_backlight;
struct i915_wm_world;
struct i915_dp_ext_world;
struct i915_tc_port_world;
struct i915_vbt_world;
struct i915_hpd_world;

/*
 * The hardware cursor plane state of the resident display.
 *
 * Protected by its own mutex so cursor moves never block behind a primary
 * presentation's vertical-blank wait.  Two 64x64 ARGB8888 scanout buffers
 * are allocated and pinned on the first cursor_set and freed at display stop.
 */
struct i915_display_cursor {
	struct mutex mutex;
	int inited;
	void *lease_owner;
	uint64_t lease;
	struct i915_scanout buf[2];
	unsigned front;
	int buf_ready;
	uint32_t staging[64U * 64U];
	int staging_valid;
	int has_image;
	int shown;
	int32_t x;
	int32_t y;
	int pipe_up;
	unsigned pipe;
	uint32_t pipe_width;
	uint32_t pipe_height;
	struct i915_xd_ddb ddb;
	struct i915_xd_plane_wm wm;
	unsigned wm_levels;
	int wm_written;
};

struct i915_display {
	/* The device this display belongs to. */
	struct i915_device *device;

	/* The xd display: its register window, the device's description and what its parts found (xd.h). */
	struct i915_xd xd;

	/*
	 * The start's state, for the device's lifetime.
	 */

	/* The VBT the parser was given and what it found (display.c). */
	struct i915_vbt_state vbt_state;

	/* The VGA arbiter client (vga.c). */
	struct i915_vga_client vga_client;

	/* The display's power: the counts of its wells, the DC states and the display core; its own lock protects the counts. */
	struct i915_display_power power;

	/* The CDCLK state and hooks. */
	struct i915_cdclk_dev cdclk;

	/* The resident eDP panel: outlives the output setup, released in the stop. */
	struct i915_edp_device edp_dev;

	/* The resident output's display ID and generation whose modes were last logged (display.c, the EDID operation). */
	uint32_t edid_log_id;
	uint64_t edid_log_generation;

	/* The DMC loader; its work runs on dmc_wq. */
	struct i915_dmc_dev dmc_dev;

	/* The PCH the display found (display.c). */
	struct i915_pch_state pch;

	/* What the serve step left: the lit firmware pipes, IPC, the references given back. */
	struct i915_driver_probe dprobe;

	/* The display half of the interrupt device and its vblank delivery. */
	struct i915_display_irq irq;
	struct i915_irq_vblank irq_vblank;

	/* The DMC loader's workqueue. */
	struct i915_workqueue dmc_wq;

	/* Nonzero when the start found an OpRegion with a usable VBT. */
	int opregion_vbt_present;

	/* The firmware's output read from the firmware check's record, the only one the driver lights by itself. */
	struct i915_gop_output gop;


	/* Nonzero once the display core was started. */
	int display_core_inited;

	/* Nonzero while the hotplug path runs (started by the serve step). */
	int hpd_started;

	/* The resident run: what the serving thread needs, and the panel dependencies it uses. */
	struct i915_resident_ctx rctx;
	struct i915_lcd_kernel_deps rlcd;

	/*
	 * The output the resident run lights: the panel, or another output;
	 * and the firmware's, which the start chose and which
	 * comes back when a moved output's last hold ends.  The
	 * device IRQ lock covers a change of output against the worker, which
	 * reads it only when it enters the display window.
	 */
	struct i915_display_output output;
	struct i915_display_output gop_output;

	/* The panel's backlight device while the node is published with the panel as its output (backlight.c), or NULL. */
	struct kern_backlight *backlight;

	/* The resident output's refresh boundaries (control.c). */
	struct i915_display_refresh refresh;

	/*
	 * The resident present path.
	 */

	/* The display lease; its own mutex protects it. */
	struct i915_resident_display rd;

	/* The hardware cursor plane of the resident display; its own mutex protects it. */
	struct i915_display_cursor cursor;

	/* The serving thread's side of the display window. */
	struct i915_present_window window;

	/* The second output. */
	struct i915_display_head head;

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

	/*
	 * Former file-scope state of the world-neutral display files.
	 */

	/* The VBT bytes the parser reads and the OpRegion's VBT (vbt-shell.c). */
	uint8_t i915_vbt_buf[I915_VBT_MAX];
	const void *opregion_vbt_buf;
	size_t opregion_vbt_size;

	/* The DMC firmware image as loaded, and how much of it is used (dmc-shell.c). */
	uint8_t g_dmc_arena[I915_DMC_ARENA_SIZE];
	unsigned g_dmc_arena_used;

	/* The trace ring of the display core's start (display-power.c). */
	struct i915_trace dc_trace;

	/* The OpRegion and VBT copies of the OpRegion read. */
	uint8_t i915_opregion_read_data_opcopy[I915_OPREGION_SIZE];
	uint8_t i915_opregion_read_data_vbtcopy[I915_NATIVE_VBT_COPY_SIZE];

	/* The last ISA bridge the PCH search looked at (display.c). */
	struct drv_pci_device *next_isa_bridge_cursor;

	/* The VGA I/O port operations (vga.c); NULL until the VGA part is bound. */
	const struct i915_vga_io_ops *g_vga_io;

	/*
	 * The state of the parts still kept apart.  Each is allocated and freed by
	 * the owner file of its environment and is NULL outside that window.
	 */
	struct i915_dp_ext_world *dp_ext;
	struct i915_tc_port_world *tc_ports;
	struct i915_vbt_world *vbt_world;
	struct i915_hpd_world *hpd_world;

	/*
	 * The steps of the display start that completed, so the stop takes
	 * apart exactly what the start built.
	 */
	int vga_registered;
	int power_started;
	int dmc_inited;

	/* DC9 was entered for a suspend and is to be left by the resume. */
	int suspend_dc9;

	/* The front of the start after the interrupts ran: the stop takes the panel's device apart. */
	int front_done;

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
