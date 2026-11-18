/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Descriptor references shared by files and typed non-file kernel objects.
 */

#include <kern/fd-object.h>
#include <kern/file.h>
#include <kern/handle.h>
#include <uapi/errno.h>
#include <stddef.h>

/*
 * Recognizes a live reference suitable for descriptor publication.
 */
int
fd_object_valid(
	const struct fd_object *object)
{
	/* A missing carrier owns no reference. */
	if (object == NULL)
		return 0;

	/* The tag determines which pointer represents the retained object. */
	switch (object->type) {
	case FD_OBJECT_FILE:
		/* A file reference must identify an open description. */
		if (object->data.file == NULL)
			return 0;
		break;
	case FD_OBJECT_HANDLE:
		/* A handle reference must identify its independent wrapper. */
		if (object->data.handle == NULL)
			return 0;
		break;
	default:
		/* Empty and unknown carriers cannot enter a live slot. */
		return 0;
	}

	/* Succeeded: the carrier names one supported object class. */
	return 1;
}

/*
 * Retains an object whose caller already owns or locks a live reference.
 */
void
fd_object_get(
	const struct fd_object *object)
{
	/* An absent carrier needs no lifetime extension. */
	if (object == NULL)
		return;

	/* Reference acquisition never invokes a backend or sleeps. */
	switch (object->type) {
	case FD_OBJECT_FILE:
		/* The open description keeps its existing file-backend lifetime. */
		file_ref(object->data.file);
		break;
	case FD_OBJECT_HANDLE:
		/* The standalone handle contributes no file-cache or inode ownership. */
		handle_get(object->data.handle);
		break;
	default:
		break;
	}

	/* Succeeded: the supported object has an additional retained reference. */
	return;
}

/*
 * Releases an owned carrier outside descriptor and socket spin locks.
 */
int
fd_object_put(
	struct fd_object *object)
{
	struct fd_object detached;
	int error;

	/* An absent carrier requires no cleanup. */
	if (object == NULL)
		return 0;

	/* Destruction may reenter subsystems, so first withdraw this ownership. */
	detached = *object;
	fd_object_clear(object);
	error = 0;

	/* Each object class controls its own final destruction. */
	switch (detached.type) {
	case FD_OBJECT_FILE:
		/* Final file close retains its established error-reporting contract. */
		error = file_close(detached.data.file);
		break;
	case FD_OBJECT_HANDLE:
		/* Handle destruction may call its subsystem only outside descriptor locks. */
		handle_put(detached.data.handle);
		break;
	case FD_OBJECT_NONE:
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Reports a file backend's final-close error to close callers. */
	if (error != 0)
		return error;

	/* Succeeded: this carrier no longer owns any object. */
	return 0;
}

/*
 * Marks a carrier empty after its ownership was moved elsewhere.
 */
void
fd_object_clear(
	struct fd_object *object)
{
	/* An absent carrier has no representation to clear. */
	if (object == NULL)
		return;

	/* NONE prevents either pointer interpretation from acquiring authority. */
	object->type = FD_OBJECT_NONE;
	object->data.file = NULL;

	/* Succeeded: the carrier holds no ownership or pointer authority. */
	return;
}
