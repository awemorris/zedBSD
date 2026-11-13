/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input thread (ws213-p004): the evdev devices read, their reports
 * assembled in the order they were made, the pointer's place written to
 * the cursor's box, and every report passed to the main thread in order
 * (input-thread.h).
 *
 * Nothing here prints, calls Vulkan or the backend's seat, or touches the
 * server: what the main thread needs comes to it as events.
 */

#include "input-thread.h"
#include "kwl.h"

#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Bound the reads one device gets per pass, so others still run. */
#define INPUT_READS_PER_PASS	16U

/* The events one read of a device takes at most. */
#define INPUT_READ_EVENTS	32U

/*
 * One ready device's events while the devices are read together: those read
 * and not yet applied, and how many reads it has had this pass.
 */
struct input_source {
	struct kwl_input_reader *reader;
	struct input_event events[INPUT_READ_EVENTS];
	size_t count;
	size_t next;
	unsigned reads;
	unsigned done;
};

static void *input_main(void *argument);
static int input_signal_init(pthread_mutex_t *lock, pthread_cond_t *signal);
static void input_commands(struct kwl_input_thread *input, unsigned *stop);
static void input_attach_reader(struct kwl_input_thread *input, const struct kwl_input_attach *device);
static void input_detach_reader(struct kwl_input_thread *input, struct kwl_input_reader *reader, enum kwl_input_detach_reason reason, int error, uint64_t sequence);
static void input_read_ready(struct kwl_input_thread *input, struct kwl_input_reader **ready, size_t count);
static int input_source_fill(struct kwl_input_thread *input, struct input_source *source);
static int input_event_earlier(const struct input_event *event, const struct input_event *other);
static void input_consume(struct kwl_input_thread *input, struct kwl_input_reader *reader, const struct input_event *event);
static void input_frame(struct kwl_input_thread *input, struct kwl_input_reader *reader, uint32_t time);
static void input_pad_report(struct kwl_input_thread *input, struct kwl_input_reader *reader, uint32_t time);
static void input_pad_actions(struct kwl_input_thread *input, struct kwl_input_reader *reader, const struct kwl_touchpad_actions *actions, uint32_t time);
static void input_report(struct kwl_input_thread *input, struct kwl_input_reader *reader, uint32_t time);
static void input_pad_ticks(struct kwl_input_thread *input);
static int input_pad_timeout(const struct kwl_input_thread *input, int timeout_ms);
static struct kwl_input_event *input_event_new(struct kwl_input_thread *input, struct kwl_input_reader *reader, enum kwl_input_event_kind kind, uint32_t time);
static void input_queue(struct kwl_input_thread *input, struct kwl_input_event *event);
static void input_fail(struct kwl_input_thread *input, int error);
static uint32_t input_event_time(const struct input_event *event);
static void input_wake(int descriptor);
static void input_motion_times(const struct kwl_input_thread *input, const struct kwl_input_reader *reader, struct kwl_cursor_motion *motion);

/*
 * Makes the input thread's state: its locks, its wake pipe and its copy of
 * the settings.  The thread itself starts with kwl_input_thread_start.
 */
int
kwl_input_thread_init(
	struct kwl_input_thread *input,
	struct kwl_cursor_box *box,
	int wake_main,
	kwl_input_read_fn *read,
	const struct kwl_input_settings *settings)
{
	unsigned index;
	int error;

	/* Nothing attached, nothing queued. */
	memset(input, 0, sizeof(*input));
	input->box = box;
	input->wake_main = wake_main;
	input->read = read;
	input->settings = *settings;
	input->own_settings = *settings;
	input->wake_input[0] = -1;
	input->wake_input[1] = -1;
	for (index = 0; index < KWL_INPUT_THREAD_DEVICES; index++)
		input->readers[index].device.fd = -1;

	/* The input lock and the detach's acknowledgement, timed by the monotonic clock. */
	error = input_signal_init(&input->input_lock, &input->acknowledged);
	if (error != 0)
		return error;

	/* The pipe the main thread wakes the input thread with; neither end blocks. */
	error = pipe(input->wake_input);
	if (error != 0) {
		error = errno;
		pthread_cond_destroy(&input->acknowledged);
		pthread_mutex_destroy(&input->input_lock);
		return error;
	}

	/* Neither end blocks, and neither goes to a program the compositor starts. */
	for (index = 0; index < 2U; index++) {
		(void)fcntl(input->wake_input[index], F_SETFL, O_NONBLOCK);
		(void)fcntl(input->wake_input[index], F_SETFD, FD_CLOEXEC);
	}

	/* Succeeded: the thread can be started. */
	return 0;
}

/*
 * Starts the input thread.
 */
int
kwl_input_thread_start(
	struct kwl_input_thread *input)
{
	sigset_t signals_all;
	sigset_t signals_before;
	int error;

	/* The thread runs passes until it is asked to stop. */
	/* The thread blocks every signal: the stops and a child's end go to the event loop's thread. */
	(void)sigfillset(&signals_all);
	(void)pthread_sigmask(SIG_SETMASK, &signals_all, &signals_before);
	error = pthread_create(&input->thread, NULL, input_main, input);
	(void)pthread_sigmask(SIG_SETMASK, &signals_before, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the thread reads what is attached to it. */
	input->started = 1U;
	return 0;
}

/*
 * Stops the input thread and waits for it to end; it no longer uses any
 * descriptor after this returns.
 */
void
kwl_input_thread_stop(
	struct kwl_input_thread *input)
{
	/* A thread never started has nothing to stop. */
	if (!input->started)
		return;

