/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UTMPX_H
#define ZEDBSD_UTMPX_H

#include <stdint.h>
#include <sys/types.h>

#define EMPTY 0
#define BOOT_TIME 2
#define LOGIN_PROCESS 5
#define USER_PROCESS 7
#define DEAD_PROCESS 8
#define UT_LINESIZE 32
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 64
#define _PATH_UTMP "/var/run/utmp"

struct utmpx {
	int16_t ut_type;
	int16_t ut_reserved0;
	pid_t ut_pid;
	int32_t ut_session;
	char ut_id[8];
	char ut_line[UT_LINESIZE];
	char ut_user[UT_NAMESIZE];
	char ut_host[UT_HOSTSIZE];
	int64_t ut_tv_sec;
	int32_t ut_tv_usec;
	uint32_t ut_reserved[8];
};

void setutxent(void);
struct utmpx *getutxent(void);
void endutxent(void);
struct utmpx *getutxid(const struct utmpx *);
struct utmpx *getutxline(const struct utmpx *);
struct utmpx *pututxline(const struct utmpx *);

#endif
