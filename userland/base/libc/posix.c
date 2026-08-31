/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library posix support.
 */

#include "userland/base/libc/syscall.h"
#include "libc/heap.h"
#include "libc/stdio-internal.h"

#include <zedbsd/auxv.h>
#include <zedbsd/dirent.h>
#include <zedbsd/fcntl.h>
#include <zedbsd/console.h>
#include <zedbsd/syscall.h>
#include <sys/sysctl.h>
#include <zedbsd/process.h>
#include <zedbsd/netif.h>
#include <zedbsd/route.h>
#include <zedbsd/rtld-abi.h>
#include <dirent.h>
#include <devctl.h>
#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/quota.h>
#include <sys/snapshot.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ZEDBSD_USER_PAGE_SIZE
#define ZEDBSD_USER_PAGE_SIZE 4096
#endif

char **environ;
char *optarg;
int opterr = 1;
int optind = 1;
int optopt;

#define ENVIRONMENT_MAX 64U
static char *environment_entries[ENVIRONMENT_MAX + 1U];
static unsigned char environment_owned[ENVIRONMENT_MAX];
static unsigned secure_execution;
extern void __pthread_cancel_point(void) __attribute__((weak));
extern void __pthread_fork_prepare(void) __attribute__((weak));
extern void __pthread_fork_parent(void) __attribute__((weak));
extern void __pthread_fork_child(void) __attribute__((weak));
extern void __timer_sigev_thread_fork_child(void) __attribute__((weak));
extern void __pthread_initialize_main(void) __attribute__((weak));
extern void __libc_environment_lock(void) __attribute__((weak));
extern void __libc_environment_unlock(void) __attribute__((weak));
extern char *__pthread_environment_exchange(char *) __attribute__((weak));
#if !defined(ZEDBSD_DYNAMIC_LIBC)
extern void __rtld_process_fini(void) __attribute__((weak));
extern void __rtld_startup_init(void) __attribute__((weak));
#endif

static char *bootstrap_environment_value;

static uintptr_t process_break;
static int process_break_known;

enum {
	SPAWN_ACTION_CLOSE = 1,
	SPAWN_ACTION_DUP2,
	SPAWN_ACTION_OPEN,
	SPAWN_ACTION_CHDIR,
	SPAWN_ACTION_FCHDIR,
};

struct __dir_stream {
	int fd;
	struct dirent current;
};

static volatile uint32_t stream_registry_lock;
static FILE *stream_registry;

static char timezone_standard[16] = "UTC";
static char timezone_daylight[16] = "UTC";
char *tzname[2] = {timezone_standard, timezone_daylight};
static long timezone_east;
static long timezone_daylight_east;
static int timezone_has_daylight;
long timezone;
int daylight;

enum timezone_rule_kind { TZ_RULE_JULIAN, TZ_RULE_DAY, TZ_RULE_MONTH };
struct timezone_rule {
	enum timezone_rule_kind kind;
	int first, second, third, seconds;
};
static struct timezone_rule timezone_start = {TZ_RULE_MONTH, 3, 2, 0, 2 * 3600};
static struct timezone_rule timezone_end = {TZ_RULE_MONTH, 11, 1, 0, 2 * 3600};

static struct heap_allocator user_heap;

static void environment_lock(void);
static int environment_name(const char *entry, const char *name);
static void environment_unlock(void);
static void environment_value_replace(char *replacement);
static intptr_t call(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);
static void cancel_point(void);
static ssize_t positional_vector_io(int fd, const struct iovec *iov, int count, off_t offset, int writing);
static int ioctl_has_argument(unsigned long request);
static long path_limit(int name);
static int aio_submit(struct aiocb *control, int writing, int notify);
static void aio_notify(const struct sigevent *event);
static struct __spawn_action *spawn_action_add(posix_spawn_file_actions_t *actions);
static int posix_spawn_common(pid_t *result, const char *path, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr, char *const argv[], char *const envp[], int search);
static int spawn_child_setup(const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr);
static int spawn_exec_search(const char *file, char *const argv[], char *const environment[]);
static const char *spawn_environment_path(char *const environment[]);
static int exec_search(const char *file, char *const argv[], char *const envp[]);
static int exec_with_shell(const char *path, char *const argv[], char *const envp[]);
static int exec_varargs(const char *path, const char *first, va_list arguments, int search, int explicit_environment);
static void stream_register(FILE *stream);
static void stream_registry_acquire(void);
static void stream_registry_release(void);
static void stream_unregister_locked(FILE *stream);
static int stream_flush_locked(FILE *stream);
static int stream_write_direct(FILE *stream, const unsigned char *buffer, size_t length, size_t *written);
static int stream_fd(FILE *stream);
static void stream_enter(FILE *stream, int *cancel_state);
static void stream_leave(FILE *stream, int cancel_state);
static ssize_t stream_read_direct(FILE *stream, void *buffer, size_t length);
static int stream_ensure_buffer(FILE *stream);
static int64_t floor_div(int64_t value, int64_t divisor);
static void civil_from_days(int64_t days, int *year, unsigned *month, unsigned *day, unsigned *year_day);
static int64_t days_from_civil(int64_t year, unsigned month, unsigned day);
static const char *timezone_name(const char *text, char *name, size_t capacity);
static const char *timezone_offset(const char *text, long *east);
static const char *timezone_rule_parse(const char *text, struct timezone_rule *rule);
static const char *timezone_number(const char *text, int *number);
static int timezone_is_daylight(time_t value);
static int timezone_rule_yday(int year, const struct timezone_rule *rule);
static int calendar_leap_year(int year);
static int strftime_append(char **output, size_t *remaining, const char *text);
static int strftime_number(char **output, size_t *remaining, int value, int width, char padding);
static void iso_week(const struct tm *value, int *year, int *week);
static int iso_weeks_in_year(int year);
static size_t user_heap_grow(void *context, void *end, size_t minimum);

/*
 * Implements the getopt operation.
 */
int
getopt(
	int argc,
	char *const argv[],
	const char *options)
{
	static const char *next;
	const char *definition;
	int option;

	optarg = NULL;

	/* Validates the command-line arguments. */
	if (optind == 0) {
		optind = 1;
		next = NULL;
	}

	/* Validates the command-line arguments. */
	if (argc < 0 || argv == NULL || options == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the next availability. */
	if (next == NULL || *next == '\0') {
		/* Validates the command-line arguments. */
		if (optind >= argc || argv[optind] == NULL ||
		    argv[optind][0] != '-' || argv[optind][1] == '\0')

			/* Reports operation failure. */
			return -1;

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[optind], "--")) {
			optind++;

			/* Reports operation failure. */
			return -1;
		}
		next = argv[optind++] + 1;
	}
	option = (unsigned char)*next++;
	optopt = option;
	definition = strchr(options + (options[0] == ':'), option);

	/* Handles the definition availability. */
	if (option == ':' || definition == NULL) {
		/* Handles the next condition. */
		if (*next == '\0')
			next = NULL;

		/* Handles the opterr condition. */
		if (opterr && options[0] != ':')
			fprintf(stderr, "%s: illegal option -- %c\n",
				argv[0] != NULL ? argv[0] : "", option);

		/* Returns the computed result. */
		return '?';
	}

	/* Handles the definition condition. */
	if (definition[1] == ':') {
		/* Handles the next condition. */
		if (*next != '\0') {
			optarg = (char *)(uintptr_t)next;
			next = NULL;
		} else if (optind < argc) {
			optarg = argv[optind++];
			next = NULL;
		} else {
			next = NULL;

			/* Handles the opterr condition. */
			if (opterr && options[0] != ':')
				fprintf(
				    stderr,
				    "%s: option requires an argument -- %c\n",
				    argv[0] != NULL ? argv[0] : "", option);

			/* Returns the computed result. */
			return options[0] == ':' ? ':' : '?';
		}
	} else if (*next == '\0') {
		next = NULL;
	}

	/* Returns the computed result. */
	return option;
}

/*
 * Implements the getenv operation.
 */
char *
getenv(
	const char *name)
{
	unsigned i;
	char *result, *snapshot;

	result = NULL;
	snapshot = NULL;

	/* Handles a failed strchr operation. */
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL)
		return NULL;
	environment_lock();

	/* Process each element required by the operation. */
	for (i = 0; environ != NULL && environ[i] != NULL; i++)

		/* Handles the environment name condition. */
		if (environment_name(environ[i], name)) {
			result = strchr(environ[i], '=') + 1;
			snapshot = strdup(result);
			break;
		}
	environment_unlock();

	/*
 * Replacing the calling thread's snapshot, including on a miss, defines
	 * the lifetime as lasting until its next environment operation. */
	environment_value_replace(snapshot);

	/* Returns the computed result. */
	return snapshot;
}

/*
 * Implements the secure getenv operation.
 */
