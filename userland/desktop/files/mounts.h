/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mounted file systems as the desktop told them (mounts.c, ws188-p004):
 * a walk over a copy of the last answer, which main.c puts in place.
 */
#ifndef FM_MOUNTS_H
#define FM_MOUNTS_H

#include <stddef.h>

struct fm_mounts;
struct kl_machine_mount;

/* One current mount record borrows strings until the next iterator read or final close. */
struct fm_mount {
	const char *path;
	const char *type;
};

/* Open publishes an owned iterator on success zero; failure returns a positive errno unchanged. */
int fm_mounts_open(struct fm_mounts **mounts);
/* Next returns one record, zero at end, or -1/errno; refusal/end leaves the record unchanged. */
int fm_mounts_next(struct fm_mounts *mounts, struct fm_mount *mount);
/* Close tolerates NULL and retires only this iterator's native ownership. */
void fm_mounts_close(struct fm_mounts *mounts);
/* Puts the desktop's last answer (libkeiland's kl_system_machine_mounts) in place of the mounts known. */
void fm_mounts_set(const struct kl_machine_mount *list, size_t count);

#endif
