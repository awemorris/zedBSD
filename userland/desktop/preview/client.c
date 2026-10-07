/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The callers' side of keiland-preview (WS168 p004; client.h): the input
 * opened (a regular file only: never a FIFO or a device, which would block
 * or reach a driver), the child started with it and the output, followed
 * until it ends or its time is up, and the PPM it wrote read back.
 */

#include "client.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long preview_wait sleeps between two looks (ms). */
#define CLIENT_STEP_MS		10L

/* The longest header of a PPM read back. */
#define CLIENT_HEADER_MAX	(PREVIEW_STAMP_MAX + 64U)

static uint64_t client_clock_ms(void);
static int client_temporary(char *path, size_t size);
static int client_number(const unsigned char *data, size_t size, size_t *at, int *number);

/*
 * Starts a preview of an input file into an output descriptor (open for
 * writing; the caller closes it after).  Returns 0 with the job (finished
 * at once when it was made in this process), EINVAL for an input that is
 * not a regular file, or an errno value.
 */
int
preview_start(
	const char *input,
	int output,
	const struct preview_request *request,
	struct preview_job *job)
{
	unsigned char head[5];
	struct stat status;
	ssize_t got;
	int regular;
	int error;
	int pdf;
	int fd;

	/* The input: a regular file, opened without waiting. */
	memset(job, 0, sizeof(*job));
	fd = open(input, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return errno;
	error = fstat(fd, &status);
	regular = error == 0 && S_ISREG(status.st_mode);
	if (!regular) {
		(void)close(fd);
		return EINVAL;
	}

	/* How long it may take: a PDF longer. */
	got = pread(fd, head, sizeof(head), 0);
	pdf = got == (ssize_t)sizeof(head) && memcmp(head, "%PDF-", sizeof(head)) == 0;
	job->limit_ms = PREVIEW_LIMIT_MS;
	if (pdf)
		job->limit_ms = PREVIEW_LIMIT_PDF_MS;

	/* The child (or this process). */
	job->started_ms = client_clock_ms();
	error = preview_spawn(fd, output, request, &job->pid, &job->status);
	(void)close(fd);
	if (error != 0)
		return error;
	job->finished = job->pid == 0;
	return 0;
}

/*
 * Looks at a job without waiting: 1 when it has finished (its status
 * kept), 0 while it runs.  A child past its time is ended (SIGKILL) and
 * collected.
 */
int
preview_poll(
	struct preview_job *job)
{
	uint64_t now;
	pid_t ended;
	int status;
	int exited;
	int signalled;

	/* Finished already. */
	if (job->finished)
		return 1;

	/* Ended, or still running. */
	ended = waitpid(job->pid, &status, WNOHANG);
	if (ended == job->pid) {
		job->finished = 1;
		job->status = PREVIEW_KILLED;
		exited = WIFEXITED(status);
		signalled = WIFSIGNALED(status);
		if (exited)
			job->status = WEXITSTATUS(status);
		else if (signalled)
			job->status = PREVIEW_KILLED + WTERMSIG(status);
		return 1;
	}

	/* Past its time: ended and collected. */
	now = client_clock_ms();
	if (now - job->started_ms > job->limit_ms) {
		(void)kill(job->pid, SIGKILL);
		(void)waitpid(job->pid, &status, 0);
		job->finished = 1;
		job->status = PREVIEW_KILLED + SIGKILL;
		return 1;
	}

	/* Still running. */
	return 0;
}

/*
 * Waits for a job to finish.  Returns its status.
 */
int
preview_wait(
	struct preview_job *job)
{
	struct timespec step;
	int finished;

	/* A look every few milliseconds. */
	for (;;) {
		finished = preview_poll(job);
		if (finished)
			return job->status;
		step.tv_sec = 0;
		step.tv_nsec = CLIENT_STEP_MS * 1000000L;
		(void)nanosleep(&step, NULL);
	}
}

/*
 * Makes a preview of a file, waits for it and reads the picture back (the
 * output is a temporary file, removed after).  Returns 0 with the picture
 * (the caller's), the child's status as an errno value
 * (preview_status_error), or an errno value.
 */
int
preview_picture(
	const char *input,
	const struct preview_request *request,
	struct preview_picture *picture)
{
	struct preview_pending pending;
	struct timespec step;
	int finished;
	int error;

