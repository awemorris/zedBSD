/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The choice of a display's resolution and refresh (resolution.h says
 * what and why).
 */

#include "resolution.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The length of an EDID block, and where the base block keeps what names the monitor. */
#define RESOLUTION_EDID_BLOCK		128U
#define RESOLUTION_EDID_VENDOR		8U
#define RESOLUTION_EDID_PRODUCT		10U
#define RESOLUTION_EDID_SERIAL		12U

/* The base block's four 18-byte descriptors, and the tag of the one that holds the serial number as text. */
#define RESOLUTION_EDID_DESCRIPTORS	54U
#define RESOLUTION_EDID_DESCRIPTOR	18U
#define RESOLUTION_EDID_SERIAL_TEXT	0xffU

/* The FNV-1a hash's start and prime, which shorten a serial string or a connector's key to eight hex digits. */
#define RESOLUTION_HASH_START		2166136261U
#define RESOLUTION_HASH_PRIME		16777619U

/*
 * The common resolutions offered beside the display's default (16:9,
 * 16:10, 4:3 and 5:4, from 1024x768 up): the sizes monitors and TVs carry
 * timings for, which people choose to lower the load, to record or to
 * present.  1360x768 is the 1366x768 panels' timing.
 */
static const uint32_t resolution_common_sizes[][2] = {
	{ 1280U, 720U },
	{ 1360U, 768U },
	{ 1366U, 768U },
	{ 1600U, 900U },
	{ 1920U, 1080U },
	{ 2560U, 1440U },
	{ 3200U, 1800U },
	{ 3840U, 2160U },
	{ 1280U, 800U },
	{ 1440U, 900U },
	{ 1680U, 1050U },
	{ 1920U, 1200U },
	{ 2560U, 1600U },
	{ 1024U, 768U },
	{ 1280U, 960U },
	{ 1400U, 1050U },
	{ 1600U, 1200U },
	{ 1280U, 1024U }
};

static int resolution_same(const struct kwl_resolution_mode *first, const struct kwl_resolution_mode *second);
static int resolution_before(const struct kwl_resolution_mode *first, const struct kwl_resolution_mode *second);
static void resolution_add(struct kwl_resolution_mode *offered, unsigned *count, unsigned capacity, const struct kwl_resolution_mode *mode, unsigned flags);
static uint32_t resolution_hash(const unsigned char *bytes, size_t length);
static size_t resolution_serial_text(const uint8_t *edid, char *text, size_t size);
static int resolution_serial_real(uint32_t serial);
static void resolution_open(struct kwl_resolution_trial *trial, const struct kwl_resolution_mode *mode, unsigned state, uint64_t now_ms, struct kwl_resolution_step *step);
static void resolution_step_clear(struct kwl_resolution_step *step);

/*
 * Tells whether two refreshes (mHz) are the same refresh: closer than
 * KWL_RESOLUTION_SAME_MHZ, the rounding a refresh measured or written in
 * Hz carries.
 */
int
kwl_resolution_same_refresh(
	uint32_t first,
	uint32_t second)
{
	uint32_t difference;

	/* The distance between them, whichever is larger. */
	if (first > second) {
		difference = first - second;
	} else {
		difference = second - first;
	}

	/* Close enough is the same. */
	if (difference < KWL_RESOLUTION_SAME_MHZ)
		return 1;

	/* Succeeded: two refreshes. */
	return 0;
}

/* Tells whether a size is one of the common resolutions offered beside the default. */
int
kwl_resolution_common(
	uint32_t width,
	uint32_t height)
{
	size_t index;

	/* Each common size. */
	for (index = 0U; index < sizeof(resolution_common_sizes) / sizeof(resolution_common_sizes[0]); index++) {
		if (resolution_common_sizes[index][0] == width && resolution_common_sizes[index][1] == height)
			return 1;
	}

	/* Succeeded: not a common size. */
	return 0;
}

/*
 * Makes the modes offered to the user from the driver's list (its default
 * first) and the mode shown now (NULL when nothing is shown): every timing
 * of the default's size (marked native when it is the default itself),
 * every timing of a common size, and the mode shown now (marked current),
 * each once, the larger first and, at one size, the faster first.  Returns
 * how many were written to offered.
 */
