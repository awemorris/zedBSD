/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_STDLIB_H
#define ZEDBSD_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

void *zedbsd_malloc(size_t size);
void *zedbsd_calloc(size_t count, size_t size);
void *zedbsd_realloc(void *pointer, size_t size);
void zedbsd_free(void *pointer);
char *zedbsd_strdup(const char *string);

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
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
int abs(int value);
long labs(long value);
int rand(void);
void srand(unsigned int seed);
void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
int posix_openpt(int);
int grantpt(int);
int unlockpt(int);
char *ptsname(int);
int ptsname_r(int, char *, size_t);

#endif
