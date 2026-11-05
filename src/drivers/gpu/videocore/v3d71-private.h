/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the V3D 7.1 files of the BCM2712 graphics driver share.
 *
 * The Raspberry Pi 5's render engine is a V3D 7.1.  Its memory management,
 * command lists and cache maintenance work as on the Pi 4's V3D 4.2, so the
 * 7.1 files keep the Pi 4's engine record (struct bcm2711_v3d) and offer the
 * same hardware and job functions under the same names; the board's source
 * list picks the 7.1 files instead of the 4.2 ones.  What 7.1 adds -- the
 * power sequencer (SMS), the second interrupt line, the version's revision
 * -- lives in the record below, which begins with the Pi 4's record so the
 * shared functions can find it from the engine they are given.
 *
 * Register offsets, bits and values of the 7.1 engine are data from the
 * GPL-2.0 Linux v3d driver (v6.19 05f7e89ab973, drivers/gpu/drm/v3d/
 * v3d_regs.h, v3d_gem.c, v3d_irq.c, v3d_sched.c, v3d_drv.c) and the GPL-2.0
 * BCM2835 power driver (drivers/pmdomain/bcm/bcm2835-power.c), named at
 * each block; command-list packets and QPU words are from MIT Mesa 25.3.6
 * (src/broadcom).  No code, comment or structure of either was copied.
 */

#ifndef KERN_DRIVERS_GPU_BCM2711_V3D71_PRIVATE_H
#define KERN_DRIVERS_GPU_BCM2711_V3D71_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/*
 * The core's interrupt sources the driver owns: render done (bit 0), bin
 * done (bit 1), binner out of memory (bit 2) and compute done (bit 6).
 *
 * Values referenced: Linux v3d_regs.h lines 283-290, v3d_irq.c lines 24-28.
 */
#define BCM2712_V3D_CORE_RENDER_DONE	0x01U
#define BCM2712_V3D_CORE_BIN_DONE	0x02U
#define BCM2712_V3D_CORE_OUT_OF_MEMORY	0x04U
#define BCM2712_V3D_CORE_COMPUTE_DONE	0x40U
#define BCM2712_V3D_CORE_EVENTS		0x47U

/*
 * The hub's interrupt sources the driver owns: texture formatting done
 * (bit 1), MMU capability exceeded (bit 3), page-table entry invalid
 * (bit 4), write violation (bit 5) and memory-protection violation (bit 6);
 * all but the first are faults.
 *
 * Values referenced: Linux v3d_regs.h lines 62-74, v3d_irq.c lines 30-34.
 */
#define BCM2712_V3D_HUB_TFU_DONE	0x02U
#define BCM2712_V3D_HUB_EVENTS		0x7aU
#define BCM2712_V3D_HUB_FAULTS		0x78U

/*
 * The V3D 7.1 engine of the BCM2712.
 *
 * One instance exists for the system, filled by discovery and kept for the
 * kernel's lifetime.  engine comes first, so the shared hardware and job
 * functions, which are handed &engine, find the rest of the record from it.
 * The fields after engine are written only by discovery, power-up and
 * identification on the single thread that starts the engine, and read
 * afterwards.
 */
struct bcm2712_v3d {
	struct bcm2711_v3d engine;

	/* The power sequencer's window (SMS), new in 7.1. */
	struct bcm2711_window sequencer;

	/* The hub's own interrupt line; engine.irq is the core's. */
	struct bcm2711_irq_line hub_irq;

	/* The version (71) and revision (the x of 7.1.x) the hub reports, zero before identification. */
	uint32_t version;
	uint32_t revision;
	uint32_t compat_revision;

	/* QPUs, VPM bytes, and the physical address bits the page table can name. */
	uint32_t qpus;
	uint32_t vpm_bytes;
	uint32_t table_physical_bits;

	/* The firmware clock's rate after power-up, in hertz. */
	uint32_t clock_hz;

	/* The power manager's graphics registers as discovery found them (diagnostics). */
	uint32_t pm_graphics;
	uint32_t pm_v3d;

	/* The bring-up ran (it runs once) and how far it came. */
	bool started;
	const char *stage_failed;
	int error;
};

/*
 * Converts the shared engine record to the 7.1 record that holds it.
 *
 * Only the 7.1 attachment creates engines on this board, and it creates the
 * one engine inside a struct bcm2712_v3d, so the conversion always holds.
 */
static __inline struct bcm2712_v3d *
bcm2712_v3d_of(
	struct bcm2711_v3d *engine)
{
	/* The engine record is the first member of the 7.1 record. */
	return (struct bcm2712_v3d *)(void *)engine;
}

/*
 * Whether the shader state record is the draw-index form (revision 10 and
 * later, the BCM2712 D0).  Set by identification before any job runs and
 * read by the record encoder (native71-shader.c).
 */
extern bool bcm2712_v3d_draw_index_record;

/* The Pi 5's firmware mailbox for the clock questions (v3d71-firmware.c). */
int bcm2712_firmware_init(const struct drv_fdt *fdt);
int bcm2712_firmware_property(uint32_t tag, uint32_t *values, unsigned request_count, unsigned capacity, uint32_t *answered);
int bcm2712_firmware_message(uint32_t *words, unsigned count);
void bcm2712_screen_report(const struct drv_fdt *fdt);
int bcm2712_clock_get(uint32_t tag, uint32_t clock_id, uint32_t *value);
int bcm2712_clock_set(uint32_t tag, uint32_t clock_id, uint32_t value);

/* Discovery, power, the sequencer and the power manager's reset (v3d71.c). */
int bcm2712_v3d_discover(const struct drv_fdt *fdt, struct bcm2712_v3d *v3d);
int bcm2712_v3d_power_start(struct bcm2712_v3d *v3d);
int bcm2712_v3d_power_reset(struct bcm2712_v3d *v3d);
int bcm2712_v3d_sequencer_reset(struct bcm2712_v3d *v3d);

/* The 7.1 interrupt service of the hub line (v3d71-hardware.c). */
bool bcm2712_v3d_hub_service(void *owner);

/* The 7.1 command-list images of the self-test (v3d71-cl.c). */
#define BCM2712_V3D_NOOP_BIN_BYTES	14U
#define BCM2712_V3D_NOOP_RENDER_BYTES	56U
#define BCM2712_V3D_NOOP_TILE_BYTES	19U
#define BCM2712_V3D_CLEAR_RENDER_BYTES	96U
int bcm2712_v3d_noop_prepare(struct bcm2711_v3d_noop *job);
int bcm2712_v3d_clear_prepare(struct bcm2711_v3d_clear *job);

/* The QPU words of the self-test's compute shaders (v3d71-cl.c). */
#define BCM2712_V3D_EMPTY_SHADER_WORDS	3U
#define BCM2712_V3D_STORE_SHADER_WORDS	8U
extern const uint64_t bcm2712_v3d_empty_shader[BCM2712_V3D_EMPTY_SHADER_WORDS];
extern const uint64_t bcm2712_v3d_store_shader[BCM2712_V3D_STORE_SHADER_WORDS];

#endif
