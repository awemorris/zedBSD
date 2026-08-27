/* Kernel boot-filesystem sources and root-selection helpers.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_BOOT_SOURCE_H
#define ZEDBSD_KERN_BOOT_SOURCE_H

#include <kern/boot-parameters.h>
#include <kern/fat.h>
#include <kern/mount.h>

#define KERN_BOOT_SOURCE_SLOT_COUNT 4U

struct kern_boot_source_reference {
	unsigned slot;
	char relative[ZEDBSD_PATH_MAX];
};

enum kern_boot_root_mode {
	KERN_BOOT_ROOT_INVALID = 0,
	KERN_BOOT_ROOT_NATIVE,
	KERN_BOOT_ROOT_OVERLAY,
};

enum kern_boot_source_failure_stage {
	KERN_BOOT_SOURCE_FAILURE_NONE = 0,
	KERN_BOOT_SOURCE_FAILURE_SELECTOR,
	KERN_BOOT_SOURCE_FAILURE_RESOLVE,
	KERN_BOOT_SOURCE_FAILURE_PARTITION,
	KERN_BOOT_SOURCE_FAILURE_DUPLICATE,
	KERN_BOOT_SOURCE_FAILURE_FILESYSTEM,
	KERN_BOOT_SOURCE_FAILURE_MOUNT,
};

struct kern_boot_source_slot {
	/* disk is borrowed from runtime_mount while runtime_mount is non-NULL. */
	struct disk *disk;
	/* Owned private mount until release or promotion. */
	struct mount *mount;
	/*
	 * System-lifetime lookup anchor.  Normally identical to mount; after a
	 * boot filesystem is promoted to the namespace root it remains a borrowed
	 * pointer to that root mount while mount becomes NULL.
	 */
	struct mount *runtime_mount;
	unsigned configured;
	unsigned retained;
	unsigned promoted;
};

struct kern_boot_source_context {
	struct kern_boot_source_slot slot[KERN_BOOT_SOURCE_SLOT_COUNT];
	unsigned failure_slot;
	enum kern_boot_source_failure_stage failure_stage;
	int cleanup_error;
	/* Immutable once set.  Published contexts have system lifetime. */
	unsigned runtime_published;
};

int
kern_boot_source_selector_validate(
	const char *selector);

int
kern_boot_source_reference_parse(
	const char *text,
	struct kern_boot_source_reference *reference);

int
kern_boot_source_root_mode(
	const char *rootpart,
	const char *overlay_root,
	const char *overlay_data,
	enum kern_boot_root_mode *mode);

int
kern_boot_source_fat_type_supported(
	enum bootfat_type type);

const char *
kern_boot_source_failure_stage_name(
	enum kern_boot_source_failure_stage stage);

void
kern_boot_source_context_init(
	struct kern_boot_source_context *context);

int
kern_boot_source_context_mount(
	struct kern_boot_source_context *context,
	const struct kern_boot_parameters *parameters,
	struct disk *loader_origin,
	const char *loader_origin_selector);

int
kern_boot_source_lookup(
	struct kern_boot_source_context *context,
	const char *text,
	unsigned *slot_out,
	struct path *path_out);

int
kern_boot_source_retain_slot(
	struct kern_boot_source_context *context,
	unsigned slot);

/*
 * Runtime bootN selectors require every configured private boot mount to
 * survive root selection.  Retain is performed before root selection can
 * release unused mounts; publication happens only after the root namespace
 * and swap-control facade are ready.
 */
int
kern_boot_source_retain_configured(
	struct kern_boot_source_context *context);

int
kern_boot_source_publish_runtime(
	struct kern_boot_source_context *context);

int
kern_boot_source_runtime_lookup(
	struct kern_boot_source_context *context,
	const char *text,
	struct path *path_out);

int
kern_boot_source_find_disk(
	const struct kern_boot_source_context *context,
	const struct disk *disk,
	unsigned *slot_out);

int
kern_boot_source_promote_root(
	struct kern_boot_source_context *context,
	unsigned slot,
	struct mount **root_out);

int
kern_boot_source_release_unused(
	struct kern_boot_source_context *context);

int
kern_boot_source_context_destroy(
	struct kern_boot_source_context *context);

#endif
