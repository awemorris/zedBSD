/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Tests actual generic and PC/AT text snapshots without granting access to live display memory. */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <kern/device-io.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/text-display.h>
#include "drivers/platform/pcat/graphics/backend.h"
#include "drivers/platform/pcat/graphics/font.h"
#include "drivers/platform/pcat/graphics/text.h"

#define TEST_FRAMEBUFFER_BYTES 8192U
#define TEST_FRAMEBUFFER_WORDS (TEST_FRAMEBUFFER_BYTES / sizeof(uint32_t))
#define TEST_APERTURE_BYTES 0x20000U
#define TEST_GUARD UINT32_C(0xdeafbeef)

/* The fake boot surface remains live across text operations and becomes inaccessible during snapshots. */
static uint32_t *framebuffer;

/* The VGA fallback exposes its ordinary retained text aperture only outside snapshot execution. */
static uint16_t *aperture;

/* This serial selection chooses framebuffer discovery or VGA fallback during actual backend initialization. */
static unsigned use_framebuffer = 1U;

/* The host fixture rejects recursive text-lock acquisition while observing every glyph operation. */
static unsigned held_locks;

/* Snapshot execution is forbidden from performing VGA port writes, regardless of the active surface. */
static unsigned snapshot_active;

/* The CRTC write count separates ordinary cursor updates from pure RAM snapshot rendering. */
static unsigned port_writes;

static void test_fallback(void);
static void test_framebuffer(void);
static void test_vga(void);
static void test_mutation_generation(void);
static void capture(struct kern_text_snapshot *snapshot, void *surface, size_t bytes, int expected);
static void expect_generation(uint32_t before);
static void expect_image(const struct kern_text_snapshot *snapshot, int cursor);
static uint32_t expected_pixel(unsigned row, unsigned column, unsigned x, unsigned y, int cursor);
static void expect_unchanged(const void *first, const void *second, size_t bytes);

/*
 * Exercises actual dispatch and retained-cell rasterization on framebuffer and VGA surfaces.
 */
int
main(void)
{
	int error;

	/* Independent mappings let snapshots prove they never read or write the live display aperture. */
	framebuffer = mmap(NULL, TEST_FRAMEBUFFER_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(framebuffer != MAP_FAILED);
	aperture = mmap(NULL, TEST_APERTURE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(aperture != MAP_FAILED);
	test_fallback();
	test_framebuffer();
	test_mutation_generation();
	test_vga();

	/* All actual snapshot exits release their text lock and leave no retained caller buffer. */
	assert(held_locks == 0U);
	error = munmap(aperture, TEST_APERTURE_BYTES);
	assert(error == 0);
	error = munmap(framebuffer, TEST_FRAMEBUFFER_BYTES);
	assert(error == 0);

	/* Succeeded: caller RAM receives exact retained glyphs without disturbing either live display surface. */
	puts("text snapshot: PASS (actual generic + PCAT, exact BGRA glyphs/cursor, guarded MMIO, restoration, generation, fallback)");
	return 0;
}

/*
 * Supplies a deliberately padded RGBX framebuffer to the actual text backend.
 */
int
drv_pcat_graphics_backend_get_framebuffer(
	volatile uint32_t **pixels,
	unsigned *width,
	unsigned *height,
	unsigned *stride,
	int *rgbx)
{
	/* Fallback initialization must leave framebuffer output arguments unused. */
	if (use_framebuffer == 0U)
		return 0;

	/* Nonzero centering and row padding make accidental snapshot state changes observable. */
	*pixels = framebuffer;
	*width = 21U;
	*height = 35U;
	*stride = 32U;
	*rgbx = 1;

	/* Succeeded: a 2-by-2 cell grid occupies a centered part of a larger RGBX surface. */
	return 1;
}

/*
 * Supplies simple independent glyphs with oversized, short and missing-font cases.
 */
int
drv_pcat_font_get_glyph(
	uint32_t character,
	uint8_t glyph[32],
	unsigned *width,
	unsigned *height)
{
	unsigned index;

	/* Both ordinary drawing and snapshot rendering must own the actual text state lock. */
	assert(held_locks == 1U);
	memset(glyph, 0, 32U);

	/* Alternating diagonal pairs expose every full-size cell row and clamp oversized font metrics. */
	if (character == 'A') {
		*width = 12U;
		*height = 20U;
		for (index = 0U; index < 20U; index++) {
			if ((index & 1U) == 0U) {
				glyph[index] = 0x81U;
			} else {
				glyph[index] = 0x42U;
			}
		}

		/* Succeeded: the caller must clamp these metrics to its 8-by-16 cell. */
		return 1;
	}

	/* A short four-dot glyph leaves the rest of the fixed cell as background. */
	if (character == 'B') {
		*width = 4U;
		*height = 8U;
		for (index = 0U; index < 8U; index++)
			glyph[index] = 0x90U;

		/* Succeeded: right and bottom padding must not read nonexistent glyph dots. */
		return 1;
	}

	/* Missing characters, including spaces, use the actual renderer's blank-cell fallback. */
	return 0;
}

/*
 * Publishes an initially unowned host lock for the actual text state.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Reinitialization is allowed only after the previous serial text operation released its lock. */
	assert(held_locks == 0U);
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;

	/* Succeeded: the next actual renderer operation can acquire this leaf lock. */
	return;
}

/*
 * Rejects recursive acquisition while exposing held state to glyph and VGA collaborators.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	unsigned previous;

	/* Each serialized renderer operation must acquire exactly one unowned lock. */
	previous = __atomic_exchange_n(&lock->held.value, 1U, __ATOMIC_ACQUIRE);
	assert(previous == 0U);
	assert(held_locks == 0U);
	held_locks++;

	/* Succeeded: this operation owns the text state with a synthetic enabled IRQ flag. */
	return 1U;
}

