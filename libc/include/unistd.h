/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UNISTD_H
#define ZEDBSD_UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define _SC_PAGE_SIZE 1
#define _SC_PAGESIZE _SC_PAGE_SIZE

int access(const char *path, int mode);
int faccessat(int, const char *, int, int);
char *getcwd(char *buffer, size_t size);
int chdir(const char *path);
int isatty(int descriptor);
int fileno(void *stream);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
ssize_t pread(int, void *, size_t, off_t);
ssize_t pwrite(int, const void *, size_t, off_t);
int fsync(int);
int fdatasync(int);
int unlink(const char *);
int unlinkat(int, const char *, int);
int rmdir(const char *);
int renameat(int, const char *, int, const char *);
int link(const char *, const char *);
int linkat(int, const char *, int, const char *, int);
int symlink(const char *, const char *);
int symlinkat(const char *, int, const char *);
ssize_t readlink(const char *, char *, size_t);
ssize_t readlinkat(int, const char *, char *, size_t);
int truncate(const char *, off_t);
int ftruncate(int, off_t);
int close(int);
int dup(int);
int dup2(int, int);
int dup3(int, int, int);
int pipe(int [2]);
int pipe2(int [2], int);
off_t lseek(int, off_t, int);
void _exit(int) __attribute__((noreturn));
int brk(void *address);
void *sbrk(intptr_t increment);
long sysconf(int name);
pid_t fork(void);
int execve(const char *, char *const [], char *const []);
pid_t getpid(void);
pid_t getppid(void);
pid_t getpgrp(void);
pid_t getpgid(pid_t);
int setpgid(pid_t, pid_t);
pid_t setsid(void);
pid_t getsid(pid_t);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int getgroups(int, gid_t []);
int setuid(uid_t);
int seteuid(uid_t);
int setgid(gid_t);
int setegid(gid_t);
int setgroups(size_t, const gid_t []);
int setreuid(uid_t, uid_t);
int setregid(gid_t, gid_t);
int chown(const char *, uid_t, gid_t);
int fchown(int, uid_t, gid_t);
int lchown(const char *, uid_t, gid_t);
int fchownat(int, const char *, uid_t, gid_t, int);

#endif
