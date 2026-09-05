/* Native pthread bridge: compile without the kernel's libc include paths.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include "mount-thread-host.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct host_thread {
	pthread_t thread;
	void (*run)(void *);
	void *argument;
};
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
static unsigned arrived[8], released[8];

static void require(int condition)
{
	if (!condition) {
		fputs("mount pthread gate failed or timed out\n", stderr);
		abort();
	}
}

static void *thread_main(void *argument)
{
	struct host_thread *thread = argument;
	thread->run(thread->argument);
	return NULL;
}

void *host_thread_start(void (*run)(void *), void *argument)
{
	struct host_thread *thread = malloc(sizeof(*thread));
	require(thread != NULL);
	thread->run = run;
	thread->argument = argument;
	require(pthread_create(&thread->thread, NULL, thread_main, thread) == 0);
	return thread;
}

void host_thread_join(void *argument)
{
	struct host_thread *thread = argument;
	require(pthread_join(thread->thread, NULL) == 0);
	free(thread);
}

void host_thread_yield(void) { sched_yield(); }

void host_gate_reset(unsigned gate)
{
	require(gate < 8);
	require(pthread_mutex_lock(&gate_lock) == 0);
	arrived[gate] = released[gate] = 0;
	require(pthread_mutex_unlock(&gate_lock) == 0);
}

void host_gate_signal(unsigned gate)
{
	require(gate < 8);
	require(pthread_mutex_lock(&gate_lock) == 0);
	arrived[gate] = 1;
	require(pthread_cond_broadcast(&gate_changed) == 0);
	require(pthread_mutex_unlock(&gate_lock) == 0);
}

static void wait_for(unsigned *value)
{
	struct timespec deadline;
	require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += 10;
	while (!*value)
		require(pthread_cond_timedwait(&gate_changed, &gate_lock,
		    &deadline) == 0);
}

void host_gate_pause(unsigned gate)
{
	if (gate == 0)
		return;
	host_gate_signal(gate);
	require(pthread_mutex_lock(&gate_lock) == 0);
	wait_for(&released[gate]);
	require(pthread_mutex_unlock(&gate_lock) == 0);
}

void host_gate_wait(unsigned gate)
{
	require(gate < 8);
	require(pthread_mutex_lock(&gate_lock) == 0);
	wait_for(&arrived[gate]);
	require(pthread_mutex_unlock(&gate_lock) == 0);
}

void host_gate_release(unsigned gate)
{
	require(gate < 8);
	require(pthread_mutex_lock(&gate_lock) == 0);
	released[gate] = 1;
	require(pthread_cond_broadcast(&gate_changed) == 0);
	require(pthread_mutex_unlock(&gate_lock) == 0);
}
