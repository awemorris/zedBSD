/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The loader's configuration file, zedbsd.cfg in the boot partition.
 *
 * One setting per line; a line ends with a carriage return (as Mac OS
 * writes text) or a line feed.  Empty lines and lines starting with '#'
 * are skipped.  "kernel=NAME" names the kernel's file in the partition's
 * top folder; every other line is a kernel parameter, and the parameters
 * are joined with spaces into the kernel's command line.  A name or a
 * line too long to keep is left out whole.
 */

#include "bootloader/mac68k/stage2.h"

/* The kernel's file when the configuration names none. */
#define DEFAULT_KERNEL		"vmunix"

/* The prefix of the line that names the kernel's file. */
#define KERNEL_PREFIX		"kernel="
#define KERNEL_PREFIX_LENGTH	7U

static void take_line(struct mac68k_config *config, const char *line, uint32_t length);
static int starts_with(const char *line, uint32_t length, const char *prefix, uint32_t prefix_length);

/*
 * Reads the configuration's text into the kernel's name and command
 * line, starting from the defaults.
 */
void
mac68k_config_parse(
	struct mac68k_config *config,
	const char *text,
	uint32_t length)
{
	uint32_t start;
	uint32_t index;

	/* Starts from the default kernel and no parameter. */
	mac68k_fill_bytes(config, 0, sizeof(*config));
	mac68k_copy_bytes(config->kernel, DEFAULT_KERNEL, sizeof(DEFAULT_KERNEL));

	/* Takes each line, up to the end of the text. */
	start = 0;
	for (index = 0; index <= length; index++) {
		/* A line ends at a line end or at the end of the text. */
		if (index < length && text[index] != '\r' && text[index] != '\n')
			continue;

		take_line(config, text + start, index - start);
		start = index + 1U;
	}
}

/* Takes one line: the kernel's name, a parameter, or nothing. */
static void
take_line(
	struct mac68k_config *config,
	const char *line,
	uint32_t length)
{
	uint32_t used;
	int is_kernel;

	/* Skips an empty line and a comment. */
	if (length == 0U)
		return;
	if (line[0] == '#')
		return;

	/* Names the kernel's file, when the name fits an HFS name. */
	is_kernel = starts_with(line, length, KERNEL_PREFIX, KERNEL_PREFIX_LENGTH);
	if (is_kernel) {
		if (length - KERNEL_PREFIX_LENGTH == 0U || length - KERNEL_PREFIX_LENGTH > MAC68K_NAME_MAX)
			return;
		mac68k_copy_bytes(config->kernel, line + KERNEL_PREFIX_LENGTH, length - KERNEL_PREFIX_LENGTH);
		config->kernel[length - KERNEL_PREFIX_LENGTH] = '\0';
		return;
	}

	/* Leaves out a parameter that does not fit with its separator and the terminator. */
	used = mac68k_string_length(config->command_line);
	if (used + 1U + length + 1U > MAC68K_COMMAND_LINE_MAX)
		return;

	/* Appends the parameter after a space. */
	if (used != 0U) {
		config->command_line[used] = ' ';
		used++;
	}
	mac68k_copy_bytes(config->command_line + used, line, length);
	config->command_line[used + length] = '\0';
}

/* Tells whether a line starts with a prefix. */
static int
starts_with(
	const char *line,
	uint32_t length,
	const char *prefix,
	uint32_t prefix_length)
{
	uint32_t index;

	/* A line shorter than the prefix cannot start with it. */
	if (length < prefix_length)
		return 0;

	/* Compares the prefix character by character. */
	for (index = 0; index < prefix_length; index++) {
		if (line[index] != prefix[index])
			return 0;
	}

	/* Succeeded: the line starts with the prefix. */
	return 1;
}