unsigned
kwl_resolution_candidates(
	const struct kwl_resolution_mode *listed,
	unsigned count,
	const struct kwl_resolution_mode *current,
	struct kwl_resolution_mode *offered,
	unsigned capacity)
{
	struct kwl_resolution_mode swap;
	unsigned written;
	unsigned index;
	unsigned other;
	unsigned flags;
	int common;
	int before;

	/* Nothing listed offers nothing but the mode shown now. */
	written = 0U;
	if (count == 0U) {
		if (current != NULL)
			resolution_add(offered, &written, capacity, current, KWL_RESOLUTION_CURRENT);
		return written;
	}

	/* Each listed timing the user is offered. */
	for (index = 0U; index < count; index++) {
		/* The default itself is the recommended one. */
		flags = 0U;
		if (index == 0U)
			flags |= KWL_RESOLUTION_NATIVE;

		/* The mode shown now is marked as such. */
		if (current != NULL) {
			if (resolution_same(&listed[index], current))
				flags |= KWL_RESOLUTION_CURRENT;
		}

		/* A timing of the default's size, of a common size, or the one shown now. */
		common = kwl_resolution_common(listed[index].width, listed[index].height);
		if (listed[index].width == listed[0].width && listed[index].height == listed[0].height) {
			resolution_add(offered, &written, capacity, &listed[index], flags);
		} else if (common) {
			resolution_add(offered, &written, capacity, &listed[index], flags);
		} else if ((flags & KWL_RESOLUTION_CURRENT) != 0U) {
			resolution_add(offered, &written, capacity, &listed[index], flags);
		}
	}

	/* The mode shown now, when the driver did not list it. */
	if (current != NULL)
		resolution_add(offered, &written, capacity, current, KWL_RESOLUTION_CURRENT);

	/* The larger first, and at one size the faster first (an insertion sort: the lists are short). */
	for (index = 1U; index < written; index++) {
		for (other = index; other > 0U; other--) {
			before = resolution_before(&offered[other], &offered[other - 1U]);
			if (!before)
				break;
			swap = offered[other];
			offered[other] = offered[other - 1U];
			offered[other - 1U] = swap;
		}
	}

	/* Succeeded: the modes offered. */
	return written;
}

/* Finds a mode of a size and refresh among some; returns its index, or -1. */
int
kwl_resolution_find(
	const struct kwl_resolution_mode *modes,
	unsigned count,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_mhz)
{
	unsigned index;
	int same;

	/* Each mode of that size, at that refresh. */
	for (index = 0U; index < count; index++) {
		if (modes[index].width != width || modes[index].height != height)
			continue;
		same = kwl_resolution_same_refresh(modes[index].refresh_mhz, refresh_mhz);
		if (same)
			return (int)index;
	}

	/* Succeeded: none. */
	return -1;
}

/*
 * Picks a mode of a size among some: at the refresh asked for (not 0), or,
 * when none was asked for, at the refresh shown now (now_mhz), else the
 * fastest of that size.  Returns its index, or -1 when none fits.
 */
int
kwl_resolution_pick(
	const struct kwl_resolution_mode *modes,
	unsigned count,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_mhz,
	uint32_t now_mhz)
{
	unsigned index;
	int found;

	/* A refresh asked for is the one. */
	if (refresh_mhz != 0U) {
		found = kwl_resolution_find(modes, count, width, height, refresh_mhz);
		return found;
	}

	/* The refresh shown now, when the size has it. */
	found = kwl_resolution_find(modes, count, width, height, now_mhz);
	if (found >= 0)
		return found;

	/* Else the fastest of the size. */
	for (index = 0U; index < count; index++) {
		if (modes[index].width != width || modes[index].height != height)
			continue;
		if (found < 0 || modes[index].refresh_mhz > modes[(unsigned)found].refresh_mhz)
			found = (int)index;
	}

	/* Succeeded: the fastest, or none of that size. */
	return found;
}

/*
 * Makes a monitor's identity (resolution.h) from its EDID (NULL or
 * length 0 for none) and its connector's key.  Returns 0, or ENOSPC when
 * the identity does not fit `size`.
 */
