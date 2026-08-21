/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/libc/syscall.h"
#include "libc/heap.h"
#include "libc/stdio-internal.h"

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
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
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
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ZEDBSD_USER_PAGE_SIZE
#define ZEDBSD_USER_PAGE_SIZE 4096
#endif

char **environ;
#define ENVIRONMENT_MAX 64U
static char *environment_entries[ENVIRONMENT_MAX + 1U];
static unsigned char environment_owned[ENVIRONMENT_MAX];
extern void zedbsd_pthread_cancel_point(void) __attribute__((weak));
extern void zedbsd_pthread_fork_prepare(void) __attribute__((weak));
extern void zedbsd_pthread_fork_parent(void) __attribute__((weak));
extern void zedbsd_pthread_fork_child(void) __attribute__((weak));
extern void zedbsd_pthread_initialize_main(void) __attribute__((weak));
extern void zedbsd_libc_environment_lock(void) __attribute__((weak));
extern void zedbsd_libc_environment_unlock(void) __attribute__((weak));
extern char *zedbsd_pthread_environment_exchange(char *)
	__attribute__((weak));
#if !defined(ZEDBSD_DYNAMIC_LIBC)
extern void __zedbsd_rtld_process_fini(void) __attribute__((weak));
extern void __zedbsd_rtld_startup_init(void) __attribute__((weak));
#endif
static void cancel_point(void)
{ if (zedbsd_pthread_cancel_point != NULL) zedbsd_pthread_cancel_point(); }
static void environment_lock(void)
{ if (zedbsd_libc_environment_lock != NULL) zedbsd_libc_environment_lock(); }
static void environment_unlock(void)
{ if (zedbsd_libc_environment_unlock != NULL) zedbsd_libc_environment_unlock(); }

static char *bootstrap_environment_value;

static void
environment_value_replace(char *replacement)
{
	char *previous;

	if (zedbsd_pthread_environment_exchange != NULL)
		previous = zedbsd_pthread_environment_exchange(replacement);
	else {
		previous = bootstrap_environment_value;
		bootstrap_environment_value = replacement;
	}
	free(previous);
}

static int environment_name(const char *entry, const char *name)
{
	size_t length;
	if (entry == NULL || name == NULL)
		return 0;
	length = strlen(name);
	return !strncmp(entry, name, length) && entry[length] == '=';
}

char *getenv(const char *name)
{
	unsigned i;
	char *result = NULL, *snapshot = NULL;
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL)
		return NULL;
	environment_lock();
	for (i = 0; environ != NULL && environ[i] != NULL; i++)
		if (environment_name(environ[i], name)) {
			result = strchr(environ[i], '=') + 1;
			snapshot = strdup(result);
			break;
		}
	environment_unlock();
	/* Replacing the calling thread's snapshot, including on a miss, defines
	 * the lifetime as lasting until its next environment operation. */
	environment_value_replace(snapshot);
	return snapshot;
}

int setenv(const char *name, const char *value, int overwrite)
{
	size_t name_length, value_length;
	char *entry;
	unsigned i, empty = ENVIRONMENT_MAX;
	if (name == NULL || value == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL; return -1;
	}
	environment_lock();
	for (i = 0; i < ENVIRONMENT_MAX; i++) {
		if (environ[i] == NULL && empty == ENVIRONMENT_MAX) empty = i;
		if (environment_name(environ[i], name)) {
			if (!overwrite) { environment_unlock(); return 0; }
			empty = i; break;
		}
	}
	if (empty == ENVIRONMENT_MAX) {
		environment_unlock(); errno = ENOSPC; return -1;
	}
	name_length = strlen(name); value_length = strlen(value);
	if (name_length > SIZE_MAX - value_length - 2U) {
		environment_unlock(); errno = ENOMEM; return -1;
	}
	entry = malloc(name_length + value_length + 2U);
	if (entry == NULL) {
		environment_unlock(); errno = ENOMEM; return -1;
	}
	memcpy(entry, name, name_length); entry[name_length] = '=';
	memcpy(entry + name_length + 1U, value, value_length + 1U);
	if (environment_owned[empty]) free(environ[empty]);
	environ[empty] = entry; environment_owned[empty] = 1U;
	environ[empty + 1U] = NULL;
	environment_unlock();
	environment_value_replace(NULL);
	return 0;
}

int unsetenv(const char *name)
{
	unsigned i;
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL; return -1;
	}
	environment_lock();
	for (i = 0; i < ENVIRONMENT_MAX && environ[i] != NULL; )
		if (environment_name(environ[i], name)) {
			unsigned j;
			if (environment_owned[i]) free(environ[i]);
			for (j = i; j < ENVIRONMENT_MAX; j++) {
				environ[j] = environ[j + 1U];
				environment_owned[j] = j + 1U < ENVIRONMENT_MAX ?
					environment_owned[j + 1U] : 0U;
			}
		} else {
			i++;
		}
	environment_unlock();
	environment_value_replace(NULL);
	return 0;
}

int
putenv(char *entry)
{
	char *equal;
	size_t name_length;
	unsigned i, empty = ENVIRONMENT_MAX;

	if (entry == NULL || entry[0] == '\0' ||
	    (equal = strchr(entry, '=')) == NULL || equal == entry) {
		errno = EINVAL;
		return -1;
	}
	name_length = (size_t)(equal - entry);
	environment_lock();
	for (i = 0; i < ENVIRONMENT_MAX; i++) {
		if (environ[i] == NULL && empty == ENVIRONMENT_MAX)
			empty = i;
		if (environ[i] != NULL &&
		    !strncmp(environ[i], entry, name_length) &&
		    environ[i][name_length] == '=') {
			empty = i;
			break;
		}
	}
	if (empty == ENVIRONMENT_MAX) {
		environment_unlock();
		errno = ENOSPC;
		return -1;
	}
	if (environment_owned[empty])
		free(environ[empty]);
	environ[empty] = entry;
	environment_owned[empty] = 0;
	environ[empty + 1U] = NULL;
	environment_unlock();
	environment_value_replace(NULL);
	return 0;
}

int
clearenv(void)
{
	unsigned i;

	environment_lock();
	for (i = 0; i < ENVIRONMENT_MAX && environ[i] != NULL; i++) {
		if (environment_owned[i])
			free(environ[i]);
		environ[i] = NULL;
		environment_owned[i] = 0;
	}
	environment_entries[0] = NULL;
	environ = environment_entries;
	environment_unlock();
	environment_value_replace(NULL);
	return 0;
}

intptr_t zedbsd_syscall_result(intptr_t result)
{
	if (result < 0 && result >= -4095) {
		errno = (int)-result;
		return -1;
	}
	return result;
}

static intptr_t call(uint32_t number, uintptr_t a0, uintptr_t a1,
		     uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return zedbsd_syscall_result(zedbsd_syscall6(number, a0, a1, a2, a3,
		a4, a5));
}

