/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The gpu program: lists the machine's GPU nodes and shows and controls
 * one node's capabilities, displays, Vulkan, GL and video, reset and
 * reports.  Everything but help is for root alone.  This file takes the
 * command line apart and hands the run to the file of the area asked for.
 *
 *   gpu [--porcelain]                       the list
 *   gpu [--porcelain] GPU [show]            one node's summary
 *   gpu [--porcelain] GPU caps [--capset=ID]
 *   gpu [--porcelain] GPU display [list | modes D | edid D [--raw] | set D WxH[@HZ] | keep D | revert D | reset D]
 *   gpu [--porcelain] GPU vulkan [properties|features|limits|formats|extensions|queues]
 *   gpu [--porcelain] GPU gl [es|desktop]
 *   gpu [--porcelain] GPU video
 *   gpu [--porcelain] GPU reset [--wait=MS] [--yes]
 *   gpu [--porcelain] GPU diagnose [list | KIND [--argument=N] [--output=FILE]]
 *   gpu help
 *
 * The exit status is 0, 1 for a request refused or failed (root alone may
 * use gpu), or 2 for a wrong use.
 */

#include "gpu-command.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The most words other than options a line can have: GPU display set D MODE. */
#define GPU_WORDS_MAX			5U

/* The options, as bits of the set seen on one line. */
#define GPU_OPTION_CAPSET		0x001U
#define GPU_OPTION_RAW			0x002U
#define GPU_OPTION_KEEP			0x004U
#define GPU_OPTION_TIMEOUT		0x008U
#define GPU_OPTION_SESSION		0x010U
#define GPU_OPTION_WAIT			0x020U
#define GPU_OPTION_YES			0x040U
#define GPU_OPTION_ARGUMENT		0x080U
#define GPU_OPTION_OUTPUT		0x100U

/* The longest trial a display change may wait for a keep (s). */
#define GPU_TIMEOUT_MAX_S		3600U

/* The largest width or height a mode may name. */
#define GPU_MODE_SIZE_MAX		65535U

/* The highest refresh a mode may name (Hz). */
#define GPU_MODE_HZ_MAX			1000U

/*
 * One word of the grammar and what it stands for: an area, a display verb,
 * a Vulkan section or a GL API, as the table it is in says.
 */
struct gpu_word {
	const char *word;
	int value;
};

/* The area words that follow GPU. */
static const struct gpu_word gpu_area_words[] = {
	{ "show", GPU_AREA_SHOW },
	{ "caps", GPU_AREA_CAPS },
	{ "display", GPU_AREA_DISPLAY },
	{ "vulkan", GPU_AREA_VULKAN },
	{ "gl", GPU_AREA_GL },
	{ "video", GPU_AREA_VIDEO },
	{ "reset", GPU_AREA_RESET },
	{ "diagnose", GPU_AREA_DIAGNOSE }
};

/* The verbs that follow display. */
static const struct gpu_word gpu_display_words[] = {
	{ "list", GPU_DISPLAY_VERB_LIST },
	{ "modes", GPU_DISPLAY_VERB_MODES },
	{ "edid", GPU_DISPLAY_VERB_EDID },
	{ "set", GPU_DISPLAY_VERB_SET },
	{ "keep", GPU_DISPLAY_VERB_KEEP },
	{ "revert", GPU_DISPLAY_VERB_REVERT },
	{ "reset", GPU_DISPLAY_VERB_RESET }
};

/* The sections that follow vulkan. */
static const struct gpu_word gpu_vulkan_words[] = {
	{ "properties", GPU_VULKAN_PROPERTIES },
	{ "features", GPU_VULKAN_FEATURES },
	{ "limits", GPU_VULKAN_LIMITS },
	{ "formats", GPU_VULKAN_FORMATS },
	{ "extensions", GPU_VULKAN_EXTENSIONS },
	{ "queues", GPU_VULKAN_QUEUES }
};

/* The APIs that follow gl. */
static const struct gpu_word gpu_gl_words[] = {
	{ "es", GPU_GL_ES },
	{ "desktop", GPU_GL_DESKTOP }
};

