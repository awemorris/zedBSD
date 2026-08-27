/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_SWAP_CONTROL_H
#define ZEDBSD_KERN_SWAP_CONTROL_H

#include <kern/swap-source.h>

struct disk;
struct path;

/*
 * VFS owns selector syntax and object lifetime.  Successful resolvers return
 * one ordinary retained object: resolve_path() returns a path released with
 * path_release(), and resolve_disk() returns a disk released with
 * disk_release().  The facade never keeps either reference after the call.
 */
struct kern_swap_control_resolver_ops {
	int (*resolve_path)(void *context, const char *selector,
	    struct path *result);
	int (*resolve_disk)(void *context, const char *selector,
	    struct disk **result);
	int (*validate_raw)(void *context, struct disk *disk);
};

struct kern_swap_control_registration {
	struct kern_swap_source_set *sources;
	const struct kern_swap_control_resolver_ops *resolver;
	void *resolver_context;
};

struct kern_swap_control_source_info {
	uint32_t source_id;
	uint32_t state;
	uint32_t header_version;
	uint32_t total_pages;
	uint32_t used_pages;
	uint8_t uuid[ZEDBSD_SWAP_V2_UUID_SIZE];
	char label[ZEDBSD_SWAP_V2_LABEL_SIZE];
	char source[KERN_SWAP_SOURCE_TEXT_MAX + 1U];
};

/* Registration is a one-time VFS handoff after the empty manager is active. */
int kern_swap_control_register(
	const struct kern_swap_control_registration *registration);

int kern_swap_control_add(const char *selector);
int kern_swap_control_remove(const char *selector);
int kern_swap_control_get(unsigned source_id,
	struct kern_swap_control_source_info *result);

#endif
