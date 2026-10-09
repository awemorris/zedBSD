/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Generates a hardware program without touching hardware or copying a live
 * display list. The program separates input validation from destructive
 * takeover and makes the exact register/clock order inspectable on a host.
 * Register fields and PHY tuning values describe the BCM2711 hardware.
 */

#include <stdbool.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/display-program.h"

/* SRAM words retained for boot firmware and the permanent eleven-word filter. */
#define PROGRAM_FILTER_START 32U
#define PROGRAM_PRIMARY_START 43U

/* Signed nine-bit samples of the symmetric B=C=1/3 reconstruction filter. */
static const int16_t program_filter_samples[18] = {
    0, -2, -6, -8, -10, -8, -3, 2, 18,
    50, 82, 119, 155, 187, 213, 227, 227, 0};

static void append(struct bcm2711_display_program *program, enum bcm2711_display_operation operation, uint32_t region, uint32_t offset, uint32_t mask, uint32_t value, uint32_t limit_us);
static void write_word(struct bcm2711_display_program *program, uint32_t region, uint32_t offset, uint32_t value);
static void change_word(struct bcm2711_display_program *program, uint32_t region, uint32_t offset, uint32_t mask, uint32_t value);
static void wait_word(struct bcm2711_display_program *program, uint32_t region, uint32_t offset, uint32_t mask, uint32_t value, uint32_t limit_us);
static void clock_rate(struct bcm2711_display_program *program, uint32_t id, uint32_t hz);
static void encoder_bind(struct bcm2711_display_program *program, uint32_t port);
static void phy_program(struct bcm2711_display_program *program, uint32_t port, uint32_t hz);
static void encoder_timing(struct bcm2711_display_program *program, const struct bcm2711_display_mode *mode, uint32_t base);
static void packet_program(struct bcm2711_display_program *program, uint32_t base, const uint8_t *bytes, uint32_t length, uint32_t slot);
static int validate_mode(const struct bcm2711_display_mode *mode, const struct drv_bcm2711_boot_screen *screen, uint32_t port, uint32_t order, uint32_t max_core_hz);

/*
 * Prepares the initial RGB8 scanout on the unique boot output.
 *
 * The caller owns the framebuffer through scanout and supplies the captured
 * mode before the display-done notification. No hardware is touched here.
 * Refusals publish count=0, preventing execution of any partial program.
 */
