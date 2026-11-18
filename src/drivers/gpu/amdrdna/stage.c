/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stage marks of the amdrdna driver and its boot parameters.
 *
 * The driver brings the GPU up one stage at a time: A0 finds and
 * identifies it, A1 reads what the firmware put on the display, A1v reads
 * the video BIOS, and the later stages start the GPU's engines.  Each
 * stage writes "amdrdna: <stage> begin" before it starts and one line with
 * "ok" and its values, or "fail" and the reason, when it ends, so the
 * console and the kernel log show how far the driver came.
 *
 * The parameters are read from the raw boot command line, because the
 * driver attaches while the platform finds its devices, before the kernel
 * parses the line:
 *
 *   amdrdna.off=1        the driver does not look at the GPU at all
 *   amdrdna.auto=<stage> the stages up to this one run at boot without
 *                        being asked (A9 when the word is absent, the
 *                        desktop's GPU; amdrdna.auto=A1v runs only the
 *                        stages that read)
 *   amdrdna.train=0      memory training (stage A3t) is skipped
 *   amdrdna.train.blank=0
 *                        the firmware's picture is not blanked while
 *                        memory training runs
 *   amdrdna.fan.duty=<n> the static fan duty in percent (50 by default)
 *   amdrdna.temp.max=<n> the temperature in degrees Celsius past which the
 *                        driver faults the GPU (95 by default)
 *   amdrdna.kiq=0        stage A5 runs the graphics ring by its registers,
 *                        without the scheduler's queue and the compute
 *                        queues
 *   amdrdna.disp.own=0   stages A6 to A8 pass and the firmware keeps its
 *                        display (by default they take it over and
 *                        register the GPU device with it)
 *   amdrdna.vk=0         the GPU device renders nothing (by default the
 *                        Vulkan executor makes it a renderer)
 *   amdrdna.disp.test=<n>
 *                        stages A6 and A7 hold their test picture and
 *                        cursor n seconds (at most 60) for an eye to check
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include <kern/boot.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/platform.h>
#include <uapi/amdrdna.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"

/* The handoff object that holds the raw boot command line. */
#define STAGE_COMMAND_LINE_HANDOFF	"boot.command-line"

/* The whole word that keeps the driver away from the GPU. */
#define STAGE_OFF_WORD			"amdrdna.off=1"

/* The longest automatic-stage word: "amdrdna.auto=" and a stage name. */
#define STAGE_WORD_CAPACITY		32U

/*
 * The names of the stages, in the order of the stage numbers of the
 * control interface.
 */
static const char *const stage_names[AMDRDNA_STAGE_COUNT] = {
	"none",
	"A0",
	"A1",
	"A1v",
	"A2",
	"A3t",
	"A3",
	"A4",
	"A5",
	"A5s",
	"A6",
	"A7",
	"A8",
	"A9",
	"A5c",
	"A10",
	"A11",
	"A12",
	"A13"
};

static bool stage_word_present(const char *word);
static bool stage_number_in(const char *word, size_t key_length, unsigned *value);

/*
 * Writes one stage mark to the console and the kernel log.
 *
 * The line reads "amdrdna: <text>"; whatever does not fit in
 * AMDRDNA_MARK_COLUMNS columns is dropped.
 */
void
amdrdna_mark(
	const char *format,
	...)
{
	char line[AMDRDNA_MARK_COLUMNS + 1U];
	va_list arguments;
	int prefix;

	/* Writes the driver's prefix. */
	prefix = kern_snprintf(line, sizeof(line), "amdrdna: ");
	if (prefix < 0)
		return;

	/* Writes the text after the prefix; the formatter cuts what does not fit. */
	va_start(arguments, format);
	(void)kern_vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, arguments);
	va_end(arguments);

	/* Publishes the line to the console and the kernel log. */
	kern_logf("%s\n", line);
}

/*
 * Names a stage for the marks.
 */
const char *
amdrdna_stage_name(
	unsigned stage)
{
	/* A number past the last stage has no name. */
	if (stage >= AMDRDNA_STAGE_COUNT)
		return "?";

	/* Succeeded: the stage's name. */
	return stage_names[stage];
}

