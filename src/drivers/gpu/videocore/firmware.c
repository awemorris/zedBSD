/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Questions the BCM2711 graphics driver asks the VideoCore firmware.
 *
 * The firmware owns the clocks of the display path and of V3D, and it set up
 * the screen the kernel's console is drawn on.  The ARM side asks about them
 * through the mailbox's property channel.  Display register access first
 * prepares the required clocks through the same provider.  The tag values and the
 * clock numbers are those of the Raspberry Pi firmware wiki's mailbox
 * property interface.
 */

#include <stdbool.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"

/* Asks whether a clock is on and whether it exists. */
#define FIRMWARE_TAG_CLOCK_ON_OFF	0x00030001U

/* Asks a clock's present rate, in hertz. */
#define FIRMWARE_TAG_CLOCK_HZ		0x00030002U

/* Asks the highest rate a clock may be set to, in hertz. */
#define FIRMWARE_TAG_CLOCK_TOP_HZ	0x00030004U

/* Supplies a usable rate before starting an initially stopped provider. */
#define FIRMWARE_TAG_CLOCK_SET_HZ	0x00038002U

/* Enables a clock without changing any display pipeline registers. */
#define FIRMWARE_TAG_CLOCK_ENABLE	0x00038001U

/* The on/off answer: bit 0 is set while the clock runs. */
#define FIRMWARE_CLOCK_RUNNING		0x00000001U

/* The on/off answer: bit 1 is set when the firmware has no such clock. */
#define FIRMWARE_CLOCK_ABSENT		0x00000002U

/* CORE supplies the shared bus and is critical, not an independently gated leaf. */
#define FIRMWARE_CLOCK_CORE		4U

/* The words of a clock question and of its answer: the clock number and one value. */
#define FIRMWARE_CLOCK_WORDS		2U

/*
 * The board's state while frames run (Raspberry Pi firmware wiki, "Mailbox
 * property interface"; Linux drivers/hwmon/raspberrypi-hwmon.c for the
 * throttling flags): a clock's rate as the firmware measures it, the SoC's
 * temperature in thousandths of a degree (sensor 0), and the throttling
 * flags.  The flags' request word names the sticky bits to clear; zero keeps
 * them, so bits 16 to 19 tell what happened since boot and bits 0 to 3 what
 * holds now (under-voltage, the CPU clock capped, throttled, the soft
 * temperature limit).
 */
#define FIRMWARE_TAG_CLOCK_MEASURED_HZ	0x00030047U
#define FIRMWARE_TAG_TEMPERATURE	0x00030006U
#define FIRMWARE_TAG_THROTTLED		0x00030046U
#define FIRMWARE_CLOCK_ARM		3U
#define FIRMWARE_CLOCK_V3D		5U
#define FIRMWARE_TEMPERATURE_SOC	0U

static int ask_clock(uint32_t tag, uint32_t clock_id, uint32_t *answer);

/*
 * Asks the firmware one get tag and checks that the answer is long enough.
 *
 * values holds request_count words on entry and answer_words words of the
 * answer on return.  An answer shorter than answer_words words is refused
 * with EIO, so the caller never reads a word the firmware did not write.
 */
int
bcm2711_firmware_get(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned answer_words)
{
	uint32_t answered;
	int error;

	/* Sends the tag and waits for the firmware. */
	error = drv_rpi4_firmware_property(tag, values, request_count, answer_words, &answered);
	if (error != 0)
		return error;

	/* Refuses an answer too short to hold the words asked for. */
	if (answered < answer_words * 4U)
		return EIO;

	/* Succeeded: values holds the answer. */
	return 0;
}

/*
 * Asks a clock's present rate, rejecting absent or gated leaf clocks.
 * CORE is the critical bus clock: its rate remains meaningful even when
 * firmware does not report an enabled leaf gate.
 */
