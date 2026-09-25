/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Redirections (POSIX XCU 2.7).
 *
 * A command is redirected in the shell itself, so what each descriptor held
 * is saved first, on a descriptor of 10 or more that closes on exec, and put
 * back when the command ends; a child the shell forks for the command
 * inherits the redirected descriptors.  The saves of one command form a
 * frame; frames nest as commands do.  exec with no command keeps what its
 * redirections did.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/expand.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The least descriptor a saved copy is put on, out of the way of scripts. */
#define SAVE_BASE 10

/* How long a here-document may be to be written into its pipe at once. */
#define HEREDOC_DIRECT 4096U

/*
 * What a descriptor held before a redirection: a copy of it, or -1 when it
 * was closed.  The copy closes on exec and belongs to the frame.
 */
struct saved_descriptor {
	int descriptor;
	int copy;
};

/*
 * The saves of one command's redirections.
 *
 * Pushed by sh_redirect with SH_REDIRECT_SAVE and popped by sh_redirect_pop,
 * or by a handler unwinding to its depth.
 */
struct redirect_frame {
	struct redirect_frame *previous;
	struct saved_descriptor *saved;
	int count;
	int capacity;
};

/* The frames of the commands being run, innermost first. */
static struct redirect_frame *redirect_frames;

/* How many frames there are: the depth handlers unwind to. */
static int redirect_depth;

static int apply_one(struct sh_redirection *redirection, struct redirect_frame *frame);
static int descriptor_word(const char *text);
static int apply_both_outputs(struct sh_redirection *redirection, const char *text, struct redirect_frame *frame);
static int apply_duplicate(const char *text, int target, struct redirect_frame *frame);
static void save_descriptor(struct redirect_frame *frame, int descriptor);
static int open_file(struct sh_redirection *redirection, const char *path);
static int open_noclobber(const char *path);
static int heredoc_descriptor(const char *text);
static void write_all(int descriptor, const char *text, size_t length);
static char *expand_target(struct sh_token *word);

/*
 * Applies a list of redirections in order.  With SH_REDIRECT_SAVE a frame
 * is pushed and what each descriptor held is saved in it; the caller pops
 * it with sh_redirect_pop, whether or not the redirections succeeded.
 * Returns 0, or the status of a failed redirection (its message written).
 */
int
sh_redirect(
	struct sh_redirection *list,
	int flags)
{
	struct redirect_frame *frame;
	int status;

	/* A frame is pushed even for no redirections, to be popped alike. */
	frame = NULL;
	if ((flags & SH_REDIRECT_SAVE) != 0) {
		frame = sh_malloc(sizeof(*frame));
		frame->saved = NULL;
		frame->count = 0;
		frame->capacity = 0;
		frame->previous = redirect_frames;
		redirect_frames = frame;
		redirect_depth++;
	}

	/* Output buffered for the old descriptors goes to them first. */
	if (list != NULL)
		fflush(NULL);

	/* Applies each redirection in the order written. */
	for (; list != NULL; list = list->next) {
		status = apply_one(list, frame);
		if (status != 0)
			return status;
	}

	/* Succeeded: every redirection is in place. */
	return 0;
}

/*
 * Pops the innermost frame, putting each descriptor back.
 */
void
sh_redirect_pop(
	void)
{
	struct redirect_frame *frame;
	int index;

	/* Nothing to pop. */
	frame = redirect_frames;
	if (frame == NULL)
		return;
	redirect_frames = frame->previous;
	redirect_depth--;

	/* Output buffered for the redirected descriptors goes there first. */
	if (frame->count > 0)
		fflush(NULL);

	/* Puts the descriptors back, newest first; -1 means it was closed. */
	for (index = frame->count - 1; index >= 0; index--) {
		if (frame->saved[index].copy < 0) {
			(void)close(frame->saved[index].descriptor);
			continue;
		}

		/* The descriptor gets its saved copy back. */
		(void)dup2(frame->saved[index].copy,
			   frame->saved[index].descriptor);
		(void)close(frame->saved[index].copy);
	}

	/* The frame goes with its records. */
	free(frame->saved);
	free(frame);
}

/*
 * Reports how many frames there are.
 */
int
sh_redirect_depth(
	void)
{
	/* Succeeded: the depth. */
	return redirect_depth;
}

/*
 * Pops frames down to a depth.
 */
void
sh_redirect_unwind(
	int depth)
{
	/* Pops the frames above the depth. */
	while (redirect_depth > depth)
		sh_redirect_pop();
}

/*
 * Makes the redirections of the innermost frame permanent (exec with no
 * command): the saved copies are dropped, and the frame is left empty for
 * its command to pop.
 */
