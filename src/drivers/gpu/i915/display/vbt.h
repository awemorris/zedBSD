/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_bios.h),
 * which carries the following notice.
 *
 * Copyright © 2016-2019 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_gmbus.h),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

/*
 * The Linux environment of the VBT parser.
 *
 * The VBT parser (vbt.c) is the Linux intel_bios.c text.  This header supplies
 * what that text expects from the Linux kernel -- integer helpers, a list, an
 * allocator, logging, the platform predicates and the few DRM objects the
 * parser fills -- fixed to the one platform the driver drives (ADL-P, display
 * version 13, PCH ADP).  The Linux enums and structures the parser shares with
 * the rest of the driver come from intel/vbt.h; the VBT block layouts of
 * intel/vbt-defs.h are private to vbt.c, which includes them itself after
 * defining _INTEL_BIOS_PRIVATE.
 *
 * The DP environment (dp-internal.h) is layered on this one and replaces its
 * world name, so a DP file sees everything below as well.
 *
 * struct drm_i915_private is deliberately not defined here.  The parser works
 * on a device without the DP members, the DP files on one with them, and both
 * layouts carry the same Linux name: vbt.c defines the parser's layout next to
 * the parser's world state (struct i915_vbt_world), and dp-internal.h defines
 * the DP layout.  Everything below uses the structure only through a pointer.
 *
 * Linux helper names whose meaning differed between the old environments are
 * not defined under their Linux name.  Each meaning this environment gives
 * such a name has an explicit name of its own:
 *
 *	DISPLAY_VER(i915)		i915_vbt_display_ver()
 *	HAS_DDI(i915)			i915_vbt_has_ddi()
 *	IS_ALDERLAKE_P(i915)		i915_vbt_is_alderlake_p()
 *	IS_TIGERLAKE(i915)		i915_vbt_is_tigerlake()
 *	kfree(p)			i915_vbt_kfree()
 *	kmemdup(src, len, gfp)		i915_vbt_kmemdup()
 *	intel_port_to_phy(i915, port)	i915_vbt_intel_port_to_phy()
 *	intel_phy_is_tc(i915, phy)	i915_vbt_intel_phy_is_tc()
 *	clamp(v, lo, hi)		I915_VBT_CLAMP()
 *	drm_dbg_kms() and the other	I915_VBT_DRM_DBG_KMS() and the others
 *	message macros			below, which count into the parser's
 *					counters (the DP environment has its
 *					own, I915_DP_*)
 *
 * The parser reports a Linux errno only where the Linux text compares it;
 * I915_VBT_ENODEV is that number.  The functions vbt.c exports return zedBSD
 * positive errno values at their boundaries.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_VBT_H
#define DRIVERS_GPU_I915_DISPLAY_VBT_H

#include "internal.h"
#include <kern/kcrt.h>

#ifdef I915_DISPLAY_LINUX_WORLD
#error "display/vbt.h: another Linux environment is already included"
#endif

/* The Linux environment this translation unit is compiled in. */
#define I915_DISPLAY_LINUX_WORLD "vbt"

/* Marks the VBT environment for the headers layered on it. */
#define I915_DISPLAY_WORLD_VBT 1

/*
 * Marks a parameter a function does not use.
 *
 * The driver's own sources take it from ../i915.h; the display environments
 * do not include that header, so the same definition is repeated here.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/*
 * The Linux language helpers the parser text uses.
 */

/* Lays a structure out without padding, as the VBT bytes are. */
#define __packed __attribute__((packed))

/* Refuses to compile when a layout assumption does not hold. */
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON")

/* The allocation flags argument; the arena allocator ignores it. */
#define GFP_KERNEL 0

/* Marks a deliberate fall through to the next case label. */
#define fallthrough __attribute__((fallthrough))

/* The sparse address-space annotation; it means nothing to the compiler. */
#define __force

/* A 16-bit big-endian value as it is stored in the VBT. */
typedef u16 __be16;

