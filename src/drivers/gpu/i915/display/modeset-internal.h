/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_crtc.h, intel_global_state.h),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2020 Intel Corporation
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_display_core.h),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_display_types.h),
 * which carries the following notice.
 *
 * Copyright (c) 2006 Dave Airlie <airlied@linux.ie>
 * Copyright (c) 2007-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_dpll_mgr.h),
 * which carries the following notice.
 *
 * Copyright © 2012-2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * The modeset environment: the Linux text of the panel and HDMI modeset.
 *
 * The display files that carry Linux modeset text (the mode and EDID
 * helpers, the link and DPLL calculations, the DDI, transcoder, pipe,
 * plane, colour, backlight, CDCLK, bandwidth and vblank bodies, and the
 * one-screen modeset object that runs them) are compiled against this
 * header.  It gives the Linux type names the layouts that text reads, and
 * maps what the text reaches outside itself -- registers, waits, sleeps,
 * locks, display power, the panel power sequencer, the sink's DPCD, the
 * error sink -- onto the backend of the device (struct i915_lcd_emit), so
 * that the same text runs on the register model and on the real GPU.
 *
 * The environment is fixed to the supported configuration: Gen12 display
 * (Tiger Lake, display version 12, or Alder Lake-P class, version 13), combo
 * PHY ports, eDP or HDMI over DDI, one pipe per stream, no big joiner, no
 * MST, no DSC, one plane on a linear framebuffer.  A branch of the Linux
 * text that would need more is either answered by a fixed definition below
 * (named as such), refused by the caller before the text runs, or reported
 * through the step hook at the position the Linux text calls it.  Nothing
 * is passed over in silence.
 *
 * Two headers are layered on this one: watermark-internal.h (the watermark
 * and DDB text) and takeover-internal.h (the readout, sanitize and takeover
 * of the display the firmware left running).  A translation unit includes
 * this header, then the layers it needs; it never includes another
 * environment.
 *
 * Linux names whose meaning differs between the environments are not
 * defined here under their Linux name.  Each meaning this environment gave
 * such a name has an explicit name of its own (i915_lcd_x() or I915_LCD_X).
 * Where the Linux text reached the device, the selected screen or the only
 * encoder through a file-scope variable, the explicit form takes that
 * object as an argument; the variables themselves are the fields of struct
 * i915_lcd_world.
 *
 * Errors: the functions of the display part return zedBSD positive errno
 * values at their boundaries.  The I915_LCD_E* constants below carry the
 * Linux numbers of this environment and exist only where a Linux number is
 * observable: a value the Linux text compares, logs, or hands to a Linux
 * hook, or one compared with a Linux dump.  I915_LCD_ETIMEDOUT and
 * I915_LCD_EIO (internal.h) are already negative, as the backend's wait
 * hook returns them.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_MODESET_INTERNAL_H
#define DRIVERS_GPU_I915_DISPLAY_MODESET_INTERNAL_H

#include "internal.h"

#ifdef I915_DISPLAY_LINUX_WORLD
#error "modeset-internal.h: another Linux environment is already included in this translation unit"
#endif
#define I915_DISPLAY_LINUX_WORLD "modeset"

/* This translation unit is compiled in the modeset environment. */
#define I915_DISPLAY_WORLD_MODESET 1

/*
 * The Linux spellings of fixed-width integers that the extracted text uses
 * in its own definitions (the fourcc helpers, the colour key, the EDID
 * words).  They come before the extracted text because it names them in
 * declarations, not only in macro bodies.
 */
typedef u16 __le16;
typedef uint32_t __u32;
typedef uint64_t __u64;

/* The Linux spelling of a packed structure, which the DP SDP definitions use. */
#define __packed __attribute__((packed))

/*
 * The Linux definitions this environment reads, in the order the modeset
 * text first included them.  Each one depends only on internal.h and the
 * integer spellings above; the few that need a definition of this header
 * (the power-domain set, the transcoders, the DBUF state, the crtc-state
 * inlines) are included where that definition is complete, further down.
 *
 * _PICK_EVEN_2RANGES() comes from internal.h (the Linux BUILD_BUG_ON_ZERO()
 * check of the register definitions was defined to 0 in this environment).
 * intel/mreg.h now supplies _PIPE() and
 * _MMIO_PIPE(), which the plane text defined again with the same body.
 */
#include "../intel/connector.h"
#include "../intel/ref.h"
#include "../intel/ddi.h"
#include "../intel/power.h"
#include "../intel/plane.h"
#include "../intel/wm.h"
#include "../intel/mreg.h"
#include "../intel/dp.h"
#include "../intel/phy.h"
#include "../intel/fourcc.h"
#include "../intel/psr.h"

/*
 * ==== Macros and constants ====
 */

/*
 * Marks a parameter a helper of this header does not read.  The same
 * definition as ../i915.h, which a display file does not include.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The Linux error numbers of this environment (see the file comment). */
#define I915_LCD_ENXIO 6
#define I915_LCD_EBUSY 16
#define I915_LCD_ENODEV 19
#define I915_LCD_EINVAL 22
#define I915_LCD_ERANGE 34

/* The number of screens (modeset objects) the one-screen path keeps at once. */
#define I915_LCD_MS_SCREENS 2

/* ---- linux/kernel.h, linux/math.h: the arithmetic helpers of the text ---- */

/* Reads a little-endian 16-bit word: the host is little endian. */
#define le16_to_cpu(x) ((u16)(x))

/* Marks a symbol exported: there is no module boundary here. */
#define EXPORT_SYMBOL(x)

/* 64-bit division, rounded up and down. */
#define DIV_ROUND_UP_ULL(n, d) ((u64)(((u64)(n) + (u64)(d) - 1u) / (u64)(d)))
#define DIV_ROUND_DOWN_ULL(n, d) ((u64)((u64)(n) / (u64)(d)))

/* Division rounded to the nearest value; the text only divides positive operands here. */
#define DIV_ROUND_CLOSEST(x, d) (((x) + ((d) / 2)) / (d))

/* The same for 64-bit operands. */
#define DIV_ROUND_CLOSEST_ULL(x, d) ((u64)(((u64)(x) + ((u64)(d) / 2u)) / (u64)(d)))

/* Marks an intended fall through between switch cases. */
#define fallthrough __attribute__((fallthrough))

/* Marks a declaration that may stay unused in a configuration. */
#define __maybe_unused __attribute__((unused))

/* The magnitude of a signed value (the WRPLL calculation's own helper). */
#define abs(x) ((x) < 0 ? -(x) : (x))

/* A frequency in kHz given in MHz units of the text (KHz(24) = 24000). */
#define KHz(x) (1000 * (x))

/* The largest values of the Linux fixed-width types. */
#ifndef U16_MAX
#define U16_MAX ((u16)0xffff)
#define U32_MAX ((u32)0xffffffffu)
#endif

/*
 * The index of the lowest and of the highest set bit (1-based, 0 for none),
 * the number of set bits of a byte and the base-2 logarithm.  The DP,
 * watermark and readout text each defined these with the same bodies.
 */
#define ffs(x) __builtin_ffs((int)(x))
#define fls(x) ((x) ? 32 - __builtin_clz((unsigned int)(x)) : 0)
#define hweight8(x) ((unsigned int)__builtin_popcount((unsigned int)(x) & 0xffu))
#define ilog2(x) (31 - __builtin_clz((unsigned int)(x)))

/* The halves of a 64-bit value. */
#define lower_32_bits(n) ((u32)((n) & 0xffffffffu))
#define upper_32_bits(n) ((u32)(((u64)(n)) >> 32))

/* Tells whether a value is a power of two. */
#define is_power_of_2(n) ((n) != 0 && ((n) & ((n) - 1)) == 0)

/* Clears a variable and yields its old value. */
#define fetch_and_zero(ptr) ({ __typeof__(*(ptr)) _v = *(ptr); *(ptr) = 0; _v; })

/*
 * Clamps a value into [lo, hi], each operand evaluated once (the Linux
 * clamp() / clamp_t()).
 */
#define I915_LCD_CLAMP(val, lo, hi) \
	({ __typeof__(val) _v = (val); __typeof__(val) _l = (lo); __typeof__(val) _h = (hi); _v < _l ? _l : (_v > _h ? _h : _v); })
#define I915_LCD_CLAMP_T(type, val, lo, hi) \
	({ type _v = (type)(val); type _l = (type)(lo); type _h = (type)(hi); _v < _l ? _l : (_v > _h ? _h : _v); })

/* The microseconds of a millisecond (the link-training delays). */
#define USEC_PER_MSEC 1000L

/*
 * The Linux jiffies counter.  Only intel_edp_init_source_oui() reads it, to
 * remember when the source OUI was written; the wait that consumes that
 * time belongs to the backlight / PPS side, so this environment reads 0.
 */
#define I915_LCD_JIFFIES (0ul)

/* linux/string_helpers.h */
#define str_yes_no(v) ((v) ? "yes" : "no")

/* ---- linux/bitmap.h / bitops.h, as far as the power-domain set needs them ---- */

/* The bits of one unsigned long. */
#define I915_BITS_PER_LONG (8u * (unsigned)sizeof(unsigned long))

/* Declares a bitmap of the given number of bits. */
#define DECLARE_BITMAP(name, bits) unsigned long name[((bits) + I915_BITS_PER_LONG - 1u) / I915_BITS_PER_LONG]

/* Runs the statement that follows only when the condition holds (drm_util.h). */
#ifndef for_each_if
#define for_each_if(condition) if (!(condition)) {} else
#endif

/*
 * Walks the set bits of a bitmap word.  The text only walks one word, so
 * the bit is tested in *addr directly.
 */
#define for_each_set_bit(bit, addr, size) \
	for ((bit) = 0; (bit) < (int)(size); (bit)++) \
		for_each_if(*(addr) & (1ul << (bit)))

/* A kernel configuration option: none is enabled (no per-domain wakeref tracking). */
#define IS_ENABLED(option) 0

/* ---- the error sink: drm_dbg / drm_err / WARN of the Linux text ---- */

/*
 * A debug line of the text: the format is checked at compile time, and only
 * the format string reaches the note sink (the caller reports the result).
 */
#define I915_LCD_DRM_DBG_KMS(dev, fmt, ...) \
	do { \
		if (0) \
			(void)drv_i915_lcd_fmtcheck(fmt, ##__VA_ARGS__); \
		drv_i915_lcd_note(fmt); \
	} while (0)

/*
 * An error line of the text: counted and handed to the error hook; the
 * first one is what a failed run is read from.
 */
#define I915_LCD_DRM_ERR(dev, fmt, ...) \
	do { \
		if (0) \
			(void)drv_i915_lcd_fmtcheck(fmt, ##__VA_ARGS__); \
		drv_i915_lcd_error(fmt); \
	} while (0)

/* A warning of the text: reported as an error when its condition holds; yields the condition. */
#define I915_LCD_DRM_WARN(dev, cond, fmt, ...) ({ int _w = !!(cond); if (_w) drv_i915_lcd_error(fmt); _w; })
#define I915_LCD_DRM_WARN_ON(dev, cond) ({ int _w = !!(cond); if (_w) drv_i915_lcd_error("WARN_ON(" #cond ")\n"); _w; })
#define I915_LCD_WARN_ON(cond) ({ int _w = !!(cond); if (_w) drv_i915_lcd_error("WARN_ON(" #cond ")\n"); _w; })
#define drm_WARN_ON_ONCE(dev, cond) I915_LCD_DRM_WARN_ON(dev, cond)
#define WARN(cond, fmt) ({ int _w = !!(cond); if (_w) drv_i915_lcd_error(fmt); _w; })

/* A switch value the text does not handle: a note, not an error. */
#define I915_LCD_MISSING_CASE(x) drv_i915_lcd_note("Missing case (" #x ")\n")

/* ---- DRM modes: the flags the kept functions test (drm_mode.h / drm_modes.h) ---- */
#define DRM_DISPLAY_MODE_LEN 32
#define DRM_MODE_TYPE_PREFERRED (1 << 3)
#define DRM_MODE_TYPE_DRIVER (1 << 6)
#define DRM_MODE_FLAG_PHSYNC (1 << 0)
#define DRM_MODE_FLAG_NHSYNC (1 << 1)
#define DRM_MODE_FLAG_PVSYNC (1 << 2)
#define DRM_MODE_FLAG_NVSYNC (1 << 3)
#define DRM_MODE_FLAG_INTERLACE (1 << 4)
#define DRM_MODE_FLAG_DBLSCAN (1 << 5)
#define DRM_MODE_FLAG_3D_MASK (0x1f << 14)
#define DRM_MODE_FLAG_3D_FRAME_PACKING (1 << 14)

/* drm_modes.h: the adjust flags of drm_mode_set_crtcinfo(). */
#define CRTC_INTERLACE_HALVE_V (1 << 0)
#define CRTC_STEREO_DOUBLE (1 << 1)
#define CRTC_NO_DBLSCAN (1 << 2)
#define CRTC_NO_VSCAN (1 << 3)
#define CRTC_STEREO_DOUBLE_ONLY (CRTC_STEREO_DOUBLE | CRTC_NO_DBLSCAN | CRTC_NO_VSCAN)

/* drm_mode.h: the connector kinds and DPMS states this path names. */
#define DRM_MODE_CONNECTOR_VGA 1
#define DRM_MODE_CONNECTOR_DisplayPort 10
#define DRM_MODE_CONNECTOR_HDMIA 11
#define DRM_MODE_CONNECTOR_eDP 14
#define DRM_MODE_DPMS_ON 0
#define DRM_MODE_DPMS_OFF 3

/* The switcheroo and fbdev states the backlight text compares with. */
#define DRM_SWITCH_POWER_CHANGING 3
#define FB_BLANK_UNBLANK 0
#define FB_BLANK_POWERDOWN 4

/*
 * The mode helpers of this environment have their own link names: the VBT
 * text has helpers of the same Linux names over its own mode type.
 */
#define drm_mode_set_name drv_i915_lcd_drm_mode_set_name

/*
 * drm_cvt_mode() is reached only through EDID_QUIRK_FORCE_REDUCED_BLANKING,
 * which no quirk table sets here.
 */
#define drm_cvt_mode(dev, h, v, r, reduced, interlaced, margins) ((struct drm_display_mode *)0)

/* Copies a mode, as the readout and the pipe-mode text do. */
#define drm_mode_copy(dst, src) (*(dst) = *(src))

/* ---- the platform: display version and predicates [fixed / device] ---- */

/*
 * The display version of the device the probe found (12 = Tiger Lake, 13 =
 * Alder Lake-P class).  The Linux DISPLAY_VER(); the i915 argument is not
 * evaluated: the version is the one drv_i915_lcd_display_ver() reports.
 */
#define I915_LCD_DISPLAY_VER(i915) (drv_i915_lcd_display_ver())

/* IS_ALDERLAKE_P() / IS_TIGERLAKE(): the device decides through its display version. */
#define I915_LCD_IS_ALDERLAKE_P(i915) (drv_i915_lcd_display_ver() >= 13)
#define I915_LCD_IS_TIGERLAKE(i915) (drv_i915_lcd_display_ver() == 12)

/* IS_DISPLAY_VER(): fixed to version 13, as this environment always answered. */
#define I915_LCD_IS_DISPLAY_VER(i915, from, until) (13 >= (from) && 13 <= (until))

/* The display runtime information of the device (the Linux DISPLAY_RUNTIME_INFO()). */
#define I915_LCD_DISPLAY_RUNTIME_INFO(i915) (&(i915)->display.runtime)

/* The platform's display information: the DBUF geometry and has_ddi. */
#define DISPLAY_INFO(i915) (&(i915)->display.device_info)

/* The platforms this path never runs on. */
#define IS_ELKHARTLAKE(i915) 0
#define IS_ALDERLAKE_S(i915) 0
#define IS_TIGERLAKE_UY(i915) (0)
#define IS_DG2(i915) 0
#define IS_DG1(i915) 0
#define IS_ROCKETLAKE(i915) 0
#define IS_JASPERLAKE(i915) 0
#define IS_ICELAKE(i915) 0
#define IS_CHERRYVIEW(i915) 0
#define IS_HASWELL(i915) 0
#define IS_BROADWELL(i915) 0
#define IS_GEMINILAKE(i915) 0
#define IS_BROXTON(i915) 0
#define IS_VALLEYVIEW(i915) 0
#define IS_G4X(i915) 0
#define IS_I830(i915) 0
#define IS_PLATFORM(i915, p) 0
#define HAS_VRR(i915) (I915_LCD_DISPLAY_VER(i915) >= 11)
#define HAS_GMCH(i915) 0
#define HAS_DP20(i915) 0
#define HAS_DSC(i915) 1
#define HAS_FLAT_CCS(i915) 0
#define IS_DISPLAY_STEP(i915, since, until) 0
#define STEP_B0 0
#define STEP_FOREVER 0

/*
 * intel_quirks.c: no entry of intel_quirks[] names the target's PCI device
 * and the DMI quirks name other machines, so no quirk is set.
 */
#define I915_LCD_INTEL_HAS_QUIRK(i915, quirk) (0)

/* The PCH the probe identified on the target (Alder Lake PCH). */
#define INTEL_PCH_TYPE(i915) PCH_ADP

/* A compile-time range check of the Linux register macros, and its constant test. */
#define BUILD_BUG_ON_ZERO(e) 0
#define __is_constexpr(x) 1

/* ---- register address helpers the text uses ---- */

/*
 * The transcoder register of transcoder `tran` at register `r` of the A
 * block: A..D sit at 0x60000 + n * 0x1000 (the Linux trans_offsets[]).
 */
#define _MMIO_TRANS2(tran, r) _MMIO(i915_lcd_trans_offset(tran) - 0x60000u + (r))

/* The pipe register blocks sit at the same 0x1000 spacing (the Linux pipe_offsets[]). */
#define _MMIO_PIPE2(pipe, r) _MMIO(0x1000u * (u32)((pipe) < 0 || (pipe) > 3 ? 0 : (pipe)) + (r))

/* Picks the register of instance `index` from an explicit list. */
#define _PICK(index, ...) (((const u32 []){ __VA_ARGS__ })[index])

/* The register of a transcoder, from the registers of transcoders A and B. */
#define _MMIO_TRANS(tran, a, b) _MMIO(_PICK_EVEN(tran, a, b))

/* The register of a plane, from the registers of planes 1 and 2. */
#define _PLANE(plane, a, b) _PICK_EVEN(plane, a, b)
#define _MMIO_PLANE(plane, a, b) _MMIO(_PLANE(plane, a, b))

/*
 * The display version < 5 branch of intel_cpu_transcoder_set_m1_n1() is
 * never taken; its register names only have to exist for the text.
 */
#define PIPE_DATA_M_G4X(pipe) INVALID_MMIO_REG
#define PIPE_DATA_N_G4X(pipe) INVALID_MMIO_REG
#define PIPE_LINK_M_G4X(pipe) INVALID_MMIO_REG
#define PIPE_LINK_N_G4X(pipe) INVALID_MMIO_REG

/*
 * Waits for a register condition through the backend (intel_de_wait_for_set
 * / _clear): 0, or I915_LCD_ETIMEDOUT / I915_LCD_EIO.
 */
#define intel_de_wait_for_set(i915, r, mask, ms) i915_lcd_wait(i915, r, mask, mask, ms)
#define intel_de_wait_for_clear(i915, r, mask, ms) i915_lcd_wait(i915, r, mask, 0u, ms)

/* ---- waits and sleeps [ops] ---- */

/*
 * Polls COND with growing sleeps until the budget is spent (the Linux
 * _wait_for(COND, US, Wmin, Wmax)).  The Linux budget is measured in
 * jiffies; here it is the sum of the sleeps requested (the backend decides
 * how long a sleep really is, never shorter).  COND is checked once more
 * after the budget is spent, as in Linux.  The sleeps go to the device the
 * caller names (the Linux text reached it through a file-scope variable).
 * Yields 0, or I915_LCD_ETIMEDOUT.
 */
#define I915_LCD_WAIT_FOR_POLL(i915, COND, US, Wmin, Wmax) ({ \
	long wait_left__ = (long)(US); \
	long wait__ = (Wmin); \
	int ret__; \
	for (;;) { \
		const bool expired__ = wait_left__ <= 0; \
		if (COND) { \
			ret__ = 0; \
			break; \
		} \
		if (expired__) { \
			ret__ = I915_LCD_ETIMEDOUT; \
			break; \
		} \
		i915_lcd_usleep_range((i915), wait__, wait__ * 2); \
		wait_left__ -= wait__; \
		if (wait__ < (Wmax)) \
			wait__ <<= 1; \
	} \
	ret__; })

