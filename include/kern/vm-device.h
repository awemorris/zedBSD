/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device-owned memory shared by VM regions and transient user-access pins.
 */

#ifndef KERN_VM_DEVICE_H
#define KERN_VM_DEVICE_H

#include <kern/atomic.h>
#include <stddef.h>
#include <stdint.h>

#define VM_DEVICE_MMIO 1U

struct file;

/*
 * Immutable storage retained by the device until the final release callback.
 * A mapping's lifetime includes every forked/split region and in-flight pin.
 * The file keeps its open device session alive until that callback completes.
 */
struct vm_device_mapping {
	refcount_t references;
	struct file *file;
	uint64_t physical;
	void *address;
	size_t bytes;
	uint32_t attributes;
	uint32_t max_prot;
	void (*release)(void *);
	void *owner;
};

/*
 * The caller owns the storage until create succeeds; failure consumes nothing.
 * Success retains file and transfers one eventual release callback to the VM.
 */
int vm_device_create(struct file *file, uint64_t physical, void *address, size_t bytes, uint32_t attributes, uint32_t max_prot, void (*release)(void *), void *owner, struct vm_device_mapping **result);
void vm_device_ref(struct vm_device_mapping *mapping);
void vm_device_put(struct vm_device_mapping *mapping);
uint32_t vm_device_page_attributes(const struct vm_device_mapping *mapping);
int vm_device_read(struct vm_device_mapping *mapping, size_t offset, void *destination, size_t bytes);
int vm_device_write(struct vm_device_mapping *mapping, size_t offset, const void *source, size_t bytes);

#endif