int
kwl_resolution_identity(
	const uint8_t *edid,
	size_t length,
	const char *connector,
	char *identity,
	size_t size)
{
	static const uint8_t header[8] = { 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U };
	char vendor[4];
	char text[16];
	uint32_t serial;
	uint32_t product;
	uint32_t letters;
	uint32_t hash;
	size_t text_length;
	size_t used;
	size_t index;
	unsigned sum;
	int differs;
	int real;
	int written;

	/* A base block whose header and checksum hold names the monitor; anything else is known by its connector. */
	sum = 0U;
	differs = 1;
	if (edid != NULL && length >= RESOLUTION_EDID_BLOCK) {
		differs = memcmp(edid, header, sizeof(header));
		for (index = 0U; index < RESOLUTION_EDID_BLOCK; index++)
			sum += edid[index];
	}
	if (differs != 0 || (sum & 0xffU) != 0U) {
		written = snprintf(identity, size, "%s", connector);
		if (written < 0 || (size_t)written >= size)
			return ENOSPC;
		return 0;
	}

	/* The manufacturer's three letters, five bits each, big-endian. */
	letters = ((uint32_t)edid[RESOLUTION_EDID_VENDOR] << 8) | edid[RESOLUTION_EDID_VENDOR + 1U];
	vendor[0] = (char)('A' - 1 + ((letters >> 10) & 0x1fU));
	vendor[1] = (char)('A' - 1 + ((letters >> 5) & 0x1fU));
	vendor[2] = (char)('A' - 1 + (letters & 0x1fU));
	vendor[3] = '\0';

	/* The product code and the serial number, little-endian. */
	product = (uint32_t)edid[RESOLUTION_EDID_PRODUCT] | ((uint32_t)edid[RESOLUTION_EDID_PRODUCT + 1U] << 8);
	serial = (uint32_t)edid[RESOLUTION_EDID_SERIAL];
	serial |= (uint32_t)edid[RESOLUTION_EDID_SERIAL + 1U] << 8;
	serial |= (uint32_t)edid[RESOLUTION_EDID_SERIAL + 2U] << 16;
	serial |= (uint32_t)edid[RESOLUTION_EDID_SERIAL + 3U] << 24;

	/* The monitor: manufacturer, product and serial number. */
	written = snprintf(identity, size, "edid-v1:%s:%04x:%08x", vendor, (unsigned)product, (unsigned)serial);
	if (written < 0 || (size_t)written >= size)
		return ENOSPC;
	used = (size_t)written;

	/* The serial number as text, shortened, when the EDID gives it. */
	text_length = resolution_serial_text(edid, text, sizeof(text));
	if (text_length != 0U) {
		hash = resolution_hash((const unsigned char *)text, text_length);
		written = snprintf(identity + used, size - used, ":%08x", (unsigned)hash);
		if (written < 0 || (size_t)written >= size - used)
			return ENOSPC;
		return 0;
	}

	/* A serial number that tells two monitors of one model apart needs nothing more. */
	real = resolution_serial_real(serial);
	if (real)
		return 0;

	/* Without one, the connector's key tells them apart. */
	hash = resolution_hash((const unsigned char *)connector, strlen(connector));
	written = snprintf(identity + used, size - used, "@%08x", (unsigned)hash);
	if (written < 0 || (size_t)written >= size - used)
		return ENOSPC;

	/* Succeeded: the monitor of this model on this connector. */
	return 0;
}

/* Readies a trial: nothing tried. */
void
kwl_resolution_trial_init(
	struct kwl_resolution_trial *trial)
{
	/* Idle, with nothing kept. */
	memset(trial, 0, sizeof(*trial));
	trial->state = KWL_RESOLUTION_IDLE;
}

/*
 * Begins a trial of a mode on a display (its connector's key and its
 * monitor's identity): the output is opened at target, and goes back to
 * current (the mode shown when the first trial began) or, failing that,
 * to fallback (the display's default).  reset says the user asked for the
 * default and the monitor's choice is forgotten when kept.  The same mode
 * as the one shown is answered at once (and kept or forgotten).  Returns
 * 0, or EBUSY while a switch runs or another display is in its trial.
 */
int
kwl_resolution_begin(
	struct kwl_resolution_trial *trial,
	const char *key,
	const char *identity,
	const struct kwl_resolution_mode *target,
	const struct kwl_resolution_mode *current,
	const struct kwl_resolution_mode *fallback,
	unsigned reset,
	uint64_t now_ms,
	struct kwl_resolution_step *step)
{
	int differs;
	int same;

	/* Nothing asked yet. */
	resolution_step_clear(step);

	/* A switch running ends first. */
	if (trial->state == KWL_RESOLUTION_SWITCHING || trial->state == KWL_RESOLUTION_REVERTING)
		return EBUSY;

