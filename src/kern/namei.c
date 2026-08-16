/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static void
release_cred(struct ucred *cred)
{
	if (cred != NULL && cred_release != NULL)
		cred_release(cred);
}

static int
path_length(const char *path, size_t *length)
{
	size_t n;
	if (path == NULL)
		return EINVAL;
	for (n = 0; n < ZEDBSD_PATH_MAX && path[n] != '\0'; n++)
		;
	if (n == 0)
		return ENOENT;
	if (n == ZEDBSD_PATH_MAX)
		return ENAMETOOLONG;
	*length = n;
	return 0;
}

static int
component_is(const char *name, size_t length, const char *literal)
{
	size_t i;
	for (i = 0; i < length && literal[i] != '\0'; i++)
		if (name[i] != literal[i])
			return 0;
	return i == length && literal[i] == '\0';
}

static int
search_access(const struct inode *directory,const struct ucred *cred)
{
	if (cred == NULL)
		return 0;
	return vfs_access(directory, cred, X_OK);
}

int
namei_path_flags_at(struct cwdinfo *context, const char *path, unsigned flags,
		    struct path *result)
{
	struct path current, root;
	char work[ZEDBSD_PATH_MAX];
	size_t length, position = 0;
	unsigned symlinks = 0;
	int error, trailing;
	struct ucred *owned_cred;
	const struct ucred *cred;
	unsigned long irq;
	if (context == NULL || result == NULL ||
	    (flags & ~NAMEI_NOFOLLOW_FINAL) != 0)
		return EINVAL;
	error = path_length(path, &length);
	if (error != 0)
		return error;
	owned_cred=cred_current_ref!=NULL?cred_current_ref():NULL;
	cred=owned_cred!=NULL?owned_cred:
	    (cred_current!=NULL?cred_current():NULL);
	memcpy(work, path, length + 1U);
	path = work;
	trailing = path[length - 1U] == '/';
	irq=spin_lock_irqsave(&context->lock);
	if(context->root.p_inode==NULL||context->cwd.p_inode==NULL){
		spin_unlock_irqrestore(&context->lock,irq);
		release_cred(owned_cred);
		return EINVAL;
	}
	path_set(&root,context->root.p_mount,context->root.p_inode);
	if(path[0]=='/')path_set(&current,root.p_mount,root.p_inode);
	else path_set(&current,context->cwd.p_mount,context->cwd.p_inode);
	spin_unlock_irqrestore(&context->lock,irq);
	error = search_access(current.p_inode,cred);
	if (error != 0)
		goto fail;
	while (position < length) {
		struct componentname component;
		struct path next_path;
		struct inode *next_inode;
		size_t start;
		while (position < length && path[position] == '/')
			position++;
		if (position == length)
			break;
		error = search_access(current.p_inode,cred);
		if (error != 0)
			goto fail;
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
		if (component_is(component.cn_nameptr, component.cn_namelen, ".")) {
			component.cn_flags |= COMPONENT_DOT;
			continue;
		}
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
		error = mount_lookup_child(&current, &component, &next_path);
		if (error == 0) {
			path_release(&current);
			current = next_path;
			continue;
		}
		if (error != ENOENT)
			goto fail;
		error = inode_lookup(current.p_inode, &component, &next_inode);
		if (error != 0)
			goto fail;
		if (next_inode->i_type == INODE_SYMLINK &&
		    !((flags & NAMEI_NOFOLLOW_FINAL) != 0 && position == length &&
		      !trailing)) {
			char target[ZEDBSD_PATH_MAX], combined[ZEDBSD_PATH_MAX];
			ssize_t target_length;
			size_t remainder = length - position;
			if (++symlinks > ZEDBSD_SYMLOOP_MAX) {
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
			if (target_length == 0 ||
			    (size_t)target_length + remainder >= sizeof(combined)) {
				error = target_length == 0 ? ENOENT : ENAMETOOLONG;
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
			if (path[0] == '/') {
				path_release(&current);
				path_set(&current, root.p_mount, root.p_inode);
			}
			continue;
		}
		path_set(&next_path, current.p_mount, next_inode);
		inode_release(next_inode);
		path_release(&current);
		current = next_path;
		if (position < length || trailing) {
			error = current.p_inode->i_type != INODE_DIR ? ENOTDIR :
			    search_access(current.p_inode,cred);
			if (error != 0)
				goto fail;
		}
	}
	if (trailing && current.p_inode->i_type != INODE_DIR) {
		error = ENOTDIR;
		goto fail;
	}
	if (trailing && (error = search_access(current.p_inode,cred)) != 0)
		goto fail;
	*result = current;
	path_release(&root);
	release_cred(owned_cred);
	return 0;
fail:
	path_release(&current);
	path_release(&root);
	release_cred(owned_cred);
	return error;
}

int
namei_path_at(struct cwdinfo *context, const char *path, struct path *result)
{
	return namei_path_flags_at(context, path, 0, result);
}

int
namei_at(struct cwdinfo *context, const char *path, struct inode **result)
{
	struct path found;
	int error;
	if (result == NULL)
		return EINVAL;
	error = namei_path_at(context, path, &found);
	if (error != 0)
		return error;
	*result = found.p_inode;
	found.p_inode = NULL;
	path_release(&found);
	return 0;
}

int
namei_parent_path_at(struct cwdinfo *context, const char *path,
		     struct path *parent, struct componentname *last,
		     char storage[NAME_MAX + 1U])
{
	char prefix[ZEDBSD_PATH_MAX];
	size_t length, start, parent_length;
	int error;

	if (context == NULL || parent == NULL || last == NULL || storage == NULL)
		return EINVAL;
	error = path_length(path, &length);
	if (error != 0)
		return error;
	while (length > 1 && path[length - 1U] == '/')
		length--;
	start = length;
	while (start > 0 && path[start - 1U] != '/')
		start--;
	if (length - start == 0 || length - start > NAME_MAX)
		return length - start > NAME_MAX ? ENAMETOOLONG : EINVAL;
	memcpy(storage, path + start, length - start);
	storage[length - start] = '\0';
	if (!strcmp(storage, ".") || !strcmp(storage, ".."))
		return EINVAL;
	last->cn_nameptr = storage;
	last->cn_namelen = length - start;
	last->cn_flags = COMPONENT_LAST;
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
	error = namei_path_at(context, prefix, parent);
	if (error == 0 && parent->p_inode->i_type != INODE_DIR) {
		path_release(parent);
		return ENOTDIR;
	}
	return error;
}

int
namei_parent_at(struct cwdinfo *context, const char *path,
		struct inode **parent, struct componentname *last,
		char storage[NAME_MAX + 1U])
{
	struct path found;
	int error;
	if (parent == NULL)
		return EINVAL;
	error = namei_parent_path_at(context, path, &found, last, storage);
	if (error != 0)
		return error;
	*parent = found.p_inode;
	found.p_inode = NULL;
	path_release(&found);
	return 0;
}

int
cwdinfo_init(struct cwdinfo *context, const struct path *root)
{
	if (context == NULL || root == NULL || root->p_mount == NULL ||
	    root->p_inode == NULL || root->p_inode->i_type != INODE_DIR)
		return EINVAL;
	memset(context, 0, sizeof(*context));
	refcount_init(&context->refs,1);
	spin_init(&context->lock,LOCK_RANK_PROCESS,"cwdinfo");
	path_set(&context->root, root->p_mount, root->p_inode);
	path_set(&context->cwd, root->p_mount, root->p_inode);
	return 0;
}

void
cwdinfo_destroy(struct cwdinfo *context)
{
	if (context == NULL)
		return;
	path_release(&context->root);
	path_release(&context->cwd);
	memset(context, 0, sizeof(*context));
}

int
fs_chdir_path(struct cwdinfo *context, const struct path *directory)
{
	struct ucred *cred=cred_current_ref!=NULL?cred_current_ref():NULL;
	const struct ucred *check=cred!=NULL?cred:
	    (cred_current!=NULL?cred_current():NULL);
	struct path replacement, old;
	unsigned long irq;
	int error;

	if (context == NULL || directory == NULL || directory->p_inode == NULL ||
	    directory->p_mount == NULL) {
		release_cred(cred);
		return EINVAL;
	}
	if (directory->p_inode->i_type != INODE_DIR) {
		release_cred(cred);
		return ENOTDIR;
	}
	error = search_access(directory->p_inode, check);
	if (error != 0) {
		release_cred(cred);
		return error;
	}
	path_init(&replacement);
	path_set(&replacement, directory->p_mount, directory->p_inode);
	irq = spin_lock_irqsave(&context->lock);
	old = context->cwd;
	context->cwd = replacement;
	spin_unlock_irqrestore(&context->lock, irq);
	path_release(&old);
	release_cred(cred);
	return 0;
}

int
fs_chdir(struct cwdinfo *context, const char *path)
{
	struct path directory;
	int error = namei_path_at(context, path, &directory);
	if (error != 0)
		return error;
	error = fs_chdir_path(context, &directory);
	path_release(&directory);
	return error;
}

static int
child_path(const struct path *parent, const char *name, struct path *result)
{
	struct componentname component;
	struct inode *inode;
	int error;

	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = COMPONENT_LAST;
	error = mount_lookup_child(parent, &component, result);
	if (error == 0)
		return 0;
	if (error != ENOENT)
		return error;
	error = inode_lookup(parent->p_inode, &component, &inode);
	if (error != 0)
		return error;
	path_set(result, parent->p_mount, inode);
	inode_release(inode);
	return 0;
}

static int
find_child_name(const struct path *parent, const struct path *child,
		char name[NAME_MAX + 1U])
{
	struct file *directory;
	struct dirent entry;
	uint64_t sequence;
	int eof = 0, error;

	sequence = parent->p_inode->i_dirseq;
	error = file_open_resolved(parent, O_RDONLY | O_DIRECTORY, &directory);
	if (error != 0)
		return error;
	while (!eof) {
		struct path candidate;

		error = file_readdir(directory, &entry, &eof);
		if (error != 0)
			break;
		if (eof)
			break;
		if (entry.d_name[0] == '\0' || !strcmp(entry.d_name, ".") ||
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
	if (parent->p_inode->i_dirseq != sequence)
		return EAGAIN;
	return error;
}

static int
getcwd_once(const struct path *root,const struct path *cwd,
    char *buffer, size_t capacity)
{
	struct componentname dotdot = { "..", 2, COMPONENT_DOTDOT };
	struct path current;
	char reverse[ZEDBSD_PATH_MAX];
	size_t position = sizeof(reverse) - 1U;
	unsigned depth = 0;
	int error = 0;

	reverse[position] = '\0';
	path_set(&current,cwd->p_mount,cwd->p_inode);
	while (!path_equal(&current,root)) {
		struct path parent;
		char name[NAME_MAX + 1U];
		size_t length;

		if (++depth > ZEDBSD_PATH_MAX / 2U) {
			error = ELOOP;
			break;
		}
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
			struct inode *parent_inode;

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
	if (position == sizeof(reverse) - 1U)
		reverse[--position] = '/';
	if (sizeof(reverse) - position > capacity)
		return ERANGE;
	memcpy(buffer, reverse + position, sizeof(reverse) - position);
	return 0;
}

int
fs_getcwd(const struct cwdinfo *context, char *buffer, size_t capacity)
{
	unsigned attempt;
	int error;
	struct path root,cwd;
	unsigned long irq;

	if (context == NULL || buffer == NULL || capacity == 0)
		return EINVAL;
	irq=spin_lock_irqsave((struct spinlock *)&context->lock);
	if(context->root.p_inode==NULL||context->cwd.p_inode==NULL){
		spin_unlock_irqrestore((struct spinlock *)&context->lock,irq);
		return EINVAL;
	}
	path_set(&root,context->root.p_mount,context->root.p_inode);
	path_set(&cwd,context->cwd.p_mount,context->cwd.p_inode);
	spin_unlock_irqrestore((struct spinlock *)&context->lock,irq);
	for (attempt = 0; attempt < 8U; attempt++) {
		error = getcwd_once(&root,&cwd,buffer,capacity);
		if (error != EAGAIN)break;
	}
	path_release(&cwd);path_release(&root);
	return error;
}