int
bcm2711_display_program_prepare(
	struct bcm2711_display_program *program,
	const struct bcm2711_display_mode *mode,
	const struct drv_bcm2711_boot_screen *screen,
	uint32_t port,
	uint32_t order,
	uint32_t max_core_hz)
{
	uint32_t filter[6];
	uint32_t primary[BCM2711_PRIMARY_WORDS];
	uint32_t base;
	uint32_t timing;
	uint32_t index;
	uint32_t sample;
	uint32_t core;
	uint32_t channel_load;
	uint32_t horizontal_total;
	uint32_t fifo;
	uint32_t value;
	uint32_t gain;
	uint32_t offset;
	int error;

	/* Starts with no executable program. */
	program->count = 0;
	program->error = 0;
	program->failure = 0;

	/* Rejects unsupported or inconsistent modes before composing commands. */
	error = validate_mode(mode, screen, port, order, max_core_hz);
	if (error != 0)
		return error;

	/* Resolves the already selected output and reserves channel zero. */
	base = BCM2711_REGION_HDMI0 + port * BCM2711_ENCODER_REGIONS;
	timing = BCM2711_REGION_PV0 + port;
	core = 500000000U;
	if (max_core_hz < core)
		core = max_core_hz;

	/* Retires firmware before altering the HVS output routing. */
	append(program, BCM2711_DISPLAY_NOTIFY, 0, 0, 0, 0, 0);
	append(program, BCM2711_DISPLAY_CLOCK_ON, 4, 0, 0, 0, 0);

	/* Enables the HVS and disconnects outputs before changing shared buffers. */
	change_word(program, BCM2711_REGION_HVS, 0x00, 0, 0x80000000U);
	change_word(program, BCM2711_REGION_HVS, 0x0c, 0x80000000U, 0);
	change_word(program, BCM2711_REGION_HVS, 0x00, 0x000c0000U, 0x000c0000U);
	change_word(program, BCM2711_REGION_HVS, 0x18, 0xc0000000U, 0xc0000000U);
	change_word(program, BCM2711_REGION_HVS, 0x14, 0xc0000000U, 0xc0000000U);
	change_word(program, BCM2711_REGION_HVS, 0x00, 0x3f03bbb1U, 0x2a00000eU);

	/* Packs signed filter coefficients, preserving the symmetric SRAM layout. */
	for (index = 0; index < 6U; index++) {
		/* Each hardware word contains three signed nine-bit samples. */
		filter[index] = 0;
		for (sample = 0; sample < 3U; sample++) {
			/* Truncates only the documented coefficient field. */
			value = (uint32_t)program_filter_samples[index * 3U + sample] & 0x1ffU;
			filter[index] |= value << (sample * 9U);
		}
	}

	/* Writes the permanent symmetric reconstruction kernel before COB allocation. */
	for (index = 0; index < 11U; index++) {
		/* The second half reverses whole coefficient words. */
		sample = index;
		if (sample >= 6U)
			sample = 10U - index;
		write_word(program, BCM2711_REGION_HVS, 0x4000U + (PROGRAM_FILTER_START + index) * 4U, filter[sample]);
	}

	/* Partitions the compositor output buffers before encoder initialization. */
	write_word(program, BCM2711_REGION_HVS, 0x6c, 12288U << 16);
	write_word(program, BCM2711_REGION_HVS, 0x5c, (24592U << 16) | 12304U);
	write_word(program, BCM2711_REGION_HVS, 0x4c, (44416U << 16) | 24608U);

	/* Opens the serviced HVS IRQ only after its shared buffer initialization. */
	append(program, BCM2711_DISPLAY_IRQ_OPEN, BCM2711_REGION_HVS, 0, 0, 0, 0);

	/* Binds both encoder blocks, without enabling either video stream. */
	encoder_bind(program, 0);
	encoder_bind(program, 1);

	/* Quiets and clears pixelvalve IRQ sources before the first commit. */
	for (index = 0; index < BCM2711_TIMING_COUNT; index++) {
		/* Quiets the source before enabling its serviced GIC line. */
		write_word(program, BCM2711_REGION_PV0 + index, 0x24, 0);
		write_word(program, BCM2711_REGION_PV0 + index, 0x28, 0x80);
		append(program, BCM2711_DISPLAY_IRQ_OPEN, BCM2711_REGION_PV0 + index, 0, 0, 0, 0);
	}

	/* Raises the shared core rate before muxing and plane programming. */
	clock_rate(program, 4, core);
	change_word(program, BCM2711_REGION_HVS, 0x80, 0x03000000U, 0);
	offset = 0x18;
	if (port == 1U)
		offset = 0x14;
	change_word(program, BCM2711_REGION_HVS, offset, 0xc0000000U, 0);

	/* Builds a new primary plane instead of relocating firmware context words. */
	primary[0] = 0x48009807U | (order << 13);
	primary[1] = 0;
	primary[2] = 0x4000fff0U;
	primary[3] = (mode->height << 16) | mode->width;
	primary[4] = 0xc0c0c0c0U;
	primary[5] = 0xc0000000U | (uint32_t)screen->physical;
	primary[6] = 0xc0c0c0c0U;
	primary[7] = screen->pitch;
	primary[8] = 0x80000000U;
	append(program, BCM2711_DISPLAY_CLEAN, 0, 0, 0, 0, 0);
	for (index = 0; index < BCM2711_PRIMARY_WORDS; index++) {
		/* Uses one ordered 32-bit MMIO write per SRAM word. */
		write_word(program, BCM2711_REGION_HVS, 0x4000U + (PROGRAM_PRIMARY_START + index) * 4U, primary[index]);
	}

	/* Enables background fill after the complete primary list is in SRAM. */
	change_word(program, BCM2711_REGION_HVS, 0x44, 0, 0x01000000U);

	/* Arms vblank before installing the list and enabling its HVS channel. */
	write_word(program, timing, 0x24, 0x80);
	write_word(program, BCM2711_REGION_HVS, 0x20, PROGRAM_PRIMARY_START);
	write_word(program, BCM2711_REGION_HVS, 0x40, 0);
	write_word(program, BCM2711_REGION_HVS, 0x40, 0x40000000U);
	write_word(program, BCM2711_REGION_HVS, 0x40, 0);
	write_word(program, BCM2711_REGION_HVS, 0x40, 0x80000000U | (mode->width << 16) | mode->height);
	change_word(program, BCM2711_REGION_HVS, 0x44, 0xe0000000U, 0);

	/* Configures clocks and PHY before writing the encoder or PV timings. */
	value = (mode->tmds_hz / 100U) * 101U;
	if (value < 120000000U)
		value = 120000000U;
	clock_rate(program, 13, value);
	value = 75000000U;
	if (mode->tmds_hz > 148500000U)
		value = 150000000U;
	if (mode->tmds_hz > 297000000U)
		value = 300000000U;
	clock_rate(program, 14, value);
	append(program, BCM2711_DISPLAY_CLOCK_ON, 14, 0, 0, 0, 0);
	phy_program(program, port, mode->tmds_hz);
	change_word(program, base + BCM2711_ENCODER_CORE, 0xe0, 0, 0x8020);
	encoder_timing(program, mode, base);

	/* Resets the PV only after the HDMI clocks, PHY and timing are ready. */
	change_word(program, timing, 0x00, 1, 0);
	change_word(program, timing, 0x00, 0, 2);
	write_word(program, timing, 0x0c, ((mode->hback / 2U) << 16) | (mode->hsync / 2U));
	write_word(program, timing, 0x10, ((mode->hfront / 2U) << 16) | (mode->width / 2U));
	write_word(program, timing, 0x04, 2);
	write_word(program, timing, 0x08, 0);
	write_word(program, timing, 0x14, (mode->vback << 16) | mode->vsync);
	write_word(program, timing, 0x18, (mode->vfront << 16) | mode->height);
	write_word(program, timing, 0x34, 0x20);
	fifo = 238U;
	if (port == 1U)
		fifo = 32U;
	value = ((fifo & 0x3fU) << 15) | ((fifo >> 6) << 25) | 0x7002U;
	write_word(program, timing, 0x00, value);
	change_word(program, timing, 0x00, 0, 1);

	/* Establishes RGB conversion and the HDMI FIFO before opening pixel flow. */
	gain = 0x2000U;
	offset = 0;
	if (mode->limited) {
		/* Limited RGB maps the full input range onto code values 16 through 235. */
		gain = 0x1b80U;
		offset = 0x400U;
	}

	/* Publishes the RGB matrix before selecting its input channel mapping. */
	write_word(program, base + BCM2711_ENCODER_COLOR, 0x04, gain);
	write_word(program, base + BCM2711_ENCODER_COLOR, 0x08, offset << 16);
	write_word(program, base + BCM2711_ENCODER_COLOR, 0x0c, gain << 16);
	write_word(program, base + BCM2711_ENCODER_COLOR, 0x10, offset << 16);
	write_word(program, base + BCM2711_ENCODER_COLOR, 0x14, 0);
	write_word(program, base + BCM2711_ENCODER_COLOR, 0x18, (offset << 16) | gain);
	write_word(program, base + BCM2711_ENCODER_DVP, 0xec, 0);
	write_word(program, base + BCM2711_ENCODER_DVP, 0xf0, 0x354021U);
	write_word(program, base + BCM2711_ENCODER_COLOR, 0x2c, 0);
	write_word(program, base + BCM2711_ENCODER_COLOR, 0x00, 7);
	write_word(program, base + BCM2711_ENCODER_CORE, 0x74, 1);
	append(program, BCM2711_DISPLAY_FRAME_ARM, timing, 0, 0, 0, 0);
	change_word(program, timing, 0x04, 0, 1);

	/* Opens HDMI video after VIDEN, keeping output polarity consistent. */
	value = 0xe0810000U;
	if (!mode->hpositive)
		value |= 0x08000000U;
	if (!mode->vpositive)
		value |= 0x10000000U;
	change_word(program, base + BCM2711_ENCODER_SHARED, 0x44U + port * 4U, 0x18000000U, value);
	change_word(program, base + BCM2711_ENCODER_SHARED, 0x44U + port * 4U, 0x00040000U, 0);

	/* Reestablishes the captured sink protocol before packet transmission. */
	if (mode->hdmi) {
		/* Starts HDMI scheduling, then publishes the validated AVI packet. */
		change_word(program, base + BCM2711_ENCODER_CORE, 0xe0, 0, 1);
		wait_word(program, base + BCM2711_ENCODER_CORE, 0xe0, 2, 2, 1000000U);
		write_word(program, base + BCM2711_ENCODER_CORE, 0xbc, 0x10000U);
		packet_program(program, base, mode->avi, 17U, 2U);
	} else {
		/* DVI has no HDMI packet stream. */
		change_word(program, base + BCM2711_ENCODER_CORE, 0xbc, 0x10000U, 0);
		change_word(program, base + BCM2711_ENCODER_CORE, 0xe0, 1, 0);
		wait_word(program, base + BCM2711_ENCODER_CORE, 0xe0, 2, 0, 1000000U);
	}

	/* Recenters twice using one captured writable value and the required gap. */
	append(program, BCM2711_DISPLAY_CAPTURE, base + BCM2711_ENCODER_CORE, 0x74, 0xefff, 0, 0);
	append(program, BCM2711_DISPLAY_REPLAY, base + BCM2711_ENCODER_CORE, 0x74, 0x40, 0, 0);
	append(program, BCM2711_DISPLAY_REPLAY, base + BCM2711_ENCODER_CORE, 0x74, 0, 0x40, 0);
	append(program, BCM2711_DISPLAY_DELAY, 0, 0, 0, 0, 1000);
	append(program, BCM2711_DISPLAY_REPLAY, base + BCM2711_ENCODER_CORE, 0x74, 0x40, 0, 0);
	append(program, BCM2711_DISPLAY_REPLAY, base + BCM2711_ENCODER_CORE, 0x74, 0, 0x40, 0);
	wait_word(program, base + BCM2711_ENCODER_CORE, 0x74, 0x4000, 0x4000, 1000);

	/* Requires the vblank handler to observe the new mode's adopted display list. */
	append(program, BCM2711_DISPLAY_FRAME_WAIT, timing, 0, 0, 0, 100000U);
	wait_word(program, BCM2711_REGION_HVS, 0x30, 0xffffffffU, PROGRAM_PRIMARY_START, 100000);

	/* Computes the single-output sustained core request in kilohertz units. */
	horizontal_total = mode->width + mode->hfront + mode->hsync + mode->hback;
	channel_load = mode->tmds_hz / 1000U;
	if (port == 0U) {
		/* HDMI0 applies the extra COB bandwidth margin. */
		value = channel_load * mode->width / horizontal_total + 8000U;
		channel_load = channel_load * 9U / 10U;
		if (channel_load < value)
			channel_load = value;
	}

	/* Converts the selected channel requirement back to a firmware clock rate. */
	core = channel_load * 1000U;
	if (core > max_core_hz)
		core = max_core_hz;
	clock_rate(program, 4, core);

	/* Refuses to publish a truncated program if its fixed capacity was exceeded. */
	if (program->error != 0) {
		program->count = 0;
		return program->error;
	}

	/* Succeeded: the complete ordered program can be executed once. */
	return 0;
}

