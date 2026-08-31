/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland pager support.
 */

#include "userland/base/common/pager.h"
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#define PIPE_LIMIT (256U * 1024U)
#define LINE_READ_MAX 8192U
struct document {
	int fd, owned, seekable;
	off_t size;
	off_t *line;
	size_t lines, cap;
	unsigned char *memory;
	size_t memory_size;
	const char *name;
};
struct terminal {
	int fd, active;
	struct termios saved;
	unsigned rows, cols;
};
static volatile int stopped;

static int terminal_open(struct terminal *t, int data_is_stdin);
static int out(const void *b, size_t n);
static int copy_fd(int fd);
static int load_document(struct document *d, const char *name);
static int add_line(struct document *d, off_t value);
static int index_bytes(struct document *d, const unsigned char *b, size_t n, off_t base);
static void close_document(struct document *d);
static int interactive(struct document *d, struct terminal *t, enum pager_style style, int numbers, size_t first);
static int render(const struct document *d, size_t top, unsigned horizontal, const struct terminal *t, int numbers, const char *prompt);
static int put_line(const struct document *d, size_t line, unsigned cols, unsigned horizontal, int numbers);
static ssize_t line_bytes(const struct document *d, size_t line, unsigned char *b, size_t cap);
static int key_read(int fd);
static int search_prompt(struct terminal *t, const struct document *d, size_t top, size_t *found);
static int find_forward(const struct document *d, size_t start, const char *pattern, size_t *found);
static void terminal_close(struct terminal *t);
static void stop_handler(int signo);

/*
 * Implements the pager main operation.
 */
int
pager_main(
	enum pager_style style,
	int argc,
	char **argv)
{
	const char *name_local;
	const char *name_local1;
	int i;
	int fd;
	struct document d;
	int index, numbers, failed, data_stdin;
	size_t first;
	struct terminal terminal;
	struct sigaction action;

	/* Process each remaining command-line operand. */
	index = 1;
	numbers = 0;
	failed = 0;
	data_stdin = 0;
	first = 0;
	while (index < argc && argv[index][0] == '-' && argv[index][1]) {
		/* Handles the selected command-line operation. */
		if (style == PAGER_LESS && !strcmp(argv[index], "-N"))
			numbers = 1;
		else
			break;
		index++;
	}

	/* Validates the command-line arguments. */
	if (index < argc && argv[index][0] == '+' && argv[index][1]) {
		first = (size_t)strtoul(argv[index] + 1, NULL, 10);

		/* Handles the first condition. */
		if (first)
			first--;
		index++;
	}

	/* Validates the command-line arguments. */
	if (index == argc) {
		data_stdin = 1;
	} else {
		/* Process each remaining command-line operand. */
		for (i = index; i < argc; i++) {
			/* Handles the selected command-line operation. */
			if (!strcmp(argv[i], "-"))
				data_stdin = 1;
		}
	}

	/* Validates the command-line arguments. */
	if (index == argc && isatty(STDIN_FILENO)) {
		fprintf(stderr, "%s: missing file operand\n",
			style == PAGER_MORE ? "more" : "less");

		/* Reports operation failure. */
		return 1;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = (uint64_t)(uintptr_t)stop_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);

	/* Handles a failed terminal open operation. */
	if (!terminal_open(&terminal, data_stdin)) {
		do {
						name_local = index == argc ? "-" : argv[index];
						fd = !strcmp(name_local, "-") ? STDIN_FILENO
						    : open(name_local, O_RDONLY);

			/* Handles a failed copy fd operation. */
			if (fd < 0 || copy_fd(fd)) {
				command_error(style == PAGER_MORE ? "more"
								  : "less",
					      name_local);
				failed = 1;
			}

			/* Checks the file descriptor. */
			if (fd >= 0 && fd != STDIN_FILENO)
				close(fd);
			index++;
		} while (index < argc);

		/* Returns the computed result. */
		return failed;
	}
	do {
				name_local1 = index == argc ? "-" : argv[index];

		/* Handles the load document condition. */
		if (load_document(&d, name_local1)) {
			command_error(style == PAGER_MORE ? "more" : "less",
				      name_local1);
			failed = 1;
			close_document(&d);
		} else {
			/* Handles the interactive condition. */
			if (interactive(&d, &terminal, style, numbers, first))
				failed = 1;
			close_document(&d);
		}
		index++;

		/* Handles the stopped condition. */
		if (stopped)
			break;
	} while (index < argc);
	terminal_close(&terminal);

	/* Returns the computed result. */
	return stopped ? 128 + stopped : failed;
}