/*
 * Releases the actual text operation's single host lock.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long flags)
{
	/* Early geometry and capacity exits must balance the same ownership as complete snapshots. */
	assert(flags == 1U);
	assert(held_locks == 1U);
	held_locks--;
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);

	/* Succeeded: another text operation can observe only the restored live state. */
	return;
}

/*
 * Maps the known legacy VGA aperture for the actual fallback attach path.
 */
int
kern_device_map(
	uint64_t address,
	size_t bytes,
	unsigned attributes,
	void **mapped)
{
	/* No snapshot is allowed to discover or remap display hardware. */
	assert(snapshot_active == 0U);
	assert(address == 0xa0000U);
	assert(bytes == TEST_APERTURE_BYTES);
	assert(attributes == KERN_DEVICE_UNCACHED);
	*mapped = aperture;

	/* Succeeded: VGA text initialization can use its real offset into the fake aperture. */
	return 0;
}

/*
 * Records ordinary CRTC cursor writes and rejects hardware output during snapshots.
 */
void
kern_io_out8(
	uint16_t port,
	uint8_t value)
{
	/* Only ordinary text operations may update the VGA cursor registers. */
	assert(snapshot_active == 0U);
	assert(held_locks == 1U);
	assert(port == 0x3d4U || port == 0x3d5U);
	(void)value;
	port_writes++;

	/* Succeeded: the fixture records hardware activity without performing host port I/O. */
	return;
}

/* Exercises absence and optional-tail fallback without substituting the generic dispatcher. */
static void
test_fallback(void)
{
	static const struct kern_text_ops legacy_ops = {
		drv_pcat_text_get_size,
		drv_pcat_text_putc,
		drv_pcat_text_write,
		drv_pcat_text_clear,
		drv_pcat_text_set_cursor,
		drv_pcat_text_get_cursor,
		drv_pcat_text_show_cursor,
		drv_pcat_text_update_cursor,
		drv_pcat_text_suspend,
		drv_pcat_text_resume,
		NULL
	};
	struct kern_text_snapshot snapshot;
	uint32_t before;
	int error;

	/* Unpublished backends have neither text output nor snapshot support. */
	memset(&snapshot, 0, sizeof(snapshot));
	before = kern_text_generation();
	error = kern_text_snapshot(NULL);
	assert(error == EINVAL);
	error = kern_text_snapshot(&snapshot);
	assert(error == ENOTSUP);
	kern_text_putc('A');
	error = kern_text_ready();
	assert(error == 0);
	expect_generation(before);

	/* A valid old table lacking only the optional snapshot tail remains usable as a text backend. */
	kern_text_register(&legacy_ops);
	expect_generation(before + 1U);
	error = kern_text_snapshot(&snapshot);
	assert(error == ENOTSUP);
	expect_generation(before + 1U);

	/* Backend withdrawal is another publication event, independent of whether any cells were painted. */
	kern_text_register(NULL);
	expect_generation(before + 2U);
	error = drv_pcat_text_snapshot(NULL);
	assert(error == EINVAL);
	error = drv_pcat_text_snapshot(&snapshot);
	assert(error == ENODEV);
	assert(held_locks == 0U);

	/* Succeeded: optional snapshots preserve the established unregistered and legacy-backend behavior. */
	return;
}

