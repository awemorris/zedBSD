/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parameters of the video BIOS's display routines, laid out as the
 * routines read them (little-endian words): the pixel clock, the
 * encoder's stream setup, the transmitter, a timing generator's enable,
 * a pipe's power and a timing generator's blank.  The routines run on the
 * runner (bytecode.c); these only pack what they read.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The byte places of each parameter block.
 *
 * The pixel clock (version 1.7, 16 bytes): the clock in 100 Hz (bytes
 * 0 to 3), the PLL (4), the encoder object's identifier (5), the encoder
 * mode (6), the flags (7), the timing generator (8), the deep colour
 * ratio (9).  The encoder's stream setup (version 1.5, 12 bytes): the DIG
 * (0), the action (1), the mode (2), the lanes (3), the pixel clock in
 * 10 kHz (4 to 7), the bits a colour (8), the DisplayPort rate in 270 MHz
 * (9).  The transmitter (version 1.6, 16 bytes and 16 reserved): the PHY
 * (0), the action (1), the mode or the lane setting (2), the lanes (3),
 * the symbol clock in 10 kHz (4 to 7), the hot-plug line (8), the DIG
 * front ends (9), the connector object's identifier (10).  A timing
 * generator's enable and a pipe's power (4 bytes): the generator (0) and
 * enable (1).  A blank (8 bytes): the generator (0) and blank (1).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct set_pixel_clock_parameter_v1_7,
 * struct dig_encoder_stream_setup_parameters_v1_5,
 * struct dig_transmitter_control_parameters_v1_6,
 * struct dig_transmitter_control_ps_allocation_v1_6,
 * struct enable_crtc_parameters,
 * struct enable_disp_power_gating_parameters_v2_1,
 * struct blank_crtc_parameters).
 */
#define CLOCK_PLL_AT			4U
#define CLOCK_ENCODER_AT		5U
#define CLOCK_MODE_AT			6U
#define CLOCK_FLAGS_AT			7U
#define CLOCK_GENERATOR_AT		8U
#define CLOCK_DEPTH_AT			9U
#define ENCODER_DIG_AT			0U
#define ENCODER_ACTION_AT		1U
#define ENCODER_MODE_AT			2U
#define ENCODER_LANES_AT		3U
#define ENCODER_DEPTH_AT		8U
#define ENCODER_RATE_AT			9U
#define TRANSMITTER_PHY_AT		0U
#define TRANSMITTER_ACTION_AT		1U
#define TRANSMITTER_MODE_AT		2U
#define TRANSMITTER_LANES_AT		3U
#define TRANSMITTER_HOTPLUG_AT		8U
#define TRANSMITTER_FRONT_ENDS_AT	9U
#define TRANSMITTER_CONNECTOR_AT	10U
#define PIPE_GENERATOR_AT		0U
#define PIPE_ENABLE_AT			1U

static void command_byte(uint32_t *words, unsigned at, uint32_t value);

/* Packs the pixel clock's parameters. */
void
amdrdna_command_pixel_clock(
	uint32_t *words,
	const struct amdrdna_pixel_clock_command *command)
{
	unsigned index;

	/* Zeros, then each field. */
	for (index = 0U; index < AMDRDNA_COMMAND_WORDS; index++)
		words[index] = 0U;
	words[0] = command->pixel_100hz;
	command_byte(words, CLOCK_PLL_AT, command->pll);
	command_byte(words, CLOCK_ENCODER_AT, command->encoder_object);
	command_byte(words, CLOCK_MODE_AT, command->mode);
	command_byte(words, CLOCK_FLAGS_AT, command->flags);
	command_byte(words, CLOCK_GENERATOR_AT, command->generator);
	command_byte(words, CLOCK_DEPTH_AT, command->depth);
}

/* Packs the encoder's stream setup. */
void
amdrdna_command_encoder(
	uint32_t *words,
	const struct amdrdna_encoder_command *command)
{
	unsigned index;

	/* Zeros, then each field. */
	for (index = 0U; index < AMDRDNA_COMMAND_WORDS; index++)
		words[index] = 0U;
	command_byte(words, ENCODER_DIG_AT, command->dig);
	command_byte(words, ENCODER_ACTION_AT, command->action);
	command_byte(words, ENCODER_MODE_AT, command->mode);
	command_byte(words, ENCODER_LANES_AT, command->lanes);
	words[1] = command->pixel_10khz;
	command_byte(words, ENCODER_DEPTH_AT, command->bits_per_colour);
	command_byte(words, ENCODER_RATE_AT, command->dp_rate);
}

/* Packs the transmitter's parameters (the reserved words after them zero). */
void
amdrdna_command_transmitter(
	uint32_t *words,
	const struct amdrdna_transmitter_command *command)
{
	unsigned index;

	/* Zeros, then each field. */
	for (index = 0U; index < AMDRDNA_COMMAND_WORDS; index++)
		words[index] = 0U;
	command_byte(words, TRANSMITTER_PHY_AT, command->phy);
	command_byte(words, TRANSMITTER_ACTION_AT, command->action);
	command_byte(words, TRANSMITTER_MODE_AT, command->mode);
	command_byte(words, TRANSMITTER_LANES_AT, command->lanes);
	words[1] = command->symbol_10khz;
	command_byte(words, TRANSMITTER_HOTPLUG_AT, command->hotplug);
	command_byte(words, TRANSMITTER_FRONT_ENDS_AT, command->front_ends);
	command_byte(words, TRANSMITTER_CONNECTOR_AT, command->connector_id);
}

/* Packs a timing generator's enable, a pipe's power or a blank: the generator and the switch. */
void
amdrdna_command_pipe(
	uint32_t *words,
	uint32_t generator,
	uint32_t on)
{
	unsigned index;

	/* Zeros, then the two bytes. */
	for (index = 0U; index < AMDRDNA_COMMAND_WORDS; index++)
		words[index] = 0U;
	command_byte(words, PIPE_GENERATOR_AT, generator);
	command_byte(words, PIPE_ENABLE_AT, on);
}

/* Puts a byte into the words at a byte place. */
static void
command_byte(
	uint32_t *words,
	unsigned at,
	uint32_t value)
{
	unsigned shift;

	/* The byte's word and its place in it. */
	shift = (at % 4U) * 8U;
	words[at / 4U] &= ~(0xffU << shift);
	words[at / 4U] |= (value & 0xffU) << shift;
}