/* wait_for(COND, MS) and wait_for_us(COND, US) of i915_utils.h, on the named device. */
#define I915_LCD_WAIT_FOR(i915, COND, MS) I915_LCD_WAIT_FOR_POLL((i915), (COND), (MS) * 1000, 10, 1000)
#define I915_LCD_WAIT_FOR_US(i915, COND, US) I915_LCD_WAIT_FOR_POLL((i915), (COND), (US), 10, 10)

/* ---- locks [ops] ---- */

/*
 * Takes and drops one of the device's display locks (the Linux mutex_lock()
 * / mutex_unlock() on i915->display.dpll.lock or .backlight.lock).  The lock
 * is named by its `which` member (I915_LCD_LOCK_*), and the backend's lock
 * hook of the named device takes it.  A macro because the lock members of
 * struct drm_i915_private are anonymous structures of their own.
 */
#define I915_LCD_MUTEX_LOCK(i915, m) \
	do { \
		if ((i915)->emit->lock != 0) \
			(i915)->emit->lock((i915)->emit->ctx, (m)->which, 1); \
	} while (0)
#define I915_LCD_MUTEX_UNLOCK(i915, m) \
	do { \
		if ((i915)->emit->lock != 0) \
			(i915)->emit->lock((i915)->emit->ctx, (m)->which, 0); \
	} while (0)

/* ---- display power [ops] ---- */

/* Gives a power reference back after a delay (the commit drops DC_OFF this way). */
#define intel_display_power_put_async_delay(i915, domain, wakeref, delay_ms) \
	(i915)->emit->power_put_async((i915)->emit->ctx, (int)(domain), (wakeref), (delay_ms))

/*
 * The power domains of an AUX channel.  For display version 12-13 the
 * platform's port-domain table puts AUX channel n at POWER_DOMAIN_AUX_A + n
 * and its I/O at POWER_DOMAIN_AUX_IO_A + n [fixed]; the Thunderbolt AUX
 * domain belongs to Type-C ports only.
 */
#define intel_display_power_legacy_aux_domain(i915, aux_ch) ((enum intel_display_power_domain)(POWER_DOMAIN_AUX_A + (int)(aux_ch)))
#define intel_display_power_tbt_aux_domain(i915, aux_ch) POWER_DOMAIN_INVALID
#define intel_display_power_aux_io_domain(i915, aux_ch) ((enum intel_display_power_domain)(POWER_DOMAIN_AUX_IO_A + (int)(aux_ch)))

/* [fixed] PSR is not enabled (crtc_state->has_psr stays false). */
#define intel_psr_needs_aux_io_power(encoder, crtc_state) (0)

/* DMC: which firmware ids are loaded is handed over by the DMC loader in display.dmc.fw_mask. */
#define has_dmc_id_fw(i915, dmc_id) ((((i915)->display.dmc.fw_mask) >> (dmc_id)) & 1u)

/* ---- [fixed] answers of the supported configuration ---- */

/* No big joiner, no MST, no Type-C port on the display outputs. */
#define intel_crtc_bigjoiner_slave_pipes(crtc_state) (0)
#define intel_crtc_is_bigjoiner_slave(crtc_state) (0)
#define intel_dp_mst_is_master_trans(crtc_state) (0)
#define intel_tc_port_in_dp_alt_mode(dig_port) (0)
#define intel_tc_port_in_legacy_mode(dig_port) (0)
#define is_trans_port_sync_mode(crtc_state) (0)

/* PCH transcoders do not exist on this platform. */
#define intel_crtc_pch_transcoder(crtc) ((enum pipe)0)

/*
 * The crtcs of the big-joiner slave pipes of a mask: the scan the modeset
 * text defined for the Linux for_each_intel_crtc_in_pipe_mask(), which
 * visits nothing.  It was never in effect at a use of that name: every
 * translation unit that used the name had the readout's registry walk or
 * the watermark walk defined after it (see takeover-internal.h and
 * watermark-internal.h), so this form is carried for completeness only.
 */
#define I915_LCD_FOR_EACH_INTEL_CRTC_IN_PIPE_MASK(dev, crtc, mask) \
	for ((crtc) = 0; (mask) != 0 && (crtc) != 0; (crtc) = 0)

/*
 * The crtc of a pipe: none (only a Haswell workaround asks).  The readout's
 * registry answer (takeover-internal.h) replaced this form before any use,
 * so it is carried for completeness only.
 */
#define I915_LCD_INTEL_CRTC_FOR_PIPE(i915, pipe) ((struct intel_crtc *)0)

/*
 * The DVI / HDMI level shift of the port (the Linux
 * intel_bios_hdmi_level_shift()): the VBT value of the modeset object the
 * named world's DDI hooks are bound to (the DDI text reached it through a
 * file-scope pointer).  devdata is not evaluated.
 */
#define I915_LCD_INTEL_BIOS_HDMI_LEVEL_SHIFT(world, devdata) drv_i915_lcd_hdmi_level_shift(world)

/* Linear framebuffers only: no CCS, no tiling, no DPT, no aux plane. */
#define is_surface_linear(fb, color_plane) ((fb)->modifier == FORMAT_MOD_LINEAR)
#define intel_tile_height(fb, color_plane) (1u)
#define intel_tile_width_bytes(fb, color_plane) (1u)
#define intel_fb_uses_dpt(fb) (0)
#define intel_fb_is_rc_ccs_cc_modifier(modifier) (0)
#define intel_format_info_is_yuv_semiplanar(info, modifier) (0)
#define skl_main_to_aux_plane(fb, color_plane) (0)

/*
 * The plane callees that are NOT ported (scaler, input CSC, black CSC):
 * named steps on the plane's own device.
 */
#define skl_program_plane_scaler(plane, crtc_state, plane_state) 	i915_lcd_to_i915((plane)->base.dev)->emit->step(i915_lcd_to_i915((plane)->base.dev)->emit->ctx, "skl_program_plane_scaler")
#define icl_program_input_csc(plane, crtc_state, plane_state) 	i915_lcd_to_i915((plane)->base.dev)->emit->step(i915_lcd_to_i915((plane)->base.dev)->emit->ctx, "icl_program_input_csc")
#define icl_plane_csc_load_black(plane) 	i915_lcd_to_i915((plane)->base.dev)->emit->step(i915_lcd_to_i915((plane)->base.dev)->emit->ctx, "icl_plane_csc_load_black")

/* The address the pinned buffer has in the GGTT (Linux: i915_ggtt_offset(ggtt_vma)). */
#define intel_plane_ggtt_offset(plane_state) ((plane_state)->ggtt_offset)

/* The HDMI sink of a DP port: none (no DP++ / PCON on these ports). */
#define intel_dp_has_hdmi_sink(intel_dp) (0)

/* No LSPCON behind these ports (lspcon.active stays false). */
#define dp_to_lspcon(intel_dp) (&i915_lcd_dp_to_dig_port(intel_dp)->lspcon)
#define lspcon_resume(dig_port) ((void)0)
#define lspcon_wait_pcon_mode(lspcon) ((void)0)

/* The DisplayPort 2.0 and panel-replay DPCD fields: not reached (DP 1.4 sinks). */
#define I915_LCD_PANEL_REPLAY_CONFIG 0
#define I915_LCD_DP_PANEL_REPLAY_ENABLE 0

/* The UHBR intra-hop check: only evaluated for a UHBR rate. */
#define intel_dp_128b132b_intra_hop(intel_dp, cs) (0)

/* The DISPLAY_VER >= 14 (Meteor Lake) half of the HDMI enable: never taken. */
#define mtl_get_port_width(lane_count) (0u)
#define XELPDP_PORT_WIDTH(width) (0u)
#define XELPDP_PORT_WIDTH_MASK 0u
#define XELPDP_PORT_REVERSAL 0u

/* The platform branches of the HDMI enable that display version 13 never takes. */
#define has_buf_trans_select(i915) (0)
#define hsw_prepare_hdmi_ddi_buffers(encoder, cs) ((void)0)
#define mtl_ddi_enable_d2d(encoder) ((void)0)
#define gen9_chicken_trans_reg_by_port(i915, port) CHICKEN_TRANS(0)

/* The HDMI infoframe types (hdmi.h). */
#define HDMI_INFOFRAME_TYPE_VENDOR 0x81
#define HDMI_INFOFRAME_TYPE_AVI 0x82
#define HDMI_INFOFRAME_TYPE_SPD 0x83
#define HDMI_INFOFRAME_TYPE_DRM 0x87
#define DP_SDP_VSC 0x07
#define HDMI_PACKET_TYPE_GAMUT_METADATA 0x0a

/* The identity of the connector object: this path passes the intel_connector itself. */
#define I915_LCD_TO_INTEL_CONNECTOR(c) (c)

/* The hardware frame counter's range; only a vblank-layer check reads it. */
#define intel_crtc_max_vblank_count(cs) (0xffffffffu)

/* The one-plane, linear-only colour path: no LUT loader is reached (colour text). */
#define LEGACY_LUT_LENGTH 256
#define PAL_PREC_INDEX_VALUE(x) (x)
#define drm_color_lut_size(blob) ((int)((blob)->length / 8u))

/* ---- the state checkers [check]: they only read back and warn ---- */
#define assert_shared_dpll_enabled(i915, pll) ((void)0)
#define assert_planes_disabled(crtc) ((void)0)
#define assert_pll_enabled(i915, pipe) ((void)0)
#define assert_dsi_pll_enabled(i915) ((void)0)
#define assert_fdi_rx_pll_enabled(i915, pipe) ((void)0)
#define assert_fdi_tx_pll_enabled(i915, pipe) ((void)0)

/* ---- the step hook: callees of the Linux text that are not ported ---- */

/*
 * Reports an unported callee of the Linux text as a named step at the
 * position the text calls it, through the step hook of the named device.
 */
#define I915_LCD_STEP(i915, name) ((i915)->emit->step((i915)->emit->ctx, name))

/*
 * Guards a callee whose Linux body begins with an early return that holds
 * in the supported configuration.  If the condition does not hold, the
 * unported rest is reported as an error, so the run is not a success.
 */
#define I915_LCD_GUARD(holds, what) \
	do { \
		if (!(holds)) \
			drv_i915_lcd_error("UNPORTED callee body reached: " what "\n"); \
	} while (0)

/* Records a callee that is deliberately not connected in the one-screen path. */
#define I915_LCD_DECIDED(i915, what) I915_LCD_STEP(i915, "(decided) " what)

/* Tells the backend that a named point of the commit was reached (enum i915_lcd_observe). */
#define I915_LCD_OBSERVE(i915, point) \
	do { \
		if ((i915)->emit->observe != 0) \
			(i915)->emit->observe((i915)->emit->ctx, (point)); \
	} while (0)

/* The device behind the objects the enable-sequence callers hold. */
#define I915_LCD_SEQ_I915_CRTC_STATE(cs) i915_lcd_to_i915((cs)->uapi.crtc->dev)
#define I915_LCD_SEQ_I915_ENCODER(e) i915_lcd_to_i915((e)->base.dev)

/* hsw_crtc_enable(): its state accessors (the atomic state is the one crtc state). */
#define intel_atomic_get_old_crtc_state(state, crtc) ((state)->old_crtc_state)
#define intel_atomic_get_new_crtc_state(state, crtc) ((struct intel_crtc_state *)(state)->crtc_state)

/*
 * The one encoder of the modeset (drm_encoder_mask() = 1 << index): the
 * Linux drm_for_each_encoder_mask() over the encoder the caller names (the
 * modeset text reached it through a file-scope variable).
 */
#define I915_LCD_DRM_FOR_EACH_ENCODER_MASK(only_encoder, encoder, mask) \
	for ((encoder) = (only_encoder); (encoder) != 0; (encoder) = 0) \
		for_each_if((mask) & (1u << (encoder)->index))

/*
 * GUARDS: callees whose Linux body begins with an early return that holds
 * in the supported configuration; the Linux condition is kept in meaning.
 */

