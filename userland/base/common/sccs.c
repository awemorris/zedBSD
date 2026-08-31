/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland sccs support.
 */

#include "userland/base/common/sccs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SCCS_CONTROL '\001'
#define SCCS_MAX_TEXT (16U * 1024U * 1024U)

struct buffer {
	char *data;
	size_t size;
	size_t capacity;
};

static int delta_append(struct sccs_history *history, struct sccs_delta *delta);
static unsigned checksum(const void *data, size_t size);
static int parse_header_line(const char *line, struct sccs_delta *delta);
static int history_serialize(const struct sccs_history *history, struct buffer *output);
static int buffer_format(struct buffer *buffer, const char *format, ...);
static int buffer_append(struct buffer *buffer, const void *data, size_t size);
static int write_all(int fd, const void *data, size_t size);

/*
 * Implements the sccs read regular operation.
 */
int
sccs_read_regular(
	const char *path,
	char **data,
	size_t *size)
{
	ssize_t n;
	int saved;
	struct stat st;
	size_t done;
	int fd;

	done = 0;
	fd = open(path, O_RDONLY);

	/* Handles a failed fstat operation. */
	if (fd < 0 || fstat(fd, &st))
		goto fail;

	/* Handles a failed S ISREG operation. */
	if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SCCS_MAX_TEXT) {
		errno = EFBIG;
		goto fail;
	}
	*data = malloc((size_t)st.st_size + 1);
	/* Handles the data condition. */
	if (!*data)
		goto fail;

	/* Process each remaining element. */
	while (done < (size_t)st.st_size) {

		n = read(fd, *data + done, (size_t)st.st_size - done);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0) {
			errno = EIO;
			free(*data);
			*data = NULL;
			goto fail;
		}
		done += (size_t)n;
	}
	(*data)[done] = '\0';
	*size = done;
	close(fd);

	/* Reports successful completion. */
	return 0;
fail:

saved = errno;

/* Checks the file descriptor. */
if (fd >= 0)
	close(fd);
errno = saved;

/* Reports operation failure. */
return -1;
}

/*
 * Implements the sccs sid valid operation.
 */
int
sccs_sid_valid(
	const char *sid)
{
	unsigned value, digits;
	unsigned components;
	const char *p;

	/* Continue while the operation condition remains true. */
	components = 0;
	p = sid;
	while (*p) {
		/* Continue while the operation condition remains true. */
		value = 0;
		digits = 0;
		while (*p >= '0' && *p <= '9') {
			/* Validates the current value. */
			if (value > 999 || ++digits > 4)
				return 0;
			value = value * 10 + (*p++ - '0');
		}

		/* Handles the digits condition. */
		if (!digits || !value)
			return 0;
		components++;

		/* Checks the current pointer. */
		if (!*p)
			break;

		/* Checks the current pointer. */
		if (*p++ != '.')
			return 0;
	}

	/* Returns the computed result. */
	return components == 2 || components == 4;
}

/*
 * Implements the sccs sid next operation.
 */
