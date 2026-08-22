/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_STDIO_H
#define ZEDBSD_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
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
FILE *freopen(const char *, const char *, FILE *);
FILE *tmpfile(void);
char *tmpnam(char *);
int fclose(FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
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
int remove(const char *);

int asprintf(char **, const char *, ...);
int vasprintf(char **, const char *, va_list);
char *fgetln(FILE *, size_t *);
const char *fmtcheck(const char *, const char *);
int fpurge(FILE *);
FILE *funopen(const void *, int (*)(void *, char *, int),
    int (*)(void *, const char *, int), fpos_t (*)(void *, fpos_t, int),
    int (*)(void *));
void setbuffer(FILE *, char *, int);
int setlinebuf(FILE *);

#endif
