/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The callers' side of keiland-preview (client.h): the input opened (a
 * regular file only: never a FIFO or a device, which would block or reach
 * a driver), the child started with it and the output (spawn.h), followed
 * until it ends or its time is up, and the PPM it wrote read back.
 */

#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* How long preview_wait sleeps between two looks (ms). */
#define CLIENT_STEP_MS		10L

/* The longest header of a PPM read back. */
#define CLIENT_HEADER_MAX	(PREVIEW_STAMP_MAX + 64U)

/* The longest text --info writes, and its largest size and date accepted. */
#define CLIENT_INFO_MAX		512U
#define CLIENT_INFO_SIDE_MAX	1000000L

/*
 * The output of a picture being made without waiting: a temporary file
 * removed from its folder, open here only.  It lives from
 * preview_picture_begin until the picture is done or given up.
 */
struct preview_output {
	int fd;
};

static int client_read(int fd, struct preview_picture *picture);
static uint64_t client_clock_ms(void);
static int client_temporary(char *path, size_t size);
static int client_number(const unsigned char *data, size_t size, size_t *at, int *number);
static int client_info_parse(char *text, struct preview_info *info);
static int client_info_field(const char *name, const char *value, struct preview_info *info);

/*
 * Starts a preview of an input file into an output file (made, or emptied,
 * for the preview: the user's alone).  Returns 0 with the job (finished at
 * once when it was made in this process), EINVAL for an input that is not
 * a regular file, or an errno value.
 */
int
preview_start(
	const char *input,
	const char *output,
	const struct preview_request *request,
	struct preview_job *job)
{
	unsigned char head[5];
	struct stat status;
	ssize_t got;
	int written;
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

	/* The output, made for the preview. */
	written = open(output, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
	if (written < 0) {
		error = errno;
		(void)close(fd);
		return error;
	}

	/* The child (or this process); both files are its alone from here. */
	job->started_ms = client_clock_ms();
	error = preview_spawn(fd, written, request, &job->child, &job->status);
	(void)close(fd);
	(void)close(written);
	if (error != 0)
		return error;

	/* Made at once in this process, or by a child that runs. */
	job->finished = 1;
	if (job->child != NULL) {
		job->finished = 0;
		job->apart = 1;
	}

	/* Succeeded: the preview is being made. */
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
	int ended;

	/* Finished already. */
	if (job->finished)
		return 1;

	/* Ended (the system freed its record), or still running. */
	ended = preview_child_ended(job->child, &job->status);
	if (ended) {
		job->child = NULL;
		job->finished = 1;
		return 1;
	}

	/* Past its time: ended and collected. */
	now = client_clock_ms();
	if (now - job->started_ms > job->limit_ms) {
		job->status = preview_child_end(job->child);
		job->child = NULL;
		job->finished = 1;
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
	struct preview_output *output;
	char path[256];
	int error;

	/* The output's record. */
	memset(pending, 0, sizeof(*pending));
	output = malloc(sizeof(*output));
	if (output == NULL)
		return ENOMEM;

	/* Makes the output, a file whose name goes once the child has it open. */
	output->fd = client_temporary(path, sizeof(path));
	if (output->fd < 0) {
		error = errno;
		free(output);
		return error;
	}

	/* Starts the child writing it, then removes its name: only this process knows the file from here. */
	error = preview_start(input, path, request, &pending->job);
	(void)unlink(path);
	if (error != 0) {
		(void)close(output->fd);
		free(output);
		return error;
	}

	/* Succeeded: the picture is being made. */
	pending->output = output;
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
		(void)lseek(pending->output->fd, 0, SEEK_SET);
		*error = client_read(pending->output->fd, picture);
	}

	/* The output goes (it was removed from its folder already). */
	(void)close(pending->output->fd);
	free(pending->output);
	pending->output = NULL;

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
	if (pending->output != NULL) {
		(void)close(pending->output->fd);
		free(pending->output);
	}

	/* The output is gone. */
	pending->output = NULL;
}

/*
 * Asks keiland-preview what a media file is (--info) and reads its answer
 * (ws157-p013): the input is a descriptor of a regular file (read by the
 * child from its start, whatever its offset), the output a temporary file
 * only this process knows.  Returns 0 with *info, the child's status as
 * an errno value (preview_status_error), EPROTO for an answer that is not
 * --info's, or an errno value.
 */
int
preview_info_read(
	int input,
	struct preview_info *info)
{
	struct preview_request request;
	struct preview_job job;
	struct stat status;
	char path[256];
	char text[CLIENT_INFO_MAX + 1U];
	ssize_t got;
	int regular;
	int output;
	int result;
	int error;

	/* Nothing known yet. */
	memset(info, 0, sizeof(*info));

	/* A regular file only. */
	error = fstat(input, &status);
	if (error != 0)
		return errno;
	regular = S_ISREG(status.st_mode);
	if (!regular)
		return EINVAL;

	/* The output, a file removed from its folder at once. */
	output = client_temporary(path, sizeof(path));
	if (output < 0) {
		error = errno;
		return error;
	}

