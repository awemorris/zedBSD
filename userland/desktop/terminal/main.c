/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * terminal: a VT100 terminal in a Wayland window, drawn with Vulkan.
 *
 * It runs a shell (or a command) on a pseudo-terminal for each tab
 * (connection-process.c), shows what it writes and sends it the keys
 * typed (run.c).  The window follows the size the compositor gives it, and
 * the shell is told the grid's new size.  The terminal ends when the last
 * shell exits or the window is closed.  New Window starts another
 * terminal, the way this one was run.
 *
 * Every outcome is one line on standard output: ZTERM DONE on a normal end
 * (with the reason), ZTERM FAILED naming what failed otherwise.
 */

#include "run.h"
#include "connection.h"

#include "userland/desktop/paths.h"

#include "include/kern/os-name.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The font the terminal uses unless told otherwise, and its size in pixels. */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland-mono.ttf"
#define MAIN_FONT_PIXELS	16U

/* The font that draws what the monospaced font has no glyph for (CJK), unless told otherwise. */
#define MAIN_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The grid the window opens with. */
#define MAIN_COLUMNS		90U
#define MAIN_ROWS		28U

/* The program started for a new window when the terminal was not run by a path. */
#define MAIN_PROGRAM		KEILAND_BINDIR "/terminal"

/* What Help > About Terminal writes on the screen. */
#define MAIN_ABOUT		"\r\n\033[1mTerminal\033[0m: a VT100 terminal for " ZEDBSD_OS_NAME ", drawn with Vulkan.\r\n" \
				"Its menus are drawn by the System Menu (xdg_toplevel_menu_v1).\r\n"

/*
 * What the command line asked for.
 */
struct main_options {
	const char *program;
	const char *display;
	const char *font;
	const char *fallback;
	const char *command;
	const char *token;
	unsigned pixels;
	int pixels_given;
	unsigned columns;
	unsigned rows;
	unsigned timeout;
};

static int main_parse(int argc, char **argv, struct main_options *options);
static const char *main_value(const char *argument, const char *name);
static int main_number(const char *text, unsigned maximum, unsigned *value);
static void main_new_window(void *data, struct kl_app *app, struct kl_window *from);
static void main_ended(void *data, const struct terminal_run_result *result);

/*
 * Runs the terminal.
 */
int
main(
	int argc,
	char **argv)
{
	struct main_options options;
	struct terminal_process_connector connector;
	struct terminal_run_options run;
	struct terminal_run_result result;
	int status;

	/* The command line. */
	status = main_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: terminal [--display=NAME] [--font=PATH] [--fallback-font=PATH] [--font-size=PIXELS] [--columns=N] [--rows=N] [--command=COMMAND] [--token=NAME] [--timeout-s=N]\n");
		return 2;
	}

	/* A write to a shell that has gone must not kill the terminal. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* Each tab a shell on a pseudo-terminal, or the shell running the command. */
	terminal_process_connector_init(&connector, options.command);

	/* The terminal's window, fonts and grid, drawn with its own Vulkan. */
	memset(&run, 0, sizeof(run));
	run.application = "terminal";
	run.display = options.display;
	run.title = "Terminal";
	run.present = KL_PRESENT_NONE;
	run.font = options.font;
	run.companion = KEILAND_FONT_FALLBACK_MONO;
	run.fallback = options.fallback;
	run.pixels = options.pixels;
	run.pixels_given = options.pixels_given;
	run.scale = 96U;
	run.columns = options.columns;
	run.rows = options.rows;
	run.timeout = options.timeout;
	run.token = options.token;
	run.about = MAIN_ABOUT;
	run.connector = &connector.connector;
	run.new_window = main_new_window;
	run.ended = main_ended;
	run.data = &options;

	/* The terminal runs until it ends; main_ended says how. */
	status = terminal_run(&run, &result);
	if (status != 0)
		return 1;

	/* Succeeded: the terminal ended normally. */
	return 0;
}

/* Reads the command line into the options; returns nonzero for a malformed one. */
static int
main_parse(
	int argc,
	char **argv,
	struct main_options *options)
{
	const char *value;
	const char *slash;
	int index;
	int status;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->program = MAIN_PROGRAM;
	options->display = NULL;
	options->font = MAIN_FONT;
	options->fallback = MAIN_FALLBACK_FONT;
	options->command = NULL;
	options->token = "term";
	options->pixels = MAIN_FONT_PIXELS;
	options->columns = MAIN_COLUMNS;
	options->rows = MAIN_ROWS;
	options->timeout = 0U;

	/* A program run by a path is run by the same path for a new window (a bare name is looked up as MAIN_PROGRAM). */
	slash = NULL;
	if (argc > 0 && argv[0] != NULL)
		slash = strchr(argv[0], '/');

