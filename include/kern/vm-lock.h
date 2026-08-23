/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Cross-vmspace metadata serialization.
 */

#ifndef ZEDBSD_KERN_VM_LOCK_H
#define ZEDBSD_KERN_VM_LOCK_H

/*
 * Reverse mappings connect vmspace, private-page and VM-object ownership
 * domains.  Code traversing more than one domain enters this outer lock
 * before taking a vmspace, reclaim or object lock.  The lock is recursive
 * for calls which cross an internal public API boundary on the same thread.
 */
void vm_metadata_init(void);
void vm_metadata_enter(void);
void vm_metadata_leave(void);
int vm_metadata_owned(void);

#endif
