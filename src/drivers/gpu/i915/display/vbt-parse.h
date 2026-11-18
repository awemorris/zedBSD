/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VBT as the older display code reads it (vbt-shell.c).
 *
 * The VBT stage chooses the VBT bytes (the OpRegion's copy, or the PCI
 * expansion ROM), parses them with the xd parser and puts what it found in
 * struct i915_vbt and struct i915_vbt_state (internal.h).  It also keeps the
 * SHA-256 and the OpRegion reader the firmware display check uses.
 *
 * Every declaration here uses the types of internal.h only.  Every function
 * returns zedBSD positive errno values.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_VBT_PARSE_H
#define DRIVERS_GPU_I915_DISPLAY_VBT_PARSE_H

#include "internal.h"

struct i915_display;
struct i915_pci;
struct i915_trace;

/* The VBT state of a display (vbt-shell.c). */
int drv_i915_vbt_world_create(struct i915_display *display);
void drv_i915_vbt_world_destroy(struct i915_display *display);

/* The VBT stage of the display start and its byte sources. */
void drv_i915_bios_set_opregion_vbt(struct i915_display *display, const void *buf, size_t size);
int drv_i915_bios_init_ex(struct i915_display *display, struct i915_vbt_state *vbt, struct i915_pci *pci, int opregion_has_vbt, int explicit_blob, struct i915_trace *trace);
void drv_i915_bios_driver_remove(struct i915_vbt_state *vbt);

/* The SHA-256 the VBT copies are hashed and pinned with. */
void drv_i915_sha256(const void *data, size_t len, uint8_t out[32]);

/* Where the OpRegion keeps its VBT. */
int drv_i915_opregion_locate_vbt(const uint8_t *op, size_t len, uint64_t asls, struct i915_opregion_info *out);

/* The parser's result as the display reads it. */
int drv_i915_vbt_validate(const void *bytes, size_t size);
const struct i915_vbt_encoder *drv_i915_vbt_encoder_for_port(const struct i915_vbt *v, int port);
int drv_i915_vbt_init_panel(struct i915_vbt *v, int port, const uint8_t *edid128, struct i915_vbt_panel *out);

#endif
