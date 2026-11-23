/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The passphrases of the SSH keys mikuterm keeps on Android: in the
 * program's memory alone, until it ends (nothing is written to the disk
 * and nothing is handed to another program).  Each is found by the key
 * file's path.  Every passphrase is wiped (a write the compiler keeps)
 * before its memory goes.
 */

#include "mikuterm.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The most passphrases kept at once, and the longest one with its NUL. */
#define CREDENTIAL_MAX		32U
#define CREDENTIAL_SECRET_MAX	1024U

/* One passphrase kept: the key file's path and the passphrase (an empty path: the place is free). */
struct credential_entry {
	char path[MIKUTERM_PATH_MAX];
	char secret[CREDENTIAL_SECRET_MAX];
};

/* The passphrases kept, read and written by the connections' threads under credential_lock. */
static struct credential_entry credential_entries[CREDENTIAL_MAX];
static pthread_mutex_t credential_lock = PTHREAD_MUTEX_INITIALIZER;

/* Writes zeros through a pointer the compiler cannot see through (the C library has no explicit_bzero). */
static void *(*volatile credential_memset)(void *, int, size_t) = memset;

static struct credential_entry *credential_find(const char *path);
static void credential_wipe(struct credential_entry *entry);

/*
 * Finds the passphrase kept for the key file at path and copies it, with
 * its NUL, into passphrase (size bytes).  Returns 0, ENOENT (none is
 * kept) or ENOBUFS (it does not fit; nothing is copied).
 */
int
mikuterm_passphrase_recall(
	const char *path,
	char *passphrase,
	size_t size)
{
	struct credential_entry *entry;
	size_t length;

	pthread_mutex_lock(&credential_lock);

	/* The key's entry, when one is kept. */
	entry = credential_find(path);
	if (entry == NULL) {
		pthread_mutex_unlock(&credential_lock);
		return ENOENT;
	}

	/* Its passphrase, when it fits. */
	length = strlen(entry->secret);
	if (length + 1U > size) {
		pthread_mutex_unlock(&credential_lock);
		return ENOBUFS;
	}
	memcpy(passphrase, entry->secret, length + 1U);

	pthread_mutex_unlock(&credential_lock);

	/* Succeeded: the passphrase. */
	return 0;
}

/*
 * Keeps passphrase (NUL-terminated) for the key file at path until
 * mikuterm ends, replacing one kept before.  Returns 0, EINVAL (empty, or
 * longer than one is kept), ENAMETOOLONG or ENOSPC (every place is
 * taken).
 */
int
mikuterm_passphrase_keep(
	const char *path,
	const char *passphrase)
{
	struct credential_entry *entry;
	size_t path_length;
	size_t length;
	unsigned index;

	/* A passphrase that can be kept, for a path that fits. */
	length = strlen(passphrase);
	if (length == 0U || length + 1U > CREDENTIAL_SECRET_MAX)
		return EINVAL;
	path_length = strlen(path);
	if (path_length == 0U || path_length + 1U > MIKUTERM_PATH_MAX)
		return ENAMETOOLONG;

	pthread_mutex_lock(&credential_lock);

	/* The key's entry, else a free place. */
	entry = credential_find(path);

	/* Looks for a free place when the key has none. */
	for (index = 0U; entry == NULL && index < CREDENTIAL_MAX; index++) {
		/* A place with no path is free. */
		if (credential_entries[index].path[0] == '\0')
			entry = &credential_entries[index];
	}

	/* Every place is taken. */
	if (entry == NULL) {
		pthread_mutex_unlock(&credential_lock);
		return ENOSPC;
	}

	/* The passphrase over whatever the place held. */
	credential_wipe(entry);
	memcpy(entry->path, path, path_length + 1U);
	memcpy(entry->secret, passphrase, length + 1U);

	pthread_mutex_unlock(&credential_lock);

	/* Succeeded: kept. */
	return 0;
}

/*
 * Forgets the passphrase kept for the key file at path; one that is not
 * kept is no error.
 */
void
mikuterm_passphrase_forget(
	const char *path)
{
	struct credential_entry *entry;

	pthread_mutex_lock(&credential_lock);

	/* The key's entry goes, when there is one. */
	entry = credential_find(path);
	if (entry != NULL)
		credential_wipe(entry);

	pthread_mutex_unlock(&credential_lock);
}

/* Forgets every passphrase mikuterm keeps.  Returns how many were forgotten. */
unsigned
mikuterm_passphrases_forget_all(void)
{
	unsigned forgotten;
	unsigned index;

	forgotten = 0U;

	pthread_mutex_lock(&credential_lock);

	/* Each place that holds one is wiped. */
	for (index = 0U; index < CREDENTIAL_MAX; index++) {
		/* A free place holds nothing. */
		if (credential_entries[index].path[0] == '\0')
			continue;
		credential_wipe(&credential_entries[index]);
		forgotten++;
	}

	pthread_mutex_unlock(&credential_lock);

	/* Succeeded: how many went. */
	return forgotten;
}

/*
 * Returns the words of the line that says how long a passphrase is kept,
 * shown after the box that chooses whether to keep it.
 */
const char *
mikuterm_passphrase_keep_text(void)
{
	/* How long this system keeps a passphrase. */
	return "Remember the passphrase until mikuterm ends (Tab switches this)\r\n";
}

/* Finds the entry of a key file's path (NULL: none); called under credential_lock. */
static struct credential_entry *
credential_find(
	const char *path)
{
	unsigned index;
	int differs;

	/* The entry whose path is the same. */
	for (index = 0U; index < CREDENTIAL_MAX; index++) {
		/* A free place is no key's. */
		if (credential_entries[index].path[0] == '\0')
			continue;
		differs = strcmp(credential_entries[index].path, path);
		if (differs == 0)
			return &credential_entries[index];
	}

	/* None is kept. */
	return NULL;
}

/* Wipes an entry, which leaves its place free; called under credential_lock. */
static void
credential_wipe(
	struct credential_entry *entry)
{
	/* The passphrase and the path, in a way the compiler keeps. */
	(void)credential_memset(entry->secret, 0, sizeof(entry->secret));
	(void)credential_memset(entry->path, 0, sizeof(entry->path));
}