/* Checks exact glyph pixels and complete live-framebuffer state restoration. */
static void
test_framebuffer(void)
{
	struct kern_text_snapshot snapshot;
	uint32_t saved[TEST_FRAMEBUFFER_WORDS];
	uint32_t image[514];
	uint32_t image_before[514];
	uint32_t generation;
	unsigned index;
	unsigned row;
	unsigned column;
	int visible;
	int error;

	/* The live display starts with canaries outside its centered character grid. */
	for (index = 0U; index < TEST_FRAMEBUFFER_WORDS; index++)
		framebuffer[index] = TEST_GUARD;

	/* Actual initialization chooses the padded RGBX surface and publishes the PC/AT snapshot callback. */
	generation = kern_text_generation();
	drv_pcat_text_init();
	expect_generation(generation + 1U);
	kern_text_show_cursor(0);
	kern_text_write(0U, 0U, 0x1eU, "AB");
	kern_text_write(1U, 0U, 0x24U, "?A");
	error = kern_text_set_cursor(1U, 1U);
	assert(error == 0);
	kern_text_show_cursor(1);
	generation = kern_text_generation();
	kern_text_get_cursor(&row, &column, &visible);
	assert(row == 1U);
	assert(column == 1U);
	assert(visible == 1);
	expect_generation(generation);

	/* Geometry queries expose a tightly packed cell image rather than the boot framebuffer's padded layout. */
	memset(&snapshot, 0, sizeof(snapshot));
	capture(&snapshot, framebuffer, TEST_FRAMEBUFFER_BYTES, 0);
	assert(snapshot.width == 16U);
	assert(snapshot.height == 32U);
	assert(snapshot.stride == 64U);
	snapshot.bytes = 1U;
	capture(&snapshot, framebuffer, TEST_FRAMEBUFFER_BYTES, EINVAL);

	/* An undersized destination must remain entirely untouched, including both output canaries. */
	for (index = 0U; index < 514U; index++)
		image[index] = TEST_GUARD;

	/* The final valid snapshot pixel is followed immediately by an independent guard word. */
	memcpy(image_before, image, sizeof(image));
	snapshot.pixels = &image[1];
	snapshot.bytes = 2047U;
	capture(&snapshot, framebuffer, TEST_FRAMEBUFFER_BYTES, ENOSPC);
	expect_unchanged(image, image_before, sizeof(image));

	/* A complete RAM snapshot must render every BGRA pixel while the original RGBX framebuffer is inaccessible. */
	memcpy(saved, framebuffer, sizeof(saved));
	snapshot.bytes = 2048U;
	capture(&snapshot, framebuffer, TEST_FRAMEBUFFER_BYTES, 0);
	expect_image(&snapshot, 1);
	assert(image[0] == TEST_GUARD);
	assert(image[513] == TEST_GUARD);
	expect_unchanged(framebuffer, saved, sizeof(saved));
	expect_generation(generation);

	/* Hidden cursor snapshots use the same retained cells without changing text geometry or position. */
	kern_text_show_cursor(0);
	generation = kern_text_generation();
	capture(&snapshot, framebuffer, TEST_FRAMEBUFFER_BYTES, 0);
	expect_image(&snapshot, 0);
	expect_generation(generation);

	/* Suspended text still has retained cells to snapshot, and snapshot must restore the suspended state. */
	kern_text_suspend();
	generation = kern_text_generation();
	memcpy(saved, framebuffer, sizeof(saved));
	capture(&snapshot, framebuffer, TEST_FRAMEBUFFER_BYTES, 0);
	expect_image(&snapshot, 0);
	error = drv_pcat_text_ready();
	assert(error == 0);
	expect_generation(generation);
	kern_text_write(0U, 0U, 0x0fU, "B");
	expect_unchanged(framebuffer, saved, sizeof(saved));

	/* Resume restores ordinary RGBX drawing; a later write must target the original origin and padded stride. */
	kern_text_resume();
	kern_text_write(0U, 0U, 0x1eU, "B");
	assert(framebuffer[1U * 32U + 2U] == UINT32_C(0x0055ffff));
	assert(framebuffer[1U * 32U + 3U] == UINT32_C(0x00aa0000));
	assert(framebuffer[1U * 32U + 5U] == UINT32_C(0x0055ffff));
	assert(framebuffer[1U * 32U + 6U] == UINT32_C(0x00aa0000));
	assert(framebuffer[0] == TEST_GUARD);
	assert(framebuffer[1U * 32U + 1U] == TEST_GUARD);
	assert(framebuffer[1U * 32U + 18U] == TEST_GUARD);
	assert(framebuffer[34U * 32U] == TEST_GUARD);
	assert(port_writes == 0U);

	/* Succeeded: snapshot redirection preserved the live pointer, format, stride, origin, readiness and cursor state. */
	return;
}