void
sh_redirect_forget(
	void)
{
	struct redirect_frame *frame;
	int index;

	/* Nothing to forget. */
	frame = redirect_frames;
	if (frame == NULL)
		return;

	/* The saved copies are no longer needed. */
	for (index = 0; index < frame->count; index++) {
		if (frame->saved[index].copy >= 0)
			(void)close(frame->saved[index].copy);
	}

	/* The frame is empty now. */
	frame->count = 0;
}

/*
 * Returns where a descriptor was before the innermost redirections: the
 * saved copy, or the descriptor itself.  set -x writes to standard error as
 * it was before the command's own redirections.
 */
int
sh_redirect_saved_descriptor(
	int descriptor)
{
	struct redirect_frame *frame;
	int index;

	/* Without a frame, the descriptor is where it always was. */
	frame = redirect_frames;
	if (frame == NULL)
		return descriptor;

	/* The innermost frame's copy, if it saved one. */
	for (index = 0; index < frame->count; index++) {
		if (frame->saved[index].descriptor == descriptor &&
		    frame->saved[index].copy >= 0)
			return frame->saved[index].copy;
	}

	/* Not redirected. */
	return descriptor;
}

/*
 * Gives a command run in the background without job control /dev/null as
 * its standard input, unless it redirects standard input itself.
 */
void
sh_redirect_stdin_null(
	struct sh_node *node)
{
	struct sh_redirection *redirection;
	int descriptor;

	/* A redirection of standard input written on the command stands. */
	if (node->kind == SH_NODE_PIPELINE)
		node = node->u.pipeline.commands[0];
	for (redirection = node->redirections;
	     redirection != NULL;
	     redirection = redirection->next) {
		if (redirection->descriptor == 0)
			return;
	}

	/* Otherwise /dev/null. */
	descriptor = open("/dev/null", O_RDONLY);
	if (descriptor < 0)
		return;
	if (descriptor != 0) {
		(void)dup2(descriptor, 0);
		(void)close(descriptor);
	}
}

/* Applies one redirection, saving the descriptor in the frame first. */
static int
apply_one(
	struct sh_redirection *redirection,
	struct redirect_frame *frame)
{
	struct sh_expand_context context;
	const char *error_text;
	char *text;
	char *line;
	int descriptor;
	int target;
	int expanded;
	int moved;
	int error;
	int status;

	/* The word is expanded before anything is changed. */
	target = redirection->descriptor;
	if (redirection->op == SH_REDIR_HEREDOC) {
		sh_expand_context_fill(&context);
		expanded = sh_expand_word(redirection->word, &context, &text,
					  &error_text);
		if (!expanded)
			sh_error("%s", error_text);
		sh_temp_own(text);
	} else {
		text = expand_target(redirection->word);
	}

	/*
	 * >& word, when the word is not a descriptor or -, sends standard
	 * output and standard error to the file, as bash does (XCU 2.7.6
	 * leaves it unspecified); with another descriptor it is ambiguous.
	 */
	if (redirection->op == SH_REDIR_DUP_OUTPUT && !descriptor_word(text)) {
		if (target != 1) {
			sh_warn("%s: ambiguous redirect", text);
			return 1;
		}
		status = apply_both_outputs(redirection, text, frame);
		return status;
	}

	/* <& and >& name a descriptor, or - to close. */
	if (redirection->op == SH_REDIR_DUP_INPUT ||
	    redirection->op == SH_REDIR_DUP_OUTPUT) {
		status = apply_duplicate(text, target, frame);
		return status;
	}

	/* A here-document, and a here-string with its newline, are read from a pipe; anything else is a file. */
	if (redirection->op == SH_REDIR_HEREDOC) {
		descriptor = heredoc_descriptor(text);
	} else if (redirection->op == SH_REDIR_HERESTRING) {
		line = sh_temp_own(sh_malloc(strlen(text) + 2U));
		strcpy(line, text);
		strcat(line, "\n");
		descriptor = heredoc_descriptor(line);
	} else
		descriptor = open_file(redirection, text);
	if (descriptor < 0)
		return 2;

	/* Saves the target, then moves the descriptor onto it. */
	if (frame != NULL)
		save_descriptor(frame, target);
	if (descriptor == target)
		return 0;
	moved = dup2(descriptor, target);
	if (moved < 0) {
		error = errno;
		(void)close(descriptor);
		sh_warn("%d: %s", target, strerror(error));
		return 2;
	}

	/* The opened descriptor is no longer needed under its own number. */
	(void)close(descriptor);

	/* Succeeded: the file is on its descriptor. */
	return 0;
}