	/* The request, seen at its next pass. */
	pthread_mutex_lock(&input->box->lock);

	input->stop = 1U;

	pthread_mutex_unlock(&input->box->lock);

	/* Woken from its poll, and waited for. */
	input_wake(input->wake_input[1]);
	(void)pthread_join(input->thread, NULL);
	input->started = 0U;
}

/*
 * Frees what the stopped input thread leaves: commands not taken, events
 * not applied, its pipe and its locks.
 */
void
kwl_input_thread_destroy(
	struct kwl_input_thread *input)
{
	struct kwl_input_command *command;
	struct kwl_input_event *event;
	unsigned index;

	/* The commands not taken. */
	while (input->commands != NULL) {
		command = input->commands;
		input->commands = command->next;
		free(command);
	}

	/* The events not applied. */
	while (input->events != NULL) {
		event = input->events;
		input->events = event->next;
		free(event);
	}

	/* The pipe. */
	for (index = 0; index < 2U; index++) {
		if (input->wake_input[index] >= 0)
			close(input->wake_input[index]);
		input->wake_input[index] = -1;
	}

	/* The locks. */
	pthread_cond_destroy(&input->acknowledged);
	pthread_mutex_destroy(&input->input_lock);
}

/*
 * Runs one pass of the input thread: the main thread's commands, a poll of
 * the attached devices (at most timeout_ms, -1 for no limit; a touch pad
 * shortens it to its next timer), their reports read in the order they
 * were made, and the touch pads' timers.
 *
 * Returns 1 to go on, 0 when the thread is asked to stop, -1 when the poll
 * failed (the error is kept for the main thread).  The thread calls it in a
 * loop; a host test calls it step by step.
 */
int
kwl_input_thread_step(
	struct kwl_input_thread *input,
	int timeout_ms)
{
	struct pollfd descriptors[KWL_INPUT_THREAD_DEVICES + 1U];
	struct kwl_input_reader *polled[KWL_INPUT_THREAD_DEVICES];
	struct kwl_input_reader *ready[KWL_INPUT_THREAD_DEVICES];
	unsigned char drained[16];
	size_t count;
	size_t ready_count;
	size_t index;
	unsigned stop;
	unsigned pads;
	ssize_t got;
	int answered;
	int error;

	/* The main thread's commands and settings, in the order given. */
	input_commands(input, &stop);
	if (stop)
		return 0;

	/* The wake pipe first, then every attached device. */
	descriptors[0].fd = input->wake_input[0];
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	count = 0;
	pads = 0;
	for (index = 0; index < KWL_INPUT_THREAD_DEVICES; index++) {
		/* A slot not attached is not read. */
		if (!input->readers[index].active)
			continue;

		/* Its descriptor, polled for its events. */
		descriptors[count + 1U].fd = input->readers[index].device.fd;
		descriptors[count + 1U].events = POLLIN;
		descriptors[count + 1U].revents = 0;
		polled[count] = &input->readers[index];
		count++;

		/* A touch pad has timers. */
		if (input->readers[index].device.touchpad)
			pads = 1;
	}

	/* A touch pad's tap completes when no drag comes in time: the wait ends then at the latest. */
	if (pads)
		timeout_ms = input_pad_timeout(input, timeout_ms);

	/* The devices' events or the main thread's wake, and when the wait began and ended (the box's lag, ws213-p004). */
	input->poll_entered_us = kwl_microseconds();
	answered = poll(descriptors, count + 1U, timeout_ms);
	input->poll_returned_us = kwl_microseconds();
	if (answered < 0) {
		error = errno;

		/* A signal wakes the pass early; the next one polls again. */
		if (error == EINTR)
			return 1;

		/* Any other failure stops the thread, and the main thread hears of it. */
		input_fail(input, error);
		return -1;
	}

	/* The wake's bytes are emptied; the commands come at the next pass. */
	if ((descriptors[0].revents & POLLIN) != 0) {
		do {
			got = read(input->wake_input[0], drained, sizeof(drained));
		} while (got > 0);
	}

	/* The devices that have events, or that failed or went. */
	ready_count = 0;
	for (index = 0; index < count; index++) {
		if (descriptors[index + 1U].revents != 0)
			ready[ready_count++] = polled[index];
	}

	/* Their reports, in the order they were made. */
	if (ready_count != 0)
		input_read_ready(input, ready, ready_count);

	/* The touch pads' timers. */
	if (pads)
		input_pad_ticks(input);

	/* Succeeded: the pass is over. */
	return 1;
}

/*
 * Asks the input thread to read a device from now on; its descriptor stays
 * the main thread's to close, after a detach.
 */
int
kwl_input_thread_attach(
	struct kwl_input_thread *input,
	const struct kwl_input_attach *device)
{
	struct kwl_input_command *command;

	/* The command, made before the lock is taken. */
	command = calloc(1, sizeof(*command));
	if (command == NULL)
		return ENOMEM;
	command->kind = KWL_INPUT_COMMAND_ATTACH;
	command->attach = *device;

	/* Queued after the commands before it. */
	pthread_mutex_lock(&input->box->lock);

	if (input->commands_tail == NULL) {
		input->commands = command;
	} else {
		input->commands_tail->next = command;
	}

	/* The new command is the last. */
	input->commands_tail = command;

	pthread_mutex_unlock(&input->box->lock);

	/* The input thread takes it at its next pass. */
	input_wake(input->wake_input[1]);

	/* Succeeded: the device is read from the input thread's next pass. */
	return 0;
}

/*
 * Asks the input thread to stop reading a device, and gives the sequence
 * number its acknowledgement will carry (kwl_input_thread_acked,
 * kwl_input_thread_detach_wait).
 */
int
kwl_input_thread_detach(
	struct kwl_input_thread *input,
	unsigned slot,
	uint64_t *sequence)
{
	struct kwl_input_command *command;

	/* The command, made before the lock is taken. */
	command = calloc(1, sizeof(*command));
	if (command == NULL)
		return ENOMEM;
	command->kind = KWL_INPUT_COMMAND_DETACH;
	command->attach.slot = slot;
	command->attach.fd = -1;

	/*
	 * Numbered and queued.  The main thread alone numbers the detaches, so
	 * the acknowledgements come in their order.
	 */
	pthread_mutex_lock(&input->box->lock);

	input->next_sequence++;
	command->sequence = input->next_sequence;
	if (input->commands_tail == NULL) {
		input->commands = command;
	} else {
		input->commands_tail->next = command;
	}

	/* The new command is the last. */
	input->commands_tail = command;

	pthread_mutex_unlock(&input->box->lock);

	/* The input thread takes it at its next pass. */
	*sequence = command->sequence;
	input_wake(input->wake_input[1]);

	/* Succeeded: the acknowledgement comes after the device's last event. */
	return 0;
}

/*
 * Waits at most wait_ms for the input thread to acknowledge a detach: it no
 * longer polls or reads that descriptor, and the device's last events are
 * queued before the acknowledgement.
 *
 * Returns 0 once acknowledged, ETIMEDOUT when the time ran out.
 */
int
kwl_input_thread_detach_wait(
	struct kwl_input_thread *input,
	uint64_t sequence,
	unsigned wait_ms)
{
	struct timespec deadline;
	int error;

	/* The deadline on the monotonic clock the signal waits by. */
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += (time_t)(wait_ms / 1000U);
	deadline.tv_nsec += (long)(wait_ms % 1000U) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	/* The acknowledgement, or the deadline. */
	error = 0;
	pthread_mutex_lock(&input->input_lock);

	while (input->acked_sequence < sequence && error == 0)
		error = pthread_cond_timedwait(&input->acknowledged, &input->input_lock, &deadline);
	if (input->acked_sequence >= sequence)
		error = 0;

	pthread_mutex_unlock(&input->input_lock);

	/* The input thread did not answer in time. */
	if (error != 0)
		return ETIMEDOUT;

	/* Succeeded: the descriptor is the main thread's alone. */
	return 0;
}

/*
 * Gives the last detach the input thread has acknowledged: every
 * descriptor detached up to it is no longer polled or read.
 */
uint64_t
kwl_input_thread_acked(
	struct kwl_input_thread *input)
{
	uint64_t acked;