void _exit(int status)
{
	(void)zedbsd_syscall6(ZEDBSD_SYS_exit, (uintptr_t)status, 0, 0, 0, 0, 0);
	for (;;) ;
}

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
	}
	return (int)call(ZEDBSD_SYS_open, (uintptr_t)path, flags, mode, 0, 0, 0);
}
int openat(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
	}
	return (int)call(ZEDBSD_SYS_openat, dirfd, (uintptr_t)path, flags,
	    mode, 0, 0);
}
int close(int fd) { return (int)call(ZEDBSD_SYS_close, fd, 0, 0, 0, 0, 0); }
int dup(int fd) { return (int)call(ZEDBSD_SYS_dup, fd, 0, 0, 0, 0, 0); }
int dup2(int oldfd, int newfd) { return (int)call(ZEDBSD_SYS_dup2, oldfd, newfd, 0, 0, 0, 0); }
int dup3(int oldfd, int newfd, int flags) { return (int)call(ZEDBSD_SYS_dup3, oldfd, newfd, flags, 0, 0, 0); }
int fcntl(int fd, int command, ...)
{
	va_list ap;
	intptr_t argument = 0;
	struct flock *native = NULL;
	struct zedbsd_flock_request request;
	if (command == F_DUPFD || command == F_DUPFD_CLOEXEC ||
	    command == F_SETFD || command == F_SETFL) {
		va_start(ap, command);
		argument = va_arg(ap, int);
		va_end(ap);
	} else if (command == F_GETLK || command == F_SETLK ||
	    command == F_SETLKW) {
		va_start(ap, command);
		native = va_arg(ap, struct flock *);
		va_end(ap);
		if (native == NULL) { errno = EFAULT; return -1; }
		memset(&request, 0, sizeof(request));
		request.type = native->l_type;
		request.whence = native->l_whence;
		request.start = native->l_start;
		request.length = native->l_len;
		request.pid = native->l_pid;
		argument = (intptr_t)&request;
	}
	{
		int result = (int)call(ZEDBSD_SYS_fcntl, fd, command, argument,
		    0, 0, 0);
		if (result == 0 && command == F_GETLK) {
			native->l_type = request.type;
			native->l_whence = request.whence;
			native->l_start = (off_t)request.start;
			native->l_len = (off_t)request.length;
			native->l_pid = request.pid;
		}
		return result;
	}
}
int lockf(int fd, int command, off_t length)
{
	struct flock lock;
	int operation, result;
	memset(&lock, 0, sizeof(lock));
	lock.l_type = command == F_ULOCK ? F_UNLCK : F_WRLCK;
	lock.l_whence = SEEK_CUR;
	lock.l_len = length;
	if (command == F_LOCK) operation = F_SETLKW;
	else if (command == F_ULOCK || command == F_TLOCK) operation = F_SETLK;
	else if (command == F_TEST) operation = F_GETLK;
	else { errno = EINVAL; return -1; }
	result = fcntl(fd, operation, &lock);
	if (result == 0 && command == F_TEST && lock.l_type != F_UNLCK) {
		errno = EACCES;
		return -1;
	}
	return result;
}
int pipe2(int result[2], int flags) { return (int)call(ZEDBSD_SYS_pipe2, (uintptr_t)result, flags, 0, 0, 0, 0); }
int pipe(int result[2]) { return pipe2(result, 0); }
ssize_t read(int fd, void *p, size_t n) { ssize_t r; cancel_point(); r = (ssize_t)call(ZEDBSD_SYS_read, fd, (uintptr_t)p, n, 0, 0, 0); cancel_point(); return r; }
ssize_t write(int fd, const void *p, size_t n) { ssize_t r; cancel_point(); r = (ssize_t)call(ZEDBSD_SYS_write, fd, (uintptr_t)p, n, 0, 0, 0); cancel_point(); return r; }
ssize_t pread(int fd, void *p, size_t n, off_t offset) { return (ssize_t)call(ZEDBSD_SYS_pread, fd, (uintptr_t)p, n, offset, 0, 0); }
ssize_t pwrite(int fd, const void *p, size_t n, off_t offset) { return (ssize_t)call(ZEDBSD_SYS_pwrite, fd, (uintptr_t)p, n, offset, 0, 0); }
ssize_t readv(int fd, const struct iovec *iov, int count) { return (ssize_t)call(ZEDBSD_SYS_readv, fd, (uintptr_t)iov, count, 0, 0, 0); }
ssize_t writev(int fd, const struct iovec *iov, int count) { return (ssize_t)call(ZEDBSD_SYS_writev, fd, (uintptr_t)iov, count, 0, 0, 0); }
static ssize_t positional_vector_io(int fd, const struct iovec *iov,
	int count, off_t offset, int writing)
{
	ssize_t total = 0;
	int index;
	if (count < 0 || count > IOV_MAX || (count != 0 && iov == NULL) ||
	    offset < 0) { errno = EINVAL; return -1; }
	for (index = 0; index < count; index++) {
		ssize_t result;
		if (iov[index].iov_len > (size_t)(SSIZE_MAX - total)) {
			if (total != 0) return total;
			errno = EINVAL; return -1;
		}
		result = writing ? pwrite(fd, iov[index].iov_base,
		    iov[index].iov_len, offset) : pread(fd, iov[index].iov_base,
		    iov[index].iov_len, offset);
		if (result < 0)
			return total != 0 ? total : -1;
		total += result;
		offset += result;
		if ((size_t)result != iov[index].iov_len)
			break;
	}
	return total;
}
int creat(const char *path, mode_t mode)
{ return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }
ssize_t preadv(int fd, const struct iovec *iov, int count, off_t offset)
{ cancel_point(); ssize_t result = positional_vector_io(fd, iov, count, offset, 0); cancel_point(); return result; }
ssize_t pwritev(int fd, const struct iovec *iov, int count, off_t offset)
{ cancel_point(); ssize_t result = positional_vector_io(fd, iov, count, offset, 1); cancel_point(); return result; }
int fsync(int fd) { return (int)call(ZEDBSD_SYS_fsync, fd, 0, 0, 0, 0, 0); }
int fdatasync(int fd) { return (int)call(ZEDBSD_SYS_fdatasync, fd, 0, 0, 0, 0, 0); }
off_t lseek(int fd, off_t off, int whence) { return (off_t)call(ZEDBSD_SYS_lseek, fd, off, whence, 0, 0, 0); }
int fstat(int fd, struct stat *st) { return (int)call(ZEDBSD_SYS_fstat, fd, (uintptr_t)st, 0, 0, 0, 0); }
int chdir(const char *p) { return (int)call(ZEDBSD_SYS_chdir, (uintptr_t)p, 0, 0, 0, 0, 0); }
int fchdir(int fd) { return (int)call(ZEDBSD_SYS_fchdir, fd, 0, 0, 0, 0, 0); }
char *getcwd(char *p, size_t n) { return (char *)call(ZEDBSD_SYS_getcwd, (uintptr_t)p, n, 0, 0, 0, 0); }
static int ioctl_has_argument(unsigned long request) {
	if (((request >> 16) & 0x1fffUL) != 0)
		return 1;
	if ((request >= SIOCGIFNAME && request <= SIOCGIFINDEX) ||
	    request == SIOCGIFSTATS)
		return 1;
	return request == SIOCADDRT || request == SIOCDELRT ||
	    request == SIOCGRTENTRY;
}
int ioctl(int fd, unsigned long request, ...) {
	va_list ap; uintptr_t arg = 0;
	if (ioctl_has_argument(request)) {
		va_start(ap, request); arg = va_arg(ap, uintptr_t); va_end(ap);
	}
	return (int)call(ZEDBSD_SYS_ioctl, fd, request, arg, 0, 0, 0);
}

int
sysctl(const int *name, unsigned int namelen, void *oldp, size_t *oldlenp,
	const void *newp, size_t newlen)
{
	return (int)call(ZEDBSD_SYS_sysctl, (uintptr_t)name, namelen,
	    (uintptr_t)oldp, (uintptr_t)oldlenp, (uintptr_t)newp, newlen);
}

