/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display software state and the state calculation (see state.h).
 *
 * The first half is the tail of the Linux driver probe's noirq stage:
 * intel_mode_config_init(), intel_cdclk_init(), intel_color_init(),
 * intel_dbuf_init(), intel_init_quirks() and intel_fbc_init().  Four global
 * state objects are registered on one list in probe order (cdclk and dbuf
 * here, bw and pmdemand in watermark.c), so the list and the storage behind
 * the objects are kept here and shared with watermark.c.
 *
 * The second half is the error and note sink of the modeset environment's
 * Linux text, and the calculation that turns what the panel reports (EDID,
 * DPCD, the VBT colour depth) into the values the display will be
 * programmed with:
 *
 *   mode          drm_mode_detailed() on the base block's first detailed
 *                 timing (the preferred timing of an EDID 1.3+/1.4 panel);
 *   bpp           intel_dp_max_bpp()'s rule for eDP: the sink's depth,
 *                 capped by the VBT's;
 *   rate, lanes   Linux trains an eDP sink older than eDP 1.4 at its
 *                 maximum (intel_dp->use_max_params); the general search of
 *                 intel_dp_compute_link_config() is not ported, only that
 *                 rule, checked with Linux's bandwidth functions;
 *   M/N           intel_link_compute_m_n() with the SST non-FEC overhead;
 *   PLL           icl_calc_dp_combo_pll() and icl_calc_dpll_state().
 *
 * The register words of a modeset are recorded, not written: the Linux
 * writer functions run against a recorder backend that appends every write,
 * read-modify-write and unported step to a word list.
 */

#include "internal.h"
#include "modeset-internal.h"
#include "state.h"
#include "clock.h"
#include "dp.h"
#include "ddi.h"
#include "edid.h"
#include "pipe.h"
#include "plane.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The PCI_ANY_ID of a quirk entry: any subsystem id matches. */
#define I915_PCI_ANY_ID ((int)(-1))

/* The link overhead of an SST link without FEC, in parts per million (intel_dp_link_required's 1000000). */
#define I915_STATE_SST_BW_OVERHEAD 1000000

/* The eDP revision from which the sink carries rate tables (DP_EDP_14). */
#define I915_STATE_DP_EDP_14 0x03

/*
 * One entry of the Linux intel_quirks[] table.
 *
 * Every Linux hook of the table reduces to intel_set_quirk() of one id plus
 * an info line, so the entry carries the id and the line instead of a
 * function pointer.
 */
struct i915_quirk {
	/* The PCI device id the entry is for. */
	int device;

	/* The subsystem vendor and device ids, or I915_PCI_ANY_ID. */
	int subsystem_vendor;
	int subsystem_device;

	/* The quirk (enum i915_quirk_id) the hook sets. */
	int quirk;

	/* The name the hook logs. */
	const char *msg;
};

/*
 * The world whose note and error counters the modeset environment's Linux
 * text reports into.
 *
 * XXX: the Linux text reaches its error sink through drv_i915_lcd_note() and
 * drv_i915_lcd_error(), which the environment header declares without a
 * world, so the sink needs one binding here.  It is set by the error hook
 * binding and by the calculation, both of which name the world explicitly,
 * and read by the sink only.  NULL until the first binding: a note or error
 * before it is not counted.  The display owner's thread is the only one
 * that runs the Linux text, so no lock protects it.
 */
static struct i915_lcd_world *i915_state_sink_world;

/*
 * The Linux intel_quirks[] table, verbatim.
 */
static const struct i915_quirk i915_quirks[] = {
	/* Lenovo U160 cannot use SSC on LVDS */
	{ 0x0046, 0x17aa, 0x3920, I915_QUIRK_LVDS_SSC_DISABLE, "lvds SSC disable" },

	/* Sony Vaio Y cannot use SSC on LVDS */
	{ 0x0046, 0x104d, 0x9076, I915_QUIRK_LVDS_SSC_DISABLE, "lvds SSC disable" },

	/* Acer Aspire 5734Z must invert backlight brightness */
	{ 0x2a42, 0x1025, 0x0459, I915_QUIRK_INVERT_BRIGHTNESS, "inverted panel brightness" },

	/* Acer/eMachines G725 */
	{ 0x2a42, 0x1025, 0x0210, I915_QUIRK_INVERT_BRIGHTNESS, "inverted panel brightness" },

	/* Acer/eMachines e725 */
	{ 0x2a42, 0x1025, 0x0212, I915_QUIRK_INVERT_BRIGHTNESS, "inverted panel brightness" },

	/* Acer/Packard Bell NCL20 */
	{ 0x2a42, 0x1025, 0x034b, I915_QUIRK_INVERT_BRIGHTNESS, "inverted panel brightness" },

	/* Acer Aspire 4736Z */
	{ 0x2a42, 0x1025, 0x0260, I915_QUIRK_INVERT_BRIGHTNESS, "inverted panel brightness" },

	/* Acer Aspire 5336 */
	{ 0x2a42, 0x1025, 0x048a, I915_QUIRK_INVERT_BRIGHTNESS, "inverted panel brightness" },

	/* Acer C720 and C720P Chromebooks (Celeron 2955U) have backlights */
	{ 0x0a06, 0x1025, 0x0a11, I915_QUIRK_BACKLIGHT_PRESENT, "backlight present" },

	/* Acer C720 Chromebook (Core i3 4005U) */
	{ 0x0a16, 0x1025, 0x0a11, I915_QUIRK_BACKLIGHT_PRESENT, "backlight present" },

	/* Apple Macbook 2,1 (Core 2 T7400) */
	{ 0x27a2, 0x8086, 0x7270, I915_QUIRK_BACKLIGHT_PRESENT, "backlight present" },

	/* Apple Macbook 4,1 */
	{ 0x2a02, 0x106b, 0x00a1, I915_QUIRK_BACKLIGHT_PRESENT, "backlight present" },

	/* Toshiba CB35 Chromebook (Celeron 2955U) */
	{ 0x0a06, 0x1179, 0x0a88, I915_QUIRK_BACKLIGHT_PRESENT, "backlight present" },

	/* HP Chromebook 14 (Celeron 2955U) */
	{ 0x0a06, 0x103c, 0x21ed, I915_QUIRK_BACKLIGHT_PRESENT, "backlight present" },

	/* Dell Chromebook 11 */
	{ 0x0a06, 0x1028, 0x0a35, I915_QUIRK_BACKLIGHT_PRESENT, "backlight present" },

	/* Dell Chromebook 11 (2015 version) */
	{ 0x0a16, 0x1028, 0x0a35, I915_QUIRK_BACKLIGHT_PRESENT, "backlight present" },

	/* Toshiba Satellite P50-C-18C */
	{ 0x191B, 0x1179, 0xF840, I915_QUIRK_INCREASE_T12_DELAY, "T12 delay" },

	/* GeminiLake NUC */
	{ 0x3185, 0x8086, 0x2072, I915_QUIRK_INCREASE_DDI_DISABLED_TIME, "Increase DDI disabled time" },
	{ 0x3184, 0x8086, 0x2072, I915_QUIRK_INCREASE_DDI_DISABLED_TIME, "Increase DDI disabled time" },
	/* ASRock ITX*/
	{ 0x3185, 0x1849, 0x2212, I915_QUIRK_INCREASE_DDI_DISABLED_TIME, "Increase DDI disabled time" },
	{ 0x3184, 0x1849, 0x2212, I915_QUIRK_INCREASE_DDI_DISABLED_TIME, "Increase DDI disabled time" },
	/* ECS Liva Q2 */
	{ 0x3185, 0x1019, 0xa94d, I915_QUIRK_INCREASE_DDI_DISABLED_TIME, "Increase DDI disabled time" },
	{ 0x3184, 0x1019, 0xa94d, I915_QUIRK_INCREASE_DDI_DISABLED_TIME, "Increase DDI disabled time" },
	/* HP Notebook - 14-r206nv */
	{ 0x0f31, 0x103c, 0x220f, I915_QUIRK_INVERT_BRIGHTNESS, "inverted panel brightness" }
};

