/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DD_DEFAULT_BLOCK 512U
#define DD_MAX_BLOCK (16U * 1024U * 1024U)

struct dd_options {
	const char *input, *output;
	size_t ibs, obs;
	uint64_t count, skip, seek;
	int have_count, sync, noerror, notrunc;
};
struct dd_stats {
	uint64_t in_full, in_partial, out_full, out_partial, bytes;
};
static volatile int interrupted;

static void
on_signal(int signo)
{
	(void)signo;
	interrupted = 1;
}
static int
mul(uint64_t a, uint64_t b, uint64_t *out)
{
	if (b && a > UINT64_MAX / b)
		return -1;
	*out = a * b;
	return 0;
}
static int
number(const char *text, uint64_t *result)
{
	uint64_t value = 0, factor = 1, piece;
	const char *p = text, *end;
	int first = 1;
	if (!p || !*p)
		return -1;
	for (;;) {
		piece = 0;
		if (*p < '0' || *p > '9')
			return -1;
		while (*p >= '0' && *p <= '9') {
			unsigned d = (unsigned)(*p++ - '0');
			if (piece > (UINT64_MAX - d) / 10U)
				return -1;
			piece = piece * 10U + d;
		}
		switch (*p) {
		case 'c':
			factor = 1;
			p++;
			break;
		case 'w':
			factor = 2;
			p++;
			break;
		case 'b':
			factor = 512;
			p++;
			break;
		case 'k':
		case 'K':
			factor = 1024;
			p++;
			break;
		case 'M':
			factor = 1024ULL * 1024ULL;
			p++;
			break;
		case 'G':
			factor = 1024ULL * 1024ULL * 1024ULL;
			p++;
			break;
		default:
			factor = 1;
			break;
		}
		if (mul(piece, factor, &piece))
			return -1;
		if (first) {
			value = piece;
			first = 0;
		} else if (mul(value, piece, &value))
			return -1;
		if (*p != 'x' && *p != 'X')
			break;
		p++;
		if (!*p)
			return -1;
	}
	end = p;
	if (*end != '\0')
		return -1;
	*result = value;
	return 0;
}
static int
parse_conv(struct dd_options *o, const char *value)
{
	const char *p = value;
	while (*p) {
		const char *end = strchr(p, ',');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (n == 4 && !memcmp(p, "sync", 4))
			o->sync = 1;
		else if (n == 7 && !memcmp(p, "noerror", 7))
			o->noerror = 1;
		else if (n == 7 && !memcmp(p, "notrunc", 7))
			o->notrunc = 1;
		else
			return -1;
		if (!end)
			break;
		p = end + 1;
		if (!*p)
			return -1;
	}
	return 0;
}
static int
option(struct dd_options *o, const char *arg)
{
	const char *eq = strchr(arg, '=');
	uint64_t value;
	if (!eq || eq == arg)
		return -1;
#define KEY(k)                                                                 \
	((size_t)(eq - arg) == sizeof(k) - 1U &&                               \
	 !memcmp(arg, k, sizeof(k) - 1U))
	if (KEY("if")) {
		o->input = eq + 1;
		return *o->input ? 0 : -1;
	}
	if (KEY("of")) {
		o->output = eq + 1;
		return *o->output ? 0 : -1;
	}
	if (KEY("conv"))
		return parse_conv(o, eq + 1);
	if (number(eq + 1, &value))
		return -1;
	if (KEY("ibs")) {
		if (!value || value > DD_MAX_BLOCK)
			return -1;
		o->ibs = (size_t)value;
		return 0;
	}
	if (KEY("obs")) {
		if (!value || value > DD_MAX_BLOCK)
			return -1;
		o->obs = (size_t)value;
		return 0;
	}
	if (KEY("bs")) {
		if (!value || value > DD_MAX_BLOCK)
			return -1;
		o->ibs = o->obs = (size_t)value;
		return 0;
	}
	if (KEY("count")) {
		o->count = value;
		o->have_count = 1;
		return 0;
	}
	if (KEY("skip")) {
		o->skip = value;
		return 0;
	}
	if (KEY("seek")) {
		o->seek = value;
		return 0;
	}
	return -1;
#undef KEY
}
static ssize_t
read_retry(int fd, void *b, size_t n)
{
	ssize_t r;
	do
		r = read(fd, b, n);
	while (r < 0 && errno == EINTR && !interrupted);
	return r;
}
static int
write_all(int fd, const unsigned char *b, size_t n, struct dd_stats *s,
	  int full)
{
	size_t at = 0;
	while (at < n) {
		ssize_t w = write(fd, b + at, n - at);
		if (w < 0 && errno == EINTR && !interrupted)
			continue;
		if (w <= 0)
			return -1;
		at += (size_t)w;
		s->bytes += (uint64_t)w;
	}
	if (full)
		s->out_full++;
	else
		s->out_partial++;
	return 0;
}
static int
skip_input(int fd, unsigned char *buffer, const struct dd_options *o)
{
	uint64_t bytes, i;
	if (mul(o->skip, o->ibs, &bytes))
		return -1;
	if (bytes == 0)
		return 0;
	if (bytes <= ((sizeof(off_t) == 4) ? INT32_MAX : INT64_MAX) &&
	    lseek(fd, (off_t)bytes, SEEK_CUR) >= 0)
		return 0;
	if (errno != ESPIPE && errno != EINVAL)
		return -1;
	for (i = 0; i < o->skip; i++) {
		ssize_t n = read_retry(fd, buffer, o->ibs);
		if (n <= 0) {
			if (n == 0)
				errno = EINVAL;
			return -1;
		}
	}
	return 0;
}
static int
seek_output(int fd, const struct dd_options *o)
{
	uint64_t bytes;
	if (mul(o->seek, o->obs, &bytes) ||
	    bytes > (uint64_t)((sizeof(off_t) == 4) ? INT32_MAX : INT64_MAX)) {
		errno = EOVERFLOW;
		return -1;
	}
	return lseek(fd, (off_t)bytes, SEEK_SET) < 0 ? -1 : 0;
}
static void
statistics(const struct dd_stats *s)
{
	fprintf(
	    stderr,
	    "%llu+%llu records in\n%llu+%llu records out\n%llu bytes "
	    "transferred\n",
	    (unsigned long long)s->in_full, (unsigned long long)s->in_partial,
	    (unsigned long long)s->out_full, (unsigned long long)s->out_partial,
	    (unsigned long long)s->bytes);
}
int
main(int argc, char **argv)
{
	struct dd_options o = {0};
	struct dd_stats stats = {0};
	unsigned char *inbuf = NULL, *outbuf = NULL;
	size_t used = 0;
	uint64_t records = 0;
	int in = STDIN_FILENO, out = STDOUT_FILENO, close_in = 0, close_out = 0,
	    failed = 0, i;
	struct sigaction sa = {0};
	o.ibs = o.obs = DD_DEFAULT_BLOCK;
	for (i = 1; i < argc; i++)
		if (option(&o, argv[i])) {
			fprintf(stderr, "dd: invalid operand: %s\n", argv[i]);
			return 2;
		}
	inbuf = malloc(o.ibs);
	outbuf = malloc(o.obs);
	if (!inbuf || !outbuf) {
		command_error("dd", NULL);
		free(inbuf);
		free(outbuf);
		return 1;
	}
	sa.sa_handler = (uint64_t)(uintptr_t)on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	if (o.input && (in = open(o.input, O_RDONLY)) < 0) {
		command_error("dd", o.input);
		failed = 1;
		goto done;
	}
	close_in = o.input != NULL;
	if (o.output) {
		int flags = O_WRONLY | O_CREAT;
		if (!o.notrunc && o.seek == 0)
			flags |= O_TRUNC;
		out = open(o.output, flags, 0666);
		if (out < 0) {
			command_error("dd", o.output);
			failed = 1;
			goto done;
		}
		close_out = 1;
	}
	if (skip_input(in, inbuf, &o)) {
		command_error("dd", o.input);
		failed = 1;
		goto done;
	}
	if (o.seek && seek_output(out, &o)) {
		command_error("dd", o.output);
		failed = 1;
		goto done;
	}
	while (!interrupted && (!o.have_count || records < o.count)) {
		ssize_t got = read_retry(in, inbuf, o.ibs);
		size_t amount, at = 0;
		if (got == 0)
			break;
		if (got < 0) {
			int saved = errno;
			command_error("dd", o.input);
			if (!o.noerror) {
				failed = 1;
				break;
			}
			if (lseek(in, (off_t)o.ibs, SEEK_CUR) < 0) {
				errno = saved;
				failed = 1;
				break;
			}
			records++;
			if (!o.sync)
				continue;
			memset(inbuf, 0, o.ibs);
			amount = o.ibs;
			stats.in_partial++;
		} else {
			records++;
			amount = (size_t)got;
			if (amount == o.ibs)
				stats.in_full++;
			else
				stats.in_partial++;
			if (o.sync && amount < o.ibs) {
				memset(inbuf + amount, 0, o.ibs - amount);
				amount = o.ibs;
			}
		}
		while (at < amount) {
			size_t take = o.obs - used;
			if (take > amount - at)
				take = amount - at;
			memcpy(outbuf + used, inbuf + at, take);
			used += take;
			at += take;
			if (used == o.obs) {
				if (write_all(out, outbuf, used, &stats, 1)) {
					command_error("dd", o.output);
					failed = 1;
					goto done;
				}
				used = 0;
			}
		}
	}
	if (used && !failed && write_all(out, outbuf, used, &stats, 0)) {
		command_error("dd", o.output);
		failed = 1;
	}
done:
	if (close_in && close(in) && !failed) {
		command_error("dd", o.input);
		failed = 1;
	}
	if (close_out && close(out) && !failed) {
		command_error("dd", o.output);
		failed = 1;
	}
	statistics(&stats);
	free(inbuf);
	free(outbuf);
	return interrupted ? 130 : failed ? 1 : 0;
}
