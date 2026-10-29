/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The programs an application starts (kl_process_*): the record, the
 * streams kept for the program, and how a process's news becomes the
 * application's KL_APP_PROCESS events.
 *
 * The platform's threads (platform/process-posix.c) read the process's
 * output into its streams, write its input from one, and wait for its end;
 * under the process's lock they change what is to be told and ring the
 * application's wake when something new waits.  The application's dispatch
 * (keiui_processes_queue) then queues one event for each process that has
 * something to tell and none queued: the output for as long as some waits,
 * the input's room and the end once.
 */

#include "process.h"

#include "ui/window.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int process_streams_valid(unsigned streams);
static int process_stream_open(struct keiui_stream *stream);
static struct keiui_stream *process_stream(struct kl_process *process, unsigned stream);
static size_t process_line_end(const struct keiui_stream *stream, size_t limit, int *found);
static void process_forget(struct kl_process *process);

/*
 * Starts a process of the application.
 */
int
kl_process_start(
	struct kl_app *app,
	const struct kl_process_options *options,
	struct kl_process **made)
{
	struct kl_process *process;
	int valid;
	int error;

	/* What is asked must make sense. */
	if (made != NULL)
		*made = NULL;
	if (app == NULL || options == NULL || made == NULL)
		return EINVAL;
	if (options->argv == NULL || options->argv[0] == NULL || options->argv[0][0] == '\0')
		return EINVAL;
	valid = process_streams_valid(options->streams);
	if (!valid)
		return EINVAL;

	/* The record, with the program's reference. */
	process = calloc(1, sizeof(*process));
	if (process == NULL)
		return ENOMEM;
	process->data = options->data;
	process->streams = options->streams;
	process->app = app;
	process->references = 1U;
	process->child.input = -1;
	process->child.output = -1;
	process->child.error = -1;
	process->child.terminal = -1;
	process->child.control[0] = -1;
	process->child.control[1] = -1;

	/* The streams it uses: output (a terminal's too), error, and input (a terminal's too). */
	error = 0;
	if ((options->streams & (KL_PROCESS_OUTPUT | KL_PROCESS_TERMINAL)) != 0U)
		error = process_stream_open(&process->output);
	else
		process->output.ended = 1;
	if (error == 0 && (options->streams & KL_PROCESS_ERROR) != 0U)
		error = process_stream_open(&process->error);
	else
		process->error.ended = 1;
	if (error == 0 && (options->streams & (KL_PROCESS_INPUT | KL_PROCESS_TERMINAL)) != 0U)
		error = process_stream_open(&process->input);
	if (error == 0)
		error = keiui_lock_open(&process->lock);
	if (error != 0) {
		free(process->output.bytes);
		free(process->error.bytes);
		free(process->input.bytes);
		free(process);
		return error;
	}

	/* The child and the platform's threads; a failure leaves the record to its last reference. */
	error = keiui_child_start(process, options);
	if (error != 0) {
		keiui_process_release(process);
		return error;
	}

	/* One of the application's processes. */
	process->app_next = app->processes;
	app->processes = process;

	/* Succeeded: it runs. */
	*made = process;
	return 0;
}

/*
 * Writes bytes to the process's input, as many as there is room for.
 */
int
kl_process_write(
	struct kl_process *process,
	const void *bytes,
	size_t size,
	size_t *written)
{
	size_t room;
	size_t taken;

	/* What is asked must make sense. */
	if (written != NULL)
		*written = 0;
	if (process == NULL || written == NULL || (bytes == NULL && size != 0U))
		return EINVAL;
	if (process->input.bytes == NULL)
		return EINVAL;

	/* As much as there is room for; a short write wants the room told. */
	keiui_lock_take(&process->lock);

	if (process->input_closing || process->input_closed || process->reaped) {
		keiui_lock_give(&process->lock);
		return EPIPE;
	}

	/* Puts what fits into the input stream. */
	room = keiui_stream_room(&process->input);
	taken = size;
	if (taken > room)
		taken = room;
	keiui_stream_put(&process->input, bytes, taken);
	if (taken < size)
		process->input_wanted = 1;

	keiui_lock_give(&process->lock);

	/* The reading thread writes it. */
	if (taken != 0U)
		keiui_child_poke(process);

	/* Succeeded: what was taken. */
	*written = taken;
	return 0;
}

/*
 * Closes the process's input once what waits is written.
 */
