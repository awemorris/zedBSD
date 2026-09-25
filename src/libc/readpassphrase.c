/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements asking at the terminal for something the screen must not keep.
 *
 * The prompt and the answer go to and come from the controlling terminal
 * rather than standard input, so that a program whose input was redirected
 * still reaches the person at the keyboard.  Echo is turned off for the
 * read and restored afterwards, including when a signal ends the read: a
 * terminal left without echo is the failure this has to avoid.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <readpassphrase.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

/* The signal that interrupted the read, for re-raising once it is safe. */
static volatile sig_atomic_t interrupting_signal;

/* The signals that must not leave the terminal as this call set it. */
static const int caught_signals[] = {
	SIGALRM, SIGHUP, SIGINT, SIGPIPE, SIGQUIT, SIGTERM,
	SIGTSTP, SIGTTIN, SIGTTOU
};

#define CAUGHT_COUNT (sizeof(caught_signals) / sizeof(caught_signals[0]))

/*
 * Supports the note signal operation.
 *
 * Records the signal so that the read can unwind and restore the terminal
 * before letting the signal have its usual effect.
 */
static void
note_signal(
	int number)
{
	interrupting_signal = number;
}

/*
 * Implements the readpassphrase operation.
 */
char *
readpassphrase(
	const char *prompt,
	char *buffer,
	size_t size,
	int flags)
{
	struct sigaction action;
	struct sigaction saved[CAUGHT_COUNT];
	struct termios original;
	struct termios quiet;
	unsigned char byte;
	size_t used;
	size_t index;
	ssize_t amount;
	int input;
	int output;
	int opened;
	int have_terminal;
	int saved_errno;

	/* Rejects a destination that could not hold even a terminator. */
	if (buffer == NULL || size == 0U) {
		errno = EINVAL;

		/* Reports operation failure. */
		return NULL;
	}
	buffer[0] = '\0';
	interrupting_signal = 0;
	opened = 0;

	/*
	 * The terminal is the one the person is at, which is not always the
	 * program's standard input.
	 */
	input = STDIN_FILENO;
	output = STDERR_FILENO;
	if ((flags & RPP_STDIN) == 0) {
		int terminal = open("/dev/tty", O_RDWR);

		if (terminal >= 0) {
			input = terminal;
			output = terminal;
			opened = 1;
		} else if ((flags & RPP_REQUIRE_TTY) != 0) {
			errno = ENOTTY;

			/* Reports operation failure. */
			return NULL;
		}
	}

	/*
	 * Every one of these signals would otherwise leave the terminal
	 * silent for whatever runs next.
	 */
	memset(&action, 0, sizeof(action));
	action.sa_handler = note_signal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	for (index = 0; index < CAUGHT_COUNT; index++)
		(void)sigaction(caught_signals[index], &action, &saved[index]);

	/* Turns off echo, remembering how the terminal was set. */
	have_terminal = tcgetattr(input, &original) == 0;
	if (have_terminal) {
		quiet = original;
		if ((flags & RPP_ECHO_ON) == 0)
			quiet.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
		(void)tcsetattr(input, TCSAFLUSH, &quiet);
	} else if ((flags & RPP_REQUIRE_TTY) != 0) {
		saved_errno = ENOTTY;
		goto restore;
	}

	/* Shows the prompt where the answer will be typed. */
	if (prompt != NULL)
		(void)write(output, prompt, strlen(prompt));

	/* Process each remaining element. */
	used = 0;
	saved_errno = 0;
	for (;;) {
		amount = read(input, &byte, 1);

		/* Stops at the end of the input. */
		if (amount == 0)
			break;

		/* Handles a failed read operation. */
		if (amount < 0) {
			if (errno == EINTR && interrupting_signal == 0)
				continue;
			saved_errno = errno;
			break;
		}

		/* Stops at the end of the line, which is not part of it. */
		if (byte == '\n' || byte == '\r')
			break;

		/* Applies the foldings the caller asked for. */
		if ((flags & RPP_SEVENBIT) != 0)
			byte &= 0x7fU;
		if ((flags & RPP_FORCELOWER) != 0 && isupper(byte))
			byte = (unsigned char)tolower(byte);
		if ((flags & RPP_FORCEUPPER) != 0 && islower(byte))
			byte = (unsigned char)toupper(byte);

		/* Keeps what fits, and reads past the rest of the line. */
		if (used + 1U < size)
			buffer[used++] = (char)byte;
	}
	buffer[used] = '\0';

	/* The newline the person typed was never echoed, so write one. */
	if (have_terminal && (flags & RPP_ECHO_ON) == 0)
		(void)write(output, "\n", 1);

restore:

	/* Puts the terminal back before anything else can use it. */
	if (have_terminal)
		(void)tcsetattr(input, TCSAFLUSH, &original);

	/* Process each remaining element. */
	for (index = 0; index < CAUGHT_COUNT; index++)
		(void)sigaction(caught_signals[index], &saved[index], NULL);

	/* Handles the opened condition. */
	if (opened)
		(void)close(input);

	/*
	 * The signal was held off only long enough to restore the terminal;
	 * it now has whatever effect it would have had.
	 */
	if (interrupting_signal != 0) {
		(void)raise((int)interrupting_signal);
		errno = EINTR;
		explicit_bzero(buffer, size);

		/* Reports operation failure. */
		return NULL;
	}

	/* Handles a failed read operation. */
	if (saved_errno != 0) {
		errno = saved_errno;
		explicit_bzero(buffer, size);

		/* Reports operation failure. */
		return NULL;
	}

	/* Returns the computed result. */
	return buffer;
}
