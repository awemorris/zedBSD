/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/libc/syscall.h"
#include "libc/heap.h"

#include <zedbsd/dirent.h>
#include <zedbsd/syscall.h>
#include <zedbsd/process.h>
#include <zedbsd/netif.h>
#include <zedbsd/route.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
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
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL)
		return NULL;
	for (i = 0; environ != NULL && environ[i] != NULL; i++)
		if (environment_name(environ[i], name))
			return strchr(environ[i], '=') + 1;
	return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
	size_t name_length, value_length;
	char *entry;
	unsigned i, empty = ENVIRONMENT_MAX;
	if (name == NULL || value == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL; return -1;
	}
	if (!overwrite && getenv(name) != NULL)
		return 0;
	for (i = 0; i < ENVIRONMENT_MAX; i++) {
		if (environ[i] == NULL && empty == ENVIRONMENT_MAX) empty = i;
		if (environment_name(environ[i], name)) { empty = i; break; }
	}
	if (empty == ENVIRONMENT_MAX) { errno = ENOSPC; return -1; }
	name_length = strlen(name); value_length = strlen(value);
	if (name_length > SIZE_MAX - value_length - 2U) { errno = ENOMEM; return -1; }
	entry = malloc(name_length + value_length + 2U);
	if (entry == NULL) { errno = ENOMEM; return -1; }
	memcpy(entry, name, name_length); entry[name_length] = '=';
	memcpy(entry + name_length + 1U, value, value_length + 1U);
	if (environment_owned[empty]) free(environ[empty]);
	environ[empty] = entry; environment_owned[empty] = 1U;
	environ[empty + 1U] = NULL;
	return 0;
}

int unsetenv(const char *name)
{
	unsigned i;
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL; return -1;
	}
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
	if (command == F_DUPFD || command == F_DUPFD_CLOEXEC ||
	    command == F_SETFD || command == F_SETFL) {
		va_start(ap, command);
		argument = va_arg(ap, int);
		va_end(ap);
	}
	return (int)call(ZEDBSD_SYS_fcntl, fd, command, argument, 0, 0, 0);
}
int pipe2(int result[2], int flags) { return (int)call(ZEDBSD_SYS_pipe2, (uintptr_t)result, flags, 0, 0, 0, 0); }
int pipe(int result[2]) { return pipe2(result, 0); }
ssize_t read(int fd, void *p, size_t n) { return (ssize_t)call(ZEDBSD_SYS_read, fd, (uintptr_t)p, n, 0, 0, 0); }
ssize_t write(int fd, const void *p, size_t n) { return (ssize_t)call(ZEDBSD_SYS_write, fd, (uintptr_t)p, n, 0, 0, 0); }
ssize_t pread(int fd, void *p, size_t n, off_t offset) { return (ssize_t)call(ZEDBSD_SYS_pread, fd, (uintptr_t)p, n, offset, 0, 0); }
ssize_t pwrite(int fd, const void *p, size_t n, off_t offset) { return (ssize_t)call(ZEDBSD_SYS_pwrite, fd, (uintptr_t)p, n, offset, 0, 0); }
ssize_t readv(int fd, const struct iovec *iov, int count) { return (ssize_t)call(ZEDBSD_SYS_readv, fd, (uintptr_t)iov, count, 0, 0, 0); }
ssize_t writev(int fd, const struct iovec *iov, int count) { return (ssize_t)call(ZEDBSD_SYS_writev, fd, (uintptr_t)iov, count, 0, 0, 0); }
int fsync(int fd) { return (int)call(ZEDBSD_SYS_fsync, fd, 0, 0, 0, 0, 0); }
int fdatasync(int fd) { return (int)call(ZEDBSD_SYS_fdatasync, fd, 0, 0, 0, 0, 0); }
off_t lseek(int fd, off_t off, int whence) { return (off_t)call(ZEDBSD_SYS_lseek, fd, off, whence, 0, 0, 0); }
int fstat(int fd, struct stat *st) { return (int)call(ZEDBSD_SYS_fstat, fd, (uintptr_t)st, 0, 0, 0, 0); }
int chdir(const char *p) { return (int)call(ZEDBSD_SYS_chdir, (uintptr_t)p, 0, 0, 0, 0, 0); }
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
	if (name == _SC_PAGE_SIZE)
		return ZEDBSD_USER_PAGE_SIZE;
	errno = EINVAL;
	return -1;
}
int mkdir(const char *path, mode_t mode) { return (int)call(ZEDBSD_SYS_mkdir, (uintptr_t)path, mode, 0, 0, 0, 0); }
int mkdirat(int dirfd, const char *path, mode_t mode) { return (int)call(ZEDBSD_SYS_mkdirat, dirfd, (uintptr_t)path, mode, 0, 0, 0); }
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
int nanosleep(const struct timespec *request, struct timespec *remain) {
	return (int)call(ZEDBSD_SYS_nanosleep, (uintptr_t)request,
		(uintptr_t)remain, 0, 0, 0, 0);
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
	return (pid_t)call(ZEDBSD_SYS_waitpid, (uintptr_t)pid,
		(uintptr_t)status, (uintptr_t)options, 0, 0, 0);
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }
pid_t fork(void) { return (pid_t)call(ZEDBSD_SYS_fork, 0, 0, 0, 0, 0, 0); }
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
int isatty(int fd) { struct stat st; return fstat(fd, &st) == 0 && S_ISCHR(st.st_mode); }
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