/*
 * The hooks of the Linux intel_dmi_quirks[]: two entries, each a DMI
 * system-id list plus a hook that sets one quirk.
 *
 * Matching a list needs a DMI backend (dmi_check_system()), which this
 * kernel does not have, so the list is walked and nothing matches.
 */
static const int i915_dmi_quirk_hooks[] = {
	I915_QUIRK_INVERT_BRIGHTNESS,            /* NCR / Thundersoft TST178 */
	I915_QUIRK_NO_PPS_BACKLIGHT_POWER_HOOK   /* Google Lillipup sku524294/5 */
};

/*
 * The duplicate and destroy hooks of the CDCLK and DBUF global objects.
 *
 * They belong to the atomic commit, which is not ported, so they are empty.
 * Each object still has a table of its own, as Linux registers a distinct
 * intel_global_state_funcs per user.
 */
static const struct i915_global_state_funcs i915_cdclk_funcs = { 0, 0 };
static const struct i915_global_state_funcs i915_dbuf_funcs = { 0, 0 };

static void i915_state_zero(void *p, unsigned n);
static void i915_intel_set_quirk(struct i915_display_state *d, int quirk, const char *msg);
static void i915_fbc_underrun_work_fn(void *ctx);
static int i915_need_fbc_vtd_wa(int legacy_platform, int vtd_active);
static struct i915_fbc *i915_intel_fbc_create(struct i915_display_state *d, int display_ver, int fbc_id);
static int i915_state_rate_from_bw_code(u8 code);
static void i915_state_record_write(void *ctx, u32 reg, u32 value);
static u32 i915_state_record_rmw(void *ctx, u32 reg, u32 clear, u32 set);
static void i915_state_record_step(void *ctx, const char *name);
static void i915_state_bind_recorder(struct i915_lcd_emit *emit, struct i915_lcd_words *out);
static void i915_state_to_mode(const struct i915_lcd_state *s, struct drm_display_mode *mode, struct intel_link_m_n *m_n);

/*
 * Initializes the mode configuration of the display (intel_mode_config_init()).
 *
 * The limits come from the display version and, for the parts older than
 * anything this driver runs on, from the legacy platform.  The global
 * object list starts empty.
 */
void
drv_i915_mode_config_init(
	struct i915_display_state *d,
	int display_ver,
	int legacy_platform)
{
	struct i915_mode_config *mc;

	/* drm_mode_config_init(): the configuration starts cleared. */
	mc = &d->mode_config;
	i915_state_zero(mc, (unsigned)sizeof(*mc));
	mc->drm_mode_config_inited = 1;

	/* INIT_LIST_HEAD(&i915->display.global.obj_list): no global object is registered yet. */
	d->obj_list_head = NULL;
	d->obj_list_tail = NULL;
	d->obj_count = 0u;
	d->obj_list_inited = 1;

	/* The smallest framebuffer has no limit. */
	mc->min_width = 0u;
	mc->min_height = 0u;

	/* The console prefers 24-bit depth and a shadow buffer. */
	mc->preferred_depth = 24u;
	mc->prefer_shadow = 1;

	/* The mode hooks (&intel_mode_funcs, &intel_mode_config_funcs) are set. */
	mc->funcs_set = 1;
	mc->helper_private_set = 1;

	/* HAS_ASYNC_FLIPS(): display version 5 and later flip asynchronously. */
	if (display_ver >= 5) {
		mc->async_page_flip = 1;
	} else {
		mc->async_page_flip = 0;
	}

	/*
	 * Maximum framebuffer dimensions, chosen to match
	 * the maximum render engine surface size on gen4+.
	 */
	if (display_ver >= 7) {
		mc->max_width = 16384u;
		mc->max_height = 16384u;
	} else if (display_ver >= 4) {
		mc->max_width = 8192u;
		mc->max_height = 8192u;
	} else if (display_ver == 3) {
		mc->max_width = 4096u;
		mc->max_height = 4096u;
	} else {
		mc->max_width = 2048u;
		mc->max_height = 2048u;
	}

	/* The cursor limits: the 845G / 865G, the other old parts, and everything later. */
	if (legacy_platform == I915_PLAT_I845G) {
		mc->cursor_width = 64u;
		mc->cursor_height = 1023u;
	} else if (legacy_platform == I915_PLAT_I865G) {
		mc->cursor_width = 512u;
		mc->cursor_height = 1023u;
	} else if (legacy_platform == I915_PLAT_I830) {
		mc->cursor_width = 64u;
		mc->cursor_height = 64u;
	} else if (legacy_platform == I915_PLAT_I85X) {
		mc->cursor_width = 64u;
		mc->cursor_height = 64u;
	} else if (legacy_platform == I915_PLAT_I915G) {
		mc->cursor_width = 64u;
		mc->cursor_height = 64u;
	} else if (legacy_platform == I915_PLAT_I915GM) {
		mc->cursor_width = 64u;
		mc->cursor_height = 64u;
	} else {
		mc->cursor_width = 256u;
		mc->cursor_height = 256u;
	}

	/* The display state is initialized: the teardown undoes it. */
	d->inited = 1;
}

