/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/swap-control.h>

#include <kern/disk.h>
#include <kern/inode.h>
#include <kern/lock.h>
#include <kern/mount.h>
#include <kern/signal.h>

#include <errno.h>
#include <string.h>

extern struct thread *thread_current(void);

static struct kern_swap_control_registration control_registration;
static unsigned control_registered;
static unsigned control_busy;
static struct spinlock control_lock = {
	{ 0 }, LOCK_RANK_SWAP, "swap control", 0, 0
};

static int
selector_validate(const char *selector)
{
	size_t length;

	if (selector == NULL)
		return EINVAL;
	for (length = 0; length <= KERN_SWAP_SOURCE_TEXT_MAX; length++)
		if (selector[length] == '\0')
			break;
	return length == 0 || length > KERN_SWAP_SOURCE_TEXT_MAX ? EINVAL : 0;
}

static int
selector_is_disk(const char *selector)
{
	return strncmp(selector, "/dev/", 5U) == 0 ||
	    strncmp(selector, "UUID=", 5U) == 0 ||
	    strncmp(selector, "PARTUUID=", 9U) == 0;
}

static int
control_enter(struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error = 0;

	irq = spin_lock_irqsave(&control_lock);
	if (!control_registered)
		error = ENXIO;
	else if (control_busy)
		error = EBUSY;
	else {
		control_busy = 1;
		*registration = control_registration;
	}
	spin_unlock_irqrestore(&control_lock, irq);
	return error;
}

static void
control_leave(void)
{
	unsigned long irq = spin_lock_irqsave(&control_lock);

	control_busy = 0;
	spin_unlock_irqrestore(&control_lock, irq);
}

static int
control_snapshot_registration(
	struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error = 0;

	irq = spin_lock_irqsave(&control_lock);
	if (!control_registered)
		error = ENXIO;
	else
		*registration = control_registration;
	spin_unlock_irqrestore(&control_lock, irq);
	return error;
}

int
kern_swap_control_register(
	const struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error = 0;

	if (registration == NULL || registration->sources == NULL ||
	    registration->resolver == NULL ||
	    registration->resolver->resolve_path == NULL ||
	    registration->resolver->resolve_disk == NULL ||
	    registration->resolver->validate_raw == NULL ||
	    !registration->sources->active)
		return EINVAL;
	irq = spin_lock_irqsave(&control_lock);
	if (control_registered || control_busy)
		error = EBUSY;
	else {
		control_registration = *registration;
		control_registered = 1;
	}
	spin_unlock_irqrestore(&control_lock, irq);
	return error;
}

static int
control_resolve_identity(const struct kern_swap_control_registration *control,
	const char *selector, struct path *path, struct disk **disk,
	struct inode **identity_inode)
{
	int error;

	path_init(path);
	*disk = NULL;
	*identity_inode = NULL;
	if (selector_is_disk(selector))
		return control->resolver->resolve_disk(control->resolver_context,
		    selector, disk);
	error = control->resolver->resolve_path(control->resolver_context,
	    selector, path);
	if (error != 0)
		return error;
	if (path->p_inode == NULL || path->p_mount == NULL ||
	    path->p_mount->m_disk == NULL) {
		path_release(path);
		path_init(path);
		return EINVAL;
	}
	*disk = path->p_mount->m_disk;
	*identity_inode = path->p_inode;
	return 0;
}

static void
control_release_identity(struct path *path, struct disk *disk,
	struct inode *identity_inode)
{
	if (identity_inode != NULL)
		path_release(path);
	else if (disk != NULL)
		disk_release(disk);
}

static int
control_source_identity_matches(const struct kern_swap_source *source,
	struct disk *disk, struct inode *identity_inode)
{
	if (source == NULL || disk == NULL || source->identity_disk == NULL ||
	    (source->identity_inode == NULL) != (identity_inode == NULL))
		return 0;
	if (source->identity_disk != disk &&
	    source->identity_disk->d_dev != disk->d_dev)
		return 0;
	return identity_inode == NULL || source->identity_inode == identity_inode ||
	    source->identity_inode->i_ino == identity_inode->i_ino;
}

