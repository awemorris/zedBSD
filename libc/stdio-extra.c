/* Remaining ISO C stdio interfaces and common BSD conveniences. SPDX-License-Identifier: Zlib */
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

static int scan_integer(const char **input, int width, int base, int sign,
    void *target, int length)
{
	char local[128], *text = (char *)*input, *allocated = NULL, *end;
	if (width > 0) {
		size_t count = strnlen(*input, (size_t)width);
		text = count < sizeof(local) ? local : (allocated = malloc(count + 1));
		if (text == NULL)
			return 0;
		memcpy(text, *input, count);
		text[count] = 0;
	}
	if (sign) { long long value = strtoll(text, &end, base); if (end == text) { free(allocated); return 0; }
		if (target) { if (length == 2) *(long long *)target = value; else if (length == 1) *(long *)target = (long)value;
			else if (length == -1) *(short *)target = (short)value; else if (length == -2) *(signed char *)target = (signed char)value; else *(int *)target = (int)value; } }
	else { unsigned long long value = strtoull(text, &end, base); if (end == text) { free(allocated); return 0; }
		if (target) { if (length == 2) *(unsigned long long *)target = value; else if (length == 1) *(unsigned long *)target = (unsigned long)value;
			else if (length == -1) *(unsigned short *)target = (unsigned short)value; else if (length == -2) *(unsigned char *)target = (unsigned char)value; else *(unsigned int *)target = (unsigned int)value; } }
	*input += end - text;
	free(allocated);
	return 1;
}
int vsscanf(const char *input, const char *format, va_list ap)
{
	const char *start = input; int assigned = 0;
	while (*format) {
		if (isspace((unsigned char)*format)) { while (isspace((unsigned char)*format)) format++; while (isspace((unsigned char)*input)) input++; continue; }
		if (*format != '%') { if (*input != *format) break; input++; format++; continue; }
		format++; if (*format == '%') { if (*input++ != '%') break; format++; continue; }
		int suppress = *format == '*', width = 0, length = 0; if (suppress) format++;
		while (isdigit((unsigned char)*format)) width = width * 10 + *format++ - '0';
		if (*format == 'h') { length = -1; format++; if (*format == 'h') { length = -2; format++; } }
		else if (*format == 'l') { length = 1; format++; if (*format == 'l') { length = 2; format++; } }
		else if (*format == 'j' || *format == 'z' || *format == 't') { length = 2; format++; }
		else if (*format == 'L') { length = 3; format++; }
		char conversion = *format++; if (conversion != 'c' && conversion != '[' && conversion != 'n') while (isspace((unsigned char)*input)) input++;
		if (!*input && conversion != 'n') break;
		void *target = suppress ? NULL : va_arg(ap, void *); int matched = 0;
		if (conversion == 'd' || conversion == 'i') matched = scan_integer(&input, width, conversion == 'i' ? 0 : 10, 1, target, length);
		else if (conversion == 'u' || conversion == 'o' || conversion == 'x' || conversion == 'X') matched = scan_integer(&input, width, conversion == 'o' ? 8 : (conversion == 'u' ? 10 : 16), 0, target, length);
		else if (conversion == 'p') { unsigned long long value; char *end; value = strtoull(input, &end, 16); matched = end != input; input = end; if (target) *(void **)target = (void *)(uintptr_t)value; }
		else if (strchr("aAeEfFgG", conversion)) {
			char *end;
			if (length == 3) {
				long double value = strtold(input, &end);
				matched = end != input;
				if (target)
					*(long double *)target = value;
			} else {
				double value = strtod(input, &end);
				matched = end != input;
				if (target) {
					if (length == 1)
						*(double *)target = value;
					else
						*(float *)target = (float)value;
				}
			}
			input = end;
		}
		else if (conversion == 'c' && length == 1) { int n = width ? width : 1; wchar_t *out = target; mbstate_t state = {0}; matched = 1; while (n--) { wchar_t wc; size_t used = mbrtowc(&wc, input, strlen(input) + 1, &state); if (used == (size_t)-1 || used == (size_t)-2 || used == 0) { matched = 0; break; } if (out) *out++ = wc; input += used; } }
		else if (conversion == 'c') { int n = width ? width : 1; char *out = target; if ((int)strnlen(input, (size_t)n) == n) { matched = 1; while (n--) { if (out) *out++ = *input; input++; } } }
		else if (conversion == 's' && length == 1) { int n = width ? width : INT_MAX; wchar_t *out = target; mbstate_t state = {0}; while (n-- && *input && !isspace((unsigned char)*input)) { wchar_t wc; size_t used = mbrtowc(&wc, input, strlen(input) + 1, &state); if (used == (size_t)-1 || used == (size_t)-2 || used == 0) break; if (out) *out++ = wc; input += used; matched = 1; } if (out) *out = 0; }
		else if (conversion == 's') { int n = width ? width : INT_MAX; char *out = target; while (n-- && *input && !isspace((unsigned char)*input)) { if (out) *out++ = *input; input++; matched = 1; } if (out) *out = 0; }
		else if (conversion == '[') { int inverse = *format == '^'; unsigned char table[256] = {0}; char *out = target; int n = width ? width : INT_MAX; if (inverse) format++; if (*format == ']') table[(unsigned char)*format++] = 1; while (*format && *format != ']') table[(unsigned char)*format++] = 1; if (*format == ']') format++; while (n-- && *input && (table[(unsigned char)*input] != 0) != inverse) { if (out) *out++ = *input; input++; matched = 1; } if (out) *out = 0; }
		else if (conversion == 'n') { if (target) { ptrdiff_t n = input - start; if (length == 2) *(long long *)target = n; else if (length == 1) *(long *)target = n; else if (length == -1) *(short *)target = (short)n; else if (length == -2) *(signed char *)target = (signed char)n; else *(int *)target = (int)n; } matched = 1; }
		else break;
		if (!matched)
			break;
		if (!suppress && conversion != 'n')
			assigned++;
	}
	return assigned;
}
int sscanf(const char *s, const char *format, ...)
{ va_list ap; va_start(ap, format); int n = vsscanf(s, format, ap); va_end(ap); return n; }
int vfscanf(FILE *stream, const char *format, va_list ap)
{ char buffer[4096]; return fgets(buffer, sizeof(buffer), stream) ? vsscanf(buffer, format, ap) : EOF; }
int fscanf(FILE *stream, const char *format, ...)
{ va_list ap; va_start(ap, format); int n = vfscanf(stream, format, ap); va_end(ap); return n; }
int vscanf(const char *format, va_list ap) { return vfscanf(stdin, format, ap); }
int scanf(const char *format, ...)
{ va_list ap; va_start(ap, format); int n = vscanf(format, ap); va_end(ap); return n; }

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