	/* Starts the preview. */
	error = preview_picture_begin(input, request, &pending);
	if (error != 0)
		return error;

	/* Waits for its end, a look every few milliseconds. */
	for (;;) {
		finished = preview_picture_follow(&pending, picture, &error);
		if (finished)
			break;
		step.tv_sec = 0;
		step.tv_nsec = CLIENT_STEP_MS * 1000000L;
		(void)nanosleep(&step, NULL);
	}

	/* Reports why there is no picture. */
	if (error != 0)
		return error;

	/* Succeeded: the picture is the caller's. */
	return 0;
}

/*
 * Starts a preview of a file without waiting for it (ws177-p010): its
 * output is a new temporary file only this process knows.  Returns 0 with
 * the pending picture, which preview_picture_follow follows, or an errno
 * value (nothing is then pending).
 */
int
preview_picture_begin(
	const char *input,
	const struct preview_request *request,
	struct preview_pending *pending)
{
	char path[256];
	int error;

	/* Makes the output, a file removed from its folder at once. */
	memset(pending, 0, sizeof(*pending));
	pending->output = client_temporary(path, sizeof(path));
	if (pending->output < 0) {
		error = errno;
		return error;
	}

	/* Removes its name: only this process knows the file from here. */
	(void)unlink(path);

	/* Starts the child writing it. */
	error = preview_start(input, pending->output, request, &pending->job);
	if (error != 0) {
		(void)close(pending->output);
		pending->output = -1;
		return error;
	}

	/* Succeeded: the picture is being made. */
	return 0;
}

/*
 * Looks at a pending picture without waiting.  Returns 0 while it is
 * being made; 1 when it is done, with the picture (the caller's) and
 * *error 0, or *error the reason there is none (preview_status_error, or
 * the reading's errno value).  The output is closed when it is done.
 */
int
preview_picture_follow(
	struct preview_pending *pending,
	struct preview_picture *picture,
	int *error)
{
	int finished;
	int status;

	/* Nothing yet. */
	memset(picture, 0, sizeof(*picture));
	*error = 0;

	/* A child still at work. */
	finished = preview_poll(&pending->job);
	if (!finished)
		return 0;

	/* Reads the picture it made, or tells why it made none. */
	status = pending->job.status;
	*error = preview_status_error(status);
	if (status == PREVIEW_OK) {
		(void)lseek(pending->output, 0, SEEK_SET);
		*error = preview_read(pending->output, picture);
	}

	/* The output goes (it was removed from its folder already). */
	(void)close(pending->output);
	pending->output = -1;

	/* Succeeded: the picture is done, made or not. */
	return 1;
}

/*
 * Gives up a pending picture: the child, still running, is ended and
 * collected, and the output goes.
 */
void
preview_picture_cancel(
	struct preview_pending *pending)
{
	/* A time of nothing ends a running child at the next look. */
	pending->job.limit_ms = 0U;
	pending->job.started_ms = 0U;
	(void)preview_poll(&pending->job);

	/* The output goes. */
	if (pending->output >= 0)
		(void)close(pending->output);
	pending->output = -1;
}

/*
 * Gives a child's exit status as an errno value: 0 for a preview made,
 * EINVAL for an input of an unknown kind or damaged, EFBIG for one too
 * large, ENOMEM for one the child had no memory for, ETIMEDOUT for a child
 * ended (its time was up, or a signal), EIO otherwise.
 */
int
preview_status_error(
	int status)
{
	/* A preview made. */
	if (status == PREVIEW_OK)
		return 0;

	/* An input the program cannot read. */
	if (status == PREVIEW_UNKNOWN)
		return EINVAL;
	if (status == PREVIEW_DAMAGED)
		return EINVAL;

	/* An input over the limits. */
	if (status == PREVIEW_TOO_LARGE)
		return EFBIG;

	/* A child without memory. */
	if (status == PREVIEW_NO_MEMORY)
		return ENOMEM;

	/* A child ended by a signal, its time's or the kernel's. */
	if (status >= PREVIEW_KILLED)
		return ETIMEDOUT;

	/* Anything else: the output could not be written, or the child could not run. */
	return EIO;
}

/*
 * Reads the binary PPM a child wrote (P6, a comment line or none, a size
 * of at most PREVIEW_SIDE_MAX each way, 255, and all the pixels).
 * Returns 0 with the picture, EINVAL for anything else, or ENOMEM.
 */
int
preview_read(
	int fd,
	struct preview_picture *picture)
{
	unsigned char header[CLIENT_HEADER_MAX];
	unsigned char *bytes;
	size_t at;
	size_t count;
	size_t index;
	size_t done;
	ssize_t got;
	int maximum;
	int error;

