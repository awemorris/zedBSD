/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display modes the VisionFive 2's HDMI output is set to without the
 * loader, and the register values each one takes.
 *
 * Only values: nothing here touches a register, so the host's tests run
 * the same code as the kernel.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_DC8200_MODE_H
#define KERN_DRIVERS_GPU_POWERVR_DC8200_MODE_H

#include <stdint.h>

/* The most register writes one HDMI step takes. */
#define POWERVR_DC_HDMI_WRITES_MAX	24U

/*
 * One display mode: the visible size, the refresh rate, the pixel clock,
 * and where the syncs fall and how long each line and frame is, in
 * pixels and lines counted from the start of the visible area.
 *
 * hsync_positive and vsync_positive are nonzero for a sync pulse that is
 * high.  vic is the CEA-861 video code, zero for a VESA mode, which picks
 * the HDMI transmitter's drive settings.
 */
struct powervr_dc_mode {
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	uint32_t clock_khz;
	uint32_t hsync_start;
	uint32_t hsync_end;
	uint32_t htotal;
	uint32_t vsync_start;
	uint32_t vsync_end;
	uint32_t vtotal;
	uint32_t hsync_positive;
	uint32_t vsync_positive;
	uint32_t vic;
};

/*
 * The HDMI transmitter's two PLL settings for one pixel clock: the
 * pre-PLL, which makes the pixel and TMDS clocks, and the post-PLL,
 * which drives the serializer.
 */
struct powervr_dc_pll {
	uint32_t clock_khz;
	uint32_t pre_divider;
	uint32_t pre_feedback;
	uint32_t tmds_a;
	uint32_t tmds_b;
	uint32_t tmds_c;
	uint32_t pixel_a;
	uint32_t pixel_b;
	uint32_t pixel_c;
	uint32_t pixel_d;
	uint32_t fraction;
	uint32_t post_divider_in;
	uint32_t post_feedback;
	uint32_t post_divider_out;
	uint32_t post_divider_enable;
};

/* One write of the HDMI transmitter's byte registers, by register number. */
struct powervr_dc_hdmi_write {
	uint32_t reg;
	uint32_t value;
};

/* The four timing words of a display controller panel. */
struct powervr_dc_panel_timing {
	uint32_t horizontal;
	uint32_t horizontal_sync;
	uint32_t vertical;
	uint32_t vertical_sync;
};

const struct powervr_dc_mode *powervr_dc_mode_default(void);
int powervr_dc_mode_find(uint32_t width, uint32_t height, uint32_t refresh, const struct powervr_dc_mode **mode);
int powervr_dc_mode_from_line(const char *line, uint32_t *width, uint32_t *height, uint32_t *refresh);
int powervr_dc_pll_find(uint32_t clock_khz, struct powervr_dc_pll *pll);
void powervr_dc_panel_timing(const struct powervr_dc_mode *mode, struct powervr_dc_panel_timing *timing);
unsigned powervr_dc_hdmi_pll_writes(const struct powervr_dc_pll *pll, struct powervr_dc_hdmi_write *writes);
unsigned powervr_dc_hdmi_timing_writes(const struct powervr_dc_mode *mode, struct powervr_dc_hdmi_write *writes);
unsigned powervr_dc_hdmi_drive_writes(const struct powervr_dc_mode *mode, struct powervr_dc_hdmi_write *writes);

#endif