/*
 * The text of gpu help, one line each: the grammar, then the names, the
 * rules and the exit status.
 */
static const char *const gpu_help_lines[] = {
	"usage: gpu [--porcelain]                    list the GPU nodes",
	"       gpu [--porcelain] GPU [show]         one node's summary",
	"       gpu [--porcelain] GPU caps [--capset=ID]",
	"       gpu [--porcelain] GPU display [list]",
	"       gpu [--porcelain] GPU display modes DISPLAY",
	"       gpu [--porcelain] GPU display edid DISPLAY [--raw]",
	"       gpu [--porcelain] GPU display set DISPLAY WIDTHxHEIGHT[@HZ] [--keep] [--timeout=S] [--session=SOCKET]",
	"       gpu [--porcelain] GPU display keep|revert|reset DISPLAY [--keep] [--timeout=S] [--session=SOCKET]",
	"       gpu [--porcelain] GPU vulkan [properties|features|limits|formats|extensions|queues]",
	"       gpu [--porcelain] GPU gl [es|desktop]",
	"       gpu [--porcelain] GPU video",
	"       gpu [--porcelain] GPU reset [--wait=MS] [--yes]",
	"       gpu [--porcelain] GPU diagnose [list | KIND [--argument=N] [--output=FILE]]",
	"       gpu help",
	"",
	"GPU is gpuN or /dev/gpuN.  DISPLAY is a display's number in the list (from 1),",
	"its name, the start of its name, or id=N, the kernel's display id.",
	"Only root may use gpu.  --porcelain prints one record a line, \"kind key=value ...\",",
	"after a first line \"porcelain version=1\".  The exit status is 0, 1 when a request",
	"is refused or fails, or 2 for a wrong use.",
	"",
	"gpu opens each node it reports on.  The open of a node whose device has failed",
	"may start the driver's recovery reset of that device."
};

static int gpu_parse_option(struct gpu_invocation *command, const char *word, unsigned *options);
static int gpu_parse_flag(struct gpu_invocation *command, const char *word, unsigned *options);
static int gpu_parse_words(struct gpu_invocation *command, const char **words, unsigned count);
static int gpu_parse_area(struct gpu_invocation *command, const char **words, unsigned count);
static int gpu_parse_display(struct gpu_invocation *command, const char **words, unsigned count);
static int gpu_parse_check_options(struct gpu_invocation *command, unsigned options);
static void gpu_parse_refuse(struct gpu_invocation *command, const char *format, ...) __attribute__((format(printf, 2, 3)));
static int gpu_parse_digits(const char *word, uint32_t max, uint32_t *value, const char **end);
static int gpu_word_lookup(const struct gpu_word *table, size_t count, const char *word);
static int gpu_run(const struct gpu_invocation *command);

/*
 * Runs one gpu command line.
 */
int
main(
	int argc,
	char **argv)
{
	struct gpu_invocation command;
	uid_t user;
	int status;

	/* Takes the line apart before anything on the machine is looked at. */
	(void)gpu_command_parse(argc, argv, &command);

	/* help is for anyone, and works whatever else the line says. */
	if (command.area == GPU_AREA_HELP) {
		gpu_help(stdout);
		return GPU_EXIT_OK;
	}

	/* Refuses everyone but root before the line is answered at all. */
	user = geteuid();
	if (user != 0) {
		fprintf(stderr, "gpu: only root may use gpu\n");
		return GPU_EXIT_FAILED;
	}

	/* Says what was wrong with the line. */
	if (command.usage) {
		fprintf(stderr, "gpu: %s\n", command.error);
		fprintf(stderr, "Run \"gpu help\" for the usage.\n");
		return GPU_EXIT_USAGE;
	}

	/* Answers the line. */
	status = gpu_run(&command);

	/* Sends what was printed out before the status is handed on. */
	fflush(stdout);

	/* The area's exit status. */
	return status;
}

/*
 * Takes a command line apart into a command.
 *
 * Options may stand anywhere on the line.  Returns 0, or -1 for a wrong use
 * with command->usage set and command->error saying why.
 */