/* Appends one bounded command, retaining overflow until final validation. */
static void
append(
	struct bcm2711_display_program *program,
	enum bcm2711_display_operation operation,
	uint32_t region,
	uint32_t offset,
	uint32_t mask,
	uint32_t value,
	uint32_t limit_us)
{
	struct bcm2711_display_command *command;

	/* An overflowed program cannot acquire additional commands. */
	if (program->count >= BCM2711_DISPLAY_COMMANDS) {
		program->error = EOVERFLOW;
		return;
	}

	/* Initializes every command field before increasing the published count. */
	command = &program->commands[program->count];
	command->operation = operation;
	command->region = region;
	command->offset = offset;
	command->mask = mask;
	command->value = value;
	command->limit_us = limit_us;
	program->count++;
}

/* Appends an exact 32-bit register write. */
static void
write_word(
	struct bcm2711_display_program *program,
	uint32_t region,
	uint32_t offset,
	uint32_t value)
{
	/* Replaces the complete register value. */
	append(program, BCM2711_DISPLAY_WRITE, region, offset, 0, value, 0);
}

/* Appends a read/modify/write with explicit replacement fields. */
static void
change_word(
	struct bcm2711_display_program *program,
	uint32_t region,
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	/* Bits outside mask are retained, then value bits are asserted. */
	append(program, BCM2711_DISPLAY_UPDATE, region, offset, mask, value, 0);
}

