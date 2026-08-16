/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_MQUEUE_H
#define ZEDBSD_MQUEUE_H
#include <signal.h>
#include <sys/types.h>
#include <time.h>

typedef int mqd_t;
#define MQ_PRIO_MAX 32

struct mq_attr {
	long mq_flags;
	long mq_maxmsg;
	long mq_msgsize;
	long mq_curmsgs;
};

mqd_t mq_open(const char *, int, ...);
int mq_close(mqd_t);
int mq_unlink(const char *);
int mq_send(mqd_t, const char *, size_t, unsigned);
int mq_timedsend(mqd_t, const char *, size_t, unsigned,
	const struct timespec *);
ssize_t mq_receive(mqd_t, char *, size_t, unsigned *);
ssize_t mq_timedreceive(mqd_t, char *, size_t, unsigned *,
	const struct timespec *);
int mq_getattr(mqd_t, struct mq_attr *);
int mq_setattr(mqd_t, const struct mq_attr *, struct mq_attr *);
int mq_notify(mqd_t, const struct sigevent *);

#endif
