/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
static void
stop_handler(int signo)
{
	stopped = signo;
}
static int
add_line(struct document *d, off_t value)
{
	off_t *p;
	size_t n;
	if (d->lines == d->cap) {
		n = d->cap ? d->cap * 2U : 128U;
		if (n < d->cap || n > SIZE_MAX / sizeof(*p)) {
			errno = EOVERFLOW;
			return -1;
		}
		p = realloc(d->line, n * sizeof(*p));
		if (!p)
			return -1;
		d->line = p;
		d->cap = n;
	}
	d->line[d->lines++] = value;
	return 0;
}
static int
index_bytes(struct document *d, const unsigned char *b, size_t n, off_t base)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (b[i] == '\n' && add_line(d, base + (off_t)i + 1))
			return -1;
	return 0;
}
static int
load_document(struct document *d, const char *name)
{
	unsigned char block[4096];
	ssize_t n;
	off_t at = 0;
	memset(d, 0, sizeof(*d));
	d->fd = -1;
	d->name = name;
	if (!strcmp(name, "-")) {
		d->fd = STDIN_FILENO;
		d->seekable = 0;
	} else {
		d->fd = open(name, O_RDONLY);
		if (d->fd < 0)
			return -1;
		d->owned = 1;
		d->seekable = lseek(d->fd, 0, SEEK_CUR) >= 0;
	}
	if (add_line(d, 0))
		return -1;
	if (d->seekable) {
		if (lseek(d->fd, 0, SEEK_SET) < 0)
			return -1;
		while ((n = read(d->fd, block, sizeof(block))) > 0) {
			if (index_bytes(d, block, (size_t)n, at))
				return -1;
			at += (off_t)n;
		}
		if (n < 0)
			return -1;
		d->size = at;
		if (d->lines && d->line[d->lines - 1] == d->size)
			d->lines--;
		if (d->size == 0)
			d->lines = 0;
		return 0;
	}
	d->memory = malloc(PIPE_LIMIT);
	if (!d->memory)
		return -1;
	while (d->memory_size < PIPE_LIMIT) {
		n = read(d->fd, d->memory + d->memory_size,
			 PIPE_LIMIT - d->memory_size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (n == 0)
			break;
		if (index_bytes(d, d->memory + d->memory_size, (size_t)n,
				(off_t)d->memory_size))
			return -1;
		d->memory_size += (size_t)n;
	}
	if (d->memory_size == PIPE_LIMIT) {
		unsigned char extra;
		n = read(d->fd, &extra, 1);
		if (n > 0) {
			errno = EFBIG;
			return -1;
		}
		if (n < 0)
			return -1;
	}
	d->size = (off_t)d->memory_size;
	if (d->lines && d->line[d->lines - 1] == d->size)
		d->lines--;
	if (d->size == 0)
		d->lines = 0;
	return 0;
}
static void
close_document(struct document *d)
{
	if (d->owned && d->fd >= 0)
		close(d->fd);
	free(d->line);
	free(d->memory);
	memset(d, 0, sizeof(*d));
	d->fd = -1;
}
static ssize_t
line_bytes(const struct document *d, size_t line, unsigned char *b, size_t cap)
{
	off_t start, end;
	size_t amount;
	if (line >= d->lines)
		return 0;
	start = d->line[line];
	end = line + 1U < d->lines ? d->line[line + 1U] : d->size;
	if (end > start && end - start > 0) {
		amount = (size_t)(end - start);
		if (amount > cap)
			amount = cap;
		if (d->seekable) {
			ssize_t n = pread(d->fd, b, amount, start);
			if (n < 0)
				return -1;
			amount = (size_t)n;
		} else
			memcpy(b, d->memory + (size_t)start, amount);
		while (amount &&
		       (b[amount - 1] == '\n' || b[amount - 1] == '\r'))
			amount--;
		return (ssize_t)amount;
	}
	return 0;
}
static int
out(const void *b, size_t n)
{
	return command_write_all(STDOUT_FILENO, b, n);
}
static int
put_line(const struct document *d, size_t line, unsigned cols,
	 unsigned horizontal, int numbers)
{
	unsigned char bytes[LINE_READ_MAX];
	ssize_t got;
	size_t at = 0;
	unsigned column = 0, shown = 0;
	if (numbers) {
		char prefix[24];
		int n = snprintf(prefix, sizeof(prefix), "%6llu  ",
				 (unsigned long long)(line + 1U));
		if (out(prefix, (size_t)n))
			return -1;
		if (cols > (unsigned)n)
			cols -= (unsigned)n;
		else
			cols = 1;
	}
	got = line_bytes(d, line, bytes, sizeof(bytes));
	if (got < 0)
		return -1;
	while (at < (size_t)got && shown < cols) {
		unsigned char c = bytes[at];
		unsigned width = 1, used = 1;
		if (c == '\t') {
			width = 8U - (column % 8U);
			if (column + width > horizontal && column >= horizontal)
				while (width-- && shown < cols) {
					if (out(" ", 1))
						return -1;
					shown++;
				}
			column += 8U - (column % 8U);
			at++;
			continue;
		}
		if (c >= 0x80U) {
			mbstate_t state;
			wchar_t wc;
			size_t n;
			memset(&state, 0, sizeof(state));
			n = mbrtowc(&wc, (const char *)bytes + at,
				    (size_t)got - at, &state);
			if (n != (size_t)-1 && n != (size_t)-2 && n > 0) {
				int w = wcwidth(wc);
				used = n;
				width = w > 0 ? (unsigned)w : 1U;
			}
		}
		if (column + width <= horizontal) {
			column += width;
			at += used;
			continue;
		}
		if (c < 32U || c == 127U) {
			char mark[2] = {'^', (char)(c == 127U ? '?' : c + '@')};
			if (shown + 2U > cols)
				break;
			if (out(mark, 2))
				return -1;
			shown += 2U;
		} else {
			if (shown + width > cols)
				break;
			if (out(bytes + at, used))
				return -1;
			shown += width;
		}
		column += width;
		at += used;
	}
	return out("\033[K\n", 4);
}
static int
terminal_open(struct terminal *t, int data_is_stdin)
{
	char path[128];
	struct winsize ws;
	struct termios raw;
	memset(t, 0, sizeof(*t));
	t->fd = -1;
	if (!isatty(STDOUT_FILENO))
		return 0;
	if (!data_is_stdin && isatty(STDIN_FILENO))
		t->fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
	else if (ttyname_r(STDOUT_FILENO, path, sizeof(path)) == 0)
		t->fd = open(path, O_RDWR | O_CLOEXEC);
	if (t->fd < 0 || tcgetattr(t->fd, &t->saved))
		return 0;
	raw = t->saved;
	raw.c_lflag &= ~(ECHO | ICANON);
	raw.c_iflag &= ~(ICRNL | INLCR);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(t->fd, TCSANOW, &raw))
		return 0;
	t->rows = 24;
	t->cols = 80;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
		if (ws.ws_row)
			t->rows = ws.ws_row;
		if (ws.ws_col)
			t->cols = ws.ws_col;
	}
	if (t->rows < 3)
		t->rows = 3;
	if (t->cols < 8)
		t->cols = 8;
	t->active = 1;
	out("\033[?25l", 6);
	return 1;
}
static void
terminal_close(struct terminal *t)
{
	if (t->active) {
		static const char restore[] = "\033[?25h\033[K\n";
		out(restore, sizeof(restore) - 1U);
		tcsetattr(t->fd, TCSANOW, &t->saved);
		t->active = 0;
	}
	if (t->fd >= 0)
		close(t->fd);
	t->fd = -1;
}
static int
key_read(int fd)
{
	unsigned char c;
	ssize_t n;
	do
		n = read(fd, &c, 1);
	while (n < 0 && errno == EINTR && !stopped);
	if (n <= 0)
		return 'q';
	if (c != 27)
		return c;
	do
		n = read(fd, &c, 1);
	while (n < 0 && errno == EINTR && !stopped);
	if (n <= 0 || c != '[')
		return 27;
	do
		n = read(fd, &c, 1);
	while (n < 0 && errno == EINTR && !stopped);
	if (n <= 0)
		return 27;
	if (c == 'A')
		return 'k';
	if (c == 'B')
		return 'j';
	if (c == 'C')
		return 'l';
	if (c == 'D')
		return 'h';
	if (c == 'H')
		return 'g';
	if (c == 'F')
		return 'G';
	return c;
}
static int
find_forward(const struct document *d, size_t start, const char *pattern,
	     size_t *found)
{
	unsigned char bytes[LINE_READ_MAX];
	size_t i;
	for (i = start; i < d->lines; i++) {
		ssize_t n = line_bytes(d, i, bytes, sizeof(bytes) - 1U);
		if (n < 0)
			return -1;
		bytes[n] = 0;
		if (strstr((char *)bytes, pattern)) {
			*found = i;
			return 1;
		}
	}
	return 0;
}
static int
search_prompt(struct terminal *t, const struct document *d, size_t top,
	      size_t *found)
{
	static const char lead[] = "\033[999;1H\033[K/";
	char pattern[257];
	size_t used = 0;
	int key;
	if (out(lead, sizeof(lead) - 1U))
		return -1;
	for (;;) {
		key = key_read(t->fd);
		if (key == '\r' || key == '\n')
			break;
		if (key == 27) {
			return 0;
		}
		if ((key == 8 || key == 127) && used) {
			used--;
			out("\b \b", 3);
			continue;
		}
		if (key >= 32 && key < 127 && used + 1U < sizeof(pattern)) {
			pattern[used++] = (char)key;
			out(&pattern[used - 1], 1);
		}
	}
	pattern[used] = 0;
	if (!used)
		return 0;
	return find_forward(d, top + 1U, pattern, found);
}
static int
render(const struct document *d, size_t top, unsigned horizontal,
       const struct terminal *t, int numbers, const char *prompt)
{
	unsigned row, body = t->rows - 1U;
	if (out("\033[H\033[J", 6))
		return -1;
	for (row = 0; row < body; row++) {
		if (top + row < d->lines) {
			if (put_line(d, top + row, t->cols, horizontal,
				     numbers))
				return -1;
		} else if (out("~\033[K\n", 5))
			return -1;
	}
	if (out("\033[K", 3) || out(prompt, strlen(prompt)))
		return -1;
	return 0;
}
static int
copy_fd(int fd)
{
	unsigned char b[4096];
	ssize_t n;
	while ((n = read(fd, b, sizeof(b))) != 0) {
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 || out(b, (size_t)n))
			return -1;
	}
	return 0;
}
static int
interactive(struct document *d, struct terminal *t, enum pager_style style,
	    int numbers, size_t first)
{
	size_t top = first < d->lines ? first : 0, found;
	unsigned horizontal = 0, body = t->rows - 1U;
	int key, status = 0;
	char prompt[128];
	for (;;) {
		if (style == PAGER_MORE)
			snprintf(prompt, sizeof(prompt), "--More--(%llu/%llu)",
				 (unsigned long long)(top + body < d->lines
							  ? top + body
							  : d->lines),
				 (unsigned long long)d->lines);
		else
			snprintf(prompt, sizeof(prompt), "%s  %llu/%llu",
				 d->name, (unsigned long long)(top + 1U),
				 (unsigned long long)d->lines);
		if (render(d, top, horizontal, t, numbers, prompt)) {
			status = -1;
			break;
		}
		if (style == PAGER_MORE && top + body >= d->lines)
			break;
		key = key_read(t->fd);
		if (stopped)
			break;
		if (key == 'q')
			break;
		if (key == ' ' || key == 'j') {
			size_t move = key == ' ' ? body : 1U;
			if (top + move < d->lines)
				top += move;
		} else if (key == '\r' || key == '\n') {
			if (top + 1U < d->lines)
				top++;
		} else if (key == 'b' || key == 'k') {
			size_t move = key == 'b' ? body : 1U;
			top = top > move ? top - move : 0;
		} else if (key == 'g')
			top = 0;
		else if (key == 'G')
			top = d->lines > body ? d->lines - body : 0;
		else if (key == 'h') {
			horizontal = horizontal >= 8U ? horizontal - 8U : 0;
		} else if (key == 'l')
			horizontal += 8U;
		else if (key == '/') {
			int r = search_prompt(t, d, top, &found);
			if (r < 0) {
				status = -1;
				break;
			}
			if (r > 0)
				top = found;
			else
				out("\a", 1);
		}
	}
	return status;
}
int
pager_main(enum pager_style style, int argc, char **argv)
{
	int index = 1, numbers = 0, failed = 0, data_stdin = 0;
	size_t first = 0;
	struct terminal terminal;
	struct sigaction action;
	while (index < argc && argv[index][0] == '-' && argv[index][1]) {
		if (style == PAGER_LESS && !strcmp(argv[index], "-N"))
			numbers = 1;
		else
			break;
		index++;
	}
	if (index < argc && argv[index][0] == '+' && argv[index][1]) {
		first = (size_t)strtoul(argv[index] + 1, NULL, 10);
		if (first)
			first--;
		index++;
	}
	if (index == argc)
		data_stdin = 1;
	else {
		int i;
		for (i = index; i < argc; i++)
			if (!strcmp(argv[i], "-"))
				data_stdin = 1;
	}
	if (index == argc && isatty(STDIN_FILENO)) {
		fprintf(stderr, "%s: missing file operand\n",
			style == PAGER_MORE ? "more" : "less");
		return 1;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = (uint64_t)(uintptr_t)stop_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (!terminal_open(&terminal, data_stdin)) {
		do {
			const char *name = index == argc ? "-" : argv[index];
			int fd = !strcmp(name, "-") ? STDIN_FILENO
						    : open(name, O_RDONLY);
			if (fd < 0 || copy_fd(fd)) {
				command_error(style == PAGER_MORE ? "more"
								  : "less",
					      name);
				failed = 1;
			}
			if (fd >= 0 && fd != STDIN_FILENO)
				close(fd);
			index++;
		} while (index < argc);
		return failed;
	}
	do {
		const char *name = index == argc ? "-" : argv[index];
		struct document d;
		if (load_document(&d, name)) {
			command_error(style == PAGER_MORE ? "more" : "less",
				      name);
			failed = 1;
			close_document(&d);
		} else {
			if (interactive(&d, &terminal, style, numbers, first))
				failed = 1;
			close_document(&d);
		}
		index++;
		if (stopped)
			break;
	} while (index < argc);
	terminal_close(&terminal);
	return stopped ? 128 + stopped : failed;
}
