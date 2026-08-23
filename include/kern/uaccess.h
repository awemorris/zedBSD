/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * uaccess
 */

#ifndef ZEDBSD_KERN_UACCESS_H
#define ZEDBSD_KERN_UACCESS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct vmspace;
struct vmspace_pinned_page;

/*
 * A pin captures every backing-page identity in a user range and keeps its
 * storage resident while a syscall performs an operation which cannot safely
 * be rolled back.  The virtual mappings may subsequently disappear or be
 * replaced.  Pins are owned by the calling thread and must not survive a
 * syscall return.
 */
struct uaccess_pin {
	uintptr_t address;
	size_t size;
	uint32_t prot;
	size_t first_offset;
	size_t page_count;
	struct vmspace_pinned_page *pages;
	unsigned active;
};

int
user_range_check(
	uintptr_t,
	size_t,
	uint32_t);

int
user_address_add(
	uintptr_t,
	size_t,
	uintptr_t *);

int
off_add_size(
	off_t,
	size_t,
	off_t *);

int
size_add_checked(
	size_t,
	size_t,
	size_t *);

int
uaccess_pin_vmspace(
	struct vmspace *,
	uintptr_t,
	size_t,
	uint32_t,
	struct uaccess_pin *);

int
uaccess_pin(
	uintptr_t,
	size_t,
	uint32_t,
	struct uaccess_pin *);

void
uaccess_unpin(
	struct uaccess_pin *);

int
copyin_pinned(
	const struct uaccess_pin *,
	size_t,
	void *,
	size_t);

int
copyout_pinned(
	const struct uaccess_pin *,
	size_t,
	const void *,
	size_t);

int
copyin(
	uintptr_t,
	void *,
	size_t);

int
copyout(
	const void *,
	uintptr_t,
	size_t);

int
copyinstr(
	uintptr_t,
	char *,
	size_t,
	size_t *);

#endif
