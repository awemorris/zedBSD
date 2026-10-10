/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Imports bounded MMS media through the compositor; original bytes use files. */
#include "phone.h"
#include "userland/desktop/libmms/mms.h"
#include "userland/desktop/photos/photos.h"

#include <errno.h>
#include <sha2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int receive_document(int descriptor, struct mms_document *document);
static int receive_file(const struct mms_media *media, const char *directory, size_t index, char *path, size_t size);

/*
 * Copies attachments to mediastorage, returning its permanent paths by hash.
 * The caller retains the MIME descriptor and releases the returned paths.
 */
int
ph_receive_media(
	struct kl_system *system,
	int descriptor,
	struct ph_received *received)
{
	struct mms_document document;
	struct ph_photo *photos;
	SHA2_CTX context;
	char directory[] = "/tmp/phone-mms-XXXXXX";
	char temporary[MMS_MEDIA_COUNT][PH_PATH_MAX];
	char hashes[MMS_MEDIA_COUNT][PH_HASH_SIZE];
	const char *paths[MMS_MEDIA_COUNT];
	char root[PH_PATH_MAX];
	FILE *snapshot;
	char *created;
	size_t made;
	size_t count;
	size_t index;
	long found;
	int metadata;
	int error;
	int same;
	int closed;

	/* Refuses incomplete MIME before writing any original. */
	memset(received, 0, sizeof(*received));
	error = receive_document(descriptor, &document);
	if (error != 0)
		return error;

	/* Text-only MMS needs no library operation. */
	if (document.count == 0U) {
		mms_release(&document);
		return 0;
	}

	/* Uses a private temporary directory only until the CLI copies originals. */
	created = mkdtemp(directory);
	if (created == NULL) {
		error = errno;
		mms_release(&document);
		return error;
	}

	/* Copies and hashes every part before issuing the path-list request. */
	made = 0U;
	for (index = 0U; index < document.count; index++) {
		error = receive_file(&document.media[index], directory, index, temporary[index], sizeof(temporary[index]));
		if (error != 0)
			break;
		made++;
		paths[index] = temporary[index];
		SHA256Init(&context);
		SHA256Update(&context, document.media[index].data, document.media[index].length);
		(void)SHA256End(&context, hashes[index]);
	}

	/* The compositor's OS backend runs the CLI and returns committed metadata. */
	metadata = -1;
	if (error == 0)
		error = kl_system_media_add_paths(system, paths, document.count, &metadata);
	ph_library_release();
	if (error == 0) {
		snapshot = fdopen(metadata, "r");
		if (snapshot == NULL) {
			error = errno;
			close(metadata);
		} else {
			error = ph_snapshot_read(snapshot, root, sizeof(root));
			closed = fclose(snapshot);
			if (closed != 0 && error == 0)
				error = EIO;
		}
	}

	/* Resolves deduplicated and renamed originals without guessing their date folders. */
	if (error == 0) {
		photos = ph_photos(&count);
		for (index = 0U; index < document.count; index++) {
			found = ph_library_find_hash(hashes[index]);
			if (found < 0 || (size_t)found >= count) {
				error = ENOENT;
				break;
			}

			/* Owns the canonical path independently of the snapshot model. */
			received->paths[index] = strdup(photos[found].path);
			if (received->paths[index] == NULL) {
				error = ENOMEM;
				break;
			}

			/* Classifies the saved original for the timeline. */
			same = strncmp(document.media[index].type, "video/", 6U);
			if (same == 0)
				received->video[index] = 1U;
			received->count++;
		}
	}

	/* Cleans only temporary originals owned by this import, even after a partial CLI failure. */
	for (index = 0U; index < made; index++)
		(void)unlink(temporary[index]);
	(void)rmdir(directory);
	ph_library_release();
	mms_release(&document);
	if (error != 0)
		ph_received_release(received);

	/* Reports a failed import without exposing partially resolved paths. */
	if (error != 0)
		return error;

	/* Succeeded: the complete operation is ready for its caller. */
	return 0;
}

/*
 * Releases paths copied from a completed metadata snapshot.
 */
void
ph_received_release(
	struct ph_received *received)
{
	size_t index;

