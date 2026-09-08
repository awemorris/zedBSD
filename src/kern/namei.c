/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pathname lookup and the working directory.
 *
 * A lookup walks the components of a path from the root or the working
 * directory of a cwdinfo, crossing mount points, following symbolic links
 * up to the loop limit, and checking search permission at every
 * directory.  getcwd() rebuilds a path by walking up to the root and
 * naming each directory in its parent.
 */

#include "kern/namei.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/mount.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Some VFS host tests deliberately link without the process subsystem. */
extern const struct ucred *cred_current(void) __attribute__((weak));
extern struct ucred *cred_current_ref(void) __attribute__((weak));
extern void cred_release(struct ucred *) __attribute__((weak));

static void release_cred(struct ucred *cred);
static int path_length(const char *path, size_t *length);
static int component_is(const char *name, size_t length, const char *literal);
static int search_access(const struct inode *directory, const struct ucred *cred);
static int child_path(const struct path *parent, const char *name, struct path *result);
static int find_child_name(const struct path *parent, const struct path *child, char name[NAME_MAX + 1U]);
static int getcwd_once(const struct path *root, const struct path *cwd, char *buffer, size_t capacity);

/*
 * Resolves a path to a referenced path record.
 *
 * With NAMEI_NOFOLLOW_FINAL a symbolic link as the last component is
 * returned itself unless the path ends in a slash.  A trailing slash
 * also requires the result to be a searchable directory.
 */
int
namei_path_flags_at(
	struct cwdinfo *context,
	const char *path,
	unsigned flags,
	struct path *result)
{
	struct path current;
	struct path root;
	struct path next_path;
	struct inode *next_inode;
	struct componentname component;
	struct ucred *owned_cred;
	const struct ucred *cred;
	char work[ZEDBSD_PATH_MAX];
	char target[ZEDBSD_PATH_MAX];
	char combined[ZEDBSD_PATH_MAX];
	size_t length;
	size_t position;
	size_t start;
	size_t remainder;
	ssize_t target_length;
	unsigned symlinks;
	unsigned long irq;
	int trailing;
	int error;

	position = 0;
	symlinks = 0;

	/* Rejects a missing context or result, or an unknown flag. */
	if (context == NULL ||
	    result == NULL ||
	    (flags & ~NAMEI_NOFOLLOW_FINAL) != 0)
		return EINVAL;
	error = path_length(path, &length);
	if (error != 0)
		return error;

	/* Takes the caller's credential when the process subsystem exists. */
	owned_cred = NULL;
	if (cred_current_ref != NULL)
		owned_cred = cred_current_ref();
	cred = owned_cred;
	if (cred == NULL && cred_current != NULL)
		cred = cred_current();

	/* Works on a private copy that symbolic links can rewrite. */
	memcpy(work, path, length + 1U);
	path = work;
	trailing = path[length - 1U] == '/';

	/* Starts at the root for an absolute path, else the working directory. */
	irq = spin_lock_irqsave(&context->lock);

	if (context->root.p_inode == NULL || context->cwd.p_inode == NULL) {
		spin_unlock_irqrestore(&context->lock, irq);
		release_cred(owned_cred);
		return EINVAL;
	}

	path_set(&root, context->root.p_mount, context->root.p_inode);
	if (path[0] == '/')
		path_set(&current, root.p_mount, root.p_inode);
	else
		path_set(&current, context->cwd.p_mount, context->cwd.p_inode);

	spin_unlock_irqrestore(&context->lock, irq);

	error = search_access(current.p_inode, cred);
	if (error != 0)
		goto fail;

