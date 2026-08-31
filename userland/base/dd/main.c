/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD dd userland command.
 */

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

static int option(struct dd_options *o, const char *arg);
static int parse_conv(struct dd_options *o, const char *value);
static int number(const char *text, uint64_t *result);
static int mul(uint64_t a, uint64_t b, uint64_t *out);
static int skip_input(int fd, unsigned char *buffer, const struct dd_options *o);
static ssize_t read_retry(int fd, void *b, size_t n);
static int seek_output(int fd, const struct dd_options *o);
static int write_all(int fd, const unsigned char *b, size_t n, struct dd_stats *s, int full);
static void statistics(const struct dd_stats *s);
static void on_signal(int signo);

/*
 * Runs the dd command.
 */
int
main(
	int argc,
	char **argv)
{
	int flags;
	int saved;
	size_t take;
	ssize_t got;
	size_t amount, at;
	struct dd_options o = {0};
	struct dd_stats stats = {0};
	unsigned char *inbuf, *outbuf;
	size_t used;
	uint64_t records;
	int in, out, close_in, close_out, failed, i;
	struct sigaction sa = {0};

	/* Process each remaining command-line operand. */
	inbuf = NULL;
	outbuf = NULL;
	used = 0;
	records = 0;
	in = STDIN_FILENO;
	out = STDOUT_FILENO;
	close_in = 0;
	close_out = 0;
	failed = 0;
	o.ibs = o.obs = DD_DEFAULT_BLOCK;
	for (i = 1; i < argc; i++) {
		/* Validates the command-line arguments. */
		if (option(&o, argv[i])) {
			fprintf(stderr, "dd: invalid operand: %s\n", argv[i]);

			/* Reports operation failure. */
			return 2;
		}
	}
	inbuf = malloc(o.ibs);
	outbuf = malloc(o.obs);

	/* Handles the inbuf condition. */
	if (!inbuf || !outbuf) {
		command_error("dd", NULL);
		free(inbuf);
		free(outbuf);

		/* Reports operation failure. */
		return 1;
	}
	sa.sa_handler = (uint64_t)(uintptr_t)on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	/* Handles a failed open operation. */
	if (o.input && (in = open(o.input, O_RDONLY)) < 0) {
		command_error("dd", o.input);
		failed = 1;
		goto done;
	}
	close_in = o.input != NULL;

	/* Handles the o condition. */
	if (o.output) {
		flags = O_WRONLY | O_CREAT;

		/* Handles the o condition. */
		if (!o.notrunc && o.seek == 0)
			flags |= O_TRUNC;
		out = open(o.output, flags, 0666);

		/* Handles the out condition. */
		if (out < 0) {
			command_error("dd", o.output);
			failed = 1;
			goto done;
		}
		close_out = 1;
	}

	/* Handles the skip input condition. */
	if (skip_input(in, inbuf, &o)) {
		command_error("dd", o.input);
		failed = 1;
		goto done;
	}

	/* Handles the o condition. */
	if (o.seek && seek_output(out, &o)) {
		command_error("dd", o.output);
		failed = 1;
		goto done;
	}
	while (!interrupted && (!o.have_count || records < o.count)) {
		got = read_retry(in, inbuf, o.ibs);
		at = 0;

		/* Handles the got condition. */
		if (got == 0)
			break;

		/* Handles the got condition. */
		if (got < 0) {
			saved = errno;
			command_error("dd", o.input);

			/* Handles an operation failure. */
			if (!o.noerror) {
				failed = 1;
				break;
			}

			/* Handles a failed lseek operation. */
			if (lseek(in, (off_t)o.ibs, SEEK_CUR) < 0) {
				errno = saved;
				failed = 1;
				break;
			}
			records++;

			/* Handles the o condition. */
			if (!o.sync)
				continue;
			memset(inbuf, 0, o.ibs);
			amount = o.ibs;
			stats.in_partial++;
		} else {
			records++;
			amount = (size_t)got;

			/* Handles the amount condition. */
			if (amount == o.ibs)
				stats.in_full++;
			else
				stats.in_partial++;

			/* Handles the o condition. */
			if (o.sync && amount < o.ibs) {
				memset(inbuf + amount, 0, o.ibs - amount);
				amount = o.ibs;
			}
		}
		while (at < amount) {
			take = o.obs - used;

			/* Handles the take condition. */
			if (take > amount - at)
				take = amount - at;
			memcpy(outbuf + used, inbuf + at, take);
			used += take;
			at += take;

			/* Checks the current capacity usage. */
			if (used == o.obs) {
				/* Handles the write all condition. */
				if (write_all(out, outbuf, used, &stats, 1)) {
					command_error("dd", o.output);
					failed = 1;
					goto done;
				}
				used = 0;
			}
		}
	}

	/* Handles an operation failure. */
	if (used && !failed && write_all(out, outbuf, used, &stats, 0)) {
		command_error("dd", o.output);
		failed = 1;
	}
done:

	/* Handles an operation failure. */
	if (close_in && close(in) && !failed) {
		command_error("dd", o.input);
		failed = 1;
	}

	/* Handles an operation failure. */
	if (close_out && close(out) && !failed) {
		command_error("dd", o.output);
		failed = 1;
	}
	statistics(&stats);
	free(inbuf);
	free(outbuf);

	/* Returns the computed result. */
	return interrupted ? 130 : failed ? 1 : 0;
}

