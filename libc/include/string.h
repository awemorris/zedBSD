/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_STRING_H
#define ZEDBSD_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t count);
void *memmove(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
void *memchr(const void *memory, int character, size_t count);
int memcmp(const void *left, const void *right, size_t count);
void *memccpy(void *destination, const void *source, int character,
	      size_t count);
size_t strlen(const char *string);
size_t strnlen(const char *string, size_t maximum);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t count);
char *strcat(char *destination, const char *source);
char *strncat(char *destination, const char *source, size_t count);
char *strchr(const char *string, int character);
char *strrchr(const char *string, int character);
char *strstr(const char *haystack, const char *needle);
size_t strcspn(const char *, const char *);
char *strpbrk(const char *, const char *);
size_t strspn(const char *, const char *);
char *strtok(char *, const char *);
char *strdup(const char *string);
char *strndup(const char *string, size_t maximum);
char *strerror(int error);
int strcoll(const char *, const char *);
size_t strxfrm(char *, const char *, size_t);

void *memmem(const void *, size_t, const void *, size_t);
void *mempcpy(void *, const void *, size_t);
void *memrchr(const void *, int, size_t);
void *memset_explicit(void *, int, size_t);
char *strcasestr(const char *, const char *);
char *strchrnul(const char *, int);
size_t strlcat(char *, const char *, size_t);
size_t strlcpy(char *, const char *, size_t);
void strmode(unsigned int, char *);
char *strnstr(const char *, const char *, size_t);
char *strsep(char **, const char *);
int strverscmp(const char *, const char *);
int timingsafe_bcmp(const void *, const void *, size_t);
int timingsafe_memcmp(const void *, const void *, size_t);

#endif