/* Supports the terminal open operation. */
static int
terminal_open(
	struct terminal *t,
	int data_is_stdin)
{
	char path[128];
	struct winsize ws;
	struct termios raw;

	memset(t, 0, sizeof(*t));
	t->fd = -1;

	/* Handles a failed isatty operation. */
	if (!isatty(STDOUT_FILENO))
		return 0;

	/* Handles a failed isatty operation. */
	if (!data_is_stdin && isatty(STDIN_FILENO))
		t->fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
	else if (ttyname_r(STDOUT_FILENO, path, sizeof(path)) == 0)
		t->fd = open(path, O_RDWR | O_CLOEXEC);

	/* Handles a failed tcgetattr operation. */
	if (t->fd < 0 || tcgetattr(t->fd, &t->saved))
		return 0;
	raw = t->saved;
	raw.c_lflag &= ~(ECHO | ICANON);
	raw.c_iflag &= ~(ICRNL | INLCR);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;

	/* Handles a failed tcsetattr operation. */
	if (tcsetattr(t->fd, TCSANOW, &raw))
		return 0;
	t->rows = 24;
	t->cols = 80;

	/* Handles a failed ioctl operation. */
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
		/* Handles the ws condition. */
		if (ws.ws_row)
			t->rows = ws.ws_row;

		/* Handles the ws condition. */
		if (ws.ws_col)
			t->cols = ws.ws_col;
	}

	/* Handles the t condition. */
	if (t->rows < 3)
		t->rows = 3;

	/* Handles the t condition. */
	if (t->cols < 8)
		t->cols = 8;
	t->active = 1;
	out("\033[?25l", 6);

	/* Reports operation failure. */
	return 1;
}

