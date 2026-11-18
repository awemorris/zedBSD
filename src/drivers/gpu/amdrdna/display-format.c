/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words of the display engine the takeover writes: the plane's flip
 * and its interrupt, the timing generator's frame interrupt and frame
 * count, the hot-plug line's interrupt, and the hardware cursor of the
 * plane and of its pixel processor.
 *
 * Every function takes the word the caller read and the change it wants,
 * and gives back the word to write; nothing here touches the hardware, so
 * the same code runs in the kernel and in the host tests.  The bits a word
 * acknowledges an event with are never written back as they were read: a
 * word is only ever written with the acknowledgement the caller asked for.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The plane's flip control: bit 1 makes a new address take effect at once
 * instead of at the next frame's update, and bit 8 says a flip is still
 * waiting for that update.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK,
 * HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK).
 */
#define FORMAT_FLIP_AT_ONCE		0x00000002U
#define FORMAT_FLIP_WAITING		0x00000100U

/*
 * The plane's flip interrupt: bit 0 lets a finished flip interrupt, bits 8
 * and 9 acknowledge a finished flip and a surface flipped away from.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_MASK_MASK,
 * HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_CLEAR_MASK,
 * HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_CLEAR_MASK).
 */
#define FORMAT_FLIP_INTERRUPT_ON	0x00000001U
#define FORMAT_FLIP_ACKNOWLEDGE		0x00000100U
#define FORMAT_FLIP_AWAY_ACKNOWLEDGE	0x00000200U

/*
 * The timing generator's sync status: bit 0 lets the start of a frame's
 * fetch interrupt, bit 4 acknowledges it, and bits 10, 16 and 22
 * acknowledge the three other sync events.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_EN_MASK,
 * OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK,
 * OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_EVENT_CLEAR_MASK,
 * OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK,
 * OTG0_OTG_GLOBAL_SYNC_STATUS__VREADY_EVENT_CLEAR_MASK).
 */
#define FORMAT_FRAME_INTERRUPT_ON	0x00000001U
#define FORMAT_FRAME_ACKNOWLEDGE	0x00000010U
#define FORMAT_SYNC_ACKNOWLEDGES	0x00410410U

/*
 * The frame count of the timing generator: 24 bits.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK).
 */
#define FORMAT_FRAME_COUNT_FIELD	0x00ffffffU

/*
 * A hot-plug line's interrupt control: bit 0 acknowledges a plug or
 * unplug, bit 8 chooses the level that interrupts (set: a connection,
 * clear: a disconnection), bit 16 lets the line interrupt, bit 20
 * acknowledges a short pulse; and its status, whose bit 4 is the line's
 * debounced level.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HPD0_DC_HPD_INT_CONTROL__DC_HPD_INT_ACK_MASK,
 * HPD0_DC_HPD_INT_CONTROL__DC_HPD_INT_POLARITY_MASK,
 * HPD0_DC_HPD_INT_CONTROL__DC_HPD_INT_EN_MASK,
 * HPD0_DC_HPD_INT_CONTROL__DC_HPD_RX_INT_ACK_MASK,
 * HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED_MASK).
 */
#define FORMAT_HOTPLUG_ACKNOWLEDGE	0x00000001U
#define FORMAT_HOTPLUG_ON_CONNECT	0x00000100U
#define FORMAT_HOTPLUG_INTERRUPT_ON	0x00010000U
#define FORMAT_HOTPLUG_SHORT_ACKNOWLEDGE 0x00100000U
#define FORMAT_HOTPLUG_LEVEL		0x00000010U

/*
 * The plane's cursor control: bit 0 shows the cursor, bit 4 doubles its
 * size, bits 10:8 the colour mode (2: 32-bit colour with premultiplied
 * alpha), bit 12 the protected-content flag, bit 14 fetches the image
 * from system memory instead of VRAM, bits 17:16 the image's pitch (0: 64
 * pixels), bits 28:24 the lines of one fetch (3: eight lines, for an
 * image 33 to 64 pixels wide).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK,
 * CURSOR0_0_CURSOR_CONTROL__CURSOR_2X_MAGNIFY_MASK,
 * CURSOR0_0_CURSOR_CONTROL__CURSOR_MODE_MASK,
 * CURSOR0_0_CURSOR_CONTROL__CURSOR_TMZ_MASK,
 * CURSOR0_0_CURSOR_CONTROL__CURSOR_SYSTEM_MASK,
 * CURSOR0_0_CURSOR_CONTROL__CURSOR_PITCH_MASK,
 * CURSOR0_0_CURSOR_CONTROL__CURSOR_LINES_PER_CHUNK_MASK),
 * drivers/gpu/drm/amd/display/dc/dc_hw_types.h
 * (enum dc_cursor_color_format), drivers/gpu/drm/amd/display/dc/inc/hw/hubp.h
 * (enum cursor_pitch, enum cursor_lines_per_chunk) and
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_get_lines_per_chunk).
 */