int
bcm2711_clock_hz(
	uint32_t clock_id,
	uint32_t *hz)
{
	uint32_t on_off;
	int error;

	/* Asks whether the clock exists and runs. */
	error = ask_clock(FIRMWARE_TAG_CLOCK_ON_OFF, clock_id, &on_off);
	if (error != 0)
		return error;

	/* A missing clock runs at no rate at all. */
	if ((on_off & FIRMWARE_CLOCK_ABSENT) != 0) {
		*hz = 0;
		return 0;
	}

	/* CORE stays live for the shared bus; only leaf clocks require a gate bit. */
	if (clock_id != FIRMWARE_CLOCK_CORE && (on_off & FIRMWARE_CLOCK_RUNNING) == 0) {
		*hz = 0;
		return 0;
	}

	/* Asks the present rate. */
	error = ask_clock(FIRMWARE_TAG_CLOCK_HZ, clock_id, hz);
	if (error != 0)
		return error;

	/* Succeeded: hz holds the clock's rate. */
	return 0;
}

/*
 * Prepares a firmware clock before any dependent display register is read.
 *
 * A configured rate survives an off state.  Preserve it when sufficient,
 * otherwise set the required minimum before enabling, as the HSM cannot
 * safely service register accesses with a zero rate.  Leave enabled clocks
 * running on failure because the firmware console may still need them.
 */
int
bcm2711_clock_prepare(
	uint32_t clock_id,
	uint32_t minimum_hz,
	uint32_t *hz)
{
	uint32_t state;
	uint32_t rate;
	uint32_t values[3];
	uint32_t answered;
	int error;

	/* Refuses an absent provider without issuing any state changes. */
	*hz = 0;
	error = ask_clock(FIRMWARE_TAG_CLOCK_ON_OFF, clock_id, &state);
	if (error != 0)
		return error;

	/* A missing clock cannot make dependent MMIO accessible. */
	if ((state & FIRMWARE_CLOCK_ABSENT) != 0)
		return ENODEV;

	/* Reads the next-enable rate even when GET_STATE reports the clock off. */
	error = ask_clock(FIRMWARE_TAG_CLOCK_HZ, clock_id, &rate);
	if (error != 0)
		return error;

	/* Raises a zero or insufficient rate without lowering an active console clock. */
	if (rate < minimum_hz) {
		values[0] = clock_id;
		values[1] = minimum_hz;
		values[2] = 1;
		error = drv_rpi4_firmware_property(
			FIRMWARE_TAG_CLOCK_SET_HZ,
			values,
			3U,
			3U,
			&answered);
		if (error != 0)
			return error;

		/* Requires the provider's response to describe the clock we requested. */
		if (answered < 8U)
			return EIO;

		/* A response for another provider cannot authorize register access. */
		if (values[0] != clock_id)
			return EIO;

		/* Verifies clamping before enabling a possibly unusable clock. */
		error = ask_clock(FIRMWARE_TAG_CLOCK_HZ, clock_id, &rate);
		if (error != 0)
			return error;
	}

	/* A provider with no configured rate cannot be enabled for MMIO. */
	if (rate == 0)
		return ENODEV;

	/* Rejects a rate the provider could not raise to the required minimum. */
	if (rate < minimum_hz)
		return EIO;

	/* Enables only a stopped clock; an already-running console keeps its state. */
	if ((state & FIRMWARE_CLOCK_RUNNING) == 0) {
		values[0] = clock_id;
		values[1] = FIRMWARE_CLOCK_RUNNING;
		error = bcm2711_firmware_get(FIRMWARE_TAG_CLOCK_ENABLE, values, 2U, 2U);
		if (error != 0)
			return error;

		/* Requires the acknowledgement to name the requested provider. */
		if (values[0] != clock_id)
			return EIO;
	}

	/* Confirms both running state and a usable rate through fresh provider reads. */
	error = bcm2711_clock_hz(clock_id, &rate);
	if (error != 0)
		return error;

	/* A refused enable leaves downstream registers inaccessible. */
	if (rate == 0)
		return ENODEV;

	/* An enable-time rate change must still meet the consumer's requirement. */
	if (rate < minimum_hz)
		return EIO;

	/* Succeeded: the caller may access registers supplied by this clock. */
	*hz = rate;
	return 0;
}

/*
 * Writes one stage-mark line with what the firmware says about a clock.
 *
 * The line names the clock, whether it runs, its present rate and its
 * highest rate.  A question the firmware does not answer is shown with the
 * error it gave, so the mark is written whatever happens.
 */
