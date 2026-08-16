/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SPAWN_H
#define ZEDBSD_SPAWN_H

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>

#define POSIX_SPAWN_RESETIDS   0x0001
#define POSIX_SPAWN_SETPGROUP  0x0002
#define POSIX_SPAWN_SETSIGDEF  0x0004
#define POSIX_SPAWN_SETSIGMASK 0x0008

#define ZEDBSD_SPAWN_ACTION_MAX 16
#define ZEDBSD_SPAWN_PATH_MAX 256
struct zedbsd_spawn_action {
	int operation;
	int descriptor;
	int new_descriptor;
	int flags;
	mode_t mode;
	char path[ZEDBSD_SPAWN_PATH_MAX];
};
typedef struct {
	unsigned count;
	struct zedbsd_spawn_action actions[ZEDBSD_SPAWN_ACTION_MAX];
} posix_spawn_file_actions_t;
typedef struct {
	short flags;
	pid_t pgroup;
	sigset_t sigmask;
	sigset_t sigdefault;
} posix_spawnattr_t;

int posix_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *,
	const posix_spawnattr_t *, char *const [], char *const []);
int posix_spawnp(pid_t *, const char *, const posix_spawn_file_actions_t *,
	const posix_spawnattr_t *, char *const [], char *const []);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *, int,
	const char *, int, mode_t);
int posix_spawnattr_init(posix_spawnattr_t *);
int posix_spawnattr_destroy(posix_spawnattr_t *);
int posix_spawnattr_getflags(const posix_spawnattr_t *, short *);
int posix_spawnattr_setflags(posix_spawnattr_t *, short);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *, pid_t *);
int posix_spawnattr_setpgroup(posix_spawnattr_t *, pid_t);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *, sigset_t *);
int posix_spawnattr_setsigmask(posix_spawnattr_t *, const sigset_t *);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *, sigset_t *);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *, const sigset_t *);

#endif
