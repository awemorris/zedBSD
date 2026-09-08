/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_NDBM_H
#define LIBC_NDBM_H

#include <sys/types.h>

typedef struct { char *dptr; int dsize; } datum;
typedef struct __zedbsd_dbm DBM;

#define DBM_INSERT 0
#define DBM_REPLACE 1

DBM *dbm_open(const char *, int, mode_t);
void dbm_close(DBM *);
datum dbm_fetch(DBM *, datum);
int dbm_store(DBM *, datum, datum, int);
int dbm_delete(DBM *, datum);
datum dbm_firstkey(DBM *);
datum dbm_nextkey(DBM *);
int dbm_error(DBM *);
int dbm_clearerr(DBM *);

#endif
