/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Invokes the media database CLI asynchronously while the compositor owns commit notifications. */
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Bounds concurrent helpers and time spent waiting for any one operation. */
#define MEDIA_JOBS_MAX 16U
#define MEDIA_JOB_SECONDS 120
#define MEDIA_INPUT_MAX (4U * 1024U * 1024U)
#define MEDIA_OUTPUT_MAX (64U * 1024U * 1024U)

extern char **environ;

/* One child owns its output stream until the caller takes the completed result. */
struct media_job {
	pid_t process;
	uint32_t id;
	int descriptor;
	int input;
	int output;
	char *request;
	size_t length;
	size_t sent;
	size_t received;
	int error;
	int done;
	time_t started;
};

/* The desktop's media helper queue, owned by its event-loop thread. */
struct kl_backend_media {
	struct media_job jobs[MEDIA_JOBS_MAX];
	uint32_t next_id;
};

static void media_finished(struct media_job *job, int status, int wait_error);
static void media_pump(struct media_job *job);
static void media_release(struct media_job *job);
static int media_output(void);
static int media_pipe(int descriptors[2]);
static int media_nonblock(int descriptor);
static int media_prepare(struct media_job *job, unsigned command, const char *argument, int descriptor);
static int media_spawn(struct media_job *job, const char *operation);
static ssize_t media_write(int descriptor, const void *bytes, size_t length);

/*
 * Opens the asynchronous CLI queue for this desktop.
 */
struct kl_backend_media *
kl_backend_media_open(
	void)
{
	struct kl_backend_media *media;
	size_t index;

	/* Allocates a queue with no child processes or retained streams. */
	media = calloc(1U, sizeof(*media));
	if (media == NULL)
		return NULL;
	media->next_id = 1U;
	for (index = 0U; index < MEDIA_JOBS_MAX; index++) {
		media->jobs[index].descriptor = -1;
		media->jobs[index].input = -1;
		media->jobs[index].output = -1;
	}

	/* Succeeded: database operations are available even before the first import. */
	return media;
}

/*
 * Ends child helpers and releases every retained descriptor.
 */
void
kl_backend_media_close(
	struct kl_backend_media *media)
{
	size_t index;
	int status;
	pid_t waited;

	/* A missing queue owns no operating-system resources. */
	if (media == NULL)
		return;
	for (index = 0U; index < MEDIA_JOBS_MAX; index++) {
		if (media->jobs[index].process > 0) {
			kill(media->jobs[index].process, SIGKILL);
			do {
				waited = waitpid(media->jobs[index].process, &status, 0);
			} while (waited < 0 && errno == EINTR);
		}

		/* A completed response belongs to the backend until taken. */
		media_release(&media->jobs[index]);
	}

	/* Releases the queue after every owned child and descriptor has ended. */
	free(media);
}

/*
 * Starts one CLI operation; the input descriptor is borrowed and copied before returning.
 */
int
kl_backend_media_request(
	struct kl_backend_media *media,
	unsigned command,
	const char *argument,
	int descriptor,
	uint32_t *id)
{
	struct media_job *job;
	const char *operation;
	size_t index;
	int error;

	/* Maps the small public command set to fixed argv entries without a shell. */
	if (media == NULL || id == NULL || argument == NULL)
		return EINVAL;
	switch (command) {
	case KL_BACKEND_MEDIA_LIST:
		operation = "list";
		break;
	case KL_BACKEND_MEDIA_ADD:
		operation = "add-list";
		break;
	case KL_BACKEND_MEDIA_APPLY:
		operation = "apply";
		break;
	default:
		return EINVAL;
	}

	/* Finds an unowned helper slot; pending results still hold theirs. */
	for (index = 0U; index < MEDIA_JOBS_MAX; index++) {
		if (media->jobs[index].id == 0U)
			break;
	}

	/* A full queue is a recoverable busy result for the client. */
	if (index == MEDIA_JOBS_MAX)
		return EBUSY;
	job = &media->jobs[index];
	memset(job, 0, sizeof(*job));
	job->descriptor = -1;
	job->input = -1;
	job->output = -1;

	/* Copies only metadata before starting the helper with two anonymous pipes. */
	error = media_prepare(job, command, argument, descriptor);
	if (error == 0)
		error = media_spawn(job, operation);
	if (error != 0) {
		media_release(job);
		return error;
	}

	/* The event loop pumps both directions so a full pipe cannot freeze the desktop. */
	job->started = time(NULL);
	job->id = media->next_id++;
	if (media->next_id == 0U)
		media->next_id = 1U;
	*id = job->id;

	/* Succeeded: the event loop can continue while the CLI copies or queries media. */
	return 0;
}