#define FORMAT_CURSOR_SHOWN		0x00000001U
#define FORMAT_CURSOR_DOUBLED		0x00000010U
#define FORMAT_CURSOR_MODE_FIELD	0x00000700U
#define FORMAT_CURSOR_MODE_PREMULTIPLIED 0x00000200U
#define FORMAT_CURSOR_PROTECTED		0x00001000U
#define FORMAT_CURSOR_FROM_SYSTEM	0x00004000U
#define FORMAT_CURSOR_PITCH_FIELD	0x00030000U
#define FORMAT_CURSOR_PITCH_64		0x00000000U
#define FORMAT_CURSOR_LINES_FIELD	0x1f000000U
#define FORMAT_CURSOR_LINES_EIGHT	0x03000000U

/*
 * The cursor's size (height in bits 8:0, width in bits 24:16), its
 * position (y in bits 13:0, x in bits 29:16) and its hot spot (y in bits
 * 7:0, x in bits 23:16).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (CURSOR0_0_CURSOR_SIZE__CURSOR_HEIGHT_MASK,
 * CURSOR0_0_CURSOR_SIZE__CURSOR_WIDTH_MASK,
 * CURSOR0_0_CURSOR_POSITION__CURSOR_Y_POSITION_MASK,
 * CURSOR0_0_CURSOR_POSITION__CURSOR_X_POSITION_MASK,
 * CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_Y_MASK,
 * CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_X_MASK).
 */
#define FORMAT_CURSOR_WIDTH_SHIFT	16U
#define FORMAT_CURSOR_X_SHIFT		16U

/*
 * The cursor's request timing: no shift of its fetch schedule (bits 7:0)
 * and its fetch deadline moved by three (bits 9:8), as the display
 * engine's own driver programs it.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBPREQ0_CURSOR_SETTINGS__CURSOR0_DST_Y_OFFSET_MASK,
 * HUBPREQ0_CURSOR_SETTINGS__CURSOR0_CHUNK_HDL_ADJUST_MASK) and
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_cursor_set_attributes).
 */
#define FORMAT_CURSOR_TIMING		0x00000300U

/*
 * The pixel processor's cursor control: bit 0 shows the cursor, bit 1 the
 * expansion of narrow colour, bit 3 the cursor's own degamma, bits 6:4 the
 * colour mode (the same numbers as the plane's).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK,
 * CNVC_CUR0_CURSOR0_CONTROL__CUR0_EXPANSION_MODE_MASK,
 * CNVC_CUR0_CURSOR0_CONTROL__CUR0_ROM_EN_MASK,
 * CNVC_CUR0_CURSOR0_CONTROL__CUR0_MODE_MASK) and
 * drivers/gpu/drm/amd/display/dc/dpp/dcn20/dcn20_dpp.c
 * (dpp2_set_cursor_attributes).
 */
#define FORMAT_PIXEL_CURSOR_SHOWN	0x00000001U
#define FORMAT_PIXEL_CURSOR_EXPANSION	0x00000002U
#define FORMAT_PIXEL_CURSOR_DEGAMMA	0x00000008U
#define FORMAT_PIXEL_CURSOR_MODE_FIELD	0x00000070U
#define FORMAT_PIXEL_CURSOR_PREMULTIPLIED 0x00000020U

/* The microseconds of one second, and the millihertz of one hertz. */
#define FORMAT_MICROSECONDS		1000000ULL
#define FORMAT_MILLIHERTZ		1000ULL

/*
 * Works out where the cursor lies against the plane from the position of
 * its image's top left corner.
 *
 * The cursor takes a position that is never negative and a hot spot it
 * draws the image's corner that far up and left of the position: a corner
 * left of or above the plane is put at zero with the overhang as the hot
 * spot.  An image wholly outside the plane is not shown.
 */
void
amdrdna_cursor_locate(
	int32_t x,
	int32_t y,
	uint32_t side,
	uint32_t plane_width,
	uint32_t plane_height,
	struct amdrdna_cursor_place *place)
{
	uint32_t position_x;
	uint32_t position_y;
	uint32_t hot_x;
	uint32_t hot_y;

