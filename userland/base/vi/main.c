/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The vi program: a small screen editor written for zedBSD.
 *
 * This file reads the command line, sets up the terminal and the
 * signals, and runs the loop that takes one command at a time.
 */

#include "userland/base/vi/vi.h"

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The signals that arrived since they were last handled.
 *
 * The handlers only set these; the main loop sees them when a read of the
 * terminal is cut short, and does the work outside the handler.
 */
static volatile sig_atomic_t vi_resize_requested;
static volatile sig_atomic_t vi_continue_requested;
static volatile sig_atomic_t vi_suspend_requested;
static volatile sig_atomic_t vi_stop_requested;

/*
 * The editing session, which is large and lives for the whole run.  The
 * exit handler reaches the terminal through it to give the terminal back.
 */
static struct vi_editor vi_session;

static int parse_arguments(int argc, char **argv, const char **path, const char **start);
static void usage(void);
static int start_editor(struct vi_editor *editor, const char *path, const char *start);
static void place_start(struct vi_editor *editor, const char *start);
static void install_handlers(struct vi_terminal *terminal);
static void install_handler(int number, void (*handler)(int));
static void restore_at_exit(void);
static void resize_handler(int number);
static void continue_handler(int number);
static void suspend_handler(int number);
static void stop_handler(int number);

/*
 * Edits the file named on the command line.
 */
int
main(
	int argc,
	char **argv)
{
	struct vi_editor *editor;
	const char *path;
	const char *start;
	int modified;
	int status;
	int key;

	/* Uses the one session this program has. */
	editor = &vi_session;

	/* Text is UTF-8 whatever the environment says; this gives wcwidth its tables. */
	(void)setlocale(LC_CTYPE, "C.UTF-8");

	/* Reads the options and the file name. */
	status = parse_arguments(argc, argv, &path, &start);
	if (status != 0)
		return status;

	/* Reads the file and readies the terminal. */
	status = start_editor(editor, path, start);
	if (status != 0)
		return status;

	/* Runs one command at a time until a command or a signal ends the session. */
	while (!editor->quit) {
		/* Starts recording the keys of the next command for the . command. */
		editor->record.length = 0;
		editor->record.count_end = 0;
		editor->record.overflow = 0;
		editor->recording = 1;
		editor->changed = 0;

		/* Waits for the command's first key; the message shown so far then goes. */
		key = vi_editor_key(editor);
		if (key == VI_KEY_STOP)
			break;
		editor->message[0] = '\0';

		/* Runs the command as one change for u. */
		(void)vi_undo_begin(editor);
		vi_command(editor, key);
		vi_undo_end(editor);
		vi_command_finish(editor);
	}

	/* Gives the terminal back. */
	vi_terminal_leave(&editor->terminal);

	/* A session cut short by a hangup or a termination keeps unwritten changes in a recovery file. */
	if (editor->exit_status != 0) {
		modified = vi_undo_modified(editor);
		if (modified)
			vi_file_recover(editor);
	}

	/* Releases the session. */
	vi_screen_free(editor);
	vi_undo_free(&editor->undo);
	vi_buffer_free(&editor->buffer);
	free(editor->yank.text);
	free(editor->file_name);
	if (editor->search_compiled)
		regfree(&editor->search_regex);

	/* Succeeded: 0 after a quit, 1 after a hangup or a termination. */
	return editor->exit_status;
}

/*
 * Handles the signals that arrived while a key was awaited.
 *
 * Returns 1 when the editor must stop, or 0 to go on waiting.
 */
int
vi_signal_pending(
	struct vi_editor *editor)
{
	int error;

	/* A hangup or a termination ends the session; unwritten changes go to a recovery file. */
	if (vi_stop_requested) {
		editor->quit = 1;
		editor->exit_status = 1;
		return 1;
	}

	/* A stop from outside suspends the editor as CONTROL-Z does. */
	if (vi_suspend_requested) {
		vi_suspend_requested = 0;
		vi_suspend(editor);
	}

	/* After a continue the shell may have changed the modes, so they are set again. */
	if (vi_continue_requested) {
		vi_continue_requested = 0;
		error = vi_terminal_enter(&editor->terminal);
		if (error != 0) {
			editor->quit = 1;
			editor->exit_status = 1;
			return 1;
		}

		/* The terminal may have changed size while the editor was stopped. */
		vi_resize_requested = 1;
	}