/* Supports the out operation. */
static int
out(
	const void *b,
	size_t n)
{
	int function_result;

	/* Obtains the command write all result. */
	function_result = command_write_all(STDOUT_FILENO, b, n);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the copy fd operation. */
static int
copy_fd(
	int fd)
{
	unsigned char b[4096];
	ssize_t n;

	/* Process input until it is exhausted. */
	while ((n = read(fd, b, sizeof(b))) != 0) {
		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Handles a failed out operation. */
		if (n < 0 || out(b, (size_t)n))
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the load document operation. */
static int
load_document(
	struct document *d,
	const char *name)
{
	unsigned char extra;
	unsigned char block[4096];
	ssize_t n;
	off_t at;

	at = 0;
	memset(d, 0, sizeof(*d));
	d->fd = -1;
	d->name = name;

	/* Selects the matching value. */
	if (!strcmp(name, "-")) {
		d->fd = STDIN_FILENO;
		d->seekable = 0;
	} else {
		d->fd = open(name, O_RDONLY);

		/* Checks the current descriptor. */
		if (d->fd < 0)
			return -1;
		d->owned = 1;
		d->seekable = lseek(d->fd, 0, SEEK_CUR) >= 0;
	}

	/* Handles the add line condition. */
	if (add_line(d, 0))
		return -1;

	/* Checks the current descriptor. */
	if (d->seekable) {
		/* Handles a failed lseek operation. */
		if (lseek(d->fd, 0, SEEK_SET) < 0)
			return -1;

		/* Process input until it is exhausted. */
		while ((n = read(d->fd, block, sizeof(block))) > 0) {
			/* Handles the index bytes condition. */
			if (index_bytes(d, block, (size_t)n, at))
				return -1;
			at += (off_t)n;
		}

		/* Checks the current item count. */
		if (n < 0)
			return -1;
		d->size = at;

		/* Checks the current descriptor. */
		if (d->lines && d->line[d->lines - 1] == d->size)
			d->lines--;

		/* Checks the current descriptor. */
		if (d->size == 0)
			d->lines = 0;

		/* Reports successful completion. */
		return 0;
	}
	d->memory = malloc(PIPE_LIMIT);

	/* Checks the current descriptor. */
	if (!d->memory)
		return -1;

	/* Process each remaining element. */
	while (d->memory_size < PIPE_LIMIT) {
		n = read(d->fd, d->memory + d->memory_size,
			 PIPE_LIMIT - d->memory_size);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n < 0)
			return -1;

		/* Checks the current item count. */
		if (n == 0)
			break;

		/* Handles a failed index bytes operation. */
		if (index_bytes(d, d->memory + d->memory_size, (size_t)n,
				(off_t)d->memory_size))

			/* Reports operation failure. */
			return -1;
		d->memory_size += (size_t)n;
	}

	/* Checks the current descriptor. */
	if (d->memory_size == PIPE_LIMIT) {
		n = read(d->fd, &extra, 1);

		/* Checks the current item count. */
		if (n > 0) {
			errno = EFBIG;

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the current item count. */
		if (n < 0)
			return -1;
	}
	d->size = (off_t)d->memory_size;

	/* Checks the current descriptor. */
	if (d->lines && d->line[d->lines - 1] == d->size)
		d->lines--;

	/* Checks the current descriptor. */
	if (d->size == 0)
		d->lines = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the add line operation. */
static int
add_line(
	struct document *d,
	off_t value)
{
	off_t *p;
	size_t n;

	/* Checks the current descriptor. */
	if (d->lines == d->cap) {
		n = d->cap ? d->cap * 2U : 128U;

		/* Checks the current item count. */
		if (n < d->cap || n > SIZE_MAX / sizeof(*p)) {
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		p = realloc(d->line, n * sizeof(*p));

		/* Checks the current pointer. */
		if (!p)
			return -1;
		d->line = p;
		d->cap = n;
	}
	d->line[d->lines++] = value;

	/* Reports successful completion. */
	return 0;
}

/* Supports the index bytes operation. */
static int
index_bytes(
	struct document *d,
	const unsigned char *b,
	size_t n,
	off_t base)
{
	size_t i;

	/* Process each element required by the operation. */
	for (i = 0; i < n; i++) {
		/* Handles a failed add line operation. */
		if (b[i] == '\n' && add_line(d, base + (off_t)i + 1))
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the close document operation. */
static void
close_document(
	struct document *d)
{
	/* Checks the current descriptor. */
	if (d->owned && d->fd >= 0)
		close(d->fd);
	free(d->line);
	free(d->memory);
	memset(d, 0, sizeof(*d));
	d->fd = -1;
}

/* Supports the interactive operation. */
static int
interactive(
	struct document *d,
	struct terminal *t,
	enum pager_style style,
	int numbers,
	size_t first)
{
	size_t move_local;
	size_t move_local1;
	int r;
	size_t top, found;
	unsigned horizontal, body;
	int key, status;
	char prompt[128];

	/* Continue until the operation reaches a terminal state. */
	top = first < d->lines ? first : 0;
	horizontal = 0;
	body = t->rows - 1U;
	status = 0;
	for (;;) {
		/* Handles the style condition. */
		if (style == PAGER_MORE) {
			snprintf(prompt, sizeof(prompt), "--More--(%llu/%llu)",
				 (unsigned long long)(top + body < d->lines
							  ? top + body
							  : d->lines),
				 (unsigned long long)d->lines);
		} else {
			snprintf(prompt, sizeof(prompt), "%s  %llu/%llu",
				 d->name, (unsigned long long)(top + 1U),
				 (unsigned long long)d->lines);
		}

		/* Handles the render condition. */
		if (render(d, top, horizontal, t, numbers, prompt)) {
			status = -1;
			break;
		}

		/* Handles the style condition. */
		if (style == PAGER_MORE && top + body >= d->lines)
			break;
		key = key_read(t->fd);

		/* Handles the stopped condition. */
		if (stopped)
			break;

		/* Handles the selected key. */
		if (key == 'q')
			break;

		/* Handles the selected key. */
		if (key == ' ' || key == 'j') {
			move_local = key == ' ' ? body : 1U;

			/* Handles the top condition. */
			if (top + move_local < d->lines)
				top += move_local;
		} else if (key == '\r' || key == '\n') {
			/* Handles the top condition. */
			if (top + 1U < d->lines)
				top++;
		} else if (key == 'b' || key == 'k') {
			move_local1 = key == 'b' ? body : 1U;
			top = top > move_local1 ? top - move_local1 : 0;
		} else if (key == 'g')
			top = 0;
		else if (key == 'G')
			top = d->lines > body ? d->lines - body : 0;
		else if (key == 'h') {
			horizontal = horizontal >= 8U ? horizontal - 8U : 0;
		} else if (key == 'l')
			horizontal += 8U;
		else if (key == '/') {
			r = search_prompt(t, d, top, &found);

			/* Handles the r condition. */
			if (r < 0) {
				status = -1;
				break;
			}

			/* Handles the r condition. */
			if (r > 0)
				top = found;
			else
				out("\a", 1);
		}
	}

	/* Returns the computed result. */
	return status;
}

/* Supports the render operation. */
static int
render(
	const struct document *d,
	size_t top,
	unsigned horizontal,
	const struct terminal *t,
	int numbers,
	const char *prompt)
{
	unsigned row, body;

	body = t->rows - 1U;

	/* Handles the out condition. */
	if (out("\033[H\033[J", 6))
		return -1;

	/* Process each element required by the operation. */
	for (row = 0; row < body; row++) {
		/* Handles the top condition. */
		if (top + row < d->lines) {
			/* Handles a failed put line operation. */
			if (put_line(d, top + row, t->cols, horizontal,
				     numbers))

				/* Reports operation failure. */
				return -1;
		} else if (out("~\033[K\n", 5))

			/* Reports operation failure. */
			return -1;
	}

	/* Handles the out condition. */
	if (out("\033[K", 3) || out(prompt, strlen(prompt)))
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the put line operation. */
static int
put_line(
	const struct document *d,
	size_t line,
	unsigned cols,
	unsigned horizontal,
	int numbers)
{
	int function_result;
	int n_local;
	size_t n_local1;
	char prefix[24];
	int w;
	mbstate_t state;
	wchar_t wc;
	unsigned char c;
	unsigned width, used;
	unsigned char bytes[LINE_READ_MAX];
	ssize_t got;
	size_t at;
	unsigned column, shown;
	char mark[2];

	at = 0;
	column = 0;
	shown = 0;

	/* Handles the numbers condition. */
	if (numbers) {
		n_local = snprintf(prefix, sizeof(prefix), "%6llu  ",
		 (unsigned long long)(line + 1U));

		/* Handles the out condition. */
		if (out(prefix, (size_t)n_local))
			return -1;

		/* Handles the cols condition. */
		if (cols > (unsigned)n_local)
			cols -= (unsigned)n_local;
		else
			cols = 1;
	}
	got = line_bytes(d, line, bytes, sizeof(bytes));

	/* Handles the got condition. */
	if (got < 0)
		return -1;

	/* Process each remaining element. */
	while (at < (size_t)got && shown < cols) {
		c = bytes[at];
		width = 1;
		used = 1;

		/* Classifies the current input character. */
		if (c == '\t') {
			width = 8U - (column % 8U);

			/* Handles the column condition. */
			if (column + width > horizontal && column >= horizontal) {
				/* Continue while the operation condition remains true. */
				while (width-- && shown < cols) {
					/* Handles the out condition. */
					if (out(" ", 1))
						return -1;
					shown++;
				}
			}
			column += 8U - (column % 8U);
			at++;
			continue;
		}

		/* Classifies the current input character. */
		if (c >= 0x80U) {
			memset(&state, 0, sizeof(state));
			n_local1 = mbrtowc(&wc, (const char *)bytes + at,
				    (size_t)got - at, &state);

			/* Handles the n local1 condition. */
			if (n_local1 != (size_t)-1 && n_local1 != (size_t)-2 && n_local1 > 0) {
				w = wcwidth(wc);
				used = n_local1;
				width = w > 0 ? (unsigned)w : 1U;
			}
		}

		/* Handles the column condition. */
		if (column + width <= horizontal) {
			column += width;
			at += used;
			continue;
		}

		/* Classifies the current input character. */
		if (c < 32U || c == 127U) {
			mark[0] = '^';
			mark[1] = (char)(c == 127U ? '?' : c + '@');

			/* Handles the shown condition. */
			if (shown + 2U > cols)
				break;

			/* Handles the out condition. */
			if (out(mark, 2))
				return -1;
			shown += 2U;
		} else {
			/* Handles the shown condition. */
			if (shown + width > cols)
				break;

			/* Handles the out condition. */
			if (out(bytes + at, used))
				return -1;
			shown += width;
		}
		column += width;
		at += used;
	}

	/* Obtains the out result. */
	function_result = out("\033[K\n", 4);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the line bytes operation. */
static ssize_t
line_bytes(
	const struct document *d,
	size_t line,
	unsigned char *b,
	size_t cap)
{
	ssize_t n;
	off_t start, end;
	size_t amount;

	/* Handles the line condition. */
	if (line >= d->lines)
		return 0;
	start = d->line[line];
	end = line + 1U < d->lines ? d->line[line + 1U] : d->size;

	/* Checks the current endpoint. */
	if (end > start && end - start > 0) {
		amount = (size_t)(end - start);

		/* Handles the amount condition. */
		if (amount > cap)
			amount = cap;

		/* Checks the current descriptor. */
		if (d->seekable) {
			n = pread(d->fd, b, amount, start);

			/* Checks the current item count. */
			if (n < 0)
				return -1;
			amount = (size_t)n;
		} else {
			memcpy(b, d->memory + (size_t)start, amount);
		}

		/* Continue while the operation condition remains true. */
		while (amount &&
		       (b[amount - 1] == '\n' || b[amount - 1] == '\r'))
			amount--;

		/* Returns the computed result. */
		return (ssize_t)amount;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the key read operation. */
static int
key_read(
	int fd)
{
	unsigned char c;
	ssize_t n;

	do

	/* Continue while the operation condition remains true. */
		n = read(fd, &c, 1);
	while (n < 0 && errno == EINTR && !stopped);

	/* Checks the current item count. */
	if (n <= 0)
		return 'q';

	/* Classifies the current input character. */
	if (c != 27)
		return c;
	do

	/* Continue while the operation condition remains true. */
		n = read(fd, &c, 1);
	while (n < 0 && errno == EINTR && !stopped);

	/* Checks the current item count. */
	if (n <= 0 || c != '[')
		return 27;
	do

	/* Continue while the operation condition remains true. */
		n = read(fd, &c, 1);
	while (n < 0 && errno == EINTR && !stopped);

	/* Checks the current item count. */
	if (n <= 0)
		return 27;

	/* Classifies the current input character. */
	if (c == 'A')
		return 'k';

	/* Classifies the current input character. */
	if (c == 'B')
		return 'j';

	/* Classifies the current input character. */
	if (c == 'C')
		return 'l';

	/* Classifies the current input character. */
	if (c == 'D')
		return 'h';

	/* Classifies the current input character. */
	if (c == 'H')
		return 'g';

	/* Classifies the current input character. */
	if (c == 'F')
		return 'G';

	/* Returns the computed result. */
	return c;
}

/* Supports the search prompt operation. */
static int
search_prompt(
	struct terminal *t,
	const struct document *d,
	size_t top,
	size_t *found)
{
	int function_result;
	static const char lead[] = "\033[999;1H\033[K/";
	char pattern[257];
	size_t used;
	int key;

	used = 0;

	/* Handles the out condition. */
	if (out(lead, sizeof(lead) - 1U))
		return -1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		key = key_read(t->fd);

		/* Handles the selected key. */
		if (key == '\r' || key == '\n')
			break;

		/* Handles the selected key. */
		if (key == 27) {
			return 0;
		}

		/* Handles the selected key. */
		if ((key == 8 || key == 127) && used) {
			used--;
			out("\b \b", 3);
			continue;
		}

		/* Handles the selected key. */
		if (key >= 32 && key < 127 && used + 1U < sizeof(pattern)) {
			pattern[used++] = (char)key;
			out(&pattern[used - 1], 1);
		}
	}
	pattern[used] = 0;

	/* Checks the current capacity usage. */
	if (!used)
		return 0;

	/* Obtains the find forward result. */
	function_result = find_forward(d, top + 1U, pattern, found);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the find forward operation. */
static int
find_forward(
	const struct document *d,
	size_t start,
	const char *pattern,
	size_t *found)
{
	ssize_t n;
	unsigned char bytes[LINE_READ_MAX];
	size_t i;

	/* Process each element required by the operation. */
	for (i = start; i < d->lines; i++) {
		n = line_bytes(d, i, bytes, sizeof(bytes) - 1U);

		/* Checks the current item count. */
		if (n < 0)
			return -1;
		bytes[n] = 0;

		/* Handles the strstr condition. */
		if (strstr((char *)bytes, pattern)) {
			*found = i;
			/* Reports operation failure. */
			return 1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the terminal close operation. */
static void
terminal_close(
	struct terminal *t)
{
	static const char restore[] = "\033[?25h\033[K\n";

	/* Handles the t condition. */
	if (t->active) {
		out(restore, sizeof(restore) - 1U);
		tcsetattr(t->fd, TCSANOW, &t->saved);
		t->active = 0;
	}

	/* Handles the t condition. */
	if (t->fd >= 0)
		close(t->fd);
	t->fd = -1;
}

/* Supports the stop handler operation. */
static void
stop_handler(
	int signo)
{
	stopped = signo;
}