	/* The number the acknowledgements left. */
	pthread_mutex_lock(&input->input_lock);

	acked = input->acked_sequence;

	pthread_mutex_unlock(&input->input_lock);

	/* Succeeded: the last acknowledged. */
	return acked;
}

/*
 * Gives the input thread the user's settings, for the reports from its
 * next pass on.
 */
void
kwl_input_thread_settings(
	struct kwl_input_thread *input,
	const struct kwl_input_settings *settings)
{
	/* The settings and a new generation, which the next pass compares. */
	pthread_mutex_lock(&input->box->lock);

	input->settings = *settings;
	input->settings_generation++;

	pthread_mutex_unlock(&input->box->lock);

	/* The input thread takes them at its next pass. */
	input_wake(input->wake_input[1]);
}

/*
 * Takes every event the input thread has queued, in order, for the main
 * thread to apply and free.  The main thread empties its wake pipe before,
 * so an event queued after this wakes it again.
 */
struct kwl_input_event *
kwl_input_thread_take(
	struct kwl_input_thread *input)
{
	struct kwl_input_event *events;

	/* The whole queue at once; the next event wakes the main thread again. */
	pthread_mutex_lock(&input->input_lock);

	events = input->events;
	input->events = NULL;
	input->events_tail = NULL;
	input->main_woken = 0U;

	pthread_mutex_unlock(&input->input_lock);

