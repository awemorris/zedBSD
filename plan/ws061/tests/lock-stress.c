/*
 * ws061-p008: contends every libc lock the three-state change touched and
 * checks the counts: mutex, condition variable, read-write lock, once and
 * semaphore, across four threads.  Prints LOCK-STRESS-OK or a FAIL line.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>

#define THREADS 4
#define ROUNDS 200000
#define ITEMS 100000

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static sem_t sem;
static long counter;
static long shared_value;
static int queue_count;
static long consumed;
static int once_runs;

static void once_body(void) { once_runs++; }

static void *
mutex_worker(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < ROUNDS; i++) {
		pthread_mutex_lock(&mutex);
		counter++;
		pthread_mutex_unlock(&mutex);
		pthread_once(&once, once_body);
		sem_wait(&sem);
		sem_post(&sem);
	}
	return NULL;
}

static void *
rw_worker(void *arg)
{
	long id = (long)arg;
	int i;

	for (i = 0; i < ROUNDS; i++) {
		if (id == 0 && i % 8 == 0) {
			pthread_rwlock_wrlock(&rwlock);
			shared_value++;
			pthread_rwlock_unlock(&rwlock);
		} else {
			pthread_rwlock_rdlock(&rwlock);
			(void)shared_value;
			pthread_rwlock_unlock(&rwlock);
		}
	}
	return NULL;
}

static void *
producer(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < ITEMS; i++) {
		pthread_mutex_lock(&mutex);
		queue_count++;
		pthread_cond_signal(&ready);
		pthread_mutex_unlock(&mutex);
	}
	return NULL;
}

static void *
consumer(void *arg)
{
	int taken = 0;

	(void)arg;
	while (taken < ITEMS / 2) {
		pthread_mutex_lock(&mutex);
		while (queue_count == 0)
			pthread_cond_wait(&ready, &mutex);
		queue_count--;
		consumed++;
		taken++;
		pthread_mutex_unlock(&mutex);
	}
	return NULL;
}

int
main(void)
{
	pthread_t t[THREADS];
	long i;

	sem_init(&sem, 0, 2);
	for (i = 0; i < THREADS; i++)
		pthread_create(&t[i], NULL, mutex_worker, NULL);
	for (i = 0; i < THREADS; i++)
		pthread_join(t[i], NULL);
	if (counter != (long)THREADS * ROUNDS || once_runs != 1) {
		printf("FAIL mutex/once counter=%ld once=%d\n", counter, once_runs);
		return 1;
	}
	for (i = 0; i < THREADS; i++)
		pthread_create(&t[i], NULL, rw_worker, (void *)i);
	for (i = 0; i < THREADS; i++)
		pthread_join(t[i], NULL);
	if (shared_value != ROUNDS / 8) {
		printf("FAIL rwlock value=%ld\n", shared_value);
		return 1;
	}
	pthread_create(&t[0], NULL, producer, NULL);
	pthread_create(&t[1], NULL, consumer, NULL);
	pthread_create(&t[2], NULL, consumer, NULL);
	for (i = 0; i < 3; i++)
		pthread_join(t[i], NULL);
	if (consumed != ITEMS || queue_count != 0) {
		printf("FAIL cond consumed=%ld left=%d\n", consumed, queue_count);
		return 1;
	}
	printf("LOCK-STRESS-OK\n");
	return 0;
}