/*
 * Creates the CDCLK global state object (intel_cdclk_init()).
 *
 * Returns 0, or the error of the state allocation.
 */
int
drv_i915_cdclk_init(
	struct i915_display_state *d)
{
	int error;

	/* Allocates the CDCLK state. */
	error = drv_i915_alloc_global_state(d, &d->cdclk_state, (unsigned)sizeof(d->cdclk_state));
	if (error != 0) {
		d->fail_where = "intel_cdclk_init";
		return error;
	}

	/* Registers the object on the global list and names it. */
	drv_i915_atomic_global_obj_init(d, &d->cdclk_obj, &d->cdclk_state.base, &i915_cdclk_funcs);
	d->cdclk_obj.name = "cdclk";

	/* Succeeded: the CDCLK object is registered. */
	return 0;
}

/*
 * Initializes the colour state (intel_color_init()).
 *
 * Only Geminilake (display version 10) has work here: a linear degamma LUT.
 * DRM property blobs do not exist in this driver, so that case reports
 * ENOSYS instead of a silent success.
 */
int
drv_i915_color_init(
	struct i915_display_state *d,
	int display_ver)
{
	/* Every version but 10 has nothing to create. */
	if (display_ver != 10) {
		d->color_done = 1;
		return 0;
	}

	/*
	 * GLK only: create_linear_lut(degamma_lut_size) into
	 * display.color.glk_linear_degamma_lut, which is not ported.
	 */
	d->fail_where = "intel_color_init";

	/* Refuses the unported degamma LUT. */
	return ENOSYS;
}

/*
 * Creates the DBUF global state object (intel_dbuf_init()).
 *
 * Returns 0, or the error of the state allocation.
 */
int
drv_i915_dbuf_init(
	struct i915_display_state *d)
{
	int error;

	/* Allocates the DBUF state. */
	error = drv_i915_alloc_global_state(d, &d->dbuf_state, (unsigned)sizeof(d->dbuf_state));
	if (error != 0) {
		d->fail_where = "intel_dbuf_init";
		return error;
	}

	/* Registers the object on the global list and names it. */
	drv_i915_atomic_global_obj_init(d, &d->dbuf_obj, &d->dbuf_state.base, &i915_dbuf_funcs);
	d->dbuf_obj.name = "dbuf";

	/* Succeeded: the DBUF object is registered. */
	return 0;
}

/*
 * Applies the quirks of the device (intel_init_quirks()).
 *
 * The PCI table is matched on the device and subsystem ids; the DMI table is
 * walked but cannot match without a DMI backend.
 */
void
drv_i915_init_quirks(
	struct i915_display_state *d,
	uint16_t device,
	uint16_t subsystem_vendor,
	uint16_t subsystem_device)
{
	const struct i915_quirk *q;
	unsigned i;
	int matched;

	/* Applies every PCI quirk whose device and subsystem ids match. */
	for (i = 0u; i < (unsigned)(sizeof(i915_quirks) / sizeof(i915_quirks[0])); i++) {
		q = &i915_quirks[i];

		/* Skips an entry of another device. */
		if ((int)device != q->device)
			continue;

		/* Skips an entry of another subsystem vendor. */
		if ((int)subsystem_vendor != q->subsystem_vendor &&
		    q->subsystem_vendor != I915_PCI_ANY_ID)
			continue;

		/* Skips an entry of another subsystem device. */
		if ((int)subsystem_device != q->subsystem_device &&
		    q->subsystem_device != I915_PCI_ANY_ID)
			continue;

		/* The entry matches: its hook sets the quirk. */
		i915_intel_set_quirk(d, q->quirk, q->msg);
	}

	/* Records that the DMI list was walked and that no DMI backend answers. */
	d->dmi_scanned = 1;
	d->dmi_available = 0;

	/* Walks the DMI quirks: dmi_check_system() cannot match without a backend. */
	for (i = 0u; i < (unsigned)(sizeof(i915_dmi_quirk_hooks) / sizeof(i915_dmi_quirk_hooks[0])); i++) {
		/* XXX: no DMI backend -- dmi_check_system(*intel_dmi_quirks[i].dmi_id_list) is 0 here. */
		matched = 0;
		if (matched != 0)
			i915_intel_set_quirk(d, i915_dmi_quirk_hooks[i], "DMI");
	}
}

/*
 * Decides whether FBC is enabled (intel_sanitize_fbc_option()).
 *
 * An explicit module parameter wins; otherwise FBC is on for Broadwell and
 * display version 9 and later, when the display has an FBC unit at all.
 */
int
drv_i915_sanitize_fbc_option(
	int display_ver,
	int legacy_platform,
	unsigned fbc_mask,
	int enable_fbc_param)
{
	/* An explicit parameter decides. */
	if (enable_fbc_param >= 0) {
		if (enable_fbc_param != 0)
			return 1;

		/* The parameter turns FBC off. */
		return 0;
	}

	/* HAS_FBC(): DISPLAY_RUNTIME_INFO(i915)->fbc_mask != 0. */
	if (fbc_mask == 0u)
		return 0;

	/* Broadwell enables FBC by default. */
	if (legacy_platform == I915_PLAT_BROADWELL)
		return 1;

	/* Display version 9 and later enable FBC by default. */
	if (display_ver >= 9)
		return 1;

	/* Succeeded: FBC stays off on the older parts. */
	return 0;
}

/*
 * Creates the FBC instances of the display (intel_fbc_init()).
 *
 * The VT-d workaround may take every instance away first; the sanitized
 * enable_fbc value is logged, and one instance is created per bit of the
 * FBC mask.
 */
void
drv_i915_fbc_init(
	struct i915_display_state *d,
	int display_ver,
	unsigned fbc_mask,
	int vtd_active,
	int legacy_platform)
{
	unsigned fbc_id;
	int wa_needed;

	/* Starts from the FBC units the display reports. */
	d->fbc_mask = fbc_mask;

	/* WaFbcTurnOffFbcWhenHyperVisorIsUsed: DISPLAY_RUNTIME_INFO(i915)->fbc_mask = 0. */
	wa_needed = i915_need_fbc_vtd_wa(legacy_platform, vtd_active);
	if (wa_needed != 0) {
		d->fbc_mask = 0u;
		d->fbc_vtd_wa = 1;
	}

	/* Decides and logs the enable_fbc value. */
	d->enable_fbc_sanitized = drv_i915_sanitize_fbc_option(display_ver, legacy_platform, d->fbc_mask, d->enable_fbc_param);
	kern_logf("i915: Sanitized enable_fbc value: %d\n", d->enable_fbc_sanitized);

	/* for_each_fbc_id(): creates one instance for every bit set in the mask. */
	for (fbc_id = 0u; fbc_id < (unsigned)I915_MAX_FBCS; fbc_id++) {
		if ((d->fbc_mask & (1u << fbc_id)) == 0u)
			continue;

		d->fbc[fbc_id] = i915_intel_fbc_create(d, display_ver, (int)fbc_id);
		if (d->fbc[fbc_id] != NULL)
			d->fbc_created++;
	}
}

