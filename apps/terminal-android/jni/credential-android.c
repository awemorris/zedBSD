/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The passphrases of the SSH keys mikuterm keeps on Android: in the
 * program's memory, each found by the key file's path, and on the disk
 * wrapped by the Keystore's key (Secrets.java) so that they outlive the
 * program.  The connections' threads only read and write the memory under a
 * lock; what the disk and Java need is asked of M with keiland_post_app, and
 * M answers the Keystore's replies, so no connection's thread waits for Java.
 * Every passphrase is wiped (a write the compiler keeps) before its memory
 * goes.
 *
 * The file of a passphrase is secrets/<SHA-256 of the key file's path in
 * hexadecimal> in the folder: the path's length (4 bytes, little endian), the
 * path, and what Secrets wraps (the IV and the cipher text).  M reads them all
 * when the program starts and asks Java to open each; a passphrase Java cannot
 * open (the Keystore lost its key) is left where it is and never used.
 */

#include "mikuterm.h"
#include "mikuterm-android.h"

#include "apps/libkeiland/android/keiland-android.h"
#include "userland/base/libcrypto-compat/cryc-sha2.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The most passphrases kept at once, and the longest one with its NUL. */
#define CREDENTIAL_MAX		32U
#define CREDENTIAL_SECRET_MAX	1024U

/* The most requests to Java waiting for an answer, and the biggest file of a passphrase read (the path, the length and the wrapped bytes). */
#define CREDENTIAL_PENDING	64U
#define CREDENTIAL_FILE_MAX	(MIKUTERM_PATH_MAX + CREDENTIAL_SECRET_MAX + 128U)

/* What a request to Java was for. */
#define CREDENTIAL_WRAP		1
#define CREDENTIAL_UNWRAP	2

/* One passphrase kept: the key file's path and the passphrase (an empty path: the place is free). */
struct credential_entry {
	char path[MIKUTERM_PATH_MAX];
	char secret[CREDENTIAL_SECRET_MAX];
};

/* One request to Java that waits for its answer: its number, what it is for and the key file's path (M alone reads and writes the list). */
struct credential_pending {
	uint32_t request;
	int kind;
	char path[MIKUTERM_PATH_MAX];
};

/* The passphrases kept, read and written by the connections' threads under credential_lock. */
static struct credential_entry credential_entries[CREDENTIAL_MAX];
static pthread_mutex_t credential_lock = PTHREAD_MUTEX_INITIALIZER;

/* The requests to Java that wait, and how many (a request is 0 when its place is free); M alone. */
static struct credential_pending credential_waiting[CREDENTIAL_PENDING];

/* Writes zeros through a pointer the compiler cannot see through (the C library has no explicit_bzero). */
static void *(*volatile credential_memset)(void *, int, size_t) = memset;

static struct credential_entry *credential_find(const char *path);
static void credential_wipe(struct credential_entry *entry);
static int credential_put(const char *path, const char *passphrase, size_t length);
static int credential_file(const char *path, char *file, size_t size);
static struct credential_pending *credential_request(uint32_t request);
static void credential_unwrap_file(const char *file);
static void credential_free_post(struct mikuterm_post_secret *post);

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
	struct mikuterm_post_secret *post;
	size_t path_length;
	size_t length;
	int error;

	/* A passphrase that can be kept, for a path that fits. */
	length = strlen(passphrase);
	if (length == 0U || length + 1U > CREDENTIAL_SECRET_MAX)
		return EINVAL;
	path_length = strlen(path);
	if (path_length == 0U || path_length + 1U > MIKUTERM_PATH_MAX)
		return ENAMETOOLONG;

	/* The passphrase in memory, over whatever the key's place held. */
	error = credential_put(path, passphrase, length);
	if (error != 0)
		return error;

	/* M is asked to wrap it and keep it on the disk (a failure leaves it in memory alone). */
	post = calloc(1U, sizeof(*post));
	if (post == NULL)
		return 0;
	post->path = malloc(path_length + 1U);
	post->passphrase = malloc(length + 1U);
	if (post->path == NULL || post->passphrase == NULL) {
		credential_free_post(post);
		return 0;
	}
	memcpy(post->path, path, path_length + 1U);
	memcpy(post->passphrase, passphrase, length + 1U);
	error = keiland_post_app(MIKUTERM_POST_KEEP_SECRET, post);
	if (error != 0)
		credential_free_post(post);

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
	struct mikuterm_post_secret *post;
	struct credential_entry *entry;
	size_t path_length;
	int error;

	pthread_mutex_lock(&credential_lock);

	/* The key's entry goes, when there is one. */
	entry = credential_find(path);
	if (entry != NULL)
		credential_wipe(entry);

	pthread_mutex_unlock(&credential_lock);

	/* M is asked to take the file away from the disk. */
	post = calloc(1U, sizeof(*post));
	if (post == NULL)
		return;
	path_length = strlen(path);
	post->path = malloc(path_length + 1U);
	if (post->path == NULL) {
		credential_free_post(post);
		return;
	}
	memcpy(post->path, path, path_length + 1U);
	error = keiland_post_app(MIKUTERM_POST_FORGET_SECRET, post);
	if (error != 0)
		credential_free_post(post);
}