/* icl_program_mg_dp_mode(): returns unless the PHY is Type-C. */
#define icl_program_mg_dp_mode(dig_port, cs) \
	I915_LCD_GUARD(!drv_i915_lcd_intel_phy_is_tc(I915_LCD_SEQ_I915_ENCODER(&(dig_port)->base), drv_i915_lcd_intel_port_to_phy(I915_LCD_SEQ_I915_ENCODER(&(dig_port)->base), (dig_port)->base.port)), "icl_program_mg_dp_mode (Type-C PHY)")

/* intel_dp_configure_protocol_converter(): returns for DPCD < 1.3 or a sink that is not a branch device. */
#define intel_dp_configure_protocol_converter(intel_dp, cs) \
	I915_LCD_GUARD((intel_dp)->dpcd[DP_DPCD_REV] < 0x13 || !dp_is_branch((intel_dp)->dpcd), "intel_dp_configure_protocol_converter (DPCD >= 1.3 branch device)")

/* Reached only with DSC: an error if it ever is. */
#define intel_dsc_power_domain(crtc, cpu_transcoder) (drv_i915_lcd_error("intel_dsc_power_domain reached: DSC is not part of this path" "\n"), POWER_DOMAIN_DISPLAY_CORE)
#define intel_vdsc_min_cdclk(cs) (drv_i915_lcd_error("intel_vdsc_min_cdclk reached: DSC is not part of this path" "\n"), 0)

/* IPS is behind IS_BROADWELL(). */
#define hsw_crtc_state_ips_capable(cs) (false)

/* DSC: each returns at once when crtc_state->dsc.compression_enable is false. */
#define intel_dp_sink_enable_decompression(state, connector, cs) I915_LCD_GUARD(!(cs)->dsc.compression_enable, "intel_dp_sink_enable_decompression (DSC)")
#define intel_dp_sink_disable_decompression(state, connector, cs) I915_LCD_GUARD(!(cs)->dsc.compression_enable, "intel_dp_sink_disable_decompression (DSC)")
#define intel_dsc_dp_pps_write(encoder, cs) I915_LCD_GUARD(!(cs)->dsc.compression_enable, "intel_dsc_dp_pps_write (DSC)")
#define intel_dsc_enable(cs) I915_LCD_GUARD(!(cs)->dsc.compression_enable, "intel_dsc_enable (DSC)")
#define intel_dsc_disable(cs) I915_LCD_GUARD(!(cs)->dsc.compression_enable, "intel_dsc_disable (DSC)")

/* intel_uncompressed_joiner_enable(): acts only with bigjoiner_pipes set. */
#define intel_uncompressed_joiner_enable(cs) I915_LCD_GUARD((cs)->bigjoiner_pipes == 0, "intel_uncompressed_joiner_enable (big joiner)")

/* FEC: each returns at once when crtc_state->fec_enable is false. */
#define intel_dp_sink_set_fec_ready(intel_dp, cs, enable) I915_LCD_GUARD(!(cs)->fec_enable, "intel_dp_sink_set_fec_ready (FEC)")
#define intel_ddi_enable_fec(encoder, cs) I915_LCD_GUARD(!(cs)->fec_enable, "intel_ddi_enable_fec (FEC)")
#define intel_ddi_wait_for_fec_status(encoder, cs, enabled) I915_LCD_GUARD(!(cs)->fec_enable, "intel_ddi_wait_for_fec_status (FEC)")

/* intel_dp_check_frl_training(): returns unless downstream_ports[2] has DP_PCON_SOURCE_CTL_MODE. */
#define intel_dp_check_frl_training(intel_dp) I915_LCD_GUARD(!((intel_dp)->downstream_ports[2] & 0x20), "intel_dp_check_frl_training (PCON, DP_PCON_SOURCE_CTL_MODE = bit 5)")

/* intel_dp_pcon_dsc_configure(): returns unless the sink is an HDMI 2.1 PCON. */
#define intel_dp_pcon_dsc_configure(intel_dp, cs) I915_LCD_GUARD(!dp_is_branch((intel_dp)->dpcd), "intel_dp_pcon_dsc_configure (HDMI 2.1 PCON)")

/* skl_pfit_enable() / skl_scaler_disable(): nothing without the pipe scaler. */
#define skl_pfit_enable(cs) I915_LCD_GUARD(!(cs)->pch_pfit.enabled, "skl_pfit_enable (panel fitter)")
#define skl_scaler_disable(cs) I915_LCD_GUARD(!(cs)->pch_pfit.enabled, "skl_scaler_disable (panel fitter)")

/* intel_audio_sdp_split_update(): a register write only with HAS_DP20. */
#define intel_audio_sdp_split_update(cs) I915_LCD_GUARD(!HAS_DP20(I915_LCD_SEQ_I915_CRTC_STATE(cs)), "intel_audio_sdp_split_update (DP 2.0)")

/* trans_port_sync_stop_link_train(): returns when sync_mode_slaves_mask is 0. */
#define trans_port_sync_stop_link_train(state, encoder, cs) I915_LCD_GUARD((cs)->sync_mode_slaves_mask == 0, "trans_port_sync_stop_link_train (port sync)")

/* intel_hdcp_enable() / _disable(): act only when content protection is asked for; eDP registers no HDCP shim. */
#define intel_hdcp_enable(state, encoder, cs, conn) I915_LCD_GUARD((conn)->content_protection == 0, "intel_hdcp_enable (content protection requested)")
#define intel_hdcp_disable(connector) ((void)0)

/* intel_tc_port_link_cancel_reset_work(): returns unless the PHY is Type-C. */
#define intel_tc_port_link_cancel_reset_work(dig_port) \
	I915_LCD_GUARD(!drv_i915_lcd_intel_phy_is_tc(I915_LCD_SEQ_I915_ENCODER(&(dig_port)->base), drv_i915_lcd_intel_port_to_phy(I915_LCD_SEQ_I915_ENCODER(&(dig_port)->base), (dig_port)->base.port)), "intel_tc_port_link_cancel_reset_work (Type-C port)")

/* Colour: the LUT / CSC loaders are reached only with a LUT blob, a non-8-bit gamma or a CSC. */
#define glk_load_degamma_lut(cs, blob) I915_LCD_GUARD((blob) == 0, "glk_load_degamma_lut (a degamma LUT)")
#define ilk_load_lut_8(cs, blob) I915_LCD_GUARD((blob) == 0, "ilk_load_lut_8 (a gamma LUT): returns at once without a blob")
#define icl_program_gamma_superfine_segment(cs) I915_LCD_GUARD(0, "icl_program_gamma_superfine_segment (12-bit multi-segment gamma)")
#define icl_program_gamma_multi_segment(cs) I915_LCD_GUARD(0, "icl_program_gamma_multi_segment (12-bit multi-segment gamma)")
#define ivb_load_lut_ext_max(cs) I915_LCD_GUARD(0, "ivb_load_lut_ext_max (10 / 12-bit gamma)")
#define glk_load_lut_ext2_max(cs) I915_LCD_GUARD(0, "glk_load_lut_ext2_max (10 / 12-bit gamma)")
#define bdw_load_lut_10(cs, blob, index) I915_LCD_GUARD(0, "bdw_load_lut_10 (10-bit gamma)")
#define ilk_update_pipe_csc(crtc, csc) I915_LCD_GUARD(0, "ilk_update_pipe_csc (a CTM)")
#define icl_update_output_csc(crtc, csc) I915_LCD_GUARD(0, "icl_update_output_csc (YCbCr output / limited range)")
#define intel_dsb_commit(dsb, wait) I915_LCD_GUARD(0, "intel_dsb_commit (a DSB)")

/* intel_psr_disable(): returns when crtc_state->has_psr is false. */
#define intel_psr_disable(intel_dp, cs) I915_LCD_GUARD(!(cs)->has_psr, "intel_psr_disable (PSR)")

/* drm_connector_update_privacy_screen(): returns without a privacy screen object. */
#define drm_connector_update_privacy_screen(conn) ((void)0)

/* intel_write_dp_sdp(): returns unless the SDP type is enabled in crtc_state->infoframes.enable. */
#define intel_write_dp_sdp(encoder, cs, type) I915_LCD_GUARD((cs)->infoframes.enable == 0, "intel_write_dp_sdp (an enabled SDP)")

/*
 * DECIDED: callees deliberately not connected in the one-screen path.
 *
 * intel_initial_watermarks() calls display.funcs.wm->initial_watermarks,
 * which skl_wm_funcs does not set: nothing happens there.  The watermarks
 * are written by the plane update (skl_write_plane_wm).
 */
#define intel_initial_watermarks(state, crtc) ((void)0)

/*
 * intel_set_cpu_fifo_underrun_reporting(): ADAPTATION of a diagnostic path.
 * The underrun interrupt is not unmasked; the LCD test owns ICL_PIPESTATUS
 * instead -- it saves it before the run, clears it where Linux would,
 * samples it per phase and logs every sample.
 */
#define intel_set_cpu_fifo_underrun_reporting(i915, pipe, enable) \
	do { \
		I915_LCD_DECIDED(i915, "intel_set_cpu_fifo_underrun_reporting: underrun IRQ stays masked; the test owns and samples ICL_PIPESTATUS"); \
		I915_LCD_OBSERVE(i915, (enable) ? I915_LCD_OBS_UNDERRUN_ARM : I915_LCD_OBS_UNDERRUN_DISARM); \
	} while (0)

/*
 * intel_crtc_vblank_on(): ADAPTATION of the synchronous path.  No DRM vblank
 * events, references or workers are used by the enable; frames are observed
 * through the pipe's hardware frame counter / scanline, and the pipe's
 * vblank interrupt stays masked (checked by the kernel binding).
 */
#define intel_crtc_vblank_on(cs) \
	I915_LCD_DECIDED(I915_LCD_SEQ_I915_CRTC_STATE(cs), "intel_crtc_vblank_on: DRM vblank events unused in this test; pipe vblank IRQ stays masked (checked by the binding)")

/*
 * intel_crtc_vblank_off(): the flip path has one pending event with a
 * reference, and it is settled here.  The screen whose event is settled is
 * the selected screen of the world the caller names (the modeset text
 * reached it through a file-scope selector).
 */
#define I915_LCD_INTEL_CRTC_VBLANK_OFF(world, cs) \
	do { \
		(void)(cs); \
		drv_i915_lcd_ms_vblank_off(world); \
	} while (0)

/*
 * XXX: UNPORTED -- the enable-sequence callees that are not ported.  Each is
 * a named step at the position Linux calls it.
 *   icl_ddi_bigjoiner_pre_enable: for each slave pipe of the master crtc
 *     state, enable its transcoder, select the master's DDI function and
 *     clock, and copy the master's timings.
 *   glk_pipe_scaler_clock_gating_wa: rmw CLKGATE_DIS_PSL(pipe), a Geminilake
 *     workaround (this display is version 13).
 *   ilk_pfit_enable: write PF_WIN_POS / PF_WIN_SZ from pch_pfit.dst and
 *     PF_CTL = PF_ENABLE | PF_FILTER_MED_3x3 (nothing is scaled here).
 *   intel_disable_primary_plane: plane->disable_arm() of the crtc's primary
 *     plane (the plane is programmed by this path's plane writer).
 */
#define icl_ddi_bigjoiner_pre_enable(state, cs) I915_LCD_STEP(I915_LCD_SEQ_I915_CRTC_STATE(cs), "icl_ddi_bigjoiner_pre_enable")
#define glk_pipe_scaler_clock_gating_wa(i915, pipe, enable) I915_LCD_STEP(i915, "glk_pipe_scaler_clock_gating_wa")
#define ilk_pfit_enable(cs) I915_LCD_STEP(I915_LCD_SEQ_I915_CRTC_STATE(cs), "ilk_pfit_enable")
#define intel_disable_primary_plane(cs) I915_LCD_STEP(I915_LCD_SEQ_I915_CRTC_STATE(cs), "intel_disable_primary_plane")

/*
 * XXX: UNPORTED -- the DDI callers' callees that are not ported.
 *   intel_tc_port_get_link / intel_tc_port_set_fia_lane_count: the Type-C
 *     link reference and its FIA lane count (both outputs are combo PHYs).
 *   intel_ddi_update_active_dpll: which of a Type-C port's PLLs the crtc
 *     uses (a combo PHY port has one, programmed directly).
 *   bxt_ddi_phy_set_lane_optim_mask: the Broxton PHY lane latency.
 *   intel_dp_128b132b_sdp_crc16: UHBR link SDP CRC (these links are 8b/10b).
 *   mtl_ddi_pre_enable_dp / hsw_ddi_pre_enable_dp: the display 14 and the
 *     display <= 11 DP enable (this display takes tgl_ddi_pre_enable_dp).
 *   intel_ddi_config_transcoder_dp2: TRANS_DP2_CTL (DP 2.0 only).
 */
#define intel_tc_port_get_link(dig_port, lanes) I915_LCD_STEP(I915_LCD_SEQ_I915_ENCODER(&(dig_port)->base), "intel_tc_port_get_link")
#define intel_ddi_update_active_dpll(state, encoder, crtc) I915_LCD_STEP(I915_LCD_SEQ_I915_ENCODER(encoder), "intel_ddi_update_active_dpll")
#define intel_tc_port_set_fia_lane_count(dig_port, lanes) I915_LCD_STEP(I915_LCD_SEQ_I915_ENCODER(&(dig_port)->base), "intel_tc_port_set_fia_lane_count")
#define bxt_ddi_phy_set_lane_optim_mask(encoder, mask) I915_LCD_STEP(I915_LCD_SEQ_I915_ENCODER(encoder), "bxt_ddi_phy_set_lane_optim_mask")
#define intel_dp_128b132b_sdp_crc16(intel_dp, cs) I915_LCD_STEP(I915_LCD_SEQ_I915_CRTC_STATE(cs), "intel_dp_128b132b_sdp_crc16")
#define mtl_ddi_pre_enable_dp(state, encoder, cs, conn) I915_LCD_STEP(I915_LCD_SEQ_I915_ENCODER(encoder), "mtl_ddi_pre_enable_dp")
#define hsw_ddi_pre_enable_dp(state, encoder, cs, conn) I915_LCD_STEP(I915_LCD_SEQ_I915_ENCODER(encoder), "hsw_ddi_pre_enable_dp")
#define intel_ddi_config_transcoder_dp2(encoder, cs) I915_LCD_STEP(I915_LCD_SEQ_I915_ENCODER(encoder), "intel_ddi_config_transcoder_dp2")

/*
 * XXX: UNPORTED -- the disable side's callees that are not ported, recorded
 * on the device the caller names (the modeset text named it through a
 * file-scope variable).
 *   ilk_pfit_disable: PF_CTL = PF_WIN_POS = PF_WIN_SZ = 0.
 *   mtl_disable_ddi_buf: the display 14 DDI buffer disable.
 *   adlp_tbt_to_dp_alt_switch_wa: an ADL-P Type-C workaround (DKL_PCS_DW5).
 *   intel_tc_port_put_link: the Type-C link reference given back.
 */
#define I915_LCD_ILK_PFIT_DISABLE(i915, cs) I915_LCD_STEP(i915, "ilk_pfit_disable")
#define I915_LCD_MTL_DISABLE_DDI_BUF(i915, encoder, cs) I915_LCD_STEP(i915, "mtl_disable_ddi_buf")
#define I915_LCD_ADLP_TBT_TO_DP_ALT_SWITCH_WA(i915, encoder) I915_LCD_STEP(i915, "adlp_tbt_to_dp_alt_switch_wa")
#define I915_LCD_INTEL_TC_PORT_PUT_LINK(i915, dig_port) I915_LCD_STEP(i915, "intel_tc_port_put_link")

/*
 * XXX: UNPORTED -- the HDMI sink-side halves that need DDC, recorded on the
 * device the caller names.
 *   drm_scdc_set_high_tmds_clock_ratio / drm_scdc_set_scrambling: read SCDC
 *     offset 0x20 over the sink's DDC, set or clear the scrambling / clock
 *     ratio bit, write it back (only above 340 MHz TMDS).
 *   drm_dp_dual_mode_set_tmds_output: a DP++ adaptor's TMDS output switch.
 *   intel_hdmi_set_gcp_infoframe: VIDEO_DIP_GCP and its DIP enable (8 bpc
 *     without deep colour writes nothing in Linux).
 *   intel_write_infoframe: pack the AVI / SPD / vendor frame into
 *     VIDEO_DIP_DATA and enable its send bit.
 */