	/* Succeeded: the events, oldest first, or NULL. */
	return events;
}

/*
 * Tells the main thread whether the input thread stopped on an error (the
 * error, 0 if not), and how many events were dropped for want of memory
 * since it last asked.
 */
int
kwl_input_thread_failure(
	struct kwl_input_thread *input,
	unsigned *dropped)
{
	int failed;

	/* Both read, the count started again. */
	pthread_mutex_lock(&input->input_lock);

	failed = input->failed;
	*dropped = input->dropped;
	input->dropped = 0U;

	pthread_mutex_unlock(&input->input_lock);

	/* Reports the error that stopped the thread, or 0. */
	return failed;
}

/* Runs the input thread's passes until it is asked to stop or its poll fails. */
static void *
input_main(
	void *argument)
{
	struct kwl_input_thread *input;
	int going;

	/* Pass after pass. */
	input = argument;
	do {
		going = kwl_input_thread_step(input, -1);
	} while (going > 0);

	/* The thread ends. */
	return NULL;
}

/* Makes a mutex and a signal whose timed waits use the monotonic clock. */
static int
input_signal_init(
	pthread_mutex_t *lock,
	pthread_cond_t *signal)
{
	pthread_condattr_t attributes;
	int error;

	/* The mutex. */
	error = pthread_mutex_init(lock, NULL);
	if (error != 0)
		return error;

	/* The signal's attributes. */
	error = pthread_condattr_init(&attributes);
	if (error != 0) {
		pthread_mutex_destroy(lock);
		return error;
	}

	/* The monotonic clock, and the signal. */
	error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (error == 0)
		error = pthread_cond_init(signal, &attributes);
	pthread_condattr_destroy(&attributes);
	if (error != 0) {
		pthread_mutex_destroy(lock);
		return error;
	}

	/* Succeeded: both can be used. */
	return 0;
}

/* Takes the main thread's commands and settings and carries the commands out in order. */
static void
input_commands(
	struct kwl_input_thread *input,
	unsigned *stop)
{
	struct kwl_input_command *commands;
	struct kwl_input_command *command;
	struct kwl_input_settings settings;
	uint64_t generation;
	unsigned index;

	/* Everything the main thread left, in one section. */
	pthread_mutex_lock(&input->box->lock);

	commands = input->commands;
	input->commands = NULL;
	input->commands_tail = NULL;
	settings = input->settings;
	generation = input->settings_generation;
	*stop = input->stop;

	pthread_mutex_unlock(&input->box->lock);

	/* New settings: the touch pads' feel changes now, the speeds from the next report. */
	if (generation != input->own_generation) {
		input->own_settings = settings;
		input->own_generation = generation;
		for (index = 0; index < KWL_INPUT_THREAD_DEVICES; index++) {
			/* Only an attached touch pad has a feel. */
			if (!input->readers[index].active || !input->readers[index].device.touchpad)
				continue;
			kwl_touchpad_set_feel(&input->readers[index].pad, settings.touchpad_acceleration, settings.touchpad_natural);
			kwl_touchpad_set_down(&input->readers[index].pad, settings.touchpad_down);
		}
	}

	/* Each command in the order it was given. */
	while (commands != NULL) {
		command = commands;
		commands = command->next;

		/* An attach starts reading; a detach stops and is acknowledged. */
		if (command->kind == KWL_INPUT_COMMAND_ATTACH) {
			input_attach_reader(input, &command->attach);
		} else if (command->attach.slot < KWL_INPUT_THREAD_DEVICES) {
			input_detach_reader(input, &input->readers[command->attach.slot], KWL_INPUT_DETACH_ASKED, 0, command->sequence);
		}

		/* The command is the input thread's to free. */
		free(command);
	}
}

/* Starts reading a device in its slot: a new report, a new acceleration and a new touch pad layer. */
static void
input_attach_reader(
	struct kwl_input_thread *input,
	const struct kwl_input_attach *device)
{
	struct kwl_input_reader *reader;

	/* A slot outside the table is refused. */
	if (device->slot >= KWL_INPUT_THREAD_DEVICES)
		return;

	/* The slot starts afresh with this descriptor. */
	reader = &input->readers[device->slot];
	memset(reader, 0, sizeof(*reader));
	reader->device = *device;
	reader->abs_x = device->abs_x;
	reader->abs_y = device->abs_y;
	kwl_pointer_accel_init(&reader->accel);

	/* A touch pad's layer knows its fingers' resolution, range and the user's feel. */
	if (device->touchpad) {
		kwl_touchpad_init(&reader->pad, device->resolution_x, device->resolution_y);
		kwl_touchpad_set_range(&reader->pad, device->pad_x_minimum, device->pad_x_maximum, device->pad_y_minimum, device->pad_y_maximum);
		kwl_touchpad_set_feel(&reader->pad, input->own_settings.touchpad_acceleration, input->own_settings.touchpad_natural);
		kwl_touchpad_set_down(&reader->pad, input->own_settings.touchpad_down);
	}

	/* Read from the next poll on. */
	reader->active = 1U;
}

/*
 * Stops reading a device: a touch pad lets go of what it holds, and the
 * device's last event says why it ended, after every event it had made.
 * A detach the main thread asked for is acknowledged with that event.
 */
static void
input_detach_reader(
	struct kwl_input_thread *input,
	struct kwl_input_reader *reader,
	enum kwl_input_detach_reason reason,
	int error,
	uint64_t sequence)
{
	struct kwl_touchpad_actions actions;
	struct kwl_input_event *event;
	uint32_t now;

	/* A touch pad lets go of every button it holds, before the device's end. */
	now = (uint32_t)kwl_milliseconds();
	if (reader->active && reader->device.touchpad) {
		kwl_touchpad_release_all(&reader->pad, &actions);
		if (actions.count != 0)
			input_pad_actions(input, reader, &actions, now);
	}

	/* The descriptor is not polled or read again. */
	reader->active = 0U;
	reader->device.fd = -1;

	/*
	 * The last event, which is also the acknowledgement: a detach the main
	 * thread waits on is acknowledged even when no memory is left for the
	 * event, or the main thread would wait in vain.
	 */
	event = input_event_new(input, reader, KWL_INPUT_EVENT_DETACHED, now);
	if (event != NULL) {
		event->u.detached.reason = reason;
		event->u.detached.error = error;
		event->u.detached.sequence = sequence;
		input_queue(input, event);
		return;
	}

	/* The acknowledgement alone. */
	if (sequence == 0U)
		return;
	pthread_mutex_lock(&input->input_lock);

	input->acked_sequence = sequence;
	pthread_cond_broadcast(&input->acknowledged);

	pthread_mutex_unlock(&input->input_lock);

	/* The main thread, which may owe the seat an answer after it, looks. */
	input_wake(input->wake_main);
}

/*
 * Drains the events the ready devices have and applies them in the order
 * they were made (BUG-142): each device's reports are gathered until its
 * SYN_REPORT, so a key pressed before a click is applied before it even
 * when both waited.  Events of one time keep the devices' order, and one
 * device's events keep their own.
 */
static void
input_read_ready(
	struct kwl_input_thread *input,
	struct kwl_input_reader **ready,
	size_t count)
{
	static struct input_source sources[KWL_INPUT_THREAD_DEVICES];
	struct input_source *earliest;
	size_t index;
	int filled;
	int before;

	/* One source for each ready device. */
	for (index = 0; index < count; index++) {
		sources[index].reader = ready[index];
		sources[index].count = 0;
		sources[index].next = 0;
		sources[index].reads = 0;
		sources[index].done = 0;
	}

	/* Applies the earliest waiting event until no device has one this pass. */
	for (;;) {
		earliest = NULL;
		for (index = 0; index < count; index++) {
			/* A source that ran out reads again, within its reads for the pass. */
			filled = input_source_fill(input, &sources[index]);
			if (!filled)
				continue;

			/* The first source with the earliest event wins a tie. */
			if (earliest == NULL) {
				earliest = &sources[index];
				continue;
			}

			/* A later source wins only with an earlier event. */
			before = input_event_earlier(&sources[index].events[sources[index].next],
						     &earliest->events[earliest->next]);
			if (before)
				earliest = &sources[index];
		}

		/* Nothing is left to apply. */
		if (earliest == NULL)
			return;

		/* Applies the event and moves its source on. */
		input_consume(input, earliest->reader, &earliest->events[earliest->next]);
		earliest->next++;
	}
}

/*
 * Makes sure a source has an event to apply, reading its device when the
 * events read before are used up.  Reports 1 when it has one, 0 when the
 * device has nothing more this pass (nothing ready, its reads used, or
 * ended: then it is detached with its reason).
 */
static int
input_source_fill(
	struct kwl_input_thread *input,
	struct input_source *source)
{
	struct kwl_input_reader *reader;
	ssize_t count;
	int error;