/* Reports whether the word of <& or >& names a descriptor or is -. */
static int
descriptor_word(
	const char *text)
{
	const char *cursor;

	/* - closes. */
	if (text[0] == '-' && text[1] == '\0')
		return 1;

	/* Digits name a descriptor, and digits and - move one. */
	if (text[0] == '\0')
		return 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor == '-' && cursor[1] == '\0' && cursor != text)
			break;
		if (*cursor < '0' || *cursor > '9')
			return 0;
	}

	/* Succeeded. */
	return 1;
}

/*
 * Opens a file for >& file and puts it on standard output and standard
 * error, as > file 2>&1 would (noclobber applies).
 */
static int
apply_both_outputs(
	struct sh_redirection *redirection,
	const char *text,
	struct redirect_frame *frame)
{
	struct sh_redirection output;
	int descriptor;
	int error;

	/* The file, opened as > would open it. */
	output = *redirection;
	output.op = SH_REDIR_OUTPUT;
	output.descriptor = 1;
	descriptor = open_file(&output, text);
	if (descriptor < 0)
		return 2;

	/* Standard output and standard error both become the file. */
	if (frame != NULL) {
		save_descriptor(frame, 1);
		save_descriptor(frame, 2);
	}
	if (dup2(descriptor, 1) < 0 || dup2(descriptor, 2) < 0) {
		error = errno;
		(void)close(descriptor);
		sh_warn("%s: %s", text, strerror(error));
		return 2;
	}
	if (descriptor != 1 && descriptor != 2)
		(void)close(descriptor);

	/* Succeeded. */
	return 0;
}

/*
 * Applies <& or >&: the word names the descriptor to copy, or is - to close
 * the target.  A word that is no descriptor is a syntax error.
 */
static int
apply_duplicate(
	const char *text,
	int target,
	struct redirect_frame *frame)
{
	char *end;
	size_t length;
	long source;
	int flags;
	int copied;
	int error;

	/* - closes the target. */
	if (text[0] == '-' && text[1] == '\0') {
		if (frame != NULL)
			save_descriptor(frame, target);
		(void)close(target);
		return 0;
	}

	/* n- moves descriptor n to the target, closing n (ksh and bash). */
	length = strlen(text);
	if (length > 1 && text[length - 1] == '-') {
		source = strtol(text, &end, 10);
		if (end != text + length - 1 || source > INT_MAX)
			sh_error("Syntax error: Bad fd number");
		if (fcntl((int)source, F_GETFD) < 0) {
			sh_warn("%ld: Bad file descriptor", source);
			return 2;
		}
		if (source == target)
			return 0;
		if (frame != NULL) {
			save_descriptor(frame, target);
			save_descriptor(frame, (int)source);
		}
		if (dup2((int)source, target) < 0) {
			error = errno;
			sh_warn("%ld: %s", source, strerror(error));
			return 2;
		}
		(void)close((int)source);
		return 0;
	}

	/* Anything but a number is an error that stops the command. */
	if (*text < '0' || *text > '9')
		sh_error("Syntax error: Bad fd number");
	source = strtol(text, &end, 10);
	if (*end != '\0' || source > INT_MAX)
		sh_error("Syntax error: Bad fd number");

	/* A descriptor copied onto itself stays as it is, open or not. */
	if (source == target)
		return 0;

	/* The source must be open. */
	flags = fcntl((int)source, F_GETFD);
	if (flags < 0) {
		sh_warn("%ld: Bad file descriptor", source);
		return 2;
	}

	/* Saves the target, then copies the source onto it. */
	if (frame != NULL)
		save_descriptor(frame, target);
	copied = dup2((int)source, target);
	if (copied < 0) {
		error = errno;
		sh_warn("%ld: %s", source, strerror(error));
		return 2;
	}

	/* Succeeded: the target is a copy of the source. */
	return 0;
}

/* Saves what a descriptor holds, once per frame. */
static void
save_descriptor(
	struct redirect_frame *frame,
	int descriptor)
{
	int index;
	int copy;

	/* The first save of a descriptor in a frame is the one to put back. */
	for (index = 0; index < frame->count; index++) {
		if (frame->saved[index].descriptor == descriptor)
			return;
	}

	/* Copies it out of the way; a closed descriptor is saved as -1. */
	copy = fcntl(descriptor, F_DUPFD_CLOEXEC, SAVE_BASE);

	/* Grows the frame when it is full. */
	if (frame->count == frame->capacity) {
		if (frame->capacity == 0)
			frame->capacity = 4;
		else
			frame->capacity *= 2;
		frame->saved = sh_realloc(frame->saved,
					  (size_t)frame->capacity *
					  sizeof(*frame->saved));
	}

	/* Records the save. */
	frame->saved[frame->count].descriptor = descriptor;
	frame->saved[frame->count].copy = copy;
	frame->count++;
}

