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

#endif