	/* A trial of another display ends first. */
	if (trial->state == KWL_RESOLUTION_TRIAL) {
		differs = strcmp(trial->key, key);
		if (differs != 0)
			return EBUSY;
	}

	/* The mode shown already: nothing to try, the choice kept (or forgotten) at once. */
	same = resolution_same(target, current);
	if (same && trial->state == KWL_RESOLUTION_IDLE) {
		step->what = KWL_RESOLUTION_DO_ANSWER | KWL_RESOLUTION_DO_SAVE;
		if (reset)
			step->what = KWL_RESOLUTION_DO_ANSWER | KWL_RESOLUTION_DO_FORGET;
		step->save = *target;
		snprintf(trial->identity, sizeof(trial->identity), "%s", identity);
		return 0;
	}

	/* The display, and what it goes back to: the first trial's mode stays through a choice made again in the trial. */
	if (trial->state == KWL_RESOLUTION_IDLE) {
		snprintf(trial->key, sizeof(trial->key), "%s", key);
		trial->back = *current;
	}
	snprintf(trial->identity, sizeof(trial->identity), "%s", identity);
	trial->target = *target;
	trial->fallback = *fallback;
	trial->reset = reset;
	trial->last_resort = 0U;
	trial->cancelled = 0U;

	/* Succeeded: the output opens at the new mode. */
	resolution_open(trial, target, KWL_RESOLUTION_SWITCHING, now_ms, step);
	return 0;
}

/* Takes the first frame shown at the mode the output was opened at. */
void
kwl_resolution_shown(
	struct kwl_resolution_trial *trial,
	uint64_t now_ms,
	struct kwl_resolution_step *step)
{
	/* Nothing asked yet. */
	resolution_step_clear(step);

	/* The new mode shows: the user's time to keep it begins, unless the trial was called off meanwhile. */
	if (trial->state == KWL_RESOLUTION_SWITCHING) {
		step->what = KWL_RESOLUTION_DO_ANSWER;
		step->answer_error = 0;
		if (trial->cancelled) {
			resolution_open(trial, &trial->back, KWL_RESOLUTION_REVERTING, now_ms, step);
			return;
		}
		trial->state = KWL_RESOLUTION_TRIAL;
		trial->deadline_ms = now_ms + KWL_RESOLUTION_TRIAL_MS;
		return;
	}

	/* Back where it was: the trial is over. */
	if (trial->state == KWL_RESOLUTION_REVERTING)
		trial->state = KWL_RESOLUTION_IDLE;
}

/*
 * Takes a failed opening, or a first frame that did not come: the new
 * mode goes back to the old one, and the old one that cannot be shown
 * either to the default; when nothing can be shown the trial ends, and
 * the compositor's loss of the display takes over.
 */
void
kwl_resolution_failed(
	struct kwl_resolution_trial *trial,
	uint64_t now_ms,
	struct kwl_resolution_step *step)
{
	int same;

	/* Nothing asked yet. */
	resolution_step_clear(step);

	/* The new mode could not be shown: answered as failed, and the old mode opened. */
	if (trial->state == KWL_RESOLUTION_SWITCHING) {
		resolution_open(trial, &trial->back, KWL_RESOLUTION_REVERTING, now_ms, step);
		step->what |= KWL_RESOLUTION_DO_ANSWER;
		step->answer_error = EIO;
		return;
	}

	/* Only a way back fails further. */
	if (trial->state != KWL_RESOLUTION_REVERTING)
		return;

	/* The old mode could not be shown: the default, unless that was it. */
	same = resolution_same(&trial->fallback, &trial->back);
	if (!trial->last_resort && !same) {
		trial->last_resort = 1U;
		resolution_open(trial, &trial->fallback, KWL_RESOLUTION_REVERTING, now_ms, step);
		return;
	}

	/* Nothing could be shown: the trial is over. */
	trial->state = KWL_RESOLUTION_IDLE;
}

/* Takes the time: a switch or a way back that took too long failed, and a trial not kept in time goes back. */
void
kwl_resolution_tick(
	struct kwl_resolution_trial *trial,
	uint64_t now_ms,
	struct kwl_resolution_step *step)
{
	/* Nothing asked yet. */
	resolution_step_clear(step);

	/* Only a state whose time ended. */
	if (trial->state == KWL_RESOLUTION_IDLE || now_ms < trial->deadline_ms)
		return;