	/* A new window size redraws everything. */
	if (vi_resize_requested) {
		vi_resize_requested = 0;
		vi_terminal_measure(&editor->terminal);
		vi_terminal_command(&editor->terminal, editor->terminal.clear_string);
		vi_screen_invalidate(editor);
	}

	/* Goes on waiting for a key. */
	return 0;
}

/*
 * Stops the editor until the shell continues it, giving the terminal back
 * in between.
 */
void
vi_suspend(
	struct vi_editor *editor)
{
	sigset_t stop_set;
	int error;

	/* The shell gets the terminal as it was before vi. */
	vi_terminal_leave(&editor->terminal);

	/*
	 * Stops the whole process group with the default action, so that a
	 * program waiting for vi, such as one that started it as an editor,
	 * stops with it and the shell gets control.
	 */
	install_handler(SIGTSTP, SIG_DFL);
	(void)sigemptyset(&stop_set);
	(void)sigaddset(&stop_set, SIGTSTP);
	(void)sigprocmask(SIG_UNBLOCK, &stop_set, NULL);
	(void)kill(0, SIGTSTP);

	/* Running again: blocks the stop signal again, takes the terminal back and draws everything. */
	(void)sigprocmask(SIG_BLOCK, &stop_set, NULL);
	install_handler(SIGTSTP, suspend_handler);
	vi_continue_requested = 0;
	error = vi_terminal_enter(&editor->terminal);
	if (error != 0) {
		editor->quit = 1;
		editor->exit_status = 1;
		return;
	}

	/* Draws everything again, on a terminal that may have changed size. */
	vi_terminal_measure(&editor->terminal);
	vi_terminal_command(&editor->terminal, editor->terminal.clear_string);
	vi_screen_invalidate(editor);
}

/* Reads the command line: [+number | +] [--] [file]. */
static int
parse_arguments(
	int argc,
	char **argv,
	const char **path,
	const char **start)
{
	int index;
	int options_done;
	int compare;

	/* Starts with no file, no start place, and options still allowed. */
	*path = NULL;
	*start = NULL;
	options_done = 0;

	/* Reads each argument in turn. */
	for (index = 1; index < argc; index++) {
		/* +N and + say where to start; -- ends the options; any other option is unknown. */
		if (!options_done) {
			if (argv[index][0] == '+') {
				*start = argv[index] + 1;
				continue;
			}

			/* The two hyphens end the options, so a file name may begin with a hyphen. */
			compare = strcmp(argv[index], "--");
			if (compare == 0) {
				options_done = 1;
				continue;
			}

			/* Refuses an option vi does not have. */
			if (argv[index][0] == '-' && argv[index][1] != '\0') {
				usage();
				return 2;
			}
		}

		/* Only one file can be edited at a time. */
		if (*path != NULL) {
			(void)fprintf(stderr, "vi: only one file can be edited at a time\n");
			return 2;
		}

		/* Takes the argument as the file to edit. */
		*path = argv[index];
	}

	/* Succeeded. */
	return 0;
}

/* Writes the usage line. */
static void
usage(
	void)
{
	/* The one form vi accepts. */
	(void)fprintf(stderr, "usage: vi [+line | +] [file]\n");
}

