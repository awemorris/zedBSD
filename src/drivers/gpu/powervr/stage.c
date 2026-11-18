/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the PowerVR stage lines:
 * a stage's ok is printed once a boot, its failures the first three times
 * and then one "further failures not shown"; the sysctl text names each
 * stage's latest state.  Nothing here locks or prints, so the host test
 * builds it as it is.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/stage.h"

/* The stages as the lines name them. */
static const char *const stage_line_names[POWERVR_STAGE_COUNT] = {
	"firmware", "render-node", "compute", "ta", "3d", "transfer", "present",
};

/* The stages as the sysctl text names them (one word each). */
static const char *const stage_text_names[POWERVR_STAGE_COUNT] = {
	"firmware", "render-node", "compute", "ta", "3d", "transfer", "present",
};

/* The reasons as the lines name them. */
static const char *const reason_names[POWERVR_REASON_COUNT] = {
	"error", "enoent", "timeout", "bad image", "ebusy", "coherency", "fault",
	"spirv subset", "free list", "format", "import", "stride", "claim", "reset",
};

/*
 * The firmware's reasons for resetting a context, as the lines name them.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_shared.h
 * (enum rogue_context_reset_reason: ROGUE_CONTEXT_RESET_REASON_NONE 0 to
 * ROGUE_CONTEXT_GEOM_OOM_DISABLED 16).
 */
static const char *const reset_names[] = {
	"none", "lockup", "innocent lockup", "overrun", "innocent overrun",
	"context switch", "wgp checksum", "trp checksum", "gpu ecc ok", "gpu ecc",
	"fw ecc ok", "fw ecc", "fw watchdog", "fw fault", "fw error",
	"host watchdog", "geometry oom",
};

static void put_text(char *out, size_t capacity, size_t *length, const char *text, bool word);
static void put_hex(char *out, size_t capacity, size_t *length, uint64_t value);

/*
 * Appends text to out at *length, keeping out terminated and *length the
 * length of what would have been written.  With word, spaces become '-'.
 */
static void
put_text(
	char *out,
	size_t capacity,
	size_t *length,
	const char *text,
	bool word)
{
	char c;

	/* One byte at a time while there is room for it and the NUL. */
	for (; *text != '\0'; text++) {
		c = *text;
		if (word && c == ' ')
			c = '-';
		if (*length + 1U < capacity)
			out[*length] = c;
		(*length)++;
	}

	/* Keeps the output terminated. */
	if (capacity != 0U)
		out[*length < capacity ? *length : capacity - 1U] = '\0';
}

/* Appends value as "0x" and its hexadecimal digits, without leading zeros. */
static void
put_hex(
	char *out,
	size_t capacity,
	size_t *length,
	uint64_t value)
{
	static const char digits[] = "0123456789abcdef";
	char text[19];
	unsigned count;
	unsigned shift;

	/* The digits from the highest one that is not zero; zero is "0x0". */
	text[0] = '0';
	text[1] = 'x';
	count = 2;
	for (shift = 64; shift != 0U; shift -= 4U) {
		if (count == 2U && shift > 4U && (value >> (shift - 4U)) == 0U)
			continue;
		text[count++] = digits[(value >> (shift - 4U)) & 0xfU];
	}
	text[count] = '\0';
	put_text(out, capacity, length, text, false);
}

/*
 * Records that stage succeeded.  Returns true and the line in line when
 * the line is to be printed: the first ok of the boot only.
 */
bool
powervr_stage_note_ok(
	struct powervr_stage_record *record,
	enum powervr_stage stage,
	char *line,
	size_t capacity)
{
	size_t length;

	/* A stage outside the table is not recorded. */
	if ((unsigned)stage >= POWERVR_STAGE_COUNT)
		return false;
	record->state[stage] = POWERVR_STAGE_OK;
	if (record->ok_shown[stage] != 0U)
		return false;
	record->ok_shown[stage] = 1U;

	/* "powervr: <stage>: ok". */
	length = 0;
	put_text(line, capacity, &length, "powervr: ", false);
	put_text(line, capacity, &length, stage_line_names[stage], false);
	put_text(line, capacity, &length, ": ok", false);
	return true;
}

/*
 * Records that stage failed for reason.  Returns true and the line in line
 * when the line is to be printed: the first three failures, then once
 * "further failures not shown".
 */