int
gpu_command_parse(
	int argc,
	char **argv,
	struct gpu_invocation *command)
{
	const char *words[GPU_WORDS_MAX];
	unsigned word_count;
	unsigned options;
	const char *word;
	int index;
	int status;

	/* Starts from the list with nothing given. */
	memset(command, 0, sizeof(*command));
	command->area = GPU_AREA_LIST;
	command->display_verb = GPU_DISPLAY_VERB_LIST;
	command->vulkan_section = GPU_VULKAN_PROPERTIES;
	command->gl_api = GPU_GL_BOTH;

	/* Sorts the line into options and the other words. */
	word_count = 0U;
	options = 0U;
	for (index = 1; index < argc; index++) {
		word = argv[index];

		/* An option of two dashes, wherever it stands. */
		if (word[0] == '-' && word[1] == '-') {
			status = gpu_parse_option(command, word, &options);
			if (status != 0)
				return -1;
			continue;
		}

		/* A word of one dash is no option gpu has. */
		if (word[0] == '-' && word[1] != '\0') {
			gpu_parse_refuse(command, "unknown option %s", word);
			return -1;
		}

		/* Refuses a line longer than any command. */
		if (word_count == GPU_WORDS_MAX) {
			gpu_parse_refuse(command, "too many words: %s", word);
			return -1;
		}

		/* Keeps the word for the grammar. */
		words[word_count] = word;
		word_count++;
	}

	/* Reads the words as a command. */
	status = gpu_parse_words(command, words, word_count);
	if (status != 0)
		return -1;

	/* Refuses an option the command does not take. */
	status = gpu_parse_check_options(command, options);
	if (status != 0)
		return -1;

	/* Succeeded: the command is complete. */
	return 0;
}

/*
 * Reads a whole number, in decimal or in hexadecimal after 0x.
 *
 * Returns 0 with the number in value, or -1 for a word that is not one or
 * is larger than max.
 */
int
gpu_parse_number(
	const char *word,
	uint32_t max,
	uint32_t *value)
{
	unsigned long long number;
	const char *end;
	unsigned digit;
	char letter;
	int status;

	/* A decimal number is read by the digit reader, which must reach the end. */
	if (word[0] != '0' || (word[1] != 'x' && word[1] != 'X')) {
		status = gpu_parse_digits(word, max, value, &end);
		if (status != 0)
			return -1;

		/* Refuses anything after the digits. */
		if (*end != '\0')
			return -1;

		/* Succeeded: the decimal number. */
		return 0;
	}

	/* A hexadecimal number needs at least one digit after 0x. */
	word += 2;
	if (*word == '\0')
		return -1;

	/* Adds up the hexadecimal digits, stopping past max. */
	number = 0ULL;
	for (;
	     *word != '\0';
	     word++) {
		letter = *word;

		/* Works out the digit's value, refusing anything that is not one. */
		if (letter >= '0' && letter <= '9') {
			digit = (unsigned)(letter - '0');
		} else if (letter >= 'a' && letter <= 'f') {
			digit = (unsigned)(letter - 'a') + 10U;
		} else if (letter >= 'A' && letter <= 'F') {
			digit = (unsigned)(letter - 'A') + 10U;
		} else {
			return -1;
		}

		/* Refuses a number past max before it can overflow. */
		number = number * 16ULL + digit;
		if (number > max)
			return -1;
	}

	/* Succeeded: the hexadecimal number. */
	*value = (uint32_t)number;
	return 0;
}

/*
 * Reads a mode word, WIDTHxHEIGHT[@HZ], where HZ may have up to three
 * decimals.
 *
 * Returns 0 with the size and the refresh in millihertz (0 when no
 * refresh was named), or -1 for a word that is not a mode.
 */