	/* Walks the components. */
	while (position < length) {
		/* Skips the slashes before the next component. */
		while (position < length && path[position] == '/')
			position++;
		if (position == length)
			break;
		error = search_access(current.p_inode, cred);
		if (error != 0)
			goto fail;

		/* Delimits the component. */
		start = position;
		while (position < length && path[position] != '/')
			position++;
		component.cn_nameptr = path + start;
		component.cn_namelen = position - start;
		component.cn_flags = 0;
		if (component.cn_namelen > NAME_MAX) {
			error = ENAMETOOLONG;
			goto fail;
		}

		if (position == length)
			component.cn_flags |= COMPONENT_LAST;

		/* A dot stays put. */
		if (component_is(component.cn_nameptr, component.cn_namelen, ".")) {
			component.cn_flags |= COMPONENT_DOT;
			continue;
		}

		/* A dot-dot goes up, crossing a mount root, but not past the root. */
		if (component_is(component.cn_nameptr, component.cn_namelen, "..")) {
			component.cn_flags |= COMPONENT_DOTDOT;
			if (path_equal(&current, &root))
				continue;
			error = mount_cross_path_parent(&current, &next_path);
			if (error == 0) {
				path_release(&current);
				current = next_path;
				continue;
			}

			error = inode_lookup(current.p_inode, &component, &next_inode);
			if (error != 0)
				goto fail;
			path_set(&next_path, current.p_mount, next_inode);
			inode_release(next_inode);
			path_release(&current);
			current = next_path;
			continue;
		}

		/* A mount point on the component enters the mounted filesystem. */
		error = mount_lookup_child(&current, &component, &next_path);
		if (error == 0) {
			path_release(&current);
			current = next_path;
			continue;
		}

		if (error != ENOENT)
			goto fail;

		/* Looks the component up in the directory. */
		error = inode_lookup(current.p_inode, &component, &next_inode);
		if (error != 0)
			goto fail;

		/* Splices a symbolic link's target in front of the rest of the path. */
		if (next_inode->i_type == INODE_SYMLINK &&
		    !((flags & NAMEI_NOFOLLOW_FINAL) != 0 &&
		      position == length &&
		      !trailing)) {
			remainder = length - position;
			symlinks++;
			if (symlinks > ZEDBSD_SYMLOOP_MAX) {
				inode_release(next_inode);
				error = ELOOP;
				goto fail;
			}

			target_length = inode_readlink(next_inode, target,
				sizeof(target) - 1U);
			inode_release(next_inode);
			if (target_length < 0) {
				error = (int)-target_length;
				goto fail;
			}

			if (target_length == 0) {
				error = ENOENT;
				goto fail;
			}

			if ((size_t)target_length + remainder >= sizeof(combined)) {
				error = ENAMETOOLONG;
				goto fail;
			}

			memcpy(combined, target, (size_t)target_length);
			memcpy(combined + target_length, path + position,
				remainder + 1U);
			memcpy(work, combined,
				(size_t)target_length + remainder + 1U);
			length = (size_t)target_length + remainder;
			path = work;
			position = 0;
			trailing = path[length - 1U] == '/';

			/* An absolute target restarts from the root. */
			if (path[0] == '/') {
				path_release(&current);
				path_set(&current, root.p_mount, root.p_inode);
			}

			continue;
		}

		/* Descends into the component. */
		path_set(&next_path, current.p_mount, next_inode);
		inode_release(next_inode);
		path_release(&current);

		/* Anything followed by more path must be a searchable directory. */
		current = next_path;
		if (position < length || trailing) {
			if (current.p_inode->i_type != INODE_DIR)
				error = ENOTDIR;
			else
				error = search_access(current.p_inode, cred);
			if (error != 0)
				goto fail;
		}
	}

	/* A trailing slash requires a searchable directory. */
	if (trailing && current.p_inode->i_type != INODE_DIR) {
		error = ENOTDIR;
		goto fail;
	}

	if (trailing) {
		error = search_access(current.p_inode, cred);
		if (error != 0)
			goto fail;
	}

	/* Hands the resolved path to the caller. */
	*result = current;
	path_release(&root);
	release_cred(owned_cred);

	/* Reports the resolved path. */
	return 0;

fail:
	path_release(&current);
	path_release(&root);
	release_cred(owned_cred);

	/* Reports the lookup failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Resolves a path, following a final symbolic link.
 */
int
namei_path_at(
	struct cwdinfo *context,
	const char *path,
	struct path *result)
{
	int error;