/*
 * Tears down the display software state.
 *
 * intel_atomic_global_obj_cleanup(): every global object drops its state's
 * reference and leaves the list.  The storage belongs to the device, so
 * only the bookkeeping is undone; the FBC instances are released the same
 * way.
 */
void
drv_i915_display_state_fini(
	struct i915_display_state *d)
{
	struct i915_global_obj *obj;
	struct i915_global_obj *next;
	unsigned i;

	/* Drops each object's state reference and unlinks the object. */
	obj = d->obj_list_head;
	while (obj != NULL) {
		next = obj->next;

		/* The reference the object held on its state goes away with the link. */
		if (obj->state != NULL) {
			if (obj->state->ref > 0u)
				obj->state->ref--;

			obj->state = NULL;
		}

		obj->next = NULL;
		obj = next;
	}

	/* The list is empty. */
	d->obj_list_head = NULL;
	d->obj_list_tail = NULL;
	d->obj_count = 0u;

	/* Releases every FBC instance. */
	for (i = 0u; i < (unsigned)I915_MAX_FBCS; i++) {
		if (d->fbc[i] != NULL) {
			d->fbc[i]->in_use = 0;
			d->fbc[i] = NULL;
		}
	}

	/* Nothing is created or initialized any more. */
	d->fbc_created = 0u;
	d->inited = 0;
}

/*
 * Allocates the storage of one global state (the kzalloc() of Linux).
 *
 * The storage belongs to the device, so the allocation only clears it and
 * counts it; the allocation cannot fail.  Returns 0.
 */
int
drv_i915_alloc_global_state(
	struct i915_display_state *d,
	void *storage,
	unsigned size)
{
	unsigned n;

	/* The allocation this call makes. */
	n = d->state_allocs + 1u;

	/* kzalloc(): the state starts cleared. */
	i915_state_zero(storage, size);
	d->state_allocs = n;

	/* Succeeded: the storage is cleared and counted. */
	return 0;
}

/*
 * Registers one global state object (intel_atomic_global_obj_init()).
 *
 * The object takes its state with one reference and is appended at the
 * tail of the global list, so the list keeps the registration order.
 */
void
drv_i915_atomic_global_obj_init(
	struct i915_display_state *d,
	struct i915_global_obj *obj,
	struct i915_global_state *state,
	const struct i915_global_state_funcs *funcs)
{
	/* The object starts cleared. */
	i915_state_zero(obj, (unsigned)sizeof(*obj));

	/* kref_init(&state->ref): the object holds the state's only reference. */
	state->obj = obj;
	state->ref = 1u;

	/* The object knows its state and hooks. */
	obj->state = state;
	obj->funcs = funcs;

	/* list_add_tail(&obj->head, &dev_priv->display.global.obj_list) */
	obj->next = NULL;
	if (d->obj_list_tail != NULL) {
		d->obj_list_tail->next = obj;
	} else {
		d->obj_list_head = obj;
	}

	d->obj_list_tail = obj;
	d->obj_count++;
}

/*
 * Counts a note of the modeset environment's Linux text (drm_dbg_kms,
 * MISSING_CASE).
 *
 * The note is printed through the world's note sink while its trace is on.
 */
void
drv_i915_lcd_note(
	const char *fmt)
{
	struct i915_lcd_world *world;

	/* XXX: a note before the first binding has no world to be counted in. */
	world = i915_state_sink_world;
	if (world == NULL)
		return;

	/* The count the calculation reports as its notes. */
	world->lcd_notes++;

	/* Prints the note while the trace is on and a sink is bound. */
	if (world->i915_lcd_note_trace != 0) {
		if (world->i915_lcd_note_sink != NULL)
			world->i915_lcd_note_sink(fmt);
	}
}

/*
 * Counts an error of the modeset environment's Linux text (drm_err, WARN)
 * and hands it to the bound error hook.
 *
 * The first error a run hands on is what a failed run is read from.
 */
void
drv_i915_lcd_error(
	const char *what)
{
	struct i915_lcd_world *world;

	/* XXX: an error before the first binding has no world to be counted in. */
	world = i915_state_sink_world;
	if (world == NULL)
		return;

	/* The count of errors since the hook was bound. */
	world->lcd_errors++;

	/* Hands the error to the hook, if one is bound. */
	if (world->lcd_error_hook != NULL)
		world->lcd_error_hook(world->lcd_error_ctx, what);
}

/*
 * Binds the error hook of the modeset environment's Linux text.
 *
 * The named world becomes the one the notes and errors are counted in; its
 * error count starts again at 0.
 */
void
drv_i915_lcd_error_bind(
	struct i915_lcd_world *world,
	void (*hook)(void *ctx, const char *what),
	void *ctx)
{
	/* The Linux text reports into this world from now on. */
	i915_state_sink_world = world;
	if (world == NULL)
		return;

	/* Binds the hook and its context, and restarts the count. */
	world->lcd_error_hook = hook;
	world->lcd_error_ctx = ctx;
	world->lcd_errors = 0u;
}

/*
 * Reports how many errors the Linux text reported since the hook was bound.
 */
unsigned
drv_i915_lcd_errors(
	const struct i915_lcd_world *world)
{
	/* A world that does not exist has counted nothing. */
	if (world == NULL)
		return 0u;

	/* Succeeded: reports the count. */
	return world->lcd_errors;
}

/*
 * Computes the mode, link, M/N and PLL values of the eDP panel.
 *
 * From the EDID base block, the DPCD receiver capabilities and the eDP
 * DPCD, and the VBT colour depth (0: none).  Nothing is written to the
 * hardware.  Returns 0, EINVAL for missing input, a sink the rule does not
 * cover, ENOSPC when the link cannot carry the mode, or the error of the
 * mode parser or the PLL calculation.
 */
int
drv_i915_lcd_compute(
	struct i915_lcd_world *world,
	const uint8_t *edid128,
	const uint8_t *dpcd,
	const uint8_t *edp_dpcd,
	int vbt_bpp,
	int ref_nssc_khz,
	struct i915_lcd_state *out)
{
	struct drm_display_mode mode;
	struct intel_link_m_n m_n;
	const struct edid *edid;
	unsigned index;
	unsigned depth;
	int bpc;
	int error;