/* Ensures VGA text snapshots use retained cells without touching text VRAM or cursor ports. */
static void
test_vga(void)
{
	struct kern_text_snapshot snapshot;
	uint16_t *saved;
	uint32_t *image;
	uint32_t generation;
	unsigned before;
	int error;

	/* Real fallback attachment derives its fixed geometry and text-memory offset from the mapped aperture. */
	use_framebuffer = 0U;
	memset(aperture, 0xa5, TEST_APERTURE_BYTES);
	drv_pcat_text_init();
	kern_text_show_cursor(0);
	kern_text_write(0U, 0U, 0x1eU, "AB");
	memset(&snapshot, 0, sizeof(snapshot));
	capture(&snapshot, aperture, TEST_APERTURE_BYTES, 0);
	assert(snapshot.width == 640U);
	assert(snapshot.height == 400U);
	assert(snapshot.stride == 2560U);

	/* Independent host buffers preserve both the VGA aperture and its complete expected output size. */
	saved = malloc(TEST_APERTURE_BYTES);
	assert(saved != NULL);
	image = malloc((size_t)snapshot.stride * snapshot.height);
	assert(image != NULL);
	memcpy(saved, aperture, TEST_APERTURE_BYTES);
	snapshot.pixels = image;
	snapshot.bytes = (size_t)snapshot.stride * snapshot.height;
	generation = kern_text_generation();
	before = port_writes;
	capture(&snapshot, aperture, TEST_APERTURE_BYTES, 0);
	assert(image[0] == UINT32_C(0x00ffff55));
	assert(image[1] == UINT32_C(0x000000aa));
	assert(image[7] == UINT32_C(0x00ffff55));
	assert(image[8] == UINT32_C(0x00ffff55));
	assert(image[9] == UINT32_C(0x000000aa));
	assert(image[639] == 0U);
	assert(image[399U * 640U + 639U] == 0U);
	assert(port_writes == before);
	expect_generation(generation);
	expect_unchanged(aperture, saved, TEST_APERTURE_BYTES);

	/* Ordinary writes after snapshot must still land in VGA cell memory rather than the freed RAM snapshot. */
	free(image);
	kern_text_write(0U, 0U, 0x24U, "A");
	assert(aperture[0x18000U / 2U] == UINT16_C(0x2441));
	kern_text_update_cursor();
	assert(port_writes > before);
	free(saved);
	error = drv_pcat_text_ready();
	assert(error != 0);

	/* Succeeded: the VGA surface and hardware cursor behavior survive independent RAM rendering. */
	return;
}

/* Makes the live display inaccessible throughout actual snapshot dispatch and rendering. */
static void
capture(
	struct kern_text_snapshot *snapshot,
	void *surface,
	size_t bytes,
	int expected)
{
	uint32_t generation;
	unsigned before;
	int error;
	int captured;

	/* Any MMIO read or write would fault, and any CRTC port access is rejected by its mock. */
	generation = kern_text_generation();
	before = port_writes;
	error = mprotect(surface, bytes, PROT_NONE);
	assert(error == 0);
	snapshot_active = 1U;
	captured = kern_text_snapshot(snapshot);
	snapshot_active = 0U;
	error = mprotect(surface, bytes, PROT_READ | PROT_WRITE);
	assert(error == 0);
	assert(captured == expected);
	assert(held_locks == 0U);
	assert(port_writes == before);
	expect_generation(generation);

	/* Succeeded: only caller-owned RAM could have been accessed by the snapshot renderer. */
	return;
}

/* Checks that read-only observations never change the redraw hint. */
static void
expect_generation(
	uint32_t before)
{
	uint32_t actual;

	/* Generation changes belong to completed text mutations or backend publication, not snapshots. */
	actual = kern_text_generation();
	assert(actual == before);

	/* Succeeded: the redraw hint has the expected independent observation value. */
	return;
}