	/* The header's bytes. */
	memset(picture, 0, sizeof(*picture));
	got = read(fd, header, sizeof(header));
	if (got < 3 || header[0] != 'P' || header[1] != '6' || header[2] != '\n')
		return EINVAL;

	/* A comment line, then the size and 255. */
	at = 3;
	if (header[at] == '#') {
		while (at < (size_t)got && header[at] != '\n')
			at++;
		at++;
	}

	/* The numbers. */
	error = client_number(header, (size_t)got, &at, &picture->width);
	if (error == 0)
		error = client_number(header, (size_t)got, &at, &picture->height);
	if (error == 0)
		error = client_number(header, (size_t)got, &at, &maximum);
	if (error != 0 || maximum != 255 || at >= (size_t)got || header[at] != '\n')
		return EINVAL;
	at++;
	if (picture->width < 1 || picture->height < 1 || picture->width > PREVIEW_SIDE_MAX || picture->height > PREVIEW_SIDE_MAX)
		return EINVAL;

	/* The pixels' bytes: those read with the header, then the rest. */
	count = (size_t)picture->width * (size_t)picture->height;
	bytes = malloc(count * 3U);
	if (bytes == NULL)
		return ENOMEM;
	done = (size_t)got - at;
	if (done > count * 3U)
		done = count * 3U;
	memcpy(bytes, header + at, done);
	while (done < count * 3U) {
		got = read(fd, bytes + done, count * 3U - done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		done += (size_t)got;
	}

	/* All of them. */
	if (done != count * 3U) {
		free(bytes);
		return EINVAL;
	}

	/* The picture, opaque. */
	picture->pixels = malloc(count * sizeof(picture->pixels[0]));
	if (picture->pixels == NULL) {
		free(bytes);
		return ENOMEM;
	}

	/* Each pixel. */
	for (index = 0; index < count; index++)
		picture->pixels[index] = 0xff000000U | ((uint32_t)bytes[index * 3U] << 16) | ((uint32_t)bytes[index * 3U + 1U] << 8) | bytes[index * 3U + 2U];
	free(bytes);
	return 0;
}

/*
 * Frees a picture read back.
 */
void
preview_picture_release(
	struct preview_picture *picture)
{
	/* The pixels. */
	free(picture->pixels);
	memset(picture, 0, sizeof(*picture));
}

/* The time in milliseconds (the monotonic clock). */
static uint64_t
client_clock_ms(void)
{
	struct timespec now;

	/* Monotonic. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Makes a new temporary file in the user's runtime folder (else /tmp); its descriptor, or -1. */
static int
client_temporary(
	char *path,
	size_t size)
{
	const char *folder;

	/* $XDG_RUNTIME_DIR, else /tmp. */
	folder = getenv("XDG_RUNTIME_DIR");
	if (folder == NULL || folder[0] != '/')
		folder = "/tmp";
	(void)snprintf(path, size, "%s/keiland-preview-XXXXXX", folder);
	return mkstemp(path);
}

/* Reads a header's number after one blank; 0, or EINVAL. */
static int
client_number(
	const unsigned char *data,
	size_t size,
	size_t *at,
	int *number)
{
	int digits;

	/* A blank before every number but the first of a line. */
	if (*at < size && (data[*at] == ' ' || data[*at] == '\n'))
		(*at)++;

	/* Up to five digits. */
	*number = 0;
	digits = 0;
	while (*at < size && data[*at] >= '0' && data[*at] <= '9' && digits < 5) {
		*number = *number * 10 + (data[*at] - '0');
		(*at)++;
		digits++;
	}

	/* A number. */
	if (digits == 0)
		return EINVAL;
	return 0;
}
