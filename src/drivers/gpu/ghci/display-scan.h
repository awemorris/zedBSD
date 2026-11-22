/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scan-out side of a GHCI device's display (display-scan.c), which
 * the display's operations (display.c) use: the display commands the
 * ring places, the records of its flips and the waits for them, and the
 * memory the display scans.
 *
 * Every function but ghci_scan_mark, ghci_scan_sleep, ghci_scan_flush,
 * ghci_scan_alloc, ghci_scan_write and the ones that only read the mode
 * table is called with the display's lock held.
 */

#ifndef DRIVERS_GPU_GHCI_DISPLAY_SCAN_H
#define DRIVERS_GPU_GHCI_DISPLAY_SCAN_H

#include <stdint.h>

#include "ghci.h"
#include "ghci-cmd.h"

/* The display's one output as the GPU core names it, and the first generation of it. */
#define GHCI_DISPLAY_ID			1U
#define GHCI_DISPLAY_FIRST_GENERATION	1U

/* A frame's pixels: 32 bits. */
#define GHCI_DISPLAY_PIXEL_BYTES	4U

/*
 * The line the display's addresses and rows are aligned to.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3, CMD_FLIP's
 * base and stride_bytes on 64 bytes).
 */
#define GHCI_DISPLAY_LINE_BYTES		GHCI_DISP_ALIGN

/*
 * The time a presentation's adoption may take before the output is
 * uncertain.  The flip waits in the ring behind the other sessions'
 * work: past the hang time that work has moved or been reset, and the
 * flip placed again.
 */
#define GHCI_DISPLAY_ADOPTION_MS	(GHCI_HANG_MS + 2000U)

/* Reports whether the ring can take the display's packets now: 1 when it can. */
int ghci_scan_ring_usable(struct ghci_device *device);

/* Reports the index of the display's largest mode, by its pixels. */
unsigned ghci_scan_largest_mode(const struct ghci_display *display);

/* Reports the bytes a frame of every mode fits in, its rows on the memory line. */
uint64_t ghci_scan_frame_bytes(const struct ghci_display *display);

/* Reports the bytes between the rows of a frame of a width, a multiple of the memory line. */
uint32_t ghci_scan_stride(uint32_t width);

/* Reports the time a frame is shown, on the scheduler's clock in nanoseconds. */
uint64_t ghci_scan_now_ns(void);

/* Reads the records the display wrote and places what waits; the lock is held. */
void ghci_scan_update(struct ghci_device *device);

/*
 * Records an image of plane 0 for the display; a mailbox present
 * (fifo 0) replaces the newest one still waiting.  Returns 0, or EAGAIN
 * when the queue is full; the lock is held.
 */
int ghci_scan_record(struct ghci_device *device, const struct ghci_display_image *image, int fifo);

/* Shows a cursor surface: flips it now when the display is lit, after the next lighting otherwise. */
void ghci_scan_cursor(struct ghci_device *device, int surface);

/* Places the cursor at a place and shows or hides it, now or once the ring takes it; the lock is held. */
void ghci_scan_cursor_state(struct ghci_device *device, int32_t x, int32_t y, int show);

/* Marks the lease ended and has the display go dark; the lock is held. */
void ghci_scan_lease_end(struct ghci_device *device);

/* Reports whether an ended lease has let go of everything: 1 when it has; the lock is held. */
int ghci_scan_lease_done(struct ghci_device *device);

/* Lets go of held memory later, once the display's lock is let go; the lock is held. */
void ghci_scan_drop(struct ghci_device *device, struct ghci_allocation *memory);

/* Lets go of the memory dropped meanwhile, and of the boot image once due; no lock of the device is held. */
void ghci_scan_flush(struct ghci_device *device);

/* Reports the wake-up sequence of the flip waiters, read before a wait's look. */
uint64_t ghci_scan_mark(struct ghci_device *device);

/* Sleeps until a flip's interrupt, a short poll, or the deadline, whichever comes first. */
void ghci_scan_sleep(struct ghci_device *device, uint64_t mark, uint64_t deadline);

/*
 * Takes memory the display scans, the device's own: local memory on a
 * discrete GPU when local is set (memory only the GPU writes), host
 * memory below the GPU's limit otherwise.  The caller holds no lock of
 * the device.
 */
int ghci_scan_alloc(struct ghci_device *device, uint64_t bytes, int local, struct ghci_allocation **result);

/* Reports whether the display can scan memory: local memory, or host memory below HOST_LIMIT. */
int ghci_scan_reachable(const struct ghci_device *device, const struct ghci_allocation *memory);

/* Writes bytes into host memory the display scans; the caller may hold any lock but the interrupt's. */
int ghci_scan_write(struct ghci_device *device, struct ghci_allocation *memory, uint64_t offset, const void *bytes, uint64_t length);

#endif