	/* Shown unless the image lies wholly past an edge of the plane. */
	place->shown = true;
	if ((int64_t)x >= (int64_t)plane_width) {
		/* Past the right edge. */
		place->shown = false;
	} else if ((int64_t)x + (int64_t)side <= 0) {
		/* Past the left edge. */
		place->shown = false;
	} else if ((int64_t)y >= (int64_t)plane_height) {
		/* Below the bottom edge. */
		place->shown = false;
	} else if ((int64_t)y + (int64_t)side <= 0) {
		/* Above the top edge. */
		place->shown = false;
	}

	/* A hidden cursor keeps the plane's origin. */
	if (!place->shown) {
		place->position = 0U;
		place->hot_spot = 0U;
		return;
	}

	/* A corner left of the plane is drawn from x zero with the overhang as the hot spot. */
	if (x < 0) {
		position_x = 0U;
		hot_x = (uint32_t)(-(int64_t)x);
	} else {
		position_x = (uint32_t)x;
		hot_x = 0U;
	}

	/* The same above the plane. */
	if (y < 0) {
		position_y = 0U;
		hot_y = (uint32_t)(-(int64_t)y);
	} else {
		position_y = (uint32_t)y;
		hot_y = 0U;
	}

	/* The two words the cursor takes. */
	place->position = (position_x << FORMAT_CURSOR_X_SHIFT) | position_y;
	place->hot_spot = (hot_x << FORMAT_CURSOR_X_SHIFT) | hot_y;
}

/*
 * Gives the plane's cursor control word for the driver's image: 64 pixels
 * wide in VRAM, 32-bit colour with premultiplied alpha, shown or not; the
 * fields the driver does not use keep what the caller read.
 */
uint32_t
amdrdna_cursor_control_word(
	uint32_t current,
	bool shown)
{
	uint32_t word;

	/* Clears every field the driver sets. */
	word = current;
	word &= ~(FORMAT_CURSOR_SHOWN |
	    FORMAT_CURSOR_DOUBLED |
	    FORMAT_CURSOR_MODE_FIELD |
	    FORMAT_CURSOR_PROTECTED |
	    FORMAT_CURSOR_FROM_SYSTEM |
	    FORMAT_CURSOR_PITCH_FIELD |
	    FORMAT_CURSOR_LINES_FIELD);

	/* The image's colour mode, pitch and fetch. */
	word |= FORMAT_CURSOR_MODE_PREMULTIPLIED;
	word |= FORMAT_CURSOR_PITCH_64;
	word |= FORMAT_CURSOR_LINES_EIGHT;

	/* Shown or hidden. */
	if (shown)
		word |= FORMAT_CURSOR_SHOWN;

	/* Succeeded: the word to write. */
	return word;
}

/* Gives the cursor's size word for a square image of side pixels. */
uint32_t
amdrdna_cursor_size_word(
	uint32_t side)
{
	/* The width and the height. */
	return (side << FORMAT_CURSOR_WIDTH_SHIFT) | side;
}

/* Gives the cursor's request timing word. */
uint32_t
amdrdna_cursor_timing_word(void)
{
	/* No shift of the schedule, the deadline moved by three. */
	return FORMAT_CURSOR_TIMING;
}

/*
 * Gives the pixel processor's cursor control word: 32-bit colour with
 * premultiplied alpha, no expansion and no degamma, shown or not; the
 * fields the driver does not use keep what the caller read.
 */
uint32_t
amdrdna_pixel_cursor_word(
	uint32_t current,
	bool shown)
{
	uint32_t word;

	/* Clears every field the driver sets. */
	word = current;
	word &= ~(FORMAT_PIXEL_CURSOR_SHOWN |
	    FORMAT_PIXEL_CURSOR_EXPANSION |
	    FORMAT_PIXEL_CURSOR_DEGAMMA |
	    FORMAT_PIXEL_CURSOR_MODE_FIELD);

	/* The colour mode. */
	word |= FORMAT_PIXEL_CURSOR_PREMULTIPLIED;

	/* Shown or hidden. */
	if (shown)
		word |= FORMAT_PIXEL_CURSOR_SHOWN;

	/* Succeeded: the word to write. */
	return word;
}

/* Gives the frames the 24-bit frame count moved by between two readings. */
uint32_t
amdrdna_frame_advance(
	uint32_t before,
	uint32_t after)
{
	/* The difference, carried round the count's 24 bits. */
	return (after - before) & FORMAT_FRAME_COUNT_FIELD;
}

/* Gives the refresh in millihertz of some frames counted over some microseconds; 0 for no time. */
uint32_t
amdrdna_refresh_millihertz(
	uint32_t frames,
	uint64_t microseconds)
{
	uint64_t millihertz;

	/* No time measured gives no refresh. */
	if (microseconds == 0U)
		return 0U;

	/* The frames per second, in thousandths, rounded to the nearest. */
	millihertz = ((uint64_t)frames * FORMAT_MICROSECONDS * FORMAT_MILLIHERTZ + microseconds / 2U) / microseconds;

	/* Succeeded: the refresh. */
	return (uint32_t)millihertz;
}