	/* Reports why the lookup failed. */
	error = namei_path_flags_at(context, path, 0, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Resolves a path to a referenced inode.
 */
int
namei_at(
	struct cwdinfo *context,
	const char *path,
	struct inode **result)
{
	struct path found;
	int error;

	/* Rejects a missing result. */
	if (result == NULL)
		return EINVAL;

	/* Resolves the path and keeps only the inode reference. */
	error = namei_path_at(context, path, &found);
	if (error != 0)
		return error;
	*result = found.p_inode;
	found.p_inode = NULL;
	path_release(&found);

	/* Reports the resolved inode. */
	return 0;
}

/*
 * Resolves the parent directory of a path and splits off the last name.
 *
 * The last component may not be dot or dot-dot, and trailing slashes are
 * ignored.  The name is copied into storage for the caller's component.
 */
int
namei_parent_path_at(
	struct cwdinfo *context,
	const char *path,
	struct path *parent,
	struct componentname *last,
	char storage[NAME_MAX + 1U])
{
	char prefix[ZEDBSD_PATH_MAX];
	size_t length;
	size_t start;
	size_t parent_length;
	int error;

	/* Rejects a missing operand. */
	if (context == NULL || parent == NULL || last == NULL || storage == NULL)
		return EINVAL;
	error = path_length(path, &length);
	if (error != 0)
		return error;

	/* Finds the last component, ignoring trailing slashes. */
	while (length > 1 && path[length - 1U] == '/')
		length--;
	start = length;
	while (start > 0 && path[start - 1U] != '/')
		start--;
	if (length - start > NAME_MAX)
		return ENAMETOOLONG;
	if (length - start == 0)
		return EINVAL;

	/* Copies the name, refusing dot and dot-dot. */
	memcpy(storage, path + start, length - start);
	storage[length - start] = '\0';
	if (!strcmp(storage, ".") || !strcmp(storage, ".."))
		return EINVAL;
	last->cn_nameptr = storage;
	last->cn_namelen = length - start;
	last->cn_flags = COMPONENT_LAST;

	/* Builds the parent path, which is the current directory for a bare name. */
	if (start == 0) {
		prefix[0] = '.';
		prefix[1] = '\0';
	} else {
		parent_length = start;
		while (parent_length > 1 && path[parent_length - 1U] == '/')
			parent_length--;
		memcpy(prefix, path, parent_length);
		prefix[parent_length] = '\0';
	}

	/* Resolves the parent, which must be a directory. */
	error = namei_path_at(context, prefix, parent);
	if (error == 0 && parent->p_inode->i_type != INODE_DIR) {
		path_release(parent);
		return ENOTDIR;
	}

	/* Reports why the parent lookup failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Resolves the parent directory of a path to a referenced inode.
 */
int
namei_parent_at(
	struct cwdinfo *context,
	const char *path,
	struct inode **parent,
	struct componentname *last,
	char storage[NAME_MAX + 1U])
{
	struct path found;
	int error;

	/* Rejects a missing result. */
	if (parent == NULL)
		return EINVAL;

	/* Resolves the parent and keeps only the inode reference. */
	error = namei_parent_path_at(context, path, &found, last, storage);
	if (error != 0)
		return error;
	*parent = found.p_inode;
	found.p_inode = NULL;
	path_release(&found);

	/* Reports the resolved parent. */
	return 0;
}

/*
 * Initializes a cwdinfo with a root that is also the working directory.
 */
int
cwdinfo_init(
	struct cwdinfo *context,
	const struct path *root)
{
	/* Rejects a missing context or a root that is not a directory. */
	if (context == NULL ||
	    root == NULL ||
	    root->p_mount == NULL ||
	    root->p_inode == NULL ||
	    root->p_inode->i_type != INODE_DIR)
		return EINVAL;

	/* Starts with one reference and both paths at the root. */
	memset(context, 0, sizeof(*context));
	refcount_init(&context->refs, 1);
	spin_init(&context->lock, LOCK_RANK_PROCESS, "cwdinfo");
	path_set(&context->root, root->p_mount, root->p_inode);
	path_set(&context->cwd, root->p_mount, root->p_inode);

	/* Reports the initialized context. */
	return 0;
}

/*
 * Releases the paths of a cwdinfo.
 */
void
cwdinfo_destroy(
	struct cwdinfo *context)
{
	/* Ignores a missing context. */
	if (context == NULL)
		return;

	/* Drops both paths and clears the record. */
	path_release(&context->root);
	path_release(&context->cwd);
	memset(context, 0, sizeof(*context));
}

/*
 * Changes the working directory to a resolved path.
 */
int
fs_chdir_path(
	struct cwdinfo *context,
	const struct path *directory)
{
	struct ucred *cred;
	const struct ucred *check;
	struct path replacement;
	struct path old;
	unsigned long irq;
	int error;

	/* Takes the caller's credential when the process subsystem exists. */
	cred = NULL;
	if (cred_current_ref != NULL)
		cred = cred_current_ref();
	check = cred;
	if (check == NULL && cred_current != NULL)
		check = cred_current();

	/* Rejects a missing context or an incomplete or non-directory path. */
	if (context == NULL ||
	    directory == NULL ||
	    directory->p_inode == NULL ||
	    directory->p_mount == NULL) {
		release_cred(cred);
		return EINVAL;
	}

	if (directory->p_inode->i_type != INODE_DIR) {
		release_cred(cred);
		return ENOTDIR;
	}

	/* The new directory must be searchable. */
	error = search_access(directory->p_inode, check);
	if (error != 0) {
		release_cred(cred);
		return error;
	}

	/* Swaps the working directory under the lock. */
	path_init(&replacement);
	path_set(&replacement, directory->p_mount, directory->p_inode);
	irq = spin_lock_irqsave(&context->lock);

	old = context->cwd;
	context->cwd = replacement;

	spin_unlock_irqrestore(&context->lock, irq);

	path_release(&old);
	release_cred(cred);

	/* Reports the changed directory. */
	return 0;
}

/*
 * Changes the working directory to a path.
 */
int
fs_chdir(
	struct cwdinfo *context,
	const char *path)
{
	struct path directory;
	int error;

	/* Resolves the path, then changes to it. */
	error = namei_path_at(context, path, &directory);
	if (error != 0)
		return error;
	error = fs_chdir_path(context, &directory);
	path_release(&directory);

	/* Reports why the change failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes the absolute path of the working directory into a buffer.
 *
 * The walk is retried a few times when a directory changes under it.
 */
int
fs_getcwd(
	const struct cwdinfo *context,
	char *buffer,
	size_t capacity)
{
	struct path root;
	struct path cwd;
	unsigned attempt;
	unsigned long irq;
	int error;

	/* Rejects a missing context or an empty buffer. */
	if (context == NULL || buffer == NULL || capacity == 0)
		return EINVAL;

	/* Takes references on the root and the working directory. */
	irq = spin_lock_irqsave((struct spinlock *)&context->lock);

	if (context->root.p_inode == NULL || context->cwd.p_inode == NULL) {
		spin_unlock_irqrestore((struct spinlock *)&context->lock, irq);
		return EINVAL;
	}

	path_set(&root, context->root.p_mount, context->root.p_inode);
	path_set(&cwd, context->cwd.p_mount, context->cwd.p_inode);

	spin_unlock_irqrestore((struct spinlock *)&context->lock, irq);

	/* Walks up, retrying when a directory changed during the walk. */
	for (attempt = 0; attempt < 8U; attempt++) {
		error = getcwd_once(&root, &cwd, buffer, capacity);
		if (error != EAGAIN)
			break;
	}

	path_release(&cwd);
	path_release(&root);

	/* Reports why the walk failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Releases a credential when the process subsystem exists. */
static void
release_cred(
	struct ucred *cred)
{
	/* Ignores a missing credential or a build without credentials. */
	if (cred != NULL && cred_release != NULL)
		cred_release(cred);
}

/* Measures a path, refusing an empty or overlong one. */
static int
path_length(
	const char *path,
	size_t *length)
{
	size_t n;

	/* Rejects a missing path. */
	if (path == NULL)
		return EINVAL;

	/* Counts the bytes up to the limit. */
	n = 0;
	while (n < ZEDBSD_PATH_MAX && path[n] != '\0')
		n++;
	if (n == 0)
		return ENOENT;
	if (n == ZEDBSD_PATH_MAX)
		return ENAMETOOLONG;

	*length = n;

	/* Reports the measured length. */
	return 0;
}

/* Tests whether an unterminated component equals a literal. */
static int
component_is(
	const char *name,
	size_t length,
	const char *literal)
{
	size_t i;

	/* Compares up to the shorter of the two. */
	for (i = 0; i < length && literal[i] != '\0'; i++) {
		if (name[i] != literal[i])
			return 0;
	}

	/* Both must end at the same place. */
	if (i != length)
		return 0;
	if (literal[i] != '\0')
		return 0;

	/* Reports a match. */
	return 1;
}

/* Checks search permission on a directory when there is a credential. */
static int
search_access(
	const struct inode *directory,
	const struct ucred *cred)
{
	int error;

	/* Without a credential nothing is checked. */
	if (cred == NULL)
		return 0;

	/* Checks execute permission. */

	/* Reports the access check. */
	error = vfs_access(directory, cred, X_OK);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Resolves one named child of a directory, crossing a mount point. */
static int
child_path(
	const struct path *parent,
	const char *name,
	struct path *result)
{
	struct componentname component;
	struct inode *inode;
	int error;

	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = COMPONENT_LAST;

	/* A mount point on the name enters the mounted filesystem. */
	error = mount_lookup_child(parent, &component, result);
	if (error == 0)
		return 0;
	if (error != ENOENT)
		return error;

	/* Looks the name up in the directory. */
	error = inode_lookup(parent->p_inode, &component, &inode);
	if (error != 0)
		return error;
	path_set(result, parent->p_mount, inode);
	inode_release(inode);

	/* Reports the resolved child. */
	return 0;
}

/* Finds the name of a child in its parent by scanning the directory. */
static int
find_child_name(
	const struct path *parent,
	const struct path *child,
	char name[NAME_MAX + 1U])
{
	struct file *directory;
	struct dirent entry;
	struct path candidate;
	uint64_t sequence;
	int eof;
	int error;

	eof = 0;

	/* Opens the directory, remembering its change sequence. */
	sequence = parent->p_inode->i_dirseq;
	error = file_open_resolved(parent, O_RDONLY | O_DIRECTORY, &directory);
	if (error != 0)
		return error;

	/* Compares every entry with the child. */
	while (!eof) {
		error = file_readdir(directory, &entry, &eof);
		if (error != 0)
			break;
		if (eof)
			break;
		if (entry.d_name[0] == '\0' ||
		    !strcmp(entry.d_name, ".") ||
		    !strcmp(entry.d_name, ".."))
			continue;
		error = child_path(parent, entry.d_name, &candidate);
		if (error == ENOENT)
			continue;
		if (error != 0)
			break;
		if (path_equal(&candidate, child)) {
			strcpy(name, entry.d_name);
			path_release(&candidate);
			error = 0;
			goto out;
		}

		path_release(&candidate);
	}

	if (error == 0)
		error = ENOENT;
out:
	(void)file_close(directory);

	/* A directory that changed during the scan must be scanned again. */
	if (parent->p_inode->i_dirseq != sequence)
		return EAGAIN;

	/* Reports why the scan failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Builds the working directory path once by walking up to the root. */
static int
getcwd_once(
	const struct path *root,
	const struct path *cwd,
	char *buffer,
	size_t capacity)
{
	struct componentname dotdot = { "..", 2, COMPONENT_DOTDOT };
	struct path current;
	struct path parent;
	struct inode *parent_inode;
	char reverse[ZEDBSD_PATH_MAX];
	char name[NAME_MAX + 1U];
	size_t position;
	size_t length;
	unsigned depth;
	int error;

	position = sizeof(reverse) - 1U;
	depth = 0;
	error = 0;

	/* Builds the path backwards from the end of the buffer. */
	reverse[position] = '\0';
	path_set(&current, cwd->p_mount, cwd->p_inode);
	while (!path_equal(&current, root)) {
		/* Bounds the walk against a cyclic directory graph. */
		depth++;
		if (depth > ZEDBSD_PATH_MAX / 2U) {
			error = ELOOP;
			break;
		}

		/* A mount root is named by its mount, other directories by scanning. */
		error = mount_cross_path_parent(&current, &parent);
		if (error == 0) {
			length = strlen(current.p_mount->m_name);
			if (length == 0 || length > NAME_MAX) {
				path_release(&parent);
				error = ENOENT;
				break;
			}

			memcpy(name, current.p_mount->m_name, length + 1U);
		} else if (error == ENOENT) {
			error = inode_lookup(current.p_inode, &dotdot,
			    &parent_inode);
			if (error != 0)
				break;
			path_set(&parent, current.p_mount, parent_inode);
			inode_release(parent_inode);
			if (path_equal(&parent, &current)) {
				path_release(&parent);
				error = ENOENT;
				break;
			}

			error = find_child_name(&parent, &current, name);
			if (error != 0) {
				path_release(&parent);
				break;
			}

			length = strlen(name);
		} else {
			break;
		}

		/* Prepends a slash and the name. */
		if (length + 1U > position) {
			path_release(&parent);
			error = ERANGE;
			break;
		}

		position -= length;
		memcpy(reverse + position, name, length);
		reverse[--position] = '/';
		path_release(&current);
		current = parent;
	}

	path_release(&current);
	if (error != 0)
		return error;

	/* The root itself is a lone slash. */
	if (position == sizeof(reverse) - 1U)
		reverse[--position] = '/';

	/* Copies the path when it fits. */
	if (sizeof(reverse) - position > capacity)
		return ERANGE;
	memcpy(buffer, reverse + position, sizeof(reverse) - position);

	/* Reports the built path. */
	return 0;
}