int
gpu_parse_mode(
	const char *word,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_mhz)
{
	const char *end;
	uint32_t hertz;
	uint32_t fraction;
	unsigned decimals;
	int status;

	/* The width, which must not be 0 and must be followed by x. */
	status = gpu_parse_digits(word, GPU_MODE_SIZE_MAX, width, &end);
	if (status != 0)
		return -1;
	if (*end != 'x' || *width == 0U)
		return -1;

	/* The height, which must not be 0. */
	status = gpu_parse_digits(end + 1, GPU_MODE_SIZE_MAX, height, &end);
	if (status != 0)
		return -1;
	if (*height == 0U)
		return -1;

	/* A mode without a refresh leaves the choice to the compositor. */
	if (*end == '\0') {
		*refresh_mhz = 0U;
		return 0;
	}

	/* Anything else after the height must be the refresh. */
	if (*end != '@')
		return -1;

	/* The whole hertz. */
	status = gpu_parse_digits(end + 1, GPU_MODE_HZ_MAX, &hertz, &end);
	if (status != 0)
		return -1;

	/* The decimals after a point, at most three. */
	fraction = 0U;
	decimals = 0U;
	if (*end == '.') {
		end++;

		/* Reads each decimal, refusing a fourth. */
		while (*end >= '0' && *end <= '9') {
			if (decimals == 3U)
				return -1;
			fraction = fraction * 10U + (uint32_t)(*end - '0');
			decimals++;
			end++;
		}

		/* A point needs a decimal after it. */
		if (decimals == 0U)
			return -1;
	}

	/* Scales the decimals up to thousandths. */
	while (decimals < 3U) {
		fraction = fraction * 10U;
		decimals++;
	}

	/* Refuses anything after the refresh. */
	if (*end != '\0')
		return -1;

	/* Refuses a refresh of nothing. */
	if (hertz == 0U && fraction == 0U)
		return -1;

	/* Succeeded: the size and the refresh in millihertz. */
	*refresh_mhz = hertz * 1000U + fraction;
	return 0;
}

/*
 * Prints how gpu is used.
 */
void
gpu_help(
	FILE *stream)
{
	size_t index;

	/* Prints the text one line at a time. */
	for (index = 0U; index < sizeof(gpu_help_lines) / sizeof(gpu_help_lines[0]); index++) {
		fputs(gpu_help_lines[index], stream);
		fputc('\n', stream);
	}
}

/* Reads one option word into the command and marks it seen; 0, or -1 for a wrong use. */
static int
gpu_parse_option(
	struct gpu_invocation *command,
	const char *word,
	unsigned *options)
{
	const char *value;
	int status;
	int same;

	/* The options without a value. */
	status = gpu_parse_flag(command, word, options);
	if (status == 0)
		return 0;

	/* --capset=ID: one capability set's bytes. */
	same = strncmp(word, "--capset=", 9U);
	if (same == 0) {
		value = word + 9;
		status = gpu_parse_number(value, UINT32_MAX, &command->capset_id);
		if (status != 0) {
			gpu_parse_refuse(command, "--capset takes a number: %s", word);
			return -1;
		}
		command->capset_given = 1U;
		*options |= GPU_OPTION_CAPSET;
		return 0;
	}

	/* --timeout=S: how long a trial waits for a keep. */
	same = strncmp(word, "--timeout=", 10U);
	if (same == 0) {
		value = word + 10;
		status = gpu_parse_number(value, GPU_TIMEOUT_MAX_S, &command->timeout_s);
		if (status != 0 || command->timeout_s == 0U) {
			gpu_parse_refuse(command, "--timeout takes seconds from 1 to %u: %s", GPU_TIMEOUT_MAX_S, word);
			return -1;
		}
		command->timeout_given = 1U;
		*options |= GPU_OPTION_TIMEOUT;
		return 0;
	}

	/* --session=SOCKET: the desktop session's socket. */
	same = strncmp(word, "--session=", 10U);
	if (same == 0) {
		value = word + 10;
		if (*value == '\0') {
			gpu_parse_refuse(command, "--session takes a socket's path");
			return -1;
		}
		command->session = value;
		*options |= GPU_OPTION_SESSION;
		return 0;
	}

	/* --wait=MS: how long a reset may wait for the device. */
	same = strncmp(word, "--wait=", 7U);
	if (same == 0) {
		value = word + 7;
		status = gpu_parse_number(value, GPU_DEVICE_RESET_WAIT_MAX_MS, &command->wait_ms);
		if (status != 0) {
			gpu_parse_refuse(command, "--wait takes milliseconds up to %u: %s", GPU_DEVICE_RESET_WAIT_MAX_MS, word);
			return -1;
		}
		command->wait_given = 1U;
		*options |= GPU_OPTION_WAIT;
		return 0;
	}

	/* --argument=N: the number a report kind takes. */
	same = strncmp(word, "--argument=", 11U);
	if (same == 0) {
		value = word + 11;
		status = gpu_parse_number(value, UINT32_MAX, &command->argument);
		if (status != 0) {
			gpu_parse_refuse(command, "--argument takes a number: %s", word);
			return -1;
		}
		command->argument_given = 1U;
		*options |= GPU_OPTION_ARGUMENT;
		return 0;
	}

	/* --output=FILE: where a report's bytes go. */
	same = strncmp(word, "--output=", 9U);
	if (same == 0) {
		value = word + 9;
		if (*value == '\0') {
			gpu_parse_refuse(command, "--output takes a file's path");
			return -1;
		}
		command->output = value;
		*options |= GPU_OPTION_OUTPUT;
		return 0;
	}

	/* Anything else. */
	gpu_parse_refuse(command, "unknown option %s", word);
	return -1;
}