char *
secure_getenv(
	const char *name)
{
	char *function_result;

	/* Computes the function result. */
	function_result = secure_execution ? NULL : getenv(name);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setenv operation.
 */
int
setenv(
	const char *name,
	const char *value,
	int overwrite)
{
	size_t name_length, value_length;
	char *entry;
	unsigned i, empty;

	empty = ENVIRONMENT_MAX;

	/* Handles a failed strchr operation. */
	if (name == NULL || value == NULL || name[0] == '\0' ||
	    strchr(name, '=') != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	environment_lock();

	/* Process each element required by the operation. */
	for (i = 0; i < ENVIRONMENT_MAX; i++) {
		/* Handles the environ condition. */
		if (environ[i] == NULL && empty == ENVIRONMENT_MAX)
			empty = i;

		/* Handles the environment name condition. */
		if (environment_name(environ[i], name)) {
			/* Handles the overwrite condition. */
			if (!overwrite) {
				environment_unlock();

				/* Reports successful completion. */
				return 0;
			}
			empty = i;
			break;
		}
	}

	/* Handles the empty condition. */
	if (empty == ENVIRONMENT_MAX) {
		environment_unlock();
		errno = ENOSPC;

		/* Reports operation failure. */
		return -1;
	}
	name_length = strlen(name);
	value_length = strlen(value);

	/* Handles the name length condition. */
	if (name_length > SIZE_MAX - value_length - 2U) {
		environment_unlock();
		errno = ENOMEM;

		/* Reports operation failure. */
		return -1;
	}
	entry = malloc(name_length + value_length + 2U);

	/* Handles the entry availability. */
	if (entry == NULL) {
		environment_unlock();
		errno = ENOMEM;

		/* Reports operation failure. */
		return -1;
	}
	memcpy(entry, name, name_length);
	entry[name_length] = '=';
	memcpy(entry + name_length + 1U, value, value_length + 1U);

	/* Handles the environment owned condition. */
	if (environment_owned[empty])
		free(environ[empty]);
	environ[empty] = entry;
	environment_owned[empty] = 1U;
	environ[empty + 1U] = NULL;
	environment_unlock();
	environment_value_replace(NULL);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the unsetenv operation.
 */
int
unsetenv(
	const char *name)
{
	unsigned j;
	unsigned i;

	/* Handles a failed strchr operation. */
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	environment_lock();

	/* Process each element required by the operation. */
	for (i = 0; i < ENVIRONMENT_MAX && environ[i] != NULL;)

		/* Handles the environment name condition. */
		if (environment_name(environ[i], name)) {
			/* Handles the environment owned condition. */
			if (environment_owned[i])
				free(environ[i]);

			/* Process each element required by the operation. */
			for (j = i; j < ENVIRONMENT_MAX; j++) {
				environ[j] = environ[j + 1U];
				environment_owned[j] =
				    j + 1U < ENVIRONMENT_MAX
					? environment_owned[j + 1U]
					: 0U;
			}
		} else {
			i++;
		}
	environment_unlock();
	environment_value_replace(NULL);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the putenv operation.
 */
int
putenv(
	char *entry)
{
	char *equal;
	size_t name_length;
	unsigned i, empty;

	empty = ENVIRONMENT_MAX;

	/* Handles a failed strchr operation. */
	if (entry == NULL || entry[0] == '\0' ||
	    (equal = strchr(entry, '=')) == NULL || equal == entry) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	name_length = (size_t)(equal - entry);
	environment_lock();

	/* Process each element required by the operation. */
	for (i = 0; i < ENVIRONMENT_MAX; i++) {
		/* Handles the environ condition. */
		if (environ[i] == NULL && empty == ENVIRONMENT_MAX)
			empty = i;

		/* Handles the environ condition. */
		if (environ[i] != NULL &&
		    !strncmp(environ[i], entry, name_length) &&
		    environ[i][name_length] == '=') {
			empty = i;
			break;
		}
	}

	/* Handles the empty condition. */
	if (empty == ENVIRONMENT_MAX) {
		environment_unlock();
		errno = ENOSPC;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the environment owned condition. */
	if (environment_owned[empty])
		free(environ[empty]);
	environ[empty] = entry;
	environment_owned[empty] = 0;
	environ[empty + 1U] = NULL;
	environment_unlock();
	environment_value_replace(NULL);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the clearenv operation.
 */
int
clearenv(
	void)
{
	unsigned i;

	environment_lock();

	/* Process each element required by the operation. */
	for (i = 0; i < ENVIRONMENT_MAX && environ[i] != NULL; i++) {
		/* Handles the environment owned condition. */
		if (environment_owned[i])
			free(environ[i]);
		environ[i] = NULL;
		environment_owned[i] = 0;
	}
	environment_entries[0] = NULL;
	environ = environment_entries;
	environment_unlock();
	environment_value_replace(NULL);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the syscall result operation.
 */
intptr_t
syscall_result(
	intptr_t result)
{
	/* Checks the operation result. */
	if (result < 0 && result >= -4095) {
		errno = (int)-result;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the exit operation.
 */
void
_exit(
	int status)
{
	(void)__syscall6(ZEDBSD_SYS_exit, (uintptr_t)status, 0, 0, 0, 0, 0);

	/* Continue until the operation reaches a terminal state. */
	for (;;)
		;
}

/*
 * Implements the open operation.
 */
int
open(
	const char *path,
	int flags,
	...)
{
	int function_result;
	mode_t mode;

	mode = 0;

	/* Checks the active flags. */
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_open, (uintptr_t)path, flags, mode, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the openat operation.
 */
int
openat(
	int dirfd,
	const char *path,
	int flags,
	...)
{
	int function_result;
	mode_t mode;

	mode = 0;

	/* Checks the active flags. */
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_openat, dirfd, (uintptr_t)path, flags, mode,
			 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the close operation.
 */
int
close(
	int fd)
{
	int result;

	result = (int)call(ZEDBSD_SYS_close, fd, 0, 0, 0, 0, 0);

	/* Handles the reported system error. */
	if (result < 0 && errno == EINTR)
		errno = EINPROGRESS;

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the posix close operation.
 */
int
posix_close(
	int fd,
	int flag)
{
	int result;

	result = close(fd);

	/* Handles the reported system error. */
	if (result < 0 && errno == EINTR)
		errno = EINPROGRESS;

	/* Checks the operation result. */
	if (result == 0 && flag != 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the dup operation.
 */
int
dup(
	int fd)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_dup, fd, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the dup2 operation.
 */
int
dup2(
	int oldfd,
	int newfd)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_dup2, oldfd, newfd, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the dup3 operation.
 */
int
dup3(
	int oldfd,
	int newfd,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_dup3, oldfd, newfd, flags, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fcntl operation.
 */
int
fcntl(
	int fd,
	int command,
	...)
{
	int result;

	va_list ap;
	intptr_t argument = 0;
	int owner_result = 0;
	struct flock *native = NULL;
	struct flock_record request;

	/* Handles the command condition. */
	if (command == F_DUPFD || command == F_DUPFD_CLOEXEC ||
	    command == F_DUPFD_CLOFORK || command == F_SETFD ||
	    command == F_SETFL || command == F_SETOWN) {
		va_start(ap, command);
		argument = va_arg(ap, int);
		va_end(ap);
	} else if (command == F_GETOWN) {
		argument = (intptr_t)&owner_result;
	} else if (command == F_GETLK || command == F_SETLK ||
		   command == F_SETLKW || command == F_OFD_GETLK ||
		   command == F_OFD_SETLK || command == F_OFD_SETLKW) {
		va_start(ap, command);
		native = va_arg(ap, struct flock *);
		va_end(ap);

		/* Handles the native availability. */
		if (native == NULL) {
			errno = EFAULT;

			/* Reports operation failure. */
			return -1;
		}
		memset(&request, 0, sizeof(request));
		request.type = native->l_type;
		request.whence = native->l_whence;
		request.start = native->l_start;
		request.length = native->l_len;
		request.pid = native->l_pid;
		argument = (intptr_t)&request;
	}

	result = (int)call(ZEDBSD_SYS_fcntl, fd, command, argument, 0, 0, 0);

	/* Checks the operation result. */
	if (result == 0 &&
	    (command == F_GETLK || command == F_OFD_GETLK)) {
		native->l_type = request.type;
		native->l_whence = request.whence;
		native->l_start = (off_t)request.start;
		native->l_len = (off_t)request.length;
		native->l_pid = request.pid;
	}

	/* Checks the operation result. */
	if (result == 0 && command == F_GETOWN)
		return owner_result;

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the lockf operation.
 */
int
lockf(
	int fd,
	int command,
	off_t length)
{
	struct flock lock;
	int operation, result;

	memset(&lock, 0, sizeof(lock));
	lock.l_type = command == F_ULOCK ? F_UNLCK : F_WRLCK;
	lock.l_whence = SEEK_CUR;
	lock.l_len = length;

	/* Handles the command condition. */
	if (command == F_LOCK)
		operation = F_SETLKW;
	else if (command == F_ULOCK || command == F_TLOCK)
		operation = F_SETLK;
	else if (command == F_TEST)
		operation = F_GETLK;
	else {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	result = fcntl(fd, operation, &lock);

	/* Checks the operation result. */
	if (result == 0 && command == F_TEST && lock.l_type != F_UNLCK) {
		errno = EACCES;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the pipe2 operation.
 */
int
pipe2(
	int result[2],
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_pipe2, (uintptr_t)result, flags, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pipe operation.
 */
int
pipe(
	int result[2])
{
	int function_result;

	/* Obtains the pipe2 result. */
	function_result = pipe2(result, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the read operation.
 */
ssize_t
read(
	int fd,
	void *p,
	size_t n)
{
	ssize_t r;

	cancel_point();
	r = (ssize_t)call(ZEDBSD_SYS_read, fd, (uintptr_t)p, n, 0, 0, 0);
	cancel_point();

	/* Returns the computed result. */
	return r;
}

/*
 * Implements the write operation.
 */
ssize_t
write(
	int fd,
	const void *p,
	size_t n)
{
	ssize_t r;

	cancel_point();
	r = (ssize_t)call(ZEDBSD_SYS_write, fd, (uintptr_t)p, n, 0, 0, 0);
	cancel_point();

	/* Returns the computed result. */
	return r;
}

/*
 * Implements the pread operation.
 */
ssize_t
pread(
	int fd,
	void *p,
	size_t n,
	off_t offset)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_pread, fd, (uintptr_t)p, n, offset, 0,
			     0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pwrite operation.
 */
ssize_t
pwrite(
	int fd,
	const void *p,
	size_t n,
	off_t offset)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_pwrite, fd, (uintptr_t)p, n, offset, 0,
			     0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the readv operation.
 */
ssize_t
readv(
	int fd,
	const struct iovec *iov,
	int count)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_readv, fd, (uintptr_t)iov, count, 0, 0,
			     0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the writev operation.
 */
ssize_t
writev(
	int fd,
	const struct iovec *iov,
	int count)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_writev, fd, (uintptr_t)iov, count, 0, 0,
			     0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the creat operation.
 */
int
creat(
	const char *path,
	mode_t mode)
{
	int function_result;

	/* Obtains the open result. */
	function_result = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the preadv operation.
 */
ssize_t
preadv(
	int fd,
	const struct iovec *iov,
	int count,
	off_t offset)
{
	ssize_t result;

	cancel_point();
	result = positional_vector_io(fd, iov, count, offset, 0);
	cancel_point();

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the pwritev operation.
 */
ssize_t
pwritev(
	int fd,
	const struct iovec *iov,
	int count,
	off_t offset)
{
	ssize_t result;

	cancel_point();
	result = positional_vector_io(fd, iov, count, offset, 1);
	cancel_point();

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the fsync operation.
 */
int
fsync(
	int fd)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fsync, fd, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sync operation.
 */
void
sync(
	void)
{
	(void)call(ZEDBSD_SYS_sync, 0, 0, 0, 0, 0, 0);
}

/*
 * Implements the fdatasync operation.
 */
int
fdatasync(
	int fd)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fdatasync, fd, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the lseek operation.
 */
off_t
lseek(
	int fd,
	off_t off,
	int whence)
{
	off_t function_result;

	/* Computes the function result. */
	function_result = (off_t)call(ZEDBSD_SYS_lseek, fd, off, whence, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fstat operation.
 */
int
fstat(
	int fd,
	struct stat *st)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fstat, fd, (uintptr_t)st, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the chdir operation.
 */
int
chdir(
	const char *p)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_chdir, (uintptr_t)p, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fchdir operation.
 */
int
fchdir(
	int fd)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fchdir, fd, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getcwd operation.
 */
char *
getcwd(
	char *p,
	size_t n)
{
	char *function_result;

	/* Computes the function result. */
	function_result = (char *)call(ZEDBSD_SYS_getcwd, (uintptr_t)p, n, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ioctl operation.
 */
int
ioctl(
	int fd,
	unsigned long request,
	...)
{
	va_list ap;
	uintptr_t arg = 0;
	int function_result;

	/* Handles the ioctl has argument condition. */
	if (ioctl_has_argument(request)) {
		va_start(ap, request);
		arg = va_arg(ap, uintptr_t);
		va_end(ap);
	}

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_ioctl, fd, request, arg, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the posix devctl operation.
 */
int
posix_devctl(
	int descriptor,
	int command,
	void *restrict data,
	size_t size,
	int *restrict information)
{
	unsigned long request;
	size_t command_size;
	intptr_t result;

	request = (unsigned int)command;
	command_size = (request >> 16) & 0x1fffUL;

	/* Checks the current data size. */
	if (size > 0x1fffU || (command_size != 0 && size < command_size))
		return EINVAL;
	cancel_point();
	result = __syscall6(ZEDBSD_SYS_ioctl, (uintptr_t)descriptor, request,
			    (uintptr_t)data, 0, 0, 0);
	cancel_point();

	/* Checks the operation result. */
	if (result < 0 && result >= -4095)
		return result == -EOPNOTSUPP ? ENOTTY : (int)-result;

	/* Handles the information availability. */
	if (information != NULL)
		*information = (int)result;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sysctl operation.
 */
int
sysctl(
	const int *name,
	unsigned int namelen,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_sysctl, (uintptr_t)name, namelen,
			 (uintptr_t)oldp, (uintptr_t)oldlenp, (uintptr_t)newp,
			 newlen);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sysctlbyname operation.
 */
int
sysctlbyname(
	const char *name,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen)
{
	int function_result;
	int query[2] = {CTL_SYSCTL, CTL_SYSCTL_NAME2OID};
	int oid[CTL_MAXNAME];
	size_t oidlen;

	oidlen = sizeof(oid);

	/* Handles the name availability. */
	if (name == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed sysctl operation. */
	if (sysctl(query, 2, oid, &oidlen, name, strlen(name) + 1U) != 0)
		return -1;

	/* Obtains the sysctl result. */
	function_result = sysctl(oid, (unsigned)(oidlen / sizeof(oid[0])), oldp, oldlenp,
		      newp, newlen);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mmap operation.
 */
void *
mmap(
	void *address,
	size_t length,
	int prot,
	int flags,
	int fd,
	off_t offset)
{
	intptr_t value;

	value = call(ZEDBSD_SYS_mmap, (uintptr_t)address, length, prot,
			      flags, fd, offset);

	/* Returns the computed result. */
	return value == -1 ? MAP_FAILED : (void *)value;
}

/*
 * Implements the munmap operation.
 */
int
munmap(
	void *p,
	size_t n)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_munmap, (uintptr_t)p, n, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mprotect operation.
 */
int
mprotect(
	void *p,
	size_t n,
	int prot)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_mprotect, (uintptr_t)p, n, prot, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the msync operation.
 */
int
msync(
	void *p,
	size_t n,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_msync, (uintptr_t)p, n, flags, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the brk operation.
 */
int
brk(
	void *address)
{
	intptr_t value;

	/* Handles the address availability. */
	if (address == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	value = call(ZEDBSD_SYS_brk, (uintptr_t)address, 0, 0, 0, 0, 0);

	/* Validates the current value. */
	if (value == -1)
		return -1;
	process_break = (uintptr_t)address;
	process_break_known = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sbrk operation.
 */
void *
sbrk(
	intptr_t increment)
{
	uintptr_t old_break, new_break;
	uintptr_t decrease;
	intptr_t value;

	decrease = 0;

	/* Handles the process break known condition. */
	if (!process_break_known) {
		value = call(ZEDBSD_SYS_brk, 0, 0, 0, 0, 0, 0);

		/* Validates the current value. */
		if (value == -1)
			return (void *)-1;
		process_break = (uintptr_t)value;
		process_break_known = 1;
	}
	old_break = process_break;

	/* Handles the increment condition. */
	if (increment < 0)
		decrease = (uintptr_t)(-(increment + 1)) + 1U;

	/* Handles the increment condition. */
	if ((increment > 0 && (uintptr_t)increment > UINTPTR_MAX - old_break) ||
	    (increment < 0 && decrease > old_break)) {
		errno = ENOMEM;

		/* Returns the computed result. */
		return (void *)-1;
	}
	new_break = increment < 0 ? old_break - decrease
				  : old_break + (uintptr_t)increment;
	value = call(ZEDBSD_SYS_brk, new_break, 0, 0, 0, 0, 0);

	/* Validates the current value. */
	if (value == -1)
		return (void *)-1;
	process_break = new_break;

	/* Returns the computed result. */
	return (void *)old_break;
}

/*
 * Implements the sysconf operation.
 */
long
sysconf(
	int name)
{
	long function_result;
	struct rlimit limit;
	uint32_t cpus;
	size_t cpus_size;

	/* Dispatch the selected operation case. */
	switch (name) {
	case _SC_PAGE_SIZE:
		/* Returns the computed result. */
		return ZEDBSD_USER_PAGE_SIZE;
	case _SC_OPEN_MAX:

	/* Computes the function result. */
	function_result = getrlimit(RLIMIT_NOFILE, &limit) == 0
		   ? (long)limit.rlim_cur
		   : -1;

	/* Returns the computed result. */
	return function_result;
	case _SC_CLK_TCK:
		/* Returns the computed result. */
		return 100;
	case _SC_JOB_CONTROL:
		/* Returns the computed result. */
		return _POSIX_JOB_CONTROL;
	case _SC_THREADS:
		/* Returns the computed result. */
		return _POSIX_THREADS;
	case _SC_THREAD_PROCESS_SHARED:
		/* Returns the computed result. */
		return _POSIX_THREAD_PROCESS_SHARED;
	case _SC_REALTIME_SIGNALS:
		/* Returns the computed result. */
		return _POSIX_REALTIME_SIGNALS;
	case _SC_SHARED_MEMORY_OBJECTS:
		/* Returns the computed result. */
		return _POSIX_SHARED_MEMORY_OBJECTS;
	case _SC_SEMAPHORES:
		/* Returns the computed result. */
		return _POSIX_SEMAPHORES;
	case _SC_MESSAGE_PASSING:
		/* Returns the computed result. */
		return _POSIX_MESSAGE_PASSING;
	case _SC_DEVICE_CONTROL:
		/* Returns the computed result. */
		return _POSIX_DEVICE_CONTROL;
	case _SC_VERSION:
		/* Returns the computed result. */
		return _POSIX_VERSION;
	case _SC_2_VERSION:
		/* Returns the computed result. */
		return _POSIX2_VERSION;
	case _SC_ARG_MAX:
		/* Returns the computed result. */
		return ARG_MAX;
	case _SC_CHILD_MAX:
		/* Returns the computed result. */
		return 64;
	case _SC_STREAM_MAX:
		/* Returns the computed result. */
		return 32;
	case _SC_THREAD_KEYS_MAX:
		/* Returns the computed result. */
		return 32;
	case _SC_THREAD_DESTRUCTOR_ITERATIONS:
		/* Returns the computed result. */
		return 4;
	case _SC_THREAD_STACK_MIN:
		/* Returns the computed result. */
		return 65536;
	case _SC_THREAD_THREADS_MAX:
		/* Returns the computed result. */
		return 64;
	case _SC_SEM_NSEMS_MAX:
		/* Returns the computed result. */
		return 64;
	case _SC_SEM_VALUE_MAX:
		/* Returns the computed result. */
		return 0x7fffffffL;
	case _SC_MQ_OPEN_MAX:
		/* Returns the computed result. */
		return 16;
	case _SC_MQ_PRIO_MAX:
		/* Returns the computed result. */
		return 32;
	case _SC_TIMERS:
		/* Returns the computed result. */
		return _POSIX_TIMERS;
	case _SC_XOPEN_VERSION:
		/* Returns the computed result. */
		return _XOPEN_VERSION;
	case _SC_XOPEN_UNIX:
		/* Returns the computed result. */
		return _XOPEN_UNIX;
	case _SC_RTSIG_MAX:
		/* Returns the computed result. */
		return SIGRTMAX - SIGRTMIN + 1;
	case _SC_SIGQUEUE_MAX:
		/* Returns the computed result. */
		return SIGQUEUE_MAX;
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		cpus_size = sizeof(cpus);

		/* Handles a failed sysctlbyname operation. */
		if (sysctlbyname(name == _SC_NPROCESSORS_CONF ? "hw.ncpu"
							      : "hw.ncpuonline",
				 &cpus, &cpus_size, NULL, 0) == 0 &&
		    cpus_size == sizeof(cpus))

			/* Returns the computed result. */
			return (long)cpus;

		/* Reports operation failure. */
		return -1;
	default:
		break;
	}
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the pathconf operation.
 */
long
pathconf(
	const char *path,
	int name)
{
	long function_result;
	struct stat status;

	/* Handles the path availability. */
	if (path == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed stat operation. */
	if (stat(path, &status) != 0)
		return -1;

	/* Obtains the path limit result. */
	function_result = path_limit(name);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fpathconf operation.
 */
long
fpathconf(
	int descriptor,
	int name)
{
	long function_result;
	struct stat status;

	/* Handles a failed fstat operation. */
	if (fstat(descriptor, &status) != 0)
		return -1;

	/* Obtains the path limit result. */
	function_result = path_limit(name);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the confstr operation.
 */
size_t
confstr(
	int name,
	char *buffer,
	size_t size)
{
	size_t copied;
	static const char path[] = "/bin:/usr/bin";
	size_t needed;

	/* Validates the current name. */
	if (name != _CS_PATH) {
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}
	needed = sizeof(path);

	/* Handles the buffer availability. */
	if (buffer != NULL && size != 0) {
				copied = needed < size ? needed : size;
		memcpy(buffer, path, copied);
		buffer[copied - 1U] = '\0';
	}

	/* Returns the computed result. */
	return needed;
}

/*
 * Implements the mkdir operation.
 */
int
mkdir(
	const char *path,
	mode_t mode)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_mkdir, (uintptr_t)path, mode, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mkdirat operation.
 */
int
mkdirat(
	int dirfd,
	const char *path,
	mode_t mode)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_mkdirat, dirfd, (uintptr_t)path, mode, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mkfifoat operation.
 */
int
mkfifoat(
	int dirfd,
	const char *path,
	mode_t mode)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_mknodat, dirfd, (uintptr_t)path,
			 S_IFIFO | (mode & 07777U), 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mkfifo operation.
 */
int
mkfifo(
	const char *path,
	mode_t mode)
{
	int function_result;

	/* Obtains the mkfifoat result. */
	function_result = mkfifoat(AT_FDCWD, path, mode);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mknodat operation.
 */
int
mknodat(
	int dirfd,
	const char *path,
	mode_t mode,
	dev_t device)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_mknodat, dirfd, (uintptr_t)path, mode,
			 device, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mknod operation.
 */
int
mknod(
	const char *path,
	mode_t mode,
	dev_t device)
{
	int function_result;

	/* Obtains the mknodat result. */
	function_result = mknodat(AT_FDCWD, path, mode, device);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getpriority operation.
 */
int
getpriority(
	int which,
	id_t who)
{
	int value;

	/* Handles a failed call operation. */
	if (call(ZEDBSD_SYS_getpriority, which, who, (uintptr_t)&value, 0, 0,
		 0) < 0)

		/* Reports operation failure. */
		return -1;

	/* Returns the computed result. */
	return value;
}

/*
 * Implements the setpriority operation.
 */
int
setpriority(
	int which,
	id_t who,
	int value)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setpriority, which, who, value, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the nice operation.
 */
int
nice(
	int increment)
{
	int function_result;
	int value;

	errno = 0;
	value = getpriority(PRIO_PROCESS, 0);

	/* Handles the reported system error. */
	if (value == -1 && errno != 0)
		return -1;

	/* Handles the increment condition. */
	if (increment > 0 && value > 20 - increment)
		value = 20;
	else if (increment < 0 && value < -20 - increment)
		value = -20;
	else
		value += increment;

	/* Handles a failed setpriority operation. */
	if (setpriority(PRIO_PROCESS, 0, value) != 0)
		return -1;

	/* Obtains the getpriority result. */
	function_result = getpriority(PRIO_PROCESS, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getrusage operation.
 */
int
getrusage(
	int who,
	struct rusage *usage)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_getrusage, who, (uintptr_t)usage, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getitimer operation.
 */
int
getitimer(
	int which,
	struct itimerval *value)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_getitimer, which, (uintptr_t)value, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setitimer operation.
 */
int
setitimer(
	int which,
	const struct itimerval *value,
	struct itimerval *old)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setitimer, which, (uintptr_t)value,
			 (uintptr_t)old, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the unlink operation.
 */
int
unlink(
	const char *path)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_unlink, (uintptr_t)path, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the unlinkat operation.
 */
int
unlinkat(
	int dirfd,
	const char *path,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_unlinkat, dirfd, (uintptr_t)path, flags, 0,
			 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the rmdir operation.
 */
int
rmdir(
	const char *path)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_rmdir, (uintptr_t)path, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the rename operation.
 */
int
rename(
	const char *oldpath,
	const char *newpath)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_rename, (uintptr_t)oldpath,
			 (uintptr_t)newpath, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the renameat operation.
 */
int
renameat(
	int olddirfd,
	const char *oldpath,
	int newdirfd,
	const char *newpath)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_renameat, olddirfd, (uintptr_t)oldpath,
			 newdirfd, (uintptr_t)newpath, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the linkat operation.
 */
int
linkat(
	int olddirfd,
	const char *oldpath,
	int newdirfd,
	const char *newpath,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_linkat, olddirfd, (uintptr_t)oldpath,
			 newdirfd, (uintptr_t)newpath, flags, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the link operation.
 */
int
link(
	const char *oldpath,
	const char *newpath)
{
	int function_result;

	/* Obtains the linkat result. */
	function_result = linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the symlinkat operation.
 */
int
symlinkat(
	const char *target,
	int dirfd,
	const char *path)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_symlinkat, (uintptr_t)target, dirfd,
			 (uintptr_t)path, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the symlink operation.
 */
int
symlink(
	const char *target,
	const char *path)
{
	int function_result;

	/* Obtains the symlinkat result. */
	function_result = symlinkat(target, AT_FDCWD, path);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the readlinkat operation.
 */
ssize_t
readlinkat(
	int dirfd,
	const char *path,
	char *buffer,
	size_t size)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_readlinkat, dirfd, (uintptr_t)path,
			     (uintptr_t)buffer, size, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the readlink operation.
 */
ssize_t
readlink(
	const char *path,
	char *buffer,
	size_t size)
{
	ssize_t function_result;

	/* Obtains the readlinkat result. */
	function_result = readlinkat(AT_FDCWD, path, buffer, size);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the truncate operation.
 */
int
truncate(
	const char *path,
	off_t length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_truncate, (uintptr_t)path, length, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ftruncate operation.
 */
int
ftruncate(
	int fd,
	off_t length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_ftruncate, fd, length, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the umask operation.
 */
mode_t
umask(
	mode_t mask)
{
	mode_t function_result;

	/* Computes the function result. */
	function_result = (mode_t)call(ZEDBSD_SYS_umask, mask, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the clock gettime operation.
 */
int
clock_gettime(
	clockid_t id,
	struct timespec *ts)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_clock_gettime, id, (uintptr_t)ts, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the clock getres operation.
 */
int
clock_getres(
	clockid_t id,
	struct timespec *ts)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_clock_getres, id, (uintptr_t)ts, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the clock settime operation.
 */
int
clock_settime(
	clockid_t id,
	const struct timespec *ts)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_clock_settime, id, (uintptr_t)ts, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the aio read operation.
 */
int
aio_read(
	struct aiocb *control)
{
	int function_result;

	/* Obtains the aio submit result. */
	function_result = aio_submit(control, 0, 1);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the aio write operation.
 */
int
aio_write(
	struct aiocb *control)
{
	int function_result;

	/* Obtains the aio submit result. */
	function_result = aio_submit(control, 1, 1);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the aio error operation.
 */
int
aio_error(
	const struct aiocb *control)
{
	/* Returns the computed result. */
	return control != NULL && control->__aio_submitted
		   ? control->__aio_error
		   : EINVAL;
}

/*
 * Implements the aio return operation.
 */
ssize_t
aio_return(
	struct aiocb *control)
{
	/* Handles the control availability. */
	if (control == NULL || !control->__aio_submitted ||
	    control->__aio_returned) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	control->__aio_returned = 1;

	/* Handles an operation failure. */
	if (control->__aio_error != 0) {
		errno = control->__aio_error;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return control->__aio_result;
}

/*
 * Implements the aio cancel operation.
 */
int
aio_cancel(
	int descriptor,
	struct aiocb *control)
{
	/* Handles the control availability. */
	if (control != NULL && control->aio_fildes != descriptor) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return AIO_ALLDONE;
}

/*
 * Implements the aio suspend operation.
 */
int
aio_suspend(
	const struct aiocb *const controls[],
	int count,
	const struct timespec *timeout)
{
	int index;

	/* Handles the controls availability. */
	if (controls == NULL || count < 0 ||
	    (timeout != NULL && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
				 timeout->tv_nsec >= 1000000000L))) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = 0; index < count; index++)

		/* Handles the controls condition. */
		if (controls[index] != NULL && controls[index]->__aio_submitted)
			return 0;

	/* Handles a failed nanosleep operation. */
	if (timeout != NULL && nanosleep(timeout, NULL) != 0)
		return -1;
	errno = EAGAIN;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the aio fsync operation.
 */
int
aio_fsync(
	int operation,
	struct aiocb *control)
{
	int result;

	/* Handles the control availability. */
	if (control == NULL || (operation != O_SYNC && operation != O_DSYNC)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	control->__aio_submitted = 1;
	control->__aio_returned = 0;
	result = operation == O_DSYNC ? fdatasync(control->aio_fildes)
				      : fsync(control->aio_fildes);
	control->__aio_result = result;
	control->__aio_error = result < 0 ? errno : 0;
	aio_notify(&control->aio_sigevent);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the lio listio operation.
 */
int
lio_listio(
	int mode,
	struct aiocb *restrict const controls[restrict],
	int count,
	struct sigevent *restrict event)
{
	struct aiocb *control;
	int index, failed;

	failed = 0;

	/* Handles the controls availability. */
	if ((mode != LIO_WAIT && mode != LIO_NOWAIT) || count < 0 ||
	    (count != 0 && controls == NULL)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
				control = controls[index];

		/* Handles the control availability. */
		if (control == NULL || control->aio_lio_opcode == LIO_NOP)
			continue;

		/* Handles the control condition. */
		if (control->aio_lio_opcode == LIO_READ) {
			/* Handles a failed aio submit operation. */
			if (aio_submit(control, 0, 0) != 0)
				failed = 1;
		} else if (control->aio_lio_opcode == LIO_WRITE) {
			/* Handles a failed aio submit operation. */
			if (aio_submit(control, 1, 0) != 0)
				failed = 1;
		} else {
			errno = EINVAL;
			failed = 1;
		}
	}

	/* Handles the event availability. */
	if (event != NULL)
		aio_notify(event);

	/* Returns the computed result. */
	return failed ? -1 : 0;
}

/*
 * Implements the mount operation.
 */
int
mount(
	const char *type,
	const char *dir,
	int flags,
	void *data)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_mount, (uintptr_t)type, (uintptr_t)dir,
			 flags, (uintptr_t)data, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the unmount operation.
 */
int
unmount(
	const char *dir,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_unmount, (uintptr_t)dir, flags, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the statvfs operation.
 */
int
statvfs(
	const char *path,
	struct statvfs *status)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_statvfs, (uintptr_t)path, (uintptr_t)status,
			 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fstatvfs operation.
 */
int
fstatvfs(
	int fd,
	struct statvfs *status)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fstatvfs, fd, (uintptr_t)status, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the quotactl operation.
 */
int
quotactl(
	const char *path,
	struct quota_control *request)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_quotactl, (uintptr_t)path,
			 (uintptr_t)request, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the snapshotctl operation.
 */
int
snapshotctl(
	const char *path,
	struct snapshot_control *request)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_snapshotctl, (uintptr_t)path,
			 (uintptr_t)request, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getxattr operation.
 */
ssize_t
getxattr(
	const char *path,
	const char *name,
	void *value,
	size_t size)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_getxattr, (uintptr_t)path,
			     (uintptr_t)name, (uintptr_t)value, size, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the lgetxattr operation.
 */
ssize_t
lgetxattr(
	const char *path,
	const char *name,
	void *value,
	size_t size)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_lgetxattr, (uintptr_t)path,
			     (uintptr_t)name, (uintptr_t)value, size, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fgetxattr operation.
 */
ssize_t
fgetxattr(
	int fd,
	const char *name,
	void *value,
	size_t size)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_fgetxattr, fd, (uintptr_t)name,
			     (uintptr_t)value, size, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setxattr operation.
 */
int
setxattr(
	const char *path,
	const char *name,
	const void *value,
	size_t size,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setxattr, (uintptr_t)path, (uintptr_t)name,
			 (uintptr_t)value, size, flags, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the lsetxattr operation.
 */
int
lsetxattr(
	const char *path,
	const char *name,
	const void *value,
	size_t size,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_lsetxattr, (uintptr_t)path, (uintptr_t)name,
			 (uintptr_t)value, size, flags, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fsetxattr operation.
 */
int
fsetxattr(
	int fd,
	const char *name,
	const void *value,
	size_t size,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fsetxattr, fd, (uintptr_t)name,
			 (uintptr_t)value, size, flags, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the listxattr operation.
 */
ssize_t
listxattr(
	const char *path,
	char *list,
	size_t size)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_listxattr, (uintptr_t)path,
			     (uintptr_t)list, size, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the llistxattr operation.
 */
ssize_t
llistxattr(
	const char *path,
	char *list,
	size_t size)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_llistxattr, (uintptr_t)path,
			     (uintptr_t)list, size, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the flistxattr operation.
 */
ssize_t
flistxattr(
	int fd,
	char *list,
	size_t size)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)call(ZEDBSD_SYS_flistxattr, fd, (uintptr_t)list, size,
			     0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the removexattr operation.
 */
int
removexattr(
	const char *path,
	const char *name)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_removexattr, (uintptr_t)path,
			 (uintptr_t)name, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the lremovexattr operation.
 */
int
lremovexattr(
	const char *path,
	const char *name)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_lremovexattr, (uintptr_t)path,
			 (uintptr_t)name, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fremovexattr operation.
 */
int
fremovexattr(
	int fd,
	const char *name)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fremovexattr, fd, (uintptr_t)name, 0, 0, 0,
			 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the nanosleep operation.
 */
int
nanosleep(
	const struct timespec *request,
	struct timespec *remain)
{
	int result;

	cancel_point();
	result = (int)call(ZEDBSD_SYS_nanosleep, (uintptr_t)request,
			   (uintptr_t)remain, 0, 0, 0, 0);
	cancel_point();

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the waitpid operation.
 */
pid_t
waitpid(
	pid_t pid,
	int *status,
	int options)
{
	pid_t result;

	cancel_point();
	result = (pid_t)call(ZEDBSD_SYS_waitpid, (uintptr_t)pid,
			     (uintptr_t)status, (uintptr_t)options, 0, 0, 0);
	cancel_point();

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the clock nanosleep operation.
 */
int
clock_nanosleep(
	clockid_t clock_id,
	int flags,
	const struct timespec *request,
	struct timespec *remain)
{
	struct timespec delay, now;
	int saved_errno;
	int result;

	saved_errno = errno;

	/* Handles the request availability. */
	if (request == NULL || request->tv_sec < 0 || request->tv_nsec < 0 ||
	    request->tv_nsec >= 1000000000L ||
	    (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME) ||
	    (flags & ~TIMER_ABSTIME) != 0)

		/* Returns the computed result. */
		return EINVAL;

	/* Checks the active flags. */
	if ((flags & TIMER_ABSTIME) == 0) {
		result = nanosleep(request, remain);

		/* Checks the operation result. */
		if (result == 0) {
			errno = saved_errno;

			/* Reports successful completion. */
			return 0;
		}
		result = errno;
		errno = saved_errno;

		/* Returns the computed result. */
		return result;
	}

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(clock_id, &now) != 0) {
		result = errno;
		errno = saved_errno;

		/* Returns the computed result. */
		return result;
	}
	delay.tv_sec = request->tv_sec - now.tv_sec;
	delay.tv_nsec = request->tv_nsec - now.tv_nsec;

	/* Handles the delay condition. */
	if (delay.tv_nsec < 0) {
		delay.tv_nsec += 1000000000L;
		delay.tv_sec--;
	}

	/* Handles the delay condition. */
	if (delay.tv_sec < 0) {
		errno = saved_errno;

		/* Reports successful completion. */
		return 0;
	}
	result = nanosleep(&delay, NULL);

	/* Checks the operation result. */
	if (result == 0) {
		errno = saved_errno;

		/* Reports successful completion. */
		return 0;
	}
	result = errno;
	errno = saved_errno;

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the sleep operation.
 */
unsigned
sleep(
	unsigned seconds)
{
	struct timespec request = {(time_t)seconds, 0}, remain = {0, 0};

	/* Handles a failed nanosleep operation. */
	if (nanosleep(&request, &remain) == 0)
		return 0;

	/* Returns the computed result. */
	return (unsigned)remain.tv_sec + (remain.tv_nsec != 0);
}

/*
 * Implements the alarm operation.
 */
unsigned
alarm(
	unsigned seconds)
{
	static timer_t alarm_timer;
	static pid_t alarm_owner;
	static int alarm_created;
	struct sigevent event;
	struct itimerspec value, old_value;
	pid_t owner;

	owner = getpid();

	/* Handles the alarm created condition. */
	if (!alarm_created || alarm_owner != owner) {
		memset(&event, 0, sizeof(event));
		event.sigev_notify = SIGEV_SIGNAL;
		event.sigev_signo = SIGALRM;

		/* Handles a failed timer create operation. */
		if (timer_create(CLOCK_MONOTONIC, &event, &alarm_timer) != 0)
			return 0;
		alarm_created = 1;
		alarm_owner = owner;
	}
	memset(&value, 0, sizeof(value));
	value.it_value.tv_sec = (time_t)seconds;

	/* Handles a failed timer settime operation. */
	if (timer_settime(alarm_timer, 0, &value, &old_value) != 0)
		return 0;

	/* Returns the computed result. */
	return (unsigned)old_value.it_value.tv_sec +
	       (old_value.it_value.tv_nsec != 0);
}

/*
 * Implements the usleep operation.
 */
int
usleep(
	useconds_t microseconds)
{
	int function_result;
	struct timespec request;

	/* Handles the microseconds condition. */
	if (microseconds >= 1000000U) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	request.tv_sec = 0;
	request.tv_nsec = (long)microseconds * 1000L;

	/* Obtains the nanosleep result. */
	function_result = nanosleep(&request, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pause operation.
 */
int
pause(
	void)
{
	int function_result;
	sigset_t mask;

	/* Handles a failed sigprocmask operation. */
	if (sigprocmask(SIG_SETMASK, NULL, &mask) != 0)
		return -1;

	/* Obtains the sigsuspend result. */
	function_result = sigsuspend(&mask);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the wait operation.
 */
pid_t
wait(
	int *status)
{
	pid_t function_result;

	/* Obtains the waitpid result. */
	function_result = waitpid(-1, status, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the waitid operation.
 */
int
waitid(
	idtype_t type,
	id_t id,
	siginfo_t *information,
	int options)
{
	int result;

	cancel_point();
	result = (int)call(ZEDBSD_SYS_waitid, type, id, (uintptr_t)information,
			   options, 0, 0);
	cancel_point();

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the getrlimit operation.
 */
int
getrlimit(
	int resource,
	struct rlimit *limit)
{
	struct rlimit_record wire;
	int result;

	/* Handles the limit availability. */
	if (limit == NULL) {
		errno = EFAULT;

		/* Reports operation failure. */
		return -1;
	}
	result = (int)call(ZEDBSD_SYS_getrlimit, resource, (uintptr_t)&wire, 0,
			   0, 0, 0);

	/* Checks the operation result. */
	if (result == 0) {
		limit->rlim_cur = wire.current;
		limit->rlim_max = wire.maximum;
	}

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the setrlimit operation.
 */
int
setrlimit(
	int resource,
	const struct rlimit *limit)
{
	int function_result;
	struct rlimit_record wire;

	/* Handles the limit availability. */
	if (limit == NULL) {
		errno = EFAULT;

		/* Reports operation failure. */
		return -1;
	}
	wire.current = limit->rlim_cur;
	wire.maximum = limit->rlim_max;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setrlimit, resource, (uintptr_t)&wire, 0, 0,
			 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the posix spawn file actions init operation.
 */
int
posix_spawn_file_actions_init(
	posix_spawn_file_actions_t *actions)
{
	/* Handles the actions availability. */
	if (actions == NULL)
		return EINVAL;
	memset(actions, 0, sizeof(*actions));

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawn file actions destroy operation.
 */
int
posix_spawn_file_actions_destroy(
	posix_spawn_file_actions_t *actions)
{
	/* Handles the actions availability. */
	if (actions == NULL)
		return EINVAL;
	memset(actions, 0, sizeof(*actions));

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawn file actions addclose operation.
 */
int
posix_spawn_file_actions_addclose(
	posix_spawn_file_actions_t *actions,
	int fd)
{
	struct __spawn_action *a;

	/* Checks the file descriptor. */
	if (fd < 0)
		return EBADF;
	a = spawn_action_add(actions);

	/* Handles the a availability. */
	if (a == NULL)
		return EINVAL;
	memset(a, 0, sizeof(*a));
	a->operation = SPAWN_ACTION_CLOSE;
	a->descriptor = fd;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawn file actions adddup2 operation.
 */
int
posix_spawn_file_actions_adddup2(
	posix_spawn_file_actions_t *actions,
	int fd,
	int newfd)
{
	struct __spawn_action *a;

	/* Checks the file descriptor. */
	if (fd < 0 || newfd < 0)
		return EBADF;
	a = spawn_action_add(actions);

	/* Handles the a availability. */
	if (a == NULL)
		return EINVAL;
	memset(a, 0, sizeof(*a));
	a->operation = SPAWN_ACTION_DUP2;
	a->descriptor = fd;
	a->new_descriptor = newfd;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawn file actions addopen operation.
 */
int
posix_spawn_file_actions_addopen(
	posix_spawn_file_actions_t *actions,
	int fd,
	const char *path,
	int flags,
	mode_t mode)
{
	struct __spawn_action *a;
	size_t n;

	/* Handles the path availability. */
	if (fd < 0 || path == NULL)
		return EINVAL;
	n = strlen(path);

	/* Checks the current item count. */
	if (n >= ZEDBSD_SPAWN_PATH_MAX)
		return ENAMETOOLONG;
	a = spawn_action_add(actions);

	/* Handles the a availability. */
	if (a == NULL)
		return EINVAL;
	memset(a, 0, sizeof(*a));
	a->operation = SPAWN_ACTION_OPEN;
	a->descriptor = fd;
	a->flags = flags;
	a->mode = mode;
	memcpy(a->path, path, n + 1U);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawn file actions addchdir operation.
 */
int
posix_spawn_file_actions_addchdir(
	posix_spawn_file_actions_t *actions,
	const char *path)
{
	struct __spawn_action *action;
	size_t length;

	/* Handles the actions availability. */
	if (actions == NULL || path == NULL)
		return EINVAL;
	length = strlen(path);

	/* Checks the current data length. */
	if (length >= ZEDBSD_SPAWN_PATH_MAX)
		return ENAMETOOLONG;
	action = spawn_action_add(actions);

	/* Handles the action availability. */
	if (action == NULL)
		return EINVAL;
	memset(action, 0, sizeof(*action));
	action->operation = SPAWN_ACTION_CHDIR;
	memcpy(action->path, path, length + 1U);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawn file actions addfchdir operation.
 */
int
posix_spawn_file_actions_addfchdir(
	posix_spawn_file_actions_t *actions,
	int descriptor)
{
	struct __spawn_action *action;

	/* Handles the actions availability. */
	if (actions == NULL)
		return EINVAL;

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return EBADF;
	action = spawn_action_add(actions);

	/* Handles the action availability. */
	if (action == NULL)
		return EINVAL;
	memset(action, 0, sizeof(*action));
	action->operation = SPAWN_ACTION_FCHDIR;
	action->descriptor = descriptor;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr init operation.
 */
int
posix_spawnattr_init(
	posix_spawnattr_t *attr)
{
	/* Handles the attr availability. */
	if (attr == NULL)
		return EINVAL;
	memset(attr, 0, sizeof(*attr));

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr destroy operation.
 */
int
posix_spawnattr_destroy(
	posix_spawnattr_t *attr)
{
	/* Handles the attr availability. */
	if (attr == NULL)
		return EINVAL;
	memset(attr, 0, sizeof(*attr));

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr getflags operation.
 */
int
posix_spawnattr_getflags(
	const posix_spawnattr_t *attr,
	short *flags)
{
	/* Handles the attr availability. */
	if (attr == NULL || flags == NULL)
		return EINVAL;
	*flags = attr->flags;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr setflags operation.
 */
int
posix_spawnattr_setflags(
	posix_spawnattr_t *attr,
	short flags)
{
	/* Handles the attr availability. */
	if (attr == NULL ||
	    (flags & ~(POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP |
		       POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK |
		       POSIX_SPAWN_SETSID)) != 0)

		/* Returns the computed result. */
		return EINVAL;
	attr->flags = flags;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr getpgroup operation.
 */
int
posix_spawnattr_getpgroup(
	const posix_spawnattr_t *attr,
	pid_t *pgroup)
{
	/* Handles the attr availability. */
	if (attr == NULL || pgroup == NULL)
		return EINVAL;
	*pgroup = attr->pgroup;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr setpgroup operation.
 */
int
posix_spawnattr_setpgroup(
	posix_spawnattr_t *attr,
	pid_t pgroup)
{
	/* Handles the attr availability. */
	if (attr == NULL || pgroup < 0)
		return EINVAL;
	attr->pgroup = pgroup;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr getsigmask operation.
 */
int
posix_spawnattr_getsigmask(
	const posix_spawnattr_t *attr,
	sigset_t *set)
{
	/* Handles the attr availability. */
	if (attr == NULL || set == NULL)
		return EINVAL;
	*set = attr->sigmask;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr setsigmask operation.
 */
int
posix_spawnattr_setsigmask(
	posix_spawnattr_t *attr,
	const sigset_t *set)
{
	sigset_t public_mask;

	public_mask = ((sigset_t)1ULL << SIGRTMAX) - 1U;

	/* Handles the attr availability. */
	if (attr == NULL || set == NULL)
		return EINVAL;
	attr->sigmask = *set & public_mask;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr getsigdefault operation.
 */
int
posix_spawnattr_getsigdefault(
	const posix_spawnattr_t *attr,
	sigset_t *set)
{
	/* Handles the attr availability. */
	if (attr == NULL || set == NULL)
		return EINVAL;
	*set = attr->sigdefault;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawnattr setsigdefault operation.
 */
int
posix_spawnattr_setsigdefault(
	posix_spawnattr_t *attr,
	const sigset_t *set)
{
	sigset_t public_mask;

	public_mask = ((sigset_t)1ULL << SIGRTMAX) - 1U;

	/* Handles the attr availability. */
	if (attr == NULL || set == NULL)
		return EINVAL;
	attr->sigdefault = *set & public_mask;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the posix spawn operation.
 */
int
posix_spawn(
	pid_t *result,
	const char *path,
	const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attr,
	char *const argv[],
	char *const envp[])
{
	int function_result;

	/* Obtains the posix spawn common result. */
	function_result = posix_spawn_common(result, path, actions, attr, argv, envp, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the posix spawnp operation.
 */
int
posix_spawnp(
	pid_t *result,
	const char *file,
	const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attr,
	char *const argv[],
	char *const envp[])
{
	int function_result;

	/* Obtains the posix spawn common result. */
	function_result = posix_spawn_common(result, file, actions, attr, argv, envp, 1);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fork operation.
 */
pid_t
fork(
	void)
{
	pid_t result;

	/* Handles the pthread fork prepare availability. */
	if (__pthread_fork_prepare != NULL)
		__pthread_fork_prepare();
	result = (pid_t)call(ZEDBSD_SYS_fork, 0, 0, 0, 0, 0, 0);

	/* Checks the operation result. */
	if (result == 0) {
		/*
 * Kernel timers are not inherited.  Reset libc's SIGEV_THREAD
		 * generation and reserved signal before user atfork handlers
		 * run. */
		if (__timer_sigev_thread_fork_child != NULL)
			__timer_sigev_thread_fork_child();

		/* Handles the pthread fork child availability. */
		if (__pthread_fork_child != NULL)
			__pthread_fork_child();
	} else if (__pthread_fork_parent != NULL) {
		__pthread_fork_parent();
	}

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the Fork operation.
 */
pid_t
_Fork(
	void)
{
	pid_t function_result;

	/*
	 * The kernel primitive already has fork semantics.  In contrast to
	 * fork(), this async-signal-safe entry deliberately runs neither user
	 * pthread_atfork handlers nor libc's pthread child recovery hooks.
	 */
	/* Computes the function result. */
	function_result = (pid_t)call(ZEDBSD_SYS_fork, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the execve operation.
 */
int
execve(
	const char *path,
	char *const argv[],
	char *const envp[])
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_execve, (uintptr_t)path, (uintptr_t)argv,
			 (uintptr_t)envp, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the execv operation.
 */
int
execv(
	const char *path,
	char *const argv[])
{
	int function_result;

	/* Obtains the execve result. */
	function_result = execve(path, argv, environ);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the execvp operation.
 */
int
execvp(
	const char *file,
	char *const argv[])
{
	int function_result;

	/* Obtains the exec search result. */
	function_result = exec_search(file, argv, environ);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fexecve operation.
 */
int
fexecve(
	int descriptor,
	char *const argv[],
	char *const envp[])
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fexecve, descriptor, (uintptr_t)argv,
			 (uintptr_t)envp, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the execl operation.
 */
int
execl(
	const char *path,
	const char *first,
	...)
{
	va_list arguments;
	int result;
	va_start(arguments, first);
	result = exec_varargs(path, first, arguments, 0, 0);
	va_end(arguments);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the execle operation.
 */
int
execle(
	const char *path,
	const char *first,
	...)
{
	va_list arguments;
	int result;
	va_start(arguments, first);
	result = exec_varargs(path, first, arguments, 0, 1);
	va_end(arguments);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the execlp operation.
 */
int
execlp(
	const char *file,
	const char *first,
	...)
{
	va_list arguments;
	int result;
	va_start(arguments, first);
	result = exec_varargs(file, first, arguments, 1, 0);
	va_end(arguments);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the getpid operation.
 */
pid_t
getpid(
	void)
{
	pid_t function_result;

	/* Computes the function result. */
	function_result = (pid_t)call(ZEDBSD_SYS_getpid, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sched yield operation.
 */
int
sched_yield(
	void)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_sched_yield, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getppid operation.
 */
pid_t
getppid(
	void)
{
	pid_t function_result;

	/* Computes the function result. */
	function_result = (pid_t)call(ZEDBSD_SYS_getppid, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getpgrp operation.
 */
pid_t
getpgrp(
	void)
{
	pid_t function_result;

	/* Computes the function result. */
	function_result = (pid_t)call(ZEDBSD_SYS_getpgrp, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getpgid operation.
 */
pid_t
getpgid(
	pid_t pid)
{
	pid_t function_result;

	/* Computes the function result. */
	function_result = (pid_t)call(ZEDBSD_SYS_getpgid, pid, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setpgid operation.
 */
int
setpgid(
	pid_t pid,
	pid_t pgid)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setpgid, pid, pgid, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setsid operation.
 */
pid_t
setsid(
	void)
{
	pid_t function_result;

	/* Computes the function result. */
	function_result = (pid_t)call(ZEDBSD_SYS_setsid, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getsid operation.
 */
pid_t
getsid(
	pid_t pid)
{
	pid_t function_result;

	/* Computes the function result. */
	function_result = (pid_t)call(ZEDBSD_SYS_getsid, pid, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getuid operation.
 */
uid_t
getuid(
	void)
{
	uid_t function_result;

	/* Computes the function result. */
	function_result = (uid_t)call(ZEDBSD_SYS_getuid, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getresuid operation.
 */
int
getresuid(
	uid_t *real,
	uid_t *effective,
	uid_t *saved)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_getresuid, (uintptr_t)real,
			 (uintptr_t)effective, (uintptr_t)saved, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the geteuid operation.
 */
uid_t
geteuid(
	void)
{
	uid_t function_result;

	/* Computes the function result. */
	function_result = (uid_t)call(ZEDBSD_SYS_geteuid, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getgid operation.
 */
gid_t
getgid(
	void)
{
	gid_t function_result;

	/* Computes the function result. */
	function_result = (gid_t)call(ZEDBSD_SYS_getgid, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getresgid operation.
 */
int
getresgid(
	gid_t *real,
	gid_t *effective,
	gid_t *saved)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_getresgid, (uintptr_t)real,
			 (uintptr_t)effective, (uintptr_t)saved, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getegid operation.
 */
gid_t
getegid(
	void)
{
	gid_t function_result;

	/* Computes the function result. */
	function_result = (gid_t)call(ZEDBSD_SYS_getegid, 0, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getgroups operation.
 */
int
getgroups(
	int count,
	gid_t groups[])
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_getgroups, count, (uintptr_t)groups, 0, 0,
			 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setuid operation.
 */
int
setuid(
	uid_t id)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setuid, id, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the seteuid operation.
 */
int
seteuid(
	uid_t id)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_seteuid, id, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setgid operation.
 */
int
setgid(
	gid_t id)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setgid, id, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setegid operation.
 */
int
setegid(
	gid_t id)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setegid, id, 0, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setgroups operation.
 */
int
setgroups(
	size_t count,
	const gid_t groups[])
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setgroups, count, (uintptr_t)groups, 0, 0,
			 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setreuid operation.
 */
int
setreuid(
	uid_t real,
	uid_t effective)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setreuid, real, effective, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setresuid operation.
 */
int
setresuid(
	uid_t real,
	uid_t effective,
	uid_t saved)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setresuid, real, effective, saved, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setregid operation.
 */
int
setregid(
	gid_t real,
	gid_t effective)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setregid, real, effective, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setresgid operation.
 */
int
setresgid(
	gid_t real,
	gid_t effective,
	gid_t saved)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_setresgid, real, effective, saved, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the stat operation.
 */
int
stat(
	const char *path,
	struct stat *status)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_stat, (uintptr_t)path, (uintptr_t)status, 0,
			 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the lstat operation.
 */
int
lstat(
	const char *path,
	struct stat *status)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_lstat, (uintptr_t)path, (uintptr_t)status,
			 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fstatat operation.
 */
int
fstatat(
	int dirfd,
	const char *path,
	struct stat *status,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fstatat, dirfd, (uintptr_t)path,
			 (uintptr_t)status, flags, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the access operation.
 */
int
access(
	const char *path,
	int mode)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_access, (uintptr_t)path, mode, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the faccessat operation.
 */
int
faccessat(
	int dirfd,
	const char *path,
	int mode,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_faccessat, dirfd, (uintptr_t)path, mode,
			 flags, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the chmod operation.
 */
int
chmod(
	const char *path,
	mode_t mode)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_chmod, (uintptr_t)path, mode, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fchmod operation.
 */
int
fchmod(
	int fd,
	mode_t mode)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fchmod, fd, mode, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fchmodat operation.
 */
int
fchmodat(
	int dirfd,
	const char *path,
	mode_t mode,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fchmodat, dirfd, (uintptr_t)path, mode,
			 flags, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the chown operation.
 */
int
chown(
	const char *path,
	uid_t uid,
	gid_t gid)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_chown, (uintptr_t)path, uid, gid, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fchown operation.
 */
int
fchown(
	int fd,
	uid_t uid,
	gid_t gid)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fchown, fd, uid, gid, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the lchown operation.
 */
int
lchown(
	const char *path,
	uid_t uid,
	gid_t gid)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_lchown, (uintptr_t)path, uid, gid, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the fchownat operation.
 */
int
fchownat(
	int dirfd,
	const char *path,
	uid_t uid,
	gid_t gid,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_fchownat, dirfd, (uintptr_t)path, uid, gid,
			 flags, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the utimensat operation.
 */
int
utimensat(
	int dirfd,
	const char *path,
	const struct timespec times[2],
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_utimensat, dirfd, (uintptr_t)path,
			 (uintptr_t)times, flags, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the futimens operation.
 */
int
futimens(
	int fd,
	const struct timespec times[2])
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_futimens, fd, (uintptr_t)times, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the isatty operation.
 */
int
isatty(
	int fd)
{
	/* Handles a failed ioctl operation. */
	if (ioctl(fd, ZEDBSD_CONSOLE_ISATTY) == 0)
		return 1;

	/* Handles the reported system error. */
	if (errno != EBADF)
		errno = ENOTTY;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the getlogin operation.
 */
char *
getlogin(
	void)
{
	static char login[] = "root";

	/* Returns the computed result. */
	return login;
}

/*
 * Implements the getlogin r operation.
 */
int
getlogin_r(
	char *buffer,
	size_t size)
{
	static const char login[] = "root";

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return EINVAL;

	/* Checks the current data size. */
	if (size < sizeof(login))
		return ERANGE;
	memcpy(buffer, login, sizeof(login));

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the gethostname operation.
 */
int
gethostname(
	char *buffer,
	size_t size)
{
	size_t length;

	length = size;

	/* Handles the buffer availability. */
	if (buffer == NULL) {
		errno = EFAULT;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the current data size. */
	if (size == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed sysctlbyname operation. */
	if (sysctlbyname("kern.hostname", buffer, &length, NULL, 0) != 0)
		return -1;
	buffer[size - 1U] = '\0';

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the getentropy operation.
 */
int
getentropy(
	void *buffer,
	size_t length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_getentropy, (uintptr_t)buffer, length, 0, 0,
			 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sethostname operation.
 */
int
sethostname(
	const char *name,
	size_t length)
{
	int function_result;

	/* Handles the name availability. */
	if (name == NULL) {
		errno = EFAULT;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the sysctlbyname result. */
	function_result = sysctlbyname("kern.hostname", NULL, NULL, name, length);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ttyname operation.
 */
char *
ttyname(
	int fd)
{
	char *function_result;
	static char name[] = "/dev/console";

	/* Computes the function result. */
	function_result = isatty(fd) ? name : NULL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ttyname r operation.
 */
int
ttyname_r(
	int fd,
	char *buffer,
	size_t size)
{
	static const char name[] = "/dev/console";

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return EINVAL;

	/* Handles a failed isatty operation. */
	if (!isatty(fd))
		return errno;

	/* Checks the current data size. */
	if (size < sizeof(name))
		return ERANGE;
	memcpy(buffer, name, sizeof(name));

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the uname operation.
 */
int
uname(
	struct utsname *name)
{
#if defined(__x86_64__)
	static const char machine[] = "x86_64";
#elif defined(__i386__)
	static const char machine[] = "i386";
#elif defined(__aarch64__)
	static const char machine[] = "aarch64";
#elif defined(__sparc__)
	static const char machine[] = "sparcv9";
#else
	static const char machine[] = "unknown";

#endif

	/* Handles the name availability. */
	if (name == NULL) {
		errno = EFAULT;

		/* Reports operation failure. */
		return -1;
	}
	memset(name, 0, sizeof(*name));
	strcpy(name->sysname, "zedBSD");

	/* Handles a failed gethostname operation. */
	if (gethostname(name->nodename, sizeof(name->nodename)) != 0)
		return -1;
	strcpy(name->release, "0.0.1");
	strcpy(name->version, "zedBSD 0.0.1");
	strcpy(name->machine, machine);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the fileno operation.
 */
int
fileno(
	void *stream)
{
	FILE *file;

	file = stream;

	/* Returns the computed result. */
	return file == NULL || file->context == NULL
		   ? -1
		   : (int)(intptr_t)file->context - 1;
}

/*
 * Implements the posix getdents operation.
 */
ssize_t
posix_getdents(
	int fd,
	void *buffer,
	size_t size,
	int flags)
{
	struct dirent_record source;
	struct posix_dent *destination;
	size_t name_length;
	size_t record_length;
	intptr_t result;
	const size_t alignment = _Alignof(struct posix_dent);
	const size_t name_offset = offsetof(struct posix_dent, d_name);
	const size_t maximum_record =
	    (name_offset + NAME_MAX + 1U + alignment - 1U) & ~(alignment - 1U);
	unsigned char *next;
	size_t remaining;
	size_t written;
	int single_probe;

	next = buffer;
	remaining = size;
	written = 0;
	single_probe = remaining < maximum_record;

	/* Checks the active flags. */
	if (flags != 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the buffer availability. */
	if ((buffer == NULL && size != 0U) ||
	    ((uintptr_t)buffer & (alignment - 1U)) != 0U) {
		errno = EFAULT;

		/* Reports operation failure. */
		return -1;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		result = call(ZEDBSD_SYS_getdents, fd, (uintptr_t)&source,
			      sizeof(source), 0, 0, 0);

		/* Checks the operation result. */
		if (result < 0)
			return written != 0U ? (ssize_t)written : -1;

		/* Checks the operation result. */
		if (result == 0)
			break;

		name_length = strnlen(source.d_name, NAME_MAX);
		record_length =
		    (name_offset + name_length + 1U + alignment - 1U) &
		    ~(alignment - 1U);

		/* Handles the record length condition. */
		if (record_length > remaining) {
			errno = EINVAL;

			/* Returns the computed result. */
			return written != 0U ? (ssize_t)written : -1;
		}
		destination = (struct posix_dent *)(void *)next;
		destination->d_ino = source.d_ino;
		destination->d_reclen = (reclen_t)record_length;
		destination->d_type = (unsigned char)source.d_type;
		memcpy(destination->d_name, source.d_name, name_length);
		destination->d_name[name_length] = '\0';

		/* Handles the record length condition. */
		if (record_length > name_offset + name_length + 1U)
			memset(next + name_offset + name_length + 1U, 0,
			       record_length - name_offset - name_length - 1U);

		next += record_length;
		remaining -= record_length;
		written += record_length;

		/* Handles the single probe condition. */
		if (single_probe || remaining < maximum_record)
			break;
	}

	/* Returns the computed result. */
	return (ssize_t)written;
}

/*
 * Implements the opendir operation.
 */
DIR *
opendir(
	const char *path)
{
	DIR *directory;

	directory = malloc(sizeof(*directory));

	/* Handles the directory availability. */
	if (directory == NULL) {
		errno = ENOMEM;

		/* Reports that no result is available. */
		return NULL;
	}
	directory->fd = open(path, O_RDONLY | O_DIRECTORY);

	/* Handles the directory condition. */
	if (directory->fd < 0) {
		free(directory);

		/* Reports that no result is available. */
		return NULL;
	}

	/* Returns the computed result. */
	return directory;
}

/*
 * Implements the fdopendir operation.
 */
DIR *
fdopendir(
	int fd)
{
	DIR *directory;
	struct stat status;
	int flags;

	/* Handles a failed fstat operation. */
	if (fd < 0 || fstat(fd, &status) != 0)
		return NULL;

	/* Handles a failed S ISDIR operation. */
	if (!S_ISDIR(status.st_mode)) {
		errno = ENOTDIR;

		/* Reports that no result is available. */
		return NULL;
	}
	flags = fcntl(fd, F_GETFL);

	/* Checks the active flags. */
	if (flags < 0)
		return NULL;
	directory = malloc(sizeof(*directory));

	/* Handles the directory availability. */
	if (directory == NULL) {
		errno = ENOMEM;

		/* Reports that no result is available. */
		return NULL;
	}
	directory->fd = fd;

	/* Returns the computed result. */
	return directory;
}

/*
 * Implements the readdir operation.
 */
struct dirent *
readdir(
	DIR *directory)
{
	struct dirent_record entry;
	intptr_t result;

	/* Handles the directory availability. */
	if (directory == NULL) {
		errno = EBADF;

		/* Reports that no result is available. */
		return NULL;
	}
	result = call(ZEDBSD_SYS_getdents, directory->fd, (uintptr_t)&entry,
		      sizeof(entry), 0, 0, 0);

	/* Checks the operation result. */
	if (result <= 0)
		return NULL;
	directory->current.d_ino = entry.d_ino;
	directory->current.d_type = entry.d_type;
	strncpy(directory->current.d_name, entry.d_name,
		sizeof(directory->current.d_name) - 1U);
	directory->current.d_name[sizeof(directory->current.d_name) - 1U] =
	    '\0';

	/* Returns the computed result. */
	return &directory->current;
}

/*
 * Implements the readdir r operation.
 */
int
readdir_r(
	DIR *directory,
	struct dirent *entry,
	struct dirent **result)
{
	struct dirent *current;
	int saved_errno;

	/* Handles the directory availability. */
	if (directory == NULL || entry == NULL || result == NULL)
		return EINVAL;
	errno = 0;
	current = readdir(directory);
	saved_errno = errno;

	/* Handles the current availability. */
	if (current == NULL) {
		*result = NULL;
		/* Returns the computed result. */
		return saved_errno;
	}
	*entry = *current;
	*result = entry;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the alphasort operation.
 */
int
alphasort(
	const struct dirent **left,
	const struct dirent **right)
{
	int function_result;

	/* Obtains the strcoll result. */
	function_result = strcoll((*left)->d_name, (*right)->d_name);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the scandir operation.
 */
int
scandir(
	const char *path,
	struct dirent ***result,
	int (*filter)(const struct dirent *),
	int (*compare)(const struct dirent **, const struct dirent **))
{
	size_t next;
	struct dirent *entry_local, *copy_local;
	struct dirent *entry_local1;
	struct dirent **grown;
	size_t position;
	size_t index;
	struct dirent **entries;
	DIR *directory;
	size_t count, capacity;
	int error;

	entries = NULL;
	count = 0;
	capacity = 0;
	error = 0;

	/* Handles the path availability. */
	if (path == NULL || result == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	directory = opendir(path);

	/* Handles the directory availability. */
	if (directory == NULL)
		return -1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		errno = 0;
		entry_local = readdir(directory);

		/* Handles the entry local availability. */
		if (entry_local == NULL) {
			error = errno;
			break;
		}

		/* Handles a failed filter operation. */
		if (filter != NULL && !filter(entry_local))
			continue;
		copy_local = malloc(sizeof(*copy_local));

		/* Handles the copy local availability. */
		if (copy_local == NULL) {
			error = ENOMEM;
			break;
		}
		*copy_local = *entry_local;
		/* Checks the remaining item count. */
		if (count == capacity) {
						next = capacity != 0 ? capacity * 2U : 16U;

			/* Handles the next condition. */
			if (next < capacity ||
			    next > SIZE_MAX / sizeof(*entries)) {
				free(copy_local);
				error = EOVERFLOW;
				break;
			}
			grown = realloc(entries, next * sizeof(*entries));

			/* Handles the grown availability. */
			if (grown == NULL) {
				free(copy_local);
				error = ENOMEM;
				break;
			}
			entries = grown;
			capacity = next;
		}
		entries[count++] = copy_local;
	}

	/* Handles an operation failure. */
	if (closedir(directory) != 0 && error == 0)
		error = errno;

	/* Handles an operation failure. */
	if (error != 0) {
		/* Process each remaining element. */
		while (count != 0)
			free(entries[--count]);
		free(entries);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the compare availability. */
	if (compare != NULL && count > 1) {
		/* Process each remaining element. */
		for (index = 1; index < count; index++) {
			/* Continue while the operation condition remains true. */
						entry_local1 = entries[index];
						position = index;
			while (
			    position != 0 &&
			    compare(
				(const struct dirent **)&entries[position - 1U],
				(const struct dirent **)&entry_local1) > 0) {
				entries[position] = entries[position - 1U];
				position--;
			}
			entries[position] = entry_local1;
		}
	}
	*result = entries;
	/* Returns the computed result. */
	return (int)count;
}

/*
 * Implements the closedir operation.
 */
int
closedir(
	DIR *directory)
{
	int result;

	/* Handles the directory availability. */
	if (directory == NULL) {
		errno = EBADF;

		/* Reports operation failure. */
		return -1;
	}
	result = close(directory->fd);
	free(directory);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the rewinddir operation.
 */
void
rewinddir(
	DIR *directory)
{
	/* Handles the directory availability. */
	if (directory != NULL)
		(void)lseek(directory->fd, 0, SEEK_SET);
}

/*
 * Implements the seekdir operation.
 */
void
seekdir(
	DIR *directory,
	long location)
{
	/* Handles the directory availability. */
	if (directory != NULL && location >= 0)
		(void)lseek(directory->fd, (off_t)location, SEEK_SET);
}

/*
 * Implements the telldir operation.
 */
long
telldir(
	DIR *directory)
{
	off_t location;

	/* Handles the directory availability. */
	if (directory == NULL) {
		errno = EBADF;

		/* Reports operation failure. */
		return -1;
	}
	location = lseek(directory->fd, 0, SEEK_CUR);

	/* Returns the computed result. */
	return location < 0 || location > LONG_MAX ? -1L : (long)location;
}

/*
 * Implements the dirfd operation.
 */
int
dirfd(
	DIR *directory)
{
	/* Handles the directory availability. */
	if (directory == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return directory->fd;
}

/*
 * Implements the stdio console write operation.
 */
size_t
__stdio_console_write(
	const char *bytes,
	size_t length)
{
	ssize_t result;

	result = write(1, bytes, length);

	/* Returns the computed result. */
	return result < 0 ? 0U : (size_t)result;
}

/*
 * Implements the stdio fork child operation.
 */
void
__stdio_fork_child(
	void)
{
	FILE *stream;

	/* Process each element required by the operation. */
	stream_registry_lock = 0;
	for (stream = stream_registry; stream != NULL;
	     stream = stream->registry_next) {
		stream->lock = 0;
		stream->lock_owner = 0;
		stream->lock_depth = 0;
	}
}

/*
 * Implements the fopen operation.
 */
FILE *
fopen(
	const char *path,
	const char *mode)
{
	off_t position;
	FILE *stream;
	int flags;

	/* Handles the mode availability. */
	if (mode == NULL || mode[0] == '\0') {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	flags = mode[0] == 'r'	 ? O_RDONLY
		: mode[0] == 'w' ? O_WRONLY | O_CREAT | O_TRUNC
		: mode[0] == 'a' ? O_WRONLY | O_CREAT | O_APPEND
				 : -1;

	/* Checks the active flags. */
	if (flags < 0) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles a failed strchr operation. */
	if (strchr(mode, '+') != NULL)
		flags = (flags & ~O_ACCMODE) | O_RDWR;
	stream = calloc(1, sizeof(*stream));

	/* Handles the stream availability. */
	if (stream == NULL) {
		errno = ENOMEM;

		/* Reports that no result is available. */
		return NULL;
	}
	flags = open(path, flags, 0666);

	/* Checks the active flags. */
	if (flags < 0) {
		free(stream);

		/* Reports that no result is available. */
		return NULL;
	}
	stream->context = (void *)(intptr_t)(flags + 1);
	stream->mode = (unsigned)(mode[0] == 'r' ? 1U : 2U);

	/* Handles a failed strchr operation. */
	if (strchr(mode, '+') != NULL)
		stream->mode = 3U;
	stream->buffering_mode = _IOFBF;
	stream->ungot_character = EOF;
	stream->heap_allocated = 1;

	position = lseek(flags, 0, SEEK_CUR);

	/* Handles the position condition. */
	if (position >= 0)
		stream->position = (uint64_t)position;
	stream_register(stream);

	/* Returns the computed result. */
	return stream;
}

/*
 * Implements the fdopen operation.
 */
FILE *
fdopen(
	int descriptor,
	const char *mode)
{
	FILE *stream;

	/* Handles the mode availability. */
	if (descriptor < 0 || mode == NULL ||
	    (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	stream = calloc(1, sizeof(*stream));

	/* Handles the stream availability. */
	if (stream == NULL)
		return NULL;
	stream->context = (void *)(intptr_t)(descriptor + 1);
	stream->mode = mode[0] == 'r' ? 1U : 2U;

	/* Handles a failed strchr operation. */
	if (strchr(mode, '+') != NULL)
		stream->mode = 3U;
	stream->buffering_mode = _IOFBF;
	stream->ungot_character = EOF;
	stream->heap_allocated = 1;
	stream_register(stream);

	/* Returns the computed result. */
	return stream;
}

/*
 * Implements the popen operation.
 */
FILE *
popen(
	const char *command,
	const char *mode)
{
	int descriptors[2];
	pid_t child;
	FILE *stream;

	/* Handles the command availability. */
	if (command == NULL || mode == NULL ||
	    !((mode[0] == 'r' || mode[0] == 'w') && mode[1] == '\0')) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles a failed pipe operation. */
	if (pipe(descriptors) != 0)
		return NULL;
	child = fork();

	/* Checks the child process state. */
	if (child == 0) {
		/* Validates the selected mode. */
		if (mode[0] == 'r') {
			(void)close(descriptors[0]);

			/* Handles a failed dup2 operation. */
			if (dup2(descriptors[1], STDOUT_FILENO) < 0)
				_exit(127);
			(void)close(descriptors[1]);
		} else {
			(void)close(descriptors[1]);

			/* Handles a failed dup2 operation. */
			if (dup2(descriptors[0], STDIN_FILENO) < 0)
				_exit(127);
			(void)close(descriptors[0]);
		}
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}

	/* Checks the child process state. */
	if (child < 0) {
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);

		/* Reports that no result is available. */
		return NULL;
	}

	/* Validates the selected mode. */
	if (mode[0] == 'r') {
		(void)close(descriptors[1]);
		stream = fdopen(descriptors[0], "r");
	} else {
		(void)close(descriptors[0]);
		stream = fdopen(descriptors[1], "w");
	}

	/* Handles the stream availability. */
	if (stream == NULL) {
		(void)kill(child, SIGTERM);
		(void)waitpid(child, NULL, 0);

		/* Reports that no result is available. */
		return NULL;
	}
	stream->child_pid = child;

	/* Returns the computed result. */
	return stream;
}

/*
 * Implements the pclose operation.
 */
int
pclose(
	FILE *stream)
{
	pid_t child;
	int status;

	/* Handles the stream availability. */
	if (stream == NULL || stream->child_pid <= 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	child = stream->child_pid;

	/* Handles the end-of-file condition. */
	if (fclose(stream) == EOF)
		return -1;

	/* Continue while the operation condition remains true. */
	while (waitpid(child, &status, 0) < 0)

		/* Handles the reported system error. */
		if (errno != EINTR)
			return -1;

	/* Returns the computed result. */
	return status;
}

/*
 * Implements the fclose operation.
 */
int
fclose(
	FILE *stream)
{
	int result, flush_result, old;
	unsigned allocated;

	/* Handles the stream availability. */
	if (stream == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return EOF;
	}
	(void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
	stream_registry_acquire();
	stream_unregister_locked(stream);
	flockfile(stream);
	stream_registry_release();
	flush_result = stream_flush_locked(stream);
	result = stream->cookie_close != NULL
		     ? stream->cookie_close(stream->context)
		     : close(stream_fd(stream));
	allocated = stream->heap_allocated;

	/* Handles the stream condition. */
	if (stream->buffer_owned)
		free(stream->buffer);
	stream->buffer = NULL;
	stream->context = NULL;
	funlockfile(stream);

	/* Handles the allocated condition. */
	if (allocated)
		free(stream);
	(void)pthread_setcancelstate(old, NULL);

	/* Returns the computed result. */
	return result == 0 && flush_result == 0 ? 0 : EOF;
}

/*
 * Implements the fflush operation.
 */
int
fflush(
	FILE *stream)
{
	int old, result;

	result = 0;

	/* Handles the stream availability. */
	if (stream != NULL) {
		stream_enter(stream, &old);
		result = stream_flush_locked(stream);
		stream_leave(stream, old);

		/* Returns the computed result. */
		return result;
	}
	(void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
	stream_registry_acquire();

	/* Process each element required by the operation. */
	for (stream = stream_registry; stream != NULL;
	     stream = stream->registry_next) {
		flockfile(stream);

		/* Handles the end-of-file condition. */
		if (stream_flush_locked(stream) == EOF)
			result = EOF;
		funlockfile(stream);
	}
	stream_registry_release();
	(void)pthread_setcancelstate(old, NULL);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the fread operation.
 */
size_t
fread(
	void *buffer,
	size_t size,
	size_t count,
	FILE *stream)
{
	size_t available;
	size_t take;
	ssize_t result;
	size_t total, done;
	int old;

	done = 0;

	/* Handles the stream availability. */
	if (stream == NULL || (buffer == NULL && size != 0 && count != 0)) {
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the current data size. */
	if (size != 0 && count > SIZE_MAX / size) {
		errno = EOVERFLOW;

		/* Reports successful completion. */
		return 0;
	}
	total = size * count;

	/* Handles the total condition. */
	if (total == 0)
		return 0;
	stream_enter(stream, &old);
	stream->io_started = 1;

	/* Handles the end-of-file condition. */
	if (stream->last_operation == 2 && stream_flush_locked(stream) == EOF)
		goto read_done;
	stream->last_operation = 1;

	/* Handles the end-of-file condition. */
	if (stream->ungot_character != EOF) {
		((unsigned char *)buffer)[done++] =
		    (unsigned char)stream->ungot_character;
		stream->ungot_character = EOF;
	}
	while (done < total) {
		/* Handles the stream condition. */
		if (stream->buffer_start < stream->buffer_length) {
						available = stream->buffer_length - stream->buffer_start;
						take = available < total - done ? available : total - done;
			memcpy((unsigned char *)buffer + done,
			       stream->buffer + stream->buffer_start, take);
			stream->buffer_start += take;
			done += take;
			continue;
		}
		stream->buffer_start = stream->buffer_length = 0;

		/* Handles the stream condition. */
		if (stream->buffering_mode == _IONBF) {
			result = stream_read_direct(
			    stream, (unsigned char *)buffer + done,
			    total - done);

			/* Checks the operation result. */
			if (result > 0) {
				done += (size_t)result;
				continue;
			}
		} else {
			/* Handles a failed stream ensure buffer operation. */
			if (stream_ensure_buffer(stream) != 0)
				break;
			result = stream_read_direct(stream, stream->buffer,
						    stream->buffer_size);

			/* Checks the operation result. */
			if (result > 0) {
				stream->buffer_length = (size_t)result;
				continue;
			}
		}

		/* Checks the operation result. */
		if (result == 0) {
			stream->eof = 1;
			break;
		}

		/* Handles the reported system error. */
		if (errno == EINTR)
			continue;
		stream->error = 1;
		break;
	}
read_done:
	stream->position += done;
	stream_leave(stream, old);

	/* Returns the computed result. */
	return done / size;
}

/*
 * Implements the fwrite operation.
 */
size_t
fwrite(
	const void *buffer,
	size_t size,
	size_t count,
	FILE *stream)
{
	size_t space;
	size_t put;
	const unsigned char *source;
	size_t total, done;
	int old;

	done = 0;

	/* Handles the stream availability. */
	if (stream == NULL || (buffer == NULL && size != 0 && count != 0)) {
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the current data size. */
	if (size != 0 && count > SIZE_MAX / size) {
		errno = EOVERFLOW;

		/* Reports successful completion. */
		return 0;
	}
	total = size * count;

	/* Handles the total condition. */
	if (total == 0)
		return 0;
	stream_enter(stream, &old);
	stream->io_started = 1;

	/* Handles the end-of-file condition. */
	if (stream->last_operation == 1 && stream_flush_locked(stream) == EOF)
		goto write_done;
	stream->last_operation = 2;

	/* Handles the stream condition. */
	if (stream->buffering_mode == _IONBF) {
		(void)stream_write_direct(stream, buffer, total, &done);
	} else if (stream_ensure_buffer(stream) == 0) {
		/* Continue while the operation condition remains true. */
		while (done < total) {

			space = stream->buffer_size - stream->buffer_length;
			put = space < total - done ? space : total - done;
			source = (const unsigned char *)buffer + done;
			memcpy(stream->buffer + stream->buffer_length, source,
			       put);
			stream->buffer_length += put;
			done += put;

			/* Handles a failed memchr operation. */
			if (stream->buffer_length == stream->buffer_size ||
			    (stream->buffering_mode == _IOLBF &&
			     memchr(source, '\n', put) != NULL))

				/* Handles the end-of-file condition. */
				if (stream_flush_locked(stream) == EOF)
					break;
		}
	}
write_done:
	stream->position += done;
	stream_leave(stream, old);

	/* Returns the computed result. */
	return done / size;
}

/*
 * Implements the getc operation.
 */
int
getc(
	FILE *stream)
{
	int function_result;
	unsigned char byte;

	/* Computes the function result. */
	function_result = fread(&byte, 1, 1, stream) == 1 ? byte : EOF;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ungetc operation.
 */
int
ungetc(
	int character,
	FILE *stream)
{
	int old, result;

	result = EOF;

	/* Handles the end-of-file condition. */
	if (stream == NULL || character == EOF)
		return EOF;
	stream_enter(stream, &old);

	/* Handles the end-of-file condition. */
	if (stream->last_operation != 2 && stream->ungot_character == EOF) {
		stream->ungot_character = (unsigned char)character;
		stream->eof = 0;

		/* Handles the stream condition. */
		if (stream->position != 0)
			stream->position--;
		result = (unsigned char)character;
	}
	stream_leave(stream, old);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the fgets operation.
 */
char *
fgets(
	char *buffer,
	int size,
	FILE *stream)
{
	int c, i, old;

	i = 0;

	/* Handles the buffer availability. */
	if (buffer == NULL || stream == NULL || size <= 0)
		return NULL;
	stream_enter(stream, &old);

	/* Process input until it is exhausted. */
	while (i + 1 < size && (c = getc(stream)) != EOF) {
		buffer[i++] = (char)c;

		/* Classifies the current input character. */
		if (c == '\n')
			break;
	}

	/* Checks the current index. */
	if (i == 0) {
		stream_leave(stream, old);

		/* Reports that no result is available. */
		return NULL;
	}
	buffer[i] = '\0';
	stream_leave(stream, old);

	/* Returns the computed result. */
	return buffer;
}

/*
 * Implements the fseek operation.
 */
int
fseek(
	FILE *stream,
	long offset,
	int whence)
{
	off_t at;
	int old;

	/* Handles the stream availability. */
	if (stream == NULL ||
	    (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	stream_enter(stream, &old);

	/* Handles the end-of-file condition. */
	if (stream->last_operation == 2 && stream_flush_locked(stream) == EOF) {
		stream_leave(stream, old);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the cookie seek availability. */
	if (stream->cookie_seek != NULL) {
		at = stream->cookie_seek(stream->context, offset, whence);
	} else if (whence == SEEK_CUR) {
		/* Checks the current offset. */
		if ((offset < 0 &&
		     (uint64_t)(-(offset + 1L)) + 1U > stream->position) ||
		    (offset > 0 &&
		     (uint64_t)offset > UINT64_MAX - stream->position)) {
			stream_leave(stream, old);
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		at = lseek(stream_fd(stream),
			   (off_t)(offset < 0
				       ? stream->position -
					     ((uint64_t)(-(offset + 1L)) + 1U)
				       : stream->position + (uint64_t)offset),
			   SEEK_SET);
	} else
		at = lseek(stream_fd(stream), offset, whence);

	/* Handles the at condition. */
	if (at >= 0) {
		stream->position = (uint64_t)at;
		stream->eof = 0;
		stream->ungot_character = EOF;
		stream->buffer_start = stream->buffer_length = 0;
		stream->last_operation = 0;
	} else
		stream->error = 1;
	stream_leave(stream, old);

	/* Returns the computed result. */
	return at < 0 ? -1 : 0;
}

/*
 * Implements the ftell operation.
 */
long
ftell(
	FILE *stream)
{
	uint64_t position;
	int old;

	/* Handles the stream availability. */
	if (stream == NULL) {
		errno = EINVAL;

		/* Returns the computed result. */
		return -1L;
	}
	stream_enter(stream, &old);
	position = stream->position;
	stream_leave(stream, old);

	/* Handles the position condition. */
	if (position > LONG_MAX) {
		errno = EOVERFLOW;

		/* Returns the computed result. */
		return -1L;
	}

	/* Returns the computed result. */
	return (long)position;
}

/*
 * Implements the setvbuf operation.
 */
int
setvbuf(
	FILE *stream,
	char *buffer,
	int mode,
	size_t size)
{
	int old;

	/* Handles the stream availability. */
	if (stream == NULL ||
	    (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) ||
	    (mode != _IONBF && size == 0)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	stream_enter(stream, &old);

	/* Handles the stream condition. */
	if (stream->io_started) {
		stream_leave(stream, old);
		errno = EBUSY;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the stream condition. */
	if (stream->buffer_owned)
		free(stream->buffer);
	stream->buffer = mode == _IONBF ? NULL : (unsigned char *)buffer;
	stream->buffer_size = mode == _IONBF ? 0 : size;
	stream->buffer_owned = 0;

	/* Handles the buffer availability. */
	if (mode != _IONBF && buffer == NULL) {
		stream->buffer = malloc(size);

		/* Handles the buffer availability. */
		if (stream->buffer == NULL) {
			stream->buffer_size = 0;
			stream->buffering_mode = _IONBF;
			stream_leave(stream, old);
			errno = ENOMEM;

			/* Reports operation failure. */
			return -1;
		}
		stream->buffer_owned = 1;
	}
	stream->buffering_mode = mode;
	stream_leave(stream, old);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the setbuf operation.
 */
void
setbuf(
	FILE *stream,
	char *buffer)
{
	(void)setvbuf(stream, buffer, buffer != NULL ? _IOFBF : _IONBF,
		      buffer != NULL ? BUFSIZ : 0);
}

/*
 * Implements the fpurge operation.
 */
int
fpurge(
	FILE *stream)
{
	int old;

	/* Handles the stream availability. */
	if (stream == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return EOF;
	}
	stream_enter(stream, &old);
	stream->buffer_start = stream->buffer_length = 0;
	stream->ungot_character = EOF;
	stream->eof = stream->error = 0;
	stream_leave(stream, old);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the funopen operation.
 */
FILE *
funopen(
	const void *cookie,
	int (*readfn)(void *, char *, int),
	int (*writefn)(void *, const char *, int),
	fpos_t (*seekfn)(void *, fpos_t, int),
	int (*closefn)(void *))
{
	FILE *stream;

	/* Handles the readfn availability. */
	if (readfn == NULL && writefn == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	stream = calloc(1, sizeof(*stream));

	/* Handles the stream availability. */
	if (stream == NULL)
		return NULL;
	stream->context = (void *)(uintptr_t)cookie;
	stream->mode = (readfn != NULL ? 1U : 0U) | (writefn != NULL ? 2U : 0U);
	stream->buffering_mode = _IOFBF;
	stream->ungot_character = EOF;
	stream->heap_allocated = 1;
	stream->cookie_read = readfn;
	stream->cookie_write = writefn;
	stream->cookie_seek = seekfn;
	stream->cookie_close = closefn;
	stream_register(stream);

	/* Returns the computed result. */
	return stream;
}

/*
 * Implements the freopen operation.
 */
FILE *
freopen(
	const char *path,
	const char *mode,
	FILE *stream)
{
	FILE *replacement;
	unsigned allocated;

	/* Handles the stream availability. */
	if (stream == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	replacement = fopen(path, mode);

	/* Handles the replacement availability. */
	if (replacement == NULL) {
		(void)fclose(stream);

		/* Reports that no result is available. */
		return NULL;
	}
	allocated = stream->heap_allocated;
	stream_registry_acquire();
	stream_unregister_locked(stream);
	stream_unregister_locked(replacement);
	stream_registry_release();
	(void)fflush(stream);

	/* Handles the cookie close availability. */
	if (stream->cookie_close != NULL)
		(void)stream->cookie_close(stream->context);
	else
		(void)close(stream_fd(stream));

	/* Handles the stream condition. */
	if (stream->buffer_owned)
		free(stream->buffer);
	*stream = *replacement;
	stream->heap_allocated = allocated;
	stream->lock = 0;
	stream->lock_owner = 0;
	stream->lock_depth = 0;
	free(replacement);
	stream_register(stream);

	/* Returns the computed result. */
	return stream;
}

/*
 * Implements the gmtime r operation.
 */
struct tm *
gmtime_r(
	const time_t *restrict value,
	struct tm *restrict result)
{
	int64_t days, seconds;
	int year;
	unsigned month, day, year_day;

	/* Handles the value availability. */
	if (value == NULL || result == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	days = floor_div(*value, 86400);
	seconds = *value - days * 86400;
	civil_from_days(days, &year, &month, &day, &year_day);
	result->tm_year = year - 1900;
	result->tm_mon = (int)month - 1;
	result->tm_mday = (int)day;
	result->tm_hour = (int)(seconds / 3600);
	result->tm_min = (int)(seconds / 60 % 60);
	result->tm_sec = (int)(seconds % 60);
	result->tm_wday = (int)((days + 4) % 7);

	/* Checks the operation result. */
	if (result->tm_wday < 0)
		result->tm_wday += 7;
	result->tm_yday = (int)year_day;
	result->tm_isdst = 0;

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the gmtime operation.
 */
struct tm *
gmtime(
	const time_t *value)
{
	struct tm *function_result;
	static struct tm result;

	/* Obtains the gmtime r result. */
	function_result = gmtime_r(value, &result);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tzset operation.
 */
void
tzset(
	void)
{
	const char *next;
	const char *text, *at;

	text = getenv("TZ");

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		text = "UTC0";
	at = timezone_name(text, timezone_standard, sizeof(timezone_standard));

	/* Handles a failed timezone offset operation. */
	if (at == NULL || (at = timezone_offset(at, &timezone_east)) == NULL) {
		strcpy(timezone_standard, "UTC");
		strcpy(timezone_daylight, "UTC");
		timezone_east = timezone_daylight_east = 0;
		timezone_has_daylight = 0;
		timezone = 0;
		daylight = 0;

		/* Returns the computed result. */
		return;
	}
	strcpy(timezone_daylight, timezone_standard);
	timezone_daylight_east = timezone_east + 3600;
	timezone_has_daylight = 0;
	timezone = -timezone_east;
	daylight = 0;

	/* Handles the at condition. */
	if (*at == '\0')
		return;
	at = timezone_name(at, timezone_daylight, sizeof(timezone_daylight));

	/* Handles the at availability. */
	if (at == NULL)
		return;
	timezone_has_daylight = 1;
	daylight = 1;

	/* Handles the at condition. */
	if (*at != ',' && *at != '\0') {
				next = timezone_offset(at, &timezone_daylight_east);

		/* Handles the next availability. */
		if (next == NULL) {
			timezone_has_daylight = 0;
			daylight = 0;

			/* Returns the computed result. */
			return;
		}
		at = next;
	}
	timezone_start =
	    (struct timezone_rule){TZ_RULE_MONTH, 3, 2, 0, 2 * 3600};
	timezone_end =
	    (struct timezone_rule){TZ_RULE_MONTH, 11, 1, 0, 2 * 3600};

	/* Handles the at condition. */
	if (*at == ',') {
		at = timezone_rule_parse(at + 1, &timezone_start);

		/* Handles a failed timezone rule parse operation. */
		if (at == NULL || *at != ',' ||
		    (at = timezone_rule_parse(at + 1, &timezone_end)) == NULL ||
		    *at != '\0') {
			timezone_has_daylight = 0;
			daylight = 0;
		}
	}
}

/*
 * Implements the localtime r operation.
 */
struct tm *
localtime_r(
	const time_t *restrict value,
	struct tm *restrict result)
{
	time_t local;
	long offset;

	/* Handles the value availability. */
	if (value == NULL || result == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	tzset();
	offset = timezone_is_daylight(*value) ? timezone_daylight_east
					      : timezone_east;

	/* Checks the current offset. */
	if ((offset > 0 && *value > INT64_MAX - offset) ||
	    (offset < 0 && *value < INT64_MIN - offset)) {
		errno = EOVERFLOW;

		/* Reports that no result is available. */
		return NULL;
	}
	local = *value + offset;

	/* Handles a failed gmtime r operation. */
	if (gmtime_r(&local, result) == NULL)
		return NULL;
	result->tm_isdst =
	    offset == timezone_daylight_east && timezone_has_daylight;

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the localtime operation.
 */
struct tm *
localtime(
	const time_t *value)
{
	struct tm *function_result;
	static struct tm result;

	/* Obtains the localtime r result. */
	function_result = localtime_r(value, &result);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mktime operation.
 */
time_t
mktime(
	struct tm *value)
{
	int64_t year, month, days, seconds;
	time_t result;
	struct tm normalized;

	/* Handles the value availability. */
	if (value == NULL) {
		errno = EINVAL;

		/* Returns the computed result. */
		return (time_t)-1;
	}
	year = (int64_t)value->tm_year + 1900;
	month = value->tm_mon;
	year += floor_div(month, 12);
	month -= floor_div(month, 12) * 12;
	days = days_from_civil(year, (unsigned)month + 1U, 1U) +
	       (int64_t)value->tm_mday - 1;
	seconds = days * 86400 + (int64_t)value->tm_hour * 3600 +
		  (int64_t)value->tm_min * 60 + value->tm_sec;
	tzset();
	result = seconds -
		 (value->tm_isdst > 0 ? timezone_daylight_east : timezone_east);

	/* Handles a failed timezone is daylight operation. */
	if (value->tm_isdst < 0 && timezone_is_daylight(result))
		result = seconds - timezone_daylight_east;

	/* Handles a failed localtime r operation. */
	if (localtime_r(&result, &normalized) == NULL)
		return (time_t)-1;
	*value = normalized;
	/* Returns the computed result. */
	return result;
}

/*
 * Implements the difftime operation.
 */
double
difftime(
	time_t end,
	time_t beginning)
{
	/* Returns the computed result. */
	return (double)end - (double)beginning;
}

/*
 * Implements the asctime r operation.
 */
char *
asctime_r(
	const struct tm *restrict value,
	char *restrict buffer)
{
	static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed",
					       "Thu", "Fri", "Sat"};
	static const char *const months[] = {"Jan", "Feb", "Mar", "Apr",
					     "May", "Jun", "Jul", "Aug",
					     "Sep", "Oct", "Nov", "Dec"};

	/* Handles the value availability. */
	if (value == NULL || buffer == NULL || value->tm_wday < 0 ||
	    value->tm_wday > 6 || value->tm_mon < 0 || value->tm_mon > 11) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles a failed snprintf operation. */
	if (snprintf(buffer, 26, "%.3s %.3s %2d %02d:%02d:%02d %04d\n",
		     weekdays[value->tm_wday], months[value->tm_mon],
		     value->tm_mday, value->tm_hour, value->tm_min,
		     value->tm_sec, value->tm_year + 1900) != 25) {
		errno = EOVERFLOW;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Returns the computed result. */
	return buffer;
}

/*
 * Implements the asctime operation.
 */
char *
asctime(
	const struct tm *value)
{
	char *function_result;
	static char buffer[26];

	/* Obtains the asctime r result. */
	function_result = asctime_r(value, buffer);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ctime r operation.
 */
char *
ctime_r(
	const time_t *restrict value,
	char *restrict buffer)
{
	char *function_result;
	struct tm local;

	/* Computes the function result. */
	function_result = localtime_r(value, &local) != NULL ? asctime_r(&local, buffer)
						  : NULL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ctime operation.
 */
char *
ctime(
	const time_t *value)
{
	char *function_result;
	static char buffer[26];

	/* Obtains the ctime r result. */
	function_result = ctime_r(value, buffer);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the strftime l operation.
 */
size_t
strftime_l(
	char *restrict destination,
	size_t capacity,
	const char *restrict format,
	const struct tm *restrict value,
	locale_t locale)
{
	char b_local[8];
	char b_local1[16];
	char b_local2[16];
	int y_local, w_local;
	int y_local3, w_local4;
	char b_local5[8];
	char b_local6[12];
	char b_local7[16];
	char b_local8[32];
	int y_local9, w_local10;
	int hour;
	long z;
	int h;
	int wd;
	char conversion;
	static const char *const weekday_short[] = {"Sun", "Mon", "Tue", "Wed",
						    "Thu", "Fri", "Sat"};
	static const char *const weekday_long[] = {
	    "Sunday",	"Monday", "Tuesday", "Wednesday",
	    "Thursday", "Friday", "Saturday"};
	static const char *const month_short[] = {"Jan", "Feb", "Mar", "Apr",
						  "May", "Jun", "Jul", "Aug",
						  "Sep", "Oct", "Nov", "Dec"};
	static const char *const month_long[] = {
	    "January",	 "February", "March",	 "April",
	    "May",	 "June",     "July",	 "August",
	    "September", "October",  "November", "December"};
	char *output;
	size_t remaining;
	char literal[2];

	output = destination;
	remaining = capacity;
	(void)locale;

	/* Handles the destination availability. */
	if (destination == NULL || format == NULL || value == NULL ||
	    capacity == 0)

		/* Reports successful completion. */
		return 0;
	tzset();

	/* Continue while the operation condition remains true. */
	while (*format != '\0') {
		/* Handles the format condition. */
		if (*format != '%') {
			literal[0] = *format++;
			literal[1] = '\0';

			/* Handles a failed strftime append operation. */
			if (strftime_append(&output, &remaining, literal) != 0)
				return 0;
			continue;
		}
		format++;

		/* Handles the format condition. */
		if (*format == 'E' || *format == 'O')
			format++;
		conversion = *format++;

		/* Dispatch the selected operation case. */
		switch (conversion) {
		case '%':
			/* Handles the strftime append condition. */
			if (strftime_append(&output, &remaining, "%"))
				return 0;
			break;
		case 'a':
			/* Handles a failed strftime append operation. */
			if (value->tm_wday < 0 || value->tm_wday > 6 ||
			    strftime_append(&output, &remaining,
					    weekday_short[value->tm_wday]))

				/* Reports successful completion. */
				return 0;
			break;
		case 'A':
			/* Handles a failed strftime append operation. */
			if (value->tm_wday < 0 || value->tm_wday > 6 ||
			    strftime_append(&output, &remaining,
					    weekday_long[value->tm_wday]))

				/* Reports successful completion. */
				return 0;
			break;
		case 'b':
		case 'h':
			/* Handles a failed strftime append operation. */
			if (value->tm_mon < 0 || value->tm_mon > 11 ||
			    strftime_append(&output, &remaining,
					    month_short[value->tm_mon]))

				/* Reports successful completion. */
				return 0;
			break;
		case 'B':
			/* Handles a failed strftime append operation. */
			if (value->tm_mon < 0 || value->tm_mon > 11 ||
			    strftime_append(&output, &remaining,
					    month_long[value->tm_mon]))

				/* Reports successful completion. */
				return 0;
			break;
		case 'C':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining,
					    (value->tm_year + 1900) / 100, 2,
					    '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'd':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining, value->tm_mday,
					    2, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'e':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining, value->tm_mday,
					    2, ' '))

				/* Reports successful completion. */
				return 0;
			break;
		case 'H':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining, value->tm_hour,
					    2, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'I':

		hour = value->tm_hour % 12;

		/* Handles the hour condition. */
		if (!hour)
			hour = 12;

		/* Handles the strftime number condition. */
		if (strftime_number(&output, &remaining, hour, 2, '0'))
			return 0;
		break;
		case 'j':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining,
					    value->tm_yday + 1, 3, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'm':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining,
					    value->tm_mon + 1, 2, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'M':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining, value->tm_min,
					    2, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'n':
			/* Handles the strftime append condition. */
			if (strftime_append(&output, &remaining, "\n"))
				return 0;
			break;
		case 'p':
			/* Handles a failed strftime append operation. */
			if (strftime_append(&output, &remaining,
					    value->tm_hour < 12 ? "AM" : "PM"))

				/* Reports successful completion. */
				return 0;
			break;
		case 'S':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining, value->tm_sec,
					    2, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 't':
			/* Handles the strftime append condition. */
			if (strftime_append(&output, &remaining, "\t"))
				return 0;
			break;
		case 'u':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining,
					    value->tm_wday ? value->tm_wday : 7,
					    1, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'w':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining, value->tm_wday,
					    1, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'y':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining,
					    (value->tm_year + 1900) % 100, 2,
					    '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'Y':
			/* Handles a failed strftime number operation. */
			if (strftime_number(&output, &remaining,
					    value->tm_year + 1900, 4, '0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'z':

		z = value->tm_isdst > 0 ? timezone_daylight_east
					     : timezone_east;
		snprintf(b_local, sizeof(b_local), "%c%02ld%02ld",
			 z < 0 ? '-' : '+', (z < 0 ? -z : z) / 3600,
			 ((z < 0 ? -z : z) % 3600) / 60);

		/* Handles the strftime append condition. */
		if (strftime_append(&output, &remaining, b_local))
			return 0;
		break;
		case 'Z':
			/* Handles a failed strftime append operation. */
			if (strftime_append(&output, &remaining,
					    tzname[value->tm_isdst > 0]))

				/* Reports successful completion. */
				return 0;
			break;
		case 'D':
		case 'x':

		snprintf(b_local1, sizeof(b_local1), "%02d/%02d/%02d",
			 value->tm_mon + 1, value->tm_mday,
			 (value->tm_year + 1900) % 100);

		/* Handles the strftime append condition. */
		if (strftime_append(&output, &remaining, b_local1))
			return 0;
		break;
		case 'F':

		snprintf(b_local2, sizeof(b_local2), "%04d-%02d-%02d",
			 value->tm_year + 1900, value->tm_mon + 1,
			 value->tm_mday);

		/* Handles the strftime append condition. */
		if (strftime_append(&output, &remaining, b_local2))
			return 0;
		break;
		case 'g':

		iso_week(value, &y_local, &w_local);
		(void)w_local;

		/* Handles the strftime number condition. */
		if (strftime_number(&output, &remaining, y_local % 100, 2,
				    '0'))

			/* Reports successful completion. */
			return 0;
		break;
		case 'G':

		iso_week(value, &y_local3, &w_local4);
		(void)w_local4;

		/* Handles the strftime number condition. */
		if (strftime_number(&output, &remaining, y_local3, 4, '0'))
			return 0;
		break;
		case 'R':

		snprintf(b_local5, sizeof(b_local5), "%02d:%02d", value->tm_hour,
			 value->tm_min);

		/* Handles the strftime append condition. */
		if (strftime_append(&output, &remaining, b_local5))
			return 0;
		break;
		case 'T':
		case 'X':

		snprintf(b_local6, sizeof(b_local6), "%02d:%02d:%02d", value->tm_hour,
			 value->tm_min, value->tm_sec);

		/* Handles the strftime append condition. */
		if (strftime_append(&output, &remaining, b_local6))
			return 0;
		break;
		case 'r':

		h = value->tm_hour % 12;

		/* Handles the h condition. */
		if (!h)
			h = 12;
		snprintf(b_local7, sizeof(b_local7), "%02d:%02d:%02d %s", h,
			 value->tm_min, value->tm_sec,
			 value->tm_hour < 12 ? "AM" : "PM");

		/* Handles the strftime append condition. */
		if (strftime_append(&output, &remaining, b_local7))
			return 0;
		break;
		case 'c':

		/* Handles a failed strftime l operation. */
		if (strftime_l(b_local8, sizeof(b_local8), "%a %b %e %T %Y", value,
			       locale) == 0 ||
		    strftime_append(&output, &remaining, b_local8))

			/* Reports successful completion. */
			return 0;
		break;
		case 'U':
			/* Handles a failed strftime number operation. */
			if (strftime_number(
				&output, &remaining,
				(value->tm_yday + 7 - value->tm_wday) / 7, 2,
				'0'))

				/* Reports successful completion. */
				return 0;
			break;
		case 'V':

		iso_week(value, &y_local9, &w_local10);
		(void)y_local9;

		/* Handles the strftime number condition. */
		if (strftime_number(&output, &remaining, w_local10, 2, '0'))
			return 0;
		break;
		case 'W':

		wd = value->tm_wday ? value->tm_wday - 1 : 6;

		/* Handles a failed strftime number operation. */
		if (strftime_number(&output, &remaining,
				    (value->tm_yday + 7 - wd) / 7, 2,
				    '0'))

			/* Reports successful completion. */
			return 0;
		break;
		default:
			/* Reports successful completion. */
			return 0;
		}
	}
	*output = '\0';
	/* Returns the computed result. */
	return (size_t)(output - destination);
}

/*
 * Implements the strftime operation.
 */
size_t
strftime(
	char *restrict destination,
	size_t capacity,
	const char *restrict format,
	const struct tm *restrict value)
{
	size_t function_result;

	/* Obtains the strftime l result. */
	function_result = strftime_l(destination, capacity, format, value,
			  LC_GLOBAL_LOCALE);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the clock operation.
 */
clock_t
clock(
	void)
{
	struct process_times_record record;
	uint64_t value;

	/* Handles a failed call operation. */
	if (call(ZEDBSD_SYS_times, (uintptr_t)&record, sizeof(record), 0, 0, 0,
		 0) < 0)

		/* Returns the computed result. */
		return (clock_t)-1;

	/* Handles the record condition. */
	if (record.self_ticks > UINT64_MAX / (CLOCKS_PER_SEC / 100L)) {
		errno = EOVERFLOW;

		/* Returns the computed result. */
		return (clock_t)-1;
	}
	value = record.self_ticks * (CLOCKS_PER_SEC / 100L);

	/* Validates the current value. */
	if (value > (uint64_t)LONG_MAX) {
		errno = EOVERFLOW;

		/* Returns the computed result. */
		return (clock_t)-1;
	}

	/* Returns the computed result. */
	return (clock_t)value;
}

/*
 * Implements the times operation.
 */
clock_t
times(
	struct tms *result)
{
	struct process_times_record record;

	/* Handles a failed call operation. */
	if (call(ZEDBSD_SYS_times, (uintptr_t)&record, sizeof(record), 0, 0, 0,
		 0) < 0)

		/* Returns the computed result. */
		return (clock_t)-1;

	/* Handles the record condition. */
	if (record.self_ticks > (uint64_t)LONG_MAX ||
	    record.child_ticks > (uint64_t)LONG_MAX ||
	    record.system_ticks > record.self_ticks ||
	    record.child_system_ticks > record.child_ticks ||
	    record.elapsed_ticks > (uint64_t)LONG_MAX) {
		errno = EOVERFLOW;

		/* Returns the computed result. */
		return (clock_t)-1;
	}

	/* Handles the result availability. */
	if (result != NULL) {
		result->tms_utime =
		    (clock_t)(record.self_ticks - record.system_ticks);
		result->tms_stime = (clock_t)record.system_ticks;
		result->tms_cutime =
		    (clock_t)(record.child_ticks - record.child_system_ticks);
		result->tms_cstime = (clock_t)record.child_system_ticks;
	}

	/* Returns the computed result. */
	return (clock_t)record.elapsed_ticks;
}

/*
 * Implements the time operation.
 */
time_t
time(
	time_t *result)
{
	struct timespec ts;
	time_t value =
	    clock_gettime(CLOCK_REALTIME, &ts) == 0 ? ts.tv_sec : (time_t)-1;

	/* Handles the result availability. */
	if (result != NULL)
		*result = value;
	/* Returns the computed result. */
	return value;
}

/*
 * Implements the gettimeofday operation.
 */
int
gettimeofday(
	struct timeval *result,
	void *timezone)
{
	struct timespec now;

	(void)timezone;

	/* Handles the result availability. */
	if (result == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return -1;
	result->tv_sec = now.tv_sec;
	result->tv_usec = now.tv_nsec / 1000L;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the exit operation.
 */
void
exit(
	int status)
{
	extern void __libc_run_exit_handlers(void);
	__libc_run_exit_handlers();
#if defined(ZEDBSD_DYNAMIC_LIBC)
	__rtld_exports.process_fini();
#else

	/* Handles the rtld process fini availability. */
	if (__rtld_process_fini != NULL)
		__rtld_process_fini();
#endif
	(void)fflush(NULL);
	_exit(status);
}

/*
 * Implements the libc panic operation.
 */
void
__libc_panic(
	const char *message)
{
	(void)write(2, message, strlen(message));
	(void)write(2, "\n", 1);
	_exit(127);
}

/*
 * Implements the libc init operation.
 */
void
__libc_init(
	int argc,
	char **argv,
	char **envp)
{
#define USER_HEAP_INITIAL (64U * 1024U)
	uintptr_t *auxiliary;
	char **environment_end;
	void *arena;
	size_t env_count;
	unsigned auxiliary_count;

	/* Continue while the operation condition remains true. */
	environment_end = envp;
	while (environment_end != NULL && *environment_end != NULL)
		environment_end++;
	auxiliary =
	    environment_end != NULL ? (uintptr_t *)(environment_end + 1) : NULL;

	/* Process each remaining element. */
	secure_execution = 0;
	for (auxiliary_count = 0; auxiliary != NULL && auxiliary_count < 64U;
	     auxiliary_count++, auxiliary += 2) {
		/* Handles the auxiliary condition. */
		if (auxiliary[0] == AT_NULL)
			break;

		/* Handles the auxiliary condition. */
		if (auxiliary[0] == AT_SECURE)
			secure_execution = auxiliary[1] != 0;
	}

	/* Validates the command-line arguments. */
	if (argc > 0 && argv != NULL)
		setprogname(argv[0]);

	/* Process each remaining element. */
	for (env_count = 0; env_count < ENVIRONMENT_MAX && envp != NULL &&
			    envp[env_count] != NULL;
	     env_count++)
		environment_entries[env_count] = envp[env_count];
	environment_entries[env_count] = NULL;
	environ = environment_entries;
	stdin->context = (void *)(intptr_t)1;
	stdout->context = (void *)(intptr_t)2;
	stderr->context = (void *)(intptr_t)3;
	stdin->mode = 1U;
	stdout->mode = stderr->mode = 2U;
	stdin->buffering_mode = _IOFBF;
	stdout->buffering_mode = _IOLBF;
	stderr->buffering_mode = _IONBF;
	stdin->ungot_character = stdout->ungot_character =
	    stderr->ungot_character = EOF;
	stream_register(stdin);
	stream_register(stdout);
	stream_register(stderr);
	arena = sbrk((intptr_t)USER_HEAP_INITIAL);

	/* Handles the arena condition. */
	if (arena == (void *)-1)
		__libc_panic("unable to initialize user heap");
	heap_allocator_init(&user_heap, arena, USER_HEAP_INITIAL);
	heap_allocator_set_grow(&user_heap, user_heap_grow, NULL);
	heap_active_set(&user_heap);

	/*
 * Constructors may use pthread state and errno.  Attach the initial
	 * thread before the runtime linker invokes any of them. */
	if (__pthread_initialize_main != NULL)
		__pthread_initialize_main();
#if defined(ZEDBSD_DYNAMIC_LIBC)
	__rtld_exports.startup_init();
#else

	/* Handles the rtld startup init availability. */
	if (__rtld_startup_init != NULL)
		__rtld_startup_init();
#endif
}

/* Supports the environment lock operation. */
static void
environment_lock(
	void)
{
	/* Handles the libc environment lock availability. */
	if (__libc_environment_lock != NULL)
		__libc_environment_lock();
}

/* Supports the environment name operation. */
static int
environment_name(
	const char *entry,
	const char *name)
{
	int function_result;
	size_t length;

	/* Handles the entry availability. */
	if (entry == NULL || name == NULL)
		return 0;
	length = strlen(name);

	/* Computes the function result. */
	function_result = !strncmp(entry, name, length) && entry[length] == '=';

	/* Returns the computed result. */
	return function_result;
}

/* Supports the environment unlock operation. */
static void
environment_unlock(
	void)
{
	/* Handles the libc environment unlock availability. */
	if (__libc_environment_unlock != NULL)
		__libc_environment_unlock();
}

/* Supports the environment value replace operation. */
static void
environment_value_replace(
	char *replacement)
{
	char *previous;

	/* Handles the pthread environment exchange availability. */
	if (__pthread_environment_exchange != NULL)
		previous = __pthread_environment_exchange(replacement);
	else {
		previous = bootstrap_environment_value;
		bootstrap_environment_value = replacement;
	}
	free(previous);
}

/* Supports the call operation. */
static intptr_t
call(
	uint32_t number,
	uintptr_t a0,
	uintptr_t a1,
	uintptr_t a2,
	uintptr_t a3,
	uintptr_t a4,
	uintptr_t a5)
{
	intptr_t function_result;

	/* Obtains the syscall result result. */
	function_result = syscall_result(__syscall6(number, a0, a1, a2, a3, a4, a5));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cancel point operation. */
static void
cancel_point(
	void)
{
	/* Handles the pthread cancel point availability. */
	if (__pthread_cancel_point != NULL)
		__pthread_cancel_point();
}

/* Supports the positional vector io operation. */
static ssize_t
positional_vector_io(
	int fd,
	const struct iovec *iov,
	int count,
	off_t offset,
	int writing)
{
	ssize_t result;
	ssize_t total;
	int index;

	total = 0;

	/* Handles the iov availability. */
	if (count < 0 || count > IOV_MAX || (count != 0 && iov == NULL) ||
	    offset < 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles the iov condition. */
		if (iov[index].iov_len > (size_t)(SSIZE_MAX - total)) {
			/* Handles the total condition. */
			if (total != 0)
				return total;
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		result = writing ? pwrite(fd, iov[index].iov_base,
					  iov[index].iov_len, offset)
				 : pread(fd, iov[index].iov_base,
					 iov[index].iov_len, offset);

		/* Checks the operation result. */
		if (result < 0)
			return total != 0 ? total : -1;
		total += result;
		offset += result;

		/* Checks the operation result. */
		if ((size_t)result != iov[index].iov_len)
			break;
	}

	/* Returns the computed result. */
	return total;
}

/* Supports the ioctl has argument operation. */
static int
ioctl_has_argument(
	unsigned long request)
{
	/* Handles the request condition. */
	if (((request >> 16) & 0x1fffUL) != 0)
		return 1;

	/* Handles the request condition. */
	if ((request >= SIOCGIFNAME && request <= SIOCGIFINDEX) ||
	    request == SIOCGIFSTATS)

		/* Reports operation failure. */
		return 1;

	/* Returns the computed result. */
	return request == SIOCADDRT || request == SIOCDELRT ||
	       request == SIOCGRTENTRY;
}

/* Supports the path limit operation. */
static long
path_limit(
	int name)
{
	/* Dispatch the selected operation case. */
	switch (name) {
	case _PC_LINK_MAX:
		/* Returns the computed result. */
		return 32767;
	case _PC_MAX_CANON:
		/* Returns the computed result. */
		return 255;
	case _PC_MAX_INPUT:
		/* Returns the computed result. */
		return 255;
	case _PC_NAME_MAX:
		/* Returns the computed result. */
		return NAME_MAX;
	case _PC_PATH_MAX:
		/* Returns the computed result. */
		return PATH_MAX;
	case _PC_PIPE_BUF:
		/* Returns the computed result. */
		return 512;
	case _PC_CHOWN_RESTRICTED:
		/* Reports operation failure. */
		return 1;
	case _PC_NO_TRUNC:
		/* Reports operation failure. */
		return 1;
	case _PC_VDISABLE:
		/* Returns the computed result. */
		return 0xff;
	case _PC_TEXTDOMAIN_MAX:
		/* Returns the computed result. */
		return TEXTDOMAIN_MAX;
	default:
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
}

/* Supports the aio submit operation. */
static int
aio_submit(
	struct aiocb *control,
	int writing,
	int notify)
{
	ssize_t result;

	/* Handles the control availability. */
	if (control == NULL || control->aio_reqprio != 0 ||
	    (control->aio_nbytes != 0 && control->aio_buf == NULL)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	control->__aio_submitted = 1;
	control->__aio_returned = 0;
	result = writing ? pwrite(control->aio_fildes,
				  (const void *)control->aio_buf,
				  control->aio_nbytes, control->aio_offset)
			 : pread(control->aio_fildes, (void *)control->aio_buf,
				 control->aio_nbytes, control->aio_offset);
	control->__aio_result = result;
	control->__aio_error = result < 0 ? errno : 0;

	/* Handles the notify condition. */
	if (notify)
		aio_notify(&control->aio_sigevent);

	/* Reports successful completion. */
	return 0;
}

/* zedBSD currently completes AIO requests synchronously.  POSIX permits an operation to have completed by the time the submission function returns. */
static void
aio_notify(
	const struct sigevent *event)
{
	/* Handles the event condition. */
	if (event->sigev_notify == SIGEV_SIGNAL)
		(void)sigqueue(getpid(), event->sigev_signo,
			       event->sigev_value);
}

/* Supports the spawn action add operation. */
static struct __spawn_action *
spawn_action_add(
	posix_spawn_file_actions_t *actions)
{
	/* Handles the actions availability. */
	if (actions == NULL || actions->count >= ZEDBSD_SPAWN_ACTION_MAX)
		return NULL;

	/* Returns the computed result. */
	return &actions->actions[actions->count++];
}

/* Supports the posix spawn common operation. */
static int
posix_spawn_common(
	pid_t *result,
	const char *path,
	const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attr,
	char *const argv[],
	char *const envp[],
	int search)
{
	char *const *environment = envp != NULL ? envp : environ;
	int error_pipe[2], child_error;
	ssize_t count;
	pid_t child;

	child_error = 0;

	/* Validates the command-line arguments. */
	if (result == NULL || path == NULL || argv == NULL)
		return EINVAL;

	/* Handles an operation failure. */
	if (pipe2(error_pipe, O_CLOEXEC) != 0)
		return errno;
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		child_error = errno;
		(void)close(error_pipe[0]);
		(void)close(error_pipe[1]);

		/* Returns the computed result. */
		return child_error;
	}

	/* Checks the child process state. */
	if (child == 0) {
		(void)close(error_pipe[0]);
		child_error = spawn_child_setup(actions, attr);

		/* Handles an operation failure. */
		if (child_error == 0) {
			/* Handles the search condition. */
			if (search)
				(void)spawn_exec_search(
				    path, argv, (char *const *)environment);
			else
				(void)execve(path, argv,
					     (char *const *)environment);
			child_error = errno;
		}
		(void)write(error_pipe[1], &child_error, sizeof(child_error));
		_exit(127);
	}
	(void)close(error_pipe[1]);
	do {
		count = read(error_pipe[0], &child_error, sizeof(child_error));
	} while (count < 0 && errno == EINTR);
	(void)close(error_pipe[0]);

	/* Checks the remaining item count. */
	if (count > 0) {
		(void)waitpid(child, NULL, 0);

		/* Returns the computed result. */
		return child_error != 0 ? child_error : EIO;
	}

	/* Checks the remaining item count. */
	if (count < 0) {
		child_error = errno;
		(void)waitpid(child, NULL, 0);

		/* Returns the computed result. */
		return child_error;
	}
	*result = child;
	/* Reports successful completion. */
	return 0;
}

/* Supports the spawn child setup operation. */
static int
spawn_child_setup(
	const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attr)
{
	struct sigaction action;
	int e;
	int fd;
	const struct __spawn_action *a;
	unsigned index;

	/* Handles the attr availability. */
	if (attr != NULL) {
		/* Handles a failed setegid operation. */
		if ((attr->flags & POSIX_SPAWN_RESETIDS) != 0 &&
		    (setegid(getgid()) != 0 || seteuid(getuid()) != 0))

			/* Returns the computed result. */
			return errno;

		/* Handles a failed setsid operation. */
		if ((attr->flags & POSIX_SPAWN_SETSID) != 0 && setsid() < 0)
			return errno;

		/* Handles a failed setpgid operation. */
		if ((attr->flags & POSIX_SPAWN_SETPGROUP) != 0 &&
		    setpgid(0, attr->pgroup) != 0)

			/* Returns the computed result. */
			return errno;

		/* Handles a failed sigprocmask operation. */
		if ((attr->flags & POSIX_SPAWN_SETSIGMASK) != 0 &&
		    sigprocmask(SIG_SETMASK, &attr->sigmask, NULL) != 0)

			/* Returns the computed result. */
			return errno;

		/* Handles the attr condition. */
		if ((attr->flags & POSIX_SPAWN_SETSIGDEF) != 0)

			/* Process each remaining element. */
			for (index = 1; index <= SIGRTMAX; index++)

				/* Handles a failed sigismember operation. */
				if (sigismember(&attr->sigdefault,
						(int)index) == 1) {

					memset(&action, 0, sizeof(action));
					action.sa_handler = SIG_DFL;

					/* Handles a failed sigaction operation. */
					if (sigaction((int)index, &action,
						      NULL) != 0)

						/* Returns the computed result. */
						return errno;
				}
	}

	/* Handles the actions availability. */
	if (actions == NULL)
		return 0;

	/* Process each remaining element. */
	for (index = 0; index < actions->count; index++) {
				a = &actions->actions[index];

		/* Handles the a condition. */
		if (a->operation == SPAWN_ACTION_CLOSE) {
			/* Handles the reported system error. */
			if (close(a->descriptor) != 0 && errno != EBADF)
				return errno;
		} else if (a->operation == SPAWN_ACTION_DUP2) {
			/* Handles a failed dup2 operation. */
			if (dup2(a->descriptor, a->new_descriptor) < 0)
				return errno;
		} else if (a->operation == SPAWN_ACTION_OPEN) {
						fd = open(a->path, a->flags, a->mode);

			/* Checks the file descriptor. */
			if (fd < 0)
				return errno;

			/* Handles a failed dup2 operation. */
			if (fd != a->descriptor &&
			    dup2(fd, a->descriptor) < 0) {
								e = errno;
				(void)close(fd);

				/* Returns the computed result. */
				return e;
			}

			/* Checks the file descriptor. */
			if (fd != a->descriptor)
				(void)close(fd);
		} else if (a->operation == SPAWN_ACTION_CHDIR) {
			/* Handles a failed chdir operation. */
			if (chdir(a->path) != 0)
				return errno;
		} else if (a->operation == SPAWN_ACTION_FCHDIR) {
			/* Handles a failed fchdir operation. */
			if (fchdir(a->descriptor) != 0)
				return errno;
		} else

			/* Returns the computed result. */
			return EINVAL;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the spawn exec search operation. */
static int
spawn_exec_search(
	const char *file,
	char *const argv[],
	char *const environment[])
{
	int function_result;
	const char *colon;
	size_t file_length;
	const char *path, *at;
	char candidate[PATH_MAX];
	int saw_access_error;

	saw_access_error = 0;

	/* Handles the file availability. */
	if (file == NULL || *file == '\0') {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed strchr operation. */
	if (strchr(file, '/') != NULL) {
		/* Obtains the execve result. */
		function_result = execve(file, argv, environment);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each element required by the operation. */
	path = spawn_environment_path(environment);
	for (at = path;;) {
				colon = strchr(at, ':');
		size_t directory_length =
		    colon != NULL ? (size_t)(colon - at) : strlen(at);
				file_length = strlen(file);

		/* Handles the directory length condition. */
		if (directory_length + file_length + 2U <= sizeof(candidate)) {
			/* Handles the directory length condition. */
			if (directory_length != 0)
				memcpy(candidate, at, directory_length);
			else {
				candidate[0] = '.';
				directory_length = 1;
			}
			candidate[directory_length] = '/';
			memcpy(candidate + directory_length + 1U, file,
			       file_length + 1U);
			execve(candidate, argv, environment);

			/* Handles the reported system error. */
			if (errno == EACCES)
				saw_access_error = 1;
			else if (errno != ENOENT && errno != ENOTDIR)

				/* Reports operation failure. */
				return -1;
		}

		/* Handles the colon availability. */
		if (colon == NULL)
			break;
		at = colon + 1;
	}
	errno = saw_access_error ? EACCES : ENOENT;

	/* Reports operation failure. */
	return -1;
}

/* Supports the spawn environment path operation. */
static const char *
spawn_environment_path(
	char *const environment[])
{
	unsigned index;

	/* Handles the environment availability. */
	if (environment != NULL)

		/* Process each remaining element. */
		for (index = 0; environment[index] != NULL; index++)

			/* Selects the matching prefix. */
			if (strncmp(environment[index], "PATH=", 5) == 0)
				return environment[index] + 5;

	/* Returns the computed result. */
	return "/bin:/usr/bin";
}

/* Supports the exec search operation. */
static int
exec_search(
	const char *file,
	char *const argv[],
	char *const envp[])
{
	int function_result;
	const char *colon;
	size_t file_length;
	const char *path, *at;
	char candidate[PATH_MAX];
	int saw_access_error;

	saw_access_error = 0;

	/* Handles the file availability. */
	if (file == NULL || *file == '\0') {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed strchr operation. */
	if (strchr(file, '/') != NULL) {
		execve(file, argv, envp);

		/* Computes the function result. */
		function_result = errno == ENOEXEC ? exec_with_shell(file, argv, envp)
					: -1;

		/* Returns the computed result. */
		return function_result;
	}
	path = getenv("PATH");

	/* Handles the path availability. */
	if (path == NULL)

	/* Process each element required by the operation. */
		path = "/bin:/usr/bin";
	for (at = path;;) {
				colon = strchr(at, ':');
		size_t directory_length =
		    colon != NULL ? (size_t)(colon - at) : strlen(at);
				file_length = strlen(file);

		/* Handles the directory length condition. */
		if (directory_length + file_length + 2U <= sizeof(candidate)) {
			/* Handles the directory length condition. */
			if (directory_length != 0)
				memcpy(candidate, at, directory_length);
			else {
				candidate[0] = '.';
				directory_length = 1;
			}
			candidate[directory_length] = '/';
			memcpy(candidate + directory_length + 1U, file,
			       file_length + 1U);
			execve(candidate, argv, envp);

			/* Handles the reported system error. */
			if (errno == ENOEXEC) {
				/* Obtains the exec with shell result. */
				function_result = exec_with_shell(candidate, argv, envp);

				/* Returns the computed result. */
				return function_result;
			}

			/* Handles the reported system error. */
			if (errno == EACCES)
				saw_access_error = 1;
			else if (errno != ENOENT && errno != ENOTDIR)

				/* Reports operation failure. */
				return -1;
		}

		/* Handles the colon availability. */
		if (colon == NULL)
			break;
		at = colon + 1;
	}
	errno = saw_access_error ? EACCES : ENOENT;

	/* Reports operation failure. */
	return -1;
}

/* Supports the exec with shell operation. */
static int
exec_with_shell(
	const char *path,
	char *const argv[],
	char *const envp[])
{
	int function_result;
	size_t index_for;
	char *shell_argv[ZEDBSD_SPAWN_ARG_MAX + 2U];
	size_t count;

	count = 0;
	shell_argv[count++] = (char *)"sh";
	shell_argv[count++] = (char *)(uintptr_t)path;

	/* Validates the command-line arguments. */
	if (argv != NULL)

		/* Process each remaining command-line operand. */
		for (index_for = 1; argv[index_for] != NULL; index_for++) {
			/* Checks the remaining item count. */
			if (count == ZEDBSD_SPAWN_ARG_MAX + 1U) {
				errno = E2BIG;

				/* Reports operation failure. */
				return -1;
			}
			shell_argv[count++] = argv[index_for];
		}
	shell_argv[count] = NULL;

	/* Obtains the execve result. */
	function_result = execve("/bin/sh", shell_argv, envp);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the exec varargs operation. */
static int
exec_varargs(
	const char *path,
	const char *first,
	va_list arguments,
	int search,
	int explicit_environment)
{
	int function_result;
	char *argv[ZEDBSD_SPAWN_ARG_MAX + 1U];
	char *const *envp = environ;
	size_t count;
	const char *argument;

	count = 0;
	argument = first;

	/* Continue while the operation condition remains true. */
	while (argument != NULL) {
		/* Checks the remaining item count. */
		if (count == ZEDBSD_SPAWN_ARG_MAX) {
			errno = E2BIG;

			/* Reports operation failure. */
			return -1;
		}
		argv[count++] = (char *)(uintptr_t)argument;
		argument = va_arg(arguments, const char *);
	}
	argv[count] = NULL;

	/* Handles the explicit environment condition. */
	if (explicit_environment)
		envp = va_arg(arguments, char *const *);

	/* Computes the function result. */
	function_result = search ? exec_search(path, argv, (char *const *)envp)
		      : execve(path, argv, (char *const *)envp);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the stream register operation. */
static void
stream_register(
	FILE *stream)
{
	stream_registry_acquire();
	stream->registry_next = stream_registry;
	stream_registry = stream;
	stream_registry_release();
}

/* Supports the stream registry acquire operation. */
static void
stream_registry_acquire(
	void)
{
	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(&stream_registry_lock, 1U,
				   __ATOMIC_ACQUIRE) != 0)
		;
}

/* Supports the stream registry release operation. */
static void
stream_registry_release(
	void)
{
	__atomic_store_n(&stream_registry_lock, 0U, __ATOMIC_RELEASE);
}

/* Supports the stream unregister locked operation. */
static void
stream_unregister_locked(
	FILE *stream)
{
	FILE **link;

	/* Process each element required by the operation. */
	for (link = &stream_registry; *link != NULL;
	     link = &(*link)->registry_next)

		/* Handles the link condition. */
		if (*link == stream) {
			*link = stream->registry_next;
			stream->registry_next = NULL;

			/* Returns the computed result. */
			return;
		}
}

/* Supports the stream flush locked operation. */
static int
stream_flush_locked(
	FILE *stream)
{
	size_t written;
	off_t unread;

	/* Handles the stream condition. */
	if (stream->last_operation == 2 && stream->buffer_length != 0) {
		/* Handles the end-of-file condition. */
		if (stream_write_direct(stream, stream->buffer,
					stream->buffer_length,
					&written) == EOF) {
			/* Handles the written condition. */
			if (written != 0) {
				memmove(stream->buffer,
					stream->buffer + written,
					stream->buffer_length - written);
				stream->buffer_length -= written;
			}

			/* Reports that no result is available. */
			return EOF;
		}
		stream->buffer_length = 0;
	} else if (stream->last_operation == 1 &&
		   stream->buffer_length > stream->buffer_start) {
				unread = (off_t)(stream->buffer_length - stream->buffer_start);
		off_t result =
		    stream->cookie_seek != NULL
			? stream->cookie_seek(stream->context, -unread,
					      SEEK_CUR)
			: lseek(stream_fd(stream), -unread, SEEK_CUR);

		/* Checks the operation result. */
		if (result < 0) {
			stream->error = 1;

			/* Reports that no result is available. */
			return EOF;
		}
		stream->buffer_start = stream->buffer_length = 0;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the stream write direct operation. */
static int
stream_write_direct(
	FILE *stream,
	const unsigned char *buffer,
	size_t length,
	size_t *written)
{
	size_t request;
	ssize_t result;

	/* Process each remaining element. */
	*written = 0;
	while (*written < length) {

		request = length - *written;

		/* Handles the cookie write availability. */
		if (stream->cookie_write != NULL) {
			/* Handles the request condition. */
			if (request > (size_t)INT_MAX)
				request = INT_MAX;
			result = stream->cookie_write(
			    stream->context, (const char *)buffer + *written,
			    (int)request);
		} else {
			result = write(stream_fd(stream), buffer + *written,
				       request);
		}

		/* Checks the operation result. */
		if (result > 0) {
			*written += (size_t)result;
			continue;
		}

		/* Handles the reported system error. */
		if (result < 0 && errno == EINTR)
			continue;
		stream->error = 1;

		/* Reports that no result is available. */
		return EOF;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the stream fd operation. */
static int
stream_fd(
	FILE *stream)
{
	int function_result;

	/* Obtains the fileno result. */
	function_result = fileno(stream);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the stream enter operation. */
static void
stream_enter(
	FILE *stream,
	int *cancel_state)
{
	(void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, cancel_state);
	flockfile(stream);
}

/* Supports the stream leave operation. */
static void
stream_leave(
	FILE *stream,
	int cancel_state)
{
	funlockfile(stream);
	(void)pthread_setcancelstate(cancel_state, NULL);
}

/* Supports the stream read direct operation. */
static ssize_t
stream_read_direct(
	FILE *stream,
	void *buffer,
	size_t length)
{
	ssize_t function_result;

	/* Handles the cookie read availability. */
	if (stream->cookie_read != NULL) {
		/* Checks the current data length. */
		if (length > (size_t)INT_MAX)
			length = INT_MAX;

		/* Computes the function result. */
		function_result = stream->cookie_read(stream->context, buffer,
					   (int)length);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the read result. */
	function_result = read(stream_fd(stream), buffer, length);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the stream ensure buffer operation. */
static int
stream_ensure_buffer(
	FILE *stream)
{
	/* Handles the buffer availability. */
	if (stream->buffering_mode == _IONBF || stream->buffer != NULL)
		return 0;
	stream->buffer = malloc(BUFSIZ);

	/* Handles the buffer availability. */
	if (stream->buffer == NULL) {
		stream->error = 1;
		errno = ENOMEM;

		/* Reports operation failure. */
		return -1;
	}
	stream->buffer_size = BUFSIZ;
	stream->buffer_owned = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the floor div operation. */
static int64_t
floor_div(
	int64_t value,
	int64_t divisor)
{
	int64_t quotient;

	quotient = value / divisor;

	/* Validates the current value. */
	if (value % divisor < 0)
		quotient--;

	/* Returns the computed result. */
	return quotient;
}

/* Supports the civil from days operation. */
static void
civil_from_days(
	int64_t days,
	int *year,
	unsigned *month,
	unsigned *day,
	unsigned *year_day)
{
	int64_t era, civil_year;
	unsigned day_of_era, year_of_era, day_of_year, month_prime;

	days += 719468;
	era = floor_div(days, 146097);
	day_of_era = (unsigned)(days - era * 146097);
	year_of_era = (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
		       day_of_era / 146096U) /
		      365U;
	civil_year = (int64_t)year_of_era + era * 400;
	day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U -
				    year_of_era / 100U);
	month_prime = (5U * day_of_year + 2U) / 153U;
	*day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
	*month = month_prime + (month_prime < 10U ? 3U : (unsigned)-9);
	civil_year += *month <= 2U;
	*year = (int)civil_year;
	*year_day = (unsigned)(days_from_civil(civil_year, *month, *day) -
			       days_from_civil(civil_year, 1, 1));
}

/* Gregorian calendar conversion, with day zero equal to 1970-01-01. */
static int64_t
days_from_civil(
	int64_t year,
	unsigned month,
	unsigned day)
{
	int64_t era;
	unsigned year_of_era, day_of_year, day_of_era;

	year -= month <= 2;
	era = floor_div(year, 400);
	year_of_era = (unsigned)(year - era * 400);
	day_of_year =
	    (153U * (month + (month > 2 ? (unsigned)-3 : 9U)) + 2U) / 5U + day -
	    1U;
	day_of_era = year_of_era * 365U + year_of_era / 4U -
		     year_of_era / 100U + day_of_year;

	/* Returns the computed result. */
	return era * 146097 + (int64_t)day_of_era - 719468;
}

/* Supports the timezone name operation. */
static const char *
timezone_name(
	const char *text,
	char *name,
	size_t capacity)
{
	size_t length;
	int quoted = *text == '<';

	length = 0;

	/* Handles the quoted condition. */
	if (quoted)
		text++;

	/* Continue while the operation condition remains true. */
	while (*text != '\0' && (quoted ? *text != '>'
					: ((*text >= 'A' && *text <= 'Z') ||
					   (*text >= 'a' && *text <= 'z')))) {
		/* Checks the current data length. */
		if (length + 1U < capacity)
			name[length++] = *text;
		text++;
	}

	/* Handles the quoted condition. */
	if (quoted && *text == '>')
		text++;

	/* Checks the current data length. */
	if (length < 3U)
		return NULL;
	name[length] = '\0';

	/* Returns the computed result. */
	return text;
}

/* Supports the timezone offset operation. */
static const char *
timezone_offset(
	const char *text,
	long *east)
{
	long sign, hours, minutes, seconds;

	sign = 1;
	hours = 0;
	minutes = 0;
	seconds = 0;

	/* Validates the current text. */
	if (*text == '+' || *text == '-') {
		/* Validates the current text. */
		if (*text++ == '-')
			sign = -1;
	}

	/* Validates the current text. */
	if (*text < '0' || *text > '9')
		return NULL;

	/* Continue while the operation condition remains true. */
	while (*text >= '0' && *text <= '9')
		hours = hours * 10 + (*text++ - '0');

	/* Validates the current text. */
	if (*text == ':') {
		text++;

		/* Validates the current text. */
		if (*text < '0' || *text > '9')
			return NULL;

		/* Continue while the operation condition remains true. */
		while (*text >= '0' && *text <= '9')
			minutes = minutes * 10 + (*text++ - '0');

		/* Validates the current text. */
		if (*text == ':') {
			text++;

			/* Validates the current text. */
			if (*text < '0' || *text > '9')
				return NULL;

			/* Continue while the operation condition remains true. */
			while (*text >= '0' && *text <= '9')
				seconds = seconds * 10 + (*text++ - '0');
		}
	}

	/* Handles the hours condition. */
	if (hours > 24 || minutes > 59 || seconds > 59)
		return NULL;
	*east = -sign * (hours * 3600 + minutes * 60 + seconds);
	/* Returns the computed result. */
	return text;
}

/* Supports the timezone rule parse operation. */
static const char *
timezone_rule_parse(
	const char *text,
	struct timezone_rule *rule)
{
	long east;

	/* Validates the current text. */
	if (*text == 'M') {
		rule->kind = TZ_RULE_MONTH;
		text++;

		/* Handles a failed timezone number operation. */
		if ((text = timezone_number(text, &rule->first)) == NULL ||
		    *text++ != '.' ||
		    (text = timezone_number(text, &rule->second)) == NULL ||
		    *text++ != '.' ||
		    (text = timezone_number(text, &rule->third)) == NULL ||
		    rule->first < 1 || rule->first > 12 || rule->second < 1 ||
		    rule->second > 5 || rule->third < 0 || rule->third > 6)

			/* Reports that no result is available. */
			return NULL;
	} else {
		rule->kind = *text == 'J' ? TZ_RULE_JULIAN : TZ_RULE_DAY;

		/* Validates the current text. */
		if (*text == 'J')
			text++;

		/* Handles a failed timezone number operation. */
		if ((text = timezone_number(text, &rule->first)) == NULL ||
		    (rule->kind == TZ_RULE_JULIAN &&
		     (rule->first < 1 || rule->first > 365)) ||
		    (rule->kind == TZ_RULE_DAY &&
		     (rule->first < 0 || rule->first > 365)))

			/* Reports that no result is available. */
			return NULL;
	}
	rule->seconds = 2 * 3600;

	/* Validates the current text. */
	if (*text == '/') {
		text++;

		/*
 * Offset parsing uses the opposite sign convention from rule
		 * times. */
		if ((text = timezone_offset(text, &east)) == NULL)
			return NULL;
		rule->seconds = (int)-east;
	}

	/* Returns the computed result. */
	return text;
}

/* Supports the timezone number operation. */
static const char *
timezone_number(
	const char *text,
	int *number)
{
	int value;

	value = 0;

	/* Validates the current text. */
	if (*text < '0' || *text > '9')
		return NULL;

	/* Continue while the operation condition remains true. */
	while (*text >= '0' && *text <= '9')
		value = value * 10 + (*text++ - '0');
	*number = value;
	/* Returns the computed result. */
	return text;
}

/* Supports the timezone is daylight operation. */
static int
timezone_is_daylight(
	time_t value)
{
	struct tm standard;
	time_t local_standard;
	int year, start_yday, end_yday;
	int64_t year_start, start, end;

	local_standard = value + timezone_east;

	/* Handles a failed gmtime r operation. */
	if (!timezone_has_daylight ||
	    gmtime_r(&local_standard, &standard) == NULL)

		/* Reports successful completion. */
		return 0;
	year = standard.tm_year + 1900;
	year_start = days_from_civil(year, 1, 1) * 86400;
	start_yday = timezone_rule_yday(year, &timezone_start);
	end_yday = timezone_rule_yday(year, &timezone_end);
	start = year_start + (int64_t)start_yday * 86400 +
		timezone_start.seconds - timezone_east;
	end = year_start + (int64_t)end_yday * 86400 + timezone_end.seconds -
	      timezone_daylight_east;

	/* Returns the computed result. */
	return start < end ? value >= start && value < end
			   : value >= start || value < end;
}

/* Supports the timezone rule yday operation. */
static int
timezone_rule_yday(
	int year,
	const struct timezone_rule *rule)
{
	int function_result;
	int next_month;
	int next_year;
	int month_days;
	int64_t first;
	int weekday;
	int day;

	/* Handles the rule condition. */
	if (rule->kind == TZ_RULE_DAY)
		return rule->first;

	/* Handles the rule condition. */
	if (rule->kind == TZ_RULE_JULIAN) {
		/* Computes the function result. */
		function_result = rule->first - 1 +
		       (calendar_leap_year(year) && rule->first >= 60);

		/* Returns the computed result. */
		return function_result;
	}

	first = days_from_civil(year, (unsigned)rule->first, 1);
	weekday = (int)((first + 4) % 7);

	/* Handles the weekday condition. */
	if (weekday < 0)
		weekday += 7;
	day = 1 + (rule->third - weekday + 7) % 7 +
	      7 * (rule->second - 1);

	/* Handles the rule condition. */
	if (rule->second == 5) {
						next_month = rule->first == 12 ? 1 : rule->first + 1;
						next_year = rule->first == 12 ? year + 1 : year;
					month_days = (int)(days_from_civil(next_year,
					  (unsigned)next_month, 1) -
			  first);

		/* Continue while the operation condition remains true. */
		while (day + 7 <= month_days)
			day += 7;
	}

	/* Computes the function result. */
	function_result = (int)(first - days_from_civil(year, 1, 1)) + day - 1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the calendar leap year operation. */
static int
calendar_leap_year(
	int year)
{
	/* Returns the computed result. */
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/* Supports the strftime append operation. */
static int
strftime_append(
	char **output,
	size_t *remaining,
	const char *text)
{
	size_t length;

	length = strlen(text);

	/* Checks the current data length. */
	if (length >= *remaining)
		return -1;
	memcpy(*output, text, length);
	*output += length;
	*remaining -= length;
	/* Reports successful completion. */
	return 0;
}

/* Supports the strftime number operation. */
static int
strftime_number(
	char **output,
	size_t *remaining,
	int value,
	int width,
	char padding)
{
	int function_result;
	char buffer[32];
	int length;

	length = snprintf(buffer, sizeof(buffer),
			      padding == '0' ? "%0*d" : "%*d", width, value);

	/* Computes the function result. */
	function_result = length < 0 ? -1 : strftime_append(output, remaining, buffer);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the iso week operation. */
static void
iso_week(
	const struct tm *value,
	int *year,
	int *week)
{
	int iso_weekday = value->tm_wday == 0 ? 7 : value->tm_wday;

	*year = value->tm_year + 1900;
	*week = (value->tm_yday + 10 - iso_weekday) / 7;
	/* Handles the week condition. */
	if (*week < 1) {
		(*year)--;
		*week = iso_weeks_in_year(*year);
	} else if (*week > iso_weeks_in_year(*year)) {
		(*year)++;
		*week = 1;
	}
}

/* Supports the iso weeks in year operation. */
static int
iso_weeks_in_year(
	int year)
{
	int function_result;
	int64_t first_day;
	int weekday;

	first_day = days_from_civil(year, 1, 1);
	weekday = (int)((first_day + 4) % 7);

	/* Handles the weekday condition. */
	if (weekday < 0)
		weekday += 7;

	/* Computes the function result. */
	function_result = weekday == 4 || (weekday == 3 && calendar_leap_year(year)) ? 53
									  : 52;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the user heap grow operation. */
static size_t
user_heap_grow(
	void *context,
	void *end,
	size_t minimum)
{
	size_t amount;
	void *old;

	(void)context;

	/* Handles the minimum condition. */
	if (minimum > SIZE_MAX - 65535U)
		return 0;
	amount = (minimum + 65535U) & ~(size_t)65535U;
	old = sbrk((intptr_t)amount);

	/* Handles the old condition. */
	if (old == (void *)-1 || old != end)
		return 0;

	/* Returns the computed result. */
	return amount;
}