/*
 * Preparing a source establishes the backing claim which excludes subsequent
 * unlink, rename, rebind, and disk teardown.  Resolve the spelling once more
 * after that exclusion point: otherwise a mutation which completed between
 * the first lookup and claim acquisition could publish an active source whose
 * diagnostic selector no longer identifies it.
 */
static int
control_revalidate_identity(
	const struct kern_swap_control_registration *control,
	const char *selector, const struct kern_swap_source *source)
{
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	int error;

	error = control_resolve_identity(control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0)
		return error == ENOMEM ? ENOMEM : EAGAIN;
	error = control_source_identity_matches(source, disk, identity_inode) ? 0 :
	    EAGAIN;
	control_release_identity(&path, disk, identity_inode);
	return error;
}

int
kern_swap_control_add(const char *selector)
{
	struct kern_swap_control_registration control;
	struct kern_swap_source source;
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	unsigned source_id;
	int error;

	error = selector_validate(selector);
	if (error != 0)
		return error;
	error = control_enter(&control);
	if (error != 0)
		return error;
	kern_swap_source_init(&source);
	error = control_resolve_identity(&control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0)
		goto out;
	error = kern_swap_source_set_find_identity(control.sources, disk,
	    identity_inode, &source_id);
	if (error == 0) {
		error = EEXIST;
		goto out_release;
	}
	if (error != ENOENT)
		goto out_release;
	if (identity_inode != NULL)
		error = kern_swap_source_prepare_file(&path, 0, &source);
	else {
		error = control.resolver->validate_raw(control.resolver_context,
		    disk);
		if (error == 0)
			error = kern_swap_source_prepare_raw(disk, 0, &source);
	}
	if (error != 0)
		goto out_release;
	/* The prepared source must first match the retained lookup object. */
	if (!control_source_identity_matches(&source, disk, identity_inode)) {
		error = EAGAIN;
		goto out_destroy;
	}
	error = control_revalidate_identity(&control, selector, &source);
	if (error != 0)
		goto out_destroy;
	error = kern_swap_source_set_diagnostic(&source, selector);
	if (error == 0)
		error = kern_swap_source_set_runtime_add(control.sources, &source,
		    NULL);
out_destroy:
	kern_swap_source_destroy(&source);
out_release:
	control_release_identity(&path, disk, identity_inode);
out:
	control_leave();
	return error;
}

static int
control_cancelled(void *argument)
{
	struct thread *thread = argument;

	return thread != NULL && signal_pending_unblocked(thread) ? EINTR : 0;
}

int
kern_swap_control_remove(const char *selector)
{
	struct kern_swap_control_registration control;
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	unsigned source_id;
	int error;

	error = selector_validate(selector);
	if (error != 0)
		return error;
	error = control_enter(&control);
	if (error != 0)
		return error;
	error = control_resolve_identity(&control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0)
		goto out;
	error = kern_swap_source_set_find_identity(control.sources, disk,
	    identity_inode, &source_id);
	if (error == 0)
		error = kern_swap_source_set_runtime_remove_cancelable(
		    control.sources, source_id, control_cancelled,
		    thread_current());
	control_release_identity(&path, disk, identity_inode);
out:
	control_leave();
	return error;
}

int
kern_swap_control_get(unsigned source_id,
	struct kern_swap_control_source_info *result)
{
	struct kern_swap_control_registration control;
	struct kern_swap_source_snapshot snapshot;
	int error;

	if (result == NULL || source_id >= KERN_SWAP_SOURCE_COUNT)
		return EINVAL;
	error = control_snapshot_registration(&control);
	if (error != 0)
		return error;
	error = kern_swap_source_set_snapshot(control.sources, source_id,
	    &snapshot);
	if (error != 0)
		return error;
	memset(result, 0, sizeof(*result));
	result->source_id = snapshot.source_id;
	result->state = snapshot.state;
	result->header_version = snapshot.header_version;
	result->total_pages = snapshot.total_pages;
	result->used_pages = snapshot.used_pages;
	memcpy(result->uuid, snapshot.uuid, sizeof(result->uuid));
	memcpy(result->label, snapshot.label, sizeof(result->label));
	memcpy(result->source, snapshot.diagnostic, sizeof(result->source));
	return 0;
}