/*
 * Reaps finished helpers without blocking the compositor.
 */
void
kl_backend_media_update(
	struct kl_backend_media *media)
{
	struct media_job *job;
	size_t index;
	pid_t waited;
	time_t now;
	int status;
	int wait_error;

	/* A missing queue owns no helper process. */
	if (media == NULL)
		return;

	/* Reaps only children owned by this queue, leaving other desktop helpers alone. */
	now = time(NULL);
	for (index = 0U; index < MEDIA_JOBS_MAX; index++) {
		job = &media->jobs[index];
		if (job->id == 0U || job->done)
			continue;
		media_pump(job);
		if (job->done || job->process <= 0)
			continue;
		if (now - job->started > MEDIA_JOB_SECONDS) {
			kill(job->process, SIGKILL);
			job->error = ETIMEDOUT;
		}

		/* A zero result leaves the child running without blocking this pass. */
		status = 0;
		waited = waitpid(job->process, &status, WNOHANG);
		if (waited == 0 || (waited < 0 && errno == EINTR))
			continue;
		/* Either this queue or Home may reap the same owned helper, never both. */
		wait_error = 0;
		if (waited < 0)
			wait_error = errno;
		media_finished(job, status, wait_error);
	}
}

/*
 * Takes one completed response, transferring descriptor ownership to the caller.
 */
int
kl_backend_media_take(
	struct kl_backend_media *media,
	uint32_t *id,
	int *error,
	int *descriptor)
{
	struct media_job *job;
	size_t index;

	/* A missing backend has no queued results. */
	if (media == NULL)
		return 0;
	for (index = 0U; index < MEDIA_JOBS_MAX; index++) {
		job = &media->jobs[index];
		if (!job->done)
			continue;
		*id = job->id;
		*error = job->error;
		*descriptor = job->descriptor;
		if (job->error != 0) {
			close(job->descriptor);
			*descriptor = -1;
		}

		/* Clears the slot after detaching its response descriptor. */
		memset(job, 0, sizeof(*job));
		job->descriptor = -1;
		job->input = -1;
		job->output = -1;

		/* Succeeded: one result is owned by the caller. */
		return 1;
	}

	/* Succeeded: no completed child is waiting. */
	return 0;
}

/*
 * Accepts an owned helper's exit status when the desktop's general child collector reaps it.
 */
int
kl_backend_media_child(
	struct kl_backend_media *media,
	int64_t child,
	int status)
{
	size_t index;

	/* Other desktop helpers keep their own exit handling. */
	if (media == NULL)
		return 0;
	for (index = 0U; index < MEDIA_JOBS_MAX; index++) {
		if (media->jobs[index].process == (pid_t)child && child > 0) {
			media_finished(&media->jobs[index], status, 0);
			return 1;
		}
	}

	/* Succeeded: this exit does not belong to the media queue. */
	return 0;
}

/* Retires one owned process and makes its rewound metadata response takeable. */
static void
media_finished(
	struct media_job *job,
	int status,
	int wait_error)
{
	int exited;
	int code;

	/* A timeout already recorded by the queue remains the authoritative failure. */
	if (job->error == 0) {
		exited = WIFEXITED(status);
		if (wait_error != 0) {
			job->error = wait_error;
		} else if (!exited) {
			job->error = EIO;
		} else {
			code = WEXITSTATUS(status);
			job->error = code;
			if (code == 127)
				job->error = ENOENT;
		}
	}

	/* Exit status may arrive before the last pipe bytes; drain them before publication. */
	job->process = 0;
	media_pump(job);
}

/* Pumps bounded amounts of metadata in both directions without blocking the desktop. */
static void
media_pump(
	struct media_job *job)
{
	char buffer[4096];
	size_t budget;
	size_t length;
	size_t copied;
	ssize_t count;
	ssize_t wrote;
	off_t offset;