/* Supports the option operation. */
static int
option(
	struct dd_options *o,
	const char *arg)
{
	int function_result;
	const char *eq;
	uint64_t value;

	eq = strchr(arg, '=');

	/* Handles the eq condition. */
	if (!eq || eq == arg)
		return -1;
#define KEY(k)                                                                 \
	((size_t)(eq - arg) == sizeof(k) - 1U &&                               \
	 !memcmp(arg, k, sizeof(k) - 1U))

	/* Handles the current operation condition. */
	if (KEY("if")) {
		o->input = eq + 1;

		/* Returns the computed result. */
		return *o->input ? 0 : -1;
	}

	/* Handles the of condition. */
	if (KEY("of")) {
		o->output = eq + 1;

		/* Returns the computed result. */
		return *o->output ? 0 : -1;
	}

	/* Handles the conv condition. */
	if (KEY("conv")) {
		/* Obtains the parse conv result. */
		function_result = parse_conv(o, eq + 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the number condition. */
	if (number(eq + 1, &value))
		return -1;

	/* Handles the ibs condition. */
	if (KEY("ibs")) {
		/* Validates the current value. */
		if (!value || value > DD_MAX_BLOCK)
			return -1;
		o->ibs = (size_t)value;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the obs condition. */
	if (KEY("obs")) {
		/* Validates the current value. */
		if (!value || value > DD_MAX_BLOCK)
			return -1;
		o->obs = (size_t)value;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the bs condition. */
	if (KEY("bs")) {
		/* Validates the current value. */
		if (!value || value > DD_MAX_BLOCK)
			return -1;
		o->ibs = o->obs = (size_t)value;

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the remaining item count. */
	if (KEY("count")) {
		o->count = value;
		o->have_count = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the skip condition. */
	if (KEY("skip")) {
		o->skip = value;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the seek condition. */
	if (KEY("seek")) {
		o->seek = value;

		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return -1;
#undef KEY
}

/* Supports the parse conv operation. */
static int
parse_conv(
	struct dd_options *o,
	const char *value)
{
	const char *end;
	size_t n;
	const char *p;

	/* Continue while the operation condition remains true. */
	p = value;
	while (*p) {
		end = strchr(p, ',');
		n = end ? (size_t)(end - p) : strlen(p);

		/* Checks the current item count. */
		if (n == 4 && !memcmp(p, "sync", 4))
			o->sync = 1;
		else if (n == 7 && !memcmp(p, "noerror", 7))
			o->noerror = 1;
		else if (n == 7 && !memcmp(p, "notrunc", 7))
			o->notrunc = 1;
		else

			/* Reports operation failure. */
			return -1;

		/* Checks the current endpoint. */
		if (!end)
			break;
		p = end + 1;

		/* Checks the current pointer. */
		if (!*p)
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the number operation. */
static int
number(
	const char *text,
	uint64_t *result)
{
	unsigned d;
	uint64_t value, factor, piece;
	const char *p, *end;
	int first;

	value = 0;
	factor = 1;
	p = text;
	first = 1;

	/* Checks the current pointer. */
	if (!p || !*p)
		return -1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		piece = 0;

		/* Checks the current pointer. */
		if (*p < '0' || *p > '9')
			return -1;

		/* Continue while the operation condition remains true. */
		while (*p >= '0' && *p <= '9') {
			d = (unsigned)(*p++ - '0');

			/* Handles the piece condition. */
			if (piece > (UINT64_MAX - d) / 10U)
				return -1;
			piece = piece * 10U + d;
		}

		/* Dispatch the selected operation case. */
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

		/* Handles the mul condition. */
		if (mul(piece, factor, &piece))
			return -1;

		/* Handles the first condition. */
		if (first) {
			value = piece;
			first = 0;
		} else if (mul(value, piece, &value))

			/* Reports operation failure. */
			return -1;

		/* Checks the current pointer. */
		if (*p != 'x' && *p != 'X')
			break;
		p++;

		/* Checks the current pointer. */
		if (!*p)
			return -1;
	}
	end = p;

	/* Checks the current endpoint. */
	if (*end != '\0')
		return -1;
	*result = value;
	/* Reports successful completion. */
	return 0;
}

/* Supports the mul operation. */
static int
mul(
	uint64_t a,
	uint64_t b,
	uint64_t *out)
{
	/* Handles the b condition. */
	if (b && a > UINT64_MAX / b)
		return -1;
	*out = a * b;
	/* Reports successful completion. */
	return 0;
}

/* Supports the skip input operation. */
static int
skip_input(
	int fd,
	unsigned char *buffer,
	const struct dd_options *o)
{
	ssize_t n;
	uint64_t bytes, i;

	/* Handles a failed mul operation. */
	if (mul(o->skip, o->ibs, &bytes))
		return -1;

	/* Handles the bytes condition. */
	if (bytes == 0)
		return 0;

	/* Handles a failed lseek operation. */
	if (bytes <= ((sizeof(off_t) == 4) ? INT32_MAX : INT64_MAX) &&
	    lseek(fd, (off_t)bytes, SEEK_CUR) >= 0)

		/* Reports successful completion. */
		return 0;

	/* Handles the reported system error. */
	if (errno != ESPIPE && errno != EINVAL)
		return -1;

	/* Process each element required by the operation. */
	for (i = 0; i < o->skip; i++) {
		n = read_retry(fd, buffer, o->ibs);

		/* Checks the current item count. */
		if (n <= 0) {
			/* Checks the current item count. */
			if (n == 0)
				errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the read retry operation. */
static ssize_t
read_retry(
	int fd,
	void *b,
	size_t n)
{
	ssize_t r;

	do

	/* Continue while the operation condition remains true. */
		r = read(fd, b, n);
	while (r < 0 && errno == EINTR && !interrupted);

	/* Returns the computed result. */
	return r;
}

/* Supports the seek output operation. */
static int
seek_output(
	int fd,
	const struct dd_options *o)
{
	int function_result;
	uint64_t bytes;

	/* Handles a failed mul operation. */
	if (mul(o->seek, o->obs, &bytes) ||
	    bytes > (uint64_t)((sizeof(off_t) == 4) ? INT32_MAX : INT64_MAX)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	/* Computes the function result. */
	function_result = lseek(fd, (off_t)bytes, SEEK_SET) < 0 ? -1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the write all operation. */
static int
write_all(
	int fd,
	const unsigned char *b,
	size_t n,
	struct dd_stats *s,
	int full)
{
	ssize_t w;
	size_t at;

	/* Continue while the operation condition remains true. */
	at = 0;
	while (at < n) {
		w = write(fd, b + at, n - at);

		/* Handles the reported system error. */
		if (w < 0 && errno == EINTR && !interrupted)
			continue;

		/* Handles the w condition. */
		if (w <= 0)
			return -1;
		at += (size_t)w;
		s->bytes += (uint64_t)w;
	}

	/* Handles the full condition. */
	if (full)
		s->out_full++;
	else
		s->out_partial++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the statistics operation. */
static void
statistics(
	const struct dd_stats *s)
{
	fprintf(
	    stderr,
	    "%llu+%llu records in\n%llu+%llu records out\n%llu bytes "
	    "transferred\n",
	    (unsigned long long)s->in_full, (unsigned long long)s->in_partial,
	    (unsigned long long)s->out_full, (unsigned long long)s->out_partial,
	    (unsigned long long)s->bytes);
}

/* Supports the on signal operation. */
static void
on_signal(
	int signo)
{
	(void)signo;
	interrupted = 1;
}