int
sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
	const void *newp, size_t newlen)
{
	int query[2] = { CTL_SYSCTL, CTL_SYSCTL_NAME2OID };
	int oid[CTL_MAXNAME];
	size_t oidlen = sizeof(oid);
	if (name == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (sysctl(query, 2, oid, &oidlen, name, strlen(name) + 1U) != 0)
		return -1;
	return sysctl(oid, (unsigned)(oidlen / sizeof(oid[0])), oldp,
	    oldlenp, newp, newlen);
}
void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset) {
	intptr_t value = call(ZEDBSD_SYS_mmap, (uintptr_t)address, length, prot,
		flags, fd, offset);
	return value == -1 ? MAP_FAILED : (void *)value;
}
int munmap(void *p, size_t n) { return (int)call(ZEDBSD_SYS_munmap, (uintptr_t)p, n, 0, 0, 0, 0); }
int mprotect(void *p, size_t n, int prot) { return (int)call(ZEDBSD_SYS_mprotect, (uintptr_t)p, n, prot, 0, 0, 0); }
int msync(void *p, size_t n, int flags) { return (int)call(ZEDBSD_SYS_msync, (uintptr_t)p, n, flags, 0, 0, 0); }
static uintptr_t process_break;
static int process_break_known;
int brk(void *address) {
	if (address == NULL) { errno = EINVAL; return -1; }
	intptr_t value = call(ZEDBSD_SYS_brk, (uintptr_t)address, 0, 0, 0, 0, 0);
	if (value == -1) return -1;
	process_break = (uintptr_t)address;
	process_break_known = 1;
	return 0;
}
void *sbrk(intptr_t increment) {
	uintptr_t old_break, new_break;
	uintptr_t decrease = 0;
	intptr_t value;
	if (!process_break_known) {
		value = call(ZEDBSD_SYS_brk, 0, 0, 0, 0, 0, 0);
		if (value == -1) return (void *)-1;
		process_break = (uintptr_t)value;
		process_break_known = 1;
	}
	old_break = process_break;
	if (increment < 0)
		decrease = (uintptr_t)(-(increment + 1)) + 1U;
	if ((increment > 0 && (uintptr_t)increment > UINTPTR_MAX - old_break) ||
	    (increment < 0 && decrease > old_break)) {
		errno = ENOMEM;
		return (void *)-1;
	}
	new_break = increment < 0 ? old_break - decrease :
		old_break + (uintptr_t)increment;
	value = call(ZEDBSD_SYS_brk, new_break, 0, 0, 0, 0, 0);
	if (value == -1) return (void *)-1;
	process_break = new_break;
	return (void *)old_break;
}
long sysconf(int name) {
	uint32_t cpus;
	size_t cpus_size;
	switch (name) {
	case _SC_PAGE_SIZE: return ZEDBSD_USER_PAGE_SIZE;
	case _SC_OPEN_MAX: return 32;
	case _SC_CLK_TCK: return 100;
	case _SC_JOB_CONTROL: return _POSIX_JOB_CONTROL;
	case _SC_THREADS: return _POSIX_THREADS;
	case _SC_THREAD_PROCESS_SHARED: return _POSIX_THREAD_PROCESS_SHARED;
	case _SC_REALTIME_SIGNALS: return _POSIX_REALTIME_SIGNALS;
	case _SC_SHARED_MEMORY_OBJECTS: return _POSIX_SHARED_MEMORY_OBJECTS;
	case _SC_SEMAPHORES: return _POSIX_SEMAPHORES;
	case _SC_MESSAGE_PASSING: return _POSIX_MESSAGE_PASSING;
	case _SC_VERSION: return _POSIX_VERSION;
	case _SC_2_VERSION: return _POSIX2_VERSION;
	case _SC_ARG_MAX: return 32;
	case _SC_CHILD_MAX: return 64;
	case _SC_STREAM_MAX: return 32;
	case _SC_THREAD_KEYS_MAX: return 32;
	case _SC_THREAD_DESTRUCTOR_ITERATIONS: return 4;
	case _SC_THREAD_STACK_MIN: return 65536;
	case _SC_THREAD_THREADS_MAX: return 64;
	case _SC_SEM_NSEMS_MAX: return 64;
	case _SC_SEM_VALUE_MAX: return 0x7fffffffL;
	case _SC_MQ_OPEN_MAX: return 16;
	case _SC_MQ_PRIO_MAX: return 32;
	case _SC_TIMERS: return _POSIX_TIMERS;
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		cpus_size = sizeof(cpus);
		if (sysctlbyname(name == _SC_NPROCESSORS_CONF ? "hw.ncpu" :
		    "hw.ncpuonline", &cpus, &cpus_size, NULL, 0) == 0 &&
		    cpus_size == sizeof(cpus))
			return (long)cpus;
		return -1;
	default: break;
	}
	errno = EINVAL;
	return -1;
}
static long path_limit(int name) {
	switch (name) {
	case _PC_LINK_MAX: return 32767;
	case _PC_MAX_CANON: return 255;
	case _PC_MAX_INPUT: return 255;
	case _PC_NAME_MAX: return NAME_MAX;
	case _PC_PATH_MAX: return PATH_MAX;
	case _PC_PIPE_BUF: return 512;
	case _PC_CHOWN_RESTRICTED: return 1;
	case _PC_NO_TRUNC: return 1;
	case _PC_VDISABLE: return 0xff;
	default: errno = EINVAL; return -1;
	}
}
long pathconf(const char *path, int name) {
	struct stat status;
	if (path == NULL) { errno = EINVAL; return -1; }
	if (stat(path, &status) != 0) return -1;
	return path_limit(name);
}
long fpathconf(int descriptor, int name) {
	struct stat status;
	if (fstat(descriptor, &status) != 0) return -1;
	return path_limit(name);
}
size_t confstr(int name, char *buffer, size_t size)
{
	static const char path[] = "/bin:/usr/bin";
	size_t needed;
	if (name != _CS_PATH) { errno = EINVAL; return 0; }
	needed = sizeof(path);
	if (buffer != NULL && size != 0) {
		size_t copied = needed < size ? needed : size;
		memcpy(buffer, path, copied);
		buffer[copied - 1U] = '\0';
	}
	return needed;
}
int mkdir(const char *path, mode_t mode) { return (int)call(ZEDBSD_SYS_mkdir, (uintptr_t)path, mode, 0, 0, 0, 0); }
int mkdirat(int dirfd, const char *path, mode_t mode) { return (int)call(ZEDBSD_SYS_mkdirat, dirfd, (uintptr_t)path, mode, 0, 0, 0); }
int mkfifoat(int dirfd, const char *path, mode_t mode) { return (int)call(ZEDBSD_SYS_mknodat, dirfd, (uintptr_t)path, S_IFIFO | (mode & 07777U), 0, 0, 0); }
int mkfifo(const char *path, mode_t mode) { return mkfifoat(AT_FDCWD, path, mode); }
int unlink(const char *path) { return (int)call(ZEDBSD_SYS_unlink, (uintptr_t)path, 0, 0, 0, 0, 0); }
int unlinkat(int dirfd, const char *path, int flags) { return (int)call(ZEDBSD_SYS_unlinkat, dirfd, (uintptr_t)path, flags, 0, 0, 0); }
int rmdir(const char *path) { return (int)call(ZEDBSD_SYS_rmdir, (uintptr_t)path, 0, 0, 0, 0, 0); }
int rename(const char *oldpath, const char *newpath) { return (int)call(ZEDBSD_SYS_rename, (uintptr_t)oldpath, (uintptr_t)newpath, 0, 0, 0, 0); }
int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) { return (int)call(ZEDBSD_SYS_renameat, olddirfd, (uintptr_t)oldpath, newdirfd, (uintptr_t)newpath, 0, 0); }
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) { return (int)call(ZEDBSD_SYS_linkat, olddirfd, (uintptr_t)oldpath, newdirfd, (uintptr_t)newpath, flags, 0); }
int link(const char *oldpath, const char *newpath) { return linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0); }
int symlinkat(const char *target, int dirfd, const char *path) { return (int)call(ZEDBSD_SYS_symlinkat, (uintptr_t)target, dirfd, (uintptr_t)path, 0, 0, 0); }
int symlink(const char *target, const char *path) { return symlinkat(target, AT_FDCWD, path); }
ssize_t readlinkat(int dirfd, const char *path, char *buffer, size_t size) { return (ssize_t)call(ZEDBSD_SYS_readlinkat, dirfd, (uintptr_t)path, (uintptr_t)buffer, size, 0, 0); }
ssize_t readlink(const char *path, char *buffer, size_t size) { return readlinkat(AT_FDCWD, path, buffer, size); }
int truncate(const char *path, off_t length) { return (int)call(ZEDBSD_SYS_truncate, (uintptr_t)path, length, 0, 0, 0, 0); }
int ftruncate(int fd, off_t length) { return (int)call(ZEDBSD_SYS_ftruncate, fd, length, 0, 0, 0, 0); }
mode_t umask(mode_t mask) { return (mode_t)call(ZEDBSD_SYS_umask, mask, 0, 0, 0, 0, 0); }
int clock_gettime(clockid_t id, struct timespec *ts) { return (int)call(ZEDBSD_SYS_clock_gettime, id, (uintptr_t)ts, 0, 0, 0, 0); }
int clock_getres(clockid_t id, struct timespec *ts) { return (int)call(ZEDBSD_SYS_clock_getres, id, (uintptr_t)ts, 0, 0, 0, 0); }
int clock_settime(clockid_t id, const struct timespec *ts) { return (int)call(ZEDBSD_SYS_clock_settime, id, (uintptr_t)ts, 0, 0, 0, 0); }
int timer_create(clockid_t id, const struct sigevent *event, timer_t *timer) { return (int)call(ZEDBSD_SYS_timer_create, id, (uintptr_t)event, (uintptr_t)timer, 0, 0, 0); }
int timer_delete(timer_t timer) { return (int)call(ZEDBSD_SYS_timer_delete, timer, 0, 0, 0, 0, 0); }
int timer_settime(timer_t timer, int flags, const struct itimerspec *value, struct itimerspec *old_value) { return (int)call(ZEDBSD_SYS_timer_settime, timer, flags, (uintptr_t)value, (uintptr_t)old_value, 0, 0); }
int timer_gettime(timer_t timer, struct itimerspec *value) { return (int)call(ZEDBSD_SYS_timer_gettime, timer, (uintptr_t)value, 0, 0, 0, 0); }
int timer_getoverrun(timer_t timer) { return (int)call(ZEDBSD_SYS_timer_getoverrun, timer, 0, 0, 0, 0, 0); }
int mount(const char *type, const char *dir, int flags, void *data) { return (int)call(ZEDBSD_SYS_mount, (uintptr_t)type, (uintptr_t)dir, flags, (uintptr_t)data, 0, 0); }
int unmount(const char *dir, int flags) { return (int)call(ZEDBSD_SYS_unmount, (uintptr_t)dir, flags, 0, 0, 0, 0); }
int statvfs(const char *path, struct statvfs *status) { return (int)call(ZEDBSD_SYS_statvfs, (uintptr_t)path, (uintptr_t)status, 0, 0, 0, 0); }
int fstatvfs(int fd, struct statvfs *status) { return (int)call(ZEDBSD_SYS_fstatvfs, fd, (uintptr_t)status, 0, 0, 0, 0); }
int quotactl(const char *path, struct zedbsd_quota_ctl *request) { return (int)call(ZEDBSD_SYS_quotactl, (uintptr_t)path, (uintptr_t)request, 0, 0, 0, 0); }
int snapshotctl(const char *path, struct zedbsd_snapshot_ctl *request) { return (int)call(ZEDBSD_SYS_snapshotctl, (uintptr_t)path, (uintptr_t)request, 0, 0, 0, 0); }
ssize_t getxattr(const char *path, const char *name, void *value, size_t size) { return (ssize_t)call(ZEDBSD_SYS_getxattr, (uintptr_t)path, (uintptr_t)name, (uintptr_t)value, size, 0, 0); }
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) { return (ssize_t)call(ZEDBSD_SYS_lgetxattr, (uintptr_t)path, (uintptr_t)name, (uintptr_t)value, size, 0, 0); }
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size) { return (ssize_t)call(ZEDBSD_SYS_fgetxattr, fd, (uintptr_t)name, (uintptr_t)value, size, 0, 0); }
int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) { return (int)call(ZEDBSD_SYS_setxattr, (uintptr_t)path, (uintptr_t)name, (uintptr_t)value, size, flags, 0); }
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) { return (int)call(ZEDBSD_SYS_lsetxattr, (uintptr_t)path, (uintptr_t)name, (uintptr_t)value, size, flags, 0); }
int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) { return (int)call(ZEDBSD_SYS_fsetxattr, fd, (uintptr_t)name, (uintptr_t)value, size, flags, 0); }
ssize_t listxattr(const char *path, char *list, size_t size) { return (ssize_t)call(ZEDBSD_SYS_listxattr, (uintptr_t)path, (uintptr_t)list, size, 0, 0, 0); }
ssize_t llistxattr(const char *path, char *list, size_t size) { return (ssize_t)call(ZEDBSD_SYS_llistxattr, (uintptr_t)path, (uintptr_t)list, size, 0, 0, 0); }
ssize_t flistxattr(int fd, char *list, size_t size) { return (ssize_t)call(ZEDBSD_SYS_flistxattr, fd, (uintptr_t)list, size, 0, 0, 0); }
int removexattr(const char *path, const char *name) { return (int)call(ZEDBSD_SYS_removexattr, (uintptr_t)path, (uintptr_t)name, 0, 0, 0, 0); }
int lremovexattr(const char *path, const char *name) { return (int)call(ZEDBSD_SYS_lremovexattr, (uintptr_t)path, (uintptr_t)name, 0, 0, 0, 0); }
int fremovexattr(int fd, const char *name) { return (int)call(ZEDBSD_SYS_fremovexattr, fd, (uintptr_t)name, 0, 0, 0, 0); }
int nanosleep(const struct timespec *request, struct timespec *remain) {
	int result; cancel_point();
	result = (int)call(ZEDBSD_SYS_nanosleep, (uintptr_t)request,
		(uintptr_t)remain, 0, 0, 0, 0);
	cancel_point(); return result;
}