/* Appends a bounded register wait. */
static void
wait_word(
	struct bcm2711_display_program *program,
	uint32_t region,
	uint32_t offset,
	uint32_t mask,
	uint32_t value,
	uint32_t limit_us)
{
	/* Execution fails if the requested state never appears. */
	append(program, BCM2711_DISPLAY_WAIT, region, offset, mask, value, limit_us);
}

/* Appends a firmware clock request. */
static void
clock_rate(
	struct bcm2711_display_program *program,
	uint32_t id,
	uint32_t hz)
{
	/* Shared-clock aggregation belongs to the single selected-output program. */
	append(program, BCM2711_DISPLAY_CLOCK_RATE, id, 0, 0, hz, 0);
}

/* Prepares one encoder's runtime clocks and effective reset writes. */
static void
encoder_bind(
	struct bcm2711_display_program *program,
	uint32_t port)
{
	uint32_t base;

	/* The HSM rate is checked by execution before the first encoder access. */
	base = BCM2711_REGION_HDMI0 + port * BCM2711_ENCODER_REGIONS;
	append(program, BCM2711_DISPLAY_CLOCK_ON, 13, 0, 0, 0, 0);
	change_word(program, BCM2711_REGION_GLUE, 0x08, 1U << (3U + port), 0);
	write_word(program, base + BCM2711_ENCODER_SHARED, 0, 0);
	change_word(program, base + BCM2711_ENCODER_DVP, 0xbc, 0, 2);
}

