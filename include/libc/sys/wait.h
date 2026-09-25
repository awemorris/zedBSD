/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_WAIT_H
#define LIBC_SYS_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <signal.h>
#include <uapi/wait.h>

pid_t waitpid(pid_t, int *, int);

/*
 * Reaps a child and reports what it spent.  The cost cannot be asked for
 * afterwards, because by then the child is gone, which is why it is
 * reported by the call that reaps it.  A null usage asks for the reaping
 * alone.
 */
struct rusage;
pid_t wait4(pid_t, int *, int, struct rusage *);
pid_t wait(int *);
int waitid(idtype_t, id_t, siginfo_t *, int);

#ifdef __cplusplus
}
#endif

#endif