pid_t zedbsd_spawn(const char *path, char *const argv[], char *const envp[],
		   unsigned flags) {
	return (pid_t)call(ZEDBSD_SYS_spawn, (uintptr_t)path, (uintptr_t)argv,
		(uintptr_t)envp, flags, 0, 0);
}
pid_t zedbsd_wait_result(pid_t pid, int *status, char *result,
			 size_t capacity) {
	return (pid_t)call(ZEDBSD_SYS_wait, (uintptr_t)pid, (uintptr_t)status,
		0, (uintptr_t)result, capacity, 0);
}
pid_t waitpid(pid_t pid, int *status, int options) {
	pid_t result; cancel_point();
	result = (pid_t)call(ZEDBSD_SYS_waitpid, (uintptr_t)pid,
		(uintptr_t)status, (uintptr_t)options, 0, 0, 0);
	cancel_point(); return result;
}
int clock_nanosleep(clockid_t clock_id, int flags,
	const struct timespec *request, struct timespec *remain)
{
	struct timespec delay, now;
	int saved_errno = errno;
	int result;
	if (request == NULL || request->tv_sec < 0 || request->tv_nsec < 0 ||
	    request->tv_nsec >= 1000000000L ||
	    (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME) ||
	    (flags & ~TIMER_ABSTIME) != 0)
		return EINVAL;
	if ((flags & TIMER_ABSTIME) == 0) {
		result = nanosleep(request, remain);
		if (result == 0) { errno = saved_errno; return 0; }
		result = errno; errno = saved_errno; return result;
	}
	if (clock_gettime(clock_id, &now) != 0) {
		result = errno; errno = saved_errno; return result;
	}
	delay.tv_sec = request->tv_sec - now.tv_sec;
	delay.tv_nsec = request->tv_nsec - now.tv_nsec;
	if (delay.tv_nsec < 0) { delay.tv_nsec += 1000000000L; delay.tv_sec--; }
	if (delay.tv_sec < 0) { errno = saved_errno; return 0; }
	result = nanosleep(&delay, NULL);
	if (result == 0) { errno = saved_errno; return 0; }
	result = errno; errno = saved_errno; return result;
}
unsigned sleep(unsigned seconds)
{
	struct timespec request = { (time_t)seconds, 0 }, remain = { 0, 0 };
	if (nanosleep(&request, &remain) == 0)
		return 0;
	return (unsigned)remain.tv_sec + (remain.tv_nsec != 0);
}
int usleep(useconds_t microseconds)
{
	struct timespec request;
	if (microseconds >= 1000000U) { errno = EINVAL; return -1; }
	request.tv_sec = 0;
	request.tv_nsec = (long)microseconds * 1000L;
	return nanosleep(&request, NULL);
}
int pause(void)
{
	sigset_t mask;
	if (sigprocmask(SIG_SETMASK, NULL, &mask) != 0)
		return -1;
	return sigsuspend(&mask);
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }
int waitid(idtype_t type, id_t id, siginfo_t *information, int options)
{
	int result;
	cancel_point();
	result = (int)call(ZEDBSD_SYS_waitid, type, id,
	    (uintptr_t)information, options, 0, 0);
	cancel_point();
	return result;
}
int getrlimit(int resource, struct rlimit *limit)
{
	struct zedbsd_rlimit wire;
	int result;
	if (limit == NULL) { errno = EFAULT; return -1; }
	result = (int)call(ZEDBSD_SYS_getrlimit, resource, (uintptr_t)&wire,
	    0, 0, 0, 0);
	if (result == 0) {
		limit->rlim_cur = wire.current;
		limit->rlim_max = wire.maximum;
	}
	return result;
}
int setrlimit(int resource, const struct rlimit *limit)
{
	struct zedbsd_rlimit wire;
	if (limit == NULL) { errno = EFAULT; return -1; }
	wire.current = limit->rlim_cur;
	wire.maximum = limit->rlim_max;
	return (int)call(ZEDBSD_SYS_setrlimit, resource, (uintptr_t)&wire,
	    0, 0, 0, 0);
}
enum { SPAWN_ACTION_CLOSE = 1, SPAWN_ACTION_DUP2, SPAWN_ACTION_OPEN };
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions)
{ if (actions == NULL) return EINVAL; memset(actions, 0, sizeof(*actions)); return 0; }
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions)
{ if (actions == NULL) return EINVAL; memset(actions, 0, sizeof(*actions)); return 0; }
static struct zedbsd_spawn_action *spawn_action_add(posix_spawn_file_actions_t *actions)
{ if (actions == NULL || actions->count >= ZEDBSD_SPAWN_ACTION_MAX) return NULL; return &actions->actions[actions->count++]; }
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd)
{ struct zedbsd_spawn_action *a; if (fd < 0) return EBADF; a = spawn_action_add(actions); if (a == NULL) return EINVAL; memset(a, 0, sizeof(*a)); a->operation = SPAWN_ACTION_CLOSE; a->descriptor = fd; return 0; }
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int newfd)
{ struct zedbsd_spawn_action *a; if (fd < 0 || newfd < 0) return EBADF; a = spawn_action_add(actions); if (a == NULL) return EINVAL; memset(a, 0, sizeof(*a)); a->operation = SPAWN_ACTION_DUP2; a->descriptor = fd; a->new_descriptor = newfd; return 0; }
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
	int fd, const char *path, int flags, mode_t mode)
{ struct zedbsd_spawn_action *a; size_t n; if (fd < 0 || path == NULL) return EINVAL; n = strlen(path); if (n >= ZEDBSD_SPAWN_PATH_MAX) return ENAMETOOLONG; a = spawn_action_add(actions); if (a == NULL) return EINVAL; memset(a, 0, sizeof(*a)); a->operation = SPAWN_ACTION_OPEN; a->descriptor = fd; a->flags = flags; a->mode = mode; memcpy(a->path, path, n + 1U); return 0; }
int posix_spawnattr_init(posix_spawnattr_t *attr)
{ if (attr == NULL) return EINVAL; memset(attr, 0, sizeof(*attr)); return 0; }
int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{ if (attr == NULL) return EINVAL; memset(attr, 0, sizeof(*attr)); return 0; }
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags)
{ if (attr == NULL || flags == NULL) return EINVAL; *flags = attr->flags; return 0; }
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{ if (attr == NULL || (flags & ~(POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK)) != 0) return EINVAL; attr->flags = flags; return 0; }
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup)
{ if (attr == NULL || pgroup == NULL) return EINVAL; *pgroup = attr->pgroup; return 0; }
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup)
{ if (attr == NULL || pgroup < 0) return EINVAL; attr->pgroup = pgroup; return 0; }
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *set)
{ if (attr == NULL || set == NULL) return EINVAL; *set = attr->sigmask; return 0; }
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *set)
{ if (attr == NULL || set == NULL) return EINVAL; attr->sigmask = *set; return 0; }
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *set)
{ if (attr == NULL || set == NULL) return EINVAL; *set = attr->sigdefault; return 0; }
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *set)
{ if (attr == NULL || set == NULL) return EINVAL; attr->sigdefault = *set; return 0; }
static int spawn_child_setup(const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attr)
{
	unsigned index;
	if (attr != NULL) {
		if ((attr->flags & POSIX_SPAWN_RESETIDS) != 0 &&
		    (setegid(getgid()) != 0 || seteuid(getuid()) != 0)) return errno;
		if ((attr->flags & POSIX_SPAWN_SETPGROUP) != 0 &&
		    setpgid(0, attr->pgroup) != 0) return errno;
		if ((attr->flags & POSIX_SPAWN_SETSIGMASK) != 0 &&
		    sigprocmask(SIG_SETMASK, &attr->sigmask, NULL) != 0) return errno;
		if ((attr->flags & POSIX_SPAWN_SETSIGDEF) != 0)
			for (index = 1; index < NSIG; index++)
				if (sigismember(&attr->sigdefault, (int)index) == 1) {
					struct sigaction action;
					memset(&action, 0, sizeof(action));
					action.sa_handler = SIG_DFL;
					if (sigaction((int)index, &action, NULL) != 0) return errno;
				}
	}
	if (actions == NULL) return 0;
	for (index = 0; index < actions->count; index++) {
		const struct zedbsd_spawn_action *a = &actions->actions[index];
		if (a->operation == SPAWN_ACTION_CLOSE) {
			if (close(a->descriptor) != 0 && errno != EBADF) return errno;
		} else if (a->operation == SPAWN_ACTION_DUP2) {
			if (dup2(a->descriptor, a->new_descriptor) < 0) return errno;
		} else if (a->operation == SPAWN_ACTION_OPEN) {
			int fd = open(a->path, a->flags, a->mode);
			if (fd < 0) return errno;
			if (fd != a->descriptor && dup2(fd, a->descriptor) < 0) { int e = errno; (void)close(fd); return e; }
			if (fd != a->descriptor) (void)close(fd);
		} else return EINVAL;
	}
	return 0;
}
int posix_spawn(pid_t *result, const char *path,
	const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
	char *const argv[], char *const envp[])
{
	int error_pipe[2], child_error = 0;
	ssize_t count;
	pid_t child;
	if (result == NULL || path == NULL || argv == NULL) return EINVAL;
	if (pipe2(error_pipe, O_CLOEXEC) != 0) return errno;
	child = fork();
	if (child < 0) { child_error = errno; (void)close(error_pipe[0]); (void)close(error_pipe[1]); return child_error; }
	if (child == 0) {
		(void)close(error_pipe[0]);
		child_error = spawn_child_setup(actions, attr);
		if (child_error == 0) {
			execve(path, argv, envp != NULL ? envp : environ);
			child_error = errno;
		}
		(void)write(error_pipe[1], &child_error, sizeof(child_error));
		_exit(127);
	}
	(void)close(error_pipe[1]);
	count = read(error_pipe[0], &child_error, sizeof(child_error));
	(void)close(error_pipe[0]);
	if (count > 0) { (void)waitpid(child, NULL, 0); return child_error != 0 ? child_error : EIO; }
	if (count < 0) { child_error = errno; (void)waitpid(child, NULL, 0); return child_error; }
	*result = child;
	return 0;
}
int posix_spawnp(pid_t *result, const char *file,
	const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
	char *const argv[], char *const envp[])
{
	const char *path, *at;
	char candidate[PATH_MAX];
	if (file == NULL) return EINVAL;
	if (strchr(file, '/') != NULL)
		return posix_spawn(result, file, actions, attr, argv, envp);
	path = getenv("PATH");
	if (path == NULL || *path == '\0') path = "/bin:/usr/bin";
	at = path;
	for (;;) {
		const char *colon = strchr(at, ':');
		size_t directory_length = colon != NULL ? (size_t)(colon - at) : strlen(at);
		size_t file_length = strlen(file);
		if (directory_length + file_length + 2U <= sizeof(candidate)) {
			if (directory_length != 0) memcpy(candidate, at, directory_length);
			else { candidate[0] = '.'; directory_length = 1; }
			candidate[directory_length] = '/';
			memcpy(candidate + directory_length + 1U, file, file_length + 1U);
			if (access(candidate, X_OK) == 0)
				return posix_spawn(result, candidate, actions, attr, argv, envp);
		}
		if (colon == NULL) break;
		at = colon + 1;
	}
	return ENOENT;
}
pid_t fork(void) {
	pid_t result;
	if (zedbsd_pthread_fork_prepare != NULL)
		zedbsd_pthread_fork_prepare();
	result = (pid_t)call(ZEDBSD_SYS_fork, 0, 0, 0, 0, 0, 0);
	if (result == 0) {
		if (zedbsd_pthread_fork_child != NULL)
			zedbsd_pthread_fork_child();
	} else if (zedbsd_pthread_fork_parent != NULL) {
		zedbsd_pthread_fork_parent();
	}
	return result;
}
int execve(const char *path, char *const argv[], char *const envp[]) {
	return (int)call(ZEDBSD_SYS_execve, (uintptr_t)path, (uintptr_t)argv,
		(uintptr_t)envp, 0, 0, 0);
}
pid_t getpid(void) { return (pid_t)call(ZEDBSD_SYS_getpid, 0, 0, 0, 0, 0, 0); }
pid_t getppid(void) { return (pid_t)call(ZEDBSD_SYS_getppid, 0, 0, 0, 0, 0, 0); }
pid_t getpgrp(void) { return (pid_t)call(ZEDBSD_SYS_getpgrp, 0, 0, 0, 0, 0, 0); }
pid_t getpgid(pid_t pid) { return (pid_t)call(ZEDBSD_SYS_getpgid, pid, 0, 0, 0, 0, 0); }
int setpgid(pid_t pid, pid_t pgid) { return (int)call(ZEDBSD_SYS_setpgid, pid, pgid, 0, 0, 0, 0); }
pid_t setsid(void) { return (pid_t)call(ZEDBSD_SYS_setsid, 0, 0, 0, 0, 0, 0); }
pid_t getsid(pid_t pid) { return (pid_t)call(ZEDBSD_SYS_getsid, pid, 0, 0, 0, 0, 0); }
uid_t getuid(void) { return (uid_t)call(ZEDBSD_SYS_getuid, 0, 0, 0, 0, 0, 0); }
uid_t geteuid(void) { return (uid_t)call(ZEDBSD_SYS_geteuid, 0, 0, 0, 0, 0, 0); }
gid_t getgid(void) { return (gid_t)call(ZEDBSD_SYS_getgid, 0, 0, 0, 0, 0, 0); }
gid_t getegid(void) { return (gid_t)call(ZEDBSD_SYS_getegid, 0, 0, 0, 0, 0, 0); }
int getgroups(int count, gid_t groups[]) { return (int)call(ZEDBSD_SYS_getgroups, count, (uintptr_t)groups, 0, 0, 0, 0); }
int setuid(uid_t id) { return (int)call(ZEDBSD_SYS_setuid, id, 0, 0, 0, 0, 0); }
int seteuid(uid_t id) { return (int)call(ZEDBSD_SYS_seteuid, id, 0, 0, 0, 0, 0); }
int setgid(gid_t id) { return (int)call(ZEDBSD_SYS_setgid, id, 0, 0, 0, 0, 0); }
int setegid(gid_t id) { return (int)call(ZEDBSD_SYS_setegid, id, 0, 0, 0, 0, 0); }
int setgroups(size_t count, const gid_t groups[]) { return (int)call(ZEDBSD_SYS_setgroups, count, (uintptr_t)groups, 0, 0, 0, 0); }
int setreuid(uid_t real, uid_t effective) { return (int)call(ZEDBSD_SYS_setreuid, real, effective, 0, 0, 0, 0); }
int setregid(gid_t real, gid_t effective) { return (int)call(ZEDBSD_SYS_setregid, real, effective, 0, 0, 0, 0); }