/* Prepares the oscillator, lane tuning and PLL release for one encoder. */
static void
phy_program(
	struct bcm2711_display_program *program,
	uint32_t port,
	uint32_t hz)
{
	uint64_t vco;
	uint32_t divider;
	uint32_t selection;
	uint32_t gain;
	uint32_t current;
	uint32_t amplitude;
	uint32_t resistance;
	uint32_t termination;
	uint32_t clock_amplitude;
	uint32_t clock_resistance;
	uint32_t clock_termination;
	uint32_t map[4];
	uint32_t drive;
	uint32_t resist;
	uint32_t term;
	uint32_t swap;
	uint32_t lane;
	uint32_t source;
	uint32_t shift;
	uint32_t base;

	/* Chooses the smallest divider that places the VCO at or above 3 GHz. */
	divider = 1;
	vco = (uint64_t)hz * 10U;
	while (vco * divider < 3000000000ULL)
		divider++;
	vco *= divider;
	selection = 0;
	if (vco > 4500000000ULL)
		selection = 1;
	current = 0x18;
	if (vco < 3700000000ULL)
		current = 0x1c;
	gain = 2;
	if (vco < 5200000000ULL)
		gain = 7;
	if (vco < 4800000000ULL)
		gain = 5;
	if (vco < 4050000000ULL)
		gain = 6;
	if (vco < 3700000000ULL)
		gain = 12;
	if (vco < 3350000000ULL)
		gain = 15;

	/* Chooses hardware drive values for the supported non-SCDC TMDS range. */
	amplitude = 0x09;
	resistance = 0x12;
	termination = 0;
	clock_amplitude = 0x0c;
	clock_resistance = 0x18;
	clock_termination = 3;
	if (hz <= 50000000U) {
		/* Low-rate data and clock lanes use their weakest amplitude. */
		amplitude = 0x0a;
		clock_amplitude = 0x0a;
		clock_termination = 0;
	}

	/* Raises drive for the next hardware tuning band. */
	if (hz > 165000000U) {
		/* The intermediate rate band adds data-lane termination. */
		amplitude = 0x0f;
		termination = 1;
	}

	/* Selects the final supported band without crossing the SCDC boundary. */
	if (hz > 250000000U) {
		/* The highest supported band adds data preemphasis. */
		amplitude = 0x4d;
	}

	/* HDMI1 crosses the physical data/clock lanes differently from HDMI0. */
	map[0] = 0;
	map[1] = 1;
	map[2] = 2;
	map[3] = 3;
	if (port == 1U) {
		/* Encodes the physical lane mapping, not just a video-port number. */
		map[0] = 1;
		map[1] = 0;
		map[2] = 3;
		map[3] = 2;
	}

	/* Packs independent physical-lane fields from the selected mapping. */
	drive = 0;
	resist = 0;
	term = gain << 16;
	swap = 0;
	for (lane = 0; lane < 4U; lane++) {
		/* Drive fields order clock, data0, data1, data2. */
		shift = ((lane + 1U) % 4U) * 8U;
		source = map[lane];
		if (source == 3U) {
			/* Applies clock tuning to the mapped physical clock lane. */
			drive |= clock_amplitude << shift;
			resist |= clock_resistance << (((lane + 1U) % 4U) * 5U);
			term |= clock_termination << (((lane + 1U) % 4U) * 4U);
		} else {
			/* Applies the same data tuning to all mapped data lanes. */
			drive |= amplitude << shift;
			resist |= resistance << (((lane + 1U) % 4U) * 5U);
			term |= termination << (((lane + 1U) % 4U) * 4U);
		}

		/* The output selector follows the physical lane rather than its drive field. */
		swap |= source << (lane * 4U);
	}

	/* Initializes power and rate control before releasing the PLL. */
	base = BCM2711_REGION_HDMI0 + port * BCM2711_ENCODER_REGIONS;
	write_word(program, base + BCM2711_ENCODER_PHY, 0x00, 0x0f);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x04, 0x400);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x04, 0x10);
	change_word(program, base + BCM2711_ENCODER_PHY, 0x00, 0x0f, 0);
	change_word(program, base + BCM2711_ENCODER_RATE, 0x00, 0, 0x000a0010U);
	change_word(program, base + BCM2711_ENCODER_PHY, 0x50, 0x0fffffffU, 0);
	change_word(program, base + BCM2711_ENCODER_PHY, 0x54, 0x0fffffffU, 0);
	write_word(program, base + BCM2711_ENCODER_RATE, 0x18, 0x80000000U | (uint32_t)(((vco * 2U) << 22) / 54000000U >> 2));
	write_word(program, base + BCM2711_ENCODER_PHY, 0x28, divider << 8);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x5c, 0x0e14e147U);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x1c, 0x2060U | (selection << 9));
	change_word(program, base + BCM2711_ENCODER_PHY, 0x20, 0, 0x008a7800U);
	change_word(program, base + BCM2711_ENCODER_RATE, 0x1c, 0, 0x02000000U);
	change_word(program, base + BCM2711_ENCODER_PHY, 0x34, 0, 1);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x44, 0);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x14, 0x000865c0U | current);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x08, drive);
	change_word(program, base + BCM2711_ENCODER_PHY, 0x0c, 0, resist);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x10, term);
	write_word(program, base + BCM2711_ENCODER_PHY, 0x4c, swap);
	change_word(program, base + BCM2711_ENCODER_PHY, 0x00, 0x30, 0);
	change_word(program, base + BCM2711_ENCODER_PHY, 0x00, 0, 0x30);
}

