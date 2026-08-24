/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
buffer_append(struct buffer *buffer, const void *data, size_t size)
{
	char *replacement;
	if (size > SIZE_MAX - buffer->size) {
		errno = EOVERFLOW;
		return -1;
	}
	if (buffer->size + size > buffer->capacity) {
		size_t capacity = buffer->capacity ? buffer->capacity : 1024;
		while (capacity < buffer->size + size) {
			if (capacity > SIZE_MAX / 2) {
				capacity = buffer->size + size;
				break;
			}
			capacity *= 2;
		}
		replacement = realloc(buffer->data, capacity);
		if (!replacement)
			return -1;
		buffer->data = replacement;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->size, data, size);
	buffer->size += size;
	return 0;
}

static int
buffer_format(struct buffer *buffer, const char *format, ...)
{
	va_list arguments;
	va_list copy;
	int length;
	char *text;
	va_start(arguments, format);
	va_copy(copy, arguments);
	length = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (length < 0) {
		va_end(arguments);
		return -1;
	}
	text = malloc((size_t)length + 1);
	if (!text) {
		va_end(arguments);
		return -1;
	}
	vsnprintf(text, (size_t)length + 1, format, arguments);
	va_end(arguments);
	int result = buffer_append(buffer, text, (size_t)length);
	free(text);
	return result;
}

static unsigned
checksum(const void *data, size_t size)
{
	const unsigned char *bytes = data;
	unsigned sum = 0;
	for (size_t i = 0; i < size; i++)
		sum = (sum + bytes[i]) & 0xffff;
	return sum;
}

int
sccs_read_regular(const char *path, char **data, size_t *size)
{
	struct stat st;
	size_t done = 0;
	int fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st))
		goto fail;
	if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SCCS_MAX_TEXT) {
		errno = EFBIG;
		goto fail;
	}
	*data = malloc((size_t)st.st_size + 1);
	if (!*data)
		goto fail;
	while (done < (size_t)st.st_size) {
		ssize_t n = read(fd, *data + done, (size_t)st.st_size - done);
		if (n < 0 && errno == EINTR)
			continue;
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
	return 0;
fail: {
	int saved = errno;
	if (fd >= 0)
		close(fd);
	errno = saved;
	return -1;
}
}

int
sccs_sid_valid(const char *sid)
{
	unsigned components = 0;
	const char *p = sid;
	while (*p) {
		unsigned value = 0, digits = 0;
		while (*p >= '0' && *p <= '9') {
			if (value > 999 || ++digits > 4)
				return 0;
			value = value * 10 + (*p++ - '0');
		}
		if (!digits || !value)
			return 0;
		components++;
		if (!*p)
			break;
		if (*p++ != '.')
			return 0;
	}
	return components == 2 || components == 4;
}

int
sccs_sid_next(const char *sid, int branch, char *output, size_t size)
{
	unsigned values[4] = {0};
	unsigned count = 0;
	const char *p = sid;
	int n;
	while (*p && count < 4) {
		char *end;
		unsigned long value = strtoul(p, &end, 10);
		if (end == p || value > 9999)
			break;
		values[count++] = (unsigned)value;
		p = end;
		if (*p == '.')
			p++;
		else
			break;
	}
	if (*p || (count != 2 && count != 4)) {
		errno = EINVAL;
		return -1;
	}
	if (count == 4) {
		if (values[3] == 9999) {
			errno = ERANGE;
			return -1;
		}
		n = snprintf(output, size, "%u.%u.%u.%u", values[0], values[1],
			     values[2], values[3] + 1);
	} else {
		if (branch)
			n = snprintf(output, size, "%u.%u.1.1", values[0],
				     values[1]);
		else {
			if (values[1] == 9999) {
				errno = ERANGE;
				return -1;
			}
			n = snprintf(output, size, "%u.%u", values[0],
				     values[1] + 1);
		}
	}
	if (n < 0 || (size_t)n >= size) {
		errno = ENOSPC;
		return -1;
	}
	return 0;
}

static int
delta_append(struct sccs_history *history, struct sccs_delta *delta)
{
	struct sccs_delta *replacement;
	if (history->count == SIZE_MAX / sizeof(*replacement)) {
		errno = EOVERFLOW;
		return -1;
	}
	replacement = realloc(history->deltas,
			      (history->count + 1) * sizeof(*replacement));
	if (!replacement)
		return -1;
	history->deltas = replacement;
	history->deltas[history->count++] = *delta;
	return 0;
}