/* Reads an option without a value into the command; 0, or -1 when the word is none of them. */
static int
gpu_parse_flag(
	struct gpu_invocation *command,
	const char *word,
	unsigned *options)
{
	int same;

	/* --porcelain goes with every command. */
	same = strcmp(word, "--porcelain");
	if (same == 0) {
		command->porcelain = 1U;
		return 0;
	}

	/* --raw: the EDID's bytes rather than their meaning. */
	same = strcmp(word, "--raw");
	if (same == 0) {
		command->raw = 1U;
		*options |= GPU_OPTION_RAW;
		return 0;
	}

	/* --keep: a new mode kept at once, without its trial. */
	same = strcmp(word, "--keep");
	if (same == 0) {
		command->keep = 1U;
		*options |= GPU_OPTION_KEEP;
		return 0;
	}

	/* --yes: a reset without the question. */
	same = strcmp(word, "--yes");
	if (same == 0) {
		command->yes = 1U;
		*options |= GPU_OPTION_YES;
		return 0;
	}

	/* Not an option without a value. */
	return -1;
}

/* Reads the words other than options as a command; 0, or -1 for a wrong use. */
static int
gpu_parse_words(
	struct gpu_invocation *command,
	const char **words,
	unsigned count)
{
	int status;
	int same;

	/* No word asks for the list. */
	if (count == 0U) {
		command->area = GPU_AREA_LIST;
		return 0;
	}

	/* help stands alone. */
	same = strcmp(words[0], "help");
	if (same == 0) {
		if (count != 1U) {
			gpu_parse_refuse(command, "help takes nothing more");
			return -1;
		}
		command->area = GPU_AREA_HELP;
		return 0;
	}

	/* The first word names the node. */
	status = gpu_node_parse_name(words[0], command->node, sizeof(command->node), &command->unit);
	if (status != 0) {
		gpu_parse_refuse(command, "GPU is gpuN or /dev/gpuN, not %s", words[0]);
		return -1;
	}

	/* The node alone asks for its summary. */
	if (count == 1U) {
		command->area = GPU_AREA_SHOW;
		return 0;
	}

	/* The second word names the area. */
	status = gpu_parse_area(command, words, count);
	if (status != 0)
		return -1;

	/* Succeeded: the area and its words are read. */
	return 0;
}

