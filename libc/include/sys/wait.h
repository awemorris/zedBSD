/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_WAIT_H
#define ZEDBSD_SYS_WAIT_H

#include <sys/types.h>
#include <signal.h>

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) ((((status) & 0x7f) != 0) && \
	(((status) & 0x7f) != 0x7f))
#define WTERMSIG(status) ((status) & 0x7f)
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status) (((status) >> 8) & 0xff)
#define WIFCONTINUED(status) ((status) == 0xffff)
#define WNOHANG 0x0001
#define WUNTRACED 0x0002
#define WCONTINUED 0x0004
#define WEXITED 0x0008
#define WSTOPPED WUNTRACED
#define WNOWAIT 0x0010

typedef enum { P_ALL = 0, P_PID = 1, P_PGID = 2 } idtype_t;

pid_t waitpid(pid_t, int *, int);
pid_t wait(int *);
int waitid(idtype_t, id_t, siginfo_t *, int);

#endif
