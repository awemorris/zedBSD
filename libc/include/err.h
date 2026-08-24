/* BSD error-reporting interfaces. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ERR_H
#define ZEDBSD_ERR_H
#include <stdarg.h>
#include <stdio.h>
void err(int, const char *, ...) __attribute__((__noreturn__));
void errc(int, int, const char *, ...) __attribute__((__noreturn__));
void errx(int, const char *, ...) __attribute__((__noreturn__));
void verr(int, const char *, va_list) __attribute__((__noreturn__));
void verrc(int, int, const char *, va_list) __attribute__((__noreturn__));
void verrx(int, const char *, va_list) __attribute__((__noreturn__));
void warn(const char *, ...);
void warnc(int, const char *, ...);
void warnx(const char *, ...);
void vwarn(const char *, va_list);
void vwarnc(int, const char *, va_list);
void vwarnx(const char *, va_list);
void err_set_file(void *);
void err_set_exit(void (*)(int));
#endif
