/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The browser's storage on Windows: a backend that wraps the files
 * backend (shell_storage_wrap) and keeps the persistent cookies'
 * records encrypted for the current user with the Data Protection API
 * (CryptProtectData without a prompt), as the Windows browsers keep their
 * cookies.  The other spaces (the HTTP cache, localStorage) pass through
 * as they are.
 *
 * A cookie's record on disk is PROTECT_MAGIC and then the protected blob.
 * A record without the magic (written before the wrapping) is read as it
 * is and written protected at its next change; one that does not
 * unprotect (another user's, another machine's, damaged) reads as
 * missing, which the engine treats as a cookie it does not have.
 */

#include "userland/desktop/browser/shell/shell.h"

#include <browser/storage.h>

#include <windows.h>
#include <wincrypt.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* What a protected record starts with, and its length. */
#define PROTECT_MAGIC		"OGDPAPI1"
#define PROTECT_MAGIC_LENGTH	8U

/*
 * The wrapping backend's context: the backend it wraps (its table and its
 * context, which this one owns and closes).
 */
struct protect_backend {
	const struct browser_storage_calls *inner;
	void *inner_context;
};

static int protect_read(void *context, unsigned space, const char *key, struct browser_storage_sink *sink, int *found);
static int protect_write(void *context, unsigned space, const char *key, const void *bytes, size_t length);
static int protect_remove(void *context, unsigned space, const char *key);
static int protect_list(void *context, unsigned space, int (*each)(void *each_context, const char *key), void *each_context);
static int protect_clear(void *context, unsigned space);
static void protect_close(void *context);
static int protect_unseal(const unsigned char *bytes, size_t length, struct browser_storage_sink *sink, int *found);

/* The wrapping backend's table; constant for the life of the program. */
static const struct browser_storage_calls protect_calls = {
	BROWSER_STORAGE_VERSION,
	protect_read,
	protect_write,
	protect_remove,
	protect_list,
	protect_clear,
	protect_close
};

/*
 * Wraps a backend (shell.h): *calls and *context become the wrapping
 * backend's, which owns the one given.  On failure (ENOMEM) the backend
 * given is closed and both are NULL.
 */
int
shell_storage_wrap(
	const struct browser_storage_calls **calls,
	void **context)
{
	struct protect_backend *backend;

	/* The wrapping backend's context. */
	backend = calloc(1U, sizeof(*backend));
	if (backend == NULL) {
		if ((*calls)->close != NULL)
			(*calls)->close(*context);
		*calls = NULL;
		*context = NULL;
		return ENOMEM;
	}
	backend->inner = *calls;
	backend->inner_context = *context;

	/* Succeeded: the wrapping backend stands for the one given. */
	*calls = &protect_calls;
	*context = backend;
	return 0;
}

/*
 * Reads a record: a cookie's through a sink of its own, unprotected into
 * the engine's sink; any other space's straight into the engine's sink.
 */
static int
protect_read(
	void *context,
	unsigned space,
	const char *key,
	struct browser_storage_sink *sink,
	int *found)
{
	struct protect_backend *backend;
	struct browser_storage_sink *own;
	const void *bytes;
	size_t length;
	int error;

	/* Only the cookies are protected. */
	backend = context;
	if (space != BROWSER_STORAGE_COOKIES) {
		error = backend->inner->read(backend->inner_context, space, key, sink, found);
		if (error != 0)
			return error;

		/* Succeeded: the record, as it is. */
		return 0;
	}

	/* The record on disk, into a sink of this backend's. */
	*found = 0;
	error = browser_storage_sink_create(&own);
	if (error != 0)
		return error;
	error = backend->inner->read(backend->inner_context, space, key, own, found);
	if (error != 0) {
		browser_storage_sink_destroy(own);
		return error;
	}

	/* A missing record. */
	if (!*found) {
		browser_storage_sink_destroy(own);
		return 0;
	}

	/* Its bytes, unprotected. */
	error = browser_storage_sink_bytes(own, &bytes, &length);
	if (error == 0)
		error = protect_unseal(bytes, length, sink, found);
	browser_storage_sink_destroy(own);
	if (error != 0)
		return error;

	/* Succeeded: the cookie's record, or none when it did not unprotect. */
	return 0;
}

/*
 * Writes a record: a cookie's protected for the current user behind the
 * magic, any other space's as it is.
 */
