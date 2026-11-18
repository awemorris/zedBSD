/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 HAL's reading of the UEFI loader's handoff.
 *
 * A handoff of any layout from the first on is taken: a later layout only
 * appends fields, so the first layout's fields are read from every one and
 * the second layout's only from a handoff that has them.  The second
 * layout names the screen the firmware left on and the RAM the firmware
 * keeps after boot services end; the HAL keeps those ranges the device
 * tree does not already reserve, and refuses to go on when it cannot keep
 * them all rather than hand firmware memory to the allocator.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "loader.h"

/* The bytes of one pixel of a screen the loader hands over. */
#define LOADER_PIXEL_BYTES	4U

static enum riscv64_loader_screen take_screen(const struct zbl6_riscv64_handoff *handoff, struct riscv64_framebuffer *framebuffer);
static int keep_range(const struct riscv64_fdt_info *info, struct riscv64_loader_extras *extras, uint64_t base, uint64_t size);
static bool range_touches_memory(const struct riscv64_fdt_info *info, uint64_t base, uint64_t end);
static bool range_inside(const struct riscv64_fdt_range *ranges, unsigned count, uint64_t base, uint64_t end);

/*
 * Checks that a loader's handoff has at least the first layout.
 *
 * Returns RISCV64_LOADER_OK, RISCV64_LOADER_OLD_VERSION for a version
 * before the first, or RISCV64_LOADER_TOO_SHORT for a record that ends
 * before the first layout does.
 */
int
riscv64_loader_check(
	const struct zbl6_riscv64_handoff *handoff)
{
	/* Refuses a version before the first layout. */
	if (handoff->version < 1U)
		return RISCV64_LOADER_OLD_VERSION;

	/* Refuses a record shorter than the first layout's fields. */
	if (handoff->size < ZBL6_RISCV64_HANDOFF_V1_SIZE)
		return RISCV64_LOADER_TOO_SHORT;

	/* Succeeded: the first layout's fields are all there. */
	return RISCV64_LOADER_OK;
}

/*
 * Takes the second layout's screen and the firmware's RAM from a handoff
 * the first layout's check passed.
 *
 * The device tree's facts decide which ranges are RAM and which the tree
 * reserves already.  Returns RISCV64_LOADER_OK with the extras filled, or
 * why the firmware's RAM cannot all be kept: a count past the record's
 * array, a range that wraps, a loader that dropped ranges, or more ranges
 * than the HAL holds.  The extras hold what was taken either way.
 */
int
riscv64_loader_take(
	const struct zbl6_riscv64_handoff *handoff,
	const struct riscv64_fdt_info *info,
	struct riscv64_loader_extras *extras)
{
	uint64_t base;
	uint64_t size;
	unsigned index;
	int error;

	/* Starts with nothing taken, which is what the first layout leaves. */
	hal_memset(extras, 0, sizeof(*extras));
	extras->layout = 1;

	/* A handoff of the first layout has nothing more to give. */
	if (handoff->version < 2U || handoff->size < ZBL6_RISCV64_HANDOFF_V2_SIZE)
		return RISCV64_LOADER_OK;

	/* Records that the handoff has the second layout's fields. */
	extras->layout = 2;

	/* Takes the screen, when the loader handed one over that can be drawn on. */
	extras->screen = take_screen(handoff, &extras->framebuffer);

	/* Records how many ranges the loader gave, refusing a count the record's array cannot hold. */
	extras->reserved_given = handoff->reserved_count;
	if (handoff->reserved_count > ZBL6_RISCV64_RESERVED_MAX)
		return RISCV64_LOADER_BAD_COUNT;

	/* Keeps each range of the firmware's RAM the tree does not reserve already. */
	for (index = 0; index < handoff->reserved_count; index++) {
		/* Keeps the range, stopping on one that cannot be kept. */
		base = handoff->reserved[index].phys;
		size = handoff->reserved[index].size;
		error = keep_range(info, extras, base, size);
		if (error != RISCV64_LOADER_OK)
			return error;
	}

	/* Refuses a loader that had to leave ranges out. */
	if ((handoff->reserved_flags & ZBL6_RISCV64_RESERVED_OVERFLOW) != 0)
		return RISCV64_LOADER_OVERFLOW;

	/* Succeeded: the screen and every range the firmware keeps. */
	return RISCV64_LOADER_OK;
}

/*
 * Takes the screen of a second-layout handoff when it is one the kernel's
 * framebuffer record can name: a known format, rows at least as long as
 * the width in whole pixels, and memory that holds every row.
 */
static enum riscv64_loader_screen
take_screen(
	const struct zbl6_riscv64_handoff *handoff,
	struct riscv64_framebuffer *framebuffer)
{
	uint64_t needed;

	/* A handoff whose screen has no memory has no screen. */
	if (handoff->framebuffer_size == 0)
		return RISCV64_LOADER_SCREEN_NONE;

	/* Refuses a pixel layout the kernel's record does not name. */
	if (handoff->framebuffer_format != KERN_BOOT_FRAMEBUFFER_RGBX8888 &&
	    handoff->framebuffer_format != KERN_BOOT_FRAMEBUFFER_BGRX8888)
		return RISCV64_LOADER_SCREEN_REFUSED;

	/* Refuses an empty screen. */
	if (handoff->framebuffer_width == 0 || handoff->framebuffer_height == 0)
		return RISCV64_LOADER_SCREEN_REFUSED;

