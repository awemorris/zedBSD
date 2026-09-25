/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_TIME_H
#define KERN_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <locale.h>
#include <uapi/signal.h>
#include <uapi/time.h>

typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L
#define TIME_UTC 1
struct tm {
	int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
	int tm_wday, tm_yday, tm_isdst;

	/*
	 * How far this time is from Coordinated Universal Time, and what the
	 * zone it belongs to is called.  A broken-down time is otherwise
	 * ambiguous: the same fields describe two different instants
	 * depending on where they were read.
	 */
	long tm_gmtoff;
	const char *tm_zone;
};
extern char *tzname[2];
extern int daylight;
extern long timezone;
extern int getdate_err;
char *asctime(const struct tm *);
char *asctime_r(const struct tm *__restrict, char *__restrict);
clock_t clock(void);
char *ctime(const time_t *);
char *ctime_r(const time_t *__restrict, char *__restrict);
double difftime(time_t, time_t);
struct tm *gmtime(const time_t *);
struct tm *gmtime_r(const time_t *__restrict, struct tm *__restrict);

/*
 * Reads a broken-down time as UTC and normalises it, which is the one thing
 * mktime cannot be asked to do.
 */
time_t timegm(struct tm *);
struct tm *localtime(const time_t *);
struct tm *localtime_r(const time_t *__restrict, struct tm *__restrict);
time_t mktime(struct tm *);
size_t strftime(char *__restrict, size_t, const char *__restrict,
	const struct tm *__restrict);
size_t strftime_l(char *__restrict, size_t, const char *__restrict,
	const struct tm *__restrict, locale_t);
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
char *strptime(const char *__restrict, const char *__restrict, struct tm *__restrict);
struct tm *getdate(const char *);

#ifdef __cplusplus
}
#endif

#endif