/* Prepares RGB8 progressive HDMI timing and opens its pixel clock gate. */
static void
encoder_timing(
	struct bcm2711_display_program *program,
	const struct bcm2711_display_mode *mode,
	uint32_t base)
{
	uint32_t horizontal;
	uint32_t vertical;
	uint32_t total;

	/* Uses full pixels for HDMI while the later PV uses two pixels per clock. */
	horizontal = (mode->hfront << 16) | mode->width;
	if (mode->hpositive)
		horizontal |= 0x4000;
	if (mode->vpositive)
		horizontal |= 0x8000;
	vertical = (mode->vsync << 24) | (mode->vfront << 16) | mode->height;
	total = mode->width + mode->hfront + mode->hsync + mode->hback;
	write_word(program, base + BCM2711_ENCODER_CORE, 0xe4, horizontal);
	write_word(program, base + BCM2711_ENCODER_CORE, 0xe8, (mode->hback << 16) | mode->hsync);
	write_word(program, base + BCM2711_ENCODER_CORE, 0xec, vertical);
	write_word(program, base + BCM2711_ENCODER_CORE, 0xf4, vertical);
	write_word(program, base + BCM2711_ENCODER_CORE, 0xf0, mode->vback);
	write_word(program, base + BCM2711_ENCODER_CORE, 0xf8, ((total / 2U) << 16) | mode->vback);
	change_word(program, base + BCM2711_ENCODER_CORE, 0x170, 0x70f, 0x200);
	change_word(program, base + BCM2711_ENCODER_CORE, 0x17c, 0xffff, 0x10);
	change_word(program, base + BCM2711_ENCODER_CORE, 0x178, 0, 0x80000000U);
	change_word(program, base + BCM2711_ENCODER_CORE, 0x100, 0x0f, 0);
	write_word(program, base + BCM2711_ENCODER_DVP, 0xbc, 0);
}