	/* Refuses a call without its world or its input. */
	if (world == NULL)
		return EINVAL;
	if (edid128 == NULL)
		return EINVAL;
	if (dpcd == NULL)
		return EINVAL;
	if (edp_dpcd == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;

	/* Starts a clean result; the notes of this calculation are counted from 0 in its world. */
	edid = (const struct edid *)edid128;
	index = 0;
	kern_memset(out, 0, sizeof(*out));
	i915_state_sink_world = world;
	world->lcd_notes = 0;

	/* The mode: the preferred detailed timing of the base block. */
	error = drv_i915_edid_preferred_mode(world, edid128, &mode, &index);
	if (error != 0)
		return error;

	/* Copies the mode's timings. */
	out->mode.clock_khz = mode.clock;
	out->mode.hdisplay = mode.hdisplay;
	out->mode.hsync_start = mode.hsync_start;
	out->mode.hsync_end = mode.hsync_end;
	out->mode.htotal = mode.htotal;
	out->mode.vdisplay = mode.vdisplay;
	out->mode.vsync_start = mode.vsync_start;
	out->mode.vsync_end = mode.vsync_end;
	out->mode.vtotal = mode.vtotal;

	/* Copies the sync polarities. */
	out->mode.hsync_positive = 0;
	if ((mode.flags & DRM_MODE_FLAG_PHSYNC) != 0)
		out->mode.hsync_positive = 1;

	out->mode.vsync_positive = 0;
	if ((mode.flags & DRM_MODE_FLAG_PVSYNC) != 0)
		out->mode.vsync_positive = 1;

	/* Copies the physical size and which descriptor the mode came from. */
	out->mode.width_mm = mode.width_mm;
	out->mode.height_mm = mode.height_mm;
	out->mode.descriptor_index = index;

	/*
	 * EDID 1.4 digital input: the colour depth is in bits 6:4 of the video
	 * input byte (1 = 6 bpc ... 6 = 16 bpc).
	 */
	bpc = 0;
	if (edid->version == 1 &&
	    edid->revision >= 4 &&
	    (edid->input & EDID_INPUT_DIGITAL) != 0) {
		depth = (edid->input >> 4) & 7u;
		if (depth >= 1u && depth <= 6u)
			bpc = 4 + 2 * (int)depth;
	}

	out->mode.edid_bpc = bpc;

	/* The bpp: the sink's depth (24 when undefined), capped by the VBT's eDP colour depth. */
	if (bpc != 0) {
		out->link.bpp = 3 * bpc;
	} else {
		out->link.bpp = 24;
	}

	/* A smaller VBT colour depth caps it. */
	if (vbt_bpp != 0 && vbt_bpp < out->link.bpp)
		out->link.bpp = vbt_bpp;

	/* The sink's capability: its maximum rate and lane count. */
	out->link.sink_max_rate_khz = i915_state_rate_from_bw_code(dpcd[1]);
	out->link.sink_max_lanes = dpcd[2] & 0x1f;

	/* Refuses a rate code the 8b/10b table does not know. */
	if (out->link.sink_max_rate_khz == 0)
		return EINVAL;

	/* Refuses a lane count other than 1, 2 or 4. */
	if (out->link.sink_max_lanes != 1 &&
	    out->link.sink_max_lanes != 2 &&
	    out->link.sink_max_lanes != 4)
		return EINVAL;

	/* The use_max_params rule applies to a sink older than eDP 1.4. */
	out->link.use_max_params = 0;
	if (edp_dpcd[0] < I915_STATE_DP_EDP_14)
		out->link.use_max_params = 1;

	/* Refuses an eDP 1.4+ sink: its rate tables are not ported. */
	if (out->link.use_max_params == 0)
		return EINVAL;

	/* The sink's maximum (the ADL-P combo PHY source limit, 810000, is higher). */
	out->link.rate_khz = out->link.sink_max_rate_khz;
	out->link.lanes = out->link.sink_max_lanes;

	/* The bandwidth the mode needs, and what the link carries. */
	out->link.required_kbps = drv_i915_dp_link_required(mode.clock, out->link.bpp);
	out->link.available_kbps = drv_i915_dp_max_data_rate(out->link.rate_khz, out->link.lanes);

	/*
	 * Refuses a mode the link cannot carry.
	 *
	 * XXX: the old code returned the literal -28 (the Linux -ENOSPC); the
	 * positive zedBSD ENOSPC is returned here.
	 */
	if (out->link.required_kbps > out->link.available_kbps)
		return ENOSPC;

	/* The M/N values of the link. */
	drv_i915_link_compute_m_n((u16)(out->link.bpp * 16), out->link.lanes, mode.clock, out->link.rate_khz, I915_STATE_SST_BW_OVERHEAD, &m_n);
	out->link.tu = m_n.tu;
	out->link.data_m = m_n.data_m;
	out->link.data_n = m_n.data_n;
	out->link.link_m = m_n.link_m;
	out->link.link_n = m_n.link_n;

	/* The combo PLL of the link rate; the notes are reported whatever the PLL answers. */
	out->pll.ref_khz = ref_nssc_khz;
	error = drv_i915_icl_dp_combo_pll(out->link.rate_khz, ref_nssc_khz, &out->pll.cfgcr0, &out->pll.cfgcr1, &out->pll.div0);
	out->notes = world->lcd_notes;

	/* Reports a PLL the calculation could not find. */
	if (error != 0)
		return error;

	/* Succeeded: the mode, link, M/N and PLL are computed. */
	return 0;
}

/*
 * Computes the link and PLL values of an HDMI mode.
 *
 * At 8 bpc the TMDS clock is the pixel clock (intel_hdmi_tmds_clock()).
 * Returns 0, EINVAL for a missing or clockless mode, or the error of the
 * WRPLL calculation.
 */
int
drv_i915_lcd_compute_hdmi(
	const struct i915_lcd_mode *mode,
	int ref_nssc_khz,
	struct i915_lcd_state *out)
{
	int error;

	/* Refuses a call without a mode with a clock, or without a result. */
	if (mode == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (mode->clock_khz <= 0)
		return EINVAL;

	/* The mode, and the 4-lane, 24 bpp link its TMDS clock runs at. */
	kern_memset(out, 0, sizeof(*out));
	out->mode = *mode;
	out->link.rate_khz = mode->clock_khz;
	out->link.lanes = 4;
	out->link.bpp = 24;

	/* The WRPLL of the TMDS clock. */
	error = drv_i915_icl_hdmi_wrpll(out->link.rate_khz, ref_nssc_khz, &out->pll.cfgcr0, &out->pll.cfgcr1, &out->pll.div0);
	if (error != 0)
		return error;

	/* Succeeded: the link and PLL are computed. */
	return 0;
}

/*
 * Records the register words of the plane update for one framebuffer.
 *
 * Returns 0, EINVAL without a word list or when the list overflowed, or the
 * error of the plane writer.
 */
int
drv_i915_lcd_emit_plane(
	struct i915_lcd_world *world,
	int pipe,
	int plane_id,
	uint32_t fourcc,
	uint64_t modifier,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t surf_ggtt_offset,
	struct i915_lcd_words *out)
{
	struct i915_lcd_emit emit;
	int error;

