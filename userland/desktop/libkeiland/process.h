/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The processes' parts the library's files share (process.c, the
 * platform's file, ui/app.c): the record of a process, the calls of the
 * application's dispatch, and the platform's calls.  Nothing here leaves
 * the library.
 */

#ifndef KEIUI_PROCESS_H
#define KEIUI_PROCESS_H

#include <keiland/keiland.h>

#include "userland/desktop/libkeiland/platform/platform.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Where a system action is: its answer awaited (its streams' descriptors
 * gather in its child), started (its threads serve the descriptors), or
 * ended without running or without its end told (refused, or lost).
 */
#define KEIUI_ADMIN_ASKING	1U
#define KEIUI_ADMIN_STARTED	2U
#define KEIUI_ADMIN_ENDED	3U

/*
 * The descriptors a system action's start brings, in the order of the
 * system's stream event's which (input, output, error, status).
 */
#define KEIUI_ADMIN_INPUT	0U
#define KEIUI_ADMIN_OUTPUT	1U
#define KEIUI_ADMIN_ERROR	2U
#define KEIUI_ADMIN_STATUS	3U

/* The longest status line of a system action kept, with its NUL ("SIGNAL n" and its end fit). */
#define KEIUI_STATUS_LINE	32U

/*
 * One stream's bytes: a ring of KL_PROCESS_BUFFER bytes (where the oldest
 * is and how many wait), and whether it ended (nothing more will come).
 */
struct keiui_stream {
	unsigned char *bytes;
	size_t first;
	size_t count;
	int ended;
};

/*
 * A process.  The lock holds everything below it that the platform's
 * threads touch: the streams, what is to be told, the end and the link to
 * the application (NULL once the program closed the process or the
 * application closed: no thread rings the application's wake after it).
 * The fields above the lock are the application's thread's.
 */
struct kl_process {
	/* The application's list of its processes, and whether an event of it waits in the queue. */
	struct kl_process *app_next;
	int queued;

	/* The program's pointer and the streams it uses (KL_PROCESS_*). */
	void *data;
	unsigned streams;

	/*
	 * A system action's (KL_VERSION 111): where it is (KEIUI_ADMIN_*; 0
	 * for a process the application started itself), and while its answer
	 * is awaited the system it was asked through and the request's number.
	 */
	unsigned admin;
	struct kl_system *admin_system;
	uint32_t admin_request;

	struct keiui_lock lock;
	struct kl_app *app;

	/* The output and the error read, and the input waiting to be written. */
	struct keiui_stream output;
	struct keiui_stream error;
	struct keiui_stream input;

	/* What is to be told of the input and the end (KL_PROCESS_READY_INPUT, _EXIT), and whether a short write wants the input's room told. */
	unsigned ready;
	int input_wanted;

	/* The input: to be closed once written (close_input), and closed or broken (writes refused). */
	int input_closing;
	int input_closed;

	/* The end: how and its code (KL_PROCESS_*), and whether it was reaped. */
	unsigned how;
	int code;
	int reaped;

	/* The program closed the process, and the references (the program's and each thread's). */
	int closed;
	unsigned references;

	/* The platform's part: the child and its descriptors. */
	struct keiui_child child;
};

/* The application's dispatch (process.c): the processes' events queued, an event taken, and every process closed. */
void keiui_processes_queue(struct kl_app *app);
void keiui_process_taken(struct kl_app_event *event);
void keiui_processes_close(struct kl_app *app);

/* The platform's threads (process.c): what changed is told to the application (under the lock), and a reference dropped. */
void keiui_process_tell(struct kl_process *process);
void keiui_process_release(struct kl_process *process);

/* The streams (process.c): room in a stream, and bytes put into it or taken out (under the lock). */
size_t keiui_stream_room(const struct keiui_stream *stream);
void keiui_stream_put(struct keiui_stream *stream, const void *bytes, size_t size);
size_t keiui_stream_take(struct keiui_stream *stream, void *bytes, size_t size);

/* The platform's part (platform-posix.c). */
int keiui_child_start(struct kl_process *process, const struct kl_process_options *options);
void keiui_child_poke(struct kl_process *process);
int keiui_child_signal(struct kl_process *process, unsigned how);
int keiui_child_resize(struct kl_process *process, unsigned columns, unsigned rows);
int keiui_child_mode(struct kl_process *process, unsigned *mode);
void keiui_child_end(struct kl_process *process);

/* A launch started apart from the application, with the environment's changes (platform-posix.c). */
int keiui_launch_start(const struct kl_launch_options *options, const char *const *environment);

/*
 * A system action's child (platform-posix.c): its control pipe made before
 * it is asked, a descriptor its start brings kept (KEIUI_ADMIN_*; a second
 * of one kind is closed), the descriptors kept closed (a refusal), and the
 * threads started on them (its start).
 */
int keiui_child_await(struct kl_process *process);
void keiui_child_take(struct kl_process *process, unsigned which, int descriptor);
void keiui_child_drop(struct kl_process *process);
int keiui_child_adopt(struct kl_process *process);

/* A descriptor that has nowhere to go, closed (platform-posix.c). */
void keiui_descriptor_close(int descriptor);

/*
 * A system action's news, as the system's events bring them on the
 * application's thread (process.c): a stream's descriptor, the start, a
 * refusal (a KL_ADMIN_* reason), and the end of the answer's way (the
 * system closed or the compositor went).  The application's dispatch has
 * the system's events read while an answer is awaited.
 */
void keiui_process_admin_stream(struct kl_process *process, unsigned which, int descriptor);
void keiui_process_admin_started(struct kl_process *process);
void keiui_process_admin_refused(struct kl_process *process, unsigned reason);
void keiui_process_admin_lost(struct kl_process *process);
void keiui_processes_admin_dispatch(struct kl_app *app);

/*
 * Reads a system action's status line ("EXIT n" or "SIGNAL n", its end
 * kept or not) as how it ended (KL_PROCESS_EXITED or KL_PROCESS_SIGNALED
 * and the number), or KL_PROCESS_LOST with 0 for anything else (process.c).
 */
void keiui_process_status_parse(const char *line, unsigned *how, int *code);

/*
 * A system action asked through the system (system.c): the run on the
 * application's connection with its window's surface, and its request's
 * number (0, EINVAL, ENOMEM or ENOTSUP); a request taken back; and the
 * system's events taken while an answer is awaited.
 */
struct wl_display;
struct wl_surface;
int keiui_system_admin_run(struct kl_system *system, struct kl_process *process, struct wl_display *display, const char *action, const char *const *argv, unsigned streams, struct wl_surface *surface, uint32_t *request);
void keiui_system_admin_withdraw(struct kl_system *system, uint32_t request);
void keiui_system_admin_dispatch(struct kl_system *system);

#endif
