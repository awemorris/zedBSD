/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UNISTD_H
#define ZEDBSD_UNISTD_H

#include <zedbsd/features.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define _SC_PAGE_SIZE 1
#define _SC_PAGESIZE _SC_PAGE_SIZE
#define _SC_OPEN_MAX 2
#define _SC_CLK_TCK 3
#define _SC_JOB_CONTROL 4
#define _SC_THREADS 5
#define _SC_THREAD_PROCESS_SHARED 6
#define _SC_REALTIME_SIGNALS 7
#define _SC_SHARED_MEMORY_OBJECTS 8
#define _SC_SEMAPHORES 9
#define _SC_MESSAGE_PASSING 10
#define _SC_VERSION 11
#define _SC_2_VERSION 12
#define _SC_ARG_MAX 13
#define _SC_CHILD_MAX 14
#define _SC_STREAM_MAX 15
#define _SC_NPROCESSORS_CONF 16
#define _SC_NPROCESSORS_ONLN 17
#define _SC_THREAD_KEYS_MAX 18
#define _SC_THREAD_DESTRUCTOR_ITERATIONS 19
#define _SC_THREAD_STACK_MIN 20
#define _SC_THREAD_THREADS_MAX 21
#define _SC_SEM_NSEMS_MAX 22
#define _SC_SEM_VALUE_MAX 23
#define _SC_MQ_OPEN_MAX 24
#define _SC_MQ_PRIO_MAX 25
#define _SC_TIMERS 26
#define _SC_XOPEN_VERSION 27
#define _SC_XOPEN_UNIX 28
#define _SC_RTSIG_MAX 29
#define _SC_SIGQUEUE_MAX 30

#define _PC_LINK_MAX 1
#define _PC_MAX_CANON 2
#define _PC_MAX_INPUT 3
#define _PC_NAME_MAX 4
#define _PC_PATH_MAX 5
#define _PC_PIPE_BUF 6
#define _PC_CHOWN_RESTRICTED 7
#define _PC_NO_TRUNC 8
#define _PC_VDISABLE 9

#define _CS_PATH 1

int access(const char *path, int mode);
int faccessat(int, const char *, int, int);
char *getcwd(char *buffer, size_t size);
int chdir(const char *path);
int fchdir(int descriptor);
unsigned sleep(unsigned seconds);
int usleep(useconds_t microseconds);
int pause(void);
char *getlogin(void);
int getlogin_r(char *, size_t);
int gethostname(char *, size_t);
int sethostname(const char *, size_t);
char *ttyname(int);
int ttyname_r(int, char *, size_t);
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
long pathconf(const char *, int);
long fpathconf(int, int);
size_t confstr(int, char *, size_t);
pid_t fork(void);
unsigned alarm(unsigned);
int execl(const char *, const char *, ...);
int execle(const char *, const char *, ...);
int execlp(const char *, const char *, ...);
int execv(const char *, char *const []);
int execve(const char *, char *const [], char *const []);
int execvp(const char *, char *const []);
int fexecve(int, char *const [], char *const []);
extern char **environ;
extern char *optarg;
extern int opterr, optind, optopt;
int getopt(int, char *const [], const char *);
pid_t getpid(void);
pid_t getppid(void);
pid_t getpgrp(void);
pid_t getpgid(pid_t);
int setpgid(pid_t, pid_t);
int setpgrp(void);
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

#define F_ULOCK 0
#define F_LOCK  1
#define F_TLOCK 2
#define F_TEST  3

int lockf(int, int, off_t);
long gethostid(void);
int nice(int);
char *crypt(const char *, const char *);
void encrypt(char [64], int);
void swab(const void *, void *, ssize_t);
void sync(void);

#endif
