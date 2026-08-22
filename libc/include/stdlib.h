/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_STDLIB_H
#define ZEDBSD_STDLIB_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
void *aligned_alloc(size_t, size_t);
char *strdup(const char *string);
char *getenv(const char *name);
int setenv(const char *, const char *, int);
int unsetenv(const char *);
int putenv(char *);
int clearenv(void);
extern char **environ;

int atoi(const char *string);
long atol(const char *string);
long long atoll(const char *string);
long strtol(const char *string, char **end, int base);
unsigned long strtoul(const char *string, char **end, int base);
long long strtoll(const char *string, char **end, int base);
unsigned long long strtoull(const char *string, char **end, int base);
double atof(const char *string);
double strtod(const char *string, char **end);
float strtof(const char *, char **);
long double strtold(const char *, char **);
int abs(int value);
long labs(long value);
long long llabs(long long);
div_t div(int, int);
ldiv_t ldiv(long, long);
lldiv_t lldiv(long long, long long);
void *bsearch(const void *, const void *, size_t, size_t,
    int (*)(const void *, const void *));
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int rand(void);
void srand(unsigned int seed);
void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
void _Exit(int) __attribute__((noreturn));
int atexit(void (*)(void));
int at_quick_exit(void (*)(void));
void quick_exit(int) __attribute__((noreturn));
int system(const char *);
int mblen(const char *, size_t);
int mbtowc(wchar_t *, const char *, size_t);
int wctomb(char *, wchar_t);
size_t mbstowcs(wchar_t *, const char *, size_t);
size_t wcstombs(char *, const wchar_t *, size_t);

uint32_t arc4random(void);
void arc4random_buf(void *, size_t);
uint32_t arc4random_uniform(uint32_t);
const char *getprogname(void);
void setprogname(const char *);
int heapsort(void *, size_t, size_t, int (*)(const void *, const void *));
int mergesort(void *, size_t, size_t, int (*)(const void *, const void *));
void qsort_r(void *, size_t, size_t,
    int (*)(const void *, const void *, void *), void *);
void *reallocarray(void *, size_t, size_t);
void *reallocf(void *, size_t);
void *recallocarray(void *, size_t, size_t, size_t);
void srandomdev(void);
long long strtonum(const char *, long long, long long, const char **);
int posix_openpt(int);
int grantpt(int);
int unlockpt(int);
char *ptsname(int);
int ptsname_r(int, char *, size_t);

#endif
