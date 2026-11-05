/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private, bounded hardware program for the initial BCM2711 RGB scanout. */
#ifndef BCM2711_DISPLAY_PROGRAM_H
#define BCM2711_DISPLAY_PROGRAM_H

#include <stdbool.h>
#include <stdint.h>

/* Register-window identities; encoder windows have the same relative order. */
enum bcm2711_display_region {
	BCM2711_REGION_HVS,
	BCM2711_REGION_PV0,
	BCM2711_REGION_PV1,
	BCM2711_REGION_HDMI0,
	BCM2711_REGION_HDMI1 = 11,
	BCM2711_REGION_GLUE = 19,
	BCM2711_REGION_COUNT
};

/* Window roles within one encoder, resolved by reg-names rather than reg order. */
enum bcm2711_encoder_region {
	BCM2711_ENCODER_CORE,
	BCM2711_ENCODER_DVP,
	BCM2711_ENCODER_PHY,
	BCM2711_ENCODER_RATE,
	BCM2711_ENCODER_PACKET,
	BCM2711_ENCODER_COLOR,
	BCM2711_ENCODER_SHARED,
	BCM2711_ENCODER_CEC,
	BCM2711_ENCODER_REGIONS
};

/* Operations executed serially; a refused operation prevents later writes. */
enum bcm2711_display_operation {
	BCM2711_DISPLAY_WRITE,
	BCM2711_DISPLAY_UPDATE,
	BCM2711_DISPLAY_WAIT,
	BCM2711_DISPLAY_DELAY,
	BCM2711_DISPLAY_CLOCK_ON,
	BCM2711_DISPLAY_CLOCK_RATE,
	BCM2711_DISPLAY_NOTIFY,
	BCM2711_DISPLAY_CLEAN,
	BCM2711_DISPLAY_CAPTURE,
	BCM2711_DISPLAY_REPLAY,
	BCM2711_DISPLAY_IRQ_OPEN,
	BCM2711_DISPLAY_FRAME_ARM,
	BCM2711_DISPLAY_FRAME_WAIT
};

/* A command owns no storage; mask describes replacement bits or a wait test. */
struct bcm2711_display_command {
	enum bcm2711_display_operation operation;
	uint32_t region;
	uint32_t offset;
	uint32_t mask;
	uint32_t value;
	uint32_t limit_us;
};

/* Enough commands for both encoder binds, one RGB mode and bounded completion. */
#define BCM2711_DISPLAY_COMMANDS 256U

/* Initial unscaled primary list: eight plane words followed by its end marker. */
#define BCM2711_PRIMARY_WORDS 9U

/*
 * The mode captured before firmware relinquishes the hardware.
 * Only progressive, unrepeated RGB8 below the SCDC threshold is accepted.
 * avi contains the validated firmware AVI packet, including header/checksum,
 * so its unchanged mode, range and aspect metadata stay coherent.
 */
struct bcm2711_display_mode {
	uint32_t width;
	uint32_t height;
	uint32_t hfront;
	uint32_t hsync;
	uint32_t hback;
	uint32_t vfront;
	uint32_t vsync;
	uint32_t vback;
	uint32_t tmds_hz;
	bool hpositive;
	bool vpositive;
	bool hdmi;
	bool limited;
	uint8_t avi[17];
};

/*
 * Caller-owned, immutable commands from successful preparation to completion.
 * count is zero on refusal; failure identifies the first rejected operation.
 * Preparation and execution are exclusive boot operations, never concurrent.
 */
struct bcm2711_display_program {
	struct bcm2711_display_command commands[BCM2711_DISPLAY_COMMANDS];
	uint32_t count;
	int error;
	uint32_t failure;
};

struct drv_bcm2711_boot_screen;
struct bcm2711_window;
struct bcm2711_display;

/* Prepares all register values before any destructive operation is executed. */
int bcm2711_display_program_prepare(struct bcm2711_display_program *program, const struct bcm2711_display_mode *mode, const struct drv_bcm2711_boot_screen *screen, uint32_t port, uint32_t order, uint32_t max_core_hz);

/* Executes the validated program once; mappings and framebuffer remain caller-owned. */
int bcm2711_display_program_execute(struct bcm2711_display_program *program, const struct bcm2711_window *windows, const struct drv_bcm2711_boot_screen *screen, struct bcm2711_display *display);

#endif