int stat(const char *path, struct stat *status) { return (int)call(ZEDBSD_SYS_stat, (uintptr_t)path, (uintptr_t)status, 0, 0, 0, 0); }
int lstat(const char *path, struct stat *status) { return (int)call(ZEDBSD_SYS_lstat, (uintptr_t)path, (uintptr_t)status, 0, 0, 0, 0); }
int fstatat(int dirfd, const char *path, struct stat *status, int flags) { return (int)call(ZEDBSD_SYS_fstatat, dirfd, (uintptr_t)path, (uintptr_t)status, flags, 0, 0); }
int access(const char *path, int mode) { return (int)call(ZEDBSD_SYS_access, (uintptr_t)path, mode, 0, 0, 0, 0); }
int faccessat(int dirfd, const char *path, int mode, int flags) { return (int)call(ZEDBSD_SYS_faccessat, dirfd, (uintptr_t)path, mode, flags, 0, 0); }
int chmod(const char *path, mode_t mode) { return (int)call(ZEDBSD_SYS_chmod, (uintptr_t)path, mode, 0, 0, 0, 0); }
int fchmod(int fd, mode_t mode) { return (int)call(ZEDBSD_SYS_fchmod, fd, mode, 0, 0, 0, 0); }
int fchmodat(int dirfd, const char *path, mode_t mode, int flags) { return (int)call(ZEDBSD_SYS_fchmodat, dirfd, (uintptr_t)path, mode, flags, 0, 0); }
int chown(const char *path, uid_t uid, gid_t gid) { return (int)call(ZEDBSD_SYS_chown, (uintptr_t)path, uid, gid, 0, 0, 0); }
int fchown(int fd, uid_t uid, gid_t gid) { return (int)call(ZEDBSD_SYS_fchown, fd, uid, gid, 0, 0, 0); }
int lchown(const char *path, uid_t uid, gid_t gid) { return (int)call(ZEDBSD_SYS_lchown, (uintptr_t)path, uid, gid, 0, 0, 0); }
int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags) { return (int)call(ZEDBSD_SYS_fchownat, dirfd, (uintptr_t)path, uid, gid, flags, 0); }
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) { return (int)call(ZEDBSD_SYS_utimensat, dirfd, (uintptr_t)path, (uintptr_t)times, flags, 0, 0); }
int futimens(int fd, const struct timespec times[2]) { return (int)call(ZEDBSD_SYS_futimens, fd, (uintptr_t)times, 0, 0, 0, 0); }
int isatty(int fd)
{
	if (ioctl(fd, ZEDBSD_CONSOLE_ISATTY) == 0)
		return 1;
	if (errno != EBADF)
		errno = ENOTTY;
	return 0;
}
char *getlogin(void)
{
	static char login[] = "root";
	return login;
}
int getlogin_r(char *buffer, size_t size)
{
	static const char login[] = "root";
	if (buffer == NULL) return EINVAL;
	if (size < sizeof(login)) return ERANGE;
	memcpy(buffer, login, sizeof(login));
	return 0;
}
int gethostname(char *buffer, size_t size)
{
	size_t length = size;
	if (buffer == NULL) { errno = EFAULT; return -1; }
	if (size == 0) { errno = EINVAL; return -1; }
	if (sysctlbyname("kern.hostname", buffer, &length, NULL, 0) != 0)
		return -1;
	buffer[size - 1U] = '\0';
	return 0;
}
int sethostname(const char *name, size_t length)
{
	if (name == NULL) { errno = EFAULT; return -1; }
	return sysctlbyname("kern.hostname", NULL, NULL, name, length);
}
char *ttyname(int fd)
{
	static char name[] = "/dev/console";
	return isatty(fd) ? name : NULL;
}
int ttyname_r(int fd, char *buffer, size_t size)
{
	static const char name[] = "/dev/console";
	if (buffer == NULL) return EINVAL;
	if (!isatty(fd)) return errno;
	if (size < sizeof(name)) return ERANGE;
	memcpy(buffer, name, sizeof(name));
	return 0;
}
int uname(struct utsname *name)
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
	if (name == NULL) { errno = EFAULT; return -1; }
	memset(name, 0, sizeof(*name));
	strcpy(name->sysname, "zedBSD");
	if (gethostname(name->nodename, sizeof(name->nodename)) != 0)
		return -1;
	strcpy(name->release, "0.0.1");
	strcpy(name->version, "zedBSD 0.0.1");
	strcpy(name->machine, machine);
	return 0;
}
int fileno(void *stream) {
	FILE *file = stream;
	return file == NULL || file->context == NULL ? -1 : (int)(intptr_t)file->context - 1;
}