	/* Refuses a call without a word list. */
	if (out == NULL)
		return EINVAL;

	/* Starts an empty list and a recorder backend that appends to it. */
	kern_memset(out, 0, sizeof(*out));
	kern_memset(&emit, 0, sizeof(emit));
	i915_state_bind_recorder(&emit, out);

	/* Runs the plane writer against the recorder. */
	error = drv_i915_plane_emit(world, &emit, pipe, plane_id, fourcc, modifier, width, height, pitch, surf_ggtt_offset);
	if (error != 0)
		return error;

	/* Refuses a list that did not hold every word. */
	if (out->overflow != 0u)
		return EINVAL;

	/* Succeeded: the plane words are recorded. */
	return 0;
}

/*
 * Finds the next unported step of a name in a word list.
 *
 * Returns the index of the first step named `name` at or after `from`, or
 * -1 when there is none.
 */
int
drv_i915_lcd_words_step(
	const struct i915_lcd_words *w,
	const char *name,
	unsigned from)
{
	unsigned i;
	int compared;

	/* A missing list or name has no step. */
	if (w == NULL)
		return -1;
	if (name == NULL)
		return -1;

	/* Looks at every entry from `from` on for a step of that name. */
	for (i = from; i < w->n; i++) {
		if (w->w[i].step == NULL)
			continue;

		/* A step of the name ends the search. */
		compared = kern_strcmp(w->w[i].step, name);
		if (compared == 0)
			return (int)i;
	}

	/* Succeeded: no step of that name follows `from`. */
	return -1;
}

/*
 * Counts the plain writes of a register in a word list.
 *
 * Read-modify-writes and steps do not count.  When `value` is given it
 * receives the value of the last such write.
 */
unsigned
drv_i915_lcd_words_find(
	const struct i915_lcd_words *w,
	uint32_t reg,
	uint32_t *value)
{
	unsigned i;
	unsigned hits;

	/* A missing list has no writes. */
	hits = 0u;
	if (w == NULL)
		return hits;

	/* Counts the plain writes of the register, keeping the last value. */
	for (i = 0u; i < w->n; i++) {
		if (w->w[i].reg != reg)
			continue;
		if (w->w[i].rmw != 0u)
			continue;
		if (w->w[i].step != NULL)
			continue;

		hits++;
		if (value != NULL)
			*value = w->w[i].value;
	}

	/* Succeeded: reports how many writes were found. */
	return hits;
}

/*
 * Records the operations of hsw_configure_cpu_transcoder() for a computed
 * state.
 *
 * Returns 0, EINVAL for a missing argument, a pipe or transcoder outside
 * A..D, or an overflowed list, or the error of the writer.
 */
int
drv_i915_lcd_emit_cpu_transcoder(
	struct i915_lcd_world *world,
	const struct i915_lcd_state *s,
	int pipe,
	int cpu_transcoder,
	struct i915_lcd_words *out)
{
	struct i915_lcd_emit emit;
	struct drm_display_mode mode;
	struct intel_link_m_n m_n;
	int error;