	/* The path it was run by. */
	if (slash != NULL)
		options->program = argv[0];

	/* Each option in turn; the first name that matches takes it. */
	for (index = 1; index < argc; index++) {
		/* The compositor's socket. */
		value = main_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The font file. */
		value = main_value(argv[index], "--font=");
		if (value != NULL) {
			options->font = value;
			continue;
		}

		/* The fallback font file. */
		value = main_value(argv[index], "--fallback-font=");
		if (value != NULL) {
			options->fallback = value;
			continue;
		}

		/* The font's size in pixels. */
		value = main_value(argv[index], "--font-size=");
		if (value != NULL) {
			status = main_number(value, 96U, &options->pixels);
			if (status != 0)
				return -1;
			options->pixels_given = 1;
			continue;
		}

		/* The grid the window opens with. */
		value = main_value(argv[index], "--columns=");
		if (value != NULL) {
			status = main_number(value, TERMINAL_MAX_COLUMNS, &options->columns);
			if (status != 0)
				return -1;
			continue;
		}

		/* The rows the window opens with. */
		value = main_value(argv[index], "--rows=");
		if (value != NULL) {
			status = main_number(value, TERMINAL_MAX_ROWS, &options->rows);
			if (status != 0)
				return -1;
			continue;
		}

		/* A command run by the shell in place of an interactive shell. */
		value = main_value(argv[index], "--command=");
		if (value != NULL) {
			options->command = value;
			continue;
		}

		/* The name of the run in the log lines. */
		value = main_value(argv[index], "--token=");
		if (value != NULL) {
			options->token = value;
			continue;
		}

		/* A deadline in seconds, 0 for none. */
		value = main_value(argv[index], "--timeout-s=");
		if (value != NULL) {
			status = main_number(value, 86400U, &options->timeout);
			if (status != 0)
				return -1;
			continue;
		}

		/* An unknown option refuses the command line. */
		return -1;
	}

	/* A font too small, or an empty grid, is refused. */
	if (options->pixels < 6U || options->columns == 0U || options->rows == 0U)
		return -1;

	/* Succeeded: the options. */
	return 0;
}

/* Returns what follows an option's name in an argument, or NULL when the argument is another option. */
static const char *
main_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int differs;

	/* The argument must start with the name, equals sign included. */
	length = strlen(name);
	differs = strncmp(argument, name, length);
	if (differs != 0)
		return NULL;

	/* Reports the value after the name. */
	return argument + length;
}

/* Reads a decimal number no larger than a maximum; returns nonzero when it is not one. */
static int
main_number(
	const char *text,
	unsigned maximum,
	unsigned *value)
{
	unsigned long parsed;
	char *end;

	/* A number with nothing after it. */
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return -1;

	/* Too large a value is refused. */
	if (parsed > maximum)
		return -1;

	/* Succeeded: the value. */
	*value = (unsigned)parsed;
	return 0;
}

/*
 * Starts another terminal for a new window, the way this one was run,
 * apart from this one (it keeps nothing of this terminal's).
 */
static void
main_new_window(
	void *data,
	struct kl_app *app,
	struct kl_window *from)
{
	const struct main_options *options;
	struct kl_launch_options launch;
	const char *arguments[4];
	char token[64];
	char display[128];
	int error;

	/* The same program, its log lines named after this run's. */
	options = data;
	(void)snprintf(token, sizeof(token), "--token=%s-new", options->token);
	arguments[0] = options->program;
	arguments[1] = token;
	arguments[2] = NULL;

	/* The same display, when this one was given one. */
	if (options->display != NULL) {
		(void)snprintf(display, sizeof(display), "--display=%s", options->display);
		arguments[2] = display;
		arguments[3] = NULL;
	}

	/* The terminal, on its way; a start that failed leaves this window as it was. */
	memset(&launch, 0, sizeof(launch));
	launch.argv = arguments;
	launch.from = from;
	error = kl_launch(app, &launch);
	if (error != 0) {
		printf("ZTERM NEW-WINDOW failed run=%s errno=%d\n", options->token, error);
		fflush(stdout);
	}
}

/*
 * Says how the run ended, in one line, before its tabs and window go:
 * ZTERM DONE with the reason, or ZTERM FAILED with what failed.
 */
static void
main_ended(
	void *data,
	const struct terminal_run_result *result)
{
	const struct main_options *options;

	/* The run's line. */
	options = data;
	if (!result->failed) {
		printf("ZTERM DONE run=%s reason=%s\n", options->token, result->reason);
	} else {
		printf("ZTERM FAILED run=%s operation=%s result=%d errno=%d\n", options->token, result->operation, result->result, result->error);
	}

	/* The line reaches whoever reads the log before the terminal goes. */
	fflush(stdout);
}