struct zedbsd_directory { int fd; struct dirent current; };
DIR *opendir(const char *path)
{
	DIR *directory = malloc(sizeof(*directory));
	if (directory == NULL) { errno = ENOMEM; return NULL; }
	directory->fd = open(path, O_RDONLY | O_DIRECTORY);
	if (directory->fd < 0) { free(directory); return NULL; }
	return directory;
}
DIR *fdopendir(int fd)
{
	DIR *directory;
	struct stat status;
	int flags;
	if (fd < 0 || fstat(fd, &status) != 0)
		return NULL;
	if (!S_ISDIR(status.st_mode)) { errno = ENOTDIR; return NULL; }
	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return NULL;
	directory = malloc(sizeof(*directory));
	if (directory == NULL) { errno = ENOMEM; return NULL; }
	directory->fd = fd;
	return directory;
}
struct dirent *readdir(DIR *directory)
{
	struct zedbsd_dirent entry;
	intptr_t result;
	if (directory == NULL) { errno = EBADF; return NULL; }
	result = call(ZEDBSD_SYS_getdents, directory->fd, (uintptr_t)&entry,
		sizeof(entry), 0, 0, 0);
	if (result <= 0) return NULL;
	directory->current.d_ino = entry.d_ino;
	directory->current.d_type = entry.d_type;
	strncpy(directory->current.d_name, entry.d_name,
		sizeof(directory->current.d_name) - 1U);
	directory->current.d_name[sizeof(directory->current.d_name) - 1U] = '\0';
	return &directory->current;
}
int closedir(DIR *directory)
{
	int result;
	if (directory == NULL) { errno = EBADF; return -1; }
	result = close(directory->fd); free(directory); return result;
}
void rewinddir(DIR *directory)
{
	if (directory != NULL)
		(void)lseek(directory->fd, 0, SEEK_SET);
}
void seekdir(DIR *directory, long location)
{
	if (directory != NULL && location >= 0)
		(void)lseek(directory->fd, (off_t)location, SEEK_SET);
}
long telldir(DIR *directory)
{
	off_t location;
	if (directory == NULL) { errno = EBADF; return -1; }
	location = lseek(directory->fd, 0, SEEK_CUR);
	return location < 0 || location > LONG_MAX ? -1L : (long)location;
}
int dirfd(DIR *directory)
{
	if (directory == NULL) { errno = EINVAL; return -1; }
	return directory->fd;
}