	/* The permanent originals remain owned by mediastorage. */
	for (index = 0U; index < received->count; index++)
		free(received->paths[index]);
	memset(received, 0, sizeof(*received));
}

/* Reads a complete MIME file independently of the offset shared by FD duplicates. */
static int
receive_document(
	int descriptor,
	struct mms_document *document)
{
	struct stat info;
	uint8_t *input;
	ssize_t got;
	size_t used;
	int error;
	int regular;

	/* A bounded regular spool completed by the backend. */
	error = fstat(descriptor, &info);
	if (error != 0)
		return errno;
	regular = S_ISREG(info.st_mode);
	if (!regular || info.st_size <= 0 || (uint64_t)info.st_size > MMS_INPUT_MAX)
		return EFBIG;
	input = malloc((size_t)info.st_size);
	if (input == NULL)
		return ENOMEM;

	/* pread lets several Phone clients read the same original safely. */
	used = 0U;
	error = 0;
	while (used < (size_t)info.st_size) {
		got = pread(descriptor, input + used, (size_t)info.st_size - used, (off_t)used);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			error = EIO;
			break;
		}

		/* Counts the bytes obtained from this fixed file offset. */
		used += (size_t)got;
	}

	/* Publishes only a completely parsed document. */
	if (error == 0)
		error = mms_parse(input, used, document);
	free(input);
	/* Reports a failed import without exposing partially resolved paths. */
	if (error != 0)
		return error;

	/* Succeeded: the complete operation is ready for its caller. */
	return 0;
}

/* Materializes one original with a type-derived extension for CLI classification. */
static int
receive_file(
	const struct mms_media *media,
	const char *directory,
	size_t index,
	char *path,
	size_t size)
{
	FILE *file;
	const char *extension;
	const char *name;
	char clean[MMS_NAME_MAX];
	size_t at;
	size_t used;
	size_t written;
	int same;
	int length;
	int closed;
	const char *dot;
	size_t extension_length;

	/* Keeps the source's basename, excluding separators and control characters. */
	name = media->name;
	used = 0U;
	for (at = 0U; name[at] != '\0' && used + 1U < sizeof(clean); at++) {
		if ((unsigned char)name[at] < 32U || name[at] == '/' || name[at] == '\\')
			continue;
		clean[used++] = name[at];
	}

	/* Ends the sanitized basename before choosing its extension. */
	clean[used] = '\0';
	extension = ".bin";
	same = strcmp(media->type, "image/jpeg");
	if (same == 0)
		extension = ".jpg";
	same = strcmp(media->type, "image/png");
	if (same == 0)
		extension = ".png";
	same = strcmp(media->type, "image/gif");
	if (same == 0)
		extension = ".gif";
	same = strcmp(media->type, "video/mp4");
	if (same == 0)
		extension = ".mp4";
	same = strcmp(media->type, "video/3gpp");
	if (same == 0)
		extension = ".3gp";
	same = strcmp(media->type, "video/quicktime");
	if (same == 0)
		extension = ".mov";
	if (used == 0U || clean[0] == '.')
		(void)snprintf(clean, sizeof(clean), "attachment-%lu%s", (unsigned long)index + 1U, extension);

	/* MIME names often have no suffix; the CLI still needs one for video classification. */
	dot = strchr(clean, '.');
	extension_length = strlen(extension);
	if (dot == NULL) {
		used = strlen(clean);
		if (used + extension_length + 1U > sizeof(clean))
			return ENAMETOOLONG;
		(void)snprintf(clean + used, sizeof(clean) - used, "%s", extension);
	}

	/* Each part is independently materialized, including duplicate attachment names. */
	length = snprintf(path, size, "%s/%lu-%s", directory, (unsigned long)index + 1U, clean);
	if (length < 0 || (size_t)length >= size)
		return ENAMETOOLONG;
	file = fopen(path, "wb");
	if (file == NULL)
		return errno;
	written = fwrite(media->data, 1U, media->length, file);
	closed = fclose(file);
	if (written != media->length || closed != 0) {
		(void)unlink(path);
		return EIO;
	}

	/* Succeeded: the requested operation is complete. */
	return 0;
}