/*
 * Gives the timing generator's sync status word with its frame interrupt
 * on or off, acknowledging a frame interrupt when asked; the other events'
 * acknowledgements are never written.
 */
uint32_t
amdrdna_sync_status_word(
	uint32_t current,
	bool frame_interrupt,
	bool acknowledge)
{
	uint32_t word;

	/* The word as read, without any acknowledgement and without the frame interrupt. */
	word = current;
	word &= ~FORMAT_SYNC_ACKNOWLEDGES;
	word &= ~FORMAT_FRAME_INTERRUPT_ON;

	/* The frame interrupt, when asked for. */
	if (frame_interrupt)
		word |= FORMAT_FRAME_INTERRUPT_ON;

	/* The acknowledgement of the frame interrupt, when asked for. */
	if (acknowledge)
		word |= FORMAT_FRAME_ACKNOWLEDGE;

	/* Succeeded: the word to write. */
	return word;
}

/*
 * Gives the plane's flip interrupt word with the interrupt on or off,
 * acknowledging a finished flip when asked; a flip away is never
 * acknowledged here.
 */
uint32_t
amdrdna_flip_interrupt_word(
	uint32_t current,
	bool enabled,
	bool acknowledge)
{
	uint32_t word;

	/* The word as read, without any acknowledgement and without the interrupt. */
	word = current;
	word &= ~(FORMAT_FLIP_ACKNOWLEDGE | FORMAT_FLIP_AWAY_ACKNOWLEDGE);
	word &= ~FORMAT_FLIP_INTERRUPT_ON;

	/* The interrupt, when asked for. */
	if (enabled)
		word |= FORMAT_FLIP_INTERRUPT_ON;

	/* The acknowledgement, when asked for. */
	if (acknowledge)
		word |= FORMAT_FLIP_ACKNOWLEDGE;

	/* Succeeded: the word to write. */
	return word;
}

/*
 * Gives a hot-plug line's interrupt control word: the interrupt on or off,
 * waiting for the change away from the line's level (a disconnection
 * while connected, a connection while not), acknowledging a plug or a
 * short pulse when asked.
 */
uint32_t
amdrdna_hotplug_control_word(
	uint32_t current,
	bool enabled,
	bool connected,
	bool acknowledge,
	bool acknowledge_short)
{
	uint32_t word;

	/* The word as read, without the acknowledgements, the level and the interrupt. */
	word = current;
	word &= ~(FORMAT_HOTPLUG_ACKNOWLEDGE | FORMAT_HOTPLUG_SHORT_ACKNOWLEDGE);
	word &= ~(FORMAT_HOTPLUG_ON_CONNECT | FORMAT_HOTPLUG_INTERRUPT_ON);

	/* The interrupt, when asked for. */
	if (enabled)
		word |= FORMAT_HOTPLUG_INTERRUPT_ON;

	/* A line without a monitor waits for a connection; one with a monitor for its loss. */
	if (!connected)
		word |= FORMAT_HOTPLUG_ON_CONNECT;

	/* The acknowledgement of a plug or an unplug, when asked for. */
	if (acknowledge)
		word |= FORMAT_HOTPLUG_ACKNOWLEDGE;

	/* The acknowledgement of a short pulse, when asked for. */
	if (acknowledge_short)
		word |= FORMAT_HOTPLUG_SHORT_ACKNOWLEDGE;

	/* Succeeded: the word to write. */
	return word;
}

/* Reports whether a hot-plug line's status says a monitor is connected (its debounced level). */
bool
amdrdna_hotplug_connected(
	uint32_t status)
{
	/* The debounced level is high while a monitor is connected. */
	if ((status & FORMAT_HOTPLUG_LEVEL) != 0U)
		return true;

	/* Low: no monitor. */
	return false;
}

/*
 * Gives the plane's flip control word that makes a new address take
 * effect at the next frame's update; the other fields keep what the caller
 * read.
 */
uint32_t
amdrdna_flip_control_word(
	uint32_t current)
{
	/* The update of the next frame, never at once (no tearing). */
	return current & ~FORMAT_FLIP_AT_ONCE;
}

/* Reports whether the plane's flip control says a flip still waits for its frame's update. */
bool
amdrdna_flip_pending(
	uint32_t flip_control)
{
	/* The waiting bit. */
	if ((flip_control & FORMAT_FLIP_WAITING) != 0U)
		return true;

	/* No flip waits. */
	return false;
}