/* Reads the area word and the words after it; 0, or -1 for a wrong use. */
static int
gpu_parse_area(
	struct gpu_invocation *command,
	const char **words,
	unsigned count)
{
	int found;
	int status;
	int same;

	/* Names the area. */
	found = gpu_word_lookup(gpu_area_words, sizeof(gpu_area_words) / sizeof(gpu_area_words[0]), words[1]);
	if (found < 0) {
		gpu_parse_refuse(command, "no area %s (show, caps, display, vulkan, gl, video, reset, diagnose)", words[1]);
		return -1;
	}
	command->area = (enum gpu_area)found;

	/* Reads what the area takes after its word. */
	switch (command->area) {
	case GPU_AREA_DISPLAY:
		/* display has verbs of its own. */
		status = gpu_parse_display(command, words + 2, count - 2U);
		if (status != 0)
			return -1;
		break;
	case GPU_AREA_VULKAN:
		/* vulkan takes one section, properties by default. */
		if (count == 2U)
			break;
		found = -1;
		if (count == 3U)
			found = gpu_word_lookup(gpu_vulkan_words, sizeof(gpu_vulkan_words) / sizeof(gpu_vulkan_words[0]), words[2]);
		if (found < 0) {
			gpu_parse_refuse(command, "vulkan takes one of properties, features, limits, formats, extensions, queues");
			return -1;
		}
		command->vulkan_section = (enum gpu_vulkan_section)found;
		break;
	case GPU_AREA_GL:
		/* gl takes es or desktop, both by default. */
		if (count == 2U)
			break;
		found = -1;
		if (count == 3U)
			found = gpu_word_lookup(gpu_gl_words, sizeof(gpu_gl_words) / sizeof(gpu_gl_words[0]), words[2]);
		if (found < 0) {
			gpu_parse_refuse(command, "gl takes es or desktop");
			return -1;
		}
		command->gl_api = (enum gpu_gl_api)found;
		break;
	case GPU_AREA_DIAGNOSE:
		/* diagnose lists the kinds by default, or reads one; the kind is the driver's to know. */
		command->diagnose_list = 1U;
		if (count == 2U)
			break;
		if (count != 3U) {
			gpu_parse_refuse(command, "diagnose takes list or one KIND");
			return -1;
		}
		same = strcmp(words[2], "list");
		if (same != 0) {
			command->diagnose_list = 0U;
			command->diagnose_kind = words[2];
		}
		break;
	default:
		/* show, caps, video and reset take nothing more. */
		if (count != 2U) {
			gpu_parse_refuse(command, "%s takes nothing more", words[1]);
			return -1;
		}
		break;
	}

	/* Succeeded: the area and its words. */
	return 0;
}

/* Reads the words after display; 0, or -1 for a wrong use. */
static int
gpu_parse_display(
	struct gpu_invocation *command,
	const char **words,
	unsigned count)
{
	int found;
	int status;

	/* No word asks for the list. */
	if (count == 0U)
		return 0;

	/* Names the verb. */
	found = gpu_word_lookup(gpu_display_words, sizeof(gpu_display_words) / sizeof(gpu_display_words[0]), words[0]);
	if (found < 0) {
		gpu_parse_refuse(command, "no display verb %s (list, modes, edid, set, keep, revert, reset)", words[0]);
		return -1;
	}
	command->display_verb = (enum gpu_display_verb)found;

	/* list takes nothing more. */
	if (command->display_verb == GPU_DISPLAY_VERB_LIST) {
		if (count != 1U) {
			gpu_parse_refuse(command, "display list takes nothing more");
			return -1;
		}
		return 0;
	}

	/* set takes a display and a mode. */
	if (command->display_verb == GPU_DISPLAY_VERB_SET) {
		if (count != 3U) {
			gpu_parse_refuse(command, "display set takes DISPLAY and WIDTHxHEIGHT[@HZ]");
			return -1;
		}
		command->display = words[1];
		status = gpu_parse_mode(words[2], &command->width, &command->height, &command->refresh_mhz);
		if (status != 0) {
			gpu_parse_refuse(command, "a mode is WIDTHxHEIGHT[@HZ], not %s", words[2]);
			return -1;
		}
		return 0;
	}

	/* The other verbs take one display. */
	if (count != 2U) {
		gpu_parse_refuse(command, "display %s takes one DISPLAY", words[0]);
		return -1;
	}

	/* Succeeded: the verb and its display. */
	command->display = words[1];
	return 0;
}

