/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_THREADS_H
#define ZEDBSD_THREADS_H

#include <pthread.h>
#include <time.h>

#define ONCE_FLAG_INIT	PTHREAD_ONCE_INIT
#define TSS_DTOR_ITERATIONS	4

enum {
	thrd_success = 0,
	thrd_nomem = 1,
	thrd_timedout = 2,
	thrd_busy = 3,
	thrd_error = 4
};

enum {
	mtx_plain = 0,
	mtx_recursive = 1,
	mtx_timed = 2
};

typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_once_t once_flag;
typedef pthread_key_t tss_t;
typedef int (*thrd_start_t)(void *argument);
typedef void (*tss_dtor_t)(void *value);

void call_once(once_flag *flag, void (*function)(void));
int cnd_broadcast(cnd_t *condition);
void cnd_destroy(cnd_t *condition);
int cnd_init(cnd_t *condition);
int cnd_signal(cnd_t *condition);
int cnd_timedwait(cnd_t *condition, mtx_t *mutex,
	const struct timespec *abstime);
int cnd_wait(cnd_t *condition, mtx_t *mutex);
void mtx_destroy(mtx_t *mutex);
int mtx_init(mtx_t *mutex, int type);
int mtx_lock(mtx_t *mutex);
int mtx_timedlock(mtx_t *mutex, const struct timespec *abstime);
int mtx_trylock(mtx_t *mutex);
int mtx_unlock(mtx_t *mutex);
int thrd_create(thrd_t *thread, thrd_start_t function, void *argument);
thrd_t thrd_current(void);
int thrd_detach(thrd_t thread);
int thrd_equal(thrd_t left, thrd_t right);
_Noreturn void thrd_exit(int result);
int thrd_join(thrd_t thread, int *result);
int thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void thrd_yield(void);
int tss_create(tss_t *key, tss_dtor_t destructor);
void tss_delete(tss_t key);
void *tss_get(tss_t key);
int tss_set(tss_t key, void *value);

#endif