	/* Refuses rows shorter than the width or not whole pixels. */
	if ((uint64_t)handoff->framebuffer_pitch < (uint64_t)handoff->framebuffer_width * LOADER_PIXEL_BYTES)
		return RISCV64_LOADER_SCREEN_REFUSED;
	if ((handoff->framebuffer_pitch % LOADER_PIXEL_BYTES) != 0)
		return RISCV64_LOADER_SCREEN_REFUSED;

	/* Refuses memory too small for every row, or that wraps the address space. */
	needed = (uint64_t)handoff->framebuffer_pitch * handoff->framebuffer_height;
	if (handoff->framebuffer_size < needed)
		return RISCV64_LOADER_SCREEN_REFUSED;
	if (handoff->framebuffer_size > ~0ULL - handoff->framebuffer_phys)
		return RISCV64_LOADER_SCREEN_REFUSED;

	/* Copies the screen out of the record, which is not kept. */
	framebuffer->phys = handoff->framebuffer_phys;
	framebuffer->size = handoff->framebuffer_size;
	framebuffer->width = handoff->framebuffer_width;
	framebuffer->height = handoff->framebuffer_height;
	framebuffer->pitch = handoff->framebuffer_pitch;
	framebuffer->format = handoff->framebuffer_format;

	/* Succeeded: the screen can be named and drawn on. */
	return RISCV64_LOADER_SCREEN_TAKEN;
}

/*
 * Adds one range of the firmware's RAM to the kept ones, joined with
 * every kept range it touches.
 *
 * A range outside every RAM bank, or inside one the tree reserves already,
 * needs nothing.  Returns RISCV64_LOADER_OK, RISCV64_LOADER_BAD_RANGE for a
 * range that wraps, or RISCV64_LOADER_TOO_MANY when no slot is left.
 */
static int
keep_range(
	const struct riscv64_fdt_info *info,
	struct riscv64_loader_extras *extras,
	uint64_t base,
	uint64_t size)
{
	struct riscv64_fdt_range *kept;
	uint64_t end;
	uint64_t kept_end;
	unsigned index;
	bool touches;
	bool covered;

	/* An empty range keeps nothing. */
	if (size == 0)
		return RISCV64_LOADER_OK;

	/* Refuses a range whose end does not fit. */
	if (size > ~0ULL - base)
		return RISCV64_LOADER_BAD_RANGE;
	end = base + size;

	/* A range outside RAM is never allocated anyway. */
	touches = range_touches_memory(info, base, end);
	if (!touches)
		return RISCV64_LOADER_OK;

	/* A range the tree reserves already is kept from the allocator by it. */
	covered = range_inside(info->reserved, info->reserved_count, base, end);
	if (covered)
		return RISCV64_LOADER_OK;

	/* A range the tree leaves out of the map is kept from the allocator too. */
	covered = range_inside(info->nomap, info->nomap_count, base, end);
	if (covered)
		return RISCV64_LOADER_OK;

	/* Absorbs every kept range the new one touches, so the kept ones never touch each other. */
	index = 0;
	while (index < extras->reserved_count) {
		/* Takes the bounds of the kept range in this slot. */
		kept = &extras->reserved[index];
		kept_end = kept->base + kept->size;

		/* Steps over a kept range that lies apart from the new one. */
		if (kept->base > end || kept_end < base) {
			index++;
			continue;
		}

		/* Widens the new range over the kept one and takes the last slot's range into its place. */
		if (kept->base < base)
			base = kept->base;
		if (kept_end > end)
			end = kept_end;
		extras->reserved_count--;
		extras->reserved[index] = extras->reserved[extras->reserved_count];
	}

	/* Refuses a range no slot is left for. */
	if (extras->reserved_count >= RISCV64_LOADER_MAX_RESERVED)
		return RISCV64_LOADER_TOO_MANY;

	/* Succeeded: the range, joined with the ones it touched, is kept. */
	extras->reserved[extras->reserved_count].base = base;
	extras->reserved[extras->reserved_count].size = end - base;
	extras->reserved_count++;
	return RISCV64_LOADER_OK;
}

/* Reports whether a range overlaps any RAM bank the tree describes. */
static bool
range_touches_memory(
	const struct riscv64_fdt_info *info,
	uint64_t base,
	uint64_t end)
{
	uint64_t bank_end;
	unsigned index;

	/* Looks for a bank that shares a byte with the range. */
	for (index = 0; index < info->memory_count; index++) {
		/* Skips a bank that starts at or after the range's end. */
		if (info->memory[index].base >= end)
			continue;

		/* Skips a bank that ends at or before the range's start. */
		bank_end = info->memory[index].base + info->memory[index].size;
		if (bank_end <= base)
			continue;
		return true;
	}

	/* No bank shares a byte with the range. */
	return false;
}

/* Reports whether a range lies wholly inside one of the given ranges. */
static bool
range_inside(
	const struct riscv64_fdt_range *ranges,
	unsigned count,
	uint64_t base,
	uint64_t end)
{
	uint64_t outer_end;
	unsigned index;

	/* Looks for a range that holds the whole of this one. */
	for (index = 0; index < count; index++) {
		/* Skips a range that starts after this one. */
		if (ranges[index].base > base)
			continue;

		/* Skips a range that ends before this one does. */
		outer_end = ranges[index].base + ranges[index].size;
		if (outer_end < end)
			continue;
		return true;
	}

	/* No range holds the whole of this one. */
	return false;
}
