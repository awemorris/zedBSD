/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Swap: the control interface and the boot-time activation.
 *
 * The control interface adds, removes and describes sources at run
 * time; the boot path prepares the source set from the boot parameters.
 */

#include "kern/swap.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include <uapi/errno.h>
#include <hal/hal.h>
#include <stddef.h>
#include <kern/swap-boot.h>
#include <kern/block-identity.h>
#include <kern/boot.h>
#include <kern/disk.h>
#include <kern/mount.h>
#include <kern/swap-control.h>
#include <kern/inode.h>
#include <kern/signal.h>
#include <kern/swap-source.h>
#include <kern/backing-claim.h>
#include <kern/buf.h>
#include <kern/fat.h>
#include <kern/file.h>
#include <kern/file-backing.h>
#include <kern/klog.h>
#include <kern/vm-commit.h>
#include <kern/vm-reclaim.h>
#include <uapi/fcntl.h>
#include <limits.h>
#include <uapi/stat.h>
#include <kern/kcrt.h>

static struct kern_swap_control_registration control_registration;

static unsigned control_registered;

static unsigned control_busy;

static struct spinlock control_lock = {
	{ 0 }, LOCK_RANK_SWAP, "swap control", 0, 0
};

static int is_boot_reference(const char *value);
extern struct thread *thread_current(void);
static int selector_validate(const char *selector);
static int selector_is_disk(const char *selector);
static int control_enter(struct kern_swap_control_registration *registration);
static void control_leave(void);
static int control_snapshot_registration(struct kern_swap_control_registration *registration);
static int control_resolve_identity(const struct kern_swap_control_registration *control, const char *selector, struct path *path, struct disk **disk, struct inode **identity_inode);
static void control_release_identity(struct path *path, struct disk *disk, struct inode *identity_inode);
static int control_source_identity_matches(const struct kern_swap_source *source, struct disk *disk, struct inode *identity_inode);
static int control_revalidate_identity(const struct kern_swap_control_registration *control, const char *selector, const struct kern_swap_source *source);
static int control_cancelled(void *argument);

/*
 * Prepares the swap source set from the boot parameters.
 *
 * On failure the set is aborted and failed_parameter names the parameter
 * that could not be prepared.
 */
int
kern_swap_boot_prepare(
	const struct kern_boot_parameters *parameters,
	struct kern_boot_source_context *boot_sources,
	struct kern_swap_source_set *swap_sources,
	unsigned *failed_parameter)
{
	struct kern_swap_source source;
	struct path path;
	struct disk *disk;
	const char *value;
	unsigned parameter;
	unsigned boot_slot;
	int error;

	error = 0;

	/* Rejects a missing operand. */
	if (parameters == NULL || boot_sources == NULL || swap_sources == NULL)
		return EINVAL;

	/* Starts with an empty set and no failed parameter. */
	kern_swap_source_set_init(swap_sources);
	if (failed_parameter != NULL)
		*failed_parameter = KERN_SWAP_SOURCE_COUNT;

	/* Prepares each named source in parameter order. */
	for (parameter = 0; parameter < KERN_SWAP_SOURCE_COUNT; parameter++) {
		value = kern_boot_parameters_swap(parameters, parameter);
		if (value == NULL)
			continue;
		kern_swap_source_init(&source);

		/* A boot reference is a swap file on a boot source. */
		if (is_boot_reference(value)) {
			path_init(&path);
			error = kern_boot_source_lookup(boot_sources, value,
			    &boot_slot, &path);
			if (error == 0)
				error = kern_swap_source_prepare_file(&path,
				    parameter, &source);
			path_release(&path);
			if (error == 0)
				error = kern_swap_source_set_diagnostic(&source, value);
			if (error == 0)
				error = kern_swap_source_set_add(swap_sources,
				    &source);
			if (error == 0)
				error = kern_boot_source_retain_slot(boot_sources,
				    boot_slot);
		} else {
			/* Anything else selects a disk used as raw swap. */
			disk = NULL;
			error = kern_boot_source_selector_validate(value);
			if (error == 0)
				error = block_identity_resolve(value, &disk);
			if (error == 0)
				error = kern_swap_source_prepare_raw(disk, parameter,
				    &source);
			if (disk != NULL)
				disk_release(disk);
			if (error == 0)
				error = kern_swap_source_set_diagnostic(&source, value);
			if (error == 0)
				error = kern_swap_source_set_add(swap_sources,
				    &source);
		}

		/* The first failure abandons the whole set. */
		if (error != 0) {
			kern_swap_source_destroy(&source);
			if (failed_parameter != NULL)
				*failed_parameter = parameter;
			(void)kern_swap_source_set_abort(swap_sources);
			return error;
		}
	}

	/* Reports the prepared set. */
	return 0;
}