	/* Events read before are still waiting. */
	if (source->next < source->count)
		return 1;

	/* Reads until the device gives events, has none, or the pass's reads are used. */
	reader = source->reader;
	while (!source->done) {
		/* A detached device, or one that had its reads, has nothing more this pass. */
		if (!reader->active || source->reads >= INPUT_READS_PER_PASS) {
			source->done = 1;
			break;
		}

		/* Reads as many whole events as the buffer holds. */
		source->reads++;
		count = input->read(reader->device.fd, source->events, INPUT_READ_EVENTS);
		if (count > 0) {
			/* The last input, which a layer's picture drawn in the quiet waits on (layer-cache.c), and the read's end for the box's lag. */
			reader->read_ms = kwl_milliseconds();
			reader->read_us = kwl_microseconds();
			source->count = (size_t)count;
			source->next = 0;
			return 1;
		}

		/* End of file means the node is gone. */
		if (count == 0) {
			input_detach_reader(input, reader, KWL_INPUT_DETACH_ENDED, EIO, 0U);
			source->done = 1;
			break;
		}

		/* Nothing more is ready; the next poll will say when there is. */
		error = errno;
		if (error == EAGAIN || error == EWOULDBLOCK) {
			source->done = 1;
			break;
		}

		/* A signal interrupted the read; it is tried again. */
		if (error == EINTR)
			continue;

		/* The seat revoked it (logind may give it back), or it failed and is gone. */
		if (error == ENODEV) {
			input_detach_reader(input, reader, KWL_INPUT_DETACH_REVOKED, error, 0U);
		} else {
			input_detach_reader(input, reader, KWL_INPUT_DETACH_ENDED, error, 0U);
		}

		/* A detached device has nothing more this pass. */
		source->done = 1;
	}