/* Opens the file of a redirection as its operator says. */
static int
open_file(
	struct sh_redirection *redirection,
	const char *path)
{
	int descriptor;
	int flags;
	int error;

	/* noclobber refuses to truncate an existing regular file with >. */
	if (redirection->op == SH_REDIR_OUTPUT &&
	    sh_option[SH_OPT_NOCLOBBER]) {
		descriptor = open_noclobber(path);
		return descriptor;
	}

	/* The open flags of the operator. */
	switch (redirection->op) {
	case SH_REDIR_INPUT:
		flags = O_RDONLY;
		break;
	case SH_REDIR_APPEND:
		flags = O_WRONLY | O_CREAT | O_APPEND;
		break;
	case SH_REDIR_READ_WRITE:
		flags = O_RDWR | O_CREAT;
		break;
	default:
		flags = O_WRONLY | O_CREAT | O_TRUNC;
		break;
	}

	/* Opens it; reading reports "open", writing "create". */
	descriptor = open(path, flags, 0666);
	if (descriptor < 0) {
		error = errno;
		if (redirection->op == SH_REDIR_INPUT)
			sh_warn("cannot open %s: %s", path, strerror(error));
		else
			sh_warn("cannot create %s: %s", path, strerror(error));
		return -1;
	}

	/* Succeeded: the descriptor. */
	return descriptor;
}

/*
 * Opens a file for > under noclobber: a new file is created, and an
 * existing one is opened only when it is not a regular file (/dev/null).
 */
static int
open_noclobber(
	const char *path)
{
	struct stat status;
	int descriptor;
	int error;
	int found;
	int regular;

	/* A file that is not there yet is created. */
	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (descriptor >= 0)
		return descriptor;
	error = errno;

	/* An existing file that is not a regular file is written to. */
	if (error == EEXIST) {
		found = stat(path, &status);
		regular = 1;
		if (found == 0)
			regular = S_ISREG(status.st_mode);
		if (!regular) {
			descriptor = open(path, O_WRONLY);
			if (descriptor >= 0)
				return descriptor;
			error = errno;
		}
	}

	/* Any other failure is reported. */
	sh_warn("cannot create %s: %s", path, strerror(error));

	/* Refused. */
	return -1;
}

/*
 * Puts the text of a here-document into a pipe and returns its reading end.
 * A text longer than the pipe holds at once is written by a process of its
 * own, since the command that reads it has not started yet.
 */
static int
heredoc_descriptor(
	const char *text)
{
	size_t length;
	pid_t child;
	pid_t writer;
	int descriptors[2];
	int piped;
	int error;

	/* A pipe carries the text. */
	piped = pipe(descriptors);
	if (piped != 0) {
		error = errno;
		sh_warn("cannot create pipe: %s", strerror(error));
		return -1;
	}

	/* The text the here-document gives. */
	length = strlen(text);

	/* A short text fits in the pipe and is written now. */
	if (length <= HEREDOC_DIRECT) {
		write_all(descriptors[1], text, length);
		(void)close(descriptors[1]);
		return descriptors[0];
	}

	/*
	 * A longer one is written by a grandchild, which nobody waits for:
	 * the child exits at once and is waited for here.
	 */
	fflush(NULL);
	child = fork();
	if (child < 0) {
		error = errno;
		sh_warn("cannot fork: %s", strerror(error));
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		return -1;
	}

	/* The child leaves the writing to a grandchild of its own and ends at once. */
	if (child == 0) {
		(void)close(descriptors[0]);
		writer = fork();
		if (writer != 0)
			_exit(0);
		write_all(descriptors[1], text, length);
		_exit(0);
	}

	/* The shell keeps only the reading end, once the child has ended. */
	(void)close(descriptors[1]);
	(void)sh_wait_process(child, 0);

	/* Succeeded: the reading end. */
	return descriptors[0];
}

/* Writes all of a text, through interruptions, until the reader goes. */
static void
write_all(
	int descriptor,
	const char *text,
	size_t length)
{
	size_t written;
	ssize_t count;

	/* Writes until everything is written or the pipe is closed. */
	written = 0;
	while (written < length) {
		count = write(descriptor, text + written, length - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		written += (size_t)count;
	}
}

/* Expands the word of a redirection: no splitting, no pathname expansion. */
static char *
expand_target(
	struct sh_token *word)
{
	struct sh_expand_context context;
	const char *error_text;
	char *text;
	int expanded;

	/* One word, quotes removed; an expansion error stops the command. */
	sh_expand_context_fill(&context);
	expanded = sh_expand_word(word, &context, &text, &error_text);
	if (!expanded)
		sh_error("%s", error_text);
	sh_temp_own(text);

	/* Succeeded: the word, freed with the command. */
	return text;
}