#define I915_LCD_DRM_SCDC_SET_HIGH_TMDS_CLOCK_RATIO(i915, connector, set) (I915_LCD_STEP(i915, "drm_scdc_set_high_tmds_clock_ratio"), false)
#define I915_LCD_DRM_SCDC_SET_SCRAMBLING(i915, connector, enable) (I915_LCD_STEP(i915, "drm_scdc_set_scrambling"), false)
#define I915_LCD_DRM_DP_DUAL_MODE_SET_TMDS_OUTPUT(i915, drm, type, ddc, enable) I915_LCD_STEP(i915, "drm_dp_dual_mode_set_tmds_output")
#define I915_LCD_INTEL_HDMI_SET_GCP_INFOFRAME(i915, encoder, cs, conn) (I915_LCD_STEP(i915, "intel_hdmi_set_gcp_infoframe"), false)
#define I915_LCD_INTEL_WRITE_INFOFRAME(i915, encoder, cs, type, frame) I915_LCD_STEP(i915, "intel_write_infoframe")

/* ---- the DP link layer: DPCD access, link-training logs, fallback [ops] ---- */

/*
 * The Linux DPCD accessors on the named device's AUX hooks.  The aux
 * argument is not evaluated: the sink is the one the device's hooks reach.
 */
#define I915_LCD_DRM_DP_DPCD_READ(i915, aux, offset, buffer, size) i915_lcd_dpcd_read((i915), (offset), (buffer), (size))
#define I915_LCD_DRM_DP_DPCD_WRITE(i915, aux, offset, buffer, size) i915_lcd_dpcd_write((i915), (offset), (buffer), (size))
#define I915_LCD_DRM_DP_DPCD_READB(i915, aux, offset, valuep) i915_lcd_dpcd_readb((i915), (offset), (valuep))
#define I915_LCD_DRM_DP_DPCD_WRITEB(i915, aux, offset, value) i915_lcd_dpcd_writeb((i915), (offset), (value))
#define I915_LCD_DRM_DP_DPCD_PROBE(i915, aux, offset) i915_lcd_dpcd_probe((i915), (offset))

/* drm_dp_read_dpcd_caps(): executed by the DP part's own copy of the Linux function. */
#define I915_LCD_DRM_DP_READ_DPCD_CAPS(i915, aux, dpcd) \
	((i915)->emit->read_dpcd_caps != 0 ? (i915)->emit->read_dpcd_caps((i915)->emit->ctx, (dpcd)) : I915_LCD_EIO)