bool
powervr_stage_note_fail(
	struct powervr_stage_record *record,
	enum powervr_stage stage,
	enum powervr_stage_reason reason,
	char *line,
	size_t capacity)
{
	bool shown;

	/* The same with no value; a fault or a reset without one prints its word alone. */
	shown = powervr_stage_note_fail_value(record, stage, reason, POWERVR_STAGE_NO_VALUE, line, capacity);

	/* Succeeded: whether the line is to be shown. */
	return shown;
}

/*
 * Records that stage failed for reason, with the value a fault or a reset
 * carries.  Prints as powervr_stage_note_fail(), the value after the
 * reason's word.
 */
bool
powervr_stage_note_fail_value(
	struct powervr_stage_record *record,
	enum powervr_stage stage,
	enum powervr_stage_reason reason,
	uint64_t value,
	char *line,
	size_t capacity)
{
	size_t length;
	uint32_t fails;

	/* A stage outside the table is not recorded; an unknown reason is "error". */
	if ((unsigned)stage >= POWERVR_STAGE_COUNT)
		return false;
	if ((unsigned)reason >= POWERVR_REASON_COUNT)
		reason = POWERVR_REASON_ERROR;
	record->state[stage] = POWERVR_STAGE_FAILED;
	record->reason[stage] = (uint8_t)reason;
	if (record->fails[stage] != UINT32_MAX)
		record->fails[stage]++;
	fails = record->fails[stage];

	/* Past the fourth failure nothing more is printed. */
	if (fails > POWERVR_STAGE_FAILS_SHOWN + 1U)
		return false;
	length = 0;
	put_text(line, capacity, &length, "powervr: ", false);
	put_text(line, capacity, &length, stage_line_names[stage], false);

	/* The fourth: one line that the rest are not shown. */
	if (fails == POWERVR_STAGE_FAILS_SHOWN + 1U) {
		put_text(line, capacity, &length, ": further failures not shown", false);
		return true;
	}

	/* "powervr: <stage>: fail(<reason>)". */
	put_text(line, capacity, &length, ": fail(", false);
	put_text(line, capacity, &length, reason_names[reason], false);
	if (reason == POWERVR_REASON_FAULT && value != POWERVR_STAGE_NO_VALUE) {
		put_text(line, capacity, &length, " ", false);
		put_hex(line, capacity, &length, value);
	} else if (reason == POWERVR_REASON_RESET && value != POWERVR_STAGE_NO_VALUE) {
		put_text(line, capacity, &length, " ", false);
		if (value < sizeof(reset_names) / sizeof(reset_names[0]))
			put_text(line, capacity, &length, reset_names[value], false);
		else
			put_hex(line, capacity, &length, value);
	}
	put_text(line, capacity, &length, ")", false);
	return true;
}

/*
 * Writes the text of hw.gpu.powervr.stages: "<stage>=ok", "<stage>=-" or
 * "<stage>=fail:<reason>" for each stage, separated by spaces.  Returns the
 * length the whole text needs without its NUL; text holds what fits.
 */
size_t
powervr_stage_text(
	const struct powervr_stage_record *record,
	char *text,
	size_t capacity)
{
	size_t length;
	unsigned stage;

	length = 0;
	if (capacity != 0U)
		text[0] = '\0';
	for (stage = 0; stage < POWERVR_STAGE_COUNT; stage++) {
		if (stage != 0U)
			put_text(text, capacity, &length, " ", false);
		put_text(text, capacity, &length, stage_text_names[stage], true);

		/* The stage's latest state. */
		if (record->state[stage] == POWERVR_STAGE_OK) {
			put_text(text, capacity, &length, "=ok", false);
		} else if (record->state[stage] == POWERVR_STAGE_FAILED) {
			put_text(text, capacity, &length, "=fail:", false);
			put_text(text, capacity, &length, reason_names[record->reason[stage]], true);
		} else {
			put_text(text, capacity, &length, "=-", false);
		}
	}

	/* Succeeded: the whole length. */
	return length;
}

/*
 * The reason for an error number where the error itself says enough;
 * "error" otherwise.
 */
enum powervr_stage_reason
powervr_stage_reason_of(
	int error)
{
	/* The errors that are their own reason. */
	switch (error) {
	case ENOENT:
		return POWERVR_REASON_ENOENT;
	case ETIMEDOUT:
		return POWERVR_REASON_TIMEOUT;
	case EBUSY:
		return POWERVR_REASON_EBUSY;
	case EFAULT:
		return POWERVR_REASON_FAULT;
	default:
		return POWERVR_REASON_ERROR;
	}
}
