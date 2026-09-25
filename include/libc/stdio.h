/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H
/* The guard name other software probes to learn that <stdio.h> is in. */
#define _STDIO_H 1

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stddef.h>
#include <uapi/rename.h>
#include <stdint.h>
#include <sys/types.h>
#include <uapi/unistd.h>

#define EOF (-1)
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define BUFSIZ 4096
#define FOPEN_MAX 16
#define FILENAME_MAX 1024
#define L_tmpnam 32
#define TMP_MAX 10000

typedef struct __stdio_file FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

size_t __stdio_console_write(const char *bytes, size_t length);

int printf(const char *format, ...);
int vprintf(const char *, va_list);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments);
int sscanf(const char *string, const char *format, ...);
int vsscanf(const char *, const char *, va_list);
int scanf(const char *, ...);
int vscanf(const char *, va_list);
int putchar(int character);
int puts(const char *string);
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int, const char *);
FILE *popen(const char *, const char *);
int pclose(FILE *);
FILE *freopen(const char *, const char *, FILE *);
FILE *tmpfile(void);

/*
 * Reads a line, or a record ending in any chosen byte, growing the caller's
 * buffer to hold it.  A null buffer asks for one to be allocated.
 */
ssize_t getdelim(char **, size_t *, int, FILE *);
ssize_t getline(char **, size_t *, FILE *);

/*
 * Reads a line, or a record ending in any chosen byte, growing the caller's
 * buffer to hold it.  A null buffer asks for one to be allocated.
 */
ssize_t getdelim(char **, size_t *, int, FILE *);
ssize_t getline(char **, size_t *, FILE *);
char *tmpnam(char *);
char *tempnam(const char *, const char *);
int fclose(FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);
int fgetpos(FILE *, fpos_t *);
int fsetpos(FILE *, const fpos_t *);
void rewind(FILE *);
char *fgets(char *buffer, int size, FILE *stream);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *, const char *, va_list);
int sprintf(char *, const char *, ...);
int vsprintf(char *, const char *, va_list);
int fscanf(FILE *, const char *, ...);
int vfscanf(FILE *, const char *, va_list);
int fputs(const char *, FILE *);
int getchar(void);
int putc(int, FILE *);
void perror(const char *);
int fputc(int character, FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int ungetc(int character, FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int ferror(FILE *stream);
int feof(FILE *stream);
void clearerr(FILE *stream);
void flockfile(FILE *stream);
int ftrylockfile(FILE *stream);
void funlockfile(FILE *stream);
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
void setbuf(FILE *stream, char *buffer);
int rename(const char *, const char *);
int renameat(int, const char *, int, const char *);
int renameat2(int, const char *, int, const char *, unsigned);
int remove(const char *);
int asprintf(char **, const char *, ...);
int vasprintf(char **, const char *, va_list);
char *fgetln(FILE *, size_t *);
const char *fmtcheck(const char *, const char *);
int fpurge(FILE *);
FILE *funopen(const void *, int (*)(void *, char *, int), int (*)(void *, const char *, int), fpos_t (*)(void *, fpos_t, int), int (*)(void *));
void setbuffer(FILE *, char *, int);
int setlinebuf(FILE *);

/*
 * The descriptor a stream was opened on.  POSIX; the implementation takes
 * void * rather than FILE * so that this header need not be included where
 * the stream type is not.
 */
int fileno(void *);

/* The locale-aware forms POSIX.1-2008 added. */
int snprintf_l(char *, size_t, locale_t, const char *, ...);
int asprintf_l(char **, locale_t, const char *, ...);

#ifdef __cplusplus
}
#endif

#endif
