/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SIGNAL_H
#define ZEDBSD_SIGNAL_H
#include <zedbsd/signal.h>
#include <sys/types.h>
typedef void (*sighandler_t)(int);
typedef void (*siginfo_handler_t)(int, siginfo_t *, void *);
#define SIG_ERR ((sighandler_t)-1)
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigpending(sigset_t *);
int sigsuspend(const sigset_t *);
int kill(pid_t, int);
sighandler_t signal(int, sighandler_t);
#endif