int
kl_process_close_input(
	struct kl_process *process)
{
	/* Only an input of its own (a terminal's stays). */
	if (process == NULL || (process->streams & KL_PROCESS_INPUT) == 0U)
		return EINVAL;

	/* Marked, and the thread told. */
	keiui_lock_take(&process->lock);

	process->input_closing = 1;

	keiui_lock_give(&process->lock);
	keiui_child_poke(process);

	/* Succeeded. */
	return 0;
}

/*
 * Reads up to size bytes of a stream.
 */
size_t
kl_process_read(
	struct kl_process *process,
	unsigned stream,
	void *buffer,
	size_t size)
{
	struct keiui_stream *from;
	size_t room;
	size_t got;

	/* A stream of the process's. */
	if (process == NULL || buffer == NULL)
		return 0;
	from = process_stream(process, stream);
	if (from == NULL)
		return 0;

	/* The bytes; a stream that was full has its reading start again. */
	keiui_lock_take(&process->lock);

	room = keiui_stream_room(from);
	got = keiui_stream_take(from, buffer, size);

	keiui_lock_give(&process->lock);
	if (room == 0U && got != 0U)
		keiui_child_poke(process);

	/* What was read. */
	return got;
}

/*
 * Reads a line of a stream.
 */
int
kl_process_read_line(
	struct kl_process *process,
	unsigned stream,
	char *line,
	size_t size)
{
	struct keiui_stream *from;
	size_t length;
	size_t room;
	int found;
	int kind;

	/* A stream of the process's, and room for a byte and the NUL. */
	if (process == NULL || line == NULL || size < 2U)
		return 0;
	from = process_stream(process, stream);
	if (from == NULL)
		return 0;

	/* The line's length (up to size - 1 bytes) and whether its end is there. */
	keiui_lock_take(&process->lock);

	room = keiui_stream_room(from);
	length = process_line_end(from, size - 1U, &found);

	/* A whole line, its end taken and not kept. */
	kind = 0;
	if (found) {
		(void)keiui_stream_take(from, line, length);
		(void)keiui_stream_take(from, NULL, 1U);
		kind = KL_PROCESS_LINE;
	} else if (length == size - 1U || (room == 0U && length != 0U)) {
		/* A longer line, or a full stream without an end: its start. */
		(void)keiui_stream_take(from, line, length);
		kind = KL_PROCESS_LINE_CUT;
	} else if (from->ended && length != 0U) {
		/* The last line of a stream that ended. */
		(void)keiui_stream_take(from, line, length);
		kind = KL_PROCESS_LINE;
	}

	keiui_lock_give(&process->lock);

	/* No line waits. */
	if (kind == 0)
		return 0;

	/* The line, without a "\r" before its end. */
	if (kind == KL_PROCESS_LINE && length != 0U && line[length - 1U] == '\r')
		length--;
	line[length] = '\0';

	/* A stream that was full reads again. */
	if (room == 0U)
		keiui_child_poke(process);

	/* What was read. */
	return kind;
}

/*
 * Tells whether a process ended and how.
 */
unsigned
kl_process_exit(
	const struct kl_process *process,
	int *code)
{
	struct kl_process *locked;
	unsigned how;
	int value;

	/* No process has not ended. */
	if (code != NULL)
		*code = 0;
	if (process == NULL)
		return KL_PROCESS_RUNNING;

	/* The end as the waiting thread left it. */
	locked = (struct kl_process *)process;
	keiui_lock_take(&locked->lock);

	how = locked->how;
	value = locked->code;

	keiui_lock_give(&locked->lock);

	/* How, and the code. */
	if (code != NULL)
		*code = value;
	return how;
}

/*
 * Asks a process to end.
 */