	/* Nothing to apply from this device this pass. */
	return 0;
}

/* Reports whether an event was made before another (by its evdev time). */
static int
input_event_earlier(
	const struct input_event *event,
	const struct input_event *other)
{
	/* The seconds decide first. */
	if (event->time.tv_sec < other->time.tv_sec)
		return 1;
	if (event->time.tv_sec > other->time.tv_sec)
		return 0;

	/* Then the microseconds. */
	if (event->time.tv_usec < other->time.tv_usec)
		return 1;

	/* Made at the same time or later. */
	return 0;
}

/* Adds one event to the device's report, or completes the report. */
static void
input_consume(
	struct kwl_input_thread *input,
	struct kwl_input_reader *reader,
	const struct input_event *event)
{
	uint32_t time;

	/* The kernel dropped events: forget the partial report and skip to the next one. */
	if (event->type == EV_SYN && event->code == SYN_DROPPED) {
		reader->frame_count = 0;
		reader->discarding = 1;
		return;
	}

	/* SYN_REPORT completes a report, unless the report was damaged by a drop. */
	if (event->type == EV_SYN && event->code == SYN_REPORT) {
		/* A damaged report is thrown away; the next one is applied again. */
		if (reader->discarding) {
			reader->discarding = 0;
			reader->frame_count = 0;
			return;
		}

		/* An intact report is applied as one group, by the kind of device. */
		time = input_event_time(event);
		reader->frame_time_us = (uint64_t)event->time.tv_sec * 1000000U + (uint64_t)event->time.tv_usec;
		if (reader->device.tablet || reader->device.touch) {
			input_report(input, reader, time);
		} else if (reader->device.touchpad) {
			input_pad_report(input, reader, time);
		} else {
			input_frame(input, reader, time);
		}

		/* The next report starts empty. */
		reader->frame_count = 0;
		return;
	}

	/* Other synchronization events and a damaged report carry nothing to keep. */
	if (event->type == EV_SYN || reader->discarding)
		return;

	/* The exit summary counts every evdev event that was not a synchronization. */
	reader->counted++;

	/* A report too large to hold is thrown away like a dropped one. */
	if (reader->frame_count == KWL_INPUT_THREAD_FRAME_MAX) {
		reader->frame_count = 0;
		reader->discarding = 1;
		return;
	}

	/* Keep the event until its report completes. */
	reader->frame[reader->frame_count] = *event;
	reader->frame_count++;
}

/*
 * Applies a mouse's or keyboard's report: the pointer's motion goes into
 * the box now, and the report goes to the main thread with the place it
 * left, its buttons and keys in order, and its wheel.
 */
static void
input_frame(
	struct kwl_input_thread *input,
	struct kwl_input_reader *reader,
	uint32_t time)
{
	struct kwl_input_event *event;
	struct kwl_input_frame *frame;
	const struct input_event *raw;
	int64_t delta_x;
	int64_t delta_y;
	int64_t moved_x;
	int64_t moved_y;
	unsigned absolute_seen;
	unsigned index;

	/* The event that carries the report to the main thread. */
	event = input_event_new(input, reader, KWL_INPUT_EVENT_FRAME, time);
	frame = NULL;
	if (event != NULL)
		frame = &event->u.frame;

	/* Collect relative movement, wheel steps and the latest absolute position. */
	delta_x = 0;
	delta_y = 0;
	absolute_seen = 0;
	for (index = 0; index < reader->frame_count; index++) {
		raw = &reader->frame[index];

		/* A key-type event is a button or a key; autorepeat (value 2) is not forwarded. */
		if (raw->type == EV_KEY && frame != NULL && (raw->value == 0 || raw->value == 1)) {
			frame->keys[frame->key_count].code = raw->code;
			frame->keys[frame->key_count].value = raw->value;
			frame->key_count++;
		}

		/* Only a pointer's axes move the pointer. */
		if (!reader->device.pointer)
			continue;

		/* Relative axes add up over the report. */
		if (raw->type == EV_REL) {
			/* Each relative code feeds its own sum. */
			if (raw->code == REL_X) {
				delta_x += raw->value;
			} else if (raw->code == REL_Y) {
				delta_y += raw->value;
			} else if (raw->code == REL_WHEEL && frame != NULL) {
				frame->wheel += raw->value;
			} else if (raw->code == REL_HWHEEL && frame != NULL) {
				frame->horizontal_wheel += raw->value;
			}
		}

		/* Absolute axes replace the device's remembered position. */
		if (raw->type == EV_ABS && reader->device.absolute) {
			/* Each axis is remembered separately, as a report may carry only one. */
			if (raw->code == ABS_X) {
				reader->abs_x = raw->value;
				absolute_seen = 1;
			} else if (raw->code == ABS_Y) {
				reader->abs_y = raw->value;
				absolute_seen = 1;
			}
		}
	}

	/*
	 * The relative movement at the mouse's speed and acceleration
	 * (ws089-p024), the fractions of a pixel carried to the next report so
	 * that a slow pointer still moves.
	 */
	if (delta_x != 0 || delta_y != 0) {
		if (frame != NULL)
			frame->placed = 1U;
		kwl_pointer_accel_move(&reader->accel, delta_x, delta_y, reader->frame_time_us, input->own_settings.mouse_speed, input->own_settings.mouse_acceleration, &moved_x, &moved_y);
		delta_x = moved_x;
		delta_y = moved_y;
	}

	/*
	 * The pointer's new place in the box: an absolute report maps onto the
	 * anchor, relative movement goes over the outputs shown, across their
	 * shared edges (ws113-p007).  The box is written even when the event
	 * could not be made, so the cursor still follows.
	 */
	if (absolute_seen) {
		if (frame != NULL) {
			frame->placed = 1U;
			frame->absolute = 1U;
			kwl_cursor_box_place(input->box, reader->abs_x, reader->device.abs_x_minimum, reader->device.abs_x_maximum, reader->abs_y, reader->device.abs_y_minimum, reader->device.abs_y_maximum, delta_x, delta_y, reader->frame_time_us, &frame->motion);
			input_motion_times(input, reader, &frame->motion);
		}
	} else if (delta_x != 0 || delta_y != 0) {
		if (frame != NULL) {
			kwl_cursor_box_move(input->box, delta_x, delta_y, reader->frame_time_us, &frame->motion);
			input_motion_times(input, reader, &frame->motion);
		}
	}

	/* The report goes to the main thread in its order. */
	if (event != NULL)
		input_queue(input, event);
}

/*
 * Applies one report of a touch pad: its events go to the touch pad layer,
 * and the layer's actions (their motions written to the box) go to the
 * main thread.
 */
static void
input_pad_report(
	struct kwl_input_thread *input,
	struct kwl_input_reader *reader,
	uint32_t time)
{
	struct kwl_touchpad_actions actions;
	const struct input_event *raw;
	unsigned index;

	/* The report's events, in order. */
	for (index = 0; index < reader->frame_count; index++) {
		raw = &reader->frame[index];
		kwl_touchpad_event(&reader->pad, raw->type, raw->code, raw->value);
	}

	/* The report's end: what the fingers did. */
	kwl_touchpad_frame(&reader->pad, kwl_milliseconds(), &actions);
	input_pad_actions(input, reader, &actions, time);
}

/*
 * Carries out a touch pad layer's motions on the box, at the touch pads'
 * speed (a percentage, the hundredths carried), and passes all its actions
 * to the main thread in order.  A report that brought no action still
 * goes, for its time.
 */
static void
input_pad_actions(
	struct kwl_input_thread *input,
	struct kwl_input_reader *reader,
	const struct kwl_touchpad_actions *actions,
	uint32_t time)
{
	struct kwl_input_event *event;
	struct kwl_input_pad *pad;
	const struct kwl_touchpad_action *action;
	int64_t delta_x;
	int64_t delta_y;
	unsigned index;

	/* The event that carries the actions. */
	event = input_event_new(input, reader, KWL_INPUT_EVENT_TOUCHPAD, time);
	if (event == NULL)
		return;
	pad = &event->u.pad;
	pad->actions = *actions;
	pad->natural_scroll = reader->pad.natural_scroll;

	/* Each motion moves the box now. */
	for (index = 0; index < actions->count; index++) {
		action = &actions->actions[index];
		if (action->kind != KWL_TOUCHPAD_MOTION)
			continue;

		/* Whether the fingers moved at all, which shows a hidden arrow (ws035-p116). */
		delta_x = action->dx;
		delta_y = action->dy;
		if (delta_x != 0 || delta_y != 0)
			pad->nonzero[index] = 1U;

		/* The motion at the touch pads' speed (ws089-p007, p024, a percentage), the hundredths carried. */
		if (input->own_settings.touchpad_speed != 100 && (delta_x != 0 || delta_y != 0)) {
			delta_x = delta_x * input->own_settings.touchpad_speed + input->remainder_x;
			delta_y = delta_y * input->own_settings.touchpad_speed + input->remainder_y;
			input->remainder_x = delta_x % 100;
			input->remainder_y = delta_y % 100;
			delta_x /= 100;
			delta_y /= 100;
		}

		/* The new place, over the outputs shown (ws113-p007). */
		kwl_cursor_box_move(input->box, delta_x, delta_y, reader->frame_time_us, &pad->motions[index]);
		input_motion_times(input, reader, &pad->motions[index]);
	}

	/* The actions go to the main thread in their order. */
	input_queue(input, event);
}

/* Passes a pen tablet's or touch screen's report to the main thread, which applies it (tablet.c, touch.c). */
static void
input_report(
	struct kwl_input_thread *input,
	struct kwl_input_reader *reader,
	uint32_t time)
{
	struct kwl_input_event *event;

	/* The report's events, copied. */
	event = input_event_new(input, reader, KWL_INPUT_EVENT_REPORT, time);
	if (event == NULL)
		return;
	event->u.report.count = reader->frame_count;
	memcpy(event->u.report.events, reader->frame, reader->frame_count * sizeof(reader->frame[0]));

	/* Queued in its order. */
	input_queue(input, event);
}

/* Lets time pass for the touch pads: a tap whose drag did not come completes its click (touchpad.c). */
static void
input_pad_ticks(
	struct kwl_input_thread *input)
{
	struct kwl_touchpad_actions actions;
	struct kwl_input_reader *reader;
	uint64_t now;
	unsigned index;