	/* The user did not keep the new mode: it goes back. */
	if (trial->state == KWL_RESOLUTION_TRIAL) {
		resolution_open(trial, &trial->back, KWL_RESOLUTION_REVERTING, now_ms, step);
		return;
	}

	/* A first frame that did not come in time. */
	kwl_resolution_failed(trial, now_ms, step);
}

/*
 * Keeps the mode in its trial on a display: the choice is written (or
 * forgotten, for a reset).  Returns 0, EBUSY while its switch runs, or
 * ENOENT when the display is not in its trial.
 */
int
kwl_resolution_keep(
	struct kwl_resolution_trial *trial,
	const char *key,
	struct kwl_resolution_step *step)
{
	int differs;

	/* Nothing asked yet. */
	resolution_step_clear(step);

	/* The display in its trial (or in its switch, which is waited for). */
	if (trial->state == KWL_RESOLUTION_IDLE)
		return ENOENT;
	differs = strcmp(trial->key, key);
	if (differs != 0)
		return ENOENT;
	if (trial->state != KWL_RESOLUTION_TRIAL)
		return EBUSY;

	/* The trial is over, and the choice written or forgotten. */
	trial->state = KWL_RESOLUTION_IDLE;
	step->what = KWL_RESOLUTION_DO_SAVE;
	if (trial->reset)
		step->what = KWL_RESOLUTION_DO_FORGET;
	step->save = trial->target;

	/* Succeeded: the mode stays. */
	return 0;
}

/*
 * Puts a display in its trial back to the mode it had (a switch running
 * goes back once it shows).  Returns 0, or ENOENT when the display is not
 * in its trial.
 */
int
kwl_resolution_revert(
	struct kwl_resolution_trial *trial,
	const char *key,
	uint64_t now_ms,
	struct kwl_resolution_step *step)
{
	int differs;

	/* Nothing asked yet. */
	resolution_step_clear(step);

	/* The display in its trial or its switch. */
	if (trial->state != KWL_RESOLUTION_TRIAL && trial->state != KWL_RESOLUTION_SWITCHING)
		return ENOENT;
	differs = strcmp(trial->key, key);
	if (differs != 0)
		return ENOENT;

	/* Succeeded: back now, or once the switch shows. */
	kwl_resolution_cancel(trial, now_ms, step);
	return 0;
}

/*
 * Calls a trial off (the user's client gone, the display unplugged, the
 * session locked or ended, a game shown): the output goes back now, or
 * once its switch shows.
 */
void
kwl_resolution_cancel(
	struct kwl_resolution_trial *trial,
	uint64_t now_ms,
	struct kwl_resolution_step *step)
{
	/* Nothing asked yet. */
	resolution_step_clear(step);

	/* A switch running goes back once it shows. */
	if (trial->state == KWL_RESOLUTION_SWITCHING) {
		trial->cancelled = 1U;
		return;
	}

	/* A trial goes back now. */
	if (trial->state == KWL_RESOLUTION_TRIAL)
		resolution_open(trial, &trial->back, KWL_RESOLUTION_REVERTING, now_ms, step);
}

/* Tells how long the user has left to keep the mode in its trial (ms), 0 outside one. */
uint32_t
kwl_resolution_remaining(
	const struct kwl_resolution_trial *trial,
	uint64_t now_ms)
{
	/* Only a trial has time left. */
	if (trial->state != KWL_RESOLUTION_TRIAL || now_ms >= trial->deadline_ms)
		return 0U;

	/* Succeeded: the time left. */
	return (uint32_t)(trial->deadline_ms - now_ms);
}

/* Tells whether two modes are the same: one size, one refresh. */
static int
resolution_same(
	const struct kwl_resolution_mode *first,
	const struct kwl_resolution_mode *second)
{
	int same;

	/* Another size. */
	if (first->width != second->width || first->height != second->height)
		return 0;

	/* The same size at the same refresh. */
	same = kwl_resolution_same_refresh(first->refresh_mhz, second->refresh_mhz);
	if (same)
		return 1;

	/* Succeeded: another refresh. */
	return 0;
}