int
sccs_sid_next(
	const char *sid,
	int branch,
	char *output,
	size_t size)
{
	char *end;
	unsigned long value;
	unsigned values[4] = {0};
	unsigned count;
	const char *p;
	int n;

	/* Process each remaining element. */
	count = 0;
	p = sid;
	while (*p && count < 4) {

		value = strtoul(p, &end, 10);

		/* Checks the current endpoint. */
		if (end == p || value > 9999)
			break;
		values[count++] = (unsigned)value;
		p = end;

		/* Checks the current pointer. */
		if (*p == '.')
			p++;
		else
			break;
	}

	/* Checks the current pointer. */
	if (*p || (count != 2 && count != 4)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the remaining item count. */
	if (count == 4) {
		/* Handles the values condition. */
		if (values[3] == 9999) {
			errno = ERANGE;

			/* Reports operation failure. */
			return -1;
		}
		n = snprintf(output, size, "%u.%u.%u.%u", values[0], values[1],
			     values[2], values[3] + 1);
	} else {
		/* Handles the branch condition. */
		if (branch)
			n = snprintf(output, size, "%u.%u.1.1", values[0],
				     values[1]);
		else {
			/* Handles the values condition. */
			if (values[1] == 9999) {
				errno = ERANGE;

				/* Reports operation failure. */
				return -1;
			}
			n = snprintf(output, size, "%u.%u", values[0],
				     values[1] + 1);
		}
	}

	/* Checks the current item count. */
	if (n < 0 || (size_t)n >= size) {
		errno = ENOSPC;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sccs find operation.
 */
const struct sccs_delta *
sccs_find(
	const struct sccs_history *history,
	const char *sid)
{
	size_t i_index_for;

	/* Handles the history condition. */
	if (!history->count)
		return NULL;

	/* Handles the sid condition. */
	if (!sid)
		return &history->deltas[history->count - 1];

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < history->count; i_index_for++)

		/* Selects the matching value. */
		if (!strcmp(history->deltas[i_index_for].sid, sid))
			return &history->deltas[i_index_for];

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the sccs add operation.
 */
int
sccs_add(
	struct sccs_history *history,
	const char *sid,
	const char *user,
	const char *comment,
	const void *text,
	size_t size,
	unsigned predecessor)
{
	struct sccs_delta delta;
	time_t now;
	struct tm value;
	char timestamp[32];

	/* Handles a failed sccs sid valid operation. */
	if (!sccs_sid_valid(sid) || sccs_find(history, sid) ||
	    size > SCCS_MAX_TEXT || memchr(text, SCCS_CONTROL, size)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(&delta, 0, sizeof(delta));
	time(&now);
	localtime_r(&now, &value);
	strftime(timestamp, sizeof(timestamp), "%Y/%m/%d %H:%M:%S", &value);
	delta.sid = strdup(sid);
	delta.timestamp = strdup(timestamp);
	delta.user = strdup(user && *user ? user : "unknown");
	delta.comment = strdup(comment ? comment : "");
	delta.text = malloc(size + 1);

	/* Handles the delta condition. */
	if (!delta.sid || !delta.timestamp || !delta.user || !delta.comment ||
	    !delta.text)
		goto fail;
	memcpy(delta.text, text, size);
	delta.text[size] = '\0';
	delta.text_size = size;
	delta.serial = (unsigned)history->count + 1;
	delta.predecessor = predecessor;

	/* Handles the delta append condition. */
	if (delta_append(history, &delta))
		goto fail;

	/* Reports successful completion. */
	return 0;
fail:
	free(delta.sid);
	free(delta.timestamp);
	free(delta.user);
	free(delta.comment);
	free(delta.text);

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the sccs remove operation.
 */
int
sccs_remove(
	struct sccs_history *history,
	const char *sid)
{
	size_t i_index_for;
	size_t j_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < history->count; i_index_for++) {
		/* Selects the matching value. */
		if (strcmp(history->deltas[i_index_for].sid, sid))
			continue;

		/* Process each remaining element. */
		for (j_index_for = 0; j_index_for < history->count; j_index_for++)

			/* Handles the history condition. */
			if (history->deltas[j_index_for].predecessor ==
			    history->deltas[i_index_for].serial) {
				errno = EBUSY;

				/* Reports operation failure. */
				return -1;
			}
		free(history->deltas[i_index_for].sid);
		free(history->deltas[i_index_for].timestamp);
		free(history->deltas[i_index_for].user);
		free(history->deltas[i_index_for].comment);
		free(history->deltas[i_index_for].text);
		memmove(history->deltas + i_index_for, history->deltas + i_index_for + 1,
			(history->count - i_index_for - 1) * sizeof(*history->deltas));
		history->count--;

		/* Reports successful completion. */
		return 0;
	}
	errno = ENOENT;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the sccs load operation.
 */
int
sccs_load(
	const char *path,
	struct sccs_history *history)
{
	struct sccs_delta *delta_local;
	struct sccs_delta delta_local1;
	size_t length;
	char *replacement;
	char *end;
	unsigned long serial_value;
	unsigned serial;
	size_t i_index_for;
	size_t i_index_for1;
	size_t i_index_for2;
	char *data;
	size_t size, offset, line_start;
	unsigned expected;
	ssize_t active;
	char *line;

	data = NULL;
	size = 0;
	active = -1;
	memset(history, 0, sizeof(*history));

	/* Handles the sccs read regular condition. */
	if (sccs_read_regular(path, &data, &size))
		return -1;

	/* Checks the current data size. */
	if (size < 8 || data[0] != SCCS_CONTROL || data[1] != 'h' ||
	    data[7] != '\n') {
		errno = EINVAL;
		goto fail;
	}

	/* Process each remaining element. */
	expected = 0;
	for (i_index_for = 2; i_index_for < 7; i_index_for++) {
		/* Handles the data condition. */
		if (data[i_index_for] < '0' || data[i_index_for] > '9') {
			errno = EINVAL;
			goto fail;
		}
		expected = expected * 10 + (unsigned)(data[i_index_for] - '0');
	}

	/* Handles a failed checksum operation. */
	if (checksum(data + 8, size - 8) != expected) {
		errno = EINVAL;
		goto fail;
	}

	/* Process each remaining element. */
	offset = 8;
	while (offset < size) {
		/* Process each remaining element. */
		line_start = offset;
		while (offset < size && data[offset] != '\n')
			offset++;

		/* Checks the current offset. */
		if (offset == size) {
			errno = EINVAL;
			goto fail;
		}
		data[offset++] = '\0';
		line = data + line_start;

		/* Handles the line condition. */
		if (line[0] != SCCS_CONTROL) {
			/* Handles the active condition. */
			if (active < 0) {
				errno = EINVAL;
				goto fail;
			}
			delta_local = &history->deltas[active];
			length = strlen(line);
			replacement =
			    realloc(delta_local->text, delta_local->text_size + length + 2);

			/* Handles the replacement condition. */
			if (!replacement)
				goto fail;
			delta_local->text = replacement;
			memcpy(delta_local->text + delta_local->text_size, line, length);
			delta_local->text_size += length;
			delta_local->text[delta_local->text_size++] = '\n';
			delta_local->text[delta_local->text_size] = '\0';
			continue;
		}

		/* Handles the line condition. */
		if (line[1] == 'd') {

			memset(&delta_local1, 0, sizeof(delta_local1));

			/* Handles the parse header line condition. */
			if (parse_header_line(line, &delta_local1) ||
			    delta_append(history, &delta_local1))
				goto fail;
		} else if (line[1] == 'c' && history->count) {
			free(history->deltas[history->count - 1].comment);
			history->deltas[history->count - 1].comment =
			    strdup(line + 3);

			/* Handles the history condition. */
			if (!history->deltas[history->count - 1].comment)
				goto fail;
		} else if (line[1] == 'I') {
			/* Continue while the operation condition remains true. */
			serial_value = strtoul(line + 2, &end, 10);
			while (*end == ' ')
				end++;

			/* Checks the current endpoint. */
			if (*end || !serial_value || serial_value > UINT_MAX)
				goto invalid;

			/* Process each remaining element. */
			serial = (unsigned)serial_value;
			active = -1;
			for (i_index_for1 = 0; i_index_for1 < history->count; i_index_for1++)

				/* Handles the history condition. */
				if (history->deltas[i_index_for1].serial == serial)
					active = (ssize_t)i_index_for1;

			/* Handles the active condition. */
			if (active < 0)
				goto invalid;
		} else if (line[1] == 'E') {
			active = -1;
		} else if (!strchr("seiuUftT", line[1])) {
			goto invalid;
		}
	}
	free(data);

	/* Handles the history condition. */
	if (!history->count) {
		errno = EINVAL;
		goto fail_no_data;
	}

	/* Process each remaining element. */
	for (i_index_for2 = 0; i_index_for2 < history->count; i_index_for2++) {
		/* Handles the history condition. */
		if (!history->deltas[i_index_for2].comment)
			history->deltas[i_index_for2].comment = strdup("");

		/* Handles the history condition. */
		if (!history->deltas[i_index_for2].text)
			history->deltas[i_index_for2].text = strdup("");

		/* Handles the history condition. */
		if (!history->deltas[i_index_for2].comment || !history->deltas[i_index_for2].text)
			goto fail_no_data;
	}

	/* Reports successful completion. */
	return 0;
invalid:
	errno = EINVAL;
fail:
	free(data);
fail_no_data:
	sccs_free(history);

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the sccs aux name operation.
 */
char *
sccs_aux_name(
	const char *sfile,
	char prefix)
{
	const char *slash;
	const char *base;
	size_t directory;
	char *result;

	slash = strrchr(sfile, '/');
	base = slash ? slash + 1 : sfile;
	directory = slash ? (size_t)(slash - sfile + 1) : 0;
	result = malloc(strlen(sfile) + 3);

	/* Checks the operation result. */
	if (!result)
		return NULL;
	memcpy(result, sfile, directory);
	result[directory] = prefix;
	result[directory + 1] = '.';
	strcpy(result + directory + 2,
	       !strncmp(base, "s.", 2) ? base + 2 : base);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the sccs gfile name operation.
 */
char *
sccs_gfile_name(
	const char *sfile)
{
	const char *slash;
	const char *base;
	size_t directory;
	char *result;

	slash = strrchr(sfile, '/');
	base = slash ? slash + 1 : sfile;
	directory = slash ? (size_t)(slash - sfile + 1) : 0;
	result = malloc(strlen(sfile) + 1);

	/* Checks the operation result. */
	if (!result)
		return NULL;
	memcpy(result, sfile, directory);
	strcpy(result + directory, !strncmp(base, "s.", 2) ? base + 2 : base);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the sccs save operation.
 */
int
sccs_save(
	const char *path,
	const struct sccs_history *history)
{
	char *end;
	long pid_value;
	char contents[32];
	int existing;
	struct buffer data = {0};
	char *lock, *temporary;
	int lock_fd, output, saved;
	int lock_owned;
	size_t length;
	char pid[32];
	int pid_length;

	lock = NULL;
	temporary = NULL;
	lock_fd = -1;
	output = -1;
	lock_owned = 0;
	length = strlen(path);

	/* Handles a failed history serialize operation. */
	if (!history->count || history_serialize(history, &data))
		goto fail;
	lock = sccs_aux_name(path, 'z');
	temporary = malloc(length + 16);

	/* Handles the lock condition. */
	if (!lock || !temporary)
		goto fail;
	lock_fd = open(lock, O_WRONLY | O_CREAT | O_EXCL, 0600);

	/* Handles the reported system error. */
	if (lock_fd < 0 && errno == EEXIST) {

				existing = open(lock, O_RDONLY);
		ssize_t amount = existing >= 0 ? read(existing, contents,
						      sizeof(contents) - 1)
					       : -1;

		/* Handles the existing condition. */
		if (existing >= 0)
			close(existing);

		/* Handles the amount condition. */
		if (amount > 0) {

			contents[amount] = '\0';
			pid_value = strtol(contents, &end, 10);

			/* Handles the reported system error. */
			if (pid_value > 1 && kill((pid_t)pid_value, 0) < 0 &&
			    errno == ESRCH && !unlink(lock))
				lock_fd = open(
				    lock, O_WRONLY | O_CREAT | O_EXCL, 0600);
		}
	}

	/* Handles the lock fd condition. */
	if (lock_fd < 0)
		goto fail;
	lock_owned = 1;
	pid_length = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());

	/* Handles the write all condition. */
	if (write_all(lock_fd, pid, (size_t)pid_length) || fsync(lock_fd) ||
	    close(lock_fd))
		goto fail;
	lock_fd = -1;
	snprintf(temporary, length + 16, "%s.tmp.XXXXXX", path);
	output = mkstemp(temporary);

	/* Handles a failed fchmod operation. */
	if (output < 0 || fchmod(output, 0444) ||
	    write_all(output, data.data, data.size) || fsync(output) ||
	    close(output))
		goto fail;
	output = -1;

	/* Handles the rename condition. */
	if (rename(temporary, path))
		goto fail;
	unlink(lock);
	free(data.data);
	free(lock);
	free(temporary);

	/* Reports successful completion. */
	return 0;
fail:
	saved = errno;

	/* Handles the lock fd condition. */
	if (lock_fd >= 0)
		close(lock_fd);

	/* Handles the output condition. */
	if (output >= 0)
		close(output);

	/* Handles the temporary condition. */
	if (temporary)
		unlink(temporary);

	/* Handles the lock condition. */
	if (lock && lock_owned)
		unlink(lock);
	free(data.data);
	free(lock);
	free(temporary);
	errno = saved;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the sccs free operation.
 */
void
sccs_free(
	struct sccs_history *history)
{
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < history->count; i_index_for++) {
		free(history->deltas[i_index_for].sid);
		free(history->deltas[i_index_for].timestamp);
		free(history->deltas[i_index_for].user);
		free(history->deltas[i_index_for].comment);
		free(history->deltas[i_index_for].text);
	}
	free(history->deltas);
	free(history->description);
	memset(history, 0, sizeof(*history));
}

/* Supports the delta append operation. */
static int
delta_append(
	struct sccs_history *history,
	struct sccs_delta *delta)
{
	struct sccs_delta *replacement;

	/* Handles the history condition. */
	if (history->count == SIZE_MAX / sizeof(*replacement)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	replacement = realloc(history->deltas,
			      (history->count + 1) * sizeof(*replacement));

	/* Handles the replacement condition. */
	if (!replacement)
		return -1;
	history->deltas = replacement;
	history->deltas[history->count++] = *delta;

	/* Reports successful completion. */
	return 0;
}

/* Supports the checksum operation. */
static unsigned
checksum(
	const void *data,
	size_t size)
{
	size_t i_index_for;
	const unsigned char *bytes;
	unsigned sum;

	/* Process each remaining element. */
	bytes = data;
	sum = 0;
	for (i_index_for = 0; i_index_for < size; i_index_for++)
		sum = (sum + bytes[i_index_for]) & 0xffff;

	/* Returns the computed result. */
	return sum;
}

/* Supports the parse header line operation. */
static int
parse_header_line(
	const char *line,
	struct sccs_delta *delta)
{
	char *word_for;
	char *copy;
	char *words[9];
	char *serial_end, *predecessor_end;
	unsigned count;
	unsigned long serial, predecessor;

	copy = strdup(line);
	memset(words, 0, sizeof(words));
	count = 0;

	/* Handles the copy condition. */
	if (!copy)
		return -1;

	/* Process each remaining element. */
	for (word_for = strtok(copy, " "); word_for && count < 9;
	     word_for = strtok(NULL, " "))
		words[count++] = word_for;

	/* Handles a failed sccs sid valid operation. */
	if (count != 8 || strcmp(words[0], "\001d") || strcmp(words[1], "D") ||
	    !sccs_sid_valid(words[2])) {
		errno = EINVAL;
		goto fail;
	}
	serial = strtoul(words[6], &serial_end, 10);
	predecessor = strtoul(words[7], &predecessor_end, 10);

	/* Handles the serial end condition. */
	if (*serial_end || *predecessor_end || !serial || serial > UINT_MAX ||
	    predecessor > UINT_MAX) {
		errno = EINVAL;
		goto fail;
	}
	delta->sid = strdup(words[2]);
	delta->timestamp = malloc(strlen(words[3]) + strlen(words[4]) + 2);
	delta->user = strdup(words[5]);

	/* Handles the delta condition. */
	if (!delta->sid || !delta->timestamp || !delta->user)
		goto fail;
	sprintf(delta->timestamp, "%s %s", words[3], words[4]);
	delta->serial = (unsigned)serial;
	delta->predecessor = (unsigned)predecessor;
	free(copy);

	/* Reports successful completion. */
	return 0;
fail:
	free(copy);
	free(delta->sid);
	free(delta->timestamp);
	free(delta->user);
	memset(delta, 0, sizeof(*delta));

	/* Reports operation failure. */
	return -1;
}

/* Supports the history serialize operation. */
static int
history_serialize(
	const struct sccs_history *history,
	struct buffer *output)
{
	const struct sccs_delta *delta_local;
	const struct sccs_delta *delta_local1;
	size_t i_index_for;
	size_t i_index_for1;
	struct buffer body = {0};

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < history->count; i_index_for++) {
				delta_local = &history->deltas[i_index_for];

		/* Handles a failed sccs sid valid operation. */
		if (!sccs_sid_valid(delta_local->sid) ||
		    strchr(delta_local->comment, '\n') ||
		    memchr(delta_local->text, SCCS_CONTROL, delta_local->text_size)) {
			errno = EINVAL;
			goto fail;
		}

		/* Handles a failed buffer format operation. */
		if (buffer_format(&body, "\001s 00000/00000/00000\n") ||
		    buffer_format(&body, "\001d D %s %s %s %u %u\n", delta_local->sid,
				  delta_local->timestamp, delta_local->user, delta_local->serial,
				  delta_local->predecessor) ||
		    buffer_format(&body, "\001c %s\n\001e\n", delta_local->comment))
			goto fail;
	}

	/* Handles a failed buffer append operation. */
	if (buffer_append(&body, "\001u\n\001U\n\001t\n",
			  strlen("\001u\n\001U\n\001t\n")) ||
	    (history->description &&
	     buffer_append(&body, history->description,
			   strlen(history->description))) ||
	    buffer_append(&body, "\001T\n", 3))
		goto fail;

	/* Process each remaining element. */
	for (i_index_for1 = 0; i_index_for1 < history->count; i_index_for1++) {
				delta_local1 = &history->deltas[i_index_for1];

		/* Handles a failed buffer format operation. */
		if (buffer_format(&body, "\001I %u\n", delta_local1->serial) ||
		    buffer_append(&body, delta_local1->text, delta_local1->text_size) ||
		    (delta_local1->text_size &&
		     delta_local1->text[delta_local1->text_size - 1] != '\n' &&
		     buffer_append(&body, "\n", 1)) ||
		    buffer_format(&body, "\001E %u\n", delta_local1->serial))
			goto fail;
	}

	/* Handles the buffer format condition. */
	if (buffer_format(output, "\001h%05u\n",
			  checksum(body.data, body.size)) ||
	    buffer_append(output, body.data, body.size))
		goto fail;
	free(body.data);

	/* Reports successful completion. */
	return 0;
fail:
	free(body.data);

	/* Reports operation failure. */
	return -1;
}

/* Supports the buffer format operation. */
static int
buffer_format(
	struct buffer *buffer,
	const char *format,
	...)
{
	va_list arguments;
	va_list copy;
	int length;
	int result;
	char *text;

	va_start(arguments, format);
	va_copy(copy, arguments);
	length = vsnprintf(NULL, 0, format, copy);
	va_end(copy);

	/* Checks the current data length. */
	if (length < 0) {
		va_end(arguments);

		/* Reports operation failure. */
		return -1;
	}
	text = malloc((size_t)length + 1);

	/* Validates the current text. */
	if (!text) {
		va_end(arguments);

		/* Reports operation failure. */
		return -1;
	}
	vsnprintf(text, (size_t)length + 1, format, arguments);
	va_end(arguments);
	result = buffer_append(buffer, text, (size_t)length);
	free(text);

	/* Returns the computed result. */
	return result;
}

/* Supports the buffer append operation. */
static int
buffer_append(
	struct buffer *buffer,
	const void *data,
	size_t size)
{
	size_t capacity;
	char *replacement;

	/* Checks the current data size. */
	if (size > SIZE_MAX - buffer->size) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the buffer condition. */
	if (buffer->size + size > buffer->capacity) {
		/* Process each remaining element. */
				capacity = buffer->capacity ? buffer->capacity : 1024;
		while (capacity < buffer->size + size) {
			/* Handles the capacity condition. */
			if (capacity > SIZE_MAX / 2) {
				capacity = buffer->size + size;
				break;
			}
			capacity *= 2;
		}
		replacement = realloc(buffer->data, capacity);

		/* Handles the replacement condition. */
		if (!replacement)
			return -1;
		buffer->data = replacement;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->size, data, size);
	buffer->size += size;

	/* Reports successful completion. */
	return 0;
}

/* Supports the write all operation. */
static int
write_all(
	int fd,
	const void *data,
	size_t size)
{
	ssize_t n;
	const char *p;

	/* Process each remaining element. */
	p = data;
	while (size) {

		n = write(fd, p, size);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0)
			return -1;
		p += (size_t)n;
		size -= (size_t)n;
	}

	/* Reports successful completion. */
	return 0;
}
