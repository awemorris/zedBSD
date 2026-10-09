/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the parts of the BCM2711 graphics driver share.
 *
 * The display path and the V3D engine are kept apart: each has its own state,
 * its own stage names (P for the display, V for V3D) and, later, its own GPU
 * device, so that a fault of the render engine does not take the display down.
 */

#ifndef KERN_DRIVERS_GPU_BCM2711_PRIVATE_H
#define KERN_DRIVERS_GPU_BCM2711_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>

#include "drivers/gpu/bcm2711/bcm2711-gpu.h"
#include "drivers/gpu/bcm2711/display-flip.h"

/* The stage-mark family of the display path, and its boot parameter prefix. */
#define BCM2711_FAMILY_DISPLAY		"rpi4gpu"

/* The stage-mark family of the V3D engine, and its boot parameter prefix. */
#define BCM2711_FAMILY_V3D		"v3d"

/* The longest stage-mark line, in columns; the console is 80 columns wide. */
#define BCM2711_STAGE_LINE_COLUMNS	79U

/* How long a stage waits before a write that may blank the screen, in ms. */
#define BCM2711_STAGE_PAUSE_MS		3000U

/* The two timing generators that feed the HDMI ports: HDMI0 and HDMI1. */
#define BCM2711_TIMING_COUNT		2U

/* The two HDMI ports. */
#define BCM2711_HDMI_COUNT		2U

/* The interrupt number that names no interrupt. */
#define BCM2711_NO_IRQ			(-1)

/* The compositor's output channels. */
#define BCM2711_CHANNEL_COUNT		3U

/* The channel number that stands for "no channel feeds this output". */
#define BCM2711_NO_CHANNEL		3U

/* The words of the compositor's display list memory. */
#define BCM2711_LIST_WORDS		4096U

/* The most planes of one display list the readout keeps. */
#define BCM2711_LIST_PLANES		8U

/* The physical and virtual page size used by the initial V3D MMU mappings. */
#define BCM2711_V3D_PAGE_BYTES		4096U

/* The 4-byte PTE slots that cover V3D's complete 4 GiB virtual space. */
#define BCM2711_V3D_PAGE_ENTRIES		1048576U

/* The complete command-list byte counts of the initial 1x1 noop job. */
#define BCM2711_V3D_NOOP_BIN_BYTES	14U
#define BCM2711_V3D_NOOP_RENDER_BYTES	56U
#define BCM2711_V3D_NOOP_TILE_BYTES	19U

/* One tile's rounded allocation plus 8 KiB and 512 KiB overflow headroom. */
#define BCM2711_V3D_NOOP_POOL_BYTES	0x83000U

/* One tile's state array, allocated separately by the future job owner. */
#define BCM2711_V3D_NOOP_STATE_BYTES	256U

/*
 * One plane of a display list, as the readout decoded it.
 *
 * Only the words of an unscaled plane are decoded; scaled says the plane
 * carries scaling words, which the readout does not interpret.
 */
struct bcm2711_list_plane {
	uint32_t control;
	uint32_t words;
	uint32_t format;
	uint32_t order;
	bool scaled;
	bool flipped;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t pointer;
	uint32_t pitch;
};

/*
 * A whole display list, as the readout decoded it.
 *
 * start is the word the list begins at and end the word of its end marker;
 * plane_count may exceed BCM2711_LIST_PLANES, of which only the first are
 * kept.  valid is false when the walk met a malformed word or ran off the
 * end of the list memory.
 */
struct bcm2711_list {
	bool valid;
	uint32_t start;
	uint32_t end;
	uint32_t plane_count;
	struct bcm2711_list_plane planes[BCM2711_LIST_PLANES];
};

/*
 * One occupied interval of display-list SRAM, measured in words.
 *
 * The caller keeps these reservations for every current and pending list,
 * filter table and firmware-owned region until takeover has been observed.
 * words includes every occupied word starting at first; zero is invalid.
 */
struct bcm2711_list_range {
	uint32_t first;
	uint32_t words;
};

