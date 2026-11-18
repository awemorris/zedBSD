/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Adapts the file manager's attribute subset to real FreeBSD namespace and list semantics. */
#include "xattr-freebsd.h"
#include <sys/extattr.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Selects one native syscall family without changing whether a symbolic link is followed. */
enum attribute_target {
	ATTRIBUTE_FILE,
	ATTRIBUTE_LINK,
	ATTRIBUTE_FD
};

/* One namespace snapshot owns its native encoded bytes until list translation finishes. */
struct attribute_names {
	unsigned char *bytes;
	size_t length;
	size_t converted;
};

static int attribute_namespace(const char *name, const char **leaf);
static ssize_t native_get(enum attribute_target target, const char *path, int descriptor, int space, const char *name, void *value, size_t capacity);
static ssize_t attribute_get(enum attribute_target target, const char *path, int descriptor, const char *name, void *value, size_t capacity);
static int attribute_set(enum attribute_target target, const char *path, int descriptor, const char *name, const void *value, size_t size, int flags);
static ssize_t native_list(enum attribute_target target, const char *path, int descriptor, int space, void *names, size_t capacity);
static int namespace_names(enum attribute_target target, const char *path, int descriptor, int space, size_t prefix, struct attribute_names *names);
static ssize_t attribute_list(enum attribute_target target, const char *path, int descriptor, char *names, size_t capacity);

/*
 * Reads a followed path's native attribute with nontruncating value semantics.
 */
ssize_t
getxattr(
	const char *path,
	const char *name,
	void *value,
	size_t capacity)
{
	ssize_t size;

	/* Uses the same namespace and bounds contract as descriptor and no-follow inquiries. */
	size = attribute_get(ATTRIBUTE_FILE, path, -1, name, value, capacity);
	if (size < 0)
		return -1;

	/* Succeeded: the caller receives the entire value or its required length. */
	return size;
}

/*
 * Reads an attribute of the link itself rather than its target.
 */
ssize_t
lgetxattr(
	const char *path,
	const char *name,
	void *value,
	size_t capacity)
{
	ssize_t size;

	/* Preserves the file information card's no-follow object identity. */
	size = attribute_get(ATTRIBUTE_LINK, path, -1, name, value, capacity);
	if (size < 0)
		return -1;

	/* Succeeded: only the named link object's entire value or size is returned. */
	return size;
}

/*
 * Reads the native attribute of an already opened copy source.
 */
ssize_t
fgetxattr(
	int descriptor,
	const char *name,
	void *value,
	size_t capacity)
{
	ssize_t size;

	/* Borrows the caller's descriptor without altering its ownership or following another path. */
	size = attribute_get(ATTRIBUTE_FD, NULL, descriptor, name, value, capacity);
	if (size < 0)
		return -1;

	/* Succeeded: the source file's entire value or required size is available to the copy. */
	return size;
}

/*
 * Stores a followed path's tags using native namespace ownership.
 */
int
setxattr(
	const char *path,
	const char *name,
	const void *value,
	size_t size,
	int flags)
{
	int error;

	/* Rejects unsupported atomic flags rather than approximating their contract with a race. */
	error = attribute_set(ATTRIBUTE_FILE, path, -1, name, value, size, flags);
	if (error != 0)
		return -1;

	/* Succeeded: the native filesystem stored the complete requested value. */
	return 0;
}

/*
 * Stores copied metadata on the caller's already opened destination file.
 */
int
fsetxattr(
	int descriptor,
	const char *name,
	const void *value,
	size_t size,
	int flags)
{
	int error;

	/* Borrows the destination descriptor and requires a complete native attribute write. */
	error = attribute_set(ATTRIBUTE_FD, NULL, descriptor, name, value, size, flags);
	if (error != 0)
		return -1;

	/* Succeeded: no descriptor owner or partial attribute success was fabricated. */
	return 0;
}

/*
 * Deletes a followed path's native attribute without affecting the file itself.
 */
