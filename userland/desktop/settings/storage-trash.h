/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Storage page's Trash (storage-trash.c, ws089-p023): where the
 * user's trash is (the freedesktop.org home trash, as Files keeps it) and
 * emptying it on a thread of its own.  The trash's size is counted by an
 * analysis of its files (storage-scan.c).
 */

#ifndef SE_STORAGE_TRASH_H
#define SE_STORAGE_TRASH_H

#include <keiland/keiland_thread.h>

#include <stddef.h>
#include <stdint.h>

/* Where emptying has got to. */
#define SE_TRASH_IDLE		0U
#define SE_TRASH_EMPTYING	1U
#define SE_TRASH_EMPTIED	2U

/*
 * The emptying of a trash: its folder, the thread, its state, how many
 * items went, and the first error met (0 for none).  lock guards the
 * state, the count and the error.
 */
struct se_trash {
	struct kl_mutex lock;
	struct kl_thread *thread;
	int started;
	char path[1024];
	unsigned state;
	uint64_t removed;
	int error;
};

int se_trash_path(char *path, size_t size);
int se_trash_empty_start(struct se_trash *trash, const char *path);
unsigned se_trash_poll(struct se_trash *trash, uint64_t *removed, int *error);
void se_trash_finish(struct se_trash *trash);

#endif
