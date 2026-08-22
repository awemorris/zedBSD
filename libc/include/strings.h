/* Traditional BSD string interfaces. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_STRINGS_H
#define ZEDBSD_STRINGS_H
#include <stddef.h>
int bcmp(const void *, const void *, size_t);
void bcopy(const void *, void *, size_t);
void bzero(void *, size_t);
void explicit_bzero(void *, size_t);
int ffs(int);
int ffsl(long);
int ffsll(long long);
int fls(int);
int flsl(long);
int flsll(long long);
char *index(const char *, int);
char *rindex(const char *, int);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
#endif