int
kl_process_kill(
	struct kl_process *process,
	unsigned how)
{
	int error;

	/* A known way. */
	if (process == NULL)
		return EINVAL;
	if (how != KL_PROCESS_HANGUP && how != KL_PROCESS_TERMINATE && how != KL_PROCESS_KILL)
		return EINVAL;

	/* The system's signal. */
	error = keiui_child_signal(process, how);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Tells the process's terminal its new size.
 */
int
kl_process_resize(
	struct kl_process *process,
	unsigned columns,
	unsigned rows)
{
	int error;

	/* A terminal's process, and a size. */
	if (process == NULL || columns == 0U || rows == 0U)
		return EINVAL;
	if ((process->streams & KL_PROCESS_TERMINAL) == 0U)
		return ENOTSUP;

	/* The terminal told. */
	error = keiui_child_resize(process, columns, rows);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports the modes the process set on its terminal.
 */
int
kl_process_terminal_mode(
	const struct kl_process *process,
	unsigned *mode)
{
	int error;

	/* A terminal's process. */
	if (mode != NULL)
		*mode = 0;
	if (process == NULL || mode == NULL)
		return EINVAL;
	if ((process->streams & KL_PROCESS_TERMINAL) == 0U)
		return ENOTSUP;

	/* The terminal asked. */
	error = keiui_child_mode((struct kl_process *)process, mode);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports the program's pointer of a process.
 */
void *
kl_process_data(
	const struct kl_process *process)
{
	/* No process has none. */
	if (process == NULL)
		return NULL;

	/* The pointer. */
	return process->data;
}

/*
 * Closes the program's side of a process: out of the application, its
 * queued events gone, its streams closed by its thread.
 */
void
kl_process_close(
	struct kl_process *process)
{
	/* No process, nothing to close. */
	if (process == NULL)
		return;

	/* Out of the application, and the application out of the threads' reach. */
	process_forget(process);
	keiui_lock_take(&process->lock);

	process->closed = 1;
	process->app = NULL;

	keiui_lock_give(&process->lock);

	/* The thread closes the streams; the program's reference goes. */
	keiui_child_poke(process);
	keiui_process_release(process);
}

/*
 * Queues an event for each process with something to tell and none
 * queued (the application's dispatch).
 */
void
keiui_processes_queue(
	struct kl_app *app)
{
	struct kl_process *process;
	struct kl_app_event *event;
	unsigned ready;

	/* Each process of the application. */
	for (process = app->processes; process != NULL; process = process->app_next) {
		if (process->queued)
			continue;

		/* What it has to tell: the output for as long as some waits, the rest once. */
		keiui_lock_take(&process->lock);

		ready = process->ready;
		if (process->output.count != 0U)
			ready |= KL_PROCESS_READY_OUTPUT;
		if (process->error.count != 0U)
			ready |= KL_PROCESS_READY_ERROR;

		keiui_lock_give(&process->lock);
		if (ready == 0U)
			continue;

		/* Its event, while the queue has room (the next dispatch tells the rest). */
		event = keiui_app_slot(app);
		if (event == NULL)
			return;
		event->kind = KL_APP_PROCESS;
		event->process = process;
		event->ready = ready;
		process->queued = 1;

		/* What was told once is told. */
		keiui_lock_take(&process->lock);

		process->ready &= ~ready;

		keiui_lock_give(&process->lock);
	}
}

/*
 * Notes that a process's event was taken, so that the next one may be
 * queued.
 */
void
keiui_process_taken(
	struct kl_app_event *event)
{
	/* A process's event. */
	if (event->kind != KL_APP_PROCESS || event->process == NULL)
		return;

	/* None of it queued now. */
	event->process->queued = 0;
}

/*
 * Closes every process of an application that closes.
 */
void
keiui_processes_close(
	struct kl_app *app)
{
	/* Each process (its close takes it out of the list). */
	while (app->processes != NULL)
		kl_process_close(app->processes);
}

/*
 * Rings the application's wake for something new of a process (the
 * platform's threads, under the process's lock): nothing once the program
 * closed it or the application closed.
 */
void
keiui_process_tell(
	struct kl_process *process)
{
	/* No application to tell. */
	if (process->app == NULL)
		return;

	/* The wake. */
	keiui_wake_ring(&process->app->post_wake);
}

/*
 * Drops a reference to a process (the program's or a thread's); the last
 * frees it.
 */
void
keiui_process_release(
	struct kl_process *process)
{
	unsigned left;

	/* One fewer. */
	keiui_lock_take(&process->lock);

	process->references--;
	left = process->references;

	keiui_lock_give(&process->lock);

	/* Others hold it still. */
	if (left != 0U)
		return;

	/* The last: the control pipe, the lock, the streams and the record. */
	keiui_child_end(process);
	keiui_lock_close(&process->lock);
	free(process->output.bytes);
	free(process->error.bytes);
	free(process->input.bytes);
	free(process);
}

/*
 * Reports the room in a stream.
 */
size_t
keiui_stream_room(
	const struct keiui_stream *stream)
{
	/* A stream not used has none. */
	if (stream->bytes == NULL)
		return 0;

	/* What is not taken. */
	return KL_PROCESS_BUFFER - stream->count;
}

/*
 * Puts bytes at a stream's end (no more than its room).
 */
void
keiui_stream_put(
	struct keiui_stream *stream,
	const void *bytes,
	size_t size)
{
	const unsigned char *from;
	size_t at;
	size_t part;

	/* Each part up to the ring's end. */
	from = bytes;
	while (size != 0U) {
		at = (stream->first + stream->count) % KL_PROCESS_BUFFER;
		part = KL_PROCESS_BUFFER - at;
		if (part > size)
			part = size;
		memcpy(stream->bytes + at, from, part);
		stream->count += part;
		from += part;
		size -= part;
	}
}

/*
 * Takes up to size bytes from a stream's start (into bytes, or dropped when
 * bytes is NULL).  Returns how many were taken.
 */
size_t
keiui_stream_take(
	struct keiui_stream *stream,
	void *bytes,
	size_t size)
{
	unsigned char *to;
	size_t taken;
	size_t part;

	/* No more than wait. */
	if (size > stream->count)
		size = stream->count;
	taken = size;

	/* Each part up to the ring's end. */
	to = bytes;
	while (size != 0U) {
		part = KL_PROCESS_BUFFER - stream->first;
		if (part > size)
			part = size;
		if (to != NULL) {
			memcpy(to, stream->bytes + stream->first, part);
			to += part;
		}

		/* Moves the ring's start past the part taken. */
		stream->first = (stream->first + part) % KL_PROCESS_BUFFER;
		stream->count -= part;
		size -= part;
	}

	/* What was taken. */
	return taken;
}

/* Reports whether the streams asked for go together: a terminal alone, or any of the three. */
static int
process_streams_valid(
	unsigned streams)
{
	/* Nothing unknown. */
	if ((streams & ~(KL_PROCESS_INPUT | KL_PROCESS_OUTPUT | KL_PROCESS_ERROR | KL_PROCESS_TERMINAL)) != 0U)
		return 0;

	/* A terminal takes the place of the three. */
	if ((streams & KL_PROCESS_TERMINAL) != 0U && streams != KL_PROCESS_TERMINAL)
		return 0;

	/* Valid. */
	return 1;
}

/* Gives a stream its ring.  Returns 0 or ENOMEM. */
static int
process_stream_open(
	struct keiui_stream *stream)
{
	/* The ring. */
	stream->bytes = malloc(KL_PROCESS_BUFFER);
	if (stream->bytes == NULL)
		return ENOMEM;

	/* Succeeded: empty. */
	return 0;
}

/* Finds the stream a program reads (KL_PROCESS_OUTPUT or KL_PROCESS_ERROR), NULL when it has none. */
static struct keiui_stream *
process_stream(
	struct kl_process *process,
	unsigned stream)
{
	struct keiui_stream *found;

	/* The one named. */
	found = NULL;
	if (stream == KL_PROCESS_OUTPUT)
		found = &process->output;
	else if (stream == KL_PROCESS_ERROR)
		found = &process->error;

	/* Only one the process has. */
	if (found == NULL || found->bytes == NULL)
		return NULL;
	return found;
}

/*
 * Measures the line at a stream's start: the bytes before its "\n" (*found
 * set) or, without one within limit bytes, how many there are up to limit.
 */
static size_t
process_line_end(
	const struct keiui_stream *stream,
	size_t limit,
	int *found)
{
	size_t length;
	size_t at;

	/* Each byte up to the limit, or the line's end. */
	*found = 0;
	for (length = 0; length < stream->count; length++) {
		at = (stream->first + length) % KL_PROCESS_BUFFER;
		if (stream->bytes[at] == '\n') {
			*found = 1;
			return length;
		}

		/* Stops at the limit without an end. */
		if (length == limit)
			return limit;
	}

	/* No end yet: what there is. */
	return length;
}

/* Takes a process out of its application: out of the list, and its queued event out of the queue. */
static void
process_forget(
	struct kl_process *process)
{
	struct kl_process **link;
	struct kl_app *app;

	/* The application, read on its own thread (the threads only read it under the lock). */
	app = process->app;
	if (app == NULL)
		return;

	/* Out of the list. */
	for (link = &app->processes; *link != NULL; link = &(*link)->app_next) {
		if (*link == process) {
			*link = process->app_next;
			break;
		}
	}

	/* Its event out of the queue. */
	keiui_app_forget_process(app, process);
	process->queued = 0;
}
