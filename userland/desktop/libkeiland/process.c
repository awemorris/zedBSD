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
 *
 * A system action (kl_process_start_admin) is asked through the system
 * (system/system.c) and has no child of the application's: while its
 * answer is awaited, the descriptors its start brings gather in the
 * record; at the start the platform serves them as it serves a child's,
 * with a thread that reads the end from the status descriptor in place of
 * waiting for a child.
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
static int process_make(struct kl_app *app, unsigned streams, void *data, struct kl_process **made);
static void process_free(struct kl_process *process);
static void process_admin_end(struct kl_process *process, unsigned how, int code);

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

	/* The record, with the program's reference and the streams it uses. */
	error = process_make(app, options->streams, options->data, &process);
	if (error != 0)
		return error;

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
 * Asks for a system action: a process whose answer comes from the system.
 */
int
kl_process_start_admin(
	struct kl_app *app,
	struct kl_system *system,
	const struct kl_admin_options *options,
	struct kl_process **made)
{
	struct kl_process *process;
	uint32_t request;
	int error;

	/* What is asked must make sense: the three streams at most, and a window of the application's. */
	if (made != NULL)
		*made = NULL;
	if (app == NULL || system == NULL || options == NULL || made == NULL)
		return EINVAL;
	if (options->action == NULL ||
	    options->window == NULL ||
	    options->window->app != app)
		return EINVAL;
	if ((options->streams & ~(KL_PROCESS_INPUT | KL_PROCESS_OUTPUT | KL_PROCESS_ERROR)) != 0U)
		return EINVAL;

	/* The record, with the program's reference and the streams it uses. */
	error = process_make(app, options->streams, options->data, &process);
	if (error != 0)
		return error;

	/* Its control pipe, so that input written before the start wakes nobody in vain. */
	error = keiui_child_await(process);
	if (error != 0) {
		process_free(process);
		return error;
	}

	/* The request; the system checks the action's name and its words. */
	error = keiui_system_admin_run(system, process, app->display, options->action, options->argv, options->streams, options->window->surface, &request);
	if (error != 0) {
		keiui_child_end(process);
		process_free(process);
		return error;
	}

	/* Its answer is awaited. */
	process->admin = KEIUI_ADMIN_ASKING;
	process->admin_system = system;
	process->admin_request = request;

	/* One of the application's processes. */
	process->app_next = app->processes;
	app->processes = process;

	/* Succeeded: it is asked. */
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
	/* Only an input of its own (a terminal's stays); a system action's is always there. */
	if (process == NULL)
		return EINVAL;
	if ((process->streams & KL_PROCESS_INPUT) == 0U && process->admin == 0U)
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

	/* A system action's program runs as another user: it stops when its input closes. */
	if (process->admin != 0U)
		return EPERM;

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

	/* A system action whose answer is awaited is taken back, and what its start brought is closed. */
	if (process->admin == KEIUI_ADMIN_ASKING) {
		keiui_system_admin_withdraw(process->admin_system, process->admin_request);
		keiui_child_drop(process);
		process->admin = KEIUI_ADMIN_ENDED;
		process->admin_system = NULL;
	}

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

	/* The last: the control pipe and what is left of the child, then the record. */
	keiui_child_end(process);
	process_free(process);
}

/*
 * Keeps a descriptor a system action's start brings (the system's stream
 * event); one that comes when no answer is awaited has nowhere to go.
 */
void
keiui_process_admin_stream(
	struct kl_process *process,
	unsigned which,
	int descriptor)
{
	/* Not awaited any more: closed. */
	if (process->admin != KEIUI_ADMIN_ASKING) {
		keiui_descriptor_close(descriptor);
		return;
	}

	/* Kept for the start. */
	keiui_child_take(process, which, descriptor);
}

/*
 * Starts serving a system action that started: the platform's threads on
 * the descriptors it brought.  Without its input or its status its end
 * cannot be known: it is lost.
 */
void
keiui_process_admin_started(
	struct kl_process *process)
{
	int error;

	/* Only an answer awaited. */
	if (process->admin != KEIUI_ADMIN_ASKING)
		return;
	process->admin = KEIUI_ADMIN_STARTED;
	process->admin_system = NULL;

	/* The threads on its descriptors. */
	error = keiui_child_adopt(process);
	if (error != 0) {
		/* What it brought is closed (its program reads its input's end); how it ends is not known. */
		keiui_child_drop(process);
		process_admin_end(process, KL_PROCESS_LOST, 0);
		return;
	}

	/* The start told, before what the threads tell. */
	keiui_lock_take(&process->lock);

	process->ready |= KL_PROCESS_READY_START;
	keiui_process_tell(process);

	keiui_lock_give(&process->lock);
}

/*
 * Ends a system action that was refused: it did not run.
 */
void
keiui_process_admin_refused(
	struct kl_process *process,
	unsigned reason)
{
	/* Only an answer awaited. */
	if (process->admin != KEIUI_ADMIN_ASKING)
		return;

	/* What came is closed, and the refusal told. */
	keiui_child_drop(process);
	process_admin_end(process, KL_PROCESS_REFUSED, (int)reason);
}

/*
 * Ends a system action whose answer cannot come any more (the system
 * closed, or the compositor went): it may have started meanwhile.
 */