/*
 * Registers the swap source set and selector resolver.
 *
 * Only one registration is accepted, and none while a control operation
 * is in progress.
 */
int
kern_swap_control_register(
	const struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Rejects an incomplete registration or an inactive source set. */
	if (registration == NULL ||
	    registration->sources == NULL ||
	    registration->resolver == NULL ||
	    registration->resolver->resolve_path == NULL ||
	    registration->resolver->resolve_disk == NULL ||
	    registration->resolver->validate_raw == NULL ||
	    !registration->sources->active)
		return EINVAL;

	/* Installs the registration unless one exists or is in use. */
	irq = spin_lock_irqsave(&control_lock);

	if (control_registered || control_busy) {
		error = EBUSY;
	} else {
		control_registration = *registration;
		control_registered = 1;
	}

	spin_unlock_irqrestore(&control_lock, irq);

	/* Reports why the registration failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Adds the swap source a selector names.
 *
 * The selector is resolved, the source is prepared as a file or a raw
 * disk, its identity is checked against the lookup and against a second
 * resolution of the selector, and it is published to the source set.
 */
int
kern_swap_control_add(
	const char *selector)
{
	struct kern_swap_control_registration control;
	struct kern_swap_source source;
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	unsigned source_id;
	int error;

	/* Validates the selector and takes the control operation. */
	error = selector_validate(selector);
	if (error != 0)
		return error;
	error = control_enter(&control);
	if (error != 0)
		return error;

	/* Resolves the selector to a disk and possibly a file. */
	kern_swap_source_init(&source);
	error = control_resolve_identity(&control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0)
		goto out;

	/* Refuses a source that is already published. */
	if (disk != NULL)
		error = kern_swap_source_set_find_identity(control.sources, disk,
		    identity_inode, &source_id);
	else
		error = ENOENT;
	if (error == 0) {
		error = EEXIST;
		goto out_release;
	}

	if (error != ENOENT)
		goto out_release;

	/* Prepares the source as a file or as a validated raw disk. */
	if (identity_inode != NULL) {
		error = kern_swap_source_prepare_file(&path, 0, &source);
	} else {
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

	/* The selector must still name the source after the backing claim. */
	error = control_revalidate_identity(&control, selector, &source);
	if (error != 0)
		goto out_destroy;

	/* Publishes the source under its selector. */
	error = kern_swap_source_set_diagnostic(&source, selector);
	if (error == 0)
		error = kern_swap_source_set_runtime_add(control.sources, &source,
		    NULL);

	/* Tells the resolver that the selector now names an active source. */
	if (error == 0) {
		if (control.resolver->source_added != NULL) {
			control.resolver->source_added(control.resolver_context,
			    selector);
		}
	}
out_destroy:
	kern_swap_source_destroy(&source);
out_release:
	control_release_identity(&path, disk, identity_inode);
out:
	control_leave();

	/* Reports why the add failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Removes the swap source a selector names.
 *
 * The removal migrates the source's pages elsewhere and can be cancelled
 * by a signal to the calling thread.
 */
int
kern_swap_control_remove(
	const char *selector)
{
	struct kern_swap_control_registration control;
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	unsigned source_id;
	int error;

	/* Validates the selector and takes the control operation. */
	error = selector_validate(selector);
	if (error != 0)
		return error;
	error = control_enter(&control);
	if (error != 0)
		return error;

	/* Resolves the selector to the published source. */
	error = control_resolve_identity(&control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0)
		goto out;
	if (disk != NULL)
		error = kern_swap_source_set_find_identity(control.sources, disk,
		    identity_inode, &source_id);
	else
		error = ENOENT;

	/* Removes it, letting a signal cancel the migration. */
	if (error == 0)
		error = kern_swap_source_set_runtime_remove_cancelable(
		    control.sources, source_id, control_cancelled,
		    thread_current());
	control_release_identity(&path, disk, identity_inode);
out:
	control_leave();

	/* Reports why the removal failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Describes one swap source by identifier.
 */
int
kern_swap_control_get(
	unsigned source_id,
	struct kern_swap_control_source_info *result)
{
	struct kern_swap_control_registration control;
	struct kern_swap_source_snapshot snapshot;
	int error;

	/* Rejects a missing result or an impossible identifier. */
	if (result == NULL || source_id >= KERN_SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Snapshots the source through the registered set. */
	error = control_snapshot_registration(&control);
	if (error != 0)
		return error;
	error = kern_swap_source_set_snapshot(control.sources, source_id,
	    &snapshot);
	if (error != 0)
		return error;

	/* Copies the snapshot into the caller's record. */
	kern_memset(result, 0, sizeof(*result));
	result->source_id = snapshot.source_id;
	result->state = snapshot.state;
	result->header_version = snapshot.header_version;
	result->total_pages = snapshot.total_pages;
	result->used_pages = snapshot.used_pages;
	kern_memcpy(result->uuid, snapshot.uuid, sizeof(result->uuid));
	kern_memcpy(result->label, snapshot.label, sizeof(result->label));
	kern_memcpy(result->source, snapshot.diagnostic, sizeof(result->source));

	/* Reports the described source. */
	return 0;
}

/* Tests whether a swap parameter is a boot<slot>:<path> reference. */
static int
is_boot_reference(
	const char *value)
{
	/* A reference starts with "boot" and carries a colon. */
	if (value == NULL)
		return 0;
	if (kern_strncmp(value, "boot", 4U) != 0)
		return 0;
	if (kern_strchr(value, ':') == NULL)
		return 0;

	/* Reports a boot reference. */
	return 1;
}

/* Checks that a selector is present, non-empty, and within the text limit. */
static int
selector_validate(
	const char *selector)
{
	size_t length;

	/* Rejects a missing selector. */
	if (selector == NULL)
		return EINVAL;

	/* Measures the selector, stopping just past the limit. */
	for (length = 0; length <= KERN_SWAP_SOURCE_TEXT_MAX; length++) {
		if (selector[length] == '\0')
			break;
	}

	/* Rejects an empty or overlong selector. */
	if (length == 0 || length > KERN_SWAP_SOURCE_TEXT_MAX)
		return EINVAL;

	/* Reports a usable selector. */
	return 0;
}

/* Tests whether a selector names a disk rather than a file path. */
static int
selector_is_disk(
	const char *selector)
{
	/* Device paths and identity selectors name disks. */
	if (kern_strncmp(selector, "/dev/", 5U) == 0)
		return 1;
	if (kern_strncmp(selector, "UUID=", 5U) == 0)
		return 1;
	if (kern_strncmp(selector, "PARTUUID=", 9U) == 0)
		return 1;

	/* Anything else is a file path. */
	return 0;
}

/* Takes the single control operation and copies the registration. */
static int
control_enter(
	struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Refuses without a registration or while another operation runs. */
	irq = spin_lock_irqsave(&control_lock);

	if (!control_registered) {
		error = ENXIO;
	} else if (control_busy) {
		error = EBUSY;
	} else {
		control_busy = 1;
		*registration = control_registration;
	}

	spin_unlock_irqrestore(&control_lock, irq);

	/* Reports whether the operation was taken. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Releases the control operation. */
static void
control_leave(
	void)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&control_lock);

	control_busy = 0;

	spin_unlock_irqrestore(&control_lock, irq);
}

/* Copies the registration without taking the control operation. */
static int
control_snapshot_registration(
	struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Copies the registration when there is one. */
	irq = spin_lock_irqsave(&control_lock);

	if (!control_registered)
		error = ENXIO;
	else
		*registration = control_registration;

	spin_unlock_irqrestore(&control_lock, irq);

	/* Reports whether a registration was copied. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Resolves a selector to a disk, and for a file path also to its inode. */
static int
control_resolve_identity(
	const struct kern_swap_control_registration *control,
	const char *selector,
	struct path *path,
	struct disk **disk,
	struct inode **identity_inode)
{
	int error;

	path_init(path);
	*disk = NULL;
	*identity_inode = NULL;

	/* A disk selector resolves directly to a disk. */
	if (selector_is_disk(selector)) {
		error = control->resolver->resolve_disk(control->resolver_context,
		    selector, disk);
		return error;
	}

	/* A file path resolves to a mounted inode. */
	error = control->resolver->resolve_path(control->resolver_context,
	    selector, path);
	if (error != 0)
		return error;
	if (path->p_inode == NULL || path->p_mount == NULL) {
		path_release(path);
		path_init(path);
		return EINVAL;
	}

	/*
	 * Canonical file identity follows the inode's owning filesystem, not
	 * the namespace mount which may be a diskless bind wrapper.  A regular
	 * file whose owning filesystem has no disk still has a valid pathname
	 * lifetime; keep that path so prepare_file() can return
	 * backend-specific EOPNOTSUPP.  Successful preparations necessarily
	 * supply a disk-backed identity before publication.
	 */
	if (path->p_inode->i_mount != NULL)
		*disk = path->p_inode->i_mount->m_disk;
	else
		*disk = NULL;
	*identity_inode = path->p_inode;

	/* Reports the resolved file. */
	return 0;
}

/* Releases whatever control_resolve_identity() retained. */
static void
control_release_identity(
	struct path *path,
	struct disk *disk,
	struct inode *identity_inode)
{
	/* A file holds its path; a disk selector holds only the disk. */
	if (identity_inode != NULL)
		path_release(path);
	else if (disk != NULL)
		disk_release(disk);
}

/* Tests whether a prepared source is backed by the looked-up disk and inode. */
static int
control_source_identity_matches(
	const struct kern_swap_source *source,
	struct disk *disk,
	struct inode *identity_inode)
{
	/* Both sides must have a disk, and agree on whether there is a file. */
	if (source == NULL ||
	    disk == NULL ||
	    source->identity_disk == NULL ||
	    (source->identity_inode == NULL) != (identity_inode == NULL))
		return 0;

	/* The disks must be the same object or the same device. */
	if (source->identity_disk != disk &&
	    source->identity_disk->d_dev != disk->d_dev)
		return 0;

	/* A raw source matches; a file source must be the same inode. */
	if (identity_inode == NULL)
		return 1;
	if (source->identity_inode == identity_inode)
		return 1;
	if (source->identity_inode->i_ino == identity_inode->i_ino)
		return 1;

	/* Reports a different file. */
	return 0;
}

/* Resolves a selector again after the backing claim and checks the match. */
static int
control_revalidate_identity(
	const struct kern_swap_control_registration *control,
	const char *selector,
	const struct kern_swap_source *source)
{
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	int error;

	/*
	 * Preparing a source establishes the backing claim which excludes
	 * subsequent unlink, rename, rebind, and disk teardown.  Resolve the
	 * spelling once more after that exclusion point: otherwise a mutation
	 * which completed between the first lookup and claim acquisition could
	 * publish an active source whose diagnostic selector no longer
	 * identifies it.
	 */
	error = control_resolve_identity(control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0) {
		if (error == ENOMEM)
			return ENOMEM;
		return EAGAIN;
	}

	/* A selector that now names something else is a lost race. */
	if (control_source_identity_matches(source, disk, identity_inode))
		error = 0;
	else
		error = EAGAIN;
	control_release_identity(&path, disk, identity_inode);

	/* Reports whether the selector still names the source. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports EINTR to a cancelable removal when its thread has a signal. */
static int
control_cancelled(
	void *argument)
{
	struct thread *thread;

	/* Only a thread with a pending unblocked signal cancels. */
	thread = argument;
	if (thread == NULL)
		return 0;
	if (!signal_pending_unblocked(thread))
		return 0;

	/* Reports the cancellation. */
	return EINTR;
}