static int
protect_write(
	void *context,
	unsigned space,
	const char *key,
	const void *bytes,
	size_t length)
{
	struct protect_backend *backend;
	DATA_BLOB plain;
	DATA_BLOB sealed;
	unsigned char *record;
	BOOL done;
	int error;

	/* Only the cookies are protected. */
	backend = context;
	if (space != BROWSER_STORAGE_COOKIES) {
		error = backend->inner->write(backend->inner_context, space, key, bytes, length);
		if (error != 0)
			return error;

		/* Succeeded: the record, as it is. */
		return 0;
	}

	/* The blob of the current user, without a prompt. */
	if (length > (size_t)0xffffffffU)
		return EFBIG;
	plain.cbData = (DWORD)length;
	plain.pbData = (BYTE *)bytes;
	memset(&sealed, 0, sizeof(sealed));
	done = CryptProtectData(&plain, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &sealed);
	if (!done)
		return EIO;

	/* The record: the magic, then the blob. */
	record = malloc(PROTECT_MAGIC_LENGTH + (size_t)sealed.cbData);
	if (record == NULL) {
		(void)LocalFree(sealed.pbData);
		return ENOMEM;
	}
	memcpy(record, PROTECT_MAGIC, PROTECT_MAGIC_LENGTH);
	memcpy(record + PROTECT_MAGIC_LENGTH, sealed.pbData, sealed.cbData);
	error = backend->inner->write(backend->inner_context, space, key, record, PROTECT_MAGIC_LENGTH + (size_t)sealed.cbData);
	SecureZeroMemory(record, PROTECT_MAGIC_LENGTH + (size_t)sealed.cbData);
	free(record);
	(void)LocalFree(sealed.pbData);
	if (error != 0)
		return error;

	/* Succeeded: the cookie's record is protected on disk. */
	return 0;
}

/* Removes a record through the wrapped backend. */
static int
protect_remove(
	void *context,
	unsigned space,
	const char *key)
{
	struct protect_backend *backend;
	int error;

	/* The wrapped backend's removal. */
	backend = context;
	error = backend->inner->remove(backend->inner_context, space, key);
	if (error != 0)
		return error;

	/* Succeeded: the record is gone. */
	return 0;
}

/* Lists a space's keys through the wrapped backend (the keys are not protected). */
static int
protect_list(
	void *context,
	unsigned space,
	int (*each)(void *each_context, const char *key),
	void *each_context)
{
	struct protect_backend *backend;
	int status;

	/* The wrapped backend's list, which stops when each does. */
	backend = context;
	status = backend->inner->list(backend->inner_context, space, each, each_context);
	if (status != 0)
		return status;

	/* Succeeded: every key was given. */
	return 0;
}

/* Clears a space through the wrapped backend. */
static int
protect_clear(
	void *context,
	unsigned space)
{
	struct protect_backend *backend;
	int error;

	/* The wrapped backend's clearing. */
	backend = context;
	error = backend->inner->clear(backend->inner_context, space);
	if (error != 0)
		return error;

	/* Succeeded: the space is empty. */
	return 0;
}

/* Closes the wrapped backend and frees the context. */
static void
protect_close(
	void *context)
{
	struct protect_backend *backend;

	/* The wrapped backend's close, then this one's context. */
	backend = context;
	if (backend->inner->close != NULL)
		backend->inner->close(backend->inner_context);
	free(backend);
}

/*
 * Appends a cookie's record as the engine wrote it to the engine's sink:
 * unprotected after the magic, or as it is without the magic.  A blob
 * that does not unprotect sets *found to 0 and appends nothing.
 */
static int
protect_unseal(
	const unsigned char *bytes,
	size_t length,
	struct browser_storage_sink *sink,
	int *found)
{
	DATA_BLOB sealed;
	DATA_BLOB plain;
	BOOL done;
	int differs;
	int error;

	/* A record of before the wrapping, as it is. */
	differs = 1;
	if (length >= PROTECT_MAGIC_LENGTH)
		differs = memcmp(bytes, PROTECT_MAGIC, PROTECT_MAGIC_LENGTH);
	if (differs != 0) {
		error = browser_storage_sink_append(sink, bytes, length);
		if (error != 0)
			return error;

		/* Succeeded: the plain record. */
		return 0;
	}

	/* The blob after the magic, unprotected for the current user. */
	if (length - PROTECT_MAGIC_LENGTH > (size_t)0xffffffffU) {
		*found = 0;
		return 0;
	}
	sealed.cbData = (DWORD)(length - PROTECT_MAGIC_LENGTH);
	sealed.pbData = (BYTE *)(bytes + PROTECT_MAGIC_LENGTH);
	memset(&plain, 0, sizeof(plain));
	done = CryptUnprotectData(&sealed, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &plain);
	if (!done) {
		*found = 0;
		return 0;
	}

	/* The plain record to the engine; the copy here is wiped. */
	error = browser_storage_sink_append(sink, plain.pbData, plain.cbData);
	SecureZeroMemory(plain.pbData, plain.cbData);
	(void)LocalFree(plain.pbData);
	if (error != 0)
		return error;

	/* Succeeded: the cookie's record. */
	return 0;
}