int
removexattr(
	const char *path,
	const char *name)
{
	const char *leaf;
	int space;
	int error;

	/* Resolves the same namespace used by native tag reads and writes. */
	space = attribute_namespace(name, &leaf);
	if (space < 0)
		return -1;

	/* Returns native missing-attribute, permission and filesystem errors unchanged. */
	error = extattr_delete_file(path, space, leaf);
	if (error != 0)
		return -1;

	/* Succeeded: only the requested native attribute was removed. */
	return 0;
}

/*
 * Lists complete namespace-qualified names from an opened native file.
 */
ssize_t
flistxattr(
	int descriptor,
	char *names,
	size_t capacity)
{
	ssize_t size;

	/* The metadata copier requires complete NUL-separated entries and retains its source fd. */
	size = attribute_list(ATTRIBUTE_FD, NULL, descriptor, names, capacity);
	if (size < 0)
		return -1;

	/* Succeeded: every published name names the same opened native file. */
	return size;
}

/*
 * Lists attributes on a symbolic link without enumerating its target.
 */
ssize_t
llistxattr(
	const char *path,
	char *names,
	size_t capacity)
{
	ssize_t size;

	/* The information card's object identity is preserved through native no-follow enumeration. */
	size = attribute_list(ATTRIBUTE_LINK, path, -1, names, capacity);
	if (size < 0)
		return -1;

	/* Succeeded: the caller receives complete names or the required translated extent. */
	return size;
}