void
bcm2711_clock_report(
	const char *family,
	const char *name,
	uint32_t clock_id)
{
	uint32_t on_off;
	uint32_t hz;
	uint32_t top_hz;
	int error;

	/* Asks whether the clock exists and runs. */
	error = ask_clock(FIRMWARE_TAG_CLOCK_ON_OFF, clock_id, &on_off);
	if (error != 0) {
		bcm2711_stage_mark(family, "clk %s (%u) no answer (%d)", name, (unsigned)clock_id, error);
		return;
	}

	/* Reports a clock the firmware does not have. */
	if ((on_off & FIRMWARE_CLOCK_ABSENT) != 0) {
		bcm2711_stage_mark(family, "clk %s (%u) absent", name, (unsigned)clock_id);
		return;
	}

	/* Asks the present rate; zero stands for an unanswered question. */
	error = ask_clock(FIRMWARE_TAG_CLOCK_HZ, clock_id, &hz);
	if (error != 0)
		hz = 0;

	/* Asks the highest rate; zero stands for an unanswered question. */
	error = ask_clock(FIRMWARE_TAG_CLOCK_TOP_HZ, clock_id, &top_hz);
	if (error != 0)
		top_hz = 0;

	/* Writes the clock's line. */
	bcm2711_stage_mark(family, "clk %s (%u) %s now %u Hz top %u Hz",
			   name,
			   (unsigned)clock_id,
			   (on_off & FIRMWARE_CLOCK_RUNNING) != 0 ? "on" : "off",
			   (unsigned)hz,
			   (unsigned)top_hz);

	/* Succeeded: the firmware clock bounds and current state have been reported. */
	return;
}

/* Sends one get tag about a clock and returns the value word of its answer. */
static int
ask_clock(
	uint32_t tag,
	uint32_t clock_id,
	uint32_t *answer)
{
	uint32_t values[FIRMWARE_CLOCK_WORDS];
	int error;

	/* Fills the request: the clock number, and room for the answer. */
	values[0] = clock_id;
	values[1] = 0;

	/* Asks the firmware and requires both words of the answer. */
	error = bcm2711_firmware_get(tag, values, 1U, FIRMWARE_CLOCK_WORDS);
	if (error != 0)
		return error;

	/* Refuses an answer about another clock. */
	if (values[0] != clock_id)
		return EIO;

	/* Succeeded: the value word answers the question. */
	*answer = values[1];
	return 0;
}

/*
 * Asks the firmware for the board's state; each question it does not
 * answer leaves its field zero.
 */
void
bcm2711_board_state_read(
	struct bcm2711_board_state *state)
{
	uint32_t values[1];

	/* Every field starts unanswered. */
	kern_memset(state, 0, sizeof(*state));

	/* The clocks: the CPU's as set and as measured, the core's and V3D's. */
	if (ask_clock(FIRMWARE_TAG_CLOCK_HZ, FIRMWARE_CLOCK_ARM, &state->arm_hz) != 0)
		state->arm_hz = 0U;
	if (ask_clock(FIRMWARE_TAG_CLOCK_MEASURED_HZ, FIRMWARE_CLOCK_ARM, &state->arm_measured_hz) != 0)
		state->arm_measured_hz = 0U;
	if (ask_clock(FIRMWARE_TAG_CLOCK_HZ, FIRMWARE_CLOCK_CORE, &state->core_hz) != 0)
		state->core_hz = 0U;
	if (ask_clock(FIRMWARE_TAG_CLOCK_HZ, FIRMWARE_CLOCK_V3D, &state->v3d_hz) != 0)
		state->v3d_hz = 0U;

	/* The SoC's temperature. */
	if (ask_clock(FIRMWARE_TAG_TEMPERATURE, FIRMWARE_TEMPERATURE_SOC, &state->milli_celsius) != 0)
		state->milli_celsius = 0U;

	/* The throttling flags, the sticky ones kept. */
	values[0] = 0U;
	if (bcm2711_firmware_get(FIRMWARE_TAG_THROTTLED, values, 1U, 1U) == 0)
		state->throttled = values[0];
}