	/* Feed a bounded request chunk, preserving progress across event-loop ticks. */
	budget = 65536U;
	while (job->input >= 0 && job->sent < job->length && budget != 0U) {
		length = job->length - job->sent;
		if (length > sizeof(buffer))
			length = sizeof(buffer);
		count = media_write(job->input, job->request + job->sent, length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (count <= 0) {
			if (job->error == 0)
				job->error = EIO;
			break;
		}

		/* Only accepted bytes advance the request. */
		job->sent += (size_t)count;
		/* Yield after a bounded amount of response work. */
		budget -= (size_t)count;
	}

	/* EOF tells the CLI that the complete path/update list has arrived. */
	if (job->input >= 0 && (job->sent == job->length || job->error != 0 || job->process == 0)) {
		close(job->input);
		job->input = -1;
		free(job->request);
		job->request = NULL;
	}

	/* Drain the response even after an error so the helper cannot wait on a full stdout. */
	budget = 65536U;
	while (job->output >= 0 && budget != 0U) {
		count = read(job->output, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (count <= 0) {
			if (count < 0 && job->error == 0)
				job->error = errno;
			close(job->output);
			job->output = -1;
			break;
		}

		/* The anonymous spool is only for the eventual Wayland metadata FD, never CLI IPC. */
		job->received += (size_t)count;
		if (job->received > MEDIA_OUTPUT_MAX && job->error == 0)
			job->error = EFBIG;
		copied = 0U;
		while (job->error == 0 && copied < (size_t)count) {
			wrote = write(job->descriptor, buffer + copied, (size_t)count - copied);
			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0) {
				job->error = EIO;
				break;
			}

			/* Preserve every byte before publishing the result. */
			copied += (size_t)wrote;
		}

		/* Yield after a bounded amount of response work. */
		budget -= (size_t)count;
	}

	/* Publish only after both process exit and stdout EOF, including externally reaped exits. */
	if (job->process == 0 && job->output < 0) {
		if (job->error == 0) {
			offset = lseek(job->descriptor, 0, SEEK_SET);
			if (offset < 0)
				job->error = errno;
		}

		/* Completion now owns a fully drained snapshot. */
		job->done = 1;
	}
}

/* Releases all pipe, response and request resources after completion or failed setup. */
static void
media_release(
	struct media_job *job)
{
	/* Each descriptor has one owner in the queue. */
	if (job->input >= 0)
		close(job->input);
	if (job->output >= 0)
		close(job->output);
	if (job->descriptor >= 0)
		close(job->descriptor);
	free(job->request);
}

/* Allocates an anonymous regular response stream with no inherited stdio owner. */
static int
media_output(
	void)
{
	FILE *stream;
	int descriptor;
	int error;

	/* tmpfile removes its directory entry while retaining readable file contents. */
	stream = tmpfile();
	if (stream == NULL)
		return -1;
	descriptor = fcntl(fileno(stream), F_DUPFD_CLOEXEC, 3);
	error = errno;
	fclose(stream);
	errno = error;

	/* Succeeded: the descriptor is independent of the temporary stdio stream. */
	return descriptor;
}

/* Creates close-on-exec pipe ends above standard IO, keeping spawn dup2 actions independent. */
static int
media_pipe(
	int descriptors[2])
{
	unsigned index;
	int result;
	int copy;
	int error;

	/* Normalize descriptors so a closed parent stdin/stdout cannot create a dup2 collision. */
	result = pipe(descriptors);
	if (result < 0)
		return errno;
	for (index = 0U; index < 2U; index++) {
		copy = fcntl(descriptors[index], F_DUPFD_CLOEXEC, 3);
		if (copy < 0) {
			error = errno;
			close(descriptors[0]);
			close(descriptors[1]);
			return error;
		}

		/* Replace each low descriptor with its owned close-on-exec copy. */
		close(descriptors[index]);
		descriptors[index] = copy;
	}

	/* Succeeded: only explicitly duplicated stdin/stdout survive exec. */
	return 0;
}

/* Sets only a parent pipe end nonblocking; the child retains conventional blocking IO. */
static int
media_nonblock(
	int descriptor)
{
	int flags;
	int result;

	/* Existing descriptor flags are preserved. */
	flags = fcntl(descriptor, F_GETFL);
	if (flags < 0)
		return errno;
	result = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	if (result < 0)
		return errno;

	/* Succeeded: the event loop can pump this end without waiting. */
	return 0;
}

/* Copies bounded request metadata without transferring picture/video bytes. */
static int
media_prepare(
	struct media_job *job,
	unsigned command,
	const char *argument,
	int descriptor)
{
	struct stat state;
	ssize_t bytes;
	size_t done;
	int result;
	int regular;

	/* Only update snapshots arrive as descriptors; path lists are ordinary strings. */
	if (command == KL_BACKEND_MEDIA_ADD) {
		job->length = strlen(argument);
		if (job->length == 0U || job->length > 32768U)
			return EINVAL;
	} else if (command == KL_BACKEND_MEDIA_APPLY) {
		if (descriptor < 0)
			return EINVAL;
		result = fstat(descriptor, &state);
		if (result < 0)
			return errno;
		regular = S_ISREG(state.st_mode);
		if (!regular || state.st_size < 0 || (uint64_t)state.st_size > MEDIA_INPUT_MAX)
			return EINVAL;
		job->length = (size_t)state.st_size;
	}

	/* Allocate and copy metadata while leaving the borrowed descriptor's offset unchanged. */
	if (job->length != 0U) {
		job->request = malloc(job->length);
		if (job->request == NULL)
			return ENOMEM;
		if (command == KL_BACKEND_MEDIA_ADD) {
			memcpy(job->request, argument, job->length);
		} else {
			done = 0U;
			while (done < job->length) {
				bytes = pread(descriptor, job->request + done, job->length - done, (off_t)done);
				if (bytes < 0 && errno == EINTR)
					continue;
				if (bytes <= 0)
					return EIO;
				done += (size_t)bytes;
			}
		}
	}

	/* Retain a metadata result spool for transfer over the Wayland extension. */
	job->descriptor = media_output();
	if (job->descriptor < 0)
		return errno;

	/* Succeeded: spawn can connect stdin/stdout pipes without inspecting source media. */
	return 0;
}

/* Starts the fixed CLI with checked POSIX spawn actions and ordinary stdin/stdout pipes. */
static int
media_spawn(
	struct media_job *job,
	const char *operation)
{
	posix_spawn_file_actions_t actions;
	char *arguments[3];
	int input[2];
	int output[2];
	int error;

	/* Both child ends block normally; only the parent's ends are pumped asynchronously. */
	error = media_pipe(input);
	if (error != 0)
		return error;
	error = media_pipe(output);
	if (error != 0) {
		close(input[0]);
		close(input[1]);
		return error;
	}

	/* Failed setup closes both pipes without creating a child. */
	error = media_nonblock(input[1]);
	if (error == 0)
		error = media_nonblock(output[0]);
	if (error != 0) {
		close(input[0]);
		close(input[1]);
		close(output[0]);
		close(output[1]);
		return error;
	}

	/* A single owner closes every unused descriptor on the parent side. */
	error = posix_spawn_file_actions_init(&actions);
	if (error == 0) {
		error = posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
		if (error == 0)
			error = posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
		arguments[0] = KEILAND_BINDIR "/mediastorage";
		arguments[1] = (char *)operation;
		arguments[2] = NULL;
		if (error == 0)
			error = posix_spawn(&job->process, arguments[0], &actions, NULL, arguments, environ);
		posix_spawn_file_actions_destroy(&actions);
	}

	/* The parent must not retain child pipe ends, which would prevent EOF. */
	close(input[0]);
	close(output[1]);
	if (error != 0) {
		close(input[1]);
		close(output[0]);
		return error;
	}

	/* Publish the two parent pipe ends only after a successful spawn. */
	job->input = input[1];
	job->output = output[0];

	/* Succeeded: CLI communication is exclusively through these two pipes. */
	return 0;
}

/* Suppresses only the SIGPIPE generated by this write, preserving the caller's signal state. */
static ssize_t
media_write(
	int descriptor,
	const void *bytes,
	size_t length)
{
	sigset_t blocked;
	sigset_t previous;
	sigset_t pending;
	struct timespec immediate;
	ssize_t result;
	int error;
	int existed;
	int status;

	/* Block SIGPIPE on the event-loop thread while a child may concurrently close stdin. */
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE);
	status = sigprocmask(SIG_BLOCK, &blocked, &previous);
	if (status < 0)
		return -1;
	sigpending(&pending);
	existed = sigismember(&pending, SIGPIPE);
	result = write(descriptor, bytes, length);
	error = errno;
	if (result < 0 && error == EPIPE && !existed) {
		memset(&immediate, 0, sizeof(immediate));
		sigtimedwait(&blocked, NULL, &immediate);
	}

	/* Restore the mask and write's error without affecting unrelated pending signals. */
	sigprocmask(SIG_SETMASK, &previous, NULL);
	errno = error;

	/* Succeeded: the caller advances only the bytes actually written. */
	return result;
}
