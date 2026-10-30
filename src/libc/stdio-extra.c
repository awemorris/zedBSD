/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Remaining ISO C stdio interfaces and common BSD conveniences.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

int vfprintf(FILE *stream, const char *format, va_list ap)
{
	va_list copy; va_copy(copy, ap); int length = vsnprintf(NULL, 0, format, copy); va_end(copy);
	if (length < 0) return EOF;
	char local[256], *buffer = local;
	if ((size_t)length >= sizeof(local) && (buffer = malloc((size_t)length + 1)) == NULL) return EOF;
	(void)vsnprintf(buffer, (size_t)length + 1, format, ap);
	size_t written = fwrite(buffer, 1, (size_t)length, stream);
	if (buffer != local) free(buffer);
	return written == (size_t)length ? length : EOF;
}
int vprintf(const char *format, va_list ap) { return vfprintf(stdout, format, ap); }
int sprintf(char *buffer, const char *format, ...)
{ va_list ap; va_start(ap, format); int n = vsprintf(buffer, format, ap); va_end(ap); return n; }
int vsprintf(char *buffer, const char *format, va_list ap)
{ return vsnprintf(buffer, (size_t)-1, format, ap); }
int fputs(const char *s, FILE *stream)
{ size_t n = strlen(s); return fwrite(s, 1, n, stream) == n ? 0 : EOF; }
int getchar(void) { return fgetc(stdin); }
int putc(int c, FILE *stream) { return fputc(c, stream); }
void perror(const char *prefix)
{ int saved = errno; if (prefix && *prefix) fprintf(stderr, "%s: %s\n", prefix, strerror(saved)); else fprintf(stderr, "%s\n", strerror(saved)); }
int fgetpos(FILE *stream, fpos_t *position)
{ long at = ftell(stream); if (at < 0) return -1; *position = at; return 0; }
int fsetpos(FILE *stream, const fpos_t *position) { return fseek(stream, *position, SEEK_SET); }
void rewind(FILE *stream) { clearerr(stream); (void)fseek(stream, 0, SEEK_SET); }
int remove(const char *path) { if (unlink(path) == 0) return 0; return rmdir(path); }

int vasprintf(char **result, const char *format, va_list ap)
{
	va_list copy; va_copy(copy, ap); int n = vsnprintf(NULL, 0, format, copy); va_end(copy);
	if (result == NULL) { errno = EINVAL; return -1; }
	if (n < 0 || (*result = malloc((size_t)n + 1)) == NULL) { *result = NULL; return -1; }
	(void)vsnprintf(*result, (size_t)n + 1, format, ap); return n;
}
int asprintf(char **result, const char *format, ...)
{ va_list ap; va_start(ap, format); int n = vasprintf(result, format, ap); va_end(ap); return n; }
void setbuffer(FILE *stream, char *buffer, int size)
{ (void)setvbuf(stream, buffer, buffer ? _IOFBF : _IONBF, buffer ? (size_t)size : 0); }
int setlinebuf(FILE *stream) { return setvbuf(stream, NULL, _IOLBF, BUFSIZ); }
__attribute__((weak)) int fpurge(FILE *stream) { clearerr(stream); return 0; }
char *fgetln(FILE *stream, size_t *length)
{
	static char *buffer;
	static size_t capacity;
	size_t used = 0;
	int character;
	if (!buffer && (buffer = malloc(BUFSIZ)) != NULL)
		capacity = BUFSIZ;
	if (!buffer)
		return NULL;
	while ((character = fgetc(stream)) != EOF) {
		if (used == capacity) {
			size_t next = capacity <= SIZE_MAX / 2 ? capacity * 2 : 0;
			char *grown = next ? realloc(buffer, next) : NULL;
			if (!grown) { errno = ENOMEM; return NULL; }
			buffer = grown;
			capacity = next;
		}
		buffer[used++] = (char)character;
		if (character == '\n')
			break;
	}
	if (used == 0)
		return NULL;
	if (length)
		*length = used;
	return buffer;
}
static int formats_compatible(const char *a, const char *b)
{
	for (;;) { while (*a && *a != '%') a++; while (*b && *b != '%') b++;
		if (!*a || !*b)
			return *a == *b;
		a++;
		b++;
		if (*a == '%') { a++; if (*b++ != '%') return 0; continue; }
		while (*a && !strchr("diouxXfFeEgGaAcspn", *a))
			a++;
		while (*b && !strchr("diouxXfFeEgGaAcspn", *b))
			b++;
		if (*a++ != *b++) return 0; }
}
const char *fmtcheck(const char *user, const char *safe)
{ return user && safe && formats_compatible(user, safe) ? user : safe; }

__attribute__((weak)) FILE *freopen(const char *path, const char *mode, FILE *stream)
{ if (stream) fclose(stream); return fopen(path, mode); }
char *tmpnam(char *result)
{ static char storage[L_tmpnam]; static unsigned counter; char *out = result ? result : storage;
	(void)snprintf(out, L_tmpnam, "/tmp/z%u-%u", (unsigned)getpid(), counter++); return out; }
FILE *tmpfile(void)
{ char path[L_tmpnam]; FILE *stream; tmpnam(path); stream = fopen(path, "w+"); if (stream) (void)unlink(path); return stream; }

__attribute__((weak)) FILE *funopen(const void *cookie, int (*readfn)(void *, char *, int),
    int (*writefn)(void *, const char *, int), fpos_t (*seekfn)(void *, fpos_t, int), int (*closefn)(void *))
{ (void)cookie; (void)readfn; (void)writefn; (void)seekfn; (void)closefn; errno = ENOSYS; return NULL; }

/*
 * Writes formatted output to a file descriptor (POSIX vdprintf).
 *
 * The text is formatted into memory first and then written whole, going on
 * after a short write or an interrupted one.  Reports the bytes written, or
 * -1 with errno when the text cannot be formatted or written.
 */
int
vdprintf(
	int descriptor,
	const char *format,
	va_list arguments)
{
	char *text;
	size_t done;
	ssize_t wrote;
	int length;
	int failure;

	/* Formats the whole text. */
	length = vasprintf(&text, format, arguments);
	if (length < 0)
		return -1;

	/* Writes it all, going on after short and interrupted writes. */
	done = 0U;
	failure = 0;
	while (done < (size_t)length) {
		wrote = write(descriptor, text + done, (size_t)length - done);
		if (wrote < 0 && errno == EINTR)
			continue;

		/* Any other failure stops the output; errno says why. */
		if (wrote < 0) {
			failure = errno;
			break;
		}

		/* Counts what was written and goes on with the rest. */
		done += (size_t)wrote;
	}

	/* The text is no longer needed. */
	free(text);

	/* Reports a write that failed. */
	if (failure != 0) {
		errno = failure;
		return -1;
	}

	/* Succeeded: the bytes written. */
	return length;
}

/* Writes formatted output to a file descriptor (POSIX dprintf). */
int
dprintf(
	int descriptor,
	const char *format,
	...)
{
	va_list arguments;
	int written;

	/* Formats and writes through vdprintf. */
	va_start(arguments, format);
	written = vdprintf(descriptor, format, arguments);
	va_end(arguments);

	/* Reports a failure, errno set. */
	if (written < 0)
		return -1;

	/* Succeeded: the bytes written. */
	return written;
}