/* Maps explicit native namespaces without inventing Linux security or trusted namespace policy. */
static int
attribute_namespace(
	const char *name,
	const char **leaf)
{
	int same;
	int space;

	/* A missing attribute name supplies no native namespace. */
	if (name == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* User metadata keeps its familiar prefix in the shared application's representation. */
	same = strncmp(name, "user.", 5);
	if (same == 0) {
		space = EXTATTR_NAMESPACE_USER;
		*leaf = name + 5;
	} else {
		/* Native system metadata is distinct from Linux security and trusted attributes. */
		same = strncmp(name, "system.", 7);
		if (same != 0) {
			errno = ENOTSUP;
			return -1;
		}

		/* Preserves the native filesystem's privileged namespace and permission checks. */
		space = EXTATTR_NAMESPACE_SYSTEM;
		*leaf = name + 7;
	}

	/* An empty leaf cannot be confused with the deprecated native list operation. */
	if ((*leaf)[0] == '\0') {
		errno = EINVAL;
		return -1;
	}

	/* Succeeded: the caller has an explicit native namespace and borrowed attribute leaf. */
	return space;
}

/* Performs one native value inquiry through the requested object-identity family. */
static ssize_t
native_get(
	enum attribute_target target,
	const char *path,
	int descriptor,
	int space,
	const char *name,
	void *value,
	size_t capacity)
{
	ssize_t size;

	/* Chooses fd identity, no-follow path identity or the ordinary followed path. */
	if (target == ATTRIBUTE_FD) {
		size = extattr_get_fd(descriptor, space, name, value, capacity);
	} else if (target == ATTRIBUTE_LINK) {
		size = extattr_get_link(path, space, name, value, capacity);
	} else {
		size = extattr_get_file(path, space, name, value, capacity);
	}

	/* Native errors remain debuggable before ownership or output publication changes. */
	if (size < 0)
		return -1;

	/* Succeeded: the native kernel returned a byte extent or a size inquiry. */
	return size;
}

/* Detects native truncation with one extra byte before publishing any caller output. */
static ssize_t
attribute_get(
	enum attribute_target target,
	const char *path,
	int descriptor,
	const char *name,
	void *value,
	size_t capacity)
{
	const char *leaf;
	unsigned char *buffer;
	ssize_t size;
	int space;
	int saved;

	/* Attribute prefix and empty-name refusals precede any native filesystem operation. */
	space = attribute_namespace(name, &leaf);
	if (space < 0)
		return -1;

	/* A zero-sized request reports the native value size without reading or allocating it. */
	if (capacity == 0) {
		size = native_get(target, path, descriptor, space, leaf, NULL, 0);
		if (size < 0)
			return -1;

		/* Succeeded: the caller receives the required complete value size. */
		return size;
	}

	/* A nonempty output request needs storage and a representable extra-byte guard. */
	if (value == NULL || capacity >= (size_t)SSIZE_MAX) {
		errno = EINVAL;
		return -1;
	}

	/* The kernel may truncate silently; private storage prevents partial caller-visible output. */
	buffer = malloc(capacity + 1);
	if (buffer == NULL)
		return -1;

	/* One native read distinguishes a fitting value from a truncated value even if it changed size. */
	size = native_get(target, path, descriptor, space, leaf, buffer, capacity + 1);
	if (size < 0) {
		saved = errno;
		free(buffer);
		errno = saved;
		return -1;
	}

	/* The extra byte proves the caller's capacity could not hold the complete native value. */
	if ((size_t)size > capacity) {
		free(buffer);
		errno = ERANGE;
		return -1;
	}

	/* Publishes only a complete snapshot and retires its private read storage. */
	memcpy(value, buffer, (size_t)size);
	free(buffer);

	/* Succeeded: the caller receives the complete value with native descriptor ownership unchanged. */
	return size;
}

/* Stores only the application's supported unconditional native attribute update. */
static int
attribute_set(
	enum attribute_target target,
	const char *path,
	int descriptor,
	const char *name,
	const void *value,
	size_t size,
	int flags)
{
	const char *leaf;
	ssize_t written;
	int space;

	/* Native extattr has no atomic create/replace flag, so an approximation is refused. */
	if (flags != 0) {
		errno = ENOTSUP;
		return -1;
	}

	/* A size must fit the native syscall's signed result before a complete write can be verified. */
	if (size > (size_t)SSIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}

	/* Resolves native namespace ownership before updating any filesystem metadata. */
	space = attribute_namespace(name, &leaf);
	if (space < 0)
		return -1;

	/* Existing write callers use only followed paths or already opened destination descriptors. */
	if (target == ATTRIBUTE_FD) {
		written = extattr_set_fd(descriptor, space, leaf, value, size);
	} else {
		written = extattr_set_file(path, space, leaf, value, size);
	}

	/* Native permissions, unsupported filesystems and missing files remain actual errors. */
	if (written < 0)
		return -1;

	/* A partial native write is not successful metadata preservation. */
	if ((size_t)written != size) {
		errno = EIO;
		return -1;
	}

	/* Succeeded: the real filesystem stored every requested byte. */
	return 0;
}

/* Performs one real native enumeration for the requested object and namespace. */
static ssize_t
native_list(
	enum attribute_target target,
	const char *path,
	int descriptor,
	int space,
	void *names,
	size_t capacity)
{
	ssize_t size;

	/* The information card uses no-follow paths; copies use their already opened source files. */
	if (target == ATTRIBUTE_FD) {
		size = extattr_list_fd(descriptor, space, names, capacity);
	} else {
		size = extattr_list_link(path, space, names, capacity);
	}

	/* Every native enumeration failure retains its own errno. */
	if (size < 0)
		return -1;

	/* Succeeded: the native kernel supplies encoded bytes or their required extent. */
	return size;
}

/* Captures and validates a complete native list before measuring its translated names. */
static int
namespace_names(
	enum attribute_target target,
	const char *path,
	int descriptor,
	int space,
	size_t prefix,
	struct attribute_names *names)
{
	ssize_t required;
	ssize_t received;
	size_t capacity;
	size_t at;
	size_t length;
	void *nul;

	/* The first native inquiry selects a bounded private allocation, not a caller-visible snapshot. */
	required = native_list(target, path, descriptor, space, NULL, 0);
	if (required < 0)
		return errno;

	/* An extra byte must fit the native signed result to detect concurrent growth. */
	if ((size_t)required >= (size_t)SSIZE_MAX)
		return EOVERFLOW;

	/* A one-byte guard avoids silently accepting a list truncated at an entry boundary. */
	capacity = (size_t)required + 1;
	names->bytes = malloc(capacity);
	if (names->bytes == NULL)
		return errno;

	/* Reads the actual snapshot without trusting that the preceding size inquiry stayed current. */
	received = native_list(target, path, descriptor, space, names->bytes, capacity);
	if (received < 0)
		return errno;

	/* Filling the guard byte makes completeness uncertain, so callers must retry explicitly. */
	if ((size_t)received >= capacity)
		return EAGAIN;

	/* Measures only complete length-prefixed records and refuses malformed or embedded-NUL names. */
	names->length = (size_t)received;
	names->converted = 0;
	at = 0;
	while (at < names->length) {
		length = names->bytes[at];
		at++;

		/* A zero-sized or torn record cannot publish a complete translated attribute name. */
		if (length == 0 || length > names->length - at)
			return EIO;

		/* Kernel names must remain valid C strings after the explicit trailing NUL is added. */
		nul = memchr(names->bytes + at, '\0', length);
		if (nul != NULL)
			return EIO;

		/* Namespace prefix and terminator must fit the signed public list result. */
		if (names->converted > (size_t)SSIZE_MAX - prefix - length - 1)
			return EOVERFLOW;

		/* The translated extent keeps each namespace-qualified name and terminator together. */
		names->converted += prefix + length + 1;
		at += length;
	}

	/* Succeeded: this owned native snapshot contains only complete translatable names. */
	return 0;
}

/* Publishes both accessible native namespaces as a complete NUL-separated list. */
static ssize_t
attribute_list(
	enum attribute_target target,
	const char *path,
	int descriptor,
	char *names,
	size_t capacity)
{
	struct attribute_names snapshots[2];
	const char *prefix;
	size_t prefix_size;
	size_t required;
	size_t index;
	size_t at;
	size_t used;
	size_t length;
	int error;

	/* Each namespace snapshot owns its allocation until the common cleanup path. */
	memset(snapshots, 0, sizeof(snapshots));
	error = namespace_names(target, path, descriptor, EXTATTR_NAMESPACE_USER, 5, &snapshots[0]);
	if (error != 0)
		goto cleanup;

	/* The system namespace is optional only when this actual user's credentials deny enumeration. */
	error = namespace_names(target, path, descriptor, EXTATTR_NAMESPACE_SYSTEM, 7, &snapshots[1]);
	if (error == EPERM || error == EACCES) {
		error = 0;
		snapshots[1].length = 0;
		snapshots[1].converted = 0;
	}

	/* Unsupported filesystems or damaged metadata cannot be described as an empty successful list. */
	if (error != 0)
		goto cleanup;

	/* The combined list must fit the public signed return type before publishing any name. */
	if (snapshots[1].converted > (size_t)SSIZE_MAX - snapshots[0].converted) {
		error = EOVERFLOW;
		goto cleanup;
	}

	/* Zero capacity asks for the translated length without requiring output storage. */
	required = snapshots[0].converted + snapshots[1].converted;
	if (capacity == 0)
		goto cleanup;

	/* A nonempty publication needs a valid destination large enough for the entire snapshot. */
	if (names == NULL) {
		error = EINVAL;
		goto cleanup;
	}

	/* An undersized list must leave the caller's previous bytes unchanged. */
	if (capacity < required) {
		error = ERANGE;
		goto cleanup;
	}

	/* Appends each validated native snapshot with its namespace and NUL terminator. */
	used = 0;
	for (index = 0; index < 2; index++) {
		prefix = "user.";
		prefix_size = 5;

		/* Native privileged metadata retains its distinct system prefix. */
		if (index == 1) {
			prefix = "system.";
			prefix_size = 7;
		}

		/* Copies only records whose complete extents were validated before publication. */
		at = 0;
		while (at < snapshots[index].length) {
			length = snapshots[index].bytes[at];
			at++;

			/* Each record becomes one namespace-qualified terminated C string. */
			memcpy(names + used, prefix, prefix_size);
			used += prefix_size;
			memcpy(names + used, snapshots[index].bytes + at, length);
			used += length;
			names[used] = '\0';
			used++;
			at += length;
		}
	}

cleanup:
	/* Both native snapshots retire before either an error or a complete extent reaches the caller. */
	free(snapshots[0].bytes);
	free(snapshots[1].bytes);
	if (error != 0) {
		errno = error;
		return -1;
	}

	/* Succeeded: the caller receives only a complete list or its required translated size. */
	return (ssize_t)required;
}