/* Packs one infoframe into its entire hardware slot, including zero padding. */
static void
packet_program(
	struct bcm2711_display_program *program,
	uint32_t base,
	const uint8_t *bytes,
	uint32_t length,
	uint32_t slot)
{
	uint32_t index;
	uint32_t word;
	uint32_t value;
	uint32_t shift;
	uint32_t first;

	/* Stops this slot before writing its header or payload. */
	change_word(program, base + BCM2711_ENCODER_CORE, 0xbc, 1U << slot, 0);
	wait_word(program, base + BCM2711_ENCODER_CORE, 0xc4, 1U << slot, 0, 100000);
	first = 0x24U * slot;
	word = 0;
	value = 0;
	for (index = 0; index < 28U; index++) {
		/* Each seven bytes occupy a three-byte word followed by a four-byte word. */
		shift = index % 7U;
		if (shift >= 3U)
			shift -= 3U;
		if (index < length)
			value |= (uint32_t)bytes[index] << (shift * 8U);
		if (index % 7U == 2U || index % 7U == 6U) {
			/* Writes a whole word, including zeros beyond the payload. */
			write_word(program, base + BCM2711_ENCODER_PACKET, first + word * 4U, value);
			word++;
			value = 0;
		}
	}

	/* Clears the entire remaining slot before restarting packet transmission. */
	write_word(program, base + BCM2711_ENCODER_PACKET, first + 32U, 0);
	change_word(program, base + BCM2711_ENCODER_CORE, 0xbc, 0, 1U << slot);
	wait_word(program, base + BCM2711_ENCODER_CORE, 0xc4, 1U << slot, 1U << slot, 100000);
}