/* The link-training log lines: the format string reaches the debug or the error hook. */
#define lt_dbg(_intel_dp, _dp_phy, _format, ...) \
	do { \
		if (0) \
			(void)drv_i915_lcd_fmtcheck(_format, ##__VA_ARGS__); \
		drv_i915_lcd_debug("link training: " _format); \
	} while (0)
#define lt_err(_intel_dp, _dp_phy, _format, ...) \
	do { \
		if (0) \
			(void)drv_i915_lcd_fmtcheck(_format, ##__VA_ARGS__); \
		drv_i915_lcd_error("link training: " _format); \
	} while (0)

/*
 * Link-training FALLBACK is not ported.  When Linux asks for it, the fact
 * is recorded as an error and no other rate / lane count is tried.
 */
#define intel_dp_get_link_train_fallback_values(intel_dp, rate, lanes) \
	(drv_i915_lcd_error("link training failed: the reference requests a FALLBACK (lower rate / lane count), which is not ported\n"), -1)

/*
 * XXX: UNPORTED -- the modeset retry work Linux queues when link training
 * fails; this path reports the failure to its caller instead.
 *   pseudo: queue connector->modeset_retry_work, whose worker asks the DRM
 *   layer for a new modeset at a lower link rate.
 * The work and its queue are not evaluated; the step is recorded on the
 * device the caller names.
 */
#define I915_LCD_QUEUE_WORK(i915, wq, work) I915_LCD_STEP(i915, "queue_work(modeset_retry_work)")

/*
 * XXX: UNPORTED -- UHBR link training (128b/132b).  These links train
 * 8b/10b, which is ported.
 *   pseudo: set the 128b/132b TPS1 pattern, wait for the LT-tunable PHY
 *   repeaters, then poll DP_LANE_ALIGN_STATUS_UPDATED for
 *   INTERLANE_ALIGN_DONE within the sink's timeout, and report it.
 */
#define I915_LCD_INTEL_DP_128B132B_LINK_TRAIN(i915, intel_dp, cs, lttpr_count) (I915_LCD_STEP(i915, "intel_dp_128b132b_link_train"), false)

/* ---- the shared DPLL allocation (intel_dpll_mgr.h) ---- */

/*
 * The atomic state's shared_dpll[]: the device pool's state array of the
 * world the caller names (the Linux intel_atomic_get_shared_dpll_state()).
 */
#define I915_LCD_INTEL_ATOMIC_GET_SHARED_DPLL_STATE(world, state) drv_i915_lcd_shared_dpll_state(world)

/* Walks the device's pool of shared DPLLs. */
#define for_each_shared_dpll(i915, pll, id) \
	for ((id) = 0; (id) < (i915)->display.dpll.num_shared_dpll && ((pll) = &(i915)->display.dpll.shared_dplls[(id)]); (id)++)

/* ---- the synchronous plane update: the DRM vblank core and the kernel [ops] ---- */

/* display/intel_crtc.h: CONFIG_PROVE_LOCKING is off. */
#define VBLANK_EVASION_TIME_US 100

/* The non-debug build: dbg_vblank_evade() is empty. */
#define dbg_vblank_evade(crtc, end) ((void)(crtc), (void)(end))

/* The backend of a device, as the update helpers call it. */
#define I915_LCD_FLIP_OPS(i915) ((i915)->emit)

/* The DRM index of a crtc: its pipe. */
#define drm_crtc_index(c) ((unsigned int)to_intel_crtc(c)->pipe)

/* drm_crtc_vblank_get / _put of the pipe, through the backend. */
#define drm_crtc_vblank_get(c) \
	(I915_LCD_FLIP_OPS(i915_lcd_to_i915((c)->dev))->vblank_get(I915_LCD_FLIP_OPS(i915_lcd_to_i915((c)->dev))->ctx, (int)to_intel_crtc(c)->pipe))
#define drm_crtc_vblank_put(c) \
	I915_LCD_FLIP_OPS(i915_lcd_to_i915((c)->dev))->vblank_put(I915_LCD_FLIP_OPS(i915_lcd_to_i915((c)->dev))->ctx, (int)to_intel_crtc(c)->pipe)

/* The vblank wait queue of a crtc: only its address is passed around. */
#define drm_crtc_vblank_waitqueue(c) ((wait_queue_head_t *)(c))
#define DEFINE_WAIT(w) int w = 0
#define prepare_to_wait(wq, w, st) ((void)(wq), (void)(w))
#define finish_wait(wq, w) ((void)(wq), (void)(w))
#define TASK_UNINTERRUPTIBLE 0

/*
 * schedule_timeout() on the pipe's vblank wait queue: sleeps until the
 * pipe's next vblank interrupt or `t` ticks.  The device and the pipe are
 * the caller's (the modeset text named them through file-scope variables).
 */
#define I915_LCD_SCHEDULE_TIMEOUT(i915, pipe, t) \
	I915_LCD_FLIP_OPS(i915)->vblank_sleep(I915_LCD_FLIP_OPS(i915)->ctx, (pipe), (t))

/*
 * msecs_to_jiffies_timeout(): msecs_to_jiffies(m) + 1, counted in the display
 * code's own 10 ms tick.  It is not the kernel tick: vblank_sleep turns it
 * back into milliseconds, so it holds at any KERN_CLOCK_HZ.
 */
#define msecs_to_jiffies_timeout(m) ((long)(((m) + 9) / 10) + 1)

/*
 * local_irq_disable / _enable / _save / _restore around the short update
 * section, on the world and device the caller names.  The section state is
 * tracked so that save / restore nest the way the kernel primitives do.
 */
#define I915_LCD_LOCAL_IRQ_DISABLE(world, i915) drv_i915_lcd_irq_disable((world), (i915))
#define I915_LCD_LOCAL_IRQ_ENABLE(world, i915) drv_i915_lcd_irq_enable((world), (i915))
#define I915_LCD_LOCAL_IRQ_SAVE(world, i915, f) ((f) = drv_i915_lcd_irq_save((world), (i915)))
#define I915_LCD_LOCAL_IRQ_RESTORE(world, i915, f) drv_i915_lcd_irq_restore((world), (i915), (f))

/* drm_crtc_arm_vblank_event(): one pending event per pipe, completed by the pipe's next vblank. */
#define drm_crtc_arm_vblank_event(c, e) \
	I915_LCD_FLIP_OPS(i915_lcd_to_i915((c)->dev))->arm_event(I915_LCD_FLIP_OPS(i915_lcd_to_i915((c)->dev))->ctx, (int)to_intel_crtc(c)->pipe)

/* Paths of the update that are not reached (max_vblank_count is set, no vblank work, no PSR / VRR / DSI). */
#define drm_crtc_accurate_vblank_count(c) (drv_i915_lcd_error("drm_crtc_accurate_vblank_count reached (max_vblank_count is set)\n"), 0u)
#define drm_vblank_work_schedule(w, c, n) I915_LCD_GUARD(0, "drm_vblank_work_schedule (vblank work: colour updates only)")
#define intel_crtc_vblank_work_init(cs) I915_LCD_GUARD(0, "intel_crtc_vblank_work_init (vblank work: colour updates only)")
#define intel_crtc_needs_color_update(cs) (false)
#define intel_color_uses_dsb(cs) (false)
#define intel_psr_lock(cs) I915_LCD_GUARD(!(cs)->has_psr, "intel_psr_lock (PSR)")
#define intel_psr_unlock(cs) I915_LCD_GUARD(!(cs)->has_psr, "intel_psr_unlock (PSR)")
#define intel_psr_wait_for_idle_locked(cs) I915_LCD_GUARD(!(cs)->has_psr, "intel_psr_wait_for_idle_locked (PSR)")
#define intel_vgpu_active(i915) (false)
#define icl_dsi_frame_update(cs) I915_LCD_GUARD(0, "icl_dsi_frame_update (DSI)")
#define intel_vrr_send_push(cs) I915_LCD_GUARD(!(cs)->vrr.enable, "intel_vrr_send_push (VRR)")
#define intel_vrr_is_push_sent(cs) (false)
#define intel_vrr_vmin_vblank_start(cs) (drv_i915_lcd_error("VRR vblank start reached (VRR is not used)\n"), 0)
#define intel_vrr_vmax_vblank_start(cs) (drv_i915_lcd_error("VRR vblank start reached (VRR is not used)\n"), 0)
#define __intel_get_crtc_scanline_from_timestamp(c) (drv_i915_lcd_error("scanline from timestamp reached (DSI only)\n"), 0)
#define intel_vblank_section_enter(i915) ((void)0)
#define intel_vblank_section_exit(i915) ((void)0)

/* The timestamping constants the readout and the update record: the pipe's hardware mode. */
#define drm_calc_timestamping_constants(c, m) (i915_lcd_to_i915((c)->dev)->drm.vblank[drm_crtc_index(c)].hwmode = *(m))
#define drm_mode_init(dst, src) (*(dst) = *(src))

/* Time and tracepoints of the update: debug only, not kept. */
#define ktime_get() ((ktime_t)0)
#define ktime_us_delta(a, b) ((long long)((a) - (b)))
#define trace_intel_pipe_update_start(c) ((void)0)
#define trace_intel_pipe_update_vblank_evaded(c) ((void)0)
#define trace_intel_pipe_update_end(c, n, s) ((void)0)

/*
 * The vblank_time_lock and uncore sections of the update.  The modeset
 * object has one owner and runs serially, so these locks guard against
 * concurrent commits and timestamp readers that do not exist on this path.
 * The Linux names are kernel functions in zedBSD, hence the explicit names.
 */
#define I915_LCD_SPIN_LOCK_IRQSAVE(l, f) ((void)(l), (f) = 0)
#define I915_LCD_SPIN_UNLOCK_IRQRESTORE(l, f) ((void)(l), (void)(f))
#define I915_LCD_SPIN_LOCK_IRQ(l) ((void)(l))
#define I915_LCD_SPIN_UNLOCK_IRQ(l) ((void)(l))
#define I915_LCD_SPIN_LOCK(l) ((void)(l))
#define I915_LCD_SPIN_UNLOCK(l) ((void)(l))

/* ---- conversions between the objects ---- */
#define to_intel_plane(p) container_of(p, struct intel_plane, base)
#define to_intel_crtc(c) container_of(c, struct intel_crtc, base)
#define to_intel_encoder(e) ((struct intel_encoder *)(e))
#define to_intel_plane_state(x) ((struct intel_plane_state *)(x))

/* The DP half of an encoder's digital port. */
#define enc_to_intel_dp(encoder) (&i915_lcd_enc_to_dig_port(encoder)->dp)

/*
 * The power-domain mask and set of the Linux text.  It is included after
 * the macros because its structures are declared with DECLARE_BITMAP().
 */
#include "../intel/power-set.h"

/*
 * ==== Enums ====
 */

/* The pipes of the platform. */
enum pipe {
	INVALID_PIPE = -1,
	PIPE_A = 0,
	PIPE_B,
	PIPE_C,
	PIPE_D,
	I915_MAX_PIPES
};

/*
 * The transcoders and their registers.  Included after enum pipe because
 * the transcoders of the pipes are numbered after them.
 */
#include "../intel/trans.h"

/* Which PLL of a port a crtc uses (intel_display_types.h). */
enum icl_port_dpll_id {
	ICL_PORT_DPLL_DEFAULT,
	ICL_PORT_DPLL_MG_PHY,
	ICL_PORT_DPLL_COUNT
};

/* The kind of DP dual-mode adaptor behind an HDMI port (drm_dp_dual_mode_helper.h). */
enum drm_dp_dual_mode_type {
	DRM_DP_DUAL_MODE_NONE,
	DRM_DP_DUAL_MODE_TYPE1_DVI,
	DRM_DP_DUAL_MODE_TYPE1_HDMI,
	DRM_DP_DUAL_MODE_TYPE2_DVI,
	DRM_DP_DUAL_MODE_TYPE2_HDMI,
	DRM_DP_DUAL_MODE_LSPCON
};

/*
 * ==== Types ====
 */

struct drm_crtc_state;
struct drm_modeset_acquire_ctx;
struct drm_plane;
struct drm_plane_state;
struct i2c_adapter;
struct intel_atomic_state;
struct intel_cdclk_vals;
struct intel_connector;
struct intel_crtc;
struct intel_crtc_state;
struct intel_global_state;
struct intel_plane;
struct intel_plane_state;
struct intel_shared_dpll;
struct i915_wm_world;

/*
 * A power reference taken on a display power domain: the backend's cookie
 * (0 = none).
 */
typedef int intel_wakeref_t;

/* A time stamp of the vblank-evasion statistics (never read: debug only). */
typedef long long ktime_t;

/* A byte count or a negative errno of the DPCD accessors. */
typedef long i915_lcd_ssize_t;

/*
 * The vblank wait queue of the update helpers.  It is never defined: the
 * helpers only pass its address (the crtc's) to the no-op waits above.
 */
typedef struct i915_wq wait_queue_head_t;

/*
 * The selector of one of the device's display locks (the modeset
 * environment's struct mutex).
 *
 * `which` is an I915_LCD_LOCK_* value the backend's lock hook maps to a
 * real lock.  The Linux name collided with the kernel's struct mutex; no
 * object of this type is declared by the modeset text (the lock members of
 * struct drm_i915_private are anonymous structures of the same layout).
 */
struct i915_lcd_lock_selector {
	/* I915_LCD_LOCK_*. */
	int which;
};

/*
 * A link of an intrusive list (linux/list.h): only the mode's list head
 * uses it, and nothing links modes here.
 */
struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

/*
 * One display mode: the timings the EDID gave and the crtc timings the
 * hardware is programmed with (drm_modes.h).  The members the kept
 * functions use.
 */
struct drm_display_mode {
	struct list_head head;
	int clock;		/* kHz */
	u16 hdisplay;
	u16 hsync_start;
	u16 hsync_end;
	u16 htotal;
	u16 vdisplay;
	u16 vsync_start;
	u16 vsync_end;
	u16 vtotal;
	u32 flags;
	u8 type;
	u16 width_mm;
	u16 height_mm;
	char name[DRM_DISPLAY_MODE_LEN];
	u16 hskew;
	u16 vscan;
	/* the timings the hardware is programmed with (drm_mode_set_crtcinfo) */
	int crtc_clock;
	u16 crtc_hdisplay;
	u16 crtc_hblank_start;
	u16 crtc_hblank_end;
	u16 crtc_hsync_start;
	u16 crtc_hsync_end;
	u16 crtc_htotal;
	u16 crtc_hskew;
	u16 crtc_vdisplay;
	u16 crtc_vblank_start;
	u16 crtc_vblank_end;
	u16 crtc_vsync_start;
	u16 crtc_vsync_end;
	u16 crtc_vtotal;
};

struct drm_vblank_crtc;

/*
 * The DRM device of a modeset object: the vblank objects of its pipes and
 * the switcheroo state the backlight text reads.  The two locks are
 * placeholders the update helpers only pass to the no-op lock macros.
 */
struct drm_device {
	int unused;
	int switch_power_state;
	struct drm_vblank_crtc *vblank;
	int vblank_time_lock;
	int event_lock;
};

/*
 * The state of a connector in one commit: its colour space, the crtc it
 * drives, and what the HDCP and backlight text read (drm_connector.h).
 */
struct drm_connector_state {
	enum colorspace colorspace;
	void *connector;
	void *best_encoder;
	int content_protection;
	unsigned int max_bpc;
	struct drm_crtc *crtc;	/* the crtc the connector drives (intel_backlight_set_acpi reads it) */
};

/* The HDMI 2.0 scrambling capabilities of a sink (drm_connector.h). */
struct drm_scrambling {
	bool supported;
	bool low_rates;
};

/* The SCDC capabilities of an HDMI sink (drm_connector.h). */
struct drm_scdc {
	bool supported;
	bool read_request;
	struct drm_scrambling scrambling;
};

/* The HDMI part of a sink's display information (drm_connector.h). */
struct drm_hdmi_info {
	struct drm_scdc scdc;
};

/* What the EDID told about a sink: its quirks and its HDMI capabilities. */
struct drm_display_info {
	u32 quirks;
	struct drm_hdmi_info hdmi;
};

/* A connector as the EDID mode text sees it (drm_connector.h). */
struct drm_connector {
	struct {
		int id;
	} base;
	const char *name;
	struct drm_device *dev;
	struct drm_display_info display_info;
};

/* An EDID the mode text parses: the base block. */
struct drm_edid {
	const struct edid *edid;
};

/* The colour hooks of the device the commit reaches (intel_color.c). */
struct intel_color_funcs {
	void (*color_commit_noarm)(const struct intel_crtc_state *crtc_state);
	void (*color_commit_arm)(const struct intel_crtc_state *crtc_state);
	void (*load_luts)(const struct intel_crtc_state *crtc_state);
};

/*
 * The display hooks the readout and the noatomic disable reach
 * (intel_display_core.h); the takeover runner binds them.
 */
struct intel_display_funcs {
	bool (*get_pipe_config)(struct intel_crtc *crtc, struct intel_crtc_state *crtc_state);
	void (*crtc_enable)(struct intel_atomic_state *state, struct intel_crtc *crtc);
	void (*crtc_disable)(struct intel_atomic_state *state, struct intel_crtc *crtc);
};

/*
 * The device as the modeset text sees it: its backend and the display
 * members the kept functions read.  Each modeset object has one; the
 * readout builds another from the same configuration.
 */
struct drm_i915_private {
	struct drm_device drm;
	struct i915_lcd_emit *emit;
	struct {
		/* the device's shared DPLLs: ONE pool, seen by every modeset object (intel_get_shared_dpll_by_id) */
		struct {
			struct {
				int nssc;
			} ref_clks;
			struct {
				int which;
			} lock;
			struct intel_shared_dpll *shared_dplls;
			int num_shared_dpll;
		} dpll;
		struct {
			bool override_afc_startup;
			u8 override_afc_startup_val;
		} vbt;
		struct {
			bool ignore_long_hpd;
		} hotplug;
		struct {
			u16 skl_latency[8];
			u8 num_levels;
			bool ipc_enabled;
		} wm;
		/* the Linux DISPLAY_INFO()->dbuf */
		struct {
			struct {
				u32 size;
				u8 slice_mask;
			} dbuf;
			bool has_ddi;
		} device_info;
		struct {
			u8 block_time_us;
		} sagv;
		struct {
			const struct intel_color_funcs *color;
			const struct intel_display_funcs *display;
		} funcs;
		struct {
			u32 fw_mask;
		} dmc;
		/* the device's DBUF object the watermark readout reads (skl_wm_get_hw_state) */
		struct {
			struct {
				struct intel_global_state *state;
			} obj;
			u8 enabled_slices;
		} dbuf;
		/* the other global states the takeover clears (intel_bw.c / intel_pmdemand.c) */
		struct {
			struct {
				struct intel_global_state *state;
			} obj;
		} bw, pmdemand;
		struct {
			struct {
				int which;
			} lock;
		} backlight;
		/* DISPLAY_RUNTIME_INFO: rawclk in kHz (read out by the caller) */
		struct {
			u32 rawclk_freq;
			u8 pipe_mask;
		} runtime;
		/* the CDCLK state the normal initialisation left: table of the platform, reference / bypass clock, limit */
		struct {
			struct {
				struct intel_global_state *state;
			} obj;
			const struct intel_cdclk_vals *table;
			struct {
				unsigned int cdclk;
				unsigned int vco;
				unsigned int ref;
				unsigned int bypass;
				u8 voltage_level;
			} hw;
			unsigned int max_cdclk_freq;
		} cdclk;
		/* module parameter: 0 (default) */
		struct {
			int invert_brightness;
		} params;
	} display;
};

struct drm_crtc_funcs;

/*
 * A crtc as the DRM core sees it: its device, its state and its planes
 * (drm_crtc.h).
 */
struct drm_crtc {
	struct drm_device *dev;
	bool enabled;
	struct drm_crtc_state *state;
	struct drm_plane *primary;
	struct {
		int id;
	} base;
	const char *name;
	struct drm_plane *cursor;
	const struct drm_crtc_funcs *funcs;
};

/* A property value: only LUT blobs, always NULL in this path (drm_property.h). */
struct drm_property_blob {
	size_t length;
	void *data;
};

/*
 * The uapi half of a crtc's state in one commit: the modeset flags and the
 * copy of the hardware state the readout fills (drm_crtc.h).
 */
struct drm_crtc_state {
	struct drm_crtc *crtc;
	u32 plane_mask;
	bool mode_changed;
	bool active_changed;
	bool connectors_changed;
	bool async_flip;
	u32 encoder_mask;
	void *event;
	/* the uapi half the readout copies the hardware state into (intel_crtc_copy_hw_to_uapi_state) */
	bool enable;
	bool active;
	struct drm_display_mode mode;
	struct drm_display_mode adjusted_mode;
	unsigned int scaling_filter;
	const struct drm_property_blob *degamma_lut;
	const struct drm_property_blob *gamma_lut;
	const struct drm_property_blob *ctm;
	u32 connector_mask;	/* the connectors the readout found on this crtc */
};

/*
 * The i915 crtc of one pipe: whether it runs, the power domains it holds,
 * and the vblank-evasion bookkeeping of the update.
 */
struct intel_crtc {
	/* base is FIRST: to_intel_crtc(NULL) must be NULL, which the sanitize text relies on */
	struct drm_crtc base;
	enum pipe pipe;
	bool active;
	struct intel_display_power_domain_set enabled_power_domains;
	/* the power domains the READOUT found this crtc using (intel_modeset_setup.c) */
	struct intel_display_power_domain_set hw_readout_power_domains;
	u8 mode_flags;
	int scanline_offset;
	int vmax_vblank_start;
	struct {
		u32 min_vbl;
		u32 max_vbl;
		int scanline_start;
		long long start_vbl_time;
		u32 start_vbl_count;
	} debug;
	void *flip_done_event;
};

/* A rectangle in pixels, or in 16.16 fixed point for a plane source (drm_rect.h). */
struct drm_rect {
	int x1;
	int y1;
	int x2;
	int y2;
};

/* A port PLL a crtc may use and the state read back from it (intel_display_types.h). */
struct icl_port_dpll {
	struct intel_shared_dpll *pll;
	struct intel_dpll_hw_state hw_state;
};

/*
 * The complete state of one crtc in one commit: mode, link, PLL, planes,
 * watermarks, colour and infoframe state.  The enable writes it, the
 * disable reads it back as the old state, and the readout fills it from
 * the hardware.
 */
struct intel_crtc_state {
	/* uapi is FIRST: to_intel_crtc_state() is a cast of the uapi state in this path, as in the reference */
	struct drm_crtc_state uapi;
	struct icl_port_dplls_dummy {
		int unused;
	} *icl_port_dplls_unused;
	struct icl_port_dpll icl_port_dplls[ICL_PORT_DPLL_COUNT];
	struct intel_dpll_hw_state dpll_hw_state;
	struct {
		struct drm_display_mode mode;
		struct drm_display_mode adjusted_mode;
		struct drm_display_mode pipe_mode;
		const struct drm_property_blob *degamma_lut;
		const struct drm_property_blob *gamma_lut;
		const struct drm_property_blob *ctm;
		bool active;
		bool enable;
		unsigned int scaling_filter;
	} hw;
	int port_clock;
	int cpu_transcoder;		/* enum transcoder */
	int master_transcoder;
	int mst_master_transcoder;
	struct drm_rect pipe_src;
	unsigned int output_types;	/* bitmask of enum intel_output_type */
	enum intel_output_format output_format;
	int pipe_bpp;
	int lane_count;
	int fdi_lanes;
	u8 pixel_multiplier;
	u8 framestart_delay;
	bool has_pch_encoder;
	bool dither;
	bool limited_color_range;
	bool gamma_enable;
	bool csc_enable;
	bool enable_psr2_sel_fetch;
	bool ips_enabled;
	bool has_infoframe;
	bool has_panel_replay;
	u8 bigjoiner_pipes;
	u8 lane_lat_optim_mask;
	struct intel_shared_dpll *shared_dpll;
	enum pipe hsw_workaround_pipe;
	u16 linetime;
	u16 ips_linetime;
	bool double_wide;
	bool fec_enable;
	bool has_psr;
	bool has_audio;
	bool enhanced_framing;
	u8 min_voltage_level;
	bool update_m_n;
	bool update_lrr;
	bool preload_luts;
	bool do_async_flip;
	u8 mode_flags;
	bool inherited;			/* the state came from the READOUT, not from a check of ours */
	int min_cdclk[I915_MAX_PLANES];
	struct {
		bool enable;
		u8 link_count;
		u8 pixel_overlap;
	} splitter;
	struct {
		bool compression_enable;
	} dsc;
	u8 active_planes;
	u8 sync_mode_slaves_mask;
	/* watermarks / DDB of the crtc (intel_crtc_wm_state.skl), and the one plane of this path with its state */
	struct {
		struct {
			struct skl_pipe_wm raw;
			struct skl_pipe_wm optimal;
			struct skl_ddb_entry ddb;
			struct skl_ddb_entry plane_ddb[I915_MAX_PLANES];
			struct skl_ddb_entry plane_ddb_y[I915_MAX_PLANES];
		} skl;
	} wm;
	struct intel_plane *only_plane;
	const struct intel_plane_state *only_plane_state;
	u32 data_rate[I915_MAX_PLANES];
	u32 data_rate_y[I915_MAX_PLANES];
	u32 rel_data_rate[I915_MAX_PLANES];
	u32 rel_data_rate_y[I915_MAX_PLANES];
	u8 nv12_planes;
	u8 enabled_planes;
	bool wm_level_disabled;
	struct {
		int scaler_id;
	} scaler_state;
	/* colour management: no LUT / CTM blobs in this path (all NULL), modes computed by the reference's check */
	void *dsb;
	const struct drm_property_blob *pre_csc_lut;
	const struct drm_property_blob *post_csc_lut;
	u32 gamma_mode;
	u32 csc_mode;
	u8 c8_planes;
	struct {
		int unused;
	} csc, output_csc;
	/* the infoframes a readout would decode: only their presence is tracked here */
	struct {
		u32 enable;
		int avi;
		int spd;
		int hdmi;
		int drm;
	} infoframes;
	int pixel_rate;
	bool has_hdmi_sink;
	bool hdmi_scrambling;
	bool hdmi_high_tmds_clock_ratio;
	int max_link_bpp_x16;		/* the readout resets it (intel_crtc_state_reset) */
	int sink_format;		/* enum intel_output_format the readout found */
	struct {
		bool force_thru;
		bool enabled;
		struct drm_rect dst;
	} pch_pfit;
	struct intel_link_m_n dp_m_n;
	struct intel_link_m_n dp_m2_n2;
	struct intel_link_m_n fdi_m_n;
	struct {
		u16 flipline;
		u16 vmin;
		u16 vmax;
		u16 guardband;
		u16 pipeline_full;
		bool enable;
	} vrr;
};

/*
 * An encoder as the DRM core sees it; crtc is the crtc the readout found it
 * driving (drm_encoder.h).
 */
struct drm_encoder {
	struct drm_device *dev;
	struct {
		int id;
	} base;
	const char *name;
	unsigned index;
	struct drm_crtc *crtc;
};

/*
 * The i915 encoder of one DDI port: its output type, power domain and the
 * hooks hsw_crtc_enable() and the readout reach, bound as intel_ddi_init()
 * binds them.
 */
struct intel_encoder {
	struct drm_encoder base;
	int type;			/* enum intel_output_type */
	int power_domain;		/* enum intel_display_power_domain: intel_ddi_init() sets the port's DDI lanes domain */
	enum port port;
	/* the hooks hsw_crtc_enable() reaches through intel_encoders_*(); bound as intel_ddi_init() binds them */
	void (*pre_pll_enable)(struct intel_atomic_state *, struct intel_encoder *, const struct intel_crtc_state *, const struct drm_connector_state *);
	void (*pre_enable)(struct intel_atomic_state *, struct intel_encoder *, const struct intel_crtc_state *, const struct drm_connector_state *);
	void (*enable)(struct intel_atomic_state *, struct intel_encoder *, const struct intel_crtc_state *, const struct drm_connector_state *);
	void (*set_signal_levels)(struct intel_encoder *, const struct intel_crtc_state *);
	void (*disable)(struct intel_atomic_state *, struct intel_encoder *, const struct intel_crtc_state *, const struct drm_connector_state *);
	void (*post_disable)(struct intel_atomic_state *, struct intel_encoder *, const struct intel_crtc_state *, const struct drm_connector_state *);
	void (*post_pll_disable)(struct intel_atomic_state *, struct intel_encoder *, const struct intel_crtc_state *, const struct drm_connector_state *);
	/* the readout hooks the reference binds for a DDI encoder (intel_ddi_init) */
	bool (*get_hw_state)(struct intel_encoder *, enum pipe *pipe);
	bool (*is_clock_enabled)(struct intel_encoder *);	/* intel_ddi_init(): icl_ddi_combo_is_clock_enabled */
	void (*get_config)(struct intel_encoder *, struct intel_crtc_state *);
	void (*sync_state)(struct intel_encoder *, const struct intel_crtc_state *);
	void (*get_power_domains)(struct intel_encoder *, struct intel_crtc_state *);
	void (*enable_clock)(struct intel_encoder *, const struct intel_crtc_state *);
	void (*disable_clock)(struct intel_encoder *);
	const struct intel_ddi_buf_trans *(*get_buf_trans)(struct intel_encoder *, const struct intel_crtc_state *, int *n_entries);
};

/* The AUX channel of a DP port: only its name and device are read (drm_dp_helper.h). */
struct drm_dp_aux {
	const char *name;
	struct drm_device *drm_dev;
};

/* The backlight hooks of a panel (intel_display_types.h). */
struct intel_panel_bl_funcs {
	int (*setup)(struct intel_connector *connector, enum pipe pipe);
	u32 (*get)(struct intel_connector *connector, enum pipe pipe);
	void (*set)(const struct drm_connector_state *conn_state, u32 level);
	void (*disable)(const struct drm_connector_state *conn_state, u32 level);
	void (*enable)(const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state, u32 level);
	u32 (*hz_to_pwm)(struct intel_connector *connector, u32 hz);
};

/*
 * The panel behind a connector: the VBT data the modeset bodies read
 * (filled from the explicit VBT by the caller) and the backlight state.
 */
struct intel_panel {
	struct {
		struct {
			bool hobl;
			bool low_vswing;
		} edp;
		struct {
			u16 pwm_freq_hz;
			bool present;
			bool active_low_pwm;
			u8 min_brightness;
			s8 controller;
		} backlight;
	} vbt;
	struct {
		bool present;
		u32 level;
		u32 min;
		u32 max;
		bool enabled;
		bool combination_mode;
		bool active_low_pwm;
		u32 pwm_level_min;
		u32 pwm_level_max;
		bool pwm_enabled;
		u8 controller;
		/* no backlight class device here: always NULL */
		struct {
			struct {
				int brightness;
				int max_brightness;
				int power;
			} props;
		} *device;
		const struct intel_panel_bl_funcs *funcs;
		const struct intel_panel_bl_funcs *pwm_funcs;
	} backlight;
};

/*
 * The i915 connector of one output: its DRM part, the encoder it is
 * attached to, its readout hooks and its panel.
 */
struct intel_connector {
	struct {
		struct drm_device *dev;
		struct {
			int id;
		} base;
		const char *name;
		struct i2c_adapter *ddc;
		struct drm_display_info display_info;
		/* drm_connector: the state, the encoder the readout found, and the DPMS the sanitize sets */
		struct drm_connector_state *state;
		struct drm_encoder *encoder;
		int dpms;
		int connector_type;
		unsigned index;
	} base;
	/* the encoder this connector is attached to, its readout hook, and the sync the sanitize calls */
	struct intel_encoder *encoder;
	bool (*get_hw_state)(struct intel_connector *connector);
	void (*sync_state)(struct intel_connector *connector, const struct intel_crtc_state *crtc_state);
	struct intel_panel panel;
	int modeset_retry_work;
};

/*
 * The DP half of a digital port: the DDI_BUF_CTL value, the sink's
 * capabilities, the link parameters and the link-training hooks.
 */
struct intel_dp {
	u32 DP;
	struct intel_connector *attached_connector;
	bool hobl_failed;
	bool hobl_active;
	bool link_trained;
	int link_rate;
	u8 lane_count;
	u8 train_set[4];
	u8 dpcd[15];
	u8 edp_dpcd[3];
	struct drm_dp_aux aux;
	u8 downstream_ports[16];	/* DP_MAX_DOWNSTREAM_PORTS */
	u8 (*voltage_max)(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
	u8 (*preemph_max)(struct intel_dp *intel_dp);
	u8 lttpr_common_caps[8];	/* DP_LTTPR_COMMON_CAP_SIZE */
	u8 lttpr_phy_caps[8][3];	/* DP_MAX_LTTPR_COUNT x DP_LTTPR_PHY_CAP_SIZE */
	bool use_rate_select;
	bool reset_link_params;
	int num_sink_rates;
	int sink_rates[8];
	int max_link_rate;
	u8 max_link_lane_count;
	unsigned long last_oui_write;
	void (*prepare_link_retrain)(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
	void (*set_link_train)(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, u8 dp_train_pat);
	void (*set_idle_link_train)(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
};

/* The HDMI half of a digital port: what the HDMI enable / disable text reads. */
struct intel_hdmi {
	struct intel_connector *attached_connector;
	struct {
		enum drm_dp_dual_mode_type type;
		int max_tmds_clock;
	} dp_dual_mode;
};

/*
 * One DDI port: its encoder, its DP and HDMI halves, the port bits kept
 * from the readout, and the power references the enable holds.
 */
struct intel_digital_port {
	struct intel_encoder base;
	struct intel_dp dp;
	struct intel_hdmi hdmi;
	u32 saved_port_bits;
	int ddi_io_wakeref;
	int ddi_io_power_domain;
	int aux_wakeref;
	int aux_ch;			/* enum aux_ch: 0 = A */
	u8 max_lanes;
	struct intel_lspcon {
		bool active;
	} lspcon;
	void (*set_infoframes)(struct intel_encoder *, bool, const struct intel_crtc_state *, const struct drm_connector_state *);
};

/*
 * The atomic state of one commit, reduced to the one crtc state: the new
 * state of the enable and the old state of the disable.  The takeover's
 * throw-away state also carries the lock context and the internal mark.
 */
struct intel_atomic_state {
	struct {
		struct drm_device *dev;
	} base;
	const struct intel_crtc_state *crtc_state;
	const struct intel_crtc_state *old_crtc_state;
	/* the takeover's throw-away state (intel_modeset_setup.c): it holds the lock context and marks the state internal */
	struct drm_modeset_acquire_ctx *acquire_ctx;
	bool internal;
	/*
	 * The modeset world of the commit (NULL: not set).  The encoder walks
	 * (drv_i915_encoders_*(), ddi.c) reach the world's bound encoder
	 * through it, since their Linux signature carries no world.  Set by
	 * whoever builds the state: drv_i915_lcd_ms_bind_encoder() sets it in
	 * the modeset object's state; the takeover sets it in its throw-away
	 * state.
	 */
	struct i915_lcd_world *world;
};

/* The hooks of one shared DPLL kind (intel_dpll_mgr.h). */
struct intel_shared_dpll_funcs {
	bool (*get_hw_state)(struct drm_i915_private *i915, struct intel_shared_dpll *pll, struct intel_dpll_hw_state *hw_state);
	int (*get_freq)(struct drm_i915_private *i915, const struct intel_shared_dpll *pll, const struct intel_dpll_hw_state *pll_state);
	void (*enable)(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
	void (*disable)(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
};

/* The description of one shared DPLL of the platform (intel_dpll_mgr.h). */
struct dpll_info {
	const char *name;
	const struct intel_shared_dpll_funcs *funcs;
	enum intel_dpll_id id;
	int power_domain;		/* 0 = none (the reference's adlp_plls[] sets none) */
};

/* The per-PLL part of the atomic state: the pipes using it and its programmed state. */
struct intel_shared_dpll_state {
	u8 pipe_mask;
	struct intel_dpll_hw_state hw_state;
};

/*
 * One shared DPLL of the device pool: its state, the pipes that drive it
 * now, whether it is on, and the power reference it holds.
 */
struct intel_shared_dpll {
	struct intel_shared_dpll_state state;
	u8 active_mask;
	bool on;
	const struct dpll_info *info;
	intel_wakeref_t wakeref;
	enum intel_dpll_id index;	/* its place in the device's pool (shared_dplls[]) */
};

/* The format of a framebuffer: fourcc, planes and bytes per pixel (drm_fourcc.h). */
struct drm_format_info {
	u32 format;
	u8 num_planes;
	u8 cpp[4];
	bool has_alpha;
	bool is_yuv;
};

/* The framebuffer a plane scans out: its format and modifier (drm_framebuffer.h). */
struct drm_framebuffer {
	struct drm_device *dev;
	const struct drm_format_info *format;
	u64 modifier;
};

/* A plane as the DRM core sees it; the readout and sanitize read these (drm_plane.h). */
struct drm_plane {
	struct drm_device *dev;
	struct {
		int id;
	} base;
	const char *name;
	struct drm_plane_state *state;
	unsigned int type;		/* DRM_PLANE_TYPE_*: the sanitize keeps the primary plane */
};

/* The i915 plane of one pipe, with its readout and CDCLK hooks. */
struct intel_plane {
	struct drm_plane base;
	enum plane_id id;
	enum pipe pipe;
	bool async_flip;
	/* the readout hook of the plane (skl_plane_get_hw_state, plane.c) */
	bool (*get_hw_state)(struct intel_plane *plane, enum pipe *pipe);
	/* the reference asks the plane for its cdclk need; the readout only tests that the hook exists */
	int (*min_cdclk)(const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state);
};

/*
 * The state of one plane in one commit: source and destination, the
 * framebuffer, the computed control words and where the buffer is pinned.
 */
struct intel_plane_state {
	/* src is 16.16 fixed point */
	struct {
		struct drm_plane *plane;
		struct drm_rect src;
		struct drm_rect dst;
		bool visible;
	} uapi;
	struct {
		const struct drm_framebuffer *fb;
		unsigned int rotation;
		u16 alpha;
		u16 pixel_blend_mode;
		enum color_encoding color_encoding;
		enum color_range color_range;
	} hw;
	struct intel_sprite_colorkey ckey;
	struct {
		struct {
			u32 offset;
			unsigned int x;
			unsigned int y;
			unsigned int scanout_stride;
			unsigned int mapping_stride;
		} color_plane[4];
	} view;
	u32 ctl;
	u32 color_ctl;
	u32 cus_ctl;
	int scaler_id;
	bool force_black;
	bool decrypt;
	bool planar_slave;
	struct intel_plane *planar_linked_plane;
	u64 ccval;
	struct drm_rect psr2_sel_fetch_area;
	/* always NULL here: linear framebuffers do not use a DPT */
	const struct {
		struct {
			u64 start;
		} node;
	} *dpt_vma;
	u32 ggtt_offset;		/* zedBSD: where the pinned buffer sits in the GGTT (Linux: i915_ggtt_offset(ggtt_vma)) */
};

/* The vblank object of one pipe: its hardware mode and counter range (drm_vblank.h). */
struct drm_vblank_crtc {
	struct drm_display_mode hwmode;
	u32 max_vblank_count;
};

/* The crtc hooks the update helpers read: the hardware frame counter. */
struct drm_crtc_funcs {
	u32 (*get_vblank_counter)(struct drm_crtc *crtc);
};

/*
 * A device-wide global state object (intel_global_state.h), reduced to the
 * one object each modeset carries: the commit it belongs to and whether it
 * changed.
 */
struct intel_global_state {
	struct intel_atomic_state *state;
	bool changed;
};

/*
 * The global DBUF state of the Linux text.  It is included here because it
 * embeds struct intel_global_state above.
 */
#include "../intel/wm-dbuf.h"

/*
 * The global DBUF states of one modeset: the one before and the one after
 * the commit, and the crtc state the watermark text is working on.
 *
 * The watermark text reached it through a file-scope pointer; the explicit
 * forms of watermark-internal.h and takeover-internal.h take it as an
 * argument.
 */
struct i915_lcd_wm_ctx {
	struct intel_dbuf_state old_dbuf;
	struct intel_dbuf_state new_dbuf;
	struct intel_crtc_state *crtc_state;
};

/*
 * The one modeset object of one screen: one crtc / pipe, one encoder on a
 * combo PHY port, one shared DPLL, one primary plane on one framebuffer.
 *
 * The Linux callers (hsw_crtc_enable, the plane update, hsw_crtc_disable)
 * and every callee below them read and write these objects -- prepared
 * once, used by the enable, the plane update and the disable alike, so
 * what the enable left behind (intel_dp->DP, the wakerefs, the PLL's active
 * mask, link_trained, crtc->active) is what the disable finds.
 * Diagnostics read them too; nothing is copied into a second record.
 */
struct i915_lcd_modeset {
	int output_hdmi;		/* the output this state drives: an HDMI sink instead of the eDP panel */
	int hdmi_level_shift;		/* intel_bios_hdmi_level_shift() of this port (< 0 = not in the VBT) */
	int dpll_id;			/* the shared DPLL the reference's rule gave this crtc */
	unsigned also_active_pipes;	/* the other pipes of this configuration (cfg) */
	struct drm_i915_private i915;
	struct intel_crtc crtc;
	struct intel_crtc_state crtc_state;	/* the new state of the enable == the old state of the disable */
	struct intel_atomic_state state;
	struct intel_digital_port dig_port;
	struct intel_connector connector;
	struct drm_connector_state conn_state;
	struct intel_shared_dpll pll;
	struct dpll_info pll_info;
	struct intel_plane plane;
	struct intel_plane_state plane_state;
	struct drm_framebuffer fb;
	struct intel_plane cursor;	/* never shown; the reference reserves DDB space for it (skl_cursor_allocation) */
	struct i915_lcd_wm_ctx wm;	/* old / new global DBUF state */
	int wm_rc;
	/* the CDCLK state this crtc requires (bxt_modeset_calc_cdclk) against the current one */
	struct {
		int crtc_min;
		int bw_min;
		int min_cdclk;
		int cdclk;
		int vco;
		int voltage_level;
		int change_needed;
	} cdclk;
	int cdclk_rc;
	unsigned int bw_data_rate;	/* MB/s, as intel_bw_check_qgv_points() compares it */
	/* the commit */
	int dc_off_held;
	int dc_off_wakeref;
	u32 bl_user;			/* the backlight device's props.brightness */
	u32 bl_user_max;		/* the backlight device's props.max_brightness */
	int stop_unconfirmed;		/* the disable reported an error: nothing further was given back */
	unsigned commits;
	struct drm_vblank_crtc vblank[4];	/* dev->vblank[]: hwmode, max_vblank_count */
	int flip_event;			/* the event token of the pending update */
	u32 fb_fourcc;
	u32 fb_width;
	u32 fb_height;
	u32 fb_pitch;
	u64 fb_modifier;
	u32 cur_surf;
	u32 pend_surf;
	u32 old_surf;
	int flip_pending;
	int flip_stuck;
	unsigned flip_gen;
	int flip_event_ref;		/* the armed event still holds its vblank reference (taken in update_end) */
	unsigned events_cancelled;
	int prepared;
	int backlight_setup_rc;		/* intel_backlight_setup(): 0, or a negative errno */
	int plane_armed;		/* a PLANE_SURF write armed the plane: the buffer may be scanned out */
	/*
	 * The world this object belongs to (its ms_pool slot): set by prepare
	 * after it clears the object.  The update section of the plane update
	 * keeps its interrupt nesting there (i915_lcd_irqs_off), and the entry
	 * points that the Linux text reached through file-scope pointers
	 * record their device there (i915_lcd_cur_i915).
	 */
	struct i915_lcd_world *world;
};

/*
 * The state of the modeset environment: every long-lived variable the
 * modeset translation units kept at file scope.
 *
 * The display root (struct i915_display) holds a pointer to one of these,
 * allocated by the owner of the modeset path.  Unless a field says
 * otherwise, it is used only by the modeset path, which runs on the display
 * owner's thread one operation at a time; no lock protects it.  A zeroed
 * structure is the state the old variables had at load time, except
 * i915_lcd_ver, which starts at 13.
 */
struct i915_lcd_world {
	/*
	 * The modeset objects of the screens (modeset.c).  One per
	 * screen; prepare fills the selected one and the disable empties it.
	 */
	struct i915_lcd_modeset ms_pool[I915_LCD_MS_SCREENS];

	/* The backend each screen's commit runs on, bound by prepare. */
	struct i915_lcd_emit *ms_ops_pool[I915_LCD_MS_SCREENS];

	/* The first error the Linux text reported in each screen's current run (NULL: none yet). */
	const char *ms_first_error_pool[I915_LCD_MS_SCREENS];

	/* How many errors the Linux text reported in each screen's current run. */
	unsigned ms_errors_pool[I915_LCD_MS_SCREENS];

	/*
	 * Whether each screen's display was retained after an unconfirmed stop.
	 * It outlives prepare's clearing of the modeset object: only discarding
	 * the model clears it.
	 */
	int ms_retained_pool[I915_LCD_MS_SCREENS];

	/* The backend the retained display of each screen belongs to (with ms_retained_pool). */
	const struct i915_lcd_emit *ms_retained_ops_pool[I915_LCD_MS_SCREENS];

	/*
	 * The selected screen: the index into the pools above that every
	 * modeset entry point works on.  Set by the entry point before it calls
	 * into the Linux text.
	 */
	unsigned ms_sel;

	/*
	 * The display version of the device the probe found (12 or 13), which
	 * the platform predicates answer from.  Starts at 13; set once by the
	 * probe before the first modeset.
	 */
	int i915_lcd_ver;

	/*
	 * The new state of the disable commit: the old crtc state with the crtc
	 * made inactive (a function static of the commit's disable).  Filled at
	 * the start of each disable commit and read only inside it.
	 */
	struct intel_crtc_state i915_lcd_modeset_commit_disable_off_state;

	/*
	 * The device the Linux text works on: the selected modeset object's
	 * i915, set by every entry point before it calls in.  The explicit
	 * forms of this header take the device as an argument instead; the
	 * field lives until no caller reads it.
	 */
	struct drm_i915_private *i915_lcd_cur_i915;

	/*
	 * The one encoder of the modeset (drm_for_each_encoder_mask() visits
	 * it): the selected screen's encoder, bound with the device.
	 */
	struct drm_encoder *i915_lcd_only_encoder;

	/*
	 * The crtc state of the CPU-transcoder word recorder (a function
	 * static): cleared and filled at each call, read only inside it.
	 */
	struct intel_crtc_state i915_display_emit_cpu_transcoder_crtc_state;

	/* The device of the CPU-transcoder word recorder (a function static, as above). */
	struct drm_i915_private i915_display_emit_cpu_transcoder_i915;

	/* The device of the transcoder word recorder (a function static, as above). */
	struct drm_i915_private i915_display_emit_transcoder_i915;

	/*
	 * The modeset object whose encoder the DDI hooks are bound to (NULL:
	 * none).  Set when the encoder is bound, read by the VBT level-shift
	 * and bound-encoder accessors.
	 */
	struct i915_lcd_modeset *ddi_ms;

	/* The crtc state of the DDI word recorder (a function static): cleared and filled at each call. */
	struct intel_crtc_state i915_ddi_emit_crtc_state;

	/* The device of the DDI word recorder (a function static, as above). */
	struct drm_i915_private i915_ddi_emit_i915;

	/* The device of the combo PLL calculation (a function static): cleared at each call. */
	struct drm_i915_private i915_icl_dp_combo_pll_i915;

	/*
	 * The device's two combo PLLs (DPLL 0 and DPLL 1): one pool for the
	 * whole device, so two screens that want the same hardware state share
	 * one object and its active_mask counts the pipes that drive it.  Built
	 * on first use (i915_lcd_dpll_pool_inited), cleared when the pool is
	 * forgotten.
	 */
	struct intel_shared_dpll i915_lcd_dpll_pool[2];

	/* The descriptions of the pool's PLLs (name, hooks, id), built with the pool. */
	struct dpll_info i915_lcd_dpll_pool_info[2];

	/* The atomic state's shared_dpll[] of the pool: the pipes using each PLL and its state. */
	struct intel_shared_dpll_state i915_lcd_dpll_pool_state[2];

	/* Nonzero once the pool above is built; zero after it is forgotten. */
	int i915_lcd_dpll_pool_inited;

	/*
	 * The pipe of the plane update in progress: the pipe whose vblank the
	 * update's sleep waits for.  Set by the plane update before the
	 * vblank evasion.
	 */
	int i915_lcd_flip_pipe;

	/*
	 * Whether the update section has interrupts off (local_irq_*): 1 between
	 * the disable and the enable, so save / restore nest.
	 */
	int i915_lcd_irqs_off;

	/* The crtc state of the plane word recorder (a function static): cleared and filled at each call. */
	struct intel_crtc_state i915_plane_emit_crtc_state;

	/* The device of the plane word recorder (a function static, as above). */
	struct drm_i915_private i915_plane_emit_i915;

	/* The plane state of the plane word recorder (a function static, as above). */
	struct intel_plane_state i915_plane_emit_plane_state;

	/*
	 * The one mode object drm_mode_create() hands out to the EDID text
	 * (edid.c).  Taken by the first create, cleared when
	 * taken.
	 */
	struct drm_display_mode edid_mode_object;

	/* Nonzero while edid_mode_object is handed out. */
	int edid_mode_taken;

	/* The DRM device of the preferred-mode parser (a function static): never written after load. */
	struct drm_device i915_edid_preferred_mode_dev;

	/* How many notes (drm_dbg_kms, MISSING_CASE) the Linux text produced (state.c). */
	unsigned lcd_notes;

	/* How many errors (drm_err, WARN) the Linux text produced since the error hook was bound. */
	unsigned lcd_errors;

	/* The hook every error is handed to (NULL: counted only); bound with lcd_error_ctx. */
	void (*lcd_error_hook)(void *ctx, const char *what);

	/* The context of lcd_error_hook. */
	void *lcd_error_ctx;

	/* The sink a note is printed to while i915_lcd_note_trace is set (NULL: none). */
	void (*i915_lcd_note_sink)(const char *fmt);

	/* Nonzero: every note of the Linux text is printed as it is reached (a diagnostic switch). */
	int i915_lcd_note_trace;

	/*
	 * The register names the observation prints (diagnostics.c): filled
	 * for the screen's pipe, port and PLL each time the table is asked for.
	 */
	struct i915_lcd_named_reg table[32];
};

/*
 * ==== Forward declarations ====
 */

/*
 * The error sink of the Linux text (the modeset path's note and error
 * counters).  drv_i915_lcd_fmtcheck() is never called: it only lets the
 * compiler check a format against its arguments.
 */
int drv_i915_lcd_fmtcheck(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void drv_i915_lcd_note(const char *fmt);
void drv_i915_lcd_error(const char *what);
void drv_i915_lcd_debug(const char *what);

/* The display version of the device the probe found (12 = Tiger Lake, 13 = ADL-P class). */
int drv_i915_lcd_display_ver(void);

/* The mode object and name helpers of the EDID text. */
void drv_i915_lcd_drm_mode_set_name(struct drm_display_mode *mode);

/* The VBT level shift of the port the named world's DDI hooks are bound to (< 0: none). */
int drv_i915_lcd_hdmi_level_shift(struct i915_lcd_world *world);

/*
 * The PHY of a port and whether it is Type-C.  Explicit names: the hotplug
 * environment defines the Linux names as macros.
 */
bool drv_i915_lcd_intel_phy_is_tc(struct drm_i915_private *dev_priv, enum phy phy);
enum phy drv_i915_lcd_intel_port_to_phy(struct drm_i915_private *i915, enum port port);

/* The plane text's kept functions (watermark.c, plane.c). */
void skl_write_plane_wm(struct intel_plane *plane, const struct intel_crtc_state *crtc_state);

/*
 * local_irq_* of the update section: interrupts off / on through the named
 * device's hooks, with the nesting state kept in the named world.
 */
void drv_i915_lcd_irq_disable(struct i915_lcd_world *world, struct drm_i915_private *i915);
void drv_i915_lcd_irq_enable(struct i915_lcd_world *world, struct drm_i915_private *i915);
unsigned long drv_i915_lcd_irq_save(struct i915_lcd_world *world, struct drm_i915_private *i915);
void drv_i915_lcd_irq_restore(struct i915_lcd_world *world, struct drm_i915_private *i915, unsigned long was_off);

/* The pool's shared_dpll[] state array of the named world (the atomic state's shared DPLL state). */
struct intel_shared_dpll_state *drv_i915_lcd_shared_dpll_state(struct i915_lcd_world *world);

/* Settles the pending flip event of the named world's selected screen (intel_crtc_vblank_off()). */
void drv_i915_lcd_ms_vblank_off(struct i915_lcd_world *world);

/*
 * The entry points the owner files of the modeset object implement (the
 * Linux callers are static there).
 */
void drv_i915_lcd_ms_bind_pll(struct i915_lcd_modeset *ms, int dpll_id);
int drv_i915_lcd_ms_alloc_pll(struct i915_lcd_world *world, struct i915_lcd_modeset *ms, const struct intel_dpll_hw_state *hw_state);
void drv_i915_lcd_ms_release_pll(struct i915_lcd_world *world, struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_bind_encoder(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_bind_buf_trans(struct intel_encoder *encoder);
void drv_i915_lcd_ms_plane_data_rates(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_wm_compute_off(struct i915_wm_world *wm_world, struct i915_lcd_modeset *ms);
int drv_i915_lcd_ms_cdclk_check(struct i915_takeover_world *takeover, struct i915_lcd_modeset *ms);
int drv_i915_lcd_ms_bw_min_cdclk(struct i915_lcd_modeset *ms);
unsigned int drv_i915_lcd_ms_bw_data_rate(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_plane_min_cdclk(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_set_brightness(struct i915_lcd_modeset *ms, u32 user_level, u32 user_max);
void drv_i915_lcd_ms_backlight_power(struct i915_lcd_modeset *ms, int on);
u32 drv_i915_lcd_ms_user_level(struct i915_lcd_modeset *ms, u32 user_max);
int drv_i915_lcd_ms_wm_compute(struct i915_wm_world *wm_world, struct i915_lcd_modeset *ms);
int drv_i915_lcd_dbuf_current(struct i915_wm_world *wm_world, struct intel_dbuf_state *out);
void drv_i915_lcd_dbuf_publish(struct i915_wm_world *wm_world, const struct intel_dbuf_state *now);
void drv_i915_lcd_dbuf_forget(struct i915_wm_world *wm_world);
void drv_i915_lcd_ms_active_timings(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_plane_update_flip(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_color_check(struct i915_lcd_modeset *ms);
int drv_i915_lcd_ms_backlight_setup(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_crtc_enable(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_crtc_disable(struct i915_lcd_modeset *ms);
int drv_i915_lcd_ms_plane_prepare(struct i915_lcd_modeset *ms, u32 fourcc, u64 modifier, u32 width, u32 height, u32 pitch, u32 surf_ggtt_offset);
void drv_i915_lcd_ms_plane_update(struct i915_lcd_modeset *ms);
void drv_i915_lcd_ms_plane_disable(struct i915_lcd_modeset *ms);
int drv_i915_lcd_ms_evade_window(struct i915_lcd_modeset *ms, int *min, int *max, int *vblank_start);
void drv_i915_lcd_ms_set_acpi(struct i915_lcd_modeset *ms, u32 user_level, u32 user_max);

/*
 * P1 exports: the power, clock, PHY and DMC functions the other modeset
 * files call, under the names they are exported by (power.c, clock.c,
 * phy.c, dmc.c).
 */
void drv_i915_display_power_get_in_set(struct drm_i915_private *i915, struct intel_display_power_domain_set *power_domain_set, enum intel_display_power_domain domain);
void drv_i915_display_power_put_mask_in_set(struct drm_i915_private *i915, struct intel_display_power_domain_set *power_domain_set, struct intel_power_domain_mask *mask);
void drv_i915_enable_shared_dpll(const struct intel_crtc_state *crtc_state);
void drv_i915_disable_shared_dpll(const struct intel_crtc_state *crtc_state);
void drv_i915_lcd_ms_release_pipe(struct i915_lcd_world *world, enum pipe pipe);
void drv_i915_combo_phy_power_up_lanes(struct drm_i915_private *dev_priv, enum phy phy, bool is_dsi, int lane_count, bool lane_reversal);
bool drv_i915_is_hobl_buf_trans(const struct intel_ddi_buf_trans *table);
void drv_i915_dmc_enable_pipe(struct drm_i915_private *i915, enum pipe pipe);
void drv_i915_dmc_disable_pipe(struct drm_i915_private *i915, enum pipe pipe);

/* The HDMI infoframe writer of the port (hsw_set_infoframes), as dig_port->set_infoframes. */
void (*i915_lcd_hdmi_set_infoframes(void))(struct intel_encoder *, bool, const struct intel_crtc_state *, const struct drm_connector_state *);

/*
 * ==== Inline helpers ====
 */

/* Multiplies two 32-bit values into 64 bits (the Linux mul_u32_u32(), which the fixed-point text calls by name). */
static __inline u64
mul_u32_u32(
	u32 a,
	u32 b)
{
	/* The product of two 32-bit values always fits 64 bits. */
	return (u64)a * b;
}

/* Divides a 64-bit value by a 32-bit one (the Linux div_u64()). */
static __inline u64
i915_div_u64(
	u64 dividend,
	u32 divisor)
{
	/* Reports the truncated quotient. */
	return dividend / divisor;
}

/* Rounds a value up to a power of two (the Linux roundup_pow_of_two()). */
static __inline unsigned long
i915_roundup_pow_of_two(
	unsigned long n)
{
	unsigned long p;

	/* Doubles from 1 until the power reaches n. */
	p = 1;
	while (p < n)
		p <<= 1;

	/* Reports the smallest power of two not below n. */
	return p;
}

/*
 * Tells whether a crtc state asks for a full modeset (the Linux
 * drm_atomic_crtc_needs_modeset(), which the crtc-state inlines call by
 * name).
 */
static __inline bool
drm_atomic_crtc_needs_modeset(
	const struct drm_crtc_state *state)
{
	/* A new mode needs a modeset. */
	if (state->mode_changed)
		return true;

	/* Turning the crtc on or off needs one. */
	if (state->active_changed)
		return true;

	/* A new set of connectors needs one. */
	if (state->connectors_changed)
		return true;

	/* Succeeded: the state keeps the running mode. */
	return false;
}

/* Sets bit nr of a bitmap (the Linux set_bit()). */
static __inline void
i915_set_bit(
	unsigned nr,
	unsigned long *p)
{
	/* Sets the bit in the word that holds it. */
	p[nr / I915_BITS_PER_LONG] |= 1ul << (nr % I915_BITS_PER_LONG);
}

/* Clears bit nr of a bitmap (the Linux clear_bit()). */
static __inline void
i915_clear_bit(
	unsigned nr,
	unsigned long *p)
{
	/* Clears the bit in the word that holds it. */
	p[nr / I915_BITS_PER_LONG] &= ~(1ul << (nr % I915_BITS_PER_LONG));
}

/*
 * Tells whether bit nr of a bitmap is set (the Linux test_bit(), which the
 * power-domain walk calls by name).
 */
static __inline bool
test_bit(
	unsigned nr,
	const unsigned long *p)
{
	/* Tests the bit in the word that holds it. */
	if (((p[nr / I915_BITS_PER_LONG] >> (nr % I915_BITS_PER_LONG)) & 1ul) == 0ul)
		return false;

	/* Succeeded: the bit is set. */
	return true;
}

/* Clears every word of a bitmap (the Linux bitmap_zero()). */
static __inline void
i915_bitmap_zero(
	unsigned long *dst,
	unsigned nbits)
{
	unsigned i;

	/* Clears each word the bits occupy. */
	for (i = 0; i < (nbits + I915_BITS_PER_LONG - 1u) / I915_BITS_PER_LONG; i++)
		dst[i] = 0ul;
}

/* Stores a & ~b into dst (the Linux bitmap_andnot()). */
static __inline void
i915_bitmap_andnot(
	unsigned long *dst,
	const unsigned long *a,
	const unsigned long *b,
	unsigned nbits)
{
	unsigned i;

	/* Combines each word the bits occupy. */
	for (i = 0; i < (nbits + I915_BITS_PER_LONG - 1u) / I915_BITS_PER_LONG; i++)
		dst[i] = a[i] & ~b[i];
}

/* Tells whether every bit of a is set in b (the Linux bitmap_subset()). */
static __inline bool
i915_bitmap_subset(
	const unsigned long *a,
	const unsigned long *b,
	unsigned nbits)
{
	unsigned i;

	/* Looks for a bit of a that b does not have. */
	for (i = 0; i < (nbits + I915_BITS_PER_LONG - 1u) / I915_BITS_PER_LONG; i++) {
		if ((a[i] & ~b[i]) != 0ul)
			return false;
	}

	/* Succeeded: a is a subset of b. */
	return true;
}

/* The width of a rectangle (the Linux drm_rect_width()). */
static __inline int
i915_drm_rect_width(
	const struct drm_rect *r)
{
	/* Reports the horizontal extent. */
	return r->x2 - r->x1;
}

/* The height of a rectangle (the Linux drm_rect_height()). */
static __inline int
i915_drm_rect_height(
	const struct drm_rect *r)
{
	/* Reports the vertical extent. */
	return r->y2 - r->y1;
}

/* The word "on" or "off" for a log line (the Linux str_on_off()). */
static __inline const char *
i915_lcd_str_on_off(
	bool v)
{
	/* Names the state. */
	if (v)
		return "on";

	/* Succeeded: the state is off. */
	return "off";
}

/* The word "enable" or "disable" for a log line (the Linux str_enable_disable()). */
static __inline const char *
i915_str_enable_disable(
	bool v)
{
	/* Names the action. */
	if (v)
		return "enable";

	/* Succeeded: the action is a disable. */
	return "disable";
}

/*
 * The offset of the register block of transcoder `tran`: A..D sit at
 * 0x60000 + n * 0x1000 (the Linux trans_offsets[]); any other value
 * selects transcoder A.
 */
static __inline u32
i915_lcd_trans_offset(
	int tran)
{
	/* A transcoder outside A..D is treated as A. */
	if (tran < 0 || tran > 3)
		return 0x60000u;

	/* Succeeded: reports the block of the transcoder. */
	return 0x60000u + 0x1000u * (u32)tran;
}

/* The device of a DRM device (the Linux to_i915()). */
static __inline struct drm_i915_private *
i915_lcd_to_i915(
	const struct drm_device *dev)
{
	/* The DRM device is the first member of the device. */
	return container_of(dev, struct drm_i915_private, drm);
}

/* The digital port of an encoder (the Linux enc_to_dig_port()). */
static __inline struct intel_digital_port *
i915_lcd_enc_to_dig_port(
	const struct intel_encoder *encoder)
{
	/* The encoder is the base of its digital port. */
	return container_of(encoder, struct intel_digital_port, base);
}

/* The digital port of a DP half (the Linux dp_to_dig_port()). */
static __inline struct intel_digital_port *
i915_lcd_dp_to_dig_port(
	const struct intel_dp *intel_dp)
{
	/* The DP half is the dp member of its digital port. */
	return container_of(intel_dp, struct intel_digital_port, dp);
}

/* The device of a DP half (the Linux dp_to_i915()). */
static __inline struct drm_i915_private *
i915_lcd_dp_to_i915(
	const struct intel_dp *intel_dp)
{
	struct intel_digital_port *dig_port;
	struct drm_i915_private *i915;

	/* Finds the port and the device its encoder belongs to. */
	dig_port = i915_lcd_dp_to_dig_port(intel_dp);
	i915 = i915_lcd_to_i915(dig_port->base.base.dev);

	/* Succeeded: reports the device. */
	return i915;
}

/*
 * Tells whether a Type-C port is in Thunderbolt-alt mode (the Linux
 * intel_tc_port_in_tbt_alt_mode()): never, the ports here are combo PHYs.
 */
static __inline bool
i915_lcd_intel_tc_port_in_tbt_alt_mode(
	const struct intel_digital_port *dig_port)
{
	UNUSED_PARAMETER(dig_port);

	/* Succeeded: only a Type-C PHY has an alt mode. */
	return false;
}

/* Reads a display register through the device's backend (the Linux intel_de_read()). */
static __inline u32
i915_lcd_intel_de_read(
	const struct drm_i915_private *i915,
	i915_reg_t reg)
{
	u32 value;

	/* The pure word recorder has no read hook: a read there answers 0. */
	if (i915->emit->read32 == NULL)
		return 0u;

	/* Reads the register. */
	value = i915->emit->read32(i915->emit->ctx, reg.reg);

	/* Succeeded: reports the register's value. */
	return value;
}

/* Reads a display register without forcewake bookkeeping (the Linux intel_de_read_fw()). */
static __inline u32
i915_lcd_intel_de_read_fw(
	const struct drm_i915_private *i915,
	i915_reg_t reg)
{
	u32 value;

	/* The display registers need no forcewake: the same read as intel_de_read(). */
	value = i915_lcd_intel_de_read(i915, reg);

	/* Succeeded: reports the register's value. */
	return value;
}

/* Writes a display register through the device's backend (the Linux intel_de_write()). */
static __inline void
i915_lcd_intel_de_write(
	const struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 value)
{
	/* Hands the write to the backend (a model, the hardware, or a recorder). */
	i915->emit->write32(i915->emit->ctx, reg.reg, value);
}

/* Writes a display register without forcewake bookkeeping (the Linux intel_de_write_fw()). */
static __inline void
i915_lcd_intel_de_write_fw(
	const struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 value)
{
	/* The display registers need no forcewake: the same write as intel_de_write(). */
	i915_lcd_intel_de_write(i915, reg, value);
}

/*
 * Clears and sets bits of a display register (the Linux intel_de_rmw()) and
 * reports the value it had before.
 */
static __inline u32
i915_lcd_intel_de_rmw(
	const struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 clear,
	u32 set)
{
	u32 old_value;

	/* Hands the read-modify-write to the backend. */
	old_value = i915->emit->rmw32(i915->emit->ctx, reg.reg, clear, set);

	/* Succeeded: reports the value before the change. */
	return old_value;
}

/* Flushes posted writes with a read of the register (the Linux intel_de_posting_read()). */
static __inline void
i915_lcd_intel_de_posting_read(
	const struct drm_i915_private *i915,
	i915_reg_t reg)
{
	/* A backend without the hook has nothing to flush. */
	if (i915->emit->posting_read == NULL)
		return;

	/* Reads the register back. */
	i915->emit->posting_read(i915->emit->ctx, reg.reg);
}

/*
 * Waits until (register & mask) == value or the timeout passes: 0,
 * I915_LCD_ETIMEDOUT, or I915_LCD_EIO.  Without a wait hook (the pure word
 * recorder) the wait succeeds.
 */
static __inline int
i915_lcd_wait(
	const struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 mask,
	u32 value,
	unsigned timeout_ms)
{
	int wait_result;

	/* The pure word recorder has no wait hook: the condition counts as reached. */
	if (i915->emit->wait_reg == NULL)
		return 0;

	/* Waits through the backend. */
	wait_result = i915->emit->wait_reg(i915->emit->ctx, reg.reg, mask, value, timeout_ms);

	/* Reports a timeout or a fault of the wait. */
	if (wait_result != 0)
		return wait_result;

	/* Succeeded: the register reached the condition. */
	return 0;
}

/* Waits for a register condition (the Linux intel_de_wait_for_register()). */
static __inline int
i915_lcd_intel_de_wait_for_register(
	const struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 mask,
	u32 value,
	unsigned timeout_ms)
{
	int wait_result;

	/* Waits through the backend. */
	wait_result = i915_lcd_wait(i915, reg, mask, value, timeout_ms);

	/* Reports a timeout or a fault of the wait. */
	if (wait_result != 0)
		return wait_result;

	/* Succeeded: the register reached the condition. */
	return 0;
}

/*
 * Sleeps at least min_us on the named device's backend (the Linux
 * usleep_range(); the upper bound is not used).  The sleep may yield.
 */
static __inline void
i915_lcd_usleep_range(
	const struct drm_i915_private *i915,
	unsigned min_us,
	unsigned max_us)
{
	UNUSED_PARAMETER(max_us);

	/* A backend without the hook does not sleep. */
	if (i915->emit->usleep == NULL)
		return;

	/* Sleeps through the backend. */
	i915->emit->usleep(i915->emit->ctx, min_us);
}

/* Sleeps ms milliseconds on the named device's backend (the Linux msleep()). */
static __inline void
i915_lcd_msleep(
	const struct drm_i915_private *i915,
	unsigned ms)
{
	/* A millisecond sleep is a microsecond range of equal bounds. */
	i915_lcd_usleep_range(i915, ms * 1000u, ms * 1000u);
}

/* Busy-waits us microseconds on the named device's backend (the Linux udelay(); it does not yield). */
static __inline void
i915_lcd_udelay(
	const struct drm_i915_private *i915,
	unsigned us)
{
	/* A backend without the hook does not wait. */
	if (i915->emit->udelay == NULL)
		return;

	/* Waits through the backend. */
	i915->emit->udelay(i915->emit->ctx, us);
}

/*
 * Takes a reference on a display power domain (the Linux
 * intel_display_power_get()): the backend's nonzero cookie, 0 on failure,
 * or 1 when the backend keeps no power state.
 */
static __inline intel_wakeref_t
i915_lcd_intel_display_power_get(
	const struct drm_i915_private *i915,
	enum intel_display_power_domain domain)
{
	intel_wakeref_t wakeref;

	/* A backend without power hooks answers every get with the cookie 1. */
	if (i915->emit->power_get == NULL)
		return 1;

	/* Takes the reference through the backend. */
	wakeref = i915->emit->power_get(i915->emit->ctx, (int)domain);

	/* Reports a failed get (cookie 0) to the caller, which checks it. */
	if (wakeref == 0)
		return 0;

	/* Succeeded: reports the cookie the put gives back. */
	return wakeref;
}

/* Gives a display power reference back (the Linux intel_display_power_put()). */
static __inline void
i915_lcd_intel_display_power_put(
	const struct drm_i915_private *i915,
	enum intel_display_power_domain domain,
	intel_wakeref_t wakeref)
{
	/* A backend without the hook took no reference. */
	if (i915->emit->power_put == NULL)
		return;

	/* Gives the reference back through the backend. */
	i915->emit->power_put(i915->emit->ctx, (int)domain, wakeref);
}

/*
 * Runs one panel power sequencer operation (enum i915_lcd_panel_op) on the
 * named device's resident eDP: 0 when done.  Without a panel hook nothing
 * runs and the answer is 0.
 */
static __inline int
i915_lcd_panel(
	const struct drm_i915_private *i915,
	int op)
{
	int panel_result;

	/* A backend without a panel does nothing. */
	if (i915->emit->panel == NULL)
		return 0;

	/* Runs the operation through the backend. */
	panel_result = i915->emit->panel(i915->emit->ctx, op);

	/* Reports an operation the panel refused. */
	if (panel_result != 0)
		return panel_result;

	/* Succeeded: the operation ran. */
	return 0;
}

/* Turns the panel's power on (the Linux intel_pps_on()); the result is not looked at. */
static __inline void
i915_lcd_intel_pps_on(
	const struct drm_i915_private *i915,
	struct intel_dp *intel_dp)
{
	UNUSED_PARAMETER(intel_dp);

	/* Asks the panel power sequencer for the power-on sequence. */
	(void)i915_lcd_panel(i915, I915_LCD_PANEL_ON);
}

/* Turns the panel's power off (the Linux intel_pps_off()). */
static __inline void
i915_lcd_intel_pps_off(
	const struct drm_i915_private *i915,
	struct intel_dp *intel_dp)
{
	UNUSED_PARAMETER(intel_dp);

	/* Asks the panel power sequencer for the power-off sequence. */
	(void)i915_lcd_panel(i915, I915_LCD_PANEL_OFF);
}

/* Forces the panel's VDD on (the Linux intel_pps_vdd_on()). */
static __inline void
i915_lcd_intel_pps_vdd_on(
	const struct drm_i915_private *i915,
	struct intel_dp *intel_dp)
{
	UNUSED_PARAMETER(intel_dp);

	/* Asks the panel power sequencer to force VDD. */
	(void)i915_lcd_panel(i915, I915_LCD_PANEL_VDD_ON);
}

/* Drops the forced VDD at once (the Linux intel_pps_vdd_off_sync()). */
static __inline void
i915_lcd_intel_pps_vdd_off_sync(
	const struct drm_i915_private *i915,
	struct intel_dp *intel_dp)
{
	UNUSED_PARAMETER(intel_dp);

	/* Asks the panel power sequencer to drop VDD synchronously. */
	(void)i915_lcd_panel(i915, I915_LCD_PANEL_VDD_OFF_SYNC);
}

/* Sets the PPS backlight-enable bit (the Linux intel_pps_backlight_on()). */
static __inline void
i915_lcd_intel_pps_backlight_on(
	const struct drm_i915_private *i915,
	struct intel_dp *intel_dp)
{
	UNUSED_PARAMETER(intel_dp);

	/* Asks the panel power sequencer to enable the backlight. */
	(void)i915_lcd_panel(i915, I915_LCD_PANEL_BACKLIGHT_ON);
}

/* Clears the PPS backlight-enable bit (the Linux intel_pps_backlight_off()). */
static __inline void
i915_lcd_intel_pps_backlight_off(
	const struct drm_i915_private *i915,
	struct intel_dp *intel_dp)
{
	UNUSED_PARAMETER(intel_dp);

	/* Asks the panel power sequencer to disable the backlight. */
	(void)i915_lcd_panel(i915, I915_LCD_PANEL_BACKLIGHT_OFF);
}

/*
 * The device of an HDMI port (the Linux intel_hdmi_to_i915()): the device
 * the caller names, which the modeset text took from a file-scope variable.
 */
static __inline struct drm_i915_private *
i915_lcd_intel_hdmi_to_i915(
	struct drm_i915_private *i915,
	const struct intel_hdmi *hdmi)
{
	UNUSED_PARAMETER(hdmi);

	/* Reports the named device. */
	return i915;
}

/*
 * Tells whether the port's sink is connected (the Linux
 * intel_digital_port_connected()): always, for the internal panel; this
 * only selects the log level of lt_err.
 */
static __inline bool
i915_lcd_intel_digital_port_connected(
	const struct intel_encoder *encoder)
{
	UNUSED_PARAMETER(encoder);

	/* Succeeded: the internal panel is always connected. */
	return true;
}

/*
 * Reads DPCD bytes of the named device's sink (drm_dp_dpcd_read()): the
 * number of bytes transferred, or a negative errno (I915_LCD_EIO without a
 * DPCD hook).
 */
static __inline long
i915_lcd_dpcd_read(
	const struct drm_i915_private *i915,
	unsigned int offset,
	void *buffer,
	size_t size)
{
	long transferred;

	/* A backend without a sink cannot answer. */
	if (i915->emit->dpcd_read == NULL)
		return I915_LCD_EIO;

	/* Reads over the AUX channel. */
	transferred = i915->emit->dpcd_read(i915->emit->ctx, offset, buffer, size);

	/* Reports a failed transfer. */
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes read. */
	return transferred;
}

/* Writes DPCD bytes of the named device's sink (drm_dp_dpcd_write()), as i915_lcd_dpcd_read(). */
static __inline long
i915_lcd_dpcd_write(
	const struct drm_i915_private *i915,
	unsigned int offset,
	const void *buffer,
	size_t size)
{
	long transferred;

	/* A backend without a sink cannot answer. */
	if (i915->emit->dpcd_write == NULL)
		return I915_LCD_EIO;

	/* Writes over the AUX channel. */
	transferred = i915->emit->dpcd_write(i915->emit->ctx, offset, buffer, size);

	/* Reports a failed transfer. */
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes written. */
	return transferred;
}

/* Reads one DPCD byte (drm_dp_dpcd_readb()). */
static __inline long
i915_lcd_dpcd_readb(
	const struct drm_i915_private *i915,
	unsigned int offset,
	u8 *valuep)
{
	long transferred;

	/* Reads the byte. */
	transferred = i915_lcd_dpcd_read(i915, offset, valuep, 1);

	/* Reports a failed transfer. */
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes read. */
	return transferred;
}

/* Writes one DPCD byte (drm_dp_dpcd_writeb()). */
static __inline long
i915_lcd_dpcd_writeb(
	const struct drm_i915_private *i915,
	unsigned int offset,
	u8 value)
{
	long transferred;

	/* Writes the byte. */
	transferred = i915_lcd_dpcd_write(i915, offset, &value, 1);

	/* Reports a failed transfer. */
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes written. */
	return transferred;
}

/*
 * Tells whether the sink answers a one-byte read (drm_dp_dpcd_probe()): 0,
 * the negative errno of the read, or I915_LCD_EIO for a short read.
 */
static __inline int
i915_lcd_dpcd_probe(
	const struct drm_i915_private *i915,
	unsigned int offset)
{
	u8 byte;
	long transferred;

	/* Reads one byte at the offset. */
	transferred = i915_lcd_dpcd_read(i915, offset, &byte, 1);

	/* Reports a failed read. */
	if (transferred < 0)
		return (int)transferred;

	/* Reports a read that did not return the byte. */
	if (transferred != 1)
		return I915_LCD_EIO;

	/* Succeeded: the sink answered. */
	return 0;
}

/*
 * The crtc-state inlines of the Linux text (transcoder_is_dsi,
 * intel_crtc_has_type, intel_crtc_has_dp_encoder, intel_crtc_needs_modeset).
 * They are included last because they need the complete crtc state and
 * drm_atomic_crtc_needs_modeset() above.
 */
#include "../intel/ref-inlines.h"

#endif /* DRIVERS_GPU_I915_DISPLAY_MODESET_INTERNAL_H */