/*
 * A relocation prepared from a stable SRAM snapshot without hardware writes.
 *
 * words includes the end marker.  A successful preparation also supplies
 * that many unchanged words in the caller's image buffer.  Zero words means
 * preparation failed, so destination must not be published to the channel.
 */
struct bcm2711_list_copy {
	uint32_t source;
	uint32_t destination;
	uint32_t words;
};

/*
 * One caller-owned command buffer and its already mapped GPU VA interval.
 *
 * bytes points to capacity writable CPU bytes that belong only to this
 * buffer.  address names the same storage in the GPU's MMU.  used is zero
 * until generation succeeds; the owner retains the storage through job
 * completion and handles cache clean before submission.
 */
struct bcm2711_v3d_cl {
	uint8_t *bytes;
	uint32_t capacity;
	uint32_t address;
	uint32_t used;
};

/*
 * The three command buffers and tile-list pool of one shader-free noop job.
 *
 * The caller supplies distinct CPU storage and GPU mappings.  The pool is
 * GPU-writable and must not overlap a command buffer.  Only its mapped VA
 * is used here; allocation, tile state, cache and submission belong to the
 * job owner.  The command buffers remain alive until both queues finish.
 */
struct bcm2711_v3d_noop {
	struct bcm2711_v3d_cl bin;
	struct bcm2711_v3d_cl render;
	struct bcm2711_v3d_cl tile;
	uint32_t pool_address;
	uint32_t pool_bytes;
};

/*
 * One register window of a device.
 *
 * It records where the device tree says the window is and where the driver
 * mapped it.  mapped stays NULL until the window is mapped and is never
 * unmapped: the driver keeps its windows for the life of the system.
 */
struct bcm2711_window {
	uint64_t physical;
	uint64_t size;
	volatile uint8_t *mapped;
};

/*
 * One interrupt line of a device.
 *
 * irq is the kernel's number (the GIC interrupt ID), or BCM2711_NO_IRQ when
 * the device tree names none.  registered is set once the handler is
 * installed; the line stays masked until the stage that enables the device's
 * own interrupts unmasks it.
 */
struct bcm2711_irq_line {
	int irq;
	bool registered;
	uint64_t count;

	/* Optional device-source service, installed while the line is masked. */
	bool (*service)(void *owner);
	void *owner;
};

/* A persistent IRQ source borrows its owning display and names one PV or HVS. */
struct bcm2711_display_irq_source {
	struct bcm2711_display *display;
	uint32_t port;
};

/*
 * The display path: the compositor, the two timing generators of the HDMI
 * ports and the HDMI encoders.
 *
 * One instance exists for the system, filled by the P0 stage.  present is set
 * when the compositor was found and mapped; the rest is meaningful only then.
 */
struct bcm2711_display {
	bool present;
	struct bcm2711_window compositor;
	struct bcm2711_irq_line compositor_irq;
	struct bcm2711_window timing[BCM2711_TIMING_COUNT];
	struct bcm2711_irq_line timing_irq[BCM2711_TIMING_COUNT];
	uint64_t hdmi_physical[BCM2711_HDMI_COUNT];
	uint32_t hdmi_window_count[BCM2711_HDMI_COUNT];

	/*
	 * What stage N0 read of the firmware's display, all zero before it.
	 * port is the HDMI port the firmware's screen goes out of, channel the
	 * compositor channel that feeds it (BCM2711_NO_CHANNEL when none), and
	 * list the decoded display list the channel shows now.  screen_matches
	 * is set when that list shows the firmware's framebuffer one to one.
	 */
	bool readout_done;

	/* Set only after the new list and a fresh scanout frame are observed. */
	bool scanout_started;

	/* Persistent synchronous flip state shares lifetime with its IRQ owners. */
	struct bcm2711_flip_state flip;

