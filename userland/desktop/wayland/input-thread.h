/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input thread (input-thread.c, ws213-p004; the design is
 * plan/ws213/phase001/phase.md 6.1).
 *
 * The input thread reads the evdev devices, assembles their reports in
 * the order they were made (BUG-142), runs the touch pads' layer and the
 * mouse's acceleration, and writes the pointer's place into the cursor's
 * box (cursor-box.h) as each report comes, whatever the main thread is
 * doing.  Every report then goes to the main thread through one ordered
 * queue, which applies it as before: the clients' events, the hover, the
 * shell's gestures.  The input thread never calls Vulkan, the backend's
 * seat or the server's state.
 *
 * The main thread owns the devices' opening and closing.  It hands a
 * device's descriptor to the input thread with an attach command and takes
 * it back with a detach, whose acknowledgement comes once the input thread
 * has stopped using the descriptor (the main thread looks for it each
 * pass, kwl_input_thread_acked, or waits for it, kwl_input_thread_detach_wait);
 * only then is the descriptor closed or given back to the seat, so a
 * descriptor number reused by another file is never read as a device.
 */

#ifndef KWL_INPUT_THREAD_H
#define KWL_INPUT_THREAD_H

#include "cursor-box.h"
#include "pointer-accel.h"
#include "touchpad.h"

#include "userland/desktop/libkeiland-backend/keiland-backend-evdev.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

/* How many devices the seat reads at once, and how many events one report holds at most (kwl.h's limits). */
#define KWL_INPUT_THREAD_DEVICES	32U
#define KWL_INPUT_THREAD_FRAME_MAX	64U

/* How long the main thread waits for the input thread to let go of a device, in milliseconds (design 6.1). */
#define KWL_INPUT_DETACH_WAIT_MS	100U

/* What one event of the queue to the main thread carries. */
enum kwl_input_event_kind {
	/* A mouse's or keyboard's report: the pointer's motion, buttons, keys and wheel. */
	KWL_INPUT_EVENT_FRAME = 1,
	/* A touch pad layer's actions, its motions already written to the box. */
	KWL_INPUT_EVENT_TOUCHPAD,
	/* A pen tablet's or touch screen's report, which the main thread applies itself. */
	KWL_INPUT_EVENT_REPORT,
	/* The device's last event: the input thread no longer reads it. */
	KWL_INPUT_EVENT_DETACHED
};

/* Why a device was detached. */
enum kwl_input_detach_reason {
	/* The main thread asked (a close, a pause, a device gone from the seat). */
	KWL_INPUT_DETACH_ASKED = 1,
	/* The device ended (end of file, or a read error other than "nothing ready"). */
	KWL_INPUT_DETACH_ENDED,
	/* The seat revoked it (ENODEV): logind may give it back later. */
	KWL_INPUT_DETACH_REVOKED
};

/* What one command of the main thread asks of the input thread. */
enum kwl_input_command_kind {
	/* Read a device's descriptor from now on. */
	KWL_INPUT_COMMAND_ATTACH = 1,
	/* Stop reading a device and acknowledge it. */
	KWL_INPUT_COMMAND_DETACH
};

/*
 * One key-type event of a report (a button or a key), in the order the
 * device reported it.
 */
struct kwl_input_key {
	uint16_t code;
	int32_t value;
};

/*
 * One device's settings that the input thread needs, copied from the
 * main thread's record when it is attached: what kind it is, an absolute
 * pointer's axis ranges and first place, and a touch pad's resolution and
 * range.
 */
struct kwl_input_attach {
	unsigned slot;
	int fd;
	unsigned pointer;
	unsigned keyboard;
	unsigned absolute;
	unsigned tablet;
	unsigned touch;
	unsigned touchpad;
	int32_t abs_x_minimum;
	int32_t abs_x_maximum;
	int32_t abs_y_minimum;
	int32_t abs_y_maximum;
	int32_t abs_x;
	int32_t abs_y;
	int32_t resolution_x;
	int32_t resolution_y;
	int32_t pad_x_minimum;
	int32_t pad_x_maximum;
	int32_t pad_y_minimum;
	int32_t pad_y_maximum;
};

/*
 * The user's settings the input thread applies (settings.c owns them):
 * the mouse's speed and acceleration, the touch pads' speed (a
 * percentage), acceleration and scrolling direction; and the shell's ask of
 * the touch pads, whether two fingers moving down are a gesture (App Home
 * shows, ws181-p012).
 */
struct kwl_input_settings {
	int32_t mouse_speed;
	int32_t mouse_acceleration;
	int32_t touchpad_speed;
	int32_t touchpad_acceleration;
	int32_t touchpad_natural;
	int32_t touchpad_down;
};

/*
 * One command on the main thread's list to the input thread; the main
 * thread allocates it, the input thread frees it.
 */
struct kwl_input_command {
	struct kwl_input_command *next;
	enum kwl_input_command_kind kind;
	struct kwl_input_attach attach;
	uint64_t sequence;
};

/*
 * A mouse's or keyboard's report as the main thread applies it: the
 * pointer's place after its motion (written to the box already), whether
 * the device moved or placed the pointer at all (which shows a hidden
 * arrow even when the place is the same), its wheel, and its buttons and
 * keys in order.
 */
struct kwl_input_frame {
	unsigned placed;
	unsigned absolute;
	struct kwl_cursor_motion motion;
	int32_t wheel;
	int32_t horizontal_wheel;
	unsigned key_count;
	struct kwl_input_key keys[KWL_INPUT_THREAD_FRAME_MAX];
};