/* Checks every pixel in the independent 2-by-2-cell glyph and cursor image. */
static void
expect_image(
	const struct kern_text_snapshot *snapshot,
	int cursor)
{
	unsigned x;
	unsigned y;
	uint32_t expected;

	/* Expected pixels derive from the chosen glyph shapes and VGA attributes, not the renderer's helper functions. */
	for (y = 0U; y < 32U; y++) {
		for (x = 0U; x < 16U; x++) {
			expected = expected_pixel(y / 16U, x / 8U, x % 8U, y % 16U, cursor);
			assert(snapshot->pixels[y * 16U + x] == expected);
		}
	}

	/* Succeeded: every fixed-cell pixel, short-glyph padding and cursor inversion matches independently. */
	return;
}

/* Describes the independently chosen glyph dots and attribute colors of the 2-by-2 test grid. */
static uint32_t
expected_pixel(
	unsigned row,
	unsigned column,
	unsigned x,
	unsigned y,
	int cursor)
{
	uint32_t foreground;
	uint32_t background;
	uint32_t swap;
	int dot;

	/* The first row is bright yellow on blue; the second is red on green. */
	foreground = UINT32_C(0x00ffff55);
	background = UINT32_C(0x000000aa);
	if (row != 0U) {
		foreground = UINT32_C(0x00aa0000);
		background = UINT32_C(0x0000aa00);
	}

	/* Only the lower-right A cell receives cursor color inversion. */
	if (row == 1U && column == 1U) {
		if (cursor) {
			swap = foreground;
			foreground = background;
			background = swap;
		}
	}

	/* A glyph alternates outer and inner dot pairs across its fixed sixteen rows. */
	dot = 0;
	if (row == column) {
		if ((y & 1U) == 0U) {
			if (x == 0U || x == 7U)
				dot = 1;
		} else {
			if (x == 1U || x == 6U)
				dot = 1;
		}
	} else if (row == 0U) {
		/* The top-right short B has only two dots in each of its first eight rows. */
		if (y < 8U) {
			if (x == 0U || x == 3U)
				dot = 1;
		}
	}

	/* A missing bottom-left glyph and every unused dot receive the cell background. */
	if (!dot)
		return background;

	/* Succeeded: an explicitly chosen glyph dot receives the cell foreground. */
	return foreground;
}

/* Compares caller-owned buffers after snapshot to detect accidental writes outside its RAM destination. */
static void
expect_unchanged(
	const void *first,
	const void *second,
	size_t bytes)
{
	int comparison;

	/* The renderer must preserve every live surface byte and every refused output byte. */
	comparison = memcmp(first, second, bytes);
	assert(comparison == 0);

	/* Succeeded: this buffer retains its exact pre-snapshot contents. */
	return;
}

/* Confirms every completed generic text mutation provides a new console-redraw observation. */
static void
test_mutation_generation(void)
{
	uint32_t generation;
	unsigned row;
	unsigned column;
	unsigned columns;
	unsigned rows;
	int visible;
	int error;

	/* Read-only geometry and cursor observations do not trigger a redundant console redraw. */
	generation = kern_text_generation();
	kern_text_get_size(&columns, &rows);
	assert(columns == 2U);
	assert(rows == 2U);
	kern_text_get_cursor(&row, &column, &visible);
	expect_generation(generation);

	/* A completed clear publishes a new retained screen to snapshot consumers. */
	kern_text_clear();
	expect_generation(generation + 1U);
	generation++;

	/* Stream output publishes the newly stored character before changing the redraw hint. */
	kern_text_putc('A');
	expect_generation(generation + 1U);
	generation++;

	/* Fixed-position text also notifies a worker which observes only the common generation. */
	kern_text_write(1U, 0U, 0x07U, "B");
	expect_generation(generation + 1U);
	generation++;

	/* Cursor placement and visibility are independently observable text-image changes. */
	error = kern_text_set_cursor(1U, 1U);
	assert(error == 0);
	expect_generation(generation + 1U);
	generation++;
	kern_text_show_cursor(1);
	expect_generation(generation + 1U);
	generation++;

	/* Cursor repaint follows the same publication ordering as ordinary character output. */
	kern_text_update_cursor();
	expect_generation(generation + 1U);
	generation++;

	/* Graphics ownership transitions publish their completed suspension and resumption states. */
	kern_text_suspend();
	expect_generation(generation + 1U);
	generation++;
	kern_text_resume();
	expect_generation(generation + 1U);

	/* Succeeded: the console worker can observe every completed mutation without sampling display memory. */
	return;
}