/* Tells whether a mode goes before another in the list: more pixels, or at one size a faster refresh. */
static int
resolution_before(
	const struct kwl_resolution_mode *first,
	const struct kwl_resolution_mode *second)
{
	uint64_t first_pixels;
	uint64_t second_pixels;

	/* The pixels of each. */
	first_pixels = (uint64_t)first->width * first->height;
	second_pixels = (uint64_t)second->width * second->height;

	/* More pixels first. */
	if (first_pixels > second_pixels)
		return 1;
	if (first_pixels < second_pixels)
		return 0;

	/* At the same count the wider first. */
	if (first->width > second->width)
		return 1;
	if (first->width < second->width)
		return 0;

	/* At one size the faster first. */
	if (first->refresh_mhz > second->refresh_mhz)
		return 1;

	/* Succeeded: it goes after, or the two are equal. */
	return 0;
}

/* Adds a mode to the list once: a mode there already takes the new marks. */
static void
resolution_add(
	struct kwl_resolution_mode *offered,
	unsigned *count,
	unsigned capacity,
	const struct kwl_resolution_mode *mode,
	unsigned flags)
{
	unsigned index;
	int same;

	/* The same mode listed already gains the marks. */
	for (index = 0U; index < *count; index++) {
		same = resolution_same(&offered[index], mode);
		if (same) {
			offered[index].flags |= flags;
			return;
		}
	}

	/* A new one, while there is room. */
	if (*count >= capacity)
		return;
	offered[*count].width = mode->width;
	offered[*count].height = mode->height;
	offered[*count].refresh_mhz = mode->refresh_mhz;
	offered[*count].flags = flags;
	(*count)++;
}

/* Shortens some bytes to 32 bits (FNV-1a). */
static uint32_t
resolution_hash(
	const unsigned char *bytes,
	size_t length)
{
	uint32_t hash;
	size_t index;

	/* Each byte folded in. */
	hash = RESOLUTION_HASH_START;
	for (index = 0U; index < length; index++) {
		hash ^= bytes[index];
		hash *= RESOLUTION_HASH_PRIME;
	}

	/* Succeeded: the hash. */
	return hash;
}

/*
 * Copies the serial number the base block gives as text (its descriptor
 * 0xff: up to 13 characters, ended by a newline and padded with spaces),
 * without the end.  Returns its length, 0 when there is none.
 */
static size_t
resolution_serial_text(
	const uint8_t *edid,
	char *text,
	size_t size)
{
	const uint8_t *descriptor;
	size_t length;
	unsigned index;

	/* Each descriptor of the base block. */
	for (index = 0U; index < 4U; index++) {
		descriptor = edid + RESOLUTION_EDID_DESCRIPTORS + index * RESOLUTION_EDID_DESCRIPTOR;

		/* A display descriptor (no pixel clock) of the serial's tag. */
		if (descriptor[0] != 0U || descriptor[1] != 0U || descriptor[2] != 0U)
			continue;
		if (descriptor[3] != RESOLUTION_EDID_SERIAL_TEXT)
			continue;

		/* Its characters up to the newline, the padding left out. */
		length = 0U;
		while (length < 13U && length + 1U < size && descriptor[5U + length] != 0x0aU) {
			text[length] = (char)descriptor[5U + length];
			length++;
		}
		while (length > 0U && text[length - 1U] == ' ')
			length--;
		text[length] = '\0';
		return length;
	}

	/* Succeeded: no serial as text. */
	return 0U;
}

/*
 * Tells whether a serial number tells monitors apart: not 0 and not four
 * equal bytes (0x01010101 and 0xffffffff are what many models carry).
 */
static int
resolution_serial_real(
	uint32_t serial)
{
	uint32_t byte;

	/* No number. */
	if (serial == 0U)
		return 0;

	/* Four equal bytes are a model's placeholder. */
	byte = serial & 0xffU;
	if (serial == byte * 0x01010101U)
		return 0;

	/* Succeeded: a number of its own. */
	return 1;
}

/* Asks the output opened at a mode, in a state whose time ends after a switch's limit. */
static void
resolution_open(
	struct kwl_resolution_trial *trial,
	const struct kwl_resolution_mode *mode,
	unsigned state,
	uint64_t now_ms,
	struct kwl_resolution_step *step)
{
	/* The state and its limit. */
	trial->state = state;
	trial->deadline_ms = now_ms + KWL_RESOLUTION_SWITCH_MS;

	/* The opening asked. */
	step->what |= KWL_RESOLUTION_DO_OPEN;
	step->open = *mode;
}

/* Clears a step: nothing asked. */
static void
resolution_step_clear(
	struct kwl_resolution_step *step)
{
	/* Nothing to open, save or answer. */
	memset(step, 0, sizeof(*step));
}
