/* Test-only compile-time I/O seams for rcconf.c. */
#ifndef ZEDBSD_RCCONF_PERSISTENCE_HOOKS_H
#define ZEDBSD_RCCONF_PERSISTENCE_HOOKS_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stddef.h>
#include <sys/types.h>

ssize_t rcconf_test_write(int, const void *, size_t);
int rcconf_test_fsync(int);
int rcconf_test_rename(const char *, const char *);
int rcconf_test_unlink(const char *);

#endif