/*
 * Reports whether the boot asked the driver to stay away from the GPU.
 */
bool
amdrdna_parameter_off(void)
{
	bool present;

	/* Looks for amdrdna.off=1 among the boot parameters. */
	present = stage_word_present(STAGE_OFF_WORD);
	if (present)
		return true;

	/* The driver looks at the GPU by default. */
	return false;
}

/*
 * Reports the last stage the boot runs without being asked.
 *
 * amdrdna.auto=<stage> names it; without the word it is A1v, the last of
 * the stages that only read.
 */
unsigned
amdrdna_parameter_auto(void)
{
	char word[STAGE_WORD_CAPACITY];
	unsigned stage;
	int length;
	bool present;

	/* Tries every stage's word, from the last one down, so the highest one named wins. */
	for (stage = AMDRDNA_STAGE_COUNT - 1U; stage > AMDRDNA_STAGE_NONE; stage--) {
		/* Spells the word of this stage. */
		length = kern_snprintf(word, sizeof(word), "amdrdna.auto=%s", stage_names[stage]);
		if (length < 0 || (size_t)length >= sizeof(word))
			continue;

		/* Stops at the first word the line holds. */
		present = stage_word_present(word);
		if (present)
			return stage;
	}

	/* Without the word the boot brings the GPU up as far as the desktop needs. */
	return AMDRDNA_STAGE_A9;
}

/*
 * Reports the number a boot word "<key>=<decimal>" gives, or fallback
 * when the line has no such word or its number is not a plain decimal.
 * The last such word on the line wins.
 */
unsigned
amdrdna_parameter_number(
	const char *key,
	unsigned fallback)
{
	const char *line;
	const char *word;
	size_t key_length;
	size_t length;
	unsigned number;
	unsigned value;
	int compared;
	bool found;

	/* A boot without a command line holds no word. */
	line = kern_boot_handoff(STAGE_COMMAND_LINE_HANDOFF);
	if (line == NULL)
		return fallback;

	/* Looks at every word of the line. */
	key_length = kern_strlen(key);
	value = fallback;
	word = line;
	while (*word != '\0') {
		/* Skips the spaces before the word. */
		if (*word == ' ' || *word == '\t') {
			word++;
			continue;
		}

		/* The word runs to the next space. */
		length = 0U;
		while (word[length] != '\0' && word[length] != ' ' && word[length] != '\t')
			length++;

		/* Compares the word's start with the key and "=". */
		compared = 1;
		if (length > key_length + 1U && word[key_length] == '=')
			compared = kern_strncmp(word, key, key_length);

		/* Takes the number of a word that is the key, "=" and digits only. */
		if (compared == 0) {
			found = stage_number_in(word + key_length + 1U, length - key_length - 1U, &number);
			if (found)
				value = number;
		}

		/* Moves past the word. */
		word += length;
	}

	/* Succeeded: the last number named, or the fallback. */
	return value;
}

/* Reports whether one whole word is among the boot parameters. */
static bool
stage_word_present(
	const char *word)
{
	const char *line;
	int present;

	/* A boot without a command line holds no word. */
	line = kern_boot_handoff(STAGE_COMMAND_LINE_HANDOFF);
	if (line == NULL)
		return false;

	/* Compares the word with each word of the line. */
	present = kern_boot_parameters_token_present(line, word);
	if (present != 0)
		return true;

	/* The word is not on the line. */
	return false;
}

/* Reads a run of decimal digits of a known length; false for anything else or a number too large. */
static bool
stage_number_in(
	const char *word,
	size_t length,
	unsigned *value)
{
	unsigned number;
	size_t index;

	/* Adds one digit at a time, refusing a non-digit and more than six digits. */
	if (length == 0U || length > 6U)
		return false;
	number = 0U;
	for (index = 0U; index < length; index++) {
		/* A non-digit ends the number as invalid. */
		if (word[index] < '0' || word[index] > '9')
			return false;
		number = number * 10U + (unsigned)(word[index] - '0');
	}

	/* Succeeded: the number. */
	*value = number;
	return true;
}