/* Converts a big-endian 16-bit value to host order (the host is little endian). */
#define be16_to_cpu(x) ((u16)((((u16)(x)) >> 8) | (((u16)(x)) << 8)))

/*
 * Tells whether [start, start + size) does not fit below max, or wraps.
 *
 * This is the contract of the Linux overflow.h helper the parser uses to
 * check a block against the VBT size.
 */
#define range_overflows_t(type, start, size, max) \
	({ type _s = (type)(start), _z = (type)(size), _m = (type)(max); _s >= _m || _z > _m - _s; })

/* The size of a structure followed by n elements of its flexible array. */
#define struct_size(p, member, n) (sizeof(*(p)) + sizeof((p)->member[0]) * (n))

/*
 * Limits a value to [lo, hi] (the Linux clamp()).
 *
 * Every argument is evaluated once, through min() and max() of internal.h.
 */
#define I915_VBT_CLAMP(v, lo, hi) min(max(v, lo), hi)

/*
 * The Linux list primitives: the element that holds a list node, and the
 * walks over a list.  The list itself is struct list_head below.
 */

/* The structure a list node is embedded in. */
#define list_entry(ptr, type, member) container_of(ptr, type, member)

/* Walks every element of a list. */
#define list_for_each_entry(pos, head, member) \
	for (pos = list_entry((head)->next, __typeof__(*pos), member); \
	     &pos->member != (head); \
	     pos = list_entry(pos->member.next, __typeof__(*pos), member))

/* Walks every element of a list, allowing the current one to be removed. */
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = list_entry((head)->next, __typeof__(*pos), member), \
	     n = list_entry(pos->member.next, __typeof__(*pos), member); \
	     &pos->member != (head); \
	     pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

/*
 * Allocation from the parser's arena.
 *
 * Every allocation the parser makes comes from one device-owned bump arena
 * that vbt.c releases as a whole when the VBT is released; the size and the
 * flags arguments of the Linux calls are passed on or dropped as the Linux
 * contract allows.
 */
#define kzalloc(sz, gfp) drv_i915_vbt_zalloc(sz)
#define kmalloc(sz, gfp) drv_i915_vbt_zalloc(sz)

/*
 * Message levels.
 *
 * The DP environment uses the same levels for its own messages.
 */
#define I915_VBT_LOG_ERR 0
#define I915_VBT_LOG_INFO 1
#define I915_VBT_LOG_DEBUG 2

/*
 * Reports one parser message.
 *
 * The kernel has no vsnprintf, and the Linux text uses conversions the kernel
 * logger does not promise (%zu, %.*s), so a message is reported as its format
 * text only.  The arguments are type-checked against the format and never
 * evaluated.  Every error-level message is counted in the parser's counters.
 */