const struct sccs_delta *
sccs_find(const struct sccs_history *history, const char *sid)
{
	if (!history->count)
		return NULL;
	if (!sid)
		return &history->deltas[history->count - 1];
	for (size_t i = 0; i < history->count; i++)
		if (!strcmp(history->deltas[i].sid, sid))
			return &history->deltas[i];
	return NULL;
}

int
sccs_add(struct sccs_history *history, const char *sid, const char *user,
	 const char *comment, const void *text, size_t size,
	 unsigned predecessor)
{
	struct sccs_delta delta;
	time_t now;
	struct tm value;
	char timestamp[32];
	if (!sccs_sid_valid(sid) || sccs_find(history, sid) ||
	    size > SCCS_MAX_TEXT || memchr(text, SCCS_CONTROL, size)) {
		errno = EINVAL;
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
	if (!delta.sid || !delta.timestamp || !delta.user || !delta.comment ||
	    !delta.text)
		goto fail;
	memcpy(delta.text, text, size);
	delta.text[size] = '\0';
	delta.text_size = size;
	delta.serial = (unsigned)history->count + 1;
	delta.predecessor = predecessor;
	if (delta_append(history, &delta))
		goto fail;
	return 0;
fail:
	free(delta.sid);
	free(delta.timestamp);
	free(delta.user);
	free(delta.comment);
	free(delta.text);
	return -1;
}

int
sccs_remove(struct sccs_history *history, const char *sid)
{
	for (size_t i = 0; i < history->count; i++) {
		if (strcmp(history->deltas[i].sid, sid))
			continue;
		for (size_t j = 0; j < history->count; j++)
			if (history->deltas[j].predecessor ==
			    history->deltas[i].serial) {
				errno = EBUSY;
				return -1;
			}
		free(history->deltas[i].sid);
		free(history->deltas[i].timestamp);
		free(history->deltas[i].user);
		free(history->deltas[i].comment);
		free(history->deltas[i].text);
		memmove(history->deltas + i, history->deltas + i + 1,
			(history->count - i - 1) * sizeof(*history->deltas));
		history->count--;
		return 0;
	}
	errno = ENOENT;
	return -1;
}

static int
parse_header_line(const char *line, struct sccs_delta *delta)
{
	char *copy = strdup(line);
	char *words[9] = {0};
	unsigned count = 0;
	if (!copy)
		return -1;
	for (char *word = strtok(copy, " "); word && count < 9;
	     word = strtok(NULL, " "))
		words[count++] = word;
	if (count != 8 || strcmp(words[0], "\001d") || strcmp(words[1], "D") ||
	    !sccs_sid_valid(words[2])) {
		errno = EINVAL;
		goto fail;
	}
	char *serial_end, *predecessor_end;
	unsigned long serial = strtoul(words[6], &serial_end, 10);
	unsigned long predecessor = strtoul(words[7], &predecessor_end, 10);
	if (*serial_end || *predecessor_end || !serial || serial > UINT_MAX ||
	    predecessor > UINT_MAX) {
		errno = EINVAL;
		goto fail;
	}
	delta->sid = strdup(words[2]);
	delta->timestamp = malloc(strlen(words[3]) + strlen(words[4]) + 2);
	delta->user = strdup(words[5]);
	if (!delta->sid || !delta->timestamp || !delta->user)
		goto fail;
	sprintf(delta->timestamp, "%s %s", words[3], words[4]);
	delta->serial = (unsigned)serial;
	delta->predecessor = (unsigned)predecessor;
	free(copy);
	return 0;
fail:
	free(copy);
	free(delta->sid);
	free(delta->timestamp);
	free(delta->user);
	memset(delta, 0, sizeof(*delta));
	return -1;
}

int
sccs_load(const char *path, struct sccs_history *history)
{
	char *data = NULL;
	size_t size = 0, offset, line_start;
	unsigned expected;
	ssize_t active = -1;
	memset(history, 0, sizeof(*history));
	if (sccs_read_regular(path, &data, &size))
		return -1;
	if (size < 8 || data[0] != SCCS_CONTROL || data[1] != 'h' ||
	    data[7] != '\n') {
		errno = EINVAL;
		goto fail;
	}
	expected = 0;
	for (size_t i = 2; i < 7; i++) {
		if (data[i] < '0' || data[i] > '9') {
			errno = EINVAL;
			goto fail;
		}
		expected = expected * 10 + (unsigned)(data[i] - '0');
	}
	if (checksum(data + 8, size - 8) != expected) {
		errno = EINVAL;
		goto fail;
	}
	offset = 8;
	while (offset < size) {
		line_start = offset;
		while (offset < size && data[offset] != '\n')
			offset++;
		if (offset == size) {
			errno = EINVAL;
			goto fail;
		}
		data[offset++] = '\0';
		char *line = data + line_start;
		if (line[0] != SCCS_CONTROL) {
			if (active < 0) {
				errno = EINVAL;
				goto fail;
			}
			struct sccs_delta *delta = &history->deltas[active];
			size_t length = strlen(line);
			char *replacement =
			    realloc(delta->text, delta->text_size + length + 2);
			if (!replacement)
				goto fail;
			delta->text = replacement;
			memcpy(delta->text + delta->text_size, line, length);
			delta->text_size += length;
			delta->text[delta->text_size++] = '\n';
			delta->text[delta->text_size] = '\0';
			continue;
		}
		if (line[1] == 'd') {
			struct sccs_delta delta;
			memset(&delta, 0, sizeof(delta));
			if (parse_header_line(line, &delta) ||
			    delta_append(history, &delta))
				goto fail;
		} else if (line[1] == 'c' && history->count) {
			free(history->deltas[history->count - 1].comment);
			history->deltas[history->count - 1].comment =
			    strdup(line + 3);
			if (!history->deltas[history->count - 1].comment)
				goto fail;
		} else if (line[1] == 'I') {
			char *end;
			unsigned long serial_value =
			    strtoul(line + 2, &end, 10);
			while (*end == ' ')
				end++;
			if (*end || !serial_value || serial_value > UINT_MAX)
				goto invalid;
			unsigned serial = (unsigned)serial_value;
			active = -1;
			for (size_t i = 0; i < history->count; i++)
				if (history->deltas[i].serial == serial)
					active = (ssize_t)i;
			if (active < 0)
				goto invalid;
		} else if (line[1] == 'E') {
			active = -1;
		} else if (!strchr("seiuUftT", line[1])) {
			goto invalid;
		}
	}
	free(data);
	if (!history->count) {
		errno = EINVAL;
		goto fail_no_data;
	}
	for (size_t i = 0; i < history->count; i++) {
		if (!history->deltas[i].comment)
			history->deltas[i].comment = strdup("");
		if (!history->deltas[i].text)
			history->deltas[i].text = strdup("");
		if (!history->deltas[i].comment || !history->deltas[i].text)
			goto fail_no_data;
	}
	return 0;
invalid:
	errno = EINVAL;
fail:
	free(data);
fail_no_data:
	sccs_free(history);
	return -1;
}

static int
history_serialize(const struct sccs_history *history, struct buffer *output)
{
	struct buffer body = {0};
	for (size_t i = 0; i < history->count; i++) {
		const struct sccs_delta *delta = &history->deltas[i];
		if (!sccs_sid_valid(delta->sid) ||
		    strchr(delta->comment, '\n') ||
		    memchr(delta->text, SCCS_CONTROL, delta->text_size)) {
			errno = EINVAL;
			goto fail;
		}
		if (buffer_format(&body, "\001s 00000/00000/00000\n") ||
		    buffer_format(&body, "\001d D %s %s %s %u %u\n", delta->sid,
				  delta->timestamp, delta->user, delta->serial,
				  delta->predecessor) ||
		    buffer_format(&body, "\001c %s\n\001e\n", delta->comment))
			goto fail;
	}
	if (buffer_append(&body, "\001u\n\001U\n\001t\n",
			  strlen("\001u\n\001U\n\001t\n")) ||
	    (history->description &&
	     buffer_append(&body, history->description,
			   strlen(history->description))) ||
	    buffer_append(&body, "\001T\n", 3))
		goto fail;
	for (size_t i = 0; i < history->count; i++) {
		const struct sccs_delta *delta = &history->deltas[i];
		if (buffer_format(&body, "\001I %u\n", delta->serial) ||
		    buffer_append(&body, delta->text, delta->text_size) ||
		    (delta->text_size &&
		     delta->text[delta->text_size - 1] != '\n' &&
		     buffer_append(&body, "\n", 1)) ||
		    buffer_format(&body, "\001E %u\n", delta->serial))
			goto fail;
	}
	if (buffer_format(output, "\001h%05u\n",
			  checksum(body.data, body.size)) ||
	    buffer_append(output, body.data, body.size))
		goto fail;
	free(body.data);
	return 0;
fail:
	free(body.data);
	return -1;
}

char *
sccs_aux_name(const char *sfile, char prefix)
{
	const char *slash = strrchr(sfile, '/');
	const char *base = slash ? slash + 1 : sfile;
	size_t directory = slash ? (size_t)(slash - sfile + 1) : 0;
	char *result = malloc(strlen(sfile) + 3);
	if (!result)
		return NULL;
	memcpy(result, sfile, directory);
	result[directory] = prefix;
	result[directory + 1] = '.';
	strcpy(result + directory + 2,
	       !strncmp(base, "s.", 2) ? base + 2 : base);
	return result;
}

char *
sccs_gfile_name(const char *sfile)
{
	const char *slash = strrchr(sfile, '/');
	const char *base = slash ? slash + 1 : sfile;
	size_t directory = slash ? (size_t)(slash - sfile + 1) : 0;
	char *result = malloc(strlen(sfile) + 1);
	if (!result)
		return NULL;
	memcpy(result, sfile, directory);
	strcpy(result + directory, !strncmp(base, "s.", 2) ? base + 2 : base);
	return result;
}

static int
write_all(int fd, const void *data, size_t size)
{
	const char *p = data;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += (size_t)n;
		size -= (size_t)n;
	}
	return 0;
}