/* Refuses an option the command does not take; 0, or -1 for a wrong use. */
static int
gpu_parse_check_options(
	struct gpu_invocation *command,
	unsigned options)
{
	unsigned allowed;

	/* Works out the options the command takes. */
	allowed = 0U;
	if (command->area == GPU_AREA_CAPS) {
		/* caps shows one capability set on request. */
		allowed = GPU_OPTION_CAPSET;
	} else if (command->area == GPU_AREA_DISPLAY && command->display_verb == GPU_DISPLAY_VERB_EDID) {
		/* edid prints the bytes on request. */
		allowed = GPU_OPTION_RAW;
	} else if (command->area == GPU_AREA_DISPLAY && command->display_verb >= GPU_DISPLAY_VERB_SET) {
		/* A change has its trial's options and the session's socket. */
		allowed = GPU_OPTION_KEEP | GPU_OPTION_TIMEOUT | GPU_OPTION_SESSION;
	} else if (command->area == GPU_AREA_RESET) {
		/* A reset has its wait and its answer. */
		allowed = GPU_OPTION_WAIT | GPU_OPTION_YES;
	} else if (command->area == GPU_AREA_DIAGNOSE && !command->diagnose_list) {
		/* One report has its argument and its file. */
		allowed = GPU_OPTION_ARGUMENT | GPU_OPTION_OUTPUT;
	}

	/* Refuses the options outside that set. */
	if ((options & ~allowed) != 0U) {
		gpu_parse_refuse(command, "an option is given that this command does not take");
		return -1;
	}

	/* Succeeded: every option belongs to the command. */
	return 0;
}

/* Marks the command a wrong use and keeps the reason for the caller to print. */
static void
gpu_parse_refuse(
	struct gpu_invocation *command,
	const char *format,
	...)
{
	va_list arguments;

	/* Writes the reason. */
	va_start(arguments, format);
	(void)vsnprintf(command->error, sizeof(command->error), format, arguments);
	va_end(arguments);

	/* The line is a wrong use. */
	command->usage = 1U;
}

/*
 * Reads the decimal digits at the start of a word, up to max; 0 with the
 * number and the first byte after the digits, or -1 for no digit or a
 * number past max.
 */
static int
gpu_parse_digits(
	const char *word,
	uint32_t max,
	uint32_t *value,
	const char **end)
{
	unsigned long long number;
	unsigned digits;

	/* Adds up the digits, stopping past max before an overflow. */
	number = 0ULL;
	digits = 0U;
	while (*word >= '0' && *word <= '9') {
		number = number * 10ULL + (unsigned long long)(*word - '0');
		if (number > max)
			return -1;
		digits++;
		word++;
	}

	/* A number needs a digit. */
	if (digits == 0U)
		return -1;

	/* Succeeded: the number and where it ended. */
	*value = (uint32_t)number;
	*end = word;
	return 0;
}

/* Finds a word in a table of the grammar; its value, or -1 when it is not there. */
static int
gpu_word_lookup(
	const struct gpu_word *table,
	size_t count,
	const char *word)
{
	size_t index;
	int same;

	/* Compares the word with each entry. */
	for (index = 0U; index < count; index++) {
		same = strcmp(table[index].word, word);
		if (same == 0)
			return table[index].value;
	}

	/* Not a word of the table. */
	return -1;
}

/* Hands the run to the file of the area asked for; the exit status. */
static int
gpu_run(
	const struct gpu_invocation *command)
{
	int status;

	/* The first line of the porcelain form names its version. */
	if (command->porcelain)
		gpu_porcelain_begin();

	/* Runs the area. */
	switch (command->area) {
	case GPU_AREA_LIST:
		status = gpu_node_list(command);
		break;
	case GPU_AREA_SHOW:
		status = gpu_node_show(command);
		break;
	case GPU_AREA_CAPS:
		status = gpu_node_caps(command);
		break;
	case GPU_AREA_DISPLAY:
		status = gpu_display_run(command);
		break;
	case GPU_AREA_VULKAN:
		status = gpu_vulkan_run(command);
		break;
	case GPU_AREA_GL:
		status = gpu_gl_run(command);
		break;
	case GPU_AREA_VIDEO:
		status = gpu_video_run(command);
		break;
	case GPU_AREA_RESET:
		status = gpu_service_reset(command);
		break;
	case GPU_AREA_DIAGNOSE:
		status = gpu_service_diagnose(command);
		break;
	default:
		status = GPU_EXIT_USAGE;
		break;
	}

	/* The area's exit status. */
	return status;
}
