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
	uintptr_t address,
	size_t size,
	uint32_t prot);

int
user_address_add(
	uintptr_t address,
	size_t delta,
	uintptr_t *result);

int
off_add_size(
	off_t offset,
	size_t delta,
	off_t *result);

int
size_add_checked(
	size_t left,
	size_t right,
	size_t *result);

int
uaccess_pin_vmspace(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t prot,
	struct uaccess_pin *pin);

int
uaccess_pin(
	uintptr_t address,
	size_t size,
	uint32_t prot,
	struct uaccess_pin *pin);

void
uaccess_unpin(
	struct uaccess_pin *pin);

int
copyin_pinned(
	const struct uaccess_pin *pin,
	size_t offset,
	void *destination,
	size_t size);

int
copyout_pinned(
	const struct uaccess_pin *pin,
	size_t offset,
	const void *source,
	size_t size);

int
copyin(
	uintptr_t source,
	void *destination,
	size_t size);

int
copyout(
	const void *source,
	uintptr_t destination,
	size_t size);

int
copyinstr(
	uintptr_t source,
	char *destination,
	size_t capacity,
	size_t *length);

#endif