	/* Every attached touch pad. */
	now = kwl_milliseconds();
	for (index = 0; index < KWL_INPUT_THREAD_DEVICES; index++) {
		reader = &input->readers[index];
		if (!reader->active || !reader->device.touchpad)
			continue;

		/* Its layer's timers, and what they give. */
		kwl_touchpad_tick(&reader->pad, now, &actions);
		if (actions.count != 0)
			input_pad_actions(input, reader, &actions, (uint32_t)now);
	}
}

/*
 * Shortens a wait (milliseconds, -1 for no limit) to the touch pads' next
 * timer, rounded up so that the timer is due when the wait ends.
 */
static int
input_pad_timeout(
	const struct kwl_input_thread *input,
	int timeout_ms)
{
	const struct kwl_input_reader *reader;
	uint64_t due;
	uint64_t next;
	uint64_t now;
	unsigned index;

	/* The earliest timer of every attached touch pad. */
	next = UINT64_MAX;
	for (index = 0; index < KWL_INPUT_THREAD_DEVICES; index++) {
		reader = &input->readers[index];
		if (!reader->active || !reader->device.touchpad)
			continue;
		due = kwl_touchpad_due(&reader->pad);
		if (due < next)
			next = due;
	}

	/* No timer: the wait is as asked. */
	if (next == UINT64_MAX)
		return timeout_ms;

	/* A timer due already ends the wait at once; a later one when it is due. */
	now = kwl_milliseconds();
	if (next <= now)
		return 0;
	if (next - now >= (uint64_t)INT_MAX)
		return timeout_ms;
	if (timeout_ms < 0 || (uint64_t)timeout_ms > next - now)
		return (int)(next - now);

	/* Succeeded: the wait asked for ends first. */
	return timeout_ms;
}

/*
 * Makes an event of a device for the main thread, carrying the device's
 * evdev events counted since its last event; NULL (counted as dropped)
 * when no memory is left.
 */
static struct kwl_input_event *
input_event_new(
	struct kwl_input_thread *input,
	struct kwl_input_reader *reader,
	enum kwl_input_event_kind kind,
	uint32_t time)
{
	struct kwl_input_event *event;

	/* The event, all zero. */
	event = calloc(1, sizeof(*event));
	if (event == NULL) {
		pthread_mutex_lock(&input->input_lock);

		input->dropped++;

		pthread_mutex_unlock(&input->input_lock);
		return NULL;
	}

	/* Its device, its times and the events it brings to the count. */
	event->kind = kind;
	event->slot = reader->device.slot;
	event->time = time;
	event->frame_time_us = reader->frame_time_us;
	event->read_ms = reader->read_ms;
	event->counted = reader->counted;
	reader->counted = 0;

	/* Succeeded: the event, to be filled and queued. */
	return event;
}

/*
 * Queues an event for the main thread after every earlier one, and wakes
 * the main thread when it was not woken for the queue yet.  A device's
 * last event acknowledges the detach it answers in the same section, so
 * the acknowledgement never comes before the device's events.
 */
static void
input_queue(
	struct kwl_input_thread *input,
	struct kwl_input_event *event)
{
	unsigned wake;

