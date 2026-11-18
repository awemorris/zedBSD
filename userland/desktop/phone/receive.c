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

/* The import's rooms are the library's and libmms's. */
_Static_assert(PH_IMPORT_PATH_MAX == PH_PATH_MAX, "an import's path is a library path");
_Static_assert(PH_IMPORT_HASH_SIZE == PH_HASH_SIZE, "an import's hash is the library's");
_Static_assert(PH_MEDIA_MAX == MMS_MEDIA_COUNT, "an import holds an MMS's media");

static int receive_document(int descriptor, struct mms_document *document);
static int receive_file(const struct mms_media *media, const char *directory, size_t index, char *path, size_t size);

/*
 * Copies attachments to mediastorage, returning its permanent paths by
 * hash, and waits for it (a worker's call: the window's thread uses
 * ph_import_prepare, ph_import_begin and ph_import_finish).  The caller
 * retains the MIME descriptor and releases the returned paths.
 */
int
ph_receive_media(
	struct kl_system *system,
	int descriptor,
	struct ph_received *received)
{
	struct ph_import *import;
	const char *paths[MMS_MEDIA_COUNT];
	size_t index;
	int metadata;
	int error;

	/* The originals written apart; text-only MMS needs no library operation. */
	memset(received, 0, sizeof(*received));
	error = ph_import_prepare(descriptor, &import);
	if (error != 0)
		return error;
	if (import == NULL)
		return 0;

	/* The compositor's OS backend runs the CLI and returns committed metadata. */
	for (index = 0U; index < import->count; index++)
		paths[index] = import->temporary[index];
	metadata = -1;
	error = kl_system_media_add_paths(system, paths, import->count, &metadata);

	/* The paths it kept, and the temporary originals gone. */
	error = ph_import_finish(import, error, metadata, received);
	ph_import_free(import);
	if (error != 0)
		return error;

	/* Succeeded: the complete operation is ready for its caller. */
	return 0;
}

/*
 * Reads an MMS's MIME body and writes its media apart, each hashed
 * (ws157-p014): *import is the import to begin (NULL for an MMS without
 * media).  Returns 0 or an errno value (nothing is left).
 */
int
ph_import_prepare(
	int descriptor,
	struct ph_import **import)
{
	struct mms_document document;
	struct ph_import *made;
	SHA2_CTX context;
	char *created;
	size_t index;
	int error;
	int same;

	/* Refuses incomplete MIME before writing any original. */
	*import = NULL;
	error = receive_document(descriptor, &document);
	if (error != 0)
		return error;

	/* Text-only MMS needs no library operation. */
	if (document.count == 0U) {
		mms_release(&document);
		return 0;
	}

	/* The import's record. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL) {
		mms_release(&document);
		return ENOMEM;
	}

	/* Uses a private temporary directory only until the CLI copies originals. */
	(void)snprintf(made->directory, sizeof(made->directory), "/tmp/phone-mms-XXXXXX");
	created = mkdtemp(made->directory);
	if (created == NULL) {
		error = errno;
		free(made);
		mms_release(&document);
		return error;
	}

	/* Copies and hashes every part, and notes each video. */
	for (index = 0U; index < document.count; index++) {
		error = receive_file(&document.media[index], made->directory, index, made->temporary[index], sizeof(made->temporary[index]));
		if (error != 0)
			break;
		made->count++;
		SHA256Init(&context);
		SHA256Update(&context, document.media[index].data, document.media[index].length);
		(void)SHA256End(&context, made->hashes[index]);
		same = strncmp(document.media[index].type, "video/", 6U);
		if (same == 0)
			made->video[index] = 1U;
	}

	/* The MIME no longer needed; a part not written leaves nothing. */
	mms_release(&document);
	if (error != 0) {
		ph_import_free(made);
		return error;
	}

	/* Succeeded: ready to begin. */
	*import = made;
	return 0;
}

/*
 * Asks the compositor to copy an import's media into the library, without
 * waiting (ws157-p014): import->request is its request.  Returns 0, or
 * EBUSY with too many requests under way (begin it again later), or
 * another errno value.
 */
int
ph_import_begin(
	struct kl_system *system,
	struct ph_import *import)
{
	const char *paths[MMS_MEDIA_COUNT];
	size_t index;
	int error;

	/* The temporary originals' paths. */
	for (index = 0U; index < import->count; index++)
		paths[index] = import->temporary[index];

	/* The request. */
	error = kl_system_media_add_paths_begin(system, paths, import->count, &import->request);
	if (error != 0)
		return error;

	/* Succeeded: under way. */
	return 0;
}

/*
 * Ends an import with its answer (error, and the snapshot's descriptor,
 * closed here): each medium's permanent path found by its hash in the
 * snapshot.  The temporary originals are left for ph_import_free.
 * Returns 0 with received (the caller releases it) or an errno value.
 */
int
ph_import_finish(
	struct ph_import *import,
	int error,
	int metadata,
	struct ph_received *received)
{
	struct ph_photo *photos;
	char root[PH_PATH_MAX];
	FILE *snapshot;
	size_t count;
	size_t index;
	long found;
	int closed;

	/* The snapshot, read into the library model. */
	memset(received, 0, sizeof(*received));
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
	} else if (metadata >= 0) {
		close(metadata);
	}

	/* Resolves deduplicated and renamed originals without guessing their date folders. */
	if (error == 0) {
		photos = ph_photos(&count);
		for (index = 0U; index < import->count; index++) {
			found = ph_library_find_hash(import->hashes[index]);
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
			received->video[index] = import->video[index];
			received->count++;
		}
	}

	/* The model is no longer needed; a failure gives no paths. */
	ph_library_release();
	if (error != 0)
		ph_received_release(received);
	if (error != 0)
		return error;

	/* Succeeded: the paths the library keeps. */
	return 0;
}

/*
 * Removes an import's temporary originals and frees it.
 */
void
ph_import_free(
	struct ph_import *import)
{
	size_t index;

	/* Nothing to free. */
	if (import == NULL)
		return;

	/* Cleans only temporary originals owned by this import, even after a partial CLI failure. */
	for (index = 0U; index < import->count; index++)
		(void)unlink(import->temporary[index]);
	(void)rmdir(import->directory);
	free(import);
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