/* Reads the file, readies the terminal and sets the signals.  Returns the exit status on failure. */
static int
start_editor(
	struct vi_editor *editor,
	const char *path,
	const char *start)
{
	int error;

	/* An empty session in command mode. */
	memset(editor, 0, sizeof(*editor));
	(void)vi_buffer_init(&editor->buffer);
	vi_undo_init(&editor->undo);
	editor->mode = VI_MODE_COMMAND;

	/* The terminal must be one vi can draw on. */
	error = vi_terminal_open(&editor->terminal);
	if (error == ENOTTY) {
		(void)fprintf(stderr, "vi: standard input and output must be a terminal\n");
		return 1;
	}

	/* A terminal that cannot move the cursor cannot show a screen editor. */
	if (error == ENOTSUP) {
		(void)fprintf(stderr, "vi: terminal '%s' cannot move the cursor\n", getenv("TERM"));
		return 1;
	}

	/* Any other failure to open the terminal is reported as the system words it. */
	if (error != 0) {
		(void)fprintf(stderr, "vi: %s\n", strerror(error));
		return 1;
	}

	/* Reads the file, or starts with one empty line. */
	if (path != NULL) {
		editor->file_name = strdup(path);
		if (editor->file_name == NULL) {
			(void)fprintf(stderr, "vi: %s\n", strerror(ENOMEM));
			return 1;
		}

		/* Reads the file into the buffer; the message says what went wrong. */
		error = vi_file_load(editor, path);
		if (error != 0) {
			(void)fprintf(stderr, "vi: %s\n", editor->message);
			return 1;
		}
	} else {
		error = vi_buffer_append_line(&editor->buffer, "", 0);
		if (error != 0) {
			(void)fprintf(stderr, "vi: %s\n", strerror(error));
			return 1;
		}
	}

	/* Puts the cursor where +line asked. */
	place_start(editor, start);

	/* Gives the terminal back however vi ends, and takes the signals vi handles. */
	(void)atexit(restore_at_exit);
	install_handlers(&editor->terminal);

	/* Puts the terminal into the editing mode. */
	error = vi_terminal_enter(&editor->terminal);
	if (error != 0) {
		vi_terminal_leave(&editor->terminal);
		(void)fprintf(stderr, "vi: %s\n", strerror(error));
		return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Moves the cursor to the line +N names, or to the last line for a bare +. */
static void
place_start(
	struct vi_editor *editor,
	const char *start)
{
	unsigned long number;

	/* Without + the session starts on the first line. */
	if (start == NULL)
		return;

	/* A bare + starts on the last line; +N on line N, held to the buffer. */
	number = (unsigned long)editor->buffer.count;
	if (start[0] != '\0')
		number = strtoul(start, NULL, 10);
	if (number == 0)
		number = 1;
	if (number > (unsigned long)editor->buffer.count)
		number = (unsigned long)editor->buffer.count;

	/* Puts the cursor on that line, at its first non-blank character. */
	editor->cursor.line = (size_t)number - 1;
	vi_cursor_first_nonblank(editor);
}

/*
 * Sets the handlers of the signals vi takes, and blocks those signals
 * except while waiting for a key, so that none can arrive between the
 * check of what arrived and the wait.
 */
static void
install_handlers(
	struct vi_terminal *terminal)
{
	sigset_t handled;

	/* The window size, a continue, a stop, and the signals that end the session. */
	install_handler(SIGWINCH, resize_handler);
	install_handler(SIGCONT, continue_handler);
	install_handler(SIGTSTP, suspend_handler);
	install_handler(SIGHUP, stop_handler);
	install_handler(SIGTERM, stop_handler);
	install_handler(SIGINT, stop_handler);
	install_handler(SIGQUIT, stop_handler);

	/* Blocks them, keeping the mask from before for the waits. */
	(void)sigemptyset(&handled);
	(void)sigaddset(&handled, SIGWINCH);
	(void)sigaddset(&handled, SIGCONT);
	(void)sigaddset(&handled, SIGTSTP);
	(void)sigaddset(&handled, SIGHUP);
	(void)sigaddset(&handled, SIGTERM);
	(void)sigaddset(&handled, SIGINT);
	(void)sigaddset(&handled, SIGQUIT);
	(void)sigprocmask(SIG_BLOCK, &handled, &terminal->wait_mask);
	terminal->wait_mask_set = 1;
}

/*
 * Sets one signal's handler without SA_RESTART, so a signal cuts a read
 * of the terminal short and the main loop sees it.
 */
static void
install_handler(
	int number,
	void (*handler)(int))
{
	struct sigaction action;

	/* No restart, and no other signals held while the handler runs. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = handler;
	(void)sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	(void)sigaction(number, &action, NULL);
}

/* Gives the terminal back when the program exits by any path. */
static void
restore_at_exit(
	void)
{
	/* Leaving twice is harmless; only the first call changes the terminal. */
	vi_terminal_leave(&vi_session.terminal);
}

/* Notes a new window size. */
static void
resize_handler(
	int number)
{
	UNUSED_PARAMETER(number);

	/* Tells the main loop to measure the window and draw again. */
	vi_resize_requested = 1;
}

/* Notes that the editor runs again after a stop. */
static void
continue_handler(
	int number)
{
	UNUSED_PARAMETER(number);

	/* Tells the main loop to set the terminal modes again. */
	vi_continue_requested = 1;
}

/* Notes a request to stop from outside. */
static void
suspend_handler(
	int number)
{
	UNUSED_PARAMETER(number);

	/* Tells the main loop to give the terminal back and stop. */
	vi_suspend_requested = 1;
}

/* Notes a hangup or a termination. */
static void
stop_handler(
	int number)
{
	UNUSED_PARAMETER(number);

	/* Tells the main loop to end the session and keep unwritten changes. */
	vi_stop_requested = 1;
}