/* Forgets every passphrase mikuterm keeps, on the disk too (called on M).  Returns how many were forgotten. */
unsigned
mikuterm_passphrases_forget_all(void)
{
	char directory[MIKUTERM_PATH_MAX];
	char file[MIKUTERM_PATH_MAX + 80U];
	struct dirent *entry;
	DIR *folder;
	unsigned forgotten;
	unsigned files;
	unsigned index;
	int written;
	int unlinked;
	int error;

	forgotten = 0U;
	files = 0U;

	/* The files on the disk: every one in the passphrases' folder. */
	error = mikuterm_path(directory, sizeof(directory), "secrets");
	if (error == 0) {
		folder = opendir(directory);
		if (folder != NULL) {
			for (;;) {
				entry = readdir(folder);
				if (entry == NULL)
					break;
				if (entry->d_name[0] == '.')
					continue;
				written = snprintf(file, sizeof(file), "%s/%s", directory, entry->d_name);
				if (written > 0 && (size_t)written < sizeof(file)) {
					unlinked = unlink(file);
					if (unlinked == 0)
						files++;
				}
			}

			(void)closedir(folder);
		}
	}

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

	/* Succeeded: how many went (a passphrase on the disk alone counts too). */
	if (files > forgotten)
		forgotten = files;
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
	return "Remember the passphrase on this device (Tab switches this)\r\n";
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

/*
 * Reads what was kept on the disk and asks Java to open each passphrase
 * (called on M when the program starts); the answers come to
 * mikuterm_credential_answer, and a recall before them finds nothing.
 */
void
mikuterm_credential_start(void)
{
	char directory[MIKUTERM_PATH_MAX];
	char file[MIKUTERM_PATH_MAX + 80U];
	struct dirent *entry;
	DIR *folder;
	int written;
	int error;

	/* The passphrases' folder, when there is one. */
	error = mikuterm_path(directory, sizeof(directory), "secrets");
	if (error != 0)
		return;
	folder = opendir(directory);
	if (folder == NULL)
		return;

	/* Each file. */
	for (;;) {
		entry = readdir(folder);
		if (entry == NULL)
			break;
		if (entry->d_name[0] == '.')
			continue;
		written = snprintf(file, sizeof(file), "%s/%s", directory, entry->d_name);
		if (written > 0 && (size_t)written < sizeof(file))
			credential_unwrap_file(file);
	}

	(void)closedir(folder);
}

/*
 * Takes Java's answer to a request (called on M, in the post hook): a
 * passphrase opened goes into memory, a passphrase wrapped goes to its file.
 * The result and its bytes are freed here.
 */
void
mikuterm_credential_answer(
	struct mikuterm_post_result *result)
{
	struct credential_pending *waiting;
	struct credential_entry *entry;
	char file[MIKUTERM_PATH_MAX + 80U];
	uint8_t *bytes;
	size_t path_length;
	size_t size;
	int error;

	/* The request this answers; one nobody waits for is dropped. */
	waiting = NULL;
	if (result->request != 0U)
		waiting = credential_request(result->request);
	if (waiting == NULL || !result->ok) {
		if (waiting != NULL)
			waiting->request = 0U;
		goto done;
	}

	/* A passphrase opened: kept in memory unless the user already gave another since. */
	if (waiting->kind == CREDENTIAL_UNWRAP) {
		pthread_mutex_lock(&credential_lock);
		entry = credential_find(waiting->path);
		pthread_mutex_unlock(&credential_lock);
		if (entry == NULL && result->size + 1U <= CREDENTIAL_SECRET_MAX && result->size != 0U) {
			result->bytes[result->size] = '\0';
			(void)credential_put(waiting->path, (const char *)result->bytes, result->size);
		}

		waiting->request = 0U;
		goto done;
	}

	/* A passphrase wrapped: the file holds the path's length, the path and the wrapped bytes. */
	path_length = strlen(waiting->path);
	size = 4U + path_length + result->size;
	bytes = malloc(size);
	if (bytes != NULL) {
		bytes[0] = (uint8_t)path_length;
		bytes[1] = (uint8_t)(path_length >> 8);
		bytes[2] = (uint8_t)(path_length >> 16);
		bytes[3] = (uint8_t)(path_length >> 24);
		memcpy(bytes + 4U, waiting->path, path_length);
		memcpy(bytes + 4U + path_length, result->bytes, result->size);
		error = credential_file(waiting->path, file, sizeof(file));
		if (error == 0)
			error = mikuterm_file_replace(file, bytes, size);
		if (error == 0)
			(void)chmod(file, 0600);
		free(bytes);
	}

	waiting->request = 0U;

done:
	/* What came is wiped and freed. */
	if (result->bytes != NULL) {
		(void)credential_memset(result->bytes, 0, result->size);
		free(result->bytes);
	}

	free(result);
}

/*
 * Asks Java to wrap a passphrase and keeps the file of it when the answer
 * comes (called on M, for a post of a connection's thread).  The post and its
 * passphrase are wiped and freed here.
 */
void
mikuterm_credential_keep(
	struct mikuterm_post_secret *post)
{
	struct credential_pending *waiting;
	uint32_t request;
	int error;

	/* A request's place and number. */
	request = keiland_android_request();
	waiting = credential_request(0U);
	if (waiting != NULL && post->passphrase != NULL) {
		waiting->request = request;
		waiting->kind = CREDENTIAL_WRAP;
		snprintf(waiting->path, sizeof(waiting->path), "%s", post->path);

		/* Asked; a request Java cannot take is no request. */
		error = mikuterm_java_secret_wrap(request, (const uint8_t *)post->passphrase, strlen(post->passphrase));
		if (error != 0)
			waiting->request = 0U;
	}

	credential_free_post(post);
}

/* Takes the file of a passphrase away (called on M, for a post of a connection's thread).  The post is freed here. */
void
mikuterm_credential_forget(
	struct mikuterm_post_secret *post)
{
	char file[MIKUTERM_PATH_MAX + 80U];
	int error;

	/* The file of the key's passphrase, when it has one. */
	error = credential_file(post->path, file, sizeof(file));
	if (error == 0)
		(void)unlink(file);
	credential_free_post(post);
}

/*
 * Puts a passphrase of a length in memory for a key file's path, over what
 * the key's place held (or in a free place).  Returns 0, EINVAL, ENAMETOOLONG
 * or ENOSPC.
 */
static int
credential_put(
	const char *path,
	const char *passphrase,
	size_t length)
{
	struct credential_entry *entry;
	size_t path_length;
	unsigned index;

	/* A passphrase that can be kept, for a path that fits. */
	if (length == 0U || length + 1U > CREDENTIAL_SECRET_MAX)
		return EINVAL;
	path_length = strlen(path);
	if (path_length == 0U || path_length + 1U > MIKUTERM_PATH_MAX)
		return ENAMETOOLONG;

	pthread_mutex_lock(&credential_lock);

	/* The key's entry, else a free place. */
	entry = credential_find(path);
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
	memcpy(entry->secret, passphrase, length);
	entry->secret[length] = '\0';

	pthread_mutex_unlock(&credential_lock);

	/* Succeeded: kept. */
	return 0;
}

/* Writes the path of the file of a key file's passphrase: secrets/ and the SHA-256 of the path in hexadecimal.  Returns 0, or ENAMETOOLONG. */
static int
credential_file(
	const char *path,
	char *file,
	size_t size)
{
	static const char digits[] = "0123456789abcdef";
	char name[CRYC_SHA256_SIZE * 2U + 1U];
	char inside[CRYC_SHA256_SIZE * 2U + 16U];
	struct cryc_sha256 hash;
	uint8_t sum[CRYC_SHA256_SIZE];
	unsigned index;
	int error;

	/* The hash of the path, as the file's name. */
	cryc_sha256_init(&hash);
	cryc_sha256_update(&hash, (const uint8_t *)path, strlen(path));
	cryc_sha256_final(&hash, sum);
	for (index = 0U; index < CRYC_SHA256_SIZE; index++) {
		name[index * 2U] = digits[sum[index] >> 4];
		name[index * 2U + 1U] = digits[sum[index] & 0x0fU];
	}

	name[CRYC_SHA256_SIZE * 2U] = '\0';

	/* Inside the folder. */
	(void)snprintf(inside, sizeof(inside), "secrets/%s", name);
	error = mikuterm_path(file, size, inside);
	if (error != 0)
		return error;

	/* Succeeded: the secret file's path. */
	return 0;
}

/* Finds the request's place that waits with a number (request non-zero), or a free place (request 0); NULL when there is none. */
static struct credential_pending *
credential_request(
	uint32_t request)
{
	unsigned index;

	/* The place. */
	for (index = 0U; index < CREDENTIAL_PENDING; index++) {
		if (credential_waiting[index].request == request)
			return &credential_waiting[index];
	}

	return NULL;
}

/* Reads the file of a passphrase and asks Java to open it. */
static void
credential_unwrap_file(
	const char *file)
{
	struct credential_pending *waiting;
	char *text;
	size_t size;
	size_t path_length;
	uint32_t request;
	int error;

	/* The file, of a size a passphrase's file has. */
	error = mikuterm_file_read(file, CREDENTIAL_FILE_MAX, &text, &size);
	if (error != 0)
		return;

	/* The path's length, the path, and the wrapped bytes after it. */
	if (size < 4U) {
		free(text);
		return;
	}

	path_length = (size_t)(unsigned char)text[0] | ((size_t)(unsigned char)text[1] << 8) | ((size_t)(unsigned char)text[2] << 16) |
	    ((size_t)(unsigned char)text[3] << 24);
	if (path_length == 0U || path_length + 1U > MIKUTERM_PATH_MAX || 4U + path_length >= size) {
		free(text);
		return;
	}

	/* A place for the request, which carries the path. */
	waiting = credential_request(0U);
	if (waiting == NULL) {
		free(text);
		return;
	}

	request = keiland_android_request();
	waiting->request = request;
	waiting->kind = CREDENTIAL_UNWRAP;
	memcpy(waiting->path, text + 4U, path_length);
	waiting->path[path_length] = '\0';

	/* Asked; a request Java cannot take is no request. */
	error = mikuterm_java_secret_unwrap(request, (const uint8_t *)text + 4U + path_length, size - 4U - path_length);
	if (error != 0)
		waiting->request = 0U;
	free(text);
}

/* Wipes and frees a post of a passphrase. */
static void
credential_free_post(
	struct mikuterm_post_secret *post)
{
	/* The passphrase is wiped first. */
	if (post->passphrase != NULL) {
		(void)credential_memset(post->passphrase, 0, strlen(post->passphrase));
		free(post->passphrase);
	}

	free(post->path);
	free(post);
}