	/* Refuses a missing state or list, and a pipe or transcoder outside A..D. */
	if (s == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (pipe < 0 || pipe > 3)
		return EINVAL;
	if (cpu_transcoder < 0 || cpu_transcoder > 3)
		return EINVAL;

	/* Starts an empty list, and the mode and M/N of the state. */
	kern_memset(out, 0, sizeof(*out));
	kern_memset(&emit, 0, sizeof(emit));
	i915_state_to_mode(s, &mode, &m_n);
	i915_state_bind_recorder(&emit, out);

	/* Runs the CPU transcoder writer against the recorder. */
	error = drv_i915_display_emit_cpu_transcoder(world, &emit, &mode, &m_n, pipe, cpu_transcoder, s->link.rate_khz, s->link.lanes, s->link.bpp);
	if (error != 0)
		return error;

	/* Refuses a list that did not hold every word. */
	if (out->overflow != 0u)
		return EINVAL;

	/* Succeeded: the CPU transcoder operations are recorded. */
	return 0;
}

/*
 * Records the DDI words (TRANS_MSA_MISC, TRANS_DDI_FUNC_CTL2,
 * TRANS_DDI_FUNC_CTL) of a computed state and reports the DDI_BUF_CTL value.
 *
 * Combo PHY ports only (A, B): a Type-C port needs the TC state the
 * calculation does not have.  Returns 0, EINVAL for a missing argument, a
 * port, pipe or transcoder out of range, or an overflowed list, or the error
 * of the writer.  The DDI_BUF_CTL value is reported in every case the
 * writer ran.
 */
int
drv_i915_lcd_emit_ddi(
	struct i915_lcd_world *world,
	const struct i915_lcd_state *s,
	int port,
	int pipe,
	int cpu_transcoder,
	uint32_t saved_port_bits,
	struct i915_lcd_words *out,
	uint32_t *ddi_buf_ctl_value)
{
	struct i915_lcd_emit emit;
	struct drm_display_mode mode;
	struct intel_link_m_n m_n;
	u32 buf;
	int error;

	/* Refuses a missing argument, and a port, pipe or transcoder out of range. */
	if (s == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (ddi_buf_ctl_value == NULL)
		return EINVAL;
	if (port < 0 || port > 1)
		return EINVAL;
	if (pipe < 0 || pipe > 3)
		return EINVAL;
	if (cpu_transcoder < 0 || cpu_transcoder > 3)
		return EINVAL;

	/* Starts an empty list, and the mode and M/N of the state. */
	buf = 0u;
	kern_memset(out, 0, sizeof(*out));
	kern_memset(&emit, 0, sizeof(emit));
	i915_state_to_mode(s, &mode, &m_n);
	i915_state_bind_recorder(&emit, out);

	/* Runs the DDI writer against the recorder. */
	error = drv_i915_ddi_emit(world, &emit, &mode, port, pipe, cpu_transcoder, s->link.rate_khz, s->link.lanes, s->link.bpp, saved_port_bits, &buf);

	/* A list that did not hold every word fails a writer that succeeded. */
	if (error == 0) {
		if (out->overflow != 0u)
			error = EINVAL;
	}

	/* Hands back the DDI_BUF_CTL value whatever the outcome. */
	*ddi_buf_ctl_value = buf;

	/* Reports a failed writer or an overflowed list. */
	if (error != 0)
		return error;

	/* Succeeded: the DDI words are recorded. */
	return 0;
}

/*
 * Records the transcoder M/N, timing and pipe-source words of a computed
 * state for a source of the given size.
 *
 * Returns 0, EINVAL for a missing argument, a pipe or transcoder outside
 * A..D, an empty source, or an overflowed list, or the error of the writer.
 */
int
drv_i915_lcd_emit_transcoder(
	struct i915_lcd_world *world,
	const struct i915_lcd_state *s,
	int pipe,
	int cpu_transcoder,
	uint32_t src_width,
	uint32_t src_height,
	struct i915_lcd_words *out)
{
	struct i915_lcd_emit emit;
	struct drm_display_mode mode;
	struct intel_link_m_n m_n;
	int error;

	/* Refuses a missing argument, a pipe or transcoder outside A..D, and an empty source. */
	if (s == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (pipe < 0 || pipe > 3)
		return EINVAL;
	if (cpu_transcoder < 0 || cpu_transcoder > 3)
		return EINVAL;
	if (src_width == 0u)
		return EINVAL;
	if (src_height == 0u)
		return EINVAL;

	/*
	 * The mode's timings without sync flags: the transcoder words do not
	 * carry the polarities.
	 */
	kern_memset(out, 0, sizeof(*out));
	kern_memset(&mode, 0, sizeof(mode));
	mode.clock = s->mode.clock_khz;
	mode.hdisplay = s->mode.hdisplay;
	mode.hsync_start = s->mode.hsync_start;
	mode.hsync_end = s->mode.hsync_end;
	mode.htotal = s->mode.htotal;
	mode.vdisplay = s->mode.vdisplay;
	mode.vsync_start = s->mode.vsync_start;
	mode.vsync_end = s->mode.vsync_end;
	mode.vtotal = s->mode.vtotal;

	/* The M/N values of the state. */
	m_n.tu = s->link.tu;
	m_n.data_m = s->link.data_m;
	m_n.data_n = s->link.data_n;
	m_n.link_m = s->link.link_m;
	m_n.link_n = s->link.link_n;

	/* A recorder backend that appends to the list. */
	kern_memset(&emit, 0, sizeof(emit));
	i915_state_bind_recorder(&emit, out);

	/* Runs the transcoder writer against the recorder. */
	error = drv_i915_display_emit_transcoder(world, &emit, &mode, &m_n, pipe, cpu_transcoder, (int)src_width, (int)src_height);
	if (error != 0)
		return error;

	/* Refuses a list that did not hold every word. */
	if (out->overflow != 0u)
		return EINVAL;

	/* Succeeded: the transcoder words are recorded. */
	return 0;
}

/*
 * Reports the mode of the resident eDP panel: its size and refresh rate.
 *
 * The refresh rate is clock_khz * 1000 pixels a second over htotal * vtotal
 * pixels a frame, in millihertz, rounded to nearest.  Returns 0, or ENODEV
 * when there is no panel or its mode is empty.
 */
int
drv_i915_display_panel_mode(
	const struct i915_lcd_kernel_deps *d,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_millihz)
{
	const struct i915_lcd_mode *m;
	uint64_t pixels;

	/*
	 * Refuses a missing panel.
	 *
	 * XXX: the old function returned -1 for every refusal; ENODEV is the
	 * positive error here.
	 */
	if (d == NULL)
		return ENODEV;
	if (d->edp == NULL)
		return ENODEV;

	/* The pixels of one frame. */
	m = &d->edp->lcd.mode;
	pixels = (uint64_t)m->htotal * m->vtotal;

	/* Refuses an empty mode. */
	if (m->hdisplay == 0u)
		return ENODEV;
	if (m->vdisplay == 0u)
		return ENODEV;
	if (pixels == 0u)
		return ENODEV;
	if (m->clock_khz <= 0)
		return ENODEV;

	/* The visible size, and the refresh rate in millihertz. */
	*width = m->hdisplay;
	*height = m->vdisplay;
	*refresh_millihz = (uint32_t)(((uint64_t)m->clock_khz * 1000000ull + pixels / 2u) / pixels);

	/* Succeeded: the panel mode is reported. */
	return 0;
}

/*
 * Reports the physical size of the resident eDP panel in millimetres.
 *
 * Returns 0, or ENODEV when there is no panel or the EDID gave no size.
 */
int
drv_i915_display_panel_size_mm(
	const struct i915_lcd_kernel_deps *d,
	uint32_t *width_mm,
	uint32_t *height_mm)
{
	/* Refuses a missing panel or size (XXX: -1 in the old function, as above). */
	if (d == NULL)
		return ENODEV;
	if (d->edp == NULL)
		return ENODEV;
	if (d->edp->lcd.mode.width_mm == 0u)
		return ENODEV;

	/* The size the EDID reported. */
	*width_mm = d->edp->lcd.mode.width_mm;
	*height_mm = d->edp->lcd.mode.height_mm;

	/* Succeeded: the panel size is reported. */
	return 0;
}

/* Clears n bytes. */
static void
i915_state_zero(
	void *p,
	unsigned n)
{
	char *c;
	unsigned i;

	/* Clears byte by byte. */
	c = (char *)p;
	for (i = 0u; i < n; i++)
		c[i] = 0;
}

/* Sets one quirk and logs it (intel_set_quirk() plus the hook's info line). */
static void
i915_intel_set_quirk(
	struct i915_display_state *d,
	int quirk,
	const char *msg)
{
	/* display.quirks.mask carries one bit per quirk id. */
	d->quirk_mask |= (1u << (unsigned)quirk);
	d->quirk_hooks_fired++;

	/* The hook's info line. */
	kern_logf("i915: applying %s quirk\n", msg);
}

/* The FBC underrun work (intel_fbc_underrun_work_fn()): not ported, reports that it ran. */
static void
i915_fbc_underrun_work_fn(
	void *ctx)
{
	UNUSED_PARAMETER(ctx);

	/*
	 * XXX: UNPORTED -- intel_fbc_underrun_work_fn() disables FBC after a
	 * FIFO underrun.  It is only queued from the display interrupt path,
	 * which does not queue it; if it ever runs, say so rather than appear
	 * to have worked.
	 */
	kern_logf("i915: intel_fbc_underrun_work_fn: UNIMPLEMENTED (queued unexpectedly)\n");
}

/* Tells whether FBC must be off under VT-d (need_fbc_vtd_wa(), WaFbcTurnOffFbcWhenHyperVisorIsUsed:skl,bxt). */
static int
i915_need_fbc_vtd_wa(
	int legacy_platform,
	int vtd_active)
{
	/* Only an active VT-d needs the workaround. */
	if (vtd_active == 0)
		return 0;

	/* Only Skylake and Broxton need it. */
	if (legacy_platform != I915_PLAT_SKYLAKE && legacy_platform != I915_PLAT_BROXTON)
		return 0;

	/* The workaround applies: FBC goes off. */
	kern_logf("i915: Disabling framebuffer compression (FBC) to prevent screen flicker with VT-d enabled\n");

	/* Succeeded: the workaround is needed. */
	return 1;
}

/* Creates one FBC instance in the device's storage (intel_fbc_create()); NULL for an id out of range. */
static struct i915_fbc *
i915_intel_fbc_create(
	struct i915_display_state *d,
	int display_ver,
	int fbc_id)
{
	struct i915_fbc *fbc;

	/* Refuses an id outside the storage. */
	if (fbc_id < 0 || fbc_id >= I915_MAX_FBCS)
		return NULL;

	/* kzalloc(): the instance starts cleared. */
	fbc = &d->fbc_store[fbc_id];
	i915_state_zero(fbc, (unsigned)sizeof(*fbc));

	/* The instance's id, its underrun work and its lock. */
	fbc->id = fbc_id;
	drv_i915_work_init(&fbc->underrun_work, i915_fbc_underrun_work_fn, fbc);
	(void)mutex_init(&fbc->lock, LOCK_RANK_DEVICE, "i915-fbc");
	fbc->lock_inited = 1;

	/* The hooks of the display version. */
	if (display_ver >= 7) {
		fbc->funcs_kind = I915_FBC_FUNCS_IVB;
	} else if (display_ver == 6) {
		fbc->funcs_kind = I915_FBC_FUNCS_SNB;
	} else if (display_ver == 5) {
		fbc->funcs_kind = I915_FBC_FUNCS_ILK;
	} else if (display_ver == 4) {
		fbc->funcs_kind = I915_FBC_FUNCS_I965;
	} else {
		fbc->funcs_kind = I915_FBC_FUNCS_I8XX;
	}

	/* The instance is in use until the teardown. */
	fbc->in_use = 1;

	/* Succeeded: reports the instance. */
	return fbc;
}

/* The link rate of an 8b/10b bandwidth code (drm_dp_bw_code_to_link_rate()), in kHz; 0 for another code. */
static int
i915_state_rate_from_bw_code(
	u8 code)
{
	/* The code times 0.27 Gbps, in 10 kbit/s units. */
	switch (code) {
	case 0x06:
		return 162000;
	case 0x0a:
		return 270000;
	case 0x14:
		return 540000;
	case 0x1e:
		return 810000;
	default:
		return 0;
	}
}

/* Appends a register write to the recorder's word list. */
static void
i915_state_record_write(
	void *ctx,
	u32 reg,
	u32 value)
{
	struct i915_lcd_words *out;

	/* Counts a write the full list cannot hold. */
	out = ctx;
	if (out->n >= I915_LCD_MAX_REGWRITES) {
		out->overflow++;
		return;
	}

	/* Records the write. */
	out->w[out->n].reg = reg;
	out->w[out->n].value = value;
	out->w[out->n].clear = 0u;
	out->w[out->n].rmw = 0u;
	out->w[out->n].step = NULL;
	out->n++;
}

/* Appends a read-modify-write to the recorder's word list; the old value reads as 0. */
static u32
i915_state_record_rmw(
	void *ctx,
	u32 reg,
	u32 clear,
	u32 set)
{
	struct i915_lcd_words *out;

	/* Counts an operation the full list cannot hold. */
	out = ctx;
	if (out->n >= I915_LCD_MAX_REGWRITES) {
		out->overflow++;
		return 0u;
	}

	/* Records the bits set and cleared. */
	out->w[out->n].reg = reg;
	out->w[out->n].value = set;
	out->w[out->n].clear = clear;
	out->w[out->n].rmw = 1u;
	out->w[out->n].step = NULL;
	out->n++;

	/* Succeeded: the recorder has no register contents; the old value is 0. */
	return 0u;
}

/* Appends an unported step to the recorder's word list. */
static void
i915_state_record_step(
	void *ctx,
	const char *name)
{
	struct i915_lcd_words *out;

	/* Counts a step the full list cannot hold. */
	out = ctx;
	if (out->n >= I915_LCD_MAX_REGWRITES) {
		out->overflow++;
		return;
	}

	/* Records the step at its position. */
	kern_memset(&out->w[out->n], 0, sizeof(out->w[out->n]));
	out->w[out->n].step = name;
	out->n++;
}

/* Makes a cleared backend a recorder that appends to the word list. */
static void
i915_state_bind_recorder(
	struct i915_lcd_emit *emit,
	struct i915_lcd_words *out)
{
	/* The list is the context of the three recording hooks. */
	emit->ctx = out;
	emit->write32 = i915_state_record_write;
	emit->rmw32 = i915_state_record_rmw;
	emit->step = i915_state_record_step;
}

/* Rebuilds the Linux mode and M/N values from a computed state. */
static void
i915_state_to_mode(
	const struct i915_lcd_state *s,
	struct drm_display_mode *mode,
	struct intel_link_m_n *m_n)
{
	/* The mode's timings. */
	kern_memset(mode, 0, sizeof(*mode));
	mode->clock = s->mode.clock_khz;
	mode->hdisplay = s->mode.hdisplay;
	mode->hsync_start = s->mode.hsync_start;
	mode->hsync_end = s->mode.hsync_end;
	mode->htotal = s->mode.htotal;
	mode->vdisplay = s->mode.vdisplay;
	mode->vsync_start = s->mode.vsync_start;
	mode->vsync_end = s->mode.vsync_end;
	mode->vtotal = s->mode.vtotal;

	/* The sync polarities as mode flags. */
	if (s->mode.hsync_positive != 0) {
		mode->flags = DRM_MODE_FLAG_PHSYNC;
	} else {
		mode->flags = DRM_MODE_FLAG_NHSYNC;
	}

	/* The vertical polarity. */
	if (s->mode.vsync_positive != 0) {
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	} else {
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	}

	/* The M/N values. */
	kern_memset(m_n, 0, sizeof(*m_n));
	m_n->tu = s->link.tu;
	m_n->data_m = s->link.data_m;
	m_n->data_n = s->link.data_n;
	m_n->link_m = s->link.link_m;
	m_n->link_n = s->link.link_n;
}