	/* After the events before it. */
	pthread_mutex_lock(&input->input_lock);

	if (input->events_tail == NULL) {
		input->events = event;
	} else {
		input->events_tail->next = event;
	}

	/* The main thread is woken once for a queue it has not taken yet. */
	input->events_tail = event;
	wake = 0U;
	if (!input->main_woken) {
		input->main_woken = 1U;
		wake = 1U;
	}

	/* The device's last event acknowledges the detach it answers. */
	if (event->kind == KWL_INPUT_EVENT_DETACHED && event->u.detached.sequence != 0U) {
		input->acked_sequence = event->u.detached.sequence;
		pthread_cond_broadcast(&input->acknowledged);
	}

	pthread_mutex_unlock(&input->input_lock);

	/* The main thread's poll returns. */
	if (wake)
		input_wake(input->wake_main);
}

/* Keeps the error that stopped the input thread and wakes the main thread to hear it. */
static void
input_fail(
	struct kwl_input_thread *input,
	int error)
{
	/* The error, kept for kwl_input_thread_failure. */
	pthread_mutex_lock(&input->input_lock);

	input->failed = error;

	pthread_mutex_unlock(&input->input_lock);

	/* The main thread looks. */
	input_wake(input->wake_main);
}

/* Converts an evdev timestamp to the wrapping millisecond count Wayland uses. */
static uint32_t
input_event_time(
	const struct input_event *event)
{
	uint64_t milliseconds;

	/* Seconds and microseconds become milliseconds, keeping the low 32 bits. */
	milliseconds = (uint64_t)event->time.tv_sec * 1000U;
	milliseconds += (uint64_t)event->time.tv_usec / 1000U;

	/* Succeeded: the event time with an undefined base, as the protocol allows. */
	return (uint32_t)milliseconds;
}

/* Writes one byte to a wake pipe; a full pipe already wakes its reader. */
static void
input_wake(
	int descriptor)
{
	unsigned char byte;
	ssize_t written;

	/* One byte; EAGAIN means the reader has bytes to wake on already. */
	byte = 1U;
	do {
		written = write(descriptor, &byte, 1U);
	} while (written < 0 && errno == EINTR);
}

/*
 * Adds to a motion where its lag went (the KWL CURSOR_LAG line, ws213-p004):
 * when the pass that read its report began to poll, when the poll returned,
 * and when the read returned.
 */
static void
input_motion_times(
	const struct kwl_input_thread *input,
	const struct kwl_input_reader *reader,
	struct kwl_cursor_motion *motion)
{
	/* The pass's wait and the device's read. */
	motion->poll_entered_us = input->poll_entered_us;
	motion->poll_returned_us = input->poll_returned_us;
	motion->read_us = reader->read_us;
}