size_t zedbsd_console_write_bytes(const char *bytes, size_t length)
{
	ssize_t result = write(1, bytes, length);
	return result < 0 ? 0U : (size_t)result;
}

static int stream_fd(FILE *stream) { return fileno(stream); }
static volatile uint32_t stream_registry_lock;
static FILE *stream_registry;

static void
stream_registry_acquire(void)
{
	while (__atomic_exchange_n(&stream_registry_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		;
}

static void
stream_registry_release(void)
{
	__atomic_store_n(&stream_registry_lock, 0U, __ATOMIC_RELEASE);
}

static void
stream_register(FILE *stream)
{
	stream_registry_acquire();
	stream->registry_next = stream_registry;
	stream_registry = stream;
	stream_registry_release();
}

static void
stream_unregister_locked(FILE *stream)
{
	FILE **link;
	for (link = &stream_registry; *link != NULL;
	    link = &(*link)->registry_next)
		if (*link == stream) {
			*link = stream->registry_next;
			stream->registry_next = NULL;
			return;
		}
}

void
zedbsd_stdio_fork_child(void)
{
	FILE *stream;
	stream_registry_lock = 0;
	for (stream = stream_registry; stream != NULL;
	    stream = stream->registry_next) {
		stream->lock = 0;
		stream->lock_owner = 0;
		stream->lock_depth = 0;
	}
}

static void
stream_enter(FILE *stream, int *cancel_state)
{
	(void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, cancel_state);
	flockfile(stream);
}

static void
stream_leave(FILE *stream, int cancel_state)
{
	funlockfile(stream);
	(void)pthread_setcancelstate(cancel_state, NULL);
}

static int
stream_write_direct(FILE *stream, const unsigned char *buffer, size_t length,
	size_t *written)
{
	*written = 0;
	while (*written < length) {
		ssize_t result = write(stream_fd(stream), buffer + *written,
		    length - *written);
		if (result > 0) {
			*written += (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		stream->error = 1;
		return EOF;
	}
	return 0;
}

static int
stream_flush_locked(FILE *stream)
{
	if (stream->last_operation == 2 && stream->buffer_length != 0) {
		size_t written;
		if (stream_write_direct(stream, stream->buffer,
		    stream->buffer_length, &written) == EOF) {
			if (written != 0) {
				memmove(stream->buffer, stream->buffer + written,
				    stream->buffer_length - written);
				stream->buffer_length -= written;
			}
			return EOF;
		}
		stream->buffer_length = 0;
	} else if (stream->last_operation == 1 &&
	    stream->buffer_length > stream->buffer_start) {
		off_t unread = (off_t)(stream->buffer_length - stream->buffer_start);
		if (lseek(stream_fd(stream), -unread, SEEK_CUR) < 0) {
			stream->error = 1;
			return EOF;
		}
		stream->buffer_start = stream->buffer_length = 0;
	}
	return 0;
}

static int
stream_ensure_buffer(FILE *stream)
{
	if (stream->buffering_mode == _IONBF || stream->buffer != NULL)
		return 0;
	stream->buffer = malloc(BUFSIZ);
	if (stream->buffer == NULL) {
		stream->error = 1;
		errno = ENOMEM;
		return -1;
	}
	stream->buffer_size = BUFSIZ;
	stream->buffer_owned = 1;
	return 0;
}

FILE *fopen(const char *path, const char *mode)
{
	FILE *stream;
	int flags;
	if (mode == NULL || mode[0] == '\0') { errno = EINVAL; return NULL; }
	flags = mode[0] == 'r' ? O_RDONLY : mode[0] == 'w' ?
		O_WRONLY | O_CREAT | O_TRUNC : mode[0] == 'a' ?
		O_WRONLY | O_CREAT | O_APPEND : -1;
	if (flags < 0) { errno = EINVAL; return NULL; }
	if (strchr(mode, '+') != NULL) flags = (flags & ~O_ACCMODE) | O_RDWR;
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL) { errno = ENOMEM; return NULL; }
	flags = open(path, flags, 0666);
	if (flags < 0) { free(stream); return NULL; }
	stream->context = (void *)(intptr_t)(flags + 1);
	stream->mode = (unsigned)(mode[0] == 'r' ? 1U : 2U);
	if (strchr(mode, '+') != NULL)
		stream->mode = 3U;
	stream->buffering_mode = _IOFBF;
	stream->ungot_character = EOF;
	stream->heap_allocated = 1;
	{
		off_t position = lseek(flags, 0, SEEK_CUR);
		if (position >= 0)
			stream->position = (uint64_t)position;
	}
	stream_register(stream);
	return stream;
}
int fclose(FILE *stream) { int result, flush_result, old; unsigned allocated; if (stream == NULL) { errno = EINVAL; return EOF; } (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old); stream_registry_acquire(); stream_unregister_locked(stream); flockfile(stream); stream_registry_release(); flush_result = stream_flush_locked(stream); result = close(stream_fd(stream)); allocated = stream->heap_allocated; if (stream->buffer_owned) free(stream->buffer); stream->buffer = NULL; stream->context = NULL; funlockfile(stream); if (allocated) free(stream); (void)pthread_setcancelstate(old, NULL); return result == 0 && flush_result == 0 ? 0 : EOF; }
int fflush(FILE *stream) { int old, result = 0; if (stream != NULL) { stream_enter(stream, &old); result = stream_flush_locked(stream); stream_leave(stream, old); return result; } (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old); stream_registry_acquire(); for (stream = stream_registry; stream != NULL; stream = stream->registry_next) { flockfile(stream); if (stream_flush_locked(stream) == EOF) result = EOF; funlockfile(stream); } stream_registry_release(); (void)pthread_setcancelstate(old, NULL); return result; }
size_t fread(void *buffer, size_t size, size_t count, FILE *stream) {
	size_t total, done = 0; int old;
	if (stream == NULL || (buffer == NULL && size != 0 && count != 0)) { errno = EINVAL; return 0; }
	if (size != 0 && count > SIZE_MAX / size) { errno = EOVERFLOW; return 0; }
	total = size * count; if (total == 0) return 0;
	stream_enter(stream, &old);
	stream->io_started = 1;
	if (stream->last_operation == 2 && stream_flush_locked(stream) == EOF)
		goto read_done;
	stream->last_operation = 1;
	if (stream->ungot_character != EOF) {
		((unsigned char *)buffer)[done++] =
		    (unsigned char)stream->ungot_character;
		stream->ungot_character = EOF;
	}
	while (done < total) {
		ssize_t result;
		if (stream->buffer_start < stream->buffer_length) {
			size_t available = stream->buffer_length - stream->buffer_start;
			size_t take = available < total - done ? available : total - done;
			memcpy((unsigned char *)buffer + done,
			    stream->buffer + stream->buffer_start, take);
			stream->buffer_start += take;
			done += take;
			continue;
		}
		stream->buffer_start = stream->buffer_length = 0;
		if (stream->buffering_mode == _IONBF) {
			result = read(stream_fd(stream), (unsigned char *)buffer + done,
			    total - done);
			if (result > 0) { done += (size_t)result; continue; }
		} else {
			if (stream_ensure_buffer(stream) != 0)
				break;
			result = read(stream_fd(stream), stream->buffer,
			    stream->buffer_size);
			if (result > 0) {
				stream->buffer_length = (size_t)result;
				continue;
			}
		}
		if (result == 0) { stream->eof = 1; break; }
		if (errno == EINTR) continue;
		stream->error = 1; break;
	}
	read_done:
	stream->position += done;
	stream_leave(stream, old);
	return done / size;
}
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
	size_t total, done = 0; int old;
	if (stream == NULL || (buffer == NULL && size != 0 && count != 0)) { errno = EINVAL; return 0; }
	if (size != 0 && count > SIZE_MAX / size) { errno = EOVERFLOW; return 0; }
	total = size * count; if (total == 0) return 0;
	stream_enter(stream, &old);
	stream->io_started = 1;
	if (stream->last_operation == 1 && stream_flush_locked(stream) == EOF)
		goto write_done;
	stream->last_operation = 2;
	if (stream->buffering_mode == _IONBF) {
		(void)stream_write_direct(stream, buffer, total, &done);
	} else if (stream_ensure_buffer(stream) == 0) {
		while (done < total) {
			size_t space = stream->buffer_size - stream->buffer_length;
			size_t put = space < total - done ? space : total - done;
			const unsigned char *source =
			    (const unsigned char *)buffer + done;
			memcpy(stream->buffer + stream->buffer_length, source, put);
			stream->buffer_length += put;
			done += put;
			if (stream->buffer_length == stream->buffer_size ||
			    (stream->buffering_mode == _IOLBF &&
			    memchr(source, '\n', put) != NULL))
				if (stream_flush_locked(stream) == EOF)
					break;
		}
	}
	write_done:
	stream->position += done;
	stream_leave(stream, old);
	return done / size;
}
int getc(FILE *stream) { unsigned char byte; return fread(&byte, 1, 1, stream) == 1 ? byte : EOF; }
int ungetc(int character, FILE *stream) { int old, result = EOF; if (stream == NULL || character == EOF) return EOF; stream_enter(stream, &old); if (stream->last_operation != 2 && stream->ungot_character == EOF) { stream->ungot_character = (unsigned char)character; stream->eof = 0; if (stream->position != 0) stream->position--; result = (unsigned char)character; } stream_leave(stream, old); return result; }
char *fgets(char *buffer, int size, FILE *stream) {
	int c, i = 0, old; if (buffer == NULL || stream == NULL || size <= 0) return NULL;
	stream_enter(stream, &old);
	while (i + 1 < size && (c = getc(stream)) != EOF) { buffer[i++] = (char)c; if (c == '\n') break; }
	if (i == 0) {
		stream_leave(stream, old);
		return NULL;
	}
	buffer[i] = '\0';
	stream_leave(stream, old);
	return buffer;
}
int fseek(FILE *stream, long offset, int whence) { off_t at; int old; if (stream == NULL || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)) { errno = EINVAL; return -1; } stream_enter(stream, &old); if (stream->last_operation == 2 && stream_flush_locked(stream) == EOF) { stream_leave(stream, old); return -1; } if (whence == SEEK_CUR) { if ((offset < 0 && (uint64_t)(-(offset + 1L)) + 1U > stream->position) || (offset > 0 && (uint64_t)offset > UINT64_MAX - stream->position)) { stream_leave(stream, old); errno = EINVAL; return -1; } at = lseek(stream_fd(stream), (off_t)(offset < 0 ? stream->position - ((uint64_t)(-(offset + 1L)) + 1U) : stream->position + (uint64_t)offset), SEEK_SET); } else at = lseek(stream_fd(stream), offset, whence); if (at >= 0) { stream->position = (uint64_t)at; stream->eof = 0; stream->ungot_character = EOF; stream->buffer_start = stream->buffer_length = 0; stream->last_operation = 0; } else stream->error = 1; stream_leave(stream, old); return at < 0 ? -1 : 0; }
long ftell(FILE *stream) { uint64_t position; int old; if (stream == NULL) { errno = EINVAL; return -1L; } stream_enter(stream, &old); position = stream->position; stream_leave(stream, old); if (position > LONG_MAX) { errno = EOVERFLOW; return -1L; } return (long)position; }

int setvbuf(FILE *stream, char *buffer, int mode, size_t size) { int old; if (stream == NULL || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) || (mode != _IONBF && size == 0)) { errno = EINVAL; return -1; } stream_enter(stream, &old); if (stream->io_started) { stream_leave(stream, old); errno = EBUSY; return -1; } if (stream->buffer_owned) free(stream->buffer); stream->buffer = mode == _IONBF ? NULL : (unsigned char *)buffer; stream->buffer_size = mode == _IONBF ? 0 : size; stream->buffer_owned = 0; if (mode != _IONBF && buffer == NULL) { stream->buffer = malloc(size); if (stream->buffer == NULL) { stream->buffer_size = 0; stream->buffering_mode = _IONBF; stream_leave(stream, old); errno = ENOMEM; return -1; } stream->buffer_owned = 1; } stream->buffering_mode = mode; stream_leave(stream, old); return 0; }
void setbuf(FILE *stream, char *buffer) { (void)setvbuf(stream, buffer, buffer != NULL ? _IOFBF : _IONBF, buffer != NULL ? BUFSIZ : 0); }

