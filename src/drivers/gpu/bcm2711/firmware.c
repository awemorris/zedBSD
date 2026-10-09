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
 * through the mailbox's property channel.  This file only asks (the get
 * tags); nothing here changes the firmware's state.  The tag values and the
 * clock numbers are those of the Raspberry Pi firmware wiki's mailbox
 * property interface.
 */

#include <stdbool.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"

/* Asks whether a clock is on and whether it exists. */
#define FIRMWARE_TAG_CLOCK_ON_OFF	0x00030001U

/* Asks a clock's present rate, in hertz. */
#define FIRMWARE_TAG_CLOCK_HZ		0x00030002U

/* Asks the highest rate a clock may be set to, in hertz. */
#define FIRMWARE_TAG_CLOCK_TOP_HZ	0x00030004U

/* The on/off answer: bit 0 is set while the clock runs. */
#define FIRMWARE_CLOCK_RUNNING		0x00000001U

/* The on/off answer: bit 1 is set when the firmware has no such clock. */
#define FIRMWARE_CLOCK_ABSENT		0x00000002U

/* The words of a clock question and of its answer: the clock number and one value. */
#define FIRMWARE_CLOCK_WORDS		2U

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
 * Asks a clock's present rate; a clock the firmware lacks or keeps off has
 * a rate of zero.
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

	/* A stopped clock runs at no rate at all. */
	if ((on_off & FIRMWARE_CLOCK_RUNNING) == 0) {
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