int
sccs_save(const char *path, const struct sccs_history *history)
{
	struct buffer data = {0};
	char *lock = NULL, *temporary = NULL;
	int lock_fd = -1, output = -1, saved;
	int lock_owned = 0;
	size_t length = strlen(path);
	if (!history->count || history_serialize(history, &data))
		goto fail;
	lock = sccs_aux_name(path, 'z');
	temporary = malloc(length + 16);
	if (!lock || !temporary)
		goto fail;
	lock_fd = open(lock, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (lock_fd < 0 && errno == EEXIST) {
		char contents[32];
		int existing = open(lock, O_RDONLY);
		ssize_t amount = existing >= 0 ? read(existing, contents,
						      sizeof(contents) - 1)
					       : -1;
		if (existing >= 0)
			close(existing);
		if (amount > 0) {
			char *end;
			contents[amount] = '\0';
			long pid_value = strtol(contents, &end, 10);
			if (pid_value > 1 && kill((pid_t)pid_value, 0) < 0 &&
			    errno == ESRCH && !unlink(lock))
				lock_fd = open(
				    lock, O_WRONLY | O_CREAT | O_EXCL, 0600);
		}
	}
	if (lock_fd < 0)
		goto fail;
	lock_owned = 1;
	char pid[32];
	int pid_length = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
	if (write_all(lock_fd, pid, (size_t)pid_length) || fsync(lock_fd) ||
	    close(lock_fd))
		goto fail;
	lock_fd = -1;
	snprintf(temporary, length + 16, "%s.tmp.XXXXXX", path);
	output = mkstemp(temporary);
	if (output < 0 || fchmod(output, 0444) ||
	    write_all(output, data.data, data.size) || fsync(output) ||
	    close(output))
		goto fail;
	output = -1;
	if (rename(temporary, path))
		goto fail;
	unlink(lock);
	free(data.data);
	free(lock);
	free(temporary);
	return 0;
fail:
	saved = errno;
	if (lock_fd >= 0)
		close(lock_fd);
	if (output >= 0)
		close(output);
	if (temporary)
		unlink(temporary);
	if (lock && lock_owned)
		unlink(lock);
	free(data.data);
	free(lock);
	free(temporary);
	errno = saved;
	return -1;
}

void
sccs_free(struct sccs_history *history)
{
	for (size_t i = 0; i < history->count; i++) {
		free(history->deltas[i].sid);
		free(history->deltas[i].timestamp);
		free(history->deltas[i].user);
		free(history->deltas[i].comment);
		free(history->deltas[i].text);
	}
	free(history->deltas);
	free(history->description);
	memset(history, 0, sizeof(*history));
}