time_t time(time_t *result) {
	struct timespec ts; time_t value = clock_gettime(CLOCK_REALTIME, &ts) == 0 ? ts.tv_sec : (time_t)-1;
	if (result != NULL)
		*result = value;
	return value;
}
int gettimeofday(struct timeval *result, void *timezone) {
	struct timespec now;
	(void)timezone;
	if (result == NULL) { errno = EINVAL; return -1; }
	if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
	result->tv_sec = now.tv_sec;
	result->tv_usec = now.tv_nsec / 1000L;
	return 0;
}
void exit(int status) {
#if defined(ZEDBSD_DYNAMIC_LIBC)
	__zedbsd_rtld_exports.process_fini();
#else
	if (__zedbsd_rtld_process_fini != NULL)
		__zedbsd_rtld_process_fini();
#endif
	(void)fflush(NULL);
	_exit(status);
}
void zedbsd_libc_panic(const char *message) { (void)write(2, message, strlen(message)); (void)write(2, "\n", 1); _exit(127); }

static struct zedbsd_heap user_heap;
static size_t user_heap_grow(void *context, void *end, size_t minimum)
{
	size_t amount;
	void *old;
	(void)context;
	if (minimum > SIZE_MAX - 65535U)
		return 0;
	amount = (minimum + 65535U) & ~(size_t)65535U;
	old = sbrk((intptr_t)amount);
	if (old == (void *)-1 || old != end)
		return 0;
	return amount;
}
void zedbsd_user_libc_init(int argc, char **argv, char **envp)
{
	#define USER_HEAP_INITIAL (64U * 1024U)
	void *arena;
	size_t env_count;
	(void)argc; (void)argv;
	for (env_count = 0; env_count < ENVIRONMENT_MAX && envp != NULL &&
	     envp[env_count] != NULL; env_count++)
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
	if (arena == (void *)-1)
		zedbsd_libc_panic("unable to initialize user heap");
	zedbsd_heap_init_instance(&user_heap, arena, USER_HEAP_INITIAL);
	zedbsd_heap_set_grow_instance(&user_heap, user_heap_grow, NULL);
	zedbsd_heap_set_active(&user_heap);
	/* Constructors may use pthread state and errno.  Attach the initial
	 * thread before the runtime linker invokes any of them. */
	if (zedbsd_pthread_initialize_main != NULL)
		zedbsd_pthread_initialize_main();
#if defined(ZEDBSD_DYNAMIC_LIBC)
	__zedbsd_rtld_exports.startup_init();
#else
	if (__zedbsd_rtld_startup_init != NULL)
		__zedbsd_rtld_startup_init();
#endif
}