	/* Its name removed: only this process knows the file from here. */
	(void)unlink(path);

	/* The child, asked for --info, followed until it ends or its time is up. */
	memset(&request, 0, sizeof(request));
	request.info = 1;
	memset(&job, 0, sizeof(job));
	job.limit_ms = PREVIEW_LIMIT_MS;
	job.started_ms = client_clock_ms();
	error = preview_spawn(input, output, &request, &job.child, &job.status);
	if (error != 0) {
		(void)close(output);
		return error;
	}

	/* Made in this process (FreeBSD) it has finished; else it is waited for. */
	job.finished = 1;
	if (job.child != NULL)
		job.finished = 0;
	result = preview_wait(&job);

	/* A child that did not answer. */
	error = preview_status_error(result);
	if (error != 0) {
		(void)close(output);
		return error;
	}

	/* Its answer, read back whole. */
	got = pread(output, text, CLIENT_INFO_MAX, 0);
	(void)close(output);
	if (got <= 0 || (size_t)got >= CLIENT_INFO_MAX)
		return EPROTO;
	text[got] = '\0';

	/* Checked as text a child wrote (not trusted); nothing of a bad answer is kept. */
	error = client_info_parse(text, info);
	if (error != 0) {
		memset(info, 0, sizeof(*info));
		return error;
	}

	/* Succeeded: what the file is. */
	return 0;
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

/*
 * Reads the binary PPM a child wrote (P6, a comment line or none, a size
 * of at most PREVIEW_SIDE_MAX each way, 255, and all the pixels).
 * Returns 0 with the picture, EINVAL for anything else, or ENOMEM.
 */
static int
client_read(
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

/*
 * Reads --info's text: its first line, then name=value lines (names it
 * does not know passed by).  Returns 0, or EPROTO for a text without the
 * first line, a kind, or with a value out of range.
 */
static int
client_info_parse(
	char *text,
	struct preview_info *info)
{
	char *line;
	char *next;
	char *equals;
	int first;
	int kinds;
	int same;
	int error;

	/* Each line. */
	first = 1;
	kinds = 0;
	for (line = text; *line != '\0'; line = next) {
		/* The line's end. */
		next = strchr(line, '\n');
		if (next == NULL)
			return EPROTO;
		*next = '\0';
		next++;

		/* The first line names the answer and its version. */
		if (first) {
			same = strcmp(line, "keiland-preview-info 1");
			if (same != 0)
				return EPROTO;
			first = 0;
			continue;
		}

		/* A field. */
		equals = strchr(line, '=');
		if (equals == NULL)
			return EPROTO;
		*equals = '\0';
		error = client_info_field(line, equals + 1, info);
		if (error != 0)
			return error;
		same = strcmp(line, "kind");
		if (same == 0)
			kinds++;
	}

	/* One kind at least. */
	if (first || kinds == 0)
		return EPROTO;

	/* Succeeded: read. */
	return 0;
}

/* Reads one field of --info's text into *info; 0, or EPROTO for a value out of range. */
static int
client_info_field(
	const char *name,
	const char *value,
	struct preview_info *info)
{
	static const char *const kinds[] = {"none", "jpeg", "png", "gif", "video"};
	char *end;
	long long number;
	size_t index;
	int same;
	int width;
	int height;
	int zone;
	int utc;

	/* The kind, by its name (KL_MEDIA_* are their places). */
	same = strcmp(name, "kind");
	if (same == 0) {
		for (index = 0U; index < sizeof(kinds) / sizeof(kinds[0]); index++) {
			same = strcmp(value, kinds[index]);
			if (same == 0) {
				info->kind = (int)index;
				return 0;
			}
		}

		/* No kind it knows. */
		return EPROTO;
	}

	/* An empty date: none known. */
	same = strcmp(name, "taken");
	if (same == 0 && value[0] == '\0') {
		info->dated = 0;
		return 0;
	}

	/* The date's zone: only UTC is said. */
	zone = strcmp(name, "zone");
	if (zone == 0) {
		utc = strcmp(value, "utc");
		if (utc != 0)
			return EPROTO;
		info->utc = 1;
		return 0;
	}

	/* Names it does not know. */
	width = strcmp(name, "width");
	height = strcmp(name, "height");
	if (width != 0 && height != 0 && same != 0)
		return 0;

	/* A decimal number, nothing after it. */
	if (value[0] == '\0')
		return EPROTO;
	errno = 0;
	number = strtoll(value, &end, 10);
	if (*end != '\0' || errno != 0)
		return EPROTO;

	/* The date. */
	if (same == 0) {
		info->taken = (int64_t)number;
		info->dated = 1;
		return 0;
	}

	/* A side, within reason. */
	if (number < 0 || number > CLIENT_INFO_SIDE_MAX)
		return EPROTO;
	if (width == 0) {
		info->width = (long)number;
	} else {
		info->height = (long)number;
	}

	/* Succeeded: the field. */
	return 0;
}