#define I915_VBT_LOG(level, fmt, ...) \
	do { \
		if (0) \
			(void)drv_i915_vbt_fmtcheck(fmt, ##__VA_ARGS__); \
		drv_i915_vbt_note(level, fmt); \
	} while (0)

/*
 * The Linux message macros in the parser's meaning.
 *
 * The device argument is never evaluated.  The WARN forms evaluate their
 * condition once and report its truth value, as in Linux.
 */
#define I915_VBT_DRM_DBG_KMS(drm, fmt, ...) I915_VBT_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_VBT_DRM_DBG(drm, fmt, ...) I915_VBT_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_VBT_DRM_INFO(drm, fmt, ...) I915_VBT_LOG(I915_VBT_LOG_INFO, fmt, ##__VA_ARGS__)
#define I915_VBT_DRM_NOTICE(drm, fmt, ...) I915_VBT_LOG(I915_VBT_LOG_INFO, fmt, ##__VA_ARGS__)
#define I915_VBT_DRM_ERR(drm, fmt, ...) I915_VBT_LOG(I915_VBT_LOG_ERR, fmt, ##__VA_ARGS__)
#define I915_VBT_DRM_WARN(drm, cond, fmt, ...) \
	({ int _w = !!(cond); if (_w) I915_VBT_LOG(I915_VBT_LOG_ERR, "WARN: " fmt, ##__VA_ARGS__); _w; })
#define I915_VBT_DRM_WARN_ON(drm, cond) \
	({ int _w = !!(cond); if (_w) I915_VBT_LOG(I915_VBT_LOG_ERR, "WARN_ON(%s)\n", #cond); _w; })
#define I915_VBT_WARN_ON(cond) I915_VBT_DRM_WARN_ON(0, cond)

/* The Linux drm_warn(): an error-level message without a condition. */
#define I915_VBT_DRM_WARN_MSG(drm, fmt, ...) I915_VBT_LOG(I915_VBT_LOG_ERR, fmt, ##__VA_ARGS__)

#define I915_VBT_DRM_DEBUG_KMS(fmt, ...) I915_VBT_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_VBT_DRM_DEBUG_DRIVER(fmt, ...) I915_VBT_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_VBT_MISSING_CASE(x) I915_VBT_LOG(I915_VBT_LOG_ERR, "Missing case (%s == %ld)\n", #x, (long)(x))

/*
 * The platform: ADL-P only.
 *
 * DISPLAY_VER, HAS_DDI, IS_ALDERLAKE_P and IS_TIGERLAKE are the functions at
 * the end of this header.  The device argument of the ones here is never
 * evaluated.
 */
#define IS_ALDERLAKE_S(i915) 0
#define IS_ROCKETLAKE(i915) 0
#define IS_DG1(i915) 0
#define IS_DGFX(i915) 0
#define IS_DG2(i915) 0
#define IS_JASPERLAKE(i915) 0
#define IS_ELKHARTLAKE(i915) 0
#define IS_ICELAKE(i915) 0
#define IS_ICL_WITH_PORT_F(i915) 0
#define IS_GEMINILAKE(i915) 0
#define IS_BROXTON(i915) 0
#define IS_BROADWELL(i915) 0
#define IS_HASWELL(i915) 0
#define IS_VALLEYVIEW(i915) 0
#define IS_CHERRYVIEW(i915) 0
#define IS_G4X(i915) 0
#define IS_PINEVIEW(i915) 0
#define IS_MOBILE(i915) 1
#define HAS_LSPCON(i915) 1
#define HAS_DISPLAY(i915) 1
#define HAS_PCH_SPLIT(i915) 1
#define HAS_PCH_TGP(i915) 0
#define HAS_PCH_MTP(i915) 0
#define INTEL_PCH_TYPE(i915) PCH_ADP
#define HAS_PCH_CNP(i915) 0

/*
 * The DPCD encodings parse_edp() stores.
 *
 * From Linux include/drm/display/drm_dp.h (the same text the DP environment
 * takes from intel/dp.h).
 */
#define DP_LINK_BW_1_62   0x06
#define DP_LINK_BW_2_7    0x0a
#define DP_LINK_BW_5_4    0x14
#define DP_LINK_BW_8_1    0x1e
#define DP_TRAIN_VOLTAGE_SWING_LEVEL_0 (0 << 0)
#define DP_TRAIN_VOLTAGE_SWING_LEVEL_1 (1 << 0)
#define DP_TRAIN_VOLTAGE_SWING_LEVEL_2 (2 << 0)
#define DP_TRAIN_VOLTAGE_SWING_LEVEL_3 (3 << 0)
#define DP_TRAIN_PRE_EMPH_LEVEL_0 (0 << 3)
#define DP_TRAIN_PRE_EMPH_LEVEL_1 (1 << 3)
#define DP_TRAIN_PRE_EMPH_LEVEL_2 (2 << 3)
#define DP_TRAIN_PRE_EMPH_LEVEL_3 (3 << 3)

/*
 * The GMBUS pin numbers the parser maps a child device's DDC pin to.
 *
 * From Linux drivers/gpu/drm/i915/display/intel_gmbus.h.
 */
#define GMBUS_PIN_DISABLED 0
#define GMBUS_PIN_1_BXT 1
#define GMBUS_PIN_2_BXT 2
#define GMBUS_PIN_3_BXT 3
#define GMBUS_PIN_4_CNP 4
#define GMBUS_PIN_9_TC1_ICP 9
#define GMBUS_PIN_10_TC2_ICP 10
#define GMBUS_PIN_11_TC3_ICP 11
#define GMBUS_PIN_12_TC4_ICP 12
#define GMBUS_PIN_13_TC5_TGP 13
#define GMBUS_PIN_14_TC6_TGP 14

/* The pre-ICP pins the parser's older pin tables name. */
#define GMBUS_PIN_SSC 1
#define GMBUS_PIN_VGADDC 2
#define GMBUS_PIN_PANEL 3
#define GMBUS_PIN_DPC 4
#define GMBUS_PIN_DPB 5
#define GMBUS_PIN_DPD 6
#define GMBUS_PIN_DPD_CHV 3
#define GMBUS_PIN_5_MTP 5

/*
 * The DRM mode fields fill_detail_timing_data() and parse_generic_dtd() write.
 *
 * From Linux include/drm/drm_modes.h.
 */
#define DRM_DISPLAY_MODE_LEN 32
#define DRM_MODE_TYPE_PREFERRED (1 << 3)
#define DRM_MODE_FLAG_PHSYNC (1 << 0)
#define DRM_MODE_FLAG_NHSYNC (1 << 1)
#define DRM_MODE_FLAG_PVSYNC (1 << 2)
#define DRM_MODE_FLAG_NVSYNC (1 << 3)

/* The format and the arguments the parser prints a mode with. */
#define DRM_MODE_FMT "\"%s\": %d %d %d %d %d %d %d %d %d 0x%x 0x%x"
#define DRM_MODE_ARG(m) (m)->name, 0, (m)->clock, (m)->hdisplay, (m)->hsync_start, \
	(m)->hsync_end, (m)->htotal, (m)->vdisplay, (m)->vsync_start, (m)->vsync_end, \
	(m)->vtotal - 0 + 0 * (int)(m)->type, (m)->flags

/* A placeholder the parser's MIPI sequence tables were extracted with. */
#define MIPI_SEQ_MAX_PLACEHOLDER 0

/* Visits every port whose bit is set in a port mask. */
#define for_each_port_masked(__port, __ports_mask) \
	for ((__port) = PORT_A; (__port) < I915_MAX_PORTS; (__port)++) \
		if (!((__ports_mask) & BIT(__port))) {} else

/* The letter Linux names a port by. */
#define port_name(p) ((p) + 'A')

/*
 * The Linux ENODEV number.
 *
 * The parser compares the panel type the OpRegion reports against it; see
 * i915_vbt_intel_opregion_get_panel_type().
 */
#define I915_VBT_ENODEV 19

/*
 * The orientation a panel is mounted in (Linux drm_connector.h).
 *
 * The parser stores it from the VBT's panel options.
 */
enum drm_panel_orientation {
	DRM_MODE_PANEL_ORIENTATION_UNKNOWN = -1,
	DRM_MODE_PANEL_ORIENTATION_NORMAL = 0,
	DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP,
	DRM_MODE_PANEL_ORIENTATION_LEFT_UP,
	DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

/*
 * One node of a Linux intrusive list, or the head of the list.
 *
 * The parser keeps its child devices and its BDB blocks on such lists inside
 * the parsed VBT; the nodes live in the parser's arena and go when the arena
 * is released.
 */
struct list_head {
	/* The next node; the head itself when this is the last one. */
	struct list_head *next;

	/* The previous node; the head itself when this is the first one. */
	struct list_head *prev;
};

/*
 * One display mode the parser builds from a VBT timing descriptor.
 *
 * Only the fields the parser writes are present.  A mode is allocated from
 * the parser's arena and lives until the VBT is released.
 */
struct drm_display_mode {
	/* The pixel clock, in kHz. */
	int clock;

	/* The horizontal timing, in pixels. */
	u16 hdisplay;
	u16 hsync_start;
	u16 hsync_end;
	u16 htotal;

	/* The vertical timing, in lines. */
	u16 vdisplay;
	u16 vsync_start;
	u16 vsync_end;
	u16 vtotal;

	/* DRM_MODE_FLAG_* sync polarities. */
	u32 flags;

	/* DRM_MODE_TYPE_* bits. */
	u8 type;

	/* The physical size of the panel, in millimetres. */
	u16 width_mm;
	u16 height_mm;

	/* "<hdisplay>x<vdisplay>", written by drv_i915_vbt_drm_mode_set_name(). */
	char name[DRM_DISPLAY_MODE_LEN];
};

/*
 * The base block of an EDID, as far as the parser reads it.
 *
 * pnpid_get_panel_type() matches the PnP id at offset 8 against the VBT's
 * panel table; the extension count and the checksum close the 128 bytes.  An
 * instance is never allocated: the parser reads the caller's EDID bytes
 * through a pointer of this type.
 */
struct edid {
	/* The fixed EDID header pattern. */
	u8 header[8];

	/* The manufacturer's PnP id, three 5-bit letters. */
	u8 mfg_id[2];

	/* The manufacturer's product code. */
	u8 prod_code[2];

	/* The serial number. */
	u32 serial;

	/* The week and the year of manufacture. */
	u8 mfg_week;
	u8 mfg_year;

	/* The rest of the base block, which the parser does not read. */
	u8 rest[108];

	/* The number of extension blocks that follow. */
	u8 extensions;

	/* The checksum of the base block. */
	u8 checksum;
} __packed;

/*
 * An EDID as the DRM core hands it to a driver.
 *
 * The parser only reaches the raw base block through it; the caller keeps the
 * bytes alive for the duration of the call.
 */
struct drm_edid {
	/* The raw EDID bytes, or NULL. */
	const struct edid *edid;
};

/*
 * The Linux intel_bios.h declarations (the backlight type, the eDP power
 * sequence, the MIPI structures and the parser's entry points) and the Linux
 * enums and data structures the parser fills: the port, AUX channel, PHY and
 * PCH enums, the per-panel VBT data and the device's VBT data.
 */
#include "../intel/vbt.h"

/*
 * The panel the parser fills for one connector.
 *
 * vbt.c keeps one instance for the panel it is initializing; the DP
 * environment embeds one in its connector.  The fixed EDID is not owned.
 */
struct intel_panel {
	/* The EDID the late panel initialization reads the PnP id from, or NULL. */
	const struct drm_edid *fixed_edid;

	/* What the VBT says about this panel. */
	struct intel_vbt_panel_data vbt;
};

/*
 * The DRM device the Linux text names in its messages.
 *
 * It carries nothing; it exists so that &i915->drm and container_of() back to
 * the device work as in Linux.
 */
struct drm_device {
	/* A placeholder: an empty structure is not ANSI C. */
	int unused;
};

/*
 * The device the Linux text works on.
 *
 * Its layout differs between the VBT parser (vbt.c defines it, without the DP
 * members) and the DP environment (dp-internal.h defines it, with them).
 */
struct drm_i915_private;

/*
 * The VBT parser's world state.
 *
 * It holds what the old parser kept in file-scope variables: the device the
 * parser works on, the allocation arena and its counters, the VBT bytes being
 * parsed, the message counters and the panel being initialized.  It is
 * defined in vbt.c, next to the parser's struct drm_i915_private, because it
 * embeds that structure by value.
 */
struct i915_vbt_world;

/*
 * The parser's arena allocator (vbt.c).
 *
 * drv_i915_vbt_zalloc() returns zeroed memory from the arena, or NULL when the
 * arena is exhausted (counted as an allocation failure); drv_i915_vbt_free() does
 * nothing, because the arena is released as a whole.
 */
void *drv_i915_vbt_zalloc(size_t bytes);
void drv_i915_vbt_free(void *p);

/*
 * The parser's message counters (vbt.c).
 *
 * drv_i915_vbt_log_enabled() counts an error-level message and tells whether the
 * level is shown; drv_i915_vbt_note() counts and shows one message's format text.
 */
int drv_i915_vbt_log_enabled(int level);
void drv_i915_vbt_note(int level, const char *fmt);

/*
 * The message hooks bios.c supplies.
 *
 * drv_i915_vbt_emit() writes one line to the kernel log; drv_i915_vbt_fmtcheck() only
 * exists so the compiler checks a message's arguments against its format.
 */
void drv_i915_vbt_emit(const char *text);
int drv_i915_vbt_fmtcheck(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Names a mode "<hdisplay>x<vdisplay>" (vbt.c).
 *
 * This is the parser's own drm_mode_set_name(): it writes this environment's
 * struct drm_display_mode, not the modeset environment's.
 */
void drv_i915_vbt_drm_mode_set_name(struct drm_display_mode *mode);

/*
 * The validated VBT bytes the parser is to read (vbt.c).
 *
 * NULL means that no VBT was found and the parser takes the missing-VBT
 * defaults.
 */
const void *drv_i915_vbt_provider_get(struct drm_i915_private *i915);

/* Makes a list head describe an empty list. */
static __inline void
i915_init_list_head(
	struct list_head *head)
{
	/* An empty list is a head whose neighbours are the head itself. */
	head->next = head;
	head->prev = head;
}

/* Tells whether a list has no element. */
static __inline int
i915_list_empty(
	const struct list_head *head)
{
	/* A head that points past itself has an element. */
	if (head->next != head)
		return 0;

	/* Succeeded: the list is empty. */
	return 1;
}

/* Appends a node at the end of a list. */
static __inline void
i915_list_add_tail(
	struct list_head *node,
	struct list_head *head)
{
	/* Links the node between the current last node and the head. */
	node->prev = head->prev;
	node->next = head;
	head->prev->next = node;
	head->prev = node;
}

/* Removes a node from its list and leaves it as a list of its own. */
static __inline void
i915_list_del(
	struct list_head *node)
{
	/* Joins the neighbours around the node. */
	node->prev->next = node->next;
	node->next->prev = node->prev;

	/* Leaves the node pointing at itself, so a second removal is harmless. */
	node->next = node;
	node->prev = node;
}

/* Releases parser memory (the Linux kfree(), which the arena makes a no-op). */
static __inline void
i915_vbt_kfree(
	const void *p)
{
	/* Hands the pointer to the arena, which keeps it until the VBT is released. */
	drv_i915_vbt_free((void *)p);
}

/* Copies a byte range into newly allocated parser memory (the Linux kmemdup()). */
static __inline void *
i915_vbt_kmemdup(
	const void *src,
	size_t len,
	int gfp)
{
	void *copy;

	UNUSED_PARAMETER(gfp);

	/* Allocates the copy from the parser's arena. */
	copy = drv_i915_vbt_zalloc(len);
	if (copy == NULL)
		return NULL;

	/* Copies the source bytes into it. */
	kern_memcpy(copy, src, len);

	/* Succeeded: the caller owns the copy until the arena is released. */
	return copy;
}

/* The raw EDID bytes of a DRM EDID, or NULL. */
static __inline const struct edid *
i915_drm_edid_raw(
	const struct drm_edid *drm_edid)
{
	/* No DRM EDID means no raw bytes. */
	if (drm_edid == NULL)
		return NULL;

	/* Succeeded: reports the raw base block. */
	return drm_edid->edid;
}

/* Decodes an EDID manufacturer id into its three letters ('A' is 1). */
static __inline const char *
i915_drm_edid_decode_mfg_id(
	u16 mfg_id,
	char vend[4])
{
	/* Each letter is five bits, the first one highest. */
	vend[0] = (char)('@' + ((mfg_id >> 10) & 0x1f));
	vend[1] = (char)('@' + ((mfg_id >> 5) & 0x1f));
	vend[2] = (char)('@' + ((mfg_id >> 0) & 0x1f));
	vend[3] = '\0';

	/* Succeeded: reports the buffer the letters were written to. */
	return vend;
}

/* The PHY of a port: ADL-P maps ports A..C to combo PHYs A..C and TC1..TC4 to PHYs F..I. */
static __inline enum phy
i915_vbt_intel_port_to_phy(
	struct drm_i915_private *i915,
	enum port port)
{
	UNUSED_PARAMETER(i915);

	/* A Type-C port starts at PHY F. */
	if (port >= PORT_TC1)
		return (enum phy)(PHY_F + (port - PORT_TC1));

	/* Succeeded: a combo port has the PHY of the same letter. */
	return (enum phy)(PHY_A + (port - PORT_A));
}

/* Tells whether a PHY is a Type-C PHY (F..I on ADL-P). */
static __inline bool
i915_vbt_intel_phy_is_tc(
	struct drm_i915_private *i915,
	enum phy phy)
{
	UNUSED_PARAMETER(i915);

	/* PHYs below F are combo PHYs. */
	if (phy < PHY_F)
		return false;

	/* PHYs above I do not exist on ADL-P. */
	if (phy > PHY_I)
		return false;

	/* Succeeded: the PHY is a Type-C PHY. */
	return true;
}

/*
 * Tells whether a GMBUS pin exists.
 *
 * A PCH of ICP or later uses the Linux gmbus_pins_icp table: pins 1..3 and
 * 9..14.
 */
static __inline bool
i915_vbt_intel_gmbus_is_valid_pin(
	struct drm_i915_private *i915,
	unsigned int pin)
{
	UNUSED_PARAMETER(i915);

	/* The first range: the BXT pins 1, 2 and 3. */
	if (pin >= 1u && pin <= 3u)
		return true;

	/* The second range: the Type-C pins 9 to 14. */
	if (pin >= 9u && pin <= 14u)
		return true;

	/* Any other pin does not exist. */
	return false;
}

/*
 * The panel type the OpRegion names.
 *
 * The parser runs without an OpRegion in this configuration (ASLS is 0), so
 * this is the Linux answer for a missing OpRegion: -ENODEV.
 */
static __inline int
i915_vbt_intel_opregion_get_panel_type(
	struct drm_i915_private *i915)
{
	UNUSED_PARAMETER(i915);

	/* Reports that no OpRegion names a panel type. */
	return -I915_VBT_ENODEV;
}

/* The display version of the device (the Linux DISPLAY_VER()): 13 on ADL-P. */
static __inline int
i915_vbt_display_ver(
	const struct drm_i915_private *i915)
{
	UNUSED_PARAMETER(i915);

	/* Reports ADL-P's display version. */
	return 13;
}

/* Tells whether the device has DDI outputs (the Linux HAS_DDI()): ADL-P has. */
static __inline int
i915_vbt_has_ddi(
	const struct drm_i915_private *i915)
{
	UNUSED_PARAMETER(i915);

	/* Reports that ADL-P drives its outputs through DDIs. */
	return 1;
}

/* Tells whether the device is an ADL-P (the Linux IS_ALDERLAKE_P()): it is. */
static __inline int
i915_vbt_is_alderlake_p(
	const struct drm_i915_private *i915)
{
	UNUSED_PARAMETER(i915);

	/* Reports that the only platform driven is ADL-P. */
	return 1;
}

/* Tells whether the device is a TGL (the Linux IS_TIGERLAKE()): it is not. */
static __inline int
i915_vbt_is_tigerlake(
	const struct drm_i915_private *i915)
{
	UNUSED_PARAMETER(i915);

	/* Reports that the only platform driven is ADL-P. */
	return 0;
}

#endif /* DRIVERS_GPU_I915_DISPLAY_VBT_H */