/* Rejects any mode that cannot be restarted without inventing sink policy. */
static int
validate_mode(
	const struct bcm2711_display_mode *mode,
	const struct drv_bcm2711_boot_screen *screen,
	uint32_t port,
	uint32_t order,
	uint32_t max_core_hz)
{
	uint32_t checksum;
	uint32_t index;
	uint32_t expected_order;

	/* Requires a real output, a known packed-pixel order and a usable core clock. */
	if (port >= BCM2711_HDMI_COUNT || order < 2U || order > 3U)
		return EINVAL;
	if (max_core_hz == 0)
		return EINVAL;

	/* Keeps the framebuffer byte order coherent with the captured plane. */
	if (screen->format > 1U)
		return ENOTSUP;
	expected_order = 2;
	if (screen->format == 1U)
		expected_order = 3;
	if (order != expected_order)
		return EINVAL;

	/* Keeps unsupported high-rate SCDC and degenerate modes out of the program. */
	if (mode->tmds_hz < 25000000U || mode->tmds_hz >= 340000000U)
		return ENOTSUP;
	if (mode->width == 0 || mode->width > 4096U)
		return EINVAL;
	if (mode->height == 0 || mode->height > 4096U)
		return EINVAL;
	if (mode->hfront == 0 || mode->hfront > 8191U)
		return EINVAL;
	if (mode->hsync == 0 || mode->hsync > 2047U)
		return EINVAL;
	if (mode->hback == 0 || mode->hback > 2047U)
		return EINVAL;
	if (mode->vfront == 0 || mode->vfront > 127U)
		return EINVAL;
	if (mode->vsync == 0 || mode->vsync > 31U)
		return EINVAL;
	if (mode->vback == 0 || mode->vback > 511U)
		return EINVAL;
	if (((mode->width | mode->hfront | mode->hsync | mode->hback) & 1U) != 0)
		return ENOTSUP;

	/* Requires an exact-size RGB32 framebuffer below the one-GiB scanout limit. */
	if (screen->width != mode->width || screen->height != mode->height)
		return EINVAL;
	if (screen->physical >= 0x40000000ULL || screen->size == 0)
		return EINVAL;
	if (screen->size > 0x40000000ULL - screen->physical)
		return EINVAL;
	if (screen->pitch < mode->width * 4U || screen->pitch > 65535U)
		return EINVAL;
	if ((uint64_t)screen->pitch * screen->height > screen->size)
		return EINVAL;

	/* A captured HDMI AVI must describe RGB without pixel repetition. */
	if (mode->hdmi) {
		/* Header, payload and checksum are verified before any notification. */
		if (mode->avi[0] != 0x82 || mode->avi[1] != 2 || mode->avi[2] != 13)
			return EINVAL;
		if ((mode->avi[4] & 0x60U) != 0 || (mode->avi[8] & 0x0fU) != 0)
			return ENOTSUP;
		checksum = 0;
		for (index = 0; index < 17U; index++) {
			/* A complete packet sums to zero modulo 256. */
			checksum += mode->avi[index];
		}

		/* Rejects metadata that would transmit an invalid checksum. */
		if ((checksum & 0xffU) != 0)
			return EINVAL;
	}

	/* Succeeded: every programmed field and referenced byte is representable. */
	return 0;
}
