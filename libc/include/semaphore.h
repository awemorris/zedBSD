/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SEMAPHORE_H
#define ZEDBSD_SEMAPHORE_H
#include <stdint.h>
#include <time.h>

#define SEM_VALUE_MAX 2147483647U
#define SEM_FAILED ((sem_t *)-1)
typedef struct {
	volatile uint32_t value;
	volatile uint32_t waiters;
	volatile uint32_t guard;
	uint32_t pshared;
	uint32_t magic;
} sem_t;

int sem_init(sem_t *, int, unsigned);
int sem_destroy(sem_t *);
int sem_wait(sem_t *);
int sem_trywait(sem_t *);
int sem_timedwait(sem_t *, const struct timespec *);
int sem_post(sem_t *);
int sem_getvalue(sem_t *, int *);
sem_t *sem_open(const char *, int, ...);
int sem_close(sem_t *);
int sem_unlink(const char *);
#endif