void
keiui_process_admin_lost(
	struct kl_process *process)
{
	/* Only an answer awaited. */
	if (process->admin != KEIUI_ADMIN_ASKING)
		return;

	/* What came is closed (a program that started reads its input's end), and the loss told. */
	keiui_child_drop(process);
	process_admin_end(process, KL_PROCESS_LOST, 0);
}

/*
 * Takes the system's events for the system actions whose answers are
 * awaited (the application's dispatch): the program that asked need not
 * dispatch its system.
 */
void
keiui_processes_admin_dispatch(
	struct kl_app *app)
{
	struct kl_process *process;

	/* Each process of the application whose answer is awaited (its events do not take it out of the list). */
	for (process = app->processes; process != NULL; process = process->app_next) {
		if (process->admin != KEIUI_ADMIN_ASKING)
			continue;

		/* Its system's events. */
		keiui_system_admin_dispatch(process->admin_system);
	}
}

/*
 * Reads a system action's status line as how it ended.
 */
void
keiui_process_status_parse(
	const char *line,
	unsigned *how,
	int *code)
{
	const char *digits;
	unsigned long number;
	unsigned kind;
	size_t count;
	size_t index;
	int same;

	/* Not told, until the line says otherwise. */
	*how = KL_PROCESS_LOST;
	*code = 0;

	/* Its word: an exit or a signal. */
	kind = 0U;
	digits = NULL;
	same = strncmp(line, "EXIT ", 5U);
	if (same == 0) {
		kind = KL_PROCESS_EXITED;
		digits = line + 5;
	}
	same = strncmp(line, "SIGNAL ", 7U);
	if (same == 0) {
		kind = KL_PROCESS_SIGNALED;
		digits = line + 7;
	}
	if (kind == 0U)
		return;

	/* One to three decimal digits, then the line's end or nothing. */
	count = strspn(digits, "0123456789");
	if (count == 0U || count > 3U)
		return;
	if (digits[count] != '\0' && strcmp(digits + count, "\n") != 0)
		return;

	/* The number, at most 255. */
	number = 0;
	for (index = 0; index < count; index++)
		number = number * 10U + (unsigned long)(digits[index] - '0');
	if (number > 255U)
		return;

	/* Succeeded: how and its number. */
	*how = kind;
	*code = (int)number;
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

/*
 * Makes a process's record: the program's reference, the streams it uses
 * (output, a terminal's too; error; input, a terminal's too) and its lock.
 * Returns 0, or ENOMEM or the lock's error.
 */
static int
process_make(
	struct kl_app *app,
	unsigned streams,
	void *data,
	struct kl_process **made)
{
	struct kl_process *process;
	int error;

	/* The record, with the program's reference and no descriptor. */
	process = calloc(1, sizeof(*process));
	if (process == NULL)
		return ENOMEM;
	process->data = data;
	process->streams = streams;
	process->app = app;
	process->references = 1U;
	process->child.input = -1;
	process->child.output = -1;
	process->child.error = -1;
	process->child.terminal = -1;
	process->child.status = -1;
	process->child.control[0] = -1;
	process->child.control[1] = -1;

	/* The streams it uses: output (a terminal's too), error, and input (a terminal's too). */
	error = 0;
	if ((streams & (KL_PROCESS_OUTPUT | KL_PROCESS_TERMINAL)) != 0U)
		error = process_stream_open(&process->output);
	else
		process->output.ended = 1;
	if (error == 0 && (streams & KL_PROCESS_ERROR) != 0U)
		error = process_stream_open(&process->error);
	else
		process->error.ended = 1;
	if (error == 0 && (streams & (KL_PROCESS_INPUT | KL_PROCESS_TERMINAL)) != 0U)
		error = process_stream_open(&process->input);

	/* Gives up, freeing the rings, when one could not be made. */
	if (error != 0) {
		free(process->output.bytes);
		free(process->error.bytes);
		free(process->input.bytes);
		free(process);
		return error;
	}

	/* The lock of what the threads touch. */
	error = keiui_lock_open(&process->lock);
	if (error != 0) {
		free(process->output.bytes);
		free(process->error.bytes);
		free(process->input.bytes);
		free(process);
		return error;
	}

	/* Succeeded: the record. */
	*made = process;
	return 0;
}

/* Frees a process's record: its lock, its streams and itself. */
static void
process_free(
	struct kl_process *process)
{
	/* The lock, the rings, the record. */
	keiui_lock_close(&process->lock);
	free(process->output.bytes);
	free(process->error.bytes);
	free(process->input.bytes);
	free(process);
}

/*
 * Ends a system action that has no thread: how and its code, the streams
 * ended and the input refused, and the end told.
 */
static void
process_admin_end(
	struct kl_process *process,
	unsigned how,
	int code)
{
	/* No answer awaited any more. */
	process->admin = KEIUI_ADMIN_ENDED;
	process->admin_system = NULL;

	/* The end, as a reaped process's. */
	keiui_lock_take(&process->lock);

	process->how = how;
	process->code = code;
	process->reaped = 1;
	process->input_closed = 1;
	process->output.ended = 1;
	process->error.ended = 1;
	process->ready |= KL_PROCESS_READY_EXIT;
	keiui_process_tell(process);

	keiui_lock_give(&process->lock);
}
