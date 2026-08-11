/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "user/libc/syscall.h"
#include "libc/heap.h"

#include <zedbsd/dirent.h>
#include <zedbsd/syscall.h>
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
#include <time.h>
#include <unistd.h>

char **environ;
static char *environment_overrides[32];

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
	for (i = 0; i < 32U; i++)
		if (environment_name(environment_overrides[i], name))
			return strchr(environment_overrides[i], '=') + 1;
	for (i = 0; environ != NULL && environ[i] != NULL; i++)
		if (environment_name(environ[i], name))
			return strchr(environ[i], '=') + 1;
	return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
	size_t name_length, value_length;
	char *entry;
	unsigned i, empty = 32U;
	if (name == NULL || value == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL; return -1;
	}
	if (!overwrite && getenv(name) != NULL)
		return 0;
	for (i = 0; i < 32U; i++) {
		if (environment_overrides[i] == NULL && empty == 32U) empty = i;
		if (environment_name(environment_overrides[i], name)) { empty = i; break; }
	}
	if (empty == 32U) { errno = ENOSPC; return -1; }
	name_length = strlen(name); value_length = strlen(value);
	if (name_length > SIZE_MAX - value_length - 2U) { errno = ENOMEM; return -1; }
	entry = malloc(name_length + value_length + 2U);
	if (entry == NULL) { errno = ENOMEM; return -1; }
	memcpy(entry, name, name_length); entry[name_length] = '=';
	memcpy(entry + name_length + 1U, value, value_length + 1U);
	free(environment_overrides[empty]); environment_overrides[empty] = entry;
	return 0;
}

int unsetenv(const char *name)
{
	unsigned i;
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL; return -1;
	}
	for (i = 0; i < 32U; i++)
		if (environment_name(environment_overrides[i], name)) {
			free(environment_overrides[i]); environment_overrides[i] = NULL;
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
int close(int fd) { return (int)call(ZEDBSD_SYS_close, fd, 0, 0, 0, 0, 0); }
ssize_t read(int fd, void *p, size_t n) { return (ssize_t)call(ZEDBSD_SYS_read, fd, (uintptr_t)p, n, 0, 0, 0); }
ssize_t write(int fd, const void *p, size_t n) { return (ssize_t)call(ZEDBSD_SYS_write, fd, (uintptr_t)p, n, 0, 0, 0); }
off_t lseek(int fd, off_t off, int whence) { return (off_t)call(ZEDBSD_SYS_lseek, fd, off, whence, 0, 0, 0); }
int fstat(int fd, struct stat *st) { return (int)call(ZEDBSD_SYS_fstat, fd, (uintptr_t)st, 0, 0, 0, 0); }
int chdir(const char *p) { return (int)call(ZEDBSD_SYS_chdir, (uintptr_t)p, 0, 0, 0, 0, 0); }
char *getcwd(char *p, size_t n) { return (char *)call(ZEDBSD_SYS_getcwd, (uintptr_t)p, n, 0, 0, 0, 0); }
int ioctl(int fd, unsigned long request, ...) {
	va_list ap; uintptr_t arg = 0;
	if (((request >> 16) & 0x1fffUL) != 0) {
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
int clock_gettime(clockid_t id, struct timespec *ts) { return (int)call(ZEDBSD_SYS_clock_gettime, id, (uintptr_t)ts, 0, 0, 0, 0); }
int nanosleep(const struct timespec *request, struct timespec *remain) {
	return (int)call(ZEDBSD_SYS_nanosleep, (uintptr_t)request,
		(uintptr_t)remain, 0, 0, 0, 0);
}

int stat(const char *path, struct stat *status)
{
	int fd = open(path, O_RDONLY);
	int result;
	if (fd < 0) return -1;
	result = fstat(fd, status);
	(void)close(fd);
	return result;
}
int access(const char *path, int mode)
{
	struct stat status;
	if (mode != F_OK) { errno = EINVAL; return -1; }
	return stat(path, &status);
}
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
	struct timespec ts; time_t value = clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? ts.tv_sec : (time_t)-1;
	if (result != NULL)
		*result = value;
	return value;
}
void exit(int status) { (void)fflush(NULL); _exit(status); }
void zedbsd_libc_panic(const char *message) { (void)write(2, message, strlen(message)); (void)write(2, "\n", 1); _exit(127); }

static struct zedbsd_heap user_heap;
void zedbsd_user_libc_init(int argc, char **argv, char **envp)
{
	static const size_t sizes[] = { 32U << 20, 16U << 20, 8U << 20, 4U << 20, 2U << 20 };
	void *arena = MAP_FAILED; size_t i;
	(void)argc; (void)argv; environ = envp;
	stdin->context = (void *)(intptr_t)1;
	stdout->context = (void *)(intptr_t)2;
	stderr->context = (void *)(intptr_t)3;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		arena = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (arena != MAP_FAILED) { zedbsd_heap_init_instance(&user_heap, arena, sizes[i]); zedbsd_heap_set_active(&user_heap); return; }
	}
	zedbsd_libc_panic("unable to initialize user heap");
}
