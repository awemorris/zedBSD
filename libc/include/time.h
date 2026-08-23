/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_TIME_H
#define ZEDBSD_TIME_H

#include <stdint.h>
#include <stddef.h>
#include <locale.h>

typedef int64_t time_t;
typedef int clockid_t;
typedef int32_t timer_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  2
#define TIME_UTC 1
#define TIMER_ABSTIME   1
#define UTIME_NOW  1073741823L
#define UTIME_OMIT 1073741822L
struct timespec { time_t tv_sec; long tv_nsec; };
struct itimerspec { struct timespec it_interval; struct timespec it_value; };
struct tm {
	int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
	int tm_wday, tm_yday, tm_isdst;
};
struct sigevent;
extern char *tzname[2];
extern int daylight;
extern long timezone;
extern int getdate_err;
char *asctime(const struct tm *);
char *asctime_r(const struct tm *restrict, char *restrict);
clock_t clock(void);
char *ctime(const time_t *);
char *ctime_r(const time_t *restrict, char *restrict);
double difftime(time_t, time_t);
struct tm *gmtime(const time_t *);
struct tm *gmtime_r(const time_t *restrict, struct tm *restrict);
struct tm *localtime(const time_t *);
struct tm *localtime_r(const time_t *restrict, struct tm *restrict);
time_t mktime(struct tm *);
size_t strftime(char *restrict, size_t, const char *restrict,
	const struct tm *restrict);
size_t strftime_l(char *restrict, size_t, const char *restrict,
	const struct tm *restrict, locale_t);
time_t time(time_t *result);
int timespec_get(struct timespec *, int);
int clock_gettime(clockid_t, struct timespec *);
int clock_getres(clockid_t, struct timespec *);
int clock_settime(clockid_t, const struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);
int clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);
int timer_create(clockid_t, const struct sigevent *, timer_t *);
int timer_delete(timer_t);
int timer_settime(timer_t, int, const struct itimerspec *, struct itimerspec *);
int timer_gettime(timer_t, struct itimerspec *);
int timer_getoverrun(timer_t);
void tzset(void);
char *strptime(const char *restrict, const char *restrict, struct tm *restrict);
struct tm *getdate(const char *);

#endif