	/* IRQ-written completion state; initialization arms only the new-mode frame. */
	volatile bool adoption_armed;
	volatile bool first_frame;
	volatile uint64_t underruns;
	struct bcm2711_display_irq_source compositor_source;
	struct bcm2711_display_irq_source timing_source[BCM2711_TIMING_COUNT];
	struct drv_bcm2711_boot_screen screen;
	uint32_t port_channel[BCM2711_HDMI_COUNT];
	uint32_t port;
	uint32_t channel;
	struct bcm2711_list list;
	bool screen_matches;
};

/*
 * The V3D 4.2 render engine.
 *
 * One instance exists for the system, filled by the V0 stage.  present is set
 * when the hub and the core windows were found and mapped.  No register of
 * the engine is read before its power and clock are up.
 */
struct bcm2711_v3d {
	bool present;
	struct bcm2711_window hub;
	struct bcm2711_window core;
	struct bcm2711_irq_line irq;
	bool has_power_domain;
	bool has_reset;
	uint32_t clock_id;
};

/* Stage marks and the driver's boot parameters (stage.c). */
bool bcm2711_stage_driver_off(void);
bool bcm2711_stage_allowed(const char *family, const char *stage);
void bcm2711_stage_mark(const char *family, const char *format, ...) __attribute__((format(printf, 2, 3)));
void bcm2711_stage_pause(const char *family, const char *stage);

/* Device tree helpers shared by both parts (fdt-util.c). */
int bcm2711_fdt_find(const struct drv_fdt *fdt, const char *compatible, uint32_t *node);
int bcm2711_fdt_window(const struct drv_fdt *fdt, uint32_t node, unsigned index, struct bcm2711_window *window);
int bcm2711_fdt_gic_irq(const struct drv_fdt *fdt, uint32_t node, unsigned index, int *irq);
uint32_t bcm2711_fdt_reg_count(const struct drv_fdt *fdt, uint32_t node);
int bcm2711_map_window(struct bcm2711_window *window);
int bcm2711_irq_install(struct bcm2711_irq_line *line);

/* Questions to the firmware through the mailbox (firmware.c). */
int bcm2711_firmware_get(uint32_t tag, uint32_t *values, unsigned request_count, unsigned answer_words);
int bcm2711_clock_hz(uint32_t clock_id, uint32_t *hz);
void bcm2711_clock_report(const char *family, const char *name, uint32_t clock_id);

/* The decoding of a compositor display list (list.c). */
void bcm2711_list_decode(const volatile uint32_t *memory, uint32_t start, struct bcm2711_list *list);
bool bcm2711_list_copy_prepare(const uint32_t *snapshot, uint32_t source, const struct bcm2711_list_range *reserved, unsigned reserved_count, uint32_t *image, uint32_t image_words, struct bcm2711_list_copy *copy);

bool bcm2711_list_screen_matches(const struct bcm2711_list *list, const struct drv_bcm2711_boot_screen *screen);

/* Software-only edits of a caller-owned V3D page table (mmu.c). */
int bcm2711_v3d_pages_map(uint32_t *table, uint32_t address, uint64_t physical, uint64_t bytes);
int bcm2711_v3d_pages_unmap(uint32_t *table, uint32_t address, uint64_t bytes);

/* Command-list generation without allocation or hardware access (cl.c). */
int bcm2711_v3d_noop_prepare(struct bcm2711_v3d_noop *job);

/* The stages of the two parts (display.c, v3d.c). */
int bcm2711_display_discover(const struct drv_fdt *fdt, struct bcm2711_display *display);
int bcm2711_display_readout(struct bcm2711_display *display, const struct drv_bcm2711_boot_screen *screen);
int bcm2711_display_irq_prepare(struct bcm2711_display *display);
int bcm2711_display_irq_open(struct bcm2711_display *display, uint32_t region);
void bcm2711_display_frame_arm(struct bcm2711_display *display);
void bcm2711_display_irq_mask(struct bcm2711_display *display);
int bcm2711_display_start(const struct drv_fdt *fdt, struct bcm2711_display *display);
int bcm2711_v3d_discover(const struct drv_fdt *fdt, struct bcm2711_v3d *v3d);

#endif
