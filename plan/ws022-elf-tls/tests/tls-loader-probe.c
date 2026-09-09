/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <unistd.h>

#if defined(HAL_ARCH_I386)
#define TLS_ARCH "i386"
#else
#define TLS_ARCH "amd64"
#endif

static const char *cases[] = {
	"filesz-over-memsz", "file-outside", "file-truncated-range",
	"alignment-three", "alignment-over-limit", "address-overflow",
	"memory-over-limit", "offset-incongruent", "duplicate", "truncated"
};

static const char *valid_cases[] = {"", "-empty", "-zero", "-offset"};

static __thread volatile unsigned thread_value = 0x314159U;
static __thread volatile unsigned thread_zero;
static __thread volatile sig_atomic_t thread_signals;
static __thread unsigned char thread_aligned[129] __attribute__((aligned(64)));
static uintptr_t addresses[8];
static volatile unsigned start_threads;

static void
signal_tls(int number)
{
	if (number == SIGUSR1)
		thread_signals++;
}

static void *
thread_tls(void *argument)
{
	uintptr_t id;
	volatile uintptr_t address;
	unsigned i;

	id = (uintptr_t)argument;
	address = (uintptr_t)thread_aligned;
	if ((address & 63U) != 0)
		return (void *)1;
	if (thread_value != 0x314159U || thread_zero != 0 || thread_signals != 0)
		return (void *)2;
	addresses[id] = (uintptr_t)&thread_value;
	thread_value = (unsigned)id;
	thread_zero = (unsigned)id + 17U;
	errno = (int)id + 31;
	while (!__atomic_load_n(&start_threads, __ATOMIC_ACQUIRE))
		sched_yield();
	for (i = 0; i < 100; i++) {
		sched_yield();
		if (thread_value != id || thread_zero != id + 17U || errno != (int)id + 31)
			return (void *)3;
	}
	if (raise(SIGUSR1) != 0 || thread_signals != 1)
		return (void *)4;
	return NULL;
}

static int
runtime_tls(void)
{
	pthread_t threads[8];
	struct sigaction action;
	struct rlimit saved;
	struct rlimit limited;
	void *result;
	unsigned i;
	unsigned j;
	pid_t child;
	int status;
	int error;

	if (thread_value != 0x314159U || thread_zero != 0)
		return 10;
	thread_value = 90;
	thread_zero = 91;
	memset(&action, 0, sizeof(action));
	action.sa_handler = (uintptr_t)signal_tls;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGUSR1, &action, NULL) != 0)
		return 11;
	for (i = 0; i < 8; i++) {
		if (pthread_create(&threads[i], NULL, thread_tls, (void *)(uintptr_t)i) != 0)
			return 12;
	}
	__atomic_store_n(&start_threads, 1U, __ATOMIC_RELEASE);
	for (i = 0; i < 8; i++) {
		error = pthread_join(threads[i], &result);
		if (error != 0 || result != NULL) {
			printf("TLS-RUNTIME JOIN index=%u error=%d result=%lu\n", i, error, (unsigned long)result);
			return 13;
		}
		if (addresses[i] == (uintptr_t)&thread_value)
			return 14;
		for (j = 0; j < i; j++) {
			if (addresses[i] == addresses[j])
				return 15;
		}
	}
	for (i = 0; i < 100; i++) {
		if (pthread_create(&threads[0], NULL, thread_tls, NULL) != 0)
			return 16;
		if (pthread_join(threads[0], &result) != 0 || result != NULL)
			return 17;
	}
	if (thread_value != 90 || thread_zero != 91 || thread_signals != 0)
		return 18;
	child = fork();
	if (child == 0) {
		if (thread_value != 90 || thread_zero != 91)
			_exit(1);
		thread_value = 999;
		_exit(0);
	}
	if (child < 0 || waitpid(child, &status, 0) != child || status != 0)
		return 19;
	if (thread_value != 90)
		return 20;

	/* A bounded address-space failure must leave the parent TLS usable. */
	if (getrlimit(RLIMIT_AS, &saved) != 0)
		return 21;
	limited = saved;
	limited.rlim_cur = 0;
	if (setrlimit(RLIMIT_AS, &limited) != 0)
		return 22;
	error = pthread_create(&threads[0], NULL, thread_tls, NULL);
	if (setrlimit(RLIMIT_AS, &saved) != 0)
		return 23;
	if (error == 0) {
		(void)pthread_join(threads[0], &result);
		return 24;
	}
	if (pthread_create(&threads[0], NULL, thread_tls, NULL) != 0)
		return 25;
	if (pthread_join(threads[0], &result) != 0 || result != NULL)
		return 26;
	puts("TLS-RUNTIME PASS concurrent=8 repeated=100 signal fork failed-create-recovery");
	return 0;
}

int
main(void)
{
	char path[128];
	char *args[2];
	char *env[1];
	unsigned i;
	pid_t child;
	int status;

	status = runtime_tls();
	if (status != 0) {
		printf("TLS-RUNTIME FAIL step=%d\n", status);
		return 1;
	}
	args[0] = path;
	args[1] = NULL;
	env[0] = NULL;
	/* Every rejected exec returns to this exact old process image. */
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		snprintf(path, sizeof(path), "/usr/share/tls/" TLS_ARCH "-%s.elf", cases[i]);
		if (chmod(path, 0755) != 0)
			return 5;
		errno = 0;
		if (execve(path, args, env) != -1 || errno != ENOEXEC) {
			printf("TLS-LOADER FAIL case=%s errno=%d\n", cases[i], errno);
			return 1;
		}
	}
	for (i = 0; i < sizeof(valid_cases) / sizeof(valid_cases[0]); i++) {
		snprintf(path, sizeof(path), "/usr/share/tls/" TLS_ARCH "%s.elf", valid_cases[i]);
		if (chmod(path, 0755) != 0)
			return 6;
		child = fork();
		if (child == 0) {
			execve(path, args, env);
			_exit(99);
		}
		if (child < 0)
			return 2;
		if (waitpid(child, &status, 0) != child)
			return 3;
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("TLS-LOADER FAIL case=%s child=%d\n", valid_cases[i], status);
			return 4;
		}
	}
	if (thread_value != 90 || thread_zero != 91)
		return 7;
	puts("TLS-LOADER PASS rejected=10 initial=4");
	return 0;
}