/*
 * A touch pad layer's actions, with each motion's place after it was
 * written to the box, whether the motion was nonzero before the speed was
 * applied, and the pad's scrolling direction then.
 */
struct kwl_input_pad {
	struct kwl_touchpad_actions actions;
	struct kwl_cursor_motion motions[KWL_TOUCHPAD_ACTIONS];
	unsigned nonzero[KWL_TOUCHPAD_ACTIONS];
	int32_t natural_scroll;
};

/*
 * A pen tablet's or touch screen's report, its events copied for the
 * main thread to apply.
 */
struct kwl_input_report {
	unsigned count;
	struct input_event events[KWL_INPUT_THREAD_FRAME_MAX];
};

/*
 * A device's last event: why it was detached, the error that ended it,
 * and the detach command it answers (0 for one the input thread decided).
 */
struct kwl_input_detached {
	enum kwl_input_detach_reason reason;
	int error;
	uint64_t sequence;
};

/*
 * One event of the ordered queue from the input thread to the main
 * thread.  The input thread allocates it; the main thread frees it once
 * applied.  time is the report's time as Wayland gives it (milliseconds);
 * frame_time_us its evdev time; read_ms when the input thread read it
 * (the last input a quiet layer picture waits on); counted how many
 * evdev events other than synchronizations it brought (the exit summary).
 */
struct kwl_input_event {
	struct kwl_input_event *next;
	enum kwl_input_event_kind kind;
	unsigned slot;
	uint32_t time;
	uint64_t frame_time_us;
	uint64_t read_ms;
	unsigned counted;
	union {
		struct kwl_input_frame frame;
		struct kwl_input_pad pad;
		struct kwl_input_report report;
		struct kwl_input_detached detached;
	} u;
};

/*
 * One device as the input thread reads it: its descriptor, its kind, an
 * absolute pointer's ranges and last place, the report being gathered,
 * when its last read returned (read_ms for the layers' quiet, read_us for
 * the box's lag, ws213-p004), its acceleration and its touch pad layer.
 * Only the input thread touches it.
 */
struct kwl_input_reader {
	unsigned active;
	struct kwl_input_attach device;
	int32_t abs_x;
	int32_t abs_y;
	unsigned discarding;
	unsigned frame_count;
	unsigned counted;
	struct input_event frame[KWL_INPUT_THREAD_FRAME_MAX];
	uint64_t frame_time_us;
	uint64_t read_ms;
	uint64_t read_us;
	struct kwl_pointer_accel accel;
	struct kwl_touchpad pad;
};

/*
 * Reads a device's events (libkeiland-backend's kl_backend_input_read in
 * the compositor, a test's own reader on the host): the whole events read,
 * 0 at the end of the device, or -1 with errno.
 */
typedef ssize_t kwl_input_read_fn(int descriptor, struct input_event *events, size_t capacity);

/*
 * The input thread and what it shares.
 *
 * Under the cursor lock (the box's): the command list, the settings and
 * their generation, and the stop request.  Under input_lock: the event
 * queue to the main thread, whether the main thread has been woken for it,
 * the last detach acknowledged (acknowledged waits on it), the events
 * dropped for want of memory and the error that stopped the thread.  The two
 * pipes wake each side: the input thread polls wake_input, the main thread
 * polls the read end of the wake pipe it gave (wake_main is its write
 * end).  The readers, the settings' copy, the touch pads' speed fractions,
 * the reader function and when the pass's poll began and returned (for the
 * box's lag, ws213-p004) are the input thread's own.
 */
struct kwl_input_thread {
	struct kwl_cursor_box *box;
	struct kwl_input_command *commands;
	struct kwl_input_command *commands_tail;
	struct kwl_input_settings settings;
	uint64_t settings_generation;
	unsigned stop;
	pthread_mutex_t input_lock;
	pthread_cond_t acknowledged;
	struct kwl_input_event *events;
	struct kwl_input_event *events_tail;
	unsigned main_woken;
	uint64_t acked_sequence;
	unsigned dropped;
	int failed;
	int wake_input[2];
	int wake_main;
	struct kwl_input_reader readers[KWL_INPUT_THREAD_DEVICES];
	struct kwl_input_settings own_settings;
	uint64_t own_generation;
	int64_t remainder_x;
	int64_t remainder_y;
	kwl_input_read_fn *read;
	uint64_t poll_entered_us;
	uint64_t poll_returned_us;
	uint64_t next_sequence;
	pthread_t thread;
	unsigned started;
};

int kwl_input_thread_init(struct kwl_input_thread *input, struct kwl_cursor_box *box, int wake_main, kwl_input_read_fn *read, const struct kwl_input_settings *settings);
int kwl_input_thread_start(struct kwl_input_thread *input);
void kwl_input_thread_stop(struct kwl_input_thread *input);
void kwl_input_thread_destroy(struct kwl_input_thread *input);
int kwl_input_thread_step(struct kwl_input_thread *input, int timeout_ms);
int kwl_input_thread_attach(struct kwl_input_thread *input, const struct kwl_input_attach *device);
int kwl_input_thread_detach(struct kwl_input_thread *input, unsigned slot, uint64_t *sequence);
int kwl_input_thread_detach_wait(struct kwl_input_thread *input, uint64_t sequence, unsigned wait_ms);
uint64_t kwl_input_thread_acked(struct kwl_input_thread *input);
void kwl_input_thread_settings(struct kwl_input_thread *input, const struct kwl_input_settings *settings);
struct kwl_input_event *kwl_input_thread_take(struct kwl_input_thread *input);
int kwl_input_thread_failure(struct kwl_input_thread *input, unsigned *dropped);

#endif