size_t zedbsd_console_write_bytes(const char *bytes, size_t length)
{
	ssize_t result = write(1, bytes, length);
	return result < 0 ? 0U : (size_t)result;
}

static int stream_fd(FILE *stream) { return fileno(stream); }
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
	return stream;
}
int fclose(FILE *stream) { int result; if (stream == NULL) return EOF; result = close(stream_fd(stream)); free(stream); return result == 0 ? 0 : EOF; }
int fflush(FILE *stream) { (void)stream; return 0; }
size_t fread(void *buffer, size_t size, size_t count, FILE *stream) {
	size_t total; ssize_t result;
	if (size != 0 && count > SIZE_MAX / size) { errno = EINVAL; return 0; }
	total = size * count; result = read(stream_fd(stream), buffer, total);
	if (result < 0) { stream->error = 1; return 0; }
	if ((size_t)result < total) stream->eof = 1;
	return size == 0 ? 0 : (size_t)result / size;
}
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
	size_t total; ssize_t result;
	if (size != 0 && count > SIZE_MAX / size) { errno = EINVAL; return 0; }
	total = size * count; result = write(stream_fd(stream), buffer, total);
	if (result < 0) { stream->error = 1; return 0; }
	return size == 0 ? 0 : (size_t)result / size;
}
int getc(FILE *stream) { unsigned char byte; return fread(&byte, 1, 1, stream) == 1 ? byte : EOF; }
char *fgets(char *buffer, int size, FILE *stream) {
	int c, i = 0; if (buffer == NULL || size <= 0) return NULL;
	while (i + 1 < size && (c = getc(stream)) != EOF) { buffer[i++] = (char)c; if (c == '\n') break; }
	if (i == 0)
		return NULL;
	buffer[i] = '\0';
	return buffer;
}
int fseek(FILE *stream, long offset, int whence) { off_t at = lseek(stream_fd(stream), offset, whence); if (at < 0) return -1; stream->position = (uint64_t)at; stream->eof = 0; return 0; }
long ftell(FILE *stream) { off_t at = lseek(stream_fd(stream), 0, SEEK_CUR); return at < 0 ? -1L : (long)at; }

time_t time(time_t *result) {
	struct timespec ts; time_t value = clock_gettime(CLOCK_REALTIME, &ts) == 0 ? ts.tv_sec : (time_t)-1;
	if (result != NULL)
		*result = value;
	return value;
}
void exit(int status) { (void)fflush(NULL); _exit(status); }
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
	arena = sbrk((intptr_t)USER_HEAP_INITIAL);
	if (arena == (void *)-1)
		zedbsd_libc_panic("unable to initialize user heap");
	zedbsd_heap_init_instance(&user_heap, arena, USER_HEAP_INITIAL);
	zedbsd_heap_set_grow_instance(&user_heap, user_heap_grow, NULL);
	zedbsd_heap_set_active(&user_heap);
}
